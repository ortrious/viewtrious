#pragma once

#include "model_document.h"

#include <memory>
#include <string>

using StepLoadProgressCallback = void (*)(void* context, float normalizedProgress);

bool StepAddonPresent();
std::shared_ptr<ModelDocument> LoadStepDocumentFromAddon(const std::wstring& path, std::wstring& error, StepLoadProgressCallback progress = nullptr, void* progressContext = nullptr);
