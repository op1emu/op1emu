#pragma once

#include "ff.h"
#include "diskio.h"
#include "storage/usb_msc.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace op1::storage {

struct StorageEntry {
    std::string name;
    uint32_t size = 0;
    bool directory = false;
    uint16_t modifiedDate = 0;
    uint16_t modifiedTime = 0;
};

class FatStorage;
bool ActivateFatStorage(FatStorage* storage);
void DeactivateFatStorage(FatStorage* storage);

class FatStorage {
public:
    explicit FatStorage(UsbHost& host);
    ~FatStorage();

    bool Connect();
    bool Attach();
    bool Mount();
    bool Format();  // native fixture support; deliberately not exported to the page
    void Disconnect();

    bool List(const std::string& path, std::vector<StorageEntry>& entries);
    bool OpenRead(const std::string& path, uint32_t& size);
    bool Read(void* data, uint32_t length, uint32_t& read);
    bool CloseRead();
    bool BeginWrite(const std::string& path, bool overwrite);
    bool Write(const void* data, uint32_t length);
    bool CommitWrite();
    void AbortWrite();

    bool Mounted() const { return mounted_; }
    UsbMassStorage& Block() { return msc_; }
    const std::string& Error() const { return error_; }

    DSTATUS DiskStatus() const;
    DRESULT DiskRead(uint8_t* data, uint32_t sector, uint32_t count);
    DRESULT DiskWrite(const uint8_t* data, uint32_t sector, uint32_t count);
    DRESULT DiskIoctl(uint8_t command, void* data);

private:
    bool Check(FRESULT result, const std::string& operation);
    bool Fail(const std::string& message);
    bool Path(const std::string& path, std::string& result);
    bool Healthy() const;

    UsbMassStorage msc_;
    std::unique_ptr<FATFS> fs_ = std::make_unique<FATFS>();
    FIL file_{};
    bool attached_ = false;
    bool registered_ = false;
    bool mounted_ = false;
    bool fileOpen_ = false;
    bool writing_ = false;
    bool overwrite_ = false;
    std::string temporaryPath_;
    std::string finalPath_;
    std::string backupPath_;
    std::string error_;
};

}  // namespace op1::storage
