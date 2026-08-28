from pathlib import Path
import sys
if len(sys.argv)!=4: raise SystemExit(2)
source,elf,binp=map(Path,sys.argv[1:])
markers=[b'PX68K_R56S5:', b'PX68K_R56S5F:']
for p in (source,elf,binp):
    data=p.read_bytes()
    for m in markers:
        if m not in data:
            print('R56s5f artifact verify FAIL:',p,'missing',m.decode())
            raise SystemExit(3)
print('R56s5f ARTIFACT VERIFY PASS: SOURCE + ELF + BIN contain R56s5/R56s5f markers')
