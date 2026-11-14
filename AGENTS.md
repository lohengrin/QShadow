# AGENTS.md

Guidance for AI agents and contributors working in this repository.

## Project

Qt6 desktop application that captures video and audio from a GENKI ShadowCast
USB capture dongle (`298f:1996`, uvcvideo + USB-audio) and displays it on the
host with minimal latency.

- Video: V4L2 MJPG → libjpeg-turbo decode to planar Y/Cb/Cr (raw data, no CPU
  colour conversion) that a compute shader (`shaders/yuv2rgb.comp`) converts
  to RGBA on the GPU (falls back to CPU RGBA decode for non-YCbCr JPEGs or if
  the GPU path is unavailable; `QSHADOW_CPU_CONVERT=1` forces the fallback), or
  uncompressed YUYV (full-range BT.601, the same values as the JPEGs, even
  though the driver claims limited range; the capture buffer is passed to the GPU untouched and converted
  by `shaders/yuyv2rgb.comp`, or on the CPU as fallback; artifact-free but
  USB 2.0 limits it to ≤720x480@30 / 720x576@25 / 800x600@20 / 1280x720@10)
  → Vulkan (QVulkanWindow) display,
  auto-adapts to the source's active resolution/rate and preserves aspect ratio.
- Audio: ALSA capture from the dongle (S16_LE mono 96 kHz) → resampled to
  stereo 48 kHz float → host default output via Qt Multimedia `QAudioSink`. A PI controller
  (`src/audiodrift.h`) nudges the resampling ratio (max ±0.5%) to hold the
  ring fill near a small target, absorbing dongle/host clock drift.
- Settings (window geometry, aspect toggle, pixel format, selected resolution, shader,
  downscale, volume) persist in `QSettings` and are restored on the next launch.
- Platform: Linux (Wayland only; native ALSA/V4L2, no GStreamer) and Windows
  (Media Foundation video, WASAPI audio; dependencies from vcpkg).
- Rendering: `QVulkanWindow` + a `QVulkanWindowRenderer` (`VulkanRenderer`).
  All Vulkan resources (pipelines, images, descriptors, command recording) live
  in the renderer; `VideoWidget` only hands over the latest decoded `QImage`.
  The shader set is compiled to SPIR-V at build time when a compiler is found,
  with committed fallbacks in `shaders/spv/`.

## Build

Requires: Qt6 (Core/Gui/Widgets/Multimedia), libjpeg-turbo, ALSA
(libasound2-dev), Vulkan (libvulkan-dev), Wayland (libwayland-dev, libegl-dev,
libgles-dev), libdrm/gbm. `glslc`/`glslangValidator` are optional and only
enable build-time shader compilation.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run:

```
./build/qshadow
```

Windows (MSVC + vcpkg; `VCPKG_ROOT` must point at the vcpkg checkout, the
`vcpkg.json` manifest installs Qt 6, libjpeg-turbo, Vulkan and glslang):

```
cmake --preset windows
cmake --build --preset windows-release
ctest --preset windows-release
```

## Testing

Unit tests (Qt Test, run by ctest; no dongle, GPU or display needed) cover the
audio ring / pipeline / drift control, the decoder (MJPG planar vs RGBA, YUYV, corrupt
frames, frame-pool recycling), the frame queue / buffer pool and the sizing helpers:

```
cmake --build build -j && ctest --test-dir build --output-on-failure
```

Everything except `main()` is in the `qshadow_core` static library so tests link
the real code. Disable with `-DQSHADOW_BUILD_TESTS=OFF`. Hardware smoke checks
use system tools:

- Video device present: `v4l2-ctl --list-devices` and `v4l2-ctl -d /dev/video0 --list-formats-ext`
- Verify app negotiated format: `v4l2-ctl -d /dev/video0 --get-fmt-video`
- Audio device: `arecord -l` (card must list a ShadowCast entry)
- Headless stats: `QSHADOW_STATS=1 ./build/qshadow` prints the status bar lines
  (rates, decode time, capture-to-render latency, audio buffer) to stderr.

## CI and releases

- `.github/workflows/build.yml` is the single, reusable build (Linux + Windows:
  configure, build, ctest, package, verify the package). `ci.yml` (push to
  `master` / PRs) and `release.yml` (tags `v*`) only call it. Do not duplicate
  build steps in them.
- Releases are tag-driven: `scripts/bump-version.sh X.Y.Z` (updates
  `CMakeLists.txt` and `vcpkg.json`), merge, then push tag `vX.Y.Z`. `release.yml`
  fails fast if the tag, `CMakeLists.txt` and `vcpkg.json` disagree; a tag with a
  `-` suffix is published as a prerelease. Assets: Windows ZIP, Linux AppImage,
  Linux tar.gz, `SHA256SUMS`. The agent sandbox cannot push tags (the proxy
  returns 403); ask the user to push the tag.
- Linux uses distro Qt from apt (Ubuntu 24.04); Windows uses vcpkg with the
  `windows-ci` preset (Ninja) and a file binary cache under `actions/cache`.
  Cold vcpkg builds take about 2.5 h: restore/save are separate steps and the
  save runs even if later steps fail. Caches are scoped per ref: only runs on
  `master` seed a cache that tags and other PRs can read.
- **Packaging rules** (each is checked by a CI step; keep the checks when changing
  packaging):
  - never bundle `vulkan-1.dll`, `libvulkan`, ALSA, EGL/GL/gbm/drm, `libwayland-*`;
  - Windows: `install(DIRECTORY ...)` must not sweep the build tree
    (`vcpkg_installed`, `tests`, `CMakeFiles`, `_deps` are excluded) and the zip
    stays well under 100 MB; `windeployqt` skips plugin types the app does not use;
  - Linux AppImage: the app forces the Wayland platform, and `linuxdeploy-plugin-qt`
    does not see the Wayland plugins, so the platform, decoration,
    graphics-integration and shell-integration plugins are copied into the AppDir,
    given RUNPATH `$ORIGIN/../../lib` with `patchelf`, and their Qt libraries are
    passed to linuxdeploy with `--library`. Without
    `libQt6WaylandEglClientHwIntegration.so.6` Qt cannot load the wayland-egl client
    buffer integration and windows have no title bar. Verify packaging with
    `readelf`, not `ldd`: the runner has system Qt installed and hides missing
    libraries.
- Windows tests run on the `offscreen` platform: `tests/CMakeLists.txt` deploys the
  Qt plugins and `qoffscreen.dll` next to `tst_mainwindow` (the test hung at startup
  without them). `ctest` runs with `--timeout 120` in CI, so a hang fails with a log.
- `ctest` cannot be run locally without Qt; CI is the first real build for packaging
  changes. Branch rulesets are not enforced on this private repository (free plan).

## Architecture

Capture and audio input are the only OS-specific parts. They sit behind the
interfaces in `src/platform/platform.h` (`CaptureSource`, `AudioInput`, and the
`platform::findDevice / enumerateModes / createCapture / createAudioInput`
functions); everything else (decoder, YUV shaders, Vulkan renderer, UI, audio
pipeline and drift control) is portable Qt/Vulkan code in `src/`.

```
src/platform/platform.h        interfaces + factory functions (the seam)
src/platform/linux/            V4L2 video, ALSA audio, sysfs USB lookup
src/platform/windows/          Media Foundation video (async source reader),
                               WASAPI audio (IAudioClient3 min period), USB-ID lookup
src/audiopipeline.*            portable audio conditioning: downmix, decimate to
                               48 kHz, drift-compensated resample -> AudioRing
src/capture.h                  Frame, FrameQueue, BufferPool (portable)
```

To add a platform (Windows already exists, see above): create `src/platform/<os>/`,
implement the five `platform::` functions and the two interfaces, and select it
in `CMakeLists.txt` next to the Linux block. Rules for a backend:

- Identify the dongle by USB vendor/product ID (`device::kUsbVendorId/ProductId`),
  never by a hard-coded node or name; device ids are opaque strings.
- `CaptureSource` pushes `Frame`s (MJPG JPEGs or packed YUYV) into the
  `FrameQueue` from its own thread and never blocks on the consumer; use
  `BufferPool` for frame storage and set `Frame::stamp` to the capture time.
- `AudioInput` feeds raw S16 frames (any 48000*k Hz rate, any channel count) to
  an `AudioPipeline`; it does not resample or buffer anything itself.
- Keep OS headers (`<linux/...>`, `<alsa/...>`, `<windows.h>`) inside the
  backend directory.

## Conventions

- C++20. Qt Widgets + `QVulkanWindow`/`QVulkanWindowRenderer` for rendering.
- Native V4L2 (open/ioctl/mmap) and ALSA (`snd_pcm_*`) on Linux, not GStreamer;
  other platforms get their own native backend (see Architecture).
- Latency budget is critical: bounded queues, frame-drop on backpressure,
  non-blocking capture thread, no unbounded buffering in the audio path.
- No `std::format`; use `QString/QByteArray` and Qt idioms.
- Match on the USB vendor/product IDs (sysfs on Linux), never hardcode
  `/dev/videoN`.
- Keep decode in a worker thread; the GUI thread only blits the latest frame.
- Vulkan resources are owned by `VulkanRenderer` and created/destroyed on the
  render thread (`initResources`/`initSwapChainResources`/`release*`); never
  touch them from the GUI thread. Set `QSHADOW_VULKAN_VALIDATION=1` to enable
  the validation layer when it is installed.
- Shaders live in `shaders/` (Vulkan GLSL 450); after editing one, regenerate
  its committed fallback in `shaders/spv/`, e.g.
  `glslangValidator -V -S frag -Ithird_party/fsr -o shaders/spv/foo.frag.spv shaders/foo.frag`
  (glslc: `-fshader-stage=frag -I third_party/fsr`).
- Formatting: 4-space indent, no trailing whitespace. clang-format (LLVM-ish)
  is optional, not mandatory.