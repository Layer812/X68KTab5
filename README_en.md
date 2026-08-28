# X68K Tab

[**日本語 README → README.md**](README.md)

<p align="center">
  <img src="./x68t.gif" alt="X68K Tab running on M5Stack Tab5" width="760">
</p>

<p align="center">
  <b>X68000 Emulator for M5Stack Tab5 / ESP32-P4</b><br>
  Developed by Layer812 &nbsp;|&nbsp; Based on PX68K &nbsp;|&nbsp; 68000 core: Musashi
</p>

**X68K Tab** is a portable X68000 emulator for the M5Stack Tab5, powered by the ESP32-P4.
It is based on PX68K and Musashi, but reorganized around the ESP32-P4's **two HP CPU cores plus the LP Core**, PSRAM, MIPI-DSI display, USB Host, microSD and integrated audio.

It originally started with a simple idea: I wanted to play old X68000 **PANIC** data on the M5Stack Tab5, so I began building [**PanicPlayerTab5**](https://github.com/Layer812/PanicPlayerTab5).

To play PANIC properly, I gradually implemented the X68000-compatible environment it needed. There was just one problem.

**I didn’t actually have any PANIC data.**

So, for the sake of all that **PANIC data I had yet to find**, I kept pushing the CPU, graphics, audio, I/O and memory-transfer paths further and further...

**and ended up with a multi-node cooperative X68000 emulator, with CPU1, the LP Core and CPU0 working asynchronously together.**

> [!IMPORTANT]
> Not every X68000 title is fully compatible yet. If you find a display, audio or USB compatibility problem, please report it **gently**, ideally with reproduction steps and a serial log. Issues are welcome.

---

## Quick Start — M5Burner

- [M5Burner official page](https://docs.m5stack.com/en/uiflow/m5burner/intro)
- [M5Stack downloads](https://docs.m5stack.com/en/download)

Open **Share Burn** in M5Burner and enter one of the Share Codes below.

| Version | Share Code | Notes |
|---|---|---|
| **Latest / Production** | `qUbdr77ZmhX8Esgo` | **Recommended for normal use** |
| Previous public build | `pfDbZl26Z3MsI3wP` | Previous public release / comparison |
| Older public build | `aFmGCMA3FSvzcW5H` | Kept for comparison / compatibility checks |
| Legacy public build | `xX5zvurDW6xMacAK` | Older public image |

Older Share Codes are intentionally kept available so users can compare behavior or roll back when checking compatibility.

Demo:

- [X68K Tab running on real Tab5 hardware (X / @Layer812)](https://x.com/layer812/status/2089625598687891632)

---

## What You Need

- **M5Stack Tab5** — [official documentation](https://docs.m5stack.com/en/core/Tab5)
- **microSD card**
- X68000 software / disk images you are legally allowed to use
  - FDD: `XDF`, `DIM`
  - HDD: `HDS`

Useful extras:

- USB keyboard
- USB Joypad / Gamepad
- USB mouse

The touch UI can also be used without external input devices.

---

## Build 6.15 Production — Highlights

Features and release-history notes are combined here. This describes the paths that remain in the current Production build rather than old experimental branches.

| Area | Production design |
|---|---|
| **68000** | Musashi + TCM / Internal SRAM dispatch caches + measured hot-path / inline fast paths |
| **CPU1** | Prioritizes the **X68000 guest time domain**: 68000, interrupts, timers and guest-side DMA events, decoupled from temporary host-side stalls |
| **CPU0** | Handles final host-side work: composition, LCD, YM2151, final audio mix, USB, SD / Flash / HostFS and host UI |
| **LP Core** | Async Broker for Notify / ACK / dirty / workset / metadata, with **no direct guest-memory access** |
| **Inter-core** | no-wait / latest-wins event journal, shadow and mailbox-style pipelines |
| **Graphics** | CPU0 compositor and GRP8 paired-page shared-scroll; PIE / XespV / PPA / DMA accelerate rendering and block processing |
| **LCD** | Managed Double-FB + dirty-tile / partial updates + `refresh_done` synchronization + latest-frame priority |
| **Audio** | guest-timed ADPCM on CPU1 + YM2151 / final mix on CPU0, using a vgmM5-family backend in a 44.1 kHz synthesis domain |
| **Storage** | microSD / Flash / HostFS, XDF / DIM / HDS, Human68k boot |
| **Input** | USB Keyboard / Joypad / Mouse + Touch UI |
| **PANIC** | Integrated PanicPlayer with automatic PANIC startup after Human68k boot |

The many internal measurement-build names are intentionally omitted from the public README; the public release is presented simply as **Build 6.15 Production**.

---

## ESP32-P4 Multi-Core Architecture

<p align="center">
  <img src="./x68ktab_emulation_block_en.png" alt="X68K Tab emulation block architecture" width="1100">
</p>

X68K Tab does not put the entire emulator into one large synchronous loop. It separates **guest time, asynchronous coordination and host output**. CPU1 publishes state changes through the Ordered Shadow / Event Journal, the LP Core handles lightweight control/notification work, and CPU0 owns the final video, audio and I/O work.

- **HP CPU1 — Guest Time Domain**  
  Keeps X68000-side time moving: 68000 execution, LSI state, interrupts, timers, guest-side DMA and ADPCM events. A central design goal is to avoid stopping guest time just because host-side work is temporarily late.

- **HP CPU0 — Host Processing**  
  Handles Tab5-side peripherals and final output: composition, LCD, FM, final audio mix, USB, SD, HostFS and touch UI.

- **LP Core — Async Broker**  
  Processes lightweight `Notify / ACK / dirty / workset / metadata` messages. It does not directly walk the large X68000 guest-memory space; instead it assists asynchronous coordination between the HP cores.

The key rule is simple: **temporary host-side work should not unnecessarily stop the guest timeline on CPU1.**

---

## PANIC Player

X68K Tab grew out of [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5).
The PANIC Player remains integrated, so `.PAN` data can be selected and played directly.

If you still have old HDDs, MO disks, CD-Rs or backups containing PANIC data or even old file listings, information is very welcome.

---

## CGROM / ROM / Human68k

Instead of redistributing an original X68000 CGROM dump, X68K Tab uses [`build_cgrom.py`](build_cgrom.py) to generate compatible CGROM data from **freely redistributable fonts**. Please follow the license of the font you use.

Copyright in games, operating systems, ROMs and disk images remains with their respective rights holders. Please use software that you are legally entitled to use or that is distributed under appropriate terms.

When building from source, obtain any required Human68k-related files yourself under the applicable terms. See [`LICENSE_SHARP_X68000.txt`](LICENSE_SHARP_X68000.txt).

---

## Building from Source

Current development is centered on ESP-IDF / M5Unified and is heavily tuned for the ESP32-P4. For most users, the M5Burner image is the easiest way to run X68K Tab.

Build settings and required local files may evolve. When building the source tree, follow the configuration and comments included in the repository.

---

## Credits / License

X68K Tab stands on decades of emulator, hardware-research and open-source work. Thank you to everyone involved.

- [PX68K](https://github.com/hissorii/px68k)
- [Musashi](https://github.com/kstenerud/Musashi)
- [vgmM5](https://github.com/Layer812/vgmM5)
- [M5Stack](https://docs.m5stack.com/en/core/Tab5)
- Espressif ESP32-P4 / ESP-IDF

For license and third-party details, see:

- [`THIRD_PARTY_NOTICES.md`](THIRD_PARTY_NOTICES.md)
- [`LICENSE_SHARP_X68000.txt`](LICENSE_SHARP_X68000.txt)
- [`LICENSE_PANIC_X.txt`](LICENSE_PANIC_X.txt)
- [`PANIC_V1.38_NOTICE.txt`](PANIC_V1.38_NOTICE.txt)

**X68K Tab is an independent unofficial personal project and is not an official, endorsed or sponsored project of SHARP, M5Stack or other respective rights holders.**
