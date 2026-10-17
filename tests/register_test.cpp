// Register and RegisterDevice dispatch through their flattened tables: every
// read and write must call the same field callbacks, in the same order and
// with the same values, as walking the maps did, and find the same register.
#include "cpu/io.h"
#include <cstdio>
#include <cstdlib>
#include <random>
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

int main() {
    for (uint32_t seed = 1; seed <= 500; ++seed) FieldsMatchMapWalk(seed);
    DeviceFindsRegisters();
    std::puts("register-test: ok");
}
