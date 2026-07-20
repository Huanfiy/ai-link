/*
 * spi_flash_init - Onboard W25Q64 SPI NOR flash registration.
 *
 * Attaches the flash as SPI device "spi20" on bus "spi2" (CS on PB12,
 * per schematic FLASH_CS net), then probes it via SFUD to expose the
 * block device "norflash0" for filesystem mounting.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <rtthread.h>

#ifdef BSP_USING_SPI_FLASH

#include <drv_spi.h>
#include <drv_gpio.h>
#include <dev_spi_flash_sfud.h>

#define DBG_TAG "drv.spiflash"
#define DBG_LVL DBG_INFO
#include <rtdbg.h>

#define SPI_FLASH_BUS_NAME "spi2"
#define SPI_FLASH_DEV_NAME "spi20"
#define SPI_FLASH_BLK_NAME "norflash0"
#define SPI_FLASH_CS_PIN   GET_PIN(B, 12)

static int rt_hw_spi_flash_init(void)
{
    rt_err_t err;

    err = rt_hw_spi_device_attach(SPI_FLASH_BUS_NAME, SPI_FLASH_DEV_NAME, SPI_FLASH_CS_PIN);
    if (err != RT_EOK)
    {
        LOG_E("attach %s to %s failed: %d", SPI_FLASH_DEV_NAME, SPI_FLASH_BUS_NAME, err);
        return err;
    }

    if (rt_sfud_flash_probe(SPI_FLASH_BLK_NAME, SPI_FLASH_DEV_NAME) == RT_NULL)
    {
        LOG_E("SFUD probe %s on %s failed", SPI_FLASH_BLK_NAME, SPI_FLASH_DEV_NAME);
        return -RT_ERROR;
    }

    return RT_EOK;
}
INIT_COMPONENT_EXPORT(rt_hw_spi_flash_init);

#endif /* BSP_USING_SPI_FLASH */
