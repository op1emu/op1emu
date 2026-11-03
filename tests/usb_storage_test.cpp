// In-process USB host + BOT/SCSI + FatFs against a scripted device and RAM disk.
#include "storage/fat_storage.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

using op1::storage::FatStorage;
using op1::storage::UsbHost;

namespace {

void Require(bool ok, const char* message) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}

uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void PutBe32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
void PutLe32(uint8_t* p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}

class FakeDevice : public USBDevice {
public:
    enum class Phase { Command, DataIn, DataOut, Status };

    bool massStorage = true;
    bool connected = true;
    bool stall = false;
    bool wrongTag = false;
    bool badResidue = false;
    bool reentered = false;
    size_t services = 0;
    std::vector<uint8_t> disk = std::vector<uint8_t>(16 * 1024 * 1024, 0xff);

    bool IsConnected() const override { return connected; }
    bool ControlIdle() override { return !pending; }

    void HandleSetupPacket(USBSetupBytes setup, const u8*, std::size_t,
                           ReplyCallback callback) override {
        if (inCallback) reentered = true;
        std::vector<uint8_t> reply;
        if (setup.bmRequestType == 0x80 && setup.bRequest == 6) {
            const bool device = (setup.wValue >> 8) == 1;
            const auto& bytes = device ? deviceDescriptor : ConfigDescriptor();
            reply.assign(bytes.begin(), bytes.begin() + std::min<size_t>(bytes.size(), setup.wLength));
        } else if (setup.bRequest == 5 || setup.bRequest == 9) {
            ++configurations;
        } else {
            Require(false, "unexpected control request");
        }
        pending = [this, callback, reply = std::move(reply)] {
            callback(reply.data(), reply.size());
        };
    }

    void HandleDataWrite(int, int, const u8* data, std::size_t length,
                         WriteDoneCallback callback) override {
        if (inCallback) reentered = true;
        std::vector<uint8_t> bytes(data, data + length);
        pending = [this, callback, bytes = std::move(bytes)] {
            if (phase == Phase::Command) {
                Require(bytes.size() == 31 && Le32(bytes.data()) == 0x43425355, "valid CBW");
                tag = Le32(bytes.data() + 4);
                transferLength = Le32(bytes.data() + 8);
                cdb.assign(bytes.begin() + 15, bytes.end());
                dataIn = (bytes[12] & 0x80) != 0;
                PrepareCommand();
            } else {
                if (phase != Phase::DataOut || bytes.size() != transferLength) {
                    std::fprintf(stderr, "OUT phase=%d bytes=%zu expected=%u op=%02x\n", int(phase),
                                 bytes.size(), transferLength, cdb.empty() ? 0 : cdb[0]);
                    Require(false, "valid OUT data");
                }
                Require(writeLba + writeSectors <= disk.size() / 512, "WRITE10 range");
                std::copy(bytes.begin(), bytes.end(), disk.begin() + writeLba * 512);
                phase = Phase::Status;
            }
            callback();
        };
    }

    void HandleDataRead(int, int, std::size_t limit, ReplyCallback callback) override {
        if (inCallback) reentered = true;
        pending = [this, callback, limit] {
            std::vector<uint8_t> reply;
            if (phase == Phase::DataIn) {
                reply = std::move(input);
                phase = Phase::Status;
            } else {
                Require(phase == Phase::Status, "CSW phase");
                reply.assign(13, 0);
                PutLe32(reply.data(), 0x53425355);
                PutLe32(reply.data() + 4, wrongTag ? tag + 1 : tag);
                PutLe32(reply.data() + 8, badResidue ? 1 : 0);
                reply[12] = commandStatus;
                phase = Phase::Command;
            }
            Require(reply.size() == limit, "exact bulk IN length");
            callback(reply.data(), reply.size());
        };
    }

    void Service() {
        ++services;
        if (stall || !pending) return;
        auto job = std::move(pending);
        pending = nullptr;
        inCallback = true;
        job();
        inCallback = false;
    }

private:
    const std::vector<uint8_t> deviceDescriptor{
        0x12, 1, 0, 2, 0, 0, 0, 0x40, 0x67, 0x23, 2, 0, 1, 0, 1, 2, 3, 1,
    };
    std::vector<uint8_t> ConfigDescriptor() const {
        return {
            9, 2, 32, 0, 1, 1, 0, 0xc0, 50,
            9, 4, 0, 0, 2, uint8_t(massStorage ? 8 : 1), uint8_t(massStorage ? 6 : 3),
            uint8_t(massStorage ? 0x50 : 0), 0,
            7, 5, 1, 2, 64, 0, 0,
            7, 5, 0x82, 2, 64, 0, 0,
        };
    }

    void PrepareCommand() {
        commandStatus = 0;
        input.clear();
        const uint8_t op = cdb[0];
        if (op == 0x12) {  // INQUIRY
            input.assign(36, 0);
            input[0] = 0;
            input[2] = 6;
            input[3] = 2;
            input[4] = 31;
            std::memcpy(input.data() + 8, "OP1EMU  TE-BOOT RAMDISK", 23);
        } else if (op == 0x25) {  // READ CAPACITY(10)
            input.assign(8, 0);
            PutBe32(input.data(), disk.size() / 512 - 1);
            PutBe32(input.data() + 4, 512);
        } else if (op == 0x28) {  // READ(10)
            const uint32_t lba = Be32(&cdb[2]);
            const uint32_t count = (uint32_t(cdb[7]) << 8) | cdb[8];
            Require(lba + count <= disk.size() / 512 && transferLength == count * 512, "READ10 range");
            input.assign(disk.begin() + lba * 512, disk.begin() + (lba + count) * 512);
        } else if (op == 0x2a) {  // WRITE(10)
            writeLba = Be32(&cdb[2]);
            writeSectors = (uint32_t(cdb[7]) << 8) | cdb[8];
        } else if (op == 0x03) {  // REQUEST SENSE
            input.assign(18, 0);
            input[0] = 0x70;
            input[2] = 0x05;
        } else if (op != 0x00) {
            commandStatus = 1;
        }
        phase = transferLength ? (dataIn ? Phase::DataIn : Phase::DataOut) : Phase::Status;
    }

    std::function<void()> pending;
    bool inCallback = false;
    Phase phase = Phase::Command;
    std::vector<uint8_t> cdb;
    std::vector<uint8_t> input;
    uint32_t tag = 0;
    uint32_t transferLength = 0;
    uint32_t writeLba = 0;
    uint32_t writeSectors = 0;
    uint8_t commandStatus = 0;
    bool dataIn = false;
    int configurations = 0;
};

struct Fixture {
    FakeDevice device;
    uint64_t now = 0;
    UsbHost host{device, [this] { device.Service(); now += 10; return true; }, [this] { return now; }};
    FatStorage storage{host};
};

void Format(Fixture& f) {
    Require(f.storage.Attach(), f.host.Error().c_str());
    Require(f.storage.Format(), f.storage.Error().c_str());
    Require(f.storage.Mount(), f.storage.Error().c_str());
}

std::vector<uint8_t> Bytes(size_t size) {
    std::vector<uint8_t> bytes(size);
    for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t((i * 37 + i / 251) & 0xff);
    return bytes;
}

void WriteFile(FatStorage& storage, const std::string& path, const std::vector<uint8_t>& bytes,
               bool overwrite = false) {
    Require(storage.BeginWrite(path, overwrite), storage.Error().c_str());
    for (size_t at = 0; at < bytes.size();) {
        const size_t count = std::min<size_t>(65536, bytes.size() - at);
        Require(storage.Write(bytes.data() + at, count), storage.Error().c_str());
        at += count;
    }
    Require(storage.CommitWrite(), storage.Error().c_str());
}

void CheckFile(FatStorage& storage, const std::string& path, const std::vector<uint8_t>& bytes) {
    uint32_t size = 0;
    Require(storage.OpenRead(path, size), storage.Error().c_str());
    Require(size == bytes.size(), "file size");
    std::vector<uint8_t> actual(size);
    size_t at = 0;
    for (;;) {
        uint32_t count = 0;
        Require(storage.Read(actual.data() + at, std::min<uint32_t>(65536, size - at), count),
                storage.Error().c_str());
        if (!count) break;
        at += count;
    }
    Require(at == size && actual == bytes, "file digest/content");
    Require(storage.CloseRead(), storage.Error().c_str());
}

void TestMountFailureRetry() {
    Fixture f;
    Require(f.storage.Attach(), f.storage.Error().c_str());
    Require(!f.storage.Mount(), "unformatted media cannot mount");
    // The failed immediate mount still registered fs_. Formatting and retrying
    // must unregister/reuse it correctly rather than leaving a dangling pointer.
    Require(f.storage.Format(), f.storage.Error().c_str());
    Require(f.storage.Mount(), f.storage.Error().c_str());
    WriteFile(f.storage, "/after-retry.bin", Bytes(513));
    CheckFile(f.storage, "/after-retry.bin", Bytes(513));
}

void TestSuperfloppyAndPaths() {
    Fixture f;
    Require(f.storage.Attach(), f.storage.Error().c_str());
    MKFS_PARM options{};
    options.fmt = FM_FAT | FM_SFD;
    options.n_fat = 2;
    std::vector<uint8_t> work(FF_MAX_SS);
    Require(f_mkfs("0:", &options, work.data(), work.size()) == FR_OK, "format superfloppy");
    Require(f.storage.Mount(), f.storage.Error().c_str());
    WriteFile(f.storage, "/empty.bin", {});
    CheckFile(f.storage, "/empty.bin", {});
    Require(!f.storage.BeginWrite("/../escape", false), "parent traversal rejected");
    Require(!f.storage.BeginWrite("relative", false), "relative path rejected");
    Require(!f.storage.BeginWrite(std::string("/bad\0name", 9), false), "embedded NUL rejected");
    Require(!f.storage.BeginWrite("/bad\xff", false), "invalid UTF-8 rejected");
}

void TestFailedBeginRetry() {
    Fixture f;
    Format(f);
    Require(!f.storage.BeginWrite("/missing/file.bin", false), "temporary creation fails");
    WriteFile(f.storage, "/retry.bin", Bytes(17));
    f.storage.Disconnect();
    Require(f.storage.Connect(), f.storage.Error().c_str());
    WriteFile(f.storage, "/after-reconnect.bin", Bytes(31));
}

void TestDirectoryLimit() {
    Fixture f;
    Format(f);
    Require(f_mkdir("0:/many") == FR_OK, "mkdir many");
    for (int i = 0; i <= 2048; ++i) {
        char name[32];
        std::snprintf(name, sizeof(name), "0:/many/f%04d", i);
        FIL file{};
        Require(f_open(&file, name, FA_CREATE_NEW | FA_WRITE) == FR_OK, "create bounded-list fixture");
        Require(f_close(&file) == FR_OK, "close bounded-list fixture");
    }
    std::vector<op1::storage::StorageEntry> entries;
    Require(!f.storage.List("/many", entries), "directory display limit is enforced");
}

void TestFatRoundTrip() {
    Fixture f;
    Format(f);
    Require(f_mkdir("0:/samples") == FR_OK, "mkdir samples");
    Require(f_mkdir("0:/samples/nested") == FR_OK, "mkdir nested");
    const auto bytes = Bytes(300001);
    const std::string path = "/samples/nested/补丁 demo.op1";
    WriteFile(f.storage, path, bytes);
    CheckFile(f.storage, path, bytes);

    std::vector<op1::storage::StorageEntry> entries;
    Require(f.storage.List("/samples/nested", entries), f.storage.Error().c_str());
    Require(entries.size() == 1 && entries[0].name == "补丁 demo.op1" && !entries[0].directory,
            "UTF-8 long filename listing");
    Require(!f.storage.BeginWrite(path, false), "collision rejected");
    f.storage.AbortWrite();
    WriteFile(f.storage, path, Bytes(8193), true);
    CheckFile(f.storage, path, Bytes(8193));

    f.storage.Disconnect();
    Require(f.storage.Connect(), f.storage.Error().c_str());
    CheckFile(f.storage, path, Bytes(8193));
    Require(!f.device.reentered, "no controller reentry from callbacks");
}

void TestNotMassStorage() {
    Fixture f;
    f.device.massStorage = false;
    Require(!f.storage.Connect(), "audio/MIDI-only device is not mounted as MSC");
    Require(f.storage.Error().find("mass storage") != std::string::npos, "useful incompatible error");
}

void TestBadCswAndLateCallback() {
    Fixture bad;
    bad.device.wrongTag = true;
    Require(!bad.storage.Connect(), "CSW tag mismatch rejected");
    Fixture residue;
    residue.device.badResidue = true;
    Require(!residue.storage.Connect(), "CSW residue rejected");

    Fixture slow;
    Format(slow);
    slow.storage.Disconnect();
    slow.device.stall = true;
    Require(!slow.storage.Connect(), "timeout rejected");
    Require(!slow.host.Quiescent(), "uncancelled callback remains outstanding");
    slow.device.stall = false;
    for (int i = 0; i < 8 && !slow.host.Quiescent(); ++i) slow.device.Service();
    Require(slow.host.Quiescent(), "late callback drains before reconnect");
}

}  // namespace

int main() {
    TestNotMassStorage();
    TestBadCswAndLateCallback();
    TestMountFailureRetry();
    TestSuperfloppyAndPaths();
    TestFailedBeginRetry();
    TestDirectoryLimit();
    TestFatRoundTrip();
    std::puts("usb-storage-test: descriptors, BOT/SCSI, FAT/LFN, upload/download and failure paths: ok");
}
