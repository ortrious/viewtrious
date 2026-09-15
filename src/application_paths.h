#pragma once

#include <filesystem>

namespace ViewtriousPaths {

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
