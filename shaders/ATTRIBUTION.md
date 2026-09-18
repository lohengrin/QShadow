# Shader sources and licensing

The files in this directory implement the rendering shader styles shown in the
toolbar *Shader* combo box. They are sorted into groups, each with its own
license terms.

All shaders are Vulkan GLSL (`#version 450`). The committed `spv/` directory
holds build-time SPIR-V output used when no shader compiler is installed; it is
generated from these sources with `glslc`/`glslangValidator` and carries the
same licenses.

## Original project shaders

`fullscreen.vert`, `passthrough.frag`, `crt-royale.frag`

- `fullscreen.vert` is the shared vertexless fullscreen-triangle vertex shader
  used by every graphics pass.
- `passthrough.frag` is the trivial texture blit used for the *None* (nearest)
  and *Bilinear smooth* modes, the offscreen downscale pass and the final
  present.
- `crt-royale.frag` is an original single-pass CRT emulation written for this
  project. It is *inspired by* the appearance of the libretro
  `crt-royale/crt-royale` preset ("fake-bloom": aperture grille, scanlines,
  halation, curvature, vignette), but it is **not** derived from libretro
  source code. The upstream shader is a multi-pass pipeline with offscreen
  buffers and cannot be used as-is in this app; this file only mimics the
  projected look. Owned by this project.

## AMD FidelityFX Super Resolution 1.0 — MIT

`fsr_easu.comp`, `fsr_rcas.comp` (headers vendored under `../third_party/fsr/`)

- The EASU and RCAS compute shaders use the FidelityFX FSR 1.0 shader headers
  `ffx_a.h` and `ffx_fsr1.h`.
- Copyright (C) 2021 Advanced Micro Devices, Inc. All rights reserved.
- Licensed under the MIT License; the full text is in
  `../third_party/fsr/license.txt`.

## hq4x — MIT

`hq4x.frag` (and the bundled lookup table `hq4x.png`)

- Adapted from:
  https://github.com/libretro/glsl-shaders (path `hqx/single-pass/shader-files/hq4x.glsl`)
  https://github.com/gigagames/hqx (original LUT `hq4x.png`)
- Copyright (C) Maxim Stepin (hqx algorithm and LUT data).
- The `hq4x.glsl` container shader produced by Hyllian is MIT licensed
  (see its header); the underlying hq4x algorithm/LUT is by Maxim Stepin and
  the LUT is redistributed under the MIT MIT-like dual terms used by the hqx
  project.

## xBRZ — MIT + GPL (see header)

`xbrz.frag`

- Adapted from:
  https://github.com/libretro/glsl-shaders (path `xbrz/shaders/xbrz-freescale.glsl`)
- Copyright (C) 2011/2016 Hyllian - sergiogdb@gmail.com (MIT licensed, full
  MIT text in the file header), using code and/or concepts from the xBRZ
  algorithm by Zenju as it appears in the Desmume source code (GPL,
  https://www.gnu.org/licenses/gpl-3.0.html). Keep the file header if you
  redistribute.

## CRT-Geom — GPL-2.0-or-later

`crt-geom.frag`

- Adapted from:
  https://github.com/libretro/glsl-shaders (path `crt/shaders/crt-geom.glsl`)
- Copyright (C) 2010-2012 cgwg, Themaister and DOLLS
- Licensed under the GNU General Public License, version 2 or later
  (https://www.gnu.org/licenses/gpl-2.0.html). This file is a derivative work;
  its license applies to it (and to any redistribution of it), independently
  of the rest of this project.

## Upstream reference

- libretro glsl-shaders repository:
  https://github.com/libretro/glsl-shaders