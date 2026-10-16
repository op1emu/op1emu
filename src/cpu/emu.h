#pragma once

#include <map>
#include <mutex>
#include <functional>
#include <vector>
#include "io.h"
#include "fastmem.h"

enum class HaltReason {
    Break,
    Interrupt,
};

class CpuInterface {
public:
    virtual ~CpuInterface() {}
    virtual void HaltExecution(HaltReason reason) = 0;
    virtual void SaveContext() = 0;
    virtual void RestoreContext() = 0;
    virtual HaltReason Run() = 0;

    virtual void SetRegister(int index, u32 value) = 0;
    virtual u32 GetRegister(int index) = 0;

    virtual void SetPC(u32 value) = 0;
    virtual u32 PC() = 0;
};

class Emulator {
public:
    Emulator();
    virtual ~Emulator() {}

    void BindFastMem(const std::shared_ptr<FastMem>& mem) { this->fastmem = mem; }
    void BindDevice(Device* dev);

    void Lock();
    void Unlock();
    void MemoryRead(u32 addr, void* buffer, int length);
    void MemoryWrite(u32 addr, const void* buffer, int length);

    u8 MemoryRead8(u32 vaddr);
    u16 MemoryRead16(u32 vaddr);
    u32 MemoryRead32(u32 vaddr);
    // True when a device maps every byte of [addr, addr + length).
    bool Maps(u32 addr, u32 length);
    u64 MemoryRead64(u32 vaddr);

    void MemoryWrite8(u32 vaddr, u8 value);
    void MemoryWrite16(u32 vaddr, u16 value);
    void MemoryWrite32(u32 vaddr, u32 value);
    void MemoryWrite64(u32 vaddr, u64 value);

    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected);

    void* MemoryMap(u32 addr);

    std::array<u8*, NUM_PAGE_TABLE_ENTRIES>& PageTable() { return *pageTable; }
    FastMem& GetFastMem() { return *fastmem; }

    // A hook runs inside a guest load or store that generated code makes
    // through EmulatorMemory, which tells bcore its callbacks leave the guest
    // registers alone (Memory::callbacks_preserve_registers()): a hook must
    // not write a guest register. The page it sits in stops being direct
    // (DirectPages()) at once, also for blocks already compiled.
    void AddReadHook(u32 addr, std::function<u32(u32)> hook) { readHooks[addr] = hook; RebuildDirectPages(); }
    void AddWriteHook(u32 addr, std::function<void(u32, u32)> hook) { writeHooks[addr] = hook; RebuildDirectPages(); }

    // The guest pages generated code may load from and store to directly
    // (bcore's Memory::page_table()): one entry per page, the host address of
    // its first byte, or null where an access must come through the device
    // map. A page qualifies when one plain MemoryDevice (not a subclass, which
    // may give its accesses side effects) maps every byte of it, it lies below
    // the MMR space at 0xFFC00000 (every MMR access stays a call, and is
    // counted as one) and no read or write hook sits in it. The table's address never
    // changes; binding a device or adding a hook rebuilds it in place.
    u8* const* DirectPages() const { return directPages.data(); }

    void PatchSoftwareBreak(u32 addr, u8 num);
    void RemoveSoftwareBreak(u32 addr);

    bool IsMemoryValid(u32 addr);
    void RebuildDirectPages();

    const std::vector<Device*>& Devices() const { return devices; }

    const std::map<u32, std::function<u32(u32)>>& ReadHooks() const { return readHooks; }
    const std::map<u32, std::function<void(u32, u32)>>& WriteHooks() const { return writeHooks; }

protected:
    std::shared_ptr<FastMem> fastmem;
    std::recursive_mutex mutex;
    std::shared_ptr<std::array<u8*, NUM_PAGE_TABLE_ENTRIES>> pageTable;
    std::map<u32, Device*> deviceSegments;
    std::vector<Device*> devices;
    std::map<u32, std::function<u32(u32)>> readHooks;
    std::map<u32, std::function<void(u32, u32)>> writeHooks;
    std::vector<u8*> directPages;
};