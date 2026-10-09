# OP-1 Emulator

An emulator for the Teenage Engineering OP-1 synthesizer.
Still work in progress.

Test with op1_246.op1/te-boot.ldr

![OP-1 Emulator Screenshot](screenshot.png)

## Building

```bash
git clone https://github.com/op1emu/op1emu.git
cd op1emu
git submodule update --init --recursive

mkdir build
cmake -B build -GNinja
cmake --build build
./build/op1emu path/to/nandflash.img path/to/te-boot.ldr
```

## Headless operation

Run from the repository root with the matching `otp.bin`:

```bash
./build/op1emu nand.bin --nand-snapshot --headless
```

No GLFW initialization, window, OpenGL rendering, or host audio device is
started. The existing executable still links GLFW/OpenGL and needs their build
dependencies. PPI/DMA, SPORT sample timing, frame-sync callbacks, USB/IP, and CPU
execution continue. The main loop retains its 16 ms sleep, random accelerometer
feed, and midpoint volume unless a script sets them (see below). Nothing is
drawn and host audio output is discarded, but the last complete frame is kept
so scripts can wait for and capture it.

Enter one command per line on stdin (press Enter in a terminal):

```text
keys
tap synth
press shift
tap play 200
release shift
quit
```

`keys` lists the button names from `gui/ui.json`, including `play`, `stop`,
`shift`, `c4`, `f3#`, and encoder **push** buttons `enc1` through `enc4`.
`press KEY` holds a key until `release KEY`; use these for chords.
`tap KEY [MS]` holds and releases it (default 100 ms).
`wait MS` delays subsequent commands while emulation and frame sync continue.
Durations are integer wall-clock milliseconds, from 1 to 86400000, serviced at
the host loop cadence. They are not deterministic guest-cycle timestamps;
short presses can be missed while firmware is busy compiling or booting.
Explicit press/release pairs need a wait between them to be observable by firmware.

Instead of guessing how long the firmware needs, wait for the screen and look at it:

- `wait-frames N [TIMEOUT_MS]` holds later commands until N *new* complete frames
  have been drawn (a frame starts at pixel (0,0) and must cover the whole panel).
  The default timeout is 600000 ms; on timeout the emulator reports an error and
  exits nonzero rather than hanging. During boot the firmware shows the logo and
  then draws nothing for a long time (about two minutes on a desktop CPU, mostly
  JIT compilation), so give the first wait a generous timeout.
- `screenshot PATH` writes the latest complete frame as a binary PPM (P6), rotated
  to the landscape orientation the GUI shows (400x200). It is an error if no
  complete frame exists yet. Paths cannot contain spaces. Convert with e.g.
  `convert shot.ppm shot.png`.

A frame appearing does not mean the firmware is ready for input. A 100 ms tap sent
the moment the main screen first appeared was ignored; after a further
`wait 5000`, `tap synth 500`, `tap drum 500` and `tap play 500` each switched the
screen. Compare screenshots taken before and after a key to see whether it was handled.

Sensors are fed by the host loop, not the firmware. By default acceleration is
random on every 16 ms tick, which makes the interrupt load (and so runs) vary
from run to run. A script can pin them:

- `accel X Y Z` fixes the accelerometer (int16 each) and sends it once;
  `accel random` goes back to the random feed.
- `volume N` (0..255, default 128) sets the volume knob.

For a repeatable run put `accel 64 -64 512` on the first line of an input script;
it is applied before the first sample is sent. Commands typed on stdin later cannot
undo samples already delivered.

For replay, save the following as `keys.txt` and pass
`--headless --input-script keys.txt`:

```text
accel 64 -64 512
# Wait until the firmware has drawn the logo, then until the main screen is up
# (the frame counts are a heuristic; the firmware needs a few more seconds).
wait-frames 1 300000
wait-frames 20 900000
wait 5000
screenshot before.ppm
tap synth 500
wait 3000
screenshot after.ppm
quit
```

Scripts and stdin use the same commands. Blank lines and lines beginning with
`#` are ignored. EOF leaves the emulator running, including any pending tap
release; `quit`, SIGINT, or SIGTERM requests clean shutdown. Invalid commands
report an error and exit nonzero, as does a JIT/execution failure in the CPU
thread (previously the emulator would retry forever). Log lines say when input was **queued**, not
when firmware handled it. NAND writes are persistent unless `--nand-snapshot` is
selected. When comparing timings, use the same frontend mode for every run:
removing GL swaps and host audio changes host overhead and frame cadence.

## Deterministic runs

By default the guest sees host time: CYCLES, the core timer, the GP timers, SPORT
sample pacing and the RTC all follow the host's clock. A slower or busier host
therefore makes the guest see more time pass per instruction (more timer
interrupts, more polling), and no two boots do the same work. That is fine for
playing the instrument and useless for measuring the emulator.

`--deterministic` takes guest time from what the core executed instead: one
cycle per instruction packet at 400 MHz (an issue model with no pipeline, cache
or PLL timing). With the same NAND, OTP, input script and RTC epoch, every run
executes the same instructions and draws the same frames, however fast or loaded
the host is.

```bash
./build/op1emu nand.bin --nand-snapshot --headless --deterministic --input-script boot.txt
```

- Execution is not paced to real time: it runs as fast as the host allows, which
  is currently much slower than real time, so animation and audio are slow.
  Use it headless, for measurement and regression runs.
- The core timer and SPORT run at their programmed rates. Host-time mode divides
  both by 10 to make up for an emulator slower than real time.
- Panel frame sync (TE, PORTG3) is a 60 Hz guest-time oscillator instead of the
  frontend's 16 ms host poll.
- The RTC starts at 2024-01-01 00:00 UTC; `--rtc-epoch SECONDS` sets another start
  (only with `--deterministic`).
- A script's leading `accel`/`volume` commands are applied before the CPU starts.
  The random accelerometer feed sends one sample then (and one per later script
  change) instead of one every 16 ms, so it no longer depends on host speed.
  Key presses, `wait` and `tap` are still host-timed, so a run that presses keys
  is not repeatable.

Frontend regressions (including the CPU input event queue) and the time model
tests can be built with:

```bash
cmake -S . -B build -DOP1_BUILD_HEADLESS_TESTS=ON
cmake --build build --target op1-headless-test op1-time-source-test
ctest --test-dir build -R "headless-test|time-source-test" --output-on-failure
```

## Acknowledgements
- [bfin_sim](https://github.com/op1emu/bfin_sim) - Blackfin simulator used for CPU emulation, from gdb/sim.
- [op1kenobi](https://github.com/alexmandelshtam/op1kenobi) - OP-1 screenshot assets used for the GUI background.
