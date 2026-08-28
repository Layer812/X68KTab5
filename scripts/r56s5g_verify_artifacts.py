from pathlib import Path
import sys, hashlib
if len(sys.argv)!=4:
    print('usage: r56s5g_verify_artifacts.py <source> <elf> <bin>'); raise SystemExit(2)
source,elf,binp=map(Path,sys.argv[1:])
markers=[b'PX68K_R56S5:',b'PX68K_R56S5F:',b'PX68K_R56S5G_BOOTPROOF:']
for p in (source,elf,binp):
    if not p.is_file():
        print('R56s5g artifact verify FAIL missing',p); raise SystemExit(3)
    data=p.read_bytes()
    for m in markers:
        if m not in data:
            print('R56s5g artifact verify FAIL:',p,'missing',m.decode()); raise SystemExit(4)
    print(p, 'sha256=', hashlib.sha256(data).hexdigest())
print('R56s5g ARTIFACT VERIFY PASS: SOURCE + ELF + BIN contain R56s5/R56s5f/BOOTPROOF')
