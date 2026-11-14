// Copyright (c) 2026 Dreex. Licencia MIT.
#pragma once
#include "common.h"

// Registro local en texto plano (Pasarela.log). Nunca se envía a ningún sitio.
void LogInit();
void Log(const wchar_t* fmt, ...);
std::wstring LogPath();
