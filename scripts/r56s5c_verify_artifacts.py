from pathlib import Path
import hashlib, sys
if len(sys.argv)!=4:
    print('usage: r56s5c_verify_artifacts.py <main.c> <elf> <bin>'); raise SystemExit(2)
main,elf,binp=map(Path,sys.argv[1:])
markers=[
 b'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active',
 b'PX68K_R56S5C: live validator stale-discard retry armed',
]
for p in (main,elf,binp):
    if not p.is_file(): print('R56s5c PROOF FAIL missing',p); raise SystemExit(3)
    d=p.read_bytes()
    for m in markers:
        if m not in d:
            print('R56s5c PROOF FAIL marker missing in',p,':',m.decode()); raise SystemExit(4)
    print(f'R56s5c markers OK: {p} size={len(d)} sha256={hashlib.sha256(d).hexdigest()}')
print('R56s5c ARTIFACT PROOF PASS')
