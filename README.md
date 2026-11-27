# Grouwdi-OBS-FrameGen

**Smoother 60 FPS streams from a capture card, using NVIDIA's hardware optical flow.**

OBS always sends Twitch, YouTube and Kick 60 frames per second. But when your game can't
hand the capture card a new picture in time, some of those 60 frames are the same picture
shown twice. Viewers still get 60 frames a second, but the motion pauses for an instant at
every repeat, and that's the stutter they see.

Grouwdi-OBS-FrameGen finds those repeated frames and replaces each one with a real
in-between frame, built from the motion before and after it. Your stream still runs at 60
FPS, but now every one of those frames moves.

- Built for **Windows** and OBS's **Direct3D 11** renderer
- Measures motion with **NVIDIA's hardware optical flow engine**, through NVIDIA's own driver API
- Only fills repeated frames: **real frames are never changed** in the recommended mode
- Not DLSS-style frame doubling: it repairs your 60 FPS signal, it doesn't multiply it

---

## Quick setup

### 1. Install

1. Close OBS.
2. Download `Grouwdi-OBS-FrameGen-1.0.0-Setup.zip` from
   [Releases](../../releases), extract it, and run `Grouwdi-OBS-FrameGen-1.0.0-Setup.exe`.
   Accept the administrator prompt.
   - Windows may show **"Windows protected your PC"** because the installer isn't code-signed.
     Click **More info**, then **Run anyway**.

### 2. Add the filter

1. Start OBS.
2. Right-click your **capture card source** > **Filters**.
3. Under **Effect Filters**, click **+** > **Motion Smoother (NVIDIA)**.

### 3. Check the settings

New filters start with the recommended settings:

| Setting | Value |
|---|---|
| Smoothness vs. ghosting | Fill missing frames only |
| Video delay (frames) | 3 |
| Optical flow quality | Medium |
| Optical flow resolution (width) | 1280 |

### 4. Fix audio sync

The filter holds the video back 3 frames (about 50 ms at 60 FPS) so it can see ahead.
Delay your audio by the same amount:

**Edit** > **Advanced Audio Properties** > your capture card's audio > **Sync Offset: 50 ms**

### 5. Play

That's it. The first time you play, the filter calibrates itself as soon as the camera
moves (normal gameplay is enough) and saves the result. You never need to do it again.

---

## Features

### Fill missing frames
**What it's for:** removing the stutter caused by repeated frames.
**How it works:** each frame is compared with the one before it. When the picture hasn't
changed, the filter generates a new frame between the last real frame and the next one,
and shows that instead of the repeat.

### Smoothness vs. ghosting
**What it's for:** choosing how much the filter is allowed to change.

| Mode | What it does |
|---|---|
| **Fill missing frames only** (recommended) | Every real frame is shown untouched. A frame is generated only where the card repeated one. |
| Least ghosting | Also evens out uneven motion, but shows a real frame whenever the in-between point is close to one |
| Balanced | More evening-out, more generated frames |
| Smoothest | The most generated frames, the most even motion |

Generated frames can show slight ghosting around fast-moving objects, which is why the
recommended mode only generates frames where one was actually missing.

### Video delay
**What it's for:** letting the filter see the next real frame before it fills a gap.
**How it works:** the video runs a few frames behind live. 3 frames is enough for "Fill
missing frames only". Higher values help the other modes on uneven motion. Add about 17 ms
of audio Sync Offset per frame of delay at 60 FPS.

### Optical flow quality
**What it's for:** trading GPU work for more accurate motion.
**How it works:** it selects one of NVIDIA's three optical flow presets. More accurate
motion means generated pixels land closer to where they really belong, which shows most on
fast camera turns, the edges of moving objects, and fine detail like grass.

| Setting | NVIDIA preset | GPU work |
|---|---|---|
| Fast | Higher performance, lower quality | Optical flow hardware only |
| **Medium** (recommended) | Medium performance, medium quality | Optical flow hardware only |
| Slow | Slower performance, highest quality | Optical flow hardware **plus CUDA** pre- and post-processing |

### Optical flow resolution
**What it's for:** the image size motion is measured at (640, 960 or 1280 wide). 1280 is
the most accurate. Lower values use less GPU time.

### HUD protection
**What it's for:** keeping your HUD, crosshair and overlays sharp.
**How it works:** for each pixel, the filter checks whether it's static content. Static
pixels are blended in place instead of being dragged along with the camera motion.

### Scene cuts
When the picture changes completely (a cutscene cut, a loading screen, alt-tab), no frame
is generated across the cut. Blending two unrelated pictures would look wrong.

### Automatic calibration
The filter learns, once, which way NVIDIA's optical flow reports motion on your PC, and
saves it for every scene and every restart. If smoothing ever looks wrong after a GPU or
driver change (generated frames jumping backward during pans), click **Recalibrate flow
direction** and move the camera for a few seconds.

### Safe on other GPUs
On a non-NVIDIA GPU, or an NVIDIA GPU without an optical flow engine, the filter shows a
plain message and passes the video through unchanged. HDR sources are also passed through
unchanged.

---

## What it fixes, and what it doesn't

| Situation | Handled? |
|---|---|
| A single repeated frame (the most common case) | ✅ Filled |
| Two repeated frames in a row | ✅ Filled |
| Three or more repeats in a row (a freeze of about 50 ms or more) | ❌ Shown as-is. With a 3-frame delay, the filter can't see far enough ahead to bridge it. |
| Real game frames | ✅ Never changed in "Fill missing frames only" mode |
| Menus, pauses and loading screens | Left alone; there's nothing to fill |
| Frames dropped by OBS's encoder or the network | ❌ These happen after the filter. OBS reports them separately in its stats. |

---

## Changing settings while live

It's safe to change any setting live. Nothing breaks, and calibration isn't lost. But each
change restarts the filter's GPU setup and frame buffer, which shows as a brief hitch, so
it's best done between streams or on a BRB scene.

- Changing **Video delay** also moves your audio out of sync until you adjust the Sync
  Offset to match.
- Dragging a slider restarts the filter at every step of the drag.

---

## How it works (technical)

### The technology

| Part | What does it |
|---|---|
| Measuring motion | **NVIDIA Optical Flow Accelerator (NVOFA)**, dedicated hardware on NVIDIA GPUs from the Turing generation onward, used through the **NVIDIA Optical Flow API** |
| Building the new frame | The plugin's own GPU shader, in OBS's Direct3D 11 renderer |
| Deciding which frames to fill | The plugin's timing engine, running on the CPU |

The plugin loads `nvcuda.dll` and `nvofapi64.dll` from the installed NVIDIA driver at
runtime. Nothing from NVIDIA is shipped with the plugin. Frames are shared between OBS's
Direct3D 11 textures and the optical flow engine through CUDA–Direct3D 11 interop, so they
never leave the GPU.

### Step by step, every frame

1. **Capture:** the filter renders the capture source into a GPU texture.
2. **Downscale:** a brightness-only copy is made at the optical flow resolution (1280 wide
   by default).
3. **Detect repeats:** a frame counts as repeated when fewer than 8 pixels changed by 2 or
   more brightness levels.
4. **Measure motion:** for each new frame, NVIDIA's optical flow engine measures motion in
   both directions, forward and backward, against the previous real frame.
5. **Decide:** the timing engine looks ahead through the delay buffer. In "Fill missing
   frames only" mode, a repeat is filled when real frames exist on both sides of it within
   reach.
6. **Generate:** the shader moves pixels from the frame before and the frame after along
   the measured motion to where they'd be at the in-between moment, and blends the two
   results. Static pixels (HUD, overlays) are kept in place.
7. **Output:** OBS gets the generated frame instead of the repeat.

### Optical flow presets

The quality setting maps directly to NVIDIA's presets: `NV_OF_PERF_LEVEL_FAST`,
`NV_OF_PERF_LEVEL_MEDIUM` and `NV_OF_PERF_LEVEL_SLOW`. Fast and Medium run entirely on the
optical flow hardware. Slow adds CUDA pre- and post-processing for the most accurate
motion. See NVIDIA's
[Optical Flow Programming Guide](https://docs.nvidia.com/video-technologies/optical-flow-sdk/nvofa-programming-guide/index.html).

---

## Requirements

- Windows 10 or 11, 64-bit
- OBS Studio 31.1 or newer, Direct3D 11 renderer (the default)
- NVIDIA GPU with an optical flow engine: Turing generation or newer (for example GeForce
  RTX 20-series and later), with a current NVIDIA driver, on the PC running OBS
- SDR video

---

## Reading the OBS log

**Help** > **Log Files** > **View Current Log**, then search for `Grouwdi-OBS-FrameGen`.

| Line | Meaning |
|---|---|
| `loaded (version 1.0.0)` | The plugin loaded |
| `GPU optical flow ready: source 2560x1440, flow 1280x720, delay 3 frames` | The filter started |
| `flow direction: loaded from saved calibration` | Calibration reused; filling works right away |
| `Last 10 s: 598 new frames, 2 repeated, 2 smoothed (pan 0, timing 0, fill 2) ...` | Every 10 seconds: frames that arrived, repeats found, frames filled |

---

## Troubleshooting

| Message or problem | What to do |
|---|---|
| "Motion Smoother needs an NVIDIA GPU, but OBS is running on ..." | OBS is rendering on another GPU. On a PC with an NVIDIA card, set OBS to use it in Windows **Settings > System > Display > Graphics**. |
| "...needs an NVIDIA GPU with an optical flow engine..." | The GPU is older than the Turing generation, or the driver is out of date. Update the NVIDIA driver. |
| "Needs OBS's Direct3D 11 renderer" | OBS **Settings > Advanced > Renderer: Direct3D 11** |
| "Paused: the source is HDR" | Only SDR video is supported |
| Audio out of sync | Set the capture audio Sync Offset to about 17 ms per frame of Video delay |
| Smoothing looks wrong on pans | Click **Recalibrate flow direction** and move the camera for a few seconds |

---

## Notes

- **Made for dual-PC setups with a capture card.** Motion Smoother can also be added to
  other video sources, such as Game Capture on a single PC, but that hasn't been tested.
  There, the game and the filter share one GPU.
- **Upgrading from an earlier build** (named "capture-smoother" or
  "Grouwdi-OBS-CaptureSmoother"): the installer removes it for you. Your filters and
  settings carry over; the first few seconds of camera movement save the calibration again.
- **Extra diagnostics (optional):** the plugin also includes a **Capture Smoother** filter,
  found under **Audio/Video Filters** on capture card sources. It changes nothing in your
  video. Every 10 seconds it writes a timing report to the OBS log: the card's real frame
  rate, frames OBS had to re-show, and gaps in the card's timestamps. This helps tell
  whether a stutter comes from the game, the capture card or OBS. If you add it, leave its
  **Repair repeated frames** option off.

---

## Uninstall

Windows **Settings > Apps > Installed apps > Grouwdi-OBS-FrameGen > Uninstall**.
The saved calibration is kept in
`%APPDATA%\obs-studio\plugin_config\Grouwdi-OBS-FrameGen` and can be deleted too.

---

## Building from source

Release builds are cross-compiled on Linux with MinGW-w64 against the OBS Studio 31.1.1
source headers (OBS itself isn't built):

```
build-aux/mingw/build.sh /path/to/obs-studio-31.1.1
```

This writes the plugin, the manual-install zip and (with NSIS installed) the installer to
`release/`. Needs `x86_64-w64-mingw32-gcc`, `zip`, and optionally `makensis`.

The CMake files from the official OBS plugin template are also included, for building with
Visual Studio 2022 (`cmake --preset windows-x64`). That route hasn't been tested yet.

| Folder | Contents |
|---|---|
| `src/` | Plugin source (C) |
| `data/` | Shader (`motion-smoother.effect`) and UI text |
| `third_party/` | NVIDIA Optical Flow API headers and FFmpeg's `dynlink_cuda.h` |
| `installer/` | NSIS installer script |
| `test/` | Test harnesses that run the filters against stubbed OBS functions (see `test/README.txt`) |
| `reference/` | Python reference model used to design and verify the timing engine |
| `build-aux/mingw/` | Release build script |

## License

GPL-2.0-or-later. See `LICENSE` and `THIRD-PARTY-NOTICES.txt`.

© 2026 Grouwdi
