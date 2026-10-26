#pragma once

#include "peripheral/audio_output.h"
#include "audio_stream.h"
#include <atomic>
#include <vector>

class MiniaudioOutput : public AudioOutput {
public:
    // The audio kept ahead of the card (audio_stream.h's target), against the
    // CPU thread running late (it compiles blocks: 8-25ms); more is heard
    // later.
    static constexpr unsigned kDefaultBufferMs = 17;
    static constexpr unsigned kMaxBufferMs = 150;

    explicit MiniaudioOutput(unsigned bufferMs = kDefaultBufferMs);
    ~MiniaudioOutput() override;

    void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) override;

private:
    static void DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount);

    void* device_;      // ma_device*
    void* ringBuffer_;  // ma_pcm_rb*
    std::atomic<bool> active_{false};
    uint32_t targetFrames_;
    // The card's data callback only.
    PlaybackGate gate_;
    // Its largest request, for the CPU thread's target level.
    std::atomic<uint32_t> request_{0};
    // The CPU thread only.
    LevelServo servo_;
    StreamResampler resampler_;
    std::vector<float> resampled_;
};
