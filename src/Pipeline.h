// Receives source frames, drops console repeats, smooths the cadence, generates in-between frames
// and hands everything to the presenter. A port of the Mac app's Pipeline.swift (same logic,
// same thresholds — they were measured on real hardware).
#pragma once
#include "Core.h"
#include "Gpu.h"
#include "Interpolators.h"
#include <condition_variable>
#include <thread>

/// A serial work queue on its own thread (like a serial DispatchQueue).
class SerialQueue {
public:
    SerialQueue(const wchar_t* name, int priority);
    ~SerialQueue();
    void async(std::function<void()> fn);
    void sync(std::function<void()> fn);   // runs fn on the queue and waits
private:
    std::thread t_;
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool stop_ = false;
};

/// Thread-safe, time-ordered queue of frames waiting to be shown.
class FrameQueue {
public:
    void push(FramePtr f);
    /// Newest frame whose content time is <= contentTime (older frames are discarded), and the
    /// newest real (non-generated) frame at that time, for A/B compare.
    std::pair<FramePtr, FramePtr> take(double contentTime);
    /// The earliest queued frame newer than `t` (removed from the queue), discarding older ones.
    FramePtr popAfter(double t);
    /// The frame to show at contentTime without removing anything: the oldest due frame that is
    /// at most `maxBehind` seconds late (the newest due frame if all are), so a late frame is shown
    /// a little late instead of being lost.
    /// Returns the current frame if nothing new is due.
    /// When frames are spaced closer than `period` (more frames than the display can show),
    /// only 1.5 periods of lag are allowed, whatever maxBehind says.
    /// `due` (optional) receives how many queued frames are due.
    std::pair<FramePtr, FramePtr> peek(double contentTime, double maxBehind, double period, int* due = nullptr);
    /// The frame from peek() was shown: drop it and everything before it. Returns how many
    /// queued frames were passed over without being shown.
    int commit(const FramePtr& shown);
    /// Content time of the next frame not yet shown (< 0: none queued).
    double nextTime();
    void clear();
private:
    std::mutex m_;
    std::deque<FramePtr> frames_;
    FramePtr current_, latestReal_;
};

/// Detects frames that repeat the last accepted frame (a 60 Hz console signal carrying a 30 fps
/// game sends every frame twice). A frame is a repeat only when *every* tile of a 24×15 grid of
/// sparse luma samples is unchanged — a whole-screen average missed small local motion and froze
/// the picture (Lesson 2).
class RepeatDetector {
public:
    std::atomic<bool> enabled{true};
    double globalThreshold = 0.6;   // whole-screen mean abs luma difference (0–255) a repeat must stay under
    double tileThreshold = 0.9;     // typical USB card: repeats ≤ 0.58, quietest real frames ≥ 1.24
    static constexpr int tilesX = 24, tilesY = 15;

    bool isRepeat(const VideoFrame& f);
    void accept() { last_ = pending_; }
    void reset() { last_.clear(); pending_.clear(); }
    double lastDiff = 0, lastMaxTile = 0;
    std::vector<double> recentDiffs();
    std::pair<std::vector<double>, std::vector<double>> tileStats();
private:
    std::vector<uint8_t> last_, pending_;
    std::deque<double> recent_, recentRepeatTiles_, recentUniqueTiles_;
    std::mutex m_;
};

/// Learns the game's real frame interval from unique-frame arrival times and smooths each
/// timestamp onto that rhythm (Lesson 4).
class CadenceTracker {
public:
    std::atomic<bool> enabled{true};
    void reset();
    double holdLimit();             // longest a frame may be held as "repeat" before it's passed on anyway
    double maxInterpolationGap();   // longest gap still interpolated (longer = pause / scene cut)
    double update(double rawTime, bool forced);
    std::pair<double, double> jitter();   // ms, before and after smoothing
    double interval();              // learned game frame interval (s), 0 if unknown
private:
    std::mutex m_;
    std::deque<double> intervals_, rawLog_, smoothLog_;
    std::optional<double> lastRaw_, smoothed_, lastSmoothedOut_, interval_;
};

class FramePipeline {
public:
    FramePipeline(EngineKind kind, int multiplier, Stats& stats);
    ~FramePipeline();

    FrameQueue output;
    RepeatDetector dedupe;
    CadenceTracker cadence;

    void configure(EngineKind k, int m);
    void push(FramePtr frame);   // called on the source thread (admit + enqueue)
    /// Repeat detection, safety net and cadence: true if this is a new game frame (sets its time,
    /// motion and pass-through flag). Needs no image when the source took CPU luma samples.
    bool admit(VideoFrame& f);
    void reset();                // new source: forget the previous frame and the learned rhythm
    double requiredDelay();      // rolling worst (readyTime − contentTime) over 2 s
    YuvInfo yuv;                 // colour matrix of incoming NV12 frames

    std::atomic<EngineKind> kind{EngineKind::Off};
    std::atomic<int> multiplier{2};
    std::atomic<EngineKind> requestedKind{EngineKind::Off};
    std::atomic<int> requestedMultiplier{2};
    std::atomic<int> lastW{0}, lastH{0};   // size of the most recent source frame
    std::string liveDumpDir;               // --dump-live
    std::atomic<bool> wantMotion{false};   // DLSS-G mode: attach motion vectors to real frames

private:
    void process(FramePtr next);
    void warm(Interpolator& interp, int m);
    void dumpSet(const VideoFrame& a, ImagePtr raw, ImagePtr mid, const VideoFrame& b, double motion);
    Stats& stats_;
    std::unique_ptr<SerialQueue> queue_, prep_;
    FramePtr prev_;
    std::shared_ptr<Interpolator> interp_;
    std::unique_ptr<class FlowEngine> motion_;
    std::mutex lock_;
    int pending_ = 0;
    int configGeneration_ = 0, preparing_ = 0;
    std::deque<std::pair<double, double>> lateness_;
    double lastRequired_ = 0.05;
    bool warmup_ = true;
    int warmW_ = 1920, warmH_ = 1080;
    double lastAcceptedTime_ = 0;
    ImagePool rgbaPool_;
    int dumped_ = 0; double lastDump_ = 0;
};
