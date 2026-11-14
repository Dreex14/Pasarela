// Copyright (c) 2026 Dreex. Licencia MIT.
// Captura de vídeo con Media Foundation (Source Reader asíncrono, MF_LOW_LATENCY).
#pragma once
#include "common.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mferror.h>
#include <mfreadwrite.h>

#include "devices.h"

// Formato de los fotogramas que realmente llegan al renderizador.
struct FrameFormat {
    GUID subtype{};        // NV12, YUY2, RGB32 o ARGB32
    GUID sourceSubtype{};  // lo que envía la capturadora (p. ej. MJPEG)
    UINT32 width = 0, height = 0;
    LONG stride = 0;       // bytes por línea (negativo = de abajo arriba)
    double fps = 0;
    bool decoded = false;  // se ha insertado un decodificador/conversor
};

class FrameSink {
public:
    virtual ~FrameSink() = default;
    virtual void OnVideoSample(IMFSample* sample, const FrameFormat& format, int64_t arrivalQpc) = 0;
    virtual void OnVideoError(HRESULT hr) = 0;
};

struct CaptureStats {
    uint64_t frames = 0;        // fotogramas entregados por el driver
    uint64_t gapFrames = 0;     // huecos detectados por marca de tiempo (perdidos antes de llegar)
    double lastArrivalAgeMs = 0;
};

class VideoCapture final : public IMFSourceReaderCallback {
public:
    static VideoCapture* Create() { return new VideoCapture(); }

    HRESULT Start(const std::wstring& symlink, const VideoFormat& wanted, FrameSink* sink);
    void Stop();
    FrameFormat Format();
    CaptureStats Stats();

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override;
    STDMETHODIMP_(ULONG) AddRef() override { return ++ref_; }
    STDMETHODIMP_(ULONG) Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    // IMFSourceReaderCallback
    STDMETHODIMP OnReadSample(HRESULT hr, DWORD stream, DWORD flags, LONGLONG timestamp, IMFSample* sample) override;
    // Flush cancela la petición pendiente sin llamar a OnReadSample.
    STDMETHODIMP OnFlush(DWORD) override {
        SetEvent(idleEvent_);
        return S_OK;
    }
    STDMETHODIMP OnEvent(DWORD, IMFMediaEvent*) override { return S_OK; }

private:
    VideoCapture();
    ~VideoCapture();
    HRESULT ReadCurrentFormat();

    std::atomic<ULONG> ref_{1};
    std::mutex mutex_;
    ComPtr<IMFMediaSource> source_;
    ComPtr<IMFSourceReader> reader_;
    FrameSink* sink_ = nullptr;
    FrameFormat format_;
    GUID sourceSubtype_{};
    std::atomic<bool> stopping_{false};
    std::atomic<bool> pending_{false};
    HANDLE idleEvent_ = nullptr;

    std::atomic<uint64_t> frames_{0}, gapFrames_{0};
    std::atomic<int64_t> lastArrival_{0};
    LONGLONG lastTimestamp_ = -1;
};
