// Vector-based motion blur core (no After Effects dependencies).
//
// Pipeline: estimate dense optical flow between neighbouring frames, derive a
// per-pixel velocity (pixels/frame) for the current frame, then average taps
// sampled along that velocity over the virtual shutter interval.
#pragma once

#include <vector>

namespace mb {

// Interleaved RGBA, float. Colour is expected premultiplied (as in After Effects).
struct Image {
    int w = 0, h = 0;
    std::vector<float> px;
    Image() = default;
    Image(int w_, int h_) : w(w_), h(h_), px(size_t(w_) * h_ * 4, 0.f) {}
    float* at(int x, int y) { return &px[(size_t(y) * w + x) * 4]; }
    const float* at(int x, int y) const { return &px[(size_t(y) * w + x) * 4]; }
};

// Per-pixel velocity in pixels per frame.
struct FlowField {
    int w = 0, h = 0;
    std::vector<float> u, v;
    FlowField() = default;
    FlowField(int w_, int h_) : w(w_), h(h_), u(size_t(w_) * h_, 0.f), v(size_t(w_) * h_, 0.f) {}
};

struct Params {
    float shutterAngle = 180.f;  // degrees; 360 = blur spans the whole frame interval
    float shutterPhase = -90.f;  // degrees; start of the shutter window relative to the frame
    float blurScale = 1.f;       // extra multiplier on vector length
    int   maxSamples = 64;       // upper bound on taps per pixel
    bool  edgeAware = false;     // reach-weighted gather; big win with accurate vectors, neutral/worse with noisy ones
    // Flow estimation
    float smoothness = 0.1f;     // Horn-Schunck alpha (on 0..1 luma); higher = smoother vectors
    float edgeSharpen = 0.f;     // luma tolerance of the edge-guided vector filter (0 = off; slow at HD)
    int   flowMaxDim = 480;      // estimate motion on a copy no larger than this (0 = full res); vectors are scaled up
    int   iterations = 40;       // solver iterations per warp step
    int   warps = 2;             // warp steps per pyramid level
    int   maxLevels = 6;         // pyramid depth cap
};

// Flow that maps pixels of `a` to their position in `b` (pixels per frame step).
FlowField estimateFlow(const Image& a, const Image& b, const Params& p);

// Velocity of the current frame from its neighbours. Either neighbour may be
// null (first/last frame); with neither, the field is zero.
FlowField estimateVelocity(const Image& cur, const Image* prev, const Image* next, const Params& p);

// Blur `cur` along `vel`.
Image applyVectorBlur(const Image& cur, const FlowField& vel, const Params& p);

// Convenience: estimateVelocity + applyVectorBlur.
Image motionBlur(const Image& cur, const Image* prev, const Image* next, const Params& p,
                 FlowField* velocityOut = nullptr);

// Debug visualisation: hue = direction, brightness = magnitude / maxMagnitude.
Image flowToImage(const FlowField& f, float maxMagnitude);

}  // namespace mb
