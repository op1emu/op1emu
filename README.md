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

## In the browser

https://op1emu.github.io/op1emu/ runs the same emulator in Chrome or Edge
(133 or later), compiled to WebAssembly, on your OP-1's NAND image and
`otp.bin`, which stay in the browser. A boot takes about a minute. See
[web/README.md](web/README.md) for how it works and how to build it.

## NAND provisioning (experimental)

To create an op1emu NAND image containing the original OP-1 factory content,
see [NAND provisioning](docs/nand-provisioning.md). This can use an official
`.op1` update package or an unpacked `content/` directory and requires an
external `mkyaffs2` utility. Provisioning content does not decrypt firmware.

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
`wait MS` delays subsequent commands while emulation continues.
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

## Guest time

Guest time comes from what the core executed: one cycle per instruction packet
at 400 MHz (an issue model with no pipeline, cache or PLL timing), plus the
cycles an IDLE waits. CYCLES, the core timer, the GP timers, SPORT sample pacing,
the panel's frame sync and the RTC all follow it.

- The core timer runs at its programmed rate.
- SPORT moves frames at the codec's 44.1 kHz, the codec being the link's clock
  master. A frame is a word per channel in use: on the OP-1 a primary and a
  secondary word, of which the primary is the audio.
- Panel frame sync (TE, PORTG3) is a 60 Hz guest-time oscillator.
- An IDLE waits for its wakeup: guest time goes on to the host's next device or
  event time without executing anything (CYCLES counts through it, as on
  hardware), unless a system interrupt SIC_IWR enables is already asserted.
  `--no-idle-wait` runs it as a NOP instead.
- A script's leading `accel`/`volume` commands are applied before the CPU starts.
  After `at MS`, key presses are for guest time MS; those before the script's
  first wait are queued before the CPU starts.

By default the emulator keeps guest time from running ahead of the host's clock,
so animation and audio go at the device's pace: when the guest gets more than
1 ms ahead, the CPU thread sleeps until the clock catches up. A key or USB
traffic arriving meanwhile ends the sleep, so the guest sees it about when it
came. A guest that falls behind (while the JIT compiles, or on a busy host)
runs flat out to catch up, by at most 100 ms: the rest of a longer stall is let
off rather than rushed through. The RTC starts at the host's calendar time when
the emulator starts, so it falls behind by the setup before the CPU runs and by
what was let off; `--rtc-epoch SECONDS` sets another start.

### Deterministic runs

Keys and USB arrive on host time, and the random accelerometer feed sends a
sample on every 16 ms host poll, so no two default runs do the same work.
`--deterministic` runs as fast as the host allows instead, not paced to its
clock. With the same NAND, OTP, input script and RTC epoch, every run executes
the same instructions and draws the same frames, however fast or loaded the
host is. Use it, headless, for measurement and regression runs.

```bash
./build/op1emu nand.bin --nand-snapshot --headless --deterministic --input-script boot.txt
```

- Animation and audio run faster than on the device whenever the host is.
- The RTC starts at 2024-01-01 00:00 UTC unless `--rtc-epoch` says otherwise.
- The random accelerometer feed sends one sample when it starts (and one per
  later script change) instead of one every 16 ms.
- Key presses, `wait` and `tap` are host-timed, so a run that presses keys that
  way is not repeatable; keys after `at MS` are, those before the script's first
  wait queued before the CPU starts (`tools/op1prof/scripts/synth-keys.txt`).
- The JIT's second compilation of a hot block is used a fixed number of packets
  after it was asked for (`--tier-publish-after`), not as soon as it is done.

Frontend regressions (including the CPU input event queue), the time model
tests and an end-to-end smoke run of `op1emu` on a synthetic LDR (no NAND or
firmware needed) can be built with:

```bash
cmake -S . -B build -DOP1_BUILD_HEADLESS_TESTS=ON
cmake --build build --target op1emu ldrdump op1-headless-test op1-time-source-test
ctest --test-dir build -R "headless-test|time-source-test|ldr-smoke" --output-on-failure
```

## USB

The OP-1's USB port is served over USB/IP, on TCP port 3240. On a Linux host,
with the `usbip` tool:

```bash
usbip list -r 127.0.0.1            # 1-1, once the firmware has connected
sudo modprobe vhci-hcd
sudo usbip attach -r 127.0.0.1 -b 1-1
```

The firmware connects to the bus some seconds into its boot (in TE-BOOT, once
"Upload firmware" is chosen). Until then the list is empty and an attach is
refused at once, to be tried again. The device is listed as the firmware
describes it, so as audio and MIDI (2367:0004) normally and as mass storage
(2367:0002) for a firmware upload. One client is attached at a time: another
attach is refused as busy until it detaches, and a list does not disturb it.

## Acknowledgements
- [bfin_sim](https://github.com/op1emu/bfin_sim) - Blackfin simulator used for CPU emulation, from gdb/sim.
- [op1kenobi](https://github.com/alexmandelshtam/op1kenobi) - OP-1 screenshot assets used for the GUI background.
