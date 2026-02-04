#include "usb.h"
#include "emu.h"
#include "utils/log.h"
#include <cstring>
#include <algorithm>

// TODO: support HOST mode

// FIFO sizes
constexpr const static int USB_EP0_FIFO_SIZE      = 64;
constexpr const static int USB_EP_FIFO_SIZE_SMALL = 128;   // EP0-4
constexpr const static int USB_EP_FIFO_SIZE_LARGE = 1024;  // EP5-7

static constexpr inline int GetMaxFIFOSize(int ep) {
    if (ep == 0) return USB_EP0_FIFO_SIZE;
    if (ep < 5) return USB_EP_FIFO_SIZE_SMALL;
    return USB_EP_FIFO_SIZE_LARGE;
}

USB::USB(u32 baseAddr): RegisterDevice("USB", baseAddr, 0x500) {
    REG32(USB_FADDR, 0x00);
    FIELD(USB_FADDR, FUNCTION_ADDRESS, 0, 7, R(funcAddr), W(funcAddr));

    REG32(USB_POWER, 0x04);
    FIELD(USB_POWER, HS_MODE, 4, 1, R(highSpeedMode), N());
    FIELD(USB_POWER, HS_ENABLE, 5, 1, R(highSpeedEnabled), W(highSpeedEnabled));
    FIELD(USB_POWER, SOFT_CONN, 6, 1, R(connected), W(connected));
    FIELD(USB_POWER, ISO_UPDATE_EN, 7, 1, R(isocUpdateEnabled), W(isocUpdateEnabled));

    REG32(USB_INTRTX, 0x08);
    FIELD(USB_INTRTX, VAL, 0, 8, R(epTxInterrupts), W1C(epTxInterrupts));
    REG32(USB_INTRRX, 0x0C);
    FIELD(USB_INTRRX, VAL, 0, 8, R(epRxInterrupts), W1C(epRxInterrupts));
    REG32(USB_INTRTXE, 0x10);
    FIELD(USB_INTRTXE, VAL, 0, 8, R(epTxIntsEnabled), W(epTxIntsEnabled));
    REG32(USB_INTRRXE, 0x14);
    FIELD(USB_INTRRXE, VAL, 0, 8, R(epRxIntsEnabled), W(epRxIntsEnabled));

    REG32(USB_INTRUSB, 0x18);
    FIELD(USB_INTRUSB, SOF_B, 3, 0, R(sofDetected), W1C(sofDetected));
    REG32(USB_INTRUSBE, 0x1C);
    FIELD(USB_INTRUSBE, VAL, 0, 8, R(commonIntsEnabled), W(commonIntsEnabled));

    REG32(USB_FRAME, 0x20);
    FIELD(USB_FRAME, FRAME_NUMBER, 0, 11, R(frameNumber), W(frameNumber));

    REG32(USB_INDEX, 0x24);
    FIELD(USB_INDEX, SELECTED_ENDPOINT, 0, 4, R(index), W(index));

    REG32(USB_GLOBINTR, 0x2C);
    FIELD(USB_GLOBINTR, USB_INT0_R, 0, 1, R(commonIntsToINT0), W(commonIntsToINT0));
    FIELD(USB_GLOBINTR, RX_INT0_R, 1, 1, R(rxIntsToINT0), W(rxIntsToINT0));
    FIELD(USB_GLOBINTR, TX_INT0_R, 2, 1, R(txIntsToINT0), W(txIntsToINT0));
    FIELD(USB_GLOBINTR, USB_INT1_R, 3, 1, R(commonIntsToINT1), W(commonIntsToINT1));
    FIELD(USB_GLOBINTR, RX_INT1_R, 4, 1, R(rxIntsToINT1), W(rxIntsToINT1));
    FIELD(USB_GLOBINTR, TX_INT1_R, 5, 1, R(txIntsToINT1), W(txIntsToINT1));
    FIELD(USB_GLOBINTR, USB_INT2_R, 6, 1, R(commonIntsToINT2), W(commonIntsToINT2));
    FIELD(USB_GLOBINTR, RX_INT2_R, 7, 1, R(rxIntsToINT2), W(rxIntsToINT2));

    REG32(USB_GLOBAL_CTL, 0x30);
    FIELD(USB_GLOBAL_CTL, GLOBAL_ENA, 0, 1, R(enabled), W(enabled));
    FIELD(USB_GLOBAL_CTL, EP_TX_ENA, 1, 7, R(epTxEnabled >> 1), [this](u32 v) {
        epTxEnabled = (v << 1) | (epTxEnabled & 0x1);
    });
    FIELD(USB_GLOBAL_CTL, EP_RX_ENA, 8, 7, R(epRxEnabled >> 1), [this](u32 v) {
        epRxEnabled = (v << 1) | (epRxEnabled & 0x1);
    });

    REG32(USB_TX_MAX_PACKET, 0x40);
    USB_TX_MAX_PACKET.readCallback = [this]() {
        return registers[0x200 + index * 0x40].Read32();
    };
    USB_TX_MAX_PACKET.writeCallback = [this](u32 value) {
        registers[0x200 + index * 0x40].Write32(value);
    };
    // alias USB_CSR0
    REG32(USB_TXCSR, 0x44);
    USB_TXCSR.readCallback = [this]() {
        return registers[0x204 + index * 0x40].Read32();
    };
    USB_TXCSR.writeCallback = [this](u32 value) {
        registers[0x204 + index * 0x40].Write32(value);
    };
    REG32(USB_RX_MAX_PACKET, 0x48);
    USB_RX_MAX_PACKET.readCallback = [this]() {
        return registers[0x208 + index * 0x40].Read32();
    };
    USB_RX_MAX_PACKET.writeCallback = [this](u32 value) {
        registers[0x208 + index * 0x40].Write32(value);
    };
    REG32(USB_RXCSR, 0x4C);
    USB_RXCSR.readCallback = [this]() {
        return registers[0x20C + index * 0x40].Read32();
    };
    USB_RXCSR.writeCallback = [this](u32 value) {
        registers[0x20C + index * 0x40].Write32(value);
    };
    // alias USB_COUNT0
    REG32(USB_RXCOUNT, 0x50);
    USB_RXCOUNT.readCallback = [this]() {
        return registers[0x210 + index * 0x40].Read32();
    };
    USB_RXCOUNT.writeCallback = [this](u32 value) {
        registers[0x210 + index * 0x40].Write32(value);
    };
    REG32(USB_TXTYPE, 0x54);
    USB_TXTYPE.readCallback = [this]() {
        return registers[0x214 + index * 0x40].Read32();
    };
    USB_TXTYPE.writeCallback = [this](u32 value) {
        registers[0x214 + index * 0x40].Write32(value);
    };
    REG32(USB_RXTYPE, 0x5C);
    USB_RXTYPE.readCallback = [this]() {
        return registers[0x21C + index * 0x40].Read32();
    };
    USB_RXTYPE.writeCallback = [this](u32 value) {
        registers[0x21C + index * 0x40].Write32(value);
    };
    REG32(USB_TXCOUNT, 0x68);
    USB_TXCOUNT.readCallback = [this]() {
        return registers[0x228 + index * 0x40].Read32();
    };
    USB_TXCOUNT.writeCallback = [this](u32 value) {
        registers[0x228 + index * 0x40].Write32(value);
    };

    REG32(USB_DMA_INTERRUPT, 0x400);
    FIELD(USB_DMA_INTERRUPT, VAL, 0, 8, R(dmaInterrupt), W1C(dmaInterrupt));

    REG32(USB_CSR0, 0x204);
    FIELD(USB_CSR0, RXPKTRDY, 0, 1, R(endpoints[0].dataPacketReceived), N());
    FIELD(USB_CSR0, TXPKTRDY, 1, 1, R(endpoints[0].dataPacketInFIFO), W(endpoints[0].dataPacketInFIFO));
    FIELD(USB_CSR0, DATAEND, 3, 1, R(endpoints[0].dataEnded), W(endpoints[0].dataEnded));
    FIELD(USB_CSR0, SERVICED_RXPKTRDY, 6, 1, R(0), W1C(endpoints[0].dataPacketReceived));
    FIELD(USB_CSR0, FLUSHFIFO, 8, 1, R(0), [this](u32 v) {
        if (v) {
            if (endpoints[0].dataPacketReceived) {
                endpoints[0].rxFifo = {};
                endpoints[0].dataPacketReceived = false;
            }
            if (endpoints[0].dataPacketInFIFO) {
                endpoints[0].txFifo = {};
                endpoints[0].dataPacketInFIFO = false;
            }
        }
    });


#undef R
#define R(x) [this, i]() { return x; }
#undef W
#define W(x) [this, i](u32 v) { x = v; }
    // fifo registers
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        REG32(USB_EPx_FIFO, 0x80 + i * 8);
        FIELD(USB_EPx_FIFO, FIFO_DATA, 0, 16, R(ReadFifo(i)), N());
        USB_EPx_FIFO.writeCallback = [this, i](u32 value) {
            WriteFifo(i, (u16)(value & 0xFFFF));
        };
    }

    // ep registers
    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        REG32(USB_EP_NIx_TXMAXP, 0x200 + i * 0x40);
        FIELD(USB_EP_NIx_TXMAXP, MAX_PACKET_SIZE_T, 0, 11, R(endpoints[i].txMaxPacketSize), W(endpoints[i].txMaxPacketSize));

        if (i != 0) {
            REG32(USB_EP_NIx_TXCSR, 0x204 + i * 0x40);
            FIELD(USB_EP_NIx_TXCSR, TXPKTRDY_T, 0, 1, R(endpoints[i].dataPacketInFIFO), W(endpoints[i].dataPacketInFIFO));
            FIELD(USB_EP_NIx_TXCSR, FIFO_NOT_EMPTY_T, 1, 1, R(endpoints[i].txFifo.size() >= endpoints[i].txMaxPacketSize), N());
            FIELD(USB_EP_NIx_TXCSR, FLUSHFIFO_T, 3, 1, R(0), [this](u32 v) {
                if (v) {
                }
            });
            FIELD(USB_EP_NIx_TXCSR, DMAREQMODE_T, 10, 1, R(endpoints[i].dmaMode1Enabled), W(endpoints[i].dmaMode1Enabled));
            FIELD(USB_EP_NIx_TXCSR, DMAREQ_ENA_T, 12, 1, R(endpoints[i].dmaRequestEnabled), W(endpoints[i].dmaRequestEnabled));
            FIELD(USB_EP_NIx_TXCSR, ISO_T, 14, 1, R(endpoints[i].isocTransferEnabled), W(endpoints[i].isocTransferEnabled));
            FIELD(USB_EP_NIx_TXCSR, AUTOSET_T, 15, 1, R(endpoints[i].dataPacketInFIFOAutoSet), W(endpoints[i].dataPacketInFIFOAutoSet));
        }

        REG32(USB_EP_NIx_RXMAXP, 0x208 + i * 0x40);
        FIELD(USB_EP_NIx_RXMAXP, MAX_PACKET_SIZE_R, 0, 11, R(endpoints[i].rxMaxPacketSize), W(endpoints[i].rxMaxPacketSize));

        if (i != 0) {
            REG32(USB_EP_NIx_RXCSR, 0x20C + i * 0x40);
            FIELD(USB_EP_NIx_RXCSR, RXPKTRDY_R, 0, 1, R(endpoints[i].dataPacketReceived), W(endpoints[i].dataPacketReceived));
            FIELD(USB_EP_NIx_RXCSR, FIFO_FULL_R, 1, 1, R(endpoints[i].rxFifo.size() >= GetMaxFIFOSize(i)), N());
            auto flushfifo_w = [this, i](u32 v) {
                if (v) {
                    if (endpoints[i].dataPacketReceived) {
                        endpoints[i].rxFifo = {};
                        endpoints[i].dataPacketReceived = false;
                    }
                }
            };
            FIELD(USB_EP_NIx_RXCSR, FLUSHFIFO_R, 4, 1, R(0), flushfifo_w);
            FIELD(USB_EP_NIx_RXCSR, DMAREQMODE_R, 11, 1, R(endpoints[i].dmaMode1Enabled), W(endpoints[i].dmaMode1Enabled));
            FIELD(USB_EP_NIx_RXCSR, DMAREQ_ENA_R, 13, 1, R(endpoints[i].dmaRequestEnabled), W(endpoints[i].dmaRequestEnabled));
            FIELD(USB_EP_NIx_RXCSR, ISO_R, 14, 1, R(endpoints[i].isocTransferEnabled), W(endpoints[i].isocTransferEnabled));
            FIELD(USB_EP_NIx_RXCSR, AUTOCLEAR_R, 15, 1, R(endpoints[i].dataPacketReceivedAutoClear), W(endpoints[i].dataPacketReceivedAutoClear));
        }

        REG32(USB_EP_NIx_RXCOUNT, 0x210 + i * 0x40);
        FIELD(USB_EP_NIx_RXCOUNT, RX_COUNT, 0, 11, R(endpoints[i].rxFifo.size()), N());

        REG32(USB_EP_NIx_TXTYPE, 0x214 + i * 0x40);
        FIELD(USB_EP_NIx_TXTYPE, PROTOCOL_T, 4, 2, R(endpoints[i].txType), W(endpoints[i].txType));

        REG32(USB_EP_NIx_TXINTERVAL, 0x218 + i * 0x40);
        FIELD(USB_EP_NIx_TXINTERVAL, INTERVAL_T, 0, 8, R(endpoints[i].txInterval), W(endpoints[i].txInterval));

        REG32(USB_EP_NIx_RXTYPE, 0x21C + i * 0x40);
        FIELD(USB_EP_NIx_RXTYPE, PROTOCOL_R, 4, 2, R(endpoints[i].rxType), W(endpoints[i].rxType));

        REG32(USB_EP_NIx_RXINTERVAL, 0x220 + i * 0x40);
        FIELD(USB_EP_NIx_RXINTERVAL, INTERVAL_R, 0, 8, R(endpoints[i].rxInterval), W(endpoints[i].rxInterval));

        REG32(USB_EP_NIx_TXCOUNT, 0x228 + i * 0x40);
        FIELD(USB_EP_NIx_TXCOUNT, TX_COUNT, 0, 11, R(endpoints[i].txCount), W(endpoints[i].txCount));
    }

    // TODO: support DMA transfers
    // dma registers
    for (int i = 0; i < USB_NUM_DMA_CHANNELS; i++) {
        REG32(USB_DMAx_CONTROL, 0x404 + i * 0x20);
        FIELD(USB_DMAx_CONTROL, DMA_ENA, 0, 1, R(dmaChannels[i].enabled), W(dmaChannels[i].enabled));
        FIELD(USB_DMAx_CONTROL, DIRECTION, 1, 1, R(dmaChannels[i].txDirection), W(dmaChannels[i].txDirection));
        FIELD(USB_DMAx_CONTROL, MODE, 2, 1, R(dmaChannels[i].mode1), W(dmaChannels[i].mode1));
        FIELD(USB_DMAx_CONTROL, INT_ENA, 3, 1, R(dmaChannels[i].interruptEnabled), W(dmaChannels[i].interruptEnabled));
        FIELD(USB_DMAx_CONTROL, EP_NUM, 4, 4, R(dmaChannels[i].epnum), W(dmaChannels[i].epnum));

        REG32(USB_DMAx_ADDRESS_LO, 0x408 + i * 0x20);
        FIELD(USB_DMAx_ADDRESS_LO, DMA_ADDR_LOW, 0, 16, R(dmaChannels[i].addr & 0xFFFF), N());
        USB_DMAx_ADDRESS_LO.writeCallback = [this, i](u32 value) {
            dmaChannels[i].addr = (dmaChannels[i].addr & 0xFFFF0000) | (value & 0xFFFF);
        };
        REG32(USB_DMAx_ADDRESS_HI, 0x40C + i * 0x20);
        FIELD(USB_DMAx_ADDRESS_HI, DMA_ADDR_HIGH, 0, 16, R((dmaChannels[i].addr >> 16) & 0xFFFF), N());
        USB_DMAx_ADDRESS_HI.writeCallback = [this, i](u32 value) {
            dmaChannels[i].addr = (dmaChannels[i].addr & 0x0000FFFF) | ((value & 0xFFFF) << 16);
        };

        REG32(USB_DMAx_COUNT_LO, 0x410 + i * 0x20);
        FIELD(USB_DMAx_COUNT_LO, DMA_COUNT_LOW, 0, 16, R(dmaChannels[i].count & 0xFFFF), N());
        USB_DMAx_COUNT_LO.writeCallback = [this, i](u32 value) {
            dmaChannels[i].count = (dmaChannels[i].count & 0xFFFF0000) | (value & 0xFFFF);
        };
        REG32(USB_DMAx_COUNT_HI, 0x414 + i * 0x20);
        FIELD(USB_DMAx_COUNT_HI, DMA_COUNT_HIGH, 0, 16, R((dmaChannels[i].count >> 16) & 0xFFFF), N());
        USB_DMAx_COUNT_HI.writeCallback = [this, i](u32 value) {
            dmaChannels[i].count = (dmaChannels[i].count & 0x0000FFFF) | ((value & 0xFFFF) << 16);
        };
    }
}

void USB::Read(u32 offset, void* buffer, u32 length) {
    // Special handling for EP0 FIFO address + 4
    if (offset == 0x84) {
        auto& ep0 = endpoints[0];
        if (ep0.rxFifo.size() > 0) {
            u8 byte = ep0.rxFifo.front();
            ep0.rxFifo.pop();
            *(u8*)buffer = byte;
        }
        return;
    }
    RegisterDevice::Read(offset, buffer, length);
}

void USB::Write(u32 offset, const void* buffer, u32 length) {
    RegisterDevice::Write(offset, buffer, length);
}

u16 USB::ReadFifo(int ep) {
    auto& endpoint = endpoints[ep];
    u16 value = 0;

    size_t available = endpoint.rxFifo.size();
    if (available > 0) {
        size_t toRead = std::min(available, sizeof(u16));
        for (size_t i = 0; i < toRead; i++) {
            value |= endpoint.rxFifo.front() << (i * 8);
            endpoint.rxFifo.pop();
        }

        if (endpoint.rxFifo.size() == 0 && endpoint.dataPacketReceivedAutoClear) {
            endpoint.dataPacketReceived = false;
        }
    }

    return value;
}

void USB::WriteFifo(int ep, u16 value) {
    auto& endpoint = endpoints[ep];

    // Write up to 2 bytes to FIFO
    size_t currentSize = endpoint.txFifo.size();
    size_t maxSize = GetMaxFIFOSize(ep);
    size_t toWrite = std::min(sizeof(u16), std::min((std::size_t)endpoint.txCount, maxSize - currentSize));

    for (size_t i = 0; i < toWrite; i++) {
        u8 byte = value >> (i * 8);
        endpoint.txFifo.push(byte);
    }
    endpoint.txCount -= toWrite;

    if (endpoint.txFifo.size() >= endpoint.txMaxPacketSize && endpoint.dataPacketInFIFOAutoSet) {
        endpoint.dataPacketInFIFO = true;
    }
}

std::size_t USB::writeHostToDeviceFIFO(int ep, const u8* data, std::size_t length) {
    auto& endpoint = endpoints[ep];
    size_t currentSize = endpoint.rxFifo.size();
    size_t maxSize = GetMaxFIFOSize(ep);
    size_t spaceAvailable = maxSize - currentSize;
    size_t toWrite = std::min(length, spaceAvailable);
    for (size_t i = 0; i < toWrite; i++) {
        endpoint.rxFifo.push(data[i]);
    }
    return toWrite;
}

std::size_t USB::readDeviceToHostFIFO(int ep, u8* data, std::size_t length) {
    auto& endpoint = endpoints[ep];
    size_t available = endpoint.txFifo.size();
    size_t toRead = std::min(length, available);
    for (size_t i = 0; i < toRead; i++) {
        data[i] = endpoint.txFifo.front();
        endpoint.txFifo.pop();
    }
    return toRead;
}

void USB::UpdateInterrupts() {
    bool int0 = false;
    bool int1 = false;
    bool int2 = false;
    // Check USB interrupts
    u32 intrusb = Read32(0x18);
    u32 intrusbe = Read32(0x1C);
    if (intrusb & intrusbe) {
        int0 |= commonIntsToINT0;
        int1 |= commonIntsToINT1;
        int2 |= commonIntsToINT2;
    }

    // Check TX interrupts
    u32 intrtx = Read32(0x08);
    u32 intrtxe = Read32(0x10);
    if (intrtx & intrtxe) {
        int0 |= txIntsToINT0;
        int1 |= txIntsToINT1;
        int2 |= txIntsToINT2;
    }

    // Check RX interrupts
    u32 intrrx = Read32(0x0C);
    u32 intrrxe = Read32(0x14);
    if (intrrx & intrrxe) {
        int0 |= rxIntsToINT0;
        int1 |= rxIntsToINT1;
        int2 |= rxIntsToINT2;
    }

    interruptHandlers[int0Irq](int0Irq, int0 ? 1 : 0);
    interruptHandlers[int1Irq](int1Irq, int1 ? 1 : 0);
    interruptHandlers[int2Irq](int2Irq, int2 ? 1 : 0);
}

void USB::BindInterrupt(int int0, int int1, int int2, int dmaint, InterruptHandler callback) {
    int0Irq = int0;
    int1Irq = int1;
    int2Irq = int2;
    dmaIntIrq = dmaint;

    RegisterDevice::BindInterrupt(int0, callback);
    RegisterDevice::BindInterrupt(int1, callback);
    RegisterDevice::BindInterrupt(int2, callback);
    RegisterDevice::BindInterrupt(dmaint, callback);
}

void USB::HandleSetupPacket(USBSetupBytes setup, const u8* data, std::size_t length, ReplyCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);

    auto& ep0 = endpoints[0];

    size_t maxSize = GetMaxFIFOSize(0);
    if (length + sizeof(USBSetupBytes) > maxSize - ep0.rxFifo.size()) {
        LogError("USB: Setup packet data too large for EP0 FIFO");
        return;
    }

    writeHostToDeviceFIFO(0, reinterpret_cast<const u8*>(&setup), sizeof(USBSetupBytes));
    writeHostToDeviceFIFO(0, data, length);
    setupCallback = callback;
    ep0.dataPacketReceived = true;
    epTxInterrupts |= (1 << 0);
    UpdateInterrupts();
}

void USB::HandleDataWrite(int ep, int interval, const u8* data, std::size_t length, WriteDoneCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);

    auto& endpoint = endpoints[ep];
    size_t maxSize = GetMaxFIFOSize(ep);

    std::size_t toWrite = std::min(length, maxSize - endpoint.rxFifo.size());
    writeHostToDeviceFIFO(ep, data, toWrite);
    endpoint.rxBuffer.insert(endpoint.rxBuffer.end(), data + toWrite, data + length);
    endpoint.dataPacketReceived = true;
    endpoint.rxCallback = callback;
    epRxInterrupts |= (1 << ep);
    UpdateInterrupts();
}

void USB::HandleDataRead(int ep, int interval, std::size_t limit, ReplyCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);

    auto& endpoint = endpoints[ep];
    endpoint.txLimit = limit;
    endpoint.txCallback = callback;
}

void USB::ProcessTransfer() {
    if (!enabled) return;
    std::lock_guard<std::mutex> lock(mutex);

    for (int i = 0; i < USB_NUM_ENDPOINTS; i++) {
        auto& ep = endpoints[i];
        // Process TX endpoints
        if ((epTxEnabled & (1 << i)) && ep.dataPacketInFIFO) {
            ep.txBuffer.reserve(ep.txFifo.size() + ep.txBuffer.size());
            while (!ep.txFifo.empty()) {
                ep.txBuffer.push_back(ep.txFifo.front());
                ep.txFifo.pop();
            }
            ep.dataPacketInFIFO = false;
            epTxInterrupts |= (1 << i);

            if (ep.txCallback && ep.txBuffer.size() >= ep.txLimit) {
                ep.txCallback(ep.txBuffer.data(), ep.txLimit);
                ep.txBuffer.erase(ep.txBuffer.begin(), ep.txBuffer.begin() + ep.txLimit);
                ep.txCallback = nullptr;
            }
        }
        // Process RX endpoints
        if ((epRxEnabled & (1 << i)) && !ep.dataPacketReceived) {
            if (ep.rxBuffer.size() > 0) {
                size_t maxSize = GetMaxFIFOSize(i);
                size_t toWrite = std::min(ep.rxBuffer.size(), maxSize - ep.rxFifo.size());
                writeHostToDeviceFIFO(i, ep.rxBuffer.data(), toWrite);
                ep.rxBuffer.erase(ep.rxBuffer.begin(), ep.rxBuffer.begin() + toWrite);
            }
            if (ep.rxFifo.size() > 0) {
                ep.dataPacketReceived = true;
                epRxInterrupts |= (1 << i);
            } else if (ep.rxCallback) {
                ep.rxCallback();
                ep.rxCallback = nullptr;
            }
        }
    }

    auto& ep0 = endpoints[0];
    if (ep0.dataEnded) {
        if (!setupCallback) {
            LogError("USB: No setup packet handler for ep0 DATAEND");
        } else {
            setupCallback(ep0.txBuffer.data(), ep0.txBuffer.size());
            ep0.txBuffer.clear();
            setupCallback = nullptr;
            ep0.dataEnded = false;
            ep0.dataPacketInFIFO = false;
            epTxInterrupts |= (1 << 0);
        }
    }
    UpdateInterrupts();
}

void USB::ProcessWithInterrupt(int ivg) {
    ProcessTransfer();
}