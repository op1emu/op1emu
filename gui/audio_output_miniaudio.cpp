#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "audio_output_miniaudio.h"
#include "utils/log.h"
#include <cstring>
#include <algorithm>

static constexpr ma_uint32 SAMPLE_RATE = 48000;
static constexpr ma_uint32 CHANNELS = 2;
static constexpr ma_uint32 RING_BUFFER_FRAMES = 4096; // ~85ms at 48kHz

MiniaudioOutput::MiniaudioOutput()
    : device_(nullptr), ringBuffer_(nullptr) {

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
    config.periodSizeInFrames = 256; // ~5.3ms low-latency period
    config.dataCallback = reinterpret_cast<ma_device_data_proc>(DataCallback);
    config.pUserData = rb;

    result = ma_device_init(nullptr, &config, dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to initialize audio device: %d", result);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        return;
    }

    result = ma_device_start(dev);
    if (result != MA_SUCCESS) {
        LogError("Failed to start audio device: %d", result);
        ma_device_uninit(dev);
        ma_pcm_rb_uninit(rb);
        delete rb;
        delete dev;
        return;
    }

    device_ = dev;
    ringBuffer_ = rb;
    active_.store(true);
    LogInfo("Audio output initialized: %dHz, %dch, period=%d frames",
            SAMPLE_RATE, CHANNELS, config.periodSizeInFrames);
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
    ma_pcm_rb* rb = static_cast<ma_pcm_rb*>(dev->pUserData);
    float* output = static_cast<float*>(pOutput);

    ma_uint32 framesRemaining = frameCount;
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

    // Fill remaining with silence (underrun)
    if (framesRemaining > 0) {
        memset(output + framesWritten * CHANNELS, 0, framesRemaining * CHANNELS * sizeof(float));
    }
}

void MiniaudioOutput::WriteSamples(const void* data, size_t samples, int channels, int bitsPerSample) {
    if (!active_.load() || samples == 0) {
        return;
    }

    ma_pcm_rb* rb = static_cast<ma_pcm_rb*>(ringBuffer_);

    // Convert input PCM to float32 stereo and push to ring buffer.
    // Process in chunks to avoid large stack allocations.
    constexpr size_t CHUNK_FRAMES = 256;
    float chunk[CHUNK_FRAMES * CHANNELS];

    const uint8_t* src = static_cast<const uint8_t*>(data);
    int wordSize = bitsPerSample > 16 ? 4 : 2;
    size_t srcStride = wordSize * channels;

    size_t framesRemaining = samples;
    while (framesRemaining > 0) {
        size_t framesToProcess = std::min(framesRemaining, CHUNK_FRAMES);

        // Convert to float32 stereo
        for (size_t i = 0; i < framesToProcess; i++) {
            float left = 0.0f, right = 0.0f;

            if (bitsPerSample <= 16) {
                // 16-bit signed integer
                int16_t sample;
                memcpy(&sample, src + i * srcStride, sizeof(int16_t));
                left = static_cast<float>(sample) / 32768.0f;

                if (channels >= 2) {
                    memcpy(&sample, src + i * srcStride + 2, sizeof(int16_t));
                    right = static_cast<float>(sample) / 32768.0f;
                } else {
                    right = left;
                }
            } else {
                // 24-bit packed in 32-bit words
                int32_t sample;
                memcpy(&sample, src + i * srcStride, sizeof(int32_t));
                // Sign-extend from 24-bit
                sample = (sample << 8) >> 8;
                left = static_cast<float>(sample) / 8388608.0f;

                if (channels >= 2) {
                    memcpy(&sample, src + i * srcStride + 4, sizeof(int32_t));
                    sample = (sample << 8) >> 8;
                    right = static_cast<float>(sample) / 8388608.0f;
                } else {
                    right = left;
                }
            }

            chunk[i * CHANNELS] = left;
            chunk[i * CHANNELS + 1] = right;
        }

        // Push to ring buffer
        ma_uint32 framesToWrite = static_cast<ma_uint32>(framesToProcess);
        void* writeBuffer;
        ma_result result = ma_pcm_rb_acquire_write(rb, &framesToWrite, &writeBuffer);
        if (result == MA_SUCCESS && framesToWrite > 0) {
            memcpy(writeBuffer, chunk, framesToWrite * CHANNELS * sizeof(float));
            ma_pcm_rb_commit_write(rb, framesToWrite);
        }
        // If ring buffer is full, we drop samples (acceptable for emulator)

        framesRemaining -= framesToProcess;
        src += framesToProcess * srcStride;
    }
}
