#include "motion_blur.h"

#include <algorithm>
#include <cmath>

namespace mb {

namespace {

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
#pragma omp parallel for
    for (int y = 0; y < s.h; ++y)
        for (int x = 0; x < s.w; ++x) {
            float acc = 0;
            for (int j = -1; j <= 1; ++j)
                for (int i = -1; i <= 1; ++i) acc += k[i + 1] * k[j + 1] * s.clamped(x + i, y + j);
            o(x, y) = acc;
        }
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
#pragma omp parallel for
            for (int y = 0; y < A.h; ++y)
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
                }
            for (int it = 0; it < prm.iterations; ++it) {
#pragma omp parallel for
                for (int y = 0; y < A.h; ++y)
                    for (int x = 0; x < A.w; ++x) {
                        ub(x, y) = 0.25f * (du.clamped(x - 1, y) + du.clamped(x + 1, y) +
                                            du.clamped(x, y - 1) + du.clamped(x, y + 1));
                        vb(x, y) = 0.25f * (dv.clamped(x - 1, y) + dv.clamped(x + 1, y) +
                                            dv.clamped(x, y - 1) + dv.clamped(x, y + 1));
                    }
#pragma omp parallel for
                for (int y = 0; y < A.h; ++y)
                    for (int x = 0; x < A.w; ++x) {
                        float ix = Ix(x, y), iy = Iy(x, y);
                        float t = (ix * ub(x, y) + iy * vb(x, y) + It(x, y)) /
                                  (alpha2 + ix * ix + iy * iy);
                        du(x, y) = ub(x, y) - ix * t;
                        dv(x, y) = vb(x, y) - iy * t;
                    }
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

FlowField estimateVelocity(const Image& cur, const Image* prev, const Image* next, const Params& p) {
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
    return vel;
}

Image applyVectorBlur(const Image& cur, const FlowField& vel, const Params& p) {
    Image out(cur.w, cur.h);
    const float t0 = p.shutterPhase / 360.f;
    const float t1 = t0 + p.shutterAngle / 360.f;
    const int maxN = std::max(1, p.maxSamples);

#pragma omp parallel for
    for (int y = 0; y < cur.h; ++y) {
        for (int x = 0; x < cur.w; ++x) {
            size_t i = size_t(y) * cur.w + x;
            float vx = vel.u[i] * p.blurScale, vy = vel.v[i] * p.blurScale;
            float len = std::sqrt(vx * vx + vy * vy) * std::fabs(t1 - t0);
            float* dst = out.at(x, y);
            if (len < 0.05f) {
                std::copy(cur.at(x, y), cur.at(x, y) + 4, dst);
                continue;
            }
            int n = std::min(maxN, std::max(2, int(std::ceil(len)) + 1));
            float acc[4] = {0, 0, 0, 0};
            for (int s = 0; s < n; ++s) {
                float t = t0 + (t1 - t0) * (s + 0.5f) / n;
                float sx = x + vx * t, sy = y + vy * t;
                int x0 = int(std::floor(sx)), y0 = int(std::floor(sy));
                float fx = sx - x0, fy = sy - y0;
                auto cl = [&](int xx, int yy) {
                    xx = std::min(std::max(xx, 0), cur.w - 1);
                    yy = std::min(std::max(yy, 0), cur.h - 1);
                    return cur.at(xx, yy);
                };
                const float *a = cl(x0, y0), *b = cl(x0 + 1, y0), *c = cl(x0, y0 + 1), *d = cl(x0 + 1, y0 + 1);
                for (int k = 0; k < 4; ++k)
                    acc[k] += (a[k] + (b[k] - a[k]) * fx) * (1 - fy) + (c[k] + (d[k] - c[k]) * fx) * fy;
            }
            for (int k = 0; k < 4; ++k) dst[k] = acc[k] / n;
        }
    }
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
