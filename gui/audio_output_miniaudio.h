#pragma once

#include "peripheral/audio_output.h"
#include "audio_stream.h"
#include <atomic>
#include <chrono>
#include <memory>

struct PulseLatency;

class MiniaudioOutput : public AudioOutput {
public:
    // The latency in all, split between the card and the ring
    // (SplitLatencyMs): the ring's share is kept ahead of the card against
    // the CPU thread running late (it compiles blocks: 8-25ms); more is heard
    // later.
    static constexpr unsigned kDefaultBufferMs = 26;
    static constexpr unsigned kMinBufferMs = 9;
    static constexpr unsigned kMaxBufferMs = 200;

    explicit MiniaudioOutput(unsigned bufferMs = kDefaultBufferMs);
    ~MiniaudioOutput() override;

    void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) override;
    double GuestRate(uint64_t wallNs) override;

private:
    static void DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount);
    void ReportLatency();

    void* device_;      // ma_device*
    void* ringBuffer_;  // ma_pcm_rb*
    std::atomic<bool> active_{false};
    LatencySplit latency_;
    uint32_t targetFrames_;
    // The card's data callback only.
    PlaybackGate gate_;
    // Its largest request, for the CPU thread's target level; whether it
    // plays and when it last asked (steady clock ns), for the pacer's rate.
    std::atomic<uint32_t> request_{0};
    std::atomic<bool> playing_{false};
    std::atomic<uint64_t> lastCallbackNs_{0};
    // PulseAudio's latency measure (the data callback's), else miniaudio's
    // estimate.
    std::unique_ptr<PulseLatency> pulse_;
    uint64_t cardEstimateUs_ = 0;
    // The CPU thread only.
    uint64_t reportedUs_ = 0;
    uint32_t reportedUnderflows_ = 0;
    std::chrono::steady_clock::time_point reportedAt_{};
    LevelServo servo_;
    uint64_t lastWriteNs_ = 0;
};
