#include "profiler.h"

#include "cpu/cpu.h"
#include "sha256.h"
#include "utils/log.h"
#include "core.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <bitset>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <stdexcept>
#include <unistd.h>
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
    struct Mark {
        std::string name;
        uint32_t lo = 0, hi = 0;
        int after = -1;        // index of the mark that must fire first
        bool inside = false;   // entry-edge state, tracked from the first run
        bool fired = false;
    };
    struct Counters {
        uint64_t wall_ns = 0, cpu_ns = 0, runs = 0, packets = 0, cycles = 0;
        uint64_t translated = 0, mmio_r = 0, mmio_w = 0, events = 0;
    };
    struct Event {
        const char* name = nullptr;  // points into marks/frameMark or a literal
        uint64_t begin = 0, end = 0, cpu_ns = 0;
        uint32_t pc = 0;
        bool ok = true;
        bool mark = false;
        Counters counters{};
    };

    ProfilerOptions options;
    BlackFinCpu& cpu;
    std::function<void()> requestStop;

    std::vector<Mark> marks;
    size_t marksLeft = 0;
    // Every block passes through CheckMarks while a mark is unfired, so most
    // must be rejected cheaply: one bit per 32-byte code chunk (modulo 4096)
    // covered by an unfired mark. A clear bit proves the PC is in no unfired
    // range; a set bit only means "check properly". Scanning the ranges for
    // every block cost 2.8% of execution samples on a stock boot.
    std::bitset<4096> markChunks;
    size_t marksInside = 0;  // unfired marks whose range held the previous PC
    Mark frameMark;            // lo/hi unused
    bool hasFrame = false;
    std::string frameSha;
    uint64_t framesRejected = 0;

    bool started = false;
    uint64_t startWall = 0, startCpu = 0;
    long cpuTid = 0;  // perf samples carry thread ids; reports keep this one
    uint64_t runs = 0, mmioReads = 0, mmioWrites = 0;

    // Trace: preallocated so recording never allocates on the CPU thread.
    std::vector<Event> events;
    size_t used = 0;
    uint64_t lost = 0;
    uint64_t translateBegin = 0, translateCpuBegin = 0;
    uint64_t endWall = 0;

    bool census = false;
    struct PcCount { uint64_t runs = 0, packets = 0; };
    struct MmioCount { uint64_t reads = 0, writes = 0; };
    std::unordered_map<uint32_t, PcCount> pcCensus;
    std::unordered_map<uint32_t, MmioCount> mmioCensus;
    bool censusFailed = false;

    int perfFd = -1;
    std::string perfBegin, perfEnd;

    Impl(const ProfilerOptions& o, BlackFinCpu& c, std::function<void()> stop)
        : options(o), cpu(c), requestStop(std::move(stop)) {}

    ~Impl() override {
        if (perfFd >= 0) close(perfFd);
    }

    // ---- configuration ----

    int FindMark(const std::string& name) const {
        for (size_t i = 0; i < marks.size(); ++i)
            if (marks[i].name == name) return static_cast<int>(i);
        return -1;
    }
    bool KnownMark(const std::string& name) const {
        return name == "start" || FindMark(name) >= 0 || (hasFrame && frameMark.name == name);
    }

    void LoadMarks() {
        std::ifstream file(options.marksPath);
        if (!file) throw std::runtime_error("Cannot read marks file: " + options.marksPath);
        nlohmann::json json;
        try { file >> json; } catch (const std::exception& e) {
            throw std::runtime_error("Invalid marks file " + options.marksPath + ": " + e.what());
        }
        // Marks are firmware addresses: on another image they silently mean
        // nothing, so the file names the NAND image it was written for.
        if (json.contains("nand_sha256")) {
            const auto expected = json.at("nand_sha256").get<std::string>();
            const auto actual = Sha256::OfFile(options.nandPath);
            if (actual != expected)
                throw std::runtime_error("Marks file " + options.marksPath + " is for NAND sha256 " + expected +
                                         ", but " + options.nandPath + " is " + (actual.empty() ? "unreadable" : actual));
        }
        for (const auto& entry : json.at("marks")) {
            Mark mark;
            mark.name = entry.at("name").get<std::string>();
            CheckName(mark.name);
            if (FindMark(mark.name) >= 0) throw std::runtime_error("Duplicate mark: " + mark.name);
            mark.lo = ParseAddress(entry.at("lo"), mark.name + ".lo");
            mark.hi = ParseAddress(entry.at("hi"), mark.name + ".hi");
            if (mark.lo > mark.hi) throw std::runtime_error("Mark " + mark.name + " has lo > hi");
            if (entry.contains("after")) {
                mark.after = FindMark(entry.at("after").get<std::string>());
                if (mark.after < 0) throw std::runtime_error("Mark " + mark.name + " is after an unknown or later mark");
            }
            marks.push_back(mark);
        }
        marksLeft = marks.size();
        RebuildMarkChunks();
        if (json.contains("frame")) {
            const auto& entry = json.at("frame");
            frameMark.name = entry.at("name").get<std::string>();
            CheckName(frameMark.name);
            if (FindMark(frameMark.name) >= 0) throw std::runtime_error("Duplicate mark: " + frameMark.name);
            frameMark.after = FindMark(entry.at("after").get<std::string>());
            if (frameMark.after < 0) throw std::runtime_error("Frame mark is after an unknown mark");
            frameSha = entry.at("rgb565_sha256").get<std::string>();
            hasFrame = true;
        }
    }

    void Configure() {
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
        if (!options.tracePath.empty()) events.resize(262144);
    }

    // ---- recording ----

    void Add(const Event& event) {
        if (events.empty() || endWall) return;
        if (used < events.size()) events[used++] = event;
        else ++lost;
    }

    void onTranslateBegin(uint32_t) override {
        translateBegin = MonotonicNs();
        translateCpuBegin = ThreadCpuNs();
    }
    void Translated(uint32_t pc, bool ok) {
        Event event;
        event.name = "translate";
        event.begin = translateBegin;
        event.end = MonotonicNs();
        event.cpu_ns = ThreadCpuNs() - translateCpuBegin;
        event.pc = pc;
        event.ok = ok;
        Add(event);
    }
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

    Counters Snapshot(uint64_t wall, uint64_t cpuNs) const {
        Counters c;
        c.wall_ns = wall - startWall;
        c.cpu_ns = cpuNs - startCpu;
        c.runs = runs;
        c.packets = cpu.Time().Packets();
        c.cycles = cpu.Time().Cycles();
        c.translated = cpu.CoreStats().blocks_translated;
        c.mmio_r = mmioReads;
        c.mmio_w = mmioWrites;
        c.events = cpu.EventsDelivered();
        return c;
    }

    void Start(uint32_t pc) {
        started = true;
        cpuTid = static_cast<long>(gettid());
        startWall = MonotonicNs();
        startCpu = ThreadCpuNs();
        if (!events.empty()) cpu.SetCoreEventSink(this);
        if (options.jitdump) cpu.SetPerfJitdump(true);
        Fire("start", pc);
    }

    void Fire(const char* name, uint32_t pc) {
        const uint64_t wall = MonotonicNs(), cpuNs = ThreadCpuNs();
        Event event;
        event.name = name;
        event.begin = wall;
        event.pc = pc;
        event.mark = true;
        event.counters = Snapshot(wall, cpuNs);
        Add(event);
        const auto& c = event.counters;
        // Cumulative from "start": difference any two marks. tools/op1prof parses this.
        std::printf("[mark] %s pc=0x%08x wall_ms=%.3f cpu_ms=%.3f runs=%" PRIu64 " packets=%" PRIu64
                    " cycles=%" PRIu64 " translated=%" PRIu64 " mmio_r=%" PRIu64 " mmio_w=%" PRIu64
                    " events=%" PRIu64 " deterministic=%d\n",
                    name, pc, c.wall_ns / 1e6, c.cpu_ns / 1e6, c.runs, c.packets, c.cycles, c.translated,
                    c.mmio_r, c.mmio_w, c.events, cpu.Time().Deterministic() ? 1 : 0);
        std::fflush(stdout);
        if (census) DumpCensus(name);
        if (perfBegin == name) PerfWrite("enable\n");
        if (perfEnd == name) PerfWrite("disable\n");
        if (options.until == name) requestStop();
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
    void DumpCensus(const char* mark) {
        const std::string base = options.censusPrefix + ".";
        const bool ok =
            WriteCensus(base + "pc." + mark, pcCensus, [](const PcCount& c) { return std::pair{c.runs, c.packets}; }) &&
            WriteCensus(base + "mmio." + mark, mmioCensus, [](const MmioCount& c) { return std::pair{c.reads, c.writes}; });
        if (!ok) {
            LogError("Failed to write census %s*.%s", base.c_str(), mark);
            censusFailed = true;
        }
    }

    // ---- per run ----

    void RebuildMarkChunks() {
        markChunks.reset();
        for (const auto& mark : marks) {
            if (mark.fired) continue;
            if ((mark.hi >> 5) - (mark.lo >> 5) >= markChunks.size()) { markChunks.set(); return; }
            for (uint32_t chunk = mark.lo >> 5; chunk <= mark.hi >> 5; ++chunk) markChunks.set(chunk % markChunks.size());
        }
    }

    void CheckMarks(uint32_t pc) {
        if (!markChunks.test((pc >> 5) % markChunks.size())) {
            // Outside every unfired range: only the entry-edge state can change.
            if (marksInside) {
                for (auto& mark : marks) mark.inside = false;
                marksInside = 0;
            }
            return;
        }
        // Every unfired mark tracks entry edges from the first run, so a mark
        // armed by its `after` mark still needs a fresh entry: "te-boot" shares
        // the bootrom range with "bootrom" and must not fire on the very next
        // block. At most one mark fires per run, in file order.
        int fire = -1;
        marksInside = 0;
        for (size_t i = 0; i < marks.size(); ++i) {
            auto& mark = marks[i];
            if (mark.fired) continue;
            const bool in = pc >= mark.lo && pc <= mark.hi;
            const bool edge = in && !mark.inside;
            mark.inside = in;
            marksInside += in;
            if (edge && fire < 0 && (mark.after < 0 || marks[mark.after].fired)) fire = static_cast<int>(i);
        }
        if (fire < 0) return;
        marks[fire].fired = true;
        --marksLeft;
        RebuildMarkChunks();
        Fire(marks[fire].name.c_str(), pc);
    }

    void OnFrame(const std::vector<uint16_t>& pixels) {
        if (!hasFrame || frameMark.fired || !marks[frameMark.after].fired) return;
        // RGB565 as stored in memory (little-endian), the same bytes a
        // headless screenshot would be computed from before rotation.
        const auto sha = Sha256::Of(pixels.data(), pixels.size() * sizeof(uint16_t));
        if (sha != frameSha) {
            // An earlier, complete but different frame (e.g. a stale boot logo
            // transfer) is not the UI being ready. The first few are logged
            // so a new reference can be chosen from them.
            if (++framesRejected <= 8)
                LogInfo("Frame %" PRIu64 " after %s rejected: rgb565_sha256=%s", framesRejected,
                        marks[frameMark.after].name.c_str(), sha.c_str());
            return;
        }
        frameMark.fired = true;
        LogInfo("Verified frame %s after %" PRIu64 " rejected frame(s)", frameMark.name.c_str(), framesRejected);
        Fire(frameMark.name.c_str(), cpu.PC());
    }

    // ---- output ----

    bool WriteTrace() {
        if (options.tracePath.empty()) return true;
        FILE* file = std::fopen(options.tracePath.c_str(), "w");
        if (!file) {
            LogError("Cannot write trace %s", options.tracePath.c_str());
            return false;
        }
        std::fprintf(file, "{\"schema\":\"op1.trace.v2\",\"clock\":\"CLOCK_MONOTONIC\",\"process_id\":%d,"
                           "\"cpu_thread_tid\":%ld,\"start_ns\":%" PRIu64 ",\"end_ns\":%" PRIu64 ",\"dropped\":%" PRIu64
                           ",\"deterministic\":%s,\"traceEvents\":[",
                     static_cast<int>(getpid()), cpuTid, startWall, endWall, lost,
                     cpu.Time().Deterministic() ? "true" : "false");
        for (size_t i = 0; i < used; ++i) {
            const auto& e = events[i];
            std::fprintf(file, "%s{\"name\":\"%s\",\"pid\":1,\"tid\":1,\"ts\":%.3f,", i ? "," : "", e.name, e.begin / 1000.0);
            if (e.mark) {
                const auto& c = e.counters;
                std::fprintf(file, "\"cat\":\"mark\",\"ph\":\"i\",\"s\":\"g\",\"args\":{\"pc\":%u,\"wall_ns\":%" PRIu64
                                   ",\"thread_cpu_ns\":%" PRIu64 ",\"runs\":%" PRIu64 ",\"packets\":%" PRIu64
                                   ",\"cycles\":%" PRIu64 ",\"translated\":%" PRIu64 ",\"mmio_r\":%" PRIu64
                                   ",\"mmio_w\":%" PRIu64 ",\"events\":%" PRIu64 "}}",
                             e.pc, c.wall_ns, c.cpu_ns, c.runs, c.packets, c.cycles, c.translated, c.mmio_r,
                             c.mmio_w, c.events);
            } else {
                const bool translate = std::strcmp(e.name, "translate") == 0;
                std::fprintf(file, "\"cat\":\"jit\",\"ph\":\"X\",\"dur\":%.3f,\"args\":{\"pc\":%u,\"ok\":%s",
                             (e.end - e.begin) / 1000.0, e.pc, e.ok ? "true" : "false");
                if (translate) std::fprintf(file, ",\"cpu_ns\":%" PRIu64, e.cpu_ns);
                std::fputs("}}", file);
            }
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
}

Profiler::~Profiler() = default;

void Profiler::BeforeRun(uint32_t pc) {
    auto& impl = *impl_;
    if (!impl.started) impl.Start(pc);
    if (impl.marksLeft) impl.CheckMarks(pc);
}

void Profiler::AfterRun(uint32_t pc, uint32_t packets) {
    auto& impl = *impl_;
    ++impl.runs;
    if (impl.census) {
        auto& count = impl.pcCensus[pc];
        ++count.runs;
        count.packets += packets;
    }
}

void Profiler::OnMmio(uint32_t addr, bool write) {
    auto& impl = *impl_;
    if (write) ++impl.mmioWrites;
    else ++impl.mmioReads;
    if (impl.census) {
        auto& count = impl.mmioCensus[addr];
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
    bool ok = impl.WriteTrace() && !impl.censusFailed;
    for (const auto& mark : impl.marks)
        if (!mark.fired) std::fprintf(stderr, "[profile] mark %s did not fire\n", mark.name.c_str());
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
