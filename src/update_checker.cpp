#include "update_checker.h"

#include <winhttp.h>

#include <algorithm>
#include <array>
#include <cwctype>
#include <limits>
#include <memory>

namespace {

constexpr wchar_t kUpdateHost[] = L"viewtrious.com";
constexpr wchar_t kUpdatePath[] = L"/update.json";
constexpr DWORD kTimeoutMs = 5000;
constexpr DWORD kMaximumResponseBytes = 16 * 1024;
constexpr size_t kMaximumFieldLength = 2048;

bool ParseVersionImpl(const std::wstring& text, std::array<unsigned int, 4>& version) {
    size_t begin = 0;
    for (size_t part = 0; part < version.size(); ++part) {
        const size_t end = text.find(L'.', begin);
        if ((part == version.size() - 1) != (end == std::wstring::npos)) return false;
        const std::wstring component = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component.empty() || component.size() > 10) return false;
        unsigned long long value = 0;
        for (wchar_t character : component) {
            if (!iswdigit(character)) return false;
            value = value * 10 + static_cast<unsigned int>(character - L'0');
            if (value > std::numeric_limits<unsigned int>::max()) return false;
        }
        version[part] = static_cast<unsigned int>(value);
        begin = end + 1;
    }
    return true;
}

bool IsNewerVersion(const std::wstring& remote) {
    std::array<unsigned int, 4> remoteParts{}, localParts{};
    return ParseVersionImpl(remote, remoteParts) && ParseVersionImpl(VIEWTRIOUS_VERSION, localParts) && remoteParts > localParts;
}

bool DecodeUtf8(const std::string& input, std::wstring& output) {
    if (input.empty()) { output.clear(); return true; }
    const int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), nullptr, 0);
    if (!length) return false;
    output.resize(static_cast<size_t>(length));
    return MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, input.data(), static_cast<int>(input.size()), output.data(), length) == length;
}

void SkipWhitespace(const std::wstring& input, size_t& cursor) { while (cursor < input.size() && iswspace(input[cursor])) ++cursor; }

bool ReadString(const std::wstring& input, size_t& cursor, std::wstring& value) {
    if (cursor >= input.size() || input[cursor++] != L'\"') return false;
    value.clear();
    while (cursor < input.size()) {
        wchar_t c = input[cursor++];
        if (c == L'\"') return value.size() <= kMaximumFieldLength;
        if (c < 0x20) return false;
        if (c != L'\\') { value.push_back(c); continue; }
        if (cursor >= input.size()) return false;
        c = input[cursor++];
        if (c == L'\"' || c == L'\\' || c == L'/') value.push_back(c);
        else if (c == L'b') value.push_back(L'\b'); else if (c == L'f') value.push_back(L'\f');
        else if (c == L'n') value.push_back(L'\n'); else if (c == L'r') value.push_back(L'\r'); else if (c == L't') value.push_back(L'\t');
        else return false; // This focused schema intentionally rejects \u escapes.
        if (value.size() > kMaximumFieldLength) return false;
    }
    return false;
}

bool ReadInteger(const std::wstring& input, size_t& cursor, unsigned int& value) {
    const size_t begin = cursor;
    unsigned long long parsed = 0;
    while (cursor < input.size() && iswdigit(input[cursor])) { parsed = parsed * 10 + input[cursor++] - L'0'; if (parsed > UINT_MAX) return false; }
    if (cursor == begin) return false;
    value = static_cast<unsigned int>(parsed); return true;
}

bool ParseResponse(const std::string& bytes, UpdateCheckResult& result) {
    std::wstring json;
    if (!DecodeUtf8(bytes, json)) return false;
    size_t cursor = 0; SkipWhitespace(json, cursor); if (cursor >= json.size() || json[cursor++] != L'{') return false;
    bool schemaSeen = false, versionSeen = false, urlSeen = false;
    unsigned int schema = 0;
    while (true) {
        SkipWhitespace(json, cursor); if (cursor < json.size() && json[cursor] == L'}') { ++cursor; break; }
        std::wstring key; if (!ReadString(json, cursor, key)) return false;
        SkipWhitespace(json, cursor); if (cursor >= json.size() || json[cursor++] != L':') return false;
        SkipWhitespace(json, cursor);
        if (key == L"schema") { if (schemaSeen || !ReadInteger(json, cursor, schema)) return false; schemaSeen = true; }
        else if (key == L"latest_version") { if (versionSeen || !ReadString(json, cursor, result.latestVersion)) return false; versionSeen = true; }
        else if (key == L"release_url") { if (urlSeen || !ReadString(json, cursor, result.releaseUrl)) return false; urlSeen = true; }
        else if (key == L"message") { if (!ReadString(json, cursor, result.message)) return false; }
        else return false;
        SkipWhitespace(json, cursor); if (cursor >= json.size()) return false;
        if (json[cursor] == L'}') { ++cursor; break; }
        if (json[cursor++] != L',') return false;
    }
    SkipWhitespace(json, cursor);
    std::array<unsigned int, 4> parsedVersion{};
    if (cursor != json.size() || !schemaSeen || schema != 1 || !versionSeen || !ParseVersionImpl(result.latestVersion, parsedVersion)) return false;
    result.updateAvailable = IsNewerVersion(result.latestVersion);
    if (result.updateAvailable && (!urlSeen || result.releaseUrl.rfind(L"https://", 0) != 0)) return false;
    if (result.message.empty()) result.message = L"a new version of viewtrious is available.";
    result.succeeded = true;
    return true;
}

bool Fetch(UpdateCheckResult& result) {
    HINTERNET session = WinHttpOpen(L"viewtrious/" VIEWTRIOUS_VERSION, WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return false;
    WinHttpSetTimeouts(session, kTimeoutMs, kTimeoutMs, kTimeoutMs, kTimeoutMs);
    HINTERNET connection = WinHttpConnect(session, kUpdateHost, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET request = connection ? WinHttpOpenRequest(connection, L"GET", kUpdatePath, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    std::string body;
    bool success = request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr);
    DWORD status = 0, size = sizeof(status);
    success = success && WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX) && status == 200;
    while (success) {
        DWORD available = 0;
        success = WinHttpQueryDataAvailable(request, &available) != FALSE;
        if (!success || !available) break;
        if (available > kMaximumResponseBytes - body.size()) { success = false; break; }
        const size_t offset = body.size(); body.resize(offset + available);
        DWORD read = 0; success = WinHttpReadData(request, body.data() + offset, available, &read) != FALSE;
        body.resize(offset + read);
    }
    if (request) WinHttpCloseHandle(request); if (connection) WinHttpCloseHandle(connection); WinHttpCloseHandle(session);
    return success && !body.empty() && ParseResponse(body, result);
}
}

bool ParseUpdateVersion(const std::wstring& text, std::array<unsigned int, 4>& version) {
    return ParseVersionImpl(text, version);
}

UpdateChecker::~UpdateChecker() { Shutdown(); }
bool UpdateChecker::Start(HWND window, UINT completionMessage) {
    if (active_.exchange(true)) return false;
    if (worker_.joinable()) worker_.join();
    shuttingDown_ = false;
    worker_ = std::thread([this, window, completionMessage] {
        auto result = std::make_unique<UpdateCheckResult>();
        Fetch(*result);
        active_ = false;
        if (shuttingDown_ || !PostMessageW(window, completionMessage, 0, reinterpret_cast<LPARAM>(result.get()))) return;
        result.release();
    });
    return true;
}
void UpdateChecker::Shutdown() { shuttingDown_ = true; if (worker_.joinable()) worker_.join(); }
