// Copyright (c) 2026 Dreex. Licencia MIT.
//
// - Se elige el formato directamente en la fuente (IMFMediaTypeHandler), así el
//   driver entrega exactamente el modo declarado que el usuario seleccionó.
// - Formatos sin compresión (NV12/YUY2/RGB32) pasan sin conversores
//   (MF_READWRITE_DISABLE_CONVERTERS): cero etapas ocultas.
// - MJPEG u otros: el Source Reader inserta el decodificador y entrega NV12.
// - Lectura asíncrona: siempre hay una sola petición en vuelo; el fotograma se
//   pasa al renderizador, que se queda solo con el más reciente.
#include "video_capture.h"

#include <cmath>

#include "log.h"

VideoCapture::VideoCapture() { idleEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr); }

VideoCapture::~VideoCapture() {
    if (idleEvent_) CloseHandle(idleEvent_);
}

STDMETHODIMP VideoCapture::QueryInterface(REFIID riid, void** ppv) {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IMFSourceReaderCallback)) {
        *ppv = static_cast<IMFSourceReaderCallback*>(this);
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}

HRESULT VideoCapture::Start(const std::wstring& symlink, const VideoFormat& wanted, FrameSink* sink) {
    sink_ = sink;
    stopping_ = false;
    frames_ = 0;
    gapFrames_ = 0;
    lastTimestamp_ = -1;
    lastArrival_ = QpcNow();
    ResetEvent(idleEvent_);

    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (FAILED(hr)) return hr;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symlink.c_str());
    hr = MFCreateDeviceSource(attr.Get(), &source_);
    if (FAILED(hr)) {
        Log(L"MFCreateDeviceSource: %s", HrHex(hr).c_str());
        return hr;
    }

    // Seleccionar el modo en la propia fuente.
    ComPtr<IMFPresentationDescriptor> pd;
    hr = source_->CreatePresentationDescriptor(&pd);
    bool selected = false;
    DWORD streams = 0;
    if (SUCCEEDED(hr)) pd->GetStreamDescriptorCount(&streams);
    for (DWORD s = 0; SUCCEEDED(hr) && s < streams && !selected; ++s) {
        BOOL sel = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(pd->GetStreamDescriptorByIndex(s, &sel, &sd))) continue;
        ComPtr<IMFMediaTypeHandler> handler;
        if (FAILED(sd->GetMediaTypeHandler(&handler))) continue;
        GUID major{};
        handler->GetMajorType(&major);
        if (major != MFMediaType_Video) continue;
        DWORD count = 0;
        handler->GetMediaTypeCount(&count);
        for (DWORD t = 0; t < count; ++t) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(handler->GetMediaTypeByIndex(t, &mt))) continue;
            GUID sub{};
            UINT32 w = 0, h = 0, n = 0, d = 1;
            mt->GetGUID(MF_MT_SUBTYPE, &sub);
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &w, &h);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &n, &d);
            if (sub == wanted.subtype && w == wanted.width && h == wanted.height &&
                uint64_t(n) * wanted.fpsDen == uint64_t(wanted.fpsNum) * d) {
                hr = handler->SetCurrentMediaType(mt.Get());
                selected = SUCCEEDED(hr);
                break;
            }
        }
        break;
    }
    if (!selected) {
        Log(L"No se pudo seleccionar el modo pedido (%s)", HrHex(hr).c_str());
        source_->Shutdown();
        source_.Reset();
        return FAILED(hr) ? hr : MF_E_INVALIDMEDIATYPE;
    }
    sourceSubtype_ = wanted.subtype;
    const bool native = IsUncompressedRenderable(wanted.subtype);

    ComPtr<IMFAttributes> ra;
    MFCreateAttributes(&ra, 4);
    ra->SetUnknown(MF_SOURCE_READER_ASYNC_CALLBACK, static_cast<IMFSourceReaderCallback*>(this));
    ra->SetUINT32(MF_LOW_LATENCY, TRUE);
    if (native)
        ra->SetUINT32(MF_READWRITE_DISABLE_CONVERTERS, TRUE);
    else
        ra->SetUINT32(MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
    hr = MFCreateSourceReaderFromMediaSource(source_.Get(), ra.Get(), &reader_);
    if (FAILED(hr)) {
        source_->Shutdown();
        source_.Reset();
        return hr;
    }
    reader_->SetStreamSelection(MF_SOURCE_READER_ALL_STREAMS, FALSE);
    reader_->SetStreamSelection(MF_SOURCE_READER_FIRST_VIDEO_STREAM, TRUE);

    if (!native) {
        const GUID outs[] = {MFVideoFormat_NV12, MFVideoFormat_RGB32};
        hr = MF_E_INVALIDMEDIATYPE;
        for (const GUID& o : outs) {
            ComPtr<IMFMediaType> out;
            MFCreateMediaType(&out);
            out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
            out->SetGUID(MF_MT_SUBTYPE, o);
            MFSetAttributeSize(out.Get(), MF_MT_FRAME_SIZE, wanted.width, wanted.height);
            MFSetAttributeRatio(out.Get(), MF_MT_FRAME_RATE, wanted.fpsNum, wanted.fpsDen);
            hr = reader_->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, out.Get());
            if (SUCCEEDED(hr)) break;
        }
        if (FAILED(hr)) {
            Log(L"No hay decodificador para %s: %s", SubtypeName(wanted.subtype), HrHex(hr).c_str());
            reader_.Reset();
            source_->Shutdown();
            source_.Reset();
            return hr;
        }
    }
    hr = ReadCurrentFormat();
    if (FAILED(hr)) {
        reader_.Reset();
        source_->Shutdown();
        source_.Reset();
        return hr;
    }
    Log(L"Captura: %ux%u @ %.3f, origen %s -> %s, stride %ld", format_.width, format_.height, format_.fps,
        SubtypeName(sourceSubtype_), SubtypeName(format_.subtype), format_.stride);

    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = true;
    hr = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(hr)) {
        pending_ = false;
        reader_.Reset();
        source_->Shutdown();
        source_.Reset();
    }
    return hr;
}

HRESULT VideoCapture::ReadCurrentFormat() {
    ComPtr<IMFMediaType> mt;
    HRESULT hr = reader_->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &mt);
    if (FAILED(hr)) return hr;
    FrameFormat f;
    mt->GetGUID(MF_MT_SUBTYPE, &f.subtype);
    MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height);
    UINT32 n = 0, d = 1;
    MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &n, &d);
    f.fps = d ? double(n) / d : 0;
    UINT32 stride = 0;
    if (SUCCEEDED(mt->GetUINT32(MF_MT_DEFAULT_STRIDE, &stride))) {
        f.stride = LONG(stride);
    } else {
        LONG s = 0;
        if (SUCCEEDED(MFGetStrideForBitmapInfoHeader(f.subtype.Data1, f.width, &s))) f.stride = s;
    }
    f.sourceSubtype = sourceSubtype_;
    f.decoded = f.subtype != sourceSubtype_;
    if (!IsUncompressedRenderable(f.subtype)) return MF_E_INVALIDMEDIATYPE;
    format_ = f;
    return S_OK;
}

STDMETHODIMP VideoCapture::OnReadSample(HRESULT hr, DWORD, DWORD flags, LONGLONG timestamp, IMFSample* sample) {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_ = false;
    if (stopping_ || !reader_) {
        SetEvent(idleEvent_);
        return S_OK;
    }
    if (FAILED(hr)) {
        Log(L"Captura: error de lectura %s", HrHex(hr).c_str());
        sink_->OnVideoError(hr);
        SetEvent(idleEvent_);
        return S_OK;
    }
    if (flags & (MF_SOURCE_READERF_CURRENTMEDIATYPECHANGED | MF_SOURCE_READERF_NATIVEMEDIATYPECHANGED)) {
        HRESULT fh = ReadCurrentFormat();
        Log(L"Captura: cambio de formato -> %ux%u (%s)", format_.width, format_.height, HrHex(fh).c_str());
    }
    if (flags & (MF_SOURCE_READERF_ENDOFSTREAM | MF_SOURCE_READERF_ERROR)) {
        Log(L"Captura: fin de flujo o error (flags 0x%X)", flags);
        sink_->OnVideoError((flags & MF_SOURCE_READERF_ERROR) ? E_FAIL : MF_E_END_OF_STREAM);
        SetEvent(idleEvent_);
        return S_OK;
    }
    if (sample) {
        int64_t arrival = QpcNow();
        lastArrival_ = arrival;
        frames_++;
        if (format_.fps > 1 && lastTimestamp_ >= 0 && timestamp > lastTimestamp_) {
            double interval = 1e7 / format_.fps;
            double delta = double(timestamp - lastTimestamp_);
            if (delta > interval * 1.5) gapFrames_ += uint64_t(std::llround(delta / interval) - 1);
        }
        lastTimestamp_ = timestamp;
        sink_->OnVideoSample(sample, format_, arrival);
    }
    pending_ = true;
    HRESULT rh = reader_->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, nullptr, nullptr, nullptr, nullptr);
    if (FAILED(rh)) {
        pending_ = false;
        sink_->OnVideoError(rh);
        SetEvent(idleEvent_);
    }
    return S_OK;
}

void VideoCapture::Stop() {
    bool wait = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        wait = pending_;
    }
    if (wait && reader_) {
        // Flush hace que la petición pendiente vuelva enseguida.
        reader_->Flush(MF_SOURCE_READER_ALL_STREAMS);
        WaitForSingleObject(idleEvent_, 2000);
    }
    std::lock_guard<std::mutex> lock(mutex_);
    reader_.Reset();
    if (source_) {
        source_->Shutdown();
        source_.Reset();
    }
}

FrameFormat VideoCapture::Format() {
    std::lock_guard<std::mutex> lock(mutex_);
    return format_;
}

CaptureStats VideoCapture::Stats() {
    CaptureStats s;
    s.frames = frames_;
    s.gapFrames = gapFrames_;
    s.lastArrivalAgeMs = QpcToMs(QpcNow() - lastArrival_.load());
    return s;
}
