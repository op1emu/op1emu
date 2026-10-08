// Synthetic RAM and peripheral fixture; no firmware or device image required.
#include "cpu/dma.h"
#include <array>
#include <cstring>
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

class TestRam : public Device {
    std::array<u8, 4096> bytes{};
public:
    TestRam() : Device("test RAM", 0x1000, 4096) {}
    void Read(u32 offset, void* output, u32 length) override {
        check(offset <= bytes.size() && length <= bytes.size() - offset, "RAM read bounds");
        std::memcpy(output, bytes.data() + offset, length);
    }
    void Write(u32 offset, const void* input, u32 length) override {
        check(offset <= bytes.size() && length <= bytes.size() - offset, "RAM write bounds");
        std::memcpy(bytes.data() + offset, input, length);
    }
    u32 Read32(u32 offset) override { u32 value; Read(offset, &value, 4); return value; }
    void Write32(u32 offset, u32 value) override { Write(offset, &value, 4); }
};

class Sink : public DMABus {
public:
    unsigned writes = 0;
    u32 DMARead(int, int, void*, u32) override { return 0; }
    u32 DMAWrite(int, int, const void*, u32 length) override { ++writes; return length; }
};

static void largeList(unsigned halfwords) {
    TestRam ram;
    Emulator emulator;
    emulator.BindDevice(&ram);
    DMA dma(0xffc00c00, emulator);
    auto sink = std::make_shared<Sink>();
    dma.AttachDMABus(DMAPeripheralSPORT0Tx, sink);
    constexpr u32 channel = 4 * 0x40;
    // Two deliberately non-contiguous descriptors point at each other.
    ram.Write32(0x800, 0x1840);
    ram.Write32(0x804, 0x1100);
    ram.Write32(0x840, 0x1800);
    ram.Write32(0x844, 0x1200);
    dma.Write32(channel, 0x1800);
    dma.Write32(channel + 4, 0x1100);
    dma.Write32(channel + 0x10, 4);
    dma.Write32(channel + 0x14, 4);
    dma.Write32(channel + 8, 0x7009 | (halfwords << 8));
    for (unsigned block = 0; block < 6; ++block) {
        const bool second = block % 2;
        check(dma.Read32(channel) == (second ? 0x1800u : 0x1840u), "next link");
        check(dma.Read32(channel + 0x20) == (second ? 0x1840u : 0x1800u) + 2 * halfwords,
              "CURR_DESC_PTR must follow the fetched descriptor, not the next link");
        check(dma.Read32(channel + 0x24) == (second && halfwords == 4 ? 0x1200u : 0x1100u), "current buffer");
        check(dma.Read32(channel + 0x30) == 4, "reloaded X count");
        dma.ProcessWithInterrupt(-1);
        check(sink->writes == block + 1, "one transfer per block");
    }
    // Disabling the channel must not fetch another descriptor.
    const auto last = dma.Read32(channel + 0x20);
    dma.Write32(channel + 8, 0);
    check(dma.Read32(channel + 0x20) == last, "disabled channel descriptor state");
}

int main() {
    try {
        largeList(2); // Link only; START_ADDR is retained.
        largeList(4); // Link and START_ADDR; alternating buffers.
        std::cout << "DMA large-list descriptor cursor regression: PASS\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
