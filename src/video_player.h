#pragma once

#include <windows.h>
#include <d2d1_1.h>
#include <d3d11.h>
#include <d3d10_1.h>
#include <AudioSessionTypes.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfmediaengine.h>
#include <wrl/client.h>

#include "media_adjustments.h"

#include <array>
#include <string>
#include <vector>

// Small Media Foundation wrapper which leaves final composition to GraphicsHost.
class VideoPlayer {
public:
    enum class FrameAcquisitionReason : unsigned char { Scheduler, InitialLoad, Seek };

    bool Open(HWND window, ID3D11Device* device, const std::wstring& path, std::wstring& error);
    void Shutdown();
    bool RebindDevice(ID3D11Device* device, std::wstring& error);
    void HandleRenderTargetResize();
    bool HandleMediaEvent(DWORD event, std::wstring& error);
    bool UpdateFrame(FrameAcquisitionReason reason);
    bool Draw(ID2D1DeviceContext* context, const RECT& canvas, float scale, D2D1_POINT_2F pan);
    HRESULT TogglePlayPause();
    bool GetPlaybackTimes(double& currentSeconds, double& durationSeconds) const;
    bool Seek(double seconds);
    bool ToggleMute();
    bool Muted() const;
    bool GetNativeVideoSize(DWORD& width, DWORD& height) const;
    bool TryGetFramesPerSecond(float& framesPerSecond);
    void SetDisplayAdjustments(const MediaAdjustments& adjustments);
    bool CopyCurrentFrameBgra(std::vector<unsigned char>& pixels, UINT& width, UINT& height) const;
    bool SetPreferredPlaybackRate(double rate);
    bool PlaybackRateSupported(double rate) const;
    double EffectivePlaybackRate() const { return effectivePlaybackRate_; }
    void RecordFramePacingSchedule(double intervalMs, LONGLONG deadlineQpc);
    void RecordFramePacingTimer(LONGLONG wakeQpc, LONGLONG deadlineQpc);
    void RecordFramePacingPaint();
    void RecordFramePacingPresent(HRESULT result);
    void FlushFramePacingDiagnostics();
    bool Playing() const { return playing_; }
    bool Ended() const { return engine_ && engine_->IsEnded(); }
    bool Active() const { return engine_ != nullptr; }
    bool Failed() const { return failed_; }
    bool HasValidFrame() const { return hasValidFrame_; }

private:
    bool CreateFrameTexture(std::wstring& error);
    bool ReadNominalFrameRate(const std::wstring& path);
    bool SetSourceFromPath(const std::wstring& path, std::wstring& error);
    bool EnsureMultithreadProtection(ID3D11Device* device, std::wstring& error);
    bool ApplyPreferredPlaybackRate();
    enum class FramePacingEvent : unsigned char { PlaybackBegin, PlaybackPause, PlaybackResume, PlaybackSeek, PlaybackEnd, Schedule, Timer, SchedulerAcquire, InitialLoadAcquire, SeekAcquire, StreamTick, Transfer, CachePublish, Paint, Present, Count };
    struct FramePacingRecord { LONGLONG qpc = 0; LONGLONG pts = 0; HRESULT result = S_OK; FramePacingEvent event = FramePacingEvent::PlaybackBegin; double first = 0.0; double second = 0.0; };
    void ResetFramePacingDiagnostics();
    void RecordFramePacingEvent(FramePacingEvent event, LONGLONG pts = 0, HRESULT result = S_OK, double first = 0.0, double second = 0.0);
    void RecordFramePacingEventAtQpc(FramePacingEvent event, LONGLONG qpc, LONGLONG pts = 0, HRESULT result = S_OK, double first = 0.0, double second = 0.0);

    Microsoft::WRL::ComPtr<IMFDXGIDeviceManager> deviceManager_;
    Microsoft::WRL::ComPtr<IMFMediaEngine> engine_;
    Microsoft::WRL::ComPtr<IMFMediaEngineEx> engineEx_;
    Microsoft::WRL::ComPtr<ID3D11Device> device_;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> frameTexture_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> frameBitmap_;
    Microsoft::WRL::ComPtr<ID2D1Bitmap1> adjustedFrameBitmap_;
    MediaAdjustmentProcessor adjustmentProcessor_;
    MediaAdjustments displayAdjustments_;
    double preferredPlaybackRate_ = 1.0;
    double effectivePlaybackRate_ = 1.0;
    UINT deviceResetToken_ = 0;
    DWORD videoWidth_ = 0;
    DWORD videoHeight_ = 0;
    bool mediaFoundationStarted_ = false;
    bool ready_ = false;
    bool playing_ = false;
    bool failed_ = false;
    bool hasValidFrame_ = false;
    bool adjustedFrameValid_ = false;
    bool hasTransferredPts_ = false;
    bool hasFramesPerSecond_ = false;
    float framesPerSecond_ = 0.0f;
    LONGLONG lastTransferredPts_ = 0;
#if defined(_DEBUG)
    static constexpr size_t kFramePacingRecordCapacity = 8192;
    std::array<FramePacingRecord, kFramePacingRecordCapacity> framePacingRecords_{};
    size_t framePacingRecordStart_ = 0;
    size_t framePacingRecordCount_ = 0;
    LONGLONG framePacingFrequency_ = 0;
    LONGLONG framePacingLastPaintPts_ = 0;
    LONGLONG framePacingLastPresentPts_ = 0;
    bool framePacingHaveLastPaintPts_ = false;
    bool framePacingHaveLastPresentPts_ = false;
#endif
};
