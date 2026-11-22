#include "Hud.h"
#include <algorithm>

static const char* kHudCommon = R"(
Texture2D<uint> P : register(t4);       // persist
Texture2D<float> BLK : register(t5);    // UI blocks
cbuffer C : register(b0) { float threshold; float edge; float minFrames; float t; float strength; int radius; int extent; float pad; };
uint stillCount(uint v) { return v & 255u; }
uint edgeCount(uint v) { return (v >> 8) & 255u; }
bool movingNow(uint v) { return ((v >> 16) & 1u) != 0; }

// Is this position part of on-screen UI? Either a UI edge within ±2 px (covers anti-aliasing),
// or a pixel that has itself been still just as long and sits next to UI edges (solid icons, bold glyphs).
bool nearUI(int2 pc) {
    uint W, H; P.GetDimensions(W, H);
    int2 c = clamp(pc, int2(0, 0), int2(W - 1, H - 1));
    uint v = P[c];
    if (movingNow(v)) return false;   // moving right now → the engine handles it
    uint BW, BH; BLK.GetDimensions(BW, BH);
    int2 bc = c / 8;
    bool any = false;
    [unroll] for (int dy = -1; dy <= 1; dy++) [unroll] for (int dx = -1; dx <= 1; dx++)
        if (BLK[clamp(bc + int2(dx, dy), int2(0, 0), int2(BW - 1, BH - 1))] > 0.5) any = true;
    if (!any) return false;
    if (float(stillCount(v)) >= minFrames) return true;
    for (int y = -2; y <= extent; y++) for (int x = -2; x <= extent; x++)
        if (float(edgeCount(P[clamp(pc + int2(x, y), int2(0, 0), int2(W - 1, H - 1))])) >= minFrames) return true;
    return false;
}
bool longStill(int2 pc) {
    uint W, H; P.GetDimensions(W, H);
    return float(stillCount(P[clamp(pc, int2(0, 0), int2(W - 1, H - 1))])) >= minFrames;
}
)";

static const std::string kHudUpdate = std::string(R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
RWTexture2D<uint> PW : register(u0);
cbuffer C : register(b0) { float threshold; float edge; float minFrames; float t; float strength; int radius; int extent; float pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint W, H; PW.GetDimensions(W, H); if (id.x >= W || id.y >= H) return;
    float4 pa = A[id.xy], pb = B[id.xy];
    float3 d = abs(pa.rgb - pb.rgb);
    float m = max(d.r, max(d.g, d.b));            // colour, not just brightness
    bool still = m < threshold;
    // Edge strength: largest luma step to any of the 4 neighbours (catches both sides of a stroke).
    float3 w3 = float3(0.299, 0.587, 0.114);
    float lc = dot(pb.rgb, w3), g = 0;
    int2 nb[4] = { int2(1, 0), int2(-1, 0), int2(0, 1), int2(0, -1) };
    [unroll] for (int k = 0; k < 4; k++) {
        int2 q = clamp(int2(id.xy) + nb[k], int2(0, 0), int2(W - 1, H - 1));
        g = max(g, abs(dot(B[q].rgb, w3) - lc));
    }
    // Leaky counters: +1 per still frame, −6 per changed frame, capped at 40 (capture-card noise
    // flickers a few % of UI edge pixels each frame; real motion still clears a pixel in ~4 frames).
    uint v = PW[id.xy];
    int r = int(v & 255u), e = int((v >> 8) & 255u);
    r = still ? min(r + 1, 40) : max(r - 6, 0);
    e = (still && g > edge) ? min(e + 1, 40) : max(e - 6, 0);
    // Changed a lot in this pair → real movement: straight back to the engine (map starts panning).
    uint moving = m > 0.25 ? 1u : 0u;
    PW[id.xy] = uint(r) | (uint(e) << 8) | (moving << 16);
}
)");

static const std::string kHudBlocks = std::string(R"(
Texture2D<uint> P : register(t0);
RWTexture2D<unorm float> O : register(u0);
cbuffer C : register(b0) { float threshold; float edge; float minFrames; float t; float strength; int radius; int extent; float pad; };
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint BW, BH; O.GetDimensions(BW, BH); if (id.x >= BW || id.y >= BH) return;
    uint W, H; P.GetDimensions(W, H);
    float hit = 0;
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        uint2 q = uint2(min(id.x * 8 + x, W - 1), min(id.y * 8 + y, H - 1));
        if (float((P[q] >> 8) & 255u) >= minFrames) hit = 1;
    }
    O[id.xy] = hit;
}
)");

// Engine input with UI hidden: UI pixels → average of nearby non-still pixels.
static const std::string kHudFill = std::string(kHudCommon) + R"(
Texture2D<float4> S : register(t0);
RWTexture2D<unorm float4> O : register(u0);
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint W, H; O.GetDimensions(W, H); if (id.x >= W || id.y >= H) return;
    int2 p = int2(id.xy);
    if (!nearUI(p)) { O[id.xy] = S[id.xy]; return; }
    float4 sum = 0; float n = 0;
    for (int dy = -radius; dy <= radius; dy += 2) for (int dx = -radius; dx <= radius; dx += 2) {
        int2 q = clamp(p + int2(dx, dy), int2(0, 0), int2(W - 1, H - 1));
        if (longStill(q)) continue;
        sum += S[q]; n += 1;
    }
    O[id.xy] = n > 0 ? sum / n : S[id.xy];
}
)";

// After generation: paste the exact UI (from the real frames) back into the generated frame.
static const std::string kHudApply = std::string(kHudCommon) + R"(
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
Texture2D<float4> G : register(t2);
RWTexture2D<unorm float4> O : register(u0);
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint W, H; O.GetDimensions(W, H); if (id.x >= W || id.y >= H) return;
    float4 g = G[id.xy];
    if (!nearUI(int2(id.xy))) { O[id.xy] = g; return; }
    float4 still = lerp(A[id.xy], B[id.xy], t);
    O[id.xy] = lerp(g, still, strength);
}
)";

struct HudParams { float threshold, edge, minFrames, t, strength; int radius, extent; float pad; };

HudStabilizer& HudStabilizer::get() { static HudStabilizer h; return h; }

void HudStabilizer::reset() { std::lock_guard<std::mutex> l(m_); persist_.reset(); blocks_.reset(); }

float HudStabilizer::lockStrength(double motion) {
    float x = float((motion - 1.0) / (3.0 - 1.0));
    float c = std::clamp(x, 0.f, 1.f);
    return c * c * (3 - 2 * c);
}

void HudStabilizer::ensure(int w, int h) {
    auto& gpu = Gpu::get();
    if (!persist_ || persist_->w != w || persist_->h != h) {
        persist_ = gpu.makeTexture(w, h, DXGI_FORMAT_R32_UINT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
        if (persist_) { auto l = gpu.lock(); UINT zero[4] = {}; gpu.ctx->ClearUnorderedAccessViewUint(persist_->uav.Get(), zero); }   // "not still" everywhere
    }
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    if (!blocks_ || blocks_->w != bw || blocks_->h != bh)
        blocks_ = gpu.makeTexture(bw, bh, DXGI_FORMAT_R8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
}

std::pair<ImagePtr, ImagePtr> HudStabilizer::prepare(const ImagePtr& prev, const ImagePtr& next, double motion) {
    auto& gpu = Gpu::get();
    std::lock_guard<std::mutex> g(m_);
    ensure(next->w, next->h);
    if (!persist_ || !blocks_) return {prev, next};
    HudParams p{kLockThreshold, kOverlayEdge, kOverlayMinFrames, 0, 0, 7, 2, 0};
    std::pair<ImagePtr, ImagePtr> result{prev, next};
    bool hide = lockStrength(motion) > 0;
    ImagePtr pf, nf;
    if (hide) { pf = pool_.acquire(prev->w, prev->h, DXGI_FORMAT_R8G8B8A8_UNORM); nf = pool_.acquire(next->w, next->h, DXGI_FORMAT_R8G8B8A8_UNORM); }
    {
        auto l = gpu.lock();
        gpu.dispatch(gpu.cs(kHudUpdate.c_str()), {prev->srv[0].Get(), next->srv[0].Get()}, {persist_->uav.Get()}, &p, sizeof p, next->w, next->h);
        gpu.dispatch(gpu.cs(kHudBlocks.c_str()), {persist_->srv[0].Get()}, {blocks_->uav.Get()}, &p, sizeof p, blocks_->w, blocks_->h);
        if (hide && pf && nf) {
            for (auto [src, dst] : {std::make_pair(prev, pf), std::make_pair(next, nf)})
                gpu.dispatch(gpu.cs(kHudFill.c_str()), {src->srv[0].Get(), nullptr, nullptr, nullptr, persist_->srv[0].Get(), blocks_->srv[0].Get()},
                             {dst->uav.Get()}, &p, sizeof p, dst->w, dst->h);
            result = {pf, nf};
        }
    }
    if (wantSnapshot_.exchange(false)) {
        std::vector<uint8_t> px;
        ComPtr<ID3D11Texture2D> stage;
        D3D11_TEXTURE2D_DESC td; blocks_->tex->GetDesc(&td);
        td.BindFlags = 0; td.Usage = D3D11_USAGE_STAGING; td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (SUCCEEDED(gpu.dev->CreateTexture2D(&td, nullptr, &stage))) {
            { auto l = gpu.lock(); gpu.ctx->CopyResource(stage.Get(), blocks_->tex.Get()); }
            gpu.waitIdle();
            auto l = gpu.lock();
            D3D11_MAPPED_SUBRESOURCE m;
            if (SUCCEEDED(gpu.ctx->Map(stage.Get(), 0, D3D11_MAP_READ, 0, &m))) {
                snap_.w = blocks_->w; snap_.h = blocks_->h; snap_.data.resize(size_t(snap_.w) * snap_.h);
                for (int y = 0; y < snap_.h; y++) memcpy(&snap_.data[size_t(y) * snap_.w], (uint8_t*)m.pData + size_t(y) * m.RowPitch, snap_.w);
                gpu.ctx->Unmap(stage.Get(), 0);
            }
        }
    }
    return result;
}

void HudStabilizer::composite(std::vector<ImagePtr>& outs, const ImagePtr& prev, const ImagePtr& next, const std::vector<float>& phases, double motion) {
    float strength = lockStrength(motion);
    if (strength <= 0) return;
    auto& gpu = Gpu::get();
    std::lock_guard<std::mutex> g(m_);
    if (!persist_ || !blocks_ || persist_->w != next->w || persist_->h != next->h) return;
    auto l = gpu.lock();
    for (size_t i = 0; i < outs.size() && i < phases.size(); i++) {
        auto dst = pool_.acquire(outs[i]->w, outs[i]->h, DXGI_FORMAT_R8G8B8A8_UNORM);
        if (!dst) continue;
        HudParams p{kLockThreshold, kOverlayEdge, kOverlayMinFrames, phases[i], strength, 7, 2, 0};
        gpu.dispatch(gpu.cs(kHudApply.c_str()), {prev->srv[0].Get(), next->srv[0].Get(), outs[i]->srv[0].Get(), nullptr, persist_->srv[0].Get(), blocks_->srv[0].Get()},
                     {dst->uav.Get()}, &p, sizeof p, dst->w, dst->h);
        outs[i] = dst;
    }
}

HudStabilizer::Snapshot HudStabilizer::takeSnapshot() { std::lock_guard<std::mutex> l(m_); Snapshot s = std::move(snap_); snap_ = {}; return s; }
