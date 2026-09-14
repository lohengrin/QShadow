# AGENTS.md

Guidance for AI agents and contributors working in this repository.

## Project

Qt6 desktop application that captures video and audio from a GENKI ShadowCast
USB capture dongle (`298f:1996`, uvcvideo + USB-audio) and displays it on the
host with minimal latency.

- Video: V4L2 MJPG → libjpeg-turbo decode → OpenGL display, auto-adapts to the
  source's active resolution/rate and preserves aspect ratio.
- Audio: ALSA capture from the dongle (S16_LE mono 96 kHz) → resampled to
  stereo 48 kHz float → host default output via Qt Multimedia `QAudioSink`.
- Platform: Linux, Wayland only. Native ALSA/V4L2, no GStreamer.

## Build

Requires: Qt6 (Core/Gui/Widgets/Multimedia), libjpeg-turbo, ALSA (libasound2-dev),
Wayland (libwayland-dev, libegl-dev, libgles-dev), libdrm/gbm.

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Run:

```
./build/qshadow
```

## Testing

No automated test framework. Smoke checks use system tools:

- Video device present: `v4l2-ctl --list-devices` and `v4l2-ctl -d /dev/video0 --list-formats-ext`
- Verify app negotiated format: `v4l2-ctl -d /dev/video0 --get-fmt-video`
- Audio device: `arecord -l` (card must list a ShadowCast entry)

## Conventions

- C++20. Qt Widgets + QOpenGLWidget for rendering.
- Native V4L2 (open/ioctl/mmap) and ALSA (`snd_pcm_*`), not GStreamer.
- Latency budget is critical: bounded queues, frame-drop on backpressure,
  non-blocking capture thread, no unbounded buffering in the audio path.
- No `std::format`; use `QString/QByteArray` and Qt idioms.
- Match on the USB vendor/product IDs from uvcvideo/sysfs, never hardcode
  `/dev/videoN`.
- Keep decode in a worker thread; the GUI thread only blits the latest frame.
- Formatting: 4-space indent, no trailing whitespace. clang-format (LLVM-ish)
  is optional, not mandatory.