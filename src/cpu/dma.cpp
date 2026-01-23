#include "dma.h"
#include "utils/log.h"
#include <stdint.h>

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

    void ProcessTransfer();

protected:
    void ProcessDescriptor();

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
    FIELD(CONFIG, WDSIZE, 2, 2, R(wordSize), W(wordSize));;
    FIELD(CONFIG, DMA2D, 4, 1, R(mode2D), W(mode2D));
    FIELD(CONFIG, SYNC, 5, 1, R(synchronized), W(synchronized));
    FIELD(CONFIG, DI_SEL, 6, 1, R(mode2DInterruptEachRow), W(mode2DInterruptEachRow));
    FIELD(CONFIG, DI_EN, 7, 1, R(dataInterruptEnabled), W(dataInterruptEnabled));
    FIELD(CONFIG, NDSIZE, 8, 4, R(descriptorSize), W(descriptorSize));
    FIELD(CONFIG, FLOW, 12, 3, R(next), [this](u32 v) {
        next = (DMANextOperation)next;
    });
    CONFIG.writeCallback = [this](u32 value) {
        if (enabled) {
            running = true;
        } else {
            running = false;
        }
        ProcessDescriptor();
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

    currDescPtr = nextDescPtr;
    currAddr = startAddr;
    currXCount = xCount ?: 0xFFFF;
    currYCount = yCount ?: 0xFFFF;
}

void DMAChannel::ProcessTransfer() {
    if (!enabled || !running) return;
    if (channelIsMemory) return; // Memory-to-memory not supported

    auto bus = dma.GetDMABus(peripheralType);
    if (!bus) {
        LogWarn("DMA channel %s: No DMA bus attached for peripheral type %d", Name().c_str(), peripheralType);
        return;
    }

    int elementBytes = 1 << wordSize;
    u32 totalBytes = currXCount * elementBytes;

    u8 buffer[4096];
    totalBytes = std::min(totalBytes, (u32)sizeof(buffer));
    dma.GetEmulator().Lock();
    if (memoryWrite) {
        totalBytes = bus->DMARead(xCount - currXCount, yCount - currYCount, buffer, totalBytes);
        if (xModify == elementBytes) {
            dma.GetEmulator().MemoryWrite(currAddr, buffer, totalBytes);
        } else {
            for (u32 i = 0; i < totalBytes; i ++) {
                dma.GetEmulator().MemoryWrite(currAddr + i * xModify, buffer + i * elementBytes, elementBytes);
            }
        }
    } else {
        // Memory read (memory to peripheral)
        if (xModify == elementBytes) {
            dma.GetEmulator().MemoryRead(currAddr, buffer, totalBytes);
        } else {
            for (u32 i = 0; i < totalBytes; i ++) {
                dma.GetEmulator().MemoryRead(currAddr + i * xModify, buffer + i * elementBytes, elementBytes);
            }
        }
        totalBytes = bus->DMAWrite(xCount - currXCount, yCount - currYCount, buffer, totalBytes);
    }
    dma.GetEmulator().Unlock();

    u32 count = totalBytes / elementBytes;
    currAddr += count * xModify;
    currXCount -= count;

    if (currXCount == 0) {
        if (mode2D) {
            if (currYCount > 1) {
                currYCount--;
                currXCount = xCount;
                currAddr = currAddr - xModify + yModify;
                return;
            }
        }
        completed = true;
        if (next == DMANextOperation::Stop) {
            running = false;
        } else {
            ProcessDescriptor();
        }
        if (dataInterruptEnabled) {
            if (!mode2D || mode2DInterruptEachRow) {
                TriggerInterrupt(1);
            } else if (currYCount == 0) {
                TriggerInterrupt(1);
            }
        }
    }
}

DMA::DMA(u32 baseAddr, Emulator& emu)
    : RegisterDevice("DMA", baseAddr, 0x400), emulator(emu)
{
    for (size_t i = 0; i < channels.size(); i++) {
        channels[i] = std::make_shared<DMAChannel>("DMAChannel" + std::to_string(i), baseAddr + i * 0x40, *this, i);
    }
}

void DMA::Write32(u32 offset, u32 value) {
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        channels[channelIndex]->Write32(offset % 0x40, value);
    }
}

u32 DMA::Read32(u32 offset) {
    size_t channelIndex = offset / 0x40;
    if (channelIndex < channels.size()) {
        return channels[channelIndex]->Read32(offset % 0x40);
    }
    return 0;
}

void DMA::ProcessWithInterrupt(int ivg) {
    for (auto& channel : channels) {
        channel->ProcessTransfer();
    }
}

void DMA::BindInterrupt(int channel, int q, InterruptHandler callback) {
    if (channel >= 0 && channel < static_cast<int>(channels.size())) {
        channels[channel]->BindInterrupt(q, callback);
    }
}