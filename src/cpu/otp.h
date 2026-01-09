#pragma once

#include "io.h"
#include <array>

class OTP : public RegisterDevice {
public:
    OTP(u32 baseAddr);

protected:
    constexpr static size_t NUM_PAGES = 0x200;
    constexpr static size_t PAGE_SIZE_WORDS = 4; // 128 bits = 4 x 32-bit words

    void ReadPage(u16 page);
    void WritePage(u16 page);
    void TransferWithMask(u32* dst, const u32* src);
    void WritePageValue(u16 page, u64 lo, u64 hi);

    // OTP memory storage: 0x200 pages, each 128 bits (4 x 32-bit words)
    std::array<u32, NUM_PAGES * PAGE_SIZE_WORDS> mem{};

    // Register values
    u16 page = 0;
    bool doRead = false;
    bool doWrite = false;
    bool statusDone = false;
    u16 ben{0xFFFF};
    u32 timing{0x00001485};
    u32 data[4]{0};
};