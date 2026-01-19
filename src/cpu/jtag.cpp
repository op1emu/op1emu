#include "jtag.h"

Jtag::Jtag(u32 baseAddr, u32 dspidValue) : RegisterDevice("jtag", baseAddr, 0x0C), dspid(dspidValue) {
    REG32(DSPID, 0x0);
    FIELD(DSPID, dspid, 0, 32, R(dspid), N());
}