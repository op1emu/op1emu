#include "bootrom.h"
#include <string.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#include "bfin_sim/bf526-0.2.h"
#pragma GCC diagnostic pop

BootROM::BootROM(u32 baseAddr) : MemoryDevice("Boot ROM", baseAddr, 0x8000) {
}

void BootROM::BindFastMem(const std::shared_ptr<FastMem>& mem) {
    MemoryDevice::BindFastMem(mem);
    memcpy(memAddress, bfrom_bf526_0_2, sizeof(bfrom_bf526_0_2));
}