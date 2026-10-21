#include "dma.h"
#include "time_source.h"
#include "utils/log.h"
#include <stdint.h>
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
    bool IsMDMA() const;
    bool IsMDMASource() const;
    DMAPeripheralType GetPeripheralType() const { return peripheralType; }

    u32 ProcessTransfer();

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
            ProcessTransfer();
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
u32 DMAChannel::ProcessTransfer() {
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
    if (pace && dma.Time()->Cycles() < pacedFrom_ + uint64_t(totalBytes / elementBytes) * pace) return 0;
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
            } else if (currYCount == 0) {
                TriggerInterrupt(1);
            }
        }
        if (transferComplete) {
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

// Offset of a channel's IRQ_STATUS: DMA_DONE and DMA_ERR are W1C (the level
// is published here), DMA_RUN is read-only. Neither what a channel moves nor
// DMA::IdleUntil() depends on them.
static constexpr u32 kIrqStatus = 0x28;

void DMA::Write32(u32 offset, u32 value) {
    if (offset % 0x40 != kIrqStatus) touched_ = true;
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
}

u32 DMA::Read32(u32 offset) {
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
    for (u32 mask = runnable_; mask; mask &= mask - 1) {
        const auto& channel = channels[__builtin_ctz(mask)];
        const uint64_t idle = channel->IdleUntil();
        if (idle < until) until = idle, idleSource_ = channel->GetPeripheralType();
    }
    return until;
}

void DMA::ProcessWithInterrupt(int ivg) {
    // Cleared first: a notice raised while the channels run is for the next call.
    touched_ = false;
    // In channel order, as a pass over all of them would; a transfer changes
    // only its own channel.
    for (u32 mask = runnable_; mask; mask &= mask - 1) {
        const size_t index = __builtin_ctz(mask);
        auto& channel = channels[index];
        if (!channel->IsMDMA()) {
            CountServiceWork(channel->ProcessTransfer());
        } else {
            u32 total = 0;
            while (total < MDMA_BURST_BYTES) {
                u32 moved = channel->ProcessTransfer();
                if (!moved) break;
                total += moved;
            }
            CountServiceWork(total);
        }
        Refresh(index);
    }
    idleUntil_ = IdleUntil();
}

void DMA::BindInterrupt(int channel, int q, InterruptHandler callback) {
    if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        channels[channel]->BindInterrupt(q, callback);
    }
}