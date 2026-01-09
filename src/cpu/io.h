#pragma once

#include "common.h"
#include <vector>
#include <map>
#include <functional>
#include <memory>
#include <string>
#include "fastmem.h"

class Device {
public:
    using InterruptHandler = std::function<void(int, int)>;

    Device(const std::string& name, u32 baseAddr, u32 size) : name(name), baseAddress(baseAddr), size(size) {}
    virtual ~Device() {}

    virtual void BindFastMem(const std::shared_ptr<FastMem>&) {}
    void BindInterrupt(int q, InterruptHandler callback) { interruptHandlers[q] = callback; }

    virtual void Read(u32 offset, void* buffer, u32 length) = 0;
    virtual void Write(u32 offset, const void* buffer, u32 length) = 0;

    virtual u32 Read32(u32 offset) = 0;
    virtual void Write32(u32 offset, u32 value) = 0;

    virtual void* Map(u32 offset) { return nullptr; }

    virtual bool UpdatePageTable(std::array<u8*, NUM_PAGE_TABLE_ENTRIES>& table) { return false; }

    const std::string& Name() const { return name; }
    u32 BaseAddress() const { return baseAddress; }
    u32 EndAddress() const { return baseAddress + size - 1; }
    u32 Size() const { return size; }

    void TriggerInterrupt(int q, int level) const {
        auto iter = interruptHandlers.find(q);
        if (iter != interruptHandlers.end()) {
            iter->second(q, level);
        }
    }
    void TriggerInterrupt(int level) const { if (interruptHandlers.size()) interruptHandlers.begin()->second(interruptHandlers.begin()->first, level); }

    virtual void ProcessWithInterrupt(int ivg) {}

protected:
    std::string name;
    u32 baseAddress;
    u32 size;
    std::map<int, InterruptHandler> interruptHandlers;
};

class MemoryDevice : public Device {
public:
    MemoryDevice(const std::string& name, u32 baseAddr, u32 size);
    ~MemoryDevice() override;
    void BindFastMem(const std::shared_ptr<FastMem>& fastmem) override;

    void Read(u32 offset, void* buffer, u32 length) override;
    void Write(u32 offset, const void* buffer, u32 length) override;

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

    void* Map(u32 offset) override;

    bool UpdatePageTable(std::array<u8*, NUM_PAGE_TABLE_ENTRIES>& table) override;

protected:
    std::vector<u8> memory;
    std::shared_ptr<FastMem> fastmem;
    u8* memAddress = nullptr;
};


struct Field {
    int offset;
    int length;
    std::function<u32()> readCallback;
    std::function<void(u32)> writeCallback;
};

struct Register {
    u32 addr;
    std::string name;
    std::function<void(u32)> writeCallback;
    std::map<std::string, Field> fields;

    u32 Read32();
    void Write32(u32 value);
};

class RegisterDevice : public Device {
public:
    using Device::Device;

    void Read(u32 offset, void* buffer, u32 length) override;
    void Write(u32 offset, const void* buffer, u32 length) override;

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

protected:
    std::map<u32, Register> registers;
};

#define REG32(r, a) Register &r = registers[a]; r.addr = a; r.name = #r;
#define FIELD(reg, f, o, l, r, w) reg.fields[#f] = { .offset = o, .length = l, .readCallback = r, .writeCallback = w }
#define R(x) [this]() -> u32 { return x; }
#define W(x) [this](u32 v) { x = v; }
#define W1C(x) [this](u32 v) { x = x & (~v); }
#define N() [](u32 v) {}