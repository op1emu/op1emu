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

For firmware metadata inspection without GUI, emulator, or submodule
dependencies, configure the headless tools:

```bash
cmake -S . -B build-headless -DOP1EMU_BUILD_GUI=OFF
cmake --build build-headless
./build-headless/op1inspect inspect path/to/firmware.op1
./build-headless/op1inspect inspect-ldr path/to/image.ldr
```

See [the firmware format reference](docs/firmware-format.md) and [the LDR
format reference](docs/ldr-format.md) for inspection and validation details.

## Acknowledgements
- [bfin_sim](https://github.com/op1emu/bfin_sim) - Blackfin simulator used for CPU emulation, from gdb/sim.
- [op1kenobi](https://github.com/alexmandelshtam/op1kenobi) - OP-1 screenshot assets used for the GUI background.
