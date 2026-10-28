#include "dma.h"
#include "time_source.h"
#include "utils/log.h"
#include <stdint.h>
#include <algorithm>
#include <cassert>
#include <cstring>

enum DMANextOperation {
    Stop = 0x0,
    Autobuffer = 0x1,
    DescriptorArray = 0x4,
    DescriptorListSmallModel = 0x6,
    DescriptorListLargeModel = 0x7
};

class DMAChannel : public RegisterDevice {
public:
    DMAChannel(const std::string& name, u32 baseAddr, DMA& dma, u16 defaultPeripheralMap);
    bool IsEnabled() const { return enabled; }
    bool IsRunning() const { return running; }
    // What ProcessTransfer() would do next: nothing until the returned guest
    // time (0: it may do something now).
    uint64_t IdleUntil() const;
    // Row and transfer ends so far (profiling: the work a settling service did).
    uint64_t Events() const { return events_; }
    // Appends the pages below the MMR space the rest of its transfer reads
    // or writes: the current row's rest, then the rows after it; all of it
    // where it starts again (autobuffer).
    void Pages(std::vector<u32>& pages) const;
    DMAPeripheralType GetPeripheralType() const { return peripheralType; }

    // At guest cycle `now`, as a service then.
    u32 ProcessTransfer(uint64_t now);

    // Memory DMA (MdmaStream), which moves the elements itself.
    u32 ElementBytes() const { return 1u << wordSize; }
    u8 WordSize() const { return wordSize; }
    // The start of the work unit it is in (memory DMA's timing: the memory
    // the unit lies in).
    u32 UnitStart() const { return unitStart_; }
    u32 CurrAddr() const { return currAddr; }
    u16 RowLeft() const { return currXCount; }
    // Elements up to and including the next whose move the guest is told of:
    // its row's end where rows interrupt (or a 1D transfer's end), else the
    // transfer's end.
    uint64_t ElementsToEvent() const;
    // The bytes the next n elements lie in, [lo, hi), as Step() goes.
    void Span(uint64_t n, int64_t& lo, int64_t& hi) const;
    // The next n elements of the current row (n <= RowLeft()), not stepped over.
    void ReadRow(u8* out, u32 n);
    void WriteRow(const u8* in, u32 n);
    // Past n elements of the current row (n <= RowLeft()): what a transfer
    // of them does to the address and counts, and at the row's end, the next
    // row, interrupts, the transfer's end and the next descriptor.
    void Step(u32 n);
    // 16-bit words of descriptors read so far.
    uint64_t DescriptorWords() const { return descriptorWords_; }

protected:
    void ProcessDescriptor();
    // The bus's pace, 0 when unpaced (see DMABus::CyclesPerElement).
    u32 CyclesPerElement(const DMABus& bus) const;
    // ProcessTransfer() may leave the chunk's elements [from, to) (in bytes)
    // unread: they lie in plain memory.
    bool QuietSource(u32 from, u32 to, u32 elementBytes) const;

    DMA& dma;

    bool enabled = false;
    bool memoryWrite = false;  // memory read/write
    u8 wordSize = 0;           // 8/16/32 bits
    bool mode2D = false;       // 2D/linear
    bool synchronized = false; // continuous/synchronized
    bool mode2DInterruptEachRow = false; // interrupt each row in 2D mode
    bool dataInterruptEnabled = false;
    u8 descriptorSize = 0;     // next descriptor size
    DMANextOperation next = DMANextOperation::Stop; // next operation

    bool completed = false;
    bool error = false;
    bool running = false;

    bool channelIsMemory = false; // peripheral/memory
    DMAPeripheralType peripheralType;

    u32 nextDescPtr = 0;    // 0x00
    u32 startAddr = 0;      // 0x04
    u16 xCount = 0;         // 0x10
    int16_t xModify = 0;   // 0x14
    u16 yCount = 0;         // 0x18
    int16_t yModify = 0;   // 0x1C
    u32 currDescPtr = 0;    // 0x20
    u32 currAddr = 0;       // 0x24
    u16 peripheralMap = 0;  // 0x2C
    u16 currXCount = 0;     // 0x30
    u16 currYCount = 0;     // 0x38
    // A paced bus: the guest cycle from which the next element accrues.
    u64 pacedFrom_ = 0;
    uint64_t events_ = 0;
    u32 unitStart_ = 0;
    uint64_t descriptorWords_ = 0;
};

DMAChannel::DMAChannel(const std::string& name, u32 baseAddr, DMA& dma, u16 defaultPeripheralType)
    : RegisterDevice(name, baseAddr, 0x40) , dma(dma)
    , peripheralType(static_cast<DMAPeripheralType>(defaultPeripheralType))
{
    REG32(NEXT_DESC_PTR, 0x00);
    FIELD(NEXT_DESC_PTR, VAL, 0, 32, R(nextDescPtr), W(nextDescPtr));

    REG32(START_ADDR, 0x04);
    FIELD(START_ADDR, VAL, 0, 32, R(startAddr), W(startAddr));

    REG32(CONFIG, 0x08);
    FIELD(CONFIG, DMAEN, 0, 1, R(enabled), W(enabled));
    FIELD(CONFIG, WNR, 1, 1, R(memoryWrite), W(memoryWrite));
    FIELD(CONFIG, WDSIZE, 2, 2, R(wordSize), W(wordSize));
    FIELD(CONFIG, DMA2D, 4, 1, R(mode2D), W(mode2D));
    FIELD(CONFIG, SYNC, 5, 1, R(synchronized), W(synchronized));
    FIELD(CONFIG, DI_SEL, 6, 1, R(mode2DInterruptEachRow), W(mode2DInterruptEachRow));
    FIELD(CONFIG, DI_EN, 7, 1, R(dataInterruptEnabled), W(dataInterruptEnabled));
    FIELD(CONFIG, NDSIZE, 8, 4, R(descriptorSize), W(descriptorSize));
    FIELD(CONFIG, FLOW, 12, 3, R(next), [this](u32 v) {
        next = (DMANextOperation)v;
    });
    CONFIG.writeCallback = [this](u32 value) {
        running = enabled;
        if (enabled && this->dma.Time()) pacedFrom_ = this->dma.Time()->Cycles();
        ProcessDescriptor();
        // NFC DMA is an unpaced 512-byte controller transfer. Running it only
        // from the global one-channel-per-BB round robin lets the previous
        // WR_DONE interrupt advance the firmware's sector state machine before
        // this newly armed transfer gets a turn; the skipped transfer then
        // arrives after the NAND column has moved into OOB. Complete it at the
        // CONFIG write, matching the hardware DMA/controller handshake.
        if (enabled && peripheralType == DMAPeripheralNFC) {
            ProcessTransfer(this->dma.Now());
        }
    };

    REG32(X_COUNT, 0x10);
    FIELD(X_COUNT, VAL, 0, 16, R(xCount), W(xCount));

    REG32(X_MODIFY, 0x14);
    FIELD(X_MODIFY, VAL, 0, 16, R(xModify), [this](u32 v) {
        xModify = (int16_t)v;
    });

    REG32(Y_COUNT, 0x18);
    FIELD(Y_COUNT, VAL, 0, 16, R(yCount), W(yCount));

    REG32(Y_MODIFY, 0x1C);
    FIELD(Y_MODIFY, VAL, 0, 16, R((u16)yModify), [this](u32 v) {
        yModify = (int16_t)v;
    });

    REG32(CURR_DESC_PTR, 0x20);
    FIELD(CURR_DESC_PTR, VAL, 0, 32, R(currDescPtr), W(currDescPtr));

    REG32(CURR_ADDR, 0x24);
    FIELD(CURR_ADDR, VAL, 0, 32, R(currAddr), W(currAddr));

    REG32(IRQ_STATUS, 0x28);
    FIELD(IRQ_STATUS, DMA_DONE, 0, 1, R(completed), [this](u32 v) {
        if (v) {
            completed = false;
            TriggerInterrupt(0);
        }
    });
    FIELD(IRQ_STATUS, DMA_ERR, 1, 1, R(error), W1C(error));
    FIELD(IRQ_STATUS, DMA_RUN, 3, 1, R(running), N());

    REG32(PERIPHERAL_MAP, 0x2C);
    FIELD(PERIPHERAL_MAP, CTYPE, 6, 1, R(channelIsMemory), N());
    FIELD(PERIPHERAL_MAP, PMAP, 12, 4, R(peripheralType), [this](u32 v) {
        peripheralType = (DMAPeripheralType)v;
    });

    REG32(CURR_X_COUNT, 0x30);
    FIELD(CURR_X_COUNT, VAL, 0, 16, R(currXCount), W(currXCount));

    REG32(CURR_Y_COUNT, 0x38);
    FIELD(CURR_Y_COUNT, VAL, 0, 16, R(currYCount), W(currYCount));
}

void DMAChannel::ProcessDescriptor()
{
    if (!enabled) return;

    int elementBytes = 1 << wordSize;

    // Address alignment check
    if (startAddr & (elementBytes - 1)) {
        error = true;
        return;
    }

    const bool largeList = descriptorSize && next == DMANextOperation::DescriptorListLargeModel;
    const u32 descriptorEnd = nextDescPtr + 2u * descriptorSize;
    if (descriptorSize) {
        u32 offset = 0;
        u16 flows[9];
        if (next == DMANextOperation::DescriptorArray) {
            offset = 0x04;
            dma.GetEmulator().MemoryRead(currDescPtr, flows + offset, descriptorSize * sizeof(u16));
        } else if (next == DMANextOperation::DescriptorListSmallModel) {
            offset = 0x02;
            // RegisterDevice do not support unaligned read, so read into u32 first
            *(u32*)flows = Read32(0x00);
            dma.GetEmulator().MemoryRead(nextDescPtr, flows + offset, descriptorSize * sizeof(u16));
        } else if (next == DMANextOperation::DescriptorListLargeModel) {
            offset = 0x00;
            dma.GetEmulator().MemoryRead(nextDescPtr, flows + offset, descriptorSize * sizeof(u16));
        }
        Write(0x00, flows, descriptorSize * sizeof(u16));
        descriptorWords_ += descriptorSize;
    }

    // BF52x HRM, DMA Current Descriptor Pointer Registers (6-87): after
    // fetching a descriptor, CURR_DESC_PTR addresses the next halfword.
    // NEXT_DESC_PTR now holds the next link, not the descriptor just fetched.
    currDescPtr = largeList ? descriptorEnd : nextDescPtr;
    currAddr = startAddr;
    unitStart_ = startAddr;
    currXCount = xCount ?: 0xFFFF;
    currYCount = yCount ?: 0xFFFF;
}

u32 DMAChannel::CyclesPerElement(const DMABus& bus) const {
    return dma.Time() ? bus.CyclesPerElement() : 0;
}

uint64_t DMAChannel::IdleUntil() const {
    if (!enabled || !running) return UINT64_MAX;
    // ProcessTransfer() logs a missing bus on every call.
    const DMABus* bus = dma.GetDMABus(peripheralType);
    if (!bus) return 0;
    // The length ProcessTransfer() offers the bus.
    const u32 elementBytes = 1u << wordSize;
    const u32 length = std::min(u32(currXCount) * elementBytes, 4096u);
    const uint64_t idle = bus->DMAIdleUntil(memoryWrite, length);
    const u32 pace = CyclesPerElement(*bus);
    if (!pace) return idle;
    // Guest nanoseconds are 2.5 per cycle, rounded down: cycles >= c exactly
    // when nanoseconds >= c * 5 / 2.
    const uint64_t dueCycles = pacedFrom_ + uint64_t(length / elementBytes) * pace;
    return std::max(idle, dueCycles * 5 / 2);
}

bool DMAChannel::QuietSource(u32 from, u32 to, u32 elementBytes) const {
    // The elements [from, to) of the chunk at currAddr: every page they lie
    // in is plain memory, one MemoryDevice mapping all of it (a direct page,
    // Emulator::DirectPages()), so leaving them unread changes nothing.
    if (from >= to) return true;
    const int64_t first = int64_t(currAddr) + int64_t(from / elementBytes) * xModify;
    const int64_t last = int64_t(currAddr) + int64_t((to - elementBytes) / elementBytes) * xModify;
    const int64_t lo = std::min(first, last), hi = std::max(first, last) + elementBytes - 1;
    if (lo < 0 || hi > int64_t(UINT32_MAX)) return false;
    u8* const* pages = dma.GetEmulator().DirectPages();
    for (int64_t page = lo >> PAGE_BITS; page <= hi >> PAGE_BITS; ++page)
        if (!pages[page]) return false;
    return true;
}

void DMAChannel::Pages(std::vector<u32>& pages) const {
    if (!currXCount) return;
    int64_t lo = currAddr, hi = currAddr;
    const auto row = [&](int64_t start, u32 elements) {
        const int64_t end = start + int64_t(elements - 1) * xModify;
        lo = std::min({lo, start, end});
        hi = std::max({hi, start, end});
    };
    row(currAddr, currXCount);
    // As ProcessTransfer() steps: a row's last element, then yModify.
    const int64_t stride = int64_t(xCount - 1) * xModify + yModify;
    if (mode2D && currYCount > 1 && xCount) {
        const int64_t second = int64_t(currAddr) + int64_t(currXCount - 1) * xModify + yModify;
        row(second, xCount);
        row(second + int64_t(currYCount - 2) * stride, xCount);
    }
    if (next == DMANextOperation::Autobuffer && xCount) {
        row(startAddr, xCount);
        if (mode2D && yCount > 1) row(int64_t(startAddr) + int64_t(yCount - 1) * stride, xCount);
    }
    hi += (1 << wordSize) - 1;
    lo = std::max<int64_t>(lo, 0);
    hi = std::min<int64_t>(hi, 0xFFBFFFFF);
    for (int64_t page = lo >> PAGE_BITS; page <= hi >> PAGE_BITS; ++page) pages.push_back(u32(page));
}

// Returns the number of bytes transferred in this call (0 if the channel is
// idle, unattached, or blocked). Not for memory DMA's channels, which their
// stream moves (MdmaStream).
u32 DMAChannel::ProcessTransfer(uint64_t now) {
    if (!enabled || !running) return 0;

    DMABus* bus = dma.GetDMABus(peripheralType);
    if (!bus) {
        LogWarn("DMA channel %s: No DMA bus attached for peripheral type %d", Name().c_str(), peripheralType);
        return 0;
    }

    int elementBytes = 1 << wordSize;
    u32 totalBytes = currXCount * elementBytes;

    u8 buffer[4096];
    totalBytes = std::min(totalBytes, (u32)sizeof(buffer));
    // A paced bus moves this chunk once its elements' time has passed.
    const u32 pace = CyclesPerElement(*bus);
    if (pace && now < pacedFrom_ + uint64_t(totalBytes / elementBytes) * pace) return 0;
    if (memoryWrite) {
        totalBytes = bus->DMARead(xCount - currXCount, yCount - currYCount, buffer, totalBytes);
        totalBytes -= totalBytes % elementBytes; // only write whole elements
        if (xModify == elementBytes) {
            dma.GetEmulator().MemoryWrite(currAddr, buffer, totalBytes);
        } else {
            u32 addr = currAddr;
            for (u32 i = 0; i < totalBytes; i += elementBytes) {
                dma.GetEmulator().MemoryWrite(addr, buffer + i, elementBytes);
                addr += xModify;
            }
        }
    } else {
        // Memory read (memory to peripheral): only the elements the bus will
        // take now (DMABus::DMAWriteTake), where the rest would be read from
        // plain memory, which no read changes. The bus is still offered the
        // whole chunk, as its own accounting may depend on the length.
        u32 take = totalBytes;
        const u32 bound = bus->DMAWriteTake(totalBytes);
        if (bound < totalBytes) {
            const u32 rounded = (bound + elementBytes - 1) / elementBytes * elementBytes;
            if (QuietSource(rounded, totalBytes, elementBytes)) take = rounded;
        }
        if (xModify == elementBytes) {
            dma.GetEmulator().MemoryRead(currAddr, buffer, take);
        } else {
            u32 addr = currAddr;
            for (u32 i = 0; i < take; i += elementBytes) {
                dma.GetEmulator().MemoryRead(addr, buffer + i, elementBytes);
                addr += xModify;
            }
        }
        totalBytes = bus->DMAWrite(xCount - currXCount, yCount - currYCount, buffer, totalBytes);
        assert(totalBytes <= take);
        totalBytes -= totalBytes % elementBytes; // only count whole elements accepted
    }

    u32 count = totalBytes / elementBytes;
    pacedFrom_ += uint64_t(count) * pace;
    Step(count);
    return count * elementBytes;
}

void DMAChannel::Step(u32 count) {
    currAddr += count * xModify;
    currXCount -= count;

    if (currXCount == 0) {
        bool transferComplete = true;
        if (mode2D) {
            currYCount--;
            if (currYCount > 0) {
                currXCount = xCount;
                currAddr = currAddr - xModify + yModify;
                transferComplete = false;
            }
        }
        if (dataInterruptEnabled) {
            if (!mode2D || mode2DInterruptEachRow) {
                TriggerInterrupt(1);
                ++events_;
            } else if (currYCount == 0) {
                TriggerInterrupt(1);
                ++events_;
            }
        }
        if (transferComplete) {
            ++events_;
            completed = true;
            running = false;
            if (next != DMANextOperation::Stop) {
                running = true;
                ProcessDescriptor();
            }
        }
    }
}

uint64_t DMAChannel::ElementsToEvent() const {
    if (!mode2D || (dataInterruptEnabled && mode2DInterruptEachRow)) return currXCount;
    // Step() counts a CURR_Y_COUNT of 0 down through 65535.
    const uint64_t rows = currYCount ? currYCount : 65536;
    return currXCount + (rows - 1) * xCount;
}

void DMAChannel::Span(uint64_t n, int64_t& lo, int64_t& hi) const {
    const int64_t elementBytes = int64_t(1) << wordSize;
    lo = INT64_MAX;
    hi = INT64_MIN;
    // A row's elements are affine in their index and the rows' starts in
    // theirs, so the first and the last of each say where they all lie.
    const auto row = [&](int64_t start, uint64_t count) {
        if (!count) return;
        const int64_t end = start + int64_t(count - 1) * xModify;
        lo = std::min({lo, start, end});
        hi = std::max({hi, start + elementBytes, end + elementBytes});
    };
    const uint64_t first = std::min<uint64_t>(n, currXCount);
    row(currAddr, first);
    if (n == first || !xCount) return;
    // As Step() goes: a row's last element, then yModify.
    const int64_t next = int64_t(currAddr) + int64_t(currXCount - 1) * xModify + yModify;
    const int64_t stride = int64_t(xCount - 1) * xModify + yModify;
    const uint64_t rest = n - first, full = rest / xCount, part = rest % xCount;
    if (full) {
        row(next, xCount);
        row(next + int64_t(full - 1) * stride, xCount);
    }
    row(next + int64_t(full) * stride, part);
}

void DMAChannel::ReadRow(u8* out, u32 n) {
    const u32 elementBytes = 1u << wordSize;
    if (xModify == int16_t(elementBytes)) {
        dma.GetEmulator().MemoryRead(currAddr, out, n * elementBytes);
        return;
    }
    if (xModify == 0 && n) {
        // The same element n times (the boot ROM's fills): the memory DMA's
        // reads of plain memory change nothing (MdmaStream::Move).
        dma.GetEmulator().MemoryRead(currAddr, out, elementBytes);
        for (u32 i = 1; i < n; ++i) std::memcpy(out + i * elementBytes, out, elementBytes);
        return;
    }
    u32 addr = currAddr;
    for (u32 i = 0; i < n; ++i, addr += xModify) dma.GetEmulator().MemoryRead(addr, out + i * elementBytes, elementBytes);
}

void DMAChannel::WriteRow(const u8* in, u32 n) {
    const u32 elementBytes = 1u << wordSize;
    if (xModify == int16_t(elementBytes)) {
        dma.GetEmulator().MemoryWrite(currAddr, in, n * elementBytes);
        return;
    }
    u32 addr = currAddr;
    for (u32 i = 0; i < n; ++i, addr += xModify) dma.GetEmulator().MemoryWrite(addr, in + i * elementBytes, elementBytes);
}

// A memory DMA stream (DMA::SetMdmaSpeedup): a destination and a source
// channel, each slot an element from the one to the other. Its time is kept
// in ticks, guest cycles times the speedup, so that a slot of n SCLK cycles
// takes n * kCyclesPerSclk ticks at any speedup and slots add up exactly; a
// slot is at the first guest cycle at or past its tick.
class MdmaStream {
public:
    MdmaStream(DMA& dma, size_t dst, size_t src) : dma_(dma), dstIndex_(dst), srcIndex_(src) {}

    // Both channels enabled and running: the stream holds the buses
    // (fixed priority), also while it waits to start again.
    bool Busy() const {
        const DMAChannel &d = Dst(), &s = Src();
        return d.IsEnabled() && d.IsRunning() && s.IsEnabled() && s.IsRunning();
    }
    // Busy, started, at rows with elements left, of one legal word size.
    bool Runnable() {
        const DMAChannel &d = Dst(), &s = Src();
        if (rearmed_ || !Busy() || !d.RowLeft() || !s.RowLeft()) return false;
        if (d.WordSize() == s.WordSize() && d.WordSize() < 3) return true;
        if (!warned_)
            LogWarn("Memory DMA %zu: word sizes %u and %u (they must match, 3 is reserved); it does not run",
                    dstIndex_ / 2 - 6, 8u << d.WordSize(), 8u << s.WordSize());
        warned_ = true;
        return false;
    }
    // A register write to one of its channels at guest cycle `now`: to the
    // destination's (dst) or the source's, whether it ran before, and
    // whether it was to a CONFIG. The pair starts three SCLKs after the
    // CONFIG write that completes it, the destination's as the HRM has the
    // source written first (6-8, 6-47): a source armed again while the pair
    // runs waits for the destination's (the firmware arms both again for its
    // next fill before the last ends); a CONFIG write to the destination
    // starts it again. Armed again the other way round, destination first, a
    // running pair waits for another destination write.
    void Written(uint64_t now, bool dst, bool wasRunnable, bool config) {
        if (!config) return;
        if (dst) rearmed_ = false;
        else if (wasRunnable) rearmed_ = true;
        if (Runnable()) last_ = now * Speedup() + 3 * TimeSource::kCyclesPerSclk;
    }
    // Memory DMA 1 while memory DMA 0 is Busy(): no slots, then again from
    // guest cycle `at`.
    void Hold() { held_ = true; }
    void Release(uint64_t at) {
        held_ = false;
        last_ = std::max(last_, at * Speedup());
    }
    bool Held() const { return held_; }
    // The guest cycle of its last slot.
    uint64_t LastCycle() const { return (last_ + Speedup() - 1) / Speedup(); }
    // The guest cycle of the slot `slots` from now (1: the next one), or
    // UINT64_MAX while it does not run.
    uint64_t SlotCycle(uint64_t slots) {
        if (held_ || !Runnable()) return UINT64_MAX;
        return (last_ + slots * SlotTicks() + Speedup() - 1) / Speedup();
    }
    // The slot of the next element whose move the guest is told of (1: the
    // next one).
    uint64_t SlotsToEvent() const { return std::min(Src().ElementsToEvent(), Dst().ElementsToEvent()); }
    // The slots due by guest cycle `until`: in a step up to the next one the
    // guest is told of (batch), else one per step.
    void Settle(uint64_t until, bool batch);
    DMAPeripheralType Type() const { return Dst().GetPeripheralType(); }

private:
    DMAChannel& Dst() const { return *dma_.channels[dstIndex_]; }
    DMAChannel& Src() const { return *dma_.channels[srcIndex_]; }
    uint64_t Speedup() const { return dma_.mdmaSpeedup_; }
    // A 16-bit word per SCLK, per two within L1 or external memory; a
    // 32-bit element is two words.
    uint64_t SlotTicks() const {
        const auto inL1 = [](u32 addr) { return addr >= 0xFF800000 && addr < 0xFFC00000; };
        const uint64_t words = Src().ElementBytes() == 4 ? 2 : 1;
        const bool shared = inL1(Src().UnitStart()) == inL1(Dst().UnitStart());
        return words * (shared ? 2 : 1) * TimeSource::kCyclesPerSclk;
    }
    // n elements, as n slots would move them; the last may end something.
    void Move(uint64_t n);
    void MoveOne() {
        u8 element[4] = {};  // what unmapped memory reads as
        Src().ReadRow(element, 1);
        Src().Step(1);
        Dst().WriteRow(element, 1);
        Dst().Step(1);
    }

    DMA& dma_;
    size_t dstIndex_, srcIndex_;
    uint64_t last_ = 0;  // the last slot's tick, or the start's
    bool held_ = false;
    bool rearmed_ = false;  // the source armed again: waits for the destination
    bool warned_ = false;
    std::vector<u8> buffer_;
};

// At most this many elements move in one Move(): the buffer's size.
static constexpr uint64_t kMdmaChunk = 4096;

void MdmaStream::Settle(uint64_t until, bool batch) {
    while (!held_ && Runnable()) {
        const uint64_t slot = SlotTicks(), now = until * Speedup();
        if (now < last_ + slot) return;
        const uint64_t due = (now - last_) / slot;
        const uint64_t n = batch ? std::min({due, SlotsToEvent(), kMdmaChunk}) : 1;
        DMAChannel &d = Dst(), &s = Src();
        const uint64_t events[2] = {d.Events(), s.Events()};
        const uint64_t words = d.DescriptorWords() + s.DescriptorWords();
        Move(n);
        last_ += n * slot;
        // A descriptor fetched at the last slot: a DMA memory cycle per word.
        last_ += (d.DescriptorWords() + s.DescriptorWords() - words) * TimeSource::kCyclesPerSclk;
        ++dma_.mdmaSteps_;
        for (size_t side = 0; side < 2; ++side) {
            const uint64_t made = (side ? s : d).Events() - events[side];
            // Outside a service, an event is the next one's work.
            if (made && !dma_.servicing_) dma_.touched_ = true;
            dma_.CountServiceWork(made);
            dma_.Refresh(side ? srcIndex_ : dstIndex_);
        }
    }
}

void MdmaStream::Move(uint64_t n) {
    DMAChannel &d = Dst(), &s = Src();
    // The element that ends something moves on its own, after the rest, as
    // at its own slot: the source's end (its next descriptor) comes before
    // the destination writes it.
    const bool last = n > 1 && (n == s.ElementsToEvent() || n == d.ElementsToEvent());
    const uint64_t bulk = last ? n - 1 : n;
    // The rest in two sweeps, every read, then every write, which is what
    // the slots do where no write lands on a later read, and below the MMRs
    // (whose reads may act), the addresses not wrapping around into them.
    int64_t readLo, readHi, writeLo, writeHi;
    s.Span(bulk, readLo, readHi);
    d.Span(bulk, writeLo, writeHi);
    const bool apart = writeHi <= readLo || readHi <= writeLo;
    const auto plain = [](int64_t lo, int64_t hi) { return lo >= 0 && hi <= 0xFFC00000; };  // no wrap either
    if (bulk > 1 && apart && plain(readLo, readHi) && plain(writeLo, writeHi)) {
        const u32 bytes = s.ElementBytes();
        buffer_.assign(bulk * bytes, 0);  // what unmapped memory reads as
        u8* out = buffer_.data();
        // A row read again (the synth's fill: the same two words each row)
        // is the row just read: nothing wrote memory since.
        const u8* previous = nullptr;
        u32 previousAddr = 0, previousCount = 0;
        for (uint64_t left = bulk; left;) {
            const u32 count = u32(std::min<uint64_t>(left, s.RowLeft()));
            if (previous && s.CurrAddr() == previousAddr && count == previousCount) std::memcpy(out, previous, count * bytes);
            else s.ReadRow(out, count);
            previous = out, previousAddr = s.CurrAddr(), previousCount = count;
            s.Step(count);
            out += count * bytes;
            left -= count;
        }
        const u8* in = buffer_.data();
        for (uint64_t left = bulk; left;) {
            const u32 count = u32(std::min<uint64_t>(left, d.RowLeft()));
            d.WriteRow(in, count);
            d.Step(count);
            in += count * bytes;
            left -= count;
        }
    } else {
        for (uint64_t i = 0; i < bulk; ++i) MoveOne();
    }
    if (last) MoveOne();
}

DMA::DMA(u32 baseAddr, Emulator& emu, const TimeSource* time)
    : RegisterDevice("DMA", baseAddr, 0x400), time_(time), emulator(emu)
{
    for (size_t i = 0; i < channels.size(); i++) {
        channels[i] = std::make_shared<DMAChannel>("DMAChannel" + std::to_string(i), baseAddr + i * 0x40, *this, i);
    }
    // Hardwired (HRM 6-7): memory DMA 0 is channels 12 (destination) and
    // 13 (source), memory DMA 1 channels 14 and 15.
    streams_[0] = std::make_unique<MdmaStream>(*this, 12, 13);
    streams_[1] = std::make_unique<MdmaStream>(*this, 14, 15);
}

DMA::~DMA() = default;

namespace {
// The DMA's own code runs for the scope (DMA::busy_, servicing_).
class Raise {
public:
    explicit Raise(bool& flag) : flag_(flag), was_(flag) { flag = true; }
    ~Raise() { flag_ = was_; }

private:
    bool& flag_;
    bool was_;
};
}  // namespace

// Offset of a channel's IRQ_STATUS: DMA_DONE and DMA_ERR are W1C (the level
// is published here), DMA_RUN is read-only. Neither what a channel moves nor
// DMA::IdleUntil() depends on them.
static constexpr u32 kIrqStatus = 0x28;

void DMA::Write32(u32 offset, u32 value) {
    Raise busy(busy_);
    if (offset % 0x40 != kIrqStatus) {
        // The elements due by now move first, as they would have before it.
        if (settling_) Settle(Now());
        touched_ = true;
    }
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        MdmaStream* stream = channelIndex >= 12 ? streams_[(channelIndex - 12) / 2].get() : nullptr;
        const bool was = stream && stream->Runnable();
        channels[channelIndex]->Write32(offset % 0x40, value);
        Refresh(channelIndex);
        if (stream) {
            stream->Written(Now(), channelIndex % 2 == 0, was, offset % 0x40 == 0x08);
            // Memory DMA 1 runs while memory DMA 0 does not.
            MdmaStream& one = *streams_[1];
            const bool zero = streams_[0]->Busy();
            if (zero && !one.Held()) one.Hold();
            else if (!zero && one.Held()) one.Release(Now());
        }
    }
}

void DMA::Refresh(size_t channel) {
    const u16 bit = u16(1u << channel);
    if (channels[channel]->IsEnabled() && channels[channel]->IsRunning()) runnable_ |= bit;
    else runnable_ &= u16(~bit);
    if ((runnable_ & bit & kMdmaChannels) && settles_) settling_ |= bit;
    else settling_ &= u16(~bit);
}

uint64_t DMA::Now() const { return time_ ? time_->Cycles() : 0; }

// CURR_ADDR, CURR_X_COUNT, CURR_Y_COUNT: what a settling stream changes at
// an element it moves late. Its other registers change only at its events,
// which a service makes when they are due (IdleUntil()).
bool DMA::SettlesOnRead(u32 offset) const {
    const u32 index = offset / 0x40, reg = offset % 0x40;
    return index < channels.size() && (settling_ >> index & 1) && (reg == 0x24 || reg == 0x30 || reg == 0x38);
}

u32 DMA::Read32(u32 offset) {
    // The elements due by now, as for a write. An event they reach is
    // published inside the read, as levels an access changes are; the run
    // stops after it (HostWorkAfterAccess).
    if (SettlesOnRead(offset)) Settle(Now());
    Raise busy(busy_);
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        return channels[channelIndex]->Read32(offset % 0x40);
    }
    return 0;
}

// Only the channels that may move data (runnable_): a channel that is not
// enabled and running moves nothing and is never due. Memory DMA: its next
// slot, or settling, the next one the guest is told of.
uint64_t DMA::IdleUntil() {
    uint64_t until = UINT64_MAX;
    for (u32 mask = runnable_ & ~kMdmaChannels; mask; mask &= mask - 1) {
        const auto& channel = channels[__builtin_ctz(mask)];
        const uint64_t idle = channel->IdleUntil();
        if (idle < until) until = idle, idleSource_ = channel->GetPeripheralType();
    }
    for (auto& stream : streams_) {
        const uint64_t cycles = stream->SlotCycle(settles_ ? stream->SlotsToEvent() : 1);
        if (cycles == UINT64_MAX) continue;
        if (cycles * 5 / 2 < until) until = cycles * 5 / 2, idleSource_ = stream->Type();
    }
    return until;
}

// Memory DMA 0 first, then memory DMA 1 from where memory DMA 0 stopped
// running, if it did.
void DMA::Settle(uint64_t until) {
    if (settlingNow_) return;
    Raise busy(busy_), settling(settlingNow_);
    MdmaStream &zero = *streams_[0], &one = *streams_[1];
    zero.Settle(until, settles_);
    if (one.Held() && !zero.Busy()) one.Release(zero.LastCycle());
    one.Settle(until, settles_);
}

void DMA::BeforeAccess() {
    if (!busy_ && settling_) Settle(Now());
}

void DMA::Watch() {
    std::vector<u32> pages;
    for (u32 mask = settling_; mask; mask &= mask - 1) channels[__builtin_ctz(mask)]->Pages(pages);
    std::sort(pages.begin(), pages.end());
    pages.erase(std::unique(pages.begin(), pages.end()), pages.end());
    if (pages == watched_) return;
    watched_ = pages;
    emulator.WatchPages(this, std::move(pages));
}

void DMA::ProcessWithInterrupt(int ivg) {
    Raise busy(busy_), servicing(servicing_);
    // Cleared first: a notice raised while the channels run is for the next call.
    touched_ = false;
    const uint64_t now = Now();
    // Memory DMA first: what it wrote is in memory for the others. The rest
    // in channel order, as a pass over all of them would; a transfer
    // changes only its own channel.
    Settle(now);
    for (u32 mask = runnable_ & ~kMdmaChannels; mask; mask &= mask - 1) {
        const size_t index = __builtin_ctz(mask);
        CountServiceWork(channels[index]->ProcessTransfer(now));
        Refresh(index);
    }
    idleUntil_ = IdleUntil();
    Watch();
}

void DMA::BindInterrupt(int channel, int q, InterruptHandler callback) {
    if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        channels[channel]->BindInterrupt(q, callback);
    }
}