from pathlib import Path
req={
 'src/tab5_guest_bus.c':['PX68K_R57B_BG_SHADOW','R57_SHADOW_BYTES','shadow_apply','PX68K_R57B_SHADOW: CPU0-owned BG/Sprite mirror ready','tab5_guest_bus_bg_shadow','tab5_guest_bus_shadow_seq'],
 'src/tab5_guest_bus.h':['TAB5_R57_DOMAIN_BG_RESET','tab5_guest_bus_bgchr8_shadow','tab5_guest_bus_bgchr16_shadow'],
 'src/main.c':['PX68K_R57B: CPU0 BG/Sprite render-shadow mirror ACTIVE','PX68K_R57A2:'],
 'components/px68k/x68k/bg.c':['TAB5_R57_BGPOST(5u, 0u, 0u);','BG_HOST_SOURCE_BARRIER();'],
 'components/px68k/libretro/windraw.c':['const int r56s5_exact_host = 0;'],
}
for p,marks in req.items():
 t=Path(p).read_text(encoding='utf-8') if Path(p).is_file() else ''
 if not t: print('R57b AUDIT FAIL missing',p); raise SystemExit(2)
 for m in marks:
  if m not in t: print('R57b AUDIT FAIL',p,'missing',m); raise SystemExit(3)
# Deliberately require the old barrier and correctness fence to remain in R57b.
bg=Path('components/px68k/x68k/bg.c').read_text(encoding='utf-8')
if bg.count('BG_HOST_SOURCE_BARRIER();')<3:
 print('R57b AUDIT FAIL: production BG barriers were removed too early'); raise SystemExit(4)
print('R57b SOURCE AUDIT PASS: lossless R57a2 bus + CPU0 BG/Sprite shadow mirror; production renderer/barriers still correctness-frozen')
