#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// A SPORT frame's words (SPORT::AudioOutputCallback) as one float stereo
// frame each, into out (frames * 2 floats). With a stereo frame sync the
// frame is its left half then its right, each a primary word and, with the
// secondary channel, a secondary one (BF52x HRM: primary first, then
// secondary): the primary words are left and right. Without, the primary
// word plays on both sides and the secondary's is not heard. A word of up to 16
// bits is a 16-bit sample; one of 17 to 32 is right-justified in a 32-bit
// slot (all of it at 32 bits).
inline void PcmToStereo(const void* data, size_t frames, int words, int bits, bool stereo, float* out) {
    const auto* src = static_cast<const uint8_t*>(data);
    const size_t wordSize = bits > 16 ? 4 : 2;
    const size_t stride = wordSize * size_t(words);
    const int shift = bits > 16 ? 32 - bits : 0;
    const float scale = bits > 16 ? 1.0f / float(1ull << (bits - 1)) : 1.0f / 32768.0f;
    auto sample = [&](const uint8_t* p) {
        if (wordSize == 2) {
            int16_t v;
            std::memcpy(&v, p, sizeof v);
            return float(v) * scale;
        }
        uint32_t v;
        std::memcpy(&v, p, sizeof v);
        return float(int32_t(v << shift) >> shift) * scale;
    };
    for (size_t i = 0; i < frames; ++i) {
        const uint8_t* frame = src + i * stride;
        out[2 * i] = sample(frame);
        out[2 * i + 1] = stereo && words >= 2 ? sample(frame + wordSize * size_t(words / 2)) : out[2 * i];
    }
}
