#pragma once

#include "peripheral/audio_output.h"
#include <atomic>

class MiniaudioOutput : public AudioOutput {
public:
    MiniaudioOutput();
    ~MiniaudioOutput() override;

    void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample) override;

private:
    static void DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount);

    void* device_;      // ma_device*
    void* ringBuffer_;  // ma_pcm_rb*
    std::atomic<bool> active_{false};
};
