from pathlib import Path
import hashlib, subprocess, sys, os

if len(sys.argv) != 4:
    print('usage: r56s5b_flash_readback_verify.py <port> <bin> <offset>')
    raise SystemExit(2)
port = sys.argv[1]
binp = Path(sys.argv[2])
offset = int(sys.argv[3], 0)
if not binp.is_file():
    print('R56s5b FLASH VERIFY FAIL: built bin missing:', binp)
    raise SystemExit(3)
size = binp.stat().st_size
out = binp.parent / 'r56s5b_flash_readback.bin'
try:
    out.unlink()
except FileNotFoundError:
    pass
cmd = [sys.executable, '-m', 'esptool', '--chip', 'esp32p4', '-p', port,
       'read_flash', hex(offset), hex(size), str(out)]
print('R56s5b readback command:', ' '.join(cmd))
rc = subprocess.run(cmd).returncode
if rc != 0:
    print('R56s5b FLASH VERIFY FAIL: esptool read_flash rc=', rc)
    raise SystemExit(10)
built = binp.read_bytes(); flashed = out.read_bytes()
if len(flashed) != len(built):
    print('R56s5b FLASH VERIFY FAIL: size mismatch built/readback', len(built), len(flashed))
    raise SystemExit(11)
sha_b = hashlib.sha256(built).hexdigest(); sha_f = hashlib.sha256(flashed).hexdigest()
print('R56s5b built   sha256=', sha_b)
print('R56s5b flashed sha256=', sha_f)
if built != flashed:
    # Report first mismatch, but do not dump image contents.
    bad = next((i for i,(a,b) in enumerate(zip(built, flashed)) if a != b), -1)
    print(f'R56s5b FLASH VERIFY FAIL: byte mismatch at +0x{bad:X}')
    raise SystemExit(12)
print('R56s5b FLASH READBACK PASS: factory app is byte-for-byte identical to built R56s5 image')
try:
    out.unlink()
except OSError:
    pass
