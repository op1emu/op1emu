// Manual real-firmware check (not registered with CTest: needs a NAND image):
//   build/op1-real-usb-storage-test nand.bin                 # boot ROM, COM held for TE-BOOT
//   build/op1-real-usb-storage-test nand.bin te-boot.ldr     # an LDR, no COM hold
//   OP1_TEST_COM=1 build/op1-real-usb-storage-test nand.bin te-boot.ldr   # LDR with COM held
// Both paths press "1" (Upload firmware) at guest 15 s, then talk to the
// firmware's mass storage through the same host the browser uses.
#include "boot_keys.h"
#include "cpu/cpu.h"
#include "headless.h"
#include "loader/ldr.h"
#include "peripheral/MT29F4G08.h"
#include "storage/fat_storage.h"

#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {
void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::fprintf(stderr, "usage: op1-real-usb-storage-test NAND [te-boot.ldr]\n");
        return 2;
    }
    BlackFinCpu cpu(false);
    const bool ldrMode = argc == 3;
    auto frontend = std::make_shared<HeadlessFrontend>("gui/ui.json", 0);
    cpu.AttachDisplay(frontend);
    cpu.AttachKeyboard(frontend);
    if (!ldrMode || std::getenv("OP1_TEST_COM")) BootKeys::HoldComForBoot(cpu);
    cpu.SetBootMode(0x0D);
    cpu.AttachNandFlash(std::make_shared<MT29F4G08>(cpu, argv[1], true));
    cpu.SetAcceleration(270, -270, 450);
    cpu.SetPotentiometerValue(0xFF - 128);
    LDRParser ldr;
    size_t dxe = 0;
    auto loadDxe = [&] {
        if (dxe >= ldr.getDXEs().size()) return false;
        for (const auto& block : ldr.getDXEs()[dxe].blocks) {
            if (!block.data.empty())
                cpu.GetEmulator().MemoryWrite(block.header.target_address, block.data.data(), block.data.size());
            if (block.IsFirstBlock()) {
                cpu.SetRegister(RegIndex::RETS, 0x8000000);
                cpu.SetPC(block.header.target_address);
            }
        }
        return true;
    };
    if (ldrMode) {
        Require(ldr.loadFile(argv[2]), "load te-boot LDR");
        Require(loadDxe(), "first te-boot DXE");
    } else {
        cpu.SetPC(0xEF000000);
    }
    auto run = [&] {
        cpu.Run();
        if (ldrMode && cpu.PC() == 0x8000000) {
            ++dxe;
            Require(loadDxe(), "last te-boot DXE returned early");
        }
    };

    // Match tools/shot_after_key1.sh: choose Upload firmware at guest 15 s.
    bool pressed = false, released = false;
    const uint64_t until = TimeSource::CyclesAt(30000000000ULL);
    while (cpu.Time().Cycles() < until && !cpu.Failed()) {
        if (!pressed && cpu.Time().Cycles() >= TimeSource::CyclesAt(15000000000ULL)) {
            frontend->OnKeyPressed(3, 6);  // t1 in gui/ui.json
            pressed = true;
        }
        if (pressed && !released && cpu.Time().Cycles() >= TimeSource::CyclesAt(15500000000ULL)) {
            frontend->OnKeyReleased(3, 6);
            released = true;
        }
        run();
    }
    Require(!cpu.Failed(), "guest execution before USB enumeration");
    Require(cpu.GetUSB().IsConnected(), "te-boot did not enter Upload firmware");

    op1::storage::UsbHost host(cpu.GetUSB(), [&] { run(); return !cpu.Failed(); });
    op1::storage::FatStorage storage(host);
    Require(storage.Connect(), storage.Error().c_str());
    std::vector<op1::storage::StorageEntry> entries;
    Require(storage.List("/", entries), storage.Error().c_str());
    std::printf("te-boot FAT root: %zu entries\n", entries.size());
    for (const auto& entry : entries)
        std::printf("  %s %s %u\n", entry.directory ? "dir " : "file", entry.name.c_str(), entry.size);

    const std::string text = "op1emu native in-process USB storage test\n";
    Require(storage.BeginWrite("/op1emu-test.txt", true), storage.Error().c_str());
    Require(storage.Write(text.data(), text.size()), storage.Error().c_str());
    Require(storage.CommitWrite(), storage.Error().c_str());
    uint32_t size = 0;
    Require(storage.OpenRead("/op1emu-test.txt", size), storage.Error().c_str());
    std::vector<char> actual(size);
    uint32_t read = 0;
    Require(storage.Read(actual.data(), actual.size(), read) && read == size, storage.Error().c_str());
    Require(storage.CloseRead(), storage.Error().c_str());
    Require(std::string(actual.begin(), actual.end()) == text, "real te-boot upload/download digest");
    storage.Disconnect();
    std::puts("real USB storage: enumeration, FAT list, upload and download digest: ok");
}
