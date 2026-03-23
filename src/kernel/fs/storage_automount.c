#include <kernel/drivers/block_dev.h>
#include <kernel/klog.h>
#include "fs_fat32.h"
#include "fs_ext4.h"
#include "fs_ntfs.h"

/*
 * Try common on-disk layouts on the active block backend.
 * Order: FAT32 (ESP / removable), EXT4 (Linux data), NTFS stub (Windows).
 */
void storage_automount_all(void)
{
    if (!block_dev_is_available()) {
        klog("[storage] skip automount: no block device\n");
        return;
    }
    if (fat32_try_mount("/fat") == 0)
        klog("[storage] FAT32 -> /fat\n");
    if (ext4_try_mount("/ext4") == 0)
        klog("[storage] EXT4 -> /ext4\n");
    if (ntfs_try_mount("/ntfs") == 0)
        klog("[storage] NTFS (stub) -> /ntfs\n");
}
