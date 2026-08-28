from pathlib import Path
import sys

ROOT = Path('.')
W = ROOT / 'components/px68k/libretro/windraw.c'
C = ROOT / 'src/tab5_compose.c'
M = ROOT / 'src/main.c'
MARK = 'PX68K_R56S5: live-validated CPU0 exact BG/TEXT + cached 65K path active'


def fail(msg):
    print('R56s5 patch ERROR:', msg)
    raise SystemExit(2)

def replace_once(s, old, new, label):
    n = s.count(old)
    if n != 1:
        fail(f'{label}: expected exactly 1 anchor, found {n}')
    return s.replace(old, new, 1)

for p in (W,C,M):
    if not p.exists(): fail(f'missing {p}')

ws = W.read_text(encoding='utf-8')
cs = C.read_text(encoding='utf-8')
ms = M.read_text(encoding='utf-8')

if MARK in ms and 'PX68K_R56S5_HOSTBT_EXACT' in cs and 'tab5_compose_gbt65k_hostbt_state' in ws:
    print('R56s5 already applied and verified:', W, C, M)
    raise SystemExit(0)

if 'PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active' not in ms:
    fail('R56s4 lineage marker missing in src/main.c')
if 'PX68K_R56S4_EXACT_BT_65K_HANDOFF' not in ws:
    fail('R56s4 windraw anchor missing')
if 'PX68K_R56S4_GBT65K_EXACT_BT' not in cs:
    fail('R56s4 compose anchor missing')

# ---- windraw declarations ----
old = '''extern int tab5_compose_gbt65k_line_admit(uint32_t y, uint32_t height);\nextern int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,\n                                                    const uint8_t *gvram,\n                                                    uint32_t gvram_row, uint32_t gvram_x,\n                                                    const uint8_t *pal_regs,\n                                                    uint32_t pal_generation, uint8_t contrast,\n                                                    const uint16_t *bg_text,\n                                                    const uint8_t *text_tr_flags,\n                                                    uint8_t grp_pri, uint8_t bg_pri,\n                                                    uint8_t text_pri, uint16_t *dst,\n                                                    uint64_t render_ticket);\nextern int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                           const uint8_t *gvram,\n                                           uint32_t gvram_row, uint32_t gvram_x,\n                                           const uint8_t *pal_regs,\n                                           uint32_t pal_generation, uint8_t contrast,\n                                           const uint8_t *text_src, uint32_t text_valid,\n                                           const uint16_t *text_palette,\n                                           const BG_HOST_LINE_STATE *bg_state,\n                                           int bg_on, int text_on,\n                                           uint8_t grp_pri, uint8_t bg_pri,\n                                           uint8_t text_pri, uint16_t *dst,\n                                           uint64_t render_ticket);'''
new = '''extern int tab5_compose_gbt65k_line_admit(uint32_t y, uint32_t height);\nextern int tab5_compose_gbt65k_hostbt_state(void);\nextern int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,\n                                                    const uint8_t *gvram,\n                                                    uint32_t gvram_row, uint32_t gvram_x,\n                                                    const uint8_t *pal_regs,\n                                                    uint32_t pal_generation, uint8_t contrast,\n                                                    const uint16_t *bg_text,\n                                                    const uint8_t *text_tr_flags,\n                                                    const uint8_t *text_src, uint32_t text_valid,\n                                                    const uint16_t *text_palette,\n                                                    const BG_HOST_LINE_STATE *bg_state,\n                                                    int bg_on, int text_on,\n                                                    uint8_t grp_pri, uint8_t bg_pri,\n                                                    uint8_t text_pri, uint16_t *dst,\n                                                    uint64_t render_ticket);\nextern int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                           const uint8_t *gvram,\n                                           uint32_t gvram_row, uint32_t gvram_x,\n                                           const uint8_t *pal_regs,\n                                           uint32_t pal_generation, uint8_t contrast,\n                                           const uint8_t *text_src, uint32_t text_valid,\n                                           const uint16_t *text_palette,\n                                           const BG_HOST_LINE_STATE *bg_state,\n                                           int bg_on, int text_on, int exact_host_bt,\n                                           uint8_t grp_pri, uint8_t bg_pri,\n                                           uint8_t text_pri, uint16_t *dst,\n                                           uint64_t render_ticket);'''
ws = replace_once(ws, old, new, 'windraw extern signatures')

# Locals retain first validation line sources until stock BG/TEXT is materialized later.
old = '''    uint64_t render_ticket = 0u;\n    int r56s4_65k_exact_bt = 0;'''
new = '''    uint64_t render_ticket = 0u;\n    int r56s4_65k_exact_bt = 0;\n    /* PX68K_R56S5_HOSTBT_EXACT\n     * Keep source snapshots for the one-shot live A/B against R56s4 stock\n     * BG_LineBuf/Text_TrFlag. Once CPU0 proves exact, visible-TEXT rows return\n     * to the early asynchronous path without running stock BG/TEXT on CPU1. */\n    const uint8_t *r56s5_text_src = NULL;\n    uint32_t r56s5_text_valid = 0u;\n    BG_HOST_LINE_STATE r56s5_bg_state;\n    int r56s5_have_bg_state = 0;\n    int r56s5_text_on = 0, r56s5_bg_on = 0;'''
ws = replace_once(ws, old, new, 'windraw R56s5 locals')

# Replace visible-text gating + normal submit block. Preserve safe stock fallback until live compare passes.
old = '''            if (r56s3_text_visible)\n                r56s4_65k_exact_bt = 1;\n\n            BG_HOST_LINE_STATE st;'''
new = '''            const int r56s5_hostbt = tab5_compose_gbt65k_hostbt_state();\n            if (r56s3_text_visible && r56s5_hostbt != 2) {\n                /* State 0/1 = validation not completed yet; state 3 = mismatch.\n                 * Keep the proven R56s4 stock CPU1 BG/TEXT path in either case. */\n                r56s4_65k_exact_bt = 1;\n                r56s5_text_src = text_src;\n                r56s5_text_valid = text_valid;\n                r56s5_text_on = text_on;\n                r56s5_bg_on = bg_on;\n            }\n\n            BG_HOST_LINE_STATE st;'''
ws = replace_once(ws, old, new, 'windraw visible text state gate')

old = '''                const int gd = (bg_pri < text_pri) ? 0 : 1;\n                if (BG_CaptureHostLineState(&st, vbg, gd)) stp = &st;\n            }\n            /* PX68K_R56S_65K_BG_HANDOFF'''
new = '''                const int gd = (bg_pri < text_pri) ? 0 : 1;\n                if (BG_CaptureHostLineState(&st, vbg, gd)) stp = &st;\n            }\n            if (r56s4_65k_exact_bt && stp) {\n                r56s5_bg_state = *stp;\n                r56s5_have_bg_state = 1;\n            }\n            /* PX68K_R56S_65K_BG_HANDOFF'''
ws = replace_once(ws, old, new, 'windraw capture validation bg state')

old = '''            if (!r56s3_text_visible && (!text_on || text_src) && (!bg_on || stp)) {\n                int accepted = tab5_compose_submit_gbt65k_line(\n                    VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],\n                    Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),\n                    text_src, text_valid, TextPal, stp, bg_on, text_on,\n                    grp_pri, bg_pri, text_pri,\n                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
new = '''            /* R56s5: after one live stock-vs-host BG/TEXT comparison passes,\n             * visible-TEXT rows re-enter CPU0. The packet already contains exact\n             * expanded text indices plus the raster-correct BG_HOST_LINE_STATE. */\n            const int r56s5_exact_host = r56s3_text_visible && (r56s5_hostbt == 2);\n            if ((!r56s3_text_visible || r56s5_exact_host) &&\n                (!text_on || text_src) && (!bg_on || stp)) {\n                int accepted = tab5_compose_submit_gbt65k_line(\n                    VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],\n                    Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),\n                    text_src, text_valid, TextPal, stp, bg_on, text_on,\n                    r56s5_exact_host, grp_pri, bg_pri, text_pri,\n                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
ws = replace_once(ws, old, new, 'windraw R56s5 early submit')

# Late R56s4 exact submit: add source snapshot only for first live validator.
old = '''        int accepted = tab5_compose_submit_gbt65k_exact_bt_line(\n            VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],\n            Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),\n            &BG_LineBuf[16], &Text_TrFlag[16],\n            grp_pri, bg_pri, text_pri,\n            &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
new = '''        const BG_HOST_LINE_STATE *r56s5_stp =\n            (r56s5_bg_on && r56s5_have_bg_state) ? &r56s5_bg_state : NULL;\n        int accepted = tab5_compose_submit_gbt65k_exact_bt_line(\n            VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],\n            Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),\n            &BG_LineBuf[16], &Text_TrFlag[16],\n            r56s5_text_src, r56s5_text_valid, TextPal, r56s5_stp,\n            r56s5_bg_on, r56s5_text_on, grp_pri, bg_pri, text_pri,\n            &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
ws = replace_once(ws, old, new, 'windraw validator exact submit')

# ---- compose globals ----
old = '''static volatile uint32_t s_gbt65k_pie_lines;\nstatic volatile uint32_t s_gbt65k_scalar_lines;\nstatic volatile uint32_t s_gbt65k_frame_epoch = 1u;'''
new = '''static volatile uint32_t s_gbt65k_pie_lines;\nstatic volatile uint32_t s_gbt65k_scalar_lines;\n/* PX68K_R56S5_HOSTBT_EXACT: 0=untested 1=live A/B pending 2=exact 3=failed. */\nstatic volatile uint32_t s_r56s5_hostbt_state;\nstatic volatile uint32_t s_gbt65k_frame_epoch = 1u;'''
cs = replace_once(cs, old, new, 'compose hostbt state')

# Forward declaration: render_gbt65k_line appears before the full host BG renderer body.
old = '''static const uint16_t *render_host_bgsp(const BG_HOST_LINE_STATE *st,\n                                        const uint16_t *text_pal,\n                                        uint32_t width);'''
new = old + '''\nstatic const uint16_t *r56s5_render_host_bt_exact(const BG_HOST_LINE_STATE *st,\n                                                   const uint8_t *text_idx,\n                                                   const uint16_t *text_pal,\n                                                   uint32_t width, int text_on,\n                                                   const uint8_t **flags_out);'''
cs = replace_once(cs, old, new, 'compose R56s5 helper forward declaration')

# Add exact CPU0 BG/TEXT merge helper immediately after render_host_bgsp.
anchor = '''static const uint16_t *render_host_bgsp(const BG_HOST_LINE_STATE *st,\n                                        const uint16_t *text_pal,\n                                        uint32_t width)\n{\n    uint16_t *line = s_bg_line_scratch;\n    uint16_t *pri = s_bg_pri_scratch;\n    uint8_t *flags = s_bg_flag_scratch;\n    if (!line || !pri || !flags)\n        return NULL;\n\n    memset(flags, 0, TAB5_BG_SCRATCH_PIXELS * sizeof(flags[0]));\n    memset(pri, 0xff, TAB5_BG_SCRATCH_PIXELS * sizeof(pri[0]));\n    if (text_pal[0] == 0u)\n        memset(&line[16], 0, (size_t)width * sizeof(line[0]));\n    else\n        for (uint32_t i = 0; i < width; ++i) line[16u + i] = text_pal[0];\n\n    host_sprite_priority(st, 0u, line, pri, flags, text_pal, width);\n    if ((st->reg9 & 8u) && st->chr_size == 8u)\n        host_bg_plane8(st->bg1_top, st->bg1_scroll_x, st->bg1_scroll_y,\n                       st, st->gd ? 0 : 1, line, flags, text_pal, width);\n    host_sprite_priority(st, 1u, line, pri, flags, text_pal, width);\n    if (st->reg9 & 1u) {\n        if (st->chr_size == 8u)\n            host_bg_plane8(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,\n                           st, st->gd ? 0 : 1, line, flags, text_pal, width);\n        else\n            host_bg_plane16(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,\n                            st, st->gd ? 0 : 1, line, flags, text_pal, width);\n    }\n    host_sprite_priority(st, 2u, line, pri, flags, text_pal, width);\n    return &line[16];\n}\n'''
insert = anchor + '''\n/* R56s5: reproduce the stock common BG/TEXT construction order on CPU0.\n * The fast-path admission already guarantees TextPal[0]==0 and no text wrap.\n * gd=0 is the stock \"TEXT opaque, then higher-priority BG/Sprite overlay\" case;\n * gd=1 is \"BG/Sprite opaque, then TEXT key-zero overlay\".  This preserves\n * BG_LineBuf + Text_TrFlag semantics instead of treating TEXT as an independent\n * global-priority layer (the R56s bug exposed by filenames in X68000~1.HDS). */\nstatic const uint16_t *r56s5_render_host_bt_exact(const BG_HOST_LINE_STATE *st,\n                                                   const uint8_t *text_idx,\n                                                   const uint16_t *text_pal,\n                                                   uint32_t width, int text_on,\n                                                   const uint8_t **flags_out)\n{\n    uint16_t *line = s_bg_line_scratch;\n    uint16_t *pri = s_bg_pri_scratch;\n    uint8_t *flags = s_bg_flag_scratch;\n    if (!line || !pri || !flags || !text_pal || width > TAB5_COMPOSE_MAX_WIDTH)\n        return NULL;\n\n    memset(flags, 0, TAB5_BG_SCRATCH_PIXELS * sizeof(flags[0]));\n    memset(pri, 0xff, TAB5_BG_SCRATCH_PIXELS * sizeof(pri[0]));\n    memset(&line[16], 0, (size_t)width * sizeof(line[0]));\n\n    const int text_first = text_on && st && !st->gd;\n    if (text_first) {\n        for (uint32_t i = 0; i < width; ++i) {\n            const uint8_t ti = (uint8_t)(text_idx[i] & 0x0fu);\n            line[16u + i] = text_pal[ti];\n            flags[16u + i] = ti ? 1u : 0u;\n        }\n    }\n\n    if (st) {\n        host_sprite_priority(st, 0u, line, pri, flags, text_pal, width);\n        if ((st->reg9 & 8u) && st->chr_size == 8u)\n            host_bg_plane8(st->bg1_top, st->bg1_scroll_x, st->bg1_scroll_y,\n                           st, st->gd ? 0 : 1, line, flags, text_pal, width);\n        host_sprite_priority(st, 1u, line, pri, flags, text_pal, width);\n        if (st->reg9 & 1u) {\n            if (st->chr_size == 8u)\n                host_bg_plane8(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,\n                               st, st->gd ? 0 : 1, line, flags, text_pal, width);\n            else\n                host_bg_plane16(st->bg0_top, st->bg0_scroll_x, st->bg0_scroll_y,\n                                st, st->gd ? 0 : 1, line, flags, text_pal, width);\n        }\n        host_sprite_priority(st, 2u, line, pri, flags, text_pal, width);\n    }\n\n    if (text_on && !text_first) {\n        for (uint32_t i = 0; i < width; ++i) {\n            const uint8_t ti = (uint8_t)(text_idx[i] & 0x0fu);\n            if (ti) {\n                line[16u + i] = text_pal[ti];\n                flags[16u + i] |= 1u;\n            }\n        }\n    }\n\n    if (flags_out) *flags_out = &flags[16];\n    return &line[16];\n}\n'''
cs = replace_once(cs, anchor, insert, 'compose exact host BT helper')

# Render: first live A/B on bit3+bit4, future CPU0 exact host path on bit4 only.
old = '''    /* R56s4 bit3: CPU1 already produced authoritative stock BG/TEXT raster.\n     * Only cached 65K GRP decode + the validated 6.14a final selector run here. */\n    if (slot->selfcheck & 8u) {\n        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                     slot->u.gbt65k.gvram_x & 511u,\n                                     slot->u.gbt65k.exact_bg_text,\n                                     slot->u.gbt65k.exact_flags,\n                                     slot->width, slot->grp_pri,\n                                     slot->bg_pri, slot->text_pri);\n        ++s_gbt65k_scalar_lines;\n        return 1;\n    }\n\n    const int text_on = (slot->selfcheck & 1u) != 0u;\n    const int bg_on = (slot->selfcheck & 2u) != 0u;'''
new = '''    /* R56s5 bit4 means the packet carries expanded TEXT + BG host state and\n     * asks CPU0 to construct the exact stock combined BG/TEXT raster.\n     * bit3+bit4 is the one-shot live validator: current output remains the\n     * authoritative R56s4 CPU1 snapshot while the host result is compared. */\n    if (slot->selfcheck & 16u) {\n        const int text_on5 = (slot->selfcheck & 1u) != 0u;\n        const int bg_on5 = (slot->selfcheck & 2u) != 0u;\n        const uint8_t *host_flags = NULL;\n        const uint16_t *host_bt = r56s5_render_host_bt_exact(\n            bg_on5 ? &slot->u.gbt65k.bg_state : NULL,\n            slot->u.gbt65k.text_idx, slot->u.gbt65k.text_pal,\n            slot->width, text_on5, &host_flags);\n        if (!host_bt || !host_flags) {\n            if (slot->selfcheck & 8u) {\n                store_release(&s_r56s5_hostbt_state, 3u);\n                r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                             slot->u.gbt65k.gvram_x & 511u,\n                                             slot->u.gbt65k.exact_bg_text,\n                                             slot->u.gbt65k.exact_flags,\n                                             slot->width, slot->grp_pri,\n                                             slot->bg_pri, slot->text_pri);\n                ++s_gbt65k_scalar_lines;\n                return 1;\n            }\n            return 0;\n        }\n\n        if (slot->selfcheck & 8u) {\n            uint32_t bad = slot->width;\n            for (uint32_t i = 0; i < slot->width; ++i) {\n                if (host_bt[i] != slot->u.gbt65k.exact_bg_text[i] ||\n                    (host_flags[i] & 3u) != (slot->u.gbt65k.exact_flags[i] & 3u)) {\n                    bad = i; break;\n                }\n            }\n            if (bad == slot->width) {\n                store_release(&s_r56s5_hostbt_state, 2u);\n                printf("PX68K_R56S5: CPU0 stock-order BG/TEXT live self-check PASS; visible TEXT 65K rows fully host-offloaded\\n");\n            } else {\n                store_release(&s_r56s5_hostbt_state, 3u);\n                printf("PX68K_R56S5: CPU0 stock-order BG/TEXT live self-check FAIL x=%lu host=%04X/%u stock=%04X/%u; R56s4 exact fallback retained\\n",\n                       (unsigned long)bad, host_bt[bad],\n                       (unsigned)(host_flags[bad] & 3u),\n                       slot->u.gbt65k.exact_bg_text[bad],\n                       (unsigned)(slot->u.gbt65k.exact_flags[bad] & 3u));\n            }\n            r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                         slot->u.gbt65k.gvram_x & 511u,\n                                         slot->u.gbt65k.exact_bg_text,\n                                         slot->u.gbt65k.exact_flags,\n                                         slot->width, slot->grp_pri,\n                                         slot->bg_pri, slot->text_pri);\n            ++s_gbt65k_scalar_lines;\n            return 1;\n        }\n\n        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                     slot->u.gbt65k.gvram_x & 511u,\n                                     host_bt, host_flags, slot->width,\n                                     slot->grp_pri, slot->bg_pri, slot->text_pri);\n        ++s_gbt65k_scalar_lines;\n        return 1;\n    }\n\n    /* R56s4 bit3 without bit4: CPU1 exact snapshot fallback after validation\n     * failure or while another validation packet is pending. */\n    if (slot->selfcheck & 8u) {\n        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                     slot->u.gbt65k.gvram_x & 511u,\n                                     slot->u.gbt65k.exact_bg_text,\n                                     slot->u.gbt65k.exact_flags,\n                                     slot->width, slot->grp_pri,\n                                     slot->bg_pri, slot->text_pri);\n        ++s_gbt65k_scalar_lines;\n        return 1;\n    }\n\n    const int text_on = (slot->selfcheck & 1u) != 0u;\n    const int bg_on = (slot->selfcheck & 2u) != 0u;'''
cs = replace_once(cs, old, new, 'compose R56s5 render branch')

# Getter + normal submit exact_host arg.
old = '''int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                    const uint8_t *gvram,'''
new = '''int tab5_compose_gbt65k_hostbt_state(void)\n{\n    return (int)load_acquire(&s_r56s5_hostbt_state);\n}\n\nint tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                    const uint8_t *gvram,'''
cs = replace_once(cs, old, new, 'compose state getter')

old = '''                                    const uint16_t *text_palette,\n                                    const BG_HOST_LINE_STATE *bg_state,\n                                    int bg_on, int text_on,\n                                    uint8_t grp_pri, uint8_t bg_pri,'''
new = '''                                    const uint16_t *text_palette,\n                                    const BG_HOST_LINE_STATE *bg_state,\n                                    int bg_on, int text_on, int exact_host_bt,\n                                    uint8_t grp_pri, uint8_t bg_pri,'''
cs = replace_once(cs, old, new, 'compose normal submit signature')

old = '''    slot->selfcheck = (uint8_t)((text_on ? 1u : 0u) | (bg_on ? 2u : 0u));'''
new = '''    slot->selfcheck = (uint8_t)((text_on ? 1u : 0u) |\n                                (bg_on ? 2u : 0u) |\n                                (exact_host_bt ? 16u : 0u));'''
cs = replace_once(cs, old, new, 'compose normal submit bits')

old = '''    if (bg_on) {\n        memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));\n        if (load_acquire(&s_bgsp_selfcheck_state) == 0u && s_bgsp_ref_scratch) {'''
new = '''    if (bg_on) {\n        memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));\n        if (!exact_host_bt && load_acquire(&s_bgsp_selfcheck_state) == 0u && s_bgsp_ref_scratch) {'''
cs = replace_once(cs, old, new, 'compose avoid BG-only selfcheck on exact-host packet')

# Exact submit signature extended with sources.
old = '''                                             const uint16_t *bg_text,\n                                             const uint8_t *text_tr_flags,\n                                             uint8_t grp_pri, uint8_t bg_pri,'''
new = '''                                             const uint16_t *bg_text,\n                                             const uint8_t *text_tr_flags,\n                                             const uint8_t *text_src, uint32_t text_valid,\n                                             const uint16_t *text_palette,\n                                             const BG_HOST_LINE_STATE *bg_state,\n                                             int bg_on, int text_on,\n                                             uint8_t grp_pri, uint8_t bg_pri,'''
cs = replace_once(cs, old, new, 'compose exact submit signature')

old = '''    if (!s_ready || !s_gbt65k_ready || !gvram || !pal_regs ||\n        !bg_text || !text_tr_flags || !dst ||\n        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH) {'''
new = '''    if (!s_ready || !s_gbt65k_ready || !gvram || !pal_regs ||\n        !bg_text || !text_tr_flags || !dst ||\n        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH ||\n        (text_on && (!text_src || !text_palette || text_valid > width)) ||\n        (bg_on && !bg_state)) {'''
cs = replace_once(cs, old, new, 'compose exact validate inputs')

# Mark first validator before queue publication; rollback on publication failure.
old = '''    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();\n    slot->type = COMPOSE_JOB_GBT65K;\n    slot->bottom_page = 0u;\n    slot->top_page = 0u;\n    slot->bg_on_top = 0u;\n    slot->selfcheck = 8u; /* authoritative BG/TEXT snapshot; no BG source barrier */'''
new = '''    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();\n    const int r56s5_validate =\n        (load_acquire(&s_r56s5_hostbt_state) == 0u) && text_on && text_src &&\n        (!bg_on || bg_state);\n    if (r56s5_validate) store_release(&s_r56s5_hostbt_state, 1u);\n    slot->type = COMPOSE_JOB_GBT65K;\n    slot->bottom_page = 0u;\n    slot->top_page = 0u;\n    slot->bg_on_top = 0u;\n    slot->selfcheck = (uint8_t)(8u | (r56s5_validate ? 16u : 0u) |\n                                (r56s5_validate && text_on ? 1u : 0u) |\n                                (r56s5_validate && bg_on ? 2u : 0u));'''
cs = replace_once(cs, old, new, 'compose validation state/flags')

old = '''    tab5_raster590_copy_run(slot->u.gbt65k.exact_flags,\n                            text_tr_flags, width);\n\n    s_last_gbt65k_build_us ='''
new = '''    tab5_raster590_copy_run(slot->u.gbt65k.exact_flags,\n                            text_tr_flags, width);\n    if (r56s5_validate) {\n        tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.text_pal,\n                                (const uint8_t *)text_palette,\n                                (uint32_t)sizeof(slot->u.gbt65k.text_pal));\n        memset(slot->u.gbt65k.text_idx, 0, width);\n        if (text_on && text_valid)\n            tab5_raster590_copy_run(slot->u.gbt65k.text_idx, text_src, text_valid);\n        if (bg_on)\n            memcpy(&slot->u.gbt65k.bg_state, bg_state, sizeof(*bg_state));\n    }\n\n    s_last_gbt65k_build_us ='''
cs = replace_once(cs, old, new, 'compose validation source snapshots')

# Need a BG writer barrier during validation because CPU0 will inspect host BG sources.
old = '''    s_last_gbt65k_build_us =\n        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);\n\n    if (burst65k) {'''
new = '''    s_last_gbt65k_build_us =\n        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);\n\n    if (r56s5_validate && bg_on)\n        __atomic_add_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);\n\n    if (burst65k) {'''
# There are two such anchors (normal and exact); target last occurrence by split.
if cs.count(old) < 1: fail('compose exact barrier anchor missing')
pos = cs.find('int tab5_compose_submit_gbt65k_exact_bt_line')
idx = cs.find(old, pos)
if idx < 0: fail('compose exact barrier local anchor missing')
cs = cs[:idx] + new + cs[idx+len(old):]

# Roll back validation state/barrier on burst ready-push failure.
old = '''        if (!gbt65k_ready_push(idx)) {\n            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);\n            (void)gbt65k_free_push(idx);\n            ++s_gbt65k_burst_full;\n            return 0;\n        }'''
new = '''        if (!gbt65k_ready_push(idx)) {\n            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);\n            if (r56s5_validate && bg_on)\n                __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);\n            if (r56s5_validate) store_release(&s_r56s5_hostbt_state, 0u);\n            (void)gbt65k_free_push(idx);\n            ++s_gbt65k_burst_full;\n            return 0;\n        }'''
# Again specifically inside exact function.
pos = cs.find('int tab5_compose_submit_gbt65k_exact_bt_line')
idx = cs.find(old, pos)
if idx < 0: fail('compose exact burst rollback anchor missing')
cs = cs[:idx] + new + cs[idx+len(old):]

# Non-burst queue failure rollback.
old = '''    if (!queue_slot(idx))\n        return 0;\n\n    ++s_gbt65k_submitted;\n    return 1;\n}'''
new = '''    if (!queue_slot(idx)) {\n        if (r56s5_validate && bg_on)\n            __atomic_sub_fetch(&s_bgsource_pending, 1u, __ATOMIC_RELEASE);\n        if (r56s5_validate) store_release(&s_r56s5_hostbt_state, 0u);\n        return 0;\n    }\n\n    ++s_gbt65k_submitted;\n    return 1;\n}'''
pos = cs.find('int tab5_compose_submit_gbt65k_exact_bt_line')
idx = cs.find(old, pos)
if idx < 0: fail('compose exact nonburst rollback anchor missing')
cs = cs[:idx] + new + cs[idx+len(old):]

# ---- main marker ----
old = '    ESP_LOGI(TAG, "PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active");\n'
new = old + f'    ESP_LOGI(TAG, "{MARK}");\n'
ms = replace_once(ms, old, new, 'main R56s5 marker')

# Contract checks before write.
checks = [
    ('windraw host state getter', 'tab5_compose_gbt65k_hostbt_state' in ws),
    ('windraw exact_host arg', 'r56s5_exact_host' in ws),
    ('compose live PASS', 'CPU0 stock-order BG/TEXT live self-check PASS' in cs),
    ('compose fail fallback', 'R56s4 exact fallback retained' in cs),
    ('compose host helper', 'r56s5_render_host_bt_exact' in cs),
    ('main marker', MARK in ms),
]
for label, ok in checks:
    if not ok: fail('contract check failed: ' + label)

W.write_text(ws, encoding='utf-8', newline='')
C.write_text(cs, encoding='utf-8', newline='')
M.write_text(ms, encoding='utf-8', newline='')
print('R56s5 applied: live-validated CPU0 exact stock-order BG/TEXT + cached 65K enabled')
