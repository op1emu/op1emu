#pragma once

#include "io.h"
#include "dma.h"
#include <vector>
#include <queue>
#include <optional>
#include <functional>
#include <cstdint>

class TimeSource;

class SPORT : public RegisterDevice, public DMABus {
public:
    // samples frames of channels words each: the primary word, then the
    // secondary channel's if it is on, for the left half of a stereo frame
    // sync and then for its right; stereo is the frame sync's.
    using AudioOutputCallback =
        std::function<void(const void* data, size_t samples, int channels, int bitsPerSample, bool stereo)>;
    using AudioInputCallback = std::function<size_t(void* data, size_t samples, int channels, int bitsPerSample)>;

    SPORT(u32 baseAddr, int sportNum, const TimeSource& time);

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    // Samples come due at the sample rate, counted from the first transfer.
    uint64_t DMAIdleUntil(bool toMemory, u32 length) const override;
    // What DMAWrite would take now (DMABus::DMAWriteTake): the samples due
    // (guest time stands still between the calls).
    u32 DMAWriteTake(u32 length) const override;

    void Write32(u32 offset, u32 value) override {
        NotifyDMA();
        RegisterDevice::Write32(offset, value);
    }

    // Audio callbacks
    void SetAudioOutputCallback(AudioOutputCallback callback) { audioOutputCallback = callback; }
    void SetAudioInputCallback(AudioInputCallback callback) { audioInputCallback = callback; }

protected:
    // When DMARead/DMAWrite next finds a sample due: the first time at which
    // `delivered` is below the samples due since `startNs`.
    struct NextSample {
        bool valid = false;
        uint64_t startNs = 0, delivered = 0, at = 0;
    };
    uint64_t NextSampleNs(uint64_t startNs, uint64_t delivered, NextSample& memo) const;
    // DMAWrite's due samples, at most requestedSamples (transmit active).
    uint64_t TxAvailable(uint64_t requestedSamples) const;
    // The words in a frame, which DMA moves together and the audio callbacks
    // see as its channels, and the frame's bytes in memory.
    int Channels(bool receive) const;
    u32 FrameSize(bool receive) const;
    void SetTransmitEnable();
    void SetReceiveEnable();

    int sportNumber;

    bool transmitEnabled = false;
    u8 transmitDataFormat = 0;
    bool transmitOrderLsbFirst = false;
    u8 transmitWordLength = 0;
    bool transmitSecondaryEnabled = false;
    bool transmitStereoFrameSync = false;
    bool transmitRightStereoOrderFirst = false;
    bool transmitOverflow = false;
    bool transmitUnderflow = false;

    bool receiveEnabled = false;
    u8 receiveDataFormat = 0;
    bool receiveOrderLsbFirst = false;
    u8 receiveWordLength = 0;
    bool receiveSecondaryEnabled = false;
    bool receiveStereoFrameSync = false;
    bool receiveRightStereoOrderFirst = false;
    bool receiveOverflow = false;
    bool receiveUnderflow = false;

    std::optional<u32> transmitHoldRegister;
    std::optional<u32> receiveHoldRegister;
    // FIFO buffers
    std::queue<u32> transmitFifo;
    std::queue<u32> receiveFifo;

    // Audio callbacks
    AudioOutputCallback audioOutputCallback;
    AudioInputCallback audioInputCallback;

    const TimeSource& time_;

    // DMA timing state — TX path
    uint64_t dmaTxStartNs_ = 0;
    uint64_t totalTxSamplesDelivered_ = 0;
    bool dmaTxActive_ = false;

    // DMA timing state — RX path
    uint64_t dmaRxStartNs_ = 0;
    uint64_t totalRxSamplesDelivered_ = 0;
    bool dmaRxActive_ = false;
    // NextSampleNs, per direction, for the DMA's repeated asks.
    mutable NextSample txNext_, rxNext_;

    // Frames per second. The OP-1's codec masters the link (its clocks are
    // not SPORT's TCLKDIV/TFSDIV) at 44.1 kHz.
    uint32_t sampleRateHz_ = 44100;
};
