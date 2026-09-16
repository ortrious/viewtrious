#pragma once

#include <filesystem>
#include <string>

namespace ViewtriousPaths {

bool Initialize(std::wstring& errorMessage);
bool IsPortable();
std::filesystem::path ExecutableDirectory();
std::wstring PortableInstanceId();
bool LocalAppDataRoot(std::filesystem::path& path);
std::filesystem::path AppDirectory();
std::filesystem::path ShellExtensionsDirectory();
std::filesystem::path AddonsDirectory();
std::filesystem::path LicensesDirectory();
std::filesystem::path DataDirectory();
std::filesystem::path WallpaperDirectory();
std::filesystem::path LegacyWallpaperDirectory();
std::filesystem::path AiModelsDirectory();

bool ResolveDatabasePath(std::filesystem::path& path);

} // namespace ViewtriousPaths
