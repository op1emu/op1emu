#include "cpu/cpu.h"
#include "cpu/twi.h"
#include "cpu/usb.h"
#include "peripheral/MT29F4G08.h"
#include "profiling/memory_census.h"
#include "profiling/profiler.h"
#include "profiling/service_stats.h"
#include "bcore_profile.h"
#include "cpu_state.h"
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

int main() {
    {
        // A master read with every TWI interrupt masked moves a byte and
        // changes no level: the census must still see the work.
        TWI twi(0xFFC01400);
        twi.AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x10, 0xA5));
        twi.Write32(0x04, 1u << 7);                       // CONTROL: TWI_ENA
        twi.Write32(0x24, 0);                             // INT_MASK: nothing
        twi.Write32(0x1C, 0x10);                          // MASTER_ADDR
        twi.Write32(0x14, (1u << 6) | (1u << 2) | 1u);    // MASTER_CTL: DCNT 1, read, MEN
        const uint64_t before = twi.ServiceWork();
        twi.ProcessWithInterrupt(-1);
        Require(twi.ServiceWork() - before == 1, "a TWI master read reports the byte it moved");
        Require(twi.Read32(0x88) == 0xA5, "and the byte is the peripheral's");
    }
    {
        profiling::ServiceStats stats;
        stats.SetDevices({{"USB", "USB", {"Device"}}});
        stats.cpuThread.store(std::this_thread::get_id());
        std::thread other([&] {
            stats.Published(52, 1);
            stats.Forwarded(13);
            stats.Queued();
        });
        other.join();
        Require(stats.publishedOtherThreads == 1 && stats.forwardedOtherThreads == 1 && stats.queuedOtherThreads == 1,
                "another thread's publication, forward and event are counted apart");
        Require(stats.pins[52].published == 0 && stats.forwarded[13] == 0,
                "and leave the CPU thread's state alone");
        for (const auto& source : stats.sources)
            Require(source.published == 0 && source.queued == 0 && source.raises == 0,
                    "no source is charged for another thread's work");
        stats.current = 0;
        stats.Published(52, 1);
        Require(stats.pins[52].published == 1 && stats.pins[52].transitions == 1 && stats.sources[0].transitions == 1,
                "the CPU thread's publication is attributed to the running source");
    }
    {
        // Other threads queue events at any time, also after the profiler is
        // gone: the counts they reach belong to the CPU and outlive the
        // profiler. The USB/IP thread's requests publish nothing: the CPU
        // thread services them.
        BlackFinCpu cpu(true);
        profiling::ServiceStats* stats = nullptr;
        {
            ProfilerOptions options;
            options.censusPrefix = "service-stats-test";
            Profiler profiler(options, cpu, [] {});
            stats = &cpu.EnableServiceStats();
            Require(stats == &cpu.EnableServiceStats(), "one set of counts per CPU");
        }
        const uint64_t published = stats->publishedOtherThreads;
        const uint64_t queued = stats->queuedOtherThreads;
        std::thread usbip([&] {
            const u8 packet[4] = {1, 2, 3, 4};
            cpu.GetUSB().HandleDataWrite(1, 0, packet, sizeof(packet), [] {});
            cpu.SetPotentiometerValue(7);
        });
        usbip.join();
        Require(stats->publishedOtherThreads == published, "a USB/IP request publishes no level off the CPU thread");
        Require(stats->queuedOtherThreads > queued, "another thread's events still land in live counts");
    }
    {
        // bcore fetches a block's instructions through the host memory as it
        // translates it: those reads are compilation, not guest accesses, so
        // the census counts the same for a cold block, a cached one and one
        // recompiled after the cache flush at the boot ROM entry.
        BlackFinCpu cpu(true);
        // Run() services the NFC, which needs a flash: an erased one, read-only.
        cpu.AttachNandFlash(std::make_shared<MT29F4G08>(cpu, "service-stats-test.nand", true));
        auto& emulator = cpu.GetEmulator();
        auto code = [&](u32 at, std::initializer_list<u16> words) {
            for (u16 word : words) { emulator.MemoryWrite16(at, word); at += 2; }
        };
        code(0xFFA00000, {0x0000, 0x0000, 0x2FFE});  // NOP; NOP; JUMP.S back (the flush entry)
        code(0xFFA00100, {0x0000, 0x0000, 0x2FFE});  // NOP; NOP; JUMP.S back: no data access
        code(0xFFA00200, {0x9100, 0x2FFF});          // R0 = [P0]; JUMP.S back: one load
        ProfilerOptions options;
        options.censusPrefix = "service-stats-test";
        Profiler profiler(options, cpu, [] {});
        cpu.SetProfiler(&profiler);  // as main() does: the hooks and the event sink
        auto& memory = cpu.EnableMemoryCensus();
        auto run = [&](u32 pc) { cpu.SetPC(pc); cpu.Run(); };
        const uint64_t translated = cpu.CoreStats().blocks_translated;
        run(0xFFA00100);
        Require(cpu.CoreStats().blocks_translated == translated + 1 && memory.accesses == 0,
                "a cold translation's instruction fetches are not guest accesses");
        run(0xFFA00100);
        Require(memory.accesses == 0, "nor are a cached block's");
        run(0xFFA00200);
        Require(memory.accesses == 1, "a load counts once, on its cold run");
        run(0xFFA00200);
        Require(memory.accesses == 2, "and once on its cached run");
        run(0xFFA00000);  // the boot ROM entry flushes the cache
        run(0xFFA00200);
        Require(cpu.CoreStats().blocks_translated == translated + 4 && memory.accesses == 3,
                "a block recompiled after the flush counts its load once");
        Require(!memory.paused, "counting resumes after every translation");
        cpu.SetProfiler(nullptr);
    }
    {
        // A block compiled before the census is switched on loads RAM through
        // the page table, past EmulatorMemory: switching it on must drop the
        // block, so its next run is recompiled and counts the load.
        BlackFinCpu cpu(true);
        cpu.AttachNandFlash(std::make_shared<MT29F4G08>(cpu, "service-stats-test.nand", true));
        auto& emulator = cpu.GetEmulator();
        emulator.MemoryWrite16(0xFFA00200, 0x9100);  // R0 = [P0]
        emulator.MemoryWrite16(0xFFA00202, 0x2FFF);  // JUMP.S back
        auto run = [&] { cpu.SetPC(0xFFA00200); cpu.Run(); };
        run();
        const uint64_t translated = cpu.CoreStats().blocks_translated;
        ProfilerOptions options;
        options.censusPrefix = "service-stats-test";
        Profiler profiler(options, cpu, [] {});  // switches the census on
        cpu.SetProfiler(&profiler);
        auto& memory = cpu.EnableMemoryCensus();
        run();
        Require(cpu.CoreStats().blocks_translated == translated + 1 && memory.accesses == 1,
                "a block compiled before the census is recompiled and counts its load");
        cpu.SetProfiler(nullptr);
    }
    std::puts("ServiceStats tests passed");
}
