#include "motion_blur.h"

#include <algorithm>
#include <cmath>
#ifdef __APPLE__
#include <dispatch/dispatch.h>
#endif

namespace mb {

namespace {

// Run f(row) for row in [0,n) on all cores: GCD on macOS (no extra install), OpenMP elsewhere.
template <class F>
void parallelRows(int n, F&& f) {
#ifdef __APPLE__
    dispatch_apply((size_t)n, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { f((int)i); });
#else
#pragma omp parallel for
    for (int i = 0; i < n; ++i) f(i);
#endif
}


struct Plane {
    int w = 0, h = 0;
    std::vector<float> d;
    Plane() = default;
    Plane(int w_, int h_) : w(w_), h(h_), d(size_t(w_) * h_, 0.f) {}
    float& operator()(int x, int y) { return d[size_t(y) * w + x]; }
    float operator()(int x, int y) const { return d[size_t(y) * w + x]; }
    float clamped(int x, int y) const {
        x = std::min(std::max(x, 0), w - 1);
        y = std::min(std::max(y, 0), h - 1);
        return d[size_t(y) * w + x];
    }
};

float bilinear(const Plane& p, float x, float y) {
    int x0 = int(std::floor(x)), y0 = int(std::floor(y));
    float fx = x - x0, fy = y - y0;
    float a = p.clamped(x0, y0), b = p.clamped(x0 + 1, y0);
    float c = p.clamped(x0, y0 + 1), d = p.clamped(x0 + 1, y0 + 1);
    return (a + (b - a) * fx) * (1 - fy) + (c + (d - c) * fx) * fy;
}

Plane lumaOf(const Image& im) {
    Plane p(im.w, im.h);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x) {
            const float* s = im.at(x, y);
            p(x, y) = 0.299f * s[0] + 0.587f * s[1] + 0.114f * s[2];
        }
    return p;
}

Plane blur3(const Plane& s) {
    Plane o(s.w, s.h);
    static const float k[3] = {0.25f, 0.5f, 0.25f};
    parallelRows(s.h, [&](int y) {
        for (int x = 0; x < s.w; ++x) {
            float acc = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) acc += k[i + 1] * k[j + 1] * s.clamped(x + i, y + j);
            o(x, y) = acc;
        }});
    return o;
}

Plane downsample(const Plane& s) {
    Plane o(std::max(1, s.w / 2), std::max(1, s.h / 2));
    Plane b = blur3(s);
    for (int y = 0; y < o.h; ++y)
        for (int x = 0; x < o.w; ++x) o(x, y) = b.clamped(2 * x, 2 * y);
    return o;
}

// Upsample a flow component to (w,h), scaling values by the size ratio.
Plane upsampleFlow(const Plane& s, int w, int h) {
    Plane o(w, h);
    float sx = float(s.w) / w, sy = float(s.h) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            o(x, y) = bilinear(s, (x + 0.5f) * sx - 0.5f, (y + 0.5f) * sy - 0.5f) / sx;
    // (sx == sy for our 2x pyramid up to rounding; vertical uses same scale below)
    return o;
}

Plane upsampleFlowV(const Plane& s, int w, int h) {
    Plane o(w, h);
    float sx = float(s.w) / w, sy = float(s.h) / h;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            o(x, y) = bilinear(s, (x + 0.5f) * sx - 0.5f, (y + 0.5f) * sy - 0.5f) / sy;
    return o;
}

}  // namespace

FlowField estimateFlow(const Image& a, const Image& b, const Params& prm) {
    const int W = a.w, H = a.h;
    FlowField out(W, H);
    if (W < 4 || H < 4 || b.w != W || b.h != H) return out;

    std::vector<Plane> pa{blur3(lumaOf(a))}, pb{blur3(lumaOf(b))};
    while (int(pa.size()) < prm.maxLevels && std::min(pa.back().w, pa.back().h) >= 24) {
        pa.push_back(downsample(pa.back()));
        pb.push_back(downsample(pb.back()));
    }

    const float alpha2 = prm.smoothness * prm.smoothness;
    Plane u, v;
    for (int lvl = int(pa.size()) - 1; lvl >= 0; --lvl) {
        const Plane& A = pa[lvl];
        const Plane& B = pb[lvl];
        if (u.d.empty()) {
            u = Plane(A.w, A.h);
            v = Plane(A.w, A.h);
        } else {
            Plane nu = upsampleFlow(u, A.w, A.h), nv = upsampleFlowV(v, A.w, A.h);
            u = std::move(nu);
            v = std::move(nv);
        }

        Plane Ix(A.w, A.h), Iy(A.w, A.h), It(A.w, A.h), du(A.w, A.h), dv(A.w, A.h);
        Plane ub(A.w, A.h), vb(A.w, A.h);
        for (int warp = 0; warp < prm.warps; ++warp) {
            parallelRows(A.h, [&](int y) {
                for (int x = 0; x < A.w; ++x) {
                    float wx = x + u(x, y), wy = y + v(x, y);
                    float bw = bilinear(B, wx, wy);
                    float bx = 0.5f * (bilinear(B, wx + 1, wy) - bilinear(B, wx - 1, wy));
                    float by = 0.5f * (bilinear(B, wx, wy + 1) - bilinear(B, wx, wy - 1));
                    float ax = 0.5f * (A.clamped(x + 1, y) - A.clamped(x - 1, y));
                    float ay = 0.5f * (A.clamped(x, y + 1) - A.clamped(x, y - 1));
                    Ix(x, y) = 0.5f * (ax + bx);
                    Iy(x, y) = 0.5f * (ay + by);
                    It(x, y) = bw - A(x, y);
                    du(x, y) = 0.f;
                    dv(x, y) = 0.f;
                }});
            for (int it = 0; it < prm.iterations; ++it) {
                parallelRows(A.h, [&](int y) {
                    for (int x = 0; x < A.w; ++x) {
                        ub(x, y) = 0.25f * (du.clamped(x - 1, y) + du.clamped(x + 1, y) +
                                            du.clamped(x, y - 1) + du.clamped(x, y + 1));
                        vb(x, y) = 0.25f * (dv.clamped(x - 1, y) + dv.clamped(x + 1, y) +
                                            dv.clamped(x, y - 1) + dv.clamped(x, y + 1));
                    }});
                parallelRows(A.h, [&](int y) {
                    for (int x = 0; x < A.w; ++x) {
                        float ix = Ix(x, y), iy = Iy(x, y);
                        float t = (ix * ub(x, y) + iy * vb(x, y) + It(x, y)) /
                                  (alpha2 + ix * ix + iy * iy);
                        du(x, y) = ub(x, y) - ix * t;
                        dv(x, y) = vb(x, y) - iy * t;
                    }});
            }
            for (size_t i = 0; i < u.d.size(); ++i) {
                u.d[i] += du.d[i];
                v.d[i] += dv.d[i];
            }
        }
    }
    out.u = std::move(u.d);
    out.v = std::move(v.d);
    return out;
}

// Joint-bilateral filter of the vectors, guided by the frame's luma: vectors are only averaged
// across pixels of similar colour, so motion boundaries snap to image edges instead of being
// smoothed across them.
static void sharpenFlowEdges(FlowField& f, const Image& guide, const Params& p) {
    if (p.edgeSharpen <= 0.f) return;
    const int W = f.w, H = f.h, R = 4;
    Plane g = lumaOf(guide);
    const float inv2r = 1.f / (2.f * p.edgeSharpen * p.edgeSharpen);
    const float inv2s = 1.f / (2.f * 2.5f * 2.5f);
    std::vector<float> ou(f.u.size()), ov(f.v.size());
    parallelRows(H, [&](int y) {
        for (int x = 0; x < W; ++x) {
            float su = 0, sv = 0, sw = 0, gc = g(x, y);
            for (int j = -R; j <= R; ++j)
                for (int i = -R; i <= R; ++i) {
                    int xx = std::min(std::max(x + i, 0), W - 1), yy = std::min(std::max(y + j, 0), H - 1);
                    float dg = g(xx, yy) - gc;
                    float w = std::exp(-(i * i + j * j) * inv2s - dg * dg * inv2r);
                    size_t k = size_t(yy) * W + xx;
                    su += w * f.u[k]; sv += w * f.v[k]; sw += w;
                }
            size_t o = size_t(y) * W + x;
            ou[o] = su / sw; ov[o] = sv / sw;
        }});
    f.u = std::move(ou);
    f.v = std::move(ov);
}

// Box-average downscale by an integer factor.
static Image boxDown(const Image& s, int k) {
    Image o((s.w + k - 1) / k, (s.h + k - 1) / k);
    parallelRows(o.h, [&](int y) {
        for (int x = 0; x < o.w; ++x) {
            float acc[4] = {0, 0, 0, 0};
            int n = 0;
            for (int j = 0; j < k && y * k + j < s.h; ++j)
                for (int i = 0; i < k && x * k + i < s.w; ++i, ++n) {
                    const float* px = s.at(x * k + i, y * k + j);
                    for (int c = 0; c < 4; ++c) acc[c] += px[c];
                }
            for (int c = 0; c < 4; ++c) o.at(x, y)[c] = acc[c] / n;
        }
    });
    return o;
}

FlowField estimateVelocity(const Image& cur, const Image* prev, const Image* next, const Params& p) {
    const int k = (p.flowMaxDim > 0) ? (std::max(cur.w, cur.h) + p.flowMaxDim - 1) / p.flowMaxDim : 1;
    if (k > 1) {
        // Motion is smooth, so estimate it on a reduced copy and scale the vectors back up.
        Image sc = boxDown(cur, k), sp, sn;
        if (prev) sp = boxDown(*prev, k);
        if (next) sn = boxDown(*next, k);
        Params q = p;
        q.flowMaxDim = 0;
        q.edgeSharpen = 0.f;
        FlowField small = estimateVelocity(sc, prev ? &sp : nullptr, next ? &sn : nullptr, q);
        FlowField full(cur.w, cur.h);
        Plane pu, pv;
        pu.w = pv.w = small.w; pu.h = pv.h = small.h;
        pu.d = small.u; pv.d = small.v;
        parallelRows(full.h, [&](int y) {
            for (int x = 0; x < full.w; ++x) {
                float sx = (x + 0.5f) / k - 0.5f, sy = (y + 0.5f) / k - 0.5f;
                full.u[size_t(y) * full.w + x] = bilinear(pu, sx, sy) * k;
                full.v[size_t(y) * full.w + x] = bilinear(pv, sx, sy) * k;
            }
        });
        sharpenFlowEdges(full, cur, p);
        return full;
    }
    FlowField vel(cur.w, cur.h);
    FlowField fwd, bwd;
    if (next) fwd = estimateFlow(cur, *next, p);   // cur -> next
    if (prev) bwd = estimateFlow(cur, *prev, p);   // cur -> prev (points backwards in time)
    for (size_t i = 0; i < vel.u.size(); ++i) {
        if (next && prev) {
            vel.u[i] = 0.5f * (fwd.u[i] - bwd.u[i]);
            vel.v[i] = 0.5f * (fwd.v[i] - bwd.v[i]);
        } else if (next) {
            vel.u[i] = fwd.u[i];
            vel.v[i] = fwd.v[i];
        } else if (prev) {
            vel.u[i] = -bwd.u[i];
            vel.v[i] = -bwd.v[i];
        }
    }
    sharpenFlowEdges(vel, cur, p);
    return vel;
}

namespace {

inline const float* clampedPx(const Image& im, int x, int y) {
    x = std::min(std::max(x, 0), im.w - 1);
    y = std::min(std::max(y, 0), im.h - 1);
    return im.at(x, y);
}

inline void sampleBilinear(const Image& im, float sx, float sy, float* out) {
    int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
    float fx = sx - x0, fy = sy - y0;
    const float *a = clampedPx(im, x0, y0), *b = clampedPx(im, x0 + 1, y0);
    const float *c = clampedPx(im, x0, y0 + 1), *d = clampedPx(im, x0 + 1, y0 + 1);
    for (int k = 0; k < 4; ++k)
        out[k] = (a[k] + (b[k] - a[k]) * fx) * (1 - fy) + (c[k] + (d[k] - c[k]) * fx) * fy;
}

}  // namespace

Image applyVectorBlur(const Image& cur, const FlowField& vel, const Params& p) {
    Image out(cur.w, cur.h);
    const int W = cur.w, H = cur.h;
    const float t0 = p.shutterPhase / 360.f;
    const float t1 = t0 + p.shutterAngle / 360.f;
    const float span = std::fabs(t1 - t0);
    const float reachT = std::max(std::fabs(t0), std::fabs(t1));  // how far (in frames) a pixel smears
    const int maxN = std::max(1, p.maxSamples);

    // Scaled velocities and per-pixel reach (px) = how far each pixel's own blur extends.
    std::vector<float> vx(vel.u.size()), vy(vel.u.size()), reach(vel.u.size());
    float maxReach = 0.f;
    for (size_t i = 0; i < vx.size(); ++i) {
        vx[i] = vel.u[i] * p.blurScale;
        vy[i] = vel.v[i] * p.blurScale;
        reach[i] = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i]) * reachT;
        maxReach = std::max(maxReach, reach[i]);
    }

    // Dominant-velocity map: per-tile max, then 3x3 tile neighbourhood max.
    const int T = std::min(64, std::max(4, int(std::ceil(maxReach))));
    const int tw = (W + T - 1) / T, th = (H + T - 1) / T;
    std::vector<float> tu(size_t(tw) * th, 0.f), tv(tu.size(), 0.f), nu(tu.size(), 0.f), nv(tu.size(), 0.f);
    if (p.edgeAware) {
        auto mag2 = [](float a, float b) { return a * a + b * b; };
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                size_t i = size_t(y) * W + x, t = size_t(y / T) * tw + x / T;
                if (mag2(vx[i], vy[i]) > mag2(tu[t], tv[t])) { tu[t] = vx[i]; tv[t] = vy[i]; }
            }
        for (int ty = 0; ty < th; ++ty)
            for (int tx = 0; tx < tw; ++tx) {
                size_t t = size_t(ty) * tw + tx;
                for (int j = -1; j <= 1; ++j)
                    for (int i = -1; i <= 1; ++i) {
                        int ax = tx + i, ay = ty + j;
                        if (ax < 0 || ay < 0 || ax >= tw || ay >= th) continue;
                        size_t s = size_t(ay) * tw + ax;
                        if (mag2(tu[s], tv[s]) > mag2(nu[t], nv[t])) { nu[t] = tu[s]; nv[t] = tv[s]; }
                    }
            }
    }

    parallelRows(H, [&](int y) { {
        for (int x = 0; x < W; ++x) {
            const size_t i = size_t(y) * W + x;
            float* dst = out.at(x, y);
            const float* c0 = cur.at(x, y);

            float ux = vx[i], uy = vy[i];
            if (p.edgeAware) {
                // Sample along the neighbourhood's dominant motion unless this pixel is itself
                // (nearly) as fast, in which case its own direction wins.
                size_t t = size_t(y / T) * tw + x / T;
                float own2 = ux * ux + uy * uy, dom2 = nu[t] * nu[t] + nv[t] * nv[t];
                if (own2 < 0.5625f * dom2) { ux = nu[t]; uy = nv[t]; }
            }
            float vmag = std::sqrt(ux * ux + uy * uy);
            float len = vmag * span;
            if (len < 0.05f) {
                std::copy(c0, c0 + 4, dst);
                continue;
            }
            int n = std::min(maxN, std::max(2, int(std::ceil(len)) + 1));
            float acc[4] = {0, 0, 0, 0};
            float wsum = 0.f;
            for (int s = 0; s < n; ++s) {
                float tt = t0 + (t1 - t0) * (s + 0.5f) / n;
                float sx = x + ux * tt, sy = y + uy * tt;
                float tap[4];
                sampleBilinear(cur, sx, sy, tap);
                float w = 1.f;
                if (p.edgeAware) {
                    // Scatter-as-gather: a faster tap (in front) contributes only if its own blur
                    // reaches this pixel; a slower tap (behind) contributes while it lies within
                    // this pixel's own blur range.
                    int qx = std::min(std::max(int(std::floor(sx + 0.5f)), 0), W - 1);
                    int qy = std::min(std::max(int(std::floor(sy + 0.5f)), 0), H - 1);
                    size_t q = size_t(qy) * W + qx;
                    float r = std::fabs(tt) * vmag;
                    float qm = std::sqrt(vx[q] * vx[q] + vy[q] * vy[q]);
                    float om = std::sqrt(vx[i] * vx[i] + vy[i] * vy[i]);
                    if (qm <= om) {
                        w = std::min(1.f, std::max(0.f, reach[i] - r + 0.5f));
                    } else {
                        w = std::min(1.f, std::max(0.f, reach[q] - r + 0.5f));
                        float al = (vx[q] * ux + vy[q] * uy) / (qm * vmag);
                        w *= std::min(1.f, std::max(0.f, al * 2.f + 0.5f));
                    }
                }
                for (int k = 0; k < 4; ++k) acc[k] += w * tap[k];
                wsum += w;
            }
            // Rejected taps are replaced by this pixel's own colour.
            for (int k = 0; k < 4; ++k) dst[k] = (acc[k] + (float(n) - wsum) * c0[k]) / n;
        }
    }});
    return out;
}

Image motionBlur(const Image& cur, const Image* prev, const Image* next, const Params& p,
                 FlowField* velocityOut) {
    FlowField vel = estimateVelocity(cur, prev, next, p);
    Image out = applyVectorBlur(cur, vel, p);
    if (velocityOut) *velocityOut = std::move(vel);
    return out;
}

Image flowToImage(const FlowField& f, float maxMag) {
    Image out(f.w, f.h);
    for (int y = 0; y < f.h; ++y)
        for (int x = 0; x < f.w; ++x) {
            size_t i = size_t(y) * f.w + x;
            float mag = std::sqrt(f.u[i] * f.u[i] + f.v[i] * f.v[i]);
            float hue = (std::atan2(f.v[i], f.u[i]) / 3.14159265f * 0.5f + 0.5f) * 6.f;
            float val = std::min(1.f, mag / std::max(maxMag, 1e-6f));
            auto ch = [&](float n) {
                float k = std::fmod(n + hue, 6.f);
                return val * (1.f - std::max(0.f, std::min(std::min(k, 4.f - k), 1.f)));
            };
            float* d = out.at(x, y);
            d[0] = ch(5); d[1] = ch(3); d[2] = ch(1); d[3] = 1.f;
        }
    return out;
}

}  // namespace mb
