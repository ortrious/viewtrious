#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace viewtrious {

struct LanczosMapping {
    float sourceOffsetX = 0.0f, sourceOffsetY = 0.0f;
    float destinationOffsetX = 0.0f, destinationOffsetY = 0.0f;
    float sourcePixelsPerDestinationX = 0.0f, sourcePixelsPerDestinationY = 0.0f;
};

// A reusable 32bppPBGRA Lanczos3 resampler. Input and output pixels are BGRA
// premultiplied-alpha bytes; no color-space conversion is performed.
class Lanczos3Scaler final {
public:
    Lanczos3Scaler() = default;
    ~Lanczos3Scaler();
    Lanczos3Scaler(const Lanczos3Scaler&) = delete;
    Lanczos3Scaler& operator=(const Lanczos3Scaler&) = delete;

    bool Initialize(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t destinationWidth, uint32_t destinationHeight,
        const LanczosMapping& mapping = {});
    bool Scale(const uint8_t* source, uint32_t sourceStride, std::vector<uint8_t>& destination);

    size_t WorkingBytes() const;
    uint32_t DestinationWidth() const { return destinationWidth_; }
    uint32_t DestinationHeight() const { return destinationHeight_; }

private:
    struct CoefficientTable;
    struct Row;

    static int ClampToByte(int value);
    static CoefficientTable BuildCoefficientTable(uint32_t sourceLength, uint32_t destinationLength, float sourceOffset,
        float destinationOffset, float sourcePixelsPerDestination);
    static size_t TableBytes(const CoefficientTable& table);
    void EnsureHorizontalRow(const uint8_t* source, uint32_t sourceStride, uint32_t sourceY,
        uint32_t protectedFirst, uint32_t protectedCount);
    int32_t* CachedRow(uint32_t sourceY);

    uint32_t sourceWidth_ = 0, sourceHeight_ = 0, destinationWidth_ = 0, destinationHeight_ = 0;
    CoefficientTable* horizontal_ = nullptr;
    CoefficientTable* vertical_ = nullptr;
    std::vector<Row>* rowCache_ = nullptr;
    size_t nextRow_ = 0;
};

} // namespace viewtrious
