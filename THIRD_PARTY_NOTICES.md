# Third-party notices

QShadow is Copyright (C) 2026 Guillaume Millet and licensed under the GNU
General Public License, version 3 (see `LICENSE`). It includes or links the
following third-party components. The full license texts are in `licenses/`
(and are installed next to the program).

## Code and data included in the source tree

| Component | Where | License |
|---|---|---|
| AMD FidelityFX Super Resolution 1.0 (`ffx_a.h`, `ffx_fsr1.h`, EASU/RCAS shaders, constant packing in `src/fsrconsts.h`) | `third_party/fsr/`, `shaders/fsr_*.comp` | MIT, Copyright (c) 2021 Advanced Micro Devices, Inc. (`licenses/AMD-FidelityFX-FSR-MIT.txt`) |
| xBRZ "freescale" shader | `shaders/xbrz.frag` | MIT, Copyright (C) 2011/2016 Hyllian, with xBRZ code by Zenju under GPL-3 |
| CRT-Geom shader | `shaders/crt-geom.frag` | GPL-2.0-or-later, Copyright (C) 2010-2012 cgwg, Themaister and DOLLS |
| hq4x shader and lookup table | `shaders/hq4x.frag`, `shaders/hq4x.png` | hqx by Maxim Stepin, LGPL-2.1-or-later (`licenses/LGPL-2.1.txt`); upstream port carries no header |

Details, sources and the exact status of each shader are in
`shaders/ATTRIBUTION.md` (installed as `licenses/SHADERS.md`). The pre-compiled SPIR-V in `shaders/spv/` is generated
from those sources and carries the same licenses.

The application icon (`qshadow.svg`, `icons/`) and the other shaders
(`crt-royale.frag`, `yuv2rgb.comp`, `yuyv2rgb.comp`, `passthrough.frag`,
`fullscreen.vert`) were created for this project and are GPL-3.0-only.

## Libraries linked or bundled in binary releases

| Library | License | Notes |
|---|---|---|
| Qt 6 (Core, Gui, Widgets, Multimedia, Network, Wayland platform and decoration plugins) | LGPL-3.0 (or GPL-3.0) (`licenses/LGPL-3.0.txt`) | Linked dynamically; the libraries are shipped as separate files (AppImage `usr/lib`, Windows DLLs), so you can replace them with your own build of Qt. Qt's source code is available from https://download.qt.io/ and https://code.qt.io/. Binary releases use Ubuntu 24.04's Qt 6.4 (Linux) and vcpkg's Qt 6 (Windows). |
| libjpeg-turbo | IJG License, Modified (3-clause) BSD, zlib (`licenses/libjpeg-turbo.txt`) | **This software is based in part on the work of the Independent JPEG Group.** |
| Windows build only: FreeType, HarfBuzz, PCRE2, libpng, zlib, double-conversion, Brotli, bzip2 and other Qt dependencies installed by vcpkg | FreeType License / GPL-2.0, MIT, BSD, libpng, zlib | Each is distributed with its own license by vcpkg (`vcpkg_installed/.../share/<port>/copyright` in the build tree). |
| Vulkan loader (`libvulkan`, `vulkan-1.dll`) | Apache-2.0 | Not bundled: provided by the system / GPU driver. |
| ALSA (`libasound`) | LGPL-2.1 | Not bundled: provided by the system. |

Build-time only (nothing is shipped): glslang / glslc (BSD-3-Clause, Apache-2.0).

## Trademarks

"GENKI" and "ShadowCast" are trademarks of their respective owner. QShadow is an
independent project, not affiliated with, endorsed by or supported by GENKI. The
names are used only to say which hardware the program works with.
