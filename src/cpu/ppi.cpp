#include "ppi.h"
#include "peripheral/display.h"

enum PPIOutputType {
    PPI_OUTPUT_SYNCLESS = 0x00,
    PPI_OUTPUT_SYNCLESS_1 = 0x01,
    PPI_OUTPUT_SYNCLESS_2 = 0x10,
    PPI_OUTPUT_SYNC = 0x11,
};

enum PPIOutputSyncConfig {
    PPI_OUTPUT_SYNC_CONFIG_1_FRAME = 0x00,
    PPI_OUTPUT_SYNC_CONFIG_2_OR_3_FRAME = 0x01,
    PPI_OUTPUT_SYNC_CONFIG_FS3 = 0x11,
};

enum PPIDataLength {
    PPI_DATALEN_8 = 0x00,
    PPI_DATALEN_10 = 0x01,
    PPI_DATALEN_11 = 0x02,
    PPI_DATALEN_12 = 0x03,
    PPI_DATALEN_13 = 0x04,
    PPI_DATALEN_14 = 0x05,
    PPI_DATALEN_15 = 0x06,
    PPI_DATALEN_16 = 0x07,
};

PPI::PPI(u32 baseAddr) : RegisterDevice("PPI", baseAddr, 0x14) {
    REG32(PPI_CONTROL, 0x00);
    FIELD(PPI_CONTROL, PORT_EN, 0, 1, R(enabled), W(enabled));
    FIELD(PPI_CONTROL, PORT_DIR, 1, 1, R(outputMode), W(outputMode));
    FIELD(PPI_CONTROL, XFR_TYPE, 2, 2, R(transferType), W(transferType));
    FIELD(PPI_CONTROL, PORT_CFG, 4, 2, R(portConfig), W(portConfig));
    FIELD(PPI_CONTROL, PACK_EN, 7, 1, R(packingEnabled ? 1 : 0), W(packingEnabled));
    FIELD(PPI_CONTROL, DLEN, 11, 3, R(dataLength), W(dataLength));
    PPI_CONTROL.writeCallback = [this](u32 value) {
        if (enabled) {
            display->Initialize(rowCount + 1, lineCount);
        }
    };

    REG32(PPI_COUNT, 0x08);
    FIELD(PPI_COUNT, PPI_COUNT, 0, 16, R(rowCount), W(rowCount));

    REG32(PPI_DELAY, 0x0C);
    FIELD(PPI_DELAY, delay, 0, 16, R(delay), W(delay));

    REG32(PPI_FRAME, 0x10);
    FIELD(PPI_FRAME, PPI_FRAME, 0, 16, R(lineCount), W(lineCount));
}

u32 PPI::DMAWrite(int x, int y, const void* source, u32 length) {
    display->UpdateRowBuffer(x, y, source, length);
    return length;
}