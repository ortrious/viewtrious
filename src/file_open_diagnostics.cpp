#include "file_open_diagnostics.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <string>

namespace {

constexpr DWORD kMaximumLogBytes = 512 * 1024;
std::mutex gLogMutex;
uint64_t gNextOpenAttemptId = 0;

std::filesystem::path LogDirectory() {
    wchar_t localAppData[MAX_PATH]{};
    const DWORD length = GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData, ARRAYSIZE(localAppData));
    if (!length || length >= ARRAYSIZE(localAppData)) return {};
    return std::filesystem::path(localAppData) / L"Viewtrious" / L"diagnostics";
}

std::string Utf8(std::wstring_view value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

void AppendLine(std::wstring_view line) {
    std::lock_guard lock(gLogMutex);
    const std::filesystem::path directory = LogDirectory();
    if (directory.empty()) return;
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return;
    const std::filesystem::path current = directory / L"file-open.log";
    const std::filesystem::path previous = directory / L"file-open.previous.log";
    const uintmax_t size = std::filesystem::file_size(current, error);
    if (!error && size >= kMaximumLogBytes) {
        std::filesystem::remove(previous, error);
        error.clear();
        std::filesystem::rename(current, previous, error);
        if (error) {
            error.clear();
            std::filesystem::remove(current, error);
        }
    }
    const std::string utf8 = Utf8(line);
    if (utf8.empty()) return;
    HANDLE file = CreateFileW(current.c_str(), FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(file, utf8.data(), static_cast<DWORD>(utf8.size()), &written, nullptr);
    FlushFileBuffers(file);
    CloseHandle(file);
}

std::wstring Timestamp() {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    wchar_t result[48]{};
    swprintf_s(result, L"%02u:%02u:%02u.%03u", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds);
    return result;
}

std::wstring FileFacts(std::wstring_view path) {
    WIN32_FILE_ATTRIBUTE_DATA attributes{};
    const std::filesystem::path file(std::wstring(path));
    std::wstring facts = L"file=\"" + file.filename().wstring() + L"\"";
    if (!GetFileAttributesExW(std::wstring(path).c_str(), GetFileExInfoStandard, &attributes))
        return facts + L" exists=0 win32=" + std::to_wstring(GetLastError());
    const uint64_t size = (static_cast<uint64_t>(attributes.nFileSizeHigh) << 32) | attributes.nFileSizeLow;
    return facts + L" exists=1 size=" + std::to_wstring(size) + L" write=" +
        std::to_wstring((static_cast<uint64_t>(attributes.ftLastWriteTime.dwHighDateTime) << 32) | attributes.ftLastWriteTime.dwLowDateTime);
}

} // namespace

namespace FileOpenDiagnostics {

uint64_t Begin(std::wstring_view path, std::wstring_view route, std::wstring_view mediaKind) {
    const uint64_t id = ++gNextOpenAttemptId;
    Log(id, L"open-begin", L"route=" + std::wstring(route) + L" kind=" + std::wstring(mediaKind) + L" " + FileFacts(path));
    return id;
}

void Log(uint64_t openAttemptId, std::wstring_view event, std::wstring_view detail) {
    std::wstring line = Timestamp() + L" pid=" + std::to_wstring(GetCurrentProcessId()) + L" open=" + std::to_wstring(openAttemptId) +
        L" " + std::wstring(event);
    if (!detail.empty()) line += L" " + std::wstring(detail);
    line += L"\r\n";
    AppendLine(line);
}

} // namespace FileOpenDiagnostics
