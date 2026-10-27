#pragma once

#include <uvw.hpp>
#include <memory>
#include <optional>
#include <vector>
#include <cqueue/cqueue.h>
#include <functional>
#include <mutex>
#include "usbip-internal.h"

enum class USBIPState {
    WaitCommand,
    WaitCommandImport,
    WaitHeader,
    WaitURB,
    WaitUnlink,
    WaitTransferBuffer,
    Done, // answered a list: the connection is closing
};

class USBIPServer {
public:
    static constexpr unsigned int kDefaultPort = 3240;

    USBIPServer(uvw::loop& loop, USBDevice& device);
    // Port 0 takes any free one; Port() tells which.
    void Start(unsigned int port = kDefaultPort);
    unsigned int Port() const;

private:
    // One per connection, in its handle's data(): a `usbip list` may come
    // while another client is attached.
    struct Session {
        cqueue<uint8_t> buffer;
        USBIPState state = USBIPState::WaitCommand;
        USBIP_CMD_SUBMIT urb;
        bool held = false; // its import waits for the server's read of the descriptors
    };

    // The guest's device descriptor and its first configuration (all of it).
    struct Descriptors {
        std::vector<uint8_t> device;
        std::vector<uint8_t> config;
        bool Valid() const;
    };

    void OnClientDataEvent(const uvw::data_event& event, uvw::tcp_handle& client);
    void Parse(uvw::tcp_handle& client, Session& session);
    void HandleURBRequest(USBIP_CMD_SUBMIT& req, uint8_t* data, std::size_t length, uvw::tcp_handle& client);
    void Reply(uvw::tcp_handle& client, uint64_t connection, const USBIP_CMD_SUBMIT& req, const void* data, std::size_t length, const USBIP_ISOC_DESC* isoc);
    bool ReplyImport(OP_REQ_IMPORT& req, uvw::tcp_handle& client); // false: refused
    // The client no longer attached, if it was: idempotent.
    void Detach(uvw::tcp_handle& client);
    void Close(uvw::tcp_handle& client);

    // usbip list. The guest answers it, so the reply may come later.
    void HandleDevList(uvw::tcp_handle& client);
    // No descriptors and status 0: an empty bus.
    static void ReplyDevList(uvw::tcp_handle& client, const Descriptors* descriptors, uint32_t status);
    static void FillDevice(USBIP_USB_DEVICE& udev, const Descriptors* descriptors, const char* busid);
    // An attached client's host reads the descriptors: keep them for a list.
    void Learn(const USBSetupBytes& setup, const uint8_t* data, std::size_t length);

    // Reading the descriptors over EP0 when no client is attached, on the
    // loop thread; the guest answers on the CPU thread, through `answered`.
    void ReadDescriptors(std::function<void(const Descriptors*)> done);
    void RequestDescriptor(uint16_t value, uint16_t length);
    void OnDescriptorAnswered();
    void FinishRead(bool ok);
    struct DescriptorRead {
        uint64_t id = 0;
        int step = 0;
        Descriptors got;
        // Callers waiting for it: lists, and an import held back so that its
        // host's requests do not meet ours on EP0.
        std::vector<std::function<void(const Descriptors*)>> waiters;
    };
    std::optional<DescriptorRead> read;
    uint64_t readId = 0;
    std::optional<std::pair<uint64_t, std::vector<uint8_t>>> answer; // under mutex
    std::shared_ptr<uvw::async_handle> answered;
    std::shared_ptr<uvw::timer_handle> readTimeout;
    // After a read that did not finish: its requests may still be in the
    // controller. Cleared once it is idle.
    bool ep0Claimed = false;
    bool EP0Claimed();

private:
    // The attached client and which connection it is: a reply for an earlier
    // connection (the guest answers late) is dropped, not written to a closed handle.
    std::shared_ptr<uvw::tcp_handle> attached;
    uint64_t connection = 0;
    Descriptors known; // under mutex

    std::mutex mutex;
    uvw::loop& loop;
    std::shared_ptr<uvw::tcp_handle> server;

    USBDevice& device;
};
