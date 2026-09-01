#pragma once

#include "model_document.h"

#include <memory>
#include <string>

bool StepAddonPresent();
std::shared_ptr<ModelDocument> LoadStepDocumentFromAddon(const std::wstring& path, std::wstring& error);
