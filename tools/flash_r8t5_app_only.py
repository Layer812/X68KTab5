#!/usr/bin/env python3
"""Flash only the R8T5 experimental X68KTab5 app on the validated Tab5 layout.

Never writes bootloader, partition table, NVS or HumanXDF.
The CI-provided manifest locks the firmware SHA-256.
"""
import argparse
import hashlib
import json
import re
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
IMAGE = ROOT / "build" / "px68k_tab5.bin"
MANIFEST = ROOT / "build_manifest.json"

# Binary partition table entries are 32 bytes, with 0x50AA little-endian magic.
ENTRY_FMT = "<HBBII16sI"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)
EXPECTED = {
    "nvs": (1, 2, 0x9000, 0x6000),
    "phy_init": (1, 1, 0xF000, 0x1000),
    "factory": (0, 0, 0x10000, 0x800000),
    "humanxdf": (1, 0x80, 0x810000, 0x140000),
}


def fail(message):
    raise RuntimeError(message)


def load_image():
    if not IMAGE.is_file() or not MANIFEST.is_file():
        fail("Missing CI artifact files: build/px68k_tab5.bin and build_manifest.json are required.")
    manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))
    image = IMAGE.read_bytes()
    digest = hashlib.sha256(image).hexdigest()
    if (
        manifest.get("repository") != "Layer812/X68KTab5"
        or manifest.get("target") != "esp32p4"
        or manifest.get("app_file") != "build/px68k_tab5.bin"
        or manifest.get("app_offset") != "0x10000"
        or manifest.get("flash_scope") != "app-only"
        or manifest.get("partition_readback_required") is not True
        or manifest.get("app_sha256") != digest
        or manifest.get("app_size") != len(image)
        or image[:1] != b"\\xe9"
        or not (100000 < len(image) < EXPECTED["factory"][3])
    ):
        fail("Application/header/manifest validation failed. Refusing to flash.")
    print("Verified CI app: %d bytes | SHA256 %s" % (len(image), digest))
    print("Source commit:", manifest.get("git_sha", "unknown"))
    return IMAGE


def esptool_commands():
    proc = subprocess.run(
        [sys.executable, "-m", "esptool", "--help"],
        capture_output=True, text=True, errors="replace",
    )
    if proc.returncode != 0:
        fail("esptool not available. Install with: python -m pip install esptool")
    output = proc.stdout + proc.stderr
    for spelling in ("read-flash", "read_flash"):
        if re.search(r"(?<![A-Za-z0-9_-])" + re.escape(spelling) + r"(?![A-Za-z0-9_-])", output):
            return spelling, "write-flash" if "-" in spelling else "write_flash"
    fail("Cannot identify esptool read-flash syntax. No write attempted.")


def run_tool(port, baud, command, args):
    cmd = [
        sys.executable, "-m", "esptool",
        "--chip", "esp32p4", "--port", port, "--baud", str(baud),
        command, *args,
    ]
    print("Executing esptool:", command, "(chip ESP32-P4; port %s)" % port, flush=True)
    subprocess.run(cmd, check=True)


def parse_partitions(data):
    if len(data) != 0x1000:
        fail("Failed to read complete device partition sector.")
    entries = {}
    for i in range(0, len(data) - ENTRY_SIZE + 1, ENTRY_SIZE):
        magic, typ, subtype, offset, size, label, flags = struct.unpack_from(ENTRY_FMT, data, i)
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            fail("Unexpected partition table entry magic at 0x%X." % i)
        name = label.split(b"\\x00", 1)[0].decode("ascii", "replace")
        if name in entries or size == 0 or offset + size > 0x1000000:
            fail("Invalid or duplicate device partition: " + name)
        entries[name] = (typ, subtype, offset, size)
    return entries


def validate_partition_table(entries):
    if entries != EXPECTED:
        print("Device layout:", entries)
        print("Required R6 layout:", EXPECTED)
        fail("Unexpected device layout. No flash; preserve HumanXDF and boot slots.")
    print("Device partition layout matches R6: factory 0x10000/8MiB, HumanXDF 0x810000/0x140000")
    print("HumanXDF and all data partitions will remain untouched.")


def main():
    parser = argparse.ArgumentParser(description="X68KTab5 R8T5 APP-only flash with device partition guard")
    parser.add_argument("--port", default="COM9", help="Tab5 port (default: COM9)")
    parser.add_argument("--baud", type=int, default=460800)
    args = parser.parse_args()

    image = load_image()
    read_cmd, write_cmd = esptool_commands()
    with tempfile.TemporaryDirectory(prefix="x68ktab_partition_read_") as td:
        part_path = Path(td) / "device_partitions.bin"
        # Read-only preflight. No erase or partition table writes.
        run_tool(args.port, args.baud, read_cmd, ["0x8000", "0x1000", str(part_path)])
        validate_partition_table(parse_partitions(part_path.read_bytes()))
    print("Writing application at 0x10000 ONLY ...", flush=True)
    run_tool(args.port, args.baud, write_cmd, ["0x10000", str(image)])
    print("APP-only flash completed; check LCD, SFXVI/MMDSP, MDX and screen transitions on Tab5.")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError, OSError, ValueError, KeyError) as exc:
        print("STOP: %s" % exc, file=sys.stderr)
        sys.exit(1)
