#include "profiler.h"

#include "cpu/cpu.h"
#include "hw_counters.h"
#include "mark_set.h"
#include "memory_census.h"
#include "probes.h"
#include "service_stats.h"
#include "sha256.h"
#include "utils/log.h"
#include "core.h"
#include "cpu_state.h"
#include "mmr.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unistd.h>
#include <thread>
#include <unordered_map>

namespace {

uint64_t MonotonicNs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// The CPU thread's own CPU time. Every caller runs on that thread, so this
// separates compile CPU from execution CPU even when the thread is
// descheduled: wall-clock execution time absorbs off-CPU time one for one, and
// on a swapping host reclaim stalls of 3-15 s were seen inside otherwise clean
// runs.
uint64_t ThreadCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

// All threads of the process: the CPU thread plus the frontend, the USB/IP
// loop and anything else, which may compete with it for the core.
uint64_t ProcessCpuNs() {
    timespec ts{};
    clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
    return uint64_t(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

uint32_t ParseAddress(const nlohmann::json& value, const std::string& what) {
    if (!value.is_string()) throw std::runtime_error(what + " must be a hex string such as \"0x019ab8b6\"");
    const auto text = value.get<std::string>();
    size_t used = 0;
    unsigned long long parsed = 0;
    try { parsed = std::stoull(text, &used, 16); } catch (const std::exception&) { used = 0; }
    if (used == 0 || used != text.size() || parsed > 0xFFFFFFFFull)
        throw std::runtime_error(what + " is not a 32-bit hex address: " + text);
    return static_cast<uint32_t>(parsed);
}

// Mark names become trace event names and file suffixes.
void CheckName(const std::string& name) {
    const bool ok = !name.empty() && name.size() <= 64 &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return std::isalnum(static_cast<unsigned char>(c)) || c == '-' || c == '_' || c == '.';
        });
    if (!ok || name == "start") throw std::runtime_error("Invalid mark name: \"" + name + "\"");
}

} // namespace

struct Profiler::Impl : BcoreEventSink {
    struct FrameMark {
        std::string name;
        int after = -1;  // index of the mark the frame must follow
        bool fired = false;
    };
    struct Counters {
        uint64_t wall_ns = 0, cpu_ns = 0, process_cpu_ns = 0, runs = 0, packets = 0, cycles = 0;
        uint64_t translated = 0, invalidations = 0, mmio_r = 0, mmio_w = 0, events = 0;
        uint64_t irq = 0, irqLevels[16] = {};
        uint64_t irqLatched = 0, irqLatchedLevels[16] = {};
        uint64_t hostInsns = 0, hostCycles = 0;  // with --profile-counters
    };
    struct Event {
        const char* name = nullptr;  // points into marks/frameMark or a literal
        uint64_t begin = 0, end = 0, cpu_ns = 0, insns = 0;
        BcoreCompileMetrics shape{};  // translate spans: the code compiled (zero on failure)
        uint32_t pc = 0;
        bool ok = true;
        bool mark = false;
        uint32_t counters = 0;  // index into markCounters for a mark
    };

    ProfilerOptions options;
    BlackFinCpu& cpu;
    std::function<void()> requestStop;

    profiling::MarkSet marks;
    FrameMark frameMark;
    bool hasFrame = false;
    std::string frameSha;
    uint64_t framesRejected = 0;
    // The verified frame completes inside a run, during device processing:
    // its mark fires at the next block entry, like every other mark, so the
    // counters and censuses it closes end on a block boundary.
    bool framePending = false;

    bool started = false;
    uint64_t startWall = 0, startCpu = 0, startProcessCpu = 0;
    HwCounters hw;
    HwCounters::Values startHw{};
    uint64_t translateInsnsBegin = 0;
    bool countersFailed = false;
    // Enabled time the counters did not count. Even a fraction of a
    // millisecond is far more than the run-to-run spread of instruction
    // counts (about 20,000 of 1.2e12), so any loss fails the run.
    static constexpr uint64_t kMaxUncountedNs = 1000;
    long cpuTid = 0;  // perf samples carry thread ids; reports keep this one
    uint64_t runs = 0, mmioReads = 0, mmioWrites = 0;
    uint64_t invalidations = 0;
    uint64_t invalidateBegin = 0, invalidateCpuBegin = 0, invalidateInsnsBegin = 0;

    // Trace: preallocated so recording never allocates on the CPU thread.
    std::vector<Event> events;
    std::vector<Counters> markCounters;  // a few per run, so kept apart from the events
    size_t used = 0;
    uint64_t lost = 0;
    uint64_t translateBegin = 0, translateCpuBegin = 0;
    BcoreCompileMetrics pendingShape{};
    uint64_t endWall = 0;

    bool census = false;
    bool stopping = false;  // --profile-until fired
    bool endFired = false;  // --profile-end-cycles
    profiling::ServiceStats* service = nullptr;  // owned by the CPU (census runs)
    profiling::MemoryCensus* memory = nullptr;   // owned by the CPU (census runs)
    // Exit census: per block, the runs after which the host was needed
    // because the block touched an MMR, because the host then did something
    // the guest can see, or both; and the block's memory accesses.
    struct ExitCount { uint64_t mmio = 0, host = 0, both = 0, accesses = 0; };
    std::unordered_map<uint32_t, ExitCount> exitCensus;
    bool exitPending = false, blockMmio = false;
    uint32_t exitPc = 0, exitIpend = 0;
    uint64_t exitHostMark = 0, blockAccessesStart = 0, exitAccesses = 0;
    struct PcCount { uint64_t runs = 0, packets = 0; };
    struct MmioCount {
        uint64_t reads = 0, writes = 0;
        // When the guest first and last touched it, and from which block.
        uint64_t firstCycles = 0, lastCycles = 0;
        uint32_t firstPc = 0;
    };
    std::unordered_map<uint32_t, PcCount> pcCensus;
    std::unordered_map<uint32_t, MmioCount> mmioCensus;
    // Block-to-block transitions: (previous block entry << 32 | this entry).
    // The previous block may have been interrupted, so an edge is the guest's
    // actual next block, interrupt entries included.
    std::unordered_map<uint64_t, uint64_t> edgeCensus;
    uint32_t lastPc = 0;
    bool haveLastPc = false;

    // Guest memory read when a mark fires, by mark name; rare, so it may allocate.
    std::unordered_map<std::string, std::vector<profiling::MemoryRead>> markReads;
    struct ReadEvent {
        std::string mark, name;
        uint64_t wallNs = 0;
        uint32_t address = 0;
        std::vector<uint32_t> values;
        std::vector<bool> valid;
    };
    std::vector<ReadEvent> readEvents;
    profiling::Probes probes;
    bool probesOn = false;
    uint32_t blockPc = 0;  // the block running now, for the MMIO census

    std::map<uint32_t, unsigned> dumpCount;
    std::vector<std::pair<std::string, std::string>> pendingDumps;
    bool dumpFailed = false;

    FILE* checksums = nullptr;
    bool checksumFailed = false;
    bool censusFailed = false;

    int perfFd = -1;
    std::string perfBegin, perfEnd;

    Impl(const ProfilerOptions& o, BlackFinCpu& c, std::function<void()> stop)
        : options(o), cpu(c), requestStop(std::move(stop)) {}

    ~Impl() override {
        if (perfFd >= 0) close(perfFd);
        if (checksums) std::fclose(checksums);
    }

    // ---- configuration ----

    int FindMark(const std::string& name) const { return marks.Find(name); }
    bool KnownMark(const std::string& name) const {
        return name == "start" || (name == "end" && options.endCycles) || FindMark(name) >= 0 ||
               (hasFrame && frameMark.name == name);
    }

    void LoadMarks() {
        std::ifstream file(options.marksPath);
        if (!file) throw std::runtime_error("Cannot read marks file: " + options.marksPath);
        nlohmann::json json;
        try { file >> json; } catch (const std::exception& e) {
            throw std::runtime_error("Invalid marks file " + options.marksPath + ": " + e.what());
        }
        // Marks are firmware addresses: on another image they silently mean
        // nothing, so every file must name the NAND image it was written for.
        if (!json.contains("nand_sha256"))
            throw std::runtime_error("Marks file " + options.marksPath +
                                     " has no nand_sha256: name the NAND image its addresses are for");
        const auto expected = json.at("nand_sha256").get<std::string>();
        if (expected.size() != 64 || expected.find_first_not_of("0123456789abcdef") != std::string::npos)
            throw std::runtime_error("Marks file " + options.marksPath +
                                     ": nand_sha256 must be 64 lowercase hex digits, got " + expected);
        const auto actual = Sha256::OfFile(options.nandPath);
        if (actual != expected)
            throw std::runtime_error("Marks file " + options.marksPath + " is for NAND sha256 " + expected +
                                     ", but " + options.nandPath + " is " + (actual.empty() ? "unreadable" : actual));
        for (const auto& entry : json.at("marks")) {
            const auto name = entry.at("name").get<std::string>();
            CheckName(name);
            if (FindMark(name) >= 0) throw std::runtime_error("Duplicate mark: " + name);
            int after = -1;
            if (entry.contains("after")) {
                after = FindMark(entry.at("after").get<std::string>());
                if (after < 0) throw std::runtime_error("Mark " + name + " is after an unknown or later mark");
            }
            marks.Add(name, ParseAddress(entry.at("lo"), name + ".lo"), ParseAddress(entry.at("hi"), name + ".hi"),
                      after);
            LoadReads(name, entry);
        }
        if (json.contains("frame")) {
            const auto& entry = json.at("frame");
            frameMark.name = entry.at("name").get<std::string>();
            CheckName(frameMark.name);
            if (FindMark(frameMark.name) >= 0) throw std::runtime_error("Duplicate mark: " + frameMark.name);
            frameMark.after = FindMark(entry.at("after").get<std::string>());
            if (frameMark.after < 0) throw std::runtime_error("Frame mark is after an unknown mark");
            frameSha = entry.at("rgb565_sha256").get<std::string>();
            hasFrame = true;
            LoadReads(frameMark.name, entry);
        }
        if (options.probes) {
            if (options.tracePath.empty())
                throw std::runtime_error("--profile-probes records into the trace: add --profile-trace");
            if (!json.contains("probes") || json.at("probes").empty())
                throw std::runtime_error("--profile-probes: " + options.marksPath + " declares no probes");
            probes.Load(json.at("probes"));
            probesOn = true;
        }
    }

    void LoadReads(const std::string& mark, const nlohmann::json& entry) {
        if (!entry.contains("read")) return;
        for (const auto& read : entry.at("read")) {
            auto parsed = profiling::ParseMemoryRead(read, "mark " + mark + " read");
            if (parsed.baseRegister >= 0)
                throw std::runtime_error("mark " + mark + " read " + parsed.name + ": a mark reads absolute addresses");
            for (const auto& other : markReads[mark])
                if (other.name == parsed.name)
                    throw std::runtime_error("mark " + mark + " reads " + parsed.name + " twice");
            markReads[mark].push_back(parsed);
        }
    }

    void Configure() {
        if (!options.dumpIrPcs.empty()) {
            std::sort(options.dumpIrPcs.begin(), options.dumpIrPcs.end());
            std::error_code error;
            std::filesystem::create_directories(options.dumpIrDir, error);
            if (error) throw std::runtime_error("Cannot create --profile-dump-dir " + options.dumpIrDir);
        }
        if (options.probes && options.marksPath.empty())
            throw std::runtime_error("--profile-probes captures the probes of a marks file: add --profile-marks");
        if (!options.marksPath.empty()) LoadMarks();
        if (!options.until.empty() && !KnownMark(options.until))
            throw std::runtime_error("--profile-until names an unknown mark: " + options.until);
        if (!options.perfWindow.empty()) {
            const auto colon = options.perfWindow.find(':');
            if (colon == std::string::npos) throw std::runtime_error("--perf-window must be BEGIN:END");
            perfBegin = options.perfWindow.substr(0, colon);
            perfEnd = options.perfWindow.substr(colon + 1);
            if (!KnownMark(perfBegin) || !KnownMark(perfEnd))
                throw std::runtime_error("--perf-window names an unknown mark: " + options.perfWindow);
            if (options.perfFifo.empty()) throw std::runtime_error("--perf-window requires --perf-ctl-fifo");
        }
        if (!options.perfFifo.empty()) {
            // O_RDWR: opening a FIFO for writing alone blocks until perf opens it.
            perfFd = open(options.perfFifo.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
            if (perfFd < 0)
                throw std::runtime_error("Cannot open perf control fifo " + options.perfFifo + ": " + std::strerror(errno));
        }
        census = !options.censusPrefix.empty();
        if (census) {
            service = &cpu.EnableServiceStats();
            memory = &cpu.EnableMemoryCensus();
        }
        if (!options.checksumPath.empty()) {
            if (options.checksumEvery == 0) throw std::runtime_error("--profile-checksum-every must be at least 1");
            checksums = std::fopen(options.checksumPath.c_str(), "wb");
            if (!checksums) throw std::runtime_error("Cannot write --profile-checksum " + options.checksumPath);
        }
        if (!options.tracePath.empty()) {
            if (options.traceEvents == 0) throw std::runtime_error("--profile-trace-events must be at least 1");
            events.resize(options.traceEvents);
        }
        markCounters.reserve(marks.Marks().size() + 2);  // start, the marks, the frame: no allocation later
        if (options.counters) {
            // Fail before the CPU starts, not with a run that counted nothing.
            // The counters themselves are opened by the CPU thread (Start).
            HwCounters probe;
            std::string error;
            if (!probe.Open(error)) throw std::runtime_error("--profile-counters: " + error);
        }
    }

    // ---- recording ----

    void Add(const Event& event) {
        if (events.empty() || endWall) return;
        if (used < events.size()) events[used++] = event;
        else ++lost;
    }

    // Reads the counters, or fails the run: a failed read must never become
    // a zero that a subtraction turns into 2^64 instructions.
    bool ReadCounters(HwCounters::Values& values) {
        if (!hw.IsOpen() || countersFailed) return false;
        if (!hw.Read(values)) {
            LogError("Hardware counter read failed: the counts are lost");
            countersFailed = true;
            return false;
        }
        if (values.enabled_ns - values.running_ns > kMaxUncountedNs) {
            LogError("Hardware counters missed %.3f ms of %.3f ms: pin the CPU thread to a performance core",
                     (values.enabled_ns - values.running_ns) / 1e6, values.enabled_ns / 1e6);
            countersFailed = true;
            return false;
        }
        return true;
    }

    void onTranslateBegin(uint32_t) override {
        // The counter read comes first here and last in Translated, so the
        // profiler's own work around a span counts as compilation.
        HwCounters::Values values;
        if (ReadCounters(values)) translateInsnsBegin = values.instructions;
        translateBegin = MonotonicNs();
        translateCpuBegin = ThreadCpuNs();
        if (memory) memory->paused = true;
    }
    void Translated(uint32_t pc, bool ok) {
        // After a failed translation too: bcore reports the failure before
        // the exception leaves Core::run.
        if (memory) memory->paused = false;
        Event event;
        event.name = "translate";
        event.begin = translateBegin;
        event.end = MonotonicNs();
        event.cpu_ns = ThreadCpuNs() - translateCpuBegin;
        event.pc = pc;
        event.ok = ok;
        event.shape = ok ? pendingShape : BcoreCompileMetrics{};
        pendingShape = {};
        HwCounters::Values values;
        if (ReadCounters(values)) event.insns = values.instructions - translateInsnsBegin;
        Add(event);
    }
    // Every translation is described: the cost is a pass over its IR and its
    // machine code, microseconds against milliseconds of compilation.
    bool wantsCompileMetrics() const override { return !events.empty(); }
    bool wantsIrDump(uint32_t pc) const override {
        return std::binary_search(options.dumpIrPcs.begin(), options.dumpIrPcs.end(), pc);
    }
    void onIrDump(uint32_t pc, const char* stage, const std::string& ir) override {
        // Kept until the next block: writing a file here would land inside
        // the translation's span. Numbered per PC, since the cache is
        // flushed at a boot ROM entry and a block can be compiled again.
        if (std::strcmp(stage, "lifted") == 0) ++dumpCount[pc];
        char name[64];
        std::snprintf(name, sizeof(name), "bb_0x%08x.%u.%s.ll", pc, dumpCount[pc], stage);
        pendingDumps.emplace_back(options.dumpIrDir + "/" + name, ir);
    }
    void WriteDumps() {
        for (const auto& [path, text] : pendingDumps) {
            std::ofstream file(path);
            file << text;
            file.close();  // a small file is still buffered: its write error shows only here
            if (file.fail()) {
                LogError("Cannot write %s", path.c_str());
                dumpFailed = true;
            }
        }
        pendingDumps.clear();
    }
    void onCompileMetrics(uint32_t, const BcoreCompileMetrics& metrics) override { pendingShape = metrics; }
    void onTranslateEnd(uint32_t pc, uint64_t) override { Translated(pc, true); }
    void onTranslateFailure(uint32_t pc, uint64_t) override { Translated(pc, false); }
    void onCompileStage(uint32_t pc, const char* stage, uint64_t begin, uint64_t end, bool ok) override {
        Event event;
        event.name = stage;  // string literals in bcore
        event.begin = begin;
        event.end = end;
        event.pc = pc;
        event.ok = ok;
        Add(event);
    }

    Counters Snapshot(uint64_t wall, uint64_t cpuNs) {
        Counters c;
        c.wall_ns = wall - startWall;
        c.cpu_ns = cpuNs - startCpu;
        c.process_cpu_ns = ProcessCpuNs() - startProcessCpu;
        c.runs = runs;
        c.packets = cpu.Time().Packets();
        c.cycles = cpu.Time().Cycles();
        c.translated = cpu.CoreStats().blocks_translated;
        c.invalidations = invalidations;
        c.mmio_r = mmioReads;
        c.mmio_w = mmioWrites;
        c.events = cpu.EventsDelivered();
        const BcoreStats stats = cpu.CoreStats();
        for (int level = 0; level < 16; ++level) {
            c.irqLevels[level] = stats.interrupts_entered[level];
            c.irq += stats.interrupts_entered[level];
            c.irqLatchedLevels[level] = cpu.IrqLatched()[level];
            c.irqLatched += cpu.IrqLatched()[level];
        }
        HwCounters::Values now;
        if (ReadCounters(now)) {
            c.hostInsns = now.instructions - startHw.instructions;
            c.hostCycles = now.cycles - startHw.cycles;
        }
        return c;
    }

    void Start(uint32_t pc) {
        started = true;
        cpuTid = static_cast<long>(gettid());
        if (service) service->cpuThread.store(std::this_thread::get_id(), std::memory_order_relaxed);
        if (options.counters) {
            std::string error;
            if (!hw.Open(error) || !hw.Read(startHw)) {
                if (error.empty()) error = "the first read failed";
                LogError("--profile-counters on the CPU thread: %s", error.c_str());
                countersFailed = true;
                requestStop();
            }
        }
        startWall = MonotonicNs();
        startCpu = ThreadCpuNs();
        startProcessCpu = ProcessCpuNs();
        // The census needs the translation brackets to leave instruction
        // fetches out of the guest's memory accesses.
        if (!events.empty() || !options.dumpIrPcs.empty() || census) cpu.SetCoreEventSink(this);
        if (options.jitdump) cpu.SetPerfJitdump(true);
        Fire("start", pc);
    }

    void BeforeInvalidate() {
        HwCounters::Values values;
        if (ReadCounters(values)) invalidateInsnsBegin = values.instructions;
        invalidateBegin = MonotonicNs();
        invalidateCpuBegin = ThreadCpuNs();
    }
    void AfterInvalidate() {
        Event event;
        event.name = "invalidate";
        event.begin = invalidateBegin;
        event.end = MonotonicNs();
        event.cpu_ns = ThreadCpuNs() - invalidateCpuBegin;
        event.pc = cpu.State().pc;
        HwCounters::Values values;
        if (ReadCounters(values)) event.insns = values.instructions - invalidateInsnsBegin;
        ++invalidations;
        Add(event);
    }

    void Fire(const char* name, uint32_t pc) {
        const uint64_t wall = MonotonicNs(), cpuNs = ThreadCpuNs();
        Event event;
        event.name = name;
        event.begin = wall;
        event.pc = pc;
        event.mark = true;
        event.counters = static_cast<uint32_t>(markCounters.size());
        markCounters.push_back(Snapshot(wall, cpuNs));
        Add(event);
        const auto& c = markCounters.back();
        // Cumulative from "start": difference any two marks. tools/op1prof parses this.
        std::printf("[mark] %s pc=0x%08x wall_ms=%.3f cpu_ms=%.3f process_cpu_ms=%.3f runs=%" PRIu64
                    " packets=%" PRIu64 " cycles=%" PRIu64 " translated=%" PRIu64 " invalidations=%" PRIu64
                    " mmio_r=%" PRIu64
                    " mmio_w=%" PRIu64 " events=%" PRIu64 " irq=%" PRIu64 " irq_latched=%" PRIu64,
                    name, pc, c.wall_ns / 1e6, c.cpu_ns / 1e6, c.process_cpu_ns / 1e6, c.runs, c.packets, c.cycles,
                    c.translated, c.invalidations, c.mmio_r, c.mmio_w, c.events, c.irq, c.irqLatched);
        if (hw.IsOpen() && !countersFailed)
            std::printf(" host_insns=%" PRIu64 " host_cycles=%" PRIu64, c.hostInsns, c.hostCycles);
        std::printf(" deterministic=%d\n", cpu.Time().Deterministic() ? 1 : 0);
        const auto& chains = cpu.Chains();
        // Returns to the host by reason (BlackFinCpu::ChainCounts), cumulative.
        std::printf("[chain] %s returns=%" PRIu64 " chained=%" PRIu64 " host_only=%" PRIu64 " service=%" PRIu64
                    " deadline=%" PRIu64 " access=%" PRIu64 " cec=%" PRIu64 " pending=%" PRIu64 " miss=%" PRIu64
                    " would=%" PRIu64 " violations=%" PRIu64,
                    name, chains.runs, chains.chained, chains.hostOnly, chains.service, chains.deadline,
                    chains.access, chains.cec, chains.pending, chains.miss, chains.would, chains.violations);
        static const char* const kWork[] = {"service", "tick", "expiry", "panel", "event", "latch", "cec", "quiet"};
        for (size_t kind = 0; kind < sizeof(kWork) / sizeof(kWork[0]); ++kind)
            if (chains.violation[kind]) std::printf(" violation_%s=%" PRIu64, kWork[kind], chains.violation[kind]);
        std::printf("\n");
        // Deadline returns by what set the budget (BlackFinCpu::DueSource), cumulative.
        std::printf("[due] %s", name);
        for (unsigned source = 0; source < chains.deadlineBy.size(); ++source)
            if (chains.deadlineBy[source])
                std::printf(" %s=%" PRIu64, BlackFinCpu::DueSourceName(source), chains.deadlineBy[source]);
        std::printf("\n");
        // IDLE waits (BlackFinCpu::Wait) and the guest cycles they let pass, cumulative.
        std::printf("[idle] %s waits=%" PRIu64 " cycles=%" PRIu64, name, cpu.IdleWaits(), cpu.IdleCycles());
        for (unsigned source = 0; source < cpu.IdleBy().size(); ++source)
            if (cpu.IdleBy()[source])
                std::printf(" %s=%" PRIu64, BlackFinCpu::DueSourceName(source), cpu.IdleBy()[source]);
        std::printf("\n");
        // What bcore made of the guest's loops (BcoreStats), cumulative.
        const BcoreStats loops = cpu.CoreStats();
        std::printf("[loops] %s loop_blocks=%" PRIu64 " proven=%" PRIu64 " speculated=%" PRIu64 " vectorized=%" PRIu64
                    " steady=%" PRIu64 " steady_iterations=%" PRIu64 " circular=%" PRIu64 "\n",
                    name, loops.loop_blocks, loops.loops_proven, loops.loops_speculated, loops.loops_vectorized,
                    loops.loops_steady, cpu.SteadyIterations(), loops.dag_circular);
        std::fflush(stdout);
        if (census) DumpCensus(name);
        if (perfBegin == name) PerfWrite("enable\n");
        if (perfEnd == name) PerfWrite("disable\n");
        ReadAtMark(name);
        if (options.markDigest) PrintDigest(name);
        if (options.until == name) {
            stopping = true;
            requestStop();
        }
    }

    void ReadAtMark(const char* mark) {
        auto found = markReads.find(mark);
        if (found == markReads.end()) return;
        for (const auto& read : found->second) {
            ReadEvent event;
            event.mark = mark;
            event.name = read.name;
            event.wallNs = MonotonicNs();
            event.address = read.address;
            std::vector<uint32_t> values(read.words);
            bool valid[64];
            profiling::Read(read, cpu.State(), cpu.GetEmulator(), values.data(), valid);
            event.values = values;
            event.valid.assign(valid, valid + read.words);
            std::printf("[read] %s %s addr=0x%08x words=", mark, read.name.c_str(), read.address);
            // A word that could not be read is null, as in the trace, never a 0.
            for (size_t i = 0; i < values.size(); ++i) {
                if (valid[i]) std::printf("%s%u", i ? "," : "", values[i]);
                else std::printf("%snull", i ? "," : "");
            }
            std::printf("\n");
            std::fflush(stdout);
            readEvents.push_back(std::move(event));
        }
    }

    void PerfWrite(const char* command) {
        if (perfFd < 0) return;
        if (write(perfFd, command, std::strlen(command)) < 0) {
            // A window that silently never opened would read as an empty profile.
            LogError("perf control fifo write failed: %s", std::strerror(errno));
            close(perfFd);
            perfFd = -1;
        }
    }

    // Cumulative counts as little-endian records (u32 key, u64, u64), sorted by key.
    template <typename Map, typename Fields>
    bool WriteCensus(const std::string& path, const Map& map, Fields fields) {
        std::vector<uint32_t> keys;
        keys.reserve(map.size());
        for (const auto& [key, value] : map) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = true;
        for (uint32_t key : keys) {
            const auto [a, b] = fields(map.at(key));
            ok = ok && std::fwrite(&key, 4, 1, file) == 1 && std::fwrite(&a, 8, 1, file) == 1 &&
                 std::fwrite(&b, 8, 1, file) == 1;
        }
        return std::fclose(file) == 0 && ok;
    }
    // When each MMR was first and last accessed: little-endian records
    // (u32 address, u32 first block PC, u64 first cycles, u64 last cycles), sorted.
    bool WriteMmioFirst(const std::string& path) {
        std::vector<uint32_t> keys;
        for (const auto& [key, value] : mmioCensus) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = true;
        for (uint32_t key : keys) {
            const auto& c = mmioCensus.at(key);
            ok = ok && std::fwrite(&key, 4, 1, file) == 1 && std::fwrite(&c.firstPc, 4, 1, file) == 1 &&
                 std::fwrite(&c.firstCycles, 8, 1, file) == 1 && std::fwrite(&c.lastCycles, 8, 1, file) == 1;
        }
        return std::fclose(file) == 0 && ok;
    }

    // What the host did after a block that the guest can see: productive
    // device calls, raises delivered and other events, new ILAT bits, GP
    // timer ticks. Any change between two readings means it did something.
    uint64_t HostWork() const {
        return service->productiveTotal + service->latchedTotal + service->timerTicks +
               (cpu.EventsDelivered() - service->raisedTotal);
    }
    static uint32_t Ipend() { return cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_IPEND); }

    void CloseExit() {
        // The window from the end of the block to the start of the next:
        // device service, events, the CEC taking an interrupt, the next
        // run's timer update.
        const bool host = HostWork() != exitHostMark || Ipend() != exitIpend;
        auto& count = exitCensus[exitPc];
        if (blockMmio && host) ++count.both;
        else if (blockMmio) ++count.mmio;
        else if (host) ++count.host;
        count.accesses += exitAccesses;
        exitPending = false;
    }

    // Cumulative exit counts as little-endian records
    // (u32 block, u32 0, u64 mmio only, u64 host only, u64 both, u64 accesses), sorted.
    bool WriteExits(const std::string& path) {
        std::vector<uint32_t> keys;
        for (const auto& [key, value] : exitCensus) keys.push_back(key);
        std::sort(keys.begin(), keys.end());
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = true;
        const uint32_t pad = 0;
        for (uint32_t key : keys) {
            const auto& c = exitCensus.at(key);
            const uint64_t fields[4] = {c.mmio, c.host, c.both, c.accesses};
            ok = ok && std::fwrite(&key, 4, 1, file) == 1 && std::fwrite(&pad, 4, 1, file) == 1 &&
                 std::fwrite(fields, sizeof(fields), 1, file) == 1;
        }
        return std::fclose(file) == 0 && ok;
    }

    // Cumulative edge counts as little-endian records (u32 from, u32 to, u64 count), sorted.
    bool WriteEdges(const std::string& path) {
        std::vector<std::pair<uint64_t, uint64_t>> edges(edgeCensus.begin(), edgeCensus.end());
        std::sort(edges.begin(), edges.end());
        FILE* file = std::fopen(path.c_str(), "wb");
        if (!file) return false;
        bool ok = true;
        for (const auto& [key, count] : edges) {
            const uint32_t from = static_cast<uint32_t>(key >> 32), to = static_cast<uint32_t>(key);
            ok = ok && std::fwrite(&from, 4, 1, file) == 1 && std::fwrite(&to, 4, 1, file) == 1 &&
                 std::fwrite(&count, 8, 1, file) == 1;
        }
        return std::fclose(file) == 0 && ok;
    }

    // One record of guest state at a block entry, little-endian:
    //   u64 runs, u64 packets, u64 cycles, u32 pc, u32 ipend, u32 ilat, u32 imask,
    //   u64 hash of R0-R7/P0-P5/SP/FP, u64 of the accumulators, DAG and loop
    //   registers, u64 of the system registers and ASTAT flags.
    // Hashed by group so a difference names its registers. Guest memory is
    // not covered: a difference there shows up in registers soon after.
    void WriteChecksum(uint32_t pc) {
        const CpuState& state = cpu.State();
        auto hash = [](const void* begin, const void* end) {
            uint64_t h = 0xcbf29ce484222325ull;
            for (auto* p = static_cast<const uint8_t*>(begin); p != end; ++p) h = (h ^ *p) * 0x100000001b3ull;
            return h;
        };
        const uint64_t head[3] = {runs, cpu.Time().Packets(), cpu.Time().Cycles()};
        const uint32_t cec[4] = {pc, cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_IPEND),
                                 cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_ILAT),
                                 cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_IMASK)};
        const uint64_t groups[3] = {hash(state.dpregs, state.dpregs + 16),
                                    hash(state.ax, &state.rets),
                                    hash(&state.rets, &state.astat_reserved + 1)};
        const bool ok = std::fwrite(head, sizeof(head), 1, checksums) == 1 &&
                        std::fwrite(cec, sizeof(cec), 1, checksums) == 1 &&
                        std::fwrite(groups, sizeof(groups), 1, checksums) == 1;
        if (!ok && !checksumFailed) {
            LogError("Failed to write --profile-checksum");
            checksumFailed = true;
        }
    }

    // The guest's registers, CEC and RAM at a mark, as hashes: two runs that
    // did the same work print the same lines, whatever the host did between
    // blocks (a checksum run cannot chain blocks; this one can).
    void PrintDigest(const char* mark) {
        const CpuState& state = cpu.State();
        auto hash = [](const void* begin, size_t bytes, uint64_t h = 0xcbf29ce484222325ull) {
            const auto* p = static_cast<const uint8_t*>(begin);
            for (size_t i = 0; i + 8 <= bytes; i += 8) {
                uint64_t word;
                std::memcpy(&word, p + i, 8);
                h = (h ^ word) * 0x100000001b3ull;
            }
            for (size_t i = bytes & ~size_t(7); i < bytes; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
            return h;
        };
        const uint32_t cec[3] = {cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_IPEND),
                                 cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_ILAT),
                                 cec_mmr_read(nullptr, CEC_MMR_BASE + 4 * CEC_IDX_IMASK)};
        uint64_t memory = 0xcbf29ce484222325ull;
        cpu.GetEmulator().SettleWatched();
        for (Device* device : cpu.GetEmulator().Devices())
            if (typeid(*device) == typeid(MemoryDevice))
                memory = hash(device->Map(0), device->Size(), memory);
        std::printf("[digest] %s regs=%016" PRIx64 " dag=%016" PRIx64 " sys=%016" PRIx64
                    " cec=%08x,%08x,%08x memory=%016" PRIx64 "\n",
                    mark, hash(state.dpregs, sizeof(state.dpregs)), hash(state.ax, size_t((const char*)&state.rets - (const char*)state.ax)),
                    hash(&state.rets, size_t((const char*)(&state.astat_reserved + 1) - (const char*)&state.rets)),
                    cec[0], cec[1], cec[2], memory);
        std::fflush(stdout);
    }

    void DumpCensus(const char* mark) {
        const std::string base = options.censusPrefix + ".";
        const bool ok =
            WriteCensus(base + "pc." + mark, pcCensus, [](const PcCount& c) { return std::pair{c.runs, c.packets}; }) &&
            WriteCensus(base + "mmio." + mark, mmioCensus, [](const MmioCount& c) { return std::pair{c.reads, c.writes}; }) &&
            WriteEdges(base + "edge." + mark) && WriteMmioFirst(base + "mmiofirst." + mark) &&
            service->Write(base + "service." + mark, cpu.IrqLatched(), cpu.CoreStats()) &&
            memory->Write(base + "memory." + mark) && WriteExits(base + "exit." + mark);
        if (!ok) {
            LogError("Failed to write census %s*.%s", base.c_str(), mark);
            censusFailed = true;
        }
    }

    // ---- per run ----

    void OnFrame(const std::vector<uint16_t>& pixels) {
        if (!hasFrame || frameMark.fired || !marks.Fired(frameMark.after)) return;
        // RGB565 as stored in memory (little-endian), the same bytes a
        // headless screenshot would be computed from before rotation.
        const auto sha = Sha256::Of(pixels.data(), pixels.size() * sizeof(uint16_t));
        if (sha != frameSha) {
            // An earlier, complete but different frame (e.g. a stale boot logo
            // transfer) is not the UI being ready. The first few are logged
            // so a new reference can be chosen from them.
            if (++framesRejected <= 8)
                LogInfo("Frame %" PRIu64 " after %s rejected: rgb565_sha256=%s", framesRejected,
                        marks.Marks()[frameMark.after].name.c_str(), sha.c_str());
            return;
        }
        frameMark.fired = true;
        framePending = true;
        LogInfo("Verified frame %s after %" PRIu64 " rejected frame(s)", frameMark.name.c_str(), framesRejected);
    }

    // ---- output ----

    bool WriteTrace() {
        if (options.tracePath.empty()) return true;
        const bool counted = hw.IsOpen() && !countersFailed;
        FILE* file = std::fopen(options.tracePath.c_str(), "w");
        if (!file) {
            LogError("Cannot write trace %s", options.tracePath.c_str());
            return false;
        }
        std::fprintf(file, "{\"schema\":\"op1.trace.v4\",\"clock\":\"CLOCK_MONOTONIC\",\"process_id\":%d,"
                           "\"cpu_thread_tid\":%ld,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"dropped\":%" PRIu64
                           ",\"capacity\":%zu,\"deterministic\":%s,\"counters\":%s,\"build\":%s,",
                     static_cast<int>(getpid()), cpuTid, startWall, endWall, lost, events.size(),
                     cpu.Time().Deterministic() ? "true" : "false", counted ? "true" : "false",
                     options.buildInfo.empty() ? "null" : options.buildInfo.c_str());
        if (probesOn) {
            std::fputs("\"probes\":", file);
            probes.WriteStats(file);
            std::fputc(',', file);
        }
        std::fputs("\"traceEvents\":[", file);
        for (size_t i = 0; i < used; ++i) {
            const auto& e = events[i];
            std::fprintf(file, "%s{\"name\":\"%s\",\"pid\":1,\"tid\":1,\"ts\":%.3f,", i ? "," : "", e.name, e.begin / 1000.0);
            if (e.mark) {
                const auto& c = markCounters[e.counters];
                std::fprintf(file, "\"cat\":\"mark\",\"ph\":\"i\",\"s\":\"g\",\"args\":{\"pc\":%u,\"wall_ns\":%" PRIu64
                                   ",\"thread_cpu_ns\":%" PRIu64 ",\"runs\":%" PRIu64 ",\"packets\":%" PRIu64
                                   ",\"cycles\":%" PRIu64 ",\"translated\":%" PRIu64 ",\"invalidations\":%" PRIu64
                                   ",\"mmio_r\":%" PRIu64
                                   ",\"mmio_w\":%" PRIu64 ",\"events\":%" PRIu64 ",\"process_cpu_ns\":%" PRIu64
                                   ",\"irq\":%" PRIu64 ",\"irq_levels\":[",
                             e.pc, c.wall_ns, c.cpu_ns, c.runs, c.packets, c.cycles, c.translated, c.invalidations,
                             c.mmio_r,
                             c.mmio_w, c.events, c.process_cpu_ns, c.irq);
                for (int level = 0; level < 16; ++level)
                    std::fprintf(file, "%s%" PRIu64, level ? "," : "", c.irqLevels[level]);
                std::fputs("]", file);
                std::fprintf(file, ",\"irq_latched\":%" PRIu64 ",\"irq_latched_levels\":[", c.irqLatched);
                for (int level = 0; level < 16; ++level)
                    std::fprintf(file, "%s%" PRIu64, level ? "," : "", c.irqLatchedLevels[level]);
                std::fputs("]", file);
                if (counted)
                    std::fprintf(file, ",\"host_insns\":%" PRIu64 ",\"host_cycles\":%" PRIu64, c.hostInsns,
                                 c.hostCycles);
                std::fputs("}}", file);
            } else {
                const bool translate = std::strcmp(e.name, "translate") == 0;
                const bool invalidate = std::strcmp(e.name, "invalidate") == 0;
                std::fprintf(file, "\"cat\":\"jit\",\"ph\":\"X\",\"dur\":%.3f,\"args\":{\"pc\":%u,\"ok\":%s",
                             (e.end - e.begin) / 1000.0, e.pc, e.ok ? "true" : "false");
                if (translate) {
                    // The hash as a string: JSON numbers lose 64-bit integers.
                    std::fprintf(file, ",\"cpu_ns\":%" PRIu64 ",\"ir_insns\":%" PRIu64 ",\"ir_blocks\":%" PRIu64
                                       ",\"object_bytes\":%" PRIu64 ",\"object_hash\":\"%016" PRIx64 "\",\"collect_ns\":%" PRIu64,
                                 e.cpu_ns, e.shape.ir_instructions, e.shape.ir_blocks, e.shape.object_bytes,
                                 e.shape.object_hash, e.shape.collect_ns);
                    // A static string from bcore: what ends the block.
                    if (e.ok && e.shape.terminator) std::fprintf(file, ",\"terminator\":\"%s\"", e.shape.terminator);
                }
                if (invalidate) std::fprintf(file, ",\"cpu_ns\":%" PRIu64, e.cpu_ns);
                if ((translate || invalidate) && counted) std::fprintf(file, ",\"insns\":%" PRIu64, e.insns);
                std::fputs("}}", file);
            }
        }
        probes.WriteTrace(file);
        for (const auto& r : readEvents) {
            std::fputs(",{\"name\":\"", file);
            profiling::WriteJsonString(file, r.name);
            std::fprintf(file, "\",\"cat\":\"read\",\"ph\":\"i\",\"s\":\"g\",\"pid\":1,\"tid\":1,\"ts\":%.3f,"
                               "\"args\":{\"mark\":\"", r.wallNs / 1000.0);
            profiling::WriteJsonString(file, r.mark);
            std::fprintf(file, "\",\"addr\":%u,\"words\":[", r.address);
            for (size_t i = 0; i < r.values.size(); ++i) {
                if (i) std::fputc(',', file);
                if (r.valid[i]) std::fprintf(file, "%u", r.values[i]);
                else std::fputs("null", file);
            }
            std::fputs("]}}", file);
        }
        std::fputs("]}\n", file);
        const bool ok = std::fclose(file) == 0;
        std::fprintf(stderr, "[profile] trace records=%zu dropped=%" PRIu64 " write=%s path=%s\n", used, lost,
                     ok ? "ok" : "failed", options.tracePath.c_str());
        return ok && lost == 0;
    }
};

Profiler::Profiler(const ProfilerOptions& options, BlackFinCpu& cpu, std::function<void()> requestStop)
    : impl_(std::make_unique<Impl>(options, cpu, std::move(requestStop))) {
    impl_->Configure();
    // Which binary this is, on the same stream as the marks.
    if (!options.buildInfo.empty()) std::printf("[build] %s\n", options.buildInfo.c_str());
}

Profiler::~Profiler() = default;

bool Profiler::ChainingAllowed(uint32_t pc) const {
    const auto& impl = *impl_;
    return !impl.census && !impl.probesOn && !impl.checksums && !impl.stopping && !HostOnly(pc);
}

uint64_t Profiler::EndCycles() const { return impl_->endFired ? 0 : impl_->options.endCycles; }

bool Profiler::HostOnly(uint32_t pc) const { return impl_->marks.InUnfired(pc); }

void Profiler::BeforeRun(uint32_t pc) {
    auto& impl = *impl_;
    if (!impl.started) impl.Start(pc);
    // The transition into this block belongs with the block it leaves, like
    // that block's run in the PC census: count it before a mark here dumps.
    impl.blockPc = pc;
    if (!impl.pendingDumps.empty()) impl.WriteDumps();
    if (impl.probesOn)
        impl.probes.OnBlock(pc, impl.cpu.State(), impl.cpu.GetEmulator(), MonotonicNs(), impl.runs,
                            impl.cpu.Time().Packets(), impl.cpu.Time().Cycles());
    if (impl.census) {
        if (impl.haveLastPc) ++impl.edgeCensus[uint64_t(impl.lastPc) << 32 | pc];
        impl.lastPc = pc;
        impl.haveLastPc = true;
        // Like the edge, the exit of the previous block belongs with it.
        if (impl.exitPending) impl.CloseExit();
        impl.blockMmio = false;
        impl.blockAccessesStart = impl.memory->accesses;
    }
    if (impl.framePending) {
        impl.framePending = false;
        impl.Fire(impl.frameMark.name.c_str(), pc);
    }
    if (impl.options.endCycles && !impl.endFired && impl.cpu.Time().Cycles() >= impl.options.endCycles) {
        impl.endFired = true;
        impl.Fire("end", pc);
    }
    if (impl.marks.Left()) {
        const int fired = impl.marks.OnBlock(pc);
        if (fired >= 0) impl.Fire(impl.marks.Marks()[fired].name.c_str(), pc);
    }
    if (impl.checksums && impl.runs >= impl.options.checksumFrom &&
        (impl.runs - impl.options.checksumFrom) % impl.options.checksumEvery == 0)
        impl.WriteChecksum(pc);
}

void Profiler::BeforeInvalidate() { impl_->BeforeInvalidate(); }

void Profiler::AfterInvalidate() { impl_->AfterInvalidate(); }

void Profiler::AfterRun(uint32_t pc, uint32_t packets, uint32_t chained) {
    auto& impl = *impl_;
    // Runs count blocks: those a run chained too (never in census runs, so
    // the PC census below stays per block).
    impl.runs += 1 + chained;
    if (impl.census) {
        auto& count = impl.pcCensus[pc];
        ++count.runs;
        count.packets += packets;
        impl.exitPending = true;
        impl.exitPc = pc;
        impl.exitAccesses = impl.memory->accesses - impl.blockAccessesStart;
        impl.exitHostMark = impl.HostWork();
        impl.exitIpend = impl.Ipend();
    }
}

void Profiler::OnMmio(uint32_t addr, bool write) {
    auto& impl = *impl_;
    if (write) ++impl.mmioWrites;
    else ++impl.mmioReads;
    if (impl.census) {
        impl.blockMmio = true;
        auto& count = impl.mmioCensus[addr];
        // Guest time: in deterministic mode that of the accessing packet.
        const uint64_t cycles = impl.cpu.Time().Cycles();
        if (count.reads + count.writes == 0) {
            count.firstCycles = cycles;
            count.firstPc = impl.blockPc;
        }
        count.lastCycles = cycles;
        if (write) ++count.writes;
        else ++count.reads;
    }
}

bool Profiler::WantsFrames() const { return impl_->hasFrame; }

void Profiler::OnFrame(const std::vector<uint16_t>& pixels) { impl_->OnFrame(pixels); }

bool Profiler::Finish() {
    auto& impl = *impl_;
    impl.endWall = MonotonicNs();
    impl.cpu.SetCoreEventSink(nullptr);
    // A thread that ran on an efficiency core (or shared the counters with
    // another user) counted only part of the time: partial instruction
    // counts would read as a speedup. The last check covers the tail.
    HwCounters::Values end;
    if (impl.hw.IsOpen() && impl.ReadCounters(end))
        std::fprintf(stderr, "[profile] counters missed %" PRIu64 " ns of %" PRIu64 " ns\n",
                     end.enabled_ns - end.running_ns, end.enabled_ns);
    if (impl.countersFailed) std::fprintf(stderr, "[profile] hardware counts are incomplete; do not use this run\n");
    if (impl.checksums && std::fflush(impl.checksums) != 0) impl.checksumFailed = true;
    impl.WriteDumps();
    bool ok = impl.WriteTrace() && !impl.censusFailed && !impl.countersFailed && !impl.checksumFailed &&
              !impl.dumpFailed;
    for (const auto& mark : impl.marks.Marks())
        if (!mark.fired) std::fprintf(stderr, "[profile] mark %s did not fire\n", mark.name.c_str());
    if (impl.probesOn) impl.probes.Report(stderr);
    if (impl.framePending)
        std::fprintf(stderr, "[profile] frame %s was verified but no block ran after it to fire its mark\n",
                     impl.frameMark.name.c_str());
    if (impl.hasFrame && !impl.frameMark.fired)
        std::fprintf(stderr, "[profile] frame mark %s did not fire (%" PRIu64 " complete frame(s) rejected)\n",
                     impl.frameMark.name.c_str(), impl.framesRejected);
    return ok;
}

void FrameTap::Initialize(int width, int height) {
    if (width != width_ || height != height_) {
        width_ = std::max(width, 0);
        height_ = std::max(height, 0);
        work_.assign(size_t(width_) * height_, 0);
        seen_.assign(work_.size(), 0);
        remaining_ = work_.size();
        started_ = false;
    }
    inner_->Initialize(width, height);
}

void FrameTap::UpdateRowBuffer(int x, int y, const void* data, int length) {
    inner_->UpdateRowBuffer(x, y, data, length);
    // Same frame rule as the headless frontend: a frame starts at (0,0) and is
    // complete once every pixel has been written since.
    if (!data || length <= 0 || length % 2 || x < 0 || y < 0 || y >= height_ || x >= width_ ||
        length / 2 > width_ - x)
        return;
    if (x == 0 && y == 0) {
        started_ = true;
        remaining_ = work_.size();
        std::fill(seen_.begin(), seen_.end(), 0);
    }
    if (!started_) return;
    const size_t begin = size_t(y) * width_ + x, count = length / 2;
    std::memcpy(work_.data() + begin, data, length);
    for (size_t i = begin; i < begin + count; ++i)
        if (!seen_[i]) { seen_[i] = 1; --remaining_; }
    if (remaining_ == 0) {
        started_ = false;
        profiler_.OnFrame(work_);
    }
}
