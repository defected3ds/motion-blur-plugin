# Vector Motion Blur

**Optical-flow motion blur for After Effects, in the style of ReelSmart Motion Blur (RSMB).**

Adds realistic motion blur to footage that has none (screen recordings, 3D renders without blur, stop-motion, fast pans,
frame-rate-converted video). It looks at the neighbouring frames, works out how every pixel is moving, and blurs along
that motion, instead of blurring in a fixed direction.

## What's included
| Part | What it is | Status |
|---|---|---|
| `core/` | Standalone C++ engine: optical-flow motion estimation + vector blur | Tested (flow error ~0.2 px on test footage; blur matches analytic reference) |
| `ae/` | After Effects effect plugin (Mac, 8/16-bit). Appears as Effect > Blur & Sharpen > Vector Motion Blur | Builds with Adobe SDK 27 and loads in After Effects; render speed fix pending real-world confirmation |
| `cli/mbvideo` | Command-line tool: video in, motion-blurred video out (uses ffmpeg) | Tested on Linux; Mac build script included |
| `ae_script/VectorMotionBlur.jsx` | After Effects script that runs `mbvideo` on the selected footage layer | Syntax-checked only; not run in After Effects |

## Controls (After Effects plugin)
- **Shutter Angle** (default 180): how long the virtual shutter stays open. 360 = blur spans a whole frame.
- **Shutter Phase** (default -90): where the shutter window sits relative to the frame.
- **Blur Amount %**: multiplier on blur length.
- **Max Samples**: upper limit of samples per pixel (quality vs speed).
- **Vector Smoothness**: how smooth the estimated motion field is.
- **Edge-Aware Blur** (experimental, off by default): limits bleeding across moving-object edges.
- **Motion Vectors**: shows the estimated motion as a colour map (debug view).

## How it works
1. Motion estimation: coarse-to-fine Horn-Schunck optical flow between the current frame and its previous/next frame,
   run on a reduced-size copy and scaled back up for speed.
2. Blur: each pixel averages samples along its motion vector over the shutter window.

## Known limitations
- CPU only; 8/16-bit colour (no 32-bit float yet). Mac build only so far.
- Not real-time: use RAM Preview and lower the comp resolution while working.
- Blur can still bleed across fast object edges; the real fix is better motion estimation at occlusions
  (forward/backward consistency checks).
- Assumes straight-line motion within a frame; no rotation or curved paths.

## Build (Mac)
Needs the full Xcode app and Adobe's After Effects SDK (free from developer.adobe.com; not redistributable, never commit it).

    bash scripts/build_mac.sh ~/Desktop/AfterEffectsSDK
    sudo cp -R build-mac/VectorMotionBlur.plugin "/Library/Application Support/Adobe/Common/Plug-ins/7.0/MediaCore/"

Restart After Effects. Standalone tool: `./build_mac.command`, then `./mbvideo in.mp4 out.mp4`.

## Pasteable summary (for continuing in another chat/account)
> Project: vector motion blur (RSMB-style) for After Effects. Repo: github.com/defected3ds/motion-blur-plugin (master).
> `core/` tested C++ engine; `ae/` plugin source (built on Mac via `scripts/build_mac.sh <SDK folder>`, SDK 27, needs Xcode);
> `cli/mbvideo` standalone video tool; `ae_script/VectorMotionBlur.jsx` AE script. Status: plugin loaded in After Effects
> but playback was too slow; I then sped up motion estimation (reduced-size flow + threading). Next: rebuild with the
> latest code, confirm it renders and plays (RAM Preview), then improve edge handling. Don't put Adobe's SDK in the repo.
