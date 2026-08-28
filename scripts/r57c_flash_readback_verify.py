from pathlib import Path
import hashlib, subprocess, sys
if len(sys.argv)!=4: print('usage: r57c_flash_readback_verify.py <port> <bin> <offset>'); raise SystemExit(2)
port=sys.argv[1]; binp=Path(sys.argv[2]); off=int(sys.argv[3],0)
if not binp.is_file(): print('R57c READBACK FAIL missing BIN',binp); raise SystemExit(3)
b=binp.read_bytes(); marker=b'PX68K_R57C:'
if marker not in b: print('R57c READBACK FAIL marker absent from built BIN'); raise SystemExit(4)
out=binp.parent/'r57c_flash_readback.bin'
try: out.unlink()
except FileNotFoundError: pass
cmd=[sys.executable,'-m','esptool','--chip','esp32p4','-p',port,'--before','default_reset','--after','hard_reset','read_flash',hex(off),hex(len(b)),str(out)]
print('R57c readback command:',' '.join(cmd)); rc=subprocess.run(cmd).returncode
if rc: print('R57c READBACK FAIL esptool rc',rc); raise SystemExit(10)
f=out.read_bytes(); bh=hashlib.sha256(b).hexdigest(); fh=hashlib.sha256(f).hexdigest()
print('R57c built   sha256=',bh); print('R57c flashed sha256=',fh)
if b!=f:
 n=min(len(b),len(f)); bad=next((i for i in range(n) if b[i]!=f[i]),n)
 print(f'R57c READBACK FAIL mismatch +0x{bad:X} built={len(b)} flash={len(f)}'); raise SystemExit(11)
print('R57C FLASH READBACK PASS: built BIN == physical factory app byte-for-byte')
try: out.unlink()
except OSError: pass
