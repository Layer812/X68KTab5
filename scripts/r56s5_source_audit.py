from pathlib import Path
import sys

W=Path('components/px68k/libretro/windraw.c')
C=Path('src/tab5_compose.c')
M=Path('src/main.c')
errs=[]
for p in (W,C,M):
    if not p.exists(): errs.append(f'missing {p}')
if errs:
    print('R56s5 AUDIT FAIL:', '; '.join(errs)); raise SystemExit(2)
ws=W.read_text(encoding='utf-8')
cs=C.read_text(encoding='utf-8')
ms=M.read_text(encoding='utf-8')

def req(cond,msg):
    if not cond: errs.append(msg)

req(ms.count('PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active')==1,'R56s5 main marker !=1')
req('PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active' in ms,'R56s4 lineage missing')
req('PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload' in ms,'R56s3 lineage missing')
req(ws.count('extern int tab5_compose_gbt65k_hostbt_state(void);')==1,'hostbt state extern missing/duplicate')
req('r56s5_exact_host = r56s3_text_visible && (r56s5_hostbt == 2)' in ws,'visible TEXT CPU0 re-entry gate missing')
req('r56s5_text_src, r56s5_text_valid, TextPal, r56s5_stp' in ws,'live validator source handoff missing')
req('int bg_on, int text_on, int exact_host_bt' in ws,'normal submit exact_host signature missing in windraw')
req(cs.count('int tab5_compose_gbt65k_hostbt_state(void)')==1,'hostbt state getter missing/duplicate')
req(cs.count('static const uint16_t *r56s5_render_host_bt_exact(')==2,'host exact helper prototype/definition count !=2')
req('CPU0 stock-order BG/TEXT live self-check PASS' in cs,'live PASS marker missing')
req('R56s4 exact fallback retained' in cs,'live FAIL fallback marker missing')
req('text_first = text_on && st && !st->gd' in cs,'stock text-first ordering missing')
req('flags[16u + i] = ti ? 1u : 0u;' in cs,'opaque Text_DrawLine flag semantics missing')
req('flags[16u + i] |= 1u;' in cs,'overlay Text_DrawLine flag semantics missing')
req('(exact_host_bt ? 16u : 0u)' in cs,'host exact packet bit missing')
req('r56s5_validate && bg_on' in cs,'validation BG source barrier missing')
req('_Static_assert(sizeof(compose_slot_t) <= 8544u' in cs,'8544-byte slot size guard missing')
req('PX68K_R56S5_HOSTBT_EXACT' in cs and 'PX68K_R56S5_HOSTBT_EXACT' in ws,'R56s5 source markers missing')
# Do not accidentally re-open retired/unsafe choices.
req('PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600' not in ms,'unsafe R56q marker resurrected')

if errs:
    print('R56s5 SOURCE AUDIT FAIL')
    for e in errs: print(' -',e)
    raise SystemExit(3)
print('R56s5 SOURCE AUDIT PASS: live A/B guarded exact CPU0 BG/TEXT path; R56s4 fallback retained; slot guard unchanged')
