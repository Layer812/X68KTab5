from pathlib import Path
req = {
    'src/tab5_guest_bus.c': ['PX68K_R57C_RASTER_TOKEN','TAB5_R57_DOMAIN_RASTER','void tab5_guest_bus_post_raster','lastRaster={seq=%lu y=%lu}'],
    'src/tab5_guest_bus.h': ['TAB5_R57_DOMAIN_RASTER','tab5_guest_bus_post_raster'],
    'components/px68k/libretro/windraw.c': ['PX68K_R57C_RASTER_TOKEN','tab5_guest_bus_post_raster(VLINE);','const int r56s5_exact_host = 0;'],
    'src/main.c': ['PX68K_R57C: ordered raster-token boundaries ACTIVE','PX68K_R57B:'],
    'components/px68k/x68k/bg.c': ['BG_HOST_SOURCE_BARRIER();'],
}
for p, marks in req.items():
    fp = Path(p)
    if not fp.is_file():
        print('R57c1 SOURCE AUDIT FAIL missing', p); raise SystemExit(2)
    t = fp.read_text(encoding='utf-8')
    for m in marks:
        if m not in t:
            print('R57c1 SOURCE AUDIT FAIL', p, 'missing', m); raise SystemExit(3)
print('R57C1 SOURCE AUDIT PASS: R57c token source intact; no source mutation performed')
