#include "bcore_memory.h"

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
    return emulator_.MemoryRead8(addr);
}

uint16_t EmulatorMemory::read16(uint32_t addr) const {
    return emulator_.MemoryRead16(addr);
}

uint32_t EmulatorMemory::read32(uint32_t addr) const {
    return emulator_.MemoryRead32(addr);
}

void EmulatorMemory::write8(uint32_t addr, uint8_t val) {
    emulator_.MemoryWrite8(addr, val);
}

void EmulatorMemory::write16(uint32_t addr, uint16_t val) {
    emulator_.MemoryWrite16(addr, val);
}

void EmulatorMemory::write32(uint32_t addr, uint32_t val) {
    emulator_.MemoryWrite32(addr, val);
}

const uint8_t* EmulatorMemory::raw() const {
    return nullptr;
}
