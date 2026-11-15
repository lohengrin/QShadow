# QShadow

Minimal-latency viewer for USB video capture devices on Linux (Wayland) and
Windows 10/11. Built for the **GENKI ShadowCast** HDMI capture dongle
(`298f:1996`), and works with other UVC capture devices and webcams too.

QShadow grabs the video input of a capture device, shows it in a window, and
plays its audio through your computer's speakers — with as little added delay
as possible.

## Features

- **Any capture device** — every video capture device is listed in the
  *Device* selector (your choice is remembered; if it is not plugged in, the
  first device is used). The audio interface of the same USB device is paired
  automatically. The ShadowCast is the tested device; others rely on what
  their driver reports (see [Other devices](#other-devices)).
- **Adapts to the input** — you pick the video mode that matches the source
  (e.g. 720p source → 1280×720), and the frame keeps its aspect ratio
  (letterboxed if the window doesn't match).
- **Formats and frame rates** — pick the *Mode* in the toolbar (resolution,
  frame rate and pixel format in one list of what the device offers). MJPG is compressed (high
  resolutions and rates, with JPEG artifacts); the uncompressed formats
  (YUYV, UYVY, NV12, RGB24/BGR24/BGRX) are artifact-free but limited by USB
  bandwidth. On the ShadowCast, YUYV is limited by USB 2.0 to low resolutions
  (e.g. 720×480 @ 30 fps, 720×576 @ 25, 800×600 @ 20, 1280×720 @ 10) — ideal
  for retro / SD sources. Windows captures MJPG, YUY2, NV12 and UYVY.
- **Low latency pipeline** — capture → decode on its own thread → Vulkan
  blit. MJPG frames are decoded (libjpeg-turbo) to planar YUV, and full-range
  BT.601 YUYV frames are passed through untouched (zero-copy); both are
  converted to RGB by GPU compute shaders. Other formats and colour
  interpretations (limited range, BT.709) are converted on the CPU, which skips the CPU colour conversion and halves the data
  uploaded (set `QSHADOW_CPU_CONVERT=1` to force the CPU path). Frames are dropped rather than queued when things get
  busy, and the audio buffer is kept short.
- **Audio** captured from the device at its native rate (96 kHz on the
  ShadowCast; 44.1, 48 and other rates work) and resampled to the host output.
  The *Audio source* option (Options menu) picks the device's own audio, none, or any other input.
  The resampling ratio is nudged by at most ±0.5% to absorb clock drift
  between the device and the sound card, so the buffer stays short without
  clicks or dropouts. A device without audio runs video only.
- **Hotplug** — start QShadow before plugging in the device, or unplug and
  replug it while running; the capture reconnects automatically.
- **Compact toolbar and live stats** — the toolbar only holds Device, Mode,
  Shader, volume, Fit and Fullscreen, plus an *Options* menu (the same entries
  as the right-click menu) for aspect ratio, audio source and downscale. The
  status bar shows the device, mode and audio state on the left and capture
  rate, decode time, latency and audio buffer on the right; hover for the
  full numbers. Problems (no frames, dropped frames, audio underruns) show up
  as a red warning only while they happen.
- **Remembers your settings** — window size, aspect-ratio toggle,
  device, pixel format, resolution, frame rate, audio source, volume, shader
  and downscale are restored on the next launch.
- **GPU sharpening & CRT shaders** — pick a post-processing shader from the
  toolbar: nearest-neighbor *None*, *Bilinear smooth*, the pixel-art upscalers
  **HQ4X** and **xBRZ**, the CRT emulators **CRT-Geom** and **CRT-Royale**, and
  **AMD FSR 1.0** (EASU edge-adaptive upscale + RCAS sharpening) in *Ultra
  Quality*, *Quality*, *Balanced*, *Performance* and *Sharpen only* variants.
- **Downscale** — the capture source may have already scaled the signal up to
  fit a fixed resolution; the *Downscale* selector point-decimates the frame
  (off / ×2 / ×4) back to its native grid before the shader runs.

## Requirements

- Linux with a **Wayland** session, or Windows 10/11 (x64)
- Qt 6 (`Core`, `Gui`, `Widgets`, `Multimedia`)
- Vulkan loader and headers (`libvulkan-dev`) with a Vulkan 1.1-capable GPU
  driver (Mesa RADV/ANV/llvmpipe, Intel, NVIDIA, …)
- `libjpeg-turbo`
- `libasound2-dev` (ALSA)
- A USB video capture device (a GENKI ShadowCast, another UVC capture card or a webcam)

The shaders are compiled to SPIR-V at build time when `glslc` or
`glslangValidator` is on `PATH`; otherwise the committed SPIR-V in
`shaders/spv/` is used, so the shader toolchain is optional.

On Ubuntu/Debian:

```
sudo apt install qt6-multimedia-dev libjpeg-turbo8-dev libasound2-dev \
                 libvulkan-dev libwayland-dev libegl-dev libgles-dev \
                 libdrm-dev libgbm-dev
```

## Build (Windows)

Needs Visual Studio 2022/2026 (C++ workload), CMake and
[vcpkg](https://github.com/microsoft/vcpkg). All other dependencies (Qt 6,
libjpeg-turbo, Vulkan, glslang) come from the `vcpkg.json` manifest; the first
configure builds Qt, which takes a while (binary caching makes later ones fast).

```
set VCPKG_ROOT=C:\Dev\vcpkg
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
build-windows\Release\qshadow.exe
```

Video uses Media Foundation (native MJPG/YUY2/NV12/UYVY passthrough, no converters) and
audio uses WASAPI shared mode at the smallest engine period the driver
allows. A Vulkan-capable GPU driver is needed; the Vulkan SDK is not.

## Build (Linux)

From the project root:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

To run the unit tests (no capture device needed) or install system-wide:

```
ctest --test-dir build --output-on-failure
sudo cmake --install build        # binary, desktop entry and icon under /usr/local
```

## CI and releases

GitHub Actions (`.github/workflows/`) builds, tests and packages QShadow on
Linux and Windows:

| Workflow | Trigger | What it does |
|----------|---------|--------------|
| `ci.yml` | push to `master`, pull requests | calls `build.yml`; the packages are kept as run artifacts (Actions tab, *Artifacts*) |
| `release.yml` | tag `v*` | checks the tag against the project version, calls `build.yml`, then publishes a GitHub Release |
| `build.yml` | reusable | the actual build, shared by the two above |

`build.yml` runs on both platforms: configure, build, `ctest`, package, then
check the package contents (a failed check fails the build).

- **Linux** (`ubuntu-24.04`, distro Qt from apt): `qshadow-<ver>-linux-x86_64.AppImage`
  (Qt bundled, runs on most distros) and `qshadow-<ver>-linux-x86_64.tar.gz`
  (binary, desktop file and icon only; needs system Qt).
- **Windows** (`windows-2025`, MSVC + Ninja, dependencies from vcpkg through the
  `windows-ci` preset): `qshadow-<ver>-windows-x64.zip`, portable (about 20 MB).

Every release also carries a `SHA256SUMS` file.

**What is deliberately not bundled:** the Vulkan loader (`vulkan-1.dll`,
`libvulkan.so`), ALSA, EGL/GL/gbm/drm and `libwayland-*`. They must come from the
GPU driver or the host system, and a bundled copy can shadow the right one. The
Qt Wayland plugins and libraries *are* bundled in the AppImage: without them Qt
cannot create client-side window decorations on GNOME.

### Making a release

```
scripts/bump-version.sh 0.3.0           # CMakeLists.txt + vcpkg.json
git commit -am "release v0.3.0"
git push origin HEAD                    # merge to master through a PR first
git tag v0.3.0 && git push origin v0.3.0
```

The tag must match the version in `CMakeLists.txt` and `vcpkg.json` (a suffix
such as `-rc1` or `-beta1` is allowed and publishes a **prerelease**), otherwise
`release.yml` stops at its first job. When it finishes, the release shows up
under *Releases* with all four files.

### Things to know

- **First Windows build is slow (about 2.5 h).** vcpkg builds Qt from source. The
  binary packages are cached with `actions/cache`, but GitHub only lets a run
  read caches from its own branch/tag, its base branch and the default branch.
  So the cache that counts is the one saved by a run on `master`; PR runs reuse
  it afterwards (about 4 min), and a tag run can read it too. A tag run never
  sees a cache that was saved by a PR branch. If `vcpkg.json` changes, the cache
  key changes and the next build is cold again.
- **Branch rules are not enforced** on a private repository on the free plan
  (rulesets need a paid plan or a public repository). CI results are advisory
  until then; merge only when both jobs are green.
- Test a package from a PR before releasing: download it from the run's
  *Artifacts* section (unzip, then run `qshadow.exe`, or run the AppImage).
  For the AppImage on Wayland,
  `QT_DEBUG_PLUGINS=1 ./qshadow-*.AppImage 2>&1 | grep -i -E "cannot load|client buffer"`
  must print nothing.

## Run

```
./build/qshadow
```

Plug in the capture device before you start it, or any time while it runs.

## Using QShadow

- **Device** — choose which capture device to show. *Audio source* (in the
  Options menu, or by right-clicking the video) defaults to that device's own
  audio interface; pick *None* or another input to change it.
- **Mode** — use the dropdown in the toolbar to select the video mode
  matching your input signal. 1080p sources: `1920×1080`; 720p sources:
  `1280×720`; retro/4:3 sources: `1024×768`, `800×600`, `640×480`, etc. The
  status bar reports what the device actually negotiated and what each frame
  measures.
- **Options menu** — the button at the right end of the toolbar (also the
  right-click menu on the video) holds *Preserve aspect*, *Audio source*,
  *Downscale* and the other settings.
- **Aspect ratio** — toggle *Preserve aspect* in the Options menu. When on, the picture is fitted
  inside the window with black bars so it never gets stretched. When off, it
  fills the whole window.
- **Shader** — pick a rendering style from the toolbar. *None* shows pixels
  hard (nearest-neighbor) — best to minimize any added smoothing; *Bilinear
  smooth* softens the upscale. *HQ4X* and *xBRZ* are sharp, edge-directed
  scaling (best on clean low-res 240p/480p sources), and *CRT-Geom* /
  *CRT-Royale* simulate an interlaced CRT (scanlines, aperture-grille mask,
  curvature). The *FSR 1.0* modes run AMD's FidelityFX Super Resolution EASU
  upscale followed by an RCAS sharpen pass on the GPU — pick a quality tier
  (*Ultra Quality* … *Performance*) for the internal render ratio, or
  *Sharpen only* to just apply RCAS at full resolution. Rendering needs a
  Vulkan 1.1-capable driver, which any modern Mesa/Intel/NVIDIA stack
  provides.
- **Downscale** — if the source/capture chain already upscaled the picture to
  a fixed resolution (e.g. 4:3 content blown up to 640×480), set this to the
  matching factor (×2, ×4, or off). The frame is point-decimated back to the
  original pixel grid with nearest-neighbour before the shader runs, so HQ4X
  and xBRZ reconstruct real edges instead of smearing an already-soft image
  (and their GPU cost drops accordingly).
- **Status bar** — left: device, frame size, pixel format and audio state;
  right: capture rate, decode time, latency and audio buffer. Hover over either
  side for the negotiated size, decode rate, drop and underrun counters and the
  drift correction. A red warning appears while frames are missing or dropped
  or the audio under/overruns. `QSHADOW_STATS=1` prints the complete numbers
  to stderr.

## Troubleshooting

- **“Waiting for a video capture device”** — check the device is plugged in
  (`v4l2-ctl --list-devices` on Linux) and that your user may open
  `/dev/video*` (the `video` group). Devices plugged in later are picked up
  automatically.
- **“no supported video format”** — the device offers only formats QShadow
  cannot capture (for example H.264 only).
- **No video** — make sure the HDMI source is powered on; the status bar shows
  the negotiated and per-frame resolution.
- **No sound** — make sure the host output volume is up and the correct output
  is selected in your system sound settings. The status bar shows whether the
  audio link is active.

## Verifying the capture (optional)

With the app running you can confirm the negotiated format:

```
v4l2-ctl -d /dev/video0 --get-fmt-video
```

List the capture devices and their formats:

```
v4l2-ctl --list-devices
v4l2-ctl -d /dev/video0 --list-formats-ext
```

## Other devices

The ShadowCast is the only device tested by the author. For any other device
QShadow uses what the driver reports:

- **Colours** — YCbCr formats are converted with the matrix (BT.601/709) and
  range (full/limited) the driver declares. If a device's colours look wrong
  (too dark or washed out), it probably declares them incorrectly; the
  ShadowCast is such a case (its YUYV is full range although the driver says
  limited) and is corrected by a small table in `src/devicequirks.h`. Reports
  with the device name, `lsusb` ID and the symptom are welcome.
- **Audio** — the format is negotiated with the sound card (S16, mono or
  stereo, common rates). Devices without a sound interface run video only.
- **Not supported** — H.264-only devices, and on Windows the uncompressed RGB
  formats.

## License

QShadow is Copyright (C) 2026 Guillaume Millet and released under the
**GNU General Public License, version 3** (see [LICENSE](LICENSE)).

It includes third-party code under compatible licenses (AMD FidelityFX FSR,
xBRZ, CRT-Geom, hq4x) and links Qt 6 (LGPL-3.0) and libjpeg-turbo; see
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md), [shaders/ATTRIBUTION.md](shaders/ATTRIBUTION.md)
and the texts in [licenses/](licenses/). This software is based in part on the
work of the Independent JPEG Group.

QShadow is an independent project and is not affiliated with, endorsed by or
supported by GENKI. "GENKI" and "ShadowCast" are trademarks of their owner and
are used only to identify the supported hardware.
