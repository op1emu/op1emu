#pragma once

#include "common.h"
#include <map>

class FastMem {
public:
    FastMem(u32 phys_mem_size, u32 virt_mem_size);
    FastMem(void* base_address, u32 phys_mem_size, u32 virt_mem_size);
    ~FastMem();

    bool Enabled();

    void* BaseAddress() { return baseAddress; }

    void* Map(u32 virt_mem_offset, u32 virt_mem_size);
    void Unmap(u32 virt_mem_offset, u32 virt_mem_size);
    void* MirrorMap(u32 virtual_mem_offset, u32 mirror_mem_offset, u32 virt_mem_size);

    void Protect(u32 virt_mem_offset, u32 virt_mem_size, bool read, bool write, bool execute);

protected:
    int memFD = 0;
    void* baseAddress = nullptr;
    u32 virtualMemSize = 0;
    u32 physicalMemSize = 0;
    u32 physicalMemOffset = 0;
    std::map<u32, u32> virtualMemMap;
};