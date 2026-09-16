#include "application_paths.h"

#include <windows.h>
#include <shlobj_core.h>

#include <cstdint>
#include <cwctype>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace {

constexpr wchar_t kPortableMarkerName[] = L"viewtrious.portable";

struct AppEnvironment {
    std::filesystem::path executableDirectory;
    std::filesystem::path root;
    std::wstring portableInstanceId;
    bool portable = false;
    bool valid = false;
    std::wstring errorMessage;
};

AppEnvironment gEnvironment;
std::once_flag gEnvironmentOnce;

std::filesystem::path InstalledRootFromKnownFolder() {
    PWSTR localAppData = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &localAppData)) || !localAppData) {
        if (localAppData) CoTaskMemFree(localAppData);
        return {};
    }
    const std::filesystem::path root = std::filesystem::path(localAppData) / L"viewtrious";
    CoTaskMemFree(localAppData);
    return root;
}

std::filesystem::path ResolveModuleDirectory() {
    std::vector<wchar_t> buffer(MAX_PATH);
    for (;;) {
        const DWORD length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
        if (!length) return {};
        if (length < buffer.size()) return std::filesystem::path(std::wstring(buffer.data(), length)).parent_path();
        buffer.resize(buffer.size() * 2);
    }
}

bool ProbeDirectoryWriteAccess(const std::filesystem::path& directory) {
    for (unsigned int attempt = 0; attempt < 16; ++attempt) {
        const std::wstring filename = L".viewtrious-write-test-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt) + L".tmp";
        const std::filesystem::path probe = directory / filename;
        HANDLE file = CreateFileW(probe.c_str(), GENERIC_WRITE, FILE_SHARE_DELETE, nullptr, CREATE_NEW,
            FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            const DWORD error = GetLastError();
            if (error == ERROR_FILE_EXISTS || error == ERROR_ALREADY_EXISTS) continue;
            return false;
        }
        const BYTE value = 0;
        DWORD written = 0;
        const DWORD valueSize = static_cast<DWORD>(sizeof(value));
        const bool success = WriteFile(file, &value, valueSize, &written, nullptr) && written == valueSize;
        CloseHandle(file);
        return success;
    }
    return false;
}

std::wstring HashPortableInstanceId(const std::filesystem::path& directory) {
    uint64_t hash = 14695981039346656037ull;
    for (const wchar_t character : directory.wstring()) {
        hash ^= static_cast<uint16_t>(std::towlower(character));
        hash *= 1099511628211ull;
    }
    wchar_t text[17]{};
    swprintf_s(text, L"%016llx", static_cast<unsigned long long>(hash));
    return text;
}

void ResolveEnvironment() {
    gEnvironment.executableDirectory = ResolveModuleDirectory();
    if (gEnvironment.executableDirectory.empty()) {
        gEnvironment.errorMessage = L"viewtrious could not resolve its application folder.";
        return;
    }

    const std::filesystem::path marker = gEnvironment.executableDirectory / kPortableMarkerName;
    const DWORD markerAttributes = GetFileAttributesW(marker.c_str());
    if (markerAttributes == INVALID_FILE_ATTRIBUTES) {
        const DWORD markerError = GetLastError();
        if (markerError != ERROR_FILE_NOT_FOUND && markerError != ERROR_PATH_NOT_FOUND) {
            gEnvironment.errorMessage = L"viewtrious could not inspect its portable-mode marker.";
            return;
        }
    } else {
        gEnvironment.portable = (markerAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
    }

    if (gEnvironment.portable) {
        const std::filesystem::path data = gEnvironment.executableDirectory / L"data";
        const DWORD dataAttributes = GetFileAttributesW(data.c_str());
        std::filesystem::path probeDirectory = gEnvironment.executableDirectory;
        if (dataAttributes != INVALID_FILE_ATTRIBUTES) {
            if ((dataAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                gEnvironment.errorMessage = L"Portable mode needs write access to the Viewtrious folder.\n\nMove Viewtrious to a writable location and try again.";
                return;
            }
            probeDirectory = data;
        } else {
            const DWORD dataError = GetLastError();
            if (dataError != ERROR_FILE_NOT_FOUND && dataError != ERROR_PATH_NOT_FOUND) {
                gEnvironment.errorMessage = L"Portable mode needs write access to the Viewtrious folder.\n\nMove Viewtrious to a writable location and try again.";
                return;
            }
        }
        if (!ProbeDirectoryWriteAccess(probeDirectory)) {
            gEnvironment.errorMessage = L"Portable mode needs write access to the Viewtrious folder.\n\nMove Viewtrious to a writable location and try again.";
            return;
        }
        gEnvironment.root = gEnvironment.executableDirectory;
        gEnvironment.portableInstanceId = HashPortableInstanceId(gEnvironment.executableDirectory);
    } else {
        gEnvironment.root = InstalledRootFromKnownFolder();
    }
    gEnvironment.valid = true;
}

bool EnsureEnvironment(std::wstring* errorMessage = nullptr) {
    std::call_once(gEnvironmentOnce, ResolveEnvironment);
    if (errorMessage) *errorMessage = gEnvironment.errorMessage;
    return gEnvironment.valid;
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

bool Initialize(std::wstring& errorMessage) { return EnsureEnvironment(&errorMessage); }

bool IsPortable() { return EnsureEnvironment() && gEnvironment.portable; }

std::filesystem::path ExecutableDirectory() {
    return EnsureEnvironment() ? gEnvironment.executableDirectory : std::filesystem::path{};
}

std::wstring PortableInstanceId() {
    return EnsureEnvironment() && gEnvironment.portable ? gEnvironment.portableInstanceId : std::wstring{};
}

bool LocalAppDataRoot(std::filesystem::path& path) {
    if (!EnsureEnvironment() || gEnvironment.portable) { path.clear(); return false; }
    path = gEnvironment.root;
    return !path.empty();
}

std::filesystem::path AppDirectory() {
    if (!EnsureEnvironment()) return {};
    if (gEnvironment.portable) return gEnvironment.executableDirectory;
    return gEnvironment.root.empty() ? std::filesystem::path{} : gEnvironment.root / L"app";
}
std::filesystem::path ShellExtensionsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"shellextensions"; }
std::filesystem::path AddonsDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"addons"; }
std::filesystem::path LicensesDirectory() { const std::filesystem::path app = AppDirectory(); return app.empty() ? std::filesystem::path{} : app / L"licenses"; }
std::filesystem::path DataDirectory() { return EnsureEnvironment() && !gEnvironment.root.empty() ? gEnvironment.root / L"data" : std::filesystem::path{}; }
std::filesystem::path WallpaperDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"wallpaper"; }
std::filesystem::path LegacyWallpaperDirectory() { const std::filesystem::path data = DataDirectory(); return data.empty() ? std::filesystem::path{} : data / L"cache" / L"wallpaper"; }
std::filesystem::path AiModelsDirectory() { return EnsureEnvironment() && !gEnvironment.root.empty() ? gEnvironment.root / L"ai" / L"models" : std::filesystem::path{}; }

bool ResolveDatabasePath(std::filesystem::path& path) {
    const std::filesystem::path data = DataDirectory();
    if (data.empty()) return false;
    std::error_code error;
    std::filesystem::create_directories(data, error);
    if (error) return false;
    path = data / L"viewtrious.db";
    const bool targetExists = std::filesystem::exists(path, error);
    if (error) return false;
    if (IsPortable()) return true;
    std::filesystem::path root;
    if (!LocalAppDataRoot(root)) return false;
    if (!targetExists) {
        const std::filesystem::path nestedLegacyDatabase = data / L"settings" / L"viewtrious.db";
        const std::filesystem::path legacyLocalAppDataDatabase = root / L"viewtrious.db";
        const std::filesystem::path moduleDirectory = ExecutableDirectory();
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
