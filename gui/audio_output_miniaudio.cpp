// The device thread's real-time priority (ma_thread_priority_realtime) on
// POSIX: low, below the sound server's, as Chromium's audio threads (8);
// miniaudio's own default is the highest.
#define MA_PTHREAD_REALTIME_THREAD_PRIORITY 8
#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_output_miniaudio.h"
#include "audio_ring.h"
#include "audio_pcm.h"
#include "utils/log.h"
#include <cstring>
#include <algorithm>
#include <chrono>
#include <cmath>
#ifdef __linux__
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#endif

// Steady clock ns from then to now, 0 for a then after it.
static uint64_t Since(uint64_t now, uint64_t then) { return now > then ? now - then : 0; }

static uint64_t SteadyNs() {
    return uint64_t(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

// SPORT's frame rate, the OP-1 codec's; miniaudio resamples for a device
// that does not take it.
static constexpr ma_uint32 SAMPLE_RATE = 44100;
static constexpr ma_uint32 CHANNELS = 2;
// Room for twice the longest ring share (MiniaudioOutput::kMaxBufferMs) and
// the card's requests.
static constexpr ma_uint32 RING_BUFFER_FRAMES = 16384; // ~372ms at 44.1kHz
static_assert(2 * TargetLevel(SAMPLE_RATE * SplitLatencyMs(MiniaudioOutput::kMaxBufferMs).ringMs / 1000,
                              kMaxRequestFrames) + 256 <= RING_BUFFER_FRAMES,
              "the ring holds twice the target and a write");
// Guest time up to 0.1% (1.7 cents) slower or faster for the level's error,
// at 0.5% for all of the target, and up to 0.1% more for the clocks' drift
// (a desktop's card: 56 ppm); the level is followed over ~0.25s and its
// error summed over ~15s. (For the longest ring the loop is underdamped,
// the level's own time constant then ~27s; tested, it settles closer than
// with the integral slowed to match.)
static constexpr double INTEGRAL_FRAMES = SAMPLE_RATE * 15;
static constexpr double GAIN = 0.005;
static constexpr double MAX_PULL = 0.001;
static constexpr double MAX_DRIFT = 0.001;
static constexpr double SMOOTH_FRAMES = SAMPLE_RATE / 4;

// PulseAudio's measure of the card's latency (pa_stream_get_latency: the
// stream's queue and the sink's), which miniaudio does not report, and its
// count of the card running dry. miniaudio runs the stream on a pa_mainloop
// of its own, which its device thread iterates and calls the data callback
// from: the stream is used only there (Measure, and the callbacks it
// registers).
struct PulseLatency {
    using SuccessCallback = void (*)(void* stream, int success, void* userdata);
    using NotifyCallback = void (*)(void* stream, void* userdata);
    void* (*updateTimingInfo)(void* stream, SuccessCallback callback, void* userdata) = nullptr;
    int (*getLatency)(void* stream, uint64_t* usec, int* negative) = nullptr;
    void (*operationUnref)(void* operation) = nullptr;
    void (*setUnderflowCallback)(void* stream, NotifyCallback callback, void* userdata) = nullptr;
    void* stream = nullptr;

    // For the CPU thread: the latency, 0 until measured, and the underflows.
    std::atomic<uint64_t> latencyUs{0};
    std::atomic<uint32_t> underflows{0};

    // The data callback, while the card plays: about once a second asks
    // for the stream's timing, whose reply gives the latency.
    void Measure(bool playing) {
        if (!hooked_) {
            setUnderflowCallback(stream, OnUnderflow, this);
            hooked_ = true;
        }
        const auto now = std::chrono::steady_clock::now();
        if (!playing || now - asked_ < std::chrono::seconds(1)) return;
        asked_ = now;
        if (void* operation = updateTimingInfo(stream, OnTimingInfo, this)) operationUnref(operation);
    }

private:
    static void OnUnderflow(void*, void* userdata) {
        static_cast<PulseLatency*>(userdata)->underflows.fetch_add(1, std::memory_order_relaxed);
    }
    static void OnTimingInfo(void* stream, int success, void* userdata) {
        auto* self = static_cast<PulseLatency*>(userdata);
        uint64_t usec = 0;
        int negative = 0;
        if (success && self->getLatency(stream, &usec, &negative) == 0 && !negative && usec)
            self->latencyUs.store(usec, std::memory_order_relaxed);
    }

    bool hooked_ = false;
    std::chrono::steady_clock::time_point asked_{};
};

MiniaudioOutput::MiniaudioOutput(unsigned bufferMs)
    : device_(nullptr), ringBuffer_(nullptr),
      latency_(SplitLatencyMs(std::clamp(bufferMs, kMinBufferMs, kMaxBufferMs))),
      targetFrames_(SAMPLE_RATE * latency_.ringMs / 1000), gate_(targetFrames_),
      servo_(targetFrames_, GAIN, MAX_PULL, MAX_DRIFT, SMOOTH_FRAMES, INTEGRAL_FRAMES) {

    auto* rb = new ma_pcm_rb;
    auto* dev = new ma_device;
    auto* context = new ma_context;

    // Initialize the ring buffer (float32, stereo)
    ma_result result = ma_pcm_rb_init(ma_format_f32, CHANNELS, RING_BUFFER_FRAMES, nullptr, nullptr, rb);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio ring buffer: %d", result);
        delete rb;
        delete dev;
        delete context;
        return;
    }

    // The thread miniaudio runs a device on (PulseAudio's, ALSA's; WASAPI's
    // too, at TIME_CRITICAL) real-time where the host allows it, else a
    // normal one: it has the card's period to refill the card in.
    ma_context_config contextConfig = ma_context_config_init();
    contextConfig.threadPriority = ma_thread_priority_realtime;
    result = ma_context_init(nullptr, 0, &contextConfig, context);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio context: %d", result);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        delete context;
        return;
    }

    // Configure the playback device
    ma_device_config config = ma_device_config_init(ma_device_type_playback);
    config.playback.format = ma_format_f32;
    config.playback.channels = CHANNELS;
    config.sampleRate = SAMPLE_RATE;
    // The card's share. miniaudio asks PulseAudio for a period of it in all
    // (ADJUST_LATENCY), which gives a quarter to the sink and three quarters
    // to the stream's queue (its tlength): what the callback thread has to
    // refill it in time. Measured on a desktop, the thread not real-time: at
    // 5.8ms (4.4ms queued) it was late about once a minute, the card playing
    // silence the ring never saw; at 8.7ms not in three. Other backends
    // buffer periods of it (ALSA, WASAPI): two of half.
    if (context->backend == ma_backend_pulseaudio) {
        config.periodSizeInMilliseconds = latency_.cardMs;
    } else {
        config.periods = 2;
        config.periodSizeInMilliseconds = (latency_.cardMs + 1) / 2;
    }
    // The callback takes what PulseAudio asks for, not miniaudio's periods
    // cut from it: those keep what is left of the last one, up to a period
    // more of latency. (Measured: no more underruns.) The gate's target
    // grows with the requests.
    config.noFixedSizedCallback = MA_TRUE;
    // Windows: the thread in MMCSS's "Pro Audio" class.
    config.wasapi.usage = ma_wasapi_usage_pro_audio;
    config.dataCallback = reinterpret_cast<ma_device_data_proc>(DataCallback);
    config.pUserData = this;

    result = ma_device_init(context, &config, dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio device: %d", result);
        ma_context_uninit(context);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        delete context;
        return;
    }

#ifdef MA_SUPPORT_PULSEAUDIO
    if (dev->pContext->backend == ma_backend_pulseaudio) {
        auto pulse = std::make_unique<PulseLatency>();
        auto symbol = [dev](const char* name) {
            return ma_dlsym(ma_device_get_log(dev), dev->pContext->pulse.pulseSO, name);
        };
        pulse->updateTimingInfo = reinterpret_cast<decltype(pulse->updateTimingInfo)>(symbol("pa_stream_update_timing_info"));
        pulse->getLatency = reinterpret_cast<decltype(pulse->getLatency)>(symbol("pa_stream_get_latency"));
        pulse->operationUnref = reinterpret_cast<decltype(pulse->operationUnref)>(symbol("pa_operation_unref"));
        pulse->setUnderflowCallback =
            reinterpret_cast<decltype(pulse->setUnderflowCallback)>(symbol("pa_stream_set_underflow_callback"));
        pulse->stream = dev->pulse.pStreamPlayback;
        if (pulse->updateTimingInfo && pulse->getLatency && pulse->operationUnref && pulse->setUnderflowCallback &&
            pulse->stream)
            pulse_ = std::move(pulse);
    }
#endif
    // Elsewhere what miniaudio was given: its periods in all, an estimate.
    if (!pulse_)
        cardEstimateUs_ = uint64_t(dev->playback.internalPeriodSizeInFrames) * dev->playback.internalPeriods * 1000000 /
                          dev->playback.internalSampleRate;

    ringBuffer_ = rb;
    result = ma_device_start(dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to start audio device: %d", result);
        ma_device_uninit(dev);
        ma_context_uninit(context);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        delete context;
        ringBuffer_ = nullptr;
        pulse_.reset();
        return;
    }

    device_ = dev;
    context_ = context;
    active_.store(true);
    LogInfo("Audio output initialized: %dHz, %dch, period=%u frames x %u",
            SAMPLE_RATE, CHANNELS, dev->playback.internalPeriodSizeInFrames, dev->playback.internalPeriods);
}

MiniaudioOutput::~MiniaudioOutput() {
    if (device_) {
        ma_device_uninit(static_cast<ma_device*>(device_));
        delete static_cast<ma_device*>(device_);
    }
    if (context_) {
        ma_context_uninit(static_cast<ma_context*>(context_));
        delete static_cast<ma_context*>(context_);
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
    const bool playing = self->gate_.Playing();
    self->playing_.store(playing, std::memory_order_relaxed);
    self->lastCallbackNs_.store(SteadyNs(), std::memory_order_relaxed);
    if (self->pulse_) self->pulse_->Measure(playing);
#ifdef __linux__
    // What the thread got: miniaudio falls back to a normal one silently.
    if (self->threadPolicy_.load(std::memory_order_relaxed) < 0) {
        int policy = 0;
        sched_param param{};
        if (pthread_getschedparam(pthread_self(), &policy, &param) == 0) {
            self->threadPriority_.store(param.sched_priority, std::memory_order_relaxed);
            self->threadPolicy_.store(policy, std::memory_order_release);
        }
    }
#endif
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
    lastWriteNs_ = SteadyNs();
    ReportThread();
    ReportLatency();

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

        // A write may stop at the physical end even when the ring has free
        // space at the start. Continue there; drop only when the ring is full.
        WriteAudioFrames(*rb, chunk, static_cast<ma_uint32>(framesToProcess), CHANNELS);

        // The pacer's rate that keeps the ring's level (audio_stream.h)
        if (FollowsCard(playing_.load(std::memory_order_relaxed), 0,
                        Since(lastWriteNs_, lastCallbackNs_.load(std::memory_order_relaxed)))) {
            servo_.SetTarget(TargetLevel(targetFrames_, request_.load(std::memory_order_relaxed)));
            servo_.Update(ma_pcm_rb_available_read(rb), framesToProcess);
        }

        framesRemaining -= framesToProcess;
        src += framesToProcess * srcStride;
    }
}

// The device thread's scheduling, once known (Linux).
void MiniaudioOutput::ReportThread() {
#ifdef __linux__
    if (threadReported_) return;
    const int policy = threadPolicy_.load(std::memory_order_acquire);
    if (policy < 0) return;
    threadReported_ = true;
    if (policy == SCHED_FIFO || policy == SCHED_RR) {
        LogInfo("Audio thread: real-time (%s %d)", policy == SCHED_FIFO ? "SCHED_FIFO" : "SCHED_RR",
                threadPriority_.load(std::memory_order_relaxed));
        return;
    }
    rlimit limit{};
    getrlimit(RLIMIT_RTPRIO, &limit);
    LogInfo("Audio thread: not real-time (RLIMIT_RTPRIO %llu, priority %d asked)", (unsigned long long)limit.rlim_cur,
            MA_PTHREAD_REALTIME_THREAD_PRIORITY);
#endif
}

double MiniaudioOutput::GuestRate(uint64_t wallNs) {
    if (!active_.load()) return 1.0;
    return PaceRate(servo_, FollowsCard(playing_.load(std::memory_order_relaxed), Since(wallNs, lastWriteNs_),
                                        Since(wallNs, lastCallbackNs_.load(std::memory_order_relaxed))));
}

// The card's latency as measured, once it is and then on a change: PulseAudio
// raises it after the card ran dry.
void MiniaudioOutput::ReportLatency() {
    const uint64_t us = pulse_ ? pulse_->latencyUs.load(std::memory_order_relaxed) : cardEstimateUs_;
    const uint32_t underflows = pulse_ ? pulse_->underflows.load(std::memory_order_relaxed) : 0;
    if (us == 0) return;
    const auto now = std::chrono::steady_clock::now();
    const double ms = us / 1000.0;
    if (!reportedUs_) {
        const char* source = pulse_ ? "PulseAudio" : "estimate";
        if (pulse_ && ms > latency_.cardMs + 1.0)
            LogWarn("Audio latency: card %.1f ms (%s), above the %u ms asked; ring %u ms", ms, source, latency_.cardMs,
                    latency_.ringMs);
        else
            LogInfo("Audio latency: card %.1f ms (%s) + ring %u ms", ms, source, latency_.ringMs);
    } else if (std::fabs(ms - reportedUs_ / 1000.0) > 2.0 ||
               (underflows != reportedUnderflows_ && now - reportedAt_ >= std::chrono::seconds(10))) {
        LogInfo("Audio latency: card %.1f ms, %u underflows", ms, underflows);
    } else {
        return;
    }
    reportedUs_ = us;
    reportedUnderflows_ = underflows;
    reportedAt_ = now;
}
