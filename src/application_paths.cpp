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

std::filesystem::path AppDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"app"; }
std::filesystem::path ShellExtensionsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"shellextensions"; }
std::filesystem::path AddonsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"addons"; }
std::filesystem::path LicensesDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"licenses"; }
std::filesystem::path DataDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"data"; }
std::filesystem::path WallpaperDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"wallpaper"; }
std::filesystem::path LegacyWallpaperDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"cache" / L"wallpaper"; }
std::filesystem::path AiModelsDirectory() { const std::filesystem::path root = RootFromKnownFolder(); return root.empty() ? std::filesystem::path{} : root / L"ai" / L"models"; }

bool ResolveDatabasePath(std::filesystem::path& path) {
    const std::filesystem::path data = DataDirectory();
    if (data.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (error) return false;
    path = data / L"viewtrious.db";
    const bool targetExists = std::filesystem::exists(path, error);
    if (error) return false;
    std::filesystem::path root;
    if (!LocalAppDataRoot(root)) return false;
    if (!targetExists) {
        const std::filesystem::path nestedLegacyDatabase = data / L"settings" / L"viewtrious.db";
        const std::filesystem::path legacyLocalAppDataDatabase = root / L"viewtrious.db";
        const std::filesystem::path moduleDirectory = ModuleDirectory();
        const std::filesystem::path legacyModuleDatabase = moduleDirectory.empty() ? std::filesystem::path{} : moduleDirectory / L"viewtrious.db";
        const std::filesystem::path* legacyDatabase = nullptr;
        if (std::filesystem::exists(nestedLegacyDatabase, error) && !error) legacyDatabase = &nestedLegacyDatabase;
        else if (error) return false;
        else if (std::filesystem::exists(legacyLocalAppDataDatabase, error) && !error) legacyDatabase = &legacyLocalAppDataDatabase;
        else if (error) return false;
        else if (!legacyModuleDatabase.empty() && std::filesystem::exists(legacyModuleDatabase, error) && !error) legacyDatabase = &legacyModuleDatabase;
        else if (error) return false;
        if (legacyDatabase) {
            if (legacyDatabase == &legacyModuleDatabase) {
                std::filesystem::copy_file(*legacyDatabase, path, std::filesystem::copy_options::none, error);
                if (error) return false;
            } else if (!MoveOrCopyDatabase(*legacyDatabase, path)) {
                return false;
            }
        }
        if (legacyDatabase == &nestedLegacyDatabase) {
            error.clear();
            std::filesystem::remove(nestedLegacyDatabase.parent_path(), error);
        }
    }

    std::filesystem::remove(root / L"file-open.log", error);
    error.clear();
    std::filesystem::remove(root / L"file-open.previous.log", error);
    return true;
}

} // namespace ViewtriousPaths
