#include "application_paths.h"

#include <windows.h>
#include <shlobj_core.h>

#include <system_error>
#include <string>
#include <vector>

namespace {

std::filesystem::path RootFromKnownFolder() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData) {
        if (localAppData) CoTaskMemFree(localAppData);
        return {};
    }
    const std::filesystem::path root = std::filesystem::path(localAppData) / L"viewtrious";
    CoTaskMemFree(localAppData);
    return root;
}

std::filesystem::path ModuleDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

bool MoveOrCopyDatabase(const std::filesystem::path& source, const std::filesystem::path& destination) {
    std::error_code error;
    std::filesystem::rename(source, destination, error);
    if (!error) return true;
    error.clear();
    std::filesystem::copy_file(source, destination, std::filesystem::copy_options::none, error);
    if (error) return false;
    std::filesystem::remove(source, error);
    return true;
}

} // namespace

namespace ViewtriousPaths {

bool LocalAppDataRoot(std::filesystem::path& path) {
    path = RootFromKnownFolder();
    return !path.empty();
}

std::filesystem::path AppDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"App"; }
std::filesystem::path ShellExtensionsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"ShellExtensions"; }
std::filesystem::path AddonsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"Addons"; }
std::filesystem::path LicensesDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"Licenses"; }
std::filesystem::path DataDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"Data"; }
std::filesystem::path CacheDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"Cache"; }
std::filesystem::path LogsDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"Logs"; }
std::filesystem::path DiagnosticsDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"Diagnostics"; }
std::filesystem::path AiModelsDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"AI" / L"Models"; }

bool SettingsDirectory(std::filesystem::path& path) {
    const std::filesystem::path data = DataDirectory();
    if (data.empty()) return false;
    path = data / L"Settings";
    std::error_code error;
    std::filesystem::create_directories(path, error);
    return !error;
}

bool ResolveSettingsDatabasePath(std::filesystem::path& path) {
    std::filesystem::path settings;
    if (!SettingsDirectory(settings)) return false;
    path = settings / L"Viewtrious.db";
    std::error_code error;
    const bool targetExists = std::filesystem::exists(path, error);
    if (error) return false;
    std::filesystem::path root;
    if (!LocalAppDataRoot(root)) return false;
    if (!targetExists) {
        const std::filesystem::path legacyLocalAppDataDatabase = root / L"Viewtrious.db";
        if (std::filesystem::exists(legacyLocalAppDataDatabase, error) && !error) {
            if (!MoveOrCopyDatabase(legacyLocalAppDataDatabase, path)) return false;
        } else if (error) {
            return false;
        } else {
            const std::filesystem::path moduleDirectory = ModuleDirectory();
            const std::filesystem::path legacyModuleDatabase = moduleDirectory.empty() ? std::filesystem::path{} : moduleDirectory / L"Viewtrious.db";
            if (!legacyModuleDatabase.empty() && std::filesystem::exists(legacyModuleDatabase, error) && !error) {
                std::filesystem::copy_file(legacyModuleDatabase, path, std::filesystem::copy_options::none, error);
                if (error) return false;
            } else if (error) {
                return false;
            }
        }
    }

    std::filesystem::remove(root / L"file-open.log", error);
    error.clear();
    std::filesystem::remove(root / L"file-open.previous.log", error);
    return true;
}

} // namespace ViewtriousPaths
