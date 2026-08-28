#!/usr/bin/env python3
from pathlib import Path
import sys, shutil

MAIN = Path("src/main.c")
Q_MARK = "PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600; heavyweight profiler remains OFF"
Q1_MARK = "PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution; legacy WinX68k perf path remains QUIET-gated"

OLD_SAMPLE = """        /* One detailed frame every 120 guest frames: enough to verify that
         * CPU1 compose time disappeared without restoring production profiler load. */
        mdx615e_perf_sample = (!PX68K_TAB5_R43_QUIET_RUNTIME) &&
                              mdx615e_diag_active && ((frame % 120u) == 60u);
"""
Q_SAMPLE = """        /* R56q: one already-existing detailed MDX performance sample every
         * 600 guest frames.  This reuses the legacy WinX68k one-frame sampler
         * only; heavyweight opcode/dispatch profiling remains disabled. */
        mdx615e_perf_sample =
            mdx615e_diag_active && ((frame % 600u) == 300u);
"""

OLD_COST_COND = """        if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && mdx615e_diag_active && ((frame % 240u) == 0u))
"""
Q_COST_COND = """        if (mdx615e_diag_active && ((frame % 600u) == 300u))
"""

DIAG = '    ESP_LOGI(TAG, "Diagnostics: QUIET (set PX68K_TAB5_DIAG_VERBOSE=1 to restore legacy traces)");\n'
Q_DIAG = DIAG + f'    ESP_LOGI(TAG, "{Q_MARK}");\n'
Q1_DIAG = DIAG + f'    ESP_LOGI(TAG, "{Q1_MARK}");\n'

EXEC_ANCHOR = """        int cycles = WinX68k_ExecVideoProbeFrame();
"""
EXEC_NEW = """        /* R56q1: deliberately do NOT wake the legacy WinX68k_Perf* graph.
         * One outer wall sample every 600 guest frames is enough to compare
         * directly with the historical MDXPERF core= number, with no large
         * profiler/static-data reachability change. */
        const bool r56q1_sample =
            mdx615e_diag_active && ((frame % 600u) == 300u);
        const int64_t r56q1_exec_t0 = r56q1_sample ? esp_timer_get_time() : 0;
        int cycles = WinX68k_ExecVideoProbeFrame();
        const uint32_t r56q1_exec_us = r56q1_sample
            ? (uint32_t)(esp_timer_get_time() - r56q1_exec_t0) : 0u;
"""

POST_ANCHOR = """#if !PX68K_TAB5_DIAG_VERBOSE
        (void)cycles;
#endif

        if (mdx615e_perf_sample)
"""
POST_NEW = """#if !PX68K_TAB5_DIAG_VERBOSE
        (void)cycles;
#endif

        if (r56q1_sample)
        {
            tab5_compose_stats_t qs = {0};
            tab5_audio_stats_t qa = {0};
            tab5_video_async_stats_t qv = {0};
            uint32_t q_fm = 0, q_fmcalls = 0, q_fmframes = 0;
            tab5_compose_get_stats(&qs);
            tab5_audio_get_stats(&qa);
            tab5_video_get_async_stats(&qv);
            OPM_AsyncWorkGet(&q_fm, &q_fmcalls, &q_fmframes);

            const int64_t q_now_us = esp_timer_get_time();
            const uint32_t q_wall = (uint32_t)(q_now_us - cpu0bill_prev_wall_us);
            const uint32_t q_dfm = q_fm - cpu0bill_prev_fm_us;
            const uint32_t q_dcomp = qs.gbt65k_cpu0_work_us - cpu0bill_prev_comp_us;
            const uint32_t q_dlcd = qv.cpu0_push_total_us - cpu0bill_prev_lcd_us;
            const uint32_t q_dmix = qa.cpu0_mix_work_us - cpu0bill_prev_mix_us;
            const uint32_t q_dspk = qa.cpu0_speaker_work_us - cpu0bill_prev_spk_us;
            const uint32_t q_dfmcalls = q_fmcalls - cpu0bill_prev_fm_calls;
            const uint32_t q_dfmframes = q_fmframes - cpu0bill_prev_fm_frames;
#define R56Q1_PCT(v) ((unsigned long)(q_wall ? ((uint64_t)(v) * 100u / q_wall) : 0u))
            ESP_LOGI(TAG,
                     "R56Q1_SAFE f=%lu exec=%luus cycles=%d budget=%s q=%lu render=%u "
                     "c0{wall=%lu fm=%lu/%lu%% comp=%lu/%lu%% lcd=%lu/%lu%% mix=%lu/%lu%% spk=%lu/%lu%% calls=%lu frames=%lu} "
                     "render{lines=%lu/%lu hit=%lu miss=%lu rebuild=%lu build=%luus last=%luus qfull=%lu waits=%lu pmax=%lu stale=%lu skip=%lu}",
                     (unsigned long)frame,
                     (unsigned long)r56q1_exec_us,
                     cycles,
                     tab5_budget_mode_name(budget.mode),
                     (unsigned long)budget.preexec_q_effective,
                     budget_render ? 1u : 0u,
                     (unsigned long)q_wall,
                     (unsigned long)q_dfm, R56Q1_PCT(q_dfm),
                     (unsigned long)q_dcomp, R56Q1_PCT(q_dcomp),
                     (unsigned long)q_dlcd, R56Q1_PCT(q_dlcd),
                     (unsigned long)q_dmix, R56Q1_PCT(q_dmix),
                     (unsigned long)q_dspk, R56Q1_PCT(q_dspk),
                     (unsigned long)q_dfmcalls, (unsigned long)q_dfmframes,
                     (unsigned long)qs.gbt65k_submitted_lines,
                     (unsigned long)qs.gbt65k_completed_lines,
                     (unsigned long)qs.gbt65k_cache_hits,
                     (unsigned long)qs.gbt65k_cache_misses,
                     (unsigned long)qs.gbt65k_cache_rebuilds,
                     (unsigned long)qs.last_gbt65k_build_us,
                     (unsigned long)qs.last_gbt65k_render_us,
                     (unsigned long)qs.queue_full,
                     (unsigned long)qs.frame_waits,
                     (unsigned long)qs.max_pending,
                     (unsigned long)qs.gbt65k_stale_dropped,
                     (unsigned long)qs.gbt65k_window_skipped);
#undef R56Q1_PCT
            cpu0bill_prev_wall_us = q_now_us;
            cpu0bill_prev_fm_us = q_fm;
            cpu0bill_prev_fm_calls = q_fmcalls;
            cpu0bill_prev_fm_frames = q_fmframes;
            cpu0bill_prev_comp_us = qs.gbt65k_cpu0_work_us;
            cpu0bill_prev_lcd_us = qv.cpu0_push_total_us;
            cpu0bill_prev_mix_us = qa.cpu0_mix_work_us;
            cpu0bill_prev_spk_us = qa.cpu0_speaker_work_us;
        }

        if (mdx615e_perf_sample)
"""

def fail(msg, rc=2):
    print("R56q1 safe attribution ERROR:", msg, file=sys.stderr)
    sys.exit(rc)

def normalize_q_to_p(s):
    # Undo only the three reachability changes R56q introduced.  This restores
    # the exact R56p quiet-gated legacy profiler shape before adding q1.
    if Q_SAMPLE in s:
        s = s.replace(Q_SAMPLE, OLD_SAMPLE, 1)
    if Q_COST_COND in s:
        n = s.count(Q_COST_COND)
        if n != 2:
            fail(f"R56q cost-condition count unexpected: {n}", 11)
        s = s.replace(Q_COST_COND, OLD_COST_COND, 2)
    if Q_DIAG in s:
        s = s.replace(Q_DIAG, DIAG, 1)
    elif Q_MARK in s:
        # Defensive line-only fallback.
        s = s.replace(f'    ESP_LOGI(TAG, "{Q_MARK}");\n', '', 1)
    return s

def verify(s):
    for tok in [
        Q1_MARK,
        "PX68K_R56P: QUIET exact-executor counters visible; fact-only",
        "MDXQ{CMPHI=",
        "R56Q1_SAFE f=",
        "const bool r56q1_sample =",
        "r56q1_exec_us",
        "CPU613C14R: production benchmark mode: heavyweight CPU/opcode/render profiler OFF",
    ]:
        if tok not in s:
            fail("verify missing: " + tok, 20)
    if s.count(Q1_MARK) != 1 or s.count("R56Q1_SAFE f=") != 1:
        fail("q1 marker/line duplicate", 21)
    if Q_MARK in s or Q_SAMPLE in s:
        fail("unsafe R56q legacy-profiler reachability remains", 22)
    if s.count(OLD_COST_COND) != 2:
        fail("legacy RENDER/CPU0COST quiet gates not restored", 23)
    if OLD_SAMPLE not in s:
        fail("legacy MDXPERF quiet gate not restored", 24)
    # Production lineage remains R56o-clean; q1 intentionally uses esp_timer
    # only on one frame per 600 and does not resurrect R56n cycle probes.
    for tok in ["R56N_WIN f=", "r56n_guest_cycles", "esp_cpu_get_cycle_count"]:
        if tok in s:
            fail("R56n residue present: " + tok, 25)
    if "PX68K_MDX622: ZERO-RUN batch ACTIVE" in s:
        fail("historical ZERO-RUN unexpectedly enabled", 26)

def main():
    if not MAIN.is_file():
        fail(r"src/main.c missing; run from G:\px68k-tab5 project root")
    raw = MAIN.read_bytes()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    s = raw.decode("utf-8").replace("\r\n", "\n")

    if Q1_MARK in s:
        verify(s)
        print("R56q1 already applied and verified:", MAIN)
        return
    if "PX68K_R56P: QUIET exact-executor counters visible; fact-only" not in s:
        fail("R56p basis missing; normalize through R56o/R56p first", 10)

    out = normalize_q_to_p(s)

    if Q_MARK in out or Q_SAMPLE in out or Q_COST_COND in out:
        fail("R56q rollback incomplete", 12)
    if OLD_SAMPLE not in out or out.count(OLD_COST_COND) != 2:
        fail("R56p quiet-gated legacy-profiler anchors missing", 13)

    if Q1_DIAG not in out:
        if out.count(DIAG) != 1:
            fail("QUIET diagnostics anchor missing/unexpected", 14)
        out = out.replace(DIAG, Q1_DIAG, 1)

    if out.count(EXEC_ANCHOR) != 1:
        fail("WinX68k exec anchor missing/unexpected", 15)
    out = out.replace(EXEC_ANCHOR, EXEC_NEW, 1)

    if out.count(POST_ANCHOR) != 1:
        fail("post-exec insertion anchor missing/unexpected", 16)
    out = out.replace(POST_ANCHOR, POST_NEW, 1)

    bak = MAIN.with_suffix(MAIN.suffix + ".r56q1.bak")
    if not bak.exists():
        shutil.copy2(MAIN, bak)
    MAIN.write_bytes(out.replace("\n", nl).encode("utf-8"))
    verify(out)
    print("R56q1 applied: R56q legacy profiler reachability rolled back; memory-safe sparse exec/CPU0 stats enabled")

if __name__ == "__main__":
    main()
