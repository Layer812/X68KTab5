#!/usr/bin/env python3
from __future__ import annotations

import argparse
import configparser
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INI = ROOT / "platformio.ini"

def load_ini():
    cp = configparser.ConfigParser(interpolation=None, strict=False)
    cp.optionxform = str
    with INI.open("r", encoding="utf-8-sig") as f:
        cp.read_file(f)
    return cp

def section(cp, env):
    name = f"env:{env}"
    if not cp.has_section(name):
        raise SystemExit(f"ERROR: [{name}] not found in {INI}")
    return name

def resolve_flags(cp, env, seen=None):
    if seen is None:
        seen = set()
    if env in seen:
        raise SystemExit(f"ERROR: circular extends detected at {env}")
    seen.add(env)

    sec = section(cp, env)
    inherited = ""
    if cp.has_option(sec, "extends"):
        parent = cp.get(sec, "extends", raw=True).strip()
        if parent.startswith("env:"):
            parent = parent[4:]
        inherited = resolve_flags(cp, parent, seen)

    own = cp.get(sec, "build_flags", raw=True, fallback="")
    own = re.sub(r"\$\{env:[^}]+\.build_flags\}", inherited, own)
    if inherited and inherited not in own:
        own = inherited + "\n" + own
    return own

def macro_value(flags, name, default):
    pat = re.compile(r"(?:^|\s)-D" + re.escape(name) + r"(?:=([^\s#;]+))?")
    hits = list(pat.finditer(flags))
    if not hits:
        return str(default)
    v = hits[-1].group(1)
    return v if v is not None else "1"

def idf_python_and_script():
    idf_path = os.environ.get("IDF_PATH")
    if not idf_path:
        raise SystemExit("ERROR: IDF_PATH is not set. Run G:\\esp-idf-5.5.4\\export.bat first.")

    idf_py = Path(idf_path) / "tools" / "idf.py"
    if not idf_py.exists():
        raise SystemExit(f"ERROR: idf.py not found: {idf_py}")

    # Prefer the Python environment selected by export.bat.
    env_path = os.environ.get("IDF_PYTHON_ENV_PATH")
    if env_path:
        candidate = Path(env_path) / "Scripts" / "python.exe"
        if candidate.exists():
            return candidate, idf_py

    # Fallback: the interpreter running this script.
    return Path(sys.executable), idf_py

def run_idf(py, idf_py, args):
    cmd = [str(py), str(idf_py), *map(str, args)]
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, cwd=ROOT, check=True)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("env", nargs="?", help="PlatformIO env name")
    ap.add_argument("--upload", action="store_true")
    ap.add_argument("--monitor", action="store_true")
    ap.add_argument("--port", default="COM9")
    args = ap.parse_args()

    cp = load_ini()
    env = args.env or cp.get("platformio", "default_envs", fallback="m5stack-tab5").split(",")[0].strip()

    flags = resolve_flags(cp, env)
    turbo = macro_value(flags, "PX68K_TAB5_P4_TURBO400", "0")
    mhz = macro_value(flags, "PX68K_TAB5_HOST_CPU_MHZ", "360")

    if turbo not in ("0", "1"):
        raise SystemExit(f"ERROR: invalid PX68K_TAB5_P4_TURBO400={turbo}")
    if mhz not in ("360", "400"):
        raise SystemExit(f"ERROR: invalid PX68K_TAB5_HOST_CPU_MHZ={mhz}")
    if turbo == "1" and mhz != "400":
        raise SystemExit("ERROR: Turbo profile must request 400 MHz")
    if turbo == "0" and mhz != "360":
        raise SystemExit("ERROR: Standard profile must remain 360 MHz")

    build = ROOT / ("build-idf55-profile-" + re.sub(r"[^A-Za-z0-9_.-]", "_", env))
    py, idf_py = idf_python_and_script()

    print("=" * 68)
    print("R57E69 Native Profile Bridge V2")
    print(f"platformio.ini env : {env}")
    print(f"Turbo macro        : {turbo}")
    print(f"Host CPU target    : {mhz} MHz")
    print("Guest target       : 10.000 MHz (unchanged)")
    print(f"IDF Python         : {py}")
    print(f"IDF script         : {idf_py}")
    print(f"Build directory    : {build.name}")
    print("=" * 68)

    cmake_defs = [
        f"-DPX68K_TAB5_P4_TURBO400={turbo}",
        f"-DPX68K_TAB5_HOST_CPU_MHZ={mhz}",
    ]

    # NOTE: idf.py is a Python script on Windows.  Do not pass "idf.py"
    # directly to CreateProcess; explicitly launch it through IDF Python.
    run_idf(py, idf_py, ["-B", build, *cmake_defs, "reconfigure"])
    run_idf(py, idf_py, ["-B", build, "build"])

    if args.upload:
        run_idf(py, idf_py, ["-B", build, "-p", args.port, "flash"])

    if args.monitor:
        print()
        if turbo == "1":
            print("Expected marker: TAB5_P4CLK: ... TURBO400 ... HP cores=400 MHz")
        else:
            print("Expected marker: TAB5_P4CLK: R57E69 P4 STANDARD: HP cores=360 MHz")
        run_idf(py, idf_py, ["-B", build, "-p", args.port, "monitor"])

if __name__ == "__main__":
    try:
        main()
    except subprocess.CalledProcessError as e:
        raise SystemExit(e.returncode)
