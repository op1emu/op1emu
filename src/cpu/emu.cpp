#include <algorithm>
#include <atomic>
#include "emu.h"
#include <utils/log.h>

Emulator::Emulator()
{
    pageTable = std::make_shared<std::array<u8*, NUM_PAGE_TABLE_ENTRIES>>();
}

void Emulator::BindDevice(Device* dev)
{
    devices.push_back(dev);
    deviceSegments.emplace(dev->BaseAddress(), dev);
    deviceSegments.emplace(dev->EndAddress(), dev);
    dev->BindFastMem(fastmem);
    dev->UpdatePageTable(*pageTable);
}

void Emulator::Lock()
{
    mutex.lock();
}

void Emulator::Unlock()
{
    mutex.unlock();
}

static Device* get_device(u32 addr, const std::map<u32, Device*>& devices)
{
    auto iter = devices.lower_bound(addr);
    if (iter != devices.end()) {
        Device* dev = iter->second;
        if (addr >= dev->BaseAddress() && addr <= dev->EndAddress()) {
            return dev;
        }
    }
    LogDebug("get_device nullptr: addr = 0x%x", addr);
    return nullptr;
}

void Emulator::MemoryRead(u32 addr, void* buffer, int length)
{
    while (length > 0) {
        Device* dev = get_device(addr, deviceSegments);
        if (!dev) {
            length = 0;
        } else {
            u32 offset = addr - dev->BaseAddress();
            u32 len = std::min((u32)length, dev->Size() - offset);
            dev->Read(offset, buffer, len);
            addr += len;
            buffer = (void*)((u8*)buffer + len);
            length -= len;
        }
    }
}

void Emulator::MemoryWrite(u32 addr, const void* buffer, int length)
{
    while (length > 0) {
        Device* dev = get_device(addr, deviceSegments);
        if (!dev) {
            length = 0;
        } else {
            u32 offset = addr - dev->BaseAddress();
            u32 len = std::min((u32)length, dev->Size() - offset);
            dev->Write(offset, buffer, len);
            addr += len;
            buffer = (void*)((u8*)buffer + len);
            length -= len;
        }
    }
}

u8 Emulator::MemoryRead8(u32 vaddr)
{
    u8 value;
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

u16 Emulator::MemoryRead16(u32 vaddr)
{
    u16 value;
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

u32 Emulator::MemoryRead32(u32 vaddr)
{
    if (readHooks.find(vaddr) != readHooks.end()) {
        return readHooks[vaddr](vaddr);
    } else {
        Device* dev = get_device(vaddr, deviceSegments);
        if (!dev) {
            return 0;
        } else {
            u32 offset = vaddr - dev->BaseAddress();
            return dev->Read32(offset);
        }
    }
}

u64 Emulator::MemoryRead64(u32 vaddr)
{
    u64 value;
    MemoryRead(vaddr, &value, sizeof(value));
    return value;
}

void Emulator::MemoryWrite8(u32 vaddr, u8 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

void Emulator::MemoryWrite16(u32 vaddr, u16 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

void Emulator::MemoryWrite32(u32 vaddr, u32 value)
{
    if (writeHooks.find(vaddr) != writeHooks.end()) {
        writeHooks[vaddr](vaddr, value);
    } else {
        Device* dev = get_device(vaddr, deviceSegments);
        if (dev) {
            u32 offset = vaddr - dev->BaseAddress();
            dev->Write32(offset, value);
        }
    }
}

void Emulator::MemoryWrite64(u32 vaddr, u64 value)
{
    MemoryWrite(vaddr, &value, sizeof(value));
}

bool Emulator::MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected)
{
    auto atomic = (std::atomic<u32>*)MemoryMap(vaddr);
    return atomic->compare_exchange_strong(expected, value);
}

void* Emulator::MemoryMap(u32 addr)
{
    Device* dev = get_device(addr, deviceSegments);
    if (!dev) {
        return nullptr;
    } else {
        u32 offset = addr - dev->BaseAddress();
        return dev->Map(offset);
    }
}

bool Emulator::IsMemoryValid(u32 addr)
{
    return get_device(addr, deviceSegments) != nullptr;
}

void Emulator::PatchSoftwareBreak(u32 addr, u8 num)
{
}

void Emulator::RemoveSoftwareBreak(u32 addr)
{
}