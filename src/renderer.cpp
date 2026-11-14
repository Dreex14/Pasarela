// Copyright (c) 2026 Dreex. Licencia MIT.
//
// Ruta de un fotograma:
//   hilo MF -> buzón (1 plaza: el nuevo sustituye al viejo) -> hilo de render:
//   espera al objeto "frame latency waitable" (máx. 1 fotograma en cola de DXGI)
//   -> toma el fotograma MÁS RECIENTE -> copia CPU->GPU (una sola copia, inevitable
//   con capturadoras USB porque el driver entrega memoria de sistema) ->
//   conversión YUV->RGB en shader a resolución nativa -> escalado con relación de
//   aspecto al back buffer -> Present (sin VSync = inmediato, con tearing si el
//   sistema lo permite; con VSync = sincronizado).
// La swap chain es flip-model (FLIP_DISCARD) en ventana normal o sin bordes; no se
// usa pantalla completa exclusiva, de modo que Discord/WGC pueden capturarla.
#include "renderer.h"

#include <d3dcompiler.h>

#include <algorithm>
#include <cstring>

#include "log.h"

namespace {

const char kShaderSource[] = R"(
cbuffer C : register(b0) { float yScale; float yOffset; float2 pad; float rv; float gu; float gv; float bu; };
Texture2D t0 : register(t0);
Texture2D t1 : register(t1);
SamplerState s0 : register(s0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; };
V VS(uint id : SV_VertexID) {
    V o;
    float2 uv = float2((id << 1) & 2, id & 2);
    o.uv = uv;
    o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float3 conv(float y, float u, float v) {
    y = (y - yOffset) * yScale; u -= 0.5; v -= 0.5;
    return saturate(float3(y + rv * v, y + gu * u + gv * v, y + bu * u));
}
float4 PSNV12(V i) : SV_Target {
    float y = t0.Load(int3(int2(i.p.xy), 0)).r;
    float2 c = t1.SampleLevel(s0, i.uv, 0).rg;
    return float4(conv(y, c.x, c.y), 1);
}
float4 PSYUY2(V i) : SV_Target {
    int2 p = int2(i.p.xy);
    float4 t = t0.Load(int3(p.x >> 1, p.y, 0));
    float y = (p.x & 1) ? t.b : t.r;
    return float4(conv(y, t.g, t.a), 1);
}
float4 PSRGB(V i) : SV_Target { return float4(t0.Load(int3(int2(i.p.xy), 0)).rgb, 1); }
float4 PSPresent(V i) : SV_Target { return float4(t0.SampleLevel(s0, i.uv, 0).rgb, 1); }
)";

struct Constants {
    float yScale, yOffset, pad0, pad1;
    float rv, gu, gv, bu;
};

Constants ColorConstants(UINT height, bool fullRange) {
    const bool hd = height >= 720;  // BT.709 para HD, BT.601 para SD
    Constants c{};
    if (!fullRange) {
        c.yScale = 255.0f / 219.0f;
        c.yOffset = 16.0f / 255.0f;
        if (hd) { c.rv = 1.792741f; c.gu = -0.213249f; c.gv = -0.532909f; c.bu = 2.112402f; }
        else    { c.rv = 1.596027f; c.gu = -0.391762f; c.gv = -0.812968f; c.bu = 2.017232f; }
    } else {
        c.yScale = 1.0f;
        c.yOffset = 0.0f;
        if (hd) { c.rv = 1.5748f; c.gu = -0.187324f; c.gv = -0.468124f; c.bu = 1.8556f; }
        else    { c.rv = 1.402f;  c.gu = -0.344136f; c.gv = -0.714136f; c.bu = 1.772f; }
    }
    return c;
}

HRESULT Compile(const char* entry, const char* profile, ComPtr<ID3DBlob>& out) {
    ComPtr<ID3DBlob> errors;
    HRESULT hr = D3DCompile(kShaderSource, sizeof(kShaderSource) - 1, "pasarela", nullptr, nullptr, entry, profile,
                            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &out, &errors);
    if (FAILED(hr) && errors) Log(L"Shader %S: %S", entry, (const char*)errors->GetBufferPointer());
    return hr;
}

}  // namespace

bool Renderer::Start(HWND hwnd, HRESULT* error) {
    Stop();
    hwnd_ = hwnd;
    stop_ = false;
    resizePending_ = true;
    hasContent_ = false;
    hasPending_ = false;
    frameEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    wakeEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    {
        std::lock_guard<std::mutex> lock(statsMutex_);
        stats_ = RenderStats{};
    }
    accumStart_ = QpcNow();
    std::promise<HRESULT> ready;
    auto fut = ready.get_future();
    thread_ = std::thread(&Renderer::Thread, this, &ready);
    HRESULT hr = fut.get();
    if (FAILED(hr)) {
        thread_.join();
        CloseHandle(frameEvent_);
        CloseHandle(wakeEvent_);
        frameEvent_ = wakeEvent_ = nullptr;
        if (error) *error = hr;
        return false;
    }
    return true;
}

void Renderer::Stop() {
    if (!thread_.joinable()) return;
    stop_ = true;
    SetEvent(wakeEvent_);
    thread_.join();
    CloseHandle(frameEvent_);
    CloseHandle(wakeEvent_);
    frameEvent_ = wakeEvent_ = nullptr;
    std::lock_guard<std::mutex> lock(mailboxMutex_);
    mailbox_ = Pending{};
    hasPending_ = false;
}

void Renderer::SetOptions(const RenderOptions& o) {
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        opts_ = o;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void Renderer::SetStatusText(const std::wstring& t) {
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        if (statusText_ == t) return;
        statusText_ = t;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void Renderer::SetDiagnosticsText(const std::wstring& t) {
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        diagText_ = t;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void Renderer::SetHintText(const std::wstring& t, int ms) {
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        hintText_ = t;
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        hintUntil_ = QpcNow() + f.QuadPart * ms / 1000;
    }
    if (wakeEvent_) SetEvent(wakeEvent_);
}

void Renderer::NotifyResize() {
    resizePending_ = true;
    if (wakeEvent_) SetEvent(wakeEvent_);
}

RenderStats Renderer::Stats() {
    std::lock_guard<std::mutex> lock(statsMutex_);
    return stats_;
}

void Renderer::OnVideoSample(IMFSample* sample, const FrameFormat& format, int64_t arrivalQpc) {
    {
        std::lock_guard<std::mutex> lock(mailboxMutex_);
        if (hasPending_) {
            std::lock_guard<std::mutex> s(statsMutex_);
            stats_.replaced++;  // el anterior nunca llegó a mostrarse: se descarta
        }
        mailbox_.sample = sample;
        mailbox_.format = format;
        mailbox_.arrival = arrivalQpc;
        hasPending_ = true;
    }
    {
        std::lock_guard<std::mutex> s(statsMutex_);
        stats_.received++;
    }
    if (frameEvent_) SetEvent(frameEvent_);
}

void Renderer::OnVideoError(HRESULT hr) {
    if (errorWindow_) PostMessageW(errorWindow_, WM_APP_VIDEO_ERROR, (WPARAM)hr, 0);
}

HRESULT Renderer::CreateDevice() {
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
                                        D3D_FEATURE_LEVEL_10_0};
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    if (FAILED(hr)) {
        Log(L"D3D11 hardware no disponible (%s), se usa WARP", HrHex(hr).c_str());
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, nullptr, &ctx_);
    }
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIDevice1> dxgiDevice;
    device_.As(&dxgiDevice);
    dxgiDevice->SetMaximumFrameLatency(1);
    ComPtr<IDXGIAdapter> adapter;
    dxgiDevice->GetAdapter(&adapter);
    DXGI_ADAPTER_DESC ad{};
    adapter->GetDesc(&ad);
    ComPtr<IDXGIFactory2> factory;
    hr = adapter->GetParent(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;

    ComPtr<IDXGIFactory5> f5;
    BOOL allowTearing = FALSE;
    if (SUCCEEDED(factory.As(&f5)))
        f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &allowTearing, sizeof(allowTearing));
    tearing_ = allowTearing != FALSE;

    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 2;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Scaling = DXGI_SCALING_STRETCH;
    sd.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swapFlags_ = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | (tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    sd.Flags = swapFlags_;
    ComPtr<IDXGISwapChain1> sc1;
    hr = factory->CreateSwapChainForHwnd(device_.Get(), hwnd_, &sd, nullptr, nullptr, &sc1);
    if (FAILED(hr)) return hr;
    factory->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);
    hr = sc1.As(&swap_);
    if (FAILED(hr)) return hr;
    swap_->SetMaximumFrameLatency(1);
    waitable_ = swap_->GetFrameLatencyWaitableObject();

    // Shaders
    ComPtr<ID3DBlob> blob;
    if (FAILED(hr = Compile("VS", "vs_4_0", blob))) return hr;
    device_->CreateVertexShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, &vs_);
    struct { const char* name; ComPtr<ID3D11PixelShader>* ps; } list[] = {
        {"PSNV12", &psNv12_}, {"PSYUY2", &psYuy2_}, {"PSRGB", &psRgb_}, {"PSPresent", &psPresent_}};
    for (auto& e : list) {
        if (FAILED(hr = Compile(e.name, "ps_4_0", blob))) return hr;
        hr = device_->CreatePixelShader(blob->GetBufferPointer(), blob->GetBufferSize(), nullptr, e.ps->GetAddressOf());
        if (FAILED(hr)) return hr;
    }
    D3D11_SAMPLER_DESC samp{};
    samp.AddressU = samp.AddressV = samp.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samp.MaxLOD = D3D11_FLOAT32_MAX;
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    device_->CreateSamplerState(&samp, &samplerLinear_);
    samp.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    device_->CreateSamplerState(&samp, &samplerPoint_);
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = sizeof(Constants);
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    hr = device_->CreateBuffer(&bd, nullptr, &constants_);
    if (FAILED(hr)) return hr;

    // Direct2D / DirectWrite para textos
    D2D1_FACTORY_OPTIONS fo{};
    hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory1), &fo,
                           reinterpret_cast<void**>(d2dFactory_.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = d2dFactory_->CreateDevice(dxgiDevice.Get(), &d2dDevice_);
    if (SUCCEEDED(hr)) hr = d2dDevice_->CreateDeviceContext(D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2dCtx_);
    if (SUCCEEDED(hr))
        hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwFactory_.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        float scale = GetDpiForWindow(hwnd_) / 96.0f;
        dwFactory_->CreateTextFormat(L"Consolas", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 13.0f * scale, L"", &textDiag_);
        dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 22.0f * scale, L"", &textStatus_);
        dwFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, 14.0f * scale, L"", &textHint_);
        if (textStatus_) {
            textStatus_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
            textStatus_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        }
        if (textHint_) textHint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
        d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.93f, 0.94f, 0.96f, 1.0f), &brushText_);
        d2dCtx_->CreateSolidColorBrush(D2D1::ColorF(0.05f, 0.06f, 0.08f, 0.78f), &brushPanel_);
    } else {
        Log(L"Direct2D no disponible (%s): sin textos superpuestos", HrHex(hr).c_str());
        d2dCtx_.Reset();
        hr = S_OK;
    }

    std::lock_guard<std::mutex> lock(statsMutex_);
    stats_.tearingSupported = tearing_;
    stats_.adapter = ad.Description;
    Log(L"Render: adaptador \"%s\", tearing %s", ad.Description, tearing_ ? L"sí" : L"no");
    return S_OK;
}

void Renderer::ReleaseSizeDependent() {
    if (d2dCtx_) d2dCtx_->SetTarget(nullptr);
    d2dTarget_.Reset();
    backRtv_.Reset();
    ctx_->OMSetRenderTargets(0, nullptr, nullptr);
    ctx_->Flush();
}

HRESULT Renderer::CreateSizeDependent() {
    RECT rc{};
    GetClientRect(hwnd_, &rc);
    UINT w = UINT(std::max<LONG>(rc.right - rc.left, 0)), h = UINT(std::max<LONG>(rc.bottom - rc.top, 0));
    if (!w || !h) {
        backW_ = backH_ = 0;
        return S_OK;
    }
    HRESULT hr = swap_->ResizeBuffers(0, w, h, DXGI_FORMAT_UNKNOWN, swapFlags_);
    if (FAILED(hr)) return hr;
    backW_ = w;
    backH_ = h;
    ComPtr<ID3D11Texture2D> back;
    hr = swap_->GetBuffer(0, IID_PPV_ARGS(&back));
    if (FAILED(hr)) return hr;
    hr = device_->CreateRenderTargetView(back.Get(), nullptr, &backRtv_);
    if (FAILED(hr)) return hr;
    if (d2dCtx_) {
        ComPtr<IDXGISurface> surface;
        back.As(&surface);
        D2D1_BITMAP_PROPERTIES1 bp = D2D1::BitmapProperties1(
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW,
            D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
        if (SUCCEEDED(d2dCtx_->CreateBitmapFromDxgiSurface(surface.Get(), &bp, &d2dTarget_)))
            d2dCtx_->SetTarget(d2dTarget_.Get());
    }
    return S_OK;
}

HRESULT Renderer::EnsureSourceTextures(const FrameFormat& f) {
    if (texY_ && f.subtype == srcSubtype_ && f.width == srcW_ && f.height == srcH_) return S_OK;
    texY_.Reset(); texUV_.Reset(); srvY_.Reset(); srvUV_.Reset();
    texRgb_.Reset(); rtvRgb_.Reset(); srvRgb_.Reset();
    hasContent_ = false;
    srcSubtype_ = f.subtype;
    srcW_ = f.width;
    srcH_ = f.height;

    auto makeDynamic = [&](UINT w, UINT h, DXGI_FORMAT fmt, ComPtr<ID3D11Texture2D>& tex,
                           ComPtr<ID3D11ShaderResourceView>& srv) -> HRESULT {
        D3D11_TEXTURE2D_DESC td{};
        td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = fmt;
        td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DYNAMIC; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        HRESULT hr = device_->CreateTexture2D(&td, nullptr, &tex);
        if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(tex.Get(), nullptr, &srv);
        return hr;
    };
    HRESULT hr;
    if (f.subtype == MFVideoFormat_NV12) {
        hr = makeDynamic(f.width, f.height, DXGI_FORMAT_R8_UNORM, texY_, srvY_);
        if (SUCCEEDED(hr)) hr = makeDynamic((f.width + 1) / 2, (f.height + 1) / 2, DXGI_FORMAT_R8G8_UNORM, texUV_, srvUV_);
    } else if (f.subtype == MFVideoFormat_YUY2) {
        hr = makeDynamic((f.width + 1) / 2, f.height, DXGI_FORMAT_R8G8B8A8_UNORM, texY_, srvY_);
    } else {
        hr = makeDynamic(f.width, f.height, DXGI_FORMAT_B8G8R8A8_UNORM, texY_, srvY_);
    }
    if (FAILED(hr)) return hr;

    D3D11_TEXTURE2D_DESC td{};
    td.Width = f.width; td.Height = f.height; td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    hr = device_->CreateTexture2D(&td, nullptr, &texRgb_);
    if (SUCCEEDED(hr)) hr = device_->CreateRenderTargetView(texRgb_.Get(), nullptr, &rtvRgb_);
    if (SUCCEEDED(hr)) hr = device_->CreateShaderResourceView(texRgb_.Get(), nullptr, &srvRgb_);
    Log(L"Render: texturas %ux%u %s (%s)", f.width, f.height, SubtypeName(f.subtype), HrHex(hr).c_str());
    return hr;
}

static void CopyPlane(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex, const BYTE* src, LONG pitch, UINT rows,
                      UINT rowBytes) {
    D3D11_MAPPED_SUBRESOURCE m{};
    if (FAILED(ctx->Map(tex, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) return;
    auto* dst = static_cast<BYTE*>(m.pData);
    UINT n = std::min<UINT>(rowBytes, m.RowPitch);
    if (pitch > 0 && UINT(pitch) == m.RowPitch && n == rowBytes && rowBytes == m.RowPitch) {
        std::memcpy(dst, src, size_t(rows) * rowBytes);
    } else {
        for (UINT r = 0; r < rows; ++r) std::memcpy(dst + size_t(r) * m.RowPitch, src + ptrdiff_t(r) * pitch, n);
    }
    ctx->Unmap(tex, 0);
}

bool Renderer::Upload(IMFSample* sample, const FrameFormat& f) {
    ComPtr<IMFMediaBuffer> buf;
    DWORD count = 0;
    sample->GetBufferCount(&count);
    HRESULT hr = count == 1 ? sample->GetBufferByIndex(0, &buf) : sample->ConvertToContiguousBuffer(&buf);
    if (FAILED(hr)) return false;

    BYTE* scan0 = nullptr;
    LONG pitch = 0;
    DWORD length = 0;
    bool locked2d = false;
    ComPtr<IMF2DBuffer> b2;
    BYTE* raw = nullptr;
    if (SUCCEEDED(buf.As(&b2)) && SUCCEEDED(b2->Lock2D(&scan0, &pitch))) {
        locked2d = true;
        b2->GetContiguousLength(&length);
    } else {
        DWORD maxLen = 0;
        if (FAILED(buf->Lock(&raw, &maxLen, &length))) return false;
        LONG stride = f.stride;
        if (!stride) {
            stride = f.subtype == MFVideoFormat_NV12 ? LONG(f.width)
                     : f.subtype == MFVideoFormat_YUY2 ? LONG(f.width * 2)
                                                       : LONG(f.width * 4);
        }
        pitch = stride;
        scan0 = stride < 0 ? raw + size_t(-stride) * (f.height - 1) : raw;
    }

    const UINT w = f.width, h = f.height;
    if (f.subtype == MFVideoFormat_NV12) {
        // Algunos drivers alinean la altura del plano Y a 16 líneas (1080 -> 1088).
        UINT planeRows = h;
        UINT aligned = (h + 15) & ~15u;
        UINT absPitch = UINT(pitch < 0 ? -pitch : pitch);
        if (aligned != h && length == size_t(absPitch) * aligned * 3 / 2) planeRows = aligned;
        CopyPlane(ctx_.Get(), texY_.Get(), scan0, pitch, h, w);
        CopyPlane(ctx_.Get(), texUV_.Get(), scan0 + ptrdiff_t(pitch) * planeRows, pitch, (h + 1) / 2, ((w + 1) / 2) * 2);
    } else if (f.subtype == MFVideoFormat_YUY2) {
        CopyPlane(ctx_.Get(), texY_.Get(), scan0, pitch, h, ((w + 1) / 2) * 4);
    } else {
        CopyPlane(ctx_.Get(), texY_.Get(), scan0, pitch, h, w * 4);
    }

    if (locked2d)
        b2->Unlock2D();
    else
        buf->Unlock();
    return true;
}

void Renderer::ConvertToIntermediate(const FrameFormat& f) {
    bool fullRange;
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        fullRange = opts_.fullRange;
    }
    Constants c = ColorConstants(f.height, fullRange);
    ctx_->UpdateSubresource(constants_.Get(), 0, nullptr, &c, 0, 0);

    D3D11_VIEWPORT vp{0, 0, float(f.width), float(f.height), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ID3D11RenderTargetView* rtv = rtvRgb_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->IASetInputLayout(nullptr);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ID3D11PixelShader* ps = f.subtype == MFVideoFormat_NV12   ? psNv12_.Get()
                            : f.subtype == MFVideoFormat_YUY2 ? psYuy2_.Get()
                                                              : psRgb_.Get();
    ctx_->PSSetShader(ps, nullptr, 0);
    ID3D11ShaderResourceView* srvs[2] = {srvY_.Get(), srvUV_.Get()};
    ctx_->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* s = samplerLinear_.Get();
    ctx_->PSSetSamplers(0, 1, &s);
    ID3D11Buffer* cb = constants_.Get();
    ctx_->PSSetConstantBuffers(0, 1, &cb);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nulls[2] = {};
    ctx_->PSSetShaderResources(0, 2, nulls);
    hasContent_ = true;
}

void Renderer::DrawFrame() {
    const float black[4] = {0, 0, 0, 1};
    ID3D11RenderTargetView* rtv = backRtv_.Get();
    ctx_->OMSetRenderTargets(1, &rtv, nullptr);
    ctx_->ClearRenderTargetView(rtv, black);
    if (!hasContent_ || !srcW_ || !srcH_) return;

    // Encaje manteniendo la relación de aspecto (bandas negras si hace falta).
    double srcAspect = double(srcW_) / srcH_;
    double dstAspect = double(backW_) / backH_;
    float vw, vh;
    if (dstAspect > srcAspect) {
        vh = float(backH_);
        vw = float(backH_ * srcAspect);
    } else {
        vw = float(backW_);
        vh = float(backW_ / srcAspect);
    }
    bool nearest;
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        nearest = opts_.nearest;
    }
    // Con "píxel exacto" se redondea a píxeles enteros para evitar costuras.
    D3D11_VIEWPORT vp{float(int((backW_ - vw) / 2)), float(int((backH_ - vh) / 2)), float(int(vw + 0.5f)),
                      float(int(vh + 0.5f)), 0, 1};
    ctx_->RSSetViewports(1, &vp);
    ctx_->VSSetShader(vs_.Get(), nullptr, 0);
    ctx_->PSSetShader(psPresent_.Get(), nullptr, 0);
    ID3D11ShaderResourceView* srv = srvRgb_.Get();
    ctx_->PSSetShaderResources(0, 1, &srv);
    ID3D11SamplerState* s = nearest ? samplerPoint_.Get() : samplerLinear_.Get();
    ctx_->PSSetSamplers(0, 1, &s);
    ctx_->Draw(3, 0);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    ctx_->PSSetShaderResources(0, 1, &nullSrv);
}

void Renderer::DrawOverlay() {
    if (!d2dCtx_ || !d2dTarget_) return;
    std::wstring status, diag, hint;
    bool showDiag;
    {
        std::lock_guard<std::mutex> lock(optMutex_);
        status = statusText_;
        diag = diagText_;
        showDiag = opts_.showDiagnostics;
        if (QpcNow() < hintUntil_) hint = hintText_;
    }
    if (status.empty() && (!showDiag || diag.empty()) && hint.empty()) return;
    float scale = GetDpiForWindow(hwnd_) / 96.0f;
    d2dCtx_->BeginDraw();
    if (showDiag && !diag.empty() && textDiag_) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwFactory_->CreateTextLayout(diag.c_str(), UINT32(diag.size()), textDiag_.Get(), 900 * scale,
                                                   900 * scale, &layout))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            float pad = 10 * scale;
            D2D1_ROUNDED_RECT rr = {D2D1::RectF(12 * scale, 12 * scale, 12 * scale + m.width + 2 * pad,
                                                12 * scale + m.height + 2 * pad),
                                    8 * scale, 8 * scale};
            d2dCtx_->FillRoundedRectangle(rr, brushPanel_.Get());
            d2dCtx_->DrawTextLayout(D2D1::Point2F(12 * scale + pad, 12 * scale + pad), layout.Get(), brushText_.Get());
        }
    }
    if (!status.empty() && textStatus_) {
        d2dCtx_->DrawText(status.c_str(), UINT32(status.size()), textStatus_.Get(),
                          D2D1::RectF(0, 0, float(backW_), float(backH_)), brushText_.Get());
    }
    if (!hint.empty() && textHint_) {
        ComPtr<IDWriteTextLayout> layout;
        if (SUCCEEDED(dwFactory_->CreateTextLayout(hint.c_str(), UINT32(hint.size()), textHint_.Get(),
                                                   float(backW_), 200 * scale, &layout))) {
            DWRITE_TEXT_METRICS m{};
            layout->GetMetrics(&m);
            float pad = 10 * scale;
            float x = (backW_ - m.widthIncludingTrailingWhitespace) / 2;
            float y = backH_ - m.height - 40 * scale;
            D2D1_ROUNDED_RECT rr = {D2D1::RectF(x - 2 * pad, y - pad, x + m.widthIncludingTrailingWhitespace + 2 * pad,
                                                y + m.height + pad),
                                    10 * scale, 10 * scale};
            d2dCtx_->FillRoundedRectangle(rr, brushPanel_.Get());
            d2dCtx_->DrawTextLayout(D2D1::Point2F(0, y), layout.Get(), brushText_.Get());
        }
    }
    d2dCtx_->EndDraw();
}

void Renderer::Thread(std::promise<HRESULT>* ready) {
    HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool comOk = SUCCEEDED(hr);
    hr = CreateDevice();
    if (FAILED(hr)) Log(L"Render: no se pudo crear el dispositivo (%s)", HrHex(hr).c_str());
    ready->set_value(hr);
    if (FAILED(hr)) {
        swap_.Reset(); ctx_.Reset(); device_.Reset();
        if (comOk) CoUninitialize();
        return;
    }
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

    HANDLE handles[2] = {frameEvent_, wakeEvent_};
    while (!stop_) {
        WaitForMultipleObjects(2, handles, FALSE, 250);
        if (stop_) break;

        if (resizePending_.exchange(false)) {
            ReleaseSizeDependent();
            hr = CreateSizeDependent();
            if (FAILED(hr)) Log(L"Render: ResizeBuffers %s", HrHex(hr).c_str());
        }

        // Esperar a que DXGI acepte otro fotograma ANTES de elegir cuál mostrar:
        // así siempre se muestra el más reciente.
        if (backW_ && waitable_) WaitForSingleObjectEx(waitable_, 100, TRUE);

        Pending frame;
        bool have = false;
        {
            std::lock_guard<std::mutex> lock(mailboxMutex_);
            if (hasPending_) {
                frame = std::move(mailbox_);
                mailbox_ = Pending{};
                hasPending_ = false;
                have = true;
            }
        }
        if (!backW_) continue;  // minimizada: se descarta y no se dibuja

        int64_t t0 = QpcNow();
        if (have) {
            if (SUCCEEDED(EnsureSourceTextures(frame.format)) && Upload(frame.sample.Get(), frame.format))
                ConvertToIntermediate(frame.format);
            frame.sample.Reset();  // devolver el buffer al driver cuanto antes
        }
        int64_t t1 = QpcNow();
        DrawFrame();
        DrawOverlay();

        bool vsync;
        {
            std::lock_guard<std::mutex> lock(optMutex_);
            vsync = opts_.vsync;
        }
        int64_t beforePresent = QpcNow();
        hr = vsync ? swap_->Present(1, 0) : swap_->Present(0, tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
            Log(L"Render: dispositivo perdido (%s)", HrHex(device_->GetDeviceRemovedReason()).c_str());
            OnVideoError(hr);
            break;
        }
        if (have) {
            uploadAccum_ += QpcToMs(t1 - t0);
            double pipe = QpcToMs(beforePresent - frame.arrival);
            pipeAccum_ += pipe;
            pipeMax_ = std::max(pipeMax_, pipe);
            accumCount_++;
        }
        std::lock_guard<std::mutex> lock(statsMutex_);
        if (have) stats_.presented++;
        if (QpcToMs(QpcNow() - accumStart_) > 1000) {
            if (accumCount_) {
                stats_.uploadMsAvg = uploadAccum_ / accumCount_;
                stats_.pipelineMsAvg = pipeAccum_ / accumCount_;
                stats_.pipelineMsMax = pipeMax_;
            }
            uploadAccum_ = pipeAccum_ = pipeMax_ = 0;
            accumCount_ = 0;
            accumStart_ = QpcNow();
        }
    }

    ctx_->ClearState();
    ReleaseSizeDependent();
    texY_.Reset(); texUV_.Reset(); srvY_.Reset(); srvUV_.Reset();
    texRgb_.Reset(); rtvRgb_.Reset(); srvRgb_.Reset();
    brushText_.Reset(); brushPanel_.Reset(); textDiag_.Reset(); textStatus_.Reset(); textHint_.Reset();
    d2dCtx_.Reset(); d2dDevice_.Reset(); d2dFactory_.Reset(); dwFactory_.Reset();
    vs_.Reset(); psNv12_.Reset(); psYuy2_.Reset(); psRgb_.Reset(); psPresent_.Reset();
    samplerLinear_.Reset(); samplerPoint_.Reset(); constants_.Reset();
    if (waitable_) CloseHandle(waitable_);
    waitable_ = nullptr;
    swap_.Reset();
    ctx_.Reset();
    device_.Reset();
    srcW_ = srcH_ = 0;
    texY_.Reset();
    if (comOk) CoUninitialize();
}
