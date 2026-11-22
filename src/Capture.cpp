#include "Capture.h"
#include <mfapi.h>
#include <mferror.h>
#include <avrt.h>
#include <algorithm>
#include <cmath>

static std::string fourccOf(const GUID& g) {
    if (g == MFVideoFormat_NV12) return "NV12";
    if (g == MFVideoFormat_YUY2) return "YUY2";
    if (g == MFVideoFormat_MJPG) return "MJPG";
    if (g == MFVideoFormat_RGB32) return "RGB32";
    if (g == MFVideoFormat_ARGB32) return "ARGB";
    if (g == MFVideoFormat_RGB24) return "RGB24";
    if (g == MFVideoFormat_I420) return "I420";
    if (g == MFVideoFormat_UYVY) return "UYVY";
    if (g == MFVideoFormat_P010) return "P010";
    if (g == MFVideoFormat_H264) return "H264";
    char s[5] = {char(g.Data1 & 0xff), char((g.Data1 >> 8) & 0xff), char((g.Data1 >> 16) & 0xff), char((g.Data1 >> 24) & 0xff), 0};
    for (char& c : s) if (c && (c < 32 || c > 126)) c = '?';
    return s;
}

Uploader CaptureSource::uploader_;
static std::mutex g_devLock;
static std::vector<CaptureDevice> g_devCache;
static bool g_devCached = false;

std::vector<CaptureDevice> CaptureSource::devices(bool refresh) {
    std::lock_guard<std::mutex> l(g_devLock);
    if (g_devCached && !refresh) return g_devCache;
    std::vector<CaptureDevice> out;
    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 1);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devs = nullptr; UINT32 n = 0;
    if (SUCCEEDED(MFEnumDeviceSources(attr.Get(), &devs, &n))) {
        for (UINT32 i = 0; i < n; i++) {
            CaptureDevice d;
            wchar_t* s = nullptr; UINT32 len = 0;
            if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &s, &len))) { d.name = narrow(s); CoTaskMemFree(s); }
            if (SUCCEEDED(devs[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &s, &len))) { d.symlink = s; CoTaskMemFree(s); }
            ComPtr<IMFMediaSource> src;
            if (SUCCEEDED(devs[i]->ActivateObject(IID_PPV_ARGS(&src)))) {
                ComPtr<IMFPresentationDescriptor> pd;
                ComPtr<IMFStreamDescriptor> sd; BOOL sel; ComPtr<IMFMediaTypeHandler> h;
                if (SUCCEEDED(src->CreatePresentationDescriptor(&pd)) && SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &sel, &sd)) &&
                    SUCCEEDED(sd->GetMediaTypeHandler(&h))) {
                    DWORD count = 0; h->GetMediaTypeCount(&count);
                    for (DWORD k = 0; k < count; k++) {
                        ComPtr<IMFMediaType> t;
                        if (FAILED(h->GetMediaTypeByIndex(k, &t))) continue;
                        CaptureFormat f; UINT32 w = 0, hh = 0;
                        MFGetAttributeSize(t.Get(), MF_MT_FRAME_SIZE, &w, &hh);
                        MFGetAttributeRatio(t.Get(), MF_MT_FRAME_RATE, &f.fpsNum, &f.fpsDen);
                        t->GetGUID(MF_MT_SUBTYPE, &f.subtype);
                        f.w = int(w); f.h = int(hh); f.fourcc = fourccOf(f.subtype); f.index = k;
                        d.formats.push_back(f);
                    }
                }
                src->Shutdown();
            }
            devs[i]->ShutdownObject();
            devs[i]->Release();
            out.push_back(std::move(d));
        }
        CoTaskMemFree(devs);
    }
    // Capture cards first, built-in cameras last.
    std::stable_sort(out.begin(), out.end(), [](const CaptureDevice& a, const CaptureDevice& b) {
        auto cam = [](const CaptureDevice& d) { return icontains(d.name, "camera") || icontains(d.name, "webcam") || icontains(d.name, "integrated"); };
        return !cam(a) && cam(b);
    });
    g_devCache = out; g_devCached = true;
    return out;
}

int CaptureSource::pickFormat(const CaptureDevice& d, int wantW, int wantH, double wantFPS) {
    int targetW = wantW > 0 ? wantW : 1920, targetH = wantH > 0 ? wantH : 1080;
    int best = -1; double bestScore = 0;
    for (size_t i = 0; i < d.formats.size(); i++) {
        const auto& f = d.formats[i];
        double rate = f.fps(), score = 0;
        if (wantFPS > 0) {
            if (std::abs(rate - wantFPS) > 0.6) score += 1e9 + std::abs(wantFPS - rate) * 1e6;
        } else {
            if (rate > kAutoMaxRate + 0.01) score += 1e9 + rate * 1e6;   // high-speed modes only add USB bandwidth
            else score += (kAutoMaxRate - rate) * 1e6;
        }
        if (!(f.w == targetW && f.h == targetH)) score += 1e12 + std::abs(double(f.w) * f.h - double(targetW) * targetH);
        // Native uncompressed 4:2:0 first, then 4:2:2, then compressed (MJPEG needs decoding).
        if (f.subtype == MFVideoFormat_NV12) {}
        else if (f.subtype == MFVideoFormat_YUY2) score += 4e4;
        else if (f.subtype == MFVideoFormat_MJPG) score += 1e5;
        else score += 2e5;
        if (best < 0 || score < bestScore) { best = int(i); bestScore = score; }
    }
    return best;
}

std::string CaptureSource::describeDevices() {
    std::string s = "Video capture devices:\n";
    auto devs = devices(true);
    if (devs.empty()) s += "  (none)\n";
    for (size_t i = 0; i < devs.size(); i++) {
        const auto& d = devs[i];
        s += strf("  [%zu] %s\n", i, d.name.c_str());
        int p = pickFormat(d, 0, 0, 0);
        if (p >= 0) s += strf("      auto capture mode -> %dx%d @ %.2f Hz [%s]\n", d.formats[p].w, d.formats[p].h, d.formats[p].fps(), d.formats[p].fourcc.c_str());
        for (const auto& f : d.formats) s += strf("      %dx%d @ %.3f fps  [%s]\n", f.w, f.h, f.fps(), f.fourcc.c_str());
    }
    return s;
}

CaptureSource::CaptureSource(std::string q, int w, int h, double fps) : query_(std::move(q)), wantW_(w), wantH_(h), wantFPS_(fps) {}

bool CaptureSource::start(std::string& error) {
    auto devs = devices();
    if (devs.empty()) { error = "No capture card found. Is it plugged in?"; return false; }
    const CaptureDevice* dev = &devs[0];
    if (!query_.empty()) {
        bool isIndex = !query_.empty() && std::all_of(query_.begin(), query_.end(), ::isdigit);
        const CaptureDevice* found = nullptr;
        if (isIndex && size_t(atoi(query_.c_str())) < devs.size()) found = &devs[atoi(query_.c_str())];
        for (auto& d : devs) if (!found && d.name == query_) found = &d;
        for (auto& d : devs) if (!found && icontains(d.name, query_)) found = &d;
        if (!found) { error = "Capture device \"" + query_ + "\" not found."; return false; }
        dev = found;
    }
    deviceName = dev->name;
    int fi = pickFormat(*dev, wantW_, wantH_, wantFPS_);
    if (fi < 0) { error = "The capture device reports no video formats."; return false; }
    CaptureFormat fmt = dev->formats[fi];

    ComPtr<IMFAttributes> attr;
    MFCreateAttributes(&attr, 2);
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, dev->symlink.c_str());
    HRESULT hr = MFCreateDeviceSource(attr.Get(), &source_);
    if (hr == E_ACCESSDENIED) {
        permissionDenied = true;
        error = "Camera access is turned off for desktop apps, so FrameGen can't read your capture card.";
        return false;
    }
    if (FAILED(hr)) { error = "Could not open the capture card (" + hr2str(hr) + "). Is another app using it?"; return false; }

    // Select the native format on the device.
    {
        ComPtr<IMFPresentationDescriptor> pd; ComPtr<IMFStreamDescriptor> sd; BOOL sel; ComPtr<IMFMediaTypeHandler> h; ComPtr<IMFMediaType> t;
        if (SUCCEEDED(source_->CreatePresentationDescriptor(&pd)) && SUCCEEDED(pd->GetStreamDescriptorByIndex(0, &sel, &sd)) &&
            SUCCEEDED(sd->GetMediaTypeHandler(&h)) && SUCCEEDED(h->GetMediaTypeByIndex(fmt.index, &t))) {
            hr = h->SetCurrentMediaType(t.Get());
            if (FAILED(hr)) logf("[capture] SetCurrentMediaType(native) failed: %s", hr2str(hr).c_str());
        }
    }

    auto& gpu = Gpu::get();
    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 4);
    // USB cards deliver frames into system memory anyway. Taking them from there lets repeat
    // detection run on the CPU (like the Mac) and the upload to the GPU never waits, so the capture
    // thread can't get stuck behind frame generation work queued on the GPU. FG_CAPTURE_D3D=1 uses
    // Media Foundation's GPU path instead.
    useD3D_ = getenv("FG_CAPTURE_D3D") != nullptr;
    if (useD3D_ && gpu.mfManager) ra->SetUnknown(MF_SOURCE_READER_D3D_MANAGER, gpu.mfManager.Get());
    ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    ra->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    ra->SetUINT32(MF_SOURCE_READER_DISCONNECT_MEDIASOURCE_ON_SHUTDOWN, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(), &reader_);
    if (FAILED(hr)) { error = "Could not start capture (" + hr2str(hr) + ")."; source_->Shutdown(); source_.Reset(); return false; }

    ComPtr<IMFMediaType> out;
    MFCreateMediaType(&out);
    out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
    MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, fmt.w, fmt.h);
    MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, fmt.fpsNum, fmt.fpsDen);
    hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
    if (FAILED(hr)) {
        error = strf("The capture card's %s format can't be converted to NV12 (%s).", fmt.fourcc.c_str(), hr2str(hr).c_str());
        reader_.Reset(); source_->Shutdown(); source_.Reset(); return false;
    }

    // Verify what was actually negotiated (Lesson 5) and read the colour matrix.
    ComPtr<IMFMediaType> native, cur;
    reader_->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, MF_SOURCE_READER_CURRENT_TYPE_INDEX, &native);
    reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &cur);
    UINT32 nw = fmt.w, nh = fmt.h, rn = fmt.fpsNum, rd = fmt.fpsDen;
    GUID nsub = fmt.subtype;
    if (native) {
        MFGetAttributeSize(native.Get(), MF_MT_FRAME_SIZE, &nw, &nh);
        MFGetAttributeRatio(native.Get(), MF_MT_FRAME_RATE, &rn, &rd);
        native->GetGUID(MF_MT_SUBTYPE, &nsub);
    }
    UINT32 matrix = 0, range = 0;
    if (cur) { matrix = MFGetAttributeUINT32(cur.Get(), MF_MT_YUV_MATRIX, 0); range = MFGetAttributeUINT32(cur.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0); }
    if (!matrix && native) matrix = MFGetAttributeUINT32(native.Get(), MF_MT_YUV_MATRIX, 0);
    if (!range && native) range = MFGetAttributeUINT32(native.Get(), MF_MT_VIDEO_NOMINAL_RANGE, 0);
    yuv.bt709 = matrix ? matrix == MFVideoTransferMatrix_BT709 : nh >= 720;
    yuv.fullRange = range == MFNominalRange_0_255;
    activeW = int(nw); activeH = int(nh); activeRate = rd ? double(rn) / rd : fmt.fps(); activeFourcc = fourccOf(nsub);
    { std::lock_guard<std::mutex> l(m_); name_ = strf("%s %dx%d@%.2f", dev->name.c_str(), activeW, activeH, activeRate); }
    logf("[capture] %s: native %dx%d %s @ %.3f Hz (%u/%u) -> NV12, matrix %s, %s range, D3D %s",
         dev->name.c_str(), activeW, activeH, activeFourcc.c_str(), activeRate, rn, rd,
         yuv.bt709 ? "BT.709" : "BT.601", yuv.fullRange ? "full" : "limited", gpu.mfManager ? "yes" : "no");

    running_ = true;
    if (!exited_) exited_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    ResetEvent(exited_);
    thread_ = std::thread([this] { run(); });
    return true;
}

void CaptureSource::stop() {
    if (!running_.exchange(false) && !thread_.joinable()) return;
    if (thread_.joinable()) {
        // Let the capture thread finish its current ReadSample and exit by itself (the card sends a
        // frame every few ms). Shutting the source down while ReadSample is in progress deadlocked
        // the two threads and froze the window on close. Only if no frame comes (card unplugged /
        // stuck) shut the source down to unblock it.
        if (WaitForSingleObject(exited_, 500) != WAIT_OBJECT_0 && source_) {
            logf("[capture] no frame for 500 ms while stopping — shutting the source down");
            source_->Shutdown();
        }
        if (WaitForSingleObject(exited_, 3000) == WAIT_OBJECT_0) thread_.join();
        else { logf("[capture] capture thread didn't stop — leaving it"); thread_.detach(); leaked_ = true; return; }
    }
    reader_.Reset();
    if (source_) source_->Shutdown();
    source_.Reset();
}

void CaptureSource::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Capture", &task);
    while (running_) {
        DWORD si = 0, flags = 0; LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        HRESULT hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &si, &flags, &ts, &sample);
        if (!running_) break;
        if (FAILED(hr)) {
            if (hr == E_ACCESSDENIED) permissionDenied = true;
            if (stats) stats->setError(hr == E_ACCESSDENIED ? "Camera access is turned off for desktop apps, so FrameGen can't read your capture card."
                                                            : "Capture stopped (" + hr2str(hr) + "). Is the card still plugged in?");
            logf("[capture] ReadSample failed: %s", hr2str(hr).c_str());
            break;
        }
        if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) { logf("[capture] stream ended (flags 0x%x)", flags); break; }
        if (!sample) continue;
        deliver(sample.Get(), ts);
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
    SetEvent(exited_);
}

bool CaptureSource::deliver(IMFSample* sample, LONGLONG ts) {
    auto& gpu = Gpu::get();
    double now = hostNow();
    // Timestamp on the host (QPC) clock: prefer the device's QPC timestamp; otherwise map the
    // sample time with the smallest observed arrival offset (keeps the device's own spacing).
    double t = now;
    UINT64 devTs = 0;
    if (SUCCEEDED(sample->GetUINT64(MFSampleExtension_DeviceTimestamp, &devTs)) && devTs) {
        double d = double(devTs) * 1e-7;
        if (std::abs(d - now) < 0.5) t = d;
    } else {
        double st = double(ts) * 1e-7, off = now - st;
        if (!haveOffset_ || off < offset_ || std::abs(off - offset_) > 0.5) { offset_ = off; haveOffset_ = true; }
        else offset_ += (off - offset_) * 0.0005;   // follow slow drift upwards
        t = st + offset_;
    }
    if (frames_ < 3 && g_opt.log) logf("[capture] frame %lld: sample time %.4f s, device ts %.4f s, now %.4f s -> %.4f", frames_, double(ts) * 1e-7, double(devTs) * 1e-7, now, t);
    frames_++;

    ComPtr<IMFMediaBuffer> buf;
    if (FAILED(sample->GetBufferByIndex(0, &buf))) return false;
    ComPtr<IMFDXGIBuffer> dx;
    ImagePtr img;
    std::shared_ptr<std::vector<uint8_t>> luma;
    uint64_t uploadFence = 0;
    FramePtr early;
    if (SUCCEEDED(buf.As(&dx))) {
        ComPtr<ID3D11Texture2D> tex; UINT sub = 0;
        if (FAILED(dx->GetResource(IID_PPV_ARGS(&tex))) || FAILED(dx->GetSubresourceIndex(&sub))) return false;
        D3D11_TEXTURE2D_DESC td; tex->GetDesc(&td);
        if (td.Format != DXGI_FORMAT_NV12) { if (frames_ < 5) logf("[capture] unexpected texture format %d", int(td.Format)); return false; }
        img = pool_.acquire(int(td.Width) & ~1, int(td.Height) & ~1, DXGI_FORMAT_NV12);
        if (!img) return false;
        auto l = gpu.lock();
        D3D11_BOX box{0, 0, 0, UINT(img->w), UINT(img->h), 1};
        gpu.ctx->CopySubresourceRegion(img->tex.Get(), 0, 0, 0, 0, tex.Get(), sub, &box);
    } else {
        // System-memory frames: sparse luma for repeat detection on the CPU, then upload through
        // a ring of staging textures (never waits for the GPU unless all four are still busy).
        ComPtr<IMF2DBuffer> b2; BYTE* p = nullptr; LONG pitch = 0;
        bool locked2d = SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&p, &pitch));
        DWORD maxLen = 0, len = 0;
        if (!locked2d) { if (FAILED(buf->Lock(&p, &maxLen, &len))) return false; pitch = activeW; }
        int w = activeW & ~1, h = activeH & ~1;
        luma = std::make_shared<std::vector<uint8_t>>();
        luma->reserve(size_t((w + 7) / 8) * ((h + 7) / 8));
        for (int y = 3; y < h; y += 8) { const BYTE* row = p + size_t(y) * pitch; for (int x = 5; x < w; x += 8) luma->push_back(row[x]); }
        early = std::make_shared<VideoFrame>();
        early->time = t; early->sparseLuma = luma; early->lumaCols = (w - 5 + 7) / 8;
        if (admit && !admit(*early)) { if (locked2d) b2->Unlock2D(); else buf->Unlock(); return true; }   // repeat: no upload
        early->admitted = admit != nullptr;
        double tu0 = hostNow();
        if (!uploaderFailed_ && (uploader_.ready() || uploader_.init())) {
            img = uploader_.upload(p, p + size_t(pitch) * activeH, pitch, w, h, uploadFence);
            if (getenv("FG_TRACE") && hostNow() - tu0 > 0.004) logf("[trace] capture upload (own device) took %.1f ms", (hostNow() - tu0) * 1000);
        } else {
            uploaderFailed_ = true;
            img = pool_.acquire(w, h, DXGI_FORMAT_NV12);
        }
        if (img && uploaderFailed_) {
            auto l = gpu.lock();
            if (getenv("FG_TRACE") && hostNow() - tu0 > 0.004) logf("[trace] capture waited %.1f ms for the GPU lock", (hostNow() - tu0) * 1000);
            if (!stage_[0] || stageW_ != w || stageH_ != h) {
                D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_NV12;
                td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                for (auto& st : stage_) { st.Reset(); gpu.dev->CreateTexture2D(&td, nullptr, &st); }
                stageW_ = w; stageH_ = h;
            }
            D3D11_MAPPED_SUBRESOURCE m;
            ID3D11Texture2D* stage = nullptr;
            for (int k = 0; k < 4 && !stage; k++) {
                int i = (stageNext_ + k) % 4;
                if (stage_[i] && SUCCEEDED(gpu.ctx->Map(stage_[i].Get(), 0, D3D11_MAP_WRITE, D3D11_MAP_FLAG_DO_NOT_WAIT, &m))) { stage = stage_[i].Get(); stageNext_ = (i + 1) % 4; }
            }
            if (!stage && stage_[stageNext_] && SUCCEEDED(gpu.ctx->Map(stage_[stageNext_].Get(), 0, D3D11_MAP_WRITE, 0, &m))) { stage = stage_[stageNext_].Get(); stageNext_ = (stageNext_ + 1) % 4; }
            if (stage) {
                for (int y = 0; y < h; y++) memcpy((BYTE*)m.pData + size_t(y) * m.RowPitch, p + size_t(y) * pitch, w);
                BYTE* uvSrc = p + size_t(pitch) * activeH;
                BYTE* uvDst = (BYTE*)m.pData + size_t(m.RowPitch) * h;
                for (int y = 0; y < h / 2; y++) memcpy(uvDst + size_t(y) * m.RowPitch, uvSrc + size_t(y) * pitch, w);
                gpu.ctx->Unmap(stage, 0);
                gpu.ctx->CopyResource(img->tex.Get(), stage);
                gpu.ctx->Flush();   // submit now: otherwise the copy waits in the command buffer and the staging texture stays busy
                if (getenv("FG_TRACE") && hostNow() - tu0 > 0.004) logf("[trace] capture upload took %.1f ms", (hostNow() - tu0) * 1000);
            }
        }
        if (locked2d) b2->Unlock2D(); else buf->Unlock();
        if (!img) return false;
    }
    auto f = early ? early : std::make_shared<VideoFrame>();
    f->image = img;
    if (!early) f->time = t;
    f->sparseLuma = luma;
    f->uploadFence = uploadFence;
    if (onFrame) onFrame(f);
    return true;
}
