#pragma once

#include <filesystem>

namespace ViewtriousPaths {

bool LocalAppDataRoot(std::filesystem::path& path);
std::filesystem::path AppDirectory();
std::filesystem::path ShellExtensionsDirectory();
std::filesystem::path AddonsDirectory();
std::filesystem::path LicensesDirectory();
std::filesystem::path DataDirectory();
std::filesystem::path CacheDirectory();
std::filesystem::path LogsDirectory();
std::filesystem::path DiagnosticsDirectory();
std::filesystem::path AiModelsDirectory();

bool SettingsDirectory(std::filesystem::path& path);
bool ResolveSettingsDatabasePath(std::filesystem::path& path);

} // namespace ViewtriousPaths
