#include "bcore_memory.h"
#include "utils/log.h"
#ifdef ENABLE_PROFILING
#include "profiling/memory_census.h"
#include "profiling/profiler.h"

void EmulatorMemory::Access(uint32_t addr, int widthIndex, bool write) const {
    if (census_) census_->Count(addr, widthIndex, write);
    if (profiler_ && addr >= 0xFFC00000) profiler_->OnMmio(addr, write);
}
#define PROFILE_ACCESS(addr, width, write) Access(addr, width, write)
#else
#define PROFILE_ACCESS(addr, width, write) ((void)0)
#endif

EmulatorMemory::EmulatorMemory(Emulator& emulator)
    : emulator_(emulator) {}

uint32_t EmulatorMemory::base() const {
    return 0;
}

uint32_t EmulatorMemory::size() const {
    return 0xFFFFFFFF;
}

uintptr_t EmulatorMemory::fast_base() const {
    return 0;
}

uint8_t EmulatorMemory::read8(uint32_t addr) const {
    PROFILE_ACCESS(addr, 0, false);
    return emulator_.MemoryRead8(addr);
}

uint16_t EmulatorMemory::read16(uint32_t addr) const {
    PROFILE_ACCESS(addr, 1, false);
    return emulator_.MemoryRead16(addr);
}

uint32_t EmulatorMemory::read32(uint32_t addr) const {
    PROFILE_ACCESS(addr, 2, false);
    return emulator_.MemoryRead32(addr);
}

void EmulatorMemory::write8(uint32_t addr, uint8_t val) {
    PROFILE_ACCESS(addr, 0, true);
    emulator_.MemoryWrite8(addr, val);
}

void EmulatorMemory::write16(uint32_t addr, uint16_t val) {
    PROFILE_ACCESS(addr, 1, true);
    emulator_.MemoryWrite16(addr, val);
}

void EmulatorMemory::write32(uint32_t addr, uint32_t val) {
    PROFILE_ACCESS(addr, 2, true);
    emulator_.MemoryWrite32(addr, val);
}

static_assert(PAGE_BITS == Memory::kPageBits, "bcore's page table has op1emu's page size");
static_assert(NUM_PAGE_TABLE_ENTRIES == Memory::kPageTableEntries, "one entry per guest page");

uint8_t* const* EmulatorMemory::page_table() const {
#ifdef ENABLE_PROFILING
    // The census counts in read*/write*. It is switched on before the first
    // translation, so no block that bypasses it is ever compiled.
    if (census_) return nullptr;
#endif
    return emulator_.DirectPages();
}

const uint8_t* EmulatorMemory::raw() const {
    return nullptr;
}

void EmulatorMemory::data_alignment_fault(uint32_t pc, uint32_t addr, uint32_t status) {
    emulator_.MemoryWrite32(0xFFE00008, status);              // DCPLB_FAULT_STATUS
    emulator_.MemoryWrite32(0xFFE0000C, addr);                // DCPLB_FAULT_ADDR
    emulator_.MemoryWrite32(0xFFE01008, status & (1u << 17)); // ICPLB_FAULT_STATUS: supervisor bit only
    emulator_.MemoryWrite32(0xFFE0100C, pc);                  // ICPLB_FAULT_ADDR: the faulting instruction
    LogWarn("Data alignment fault: PC=%08x ADDR=%08x STATUS=%08x", pc, addr, status);
}
