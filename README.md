# Vector Motion Blur (After Effects plugin)

An RSMB-style (ReelSmart Motion Blur) effect: it adds motion blur to footage that has none by
estimating per-pixel motion from neighbouring frames and blurring along those vectors.

## How it works
1. **Flow** (`core/motion_blur.cpp`): coarse-to-fine, warped Horn–Schunck optical flow between
   current↔next and current↔previous frames. The two are averaged into one velocity (pixels/frame)
   per pixel; first/last frames fall back to the single available neighbour.
2. **Blur**: each pixel averages bilinear taps along its velocity over the shutter window
   (`Shutter Angle`, `Shutter Phase`), with the tap count adapted to the vector length.

## Parameters
Shutter Angle, Shutter Phase, Blur Amount %, Max Samples, Vector Smoothness, Show Motion Vectors.

## Build & test the core (any platform)
    cmake -S . -B build && cmake --build build && ./build/test_motion_blur

## Build the After Effects plugin
The Adobe After Effects SDK is not redistributable; download it from Adobe and:

    cmake -S . -B build -DAE_SDK_DIR=/path/to/AfterEffectsSDK && cmake --build build

* macOS: the target produces a `.plugin` bundle; compile `ae/MotionBlur.r` with `Rez` into the bundle's resources.
* Windows: compile `ae/MotionBlur.r` with `cl /EP` + `PiPLtool` into a `.rc`, add it to the module, rename to `.aex`.
  (Easiest is to copy the SDK's `Skeleton` sample project and swap in `ae/MotionBlur.cpp`, `ae/MotionBlur.r` and `core/`.)
* Install into `Plug-ins/` of your After Effects folder.

Status: the core is tested (`tests/`); `ae/MotionBlur.cpp` was written against the SDK but has **not** been compiled or run inside After Effects.

## Limitations / ideas
- CPU only, 8/16-bit (32-bit float needs SmartFX).
- No foreground/background separation: blur from fast objects can bleed across occlusion edges (RSMB masks this); an alpha/depth-aware gather would fix it.
- Linear motion within a frame; no rotation/curved paths.
