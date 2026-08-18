# Build 6.12c: keep production CPU affinity/WDT and the 6.11b fresh-build PSRAM fix self-contained.
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
    print(f"Build 6.12c: CPU0/main-stack/WDT/USB + Tab5 HEX PSRAM 200MHz settings enforced in {path.name}")
    return True

patched = patch_sdkconfig(project / f"sdkconfig.{pioenv}")
patched = patch_sdkconfig(project / "sdkconfig") or patched
if not patched:
    print("Build 6.12c: no generated sdkconfig yet; sdkconfig.defaults will seed CPU0/main-stack/WDT/USB + Tab5 HEX PSRAM 200MHz")
