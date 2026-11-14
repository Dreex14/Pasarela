// Copyright (c) 2026 Dreex. Licencia MIT.
#include "common.h"

#include <shlobj.h>

#include <cstdarg>
#include <cstdio>

std::wstring Fmt(const wchar_t* fmt, ...) {
    wchar_t buf[2048];
    va_list ap;
    va_start(ap, fmt);
    _vsnwprintf_s(buf, _countof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    return buf;
}

std::wstring HrHex(HRESULT hr) { return Fmt(L"0x%08X", static_cast<unsigned>(hr)); }

std::string ToUtf8(const std::wstring& s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string out(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring out(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), out.data(), n);
    return out;
}

std::wstring ExeDirectory() {
    wchar_t path[MAX_PATH * 4];
    DWORD n = GetModuleFileNameW(nullptr, path, _countof(path));
    std::wstring p(path, n);
    size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? L"." : p.substr(0, slash);
}

static bool CanWriteTo(const std::wstring& dir) {
    std::wstring probe = dir + L"\\.pasarela_write_test";
    HANDLE h = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    CloseHandle(h);
    return true;
}

const std::wstring& DataDirectory() {
    static const std::wstring dir = [] {
        std::wstring exe = ExeDirectory();
        // Portátil: si ya existe el .ini junto al exe, o se puede escribir allí.
        if (GetFileAttributesW((exe + L"\\Pasarela.ini").c_str()) != INVALID_FILE_ATTRIBUTES ||
            CanWriteTo(exe)) {
            return exe;
        }
        wchar_t* appData = nullptr;
        std::wstring d = exe;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData))) {
            d = std::wstring(appData) + L"\\Pasarela";
            CoTaskMemFree(appData);
            CreateDirectoryW(d.c_str(), nullptr);
        }
        return d;
    }();
    return dir;
}
