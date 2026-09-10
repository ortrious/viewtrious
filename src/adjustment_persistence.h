#pragma once

#include "media_adjustments.h"

#include <array>
#include <cstdint>
#include <functional>
#include <string>

enum class AdjustmentMediaKind : unsigned char { Image, Video };

struct ImageAdjustmentPersistenceResult {
    AdjustmentMediaKind mediaKind = AdjustmentMediaKind::Image;
    std::wstring path;
    uint64_t mediaGeneration = 0;
    uint64_t editGeneration = 0;
    std::array<unsigned char, 32> hash{};
    bool hashResolved = false;
    bool hasAdjustments = false;
    ImageAdjustments adjustments{};
};

// A single worker owns both the optional sqlite3 runtime and its connection.
// Results are delivered from that worker; callers must marshal them to their UI.
class ImageAdjustmentPersistence {
public:
    ImageAdjustmentPersistence();
    ~ImageAdjustmentPersistence();
    ImageAdjustmentPersistence(const ImageAdjustmentPersistence&) = delete;
    ImageAdjustmentPersistence& operator=(const ImageAdjustmentPersistence&) = delete;

    void Start(std::function<void(ImageAdjustmentPersistenceResult&&)> completion);
    void Resolve(const std::wstring& path, uint64_t mediaGeneration, uint64_t editGeneration, AdjustmentMediaKind mediaKind = AdjustmentMediaKind::Image);
    void Save(const std::array<unsigned char, 32>& hash, const ImageAdjustments& adjustments, AdjustmentMediaKind mediaKind = AdjustmentMediaKind::Image);
    void Shutdown();

private:
    struct Impl;
    Impl* impl_ = nullptr;
};
