#include "Audio.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <avrt.h>
#include <ksmedia.h>
#include <algorithm>
#include <cmath>

static WAVEFORMATEXTENSIBLE floatFormat() {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = AudioPassthrough::kChannels;
    f.Format.nSamplesPerSec = AudioPassthrough::kRate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = f.Format.nChannels * 4;
    f.Format.nAvgBytesPerSec = f.Format.nSamplesPerSec * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    return f;
}

static std::string propString(IMMDevice* d, const PROPERTYKEY& key) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(d->OpenPropertyStore(STGM_READ, &ps))) return {};
    PROPVARIANT v; PropVariantInit(&v);
    std::string s;
    if (SUCCEEDED(ps->GetValue(key, &v)) && v.vt == VT_LPWSTR) s = narrow(v.pwszVal);
    PropVariantClear(&v);
    return s;
}

// ---------------------------------------------------------------- AudioPassthrough

AudioPassthrough::AudioPassthrough() : ring_(size_t(kRate) * 3 * kChannels), scratch_(8192 * 2) {}
AudioPassthrough::~AudioPassthrough() { stop(); }

void AudioPassthrough::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { renderThread(); });
}
void AudioPassthrough::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
    std::lock_guard<std::mutex> l(lock_);
    writePos_ = readPos_ = 0; primed_ = false;
}

double AudioPassthrough::bufferedSeconds() { std::lock_guard<std::mutex> l(lock_); return double(writePos_ - readPos_) / kRate; }

void AudioPassthrough::push(const float* src, int frames) {
    {
        std::lock_guard<std::mutex> l(lock_);
        for (int i = 0; i < frames; i++) {
            size_t o = size_t((writePos_ + i) % capacity_) * 2;
            ring_[o] = src[2 * i]; ring_[o + 1] = src[2 * i + 1];
        }
        writePos_ += frames;
        if (writePos_ - readPos_ > capacity_) readPos_ = writePos_ - capacity_;   // overflow: drop oldest
    }
    lastInput_ = hostNow();
}

// Real-time audio thread: n frames of interleaved stereo.
void AudioPassthrough::render(float* out, int n) {
    const double sr = kRate;
    long long target = (long long)(std::max(0.01, targetDelay + userOffset - outputLatency()) * sr);
    std::unique_lock<std::mutex> l(lock_);
    long long available = writePos_ - readPos_;
    // Way behind (e.g. after a stall): jump to the target instead of playing old sound.
    if (available > target + (long long)(0.12 * sr)) { readPos_ = writePos_ - target; available = target; }
    if (!primed_ && available >= target) primed_ = true;
    if (!primed_ || available < n + 2) {
        if (primed_) { underruns_++; primed_ = false; }
        l.unlock();
        std::fill(out, out + size_t(n) * 2, 0.f);
        return;
    }
    // Gentle drift correction: read slightly more/fewer frames than we output.
    long long band = (long long)(0.012 * sr);
    int m = n;
    if (available > target + band) m = n + std::max(1, n / 256);
    else if (available < target - band) m = n - std::max(1, n / 256);
    m = int(std::min<long long>({(long long)m, available - 1, (long long)scratch_.size() / 2 - 1}));
    for (int i = 0; i <= m; i++) {   // copy m+1 frames for interpolation
        size_t o = size_t((readPos_ + i) % capacity_) * 2;
        scratch_[2 * i] = ring_[o]; scratch_[2 * i + 1] = ring_[o + 1];
    }
    readPos_ += m;
    l.unlock();
    float g = muted ? 0.f : volume.load();
    if (m == n) {
        for (int i = 0; i < 2 * n; i++) out[i] = scratch_[i] * g;
    } else {   // linear resample m → n
        double step = double(m) / n;
        for (int i = 0; i < n; i++) {
            double x = i * step; int j = int(x); float f = float(x - j);
            out[2 * i] = (scratch_[2 * j] + (scratch_[2 * j + 2] - scratch_[2 * j]) * f) * g;
            out[2 * i + 1] = (scratch_[2 * j + 1] + (scratch_[2 * j + 3] - scratch_[2 * j + 1]) * f) * g;
        }
    }
}

void AudioPassthrough::renderThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    while (running_) {
        // (Re)open the chosen output, or the current default — also after headphones are plugged in.
        ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioClient> client; ComPtr<IAudioRenderClient> rc;
        std::wstring wanted = wantedOutput();
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) { Sleep(1000); continue; }
        DWORD state = 0;
        if (!wanted.empty() && (FAILED(en->GetDevice(wanted.c_str(), &dev)) || FAILED(dev->GetState(&state)) || state != DEVICE_STATE_ACTIVE)) dev.Reset();
        if (!dev && FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev))) { Sleep(1000); continue; }
        if (FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client))) { Sleep(1000); continue; }
        wchar_t* devId = nullptr; dev->GetId(&devId);
        std::wstring myId = devId ? devId : L""; CoTaskMemFree(devId);
        { std::lock_guard<std::mutex> l(nameLock_); outName_ = propString(dev.Get(), PKEY_Device_FriendlyName); }
        auto fmt = floatFormat();
        HRESULT hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                        AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                        200000 /* 20 ms */, 0, &fmt.Format, nullptr);
        if (FAILED(hr)) { logf("[audio] output init failed: %s", hr2str(hr).c_str()); Sleep(1000); continue; }
        client->SetEventHandle(ev);
        client->GetService(IID_PPV_ARGS(&rc));
        UINT32 bufFrames = 0; client->GetBufferSize(&bufFrames);
        REFERENCE_TIME lat = 0; client->GetStreamLatency(&lat);
        client->Start();
        double lastCheck = hostNow();
        while (running_) {
            WaitForSingleObject(ev, 100);
            UINT32 pad = 0;
            if (FAILED(client->GetCurrentPadding(&pad))) break;   // device removed → reopen
            UINT32 n = bufFrames - pad;
            outLatency_ = double(lat) * 1e-7 + double(pad) / kRate;
            if (n > 0) {
                BYTE* p = nullptr;
                if (SUCCEEDED(rc->GetBuffer(n, &p))) { render((float*)p, int(n)); rc->ReleaseBuffer(n, 0); }
            }
            if (wantedOutput() != wanted) break;   // a different output was chosen
            if (hostNow() - lastCheck > 1.0) {   // default output changed (or the chosen one came back)?
                lastCheck = hostNow();
                ComPtr<IMMDevice> d2;
                if (!wanted.empty() && myId != wanted) { if (SUCCEEDED(en->GetDevice(wanted.c_str(), &d2)) && SUCCEEDED(d2->GetState(&state)) && state == DEVICE_STATE_ACTIVE) break; }
                else if (wanted.empty() && SUCCEEDED(en->GetDefaultAudioEndpoint(eRender, eConsole, &d2))) {
                    wchar_t* id2 = nullptr; d2->GetId(&id2);
                    bool changed = id2 && myId != id2; CoTaskMemFree(id2);
                    if (changed) break;
                }
            }
        }
        client->Stop();
    }
    CloseHandle(ev);
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
}

void AudioPassthrough::setOutput(const std::wstring& id) { std::lock_guard<std::mutex> l(nameLock_); wanted_ = id; }
std::wstring AudioPassthrough::wantedOutput() { std::lock_guard<std::mutex> l(nameLock_); return wanted_; }

std::vector<std::pair<std::wstring, std::string>> AudioPassthrough::outputs() {
    std::vector<std::pair<std::wstring, std::string>> out;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> col;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    if (FAILED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d; wchar_t* id = nullptr;
        if (FAILED(col->Item(i, &d)) || FAILED(d->GetId(&id))) continue;
        out.push_back({id, propString(d.Get(), PKEY_Device_FriendlyName)});
        CoTaskMemFree(id);
    }
    return out;
}

// ---------------------------------------------------------------- AudioCapture

std::vector<std::string> AudioCapture::endpoints() {
    std::vector<std::string> out;
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> col;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return out;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return out;
    UINT n = 0; col->GetCount(&n);
    for (UINT i = 0; i < n; i++) { ComPtr<IMMDevice> d; if (SUCCEEDED(col->Item(i, &d))) out.push_back(propString(d.Get(), PKEY_Device_FriendlyName)); }
    return out;
}

bool AudioCapture::start(const std::string& cardName) {
    stop();
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDeviceCollection> col;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return false;
    if (FAILED(en->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &col))) return false;
    UINT n = 0; col->GetCount(&n);
    std::string first = cardName.substr(0, cardName.find(' '));
    std::wstring bestId; int bestScore = 0;
    for (UINT i = 0; i < n; i++) {
        ComPtr<IMMDevice> d;
        if (FAILED(col->Item(i, &d))) continue;
        std::string name = propString(d.Get(), PKEY_Device_FriendlyName), iface = propString(d.Get(), PKEY_DeviceInterface_FriendlyName);
        int score = 0;
        if (iface == cardName) score = 4;
        else if (icontains(name, cardName)) score = 3;
        else if (!first.empty() && first.size() >= 3 && (icontains(iface, first) || icontains(name, first))) score = 1;
        if (score > bestScore) {
            wchar_t* id = nullptr; d->GetId(&id);
            if (id) { bestId = id; CoTaskMemFree(id); bestScore = score; deviceName = name; }
        }
    }
    if (bestId.empty()) { deviceName.clear(); return false; }
    running_ = true;
    thread_ = std::thread([this, bestId] { run(bestId); });
    return true;
}

void AudioCapture::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void AudioCapture::run(std::wstring id) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    ComPtr<IMMDeviceEnumerator> en; ComPtr<IMMDevice> dev; ComPtr<IAudioClient> client; ComPtr<IAudioCaptureClient> cc;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (SUCCEEDED(hr)) hr = en->GetDevice(id.c_str(), &dev);
    if (SUCCEEDED(hr)) hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    auto fmt = floatFormat();
    if (SUCCEEDED(hr)) hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED,
                                               AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
                                               200000, 0, &fmt.Format, nullptr);
    if (SUCCEEDED(hr)) hr = client->SetEventHandle(ev);
    if (SUCCEEDED(hr)) hr = client->GetService(IID_PPV_ARGS(&cc));
    if (SUCCEEDED(hr)) hr = client->Start();
    if (FAILED(hr)) {
        if (hr == E_ACCESSDENIED) accessDenied = true;
        logf("[audio] capture from %s failed: %s", deviceName.c_str(), hr2str(hr).c_str());
        running_ = false;
    }
    std::vector<float> silence;
    while (running_) {
        WaitForSingleObject(ev, 100);
        UINT32 packet = 0;
        while (running_ && SUCCEEDED(cc->GetNextPacketSize(&packet)) && packet > 0) {
            BYTE* data = nullptr; UINT32 frames = 0; DWORD flags = 0;
            if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) break;
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT) { silence.assign(size_t(frames) * 2, 0.f); out_.push(silence.data(), int(frames)); }
            else out_.push((const float*)data, int(frames));
            cc->ReleaseBuffer(frames);
        }
    }
    if (client) client->Stop();
    CloseHandle(ev);
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
}

// ---------------------------------------------------------------- TestTone

TestTone::TestTone(AudioPassthrough& out, double startTime) {
    thread_ = std::thread([this, &out, startTime] {
        const int chunk = 480;   // 10 ms
        std::vector<float> buf(chunk * 2);
        long long frame = 0;
        while (running_) {
            long long due = (long long)((hostNow() - startTime) * AudioPassthrough::kRate);
            while (frame + chunk <= due) {
                for (int i = 0; i < chunk; i++) {
                    double t = double(frame + i) / AudioPassthrough::kRate;
                    // Ball bounce in the test pattern: |sin(5t)| hits 0 every π/5 s.
                    double phase = std::fmod(t * 5, 3.14159265358979);
                    double since = std::min(phase, 3.14159265358979 - phase) / 5;
                    float env = since < 0.03 ? float(1 - since / 0.03) : 0.f;
                    float v = env * 0.3f * float(std::sin(2 * 3.14159265358979 * 880 * t));
                    buf[2 * i] = v; buf[2 * i + 1] = v;
                }
                out.push(buf.data(), chunk);
                frame += chunk;
            }
            Sleep(10);
        }
    });
}
TestTone::~TestTone() { running_ = false; if (thread_.joinable()) thread_.join(); }
