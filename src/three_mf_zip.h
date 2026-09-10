#pragma once

#include <string>
#include <vector>

bool ReadThreeMfModelXml(const std::wstring& path, std::vector<unsigned char>& xml, std::wstring& modelPartPath, std::wstring& error);
bool ReadThreeMfModelXmlPart(const std::wstring& path, const std::wstring& modelPartPath, std::vector<unsigned char>& xml, std::wstring& error);
bool ReadThreeMfPackagePart(const std::wstring& path, const std::wstring& partPath, std::vector<unsigned char>& bytes, std::wstring& error);
