#!/usr/bin/env python3
from pathlib import Path
import re, sys

FILES = [
    (Path("src/tab5_audio.cpp"), r"static\s+void\s+audio_task\s*\([^)]*\)", "px68k_audio", 4096),
    (Path("components/px68k/fmgen/fmg_wrap.cpp"), r"static(?:\s+IRAM_ATTR)?\s+void\s+async_opm_task\s*\([^)]*\)", "px68k_ym2151", 6144),
    (Path("src/tab5_usb_keyboard.c"), r"static\s+void\s+usb_library_task\s*\([^)]*\)", "tab5_usb_lib", 4096),
    (Path("src/tab5_usb_keyboard.c"), r"static\s+void\s+usb_control_task\s*\([^)]*\)", "tab5_usb_ctl", 4096),
]
INCS = ['#include \"freertos/idf_additions.h\"', '#include \"esp_rom_sys.h\"']

def fail(msg, rc=2):
    print("R56k5 self-stack patch ERROR:", msg, file=sys.stderr)
    sys.exit(rc)

def ensure_include(s):
    lines=s.splitlines()
    idx=0
    for i,l in enumerate(lines[:120]):
        if l.startswith("#include"):
            idx=i+1
    for inc in INCS:
        if inc not in lines:
            lines.insert(idx, inc)
            idx += 1
    return "\n".join(lines) + "\n"

def patch_one(path, sig_re, name, size):
    if not path.is_file():
        fail(f"{path} not found")
    raw=path.read_bytes()
    nl="\r\n" if b"\r\n" in raw else "\n"
    s=raw.decode("utf-8").replace("\r\n","\n")
    marker=f"R56K5_TASKSELF name={name}"
    if marker in s:
        print("R56k5 self-stack already present:", name)
        return
    s=ensure_include(s)
    m=re.search(sig_re,s,re.M)
    if not m:
        fail(f"function for {name} not found in {path}")
    brace=s.find("{",m.end())
    if brace<0:
        fail(f"opening brace not found for {name}")
    block = (
        "\n    {\n"
        "        const uintptr_t r56k5_base = (uintptr_t)pxTaskGetStackStart(NULL);\n"
        f'        esp_rom_printf("R56K5_TASKSELF name={name} core=%d base=0x%08x top=0x%08x bytes={size} hwm=%u\\\\n",\n'
        "                       (int)xPortGetCoreID(), (unsigned)r56k5_base,\n"
        f"                       (unsigned)(r56k5_base + {size}u),\n"
        "                       (unsigned)uxTaskGetStackHighWaterMark(NULL));\n"
        "    }\n"
    )
    s=s[:brace+1]+block+s[brace+1:]
    path.write_bytes(s.replace("\n",nl).encode("utf-8"))
    print("R56k5 self-stack patched:", name, path)

def main():
    for item in FILES:
        patch_one(*item)

if __name__=="__main__":
    main()
