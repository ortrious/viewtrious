#pragma once

#include "model_document.h"

#include <memory>
#include <string>

struct ThreeMfLoadResult {
    std::shared_ptr<ModelDocument> document;
    std::wstring error;
    bool IsSuccess() const { return document != nullptr; }
};

ThreeMfLoadResult LoadThreeMfDocument(const std::wstring& path);
