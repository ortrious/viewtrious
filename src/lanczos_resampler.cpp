#include "lanczos_resampler.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>

namespace viewtrious {
namespace {

constexpr int kCoefficientShift = 14;
constexpr int kCoefficientScale = 1 << kCoefficientShift;
constexpr float kLanczosRadius = 3.0f;

float Sinc(float x) {
    if (std::abs(x) < 1.0e-6f) return 1.0f;
    const float piX = 3.14159265358979323846f * x;
    return std::sin(piX) / piX;
}

float Lanczos3(float x) {
    x = std::abs(x);
    return x < kLanczosRadius ? Sinc(x) * Sinc(x / kLanczosRadius) : 0.0f;
}

} // namespace

struct Lanczos3Scaler::CoefficientTable {
    struct Entry { uint32_t first = 0; std::vector<int16_t> weights; };
    std::vector<Entry> entries;
    uint32_t maxTaps = 0;
};

struct Lanczos3Scaler::Row { uint32_t sourceY = UINT32_MAX; std::vector<int32_t> values; };

Lanczos3Scaler::CoefficientTable Lanczos3Scaler::BuildCoefficientTable(uint32_t sourceLength, uint32_t destinationLength,
    float sourceOffset, float destinationOffset, float sourcePixelsPerDestination) {
    Lanczos3Scaler::CoefficientTable table;
    table.entries.reserve(destinationLength);
    const float scale = sourcePixelsPerDestination > 0.0f ? sourcePixelsPerDestination : static_cast<float>(sourceLength) / destinationLength;
    const float filterScale = std::max(1.0f, scale);
    const float radius = kLanczosRadius * filterScale;
    for (uint32_t destination = 0; destination < destinationLength; ++destination) {
        const float center = (destinationOffset + static_cast<float>(destination) + 0.5f) * scale - 0.5f - sourceOffset;
        const int start = std::max(0, static_cast<int>(std::ceil(center - radius)));
        const int end = std::min(static_cast<int>(sourceLength) - 1, static_cast<int>(std::floor(center + radius)));
        Lanczos3Scaler::CoefficientTable::Entry entry;
        entry.first = static_cast<uint32_t>(start);
        std::vector<float> floatingWeights;
        floatingWeights.reserve(static_cast<size_t>(end - start + 1));
        float sum = 0.0f;
        for (int source = start; source <= end; ++source) {
            const float weight = Lanczos3((center - source) / filterScale);
            floatingWeights.push_back(weight);
            sum += weight;
        }
        entry.weights.resize(floatingWeights.size());
        int normalizedSum = 0;
        size_t correctionIndex = 0;
        float greatestWeight = -std::numeric_limits<float>::infinity();
        for (size_t index = 0; index < floatingWeights.size(); ++index) {
            if (floatingWeights[index] > greatestWeight) { greatestWeight = floatingWeights[index]; correctionIndex = index; }
            const int value = static_cast<int>(std::lround(floatingWeights[index] * kCoefficientScale / sum));
            entry.weights[index] = static_cast<int16_t>(value);
            normalizedSum += value;
        }
        entry.weights[correctionIndex] = static_cast<int16_t>(entry.weights[correctionIndex] + kCoefficientScale - normalizedSum);
        table.maxTaps = std::max(table.maxTaps, static_cast<uint32_t>(entry.weights.size()));
        table.entries.push_back(std::move(entry));
    }
    return table;
}

bool Lanczos3Scaler::Initialize(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t destinationWidth, uint32_t destinationHeight,
    const LanczosMapping& mapping) {
    if (!sourceWidth || !sourceHeight || !destinationWidth || !destinationHeight) return false;
    sourceWidth_ = sourceWidth; sourceHeight_ = sourceHeight;
    destinationWidth_ = destinationWidth; destinationHeight_ = destinationHeight;
    delete horizontal_; delete vertical_; delete rowCache_;
    horizontal_ = new CoefficientTable(BuildCoefficientTable(sourceWidth, destinationWidth, mapping.sourceOffsetX,
        mapping.destinationOffsetX, mapping.sourcePixelsPerDestinationX));
    vertical_ = new CoefficientTable(BuildCoefficientTable(sourceHeight, destinationHeight, mapping.sourceOffsetY,
        mapping.destinationOffsetY, mapping.sourcePixelsPerDestinationY));
    rowCache_ = new std::vector<Row>(vertical_->maxTaps);
    for (Row& row : *rowCache_) row.values.resize(static_cast<size_t>(destinationWidth_) * 4);
    nextRow_ = 0;
    return true;
}

Lanczos3Scaler::~Lanczos3Scaler() {
    delete horizontal_;
    delete vertical_;
    delete rowCache_;
}

bool Lanczos3Scaler::Scale(const uint8_t* source, uint32_t sourceStride, std::vector<uint8_t>& destination) {
    if (!source || !horizontal_ || !vertical_ || !rowCache_ || sourceStride < sourceWidth_ * 4) return false;
    destination.resize(static_cast<size_t>(destinationWidth_) * destinationHeight_ * 4);
    for (uint32_t y = 0; y < destinationHeight_; ++y) {
        const CoefficientTable::Entry& vertical = vertical_->entries[y];
        for (size_t tap = 0; tap < vertical.weights.size(); ++tap)
            EnsureHorizontalRow(source, sourceStride, vertical.first + static_cast<uint32_t>(tap), vertical.first, static_cast<uint32_t>(vertical.weights.size()));
        uint8_t* output = destination.data() + static_cast<size_t>(y) * destinationWidth_ * 4;
        for (uint32_t x = 0; x < destinationWidth_; ++x) {
            std::array<int64_t, 4> sums{};
            for (size_t tap = 0; tap < vertical.weights.size(); ++tap) {
                const int32_t* input = CachedRow(vertical.first + static_cast<uint32_t>(tap)) + static_cast<size_t>(x) * 4;
                for (uint32_t channel = 0; channel < 4; ++channel) sums[channel] += static_cast<int64_t>(input[channel]) * vertical.weights[tap];
            }
            const int alpha = ClampToByte(static_cast<int>((sums[3] + (1LL << (2 * kCoefficientShift - 1))) >> (2 * kCoefficientShift)));
            for (uint32_t channel = 0; channel < 3; ++channel) output[x * 4 + channel] = static_cast<uint8_t>(std::min(alpha, ClampToByte(
                static_cast<int>((sums[channel] + (1LL << (2 * kCoefficientShift - 1))) >> (2 * kCoefficientShift)) )));
            output[x * 4 + 3] = static_cast<uint8_t>(alpha);
        }
    }
    return true;
}

size_t Lanczos3Scaler::WorkingBytes() const {
    return horizontal_ && vertical_ && rowCache_ ? TableBytes(*horizontal_) + TableBytes(*vertical_) + rowCache_->size() * sizeof(Row) +
        rowCache_->size() * static_cast<size_t>(destinationWidth_) * 4 * sizeof(int32_t) : 0;
}

int Lanczos3Scaler::ClampToByte(int value) { return std::clamp(value, 0, 255); }

size_t Lanczos3Scaler::TableBytes(const CoefficientTable& table) {
    size_t bytes = table.entries.capacity() * sizeof(CoefficientTable::Entry);
    for (const CoefficientTable::Entry& entry : table.entries) bytes += entry.weights.capacity() * sizeof(int16_t);
    return bytes;
}

void Lanczos3Scaler::EnsureHorizontalRow(const uint8_t* source, uint32_t sourceStride, uint32_t sourceY, uint32_t protectedFirst, uint32_t protectedCount) {
    if (CachedRow(sourceY)) return;
    Row* destination = nullptr;
    const uint32_t protectedLast = protectedFirst + protectedCount;
    for (size_t attempt = 0; attempt < rowCache_->size(); ++attempt) {
        Row& candidate = (*rowCache_)[(nextRow_ + attempt) % rowCache_->size()];
        if (candidate.sourceY < protectedFirst || candidate.sourceY >= protectedLast) {
            destination = &candidate; nextRow_ = (nextRow_ + attempt + 1) % rowCache_->size(); break;
        }
    }
    if (!destination) return;
    const uint8_t* input = source + static_cast<size_t>(sourceY) * sourceStride;
    for (uint32_t x = 0; x < destinationWidth_; ++x) {
        const CoefficientTable::Entry& horizontal = horizontal_->entries[x];
        for (uint32_t channel = 0; channel < 4; ++channel) {
            int32_t sum = 0;
            for (size_t tap = 0; tap < horizontal.weights.size(); ++tap) sum += input[(static_cast<size_t>(horizontal.first) + tap) * 4 + channel] * horizontal.weights[tap];
            destination->values[static_cast<size_t>(x) * 4 + channel] = sum;
        }
    }
    destination->sourceY = sourceY;
}

int32_t* Lanczos3Scaler::CachedRow(uint32_t sourceY) {
    for (Row& row : *rowCache_) if (row.sourceY == sourceY) return row.values.data();
    return nullptr;
}

} // namespace viewtrious
