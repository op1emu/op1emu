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
    // An access at addr has no effect but on memory: a plain MemoryDevice
    // (see DirectPages()) below the MMR space and no hook in its page.
    // Anything else may give the host work between blocks.
    bool Quiet(u32 addr) const;
    // After a guest access of length bytes at addr: whether it may have given
    // the host work between blocks. No below the MMR space where Quiet(), and
    // in the MMR space when the one device it reached says so
    // (Device::HostWorkAfterAccess); yes anywhere else.
    bool HostWorkAfter(u32 addr, u32 length, bool write) const;
    // MemoryRead8/16/32 and MemoryWrite8/16/32 followed by HostWorkAfter,
    // with one device lookup for both where an MMR access stays inside its
    // device: generated code's accesses outside the direct pages.
    template <typename T> T ReadAttended(u32 addr, bool& hostWork);
    template <typename T> void WriteAttended(u32 addr, T value, bool& hostWork);
    void RebuildDirectPages();

    const std::vector<Device*>& Devices() const { return devices; }

    const std::map<u32, std::function<u32(u32)>>& ReadHooks() const { return readHooks; }
    const std::map<u32, std::function<void(u32, u32)>>& WriteHooks() const { return writeHooks; }

protected:
    // get_device() for addr, from a small per-thread cache of the addresses
    // looked up last (the same few MMRs, mostly). Exact: an entry holds only
    // for the device map it was looked up in (generation_).
    Device* Find(u32 addr) const;
    uint64_t generation_ = 0;

    std::shared_ptr<FastMem> fastmem;
    std::shared_ptr<std::array<u8*, NUM_PAGE_TABLE_ENTRIES>> pageTable;
    std::map<u32, Device*> deviceSegments;
    std::vector<Device*> devices;
    std::map<u32, std::function<u32(u32)>> readHooks;
    std::map<u32, std::function<void(u32, u32)>> writeHooks;
    std::vector<u8*> directPages;
};