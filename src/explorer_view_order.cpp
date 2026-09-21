#include "explorer_view_order.h"

#include <ole2.h>
#include <exdisp.h>
#include <servprov.h>
#include <shlguid.h>
#include <shlobj_core.h>
#include <shobjidl_core.h>
#include <wrl/client.h>

#include <algorithm>
#include <utility>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;

namespace {

struct OwnedPidl {
    PIDLIST_ABSOLUTE value = nullptr;
    OwnedPidl() = default;
    explicit OwnedPidl(PIDLIST_ABSOLUTE owned) : value(owned) {}
    OwnedPidl(const OwnedPidl&) = delete;
    OwnedPidl& operator=(const OwnedPidl&) = delete;
    OwnedPidl(OwnedPidl&& other) noexcept : value(std::exchange(other.value, nullptr)) {}
    OwnedPidl& operator=(OwnedPidl&& other) noexcept {
        if (this != &other) { CoTaskMemFree(value); value = std::exchange(other.value, nullptr); }
        return *this;
    }
    ~OwnedPidl() { CoTaskMemFree(value); }
    explicit operator bool() const { return value != nullptr; }
    PCIDLIST_ABSOLUTE get() const { return value; }
};

HWND RootWindow(HWND window) {
    return window ? GetAncestor(window, GA_ROOT) : nullptr;
}

struct ExplorerCandidate {
    HWND window = nullptr;
    ComPtr<IFolderView> view;
    ComPtr<IShellFolder> folder;
    OwnedPidl folderPidl;
};

std::vector<fs::path> ReadViewItems(const ExplorerCandidate& candidate) {
    std::vector<fs::path> paths;
    int count = 0;
    if (!candidate.view || !candidate.folderPidl ||
        FAILED(candidate.view->ItemCount(SVGIO_ALLVIEW, &count)) || count <= 0) return paths;
    paths.reserve(static_cast<size_t>(count));
    for (int index = 0; index < count; ++index) {
        PITEMID_CHILD relative = nullptr;
        if (FAILED(candidate.view->Item(index, &relative)) || !relative) continue;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(SHCreateItemWithParent(candidate.folderPidl.get(), candidate.folder.Get(), relative,
                IID_PPV_ARGS(&item))) && item && SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path)
            paths.emplace_back(path);
        if (path) CoTaskMemFree(path);
        CoTaskMemFree(relative);
    }
    return paths;
}

} // namespace

std::optional<ExplorerViewOrder> ReadExplorerViewOrder(const fs::path& folder, HWND preferredWindow) {
    PIDLIST_ABSOLUTE targetRaw = nullptr;
    if (FAILED(SHParseDisplayName(folder.c_str(), nullptr, &targetRaw, 0, nullptr)) || !targetRaw) return std::nullopt;
    OwnedPidl target(targetRaw);

    ComPtr<IShellWindows> windows;
    if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows)))) return std::nullopt;
    long count = 0;
    if (FAILED(windows->get_Count(&count)) || count <= 0) return std::nullopt;

    std::vector<ExplorerCandidate> matches;
    matches.reserve(2);
    for (long index = 0; index < count; ++index) {
        VARIANT itemIndex{};
        VariantInit(&itemIndex);
        itemIndex.vt = VT_I4;
        itemIndex.lVal = index;
        ComPtr<IDispatch> dispatch;
        if (FAILED(windows->Item(itemIndex, &dispatch)) || !dispatch) continue;
        ComPtr<IServiceProvider> services;
        ComPtr<IShellBrowser> browser;
        ComPtr<IShellView> shellView;
        ComPtr<IFolderView> folderView;
        ComPtr<IShellFolder> shellFolder;
        ComPtr<IPersistFolder2> persistedFolder;
        if (FAILED(dispatch.As(&services)) ||
            FAILED(services->QueryService(SID_STopLevelBrowser, IID_PPV_ARGS(&browser))) ||
            FAILED(browser->QueryActiveShellView(&shellView)) || FAILED(shellView.As(&folderView)) ||
            FAILED(folderView->GetFolder(IID_PPV_ARGS(&shellFolder))) || FAILED(shellFolder.As(&persistedFolder))) continue;
        PIDLIST_ABSOLUTE folderRaw = nullptr;
        if (FAILED(persistedFolder->GetCurFolder(&folderRaw)) || !folderRaw) continue;
        OwnedPidl folderPidl(folderRaw);
        if (!ILIsEqual(target.get(), folderPidl.get())) continue;
        HWND browserWindow = nullptr;
        browser->GetWindow(&browserWindow);
        matches.push_back({ RootWindow(browserWindow), std::move(folderView), std::move(shellFolder), std::move(folderPidl) });
    }

    if (matches.empty()) return std::nullopt;
    ExplorerCandidate* selected = nullptr;
    const HWND preferredRoot = RootWindow(preferredWindow);
    if (preferredRoot) {
        const auto match = std::find_if(matches.begin(), matches.end(), [preferredRoot](const ExplorerCandidate& candidate) {
            return candidate.window == preferredRoot;
        });
        if (match != matches.end() && std::count_if(matches.begin(), matches.end(), [preferredRoot](const ExplorerCandidate& candidate) {
                return candidate.window == preferredRoot;
            }) == 1) selected = &*match;
    }
    if (!selected && matches.size() == 1) selected = &matches.front();
    if (!selected) return std::nullopt;

    std::vector<fs::path> items = ReadViewItems(*selected);
    if (items.empty()) return std::nullopt;
    return ExplorerViewOrder{ selected->window, std::move(items) };
}
