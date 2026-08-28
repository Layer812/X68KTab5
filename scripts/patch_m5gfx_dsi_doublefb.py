# Build 6.15h17 DBFB patch revision 5 / R18 ST7123 long-VBlank pacing.
#
# PlatformIO can keep more than one physical M5GFX dependency directory under
# .pio/libdeps/<env>/ (for example "M5GFX" and "M5GFX@0.2.26"). Revision 2
# selected only one candidate, which could patch the dependency copy that was
# NOT actually compiled. R3 patches and verifies EVERY ESP32-P4 Panel_DSI
# candidate under PROJECT_LIBDEPS_DIR.
Import("env")

from pathlib import Path
import re

MARK = "PX68K_DSI_DOUBLEFB_PATCH_615H17R5"


def die(msg):
    print(f"*** {MARK}: ERROR: {msg}")
    env.Exit(1)


pioenv = env.subst("$PIOENV")
libdeps = Path(env.subst("$PROJECT_LIBDEPS_DIR")) / pioenv

hpp_candidates = sorted(
    [p for p in libdeps.rglob("Panel_DSI.hpp") if "esp32p4" in str(p).replace('\\', '/').lower()],
    key=lambda p: str(p).lower(),
)
if not hpp_candidates:
    die(f"Panel_DSI source not found under {libdeps}. Run `pio pkg install -e {pioenv}` first.")

pairs = []
for hpp in hpp_candidates:
    cpp = hpp.with_suffix('.cpp')
    if cpp.exists():
        pairs.append((hpp, cpp))
if not pairs:
    die(f"Panel_DSI.cpp not found beside any ESP32-P4 Panel_DSI.hpp under {libdeps}")


def patch_pair(hpp: Path, cpp: Path):
    ht = hpp.read_text(encoding='utf-8')
    ct = cpp.read_text(encoding='utf-8')

    # Header: expose framebuffer #1 and the raw ESP-IDF DPI panel handle.
    if 'esp_lcd_panel_ops.h' not in ht:
        inc = '#include "esp_lcd_panel_ops.h"\n'
        m_inc = list(re.finditer(r'(?m)^#include[^\n]*\n', ht))
        if m_inc:
            pos = m_inc[-1].end()
            ht = ht[:pos] + inc + ht[pos:]
        else:
            ht = inc + ht

    buffer_pat = re.compile(r'(?P<indent>[ \t]*)void\s*\*\s*buffer\s*(?:=\s*nullptr)?\s*;')
    m_buffer = buffer_pat.search(ht)
    if not m_buffer:
        raise RuntimeError(f"could not locate config_detail_t::buffer in {hpp}")
    indent = m_buffer.group('indent')
    insert_pos = m_buffer.end()

    header_extra = ''
    if not re.search(r'(?m)^\s*void\s*\*\s*buffer2\b', ht):
        header_extra += f"\n{indent}// {MARK}: second ESP-IDF DPI framebuffer.\n{indent}void *buffer2 = nullptr;"
    if not re.search(r'(?m)^\s*esp_lcd_panel_handle_t\s+px68k_dpi_panel\b', ht):
        header_extra += f"\n{indent}// {MARK}: raw DPI panel handle for refresh callback / FB swap.\n{indent}esp_lcd_panel_handle_t px68k_dpi_panel = nullptr;"
    if header_extra:
        ht = ht[:insert_pos] + header_extra + ht[insert_pos:]
        hpp.write_text(ht, encoding='utf-8')

    # Find the actual DPI config object passed to esp_lcd_new_panel_dpi.
    decl_names = re.findall(r'\besp_lcd_dpi_panel_config_t\s+([A-Za-z_]\w*)\b', ct)
    if not decl_names:
        raise RuntimeError(f"esp_lcd_dpi_panel_config_t declaration not found in {cpp}")

    call_match = None
    cfg_name = None
    for name in decl_names:
        pat = re.compile(
            r'esp_lcd_new_panel_dpi\s*\('
            r'(?:(?!\);).)*?'
            r'&\s*' + re.escape(name) + r'\b'
            r'(?:(?!\);).)*?\)',
            re.S,
        )
        m = pat.search(ct)
        if m:
            call_match = m
            cfg_name = name
            break
    if call_match is None or cfg_name is None:
        raise RuntimeError(
            f"could not match esp_lcd_new_panel_dpi(..., &<dpi_config>, ...) in {cpp}; declarations={decl_names}"
        )

    # R18: ESP32-P4 DSI uses an integer divider for the DPI clock.  A 74-MHz
    # request therefore rounds back to 80 MHz on the 480-MHz source and did not
    # lower the measured refresh.  Keep a factory-like 78-MHz request (real
    # clock remains ~80 MHz on this silicon), and lower refresh by using the
    # ST7123 long-VBlank timing seen in M5-derived configurations: VBP/VPW/VFP
    # = 4/2/320.  With H total 802 and active V=1280 this measures about 62 Hz,
    # reducing continuous framebuffer scanout contention without touching DSI
    # lane rate or the active image geometry.
    timing_assignments = [
        (f"{cfg_name}.dpi_clock_freq_mhz", "78"),
        (f"{cfg_name}.video_timing.vsync_back_porch", "4"),
        (f"{cfg_name}.video_timing.vsync_pulse_width", "2"),
        (f"{cfg_name}.video_timing.vsync_front_porch", "320"),
    ]
    # First remove any earlier PX68K direct assignments for these fields.
    # Refresh call_match afterwards because deleting a prior R11 line changes
    # source offsets.
    for lhs, rhs in timing_assignments:
        exact = re.compile(r'\b' + re.escape(lhs) + r'\s*=\s*' + re.escape(rhs) + r'\s*;')
        if not exact.search(ct):
            stale = re.compile(r'(?m)^[ \t]*' + re.escape(lhs) + r'\s*=\s*[^;]+;\s*\n?')
            ct = stale.sub('', ct)

    call_match = re.search(
        r'esp_lcd_new_panel_dpi\s*\(' r'(?:(?!\);).)*?' r'&\s*' + re.escape(cfg_name) + r'\b' r'(?:(?!\);).)*?\)',
        ct, re.S)
    if call_match is None:
        raise RuntimeError(f"lost esp_lcd_new_panel_dpi call while preparing R18 timing patch in {cpp}")
    line_start = ct.rfind('\n', 0, call_match.start()) + 1
    leading = re.match(r'[ \t]*', ct[line_start:call_match.start()]).group(0)
    inject_lines = []
    for lhs, rhs in timing_assignments:
        exact = re.compile(r'\b' + re.escape(lhs) + r'\s*=\s*' + re.escape(rhs) + r'\s*;')
        if not exact.search(ct):
            inject_lines.append(f"{leading}{lhs} = {rhs};\n")
    if inject_lines:
        ct = (ct[:line_start] +
              f"{leading}// {MARK}: ST7123 long-VBlank scanout pacing (~62 Hz).\n" +
              ''.join(inject_lines) + ct[line_start:])
        call_match = re.search(
            r'esp_lcd_new_panel_dpi\s*\(' r'(?:(?!\);).)*?' r'&\s*' + re.escape(cfg_name) + r'\b' r'(?:(?!\);).)*?\)',
            ct, re.S)
        if call_match is None:
            raise RuntimeError(f"lost esp_lcd_new_panel_dpi call after R18 timing patch in {cpp}")

    num2_pat = re.compile(r'\b' + re.escape(cfg_name) + r'\s*\.\s*num_fbs\s*=\s*2\s*;')
    if not num2_pat.search(ct):
        line_start = ct.rfind('\n', 0, call_match.start()) + 1
        leading = re.match(r'[ \t]*', ct[line_start:call_match.start()]).group(0)
        inject = (
            f"{leading}// {MARK}: request two driver-owned DPI framebuffers.\n"
            f"{leading}{cfg_name}.num_fbs = 2;\n"
        )
        ct = ct[:line_start] + inject + ct[line_start:]

    # Publish both allocated framebuffer pointers and the raw panel handle.
    if '_config_detail.buffer2 = px68k_fb1' not in ct:
        m_fb = re.search(r'esp_lcd_dpi_panel_get_frame_buffer\s*\(\s*([^,\n]+)\s*,', ct)
        if not m_fb:
            raise RuntimeError(f"existing esp_lcd_dpi_panel_get_frame_buffer call not found in {cpp}")
        handle_expr = m_fb.group(1).strip()

        call_line_end = ct.find('\n', m_fb.end())
        if call_line_end < 0:
            call_line_end = len(ct)
        else:
            call_line_end += 1
        line_start = ct.rfind('\n', 0, m_fb.start()) + 1
        indent2 = re.match(r'[ \t]*', ct[line_start:m_fb.start()]).group(0)
        inject2 = f'''{indent2}// {MARK}: retrieve both driver-owned PSRAM buffers.\n{indent2}{{\n{indent2}  void *px68k_fb0 = nullptr;\n{indent2}  void *px68k_fb1 = nullptr;\n{indent2}  if (ESP_OK == esp_lcd_dpi_panel_get_frame_buffer({handle_expr}, 2, &px68k_fb0, &px68k_fb1)) {{\n{indent2}    _config_detail.buffer = px68k_fb0;\n{indent2}    _config_detail.buffer2 = px68k_fb1;\n{indent2}    _config_detail.px68k_dpi_panel = {handle_expr};\n{indent2}  }}\n{indent2}}}\n'''
        ct = ct[:call_line_end] + inject2 + ct[call_line_end:]

    cpp.write_text(ct, encoding='utf-8')

    # Verify THIS exact candidate. Every pair must pass before build starts.
    ht2 = hpp.read_text(encoding='utf-8')
    ct2 = cpp.read_text(encoding='utf-8')
    checks = {
        'header buffer2': bool(re.search(r'\bvoid\s*\*\s*buffer2\b', ht2)),
        'header panel handle': 'px68k_dpi_panel' in ht2,
        'R18 78MHz request': bool(re.search(r'\b' + re.escape(cfg_name) + r'\s*\.\s*dpi_clock_freq_mhz\s*=\s*78\s*;', ct2)),
        'R18 VBP=4': bool(re.search(r'\b' + re.escape(cfg_name) + r'\s*\.\s*video_timing\.vsync_back_porch\s*=\s*4\s*;', ct2)),
        'R18 VPW=2': bool(re.search(r'\b' + re.escape(cfg_name) + r'\s*\.\s*video_timing\.vsync_pulse_width\s*=\s*2\s*;', ct2)),
        'R18 VFP=320': bool(re.search(r'\b' + re.escape(cfg_name) + r'\s*\.\s*video_timing\.vsync_front_porch\s*=\s*320\s*;', ct2)),
        'num_fbs assignment': bool(re.search(r'\b' + re.escape(cfg_name) + r'\s*\.\s*num_fbs\s*=\s*2\s*;', ct2)),
        'two-FB retrieval': bool(re.search(r'esp_lcd_dpi_panel_get_frame_buffer\s*\([^;]*?\b2\s*,\s*&px68k_fb0\s*,\s*&px68k_fb1', ct2, re.S)),
        'published buffer2': '_config_detail.buffer2 = px68k_fb1' in ct2,
        'published panel': '_config_detail.px68k_dpi_panel' in ct2,
    }
    failed = [k for k, ok in checks.items() if not ok]
    if failed:
        raise RuntimeError('post-patch verification failed: ' + ', '.join(failed))
    return cfg_name


patched = []
for hpp, cpp in pairs:
    try:
        cfg_name = patch_pair(hpp, cpp)
    except Exception as exc:
        die(str(exc))
    patched.append((hpp, cpp, cfg_name))

print(f"*** {MARK}: patched+verified {len(patched)} Panel_DSI candidate(s)")
for hpp, cpp, cfg_name in patched:
    print(f"*** {MARK}: config={cfg_name}.num_fbs=2 pclk_req=78MHz vblank=4/2/320 (~62Hz expected)")
    print(f"*** {MARK}: hpp={hpp}")
    print(f"*** {MARK}: cpp={cpp}")
