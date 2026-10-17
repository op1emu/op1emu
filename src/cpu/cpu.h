#pragma once

#include "emu.h"
#include "time_source.h"
#include <array>
#include <atomic>
#include <memory>
#include <vector>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <tuple>

// Forward declarations for bcore types (headers included in cpu.cpp only)
struct CpuState;
class Core;
class EmulatorMemory;

enum RegIndex {
    FP,
    SP,
    RETS,
    R0,
    R1,
    R2,
    P1,
};

class SIC;
class RTC;
class DMA;
class TWI;
class USB;
class CoreTimer;
class GPIO;
class PPI;
class Display;
class OLED;
class NFC;
class GPTimer;
class USBDevice;
class NandFlash;
class Keyboard;
class MCP230XX;
class ADXL345;
class Potentiometer;
class GPIOPeripheral;
class SPORT;
class AudioOutput;
class Profiler;
struct BcoreStats;
namespace profiling { struct ServiceStats; struct MemoryCensus; }
class BcoreEventSink;

class BlackFinCpu : public CpuInterface {
public:
    // deterministic selects the guest-time model; see TimeSource.
    explicit BlackFinCpu(bool deterministic = false);
    ~BlackFinCpu() override;

    void HaltExecution(HaltReason reason) override;
    void SaveContext() override;
    void RestoreContext() override;
    HaltReason Run() override;

    void SetRegister(int index, u32 value) override;
    u32 GetRegister(int index) override;

    void SetPC(u32 value) override;
    u32 PC() override;

    Emulator& GetEmulator() { return emulator; }
    USBDevice& GetUSB() { return *usb; }

    void SetBootMode(int mode);
    // Whether a run may go on from one block to the next without returning
    // here (bcore's CpuState::chain_budget), until the host has work. With
    // dry, blocks only report that they would have (profiling builds check
    // that the host then had nothing to do). Before the first Run().
    void SetChaining(bool chaining, bool dry = false);
#ifdef ENABLE_PROFILING
    // Returns to the host by why the run ended (cumulative), and the dry
    // run's checks: profiling's [chain] lines.
    struct ChainCounts {
        uint64_t runs = 0;        // returns to the host
        uint64_t chained = 0;     // blocks run without a return before them
        uint64_t hostOnly = 0;    // the run started where it may not chain
        uint64_t service = 0;     // a device had work before the run
        uint64_t deadline = 0;    // the budget ran out: a device or event came due
        uint64_t access = 0;      // a device access
        uint64_t cec = 0;         // an interrupt entry or return, an exception or a halt
        uint64_t pending = 0;     // an interrupt may be pending
        uint64_t miss = 0;        // the next block was not in the chain table
        uint64_t would = 0;       // dry: the block would have chained
        uint64_t violations = 0;  // dry: ... but the host had work after it
        std::array<uint64_t, 8> violation{};  // by HostWork kind
    };
    const ChainCounts& Chains() const { return chainCounts_; }
#endif
    // See TWI::SetSpeedup.
    void SetTwiSpeedup(unsigned speedup);

    // True once the JIT could not translate or run a block. The CPU state is
    // then no longer advancing, so the host should stop instead of retrying.
    bool Failed() const { return failed_.load(std::memory_order_acquire); }

    void QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay = std::chrono::nanoseconds(1));

    void AttachDisplay(const std::shared_ptr<Display>& display);
    void AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard);
    void AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash);
    void AttachAudioOutput(const std::shared_ptr<AudioOutput>& audioOutput);
    void SetAcceleration(int16_t x, int16_t y, int16_t z);
    void SetPotentiometerValue(u8 value);

    const TimeSource& Time() const { return time_; }
#ifdef ENABLE_PROFILING
    // Profiling hooks (see profiling/profiler.h). Not owned.
    void SetProfiler(Profiler* profiler);
    BcoreStats CoreStats() const;
    void SetCoreEventSink(BcoreEventSink* sink);
    void SetPerfJitdump(bool enable);
    uint64_t EventsDelivered() const { return eventsDelivered_; }
    // Host-raised interrupts that set a new ILAT bit, per IVG level.
    const std::array<uint64_t, 16>& IrqLatched() const { return irqLatched_; }
    // Count the host's service work from now on (census runs). The CPU owns
    // the counts and keeps them as long as it lives: the USB/IP thread
    // publishes interrupt levels through the CPU's USB device at any time,
    // so the counts live exactly as long as what it calls into.
    profiling::ServiceStats& EnableServiceStats();
    // Count guest memory accesses by device (census runs); owned like the service counts.
    profiling::MemoryCensus& EnableMemoryCensus();
    const CpuState& State() const { return *cpuState_; }
#endif
    // Deterministic mode only: RTC calendar time at guest time zero.
    void SetRtcEpoch(std::chrono::system_clock::time_point epoch) { time_.SetEpoch(epoch); }

protected:
    void ProcessInterrupt(int pin, int level);
    void ServicePanel();
    void ProcessEvents();
    void RaiseInterrupt(int ivg);
    void LatchLines();
    // The devices with work between blocks, each called only when it has some.
    void ServiceDevices(int ivg);
    template <class D> void ServiceDevice(D& device, size_t index, int ivg);
    // Drop every compiled block (bcore's cache), timed by the profiler.
    void InvalidateTranslations();

    // Declared before the devices, which keep a reference to it.
    TimeSource time_;
    PanelOscillator panel_;

    std::shared_ptr<SIC> sic;
    std::shared_ptr<CoreTimer> coreTimer;
    std::shared_ptr<GPIO> portG;
    std::shared_ptr<PPI> ppi;
    std::shared_ptr<OLED> oled;
    std::shared_ptr<NFC> nfc;
    std::shared_ptr<GPTimer> gptimer;
    std::shared_ptr<USBDevice> usb;
    std::shared_ptr<SPORT> sport0;
    std::shared_ptr<SPORT> sport1;
    std::shared_ptr<ADXL345> adxl345;
    std::shared_ptr<Potentiometer> potentiometer;
    std::shared_ptr<GPIOPeripheral> gpioOrConnection;
    std::vector<std::shared_ptr<MCP230XX>> gpioExpanders;
    std::vector<std::shared_ptr<Device>> devices;
    // The devices ServiceDevices() calls, in the order of `devices`, with
    // their index there (the service census counts per device index).
    struct Serviced {
        std::shared_ptr<USB> usb;
        std::shared_ptr<NFC> nfc;
        std::shared_ptr<RTC> rtc;
        std::shared_ptr<DMA> dma;
        std::shared_ptr<CoreTimer> coreTimer;
        std::shared_ptr<TWI> twi;
        size_t usbIndex = 0, nfcIndex = 0, rtcIndex = 0, dmaIndex = 0, coreTimerIndex = 0, twiIndex = 0;
    } serviced_;
    // Events in guest time (TimeSource::Nanoseconds()), owned by the CPU
    // thread: a min-heap on (due, sequence), so events due together run in
    // the order they were queued.
    struct TimedEvent {
        uint64_t dueNs;
        uint64_t sequence;
        std::function<void()> run;
        bool operator>(const TimedEvent& other) const {
            return dueNs != other.dueNs ? dueNs > other.dueNs : sequence > other.sequence;
        }
    };
    std::vector<TimedEvent> events_;
    uint64_t nextEventNs_ = UINT64_MAX;  // events_.front().dueNs, or none
    uint64_t eventSequence_ = 0;
    // What other threads queue: timed from when the CPU thread takes it.
    std::mutex inboxMutex_;
    std::vector<std::tuple<std::chrono::nanoseconds, std::function<void()>>> inbox_;
    std::atomic<bool> inboxPending_{false};
    // Set by the first Run(); until then every caller counts as another thread.
    std::atomic<std::thread::id> cpuThread_{};
    void ScheduleEvent(std::function<void()> event, uint64_t dueNs);
    void StopChaining();
    // Packets the run starting at pc may retire before the host has work.
    uint32_t ChainBudget(uint32_t pc);
    bool chaining_ = false;
    // A block is running (Core::run): what a device access queues now was
    // not in the run's budget.
    bool inRun_ = false;
    static constexpr uint32_t kMaxChainPackets = 1u << 24;
    // Wall mode: no packet count says when a device is due.
    static constexpr uint32_t kWallChainPackets = 4096;
    // The GP timers tick every kCyclesPerTimerTick guest cycles (see Run()).
    static constexpr uint64_t kCyclesPerTimerTick = 10000;
    static constexpr uint64_t kMaxCatchupTicks = 64;
#ifdef ENABLE_PROFILING
    // The host's work between blocks, by kind, for the dry run's check.
    // kWorkQuiet: a service the device's contract skipped would have changed
    // something (CheckSkippedServices).
    enum HostWork { kWorkService, kWorkTimerTick, kWorkExpiry, kWorkPanel, kWorkEvent, kWorkLatch, kWorkCec, kWorkQuiet,
                    kWorkKinds };
    void CheckSkippedServices(int ivg);
    std::array<uint64_t, kWorkKinds> hostWork_{};
    ChainCounts chainCounts_;
    void CountChain(uint32_t budget, uint32_t retired, uint32_t chained, uint64_t accessStops);
    enum ChainReason { kChainHostOnly, kChainService, kChainDue } chainReason_ = kChainDue;
#endif
    // A line rose on another thread (LatchLines() left it to the CPU thread).
    std::atomic<bool> offThreadLatch_{false};
#ifdef ENABLE_PROFILING
    Profiler* profiler_ = nullptr;
    uint64_t eventsDelivered_ = 0;
    std::array<uint64_t, 16> irqLatched_{};
    std::unique_ptr<profiling::ServiceStats> serviceStats_;
    std::unique_ptr<profiling::MemoryCensus> memoryCensus_;
    std::atomic<profiling::ServiceStats*> service_{nullptr};
    profiling::ServiceStats* Service() const { return service_.load(std::memory_order_acquire); }
#endif
    bool coreTimerExpired_ = false;  // an expiry not latched yet (user mode holds it)
    bool userModeLatchWarned_ = false;
#ifdef ENABLE_PROFILING
    uint64_t userModeLatchDeferred_ = 0;
#endif
    // Last cyclesElapsed value the GP timers were advanced to (see Run()).
    uint64_t lastTimerCycles_ = 0;
    Emulator emulator;
    std::unique_ptr<CpuState> cpuState_;
    std::unique_ptr<EmulatorMemory> bcoreMemory_;
    std::shared_ptr<Core> core_;
    uint32_t pc;
    std::atomic<bool> failed_{false};
};