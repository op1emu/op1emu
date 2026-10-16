// A device serviced only when it has work must show the guest exactly what
// the same device shows when it is serviced after every block: each test
// drives two instances with the same inputs, one polled at every step and one
// through its service contract, and compares what the guest can observe.
#include "cpu/nand.h"
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

extern int cec_current_ivg();

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
    std::vector<int> polledLevels, scheduledLevels;
    polled.BindInterrupt(14, [&](int, int level) { polledLevels.push_back(level); });
    scheduled.BindInterrupt(14, [&](int, int level) { scheduledLevels.push_back(level); });

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
        } else {
            scheduled.ReplayInterrupt();
        }
        Require(RtcRegisters(polled) == RtcRegisters(scheduled),
                "seed " + std::to_string(seed) + ": registers differ at step " + std::to_string(step));
        Require(polledLevels == scheduledLevels,
                "seed " + std::to_string(seed) + ": published levels differ at step " + std::to_string(step));
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
    std::vector<int> polledLevels, scheduledLevels;
    polled.BindInterrupt(48, [&](int, int level) { polledLevels.push_back(level); });
    scheduled.BindInterrupt(48, [&](int, int level) { scheduledLevels.push_back(level); });
    // The NFC's ISR is running when the resolver names the current IVG.
    const int current = cec_current_ivg();
    int routedTo = current + 1;
    uint64_t routing = 0;
    polled.SetIVGResolver([&] { return routedTo; });
    scheduled.SetIVGResolver([&] { return routedTo; });
    scheduled.SetRoutingVersion(&routing);

    std::mt19937 rng(seed);
    uint64_t services = 0;
    for (int step = 0; step < 20000; ++step) {
        // What one block does to the NFC.
        const int ops = rng() % 4 == 0 ? rng() % 3 + 1 : 0;
        for (int op = 0; op < ops; ++op) {
            const int kind = rng() % 9;
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
            } else if (kind == 7) {
                // The firmware routes the NFC to another IVG (an SIC_IAR write).
                routedTo = routedTo == current ? current + 1 : current;
                ++routing;
            } else {
                polled.Write32(0x24, 1);
                scheduled.Write32(0x24, 1);
            }
        }
        polled.ProcessWithInterrupt(current);
        if (scheduled.NeedsService(current)) {
            scheduled.ProcessWithInterrupt(current);
            ++services;
        } else {
            scheduled.ReplayInterrupt();
        }
        polledFlash->EndOfBlock();
        scheduledFlash->EndOfBlock();
        Require(NfcRegisters(polled) == NfcRegisters(scheduled),
                "seed " + std::to_string(seed) + ": NFC registers differ at step " + std::to_string(step));
        Require(polledLevels == scheduledLevels,
                "seed " + std::to_string(seed) + ": NFC levels differ at step " + std::to_string(step));
    }
    Require(services < 20000 * 3 / 4, "the scheduled NFC must skip calls with nothing to do: " + std::to_string(services));
}

int main() {
    for (uint32_t seed = 1; seed <= 20; ++seed) RtcMatchesPolling(seed);
    for (uint32_t seed = 1; seed <= 20; ++seed) NfcMatchesPolling(seed);
    std::puts("device-service-test: ok");
}
