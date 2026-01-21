#include "twi.h"
#include "utils/log.h"

// TWI_FIFO_STAT
static constexpr u16 FIFO_EMPTY = 0x0;
static constexpr u16 FIFO_HALF  = 0x1;
static constexpr u16 FIFO_FULL  = 0x3;

static constexpr size_t FIFO_SIZE = 2;
static constexpr int IVG_TWI = 10;

TWI::TWI(u32 baseAddr) : RegisterDevice("TWI", baseAddr, 0x90) {
    REG32(CLKDIV, 0x00);
    FIELD(CLKDIV, CLKLOW, 0, 8, R(clkLow), W(clkLow));
    FIELD(CLKDIV, CLKHI, 8, 8, R(clkHigh), W(clkHigh));

    REG32(CONTROL, 0x04);
    FIELD(CONTROL, PRESCALE, 0, 7, R(prescale), W(prescale));
    FIELD(CONTROL, TWI_ENA, 7, 1, R(enabled ? 1 : 0), W(enabled));
    FIELD(CONTROL, SCCB, 9, 1, R(sccbMode ? 1 : 0), W(sccbMode));

    REG32(SLAVE_CTL, 0x08);
    FIELD(SLAVE_CTL, VAL, 0, 16, R(slaveCtl), W(slaveCtl));

    REG32(SLAVE_STAT, 0x0C);
    FIELD(SLAVE_STAT, VAL, 0, 16, R(slaveStat), W(slaveStat));

    REG32(SLAVE_ADDR, 0x10);
    FIELD(SLAVE_ADDR, VAL, 0, 16, R(slaveAddr), W(slaveAddr));

    REG32(MASTER_CTL, 0x14);
    FIELD(MASTER_CTL, DCNT, 6, 8, R(masterDCNT), W(masterDCNT));
    FIELD(MASTER_CTL, RESTART, 5, 1, R(masterRepeatStart ? 1 : 0), W(masterRepeatStart));
    FIELD(MASTER_CTL, STOP, 4, 1, R(masterStop ? 1 : 0), W(masterStop));
    FIELD(MASTER_CTL, FAST, 3, 1, R(masterFast ? 1 : 0), W(masterFast));
    FIELD(MASTER_CTL, MDIR, 2, 1, R(masterRead ? 1 : 0), W(masterRead));
    FIELD(MASTER_CTL, MEN, 0, 1, R(masterEnable ? 1 : 0), W(masterEnable));
    MASTER_CTL.writeCallback = [this](u32 value) {
        if (masterStop) {
            auto iter = clients.find(masterAddr);
            if (iter != clients.end()) {
                iter->second->Stop();
            }
        }
    };

    REG32(MASTER_STAT, 0x18);
    FIELD(MASTER_STAT, MPROG, 0, 1, R(masterTransferInProgress ? 1 : 0), N());
    FIELD(MASTER_STAT, LOSTARB, 1, 1, R(masterLostArbitration ? 1 : 0), W1C(masterLostArbitration));
    FIELD(MASTER_STAT, ANAK, 2, 1, R(masterAddressNack ? 1 : 0), W1C(masterAddressNack));
    FIELD(MASTER_STAT, DNAK, 3, 1, R(masterDataNack ? 1 : 0), W1C(masterDataNack));
    FIELD(MASTER_STAT, BUFRDERR, 4, 1, R(masterBufferReadError ? 1 : 0), W1C(masterBufferReadError));
    FIELD(MASTER_STAT, BUFWRERR, 5, 1, R(masterBufferWriteError ? 1 : 0), W1C(masterBufferWriteError));

    REG32(MASTER_ADDR, 0x1C);
    FIELD(MASTER_ADDR, MADDR, 0, 7, R(masterAddr), W(masterAddr));

    REG32(INT_STAT, 0x20);
    FIELD(INT_STAT, VAL, 0, 4, R(slaveIntStat), W1C(slaveIntStat));
    FIELD(INT_STAT, MCOMP, 4, 1, R(masterTransferComplete ? 1 : 0), W1C(masterTransferComplete));
    FIELD(INT_STAT, MERR, 5, 1, R(masterTransferError ? 1 : 0), W1C(masterTransferError));
    FIELD(INT_STAT, XMTSERV, 6, 1, R(transmitFIFOService ? 1 : 0), W1C(transmitFIFOService));
    FIELD(INT_STAT, RCVSERV, 7, 1, R(receiveFIFOService ? 1 : 0), W1C(receiveFIFOService));
    INT_STAT.writeCallback = [this](u32 value) {
        UpdateInterrupts();
    };

    REG32(INT_MASK, 0x24);
    FIELD(INT_MASK, VAL, 0, 16, R(intMask), W(intMask));
    INT_MASK.writeCallback = [this](u32 value) {
        UpdateInterrupts();
    };

    REG32(FIFO_CTL, 0x28);
    FIELD(FIFO_CTL, XMTFLUSH, 0, 1, R(transmitBufferFlush ? 1 : 0), N());
    FIELD(FIFO_CTL, RCVFLUSH, 1, 1, R(receiveBufferFlush ? 1 : 0), N());
    FIELD(FIFO_CTL, XMTINTLEN, 2, 1, R(transmitBufferInterruptLength ? 1 : 0), W(transmitBufferInterruptLength));
    FIELD(FIFO_CTL, RCVINTLEN, 3, 1, R(receiveBufferInterruptLength ? 1 : 0), W(receiveBufferInterruptLength));

    REG32(FIFO_STAT, 0x2C);
    FIELD(FIFO_STAT, XMTSTAT, 0, 2, [this]() -> u32 {
        if (xmtFifo.empty()) return FIFO_EMPTY;
        else if (xmtFifo.size() == 1) return FIFO_HALF;
        else return FIFO_FULL;
    }, N());
    FIELD(FIFO_STAT, RCVSTAT, 2, 2, [this]() -> u32 {
        if (rcvFifo.empty()) return FIFO_EMPTY;
        else if (rcvFifo.size() == 1) return FIFO_HALF;
        else return FIFO_FULL;
    }, N());

    REG32(XMT_DATA8, 0x80);
    FIELD(XMT_DATA8, XMTDATA8, 0, 8, R(0), [this](u32 v) {
        if (xmtFifo.size() < FIFO_SIZE) {
            xmtFifo.push(v & 0xFF);
        }
    });

    REG32(XMT_DATA16, 0x84);
    FIELD(XMT_DATA16, XMTDATA16, 0, 16, R(0), [this](u32 v) {
        if (xmtFifo.size() < FIFO_SIZE) {
            xmtFifo.push(v & 0xFF);
            if (xmtFifo.size() < FIFO_SIZE) {
                xmtFifo.push((v >> 8) & 0xFF);
            }
        }
    });

    REG32(RCV_DATA8, 0x88);
    FIELD(RCV_DATA8, RCVDATA8, 0, 8, [this]() -> u32 {
        if (rcvFifo.empty()) return 0;
        u8 val = rcvFifo.front();
        rcvFifo.pop();
        return val;
    }, N());

    REG32(RCV_DATA16, 0x8C);
    FIELD(RCV_DATA16, RCVDATA16, 0, 16, [this]() -> u32 {
        u16 val = 0;
        if (!rcvFifo.empty()) {
            val = rcvFifo.front();
            rcvFifo.pop();
        }
        if (!rcvFifo.empty()) {
            val |= (rcvFifo.front() << 8);
            rcvFifo.pop();
        }
        return val;
    }, N());
}

void TWI::ProcessWithInterrupt(int ivg) {
    if (ivg != IVG_TWI) {
        ProcessMasterTransfer();
    }
}

void TWI::ProcessMasterTransfer() {
    if (!enabled) return;
    if (!masterEnable) return;

    // Set transfer in progress
    masterTransferInProgress = true;

    if (masterRead) {
        // Master read operation
        size_t bytesRead = std::min(FIFO_SIZE - rcvFifo.size(), (size_t)masterDCNT);
        if (bytesRead == 0) {
            // Nothing to read
            masterTransferInProgress = false;
            return;
        }
        auto iter = clients.find(masterAddr);
        if (iter == clients.end()) {
            LogDebug("TWI: No client found at address 0x%02X\n", masterAddr);
            masterAddressNack = true;
            masterTransferError = true;
            masterEnable = false;
        } else {
            auto client = iter->second;
            std::vector<u8> data(bytesRead);
            bool success = client->Read(data.data(), bytesRead);
            if (!success) {
                masterBufferReadError = true;
                masterTransferError = true;
            } else {
                masterDCNT -= bytesRead;
                for (size_t i = 0; i < bytesRead; i++) {
                    rcvFifo.push(data[i]);
                }
                if (masterDCNT == 0) {
                    masterTransferComplete = true;
                }
                if (rcvFifo.size() > 0 && !receiveBufferInterruptLength) {
                    receiveFIFOService = true;
                }
                if (rcvFifo.size() >= FIFO_SIZE && receiveBufferInterruptLength) {
                    receiveFIFOService = true;
                }
            }
        }
    } else {
        // Master write operation
        size_t bytesWrite = std::min(xmtFifo.size(), (size_t)masterDCNT);
        if (bytesWrite == 0) {
            // Nothing to write
            masterTransferInProgress = false;
            return;
        }
        auto iter = clients.find(masterAddr);
        if (iter == clients.end()) {
            LogDebug("TWI: No client found at address 0x%02X\n", masterAddr);
            masterAddressNack = true;
            masterTransferError = true;
        } else {
            // Collect data from FIFO
            std::vector<u8> data;
            while (data.size() < bytesWrite) {
                data.push_back(xmtFifo.front());
                xmtFifo.pop();
            }
            auto client = iter->second;
            bool success = client->Write(data.data(), data.size());
            if (!success) {
                masterBufferWriteError = true;
                masterTransferError = true;
                masterEnable = false;
            } else {
                masterDCNT -= bytesWrite;
                if (masterDCNT == 0) {
                    masterTransferComplete = true;
                }
                if (xmtFifo.size() < FIFO_SIZE && !transmitBufferInterruptLength) {
                    transmitFIFOService = true;
                }
                if (xmtFifo.size() == 0 && transmitBufferInterruptLength) {
                    transmitFIFOService = true;
                }
            }
        }
    }
    if ((masterTransferComplete && !masterRepeatStart) || masterTransferError) {
        masterEnable = false;
        auto iter = clients.find(masterAddr);
        if (iter != clients.end()) {
            iter->second->Stop();
        }
    }

    // Transfer complete
    masterTransferInProgress = false;
    UpdateInterrupts();
}

void TWI::UpdateInterrupts() {
    u16 intStat = Read32(0x20) & 0xFFFF;
    if (intStat & intMask) {
        TriggerInterrupt(1);
    } else {
        TriggerInterrupt(0);
    }
}

bool RegisterI2CPeripheral::Read(u8* buffer, u32 length) {
    if (readRegister) {
        for (int i = 0; i < length; i++) {
            u32 value = readRegister->Read32();
            buffer[i] = value & 0xFF;
            readRegister = Next(readRegister->addr);
        }
        return true;
    }
    return false;
}

bool RegisterI2CPeripheral::Write(const u8* buffer, u32 length) {
    int index = 0;
    if (length == 0) {
        return false;
    }
    if (!writeRegister) {
        u32 regAddr = buffer[0];
        auto iter = registers.find(regAddr);
        if (iter == registers.end()) {
            return false;
        }
        writeRegister = &iter->second;
        index++;
    }
    for (; index < length; index++) {
        u32 value = buffer[index];
        writeRegister->Write32(value);
        writeRegister = Next(writeRegister->addr);
    }
    readRegister = writeRegister;
    return true;
}

Register* RegisterI2CPeripheral::Next(u32 addr) const {
    auto iter = registers.find(addr + 1);
    if (iter != registers.end()) {
        return const_cast<Register*>(&iter->second);
    }
    return nullptr;
}

void RegisterI2CPeripheral::Stop() {
    // write register reset on stop condition
    writeRegister = nullptr;
}