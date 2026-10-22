#include "dma.h"
#include "time_source.h"
#include "utils/log.h"
#include <stdint.h>
#include <algorithm>
#include <cassert>

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
    // Whether its rows may move late (DMA::Settle): paced, on a bus that
    // Settles(), with counts EventCycles() can go by.
    bool Settles() const;
    // The guest cycle from which ProcessTransfer() moves the chunk it is at.
    uint64_t DueCycles() const;
    // No earlier than this guest cycle does the channel next do something
    // the guest is told of: end a row that interrupts, or its transfer (its
    // status changes; a descriptor or the next transfer starts). Each of the
    // elements before it takes its time from pacedFrom_.
    uint64_t EventCycles() const;
    // Row and transfer ends so far (profiling: the work a settling service did).
    uint64_t Events() const { return events_; }
    // The bytes it moves up to the end EventCycles() bounds.
    uint64_t EventBytes() const;
    // No earlier than this guest cycle has it moved `bytes` more (paced).
    uint64_t CyclesToMove(uint64_t bytes) const;
    bool ToMemory() const { return memoryWrite; }
    // Appends the pages below the MMR space the rest of its transfer reads
    // or writes: the current row's rest, then the rows after it; all of it
    // where it starts again (autobuffer).
    void Pages(std::vector<u32>& pages) const;
    bool IsMDMA() const;
    bool IsMDMASource() const;
    DMAPeripheralType GetPeripheralType() const { return peripheralType; }

    // At guest cycle `now`, as a service then.
    u32 ProcessTransfer(uint64_t now);

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
        if (enabled && IsMDMASource()) {
            // A new MDMA transfer starts from an empty FIFO.
            if (auto* bus = this->dma.GetDMABus(peripheralType)) bus->DMAFlush();
        }
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
    }

    // BF52x HRM, DMA Current Descriptor Pointer Registers (6-87): after
    // fetching a descriptor, CURR_DESC_PTR addresses the next halfword.
    // NEXT_DESC_PTR now holds the next link, not the descriptor just fetched.
    currDescPtr = largeList ? descriptorEnd : nextDescPtr;
    currAddr = startAddr;
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
    // when nanoseconds >= c * 5 / 2 (in wall mode about then: at worst the
    // DMA is called before its row is due, and moves nothing).
    const uint64_t dueCycles = pacedFrom_ + uint64_t(length / elementBytes) * pace;
    return std::max(idle, dueCycles * 5 / 2);
}

bool DMAChannel::Settles() const {
    const DMABus* bus = dma.GetDMABus(peripheralType);
    return dma.Settling() && bus && bus->Settles() && CyclesPerElement(*bus) && xCount && (!mode2D || yCount);
}

// DueCycles(), EventCycles(): of a channel that Settles(), so its bus is
// there (DMA::Refresh runs after every PERIPHERAL_MAP write).
uint64_t DMAChannel::DueCycles() const {
    // ProcessTransfer()'s chunk and pace check.
    const u32 elementBytes = 1u << wordSize;
    const u32 length = std::min(u32(currXCount) * elementBytes, 4096u);
    return pacedFrom_ + uint64_t(length / elementBytes) * CyclesPerElement(*dma.GetDMABus(peripheralType));
}

uint64_t DMAChannel::EventBytes() const {
    // A 2D transfer that interrupts only at its end tells of no row before
    // its last.
    const bool rowsSilent = mode2D && !(dataInterruptEnabled && mode2DInterruptEachRow);
    const uint64_t elements = currXCount + (rowsSilent && currYCount ? uint64_t(currYCount - 1) * xCount : 0);
    return elements << wordSize;
}

uint64_t DMAChannel::CyclesToMove(uint64_t bytes) const {
    // Moving n elements needs n paces from pacedFrom_ (each chunk waits for
    // its own), however the rows split into chunks.
    const u32 elementBytes = 1u << wordSize;
    return pacedFrom_ + (bytes + elementBytes - 1) / elementBytes * CyclesPerElement(*dma.GetDMABus(peripheralType));
}

uint64_t DMAChannel::EventCycles() const { return CyclesToMove(EventBytes()); }

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

bool DMAChannel::IsMDMA() const {
    return peripheralType >= DMAPeripheralMDMADest0 && peripheralType <= DMAPeripheralMDMASrc1;
}

bool DMAChannel::IsMDMASource() const {
    return peripheralType == DMAPeripheralMDMASrc0 || peripheralType == DMAPeripheralMDMASrc1;
}

// Returns the number of bytes transferred in this call (0 if the channel is
// idle, unattached, or blocked). MDMA channels are ordinary DMA channels
// bound to a MemorySrcDMABus/MemoryDestDMABus pair (see dma.h) rather than a
// special-cased transfer path.
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

    return count * elementBytes;
}

u32 MemorySrcDMABus::DMAWriteTake(u32 length) const {
    // DMAWrite's room, without its rewind and compaction.
    if (head == tail) return std::min(length, CAPACITY);
    if (head > 0 && length > CAPACITY - tail) return std::min(length, CAPACITY - (tail - head));
    return std::min(length, CAPACITY - tail);
}

u32 MemorySrcDMABus::DMAWrite(int x, int y, const void* source, u32 length) {
    if (head == tail) {
        head = tail = 0;                                 // empty queue: rewind to the front
    } else if (head > 0 && length > CAPACITY - tail) {
        std::copy(buffer + head, buffer + tail, buffer); // compact to make room at the tail
        tail -= head;
        head = 0;
    }
    length = std::min(length, CAPACITY - tail);
    auto src = static_cast<const u8*>(source);
    std::copy(src, src + length, buffer + tail);
    tail += length;
    return length;
}

u32 MemorySrcDMABus::DMARead(int x, int y, void* dest, u32 length) {
    length = std::min(length, tail - head);
    std::copy(buffer + head, buffer + head + length, static_cast<u8*>(dest));
    head += length;
    if (head == tail) head = tail = 0;
    return length;
}

u32 MemoryDestDMABus::DMARead(int x, int y, void* dest, u32 length) {
    return source.DMARead(x, y, dest, length);
}

u32 MemoryDestDMABus::DMAWriteTake(u32 length) const { return source.DMAWriteTake(length); }

u32 MemoryDestDMABus::DMAWrite(int x, int y, const void* src, u32 length) {
    return source.DMAWrite(x, y, src, length);
}

DMA::DMA(u32 baseAddr, Emulator& emu, const TimeSource* time)
    : RegisterDevice("DMA", baseAddr, 0x400), time_(time), emulator(emu)
{
    for (size_t i = 0; i < channels.size(); i++) {
        channels[i] = std::make_shared<DMAChannel>("DMAChannel" + std::to_string(i), baseAddr + i * 0x40, *this, i);
    }
}

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
        // The rows due by now move first, as they would have before it.
        if (settling_) Settle(Now());
        blocked_ = 0;
        touched_ = true;
    }
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        channels[channelIndex]->Write32(offset % 0x40, value);
        Refresh(channelIndex);
    }
}

void DMA::Refresh(size_t channel) {
    const u16 bit = u16(1u << channel);
    if (channels[channel]->IsEnabled() && channels[channel]->IsRunning()) runnable_ |= bit;
    else runnable_ &= u16(~bit);
    if ((runnable_ & bit) && channels[channel]->Settles()) settling_ |= bit;
    else settling_ &= u16(~bit);
}

uint64_t DMA::Now() const { return time_ ? time_->Cycles() : 0; }

// CURR_ADDR, CURR_X_COUNT, CURR_Y_COUNT: what a settling channel changes
// at a row it moves late. Its other registers change only at its events,
// which a service makes when they are due (IdleUntil()).
bool DMA::SettlesOnRead(u32 offset) const {
    const u32 index = offset / 0x40, reg = offset % 0x40;
    return index < channels.size() && (settling_ >> index & 1) && (reg == 0x24 || reg == 0x30 || reg == 0x38);
}

u32 DMA::Read32(u32 offset) {
    // The rows due by now, as for a write. An event they reach (a channel
    // that waited past its bound for the FIFO's other side) is published
    // inside the read, as levels an access changes are; the run stops after
    // it (HostWorkAfterAccess).
    if (SettlesOnRead(offset)) Settle(Now());
    Raise busy(busy_);
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        return channels[channelIndex]->Read32(offset % 0x40);
    }
    return 0;
}

// MDMA channels aren't rate-limited by a peripheral, so pump them until their
// internal FIFO blocks (source full / destination empty). Every byte
// returned necessarily moved in or out of the FIFO, so the loop is naturally
// bounded by FIFO capacity; MDMA_BURST_BYTES is just an extra guard against
// FLOW=Autobuffer auto-restarting the transfer indefinitely.
static constexpr u32 MDMA_BURST_BYTES = 4096;

// Only the channels that may move data (runnable_): a channel that is not
// enabled and running moves nothing and is never due.
uint64_t DMA::IdleUntil() {
    uint64_t until = UINT64_MAX;
    const uint64_t now = Now();
    for (u32 mask = runnable_; mask; mask &= mask - 1) {
        const size_t index = __builtin_ctz(mask);
        const auto& channel = channels[index];
        uint64_t idle;
        if (settling_ >> index & 1) {
            const uint64_t cycles = EventBound(index, now);
            idle = cycles == UINT64_MAX ? UINT64_MAX : cycles * 5 / 2;
        } else {
            idle = channel->IdleUntil();
        }
        if (idle < until) until = idle, idleSource_ = channel->GetPeripheralType();
    }
    return until;
}

// One channel's turn in a pass at guest cycle `now`.
static u32 Pump(DMAChannel& channel, uint64_t now) {
    if (!channel.IsMDMA()) return channel.ProcessTransfer(now);
    u32 total = 0;
    while (total < MDMA_BURST_BYTES) {
        u32 moved = channel.ProcessTransfer(now);
        if (!moved) break;
        total += moved;
    }
    return total;
}

uint64_t DMA::EventBound(size_t index, uint64_t now) const {
    const DMAChannel& channel = *channels[index];
    uint64_t bound = channel.EventCycles();
    // What its FIFO lacks for it (bytes for the destination, room for the
    // source) the other side has to move first, at its own pace; a pass
    // runs the lower channel first, which then moves at the next one.
    const DMABus& bus = *GetDMABus(channel.GetPeripheralType());
    const uint64_t bytes = channel.EventBytes();
    const uint64_t ready = channel.ToMemory() ? bus.DMAReadTake() : bus.DMAWriteTake(UINT32_MAX);
    if (bytes > ready && channel.IsMDMA()) {
        const unsigned other = channel.GetPeripheralType() ^ 1;  // MDMA source <-> destination
        for (u32 mask = runnable_; mask; mask &= mask - 1) {
            const size_t side = __builtin_ctz(mask);
            if (channels[side]->GetPeripheralType() == other)
                bound = std::max(bound, channels[side]->CyclesToMove(bytes - ready) + (side > index));
        }
    }
    // Past it, the event waits on something no pace bounds: the next pass.
    return bound > now ? bound : nextPass_;
}

// A pass at each cycle some settling channel's chunk is due, in channel
// order: what a service at every cycle would do, without the cycles at which
// nothing can move. A channel that was due and could not move (its FIFO side
// empty or full) waits in blocked_ until another moves; the pass after one
// that moved anything is at the next cycle, as the next service would be.
void DMA::Settle(uint64_t until) {
    Raise busy(busy_);
    while (settling_) {
        uint64_t at = UINT64_MAX;
        for (u32 mask = settling_ & ~blocked_; mask; mask &= mask - 1)
            at = std::min(at, channels[__builtin_ctz(mask)]->DueCycles());
        nextPass_ = at == UINT64_MAX ? at : std::max(at, passFloor_);
        if (nextPass_ > until) return;
        at = nextPass_;
        bool moved = false;
        u16 waiting = 0;
        for (u32 mask = settling_; mask; mask &= mask - 1) {
            const size_t index = __builtin_ctz(mask);
            auto& channel = *channels[index];
            if (channel.DueCycles() > at) continue;
            const uint64_t events = channel.Events();
            if (Pump(channel, at)) moved = true;
            else waiting |= u16(1u << index);
            // Outside a service, an event is the next one's work.
            if (channel.Events() != events && !servicing_) touched_ = true;
            CountServiceWork(channel.Events() - events);
            Refresh(index);
        }
        blocked_ = moved ? 0 : waiting;
        passFloor_ = at + 1;
    }
    nextPass_ = UINT64_MAX;
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
    // The settling channels' rows first: what they wrote is in memory for
    // the others. The rest in channel order, as a pass over all of them
    // would; a transfer changes only its own channel.
    if (settling_) Settle(now);
    for (u32 mask = runnable_ & ~settling_; mask; mask &= mask - 1) {
        const size_t index = __builtin_ctz(mask);
        const u32 moved = Pump(*channels[index], now);
        if (moved) blocked_ = 0;  // a FIFO's side that does not settle
        CountServiceWork(moved);
        Refresh(index);
    }
    // A channel the loop made settle (a descriptor) gets its next pass.
    if (settling_) Settle(now);
    idleUntil_ = IdleUntil();
    Watch();
}

void DMA::BindInterrupt(int channel, int q, InterruptHandler callback) {
    if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        channels[channel]->BindInterrupt(q, callback);
    }
}