// Copyright (c) 2026 Dreex. Licencia MIT.
//
// Ventana principal, ventana de vídeo, bandeja del sistema y controlador.
//
// Dos acciones principales:
//   «Solo audio»    -> únicamente AudioMonitor. No se abre el vídeo de la
//                      capturadora, no se crea Direct3D, no se procesa ningún
//                      fotograma.
//   «Mostrar vídeo» -> añade VideoCapture + Renderer (ventana de vídeo). El audio
//                      sigue siendo el mismo AudioMonitor: abrir o cerrar el
//                      vídeo no corta el sonido.
#include "app.h"

#include <commctrl.h>
#include <dbt.h>
#include <shellapi.h>
#include <shlobj.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <thread>

#include "audio_monitor.h"
#include "devices.h"
#include "log.h"
#include "renderer.h"
#include "resource.h"
#include "settings.h"
#include "ui.h"
#include "video_capture.h"

namespace {

enum : int {
    ID_BTN_AUDIO = 101, ID_BTN_VIDEO, ID_BTN_STOP, ID_SLIDER_VOL, ID_BTN_MUTE, ID_CB_VIDEO, ID_BTN_REFRESH,
    ID_CB_AOUT, ID_BTN_ADV, ID_CB_QUALITY, ID_CB_FPS, ID_CB_FORMAT, ID_CB_AIN, ID_TG_AUTO_AUDIO, ID_TG_TRAY,
    ID_TG_TOPMOST, ID_TG_VSYNC, ID_TG_FULLRANGE, ID_TG_DIAG, ID_CB_BUFFER, ID_SLIDER_DELAY,
    ID_BTN_FOLDER, ID_BTN_SHOTS, ID_TG_HIDE_VIDEO, ID_TG_KEEP_DISPLAY,
};
enum : int {
    IDM_FULLSCREEN = 301, IDM_TOPMOST, IDM_MUTE, IDM_DIAG, IDM_HIDE_VIDEO, IDM_SCREENSHOT, IDM_STOP, IDM_SHOW, IDM_AUDIO, IDM_VIDEO,
    IDM_EXIT
};
enum : UINT_PTR { TIMER_TICK = 1, TIMER_VIDEO_DEVICES, TIMER_AUDIO_DEVICES, TIMER_AUDIO_DEFAULT, TIMER_CURSOR };
enum Session { kOff = 0, kAudio = 1, kVideo = 2 };

const wchar_t kMainClass[] = L"PasarelaPrincipal";
const wchar_t kVideoClass[] = L"PasarelaVideo";
const wchar_t kSettingsClass[] = L"PasarelaAjustes";
const wchar_t kVideoTitle[] = L"Pasarela — Vídeo";
const HRESULT E_APP_NO_DEVICE = MAKE_HRESULT(1, FACILITY_ITF, 0x201);
const HRESULT E_APP_GPU = MAKE_HRESULT(1, FACILITY_ITF, 0x202);

struct FormatsResult {
    std::wstring symlink;
    std::vector<VideoFormat> formats;
    HRESULT hr = S_OK;
};

std::wstring ResolutionText(UINT32 w, UINT32 h) {
    const wchar_t* tag = h == 2160 ? L" (4K)" : h == 1440 ? L" (1440p)" : h == 1080 ? L" (1080p)"
                       : h == 720 ? L" (720p)" : h == 480 ? L" (480p)" : L"";
    return Fmt(L"%u×%u%s", w, h, tag);
}

std::wstring FpsText(double fps) {
    double r = std::round(fps);
    return std::fabs(fps - r) < 0.01 ? Fmt(L"%.0f", fps) : Fmt(L"%.2f", fps);
}

bool SameFps(double a, double b) { return std::fabs(a - b) < 0.01; }

int64_t MsToQpc(int ms) {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return f.QuadPart * ms / 1000;
}

class App {
public:
    int Run(HINSTANCE inst, int show);
    LRESULT MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
    LRESULT SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);

private:
    // Interfaz
    struct Label {
        RECT rc;
        std::wstring text;
        float size;
        ui::Color color;
        bool bold;
    };
    void CreateControls();
    void Layout();
    void LayoutSettings();
    void ShowSettings();
    void PaintSettings(HDC dc);
    void PaintLabels(ui::Painter& p, const std::vector<Label>& labels);
    void AddTooltip(HWND owner, HWND control, const wchar_t* text);
    void HideToBackground(bool notify);
    int P(float dip) const { return int(std::lround(dip * scale_)); }
    HWND MakeCombo(HWND parent, int id);
    HWND MakeButton(HWND parent, int id, const wchar_t* text = L"");
    void Paint(HDC dc);
    void DrawItem(const DRAWITEMSTRUCT* d);
    void SetToggle(HWND h, bool on);
    bool Toggle(HWND h) const { return GetWindowLongPtrW(h, GWLP_USERDATA) != 0; }
    void OnCommand(int id, int code);
    void Redraw();

    // Dispositivos y formatos
    void RefreshVideoDevices();
    void RefreshAudioDevices();
    void OnVideoDeviceSelected(bool fromUser);
    void RequestFormats(const std::wstring& symlink);
    void OnFormatsReady(FormatsResult* r);
    const VideoDevice* SelectedVideoDevice() const;
    const std::vector<VideoFormat>* CurrentFormats() const;
    void PopulateQuality();
    void PopulateFps();
    void PopulateFormat();
    bool ResolveFormat(VideoFormat& out, std::vector<VideoFormat>* fallbacks = nullptr) const;
    void DisplaySize(UINT32& w, UINT32& h) const;
    std::wstring ResolveAudioInput() const;
    std::wstring ResolveAudioOutput() const;
    std::wstring AudioName(const std::vector<AudioEndpoint>& list, const std::wstring& id) const;

    // Sesión
    void GoAudio();
    void GoVideo();
    void HideVideo();
    void StopAll();
    HRESULT StartVideo();
    void StopVideo();
    HRESULT RestartCapture(bool useActive);
    bool StartAudio(bool reportErrors);
    void StopAudio();
    void RestartAudio();
    void Tick();
    void ApplyRenderOptions();
    void ApplyVolume();
    void UpdateExecutionState();
    void SetSession(Session s);

    // Ventana de vídeo
    void CreateVideoWindow();
    void DestroyVideoWindow();
    void ToggleFullscreen();
    void TakeScreenshot();
    std::wstring ScreenshotFolder() const;
    void SetTopmost(bool on);
    void ShowMenu(HWND owner, POINT pt, bool tray);

    // Estado y mensajes
    void SetStatus(const std::wstring& text, ui::Color color, bool sticky);
    void ShowError(const std::wstring& text) { SetStatus(text, ui::theme::danger, true); }
    void ClearSticky() { statusSticky_ = false; }
    void UpdateRunningStatus();
    std::wstring VideoErrorText(HRESULT hr) const;
    std::wstring AudioErrorText(HRESULT hr) const;
    std::wstring BuildVideoDiagnostics(const CaptureStats& cs, const RenderStats& rs) const;
    std::wstring BuildAudioDiagnostics();

    // Bandeja
    void TrayAdd();
    void TrayRemove();
    void RestoreFromTray();

    HINSTANCE inst_ = nullptr;
    HWND hwnd_ = nullptr, video_ = nullptr, settings_ = nullptr, tooltip_ = nullptr;
    std::vector<Label> mainLabels_, settingsLabels_;
    RECT rcSettingsSep_{};
    bool autoHidden_ = false;  // la principal se ocultó sola al mostrar el vídeo
    bool balloonShown_ = false;
    HICON iconBig_ = nullptr, iconSmall_ = nullptr;
    HBRUSH bgBrush_ = nullptr, surfaceBrush_ = nullptr;
    float scale_ = 1.0f;
    Settings s_;

    HWND btnAudio_{}, btnVideo_{}, btnStop_{}, slVolume_{}, btnMute_{}, cbVideo_{}, btnRefresh_{}, cbOut_{}, btnAdv_{};
    HWND cbQuality_{}, cbFps_{}, cbFormat_{}, cbIn_{}, tgAutoAudio_{}, tgTray_{}, tgTopmost_{}, tgVsync_{}, tgFullRange_{};
    HWND tgDiag_{}, cbBuffer_{}, slDelay_{}, btnFolder_{}, btnShots_{}, tgHideVideo_{}, tgKeepDisplay_{};

    RECT rcTitle_{}, rcSubtitle_{}, rcVersion_{}, rcVolumeValue_{}, rcStatus_{}, rcTip_{}, rcLblDelay_{},
        rcDiag_{};
    std::wstring statusText_;
    ui::Color statusColor_ = ui::theme::textDim;
    bool statusSticky_ = false;
    std::wstring audioDiagText_;

    std::vector<VideoDevice> videoDevs_;
    std::vector<AudioEndpoint> inDevs_, outDevs_;
    std::map<std::wstring, std::vector<VideoFormat>> formatCache_;
    std::map<std::wstring, HRESULT> formatErrors_;
    std::set<std::wstring> formatsLoading_;
    bool populating_ = false;
    bool pendingVideo_ = false;

    Session session_ = kOff;
    bool videoActive_ = false, audioActive_ = false;
    VideoCapture* capture_ = nullptr;
    Renderer renderer_;
    AudioMonitor audio_;
    VideoFormat activeFormat_{};
    std::wstring activeSymlink_;
    HRESULT videoErr_ = S_OK;
    bool rendererLost_ = false;
    int64_t nextVideoRetry_ = 0, nextAudioRetry_ = 0;
    int videoRetryMs_ = 2000;
    uint64_t lastFrames_ = 0, lastPresented_ = 0;
    int64_t lastTickQpc_ = 0;
    double fpsIn_ = 0, fpsOut_ = 0;
    bool videoNoSignal_ = false;
    std::vector<VideoFormat> autoFallbacks_;  // siguientes candidatos si el modo automático no arranca
    bool videoBusyShown_ = false;

    bool fullscreen_ = false;
    WINDOWPLACEMENT prevPlacement_{sizeof(WINDOWPLACEMENT)};
    bool cursorHidden_ = false;
    POINT lastMouse_{-1, -1};

    bool trayActive_ = false;
    UINT taskbarCreatedMsg_ = 0;
    AudioDeviceWatcher* watcher_ = nullptr;
};

App* g_app = nullptr;

LRESULT CALLBACK MainWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_app ? g_app->MainProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK VideoWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_app ? g_app->VideoProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}
LRESULT CALLBACK SettingsWndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    return g_app ? g_app->SettingsProc(h, m, w, l) : DefWindowProcW(h, m, w, l);
}

// ============================================================================
// Arranque y disposición
// ============================================================================

int App::Run(HINSTANCE inst, int show) {
    inst_ = inst;
    g_app = this;
    s_.Load();
    iconBig_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXICON),
                                 GetSystemMetrics(SM_CYICON), 0);
    iconSmall_ = (HICON)LoadImageW(inst, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                   GetSystemMetrics(SM_CYSMICON), 0);
    bgBrush_ = CreateSolidBrush(RGB(0x15, 0x16, 0x1A));
    surfaceBrush_ = CreateSolidBrush(RGB(0x23, 0x25, 0x2B));
    taskbarCreatedMsg_ = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = MainWndProc;
    wc.hInstance = inst;
    wc.hIcon = iconBig_;
    wc.hIconSm = iconSmall_;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = bgBrush_;
    wc.lpszClassName = kMainClass;
    RegisterClassExW(&wc);

    WNDCLASSEXW vc{sizeof(vc)};
    vc.style = CS_DBLCLKS;
    vc.lpfnWndProc = VideoWndProc;
    vc.hInstance = inst;
    vc.hIcon = iconBig_;
    vc.hIconSm = iconSmall_;
    vc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    vc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    vc.lpszClassName = kVideoClass;
    RegisterClassExW(&vc);

    WNDCLASSEXW sc{sizeof(sc)};
    sc.lpfnWndProc = SettingsWndProc;
    sc.hInstance = inst;
    sc.hIcon = iconBig_;
    sc.hIconSm = iconSmall_;
    sc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    sc.hbrBackground = bgBrush_;
    sc.lpszClassName = kSettingsClass;
    RegisterClassExW(&sc);

    DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN;
    hwnd_ = CreateWindowExW(0, kMainClass, APP_NAME, style, s_.mainX, s_.mainY, 480, 640, nullptr, nullptr, inst,
                            nullptr);
    if (!hwnd_) return 1;
    RECT wr;
    GetWindowRect(hwnd_, &wr);
    if (!MonitorFromRect(&wr, MONITOR_DEFAULTTONULL))
        SetWindowPos(hwnd_, nullptr, 120, 120, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    ShowWindow(hwnd_, show);
    UpdateWindow(hwnd_);

    // Opcional: empezar a oír la consola nada más abrir.
    if (s_.autoAudio) GoAudio();

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        if (msg.hwnd && (msg.hwnd == hwnd_ || IsChild(hwnd_, msg.hwnd)) && IsDialogMessageW(hwnd_, &msg)) continue;
        if (settings_ && msg.hwnd && (msg.hwnd == settings_ || IsChild(settings_, msg.hwnd)) &&
            IsDialogMessageW(settings_, &msg))
            continue;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    DeleteObject(bgBrush_);
    DeleteObject(surfaceBrush_);
    g_app = nullptr;
    return int(msg.wParam);
}

HWND App::MakeCombo(HWND parent, int id) {
    HWND h = CreateWindowExW(0, WC_COMBOBOXW, L"",
                             WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED |
                                 CBS_HASSTRINGS,
                             0, 0, 10, 300, parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    SetWindowTheme(h, L"DarkMode_CFD", nullptr);  // la lista desplegable, con barra de desplazamiento oscura
    ui::MakeFlatCombo(h);
    return h;
}

HWND App::MakeButton(HWND parent, int id, const wchar_t* text) {
    HWND h = CreateWindowExW(0, WC_BUTTONW, text, WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, 0, 0, 10, 10,
                             parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), inst_, nullptr);
    ui::EnableHover(h);
    return h;
}

void App::SetToggle(HWND h, bool on) {
    SetWindowLongPtrW(h, GWLP_USERDATA, on ? 1 : 0);
    InvalidateRect(h, nullptr, FALSE);
}

void App::AddTooltip(HWND owner, HWND control, const wchar_t* text) {
    if (!tooltip_)
        tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP, 0, 0, 0, 0,
                                   hwnd_, nullptr, inst_, nullptr);
    TOOLINFOW ti{sizeof(ti)};
    ti.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    ti.hwnd = owner;
    ti.uId = reinterpret_cast<UINT_PTR>(control);
    ti.lpszText = const_cast<wchar_t*>(text);
    SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&ti));
}

void App::CreateControls() {
    // Ventana principal
    btnAdv_ = MakeButton(hwnd_, ID_BTN_ADV);
    btnAudio_ = MakeButton(hwnd_, ID_BTN_AUDIO, L"Solo audio");
    btnVideo_ = MakeButton(hwnd_, ID_BTN_VIDEO, L"Mostrar vídeo");
    btnStop_ = MakeButton(hwnd_, ID_BTN_STOP, L"Detener");
    slVolume_ = ui::CreateSlider(hwnd_, ID_SLIDER_VOL, 0, 100, s_.volume);
    btnMute_ = MakeButton(hwnd_, ID_BTN_MUTE);
    cbOut_ = MakeCombo(hwnd_, ID_CB_AOUT);
    cbVideo_ = MakeCombo(hwnd_, ID_CB_VIDEO);
    btnRefresh_ = MakeButton(hwnd_, ID_BTN_REFRESH);
    AddTooltip(hwnd_, btnAdv_, L"Ajustes");
    AddTooltip(hwnd_, btnRefresh_, L"Buscar dispositivos de nuevo");
    AddTooltip(hwnd_, btnMute_, L"Silenciar (M)");

    // Ventana de ajustes
    HWND w = settings_;
    cbQuality_ = MakeCombo(w, ID_CB_QUALITY);
    cbFps_ = MakeCombo(w, ID_CB_FPS);
    cbFormat_ = MakeCombo(w, ID_CB_FORMAT);
    tgVsync_ = MakeButton(w, ID_TG_VSYNC, L"Sincronización vertical (VSync)");
    tgFullRange_ = MakeButton(w, ID_TG_FULLRANGE, L"Rango de color completo");
    tgTopmost_ = MakeButton(w, ID_TG_TOPMOST, L"Vídeo siempre encima");
    tgHideVideo_ = MakeButton(w, ID_TG_HIDE_VIDEO, L"Ocultar Pasarela al mostrar el vídeo");
    tgTray_ = MakeButton(w, ID_TG_TRAY, L"Al minimizar, ir a la bandeja");
    tgAutoAudio_ = MakeButton(w, ID_TG_AUTO_AUDIO, L"Al abrir, iniciar Solo audio");
    tgKeepDisplay_ = MakeButton(w, ID_TG_KEEP_DISPLAY, L"Pantalla encendida en «Solo audio»");
    AddTooltip(w, tgKeepDisplay_, L"Evita que Windows apague la pantalla mientras suena el audio");
    cbIn_ = MakeCombo(w, ID_CB_AIN);
    cbBuffer_ = MakeCombo(w, ID_CB_BUFFER);
    slDelay_ = ui::CreateSlider(w, ID_SLIDER_DELAY, 0, 300, s_.audioDelayMs);
    tgDiag_ = MakeButton(w, ID_TG_DIAG, L"Mostrar diagnóstico");
    btnShots_ = MakeButton(w, ID_BTN_SHOTS, L"Abrir carpeta de capturas");
    btnFolder_ = MakeButton(w, ID_BTN_FOLDER, L"Abrir carpeta de configuración");
    AddTooltip(w, tgVsync_, L"Sin tearing, a cambio de hasta un refresco más de retraso");
    AddTooltip(w, tgFullRange_, L"Actívalo solo si ves los negros grisáceos o los colores lavados");

    SetToggle(tgVsync_, s_.vsync);
    SetToggle(tgFullRange_, s_.fullRange);
    SetToggle(tgTopmost_, s_.topmost);
    SetToggle(tgHideVideo_, s_.hideOnVideo);
    SetToggle(tgTray_, s_.tray);
    SetToggle(tgAutoAudio_, s_.autoAudio);
    SetToggle(tgKeepDisplay_, s_.keepDisplayOnAudio);
    SetToggle(tgDiag_, s_.diagnostics);

    for (const wchar_t* t : {L"Ultra bajo (puede chasquear)", L"Bajo (recomendado)", L"Seguro (más estable)"})
        SendMessageW(cbBuffer_, CB_ADDSTRING, 0, (LPARAM)t);
    SendMessageW(cbBuffer_, CB_SETCURSEL, s_.bufferProfile, 0);
}

void App::Layout() {
    const int W = 440;
    const int m = P(26);
    const int width = P(W) - 2 * m;
    const int comboH = P(36);
    const int labelH = P(20);
    const int icon = P(36);
    int y = P(22);
    mainLabels_.clear();

    auto place = [&](HWND c, int x, int yy, int w, int h) {
        SetWindowPos(c, nullptr, x, yy, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto placeCombo = [&](HWND c, int x, int yy, int w) {
        SetWindowPos(c, nullptr, x, yy, w, P(320), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, comboH - 6);
    };
    auto section = [&](const wchar_t* t) {
        mainLabels_.push_back({{m, y, m + width, y + P(18)}, t, 11.5f, ui::theme::textDim, true});
        y += P(18) + P(10);
    };
    auto label = [&](const wchar_t* t, int w) {
        mainLabels_.push_back({{m, y, m + w, y + labelH}, t, 13.0f, ui::theme::text, false});
    };

    // Cabecera: título y botones de icono
    // La versión va a la izquierda del engranaje, para identificar rápido qué compilación es.
    rcVersion_ = {m + width - icon - P(8) - P(70), y, m + width - icon - P(8), y + P(32)};
    rcTitle_ = {m, y, rcVersion_.left - P(4), y + P(32)};
    place(btnAdv_, m + width - icon, y, icon, icon);
    y += P(32);
    rcSubtitle_ = {m, y, m + width - icon - P(8), y + P(20)};
    y += P(20) + P(22);

    // Acciones principales
    int half = (width - P(12)) / 2;
    place(btnAudio_, m, y, half, P(92));
    place(btnVideo_, m + half + P(12), y, width - half - P(12), P(92));
    y += P(92) + P(12);
    place(btnStop_, m, y, width, P(40));
    y += P(40) + P(30);

    // Sonido
    section(L"SONIDO");
    label(L"Volumen", width / 2);
    rcVolumeValue_ = {m + width / 2, y, m + width, y + labelH};
    y += labelH + P(6);
    place(slVolume_, m - P(4), y, width - P(48), P(34));
    place(btnMute_, m + width - icon, y - P(1), icon, icon);
    y += P(34) + P(16);
    label(L"Salida de audio", width);
    y += labelH + P(8);
    placeCombo(cbOut_, m, y, width);
    y += comboH + P(30);

    // Capturadora
    section(L"CAPTURADORA");
    placeCombo(cbVideo_, m, y, width - icon - P(8));
    place(btnRefresh_, m + width - icon, y, icon, comboH);
    y += comboH + P(26);

    // Estado
    float th = ui::Painter::TextHeight(statusText_, float(width - P(40)), 13.0f * scale_);
    int statusH = std::max(P(44), int(th + 0.99f) + P(24));
    rcStatus_ = {m, y, m + width, y + statusH};
    y += statusH + P(12);

    if (session_ == kVideo) {
        rcTip_ = {m, y, m + width, y + P(44)};
        y += P(44) + P(12);
    } else {
        rcTip_ = {};
    }
    if (s_.diagnostics) {
        rcDiag_ = {m, y, m + width, y + P(132)};
        y += P(132) + P(12);
    } else {
        rcDiag_ = {};
    }
    y += P(14);

    RECT want = {0, 0, P(W), y};
    AdjustWindowRectExForDpi(&want, GetWindowLongW(hwnd_, GWL_STYLE), FALSE, GetWindowLongW(hwnd_, GWL_EXSTYLE),
                             UINT(96 * scale_));
    SetWindowPos(hwnd_, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(hwnd_, nullptr, TRUE);
}

void App::LayoutSettings() {
    const int m = P(28);
    const int colW = P(320);
    const int gap = P(36);
    const int comboH = P(36);
    const int labelH = P(20);
    settingsLabels_.clear();

    auto place = [&](HWND c, int x, int yy, int w, int h) {
        SetWindowPos(c, nullptr, x, yy, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };
    auto placeCombo = [&](HWND c, int x, int yy, int w) {
        SetWindowPos(c, nullptr, x, yy, w, P(320), SWP_NOZORDER | SWP_NOACTIVATE);
        SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, comboH - 6);
    };
    auto add = [&](RECT r, const std::wstring& t, float size, ui::Color c, bool bold) {
        settingsLabels_.push_back({r, t, size, c, bold});
    };

    int top = P(22);
    add({m, top, m + 2 * colW + gap, top + P(32)}, L"Ajustes", 20.0f, ui::theme::text, true);
    top += P(32) + P(20);

    // Columna izquierda: vídeo y ventana
    int x = m, y = top;
    add({x, y, x + colW, y + P(18)}, L"VÍDEO", 11.5f, ui::theme::textDim, true);
    y += P(28);
    add({x, y, x + colW, y + labelH}, L"Calidad", 13.0f, ui::theme::text, false);
    y += labelH + P(8);
    placeCombo(cbQuality_, x, y, colW);
    y += comboH + P(14);
    int fpsW = P(100);
    add({x, y, x + fpsW, y + labelH}, L"FPS", 13.0f, ui::theme::text, false);
    add({x + fpsW + P(10), y, x + colW, y + labelH}, L"Formato", 13.0f, ui::theme::text, false);
    y += labelH + P(8);
    placeCombo(cbFps_, x, y, fpsW);
    placeCombo(cbFormat_, x + fpsW + P(10), y, colW - fpsW - P(10));
    y += comboH + P(16);
    for (HWND t : {tgVsync_, tgFullRange_, tgTopmost_}) {
        place(t, x, y, colW, P(30));
        y += P(36);
    }
    y += P(20);
    add({x, y, x + colW, y + P(18)}, L"VENTANA", 11.5f, ui::theme::textDim, true);
    y += P(28);
    for (HWND t : {tgHideVideo_, tgTray_, tgAutoAudio_, tgKeepDisplay_}) {
        place(t, x, y, colW, P(30));
        y += P(36);
    }
    int leftBottom = y;

    // Columna derecha: audio
    x = m + colW + gap;
    y = top;
    add({x, y, x + colW, y + P(18)}, L"AUDIO", 11.5f, ui::theme::textDim, true);
    y += P(28);
    add({x, y, x + colW, y + labelH}, L"Entrada de audio", 13.0f, ui::theme::text, false);
    y += labelH + P(8);
    placeCombo(cbIn_, x, y, colW);
    y += comboH + P(14);
    add({x, y, x + colW, y + labelH}, L"Búfer de audio", 13.0f, ui::theme::text, false);
    y += labelH + P(8);
    placeCombo(cbBuffer_, x, y, colW);
    y += comboH + P(14);
    rcLblDelay_ = {x, y, x + colW, y + labelH};
    y += labelH + P(6);
    place(slDelay_, x - P(4), y, colW + P(8), P(30));
    y += P(30);
    add({x, y, x + colW, y + P(18)}, L"Retrásalo si oyes el sonido antes que la imagen.", 11.5f, ui::theme::textDim,
        false);
    y += P(18) + P(16);
    place(tgDiag_, x, y, colW, P(30));
    y += P(36) + P(14);
    place(btnShots_, x, y, colW, P(38));
    y += P(38) + P(10);
    place(btnFolder_, x, y, colW, P(38));
    y += P(38);
    int bottom = std::max(leftBottom, y) + P(22);

    rcSettingsSep_ = {m + colW + gap / 2, top, m + colW + gap / 2 + 1, bottom - P(24)};
    RECT want = {0, 0, m + 2 * colW + gap + m, bottom};
    AdjustWindowRectExForDpi(&want, GetWindowLongW(settings_, GWL_STYLE), FALSE,
                             GetWindowLongW(settings_, GWL_EXSTYLE), UINT(96 * scale_));
    SetWindowPos(settings_, nullptr, 0, 0, want.right - want.left, want.bottom - want.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    InvalidateRect(settings_, nullptr, TRUE);
}

void App::ShowSettings() {
    LayoutSettings();
    if (!IsWindowVisible(settings_)) {
        // Al lado de la ventana principal si cabe; si no, centrada sobre ella.
        RECT mr, sr;
        GetWindowRect(hwnd_, &mr);
        GetWindowRect(settings_, &sr);
        int sw = sr.right - sr.left, sh = sr.bottom - sr.top;
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
        int x = mr.right + P(8), y = mr.top;
        if (x + sw > mi.rcWork.right) x = mr.left - sw - P(8);
        if (x < mi.rcWork.left) x = mr.left + ((mr.right - mr.left) - sw) / 2;
        x = Clamp<int>(x, mi.rcWork.left, std::max<int>(mi.rcWork.left, mi.rcWork.right - sw));
        y = Clamp<int>(y, mi.rcWork.top, std::max<int>(mi.rcWork.top, mi.rcWork.bottom - sh));
        SetWindowPos(settings_, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
    }
    ShowWindow(settings_, SW_SHOW);
    SetForegroundWindow(settings_);
}

void App::Redraw() {
    for (HWND h : {btnAudio_, btnVideo_, btnStop_, btnMute_}) InvalidateRect(h, nullptr, FALSE);
    InvalidateRect(hwnd_, nullptr, FALSE);
}

// ============================================================================
// Pintado
// ============================================================================

void App::PaintLabels(ui::Painter& p, const std::vector<Label>& labels) {
    for (auto& l : labels) p.Text(l.text, l.rc, l.size * scale_, l.color, ui::Painter::Left, l.bold);
}

void App::Paint(HDC dc) {
    RECT rc;
    GetClientRect(hwnd_, &rc);
    ui::Painter p(dc, rc);
    using namespace ui::theme;
    p.Fill(rc, bg);
    p.Text(APP_NAME, rcTitle_, 24.0f * scale_, text, ui::Painter::Left, true);
    p.Text(std::wstring(L"v") + APP_VERSION, rcVersion_, 12.0f * scale_, textDim, ui::Painter::Right);
    const VideoDevice* dev = SelectedVideoDevice();
    p.Text(dev ? dev->name : L"Capturadora HDMI", rcSubtitle_, 13.0f * scale_, textDim);
    PaintLabels(p, mainLabels_);
    p.Text(s_.muted ? std::wstring(L"Silenciado") : Fmt(L"%d %%", s_.volume), rcVolumeValue_, 13.0f * scale_,
           s_.muted ? danger : textDim, ui::Painter::Right);

    // Estado: tarjeta con un punto del color del estado
    p.Round(rcStatus_, 10 * scale_, surface);
    float cy = rcStatus_.top + P(22);
    p.Circle(rcStatus_.left + P(18), cy, 4.5f * scale_, statusColor_);
    RECT st = {rcStatus_.left + P(32), rcStatus_.top + P(12), rcStatus_.right - P(14), rcStatus_.bottom - P(10)};
    p.Text(statusText_, st, 13.0f * scale_, text, ui::Painter::Left, false, true);

    if (rcTip_.bottom > rcTip_.top) {
        p.Round(rcTip_, 10 * scale_, surface);
        RECT inner = rcTip_;
        InflateRect(&inner, -P(14), 0);
        p.Text(Fmt(L"Discord: comparte la ventana «%s» con sonido.", kVideoTitle), inner, 12.5f * scale_, textDim);
    }
    if (rcDiag_.bottom > rcDiag_.top) {
        p.Round(rcDiag_, 10 * scale_, surface);
        RECT inner = rcDiag_;
        InflateRect(&inner, -P(12), -P(10));
        p.Text(audioDiagText_.empty() ? std::wstring(L"Audio: parado") : audioDiagText_, inner, 11.0f * scale_,
               textDim, ui::Painter::Left, false, true, L"Consolas");
    }
}

void App::PaintSettings(HDC dc) {
    RECT rc;
    GetClientRect(settings_, &rc);
    ui::Painter p(dc, rc);
    using namespace ui::theme;
    p.Fill(rc, bg);
    p.Fill(rcSettingsSep_, border);
    PaintLabels(p, settingsLabels_);
    p.Text(Fmt(L"Retardo de audio: %d ms", s_.audioDelayMs), rcLblDelay_, 13.0f * scale_, text);
}

LRESULT App::SettingsProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            PaintSettings(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(0x23, 0x25, 0x2B));
            SetTextColor(dc, RGB(0xE9, 0xEA, 0xEE));
            return (LRESULT)surfaceBrush_;
        }
        case WM_MEASUREITEM: {
            auto* mi = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (mi->CtlType == ODT_COMBOBOX) mi->itemHeight = (mi->itemID == UINT(-1)) ? P(30) : P(30);
            return TRUE;
        }
        case WM_DRAWITEM:
            DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            return MainProc(hwnd_, msg, wp, lp);
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE) ShowWindow(h, SW_HIDE);
            return 0;
        case WM_CLOSE:
            ShowWindow(h, SW_HIDE);
            s_.Save();
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

void App::DrawItem(const DRAWITEMSTRUCT* d) {
    using namespace ui::theme;
    // El campo de un combo propio deja un marco claro alrededor de rcItem: se
    // pinta también ese margen para que no se vea.
    RECT area = d->rcItem;
    if (d->CtlType == ODT_COMBOBOX && (d->itemState & ODS_COMBOBOXEDIT)) InflateRect(&area, 3, 3);
    ui::Painter p(d->hDC, area);
    const RECT& r = area;
    const bool disabled = (d->itemState & ODS_DISABLED) != 0;

    if (d->CtlType == ODT_COMBOBOX) {
        const bool field = (d->itemState & ODS_COMBOBOXEDIT) != 0;
        const bool selected = (d->itemState & ODS_SELECTED) != 0;
        p.Fill(r, (!field && selected) ? surfaceHover : surface);
        if (!field && selected) {
            RECT bar = {r.left + P(2), r.top + P(7), r.left + P(5), r.bottom - P(7)};
            p.Round(bar, 1.5f * scale_, accent);
        }
        if (d->itemID != UINT(-1)) {
            int len = (int)SendMessageW(d->hwndItem, CB_GETLBTEXTLEN, d->itemID, 0);
            std::wstring s(std::max(len, 0), L'\0');
            if (len > 0) SendMessageW(d->hwndItem, CB_GETLBTEXT, d->itemID, (LPARAM)s.data());
            RECT tr = r;
            tr.left += P(field ? 6 : 12);
            tr.right -= P(4);
            p.Text(s, tr, 13.0f * scale_, disabled ? textDim : text);
        }
        return;
    }
    if (d->CtlType != ODT_BUTTON) return;

    HWND h = d->hwndItem;
    const bool hover = ui::IsHover(h) && !disabled;
    const bool pressed = (d->itemState & ODS_SELECTED) != 0;
    wchar_t buf[256];
    GetWindowTextW(h, buf, 256);
    std::wstring label = buf;
    p.Fill(r, bg);
    const float radius = 8 * scale_;
    const int id = int(d->CtlID);

    if (id == ID_BTN_AUDIO || id == ID_BTN_VIDEO) {
        const bool isAudio = id == ID_BTN_AUDIO;
        const bool active = isAudio ? session_ == kAudio : session_ == kVideo;
        ui::Color fill = active ? (hover ? accentHover : accent) : (hover ? surfaceHover : surface);
        if (pressed) fill.a = 0.85f;
        p.Round(r, 12 * scale_, fill);
        RECT icon = {r.left + P(14), r.top, r.left + P(46), r.bottom};
        p.Text(isAudio ? L"" : L"", icon, 22.0f * scale_, active ? accentText : text,
               ui::Painter::Center, false, false, ui::IconFont());
        RECT title = {r.left + P(52), r.top + P(14), r.right - P(8), r.top + P(38)};
        p.Text(label, title, 15.0f * scale_, active ? accentText : text, ui::Painter::Left, true);
        std::wstring sub;
        if (isAudio)
            sub = session_ == kAudio ? L"Escuchando · pulsa para parar"
                : session_ == kVideo ? L"Cerrar el vídeo y seguir oyendo"
                                     : L"Oír la consola, sin vídeo";
        else
            sub = session_ == kVideo ? L"Abierto · pulsa para ocultar" : L"Para jugar o para Discord";
        RECT subR = {r.left + P(52), r.top + P(40), r.right - P(10), r.bottom - P(6)};
        p.Text(sub, subR, 11.5f * scale_, active ? ui::Rgb(0xFFFFFF, 0.85f) : textDim, ui::Painter::Left, false, true);
    } else if (id == ID_BTN_STOP) {
        ui::Color fill = disabled ? surface : (hover ? ui::Rgb(0x4A2A2E) : ui::Rgb(0x3A2427));
        p.Round(r, radius, fill);
        p.Text(label, r, 13.5f * scale_, disabled ? textDim : ui::Rgb(0xFF9A9A), ui::Painter::Center);
    } else if (id == ID_BTN_REFRESH || id == ID_BTN_MUTE) {
        p.Round(r, radius, hover ? surfaceHover : surface);
        const wchar_t* glyph = id == ID_BTN_REFRESH ? L"" : (s_.muted ? L"" : L"");
        p.Text(glyph, r, 15.0f * scale_, (id == ID_BTN_MUTE && s_.muted) ? danger : text, ui::Painter::Center, false,
               false, ui::IconFont());
    } else if (id == ID_BTN_ADV) {
        if (hover) p.Round(r, radius, surfaceHover);
        p.Text(L"\uE713", r, 15.0f * scale_, hover ? text : textDim,
               ui::Painter::Center, false, false, ui::IconFont());
    } else if (id == ID_BTN_FOLDER || id == ID_BTN_SHOTS) {
        p.Round(r, radius, hover ? surfaceHover : surface);
        p.Text(label, r, 13.0f * scale_, text, ui::Painter::Center);
    } else {
        // Interruptor
        bool on = Toggle(h);
        int sw = P(36), sh = P(20);
        RECT trk = {r.left, r.top + (r.bottom - r.top - sh) / 2, r.left + sw, r.top + (r.bottom - r.top + sh) / 2};
        p.Round(trk, sh / 2.0f, on ? (hover ? accentHover : accent) : (hover ? surfaceHover : track));
        float knob = sh / 2.0f - P(3);
        float cx = on ? trk.right - sh / 2.0f : trk.left + sh / 2.0f;
        p.Circle(cx, (trk.top + trk.bottom) / 2.0f, knob, on ? accentText : textDim);
        RECT tr = r;
        tr.left += sw + P(10);
        p.Text(label, tr, 13.0f * scale_, text);
    }
}

// ============================================================================
// Dispositivos y formatos
// ============================================================================

const VideoDevice* App::SelectedVideoDevice() const {
    if (!cbVideo_) return nullptr;
    int i = (int)SendMessageW(cbVideo_, CB_GETCURSEL, 0, 0);
    if (i < 0 || i >= (int)videoDevs_.size()) return nullptr;
    return &videoDevs_[i];
}

const std::vector<VideoFormat>* App::CurrentFormats() const {
    const VideoDevice* d = SelectedVideoDevice();
    if (!d) return nullptr;
    auto it = formatCache_.find(d->symlink);
    return it == formatCache_.end() ? nullptr : &it->second;
}

void App::RefreshVideoDevices() {
    populating_ = true;
    videoDevs_ = EnumerateVideoDevices();
    SendMessageW(cbVideo_, CB_RESETCONTENT, 0, 0);
    int sel = -1;
    for (size_t i = 0; i < videoDevs_.size(); ++i) {
        SendMessageW(cbVideo_, CB_ADDSTRING, 0, (LPARAM)videoDevs_[i].name.c_str());
        if (videoDevs_[i].symlink == s_.videoDevice) sel = int(i);
    }
    if (sel < 0) {
        for (size_t i = 0; i < videoDevs_.size(); ++i)
            if (!s_.videoName.empty() && videoDevs_[i].name == s_.videoName) { sel = int(i); break; }
    }
    if (videoDevs_.empty()) {
        SendMessageW(cbVideo_, CB_ADDSTRING, 0, (LPARAM)L"No se encontró ninguna capturadora");
        SendMessageW(cbVideo_, CB_SETCURSEL, 0, 0);
        EnableWindow(cbVideo_, FALSE);
    } else {
        EnableWindow(cbVideo_, TRUE);
        SendMessageW(cbVideo_, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
    }
    populating_ = false;
    // Solo se recuerda como "la capturadora" si no había otra guardada.
    OnVideoDeviceSelected(s_.videoDevice.empty());
}

void App::OnVideoDeviceSelected(bool fromUser) {
    const VideoDevice* d = SelectedVideoDevice();
    if (d && fromUser) {
        s_.videoDevice = d->symlink;
        s_.videoName = d->name;
    }
    if (d) RequestFormats(d->symlink);
    PopulateQuality();
    // La entrada automática depende de la capturadora elegida.
    RefreshAudioDevices();
    InvalidateRect(hwnd_, &rcSubtitle_, FALSE);
}

void App::RequestFormats(const std::wstring& symlink) {
    if (formatCache_.count(symlink) || formatsLoading_.count(symlink)) return;
    if (videoActive_ && activeSymlink_ == symlink) return;  // en uso por nosotros mismos
    formatsLoading_.insert(symlink);
    HWND target = hwnd_;
    std::thread([target, symlink] {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        auto* r = new FormatsResult;
        r->symlink = symlink;
        r->hr = EnumerateVideoFormats(symlink, r->formats);
        CoUninitialize();
        if (!PostMessageW(target, WM_APP_FORMATS_READY, 0, reinterpret_cast<LPARAM>(r))) delete r;
    }).detach();
}

void App::OnFormatsReady(FormatsResult* r) {
    formatsLoading_.erase(r->symlink);
    if (SUCCEEDED(r->hr) && !r->formats.empty()) {
        formatCache_[r->symlink] = std::move(r->formats);
        formatErrors_.erase(r->symlink);
    } else {
        formatErrors_[r->symlink] = r->hr;
        Log(L"No se pudieron leer los modos: %s", HrHex(r->hr).c_str());
    }
    const VideoDevice* d = SelectedVideoDevice();
    if (d && d->symlink == r->symlink) {
        PopulateQuality();
        if (pendingVideo_) {
            pendingVideo_ = false;
            GoVideo();
        }
    }
    delete r;
}

void App::PopulateQuality() {
    populating_ = true;
    SendMessageW(cbQuality_, CB_RESETCONTENT, 0, 0);
    const auto* fm = CurrentFormats();
    const VideoDevice* d = SelectedVideoDevice();
    if (!fm) {
        std::wstring msg = L"—";
        if (d && formatsLoading_.count(d->symlink)) msg = L"Leyendo los modos…";
        else if (d && formatErrors_.count(d->symlink)) msg = L"No se pudieron leer los modos";
        SendMessageW(cbQuality_, CB_ADDSTRING, 0, (LPARAM)msg.c_str());
        SendMessageW(cbQuality_, CB_SETCURSEL, 0, 0);
        EnableWindow(cbQuality_, FALSE);
        populating_ = false;
        PopulateFps();
        return;
    }
    UINT32 dw = 0, dh = 0;
    DisplaySize(dw, dh);
    int autoIdx = ChooseAutoFormat(*fm, dw, dh);
    std::wstring autoText = L"Automática (mejor calidad nativa)";
    if (autoIdx >= 0)
        autoText = Fmt(L"Automática · %s · %s fps", ResolutionText((*fm)[autoIdx].width, (*fm)[autoIdx].height).c_str(),
                       FpsText((*fm)[autoIdx].Fps()).c_str());
    SendMessageW(cbQuality_, CB_ADDSTRING, 0, (LPARAM)autoText.c_str());
    SendMessageW(cbQuality_, CB_SETITEMDATA, 0, 0);

    std::vector<std::pair<UINT32, UINT32>> res;
    for (auto& f : *fm)
        if (SubtypePreference(f.subtype) < 8 &&
            std::find(res.begin(), res.end(), std::make_pair(f.width, f.height)) == res.end())
            res.push_back({f.width, f.height});
    std::sort(res.begin(), res.end(), [](auto& a, auto& b) {
        return uint64_t(a.first) * a.second != uint64_t(b.first) * b.second
                   ? uint64_t(a.first) * a.second > uint64_t(b.first) * b.second
                   : a.first > b.first;
    });
    int sel = 0;
    for (auto& rr : res) {
        int i = (int)SendMessageW(cbQuality_, CB_ADDSTRING, 0, (LPARAM)ResolutionText(rr.first, rr.second).c_str());
        SendMessageW(cbQuality_, CB_SETITEMDATA, i, (LPARAM)((rr.first << 16) | rr.second));
        if (rr.first == s_.width && rr.second == s_.height) sel = i;
    }
    if (sel == 0) s_.width = s_.height = 0;
    SendMessageW(cbQuality_, CB_SETCURSEL, sel, 0);
    EnableWindow(cbQuality_, TRUE);
    populating_ = false;
    PopulateFps();
}

void App::PopulateFps() {
    populating_ = true;
    SendMessageW(cbFps_, CB_RESETCONTENT, 0, 0);
    const auto* fm = CurrentFormats();
    if (!fm || s_.width == 0) {
        SendMessageW(cbFps_, CB_ADDSTRING, 0, (LPARAM)L"Auto");
        SendMessageW(cbFps_, CB_SETCURSEL, 0, 0);
        EnableWindow(cbFps_, FALSE);
        populating_ = false;
        PopulateFormat();
        return;
    }
    std::vector<double> rates;
    for (auto& f : *fm)
        if (f.width == s_.width && f.height == s_.height && SubtypePreference(f.subtype) < 8 &&
            std::none_of(rates.begin(), rates.end(), [&](double r) { return SameFps(r, f.Fps()); }))
            rates.push_back(f.Fps());
    std::sort(rates.rbegin(), rates.rend());
    int sel = 0;
    for (size_t i = 0; i < rates.size(); ++i) {
        SendMessageW(cbFps_, CB_ADDSTRING, 0, (LPARAM)FpsText(rates[i]).c_str());
        SendMessageW(cbFps_, CB_SETITEMDATA, i, (LPARAM)std::lround(rates[i] * 1000));
        if (SameFps(rates[i], s_.fps)) sel = int(i);
    }
    if (!rates.empty()) s_.fps = rates[sel];
    SendMessageW(cbFps_, CB_SETCURSEL, sel, 0);
    EnableWindow(cbFps_, !rates.empty());
    populating_ = false;
    PopulateFormat();
}

void App::PopulateFormat() {
    populating_ = true;
    SendMessageW(cbFormat_, CB_RESETCONTENT, 0, 0);
    SendMessageW(cbFormat_, CB_ADDSTRING, 0, (LPARAM)L"Automático");
    int sel = 0;
    const auto* fm = CurrentFormats();
    if (fm) {
        UINT32 w = s_.width, h = s_.height;
        double fps = s_.fps;
        if (w == 0) {
            UINT32 dw = 0, dh = 0;
            DisplaySize(dw, dh);
            int a = ChooseAutoFormat(*fm, dw, dh);
            if (a >= 0) { w = (*fm)[a].width; h = (*fm)[a].height; fps = (*fm)[a].Fps(); }
        }
        std::vector<GUID> subs;
        for (auto& f : *fm)
            if (f.width == w && f.height == h && SameFps(f.Fps(), fps) && SubtypePreference(f.subtype) < 8 &&
                std::find(subs.begin(), subs.end(), f.subtype) == subs.end())
                subs.push_back(f.subtype);
        std::sort(subs.begin(), subs.end(),
                  [](const GUID& a, const GUID& b) { return SubtypePreference(a) < SubtypePreference(b); });
        for (auto& sbt : subs) {
            int i = (int)SendMessageW(cbFormat_, CB_ADDSTRING, 0, (LPARAM)SubtypeName(sbt));
            if (s_.format == SubtypeName(sbt)) sel = i;
        }
    }
    SendMessageW(cbFormat_, CB_SETCURSEL, sel, 0);
    EnableWindow(cbFormat_, fm != nullptr);
    populating_ = false;
}

void App::DisplaySize(UINT32& w, UINT32& h) const {
    MONITORINFO mi{sizeof(mi)};
    GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &mi);
    w = UINT32(mi.rcMonitor.right - mi.rcMonitor.left);
    h = UINT32(mi.rcMonitor.bottom - mi.rcMonitor.top);
}

bool App::ResolveFormat(VideoFormat& out, std::vector<VideoFormat>* fallbacks) const {
    if (fallbacks) fallbacks->clear();
    const auto* fm = CurrentFormats();
    if (!fm || fm->empty()) return false;
    std::vector<VideoFormat> pool;
    for (auto& f : *fm)
        if (s_.format.empty() || s_.format == SubtypeName(f.subtype)) pool.push_back(f);
    if (pool.empty()) pool = *fm;

    if (s_.width) {
        std::vector<VideoFormat> match;
        for (auto& f : pool)
            if (f.width == s_.width && f.height == s_.height && (s_.fps == 0 || SameFps(f.Fps(), s_.fps)))
                match.push_back(f);
        if (!match.empty()) {
            std::sort(match.begin(), match.end(), [](const VideoFormat& a, const VideoFormat& b) {
                if (!SameFps(a.Fps(), b.Fps())) return a.Fps() > b.Fps();
                return SubtypePreference(a.subtype) < SubtypePreference(b.subtype);
            });
            if (SubtypePreference(match[0].subtype) < 8) {
                out = match[0];
                return true;
            }
        }
    }
    UINT32 dw = 0, dh = 0;
    DisplaySize(dw, dh);
    auto ranked = RankAutoFormats(pool, dw, dh);
    if (ranked.empty()) return false;
    out = pool[ranked[0]];
    if (fallbacks)
        for (size_t i = 1; i < ranked.size() && fallbacks->size() < 2; ++i) {
            const VideoFormat& f = pool[ranked[i]];
            if (f.width != out.width || f.height != out.height || !SameFps(f.Fps(), out.Fps()))
                if (std::none_of(fallbacks->begin(), fallbacks->end(), [&](const VideoFormat& g) {
                        return g.width == f.width && g.height == f.height && SameFps(g.Fps(), f.Fps());
                    }))
                    fallbacks->push_back(f);
        }
    return true;
}

std::wstring App::AudioName(const std::vector<AudioEndpoint>& list, const std::wstring& id) const {
    for (auto& e : list)
        if (e.id == id) return e.name;
    return {};
}

std::wstring App::ResolveAudioInput() const {
    if (!s_.audioIn.empty())
        for (auto& e : inDevs_)
            if (e.id == s_.audioIn) return e.id;
    const VideoDevice* d = SelectedVideoDevice();
    if (!d) return {};
    int i = FindAssociatedAudioInput(*d, inDevs_);
    return i >= 0 ? inDevs_[i].id : std::wstring();
}

std::wstring App::ResolveAudioOutput() const {
    if (!s_.audioOut.empty())
        for (auto& e : outDevs_)
            if (e.id == s_.audioOut) return e.id;
    return {};  // predeterminada
}

void App::RefreshAudioDevices() {
    populating_ = true;
    inDevs_ = EnumerateAudioEndpoints(eCapture);
    outDevs_ = EnumerateAudioEndpoints(eRender);

    SendMessageW(cbIn_, CB_RESETCONTENT, 0, 0);
    std::wstring autoText = L"Automática (la de la capturadora)";
    if (const VideoDevice* d = SelectedVideoDevice()) {
        int i = FindAssociatedAudioInput(*d, inDevs_);
        autoText = i >= 0 ? L"Automática · " + inDevs_[i].name : L"Automática · no encontrada, elige una";
    }
    SendMessageW(cbIn_, CB_ADDSTRING, 0, (LPARAM)autoText.c_str());
    int selIn = 0;
    for (size_t i = 0; i < inDevs_.size(); ++i) {
        SendMessageW(cbIn_, CB_ADDSTRING, 0, (LPARAM)inDevs_[i].name.c_str());
        if (inDevs_[i].id == s_.audioIn) selIn = int(i) + 1;
    }
    SendMessageW(cbIn_, CB_SETCURSEL, selIn, 0);

    SendMessageW(cbOut_, CB_RESETCONTENT, 0, 0);
    std::wstring defName;
    for (auto& e : outDevs_)
        if (e.isDefault) defName = e.name;
    std::wstring defText = defName.empty() ? L"Predeterminada de Windows" : L"Predeterminada · " + defName;
    SendMessageW(cbOut_, CB_ADDSTRING, 0, (LPARAM)defText.c_str());
    int selOut = 0;
    for (size_t i = 0; i < outDevs_.size(); ++i) {
        SendMessageW(cbOut_, CB_ADDSTRING, 0, (LPARAM)outDevs_[i].name.c_str());
        if (outDevs_[i].id == s_.audioOut) selOut = int(i) + 1;
    }
    SendMessageW(cbOut_, CB_SETCURSEL, selOut, 0);
    populating_ = false;
}

// ============================================================================
// Sesión
// ============================================================================

void App::SetSession(Session s) {
    session_ = s;
    EnableWindow(btnStop_, s != kOff);
    UpdateExecutionState();
    UpdateRunningStatus();
    Layout();
    Redraw();
    s_.Save();
}

void App::GoAudio() {
    ClearSticky();
    if (session_ == kVideo) {  // cerrar el vídeo y seguir oyendo
        StopVideo();
        SetSession(audioActive_ ? kAudio : kOff);
        return;
    }
    if (session_ == kAudio) {  // segundo clic: parar
        StopAll();
        return;
    }
    if (!StartAudio(true)) return;
    SetSession(kAudio);
}

void App::GoVideo() {
    ClearSticky();
    if (session_ == kVideo) {  // segundo clic: ocultar el vídeo, el audio sigue
        HideVideo();
        return;
    }
    const VideoDevice* d = SelectedVideoDevice();
    if (!d) {
        ShowError(L"Conecta y selecciona una capturadora.");
        return;
    }
    if (!CurrentFormats()) {
        if (formatsLoading_.count(d->symlink)) {
            pendingVideo_ = true;
            SetStatus(L"Leyendo los modos de la capturadora…", ui::theme::textDim, false);
            return;
        }
        HRESULT hr = formatErrors_.count(d->symlink) ? formatErrors_[d->symlink] : E_FAIL;
        formatErrors_.erase(d->symlink);
        RequestFormats(d->symlink);  // reintenta en segundo plano
        pendingVideo_ = true;
        ShowError(Fmt(L"No se pudieron leer los modos de la capturadora (código %s). Reintentando… ¿La está usando "
                      L"otra aplicación?",
                      HrHex(hr).c_str()));
        return;
    }
    bool hadAudio = audioActive_;
    if (!audioActive_) StartAudio(false);
    HRESULT hr = StartVideo();
    if (FAILED(hr)) {
        if (!hadAudio && session_ == kOff) StopAudio();
        ShowError(VideoErrorText(hr));
        return;
    }
    SetSession(kVideo);
    if (s_.hideOnVideo && IsWindowVisible(hwnd_)) {
        HideToBackground(false);
        autoHidden_ = true;
    }
    if (!audioActive_ && ResolveAudioInput().empty())
        SetStatus(L"Vídeo en marcha, pero sin audio: no se encontró la entrada de la capturadora. Elígela en "
                  L"Ajustes › Entrada de audio.",
                  ui::theme::warn, true);
}

void App::HideVideo() {
    if (session_ != kVideo) return;
    StopVideo();
    if (autoHidden_) RestoreFromTray();
    SetSession(audioActive_ || !ResolveAudioInput().empty() ? kAudio : kOff);
    if (session_ == kOff) StopAudio();
}

void App::StopAll() {
    pendingVideo_ = false;
    StopVideo();
    if (autoHidden_) RestoreFromTray();
    StopAudio();
    ClearSticky();
    SetSession(kOff);
    SetStatus(L"Detenido.", ui::theme::textDim, false);
}

HRESULT App::StartVideo() {
    const VideoDevice* d = SelectedVideoDevice();
    if (!d) return E_APP_NO_DEVICE;
    VideoFormat fmt;
    if (!ResolveFormat(fmt, &autoFallbacks_)) return MF_E_INVALIDMEDIATYPE;
    videoBusyShown_ = false;
    CreateVideoWindow();
    HRESULT hr = S_OK;
    renderer_.SetErrorCallback(hwnd_);
    if (!renderer_.Start(video_, &hr)) {
        Log(L"Renderer: %s", HrHex(hr).c_str());
        DestroyVideoWindow();
        return E_APP_GPU;
    }
    ApplyRenderOptions();
    renderer_.SetHintText(L"Doble clic: pantalla completa · Esc: salir · C: captura · Clic derecho: opciones", 4000);
    capture_ = VideoCapture::Create();
    hr = capture_->Start(d->symlink, fmt, &renderer_);
    if (FAILED(hr)) {
        capture_->Release();
        capture_ = nullptr;
        renderer_.Stop();
        DestroyVideoWindow();
        return hr;
    }
    activeFormat_ = fmt;
    activeSymlink_ = d->symlink;
    videoActive_ = true;
    videoErr_ = S_OK;
    rendererLost_ = false;
    videoNoSignal_ = false;
    videoRetryMs_ = 2000;
    lastFrames_ = lastPresented_ = 0;
    lastTickQpc_ = QpcNow();
    return S_OK;
}

void App::StopVideo() {
    if (capture_) {
        capture_->Stop();
        capture_->Release();
        capture_ = nullptr;
    }
    renderer_.Stop();
    DestroyVideoWindow();
    videoActive_ = false;
}

HRESULT App::RestartCapture(bool useActive) {
    if (capture_) {
        capture_->Stop();
        capture_->Release();
        capture_ = nullptr;
    }
    std::wstring symlink = activeSymlink_;
    VideoFormat fmt = activeFormat_;
    if (!useActive) {
        const VideoDevice* d = SelectedVideoDevice();
        if (!d || !ResolveFormat(fmt, &autoFallbacks_)) return E_APP_NO_DEVICE;
        videoBusyShown_ = false;
        symlink = d->symlink;
    }
    VideoCapture* c = VideoCapture::Create();
    HRESULT hr = c->Start(symlink, fmt, &renderer_);
    if (SUCCEEDED(hr)) {
        capture_ = c;
        activeFormat_ = fmt;
        activeSymlink_ = symlink;
        videoErr_ = S_OK;
    } else {
        c->Release();
    }
    Log(L"Reinicio de captura: %s", HrHex(hr).c_str());
    return hr;
}

bool App::StartAudio(bool reportErrors) {
    std::wstring in = ResolveAudioInput();
    if (in.empty()) {
        if (reportErrors)
            ShowError(L"No se encontró la entrada de audio de la capturadora. Elígela en Ajustes › Entrada de audio.");
        return false;
    }
    AudioSettings as;
    as.inputId = in;
    as.outputId = ResolveAudioOutput();
    as.profile = AudioBufferProfile(s_.bufferProfile);
    as.extraDelayMs = s_.audioDelayMs;
    std::wstring detail;
    HRESULT hr = audio_.Start(as, &detail);
    if (FAILED(hr)) {
        if (reportErrors) ShowError(AudioErrorText(hr));
        return false;
    }
    audioActive_ = true;
    ApplyVolume();
    return true;
}

void App::StopAudio() {
    audio_.Stop();
    audioActive_ = false;
}

void App::RestartAudio() {
    if (session_ == kOff) return;
    StopAudio();
    ClearSticky();
    if (!StartAudio(true)) nextAudioRetry_ = QpcNow() + MsToQpc(3000);
    UpdateRunningStatus();
}

void App::ApplyVolume() {
    audio_.SetVolume(VolumeToGain(s_.volume));
    audio_.SetMuted(s_.muted);
}

void App::ApplyRenderOptions() {
    RenderOptions o;
    o.vsync = s_.vsync;
    o.fullRange = s_.fullRange;
    o.showDiagnostics = s_.diagnostics;
    renderer_.SetOptions(o);
}

void App::UpdateExecutionState() {
    if (session_ == kVideo)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    else if (session_ == kAudio)
        SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED |
                                (s_.keepDisplayOnAudio ? ES_DISPLAY_REQUIRED : 0));
    else
        SetThreadExecutionState(ES_CONTINUOUS);
}

void App::Tick() {
    const int64_t now = QpcNow();

    if (videoActive_) {
        if (rendererLost_) {
            rendererLost_ = false;
            StopVideo();
            HRESULT hr = StartVideo();
            if (FAILED(hr)) {
                ShowError(VideoErrorText(hr));
                SetSession(audioActive_ ? kAudio : kOff);
            }
        }
    }
    if (videoActive_) {
        CaptureStats cs = capture_ ? capture_->Stats() : CaptureStats{};
        RenderStats rs = renderer_.Stats();
        double dt = QpcToMs(now - lastTickQpc_) / 1000.0;
        if (dt > 0.2) {
            fpsIn_ = capture_ ? double(cs.frames - std::min(cs.frames, lastFrames_)) / dt : 0;
            fpsOut_ = double(rs.presented - std::min(rs.presented, lastPresented_)) / dt;
            lastFrames_ = cs.frames;
            lastPresented_ = rs.presented;
            lastTickQpc_ = now;
        }
        // El modo automático no arrancó (sin fotogramas): probar el siguiente candidato.
        const bool startFailed = videoErr_ == MF_E_HW_MFT_FAILED_START_STREAMING && cs.frames == 0;
        if (startFailed && s_.width == 0 && !autoFallbacks_.empty()) {
            VideoFormat next = autoFallbacks_.front();
            autoFallbacks_.erase(autoFallbacks_.begin());
            Log(L"El modo %ux%u@%.2f no arrancó; se prueba %ux%u@%.2f %s", activeFormat_.width, activeFormat_.height,
                activeFormat_.Fps(), next.width, next.height, next.Fps(), SubtypeName(next.subtype));
            activeFormat_ = next;
            videoErr_ = S_OK;
            RestartCapture(true);
            return;
        }
        if (startFailed && !videoBusyShown_) {
            videoBusyShown_ = true;
            ShowError(VideoErrorText(videoErr_));
        }
        bool stalled = !capture_ || videoErr_ != S_OK || cs.lastArrivalAgeMs > 2000;
        if (stalled) {
            bool present = std::any_of(videoDevs_.begin(), videoDevs_.end(),
                                       [&](const VideoDevice& v) { return v.symlink == activeSymlink_; });
            renderer_.SetStatusText(!present          ? L"Capturadora desconectada"
                                    : videoBusyShown_ ? L"La capturadora está ocupada (¿OBS abierto?)"
                                                      : L"Sin señal");
            if (!videoNoSignal_) {
                videoNoSignal_ = true;
                UpdateRunningStatus();
            }
            if (present && now >= nextVideoRetry_ &&
                (videoErr_ != S_OK || !capture_ || cs.lastArrivalAgeMs > 4000)) {
                videoErr_ = S_OK;
                if (capture_) lastFrames_ = 0;
                RestartCapture(true);
                nextVideoRetry_ = now + MsToQpc(videoRetryMs_);
                videoRetryMs_ = std::min(videoRetryMs_ * 2, 10000);
            }
        } else if (videoNoSignal_) {
            videoNoSignal_ = false;
            videoRetryMs_ = 2000;
            if (videoBusyShown_) {
                videoBusyShown_ = false;
                ClearSticky();
            }
            renderer_.SetStatusText(L"");
            UpdateRunningStatus();
        }
        if (s_.diagnostics) renderer_.SetDiagnosticsText(BuildVideoDiagnostics(cs, rs) + L"\n" + BuildAudioDiagnostics());
    }

    if (audioActive_ && audio_.Failure() != AudioFailure::None) {
        Log(L"Audio perdido (%s); se reintentará", HrHex(audio_.FailureCode()).c_str());
        StopAudio();
        nextAudioRetry_ = now + MsToQpc(1500);
        SetStatus(L"Se perdió el audio; reintentando…", ui::theme::warn, false);
    }
    if (session_ != kOff && !audioActive_ && now >= nextAudioRetry_ && !ResolveAudioInput().empty()) {
        if (StartAudio(false)) {
            ClearSticky();
            UpdateRunningStatus();
        } else {
            nextAudioRetry_ = now + MsToQpc(3000);
        }
    }

    if (s_.diagnostics) {
        audioDiagText_ = BuildAudioDiagnostics();
        InvalidateRect(hwnd_, &rcDiag_, FALSE);
    }
    if (session_ != kOff && !statusSticky_) UpdateRunningStatus();
}

// ============================================================================
// Ventana de vídeo
// ============================================================================

void App::CreateVideoWindow() {
    if (video_) return;
    RECT r = s_.videoRect;
    bool valid = (r.right - r.left) >= 320 && (r.bottom - r.top) >= 180 && MonitorFromRect(&r, MONITOR_DEFAULTTONULL);
    if (!valid) {
        HMONITOR mon = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(mon, &mi);
        UINT dpi = GetDpiForWindow(hwnd_);
        int w = MulDiv(1280, dpi, 96), h = MulDiv(720, dpi, 96);
        int maxW = (mi.rcWork.right - mi.rcWork.left) * 8 / 10, maxH = (mi.rcWork.bottom - mi.rcWork.top) * 8 / 10;
        if (w > maxW) { h = h * maxW / w; w = maxW; }
        if (h > maxH) { w = w * maxH / h; h = maxH; }
        RECT c = {0, 0, w, h};
        AdjustWindowRectExForDpi(&c, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
        int ww = c.right - c.left, wh = c.bottom - c.top;
        r.left = mi.rcWork.left + ((mi.rcWork.right - mi.rcWork.left) - ww) / 2;
        r.top = mi.rcWork.top + ((mi.rcWork.bottom - mi.rcWork.top) - wh) / 2;
        r.right = r.left + ww;
        r.bottom = r.top + wh;
    }
    video_ = CreateWindowExW(WS_EX_APPWINDOW | (s_.topmost ? WS_EX_TOPMOST : 0), kVideoClass, kVideoTitle,
                             WS_OVERLAPPEDWINDOW, r.left, r.top, r.right - r.left, r.bottom - r.top, nullptr, nullptr,
                             inst_, nullptr);
    ui::ApplyDarkTitleBar(video_);
    fullscreen_ = false;
    cursorHidden_ = false;
    ShowWindow(video_, SW_SHOW);
    SetForegroundWindow(video_);
}

void App::DestroyVideoWindow() {
    if (!video_) return;
    WINDOWPLACEMENT wp{sizeof(wp)};
    if (fullscreen_) wp = prevPlacement_;
    else GetWindowPlacement(video_, &wp);
    if (wp.rcNormalPosition.right > wp.rcNormalPosition.left) s_.videoRect = wp.rcNormalPosition;
    HWND v = video_;
    video_ = nullptr;
    DestroyWindow(v);
    fullscreen_ = false;
}

void App::ToggleFullscreen() {
    if (!video_) return;
    DWORD style = GetWindowLongW(video_, GWL_STYLE);
    if (!fullscreen_) {
        GetWindowPlacement(video_, &prevPlacement_);
        MONITORINFO mi{sizeof(mi)};
        GetMonitorInfoW(MonitorFromWindow(video_, MONITOR_DEFAULTTONEAREST), &mi);
        // Ventana sin bordes del tamaño del monitor (no pantalla completa exclusiva):
        // misma latencia con flip-model y Discord puede seguir capturándola.
        SetWindowLongW(video_, GWL_STYLE, (style & ~WS_OVERLAPPEDWINDOW) | WS_POPUP);
        SetWindowPos(video_, s_.topmost ? HWND_TOPMOST : HWND_TOP, mi.rcMonitor.left, mi.rcMonitor.top,
                     mi.rcMonitor.right - mi.rcMonitor.left, mi.rcMonitor.bottom - mi.rcMonitor.top,
                     SWP_FRAMECHANGED | SWP_NOOWNERZORDER);
        fullscreen_ = true;
        SetTimer(video_, TIMER_CURSOR, 2000, nullptr);
    } else {
        SetWindowLongW(video_, GWL_STYLE, (style & ~WS_POPUP) | WS_OVERLAPPEDWINDOW);
        SetWindowPlacement(video_, &prevPlacement_);
        SetWindowPos(video_, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fullscreen_ = false;
        KillTimer(video_, TIMER_CURSOR);
        if (cursorHidden_) {
            cursorHidden_ = false;
            SetCursor(LoadCursorW(nullptr, IDC_ARROW));
        }
    }
}

std::wstring App::ScreenshotFolder() const {
    std::wstring dir;
    wchar_t* pictures = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Pictures, 0, nullptr, &pictures))) {
        dir = std::wstring(pictures) + L"\\Pasarela";
        CoTaskMemFree(pictures);
    } else {
        dir = DataDirectory() + L"\\Capturas";
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    return dir;
}

void App::TakeScreenshot() {
    if (!videoActive_) return;
    renderer_.RequestScreenshot(ScreenshotFolder());
}

void App::SetTopmost(bool on) {
    s_.topmost = on;
    SetToggle(tgTopmost_, on);
    if (video_)
        SetWindowPos(video_, on ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void App::ShowMenu(HWND owner, POINT pt, bool tray) {
    HMENU m = CreatePopupMenu();
    if (tray) {
        AppendMenuW(m, MF_STRING, IDM_SHOW, L"Mostrar Pasarela");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (session_ == kAudio ? MF_CHECKED : 0), IDM_AUDIO, L"Solo audio");
        AppendMenuW(m, MF_STRING | (session_ == kVideo ? MF_CHECKED : 0), IDM_VIDEO, L"Mostrar vídeo");
        AppendMenuW(m, MF_STRING | (s_.muted ? MF_CHECKED : 0), IDM_MUTE, L"Silenciar");
        if (session_ == kVideo) AppendMenuW(m, MF_STRING, IDM_SCREENSHOT, L"Hacer captura");
        AppendMenuW(m, MF_STRING | (session_ == kOff ? MF_GRAYED : 0), IDM_STOP, L"Detener");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_EXIT, L"Salir");
    } else {
        AppendMenuW(m, MF_STRING, IDM_SCREENSHOT, L"Hacer captura\tC");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING | (fullscreen_ ? MF_CHECKED : 0), IDM_FULLSCREEN, L"Pantalla completa\tDoble clic");
        AppendMenuW(m, MF_STRING | (s_.topmost ? MF_CHECKED : 0), IDM_TOPMOST, L"Siempre encima\tT");
        AppendMenuW(m, MF_STRING | (s_.muted ? MF_CHECKED : 0), IDM_MUTE, L"Silenciar\tM");
        AppendMenuW(m, MF_STRING | (s_.diagnostics ? MF_CHECKED : 0), IDM_DIAG, L"Diagnóstico\tF3");
        AppendMenuW(m, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(m, MF_STRING, IDM_HIDE_VIDEO, L"Ocultar vídeo (seguir oyendo)");
        AppendMenuW(m, MF_STRING, IDM_STOP, L"Detener todo");
    }
    SetForegroundWindow(owner);
    int cmd = TrackPopupMenu(m, TPM_RETURNCMD | TPM_RIGHTBUTTON, pt.x, pt.y, 0, owner, nullptr);
    DestroyMenu(m);
    if (cmd) OnCommand(cmd, 0);
}

LRESULT App::VideoProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_SIZE:
            renderer_.NotifyResize();
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            renderer_.NotifyResize();
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            ToggleFullscreen();
            return 0;
        case WM_RBUTTONUP: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ClientToScreen(h, &pt);
            ShowMenu(h, pt, false);
            return 0;
        }
        case WM_KEYDOWN:
            if (wp == VK_ESCAPE && fullscreen_) ToggleFullscreen();
            else if (wp == VK_F11) ToggleFullscreen();
            else if (wp == 'C') OnCommand(IDM_SCREENSHOT, 0);
            else if (wp == 'M') OnCommand(IDM_MUTE, 0);
            else if (wp == 'T') OnCommand(IDM_TOPMOST, 0);
            else if (wp == VK_F3) OnCommand(IDM_DIAG, 0);
            return 0;
        case WM_SYSKEYDOWN:
            if (wp == VK_RETURN) {
                ToggleFullscreen();
                return 0;
            }
            break;
        case WM_MOUSEMOVE: {
            POINT pt = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (pt.x == lastMouse_.x && pt.y == lastMouse_.y) return 0;
            lastMouse_ = pt;
            if (cursorHidden_) {
                cursorHidden_ = false;
                SetCursor(LoadCursorW(nullptr, IDC_ARROW));
            }
            if (fullscreen_) SetTimer(h, TIMER_CURSOR, 2000, nullptr);
            return 0;
        }
        case WM_TIMER:
            if (wp == TIMER_CURSOR) {
                KillTimer(h, TIMER_CURSOR);
                if (fullscreen_) {
                    cursorHidden_ = true;
                    SetCursor(nullptr);
                }
            }
            return 0;
        case WM_SETCURSOR:
            if (cursorHidden_ && LOWORD(lp) == HTCLIENT) {
                SetCursor(nullptr);
                return TRUE;
            }
            break;
        case WM_GETMINMAXINFO: {
            auto* mm = reinterpret_cast<MINMAXINFO*>(lp);
            mm->ptMinTrackSize = {320, 200};
            return 0;
        }
        case WM_DPICHANGED: {
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_CLOSE:
            PostMessageW(hwnd_, WM_APP_VIDEO_WINDOW_CLOSED, 0, 0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

// ============================================================================
// Estado, errores y diagnóstico
// ============================================================================

void App::SetStatus(const std::wstring& text, ui::Color color, bool sticky) {
    bool relayout = ui::Painter::TextHeight(text, float(P(396)), 13.0f * scale_) !=
                    ui::Painter::TextHeight(statusText_, float(P(396)), 13.0f * scale_);
    statusText_ = text;
    statusColor_ = color;
    statusSticky_ = sticky;
    if (relayout) Layout();
    else InvalidateRect(hwnd_, &rcStatus_, FALSE);
    if (trayActive_) TrayAdd();  // actualiza el texto del icono
}

void App::UpdateRunningStatus() {
    if (statusSticky_) return;
    if (session_ == kOff) {
        if (statusText_.empty()) SetStatus(L"Listo.", ui::theme::textDim, false);
        return;
    }
    std::wstring in = AudioName(inDevs_, audio_.Settings().inputId);
    std::wstring out = AudioName(outDevs_, audio_.Settings().outputId);
    if (audio_.Settings().outputId.empty())
        for (auto& e : outDevs_)
            if (e.isDefault) out = e.name;
    if (out.empty()) out = L"la salida predeterminada";
    std::wstring audioPart = audioActive_ ? Fmt(L"Escuchando la consola por «%s».", out.c_str())
                                          : std::wstring(L"Sin audio.");
    if (session_ == kVideo) {
        if (videoNoSignal_) {
            SetStatus(L"Sin señal de vídeo. Comprueba que la consola está encendida y el cable HDMI. " + audioPart,
                      ui::theme::warn, false);
            return;
        }
        FrameFormat f = capture_ ? capture_->Format() : FrameFormat{};
        SetStatus(Fmt(L"Vídeo %u×%u a %s fps (%s). %s", activeFormat_.width, activeFormat_.height,
                      FpsText(activeFormat_.Fps()).c_str(), SubtypeName(f.sourceSubtype), audioPart.c_str()),
                  audioActive_ ? ui::theme::ok : ui::theme::warn, false);
    } else {
        SetStatus(audioPart, audioActive_ ? ui::theme::ok : ui::theme::warn, false);
    }
}

std::wstring App::VideoErrorText(HRESULT hr) const {
    if (hr == E_APP_NO_DEVICE) return L"Conecta y selecciona una capturadora.";
    if (hr == E_APP_GPU) return L"No se pudo iniciar Direct3D 11. Actualiza el controlador de la tarjeta gráfica.";
    if (hr == E_ACCESSDENIED)
        return L"Windows bloquea el acceso al vídeo. Activa Configuración › Privacidad y seguridad › Cámara › "
               L"«Permitir que las aplicaciones de escritorio accedan a la cámara».";
    if (hr == MF_E_HW_MFT_FAILED_START_STREAMING || hr == MF_E_VIDEO_RECORDING_DEVICE_PREEMPTED ||
        hr == HRESULT_FROM_WIN32(ERROR_SHARING_VIOLATION) || hr == HRESULT_FROM_WIN32(ERROR_BUSY))
        return L"La capturadora está en uso por otra aplicación (OBS, Discord, el programa del fabricante…). "
               L"Ciérrala e inténtalo de nuevo.";
    if (hr == MF_E_INVALIDMEDIATYPE) return L"La capturadora no ofrece ningún modo de vídeo que se pueda mostrar.";
    if (hr == MF_E_VIDEO_RECORDING_DEVICE_INVALIDATED)
        return L"La capturadora se ha desconectado.";
    return Fmt(L"No se pudo abrir el vídeo (código %s).", HrHex(hr).c_str());
}

std::wstring App::AudioErrorText(HRESULT hr) const {
    const bool input = audio_.FailedStage() == 0;
    if (hr == E_ACCESSDENIED && input)
        return L"Windows bloquea la entrada de audio. Activa Configuración › Privacidad y seguridad › Micrófono › "
               L"«Permitir que las aplicaciones de escritorio accedan al micrófono».";
    if (hr == AUDCLNT_E_DEVICE_IN_USE)
        return L"Otra aplicación tiene el dispositivo de audio en modo exclusivo. Ciérrala e inténtalo de nuevo.";
    if (hr == E_NOTFOUND || hr == HRESULT_FROM_WIN32(ERROR_NOT_FOUND) || hr == AUDCLNT_E_DEVICE_INVALIDATED)
        return input ? L"La entrada de audio de la capturadora no está disponible." : L"La salida de audio no está disponible.";
    return Fmt(input ? L"No se pudo abrir la entrada de audio (código %s)." : L"No se pudo abrir la salida de audio (código %s).",
               HrHex(hr).c_str());
}

std::wstring App::BuildVideoDiagnostics(const CaptureStats& cs, const RenderStats& rs) const {
    FrameFormat f = capture_ ? capture_->Format() : FrameFormat{};
    std::wstring s = Fmt(L"Captura: %u×%u @ %.2f fps · %s", f.width, f.height, f.fps, SubtypeName(f.subtype));
    if (f.decoded) s += Fmt(L" (origen %s, decodificado en CPU)", SubtypeName(f.sourceSubtype));
    s += Fmt(L"\nRecibidos: %.1f fps · Mostrados: %.1f fps", fpsIn_, fpsOut_);
    s += Fmt(L"\nDescartados por viejos: %llu · Huecos del driver: %llu", (unsigned long long)rs.replaced,
             (unsigned long long)cs.gapFrames);
    s += Fmt(L"\nCopia CPU→GPU: %.2f ms · Llegada→Present: %.2f ms (máx %.2f) [medido]", rs.uploadMsAvg,
             rs.pipelineMsAvg, rs.pipelineMsMax);
    const wchar_t* present = s_.vsync ? L"con VSync" : (rs.tearingSupported ? L"inmediata (tearing permitido)" : L"inmediata");
    s += Fmt(L"\nPresentación: %s · GPU: %s", present, rs.adapter.c_str());
    return s;
}

std::wstring App::BuildAudioDiagnostics() {
    if (!audioActive_) return L"Audio: parado";
    AudioStats a = audio_.Stats();
    std::wstring s = Fmt(L"Audio %s: %u Hz · %u→%u canales\nEntrada %s %.2f ms · Salida %s %.2f ms (búfer %.2f)%s",
                         L"compartido", a.sampleRate, a.inChannels, a.outChannels,
                         a.lowLatencyCapture ? L"baja latencia" : L"estándar", a.capturePeriodMs,
                         a.lowLatencyRender ? L"baja latencia" : L"estándar", a.renderPeriodMs, a.renderBufferMs,
                         a.systemResampler ? L" · Windows convierte la frecuencia" : L"");
    s += Fmt(L"\nColchón: %.1f ms (objetivo %.1f) · Cola salida: %.1f ms · Deriva: %+.0f ppm", a.ringFillMs,
             a.targetFillMs, a.renderQueuedMs, a.driftPpm);
    s += Fmt(L"\nVacíos: %llu · Recortes: %llu · Desbordes: %llu · Discont.: %llu",
             (unsigned long long)a.underruns, (unsigned long long)a.trims, (unsigned long long)a.overflows,
             (unsigned long long)a.discontinuities);
    double estimate = a.capturePacketMaxMs + a.ringFillMs + a.renderQueuedMs + a.captureStreamLatencyMs +
                      a.renderStreamLatencyMs;
    s += Fmt(L"\nLatencia de audio estimada (no medida): ≈ %.0f ms · Nivel: %d %%", estimate, int(a.peak * 100));
    return s;
}

// ============================================================================
// Bandeja
// ============================================================================

void App::TrayAdd() {
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    nid.uCallbackMessage = WM_APP_TRAY;
    nid.hIcon = iconSmall_;
    std::wstring tip = L"Pasarela — " + statusText_;
    wcsncpy_s(nid.szTip, tip.c_str(), _TRUNCATE);
    if (!trayActive_) {
        trayActive_ = Shell_NotifyIconW(NIM_ADD, &nid) != FALSE;
    } else {
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
}

void App::TrayRemove() {
    if (!trayActive_) return;
    NOTIFYICONDATAW nid{sizeof(nid)};
    nid.hWnd = hwnd_;
    nid.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &nid);
    trayActive_ = false;
}

void App::HideToBackground(bool notify) {
    if (settings_) ShowWindow(settings_, SW_HIDE);
    ShowWindow(hwnd_, SW_HIDE);
    TrayAdd();
    if (notify && !balloonShown_ && trayActive_) {
        balloonShown_ = true;
        NOTIFYICONDATAW nid{sizeof(nid)};
        nid.hWnd = hwnd_;
        nid.uID = 1;
        nid.uFlags = NIF_INFO;
        nid.dwInfoFlags = NIIF_USER | NIIF_NOSOUND;
        wcscpy_s(nid.szInfoTitle, L"Pasarela sigue funcionando");
        wcscpy_s(nid.szInfo, L"Está en la bandeja del sistema. Haz clic en su icono para volver.");
        Shell_NotifyIconW(NIM_MODIFY, &nid);
    }
    s_.Save();
}

void App::RestoreFromTray() {
    autoHidden_ = false;
    ShowWindow(hwnd_, SW_SHOW);
    ShowWindow(hwnd_, SW_RESTORE);
    SetForegroundWindow(hwnd_);
    TrayRemove();
}

// ============================================================================
// Mensajes de la ventana principal
// ============================================================================

void App::OnCommand(int id, int code) {
    switch (id) {
        case ID_BTN_AUDIO: case IDM_AUDIO: GoAudio(); return;
        case ID_BTN_VIDEO: case IDM_VIDEO: GoVideo(); return;
        case ID_BTN_STOP: case IDM_STOP: StopAll(); return;
        case IDM_HIDE_VIDEO: HideVideo(); return;
        case IDM_SHOW: RestoreFromTray(); return;
        case IDM_EXIT: SendMessageW(hwnd_, WM_CLOSE, 0, 0); return;
        case IDM_FULLSCREEN: ToggleFullscreen(); return;
        case IDM_SCREENSHOT: TakeScreenshot(); return;
        case ID_BTN_SHOTS:
            ShellExecuteW(hwnd_, L"open", ScreenshotFolder().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case ID_BTN_MUTE:
        case IDM_MUTE:
            s_.muted = !s_.muted;
            ApplyVolume();
            Redraw();
            return;
        case IDM_TOPMOST: SetTopmost(!s_.topmost); return;
        case ID_TG_TOPMOST: SetTopmost(!Toggle(tgTopmost_)); return;
        case IDM_DIAG:
        case ID_TG_DIAG:
            s_.diagnostics = !s_.diagnostics;
            SetToggle(tgDiag_, s_.diagnostics);
            ApplyRenderOptions();
            Layout();
            return;
        case ID_BTN_REFRESH:
            RefreshVideoDevices();
            return;
        case ID_BTN_ADV:
            ShowSettings();
            return;
        case IDCANCEL:
            if (settings_ && IsWindowVisible(settings_)) ShowWindow(settings_, SW_HIDE);
            return;
        case ID_TG_HIDE_VIDEO: s_.hideOnVideo = !s_.hideOnVideo; SetToggle(tgHideVideo_, s_.hideOnVideo); return;
        case ID_BTN_FOLDER:
            ShellExecuteW(hwnd_, L"open", DataDirectory().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            return;
        case ID_TG_AUTO_AUDIO: s_.autoAudio = !s_.autoAudio; SetToggle(tgAutoAudio_, s_.autoAudio); return;
        case ID_TG_KEEP_DISPLAY:
            s_.keepDisplayOnAudio = !s_.keepDisplayOnAudio;
            SetToggle(tgKeepDisplay_, s_.keepDisplayOnAudio);
            UpdateExecutionState();  // se aplica al momento si «Solo audio» ya está en marcha
            return;
        case ID_TG_TRAY: s_.tray = !s_.tray; SetToggle(tgTray_, s_.tray); return;
        case ID_TG_VSYNC: s_.vsync = !s_.vsync; SetToggle(tgVsync_, s_.vsync); ApplyRenderOptions(); return;
        case ID_TG_FULLRANGE:
            s_.fullRange = !s_.fullRange;
            SetToggle(tgFullRange_, s_.fullRange);
            ApplyRenderOptions();
            return;
    }
    if (code != CBN_SELCHANGE || populating_) return;
    switch (id) {
        case ID_CB_VIDEO: {
            ClearSticky();
            OnVideoDeviceSelected(true);
            if (session_ == kVideo) {
                StopVideo();
                SetSession(kOff);
                GoVideo();
            } else if (session_ == kAudio) {
                RestartAudio();
            }
            return;
        }
        case ID_CB_QUALITY: {
            int i = (int)SendMessageW(cbQuality_, CB_GETCURSEL, 0, 0);
            LPARAM data = SendMessageW(cbQuality_, CB_GETITEMDATA, i, 0);
            s_.width = i <= 0 ? 0 : UINT32((data >> 16) & 0xFFFF);
            s_.height = i <= 0 ? 0 : UINT32(data & 0xFFFF);
            s_.fps = 0;
            PopulateFps();
            if (videoActive_) RestartCapture(false);
            return;
        }
        case ID_CB_FPS: {
            int i = (int)SendMessageW(cbFps_, CB_GETCURSEL, 0, 0);
            s_.fps = double(SendMessageW(cbFps_, CB_GETITEMDATA, i, 0)) / 1000.0;
            PopulateFormat();
            if (videoActive_) RestartCapture(false);
            return;
        }
        case ID_CB_FORMAT: {
            int i = (int)SendMessageW(cbFormat_, CB_GETCURSEL, 0, 0);
            wchar_t buf[64] = {};
            if (i > 0) SendMessageW(cbFormat_, CB_GETLBTEXT, i, (LPARAM)buf);
            s_.format = i > 0 ? buf : L"";
            if (videoActive_) RestartCapture(false);
            return;
        }
        case ID_CB_AIN: {
            int i = (int)SendMessageW(cbIn_, CB_GETCURSEL, 0, 0);
            s_.audioIn = i > 0 && i <= (int)inDevs_.size() ? inDevs_[i - 1].id : L"";
            RestartAudio();
            return;
        }
        case ID_CB_AOUT: {
            int i = (int)SendMessageW(cbOut_, CB_GETCURSEL, 0, 0);
            s_.audioOut = i > 0 && i <= (int)outDevs_.size() ? outDevs_[i - 1].id : L"";
            RestartAudio();
            return;
        }
        case ID_CB_BUFFER:
            s_.bufferProfile = Clamp((int)SendMessageW(cbBuffer_, CB_GETCURSEL, 0, 0), 0, 2);
            RestartAudio();
            return;
    }
}

LRESULT App::MainProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == taskbarCreatedMsg_ && taskbarCreatedMsg_) {
        if (trayActive_) {
            trayActive_ = false;
            TrayAdd();
        }
        return 0;
    }
    switch (msg) {
        case WM_CREATE:
            hwnd_ = h;
            scale_ = GetDpiForWindow(h) / 96.0f;
            ui::ApplyDarkTitleBar(h);
            settings_ = CreateWindowExW(0, kSettingsClass, L"Pasarela — Ajustes",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN, CW_USEDEFAULT, 0,
                                        700, 600, h, nullptr, inst_, nullptr);
            ui::ApplyDarkTitleBar(settings_);
            CreateControls();
            RefreshVideoDevices();  // también refresca el audio
            EnableWindow(btnStop_, FALSE);
            statusText_ = L"Listo.";
            Layout();
            LayoutSettings();
            watcher_ = StartAudioDeviceWatcher(h);
            SetTimer(h, TIMER_TICK, 500, nullptr);
            return 0;
        case WM_ERASEBKGND:
            return 1;
        case WM_PAINT: {
            PAINTSTRUCT ps;
            HDC dc = BeginPaint(h, &ps);
            Paint(dc);
            EndPaint(h, &ps);
            return 0;
        }
        case WM_CTLCOLORLISTBOX: {
            HDC dc = (HDC)wp;
            SetBkColor(dc, RGB(0x23, 0x25, 0x2B));
            SetTextColor(dc, RGB(0xE9, 0xEA, 0xEE));
            return (LRESULT)surfaceBrush_;
        }
        case WM_CTLCOLORSTATIC:
        case WM_CTLCOLORBTN:
            return (LRESULT)bgBrush_;
        case WM_MEASUREITEM: {
            auto* m = reinterpret_cast<MEASUREITEMSTRUCT*>(lp);
            if (m->CtlType == ODT_COMBOBOX) m->itemHeight = (m->itemID == UINT(-1)) ? P(26) : P(30);
            return TRUE;
        }
        case WM_DRAWITEM:
            DrawItem(reinterpret_cast<DRAWITEMSTRUCT*>(lp));
            return TRUE;
        case WM_COMMAND:
            OnCommand(LOWORD(wp), HIWORD(wp));
            return 0;
        case WM_HSCROLL:
            if (reinterpret_cast<HWND>(lp) == slVolume_) {
                s_.volume = ui::SliderGetValue(slVolume_);
                if (s_.muted && s_.volume > 0) s_.muted = false;
                ApplyVolume();
                InvalidateRect(h, &rcVolumeValue_, FALSE);
                InvalidateRect(btnMute_, nullptr, FALSE);
            } else if (reinterpret_cast<HWND>(lp) == slDelay_) {
                s_.audioDelayMs = ui::SliderGetValue(slDelay_);
                audio_.SetExtraDelayMs(s_.audioDelayMs);
                InvalidateRect(settings_, &rcLblDelay_, FALSE);
            }
            return 0;
        case WM_TIMER:
            switch (wp) {
                case TIMER_TICK: Tick(); break;
                case TIMER_VIDEO_DEVICES:
                    KillTimer(h, wp);
                    RefreshVideoDevices();
                    break;
                case TIMER_AUDIO_DEVICES:
                    KillTimer(h, wp);
                    RefreshAudioDevices();
                    break;
                case TIMER_AUDIO_DEFAULT:
                    KillTimer(h, wp);
                    if (audioActive_ && audio_.Settings().outputId.empty()) RestartAudio();
                    break;
            }
            return 0;
        case WM_DEVICECHANGE:
            if (wp == DBT_DEVNODES_CHANGED) SetTimer(h, TIMER_VIDEO_DEVICES, 800, nullptr);
            return TRUE;
        case WM_APP_AUDIO_DEVICES_CHANGED:
            SetTimer(h, TIMER_AUDIO_DEVICES, 500, nullptr);
            return 0;
        case WM_APP_AUDIO_DEFAULT_CHANGED:
            SetTimer(h, TIMER_AUDIO_DEFAULT, 700, nullptr);
            return 0;
        case WM_APP_FORMATS_READY:
            OnFormatsReady(reinterpret_cast<FormatsResult*>(lp));
            return 0;
        case WM_APP_VIDEO_ERROR: {
            HRESULT hr = HRESULT(wp);
            Log(L"Error de vídeo: %s", HrHex(hr).c_str());
            if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) rendererLost_ = true;
            else videoErr_ = hr;
            return 0;
        }
        case WM_APP_VIDEO_WINDOW_CLOSED:
            HideVideo();
            return 0;
        case WM_APP_TRAY:
            switch (LOWORD(lp)) {
                case WM_LBUTTONUP:
                case WM_LBUTTONDBLCLK: RestoreFromTray(); break;
                case WM_RBUTTONUP:
                case WM_CONTEXTMENU: {
                    POINT pt;
                    GetCursorPos(&pt);
                    ShowMenu(h, pt, true);
                    break;
                }
            }
            return 0;
        case WM_SYSCOMMAND:
            if ((wp & 0xFFF0) == SC_MINIMIZE && s_.tray) {
                HideToBackground(true);
                return 0;
            }
            break;
        case WM_KEYDOWN:
            if (wp == 'M' && GetFocus() == h) OnCommand(IDM_MUTE, 0);
            break;
        case WM_DPICHANGED: {
            scale_ = HIWORD(wp) / 96.0f;
            auto* r = reinterpret_cast<RECT*>(lp);
            SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            for (HWND c : {cbVideo_, cbOut_, cbQuality_, cbFps_, cbFormat_, cbIn_, cbBuffer_}) {
                SendMessageW(c, CB_SETITEMHEIGHT, (WPARAM)-1, P(26));
                SendMessageW(c, CB_SETITEMHEIGHT, 0, P(30));
            }
            Layout();
            LayoutSettings();
            return 0;
        }
        case WM_CLOSE: {
            WINDOWPLACEMENT wp2{sizeof(wp2)};
            GetWindowPlacement(h, &wp2);
            s_.mainX = wp2.rcNormalPosition.left;
            s_.mainY = wp2.rcNormalPosition.top;
            StopVideo();
            StopAudio();
            s_.Save();
            DestroyWindow(h);
            return 0;
        }
        case WM_DESTROY:
            KillTimer(h, TIMER_TICK);
            StopAudioDeviceWatcher(watcher_);
            watcher_ = nullptr;
            TrayRemove();
            SetThreadExecutionState(ES_CONTINUOUS);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

int RunApp(HINSTANCE instance, int show) {
    App app;
    return app.Run(instance, show);
}
