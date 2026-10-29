#include "bootrom.h"
#include <string.h>

#ifdef __EMSCRIPTEN__
#include "utils/log.h"
#include <fstream>
#else
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wnarrowing"
#include "bfin_sim/bf526-0.2.h"
#pragma GCC diagnostic pop
#endif

BootROM::BootROM(u32 baseAddr) : MemoryDevice("Boot ROM", baseAddr, 0x8000) {
}

void BootROM::BindFastMem(const std::shared_ptr<FastMem>& mem) {
    MemoryDevice::BindFastMem(mem);
#ifdef __EMSCRIPTEN__
    // The browser's build carries no ROM: bootrom.bin in the working
    // directory, which the page fetched (web/README.md). Without it the ROM
    // reads as zeros, which only an LDR run (it starts past the ROM) survives.
    std::ifstream file("bootrom.bin", std::ios::binary);
    if (!file.read(reinterpret_cast<char*>(memAddress), 0x8000)) {
        memset(memAddress, 0, 0x8000);
        LogWarn("Boot ROM: no 32 KiB bootrom.bin in the working directory");
    }
#else
    memcpy(memAddress, bfrom_bf526_0_2, sizeof(bfrom_bf526_0_2));
#endif
}
