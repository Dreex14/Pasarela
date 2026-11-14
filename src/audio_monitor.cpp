// Copyright (c) 2026 Dreex. Licencia MIT.
//
// Diseño:
//  - Hilo de captura (evento WASAPI) convierte cada paquete a float y lo mete en
//    un anillo SPSC sin bloqueos.
//  - Hilo de salida (evento WASAPI, MMCSS "Pro Audio") saca del anillo lo justo.
//  - La entrada y la salida tienen relojes distintos: un pequeño corrector de
//    velocidad (±0,3 % como máximo, interpolación lineal) mantiene el anillo en su
//    nivel objetivo, de modo que la latencia no crece con el tiempo ni se vacía.
//  - Si algo se atasca (más de 40 ms de exceso) se descarta lo viejo de golpe; si
//    se vacía, se vuelve a cebar hasta el nivel objetivo. Ambos casos se cuentan.
#include "audio_monitor.h"

#include <avrt.h>
#include <mmdeviceapi.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "log.h"

namespace {

// GUIDs de subformato (definidos aquí para no depender de ksmedia/initguid).
const GUID kSubtypePcm = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
const GUID kSubtypeFloat = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

enum SampleKind { kFloat32 = 0, kInt16 = 1, kInt24 = 2, kInt32 = 3, kUnknown = 9 };

int KindOf(const WAVEFORMATEX* wf) {
    bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool isPcm = wf->wFormatTag == WAVE_FORMAT_PCM;
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22) {
        auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf);
        isFloat = ext->SubFormat == kSubtypeFloat;
        isPcm = ext->SubFormat == kSubtypePcm;
    }
    if (isFloat && wf->wBitsPerSample == 32) return kFloat32;
    if (isPcm && wf->wBitsPerSample == 16) return kInt16;
    if (isPcm && wf->wBitsPerSample == 24) return kInt24;
    if (isPcm && wf->wBitsPerSample == 32) return kInt32;
    return kUnknown;
}

std::vector<uint8_t> CopyFormat(const WAVEFORMATEX* wf) {
    size_t size = sizeof(WAVEFORMATEX) + wf->cbSize;
    std::vector<uint8_t> v(size);
    std::memcpy(v.data(), wf, size);
    return v;
}

std::vector<uint8_t> MakeFormat(int kind, UINT32 rate, UINT32 channels, DWORD channelMask) {
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)channels;
    f.Format.nSamplesPerSec = rate;
    WORD bits = kind == kInt16 ? 16 : kind == kInt24 ? 24 : 32;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = WORD(channels * bits / 8);
    f.Format.nAvgBytesPerSec = rate * f.Format.nBlockAlign;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = kind == kInt32 ? 24 : bits;
    if (!channelMask) channelMask = channels == 1 ? SPEAKER_FRONT_CENTER : (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT);
    f.dwChannelMask = channelMask;
    f.SubFormat = kind == kFloat32 ? kSubtypeFloat : kSubtypePcm;
    std::vector<uint8_t> v(sizeof(f));
    std::memcpy(v.data(), &f, sizeof(f));
    return v;
}

DWORD ChannelMaskOf(const WAVEFORMATEX* wf) {
    if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE && wf->cbSize >= 22)
        return reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(wf)->dwChannelMask;
    return 0;
}

void ToFloat(const BYTE* src, int kind, size_t samples, float* dst) {
    switch (kind) {
        case kFloat32: std::memcpy(dst, src, samples * sizeof(float)); break;
        case kInt16: {
            auto* s = reinterpret_cast<const int16_t*>(src);
            for (size_t i = 0; i < samples; ++i) dst[i] = s[i] * (1.0f / 32768.0f);
            break;
        }
        case kInt24:
            for (size_t i = 0; i < samples; ++i) {
                int32_t v = (int32_t(src[3 * i]) << 8) | (int32_t(src[3 * i + 1]) << 16) | (int32_t(src[3 * i + 2]) << 24);
                dst[i] = float(v) * (1.0f / 2147483648.0f);
            }
            break;
        case kInt32: {
            auto* s = reinterpret_cast<const int32_t*>(src);
            for (size_t i = 0; i < samples; ++i) dst[i] = float(s[i]) * (1.0f / 2147483648.0f);
            break;
        }
        default: std::memset(dst, 0, samples * sizeof(float)); break;
    }
}

void FromFloat(const float* src, int kind, size_t samples, BYTE* dst) {
    switch (kind) {
        case kFloat32: std::memcpy(dst, src, samples * sizeof(float)); break;
        case kInt16: {
            auto* d = reinterpret_cast<int16_t*>(dst);
            for (size_t i = 0; i < samples; ++i) d[i] = int16_t(std::lrintf(Clamp(src[i], -1.0f, 1.0f) * 32767.0f));
            break;
        }
        case kInt24:
            for (size_t i = 0; i < samples; ++i) {
                int32_t v = int32_t(std::lrint(double(Clamp(src[i], -1.0f, 1.0f)) * 8388607.0));
                dst[3 * i] = BYTE(v);
                dst[3 * i + 1] = BYTE(v >> 8);
                dst[3 * i + 2] = BYTE(v >> 16);
            }
            break;
        case kInt32: {
            auto* d = reinterpret_cast<int32_t*>(dst);
            for (size_t i = 0; i < samples; ++i)
                d[i] = int32_t(std::lrint(double(Clamp(src[i], -1.0f, 1.0f)) * 2147483392.0));
            break;
        }
        default: std::memset(dst, 0, samples * 4); break;
    }
}

const REFERENCE_TIME kHnsPerSec = 10000000;

struct ComInit {
    HRESULT hr;
    ComInit() : hr(CoInitializeEx(nullptr, COINIT_MULTITHREADED)) {}
    ~ComInit() { if (SUCCEEDED(hr)) CoUninitialize(); }
};

// Inicializa un cliente compartido intentando el período mínimo del motor
// (IAudioClient3). Si el driver no lo admite, se usa el período estándar.
HRESULT InitShared(IMMDevice* dev, const WAVEFORMATEX* fmt, DWORD extraFlags, ComPtr<IAudioClient>& client,
                   bool& lowLatency, UINT32& periodFrames) {
    lowLatency = false;
    HRESULT hr;
    if (!(extraFlags & AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM)) {
        ComPtr<IAudioClient3> ac3;
        if (SUCCEEDED(dev->Activate(__uuidof(IAudioClient3), CLSCTX_ALL, nullptr, &ac3))) {
            UINT32 def = 0, fund = 0, minP = 0, maxP = 0;
            if (SUCCEEDED(ac3->GetSharedModeEnginePeriod(fmt, &def, &fund, &minP, &maxP)) && minP && minP < def) {
                hr = ac3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extraFlags, minP, fmt, nullptr);
                if (SUCCEEDED(hr)) {
                    client = ac3;
                    lowLatency = true;
                    periodFrames = minP;
                    return S_OK;
                }
                Log(L"IAudioClient3 bajo período no disponible (%s); se usa el estándar", HrHex(hr).c_str());
            }
        }
    }
    client.Reset();
    hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client);
    if (FAILED(hr)) return hr;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK | extraFlags, 0, 0, fmt, nullptr);
    if (FAILED(hr)) return hr;
    REFERENCE_TIME defPeriod = 0, minPeriod = 0;
    client->GetDevicePeriod(&defPeriod, &minPeriod);
    periodFrames = UINT32((defPeriod * fmt->nSamplesPerSec + kHnsPerSec / 2) / kHnsPerSec);
    return S_OK;
}

}  // namespace

float VolumeToGain(int v) {
    float x = Clamp(v, 0, 100) / 100.0f;
    return x * x;
}

void AudioMonitor::Ring::Init(size_t frames, int ch) {
    capacity = frames;
    channels = ch;
    data.assign(frames * ch, 0.0f);
    writePos = 0;
    readPos = 0;
}

void AudioMonitor::Ring::Write(const float* src, size_t frames) {
    uint64_t w = writePos.load(std::memory_order_relaxed);
    size_t start = size_t(w % capacity);
    size_t first = std::min(frames, capacity - start);
    std::memcpy(&data[start * channels], src, first * channels * sizeof(float));
    if (frames > first) std::memcpy(&data[0], src + first * channels, (frames - first) * channels * sizeof(float));
    writePos.store(w + frames, std::memory_order_release);
}

HRESULT AudioMonitor::Start(const AudioSettings& s, std::wstring* detail) {
    Stop();
    settings_ = s;
    stop_ = false;
    failure_ = AudioFailure::None;
    failureHr_ = S_OK;
    failedStage_ = -1;
    extraDelayMs_ = s.extraDelayMs;
    priming_ = true;
    fillEma_ = -1;
    readFrac_ = 0;
    ratio_ = 1.0;
    gain_ = 0;
    capPacketMax_ = 0;
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = AudioStats{};
    }
    std::promise<HRESULT> ready;
    auto fut = ready.get_future();
    std::wstring err;
    thread_ = std::thread(&AudioMonitor::AudioThread, this, &ready, &err);
    HRESULT hr = fut.get();
    if (FAILED(hr)) {
        if (thread_.joinable()) thread_.join();
        if (detail) *detail = err;
        Log(L"Audio: no se pudo iniciar: %s", err.c_str());
        return hr;
    }
    running_ = true;
    return S_OK;
}

void AudioMonitor::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    if (renEvent_) SetEvent(renEvent_);
    if (capEvent_) SetEvent(capEvent_);
    thread_.join();
    running_ = false;
    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.running = false;
}

void AudioMonitor::SetExtraDelayMs(int ms) {
    extraDelayMs_ = Clamp(ms, 0, 300);
    RecomputeTarget();
}

void AudioMonitor::RecomputeTarget() {
    double rate = sampleRate_;
    if (rate <= 0) return;
    double capMs = std::max<double>(capPeriodFrames_, capPacketMax_.load()) * 1000.0 / rate;
    double renChunkMs = renPeriodFrames_ * 1000.0 / rate;
    double margin = settings_.profile == AudioBufferProfile::Ultra ? 1.0
                    : settings_.profile == AudioBufferProfile::Low ? 4.0
                                                                   : 12.0;
    double targetMs = capMs + renChunkMs * 0.5 + margin + extraDelayMs_.load();
    targetFrames_ = targetMs * rate / 1000.0;
}

void AudioMonitor::Fail(AudioFailure f, HRESULT hr) {
    AudioFailure expected = AudioFailure::None;
    if (failure_.compare_exchange_strong(expected, f)) {
        failureHr_ = hr;
        Log(L"Audio: fallo %d (%s)", int(f), HrHex(hr).c_str());
    }
}

void AudioMonitor::AudioThread(std::promise<HRESULT>* ready, std::wstring* error) {
    ComInit com;
    HRESULT hr = S_OK;
    auto fail = [&](int stage, const wchar_t* what, HRESULT code) {
        failedStage_ = stage;
        *error = Fmt(L"%s: %s", what, HrHex(code).c_str());
        capture_.Reset();
        render_.Reset();
        capClient_.Reset();
        renClient_.Reset();
        if (capEvent_) CloseHandle(capEvent_), capEvent_ = nullptr;
        if (renEvent_) CloseHandle(renEvent_), renEvent_ = nullptr;
        ready->set_value(code);
    };

    ComPtr<IMMDeviceEnumerator> en;
    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return fail(0, L"MMDeviceEnumerator", hr);

    // --- Entrada -------------------------------------------------------------
    ComPtr<IMMDevice> inDev;
    hr = en->GetDevice(settings_.inputId.c_str(), &inDev);
    if (FAILED(hr)) return fail(0, L"GetDevice(entrada)", hr);
    {
        ComPtr<IAudioClient> probe;
        hr = inDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe);
        if (FAILED(hr)) return fail(0, L"Activate(entrada)", hr);
        WAVEFORMATEX* mix = nullptr;
        hr = probe->GetMixFormat(&mix);
        if (FAILED(hr)) return fail(0, L"GetMixFormat(entrada)", hr);
        capFormat_ = CopyFormat(mix);
        CoTaskMemFree(mix);
    }
    auto* capFmt = reinterpret_cast<WAVEFORMATEX*>(capFormat_.data());
    capKind_ = KindOf(capFmt);
    if (capKind_ == kUnknown) return fail(0, L"Formato de entrada no admitido", AUDCLNT_E_UNSUPPORTED_FORMAT);
    sampleRate_ = capFmt->nSamplesPerSec;
    inCh_ = capFmt->nChannels;
    bool capLowLatency = false;
    hr = InitShared(inDev.Get(), capFmt, 0, capClient_, capLowLatency, capPeriodFrames_);
    if (FAILED(hr)) return fail(0, L"Initialize(entrada)", hr);
    hr = capClient_->GetService(IID_PPV_ARGS(&capture_));
    if (FAILED(hr)) return fail(0, L"GetService(captura)", hr);

    // --- Salida --------------------------------------------------------------
    ComPtr<IMMDevice> outDev;
    if (settings_.outputId.empty())
        hr = en->GetDefaultAudioEndpoint(eRender, eConsole, &outDev);
    else
        hr = en->GetDevice(settings_.outputId.c_str(), &outDev);
    if (FAILED(hr)) return fail(1, L"GetDevice(salida)", hr);

    std::vector<uint8_t> outMix;
    {
        ComPtr<IAudioClient> probe;
        hr = outDev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &probe);
        if (FAILED(hr)) return fail(1, L"Activate(salida)", hr);
        WAVEFORMATEX* mix = nullptr;
        hr = probe->GetMixFormat(&mix);
        if (FAILED(hr)) return fail(1, L"GetMixFormat(salida)", hr);
        outMix = CopyFormat(mix);
        CoTaskMemFree(mix);
    }
    auto* mixOut = reinterpret_cast<WAVEFORMATEX*>(outMix.data());
    bool renLowLatency = false, systemResampler = false;

    if (mixOut->nSamplesPerSec == sampleRate_ && KindOf(mixOut) != kUnknown) {
        renFormat_ = outMix;
        hr = InitShared(outDev.Get(), mixOut, 0, renClient_, renLowLatency, renPeriodFrames_);
    } else {
        // Frecuencias distintas: Windows convierte (añade algo de latencia).
        renFormat_ = MakeFormat(kFloat32, sampleRate_, mixOut->nChannels, ChannelMaskOf(mixOut));
        systemResampler = true;
        hr = InitShared(outDev.Get(), reinterpret_cast<WAVEFORMATEX*>(renFormat_.data()),
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, renClient_,
                        renLowLatency, renPeriodFrames_);
    }
    if (FAILED(hr)) return fail(1, L"Initialize(salida)", hr);
    auto* renFmt = reinterpret_cast<WAVEFORMATEX*>(renFormat_.data());
    renKind_ = KindOf(renFmt);
    outCh_ = renFmt->nChannels;
    renClient_->GetBufferSize(&renBufferFrames_);
    hr = renClient_->GetService(IID_PPV_ARGS(&render_));
    if (FAILED(hr)) return fail(1, L"GetService(render)", hr);

    // --- Preparación ---------------------------------------------------------
    ring_.Init(sampleRate_, int(inCh_));  // 1 s de capacidad; en régimen solo se usan unos ms
    capScratch_.assign(size_t(sampleRate_) * inCh_ / 2, 0.0f);
    renScratch_.assign(size_t(renBufferFrames_) * outCh_, 0.0f);
    RecomputeTarget();

    capEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    renEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    capClient_->SetEventHandle(capEvent_);
    renClient_->SetEventHandle(renEvent_);

    REFERENCE_TIME capLat = 0, renLat = 0;
    capClient_->GetStreamLatency(&capLat);
    renClient_->GetStreamLatency(&renLat);

    hr = capClient_->Start();
    if (FAILED(hr)) return fail(0, L"Start(entrada)", hr);
    hr = renClient_->Start();
    if (FAILED(hr)) {
        capClient_->Stop();
        return fail(1, L"Start(salida)", hr);
    }

    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.running = true;
        stats_.lowLatencyCapture = capLowLatency;
        stats_.lowLatencyRender = renLowLatency;
        stats_.systemResampler = systemResampler;
        stats_.sampleRate = sampleRate_;
        stats_.inChannels = inCh_;
        stats_.outChannels = outCh_;
        stats_.capturePeriodMs = capPeriodFrames_ * 1000.0 / sampleRate_;
        stats_.renderPeriodMs = renPeriodFrames_ * 1000.0 / sampleRate_;
        stats_.renderBufferMs = renBufferFrames_ * 1000.0 / sampleRate_;
        stats_.captureStreamLatencyMs = capLat / 10000.0;
        stats_.renderStreamLatencyMs = renLat / 10000.0;
    }
    Log(L"Audio: %u Hz, %u->%u canales, entrada %s período %.2f ms, salida %s período %.2f ms búfer %.2f ms%s",
        sampleRate_, inCh_, outCh_, capLowLatency ? L"IAudioClient3" : L"estándar",
        capPeriodFrames_ * 1000.0 / sampleRate_,
        renLowLatency ? L"baja latencia" : L"estándar", renPeriodFrames_ * 1000.0 / sampleRate_,
        renBufferFrames_ * 1000.0 / sampleRate_, systemResampler ? L" (conversión de frecuencia de Windows)" : L"");

    ready->set_value(S_OK);

    captureThread_ = std::thread(&AudioMonitor::CaptureLoop, this);
    RenderLoop();
    stop_ = true;
    if (captureThread_.joinable()) captureThread_.join();

    capClient_->Stop();
    renClient_->Stop();
    capture_.Reset();
    render_.Reset();
    capClient_.Reset();
    renClient_.Reset();
    CloseHandle(capEvent_);
    CloseHandle(renEvent_);
    capEvent_ = renEvent_ = nullptr;
}

void AudioMonitor::CaptureLoop() {
    ComInit com;
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    const UINT32 blockAlign = reinterpret_cast<WAVEFORMATEX*>(capFormat_.data())->nBlockAlign;
    (void)blockAlign;
    while (!stop_) {
        WaitForSingleObject(capEvent_, 200);
        if (stop_) break;
        UINT32 packet = 0;
        HRESULT hr = capture_->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;
            hr = capture_->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr)) break;
            if (frames > capPacketMax_.load()) {
                capPacketMax_ = frames;
                RecomputeTarget();
            }
            size_t samples = size_t(frames) * inCh_;
            if (capScratch_.size() < samples) capScratch_.resize(samples);
            if (flags & AUDCLNT_BUFFERFLAGS_SILENT)
                std::fill_n(capScratch_.data(), samples, 0.0f);
            else
                ToFloat(data, capKind_, samples, capScratch_.data());
            capture_->ReleaseBuffer(frames);
            if (flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.discontinuities++;
            }
            size_t free = ring_.Free();
            size_t toWrite = std::min<size_t>(frames, free);
            if (toWrite < frames) {
                std::lock_guard<std::mutex> lock(statsMutex_);
                stats_.overflows++;
            }
            if (toWrite) ring_.Write(capScratch_.data(), toWrite);
            hr = capture_->GetNextPacketSize(&packet);
        }
        if (FAILED(hr)) {
            Fail(hr == AUDCLNT_E_DEVICE_INVALIDATED ? AudioFailure::InputLost : AudioFailure::Other, hr);
            break;
        }
    }
    if (task) AvRevertMmThreadCharacteristics(task);
}

void AudioMonitor::Produce(float* out, UINT32 frames) {
    const double rate = sampleRate_;
    size_t avail = ring_.Available();
    const double target = targetFrames_.load();
    if (fillEma_ < 0) fillEma_ = double(avail);
    fillEma_ += (double(avail) - fillEma_) * 0.05;

    std::fill_n(out, size_t(frames) * outCh_, 0.0f);
    if (priming_) {
        if (double(avail) < target) return;
        priming_ = false;
        rampPos_ = 0;
        readFrac_ = 0;
        fillEma_ = double(avail);
    }

    // Exceso grande (atasco puntual): se tira lo viejo para recuperar la respuesta inmediata.
    double hardLimit = target + std::max(rate * 0.040, target);
    if (double(avail) > hardLimit) {
        size_t drop = avail - size_t(target);
        ring_.Consume(drop);
        avail -= drop;
        fillEma_ = double(avail);
        readFrac_ = 0;
        rampPos_ = 0;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.trims++;
    }

    // Corrección suave de deriva entre relojes: 10 ms de error -> 0,2 % de velocidad.
    double errMs = (fillEma_ - target) * 1000.0 / rate;
    ratio_ = 1.0 + Clamp(errMs * 0.0002, -0.003, 0.003);

    const float gainTarget = muted_.load() ? 0.0f : gainTarget_.load();
    const UINT32 inCh = inCh_, outCh = outCh_;
    const UINT32 rampLen = 96;
    float peak = 0;
    bool underrun = false;
    UINT32 i = 0;
    for (; i < frames; ++i) {
        size_t i0 = size_t(readFrac_);
        if (i0 + 1 >= avail) {
            underrun = true;
            break;
        }
        const float* a = ring_.Frame(i0);
        const float* b = ring_.Frame(i0 + 1);
        const float f = float(readFrac_ - double(i0));
        gain_ += (gainTarget - gain_) * 0.0015f;
        float g = gain_;
        if (rampPos_ < rampLen) g *= float(rampPos_++) / rampLen;
        float* o = out + size_t(i) * outCh;
        if (inCh == 1) {
            float v = a[0] + (b[0] - a[0]) * f;
            peak = std::max(peak, std::fabs(v));
            o[0] = v * g;
            if (outCh > 1) o[1] = v * g;
        } else if (outCh == 1) {
            float l = a[0] + (b[0] - a[0]) * f, r = a[1] + (b[1] - a[1]) * f;
            float v = 0.5f * (l + r);
            peak = std::max(peak, std::fabs(v));
            o[0] = v * g;
        } else {
            UINT32 n = std::min(inCh, outCh);
            for (UINT32 c = 0; c < n; ++c) {
                float v = a[c] + (b[c] - a[c]) * f;
                peak = std::max(peak, std::fabs(v));
                o[c] = v * g;
            }
        }
        readFrac_ += ratio_;
    }
    if (underrun) {
        size_t used = std::min<size_t>(size_t(readFrac_), avail);
        ring_.Consume(used);
        readFrac_ = 0;
        priming_ = true;
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_.underruns++;
    } else {
        size_t used = size_t(readFrac_);
        ring_.Consume(used);
        readFrac_ -= double(used);
    }
    peakHold_ = std::max(peak, peakHold_ * 0.9f);
}

void AudioMonitor::RenderLoop() {
    DWORD taskIndex = 0;
    HANDLE task = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    const UINT32 bytesPerFrame = reinterpret_cast<WAVEFORMATEX*>(renFormat_.data())->nBlockAlign;
    (void)bytesPerFrame;
    int64_t lastStats = QpcNow();
    while (!stop_) {
        DWORD w = WaitForSingleObject(renEvent_, 200);
        if (stop_) break;
        if (w != WAIT_OBJECT_0) continue;
        UINT32 frames = 0, padding = 0;
        HRESULT hr = S_OK;
        hr = renClient_->GetCurrentPadding(&padding);
        if (SUCCEEDED(hr)) {
            double mult = settings_.profile == AudioBufferProfile::Ultra ? 1.5
                          : settings_.profile == AudioBufferProfile::Low ? 2.0
                                                                         : 3.0;
            UINT32 targetPad = std::min<UINT32>(renBufferFrames_, UINT32(renPeriodFrames_ * mult + 0.5));
            frames = targetPad > padding ? targetPad - padding : 0;
        }
        if (SUCCEEDED(hr) && frames) {
            BYTE* p = nullptr;
            hr = render_->GetBuffer(frames, &p);
            if (SUCCEEDED(hr)) {
                Produce(renScratch_.data(), frames);
                FromFloat(renScratch_.data(), renKind_, size_t(frames) * outCh_, p);
                hr = render_->ReleaseBuffer(frames, 0);
            }
        }
        if (FAILED(hr)) {
            Fail(hr == AUDCLNT_E_DEVICE_INVALIDATED ? AudioFailure::OutputLost : AudioFailure::Other, hr);
            break;
        }
        int64_t now = QpcNow();
        if (QpcToMs(now - lastStats) > 200) {
            lastStats = now;
            std::lock_guard<std::mutex> lock(statsMutex_);
            stats_.ringFillMs = fillEma_ * 1000.0 / sampleRate_;
            stats_.targetFillMs = targetFrames_.load() * 1000.0 / sampleRate_;
            stats_.renderQueuedMs = padding * 1000.0 / sampleRate_;
            stats_.capturePacketMaxMs = capPacketMax_.load() * 1000.0 / sampleRate_;
            stats_.driftPpm = (ratio_ - 1.0) * 1e6;
            stats_.peak = peakHold_;
        }
    }
    if (task) AvRevertMmThreadCharacteristics(task);
}

AudioStats AudioMonitor::Stats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}
