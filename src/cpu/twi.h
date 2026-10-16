#pragma once

#include "io.h"
#include <queue>
#include <functional>
#include <memory>
#include <map>

class I2CPeripheral {
public:
    virtual ~I2CPeripheral() {}
    virtual u32 Address() const = 0;
    virtual bool Read(u8* buffer, u32 length) = 0;
    virtual bool Write(const u8* buffer, u32 length) = 0;
    virtual void Stop() = 0;
};

class RegisterI2CPeripheral : public I2CPeripheral {
public:
    RegisterI2CPeripheral(u32 address) : address(address) {}
    u32 Address() const override { return address; }

    bool Read(u8* buffer, u32 length) override;
    bool Write(const u8* buffer, u32 length) override;
    void Stop() override;

protected:
    virtual Register* Next(u32 addr) const;

    u32 address;
    Register* writeRegister = nullptr;
    Register* readRegister = nullptr;
    std::map<u32, Register> registers;
};

class DummyI2CPeripheral : public I2CPeripheral {
public:
    DummyI2CPeripheral(u32 address, u8 data) : address(address), data(data) {}
    u32 Address() const override { return address; }
    bool Read(u8* buffer, u32 length) override {
        for(int i = 0; i < length; i++) buffer[i] = data;
        return true;
    }
    bool Write(const u8* buffer, u32 length) override { return true; }
    void Stop() override {}
protected:
    u32 address;
    u8 data;
};

class TimeSource;

// The master moves a byte every nine SCL periods of guest time: SCL is
// CLKHI + CLKLOW periods of the 10 MHz reference PRESCALE derives from SCLK,
// divided by a speedup (SetSpeedup; 1 is the hardware's rate). An address
// byte comes first; a full receive FIFO or an empty transmit FIFO holds SCL
// until the guest makes room or data.
class TWI : public RegisterDevice {
public:
    explicit TWI(u32 baseAddr, const TimeSource* time = nullptr);

    void AttachPeripheral(const std::shared_ptr<I2CPeripheral>& client) {
        clients[client->Address()] = client;
    }

    void ProcessWithInterrupt(int ivg) override;
    // A master transfer may move data; otherwise ProcessWithInterrupt does nothing.
    bool MasterActive() const { return enabled && masterEnable; }
    // ProcessWithInterrupt() has work at guest time nowNs: the next byte of
    // an enabled master transfer is due.
    bool NeedsService(uint64_t nowNs) const { return MasterActive() && nowNs >= nextByteNs_; }
    static constexpr int kIvg = 10;  // no master progress while the CPU is in this IVG

    // How many times faster than the programmed SCL the bus runs. At the
    // hardware's rate the stock boot spends 167 ms more guest time on I2C than
    // the emulator used to (it moved two bytes per block) and executes 5.9%
    // more packets to the main display; the default keeps the rate a model
    // of guest time while the bus stays fast.
    static constexpr unsigned kDefaultSpeedup = 10;
    void SetSpeedup(unsigned speedup) { speedup_ = speedup ? speedup : 1; }

    void Write32(u32 offset, u32 value) override;

protected:
    void ProcessMasterTransfer();
    void UpdateInterrupts();
    uint64_t Now() const;
    uint64_t ByteNs() const;
    // The guest made room or data for a transfer held by its FIFO.
    void Unstall();

    const TimeSource* time_;
    unsigned speedup_ = kDefaultSpeedup;
    // When the master's next byte completes; UINT64_MAX while the FIFO
    // holds it.
    uint64_t nextByteNs_ = 0;

    // Control register
    u16 prescale = 0;
    bool enabled = false;
    bool sccbMode = false;

    // Clock divider
    u8 clkLow = 0;
    u8 clkHigh = 0;

    // Slave registers (minimal implementation)
    u16 slaveCtl = 0;
    u16 slaveStat = 0;
    u16 slaveAddr = 0;

    // Master registers
    u8 masterDCNT = 0;
    u8 masterAddr = 0;
    bool masterRepeatStart = false;
    bool masterStop = false;
    bool masterFast = false;
    bool masterRead = false;
    bool masterEnable = false;
    bool masterTransferInProgress = false;
    bool masterLostArbitration = false;
    bool masterAddressNack = false;
    bool masterDataNack = false;
    bool masterBufferReadError = false;
    bool masterBufferWriteError = false;

    // Interrupt registers
    u16 intMask = 0;
    u8 slaveIntStat = 0;
    bool masterTransferComplete = false;
    bool masterTransferError = false;
    bool transmitFIFOService = false;
    bool receiveFIFOService = false;

    // FIFO registers
    bool transmitBufferFlush = false;
    bool receiveBufferFlush = false;
    bool transmitBufferInterruptLength = false;
    bool receiveBufferInterruptLength = false;

    // FIFO buffers
    std::queue<u8> xmtFifo;
    std::queue<u8> rcvFifo;

    std::map<u32, std::shared_ptr<I2CPeripheral>> clients;
};