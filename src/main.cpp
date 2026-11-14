// Pasarela - visor y monitor de audio para capturadoras HDMI.
// Copyright (c) 2026 Dreex. Licencia MIT.
#include <mfapi.h>
#include <objbase.h>

#include "app.h"
#include "log.h"
#include "ui.h"

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    // Una sola instancia: la capturadora solo admite un programa a la vez.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\Pasarela.InstanciaUnica");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND existing = FindWindowW(L"PasarelaPrincipal", nullptr);
        if (existing) {
            ShowWindow(existing, SW_SHOW);
            ShowWindow(existing, SW_RESTORE);
            SetForegroundWindow(existing);
        }
        return 0;
    }

    HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
    LogInit();
    Log(L"%s %s", APP_NAME, APP_VERSION);

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(hr)) return 1;
    hr = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"No se pudo iniciar Media Foundation. En Windows N/KN instala el «Media Feature Pack».",
                    APP_NAME, MB_ICONERROR);
        return 1;
    }
    if (!ui::Init()) {
        MessageBoxW(nullptr, L"No se pudo iniciar Direct2D.", APP_NAME, MB_ICONERROR);
        return 1;
    }

    int code = RunApp(instance, show);

    ui::Shutdown();
    MFShutdown();
    CoUninitialize();
    Log(L"Fin");
    if (mutex) CloseHandle(mutex);
    return code;
}
