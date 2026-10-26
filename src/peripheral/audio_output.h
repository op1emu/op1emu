#pragma once

#include <cstddef>
#include <cstdint>

class AudioOutput {
public:
    virtual ~AudioOutput() = default;

    // Called from CPU thread (via SPORT DMA callback).
    // data: raw PCM frames from the SPORT, as SPORT::AudioOutputCallback
    // samples: number of frames
    // channels: words per frame (primary, then secondary if on; for the left
    //           and then the right half with stereo)
    // bitsPerSample: the word length; up to 16 a word is 16 bits, above it is
    //                right-justified in 32
    // stereo: the frame's halves are left and right
    virtual void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) = 0;

    // Called from the CPU thread (BlackFinCpu::Pace), at wallNs on the
    // steady clock: guest ns per wall ns from then on (RealtimePacer::SetRate),
    // so that the frames come as fast as the card plays them.
    virtual double GuestRate(uint64_t wallNs) {
        (void)wallNs;
        return 1.0;
    }
};
