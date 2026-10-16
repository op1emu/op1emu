#pragma once

// The firing rule of phase marks (docs/profiling.md), apart from the profiler
// so it can be tested on its own: a mark fires the first time a block starts
// inside its PC range after starting outside it, once its `after` mark has
// fired, and at most one mark fires per block, the first in file order.

#include <bitset>
#include <cstdint>
#include <string>
#include <vector>

namespace profiling {

class MarkSet {
public:
    struct Mark {
        std::string name;
        uint32_t lo = 0, hi = 0;
        int after = -1;        // index of the mark that must fire first
        bool inside = false;   // entry-edge state, tracked from the first block
        bool fired = false;
    };

    // Throws std::runtime_error for lo > hi or an `after` that is not an
    // earlier mark (-1 for none). Names are the caller's to check.
    void Add(const std::string& name, uint32_t lo, uint32_t hi, int after);
    int Find(const std::string& name) const;

    const std::vector<Mark>& Marks() const { return marks_; }
    bool Fired(int index) const { return marks_[index].fired; }
    size_t Left() const { return left_; }

    // A block starts at pc: returns the index of the mark that fires, or -1.
    int OnBlock(uint32_t pc);
    // Whether pc is in the range of a mark that has not fired. Only a block
    // that starts in one can fire a mark, and only a block whose start the
    // host sees (OnBlock) can.
    bool InUnfired(uint32_t pc) const;

private:
    void RebuildChunks();

    std::vector<Mark> marks_;
    size_t left_ = 0;
    // Every block passes through OnBlock while a mark is unfired, so most
    // must be rejected cheaply: one bit per 32-byte code chunk (modulo 4096)
    // covered by an unfired mark. A clear bit proves the PC is in no unfired
    // range; a set bit only means "check properly". Scanning the ranges for
    // every block cost 2.8% of execution samples on a stock boot.
    std::bitset<4096> chunks_;
    size_t inside_ = 0;  // unfired marks whose range held the previous PC
};

}  // namespace profiling
