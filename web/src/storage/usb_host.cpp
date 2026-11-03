#include "storage/usb_host.h"

#include <algorithm>
#include <chrono>

namespace op1::storage {
namespace {

constexpr uint64_t kTransferTimeoutMs = 2000;
constexpr size_t kMaxConfigurationBytes = 64 * 1024;
constexpr uint8_t kDeviceAddress = 1;

uint16_t Le16(const uint8_t* p) { return uint16_t(p[0]) | (uint16_t(p[1]) << 8); }

USBSetupBytes Setup(uint8_t type, uint8_t request, uint16_t value, uint16_t index, uint16_t length) {
    USBSetupBytes setup{};
    setup.bmRequestType = type;
    setup.bRequest = request;
    setup.wValue = value;
    setup.wIndex = index;
    setup.wLength = length;
    return setup;
}

uint64_t WallMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

}  // namespace

struct UsbHost::Transfer {
    bool done = false;
    std::vector<uint8_t> reply;
};

UsbHost::UsbHost(USBDevice& device, Pump pump, Clock clock)
    : device_(device), pump_(std::move(pump)), clock_(clock ? std::move(clock) : Clock(WallMs)) {}

bool UsbHost::Fail(const std::string& message) {
    error_ = message;
    return false;
}

void UsbHost::Reap() {
    transfers_.erase(std::remove_if(transfers_.begin(), transfers_.end(),
                                    [](const auto& transfer) { return transfer->done; }),
                     transfers_.end());
}

bool UsbHost::Run(const std::shared_ptr<Transfer>& transfer, const std::function<void()>& start) {
    if (active_) return Fail("USB host transfer reentry");
    Reap();
    if (!transfers_.empty()) return Fail("an older USB transfer is still outstanding");
    if (!device_.IsConnected()) return Fail("USB device is not connected");

    active_ = true;
    struct ActiveGuard {
        bool& active;
        ~ActiveGuard() { active = false; }
    } guard{active_};
    transfers_.push_back(transfer);
    start();
    const uint64_t deadline = clock_() + kTransferTimeoutMs;
    while (!transfer->done) {
        if (!device_.IsConnected()) {
            return Fail("USB device disconnected during a transfer");
        }
        if (clock_() >= deadline) {
            return Fail("USB transfer timed out");
        }
        if (!pump_()) {
            return Fail("emulator stopped during a USB transfer");
        }
    }
    Reap();
    return true;
}

bool UsbHost::ControlIn(uint8_t request, uint16_t value, uint16_t index,
                        std::vector<uint8_t>& data, size_t length) {
    if (length > UINT16_MAX) return Fail("control read is too large");
    auto transfer = std::make_shared<Transfer>();
    if (!Run(transfer, [&] {
            device_.HandleSetupPacket(Setup(0x80, request, value, index, length), nullptr, 0,
                                      [transfer](const u8* bytes, std::size_t size) {
                                          transfer->reply.assign(bytes, bytes + size);
                                          transfer->done = true;
                                      });
        }))
        return false;
    data = std::move(transfer->reply);
    return true;
}

bool UsbHost::ControlOut(uint8_t request, uint16_t value, uint16_t index,
                         const std::vector<uint8_t>& data) {
    if (data.size() > UINT16_MAX) return Fail("control write is too large");
    auto transfer = std::make_shared<Transfer>();
    return Run(transfer, [&] {
        device_.HandleSetupPacket(Setup(0x00, request, value, index, data.size()), data.data(),
                                  data.size(), [transfer](const u8*, std::size_t) {
                                      transfer->done = true;
                                  });
    });
}

bool UsbHost::BulkRead(int endpoint, std::vector<uint8_t>& data, size_t length) {
    auto transfer = std::make_shared<Transfer>();
    if (!Run(transfer, [&] {
            device_.HandleDataRead(endpoint, 0, length, [transfer](const u8* bytes, std::size_t size) {
                transfer->reply.assign(bytes, bytes + size);
                transfer->done = true;
            });
        }))
        return false;
    if (transfer->reply.size() != length) return Fail("short bulk IN transfer");
    data = std::move(transfer->reply);
    return true;
}

bool UsbHost::BulkWrite(int endpoint, const uint8_t* data, size_t length) {
    auto transfer = std::make_shared<Transfer>();
    return Run(transfer, [&] {
        device_.HandleDataWrite(endpoint, 0, data, length, [transfer] { transfer->done = true; });
    });
}

bool UsbHost::Connected() const { return device_.IsConnected(); }

bool UsbHost::Quiescent() {
    Reap();
    return transfers_.empty() && device_.ControlIdle();
}

bool UsbHost::Enumerate(Device& result) {
    if (!Quiescent()) return Fail("USB is not ready for enumeration");
    ClearError();

    std::vector<uint8_t> descriptor;
    if (!ControlIn(6, 0x0100, 0, descriptor, 18)) return false;
    if (descriptor.size() != 18 || descriptor[0] != 18 || descriptor[1] != 1)
        return Fail("invalid USB device descriptor");
    result = {};
    result.vendor = Le16(&descriptor[8]);
    result.product = Le16(&descriptor[10]);
    if (!ControlOut(5, kDeviceAddress, 0)) return false;

    if (!ControlIn(6, 0x0200, 0, descriptor, 9)) return false;
    if (descriptor.size() != 9 || descriptor[0] != 9 || descriptor[1] != 2)
        return Fail("invalid USB configuration descriptor");
    const size_t total = Le16(&descriptor[2]);
    if (total < 9 || total > kMaxConfigurationBytes) return Fail("invalid configuration size");
    if (!ControlIn(6, 0x0200, 0, descriptor, total)) return false;
    if (descriptor.size() != total) return Fail("short configuration descriptor");
    result.configuration = descriptor[5];

    const UsbHost::Interface* current = nullptr;
    for (size_t at = 9; at < descriptor.size();) {
        const uint8_t length = descriptor[at];
        if (length < 2 || at + length > descriptor.size()) return Fail("malformed USB descriptor");
        const uint8_t type = descriptor[at + 1];
        if (type == 4) {
            if (length < 9) return Fail("short interface descriptor");
            Interface interface;
            interface.number = descriptor[at + 2];
            interface.alternate = descriptor[at + 3];
            interface.classCode = descriptor[at + 5];
            interface.subclass = descriptor[at + 6];
            interface.protocol = descriptor[at + 7];
            result.interfaces.push_back(std::move(interface));
            current = &result.interfaces.back();
        } else if (type == 5) {
            if (!current || length < 7) return Fail("endpoint without an interface");
            Endpoint endpoint;
            endpoint.address = descriptor[at + 2];
            endpoint.attributes = descriptor[at + 3];
            endpoint.maxPacketSize = Le16(&descriptor[at + 4]);
            endpoint.interval = descriptor[at + 6];
            const int index = endpoint.address & 0x0f;
            if (!index || index >= USB_NUM_ENDPOINTS || !endpoint.maxPacketSize)
                return Fail("invalid bulk endpoint descriptor");
            result.interfaces.back().endpoints.push_back(endpoint);
        }
        at += length;
    }
    if (!result.configuration) return Fail("USB device has no usable configuration");
    return ControlOut(9, result.configuration, 0);
}

const UsbHost::Interface* FindInterface(const UsbHost::Device& device, uint8_t classCode,
                                        uint8_t subclass, uint8_t protocol) {
    for (const auto& interface : device.interfaces) {
        if (interface.classCode == classCode && interface.subclass == subclass &&
            interface.protocol == protocol)
            return &interface;
    }
    return nullptr;
}

const UsbHost::Endpoint* FindBulkEndpoint(const UsbHost::Interface& interface, bool in) {
    for (const auto& endpoint : interface.endpoints) {
        if ((endpoint.attributes & 3) == 2 && ((endpoint.address & 0x80) != 0) == in)
            return &endpoint;
    }
    return nullptr;
}

}  // namespace op1::storage
