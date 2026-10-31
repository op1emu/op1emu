// The tier Worker's (web/compile-worker.js): the emulator's promotions,
// compiled in another instance of this module, one at a time (bcore's
// RemoteCompiler, remote_tier.h). The emulator's Worker instantiates the
// object as a module there (Core::deliver).

#include "remote_tier.h"

#include <emscripten.h>

#include <cstdint>

namespace {

bcore::RemoteCompiler* compiler = nullptr;
bcore::RemoteCode code;
// op1_tier_compile's answer: ok, the object's address and size, the
// function's name, the fallthrough PC, why it failed (C strings).
uint32_t answer[6];

uint32_t Address(const void* p) { return uint32_t(reinterpret_cast<uintptr_t>(p)); }

}  // namespace

extern "C" {

// The job of size bytes at job compiled; the answer (above) holds until the
// next call.
EMSCRIPTEN_KEEPALIVE const uint32_t* op1_tier_compile(const uint8_t* job, uint32_t size) {
    if (!compiler) compiler = new bcore::RemoteCompiler;
    const bool ok = compiler->compile(job, size, code);
    answer[0] = ok;
    answer[1] = Address(code.object.data());
    answer[2] = uint32_t(code.object.size());
    answer[3] = Address(code.name.c_str());
    answer[4] = code.fallthrough_pc;
    answer[5] = Address(code.why.c_str());
    return answer;
}

}  // extern "C"
