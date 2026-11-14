// Copyright (c) 2026 Dreex. Licencia MIT.
#include "ui.h"

#include <commctrl.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

#include <map>
#include <tuple>

namespace ui {

Color Rgb(unsigned hex, float alpha) {
    return {((hex >> 16) & 0xFF) / 255.0f, ((hex >> 8) & 0xFF) / 255.0f, (hex & 0xFF) / 255.0f, alpha};
}

namespace theme {
const Color bg = Rgb(0x15161A);
const Color surface = Rgb(0x23252B);
const Color surfaceHover = Rgb(0x2C2F36);
const Color border = Rgb(0x353842);
const Color text = Rgb(0xE9EAEE);
const Color textDim = Rgb(0x9CA1AB);
const Color accent = Rgb(0x4F7DFF);
const Color accentHover = Rgb(0x6A92FF);
const Color accentText = Rgb(0xFFFFFF);
const Color danger = Rgb(0xFF6B6B);
const Color ok = Rgb(0x4CC38A);
const Color warn = Rgb(0xF2C94C);
const Color track = Rgb(0x3A3D46);
}  // namespace theme

static ComPtr<ID2D1Factory> g_d2d;
static ComPtr<IDWriteFactory> g_dw;
static ComPtr<ID2D1DCRenderTarget> g_rt;
static std::map<std::tuple<int, bool, int, bool, std::wstring>, ComPtr<IDWriteTextFormat>> g_formats;
static const wchar_t* g_iconFont = L"Segoe MDL2 Assets";

static D2D1_COLOR_F C(Color c) { return D2D1::ColorF(c.r, c.g, c.b, c.a); }
static D2D1_RECT_F R(const RECT& r) { return D2D1::RectF(float(r.left), float(r.top), float(r.right), float(r.bottom)); }

static LRESULT CALLBACK SliderProc(HWND, UINT, WPARAM, LPARAM);

static int CALLBACK FontProbe(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM found) {
    *reinterpret_cast<bool*>(found) = true;
    return 0;
}

bool Init() {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, g_d2d.GetAddressOf()))) return false;
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(g_dw.GetAddressOf()))))
        return false;
    D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties(
        // Por software: la interfaz es pequeña y así el modo "Solo audio" no carga
        // Direct3D ni el driver de la GPU.
        D2D1_RENDER_TARGET_TYPE_SOFTWARE, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f,
        96.0f);
    if (FAILED(g_d2d->CreateDCRenderTarget(&p, &g_rt))) return false;

    // Windows 11 trae "Segoe Fluent Icons"; Windows 10, "Segoe MDL2 Assets".
    HDC dc = GetDC(nullptr);
    LOGFONTW lf{};
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe Fluent Icons");
    bool found = false;
    EnumFontFamiliesExW(dc, &lf, FontProbe, reinterpret_cast<LPARAM>(&found), 0);
    ReleaseDC(nullptr, dc);
    if (found) g_iconFont = L"Segoe Fluent Icons";

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = SliderProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_HAND);
    wc.lpszClassName = L"PasarelaSlider";
    wc.cbWndExtra = sizeof(void*);
    RegisterClassExW(&wc);
    return true;
}

void Shutdown() {
    g_formats.clear();
    g_rt.Reset();
    g_dw.Reset();
    g_d2d.Reset();
}

const wchar_t* IconFont() { return g_iconFont; }

Painter::Painter(HDC dc, const RECT& rc) {
    if (!g_rt) return;
    if (FAILED(g_rt->BindDC(dc, &rc))) return;
    g_rt->BeginDraw();
    // El DC render target trabaja en coordenadas relativas a rc.
    g_rt->SetTransform(D2D1::Matrix3x2F::Translation(-float(rc.left), -float(rc.top)));
    g_rt->SetTextAntialiasMode(D2D1_TEXT_ANTIALIAS_MODE_GRAYSCALE);
    ok_ = true;
}

Painter::~Painter() {
    if (ok_) {
        g_rt->SetTransform(D2D1::Matrix3x2F::Identity());
        g_rt->EndDraw();
    }
}

ID2D1RenderTarget* Painter::Target() { return g_rt.Get(); }

static ComPtr<ID2D1SolidColorBrush> Brush(Color c) {
    ComPtr<ID2D1SolidColorBrush> b;
    g_rt->CreateSolidColorBrush(C(c), &b);
    return b;
}

void Painter::Fill(const RECT& r, Color c) {
    if (ok_) g_rt->FillRectangle(R(r), Brush(c).Get());
}

void Painter::Round(const RECT& r, float radius, Color c) {
    if (ok_) g_rt->FillRoundedRectangle(D2D1::RoundedRect(R(r), radius, radius), Brush(c).Get());
}

void Painter::RoundOutline(const RECT& r, float radius, Color c, float width) {
    if (!ok_) return;
    D2D1_RECT_F f = R(r);
    f.left += width / 2; f.top += width / 2; f.right -= width / 2; f.bottom -= width / 2;
    g_rt->DrawRoundedRectangle(D2D1::RoundedRect(f, radius, radius), Brush(c).Get(), width);
}

void Painter::Circle(float cx, float cy, float radius, Color c) {
    if (ok_) g_rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), radius, radius), Brush(c).Get());
}

static IDWriteTextFormat* Format(float sizePx, bool bold, Painter::Align a, bool wrap, const wchar_t* font) {
    auto key = std::make_tuple(int(sizePx * 10), bold, int(a), wrap, std::wstring(font));
    auto it = g_formats.find(key);
    if (it != g_formats.end()) return it->second.Get();
    ComPtr<IDWriteTextFormat> f;
    g_dw->CreateTextFormat(font, nullptr, bold ? DWRITE_FONT_WEIGHT_SEMI_BOLD : DWRITE_FONT_WEIGHT_NORMAL,
                           DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, sizePx, L"", &f);
    if (!f) return nullptr;
    f->SetTextAlignment(a == Painter::Center ? DWRITE_TEXT_ALIGNMENT_CENTER
                        : a == Painter::Right ? DWRITE_TEXT_ALIGNMENT_TRAILING
                                              : DWRITE_TEXT_ALIGNMENT_LEADING);
    f->SetParagraphAlignment(wrap ? DWRITE_PARAGRAPH_ALIGNMENT_NEAR : DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    f->SetWordWrapping(wrap ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
    if (!wrap) {
        DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
        ComPtr<IDWriteInlineObject> ellipsis;
        g_dw->CreateEllipsisTrimmingSign(f.Get(), &ellipsis);
        f->SetTrimming(&trim, ellipsis.Get());
    }
    g_formats[key] = f;
    return f.Get();
}

void Painter::Text(const std::wstring& s, const RECT& r, float sizePx, Color c, Align a, bool bold, bool wrap,
                   const wchar_t* font) {
    if (!ok_ || s.empty()) return;
    IDWriteTextFormat* f = Format(sizePx, bold, a, wrap, font);
    if (!f) return;
    g_rt->DrawText(s.c_str(), UINT32(s.size()), f, R(r), Brush(c).Get(), D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

float Painter::TextHeight(const std::wstring& s, float width, float sizePx, bool bold) {
    if (!g_dw || s.empty()) return 0;
    IDWriteTextFormat* f = Format(sizePx, bold, Left, true, L"Segoe UI");
    ComPtr<IDWriteTextLayout> layout;
    if (FAILED(g_dw->CreateTextLayout(s.c_str(), UINT32(s.size()), f, width, 10000, &layout))) return 0;
    DWRITE_TEXT_METRICS m{};
    layout->GetMetrics(&m);
    return m.height;
}

// --- Hover --------------------------------------------------------------------

static LRESULT CALLBACK HoverProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_MOUSEMOVE:
            if (!GetPropW(h, L"hover")) {
                SetPropW(h, L"hover", (HANDLE)1);
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            break;
        case WM_MOUSELEAVE:
            RemovePropW(h, L"hover");
            InvalidateRect(h, nullptr, FALSE);
            break;
        case WM_NCDESTROY:
            RemovePropW(h, L"hover");
            break;
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void EnableHover(HWND b) { SetWindowSubclass(b, HoverProc, 1, 0); }
bool IsHover(HWND b) { return GetPropW(b, L"hover") != nullptr; }

// --- Desplegable plano ----------------------------------------------------------

static LRESULT CALLBACK FlatComboProc(HWND h, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            {
                Painter p(dc, rc);
                float s = GetDpiForWindow(h) / 96.0f;
                bool enabled = IsWindowEnabled(h) != FALSE;
                bool open = SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0) != 0;
                bool focus = GetFocus() == h;
                p.Fill(rc, theme::bg);
                p.Round(rc, 7 * s, (IsHover(h) || open) && enabled ? theme::surfaceHover : theme::surface);
                if (focus || open) p.RoundOutline(rc, 7 * s, theme::accent, 1.0f * s);
                int sel = (int)SendMessageW(h, CB_GETCURSEL, 0, 0);
                if (sel >= 0) {
                    int len = (int)SendMessageW(h, CB_GETLBTEXTLEN, sel, 0);
                    std::wstring t(std::max(len, 0), L'\0');
                    if (len > 0) SendMessageW(h, CB_GETLBTEXT, sel, (LPARAM)t.data());
                    RECT tr = rc;
                    tr.left += LONG(12 * s);
                    tr.right -= LONG(34 * s);
                    p.Text(t, tr, 13.0f * s, enabled ? theme::text : theme::textDim);
                }
                RECT chev = {rc.right - LONG(32 * s), rc.top, rc.right - LONG(8 * s), rc.bottom};
                p.Text(L"", chev, 10.0f * s, enabled ? theme::textDim : theme::track, Painter::Center, false,
                       false, IconFont());
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
        case CB_SETCURSEL: {
            LRESULT r = DefSubclassProc(h, msg, wp, lp);
            InvalidateRect(h, nullptr, FALSE);
            return r;
        }
    }
    return DefSubclassProc(h, msg, wp, lp);
}

void MakeFlatCombo(HWND combo) {
    EnableHover(combo);
    SetWindowSubclass(combo, FlatComboProc, 2, 0);
}

// --- Deslizador ------------------------------------------------------------------

struct SliderState {
    int minV = 0, maxV = 100, value = 0;
    bool dragging = false, hover = false;
};

static void SliderNotify(HWND h) {
    SendMessageW(GetParent(h), WM_HSCROLL, 0, reinterpret_cast<LPARAM>(h));
}

static void SliderSetFromX(HWND h, SliderState* s, int x) {
    RECT rc;
    GetClientRect(h, &rc);
    float scale = GetDpiForWindow(h) / 96.0f;
    float pad = 9 * scale;
    float t = (x - pad) / std::max(1.0f, (rc.right - 2 * pad));
    t = Clamp(t, 0.0f, 1.0f);
    int v = s->minV + int(t * (s->maxV - s->minV) + 0.5f);
    if (v != s->value) {
        s->value = v;
        InvalidateRect(h, nullptr, FALSE);
        SliderNotify(h);
    }
}

static LRESULT CALLBACK SliderProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* s = reinterpret_cast<SliderState*>(GetWindowLongPtrW(h, 0));
    switch (msg) {
        case WM_NCCREATE: {
            auto* st = new SliderState();
            SetWindowLongPtrW(h, 0, reinterpret_cast<LONG_PTR>(st));
            break;
        }
        case WM_NCDESTROY:
            delete s;
            SetWindowLongPtrW(h, 0, 0);
            break;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            RECT rc;
            GetClientRect(h, &rc);
            {
                Painter p(dc, rc);
                float scale = GetDpiForWindow(h) / 96.0f;
                p.Fill(rc, theme::bg);
                float pad = 9 * scale;
                float cy = rc.bottom / 2.0f;
                float t = s->maxV > s->minV ? float(s->value - s->minV) / (s->maxV - s->minV) : 0;
                float x = pad + t * (rc.right - 2 * pad);
                RECT track = {LONG(pad), LONG(cy - 2 * scale), LONG(rc.right - pad), LONG(cy + 2 * scale)};
                p.Round(track, 2 * scale, theme::track);
                RECT fill = track;
                fill.right = LONG(x);
                p.Round(fill, 2 * scale, IsWindowEnabled(h) ? theme::accent : theme::textDim);
                bool focus = GetFocus() == h;
                p.Circle(x, cy, (s->hover || s->dragging || focus ? 8.0f : 7.0f) * scale, theme::text);
            }
            EndPaint(h, &ps);
            return 0;
        }
        case WM_LBUTTONDOWN:
            SetFocus(h);
            SetCapture(h);
            s->dragging = true;
            SliderSetFromX(h, s, GET_X_LPARAM(lp));
            return 0;
        case WM_MOUSEMOVE:
            if (!s->hover) {
                s->hover = true;
                TRACKMOUSEEVENT t{sizeof(t), TME_LEAVE, h, 0};
                TrackMouseEvent(&t);
                InvalidateRect(h, nullptr, FALSE);
            }
            if (s->dragging) SliderSetFromX(h, s, GET_X_LPARAM(lp));
            return 0;
        case WM_MOUSELEAVE:
            s->hover = false;
            InvalidateRect(h, nullptr, FALSE);
            return 0;
        case WM_LBUTTONUP:
            if (s->dragging) {
                s->dragging = false;
                ReleaseCapture();
                InvalidateRect(h, nullptr, FALSE);
            }
            return 0;
        case WM_MOUSEWHEEL: {
            int step = std::max(1, (s->maxV - s->minV) / 20);
            int delta = GET_WHEEL_DELTA_WPARAM(wp) > 0 ? step : -step;
            int v = Clamp(s->value + delta, s->minV, s->maxV);
            if (v != s->value) {
                s->value = v;
                InvalidateRect(h, nullptr, FALSE);
                SliderNotify(h);
            }
            return 0;
        }
        case WM_GETDLGCODE:
            return DLGC_WANTARROWS;
        case WM_KEYDOWN: {
            int step = (wp == VK_PRIOR || wp == VK_NEXT) ? std::max(1, (s->maxV - s->minV) / 10) : 1;
            int v = s->value;
            if (wp == VK_LEFT || wp == VK_DOWN || wp == VK_NEXT) v -= step;
            else if (wp == VK_RIGHT || wp == VK_UP || wp == VK_PRIOR) v += step;
            else if (wp == VK_HOME) v = s->minV;
            else if (wp == VK_END) v = s->maxV;
            else break;
            v = Clamp(v, s->minV, s->maxV);
            if (v != s->value) {
                s->value = v;
                InvalidateRect(h, nullptr, FALSE);
                SliderNotify(h);
            }
            return 0;
        }
        case WM_SETFOCUS:
        case WM_KILLFOCUS:
        case WM_ENABLE:
            InvalidateRect(h, nullptr, FALSE);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

HWND CreateSlider(HWND parent, int id, int minV, int maxV, int value) {
    HWND h = CreateWindowExW(0, L"PasarelaSlider", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP, 0, 0, 10, 10, parent,
                             reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
    auto* s = reinterpret_cast<SliderState*>(GetWindowLongPtrW(h, 0));
    s->minV = minV;
    s->maxV = maxV;
    s->value = Clamp(value, minV, maxV);
    return h;
}

void SliderSetValue(HWND h, int v) {
    auto* s = reinterpret_cast<SliderState*>(GetWindowLongPtrW(h, 0));
    if (!s) return;
    s->value = Clamp(v, s->minV, s->maxV);
    InvalidateRect(h, nullptr, FALSE);
}

int SliderGetValue(HWND h) {
    auto* s = reinterpret_cast<SliderState*>(GetWindowLongPtrW(h, 0));
    return s ? s->value : 0;
}

void ApplyDarkTitleBar(HWND hwnd) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    COLORREF caption = RGB(0x15, 0x16, 0x1A);
    DwmSetWindowAttribute(hwnd, 35 /* DWMWA_CAPTION_COLOR (Windows 11) */, &caption, sizeof(caption));
}

}  // namespace ui
