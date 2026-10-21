// Register and RegisterDevice dispatch through their flattened tables: every
// read and write must call the same field callbacks, in the same order and
// with the same values, as walking the maps did, and find the same register.
#include "cpu/emu.h"
#include "cpu/io.h"
#include <cstdio>
#include <cstdlib>
#include <random>
#include <tuple>
#include <string>
#include <vector>

static void Require(bool ok, const std::string& message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

// The map walk the tables replaced, as compiled for x86 (a shift count of 32
// shifts nothing).
static u32 ReferenceMask(int offset, int length) {
    const unsigned count = unsigned(32 - length) & 31;
    return ((~0U) >> count) << offset;
}
static u32 ReferenceRead(const Register& reg) {
    if (reg.readCallback) return reg.readCallback();
    u32 value = 0;
    for (const auto& [name, f] : reg.fields) {
        const u32 mask = ReferenceMask(f.offset, f.length);
        value = (value & ~mask) | ((f.readCallback() << f.offset) & mask);
    }
    return value;
}
static void ReferenceWrite(const Register& reg, u32 value) {
    for (const auto& [name, f] : reg.fields) f.writeCallback((value & ReferenceMask(f.offset, f.length)) >> f.offset);
    if (reg.writeCallback) reg.writeCallback(value);
}

// Fields in random places, of every length including 0 (USB_INTRUSB.SOF_B),
// overlapping, under names whose order is not their bit order.
static void FieldsMatchMapWalk(uint32_t seed) {
    std::mt19937 rng(seed);
    std::vector<std::string> log;
    std::vector<u32> sources(8);
    Register reg{};
    const int count = 1 + rng() % 8;
    for (int i = 0; i < count; ++i) {
        const std::string name = std::string(1, char('a' + rng() % 26)) + std::to_string(i);
        const int length = rng() % 33, offset = length == 32 ? 0 : rng() % (32 - length + (length == 0));
        reg.fields[name] = {offset, length, [&sources, i] { return sources[i]; },
                            [&log, name](u32 v) { log.push_back(name + "=" + std::to_string(v)); }};
    }
    if (rng() % 2) reg.writeCallback = [&log](u32 v) { log.push_back("reg=" + std::to_string(v)); };
    for (int step = 0; step < 200; ++step) {
        for (auto& source : sources) source = rng();
        Require(reg.Read32() == ReferenceRead(reg), "seed " + std::to_string(seed) + ": a read differs");
        const u32 value = rng();
        log.clear();
        reg.Write32(value);
        const auto flat = log;
        log.clear();
        ReferenceWrite(reg, value);
        Require(flat == log, "seed " + std::to_string(seed) + ": a write calls other callbacks or in another order");
        // A field added after the first access joins the table.
        if (step == 100) {
            reg.fields["0late"] = {4, 4, [] { return 0xFu; }, [&log](u32 v) { log.push_back("late=" + std::to_string(v)); }};
            Require(reg.Read32() == ReferenceRead(reg), "a field added late is read");
        }
    }
    // A copy reads its own fields.
    Register copy = reg;
    Require(copy.Read32() == ReferenceRead(copy), "a copied register reads like the original");
}

class TestDevice : public RegisterDevice {
public:
    TestDevice() : RegisterDevice("test", 0, 0x100) {
        for (u32 addr : {0x00u, 0x04u, 0x0Cu, 0x40u, 0x41u, 0x80u}) {
            REG32(r, addr);
            r.readCallback = [addr] { return 0x1000 + addr; };
            r.writeCallback = [this, addr](u32 v) { written.push_back(addr << 16 | (v & 0xFFFF)); };
        }
    }
    void AddLate() {
        REG32(late, 0xC0);
        late.readCallback = [] { return 0xABCDu; };
    }
    std::vector<u32> written;
};

static void DeviceFindsRegisters() {
    TestDevice dev;
    for (u32 offset = 0; offset < 0x100; ++offset) {
        const bool exists = offset == 0x00 || offset == 0x04 || offset == 0x0C || offset == 0x40 || offset == 0x41 ||
                            offset == 0x80;
        Require(dev.Read32(offset) == (exists ? 0x1000 + offset : 0), "register at " + std::to_string(offset));
        dev.Write32(offset, offset);
    }
    Require(dev.written == std::vector<u32>({0x00000000, 0x00040004, 0x000C000C, 0x00400040, 0x00410041, 0x00800080}),
            "writes reach the registers at their offsets and nowhere else");
    Require(dev.Read32(0xC0) == 0, "no register at 0xC0 yet");
    dev.AddLate();
    Require(dev.Read32(0xC0) == 0xABCD, "a register added after the first access is found");
    u16 half = 0;
    dev.Read(0x04, &half, sizeof(half));
    Require(half == 0x1004, "a 16-bit read takes the low half of the register");
}

// A device at the end of an MMR range that says the host has work after
// writes only, logging what reaches it.
class MmrDevice : public RegisterDevice {
public:
    MmrDevice(u32 base, std::vector<std::string>& log) : RegisterDevice("mmr", base, 0x10), log_(log) {
        for (u32 addr = 0; addr < 0x10; addr += 4) {
            REG32(r, addr);
            r.readCallback = [this, addr] { log_.push_back("r" + std::to_string(addr)); return 0x11223344u + addr; };
            r.writeCallback = [this, addr](u32 v) { log_.push_back("w" + std::to_string(addr) + "=" + std::to_string(v)); };
        }
    }
    bool HostWorkAfterAccess(u32 offset, bool write) const override { return write; }

private:
    std::vector<std::string>& log_;
};

// One lookup for an access and the question after it gives what the access
// and Emulator::HostWorkAfter give, wherever the access lands.
static void AttendedAccessesMatch(bool hook) {
    std::vector<std::string> fastLog, slowLog;
    Emulator fast, slow;
    MemoryDevice fastRam("SDRAM", 0, 0x2000), slowRam("SDRAM", 0, 0x2000);
    MmrDevice fastMmr(0xFFC00100, fastLog), slowMmr(0xFFC00100, slowLog);
    for (auto [emu, ram, mmr] : {std::tuple{&fast, &fastRam, &fastMmr}, std::tuple{&slow, &slowRam, &slowMmr}}) {
        emu->BindDevice(ram);
        emu->BindDevice(mmr);
        if (hook) emu->AddReadHook(0xFFC00104, [](u32) { return 0xABCDu; });
    }
    for (u32 addr : {0x100u, 0xFFC000FEu, 0xFFC00100u, 0xFFC00102u, 0xFFC00104u, 0xFFC0010Cu, 0xFFC0010Du,
                     0xFFC0010Eu, 0xFFC0010Fu, 0xFFC00110u, 0xFFC00200u}) {
        bool fastWork = false, fastStable = false;
        const std::string where = std::to_string(addr) + (hook ? " (hook)" : "");
        Require(fast.ReadAttended<u8>(addr, fastWork, fastStable) == slow.MemoryRead8(addr) &&
                    fastWork == slow.HostWorkAfter(addr, 1, false), "8-bit read at " + where);
        Require(fast.ReadAttended<u16>(addr, fastWork, fastStable) == slow.MemoryRead16(addr) &&
                    fastWork == slow.HostWorkAfter(addr, 2, false), "16-bit read at " + where);
        Require(fast.ReadAttended<u32>(addr, fastWork, fastStable) == slow.MemoryRead32(addr) &&
                    fastWork == slow.HostWorkAfter(addr, 4, false), "32-bit read at " + where);
        fast.WriteAttended<u8>(addr, 0x5A, fastWork);
        slow.MemoryWrite8(addr, 0x5A);
        Require(fastWork == slow.HostWorkAfter(addr, 1, true), "8-bit write at " + where);
        fast.WriteAttended<u16>(addr, 0x1234, fastWork);
        slow.MemoryWrite16(addr, 0x1234);
        Require(fastWork == slow.HostWorkAfter(addr, 2, true), "16-bit write at " + where);
        fast.WriteAttended<u32>(addr, 0x89ABCDEF, fastWork);
        slow.MemoryWrite32(addr, 0x89ABCDEF);
        Require(fastWork == slow.HostWorkAfter(addr, 4, true), "32-bit write at " + where);
        Require(fastLog == slowLog, "the device sees the same accesses at " + where);
    }
    for (u32 addr = 0; addr < 0x2000; addr += 4)
        Require(fastRam.Read32(addr) == slowRam.Read32(addr), "RAM differs");
}

int main() {
    AttendedAccessesMatch(false);
    AttendedAccessesMatch(true);
    for (uint32_t seed = 1; seed <= 500; ++seed) FieldsMatchMapWalk(seed);
    DeviceFindsRegisters();
    std::puts("register-test: ok");
}
