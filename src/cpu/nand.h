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

    // Returns the core IVG the NFC interrupt is currently routed to (from
    // SIC_IARx), so the NFC can tell when its own ISR is running.
    void SetIVGResolver(std::function<int()> resolver) {
        ivgResolver = std::move(resolver);
    }

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    void ProcessWithInterrupt(int ivg) override;

    // A count that changes whenever the SIC's routing may have (see
    // SIC::Version()): the IVG the resolver returns is only re-read then.
    void SetRoutingVersion(const uint64_t* version) { routingVersion_ = version; }

    // ProcessWithInterrupt() has work: a register access or a DMA transfer
    // since the last call, a flash still busy at it, or a change in what says
    // whether the NFC's own ISR is running (the current IVG, the SIC's
    // routing). Otherwise it would only publish its level again, if it
    // published one, which ReplayInterrupt() does.
    bool NeedsService(int ivg) const {
        return touched_ || !notBusy || ivg != serviceIvg_ ||
               (routingVersion_ && *routingVersion_ != serviceRouting_);
    }
    void ReplayInterrupt() const {
        if (published_) TriggerInterrupt(level_);
    }

    void Read(u32 offset, void* buffer, u32 length) override {
        touched_ = true;
        RegisterDevice::Read(offset, buffer, length);
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        touched_ = true;
        RegisterDevice::Write(offset, buffer, length);
    }
    u32 Read32(u32 offset) override {
        touched_ = true;
        return RegisterDevice::Read32(offset);
    }
    void Write32(u32 offset, u32 value) override {
        touched_ = true;
        RegisterDevice::Write32(offset, value);
    }

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
    u16 ecc[4] = {0, 0, 0, 0};

    bool pageReadStart = false;
    bool pageWriteStart = false;

    u8 readData = 0;
    u8 address = 0;
    u8 command = 0;
    u8 writeData = 0;

    std::shared_ptr<NandFlash> nandFlash;
    std::function<int()> ivgResolver;

    // Since the last ProcessWithInterrupt(): accessed, and what it saw.
    bool touched_ = true;
    int serviceIvg_ = -2;
    uint64_t serviceRouting_ = 0;
    const uint64_t* routingVersion_ = nullptr;
    bool published_ = false;  // UpdateInterrupts() published level_ (it may hold back)
    int level_ = 0;
};