#pragma once

#include "io.h"
#include <array>
#include <string>
#include <fstream>

class OTP : public RegisterDevice {
public:
    OTP(u32 baseAddr, const std::string& storagePath);
    ~OTP();

protected:
    constexpr static size_t NUM_PAGES = 0x200;
    constexpr static size_t PAGE_SIZE_WORDS = 4; // 128 bits = 4 x 32-bit words
    constexpr static size_t PAGE_SIZE_BYTES = PAGE_SIZE_WORDS * sizeof(u32); // 16 bytes
    constexpr static size_t TOTAL_SIZE_BYTES = NUM_PAGES * PAGE_SIZE_BYTES; // 8KB

    void ReadPage(u16 page);
    void WritePage(u16 page);
    void TransferWithMask(u32* dst, const u32* src);
    void WritePageValue(u16 page, u64 lo, u64 hi);
    u8 CalculateEcc(u64 data);
    void WriteEcc(u16 page, u8 lo, u8 hi);
    void LoadFromFile();
    void SavePageToFile(u16 page);

    // OTP memory storage: 0x200 pages, each 128 bits (4 x 32-bit words)
    std::array<u32, NUM_PAGES * PAGE_SIZE_WORDS> mem{};

    // Storage file
    std::fstream storageFile;

    // Register values
    u16 page = 0;
    bool doRead = false;
    bool doWrite = false;
    bool statusDone = false;
    u16 ben{0xFFFF};
    u32 timing{0x00001485};
    u32 data[4]{0};
};