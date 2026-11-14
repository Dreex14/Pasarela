// Prueba sin interfaz contra el hardware real (consola de texto).
// Uso: hw_probe.exe [segundos_video] [segundos_audio]
// Copyright (c) 2026 Dreex. Licencia MIT.
#include <cstdio>

#include "../src/audio_monitor.h"
#include "../src/devices.h"
#include "../src/video_capture.h"

namespace {

struct CountingSink : FrameSink {
    std::atomic<uint64_t> frames{0};
    std::atomic<HRESULT> error{S_OK};
    FrameFormat last;
    std::mutex m;
    double copyMsTotal = 0;
    void OnVideoSample(IMFSample* s, const FrameFormat& f, int64_t) override {
        // Simula el trabajo real del renderizador: bloquear el buffer y leerlo entero.
        int64_t t0 = QpcNow();
        ComPtr<IMFMediaBuffer> b;
        if (SUCCEEDED(s->ConvertToContiguousBuffer(&b))) {
            BYTE* p = nullptr;
            DWORD max = 0, len = 0;
            if (SUCCEEDED(b->Lock(&p, &max, &len))) {
                volatile uint64_t sum = 0;
                for (DWORD i = 0; i < len; i += 4096) sum += p[i];
                b->Unlock();
            }
        }
        std::lock_guard<std::mutex> l(m);
        copyMsTotal += QpcToMs(QpcNow() - t0);
        last = f;
        frames++;
    }
    void OnVideoError(HRESULT hr) override { error = hr; }
};

void Print(const std::wstring& s) {
    std::string u = ToUtf8(s);
    fwrite(u.data(), 1, u.size(), stdout);
    fflush(stdout);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    int videoSecs = argc > 1 ? _wtoi(argv[1]) : 5;
    int audioSecs = argc > 2 ? _wtoi(argv[2]) : 5;
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);

    auto videos = EnumerateVideoDevices();
    auto inputs = EnumerateAudioEndpoints(eCapture);
    auto outputs = EnumerateAudioEndpoints(eRender);
    Print(Fmt(L"Capturadoras: %zu\n", videos.size()));
    for (auto& v : videos) Print(L"  - " + v.name + L"  contenedor " + v.containerId + L"\n");
    Print(Fmt(L"Entradas de audio: %zu\n", inputs.size()));
    for (auto& a : inputs) Print(L"  - " + a.name + (a.isDefault ? L"  [predet.]" : L"") + L"\n");
    Print(Fmt(L"Salidas de audio: %zu\n", outputs.size()));
    for (auto& a : outputs) Print(L"  - " + a.name + (a.isDefault ? L"  [predet.]" : L"") + L"\n");
    if (videos.empty()) return 2;

    const VideoDevice& dev = videos[0];
    int assoc = FindAssociatedAudioInput(dev, inputs);
    Print(L"\nAudio asociado a \"" + dev.name + L"\": " + (assoc >= 0 ? inputs[assoc].name : L"(ninguno)") + L"\n");

    std::vector<VideoFormat> formats;
    HRESULT hr = EnumerateVideoFormats(dev.symlink, formats);
    Print(Fmt(L"\nModos declarados (%s): %zu\n", HrHex(hr).c_str(), formats.size()));
    for (auto& f : formats) Print(Fmt(L"  %ux%u @ %.3f  %s\n", f.width, f.height, f.Fps(), SubtypeName(f.subtype)));
    int a = ChooseAutoFormat(formats, 1920, 1080);
    if (a >= 0)
        Print(Fmt(L"Automático: %ux%u @ %.2f %s\n", formats[a].width, formats[a].height, formats[a].Fps(),
                  SubtypeName(formats[a].subtype)));

    if (a >= 0 && videoSecs > 0) {
        CountingSink sink;
        VideoCapture* cap = VideoCapture::Create();
        int64_t t0 = QpcNow();
        hr = cap->Start(dev.symlink, formats[a], &sink);
        Print(Fmt(L"\nVídeo Start: %s (%.0f ms)\n", HrHex(hr).c_str(), QpcToMs(QpcNow() - t0)));
        if (SUCCEEDED(hr)) {
            int64_t first = 0;
            for (int i = 0; i < videoSecs * 100; ++i) {
                Sleep(10);
                if (!first && sink.frames) first = QpcNow();
            }
            if (first) Print(Fmt(L"Primer fotograma tras %.0f ms\n", QpcToMs(first - t0)));
            uint64_t n0 = sink.frames;
            Sleep(1000);
            uint64_t n1 = sink.frames;
            CaptureStats cs = cap->Stats();
            FrameFormat f = cap->Format();
            Print(Fmt(L"Formato entregado: %ux%u %s (origen %s, decodificado %s) stride %ld\n", f.width, f.height,
                      SubtypeName(f.subtype), SubtypeName(f.sourceSubtype), f.decoded ? L"sí" : L"no", f.stride));
            Print(Fmt(L"FPS medidos (último segundo): %llu · total %llu · huecos por marca de tiempo %llu · error %s\n",
                      (unsigned long long)(n1 - n0), (unsigned long long)cs.frames,
                      (unsigned long long)cs.gapFrames, HrHex(sink.error.load()).c_str()));
            Print(Fmt(L"Lectura de buffer media: %.3f ms\n", cs.frames ? sink.copyMsTotal / cs.frames : 0.0));
            cap->Stop();
        }
        cap->Release();
    }

    if (assoc >= 0 && audioSecs > 0) {
        for (int profile = 0; profile < 3; ++profile) {
            AudioMonitor mon;
            AudioSettings as;
            as.inputId = inputs[assoc].id;
            as.profile = AudioBufferProfile(profile);
            std::wstring detail;
            hr = mon.Start(as, &detail);
            Print(Fmt(L"\nAudio perfil %d Start: %s %s\n", profile, HrHex(hr).c_str(), detail.c_str()));
            if (FAILED(hr)) continue;
            mon.SetVolume(0.0f);  // la prueba no hace sonar nada
            Sleep(audioSecs * 1000);
            AudioStats s = mon.Stats();
            Print(Fmt(L"  %u Hz %u->%u canales · entrada %s %.2f ms (paquete máx %.2f) · salida %s %.2f ms búfer %.2f\n",
                      s.sampleRate, s.inChannels, s.outChannels, s.lowLatencyCapture ? L"IAudioClient3" : L"estándar",
                      s.capturePeriodMs, s.capturePacketMaxMs, s.lowLatencyRender ? L"IAudioClient3" : L"estándar",
                      s.renderPeriodMs, s.renderBufferMs));
            Print(Fmt(L"  colchón %.2f ms (objetivo %.2f) · cola salida %.2f ms · deriva %+.0f ppm · latencias driver "
                      L"%.2f/%.2f ms\n",
                      s.ringFillMs, s.targetFillMs, s.renderQueuedMs, s.driftPpm, s.captureStreamLatencyMs,
                      s.renderStreamLatencyMs));
            Print(Fmt(L"  vacíos %llu · recortes %llu · desbordes %llu · discontinuidades %llu · pico %.3f · fallo %d\n",
                      (unsigned long long)s.underruns, (unsigned long long)s.trims, (unsigned long long)s.overflows,
                      (unsigned long long)s.discontinuities, s.peak, int(mon.Failure())));
            mon.Stop();
        }
    }
    MFShutdown();
    CoUninitialize();
    return 0;
}
