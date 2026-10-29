// FatFs <-> flash_disk glue (single volume)
#include "fatfs/ff.h"
#include "fatfs/diskio.h"
#include "flash_disk.h"

DSTATUS disk_status(BYTE pdrv)     { return pdrv ? STA_NOINIT : 0; }
DSTATUS disk_initialize(BYTE pdrv) { return pdrv ? STA_NOINIT : 0; }

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv) return RES_PARERR;
    flashdisk_read((uint32_t)sector, buff, count);
    return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count) {
    if (pdrv) return RES_PARERR;
    flashdisk_write((uint32_t)sector, buff, count);
    return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff) {
    if (pdrv) return RES_PARERR;
    switch (cmd) {
        case CTRL_SYNC:        flashdisk_flush(); return RES_OK;
        case GET_SECTOR_COUNT: *(LBA_t *)buff = DISK_BLOCK_COUNT; return RES_OK;
        case GET_SECTOR_SIZE:  *(WORD *)buff = DISK_BLOCK_SIZE; return RES_OK;
        case GET_BLOCK_SIZE:   *(DWORD *)buff = 4096 / DISK_BLOCK_SIZE; return RES_OK;
        default:               return RES_PARERR;
    }
}
