from pathlib import Path
import sys

W = Path('components/px68k/libretro/windraw.c')
M = Path('src/main.c')
TAG = 'PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE'
MARK = 'PX68K_R56S5K: visible TEXT host-BT quarantined after false one-line PASS; R56s4 exact BT authoritative'


def fail(msg):
    print('R56s5k patch ERROR:', msg)
    raise SystemExit(2)

if not W.is_file() or not M.is_file():
    fail('missing windraw.c or main.c')
ws = W.read_text(encoding='utf-8')
ms = M.read_text(encoding='utf-8')

if TAG in ws and MARK in ms:
    print('R56s5k already applied and verified')
    raise SystemExit(0)
if TAG in ws or MARK in ms:
    fail('partial R56s5k state')

old = '            const int r56s5_exact_host = r56s3_text_visible && (r56s5_hostbt == 2);'
if ws.count(old) != 1:
    fail(f'R56s5 visible-TEXT re-entry gate count={ws.count(old)}')

new = r'''            /* PX68K_R56S5K_VISIBLE_TEXT_QUARANTINE
             * R56s5's one-line validator produced a false PASS on real HDS content.
             * Restore the proven R56s4 correctness contract immediately: any row
             * with visible TEXT must continue through stock CPU1 BG/TEXT generation
             * and only hand the authoritative combined BT snapshot + 65K GRP to CPU0.
             *
             * Keep a one-time structural-order trace.  The first R56s5 PASS was for
             * gd=1 (BG-first -> TEXT key-zero overlay); this tells us whether the HDS
             * application later exercises the unvalidated gd=0 order. */
            static uint8_t s_r56s5k_seen_order;
            if (r56s3_text_visible) {
                const uint8_t r56s5k_bit = (!bg_on || !stp) ? 4u : (stp->gd ? 2u : 1u);
                if (!(s_r56s5k_seen_order & r56s5k_bit)) {
                    s_r56s5k_seen_order |= r56s5k_bit;
                    printf("PX68K_R56S5K_ORDER: visible TEXT stock-fenced gd=%d bg=%d pri G/T/B=%u/%u/%u chr=%u reg9=%02X\n",
                           stp ? (int)stp->gd : -1, bg_on,
                           (unsigned)grp_pri, (unsigned)text_pri, (unsigned)bg_pri,
                           stp ? (unsigned)stp->chr_size : 0u,
                           stp ? (unsigned)stp->reg9 : 0u);
                }
            }
            const int r56s5_exact_host = 0; /* correctness quarantine: no visible-TEXT CPU0 host-BT */'''
ws2 = ws.replace(old, new, 1)

anchor = '    ESP_LOGI(TAG, "PX68K_R56S5F: validator forced to ordinary queue by brace-scoped function patch; burst after PASS");\n'
if ms.count(anchor) != 1:
    fail(f'R56s5F main marker line count={ms.count(anchor)}')
ms2 = ms.replace(anchor, anchor + f'    ESP_LOGW(TAG, "{MARK}");\n', 1)

# In-memory audit before write.
checks = [
    (TAG in ws2, 'quarantine tag missing'),
    ('const int r56s5_exact_host = 0;' in ws2, 'forced-safe gate missing'),
    ('PX68K_R56S5K_ORDER:' in ws2, 'order trace missing'),
    (MARK in ms2, 'main marker missing'),
    (old not in ws2, 'unsafe re-entry gate survived'),
]
for ok, msg in checks:
    if not ok: fail(msg)

W.write_text(ws2, encoding='utf-8', newline='')
M.write_text(ms2, encoding='utf-8', newline='')
print('R56s5k applied: visible TEXT forced back to R56s4 exact CPU1 BG/TEXT; one-time order trace armed')
