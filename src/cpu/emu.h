#pragma once

#include <map>
#include <mutex>
#include <functional>
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
    u64 MemoryRead64(u32 vaddr);

    void MemoryWrite8(u32 vaddr, u8 value);
    void MemoryWrite16(u32 vaddr, u16 value);
    void MemoryWrite32(u32 vaddr, u32 value);
    void MemoryWrite64(u32 vaddr, u64 value);

    bool MemoryWriteExclusive32(u32 vaddr, u32 value, u32 expected);

    void* MemoryMap(u32 addr);

    std::array<u8*, NUM_PAGE_TABLE_ENTRIES>& PageTable() { return *pageTable; }
    FastMem& GetFastMem() { return *fastmem; }

    void AddReadHook(u32 addr, std::function<u32(u32)> hook) { readHooks[addr] = hook; }
    void AddWriteHook(u32 addr, std::function<void(u32, u32)> hook) { writeHooks[addr] = hook; }

    void PatchSoftwareBreak(u32 addr, u8 num);
    void RemoveSoftwareBreak(u32 addr);

    bool IsMemoryValid(u32 addr);

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
};