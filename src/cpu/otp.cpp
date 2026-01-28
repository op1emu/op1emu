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
    TransferWithMask(&mem[OTP_FPS03 * PAGE_SIZE_WORDS], (const u32*)partStr);
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
}
