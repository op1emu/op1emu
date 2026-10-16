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
    using AudioOutputCallback = std::function<void(const void* data, size_t samples, int channels, int bitsPerSample)>;
    using AudioInputCallback = std::function<size_t(void* data, size_t samples, int channels, int bitsPerSample)>;

    SPORT(u32 baseAddr, int sportNum, const TimeSource& time);

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    // Samples come due at the sample rate, counted from the first transfer.
    uint64_t DMAIdleUntil(bool toMemory, u32 length) const override;

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
    uint64_t NextSampleNs(uint64_t startNs, uint64_t delivered) const;
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

    // Nominal sample rate in Hz (future: derive from TCLKDIV/TFSDIV registers)
    uint32_t sampleRateHz_ = 48000;
};
