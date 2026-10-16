// A device serviced only when it has work must show the guest exactly what
// the same device shows when it is serviced after every block: each test
// drives two instances with the same inputs, one polled at every step and one
// through its service contract, and compares what the guest can observe. An
// interrupt is a level, so what counts is the level last published, not how
// often it was published again.
#include "cpu/coretimer.h"
#include "cpu/dma.h"
#include "cpu/nand.h"
#include "cpu/sic.h"
#include "cpu/sport.h"
#include "cpu/rtc.h"
#include "cpu/time_source.h"
#include "cpu/twi.h"
#include "peripheral/mcp230xx.h"
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


// A flash that, like MT29F4G08, stays busy after a command until an event
// (here: at the end of the next block) makes it ready again.
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
    void SetReadyCallback(std::function<void()> callback) override { onReady_ = std::move(callback); }
    // The end of a block, after the devices: the CPU's event queue.
    void EndOfBlock() {
        if (clearIn_ > 0 && --clearIn_ == 0) {
            busy_ = false;
            if (onReady_) onReady_();
        }
    }

private:
    std::function<void()> onReady_;
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

// The core timer counts guest cycles: it expires exactly TPERIOD * (TSCALE + 1)
// cycles after it starts, and auto-reload keeps that period exactly.
static void CoreTimerCountsGuestCycles() {
    TimeSource time(true);
    CoreTimer timer(0xFFE03000, time);
    int level = 0, rises = 0;
    timer.BindInterrupt(6, [&](int, int value) {
        rises += value && !level;
        level = value;
    });
    auto at = [&](uint64_t cycles) { time.Retire(cycles - time.Cycles()); };
    at(1000);
    timer.Write32(0x08, 1);   // TSCALE: every 2 cycles
    timer.Write32(0x04, 50);  // TPERIOD (and TCOUNT) 50: 100 cycles
    timer.Write32(0x00, 7);   // TMPWR | TMREN | TAUTORLD
    Require(timer.NextExpiry() == 1100u, "first expiry 100 cycles after the start");
    at(1099);
    timer.Advance(time.Cycles());
    Require(level == 0 && timer.Read32(0x0C) == 1, "one tick left at 1099");
    at(1100);
    timer.Advance(time.Cycles());
    Require(level == 1 && rises == 1, "expires at 1100");
    Require(timer.NextExpiry() == 1200u, "reloads at the expiry, not when it was noticed");
    timer.ProcessWithInterrupt(6);  // the ISR is entered
    Require(level == 0, "TINT clears in IVG6");
    at(1150);
    timer.Write32(0x00, 7);  // the ISR rewrites TCNTL while counting
    Require(timer.NextExpiry() == 1200u, "a TCNTL write does not restart a running count");
    at(1450);
    timer.Advance(time.Cycles());
    Require(rises == 2 && timer.NextExpiry() == 1500u, "a late look sees one interrupt and the period kept");
    timer.ProcessWithInterrupt(6);
    timer.Write32(0x00, 3);  // one-shot
    at(1500);
    timer.Advance(time.Cycles());
    Require(rises == 3 && level == 1, "the one-shot expires");
    Require(!timer.NextExpiry() && (timer.Read32(0x00) & 2) == 0, "and stops, clearing TMREN");
    timer.Write32(0x04, 10);
    timer.Write32(0x00, 3);
    at(1600);
    Require(timer.Read32(0x0C) == 0 && rises == 4, "reading TCOUNT brings the count up to date (TCNTL cleared TINT)");
    Require(timer.NextExpiry() == std::nullopt, "and expires it");
    // Every expiry is an interrupt, also while TINT is still set from the last.
    int expiries = 0;
    timer.BindExpiry([&] { ++expiries; });
    timer.Write32(0x08, 0);
    timer.Write32(0x04, 10);
    timer.Write32(0x00, 7 | 8);  // TINT left set
    at(time.Cycles() + 10);
    timer.Advance(time.Cycles());
    at(time.Cycles() + 10);
    timer.Advance(time.Cycles());
    Require(expiries == 2 && timer.InterruptPending(), "each period's expiry counts, TINT or not");
    timer.Write32(0x00, 0);
    timer.Write32(0x08, 1);
    timer.Write32(0x04, 0);
    // Enabled first, loaded after: counting starts with the load.
    timer.Write32(0x00, 7);
    Require(!timer.NextExpiry(), "nothing to count with TCOUNT 0");
    at(2000);
    timer.Write32(0x04, 10);
    Require(timer.NextExpiry() == 2020u, "a TPERIOD write starts an enabled timer");
}

// An unpaced peripheral (the PPI, MDMA) moves a row once its elements' SCLK
// cycles have passed: a 2D frame takes rows * columns * 4 core cycles.
class PacedSink : public DMABus {
public:
    std::vector<uint64_t> rows;  // guest cycles at which each row arrived
    const TimeSource& time;
    explicit PacedSink(const TimeSource& t) : time(t) {}
    u32 DMARead(int, int, void*, u32) override { return 0; }
    u32 DMAWrite(int, int, const void*, u32 length) override {
        rows.push_back(time.Cycles());
        return length;
    }
    u32 CyclesPerElement() const override { return kSclkCyclesPerElement; }
};

static void DmaPacesUnpacedBuses() {
    TimeSource time(true);
    Emulator emulator;
    MemoryDevice ram{"SDRAM", 0, 0x10000};
    emulator.BindDevice(&ram);
    DMA dma(0xFFC00C00, emulator, &time);
    auto sink = std::make_shared<PacedSink>(time);
    dma.AttachDMABus(DMAPeripheralPPI, sink);
    time.Retire(1000);
    dma.Write32(0x04, 0x100);  // START_ADDR
    dma.Write32(0x10, 200);    // X_COUNT
    dma.Write32(0x14, 2);      // X_MODIFY
    dma.Write32(0x18, 3);      // Y_COUNT
    dma.Write32(0x1C, 2);      // Y_MODIFY
    dma.Write32(0x08, 0x35);   // DMAEN, 16-bit, 2D, sync: the OP-1's frame transfer
    std::vector<uint64_t> serviced;
    for (int step = 0; step < 4000; ++step) {
        time.Retire(1);
        if (dma.NeedsService(time.Nanoseconds())) {
            dma.ProcessWithInterrupt(-1);
            serviced.push_back(time.Cycles());
        }
    }
    const std::vector<uint64_t> expected{1800, 2600, 3400};
    Require(sink->rows == expected, "a row of 200 elements every 800 cycles");
    Require((dma.Read32(0x28) & 1) != 0, "DMA_DONE after the last row");
    Require(serviced.size() <= 8, "the DMA is not called between rows: " + std::to_string(serviced.size()));
}

// TWI master bytes take nine SCL periods of guest time each, after an address
// byte; a full receive FIFO holds SCL until the guest reads it.
static void TwiPacesBytes() {
    TimeSource time(true);
    TWI twi(0xFFC01400, &time);
    twi.SetSpeedup(1);  // the hardware's rate
    twi.AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x20, 0x5A));
    std::vector<uint64_t> fifoAt;  // guest ns at which the receive FIFO grew
    auto fifo = [&] { return (twi.Read32(0x2C) >> 2) & 3; };  // FIFO_STAT RCVSTAT
    auto at = [&](uint64_t ns) { time.Retire((ns * 2 + 4) / 5 - time.Packets()); };
    twi.Write32(0x00, 12 << 8 | 12);  // CLKDIV: 2.4 us SCL, 21.6 us per byte
    twi.Write32(0x04, 0x80);          // TWI_ENA
    twi.Write32(0x1C, 0x20);          // MASTER_ADDR
    at(1000);
    twi.Write32(0x14, 4 << 6 | 4 | 1);  // read 4 bytes
    uint64_t last = 0;
    unsigned seen = 0;
    auto run = [&](uint64_t untilNs) {
        while (time.Nanoseconds() < untilNs) {
            time.Retire(7);
            if (twi.NeedsService(time.Nanoseconds())) twi.ProcessWithInterrupt(-1);
            const unsigned now = fifo();
            if (now > seen) fifoAt.push_back(time.Nanoseconds());
            seen = now;
        }
        last = time.Nanoseconds();
    };
    run(200000);
    Require(fifoAt.size() == 2, "two bytes fill the FIFO, then SCL is held: " + std::to_string(fifoAt.size()));
    Require(fifoAt[0] >= 1000 + 43200 && fifoAt[0] < 1000 + 43200 + 20, "address and first byte take 43.2 us");
    Require(fifoAt[1] >= 1000 + 64800 && fifoAt[1] < 1000 + 64800 + 20, "the second byte 21.6 us later");
    twi.Read32(0x88);  // RCV_DATA8 at `last`
    seen = fifo();
    const uint64_t read = last;
    run(300000);
    Require(fifoAt.size() == 3 && fifoAt[2] >= read + 21600 && fifoAt[2] < read + 21600 + 20,
            "a read releases SCL: the next byte 21.6 us after it");
    twi.SetSpeedup(10);
    twi.Read32(0x88);
    twi.Read32(0x88);
    seen = fifo();
    twi.Write32(0x14, 1 << 6 | 4 | 1);  // read 1 byte, 10 times faster
    const uint64_t start = time.Nanoseconds();
    const size_t before = fifoAt.size();
    run(time.Nanoseconds() + 10000);
    Require(fifoAt.size() == before + 1 && fifoAt.back() >= start + 4320 && fifoAt.back() < start + 4320 + 20,
            "a speedup of 10 divides the byte time");
}

// What the guest sees of a TWI transfer depends on guest time only, not on
// where blocks end: two TWIs get the same accesses at the same times, one is
// serviced after every step, the other rarely.
static void TwiIgnoresBlockBoundaries(uint32_t seed) {
    struct Rig {
        TimeSource time{true};
        TWI twi{0xFFC01400, &time};
        Rig() {
            twi.AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x10, 0xA5));
            twi.Write32(0x00, 0x0C0C);
            twi.Write32(0x04, 0x80);
            twi.Write32(0x1C, 0x10);
        }
    } often, rarely;
    std::mt19937 rng(seed);
    for (int step = 0; step < 20000; ++step) {
        const uint64_t packets = rng() % 400 + 1;
        const uint32_t op = rng() % 16;
        const bool boundary = rng() % 6 == 0;
        uint32_t control = (1 + rng() % 6) << 6 | (rng() % 2 ? 4 : 0) | 1;
        for (Rig* rig : {&often, &rarely}) {
            rig->time.Retire(packets);
            if (rig == &often || boundary)
                if (rig->twi.NeedsService(rig->time.Nanoseconds())) rig->twi.ProcessWithInterrupt(-1);
        }
        uint32_t a = 0, b = 0;
        if (op == 0) {
            often.twi.Write32(0x14, control);
            rarely.twi.Write32(0x14, control);
        } else if (op < 4) {
            a = often.twi.Read32(0x88);  // RCV_DATA8
            b = rarely.twi.Read32(0x88);
        } else if (op < 7) {
            often.twi.Write32(0x80, step & 0xFF);  // XMT_DATA8
            rarely.twi.Write32(0x80, step & 0xFF);
        } else if (op < 10) {
            a = often.twi.Read32(0x2C);  // FIFO_STAT
            b = rarely.twi.Read32(0x2C);
        } else if (op < 12) {
            a = often.twi.Read32(0x20);  // INT_STAT
            b = rarely.twi.Read32(0x20);
            often.twi.Write32(0x20, a);
            rarely.twi.Write32(0x20, a);
        }
        Require(a == b, "seed " + std::to_string(seed) + ": TWI read differs at step " + std::to_string(step));
    }
}

// A sequential register write to a real MCP23017 over the TWI: the address
// pointer advances through IODIRA to IODIRB however the bytes are split
// between transfers and calls.
static void Mcp23017SequentialWrite(bool oneByteAtATime) {
    TimeSource time(true);
    TWI twi(0xFFC01400, &time);
    auto gpio = std::make_shared<MCP230XX>(0x20, MCP230XXModel::MCP23017);
    twi.AttachPeripheral(gpio);
    twi.Write32(0x00, 0x0C0C);
    twi.Write32(0x04, 0x80);
    twi.Write32(0x1C, 0x20);
    const u8 bytes[] = {0x00, 0x00, 0xFF};  // IODIRA = outputs, IODIRB = inputs
    if (oneByteAtATime) {
        twi.Write32(0x80, bytes[0]);
        twi.Write32(0x14, 3 << 6 | 1);
        for (int i = 1; i < 3; ++i) {
            time.Retire(4000);
            twi.ProcessWithInterrupt(-1);
            twi.Write32(0x80, bytes[i]);
        }
    } else {
        twi.Write32(0x84, bytes[0] | bytes[1] << 8);
        twi.Write32(0x14, 3 << 6 | 1);
        time.Retire(4000);
        twi.ProcessWithInterrupt(-1);
        twi.Write32(0x80, bytes[2]);
    }
    time.Retire(4000);
    twi.ProcessWithInterrupt(-1);
    Require(((twi.Read32(0x20) >> 4) & 1) == 1, "the write completes");
    Require(gpio->GetDirection(0) == GPIOPinDirection::Output, "IODIRA gets the first data byte");
    Require(gpio->GetDirection(8) == GPIOPinDirection::Input, "IODIRB gets the second");
}

int main() {
    Mcp23017SequentialWrite(false);
    Mcp23017SequentialWrite(true);
    for (uint32_t seed = 1; seed <= 10; ++seed) TwiIgnoresBlockBoundaries(seed);
    TwiPacesBytes();
    DmaPacesUnpacedBuses();
    CoreTimerCountsGuestCycles();
    SicLines();
    for (uint32_t seed = 1; seed <= 20; ++seed) RtcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 20; ++seed) NfcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 10; ++seed) AudioMatchesPolling(seed);
    std::puts("device-service-test: ok");
}
