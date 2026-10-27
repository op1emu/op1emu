#include "usbipd.h"
#include "usbip-internal.h"
#include <utils/log.h>
#include <cassert>
#include <cstring>

// What usbip list shows. The guest's descriptors give the rest.
static constexpr const char* kBusId = "1-1";
static constexpr const char* kDevicePath = "/sys/devices/platform/op1emu/usb1/1-1";
static constexpr uint32_t kBusNum = 1;
static constexpr uint32_t kDevNum = 2;
static constexpr uint32_t kSpeedHigh = 3;

static constexpr uint16_t kVersion = 0x0111;
static constexpr uint16_t kReqDevList = 0x8005;
static constexpr uint16_t kRepDevList = 0x0005;
static constexpr uint16_t kReqImport = 0x8003;
static constexpr uint16_t kRepImport = 0x0003;
// OP_REP_* status
static constexpr uint32_t kStatusUnavailable = 1;
static constexpr uint32_t kStatusBusy = 2;
static constexpr uint32_t kStatusError = 5;

static constexpr uint8_t kGetDescriptor = 6;
static constexpr uint8_t kDescDevice = 1;
static constexpr uint8_t kDescConfig = 2;
static constexpr uint8_t kDescInterface = 4;
// The guest answers in microseconds of its time; this is for one that never does.
static constexpr uvw::timer_handle::time kReadTimeout{2000};

static std::unique_ptr<char[]> Copy(const void* data, std::size_t length)
{
    std::unique_ptr<char[]> copy(new char[length]);
    memcpy(copy.get(), data, length);
    return copy;
}

USBIPServer::USBIPServer(uvw::loop& loop, USBDevice& device) : loop(loop), device(device)
{
}

void USBIPServer::Start(unsigned int port)
{
    answered = loop.resource<uvw::async_handle>();
    answered->on<uvw::async_event>([this] (const uvw::async_event&, uvw::async_handle&) { OnDescriptorAnswered(); });
    readTimeout = loop.resource<uvw::timer_handle>();
    readTimeout->on<uvw::timer_event>([this] (const uvw::timer_event&, uvw::timer_handle&) {
        LogWarn("USBIP: the firmware did not answer for its descriptors");
        FinishRead(false);
    });

    server = loop.resource<uvw::tcp_handle>();
    server->on<uvw::listen_event>([this] (const uvw::listen_event&, uvw::tcp_handle& srv) {
        auto client = srv.parent().resource<uvw::tcp_handle>();
        client->data(std::make_shared<Session>());

        // The listening handle stays open: a client that goes away (a failed or
        // detached attach, a list) must not stop later ones from connecting.
        client->on<uvw::close_event>([this] (const uvw::close_event&, uvw::tcp_handle& client) { Detach(client); });
        client->on<uvw::end_event>([this] (const uvw::end_event&, uvw::tcp_handle& client) { Close(client); });
        // A reset connection must not stay attached and keep others out.
        client->on<uvw::error_event>([this] (const uvw::error_event&, uvw::tcp_handle& client) { Close(client); });
        client->on<uvw::shutdown_event>([this] (const uvw::shutdown_event&, uvw::tcp_handle& client) { Close(client); });
        client->on<uvw::data_event>([this] (const uvw::data_event& event, uvw::tcp_handle& client) { OnClientDataEvent(event, client); });

        srv.accept(*client);
        // As usbipd: a host that went away without a word frees the device
        // in time, rather than leaving every later attach refused as busy.
        client->keep_alive(true, uvw::tcp_handle::time{30});
        client->no_delay(true);
        client->read();
    });
    server->bind("0.0.0.0", port);
    server->listen();
}

unsigned int USBIPServer::Port() const
{
    return server->sock().port;
}

void USBIPServer::Detach(uvw::tcp_handle& client)
{
    std::lock_guard lock(mutex);
    if (attached.get() != &client) return; // a list, a refused import, already detached
    ++connection;
    attached.reset();
}

void USBIPServer::Close(uvw::tcp_handle& client)
{
    // Before the close: from then on a late reply must not write to the handle.
    Detach(client);
    client.close();
}

bool USBIPServer::EP0Claimed()
{
    // ControlIdle() takes the USB's lock: not under ours (the guest's
    // answers take the two the other way round).
    if (ep0Claimed && device.ControlIdle()) ep0Claimed = false;
    return ep0Claimed;
}

bool USBIPServer::ReplyImport(OP_REQ_IMPORT& req, uvw::tcp_handle& client)
{
    const bool claimed = EP0Claimed();
    std::lock_guard lock(mutex);
    uint32_t status = 0;
    if (!device.IsConnected()) {
        // Refuse rather than let the host's first requests go unanswered: it would
        // give up after ~10 s, and those requests would be left in the controller.
        LogWarn("USBIP: refusing attach, the firmware has not connected to the USB bus yet");
        status = kStatusUnavailable;
    } else if (attached) {
        LogWarn("USBIP: refusing attach, another client is attached");
        status = kStatusBusy;
    } else if (claimed) {
        // Its host's requests would queue behind the server's, and each get
        // the answer to the one before.
        LogWarn("USBIP: refusing attach, the firmware has not answered the server's own request yet");
        status = kStatusUnavailable;
    }
    if (status) {
        OP_REQ_HEADER rep{};
        rep.version = req.version;
        rep.command = kRepImport;
        rep.status = status;
        client.write(Copy(&rep, sizeof(rep)), sizeof(rep));
        return false;
    }
    ++connection;
    attached = client.shared_from_this();
    // The firmware may have connected again as another device since: a list
    // while this client is attached is from what its host reads.
    known = {};
    OP_REP_IMPORT rep{};
    rep.version = req.version;
    rep.command = kRepImport;
    req.busid[sizeof(req.busid) - 1] = '\0';
    // The host checks that the busid is the one it asked for. The rest is
    // the host's to read; vhci takes only the speed.
    FillDevice(rep.udev, nullptr, req.busid);
    client.write(Copy(&rep, sizeof(rep)), sizeof(rep));
    return true;
}

bool USBIPServer::Descriptors::Valid() const
{
    return device.size() >= 18 && device[0] == 18 && device[1] == kDescDevice
        && config.size() >= 9 && config[0] == 9 && config[1] == kDescConfig
        && config.size() == (config[2] | config[3] << 8);
}

void USBIPServer::FillDevice(USBIP_USB_DEVICE& udev, const Descriptors* descriptors, const char* busid)
{
    udev = {};
    strncpy(udev.path, kDevicePath, sizeof(udev.path) - 1);
    strncpy(udev.busid, busid, sizeof(udev.busid) - 1);
    udev.busnum = kBusNum;
    udev.devnum = kDevNum;
    udev.speed = kSpeedHigh;
    if (!descriptors) return;
    const auto& d = descriptors->device;
    udev.idVendor = d[8] | d[9] << 8;
    udev.idProduct = d[10] | d[11] << 8;
    udev.bcdDevice = d[12] | d[13] << 8;
    udev.bDeviceClass = d[4];
    udev.bDeviceSubClass = d[5];
    udev.bDeviceProtocol = d[6];
    udev.bNumConfigurations = d[17];
    udev.bConfigurationValue = descriptors->config[5];
}

void USBIPServer::ReplyDevList(uvw::tcp_handle& client, const Descriptors* descriptors, uint32_t status)
{
    // As Linux's usbip-host reports them: each interface by its first
    // alternate setting, of the configuration listed.
    std::vector<USBIP_USB_INTERFACE> interfaces;
    if (descriptors) {
        const auto& c = descriptors->config;
        for (std::size_t i = c[0]; i + 1 < c.size() && c[i] != 0; i += c[i]) {
            if (c[i + 1] == kDescInterface && c[i] >= 9 && i + 9 <= c.size() && c[i + 3] == 0) {
                interfaces.push_back({c[i + 5], c[i + 6], c[i + 7], 0});
            }
        }
        if (interfaces.size() > UINT8_MAX) { // bNumInterfaces could not say so
            descriptors = nullptr;
            status = kStatusError;
        }
    }

    std::vector<char> rep;
    if (status) {
        OP_REQ_HEADER header{};
        header.version = kVersion;
        header.command = kRepDevList;
        header.status = status;
        rep.assign((char*)&header, (char*)&header + sizeof(header));
    } else {
        OP_REP_DEVLIST header{};
        header.version = kVersion;
        header.command = kRepDevList;
        header.ndev = descriptors ? 1 : 0;
        rep.assign((char*)&header, (char*)&header + sizeof(header));
    }
    if (descriptors) {
        USBIP_USB_DEVICE udev;
        FillDevice(udev, descriptors, kBusId);
        udev.bNumInterfaces = interfaces.size();
        rep.insert(rep.end(), (char*)&udev, (char*)&udev + sizeof(udev));
        rep.insert(rep.end(), (char*)interfaces.data(), (char*)(interfaces.data() + interfaces.size()));
    }
    client.write(Copy(rep.data(), rep.size()), rep.size());
    // As usbipd does: one request per connection. The close waits for the write.
    client.shutdown();
}

void USBIPServer::HandleDevList(uvw::tcp_handle& client)
{
    if (!device.IsConnected()) {
        LogInfo("USBIP: list: the firmware has not connected to the USB bus yet");
        ReplyDevList(client, nullptr, 0); // nothing on the bus
        return;
    }
    {
        // The attached client's host owns EP0; it has read the descriptors.
        std::lock_guard lock(mutex);
        if (attached) {
            // Before its host has read them, the device is there but cannot be described.
            ReplyDevList(client, known.Valid() ? &known : nullptr, known.Valid() ? 0 : kStatusError);
            return;
        }
    }
    if (!read) {
        const bool claimed = EP0Claimed();
        if (claimed || !device.ControlIdle()) {
            // Requests are still waiting on EP0 (the server's last ones, or
            // an earlier client's): one sent now would get their answers.
            LogWarn("USBIP: list: the firmware has not answered earlier requests yet");
            std::lock_guard lock(mutex);
            ReplyDevList(client, known.Valid() ? &known : nullptr, known.Valid() ? 0 : kStatusError);
            return;
        }
    }
    // Read them each time: the firmware may have connected again as another device.
    ReadDescriptors([weak = client.weak_from_this()] (const Descriptors* descriptors) {
        // A guest that does not answer, or not with descriptors, is not an empty bus.
        if (auto client = weak.lock(); client && !client->closing()) ReplyDevList(*client, descriptors, descriptors ? 0 : kStatusError);
    });
}

void USBIPServer::Learn(const USBSetupBytes& setup, const uint8_t* data, std::size_t length)
{
    if (setup.bmRequestType != 0x80 || setup.bRequest != kGetDescriptor) return;
    std::lock_guard lock(mutex);
    const uint8_t type = setup.wValue >> 8;
    const uint8_t index = setup.wValue & 0xFF;
    if (type == kDescDevice && length >= 18) {
        // A new enumeration: the configuration that follows is its own.
        known.device.assign(data, data + 18);
        known.config.clear();
    } else if (type == kDescConfig && index == 0 && length >= 9 && length == (data[2] | data[3] << 8)) {
        known.config.assign(data, data + length);
    }
}

void USBIPServer::ReadDescriptors(std::function<void(const Descriptors*)> done)
{
    if (read) { // one is under way: its answer serves this caller too
        read->waiters.push_back(std::move(done));
        return;
    }
    read.emplace();
    read->id = ++readId;
    read->waiters.push_back(std::move(done));
    readTimeout->start(kReadTimeout, uvw::timer_handle::time{0});
    RequestDescriptor(kDescDevice << 8, 18);
}

void USBIPServer::RequestDescriptor(uint16_t value, uint16_t length)
{
    USBSetupBytes setup{};
    setup.bmRequestType = 0x80; // standard, device to host, device
    setup.bRequest = kGetDescriptor;
    setup.wValue = value;
    setup.wIndex = 0;
    setup.wLength = length;
    // The guest answers on the CPU thread, with the USB's lock held.
    device.HandleSetupPacket(setup, nullptr, 0, [this, id = read->id] (const uint8_t* data, std::size_t length) {
        {
            std::lock_guard lock(mutex);
            answer.emplace(id, std::vector<uint8_t>(data, data + length));
        }
        answered->send();
    });
}

void USBIPServer::OnDescriptorAnswered()
{
    std::optional<std::pair<uint64_t, std::vector<uint8_t>>> got;
    {
        std::lock_guard lock(mutex);
        got.swap(answer);
    }
    if (!got || !read || got->first != read->id) return; // after a timeout
    auto& data = got->second;
    if (read->step == 0) {
        // A request an earlier client left in the controller may be what the
        // guest answered: check what it is.
        if (data.size() < 18 || data[0] != 18 || data[1] != kDescDevice) return FinishRead(false);
        read->got.device = std::move(data);
        read->step = 1;
        RequestDescriptor(kDescConfig << 8, 9);
    } else if (read->step == 1) {
        if (data.size() < 9 || data[1] != kDescConfig) return FinishRead(false);
        const uint16_t total = data[2] | data[3] << 8;
        if (total == data.size()) {
            read->got.config = std::move(data);
            return FinishRead(true);
        }
        read->step = 2;
        RequestDescriptor(kDescConfig << 8, total);
    } else {
        read->got.config = std::move(data);
        FinishRead(true);
    }
}

void USBIPServer::FinishRead(bool ok)
{
    if (!read) return;
    readTimeout->stop();
    auto finished = std::move(*read);
    read.reset();
    const bool valid = ok && finished.got.Valid();
    // What was asked may still be waiting on EP0: keep it from an attach
    // until the guest has answered it.
    if (!valid) ep0Claimed = true;
    if (valid) {
        std::lock_guard lock(mutex);
        known = finished.got;
    } else if (ok) {
        LogWarn("USBIP: the firmware's descriptors do not add up");
    }
    for (auto& waiter : finished.waiters) waiter(valid ? &finished.got : nullptr);
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

void USBIPServer::Reply(uvw::tcp_handle& client, uint64_t conn, const USBIP_CMD_SUBMIT& req, const void* data, std::size_t length, const USBIP_ISOC_DESC* isoc)
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
    if (conn != connection) return; // the client this answers has gone
    /* FIXME: currently write the buffer synchronously */
    std::size_t remain = size;
    while (remain > 0) {
        auto len = client.try_write(buffer.get() + size - remain, remain);
        if (len > 0) {
            remain -= len;
        } else if (len != UV_EAGAIN) {
            break; // the connection failed; retrying would never end
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
    const uint64_t conn = connection;
    if (req.ep == 0) { // Control Endpoint #0
        device.HandleSetupPacket(req.setup, data, length, [req=req, &client, conn, this] (const uint8_t* data, std::size_t length) {
            Learn(req.setup, data, length);
            Reply(client, conn, req, data, length, nullptr);
        });
    } else if (req.direction) { // Device to Host
        std::vector<USBIP_ISOC_DESC> isoc(req.number_of_packets);
        for (int i = 0; i < req.number_of_packets; i++) {
            isoc[i] = *((USBIP_ISOC_DESC*)data + i);
        }
        device.HandleDataRead(req.ep, req.interval, req.transfer_buffer_length, [req=req, isoc=isoc, &client, conn, this] (const uint8_t* data, std::size_t length) {
            FillIsocDesc(const_cast<USBIP_ISOC_DESC*>(isoc.data()), req.number_of_packets, length);
            Reply(client, conn, req, data, length, isoc.data());
        });
    } else { // Host to Device
        std::vector<USBIP_ISOC_DESC> isoc(req.number_of_packets);
        for (int i = 0; i < req.number_of_packets; i++) {
            isoc[i] = *((USBIP_ISOC_DESC*)(data + req.transfer_buffer_length) + i);
        }
        device.HandleDataWrite(req.ep, req.interval, data, req.transfer_buffer_length, [req=req, isoc=isoc, &client, conn, this]() {
            FillIsocDesc(const_cast<USBIP_ISOC_DESC*>(isoc.data()), req.number_of_packets, 0);
            Reply(client, conn, req, nullptr, req.transfer_buffer_length, isoc.data());
        });
    }
}

void USBIPServer::OnClientDataEvent(const uvw::data_event& event, uvw::tcp_handle& client)
{
    auto session = client.data<Session>();
    session->buffer.push(event.data.get(), event.length);
    Parse(client, *session);
}

void USBIPServer::Parse(uvw::tcp_handle& client, Session& session)
{
    auto& buffer = session.buffer;
    auto& state = session.state;
    auto& urbRequest = session.urb;
    USBIPState last;
    do {
        last = state;
        if (state == USBIPState::WaitCommand) {
            if (buffer.size() >= sizeof(OP_REQ_HEADER)) {
                OP_REQ_HEADER header;
                buffer.peek(&header, sizeof(header));
                if (header.command == kReqImport) {
                    state = USBIPState::WaitCommandImport;
                } else if (header.command == kReqDevList) {
                    buffer.pop(sizeof(OP_REQ_HEADER));
                    LogInfo("USBIP: list devices");
                    state = USBIPState::Done;
                    HandleDevList(client);
                } else {
                    buffer.pop(sizeof(OP_REQ_HEADER));
                }
            }
        } else if (state == USBIPState::WaitCommandImport) {
            if (buffer.size() >= sizeof(OP_REQ_IMPORT) && read) {
                // Our requests are on EP0: its host's would meet them there.
                if (!session.held) {
                    session.held = true;
                    read->waiters.push_back([this, weak = client.weak_from_this()] (const Descriptors*) {
                        auto client = weak.lock();
                        if (!client || client->closing()) return;
                        auto session = client->data<Session>();
                        session->held = false;
                        Parse(*client, *session);
                    });
                }
            } else if (buffer.size() >= sizeof(OP_REQ_IMPORT)) {
                OP_REQ_IMPORT req;
                buffer.pop(&req, sizeof(req));
                LogInfo("USBIP: attach device");
                state = ReplyImport(req, client) ? USBIPState::WaitHeader : USBIPState::WaitCommand;
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