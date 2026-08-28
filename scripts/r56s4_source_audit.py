#!/usr/bin/env python3
from pathlib import Path
import sys
m=Path('src/main.c').read_text(encoding='utf-8')
w=Path('components/px68k/libretro/windraw.c').read_text(encoding='utf-8')
c=Path('src/tab5_compose.c').read_text(encoding='utf-8')
checks = [
 ('main_marker_once', m.count('PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active') == 1),
 ('s3_marker_retained', m.count('PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload') == 1),
 ('windraw_marker_once', w.count('PX68K_R56S4_EXACT_BT_65K_HANDOFF') == 1),
 ('compose_marker_once', c.count('PX68K_R56S4_GBT65K_EXACT_BT') == 1),
 ('exact_api_decl_once', w.count('extern int tab5_compose_submit_gbt65k_exact_bt_line(') == 1),
 ('exact_api_def_once', c.count('int tab5_compose_submit_gbt65k_exact_bt_line(') == 1),
 ('exact_submit_call_once', w.count('int accepted = tab5_compose_submit_gbt65k_exact_bt_line(') == 1),
 ('exact_mode_state_once', w.count('int r56s4_65k_exact_bt = 0;') == 1),
 ('visible_mode_set_once', w.count('r56s4_65k_exact_bt = 1;') == 1),
 ('cpu1_grp_skip_once', w.count('if (!r56s4_65k_exact_bt)\n                    WD_PERF_GRP(Grp_DrawLine16());') == 1),
 ('fallback_grp_once', w.count('++s_r56r_path[R56R_65_REJECT];\n        WD_PERF_GRP(Grp_DrawLine16());') == 1),
 ('authoritative_arrays_once', c.count('uint16_t exact_bg_text[TAB5_COMPOSE_MAX_WIDTH];') == 1 and c.count('uint8_t exact_flags[TAB5_COMPOSE_MAX_WIDTH];') == 1),
 ('exact_render_branch_once', c.count('if (slot->selfcheck & 8u) {') == 1),
 ('slot_size_guard_retained', c.count('_Static_assert(sizeof(compose_slot_t) <= 8544u,') == 1),
 ('burst_queue_retained', 'TAB5_GBT65K_BURST_SLOTS 128u' in c),
 ('r56_screen_ticket_retained', 'tab5_screen_render_result(render_ticket, slot->width, slot->dst)' in c),
 ('s3_simplified_fast_path_retained', 'if (!r56s3_text_visible && (!text_on || text_src) && (!bg_on || stp)) {' in w),
 ('zero_run_not_enabled', 'PX68K_MDX622: ZERO-RUN batch ACTIVE' not in m),
]
bad=[n for n,ok in checks if not ok]
for n,ok in checks: print(('PASS ' if ok else 'FAIL ')+n)
if bad:
    print('R56s4 source audit FAILED:', ', '.join(bad)); sys.exit(2)
print('R56s4 source audit PASS')
