#!/usr/bin/env python3
from pathlib import Path

MAIN = Path('src/main.c')
WD = Path('components/px68k/libretro/windraw.c')
CP = Path('src/tab5_compose.c')

MARK = 'PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active'
WD_MARK = 'PX68K_R56S4_EXACT_BT_65K_HANDOFF'
CP_MARK = 'PX68K_R56S4_GBT65K_EXACT_BT'


def die(msg, rc=2):
    print('R56s4 exact-BT 65K handoff ERROR:', msg)
    raise SystemExit(rc)


def one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old, new, 1)

m = MAIN.read_text(encoding='utf-8')
w = WD.read_text(encoding='utf-8')
c = CP.read_text(encoding='utf-8')

if MARK in m or WD_MARK in w or CP_MARK in c:
    if not (MARK in m and WD_MARK in w and CP_MARK in c):
        die('partial prior R56s4 application')
    print('R56s4 already applied and verified')
    raise SystemExit(0)

for need in [
    'PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload',
    'PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored',
]:
    if need not in m:
        die(f'main predecessor missing: {need}')

for need in [
    'PX68K_R56S3_VISIBLE_TEXT_EXACT_FENCE',
    'int r56s3_text_visible = 0;',
    'if (!r56s3_text_visible && (!text_on || text_src) && (!bg_on || stp)) {',
    'case 3:\t\t\t\t\t/* 65536 colors */',
    'WD_PERF_GRP(Grp_DrawLine16());',
    '/* Build 6.14b2 Render Phase B2.',
]:
    if need not in w:
        die(f'windraw predecessor missing: {need}')

for need in [
    'COMPOSE_JOB_GBT65K = 8',
    'uint8_t text_idx[TAB5_COMPOSE_MAX_WIDTH];',
    'static int render_gbt65k_line(compose_slot_t *slot)',
    'int tab5_compose_submit_gbt65k_line(',
    '_Static_assert(sizeof(compose_slot_t) <= 8544u,',
]:
    if need not in c:
        die(f'compose predecessor missing: {need}')

# ---------------------------------------------------------------------------
# 1) CPU0 packet: retain existing simplified text/BG fields, but add a second
#    authoritative raster representation.  The union's dominant GRP8 split
#    payload is already ~8.5 KiB, so these fields do NOT enlarge compose_slot_t.
# ---------------------------------------------------------------------------
c = one(
    c,
    '''            uint8_t text_idx[TAB5_COMPOSE_MAX_WIDTH];\n            uint16_t text_pal[256];\n            BG_HOST_LINE_STATE bg_state;\n        } gbt65k;''',
    '''            uint8_t text_idx[TAB5_COMPOSE_MAX_WIDTH];\n            uint16_t text_pal[256];\n            BG_HOST_LINE_STATE bg_state;\n            /* PX68K_R56S4_GBT65K_EXACT_BT\n             * Stock WinDraw has already resolved BG/TEXT interaction into\n             * BG_LineBuf + Text_TrFlag.  These snapshots are used only when\n             * selfcheck bit3 is set; simplified MDX/transparent-TEXT packets\n             * continue using text_idx/text_pal/bg_state above. */\n            uint16_t exact_bg_text[TAB5_COMPOSE_MAX_WIDTH];\n            uint8_t exact_flags[TAB5_COMPOSE_MAX_WIDTH];\n        } gbt65k;''',
    'authoritative fields')

# Exact final selector over cached/wrapped 65K GRP row.  This is deliberately
# the same 6.14a selector already validated for authoritative BG_LineBuf/flags.
render_anchor = '''static int render_gbt65k_line(compose_slot_t *slot)\n{\n    const uint16_t *grp_row = NULL;'''
render_insert = '''static inline void r56s4_render_gbt65k_exact_bt(uint16_t *dst,\n                                                    const uint16_t *grp_row,\n                                                    uint32_t gx,\n                                                    const uint16_t *bg_text,\n                                                    const uint8_t *flags,\n                                                    uint32_t width,\n                                                    uint8_t grp_pri,\n                                                    uint8_t bg_pri,\n                                                    uint8_t text_pri)\n{\n    grp_pri &= 3u;\n    bg_pri &= 3u;\n    text_pri &= 3u;\n    for (uint32_t i = 0; i < width; ++i) {\n        const uint16_t g = grp_row[(gx + i) & 511u];\n        const uint16_t bt = bg_text[i];\n        const uint8_t f = (uint8_t)(flags[i] & 3u);\n        if (__builtin_expect(f == 0u, 0)) {\n            dst[i] = g;\n            continue;\n        }\n        uint8_t bt_pri = 4u;\n        if (f & 2u) bt_pri = bg_pri;\n        if ((f & 1u) && text_pri < bt_pri) bt_pri = text_pri;\n        dst[i] = (bt_pri <= grp_pri) ? (bt ? bt : g) : (g ? g : bt);\n    }\n}\n\nstatic int render_gbt65k_line(compose_slot_t *slot)\n{\n    const uint16_t *grp_row = NULL;'''
c = one(c, render_anchor, render_insert, 'exact cached-GRP renderer insertion')

# Branch before the older independent-layer renderer.
branch_anchor = '''    const int text_on = (slot->selfcheck & 1u) != 0u;\n    const int bg_on = (slot->selfcheck & 2u) != 0u;'''
branch_insert = '''    /* R56s4 bit3: CPU1 already produced authoritative stock BG/TEXT raster.\n     * Only cached 65K GRP decode + the validated 6.14a final selector run here. */\n    if (slot->selfcheck & 8u) {\n        r56s4_render_gbt65k_exact_bt(slot->dst, grp_row,\n                                     slot->u.gbt65k.gvram_x & 511u,\n                                     slot->u.gbt65k.exact_bg_text,\n                                     slot->u.gbt65k.exact_flags,\n                                     slot->width, slot->grp_pri,\n                                     slot->bg_pri, slot->text_pri);\n        ++s_gbt65k_scalar_lines;\n        return 1;\n    }\n\n    const int text_on = (slot->selfcheck & 1u) != 0u;\n    const int bg_on = (slot->selfcheck & 2u) != 0u;'''
c = one(c, branch_anchor, branch_insert, 'render exact-mode branch')

# New submission function: same nonblocking 128-slot 65K queue and same raw-row
# snapshot/cache identity, but copy stock BG_LineBuf/Text_TrFlag instead of
# asking CPU0 to reconstruct those semantics.
submit_anchor = '''int tab5_compose_submit_gbt_scrollcache_line(uint32_t y, uint32_t width,'''
submit_fn = r'''int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,
                                             const uint8_t *gvram,
                                             uint32_t gvram_row, uint32_t gvram_x,
                                             const uint8_t *pal_regs,
                                             uint32_t pal_generation, uint8_t contrast,
                                             const uint16_t *bg_text,
                                             const uint8_t *text_tr_flags,
                                             uint8_t grp_pri, uint8_t bg_pri,
                                             uint8_t text_pri, uint16_t *dst,
                                             uint64_t render_ticket)
{
    if (!s_ready || !s_gbt65k_ready || !gvram || !pal_regs ||
        !bg_text || !text_tr_flags || !dst ||
        width == 0u || width > TAB5_COMPOSE_MAX_WIDTH) {
        ++s_fallback;
        return -1;
    }
    if (!gbt65k_runtime_pal_selfcheck(contrast)) {
        ++s_fallback;
        return -1;
    }

    uint8_t idx;
    const int burst65k = s_gbt65k_burst_ready;
    compose_slot_t *slot = NULL;
    if (burst65k) {
        if (!gbt65k_free_pop(&idx)) {
            ++s_gbt65k_burst_full;
            return 0;
        }
        slot = &s_gbt65k_burst_slots[idx];
    } else {
        if (!acquire_slot(&idx))
            return 0;
        slot = &s_slots[idx];
    }

    if (burst65k)
        s_gbt65k_burst_render_ticket[idx] = render_ticket;
    else
        s_slot_render_ticket[idx] = render_ticket;

    const uint32_t cc0 = (uint32_t)esp_cpu_get_cycle_count();
    slot->type = COMPOSE_JOB_GBT65K;
    slot->bottom_page = 0u;
    slot->top_page = 0u;
    slot->bg_on_top = 0u;
    slot->selfcheck = 8u; /* authoritative BG/TEXT snapshot; no BG source barrier */
    slot->grp_pri = (uint8_t)(grp_pri & 3u);
    slot->bg_pri = (uint8_t)(bg_pri & 3u);
    slot->text_pri = (uint8_t)(text_pri & 3u);
    slot->y = y;
    slot->width = width;
    slot->dst = dst;
    slot->u.gbt65k.gvram_row = gvram_row & 511u;
    slot->u.gbt65k.gvram_x = gvram_x & 511u;
    slot->u.gbt65k.row_generation =
        __atomic_load_n(&GVRAM_RowGeneration[slot->u.gbt65k.gvram_row],
                        __ATOMIC_ACQUIRE);
    tab5_raster590_copy_run(
        (uint8_t *)slot->u.gbt65k.raw_row,
        gvram + (slot->u.gbt65k.gvram_row << 10),
        (uint32_t)sizeof(slot->u.gbt65k.raw_row));
    slot->u.gbt65k.pal_generation = pal_generation;
    slot->u.gbt65k.frame_epoch = load_acquire(&s_gbt65k_frame_epoch);
    slot->u.gbt65k.contrast = (uint8_t)(contrast & 15u);
    tab5_raster590_copy_run(slot->u.gbt65k.pal_regs, pal_regs,
                            (uint32_t)sizeof(slot->u.gbt65k.pal_regs));
    tab5_raster590_copy_run((uint8_t *)slot->u.gbt65k.exact_bg_text,
                            (const uint8_t *)bg_text,
                            width * (uint32_t)sizeof(uint16_t));
    tab5_raster590_copy_run(slot->u.gbt65k.exact_flags,
                            text_tr_flags, width);

    s_last_gbt65k_build_us =
        cycles_to_us((uint32_t)esp_cpu_get_cycle_count() - cc0);

    if (burst65k) {
        const uint32_t pending_now =
            __atomic_add_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_ACQ_REL);
        uint32_t old_max = load_relaxed(&s_gbt65k_burst_max_pending);
        while (pending_now > old_max &&
               !__atomic_compare_exchange_n(&s_gbt65k_burst_max_pending,
                                            &old_max, pending_now, 0,
                                            __ATOMIC_RELAXED,
                                            __ATOMIC_RELAXED)) {
        }
        if (!gbt65k_ready_push(idx)) {
            __atomic_sub_fetch(&s_gbt65k_burst_pending, 1u, __ATOMIC_RELEASE);
            (void)gbt65k_free_push(idx);
            ++s_gbt65k_burst_full;
            return 0;
        }
        ++s_gbt65k_burst_notify;
        ++s_gbt65k_submitted;
        __atomic_add_fetch(&s_mb->notify_count, 1u, __ATOMIC_RELAXED);
        xTaskNotifyGive(s_task);
        return 1;
    }

    if (!queue_slot(idx))
        return 0;

    ++s_gbt65k_submitted;
    return 1;
}

int tab5_compose_submit_gbt_scrollcache_line(uint32_t y, uint32_t width,'''
c = one(c, submit_anchor, submit_fn, 'exact submission function')

# ---------------------------------------------------------------------------
# 2) WinDraw: mark visible-TEXT common 65K rows for exact-BT handoff.  Skip
#    Grp_DrawLine16 on CPU1, let the stock code construct TEXT/BG, then submit
#    those authoritative pixels/flags together with the raw 65K row to CPU0.
# ---------------------------------------------------------------------------
w = one(
    w,
    '''    WinDraw_Grp8Geom grp8_geom = {0};\n    uint64_t render_ticket = 0u;''',
    '''    WinDraw_Grp8Geom grp8_geom = {0};\n    uint64_t render_ticket = 0u;\n    int r56s4_65k_exact_bt = 0;''',
    'per-line exact mode state')

# Set the mode only for the visible-text lines that R56s3 would have sent to
# full CPU1 legacy.  Transparent lines still take the original R56s fast path.
w = one(
    w,
    '''            BG_HOST_LINE_STATE st;''',
    '''            if (r56s3_text_visible)\n                r56s4_65k_exact_bt = 1;\n\n            BG_HOST_LINE_STATE st;''',
    'visible text -> exact-BT mode')

# Skip only normal Grp_DrawLine16. Special/half-transparent 65K remains stock.
w = one(
    w,
    '''\t\t\telse\n\t\t\t{\n\t\t\t\tWD_PERF_GRP(Grp_DrawLine16());\n\t\t\t\tgon=1;\n\t\t\t}''',
    '''\t\t\telse\n\t\t\t{\n                /* PX68K_R56S4_EXACT_BT_65K_HANDOFF\n                 * Visible-TEXT common 65K rows keep GRP unmaterialized on CPU1.\n                 * Stock TEXT/BG generation below remains authoritative; CPU0\n                 * reconstructs cached 65K GRP and performs the proven 6.14a\n                 * final selector. */\n                if (!r56s4_65k_exact_bt)\n                    WD_PERF_GRP(Grp_DrawLine16());\n\t\t\t\tgon=1;\n\t\t\t}''',
    'skip CPU1 normal 65K GRP')

# Add extern API next to existing 65K submit declaration.
w = one(
    w,
    '''extern int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                           const uint8_t *gvram,''',
    '''extern int tab5_compose_submit_gbt65k_exact_bt_line(uint32_t y, uint32_t width,\n                                                    const uint8_t *gvram,\n                                                    uint32_t gvram_row, uint32_t gvram_x,\n                                                    const uint8_t *pal_regs,\n                                                    uint32_t pal_generation, uint8_t contrast,\n                                                    const uint16_t *bg_text,\n                                                    const uint8_t *text_tr_flags,\n                                                    uint8_t grp_pri, uint8_t bg_pri,\n                                                    uint8_t text_pri, uint16_t *dst,\n                                                    uint64_t render_ticket);\nextern int tab5_compose_submit_gbt65k_line(uint32_t y, uint32_t width,\n                                           const uint8_t *gvram,''',
    'exact API extern')

# Submit after the untouched stock TEXT/BG generation and before any legacy
# final compositor / 256-colour GBT logic.
late_anchor = '''    /* Build 6.14b2 Render Phase B2.\n'''
late_insert = '''    /* R56s4: for visible-TEXT normal 65K, stock WinDraw has now generated\n     * authoritative BG_LineBuf/Text_TrFlag.  Hand those exact semantics to\n     * CPU0 together with an immutable 65K GVRAM row snapshot. */\n    if (r56s4_65k_exact_bt && gon && bgon && ton && !tron && !pron)\n    {\n        uint32_t gy = GrphScrollY[0] + VLINE;\n        if ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu) gy += VLINE;\n        gy &= 0x1ffu;\n        const uint8_t grp_pri = (uint8_t)(VCReg1[0] & 3u);\n        const uint8_t text_pri = (uint8_t)((VCReg1[0] >> 2) & 3u);\n        const uint8_t bg_pri = (uint8_t)((VCReg1[0] >> 4) & 3u);\n        int accepted = tab5_compose_submit_gbt65k_exact_bt_line(\n            VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],\n            Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),\n            &BG_LineBuf[16], &Text_TrFlag[16],\n            grp_pri, bg_pri, text_pri,\n            &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);\n        if (accepted > 0) {\n            ++s_r56r_path[R56R_CPU0_65];\n            return;\n        }\n        if (accepted == 0) {\n            ++s_r56r_path[R56R_65_QFULL];\n            tab5_screen_render_cancel(render_ticket);\n            return;\n        }\n        /* Host exact path unavailable: materialize the GRP line now and enter\n         * the untouched stock final compositor. Correctness always wins. */\n        ++s_r56r_path[R56R_65_REJECT];\n        WD_PERF_GRP(Grp_DrawLine16());\n    }\n\n    /* Build 6.14b2 Render Phase B2.\n'''
w = one(w, late_anchor, late_insert, 'late exact-BT submission')

# ---------------------------------------------------------------------------
# 3) Startup marker.
# ---------------------------------------------------------------------------
m = one(
    m,
    '    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");',
    '    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");\n'
    '    ESP_LOGI(TAG, "PX68K_R56S4: authoritative stock BG/TEXT + CPU0 cached 65K GRP handoff active");',
    'startup marker')

MAIN.write_text(m, encoding='utf-8', newline='\n')
WD.write_text(w, encoding='utf-8', newline='\n')
CP.write_text(c, encoding='utf-8', newline='\n')
print('R56s4 applied: visible TEXT keeps stock BG/TEXT semantics while cached 65K GRP returns to CPU0')
