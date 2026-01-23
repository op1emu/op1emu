#include "sport.h"
#include "emu.h"
#include "utils/log.h"

enum DataFormat {
    NORMAL = 0,
    COMPANDING_U_LAW = 2,
    COMPANDING_A_LAW = 3,
};

constexpr std::size_t FIFO_SIZE = 8; // 8x16-bit words or 4x32-bit words

SPORT::SPORT(u32 baseAddr, int sportNum)
    : RegisterDevice("SPORT" + std::to_string(sportNum), baseAddr, 0x60), sportNumber(sportNum) {

    REG32(SPORT_TCR1, 0x00);
    FIELD(SPORT_TCR1, TSPEN, 0, 1, R(transmitEnabled), W(transmitEnabled));
    FIELD(SPORT_TCR1, TDTYPE, 2, 2, R(transmitDataFormat), W(transmitDataFormat));
    FIELD(SPORT_TCR1, TLSBIT, 4, 1, R(transmitOrderLsbFirst), W(transmitOrderLsbFirst));
    SPORT_TCR1.writeCallback = [this](u32 value) {
        SetTransmitEnable();
    };

    REG32(SPORT_TCR2, 0x04);
    FIELD(SPORT_TCR2, SLEN, 0, 5, R(transmitWordLength - 1), [this](u32 v) {
        transmitWordLength = v + 1;
    });
    FIELD(SPORT_TCR2, TSFSE, 8, 1, R(transmitStereoFrameSync), W(transmitStereoFrameSync));
    FIELD(SPORT_TCR2, TXSE, 9, 1, R(transmitSecondaryEnabled), W(transmitSecondaryEnabled));
    FIELD(SPORT_TCR2, TRFST, 10, 1, R(transmitRightStereoOrderFirst), W(transmitRightStereoOrderFirst));

    REG32(SPORT_TX, 0x10);
    SPORT_TX.writeCallback = [this](u32 v) {
        std::size_t fifoSize = transmitWordLength > 16 ? FIFO_SIZE/2 : FIFO_SIZE;
        if (transmitFifo.size() >= fifoSize) {
            transmitOverflow = true;
        } else {
            transmitFifo.push(v);
        }
    };

    REG32(RX, 0x18);
    FIELD(RX, VAL, 0, 32, [this]() {
        if (receiveFifo.empty()) {
            receiveUnderflow = true;
            return 0u;
        } else {
            u32 value = receiveFifo.front();
            receiveFifo.pop();
            return value;
        }
    }, N());

    REG32(SPORT_RCR1, 0x20);
    FIELD(SPORT_RCR1, RSPEN, 0, 1, R(receiveEnabled), W(receiveEnabled));
    FIELD(SPORT_RCR1, RDTYPE, 2, 2, R(receiveDataFormat), W(receiveDataFormat));
    FIELD(SPORT_RCR1, RLSBIT, 4, 1, R(receiveOrderLsbFirst), W(receiveOrderLsbFirst));
    SPORT_RCR1.writeCallback = [this](u32 value) {
        SetReceiveEnable();
    };

    REG32(SPORT_RCR2, 0x24);
    FIELD(SPORT_RCR2, SLEN, 0, 5, R(receiveWordLength - 1), [this](u32 v) {
        receiveWordLength = v + 1;
    });
    FIELD(SPORT_RCR2, RSFSE, 8, 1, R(receiveStereoFrameSync), W(receiveStereoFrameSync));
    FIELD(SPORT_RCR2, RXSE, 9, 1, R(receiveSecondaryEnabled), W(receiveSecondaryEnabled));
    FIELD(SPORT_RCR2, RRFST, 10, 1, R(receiveRightStereoOrderFirst), W(receiveRightStereoOrderFirst));

    REG32(SPORT_STAT, 0x30);
    FIELD(SPORT_STAT, RXNE, 0, 1, R(!receiveFifo.empty()), N());
    FIELD(SPORT_STAT, RUVF, 1, 1, R(receiveUnderflow), W1C(receiveUnderflow));
    FIELD(SPORT_STAT, ROVF, 2, 1, R(receiveOverflow), W1C(receiveOverflow));
    FIELD(SPORT_STAT, TXF, 3, 1, R(transmitFifo.size() >= (transmitWordLength > 16 ? FIFO_SIZE/2 : FIFO_SIZE)), N());
    FIELD(SPORT_STAT, TUVF, 4, 1, R(transmitUnderflow), W1C(transmitUnderflow));
    FIELD(SPORT_STAT, TOVF, 5, 1, R(transmitOverflow), W1C(transmitOverflow));
    FIELD(SPORT_STAT, TXHRE, 6, 1, R(transmitHoldRegister.has_value()), N());
}

void SPORT::SetTransmitEnable() {
    if (!transmitEnabled) {
        transmitOverflow = false;
        transmitUnderflow = false;
        transmitHoldRegister.reset();
    }
}

void SPORT::SetReceiveEnable() {
    if (!receiveEnabled) {
        receiveOverflow = false;
        receiveUnderflow = false;
        receiveHoldRegister.reset();
    }
}

u32 SPORT::DMARead(int x, int y, void* dest, u32 length)
{
    if (!receiveEnabled) {
        return 0;
    }

    int wordSize = receiveWordLength > 16 ? 4 : 2;
    int channels = receiveStereoFrameSync ? 2 : 1;
    int bitsPerSample = receiveWordLength;
    size_t samples = length / wordSize / channels;

    if (audioInputCallback) {
        size_t read = audioInputCallback(dest, samples, channels, bitsPerSample);
        return read * wordSize * channels;
    }

    // Fill with silence
    return length;
}

u32 SPORT::DMAWrite(int x, int y, const void* source, u32 length)
{
    if (!transmitEnabled) {
        return 0;
    }

    int wordSize = transmitWordLength > 16 ? 4 : 2;
    int channels = transmitStereoFrameSync ? 2 : 1;
    int bitsPerSample = transmitWordLength;
    size_t samples = length / wordSize / channels;

    if (audioOutputCallback) {
        audioOutputCallback(source, samples, channels, bitsPerSample);
        return length;
    }

    // Discard data
    return length;
}
