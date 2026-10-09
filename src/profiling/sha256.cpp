#include "sha256.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {
constexpr uint32_t kRound[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
};
inline uint32_t Rotr(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }
} // namespace

Sha256::Sha256()
    : state_{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
             0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19} {}

void Sha256::Block(const uint8_t* block) {
    uint32_t w[64];
    for (int i = 0; i < 16; ++i)
        w[i] = uint32_t(block[i * 4]) << 24 | uint32_t(block[i * 4 + 1]) << 16 |
               uint32_t(block[i * 4 + 2]) << 8 | block[i * 4 + 3];
    for (int i = 16; i < 64; ++i) {
        const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
        const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    uint32_t a = state_[0], b = state_[1], c = state_[2], d = state_[3];
    uint32_t e = state_[4], f = state_[5], g = state_[6], h = state_[7];
    for (int i = 0; i < 64; ++i) {
        const uint32_t t1 = h + (Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25)) + ((e & f) ^ (~e & g)) + kRound[i] + w[i];
        const uint32_t t2 = (Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
        h = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    state_[0] += a; state_[1] += b; state_[2] += c; state_[3] += d;
    state_[4] += e; state_[5] += f; state_[6] += g; state_[7] += h;
}

void Sha256::Update(const void* data, size_t length) {
    auto* bytes = static_cast<const uint8_t*>(data);
    length_ += length;
    if (buffered_) {
        const size_t take = std::min(length, sizeof(buffer_) - buffered_);
        std::memcpy(buffer_ + buffered_, bytes, take);
        buffered_ += take; bytes += take; length -= take;
        if (buffered_ < sizeof(buffer_)) return;
        Block(buffer_);
        buffered_ = 0;
    }
    for (; length >= 64; bytes += 64, length -= 64) Block(bytes);
    std::memcpy(buffer_, bytes, length);
    buffered_ = length;
}

std::array<uint8_t, 32> Sha256::Final() {
    const uint64_t bits = length_ * 8;
    const uint8_t pad = 0x80;
    Update(&pad, 1);
    const uint8_t zero = 0;
    while (buffered_ != 56) Update(&zero, 1);
    uint8_t tail[8];
    for (int i = 0; i < 8; ++i) tail[i] = uint8_t(bits >> (56 - 8 * i));
    Update(tail, 8);
    std::array<uint8_t, 32> digest{};
    for (int i = 0; i < 8; ++i)
        for (int j = 0; j < 4; ++j) digest[i * 4 + j] = uint8_t(state_[i] >> (24 - 8 * j));
    return digest;
}

std::string Sha256::Hex(const std::array<uint8_t, 32>& digest) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    for (uint8_t byte : digest) { out += kDigits[byte >> 4]; out += kDigits[byte & 15]; }
    return out;
}

std::string Sha256::Of(const void* data, size_t length) {
    Sha256 hash;
    hash.Update(data, length);
    return Hex(hash.Final());
}

std::string Sha256::OfFile(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) return {};
    Sha256 hash;
    std::vector<uint8_t> chunk(1 << 20);
    size_t got;
    while ((got = std::fread(chunk.data(), 1, chunk.size(), file)) > 0) hash.Update(chunk.data(), got);
    const bool ok = !std::ferror(file);
    std::fclose(file);
    return ok ? Hex(hash.Final()) : std::string();
}
