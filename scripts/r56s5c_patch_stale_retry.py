from pathlib import Path
import re

C=Path('src/tab5_compose.c')
M=Path('src/main.c')
MARK5='PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active'
MARK5C='PX68K_R56S5C: live validator stale-discard retry armed'
TAG='PX68K_R56S5C_VALIDATOR_STALE_RETRY'

def fail(msg):
    print('R56s5d patch ERROR:',msg)
    raise SystemExit(2)

if not C.exists() or not M.exists(): fail('missing src/tab5_compose.c or src/main.c')
cs=C.read_text(encoding='utf-8')
ms=M.read_text(encoding='utf-8')
if MARK5 not in ms: fail('R56s5 must be applied first')
if 's_r56s5_hostbt_state' not in cs: fail('R56s5 hostbt state missing')
if MARK5C in ms and TAG in cs:
    print('R56s5d already applied: robust stale validator retry present')
    raise SystemExit(0)
if MARK5C in ms or TAG in cs: fail('partial R56s5c/R56s5d state')

# Robust anchor: insert at the actual stale-discard accounting site, not at
# the surrounding if-condition text (which R56s5 may legitimately rewrite).
needle='++s_gbt65k_stale_dropped;'
pos=[m.start() for m in re.finditer(re.escape(needle),cs)]
if not pos:
    fail('stale counter anchor not found')
# Prefer the occurrence whose nearby preceding code references frame_epoch/latest_epoch.
candidates=[]
for p in pos:
    pre=cs[max(0,p-1200):p]
    if 'frame_epoch' in pre or 'latest_epoch' in pre:
        candidates.append(p)
if len(candidates)==1:
    p=candidates[0]
elif len(pos)==1:
    p=pos[0]
else:
    fail(f'ambiguous stale counter anchors total={len(pos)} frame-related={len(candidates)}')
line_start=cs.rfind('\n',0,p)+1
ind=re.match(r'[ \t]*',cs[line_start:p]).group(0)
block=(
    ind+'/* PX68K_R56S5C_VALIDATOR_STALE_RETRY:\n'
    +ind+' * If the one-shot live validator packet is retired as stale, re-arm\n'
    +ind+' * validation instead of leaving state=1 latched forever. The normal\n'
    +ind+' * stale retirement tail still owns all queue/pending cleanup. */\n'
    +ind+'if ((slot->selfcheck & (8u | 16u)) == (8u | 16u) &&\n'
    +ind+'    load_acquire(&s_r56s5_hostbt_state) == 1u)\n'
    +ind+'    store_release(&s_r56s5_hostbt_state, 0u);\n'
)
cs=cs[:line_start]+block+cs[line_start:]
mainline=f'    ESP_LOGI(TAG, "{MARK5}");\n'
if ms.count(mainline)!=1: fail(f'R56s5 main line count={ms.count(mainline)}')
ms=ms.replace(mainline,mainline+f'    ESP_LOGI(TAG, "{MARK5C}");\n',1)
C.write_text(cs,encoding='utf-8',newline='')
M.write_text(ms,encoding='utf-8',newline='')
print('R56s5d applied: stale validator retry inserted at stale-drop accounting site')
