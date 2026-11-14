// Copyright (c) 2026 Dreex. Licencia MIT.
#include "devices.h"

#include <cfgmgr32.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <propvarutil.h>

#include <initguid.h>
#include <devpkey.h>
#include <functiondiscoverykeys_devpkey.h>

#include <algorithm>
#include <cmath>
#include <cwctype>

#include "log.h"

// PKEY_Device_ContainerId (mismo GUID/pid que DEVPKEY_Device_ContainerId).
static const PROPERTYKEY kPkeyContainerId = {
    {0x8c7ed206, 0x3f8a, 0x4827, {0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c}}, 2};

static std::wstring GuidToString(const GUID& g) {
    wchar_t buf[64];
    StringFromGUID2(g, buf, 64);
    return buf;
}

static std::wstring ContainerIdFromInterface(const std::wstring& symlink) {
    wchar_t instanceId[512];
    ULONG size = sizeof(instanceId);
    DEVPROPTYPE type = 0;
    if (CM_Get_Device_Interface_PropertyW(symlink.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instanceId), &size,
                                          0) != CR_SUCCESS) {
        return {};
    }
    DEVINST devInst = 0;
    if (CM_Locate_DevNodeW(&devInst, instanceId, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return {};
    GUID container{};
    size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(devInst, &DEVPKEY_Device_ContainerId, &type,
                                 reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS) {
        return {};
    }
    return GuidToString(container);
}

std::vector<VideoDevice> EnumerateVideoDevices() {
    std::vector<VideoDevice> result;
    ComPtr<IMFAttributes> attr;
    if (FAILED(MFCreateAttributes(&attr, 1))) return result;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    IMFActivate** devices = nullptr;
    UINT32 count = 0;
    HRESULT hr = MFEnumDeviceSources(attr.Get(), &devices, &count);
    if (FAILED(hr)) {
        Log(L"MFEnumDeviceSources falló: %s", HrHex(hr).c_str());
        return result;
    }
    for (UINT32 i = 0; i < count; ++i) {
        VideoDevice d;
        wchar_t* str = nullptr;
        UINT32 len = 0;
        if (SUCCEEDED(devices[i]->GetAllocatedString(MF_DEVSOURCE_ATTRIBUTE_FRIENDLY_NAME, &str, &len))) {
            d.name = str;
            CoTaskMemFree(str);
        }
        if (SUCCEEDED(devices[i]->GetAllocatedString(
                MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, &str, &len))) {
            d.symlink = str;
            CoTaskMemFree(str);
        }
        d.containerId = ContainerIdFromInterface(d.symlink);
        devices[i]->Release();
        if (!d.symlink.empty()) {
            Log(L"Vídeo: \"%s\" contenedor=%s", d.name.c_str(), d.containerId.c_str());
            result.push_back(std::move(d));
        }
    }
    CoTaskMemFree(devices);
    return result;
}

std::vector<AudioEndpoint> EnumerateAudioEndpoints(EDataFlow flow) {
    std::vector<AudioEndpoint> result;
    ComPtr<IMMDeviceEnumerator> en;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&en)))) {
        return result;
    }
    std::wstring defaultId;
    ComPtr<IMMDevice> def;
    if (SUCCEEDED(en->GetDefaultAudioEndpoint(flow, eConsole, &def))) {
        wchar_t* id = nullptr;
        if (SUCCEEDED(def->GetId(&id))) {
            defaultId = id;
            CoTaskMemFree(id);
        }
    }
    ComPtr<IMMDeviceCollection> coll;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &coll))) return result;
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> dev;
        if (FAILED(coll->Item(i, &dev))) continue;
        AudioEndpoint ep;
        wchar_t* id = nullptr;
        if (SUCCEEDED(dev->GetId(&id))) {
            ep.id = id;
            CoTaskMemFree(id);
        }
        ComPtr<IPropertyStore> props;
        if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &props))) {
            PROPVARIANT pv;
            PropVariantInit(&pv);
            if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &pv)) && pv.vt == VT_LPWSTR) {
                ep.name = pv.pwszVal;
            }
            PropVariantClear(&pv);
            if (SUCCEEDED(props->GetValue(kPkeyContainerId, &pv)) && pv.vt == VT_CLSID && pv.puuid) {
                ep.containerId = GuidToString(*pv.puuid);
            }
            PropVariantClear(&pv);
        }
        ep.isDefault = (ep.id == defaultId);
        result.push_back(std::move(ep));
    }
    return result;
}

HRESULT EnumerateVideoFormats(const std::wstring& symlink, std::vector<VideoFormat>& out) {
    out.clear();
    ComPtr<IMFAttributes> attr;
    HRESULT hr = MFCreateAttributes(&attr, 2);
    if (FAILED(hr)) return hr;
    attr->SetGUID(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
    attr->SetString(MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK, symlink.c_str());
    ComPtr<IMFMediaSource> source;
    hr = MFCreateDeviceSource(attr.Get(), &source);
    if (FAILED(hr)) return hr;

    ComPtr<IMFPresentationDescriptor> pd;
    hr = source->CreatePresentationDescriptor(&pd);
    DWORD streams = 0;
    if (SUCCEEDED(hr)) pd->GetStreamDescriptorCount(&streams);
    for (DWORD s = 0; SUCCEEDED(hr) && s < streams; ++s) {
        BOOL selected = FALSE;
        ComPtr<IMFStreamDescriptor> sd;
        if (FAILED(pd->GetStreamDescriptorByIndex(s, &selected, &sd))) continue;
        ComPtr<IMFMediaTypeHandler> handler;
        if (FAILED(sd->GetMediaTypeHandler(&handler))) continue;
        GUID major{};
        handler->GetMajorType(&major);
        if (major != MFMediaType_Video) continue;
        DWORD typeCount = 0;
        handler->GetMediaTypeCount(&typeCount);
        for (DWORD t = 0; t < typeCount; ++t) {
            ComPtr<IMFMediaType> mt;
            if (FAILED(handler->GetMediaTypeByIndex(t, &mt))) continue;
            VideoFormat f;
            mt->GetGUID(MF_MT_SUBTYPE, &f.subtype);
            MFGetAttributeSize(mt.Get(), MF_MT_FRAME_SIZE, &f.width, &f.height);
            MFGetAttributeRatio(mt.Get(), MF_MT_FRAME_RATE, &f.fpsNum, &f.fpsDen);
            if (!f.width || !f.height || !f.fpsNum || !f.fpsDen) continue;
            bool dup = std::any_of(out.begin(), out.end(), [&](const VideoFormat& o) {
                return o.subtype == f.subtype && o.width == f.width && o.height == f.height &&
                       uint64_t(o.fpsNum) * f.fpsDen == uint64_t(f.fpsNum) * o.fpsDen;
            });
            if (!dup) out.push_back(f);
        }
        break;  // primer flujo de vídeo
    }
    source->Shutdown();
    for (auto& f : out) {
        Log(L"  modo: %ux%u @ %.3f %s", f.width, f.height, f.Fps(), SubtypeName(f.subtype));
    }
    return out.empty() && SUCCEEDED(hr) ? MF_E_INVALIDMEDIATYPE : hr;
}

static std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

int FindAssociatedAudioInput(const VideoDevice& video, const std::vector<AudioEndpoint>& inputs) {
    // 1) Mismo contenedor físico. Se ignora el contenedor nulo/genérico del sistema.
    const std::wstring nullContainer = L"{00000000-0000-0000-FFFF-FFFFFFFFFFFF}";
    if (!video.containerId.empty() && video.containerId != nullContainer) {
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (_wcsicmp(inputs[i].containerId.c_str(), video.containerId.c_str()) == 0) return (int)i;
        }
    }
    // 2) El nombre del vídeo aparece en el del audio, p. ej. "USB3 Video" en
    //    "Digital Audio Interface (USB3 Video)".
    std::wstring vn = Lower(video.name);
    if (vn.size() >= 4) {
        for (size_t i = 0; i < inputs.size(); ++i) {
            if (Lower(inputs[i].name).find(vn) != std::wstring::npos) return (int)i;
        }
    }
    return -1;
}

const wchar_t* SubtypeName(const GUID& s) {
    if (s == MFVideoFormat_NV12) return L"NV12";
    if (s == MFVideoFormat_YUY2) return L"YUY2";
    if (s == MFVideoFormat_MJPG) return L"MJPEG";
    if (s == MFVideoFormat_RGB32) return L"RGB32";
    if (s == MFVideoFormat_ARGB32) return L"ARGB32";
    if (s == MFVideoFormat_RGB24) return L"RGB24";
    if (s == MFVideoFormat_UYVY) return L"UYVY";
    if (s == MFVideoFormat_I420) return L"I420";
    if (s == MFVideoFormat_IYUV) return L"IYUV";
    if (s == MFVideoFormat_YV12) return L"YV12";
    if (s == MFVideoFormat_P010) return L"P010";
    if (s == MFVideoFormat_H264) return L"H.264";
    if (s == MFVideoFormat_HEVC) return L"HEVC";
    return L"?";
}

bool IsUncompressedRenderable(const GUID& s) {
    return s == MFVideoFormat_NV12 || s == MFVideoFormat_YUY2 || s == MFVideoFormat_RGB32 ||
           s == MFVideoFormat_ARGB32;
}

int SubtypePreference(const GUID& s) {
    if (s == MFVideoFormat_NV12) return 0;  // la mitad de datos que YUY2 y sin decodificar
    if (s == MFVideoFormat_YUY2) return 1;
    if (s == MFVideoFormat_RGB32 || s == MFVideoFormat_ARGB32) return 2;
    if (IsUncompressedRenderable(s)) return 3;
    if (s == MFVideoFormat_MJPG) return 5;  // requiere decodificación
    return 8;
}

std::vector<int> RankAutoFormats(const std::vector<VideoFormat>& formats, UINT32 maxW, UINT32 maxH) {
    // Criterios, por orden:
    //  1. ~60 FPS (lo que entregan las consolas casi siempre); después más; después menos.
    //  2. Que quepa en la pantalla: capturar por encima de ella solo gasta ancho de
    //     banda USB (la consola ya sale a la resolución del monitor por el passthrough).
    //  3. Dentro de lo que cabe, la mayor resolución; si nada cabe, la menor.
    //  4. Sin compresión (NV12 > YUY2 > RGB) antes que MJPEG.
    auto fpsClass = [](const VideoFormat& f) {
        double fps = f.Fps();
        return fps >= 59.0 && fps < 61.0 ? 0 : fps >= 61.0 ? 1 : 2;
    };
    auto fits = [&](const VideoFormat& f) { return !maxW || !maxH || (f.width <= maxW && f.height <= maxH); };
    std::vector<int> idx;
    for (int i = 0; i < (int)formats.size(); ++i)
        if (SubtypePreference(formats[i].subtype) < 8) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](int ia, int ib) {
        const VideoFormat& a = formats[ia];
        const VideoFormat& b = formats[ib];
        if (fpsClass(a) != fpsClass(b)) return fpsClass(a) < fpsClass(b);
        if (fits(a) != fits(b)) return fits(a);
        uint64_t pa = uint64_t(a.width) * a.height, pb = uint64_t(b.width) * b.height;
        if (pa != pb) return fits(a) ? pa > pb : pa < pb;
        int sa = SubtypePreference(a.subtype), sb = SubtypePreference(b.subtype);
        if (sa != sb) return sa < sb;
        return std::fabs(a.Fps() - 60.0) < std::fabs(b.Fps() - 60.0);
    });
    return idx;
}

int ChooseAutoFormat(const std::vector<VideoFormat>& formats, UINT32 maxW, UINT32 maxH) {
    auto r = RankAutoFormats(formats, maxW, maxH);
    return r.empty() ? -1 : r.front();
}

// --- Vigilancia de dispositivos de audio -------------------------------------

class AudioDeviceWatcher : public IMMNotificationClient {
public:
    explicit AudioDeviceWatcher(HWND hwnd) : hwnd_(hwnd) {}
    virtual ~AudioDeviceWatcher() = default;
    ComPtr<IMMDeviceEnumerator> enumerator;

    ULONG STDMETHODCALLTYPE AddRef() override { return ++ref_; }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG r = --ref_;
        if (!r) delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IMMNotificationClient)) {
            *ppv = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return Post(false); }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return Post(false); }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return Post(false); }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) PostMessageW(hwnd_, WM_APP_AUDIO_DEFAULT_CHANGED, 0, 0);
        return Post(false);
    }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    HRESULT Post(bool) {
        PostMessageW(hwnd_, WM_APP_AUDIO_DEVICES_CHANGED, 0, 0);
        return S_OK;
    }
    HWND hwnd_;
    std::atomic<ULONG> ref_{1};
};

AudioDeviceWatcher* StartAudioDeviceWatcher(HWND hwnd) {
    auto* w = new AudioDeviceWatcher(hwnd);
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                IID_PPV_ARGS(&w->enumerator))) ||
        FAILED(w->enumerator->RegisterEndpointNotificationCallback(w))) {
        w->Release();
        return nullptr;
    }
    return w;
}

void StopAudioDeviceWatcher(AudioDeviceWatcher* w) {
    if (!w) return;
    w->enumerator->UnregisterEndpointNotificationCallback(w);
    w->enumerator.Reset();
    w->Release();
}
