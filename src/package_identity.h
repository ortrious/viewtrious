#pragma once

#include <string>

namespace ViewtriousPackage {

bool IsPackagedProcess();
bool TryGetCurrentApplicationUserModelId(std::wstring& applicationUserModelId);

} // namespace ViewtriousPackage
