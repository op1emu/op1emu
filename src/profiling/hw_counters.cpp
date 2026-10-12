#include "hw_counters.h"

#include <cerrno>
#include <cstring>
#include <fstream>
#include <linux/perf_event.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <unistd.h>

namespace {

// On a hybrid CPU a generic hardware event needs the PMU it counts on in the
// upper config bits; the CPU thread is meant to be pinned to a performance
// core, so it is the cpu_core PMU. Zero elsewhere means the only PMU.
uint64_t CorePmuType() {
    std::ifstream file("/sys/devices/cpu_core/type");
    uint64_t type = 0;
    return file >> type ? type : 0;
}

int OpenEvent(uint64_t config, int group) {
    perf_event_attr attr{};
    attr.size = sizeof(attr);
    attr.type = PERF_TYPE_HARDWARE;
    attr.config = config | (CorePmuType() << 32);  // PERF_PMU_TYPE_SHIFT
    attr.disabled = group < 0;  // the leader starts the group
    // Pinned: if the group cannot count (another user took the counters, or
    // the thread runs where this PMU does not count) it goes into an error
    // state that Read reports, instead of being multiplexed and scaled.
    attr.pinned = group < 0;
    attr.exclude_kernel = 1;
    attr.exclude_hv = 1;
    attr.read_format = PERF_FORMAT_GROUP | PERF_FORMAT_TOTAL_TIME_ENABLED | PERF_FORMAT_TOTAL_TIME_RUNNING;
    return static_cast<int>(syscall(SYS_perf_event_open, &attr, 0 /* this thread */, -1, group, PERF_FLAG_FD_CLOEXEC));
}

} // namespace

HwCounters::~HwCounters() {
    if (cycles_ >= 0) close(cycles_);
    if (leader_ >= 0) close(leader_);
}

bool HwCounters::Open(std::string& error) {
    leader_ = OpenEvent(PERF_COUNT_HW_INSTRUCTIONS, -1);
    if (leader_ >= 0) cycles_ = OpenEvent(PERF_COUNT_HW_CPU_CYCLES, leader_);
    if (leader_ < 0 || cycles_ < 0) {
        error = std::string("perf_event_open failed: ") + std::strerror(errno);
        std::ifstream paranoid("/proc/sys/kernel/perf_event_paranoid");
        int level = 0;
        if (paranoid >> level && level > 2)
            error += "; kernel.perf_event_paranoid is " + std::to_string(level) +
                     ", user-space counters need at most 2 (sudo sysctl kernel.perf_event_paranoid=2)";
        if (cycles_ >= 0) close(cycles_);
        if (leader_ >= 0) close(leader_);
        leader_ = cycles_ = -1;
        return false;
    }
    if (ioctl(leader_, PERF_EVENT_IOC_RESET, PERF_IOC_FLAG_GROUP) != 0 ||
        ioctl(leader_, PERF_EVENT_IOC_ENABLE, PERF_IOC_FLAG_GROUP) != 0) {
        error = std::string("cannot enable the counters: ") + std::strerror(errno);
        close(cycles_);
        close(leader_);
        leader_ = cycles_ = -1;
        return false;
    }
    return true;
}

bool HwCounters::Read(Values& values) const {
    // PERF_FORMAT_GROUP: nr, time_enabled, time_running, then one value per event.
    uint64_t data[5] = {};
    if (leader_ < 0 || read(leader_, data, sizeof(data)) != static_cast<ssize_t>(sizeof(data)) || data[0] != 2)
        return false;
    values.enabled_ns = data[1];
    values.running_ns = data[2];
    values.instructions = data[3];
    values.cycles = data[4];
    return true;
}
