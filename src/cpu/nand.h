#pragma once

#include "io.h"
#include "dma.h"
#include <vector>
#include <functional>

class NandFlash {
public:
    using ReadCallback = std::function<u8(NandFlash&)>;
    virtual ~NandFlash() {}

    virtual void SendCommand(u8 command) = 0;
    virtual void SendAddress(u8 address) = 0;

    virtual u8 ReadData() = 0;
    virtual void WriteData(u8 data) = 0;

    virtual bool IsDataReady() const = 0;
    virtual bool IsBusy() const = 0;

    virtual void StartPageRead() = 0;
    virtual void StartPageWrite() = 0;

    virtual u32 PageRead(u8* data, u32 length) = 0;
    virtual u32 PageWrite(const u8* data, u32 length) = 0;
    virtual void SetReadCallback(ReadCallback callback) = 0;
};

class NFC : public RegisterDevice, public DMABus {
public:
    NFC(u32 baseAddr);

    void AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash) {
        this->nandFlash = nandFlash;
    }

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    void ProcessWithInterrupt(int ivg) override;

protected:
    u32 PageSize() const { return (pageSize == 0) ? 256 : 512; }
    void ResetECC();
    void CalculateECC(const u8* data, u32 length);
    void UpdateInterrupts();

    void SetNotBusy(bool value);
    void SetWriteBufferEmpty(bool value);

    u8 pageSize = 1; // 0: 256 bytes, 1: 512 bytes

    bool notBusy = true;
    bool writeBufferFull = false;
    bool pageWritePending = false;
    bool pageReadPending = false;
    bool writeBufferEmpty = true;

    bool notBusyRising = false;
    bool writeBufferOverflow = false;
    bool writeBufferEmptyRising = true;
    bool readDataReady = false;
    bool pageWriteDone = false;

    u16 irqmask = 0x1F;
    u16 transferCount = 0;
    u32 eccValue = 0;
    u16 ecc[4] = {0, 0, 0, 0};

    bool pageReadStart = false;
    bool pageWriteStart = false;

    u8 readData = 0;
    u8 address = 0;
    u8 command = 0;
    u8 writeData = 0;

    std::shared_ptr<NandFlash> nandFlash;
};