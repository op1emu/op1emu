#pragma once

#include "io.h"
#include <functional>
#include <queue>
#include <mutex>
#include <vector>
#include <queue>
#include <memory>

struct __attribute__ ((__packed__)) USBSetupBytes {
    union {
        struct {
            union {
                struct {
                    u8 bmRequestType;
                    u8 bRequest;
                };
                u16 wRequestAndType;
            };
            u16 wValue;
            u16 wIndex;
            u16 wLength;
        };
        struct {
            u32 bytes0;
            u32 bytes1;
        };
    };
};

class USBDevice {
public:
    virtual ~USBDevice() {}

    using ReplyCallback = std::function<void(const u8*, std::size_t)>;
    using WriteDoneCallback = std::function<void()>;

    virtual void HandleSetupPacket(USBSetupBytes setup, const u8* data, std::size_t length, ReplyCallback callback) = 0;
    virtual void HandleDataWrite(int ep, int interval, const u8* data, std::size_t length, WriteDoneCallback callback) = 0;
    virtual void HandleDataRead(int ep, int interval, std::size_t limit, ReplyCallback callback) = 0;
};


// Number of endpoints
#define USB_NUM_ENDPOINTS    8
#define USB_NUM_DMA_CHANNELS 8

// Endpoint state
struct USBEndpoint {
    u8 txType = 0;
    u8 txInterval = 0;
    u8 rxType = 0;
    u8 rxInterval = 0;

    std::queue<uint8_t> txFifo;
    std::queue<uint8_t> rxFifo;

    std::vector<uint8_t> txBuffer;
    std::size_t txLimit = 0;
    USBDevice::ReplyCallback txCallback = nullptr;
    std::vector<uint8_t> rxBuffer;
    USBDevice::WriteDoneCallback rxCallback = nullptr;

    u16 txCount = 0;

    u16 txMaxPacketSize = 0;
    bool dataPacketReceived = false;
    bool dataPacketInFIFO = false;
    bool dataEnded = false;
    bool dmaMode1Enabled = false;
    bool dmaRequestEnabled = false;
    bool isocTransferEnabled = false;
    bool dataPacketInFIFOAutoSet = false;
    bool dataPacketReceivedAutoClear = false;
    u16 rxMaxPacketSize = 0;
};

// DMA Channel state
struct USBDMAChannel {
    u16 ctrl = 0;
    u32 addr = 0;
    u32 count = 0;
    bool enabled = false;
    bool txDirection = false; // true=TX, false=RX
    bool interruptEnabled = false;
    bool mode1 = false;
    u8 epnum = 0;
};

class USB: public RegisterDevice, public USBDevice {
public:
    USB(u32 baseAddr);

    void BindInterrupt(int int0, int int1, int int2, int dmaint, InterruptHandler callback);

    void Read(u32 offset, void* buffer, u32 length) override;
    void Write(u32 offset, const void* buffer, u32 length) override;

    void HandleSetupPacket(USBSetupBytes setup, const u8* data, std::size_t length, ReplyCallback callback) override;
    void HandleDataWrite(int ep, int interval, const u8* data, std::size_t length, WriteDoneCallback callback) override;
    void HandleDataRead(int ep, int interval, std::size_t limit, ReplyCallback callback) override;

    void ProcessWithInterrupt(int ivg) override;

protected:
    void UpdateInterrupts();
    void ProcessTransfer();

    u16 ReadFifo(int ep);
    void WriteFifo(int ep, u16 value);

    std::size_t writeHostToDeviceFIFO(int ep, const u8* data, std::size_t length);
    std::size_t readDeviceToHostFIFO(int ep, u8* data, std::size_t length);

protected:
    // Connection state
    bool connected = false;

    // Common registers
    u8 funcAddr = 0;
    bool highSpeedMode = false;
    bool highSpeedEnabled = true;
    bool isocUpdateEnabled = false;
    u8 epTxInterrupts = 0;
    u8 epTxIntsEnabled = 0xFF;
    u8 epRxInterrupts = 0;
    u8 epRxIntsEnabled = 0xFF;

    bool sofDetected = false;
    u8 commonIntsEnabled = 0x06;

    u16 frameNumber = 0;

    bool commonIntsToINT0 = true;
    bool rxIntsToINT0 = false;
    bool txIntsToINT0 = false;
    bool commonIntsToINT1 = false;
    bool rxIntsToINT1 = true;
    bool txIntsToINT1 = false;
    bool commonIntsToINT2 = false;
    bool rxIntsToINT2 = false;
    bool txIntsToINT2 = true;

    bool enabled = false;
    u8 epTxEnabled = 0x1;
    u8 epRxEnabled = 0x1;

    u8 index = 0;

    int int0Irq = 0;
    int int1Irq = 0;
    int int2Irq = 0;
    int dmaIntIrq = 0;

    // Endpoints
    USBEndpoint endpoints[USB_NUM_ENDPOINTS];
    // DMA
    u8 dmaInterrupt = 0;
    USBDMAChannel dmaChannels[USB_NUM_DMA_CHANNELS];

    ReplyCallback setupCallback = nullptr;
    std::mutex mutex;
};
