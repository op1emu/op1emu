#include "io.h"
#include <cstring>

MemoryDevice::MemoryDevice(const std::string& name, u32 baseAddr, u32 size) : Device(name, baseAddr, size)
{
}

MemoryDevice::~MemoryDevice()
{
    if (fastmem) {
        fastmem->Unmap(baseAddress, size);
    }
}

void MemoryDevice::BindFastMem(const std::shared_ptr<FastMem>& mem)
{
    fastmem = mem;
    if (fastmem && fastmem->Enabled()) {
        memAddress = (u8*)fastmem->Map(baseAddress, size);
    } else {
        memory.resize(this->size);
        memAddress = memory.data();
    }
    memset(memAddress, 0, size);
}

void MemoryDevice::Read(u32 offset, void* buffer, u32 length)
{
    memcpy(buffer, memAddress + offset, length);
}

void MemoryDevice::Write(u32 offset, const void* buffer, u32 length)
{
    memcpy(memAddress + offset, buffer, length);
}

u32 MemoryDevice::Read32(u32 offset)
{
    return *(u32*)(memAddress + offset);
}

void MemoryDevice::Write32(u32 offset, u32 value)
{
    *(u32*)(memAddress + offset) = value;
}

void* MemoryDevice::Map(u32 offset)
{
    return memAddress + offset;
}

bool MemoryDevice::UpdatePageTable(std::array<u8*, NUM_PAGE_TABLE_ENTRIES>& table)
{
    for (u32 offset = 0; offset < size; offset += 1 << PAGE_BITS) {
        u32 addr = baseAddress + offset;
        table[addr >> PAGE_BITS] = memAddress + offset;
    }
    return true;
}


#define MAKE_32BIT_MASK(offset, length) (((~0U) >> (32 - (length))) << (offset))

u32 Register::Read32()
{
    u32 value = 0;
    if (readCallback) {
        value = readCallback();
    } else {
        for (const auto& [name, f] : fields) {
            u32 mask = MAKE_32BIT_MASK(f.offset, f.length);
            value = (value & ~mask) | ((f.readCallback() << f.offset) & mask);
        }
    }
    return value;
}

void Register::Write32(u32 value)
{
    for (const auto& [name, f] : fields) {
        u32 mask = MAKE_32BIT_MASK(f.offset, f.length);
        f.writeCallback((value & mask) >> f.offset);
    }
    if (writeCallback) {
        writeCallback(value);
    }
}

void RegisterDevice::Read(u32 offset, void* buffer, u32 length)
{
    u8* ptr = (u8*)buffer;
    for (int i = 0; i < (int)length; i += 4, offset += 4) {
        u32 val = Read32(offset);
        u32 bytes = std::min(4u, length - (u32)i);
        memcpy(ptr, &val, bytes);
        ptr += bytes;
    }
}

void RegisterDevice::Write(u32 offset, const void* buffer, u32 length)
{
    const u8* ptr = (const u8*)buffer;
    for (int i = 0; i < (int)length; i += 4, offset += 4) {
        u32 bytes = std::min(4u, length - (u32)i);
        u32 val = 0;
        memcpy(&val, ptr, bytes);
        Write32(offset, val);
        ptr += bytes;
    }
}

u32 RegisterDevice::Read32(u32 offset)
{
    auto iter = registers.find(offset);
    if (iter != registers.end()) {
        return iter->second.Read32();
    }
    return 0;
}

void RegisterDevice::Write32(u32 offset, u32 value)
{
    auto iter = registers.find(offset);
    if (iter != registers.end()) {
        iter->second.Write32(value);
    }
}