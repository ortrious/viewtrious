#pragma once

#include <windows.h>

namespace ApplicationSettings {

bool Initialize();
bool ReadDword(const wchar_t* name, DWORD& value);
bool WriteDword(const wchar_t* name, DWORD value);
bool DeleteDword(const wchar_t* name);
bool Clear();

} // namespace ApplicationSettings
