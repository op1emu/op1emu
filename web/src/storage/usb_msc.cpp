#include "storage/usb_msc.h"

#include <cstring>

namespace op1::storage {
namespace {

uint32_t Be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}
void PutBe32(uint8_t* p, uint32_t v) {
    p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v;
}
void PutLe32(uint8_t* p, uint32_t v) {
    p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24;
}
uint32_t Le32(const uint8_t* p) {
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

}  // namespace

bool UsbMassStorage::Fail(const std::string& message) {
    ready_ = false;
    error_ = message;
    return false;
}

void UsbMassStorage::Reset() {
    ready_ = false;
    bulkIn_ = bulkOut_ = 0;
    sectorSize_ = sectorCount_ = 0;
    error_.clear();
}

bool UsbMassStorage::Connect() {
    Reset();
    UsbHost::Device device;
    if (!host_.Enumerate(device)) return Fail(host_.Error());
    const auto* interface = FindInterface(device, 0x08, 0x06, 0x50);
    if (!interface) return Fail("USB device has no mass storage interface (08/06/50)");
    const auto* in = FindBulkEndpoint(*interface, true);
    const auto* out = FindBulkEndpoint(*interface, false);
    if (!in || !out) return Fail("mass storage interface lacks bulk endpoints");
    bulkIn_ = in->address & 0x0f;
    bulkOut_ = out->address & 0x0f;

    if (!Inquiry()) return false;
    // UNIT ATTENTION immediately after enumeration is normal. Retry only the
    // SCSI command, and only when the transport itself is healthy.
    const std::vector<uint8_t> testUnitReady(6, 0);
    if (!Transaction(testUnitReady, nullptr, 0, true)) {
        if (!host_.Quiescent()) return false;
        error_.clear();
        if (!Transaction(testUnitReady, nullptr, 0, true)) return false;
    }
    if (!ReadCapacity()) return false;
    ready_ = true;
    return true;
}

bool UsbMassStorage::Inquiry() {
    std::vector<uint8_t> data(36);
    std::vector<uint8_t> cdb(6, 0);
    cdb[0] = 0x12;
    cdb[4] = data.size();
    return Transaction(cdb, data.data(), data.size(), true);
}

bool UsbMassStorage::ReadCapacity() {
    std::vector<uint8_t> data(8);
    std::vector<uint8_t> cdb(10, 0);
    cdb[0] = 0x25;
    if (!Transaction(cdb, data.data(), data.size(), true)) return false;
    const uint32_t last = Be32(data.data());
    const uint32_t size = Be32(data.data() + 4);
    if (last == UINT32_MAX) return Fail("LUN requires READ CAPACITY(16), which is not supported");
    if (size != 512 && size != 1024 && size != 2048 && size != 4096)
        return Fail("unsupported mass storage sector size");
    sectorCount_ = last + 1;
    sectorSize_ = size;
    return sectorCount_ != 0 ? true : Fail("empty mass storage LUN");
}

bool UsbMassStorage::Transaction(const std::vector<uint8_t>& cdb, uint8_t* data, size_t length,
                                 bool in, bool senseOnError) {
    if (cdb.empty() || cdb.size() > 16 || length > UINT32_MAX || (!data && length))
        return Fail("invalid SCSI request");

    std::vector<uint8_t> cbw(31, 0);
    PutLe32(cbw.data(), 0x43425355);
    const uint32_t tag = nextTag_++;
    PutLe32(cbw.data() + 4, tag);
    PutLe32(cbw.data() + 8, length);
    cbw[12] = in ? 0x80 : 0;
    cbw[13] = 0;  // LUN 0: the only personality needed by te-boot
    cbw[14] = cdb.size();
    std::memcpy(cbw.data() + 15, cdb.data(), cdb.size());
    if (!host_.BulkWrite(bulkOut_, cbw.data(), cbw.size())) return Fail(host_.Error());
    if (length) {
        if (in) {
            std::vector<uint8_t> input;
            if (!host_.BulkRead(bulkIn_, input, length)) return Fail(host_.Error());
            std::memcpy(data, input.data(), length);
        } else if (!host_.BulkWrite(bulkOut_, data, length)) {
            return Fail(host_.Error());
        }
    }

    std::vector<uint8_t> csw;
    if (!host_.BulkRead(bulkIn_, csw, 13)) return Fail(host_.Error());
    if (Le32(csw.data()) != 0x53425355 || Le32(csw.data() + 4) != tag)
        return Fail("invalid BOT command status wrapper");
    if (Le32(csw.data() + 8) != 0) return Fail("unexpected BOT data residue");
    if (csw[12] == 2) return Fail("BOT phase error");
    if (csw[12] != 0) {
        if (senseOnError) RequestSense();
        return Fail("SCSI CHECK CONDITION");
    }
    return true;
}

bool UsbMassStorage::RequestSense() {
    std::vector<uint8_t> data(18);
    std::vector<uint8_t> cdb(6, 0);
    cdb[0] = 0x03;
    cdb[4] = data.size();
    return Transaction(cdb, data.data(), data.size(), true, false);
}

bool UsbMassStorage::CheckRange(uint32_t sector, uint32_t count) const {
    const uint64_t end = uint64_t(sector) + count;
    return ready_ && count && end <= sectorCount_ && count <= SIZE_MAX / sectorSize_;
}

bool UsbMassStorage::Read(uint32_t sector, uint32_t count, std::vector<uint8_t>& data) {
    if (!CheckRange(sector, count)) return Fail("invalid or out-of-range READ(10)");
    const size_t bytes = count * sectorSize_;
    std::vector<uint8_t> cdb(10, 0);
    cdb[0] = 0x28;
    PutBe32(cdb.data() + 2, sector);
    cdb[7] = count >> 8;
    cdb[8] = count;
    if (count > UINT16_MAX) return Fail("READ(10) is too large");
    data.resize(bytes);
    return Transaction(cdb, data.data(), bytes, true);
}

bool UsbMassStorage::Write(uint32_t sector, uint32_t count, const uint8_t* data) {
    if (!CheckRange(sector, count) || !data) return Fail("invalid or out-of-range WRITE(10)");
    if (count > UINT16_MAX) return Fail("WRITE(10) is too large");
    std::vector<uint8_t> cdb(10, 0);
    cdb[0] = 0x2a;
    PutBe32(cdb.data() + 2, sector);
    cdb[7] = count >> 8;
    cdb[8] = count;
    return Transaction(cdb, const_cast<uint8_t*>(data), count * sectorSize_, false);
}

}  // namespace op1::storage
