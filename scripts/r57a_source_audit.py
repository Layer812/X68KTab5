from pathlib import Path
import sys
files={p:Path(p).read_text(encoding='utf-8') for p in [
 'src/CMakeLists.txt','src/main.c','src/tab5_compose.c','src/tab5_guest_bus.c','src/tab5_guest_bus.h',
 'components/px68k/x68k/tvram.c','components/px68k/x68k/bg.c'] if Path(p).is_file()}
need={
 'src/CMakeLists.txt':['tab5_guest_bus.c'],
 'src/main.c':['PX68K_R57A: CPU1 guest/render posted-write journal ACTIVE','PX68K_R56S5K:'],
 'src/tab5_compose.c':['PX68K_R57A_GUEST_RENDER_JOURNAL','#define TAB5_GBT65K_BURST_SLOTS 64u','tab5_guest_bus_drain_cpu0();','tab5_compose_guest_event_kick'],
 'src/tab5_guest_bus.c':['PX68K_R57A_JOURNAL: CPU1->CPU0 posted-write SPSC ready','R57_RING_SLOTS 4096u'],
 'components/px68k/x68k/tvram.c':['TAB5_R57_POST(adr, data);'],
 'components/px68k/x68k/bg.c':['TAB5_R57_BGPOST(2u, adr, data);','TAB5_R57_BGPOST(3u, adr, data);','TAB5_R57_BGPOST(4u, adr, data);'],
}
for p,marks in need.items():
    if p not in files: print('R57a AUDIT FAIL missing',p); raise SystemExit(2)
    for m in marks:
        if m not in files[p]: print('R57a AUDIT FAIL',p,'missing',m); raise SystemExit(3)
if files['components/px68k/x68k/tvram.c'].count('TAB5_R57_POST(adr, data);')!=2:
    print('R57a AUDIT FAIL TVRAM hook count'); raise SystemExit(4)
if 'const int r56s5_exact_host = 0;' not in Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8'):
    print('R57a AUDIT FAIL R56s5k visible-TEXT quarantine not present'); raise SystemExit(5)
print('R57a SOURCE AUDIT PASS: R56s5k correctness retained; CPU1->CPU0 render journal armed; burst queue 64')
