// mbvideo: add vector motion blur to a video file. Uses the ffmpeg/ffprobe on PATH for I/O.
//   mbvideo in.mp4 out.mp4 [--shutter 180] [--phase -90] [--amount 100] [--samples 64]
//                          [--smooth 0.1] [--edge-aware] [--vectors]
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>

#include "motion_blur.h"

static std::string shq(const std::string& s) {  // POSIX single-quote escaping
    std::string o = "'";
    for (char c : s) o += (c == '\'') ? std::string("'\\''") : std::string(1, c);
    return o + "'";
}

static bool readFrame(FILE* f, int w, int h, mb::Image& im) {
    std::vector<unsigned char> buf(size_t(w) * h * 3);
    if (std::fread(buf.data(), 1, buf.size(), f) != buf.size()) return false;
    im = mb::Image(w, h);
    for (size_t i = 0; i < size_t(w) * h; ++i) {
        im.px[i * 4 + 0] = buf[i * 3 + 0] / 255.f;
        im.px[i * 4 + 1] = buf[i * 3 + 1] / 255.f;
        im.px[i * 4 + 2] = buf[i * 3 + 2] / 255.f;
        im.px[i * 4 + 3] = 1.f;
    }
    return true;
}

static void writeFrame(FILE* f, const mb::Image& im) {
    std::vector<unsigned char> buf(size_t(im.w) * im.h * 3);
    for (size_t i = 0; i < size_t(im.w) * im.h; ++i)
        for (int c = 0; c < 3; ++c) {
            float v = im.px[i * 4 + c];
            buf[i * 3 + c] = (unsigned char)(v <= 0.f ? 0 : v >= 1.f ? 255 : v * 255.f + 0.5f);
        }
    std::fwrite(buf.data(), 1, buf.size(), f);
}

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
            "usage: %s in.mp4 out.mp4 [--shutter 180] [--phase -90] [--amount 100] [--samples 64]\n"
            "          [--smooth 0.1] [--edge-aware] [--vectors]\n", argv[0]);
        return 2;
    }
    std::string in = argv[1], out = argv[2];
    mb::Params p;
    bool vectors = false;
    for (int i = 3; i < argc; ++i) {
        std::string a = argv[i];
        auto val = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--shutter") p.shutterAngle = float(std::atof(val()));
        else if (a == "--phase") p.shutterPhase = float(std::atof(val()));
        else if (a == "--amount") p.blurScale = float(std::atof(val())) / 100.f;
        else if (a == "--samples") p.maxSamples = std::atoi(val());
        else if (a == "--smooth") p.smoothness = float(std::atof(val()));
        else if (a == "--edge-aware") p.edgeAware = true;
        else if (a == "--vectors") vectors = true;
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    // Probe size and frame rate.
    std::string probe = "ffprobe -v error -select_streams v:0 -show_entries stream=width,height,r_frame_rate "
                        "-of csv=p=0 " + shq(in);
    FILE* pp = popen(probe.c_str(), "r");
    int w = 0, h = 0, fn = 0, fd = 1;
    if (!pp || std::fscanf(pp, "%d,%d,%d/%d", &w, &h, &fn, &fd) < 3 || w <= 0 || h <= 0) {
        std::fprintf(stderr, "could not probe %s (is ffprobe installed? brew install ffmpeg)\n", in.c_str());
        return 1;
    }
    pclose(pp);
    if (fd <= 0) fd = 1;

    FILE* dec = popen(("ffmpeg -v error -i " + shq(in) + " -f rawvideo -pix_fmt rgb24 -").c_str(), "r");
    char fps[64];
    std::snprintf(fps, sizeof fps, "%d/%d", fn, fd);
    // Video from stdin, audio (if any) copied from the original.
    std::string enc = "ffmpeg -v error -y -f rawvideo -pix_fmt rgb24 -s " + std::to_string(w) + "x" +
                      std::to_string(h) + " -r " + fps + " -i - -i " + shq(in) +
                      " -map 0:v -map 1:a? -c:v libx264 -crf 16 -preset medium -pix_fmt yuv420p -c:a copy -shortest " +
                      shq(out);
    FILE* encp = popen(enc.c_str(), "w");
    if (!dec || !encp) { std::fprintf(stderr, "could not start ffmpeg\n"); return 1; }

    // Sliding window prev / cur / next.
    std::deque<mb::Image> win;
    mb::Image im;
    auto emit = [&](const mb::Image* prev, const mb::Image& cur, const mb::Image* next) {
        mb::FlowField vel;
        mb::Image r = mb::motionBlur(cur, prev, next, p, &vel);
        if (vectors) {
            float m = 1.f;
            for (size_t k = 0; k < vel.u.size(); ++k) m = std::max(m, std::hypot(vel.u[k], vel.v[k]));
            r = mb::flowToImage(vel, m);
        }
        writeFrame(encp, r);
    };
    int n = 0;
    while (readFrame(dec, w, h, im)) {
        win.push_back(std::move(im));
        if (win.size() == 3) {
            emit(&win[0], win[1], &win[2]);
            win.pop_front();
            ++n;
        } else if (win.size() == 2) {
            emit(nullptr, win[0], &win[1]);  // first frame
            ++n;
        }
        if (n % 10 == 0) std::fprintf(stderr, "\rframes: %d", n);
    }
    if (win.size() == 2) { emit(&win[0], win[1], nullptr); ++n; }       // last frame
    else if (win.size() == 1 && n == 0) { emit(nullptr, win[0], nullptr); ++n; }  // single frame
    pclose(dec);
    int rc = pclose(encp);
    std::fprintf(stderr, "\rdone: %d frames -> %s\n", n, out.c_str());
    return rc == 0 ? 0 : 1;
}
