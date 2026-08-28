from pathlib import Path
import re, sys
P=Path('src/main.c')
MARK='PX68K_R56S5G_BOOTPROOF: early app_main reached; this binary is the R56s5f app-flash build'
TAG='PX68K_R56S5G_BOOTPROOF'

def fail(s):
    print('R56s5g bootproof patch ERROR:',s); raise SystemExit(2)
if not P.is_file(): fail('missing src/main.c')
s=P.read_text(encoding='utf-8')
if MARK in s:
    print('R56s5g bootproof already applied')
    raise SystemExit(0)
if 'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active' not in s:
    fail('R56s5 lineage missing')
m=re.search(r'\bvoid\s+app_main\s*\(\s*void\s*\)\s*\{',s)
if not m:
    # tolerate empty parameter spelling
    m=re.search(r'\bvoid\s+app_main\s*\(\s*\)\s*\{',s)
if not m: fail('app_main opening brace not found')
pos=m.end()
insert='\n    ESP_LOGI(TAG, "'+MARK+'");'
s2=s[:pos]+insert+s[pos:]
if s2.count(TAG)!=1: fail('post-patch bootproof tag count !=1')
P.write_text(s2,encoding='utf-8',newline='')
print('R56s5g bootproof applied at app_main entry')
