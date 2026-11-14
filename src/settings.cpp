// Copyright (c) 2026 Dreex. Licencia MIT.
#include "settings.h"

#include <cwchar>

static std::wstring IniPath() { return DataDirectory() + L"\\Pasarela.ini"; }

static std::wstring ReadStr(const wchar_t* sec, const wchar_t* key, const wchar_t* def) {
    wchar_t buf[2048];
    GetPrivateProfileStringW(sec, key, def, buf, _countof(buf), IniPath().c_str());
    return buf;
}
static int ReadInt(const wchar_t* sec, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(sec, key, def, IniPath().c_str());
}
static void WriteStr(const wchar_t* sec, const wchar_t* key, const std::wstring& v) {
    // Entre comillas para conservar espacios iniciales/finales.
    std::wstring q = L"\"" + v + L"\"";
    WritePrivateProfileStringW(sec, key, q.c_str(), IniPath().c_str());
}
static void WriteInt(const wchar_t* sec, const wchar_t* key, int v) {
    WritePrivateProfileStringW(sec, key, std::to_wstring(v).c_str(), IniPath().c_str());
}

void Settings::Load() {
    const wchar_t* d = L"Dispositivos";
    const wchar_t* a = L"Avanzado";
    const wchar_t* w = L"Ventanas";
    videoDevice = ReadStr(d, L"Video", L"");
    videoName = ReadStr(d, L"VideoNombre", L"");
    // "Reanudar" es el nombre antiguo de esta opción: se respeta si ya estaba activada.
    autoAudio = ReadInt(d, L"IniciarSoloAudio", ReadInt(d, L"Reanudar", 0)) != 0;
    width = (UINT32)ReadInt(d, L"Ancho", 0);
    height = (UINT32)ReadInt(d, L"Alto", 0);
    fps = _wtof(ReadStr(d, L"FPS", L"0").c_str());
    format = ReadStr(d, L"Formato", L"");
    audioIn = ReadStr(d, L"AudioEntrada", L"");
    audioOut = ReadStr(d, L"AudioSalida", L"");
    volume = Clamp(ReadInt(d, L"Volumen", 80), 0, 100);
    muted = ReadInt(d, L"Silencio", 0) != 0;

    vsync = ReadInt(a, L"VSync", 0) != 0;
    fullRange = ReadInt(a, L"RangoCompleto", 0) != 0;
    topmost = ReadInt(a, L"SiempreEncima", 0) != 0;
    tray = ReadInt(a, L"Bandeja", 1) != 0;
    diagnostics = ReadInt(a, L"Diagnostico", 0) != 0;
    bufferProfile = Clamp(ReadInt(a, L"BuferAudio", 1), 0, 2);
    audioDelayMs = Clamp(ReadInt(a, L"RetardoAudioMs", 0), 0, 300);
    hideOnVideo = ReadInt(a, L"OcultarConVideo", 1) != 0;

    videoRect.left = ReadInt(w, L"VideoX", 0);
    videoRect.top = ReadInt(w, L"VideoY", 0);
    videoRect.right = videoRect.left + ReadInt(w, L"VideoAncho", 0);
    videoRect.bottom = videoRect.top + ReadInt(w, L"VideoAlto", 0);
    mainX = ReadInt(w, L"PrincipalX", CW_USEDEFAULT);
    mainY = ReadInt(w, L"PrincipalY", CW_USEDEFAULT);
}

void Settings::Save() const {
    // Las APIs de perfiles solo guardan Unicode si el archivo ya es UTF-16 con BOM.
    if (GetFileAttributesW(IniPath().c_str()) == INVALID_FILE_ATTRIBUTES) {
        HANDLE h = CreateFileW(IniPath().c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            const BYTE bom[2] = {0xFF, 0xFE};
            DWORD written = 0;
            WriteFile(h, bom, 2, &written, nullptr);
            CloseHandle(h);
        }
    }
    const wchar_t* d = L"Dispositivos";
    const wchar_t* a = L"Avanzado";
    const wchar_t* w = L"Ventanas";
    WriteStr(d, L"Video", videoDevice);
    WriteStr(d, L"VideoNombre", videoName);
    WriteInt(d, L"IniciarSoloAudio", autoAudio);
    WriteInt(d, L"Ancho", (int)width);
    WriteInt(d, L"Alto", (int)height);
    WriteStr(d, L"FPS", Fmt(L"%.3f", fps));
    WriteStr(d, L"Formato", format);
    WriteStr(d, L"AudioEntrada", audioIn);
    WriteStr(d, L"AudioSalida", audioOut);
    WriteInt(d, L"Volumen", volume);
    WriteInt(d, L"Silencio", muted);

    WriteInt(a, L"VSync", vsync);
    WriteInt(a, L"RangoCompleto", fullRange);
    WriteInt(a, L"SiempreEncima", topmost);
    WriteInt(a, L"Bandeja", tray);
    WriteInt(a, L"Diagnostico", diagnostics);
    WriteInt(a, L"BuferAudio", bufferProfile);
    WriteInt(a, L"RetardoAudioMs", audioDelayMs);
    WriteInt(a, L"OcultarConVideo", hideOnVideo);

    WriteInt(w, L"VideoX", videoRect.left);
    WriteInt(w, L"VideoY", videoRect.top);
    WriteInt(w, L"VideoAncho", videoRect.right - videoRect.left);
    WriteInt(w, L"VideoAlto", videoRect.bottom - videoRect.top);
    WriteInt(w, L"PrincipalX", mainX);
    WriteInt(w, L"PrincipalY", mainY);
}
