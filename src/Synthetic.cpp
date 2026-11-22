#include "Synthetic.h"
#include <avrt.h>
#include <cmath>

// Capture-card compression noise: at sharp edges, ~5 % of pixels change by 10–40 levels per frame.
static const char* kNoise = R"(
Texture2D<float4> T : register(t0);
RWTexture2D<unorm float4> O : register(u0);
cbuffer C : register(b0) { uint seed; uint3 pad; };
uint hash(uint x) { x ^= x >> 16; x *= 0x7feb352du; x ^= x >> 15; x *= 0x846ca68bu; x ^= x >> 16; return x; }
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    float4 c = T[id.xy];
    float4 n = T[uint2(min(id.x + 1, w - 1), id.y)];
    uint r = hash(id.x * 1973u + id.y * 9277u + seed * 26699u);
    if (abs(c.g - n.g) * 255.0 > 40.0 && (r % 100u) < 5u) {
        float d = float((r >> 8) % 31u + 10u) / 255.0;
        c.rgb = saturate(c.rgb + (((r >> 16) & 1u) ? -d : d));
    }
    O[id.xy] = float4(c.rgb, 1);
}
)";
static const char* kCopy = R"(
Texture2D<float4> T : register(t0);
RWTexture2D<unorm float4> O : register(u0);
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    O[id.xy] = float4(T[id.xy].rgb, 1);
}
)";

SyntheticSource::SyntheticSource(double fps, int width, int height, int repeatCount)
    : fps_(fps), w_(width & ~1), h_(height & ~1), repeat_(std::max(1, repeatCount)) {
    name_ = strf("Test pattern %dx%d@%d", w_, h_, int(std::lround(fps_)));
    if (repeat_ > 1) name_ += strf(" (each frame x%d)", repeat_);
    yuv = YuvInfo{true, false};
}

D2D1_RECT_F SyntheticSource::hudPanel(float W, float H) { return D2D1::RectF(W * 0.56f, H * 0.29f, W * 0.96f, H * 0.53f); }

ImagePtr SyntheticSource::render(int index, double fps) { return draw(index, fps > 0 ? fps : fps_, noise, true); }
ImagePtr SyntheticSource::renderRGBA(int index, double fps) { return draw(index, fps > 0 ? fps : fps_, false, false); }

ImagePtr SyntheticSource::draw(int index, double fps, bool withNoise, bool toNV12) {
    std::lock_guard<std::mutex> guard(drawLock_);
    auto& gpu = Gpu::get();
    if (!dc_) {
        if (!gpu.d2dDevice || FAILED(gpu.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return nullptr;
        float s = h_ / 1080.f;
        gpu.dwrite->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 44 * s, L"en-us", &big_);
        gpu.dwrite->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 26 * s, L"en-us", &small_);
    }
    auto bgra = bgraPool_.acquire(w_, h_, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!bgra) return nullptr;
    ComPtr<IDXGISurface> surf; bgra->tex.As(&surf);
    ComPtr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return nullptr;

    const float W = float(w_), H = float(h_), s = H / 1080.f;
    const double tReal = index / fps;
    const double t = (scene == "still" || scene == "static") ? 1.0 : tReal;   // freeze the big motion in low-motion scenes
    // CoreGraphics-style rect (origin bottom-left, like the Mac app's scene) → Direct2D.
    auto cg = [&](double x, double y, double w, double h) { return D2D1::RectF(float(x), float(H - y - h), float(x + w), float(H - y)); };
    {
        auto l = gpu.lock();
        dc_->SetTarget(target.Get());
        dc_->BeginDraw();
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());
        ComPtr<ID2D1SolidColorBrush> br;
        dc_->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0), &br);
        auto fill = [&](D2D1_RECT_F r, float cr, float cgc, float cb, float a = 1) { br->SetColor(D2D1::ColorF(cr, cgc, cb, a)); dc_->FillRectangle(r, br.Get()); };

        fill(cg(0, H * 0.45, W, H * 0.55), 0.35f, 0.55f, 0.85f);   // sky
        fill(cg(0, 0, W, H * 0.45), 0.25f, 0.45f, 0.2f);            // ground

        // Panning "buildings" — a camera pan (≈ 900 px/s at 1080p). "stopgo": still 1.5 s, then moving 1.5 s.
        double panT = t;
        if (scene == "stopgo") { double cycle = std::floor(t / 3), ph = t - cycle * 3; panT = cycle * 1.5 + std::max(0.0, ph - 1.5); }
        double pan = panT * (scene == "fastpan" ? 3600 : 900) * s, spacing = 260 * s;   // fastpan: 120 px/frame, like a quick camera turn
        double x = -std::fmod(pan, spacing) - spacing;
        long long k = (long long)std::floor(pan / spacing);
        while (x < W + spacing) {
            double hgt = (220 + double((k * 7919) % 5) * 70) * s;
            float shade = 0.25f + float((k * 31) % 4) * 0.08f;
            fill(cg(x, H * 0.45, 180 * s, hgt), shade, shade, shade + 0.05f);
            for (double wy = H * 0.45 + 20 * s; wy < H * 0.45 + hgt - 30 * s; wy += 50 * s)
                for (double wx : {x + 25 * s, x + 105 * s}) fill(cg(wx, wy, 40 * s, 25 * s), 1, 0.9f, 0.5f);
            x += spacing; k++;
        }

        // Fast ball bouncing across the road.
        double bx = (0.5 + 0.42 * std::sin(t * 2.4)) * W;
        double by = H * 0.2 + std::abs(std::sin(t * 5)) * 180 * s;
        br->SetColor(D2D1::ColorF(0.95f, 0.2f, 0.15f));
        dc_->FillEllipse(D2D1::Ellipse(D2D1::Point2F(float(bx), float(H - by)), 50 * s, 50 * s), br.Get());

        // Low-motion scene: one small distant car crawling along the road (~120 px/s).
        if (scene == "still") fill(cg(200 + std::fmod(tReal * 120, W - 400), H * 0.38, 40 * s, 18 * s), 0.9f, 0.85f, 0.2f);

        // Spinning bar (rotation is hard for optical flow).
        dc_->SetTransform(D2D1::Matrix3x2F::Rotation(float(-t * 3.0 * 180.0 / 3.14159265358979), D2D1::Point2F(0, 0)) *
                          D2D1::Matrix3x2F::Translation(W * 0.82f, H * 0.25f));
        fill(D2D1::RectF(-150 * s, -12 * s, 150 * s, 12 * s), 1, 1, 1);
        dc_->SetTransform(D2D1::Matrix3x2F::Identity());

        // Static HUD with a changing counter (HUDs commonly smear under interpolation).
        fill(cg(30 * s, H - 110 * s, 520 * s, 80 * s), 0, 0, 0, 0.6f);
        br->SetColor(D2D1::ColorF(1, 1, 1));
        std::wstring counter = widen(strf("SOURCE FRAME %05d", scene == "static" ? 0 : index));
        dc_->DrawTextW(counter.c_str(), UINT32(counter.size()), big_.Get(), D2D1::RectF(50 * s, 40 * s, 600 * s, 110 * s), br.Get());
        if (scene == "hud") {
            // Game-style overlay: semi-transparent panel of static text over the panning city.
            D2D1_RECT_F r = hudPanel(W, H);
            fill(r, 0, 0, 0, 0.45f);
            br->SetColor(D2D1::ColorF(1, 1, 1));
            const wchar_t* lines[] = {L"OBJECTIVE  Reach the marina", L"◆ Distance  1.2 mi", L"◆ Wanted level  ★★☆☆☆",
                                      L"R1  Weapon wheel    △  Phone", L"Cash  $48,250"};
            for (int i = 0; i < 5; i++) {
                float top = r.top + (i + 1) * 40 * s - 32 * s;
                dc_->DrawTextW(lines[i], UINT32(wcslen(lines[i])), small_.Get(), D2D1::RectF(r.left + 20 * s, top, r.right, top + 40 * s), br.Get());
            }
        }
        dc_->EndDraw();
        dc_->SetTarget(nullptr);
    }

    ImagePtr src = bgra;
    if (withNoise || !toNV12) {
        auto rgba = rgbaPool_.acquire(w_, h_, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!rgba) return nullptr;
        auto l = gpu.lock();
        if (withNoise) { struct { UINT seed, pad[3]; } c{UINT(index), {}}; gpu.dispatch(gpu.cs(kNoise), {bgra->srv[0].Get()}, {rgba->uav.Get()}, &c, sizeof c, w_, h_); }
        else gpu.dispatch(gpu.cs(kCopy), {bgra->srv[0].Get()}, {rgba->uav.Get()}, nullptr, 0, w_, h_);
        src = rgba;
    }
    if (!toNV12) return src;
    auto nv12 = nv12Pool_.acquire(w_, h_, DXGI_FORMAT_NV12);
    if (!nv12) return nullptr;
    gpu.rgbaToNv12(*src, *nv12);
    return nv12;
}

bool SyntheticSource::start(std::string& error) {
    if (!Gpu::get().d2dDevice) { error = "Direct2D is not available."; return false; }
    running_ = true;
    startTime = hostNow() + 0.02;
    thread_ = std::thread([this] { run(); });
    return true;
}

void SyntheticSource::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

void SyntheticSource::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Capture", &task);
    int lastContent = -1;
    ImagePtr last;
    for (long long i = 0; running_; i++) {
        double captureTime = startTime + double(i) / fps_;
        if (hostNow() - captureTime > 0.25) { startTime = hostNow() - double(i) / fps_; captureTime = startTime + double(i) / fps_; }   // fell behind (debugger, sleep)
        preciseSleepUntil(captureTime);
        if (!running_) break;
        int content; double contentFPS;
        if (gameFPS > 0) {
            double tCap = double(i) / fps_;
            double tSig = signalHz > 0 ? std::floor(tCap * signalHz + 1e-9) / signalHz : tCap;
            content = int(std::floor(tSig * gameFPS + 1e-9)); contentFPS = gameFPS;
        } else {
            content = int(i / repeat_); contentFPS = fps_ / repeat_;
        }
        // Repeated content: send the same picture again, like a real capture card does.
        if (content != lastContent || !last) {
            last = render(content, contentFPS);
            lastContent = content;
        }
        if (!last) continue;
        auto f = std::make_shared<VideoFrame>();
        f->image = last;
        f->time = captureTime;
        if (onFrame) onFrame(f);
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
}
