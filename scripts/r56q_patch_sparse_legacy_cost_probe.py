#!/usr/bin/env python3
from pathlib import Path
import sys, shutil

MAIN = Path("src/main.c")
MARK = "PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600; heavyweight profiler remains OFF"

OLD_SAMPLE = """        /* One detailed frame every 120 guest frames: enough to verify that
         * CPU1 compose time disappeared without restoring production profiler load. */
        mdx615e_perf_sample = (!PX68K_TAB5_R43_QUIET_RUNTIME) &&
                              mdx615e_diag_active && ((frame % 120u) == 60u);
"""
NEW_SAMPLE = """        /* R56q: one already-existing detailed MDX performance sample every
         * 600 guest frames.  This reuses the legacy WinX68k one-frame sampler
         * only; heavyweight opcode/dispatch profiling remains disabled. */
        mdx615e_perf_sample =
            mdx615e_diag_active && ((frame % 600u) == 300u);
"""

OLD_COST_COND = """        if ((!PX68K_TAB5_R43_QUIET_RUNTIME) && mdx615e_diag_active && ((frame % 240u) == 0u))
"""
NEW_COST_COND = """        if (mdx615e_diag_active && ((frame % 600u) == 300u))
"""

OLD_DIAG = """    ESP_LOGI(TAG, "Diagnostics: QUIET (set PX68K_TAB5_DIAG_VERBOSE=1 to restore legacy traces)");
"""
NEW_DIAG = """    ESP_LOGI(TAG, "Diagnostics: QUIET (set PX68K_TAB5_DIAG_VERBOSE=1 to restore legacy traces)");
    ESP_LOGI(TAG, "PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600; heavyweight profiler remains OFF");
"""

def fail(msg, rc=2):
    print("R56q sparse legacy-cost probe ERROR:", msg, file=sys.stderr)
    sys.exit(rc)

def verify(s):
    must = [
        MARK,
        "PX68K_R56P: QUIET exact-executor counters visible; fact-only",
        "MDXQ{CMPHI=",
        "MDXPERF615H17 f=",
        "RENDER615H17 f=",
        "CPU0COST615H17 wall=",
        "mdx615e_diag_active && ((frame % 600u) == 300u)",
        "CPU613C14R: production benchmark mode: heavyweight CPU/opcode/render profiler OFF",
    ]
    for tok in must:
        if tok not in s:
            fail("verify missing: " + tok, 20)
    if s.count(MARK) != 1:
        fail("R56q marker duplicate/missing", 21)
    # Exactly three sparse cadence sites: detailed-frame sample, RENDER, CPU0COST.
    if s.count("mdx615e_diag_active && ((frame % 600u) == 300u)") != 3:
        fail("unexpected sparse cadence site count", 22)
    if OLD_COST_COND in s or OLD_SAMPLE in s:
        fail("old quiet-gated/high-cadence attribution still present", 23)
    # Keep R56o production cleanup intact.
    for tok in ["R56N_WIN f=", "r56n_guest_cycles", "esp_cpu_get_cycle_count"]:
        if tok in s:
            fail("R56n attribution residue returned: " + tok, 24)
    # R56q must not enable the abandoned historical ZERO-RUN executor.
    if "PX68K_MDX622: ZERO-RUN batch ACTIVE" in s:
        fail("historical ZERO-RUN unexpectedly enabled", 25)

def main():
    if not MAIN.is_file():
        fail(r"src/main.c missing; run from G:\px68k-tab5 project root")
    raw = MAIN.read_bytes()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    s = raw.decode("utf-8").replace("\r\n", "\n")
    if MARK in s:
        verify(s)
        print("R56q already applied and verified:", MAIN)
        return
    if "PX68K_R56P: QUIET exact-executor counters visible; fact-only" not in s:
        fail("R56p basis missing; run R56p normalizer first", 10)
    if "R56N_WIN f=" in s or "r56n_guest_cycles" in s or "esp_cpu_get_cycle_count" in s:
        fail("R56n attribution residue present; run R56o/R56p normalization first", 11)
    if s.count(OLD_SAMPLE) != 1:
        fail("legacy MDXPERF sample anchor missing/unexpected", 12)
    if s.count(OLD_COST_COND) != 2:
        fail("expected exactly two quiet-gated RENDER/CPU0COST blocks", 13)
    if s.count(OLD_DIAG) != 1:
        fail("diagnostics startup anchor missing/unexpected", 14)

    out = s.replace(OLD_SAMPLE, NEW_SAMPLE, 1)
    out = out.replace(OLD_COST_COND, NEW_COST_COND, 2)
    out = out.replace(OLD_DIAG, NEW_DIAG, 1)

    bak = MAIN.with_suffix(MAIN.suffix + ".r56q.bak")
    if not bak.exists():
        shutil.copy2(MAIN, bak)
    MAIN.write_bytes(out.replace("\n", nl).encode("utf-8"))
    verify(out)
    print("R56q applied: sparse legacy MDXPERF/RENDER/CPU0COST visible every 600 frames; no emulator/audio/scheduler policy change")

if __name__ == "__main__":
    main()
