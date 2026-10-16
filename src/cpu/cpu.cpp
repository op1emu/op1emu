#include "cpu.h"
#include "bcore_memory.h"
#include "ebiu.h"
#include "bootrom.h"
#include "twi.h"
#include "sic.h"
#include "coretimer.h"
#include "otp.h"
#include "gptimer.h"
#include "dma.h"
#include "ppi.h"
#include "gpio.h"
#include "nand.h"
#include "jtag.h"
#include "rtc.h"
#include "usb.h"
#include "sport.h"
#include "emu.h"
#ifdef ENABLE_PROFILING
#include "profiling/profiler.h"
#include "profiling/memory_census.h"
#include "profiling/service_stats.h"
#include <cstdlib>
#include <cxxabi.h>
#include <typeinfo>
#endif
#include "peripheral/mcp230xx.h"
#include "peripheral/adxl345.h"
#include "peripheral/oled.h"
#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include "peripheral/potentiometer.h"
#include "peripheral/audio_output.h"
#include "utils/log.h"

#include "core.h"
#include "cpu_state.h"
#include "mmr.h"
#include <cstring>
#include <algorithm>
#include <stdexcept>

// bcore CEC functions (extern "C" in bcore's src/cec.h)
extern "C" void cec_raise(CpuState* cpu, uint32_t ivg);
extern "C" void cec_check_pending(CpuState* cpu);

// bcore EVT init (extern in bcore's src/evt.h)
void evt_init();

static constexpr int IVG_IVTMR = 6;

static constexpr int IRQ_TWI = 20;
static constexpr int IRQ_PORTF_A = 45;
static constexpr int IRQ_PORTF_B = 46;
static constexpr int IRQ_PORTG_A = 40;
static constexpr int IRQ_PORTG_B = 41;
static constexpr int IRQ_PORTH_A = 29;
static constexpr int IRQ_PORTH_B = 31;
static constexpr int IRQ_DMA0 = 15;
static constexpr int IRQ_DMA1 = 28;
static constexpr int IRQ_DMA2 = 30;
static constexpr int IRQ_DMA3 = 16;
static constexpr int IRQ_RTC = 14;
static constexpr int IRQ_NFC = 48;
static constexpr int IRQ_USB_INT0 = 52;
static constexpr int IRQ_USB_INT1 = 53;
static constexpr int IRQ_USB_INT2 = 54;
static constexpr int IRQ_USB_DMAINT = 55;
static constexpr int IRQ_GPTIMER_0 = 32;

// Thin CEC device shim so the Emulator device map covers the CEC MMR range.
// bcore intercepts these internally during JIT execution, but the Emulator
// needs a device registered to avoid "unmapped address" warnings.
// Note: CEC state is process-global (module-level statics in bcore's cec.cpp).
// Only one BlackFinCpu instance may exist per process.
class BcoreCECDevice : public Device {
public:
    BcoreCECDevice(CpuState* cpu)
        : Device("CEC", CEC_MMR_BASE, CEC_MMR_SIZE), cpu_(cpu) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = cec_mmr_read(cpu_, baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        cec_mmr_write(cpu_, baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return cec_mmr_read(cpu_, baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        cec_mmr_write(cpu_, baseAddress + offset, value);
    }
private:
    CpuState* cpu_;
};

// Thin EVT device shim for the same reason.
class BcoreEVTDevice : public Device {
public:
    BcoreEVTDevice()
        : Device("EVT", EVT_BASE, EVT_SIZE) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = evt_read(baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        evt_write(baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return evt_read(baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        evt_write(baseAddress + offset, value);
    }
};

BlackFinCpu::BlackFinCpu(bool deterministic) : time_(deterministic), pc(0) {
    cpuState_ = std::make_unique<CpuState>();
    memset(cpuState_.get(), 0, sizeof(CpuState));

    auto irqHandler = [this](int q, int level) { this->ProcessInterrupt(q, level); };
    devices.emplace_back(std::make_shared<MemoryDevice>("L1 SRAM", 0xFFB00000, 0x1000));
    devices.emplace_back(std::make_shared<MemoryDevice>("PORT_MUX", 0xFFC03200, 0x100));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A",   0xFF800000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A Cache", 0xFF804000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B",   0xFF900000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B Cache", 0xFF904000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst A",   0xFFA00000, 0x8000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst B",   0xFFA08000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst Cache", 0xFFA10000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("SDRAM", 0, 0x8000000));

    // CEC/EVT shim devices for emulator device map coverage
    devices.emplace_back(std::make_shared<BcoreEVTDevice>());
    devices.emplace_back(std::make_shared<BcoreCECDevice>(cpuState_.get()));
    // MMU region — register as plain memory (bcore does not use a separate MMU device)
    devices.emplace_back(std::make_shared<MemoryDevice>("MMU", 0xFFE00000, 0x2000));

    devices.emplace_back(std::make_shared<EBIU>(0xFFC00A00));
    devices.emplace_back(std::make_shared<OTP>(0xFFC03600, "otp.bin"));
    std::shared_ptr<USB> usb = std::make_shared<USB>(0xFFC03800);
    usb->BindInterrupt(IRQ_USB_INT0, IRQ_USB_INT1, IRQ_USB_INT2, IRQ_USB_DMAINT, irqHandler);
    devices.emplace_back(usb);
    this->usb = usb;
    serviced_.usb = usb;
    sport0 = std::make_shared<SPORT>(0xFFC00800, 0, time_);
    devices.emplace_back(sport0);
    sport1 = std::make_shared<SPORT>(0xFFC00900, 1, time_);
    devices.emplace_back(sport1);
    // OP-1 seems only use last byte of DSPID, which is 0x02 for BF524 rev 02
    devices.emplace_back(std::make_shared<Jtag>(0xFFE05000, 0x02));
    devices.emplace_back(std::make_shared<BootROM>(0xEF000000));
    gptimer = std::make_shared<GPTimer>(0xFFC00600);
    for (int i = 0; i < 8; i++) {
        gptimer->BindInterrupt(i, IRQ_GPTIMER_0 + i, irqHandler);
    }
    devices.emplace_back(gptimer);
    nfc = std::make_shared<NFC>(0xFFC03700);
    nfc->BindInterrupt(IRQ_NFC, irqHandler);
    devices.emplace_back(nfc);

    std::shared_ptr<RTC> rtc = std::make_shared<RTC>(0xFFC00300, time_);
    rtc->BindInterrupt(IRQ_RTC, irqHandler);
    devices.emplace_back(rtc);
    serviced_.rtc = rtc;

    ppi = std::make_shared<PPI>(0xFFC01000);
    devices.emplace_back(ppi);
    std::shared_ptr<DMA> dma = std::make_shared<DMA>(0xFFC00C00, emulator, &time_);
    devices.emplace_back(dma);
    serviced_.dma = dma;
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralPPI, ppi);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralNFC, nfc);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Rx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Tx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Rx, sport1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Tx, sport1);
    auto memorySrcBus0 = std::make_shared<MemorySrcDMABus>();
    auto memoryDestBus0 = std::make_shared<MemoryDestDMABus>(*memorySrcBus0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMADest0, memoryDestBus0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMASrc0, memorySrcBus0);
    auto memorySrcBus1 = std::make_shared<MemorySrcDMABus>();
    auto memoryDestBus1 = std::make_shared<MemoryDestDMABus>(*memorySrcBus1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMADest1, memoryDestBus1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralMDMASrc1, memorySrcBus1);
    dma->BindInterrupt(0, IRQ_DMA0, irqHandler);
    dma->BindInterrupt(1, IRQ_DMA1, irqHandler);
    dma->BindInterrupt(2, IRQ_DMA2, irqHandler);
    dma->BindInterrupt(3, IRQ_DMA3, irqHandler);
    std::shared_ptr<GPIO> portF = std::make_shared<GPIO>("PORTF", 0xFFC00700);
    portF->BindInterruptA(IRQ_PORTF_A, irqHandler);
    portF->BindInterruptB(IRQ_PORTF_B, irqHandler);
    devices.emplace_back(portF);
    portG = std::make_shared<GPIO>("PORTG", 0xFFC01500);
    portG->BindInterruptA(IRQ_PORTG_A, irqHandler);
    portG->BindInterruptB(IRQ_PORTG_B, irqHandler);
    devices.emplace_back(portG);
    std::shared_ptr<GPIO> portH = std::make_shared<GPIO>("PORTH", 0xFFC01700);
    portH->BindInterruptA(IRQ_PORTH_A, irqHandler);
    portH->BindInterruptB(IRQ_PORTH_B, irqHandler);
    devices.emplace_back(portH);

    sic = std::make_shared<SIC>(0xFFC00100);
    sic->SetLineRaisedCallback([this](u16) { LatchLines(); });
    devices.push_back(sic);
    coreTimer = std::make_shared<CoreTimer>(0xFFE03000, time_);
    // The core timer's interrupt is a line of the core's own, IVG6.
    coreTimer->BindInterrupt(IVG_IVTMR, [this](int ivg, int level) {
        coreTimerLine_ = level != 0;
        if (coreTimerLine_) LatchLines();
    });
    devices.emplace_back(coreTimer);

    std::shared_ptr<TWI> twi = std::make_shared<TWI>(0xFFC01400, &time_);
    devices.push_back(twi);
    serviced_.twi = twi;

    for (int i = 0; i < 8; i++) {
        u32 addr = 0x20 + i;
        gpioExpanders.push_back(std::make_shared<MCP230XX>(addr, MCP230XXModel::MCP23017));
        twi->AttachPeripheral(gpioExpanders.back());
    }
    // TODO: this bit controls DAT_01f007e0, figure out its function
    gpioExpanders[0]->SetPinInput(6, GPIOPinLevel::High); // Set MCP23017 #6 high
    // Connect gpio expanders
    gpioExpanders[3]->Connect(16, {*gpioExpanders[2], 0}); // Connect gpio3 INTA to gpio2 #0
    gpioExpanders[4]->Connect(16, {*gpioExpanders[2], 1}); // Connect gpio4 INTA to gpio2 #1
    gpioExpanders[6]->Connect(16, {*gpioExpanders[2], 2}); // Connect gpio6 INTA to gpio2 #2
    gpioExpanders[5]->Connect(16, {*gpioExpanders[2], 3}); // Connect gpio5 INTA to gpio2 #3

    gpioOrConnection = std::make_shared<GPIOOrGate>(true); // active low
    gpioOrConnection->Connect(2, {*portG, 0}); // Connect GPIOOr output to portG #0
    gpioExpanders[2]->Connect(16, {*gpioOrConnection, 0}); // Connect gpio2 INTA to GPIOOr #0
    // FIXME: figure out how gpio2 INTA and gpio0 INTA are connected to portG #0
    gpioExpanders[0]->Connect(16, {*gpioOrConnection, 1}); // Connect gpio0 INTA to GPIOOr #1

    adxl345 = std::make_shared<ADXL345>(0x53);
    adxl345->Connect(0, {*gpioExpanders[0], 1}); // Connect ADXL345 INT1 to gpio0 #1

    twi->AttachPeripheral(adxl345);
    // This one byte controls DAT_ff802894, which seams to be "is_not_display_flip"
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x1a, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x18, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x58, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x09, 0x0)); // Dummy I2C device
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x4a, 0x0)); // probably ADC?
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x64, 0x3C)); // LTC2941 battgauge
    twi->AttachPeripheral(std::make_shared<DummyI2CPeripheral>(0x11, 0x80)); // Si4713? FM radio
    // TODO: figure out the exact device of the potentiometer
    potentiometer = std::make_shared<Potentiometer>(0x54);
    twi->AttachPeripheral(potentiometer);
    twi->BindInterrupt(IRQ_TWI, irqHandler);

    for (const auto& device : devices) {
        emulator.BindDevice(device.get());
    }
    serviced_.nfc = nfc;
    serviced_.coreTimer = coreTimer;
    auto indexOf = [this](const Device* device) {
        for (size_t i = 0; i < devices.size(); ++i)
            if (devices[i].get() == device) return i;
        throw std::logic_error("serviced device not in the device list");
    };
    serviced_.usbIndex = indexOf(serviced_.usb.get());
    serviced_.nfcIndex = indexOf(serviced_.nfc.get());
    serviced_.rtcIndex = indexOf(serviced_.rtc.get());
    serviced_.dmaIndex = indexOf(serviced_.dma.get());
    serviced_.coreTimerIndex = indexOf(serviced_.coreTimer.get());
    serviced_.twiIndex = indexOf(serviced_.twi.get());

    std::array<GPIOPeripheral::GPIOConnection, OLED::DATA_PINS> oledDatabus{
        std::tuple<GPIOPeripheral&, int>{*portF, 0},
        std::tuple<GPIOPeripheral&, int>{*portF, 1},
        std::tuple<GPIOPeripheral&, int>{*portF, 2},
        std::tuple<GPIOPeripheral&, int>{*portF, 3},
        std::tuple<GPIOPeripheral&, int>{*portF, 4},
        std::tuple<GPIOPeripheral&, int>{*portF, 5},
        std::tuple<GPIOPeripheral&, int>{*portF, 6},
        std::tuple<GPIOPeripheral&, int>{*portF, 7},
        std::tuple<GPIOPeripheral&, int>{*portF, 8},
        std::tuple<GPIOPeripheral&, int>{*portF, 9},
        std::tuple<GPIOPeripheral&, int>{*portF, 10},
        std::tuple<GPIOPeripheral&, int>{*portF, 11},
        std::tuple<GPIOPeripheral&, int>{*portF, 12},
        std::tuple<GPIOPeripheral&, int>{*portF, 13},
        std::tuple<GPIOPeripheral&, int>{*portF, 14},
        std::tuple<GPIOPeripheral&, int>{*portF, 15},
    };
    std::tuple<GPIOPeripheral&, int> oledWr {*portG, 11};
    std::tuple<GPIOPeripheral&, int> oledCs {*portG, 5};
    std::tuple<GPIOPeripheral&, int> oledRd {*portG, 4};
    std::tuple<GPIOPeripheral&, int> oledRs {*portG, 2};
    oled = std::make_shared<OLED>(oledDatabus, oledCs, oledRs, oledRd, oledWr);

    // Initialize bcore after all devices are bound
    bcoreMemory_ = std::make_unique<EmulatorMemory>(emulator);
    core_ = std::make_shared<Core>(cpuState_.get(), bcoreMemory_.get());
    // IR pipeline at O1, backend at CodeGenOpt::Default. Until bcore wired
    // optimize_module into compilation, the IR level only ever affected the
    // IR-dump output, so init(2) compiled with the backend alone. Measured on
    // a boot to the main display with the backend at CodeGenOpt::Less (this
    // exact pairing with Default was not timed): O1 -6% execution for +43%
    // compile; O2 no faster than O1 and +69% compile.
    core_->init(/*opt_level=*/1, /*codegen_level=*/2);

    // Initialize bcore CEC and EVT
    cec_init();
    evt_init();

    SetRegister(RegIndex::SP, 0x7000000); // Set stack pointer to top of SDRAM
    SetRegister(RegIndex::FP, 0x7000000); // Set frame pointer to top of SDRAM
    cpuState_->ksp = 0x7000000;
    cpuState_->usp = 0x7000000;
    cpuState_->syscfg = 0x30;
    time_.SetBlockPackets(&cpuState_->packets);

    time_.StartWallClock();
}

BlackFinCpu::~BlackFinCpu() {
}

void BlackFinCpu::ProcessInterrupt(int pin, int level) {
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->Published(pin, level);
#endif
    sic->SetInterruptLevel(pin, level);
}

void BlackFinCpu::HaltExecution(HaltReason reason) {
    // TODO
}

void BlackFinCpu::SaveContext() {
    // TODO
}

void BlackFinCpu::RestoreContext() {
    // TODO
}

static void SetBfinCycles(CpuState& cpu_state, u64 cycles) {
    cpu_state.cycles[0] = (u32)(cycles & 0xffffffff);
    cpu_state.cycles[1] = (u32)(cycles >> 32);
    cpu_state.cycles[2] = cpu_state.cycles[1];
}

void BlackFinCpu::InvalidateTranslations() {
#ifdef ENABLE_PROFILING
    if (profiler_) profiler_->BeforeInvalidate();
#endif
    core_->invalidate();
#ifdef ENABLE_PROFILING
    if (profiler_) profiler_->AfterInvalidate();
#endif
}

HaltReason BlackFinCpu::Run() {
    // Events queued from this thread from now on are timed in guest time.
    if (cpuThread_.load(std::memory_order_relaxed) != std::this_thread::get_id())
        cpuThread_.store(std::this_thread::get_id(), std::memory_order_relaxed);
    // Guest time at the start of this block: host time, or in deterministic
    // mode the packets every earlier block executed (see TimeSource).
    const uint64_t cyclesElapsed = time_.Cycles();
    SetBfinCycles(*cpuState_, cyclesElapsed);

#ifdef ENABLE_PROFILING
    const uint32_t blockPc = cpuState_->pc;
    if (profiler_) profiler_->BeforeRun(blockPc);
#endif
    // Hit entry point, invalidating core to reset cached translations
    if (cpuState_->pc == 0xFFA00000) InvalidateTranslations();
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->current = stats->block;
#endif
    // Execute one basic block — bcore updates cpuState_->pc internally.
    // Hardware loops, PC advance, and hwloop counters are all handled by bcore.
    if (!core_->run(cpuState_->pc)) {
        // Translation/JIT failure: PC is unchanged, so retrying would loop
        // forever. Skip device processing and let the host stop the run.
        LogError("JIT execution failed at PC 0x%08x", cpuState_->pc);
        failed_.store(true, std::memory_order_release);
        return HaltReason::Break;
    }
    // Retired, and cleared so the guest time stops counting them twice
    // (TimeSource::SetBlockPackets).
    const uint32_t packets = cpuState_->packets;
    cpuState_->packets = 0;
    time_.Retire(packets);
#ifdef ENABLE_PROFILING
    if (profiler_) profiler_->AfterRun(blockPc, packets);
#endif
    cpuState_->did_jump = false; // Clear jump flag set by bcore, since we handle it in the emulator loop
    cec_check_pending(cpuState_.get());

    // Get active IVG from CEC
    int ivg = cec_current_ivg();
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->current = stats->other;
#endif
    ServiceDevices(ivg);
    // The guest time at the end of the block: timers count to it.
    const uint64_t blockEnd = time_.Cycles();
    coreTimer->Advance(blockEnd);
    // Drive the GP timers off the *delta* in guest cycles, not off it landing
    // on a multiple. `cycles % 10000 == 0` only fires when the sampled
    // value happens to be an exact multiple of 10000, which is essentially
    // never true for a value derived from wall-clock microseconds -- most
    // ticks were silently dropped, and occasionally the value sat on a
    // multiple across calls and ticked repeatedly, so the emulated GP timer
    // rate was erratic (measured: programmed period=200 firing at 10-19 ms
    // instead of the 5 ms that rate implies).
    //
    // An accumulator is both correct and cheap. At most kMaxCatchupTicks are
    // delivered per call and the rest of a long gap is dropped on purpose
    // (the accumulator still advances by the full gap): after a host stall or
    // suspend/resume, replaying the backlog would make the guest storm timer
    // interrupts for as long as the gap was.
    // FIXME: still one shared rate for all three clock domains, which is not
    // how SCLK/TACLK/TMRCLK relate on real hardware.
    constexpr uint64_t kCyclesPerTimerTick = 10000;
    constexpr uint64_t kMaxCatchupTicks = 64;
    uint64_t ticks = (blockEnd - lastTimerCycles_) / kCyclesPerTimerTick;
    if (ticks) {
        lastTimerCycles_ += ticks * kCyclesPerTimerTick;
        if (ticks > kMaxCatchupTicks) {
#ifdef ENABLE_PROFILING
            if (auto* stats = Service()) stats->timerTicksDropped += ticks - kMaxCatchupTicks;
#endif
            ticks = kMaxCatchupTicks;
        }
#ifdef ENABLE_PROFILING
        if (auto* stats = Service()) stats->timerTicks += ticks;
#endif
        for (uint64_t i = 0; i < ticks; ++i) {
            gptimer->Tick(GPTimerClockTypeSCLK);
            gptimer->Tick(GPTimerClockTypeTACLK);
            gptimer->Tick(GPTimerClockTypeTMRCLK);
        }
    }

    if (time_.Deterministic()) ServicePanel();
    ProcessEvents();
    // Lines still high get latched again, and a latched interrupt is taken
    // before the next block.
    LatchLines();
    cec_check_pending(cpuState_.get());
    return HaltReason::Break;
}

#ifdef ENABLE_PROFILING
void BlackFinCpu::SetProfiler(Profiler* profiler) {
    profiler_ = profiler;
    bcoreMemory_->SetProfiler(profiler);
}

BcoreStats BlackFinCpu::CoreStats() const { return core_->stats(); }

void BlackFinCpu::SetCoreEventSink(BcoreEventSink* sink) { core_->setEventSink(sink); }

void BlackFinCpu::SetPerfJitdump(bool enable) { core_->set_perf_jitdump(enable); }
#endif

void BlackFinCpu::ServicePanel() {
    if (auto low = panel_.Service(time_.Nanoseconds()))
        portG->SetPinInput(3, *low ? GPIOPinLevel::Low : GPIOPinLevel::High);
}

void BlackFinCpu::SetRegister(int index, u32 value) {
    switch (index)
    {
    case RegIndex::FP:
        cpuState_->dpregs[15] = value;
        break;
    case RegIndex::SP:
        cpuState_->dpregs[14] = value;
        break;
    case RegIndex::RETS:
        cpuState_->rets = value;
        break;
    case RegIndex::R0...RegIndex::R2:
        cpuState_->dpregs[index - RegIndex::R0] = value;
        break;
    case RegIndex::P1:
        cpuState_->dpregs[9] = value;
        break;
    default:
        break;
    }
}

u32 BlackFinCpu::GetRegister(int index) {
    switch (index)
    {
    case RegIndex::FP:
        return cpuState_->dpregs[15];
    case RegIndex::SP:
        return cpuState_->dpregs[14];
    case RegIndex::RETS:
        return cpuState_->rets;
    case RegIndex::R0...RegIndex::R2:
        return cpuState_->dpregs[index - RegIndex::R0];
    case RegIndex::P1:
        return cpuState_->dpregs[9];
    default:
        return 0;
    }
}

void BlackFinCpu::SetPC(u32 value) {
    cpuState_->pc = value;
}

u32 BlackFinCpu::PC() {
    return cpuState_->pc;
}

// Runs `event` once the guest time has passed `delay` from now: for the CPU
// thread now is the time of the packet queuing it, for another thread the
// CPU thread's next ProcessEvents().
void BlackFinCpu::QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay) {
    if (std::this_thread::get_id() == cpuThread_.load(std::memory_order_relaxed)) {
        const uint64_t ticks = delay.count() > 0 ? uint64_t(delay.count()) : 0;
        ScheduleEvent(event, time_.Nanoseconds() + ticks);
    } else {
        std::lock_guard<std::mutex> lock(inboxMutex_);
        inbox_.emplace_back(delay, event);
        inboxPending_.store(true, std::memory_order_release);
    }
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->Queued();
#endif
}

void BlackFinCpu::ScheduleEvent(std::function<void()> event, uint64_t dueNs) {
    events_.push_back({dueNs, eventSequence_++, std::move(event)});
    std::push_heap(events_.begin(), events_.end(), std::greater<>());
    nextEventNs_ = events_.front().dueNs;
}

// Latches every interrupt line that is high and neither latched (ILAT) nor
// being serviced (IPEND): the core's level-sensitive inputs. Called when a
// line rises and after every block, which covers a line still high when its
// ISR returns or the guest clears its ILAT bit. Inside a block (a register
// write raised the line) the core takes the interrupt at its next check.
void BlackFinCpu::LatchLines() {
    const u16 lines = sic->Lines() | (coreTimerLine_ ? 1u << IVG_IVTMR : 0u);
    if (!lines) return;
    // Another thread (the USB/IP loop) must not touch the CEC; the CPU thread
    // latches the line after its next block.
    if (std::this_thread::get_id() != cpuThread_.load(std::memory_order_relaxed)) return;
    const uint32_t ilat = cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 4 * CEC_IDX_ILAT);
    const uint32_t ipend = cec_mmr_read(cpuState_.get(), CEC_MMR_BASE + 4 * CEC_IDX_IPEND);
    u16 latch = lines & ~(ilat | ipend);
    if (!latch) return;
    // bcore's cec_raise() is the RAISE instruction: in user mode it raises an
    // exception instead. A peripheral cannot, so wait for supervisor mode.
    constexpr uint32_t kSupervisorPend = ~0x11u;  // all but EMU and IRPTEN
    if (!(ipend & kSupervisorPend)) {
        if (!userModeLatchWarned_) {
            LogWarn("interrupt lines 0x%04x held off: bcore cannot latch them in user mode", latch);
            userModeLatchWarned_ = true;
        }
#ifdef ENABLE_PROFILING
        ++userModeLatchDeferred_;
#endif
        return;
    }
    for (; latch; latch &= latch - 1) {
        const int ivg = __builtin_ctz(latch);
#ifdef ENABLE_PROFILING
        if (auto* stats = Service()) stats->Forwarded(ivg);
#endif
        RaiseInterrupt(ivg);
    }
}

#ifdef ENABLE_PROFILING
static std::string Demangled(const std::type_info& type) {
    int status = 0;
    char* name = abi::__cxa_demangle(type.name(), nullptr, nullptr, &status);
    std::string out = status == 0 && name ? name : type.name();
    std::free(name);
    return out;
}

// Base classes depth first, nearest first: the order a lookup of an inherited
// member searches them in when only one base declares it.
static void AddBases(const std::type_info& type, std::vector<std::string>& out) {
    if (auto* single = dynamic_cast<const abi::__si_class_type_info*>(&type)) {
        out.push_back(Demangled(*single->__base_type));
        AddBases(*single->__base_type, out);
    } else if (auto* multiple = dynamic_cast<const abi::__vmi_class_type_info*>(&type)) {
        for (unsigned i = 0; i < multiple->__base_count; ++i) {
            const auto& base = *multiple->__base_info[i].__base_type;
            out.push_back(Demangled(base));
            AddBases(base, out);
        }
    }
}

profiling::ServiceStats& BlackFinCpu::EnableServiceStats() {
    if (serviceStats_) return *serviceStats_;
    std::vector<profiling::ServiceStats::DeviceInfo> infos;
    for (const auto& device : devices) {
        const auto& value = *device;
        profiling::ServiceStats::DeviceInfo info{device->Name(), Demangled(typeid(value)), {}};
        // The base classes, so a report can find the ProcessWithInterrupt a
        // device inherits (GPIO, SPORT and most others run Device's).
        AddBases(typeid(value), info.bases);
        infos.push_back(std::move(info));
    }
    serviceStats_ = std::make_unique<profiling::ServiceStats>();
    serviceStats_->SetDevices(infos);
    // Published last: the USB/IP thread reads it.
    service_.store(serviceStats_.get(), std::memory_order_release);
    return *serviceStats_;
}

profiling::MemoryCensus& BlackFinCpu::EnableMemoryCensus() {
    if (memoryCensus_) return *memoryCensus_;
    std::vector<profiling::MemoryCensus::DeviceRange> ranges;
    for (const Device* device : emulator.Devices())
        ranges.push_back({device->Name(), device->BaseAddress(), device->EndAddress()});
    memoryCensus_ = std::make_unique<profiling::MemoryCensus>();
    memoryCensus_->SetDevices(ranges);
    bcoreMemory_->SetMemoryCensus(memoryCensus_.get());
    // A block compiled before now loads and stores RAM without calling
    // EmulatorMemory (see its page_table()): drop them all, so the census
    // sees every access. Nothing to drop before the first translation.
    if (core_->stats().blocks_translated) InvalidateTranslations();
    return *memoryCensus_;
}
#endif

// One service call. In census runs it is counted, and counted as productive
// when it changed an interrupt level, queued an event other than a raise, or
// moved data. The call names D's method, so it is not a virtual call.
template <class D>
void BlackFinCpu::ServiceDevice(D& device, size_t index, int ivg) {
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) {
        auto& source = stats->sources[index];
        const uint64_t before = source.transitions + source.queued;
        const uint64_t work = device.ServiceWork();
        stats->current = static_cast<int>(index);
        device.D::ProcessWithInterrupt(ivg);
        const uint64_t moved = device.ServiceWork() - work;
        source.work += moved;
        ++source.calls;
        if (moved || source.transitions + source.queued != before) {
            ++source.productive;
            ++stats->productiveTotal;
        }
        stats->current = stats->other;
        return;
    }
#endif
    device.D::ProcessWithInterrupt(ivg);
}

// Between blocks, in the order of `devices`. Every other device has no
// ProcessWithInterrupt of its own, and each of these is left out when its call
// would change nothing: publishing a level that is already published does
// nothing since the core's inputs are levels (LatchLines()).
void BlackFinCpu::ServiceDevices(int ivg) {
    auto& s = serviced_;
    if (s.usb->NeedsService()) ServiceDevice(*s.usb, s.usbIndex, ivg);
    if (s.nfc->NeedsService()) ServiceDevice(*s.nfc, s.nfcIndex, ivg);
    if (s.rtc->NeedsTick()) ServiceDevice(*s.rtc, s.rtcIndex, ivg);
    if (s.dma->NeedsService(time_.Nanoseconds())) ServiceDevice(*s.dma, s.dmaIndex, ivg);
    // The core timer only clears TINT, once the CPU is in its IVG.
    if (ivg == IVG_IVTMR && s.coreTimer->InterruptPending())
        ServiceDevice(*s.coreTimer, s.coreTimerIndex, ivg);
    // Without an enabled master transfer, or inside the TWI's IVG, the TWI
    // does nothing (MPROG is set and cleared within the call).
    if (ivg != TWI::kIvg && s.twi->NeedsService(time_.Nanoseconds())) ServiceDevice(*s.twi, s.twiIndex, ivg);
}

void BlackFinCpu::RaiseInterrupt(int ivg) {
#ifdef ENABLE_PROFILING
    // Only a raise that sets a new ILAT bit is something the guest can see;
    // LatchLines() raises only those.
    const uint32_t ilatAddr = CEC_MMR_BASE + 4 * CEC_IDX_ILAT;
    const uint32_t before = cec_mmr_read(cpuState_.get(), ilatAddr);
    if (auto* stats = Service(); stats && ivg >= 0 && ivg < 16) {
        ++stats->raised[ivg];
        ++stats->raisedTotal;
    }
    cec_raise(cpuState_.get(), ivg);
    const uint32_t latched = cec_mmr_read(cpuState_.get(), ilatAddr) & ~before;
    if (auto* stats = Service()) stats->latchedTotal += __builtin_popcount(latched);
    for (int level = 0; level < 16; ++level)
        if (latched >> level & 1) ++irqLatched_[level];
#else
    cec_raise(cpuState_.get(), ivg);
#endif
}

// Runs the events due by now, in due order. One an event queues for now or
// earlier runs in the same call.
void BlackFinCpu::ProcessEvents() {
    const uint64_t now = time_.Nanoseconds();
    if (inboxPending_.load(std::memory_order_acquire)) {
        std::vector<std::tuple<std::chrono::nanoseconds, std::function<void()>>> inbox;
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox.swap(inbox_);
            inboxPending_.store(false, std::memory_order_relaxed);
        }
        for (auto& [delay, event] : inbox)
            ScheduleEvent(std::move(event), now + (delay.count() > 0 ? uint64_t(delay.count()) : 0));
    }
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) {
        const uint64_t length = events_.size();
        ++stats->processCalls;
        stats->nonEmptyCalls += length != 0;
        stats->lengthSum += length;
        if (length > stats->lengthMax) stats->lengthMax = length;
    }
#endif
    if (now < nextEventNs_) return;
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->current = stats->events;
#endif
    while (!events_.empty() && events_.front().dueNs <= now) {
        std::pop_heap(events_.begin(), events_.end(), std::greater<>());
        auto event = std::move(events_.back().run);
        events_.pop_back();
        event();
#ifdef ENABLE_PROFILING
        ++eventsDelivered_;
#endif
    }
    nextEventNs_ = events_.empty() ? UINT64_MAX : events_.front().dueNs;
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->current = stats->other;
#endif
}

void BlackFinCpu::AttachDisplay(const std::shared_ptr<Display>& display) {
    ppi->AttachDisplay(display);
    // Deterministic mode drives TE from guest time (ServicePanel); the
    // frontend only presents pixels. Wall mode keeps the frontend's host poll.
    if (time_.Deterministic()) return;
    display->SetOnFrameStartCallback([this](Display& disp) {
        this->QueueEvent([this]() {
            this->portG->SetPinInput(3, GPIOPinLevel::Low);
        });
        this->QueueEvent([this]() {
            this->portG->SetPinInput(3, GPIOPinLevel::High);
        }, std::chrono::nanoseconds(1000));
    });
}

void BlackFinCpu::AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash) {
    nfc->AttachNandFlash(nandFlash);
}

void BlackFinCpu::AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard) {
    keyboard->SetKeyEventCallback([this](int bank, int index, bool pressed) {
        this->QueueEvent([this, bank, index, pressed]() {
            // Map keyboard events to GPIO expander pins
            gpioExpanders[bank]->SetPinInput(index, pressed ? GPIOPinLevel::Low : GPIOPinLevel::High);
        });
    });
}

void BlackFinCpu::AttachAudioOutput(const std::shared_ptr<AudioOutput>& audioOutput) {
    auto cb = [audioOutput](const void* data, size_t samples, int channels, int bitsPerSample) {
        audioOutput->WriteSamples(data, samples, channels, bitsPerSample);
    };
    sport0->SetAudioOutputCallback(cb);
    sport1->SetAudioOutputCallback(cb);
}

void BlackFinCpu::SetAcceleration(int16_t x, int16_t y, int16_t z) {
    QueueEvent([this, x, y, z]() {
        this->adxl345->SetAcceleration(x, y, z);
    });
}

void BlackFinCpu::SetPotentiometerValue(u8 value) {
    QueueEvent([this, value]() {
        this->potentiometer->SetValue(value);
    });
}

void BlackFinCpu::SetTwiSpeedup(unsigned speedup) {
    serviced_.twi->SetSpeedup(speedup);
}

void BlackFinCpu::SetBootMode(int mode) {
    sic->SetBootMode(mode);
}