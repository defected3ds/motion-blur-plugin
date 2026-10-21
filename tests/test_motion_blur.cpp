#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

#include "../core/motion_blur.h"

using namespace mb;

static int failures = 0;
#define CHECK(cond, ...)                                  \
    do {                                                  \
        if (!(cond)) {                                    \
            ++failures;                                   \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__); \
            std::printf(__VA_ARGS__);                     \
            std::printf("\n");                            \
        }                                                 \
    } while (0)

// Aperiodic multi-scale texture (random Gaussian blobs) so flow is well defined.
struct Blob { float x, y, inv2s2, amp; };
static const std::vector<Blob>& blobs() {
    static std::vector<Blob> b = [] {
        std::vector<Blob> v;
        unsigned s = 12345;
        auto rnd = [&] { s = s * 1664525u + 1013904223u; return (s >> 8) / float(1 << 24); };
        for (int i = 0; i < 90; ++i) {
            float sg = 3.f + 10.f * rnd();
            v.push_back({-60.f + 250.f * rnd(), -60.f + 220.f * rnd(), 1.f / (2 * sg * sg), (rnd() - 0.5f) * 0.6f});
        }
        return v;
    }();
    return b;
}
static float tex(float x, float y) {
    float v = 0.5f;
    for (const Blob& b : blobs()) {
        float dx = x - b.x, dy = y - b.y;
        v += b.amp * std::exp(-(dx * dx + dy * dy) * b.inv2s2);
    }
    return v;
}

static Image frame(int w, int h, float dx, float dy) {
    Image im(w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            float v = tex(x - dx, y - dy);
            float* p = im.at(x, y);
            p[0] = v; p[1] = v * 0.8f; p[2] = v * 0.6f; p[3] = 1.f;
        }
    return im;
}

static void writePPM(const char* path, const Image& im) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return;
    std::fprintf(f, "P6\n%d %d\n255\n", im.w, im.h);
    for (int y = 0; y < im.h; ++y)
        for (int x = 0; x < im.w; ++x)
            for (int c = 0; c < 3; ++c) {
                float v = std::fmin(1.f, std::fmax(0.f, im.at(x, y)[c]));
                std::fputc(int(v * 255.f + 0.5f), f);
            }
    std::fclose(f);
}

int main() {
    const int W = 128, H = 96;
    Params p;

    // Pattern moves (+9, +3) px per frame.
    const float mx = 9.f, my = 3.f;
    Image prev = frame(W, H, -mx, -my), cur = frame(W, H, 0, 0), next = frame(W, H, mx, my);

    FlowField vel = estimateVelocity(cur, &prev, &next, p);
    double eu = 0, ev = 0;
    int n = 0;
    for (int y = 16; y < H - 16; ++y)
        for (int x = 16; x < W - 16; ++x) {
            eu += std::fabs(vel.u[size_t(y) * W + x] - mx);
            ev += std::fabs(vel.v[size_t(y) * W + x] - my);
            ++n;
        }
    eu /= n; ev /= n;
    std::printf("flow mean abs error: u=%.3f v=%.3f px (truth %.0f,%.0f)\n", eu, ev, mx, my);
    CHECK(eu < 0.6 && ev < 0.6, "flow error too large");

    // Blur vs. analytic box average of the texture along the motion path.
    p.shutterAngle = 180.f; p.shutterPhase = -90.f;
    Image out = applyVectorBlur(cur, [&] {
        FlowField f(W, H);
        std::fill(f.u.begin(), f.u.end(), mx);
        std::fill(f.v.begin(), f.v.end(), my);
        return f;
    }(), p);
    double rms = 0, rmsUnblurred = 0;
    n = 0;
    for (int y = 16; y < H - 16; ++y)
        for (int x = 16; x < W - 16; ++x) {
            double ref = 0;
            const int K = 120;
            for (int s = 0; s < K; ++s) {
                float t = -0.25f + 0.5f * (s + 0.5f) / K;
                ref += tex(x + mx * t, y + my * t);
            }
            ref /= K;
            double d = out.at(x, y)[0] - ref, d0 = cur.at(x, y)[0] - ref;
            rms += d * d; rmsUnblurred += d0 * d0; ++n;
        }
    rms = std::sqrt(rms / n); rmsUnblurred = std::sqrt(rmsUnblurred / n);
    std::printf("blur rms vs analytic: %.5f (unblurred would be %.5f)\n", rms, rmsUnblurred);
    CHECK(rms < 0.01 && rms < rmsUnblurred * 0.2, "blur does not match analytic box blur");

    // Zero motion must be an identity.
    Image still = motionBlur(cur, &cur, &cur, p);
    double maxd = 0;
    for (size_t i = 0; i < still.px.size(); ++i) maxd = std::fmax(maxd, std::fabs(still.px[i] - cur.px[i]));
    CHECK(maxd < 1e-3, "static frames altered by %g", maxd);

    // Edge frames (only one neighbour) still produce the right direction.
    FlowField first = estimateVelocity(cur, nullptr, &next, p);
    FlowField last = estimateVelocity(cur, &prev, nullptr, p);
    float fu = first.u[size_t(H / 2) * W + W / 2], lu = last.u[size_t(H / 2) * W + W / 2];
    std::printf("one-sided flow centre u: first=%.2f last=%.2f\n", fu, lu);
    CHECK(std::fabs(fu - mx) < 1.f && std::fabs(lu - mx) < 1.f, "one-sided flow wrong");

    Image full = motionBlur(cur, &prev, &next, p);
    writePPM("cur.ppm", cur);
    writePPM("blurred.ppm", full);
    writePPM("flow.ppm", flowToImage(vel, 12.f));

    std::printf(failures ? "FAILED\n" : "all tests passed\n");
    return failures ? 1 : 0;
}
