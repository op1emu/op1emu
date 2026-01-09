#pragma once

#include <cstdint>
#include <cstddef>
#include <endian.h>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;

template <typename T>
T swap(const T& arg)
{
#if BYTE_ORDER == BIG_ENDIAN
    return arg;             // no byte-swapping needed
#else                       // swap bytes
    T ret;
    char* dst = reinterpret_cast<char*>(&ret);
    const char* src = reinterpret_cast<const char*>(&arg + 1);
    for (auto i = 0; i < sizeof(T); i++)
        *dst++ = *--src;
    return ret;
#endif
}

template <typename T>
class __attribute__ ((__packed__)) BigEndian
{
    T value;

public:
    BigEndian() { }
    BigEndian(const T& t) : value(swap(t)) { }
    operator T() const { return swap(value); }
};

using be_uint16_t = BigEndian<uint16_t>;
using be_uint32_t = BigEndian<uint32_t>;

constexpr std::size_t PAGE_BITS = 12;
constexpr std::size_t NUM_PAGE_TABLE_ENTRIES = 1 << (32 - PAGE_BITS);