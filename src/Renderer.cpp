#include "Renderer.h"
#include <avrt.h>
#include <dwmapi.h>
#include <algorithm>
#include <cmath>
#include <set>

static const char* kQuad = R"(
cbuffer C : register(b0) { float4 rect; float alpha; float3 pad; };   // rect = NDC left, top, right, bottom
struct VO { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
VO vsQuad(uint id : SV_VertexID) {
    float2 t = float2(id & 1, id >> 1);
    VO o; o.pos = float4(lerp(rect.xy, rect.zw, t), 0, 1); o.uv = t; return o;
}
Texture2D T : register(t0);
SamplerState S : register(s0);
float4 psImage(VO i) : SV_Target { return float4(T.Sample(S, i.uv).rgb, 1); }
float4 psOverlay(VO i) : SV_Target { return T.Sample(S, i.uv) * alpha; }   // premultiplied (Direct2D)
)";

struct QuadCB { float rect[4]; float alpha; float pad[3]; };

bool Renderer::init(HWND hwnd, std::string& error) {
    hwnd_ = hwnd;
    auto& gpu = Gpu::get();
    RECT rc; GetClientRect(hwnd, &rc);
    bw_ = std::max<int>(1, rc.right - rc.left); bh_ = std::max<int>(1, rc.bottom - rc.top);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bw_; sd.Height = bh_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (gpu.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    ComPtr<IDXGISwapChain1> sc1;
    HRESULT hr = gpu.factory->CreateSwapChainForHwnd(gpu.dev.Get(), hwnd, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) { error = "Could not create the display surface (" + hr2str(hr) + ")."; return false; }
    gpu.factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);   // we do borderless full screen ourselves
    sc1.As(&swap_);
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    gpu.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE; bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA; bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    gpu.dev->CreateBlendState(&bd, &premul_);
    D3D11_RASTERIZER_DESC rd{}; rd.FillMode = D3D11_FILL_SOLID; rd.CullMode = D3D11_CULL_NONE; rd.DepthClipEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &rs_);
    rd.ScissorEnable = TRUE;
    gpu.dev->CreateRasterizerState(&rd, &rsScissor_);
    gpu.vs(kQuad, "vsQuad"); gpu.ps(kQuad, "psImage"); gpu.ps(kQuad, "psOverlay");   // compile now, not on the first frame
    return true;
}

void Renderer::start() {
    if (running_) return;
    running_ = true;
    thread_ = std::thread([this] { run(); });
    SetThreadDescription(thread_.native_handle(), L"FrameGen.present");
}

void Renderer::stop() {
    running_ = false;
    if (thread_.joinable()) thread_.join();
}

double Presenter::displayHz() {
    double now = hostNow();
    if (now - cachedHzAt_ < 1.0 && cachedHz_ > 0) return cachedHz_;
    cachedHzAt_ = now;
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
    double hz = 60;
    if (GetMonitorInfoW(mon, &mi)) {
        DEVMODEW dm{}; dm.dmSize = sizeof dm;
        if (EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1) hz = dm.dmDisplayFrequency;
        // Exact rate (e.g. 59.94) from DWM when it agrees with the monitor's nominal rate.
        DWM_TIMING_INFO ti{}; ti.cbSize = sizeof ti;
        if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &ti)) && ti.rateRefresh.uiDenominator) {
            double d = double(ti.rateRefresh.uiNumerator) / ti.rateRefresh.uiDenominator;
            if (std::abs(d - hz) < 1.5) hz = d;
        }
    }
    cachedHz_ = hz;
    return hz;
}

std::vector<int> Presenter::displayRates() {
    HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{}; mi.cbSize = sizeof mi;
    if (!GetMonitorInfoW(mon, &mi)) return {};
    if (ratesMonitor_ == mi.szDevice) return ratesCache_;
    DEVMODEW cur{}; cur.dmSize = sizeof cur;
    EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &cur);
    std::set<int> rates;
    DEVMODEW dm{}; dm.dmSize = sizeof dm;
    for (DWORD i = 0; EnumDisplaySettingsW(mi.szDevice, i, &dm); i++)
        if (dm.dmPelsWidth == cur.dmPelsWidth && dm.dmPelsHeight == cur.dmPelsHeight && dm.dmDisplayFrequency > 1) rates.insert(int(dm.dmDisplayFrequency));
    ratesCache_.assign(rates.begin(), rates.end());
    ratesMonitor_ = mi.szDevice;
    return ratesCache_;
}

bool Renderer::resizeBuffers() {
    auto& gpu = Gpu::get();
    int w = std::max(1, pendW_.load()), h = std::max(1, pendH_.load());
    resizePending_ = false;
    if (w == bw_ && h == bh_) return true;
    auto l = gpu.lock();
    gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    gpu.ctx->Flush();
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN,
                                      DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (gpu.tearingSupported ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0));
    if (FAILED(hr)) { logf("[present] ResizeBuffers failed: %s", hr2str(hr).c_str()); }
    ComPtr<ID3D11Texture2D> bb;
    swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
    gpu.dev->CreateRenderTargetView(bb.Get(), nullptr, &rtv_);
    bw_ = w; bh_ = h;
    return SUCCEEDED(hr);
}

void Renderer::drawFrame(const GpuImage& img, int dw, int dh, const D3D11_RECT* scissor) {
    auto& gpu = Gpu::get();
    float va = float(dw) / dh, fa = float(img.w) / img.h, sx = 1, sy = 1;
    switch (scaling.load()) {
    case 2: break;                                                      // stretch
    case 1: if (va > fa) sy = va / fa; else sx = fa / va; break;        // fill: cover, crop the overflow
    default: if (va > fa) sx = fa / va; else sy = va / fa; break;       // fit: letterbox
    }
    QuadCB c{{-sx, sy, sx, -sy}, 1, {}};
    gpu.setCB(0, &c, sizeof c, true);
    ID3D11ShaderResourceView* srv = img.srv[0].Get();
    gpu.ctx->PSSetShaderResources(0, 1, &srv);
    gpu.ctx->PSSetShader(gpu.ps(kQuad, "psImage"), nullptr, 0);
    gpu.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
    if (scissor) { gpu.ctx->RSSetState(rsScissor_.Get()); gpu.ctx->RSSetScissorRects(1, scissor); }
    else gpu.ctx->RSSetState(rs_.Get());
    gpu.ctx->Draw(4, 0);
}

void Renderer::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    DWORD task = 0;
    HANDLE av = AvSetMmThreadCharacteristicsW(L"Games", &task);
    if (av) AvSetMmThreadPriority(av, AVRT_PRIORITY_HIGH);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    auto& gpu = Gpu::get();
    double lastTarget = 0, lastPresent = 0;
    while (running_) {
        if (resizePending_) { resizeBuffers(); justResized_ = true; }
        bool useVrr = vrr && gpu.tearingSupported;
        double hz = displayHz(), period = 1.0 / std::max(hz, 1.0);
        // At high refresh rates allow one more queued frame (≤ 1 refresh of extra delay, e.g. 3.6 ms
        // at 280 Hz): otherwise a GPU busy generating frames often isn't done with the previous
        // present within the short window and the refresh is skipped.
        if (UINT want = hz >= 119 ? 2u : 1u; want != frameLatency_) { swap_->SetMaximumFrameLatency(want); frameLatency_ = want; }
        double tw0 = hostNow();
        if (!timedMode_ && getenv("FG_TIMED")) { timedMode_ = true; timed = true; }   // test aid
        if (!useVrr && !timedMode_) {
            // Wake on every vblank (no GPU work), but draw and present only when the picture
            // changes: presenting every refresh of a 240–280 Hz panel makes the GPU switch to the
            // compositor that often, which more than doubles the cost of frame generation.
            if (!output_ || tw0 - outputAt_ > 1.0) { output_.Reset(); swap_->GetContainingOutput(&output_); outputAt_ = tw0; }
            if (!output_ || FAILED(output_->WaitForVBlank())) Sleep(4);
            double t = hostNow();
            // A vblank wait that returns at once must never turn this loop into a busy spin.
            if (t - lastWake_ < 0.4 * period) { preciseSleepUntil(lastWake_ + period); t = hostNow(); }
            // Health check: refresh signals per second should match the display's rate. With
            // G-SYNC/FreeSync active the panel follows the app instead, and waiting for its
            // refresh loses frames — after 2 bad seconds present on a steady timer.
            vblankCount_++;
            if (vblankWindowAt_ == 0) vblankWindowAt_ = t;
            if (t - vblankWindowAt_ >= 1.0) {
                double rate = vblankCount_ / (t - vblankWindowAt_);
                badSeconds_ = (rate < 0.8 * hz || rate > 1.3 * hz) ? badSeconds_ + 1 : 0;
                if (badSeconds_ >= 2) {
                    timedMode_ = true; timed = true;
                    logf("[present] the display's refresh signal is irregular (%.0f per second, display %.0f Hz — G-SYNC/FreeSync?): presenting on a steady timer instead", rate, hz);
                }
                vblankCount_ = 0; vblankWindowAt_ = t;
            }
        } else if (!useVrr) {
            // Timed mode: wake once per refresh period on a steady timeline.
            double next = lastWake_ + period;
            if (next < tw0 - period) next = tw0;   // catch up after a stall instead of bursting
            preciseSleepUntil(next);
        }
        if (!useVrr) lastWake_ = hostNow();
        double tw1 = hostNow();
        double now = hostNow();

        // Target presentation time: the next refresh, kept on a regular timeline (like a display
        // link) so frame selection doesn't jitter with wake-up times.
        double target = now + period;
        if (lastTarget > 0 && std::abs(target - (lastTarget + period)) < period * 0.5) target = lastTarget + period;
        if (useVrr) {
            // VRR: the display follows us. Sleep until the next frame is due (re-present at ≥ 40 Hz,
            // never faster than the display allows), checking at least every 2 ms for new frames —
            // never spin.
            double minGap = 1.0 / std::max(1.0, maxHz > 0 ? maxHz.load() : hz);
            double next = pipeline_.output.nextTime();
            double wake = lastPresent + 0.025;
            if (next >= 0) wake = std::min(wake, next + double(delay));
            wake = std::max(wake, lastPresent + minGap);
            wake = std::min(wake, now + 0.002);
            if (wake > now) preciseSleepUntil(wake);
            now = hostNow(); target = now + 0.001;
        }
        lastTarget = target;
        double lead = std::max(0.0, target - now);
        double wanted = pipeline_.requiredDelay() + lead + extraDelay;
        // Rise fast (avoid stutter), fall slowly (avoid oscillation) — ~2 %/refresh at 120 Hz.
        double d = delay;
        double fall = std::min(0.2, 0.02 * period * 120);
        d = wanted > d ? wanted : d + (wanted - d) * fall;
        delay = d;
        stats_.pipelineDelayMs = d * 1000;

        // A frame leaves the queue only once it's on screen (commit below): if this refresh is
        // skipped, it's shown at the next one instead of being lost.
        // Every frame is shown in order, even a few ms late (a generated frame that arrives with
        // the next one would otherwise be skipped — a visible stutter, ~56 instead of 60 fps).
        // The display refreshes far more often than frames arrive, so it catches up within a
        // refresh or two; skip only when hopelessly behind.
        static const double maxBehind = getenv("FG_MAX_BEHIND") ? atof(getenv("FG_MAX_BEHIND")) : 0.1;
        int due = 0;
        auto [shown, real] = pipeline_.output.peek(target - d, std::max(maxBehind, 1.5 * period), period, &due);
        // A frame behind for more than ¼ s (two frames due at once, every time — frames arrive as
        // fast as the display refreshes, so it can't catch up by itself): skip ahead once instead
        // of staying a frame late. A one-off late frame is still shown.
        if (shown && shown != lastShown_ && due >= 2) {
            if (behindSince_ == 0) behindSince_ = now;
            else if (now - behindSince_ > 0.25) {
                std::tie(shown, real) = pipeline_.output.peek(target - d, 0.5 * period, period);
                behindSince_ = 0;
            }
        } else if (shown != lastShown_) {
            behindSince_ = 0;
        }
        FramePtr frame = shown;
        loops++;
        if (frame && (!frame->image || frame->image->isNV12() || !frame->image->srv[0])) {   // never present a frame we can't draw
            undrawable++;
            frame = lastDrawable_;
        } else if (frame) {
            lastDrawable_ = frame;
        }
        stats_.recordRefresh(target);
        bool hasShot; { std::lock_guard<std::mutex> l(ovLock_); hasShot = !shotPath_.empty(); }
        int ovv = ovVersion_.load(), sc = scaling.load(); bool cmp = compare.load();
        bool changed = frame != lastShown_ || ovv != lastOvVersion_ || sc != lastScaling_ || cmp != lastCompare_ || hasShot ||
                       justResized_ || now - lastPresent > 0.25;
        // Refresh cap (e.g. 120 Hz on a 240 Hz panel): skip refreshes.
        if (changed && maxHz > 0 && maxHz < hz - 0.5 && now - lastPresent < 1.0 / maxHz - 0.3 * period && !justResized_ && !hasShot) changed = false;
        if (!changed) continue;
        // Only draw when a back buffer is free (frame-latency waitable signalled). Drawing into a
        // buffer still queued for display stalls the GPU queue — and with it frame generation,
        // capture and the UI. If none is free yet, try again at the next refresh.
        // Wait briefly (the previous frame is usually released right at this vblank) rather than
        // skipping the refresh — skipping halved the frame rate when output = refresh rate.
        if (!useVrr && WaitForSingleObjectEx(waitable_, DWORD(std::clamp(period * 700.0, 1.0, 8.0)), FALSE) != WAIT_OBJECT_0) { notReady++; continue; }
        if (useVrr) WaitForSingleObjectEx(waitable_, 50, FALSE);
        if (shown && shown != lastShown_) {
            passedOver += pipeline_.output.commit(shown);
            if (shown != lastShown_ && shown->time + d < target - 1.5 * period) shownLate++;
            if (getenv("FG_TRACE_PRESENT") && shown != lastShown_) { static int n; if (n++ % 15 == 0) logf("[trace] show: due %.1f ms ago (content %.3f, now %.3f, target %.3f, delay %.1f ms, next queued %.3f)", (target - d - shown->time) * 1000, shown->time, now, target, d * 1000, pipeline_.output.nextTime()); }
        }
        lastOvVersion_ = ovv; lastScaling_ = sc; lastCompare_ = cmp; justResized_ = false;
        if (frame && frame != lastShown_) { stats_.recordPresent(target); lastShown_ = frame; }

        Overlay ov[2]; std::string shot;
        { std::lock_guard<std::mutex> l(ovLock_); ov[0] = overlays_[0]; ov[1] = overlays_[1]; shot.swap(shotPath_); }
        double tl0 = hostNow();
        {
            auto l = gpu.lock();
            double tl1 = hostNow();
            prof_[1] += tl1 - tl0;
            ID3D11RenderTargetView* rtv = rtv_.Get();
            gpu.ctx->OMSetRenderTargets(1, &rtv, nullptr);
            float black[4] = {0, 0, 0, 1};
            gpu.ctx->ClearRenderTargetView(rtv, black);
            D3D11_VIEWPORT vp{0, 0, float(bw_), float(bh_), 0, 1};
            gpu.ctx->RSSetViewports(1, &vp);
            gpu.ctx->IASetInputLayout(nullptr);
            gpu.ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
            gpu.ctx->VSSetShader(gpu.vs(kQuad, "vsQuad"), nullptr, 0);
            ID3D11SamplerState* samp = gpu.linearClamp();
            gpu.ctx->PSSetSamplers(0, 1, &samp);
            if (frame) {
                if (compare && real && real->image && !real->image->isNV12()) {
                    // Left half: real frames only. Right half: with frame generation.
                    D3D11_RECT left{0, 0, bw_ / 2, bh_}, right{bw_ / 2, 0, bw_, bh_};
                    drawFrame(*real->image, bw_, bh_, &left);
                    drawFrame(*frame->image, bw_, bh_, &right);
                } else {
                    drawFrame(*frame->image, bw_, bh_, nullptr);
                }
            }
            gpu.ctx->RSSetState(rs_.Get());
            for (auto& o : ov) {
                if (!o.visible || !o.image) continue;
                float x0 = 2.f * o.x / bw_ - 1, y0 = 1 - 2.f * o.y / bh_;
                float x1 = 2.f * (o.x + o.image->w) / bw_ - 1, y1 = 1 - 2.f * (o.y + o.image->h) / bh_;
                QuadCB c{{x0, y0, x1, y1}, o.alpha, {}};
                gpu.setCB(0, &c, sizeof c, true);
                ID3D11SamplerState* pt = gpu.pointClamp();
                gpu.ctx->PSSetSamplers(0, 1, &pt);
                ID3D11ShaderResourceView* srv = o.image->srv[0].Get();
                gpu.ctx->PSSetShaderResources(0, 1, &srv);
                gpu.ctx->PSSetShader(gpu.ps(kQuad, "psOverlay"), nullptr, 0);
                gpu.ctx->OMSetBlendState(premul_.Get(), nullptr, 0xffffffff);
                gpu.ctx->Draw(4, 0);
            }
            ID3D11ShaderResourceView* n = nullptr;
            gpu.ctx->PSSetShaderResources(0, 1, &n);
            gpu.ctx->OMSetBlendState(nullptr, nullptr, 0xffffffff);
            if (!shot.empty()) {
                ComPtr<ID3D11Texture2D> bb; swap_->GetBuffer(0, IID_PPV_ARGS(&bb));
                auto copy = gpu.makeTexture(bw_, bh_, DXGI_FORMAT_B8G8R8A8_UNORM, D3D11_BIND_SHADER_RESOURCE);
                if (copy) {
                    gpu.ctx->CopyResource(copy->tex.Get(), bb.Get());
                    std::thread([copy, shot] { if (Gpu::get().savePNG(*copy, shot)) logf("[ui-shot] %s", shot.c_str()); }).detach();
                }
            }
            gpu.ctx->OMSetRenderTargets(0, nullptr, nullptr);
        }
        // Present outside the GPU lock: it can block until a refresh slot is free, and holding the
        // lock meanwhile would stall capture and frame generation (D3D's own multithread
        // protection still serialises the call itself).
        double tp0 = hostNow();
        HRESULT hr = useVrr ? swap_->Present(0, DXGI_PRESENT_ALLOW_TEARING) : swap_->Present(0, 0);   // after our own vblank wait: shown at the next refresh, never blocks
        double tp1 = hostNow();
        prof_[0] += tw1 - tw0; prof_[2] += tp1 - tp0; prof_[3] += 1;
        if (g_opt.log && tp1 - profAt_ > 1.0) {
            logf("[present] per frame: wait %.2f ms, lock %.2f ms, present %.2f ms (%d frames)", prof_[0] / prof_[3] * 1000, prof_[1] / prof_[3] * 1000, prof_[2] / prof_[3] * 1000, int(prof_[3]));
            prof_[0] = prof_[1] = prof_[2] = prof_[3] = 0; profAt_ = tp1;
        }
        if (hr == DXGI_STATUS_OCCLUDED) Sleep(10);
        else if (FAILED(hr)) {
            if (Gpu::isDeviceLost(hr)) { gpu.reportDeviceLost("present", hr); Sleep(100); }
            else { logf("[present] Present failed: %s", hr2str(hr).c_str()); Sleep(50); }
        }
        lastPresent = hostNow();
    }
    if (av) AvRevertMmThreadCharacteristics(av);
    CoUninitialize();
}
