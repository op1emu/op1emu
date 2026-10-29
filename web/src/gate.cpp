// The tests' view of a boot (web/tools/run-node.mjs gate): the phase marks
// of a marks file fire as the profiler fires them (src/profiling/mark_set.h:
// at the start of a block, which with chaining off is every Run()), and at
// each the guest's state prints as --profile-mark-digest prints it
// (Profiler's PrintDigest), so that a run here and a native profiling run
// with the same NAND, OTP and sensors print the same lines.

#include "cpu/cpu.h"
#include "cpu/io.h"
#include "cpu_state.h"
#include "mmr.h"
#include "mark_set.h"
#include "utils/log.h"

#include <emscripten.h>
#include <nlohmann/json.hpp>

#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <typeinfo>
#include <vector>

BlackFinCpu* WebCpu();
bool WebRunOnce();

namespace {

uint64_t Hash(const void* begin, size_t bytes, uint64_t h = 0xcbf29ce484222325ull) {
    const auto* p = static_cast<const uint8_t*>(begin);
    for (size_t i = 0; i + 8 <= bytes; i += 8) {
        uint64_t word;
        std::memcpy(&word, p + i, 8);
        h = (h ^ word) * 0x100000001b3ull;
    }
    for (size_t i = bytes & ~size_t(7); i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

void PrintDigest(BlackFinCpu& cpu, const char* mark) {
    const bcore::CpuState& state = cpu.State();
    const uint32_t cec[3] = {bcore::cec_mmr_read(nullptr, bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_IPEND),
                             bcore::cec_mmr_read(nullptr, bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_ILAT),
                             bcore::cec_mmr_read(nullptr, bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_IMASK)};
    uint64_t memory = 0xcbf29ce484222325ull;
    cpu.GetEmulator().SettleWatched();
    for (Device* device : cpu.GetEmulator().Devices())
        if (typeid(*device) == typeid(MemoryDevice)) memory = Hash(device->Map(0), device->Size(), memory);
    // The system registers as the profiler sees them, in Run() after it set
    // CYCLES to the guest time (cpu.cpp's SetBfinCycles): here that is
    // before the block's Run().
    const auto* sysBegin = reinterpret_cast<const char*>(&state.rets);
    std::vector<char> sys(sysBegin, reinterpret_cast<const char*>(&state.astat_reserved + 1));
    const uint64_t cycles = cpu.Time().Cycles();
    const uint32_t cyclesRegs[3] = {uint32_t(cycles), uint32_t(cycles >> 32), uint32_t(cycles >> 32)};
    std::memcpy(sys.data() + (reinterpret_cast<const char*>(state.cycles) - sysBegin), cyclesRegs, sizeof(cyclesRegs));
    std::printf("[digest] %s regs=%016" PRIx64 " dag=%016" PRIx64 " sys=%016" PRIx64 " cec=%08x,%08x,%08x memory=%016" PRIx64
                "\n",
                mark, Hash(state.dpregs, sizeof(state.dpregs)),
                Hash(state.ax, size_t((const char*)&state.rets - (const char*)state.ax)), Hash(sys.data(), sys.size()),
                cec[0], cec[1], cec[2], memory);
}

void Fire(BlackFinCpu& cpu, const char* mark, double startMs) {
    std::printf("[mark] %s pc=0x%08x wall_ms=%.3f packets=%" PRIu64 " cycles=%" PRIu64 "\n", mark, cpu.PC(),
                emscripten_get_now() - startMs, cpu.Time().Packets(), cpu.Time().Cycles());
    PrintDigest(cpu, mark);
    std::fflush(stdout);
}

}  // namespace

extern "C" {

// Runs the started emulator (op1_start, with chaining off) until the mark
// `until` of the marks file at marksPath fires. Returns 0, or -1 if the run
// ended first or the file is bad.
EMSCRIPTEN_KEEPALIVE int op1_gate(const char* marksPath, const char* until) {
    BlackFinCpu* cpu = WebCpu();
    if (!cpu) return -1;
    profiling::MarkSet marks;
    try {
        std::ifstream file(marksPath);
        const auto json = nlohmann::json::parse(file);
        for (const auto& entry : json.at("marks")) {
            const auto name = entry.at("name").get<std::string>();
            const int after = entry.contains("after") ? marks.Find(entry.at("after").get<std::string>()) : -1;
            marks.Add(name, std::stoul(entry.at("lo").get<std::string>(), nullptr, 16),
                      std::stoul(entry.at("hi").get<std::string>(), nullptr, 16), after);
        }
        if (marks.Find(until) < 0) throw std::runtime_error(std::string("no mark ") + until);
    } catch (const std::exception& e) {
        LogError("%s: %s", marksPath, e.what());
        return -1;
    }
    const double start = emscripten_get_now();
    Fire(*cpu, "start", start);
    for (;;) {
        const int fired = marks.OnBlock(cpu->PC());
        if (fired >= 0) {
            const std::string& name = marks.Marks()[fired].name;
            Fire(*cpu, name.c_str(), start);
            if (name == until) return 0;
        }
        if (!WebRunOnce()) return -1;
    }
}

}  // extern "C"
