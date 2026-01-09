#include "fastmem.h"
#ifdef ENABLE_FASTMEM
#include <sys/mman.h>
#include <unistd.h>
#include <sys/types.h>
#endif
#include <cassert>
#include <utils/log.h>
#include <stdio.h>

#ifdef ENABLE_FASTMEM
FastMem::FastMem(u32 phys_mem_size, u32 virt_mem_size) : FastMem(nullptr, phys_mem_size, virt_mem_size)
{
    baseAddress = mmap(nullptr, virt_mem_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    assert(baseAddress != MAP_FAILED);
}

FastMem::FastMem(void* base_address, u32 phys_mem_size, u32 virt_mem_size)
{
    memFD = memfd_create("m8::FastMem", 0);
    int ret = ftruncate(memFD, phys_mem_size);
    assert(ret == 0);
    virtualMemSize = virt_mem_size;
    physicalMemSize = phys_mem_size;
    baseAddress = base_address;
}

FastMem::~FastMem()
{
    munmap(baseAddress, virtualMemSize);
    close(memFD);
}

bool FastMem::Enabled()
{
    return true;
}

void* FastMem::Map(u32 virt_mem_offset, u32 virt_mem_size)
{
    assert(physicalMemOffset + virt_mem_size <= physicalMemSize);
    void* ret = mmap((u8*)baseAddress + virt_mem_offset, virt_mem_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED, memFD, physicalMemOffset);
    assert(ret != MAP_FAILED);
    virtualMemMap.emplace(virt_mem_offset, physicalMemOffset);
    physicalMemOffset += virt_mem_size;
    return ret;
}

void FastMem::Unmap(u32 virt_mem_offset, u32 virt_mem_size)
{
    munmap((u8*)baseAddress + virt_mem_offset, virt_mem_size);
}

void* FastMem::MirrorMap(u32 virtual_mem_offset, u32 mirror_mem_offset, u32 virt_mem_size)
{
    if (virtualMemMap.count(virtual_mem_offset)) {
        u32 phys_mem_offset = virtualMemMap[virtual_mem_offset];
        void* ret = mmap((u8*)baseAddress + mirror_mem_offset, virt_mem_size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED | MAP_FIXED, memFD, phys_mem_offset);
        assert(ret != MAP_FAILED);
        virtualMemMap.emplace(mirror_mem_offset, phys_mem_offset);
        return ret;
    }
    return MAP_FAILED;
}

void FastMem::Protect(u32 virt_mem_offset, u32 virt_mem_size, bool read, bool write, bool execute)
{
    int prot = PROT_NONE;
    if (read) {
        prot |= PROT_READ;
    }
    if (write) {
        prot |= PROT_WRITE;
    }
    if (execute) {
        prot |= PROT_EXEC;
    }
    int ret = mprotect((u8*)baseAddress + virt_mem_offset, virt_mem_size, prot);
    if (ret != 0) {
        ext::LogError("FastMem: protect 0x%x:0x%x failed", virt_mem_offset, virt_mem_size);
    }
}

#else

FastMem::FastMem(u32 phys_mem_size, u32 virt_mem_size)
{
}

FastMem::FastMem(void* base_address, u32 phys_mem_size, u32 virt_mem_size)
{
}

FastMem::~FastMem()
{
}

bool FastMem::Enabled()
{
    return false;
}

void* FastMem::Map(u32 virt_mem_offset, u32 virt_mem_size)
{
    return nullptr;
}

void FastMem::Unmap(u32 virt_mem_offset, u32 virt_mem_size)
{
}

void* FastMem::MirrorMap(u32 virtual_mem_offset, u32 mirror_mem_offset, u32 virt_mem_size)
{
    return nullptr;
}

void FastMem::Protect(u32 virt_mem_offset, u32 virt_mem_size, bool read, bool write, bool execute)
{
}

#endif