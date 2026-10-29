// The computer keyboard's keys for the OP-1's (gui/ui.json's button names),
// by physical key (KeyboardEvent.code): the musical keys as op1kenobi lays
// them out (https://github.com/alexmandelshtam/op1kenobi, MIT; THIRD_PARTY.md),
// white keys on Q..[ and Z..C, the black ones above and between them; and
// shift as gui/ui.json's keycaps has it.

export const KEYMAP = {
  KeyQ: "f3", KeyW: "g3", KeyE: "a3", KeyR: "b3", KeyT: "c4", KeyY: "d4", KeyU: "e4",
  KeyI: "f4", KeyO: "g4", KeyP: "a4", BracketLeft: "b4", KeyZ: "c5", KeyX: "d5", KeyC: "e5",
  Digit2: "f3#", Digit3: "g3#", Digit4: "a3#", Digit6: "c4#", Digit7: "d4#",
  Digit9: "f4#", Digit0: "g4#", Minus: "a4#", KeyS: "c5#", KeyD: "d5#",
  ShiftLeft: "shift", ShiftRight: "shift",
};
