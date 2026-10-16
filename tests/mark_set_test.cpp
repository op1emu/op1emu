#include "profiling/mark_set.h"
#include <cstdio>
#include <cstdlib>
#include <stdexcept>

using profiling::MarkSet;

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static bool Throws(MarkSet& set, const char* name, uint32_t lo, uint32_t hi, int after) {
    try { set.Add(name, lo, hi, after); } catch (const std::runtime_error&) { return true; }
    return false;
}

int main() {
    {
        // The stock file's first two marks share the bootrom range; the
        // second needs a fresh entry after the first has fired.
        MarkSet set;
        set.Add("bootrom", 0xffa00000, 0xffa00020, -1);
        set.Add("te-boot", 0xffa00000, 0xffa00020, 0);
        Require(set.OnBlock(0x1000) == -1, "outside every range");
        Require(set.OnBlock(0xffa00000) == 0, "entering the range fires the first mark");
        Require(set.OnBlock(0xffa00010) == -1, "staying inside is no new entry");
        Require(set.OnBlock(0x2000) == -1, "leaving fires nothing");
        Require(set.OnBlock(0xffa00004) == 1, "a fresh entry fires the armed mark");
        Require(set.Left() == 0 && set.OnBlock(0xffa00000) == -1, "every mark fires once");
    }
    {
        // An armed mark whose range already held the PC must wait for a new
        // entry, even though its range was entered earlier.
        MarkSet set;
        set.Add("a", 0x100, 0x1ff, -1);
        set.Add("b", 0x180, 0x2ff, 0);
        Require(set.OnBlock(0x190) == 0, "a fires; b is inside but not armed");
        Require(set.OnBlock(0x1a0) == -1, "b is armed now, but this is no new entry");
        Require(set.OnBlock(0x400) == -1 && set.OnBlock(0x200) == 1, "b fires on its next entry");
    }
    {
        // At most one mark per block, the first in file order; the other
        // keeps its pending entry only until the PC leaves its range.
        MarkSet set;
        set.Add("first", 0x100, 0x1ff, -1);
        set.Add("second", 0x100, 0x1ff, -1);
        Require(set.OnBlock(0x100) == 0, "the earlier mark wins");
        Require(set.OnBlock(0x104) == -1, "the later one saw this entry already");
        Require(set.OnBlock(0x500) == -1 && set.OnBlock(0x108) == 1, "and fires on the next entry");
    }
    {
        // The chunk filter folds addresses modulo 4096 chunks of 32 bytes: an
        // alias of a range's chunk must be checked properly, not fire.
        MarkSet set;
        set.Add("high", 0x01ab3000, 0x01ab301f, -1);
        const uint32_t alias = 0x01ab3000 + 4096 * 32;
        Require(set.OnBlock(alias) == -1, "an aliased chunk is not the range");
        Require(set.OnBlock(0x01ab3010) == 0, "the range itself fires");
    }
    {
        // A range wider than the filter sets every chunk and still works.
        MarkSet set;
        set.Add("wide", 0x10000000, 0x1fffffff, -1);
        Require(set.OnBlock(0x0fffffff) == -1 && set.OnBlock(0x10000020) == 0, "wide range");
    }
    {
        // The host runs every block in an unfired range (chaining skips the
        // starts of the others): only those can fire a mark.
        MarkSet set;
        set.Add("a", 0x100, 0x1ff, -1);
        set.Add("b", 0x01ab3000, 0x01ab301f, -1);
        Require(set.InUnfired(0x100) && set.InUnfired(0x1ff) && set.InUnfired(0x01ab3010), "in unfired ranges");
        Require(!set.InUnfired(0x200) && !set.InUnfired(0x01ab3000 + 4096 * 32), "outside, aliased chunk too");
        Require(set.OnBlock(0x180) == 0 && !set.InUnfired(0x180) && set.InUnfired(0x01ab3000), "a fired range is free");
        Require(set.OnBlock(0x01ab3000) == 1 && !set.InUnfired(0x01ab3000), "none left");
    }
    {
        MarkSet set;
        Require(Throws(set, "inverted", 0x200, 0x100, -1), "lo > hi is refused");
        Require(Throws(set, "early", 0x100, 0x200, 0), "after a mark that does not exist yet is refused");
        set.Add("x", 0x100, 0x200, -1);
        Require(set.Find("x") == 0 && set.Find("y") == -1, "find by name");
    }
    std::puts("MarkSet tests passed");
}
