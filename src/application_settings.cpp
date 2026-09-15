#include "application_settings.h"

#include "application_paths.h"

#include <windows.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct sqlite3;
struct sqlite3_stmt;
constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteOpenFullMutex = 0x00010000;
using SqliteDestructor = void(__cdecl*)(void*);
SqliteDestructor const kSqliteTransient = reinterpret_cast<SqliteDestructor>(static_cast<intptr_t>(-1));
using SqliteOpenV2 = int(__cdecl*)(const char*, sqlite3**, int, const char*);
using SqliteClose = int(__cdecl*)(sqlite3*);
using SqliteExec = int(__cdecl*)(sqlite3*, const char*, int(__cdecl*)(void*, int, char**, char**), void*, char**);
using SqlitePrepareV2 = int(__cdecl*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using SqliteFinalize = int(__cdecl*)(sqlite3_stmt*);
using SqliteStep = int(__cdecl*)(sqlite3_stmt*);
using SqliteBindText = int(__cdecl*)(sqlite3_stmt*, int, const char*, int, SqliteDestructor);
using SqliteBindInt64 = int(__cdecl*)(sqlite3_stmt*, int, int64_t);
using SqliteColumnInt64 = int64_t(__cdecl*)(sqlite3_stmt*, int);
using SqliteBusyTimeout = int(__cdecl*)(sqlite3*, int);

constexpr wchar_t kLegacySettingsKey[] = L"Software\\Viewtrious";
constexpr std::array<const wchar_t*, 47> kLegacyPreferenceNames{
    L"RememberWindowPlacement", L"WindowLeft", L"WindowTop", L"WindowWidth", L"WindowHeight", L"WindowMaximized",
    L"IncludeHiddenImages", L"ConfirmBeforeDeleting", L"SwipeToNavigateWhenFit", L"ShowZoomPercentage", L"ZoomHudPosition",
    L"AnimationsAndFadeEffects", L"AlwaysShowFilmstrip", L"ReverseMouseWheelZoom", L"EnableSpaceMouse", L"Theme",
    L"ModelProjectionMode", L"ModelVisualStyle", L"ModelUpAxis", L"ModelBuildPlate", L"ModelBuildPlateWidthMm",
    L"ModelBuildPlateDepthMm", L"ModelBuildPlateSizeLinked", L"AxisIndicatorPosition", L"ModelRenderingApi",
    L"GraphicsAdapterAuto", L"GraphicsAdapterLuidLow", L"GraphicsAdapterLuidHigh", L"ModelAntiAliasing", L"ImageScaling",
    L"VideoWindowSizing", L"VideoMuted", L"VideoAutoPlayNext", L"VideoVolumeMilli", L"ImageExternalOpenBehavior",
    L"VideoExternalOpenBehavior", L"OnboardingVersion", L"TourPending", L"UserAdjustmentPresetSaved",
    L"UserAdjustmentPresetExposure", L"UserAdjustmentPresetBrightness", L"UserAdjustmentPresetContrast", L"UserAdjustmentPresetShadows",
    L"UserAdjustmentPresetHighlights", L"UserAdjustmentPresetSaturation", L"UserAdjustmentPresetSharpness" };

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

class Store {
public:
    ~Store() { Close(); }

    bool Initialize() {
        std::lock_guard lock(mutex_);
        return Open() && MigrateLegacyRegistry();
    }

    bool Read(const wchar_t* name, DWORD& value) {
        std::lock_guard lock(mutex_);
        if (!Open()) return ReadLegacyDword(name, value);
        if (!MigrateLegacyRegistry()) return ReadUnlocked(name, value) || ReadLegacyDword(name, value);
        return ReadUnlocked(name, value);
    }

    bool Write(const wchar_t* name, DWORD value) {
        std::lock_guard lock(mutex_);
        if (!Open() || !MigrateLegacyRegistry()) return false;
        return WriteUnlocked(name, value, false);
    }

    bool Delete(const wchar_t* name) {
        std::lock_guard lock(mutex_);
        if (!Open()) return false;
        sqlite3_stmt* statement = nullptr;
        if (!Prepare("DELETE FROM app_settings WHERE key=?1;", &statement)) return false;
        const std::string key = Utf8(name);
        bindText_(statement, 1, key.c_str(), static_cast<int>(key.size()), kSqliteTransient);
        const bool success = step_(statement) == kSqliteDone;
        finalize_(statement);
        return success;
    }

    bool Clear() {
        std::lock_guard lock(mutex_);
        if (!Open() || !MigrateLegacyRegistry()) return false;
        const bool cleared = Execute("DELETE FROM app_settings;");
        if (cleared) migrationComplete_ = false;
        return cleared;
    }

private:
    template<typename T> bool Resolve(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(module_, name));
        return target != nullptr;
    }

    bool Open() {
        if (database_) return true;
        std::filesystem::path path;
        if (!ViewtriousPaths::ResolveSettingsDatabasePath(path)) return false;
        module_ = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_ || !Resolve(openV2_, "sqlite3_open_v2") || !Resolve(close_, "sqlite3_close") || !Resolve(exec_, "sqlite3_exec") ||
            !Resolve(prepareV2_, "sqlite3_prepare_v2") || !Resolve(finalize_, "sqlite3_finalize") || !Resolve(step_, "sqlite3_step") ||
            !Resolve(bindText_, "sqlite3_bind_text") || !Resolve(bindInt64_, "sqlite3_bind_int64") || !Resolve(columnInt64_, "sqlite3_column_int64") ||
            !Resolve(busyTimeout_, "sqlite3_busy_timeout")) { Close(); return false; }
        const std::string utf8Path = Utf8(path.wstring());
        if (openV2_(utf8Path.c_str(), &database_, kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex, nullptr) != kSqliteOk || !database_) { Close(); return false; }
        busyTimeout_(database_, 1500);
        if (!Execute("PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS app_settings (key TEXT PRIMARY KEY, value INTEGER NOT NULL);")) { Close(); return false; }
        return true;
    }

    void Close() {
        if (database_ && close_) close_(database_);
        database_ = nullptr;
        if (module_) FreeLibrary(module_);
        module_ = nullptr;
    }

    bool Execute(const char* sql) { return exec_(database_, sql, nullptr, nullptr, nullptr) == kSqliteOk; }
    bool Prepare(const char* sql, sqlite3_stmt** statement) { return prepareV2_(database_, sql, -1, statement, nullptr) == kSqliteOk; }

    bool WriteUnlocked(const wchar_t* name, DWORD value, bool preserveExisting) {
        sqlite3_stmt* statement = nullptr;
        const char* sql = preserveExisting ? "INSERT OR IGNORE INTO app_settings(key,value) VALUES(?1,?2);" : "INSERT INTO app_settings(key,value) VALUES(?1,?2) ON CONFLICT(key) DO UPDATE SET value=excluded.value;";
        if (!Prepare(sql, &statement)) return false;
        const std::string key = Utf8(name);
        bindText_(statement, 1, key.c_str(), static_cast<int>(key.size()), kSqliteTransient);
        bindInt64_(statement, 2, value);
        const bool success = step_(statement) == kSqliteDone;
        finalize_(statement);
        return success;
    }

    bool HasMigrationMarker() {
        DWORD marker = 0;
        return ReadUnlocked(L"__legacy_registry_migration_v1", marker) && marker == 1;
    }

    bool ReadUnlocked(const wchar_t* name, DWORD& value) {
        sqlite3_stmt* statement = nullptr;
        if (!Prepare("SELECT value FROM app_settings WHERE key=?1;", &statement)) return false;
        const std::string key = Utf8(name);
        bindText_(statement, 1, key.c_str(), static_cast<int>(key.size()), kSqliteTransient);
        const bool found = step_(statement) == kSqliteRow;
        if (found) value = static_cast<DWORD>(columnInt64_(statement, 0));
        finalize_(statement);
        return found;
    }

    bool ReadLegacyDword(const wchar_t* name, DWORD& value) const {
        DWORD size = sizeof(value);
        return RegGetValueW(HKEY_CURRENT_USER, kLegacySettingsKey, name, RRF_RT_REG_DWORD, nullptr, &value, &size) == ERROR_SUCCESS;
    }

    bool MigrateLegacyRegistry() {
        if (migrationComplete_ || HasMigrationMarker()) { migrationComplete_ = true; return true; }
        struct LegacyValue { const wchar_t* name; DWORD value; };
        std::vector<LegacyValue> legacy;
        for (const wchar_t* name : kLegacyPreferenceNames) {
            DWORD value = 0;
            if (ReadLegacyDword(name, value))
                legacy.push_back({ name, value });
        }
        if (!Execute("BEGIN IMMEDIATE;")) return false;
        bool success = true;
        for (const LegacyValue& setting : legacy) success &= WriteUnlocked(setting.name, setting.value, true);
        success &= Execute(success ? "COMMIT;" : "ROLLBACK;");
        if (!success) return false;

        HKEY key = nullptr;
        const LONG open = RegOpenKeyExW(HKEY_CURRENT_USER, kLegacySettingsKey, 0, KEY_SET_VALUE, &key);
        bool deleted = open == ERROR_FILE_NOT_FOUND || open == ERROR_PATH_NOT_FOUND;
        if (open == ERROR_SUCCESS) {
            deleted = true;
            for (const LegacyValue& setting : legacy) {
                const LONG result = RegDeleteValueW(key, setting.name);
                deleted &= result == ERROR_SUCCESS || result == ERROR_FILE_NOT_FOUND;
            }
            RegCloseKey(key);
        }
        migrationComplete_ = deleted && WriteUnlocked(L"__legacy_registry_migration_v1", 1, false);
        return migrationComplete_;
    }

    std::mutex mutex_;
    HMODULE module_ = nullptr;
    sqlite3* database_ = nullptr;
    SqliteOpenV2 openV2_ = nullptr; SqliteClose close_ = nullptr; SqliteExec exec_ = nullptr; SqlitePrepareV2 prepareV2_ = nullptr;
    SqliteFinalize finalize_ = nullptr; SqliteStep step_ = nullptr; SqliteBindText bindText_ = nullptr; SqliteBindInt64 bindInt64_ = nullptr;
    SqliteColumnInt64 columnInt64_ = nullptr; SqliteBusyTimeout busyTimeout_ = nullptr;
    bool migrationComplete_ = false;
};

Store& SettingsStore() { static Store store; return store; }

} // namespace

namespace ApplicationSettings {

bool Initialize() { return SettingsStore().Initialize(); }
bool ReadDword(const wchar_t* name, DWORD& value) { return SettingsStore().Read(name, value); }
bool WriteDword(const wchar_t* name, DWORD value) { return SettingsStore().Write(name, value); }
bool DeleteDword(const wchar_t* name) { return SettingsStore().Delete(name); }
bool Clear() { return SettingsStore().Clear(); }

} // namespace ApplicationSettings
