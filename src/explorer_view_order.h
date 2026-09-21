#pragma once

#include <windows.h>

#include <filesystem>
#include <optional>
#include <vector>

struct ExplorerViewOrder {
    HWND window = nullptr;
    std::vector<std::filesystem::path> items;
};

// Returns the active Explorer view's filesystem items in view-index order.
// Ambiguous same-folder views are rejected unless preferredWindow identifies one.
std::optional<ExplorerViewOrder> ReadExplorerViewOrder(
    const std::filesystem::path& folder, HWND preferredWindow);
