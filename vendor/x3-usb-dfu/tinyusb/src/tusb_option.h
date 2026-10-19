/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#pragma once

#include "common/tusb_compiler.h"

// Version is release as major.minor.revision eg 1.0.0
#define TUSB_VERSION_MAJOR     0
#define TUSB_VERSION_MINOR     21
#define TUSB_VERSION_REVISION  0

#define TUSB_VERSION_NUMBER    (TUSB_VERSION_MAJOR * 10000 + TUSB_VERSION_MINOR * 100 + TUSB_VERSION_REVISION)
#define TUSB_VERSION_STRING    TU_XSTRING(TUSB_VERSION_MAJOR) "." TU_XSTRING(TUSB_VERSION_MINOR) "." TU_XSTRING(TUSB_VERSION_REVISION)

//--------------------------------------------------------------------+
// Supported MCUs
// CFG_TUSB_MCU must be defined to one of following value
//--------------------------------------------------------------------+

#define OPT_MCU_NONE                0

// LPC
#define OPT_MCU_LPC11UXX            1 ///< NXP LPC11Uxx
#define OPT_MCU_LPC13XX             2 ///< NXP LPC13xx
#define OPT_MCU_LPC15XX             3 ///< NXP LPC15xx
#define OPT_MCU_LPC175X_6X          4 ///< NXP LPC175x, LPC176x
#define OPT_MCU_LPC177X_8X          5 ///< NXP LPC177x, LPC178x
#define OPT_MCU_LPC18XX             6 ///< NXP LPC18xx
#define OPT_MCU_LPC40XX             7 ///< NXP LPC40xx
#define OPT_MCU_LPC43XX             8 ///< NXP LPC43xx
#define OPT_MCU_LPC51               9 ///< NXP LPC51
#define OPT_MCU_LPC51UXX            OPT_MCU_LPC51 ///< NXP LPC51
#define OPT_MCU_LPC54              10 ///< NXP LPC54
#define OPT_MCU_LPC55              11 ///< NXP LPC55
// legacy naming
#define OPT_MCU_LPC54XXX           OPT_MCU_LPC54
#define OPT_MCU_LPC55XX            OPT_MCU_LPC55

// NRF
#define OPT_MCU_NRF5X             100 ///< Nordic nRF 52,53 series
#define OPT_MCU_NRF54             101 ///< Nordic nRF54 series

// SAM
#define OPT_MCU_SAMD21            200 ///< MicroChip SAMD21
#define OPT_MCU_SAMD51            201 ///< MicroChip SAMD51
#define OPT_MCU_SAMG              202 ///< MicroChip SAMDG series
#define OPT_MCU_SAME5X            203 ///< MicroChip SAM E5x
#define OPT_MCU_SAMD11            204 ///< MicroChip SAMD11
#define OPT_MCU_SAML2X            205 ///< MicroChip SAML2x
#define OPT_MCU_SAML21            OPT_MCU_SAML2X ///< SAML21 backward compatibility
#define OPT_MCU_SAML22            OPT_MCU_SAML2X ///< SAML22 backward compatibility
#define OPT_MCU_SAMX7X            207 ///< MicroChip SAME70, S70, V70, V71 family

// STM32
#define OPT_MCU_STM32F0           300 ///< ST F0
#define OPT_MCU_STM32F1           301 ///< ST F1
#define OPT_MCU_STM32F2           302 ///< ST F2
#define OPT_MCU_STM32F3           303 ///< ST F3
#define OPT_MCU_STM32F4           304 ///< ST F4
#define OPT_MCU_STM32F7           305 ///< ST F7
#define OPT_MCU_STM32H7           306 ///< ST H7
#define OPT_MCU_STM32L1           308 ///< ST L1
#define OPT_MCU_STM32L0           307 ///< ST L0
#define OPT_MCU_STM32L4           309 ///< ST L4
#define OPT_MCU_STM32G0           310 ///< ST G0
#define OPT_MCU_STM32G4           311 ///< ST G4
#define OPT_MCU_STM32WB           312 ///< ST WB
#define OPT_MCU_STM32U5           313 ///< ST U5
#define OPT_MCU_STM32L5           314 ///< ST L5
#define OPT_MCU_STM32H5           315 ///< ST H5
#define OPT_MCU_STM32U0           316 ///< ST U0
#define OPT_MCU_STM32H7RS         317 ///< ST F7RS
#define OPT_MCU_STM32C0           318 ///< ST C0
#define OPT_MCU_STM32N6           319 ///< ST N6
#define OPT_MCU_STM32WBA          320 ///< ST WBA
#define OPT_MCU_STM32U3           321 ///< ST U3
#define OPT_MCU_STM32C5           322 ///< ST C5

// Sony
#define OPT_MCU_CXD56             400 ///< SONY CXD56

// TI
#define OPT_MCU_MSP430x5xx        500 ///< TI MSP430x5xx
#define OPT_MCU_MSP432E4          510 ///< TI MSP432E4xx
#define OPT_MCU_TM4C123           511 ///< TI Tiva-C 123x
#define OPT_MCU_TM4C129           512 ///< TI Tiva-C 129x

// ValentyUSB eptri
#define OPT_MCU_VALENTYUSB_EPTRI  600 ///< Fomu eptri config

// NXP iMX RT
#define OPT_MCU_MIMXRT1XXX        700                 ///< NXP iMX RT1xxx Series
#define OPT_MCU_MIMXRT10XX        OPT_MCU_MIMXRT1XXX  ///< RT10xx
#define OPT_MCU_MIMXRT11XX        OPT_MCU_MIMXRT1XXX  ///< RT11xx

// Nuvoton
#define OPT_MCU_NUC121            800
#define OPT_MCU_NUC126            801
#define OPT_MCU_NUC120            802
#define OPT_MCU_NUC505            803

// Espressif
#define OPT_MCU_ESP32S2           900 ///< Espressif ESP32-S2
#define OPT_MCU_ESP32S3           901 ///< Espressif ESP32-S3
#define OPT_MCU_ESP32             902 ///< Espressif ESP32 (for host max3421e)
#define OPT_MCU_ESP32C3           903 ///< Espressif ESP32-C3
#define OPT_MCU_ESP32C6           904 ///< Espressif ESP32-C6
#define OPT_MCU_ESP32C2           905 ///< Espressif ESP32-C2
#define OPT_MCU_ESP32H2           906 ///< Espressif ESP32-H2
#define OPT_MCU_ESP32P4           907 ///< Espressif ESP32-P4
#define OPT_MCU_ESP32C5           908 ///< Espressif ESP32-C5
#define OPT_MCU_ESP32C61          909 ///< Espressif ESP32-C61
#define OPT_MCU_ESP32H4           910 ///< Espressif ESP32-H4
#define OPT_MCU_ESP32S31          911 ///< Espressif ESP32-S31

// Dialog
#define OPT_MCU_DA1469X          1000 ///< Dialog Semiconductor DA1469x

// Raspberry Pi
#define OPT_MCU_RP2040           1100 ///< Raspberry Pi RP2040

// NXP Kinetis
#define OPT_MCU_KINETIS_KL       1200 ///< NXP KL series
#define OPT_MCU_KINETIS_K32L     1201 ///< NXP K32L series
#define OPT_MCU_KINETIS_K32      1201 ///< Alias to K32L
#define OPT_MCU_KINETIS_K        1202 ///< NXP K series

#define OPT_MCU_MKL25ZXX         1200 ///< Alias to KL (obsolete)
#define OPT_MCU_K32L2BXX         1201 ///< Alias to K32 (obsolete)

// Silabs
#define OPT_MCU_EFM32GG          1300 ///< Silabs EFM32GG

// Renesas RX
#define OPT_MCU_RX63X            1400 ///< Renesas RX63N/631
#define OPT_MCU_RX65X            1401 ///< Renesas RX65N/RX651
#define OPT_MCU_RX72N            1402 ///< Renesas RX72N
#define OPT_MCU_RAXXX            1403 ///< Renesas RA generic

// Mind Motion
#define OPT_MCU_MM32F327X        1500 ///< Mind Motion MM32F327

// GigaDevice
#define OPT_MCU_GD32VF103        1600 ///< GigaDevice GD32VF103

// Broadcom
#define OPT_MCU_BCM2711          1700 ///< Broadcom BCM2711
#define OPT_MCU_BCM2835          1701 ///< Broadcom BCM2835
#define OPT_MCU_BCM2837          1702 ///< Broadcom BCM2837

// Infineon
#define OPT_MCU_XMC4000          1800 ///< Infineon XMC4000

// PIC
#define OPT_MCU_PIC32MZ          1900 ///< MicroChip PIC32MZ family
#define OPT_MCU_PIC32MM          1901 ///< MicroChip PIC32MM family
#define OPT_MCU_PIC32MX          1902 ///< MicroChip PIC32MX family
#define OPT_MCU_PIC32MK          1903 ///< MicroChip PIC32MK family
#define OPT_MCU_PIC24            1910 ///< MicroChip PIC24 family
#define OPT_MCU_DSPIC33          1911 ///< MicroChip DSPIC33 family

// BridgeTek
#define OPT_MCU_FT90X            2000 ///< BridgeTek FT90x
#define OPT_MCU_FT93X            2001 ///< BridgeTek FT93x

// Allwinner
#define OPT_MCU_F1C100S          2100 ///< Allwinner F1C100s family

// WCH
#define OPT_MCU_CH32V307         2200 ///< WCH CH32V307
#define OPT_MCU_CH32F20X         2210 ///< WCH CH32F20x
#define OPT_MCU_CH32V20X         2220 ///< WCH CH32V20X
#define OPT_MCU_CH32V103         2230 ///< WCH CH32V103
#define OPT_MCU_CH583            2240 ///< WCH CH583
#define OPT_MCU_CH582            OPT_MCU_CH583 ///< WCH CH582 (alias, same USB IP as CH583)
#define OPT_MCU_CH32X035         2250 ///< WCH CH32X035

// NXP LPC MCX
#define OPT_MCU_MCXN9            2300  ///< NXP MCX N9 Series
#define OPT_MCU_MCXA15           2301  ///< NXP MCX A15 Series
#define OPT_MCU_RW61X            2302  ///< NXP RW61x Series

// Analog Devices
#define OPT_MCU_MAX32690         2400  ///< ADI MAX32690
#define OPT_MCU_MAX32665         2401  ///< ADI MAX32666/5
#define OPT_MCU_MAX32666         2401  ///< ADI MAX32666/5
#define OPT_MCU_MAX32650         2402  ///< ADI MAX32650/1/2
#define OPT_MCU_MAX78002         2403  ///< ADI MAX78002

// ArteryTek
#define OPT_MCU_AT32F403A_407    2500  ///< ArteryTek AT32F403A_AT32F407
#define OPT_MCU_AT32F415         2501  ///< ArteryTek AT32F415
#define OPT_MCU_AT32F435_437     2502  ///< ArteryTek AT32F435_AT32F437
#define OPT_MCU_AT32F423         2503  ///< ArteryTek AT32F423
#define OPT_MCU_AT32F402_405     2504  ///< ArteryTek AT32F402_405
#define OPT_MCU_AT32F425         2505  ///< ArteryTek AT32F425
#define OPT_MCU_AT32F413         2506  ///< ArteryTek AT32F413
#define OPT_MCU_AT32F45X         2507  ///< ArteryTek AT32F45x

// HPMicro
#define OPT_MCU_HPM              2600  ///< HPMicro

// Puya
#define OPT_MCU_PY32F0           2700  ///< Puya PY32F0

// Geehy
#define OPT_MCU_APM32F0XX        2800  ///< Geehy APM32F0xx

// Check if configured MCU is one of listed
// Apply TU_MCU_IS_EQUAL with || as separator to list of input
#define TU_MCU_IS_EQUAL(_m)  (CFG_TUSB_MCU == (_m))
#define TU_CHECK_MCU(...)    (TU_ARGS_APPLY(TU_MCU_IS_EQUAL, ||, __VA_ARGS__))

//--------------------------------------------------------------------+
// Supported OS
//--------------------------------------------------------------------+

#define OPT_OS_NONE       1  ///< No RTOS
#define OPT_OS_FREERTOS   2  ///< FreeRTOS
#define OPT_OS_MYNEWT     3  ///< Mynewt OS
#define OPT_OS_CUSTOM     4  ///< Custom OS is implemented by application
#define OPT_OS_PICO       5  ///< Raspberry Pi Pico SDK
#define OPT_OS_RTTHREAD   6  ///< RT-Thread
#define OPT_OS_RTX4       7  ///< Keil RTX 4
#define OPT_OS_ZEPHYR     8  ///< Zephyr
#define OPT_OS_THREADX    9  ///< ThreadX

//--------------------------------------------------------------------+
// Mode and Speed
//--------------------------------------------------------------------+

// Low byte is operational mode
#define OPT_MODE_NONE           0x0000 ///< Disabled
#define OPT_MODE_DEVICE         0x0001 ///< Device Mode
#define OPT_MODE_HOST           0x0002 ///< Host Mode

// High byte is max operational speed (corresponding to tusb_speed_t)
#define OPT_MODE_DEFAULT_SPEED  0x0000u ///< Default (max) speed supported by MCU
#define OPT_MODE_LOW_SPEED      0x0100u ///< Low Speed
#define OPT_MODE_FULL_SPEED     0x0200u ///< Full Speed
#define OPT_MODE_HIGH_SPEED     0x0400u ///< High Speed
#define OPT_MODE_SPEED_MASK     0xff00u

//--------------------------------------------------------------------+
// Validation Level
// Optional validation of data received from the USB peer, traded against code size. Coverage is parser-specific
// and expanded incrementally. CFG_TUSB_VALIDATION_LEVEL sets the default for both device and host; use the
// CFG_TUD_VALIDATION_LEVEL / CFG_TUH_VALIDATION_LEVEL overrides when the roles need different policies.
//--------------------------------------------------------------------+
#define TUSB_VALIDATION_NONE   0 ///< trusted peers, minimal code size
#define TUSB_VALIDATION_BASIC  1 ///< structural memory-safety checks where supported
#define TUSB_VALIDATION_STRICT 2 ///< additional USB and class-specific conformance checks

//--------------------------------------------------------------------+
// Include tusb_config.h
//--------------------------------------------------------------------+

// Allow to use command line to change the config name/location
#ifdef CFG_TUSB_CONFIG_FILE
  #include CFG_TUSB_CONFIG_FILE
#else
  #include "tusb_config.h"
#endif

#include "common/tusb_mcu.h"

//--------------------------------------------------------------------+
// Validation Options
//--------------------------------------------------------------------+

#ifndef CFG_TUSB_VALIDATION_LEVEL
  #define CFG_TUSB_VALIDATION_LEVEL TUSB_VALIDATION_BASIC
#endif

#ifndef CFG_TUD_VALIDATION_LEVEL
  #define CFG_TUD_VALIDATION_LEVEL CFG_TUSB_VALIDATION_LEVEL
#endif

#ifndef CFG_TUH_VALIDATION_LEVEL
  #define CFG_TUH_VALIDATION_LEVEL CFG_TUSB_VALIDATION_LEVEL
#endif

#if (CFG_TUSB_VALIDATION_LEVEL < TUSB_VALIDATION_NONE) || \
    (CFG_TUSB_VALIDATION_LEVEL > TUSB_VALIDATION_STRICT)
  #error "CFG_TUSB_VALIDATION_LEVEL must be TUSB_VALIDATION_NONE, TUSB_VALIDATION_BASIC, or TUSB_VALIDATION_STRICT"
#endif

#if (CFG_TUD_VALIDATION_LEVEL < TUSB_VALIDATION_NONE) || \
    (CFG_TUD_VALIDATION_LEVEL > TUSB_VALIDATION_STRICT)
  #error "CFG_TUD_VALIDATION_LEVEL must be TUSB_VALIDATION_NONE, TUSB_VALIDATION_BASIC, or TUSB_VALIDATION_STRICT"
#endif

#if (CFG_TUH_VALIDATION_LEVEL < TUSB_VALIDATION_NONE) || \
    (CFG_TUH_VALIDATION_LEVEL > TUSB_VALIDATION_STRICT)
  #error "CFG_TUH_VALIDATION_LEVEL must be TUSB_VALIDATION_NONE, TUSB_VALIDATION_BASIC, or TUSB_VALIDATION_STRICT"
#endif

// Validation conditions are short-circuited at lower levels and compile out when the result is unused.
#define TUD_VALIDATION_CHECK(_level, _cond) ((CFG_TUD_VALIDATION_LEVEL < (_level)) || (_cond))
#define TUH_VALIDATION_CHECK(_level, _cond) ((CFG_TUH_VALIDATION_LEVEL < (_level)) || (_cond))

#define TUD_VALIDATE_BASIC(_cond) TUD_VALIDATION_CHECK(TUSB_VALIDATION_BASIC, _cond)
#define TUH_VALIDATE_BASIC(_cond) TUH_VALIDATION_CHECK(TUSB_VALIDATION_BASIC, _cond)
#define TUD_VALIDATE_STRICT(_cond) TUD_VALIDATION_CHECK(TUSB_VALIDATION_STRICT, _cond)
#define TUH_VALIDATE_STRICT(_cond) TUH_VALIDATION_CHECK(TUSB_VALIDATION_STRICT, _cond)

//--------------------------------------------------------------------+
// USBIP
//--------------------------------------------------------------------+

//------------- ChipIdea -------------//
// Enable CI_HS VBUS Charge. Set this to 1 if the USB_VBUS pin is not connected to 5V VBUS (note: 3.3V is
// insufficient).
#ifndef CFG_TUD_CI_HS_VBUS_CHARGE
  #ifndef CFG_TUD_CI_HS_VBUS_CHARGE_DEFAULT
    #define CFG_TUD_CI_HS_VBUS_CHARGE_DEFAULT 0
  #endif
  #define CFG_TUD_CI_HS_VBUS_CHARGE CFG_TUD_CI_HS_VBUS_CHARGE_DEFAULT
#endif

// CI_HS support FIFO transfer if endpoint buffer is 4k aligned and size is multiple of 4k, also DCACHE is disabled
#ifndef CFG_TUD_CI_HS_EPBUF_4K_ALIGNED
  #define CFG_TUD_CI_HS_EPBUF_4K_ALIGNED 0
#endif

#if CFG_TUD_CI_HS_EPBUF_4K_ALIGNED && !CFG_TUD_MEM_DCACHE_ENABLE
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO 1
#endif

//------------- DWC2 -------------//
// DMA mode for device
#ifndef CFG_TUD_DWC2_DMA_ENABLE
  #ifndef CFG_TUD_DWC2_DMA_ENABLE_DEFAULT
  #define CFG_TUD_DWC2_DMA_ENABLE_DEFAULT 0
  #endif

  #define CFG_TUD_DWC2_DMA_ENABLE CFG_TUD_DWC2_DMA_ENABLE_DEFAULT
#endif

// Slave mode for device
#ifndef CFG_TUD_DWC2_SLAVE_ENABLE
  #ifndef CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT
    #define CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT !CFG_TUD_DWC2_DMA_ENABLE // disabled if DMA is enabled
  #endif

  #define CFG_TUD_DWC2_SLAVE_ENABLE CFG_TUD_DWC2_SLAVE_ENABLE_DEFAULT
#endif

// Periodic Transfer Interrupt (PTI) for device Buffer DMA (DCTL.IgnrFrmNum).
// When enabled, ISOC even/odd frame matching is ignored so a transfer can be
// armed in the current or next (micro)frame. Invalid in Slave mode; requires DMA.
#ifndef CFG_TUD_DWC2_PTI_ENABLE
  #ifndef CFG_TUD_DWC2_PTI_ENABLE_DEFAULT
    #define CFG_TUD_DWC2_PTI_ENABLE_DEFAULT 0
  #endif

  #define CFG_TUD_DWC2_PTI_ENABLE CFG_TUD_DWC2_PTI_ENABLE_DEFAULT
#endif

// DMA mode for host
#ifndef CFG_TUH_DWC2_DMA_ENABLE
  #ifndef CFG_TUH_DWC2_DMA_ENABLE_DEFAULT
    #define CFG_TUH_DWC2_DMA_ENABLE_DEFAULT 0
  #endif

  #define CFG_TUH_DWC2_DMA_ENABLE CFG_TUH_DWC2_DMA_ENABLE_DEFAULT
#endif

// Slave mode for host
#ifndef CFG_TUH_DWC2_SLAVE_ENABLE
  #ifndef CFG_TUH_DWC2_SLAVE_ENABLE_DEFAULT
    #define CFG_TUH_DWC2_SLAVE_ENABLE_DEFAULT !CFG_TUH_DWC2_DMA_ENABLE // disabled if DMA is enabled
  #endif

  #define CFG_TUH_DWC2_SLAVE_ENABLE CFG_TUH_DWC2_SLAVE_ENABLE_DEFAULT
#endif

#if defined(TUP_USBIP_DWC2)
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO    CFG_TUD_DWC2_SLAVE_ENABLE
  #define CFG_TUH_EDPT_DEDICATED_HWFIFO    CFG_TUH_DWC2_SLAVE_ENABLE

  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 4 // 32bit access
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 0 // fixed hwfifo address
#endif

//------------ FSDEV --------------//
#if defined(TUP_USBIP_FSDEV)
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO 1

  #if CFG_TUSB_FSDEV_PMA_SIZE == 2048 || TU_CHECK_MCU(OPT_MCU_STM32U0)
    // 32-bit access scheme
    #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 4 // 32-bit data
    #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 4 // 32-bit address increase
    #define CFG_TUSB_FSDEV_32BIT
  #elif CFG_TUSB_FSDEV_PMA_SIZE == 1024
    // 2 x 16-bit access scheme
    #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 2 // 16-bit data
    #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 2 // 16-bit address increase
  #elif CFG_TUSB_FSDEV_PMA_SIZE == 512
    // 1 x 16-bit access scheme
    #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 2 // 16-bit data
    #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 4 // 32-bit address increase
  #endif
#endif

//------------ MAX3421 -------------//
// Enable MAX3421 USB host controller
#ifndef CFG_TUH_MAX3421
  #define CFG_TUH_MAX3421 0
#endif

//------------ MUSB --------------//
#if defined(TUP_USBIP_MUSB)
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO              1
  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE           4 // 32 bit data
  #if !defined(TUP_USBIP_MUSB_PY32)
    #define CFG_TUSB_FIFO_HWFIFO_DATA_ODD_16BIT_ACCESS // allow odd 16bit access
  #endif
  #define CFG_TUSB_FIFO_HWFIFO_DATA_ODD_8BIT_ACCESS    // allow odd 8bit access
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE           0 // fixed hwfifo
#endif

//------------- Raspberry Pi -------------//
// Enable PIO-USB software host controller
#ifndef CFG_TUH_RPI_PIO_USB
  #define CFG_TUH_RPI_PIO_USB 0
#endif

#ifndef CFG_TUD_RPI_PIO_USB
  #define CFG_TUD_RPI_PIO_USB 0
#endif

#if (CFG_TUSB_MCU == OPT_MCU_RP2040) && !CFG_TUD_RPI_PIO_USB
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO    0
  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 1
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 1
  #define CFG_TUSB_FIFO_HWFIFO_CUSTOM_WRITE
  #define CFG_TUSB_FIFO_HWFIFO_CUSTOM_READ
#endif

//------------ RUSB2 --------------//
#if defined(TUP_USBIP_RUSB2)
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO     1
  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE  (2 | (TUD_OPT_HIGH_SPEED ? 4 : 0)) // 16 bit and 32 bit if highspeed
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE  0
  #define CFG_TUSB_FIFO_HWFIFO_CUSTOM_WRITE // custom write since rusb2 can change access width 32 -> 16 and can write
                                            // odd byte with byte access
#endif

//------- Microchip SAMX7X -------//
// DMA mode for device
#ifndef CFG_TUD_SAMX7X_DMA_ENABLE
  #ifndef CFG_TUD_SAMX7X_DMA_ENABLE_DEFAULT
  #define CFG_TUD_SAMX7X_DMA_ENABLE_DEFAULT 0
  #endif

  #define CFG_TUD_SAMX7X_DMA_ENABLE CFG_TUD_SAMX7X_DMA_ENABLE_DEFAULT
#endif

#if (CFG_TUSB_MCU == OPT_MCU_SAMX7X)
  #define CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE 4
  #define CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE 4
  #define CFG_TUSB_FIFO_HWFIFO_DATA_ODD_16BIT_ACCESS
  #define CFG_TUSB_FIFO_HWFIFO_DATA_ODD_8BIT_ACCESS
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO 1
#endif

//--------------------------------------------------------------------
// RootHub Mode detection
//--------------------------------------------------------------------

//------------- Root hub as Device -------------//

#if defined(CFG_TUSB_RHPORT0_MODE) && ((CFG_TUSB_RHPORT0_MODE) & OPT_MODE_DEVICE)
  #define TUD_RHPORT_MODE     (CFG_TUSB_RHPORT0_MODE)
  #define TUD_OPT_RHPORT      0
#elif defined(CFG_TUSB_RHPORT1_MODE) && ((CFG_TUSB_RHPORT1_MODE) & OPT_MODE_DEVICE)
  #define TUD_RHPORT_MODE     (CFG_TUSB_RHPORT1_MODE)
  #define TUD_OPT_RHPORT      1
#else
  #define TUD_RHPORT_MODE     OPT_MODE_NONE
#endif

#ifndef CFG_TUD_ENABLED
  // fallback to use CFG_TUSB_RHPORTx_MODE
  #define CFG_TUD_ENABLED     (TUD_RHPORT_MODE & OPT_MODE_DEVICE)
#endif

#ifndef CFG_TUD_MAX_SPEED
  // fallback to use CFG_TUSB_RHPORTx_MODE
  #define CFG_TUD_MAX_SPEED   (TUD_RHPORT_MODE & OPT_MODE_SPEED_MASK)
#endif

// For backward compatible
#define TUSB_OPT_DEVICE_ENABLED CFG_TUD_ENABLED

// highspeed support indicator
#define TUD_OPT_HIGH_SPEED    (CFG_TUD_MAX_SPEED ? (CFG_TUD_MAX_SPEED & OPT_MODE_HIGH_SPEED) : TUP_RHPORT_HIGHSPEED)

//------------- Root hub as Host -------------//

#if defined(CFG_TUSB_RHPORT0_MODE) && ((CFG_TUSB_RHPORT0_MODE) & OPT_MODE_HOST)
  #define TUH_RHPORT_MODE  (CFG_TUSB_RHPORT0_MODE)
  #define TUH_OPT_RHPORT   0
#elif defined(CFG_TUSB_RHPORT1_MODE) && ((CFG_TUSB_RHPORT1_MODE) & OPT_MODE_HOST)
  #define TUH_RHPORT_MODE  (CFG_TUSB_RHPORT1_MODE)
  #define TUH_OPT_RHPORT   1
#else
  #define TUH_RHPORT_MODE   OPT_MODE_NONE
#endif

#ifndef CFG_TUH_ENABLED
  // fallback to use CFG_TUSB_RHPORTx_MODE
  #define CFG_TUH_ENABLED     (TUH_RHPORT_MODE & OPT_MODE_HOST)
#endif

#ifndef CFG_TUH_MAX_SPEED
  // fallback to use CFG_TUSB_RHPORTx_MODE
  #define CFG_TUH_MAX_SPEED   (TUH_RHPORT_MODE & OPT_MODE_SPEED_MASK)
#endif

// For backward compatible
#define TUSB_OPT_HOST_ENABLED   CFG_TUH_ENABLED

// highspeed support indicator
#define TUH_OPT_HIGH_SPEED    (CFG_TUH_MAX_SPEED ? (CFG_TUH_MAX_SPEED & OPT_MODE_HIGH_SPEED) : TUP_RHPORT_HIGHSPEED)


//--------------------------------------------------------------------+
// TODO move later
//--------------------------------------------------------------------+

// TUP_MCU_STRICT_ALIGN will overwrite TUP_ARCH_STRICT_ALIGN.
// In case TUP_MCU_STRICT_ALIGN = 1 and TUP_ARCH_STRICT_ALIGN =0, we will not reply on compiler
// to generate unaligned access code.
// LPC_IP3511 Highspeed cannot access unaligned memory on USB_RAM
#if TUD_OPT_HIGH_SPEED && TU_CHECK_MCU(OPT_MCU_LPC54XXX, OPT_MCU_LPC55XX)
  #define TUP_MCU_STRICT_ALIGN   1
#else
  #define TUP_MCU_STRICT_ALIGN   0
#endif

//--------------------------------------------------------------------+
// Common Options (Default)
//--------------------------------------------------------------------+

// Debug enable to print out error message
#ifndef CFG_TUSB_DEBUG
  #define CFG_TUSB_DEBUG 0
#endif

// Level where CFG_TUSB_DEBUG must be at least for USBH is logged
#ifndef CFG_TUH_LOG_LEVEL
  #define CFG_TUH_LOG_LEVEL   2
#endif

// Level where CFG_TUSB_DEBUG must be at least for USBD is logged
#ifndef CFG_TUD_LOG_LEVEL
  #define CFG_TUD_LOG_LEVEL   2
#endif

// Memory section for placing buffer used for usb transferring. If MEM_SECTION is different for
// host and device use: CFG_TUD_MEM_SECTION, CFG_TUH_MEM_SECTION instead
#ifndef CFG_TUSB_MEM_SECTION
  #define CFG_TUSB_MEM_SECTION
#endif

// Alignment requirement of buffer used for usb transferring. if MEM_ALIGN is different for
// host and device controller use: CFG_TUD_MEM_ALIGN, CFG_TUH_MEM_ALIGN instead
#ifndef CFG_TUSB_MEM_ALIGN
  #define CFG_TUSB_MEM_ALIGN      TU_ATTR_ALIGNED(4)
#endif

#ifndef CFG_TUSB_MEM_DCACHE_LINE_SIZE
  #ifndef CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT
  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT 1
  #endif

  #define CFG_TUSB_MEM_DCACHE_LINE_SIZE CFG_TUSB_MEM_DCACHE_LINE_SIZE_DEFAULT
#endif

// OS selection
#ifndef CFG_TUSB_OS
  #define CFG_TUSB_OS           OPT_OS_NONE
#endif

// 1 when CFG_TUSB_OS provides a preemptive scheduler with distinct tasks
// (FreeRTOS, Zephyr, ThreadX, etc.); 0 when the application is single-context
// (bare-metal OS_NONE or Pico SDK). Sync host control xfers from the host
// task are forbidden when this is 1.
#ifndef CFG_TUSB_OS_HAS_SCHEDULER
  #if CFG_TUSB_OS == OPT_OS_NONE || CFG_TUSB_OS == OPT_OS_PICO
    #define CFG_TUSB_OS_HAS_SCHEDULER 0
  #else
    #define CFG_TUSB_OS_HAS_SCHEDULER 1
  #endif
#endif

#ifndef CFG_TUSB_OS_INC_PATH
  #ifndef CFG_TUSB_OS_INC_PATH_DEFAULT
  #define CFG_TUSB_OS_INC_PATH_DEFAULT
  #endif

  #define CFG_TUSB_OS_INC_PATH  CFG_TUSB_OS_INC_PATH_DEFAULT
#endif

//--------------------------------------------------------------------
// Device Options (Default)
//--------------------------------------------------------------------

// Attribute to place data in accessible RAM for device controller (default: CFG_TUSB_MEM_SECTION)
#ifndef CFG_TUD_MEM_SECTION
  #define CFG_TUD_MEM_SECTION     CFG_TUSB_MEM_SECTION
#endif

// Attribute to align memory for device controller (default: CFG_TUSB_MEM_ALIGN)
#ifndef CFG_TUD_MEM_ALIGN
  #define CFG_TUD_MEM_ALIGN       CFG_TUSB_MEM_ALIGN
#endif

#ifndef CFG_TUD_MEM_DCACHE_ENABLE
  #ifndef CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT
  #define CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT  0
  #endif

  #define CFG_TUD_MEM_DCACHE_ENABLE   CFG_TUD_MEM_DCACHE_ENABLE_DEFAULT
#endif

#ifndef CFG_TUD_MEM_DCACHE_LINE_SIZE
  #define CFG_TUD_MEM_DCACHE_LINE_SIZE CFG_TUSB_MEM_DCACHE_LINE_SIZE
#endif

#ifndef CFG_TUD_ENDPOINT0_SIZE
  #define CFG_TUD_ENDPOINT0_SIZE  64
#endif

#ifndef CFG_TUD_ENDPOINT0_BUFSIZE
  #define CFG_TUD_ENDPOINT0_BUFSIZE  CFG_TUD_ENDPOINT0_SIZE
#endif

#ifndef CFG_TUD_INTERFACE_MAX
  #define CFG_TUD_INTERFACE_MAX   16
#endif

// max events processed in one tud_task_ext() call, 0 for unlimited
#ifndef CFG_TUD_TASK_EVENTS_PER_RUN
  #define CFG_TUD_TASK_EVENTS_PER_RUN  16
#endif

// default to max hardware endpoint, but can be smaller to save RAM
#ifndef CFG_TUD_ENDPPOINT_MAX
  #define CFG_TUD_ENDPPOINT_MAX   TUP_DCD_ENDPOINT_MAX
#endif

#if CFG_TUD_ENDPPOINT_MAX > TUP_DCD_ENDPOINT_MAX
  #error "CFG_TUD_ENDPPOINT_MAX must be less than or equal to TUP_DCD_ENDPOINT_MAX"
#endif

// USB 2.0 7.1.20: compliance test mode support
#ifndef CFG_TUD_TEST_MODE
  #define CFG_TUD_TEST_MODE       0
#endif

#ifndef CFG_TUD_VBUS_DETECT_HW_DEFAULT
  #define CFG_TUD_VBUS_DETECT_HW_DEFAULT 0
#endif

// Enable VBUS Detect hardware, usually via functional GPIO
#ifndef CFG_TUD_VBUS_DETECT_HW
  #define CFG_TUD_VBUS_DETECT_HW CFG_TUD_VBUS_DETECT_HW_DEFAULT
#endif

//------------- Device Class Driver -------------//
#ifndef CFG_TUD_BTH
  #define CFG_TUD_BTH 0
#endif

#if CFG_TUD_BTH && !defined(CFG_TUD_BTH_ISO_ALT_COUNT)
#error CFG_TUD_BTH_ISO_ALT_COUNT must be defined to tell Bluetooth driver the number of ISO endpoints to use
#endif

#ifndef CFG_TUD_CDC
  #define CFG_TUD_CDC             0
#endif

#ifndef CFG_TUD_MSC
  #define CFG_TUD_MSC             0
#endif

#ifndef CFG_TUD_MTP
  #define CFG_TUD_MTP             0
#endif

#ifndef CFG_TUD_HID
  #define CFG_TUD_HID             0
#endif

#ifndef CFG_TUD_AUDIO
  #define CFG_TUD_AUDIO           0
#endif

#ifndef CFG_TUD_VIDEO
  #define CFG_TUD_VIDEO           0
#endif

#ifndef CFG_TUD_MIDI
  #define CFG_TUD_MIDI            0
#endif

#ifndef CFG_TUD_MIDI2
  #define CFG_TUD_MIDI2           0
#endif

#ifndef CFG_TUD_VENDOR
  #define CFG_TUD_VENDOR          0
#endif

#ifndef CFG_TUD_USBTMC
  #define CFG_TUD_USBTMC          0
#endif

#ifndef CFG_TUD_DFU_RUNTIME
  #define CFG_TUD_DFU_RUNTIME     0
#endif

#ifndef CFG_TUD_DFU
  #define CFG_TUD_DFU             0
#endif

#ifndef CFG_TUD_ECM_RNDIS
  #ifdef CFG_TUD_NET
    #warning "CFG_TUD_NET is renamed to CFG_TUD_ECM_RNDIS"
    #define CFG_TUD_ECM_RNDIS   CFG_TUD_NET
  #else
    #define CFG_TUD_ECM_RNDIS   0
  #endif
#endif

#ifndef CFG_TUD_NCM
  #define CFG_TUD_NCM         0
#endif

#ifndef CFG_TUD_PRINTER
  #define CFG_TUD_PRINTER         0
#endif

#ifndef CFG_TUD_EDPT_DEDICATED_HWFIFO
  #define CFG_TUD_EDPT_DEDICATED_HWFIFO 0
#endif

//--------------------------------------------------------------------
// Host Options (Default)
//--------------------------------------------------------------------
#if CFG_TUH_ENABLED
  #ifndef CFG_TUH_DEVICE_MAX
    #define CFG_TUH_DEVICE_MAX 1
  #endif

  #ifndef CFG_TUH_ENUMERATION_BUFSIZE
    #define CFG_TUH_ENUMERATION_BUFSIZE 256
  #endif

  // Timeout in ms for each control transfer; raise it for a device with slow control requests.
  // A broken device that ACKs SETUP but never completes a later stage (e.g NAK forever) would
  // otherwise stall the control pipe - and during enumeration also hub polling - forever.
  // 5s matches Linux's USB_CTRL_GET/SET_TIMEOUT.
  #ifndef CFG_TUH_CONTROL_TIMEOUT_MS
    #define CFG_TUH_CONTROL_TIMEOUT_MS 5000
  #endif

  // Total enumeration attempts per attach: when the device fails an enumeration transfer, the
  // port is reset and enumeration restarted. Some devices wedge EP0 when a bus reset arrives in
  // their enumerated state but recover on the next reset. 1 = no retry.
  #ifndef CFG_TUH_ENUM_ATTEMPT_MAX
    #define CFG_TUH_ENUM_ATTEMPT_MAX 2
  #endif

#endif // CFG_TUH_ENABLED

// Attribute to place data in accessible RAM for host controller (default: CFG_TUSB_MEM_SECTION)
#ifndef CFG_TUH_MEM_SECTION
  #define CFG_TUH_MEM_SECTION   CFG_TUSB_MEM_SECTION
#endif

// Attribute to align memory for host controller
#ifndef CFG_TUH_MEM_ALIGN
  #define CFG_TUH_MEM_ALIGN     CFG_TUSB_MEM_ALIGN
#endif

#ifndef CFG_TUH_MEM_DCACHE_ENABLE
  #ifndef CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT
  #define CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT  0
  #endif

  #define CFG_TUH_MEM_DCACHE_ENABLE   CFG_TUH_MEM_DCACHE_ENABLE_DEFAULT
#endif

#ifndef CFG_TUH_MEM_DCACHE_LINE_SIZE
  #define CFG_TUH_MEM_DCACHE_LINE_SIZE CFG_TUSB_MEM_DCACHE_LINE_SIZE
#endif

// max events processed in one tuh_task_ext() call, 0 for unlimited
#ifndef CFG_TUH_TASK_EVENTS_PER_RUN
  #define CFG_TUH_TASK_EVENTS_PER_RUN  16
#endif

//------------- CLASS -------------//

#ifndef CFG_TUH_HUB
  #define CFG_TUH_HUB    0
#endif

#ifndef CFG_TUH_CDC
  #define CFG_TUH_CDC    0
#endif

// FTDI is not part of CDC class, only to re-use CDC driver API
#ifndef CFG_TUH_CDC_FTDI
  #define CFG_TUH_CDC_FTDI 0
#endif

// List of product IDs that can use the FTDI CDC driver. 0x0403 is FTDI's VID
#ifndef CFG_TUH_CDC_FTDI_VID_PID_LIST
  #define CFG_TUH_CDC_FTDI_VID_PID_LIST \
    {0x0403, 0x6001}, /* Similar device to SIO above */ \
    {0x0403, 0x6006}, /* FTDI's alternate PID for above */ \
    {0x0403, 0x6010}, /* Dual channel device */ \
    {0x0403, 0x6011}, /* Quad channel hi-speed device */ \
    {0x0403, 0x6014}, /* Single channel hi-speed device */ \
    {0x0403, 0x6015}, /* FT-X series (FT201X, FT230X, FT231X, etc) */ \
    {0x0403, 0x6040}, /* Dual channel hi-speed device with PD */ \
    {0x0403, 0x6041}, /* Quad channel hi-speed device with PD */ \
    {0x0403, 0x6042}, /* Dual channel hi-speed device with PD */ \
    {0x0403, 0x6043}, /* Quad channel hi-speed device with PD */ \
    {0x0403, 0x6044}, /* Dual channel hi-speed device with PD */ \
    {0x0403, 0x6045}, /* Dual channel hi-speed device with PD */ \
    {0x0403, 0x6048}, /* Quad channel automotive grade hi-speed device */ \
    {0x0403, 0x8372}, /* Product Id SIO application of 8U100AX */ \
    {0x0403, 0xFBFA}, /* Product ID for FT232RL */ \
    {0x0403, 0xCD18}, /* ??? */
#endif

// CP210X is not part of CDC class, only to re-use CDC driver API
#ifndef CFG_TUH_CDC_CP210X
  #define CFG_TUH_CDC_CP210X 0
#endif

// List of product IDs that can use the CP210X CDC driver. 0x10C4 is Silicon Labs' VID
#ifndef CFG_TUH_CDC_CP210X_VID_PID_LIST
  #define CFG_TUH_CDC_CP210X_VID_PID_LIST \
  { 0x10C4, 0xEA60 }, /* Silicon Labs factory default */ \
  { 0x10C4, 0xEA61 }, /* Silicon Labs factory default */ \
  { 0x10C4, 0xEA70 }  /* Silicon Labs Dual Port factory default */
#endif

#ifndef CFG_TUH_CDC_CH34X
  // CH34X is not part of CDC class, only to re-use CDC driver API
  #define CFG_TUH_CDC_CH34X 0
#endif

// List of product IDs that can use the CH34X CDC driver
#ifndef CFG_TUH_CDC_CH34X_VID_PID_LIST
  #define CFG_TUH_CDC_CH34X_VID_PID_LIST \
    { 0x1a86, 0x5523 }, /* ch341 chip */ \
    { 0x1a86, 0x7522 }, /* ch340k chip */ \
    { 0x1a86, 0x7523 }, /* ch340 chip */ \
    { 0x1a86, 0xe523 }, /* ch330 chip */ \
    { 0x4348, 0x5523 }, /* ch340 custom chip */ \
    { 0x2184, 0x0057 }, /* overtaken from Linux Kernel driver /drivers/usb/serial/ch341.c */ \
    { 0x9986, 0x7523 }  /* overtaken from Linux Kernel driver /drivers/usb/serial/ch341.c */
#endif

#ifndef CFG_TUH_CDC_PL2303
  // PL2303 is not part of CDC class, only to re-use CDC driver API
  #define CFG_TUH_CDC_PL2303 0
#endif

#ifndef CFG_TUH_CDC_PL2303_VID_PID_LIST
  // List of product IDs that can use the PL2303 CDC driver
  #define CFG_TUH_CDC_PL2303_VID_PID_LIST \
  { 0x067b, 0x2303 }, /* initial 2303 */ \
  { 0x067b, 0x2304 }, /* TB */ \
  { 0x067b, 0x23a3 }, /* GC */ \
  { 0x067b, 0x23b3 }, /* GB */ \
  { 0x067b, 0x23c3 }, /* GT */ \
  { 0x067b, 0x23d3 }, /* GL */ \
  { 0x067b, 0x23e3 }, /* GE */ \
  { 0x067b, 0x23f3 }  /* GS */
#endif

#ifndef CFG_TUH_AUDIO
  #define CFG_TUH_AUDIO  0
#endif

#ifndef CFG_TUH_HID
  #define CFG_TUH_HID    0
#endif

#ifndef CFG_TUH_MIDI
  #define CFG_TUH_MIDI   0
#endif

#ifndef CFG_TUH_MIDI2
  #define CFG_TUH_MIDI2  0
#endif

#ifndef CFG_TUH_MIDI2_RX_BUFSIZE
  #define CFG_TUH_MIDI2_RX_BUFSIZE TUH_EPSIZE_BULK_MAX
#endif

#ifndef CFG_TUH_MIDI2_TX_BUFSIZE
  #define CFG_TUH_MIDI2_TX_BUFSIZE TUH_EPSIZE_BULK_MAX
#endif

#ifndef CFG_TUH_MIDI2_LOG_LEVEL
  #define CFG_TUH_MIDI2_LOG_LEVEL CFG_TUH_LOG_LEVEL
#endif

#ifndef CFG_TUH_MSC
  #define CFG_TUH_MSC    0
#endif


#ifndef CFG_TUH_API_EDPT_XFER
  #define CFG_TUH_API_EDPT_XFER 0
#endif

#ifndef CFG_TUH_EDPT_DEDICATED_HWFIFO
  #define CFG_TUH_EDPT_DEDICATED_HWFIFO 0
#endif

//--------------------------------------------------------------------+
// TypeC Options (Default)
//--------------------------------------------------------------------+

#ifndef CFG_TUC_ENABLED
#define CFG_TUC_ENABLED 0

#define tuc_int_handler(_p)
#endif

//------------------------------------------------------------------
// Configuration Validation
//------------------------------------------------------------------
#if CFG_TUD_ENDPOINT0_SIZE > 64
  #error Control Endpoint Max Packet Size cannot be larger than 64
#endif

// To avoid GCC compiler warnings when -pedantic option is used (strict ISO C)
typedef int make_iso_compilers_happy;
