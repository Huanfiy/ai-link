/*
 * Copyright (c) 2006-2021, RT-Thread Development Team
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * Change Logs:
 * Date           Author       Notes
 * 2018-11-06     SummerGift   first version
 * 2019-01-08     AndeyQi      add stm32f446-st-nucleo bsp
 * 2026-07-21     ailink       180MHz main PLL + PLLSAI 48MHz for USB OTG_FS
 */

#include <board.h>
#include <drv_common.h>

/* Derive PLL dividers from HSE_VALUE (stm32f4xx_hal_conf.h) so the clock tree
 * follows the crystal in one place: PFD is pinned at 2MHz for both PLLs.
 *   main PLL : HSE/M(=HSE/2M) *180 /2 -> 180MHz SYSCLK
 *   PLLSAI   : HSE/M(=HSE/2M) *192 /8 -> 48MHz  -> CK48MSEL -> OTG_FS/SDIO
 */
#define PLL_M_FOR_2MHZ_PFD (HSE_VALUE / 2000000U)

void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};
  RCC_PeriphCLKInitTypeDef PeriphClkInit = {0};

  /* Point VTOR at our own vector table (app links at 0x08010000). The boot
   * jump path sets it too, but the dfu-util ":leave" path jumps here straight
   * from the ROM bootloader without a reset, leaving VTOR at the ROM's value.
   * This is the earliest project-owned hook on that path: CMSIS SystemInit
   * (packages/, compiled without USER_VECT_TAB_ADDRESS) never touches VTOR,
   * and no NVIC line has been enabled by us before this point. */
  extern uint32_t g_pfnVectors[];
  SCB->VTOR = (uint32_t)g_pfnVectors;
  __DSB();
  __ISB();

  /**Configure the main internal regulator output voltage
  */
  __HAL_RCC_PWR_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
  /**Initializes the CPU, AHB and APB busses clocks
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = PLL_M_FOR_2MHZ_PFD;
  RCC_OscInitStruct.PLL.PLLN = 180;
  RCC_OscInitStruct.PLL.PLLP = RCC_PLLP_DIV2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }
  /**Activate the Over-Drive mode
  */
  if (HAL_PWREx_EnableOverDrive() != HAL_OK)
  {
    Error_Handler();
  }
  /**Initializes the CPU, AHB and APB busses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /* 48MHz for OTG_FS from PLLSAI (the main PLL keeps 180MHz, PLLQ unusable):
   * PLLSAI VCO = 2MHz * 192 = 384MHz, /8 = 48MHz, routed via CK48MSEL. */
  PeriphClkInit.PeriphClockSelection = RCC_PERIPHCLK_CLK48;
  PeriphClkInit.PLLSAI.PLLSAIM = PLL_M_FOR_2MHZ_PFD;
  PeriphClkInit.PLLSAI.PLLSAIN = 192;
  PeriphClkInit.PLLSAI.PLLSAIP = RCC_PLLSAIP_DIV8;
  PeriphClkInit.PLLSAI.PLLSAIQ = 2;
  PeriphClkInit.Clk48ClockSelection = RCC_CLK48CLKSOURCE_PLLSAIP;
  if (HAL_RCCEx_PeriphCLKConfig(&PeriphClkInit) != HAL_OK)
  {
    Error_Handler();
  }
}
