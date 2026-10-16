// A device serviced only when it has work must show the guest exactly what
// the same device shows when it is serviced after every block: each test
// drives two instances with the same inputs, one polled at every step and one
// through its service contract, and compares what the guest can observe. An
// interrupt is a level, so what counts is the level last published, not how
// often it was published again.
#include "cpu/dma.h"
#include "cpu/nand.h"
#include "cpu/sic.h"
#include "cpu/sport.h"
#include "cpu/rtc.h"
#include "cpu/time_source.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <memory>
#include <random>
#include <string>
#include <vector>

static void Require(bool ok, const std::string& message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

// The RTC's registers, as the guest reads them.
static std::vector<u32> RtcRegisters(RTC& rtc) {
    std::vector<u32> out;
    for (u32 offset = 0; offset < 0x18; offset += 4) out.push_back(rtc.Read32(offset));
    return out;
}

static void RtcMatchesPolling(uint32_t seed) {
    TimeSource polledTime(true), scheduledTime(true);
    RTC polled(0xFFC00300, polledTime), scheduled(0xFFC00300, scheduledTime);
    int polledLevel = 0, scheduledLevel = 0;
    polled.BindInterrupt(14, [&](int, int level) { polledLevel = level; });
    scheduled.BindInterrupt(14, [&](int, int level) { scheduledLevel = level; });

    std::mt19937 rng(seed);
    uint64_t ticks = 0;
    // The calendar's seconds start at guest time zero, and again at each
    // RTC_STAT write: steps aimed at their boundaries catch a deadline that
    // is off by a packet.
    uint64_t secondsOrigin = 0;
    for (int step = 0; step < 20000; ++step) {
        uint64_t packets;
        if (rng() % 4 == 0) {
            const uint64_t now = polledTime.Nanoseconds();
            const uint64_t boundary = secondsOrigin + ((now - secondsOrigin) / 1000000000u + 1) * 1000000000u;
            const uint64_t target = boundary - 3 + rng() % 7;  // within a packet or two of it
            const uint64_t targetPackets = (target * 2 + 4) / 5;  // first packet count reaching target ns
            packets = targetPackets > polledTime.Packets() ? targetPackets - polledTime.Packets() : 1;
        } else {
            // Mostly short blocks, sometimes a long stretch of guest time.
            packets = rng() % 8 == 0 ? rng() % 800000000u : rng() % 4000 + 1;
        }
        polledTime.Retire(packets);
        scheduledTime.Retire(packets);
        if (rng() % 64 == 0) {
            static const u32 offsets[] = {0x00, 0x04, 0x08, 0x0C, 0x10, 0x14};
            const u32 offset = offsets[rng() % 6];
            u32 value = rng();
            if (offset == 0x14) value &= 1;
            if (offset == 0x04) value &= 0x807F;
            polled.Write32(offset, value);
            scheduled.Write32(offset, value);
            if (offset == 0x00) secondsOrigin = polledTime.Nanoseconds();
        }
        polled.Tick();
        if (scheduled.NeedsTick()) {
            scheduled.Tick();
            ++ticks;
        }
        Require(RtcRegisters(polled) == RtcRegisters(scheduled),
                "seed " + std::to_string(seed) + ": registers differ at step " + std::to_string(step));
        Require(polledLevel == scheduledLevel,
                "seed " + std::to_string(seed) + ": RTC levels differ at step " + std::to_string(step));
    }
    Require(ticks < 20000, "the scheduled RTC must skip calls with nothing to do");
}


// A flash with the timing the CPU gives MT29F4G08: a command that makes it
// busy is answered by an event that runs at the end of the next block.
class StepFlash : public NandFlash {
public:
    void SendCommand(u8 command) override {
        if (command == 0xFF || command == 0x30 || command == 0x10 || command == 0xD0) {
            busy_ = true;
            clearIn_ = 2;
        }
        ready_ = command == 0x30 || command == 0x70 || command == 0x90;
        remaining_ = 3;
    }
    void SendAddress(u8) override {}
    u8 ReadData() override {
        if (ready_ && --remaining_ == 0) ready_ = false;
        return 0x5A;
    }
    void WriteData(u8) override {}
    bool IsDataReady() const override { return ready_; }
    bool IsBusy() const override { return busy_; }
    void StartPageRead() override {}
    void StartPageWrite() override {}
    u32 PageRead(u8* data, u32 length) override {
        std::fill(data, data + length, u8(0xA5));
        return length;
    }
    u32 PageWrite(const u8*, u32 length) override { return length; }
    void SetReadCallback(ReadCallback) override {}
    // The end of a block, after the devices: the CPU's event queue.
    void EndOfBlock() {
        if (clearIn_ > 0 && --clearIn_ == 0) busy_ = false;
    }

private:
    bool busy_ = false, ready_ = false;
    int clearIn_ = 0, remaining_ = 0;
};

static std::vector<u32> NfcRegisters(NFC& nfc) {
    std::vector<u32> out;
    // NFC_READ (0x2C) reads the flash: left out, it is a guest input below.
    for (u32 offset = 0; offset < 0x2C; offset += 4) out.push_back(nfc.RegisterDevice::Read32(offset));
    return out;
}

static void NfcMatchesPolling(uint32_t seed) {
    NFC polled(0xFFC03700), scheduled(0xFFC03700);
    auto polledFlash = std::make_shared<StepFlash>(), scheduledFlash = std::make_shared<StepFlash>();
    polled.AttachNandFlash(polledFlash);
    scheduled.AttachNandFlash(scheduledFlash);
    int polledLevel = 0, scheduledLevel = 0;
    polled.BindInterrupt(48, [&](int, int level) { polledLevel = level; });
    scheduled.BindInterrupt(48, [&](int, int level) { scheduledLevel = level; });

    std::mt19937 rng(seed);
    uint64_t services = 0;
    for (int step = 0; step < 20000; ++step) {
        // What one block does to the NFC.
        const int ops = rng() % 4 == 0 ? rng() % 3 + 1 : 0;
        for (int op = 0; op < ops; ++op) {
            const int kind = rng() % 8;
            if (kind == 0) {
                static const u8 commands[] = {0xFF, 0x00, 0x30, 0x80, 0x10, 0x60, 0xD0, 0x70, 0x90};
                const u8 command = commands[rng() % 9];
                polled.Write32(0x44, command);
                scheduled.Write32(0x44, command);
            } else if (kind == 1) {
                const u32 value = rng() & 0x1F;
                polled.Write32(0x08, value);
                scheduled.Write32(0x08, value);
            } else if (kind == 2) {
                const u32 value = rng() & 0x1F;
                polled.Write32(0x0C, value);
                scheduled.Write32(0x0C, value);
            } else if (kind == 3) {
                polled.Write32(0x4C, 0);
                scheduled.Write32(0x4C, 0);
            } else if (kind == 4) {
                Require(polled.Read32(0x2C) == scheduled.Read32(0x2C), "NFC_READ differs");
            } else if (kind == 5) {
                u8 a[512], b[512];
                const u32 length = 1 + rng() % 512;
                Require(polled.DMARead(0, 0, a, length) == scheduled.DMARead(0, 0, b, length), "DMARead differs");
            } else if (kind == 6) {
                const u8 data[512] = {};
                const u32 length = 1 + rng() % 512;
                Require(polled.DMAWrite(0, 0, data, length) == scheduled.DMAWrite(0, 0, data, length),
                        "DMAWrite differs");
            } else {
                polled.Write32(0x24, 1);
                scheduled.Write32(0x24, 1);
            }
        }
        polled.ProcessWithInterrupt(-1);
        if (scheduled.NeedsService()) {
            scheduled.ProcessWithInterrupt(-1);
            ++services;
        }
        polledFlash->EndOfBlock();
        scheduledFlash->EndOfBlock();
        Require(NfcRegisters(polled) == NfcRegisters(scheduled),
                "seed " + std::to_string(seed) + ": NFC registers differ at step " + std::to_string(step));
        Require(polledLevel == scheduledLevel,
                "seed " + std::to_string(seed) + ": NFC levels differ at step " + std::to_string(step));
    }
    Require(services < 20000 * 3 / 4, "the scheduled NFC must skip calls with nothing to do: " + std::to_string(services));
}

// SPORT0 audio through DMA channels 3 (receive) and 4 (transmit), autobuffer.
struct AudioRig {
    TimeSource time{true};
    Emulator emulator;
    MemoryDevice ram{"SDRAM", 0, 0x10000};
    DMA dma{0xFFC00C00, emulator};
    std::shared_ptr<SPORT> sport = std::make_shared<SPORT>(0xFFC00800, 0, time);
    std::vector<std::string> log;
    int step = 0;
    uint16_t input = 0;

    AudioRig() {
        emulator.BindDevice(&ram);
        dma.AttachDMABus(DMAPeripheralSPORT0Rx, sport);
        dma.AttachDMABus(DMAPeripheralSPORT0Tx, sport);
        sport->SetAudioOutputCallback([this](const void* data, size_t samples, int channels, int bits) {
            log.push_back(std::to_string(step) + " out " + std::to_string(samples) + "x" + std::to_string(channels) +
                          " " + std::to_string(bits) + " " + std::to_string(*static_cast<const uint16_t*>(data)));
        });
        sport->SetAudioInputCallback([this](void* data, size_t samples, int channels, int bits) {
            auto* out = static_cast<uint16_t*>(data);
            const size_t words = samples * channels * (bits > 16 ? 2 : 1);
            for (size_t i = 0; i < words; ++i) out[i] = input++;
            log.push_back(std::to_string(step) + " in " + std::to_string(samples));
            return samples;
        });
        for (int channel : {3, 4})
            dma.BindInterrupt(channel, channel, [this](int q, int level) {
                log.push_back(std::to_string(step) + " irq " + std::to_string(q) + "=" + std::to_string(level));
            });
        for (u32 i = 0; i < 0x2000; i += 2) ram.Write32(i, i * 2654435761u);
    }
    void Channel(int channel, u32 start, u32 count, bool toMemory, bool enable) {
        const u32 base = channel * 0x40;
        dma.Write32(base + 0x04, start);
        dma.Write32(base + 0x10, count);
        dma.Write32(base + 0x14, 2);
        // 16-bit words, autobuffer, interrupt on completion.
        dma.Write32(base + 0x08, (enable ? 1 : 0) | (toMemory ? 2 : 0) | (1 << 2) | (1 << 7) | (1 << 12));
    }
};

static void AudioMatchesPolling(uint32_t seed) {
    AudioRig polled, scheduled;
    auto both = [&](auto&& f) { f(polled); f(scheduled); };
    both([](AudioRig& r) {
        r.sport->Write32(0x04, 15);        // TCR2: 16-bit words
        r.sport->Write32(0x24, 15 | 256);  // RCR2: 16-bit stereo
        r.sport->Write32(0x00, 1);         // TSPEN
        r.sport->Write32(0x20, 1);         // RSPEN
        r.Channel(4, 0x1000, 64, false, true);
        r.Channel(3, 0x4000, 32, true, true);
    });
    std::mt19937 rng(seed);
    uint64_t services = 0;
    const int steps = 40000;
    for (int step = 0; step < steps; ++step) {
        const uint64_t packets = rng() % 2 ? rng() % 30 + 1 : rng() % 20000 + 1;
        const uint32_t event = rng() % 512;
        const bool transmit = rng() % 2;
        both([&](AudioRig& r) {
            r.step = step;
            r.time.Retire(packets);
            if (event == 0) r.sport->Write32(0x00, transmit ? 1 : 0);
            if (event == 1) r.sport->Write32(0x20, 1);
            if (event == 2) r.sport->Write32(0x04, 15 | (step % 2 ? 256 : 0));
            if (event == 3) r.Channel(4, 0x1000 + (step % 7) * 64, 16 + step % 48, false, step % 5 != 0);
            if (event == 4) r.Channel(3, 0x4000, 1 + step % 32, true, true);
        });
        polled.dma.ProcessWithInterrupt(-1);
        if (scheduled.dma.NeedsService(scheduled.time.Nanoseconds())) {
            scheduled.dma.ProcessWithInterrupt(-1);
            ++services;
        }
        Require(polled.log == scheduled.log,
                "seed " + std::to_string(seed) + ": audio differs at step " + std::to_string(step));
        if (step % 997 == 0)
            for (u32 i = 0x4000; i < 0x4100; i += 4)
                Require(polled.ram.Read32(i) == scheduled.ram.Read32(i), "received samples differ");
    }
    std::vector<u32> a, b;
    for (u32 offset = 0; offset < 0x400; offset += 4) {
        a.push_back(polled.dma.Read32(offset));
        b.push_back(scheduled.dma.Read32(offset));
    }
    Require(a == b, "seed " + std::to_string(seed) + ": DMA registers differ");
    Require(services < steps / 2, "the scheduled DMA must skip calls with nothing to do: " + std::to_string(services));
}

// The SIC's outputs are levels: high while a routed pin is asserted and
// unmasked, whatever else is published.
static void SicLines() {
    SIC sic(0xFFC00100);
    std::vector<u16> rising;
    sic.SetLineRaisedCallback([&](u16 lines) { rising.push_back(lines); });
    sic.Write32(0x0C, 1u << 14);           // SIC_IMASK0: unmask pin 14 (RTC)
    // SIC_IAR1 routes pins 8..15: pin 14 to IVG 7 + 4 = 11.
    sic.Write32(0x14, 0x04000000);
    Require(sic.Lines() == 0 && rising.empty(), "no pin asserted");
    sic.SetInterruptLevel(14, 1);
    Require(sic.Lines() == 1u << 11 && rising == std::vector<u16>{1u << 11}, "pin 14 drives IVG11");
    sic.SetInterruptLevel(14, 1);
    sic.SetInterruptLevel(15, 1);           // masked
    Require(rising.size() == 1, "publishing the same level again raises nothing");
    Require(sic.Lines() == 1u << 11, "a masked pin drives nothing");
    sic.Write32(0x14, 0x05000000);         // reroute pin 14 to IVG12
    Require(sic.Lines() == 1u << 12 && rising.back() == 1u << 12, "IAR writes move the line");
    sic.Write32(0x0C, 0);
    Require(sic.Lines() == 0, "IMASK drops the line");
    sic.Write32(0x0C, 1u << 14);
    Require(sic.Lines() == 1u << 12 && rising.size() == 3, "unmasking a pending pin raises its line");
    sic.SetInterruptLevel(14, 0);
    Require(sic.Lines() == 0, "deasserting the pin drops the line");
}

int main() {
    SicLines();
    for (uint32_t seed = 1; seed <= 20; ++seed) RtcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 20; ++seed) NfcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 10; ++seed) AudioMatchesPolling(seed);
    std::puts("device-service-test: ok");
}
