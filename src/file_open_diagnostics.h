#pragma once

#include <cstdint>
#include <string_view>

namespace FileOpenDiagnostics {

uint64_t Begin(std::wstring_view path, std::wstring_view route, std::wstring_view mediaKind);
void Log(uint64_t openAttemptId, std::wstring_view event, std::wstring_view detail = {});

} // namespace FileOpenDiagnostics
