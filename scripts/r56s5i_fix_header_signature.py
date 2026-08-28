from pathlib import Path
import re, sys
ROOT=Path('.')
H=ROOT/'src/tab5_compose.h'
C=ROOT/'src/tab5_compose.c'
W=ROOT/'components/px68k/libretro/windraw.c'
M=ROOT/'src/main.c'
for p in (H,C,W,M):
    if not p.is_file():
        print('R56s5i ERROR missing', p); raise SystemExit(2)

def function_decl_block(text, name):
    pos=text.find(name+'(')
    if pos<0: return None, None, None
    start=text.rfind('\n',0,pos)+1
    depth=0; seen=False
    for i in range(pos+len(name), len(text)):
        ch=text[i]
        if ch=='(':
            depth+=1; seen=True
        elif ch==')':
            depth-=1
            if seen and depth==0:
                semi=text.find(';',i)
                if semi<0: return None,None,None
                return start, semi+1, text[start:semi+1]
    return None,None,None

h=H.read_text(encoding='utf-8')
c=C.read_text(encoding='utf-8')
w=W.read_text(encoding='utf-8')
m=M.read_text(encoding='utf-8')
name='tab5_compose_submit_gbt65k_line'
start,end,block=function_decl_block(h,name)
if block is None:
    print('R56s5i ERROR header declaration not found'); raise SystemExit(3)
if 'int exact_host_bt' in block:
    print('R56s5i header signature already fixed')
else:
    pat=re.compile(r'(int\s+bg_on\s*,\s*int\s+text_on\s*,\s*\n)(\s*)(uint8_t\s+grp_pri)')
    newblock,n=pat.subn(r'\1\2int exact_host_bt,\n\2\3',block,count=1)
    if n!=1:
        print('R56s5i ERROR expected bg_on/text_on signature anchor not found exactly once')
        print(block)
        raise SystemExit(4)
    h=h[:start]+newblock+h[end:]
    H.write_text(h,encoding='utf-8',newline='\n')
    print('R56s5i patched src\\tab5_compose.h: added exact_host_bt prototype argument')

# Re-read and audit exact cross-TU contract.
h=H.read_text(encoding='utf-8')
_,_,hb=function_decl_block(h,name)
# definition signature ends at first ')' after function name; enough for exact token audit.
pos=c.find('int '+name+'(')
if pos<0:
    print('R56s5i ERROR C definition missing'); raise SystemExit(5)
end_sig=c.find(')\n{',pos)
if end_sig<0: end_sig=c.find(')\r\n{',pos)
if end_sig<0:
    print('R56s5i ERROR C definition signature end missing'); raise SystemExit(6)
cb=c[pos:end_sig+1]
checks=[
 ('header exact_host_bt', 'int exact_host_bt' in hb),
 ('definition exact_host_bt', 'int exact_host_bt' in cb),
 ('windraw passes exact flag', 'r56s5_exact_host, grp_pri, bg_pri, text_pri' in w),
 ('R56s5 marker', 'PX68K_R56S5:' in m),
 ('R56s5F marker', 'PX68K_R56S5F:' in m),
 ('BOOTPROOF marker', 'PX68K_R56S5G_BOOTPROOF:' in m),
]
for label,ok in checks: print(('PASS ' if ok else 'FAIL ')+label)
if not all(ok for _,ok in checks): raise SystemExit(7)
print('R56s5i SOURCE SIGNATURE AUDIT PASS: header/definition/caller exact_host_bt contract aligned')
