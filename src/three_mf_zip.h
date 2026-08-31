#pragma once

#include <string>
#include <vector>

bool ReadThreeMfModelXml(const std::wstring& path, std::vector<unsigned char>& xml, std::wstring& modelPartPath, std::wstring& error);
