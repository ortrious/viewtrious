#pragma once

#include "model_document.h"

#include <memory>
#include <string>

struct StepLoadResult {
    std::shared_ptr<ModelDocument> document;
    std::wstring error;
    bool IsSuccess() const { return document != nullptr; }
};

StepLoadResult LoadStepDocument(const std::wstring& path);
