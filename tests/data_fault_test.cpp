// A misaligned guest load raises EXCAUSE 0x24 and the fault MMRs hold what the guest's EVT3 handler reads.
#include "cpu/emu.h"
#include "cpu/bcore_memory.h"
#include "core.h"
#include "mmr.h"
#include "../ext/bcore/src/cec.h"
#include "../ext/bcore/src/evt.h"
#include <cstdio>
#include <cstdlib>

static void require(bool ok, const char* why) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", why); exit(1); }
}

int main() {
    Emulator emu;
    MemoryDevice ram("RAM", 0, 0x10000), mmu("MMU", 0xFFE00000, 0x2000);
    ram.BindFastMem(nullptr); mmu.BindFastMem(nullptr);
    emu.BindDevice(&ram); emu.BindDevice(&mmu);
    EmulatorMemory mem(emu);
    bcore::CpuState cpu{};
    bcore::Core core(&cpu, &mem);
    require(core.init(2), "init");
    bcore::cec_init(); bcore::evt_init(); bcore::evt_write(bcore::EVT_BASE + 12, 0x300); bcore::cec_return_rti(&cpu);

    emu.MemoryWrite16(0x100, 0x9000); // R0 = [P0++]
    emu.MemoryWrite16(0x102, 0x2000);
    cpu.dpregs[0] = 0x1234; cpu.dpregs[8] = 0x201;
    require(core.run(0x100), "faulting load");
    require(cpu.pc == 0x300 && cpu.retx == 0x100 && (cpu.seqstat & 63) == 0x24, "exception dispatch");
    require(cpu.dpregs[0] == 0x1234 && cpu.dpregs[8] == 0x201, "destination and post-increment untouched");
    require(emu.MemoryRead32(0xFFE0000C) == 0x201 && emu.MemoryRead32(0xFFE00008) == 0, "data fault MMRs");
    require(emu.MemoryRead32(0xFFE0100C) == 0x100 && emu.MemoryRead32(0xFFE01008) == 0, "instruction fault MMRs");

    // The guest handler reads the fault address through the JIT.
    emu.MemoryWrite16(0x300, 0x9000); emu.MemoryWrite16(0x302, 0x2000);
    cpu.dpregs[8] = 0xFFE0000C;
    require(core.run(cpu.pc), "guest handler");
    require(cpu.dpregs[0] == 0x201, "guest can read the fault address");
    puts("PASS: data alignment fault reaches EVT3 with fault MMRs");
}
