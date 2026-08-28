#!/usr/bin/env python3
from pathlib import Path

MAIN = Path('src/main.c')
WD = Path('components/px68k/libretro/windraw.c')
MARK = 'PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload'
WD_MARK = 'PX68K_R56S3_VISIBLE_TEXT_EXACT_FENCE'

def die(msg, rc=2):
    print('R56s3 visible-text exact fence ERROR:', msg)
    raise SystemExit(rc)

def one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old, new, 1)

m = MAIN.read_text(encoding='utf-8')
w = WD.read_text(encoding='utf-8')

if MARK in m or WD_MARK in w:
    if MARK not in m or WD_MARK not in w:
        die('partial prior R56s3 application')
    if w.count('r56s3_text_visible') != 3:
        die(f'already-applied visible-text token count unexpected: {w.count("r56s3_text_visible")}')
    print('R56s3 already applied and verified')
    raise SystemExit(0)

for need in [
    'PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored',
    'PX68K_R56R: WinDraw render-path taxonomy fact probe; counters-only',
]:
    if need not in m:
        die(f'main predecessor missing: {need}')

for need in [
    'PX68K_R56S_65K_BG_HANDOFF',
    'const uint8_t *text_src = NULL;',
    'text_src = expanded + (ty << 10) + tx;',
    'if ((!text_on || text_src) && (!bg_on || stp)) {',
    'tab5_compose_submit_gbt65k_line(',
]:
    if need not in w:
        die(f'windraw predecessor missing: {need}')

# Build a correctness fence from the exact pixels the current fast path would
# packetize.  The host compositor's compact priority selector is proven for
# MDX/common transparent-TEXT lines, but some Human68k applications exercise
# text semantics not covered by its synthetic self-check.  If this raster line
# actually contains any non-transparent text pixel, retain the stock renderer.
anchor = '''            BG_HOST_LINE_STATE st;\n'''
insert = '''            /* PX68K_R56S3_VISIBLE_TEXT_EXACT_FENCE\n             * Keep R56s speed for transparent TEXT rows, but preserve the stock\n             * WinDraw semantics for rows that actually contain visible TVRAM\n             * pixels.  This is deliberately per-line rather than text_on-wide:\n             * MDX/common 65K rows without text still use the CPU0 cache path. */\n            int r56s3_text_visible = 0;\n            if (text_on && text_src) {\n                for (uint32_t r56s3_x = 0; r56s3_x < text_valid; ++r56s3_x) {\n                    if (text_src[r56s3_x] & 0x0fu) {\n                        r56s3_text_visible = 1;\n                        break;\n                    }\n                }\n            }\n\n            BG_HOST_LINE_STATE st;\n'''
w = one(w, anchor, insert, 'visible-text scan insertion')

w = one(
    w,
    '            if ((!text_on || text_src) && (!bg_on || stp)) {',
    '            if (!r56s3_text_visible && (!text_on || text_src) && (!bg_on || stp)) {',
    'offload admission fence')

# Put startup marker after the newest UI marker when present, otherwise after R56s.
if '    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");' in m:
    m = one(
        m,
        '    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");',
        '    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");\n'
        '    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");',
        'startup marker after R56s2')
else:
    m = one(
        m,
        '    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");',
        '    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");\n'
        '    ESP_LOGI(TAG, "PX68K_R56S3: visible TEXT lines use exact CPU1 renderer; transparent TEXT keeps CPU0 65K offload");',
        'startup marker after R56s')

MAIN.write_text(m, encoding='utf-8', newline='\n')
WD.write_text(w, encoding='utf-8', newline='\n')
print('R56s3 applied: visible TVRAM text rows stay exact; transparent rows retain R56s CPU0 65K path')
