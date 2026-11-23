// Headless test tools (no window): --list-devices, --bench, --dump, --check-colors, --check-hud,
// --check-quality. The test pattern can render the exact in-between frame, so engines are scored
// against ground truth.
#include "Audio.h"
#include "Capture.h"
#include "Hud.h"
#include "Interpolators.h"
#include "Pipeline.h"
#include "Synthetic.h"
#include <algorithm>
#include <map>
#include <cmath>

static void printSupport() { logf("%s", EngineSupport::describe().c_str()); }

int runListDevices() {
    logf("%s", CaptureSource::describeDevices().c_str());
    logf("Audio capture endpoints:");
    for (auto& n : AudioCapture::endpoints()) logf("  %s", n.c_str());
    printSupport();
    return 0;
}

/// Renders frame `index` of the test pattern as an RGBA working frame (like the pipeline produces).
static FramePtr syntheticFrame(SyntheticSource& syn, int index, double fps) {
    auto& gpu = Gpu::get();
    auto nv12 = syn.render(index, fps);
    if (!nv12) return nullptr;
    auto rgba = gpu.makeImage(nv12->w, nv12->h, DXGI_FORMAT_R8G8B8A8_UNORM);
    gpu.nv12ToRgba(*nv12, *rgba, syn.yuv);
    auto f = std::make_shared<VideoFrame>();
    f->image = rgba; f->time = index / fps;
    return f;
}

static const EngineKind kEngines[] = {EngineKind::Blend, EngineKind::Fast, EngineKind::Quality};

int runBench() {
    printSupport();
    int w = g_opt.width > 0 ? g_opt.width : 1920, h = g_opt.height > 0 ? g_opt.height : 1080;
    SyntheticSource syn(30, w, h);
    syn.scene = g_opt.scene;
    std::vector<FramePtr> frames;
    RepeatDetector det;
    for (int i = 0; i < 32; i++) {
        auto f = syntheticFrame(syn, i, 30);
        if (!f) continue;
        det.isRepeat(*f); det.accept(); f->motion = det.lastDiff;
        frames.push_back(f);
    }
    logf("Benchmark at %dx%d, %zu source frames (scene %s, HUD stabiliser %s)", w, h, frames.size(), g_opt.scene.c_str(), g_hudLock ? "on" : "off");
    std::vector<EngineKind> engines(std::begin(kEngines), std::end(kEngines));
    if (EngineSupport::isSupported(EngineKind::Vfg)) engines.push_back(EngineKind::Vfg);
    if (const char* e = getenv("FG_ENGINES")) {
        engines.clear(); std::string s = e; size_t p = 0;
        while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); if (auto k = engineFromId(s.substr(p, q - p))) engines.push_back(*k); p = q + 1; }
    }
    for (EngineKind k : engines) {
        static const std::vector<int> mults = getenv("FG_MULTS") ? std::vector<int>{2, 4, 6, 8} : std::vector<int>{2, 3, 4};
        for (int mult : mults) {
            std::vector<float> phases; for (int i = 1; i < mult; i++) phases.push_back(float(i) / mult);
            std::string err;
            auto interp = makeInterpolator(k, err);
            if (!interp) { logf("  %-40s x%d: FAILED — %s", engineLabel(k), mult, err.c_str()); continue; }
            std::vector<double> times;
            for (size_t i = 1; i < frames.size(); i++) {
                double t0 = hostNow();
                std::vector<ImagePtr> outs;
                if (i > 2) GpuProfiler::get().begin();
                if (!interp->interpolate(*frames[i - 1], *frames[i], phases, outs, err)) { logf("  failed: %s", err.c_str()); break; }
                Gpu::get().waitIdle();
                GpuProfiler::get().end();
                if (outs.size() != phases.size()) { logf("  got %zu outputs, expected %zu", outs.size(), phases.size()); break; }
                if (i > 2) times.push_back((hostNow() - t0) * 1000);   // skip warm-up
            }
            if (times.empty()) continue;
            std::sort(times.begin(), times.end());
            double avg = 0; for (double t : times) avg += t; avg /= times.size();
            logf("  %-40s x%d  avg %6.2f ms   p95 %6.2f ms   max %6.2f ms   (budget at 30 fps: 33.3 ms)",
                 engineLabel(k), mult, avg, times[size_t(times.size() * 0.95)], times.back());
            GpuProfiler::get().report();
        }
    }
    return 0;
}

int runDump(const std::string& dir) {
    printSupport();
    CreateDirectoryW(widen(dir).c_str(), nullptr);
    SyntheticSource syn(30, 1920, 1080);
    syn.scene = g_opt.scene;
    auto& gpu = Gpu::get();
    auto a = syntheticFrame(syn, 40, 30), b = syntheticFrame(syn, 41, 30);
    gpu.savePNG(*a->image, dir + "\\0_prev.png");
    gpu.savePNG(*b->image, dir + "\\2_next.png");
    auto truth = syn.renderRGBA(81, 60);
    gpu.savePNG(*truth, dir + "\\1_mid_truth.png");
    for (EngineKind k : kEngines) {
        std::string err;
        auto interp = makeInterpolator(k, err);
        std::vector<ImagePtr> outs;
        if (interp && interp->interpolate(*a, *b, {0.5f}, outs, err) && !outs.empty()) {
            gpu.savePNG(*outs[0], strf("%s\\1_mid_%s.png", dir.c_str(), engineId(k)));
            logf("  wrote 1_mid_%s.png", engineId(k));
        } else logf("  %s: %s", engineId(k), err.c_str());
    }
    logf("  wrote %s", dir.c_str());
    return 0;
}

/// Checks that frames look identical on screen to the source: the test pattern's own RGBA
/// rendering vs the NV12 → RGBA path every captured frame takes, and generated frames at a
/// phase close to 0 vs the real frame.
int runColorCheck() {
    printSupport();
    auto& gpu = Gpu::get();
    SyntheticSource syn(30, 1920, 1080);
    auto truth = syn.renderRGBA(40, 30);
    auto real = syntheticFrame(syn, 40, 30), next = syntheticFrame(syn, 41, 30);
    std::vector<uint8_t> t, r;
    gpu.readRGBA(*truth, t); gpu.readRGBA(*real->image, r);
    auto compare = [&](const std::vector<uint8_t>& x, const std::vector<uint8_t>& y, const char* what) {
        double sx[3] = {}, sy[3] = {}, diff = 0, worst = 0; int n = 0;
        for (int yy = 20; yy < 1060; yy += 24) for (int xx = 20; xx < 1900; xx += 24) {
            size_t o = (size_t(yy) * 1920 + xx) * 4;
            double d = 0;
            for (int c = 0; c < 3; c++) { sx[c] += x[o + c]; sy[c] += y[o + c]; d += std::abs(double(x[o + c]) - y[o + c]); }
            d /= 3; diff += d; worst = std::max(worst, d); n++;
        }
        logf("%s: mean RGB %.1f %.1f %.1f vs %.1f %.1f %.1f | mean abs diff %.2f /255 | worst sample %.1f /255",
             what, sx[0] / n, sx[1] / n, sx[2] / n, sy[0] / n, sy[1] / n, sy[2] / n, diff / n, worst);
        return diff / n;
    };
    double d1 = compare(t, r, "NV12 path vs source");
    double d2 = 0;
    for (EngineKind k : {EngineKind::Fast, EngineKind::Quality}) {
        std::string err;
        auto interp = makeInterpolator(k, err);
        std::vector<ImagePtr> outs;
        bool old = g_hudLock; g_hudLock = false;
        if (interp && interp->interpolate(*real, *next, {0.02f}, outs, err) && !outs.empty()) {
            std::vector<uint8_t> g; gpu.readRGBA(*outs[0], g);
            d2 = std::max(d2, compare(r, g, strf("%s output at phase 0.02 vs real frame", engineId(k)).c_str()));
        }
        g_hudLock = old;
    }
    logf(d1 < 2.0 && d2 < 4.0 ? "PASS" : "FAIL");
    return d1 < 2.0 && d2 < 4.0 ? 0 : 1;
}

static std::vector<uint8_t> lumaOf(const GpuImage& img) { std::vector<uint8_t> l; Gpu::get().readLuma(img, l); return l; }

/// Scores HUD handling against ground truth: error on the UI panel and on the rest of the picture.
int runHudCheck() {
    printSupport();
    const int W = 1920, H = 1080;
    SyntheticSource syn(30, W, H);
    const char* sc = getenv("FG_SCENE");
    syn.scene = sc ? sc : "hud";
    syn.noise = getenv("FG_NOISE") != nullptr || g_opt.noise;
    RepeatDetector det;
    std::vector<FramePtr> frames;
    std::vector<std::vector<uint8_t>> truth;
    int total = syn.scene == "stopgo" ? 130 : 60;
    for (int i = 0; i < total; i++) {
        auto f = syntheticFrame(syn, i, 30);
        det.isRepeat(*f); det.accept(); f->motion = det.lastDiff;
        frames.push_back(f);
        truth.push_back(lumaOf(*syn.renderRGBA(2 * i + 1, 60)));   // exact frame halfway between i and i+1 (noise-free)
    }
    D2D1_RECT_F panel = SyntheticSource::hudPanel(float(W), float(H));
    int r0 = int(panel.top), r1 = int(panel.bottom), c0 = int(panel.left), c1 = int(panel.right);
    auto isUI = [&](int r, int c) { return (r >= r0 - 40 && r < r1 + 40 && c >= c0 - 40 && c < c1 + 40) || (r < 150 && c < 600); };
    for (EngineKind k : {EngineKind::Fast, EngineKind::Quality}) {
        logf("%s:  error vs ground truth (/255, lower is better)", engineLabel(k));
        for (bool lock : {false, true}) {
            g_hudLock = lock;
            HudStabilizer::get().reset();
            std::string err;
            auto interp = makeInterpolator(k, err);
            double uiErr = 0, uiN = 0, worldErr = 0, worldN = 0, worstBlock = 0;
            for (size_t i = 0; i + 1 < frames.size(); i++) {
                std::vector<ImagePtr> outs;
                if (!interp->interpolate(*frames[i], *frames[i + 1], {0.5f}, outs, err) || outs.empty()) continue;
                if (i < 36) continue;   // let the stillness map build up (UI needs ~1 s)
                auto gl = lumaOf(*outs[0]);
                auto& t = truth[i];
                for (int r = r0; r < r1; r++) for (int c = c0; c < c1; c++) { uiErr += std::abs(int(gl[size_t(r) * W + c]) - int(t[size_t(r) * W + c])); uiN++; }
                for (int by = 0; by + 32 <= H; by += 32) for (int bx = 0; bx + 32 <= W; bx += 32) {
                    if (isUI(by + 16, bx + 16)) continue;
                    double e = 0;
                    for (int r = by; r < by + 32; r += 2) for (int c = bx; c < bx + 32; c += 2) e += std::abs(int(gl[size_t(r) * W + c]) - int(t[size_t(r) * W + c]));
                    e /= 256; worldErr += e; worldN++; worstBlock = std::max(worstBlock, e);
                }
                if (i == 45) if (const char* dir = getenv("FG_HUD_DUMP")) Gpu::get().savePNG(*outs[0], strf("%s\\%s_%s.png", dir, engineId(k), lock ? "stabiliser" : "off"));
            }
            logf("   %-14s UI panel %6.2f   game world %5.2f   worst world block %6.2f", lock ? "UI stabiliser" : "off",
                 uiErr / std::max(uiN, 1.0), worldErr / std::max(worldN, 1.0), worstBlock);
        }
    }
    g_hudLock = true;
    return 0;
}

/// Overall quality vs ground truth for every engine and scene: mean error, worst 32×32 block,
/// PSNR (luma), at 2× (phase ½) and 4× (phases ¼, ½, ¾).
int runQualityCheck() {
    printSupport();
    const int W = g_opt.width > 0 ? g_opt.width : 1920, H = g_opt.height > 0 ? g_opt.height : 1080;
    std::vector<std::string> scenes = {"action", "fastpan", "still", "hud"};
    if (const char* s = getenv("FG_SCENE")) scenes = {s};
    g_hudLock = g_opt.hudLock;
    for (auto& scene : scenes) {
        SyntheticSource syn(30, W, H);
        syn.scene = scene;
        syn.noise = g_opt.noise;
        RepeatDetector det;
        std::vector<FramePtr> frames;
        const int N = 24, start = 30;
        for (int i = 0; i < N; i++) {
            auto f = syntheticFrame(syn, start + i, 30);
            det.isRepeat(*f); det.accept(); f->motion = det.lastDiff;
            frames.push_back(f);
        }
        logf("Scene \"%s\" %dx%d (error vs exact in-between frames, /255; lower is better)", scene.c_str(), W, H);
        std::vector<EngineKind> engines(std::begin(kEngines), std::end(kEngines));
        if (EngineSupport::isSupported(EngineKind::Vfg)) engines.push_back(EngineKind::Vfg);
        if (const char* e = getenv("FG_ENGINES")) {
            engines.clear(); std::string s = e; size_t p = 0;
            while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size(); if (auto k = engineFromId(s.substr(p, q - p))) engines.push_back(*k); p = q + 1; }
        }
        for (EngineKind k : engines) {
            for (int mult : {2, 4}) {
                std::vector<float> phases; for (int i = 1; i < mult; i++) phases.push_back(float(i) / mult);
                HudStabilizer::get().reset();
                std::string err;
                auto interp = makeInterpolator(k, err);
                double sum = 0, sq = 0, n = 0, worst = 0; int wx = 0, wy = 0, wi = 0; size_t wp = 0;
                for (size_t i = 0; i + 1 < frames.size(); i++) {
                    std::vector<ImagePtr> outs;
                    if (!interp->interpolate(*frames[i], *frames[i + 1], phases, outs, err)) { logf("  failed: %s", err.c_str()); break; }
                    if (i < 4) continue;
                    for (size_t p = 0; p < outs.size(); p++) {
                        auto gl = lumaOf(*outs[p]);
                        auto tl = lumaOf(*syn.renderRGBA(int((start + i) * mult + p + 1), 30.0 * mult));
                        for (int by = 0; by + 32 <= H; by += 32) for (int bx = 0; bx + 32 <= W; bx += 32) {
                            // The frame counter shows numbers real frames never contain: not predictable, not scored.
                            if (by < 120 * H / 1080 && bx < 560 * H / 1080) continue;
                            double e = 0;
                            for (int r = by; r < by + 32; r++) for (int c = bx; c < bx + 32; c++) {
                                double d = double(gl[size_t(r) * W + c]) - tl[size_t(r) * W + c];
                                e += std::abs(d); sq += d * d;
                            }
                            sum += e; n += 1024;
                            if (e / 1024 > worst) { worst = e / 1024; wx = bx; wy = by; wi = int(start + i); wp = p; }
                        }
                        if (const char* dir = getenv("FG_QUALITY_DUMP"); dir && i == 10 && p == outs.size() / 2)
                            Gpu::get().savePNG(*outs[p], strf("%s\\%s_%s_x%d.png", dir, scene.c_str(), engineId(k), mult));
                    }
                }
                double mse = sq / std::max(n, 1.0);
                logf("  %-40s x%d  mean %5.2f   worst block %6.2f (at %d,%d frame %d phase %zu)   PSNR %5.2f dB", engineLabel(k), mult, sum / std::max(n, 1.0), worst,
                     wx, wy, wi, wp, mse > 0 ? 10 * std::log10(255.0 * 255.0 / mse) : 99.0);
            }
        }
    }
    return 0;
}

// ---- --check-real <dir>: runs the engines on real captured frame pairs (liveN_a_before.png /
// liveN_d_after.png from --dump-live) at t = ½. There is no true in-between frame, so it reports
// blockiness (luma steps on the 8/16 px block grid relative to elsewhere, 1.00 = no visible grid,
// relative to the input frame) and, when NVIDIA VFG is available, the PSNR against VFG's output.
// FG_REAL_OUT=<dir> saves every result; FG_ENGINES=fast,quality,vfg picks engines.
static double blockiness(const std::vector<uint8_t>& l, int W, int H) {
    double onG = 0, offG = 0; double nOn = 0, nOff = 0;
    for (int y = 1; y < H; y++) for (int x = 1; x < W; x++) {
        double gx = std::abs(int(l[size_t(y) * W + x]) - int(l[size_t(y) * W + x - 1]));
        double gy = std::abs(int(l[size_t(y) * W + x]) - int(l[size_t(y - 1) * W + x]));
        if (x % 8 == 0) { onG += gx; nOn++; } else { offG += gx; nOff++; }
        if (y % 8 == 0) { onG += gy; nOn++; } else { offG += gy; nOff++; }
    }
    return (onG / std::max(nOn, 1.0)) / std::max(offG / std::max(nOff, 1.0), 1e-6);
}

int runRealCheck(const std::string& dir) {
    printSupport();
    auto& gpu = Gpu::get();
    g_hudLock = g_opt.hudLock;
    std::vector<EngineKind> engines = {EngineKind::Fast, EngineKind::Quality};
    if (EngineSupport::isSupported(EngineKind::Vfg)) engines.push_back(EngineKind::Vfg);
    if (const char* e = getenv("FG_ENGINES")) {
        engines.clear();
        std::string s = e; size_t p = 0;
        while (p <= s.size()) { size_t q = s.find(',', p); if (q == std::string::npos) q = s.size();
            if (auto k = engineFromId(s.substr(p, q - p))) engines.push_back(*k); p = q + 1; }
    }
    const char* outDir = getenv("FG_REAL_OUT");
    std::map<EngineKind, std::unique_ptr<Interpolator>> interps;
    for (auto k : engines) { std::string err; interps[k] = makeInterpolator(k, err); if (!interps[k]) logf("  %s: %s", engineLabel(k), err.c_str()); }
    std::map<EngineKind, double> sumBlk, sumPsnr; std::map<EngineKind, int> nPsnr; double sumIn = 0; int sets = 0;
    for (int n = 1; n < 200; n++) {
        auto a = gpu.loadPNG(strf("%s/live%d_a_before.png", dir.c_str(), n));
        auto b = gpu.loadPNG(strf("%s/live%d_d_after.png", dir.c_str(), n));
        if (!a || !b) { if (n > 60) break; continue; }
        auto fa = std::make_shared<VideoFrame>(), fb = std::make_shared<VideoFrame>();
        fa->image = a; fb->image = b; fa->time = 0; fb->time = 1.0 / 30;
        auto la = lumaOf(*a), lb = lumaOf(*b);
        double diff = 0; for (size_t i = 0; i < la.size(); i += 7) diff += std::abs(int(la[i]) - int(lb[i])); diff /= (la.size() / 7.0);
        fb->motion = diff / 255.0;
        double bin = blockiness(la, a->w, a->h); sumIn += bin; sets++;
        if (!(bin > 0.2)) bin = 1;   // flat / black frame: no texture to compare against
        std::string line = strf("live%-3d diff %5.1f  input grid %.3f |", n, diff, bin);
        std::map<EngineKind, std::vector<uint8_t>> res;
        for (auto k : engines) {
            auto& in = interps[k]; if (!in) continue;
            std::vector<ImagePtr> outs; std::string err;
            HudStabilizer::get().reset();
            // Two calls: engines with history (previous pair) get the same warm start as live.
            in->interpolate(*fa, *fb, {0.5f}, outs, err); outs.clear();
            if (!in->interpolate(*fa, *fb, {0.5f}, outs, err) || outs.empty()) { line += strf(" %s FAILED (%s)", engineId(k), err.c_str()); continue; }
            gpu.waitIdle();
            auto l = lumaOf(*outs[0]);
            double blk = blockiness(l, a->w, a->h) / bin;
            sumBlk[k] += blk;
            line += strf(" %s grid %.3f", engineId(k), blk);
            res[k] = l;
            if (outDir) gpu.savePNG(*outs[0], strf("%s/live%d_%s.png", outDir, n, engineId(k)));
        }
        if (res.count(EngineKind::Vfg)) {
            auto& ref = res[EngineKind::Vfg];
            for (auto& [k, l] : res) {
                if (k == EngineKind::Vfg) continue;
                double sq = 0; for (size_t i = 0; i < l.size(); i++) { double d = double(l[i]) - ref[i]; sq += d * d; }
                double psnr = 10 * std::log10(255.0 * 255.0 / std::max(sq / l.size(), 1e-6));
                sumPsnr[k] += psnr; nPsnr[k]++;
                line += strf(" %s~vfg %.1fdB", engineId(k), psnr);
            }
        }
        logf("%s", line.c_str());
    }
    if (!sets) { logf("No liveN_a_before.png / liveN_d_after.png pairs in %s", dir.c_str()); return 1; }
    logf("Average over %d pairs (grid: 1.00 = like the input; higher = visible block edges):", sets);
    for (auto k : engines) {
        std::string s = strf("  %-40s grid %.3f", engineLabel(k), sumBlk[k] / sets);
        if (nPsnr[k]) s += strf("   PSNR vs VFG %.2f dB", sumPsnr[k] / nPsnr[k]);
        logf("%s", s.c_str());
    }
    return 0;
}

// ---- --stress <seconds>: GPU health check. Heavy deterministic integer compute (every result is
// checked against the first run, a sample against the CPU) plus a video-memory pattern test.
static const char* kStress = R"(
RWStructuredBuffer<uint> O : register(u0);
cbuffer C : register(b0) { uint iters; uint seed; uint2 pad; };
[numthreads(256, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint x = id.x * 2654435761u + seed, y = x ^ 0x9E3779B9u;
    for (uint i = 0; i < iters; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; y = y * 1664525u + 1013904223u + x; }
    O[id.x] = x ^ y;
}
)";
static const char* kPattern = R"(
RWStructuredBuffer<uint> O : register(u0);
cbuffer C : register(b0) { uint mode; uint seed; uint2 pad; };   // mode 0 write, 1 verify (count errors into O[0] of a second buffer)
RWStructuredBuffer<uint> E : register(u1);
[numthreads(256, 1, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint want = (id.x * 2246822519u) ^ seed;
    if (mode == 0) O[id.x] = want;
    else if (O[id.x] != want) InterlockedAdd(E[0], 1);
}
)";

static uint32_t stressRef(uint32_t idx, uint32_t seed, uint32_t iters) {
    uint32_t x = idx * 2654435761u + seed, y = x ^ 0x9E3779B9u;
    for (uint32_t i = 0; i < iters; i++) { x ^= x << 13; x ^= x >> 17; x ^= x << 5; y = y * 1664525u + 1013904223u + x; }
    return x ^ y;
}

static std::string smi() {
    std::string out;
    FILE* p = _popen("nvidia-smi --query-gpu=temperature.gpu,clocks.gr,power.draw,utilization.gpu --format=csv,noheader 2>nul", "r");
    if (!p) return "";
    char buf[256]; while (fgets(buf, sizeof buf, p)) out += buf;
    _pclose(p);
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

int runStress(double seconds) {
    printSupport();
    auto& g = Gpu::get();
    auto makeBuf = [&](UINT count, ComPtr<ID3D11Buffer>& b, ComPtr<ID3D11UnorderedAccessView>& u, bool readback, ComPtr<ID3D11Buffer>* stage) {
        D3D11_BUFFER_DESC d{}; d.ByteWidth = count * 4; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
        d.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED; d.StructureByteStride = 4;
        if (FAILED(g.dev->CreateBuffer(&d, nullptr, &b))) return false;
        g.dev->CreateUnorderedAccessView(b.Get(), nullptr, &u);
        if (readback) { d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0; g.dev->CreateBuffer(&d, nullptr, stage->GetAddressOf()); }
        return true;
    };
    const UINT N = 1u << 22, iters = 2048;   // 4M threads × 2048 iterations
    ComPtr<ID3D11Buffer> buf, stage; ComPtr<ID3D11UnorderedAccessView> uav;
    makeBuf(N, buf, uav, true, &stage);
    std::vector<uint32_t> first(N), cur(N);
    auto readBack = [&](std::vector<uint32_t>& dst) {
        { auto l = g.lock(); g.ctx->CopyResource(stage.Get(), buf.Get()); }
        g.waitIdle();
        auto l = g.lock(); D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(g.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) { memcpy(dst.data(), m.pData, size_t(N) * 4); g.ctx->Unmap(stage.Get(), 0); }
    };
    logf("Compute stress: %u threads x %u iterations per pass, for %.0f s (results verified every pass)", N, iters, seconds);
    long long errors = 0, passes = 0; double tMin = 1e9, tMax = 0, tSum = 0, t0 = hostNow(), lastReport = t0;
    while (hostNow() - t0 < seconds) {
        double ts = hostNow();
        struct { UINT iters, seed, pad[2]; } c{iters, 12345u, {}};
        { auto l = g.lock(); g.dispatch(g.cs(kStress), {}, {uav.Get()}, &c, sizeof c, N, 1, 256, 1); }
        readBack(passes == 0 ? first : cur);
        double dt = (hostNow() - ts) * 1000;
        if (passes == 0) {
            for (UINT i = 0; i < N; i += N / 512) if (first[i] != stressRef(i, 12345u, iters)) errors++;   // CPU reference for 512 samples
        } else {
            for (UINT i = 0; i < N; i++) if (cur[i] != first[i]) errors++;
            tMin = std::min(tMin, dt); tMax = std::max(tMax, dt); tSum += dt;
        }
        passes++;
        if (hostNow() - lastReport > 5) { lastReport = hostNow(); logf("  %3.0f s: %lld passes, avg %.1f ms/pass, errors %lld | GPU %s", hostNow() - t0, passes, tSum / std::max(1LL, passes - 1), errors, smi().c_str()); }
    }
    logf("Compute: %lld passes, %.1f / %.1f / %.1f ms per pass (min/avg/max), %lld wrong results", passes, tMin, tSum / std::max(1LL, passes - 1), tMax, errors);

    // Video memory: write and verify a pattern in 4 × 1 GB buffers.
    long long memErrors = 0; int gb = 0;
    ComPtr<ID3D11Buffer> eb, es; ComPtr<ID3D11UnorderedAccessView> eu;
    makeBuf(1, eb, eu, true, &es);
    for (int k = 0; k < 4; k++) {
        ComPtr<ID3D11Buffer> big; ComPtr<ID3D11UnorderedAccessView> bu;
        const UINT M = 1u << 28;   // 1 GB
        if (!makeBuf(M, big, bu, false, nullptr)) break;
        gb++;
        for (UINT seed : {0xA5A5A5A5u, 0x5A5A5A5Au}) {
            UINT zero[4] = {};
            auto l = g.lock();
            g.ctx->ClearUnorderedAccessViewUint(eu.Get(), zero);
            struct { UINT mode, seed, pad[2]; } w{0, seed, {}}, v{1, seed, {}};
            for (UINT off = 0; off < M; off += 1u << 24) {}   // (single dispatch below covers the buffer)
            g.dispatch(g.cs(kPattern), {}, {bu.Get(), eu.Get()}, &w, sizeof w, 65535 * 256, 1, 256, 1);
            g.dispatch(g.cs(kPattern), {}, {bu.Get(), eu.Get()}, &v, sizeof v, 65535 * 256, 1, 256, 1);
            g.ctx->CopyResource(es.Get(), eb.Get());
            l.unlock();
            g.waitIdle();
            auto l2 = g.lock(); D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(g.ctx->Map(es.Get(), 0, D3D11_MAP_READ, 0, &m))) { memErrors += *(uint32_t*)m.pData; g.ctx->Unmap(es.Get(), 0); }
        }
    }
    logf("Video memory: %d GB tested (pattern write + verify, 2 patterns), %lld errors", gb, memErrors);
    bool ok = errors == 0 && memErrors == 0 && tMax < tMin * 3;
    logf(ok ? "RESULT: PASS — the GPU computes correctly and at a steady speed." : "RESULT: CHECK — see the numbers above.");
    return ok ? 0 : 1;
}
