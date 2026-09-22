#pragma once
#include <windows.h>
#include <atomic>
#include <memory>
#include <string>
#include <vector>

struct VideoHoverPreviewRequest { std::wstring path; uint64_t generation = 0; UINT maximumDimension = 768; bool earliestFrame = false; };
struct VideoHoverPreviewFrame { std::shared_ptr<std::vector<BYTE>> pixels; UINT width = 0, height = 0, stride = 0; LONGLONG timestamp = 0; };
class VideoHoverFrameStream {
public:
    VideoHoverFrameStream(); ~VideoHoverFrameStream();
    HRESULT Open(const VideoHoverPreviewRequest& request, const std::atomic<uint64_t>* currentGeneration);
    HRESULT ReadNext(VideoHoverPreviewFrame& frame);
    void Close();
    double DurationSeconds() const { return durationSeconds_; } double StartSeconds() const { return startSeconds_; }
private:
    struct Impl; std::unique_ptr<Impl> impl_; const std::atomic<uint64_t>* generation_ = nullptr; uint64_t requestGeneration_ = 0; LONGLONG requestedStartTimestamp_ = 0; double durationSeconds_ = 0, startSeconds_ = 0;
};
