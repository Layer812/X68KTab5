#!/usr/bin/env python3
from pathlib import Path

MAIN = Path('src/main.c')
WD = Path('components/px68k/libretro/windraw.c')
MARK = 'PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored'


def die(msg, rc=2):
    print('R56s 65K BG handoff patch ERROR:', msg)
    raise SystemExit(rc)


def one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old, new, 1)

m = MAIN.read_text(encoding='utf-8')
w = WD.read_text(encoding='utf-8')

if MARK in m and 'PX68K_R56S_65K_BG_HANDOFF' in w:
    if w.count('TextPal, stp, bg_on, text_on') != 1:
        die('already-applied BG packet call count')
    if 'if (!bg_on && (!text_on || text_src))' in w:
        die('already-applied source still contains R56a BG disable gate')
    print('R56s already applied and verified')
    raise SystemExit(0)

for need in [
    'PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only',
    'R56R_PATH f=',
    'PX68K_R56Q1: memory-safe sparse exec/CPU0 attribution',
]:
    if need not in m:
        die(f'main predecessor missing: {need}')

for need in [
    'PX68K_R56R_COUNTERS',
    'BG_CaptureHostLineState(&st, vbg, gd)',
    'tab5_compose_submit_gbt65k_line(',
    'tab5_screen_render_cancel(render_ticket)',
    'TextPal, NULL, 0, text_on',
    'if (!bg_on && (!text_on || text_src))',
]:
    if need not in w:
        die(f'windraw predecessor missing: {need}')

m = one(
    m,
    '    ESP_LOGI(TAG, "PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only");',
    '    ESP_LOGI(TAG, "PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only");\n'
    '    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");',
    'startup marker')

old = '''            /* R56a: BG async is intentionally disabled below, but keep the
             * capture-side state calculation intact for now without an
             * unused-but-set diagnostic on the target toolchain. */
            (void)stp;

            /* R56: async 65K is admitted only when all source material is
             * immutable in the packet. BG/Sprite still depends on broader guest
             * structures, so BG-on lines intentionally stay on the synchronous
             * reference renderer until that source family is packetized. */
            if (!bg_on && (!text_on || text_src)) {
                int accepted = tab5_compose_submit_gbt65k_line(
                    VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],
                    Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),
                    text_src, text_valid, TextPal, NULL, 0, text_on,
                    grp_pri, bg_pri, text_pri,
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
new = '''            /* PX68K_R56S_65K_BG_HANDOFF
             * Restore the proven 6.15e BG/Sprite host handoff under the R56
             * ownership contract.  The current compositor copies BG_HOST_LINE_STATE
             * into the 65K job, retains the BG/Sprite source barrier for the shared
             * character/sprite arrays, snapshots the 65K GVRAM row, and binds the
             * result to this Screen Manager render_ticket.  If capture fails, keep
             * the exact CPU1 legacy renderer. */
            if ((!text_on || text_src) && (!bg_on || stp)) {
                int accepted = tab5_compose_submit_gbt65k_line(
                    VLINE, (uint32_t)TextDotX, GVRAM, gy, GrphScrollX[0],
                    Pal_Regs, Pal_DebugVisualGeneration(), Pal_DebugEffectiveContrast(),
                    text_src, text_valid, TextPal, stp, bg_on, text_on,
                    grp_pri, bg_pri, text_pri,
                    &RenderBuf[VLINE * FULLSCREEN_WIDTH], render_ticket);'''
w = one(w, old, new, 'R56a BG disable -> R56s handoff')

MAIN.write_text(m, encoding='utf-8', newline='\n')
WD.write_text(w, encoding='utf-8', newline='\n')
print('R56s applied: proven 65K BG/Sprite host path reconnected to R56 render tickets')
