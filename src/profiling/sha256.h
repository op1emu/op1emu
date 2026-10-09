#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

// FIPS 180-4 SHA-256, for binding a profiling run to its firmware image and
// for recognizing a reference frame. Not a security primitive here.
class Sha256 {
public:
    Sha256();
    void Update(const void* data, size_t length);
    std::array<uint8_t, 32> Final();
    static std::string Hex(const std::array<uint8_t, 32>& digest);
    static std::string Of(const void* data, size_t length);
    // Empty string if the file cannot be read.
    static std::string OfFile(const std::string& path);

private:
    void Block(const uint8_t* block);
    uint32_t state_[8];
    uint8_t buffer_[64];
    size_t buffered_ = 0;
    uint64_t length_ = 0;
};
