/* BAT177NW0: CPU1 guest-video ownership with zero Screen/CPU0 admission calls. */
#include "common.h"
#include "tab5_video_cpu1.h"

static tab5_cpu1_video_publish_stats_t s_pub_stats;
/* R57E54: downstream Screen pressure must never make CPU0 mutate the guest
 * dirty byte concurrently.  CPU0 sets this atomic bitmap; CPU1 consumes it at
 * its frame boundary and owns the actual TextDirtyLine write. */
static uint32_t s_cpu0_retry_bits[(TAB5_VIDEO_FLOW_FB_LINES + 31u) / 32u];
static uint32_t s_cpu0_retry_pending;

int tab5_cpu1_video_publish_commit(tab5_cpu1_video_line_t *line,
                                   const uint16_t *private_result_line)
{
    if (!line || !line->active || !private_result_line ||
        line->y >= TAB5_VIDEO_FLOW_FB_LINES || !line->width) {
        if (line && line->active)
            tab5_video_flow_cpu1_abort(line->y);
        if (line) line->active = 0u;
        return 0;
    }

    /* Guest correctness ends here. Host readiness is not consulted. */
    TextDirtyLine[line->y] = 0u;
    ++s_pub_stats.exact_renders;
    ++s_pub_stats.no_wait_commits;
    tab5_video_flow_cpu1_commit(line->y, line->width, line->render_seq,
                                line->video_epoch, line->visual_seq);
    line->active = 0u;
    return 1;
}

void tab5_cpu1_video_offload_submitted(tab5_cpu1_video_line_t *line)
{
    if (!line || !line->active || line->y >= TAB5_VIDEO_FLOW_FB_LINES)
        return;
    /* Immutable inputs are already captured by the CPU0 final-stage job.
     * CPU1 owns no completion wait. The CPU0 result publishes its own seq. */
    TextDirtyLine[line->y] = 0u;
    tab5_video_flow_cpu1_abort(line->y);
    ++s_pub_stats.accelerated_renders;
    ++s_pub_stats.no_wait_offloads;
    line->active = 0u;
}

void tab5_cpu1_video_offload_dropped(tab5_cpu1_video_line_t *line)
{
    if (!line || !line->active || line->y >= TAB5_VIDEO_FLOW_FB_LINES)
        return;
    TextDirtyLine[line->y] = 1u;
    tab5_video_flow_cpu1_abort(line->y);
    ++s_pub_stats.visual_drops;
    line->active = 0u;
}


void tab5_cpu1_video_retry_from_cpu0(uint32_t y)
{
    if (y >= TAB5_VIDEO_FLOW_FB_LINES)
        return;
    __atomic_fetch_or(&s_cpu0_retry_bits[y >> 5], 1u << (y & 31u), __ATOMIC_RELEASE);
    __atomic_store_n(&s_cpu0_retry_pending, 1u, __ATOMIC_RELEASE);
}

void WinX68k_MarkVideoLineDirty(uint32_t y)
{
    if (y < 1024u)
        TextDirtyLine[y] = 1u;
}

extern void WinX68k_MarkAllVideoDirty(void);

void tab5_cpu1_video_frame_boundary_sync(void)
{
    static uint32_t renderer_epoch;
    const tab5_guest_video_stamp_t stamp = tab5_guest_video_state_stamp();

    /* R57E54: fold CPU0 Screen-drop retries into the guest-owned dirty bytes.
     * Scan only when CPU0 actually posted a retry.  exchange() also safely
     * carries a retry that races this boundary into the next guest frame. */
    if (__atomic_exchange_n(&s_cpu0_retry_pending, 0u, __ATOMIC_ACQ_REL)) {
        for (uint32_t w = 0; w < (TAB5_VIDEO_FLOW_FB_LINES + 31u) / 32u; ++w) {
            uint32_t bits = __atomic_exchange_n(&s_cpu0_retry_bits[w], 0u, __ATOMIC_ACQ_REL);
            while (bits) {
                const uint32_t b = (uint32_t)__builtin_ctz(bits);
                const uint32_t y = (w << 5) + b;
                if (y < TAB5_VIDEO_FLOW_FB_LINES)
                    TextDirtyLine[y] = 1u;
                bits &= bits - 1u;
            }
        }
    }

    if (!renderer_epoch) {
        renderer_epoch = stamp.video_epoch;
        return;
    }
    if (stamp.video_epoch != renderer_epoch) {
        renderer_epoch = stamp.video_epoch;
        WinX68k_MarkAllVideoDirty();
    }
}

void tab5_cpu1_video_get_publish_stats(tab5_cpu1_video_publish_stats_t *out)
{
    if (!out) return;
    *out = s_pub_stats;
}
