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

} // namespace ViewtriousPackage
