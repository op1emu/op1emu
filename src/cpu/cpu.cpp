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
#include <pthread.h>
#include <time.h>
#ifdef __linux__
#include <sched.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <unistd.h>
#elif defined(__APPLE__)
#include <pthread/qos.h>
#endif
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

// bcore's CEC and EVT functions (bcore's src/cec.h, src/evt.h)
namespace bcore {
void cec_raise(CpuState* cpu, uint32_t ivg);
void cec_check_pending(CpuState* cpu);
void evt_init();
}  // namespace bcore

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

// The CEC's MMRs, which bcore keeps (cec_mmr_read/cec_mmr_write): generated
// code reaches them through the device map like any other MMR.
// Note: CEC state is process-global (module-level statics in bcore's cec.cpp).
// Only one BlackFinCpu instance may exist per process.
class BcoreCECDevice : public Device {
public:
    BcoreCECDevice(bcore::CpuState* cpu)
        : Device("CEC", bcore::CEC_MMR_BASE, bcore::CEC_MMR_SIZE), cpu_(cpu) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = bcore::cec_mmr_read(cpu_, baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        bcore::cec_mmr_write(cpu_, baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return bcore::cec_mmr_read(cpu_, baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        bcore::cec_mmr_write(cpu_, baseAddress + offset, value);
    }
    // A read is a load from bcore's state. A write can change what is
    // pending (IMASK) and which high lines the host latches again (ILAT).
    bool HostWorkAfterAccess(u32 offset, bool write) const override { return write; }
private:
    bcore::CpuState* cpu_;
};

// Thin EVT device shim for the same reason.
class BcoreEVTDevice : public Device {
public:
    BcoreEVTDevice()
        : Device("EVT", bcore::EVT_BASE, bcore::EVT_SIZE) {}
    void Read(u32 offset, void* buffer, u32 length) override {
        u32 val = bcore::evt_read(baseAddress + offset);
        memcpy(buffer, &val, std::min(length, (u32)sizeof(val)));
    }
    void Write(u32 offset, const void* buffer, u32 length) override {
        u32 val = 0;
        memcpy(&val, buffer, std::min(length, (u32)sizeof(val)));
        bcore::evt_write(baseAddress + offset, val);
    }
    u32 Read32(u32 offset) override {
        return bcore::evt_read(baseAddress + offset);
    }
    void Write32(u32 offset, u32 value) override {
        bcore::evt_write(baseAddress + offset, value);
    }
};

BlackFinCpu::BlackFinCpu(bool deterministic) : time_(deterministic), pc(0) {
    cpuState_ = std::make_unique<bcore::CpuState>();
    memset(cpuState_.get(), 0, sizeof(bcore::CpuState));

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
    // A write comes after the ticks before it (AdvanceGpTimers).
    gptimer->SetBeforeWrite([this] { AdvanceGpTimers(time_.Cycles()); });
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
    // Every core timer expiry latches IVG6, once: TINT is its status, not a
    // line the core keeps latching while it is set.
    coreTimer->BindExpiry([this] {
#ifdef ENABLE_PROFILING
        ++hostWork_[kWorkExpiry];
#endif
        coreTimerExpired_ = true;
        LatchLines();
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
    bcoreMemory_->SetChainBudget(&cpuState_->chain_budget);
    bcoreMemory_->SetUnstableReads(&cpuState_->unstable_reads);
    // A host request reaches the USB on the USB/IP thread; the CPU thread
    // services it after its next block.
    usb->SetHostRequestCallback([this] { StopChaining(); });
    core_ = std::make_shared<bcore::Core>(cpuState_.get(), bcoreMemory_.get());
    // IR pipeline at O1, backend at CodeGenOpt::Default. Until bcore wired
    // optimize_module into compilation, the IR level only ever affected the
    // IR-dump output, so init(2) compiled with the backend alone. Measured on
    // a boot to the main display with the backend at CodeGenOpt::Less (this
    // exact pairing with Default was not timed): O1 -6% execution for +43%
    // compile; O2 no faster than O1 and +69% compile.
    core_->init(/*opt_level=*/1, /*codegen_level=*/2);
    // bcore's tests and ISA suite verify every module the lifter makes; here
    // the verifier would be about 1% of the JIT's time.
    core_->set_verify(false);
    // Most blocks of a boot run too rarely for the whole pipeline to pay:
    // tier 0 compiles each about four times faster.
    core_->set_tiering(kTierThreshold);
    // And where a block's instructions have stencils, tier 0 copies it
    // together from them: on the boot to the main display 13,500 of 21,300
    // tier-0 blocks, 4 us each where LLVM took about 1 ms, the CPU thread's
    // compile time -47%, the same guest work.
    core_->set_copy_patch(true);

    // Initialize bcore CEC and EVT
    bcore::cec_init();
    bcore::evt_init();

    SetRegister(RegIndex::SP, 0x7000000); // Set stack pointer to top of SDRAM
    SetRegister(RegIndex::FP, 0x7000000); // Set frame pointer to top of SDRAM
    cpuState_->ksp = 0x7000000;
    cpuState_->usp = 0x7000000;
    cpuState_->syscfg = 0x30;
    time_.SetBlockPackets(&cpuState_->packets);
    time_.SetChainedPackets(&cpuState_->chain_packets);

    // Every block the host would not do anything after runs the next itself
    // (SetChaining); the host runs every block at a PC it rejects here.
    core_->set_chain_filter([this](uint32_t pc) {
        if (pc == 0xFFA00000) return false;
#ifdef ENABLE_PROFILING
        if (profiler_ && profiler_->HostOnly(pc)) return false;
#endif
        return true;
    });
    // A chained block that accesses CYCLES sees it advance as this host's
    // Run() writes it: from the packets.
    core_->set_host_cycles(true);
    // An IDLE waits for its wakeup (Wait()) on guest time, but not while one
    // is already asserted (SIC::Wakeup): generated code reads the flag and
    // runs on, as Wait() would.
    sic->SetWakeFlag(&idleWake_);
    core_->set_idle_waits(true, &idleWake_);
#ifndef ENABLE_PROFILING
    // Block runs are counted for the profiler only (Profiler::AfterRun).
    core_->set_count_chained(false);
#endif
    SetChaining(false);
    SetTierBackground(true);
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

static void SetBfinCycles(bcore::CpuState& cpu_state, u64 cycles) {
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
    // Guest time at the start of this block: the packets every earlier block
    // executed and the cycles IDLEs waited (see TimeSource).
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
    // How far the run may chain blocks before the host has work. Another
    // thread stops it by setting the budget to 0 after what it hands the CPU
    // thread (StopChaining()), so look at that after setting it.
    const uint32_t budget = ChainBudget(cpuState_->pc);
    __atomic_store_n(&cpuState_->chain_budget, budget, __ATOMIC_SEQ_CST);
    if (budget && (inboxPending_.load() || serviced_.usb->NeedsService() || offThreadLatch_.exchange(false)))
        StopChaining();
#ifdef ENABLE_PROFILING
    const uint64_t accessStops = bcoreMemory_->ChainStops();
#endif
    // Execute one basic block — bcore updates cpuState_->pc internally.
    // Hardware loops, PC advance, and hwloop counters are all handled by bcore.
    inRun_ = true;
    const bool ran = core_->run(cpuState_->pc);
    inRun_ = false;
    if (!ran) {
        // Translation/JIT failure: PC is unchanged, so retrying would loop
        // forever. Skip device processing and let the host stop the run.
        LogError("JIT execution failed at PC 0x%08x", cpuState_->pc);
        failed_.store(true, std::memory_order_release);
        return HaltReason::Break;
    }
    // Retired, the run's blocks chained after the first included, and
    // cleared so the guest time stops counting them twice
    // (TimeSource::SetBlockPackets, SetChainedPackets).
    const uint32_t packets = cpuState_->chain_packets + cpuState_->packets;
    const uint32_t chained = cpuState_->chain_blocks;
    cpuState_->packets = 0;
    cpuState_->chain_packets = 0;
    cpuState_->chain_blocks = 0;
    time_.Retire(packets);
#ifdef ENABLE_PROFILING
    if (profiler_) profiler_->AfterRun(blockPc, packets, chained);
    CountChain(budget, packets, chained, accessStops);
    // Dry run: the block would have run the next one itself, so the host
    // must have nothing to do now.
    const bool would = cpuState_->chain_would;
    const auto workBefore = hostWork_;
#else
    (void)chained;
    const bool would = false;
#endif
    cpuState_->did_jump = false; // Clear jump flag set by bcore, since we handle it in the emulator loop
    HostWork(would);
    // An IDLE waits for its wakeup (Wait()), unless HostWork() just took an
    // interrupt: that was it (it cleared the pending flag Wait() looks at).
    if (cpuState_->idle && !cpuState_->did_jump) Wait();
#ifdef ENABLE_PROFILING
    if (would) {
        bool violated = false;
        for (size_t kind = 0; kind < kWorkKinds; ++kind) {
            if (hostWork_[kind] == workBefore[kind]) continue;
            ++chainCounts_.violation[kind];
            violated = true;
        }
        chainCounts_.violations += violated;
    }
#endif
    return HaltReason::Break;
}

// The host's work between blocks, after one or after an IDLE's wait.
void BlackFinCpu::HostWork(bool would) {
    // Real-time mode: every millisecond of guest time or so, a guest running
    // ahead of the host's clock waits for it. Before the devices and events,
    // which then see what another thread handed in meanwhile.
    if (!time_.Deterministic() && time_.Cycles() >= nextPaceCycles_) {
        Pace(time_.Nanoseconds());
        nextPaceCycles_ = time_.Cycles() + kPaceCycles;
    }
#ifdef ENABLE_PROFILING
    if (*cpuState_->cec_pending) ++hostWork_[kWorkCec];
#endif
    bcore::cec_check_pending(cpuState_.get());

    // Get active IVG from CEC
    int ivg = bcore::cec_current_ivg();
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->current = stats->other;
#endif
    ServiceDevices(ivg);
#ifdef ENABLE_PROFILING
    if (would) CheckSkippedServices(ivg);
#else
    (void)would;
#endif
    // The guest time at the end of the block: timers count to it.
    const uint64_t blockEnd = time_.Cycles();
    coreTimer->Advance(blockEnd);
    AdvanceGpTimers(blockEnd);

    ServicePanel();
    ProcessEvents();
    // Lines still high get latched again, and a latched interrupt is taken
    // before the next block.
    LatchLines();
#ifdef ENABLE_PROFILING
    if (*cpuState_->cec_pending) ++hostWork_[kWorkCec];
#endif
    bcore::cec_check_pending(cpuState_.get());
}

// Before a run: how many packets it may retire before the host has work
// between blocks. The host acts after a block when a device it services has
// work (an access, a transfer, a due time), when an event, the panel's TE,
// the core timer or a GP timer tick is due, or when the CEC changed: the
// accesses and the CEC set the budget to 0 as they happen (EmulatorMemory,
// bcore), and the rest are guest times known now. A block that ends at or
// past the earliest of them returns, so the host does all of it after the
// same block as it would after every block.
uint32_t BlackFinCpu::ChainBudget(uint32_t pc) {
    if (!chaining_) return 0;
#ifdef ENABLE_PROFILING
    chainReason_ = kChainHostOnly;
    if (profiler_ && !profiler_->ChainingAllowed(pc)) return 0;
#endif
    // The host drops every translation there (Run()).
    if (pc == 0xFFA00000) return 0;
#ifdef ENABLE_PROFILING
    chainReason_ = kChainService;
#endif
    // Constant while the run chains: an IVG change returns.
    const int ivg = bcore::cec_current_ivg();
    if (HostHasWork(ivg)) return 0;
#ifdef ENABLE_PROFILING
    chainReason_ = kChainDue;
#endif
    // Packets are cycles while a run lasts: it retires one per cycle.
    const uint64_t due = DueCycles(ivg), now = time_.Cycles();
    return due <= now ? 0 : uint32_t(std::min<uint64_t>(due - now, kMaxChainPackets));
}

// The GP timers' ticks up to guest cycle `cycles`: after a block, and before
// a write to their registers, so that a write comes after the ticks before it
// whenever the host works.
void BlackFinCpu::AdvanceGpTimers(uint64_t cycles) {
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
    // FIXME: SCLK and TACLK still share one rate, which is not how they
    // relate on real hardware. TMRCLK has its own (SettleTmrclk).
    uint64_t ticks = (cycles - lastTimerCycles_) / kCyclesPerTimerTick;
    if (ticks) {
        const uint64_t from = lastTimerCycles_;
        uint64_t dropped = 0;
        lastTimerCycles_ += ticks * kCyclesPerTimerTick;
        if (ticks > kMaxCatchupTicks) {
#ifdef ENABLE_PROFILING
            if (auto* stats = Service()) stats->timerTicksDropped += ticks - kMaxCatchupTicks;
#endif
            dropped = ticks - kMaxCatchupTicks;
            ticks = kMaxCatchupTicks;
        }
#ifdef ENABLE_PROFILING
        if (auto* stats = Service()) stats->timerTicks += ticks;
        ++hostWork_[kWorkTimerTick];
#endif
        for (uint64_t i = 0; i < ticks; ++i) {
            // An SCLK tick can change TMRCLK's clock (TIMER7 takes a new
            // period or stops): TMRCLK's time before it, at the rate before.
            SettleTmrclk(from + (dropped + i + 1) * kCyclesPerTimerTick);
            gptimer->Tick(GPTimerClockTypeSCLK);
            gptimer->Tick(GPTimerClockTypeTACLK);
        }
    }
    SettleTmrclk(cycles);
}

uint64_t BlackFinCpu::TmrclkCycles() const { return uint64_t(gptimer->TmrclkSclks()) * kCyclesPerSclk; }

// TMRCLK runs at MHz: its ticks up to guest cycle `cycles`, at once, at the
// rate since the last settle. The rate changes only at an SCLK tick (TIMER7
// takes a new period or stops) or a register write, which settle first, so
// what the timers count does not depend on when the host works. With no
// clock TMRCLK does not count, and starts from where one starts.
void BlackFinCpu::SettleTmrclk(uint64_t cycles) {
    if (cycles <= lastTmrclkCycles_) return;
    const uint64_t per = TmrclkCycles();
    if (!per) {
        lastTmrclkCycles_ = cycles;
        return;
    }
    if (const uint64_t n = (cycles - lastTmrclkCycles_) / per) {
        lastTmrclkCycles_ += n * per;
        gptimer->Advance(GPTimerClockTypeTMRCLK, n);
    }
}

// A device the host services between blocks has work now.
bool BlackFinCpu::HostHasWork(int ivg) const {
    auto& s = serviced_;
    const uint64_t nowNs = time_.Nanoseconds();
    return inboxPending_.load(std::memory_order_relaxed) || s.usb->NeedsService() || s.nfc->NeedsService() ||
           s.rtc->NeedsTick() || s.dma->NeedsService(nowNs) ||
           (ivg == IVG_IVTMR && s.coreTimer->InterruptPending()) ||
           (ivg != TWI::kIvg && s.twi->NeedsService(nowNs));
}

// The guest cycle of the host's next work between blocks (a device's, an
// event's, the GP timers' tick, the core timer's expiry), the earliest of
// them.
uint64_t BlackFinCpu::DueCycles(int ivg) {
    auto& s = serviced_;
    const uint64_t dmaNs = s.dma->IdleUntilNs();
    uint64_t dueNs = std::min({nextEventNs_, panel_.DeadlineNs(), dmaNs, time_.DueNs(s.rtc->NextChange())});
    if (ivg != TWI::kIvg) dueNs = std::min(dueNs, s.twi->NextByteNs());
    uint64_t tick = lastTimerCycles_ + kCyclesPerTimerTick;
    // A TMRCLK timer's next end of period or width, where its TRUN or
    // interrupt changes.
    if (const uint64_t per = TmrclkCycles()) {
        const uint64_t ticks = gptimer->TicksToChange(GPTimerClockTypeTMRCLK);
        if (ticks != UINT64_MAX) tick = std::min(tick, lastTmrclkCycles_ + ticks * per);
    }
    uint64_t due = std::min(TimeSource::CyclesAt(dueNs), tick);
    const auto expiry = s.coreTimer->NextExpiry();
    if (expiry) due = std::min(due, *expiry);
#ifdef ENABLE_PROFILING
    if (profiler_)
        if (const uint64_t end = profiler_->EndCycles()) due = std::min(due, end);
    // What set it, for the [due] census of deadline returns. Ties go to the
    // first source in this order.
    const uint64_t now = time_.Cycles();
    if (due > now && due - now > kMaxChainPackets) dueSource_ = kDueCap;
    else if (profiler_ && profiler_->EndCycles() == due) dueSource_ = kDueEnd;
    else if (expiry && *expiry == due) dueSource_ = kDueCoreTimer;
    else if (tick == due) dueSource_ = kDueTick;
    else if (TimeSource::CyclesAt(dmaNs) == due) dueSource_ = kDueDma + s.dma->IdleSource();
    else if (TimeSource::CyclesAt(nextEventNs_) == due) dueSource_ = kDueEvent;
    else if (TimeSource::CyclesAt(panel_.DeadlineNs()) == due) dueSource_ = kDuePanel;
    else if (TimeSource::CyclesAt(time_.DueNs(s.rtc->NextChange())) == due) dueSource_ = kDueRtc;
    else dueSource_ = kDueTwi;
#endif
    return due;
}

// After a block that ended at an IDLE (bcore's CpuState::idle): the core
// waits for a wakeup, and nothing runs until the host's next work is due
// (DueCycles), so guest time goes there at once (in real-time mode, once the
// host's clock is about there: Pace()). CYCLES counts through it,
// as through a plain IDLE on hardware (only sleep mode stops CCLK), since it
// follows the guest cycles (SetBfinCycles). The host then does that work, at
// that time and before the next packet, as after a block that ended there.
// The guest runs on past the IDLE: a wakeup that took no interrupt finds it
// in its idle loop, which waits again.
//
// Not where the wakeup is now: an interrupt pending, or a system interrupt
// asserted that SIC_IWR lets wake the core whether or not
// SIC_IMASK passes it (the boot ROM reads NAND a byte at a time that way,
// polling NFC_IRQSTAT after each IDLE), or host work due now. Nor for longer
// than a run could chain (kMaxChainPackets), so that a wakeup the host does
// not time (a PLL relock, which no device here models) comes as late as
// after a capped run.
void BlackFinCpu::Wait() {
    const int ivg = bcore::cec_current_ivg();
    if (HostHasWork(ivg) || *cpuState_->cec_pending || sic->Wakeup()) return;
    const uint64_t now = time_.Cycles();
    uint64_t due = std::min(DueCycles(ivg), now + kMaxChainPackets);
    if (due <= now) return;
    // Real-time mode: no further than the host's clock allows, and on work
    // another thread hands in meanwhile no further than the clock is: the
    // guest sees a key about when it came, and runs on past the IDLE.
    if (!time_.Deterministic()) {
        const uint64_t dueNs = due * 5 / 2;
        const uint64_t until = Pace(dueNs);
        if (until < dueNs) due = std::max(now, TimeSource::CyclesAt(until));
    }
    time_.Idle(due - now);
#ifdef ENABLE_PROFILING
    ++idleWaits_;
    idleCycles_ += due - now;
    ++idleBy_[due == now + kMaxChainPackets ? kDueCap : dueSource_];
#endif
    HostWork(false);
}

// Real-time mode: before guest time goes on to targetNs, the host's clock
// must be no more than RealtimePacer::kAheadNs behind it. When it is, sleep
// until the clock is there, so that the next sleep comes about kAheadNs of
// guest time later, in slices: work from another thread (a key, USB) ends
// the sleep. Returns how far guest time may go: targetNs, or on such work the
// host clock's guest time if that is less. Guest time goes at the audio
// output's rate.
uint64_t BlackFinCpu::Pace(uint64_t targetNs) {
    auto wallNs = [] {
        return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch()).count());
    };
    const uint64_t wall = wallNs();
    if (audioOutput_) pacer_.SetRate(audioOutput_->GuestRate(wall), wall);
    pacer_.Sync(time_.Nanoseconds(), wall);
    uint64_t clockNs = pacer_.GuestNsAt(wall);
    if (targetNs <= clockNs + RealtimePacer::kAheadNs) return targetNs;
    while (clockNs < targetNs) {
        if (OffThreadWork()) return clockNs;
        std::this_thread::sleep_for(std::chrono::nanoseconds(std::min(targetNs - clockNs, RealtimePacer::kSliceNs)));
        clockNs = pacer_.GuestNsAt(wallNs());
    }
    return targetNs;
}

// Pace()'s caller then does it: ProcessEvents() takes the inbox,
// ServiceDevices() the USB request, LatchLines() the line another thread
// raised (so that flag is taken here). Not the guest's own USB accesses,
// which would end every sleep while it polls the USB.
bool BlackFinCpu::OffThreadWork() {
    return inboxPending_.load(std::memory_order_acquire) || serviced_.usb->HostRequestPending() ||
           offThreadLatch_.exchange(false);
}

void BlackFinCpu::SetChaining(bool chaining, bool dry) {
    chaining_ = chaining;
    core_->set_chaining(chaining, dry);
}

void BlackFinCpu::SetLoopBlocks(bool loop) { core_->set_loop_blocks(loop); }

void BlackFinCpu::SetSteadySkip(int mode) { core_->set_steady_skip(mode); }

void BlackFinCpu::SetTiering(uint32_t threshold, uint32_t sizeRef) { core_->set_tiering(threshold, sizeRef); }

void BlackFinCpu::SetLightTier1(bool light) { core_->set_light_tier1(light); }

void BlackFinCpu::SetCopyPatch(bool enable) { core_->set_copy_patch(enable); }

void BlackFinCpu::SetTierBackground(bool background, uint64_t publishAfter, int jitCpu) {
    if (!publishAfter) publishAfter = kTierPublishAfter;
    workerClockSet_.store(false, std::memory_order_release);  // the worker it names stops here
    core_->set_tier_background(background, time_.Deterministic() ? publishAfter : 0, [this, jitCpu] {
        // Behind the CPU thread, whose time is the guest's.
#ifdef __linux__
        setpriority(PRIO_PROCESS, static_cast<id_t>(syscall(SYS_gettid)), 10);
        if (jitCpu >= 0) {
            cpu_set_t set;
            CPU_ZERO(&set);
            CPU_SET(jitCpu, &set);
            if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
                LogError("Cannot pin the JIT worker to CPU %d", jitCpu);
        }
#else
#ifdef __APPLE__
        pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
#endif
        if (jitCpu >= 0) LogError("--jit-cpu: this platform cannot pin a thread to a CPU");
#endif
#ifdef __linux__
        clockid_t clock;
        if (pthread_getcpuclockid(pthread_self(), &clock) == 0) {
            workerClock_.store(clock, std::memory_order_relaxed);
            workerClockSet_.store(true, std::memory_order_release);
        }
#endif
    });
}

int64_t BlackFinCpu::JitWorkerCpuNs() const {
#ifdef __linux__
    if (!workerClockSet_.load(std::memory_order_acquire)) return -1;
    timespec ts{};
    if (clock_gettime(workerClock_.load(std::memory_order_relaxed), &ts) != 0) return -1;
    return int64_t(ts.tv_sec) * 1000000000 + ts.tv_nsec;
#else
    return -1;
#endif
}

void BlackFinCpu::SetDmaSettling(bool on) { serviced_.dma->SetSettling(on); }

uint64_t BlackFinCpu::SteadyIterations() const { return cpuState_->steady_iterations; }

void BlackFinCpu::SetIdleWaits(bool waits) { core_->set_idle_waits(waits, &idleWake_); }

#ifdef ENABLE_PROFILING
void BlackFinCpu::SetProfiler(Profiler* profiler) {
    profiler_ = profiler;
    bcoreMemory_->SetProfiler(profiler);
}

BcoreStats BlackFinCpu::CoreStats() const { return core_->stats(); }

void BlackFinCpu::SetCoreEventSink(BcoreEventSink* sink) { core_->setEventSink(sink); }

void BlackFinCpu::SetPerfJitdump(bool enable) { core_->set_perf_jitdump(enable); }

const char* BlackFinCpu::DueSourceName(unsigned source) {
    static const char* const kNames[] = {"cap", "end", "coretimer", "tick", "event", "panel", "rtc", "twi"};
    static const char* const kDma[] = {"dma_ppi", "dma_hostdp", "dma_nfc", "dma_sport0rx", "dma_sport0tx",
                                       "dma_sport1rx", "dma_sport1tx", "dma_spi", "dma_uart0rx", "dma_uart0tx",
                                       "dma_uart1rx", "dma_uart1tx", "dma_mdmad0", "dma_mdmas0", "dma_mdmad1",
                                       "dma_mdmas1"};
    return source < kDueDma ? kNames[source] : source < kDueSources ? kDma[source - kDueDma] : "?";
}

// Why the run returned to the host.
void BlackFinCpu::CountChain(uint32_t budget, uint32_t retired, uint32_t chained, uint64_t accessStops) {
    auto& c = chainCounts_;
    ++c.runs;
    c.chained += chained;
    if (cpuState_->chain_would) ++c.would;
    else if (!budget) ++(chainReason_ == kChainHostOnly ? c.hostOnly : c.service);
    else if (retired >= budget) ++c.deadline, ++c.deadlineBy[dueSource_];
    else if (bcoreMemory_->ChainStops() != accessStops) ++c.access;
    else if (!cpuState_->chain_budget) ++c.cec;
    else if (*cpuState_->cec_pending) ++c.pending;
    else ++c.miss;
}
#endif

void BlackFinCpu::ServicePanel() {
    if (auto low = panel_.Service(time_.Nanoseconds())) {
#ifdef ENABLE_PROFILING
        ++hostWork_[kWorkPanel];
#endif
        portG->SetPinInput(3, *low ? GPIOPinLevel::Low : GPIOPinLevel::High);
    }
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
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox_.emplace_back(delay, event);
            inboxPending_.store(true);
        }
        StopChaining();
    }
#ifdef ENABLE_PROFILING
    if (auto* stats = Service()) stats->Queued();
#endif
}

// From any thread: the CPU's current run returns to the host after its block.
// Another thread sets what the host is to do first, then calls this; the CPU
// thread sets the budget, then looks at what other threads set (sequentially
// consistent both ways, so one of them sees the other).
void BlackFinCpu::StopChaining() {
    __atomic_store_n(&cpuState_->chain_budget, 0, __ATOMIC_SEQ_CST);
}

void BlackFinCpu::ScheduleEvent(std::function<void()> event, uint64_t dueNs) {
    // Queued inside a run, by a device access: the run's budget did not
    // count it, so the host takes over after the block.
    if (inRun_) StopChaining();
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
    const u16 lines = sic->Lines();
    if (!lines && !coreTimerExpired_) return;
    // Another thread (the USB/IP loop) must not touch the CEC; the CPU thread
    // latches the line after its next block.
    if (std::this_thread::get_id() != cpuThread_.load(std::memory_order_relaxed)) {
        offThreadLatch_.store(true);
        StopChaining();
        return;
    }
    const uint32_t ilat = bcore::cec_mmr_read(cpuState_.get(), bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_ILAT);
    const uint32_t ipend = bcore::cec_mmr_read(cpuState_.get(), bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_IPEND);
    u16 latch = lines & ~(ilat | ipend);
    if (coreTimerExpired_) latch |= 1u << IVG_IVTMR;  // an edge: latched even while IVG6 runs
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
    coreTimerExpired_ = false;
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
    ++hostWork_[kWorkService];
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

#ifdef ENABLE_PROFILING
// Dry run, after a block that would have chained: the devices whose accesses
// keep chaining when they leave a service nothing to do (NFC, DMA) are
// serviced anyway, and must come out of it as they went in. A run that
// chains skips these calls; the check says that skipping them was right.
void BlackFinCpu::CheckSkippedServices(int ivg) {
    auto& s = serviced_;
    bool changed = false;
    if (!s.nfc->NeedsService()) {
        const u32 before = s.nfc->ServiceState();
        s.nfc->ProcessWithInterrupt(ivg);
        changed |= s.nfc->ServiceState() != before;
    }
    if (!s.dma->NeedsService(time_.Nanoseconds())) {
        const uint64_t work = s.dma->ServiceWork(), idle = s.dma->IdleUntilNs();
        s.dma->ProcessWithInterrupt(ivg);
        changed |= s.dma->ServiceWork() != work || s.dma->IdleUntilNs() != idle;
    }
    if (changed) ++hostWork_[kWorkQuiet];
}
#endif

void BlackFinCpu::RaiseInterrupt(int ivg) {
#ifdef ENABLE_PROFILING
    ++hostWork_[kWorkLatch];
    // Only a raise that sets a new ILAT bit is something the guest can see;
    // LatchLines() raises only those.
    const uint32_t ilatAddr = bcore::CEC_MMR_BASE + 4 * bcore::CEC_IDX_ILAT;
    const uint32_t before = bcore::cec_mmr_read(cpuState_.get(), ilatAddr);
    if (auto* stats = Service(); stats && ivg >= 0 && ivg < 16) {
        ++stats->raised[ivg];
        ++stats->raisedTotal;
    }
    bcore::cec_raise(cpuState_.get(), ivg);
    const uint32_t latched = bcore::cec_mmr_read(cpuState_.get(), ilatAddr) & ~before;
    if (auto* stats = Service()) stats->latchedTotal += __builtin_popcount(latched);
    for (int level = 0; level < 16; ++level)
        if (latched >> level & 1) ++irqLatched_[level];
#else
    bcore::cec_raise(cpuState_.get(), ivg);
#endif
}

// Runs the events due by now, in due order. An event that queues another
// for now or earlier runs it in the same call.
void BlackFinCpu::ProcessEvents() {
    const uint64_t now = time_.Nanoseconds();
    if (inboxPending_.load(std::memory_order_acquire)) {
        std::vector<std::tuple<std::chrono::nanoseconds, std::function<void()>>> inbox;
        {
            std::lock_guard<std::mutex> lock(inboxMutex_);
            inbox.swap(inbox_);
            inboxPending_.store(false, std::memory_order_relaxed);
        }
#ifdef ENABLE_PROFILING
        ++hostWork_[kWorkEvent];
#endif
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
#ifdef ENABLE_PROFILING
        ++hostWork_[kWorkEvent];
#endif
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
    // The frontend only presents pixels: TE comes from guest time
    // (ServicePanel).
    ppi->AttachDisplay(display);
}

void BlackFinCpu::AttachNandFlash(const std::shared_ptr<NandFlash>& nandFlash) {
    nfc->AttachNandFlash(nandFlash);
}

void BlackFinCpu::AttachKeyboard(const std::shared_ptr<Keyboard>& keyboard) {
    auto key = [this](int bank, int index, bool pressed) {
        return [this, bank, index, pressed]() {
            // Map keyboard events to GPIO expander pins
            gpioExpanders[bank]->SetPinInput(index, pressed ? GPIOPinLevel::Low : GPIOPinLevel::High);
        };
    };
    keyboard->SetKeyEventCallback([this, key](int bank, int index, bool pressed) {
        this->QueueEvent(key(bank, index, pressed));
    });
    keyboard->SetTimedKeyEventCallback([this, key](int bank, int index, bool pressed, uint64_t delayNs) {
        this->QueueEvent(key(bank, index, pressed), std::chrono::nanoseconds(std::max<uint64_t>(delayNs, 1)));
    });
}

void BlackFinCpu::AttachAudioOutput(const std::shared_ptr<AudioOutput>& audioOutput) {
    audioOutput_ = audioOutput;
    auto cb = [audioOutput](const void* data, size_t samples, int channels, int bitsPerSample, bool stereo) {
        audioOutput->WriteSamples(data, samples, channels, bitsPerSample, stereo);
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