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

// Generated code calls here only for what is not in DirectPages(): the MMRs,
// mostly. After the access, so that the device it reached can say from its
// state then whether the host has work after the block (see
// Emulator::HostWorkAfter, ReadAttended and WriteAttended). The CPU thread is the only one running guest
// code, but other threads set the budget to 0 too.
void EmulatorMemory::Attend(bool hostWork) const {
    if (!chainBudget_ || !hostWork) return;
#ifdef ENABLE_PROFILING
    if (__atomic_load_n(chainBudget_, __ATOMIC_RELAXED)) ++chainStops_;
#endif
    __atomic_store_n(chainBudget_, 0, __ATOMIC_RELAXED);
}

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
    bool hostWork, stable;
    const uint8_t value = emulator_.ReadAttended<uint8_t>(addr, hostWork, stable);
    Attend(hostWork);
    if (!stable && unstableReads_) ++*unstableReads_;
    return value;
}

uint16_t EmulatorMemory::read16(uint32_t addr) const {
    PROFILE_ACCESS(addr, 1, false);
    bool hostWork, stable;
    const uint16_t value = emulator_.ReadAttended<uint16_t>(addr, hostWork, stable);
    Attend(hostWork);
    if (!stable && unstableReads_) ++*unstableReads_;
    return value;
}

uint32_t EmulatorMemory::read32(uint32_t addr) const {
    PROFILE_ACCESS(addr, 2, false);
    bool hostWork, stable;
    const uint32_t value = emulator_.ReadAttended<uint32_t>(addr, hostWork, stable);
    Attend(hostWork);
    if (!stable && unstableReads_) ++*unstableReads_;
    return value;
}

void EmulatorMemory::write8(uint32_t addr, uint8_t val) {
    PROFILE_ACCESS(addr, 0, true);
    bool hostWork;
    emulator_.WriteAttended<uint8_t>(addr, val, hostWork);
    Attend(hostWork);
}

void EmulatorMemory::write16(uint32_t addr, uint16_t val) {
    PROFILE_ACCESS(addr, 1, true);
    bool hostWork;
    emulator_.WriteAttended<uint16_t>(addr, val, hostWork);
    Attend(hostWork);
}

void EmulatorMemory::write32(uint32_t addr, uint32_t val) {
    PROFILE_ACCESS(addr, 2, true);
    bool hostWork;
    emulator_.WriteAttended<uint32_t>(addr, val, hostWork);
    Attend(hostWork);
}

static_assert(PAGE_BITS == bcore::Memory::kPageBits, "bcore's page table has op1emu's page size");
static_assert(NUM_PAGE_TABLE_ENTRIES == bcore::Memory::kPageTableEntries, "one entry per guest page");

uint8_t* const* EmulatorMemory::page_table() const {
#ifdef ENABLE_PROFILING
    // The census counts in read*/write*. Switching it on drops every block
    // compiled before (BlackFinCpu::EnableMemoryCensus()), so no block that
    // bypasses it runs while it counts.
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
