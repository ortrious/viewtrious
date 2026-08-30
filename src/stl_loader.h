#pragma once

#include "model_document.h"

#include <memory>
#include <string>

struct StlLoadResult {
    std::shared_ptr<ModelDocument> document;
    std::wstring error;
    bool IsSuccess() const { return document != nullptr; }
};

StlLoadResult LoadStlDocument(const std::wstring& path);
