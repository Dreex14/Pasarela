// Copyright (c) 2026 Dreex. Licencia MIT.
// Presentación en GPU con Direct3D 11. Hilo propio, sin bloquear la interfaz.
#pragma once
#include "common.h"

#include <d2d1_1.h>
#include <d3d11_1.h>
#include <dwrite.h>
#include <dxgi1_5.h>
#include <wincodec.h>

#include <future>
#include <thread>

#include "video_capture.h"

struct RenderOptions {
    bool vsync = false;         // false = presentar en cuanto llega (mínima latencia)
    bool nearest = false;       // escalado por píxel exacto en vez de bilineal
    bool fullRange = false;     // RGB/YUV de rango completo (0-255)
    bool showDiagnostics = false;
};

struct RenderStats {
    uint64_t received = 0;     // fotogramas que llegaron al buzón
    uint64_t presented = 0;    // fotogramas mostrados
    uint64_t replaced = 0;     // descartados por llegar uno más nuevo antes de mostrarse
    double uploadMsAvg = 0;    // copia CPU->GPU
    double pipelineMsAvg = 0;  // llegada del fotograma -> Present (medido)
    double pipelineMsMax = 0;
    bool tearingSupported = false;
    std::wstring adapter;
};

// Barra de volumen sobre el vídeo: panel abajo en el centro y pista por la que se arrastra
// (coordenadas de cliente en píxeles).
struct VolumeBarGeometry {
    RECT panel{};
    int trackLeft = 0, trackRight = 0, trackY = 0;
};
inline VolumeBarGeometry ComputeVolumeBar(int clientW, int clientH, float scale) {
    VolumeBarGeometry g;
    int bw = std::min(int(320 * scale), clientW - int(24 * scale));
    int bh = int(44 * scale);
    int x = (clientW - bw) / 2, y = clientH - bh - int(20 * scale);
    g.panel = {x, y, x + bw, y + bh};
    g.trackLeft = x + int(44 * scale);
    g.trackRight = x + bw - int(68 * scale);
    g.trackY = y + bh / 2;
    return g;
}

class Renderer : public FrameSink {
public:
    Renderer() = default;
    ~Renderer() override { Stop(); }

    bool Start(HWND hwnd, HRESULT* error);
    void Stop();

    void SetOptions(const RenderOptions& o);
    void SetStatusText(const std::wstring& text);   // mensaje central (p. ej. "Sin señal")
    void SetDiagnosticsText(const std::wstring& t); // panel de diagnóstico
    void SetHintText(const std::wstring& t, int ms);
    // Barra de volumen superpuesta (0..maxVolume) visible durante «ms».
    void SetVolumeOverlay(int volume, int maxVolume, bool muted, int ms);
    void NotifyResize();
    // Guarda en `folder` un PNG sin pérdida del último fotograma, a la resolución
    // nativa de la capturadora (sin escalar y sin textos superpuestos).
    void RequestScreenshot(const std::wstring& folder);
    RenderStats Stats();

    // FrameSink (lo llama el hilo de Media Foundation)
    void OnVideoSample(IMFSample* sample, const FrameFormat& format, int64_t arrivalQpc) override;
    void OnVideoError(HRESULT hr) override;
    void SetErrorCallback(HWND notify) { errorWindow_ = notify; }

private:
    struct Pending {
        ComPtr<IMFSample> sample;
        FrameFormat format;
        int64_t arrival = 0;
    };

    void Thread(std::promise<HRESULT>* ready);
    HRESULT CreateDevice();
    HRESULT CreateSizeDependent();
    void ReleaseSizeDependent();
    HRESULT EnsureSourceTextures(const FrameFormat& f);
    bool Upload(IMFSample* sample, const FrameFormat& f);
    void ConvertToIntermediate(const FrameFormat& f);
    void DrawFrame();
    void DrawOverlay();
    void DoScreenshot();

    HWND hwnd_ = nullptr;
    HWND errorWindow_ = nullptr;
    std::thread thread_;
    std::thread shotThread_;   // codifica y escribe el PNG sin frenar el render
    std::mutex shotMutex_;
    std::wstring shotFolder_;
    bool shotPending_ = false;
    std::atomic<bool> stop_{false};
    HANDLE frameEvent_ = nullptr, wakeEvent_ = nullptr;
    std::atomic<bool> resizePending_{true};

    std::mutex mailboxMutex_;
    Pending mailbox_;
    bool hasPending_ = false;

    std::mutex optMutex_;
    RenderOptions opts_;
    std::wstring statusText_, diagText_, hintText_;
    int64_t hintUntil_ = 0;
    int volValue_ = 0, volMax_ = 200;
    bool volMuted_ = false;
    int64_t volUntil_ = 0;

    // D3D
    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> ctx_;
    ComPtr<IDXGISwapChain2> swap_;
    HANDLE waitable_ = nullptr;
    bool tearing_ = false;
    UINT swapFlags_ = 0;
    ComPtr<ID3D11RenderTargetView> backRtv_;
    UINT backW_ = 0, backH_ = 0;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> psNv12_, psYuy2_, psRgb_, psPresent_;
    ComPtr<ID3D11SamplerState> samplerLinear_, samplerPoint_;
    ComPtr<ID3D11Buffer> constants_;

    // Texturas de entrada (dinámicas) y la intermedia RGB a resolución nativa.
    ComPtr<ID3D11Texture2D> texY_, texUV_;
    ComPtr<ID3D11ShaderResourceView> srvY_, srvUV_;
    ComPtr<ID3D11Texture2D> texRgb_;
    ComPtr<ID3D11RenderTargetView> rtvRgb_;
    ComPtr<ID3D11ShaderResourceView> srvRgb_;
    GUID srcSubtype_{};
    UINT srcW_ = 0, srcH_ = 0;
    bool hasContent_ = false;

    // Superposición (Direct2D / DirectWrite sobre el back buffer)
    ComPtr<ID2D1Factory1> d2dFactory_;
    ComPtr<ID2D1Device> d2dDevice_;
    ComPtr<ID2D1DeviceContext> d2dCtx_;
    ComPtr<ID2D1Bitmap1> d2dTarget_;
    ComPtr<IDWriteFactory> dwFactory_;
    ComPtr<IDWriteTextFormat> textDiag_, textStatus_, textHint_;
    ComPtr<ID2D1SolidColorBrush> brushText_, brushPanel_;

    // Estadísticas
    std::mutex statsMutex_;
    RenderStats stats_;
    double uploadAccum_ = 0, pipeAccum_ = 0, pipeMax_ = 0;
    uint64_t accumCount_ = 0;
    int64_t accumStart_ = 0;
};
