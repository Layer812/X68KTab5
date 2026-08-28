#!/usr/bin/env python3
from pathlib import Path

MAIN = Path('src/main.c')
WD = Path('components/px68k/libretro/windraw.c')
MARK = 'PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active'
BEGIN_MARK = 'PX68K_R56S1_LAUNCHER_HOST_UI'

def die(msg, rc=2):
    print('R56s1 launcher Host-UI patch ERROR:', msg)
    raise SystemExit(rc)

def one(s, old, new, label):
    n = s.count(old)
    if n != 1:
        die(f'{label}: expected 1 anchor, found {n}')
    return s.replace(old, new, 1)

m = MAIN.read_text(encoding='utf-8')
w = WD.read_text(encoding='utf-8')

if 'PX68K_R56S_65K_BG_HANDOFF' not in w:
    die('R56s 65K BG/Sprite handoff predecessor missing')
if 'PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored' not in m:
    die('R56s main marker missing')

if MARK in m or BEGIN_MARK in m:
    if MARK not in m or BEGIN_MARK not in m:
        die('partial prior R56s1 application')
    # Runtime launcher already has begin/end calls in this lineage, so validate
    # only the cold-launcher markers/order rather than global call counts.
    bi = m.index(BEGIN_MARK)
    try:
        bcall = m.index('tab5_video_begin_host_ui();', bi)
        ecall = m.index('tab5_video_end_host_ui();', bcall)
        hp = m.index('tab5_log_memory_550("host-preworkers")', ecall)
    except ValueError:
        die('already-applied cold ownership fence incomplete')
    if not (bi < bcall < ecall < hp):
        die('already-applied cold ownership fence ordering')
    print('R56s1 already applied and verified')
    raise SystemExit(0)

m = one(
    m,
    '    tab5_video_init();\n',
    '    tab5_video_init();\n'
    '    /* PX68K_R56S1_LAUNCHER_HOST_UI\n'
    '     * Keep cold-start Media Setup + picker on the M5GFX/FB0 ownership\n'
    '     * path for the entire launcher session. R49 quarantines FB1 scanout;\n'
    '     * begin_host_ui() prepares the safe M5GFX front and prevents the LCD\n'
    '     * presenter from racing native UI draws. */\n'
    '    tab5_video_begin_host_ui();\n',
    'video init -> cold launcher Host-UI begin')

m = one(
    m,
    '    tab5_log_memory_550("host-preworkers");\n',
    '    /* R56s1: launcher selection is complete; return panel ownership before\n'
    '     * starting audio/compose/USB workers and the guest. */\n'
    '    tab5_video_end_host_ui();\n'
    '    tab5_log_memory_550("host-preworkers");\n',
    'cold launcher complete -> Host-UI end')

m = one(
    m,
    '    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");',
    '    ESP_LOGI(TAG, "PX68K_R56S: R56 ownership-safe 65K BG/Sprite host handoff restored");\n'
    '    ESP_LOGI(TAG, "PX68K_R56S1: cold launcher Host-UI FB0 ownership fence active");',
    'startup marker')

MAIN.write_text(m, encoding='utf-8', newline='\n')
print('R56s1 applied: cold Media Setup/picker held under Host-UI FB0 ownership until boot selection')
