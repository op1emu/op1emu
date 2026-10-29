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
#include <cstring>
#include <algorithm>
#include <chrono>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <thread>
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
        // The level the service published is NFC_IRQSTAT & ~NFC_IRQMASK as
        // the registers read.
        const u32 stat = polled.RegisterDevice::Read32(0x08), mask = polled.RegisterDevice::Read32(0x0C);
        Require(polledLevel == int((stat & ~mask & 0x1F) != 0),
                "seed " + std::to_string(seed) + ": NFC level is not IRQSTAT & ~IRQMASK at step " + std::to_string(step));
    }
    Require(services < 20000 * 3 / 4, "the scheduled NFC must skip calls with nothing to do: " + std::to_string(services));
}

// A flash that, after a read command, is busy until the end of the next
// block and then has a page of kPage bytes ready.
class PageFlash : public NandFlash {
public:
    static constexpr int kPage = 64;
    void SendCommand(u8 command) override {
        if (command == 0x30) {
            busy_ = true;
            clearIn_ = 2;
        }
    }
    void SendAddress(u8) override {}
    u8 ReadData() override {
        if (remaining_ == 0) return 0xFF;
        return u8(--remaining_);
    }
    void WriteData(u8) override {}
    bool IsDataReady() const override { return remaining_ != 0; }
    bool IsBusy() const override { return busy_; }
    void StartPageRead() override {}
    void StartPageWrite() override {}
    u32 PageRead(u8*, u32) override { return 0; }
    u32 PageWrite(const u8*, u32 length) override { return length; }
    void SetReadCallback(ReadCallback) override {}
    void SetReadyCallback(std::function<void()> callback) override { onReady_ = std::move(callback); }
    void EndOfBlock() {
        if (clearIn_ > 0 && --clearIn_ == 0) {
            busy_ = false;
            remaining_ = kPage;
            if (onReady_) onReady_();
        }
    }

private:
    std::function<void()> onReady_;
    bool busy_ = false;
    int clearIn_ = 0, remaining_ = 0;
};

// The firmware's PIO read: per byte a block that writes NFC_DATA_RD, polls
// NFC_IRQSTAT for RD_RDY, writes RD_RDY back and reads NFC_READ, a read
// command at every page end. Serviced only when the service would change
// something, the NFC shows the guest what it shows serviced after every
// block, and is serviced about once a page rather than once a byte.
static void NfcPioMatchesPolling(u32 irqmask) {
    NFC polled(0xFFC03700), scheduled(0xFFC03700);
    auto polledFlash = std::make_shared<PageFlash>(), scheduledFlash = std::make_shared<PageFlash>();
    polled.AttachNandFlash(polledFlash);
    scheduled.AttachNandFlash(scheduledFlash);
    int polledLevel = 0, scheduledLevel = 0;
    polled.BindInterrupt(48, [&](int, int level) { polledLevel = level; });
    scheduled.BindInterrupt(48, [&](int, int level) { scheduledLevel = level; });
    polled.Write32(0x0C, irqmask);
    scheduled.Write32(0x0C, irqmask);

    constexpr int kSteps = 20000;
    uint64_t services = 0, quiet = 0;
    bool needCommand = true;
    for (int step = 0; step < kSteps; ++step) {
        auto both = [&](auto&& op) {
            const u32 a = op(polled), b = op(scheduled);
            Require(a == b, "NFC PIO: an access reads differently at step " + std::to_string(step));
            // What the access says about the host is what the service then does.
            quiet += !scheduled.HostWorkAfterAccess(0, false);
        };
        if (needCommand) {
            both([](NFC& n) { n.Write32(0x44, 0x00); return 0u; });
            both([](NFC& n) { n.Write32(0x44, 0x30); return 0u; });
            needCommand = false;
        } else {
            both([](NFC& n) { n.Write32(0x4C, 0); return 0u; });
            const u32 stat = polled.Read32(0x08);
            Require(stat == scheduled.Read32(0x08), "NFC PIO: NFC_IRQSTAT differs at step " + std::to_string(step));
            if (stat & 0x08) {
                both([](NFC& n) { n.Write32(0x08, 0x08); return 0u; });
                both([](NFC& n) { return n.Read32(0x2C); });
            } else if (!polledFlash->IsBusy()) {
                needCommand = true;
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
                "NFC PIO: registers differ at step " + std::to_string(step));
        Require(polledLevel == scheduledLevel, "NFC PIO: levels differ at step " + std::to_string(step));
    }
    Require(services * 10 < kSteps, "NFC PIO: the service runs about once a page, not once a byte: " +
                                        std::to_string(services) + " of " + std::to_string(kSteps));
    Require(quiet * 2 > kSteps, "NFC PIO: most accesses leave the host nothing to do");
}

// Memory DMA 0 (and 1) between SDRAM and L1, at the hardware's rate unless a
// speedup is given.
struct MdmaRig {
    static constexpr u32 kL1 = 0xFF800000;
    TimeSource time{true};
    Emulator emulator;
    MemoryDevice ram{"SDRAM", 0, 0x10000};
    MemoryDevice l1{"Data A", kL1, 0x4000};
    DMA dma{0xFFC00C00, emulator, &time};
    int level = 0, level1 = 0;

    explicit MdmaRig(unsigned speedup = 1) {
        emulator.BindDevice(&ram);
        emulator.BindDevice(&l1);
        dma.SetMdmaSpeedup(speedup);
        dma.BindInterrupt(12, 12, [this](int, int l) { level = l; });
        dma.BindInterrupt(14, 14, [this](int, int l) { level1 = l; });
        for (u32 i = 0; i < 0x10000; i += 4) ram.Write32(i, i * 2654435761u);
        for (u32 i = 0; i < 0x4000; i += 4) l1.Write32(i, ~i * 40503u);
    }
    void Start(u32 from, u32 to, u32 words) {
        dma.Write32(0x344, from);   // MDMA_S0_START_ADDR
        dma.Write32(0x350, words);  // MDMA_S0_X_COUNT
        dma.Write32(0x354, 4);      // MDMA_S0_X_MODIFY
        dma.Write32(0x304, to);     // MDMA_D0_START_ADDR
        dma.Write32(0x310, words);
        dma.Write32(0x314, 4);
        dma.Write32(0x348, 0x09);         // DMAEN, 32-bit
        dma.Write32(0x308, 0x8B);         // DMAEN, WNR, 32-bit, DI_EN
    }
    // Rows of x elements of 1 << wsize bytes, y of them (2D where y > 1),
    // on stream 0 or 1, armed in either order (Arm, then Arm(second) later
    // or at once); the destination interrupts at its end, at each row
    // (DI_SEL) or not at all; both restart at their ends (autobuffer) or
    // stop, or the destination goes on through a list of two descriptors
    // (listed: its START_ADDR, from kList). Modifies in bytes; kElement is
    // the element's size.
    static constexpr int16_t kElement = INT16_MIN;
    static constexpr u32 kList = kL1 + 0x3F00;
    struct Shape {
        u32 from, to, x1, y1, x2, y2;
        bool destFirst, interrupt, eachRow, autobuffer;
        int16_t ymod1 = 4, ymod2 = 4;
        u32 wsize = 2;
        u32 stream = 0;
        int16_t xmod1 = kElement, xmod2 = kElement;
        bool listed = false;
    };
    void Arm(const Shape& t, bool second = false) {
        const u32 d = 0x300 + t.stream * 0x80, s = d + 0x40;
        const u32 flow = t.autobuffer ? 0x1000 : 0;
        const u32 size = t.wsize << 2;
        const u32 source = 0x01 | size | (t.y1 > 1 ? 0x10 : 0) | flow;
        // The large descriptor model, four halfwords: the link and START_ADDR.
        const u32 destFlow = t.listed ? 0x7400 : flow;
        const u32 dest = 0x03 | size | (t.y2 > 1 ? 0x10 : 0) | (t.interrupt ? 0x80 : 0) | (t.eachRow ? 0x40 : 0) | destFlow;
        if (second) {
            dma.Write32(t.destFirst ? s + 0x08 : d + 0x08, t.destFirst ? source : dest);
            return;
        }
        const int16_t e = int16_t(1 << t.wsize);
        dma.Write32(s + 0x04, t.from);
        dma.Write32(s + 0x10, t.x1);
        dma.Write32(s + 0x14, u16(t.xmod1 == kElement ? e : t.xmod1));
        dma.Write32(s + 0x18, t.y1);
        dma.Write32(s + 0x1C, u16(t.ymod1));
        if (t.listed) {
            // Two descriptors in turn, each back to the destination's start.
            const u32 list = kList + 0x20 * t.stream;
            for (u32 i = 0; i < 2; ++i) {
                emulator.MemoryWrite32(list + 0x10 * i, list + 0x10 * (1 - i));
                emulator.MemoryWrite32(list + 0x10 * i + 4, t.to);
            }
            dma.Write32(d + 0x00, list);
        }
        dma.Write32(d + 0x04, t.to);
        dma.Write32(d + 0x10, t.x2);
        dma.Write32(d + 0x14, u16(t.xmod2 == kElement ? e : t.xmod2));
        dma.Write32(d + 0x18, t.y2);
        dma.Write32(d + 0x1C, u16(t.ymod2));
        dma.Write32(t.destFirst ? d + 0x08 : s + 0x08, t.destFirst ? dest : source);
    }
    // Guest cycles from now until MDMA_D0's DMA_DONE, serviced as the host does.
    uint64_t RunUntilDone(u32 status = 0x328, uint64_t limit = 1u << 22) {
        const uint64_t from = time.Cycles();
        while (!(dma.Read32(status) & 1)) {
            Require(time.Cycles() - from < limit, "memory DMA: the transfer ends");
            time.Retire(1);
            if (dma.NeedsService(time.Nanoseconds())) dma.ProcessWithInterrupt(-1);
        }
        return time.Cycles() - from;
    }
};

// A memory DMA the guest polls: it starts a transfer from MDMA_S0 to
// MDMA_D0, reads MDMA_D0_IRQ_STATUS every block and acknowledges DMA_DONE
// before the next one. Serviced through its contract (reads and the
// acknowledgement give it no work), the DMA moves the same bytes at the same
// guest times and shows the same status as one serviced after every block.
static void MdmaPollingMatchesPolling(uint32_t seed) {
    MdmaRig polled(10), scheduled(10);
    std::mt19937 rng(seed);
    uint64_t services = 0;
    int transfers = 0;
    constexpr int kSteps = 30000;
    for (int step = 0; step < kSteps; ++step) {
        const u32 status = polled.dma.Read32(0x328);
        Require(status == scheduled.dma.Read32(0x328),
                "seed " + std::to_string(seed) + ": MDMA_D0_IRQ_STATUS differs at step " + std::to_string(step));
        if (status & 1) {
            polled.dma.Write32(0x328, 1);
            scheduled.dma.Write32(0x328, 1);
        }
        if (!(status & 8) && rng() % 8 == 0) {
            const u32 words = 16 + rng() % 400, from = (rng() % 0x400) * 4, to = 0x2000 + (rng() % 0x200) * 4;
            polled.Start(from, to, words);
            scheduled.Start(from, to, words);
            ++transfers;
        }
        const uint64_t packets = 1 + rng() % 40;
        polled.time.Retire(packets);
        scheduled.time.Retire(packets);
        polled.dma.ProcessWithInterrupt(-1);
        if (scheduled.dma.NeedsService(scheduled.time.Nanoseconds())) {
            scheduled.dma.ProcessWithInterrupt(-1);
            ++services;
        }
        Require(polled.level == scheduled.level,
                "seed " + std::to_string(seed) + ": MDMA levels differ at step " + std::to_string(step));
    }
    for (u32 i = 0; i < 0x4000; i += 4)
        Require(polled.ram.Read32(i) == scheduled.emulator.MemoryRead32(i), "seed " + std::to_string(seed) + ": memory differs at " + std::to_string(i));
    Require(transfers > 100, "the guest ran transfers");
    Require(services * 2 < kSteps, "the polled MDMA is serviced when a row is due, not at every poll: " +
                                       std::to_string(services) + " of " + std::to_string(kSteps));
}

// A 32-bit guest load or store as generated code makes it: straight to the
// page where DirectPages() has it, else through the Emulator.
static u32 GuestRead(Emulator& emulator, u32 addr) {
    if (u8* page = emulator.DirectPages()[addr >> PAGE_BITS]) {
        u32 value;
        std::memcpy(&value, page + (addr & ((1u << PAGE_BITS) - 1)), 4);
        return value;
    }
    bool hostWork, stable;
    return emulator.ReadAttended<u32>(addr, hostWork, stable);
}

static void GuestWrite(Emulator& emulator, u32 addr, u32 value) {
    if (u8* page = emulator.DirectPages()[addr >> PAGE_BITS]) {
        std::memcpy(page + (addr & ((1u << PAGE_BITS) - 1)), &value, 4);
        return;
    }
    bool hostWork;
    emulator.WriteAttended<u32>(addr, value, hostWork);
}

// The HRM's memory DMA timing (6-44 to 6-47): a stream moves a 16-bit word
// per SCLK (five core cycles at the firmware's PLL_DIV) between L1 and
// external memory and one per two SCLKs within either, a 32-bit element as
// two words; it starts three SCLKs after the CONFIG write that completes the
// pair, and a speedup divides all of it (rounded up to a cycle). The
// source alone moves nothing; memory DMA 1 waits while memory DMA 0 runs.
static void MdmaHrmTiming() {
    struct Case { u32 from, to, wsize; unsigned speedup; uint64_t cycles; const char* what; };
    const u32 L1 = MdmaRig::kL1;
    const Case cases[] = {
        {L1, 0x1000, 2, 1, 15 + 100 * 10, "32-bit, L1 to SDRAM"},
        {0x1000, L1 + 0x1000, 2, 1, 15 + 100 * 10, "32-bit, SDRAM to L1"},
        {0x1000, 0x8000, 2, 1, 15 + 100 * 20, "32-bit, SDRAM to SDRAM"},
        {L1, L1 + 0x1000, 2, 1, 15 + 100 * 20, "32-bit, L1 to L1"},
        {L1, 0x1000, 1, 1, 15 + 100 * 5, "16-bit, L1 to SDRAM"},
        {0x1000, 0x8000, 0, 1, 15 + 100 * 10, "8-bit, SDRAM to SDRAM"},
        {L1, 0x1000, 2, 10, (15 + 100 * 10 + 9) / 10, "32-bit, L1 to SDRAM, ten times faster"},
        {0x1000, 0x8000, 1, 3, (15 + 100 * 10 + 2) / 3, "16-bit, SDRAM to SDRAM, three times faster"},
    };
    for (const Case& c : cases) {
        MdmaRig rig(c.speedup);
        rig.time.Retire(1000);
        const MdmaRig::Shape t{c.from, c.to, 100, 1, 100, 1, false, true, false, false, 0, 0, c.wsize};
        rig.Arm(t);
        rig.time.Retire(3);  // the firmware's gap: the source, then the destination
        rig.Arm(t, true);
        const uint64_t took = rig.RunUntilDone();
        Require(took == c.cycles, std::string("memory DMA timing, ") + c.what + ": " + std::to_string(took) +
                                      " cycles, not " + std::to_string(c.cycles));
        for (u32 i = 0; i < 100u << c.wsize; ++i) {
            u8 a, b;
            rig.emulator.MemoryRead(c.from + i, &a, 1);
            rig.emulator.MemoryRead(c.to + i, &b, 1);
            Require(a == b, std::string("memory DMA timing, ") + c.what + ": the copy");
        }
    }
    {
        // A descriptor fetched at a work unit's end: an SCLK per halfword,
        // four of them (the link and START_ADDR), before the next element.
        MdmaRig rig;
        MdmaRig::Shape t{L1, 0x1000, 100, 1, 100, 1, false, true, false, true};
        t.listed = true;
        rig.Arm(t);
        rig.Arm(t, true);
        Require(rig.RunUntilDone() == 15 + 100 * 10, "memory DMA through descriptors: the first work unit");
        rig.dma.Write32(0x328, 1);
        Require(rig.RunUntilDone() == 4 * 5 + 100 * 10, "memory DMA through descriptors: the next one, after its descriptor");
    }
    {
        // The destination is what starts it.
        MdmaRig rig;
        const MdmaRig::Shape t{L1, 0x1000, 100, 1, 100, 1, false, true, false, false};
        rig.Arm(t);
        for (int i = 0; i < 500; ++i) {
            rig.time.Retire(1);
            if (rig.dma.NeedsService(rig.time.Nanoseconds())) rig.dma.ProcessWithInterrupt(-1);
        }
        Require(rig.dma.Read32(0x370) == 100 && (rig.dma.Read32(0x368) & 8),
                "memory DMA: the source alone reads nothing and keeps running");
        rig.Arm(t, true);
        Require(rig.RunUntilDone() == 15 + 100 * 10, "memory DMA: it starts at the destination's CONFIG write");
    }
    {
        // Fixed priority (DMA_TC_PER's MDMA_ROUND_ROBIN_PERIOD 0, as the
        // firmware sets it): memory DMA 1 stops while memory DMA 0 runs.
        MdmaRig rig;
        MdmaRig::Shape one{L1, 0x4000, 100, 1, 100, 1, false, false, false, false};
        one.stream = 1;
        const MdmaRig::Shape zero{L1 + 0x1000, 0x1000, 100, 1, 100, 1, false, false, false, false};
        rig.Arm(one);
        rig.Arm(one, true);
        rig.time.Retire(300);
        rig.Arm(zero);
        rig.Arm(zero, true);
        const uint64_t zeroDone = rig.RunUntilDone();
        Require(zeroDone == 15 + 1000, "memory DMA 0 runs at its own rate beside memory DMA 1");
        Require(!(rig.dma.Read32(0x3A8) & 1), "memory DMA 1 is not done while memory DMA 0 ran");
        // Memory DMA 1 moved 28 elements in the 285 cycles before (10 each),
        // and the remaining 72 after memory DMA 0's end.
        Require(rig.RunUntilDone(0x3A8) == 72 * 10, "memory DMA 1 goes on where memory DMA 0 ends");
    }
}

// A memory DMA settles its rows (DMA::SetSettling): serviced only at the
// cycles NeedsService() says, it moves the same bytes, interrupts at the
// same cycles and shows the same status, counts and memory at every guest
// access -- loads and stores of what it copies included, by generated code
// or the host -- as the DMA that moves each element at its own slot,
// serviced every cycle; with a service about once a transfer, not once a
// row. Like the synth's fill (8-byte source rows read over and over into 99-word
// ones), with elements of any size, rows apart, backwards or the same,
// copies within one memory and across, overlapping, both streams at once,
// sparse accesses or dense, and transfers aborted now and then.
static void MdmaSettlesLikeEveryCycle(uint32_t seed) {
    std::mt19937 rng(seed);
    static constexpr unsigned kSpeedups[] = {1, 3, 10};
    const unsigned speedup = kSpeedups[seed % 3];
    MdmaRig every(speedup), settled(speedup);
    every.dma.SetSettling(false);
    const u32 accessEvery = seed % 2 ? 8 : 512;
    uint64_t services = 0, rows = 0, rowEvents = 0;
    int transfers = 0, reads = 0, accesses = 0, listed = 0, still = 0, backwards = 0;
    bool autobuffer[2] = {false, false};
    std::optional<std::pair<MdmaRig::Shape, uint64_t>> second[2];  // the side armed later, and when
    constexpr int kSteps = 20000;
    const auto where = [&](int step) { return "seed " + std::to_string(seed) + " step " + std::to_string(step); };
    // Where a side's elements lie relative to its start: x of them xmod
    // apart, rows `stride` apart (Y_MODIFY is stride - (x - 1) * xmod), as
    // far below the start as above it at most.
    const auto reach = [](u32 x, u32 y, int xmod, int stride, int e) {
        return u32((int(x) - 1) * std::abs(xmod) + (int(y) - 1) * std::abs(stride) + e);
    };
    // An address in SDRAM or L1 with `bytes` on either side (L1's top holds
    // the descriptor lists).
    const auto place = [&](u32 bytes, bool l1) {
        const u32 room = l1 ? MdmaRig::kList - MdmaRig::kL1 : 0x10000;
        if (2 * bytes + 64 > room) return u32(UINT32_MAX);
        const u32 at = bytes + (rng() % ((room - 2 * bytes) / 4)) * 4;
        return (l1 ? MdmaRig::kL1 : 0) + at;
    };
    for (int step = 0; step < kSteps; ++step) {
        for (u32 stream = 0; stream < 2; ++stream) {
            const u32 d = 0x300 + stream * 0x80, s = d + 0x40;
            const u32 status = every.dma.Read32(d + 0x28);
            Require(status == settled.dma.Read32(d + 0x28), "settling MDMA: D_IRQ_STATUS differs, " + where(step));
            Require(every.dma.Read32(s + 0x28) == settled.dma.Read32(s + 0x28),
                    "settling MDMA: S_IRQ_STATUS differs, " + where(step));
            if (rng() % 3 == 0) {
                static constexpr u32 kCurrent[] = {0x24, 0x30, 0x38};
                // In this order on every compiler (the operands of + are not).
                const u32 channel = rng() % 2 ? d : s;
                const u32 reg = channel + kCurrent[rng() % 3];
                Require(every.dma.Read32(reg) == settled.dma.Read32(reg),
                        "settling MDMA: current registers differ, " + where(step));
                ++reads;
            }
            if (status & 1) {
                every.dma.Write32(d + 0x28, 1);
                settled.dma.Write32(d + 0x28, 1);
            }
            if ((status & 8) && rng() % (autobuffer[stream] ? 40 : 400) == 0) {
                for (auto* rig : {&every, &settled}) rig->dma.Write32(d + 0x08, 0), rig->dma.Write32(s + 0x08, 0);
                second[stream].reset();
            } else if (!(status & 8) && !second[stream] && rng() % (stream ? 16 : 4) == 0) {
                MdmaRig::Shape t{};
                t.stream = stream;
                t.wsize = rng() % 3;
                const int16_t e = int16_t(1 << t.wsize);
                t.x1 = 1 + rng() % 8;
                t.y1 = 1 + rng() % 60;
                const u32 total = t.x1 * t.y1;
                t.x2 = 1 + rng() % total;
                while (total % t.x2) --t.x2;
                t.y2 = total / t.x2;
                t.destFirst = rng() % 2;
                t.interrupt = rng() % 4 != 0;
                t.autobuffer = autobuffer[stream] = rng() % 8 == 0;
                // Rows that interrupt in a transfer that restarts would do so
                // until it is aborted: their services are not counted below.
                t.eachRow = !t.autobuffer && rng() % 4 == 0;
                // Elements next to each other, the same one, backwards or
                // apart; rows next to each other, apart, the same ones again
                // (the synth's fill) or backwards. The destination's
                // elements as the source's, but never the same one.
                static constexpr int kXmods[] = {1, 0, -1, 3};
                static constexpr int kStrides[] = {1, 3, 0, -2};
                const int xm1 = kXmods[rng() % 4] * e;
                const int xm2 = kXmods[rng() % 3 == 0 ? 2 + rng() % 2 : 0] * e;
                const int stride1 = kStrides[rng() % 4] * int(t.x1) * e;
                const int stride2 = kStrides[rng() % 4 == 2 ? 3 : rng() % 2] * int(t.x2) * e;
                t.xmod1 = int16_t(xm1);
                t.xmod2 = int16_t(xm2);
                t.ymod1 = int16_t(stride1 - (int(t.x1) - 1) * xm1);
                t.ymod2 = int16_t(stride2 - (int(t.x2) - 1) * xm2);
                const u32 reach1 = reach(t.x1, t.y1, xm1, stride1, e), reach2 = reach(t.x2, t.y2, xm2, stride2, e);
                const bool l1 = rng() % 2;
                const bool forward = xm1 >= 0 && xm2 > 0 && stride1 >= 0 && stride2 > 0;
                if (rng() % 6 == 0 && forward) {
                    // The destination within the elements the source is still to read.
                    t.from = place(reach1 + reach2 + 64, l1);
                    t.to = t.from + u32(e) * (1 + rng() % 8);
                } else {
                    t.from = place(reach1, l1);
                    t.to = place(reach2, rng() % 2);
                }
                if (t.from == UINT32_MAX || t.to == UINT32_MAX) continue;  // too far apart for the memory
                t.listed = t.autobuffer && rng() % 3 == 0;
                listed += t.listed;
                still += xm1 == 0;
                backwards += xm1 < 0 || xm2 < 0;
                every.Arm(t);
                settled.Arm(t);
                second[stream].emplace(t, every.time.Cycles() + (rng() % 2 ? rng() % 12 : rng() % 100));
                ++transfers;
                rows += t.y1;
                if (t.interrupt && t.eachRow) rowEvents += t.y2;
            }
        }
        // Cycle by cycle: the reference is serviced at each, the settling
        // DMA where it asks to be, and their interrupt levels agree at each.
        for (uint64_t i = 1 + rng() % 200; i; --i) {
            for (auto& later : second)
                if (later && every.time.Cycles() >= later->second) {
                    every.Arm(later->first, true);
                    settled.Arm(later->first, true);
                    later.reset();
                }
            every.time.Retire(1);
            every.dma.ProcessWithInterrupt(-1);
            settled.time.Retire(1);
            if (settled.dma.NeedsService(settled.time.Nanoseconds())) {
                settled.dma.ProcessWithInterrupt(-1);
                ++services;
            }
            Require(every.level == settled.level && every.level1 == settled.level1,
                    "settling MDMA: levels differ, " + where(step));
            if (rng() % accessEvery == 0) {
                const bool l1 = rng() % 2;
                const u32 addr = (l1 ? MdmaRig::kL1 + (rng() % 0x1000) * 4 : (rng() % 0x4000) * 4);
                const bool host = rng() % 4 == 0;
                if (rng() % 3 == 0) {
                    const u32 value = rng();
                    for (auto* rig : {&every, &settled}) {
                        if (host) rig->emulator.MemoryWrite32(addr, value);
                        else GuestWrite(rig->emulator, addr, value);
                    }
                } else {
                    const u32 a = host ? every.emulator.MemoryRead32(addr) : GuestRead(every.emulator, addr);
                    const u32 b = host ? settled.emulator.MemoryRead32(addr) : GuestRead(settled.emulator, addr);
                    Require(a == b, "settling MDMA: memory at " + std::to_string(addr) + " differs, " + where(step));
                }
                ++accesses;
            }
        }
    }
    // Read as the host does, which moves the rows due first.
    for (u32 i = 0; i < 0x10000; i += 4)
        Require(every.ram.Read32(i) == settled.emulator.MemoryRead32(i),
                "seed " + std::to_string(seed) + ": settled SDRAM differs");
    for (u32 i = 0; i < 0x4000; i += 4)
        Require(every.l1.Read32(i) == settled.emulator.MemoryRead32(MdmaRig::kL1 + i),
                "seed " + std::to_string(seed) + ": settled L1 differs");
    Require(transfers > 100 && reads > 1000 && accesses > (accessEvery == 8 ? 1000 : 10),
            "the guest ran transfers, read their counts and accessed memory");
    Require(listed > 2 && still > 10 && backwards > 10, "transfers through descriptors, of one element, backwards");
    // Destination rows that interrupt (DI_SEL) need a service each.
    Require(services < rows + rowEvents, "settling MDMA: fewer services than source rows and rows that interrupt: " +
                                             std::to_string(services) + " of " + std::to_string(rows) + " + " +
                                             std::to_string(rowEvents));
}

// The firmware's first fill of the synth phase: armed, then armed again with
// another destination 12 cycles later, before the first moved much. Each
// side starts over at its CONFIG write, and the pair at the destination's
// (HRM 6-47): the source does not run on into the first destination in
// between, at any speedup.
static void MdmaArmedAgain() {
    for (unsigned speedup : {1u, 3u, 10u, 100u}) {
        MdmaRig rig(speedup);
        rig.l1.Write32(0x100, 0x11223344);
        rig.l1.Write32(0x104, 0x55667788);
        MdmaRig::Shape fill{MdmaRig::kL1 + 0x100, 0x2000, 2, 1980, 99, 40, false, true, false, false, -4, 120};
        rig.Arm(fill);
        rig.time.Retire(3);
        rig.Arm(fill, true);
        rig.time.Retire(9);
        fill.to = 0x8000;
        rig.Arm(fill);
        rig.time.Retire(3);
        rig.Arm(fill, true);
        const std::string what = "memory DMA armed again, " + std::to_string(speedup) + " times faster: ";
        rig.RunUntilDone();
        for (u32 i = 0; i < 99; ++i)
            Require(rig.ram.Read32(0x8000 + 39 * 512 + i * 4) == ((39 * 99 + i) % 2 ? 0x55667788u : 0x11223344u),
                    what + "the second fill");
    }
}

// The synth's fill, as the firmware makes it: the source (2-word rows, the
// same 8 bytes of L1 over and over) armed first, then the destination
// (99-word rows of SDRAM, 40 of them, an interrupt at the end) 3 cycles
// later, IRQ_STATUS polled every few cycles and acknowledged. A transfer
// takes its HRM time, a few services (after each arming write, at its end)
// and as few steps: its rows move together, not one by one.
static void MdmaSynthFill() {
    MdmaRig rig;
    uint64_t services = 0;
    constexpr int kTransfers = 20;
    rig.l1.Write32(0x100, 0x11223344);
    rig.l1.Write32(0x104, 0x55667788);
    const uint64_t steps = rig.dma.MdmaSteps();
    for (int n = 0; n < kTransfers; ++n) {
        MdmaRig::Shape fill{MdmaRig::kL1 + 0x100, 0x4000, 2, 1980, 99, 40, false, true, false, false, -4, 120};
        rig.Arm(fill);
        uint64_t cycles = 0;
        for (; !(rig.dma.Read32(0x328) & 1); cycles += 5) {
            if (cycles == 0) rig.time.Retire(3), rig.Arm(fill, true);
            rig.time.Retire(5);
            if (rig.dma.NeedsService(rig.time.Nanoseconds())) {
                rig.dma.ProcessWithInterrupt(-1);
                ++services;
            }
        }
        Require(cycles >= 15 + 3960 * 10 && cycles < 15 + 3960 * 10 + 5,
                "synth fill: the HRM's time, " + std::to_string(cycles) + " cycles");
        Require(rig.level == 1, "synth fill: the transfer's interrupt");
        rig.dma.Write32(0x328, 1);
    }
    for (u32 row = 0; row < 40; ++row)
        for (u32 i = 0; i < 99; ++i)
            Require(rig.ram.Read32(0x4000 + row * 512 + i * 4) == ((row * 99 + i) % 2 ? 0x55667788u : 0x11223344u),
                    "synth fill: the pattern");
    Require(services <= 4 * kTransfers, "synth fill: services per transfer: " + std::to_string(services));
    Require(rig.dma.MdmaSteps() - steps <= 4 * kTransfers,
            "synth fill: steps per transfer: " + std::to_string(rig.dma.MdmaSteps() - steps));
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
        sport->SetAudioOutputCallback([this](const void* data, size_t samples, int channels, int bits, bool) {
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
            // Stereo frame sync and the secondary channel: frames of 1, 2 or 4 words.
            if (event == 2) r.sport->Write32(0x04, 15 | (step % 2 ? 256 : 0) | (step % 3 ? 0 : 512));
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

// SPORT0 transmit through DMA reads only the samples due (DMABus::
// DMAWriteTake): what the audio sink gets must still be the buffer's words in
// order, for a contiguous and a strided buffer, whatever the service times.
static void SportTransmitDeliversTheBuffer(u32 modify) {
    AudioRig rig;
    std::vector<uint16_t> heard;
    rig.sport->SetAudioOutputCallback([&](const void* data, size_t samples, int channels, int, bool) {
        auto* words = static_cast<const uint16_t*>(data);
        heard.insert(heard.end(), words, words + samples * channels);
    });
    rig.sport->Write32(0x04, 15);  // TCR2: 16-bit words
    rig.sport->Write32(0x00, 1);   // TSPEN
    const u32 count = 48, start = 0x1000, base = 4 * 0x40;
    rig.dma.Write32(base + 0x04, start);
    rig.dma.Write32(base + 0x10, count);
    rig.dma.Write32(base + 0x14, modify);
    rig.dma.Write32(base + 0x08, 1 | (1 << 2) | (1 << 12));  // 16-bit, autobuffer
    std::mt19937 rng(modify);
    for (int step = 0; step < 20000; ++step) {
        rig.time.Retire(rng() % 3 ? rng() % 9000 + 1 : rng() % 200000 + 1);
        rig.dma.ProcessWithInterrupt(-1);
    }
    Require(heard.size() > 3 * count, "the sink hears the buffer go round several times");
    for (size_t i = 0; i < heard.size(); ++i) {
        const u32 addr = start + (i % count) * modify;
        Require(heard[i] == uint16_t(rig.ram.Read32(addr & ~3u) >> (addr & 2 ? 16 : 0)),
                "sample " + std::to_string(i) + " is the buffer's word (modify " + std::to_string(modify) + ")");
    }
}

// The OP-1's codec link: SPORT0 with the secondary channels on (TXSE, RXSE),
// 32-bit words, no stereo frame sync. A frame is a primary word and a
// secondary word, interleaved in memory, at the codec's 44.1 kHz: one second
// of guest time moves 44100 two-word frames each way, and the sink gets the
// buffer's words in order, two to a frame.
static void SportSecondaryChannelsFrameTwoWords() {
    AudioRig rig;
    uint64_t heard = 0, taken = 0;
    std::vector<uint32_t> words;
    rig.sport->SetAudioOutputCallback([&](const void* data, size_t samples, int channels, int bits, bool stereo) {
        Require(channels == 2 && bits == 32 && !stereo, "a frame is a primary and a secondary word, not left and right");
        auto* w = static_cast<const uint32_t*>(data);
        if (words.size() < 64) words.insert(words.end(), w, w + samples * channels);
        heard += samples;
    });
    rig.sport->SetAudioInputCallback([&](void* data, size_t samples, int channels, int bits) {
        Require(channels == 2 && bits == 32, "a received frame has a primary and a secondary word");
        std::memset(data, 0, samples * channels * 4);
        taken += samples;
        return samples;
    });
    rig.sport->Write32(0x04, 31 | 512);  // TCR2: 32-bit words, TXSE
    rig.sport->Write32(0x24, 31 | 512);  // RCR2: 32-bit words, RXSE
    rig.sport->Write32(0x00, 1);         // TSPEN
    rig.sport->Write32(0x20, 1);         // RSPEN
    for (int channel : {3, 4}) {
        const u32 base = channel * 0x40;
        rig.dma.Write32(base + 0x04, channel == 3 ? 0x4000 : 0x1000);
        rig.dma.Write32(base + 0x10, 128);
        rig.dma.Write32(base + 0x14, 4);
        // 32-bit words, autobuffer.
        rig.dma.Write32(base + 0x08, 1 | (channel == 3 ? 2 : 0) | (2 << 2) | (1 << 12));
    }
    // Both directions start their clocks at their first transfer, here.
    rig.dma.ProcessWithInterrupt(-1);
    for (int ms = 0; ms < 1000; ++ms) {
        rig.time.Retire(TimeSource::kHz / 1000);
        rig.dma.ProcessWithInterrupt(-1);
    }
    // A service moves the frames due up to the buffer's end, the rest come at
    // the next: at most one buffer (64 frames) of the second is still to go.
    auto aSecond = [](uint64_t frames) { return frames <= 44100 && frames + 64 >= 44100; };
    Require(aSecond(heard), "transmit: 44100 frames in a second, not " + std::to_string(heard));
    Require(aSecond(taken), "receive: 44100 frames in a second, not " + std::to_string(taken));
    for (size_t i = 0; i < words.size(); ++i)
        Require(words[i] == rig.ram.Read32(0x1000 + u32(i % 128) * 4), "the sink hears the buffer's words in order");
}

// With no audio input attached, SPORT receive hands the DMA silence: zero
// words in the buffer it is given, not whatever that held (the DMA stages a
// transfer on its stack; the firmware mixes its input into what it plays).
static void SportReceiveWithoutInputIsSilence() {
    TimeSource time{true};
    SPORT sport(0xFFC00800, 0, time);
    sport.Write32(0x24, 31 | 512);  // RCR2: 32-bit words, RXSE
    sport.Write32(0x20, 1);         // RSPEN
    uint32_t buffer[64];
    Require(sport.DMARead(0, 0, buffer, sizeof buffer) == 0, "the first transfer only starts the clock");
    time.Retire(TimeSource::kHz / 1000);
    std::fill(std::begin(buffer), std::end(buffer), 0xDEADBEEFu);
    const u32 moved = sport.DMARead(0, 0, buffer, sizeof buffer);
    Require(moved == sizeof buffer, "a millisecond has a buffer's worth of frames due");
    for (uint32_t word : buffer) Require(word == 0, "a received word is silence");
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
// The same in real-time mode, which paces guest time and nothing else.
static void CoreTimerCountsGuestCycles(bool deterministic) {
    TimeSource time(deterministic);
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
    CoreTimerCountsGuestCycles(true);
    CoreTimerCountsGuestCycles(false);
    SicLines();
    for (uint32_t seed = 1; seed <= 20; ++seed) RtcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 20; ++seed) NfcMatchesPolling(seed);
    for (u32 irqmask : {0x1Fu, 0x17u, 0x16u, 0x00u}) NfcPioMatchesPolling(irqmask);
    for (uint32_t seed = 1; seed <= 10; ++seed) MdmaPollingMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 10; ++seed) MdmaSettlesLikeEveryCycle(seed);
    MdmaHrmTiming();
    MdmaArmedAgain();
    MdmaSynthFill();
    for (uint32_t seed = 1; seed <= 10; ++seed) AudioMatchesPolling(seed);
    SportTransmitDeliversTheBuffer(2);
    SportTransmitDeliversTheBuffer(6);
    SportSecondaryChannelsFrameTwoWords();
    SportReceiveWithoutInputIsSilence();
    std::puts("device-service-test: ok");
}
