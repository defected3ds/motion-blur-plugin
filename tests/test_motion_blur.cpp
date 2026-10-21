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


// Moving square over a static background. Ground truth = brute-force temporal supersampling.
static void edgeTest() {
    const int W = 128, H = 96, S = 40;
    const float vxp = 12.f, x0 = 40.f, y0 = 28.f;
    auto sceneAt = [&](int x, int y, float t, bool* fg) {
        float sx0 = x0 + vxp * t;
        bool in = x >= sx0 && x < sx0 + S && y >= y0 && y < y0 + S;
        if (fg) *fg = in;
        return in ? 0.9f - 0.3f * tex(x - vxp * t + 70, y + 20) : 0.15f + 0.3f * tex(x, y);
    };
    Image cur(W, H);
    FlowField vel(W, H);
    std::vector<unsigned char> mask(size_t(W) * H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            bool fg;
            float v = sceneAt(x, y, 0, &fg);
            float* px = cur.at(x, y);
            px[0] = px[1] = px[2] = v; px[3] = 1.f;
            mask[size_t(y) * W + x] = fg;
            vel.u[size_t(y) * W + x] = fg ? vxp : 0.f;
        }
    Params p;
    p.edgeAware = true;  // 180 degree shutter, centred (defaults)
    Image truth(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x) {
            double a = 0; const int K = 96;
            for (int s = 0; s < K; ++s) a += sceneAt(x, y, -0.25f + 0.5f * (s + 0.5f) / K, nullptr);
            float* px = truth.at(x, y);
            px[0] = px[1] = px[2] = float(a / K); px[3] = 1.f;
        }
    auto zoneRms = [&](const Image& im, int zone) {  // 0 all, 1 bg band outside square, 2 fg
        double s = 0; int n = 0;
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                bool fg = mask[size_t(y) * W + x];
                bool nearBg = false;
                if (!fg)
                    for (int dx = -5; dx <= 5 && !nearBg; ++dx) {
                        int xx = x + dx;
                        nearBg = xx >= 0 && xx < W && mask[size_t(y) * W + xx];
                    }
                if ((zone == 1 && !nearBg) || (zone == 2 && !fg)) continue;
                double d = im.at(x, y)[0] - truth.at(x, y)[0];
                s += d * d; ++n;
            }
        return std::sqrt(s / std::max(n, 1));
    };
    Params po = p;
    po.edgeAware = false;
    Image oldI = applyVectorBlur(cur, vel, po), newI = applyVectorBlur(cur, vel, p);
    double o1 = zoneRms(oldI, 1), n1 = zoneRms(newI, 1), o2 = zoneRms(oldI, 2), n2 = zoneRms(newI, 2);
    std::printf("edge test (true vectors)  bg band: old %.4f new %.4f | fg: old %.4f new %.4f | all: old %.4f new %.4f\n",
                o1, n1, o2, n2, zoneRms(oldI, 0), zoneRms(newI, 0));
    CHECK(n1 < o1 * 0.6, "edge-aware did not reduce error outside the silhouette");
    CHECK(n2 <= o2 * 1.05, "edge-aware made the object interior worse");
    CHECK(zoneRms(newI, 0) < zoneRms(oldI, 0), "edge-aware worse overall");
    // Full pipeline with estimated (not ground-truth) flow.
    auto frameAt = [&](float tt) {
        Image im(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x) {
                float v = sceneAt(x, y, tt, nullptr);
                float* px = im.at(x, y);
                px[0] = px[1] = px[2] = v; px[3] = 1.f;
            }
        return im;
    };
    Image fp = frameAt(-1.f), fn = frameAt(1.f);
    Image oldE = motionBlur(cur, &fp, &fn, po), newE = motionBlur(cur, &fp, &fn, p);
    std::printf("edge test (estimated flow) bg band: old %.4f new %.4f | fg: old %.4f new %.4f | all: old %.4f new %.4f\n",
                zoneRms(oldE, 1), zoneRms(newE, 1), zoneRms(oldE, 2), zoneRms(newE, 2), zoneRms(oldE, 0), zoneRms(newE, 0));
    // With estimated flow the gain disappears (vectors are smeared across the object edge), so only
    // guard against a meaningful regression.
    CHECK(zoneRms(newE, 0) <= zoneRms(oldE, 0) * 1.05, "edge-aware regressed with estimated flow");
    writePPM("edge_old.ppm", oldI);
    writePPM("edge_new.ppm", newI);
    writePPM("edge_truth.ppm", truth);
}

int main() {
    edgeTest();
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
