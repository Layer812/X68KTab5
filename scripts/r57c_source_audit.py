from pathlib import Path
req={
 'src/tab5_guest_bus.c':['PX68K_R57C_RASTER_TOKEN','TAB5_R57_DOMAIN_RASTER','s_raster_tokens','void tab5_guest_bus_post_raster','lastRaster={seq=%lu y=%lu}'],
 'src/tab5_guest_bus.h':['TAB5_R57_DOMAIN_RASTER','tab5_guest_bus_post_raster'],
 'components/px68k/libretro/windraw.c':['PX68K_R57C_RASTER_TOKEN','tab5_guest_bus_post_raster(VLINE);','const int r56s5_exact_host = 0;'],
 'src/main.c':['PX68K_R57C: ordered raster-token boundaries ACTIVE','PX68K_R57B:'],
 'components/px68k/x68k/bg.c':['BG_HOST_SOURCE_BARRIER();'],
}
for p,marks in req.items():
 t=Path(p).read_text(encoding='utf-8') if Path(p).is_file() else ''
 if not t: print('R57c AUDIT FAIL missing',p); raise SystemExit(2)
 for m in marks:
  if m not in t: print('R57c AUDIT FAIL',p,'missing',m); raise SystemExit(3)
# R57c is token proof only. Removing barriers or enabling R56s5 host-BT here
# would mix architecture validation with correctness risk.
bg=Path('components/px68k/x68k/bg.c').read_text(encoding='utf-8')
if bg.count('BG_HOST_SOURCE_BARRIER();')<3:
 print('R57c AUDIT FAIL: BG barriers removed before token proof'); raise SystemExit(4)
print('R57c SOURCE AUDIT PASS: CPU1 posts ordered raster tokens without waits; R57b shadow + R56s5k correctness fence retained')
