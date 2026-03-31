#pragma once

#include <cstddef>
#include <cstdint>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Called from CPU thread (via SPORT DMA callback).
    // data: raw PCM samples from the SPORT
    // samples: number of sample frames
    // channels: 1 (mono) or 2 (stereo)
    // bitsPerSample: typically 16 or 24
    virtual void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample) = 0;
};
