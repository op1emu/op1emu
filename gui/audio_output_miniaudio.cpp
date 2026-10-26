#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_output_miniaudio.h"
#include "audio_ring.h"
#include "audio_pcm.h"
#include "utils/log.h"
#include <cstring>
#include <algorithm>

// SPORT's frame rate, the OP-1 codec's; miniaudio resamples for a device
// that does not take it.
static constexpr ma_uint32 SAMPLE_RATE = 44100;
static constexpr ma_uint32 CHANNELS = 2;
// Room for twice the longest ring share (MiniaudioOutput::kMaxBufferMs) and
// the card's requests.
static constexpr ma_uint32 RING_BUFFER_FRAMES = 16384; // ~372ms at 44.1kHz
static_assert(2 * (SAMPLE_RATE * SplitLatencyMs(MiniaudioOutput::kMaxBufferMs).ringMs / 1000) + 4096 <= RING_BUFFER_FRAMES,
              "the ring holds twice the target and a request");
// Up to 0.5% (9 cents) faster or slower for the level's error, and up to
// 0.5% more for the clocks' drift; the level is followed over ~0.25s and its
// error summed over ~15s.
static constexpr double GAIN = 0.005;
static constexpr double MAX_DRIFT = 0.005;
static constexpr double SMOOTH_FRAMES = SAMPLE_RATE / 4;
static constexpr double INTEGRAL_FRAMES = SAMPLE_RATE * 15;

MiniaudioOutput::MiniaudioOutput(unsigned bufferMs)
    : device_(nullptr), ringBuffer_(nullptr),
      latency_(SplitLatencyMs(std::clamp(bufferMs, kMinBufferMs, kMaxBufferMs))),
      targetFrames_(SAMPLE_RATE * latency_.ringMs / 1000), gate_(targetFrames_),
      servo_(targetFrames_, GAIN, MAX_DRIFT, SMOOTH_FRAMES, INTEGRAL_FRAMES) {

    auto* rb = new ma_pcm_rb;
    auto* dev = new ma_device;

    // Initialize the ring buffer (float32, stereo)
    ma_result result = ma_pcm_rb_init(ma_format_f32, CHANNELS, RING_BUFFER_FRAMES, nullptr, nullptr, rb);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio ring buffer: %d", result);
        delete rb;
        delete dev;
        return;
    }

    // Configure the playback device
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate = SAMPLE_RATE;
    // The card's share. miniaudio asks PulseAudio for a period of it in all
    // (ADJUST_LATENCY), which gives a quarter to the sink and three quarters
    // to the stream's queue (its tlength): what the callback thread, which
    // is not real-time, has to refill it in time. Measured on a desktop: at
    // 5.8ms (4.4ms queued) it was late about once a minute, the card playing
    // silence the ring never saw; at 8.7ms not in three.
    config.periodSizeInMilliseconds = latency_.cardMs;
    // The callback takes what PulseAudio asks for, not miniaudio's periods
    // cut from it: those keep what is left of the last one, up to a period
    // more of latency. (Measured: no more underruns.) The gate's target
    // grows with the requests.
    config.noFixedSizedCallback = MA_TRUE;
    config.dataCallback = reinterpret_cast<ma_device_data_proc>(DataCallback);
    config.pUserData = this;

    result = ma_device_init(nullptr, &config, dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio device: %d", result);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        return;
    }

    ringBuffer_ = rb;
    result = ma_device_start(dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to start audio device: %d", result);
        ma_device_uninit(dev);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        ringBuffer_ = nullptr;
        return;
    }

    device_ = dev;
    active_.store(true);
    LogInfo("Audio output initialized: %dHz, %dch, period=%u frames x %u",
            SAMPLE_RATE, CHANNELS, dev->playback.internalPeriodSizeInFrames, dev->playback.internalPeriods);
}

MiniaudioOutput::~MiniaudioOutput() {
    if (device_) {
        ma_device_uninit(static_cast<ma_device*>(device_));
        delete static_cast<ma_device*>(device_);
    }
    if (ringBuffer_) {
        ma_pcm_rb_uninit(static_cast<ma_pcm_rb*>(ringBuffer_));
        delete static_cast<ma_pcm_rb*>(ringBuffer_);
    }
}

void MiniaudioOutput::DataCallback(void* pDevice, void* pOutput, const void* pInput, unsigned int frameCount) {
    (void)pInput;

    ma_device* dev = static_cast<ma_device*>(pDevice);
    auto* self = static_cast<MiniaudioOutput*>(dev->pUserData);
    ma_pcm_rb* rb = static_cast<ma_pcm_rb*>(self->ringBuffer_);
    float* output = static_cast<float*>(pOutput);

    if (frameCount > self->request_.load(std::memory_order_relaxed))
        self->request_.store(frameCount, std::memory_order_relaxed);
    const PlaybackGate::Frames frames = self->gate_.Take(ma_pcm_rb_available_read(rb), frameCount);
    if (frames.skip) ma_pcm_rb_seek_read(rb, frames.skip);
    ma_uint32 framesRemaining = frames.play;
    ma_uint32 framesWritten = 0;

    while (framesRemaining > 0) {
        void* readBuffer;
        ma_uint32 framesToRead = framesRemaining;
        ma_result result = ma_pcm_rb_acquire_read(rb, &framesToRead, &readBuffer);
        if (result != MA_SUCCESS || framesToRead == 0) {
            break;
        }

        memcpy(output + framesWritten * CHANNELS, readBuffer, framesToRead * CHANNELS * sizeof(float));
        ma_pcm_rb_commit_read(rb, framesToRead);

        framesWritten += framesToRead;
        framesRemaining -= framesToRead;
    }

    // Silence for what the gate held back or the ring did not have
    if (framesWritten < frameCount) {
        memset(output + framesWritten * CHANNELS, 0, (frameCount - framesWritten) * CHANNELS * sizeof(float));
    }
}

void MiniaudioOutput::WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) {
    if (!active_.load() || samples == 0 || channels <= 0) {
        return;
    }

    ma_pcm_rb* rb = static_cast<ma_pcm_rb*>(ringBuffer_);

    // Convert input PCM to float32 stereo and push to ring buffer.
    // Process in chunks to avoid large stack allocations.
    constexpr size_t CHUNK_FRAMES = 256;
    float chunk[CHUNK_FRAMES * CHANNELS];

    const uint8_t* src = static_cast<const uint8_t*>(data);
    const size_t srcStride = size_t(bitsPerSample > 16 ? 4 : 2) * channels;

    size_t framesRemaining = samples;
    while (framesRemaining > 0) {
        size_t framesToProcess = std::min(framesRemaining, CHUNK_FRAMES);
        PcmToStereo(src, framesToProcess, channels, bitsPerSample, stereo, chunk);

        // To the ring at the rate that keeps its level (audio_stream.h)
        servo_.SetTarget(TargetLevel(targetFrames_, request_.load(std::memory_order_relaxed)));
        const double ratio = servo_.Update(ma_pcm_rb_available_read(rb), framesToProcess);
        resampled_.clear();
        resampler_.Push(chunk, framesToProcess, ratio, resampled_);

        // A write may stop at the physical end even when the ring has free
        // space at the start. Continue there; drop only when the ring is full.
        WriteAudioFrames(*rb, resampled_.data(), static_cast<ma_uint32>(resampled_.size() / CHANNELS), CHANNELS);

        framesRemaining -= framesToProcess;
        src += framesToProcess * srcStride;
    }
}
