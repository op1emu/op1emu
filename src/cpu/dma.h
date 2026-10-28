#pragma once

#include "io.h"
#include "emu.h"
#include <vector>
#include <map>
#include <memory>

class Bus;
class TimeSource;

// Core cycles per element of the PPI's DMA, one element per SCLK cycle: an
// assumed 100 MHz SCLK (CCLK / 4). The PPI is not paced by its peripheral,
// so this is what paces it in guest time. Memory DMA has its own timing
// (DMA::SetMdmaSpeedup).
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
    // The DMA controller's flag NotifyDMA() raises.
    void SetDMANotify(bool* flag) { dmaNotify_ = flag; }

protected:
    void NotifyDMA() const {
        if (dmaNotify_) *dmaNotify_ = true;
    }

private:
    bool* dmaNotify_ = nullptr;
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
class MdmaStream;
class DMA : public RegisterDevice, public PageWatcher {
public:
    // Without a time source no bus is paced (CyclesPerElement is ignored),
    // and memory DMA does not run.
    DMA(u32 baseAddr, Emulator& emu, const TimeSource* time = nullptr);
    ~DMA() override;

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

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void ProcessWithInterrupt(int ivg) override;
    // Acknowledging a channel's IRQ_STATUS changes nothing a service would
    // act on; any other write may (touched_). A read has no effect but where
    // it settles memory DMA (SettlesOnRead): it moved guest memory, and may
    // have ended a row.
    bool HostWorkAfterAccess(u32 offset, bool write) const override {
        return write ? touched_ : SettlesOnRead(offset);
    }
    // The channels' registers change only in a service or a write, but for
    // the current address and counts of a memory DMA channel that settles.
    bool ReadStable(u32 offset) const override { return !SettlesOnRead(offset); }

    // Memory DMA (HRM 6-7, 6-44 to 6-47): each pair of channels (12 and 13,
    // 14 and 15: destination and source) is a stream, granted the buses as
    // one: each of its slots moves an element, read by the source and
    // written by the destination, at a 16-bit word per SCLK between L1 and
    // external memory or per two SCLKs within either (the bus is used twice),
    // a 32-bit element as two words, SCLK five core cycles (the firmware's
    // PLL_DIV); a descriptor fetched as a work unit ends costs an SCLK per
    // 16-bit word (the first, at the CONFIG write, nothing). A stream runs
    // while both its channels do, starting three SCLKs after the CONFIG write
    // that completes the pair, and memory DMA 1 waits while memory DMA 0 runs
    // (fixed priority: DMA_TC_PER's MDMA_ROUND_ROBIN_PERIOD 0, as the firmware
    // sets it). Not modelled: the 8-word FIFO between the two, which lets the
    // source run up to 16 bytes ahead (the firmware's two sides always count
    // the same elements); the latencies after a descriptor fetch; bursts,
    // direction changes, SDRAM pages and the core's own accesses, which slow
    // the hardware down. Channels of different word sizes (illegal) or of
    // the reserved one do not run, nor without a time source. A speedup
    // divides all of it.
    static constexpr unsigned kDefaultMdmaSpeedup = 1;
    // Before any channel runs; 0 counts as 1, the hardware's rate.
    void SetMdmaSpeedup(unsigned speedup) { mdmaSpeedup_ = speedup ? speedup : 1; }
    // Steps the memory DMA streams took: each moves the slots due up to the
    // next one the guest is told of, or one slot (profiling, tests).
    uint64_t MdmaSteps() const { return mdmaSteps_; }

    // Settling (the default; set before any channel runs): memory DMA moves
    // its elements when something could see them -- a service at an element
    // whose move the guest is told of (a row end that interrupts, a
    // transfer's end), a register write, a read of a current address or
    // count, an access to memory its transfer has yet to read or write -- not
    // at each slot's time. Each element moves as a service at its own slot
    // would have moved it, so the transfer is the one a service every cycle
    // makes, and every access sees memory as it would then. For memory the
    // service watches the pages the rest of each transfer lies in
    // (Emulator::WatchPages()), from the service after the write that starts
    // it: until then the block that wrote it sees them as before, as a
    // stream that does not settle shows its first element only at that
    // service. So for the next descriptor of a transfer that ends inside an
    // access: its pages are watched from the service after that block. A
    // service moves memory DMA before the other channels (a PPI or SPORT
    // reading what memory DMA wrote sees it written). Off, every slot is due
    // on its own (the test's reference).
    void SetSettling(bool on) { settles_ = on; }

    // PageWatcher: the elements due by now move first, but where the DMA
    // itself reads or writes memory. A row end the guest is told of (an interrupt,
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
    friend class MdmaStream;
    // Before when no running channel can move data (see DMABus::DMAIdleUntil)
    // or, settling, memory DMA does something the guest is told of.
    uint64_t IdleUntil();
    // Moves the memory DMA elements due by guest cycle `until`.
    void Settle(uint64_t until);
    // A read of the current address or counts of a memory DMA channel that
    // settles.
    bool SettlesOnRead(u32 offset) const;
    // runnable_'s bit for a channel after something may have changed it: a
    // register write, or its own transfer (completion, the next descriptor).
    void Refresh(size_t channel);
    // At the end of a service: the pages the settling streams' transfers
    // have yet to reach, watched (SetSettling()).
    void Watch();

    static constexpr u16 kMdmaChannels = 0xF000;  // 12 to 15

    bool touched_ = true;
    uint64_t idleUntil_ = 0;
    unsigned idleSource_ = 0;
    const TimeSource* time_;
    Emulator& emulator;
    std::array<std::shared_ptr<DMAChannel>, 16> channels;
    std::map<DMAPeripheralType, std::shared_ptr<DMABus>> dmaBuses;
    std::array<DMABus*, 16> busByType_{};
    u16 runnable_ = 0;  // bit n: channel n is enabled and running (Refresh)
    // Settling: the memory DMA channels of runnable_ (with settles_).
    bool settles_ = true;
    u16 settling_ = 0;
    std::array<std::unique_ptr<MdmaStream>, 2> streams_;
    unsigned mdmaSpeedup_ = kDefaultMdmaSpeedup;
    uint64_t mdmaSteps_ = 0;
    // The DMA's own code runs (its memory accesses need no settling); a
    // service runs (an event in it is its own work, not the next one's);
    // Settle runs (a read it makes of a DMA register settles nothing).
    bool busy_ = false;
    bool servicing_ = false;
    bool settlingNow_ = false;
    std::vector<u32> watched_;  // as last given to Emulator::WatchPages()
};