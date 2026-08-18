# Third-Party Notices / 第三者ソフトウェアに関する表示

This file records third-party software, source code, libraries, system software,
and other materials used by or referenced from **X68K Tab**.

このファイルは **X68K Tab** が利用・参照している第三者ソフトウェア、ソースコード、
ライブラリ、システムソフトウェア等について整理したものです。

> [!IMPORTANT]
> This document is an attribution/notice index. It does **not** replace the
> original copyright notices or license texts shipped with each component.
>
> 本文書は謝辞・ライセンス情報を整理するための一覧です。各コンポーネントに付属する
> **オリジナルの著作権表示・ライセンス本文を置き換えるものではありません**。
>
> When the original source file, license file, README, or distribution notice
> differs from this summary, the original text takes precedence.
>
> README の要約と原文が異なる場合は、必ず原文を優先してください。

---

## 1. PX68K

**Project:** PX68K — Portable SHARP X68000 Emulator  
**Upstream:** https://github.com/hissorii/px68k

X68K Tab is based on PX68K and contains substantial code derived from the PX68K
source tree.

PX68K itself is the result of a long X68000 emulator development lineage. Its
upstream documentation credits, among others:

- WinX68k / "keropi"
- xkeropi
- MC68000 emulator implementations used by PX68K
- fmgen
- many additional contributors, ports, and platform-specific implementations

X68K Tab preserves this history and the original notices found in the inherited
source files and documentation.

### Important licensing note

The PX68K source tree contains code originating from multiple projects and
authors. This `THIRD_PARTY_NOTICES.md` therefore does **not** claim that one
single license covers every file under `components/px68k/`.

For redistributed source:

- keep original copyright and license headers;
- keep relevant upstream README / notice files;
- do not remove attribution comments from inherited source;
- check the terms applying to each inherited module before changing its
  redistribution model.

The upstream PX68K README should be consulted together with the notices embedded
in the individual source files.

---

## 2. WinX68k / xkeropi and the X68000 emulator lineage

PX68K documents that it incorporates work descended from the WinX68k and
xkeropi projects.

X68K Tab thanks the original authors, porters, reverse engineers, documenters,
and maintainers who made these implementations available.

Because the exact provenance and terms can differ by source file, X68K Tab
preserves the original headers and accompanying documentation rather than
assigning a new blanket license to those files.

---

## 3. Musashi

**Project:** Musashi  
**Author:** Karl Stenerud and contributors  
**Upstream:** https://github.com/kstenerud/Musashi  
**License:** MIT License

X68K Tab includes and/or builds Musashi-derived Motorola 680x0 emulation code
under:

```text
components/px68k/m68000/musashi/
```

The Musashi project is distributed under the MIT License.

### Musashi MIT notice

Copyright (c) Karl Stenerud

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is furnished
to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

### SoftFloat files bundled with the Musashi tree

The X68K Tab source tree also contains SoftFloat-related files below the
Musashi directory, including:

```text
components/px68k/m68000/musashi/softfloat/
```

Those files may carry notices and terms separate from the Musashi MIT license.
Their original headers and `README.txt` must be retained.

**Do not assume that the Musashi MIT license replaces a license or notice
contained inside the SoftFloat-derived files.**

---

## 4. Other 68000 cores / legacy CPU sources

The PX68K source tree may also contain alternative or historical 68000 CPU
cores, including directories/files such as:

```text
components/px68k/m68000/c68k/
components/px68k/m68000/cyclone.*
```

Even when those implementations are not selected by the current X68K Tab
build, their presence in a public source repository means their original
copyright and licensing notices must be preserved.

If an unused legacy core is removed before publication, its notice is no
longer required for files that are not distributed. If it remains in the
repository, keep its original documentation and headers.

---

## 5. fmgen

**Project / component:** fmgen  
**Original author credited by PX68K:** cisc  
**Location in X68K Tab:**

```text
components/px68k/fmgen/
```

PX68K includes fmgen-derived FM/PSG audio code and explicitly directs users to
the original documentation in:

```text
components/px68k/fmgen/readme.txt
```

X68K Tab modifies and replaces parts of the audio path for ESP32-P4, but legacy
fmgen code remains important for compatibility and fallback behavior.

The original copyright notices, source headers, and `fmgen/readme.txt` must be
preserved. This notice intentionally does not assign a new license to fmgen
code.

---

## 6. vgmM5 YM2151 engine

**Project:** vgmM5  
**Author / maintainer:** Layer812  
**Upstream:** https://github.com/Layer812/vgmM5  
**Reference implementation:** `src/fm_engine.c` in vgmM5  
**License:** MIT License for the original vgmM5 source/modifications described
by the upstream project

vgmM5 is another project developed by Layer812, the author of X68K Tab.
It is listed here because it is a separate codebase with its own license and
provenance, even though the projects share the same author.

X68K Tab contains an ESP32-P4-specific YM2151 backend adapted from the optimized
YM2151 path in vgmM5.

Relevant X68K Tab files include:

```text
components/px68k/fmgen/vgmm5_ym2151.cpp
components/px68k/fmgen/vgmm5_ym2151.h
components/px68k/fmgen/VGMM5_YM2151_NOTICE.md
```

The X68K Tab backend uses the YM2151 waveform-generation ideas and fixed-point
hot path while retaining PX68K-side guest-visible timer/status/IRQ behavior.

The project-specific notice in:

```text
components/px68k/fmgen/VGMM5_YM2151_NOTICE.md
```

must be kept with the source.

### MIT license text applicable to MIT-licensed vgmM5-derived work

Permission is hereby granted, free of charge, to any person obtaining a copy
of the MIT-licensed software and associated documentation files (the
"Software"), to deal in the Software without restriction, including without
limitation the rights to use, copy, modify, merge, publish, distribute,
sublicense, and/or sell copies of the Software, and to permit persons to whom
the Software is furnished to do so, subject to the following conditions:

The applicable copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

For the authoritative copyright statement and current license, consult the
vgmM5 repository itself.

---

## 7. M5Unified

**Project:** M5Unified  
**Copyright:** M5Stack  
**Upstream:** https://github.com/m5stack/M5Unified  
**License:** MIT License

X68K Tab uses M5Unified for M5Stack hardware integration.

The upstream project states that M5Unified is licensed under the MIT License.
The original M5Unified `LICENSE` file and copyright notices should be retained
when source from that project is redistributed.

M5Stack, M5Stack Tab5, M5Unified, M5GFX, M5Burner and related names are owned
by their respective rights holders.

**X68K Tab is an independent project and is not affiliated with or endorsed by
M5Stack Technology Co., Ltd.**

---

## 8. M5GFX and bundled M5GFX third-party components

**Project:** M5GFX  
**Copyright:** M5Stack  
**Upstream:** https://github.com/m5stack/M5GFX  
**Top-level license:** MIT License

X68K Tab uses M5GFX through the M5Stack graphics stack.

The M5GFX project itself is MIT-licensed, but its upstream distribution also
contains components and fonts under additional licenses. The M5GFX project
currently documents, among others:

- LovyanGFX — FreeBSD-style license
- TJpgDec — original ChaN terms
- Pngle — MIT
- QRCode code — MIT
- Adafruit GFX / GLCD fonts — 2-clause BSD
- Bodmer fonts — FreeBSD-style terms
- converted IPA font — IPA Font License Agreement
- efont — 3-clause BSD
- TomThumb font — 3-clause BSD

The exact list can change with the M5GFX version. When redistributing an X68K
Tab binary, preserve the license/attribution material shipped with the exact
M5GFX version used for the release.

In particular, do not remove license files associated with bundled fonts.

---

## 9. ESP-IDF

**Project:** Espressif IoT Development Framework (ESP-IDF)  
**Copyright:** Espressif Systems (Shanghai) Co., Ltd. and contributors  
**Upstream:** https://github.com/espressif/esp-idf  
**License:** Apache License 2.0 (with individual files/components carrying their
own SPDX identifiers where applicable)

X68K Tab is built on ESP-IDF and uses FreeRTOS integration, drivers, DMA,
memory-management facilities, storage, USB support, and other ESP32-P4
platform services.

The authoritative terms are the license and per-file SPDX notices distributed
with the ESP-IDF version used to build the firmware.

Apache License 2.0:
https://www.apache.org/licenses/LICENSE-2.0

---

## 10. Espressif USB Host HID

**Component:** `espressif/usb_host_hid`  
**Upstream:** https://github.com/espressif/esp-usb/tree/master/host/class/hid/usb_host_hid  
**ESP Component Registry:** https://components.espressif.com/components/espressif/usb_host_hid  
**License:** Apache License 2.0

X68K Tab uses the Espressif USB Host HID component for USB keyboard, mouse,
gamepad/joystick, and related HID input work.

The component is downloaded by the ESP-IDF Component Manager and normally
appears locally as:

```text
managed_components/espressif__usb_host_hid/
```

That generated/downloaded directory does not need to be committed to the X68K
Tab repository, but its license obligations still apply to distributions that
include the component in the firmware.

Keep or reproduce the notices required by the Apache License 2.0 for binary
redistribution.

---

## 11. SHARP X68000 system software

X68000 was developed by SHARP Corporation.

Some X68K Tab distributions may use or include X68000 system software that was
made available by SHARP through the SHARP Products Users Forum (FSHARP) under
specific permission terms.

This material is **not** placed under the X68K Tab project license.

For source builds of X68K Tab, `human302.xdf` is expected as a **local build
input in the repository root**. It is intentionally excluded from Git and is
not intended to be supplied as ordinary X68K Tab project source.

```text
X68K-Tab/
├── human302.xdf
└── LICENSE_SHARP_X68000.txt
```

Users must obtain and use `human302.xdf` under the applicable SHARP
distribution/permission terms.

The authoritative terms must be kept separately as:

```text
LICENSE_SHARP_X68000.txt
```

When SHARP-originated system software is embedded in a generated C source,
firmware image, or other transformed representation, the transformed form
does not erase the original rights or permission conditions.

For example, review the origin and applicable permission before distributing
generated/embedded files such as an IPL ROM byte array.

Reference:
https://retropc.net/x68000/software/sharp/

SHARP, X68000, Human68k, and related names/software remain the property of
SHARP Corporation and/or their respective rights holders.

**X68K Tab is an unofficial independent project and is not affiliated with,
endorsed by, or sponsored by SHARP Corporation.**

---

## 12. PANIC / PANIC.X V1.38

X68K Tab grew out of the PanicPlayerTab5 project and includes PANIC playback
support. The firmware embeds the same original `panic.x` V1.38 player payload
used by PanicPlayerTab5.

The preserved PANIC V1.38 documentation identifies PANIC as originally written
by **Hideya Nagata (pako / ぱこたん / 永田英哉)**. `panic.x` V1.38 is based on pako's
V1.34 and includes modifications by **Nashimi (なしみ)**. Copyright in PANIC
remains with Hideya Nagata (pako).

The original V1.38 distribution documentation states that PANIC may be used,
redistributed, modified, and used commercially. The original documentation is
the authority for those permissions; this file is only a summary/index.

Keep the following X68K Tab redistribution files with public source/binary
releases containing PANIC.X:

```text
LICENSE_PANIC_X.txt
PANIC_V1.38_NOTICE.txt
```

Where practical, also retain the original PANIC V1.38 documentation distributed
with the player.

A generated C array or embedded binary representation does not change the
copyright or redistribution conditions of PANIC.X.

Individual `.PAN` animation/data files are separate works. Their copyright and
redistribution conditions belong to their respective creators and are not
covered automatically by the PANIC.X permission.

References:

- PanicPlayerTab5: https://github.com/Layer812/PanicPlayerTab5
- Original archive/documentation: https://retropc.net/x68000/software/movie/panic/panic/

---

## 13. CGROM generation and fonts

X68K Tab is designed so that a compatible CGROM can be generated from a
redistributable/free font rather than requiring distribution of an original
X68000 CGROM dump.

The generator itself can be distributed separately, for example:

```text
build_cgrom.py
```

The generated CGROM is still subject to the license of the **font actually used
as its input**.

Before publishing a pre-generated CGROM:

1. record the exact font name and version;
2. include the font's license/attribution;
3. verify that redistribution of a transformed/generated bitmap representation
   is permitted;
4. do not describe a font merely as "free" without retaining its actual
   license terms.

If the generated CGROM is not distributed by the repository, users can instead
generate it locally from a suitable font.

Original SHARP CGROM dumps are not relicensed by X68K Tab.

---

## 14. User-supplied X68000 software and disk images

XDF, DIM, HDS/HDF and other user software/media are **not part of the X68K Tab
project license** merely because the emulator can load them.

Commercial games, applications, operating systems, ROM dumps, disk images,
music, PANIC data, and other user content remain subject to their respective
copyright and distribution terms.

The X68K Tab source repository should not contain user-owned or commercial disk
images unless redistribution is explicitly permitted.

---

## 15. Build-generated dependency directories

The local development tree may contain generated/downloaded directories such
as:

```text
.pio/
managed_components/
```

These directories are normally excluded from Git because they can be
reconstructed by PlatformIO / ESP-IDF dependency management.

Excluding them from Git does **not** remove the obligation to honor the licenses
of the libraries linked into a distributed firmware image.

For reproducible releases, keep the dependency lock/configuration files needed
to identify the versions used, and preserve the relevant notices for those
versions.

---

## 16. X68K Tab original code

Code written specifically for X68K Tab is covered by the project license stated
in the repository's top-level:

```text
LICENSE
```

unless a file explicitly says otherwise.

A file derived from third-party source remains subject to the applicable
third-party terms even when it also contains X68K Tab modifications.

---

# Release checklist / 公開前チェック

Before publishing source or an M5Burner / GitHub Release firmware image, verify
the following:

- [ ] `LICENSE` exists for original X68K Tab code.
- [ ] `THIRD_PARTY_NOTICES.md` is included.
- [ ] `LICENSE_SHARP_X68000.txt` is included whenever applicable SHARP material
      is distributed.
- [ ] `human302.xdf` is **not** accidentally committed to Git; source-build users are
      told to place it locally in the repository root.
- [ ] `LICENSE_PANIC_X.txt` is included whenever PANIC.X is distributed.
- [ ] `PANIC_V1.38_NOTICE.txt` is included whenever PANIC.X is distributed.
- [ ] Original PANIC V1.38 documentation is retained/referenced with public PANIC.X redistributions.
- [ ] `components/px68k/fmgen/VGMM5_YM2151_NOTICE.md` remains in the source tree.
- [ ] Original PX68K / WinX68k / xkeropi / fmgen source headers and documents
      remain intact.
- [ ] Musashi MIT notices remain intact.
- [ ] SoftFloat-specific notices remain intact.
- [ ] M5Unified and M5GFX license/attribution requirements for the exact build
      versions are satisfied.
- [ ] M5GFX bundled-font notices are retained where required.
- [ ] ESP-IDF / Espressif component notices required for the distributed binary
      are retained.
- [ ] The exact CGROM source font and license are recorded if a generated CGROM
      is distributed.
- [ ] No commercial/user-owned XDF, DIM, HDS/HDF, ROM, or other software image
      has accidentally entered the public repository or release package.
- [ ] Generated C arrays (`*_bin.c`, etc.) have been checked for the provenance
      and redistribution terms of the binary from which they were generated.
- [ ] The final notice set is checked against the **actual files included in the
      release**, not only against the development tree.

---

## No endorsement

Names of third-party projects, companies, products, and trademarks are used
only to identify the software and hardware involved.

X68K Tab is an independent, unofficial project. Inclusion of a name or
attribution in this file does not imply endorsement of X68K Tab by the
respective author, project, company, or rights holder.

---

## Maintenance note

This notice is intentionally conservative because X68K Tab inherits a long
emulator lineage and uses several modern embedded-software dependencies.

When the source tree or dependency versions change, update this file to match
the exact distributed contents.

The original license text shipped with each third-party component always takes
precedence over this summary.
