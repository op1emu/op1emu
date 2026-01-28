#include "otp.h"
#include <cstring>

static constexpr u32 OTP_FPS00 = 0x004;
static constexpr u32 OTP_FPS03 = 0x007;

OTP::OTP(u32 baseAddr)
    : RegisterDevice("OTP", baseAddr, 0xA0),
      ben(0xFFFF), timing(0x00001485) {

    REG32(OTP_CONTROL, 0x00);
    FIELD(OTP_CONTROL, PAGE, 0, 9, R(page), W(page));
    FIELD(OTP_CONTROL, DO_WRITE, 15, 1, R(doWrite ? 1 : 0), W(doWrite));
    FIELD(OTP_CONTROL, DO_READ, 14, 1, R(doRead ? 1 : 0), W(doRead));
    OTP_CONTROL.writeCallback = [this](u32 value) {
        if (doWrite) {
            WritePage(page);
        }
        if (doRead) {
            ReadPage(page);
        }
        statusDone = true;
    };

    REG32(OTP_BEN, 0x04);
    FIELD(OTP_BEN, BEN, 0, 16, R(ben), W(ben));

    REG32(OTP_STATUS, 0x08);
    FIELD(OTP_STATUS, DONE, 0, 1, R(statusDone ? 1 : 0), W1C(statusDone));
    FIELD(OTP_STATUS, RESERVED, 1, 15, R(0), N());

    REG32(OTP_TIMING, 0x0C);
    FIELD(OTP_TIMING, TIMING, 0, 32, R(timing), W(timing));

    // OTP_DATA0-3 registers (32-bit each)
    for (u32 i = 0; i < 4; i++) {
        REG32(OTP_DATA, 0x80 + i * 4);
        auto r = [this, i]() -> u32 {
            return data[i];
        };
        auto w = [this, i](u32 v) {
            data[i] = v;
        };
        FIELD(OTP_DATA, DATA, 0, 32, r, w);
    }

    // Initialize OTP memory with chip-specific data
    // Semi-random value for unique chip id (using address as seed)
    WritePageValue(OTP_FPS00, (u64)(uintptr_t)this, ~(u64)(uintptr_t)this);
    // Part string and FPS03 value
    char partStr[16] = "ADSP-BF524";
    u16 fps03 = 0x420C; // BF524
    partStr[14] = (fps03 >> 0) & 0xFF;
    partStr[15] = (fps03 >> 8) & 0xFF;

    u64 lo = *(u64*)&partStr[0];
    u64 hi = *(u64*)&partStr[8];
    WritePageValue(OTP_FPS03, lo, hi);
}

void OTP::TransferWithMask(u32* dst, const u32* src)
{
    // Transfer bytes based on ben (byte enable) mask
    u8* dstBytes = reinterpret_cast<u8*>(dst);
    const u8* srcBytes = reinterpret_cast<const u8*>(src);

    for (int i = 0; i < 16; ++i) {
        if (ben & (1 << i)) {
            dstBytes[i] = srcBytes[i];
        }
    }
}

void OTP::ReadPage(u16 page)
{
    if (page >= NUM_PAGES) return;

    TransferWithMask(data, &mem[page * PAGE_SIZE_WORDS]);
}

void OTP::WritePage(u16 page)
{
    if (page >= NUM_PAGES) return;

    TransferWithMask(&mem[page * PAGE_SIZE_WORDS], data);
}

void OTP::WritePageValue(u16 page, u64 lo, u64 hi)
{
    if (page >= NUM_PAGES) return;

    u32 src[4] = {
        static_cast<u32>(lo),
        static_cast<u32>(lo >> 32),
        static_cast<u32>(hi),
        static_cast<u32>(hi >> 32)
    };
    TransferWithMask(&mem[page * PAGE_SIZE_WORDS], src);

    u64* ptr = (u64*)&mem[page * PAGE_SIZE_WORDS];
    lo = ptr[0];
    hi = ptr[1];
    u8 loEcc = CalculateEcc(lo);
    u8 hiEcc = CalculateEcc(hi);
    WriteEcc(page, loEcc, hiEcc);
}

u8 OTP::CalculateEcc(u64 data)
{
    u8 p1 = 0, p2 = 0, p4 = 0, p8 = 0, p16 = 0, p32 = 0, p64 = 0;
    u8 overall = 0;
    int dataIdx = 0;
    for (int pos = 1; pos <= 72 && dataIdx < 64; pos++) {
        // Skip parity positions (powers of 2)
        if ((pos & (pos - 1)) == 0)  // pos is a power of 2
            continue;
        // Get the data bit at this logical position
        int bit = (data >> dataIdx) & 1;
        dataIdx++;
        // XOR into appropriate parity accumulators
        if (pos & 1)   p1 ^= bit;
        if (pos & 2)   p2 ^= bit;
        if (pos & 4)   p4 ^= bit;
        if (pos & 8)   p8 ^= bit;
        if (pos & 16)  p16 ^= bit;
        if (pos & 32)  p32 ^= bit;
        if (pos & 64)  p64 ^= bit;
        // Overall parity includes all data bits
        overall ^= bit;
    }
    // Overall parity also includes all parity bits
    overall ^= p1 ^ p2 ^ p4 ^ p8 ^ p16 ^ p32 ^ p64;
    // Pack ECC: [P0, P64, P32, P16, P8, P4, P2, P1]
    u8 ecc = (overall << 7) | (p64 << 6) | (p32 << 5) | (p16 << 4) |
             (p8 << 3) | (p4 << 2) | (p2 << 1) | p1;
    return ecc;
}

void OTP::WriteEcc(u16 page, u8 lo, u8 hi)
{
    if (page >= NUM_PAGES) return;

    u16 eccPage = 0;
    if (page < NUM_PAGES / 2) {
        eccPage = 0xE0 + page / 8;
    } else {
        eccPage = 0x1E0 + (page - NUM_PAGES / 2) / 8;
    }

    int offset = page % 8;
    u16* ptr = reinterpret_cast<u16*>(&mem[eccPage * PAGE_SIZE_WORDS]);
    ptr[offset] = lo | (hi << 8);
}