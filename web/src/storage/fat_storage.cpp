#include "storage/fat_storage.h"

#include <diskio.h>

#include <algorithm>
#include <array>

namespace op1::storage {
namespace {

constexpr size_t kMaxDirectoryEntries = 2048;

bool ValidUtf8(const std::string& path) {
    for (size_t i = 0; i < path.size();) {
        const uint8_t c = path[i];
        if (c < 0x80) {
            if (c < 0x20 || c == 0x7f) return false;
            ++i;
            continue;
        }
        size_t count = 0;
        if (c >= 0xc2 && c <= 0xdf) count = 2;
        else if (c >= 0xe0 && c <= 0xef) count = 3;
        else if (c >= 0xf0 && c <= 0xf4) count = 4;
        else return false;
        if (i + count > path.size()) return false;
        for (size_t j = 1; j < count; ++j)
            if ((uint8_t(path[i + j]) & 0xc0) != 0x80) return false;
        if (count == 3) {
            const uint8_t next = path[i + 1];
            if ((c == 0xe0 && next < 0xa0) || (c == 0xed && next >= 0xa0)) return false;
        } else if (count == 4) {
            const uint8_t next = path[i + 1];
            if ((c == 0xf0 && next < 0x90) || (c == 0xf4 && next >= 0x90)) return false;
        }
        i += count;
    }
    return true;
}

const char* ResultName(FRESULT result) {
    static constexpr const char* names[] = {
        "OK", "disk I/O error", "internal error", "not ready", "no file", "no path",
        "invalid name", "denied", "already exists", "invalid object", "write protected",
        "invalid drive", "volume not mounted", "no FAT filesystem", "format aborted", "timeout",
        "locked", "not enough memory", "too many open files", "invalid parameter",
    };
    const unsigned index = static_cast<unsigned>(result);
    return index < std::size(names) ? names[index] : "unknown FatFs error";
}

}  // namespace

FatStorage::FatStorage(UsbHost& host) : msc_(host) {}
FatStorage::~FatStorage() { Disconnect(); }

bool FatStorage::Fail(const std::string& message) {
    error_ = message;
    return false;
}

bool FatStorage::Check(FRESULT result, const std::string& operation) {
    if (result == FR_OK) return true;
    return Fail(operation + ": " + ResultName(result));
}

bool FatStorage::Healthy() const { return attached_ && msc_.Ready(); }

bool FatStorage::Attach() {
    if (attached_) return Fail("storage is already attached");
    if (!ActivateFatStorage(this)) return Fail("another storage session is active");
    if (!msc_.Connect()) {
        DeactivateFatStorage(this);
        return Fail(msc_.Error());
    }
    attached_ = true;
    error_.clear();
    return true;
}

bool FatStorage::Mount() {
    if (!Healthy()) return Fail("mass storage is not ready");
    if (mounted_) return true;
    // f_mount() registers the work area even when its immediate mount fails.
    // Keep that registration until Disconnect, never free a registered FATFS.
    registered_ = true;
    if (!Check(f_mount(fs_.get(), "0:", 1), "mount FAT")) return false;
    mounted_ = true;
    return true;
}

bool FatStorage::Connect() {
    if (!Attach()) return false;
    if (Mount()) return true;
    const std::string message = error_;
    Disconnect();
    error_ = message;
    return false;
}

bool FatStorage::Format() {
    if (!Healthy()) return Fail("mass storage is not ready");
    if (registered_) {
        f_unmount("0:");
        registered_ = false;
        mounted_ = false;
    }
    MKFS_PARM options{};
    options.fmt = FM_FAT;
    options.n_fat = 2;
    std::vector<uint8_t> work(FF_MAX_SS);
    return Check(f_mkfs("0:", &options, work.data(), work.size()), "format FAT fixture");
}

void FatStorage::Disconnect() {
    if (writing_) AbortWrite();
    if (fileOpen_) {
        if (Healthy()) f_close(&file_);
        fileOpen_ = false;
    }
    if (registered_) {
        f_unmount("0:");
        registered_ = false;
    }
    mounted_ = false;
    file_ = {};
    temporaryPath_.clear();
    finalPath_.clear();
    backupPath_.clear();
    writing_ = false;
    fs_ = std::make_unique<FATFS>();
    attached_ = false;
    DeactivateFatStorage(this);
    msc_.Reset();
    error_.clear();
}

bool FatStorage::Path(const std::string& path, std::string& result) {
    if (path.empty() || path[0] != '/' || path.find('\0') != std::string::npos || !ValidUtf8(path))
        return Fail("invalid storage path");
    if (path.size() > 1 && path.back() == '/') return Fail("path must not have a trailing slash");
    size_t at = 1;
    while (at < path.size()) {
        const size_t slash = path.find('/', at);
        const size_t end = slash == std::string::npos ? path.size() : slash;
        const std::string part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == ".." || part.find(':') != std::string::npos ||
            part.find('\\') != std::string::npos)
            return Fail("storage path escapes the mounted volume");
        at = end + 1;
    }
    result = "0:" + path;
    return true;
}

bool FatStorage::List(const std::string& path, std::vector<StorageEntry>& entries) {
    if (!mounted_ || fileOpen_) return Fail("storage is not ready for directory listing");
    std::string fatPath;
    if (!Path(path, fatPath)) return false;
    DIR dir{};
    if (!Check(f_opendir(&dir, fatPath.c_str()), "open directory")) return false;
    std::vector<StorageEntry> result;
    for (;;) {
        FILINFO info{};
        const FRESULT read = f_readdir(&dir, &info);
        if (read != FR_OK) {
            f_closedir(&dir);
            return Check(read, "read directory");
        }
        if (!info.fname[0]) break;
        if (result.size() >= kMaxDirectoryEntries) {
            f_closedir(&dir);
            return Fail("directory exceeds the 2048-entry display limit");
        }
        const std::string name = info.fname;
        if (name == "." || name == "..") continue;
        result.push_back({name, static_cast<uint32_t>(info.fsize), (info.fattrib & AM_DIR) != 0,
            info.fdate, info.ftime});
    }
    if (!Check(f_closedir(&dir), "close directory")) return false;
    entries = std::move(result);
    return true;
}

bool FatStorage::OpenRead(const std::string& path, uint32_t& size) {
    if (!mounted_ || fileOpen_) return Fail("storage is not ready to open a file");
    std::string fatPath;
    if (!Path(path, fatPath)) return false;
    if (!Check(f_open(&file_, fatPath.c_str(), FA_READ), "open file")) return false;
    fileOpen_ = true;
    writing_ = false;
    size = f_size(&file_);
    return true;
}

bool FatStorage::Read(void* data, uint32_t length, uint32_t& read) {
    read = 0;
    if (!fileOpen_ || writing_ || (!data && length)) return Fail("invalid file read");
    if (!length) return true;
    UINT count = 0;
    if (!Check(f_read(&file_, data, length, &count), "read file")) return false;
    read = count;
    return true;
}

bool FatStorage::CloseRead() {
    if (!fileOpen_ || writing_) return Fail("no file is open for reading");
    const FRESULT result = f_close(&file_);
    fileOpen_ = false;
    file_ = {};
    return Check(result, "close file");
}

bool FatStorage::BeginWrite(const std::string& path, bool overwrite) {
    if (!mounted_ || fileOpen_) return Fail("storage is not ready to create a file");
    if (!temporaryPath_.empty()) return Fail("finish or abort the previous upload first");
    backupPath_.clear();
    finalPath_.clear();
    if (!Path(path, finalPath_)) return false;
    FILINFO info{};
    const FRESULT exists = f_stat(finalPath_.c_str(), &info);
    if (exists == FR_OK && (info.fattrib & AM_DIR)) return Fail("destination is a directory");
    if (exists == FR_OK && !overwrite) return Fail("destination already exists");
    if (exists != FR_OK && exists != FR_NO_FILE && exists != FR_NO_PATH)
        return Check(exists, "check destination");
    overwrite_ = overwrite;

    static uint32_t sequence = 0;
    const size_t slash = path.rfind('/');
    const std::string parent = path.substr(0, slash + 1);
    std::string temporary;
    for (int attempt = 0; attempt < 100; ++attempt) {
        temporary = parent + ".op1-upload-" + std::to_string(++sequence) + ".part";
        if (!Path(temporary, temporaryPath_)) return false;
        const FRESULT result = f_open(&file_, temporaryPath_.c_str(), FA_CREATE_NEW | FA_WRITE);
        if (result == FR_EXIST) continue;
        if (result != FR_OK) {
            Check(result, "create temporary upload");
            const std::string message = error_;
            if (Healthy()) f_unlink(temporaryPath_.c_str());
            temporaryPath_.clear();
            finalPath_.clear();
            backupPath_.clear();
            file_ = {};
            error_ = message;
            return false;
        }
        fileOpen_ = writing_ = true;
        return true;
    }
    temporaryPath_.clear();
    finalPath_.clear();
    backupPath_.clear();
    return Fail("cannot allocate a temporary upload name");
}

bool FatStorage::Write(const void* data, uint32_t length) {
    if (!fileOpen_ || !writing_ || (!data && length)) return Fail("invalid file write");
    if (!length) return true;
    UINT count = 0;
    if (!Check(f_write(&file_, data, length, &count), "write upload")) return false;
    return count == length ? true : Fail("short upload write (disk full?)");
}

bool FatStorage::CommitWrite() {
    if (!writing_ || (fileOpen_ && temporaryPath_.empty())) return Fail("no upload to commit");
    if (fileOpen_) {
        const FRESULT synced = f_sync(&file_);
        const FRESULT closed = f_close(&file_);
        fileOpen_ = false;
        file_ = {};
        if (!Check(synced, "sync upload") || !Check(closed, "close upload")) return false;
    }

    if (overwrite_ && backupPath_.empty()) {
        FILINFO info{};
        const FRESULT exists = f_stat(finalPath_.c_str(), &info);
        if (exists != FR_OK && exists != FR_NO_FILE) return Check(exists, "check replaced file");
        if (exists == FR_OK) {
            static uint32_t sequence = 0;
            const size_t volumeAndSlash = finalPath_.rfind('/');
            const std::string parent = finalPath_.substr(0, volumeAndSlash + 1);
            for (int attempt = 0; attempt < 100 && backupPath_.empty(); ++attempt) {
                const std::string candidate = parent + ".op1-backup-" + std::to_string(++sequence) + ".part";
                if (f_stat(candidate.c_str(), &info) == FR_NO_FILE) backupPath_ = candidate;
            }
            if (backupPath_.empty()) return Fail("cannot allocate an overwrite backup name");
            if (!Check(f_rename(finalPath_.c_str(), backupPath_.c_str()), "back up destination")) return false;
        }
    }

    const FRESULT renamed = f_rename(temporaryPath_.c_str(), finalPath_.c_str());
    if (renamed != FR_OK) {
        std::string message = std::string("commit upload: ") + ResultName(renamed);
        if (!backupPath_.empty()) {
            const FRESULT restored = f_rename(backupPath_.c_str(), finalPath_.c_str());
            if (restored == FR_OK) backupPath_.clear();
            else message += std::string("; restoring the original also failed: ") + ResultName(restored);
        }
        return Fail(message);
    }
    if (!backupPath_.empty()) {
        // The new destination is already in place. A stale backup is safer
        // than reporting the committed upload as failed solely for cleanup.
        f_unlink(backupPath_.c_str());
        backupPath_.clear();
    }
    temporaryPath_.clear();
    finalPath_.clear();
    writing_ = false;
    return true;
}

void FatStorage::AbortWrite() {
    if (temporaryPath_.empty()) return;
    if (Healthy()) {
        if (fileOpen_) f_close(&file_);
        f_unlink(temporaryPath_.c_str());
        if (!backupPath_.empty()) {
            FILINFO info{};
            if (f_stat(finalPath_.c_str(), &info) == FR_NO_FILE) f_rename(backupPath_.c_str(), finalPath_.c_str());
        }
    }
    fileOpen_ = writing_ = false;
    file_ = {};
    temporaryPath_.clear();
    finalPath_.clear();
    backupPath_.clear();
}

DSTATUS FatStorage::DiskStatus() const { return Healthy() ? 0 : STA_NOINIT; }

DRESULT FatStorage::DiskRead(uint8_t* data, uint32_t sector, uint32_t count) {
    if (!Healthy()) return RES_NOTRDY;
    std::vector<uint8_t> bytes;
    if (!msc_.Read(sector, count, bytes)) return RES_ERROR;
    std::copy(bytes.begin(), bytes.end(), data);
    return RES_OK;
}

DRESULT FatStorage::DiskWrite(const uint8_t* data, uint32_t sector, uint32_t count) {
    if (!Healthy()) return RES_NOTRDY;
    return msc_.Write(sector, count, data) ? RES_OK : RES_ERROR;
}

DRESULT FatStorage::DiskIoctl(uint8_t command, void* data) {
    if (!Healthy()) return RES_NOTRDY;
    switch (command) {
        case CTRL_SYNC: return RES_OK;  // every BOT WRITE(10) has completed synchronously
        case GET_SECTOR_COUNT:
            *static_cast<DWORD*>(data) = msc_.SectorCount();
            return RES_OK;
        case GET_SECTOR_SIZE:
            *static_cast<WORD*>(data) = msc_.SectorSize();
            return RES_OK;
        case GET_BLOCK_SIZE:
            *static_cast<DWORD*>(data) = 1;
            return RES_OK;
        default: return RES_PARERR;
    }
}

}  // namespace op1::storage
