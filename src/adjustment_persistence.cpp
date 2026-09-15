#include "adjustment_persistence.h"
#include "application_paths.h"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <cwctype>
#include <deque>
#include <filesystem>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace {

struct sqlite3;
struct sqlite3_stmt;
constexpr int kSqliteOk = 0;
constexpr int kSqliteRow = 100;
constexpr int kSqliteDone = 101;
constexpr int kSqliteOpenReadWrite = 0x00000002;
constexpr int kSqliteOpenCreate = 0x00000004;
constexpr int kSqliteOpenFullMutex = 0x00010000;
constexpr int64_t kAdjustmentVersion = 1;
using SqliteDestructor = void(__cdecl*)(void*);
SqliteDestructor const kSqliteTransient = reinterpret_cast<SqliteDestructor>(static_cast<intptr_t>(-1));

using SqliteOpenV2 = int(__cdecl*)(const char*, sqlite3**, int, const char*);
using SqliteClose = int(__cdecl*)(sqlite3*);
using SqliteExec = int(__cdecl*)(sqlite3*, const char*, int(__cdecl*)(void*, int, char**, char**), void*, char**);
using SqliteFree = void(__cdecl*)(void*);
using SqlitePrepareV2 = int(__cdecl*)(sqlite3*, const char*, int, sqlite3_stmt**, const char**);
using SqliteFinalize = int(__cdecl*)(sqlite3_stmt*);
using SqliteStep = int(__cdecl*)(sqlite3_stmt*);
using SqliteBindText = int(__cdecl*)(sqlite3_stmt*, int, const char*, int, void(__cdecl*)(void*));
using SqliteBindBlob = int(__cdecl*)(sqlite3_stmt*, int, const void*, int, void(__cdecl*)(void*));
using SqliteBindInt64 = int(__cdecl*)(sqlite3_stmt*, int, int64_t);
using SqliteBindDouble = int(__cdecl*)(sqlite3_stmt*, int, double);
using SqliteColumnInt = int(__cdecl*)(sqlite3_stmt*, int);
using SqliteColumnInt64 = int64_t(__cdecl*)(sqlite3_stmt*, int);
using SqliteColumnDouble = double(__cdecl*)(sqlite3_stmt*, int);
using SqliteColumnBlob = const void*(__cdecl*)(sqlite3_stmt*, int);
using SqliteColumnBytes = int(__cdecl*)(sqlite3_stmt*, int);
using SqliteBusyTimeout = int(__cdecl*)(sqlite3*, int);

void Trace(const wchar_t* text) { OutputDebugStringW(text); OutputDebugStringW(L"\n"); }
void TraceSqliteError(const wchar_t* context, int result) {
    wchar_t message[160]{};
    swprintf_s(message, L"[Viewtrious] SQLITE_ERROR %s rc=%d", context, result);
    Trace(message);
}

std::wstring NormalizedPath(const std::wstring& path) {
    std::vector<wchar_t> buffer(MAX_PATH);
    DWORD length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
    if (!length) return path;
    if (length >= buffer.size()) {
        buffer.resize(static_cast<size_t>(length) + 1);
        length = GetFullPathNameW(path.c_str(), static_cast<DWORD>(buffer.size()), buffer.data(), nullptr);
        if (!length || length >= buffer.size()) return path;
    }
    std::wstring result(buffer.data(), length);
    std::transform(result.begin(), result.end(), result.begin(), [](wchar_t value) { return static_cast<wchar_t>(std::towlower(value)); });
    return result;
}

std::string Utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int length = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<size_t>(length), '\0');
    if (length) WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), length, nullptr, nullptr);
    return result;
}

struct FileMetadata { int64_t size = 0; int64_t mtime = 0; };

bool ReadMetadata(HANDLE file, FileMetadata& metadata) {
    FILE_STANDARD_INFO standard{};
    FILE_BASIC_INFO basic{};
    if (!GetFileInformationByHandleEx(file, FileStandardInfo, &standard, sizeof(standard)) ||
        !GetFileInformationByHandleEx(file, FileBasicInfo, &basic, sizeof(basic))) return false;
    metadata.size = standard.EndOfFile.QuadPart;
    metadata.mtime = basic.LastWriteTime.QuadPart;
    return true;
}

bool HashFile(const std::wstring& path, std::array<unsigned char, 32>& output, FileMetadata& metadata,
    const std::function<bool()>& cancelled) {
    if (cancelled()) return false;
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool metadataBefore = ReadMetadata(file, metadata);
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    DWORD objectLength = 0, returned = 0;
    bool ok = metadataBefore && BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) == 0 &&
        BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&objectLength), sizeof(objectLength), &returned, 0) == 0;
    std::vector<unsigned char> object(objectLength);
    if (ok) ok = BCryptCreateHash(algorithm, &hash, object.data(), objectLength, nullptr, 0, 0) == 0;
    std::array<unsigned char, 128 * 1024> buffer{};
    while (ok) {
        if (cancelled()) { ok = false; break; }
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) { ok = false; break; }
        if (!read) break;
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0) { ok = false; break; }
    }
    if (ok && !cancelled()) ok = BCryptFinishHash(hash, output.data(), static_cast<ULONG>(output.size()), 0) == 0;
    FileMetadata after{};
    ok = ok && ReadMetadata(file, after) && after.size == metadata.size && after.mtime == metadata.mtime;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    return ok;
}

bool IsNeutral(const ImageAdjustments& value) { return value.IsNeutral(); }

} // namespace

struct ImageAdjustmentPersistence::Impl {
    enum class TaskKind { Resolve, Save, Reset };
    struct Task {
        TaskKind kind = TaskKind::Resolve;
        AdjustmentMediaKind mediaKind = AdjustmentMediaKind::Image;
        std::wstring path;
        uint64_t mediaGeneration = 0;
        uint64_t editGeneration = 0;
        uint64_t persistenceGeneration = 0;
        std::array<unsigned char, 32> hash{};
        ImageAdjustments adjustments{};
        std::shared_ptr<std::promise<bool>> resetCompletion;
    };

    HMODULE module = nullptr;
    sqlite3* database = nullptr;
    SqliteOpenV2 openV2 = nullptr; SqliteClose close = nullptr; SqliteExec exec = nullptr; SqliteFree free = nullptr;
    SqlitePrepareV2 prepareV2 = nullptr; SqliteFinalize finalize = nullptr; SqliteStep step = nullptr;
    SqliteBindText bindText = nullptr; SqliteBindBlob bindBlob = nullptr; SqliteBindInt64 bindInt64 = nullptr; SqliteBindDouble bindDouble = nullptr;
    SqliteColumnInt columnInt = nullptr; SqliteColumnInt64 columnInt64 = nullptr; SqliteColumnDouble columnDouble = nullptr; SqliteColumnBlob columnBlob = nullptr; SqliteColumnBytes columnBytes = nullptr;
    SqliteBusyTimeout busyTimeout = nullptr;
    std::function<void(ImageAdjustmentPersistenceResult&&)> completion;
    std::mutex mutex;
    std::mutex operationMutex;
    std::condition_variable wake;
    std::deque<Task> tasks;
    std::thread thread;
    bool stopping = false;
    std::atomic<bool> enabled{ true };
    std::atomic<uint64_t> persistenceGeneration{ 1 };

    template<typename T> bool ResolveProc(T& target, const char* name) {
        target = reinterpret_cast<T>(GetProcAddress(module, name));
        return target != nullptr;
    }

    bool OpenDatabase() {
        std::filesystem::path databasePath;
        if (!ViewtriousPaths::ResolveDatabasePath(databasePath)) return false;
        module = LoadLibraryExW(L"winsqlite3.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) { Trace(L"[Viewtrious] WINSQLITE_RUNTIME_UNAVAILABLE system runtime"); return false; }
        if (!ResolveProc(openV2, "sqlite3_open_v2") || !ResolveProc(close, "sqlite3_close") || !ResolveProc(exec, "sqlite3_exec") || !ResolveProc(free, "sqlite3_free") ||
            !ResolveProc(prepareV2, "sqlite3_prepare_v2") || !ResolveProc(finalize, "sqlite3_finalize") || !ResolveProc(step, "sqlite3_step") ||
            !ResolveProc(bindText, "sqlite3_bind_text") || !ResolveProc(bindBlob, "sqlite3_bind_blob") || !ResolveProc(bindInt64, "sqlite3_bind_int64") || !ResolveProc(bindDouble, "sqlite3_bind_double") ||
            !ResolveProc(columnInt, "sqlite3_column_int") || !ResolveProc(columnInt64, "sqlite3_column_int64") || !ResolveProc(columnDouble, "sqlite3_column_double") ||
            !ResolveProc(columnBlob, "sqlite3_column_blob") || !ResolveProc(columnBytes, "sqlite3_column_bytes") || !ResolveProc(busyTimeout, "sqlite3_busy_timeout")) {
            Trace(L"[Viewtrious] WINSQLITE_RUNTIME_UNAVAILABLE missing export"); CloseDatabase(); return false;
        }
        const std::string path = Utf8(databasePath.wstring());
        const int openResult = openV2(path.c_str(), &database, kSqliteOpenReadWrite | kSqliteOpenCreate | kSqliteOpenFullMutex, nullptr);
        if (openResult != kSqliteOk || !database) {
            TraceSqliteError(L"open", openResult); CloseDatabase(); return false;
        }
        busyTimeout(database, 1500);
        sqlite3_stmt* statement = nullptr;
        int version = 0;
        if (!Prepare("PRAGMA user_version;", &statement) || step(statement) != kSqliteRow) { if (statement) finalize(statement); CloseDatabase(); return false; }
        version = columnInt(statement, 0); finalize(statement);
        if (version > 4) { Trace(L"[Viewtrious] SQLITE_ERROR newer schema"); CloseDatabase(); return false; }
        if (!Execute("PRAGMA journal_mode=DELETE; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS file_hash_cache (path TEXT PRIMARY KEY, file_size INTEGER NOT NULL, file_mtime INTEGER NOT NULL, sha256 BLOB NOT NULL); CREATE TABLE IF NOT EXISTS image_adjustments (sha256 BLOB PRIMARY KEY, adjustment_version INTEGER NOT NULL, exposure REAL NOT NULL, brightness REAL NOT NULL, contrast REAL NOT NULL, shadows REAL NOT NULL, highlights REAL NOT NULL, saturation REAL NOT NULL, sharpness REAL NOT NULL DEFAULT 0, updated_utc INTEGER NOT NULL); CREATE TABLE IF NOT EXISTS video_adjustments (sha256 BLOB PRIMARY KEY, adjustment_version INTEGER NOT NULL, exposure REAL NOT NULL, brightness REAL NOT NULL, contrast REAL NOT NULL, shadows REAL NOT NULL, highlights REAL NOT NULL, saturation REAL NOT NULL, sharpness REAL NOT NULL DEFAULT 0, updated_utc INTEGER NOT NULL); CREATE TABLE IF NOT EXISTS app_settings (key TEXT PRIMARY KEY, value INTEGER NOT NULL);")) { CloseDatabase(); return false; }
        if (version == 1 && !Execute("BEGIN IMMEDIATE; ALTER TABLE image_adjustments ADD COLUMN sharpness REAL NOT NULL DEFAULT 0; PRAGMA user_version=4; COMMIT;")) { CloseDatabase(); return false; }
        if (version < 4 && version != 1 && !Execute("PRAGMA user_version=4;")) { CloseDatabase(); return false; }
        Trace(L"[Viewtrious] WINSQLITE_RUNTIME_LOADED"); Trace(L"[Viewtrious] SQLITE_DB_OPEN"); Trace(L"[Viewtrious] SQLITE_SCHEMA_READY");
        return true;
    }

    void CloseDatabase() {
        if (database && close) close(database);
        database = nullptr;
        if (module) FreeLibrary(module);
        module = nullptr;
    }

    bool Execute(const char* sql) {
        char* error = nullptr;
        const int result = exec(database, sql, nullptr, nullptr, &error);
        if (error) free(error);
        if (result != kSqliteOk) TraceSqliteError(L"exec", result);
        return result == kSqliteOk;
    }

    bool Prepare(const char* sql, sqlite3_stmt** statement) {
        const int result = prepareV2(database, sql, -1, statement, nullptr);
        if (result != kSqliteOk) TraceSqliteError(L"prepare", result);
        return result == kSqliteOk;
    }

    bool ReadCache(const std::string& path, const FileMetadata& metadata, std::array<unsigned char, 32>& hash) {
        sqlite3_stmt* statement = nullptr;
        if (!Prepare("SELECT file_size,file_mtime,sha256 FROM file_hash_cache WHERE path=?1;", &statement)) return false;
        bindText(statement, 1, path.c_str(), static_cast<int>(path.size()), kSqliteTransient);
        const bool hit = step(statement) == kSqliteRow && columnInt64(statement, 0) == metadata.size && columnInt64(statement, 1) == metadata.mtime && columnBytes(statement, 2) == static_cast<int>(hash.size());
        if (hit) std::memcpy(hash.data(), columnBlob(statement, 2), hash.size());
        finalize(statement);
        return hit;
    }

    void WriteCache(const std::string& path, const FileMetadata& metadata, const std::array<unsigned char, 32>& hash) {
        sqlite3_stmt* statement = nullptr;
        if (!Prepare("INSERT INTO file_hash_cache(path,file_size,file_mtime,sha256) VALUES(?1,?2,?3,?4) ON CONFLICT(path) DO UPDATE SET file_size=excluded.file_size,file_mtime=excluded.file_mtime,sha256=excluded.sha256;", &statement)) return;
        bindText(statement, 1, path.c_str(), static_cast<int>(path.size()), kSqliteTransient);
        bindInt64(statement, 2, metadata.size); bindInt64(statement, 3, metadata.mtime);
        bindBlob(statement, 4, hash.data(), static_cast<int>(hash.size()), kSqliteTransient);
        const int result = step(statement);
        if (result != kSqliteDone) TraceSqliteError(L"hash cache write", result);
        finalize(statement);
    }

    const char* AdjustmentsTable(AdjustmentMediaKind mediaKind) const {
        return mediaKind == AdjustmentMediaKind::Video ? "video_adjustments" : "image_adjustments";
    }

    bool ReadAdjustments(const std::array<unsigned char, 32>& hash, ImageAdjustments& adjustments, AdjustmentMediaKind mediaKind) {
        sqlite3_stmt* statement = nullptr;
        const std::string query = std::string("SELECT adjustment_version,exposure,brightness,contrast,shadows,highlights,saturation,sharpness FROM ") + AdjustmentsTable(mediaKind) + " WHERE sha256=?1;";
        if (!Prepare(query.c_str(), &statement)) return false;
        bindBlob(statement, 1, hash.data(), static_cast<int>(hash.size()), kSqliteTransient);
        const bool hit = step(statement) == kSqliteRow && columnInt(statement, 0) == kAdjustmentVersion;
        if (hit) adjustments = { static_cast<float>(columnDouble(statement, 1)), static_cast<float>(columnDouble(statement, 2)), static_cast<float>(columnDouble(statement, 3)), static_cast<float>(columnDouble(statement, 4)), static_cast<float>(columnDouble(statement, 5)), static_cast<float>(columnDouble(statement, 6)), static_cast<float>(columnDouble(statement, 7)) };
        finalize(statement);
        return hit;
    }

    void SaveAdjustments(const std::array<unsigned char, 32>& hash, const ImageAdjustments& adjustments, AdjustmentMediaKind mediaKind) {
        sqlite3_stmt* statement = nullptr;
        const char* table = AdjustmentsTable(mediaKind);
        if (IsNeutral(adjustments)) {
            const std::string query = std::string("DELETE FROM ") + table + " WHERE sha256=?1;";
            if (!Prepare(query.c_str(), &statement)) return;
            bindBlob(statement, 1, hash.data(), static_cast<int>(hash.size()), kSqliteTransient);
            const int result = step(statement);
            if (result != kSqliteDone) TraceSqliteError(L"adjustment delete", result); else Trace(L"[Viewtrious] ADJUST_DB_DELETE_NEUTRAL");
            finalize(statement); return;
        }
        const std::string query = std::string("INSERT INTO ") + table + "(sha256,adjustment_version,exposure,brightness,contrast,shadows,highlights,saturation,sharpness,updated_utc) VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9,?10) ON CONFLICT(sha256) DO UPDATE SET adjustment_version=excluded.adjustment_version,exposure=excluded.exposure,brightness=excluded.brightness,contrast=excluded.contrast,shadows=excluded.shadows,highlights=excluded.highlights,saturation=excluded.saturation,sharpness=excluded.sharpness,updated_utc=excluded.updated_utc;";
        if (!Prepare(query.c_str(), &statement)) return;
        bindBlob(statement, 1, hash.data(), static_cast<int>(hash.size()), kSqliteTransient);
        bindInt64(statement, 2, kAdjustmentVersion); bindDouble(statement, 3, adjustments.exposure); bindDouble(statement, 4, adjustments.brightness); bindDouble(statement, 5, adjustments.contrast); bindDouble(statement, 6, adjustments.shadows); bindDouble(statement, 7, adjustments.highlights); bindDouble(statement, 8, adjustments.saturation); bindDouble(statement, 9, adjustments.sharpness); bindInt64(statement, 10, static_cast<int64_t>(std::time(nullptr)));
        const int result = step(statement);
        if (result != kSqliteDone) TraceSqliteError(L"adjustment save", result); else Trace(L"[Viewtrious] ADJUST_DB_SAVE");
        finalize(statement);
    }

    bool TaskCurrent(const Task& task) const {
        return enabled.load(std::memory_order_acquire) &&
            persistenceGeneration.load(std::memory_order_acquire) == task.persistenceGeneration;
    }

    void ResolveMedia(const Task& task) {
        if (!database || !TaskCurrent(task)) return;
        const std::wstring normalized = NormalizedPath(task.path);
        HANDLE file = CreateFileW(task.path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        FileMetadata metadata{};
        if (file == INVALID_HANDLE_VALUE || !ReadMetadata(file, metadata)) { if (file != INVALID_HANDLE_VALUE) CloseHandle(file); return; }
        CloseHandle(file);
        std::array<unsigned char, 32> hash{};
        const std::string path = Utf8(normalized);
        if (!TaskCurrent(task)) return;
        if (ReadCache(path, metadata, hash)) Trace(L"[Viewtrious] ADJUST_HASH_CACHE_HIT");
        else {
            Trace(L"[Viewtrious] ADJUST_HASH_CACHE_MISS"); Trace(L"[Viewtrious] ADJUST_HASH_BEGIN");
            if (!HashFile(task.path, hash, metadata, [this, &task] { return !TaskCurrent(task); })) {
                if (TaskCurrent(task)) Trace(L"[Viewtrious] ADJUST_HASH_STALE_FILE");
                return;
            }
            if (!TaskCurrent(task)) return;
            WriteCache(path, metadata, hash); Trace(L"[Viewtrious] ADJUST_HASH_COMPLETE");
        }
        if (!TaskCurrent(task)) return;
        ImageAdjustmentPersistenceResult result{};
        result.mediaKind = task.mediaKind; result.path = task.path; result.mediaGeneration = task.mediaGeneration; result.editGeneration = task.editGeneration;
        result.hash = hash; result.hashResolved = true; result.hasAdjustments = ReadAdjustments(hash, result.adjustments, task.mediaKind);
        if (TaskCurrent(task) && completion) completion(std::move(result));
    }

    void Run() {
        const bool available = OpenDatabase();
        for (;;) {
            Task task;
            { std::unique_lock lock(mutex); wake.wait(lock, [this] { return stopping || !tasks.empty(); }); if (tasks.empty() && stopping) break; task = std::move(tasks.front()); tasks.pop_front(); }
            std::lock_guard operationLock(operationMutex);
            if (task.kind == TaskKind::Reset) {
                bool reset = available && Execute("BEGIN IMMEDIATE; DELETE FROM image_adjustments; DELETE FROM video_adjustments; DELETE FROM file_hash_cache; COMMIT;");
                if (available && !reset) Execute("ROLLBACK;");
                if (task.resetCompletion) task.resetCompletion->set_value(reset);
                continue;
            }
            if (!available || !TaskCurrent(task)) continue;
            if (task.kind == TaskKind::Resolve) ResolveMedia(task);
            else if (TaskCurrent(task)) SaveAdjustments(task.hash, task.adjustments, task.mediaKind);
        }
        CloseDatabase();
    }
};

ImageAdjustmentPersistence::ImageAdjustmentPersistence() : impl_(new Impl) {}
ImageAdjustmentPersistence::~ImageAdjustmentPersistence() { Shutdown(); delete impl_; }

void ImageAdjustmentPersistence::Start(std::function<void(ImageAdjustmentPersistenceResult&&)> completion) {
    if (!impl_ || impl_->thread.joinable()) return;
    impl_->completion = std::move(completion); impl_->stopping = false; impl_->thread = std::thread([impl = impl_] { impl->Run(); });
}

void ImageAdjustmentPersistence::SetEnabled(bool enabled) {
    if (!impl_) return;
    {
        std::lock_guard lock(impl_->mutex);
        impl_->enabled.store(enabled, std::memory_order_release);
        impl_->persistenceGeneration.fetch_add(1, std::memory_order_acq_rel);
        impl_->tasks.erase(std::remove_if(impl_->tasks.begin(), impl_->tasks.end(), [](const Impl::Task& task) {
            return task.kind != Impl::TaskKind::Reset;
        }), impl_->tasks.end());
    }
    impl_->wake.notify_one();
    if (!enabled) {
        std::lock_guard operationLock(impl_->operationMutex);
    }
}

void ImageAdjustmentPersistence::Resolve(const std::wstring& path, uint64_t mediaGeneration, uint64_t editGeneration, AdjustmentMediaKind mediaKind) {
    if (!impl_ || path.empty() || !impl_->enabled.load(std::memory_order_acquire)) return;
    { std::lock_guard lock(impl_->mutex); if (impl_->stopping || !impl_->enabled.load(std::memory_order_relaxed)) return; Impl::Task task{}; task.kind = Impl::TaskKind::Resolve; task.mediaKind = mediaKind; task.path = path; task.mediaGeneration = mediaGeneration; task.editGeneration = editGeneration; task.persistenceGeneration = impl_->persistenceGeneration.load(std::memory_order_relaxed); impl_->tasks.push_back(std::move(task)); }
    impl_->wake.notify_one();
}

void ImageAdjustmentPersistence::Save(const std::array<unsigned char, 32>& hash, const ImageAdjustments& adjustments, AdjustmentMediaKind mediaKind) {
    if (!impl_ || !impl_->enabled.load(std::memory_order_acquire)) return;
    { std::lock_guard lock(impl_->mutex); if (impl_->stopping || !impl_->enabled.load(std::memory_order_relaxed)) return; Impl::Task task{}; task.kind = Impl::TaskKind::Save; task.mediaKind = mediaKind; task.hash = hash; task.adjustments = adjustments; task.persistenceGeneration = impl_->persistenceGeneration.load(std::memory_order_relaxed); impl_->tasks.push_back(std::move(task)); }
    impl_->wake.notify_one();
}

bool ImageAdjustmentPersistence::Reset() {
    if (!impl_ || !impl_->thread.joinable()) return false;
    auto completion = std::make_shared<std::promise<bool>>();
    std::future<bool> result = completion->get_future();
    {
        std::lock_guard lock(impl_->mutex);
        if (impl_->stopping) return false;
        impl_->persistenceGeneration.fetch_add(1, std::memory_order_acq_rel);
        impl_->tasks.clear();
        Impl::Task task{};
        task.kind = Impl::TaskKind::Reset;
        task.resetCompletion = std::move(completion);
        impl_->tasks.push_back(std::move(task));
    }
    impl_->wake.notify_one();
    return result.get();
}

void ImageAdjustmentPersistence::Shutdown() {
    if (!impl_ || !impl_->thread.joinable()) return;
    { std::lock_guard lock(impl_->mutex); impl_->stopping = true; }
    impl_->wake.notify_one(); impl_->thread.join();
}
