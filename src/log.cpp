// Copyright (c) 2026 Dreex. Licencia MIT.
#include "log.h"

#include <cstdarg>
#include <cstdio>
#include <share.h>

static std::mutex g_logMutex;
static FILE* g_logFile = nullptr;

std::wstring LogPath() { return DataDirectory() + L"\\Pasarela.log"; }

void LogInit() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) return;
    std::wstring path = LogPath();
    // Se reinicia en cada ejecución para que no crezca sin límite.
    g_logFile = _wfsopen(path.c_str(), L"w, ccs=UTF-8", _SH_DENYWR);
}

void Log(const wchar_t* fmt, ...) {
    wchar_t msg[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(msg, _countof(msg), _TRUNCATE, fmt, ap);
    va_end(ap);

    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t line[2200];
    _snwprintf_s(line, _countof(line), _TRUNCATE, L"%02u:%02u:%02u.%03u [%5lu] %s\n", st.wHour,
                 st.wMinute, st.wSecond, st.wMilliseconds, GetCurrentThreadId(), msg);
    OutputDebugStringW(line);
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_logFile) {
        fputws(line, g_logFile);
        fflush(g_logFile);
    }
}
