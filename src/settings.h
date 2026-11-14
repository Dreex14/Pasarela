// Copyright (c) 2026 Dreex. Licencia MIT.
// Configuración persistente en Pasarela.ini (texto plano, local).
#pragma once
#include "common.h"

struct Settings {
    // Selección
    std::wstring videoDevice;   // symlink
    std::wstring videoName;     // para reconocerla aunque cambie de puerto
    bool autoAudio = false;     // al abrir, iniciar «Solo audio» directamente
    UINT32 width = 0, height = 0;  // 0 = automático
    double fps = 0;                // 0 = el máximo disponible
    std::wstring format;           // "" = automático, si no "NV12", "YUY2", "MJPEG"...
    std::wstring audioIn;          // "" = automático (la de la capturadora)
    std::wstring audioOut;         // "" = predeterminada de Windows
    int volume = 80;
    bool muted = false;

    // Avanzado
    bool vsync = false;
    bool fullRange = false;
    bool topmost = false;
    bool tray = true;
    bool diagnostics = false;
    int bufferProfile = 1;
    int audioDelayMs = 0;
    bool hideOnVideo = true;    // ocultar la ventana principal al mostrar el vídeo

    // Ventanas
    RECT videoRect{0, 0, 0, 0};
    int mainX = CW_USEDEFAULT, mainY = CW_USEDEFAULT;

    void Load();
    void Save() const;
};
