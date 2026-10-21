#pragma once

#include <uvw.hpp>
#include <memory>
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
};

class USBIPServer {
public:
    USBIPServer(uvw::loop& loop, USBDevice& device);
    void Start();

private:
    void OnClientDataEvent(const uvw::data_event& event, uvw::tcp_handle& client);
    void HandleURBRequest(USBIP_CMD_SUBMIT& req, uint8_t* data, std::size_t length, uvw::tcp_handle& client);
    void Reply(uvw::tcp_handle& client, uint64_t connection, const USBIP_CMD_SUBMIT& req, const void* data, std::size_t length, const USBIP_ISOC_DESC* isoc);
    bool ReplyImport(OP_REQ_IMPORT& req, uvw::tcp_handle& client); // false: refused
    void OnClientAccepted(const std::shared_ptr<uvw::tcp_handle>& client);
    void OnClientClosed(uvw::tcp_handle& client);

private:
    cqueue<uint8_t> buffer;
    USBIPState state = USBIPState::WaitCommand;

    // The client being served and which connection it is: a reply for an earlier
    // connection (the guest answers late) is dropped, not written to a closed handle.
    std::shared_ptr<uvw::tcp_handle> current;
    uint64_t connection = 0;

    std::mutex mutex;
    uvw::loop& loop;
    std::shared_ptr<uvw::tcp_handle> server;

    USBDevice& device;
};