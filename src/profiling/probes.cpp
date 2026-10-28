#include "probes.h"

#include "cpu/emu.h"
#include "cpu_state.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cstddef>
#include <stdexcept>

namespace profiling {

namespace {

struct Register {
    const char* name;
    size_t offset;  // of a uint32_t in CpuState
};

#define REG(name, field) {name, offsetof(bcore::CpuState, field)}
const Register kRegisters[] = {
    REG("R0", dpregs[0]), REG("R1", dpregs[1]), REG("R2", dpregs[2]), REG("R3", dpregs[3]),
    REG("R4", dpregs[4]), REG("R5", dpregs[5]), REG("R6", dpregs[6]), REG("R7", dpregs[7]),
    REG("P0", dpregs[8]), REG("P1", dpregs[9]), REG("P2", dpregs[10]), REG("P3", dpregs[11]),
    REG("P4", dpregs[12]), REG("P5", dpregs[13]), REG("SP", dpregs[14]), REG("FP", dpregs[15]),
    REG("I0", iregs[0]), REG("I1", iregs[1]), REG("I2", iregs[2]), REG("I3", iregs[3]),
    REG("M0", mregs[0]), REG("M1", mregs[1]), REG("M2", mregs[2]), REG("M3", mregs[3]),
    REG("B0", bregs[0]), REG("B1", bregs[1]), REG("B2", bregs[2]), REG("B3", bregs[3]),
    REG("L0", lregs[0]), REG("L1", lregs[1]), REG("L2", lregs[2]), REG("L3", lregs[3]),
    REG("LT0", lt[0]), REG("LT1", lt[1]), REG("LB0", lb[0]), REG("LB1", lb[1]),
    REG("LC0", lc[0]), REG("LC1", lc[1]), REG("RETS", rets), REG("RETI", reti), REG("RETX", retx),
    REG("RETN", retn), REG("USP", usp), REG("SEQSTAT", seqstat), REG("SYSCFG", syscfg),
};
#undef REG

uint32_t ParseHex(const nlohmann::json& value, const std::string& what) {
    if (!value.is_string()) throw std::runtime_error(what + " must be a hex string");
    const auto text = value.get<std::string>();
    size_t used = 0;
    unsigned long long parsed = 0;
    try { parsed = std::stoull(text, &used, 16); } catch (const std::exception&) { used = 0; }
    if (used == 0 || used != text.size() || parsed > 0xFFFFFFFFull)
        throw std::runtime_error(what + " is not a 32-bit hex value: " + text);
    return static_cast<uint32_t>(parsed);
}

// Probe and read names become JSON keys and trace event names: the same
// rule as mark names, so nothing needs escaping and no byte (a NUL, say) is
// lost on the way out.
std::string CheckedName(const nlohmann::json& value, const std::string& what) {
    const auto name = value.get<std::string>();
    const bool ok = !name.empty() && name.size() <= 64 &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        });
    if (!ok) throw std::runtime_error("Invalid " + what + " name: \"" + name + "\" (letters, digits, - _ . only)");
    return name;
}

} // namespace

void WriteJsonString(FILE* file, const std::string& text) {
    for (unsigned char c : text) {
        if (c == '"' || c == '\\') std::fprintf(file, "\\%c", c);
        else if (c < 0x20) std::fprintf(file, "\\u%04x", c);
        else std::fputc(c, file);
    }
}

int RegisterIndex(const std::string& name) {
    for (size_t i = 0; i < std::size(kRegisters); ++i)
        if (name == kRegisters[i].name) return static_cast<int>(i);
    return -1;
}

const char* RegisterName(int index) { return kRegisters[index].name; }

uint32_t RegisterValue(const bcore::CpuState& state, int index) {
    return *reinterpret_cast<const uint32_t*>(reinterpret_cast<const uint8_t*>(&state) + kRegisters[index].offset);
}

MemoryRead ParseMemoryRead(const nlohmann::json& entry, const std::string& where) {
    MemoryRead read;
    read.name = CheckedName(entry.at("name"), where + " read");
    const std::string what = where + "." + read.name;
    if (entry.contains("base")) {
        read.baseRegister = RegisterIndex(entry.at("base").get<std::string>());
        if (read.baseRegister < 0) throw std::runtime_error(what + ": unknown base register");
        read.address = entry.contains("offset") ? ParseHex(entry.at("offset"), what + ".offset") : 0;
    } else {
        read.address = ParseHex(entry.at("addr"), what + ".addr");
    }
    read.words = entry.contains("words") ? entry.at("words").get<uint32_t>() : 1;
    if (read.words == 0 || read.words > 64) throw std::runtime_error(what + ": words must be 1 to 64");
    if (read.baseRegister < 0 && uint64_t(read.address) + 4ull * read.words > kMmrBase)
        throw std::runtime_error(what + ": reads the MMR space, where a read can change what the guest does");
    return read;
}

void Read(const MemoryRead& read, const bcore::CpuState& state, Emulator& memory, uint32_t* out, bool* valid) {
    const uint64_t base = read.baseRegister < 0 ? read.address
                                                : uint64_t(RegisterValue(state, read.baseRegister)) + read.address;
    for (uint32_t i = 0; i < read.words; ++i) {
        const uint64_t address = base + 4ull * i;
        // Devices start and end on 4-byte boundaries, so an aligned mapped
        // word lies inside one device and never past its buffer.
        valid[i] = address + 4 <= kMmrBase && (address & 3) == 0 &&
                   memory.Maps(static_cast<uint32_t>(address), 4);
        out[i] = valid[i] ? memory.MemoryRead32(static_cast<uint32_t>(address)) : 0;
    }
}

void Probes::Load(const nlohmann::json& probes) {
    size_t total = 0;
    for (const auto& entry : probes) {
        Spec spec;
        spec.name = CheckedName(entry.at("name"), "probe");
        spec.pc = ParseHex(entry.at("pc"), "probe " + spec.name + ".pc");
        size_t values = 0;
        if (entry.contains("regs")) {
            for (const auto& name : entry.at("regs")) {
                const int index = RegisterIndex(name.get<std::string>());
                if (index < 0) throw std::runtime_error("probe " + spec.name + ": unknown register " + name.dump());
                spec.registers.push_back(index);
                ++values;
            }
        }
        if (entry.contains("mem")) {
            for (const auto& read : entry.at("mem")) {
                spec.memory.push_back(ParseMemoryRead(read, "probe " + spec.name));
                values += spec.memory.back().words;
            }
        }
        std::vector<std::string> names{"pc", "runs", "packets", "cycles"};
        for (int reg : spec.registers) names.push_back(RegisterName(reg));
        for (const auto& read : spec.memory) names.push_back(read.name);
        for (size_t i = 4; i < names.size(); ++i)
            for (size_t j = 0; j < i; ++j)
                if (names[i] == names[j])
                    throw std::runtime_error("probe " + spec.name + ": captured name " + names[i] +
                                             " is used twice (pc, runs, packets and cycles are taken)");
        if (values > kMaxValues)
            throw std::runtime_error("probe " + spec.name + " captures more than " + std::to_string(kMaxValues) +
                                     " words");
        if (entry.contains("limit")) spec.limit = entry.at("limit").get<uint64_t>();
        if (spec.limit == 0 || spec.limit > (1u << 20))
            throw std::runtime_error("probe " + spec.name + ": limit must be 1 to 1048576");
        for (const auto& other : specs_)
            if (other.name == spec.name) throw std::runtime_error("duplicate probe " + spec.name);
        total += spec.limit;
        if (total > kMaxRecords)
            throw std::runtime_error("probes keep at most " + std::to_string(kMaxRecords) +
                                     " hits in all: lower their limits");
        chunks_.set((spec.pc >> 5) % chunks_.size());
        specs_.push_back(spec);
    }
    records_.resize(total);
}

void Probes::Hit(uint32_t pc, const bcore::CpuState& state, Emulator& memory, uint64_t wallNs, uint64_t runs,
                 uint64_t packets, uint64_t cycles) {
    for (uint32_t index = 0; index < specs_.size(); ++index) {
        Spec& spec = specs_[index];
        if (spec.pc != pc) continue;
        ++spec.hits;
        if (spec.hits > spec.limit || used_ == records_.size()) {
            ++spec.dropped;
            continue;
        }
        Record& record = records_[used_++];
        record = Record{};
        record.probe = index;
        record.pc = pc;
        record.wallNs = wallNs;
        record.runs = runs;
        record.packets = packets;
        record.cycles = cycles;
        size_t slot = 0;
        for (int reg : spec.registers) {
            record.values[slot] = RegisterValue(state, reg);
            record.valid |= 1u << slot++;
        }
        for (const auto& read : spec.memory) {
            bool valid[64];
            Read(read, state, memory, record.values + slot, valid);
            for (uint32_t i = 0; i < read.words; ++i, ++slot)
                if (valid[i]) record.valid |= 1u << slot;
        }
    }
}

void Probes::WriteTrace(FILE* file) const {
    for (size_t i = 0; i < used_; ++i) {
        const Record& r = records_[i];
        const Spec& spec = specs_[r.probe];
        std::fputs(",{\"name\":\"", file);
        WriteJsonString(file, spec.name);
        std::fprintf(file, "\",\"cat\":\"probe\",\"ph\":\"i\",\"s\":\"t\",\"pid\":1,\"tid\":1,\"ts\":%.3f,"
                           "\"args\":{\"pc\":%u,\"runs\":%" PRIu64 ",\"packets\":%" PRIu64 ",\"cycles\":%" PRIu64,
                     r.wallNs / 1000.0, r.pc, r.runs, r.packets, r.cycles);
        size_t slot = 0;
        auto value = [&](const char* name, size_t at) {
            std::fputs(",\"", file);
            WriteJsonString(file, name);
            if (r.valid & (1u << at)) std::fprintf(file, "\":%u", r.values[at]);
            else std::fputs("\":null", file);
        };
        for (int reg : spec.registers) value(RegisterName(reg), slot++);
        for (const auto& read : spec.memory) {
            if (read.words == 1) {
                value(read.name.c_str(), slot++);
                continue;
            }
            std::fputs(",\"", file);
            WriteJsonString(file, read.name);
            std::fputs("\":[", file);
            for (uint32_t w = 0; w < read.words; ++w, ++slot) {
                if (w) std::fputc(',', file);
                if (r.valid & (1u << slot)) std::fprintf(file, "%u", r.values[slot]);
                else std::fputs("null", file);
            }
            std::fputc(']', file);
        }
        std::fputs("}}", file);
    }
}

void Probes::WriteStats(FILE* file) const {
    std::fputc('{', file);
    for (size_t i = 0; i < specs_.size(); ++i) {
        std::fputs(i ? ",\"" : "\"", file);
        WriteJsonString(file, specs_[i].name);
        std::fprintf(file, "\":{\"hits\":%" PRIu64 ",\"dropped\":%" PRIu64 "}", specs_[i].hits, specs_[i].dropped);
    }
    std::fputc('}', file);
}

void Probes::Report(FILE* out) const {
    for (const auto& spec : specs_) {
        std::fprintf(out, "[profile] probe %s pc=0x%08x hits=%" PRIu64 " dropped=%" PRIu64 "\n", spec.name.c_str(),
                     spec.pc, spec.hits, spec.dropped);
        if (!spec.hits)
            std::fprintf(out, "[profile] probe %s never fired: the guest did not get there before the run "
                              "ended, or its PC does not start a block (probes see block entries only)\n",
                         spec.name.c_str());
    }
}

} // namespace profiling
