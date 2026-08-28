from pathlib import Path
import hashlib, subprocess, sys

if len(sys.argv) != 4:
    print('usage: r56s5c_flash_readback_verify.py <port> <bin> <offset>')
    raise SystemExit(2)
port=sys.argv[1]; binp=Path(sys.argv[2]); offset=int(sys.argv[3],0)
if not binp.is_file():
    print('R56s5c READBACK FAIL: built bin missing:', binp); raise SystemExit(3)
out=binp.parent/'r56s5c_flash_readback.bin'
try: out.unlink()
except FileNotFoundError: pass
size=binp.stat().st_size
cmd=[sys.executable,'-m','esptool','--chip','esp32p4','-p',port,
     '--before','default-reset','--after','hard-reset','read_flash',hex(offset),hex(size),str(out)]
print('R56s5c readback:', ' '.join(cmd))
rc=subprocess.run(cmd).returncode
if rc:
    print('R56s5c READBACK FAIL: esptool rc=',rc); raise SystemExit(10)
b=binp.read_bytes(); f=out.read_bytes()
print('R56s5c built   sha256=',hashlib.sha256(b).hexdigest())
print('R56s5c flashed sha256=',hashlib.sha256(f).hexdigest())
if b != f:
    n=min(len(b),len(f)); bad=next((i for i in range(n) if b[i]!=f[i]),n)
    print(f'R56s5c READBACK FAIL: mismatch at +0x{bad:X}, built={len(b)}, flashed={len(f)}')
    raise SystemExit(11)
print('R56s5c FLASH READBACK PROOF PASS: app partition exactly matches built R56s5 image')
try: out.unlink()
except OSError: pass
