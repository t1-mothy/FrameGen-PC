#include "Pipeline.h"
#include "Hud.h"
#include "Capture.h"
#include "FlowEngine.h"
#include <avrt.h>
#include <algorithm>
#include <cmath>
#include <numeric>

// ---------------------------------------------------------------- SerialQueue

SerialQueue::SerialQueue(const wchar_t* name, int priority) {
    t_ = std::thread([this, priority] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        SetThreadPriority(GetCurrentThread(), priority);
        for (;;) {
            std::function<void()> fn;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [&] { return stop_ || !q_.empty(); });
                if (q_.empty()) break;
                fn = std::move(q_.front()); q_.pop_front();
            }
            fn();
        }
        CoUninitialize();
    });
    SetThreadDescription(t_.native_handle(), name);
}
SerialQueue::~SerialQueue() {
    { std::lock_guard<std::mutex> l(m_); stop_ = true; }
    cv_.notify_all();
    if (t_.joinable()) t_.join();
}
void SerialQueue::async(std::function<void()> fn) {
    { std::lock_guard<std::mutex> l(m_); q_.push_back(std::move(fn)); }
    cv_.notify_one();
}
void SerialQueue::sync(std::function<void()> fn) {
    std::mutex dm; std::condition_variable dcv; bool done = false;
    async([&] { fn(); { std::lock_guard<std::mutex> l(dm); done = true; } dcv.notify_one(); });
    std::unique_lock<std::mutex> l(dm);
    dcv.wait(l, [&] { return done; });
}

// ---------------------------------------------------------------- FrameQueue

void FrameQueue::push(FramePtr f) {
    std::lock_guard<std::mutex> l(m_);
    auto it = std::find_if(frames_.begin(), frames_.end(), [&](const FramePtr& x) { return x->time > f->time; });
    frames_.insert(it, std::move(f));
    while (frames_.size() > 128) frames_.pop_front();   // 240 fps × up to ~0.5 s
}
std::pair<FramePtr, FramePtr> FrameQueue::take(double contentTime) {
    std::lock_guard<std::mutex> l(m_);
    while (!frames_.empty() && frames_.front()->time <= contentTime) {
        current_ = frames_.front(); frames_.pop_front();
        if (!current_->isGenerated) latestReal_ = current_;
    }
    return {current_, latestReal_};
}
FramePtr FrameQueue::popAfter(double t) {
    std::lock_guard<std::mutex> l(m_);
    while (!frames_.empty() && frames_.front()->time <= t) frames_.pop_front();
    if (frames_.empty()) return nullptr;
    FramePtr f = frames_.front(); frames_.pop_front();
    current_ = f; if (!f->isGenerated) latestReal_ = f;
    return f;
}

void FrameQueue::clear() { std::lock_guard<std::mutex> l(m_); frames_.clear(); }

// ---------------------------------------------------------------- RepeatDetector

std::vector<double> RepeatDetector::recentDiffs() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<double> v(recent_.begin(), recent_.end()); std::sort(v.begin(), v.end()); return v;
}
std::pair<std::vector<double>, std::vector<double>> RepeatDetector::tileStats() {
    std::lock_guard<std::mutex> l(m_);
    std::vector<double> a(recentRepeatTiles_.begin(), recentRepeatTiles_.end()), b(recentUniqueTiles_.begin(), recentUniqueTiles_.end());
    std::sort(a.begin(), a.end()); std::sort(b.begin(), b.end());
    return {a, b};
}

bool RepeatDetector::isRepeat(const VideoFrame& f) {
    if (!enabled) return false;
    int cols = 0, rows = 0;
    std::vector<uint8_t> sample;
    if (f.sparseLuma && f.lumaCols > 0) { sample = *f.sparseLuma; cols = f.lumaCols; rows = int(sample.size() / cols); }   // taken on the CPU by the capture source
    else if (!f.image || !Gpu::get().sparseLuma(*f.image, sample, cols, rows)) return false;
    if (cols <= 0 || rows <= 0) return false;
    pending_ = sample;
    if (last_.size() != sample.size()) { lastDiff = 255; lastMaxTile = 255; return false; }
    const int tx = tilesX, ty = tilesY;
    int tileSum[tx * ty] = {}, tileCount[tx * ty] = {};
    long long total = 0;
    for (int r = 0; r < rows; r++) {
        int tr = std::min(r * ty / rows, ty - 1) * tx;
        int base = r * cols;
        for (int c = 0; c < cols; c++) {
            int d = std::abs(int(sample[base + c]) - int(last_[base + c]));
            int t = tr + std::min(c * tx / cols, tx - 1);
            tileSum[t] += d; tileCount[t]++; total += d;
        }
    }
    lastDiff = double(total) / double(sample.size());
    double maxTile = 0;
    for (int t = 0; t < tx * ty; t++) if (tileCount[t] > 0) maxTile = std::max(maxTile, double(tileSum[t]) / tileCount[t]);
    lastMaxTile = maxTile;
    bool isRep = lastDiff < globalThreshold && maxTile < tileThreshold;
    std::lock_guard<std::mutex> l(m_);
    recent_.push_back(lastDiff); if (recent_.size() > 120) recent_.pop_front();
    auto& dst = isRep ? recentRepeatTiles_ : recentUniqueTiles_;
    dst.push_back(maxTile); if (dst.size() > 120) dst.pop_front();
    return isRep;
}

// ---------------------------------------------------------------- CadenceTracker

void CadenceTracker::reset() {
    std::lock_guard<std::mutex> l(m_);
    intervals_.clear(); rawLog_.clear(); smoothLog_.clear();
    lastRaw_.reset(); smoothed_.reset(); interval_.reset(); lastSmoothedOut_.reset();
}
double CadenceTracker::interval() { std::lock_guard<std::mutex> l(m_); return interval_.value_or(0); }
double CadenceTracker::holdLimit() { return std::min(0.11, std::max(0.045, 1.6 * interval())); }
double CadenceTracker::maxInterpolationGap() { return std::min(0.11, std::max(0.06, 2.6 * interval())); }

double CadenceTracker::update(double rawTime, bool forced) {
    std::lock_guard<std::mutex> l(m_);
    if (lastRaw_) {
        double d = rawTime - *lastRaw_;
        if (!forced && d > 0.004 && d < 0.2) {
            intervals_.push_back(d); if (intervals_.size() > 40) intervals_.pop_front();
            rawLog_.push_back(d); if (rawLog_.size() > 90) rawLog_.pop_front();
        }
    }
    lastRaw_ = rawTime;
    // Learned interval: mean of recent intervals, ignoring outliers (drops, hitches).
    if (intervals_.size() >= 6) {
        std::vector<double> s(intervals_.begin(), intervals_.end());
        std::sort(s.begin(), s.end());
        double med = s[s.size() / 2], sum = 0; int n = 0;
        for (double v : intervals_) if (v > 0.6 * med && v < 1.5 * med) { sum += v; n++; }
        if (n >= 4) interval_ = sum / n;
    }
    double t = rawTime;
    if (enabled && interval_ && smoothed_) {
        double p = *interval_;
        double k = std::round((rawTime - *smoothed_) / p);
        if (k >= 1 && k <= 4) {
            double predicted = *smoothed_ + k * p, err = rawTime - predicted;
            if (std::abs(err) < 0.45 * p) t = predicted + 0.15 * err;
        }
    }
    if (lastSmoothedOut_ && t <= *lastSmoothedOut_) t = rawTime;   // never go backwards
    if (lastSmoothedOut_) {
        double d = t - *lastSmoothedOut_;
        if (d > 0.004 && d < 0.2) { smoothLog_.push_back(d); if (smoothLog_.size() > 90) smoothLog_.pop_front(); }
    }
    smoothed_ = t;
    lastSmoothedOut_ = t;
    return t;
}

std::pair<double, double> CadenceTracker::jitter() {
    std::lock_guard<std::mutex> l(m_);
    auto sd = [](const std::deque<double>& a) {
        if (a.size() <= 4) return 0.0;
        std::vector<double> s(a.begin(), a.end()); std::sort(s.begin(), s.end());
        double med = s[s.size() / 2];
        std::vector<double> g; for (double v : a) if (v > 0.6 * med && v < 1.5 * med) g.push_back(v);
        if (g.size() <= 3) return 0.0;
        double m = std::accumulate(g.begin(), g.end(), 0.0) / g.size(), var = 0;
        for (double v : g) var += (v - m) * (v - m);
        return std::sqrt(var / g.size()) * 1000;
    };
    return {sd(rawLog_), sd(smoothLog_)};
}

// ---------------------------------------------------------------- FramePipeline

FramePipeline::FramePipeline(EngineKind k, int m, Stats& stats) : stats_(stats) {
    queue_ = std::make_unique<SerialQueue>(L"FrameGen.pipeline", THREAD_PRIORITY_HIGHEST);
    prep_ = std::make_unique<SerialQueue>(L"FrameGen.prep", THREAD_PRIORITY_ABOVE_NORMAL);
    multiplier = m; requestedKind = k; requestedMultiplier = m;
    configure(k, m);
}

FramePipeline::~FramePipeline() {
    prep_.reset();
    queue_.reset();
}

/// Builds and warms up the new engine on a background queue while real frames keep flowing,
/// then swaps it in, so changing modes never freezes the picture (Lessons 7–8).
void FramePipeline::configure(EngineKind k, int m) {
    int gen;
    { std::lock_guard<std::mutex> l(lock_); gen = ++configGeneration_; preparing_++; }
    requestedKind = k; requestedMultiplier = m;
    // Running two engine sessions at once can stall both, so release the current one now and
    // show real frames until the new one is ready (a brief dip instead of a freeze).
    queue_->async([this] { interp_.reset(); });
    prep_->async([this, k, m, gen] {
        { std::lock_guard<std::mutex> l(lock_); if (gen != configGeneration_) { preparing_--; return; } }   // superseded
        std::string err;
        std::shared_ptr<Interpolator> made(makeInterpolator(k, err).release());
        if (made && k != EngineKind::Blend) warm(*made, m);
        queue_->async([this, k, m, gen, made, err]() mutable {
            { std::lock_guard<std::mutex> l(lock_); preparing_--; if (gen != configGeneration_) return; }   // superseded by a newer request
            interp_.reset();
            interp_ = made;
            multiplier = m;
            if (!err.empty()) {
                kind = EngineKind::Off;
                stats_.setError(err);
                stats_.setText(stats_.interpolatorName, "Off");
            } else {
                kind = k;
                stats_.setText(stats_.interpolatorName, k == EngineKind::Off ? std::string("Off") : strf("%s x%d", engineLabel(k), m));
            }
            { std::lock_guard<std::mutex> l(lock_); lateness_.clear(); }
            warmup_ = false;
        });
    });
}

/// Runs one throw-away interpolation so the engine is ready before live frames arrive.
void FramePipeline::warm(Interpolator& interp, int m) {
    auto& gpu = Gpu::get();
    auto a = gpu.makeImage(warmW_, warmH_, DXGI_FORMAT_R8G8B8A8_UNORM), b = gpu.makeImage(warmW_, warmH_, DXGI_FORMAT_R8G8B8A8_UNORM);
    if (!a || !b) return;
    VideoFrame fa, fb;
    fa.image = a; fa.time = 0; fa.forWarmup = true;
    fb.image = b; fb.time = 1.0 / 30; fb.forWarmup = true;
    int mm = std::max(m, 2);
    std::vector<float> phases; for (int i = 1; i < mm; i++) phases.push_back(float(i) / mm);
    std::vector<ImagePtr> out; std::string err;
    interp.interpolate(fa, fb, phases, out, err);
    gpu.waitIdle();
}

double FramePipeline::requiredDelay() {
    std::lock_guard<std::mutex> l(lock_);
    // After a still scene (no new frames), keep the last known value instead of resetting.
    if (!lateness_.empty()) {
        double m = 0; for (auto& p : lateness_) m = std::max(m, p.second);
        lastRequired_ = m;
    }
    return lastRequired_;
}

void FramePipeline::reset() {
    lastW = 0; lastH = 0;
    cadence.reset();
    HudStabilizer::get().reset();
    lastAcceptedTime_ = 0;
    dedupe.reset();
    queue_->async([this] { prev_.reset(); });
    output.clear();
}

bool FramePipeline::admit(VideoFrame& f) {
    stats_.recordInput(f.time);
    // Consoles send a fixed 60 Hz signal; a 30 fps game repeats every frame. Interpolating
    // between identical frames does nothing, so drop repeats and keep the first copy's time.
    bool forced = false;
    if (dedupe.isRepeat(f)) {
        // Safety net: never hold a frame much longer than the game's own frame interval
        // (45–110 ms), so a missed small motion can't freeze the picture (Lesson 3).
        if (f.time - lastAcceptedTime_ < cadence.holdLimit()) { stats_.recordRepeat(); return false; }
        stats_.recordForced();
        forced = true;
    }
    dedupe.accept();
    lastAcceptedTime_ = f.time;
    // Snap the timestamp to the game's steady rhythm (the card's clock quantises arrival times).
    f.time = cadence.update(f.time, forced);
    f.motion = dedupe.enabled ? dedupe.lastDiff : 10;
    f.isRepeatPassThrough = forced;
    f.admitted = true;
    stats_.recordSource(f.time);
    return true;
}

void FramePipeline::push(FramePtr frame) {
    if (!frame->admitted && !admit(*frame)) return;
    auto f = std::make_shared<VideoFrame>(*frame);
    {
        std::lock_guard<std::mutex> l(lock_);
        // More than one frame behind: drop to stay real-time.
        if (pending_ >= 2) { stats_.recordSourceDrop(); return; }
        pending_++;
    }
    queue_->async([this, f] {
        process(f);
        std::lock_guard<std::mutex> l(lock_); pending_--;
    });
}

void FramePipeline::dumpSet(const VideoFrame& a, ImagePtr raw, ImagePtr mid, const VideoFrame& b, double motion) {
    dumped_++; lastDump_ = hostNow();
    int n = dumped_;
    std::string dir = liveDumpDir;
    auto& gpu = Gpu::get();
    // Copy on the GPU now (pooled images get reused), read back and encode PNGs on a background
    // thread so the live pipeline never stalls.
    auto copy = [&](const ImagePtr& src) -> ImagePtr {
        if (!src) return nullptr;
        auto c = gpu.makeImage(src->w, src->h, src->fmt);
        if (c) { auto l = gpu.lock(); gpu.ctx->CopyResource(c->tex.Get(), src->tex.Get()); }
        return c;
    };
    ImagePtr ia = copy(a.image), ir = copy(raw), im = copy(mid), ib = copy(b.image);
    auto snap = HudStabilizer::get().takeSnapshot();
    std::thread([=] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        CreateDirectoryW(widen(dir).c_str(), nullptr);
        auto& g = Gpu::get();
        if (ia) g.savePNG(*ia, strf("%s/live%d_a_before.png", dir.c_str(), n));
        if (ir) g.savePNG(*ir, strf("%s/live%d_b_raw.png", dir.c_str(), n));
        if (im) g.savePNG(*im, strf("%s/live%d_c_stabilised.png", dir.c_str(), n));
        if (ib) g.savePNG(*ib, strf("%s/live%d_d_after.png", dir.c_str(), n));
        if (!snap.data.empty()) {   // UI detection map: white = recognised as on-screen UI (one pixel per 8×8 block, scaled ×8)
            std::vector<uint8_t> big(size_t(snap.w) * 8 * snap.h * 8);
            for (int y = 0; y < snap.h * 8; y++) for (int x = 0; x < snap.w * 8; x++) big[size_t(y) * snap.w * 8 + x] = snap.data[size_t(y / 8) * snap.w + x / 8] ? 255 : 0;
            Gpu::savePNG(big.data(), snap.w * 8, snap.h * 8, strf("%s/live%d_e_uimap.png", dir.c_str(), n), true);
        }
        logf("[dump] set %d (motion %.1f) -> %s", n, motion, dir.c_str());
        CoUninitialize();
    }).detach();
}

void FramePipeline::process(FramePtr next) {
    auto& gpu = Gpu::get();
    // Convert the card's NV12 to the RGBA working format once per unique frame.
    if (next->image && next->image->isNV12()) {
        auto rgba = rgbaPool_.acquire(next->image->w, next->image->h, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!rgba) return;
        auto l = gpu.lock();
        CaptureSource::uploader_.waitOnMain(next->uploadFence);   // GPU-side wait for the capture upload
        gpu.nv12ToRgba(*next->image, *rgba, yuv);
        CaptureSource::uploader_.markRead(*next->image);
        next->image = rgba;
    }
    int w = next->width(), h = next->height();
    stats_.setText(stats_.sourceSize, strf("%dx%d", w, h));
    // A new frame size means the engine re-creates its resources on the next call: treat that
    // call as warm-up, not as a measure of this PC's speed (Lesson 7).
    if (warmW_ != w || warmH_ != h || (prev_ && (prev_->width() != w || prev_->height() != h))) warmup_ = true;
    warmW_ = w; warmH_ = h;
    lastW = w; lastH = h;
    std::vector<FramePtr> produced;
    bool wasWarmup = false;
    bool busy; { std::lock_guard<std::mutex> l(lock_); busy = preparing_ > 0; }
    int m = multiplier;
    if (!busy && prev_ && interp_ && m > 1 && prev_->width() == w && prev_->height() == h) {
        double dt = next->time - prev_->time;
        // Across longer gaps (menus pausing, scene cuts) show the real frame without interpolating.
        // A repeat passed on by the safety net is the same picture: nothing to generate.
        if (dt > 0 && dt < cadence.maxInterpolationGap() && !next->isRepeatPassThrough) {
            std::vector<float> phases; for (int i = 1; i < m; i++) phases.push_back(float(i) / m);
            double t0 = hostNow();
            bool wantDump = !liveDumpDir.empty() && dumped_ < 8 && next->motion > 3 && hostNow() - lastDump_ > 3 && !warmup_;
            interp_->wantRaw = wantDump; interp_->rawCopy.reset();
            if (wantDump) HudStabilizer::get().requestSnapshot();
            std::vector<ImagePtr> outs; std::string err;
            GpuProfiler::get().begin();
            bool ok = interp_->interpolate(*prev_, *next, phases, outs, err);
            double tSubmit = hostNow();
            interp_->wantRaw = false;
            if (ok) {
                gpu.waitIdle();
                GpuProfiler::get().end();
                { static int n; if (++n % 60 == 0) GpuProfiler::get().report(); }
                if (getenv("FG_TRACE")) logf("[trace] pair at %.3f: submit %.2f ms, GPU %.2f ms, motion %.1f", hostNow(), (tSubmit - t0) * 1000, (hostNow() - tSubmit) * 1000, next->motion);
                if (g_opt.log) { static double acc[3]; static double at; acc[0] += tSubmit - t0; acc[1] += hostNow() - tSubmit; acc[2]++;
                    if (hostNow() - at > 1) { logf("[pipeline] per pair: submit %.2f ms, GPU wait %.2f ms", acc[0] / acc[2] * 1000, acc[1] / acc[2] * 1000); acc[0] = acc[1] = acc[2] = 0; at = hostNow(); } }
                if (g_simulatedSlowMs > 0) Sleep(DWORD(g_simulatedSlowMs * phases.size()));
                wasWarmup = warmup_;
                warmup_ = false;
                if (!wasWarmup) stats_.recordInterp((hostNow() - t0) * 1000);
                for (size_t i = 0; i < outs.size() && i < phases.size(); i++) {
                    auto g = std::make_shared<VideoFrame>();
                    g->image = outs[i]; g->isGenerated = true;
                    g->time = prev_->time + double(phases[i]) * dt;
                    produced.push_back(g);
                }
                if (wantDump && !outs.empty()) dumpSet(*prev_, interp_->rawCopy, outs[0], *next, next->motion);
            } else {
                stats_.setError(err);
            }
        }
    }
    // DLSS-G mode: DLSS Frame Generation creates the in-between frames itself, from the real frames
    // plus motion vectors — which a capture card can't provide, so they're estimated here.
    if (wantMotion && prev_ && prev_->width() == w && prev_->height() == h) {
        double dt = next->time - prev_->time;
        if (dt > 0 && dt < cadence.maxInterpolationGap() && !next->isRepeatPassThrough) {
            if (!motion_) motion_ = std::make_unique<FlowEngine>(FlowEngine::Quality::Fast);
            double t0 = hostNow();
            next->mvec = motion_->motionField(*prev_, *next, next->mvecScale);
            gpu.waitIdle();
            stats_.recordInterp((hostNow() - t0) * 1000);
        }
    }
    produced.push_back(next);
    double ready = hostNow();
    {
        std::lock_guard<std::mutex> l(lock_);
        for (auto& f : produced) {
            f->readyTime = ready;
            if (!wasWarmup) lateness_.push_back({ready, ready - f->time});
        }
        // Keep the last 2 seconds of measurements.
        while (!lateness_.empty() && ready - lateness_.front().first > 2.0) lateness_.pop_front();
    }
    for (auto& f : produced) output.push(f);
    prev_ = next;
}
