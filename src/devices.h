// Copyright (c) 2026 Dreex. Licencia MIT.
// Detección de capturadoras (Media Foundation) y de dispositivos de audio (MMDevice/WASAPI).
#pragma once
#include "common.h"

#include <mferror.h>
#include <mmdeviceapi.h>

struct VideoDevice {
    std::wstring name;
    std::wstring symlink;      // identificador estable del dispositivo
    std::wstring containerId;  // agrupa vídeo y audio del mismo aparato físico
};

struct AudioEndpoint {
    std::wstring name;
    std::wstring id;
    std::wstring containerId;
    bool isDefault = false;
};

// Un modo que la capturadora declara de verdad (no se inventa ninguno).
struct VideoFormat {
    GUID subtype{};
    UINT32 width = 0, height = 0;
    UINT32 fpsNum = 0, fpsDen = 1;
    double Fps() const { return fpsDen ? double(fpsNum) / double(fpsDen) : 0.0; }
};

std::vector<VideoDevice> EnumerateVideoDevices();
std::vector<AudioEndpoint> EnumerateAudioEndpoints(EDataFlow flow);
// Abre el dispositivo y lee la lista de tipos nativos. Puede tardar y fallar si
// otra aplicación tiene la capturadora en uso.
HRESULT EnumerateVideoFormats(const std::wstring& symlink, std::vector<VideoFormat>& out);

// Busca la entrada de audio que pertenece a la misma capturadora (por ContainerId
// y, si no, por nombre). Devuelve -1 si no hay coincidencia fiable.
int FindAssociatedAudioInput(const VideoDevice& video, const std::vector<AudioEndpoint>& inputs);

const wchar_t* SubtypeName(const GUID& subtype);
bool IsUncompressedRenderable(const GUID& subtype);  // NV12, YUY2, RGB32, ARGB32
int SubtypePreference(const GUID& subtype);          // menor = mejor

// Elección automática: ~60 FPS, la mayor resolución que quepa en la pantalla
// (maxW×maxH; 0 = sin límite) y formatos sin compresión. RankAutoFormats
// devuelve todos los candidatos ordenados, para poder probar el siguiente si la
// capturadora no consigue arrancar uno (p. ej. por ancho de banda USB).
std::vector<int> RankAutoFormats(const std::vector<VideoFormat>& formats, UINT32 maxW, UINT32 maxH);
int ChooseAutoFormat(const std::vector<VideoFormat>& formats, UINT32 maxW = 0, UINT32 maxH = 0);

// Notificaciones de MMDevice (altas, bajas y cambio de salida predeterminada).
class AudioDeviceWatcher;
AudioDeviceWatcher* StartAudioDeviceWatcher(HWND notifyWindow);
void StopAudioDeviceWatcher(AudioDeviceWatcher* watcher);
