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
- Platform: Linux, Wayland only. Native ALSA/V4L2, no GStreamer.
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

## Testing

Unit tests (Qt Test, run by ctest; no dongle, GPU or display needed) cover the
audio ring / drift control, the decoder (MJPG planar vs RGBA, YUYV, corrupt
frames, frame-pool recycling), the frame queue and the sizing helpers:

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

## Conventions

- C++20. Qt Widgets + `QVulkanWindow`/`QVulkanWindowRenderer` for rendering.
- Native V4L2 (open/ioctl/mmap) and ALSA (`snd_pcm_*`), not GStreamer.
- Latency budget is critical: bounded queues, frame-drop on backpressure,
  non-blocking capture thread, no unbounded buffering in the audio path.
- No `std::format`; use `QString/QByteArray` and Qt idioms.
- Match on the USB vendor/product IDs from uvcvideo/sysfs, never hardcode
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