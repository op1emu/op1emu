#pragma once

// bcore headers (from ext/bcore/include/)
#include "mem.h"

// op1emu headers
#include "emu.h"

class Profiler;
namespace profiling { struct MemoryCensus; }

class EmulatorMemory : public Memory {
public:
    explicit EmulatorMemory(Emulator& emulator);

    uint32_t base() const override;
    uint32_t size() const override;
    uintptr_t fast_base() const override;

    uint8_t  read8(uint32_t addr) const override;
    uint16_t read16(uint32_t addr) const override;
    uint32_t read32(uint32_t addr) const override;

    void write8(uint32_t addr, uint8_t val) override;
    void write16(uint32_t addr, uint16_t val) override;
    void write32(uint32_t addr, uint32_t val) override;

    // Records the fault in DCPLB_FAULT_STATUS/ADDR and ICPLB_FAULT_STATUS/ADDR for the guest's EVT3 handler.
    void data_alignment_fault(uint32_t pc, uint32_t addr, uint32_t status) override;
    const uint8_t* raw() const override;

    uint32_t rawmem_limit() const override { return 0; }

    // Emulator::DirectPages(), so generated code reaches RAM without calling
    // back here; null while the memory census counts every access.
    uint8_t* const* page_table() const override;

    // Device callbacks, data_alignment_fault() and the Emulator's read and
    // write hooks reach CEC state (cec_mmr_*, cec_raise) and peripheral state,
    // never a guest register, so generated code may keep registers in host
    // registers across an access that calls back here.
    bool callbacks_preserve_registers() const override { return true; }

    // bcore's CpuState::chain_budget: an access that may give the host work
    // (one that is not Emulator::Quiet) sets it to 0, so the run returns to
    // the host after the block. Not owned.
    void SetChainBudget(uint32_t* budget) { chainBudget_ = budget; }
#ifdef ENABLE_PROFILING
    // Accesses that stopped a run's chaining (BlackFinCpu's [chain] counts).
    uint64_t ChainStops() const { return chainStops_; }

    // Counts guest accesses to the MMR space (0xFFC00000 and up). Not owned.
    void SetProfiler(Profiler* profiler) { profiler_ = profiler; }
    // Counts every access by device and width (census runs). Not owned.
    void SetMemoryCensus(profiling::MemoryCensus* census) { census_ = census; }
#endif

private:
    void Attend(uint32_t addr) const;

    Emulator& emulator_;
    uint32_t* chainBudget_ = nullptr;
#ifdef ENABLE_PROFILING
    Profiler* profiler_ = nullptr;
    profiling::MemoryCensus* census_ = nullptr;
    mutable uint64_t chainStops_ = 0;
    void Access(uint32_t addr, int widthIndex, bool write) const;
#endif
};
