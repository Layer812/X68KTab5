#!/usr/bin/env python3
from pathlib import Path

UI = Path('src/tab5_media_ui.cpp')
MAIN = Path('src/main.c')
MARK = 'PX68K_R56S2: media browser PSRAM-string staging bridge active'
UI_MARK = 'PX68K_R56S2_PSRAM_UI_STRING_BRIDGE'

def die(msg, rc=2):
    print('R56s2 PSRAM UI string patch ERROR:', msg)
    raise SystemExit(rc)

def one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old, new, 1)

u = UI.read_text(encoding='utf-8')
m = MAIN.read_text(encoding='utf-8')

if 'PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active' not in m:
    die('R56s1 predecessor marker missing')
if 'tab5_media_ui_draw_browser' not in u:
    die('shared media browser missing')

if UI_MARK in u or MARK in m:
    if UI_MARK not in u or MARK not in m:
        die('partial prior R56s2 application')
    if 'char r56s2_path_buf[TAB5_MEDIA_UI_PATH_MAX];' not in u:
        die('already-applied bridge buffer missing')
    if 'M5.Display.print(r56s2_name);' not in u or 'M5.Display.print(r56s2_path);' not in u:
        die('already-applied staged draws incomplete')
    print('R56s2 already applied and verified')
    raise SystemExit(0)

# Insert one reusable stack staging area in the browser function. The media
# catalog remains in PSRAM; only the strings handed to M5GFX are staged.
anchor = '''    const size_t first = page * TAB5_MEDIA_UI_ROWS_PER_PAGE;\n    for (int r = 0; r < TAB5_MEDIA_UI_ROWS_PER_PAGE; ++r) {\n'''
repl = '''    /* PX68K_R56S2_PSRAM_UI_STRING_BRIDGE\n     * R23 intentionally keeps the cold media catalog in PSRAM. Keep that\n     * memory policy, but do not hand PSRAM-backed C strings directly to the\n     * M5GFX Print path on ESP32-P4. Stage only the currently drawn row into\n     * this function's internal stack first. Selection/filtering still uses\n     * the original catalog and is behavior-identical. */\n    char r56s2_path_buf[TAB5_MEDIA_UI_PATH_MAX];\n    char r56s2_name_buf[128];\n\n    const size_t first = page * TAB5_MEDIA_UI_ROWS_PER_PAGE;\n    for (int r = 0; r < TAB5_MEDIA_UI_ROWS_PER_PAGE; ++r) {\n'''
u = one(u, anchor, repl, 'browser loop staging insertion')

old = '''        M5.Display.setTextColor(TFT_WHITE, fill);\n        M5.Display.setCursor(150, y + 11);\n        M5.Display.print(empty ? "Empty / eject" : tab5_media_ui_basename(e->path));\n        M5.Display.setTextSize(1);\n        M5.Display.setTextColor(0x8C71, fill);\n        M5.Display.setCursor(151, y + 42);\n        M5.Display.print(empty ? "Leave this slot empty" : e->path);\n'''
new = '''        const char *r56s2_name = "Empty / eject";\n        const char *r56s2_path = "Leave this slot empty";\n        if (!empty) {\n            std::snprintf(r56s2_path_buf, sizeof(r56s2_path_buf), "%s", e->path);\n            std::snprintf(r56s2_name_buf, sizeof(r56s2_name_buf), "%s",\n                          tab5_media_ui_basename(r56s2_path_buf));\n            r56s2_name = r56s2_name_buf;\n            r56s2_path = r56s2_path_buf;\n        }\n        M5.Display.setTextColor(TFT_WHITE, fill);\n        M5.Display.setCursor(150, y + 11);\n        M5.Display.print(r56s2_name);\n        M5.Display.setTextSize(1);\n        M5.Display.setTextColor(0x8C71, fill);\n        M5.Display.setCursor(151, y + 42);\n        M5.Display.print(r56s2_path);\n'''
u = one(u, old, new, 'PSRAM path direct print -> staged print')

m = one(
    m,
    '    ESP_LOGI(TAG, "PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active");',
    '    ESP_LOGI(TAG, "PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active");\n'
    '    ESP_LOGI(TAG, "PX68K_R56S2: media browser PSRAM-string staging bridge active");',
    'startup marker')

UI.write_text(u, encoding='utf-8', newline='\n')
MAIN.write_text(m, encoding='utf-8', newline='\n')
print('R56s2 applied: media catalog stays in PSRAM; browser strings are staged to stack before M5GFX draw')
