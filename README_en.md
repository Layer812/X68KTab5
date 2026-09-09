# X68K Tab

[**日本語 README → README.md**](README.md)

<p align="center">
  <img src="./x68t.gif" alt="X68K Tab running on M5Stack Tab5" width="760">
</p>

<p align="center">
  <b>X68000 Emulator for M5Stack Tab5 / ESP32-P4</b><br>
  Developed by Layer812 &nbsp;|&nbsp; Based on PX68K &nbsp;|&nbsp; 68000 core: Musashi
</p>

## What is X68K Tab?

**X68K Tab** is a portable X68000 emulator for the M5Stack Tab5, powered by the ESP32-P4.

It is based on PX68K and Musashi, but reorganized around the ESP32-P4's two HP CPU cores, LP Core, PSRAM, MIPI-DSI display, USB Host, microSD, and integrated audio.

The project started with a much smaller idea: I wanted to play old X68000 **PANIC** data on the M5Stack Tab5, so I began building [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5).

To play PANIC properly, I gradually implemented the X68000-compatible environment it needed. There was just one problem:

**I only had one piece of PANIC data.....**  
So, for the sake of all the PANIC data I had yet to find, I kept pushing the CPU, graphics, audio, I/O, and memory-transfer paths until the project became an X68000 emulator where CPU1, the LP Core, and CPU0 cooperate asynchronously.

> [!IMPORTANT]
> X68K Tab does not guarantee complete compatibility with every X68000 title.  
> If you find a display, audio, input, USB, or disk problem, please open an Issue with reproduction steps and a serial log if possible.

---

## Current Production

The current Production configuration uses a **12 MHz 68000 guest CPU** with an **exact 10 MHz X68000 peripheral time domain**.

### Release policy

This build is the **current official X68K Tab release**.

The core feature set, performance, audio, display, input, and storage paths are now being treated as a Production baseline. Future development will primarily focus on **compatibility issues, regressions, and bug fixes** found with real X68000 software and peripherals rather than continually changing the basic architecture.

Improvements required for compatibility or stability will of course continue, but the current 12 MHz / exact 10 MHz / no-wait configuration is the reference baseline.

| Area | Production design |
| --- | --- |
| Guest CPU | **12 MHz** |
| Peripheral domain | **exact 10 MHz** |
| Guest RAM | **12 MiB** |
| CPU1 | Prioritizes 68000 execution and guest-device time; it is not made to wait for host-side work |
| CPU0 | Video / LCD / YM2151 / final audio mix / USB / storage / host UI |
| Inter-core | Asynchronous no-wait / latest-wins pipelines |
| Graphics | Sparse dirty updates written only where needed to the current Front FB |
| Audio | guest-timed ADPCM + CPU0 YM2151 / final mix |
| Storage | microSD / Flash / HostFS, XDF / DIM / HDS |
| Input | USB Keyboard / Joypad / Mouse + Touch UI |
| PANIC | Integrated PanicPlayer and PANIC data launch support |

Internal measurement-build names and A/B experiment names are intentionally omitted from the public README.

### Turbo

The on-screen `TURBO` button has three stages.

| Indicator | Mode | Audio source rate | Video |
| --- | --- | ---: | --- |
| BLACK | NORMAL | 44.1 kHz | normal |
| GREEN | TURBO | 22.05 kHz | adaptive 15 / 20 / 24 fps |
| RED | RED TURBO | 11.025 kHz | stronger host-work shedding / MAX 24 fps |

Turbo **does not change the guest CPU clock**. The 68000 remains at 12 MHz and the peripheral domain remains exact 10 MHz.

---

## Quick Start — M5Burner

- [M5Burner official page](https://docs.m5stack.com/en/uiflow/m5burner/intro)
- [M5Stack downloads](https://docs.m5stack.com/en/download)

Open **Share Burn** in M5Burner and enter a Share Code.

| Version | Share Code | Notes |
| --- | --- | --- |
| **Latest / Production Release** | `KJL8QIk1H35dtT7O` | **Current release / recommended for normal use** |
| Previous public build | `wUgYltOEbYF7mrBn` | Previous public release / comparison |
| Older public build | `qUbdr77ZmhX8Esgo` | Comparison / compatibility checks |
| Older public build | `pfDbZl26Z3MsI3wP` | Comparison / compatibility checks |
| Older public build | `aFmGCMA3FSvzcW5H` | Comparison / compatibility checks |
| Legacy public build | `xX5zvurDW6xMacAK` | Older public image |

> When publishing a new M5Burner image, pass the new Share Code as the second argument to the included public-folder preparation script. It updates the Latest row in both Japanese and English READMEs.

---

## What You Need

- [M5Stack Tab5](https://docs.m5stack.com/en/core/Tab5)
- microSD card
- X68000 software / disk images that you are legally entitled to use
  - FDD: `XDF`, `DIM`
  - HDD: `HDS`

### Input devices

- [**Keyboard for M5Stack Tab5**](https://www.switch-science.com/products/11257)
- USB keyboard
- USB Joypad / Gamepad
- USB mouse
- On-screen Touch UI / virtual keyboard / Joypad

Basic operation is possible from the touch UI even without external input devices.

---

## Display / MULTISCAN

X68K Tab automatically detects guest CRTC modes corresponding to **15 kHz / 24 kHz / 31 kHz-class operation** and converts them to the Tab5's fixed 1280×720 LCD output.

It does not electrically output the original X68000 scan frequency. Guest video modes are converted internally for the Tab5 display.

---

## Audio

CPU1 advances guest-timed X68000 audio events, while CPU0 handles YM2151 waveform generation, the final mix with ADPCM, and speaker output.

The design prioritizes keeping the guest timeline moving even when host-side display work is temporarily heavy. Audio Guard can reduce host visual work when audio reserve becomes low.

---

## ESP32-P4 Multi-Core Architecture

<p align="center">
  <img src="./x68ktab_emulation_block_en.png" alt="X68K Tab ESP32-P4 multi-core architecture" width="1100">
</p>

- **HP CPU1 — Guest Time Domain**  
  Prioritizes X68000-side time: 68000 execution, interrupts, timers, guest-side DMA, CRTC, and audio events.

- **HP CPU0 — Host Processing**  
  Handles composition, LCD, YM2151, final audio mix, USB, SD / Flash / HostFS, and Touch UI.

- **LP Core — Lightweight Broker**  
  Assists selected lightweight notification / metadata / broker work. It is not used to walk large guest-memory regions directly.

The central rule is simple: **temporary host-side delays should not unnecessarily stop the guest timeline on CPU1.**

---

## PANIC Player

X68K Tab grew out of [PanicPlayerTab5](https://github.com/Layer812/PanicPlayerTab5). PANIC Player functionality remains integrated.

### Looking for PANIC data

If you still have old X68000 media, information about any of the following is very welcome:

- `.PAN` files
- LZH / ZIP archives containing PANIC data
- MO / HDD / CD-R backups
- BBS file listings
- README / DOC files
- remembered filenames

If copyright or other restrictions make it difficult to share the data itself, **even a filename or directory listing would be very helpful.**

---

## ROM / CGROM / Human68k

This repository does **not** include original SHARP X68000 ROM dumps, Human68k disk images, or user-owned X68000 software.

Instead of redistributing an original CGROM dump, X68K Tab provides `build_cgrom.py`. Please follow the license and rights conditions of the fonts and generated data you use.

If Human68k-related files are required, obtain them yourself under the applicable terms.

See:

- `LICENSE_SHARP_X68000.txt`
- `LICENSE_PANIC_X.txt`
- `PANIC_V1.38_NOTICE.txt`
- `THIRD_PARTY_NOTICES.md`

---

## Building from Source

Development and hardware validation are centered on ESP32-P4 / M5Stack Tab5.

Current Production baseline:

- ESP-IDF 5.5.x family
- ESP32-P4 360 MHz
- 32 MiB PSRAM / 200 MHz
- QIO Flash / 80 MHz
- M5Unified / M5GFX
- Musashi
- PX68K

The source tree contains PlatformIO / ESP-IDF configuration files. Do not add locally obtained ROM, OS, or disk-image files to the public repository.

---

## Credits / License

X68K Tab stands on decades of emulator, hardware-research, and open-source work.

- PX68K
- Musashi
- vgmM5
- M5Stack / M5Unified / M5GFX
- Espressif ESP32-P4 / ESP-IDF

Please see the license and third-party notice documents included in this repository.

X68K Tab is an independent unofficial personal project. It is not an official, endorsed, or sponsored project of SHARP, M5Stack, or the other respective rights holders.
