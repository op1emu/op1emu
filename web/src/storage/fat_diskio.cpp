#include "storage/fat_storage.h"

#include <diskio.h>

namespace op1::storage {
namespace {
FatStorage* active = nullptr;
}  // namespace

bool ActivateFatStorage(FatStorage* storage) {
    if (active || !storage) return false;
    active = storage;
    return true;
}

void DeactivateFatStorage(FatStorage* storage) {
    if (active == storage) active = nullptr;
}

}  // namespace op1::storage

extern "C" {
PARTITION VolToPart[FF_VOLUMES] = {{0, 0}};  // physical drive 0, auto MBR/superfloppy
}

extern "C" DSTATUS disk_initialize(BYTE pdrv) {
    return pdrv == 0 && op1::storage::active ? op1::storage::active->DiskStatus() : STA_NOINIT;
}
extern "C" DSTATUS disk_status(BYTE pdrv) {
    return pdrv == 0 && op1::storage::active ? op1::storage::active->DiskStatus() : STA_NOINIT;
}
extern "C" DRESULT disk_read(BYTE pdrv, BYTE* data, LBA_t sector, UINT count) {
    if (pdrv != 0 || !op1::storage::active) return RES_NOTRDY;
    return op1::storage::active->DiskRead(data, sector, count);
}
extern "C" DRESULT disk_write(BYTE pdrv, const BYTE* data, LBA_t sector, UINT count) {
    if (pdrv != 0 || !op1::storage::active) return RES_NOTRDY;
    return op1::storage::active->DiskWrite(data, sector, count);
}
extern "C" DRESULT disk_ioctl(BYTE pdrv, BYTE command, void* data) {
    if (pdrv != 0 || !op1::storage::active) return RES_NOTRDY;
    return op1::storage::active->DiskIoctl(command, data);
}
