#pragma once

#include "storage/usb_host.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace op1::storage {

class UsbMassStorage {
public:
    explicit UsbMassStorage(UsbHost& host) : host_(host) {}

    bool Connect();
    void Reset();
    bool Read(uint32_t sector, uint32_t count, std::vector<uint8_t>& data);
    bool Write(uint32_t sector, uint32_t count, const uint8_t* data);

    bool Ready() const { return ready_; }
    uint32_t SectorSize() const { return sectorSize_; }
    uint32_t SectorCount() const { return sectorCount_; }
    const std::string& Error() const { return error_; }

private:
    bool Inquiry();
    bool ReadCapacity();
    bool Transaction(const std::vector<uint8_t>& cdb, uint8_t* data, size_t length, bool in,
                     bool senseOnError = true);
    bool RequestSense();
    bool Fail(const std::string& message);
    bool CheckRange(uint32_t sector, uint32_t count) const;

    UsbHost& host_;
    bool ready_ = false;
    int bulkIn_ = 0;
    int bulkOut_ = 0;
    uint32_t sectorSize_ = 0;
    uint32_t sectorCount_ = 0;
    uint32_t nextTag_ = 1;
    std::string error_;
};

}  // namespace op1::storage
