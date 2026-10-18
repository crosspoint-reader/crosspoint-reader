/*
 * SPDX-FileCopyrightText: Copyright (c) 2021, Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#pragma once

//--------------------------------------------------------------------+
// Port/Platform Specific
// TUP stand for TinyUSB Port/Platform (can be renamed)
//--------------------------------------------------------------------+

//------------- Unaligned Memory Access -------------//

#ifdef __ARM_ARCH
  // ARM Architecture set __ARM_FEATURE_UNALIGNED to 1 for mcu supports unaligned access
  #if defined(__ARM_FEATURE_UNALIGNED) && __ARM_FEATURE_UNALIGNED == 1
    #define TUP_ARCH_STRICT_ALIGN 0
  #else
    #define TUP_ARCH_STRICT_ALIGN 1
  #endif
#else
  // TODO default to strict align for others
  // Should investigate other architecture such as risv, xtensa, mips for optimal setting
  #define TUP_ARCH_STRICT_ALIGN 1
#endif

/* USB Controller Attributes for Device, Host or MCU (both)
 * - ENDPOINT_MAX: max (logical) number of endpoint
 * - ENDPOINT_EXCLUSIVE_NUMBER: endpoint number with different direction IN and OUT aren't allowed,
 *                              e.g EP1 OUT & EP1 IN cannot exist together
 * - RHPORT_HIGHSPEED: support highspeed with on-chip PHY
 */

//--------------------------------------------------------------------+
// NXP
//--------------------------------------------------------------------+
#if TU_CHECK_MCU(OPT_MCU_LPC11UXX, OPT_MCU_LPC13XX, OPT_MCU_LPC15XX)
  #define TUP_USBIP_IP3511
  #define TUP_DCD_ENDPOINT_MAX 5

#elif TU_CHECK_MCU(OPT_MCU_LPC175X_6X, OPT_MCU_LPC177X_8X, OPT_MCU_LPC40XX)
  #define TUP_DCD_ENDPOINT_MAX 16
  #define TUP_USBIP_OHCI
  #define TUP_USBIP_OHCI_NXP
  #define TUP_OHCI_RHPORTS 2

#elif TU_CHECK_MCU(OPT_MCU_LPC51UXX)
  #define TUP_USBIP_IP3511
  #define TUP_DCD_ENDPOINT_MAX 5

#elif TU_CHECK_MCU(OPT_MCU_LPC54)
  #include "fsl_device_registers.h"

  // TODO USB0 has 5, USB1 has 6
  #define TUP_USBIP_IP3511

  #if !defined(LPC54114_cm4_SERIES) && !defined(LPC54114_cm0plus_SERIES)
    #define TUP_USBIP_IP3516
    #define TUP_USBIP_OHCI
    #define TUP_USBIP_OHCI_NXP
    #define TUP_OHCI_RHPORTS     1 // 1 downstream port
  #endif

  #define TUP_DCD_ENDPOINT_MAX 6

#elif TU_CHECK_MCU(OPT_MCU_LPC55)
  // TODO USB0 has 5, USB1 has 6
  #define TUP_USBIP_IP3511
  #define TUP_USBIP_IP3516
  #define TUP_USBIP_OHCI
  #define TUP_USBIP_OHCI_NXP
  #define TUP_OHCI_RHPORTS     1 // 1 downstream port

  #define TUP_DCD_ENDPOINT_MAX 6

#elif TU_CHECK_MCU(OPT_MCU_LPC18XX, OPT_MCU_LPC43XX)
  // USB0 has 6 with HS PHY, USB1 has 4 only FS
  #define TUP_USBIP_CHIPIDEA_HS
  #define TUP_USBIP_EHCI

  #define TUP_DCD_ENDPOINT_MAX 6
  #define TUP_RHPORT_HIGHSPEED 1

#elif TU_CHECK_MCU(OPT_MCU_MCXN9)
  // USB0 is chipidea FS
  #define TUP_USBIP_CHIPIDEA_FS
  #define TUP_USBIP_CHIPIDEA_FS_MCX

  // USB1 is chipidea HS
  #define TUP_USBIP_CHIPIDEA_HS
  #define TUP_USBIP_EHCI

  #define TUP_DCD_ENDPOINT_MAX 8
  #define TUP_RHPORT_HIGHSPEED 1

#elif TU_CHECK_MCU(OPT_MCU_MCXA15)
  // USB0 is chipidea FS
  #define TUP_USBIP_CHIPIDEA_FS
  #define TUP_USBIP_CHIPIDEA_FS_MCX

  #define TUP_DCD_ENDPOINT_MAX 16

#elif TU_CHECK_MCU(OPT_MCU_RW61X)
  // USB0 is chipidea HS
  #define TUP_USBIP_CHIPIDEA_HS
  #define TUP_USBIP_EHCI

  #define TUP_DCD_ENDPOINT_MAX 8
  #define TUP_RHPORT_HIGHSPEED 1

#elif TU_CHECK_MCU(OPT_MCU_MIMXRT1XXX)
  #include "fsl_device_registers.h"

  #define TUP_USBIP_CHIPIDEA_HS
  #define TUP_USBIP_EHCI

  #define TUP_DCD_ENDPOINT_MAX 8
  #define TUP_RHPORT_HIGHSPEED 1

  #if __CORTEX_M == 7
    #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     1
    #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT     1
    #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 32
  #endif

  // Errata ERR050101, listed for RT1015/RT1020/RT1024/RT1050 (no fix scheduled) and for
  // RT1060/RT1064 rev A (fixed in rev B); not listed for RT1010 or the RT11xx family.
  #if defined(MIMXRT1015_SERIES) || defined(MIMXRT1021_SERIES) || defined(MIMXRT1024_SERIES) || \
      defined(MIMXRT1051_SERIES) || defined(MIMXRT1052_SERIES) || defined(MIMXRT1061_SERIES) || \
      defined(MIMXRT1062_SERIES) || defined(MIMXRT1064_SERIES)
    #define CFG_TUSB_MIMXRT1XXX_ERRATA_ERR050101 1
  #endif

#elif TU_CHECK_MCU(OPT_MCU_KINETIS_KL, OPT_MCU_KINETIS_K32L, OPT_MCU_KINETIS_K)
  #define TUP_USBIP_CHIPIDEA_FS
  #define TUP_USBIP_CHIPIDEA_FS_KINETIS
  #define TUP_DCD_ENDPOINT_MAX 16

#elif TU_CHECK_MCU(OPT_MCU_MM32F327X)
  #define TUP_DCD_ENDPOINT_MAX 16
  #define TUP_DCD_EDPT_CLOSE_API

//--------------------------------------------------------------------+
// Nordic
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_NRF5X)
  // 8 CBI + 1 ISO
  #define TUP_DCD_ENDPOINT_MAX 9

#elif TU_CHECK_MCU(OPT_MCU_NRF54)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_NRF
  #define TUP_DCD_ENDPOINT_MAX            16
  #define TUP_RHPORT_HIGHSPEED            1
  #define CFG_TUH_DWC2_DMA_ENABLE_DEFAULT 0

//--------------------------------------------------------------------+
// Microchip
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_SAMD11, OPT_MCU_SAML2X, OPT_MCU_SAMD21) || TU_CHECK_MCU(OPT_MCU_SAMD51, OPT_MCU_SAME5X)
  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_SAMG)
  #define TUP_DCD_ENDPOINT_MAX 6
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

#elif TU_CHECK_MCU(OPT_MCU_SAMX7X)
  #define TUP_DCD_ENDPOINT_MAX 10
  #define TUP_RHPORT_HIGHSPEED 1
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

  // Enable dcache if DMA is enabled
  #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUD_SAMX7X_DMA_ENABLE
  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 32

#elif TU_CHECK_MCU(OPT_MCU_PIC32MZ)
  #define TUP_DCD_ENDPOINT_MAX 8
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

#elif TU_CHECK_MCU(OPT_MCU_PIC32MX, OPT_MCU_PIC32MM, OPT_MCU_PIC32MK) || TU_CHECK_MCU(OPT_MCU_PIC24, OPT_MCU_DSPIC33)
  #define TUP_DCD_ENDPOINT_MAX 16
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1
  #define TUP_DCD_EDPT_CLOSE_API

//--------------------------------------------------------------------+
// ST
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_STM32C0)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define TUP_USBIP_FSDEV_DRD
  #define CFG_TUSB_FSDEV_PMA_SIZE 2048u

#elif TU_CHECK_MCU(OPT_MCU_STM32C5)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define TUP_USBIP_FSDEV_DRD
  #define CFG_TUSB_FSDEV_PMA_SIZE 2048u

#elif TU_CHECK_MCU(OPT_MCU_STM32F0)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

#elif TU_CHECK_MCU(OPT_MCU_STM32F1)
  // - F102, F103 use fsdev
  // - F105, F107 use dwc2
  #if defined(STM32F105x8) || defined(STM32F105xB) || defined(STM32F105xC) || defined(STM32F107xB) || \
    defined(STM32F107xC)
    #define TUP_USBIP_DWC2
    #define TUP_USBIP_DWC2_STM32
    #define CFG_TUH_DWC2_DMA_ENABLE_DEFAULT 0

    #define TUP_DCD_ENDPOINT_MAX            4
  #elif defined(STM32F102x6) || defined(STM32F102xB) || defined(STM32F103x6) || defined(STM32F103xB) || \
    defined(STM32F103xE) || defined(STM32F103xG)
    #define TUP_USBIP_FSDEV
    #define TUP_USBIP_FSDEV_STM32
    #define CFG_TUSB_FSDEV_PMA_SIZE 512u
  #else
    #error "Unsupported STM32F1 mcu"
  #endif

#elif TU_CHECK_MCU(OPT_MCU_STM32F2)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32

  // FS has 4 ep, HS has 5 ep
  #define TUP_DCD_ENDPOINT_MAX 6

#elif TU_CHECK_MCU(OPT_MCU_STM32F3)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32

  #if defined(STM32F302xB) || defined(STM32F302xC) || defined(STM32F303xB) || defined(STM32F303xC) || \
    defined(STM32F373xC)
    // xB, and xC: 512
    #define CFG_TUSB_FSDEV_PMA_SIZE 512u
  #elif defined(STM32F302x6) || defined(STM32F302x8) || defined(STM32F302xD) || defined(STM32F302xE) || \
    defined(STM32F303xD) || defined(STM32F303xE)
    // x6, x8, xD, and xE: 1024 + LPM Support
    #define CFG_TUSB_FSDEV_PMA_SIZE 1024u
  #else
    #error "Unsupported STM32F3 mcu"
  #endif

#elif TU_CHECK_MCU(OPT_MCU_STM32F4)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32

  // For most mcu, FS has 4, HS has 6. TODO 446/469/479 HS has 9
  #define TUP_DCD_ENDPOINT_MAX 6

#elif TU_CHECK_MCU(OPT_MCU_STM32F7)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32

  // FS has 6, HS has 9
  #define TUP_DCD_ENDPOINT_MAX 9

  // MCU with on-chip HS Phy
  #if defined(STM32F723xx) || defined(STM32F730xx) || defined(STM32F733xx)
    #define TUP_RHPORT_HIGHSPEED 1 // Port0: FS, Port1: HS
  #endif

  // Enable dcache if DMA is enabled
  #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUD_DWC2_DMA_ENABLE
  #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUH_DWC2_DMA_ENABLE
  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 32

#elif TU_CHECK_MCU(OPT_MCU_STM32G0)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define TUP_USBIP_FSDEV_DRD
  #define CFG_TUSB_FSDEV_PMA_SIZE 2048u

#elif TU_CHECK_MCU(OPT_MCU_STM32G4)
  // Device controller
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

  // TypeC controller
  #define TUP_USBIP_TYPEC_STM32
  #define TUP_TYPEC_RHPORTS_NUM 1

#elif TU_CHECK_MCU(OPT_MCU_STM32H5)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define TUP_USBIP_FSDEV_DRD
  #define CFG_TUSB_FSDEV_PMA_SIZE 2048u

#elif TU_CHECK_MCU(OPT_MCU_STM32H7)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32

  #define TUP_DCD_ENDPOINT_MAX 9

  #ifndef CORE_CM4
    // Enable dcache if DMA is enabled
    #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUD_DWC2_DMA_ENABLE
    #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUH_DWC2_DMA_ENABLE
    #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 32
  #endif

#elif TU_CHECK_MCU(OPT_MCU_STM32H7RS, OPT_MCU_STM32N6)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32

  // FS has 6, HS has 9
  #define TUP_DCD_ENDPOINT_MAX                  9

  // MCU with on-chip HS Phy
  #define TUP_RHPORT_HIGHSPEED                  1

  // Enable dcache if DMA is enabled
  #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUD_DWC2_DMA_ENABLE
  #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUH_DWC2_DMA_ENABLE
  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 32

#elif TU_CHECK_MCU(OPT_MCU_STM32L0)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

#elif TU_CHECK_MCU(OPT_MCU_STM32L1)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 512u

#elif TU_CHECK_MCU(OPT_MCU_STM32L4)
  // - L4x2, L4x3 use fsdev
  // - L4x4, L4x6, L4x7, L4x9 use dwc2
  #if defined(STM32L475xx) || defined(STM32L476xx) || defined(STM32L485xx) || defined(STM32L486xx) || \
    defined(STM32L496xx) || defined(STM32L4A6xx) || defined(STM32L4P5xx) || defined(STM32L4Q5xx) ||   \
    defined(STM32L4R5xx) || defined(STM32L4R7xx) || defined(STM32L4R9xx) || defined(STM32L4S5xx) ||   \
    defined(STM32L4S7xx) || defined(STM32L4S9xx)
    #define TUP_USBIP_DWC2
    #define TUP_USBIP_DWC2_STM32

    #define TUP_DCD_ENDPOINT_MAX 6
  #elif defined(STM32L412xx) || defined(STM32L422xx) || defined(STM32L432xx) || defined(STM32L433xx) || \
    defined(STM32L442xx) || defined(STM32L443xx) || defined(STM32L452xx) || defined(STM32L462xx)
    #define TUP_USBIP_FSDEV
    #define TUP_USBIP_FSDEV_STM32
    #define CFG_TUSB_FSDEV_PMA_SIZE 1024u
  #else
    #error "Unsupported STM32L4 mcu"
  #endif

#elif TU_CHECK_MCU(OPT_MCU_STM32L5)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE (1024u)

#elif TU_CHECK_MCU(OPT_MCU_STM32U0)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

#elif TU_CHECK_MCU(OPT_MCU_STM32U3)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define TUP_USBIP_FSDEV_DRD
  #define CFG_TUSB_FSDEV_PMA_SIZE 2048u

#elif TU_CHECK_MCU(OPT_MCU_STM32U5)
  // U535/545 use fsdev
  #if defined(STM32U535xx) || defined(STM32U545xx)
    #define TUP_USBIP_FSDEV
    #define TUP_USBIP_FSDEV_STM32
    #define TUP_USBIP_FSDEV_DRD
    #define CFG_TUSB_FSDEV_PMA_SIZE 2048u
  #else
    #define TUP_USBIP_DWC2
    #define TUP_USBIP_DWC2_STM32

    // U59x/5Ax/5Fx/5Gx are highspeed with built-in HS PHY
    #if defined(STM32U595xx) || defined(STM32U599xx) || defined(STM32U5A5xx) || defined(STM32U5A9xx) || \
      defined(STM32U5F7xx) || defined(STM32U5F9xx) || defined(STM32U5G7xx) || defined(STM32U5G9xx)
      #define TUP_DCD_ENDPOINT_MAX 9
      #define TUP_RHPORT_HIGHSPEED 1
    #else
      #define TUP_DCD_ENDPOINT_MAX 6
    #endif
  #endif

  // TypeC controller
  #define TUP_USBIP_TYPEC_STM32
  #define TUP_TYPEC_RHPORTS_NUM 1

#elif TU_CHECK_MCU(OPT_MCU_STM32WB)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_STM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

#elif TU_CHECK_MCU(OPT_MCU_STM32WBA)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_STM32
  #define TUP_DCD_ENDPOINT_MAX 9
  #define TUP_RHPORT_HIGHSPEED 1

//--------------------------------------------------------------------+
// Sony
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_CXD56)
  #define TUP_DCD_ENDPOINT_MAX 7
  #define TUP_RHPORT_HIGHSPEED 1
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

//--------------------------------------------------------------------+
// TI
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_MSP430x5xx)
  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_MSP432E4, OPT_MCU_TM4C123, OPT_MCU_TM4C129)
  #define TUP_USBIP_MUSB
  #define TUP_USBIP_MUSB_TI
  #define TUP_DCD_ENDPOINT_MAX 8

//--------------------------------------------------------------------+
// ValentyUSB (Litex)
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_VALENTYUSB_EPTRI)
  #define TUP_DCD_ENDPOINT_MAX 16

//--------------------------------------------------------------------+
// Nuvoton
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_NUC121, OPT_MCU_NUC126)
  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_NUC120)
  #define TUP_DCD_ENDPOINT_MAX 6

#elif TU_CHECK_MCU(OPT_MCU_NUC505)
  #define TUP_DCD_ENDPOINT_MAX 12
  #define TUP_RHPORT_HIGHSPEED 1

//--------------------------------------------------------------------+
// Espressif
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_ESP32S2, OPT_MCU_ESP32S3, OPT_MCU_ESP32H4)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_ESP32
  #define TUP_DCD_ENDPOINT_MAX         7 // only 5 TX FIFO for endpoint IN

  // clang-format off
  #define CFG_TUSB_OS_INC_PATH_DEFAULT freertos/
  // clang-format on

  #if CFG_TUSB_MCU == OPT_MCU_ESP32S3 || CFG_TUSB_MCU == OPT_MCU_ESP32H4
    #define TUP_MCU_MULTIPLE_CORE 1
  #endif

#elif TU_CHECK_MCU(OPT_MCU_ESP32P4)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_ESP32
  #define TUP_RHPORT_HIGHSPEED                  1  // port0 FS, port1 HS
  #define TUP_DCD_ENDPOINT_MAX                  16 // FS 7 ep, HS 16 ep

  // clang-format off
  #define CFG_TUSB_OS_INC_PATH_DEFAULT          freertos/
  // clang-format on

  #define TUP_MCU_MULTIPLE_CORE                 1

  // Enable dcache if DMA is enabled
  #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUD_DWC2_DMA_ENABLE
  #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT     CFG_TUH_DWC2_DMA_ENABLE
  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 64

#elif TU_CHECK_MCU(OPT_MCU_ESP32S31)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_ESP32
  #define TUP_RHPORT_HIGHSPEED                  1
  #define TUP_DCD_ENDPOINT_MAX                  16

  // clang-format off
  #define CFG_TUSB_OS_INC_PATH_DEFAULT          freertos/
  // clang-format on

  #define TUP_MCU_MULTIPLE_CORE                 1

  // Disable slave if DMA is enabled
  #define CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT     !CFG_TUD_DWC2_DMA_ENABLE
  #define CFG_TUH_DWC2_SLAVE_ENABLE_DEFAULT     !CFG_TUH_DWC2_DMA_ENABLE

#elif TU_CHECK_MCU(OPT_MCU_ESP32, OPT_MCU_ESP32C2, OPT_MCU_ESP32C3, OPT_MCU_ESP32C5, OPT_MCU_ESP32C6, \
                   OPT_MCU_ESP32C61, OPT_MCU_ESP32H2)
  #if (CFG_TUD_ENABLED || !(defined(CFG_TUH_MAX3421) && CFG_TUH_MAX3421))
    #error "MCUs are only supported with CFG_TUH_MAX3421 enabled"
  #endif

  #define TUP_DCD_ENDPOINT_MAX         0

  // clang-format off
  #define CFG_TUSB_OS_INC_PATH_DEFAULT freertos/
  // clang-format on


//--------------------------------------------------------------------+
// Dialog
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_DA1469X)
  #define TUP_DCD_ENDPOINT_MAX 4
  #define TUP_DCD_EDPT_CLOSE_API

//--------------------------------------------------------------------+
// Raspberry Pi
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_RP2040)
  #define TUP_DCD_ENDPOINT_MAX  16
  #define TUP_MCU_MULTIPLE_CORE 1

  #define TU_ATTR_FAST_FUNC     __not_in_flash("tinyusb")

//--------------------------------------------------------------------+
// Silabs
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_EFM32GG)
  #define TUP_USBIP_DWC2
  #define TUP_DCD_ENDPOINT_MAX 7

//--------------------------------------------------------------------+
// Renesas
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_RX63X, OPT_MCU_RX65X, OPT_MCU_RX72N, OPT_MCU_RAXXX)
  #define TUP_USBIP_RUSB2
  #define TUP_DCD_ENDPOINT_MAX 10

//--------------------------------------------------------------------+
// GigaDevice
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_GD32VF103)
  #define TUP_USBIP_DWC2
  #define TUP_DCD_ENDPOINT_MAX 4

//--------------------------------------------------------------------+
// Broadcom
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_BCM2711, OPT_MCU_BCM2835, OPT_MCU_BCM2837)
  #define TUP_USBIP_DWC2
  #define TUP_DCD_ENDPOINT_MAX 8
  #define TUP_RHPORT_HIGHSPEED 1

//--------------------------------------------------------------------+
// Infineon
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_XMC4000)
  #define TUP_USBIP_DWC2
  #define TUP_DCD_ENDPOINT_MAX 8

//--------------------------------------------------------------------+
// BridgeTek
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_FT90X)
  #define TUP_DCD_ENDPOINT_MAX 8
  #define TUP_RHPORT_HIGHSPEED 1
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

#elif TU_CHECK_MCU(OPT_MCU_FT93X)
  #define TUP_DCD_ENDPOINT_MAX 16
  #define TUP_RHPORT_HIGHSPEED 1
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

//--------------------------------------------------------------------+
// Allwinner
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_F1C100S)
  #define TUP_DCD_ENDPOINT_MAX 4
  #define TUP_DCD_EDPT_CLOSE_API

//--------------------------------------------------------------------+
// WCH
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_CH32F20X)
  #define TUP_USBIP_WCH_USBHS
  #define TUP_USBIP_WCH_USBFS

  #if !defined(CFG_TUD_WCH_USBIP_USBFS)
    #define CFG_TUD_WCH_USBIP_USBFS 0
  #endif

  #if !defined(CFG_TUD_WCH_USBIP_USBHS)
    #define CFG_TUD_WCH_USBIP_USBHS (CFG_TUD_WCH_USBIP_USBFS ? 0 : 1)
  #endif

  #define TUP_RHPORT_HIGHSPEED CFG_TUD_WCH_USBIP_USBHS
  #define TUP_DCD_ENDPOINT_MAX (CFG_TUD_WCH_USBIP_USBHS ? 16 : 8)

  #if CFG_TUD_WCH_USBIP_USBHS
    #define TUP_DCD_EDPT_CLOSE_API
  #endif

#elif TU_CHECK_MCU(OPT_MCU_CH32V103)
  #define TUP_USBIP_WCH_USBFS

  #if !defined(CFG_TUD_WCH_USBIP_USBFS)
    #define CFG_TUD_WCH_USBIP_USBFS 1
  #endif

  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_CH32X035)
  #define TUP_USBIP_WCH_USBFS

  #if !defined(CFG_TUD_WCH_USBIP_USBFS)
    #define CFG_TUD_WCH_USBIP_USBFS 1
  #endif

  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_CH32V20X)
  // v20x support both port0 FSDEV (USBD) and port1 USBFS
  #define TUP_USBIP_WCH_USBFS

  #ifndef CFG_TUH_WCH_USBIP_USBFS
    #define CFG_TUH_WCH_USBIP_USBFS 1
  #endif

  // default to FSDEV for device
  #if !defined(CFG_TUD_WCH_USBIP_USBFS)
    #define CFG_TUD_WCH_USBIP_USBFS 0
  #endif

  #if !defined(CFG_TUD_WCH_USBIP_FSDEV)
    #define CFG_TUD_WCH_USBIP_FSDEV (CFG_TUD_WCH_USBIP_USBFS ? 0 : 1)
  #endif

  #if CFG_TUD_WCH_USBIP_FSDEV
    #define TUP_USBIP_FSDEV
    #define TUP_USBIP_FSDEV_CH32
    #define CFG_TUSB_FSDEV_PMA_SIZE 512u
  #endif

  #define TUP_DCD_ENDPOINT_MAX 8

#elif TU_CHECK_MCU(OPT_MCU_CH32V307)
  // v307 support both FS and HS, default to HS
  #define TUP_USBIP_WCH_USBHS
  #define TUP_USBIP_WCH_USBFS

  #if !defined(CFG_TUD_WCH_USBIP_USBFS)
    #define CFG_TUD_WCH_USBIP_USBFS 0
  #endif

  #if !defined(CFG_TUD_WCH_USBIP_USBHS)
    #define CFG_TUD_WCH_USBIP_USBHS (CFG_TUD_WCH_USBIP_USBFS ? 0 : 1)
  #endif

  #define TUP_RHPORT_HIGHSPEED CFG_TUD_WCH_USBIP_USBHS
  #define TUP_DCD_ENDPOINT_MAX (CFG_TUD_WCH_USBIP_USBHS ? 16 : 8)

  #if CFG_TUD_WCH_USBIP_USBHS
    #define TUP_DCD_EDPT_CLOSE_API
  #endif

#elif TU_CHECK_MCU(OPT_MCU_CH583)
  // CH582/583 USBFS: older WCH USBFS IP with a single combined per-endpoint control register
  // (like CH32V103), driven by the shared dcd_ch32_usbfs.c on USB0 (rhport 0). Device only:
  // the shared hcd_ch32_usbfs.c is CH32V20x-specific and does not support CH58x, so host /
  // USB2 (rhport 1) is not provided here.
  #define TUP_USBIP_WCH_USBFS

  #ifndef CFG_TUD_WCH_USBIP_USBFS
    #define CFG_TUD_WCH_USBIP_USBFS 1
  #endif

  #define TUP_DCD_ENDPOINT_MAX 8

//--------------------------------------------------------------------+
// Analog Devices
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_MAX32650, OPT_MCU_MAX32666, OPT_MCU_MAX32690, OPT_MCU_MAX78002)
  #define TUP_USBIP_MUSB
  #define TUP_USBIP_MUSB_ADI
  #define TUP_DCD_ENDPOINT_MAX 12
  #define TUP_RHPORT_HIGHSPEED 1
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

//--------------------------------------------------------------------+
// ArteryTek
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_AT32F403A_407, OPT_MCU_AT32F413)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_AT32
  #define CFG_TUSB_FSDEV_PMA_SIZE 768u
  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 2
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 4

#elif TU_CHECK_MCU(OPT_MCU_AT32F415)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_AT32
  #define TUP_DCD_ENDPOINT_MAX 4

#elif TU_CHECK_MCU(OPT_MCU_AT32F402_405, OPT_MCU_AT32F423, OPT_MCU_AT32F425, OPT_MCU_AT32F435_437, OPT_MCU_AT32F45X)
  #define TUP_USBIP_DWC2
  #define TUP_USBIP_DWC2_AT32
  #define TUP_DCD_ENDPOINT_MAX 8

  // AT32F405xx has on-chip HS PHY
  #if defined(AT32F405CBT7) || defined(AT32F405CBU7) || defined(AT32F405CCT7) || defined(AT32F405CCU7) ||     \
    defined(AT32F405KBU7_4) || defined(AT32F405KCU7_4) || defined(AT32F405RBT7_7) || defined(AT32F405RBT7) || \
    defined(AT32F405RCT7_7) || defined(AT32F405RCT7)
    #define TUP_RHPORT_HIGHSPEED 1 // Port0: FS, Port1: HS
  #endif

//--------------------------------------------------------------------+
// HPMicro
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_HPM)
  #define TUP_USBIP_CHIPIDEA_HS
  #define TUP_USBIP_EHCI

  #define TUP_DCD_ENDPOINT_MAX    16
  #define TUP_RHPORT_HIGHSPEED    1

  #define TU_ATTR_FAST_FUNC __attribute__((section(".fast")))

//--------------------------------------------------------------------+
// Puya
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_PY32F0)
  #define TUP_USBIP_MUSB
  #define TUP_USBIP_MUSB_PY32
  #define TUP_DCD_ENDPOINT_MAX 6
  // PY32 shares the buffer between IN and OUT of the same endpoint number.
  // Possible to share IN/OUT if only one direction is armed at any one time
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 1

//--------------------------------------------------------------------+
// Geehy
//--------------------------------------------------------------------+
#elif TU_CHECK_MCU(OPT_MCU_APM32F0XX)
  #define TUP_USBIP_FSDEV
  #define TUP_USBIP_FSDEV_APM32
  #define CFG_TUSB_FSDEV_PMA_SIZE 1024u

#endif

// External USB controller
#if defined(CFG_TUH_MAX3421) && CFG_TUH_MAX3421
  #ifndef CFG_TUH_MAX3421_ENDPOINT_TOTAL
    #define CFG_TUH_MAX3421_ENDPOINT_TOTAL (8 + 4 * (CFG_TUH_DEVICE_MAX - 1))
  #endif
#endif


//--------------------------------------------------------------------+
// Default Values
//--------------------------------------------------------------------+

#if defined(TUP_USBIP_FSDEV)
  #define TUP_DCD_ENDPOINT_MAX 8
#endif

#ifndef TUP_MCU_MULTIPLE_CORE
  #define TUP_MCU_MULTIPLE_CORE 0
#endif

#if !defined(TUP_DCD_ENDPOINT_MAX) && defined(CFG_TUD_ENABLED) && CFG_TUD_ENABLED
  #warning "TUP_DCD_ENDPOINT_MAX is not defined for this MCU, default to 8"
  #define TUP_DCD_ENDPOINT_MAX 8
#endif

// Default to fullspeed if not defined
#ifndef TUP_RHPORT_HIGHSPEED
  #define TUP_RHPORT_HIGHSPEED 0
#endif

// fast function, normally mean placing function in SRAM
#ifndef TU_ATTR_FAST_FUNC
  #define TU_ATTR_FAST_FUNC
#endif

// TUP_DCD_EDPT_CLOSE_API is deprecated: these USBIPs implement dcd_edpt_close() and lack the
// ISO alloc & activate API. IP3511, RUSB2 and NRF5X have been migrated to ISO_ALLOC; the remaining
// CLOSE_API MCUs (mm32, pic, da1469x, f1c100s, ch32-usbhs) are pending per-board verification.
#ifndef TUP_DCD_EDPT_CLOSE_API
  #define TUP_DCD_EDPT_ISO_ALLOC
#endif

// Set by silicon whose isochronous IN endpoint can be unprimed by an IN token sent to that same
// endpoint number on ANOTHER device sharing the host, taking one of this device's OUT endpoints
// down with it - undetectable in software. Descriptors must then give an isochronous IN endpoint
// a number no other device on the bus uses; a number is only safe while it stays unique, so two
// affected boards on one hub must not pick the same one. Default 0 (no such conflict). Set it to
// 0 by hand on RT1060/RT1064 rev B, which carry the fix - the revision cannot be told apart at
// compile time, so the affected parts are assumed to be rev A.
#ifndef CFG_TUSB_MIMXRT1XXX_ERRATA_ERR050101
  #define CFG_TUSB_MIMXRT1XXX_ERRATA_ERR050101 0
#endif

// Some USBIPs (SAMG, SAMX7X, PIC32, MAX3266x/MAX78002) cannot assign the same endpoint
// number to both IN and OUT. Default to 0 (same endpoint number may be used for IN and OUT).
#ifndef CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY
  #define CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY 0
#endif

// Backward-compatible alias: legacy code only tests defined(TUD_ENDPOINT_ONE_DIRECTION_ONLY)
#if CFG_TUD_ENDPOINT_ONE_DIRECTION_ONLY && !defined(TUD_ENDPOINT_ONE_DIRECTION_ONLY)
  #define TUD_ENDPOINT_ONE_DIRECTION_ONLY
#endif
