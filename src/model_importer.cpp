#include "model_importer.h"
#include "model_importer_abi.h"

#include <windows.h>

#include <filesystem>
#include <mutex>
#include <cmath>
#include <unordered_map>

namespace {
struct StepAddon {
    HMODULE module = nullptr;
    ViewtriousModelImporterApi api{};
    std::wstring error;
};

std::once_flag g_stepAddonOnce;
StepAddon g_stepAddon;

std::filesystem::path AddonPath() {
    wchar_t executable[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, executable, ARRAYSIZE(executable));
    if (!length || length == ARRAYSIZE(executable)) return {};
    return std::filesystem::path(executable).parent_path() / L"Addons" / L"STEP" / L"ViewtriousStep.dll";
}

void LoadAddonOnce() {
    const std::filesystem::path path = AddonPath();
    if (!std::filesystem::exists(path)) { g_stepAddon.error = L"STEP/STP support requires the optional Viewtrious STEP add-on."; return; }
    g_stepAddon.module = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    if (!g_stepAddon.module) { g_stepAddon.error = L"The optional Viewtrious STEP add-on could not be loaded."; return; }
    g_stepAddon.api.structSize = sizeof(g_stepAddon.api);
    const auto getApi = reinterpret_cast<decltype(&ViewtriousModelImporter_GetApi)>(GetProcAddress(g_stepAddon.module, "ViewtriousModelImporter_GetApi"));
    if (!getApi || !getApi(kViewtriousModelImporterApiVersion, &g_stepAddon.api) || g_stepAddon.api.structSize != sizeof(ViewtriousModelImporterApi) || g_stepAddon.api.apiVersion != kViewtriousModelImporterApiVersion || !g_stepAddon.api.loadStep || !g_stepAddon.api.releaseResult || !g_stepAddon.api.releaseDocumentContext) {
        g_stepAddon.error = L"The optional Viewtrious STEP add-on is incompatible."; return;
    }
}

StepAddon& Addon() { std::call_once(g_stepAddonOnce, LoadAddonOnce); return g_stepAddon; }

void BuildSnapPlanes(ModelDocument& document) {
    const auto& mesh = document.geometries.front(); const auto sub=[](Float3 a,Float3 b){return Float3{a.x-b.x,a.y-b.y,a.z-b.z};}; const auto dot=[](Float3 a,Float3 b){return a.x*b.x+a.y*b.y+a.z*b.z;}; const auto length=[&](Float3 value){return std::sqrt(dot(value,value));}; const Float3 span=sub(document.bounds.maximum,document.bounds.minimum); const float scale=std::max(1e-5f,length(span)),normalStep=.002f,offsetStep=scale*1e-4f; std::unordered_map<std::string,uint32_t> ids; document.triangleSnapPlanes.resize(mesh.indices.size()/3);
    for(size_t triangle=0;triangle<mesh.indices.size()/3;++triangle){const Float3 normal=mesh.normals[mesh.indices[triangle*3]];const float offset=dot(normal,mesh.positions[mesh.indices[triangle*3]]);const auto q=[](float value,float step){return int(std::lround(value/step));};const std::string key=std::to_string(q(normal.x,normalStep))+":"+std::to_string(q(normal.y,normalStep))+":"+std::to_string(q(normal.z,normalStep))+":"+std::to_string(q(offset,offsetStep));auto [entry,added]=ids.emplace(key,uint32_t(document.snapPlanes.size()));if(added)document.snapPlanes.push_back({normal,offset,{}});document.triangleSnapPlanes[triangle]=entry->second;document.snapPlanes[entry->second].triangles.push_back(uint32_t(triangle));}
}

std::wstring Utf8ToWide(const char* text, uint32_t length) {
    if (!text || !length) return {};
    const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), nullptr, 0);
    if (!count) return {};
    std::wstring result(static_cast<size_t>(count), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text, static_cast<int>(length), result.data(), count)) return {};
    return result;
}
}

bool StepAddonPresent() { return std::filesystem::exists(AddonPath()); }

std::shared_ptr<ModelDocument> LoadStepDocumentFromAddon(const std::wstring& path, std::wstring& error, StepLoadProgressCallback progress, void* progressContext) {
    StepAddon& addon = Addon();
    if (!addon.module || addon.error.size()) { error = addon.error; return nullptr; }
    ViewtriousImporterResult result{}; result.structSize = sizeof(result); result.apiVersion = kViewtriousModelImporterApiVersion;
    if (!addon.api.loadStep(path.c_str(), progress, progressContext, &result)) { error = result.error ? result.error : L"The STEP file could not be loaded."; addon.api.releaseResult(&result); return nullptr; }
    const bool valid = result.structSize == sizeof(result) && result.apiVersion == kViewtriousModelImporterApiVersion && result.positions && result.normals && result.indices && result.ranges && result.vertexColors && result.vertexColorFlags && result.hierarchyNodes && result.hierarchyNamesUtf8 && result.positionCount == result.indexCount && result.indexCount % 3 == 0 && result.rangeCount && result.triangleCadFaceIdCount == result.indexCount/3 && result.vertexColorCount == result.positionCount && result.hierarchyNodeCount && result.hierarchyNamesUtf8Count && (!result.hierarchyChildIndexCount || result.hierarchyChildIndices);
    if (!valid) { addon.api.releaseResult(&result); error = L"The optional Viewtrious STEP add-on returned invalid model data."; return nullptr; }
    auto document = std::make_shared<ModelDocument>();
    MeshGeometry mesh; mesh.positions.assign(reinterpret_cast<const Float3*>(result.positions), reinterpret_cast<const Float3*>(result.positions)+result.positionCount); mesh.normals.assign(reinterpret_cast<const Float3*>(result.normals), reinterpret_cast<const Float3*>(result.normals)+result.positionCount); mesh.indices.assign(result.indices, result.indices+result.indexCount);
    bool hasAuthoredColor = false; for (uint32_t i = 0; i < result.vertexColorCount; ++i) if (result.vertexColorFlags[i] & kViewtriousImporterColorAuthored) { hasAuthoredColor = true; break; }
    if (hasAuthoredColor) { mesh.colors.reserve(result.vertexColorCount); for (uint32_t i = 0; i < result.vertexColorCount; ++i) { const auto& color = result.vertexColors[i]; mesh.colors.push_back(result.vertexColorFlags[i] & kViewtriousImporterColorAuthored && std::isfinite(color.x) && std::isfinite(color.y) && std::isfinite(color.z) ? Float3{color.x, color.y, color.z} : Float3{.72f, .75f, .80f}); } }
    document->geometries.push_back(std::move(mesh)); document->instances.push_back({0,Matrix4::Identity()}); document->bounds={{result.bounds.minimum.x,result.bounds.minimum.y,result.bounds.minimum.z},{result.bounds.maximum.x,result.bounds.maximum.y,result.bounds.maximum.z}}; document->sourceFormat=ModelSourceFormat::Step; document->sourceUnit=L"STEP"; document->unitScaleMillimeters=result.metersPerUnit*1000.0; document->metersPerUnit=result.metersPerUnit; document->triangleCadFaceIds.assign(result.triangleCadFaceIds,result.triangleCadFaceIds+result.triangleCadFaceIdCount);
    for(uint32_t i=0;i<result.rangeCount;++i) { const auto& range=result.ranges[i]; document->instanceRanges.push_back({range.sourceObjectId,range.buildItemIndex,range.firstVertex,range.vertexCount,range.firstTriangle,range.triangleCount,{{range.bounds.minimum.x,range.bounds.minimum.y,range.bounds.minimum.z},{range.bounds.maximum.x,range.bounds.maximum.y,range.bounds.maximum.z}}}); }
    StepImportedMetadata metadata;
    metadata.hierarchy.reserve(result.hierarchyNodeCount);
    for (uint32_t i = 0; i < result.hierarchyNodeCount; ++i) {
        const auto& node = result.hierarchyNodes[i];
        if ((node.parentIndex != UINT32_MAX && node.parentIndex >= result.hierarchyNodeCount) || node.firstChildIndex > result.hierarchyChildIndexCount || node.childCount > result.hierarchyChildIndexCount - node.firstChildIndex || (node.firstRangeIndex == UINT32_MAX ? node.rangeCount != 0 : node.firstRangeIndex > result.rangeCount || node.rangeCount > result.rangeCount - node.firstRangeIndex) || node.nameOffset > result.hierarchyNamesUtf8Count || node.nameLength > result.hierarchyNamesUtf8Count - node.nameOffset) { addon.api.releaseResult(&result); error = L"The optional Viewtrious STEP add-on returned invalid hierarchy metadata."; return nullptr; }
        metadata.hierarchy.push_back({node.parentIndex, node.firstChildIndex, node.childCount, node.firstRangeIndex, node.rangeCount, node.flags, Utf8ToWide(result.hierarchyNamesUtf8 + node.nameOffset, node.nameLength)});
    }
    for (uint32_t parent = 0; parent < result.hierarchyNodeCount; ++parent) for (uint32_t childOffset = 0; childOffset < result.hierarchyNodes[parent].childCount; ++childOffset) { const uint32_t child = result.hierarchyChildIndices[result.hierarchyNodes[parent].firstChildIndex + childOffset]; if (child >= result.hierarchyNodeCount || result.hierarchyNodes[child].parentIndex != parent) { addon.api.releaseResult(&result); error = L"The optional Viewtrious STEP add-on returned invalid hierarchy metadata."; return nullptr; } }
    if (result.hierarchyChildIndexCount) metadata.hierarchyChildIndices.assign(result.hierarchyChildIndices, result.hierarchyChildIndices + result.hierarchyChildIndexCount);
    document->stepImportedMetadata = std::move(metadata);
    void* context=result.documentContext; addon.api.releaseResult(&result); document->cadTopology=std::shared_ptr<void>(context,[release=addon.api.releaseDocumentContext](void* value){if(value)release(value);}); BuildSnapPlanes(*document); return document;
}
