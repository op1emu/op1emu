#pragma once

// The SPORT's frames for the host's AudioWorklet (web/audio-worklet.js), as
// MiniaudioOutput gives them to a sound card (gui/audio_output_miniaudio.cpp):
// stereo float at 44.1 kHz, and the pacer's rate the card's, so that the
// worklet's ring stays at its target (gui/audio_stream.h). Chunks go to the
// host's Module.onAudio(pointer, frames): frames interleaved stereo floats in
// the module's memory, valid until it returns. The worklet reports how many
// it has taken (op1_audio_report), and the ring's level is what was written
// less that, less what it took since at its rate.

#include "audio_pcm.h"
#include "audio_stream.h"
#include "peripheral/audio_output.h"

#include <emscripten.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <vector>

EM_JS(void, op1_js_audio, (const float* frames, int count), {
    if (Module.onAudio) Module.onAudio(frames >>> 0, count);
});

class WebAudioOutput final : public AudioOutput {
public:
    static constexpr uint32_t kSampleRate = 44100;

    // targetMs: the worklet's ring level, which it waits for before it plays
    // (and again after running dry).
    explicit WebAudioOutput(unsigned targetMs)
        : target_(kSampleRate * targetMs / 1000.0),
          servo_(target_, kGain, kMaxPull, kMaxDrift, kSmoothFrames, kIntegralFrames) {}

    double TargetFrames() const { return target_; }

    void WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) override {
        if (samples == 0 || channels <= 0) return;
        const uint64_t now = NowNs();
        lastWriteNs_ = now;
        const size_t at = pending_.size();
        pending_.resize(at + samples * 2);
        PcmToStereo(data, samples, channels, bitsPerSample, stereo, pending_.data() + at);
        written_ += samples;
        if (FollowsCard(playing_, 0, now - reportNs_)) servo_.Update(Level(now), samples);
        if (pending_.size() >= kChunkFrames * 2) Flush();
    }

    double GuestRate(uint64_t wallNs) override {
        return PaceRate(servo_, FollowsCard(playing_, wallNs - std::min(wallNs, lastWriteNs_),
                                            wallNs - std::min(wallNs, reportNs_)));
    }

    // The frames written and not yet handed over, at the end of a slice.
    void Flush() {
        if (pending_.empty()) return;
        op1_js_audio(pending_.data(), int(pending_.size() / 2));
        pending_.clear();
    }

    // The worklet's report: frames taken in all (played, skipped or dropped), and
    // whether it plays (not waiting for its target).
    void Report(double consumed, bool playing) {
        consumed_ = consumed;
        playing_ = playing;
        reportNs_ = NowNs();
    }

private:
    // The servo's constants are MiniaudioOutput's.
    static constexpr double kGain = 0.005, kMaxPull = 0.001, kMaxDrift = 0.001;
    static constexpr double kSmoothFrames = kSampleRate / 4.0, kIntegralFrames = kSampleRate * 15.0;
    // About 12 ms: the worklet gets them a few at a time, not a frame each.
    static constexpr size_t kChunkFrames = 512;

    // The steady clock, as Pace() reads it.
    static uint64_t NowNs() {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count());
    }

    double Level(uint64_t now) const {
        const double taken = consumed_ + (playing_ ? double(now - std::min(now, reportNs_)) * kSampleRate / 1e9 : 0);
        return std::max(0.0, double(written_) - taken);
    }

    double target_;
    LevelServo servo_;
    std::vector<float> pending_;
    uint64_t written_ = 0;
    double consumed_ = 0;
    bool playing_ = false;
    uint64_t lastWriteNs_ = 0, reportNs_ = 0;
};
