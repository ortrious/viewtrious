#pragma once

#include <cstdint>
#include <optional>
#include <vector>

struct Float3 { float x = 0, y = 0, z = 0; };

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
    std::vector<uint32_t> indices;
};

struct MeshInstance {
    uint32_t geometryIndex = 0;
    Matrix4 transform = Matrix4::Identity();
};

struct ModelDocument {
    std::vector<MeshGeometry> geometries;
    std::vector<MeshInstance> instances;
    ModelBounds bounds{};
    std::optional<double> metersPerUnit; // STL does not define units.
};
