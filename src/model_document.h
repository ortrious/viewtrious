#pragma once

#include <cstdint>
#include <optional>
#include <memory>
#include <string>
#include <vector>

struct Float3 { float x = 0, y = 0, z = 0; };
enum class ModelVisualStyle : uint32_t { Shaded = 0, ShadedWithVisibleEdges = 1, Wireframe = 2 };
enum class ModelAntiAliasing : uint32_t { Off = 0, Msaa2x = 1, Msaa4x = 2, Msaa8x = 3, Ssaa1_5x = 4, Ssaa2x = 5 };
constexpr bool IsModelAntiAliasingSsaa(ModelAntiAliasing mode) { return mode == ModelAntiAliasing::Ssaa1_5x || mode == ModelAntiAliasing::Ssaa2x; }
constexpr ModelAntiAliasing EffectiveModelAntiAliasing(ModelAntiAliasing configured, ModelVisualStyle visualStyle) {
    return visualStyle == ModelVisualStyle::Wireframe && IsModelAntiAliasingSsaa(configured) ? ModelAntiAliasing::Msaa8x : configured;
}
enum class ModelUpAxis : uint32_t { XUp = 0, YUp = 1, ZUp = 2 };
enum class ModelBuildPlate : uint32_t { Auto = 0, On = 1, Off = 2 };

struct Matrix4 {
    float m[16]{};
    static Matrix4 Identity() {
        Matrix4 result{};
        result.m[0] = result.m[5] = result.m[10] = result.m[15] = 1.0f;
        return result;
    }
};

struct ModelBounds {
    Float3 minimum{};
    Float3 maximum{};
};

struct MeshGeometry {
    std::vector<Float3> positions;
    std::vector<Float3> normals;
    std::vector<Float3> colors;
    std::vector<uint32_t> indices;
};

struct MeshInstance {
    uint32_t geometryIndex = 0;
    Matrix4 transform = Matrix4::Identity();
};

struct SnapPlane {
    Float3 normal{};
    float offset = 0.0f;
    std::vector<uint32_t> triangles;
};

enum class ModelSourceFormat : uint32_t { Stl, ThreeMf };

struct ModelInstanceRange {
    uint32_t sourceObjectId = 0;
    uint32_t buildItemIndex = 0;
    uint32_t firstVertex = 0;
    uint32_t vertexCount = 0;
    uint32_t firstTriangle = 0;
    uint32_t triangleCount = 0;
    ModelBounds bounds{};
};

struct ModelComponentNode {
    uint32_t id = 0;
    uint32_t parentId = UINT32_MAX;
    std::vector<uint32_t> children;
    std::wstring name;
    int32_t instanceRange = -1;
    uint32_t sourceObjectId = 0;
};

struct ModelDocument {
    std::vector<MeshGeometry> geometries;
    std::vector<MeshInstance> instances;
    std::vector<SnapPlane> snapPlanes;
    std::vector<uint32_t> triangleSnapPlanes;
    ModelBounds bounds{};
    ModelSourceFormat sourceFormat = ModelSourceFormat::Stl;
    std::wstring sourceUnit = L"unspecified";
    double unitScaleMillimeters = 1.0;
    std::vector<ModelInstanceRange> instanceRanges;
    std::vector<ModelComponentNode> componentTree;
    std::optional<double> metersPerUnit; // STL does not define units.
};
