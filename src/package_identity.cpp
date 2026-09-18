#include "package_identity.h"

#include <windows.h>
#include <appmodel.h>

namespace ViewtriousPackage {

bool IsPackagedProcess() {
    static const bool packaged = [] {
        UINT32 length = 0;
        const LONG result = GetCurrentPackageFullName(&length, nullptr);
        return result == ERROR_INSUFFICIENT_BUFFER || result == ERROR_SUCCESS;
    }();
    return packaged;
}

bool TryGetCurrentApplicationUserModelId(std::wstring& applicationUserModelId) {
    applicationUserModelId.clear();
    UINT32 length = 0;
    if (GetCurrentApplicationUserModelId(&length, nullptr) != ERROR_INSUFFICIENT_BUFFER || length <= 1) return false;

    std::wstring buffer(length, L'\0');
    if (GetCurrentApplicationUserModelId(&length, buffer.data()) != ERROR_SUCCESS || length <= 1) return false;
    buffer.resize(length - 1);
    if (buffer.empty()) return false;
    applicationUserModelId.swap(buffer);
    return true;
}

} // namespace ViewtriousPackage
