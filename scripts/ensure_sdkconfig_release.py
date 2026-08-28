# Build 6.15h17-dbfb: converge old/fresh PlatformIO worktrees on the exact
# known-good 6.15g memory/runtime baseline.  Explicitly remove every LP-core/ULP
# setting from experimental h..h13 trees so this build is a genuinely clean HP-only baseline.
Import("env")
from pathlib import Path

project = Path(env.subst("$PROJECT_DIR"))
pioenv = env.subst("$PIOENV")
settings = {
    "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU0": "y",
    "CONFIG_ESP_MAIN_TASK_AFFINITY": "0x0",
    "CONFIG_ESP_MAIN_TASK_STACK_SIZE": "12288",
    "CONFIG_ESP_TASK_WDT_TIMEOUT_S": "15",
    "CONFIG_USB_HOST_RESET_RECOVERY_MS": "80",
    # Build 6.11b: a fresh ZIP has no previously generated sdkconfig.  Keep
    # the proven Tab5 HEX-PSRAM/200 MHz configuration explicit so both fresh
    # builds and old working directories converge on the same configuration.
    "CONFIG_IDF_EXPERIMENTAL_FEATURES": "y",
    "CONFIG_SPIRAM": "y",
    "CONFIG_SPIRAM_MODE_HEX": "y",
    "CONFIG_SPIRAM_SPEED_200M": "y",
    "CONFIG_SPIRAM_BOOT_INIT": "y",
    "CONFIG_SPIRAM_USE_MALLOC": "y",
    "CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY": "y",
    "CONFIG_SPIRAM_MALLOC_RESERVE_INTERNAL": "32768",
    # Build 6.13c14r2: c13 hardware used 128 KiB L2.  On ESP32-P4 rev<3,
    # selecting 512 KiB removes SRAM_HIGH entirely and forces its .dram1 data
    # back into SRAM_LOW.  The observed 244160-byte overflow is exactly the
    # known-good high-data footprint minus the c13 low-SRAM slack.
    "CONFIG_CACHE_L2_CACHE_128KB": "y",
    "CONFIG_CACHE_L2_CACHE_SIZE": "0x20000",
    "CONFIG_CACHE_L2_CACHE_LINE_64B": "y",
    "CONFIG_CACHE_L2_CACHE_LINE_SIZE": "64",
    # Match platformio.ini / Tab5 hardware and silence stale 2 MiB sdkconfig.
    "CONFIG_ESPTOOLPY_FLASHSIZE_16MB": "y",
    "CONFIG_ESPTOOLPY_FLASHSIZE": '"16MB"',
}
unsets = {
    "CONFIG_ESP_MAIN_TASK_AFFINITY_CPU1",
    "CONFIG_ESP_MAIN_TASK_AFFINITY_NO_AFFINITY",
    "CONFIG_FREERTOS_GENERATE_RUN_TIME_STATS",
    "CONFIG_FREERTOS_USE_TRACE_FACILITY",
    "CONFIG_FREERTOS_RUN_TIME_STATS_USING_ESP_TIMER",
    "CONFIG_FREERTOS_RUN_TIME_COUNTER_TYPE_U64",
    "CONFIG_SPIRAM_MODE_QUAD",
    "CONFIG_SPIRAM_MODE_OCT",
    "CONFIG_SPIRAM_SPEED_40M",
    "CONFIG_SPIRAM_SPEED_80M",
    "CONFIG_SPIRAM_SPEED_120M",
    # Build 6.13c14r2: remove stale cache/flash choice alternatives before
    # setting the known-good values above.
    "CONFIG_CACHE_L2_CACHE_256KB",
    "CONFIG_CACHE_L2_CACHE_512KB",
    "CONFIG_CACHE_L2_CACHE_LINE_128B",
    "CONFIG_ESPTOOLPY_FLASHSIZE_1MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_2MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_4MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_8MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_32MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_64MB",
    "CONFIG_ESPTOOLPY_FLASHSIZE_128MB",
    # Build 6.15h17-dbfb: LP-core experiments are archived, not active.
    "CONFIG_ULP_COPROC_ENABLED",
    "CONFIG_ULP_COPROC_TYPE_LP_CORE",
    "CONFIG_ULP_COPROC_RUN_FROM_HP_MEM",
    "CONFIG_RTC_FAST_CLK_SRC_XTAL",
    # Build 6.13b2: native Dynarec needs runtime write + execute on the
    # statically reserved internal SRAM arena. ESP-IDF 5.4.2 enables both
    # protection mechanisms by default where supported; force them off even
    # when rebuilding in an existing PlatformIO worktree.
    "CONFIG_ESP_SYSTEM_PMP_IDRAM_SPLIT",
    "CONFIG_ESP_SYSTEM_MEMPROT_FEATURE",
    "CONFIG_ESP_SYSTEM_MEMPROT_FEATURE_LOCK",
}

def patch_sdkconfig(path: Path) -> bool:
    if not path.exists():
        return False
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    done, unset_done, out = set(), set(), []
    for line in lines:
        replaced = False
        for key in unsets:
            if line.startswith(key + "=") or line == f"# {key} is not set":
                out.append(f"# {key} is not set")
                unset_done.add(key); replaced = True; break
        if replaced:
            continue
        for key, value in settings.items():
            if line.startswith(key + "=") or line == f"# {key} is not set":
                out.append(f"{key}={value}")
                done.add(key); replaced = True; break
        if not replaced:
            out.append(line)
    for key, value in settings.items():
        if key not in done: out.append(f"{key}={value}")
    for key in unsets:
        if key not in unset_done: out.append(f"# {key} is not set")
    path.write_text("\n".join(out) + "\n", encoding="utf-8")
    print(f"Build 6.15h17-dbfb: known-good 6.15g memory + HP-only (ULP disabled) settings enforced for DoubleFB build in {path.name}")
    return True

patched = patch_sdkconfig(project / f"sdkconfig.{pioenv}")
patched = patch_sdkconfig(project / "sdkconfig") or patched
if not patched:
    print("Build 6.15h17-dbfb: no generated sdkconfig yet; sdkconfig.defaults will seed known-good 6.15g HP-only configuration")
