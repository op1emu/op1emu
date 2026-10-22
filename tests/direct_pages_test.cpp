// Emulator::DirectPages(): which guest pages generated code may access
// directly. Synthetic devices; no firmware or device image required.
#include "cpu/bootrom.h"
#include "cpu/emu.h"
#include <iostream>
#include <stdexcept>

static void check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

static u8* Page(Emulator& emulator, u32 addr) { return emulator.DirectPages()[addr >> PAGE_BITS]; }

int main() {
    try {
        Emulator emulator;
        u8* const* table = emulator.DirectPages();

        MemoryDevice ram("RAM", 0x10000, 0x3000);
        emulator.BindDevice(&ram);
        for (u32 page = 0x10000; page < 0x13000; page += 0x1000)
            check(Page(emulator, page) == static_cast<u8*>(ram.Map(page - 0x10000)), "whole RAM pages are direct");
        check(!Page(emulator, 0x13000), "past the device");

        MemoryDevice partial("partial", 0x20000, 0x800);  // shares its page with unmapped space
        emulator.BindDevice(&partial);
        check(!Page(emulator, 0x20000), "a device smaller than a page");

        BootROM rom(0x30000);  // a MemoryDevice subclass
        emulator.BindDevice(&rom);
        check(!Page(emulator, 0x30000), "subclasses keep their accesses");

        // A device nested inside RAM owns part of the page around it. In the
        // RAM's first page, with no other device after it, both the page's
        // first and its last byte still find the RAM.
        MemoryDevice outer("outer RAM", 0x40000, 0x3000);
        MemoryDevice nested("nested", 0x40800, 0x100);
        emulator.BindDevice(&outer);
        emulator.BindDevice(&nested);
        check(!Page(emulator, 0x40000), "a device nested in a page takes it out");
        check(Page(emulator, 0x41000) == static_cast<u8*>(outer.Map(0x1000)) &&
              Page(emulator, 0x42000) == static_cast<u8*>(outer.Map(0x2000)), "and only its page");

        MemoryDevice mmr("MMR RAM", 0xFFE00000, 0x2000);
        emulator.BindDevice(&mmr);
        check(!Page(emulator, 0xFFE00000), "nothing at or above 0xFFC00000");

        emulator.AddReadHook(0x11004, [](u32) { return 0u; });
        check(!Page(emulator, 0x11000), "a read hook takes its page out");
        check(Page(emulator, 0x10000) && Page(emulator, 0x12000), "and only its page");
        emulator.AddWriteHook(0x12ffc, [](u32, u32) {});
        check(!Page(emulator, 0x12000), "so does a write hook");

        // A watch (WatchPages) takes its pages out while it lasts, sees
        // the host's accesses to them first and makes them not quiet; a
        // rebuild in between keeps them out.
        struct Counter : PageWatcher {
            int seen = 0;
            void BeforeAccess() override { ++seen; }
            bool HostWorkAfterAccess() const override { return false; }
        } watcher;
        MemoryDevice watched("watched RAM", 0x50000, 0x3000);
        emulator.BindDevice(&watched);
        emulator.WatchPages(&watcher, {0x51});
        check(!Page(emulator, 0x51000) && Page(emulator, 0x50000) && Page(emulator, 0x52000), "a watch takes its page out");
        check(!emulator.Quiet(0x51000) && emulator.Quiet(0x50000), "a watched page is not quiet");
        check(!emulator.HostWorkAfter(0x51000, 4, false), "the watcher says whether an access gives the host work");
        emulator.MemoryRead32(0x50ffc);
        check(watcher.seen == 0, "an access elsewhere is not seen");
        emulator.MemoryRead32(0x51000);
        u8 bytes[8];
        emulator.MemoryRead(0x50ffc, bytes, sizeof(bytes));  // across into the watched page
        emulator.MemoryWrite8(0x51fff, 1);
        check(watcher.seen == 3, "the host's accesses to it are seen first");
        emulator.AddReadHook(0x50004, [](u32) { return 0u; });
        check(!Page(emulator, 0x51000) && !Page(emulator, 0x50000) && Page(emulator, 0x52000), "a rebuild keeps the watch");
        emulator.WatchPages(&watcher, {});
        check(Page(emulator, 0x51000) == static_cast<u8*>(watched.Map(0x1000)) && !Page(emulator, 0x50000),
              "the watch's end gives its page back");
        emulator.MemoryRead32(0x51000);
        check(watcher.seen == 3 && emulator.Quiet(0x51000), "and nothing is seen any more");

        check(emulator.DirectPages() == table, "the table never moves");
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << "\n";
        return 1;
    }
    std::cout << "PASS: direct pages\n";
    return 0;
}
