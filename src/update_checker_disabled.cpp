#include "update_checker.h"

#include <cwctype>
#include <limits>

bool ParseUpdateVersion(const std::wstring& text, std::array<unsigned int, 4>& version) {
    version.fill(0);
    size_t begin = 0;
    for (size_t part = 0; part < version.size(); ++part) {
        const size_t end = text.find(L'.', begin);
        if ((part < 2 && end == std::wstring::npos) || (part == version.size() - 1 && end != std::wstring::npos)) return false;
        const std::wstring component = text.substr(begin, end == std::wstring::npos ? end : end - begin);
        if (component.empty() || component.size() > 10) return false;
        unsigned long long value = 0;
        for (const wchar_t character : component) {
            if (!iswdigit(character)) return false;
            value = value * 10 + static_cast<unsigned int>(character - L'0');
            if (value > std::numeric_limits<unsigned int>::max()) return false;
        }
        version[part] = static_cast<unsigned int>(value);
        if (end == std::wstring::npos) return true; // Three-part app versions and legacy four-part release values.
        begin = end + 1;
    }
    return false;
}

UpdateChecker::~UpdateChecker() = default;
bool UpdateChecker::Start(HWND, UINT) { return false; }
void UpdateChecker::Shutdown() {}
