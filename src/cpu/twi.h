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
    Register* selectedRegister = nullptr;
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

class TWI : public RegisterDevice {
public:
    TWI(u32 baseAddr);

    void AttachPeripheral(const std::shared_ptr<I2CPeripheral>& client) {
        clients[client->Address()] = client;
    }

    void ProcessWithInterrupt(int ivg) override;

protected:
    void ProcessMasterTransfer();
    void UpdateInterrupts();

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