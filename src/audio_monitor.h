// Copyright (c) 2026 Dreex. Licencia MIT.
// Monitorización de audio de baja latencia: entrada WASAPI -> anillo -> salida WASAPI.
// Funciona igual en "Vídeo + audio" y en "Solo audio"; no toca nada de vídeo.
#pragma once
#include "common.h"

#include <audioclient.h>

#include <future>
#include <thread>

enum class AudioBufferProfile { Ultra = 0, Low = 1, Safe = 2 };

struct AudioSettings {
    std::wstring inputId;   // id de endpoint de captura (obligatorio)
    std::wstring outputId;  // vacío = salida predeterminada de Windows
    bool exclusive = false; // salida en modo exclusivo
    AudioBufferProfile profile = AudioBufferProfile::Low;
    int extraDelayMs = 0;   // retardo añadido a propósito para cuadrar con el vídeo
};

struct AudioStats {
    bool running = false;
    bool exclusive = false;
    bool lowLatencyCapture = false;  // IAudioClient3 en la entrada
    bool lowLatencyRender = false;   // IAudioClient3 en la salida
    bool systemResampler = false;    // Windows convierte la frecuencia
    UINT32 sampleRate = 0, inChannels = 0, outChannels = 0;
    double capturePeriodMs = 0, renderPeriodMs = 0, renderBufferMs = 0;
    double capturePacketMaxMs = 0;
    double ringFillMs = 0, targetFillMs = 0, renderQueuedMs = 0;
    double captureStreamLatencyMs = 0, renderStreamLatencyMs = 0;  // lo que declara el driver
    double driftPpm = 0;  // corrección aplicada para compensar relojes distintos
    uint64_t underruns = 0, trims = 0, overflows = 0, discontinuities = 0;
    float peak = 0;       // nivel de pico reciente (0..1)
};

enum class AudioFailure { None, InputLost, OutputLost, Other };

class AudioMonitor {
public:
    AudioMonitor() = default;
    ~AudioMonitor() { Stop(); }
    AudioMonitor(const AudioMonitor&) = delete;
    AudioMonitor& operator=(const AudioMonitor&) = delete;

    // Bloquea hasta que ambos extremos están abiertos (o falla). En caso de error,
    // FailedStage() indica si falló la entrada (0) o la salida (1) y `detail`
    // recibe un texto técnico para el registro.
    HRESULT Start(const AudioSettings& s, std::wstring* detail);
    int FailedStage() const { return failedStage_; }
    UINT32 SampleRate() const { return sampleRate_; }
    void Stop();
    bool IsRunning() const { return running_; }

    void SetVolume(float linearGain) { gainTarget_.store(linearGain); }
    void SetMuted(bool m) { muted_.store(m); }
    void SetExtraDelayMs(int ms);

    AudioFailure Failure() const { return failure_.load(); }
    HRESULT FailureCode() const { return failureHr_.load(); }
    AudioStats Stats();
    const AudioSettings& Settings() const { return settings_; }

private:
    struct Ring {
        std::vector<float> data;
        size_t capacity = 0;
        int channels = 1;
        std::atomic<uint64_t> writePos{0}, readPos{0};
        void Init(size_t frames, int ch);
        size_t Available() const { return size_t(writePos.load(std::memory_order_acquire) - readPos.load(std::memory_order_relaxed)); }
        size_t Free() const { return capacity - size_t(writePos.load(std::memory_order_relaxed) - readPos.load(std::memory_order_acquire)); }
        void Write(const float* src, size_t frames);
        const float* Frame(size_t offset) const {
            return &data[size_t((readPos.load(std::memory_order_relaxed) + offset) % capacity) * channels];
        }
        void Consume(size_t frames) { readPos.fetch_add(frames, std::memory_order_release); }
    };

    void AudioThread(std::promise<HRESULT>* ready, std::wstring* error);
    void CaptureLoop();
    void RenderLoop();
    void Produce(float* out, UINT32 frames);
    void RecomputeTarget();
    void Fail(AudioFailure f, HRESULT hr);

    AudioSettings settings_;
    std::thread thread_, captureThread_;
    std::atomic<bool> stop_{false};
    bool running_ = false;
    int failedStage_ = -1;

    // WASAPI (creados y usados en hilos MTA)
    ComPtr<IAudioClient> capClient_, renClient_;
    ComPtr<IAudioCaptureClient> capture_;
    ComPtr<IAudioRenderClient> render_;
    HANDLE capEvent_ = nullptr, renEvent_ = nullptr;
    std::vector<uint8_t> capFormat_, renFormat_;  // WAVEFORMATEX(+extensible)
    int capKind_ = 0, renKind_ = 0;               // tipo de muestra
    UINT32 sampleRate_ = 48000, inCh_ = 2, outCh_ = 2;
    UINT32 renBufferFrames_ = 0, renPeriodFrames_ = 0, capPeriodFrames_ = 0;
    std::atomic<UINT32> capPacketMax_{0};

    Ring ring_;
    std::vector<float> capScratch_, renScratch_;

    // Estado del consumidor (hilo de salida)
    double readFrac_ = 0;
    double fillEma_ = -1;
    double ratio_ = 1.0;
    bool priming_ = true;
    UINT32 rampPos_ = 0;
    float gain_ = 0;
    std::atomic<double> targetFrames_{0};
    std::atomic<int> extraDelayMs_{0};
    std::atomic<float> gainTarget_{1.0f};
    std::atomic<bool> muted_{false};
    float peakHold_ = 0;

    std::atomic<AudioFailure> failure_{AudioFailure::None};
    std::atomic<HRESULT> failureHr_{S_OK};

    std::mutex statsMutex_;
    AudioStats stats_;
};

// Curva perceptual para el control de volumen (0..100 -> ganancia lineal).
float VolumeToGain(int volume0to100);
