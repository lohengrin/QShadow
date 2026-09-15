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
- **Low latency pipeline** — MJPG capture → libjpeg-turbo decode on its own
  thread → OpenGL blit. Frames are dropped rather than queued when things get
  busy, and the audio buffer is kept short.
- **Audio** captured from the dongle at native 96 kHz and resampled to the
  host output.
- **Remembers your settings** — window size, aspect-ratio toggle,
  selected resolution, volume and shader are restored on the next launch.
- **GPU sharpening & CRT shaders** — pick a post-processing shader from the
  toolbar: nearest-neighbor *None*, *Bilinear smooth*, the pixel-art upscalers
  **HQ4X** and **xBRZ**, and the CRT emulators **CRT-Geom** and
  **CRT-Royale**.

## Requirements

- Linux with a **Wayland** session
- Qt 6 (`Core`, `Gui`, `Widgets`, `OpenGLWidgets`, `Multimedia`)
- `libjpeg-turbo`
- `libasound2-dev` (ALSA)
- A ShadowCast dongle connected by USB

On Ubuntu/Debian:

```
sudo apt install qt6-multimedia-dev libjpeg-turbo8-dev libasound2-dev \
                 libwayland-dev libegl-dev libgles-dev libdrm-dev libgbm-dev
```

## Build

From the project root:

```
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
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
  curvature). GPU shaders need a GL 3.3-core-capable driver, which any modern
  Mesa/ANGLE stack provides.
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