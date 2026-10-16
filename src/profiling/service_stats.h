#pragma once

// What the host does around every block, counted (--profile-census): the
// device service calls and which of them did anything, the interrupt chain
// from a device's level to the CEC, and the CPU event queue. Times come from
// perf; these say how often, and how often for nothing.
//
// Sources are the CPU's devices in service order, then "block" (MMR
// accesses while a block runs), "events" (queue callbacks) and "other"
// (the rest of the CPU loop). Everything is counted on the CPU thread; what
// other threads do (the USB/IP loop publishes levels and queues events
// through the USB device) is only counted, in atomics, never attributed: the
// per-source and per-pin state belongs to the CPU thread.

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <thread>
#include <vector>

struct BcoreStats;

namespace profiling {

struct ServiceStats {
    struct Source {
        std::string name, type;  // device name and C++ class ("" for the non-device sources)
        std::vector<std::string> bases;  // the class's base classes, nearest first (single inheritance)
        uint64_t calls = 0;      // service calls (devices only)
        uint64_t productive = 0; // calls that changed a level, queued an event or moved data
        uint64_t published = 0;  // interrupt levels published to the SIC
        uint64_t transitions = 0;  // ... that changed the pin's level
        uint64_t queued = 0;     // events queued, other than interrupt raises
        uint64_t raises = 0;     // interrupt raises queued: none since lines are levels, kept for the schema
        uint64_t work = 0;       // data the device moved (DMA: bytes)
    };
    struct Pin {
        uint64_t published = 0, transitions = 0;
        int level = 0;
        int firstSource = -1;
    };

    std::vector<Source> sources;
    int block = 0, events = 0, other = 0;  // indexes of the non-device sources
    int current = 0;                       // the source running now
    std::array<Pin, 64> pins{};
    std::array<uint64_t, 16> forwarded{};  // high lines the CPU latched, per IVG
    std::array<uint64_t, 16> raised{};     // raises delivered to the CEC, per IVG
    uint64_t processCalls = 0, nonEmptyCalls = 0, lengthSum = 0, lengthMax = 0;
    std::atomic<uint64_t> queuedOtherThreads{0};
    std::atomic<uint64_t> publishedOtherThreads{0};
    std::atomic<uint64_t> forwardedOtherThreads{0};
    // Set when the CPU thread starts; until then every caller is another thread.
    std::atomic<std::thread::id> cpuThread{};
    uint64_t timerTicksDropped = 0;
    // Running totals of what the host did that the guest can see, for the
    // exit census: productive device calls, raises delivered, new ILAT bits
    // and GP timer ticks.
    uint64_t productiveTotal = 0, raisedTotal = 0, latchedTotal = 0, timerTicks = 0;

    struct DeviceInfo {
        std::string name, type;
        std::vector<std::string> bases;
    };
    // Devices first, in service order; then the three other sources.
    void SetDevices(const std::vector<DeviceInfo>& devices);
    bool OnCpuThread() const { return std::this_thread::get_id() == cpuThread.load(std::memory_order_relaxed); }
    void Published(int pin, int level);
    void Forwarded(int ivg);
    void Queued();  // from any thread

    // Cumulative counts as JSON (op1.service.v1), with the CEC's per-level
    // latched and taken interrupts and bcore's dispatch counts beside them.
    bool Write(const std::string& path, const std::array<uint64_t, 16>& latched, const BcoreStats& core) const;
};

}  // namespace profiling
