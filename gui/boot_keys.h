#pragma once

#include "cpu/cpu.h"
#include "peripheral/keyboard.h"

namespace BootKeys {

// The OP-1's COM / USB key (gui/ui.json: usb).
inline constexpr int kComBank = 5, kComPin = 12;
// Tested with TE-BOOT v2.30. This is a conservative startup hold, not a
// measured scan deadline; guest time keeps JIT compilation out of it.
inline constexpr uint64_t kComHoldNs = 5000000000ULL;

// A separate input source, so its release cannot release a manually held
// key. AttachKeyboard's queued events keep the source state alive.
inline void HoldComForBoot(BlackFinCpu& cpu) {
    auto keys = std::make_shared<Keyboard>();
    cpu.AttachKeyboard(keys);
    keys->OnKeyPressed(kComBank, kComPin);
    keys->OnKeyAt(kComBank, kComPin, false, kComHoldNs);
}

}  // namespace BootKeys
