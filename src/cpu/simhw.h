#pragma once

#include "io.h"

class SimHWDevice : public Device {
public:
    SimHWDevice(const char* name, u32 base, u32 size, void* system);
    ~SimHWDevice() override;

    void Read(u32 offset, void* buffer, u32 length) override;
    void Write(u32 offset, const void* buffer, u32 length) override;

    u32 Read32(u32 offset) override;
    void Write32(u32 offset, u32 value) override;

protected:
    void* simhw;
};

class SimCECDevice : public SimHWDevice {
public:
    SimCECDevice(u32 base, void* system);
    void RaiseInterrupt(int ivg, int level);
};

class SimEVTDevice : public SimHWDevice {
public:
    SimEVTDevice(u32 base, void* system);
};

class SimMMUDevice : public SimHWDevice {
public:
    SimMMUDevice(u32 base, void* system);
};