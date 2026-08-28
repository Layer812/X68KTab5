from pathlib import Path
import re

def fail(msg):
    print('R57d SOURCE AUDIT FAIL:', msg)
    raise SystemExit(2)

req={
 'src/tab5_guest_bus.c':[
  'PX68K_R57D_ORDERED_SHADOW_HOLD','R57_HOLD_SLOTS 256u',
  'tab5_guest_bus_post_raster_hold','tab5_guest_bus_shadow_hold_wait',
  's_shadow_invalid','PX68K_R57D_SHADOW:'
 ],
 'src/tab5_guest_bus.h':[
  'tab5_guest_bus_post_raster_hold','tab5_guest_bus_shadow_hold_cancel',
  'tab5_guest_bus_bg_shadow_ready'
 ],
 'src/tab5_compose.c':[
  'PX68K_R57D_CLASS_VALIDATE','R57D_CLASS_PASS_NEED 8u',
  'tab5_compose_r57d_class_state','r57d_bg_src','r57d_c16_src',
  'r57d_shadow_seq','PX68K_R57D3_HOLD_BIND','!(slot->selfcheck & 32u)',
  'const int burst65k = s_gbt65k_burst_ready;'
 ],
 'src/tab5_compose.h':['r57d_shadow_seq'],
 'components/px68k/libretro/windraw.c':[
  'PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE','tab5_guest_bus_post_raster_hold',
  'tab5_compose_r57d_class_state','r57d_class_state==2',
  'r57d_class, r57d_shadow_seq'
 ],
 'src/main.c':[
  'PX68K_R57C: ordered raster-token boundaries ACTIVE',
  'PX68K_R57D: class-certified visible TEXT + ordered CPU0 BG/Sprite shadow ownership ACTIVE; CPU1 never waits'
 ],
}
texts={}
for name,marks in req.items():
    p=Path(name)
    if not p.is_file(): fail('missing '+name)
    t=p.read_text(encoding='utf-8'); texts[name]=t
    for m in marks:
        if m not in t: fail(f'{name} missing {m}')

c=texts['src/tab5_compose.c']
w=texts['components/px68k/libretro/windraw.c']
b=texts['src/tab5_guest_bus.c']
h=texts['src/tab5_compose.h']
# R57d1 ABI contract: normal 65K submit is public; exact-BT submit remains
# private to compose.c with a matching local extern in windraw.c, exactly as
# introduced by R56s5.  Do not require or synthesize a header declaration.
if 'tab5_compose_submit_gbt65k_exact_bt_line' in h:
    fail('private exact-BT submit unexpectedly exported from tab5_compose.h')
for blob,label in ((c,'compose definition'),(w,'windraw extern')):
    i=blob.find('tab5_compose_submit_gbt65k_exact_bt_line')
    if i<0: fail('missing exact-BT '+label)
    sig=blob[i:blob.find(')',i)+1]
    if 'r57d_class' not in sig or 'r57d_shadow_seq' not in sig:
        fail('R57d exact-BT arguments missing from '+label)
# Unsafe one-global-line re-entry must remain superseded.
if 'const int r56s5_exact_host = 0; /* correctness quarantine' in w:
    fail('R56s5k forced-off gate still active; R57d class gate did not replace it')
# R57d must not remove the correctness lineage or old safe fallbacks.
for m in ('r56s4_render_gbt65k_exact_bt','r56s5_render_host_bt_exact'):
    if m not in c: fail('fallback/helper missing '+m)
# Hot shadow-owned normal jobs skip shared-source pending barrier bookkeeping.
# It is okay for legacy/non-shadow modes and exact failed classes to keep it.
# Confirm the transformed normal function contains the hold condition.
if 'if (bg_on && !r57d_shadow_seq)' not in c:
    fail('normal 65K shared BG pending is not shadow-aware')
# Every hold has a cancel path on queue rejection and a CPU0 release path.
if w.count('tab5_guest_bus_shadow_hold_cancel') < 2:
    fail('windraw rejection cancellation paths incomplete')
if 'tab5_guest_bus_shadow_hold_release(r57d_hold_seq)' not in c:
    fail('CPU0 hold release missing')
# Validation and production shadow packets must share FIFO to avoid hold-order deadlock.
if 's_gbt65k_burst_ready && !r57d_validate' in c:
    fail('validator still diverts to ordinary queue; hold order can deadlock')
# No CPU1-side wait API is referenced by WinDraw.
for banned in ('tab5_guest_bus_shadow_hold_wait(', 'tab5_compose_wait_idle()'):
    if banned in w: fail('CPU1 wait survived in WinDraw: '+banned)
# A dropped journal event invalidates shadow use for the boot.
if 'st_rel(&s_shadow_invalid,1u)' not in b:
    fail('lossless overflow fail-safe missing')
print('R57d3 SOURCE AUDIT PASS: 65K BG/Sprite ownership uses ordered CPU0 shadow; per-class visible TEXT certification active; CPU1 has no shadow wait')
