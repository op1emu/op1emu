#include "audio_pcm.h"
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static bool Near(float a, float b) { return std::fabs(a - b) < 1e-6f; }

int main() {
    {
        // The OP-1's codec frames: a primary and a secondary 32-bit word, the
        // whole word the sample, no stereo frame sync. The secondary (silent
        // in all the firmware plays) is no right channel: the primary plays
        // on both.
        const int32_t words[] = {INT32_MIN / 2, 0, 0x40000000 / 4, 123};
        float out[4];
        PcmToStereo(words, 2, 2, 32, false, out);
        Require(Near(out[0], -0.5f) && Near(out[1], -0.5f), "a 32-bit word is a sample of full scale 2^31");
        Require(Near(out[2], 0.125f) && Near(out[3], 0.125f), "only the primary word is heard");
    }
    {
        // A 24-bit word is right-justified in its 32 bits, sign-extended.
        const uint32_t words[] = {0x00C00000u, 0x00400000u};
        float out[4];
        PcmToStereo(words, 2, 1, 24, false, out);
        Require(Near(out[0], -0.5f) && Near(out[1], -0.5f), "a 24-bit word's sign is its bit 23");
        Require(Near(out[2], 0.5f) && Near(out[3], 0.5f), "and its full scale 2^23");
        // 20 bits: the sign is bit 19, whatever the slot's top bits hold.
        const uint32_t twenty[] = {0xFFF80000u, 0x00040000u};
        PcmToStereo(twenty, 2, 1, 20, false, out);
        Require(Near(out[0], -1.0f) && Near(out[2], 0.5f), "a 20-bit word's sign is its bit 19, its full scale 2^19");
        const int32_t extreme[] = {INT32_MIN, INT32_MAX};
        PcmToStereo(extreme, 2, 1, 32, false, out);
        Require(Near(out[0], -1.0f) && Near(out[2], 1.0f), "a 32-bit word's extremes are full scale");
    }
    {
        // 16-bit words; with a stereo frame sync a frame is left, right.
        const int16_t words[] = {-16384, 8192};
        float out[2];
        PcmToStereo(words, 1, 2, 16, true, out);
        Require(Near(out[0], -0.5f) && Near(out[1], 0.25f), "a 16-bit stereo frame is left, right");
        const int16_t mono[] = {16384};
        PcmToStereo(mono, 1, 1, 16, false, out);
        Require(Near(out[0], 0.5f) && Near(out[1], 0.5f), "a mono frame plays on both");
        // Stereo with the secondary channel: each half of the frame sends a
        // primary then a secondary word, so the right primary is word 2.
        const int16_t four[] = {16384, 1, -16384, 2};
        PcmToStereo(four, 1, 4, 16, true, out);
        Require(Near(out[0], 0.5f) && Near(out[1], -0.5f), "a stereo frame with secondary words is pL, sL, pR, sR");
    }
    std::puts("audio-pcm-test: ok");
    return 0;
}
