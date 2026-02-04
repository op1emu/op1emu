#include "usbipd.h"
#include "usbip-internal.h"
#include <utils/log.h>
#include <cassert>
#include <cstring>

#define USBIP_SERVER_PORT 3240

USBIPServer::USBIPServer(uvw::loop& loop, USBDevice& device) : loop(loop), device(device)
{
}

void USBIPServer::Start()
{
    server = loop.resource<uvw::tcp_handle>();
    server->on<uvw::listen_event>([this] (const uvw::listen_event&, uvw::tcp_handle& srv) {
        auto client = srv.parent().resource<uvw::tcp_handle>();

        client->on<uvw::close_event>([ptr = srv.shared_from_this()] (const uvw::close_event&, uvw::tcp_handle&) { ptr->close(); });
        client->on<uvw::end_event>([] (const uvw::end_event&, uvw::tcp_handle& client) { client.close(); });
        client->on<uvw::data_event>([this] (const uvw::data_event& event, uvw::tcp_handle& client) { OnClientDataEvent(event, client); });

        srv.accept(*client);
        client->read();
    });
    server->bind("0.0.0.0", USBIP_SERVER_PORT);
    server->listen();
}

void USBIPServer::ReplyImport(OP_REQ_IMPORT& req, uvw::tcp_handle& client)
{
    std::lock_guard lock(mutex);
    OP_REP_IMPORT rep{};
    rep.version = req.version;
    rep.command = 0x0003;
    strcpy(rep.busid, req.busid);
    rep.speed = 3; // High Speed
    client.write((char*)&rep, sizeof(rep));
}

std::size_t static GetTotalSize(USBIP_CMD_SUBMIT& req)
{
    std::size_t total = sizeof(req);
    if (!req.direction) {
        total += req.transfer_buffer_length;
    }
    total += req.number_of_packets * sizeof(USBIP_ISOC_DESC);
    return total;
}

static void GenerateURBReply(const USBIP_CMD_SUBMIT& req, USBIP_RET_SUBMIT& reply)
{
    reply.command = 0x00000003;
    reply.seqnum = req.seqnum;
    reply.devid = 0;
    reply.direction = 0;
    reply.ep = 0;
    reply.status = 0;
    reply.start_frame = req.seqnum;
    reply.number_of_packets = req.number_of_packets;
    reply.error_count = 0;
    reply.setup = {};
}

void USBIPServer::Reply(uvw::tcp_handle& client, const USBIP_CMD_SUBMIT& req, const void* data, std::size_t length, const USBIP_ISOC_DESC* isoc)
{
    USBIP_RET_SUBMIT reply;
    GenerateURBReply(req, reply);
    if (req.direction == 0 && req.number_of_packets) {
        reply.actual_length = 0;
    } else {
        reply.actual_length = length;
    }
    auto size = sizeof(reply) + ((data && length) ? length : 0) + ((isoc && req.number_of_packets) ? (req.number_of_packets * sizeof(USBIP_ISOC_DESC)) : 0);
    std::unique_ptr<char[]> buffer(new char[size]);
    char* ptr = buffer.get();
    memcpy(ptr, &reply, sizeof(reply));
    ptr += sizeof(reply);
    if (data && length) {
        memcpy(ptr, data, length);
        ptr += length;
    }
    if (isoc && req.number_of_packets) {
        memcpy(ptr, isoc, req.number_of_packets * sizeof(USBIP_ISOC_DESC));
    }
    std::lock_guard lock(mutex);
    /* FIXME: currently write the buffer synchronously */
    std::size_t remain = size;
    while (remain > 0) {
        auto len = client.try_write(buffer.get() + size - remain, remain);
        if (len > 0) {
            remain -= len;
        }
    }
}

static void FillIsocDesc(USBIP_ISOC_DESC* isoc, uint32_t number_of_packets, uint32_t transfer_buffer_length) {
    for (int i = 0; i < number_of_packets; i++) {
        isoc[i].status = 0;
        isoc[i].actual_length = std::min(transfer_buffer_length, (uint32_t)isoc[i].length);
        transfer_buffer_length -= isoc[i].actual_length;
    }
}

void USBIPServer::HandleURBRequest(USBIP_CMD_SUBMIT& req, uint8_t* data, std::size_t length, uvw::tcp_handle& client)
{
    if (req.ep == 0) { // Control Endpoint #0
        device.HandleSetupPacket(req.setup, data, length, [req=req, &client, this] (const uint8_t* data, std::size_t length) {
            Reply(client, req, data, length, nullptr);
        });
    } else if (req.direction) { // Device to Host
        std::vector<USBIP_ISOC_DESC> isoc(req.number_of_packets);
        for (int i = 0; i < req.number_of_packets; i++) {
            isoc[i] = *((USBIP_ISOC_DESC*)data + i);
        }
        device.HandleDataRead(req.ep, req.interval, req.transfer_buffer_length, [req=req, isoc=isoc, &client, this] (const uint8_t* data, std::size_t length) {
            FillIsocDesc(const_cast<USBIP_ISOC_DESC*>(isoc.data()), req.number_of_packets, length);
            Reply(client, req, data, length, isoc.data());
        });
    } else { // Host to Device
        std::vector<USBIP_ISOC_DESC> isoc(req.number_of_packets);
        for (int i = 0; i < req.number_of_packets; i++) {
            isoc[i] = *((USBIP_ISOC_DESC*)(data + req.transfer_buffer_length) + i);
        }
        device.HandleDataWrite(req.ep, req.interval, data, req.transfer_buffer_length, [req=req, isoc=isoc, &client, this]() {
            FillIsocDesc(const_cast<USBIP_ISOC_DESC*>(isoc.data()), req.number_of_packets, 0);
            Reply(client, req, nullptr, req.transfer_buffer_length, isoc.data());
        });
    }
}

void USBIPServer::OnClientDataEvent(const uvw::data_event& event, uvw::tcp_handle& client)
{
    static USBIP_CMD_SUBMIT urbRequest;
    buffer.push(event.data.get(), event.length);
    USBIPState last;
    do {
        last = state;
        if (state == USBIPState::WaitCommand) {
            if (buffer.size() >= sizeof(OP_REQ_HEADER)) {
                OP_REQ_HEADER header;
                buffer.peek(&header, sizeof(header));
                if (header.command == 0x8003) {
                    state = USBIPState::WaitCommandImport;
                } else {
                    buffer.pop(sizeof(OP_REQ_HEADER));
                }
            }
        } else if (state == USBIPState::WaitCommandImport) {
            if (buffer.size() >= sizeof(OP_REQ_IMPORT)) {
                OP_REQ_IMPORT req;
                buffer.pop(&req, sizeof(req));
                LogInfo("USBIP: attach device");
                ReplyImport(req, client);
                state = USBIPState::WaitHeader;
            }
        } else if (state == USBIPState::WaitHeader) {
            if (buffer.size() >= sizeof(USBIP_HEADER_BASIC)) {
                USBIP_HEADER_BASIC header;
                buffer.peek(&header, sizeof(header));
                if (header.command == 0x00000001) {
                    state = USBIPState::WaitURB;
                } else if (header.command == 0x00000002) {
                    state = USBIPState::WaitUnlink;
                }
            }
        } else if (state == USBIPState::WaitURB) {
            if (buffer.size() >= sizeof(USBIP_CMD_SUBMIT)) {
                buffer.pop(&urbRequest, sizeof(USBIP_CMD_SUBMIT));
                state = USBIPState::WaitTransferBuffer;
            }
        } else if (state == USBIPState::WaitTransferBuffer) {
            auto remain = GetTotalSize(urbRequest) - sizeof(USBIP_CMD_SUBMIT);
            if (buffer.size() >= remain) {
                std::vector<uint8_t> data(remain);
                buffer.pop(data.data(), remain);
                HandleURBRequest(urbRequest, data.data(), remain, client);
                state = USBIPState::WaitHeader;
            }
        } else if (state == USBIPState::WaitUnlink) {
            if (buffer.size() >= sizeof(USBIP_CMD_SUBMIT)) {
                /* ignore cmd UNLINK */
                buffer.pop(&urbRequest, sizeof(USBIP_CMD_SUBMIT));
                state = USBIPState::WaitHeader;
            }
        }
    } while (last != state);
}