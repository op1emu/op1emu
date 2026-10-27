// USBIPServer against a fake USB device: usbip list, attach, and the two
// together, over loopback TCP. No firmware.
#include "usbipd.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

static void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

static std::vector<uint8_t> Hex(const char* hex) {
    std::vector<uint8_t> bytes;
    for (; hex[0] && hex[1]; hex += 2) bytes.push_back(std::stoi(std::string(hex, 2), nullptr, 16));
    return bytes;
}

// The OP-1's own, as its normal firmware gives them: audio and MIDI, with
// alternate settings.
static const auto kDevice = Hex("120100020000004067230400420201020303");
static const auto kConfig = Hex(
    "09020201040104c0fa0904000000010100020b240100013500030102030c24020101010002030000000924030201030001000c"
    "24020b13070002030000000924030c0101000b000904010000010200050904010101010200060724010c0101000b2402010203"
    "180144ac0009058625140104000007250101000000090402000001020002090402010201020002072401010101000b24020102"
    "03180144ac00090507051401040082072501010000000905821104000402000904030002010300020724010001250006240201"
    "150006240202160009240301170116010009240302180115010009050402800000000005250101150905830280000000000525"
    "010117");

// Answers GET_DESCRIPTOR on its own thread, as the CPU thread would.
class FakeDevice : public USBDevice {
public:
    std::atomic<bool> connected{false};
    std::atomic<bool> answers{true};
    std::atomic<bool> wrongConfig{false}; // the device descriptor where the configuration is asked for
    std::atomic<int> setups{0};

    FakeDevice() : worker([this] { Run(); }) {}
    ~FakeDevice() override {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        wake.notify_all();
        worker.join();
    }

    bool IsConnected() const override { return connected; }

    void HandleSetupPacket(USBSetupBytes setup, const u8*, std::size_t, ReplyCallback callback) override {
        ++setups;
        if (!answers) return;
        std::vector<uint8_t> reply;
        if (setup.bmRequestType == 0x80 && setup.bRequest == 6) {
            const auto& d = (setup.wValue >> 8) == 1 || wrongConfig ? kDevice : kConfig;
            reply.assign(d.begin(), d.begin() + std::min<std::size_t>(d.size(), setup.wLength));
        }
        {
            std::lock_guard lock(mutex);
            queue.emplace_back([callback, reply] { callback(reply.data(), reply.size()); });
        }
        wake.notify_all();
    }
    void HandleDataWrite(int, int, const u8*, std::size_t, WriteDoneCallback) override {}
    void HandleDataRead(int, int, std::size_t, ReplyCallback) override {}

private:
    void Run() {
        std::unique_lock lock(mutex);
        while (true) {
            wake.wait(lock, [this] { return stop || !queue.empty(); });
            if (stop) return;
            auto job = std::move(queue.front());
            queue.pop_front();
            lock.unlock();
            job();
            lock.lock();
        }
    }

    std::mutex mutex;
    std::condition_variable wake;
    std::deque<std::function<void()>> queue;
    bool stop = false;
    std::thread worker;
};

static int Connect(unsigned int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    Require(connect(fd, (sockaddr*)&addr, sizeof(addr)) == 0, "connects");
    return fd;
}

static void Send(int fd, const void* data, std::size_t length) {
    Require(send(fd, data, length, 0) == (ssize_t)length, "sends");
}

static void Receive(int fd, void* data, std::size_t length) {
    std::size_t got = 0;
    while (got < length) {
        auto n = recv(fd, (char*)data + got, length - got, 0);
        Require(n > 0, "receives all it expects");
        got += n;
    }
}

static bool ClosedByServer(int fd) {
    char byte;
    return recv(fd, &byte, 1, 0) == 0;
}

struct List {
    uint32_t status = 0; // 0: the devices that follow
    uint32_t ndev = 0;
    USBIP_USB_DEVICE udev{};
    std::vector<USBIP_USB_INTERFACE> interfaces;
};

// As `usbip list -r` does it.
static List ListDevices(unsigned int port) {
    int fd = Connect(port);
    OP_REQ_HEADER req{};
    req.version = 0x0111;
    req.command = 0x8005;
    Send(fd, &req, sizeof(req));
    OP_REQ_HEADER rep;
    Receive(fd, &rep, sizeof(rep));
    Require(rep.version == 0x0111 && rep.command == 0x0005, "OP_REP_DEVLIST header");
    List list;
    list.status = rep.status;
    if (list.status == 0) {
        be_uint32_t ndev;
        Receive(fd, &ndev, sizeof(ndev));
        list.ndev = ndev;
    }
    if (list.ndev) {
        Receive(fd, &list.udev, sizeof(list.udev));
        list.interfaces.resize(list.udev.bNumInterfaces);
        Receive(fd, list.interfaces.data(), list.interfaces.size() * sizeof(USBIP_USB_INTERFACE));
    }
    Require(ClosedByServer(fd), "the server closes a list's connection");
    close(fd);
    return list;
}

// As `usbip attach -r` does it: the connection, and the reply's status.
static int Attach(unsigned int port, uint32_t& status) {
    int fd = Connect(port);
    OP_REQ_IMPORT req{};
    req.version = 0x0111;
    req.command = 0x8003;
    std::strcpy(req.busid, "1-1");
    Send(fd, &req, sizeof(req));
    OP_REQ_HEADER header;
    Receive(fd, &header, sizeof(header));
    Require(header.command == 0x0003, "OP_REP_IMPORT");
    status = header.status;
    if (status == 0) {
        USBIP_USB_DEVICE udev;
        Receive(fd, &udev, sizeof(udev));
        Require(std::strcmp(udev.busid, "1-1") == 0, "the import names the busid asked for");
    }
    return fd;
}

// The attached host's GET_DESCRIPTOR, answered.
static std::vector<uint8_t> GetDescriptor(int fd, uint32_t seqnum, uint16_t value, uint16_t length) {
    USBIP_CMD_SUBMIT cmd{};
    cmd.command = 1;
    cmd.seqnum = seqnum;
    cmd.devid = (1 << 16) | 2;
    cmd.direction = 1;
    cmd.ep = 0;
    cmd.transfer_buffer_length = length;
    cmd.setup.bmRequestType = 0x80;
    cmd.setup.bRequest = 6;
    cmd.setup.wValue = value;
    cmd.setup.wLength = length;
    Send(fd, &cmd, sizeof(cmd));
    USBIP_RET_SUBMIT ret;
    Receive(fd, &ret, sizeof(ret));
    Require(ret.command == 3 && ret.seqnum == seqnum && ret.status == 0, "USBIP_RET_SUBMIT");
    std::vector<uint8_t> data(ret.actual_length);
    Receive(fd, data.data(), data.size());
    return data;
}

static std::vector<uint8_t> GetDeviceDescriptor(int fd, uint32_t seqnum) {
    return GetDescriptor(fd, seqnum, 0x0100, 18);
}

// Once the server has seen the last client go.
static int AttachWhenFree(unsigned int port) {
    uint32_t status = 0;
    for (int i = 0; i < 100; i++) {
        int fd = Attach(port, status);
        if (status == 0) return fd;
        close(fd);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    Require(false, "attach after the last client has gone");
    return -1;
}

static void RequireOP1(const List& list) {
    Require(list.ndev == 1, "one device");
    Require(std::strcmp(list.udev.busid, "1-1") == 0, "busid 1-1");
    Require(list.udev.busnum == 1 && list.udev.devnum == 2 && list.udev.speed == 3, "bus 1, device 2, high speed");
    Require(list.udev.idVendor == 0x2367 && list.udev.idProduct == 0x0004 && list.udev.bcdDevice == 0x0242,
            "the guest's ids");
    Require(list.udev.bDeviceClass == 0 && list.udev.bNumConfigurations == 3 && list.udev.bConfigurationValue == 1,
            "class, configurations and the one listed");
    // Each interface once, by alternate setting 0.
    Require(list.interfaces.size() == 4, "four interfaces");
    const uint8_t classes[4][3] = {{1, 1, 0}, {1, 2, 0}, {1, 2, 0}, {1, 3, 0}};
    for (int i = 0; i < 4; i++) {
        Require(list.interfaces[i].bInterfaceClass == classes[i][0] &&
                    list.interfaces[i].bInterfaceSubClass == classes[i][1] &&
                    list.interfaces[i].bInterfaceProtocol == classes[i][2],
                "audio control, two audio streaming, MIDI");
    }
}

int main() {
    auto loop = uvw::loop::create();
    FakeDevice device;
    USBIPServer server(*loop, device);
    server.Start(0);
    const unsigned int port = server.Port();
    // Never asked for a list before a client attaches.
    FakeDevice device2;
    device2.connected = true;
    USBIPServer server2(*loop, device2);
    server2.Start(0);
    auto stop = loop->resource<uvw::async_handle>();
    stop->on<uvw::async_event>([](const uvw::async_event&, uvw::async_handle& handle) {
        handle.parent().walk([](auto&& h) { h.close(); });
    });
    std::thread thread([loop] { loop->run(); });

    {
        // Before the firmware connects: nothing to list, nothing to attach,
        // and the guest is not asked.
        const auto list = ListDevices(port);
        Require(list.status == 0 && list.ndev == 0, "an empty bus before the guest connects");
        uint32_t status;
        close(Attach(port, status));
        Require(status == 1, "attach refused before the guest connects");
        Require(device.setups == 0, "the guest is not asked before it connects");
    }
    device.connected = true;
    {
        // The guest's descriptors, read over EP0: three requests.
        RequireOP1(ListDevices(port));
        Require(device.setups == 3, "device, config header, whole config");
    }
    {
        // An answer that is not what was asked for (as one to a request an
        // earlier client left would be) is an error, not an empty bus.
        device.wrongConfig = true;
        Require(ListDevices(port).status == 5, "ST_ERROR for a configuration that is not one");
        device.wrongConfig = false;
    }
    {
        // A guest that does not answer: the list ends, with nothing in it. An
        // attach meanwhile waits for it to end, so that its host's requests do
        // not meet the server's on EP0.
        device.answers = false;
        int fd = Connect(port);
        OP_REQ_HEADER req{};
        req.version = 0x0111;
        req.command = 0x8005;
        Send(fd, &req, sizeof(req));
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); // its request first
        const int before = device.setups;
        const auto start = std::chrono::steady_clock::now();
        uint32_t status;
        int attached = Attach(port, status);
        Require(status == 0, "attach while the server reads the descriptors");
        Require(std::chrono::steady_clock::now() - start > std::chrono::milliseconds(1000),
                "the attach answered once the read has ended");
        Require(device.setups == before, "the attach sends nothing to the guest");
        OP_REQ_HEADER rep;
        Receive(fd, &rep, sizeof(rep));
        Require(rep.command == 0x0005 && rep.status == 5, "ST_ERROR when the guest does not answer");
        Require(ClosedByServer(fd), "the server closes a list's connection");
        close(fd);
        close(attached);
        device.answers = true;
    }
    {
        // A list while a client is attached: from what its host read (not
        // what was known before it attached), without a request of our own on
        // EP0, and the client still served.
        int fd = AttachWhenFree(port);
        Require(GetDeviceDescriptor(fd, 1) == kDevice, "the attached host's request answered");
        Require(ListDevices(port).status == 5, "ST_ERROR before the attached host has read the configuration");
        Require(GetDescriptor(fd, 10, 0x0200, kConfig.size()) == kConfig, "the attached host reads the configuration");
        const int before = device.setups;
        RequireOP1(ListDevices(port));
        Require(device.setups == before, "no request of the server's while a client is attached");
        Require(GetDeviceDescriptor(fd, 2) == kDevice, "the attached client still served after a list");

        uint32_t status;
        int other = Attach(port, status);
        Require(status == 2, "a second attach is refused as busy");
        close(other);
        Require(GetDeviceDescriptor(fd, 3) == kDevice, "the attached client still served after a refused attach");
        close(fd);
    }
    {
        // Once it has gone, another can attach.
        int fd = AttachWhenFree(port);
        Require(GetDeviceDescriptor(fd, 1) == kDevice, "the new client served");
        close(fd);
    }
    {
        // A list while attached, before any of the server's own: from what the
        // attached host read.
        int fd = AttachWhenFree(server2.Port());
        Require(GetDeviceDescriptor(fd, 1) == kDevice, "the attached host reads the device descriptor");
        Require(GetDescriptor(fd, 2, 0x0200, 9).size() == 9, "and its configuration's header");
        Require(GetDescriptor(fd, 3, 0x0200, kConfig.size()) == kConfig, "and all of it");
        Require(device2.setups == 3, "only the attached host's requests");
        RequireOP1(ListDevices(server2.Port()));
        Require(device2.setups == 3, "a list while attached sends nothing to the guest");
        close(fd);
    }
    stop->send();
    thread.join();
    std::puts("usbipd test: ok");
    return 0;
}
