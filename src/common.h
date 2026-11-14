// Pasarela - visor y monitor de audio para capturadoras HDMI.
// Copyright (c) 2026 Dreex. Licencia MIT (ver LICENSE).
#pragma once

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wrl/client.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;

#define APP_NAME L"Pasarela"
#define APP_VERSION L"0.1.0"

// Mensajes privados entre hilos y la ventana principal.
enum : UINT {
    WM_APP_FORMATS_READY = WM_APP + 1,   // lParam = FormatsResult*
    WM_APP_VIDEO_ERROR,                  // wParam = HRESULT
    WM_APP_AUDIO_DEVICES_CHANGED,        // lista de dispositivos de audio cambió
    WM_APP_AUDIO_DEFAULT_CHANGED,        // cambió la salida predeterminada
    WM_APP_TRAY,                         // icono de la bandeja
    WM_APP_VIDEO_WINDOW_CLOSED,
    WM_APP_VIDEO_WINDOW_KEY,             // wParam = acción
};

inline int64_t QpcNow() {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline double QpcToMs(int64_t ticks) {
    static const double factor = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1000.0 / static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) * factor;
}

std::wstring Fmt(const wchar_t* fmt, ...);
std::wstring HrHex(HRESULT hr);
std::string ToUtf8(const std::wstring& s);
std::wstring FromUtf8(const std::string& s);

// Carpeta donde se guardan configuración y registro: junto al .exe si se puede
// escribir allí (modo portátil); si no, %APPDATA%\Pasarela.
const std::wstring& DataDirectory();
std::wstring ExeDirectory();

template <typename T>
T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
