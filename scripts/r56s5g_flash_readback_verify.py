from pathlib import Path
import hashlib, subprocess, sys
if len(sys.argv)!=4:
    print('usage: r56s5g_flash_readback_verify.py <port> <bin> <offset>'); raise SystemExit(2)
port=sys.argv[1]; binp=Path(sys.argv[2]); off=int(sys.argv[3],0)
if not binp.is_file():
    print('R56s5g READBACK FAIL missing built bin',binp); raise SystemExit(3)
marker=b'PX68K_R56S5G_BOOTPROOF:'
b=binp.read_bytes()
if marker not in b:
    print('R56s5g READBACK FAIL: BOOTPROOF absent from built bin'); raise SystemExit(4)
out=binp.parent/'r56s5g_flash_readback.bin'
try: out.unlink()
except FileNotFoundError: pass
cmd=[sys.executable,'-m','esptool','--chip','esp32p4','-p',port,'--before','default-reset','--after','hard-reset','read_flash',hex(off),hex(len(b)),str(out)]
print('R56s5g readback command:', ' '.join(cmd))
rc=subprocess.run(cmd).returncode
if rc: print('R56s5g READBACK FAIL esptool rc',rc); raise SystemExit(10)
f=out.read_bytes()
print('R56s5g built   sha256=',hashlib.sha256(b).hexdigest())
print('R56s5g flashed sha256=',hashlib.sha256(f).hexdigest())
if b!=f:
    n=min(len(b),len(f)); bad=next((i for i in range(n) if b[i]!=f[i]),n)
    print(f'R56s5g READBACK FAIL mismatch +0x{bad:X} built={len(b)} flash={len(f)}'); raise SystemExit(11)
if marker not in f:
    print('R56s5g READBACK FAIL: BOOTPROOF absent from FLASH bytes despite compare'); raise SystemExit(12)
print('R56s5g FLASH READBACK PASS: byte-identical AND BOOTPROOF marker physically present at factory app')
try: out.unlink()
except OSError: pass
