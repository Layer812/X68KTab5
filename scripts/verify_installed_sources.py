from pathlib import Path
import hashlib, sys
if len(sys.argv) != 3:
    print('usage: verify_installed_sources.py <project_root> <package_root>')
    raise SystemExit(2)
root=Path(sys.argv[1]); pkg=Path(sys.argv[2])
manifest=pkg/'MANIFEST_SHA256.txt'
if not manifest.is_file():
    print('R57d4 SOURCE VERIFY FAIL: manifest missing',manifest); raise SystemExit(3)
for line in manifest.read_text().splitlines():
    if not line.strip(): continue
    h, rel=line.split(None,1); rel=rel.strip()
    dst=root/rel; src=pkg/'payload'/rel
    for label,p in [('payload',src),('installed',dst)]:
        if not p.is_file(): print('R57d4 SOURCE VERIFY FAIL:',label,'missing',p); raise SystemExit(4)
        got=hashlib.sha256(p.read_bytes()).hexdigest()
        if got != h:
            print('R57d4 SOURCE VERIFY FAIL:',label,rel,'expected',h,'got',got); raise SystemExit(5)
    print('PASS',rel,h[:16])
print('R57D4 SOURCE OVERWRITE VERIFY PASS: installed files are byte-identical to reviewed payload')
