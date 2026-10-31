// Regression for edge-sensitive GPIO DATA readback versus physical input state.
//
// The original OP-1 MAIN 246 configures Port G pin 3 for single falling-edge
// detection, with interrupt B as SIC source 41 (bank 1 bit 9) routed to IVG13.
// A selected handler acknowledges the edge with PORTGIO_CLEAR bit 3.
// The BF52x HRM also requires edge-sensitive GPIO data readback to remain
// latched until explicitly cleared, even after the pin returns high.
//
// Source: op1-research/screen-rendering-animation/gpio-notification-static/
// FINDINGS.md, cross-checked with ADSP-BF52x HRM rev 1.2, chapter 9.
// This is a synthetic register-level contract. It does not identify PG3's
// board-level signal or prove physical scanout/vblank synchronization.
#include "cpu/gpio.h"
#include "cpu/sic.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {
constexpr u32 kPg0 = 1u << 0;
constexpr u32 kPg3 = 1u << 3;
constexpr u32 kPg4 = 1u << 4;
constexpr u32 kSicPortGB = 1u << 9;
constexpr u16 kIvg13 = 1u << 13;

void Check(bool condition, const std::string& message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
        std::exit(1);
    }
}

// DATA, CLEAR, SET and TOGGLE all expose the same data-register readback.
// An edge-sensitive input's value is its pending edge latch, not the pin level.
void CheckEdgeReadback(GPIO& gpio, u32 mask, u32 expected, const std::string& context) {
    for (u32 offset : {0x00u, 0x04u, 0x08u, 0x0Cu}) {
        const u32 actual = gpio.Read32(offset) & mask;
        Check(actual == expected,
              context + ": readback at offset " + std::to_string(offset));
    }
}

void Stock246Pg3FallingEdge() {
    SIC sic(0xFFC00100);
    GPIO portG("PORTG", 0xFFC01500);
    std::vector<u16> raised;
    sic.SetLineRaisedCallback([&](u16 newlyRaised) { raised.push_back(newlyRaised); });
    portG.BindInterruptB(41, [&](int source, int level) {
        sic.SetInterruptLevel(source, level);
    });

    // Firmware's SIC_IAR5 nibble 1 selects assignment value 6 -> IVG13.
    const u32 iar5 = sic.Read32(0x54);
    sic.Write32(0x54, (iar5 & ~0xF0u) | 0x60u);
    sic.Write32(0x4C, kSicPortGB); // SIC_IMASK1 bit 9, source 41
    Check(sic.GetIVG(41) == 13, "Port G interrupt B routes to IVG13");
    Check(sic.Read32(0x54) == ((iar5 & ~0xF0u) | 0x60u),
          "SIC_IAR5 preserves other source assignments");

    // Firmware also accesses PORTG_FER and grouped hysteresis registers;
    // neither is implemented by this GPIO class, so do not emulate them here.
    portG.Write32(0x30, 0);       // PORTGIO_DIR: PG3 input
    portG.Write32(0x40, kPg3);    // PORTGIO_INEN
    portG.Write32(0x34, kPg3);    // PORTGIO_POLAR: active low
    portG.Write32(0x38, kPg3);    // PORTGIO_EDGE: edge triggered
    portG.Write32(0x3C, 0);       // PORTGIO_BOTH: single falling edge
    portG.Write32(0x28, kPg3);    // PORTGIO_MASKB_SET
    Check((portG.Read32(0x20) & kPg3) != 0, "PG3 enabled in interrupt B");
    Check(sic.Lines() == 0 && raised.empty(), "no interrupt before a falling edge");

    Check(portG.SetPinInput(3, GPIOPinLevel::High), "PG3 initially high");
    Check(portG.GetPinOutput(3) == GPIOPinLevel::High, "PG3 physical level high");
    CheckEdgeReadback(portG, kPg3, 0, "initial high without edge");
    Check(sic.Lines() == 0, "initial high does not assert interrupt");

    Check(portG.SetPinInput(3, GPIOPinLevel::Low), "PG3 first falling edge");
    Check(portG.GetPinOutput(3) == GPIOPinLevel::Low, "PG3 physical level low");
    CheckEdgeReadback(portG, kPg3, kPg3, "first falling edge latches data");
    Check((sic.Read32(0x60) & kSicPortGB) != 0, "SIC_ISR1 reports source 41");
    Check(sic.Lines() == kIvg13 && raised == std::vector<u16>{kIvg13},
          "first falling edge asserts IVG13 once");

    // Edge input readback stays sticky even if the *pin* returns high.
    Check(portG.SetPinInput(3, GPIOPinLevel::High), "PG3 returns high while pending");
    Check(portG.GetPinOutput(3) == GPIOPinLevel::High, "physical PG3 high before ack");
    CheckEdgeReadback(portG, kPg3, kPg3, "latch stays set after pin goes high");
    Check(sic.Lines() == kIvg13 && raised.size() == 1,
          "rising input does not acknowledge falling-edge interrupt");

    Check(portG.SetPinInput(3, GPIOPinLevel::Low), "PG3 another falling transition");
    CheckEdgeReadback(portG, kPg3, kPg3, "edge still pending after second fall");
    Check(sic.Lines() == kIvg13 && raised.size() == 1,
          "second falling edge does not create a second rising IRQ while pending");

    // The selected firmware handler writes bit 3 to PORTGIO_CLEAR to ack.
    portG.Write32(0x04, kPg3);
    Check((sic.Read32(0x60) & kSicPortGB) == 0 && sic.Lines() == 0,
          "acknowledgment deasserts source 41 and IVG13");
    CheckEdgeReadback(portG, kPg3, 0, "edge latch reads zero after ack");
    Check(portG.GetPinOutput(3) == GPIOPinLevel::Low,
          "acknowledgment preserves externally driven PG3 level");

    const auto callbacksAfterAck = raised.size();
    Check(portG.SetPinInput(3, GPIOPinLevel::Low), "PG3 repeatedly sampled low");
    CheckEdgeReadback(portG, kPg3, 0, "repeated low does not relatch edge");
    Check(raised.size() == callbacksAfterAck && sic.Lines() == 0,
          "repeated low sample does not synthesize another falling edge");

    Check(portG.SetPinInput(3, GPIOPinLevel::High), "PG3 returns high");
    CheckEdgeReadback(portG, kPg3, 0, "rising transition does not set falling-edge latch");
    Check(sic.Lines() == 0, "rising transition does not trigger interrupt");
    Check(portG.SetPinInput(3, GPIOPinLevel::Low), "PG3 falls again");
    CheckEdgeReadback(portG, kPg3, kPg3, "next high-to-low sets latch again");
    Check(raised.size() == callbacksAfterAck + 1 && raised.back() == kIvg13,
          "high-to-low transition rearms the interrupt");

    portG.Write32(0x24, kPg3); // PORTGIO_MASKB_CLEAR
    Check(sic.Lines() == 0 && (sic.Read32(0x60) & kSicPortGB) == 0,
          "masking pending edge lowers SIC output without clearing latch");
    CheckEdgeReadback(portG, kPg3, kPg3, "edge stays latched while masked");
    portG.Write32(0x28, kPg3); // PORTGIO_MASKB_SET
    Check(sic.Lines() == kIvg13 && raised.size() == callbacksAfterAck + 2,
          "unmasking pending edge republishes interrupt");
    portG.Write32(0x04, kPg3); // acknowledge again
    Check(sic.Lines() == 0 && portG.GetPinOutput(3) == GPIOPinLevel::Low,
          "second acknowledgment lowers IRQ, not input level");
    CheckEdgeReadback(portG, kPg3, 0, "second acknowledgement clears latch");

    // An ordinary output still uses its data bit, not the edge latch.
    portG.Write32(0x30, kPg4); // PG4 output; PG3 stays input
    portG.Write32(0x08, kPg4); // PORTGIO_SET
    Check(portG.GetPinOutput(4) == GPIOPinLevel::High, "PG4 output was set");
    CheckEdgeReadback(portG, kPg4, kPg4, "output readback reflects set");
    portG.Write32(0x04, kPg4); // PORTGIO_CLEAR for output
    Check(portG.GetPinOutput(4) == GPIOPinLevel::Low, "PG4 output was cleared");
    CheckEdgeReadback(portG, kPg4, 0, "output readback reflects clear");
    Check(portG.GetPinOutput(3) == GPIOPinLevel::Low && sic.Lines() == 0,
          "clearing another pin does not disturb PG3");
}

// General GPIO behavior matters for the keypad's PG0 GPIOOrGate connection,
// which may deliver repeated Low samples. This is a *synthetic configuration*;
// it does not assert the original firmware's exact PG0 POLAR/EDGE settings.
void SharedMaskAndRepeatedLow() {
    GPIO portG("PORTG", 0xFFC01500);
    int interruptLevel = 0;
    portG.BindInterruptB(41, [&](int source, int level) {
        Check(source == 41, "Port G source index 41");
        interruptLevel = level;
    });
    portG.Write32(0x30, 0);                  // both pins input
    portG.Write32(0x40, kPg0 | kPg3);       // both input buffers enabled
    portG.Write32(0x34, kPg0 | kPg3);       // active low
    portG.Write32(0x38, kPg0 | kPg3);       // falling-edge detection
    portG.Write32(0x3C, 0);                 // single edge
    portG.Write32(0x28, kPg0 | kPg3);       // both contribute to MASKB

    Check(portG.SetPinInput(0, GPIOPinLevel::High), "PG0 initialized high");
    Check(portG.SetPinInput(3, GPIOPinLevel::High), "PG3 initialized high");
    Check(interruptLevel == 0, "initial inputs do not assert source 41");

    Check(portG.SetPinInput(0, GPIOPinLevel::Low), "first PG0 falling edge");
    CheckEdgeReadback(portG, kPg0 | kPg3, kPg0, "only PG0 initially latched");
    Check(interruptLevel == 1, "PG0 asserts shared interrupt B");

    Check(portG.SetPinInput(3, GPIOPinLevel::Low), "PG3 falling edge while PG0 pending");
    CheckEdgeReadback(portG, kPg0 | kPg3, kPg0 | kPg3, "two pending edge sources");

    portG.Write32(0x04, kPg3); // ack one edge while another remains pending
    CheckEdgeReadback(portG, kPg0 | kPg3, kPg0, "acknowledge PG3 but not PG0");
    Check(interruptLevel == 1, "shared interrupt remains asserted for PG0");

    portG.Write32(0x04, kPg0);
    CheckEdgeReadback(portG, kPg0 | kPg3, 0, "acknowledging both removes sources");
    Check(interruptLevel == 0, "shared interrupt deasserts after final ack");
    Check(portG.GetPinOutput(0) == GPIOPinLevel::Low,
          "PG0 physical Low persists across acknowledgment");

    for (int i = 0; i < 3; ++i) {
        Check(portG.SetPinInput(0, GPIOPinLevel::Low), "repeated PG0 low delivery");
        CheckEdgeReadback(portG, kPg0, 0, "repeated low does not generate false edge");
        Check(interruptLevel == 0, "repeated PG0 low does not reactivate interrupt");
    }
    Check(portG.SetPinInput(0, GPIOPinLevel::High), "PG0 rises to rearm");
    Check(interruptLevel == 0, "PG0 rising edge ignored");
    Check(portG.SetPinInput(0, GPIOPinLevel::Low), "PG0 falls again");
    CheckEdgeReadback(portG, kPg0, kPg0, "rearmed PG0 edge is visible");
    Check(interruptLevel == 1, "rearmed PG0 interrupt is asserted");
}
// Register writes must never copy sticky readback into sampled input levels.
// Exercise each write with PG3 input and PG4 output in the same port.
void InputWritesPreserveSamples() {
    for (u32 offset : {0x00u, 0x08u, 0x0Cu}) {
        const std::string context = "write offset " + std::to_string(offset);
        GPIO portG("PORTG", 0xFFC01500);
        int interruptLevel = 0;
        portG.BindInterruptB(41, [&](int, int level) { interruptLevel = level; });
        portG.Write32(0x30, kPg4);
        portG.Write32(0x40, kPg3);
        portG.Write32(0x34, kPg3);
        portG.Write32(0x38, kPg3);
        portG.Write32(0x28, kPg3);
        portG.SetPinInput(3, GPIOPinLevel::High);
        portG.SetPinInput(3, GPIOPinLevel::Low);
        portG.SetPinInput(3, GPIOPinLevel::High);
        CheckEdgeReadback(portG, kPg3, kPg3, context + ": pending after return high");

        // DATA performs a real read/modify/write; SET/TOGGLE receive the same
        // mixed input/output mask. None may modify the sampled high input.
        portG.Write32(offset, portG.Read32(0x00) | kPg4);
        Check(portG.GetPinOutput(3) == GPIOPinLevel::High,
              context + ": pending write preserves sampled high");
        CheckEdgeReadback(portG, kPg3 | kPg4, kPg3 | kPg4,
                          context + ": write changes output and preserves latch");
        Check(interruptLevel == 1, context + ": pending IRQ survives write");
        portG.Write32(0x04, kPg3);
        CheckEdgeReadback(portG, kPg3, 0, context + ": acknowledgment clears latch");
        portG.SetPinInput(3, GPIOPinLevel::Low);
        CheckEdgeReadback(portG, kPg3, kPg3, context + ": next real fall is detected");
        Check(interruptLevel == 1, context + ": next real fall asserts IRQ");

        portG.Write32(0x04, kPg3);
        // After acknowledgment DATA reads zero for a still-low input. A
        // write-back must not turn that sampled low into a high, either.
        portG.Write32(offset, portG.Read32(0x00));
        Check(portG.GetPinOutput(3) == GPIOPinLevel::Low,
              context + ": post-ack write preserves sampled low");
        for (int i = 0; i < 3; ++i) {
            portG.SetPinInput(3, GPIOPinLevel::Low);
            CheckEdgeReadback(portG, kPg3, 0, context + ": repeated low has no false edge");
            Check(interruptLevel == 0, context + ": repeated low leaves IRQ clear");
        }
        // Explicit zero and one writes cover both stored input values and
        // verify normal output DATA/SET/TOGGLE behavior on the same register.
        portG.Write32(offset, kPg3 | kPg4);
        Check(portG.GetPinOutput(3) == GPIOPinLevel::Low,
              context + ": one write preserves low input");
        CheckEdgeReadback(portG, kPg3 | kPg4, kPg4, context + ": output set with input ignored");
        portG.Write32(offset, 0);
        CheckEdgeReadback(portG, kPg3 | kPg4, offset == 0 ? 0 : kPg4,
                          context + ": zero write obeys output register semantics");
        portG.Write32(0x04, kPg4);
        CheckEdgeReadback(portG, kPg3 | kPg4, 0, context + ": output clear still works");
    }
}
} // namespace

int main() {
    Stock246Pg3FallingEdge();
    SharedMaskAndRepeatedLow();
    InputWritesPreserveSamples();
    std::puts("gpio-edge-test: sticky readback, input writes, acknowledgment, PG3 routing and PG0 passed");
}
