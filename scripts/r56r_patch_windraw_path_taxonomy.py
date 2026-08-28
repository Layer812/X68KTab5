#!/usr/bin/env python3
from pathlib import Path
MAIN=Path('src/main.c')
WD=Path('components/px68k/libretro/windraw.c')
MARK='PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only'

def die(msg,rc=2):
    print('R56r path taxonomy patch ERROR:', msg)
    raise SystemExit(rc)

def one(s, old, new, label):
    n=s.count(old)
    if n!=1: die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old,new,1)

m=MAIN.read_text(encoding='utf-8')
w=WD.read_text(encoding='utf-8')
if MARK in m and 'WinDraw_R56RPathTake' in w:
    if m.count('R56R_PATH f=')!=1: die('already-applied main line count')
    if w.count('PX68K_R56R_COUNTERS')!=1: die('already-applied windraw counter marker')
    print('R56r already applied and verified')
    raise SystemExit(0)

for need in ['PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution', 'R56Q1_SAFE f=', 'PX68K_R56P: QUIET exact-executor counters visible; fact-only']:
    if need not in m: die(f'main predecessor missing: {need}')
if 'PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600' in m:
    die('unsafe R56q residue present')
for need in ['void WinDraw_DrawLine(void)', 'tab5_screen_video_line_latch', 'tab5_compose_submit_gbt65k_line', 'WinDraw_QueueHostCommonTwoLayer']:
    if need not in w: die(f'windraw lineage missing: {need}')

anchor='static uint16_t *RenderBuf = 0;\n'
block='''static uint16_t *RenderBuf = 0;

/* PX68K_R56R_COUNTERS
 * Counters-only taxonomy of the already-existing WinDraw return paths.
 * CPU1 is the sole writer/taker. No timers, queue changes, allocations, or
 * rendering decisions are introduced. 16 uint32_t = 64 bytes total. */
enum {
    R56R_LAT=0, R56R_LATCH_REJ, R56R_MODE16, R56R_MODE256, R56R_MODE65,
    R56R_CPU0_65, R56R_CPU0_GBT, R56R_CPU0_GRP8, R56R_CPU0_2L,
    R56R_CPU1_2L, R56R_CPU1_LEGACY, R56R_LEG16, R56R_LEG256, R56R_LEG65,
    R56R_65_QFULL, R56R_65_REJECT, R56R_N
};
static uint32_t s_r56r_path[R56R_N];

void WinDraw_R56RPathTake(uint32_t *out, uint32_t count)
{
    if (!out || count < R56R_N) return;
    for (uint32_t i = 0; i < R56R_N; ++i) {
        out[i] = s_r56r_path[i];
        s_r56r_path[i] = 0u;
    }
}
'''
w=one(w,anchor,block,'counter block')

old='''    if (!tab5_screen_video_line_latch(VLINE, (uint32_t)TextDotX,
                                        &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                                        &render_ticket)) {
        return;
    }

\tTextDirtyLine[VLINE] = 0;'''
new='''    if (!tab5_screen_video_line_latch(VLINE, (uint32_t)TextDotX,
                                        &RenderBuf[VLINE * FULLSCREEN_WIDTH],
                                        &render_ticket)) {
        ++s_r56r_path[R56R_LATCH_REJ];
        return;
    }
    ++s_r56r_path[R56R_LAT];
    switch (VCReg0[1] & 3u) {
        case 0u: ++s_r56r_path[R56R_MODE16]; break;
        case 1u:
        case 2u: ++s_r56r_path[R56R_MODE256]; break;
        default: ++s_r56r_path[R56R_MODE65]; break;
    }

\tTextDirtyLine[VLINE] = 0;'''
w=one(w,old,new,'latch entry')

w=one(w,
'''                if (accepted > 0) {
                    if (!s_gbt65k615e_reported) {''',
'''                if (accepted > 0) {
                    ++s_r56r_path[R56R_CPU0_65];
                    if (!s_gbt65k615e_reported) {''','65k accepted')
w=one(w,
'''                if (accepted == 0) {
                    /* Do not fall back to the measured ~99 ms CPU1 legacy''',
'''                if (accepted == 0) {
                    ++s_r56r_path[R56R_65_QFULL];
                    /* Do not fall back to the measured ~99 ms CPU1 legacy''','65k qfull')
w=one(w,
'''                /* accepted < 0 means the fast path is unavailable/invalid.
                 * Continue into the exact stock renderer for compatibility. */''',
'''                /* accepted < 0 means the fast path is unavailable/invalid.
                 * Continue into the exact stock renderer for compatibility. */
                ++s_r56r_path[R56R_65_REJECT];''','65k reject')

# Exact GBT accept sites.
w=one(w,
'''            if (accepted) {
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_SCROLL614C: CPU0 persistent GRP8 scroll-cache + BG+TEXT compositor ACTIVE pri G/T/B=%u/%u/%u\\n",''',
'''            if (accepted) {
                ++s_r56r_path[R56R_CPU0_GBT];
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_SCROLL614C: CPU0 persistent GRP8 scroll-cache + BG+TEXT compositor ACTIVE pri G/T/B=%u/%u/%u\\n",''','GBT scroll accepted')
w=one(w,
'''            if (accepted) return;
            WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));''',
'''            if (accepted) {
                ++s_r56r_path[R56R_CPU0_GBT];
                return;
            }
            WD_PERF_GRP(WinDraw_DrawGrp8PairChecked(grp8_bottom, grp8_top));''','GBT raw accepted')
w=one(w,
'''            if (accepted) {
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_GBT614B2: CPU0 G+BG+TEXT compositor ACTIVE; unequal-scroll uses 6.14a materialized-GRP fallback\\n");''',
'''            if (accepted) {
                ++s_r56r_path[R56R_CPU0_GBT];
                if (!s_gbt614a_reported) {
                    s_gbt614a_reported = 1;
                    printf("PX68K_GBT614B2: CPU0 G+BG+TEXT compositor ACTIVE; unequal-scroll uses 6.14a materialized-GRP fallback\\n");''','GBT split accepted')

w=one(w,
'''            if (accepted) {
                if (WD_PERF_ACTIVE) {
                    ++s_hp_accept;''',
'''            if (accepted) {
                ++s_r56r_path[R56R_CPU0_GRP8];
                if (WD_PERF_ACTIVE) {
                    ++s_hp_accept;''','GRP8 accepted')

w=one(w,
'''        if (compose_result != 2) {
            (void)tab5_screen_render_result(render_ticket, (uint32_t)TextDotX,
                                                &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
        }
\t\treturn;''',
'''        if (compose_result != 2) {
            ++s_r56r_path[R56R_CPU1_2L];
            (void)tab5_screen_render_result(render_ticket, (uint32_t)TextDotX,
                                                &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
        } else {
            ++s_r56r_path[R56R_CPU0_2L];
        }
\t\treturn;''','common two-layer outcome')

w=one(w,
'''    (void)tab5_screen_render_result(render_ticket, (uint32_t)TextDotX,
                                        &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
}

/********** menu''',
'''    ++s_r56r_path[R56R_CPU1_LEGACY];
    switch (VCReg0[1] & 3u) {
        case 0u: ++s_r56r_path[R56R_LEG16]; break;
        case 1u:
        case 2u: ++s_r56r_path[R56R_LEG256]; break;
        default: ++s_r56r_path[R56R_LEG65]; break;
    }
    (void)tab5_screen_render_result(render_ticket, (uint32_t)TextDotX,
                                        &RenderBuf[VLINE * FULLSCREEN_WIDTH]);
}

/********** menu''','legacy outcome')

m=one(m,'extern int WinX68k_ExecVideoProbeFrame(void);\n',
      'extern int WinX68k_ExecVideoProbeFrame(void);\nextern void WinDraw_R56RPathTake(uint32_t *out, uint32_t count);\n','main extern')
m=one(m,
'''    ESP_LOGI(TAG, "PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution; legacy WinX68k perf path remains QUIET-gated");''',
'''    ESP_LOGI(TAG, "PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution; legacy WinX68k perf path remains QUIET-gated");
    ESP_LOGI(TAG, "PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only");''','startup marker')
m=one(m,
'''#undef R56Q1_PCT
            cpu0bill_prev_wall_us = q_now_us;''',
'''#undef R56Q1_PCT
            {
                uint32_t rp[16] = {0};
                WinDraw_R56RPathTake(rp, 16u);
                ESP_LOGI(TAG,
                         "R56R_PATH f=%lu lat=%lu rej=%lu mode{16=%lu 256=%lu 65=%lu} "
                         "cpu0{65=%lu gbt=%lu grp8=%lu two=%lu} cpu1{two=%lu legacy=%lu} "
                         "legacyMode{16=%lu 256=%lu 65=%lu} 65fail{q=%lu reject=%lu}",
                         (unsigned long)frame,
                         (unsigned long)rp[0], (unsigned long)rp[1],
                         (unsigned long)rp[2], (unsigned long)rp[3], (unsigned long)rp[4],
                         (unsigned long)rp[5], (unsigned long)rp[6],
                         (unsigned long)rp[7], (unsigned long)rp[8],
                         (unsigned long)rp[9], (unsigned long)rp[10],
                         (unsigned long)rp[11], (unsigned long)rp[12], (unsigned long)rp[13],
                         (unsigned long)rp[14], (unsigned long)rp[15]);
            }
            cpu0bill_prev_wall_us = q_now_us;''','R56R output')

MAIN.write_text(m,encoding='utf-8',newline='\n')
WD.write_text(w,encoding='utf-8',newline='\n')
print('R56r applied: WinDraw path taxonomy counters enabled (64B, no timers/allocations/behavior changes)')
