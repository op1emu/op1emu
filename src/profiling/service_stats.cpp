#include "service_stats.h"

#include "bcore_profile.h"

#include <cinttypes>
#include <cstdio>

namespace profiling {

void ServiceStats::SetDevices(const std::vector<DeviceInfo>& devices) {
    sources.clear();
    for (const auto& device : devices) {
        Source source;
        source.name = device.name;
        source.type = device.type;
        source.bases = device.bases;
        sources.push_back(source);
    }
    for (const char* name : {"block", "events", "other"}) {
        Source source;
        source.name = name;
        sources.push_back(source);
    }
    block = static_cast<int>(sources.size()) - 3;
    events = block + 1;
    other = block + 2;
    current = other;
}

void ServiceStats::Published(int pin, int level) {
    if (!OnCpuThread()) {
        // The pin's last level is the CPU thread's: a transition another
        // thread made shows up at the CPU thread's next publication.
        publishedOtherThreads.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (pin < 0 || pin >= static_cast<int>(pins.size())) return;
    auto& p = pins[pin];
    auto& source = sources[current];
    ++p.published;
    ++source.published;
    if (p.firstSource < 0) p.firstSource = current;
    const int value = level ? 1 : 0;
    if (value != p.level) {
        p.level = value;
        ++p.transitions;
        ++source.transitions;
    }
}

void ServiceStats::Forwarded(int ivg) {
    if (!OnCpuThread()) {
        forwardedOtherThreads.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ivg >= 0 && ivg < static_cast<int>(forwarded.size())) ++forwarded[ivg];
}

void ServiceStats::Queued() {
    if (!OnCpuThread()) {
        queuedOtherThreads.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    auto& source = sources[current];
    ++source.queued;
}

static void WriteString(FILE* file, const std::string& text) {
    std::fputc('"', file);
    for (char c : text) {
        if (c == '"' || c == '\\') std::fputc('\\', file);
        if (static_cast<unsigned char>(c) >= 0x20) std::fputc(c, file);
    }
    std::fputc('"', file);
}

bool ServiceStats::Write(const std::string& path, const std::array<uint64_t, 16>& latched,
                         const BcoreStats& core) const {
    FILE* file = std::fopen(path.c_str(), "w");
    if (!file) return false;
    std::fputs("{\"schema\":\"op1.service.v1\",\"sources\":[", file);
    for (size_t i = 0; i < sources.size(); ++i) {
        const auto& s = sources[i];
        std::fputs(i ? ",{\"name\":" : "{\"name\":", file);
        WriteString(file, s.name);
        std::fputs(",\"type\":", file);
        WriteString(file, s.type);
        std::fputs(",\"bases\":[", file);
        for (size_t b = 0; b < s.bases.size(); ++b) {
            if (b) std::fputc(',', file);
            WriteString(file, s.bases[b]);
        }
        std::fputc(']', file);
        std::fprintf(file, ",\"device\":%s,\"calls\":%" PRIu64 ",\"productive\":%" PRIu64 ",\"published\":%" PRIu64
                           ",\"transitions\":%" PRIu64 ",\"queued\":%" PRIu64 ",\"raises\":%" PRIu64
                           ",\"work\":%" PRIu64 "}",
                     static_cast<int>(i) < block ? "true" : "false", s.calls, s.productive, s.published,
                     s.transitions, s.queued, s.raises, s.work);
    }
    std::fputs("],\"pins\":[", file);
    bool first = true;
    for (size_t pin = 0; pin < pins.size(); ++pin) {
        const auto& p = pins[pin];
        if (!p.published) continue;
        std::fprintf(file, "%s{\"pin\":%zu,\"published\":%" PRIu64 ",\"transitions\":%" PRIu64 ",\"source\":",
                     first ? "" : ",", pin, p.published, p.transitions);
        WriteString(file, p.firstSource >= 0 ? sources[p.firstSource].name : std::string());
        std::fputc('}', file);
        first = false;
    }
    std::fputs("],\"ivg\":[", file);
    for (int level = 0; level < 16; ++level)
        std::fprintf(file, "%s{\"level\":%d,\"forwarded\":%" PRIu64 ",\"raised\":%" PRIu64 ",\"latched\":%" PRIu64
                           ",\"entered\":%" PRIu64 "}",
                     level ? "," : "", level, forwarded[level], raised[level], latched[level],
                     core.interrupts_entered[level]);
    std::fprintf(file, "],\"queue\":{\"process_calls\":%" PRIu64 ",\"nonempty_calls\":%" PRIu64
                       ",\"length_sum\":%" PRIu64 ",\"length_max\":%" PRIu64 ",\"queued_other_threads\":%" PRIu64
                       "},\"other_threads\":{\"published\":%" PRIu64 ",\"forwarded\":%" PRIu64
                       "},\"timer_ticks_dropped\":%" PRIu64 ",\"bcore\":{\"cache_hits\":%" PRIu64
                       ",\"cache_misses\":%" PRIu64 ",\"blocks_executed\":%" PRIu64 "}}\n",
                 processCalls, nonEmptyCalls, lengthSum, lengthMax,
                 queuedOtherThreads.load(std::memory_order_relaxed),
                 publishedOtherThreads.load(std::memory_order_relaxed),
                 forwardedOtherThreads.load(std::memory_order_relaxed), timerTicksDropped, core.cache_hits,
                 core.cache_misses, core.blocks_executed);
    return std::fclose(file) == 0;
}

}  // namespace profiling
