#include "Dashboard.h"
#include <algorithm>
#include <cmath>

namespace {
const D2D1_COLOR_F kWhite = {1, 1, 1, 1};
const D2D1_COLOR_F kGreen = {0.20f, 0.78f, 0.35f, 1};
const D2D1_COLOR_F kYellow = {1.0f, 0.80f, 0.0f, 1};
const D2D1_COLOR_F kOrange = {1.0f, 0.58f, 0.0f, 1};
D2D1_COLOR_F white(float a) { return {1, 1, 1, a}; }
bool inside(const D2D1_RECT_F& r, float x, float y) { return x >= r.left && x < r.right && y >= r.top && y < r.bottom; }
}

void Dashboard::ensureFonts() {
    if (fonts_) return;
    auto& dw = Gpu::get().dwrite;
    auto mk = [&](ComPtr<IDWriteTextFormat>& f, const wchar_t* family, float size, DWRITE_FONT_WEIGHT w) {
        f.Reset();
        dw->CreateTextFormat(family, nullptr, w, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, size, L"en-us", &f);
        if (f) f->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    };
    mk(fCaption_, L"Segoe UI", 10, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fLabel_, L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fSmall_, L"Segoe UI", 11, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fStatus_, L"Segoe UI", 12, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fBig_, L"Segoe UI", 44, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fMono_, L"Cascadia Mono", 10, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fValue_, L"Segoe UI", 13, DWRITE_FONT_WEIGHT_SEMI_BOLD);
    mk(fIcon_, L"Segoe MDL2 Assets", 14, DWRITE_FONT_WEIGHT_NORMAL);
    mk(fButton_, L"Segoe UI", 12, DWRITE_FONT_WEIGHT_NORMAL);
    fonts_ = true;
}

void Dashboard::setControls(int multiplier, EngineKind engine, bool compare, bool miniCounter, float volume, bool muted, bool hudLock, bool enginesOK,
                            const std::vector<EngineChoice>& engines) {
    std::lock_guard<std::mutex> l(m_);
    multiplier_ = multiplier; engine_ = engine; compare_ = compare; miniOn_ = miniCounter;
    volume_ = volume; muted_ = muted; hud_ = hudLock; enginesOK_ = enginesOK; engines_ = engines;
}
void Dashboard::setSources(const std::vector<std::string>& devices, const std::string& selected) {
    std::lock_guard<std::mutex> l(m_); sources_ = devices; selectedSource_ = selected;
}
void Dashboard::setResolutions(const std::vector<std::pair<int, int>>& list, std::pair<int, int> selected) {
    std::lock_guard<std::mutex> l(m_); resolutions_ = list; selectedRes_ = selected;
}
void Dashboard::update(const Live& s) {
    std::lock_guard<std::mutex> l(m_);
    live_ = s;
    history_.push_back(s.outputFPS);
    while (history_.size() > 60) history_.pop_front();
}

float Dashboard::layoutAndDraw(ID2D1DeviceContext* dc, bool draw) {
    items_.clear();
    const float W = 340, P = 16;
    ComPtr<ID2D1SolidColorBrush> br;
    if (draw) dc->CreateSolidColorBrush(kWhite, &br);
    auto color = [&](D2D1_COLOR_F c) { br->SetColor(c); return br.Get(); };
    auto text = [&](const std::string& s, D2D1_RECT_F r, IDWriteTextFormat* f, D2D1_COLOR_F c,
                    DWRITE_TEXT_ALIGNMENT a = DWRITE_TEXT_ALIGNMENT_LEADING) {
        if (!draw || s.empty()) return;
        std::wstring w = widen(s);
        f->SetTextAlignment(a);
        f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        dc->DrawTextW(w.c_str(), UINT32(w.size()), f, r, color(c), D2D1_DRAW_TEXT_OPTIONS_CLIP);
    };
    auto rrect = [&](D2D1_RECT_F r, float rad, D2D1_COLOR_F c) { if (draw) dc->FillRoundedRectangle(D2D1::RoundedRect(r, rad, rad), color(c)); };
    auto button = [&](Id id, D2D1_RECT_F r, const std::string& label) {
        items_.push_back({id, r});
        rrect(r, 5, white(0.14f));
        text(label, r, fButton_.Get(), kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
    };
    auto segmented = [&](Id id, D2D1_RECT_F r, const std::vector<std::string>& labels, int sel, std::vector<bool> enabled) {
        items_.push_back({id, r, int(labels.size())});
        rrect(r, 6, white(0.08f));
        float sw = (r.right - r.left) / labels.size();
        for (size_t i = 0; i < labels.size(); i++) {
            D2D1_RECT_F s = D2D1::RectF(r.left + sw * i, r.top, r.left + sw * (i + 1), r.bottom);
            if (int(i) == sel) rrect(D2D1::RectF(s.left + 2, s.top + 2, s.right - 2, s.bottom - 2), 5, white(enabled[i] ? 0.26f : 0.12f));
            text(labels[i], s, fButton_.Get(), white(enabled[i] ? 1.f : 0.35f), DWRITE_TEXT_ALIGNMENT_CENTER);
        }
    };
    auto checkbox = [&](Id id, float y, const std::string& label, bool on, bool enabled) {
        D2D1_RECT_F r = D2D1::RectF(P, y, W - P, y + 18);
        items_.push_back({id, r});
        D2D1_RECT_F box = D2D1::RectF(P, y + 2, P + 14, y + 16);
        if (draw) {
            if (on) rrect(box, 3, enabled ? D2D1_COLOR_F{0.0f, 0.47f, 0.84f, 1} : white(0.3f));
            else dc->DrawRoundedRectangle(D2D1::RoundedRect(box, 3, 3), color(white(enabled ? 0.6f : 0.25f)), 1.2f);
            if (on) {
                dc->DrawLine(D2D1::Point2F(P + 3, y + 9), D2D1::Point2F(P + 6, y + 12.5f), color(kWhite), 1.8f);
                dc->DrawLine(D2D1::Point2F(P + 6, y + 12.5f), D2D1::Point2F(P + 11.5f, y + 5), color(kWhite), 1.8f);
            }
        }
        text(label, D2D1::RectF(P + 22, y, W - P, y + 18), fSmall_.Get(), white(enabled ? 0.9f : 0.4f));
    };
    auto separator = [&](float y) { if (draw) dc->FillRectangle(D2D1::RectF(P, y, W - P, y + 1), color(white(0.12f))); };
    auto rowLabel = [&](const std::string& s, float y) { text(s, D2D1::RectF(P, y, P + 112, y + 26), fLabel_.Get(), white(0.75f)); };

    float y = 14;
    if (draw) rrect(D2D1::RectF(0, 0, W, contentH_), 16, D2D1_COLOR_F{0.11f, 0.11f, 0.12f, 0.88f});

    // Header: status + hide button
    if (draw) dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(P + 4, y + 9), 4, 4), color(live_.live ? kGreen : kOrange));
    text(live_.status, D2D1::RectF(P + 14, y, W - P - 76, y + 18), fStatus_.Get(), kWhite);
    button(kHide, D2D1::RectF(W - P - 66, y - 1, W - P, y + 19), "Hide  H");
    y += 26;

    // Big numbers
    bool healthy = live_.outputFPS >= live_.targetFPS * 0.93 || live_.targetFPS == 0;
    std::string out = live_.outputFPS > 0 ? std::to_string(int(std::lround(live_.outputFPS))) : "—";
    std::string game = live_.gameFPS > 0 ? std::to_string(int(std::lround(live_.gameFPS))) : "—";
    text(out, D2D1::RectF(P, y, P + 110, y + 54), fBig_.Get(), live_.outputFPS == 0 ? kWhite : (healthy ? kGreen : kYellow));
    text("←", D2D1::RectF(P + 112, y + 6, P + 140, y + 48), fValue_.Get(), white(0.35f), DWRITE_TEXT_ALIGNMENT_CENTER);
    text(game, D2D1::RectF(P + 146, y, P + 260, y + 54), fBig_.Get(), white(0.6f));
    y += 52;
    text("OUTPUT FPS", D2D1::RectF(P, y, P + 120, y + 14), fCaption_.Get(), white(0.5f));
    text("GAME FPS", D2D1::RectF(P + 146, y, P + 260, y + 14), fCaption_.Get(), white(0.5f));
    y += 18;

    // Sparkline: last 15 s of output FPS against the target.
    {
        D2D1_RECT_F r = D2D1::RectF(P + 1, y + 3, W - P - 1, y + 33);
        double target = std::max(live_.targetFPS, 1.0), top = target * 1.2;
        for (double v : history_) top = std::max(top, v * 1.05);
        auto Y = [&](double v) { return float(r.bottom - std::min(v, top) / top * (r.bottom - r.top)); };
        if (draw) {
            ComPtr<ID2D1StrokeStyle> dash;
            float dashes[] = {3, 3};
            Gpu::get().d2dFactory->CreateStrokeStyle(D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT, D2D1_CAP_STYLE_FLAT,
                                                     D2D1_LINE_JOIN_MITER, 10, D2D1_DASH_STYLE_CUSTOM, 0), dashes, 2, &dash);
            dc->DrawLine(D2D1::Point2F(r.left, Y(target)), D2D1::Point2F(r.right, Y(target)), color(white(0.25f)), 1, dash.Get());
            if (history_.size() > 1) {
                float step = (r.right - r.left) / float(history_.size() - 1);
                ComPtr<ID2D1PathGeometry> line, fill;
                Gpu::get().d2dFactory->CreatePathGeometry(&line);
                Gpu::get().d2dFactory->CreatePathGeometry(&fill);
                ComPtr<ID2D1GeometrySink> s1, s2;
                line->Open(&s1); fill->Open(&s2);
                for (size_t i = 0; i < history_.size(); i++) {
                    D2D1_POINT_2F pt = D2D1::Point2F(r.left + step * i, Y(history_[i]));
                    if (i == 0) { s1->BeginFigure(pt, D2D1_FIGURE_BEGIN_HOLLOW); s2->BeginFigure(D2D1::Point2F(r.left, r.bottom), D2D1_FIGURE_BEGIN_FILLED); s2->AddLine(pt); }
                    else { s1->AddLine(pt); s2->AddLine(pt); }
                }
                s2->AddLine(D2D1::Point2F(r.left + step * (history_.size() - 1), r.bottom));
                s1->EndFigure(D2D1_FIGURE_END_OPEN); s2->EndFigure(D2D1_FIGURE_END_CLOSED);
                s1->Close(); s2->Close();
                dc->FillGeometry(fill.Get(), color(D2D1_COLOR_F{kGreen.r, kGreen.g, kGreen.b, 0.18f}));
                dc->DrawGeometry(line.Get(), color(kGreen), 1.5f);
            }
        }
        y += 36 + 10;
    }
    separator(y); y += 11;

    // Controls
    const float cx = P + 118;
    rowLabel("Frame generation", y);
    // Off 2× 3× 4× 6× 8× (5× and 7× are in Settings and the right-click menu)
    static const int kSegMult[] = {1, 2, 3, 4, 6, 8};
    int segSel = -1; for (int i = 0; i < 6; i++) if (kSegMult[i] == multiplier_) segSel = i;
    segmented(kMult, D2D1::RectF(cx, y, W - P, y + 26), {"Off", "2×", "3×", "4×", "6×", "8×"}, segSel, {true, enginesOK_, enginesOK_, enginesOK_, enginesOK_, enginesOK_});
    y += 36;
    bool genOn = multiplier_ > 1;
    auto dropdown = [&](Id id, const std::string& label, bool enabled = true) {
        D2D1_RECT_F r = D2D1::RectF(cx, y, W - P, y + 26);
        items_.push_back({id, r});
        rrect(r, 5, white(0.10f));
        text(label, D2D1::RectF(r.left + 8, r.top, r.right - 22, r.bottom), fButton_.Get(), white(enabled ? 1.f : 0.45f));
        text("\xEE\x9C\x8D", D2D1::RectF(r.right - 22, r.top, r.right - 4, r.bottom), fIcon_.Get(), white(0.7f), DWRITE_TEXT_ALIGNMENT_CENTER);   // U+E70D chevron
    };
    rowLabel("Mode", y);
    dropdown(kEngine, engineShortLabel(engine_), genOn);
    y += 36;
    rowLabel("Input", y);
    dropdown(kSource, selectedSource_.empty() ? "Test pattern" : selectedSource_);
    y += 36;
    rowLabel("Resolution", y);
    dropdown(kRes, selectedRes_.first > 0 ? strf("%d × %d", selectedRes_.first, selectedRes_.second) : "Auto (1920 × 1080)");
    y += 36;
    rowLabel("Sound", y);
    {
        D2D1_RECT_F mr = D2D1::RectF(cx, y + 1, cx + 24, y + 25);
        items_.push_back({kMute, mr});
        const char* glyph = (muted_ || volume_ == 0) ? "\xEE\x9D\x8F" /* E74F mute */ : (volume_ < 0.5f ? "\xEE\xA6\x93" /* E993 */ : "\xEE\xA6\x95" /* E995 */);
        text(glyph, mr, fIcon_.Get(), kWhite, DWRITE_TEXT_ALIGNMENT_CENTER);
        sliderRect_ = D2D1::RectF(cx + 32, y + 4, cx + 182, y + 22);
        items_.push_back({kVolume, sliderRect_});
        if (draw) {
            float my = (sliderRect_.top + sliderRect_.bottom) / 2, kx = sliderRect_.left + (sliderRect_.right - sliderRect_.left) * volume_;
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sliderRect_.left, my - 2, sliderRect_.right, my + 2), 2, 2), color(white(0.2f)));
            dc->FillRoundedRectangle(D2D1::RoundedRect(D2D1::RectF(sliderRect_.left, my - 2, kx, my + 2), 2, 2), color(D2D1_COLOR_F{0.0f, 0.47f, 0.84f, 1}));
            dc->FillEllipse(D2D1::Ellipse(D2D1::Point2F(kx, my), 7, 7), color(kWhite));
        }
    }
    y += 32;
    if (!live_.sound.empty()) { text(live_.sound, D2D1::RectF(P, y, W - P, y + 16), fSmall_.Get(), white(0.6f)); y += 24; }
    checkbox(kHud, y, "Stabilize HUD & text (stops menus wobbling)", hud_, genOn); y += 26;
    checkbox(kCompare, y, "Compare (left: original, right: generated)", compare_, true); y += 28;
    separator(y); y += 11;

    // Health
    auto stat = [&](float x, const std::string& cap, const std::string& val) {
        text(cap, D2D1::RectF(x, y, x + 100, y + 14), fCaption_.Get(), white(0.5f));
        text(val, D2D1::RectF(x, y + 15, x + 100, y + 33), fValue_.Get(), kWhite);
    };
    float col = (W - 2 * P) / 3;
    stat(P, "DELAY", live_.delayMs > 0 ? strf("%.0f ms", live_.delayMs) : "—");
    stat(P + col, "GEN TIME", live_.genMs > 0 ? strf("%.1f ms", live_.genMs) : "—");
    stat(P + 2 * col, "DISPLAY", live_.displayHz > 0 ? strf("%.0f Hz", live_.displayHz) : "—");
    y += 40;
    text(live_.signal, D2D1::RectF(P, y, W - P, y + 16), fMono_.Get(), white(0.6f));
    y += 22;
    if (!live_.warning.empty()) {
        std::wstring w = widen(live_.warning);
        ComPtr<IDWriteTextLayout> lay;
        fSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_WRAP);
        fSmall_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        fSmall_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        Gpu::get().dwrite->CreateTextLayout(w.c_str(), UINT32(w.size()), fSmall_.Get(), W - 2 * P, 400, &lay);
        fSmall_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TEXT_METRICS tm{};
        if (lay) {
            lay->GetMetrics(&tm);
            if (draw) dc->DrawTextLayout(D2D1::Point2F(P, y), lay.Get(), color(kYellow));
        }
        y += tm.height + 10;
    }
    if (!live_.actionTitle.empty()) { button(kAction, D2D1::RectF(P, y, P + 220, y + 24), live_.actionTitle); y += 34; }
    button(kFull, D2D1::RectF(P, y, P + 120, y + 26), "Full Screen  F");
    button(kSettings, D2D1::RectF(P + 128, y, P + 228, y + 26), "Settings…");
    y += 36;
    checkbox(kMini, y, "Show small FPS counter when hidden", miniOn_, true); y += 26;
    text("Right-click the picture for all options · H hide · F full screen", D2D1::RectF(P, y, W - P, y + 14), fCaption_.Get(), white(0.4f));
    y += 14 + 14;
    return y;
}

ImagePtr Dashboard::render() {
    std::lock_guard<std::mutex> l(m_);
    auto& gpu = Gpu::get();
    ensureFonts();
    if (!dc_ && FAILED(gpu.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return nullptr;
    contentH_ = layoutAndDraw(nullptr, false);
    int w = int(340 * scale_), h = int(std::ceil(820 * scale_));
    if (!image_) image_ = gpu.makeImage(w, h, DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!image_) return nullptr;
    ComPtr<IDXGISurface> surf; image_->tex.As(&surf);
    ComPtr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return nullptr;
    auto gl = gpu.lock();
    dc_->SetTarget(target.Get());
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    dc_->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    layoutAndDraw(dc_.Get(), true);
    dc_->EndDraw();
    dc_->SetTarget(nullptr);
    return image_;
}

ImagePtr Dashboard::renderMini(double fps, bool healthy) {
    std::lock_guard<std::mutex> l(m_);
    auto& gpu = Gpu::get();
    ensureFonts();
    if (!dc_ && FAILED(gpu.d2dDevice->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &dc_))) return nullptr;
    miniW_ = 80;
    if (!mini_) mini_ = gpu.makeImage(int(miniW_ * scale_), int(26 * scale_), DXGI_FORMAT_B8G8R8A8_UNORM);
    if (!mini_) return nullptr;
    ComPtr<IDXGISurface> surf; mini_->tex.As(&surf);
    ComPtr<ID2D1Bitmap1> target;
    D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
                                                         D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
    if (FAILED(dc_->CreateBitmapFromDxgiSurface(surf.Get(), &bp, &target))) return nullptr;
    auto gl = gpu.lock();
    dc_->SetTarget(target.Get());
    dc_->BeginDraw();
    dc_->Clear(D2D1::ColorF(0, 0, 0, 0));
    dc_->SetTransform(D2D1::Matrix3x2F::Scale(scale_, scale_));
    ComPtr<ID2D1SolidColorBrush> br;
    dc_->CreateSolidColorBrush(D2D1_COLOR_F{0.11f, 0.11f, 0.12f, 0.85f}, &br);
    D2D1_RECT_F r = D2D1::RectF(0, 0, miniW_, 26);
    dc_->FillRoundedRectangle(D2D1::RoundedRect(r, 9, 9), br.Get());
    br->SetColor(healthy ? kGreen : kYellow);
    std::wstring s = widen(fps > 0 ? strf("%d FPS", int(std::lround(fps))) : "— FPS");
    fValue_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    fValue_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    dc_->DrawTextW(s.c_str(), UINT32(s.size()), fValue_.Get(), r, br.Get());
    dc_->EndDraw();
    dc_->SetTarget(nullptr);
    return mini_;
}

void Dashboard::appendEngineItems(HMENU m, const std::vector<EngineChoice>& list, EngineKind current, UINT firstId) {
    for (size_t i = 0; i < list.size(); i++) {
        std::string s = list[i].label;
        if (!list[i].available) s += "  (not available)";
        AppendMenuW(m, MF_STRING | (list[i].kind == current ? MF_CHECKED : 0) | (list[i].available ? 0 : MF_GRAYED), firstId + UINT(i), widen(s).c_str());
    }
}

bool Dashboard::mouseDown(int px, int py, HWND hwnd, POINT screen) {
    float x = px / scale_, y = py / scale_;
    if (x < 0 || y < 0 || x >= 340 || y >= contentH_) return false;
    Item hit{kNone, {}};
    {
        std::lock_guard<std::mutex> l(m_);
        for (auto& it : items_) if (inside(it.r, x, y)) { hit = it; break; }
    }
    auto seg = [&](int n) { return std::clamp(int((x - hit.r.left) / ((hit.r.right - hit.r.left) / n)), 0, n - 1); };
    switch (hit.id) {
    case kHide: if (onHide) onHide(); break;
    case kMult: { static const int kSegMult[] = {1, 2, 3, 4, 6, 8}; int i = seg(6); if ((i == 0 || enginesOK_) && onMultiplier) onMultiplier(kSegMult[i]); break; }
    case kEngine: {
        HMENU m = CreatePopupMenu();
        std::vector<EngineChoice> list; EngineKind cur;
        { std::lock_guard<std::mutex> l(m_); list = engines_; cur = engine_; }
        appendEngineItems(m, list, cur, 100);
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd >= 100 && size_t(cmd - 100) < list.size() && list[cmd - 100].available && onEngine) onEngine(list[cmd - 100].kind);
        break;
    }
    case kSource: {
        HMENU m = CreatePopupMenu();
        std::vector<std::string> src; std::string sel;
        { std::lock_guard<std::mutex> l(m_); src = sources_; sel = selectedSource_; }
        for (size_t i = 0; i < src.size(); i++) AppendMenuW(m, MF_STRING | (src[i] == sel ? MF_CHECKED : 0), 100 + i, widen(src[i]).c_str());
        if (!src.empty()) AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (sel.empty() ? MF_CHECKED : 0), 99, L"Test pattern");
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 99 && onSource) onSource("");
        else if (cmd >= 100 && size_t(cmd - 100) < src.size() && onSource) onSource(src[cmd - 100]);
        break;
    }
    case kRes: {
        HMENU m = CreatePopupMenu();
        std::vector<std::pair<int, int>> list; std::pair<int, int> sel;
        { std::lock_guard<std::mutex> l(m_); list = resolutions_; sel = selectedRes_; }
        AppendMenuW(m, MF_STRING | (sel.first == 0 ? MF_CHECKED : 0), 99, L"Auto (1920 × 1080)");
        for (size_t i = 0; i < list.size(); i++)
            AppendMenuW(m, MF_STRING | (list[i] == sel ? MF_CHECKED : 0), 100 + i, widen(strf("%d × %d", list[i].first, list[i].second)).c_str());
        int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_NONOTIFY, screen.x, screen.y, 0, hwnd, nullptr);
        DestroyMenu(m);
        if (cmd == 99 && onResolution) onResolution(0, 0);
        else if (cmd >= 100 && size_t(cmd - 100) < list.size() && onResolution) onResolution(list[cmd - 100].first, list[cmd - 100].second);
        break;
    }
    case kMute: if (onMute) onMute(!muted_); break;
    case kVolume: dragSlider_ = true; mouseMove(px, py); break;
    case kHud: if (multiplier_ > 1 && onHudLock) onHudLock(!hud_); break;
    case kCompare: if (onCompare) onCompare(!compare_); break;
    case kAction: { std::function<void()> a; { std::lock_guard<std::mutex> l(m_); a = live_.action; } if (a) a(); break; }
    case kFull: if (onFullscreen) onFullscreen(); break;
    case kSettings: if (onSettings) onSettings(); break;
    case kMini: if (onMiniCounter) onMiniCounter(!miniOn_); break;
    default: break;
    }
    return true;   // clicks anywhere on the panel belong to the dashboard
}

void Dashboard::mouseMove(int px, int) {
    if (!dragSlider_) return;
    float x = px / scale_;
    float v = std::clamp((x - sliderRect_.left) / (sliderRect_.right - sliderRect_.left), 0.f, 1.f);
    { std::lock_guard<std::mutex> l(m_); volume_ = v; if (muted_) muted_ = false; }
    if (onVolume) onVolume(v);
}

void Dashboard::mouseUp() { dragSlider_ = false; }
