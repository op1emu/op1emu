#pragma once

// Guest-semantic probes for profiling builds: named block-entry PCs whose
// registers and memory are captured each time a block starts there (for
// example the RTOS context switch, to rebuild a thread timeline), and
// memory read at a mark (for example a table the firmware fills). Declared in
// the marks file, since the addresses belong to one firmware image.
//
// Reads only plain memory: an address in the MMR space (0xFFC00000 and up)
// could have side effects (a FIFO pop, a status clear) and change what the
// guest does, so it is refused in the configuration and skipped at run time.

#include <nlohmann/json.hpp>

#include <bitset>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace bcore { struct CpuState; }
class Emulator;

namespace profiling {

constexpr uint32_t kMmrBase = 0xFFC00000;

// A guest memory read: an absolute address, or a register plus an offset.
struct MemoryRead {
    std::string name;
    int baseRegister = -1;  // index into the register table; -1 = absolute
    uint32_t address = 0;   // absolute address, or the offset from the register
    uint32_t words = 1;
};

// JSON string contents, escaped (quotes, backslashes, control characters).
void WriteJsonString(FILE* file, const std::string& text);

// Parses a register name (R0-R7, P0-P5, SP, FP, RETS, ...); -1 if unknown.
int RegisterIndex(const std::string& name);
const char* RegisterName(int index);
uint32_t RegisterValue(const bcore::CpuState& state, int index);

MemoryRead ParseMemoryRead(const nlohmann::json& entry, const std::string& where);
// Reads `read`'s words into `out`. A word is read only at a 4-byte aligned
// address below the MMR space that one device maps (an unaligned word could
// run past the device's buffer); any other is reported false in `valid`.
void Read(const MemoryRead& read, const bcore::CpuState& state, Emulator& memory, uint32_t* out, bool* valid);

class Probes {
public:
    // Up to this many captured words per hit (registers plus memory).
    static constexpr size_t kMaxValues = 16;
    // Records kept in all, preallocated: about 34 MB.
    static constexpr uint64_t kMaxRecords = 262144;

    void Load(const nlohmann::json& probes);
    bool Empty() const { return specs_.empty(); }

    // Called at every block entry while probes are enabled.
    void OnBlock(uint32_t pc, const bcore::CpuState& state, Emulator& memory, uint64_t wallNs, uint64_t runs,
                 uint64_t packets, uint64_t cycles) {
        if (!chunks_.test((pc >> 5) % chunks_.size())) return;
        Hit(pc, state, memory, wallNs, runs, packets, cycles);
    }

    // Trace events (category "probe"), each prefixed with a comma.
    void WriteTrace(FILE* file) const;
    // Hit and drop counts; warns about probes that never fired.
    void Report(FILE* out) const;
    // The same counts as a JSON object for the trace header.
    void WriteStats(FILE* file) const;

private:
    struct Spec {
        std::string name;
        uint32_t pc = 0;
        std::vector<int> registers;
        std::vector<MemoryRead> memory;
        uint64_t limit = 65536;
        uint64_t hits = 0, dropped = 0;
    };
    struct Record {
        uint32_t probe = 0, pc = 0;
        uint64_t wallNs = 0, runs = 0, packets = 0, cycles = 0;
        uint32_t values[kMaxValues] = {};
        uint32_t valid = 0;  // bit per value
    };

    void Hit(uint32_t pc, const bcore::CpuState& state, Emulator& memory, uint64_t wallNs, uint64_t runs,
             uint64_t packets, uint64_t cycles);

    std::vector<Spec> specs_;
    // As for marks: one bit per 32-byte code chunk (modulo 4096) holding a
    // probe PC, so most blocks are rejected with one test.
    std::bitset<4096> chunks_;
    std::vector<Record> records_;  // preallocated: recording never allocates
    size_t used_ = 0;
};

} // namespace profiling
