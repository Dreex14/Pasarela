// Copyright (c) 2026 Dreex. Licencia MIT.
// Utilidades de interfaz: tema oscuro, dibujo con Direct2D sobre GDI y un
// deslizador propio. Sin dependencias externas.
#pragma once
#include "common.h"

#include <d2d1.h>
#include <dwrite.h>

namespace ui {

struct Color {
    float r, g, b, a;
};
Color Rgb(unsigned hex, float alpha = 1.0f);

namespace theme {
extern const Color bg, surface, surfaceHover, border, text, textDim, accent, accentHover, accentText, danger, ok,
    warn, track;
}

bool Init();
void Shutdown();

// Pintor sobre un HDC (WM_PAINT / WM_DRAWITEM) con antialiasing.
class Painter {
public:
    Painter(HDC dc, const RECT& rc);
    ~Painter();
    bool Ok() const { return ok_; }
    void Fill(const RECT& r, Color c);
    void Round(const RECT& r, float radius, Color fill);
    void RoundOutline(const RECT& r, float radius, Color stroke, float width = 1.0f);
    void Circle(float cx, float cy, float radius, Color fill);
    enum Align { Left, Center, Right };
    void Text(const std::wstring& s, const RECT& r, float sizePx, Color c, Align a = Left, bool bold = false,
              bool wrap = false, const wchar_t* font = L"Segoe UI");
    static float TextHeight(const std::wstring& s, float width, float sizePx, bool bold = false);
    ID2D1RenderTarget* Target();

private:
    bool ok_ = false;
};

// Iconos de "Segoe Fluent Icons"/"Segoe MDL2 Assets" (incluidas en Windows).
const wchar_t* IconFont();

// Desplegable (CBS_DROPDOWNLIST) dibujado por completo en tema oscuro cuando
// está cerrado. La lista se sigue dibujando con WM_DRAWITEM.
void MakeFlatCombo(HWND combo);

// Seguimiento de "hover" para botones propios.
void EnableHover(HWND button);
bool IsHover(HWND button);

// Deslizador horizontal. Envía WM_HSCROLL al padre con lParam = hwnd.
HWND CreateSlider(HWND parent, int id, int minV, int maxV, int value);
void SliderSetValue(HWND slider, int value);
int SliderGetValue(HWND slider);

// Ventana con barra de título oscura.
void ApplyDarkTitleBar(HWND hwnd);

}  // namespace ui
