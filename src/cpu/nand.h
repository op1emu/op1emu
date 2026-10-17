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
    // Called when the flash finishes an operation and stops being busy.
    virtual void SetReadyCallback(std::function<void()> callback) {}
};

class NFC : public RegisterDevice, public DMABus {
public:
    NFC(u32 baseAddr);

    void AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash);

    // DMABus interface
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;

    void ProcessWithInterrupt(int ivg) override;

    // ProcessWithInterrupt() has work: it would change NBUSY, RD_RDY or the
    // level it last published (ServiceIsNoop()). Its result depends only on
    // the NFC's and the flash's state, which change only at register
    // accesses, DMA transfers and the flash's ready callback (an event), so
    // each of those decides it anew from the state it leaves.
    bool NeedsService() const { return touched_; }
    // A register access gives the host work after the block only when the
    // service would then do something. A command that makes the flash busy
    // queues its ready event, which stops the run by itself.
    bool HostWorkAfterAccess(u32 offset, bool write) const override { return touched_; }

#ifdef ENABLE_PROFILING
    // What ProcessWithInterrupt() sets, packed: the dry run's check that a
    // service it skipped would have changed nothing.
    u32 ServiceState() const {
        return u32(notBusy) | u32(notBusyRising) << 1 | u32(readDataReady) << 2 | u32(publishedLevel_ + 1) << 3;
    }
#endif

    // 8- and 16-bit accesses reach these through RegisterDevice::Read/Write.
    u32 Read32(u32 offset) override {
        const u32 value = RegisterDevice::Read32(offset);
        Settle();
        return value;
    }
    void Write32(u32 offset, u32 value) override {
        RegisterDevice::Write32(offset, value);
        Settle();
    }

protected:
    u32 PageSize() const { return (pageSize == 0) ? 256 : 512; }
    void ResetECC();
    void CalculateECC(const u8* data, u32 length);
    void UpdateInterrupts();
    // NFC_IRQSTAT as the guest reads it.
    u16 IrqStat() const;
    // ProcessWithInterrupt() would change nothing.
    bool ServiceIsNoop() const;
    void Settle() { touched_ = !ServiceIsNoop(); }

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

    // ProcessWithInterrupt() has work (NeedsService()).
    bool touched_ = true;
    // The level UpdateInterrupts() last published, -1 before the first.
    int publishedLevel_ = -1;
};