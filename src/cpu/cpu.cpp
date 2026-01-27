#include "cpu.h"
#include "ebiu.h"
#include "bootrom.h"
#include "twi.h"
#include "sic.h"
#include "otp.h"
#include "gptimer.h"
#include "dma.h"
#include "ppi.h"
#include "gpio.h"
#include "nand.h"
#include "jtag.h"
#include "rtc.h"
#include "sport.h"
#include "simhw.h"
#include "emu.h"
#include "peripheral/mcp230xx.h"
#include "peripheral/adxl345.h"
#include "peripheral/oled.h"
#include "peripheral/display.h"
#include "peripheral/keyboard.h"
#include "peripheral/potentiometer.h"
#include "utils/log.h"

extern "C" {
#include "bfin_sim/sim-main.h"
#include "bfin_sim/bfin-sim.h"
#include "bfin_sim/devices.h"
#include "bfin_sim/dv-bfin_cec.h"
void bfin_trace_queue (SIM_CPU *, bu32 src_pc, bu32 dst_pc, int hwloop) {

}

void bfin_syscall (SIM_CPU *cpu) {

}

void hw_event_queue_deschedule(struct hw *me, struct hw_event *event) {
}

struct hw_event* hw_event_queue_schedule (struct hw *me,
			 int64_t delta_time,
			 hw_event_callback *callback,
			 void *data) {
    BlackFinCpu& self = BlackFinCpu::FromCPU(STATE_CPU(me->system_of_hw, 0));
    self.QueueEvent([data, callback, me]() {
        callback(me, data);
    });
    return (struct hw_event*)-1;
}

unsigned sim_core_read_buffer (SIM_DESC sd,
                sim_cpu *cpu,
                unsigned map,
                void *buffer,
                address_word addr,
                unsigned len) {
    BlackFinCpu& self = BlackFinCpu::FromCPU(cpu);
    self.GetEmulator().MemoryRead(addr, buffer, len);
    return len;
}

unsigned sim_core_write_buffer (SIM_DESC sd,
                sim_cpu *cpu,
                unsigned map,
                const void *buffer,
                address_word addr,
                unsigned len) {
    BlackFinCpu& self = BlackFinCpu::FromCPU(cpu);
    self.GetEmulator().MemoryWrite(addr, buffer, len);
    return len;
}
}  // extern "C"

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
static constexpr int IRQ_RTC = 14;
static constexpr int IRQ_NFC = 48;

BlackFinCpuWrapper::BlackFinCpuWrapper(BlackFinCpu* host)
    : host(host) {
    SIM_CPU* cpu = (SIM_CPU*)calloc(1, sizeof(SIM_CPU));
    bfin_cpu_state* bfin = (bfin_cpu_state*)calloc(1, sizeof(bfin_cpu_state));
    cpu->arch_data = bfin;
    cpu->state = (sim_state*)calloc(1, sizeof(sim_state));
    cpu->state->cpu = cpu;
    cpu->state->environment = OPERATING_ENVIRONMENT;
    cpu->host = host;
    this->cpu = cpu;
}

BlackFinCpuWrapper::~BlackFinCpuWrapper() {
    free(((SIM_CPU*)cpu)->arch_data);
    free(((SIM_CPU*)cpu)->state);
    free(cpu);
}

#define SIM reinterpret_cast<SIM_CPU*>(wrapper.cpu)
#define CPU reinterpret_cast<bfin_cpu_state*>(SIM->arch_data)

BlackFinCpu::BlackFinCpu() : wrapper(this), pc(0) {
    auto irqHandler = [this](int q, int level) { this->ProcessInterrupt(q, level); };
    devices.emplace_back(std::make_shared<MemoryDevice>("PORT_MUX", 0xFFC03200, 0x100));
    devices.emplace_back(std::make_shared<MemoryDevice>("MUSB",     0xFFC03800, 0x500));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A",   0xFF800000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data A Cache", 0xFF804000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B",   0xFF900000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Data B Cache", 0xFF904000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst A",   0xFFA00000, 0x8000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst B",   0xFFA08000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("Inst Cache", 0xFFA10000, 0x4000));
    devices.emplace_back(std::make_shared<MemoryDevice>("SDRAM", 0, 0x8000000));
    devices.emplace_back(std::make_shared<SimEVTDevice>(BFIN_COREMMR_EVT_BASE, SIM->state));
    devices.emplace_back(std::make_shared<SimMMUDevice>(BFIN_COREMMR_MMU_BASE, SIM->state));
    devices.emplace_back(std::make_shared<EBIU>(0xFFC00A00));
    devices.emplace_back(std::make_shared<OTP>(0xFFC03600));
    std::shared_ptr<SPORT> sport0 = std::make_shared<SPORT>(0xFFC00800, 0);
    devices.emplace_back(sport0);
    std::shared_ptr<SPORT> sport1 = std::make_shared<SPORT>(0xFFC00900, 1);
    devices.emplace_back(sport1);
    // OP-1 seems only use last byte of DSPID, which is 0x02 for BF524 rev 02
    devices.emplace_back(std::make_shared<Jtag>(0xFFE05000, 0x02));
    devices.emplace_back(std::make_shared<BootROM>(0xEF000000));
    devices.emplace_back(std::make_shared<GPTimer>(0xFFC00600));
    nfc = std::make_shared<NFC>(0xFFC03700);
    nfc->BindInterrupt(IRQ_NFC, irqHandler);
    devices.emplace_back(nfc);

    std::shared_ptr<RTC> rtc = std::make_shared<RTC>(0xFFC00300);
    rtc->BindInterrupt(IRQ_RTC, irqHandler);
    devices.emplace_back(rtc);

    ppi = std::make_shared<PPI>(0xFFC01000);
    devices.emplace_back(ppi);
    std::shared_ptr<DMA> dma = std::make_shared<DMA>(0xFFC00C00, emulator);
    devices.emplace_back(dma);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralPPI, ppi);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralNFC, nfc);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Rx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT0Tx, sport0);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Rx, sport1);
    dma->AttachDMABus(DMAPeripheralType::DMAPeripheralSPORT1Tx, sport1);
    dma->BindInterrupt(0, IRQ_DMA0, irqHandler);
    dma->BindInterrupt(1, IRQ_DMA1, irqHandler);
    dma->BindInterrupt(2, IRQ_DMA2, irqHandler);
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

    std::shared_ptr<SimCECDevice> cec = std::make_shared<SimCECDevice>(BFIN_COREMMR_CEC_BASE, SIM->state);
    devices.push_back(cec);
    sic = std::make_shared<SIC>(0xFFC00100);
    sic->SetInterruptForwardCallback([this, cec](int ivg, int level) {
        QueueEvent([ivg, level, cec]() {
            cec->RaiseInterrupt(ivg, level);
        });
    });
    devices.push_back(sic);

    std::shared_ptr<TWI> twi = std::make_shared<TWI>(0xFFC01400);
    devices.push_back(twi);

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

    SetRegister(RegIndex::SP, 0x7000000); // Set stack pointer to top of SDRAM
    SetRegister(RegIndex::FP, 0x7000000); // Set frame pointer to top of SDRAM
    CPU->ksp = 0x7000000;
    CPU->usp = 0x7000000;
    CPU->syscfg = 0x30;

    startTime = std::chrono::system_clock::now();
}

BlackFinCpu::~BlackFinCpu() {
}

BlackFinCpu& BlackFinCpu::FromCPU(void* cpu) {
    return *(BlackFinCpu*)(((SIM_CPU*)cpu)->host);
}

void BlackFinCpu::ProcessInterrupt(int pin, int level) {
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

static u64 GetBfinCycles(BlackFinCpuWrapper& wrapper) {
    return ((u64)CPU->cycles[2]) | CPU->cycles[0];
}

static void SetBfinCycles(BlackFinCpuWrapper& wrapper, u64 cycles) {
    CPU->cycles[0] = (u32)(cycles & 0xffffffff);
    CPU->cycles[1] = (u32)(cycles >> 32);
    CPU->cycles[2] = CPU->cycles[1];
}

HaltReason BlackFinCpu::Run() {
    auto microSecondsElapsed = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::system_clock::now() - startTime).count();
    auto cyclesElapsed = microSecondsElapsed * 400; // assuming 400MHz CPU clock
    // Sync cycles with system time
    SetBfinCycles(wrapper, cyclesElapsed);

    CPU->did_jump = false;
    u32 pc = PC();
    u32 len = interp_insn_bfin(SIM, pc);
    if (!CPU->did_jump) {
        SetPC(hwloop_get_next_pc(SIM, pc, len));
    }
    for (int i = 1; i >= 0; --i) {
        if(CPU->lc[i] && pc == CPU->lb[i]) {
	        CPU->lc[i]--;
	        if (CPU->lc[i]) {
	            break;
            }
        }
    }
    int ivg = cec_get_ivg(SIM);
    for (const auto& device : devices) {
        device->ProcessWithInterrupt(ivg);
    }

    ProcessEvents();
    return HaltReason::Break;
}

void BlackFinCpu::SetRegister(int index, u32 value) {
    switch (index)
    {
    case RegIndex::FP:
        CPU->dpregs[15] = value;
        break;
    case RegIndex::SP:
        CPU->dpregs[14] = value;
        break;
    case RegIndex::RETS:
        CPU->rets = value;
        break;
    case RegIndex::R0...RegIndex::R2:
        CPU->dpregs[index - RegIndex::R0] = value;
        break;
    case RegIndex::P1:
        CPU->dpregs[9] = value;
        break;
    default:
        break;
    }
}

u32 BlackFinCpu::GetRegister(int index) {
    switch (index)
    {
    case RegIndex::FP:
        return CPU->dpregs[15];
    case RegIndex::SP:
        return CPU->dpregs[14];
    case RegIndex::RETS:
        return CPU->rets;
    case RegIndex::R0...RegIndex::R2:
        return CPU->dpregs[index - RegIndex::R0];
    case RegIndex::P1:
        return CPU->dpregs[9];
    default:
        return 0;
    }
}

void BlackFinCpu::SetPC(u32 value) {
    CPU->pc = value;
}

u32 BlackFinCpu::PC() {
    return CPU->pc;
}

void BlackFinCpu::QueueEvent(const std::function<void()>& event, std::chrono::nanoseconds delay) {
    std::unique_lock<std::recursive_mutex> lock(eventQueueMutex);
    eventQueue.push_back({delay, event});
}

void BlackFinCpu::ProcessEvents() {
    std::unique_lock<std::recursive_mutex> lock(eventQueueMutex);
    constexpr std::chrono::nanoseconds instructionTime(1); // Approximate instruction execution time
    auto events = eventQueue;
    for (auto& [delay, event] : events) {
        if (delay <= std::chrono::nanoseconds(0)) {
            event();
        }
    }
    // Remove processed events
    auto it = eventQueue.begin();
    while (it != eventQueue.end()) {
        if (std::get<0>(*it) <= std::chrono::nanoseconds(0)) {
            it = eventQueue.erase(it);
        } else {
            std::get<0>(*it) -= instructionTime;
            ++it;
        }
    }
    elapsedTime += instructionTime;
}

void BlackFinCpu::AttachDisplay(const std::shared_ptr<Display>& display) {
    ppi->AttachDisplay(display);
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