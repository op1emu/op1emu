#pragma once

// bcore headers (from ext/bcore/include/)
#include "mem.h"

// op1emu headers
#include "emu.h"

class Profiler;

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

    const uint8_t* raw() const override;

    uint32_t rawmem_limit() const override { return 0; }

#ifdef ENABLE_PROFILING
    // Counts guest accesses to the MMR space (0xFFC00000 and up). Not owned.
    void SetProfiler(Profiler* profiler) { profiler_ = profiler; }
#endif

private:
    Emulator& emulator_;
#ifdef ENABLE_PROFILING
    Profiler* profiler_ = nullptr;
    void Mmio(uint32_t addr, bool write) const;
#endif
};
