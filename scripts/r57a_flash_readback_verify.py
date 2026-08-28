from pathlib import Path
import hashlib, subprocess, sys

if len(sys.argv) != 4:
    print('usage: r56s5j_flash_readback_verify.py <port> <bin> <offset>')
    raise SystemExit(2)

port = sys.argv[1]
binp = Path(sys.argv[2])
off = int(sys.argv[3], 0)

if not binp.is_file():
    print('R57a READBACK FAIL: missing built BIN:', binp)
    raise SystemExit(3)

marker = b'PX68K_R56S5G_BOOTPROOF:'
b = binp.read_bytes()
if marker not in b:
    print('R57a READBACK FAIL: BOOTPROOF absent from built BIN')
    raise SystemExit(4)

out = binp.parent / 'r57a_flash_readback.bin'
try:
    out.unlink()
except FileNotFoundError:
    pass

cmd = [
    sys.executable, '-m', 'esptool', '--chip', 'esp32p4', '-p', port,
    '--before', 'default_reset', '--after', 'hard_reset',
    'read_flash', hex(off), hex(len(b)), str(out)
]
print('R57a readback command:', ' '.join(cmd))
rc = subprocess.run(cmd).returncode
if rc:
    print('R57a READBACK FAIL: esptool rc', rc)
    raise SystemExit(10)

f = out.read_bytes()
bh = hashlib.sha256(b).hexdigest()
fh = hashlib.sha256(f).hexdigest()
print('R57a built   sha256=', bh)
print('R57a flashed sha256=', fh)

if b != f:
    n = min(len(b), len(f))
    bad = next((i for i in range(n) if b[i] != f[i]), n)
    print(f'R57a READBACK FAIL: mismatch +0x{bad:X} built={len(b)} flash={len(f)}')
    raise SystemExit(11)

if marker not in f:
    print('R57a READBACK FAIL: BOOTPROOF absent from physical FLASH bytes')
    raise SystemExit(12)

print('R57A FLASH READBACK PASS: built BIN == physical factory app byte-for-byte')
print('R57A BOOTPROOF PRESENT IN PHYSICAL FLASH')
try:
    out.unlink()
except OSError:
    pass
