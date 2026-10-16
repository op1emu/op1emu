#include "mark_set.h"

#include <stdexcept>

namespace profiling {

void MarkSet::Add(const std::string& name, uint32_t lo, uint32_t hi, int after) {
    if (lo > hi) throw std::runtime_error("Mark " + name + " has lo > hi");
    if (after >= static_cast<int>(marks_.size()) || after < -1)
        throw std::runtime_error("Mark " + name + " is after an unknown or later mark");
    Mark mark;
    mark.name = name;
    mark.lo = lo;
    mark.hi = hi;
    mark.after = after;
    marks_.push_back(mark);
    ++left_;
    RebuildChunks();
}

int MarkSet::Find(const std::string& name) const {
    for (size_t i = 0; i < marks_.size(); ++i)
        if (marks_[i].name == name) return static_cast<int>(i);
    return -1;
}

void MarkSet::RebuildChunks() {
    chunks_.reset();
    for (const auto& mark : marks_) {
        if (mark.fired) continue;
        if ((mark.hi >> 5) - (mark.lo >> 5) >= chunks_.size()) { chunks_.set(); return; }
        for (uint32_t chunk = mark.lo >> 5; chunk <= mark.hi >> 5; ++chunk) chunks_.set(chunk % chunks_.size());
    }
}

bool MarkSet::InUnfired(uint32_t pc) const {
    if (!left_ || !chunks_.test((pc >> 5) % chunks_.size())) return false;
    for (const auto& mark : marks_)
        if (!mark.fired && pc >= mark.lo && pc <= mark.hi) return true;
    return false;
}

int MarkSet::OnBlock(uint32_t pc) {
    if (!left_) return -1;
    if (!chunks_.test((pc >> 5) % chunks_.size())) {
        // Outside every unfired range: only the entry-edge state can change.
        if (inside_) {
            for (auto& mark : marks_) mark.inside = false;
            inside_ = 0;
        }
        return -1;
    }
    // Every unfired mark tracks entry edges from the first block, so a mark
    // armed by its `after` mark still needs a fresh entry: "te-boot" shares
    // the bootrom range with "bootrom" and must not fire on the very next
    // block. At most one mark fires per block, in file order.
    int fire = -1;
    inside_ = 0;
    for (size_t i = 0; i < marks_.size(); ++i) {
        auto& mark = marks_[i];
        if (mark.fired) continue;
        const bool in = pc >= mark.lo && pc <= mark.hi;
        const bool edge = in && !mark.inside;
        mark.inside = in;
        inside_ += in;
        if (edge && fire < 0 && (mark.after < 0 || marks_[mark.after].fired)) fire = static_cast<int>(i);
    }
    if (fire < 0) return -1;
    marks_[fire].fired = true;
    --left_;
    RebuildChunks();
    return fire;
}

}  // namespace profiling
