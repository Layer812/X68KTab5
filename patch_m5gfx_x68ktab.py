#!/usr/bin/env python3
from __future__ import annotations

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent
M5ROOT = ROOT / "components" / "M5GFX"
M5GFX_CPP = M5ROOT / "src" / "M5GFX.cpp"
PANEL_HPP = M5ROOT / "src" / "lgfx" / "v1" / "platforms" / "esp32p4" / "Panel_DSI.hpp"
PANEL_CPP = M5ROOT / "src" / "lgfx" / "v1" / "platforms" / "esp32p4" / "Panel_DSI.inl"

EXPECTED_COMMIT = "65c816a38386062d2c3dc47d41237a9083acbc75"
MARK = "X68KTAB_PANEL_COMPAT_R3"


def die(msg: str) -> None:
    print(f"ERROR {MARK}: {msg}", file=sys.stderr)
    raise SystemExit(1)


def require(cond: bool, msg: str) -> None:
    if not cond:
        die(msg)


class TextFile:
    def __init__(self, path: Path):
        self.path = path
        data = path.read_bytes()
        self.eol = "\r\n" if data.count(b"\r\n") > data.count(b"\n") // 2 else "\n"
        self.text = data.decode("utf-8").replace("\r\n", "\n")

    def write(self, text: str) -> None:
        data = text if self.eol == "\n" else text.replace("\n", "\r\n")
        self.path.write_bytes(data.encode("utf-8"))


def replace_once(src: str, old: str, new: str, label: str) -> str:
    count = src.count(old)
    require(count == 1, f"{label}: expected exactly one anchor, got {count}")
    return src.replace(old, new, 1)


def patch_between(src: str, start: str, end: str, old: str, new: str, label: str) -> str:
    i = src.find(start)
    require(i >= 0, f"{label}: start marker missing")
    j = src.find(end, i)
    require(j >= 0, f"{label}: end marker missing")
    chunk = src[i:j]
    require(chunk.count(old) == 1, f"{label}: expected one target in branch, got {chunk.count(old)}")
    chunk = chunk.replace(old, new, 1)
    return src[:i] + chunk + src[j:]


def patch_m5gfx(src: str) -> str:
    # Upstream 0.2.35 already retries ST7121/ST7123 and GT911 detection.
    # Keep official panel probing; only preserve device-tested DSI timing.
    require(src.count("for (int i = 0; i < 60; ++i)") == 1,
            "official ST touch probe has changed")
    require("bool found_gt911 = false;" in src,
            "official GT911 probe missing")

    src = replace_once(
        src,
        "            bus_cfg.lane_mbps = hit_st7121 ? 900 : 1040;\n",
        f"            // {MARK}: panel-specific Tab5 DSI link margin.\n"
        "            bus_cfg.lane_mbps = hit_st7121 ? 965 : (hit_st7123 ? 1040 : 730);\n",
        "DSI lane selector",
    )

    src = patch_between(
        src,
        "auto p = new Panel_ILI9881C();",
        "} else if (hit_st7121) {",
        "                det.dpi_freq_mhz = 80;\n",
        "                det.dpi_freq_mhz = 60;\n",
        "ILI9881C pixel clock",
    )

    src = patch_between(
        src,
        "auto p = new Panel_ST7123();",
        "if (_panel_last == nullptr) {",
        "                det.dpi_freq_mhz = 80;\n",
        "                det.dpi_freq_mhz = 78;\n",
        "ST7123 pixel clock",
    )
    src = patch_between(
        src,
        "auto p = new Panel_ST7123();",
        "if (_panel_last == nullptr) {",
        "                det.vsync_back_porch = 8;\n",
        "                det.vsync_back_porch = 4;\n",
        "ST7123 VBP",
    )
    src = patch_between(
        src,
        "auto p = new Panel_ST7123();",
        "if (_panel_last == nullptr) {",
        "                det.vsync_front_porch = 220;\n",
        "                det.vsync_front_porch = 320;\n",
        "ST7123 VFP",
    )
    return src


def patch_panel_hpp(src: str) -> str:
    src = replace_once(
        src,
        "#include <esp_lcd_mipi_dsi.h>\n",
        "#include <esp_lcd_mipi_dsi.h>\n#include <esp_lcd_panel_ops.h>\n",
        "Panel_DSI.hpp panel ops include",
    )
    src = replace_once(
        src,
        "      void* buffer = nullptr;\n",
        "      void* buffer = nullptr;\n"
        f"      // {MARK}: X68K Tab driver-owned second framebuffer and DPI handle.\n"
        "      void* buffer2 = nullptr;\n"
        "      esp_lcd_panel_handle_t px68k_dpi_panel = nullptr;\n",
        "Panel_DSI.hpp config_detail fields",
    )
    return src


def patch_panel_inl(src: str) -> str:
    src = replace_once(
        src,
        "#include <esp_lcd_panel_io.h>\n",
        "#include <esp_lcd_panel_io.h>\n"
        '#include "hal/mipi_dsi_host_ll.h"\n'
        '#include "soc/mipi_dsi_host_struct.h"\n',
        "Panel_DSI.inl LL includes",
    )
    # Upstream already allocates two DPI framebuffers.
    require(src.count("    dpi_config.num_fbs = 2;") == 1,
            "upstream double framebuffer allocation has changed")
    src = replace_once(
        src,
        "    auto ret = esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &_disp_panel_handle);\n",
        "    auto ret = esp_lcd_new_panel_dpi(mipi_dsi_bus, &dpi_config, &_disp_panel_handle);\n"
        f"    // {MARK}: ACK-off is used by the ST7121/ILI compatibility paths.\n"
        "    const bool x68ktab_st7123_known_good =\n"
        "        (_config_detail.hsync_back_porch == 40\n"
        "         && _config_detail.hsync_pulse_width == 2\n"
        "         && _config_detail.hsync_front_porch == 40\n"
        "         && _config_detail.vsync_back_porch == 4\n"
        "         && _config_detail.vsync_pulse_width == 2\n"
        "         && _config_detail.vsync_front_porch == 320);\n"
        "    if (ret == ESP_OK && !x68ktab_st7123_known_good)\n"
        "    {\n"
        "      mipi_dsi_host_ll_dpi_set_video_burst_type(\n"
        "          &MIPI_DSI_HOST, MIPI_DSI_LL_VIDEO_BURST_WITH_SYNC_PULSES);\n"
        "      mipi_dsi_host_ll_dpi_enable_frame_ack(&MIPI_DSI_HOST, false);\n"
        "    }\n",
        "Panel_DSI.inl ACK policy",
    )

    old = (
        "    if (init_dpi(bus) && init_panel())\n"
        "    {\n"
        "        esp_lcd_dpi_panel_get_frame_buffer(_disp_panel_handle, 1, &(_config_detail.buffer));\n"
        "    }\n"
    )
    new = (
        "    if (init_dpi(bus) && init_panel())\n"
        "    {\n"
        f"      // {MARK}: ST7121 needs the clock lane kept in HS on tested hardware.\n"
        "      const bool x68ktab_st7121 =\n"
        "          (_config_detail.hsync_back_porch == 40\n"
        "           && _config_detail.hsync_pulse_width == 2\n"
        "           && _config_detail.hsync_front_porch == 40\n"
        "           && _config_detail.vsync_back_porch == 24\n"
        "           && _config_detail.vsync_pulse_width == 20\n"
        "           && _config_detail.vsync_front_porch == 200);\n"
        "      if (x68ktab_st7121)\n"
        "      {\n"
        "        mipi_dsi_host_ll_set_clock_lane_state(\n"
        "            &MIPI_DSI_HOST, MIPI_DSI_LL_CLOCK_LANE_STATE_HS);\n"
        "      }\n"
        "\n"
        f"      // {MARK}: publish both driver-owned framebuffers to the presenter.\n"
        "      void *px68k_fb0 = nullptr;\n"
        "      void *px68k_fb1 = nullptr;\n"
        "      _config_detail.px68k_dpi_panel = _disp_panel_handle;\n"
        "      if (ESP_OK == esp_lcd_dpi_panel_get_frame_buffer(\n"
        "              _disp_panel_handle, 2, &px68k_fb0, &px68k_fb1))\n"
        "      {\n"
        "        _config_detail.buffer = px68k_fb0;\n"
        "        _config_detail.buffer2 = px68k_fb1;\n"
        "      }\n"
        "      else\n"
        "      {\n"
        "        esp_lcd_dpi_panel_get_frame_buffer(\n"
        "            _disp_panel_handle, 1, &(_config_detail.buffer));\n"
        "        _config_detail.buffer2 = nullptr;\n"
        "      }\n"
        "    }\n"
    )
    src = replace_once(src, old, new, "Panel_DSI.inl init/DoubleFB block")
    return src


def branch(src: str, start: str, end: str) -> str:
    i = src.find(start)
    j = src.find(end, i)
    require(i >= 0 and j >= 0, f"audit branch missing: {start}")
    return src[i:j]


def audit(m5: str, hpp: str, cpp: str) -> None:
    require(MARK in m5 and MARK in hpp and MARK in cpp, "patch marker missing")
    require(m5.count("for (int i = 0; i < 60; ++i)") == 1,
            "upstream touch retry must remain")
    require("bool found_gt911 = false;" in m5,
            "upstream GT911 detection must remain")
    require(m5.count("bus_cfg.lane_mbps = hit_st7121 ? 965 : (hit_st7123 ? 1040 : 730);") == 1,
            "panel-specific DSI lane selector missing")

    ili = branch(m5, "auto p = new Panel_ILI9881C();", "} else if (hit_st7121) {")
    st1 = branch(m5, "auto p = new Panel_ST7121();", "} else if (hit_st7123) {")
    st3 = branch(m5, "auto p = new Panel_ST7123();", "if (_panel_last == nullptr) {")
    for token in (
        "det.dpi_freq_mhz = 60;", "det.hsync_back_porch = 140;",
        "det.hsync_pulse_width = 40;", "det.hsync_front_porch = 40;",
        "det.vsync_back_porch = 20;", "det.vsync_pulse_width = 4;",
        "det.vsync_front_porch = 20;",
    ):
        require(token in ili, f"ILI audit missing {token}")
    for token in (
        "det.dpi_freq_mhz = 70;", "det.hsync_back_porch = 40;",
        "det.hsync_pulse_width = 2;", "det.hsync_front_porch = 40;",
        "det.vsync_back_porch = 24;", "det.vsync_pulse_width = 20;",
        "det.vsync_front_porch = 200;",
    ):
        require(token in st1, f"ST7121 audit missing {token}")
    for token in (
        "det.dpi_freq_mhz = 78;", "det.hsync_back_porch = 40;",
        "det.hsync_pulse_width = 2;", "det.hsync_front_porch = 40;",
        "det.vsync_back_porch = 4;", "det.vsync_pulse_width = 2;",
        "det.vsync_front_porch = 320;",
    ):
        require(token in st3, f"ST7123 audit missing {token}")

    require(hpp.count("void* buffer2 = nullptr;") == 1, "buffer2 field missing/duplicated")
    require(hpp.count("esp_lcd_panel_handle_t px68k_dpi_panel = nullptr;") == 1,
            "raw DPI panel field missing/duplicated")
    require(cpp.count("dpi_config.num_fbs = 2;") == 1, "num_fbs=2 missing/duplicated")
    require("dpi_config.num_fbs = 1;" not in cpp, "num_fbs=1 remains")
    require(cpp.count("mipi_dsi_host_ll_dpi_enable_frame_ack(&MIPI_DSI_HOST, false);") == 1,
            "frame ACK-off policy missing/duplicated")
    require(cpp.count("MIPI_DSI_LL_CLOCK_LANE_STATE_HS") == 1,
            "ST7121 HS clock-lane policy missing/duplicated")
    require(cpp.count("_disp_panel_handle, 2, &px68k_fb0, &px68k_fb1") == 1,
            "DoubleFB retrieval missing/duplicated")
    require(cpp.count("_config_detail.px68k_dpi_panel = _disp_panel_handle;") == 1,
            "raw DPI panel publish missing/duplicated")


def git_head() -> str:
    p = subprocess.run(
        ["git", "-C", str(M5ROOT), "rev-parse", "HEAD"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    require(p.returncode == 0,
            "components/M5GFX is not an initialized git submodule; run git submodule update --init --recursive")
    return p.stdout.strip()


def main() -> int:
    for path in (M5GFX_CPP, PANEL_HPP, PANEL_CPP):
        require(path.is_file(), f"missing {path}; initialize submodules first")

    head = git_head()
    require(head == EXPECTED_COMMIT,
            f"M5GFX commit mismatch: expected {EXPECTED_COMMIT}, got {head}")

    m5f, hppf, cppf = TextFile(M5GFX_CPP), TextFile(PANEL_HPP), TextFile(PANEL_CPP)
    m5, hpp, cpp = m5f.text, hppf.text, cppf.text

    marks = [MARK in m5, MARK in hpp, MARK in cpp]
    if all(marks):
        audit(m5, hpp, cpp)
        print(f"PASS {MARK}: already applied and verified")
        return 0
    require(not any(marks), "partial panel compatibility patch detected; restore the pinned M5GFX submodule first")

    new_m5 = patch_m5gfx(m5)
    new_hpp = patch_panel_hpp(hpp)
    new_cpp = patch_panel_inl(cpp)
    audit(new_m5, new_hpp, new_cpp)

    # All guards and audits pass in memory before touching the submodule.
    m5f.write(new_m5)
    hppf.write(new_hpp)
    cppf.write(new_cpp)

    # Reread and verify the written state.
    audit(TextFile(M5GFX_CPP).text, TextFile(PANEL_HPP).text, TextFile(PANEL_CPP).text)
    print(f"PASS {MARK}: patched pinned M5GFX {EXPECTED_COMMIT}")
    print("  ST7121 : 965 Mbps / 70 MHz / H40-2-40 / V24-20-200 / ACK off / clock HS")
    print("  ST7123 : 1040 Mbps / 78 MHz / H40-2-40 / V4-2-320 / default ACK / clock AUTO")
    print("  ILI9881C: 730 Mbps / 60 MHz / H140-40-40 / V20-4-20 / ACK off")
    print("  DoubleFB: num_fbs=2, buffer2 + raw DPI handle published")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
