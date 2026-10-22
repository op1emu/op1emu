#pragma once

#include "io.h"
#include "emu.h"
#include <vector>
#include <map>
#include <memory>

class Bus;
class TimeSource;

// Core cycles per element of a DMA bus clocked by SCLK, one element per
// SCLK cycle: an assumed 100 MHz SCLK (CCLK / 4). The PPI and MDMA are not
// paced by their peripheral, so this is what paces them in guest time.
constexpr u32 kSclkCyclesPerElement = 4;

class DMABus {
public:
    virtual ~DMABus() {}

    virtual u32 DMARead(int x, int y, void* dest, u32 length) = 0;
    virtual u32 DMAWrite(int x, int y, const void* source, u32 length) = 0;
    // At most how many of `length` bytes a DMAWrite now would take (it may
    // take fewer), also when the DMA reads its source in between: a bus
    // whose take may grow meanwhile (a clock that moves) must not bound it.
    // The default, all of them, is always right.
    virtual u32 DMAWriteTake(u32 length) const { return length; }
    // At most how many bytes a DMARead now would give. The default, any
    // number, is always right.
    virtual u32 DMAReadTake() const { return UINT32_MAX; }
    // Discard any buffered data. MDMA's internal FIFO is flushed when a new
    // transfer starts; peripheral buses ignore this.
    virtual void DMAFlush() {}

    // Before which guest time (TimeSource::Nanoseconds()) a DMARead
    // (toMemory) or DMAWrite of `length` bytes would do nothing at all: no
    // data, no state. 0, the default, says any call may do something; the
    // maximum says none will until the bus is told something new, which it
    // reports with NotifyDMA().
    virtual uint64_t DMAIdleUntil(bool toMemory, u32 length) const { return 0; }
    // Core cycles of guest time the channel takes per element on this bus:
    // it moves a row (or 4096 bytes) once that many cycles per element have
    // passed since the last one. 0: as fast as the bus accepts.
    virtual u32 CyclesPerElement() const { return 0; }
    // Whether DMARead and DMAWrite depend only on the bytes moved so far, not
    // on when they are called: a paced channel on such a bus may move its
    // rows late, each as at its own time (DMA::Settle). Memory DMA's FIFO;
    // not a peripheral, whose data is due at times of its own.
    virtual bool Settles() const { return false; }
    // The DMA controller's flag NotifyDMA() raises.
    void SetDMANotify(bool* flag) { dmaNotify_ = flag; }

protected:
    void NotifyDMA() const {
        if (dmaNotify_) *dmaNotify_ = true;
    }

private:
    bool* dmaNotify_ = nullptr;
};

// MDMA source/destination channels are just two ordinary DMA channels
// connected through an internal FIFO: the source channel does memory->FIFO
// (WNR=0), the destination channel does FIFO->memory (WNR=1). Both sides run
// through the normal DMAChannel::ProcessTransfer(), so this bus only needs to
// hold bytes in flight; it decouples the two channels' independent 1D/2D
// stride advancement and provides backpressure via short reads/writes.
class MemorySrcDMABus : public DMABus {
public:
    MemorySrcDMABus() {}
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    u32 DMAWriteTake(u32 length) const override;
    void DMAFlush() override { head = tail = 0; }
    u32 CyclesPerElement() const override { return kSclkCyclesPerElement; }
    bool Settles() const override { return true; }
    u32 Held() const { return tail - head; }

protected:
    static constexpr u32 CAPACITY = 4096; // matches ProcessTransfer's staging buffer
    u8 buffer[CAPACITY];
    u32 head = 0; // dequeue offset
    u32 tail = 0; // enqueue offset
};

class MemoryDestDMABus : public DMABus {
public:
    MemoryDestDMABus(MemorySrcDMABus& source) : source(source) {}
    u32 DMARead(int x, int y, void* dest, u32 length) override;
    u32 DMAWrite(int x, int y, const void* source, u32 length) override;
    u32 DMAWriteTake(u32 length) const override;
    u32 DMAReadTake() const override { return source.Held(); }
    void DMAFlush() override { source.DMAFlush(); }
    u32 CyclesPerElement() const override { return kSclkCyclesPerElement; }
    bool Settles() const override { return true; }

protected:
    MemorySrcDMABus& source;
};

enum DMAPeripheralType {
    DMAPeripheralPPI = 0x0,
    DMAPeripheralHOSTDP = 0x1,
    DMAPeripheralNFC = 0x2,
    DMAPeripheralSPORT0Rx = 0x3,
    DMAPeripheralSPORT0Tx = 0x4,
    DMAPeripheralSPORT1Rx = 0x5,
    DMAPeripheralSPORT1Tx = 0x6,
    DMAPeripheralSPI = 0x7,
    DMAPeripheralUART0Rx = 0x8,
    DMAPeripheralUART0Tx = 0x9,
    DMAPeripheralUART1Rx = 0xA,
    DMAPeripheralUART1Tx = 0xB,
    DMAPeripheralMDMADest0 = 0xC,
    DMAPeripheralMDMASrc0 = 0xD,
    DMAPeripheralMDMADest1 = 0xE,
    DMAPeripheralMDMASrc1 = 0xF,
};

class DMAChannel;
class DMA : public RegisterDevice, public PageWatcher {
public:
    // Without a time source no bus is paced (CyclesPerElement is ignored).
    DMA(u32 baseAddr, Emulator& emu, const TimeSource* time = nullptr);

    void AttachDMABus(DMAPeripheralType type, const std::shared_ptr<DMABus>& bus) {
        dmaBuses[type] = bus;
        busByType_[type & 15] = bus.get();
        bus->SetDMANotify(&touched_);
        touched_ = true;
    }

    void BindInterrupt(int channel, int q, InterruptHandler callback);

    // Owned by the DMA (AttachDMABus); null where none is attached.
    DMABus* GetDMABus(DMAPeripheralType type) const { return busByType_[type & 15]; }
    Emulator& GetEmulator() { return emulator; }
    const TimeSource* Time() const { return time_; }
    // The guest cycle now (0 without a time source).
    uint64_t Now() const;
    bool Settling() const { return settleRows_; }

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void ProcessWithInterrupt(int ivg) override;
    // Acknowledging a channel's IRQ_STATUS changes nothing a service would
    // act on; any other write may (touched_). A read has no effect but where
    // it settles a channel's rows (SettlesOnRead): it moved guest memory,
    // and may have ended a row.
    bool HostWorkAfterAccess(u32 offset, bool write) const override {
        return write ? touched_ : SettlesOnRead(offset);
    }
    // The channels' registers change only in a service or a write, but for
    // the current address and counts of a channel that settles its rows.
    bool ReadStable(u32 offset) const override { return !SettlesOnRead(offset); }

    // Settling (the default; set before any channel runs): a paced channel
    // on a bus that Settles() moves its rows when something could see them
    // -- a service at a row whose end the guest is told of (an interrupt,
    // the transfer's end), a register write, a read of its current address
    // or counts, an access to memory its transfer has yet to read or write
    // -- not at each row's time. Each row moves as a service at its own
    // cycle would have moved it, so the transfer is the one a service every
    // cycle makes, and every access sees memory as it would then. For
    // memory the service watches the pages the rest of each transfer lies
    // in (Emulator::WatchPages()), from the service after the write that
    // starts it: until then the block that wrote it sees them as before,
    // as a channel that does not settle shows its first row only at that
    // service. So for the next descriptor of a transfer that ends inside an
    // access: its pages are watched from the service after that block. A
    // service moves the rows before the other channels' (a PPI
    // or SPORT reading what memory DMA wrote sees it written). Off, every
    // row is due on its own (the test's reference).
    void SetSettling(bool on) { settleRows_ = on; }

    // PageWatcher: the rows due by now move first, but where the DMA itself
    // reads or writes memory. A row end the guest is told of (an interrupt,
    // a transfer's end) comes inside the access, as at a read of a current
    // count, and gives the host work: a service, which also watches the
    // pages of a transfer it began.
    void BeforeAccess() override;
    bool HostWorkAfterAccess() const override { return touched_; }

    // ProcessWithInterrupt() has work at guest time nowNs: a register write
    // or a bus notice since the last call, or a running channel whose bus may
    // move data. Otherwise every channel's transfer would return at once.
    bool NeedsService(uint64_t nowNs) const { return touched_ || nowNs >= idleUntil_; }
    // Untouched, NeedsService() turns true at this guest time.
    uint64_t IdleUntilNs() const { return idleUntil_; }
    // The peripheral type of the channel that sets IdleUntilNs() (profiling).
    unsigned IdleSource() const { return idleSource_; }

protected:
    // Before when no running channel can move data (see DMABus::DMAIdleUntil)
    // or, settling, do something the guest is told of.
    uint64_t IdleUntil();
    // Moves the settling channels' rows due by guest cycle `until`: at each
    // cycle a row is due, a pass over them as a service then would make.
    void Settle(uint64_t until);
    // A read of the current address or counts of a channel that settles.
    bool SettlesOnRead(u32 offset) const;
    // No earlier than this guest cycle does settling channel `index`'s next
    // event come, as of the last Settle, at guest cycle `now`.
    uint64_t EventBound(size_t index, uint64_t now) const;
    // runnable_'s bit for a channel after something may have changed it: a
    // register write, or its own transfer (completion, the next descriptor).
    void Refresh(size_t channel);
    // At the end of a service: the pages the settling channels' transfers
    // have yet to reach, watched (SetSettling()).
    void Watch();

    bool touched_ = true;
    uint64_t idleUntil_ = 0;
    unsigned idleSource_ = 0;
    const TimeSource* time_;
    Emulator& emulator;
    std::array<std::shared_ptr<DMAChannel>, 16> channels;
    std::map<DMAPeripheralType, std::shared_ptr<DMABus>> dmaBuses;
    std::array<DMABus*, 16> busByType_{};
    u16 runnable_ = 0;  // bit n: channel n is enabled and running (Refresh)
    // Settling (Settle): the channels of runnable_ that settle; those that
    // could not move at the last pass (until another moves); the cycle
    // after the last pass; the next pass's cycle, as of the last Settle.
    bool settleRows_ = true;
    u16 settling_ = 0;
    u16 blocked_ = 0;
    uint64_t passFloor_ = 0;
    uint64_t nextPass_ = UINT64_MAX;
    // The DMA's own code runs (its memory accesses need no settling); a
    // service runs (an event in it is its own work, not the next one's).
    bool busy_ = false;
    bool servicing_ = false;
    std::vector<u32> watched_;  // as last given to Emulator::WatchPages()
};