#pragma once

#include "cpu/usb.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace op1::storage {

// The small, device-independent half of the in-process USB host. It owns no
// class-specific policy: MSC uses synchronous transfers, while a future MIDI
// class can build asynchronous streaming on the same descriptor/transfer API.
class UsbHost {
public:
    using Pump = std::function<bool()>;
    using Clock = std::function<uint64_t()>;

    struct Endpoint {
        uint8_t address = 0;
        uint8_t attributes = 0;
        uint16_t maxPacketSize = 0;
        uint8_t interval = 0;
    };

    struct Interface {
        uint8_t number = 0;
        uint8_t alternate = 0;
        uint8_t classCode = 0;
        uint8_t subclass = 0;
        uint8_t protocol = 0;
        std::vector<Endpoint> endpoints;
    };

    struct Device {
        uint16_t vendor = 0;
        uint16_t product = 0;
        uint8_t configuration = 0;
        std::vector<Interface> interfaces;
    };

    UsbHost(USBDevice& device, Pump pump, Clock clock = {});

    bool Enumerate(Device& result);
    bool ControlIn(uint8_t request, uint16_t value, uint16_t index, std::vector<uint8_t>& data,
                   size_t length);
    bool ControlOut(uint8_t request, uint16_t value, uint16_t index,
                    const std::vector<uint8_t>& data = {});
    bool BulkRead(int endpoint, std::vector<uint8_t>& data, size_t length);
    bool BulkWrite(int endpoint, const uint8_t* data, size_t length);

    bool Connected() const;
    // True only when no callback from an older transfer is outstanding and
    // EP0 is idle. Called at top level, never from a USB completion callback.
    bool Quiescent();
    const std::string& Error() const { return error_; }
    void ClearError() { error_.clear(); }

private:
    struct Transfer;
    bool Run(const std::shared_ptr<Transfer>& transfer, const std::function<void()>& start);
    void Reap();
    bool Fail(const std::string& message);

    USBDevice& device_;
    Pump pump_;
    Clock clock_;
    std::vector<std::shared_ptr<Transfer>> transfers_;
    bool active_ = false;
    std::string error_;
};

const UsbHost::Interface* FindInterface(const UsbHost::Device& device, uint8_t classCode,
                                        uint8_t subclass, uint8_t protocol);
const UsbHost::Endpoint* FindBulkEndpoint(const UsbHost::Interface& interface, bool in);

}  // namespace op1::storage
