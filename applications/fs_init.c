/*
 * fs_init - Mount the FAT filesystem from onboard SPI NOR flash.
 *
 * Mounts block device "norflash0" (registered by board/ports/spi_flash_init.c)
 * to "/" with elmFAT. A bare flash carries no FAT volume, so the first mount
 * failure triggers a one-time format before retrying.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

#if defined(BSP_USING_SPI_FLASH) && defined(RT_USING_DFS_ELMFAT)

#include <dfs_fs.h>

#define DBG_TAG "app.fs"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define FS_DEVICE_NAME "norflash0"
#define FS_MOUNT_PATH  "/"
#define FS_TYPE_NAME   "elm"

static int app_fs_mount(void)
{
    if (dfs_mount(FS_DEVICE_NAME, FS_MOUNT_PATH, FS_TYPE_NAME, 0, RT_NULL) == 0)
    {
        LOG_I("FAT filesystem mounted on %s", FS_MOUNT_PATH);
        return RT_EOK;
    }

    /* Bare flash has no FAT volume yet: format once, then retry. */
    LOG_W("mount %s failed, formatting with FAT...", FS_DEVICE_NAME);
    if (dfs_mkfs(FS_TYPE_NAME, FS_DEVICE_NAME) != 0)
    {
        LOG_E("format %s failed", FS_DEVICE_NAME);
        return -RT_ERROR;
    }

    if (dfs_mount(FS_DEVICE_NAME, FS_MOUNT_PATH, FS_TYPE_NAME, 0, RT_NULL) != 0)
    {
        LOG_E("mount %s on %s failed after format", FS_DEVICE_NAME, FS_MOUNT_PATH);
        return -RT_ERROR;
    }

    LOG_I("FAT filesystem mounted on %s", FS_MOUNT_PATH);
    return RT_EOK;
}
INIT_ENV_EXPORT(app_fs_mount);

#endif /* BSP_USING_SPI_FLASH && RT_USING_DFS_ELMFAT */
