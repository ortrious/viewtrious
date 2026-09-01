#pragma once

#include <cstddef>
#include <cstdint>

constexpr uint32_t kViewtriousModelImporterApiVersion = 1;

struct ViewtriousImporterFloat3 { float x; float y; float z; };
struct ViewtriousImporterBounds { ViewtriousImporterFloat3 minimum; ViewtriousImporterFloat3 maximum; };
struct ViewtriousImporterRange {
    uint32_t sourceObjectId, buildItemIndex, firstVertex, vertexCount, firstTriangle, triangleCount;
    ViewtriousImporterBounds bounds;
};
struct ViewtriousImporterResult {
    uint32_t structSize;
    uint32_t apiVersion;
    const ViewtriousImporterFloat3* positions;
    const ViewtriousImporterFloat3* normals;
    const uint32_t* indices;
    const ViewtriousImporterRange* ranges;
    const uint32_t* triangleCadFaceIds;
    uint32_t positionCount, indexCount, rangeCount, triangleCadFaceIdCount;
    ViewtriousImporterBounds bounds;
    double metersPerUnit;
    void* resultOwner;
    void* documentContext;
    const wchar_t* error;
};
struct ViewtriousModelImporterApi {
    uint32_t structSize;
    uint32_t apiVersion;
    bool (*loadStep)(const wchar_t* path, ViewtriousImporterResult* result);
    void (*releaseResult)(ViewtriousImporterResult* result);
    void (*releaseDocumentContext)(void* context);
};

extern "C" __declspec(dllexport) bool ViewtriousModelImporter_GetApi(uint32_t requestedVersion, ViewtriousModelImporterApi* api);
