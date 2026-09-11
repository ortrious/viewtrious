#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t kViewtriousModelImporterApiVersion = 3;

using ViewtriousImporterProgressCallback = void (*)(void* context, float normalizedProgress);

struct ViewtriousImporterFloat3 { float x; float y; float z; };
struct ViewtriousImporterFloat4 { float x; float y; float z; float w; };
struct ViewtriousImporterBounds { ViewtriousImporterFloat3 minimum; ViewtriousImporterFloat3 maximum; };
struct ViewtriousImporterRange {
    uint32_t sourceObjectId, buildItemIndex, firstVertex, vertexCount, firstTriangle, triangleCount;
    ViewtriousImporterBounds bounds;
};
// UTF-8 names are slices of ViewtriousImporterResult::hierarchyNamesUtf8.
// Child spans refer to ViewtriousImporterResult::hierarchyChildIndices; range spans
// refer to ViewtriousImporterResult::ranges. UINT32_MAX means no parent/range.
struct ViewtriousImporterHierarchyNode {
    uint32_t parentIndex, firstChildIndex, childCount;
    uint32_t firstRangeIndex, rangeCount;
    uint32_t nameOffset, nameLength;
    uint32_t flags;
};
constexpr uint32_t kViewtriousImporterHierarchyAssembly = 1u << 0;
constexpr uint32_t kViewtriousImporterHierarchyRenderable = 1u << 1;
constexpr uint8_t kViewtriousImporterColorAuthored = 1u << 0;
struct ViewtriousImporterResult {
    uint32_t structSize;
    uint32_t apiVersion;
    const ViewtriousImporterFloat3* positions;
    const ViewtriousImporterFloat3* normals;
    const ViewtriousImporterFloat4* vertexColors;
    const uint8_t* vertexColorFlags;
    const uint32_t* indices;
    const ViewtriousImporterRange* ranges;
    const uint32_t* triangleCadFaceIds;
    const ViewtriousImporterHierarchyNode* hierarchyNodes;
    const uint32_t* hierarchyChildIndices;
    const char* hierarchyNamesUtf8;
    uint32_t positionCount, indexCount, rangeCount, triangleCadFaceIdCount;
    uint32_t vertexColorCount, hierarchyNodeCount, hierarchyChildIndexCount, hierarchyNamesUtf8Count;
    ViewtriousImporterBounds bounds;
    double metersPerUnit;
    void* resultOwner;
    void* documentContext;
    const wchar_t* error;
};
struct ViewtriousModelImporterApi {
    uint32_t structSize;
    uint32_t apiVersion;
    bool (*loadStep)(const wchar_t* path, ViewtriousImporterProgressCallback progress, void* progressContext, ViewtriousImporterResult* result);
    void (*releaseResult)(ViewtriousImporterResult* result);
    void (*releaseDocumentContext)(void* context);
};

extern "C" __declspec(dllexport) bool ViewtriousModelImporter_GetApi(uint32_t requestedVersion, ViewtriousModelImporterApi* api);
