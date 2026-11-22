// Plays the capture card's sound through the PC's default output, delayed to stay in sync with
// the frame-generated video (Lesson 14). Capture (WASAPI, the card's own audio endpoint) → ring
// buffer → WASAPI render. The ring's fill level is steered to the video delay minus the output
// latency; clock drift between card and output device is absorbed by ±0.4 % resampling.
#pragma once
#include "Core.h"
#include <thread>

class AudioPassthrough {
public:
    static constexpr int kRate = 48000, kChannels = 2;
    AudioPassthrough();
    ~AudioPassthrough();
    void start();
    void stop();

    std::atomic<double> targetDelay{0.06};   // seconds of lag behind capture (from the video delay)
    std::atomic<double> userOffset{0};       // positive plays sound later
    std::atomic<float> volume{1};
    std::atomic<bool> muted{false};

    void push(const float* interleaved, int frames);   // 48 kHz stereo float
    double outputLatency() const { return outLatency_; }
    double bufferedSeconds();
    bool isReceiving() const { return hostNow() - lastInput_ < 0.5; }
    int underruns() const { return underruns_; }
    std::string outputName() { std::lock_guard<std::mutex> l(nameLock_); return outName_; }
    /// Output device: endpoint id, or empty for the Windows default (followed when it changes).
    void setOutput(const std::wstring& id);
    std::wstring wantedOutput();
    static std::vector<std::pair<std::wstring, std::string>> outputs();   // (id, name) of active outputs

private:
    void renderThread();
    void render(float* out, int n);
    std::vector<float> ring_;
    const int capacity_ = kRate * 3;   // 3 s
    long long writePos_ = 0, readPos_ = 0;
    bool primed_ = false;
    std::vector<float> scratch_;
    std::mutex lock_;
    std::atomic<double> outLatency_{0.02};
    std::atomic<double> lastInput_{0};
    std::atomic<int> underruns_{0};
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex nameLock_;
    std::string outName_;
    std::wstring wanted_;
};

/// Captures the capture card's audio endpoint (matched by name) as 48 kHz stereo float.
class AudioCapture {
public:
    explicit AudioCapture(AudioPassthrough& out) : out_(out) {}
    ~AudioCapture() { stop(); }
    /// Starts capturing the endpoint whose name matches `cardName`. False if there is none.
    bool start(const std::string& cardName);
    void stop();
    std::string deviceName;
    std::atomic<bool> accessDenied{false};
    static std::vector<std::string> endpoints();   // capture endpoint names (for --list-devices)
private:
    void run(std::wstring id);
    AudioPassthrough& out_;
    std::thread thread_;
    std::atomic<bool> running_{false};
};

/// Test tone for the test pattern: a short beep each time the ball touches the ground.
class TestTone {
public:
    TestTone(AudioPassthrough& out, double startTime);
    ~TestTone();
private:
    std::thread thread_;
    std::atomic<bool> running_{true};
};
