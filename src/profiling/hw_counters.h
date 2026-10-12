#pragma once

// User-space instructions and cycles of the calling thread, from the CPU's
// performance counters (perf_event_open). Instructions retired are nearly
// the same from run to run, unlike CPU time, so they can show an effect far
// smaller than the run-to-run spread of time. They are host work, not time.

#include <cstdint>
#include <string>

class HwCounters {
public:
    struct Values {
        uint64_t instructions = 0, cycles = 0;
        uint64_t enabled_ns = 0, running_ns = 0;  // running < enabled: counts are partial
    };

    HwCounters() = default;
    ~HwCounters();
    HwCounters(const HwCounters&) = delete;
    HwCounters& operator=(const HwCounters&) = delete;

    // Opens the counters for the calling thread. Returns false with a message
    // for the user (permissions, no PMU) instead of counting nothing.
    bool Open(std::string& error);
    bool IsOpen() const { return leader_ >= 0; }
    // False if the read failed: the group lost its counters (it is pinned,
    // so it errors instead of being multiplexed) or was never open.
    bool Read(Values& values) const;

private:
    int leader_ = -1, cycles_ = -1;
};
