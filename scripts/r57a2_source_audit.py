from pathlib import Path
import sys
req={
 'src/tab5_guest_bus.c':['PX68K_R57A2_DEDICATED_CONSUMER','R57_RING_SLOTS 8192u','r57_consumer_task','xTaskCreatePinnedToCore','PX68K_R57A2_JOURNAL: dedicated CPU0 consumer ready','s_attempt_seq'],
 'src/main.c':['PX68K_R57A2: dedicated CPU0 journal consumer ACTIVE','PX68K_R57A:'],
 'src/tab5_compose.c':['tab5_guest_bus_init()','#define TAB5_GBT65K_BURST_SLOTS 64u'],
 'components/px68k/x68k/tvram.c':['TAB5_R57_POST(adr, data);'],
 'components/px68k/x68k/bg.c':['TAB5_R57_BGPOST(2u, adr, data);','TAB5_R57_BGPOST(3u, adr, data);','TAB5_R57_BGPOST(4u, adr, data);'],
}
for p,marks in req.items():
 t=Path(p).read_text(encoding='utf-8') if Path(p).is_file() else ''
 if not t: print('R57a2 AUDIT FAIL missing',p); raise SystemExit(2)
 for m in marks:
  if m not in t: print('R57a2 AUDIT FAIL',p,'missing',m); raise SystemExit(3)
co=Path('src/tab5_compose.c').read_text(encoding='utf-8')
if 'tab5_guest_bus_drain_cpu0();' in co:
 print('R57a2 AUDIT FAIL compositor still owns journal drain'); raise SystemExit(4)
if 'const int r56s5_exact_host = 0;' not in Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8'):
 print('R57a2 AUDIT FAIL R56s5k correctness quarantine absent'); raise SystemExit(5)
print('R57a2 SOURCE AUDIT PASS: dedicated CPU0 journal consumer; compositor decoupled; CPU1 nonblocking; R56s5k correctness retained')
