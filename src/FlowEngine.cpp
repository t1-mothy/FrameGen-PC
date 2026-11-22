#include "FlowEngine.h"
#include "Hud.h"
#include <algorithm>
#include <cmath>

static const char* kLuma = R"(
Texture2D<float4> T : register(t0);
RWTexture2D<unorm float4> O : register(u0);
// Matching feature: luma + chroma (luma alone can't tell a red ball from green grass).
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    float3 c = T[id.xy].rgb;
    float y = dot(c, float3(0.299, 0.587, 0.114));
    O[id.xy] = float4(y, (c.b - y) * 0.564 + 0.5, (c.r - y) * 0.713 + 0.5, 0);   // stored unsigned (10-bit)
}
)";

static const char* kDown = R"(
Texture2D<float4> T : register(t0);
RWTexture2D<unorm float4> O : register(u0);
[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint w, h; O.GetDimensions(w, h); if (id.x >= w || id.y >= h) return;
    uint sw, sh; T.GetDimensions(sw, sh);
    uint2 p = id.xy * 2, q = min(p + 1, uint2(sw - 1, sh - 1));
    O[id.xy] = 0.25 * (T[p] + T[uint2(q.x, p.y)] + T[uint2(p.x, q.y)] + T[q]);
}
)";

// Symmetric block motion search at time t. Candidates are evaluated in parallel: one thread
// group handles two 8×8 blocks of this pyramid level, 16 threads per block.
//   fullSearch: every vector within ±8 px (coarsest level, 1/32 scale at 1080p = ±256 px; 17×17 threads per block)
//   mode 0: candidates from the coarser level (×2), zero and the previous pair, then refinement
//   mode 1: propagation — the block's own and its 8 neighbours' vectors, smoothness-weighted
// WIN / STEP (matching window half-size and sample step) are compile-time constants so the
// sampling loops unroll.
static const char* kMEBody = R"(
Texture2D<float4> LA : register(t0);
Texture2D<float4> LB : register(t1);
Texture2D<float2> COARSE : register(t2);
Texture2D<float2> CUR : register(t3);
Texture2D<float2> HIST : register(t4);
Texture2D<float2> GLOB : register(t5);   // dominant motions of the frame (full-res pixels)
RWTexture2D<float2> OUT : register(u0);
SamplerState S : register(s0);
cbuffer C : register(b0) {
    float t; float lambda; int mode; int hasCoarse;
    float2 invSize; int win_; int step_;
    float refine; int hasHist; float maxVec; float zeroBias;
    int range; float mu; float muCap; int hasFull;
    int hasGlob; float globScale; float2 pad2;
};

float cost(float2 c, float2 v) {
    float2 pa = (c - t * v + 0.5) * invSize, pb = (c + (1 - t) * v + 0.5) * invSize;
    float s = 0;
    [unroll] for (int y = -WIN; y < WIN; y += STEP)
        [unroll] for (int x = -WIN; x < WIN; x += STEP) {
            float2 o = float2(x, y) * invSize;
            s += dot(abs(LA.SampleLevel(S, pa + o, 0).xyz - LB.SampleLevel(S, pb + o, 0).xyz), float3(1, 1.5, 1.5));
        }
    return s / float((2 * WIN / STEP) * (2 * WIN / STEP));
}

float2 clampVec(float2 v) { float m = length(v); return m > maxVec ? v * (maxVec / m) : v; }
float score(float2 c, float2 v, float2 pred) {
    // Smoothness (capped, so real motion boundaries survive) + a small length penalty: on flat
    // areas every vector "matches", and without it random vectors make flat-coloured objects vanish.
    float e = cost(c, v) + lambda * min(abs(v.x - pred.x) + abs(v.y - pred.y), 6.0) + mu * min(length(v), muCap);
    return all(v == 0) ? e - zeroBias : e;
}

groupshared float gsCost[2][16];
groupshared float2 gsVec[2][16];
groupshared float2 gsBest[2];
groupshared float gsBestCost[2];

static const int2 kDirs[8] = { int2(-1, -1), int2(0, -1), int2(1, -1), int2(-1, 0), int2(1, 0), int2(-1, 1), int2(0, 1), int2(1, 1) };

[numthreads(16, 2, 1)] void main(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID) {
    uint bw, bh; OUT.GetDimensions(bw, bh);
    int2 b = int2(gid.x * 2 + tid.y, gid.y);
    int2 m = int2(bw - 1, bh - 1);
    bool valid = b.x <= m.x && b.y <= m.y;
    b = min(b, m);
    float2 c = float2(b) * 8 + 4;
    uint i = tid.x, s = tid.y;
    float2 pred = 0, cand = 0;
    bool use = false;
    if (mode == 0) {
        if (hasCoarse) {
            uint cw, ch; COARSE.GetDimensions(cw, ch);
            int2 cb = b / 2, cm = int2(cw - 1, ch - 1);
            pred = COARSE[clamp(cb, int2(0, 0), cm)] * 2;
            if (i < 9) { cand = COARSE[clamp(cb + int2(int(i % 3) - 1, int(i / 3) - 1), int2(0, 0), cm)] * 2; use = true; }
        }
        if (i == 9) { cand = 0; use = true; }
        if (i == 10 && hasHist) { cand = HIST[b]; use = true; }
        // This level's own exhaustive search (coarse levels): finds motion of thin bands
        // (a horizon behind a still car) that the coarser level averaged away.
        if (i == 11 && hasFull) { cand = CUR[b]; use = true; }
        // The frame's dominant motions (camera pan / turn), offered to every block.
        if (i >= 12 && hasGlob) { cand = GLOB[uint2(i - 12, 0)] * globScale; use = true; }
    } else {
        float2 l = CUR[clamp(b + int2(-1, 0), int2(0, 0), m)], r = CUR[clamp(b + int2(1, 0), int2(0, 0), m)];
        float2 u = CUR[clamp(b + int2(0, -1), int2(0, 0), m)], d = CUR[clamp(b + int2(0, 1), int2(0, 0), m)];
        pred = 0.25 * (l + r + u + d);
        if (i < 9) { cand = CUR[clamp(b + int2(int(i % 3) - 1, int(i / 3) - 1), int2(0, 0), m)]; use = true; }
    }
    cand = clampVec(cand);
    gsCost[s][i] = use ? score(c, cand, pred) : 1e9;
    gsVec[s][i] = cand;
    GroupMemoryBarrierWithGroupSync();
    if (i == 0) {
        float bc = gsCost[s][0]; float2 bv = gsVec[s][0];
        for (int k = 1; k < 16; k++) if (gsCost[s][k] < bc) { bc = gsCost[s][k]; bv = gsVec[s][k]; }
        gsBest[s] = bv; gsBestCost[s] = bc;
    }
    GroupMemoryBarrierWithGroupSync();
    float rad = mode == 1 ? refine * 0.5 : refine;
    [unroll] for (int it = 0; it < 2; it++) {
        float2 base = gsBest[s];
        cand = clampVec(base + float2(kDirs[i & 7]) * rad);
        gsCost[s][i] = i < 8 ? score(c, cand, pred) : 1e9;
        gsVec[s][i] = cand;
        GroupMemoryBarrierWithGroupSync();
        if (i == 0) {
            float bc = gsBestCost[s]; float2 bv = gsBest[s];
            for (int k = 0; k < 8; k++) if (gsCost[s][k] < bc) { bc = gsCost[s][k]; bv = gsVec[s][k]; }
            gsBest[s] = bv; gsBestCost[s] = bc;
        }
        GroupMemoryBarrierWithGroupSync();
        rad *= 0.5;
    }
    if (i == 0 && valid) OUT[b] = gsBest[s];
}

groupshared float fsCost[289];
[numthreads(17, 17, 1)] void fullSearch(uint3 gid : SV_GroupID, uint3 tid : SV_GroupThreadID, uint gi : SV_GroupIndex) {
    float2 c = float2(gid.xy) * 8 + 4;
    float2 v = float2(int2(tid.xy) - 8);
    fsCost[gi] = score(c, v, 0);
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        uint bw, bh; OUT.GetDimensions(bw, bh);
        uint best = 144; float bc = fsCost[144];   // zero vector first
        for (uint k = 0; k < 289; k++) if (fsCost[k] < bc) { bc = fsCost[k]; best = k; }
        if (gid.x < bw && gid.y < bh) OUT[gid.xy] = float2(int(best % 17) - 8, int(best / 17) - 8);
    }
}
)";
// The frame's 4 dominant motions (vote: vectors within 1 px of each other), from one level's
// block field; written in full-res pixels.
static const char* kGlobal = R"(
Texture2D<float2> F : register(t0);
RWTexture2D<float2> G : register(u0);
cbuffer C : register(b0) { float scale; float3 pad; };
groupshared float2 vs[1024];
groupshared uint cnt[1024];
[numthreads(256, 1, 1)] void main(uint gi : SV_GroupIndex) {
    uint w, h; F.GetDimensions(w, h);
    uint total = w * h, stride = (total + 1023) / 1024, n = min(total, 1024u);
    for (uint k = gi; k < n; k += 256) { uint idx = min(k * stride, total - 1); vs[k] = F[uint2(idx % w, idx / w)]; }
    GroupMemoryBarrierWithGroupSync();
    for (uint k2 = gi; k2 < n; k2 += 256) {
        uint c = 0; float2 v = vs[k2];
        for (uint j = 0; j < n; j++) { float2 d = abs(vs[j] - v); c += (d.x <= 1 && d.y <= 1) ? 1 : 0; }
        cnt[k2] = c;
    }
    GroupMemoryBarrierWithGroupSync();
    if (gi == 0) {
        float2 chosen[4] = { float2(0, 0), float2(0, 0), float2(0, 0), float2(0, 0) };
        uint got = 0;
        for (uint r = 0; r < 4; r++) {
            uint best = 0; int bi = -1;
            for (uint k3 = 0; k3 < n; k3++) {
                bool near = false;
                for (uint q = 0; q < got; q++) { float2 d = abs(vs[k3] - chosen[q]); near = near || (d.x <= 2 && d.y <= 2); }
                if (!near && cnt[k3] > best) { best = cnt[k3]; bi = int(k3); }
            }
            if (bi < 0) break;
            chosen[got++] = vs[bi];
        }
        for (uint o = 0; o < 4; o++) G[uint2(o, 0)] = (o < got ? chosen[o] : chosen[0]) * scale;
    }
}
)";

static const std::string kME_W4S1 = std::string("#define WIN 4\n#define STEP 1\n") + kMEBody;
static const std::string kME_W6S1 = std::string("#define WIN 6\n#define STEP 1\n") + kMEBody;
static const std::string kME_W6S2 = std::string("#define WIN 6\n#define STEP 2\n") + kMEBody;

// Per-pixel motion-compensated interpolation.
static const char* kInterpBody = R"(
#define MUCAP 24.0
Texture2D<float4> A : register(t0);
Texture2D<float4> B : register(t1);
Texture2D<float4> LA : register(t2);
Texture2D<float4> LB : register(t3);
Texture2D<float2> F : register(t4);
RWTexture2D<unorm float4> O : register(u0);
SamplerState S : register(s0);
cbuffer C : register(b0) { float t; float scale; float2 invSize; float lo; float hi; float distBias; float muPx; float sigma; float3 pad; };

float patch(float2 pc, float2 v) {
    float2 pa = pc - t * v, pb = pc + (1 - t) * v;
    float s = 0;
    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
        float2 o = float2(x, y) * 1.5;
        s += dot(abs(LA.SampleLevel(S, (pa + o) * invSize, 0).xyz - LB.SampleLevel(S, (pb + o) * invSize, 0).xyz), float3(1, 1.5, 1.5));
    }
    return s / 9;
}

// Colour along vector v, with the occlusion rule: where the two frames disagree, use the one that
// actually shows this pixel (converging motion = covered → earlier frame; diverging → later).
float3 along(float2 pc, float2 v, float side, uint W, uint H) {
    float2 pa = pc - t * v, pb = pc + (1 - t) * v;
    float4 cA = A.SampleLevel(S, pa * invSize, 0), cB = B.SampleLevel(S, pb * invSize, 0);
    float3 c = lerp(cA.rgb, cB.rgb, t);
    float3 dd = abs(cA.rgb - cB.rgb);
    float w = saturate((max(dd.r, max(dd.g, dd.b)) - lo) / (hi - lo));
    if (w > 0 && side >= 0) c = lerp(c, side < 0.5 ? cA.rgb : cB.rgb, w);
    bool inA = all(pa >= 0) && all(pa <= float2(W, H)), inB = all(pb >= 0) && all(pb <= float2(W, H));
    if (inA && !inB) c = cA.rgb; else if (inB && !inA) c = cB.rgb;
    return c;
}

[numthreads(16, 16, 1)] void main(uint3 id : SV_DispatchThreadID) {
    uint W, H; O.GetDimensions(W, H); if (id.x >= W || id.y >= H) return;
    uint fw, fh; F.GetDimensions(fw, fh);
    int2 fm = int2(fw - 1, fh - 1);
    float2 pc = float2(id.xy) + 0.5;
    float blockPx = 8 * scale;
    float2 fb = pc / blockPx - 0.5;              // position in block-centre units
    int2 nb = clamp(int2(round(fb)), int2(0, 0), fm);
    // Which frame shows hidden / revealed background here (motion-field divergence).
    float div = 0, strongest = 0;
    [unroll] for (int y = -1; y <= 1; y++) [unroll] for (int x = -1; x <= 1; x++) {
        int2 q = clamp(nb + int2(x, y), int2(0, 0), fm);
        float d = (F[clamp(q + int2(1, 0), int2(0, 0), fm)].x - F[clamp(q - int2(1, 0), int2(0, 0), fm)].x +
                   F[clamp(q + int2(0, 1), int2(0, 0), fm)].y - F[clamp(q - int2(0, 1), int2(0, 0), fm)].y) * 0.5 * scale;
        if (abs(d) > strongest) { strongest = abs(d); div = d; }
    }
    float side = div < -1 ? 0 : (div > 1 ? 1 : (t < 0.5 ? 0 : (t > 0.5 ? 1 : -1)));
    // Candidates: the smoothly interpolated vector and the nearest (2R+1)² block vectors — wide
    // enough to reach a fast object's vector from the flat background around it.
    const int N = 1 + (2 * R + 1) * (2 * R + 1);
    float2 cv[N]; float ce[N]; float cw[N];
    cv[0] = F.SampleLevel(S, (fb + 0.5) / float2(fw, fh), 0) * scale;
    ce[0] = patch(pc, cv[0]) + muPx * min(length(cv[0]), MUCAP);
    cw[0] = 1;
    float emin = ce[0];
    int k = 1;
    [unroll] for (int y2 = -R; y2 <= R; y2++) [unroll] for (int x2 = -R; x2 <= R; x2++) {
        int2 q = clamp(nb + int2(x2, y2), int2(0, 0), fm);
        float2 v = F[q] * scale;
        float2 d = abs(fb - float2(q));
        cv[k] = v;
        ce[k] = patch(pc, v) + distBias * length(d) + muPx * min(length(v), MUCAP);
        // Overlapped-block weighting: a block's vector fades out towards its neighbours, so
        // neighbouring blocks with different vectors blend instead of meeting at a hard edge.
        cw[k] = 0.15 + saturate(1 - d.x) * saturate(1 - d.y);
        emin = min(emin, ce[k]);
        k++;
    }
    float3 c = 0; float wsum = 0;
    if (sigma <= 0) {   // hard choice (old behaviour)
        int best = 0; [unroll] for (int i = 1; i < N; i++) if (ce[i] < ce[best]) best = i;
        c = along(pc, cv[best], side, W, H); wsum = 1;
    } else {
        // Soft choice: every candidate that matches about as well as the best contributes.
        [unroll] for (int i = 0; i < N; i++) {
            float w = cw[i] * exp(-(ce[i] - emin) / sigma);
            if (w > 0.02) { c += w * along(pc, cv[i], side, W, H); wsum += w; }
        }
    }
    O[id.xy] = float4(c / wsum, 1);
}
)";

static const std::string kInterpR1 = std::string("#define R 1\n") + kInterpBody;
static const std::string kInterpR2 = std::string("#define R 2\n") + kInterpBody;

struct MEParams { float t, lambda; int mode, hasCoarse; float invW, invH; int win, stepPx; float refine; int hasHist; float maxVec, zeroBias; int range; float mu; float muCap; int hasFull; int hasGlob; float globScale, pad2[2]; };
struct InterpParams { float t, scale, invW, invH, lo, hi, distBias, muPx, sigma, pad[3]; };

bool FlowEngine::ensure(int w, int h) {
    if (w == w_ && h == h_ && !levels_.empty()) return true;
    auto& gpu = Gpu::get();
    levels_.clear();
    int lw = w, lh = h;
    for (int l = 0; l < 7; l++) {
        Level lv;
        lv.w = lw; lv.h = lh; lv.bw = (lw + 7) / 8; lv.bh = (lh + 7) / 8;
        UINT bind = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
        lv.lumaA = gpu.makeTexture(lw, lh, DXGI_FORMAT_R10G10B10A2_UNORM, bind);
        lv.lumaB = gpu.makeTexture(lw, lh, DXGI_FORMAT_R10G10B10A2_UNORM, bind);
        for (auto& f : lv.field) f = gpu.makeTexture(lv.bw, lv.bh, DXGI_FORMAT_R16G16_FLOAT, bind);
        lv.history = gpu.makeTexture(lv.bw, lv.bh, DXGI_FORMAT_R16G16_FLOAT, bind);
        if (!lv.lumaA || !lv.lumaB || !lv.field[0] || !lv.field[1] || !lv.history) { levels_.clear(); return false; }
        levels_.push_back(lv);
        if (lw / 2 < 48 || lh / 2 < 27) break;   // coarsest level ≈ 1/32 at 1080p (±8 px search = ±256 px)
        lw = (lw + 1) / 2; lh = (lh + 1) / 2;
    }
    glob_ = gpu.makeTexture(4, 1, DXGI_FORMAT_R32G32_FLOAT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS);
    w_ = w; h_ = h;
    return true;
}

void FlowEngine::buildPyramid(const GpuImage& rgba, bool isA) {
    auto& gpu = Gpu::get();
    auto get = [&](Level& l) { return isA ? l.lumaA : l.lumaB; };
    gpu.dispatch(gpu.cs(kLuma), {rgba.srv[0].Get()}, {get(levels_[0])->uav.Get()}, nullptr, 0, levels_[0].w, levels_[0].h);
    for (size_t l = 1; l < levels_.size(); l++)
        gpu.dispatch(gpu.cs(kDown), {get(levels_[l - 1])->srv[0].Get()}, {get(levels_[l])->uav.Get()}, nullptr, 0, levels_[l].w, levels_[l].h);
}

int FlowEngine::estimate(float t, int finest) {
    auto& gpu = Gpu::get();
    const bool hq = q_ == Quality::High;
    const int coarsest = int(levels_.size()) - 1;
    ImagePtr coarse;
    int cur = 0;
    bool haveGlob = false;
    for (int l = coarsest; l >= finest; l--) {
        Level& lv = levels_[l];
        MEParams p{};
        p.t = t; p.lambda = 0.003f; p.mode = 0; p.hasCoarse = coarse ? 1 : 0;
        p.invW = 1.f / lv.w; p.invH = 1.f / lv.h;
        p.refine = 1; p.hasHist = (l == finest && lv.hasHistory) ? 1 : 0;
        p.maxVec = float(320 >> l) + 8; p.zeroBias = 0.002f; p.range = 8;
        p.mu = muFull_ * float(1 << l);   // per level pixel
        p.muCap = muCapFull_ / float(1 << l);   // saturates: flat areas stay still, real large motion isn't fought
        const std::string& src = l == 0 ? kME_W6S2 : (l == 1 && hq ? kME_W6S1 : kME_W4S1);
        ID3D11ComputeShader* me = gpu.cs(src.c_str());
        UINT gx = UINT(lv.bw + 1) / 2;
        const bool full = l >= coarsest - fullLevels_;   // exhaustive ±8 search at the coarse levels
        if (full) gpu.dispatch(gpu.cs(src.c_str(), "fullSearch"), {lv.lumaA->srv[0].Get(), lv.lumaB->srv[0].Get()}, {lv.field[l == coarsest ? 0 : 1]->uav.Get()},
                               &p, sizeof p, lv.bw * 17, lv.bh * 17, 17, 17);
        if (l != coarsest) {
            ID3D11ShaderResourceView* hs = p.hasHist ? lv.history->srv[0].Get() : nullptr;
            p.hasFull = full ? 1 : 0;
            p.hasGlob = haveGlob ? 1 : 0;
            p.globScale = 1.f / float(1 << l);
            gpu.dispatch(me, {lv.lumaA->srv[0].Get(), lv.lumaB->srv[0].Get(), coarse->srv[0].Get(), full ? lv.field[1]->srv[0].Get() : nullptr, hs,
                              haveGlob ? glob_->srv[0].Get() : nullptr}, {lv.field[0]->uav.Get()},
                         &p, sizeof p, gx * 16, lv.bh * 2, 16, 2);
            p.hasFull = 0; p.hasGlob = 0;
        }
        cur = 0;
        int iters = hq ? 2 : 1;
        for (int it = 0; it < iters; it++) {
            p.mode = 1;
            gpu.dispatch(me, {lv.lumaA->srv[0].Get(), lv.lumaB->srv[0].Get(), nullptr, lv.field[cur]->srv[0].Get(), nullptr},
                         {lv.field[1 - cur]->uav.Get()}, &p, sizeof p, gx * 16, lv.bh * 2, 16, 2);
            cur = 1 - cur;
        }
        coarse = lv.field[cur];
        // After the finer exhaustive level: the frame's dominant motions, for every finer block.
        if (l == coarsest - fullLevels_ && l > finest && glob_ && !getenv("FG_NOGLOB")) {
            struct { float scale, pad[3]; } gp{float(1 << l), {}};
            gpu.dispatch(gpu.cs(kGlobal), {coarse->srv[0].Get()}, {glob_->uav.Get()}, &gp, sizeof gp, 256, 1, 256, 1);
            haveGlob = true;
        }
    }
    Level& f = levels_[finest];
    gpu.ctx->CopyResource(f.history->tex.Get(), f.field[cur]->tex.Get());
    f.hasHistory = true;
    return cur;
}

ImagePtr FlowEngine::motionField(const VideoFrame& prev, const VideoFrame& next, float& scale) {
    auto& gpu = Gpu::get();
    if (!prev.image || !next.image || prev.image->w != next.image->w || prev.image->h != next.image->h) return nullptr;
    if (!ensure(next.image->w, next.image->h)) return nullptr;
    const int finest = std::min(q_ == Quality::Fast ? 1 : 0, int(levels_.size()) - 1);
    scale = float(1 << finest);
    auto l = gpu.lock();
    buildPyramid(*prev.image, true);
    buildPyramid(*next.image, false);
    int f = estimate(1.0f, finest);   // t = 1: A(p − v) matches B(p) → v is the motion arriving at p
    Level& lv = levels_[finest];
    auto out = fieldPool_.acquire(lv.bw, lv.bh, DXGI_FORMAT_R16G16_FLOAT);
    if (out) gpu.ctx->CopyResource(out->tex.Get(), lv.field[f]->tex.Get());
    return out;
}

bool FlowEngine::interpolate(const VideoFrame& prev, const VideoFrame& next, const std::vector<float>& phases,
                             std::vector<ImagePtr>& out, std::string& error) {
    auto& gpu = Gpu::get();
    ImagePtr A = prev.image, B = next.image;
    if (!A || !B || A->w != B->w || A->h != B->h || A->isNV12() || B->isNV12()) { error = "GPU flow: frames must be RGBA and the same size"; return false; }
    if (!ensure(B->w, B->h)) { error = "GPU flow: out of video memory"; return false; }
    // Hide on-screen UI from the motion search (it would smear it); it's pasted back afterwards.
    bool hud = g_hudLock && !next.forWarmup;
    if (hud) std::tie(A, B) = HudStabilizer::get().prepare(prev.image, next.image, next.motion);
    const int finest = std::min(q_ == Quality::Fast ? 1 : 0, int(levels_.size()) - 1);
    const float scale = float(1 << finest);
    {
        auto l = gpu.lock();
        GpuProfiler::get().mark("hud-prepare");
        buildPyramid(*A, true);
        buildPyramid(*B, false);
        GpuProfiler::get().mark("pyramid");
        int field = -1;
        for (float t : phases) {
            if (q_ == Quality::High || field < 0) field = estimate(q_ == Quality::Fast ? 0.5f : t, finest);
            GpuProfiler::get().mark("motion");
            auto o = outPool_.acquire(B->w, B->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (!o) { error = "GPU flow: out of video memory"; return false; }
            InterpParams ip{t, scale, 1.f / B->w, 1.f / B->h, 0.08f, 0.25f, 0.004f, muFull_, sigma_, {}};
            gpu.dispatch(gpu.cs((getenv("FG_R1") ? kInterpR1 : kInterpR2).c_str()), {A->srv[0].Get(), B->srv[0].Get(), levels_[0].lumaA->srv[0].Get(), levels_[0].lumaB->srv[0].Get(),
                                           levels_[finest].field[field]->srv[0].Get()}, {o->uav.Get()}, &ip, sizeof ip, B->w, B->h);
            out.push_back(o);
            GpuProfiler::get().mark("interp");
        }
        if (wantRaw && !out.empty()) {
            rawCopy = gpu.makeImage(out[0]->w, out[0]->h, DXGI_FORMAT_R8G8B8A8_UNORM);
            if (rawCopy) gpu.ctx->CopyResource(rawCopy->tex.Get(), out[0]->tex.Get());
        }
    }
    if (hud) HudStabilizer::get().composite(out, prev.image, next.image, phases, next.motion);
    GpuProfiler::get().mark("hud-paste");
    return true;
}
