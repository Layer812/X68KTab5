#!/usr/bin/env python3
from pathlib import Path
import sys

main = Path('src/main.c').read_text(encoding='utf-8')
wd = Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8')
comp = Path('src/tab5_compose.c').read_text(encoding='utf-8')
scr = Path('src/tab5_screen_manager.c').read_text(encoding='utf-8')

checks = [
    ('r56s_marker', 'PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored' in main),
    ('r56r_retained', 'PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only' in main and 'R56R_PATH f=' in main),
    ('handoff_marker_once', wd.count('PX68K_R56S_65K_BG_HANDOFF') == 1),
    ('old_bg_disable_absent', 'if (!bg_on && (!text_on || text_src))' not in wd),
    ('proven_gate_once', wd.count('if ((!text_on || text_src) && (!bg_on || stp))') == 1),
    ('bg_packet_call_once', wd.count('TextPal, stp, bg_on, text_on') == 1),
    ('ticket_bound', '&RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket' in wd),
    ('compose_bg_snapshot', 'BG_HOST_LINE_STATE bg_state;' in comp and 'memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));' in comp),
    ('compose_raw65_snapshot', 'uint16_t raw_row[512];' in comp and 'capture the complete 65K source row at the guest-time latch' in comp),
    ('compose_bg_barrier', '__atomic_add_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);' in comp),
    ('compose_ticket', 's_gbt65k_burst_render_ticket[idx] = render_ticket;' in comp),
    ('bg_selfcheck', 'BG/Sprite host self-check PASS' in comp and 's_bgsp_selfcheck_state' in comp),
    ('screen_stdio_fence', 'PX68K_SCREEN_R56K6: worker stdio=FORBIDDEN' in scr),
    ('unsafe_r56q_absent', 'PX68K_R56Q: sparse MDXPERF/RENDER/CPU0COST visibility cadence=600' not in main),
]
failed = False
for name, ok in checks:
    print(('PASS' if ok else 'FAIL'), name)
    failed |= not ok
sys.exit(1 if failed else 0)
