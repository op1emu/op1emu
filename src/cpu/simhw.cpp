#include "simhw.h"

extern "C" {
#include "bfin_sim/sim-main.h"
#include "bfin_sim/devices.h"
extern void create_hw_port_data(struct hw *hw);
extern void delete_hw_port_data(struct hw *hw);
#include "bfin_sim/dv-bfin_cec.h"
extern const struct hw_descriptor dv_bfin_cec_descriptor[];
#include "bfin_sim/dv-bfin_evt.h"
extern const struct hw_descriptor dv_bfin_evt_descriptor[];
#include "bfin_sim/dv-bfin_mmu.h"
extern const struct hw_descriptor dv_bfin_mmu_descriptor[];
}

#include <cstdlib>
#include <cstring>

#define HW ((struct hw*)simhw)

static int hw_to_attach_address(struct hw *bus,
                                const hw_unit *unit_addr,
                                int *attach_space,
                                unsigned_word *attach_addr,
                                struct hw *client) {
    SimHWDevice* dev = (SimHWDevice*)bus->host;
    *attach_addr = dev->BaseAddress();
    return 0;
}

static int hw_to_attach_size(struct hw *bus,
                             const hw_unit *unit_size,
                             unsigned_word *attach_size,
                             struct hw *client) {
    SimHWDevice* dev = (SimHWDevice*)bus->host;
    *attach_size = dev->Size();
    return 0;
}

SimHWDevice::SimHWDevice(const char* name, u32 base, u32 size, void* system)
    : Device(name, base, size)
    , simhw(calloc(1, sizeof(struct hw))) {
    HW->parent_of_hw = HW;
    HW->host = this;
    HW->system_of_hw = (sim_state*)system;
    create_hw_port_data(HW);
    set_hw_unit_address_to_attach_address(HW, hw_to_attach_address);
    set_hw_unit_size_to_attach_size(HW, hw_to_attach_size);
}

SimHWDevice::~SimHWDevice() {
    delete_hw_port_data(HW);
    free(HW);
}

void SimHWDevice::Read(u32 offset, void* buffer, u32 length) {
    hw_io_read_buffer(HW, buffer, 0, baseAddress + offset, length);
}

void SimHWDevice::Write(u32 offset, const void* buffer, u32 length) {
    hw_io_write_buffer(HW, buffer, 0, baseAddress + offset, length);
}

u32 SimHWDevice::Read32(u32 offset) {
    u32 value = 0;
    Read(offset, &value, sizeof(value));
    return value;
}

void SimHWDevice::Write32(u32 offset, u32 value) {
    Write(offset, &value, sizeof(value));
}

SimCECDevice::SimCECDevice(u32 base, void* system)
    : SimHWDevice(dv_bfin_cec_descriptor[0].family, base, BFIN_COREMMR_CEC_SIZE, system) {
    auto* cpu = ((sim_state*)system)->cpu;
    dv_bfin_cec_descriptor[0].to_finish(HW);
    BFIN_CPU_STATE.cec_cache = HW->data_of_hw;
}

void SimCECDevice::RaiseInterrupt(int ivg, int level) {
    get_hw_port_event(HW)(HW, ivg, HW, 0, level);
}

SimEVTDevice::SimEVTDevice(u32 base, void* system)
    : SimHWDevice(dv_bfin_evt_descriptor[0].family, base, BFIN_COREMMR_EVT_SIZE, system) {
    auto* cpu = ((sim_state*)system)->cpu;
    dv_bfin_evt_descriptor[0].to_finish(HW);
    BFIN_CPU_STATE.evt_cache = HW->data_of_hw;
}

SimMMUDevice::SimMMUDevice(u32 base, void* system)
    : SimHWDevice(dv_bfin_mmu_descriptor[0].family, base, BFIN_COREMMR_MMU_SIZE, system) {
    auto* cpu = ((sim_state*)system)->cpu;
    dv_bfin_mmu_descriptor[0].to_finish(HW);
    BFIN_CPU_STATE.mmu_cache = HW->data_of_hw;
}