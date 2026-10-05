# QShadow

Minimal-latency viewer for the **GENKI ShadowCast** USB capture dongle
(`298f:1996`) on Linux (Wayland).

QShadow grabs the HDMI input of a ShadowCast, shows it in a window, and plays
its audio through your computer's speakers — with as little added delay as
possible.

## Features

- **Automatic device detection** — finds the ShadowCast by its USB IDs, no
  need to hardcode `/dev/videoN`.
- **Adapts to the input** — you pick the video mode that matches the source
  (e.g. 720p source → 1280×720), and the frame keeps its aspect ratio
  (letterboxed if the window doesn't match).
- **MJPG or YUYV capture** — pick the *Format* in the toolbar. MJPG is the
  dongle's compressed mode (up to 60 fps, but with JPEG compression artifacts);
  YUYV is uncompressed and artifact-free, but USB 2.0 bandwidth limits it to
  low resolutions (e.g. 720×480 @ 30 fps, 720×576 @ 25, 800×600 @ 20,
  1280×720 @ 10) — ideal for retro / SD sources.
- **Low latency pipeline** — MJPG capture → libjpeg-turbo decode on its own
  thread → Vulkan blit. MJPG frames are decoded to planar YUV, and YUYV frames
  are passed through untouched (zero-copy); both are converted to RGB by GPU
  compute shaders, which skips the CPU colour conversion and halves the data
  uploaded (set `QSHADOW_CPU_CONVERT=1` to force the CPU path). Frames are dropped rather than queued when things get
  busy, and the audio buffer is kept short.
- **Audio** captured from the dongle at native 96 kHz and resampled to the
  host output. The resampling ratio is nudged by at most ±0.5% to absorb clock
  drift between the dongle and the sound card, so the buffer stays short
  without clicks or dropouts.
- **Hotplug** — start QShadow before plugging in the dongle, or unplug and
  replug it while running; the capture reconnects automatically.
- **Live stats** — the status bar shows capture/decode rates, decode time,
  capture-to-render latency, audio buffer fill and drift correction.
- **Remembers your settings** — window size, aspect-ratio toggle,
  pixel format, selected resolution, volume, shader and downscale are restored on the next
  launch.
- **GPU sharpening & CRT shaders** — pick a post-processing shader from the
  toolbar: nearest-neighbor *None*, *Bilinear smooth*, the pixel-art upscalers
  **HQ4X** and **xBRZ**, the CRT emulators **CRT-Geom** and **CRT-Royale**, and
  **AMD FSR 1.0** (EASU edge-adaptive upscale + RCAS sharpening) in *Ultra
  Quality*, *Quality*, *Balanced*, *Performance* and *Sharpen only* variants.
- **Downscale** — the capture source may have already scaled the signal up to
  fit a fixed resolution; the *Downscale* selector point-decimates the frame
  (off / ×2 / ×4) back to its native grid before the shader runs.

## Requirements

- Linux with a **Wayland** session
- Qt 6 (`Core`, `Gui`, `Widgets`, `Multimedia`)
- Vulkan loader and headers (`libvulkan-dev`) with a Vulkan 1.1-capable GPU
  driver (Mesa RADV/ANV/llvmpipe, Intel, NVIDIA, …)
- `libjpeg-turbo`
- `libasound2-dev` (ALSA)
- A ShadowCast dongle connected by USB

The shaders are compiled to SPIR-V at build time when `glslc` or
`glslangValidator` is on `PATH`; otherwise the committed SPIR-V in
`shaders/spv/` is used, so the shader toolchain is optional.

On Ubuntu/Debian:

```
sudo apt install qt6-multimedia-dev libjpeg-turbo8-dev libasound2-dev \
                 libvulkan-dev libwayland-dev libegl-dev libgles-dev \
                 libdrm-dev libgbm-dev
```

## Build

From the project root:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

To run the unit tests (no dongle needed) or install system-wide:

```
ctest --test-dir build --output-on-failure
sudo cmake --install build        # binary, desktop entry and icon under /usr/local
```

## Run

```
./build/qshadow
```

Make sure the ShadowCast is plugged in before you start it.

## Using QShadow

- **Resolution** — use the dropdown in the toolbar to select the video mode
  matching your input signal. 1080p sources: `1920×1080`; 720p sources:
  `1280×720`; retro/4:3 sources: `1024×768`, `800×600`, `640×480`, etc. The
  status bar reports what the device actually negotiated and what each frame
  measures.
- **Aspect ratio** — toggle *Preserve aspect*. When on, the picture is fitted
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
- **Status bar** — shows the device, negotiated resolution, per-frame
  resolution, audio state, capture/decode frame rates, dropped frames, and
  average decode time.

## Troubleshooting

- **“No ShadowCast capture dongle found”** — check it is plugged in and that
  `lsusb` shows `298f:1996 MACROSILICON ShadowCast`. If the dongle was plugged
  in after the app started, restart the app.
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

The capture device is listed as a ShadowCast:

```
v4l2-ctl --list-devices
```