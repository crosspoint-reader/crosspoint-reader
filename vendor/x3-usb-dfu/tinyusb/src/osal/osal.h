/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_OSAL_H_
#define TUSB_OSAL_H_

#ifdef __cplusplus
 extern "C" {
#endif

#include "common/tusb_common.h"

typedef void (*osal_task_func_t)(void* param);

// Timeout
#define OSAL_TIMEOUT_NOTIMEOUT     (0)          // Return immediately
#define OSAL_TIMEOUT_NORMAL        (10)         // Default timeout
#define OSAL_TIMEOUT_WAIT_FOREVER  (UINT32_MAX) // Wait forever
#define OSAL_TIMEOUT_CONTROL_XFER  OSAL_TIMEOUT_WAIT_FOREVER

// Mutex is required when using a preempted RTOS or MCU has multiple cores
#if (CFG_TUSB_OS == OPT_OS_NONE) && !TUP_MCU_MULTIPLE_CORE
  #define OSAL_MUTEX_REQUIRED   0
  #define OSAL_MUTEX_DEF(_name) uint8_t :0
#else
  #define OSAL_MUTEX_REQUIRED   1
  #define OSAL_MUTEX_DEF(_name) osal_mutex_def_t _name
#endif

// OS thin implementation
#if CFG_TUSB_OS == OPT_OS_NONE
  #include "osal_none.h"
#elif CFG_TUSB_OS == OPT_OS_FREERTOS
  #include "osal_freertos.h"
#elif CFG_TUSB_OS == OPT_OS_MYNEWT
  #include "osal_mynewt.h"
#elif CFG_TUSB_OS == OPT_OS_PICO
  #include "osal_pico.h"
#elif CFG_TUSB_OS == OPT_OS_RTTHREAD
  #include "osal_rtthread.h"
#elif CFG_TUSB_OS == OPT_OS_RTX4
  #include "osal_rtx4.h"
#elif CFG_TUSB_OS == OPT_OS_ZEPHYR
  #include "osal_zephyr.h"
#elif CFG_TUSB_OS == OPT_OS_THREADX
  #include "osal_threadx.h"
#elif CFG_TUSB_OS == OPT_OS_CUSTOM
  #include "tusb_os_custom.h" // implemented by application
#else
  #error OS is not supported yet
#endif

/*--------------------------------------------------------------------
  OSAL Porting API
  Should be implemented as static inline function in osal_port.h header
    uint32_t osal_time_millis(void);

    void osal_task_delay(uint32_t msec);
    osal_task_handle_t osal_task_get_current_handle(void);

    void osal_spin_init(osal_spinlock_t *ctx);
    void osal_spin_deinit(osal_spinlock_t *ctx);
    void osal_spin_lock(osal_spinlock_t *ctx, bool in_isr);
    void osal_spin_unlock(osal_spinlock_t *ctx, bool in_isr);

    osal_semaphore_t osal_semaphore_create(osal_semaphore_def_t* semdef);
    bool osal_semaphore_delete(osal_semaphore_t semd_hdl);
    bool osal_semaphore_post(osal_semaphore_t sem_hdl, bool in_isr);
    bool osal_semaphore_wait(osal_semaphore_t sem_hdl, uint32_t msec);
    void osal_semaphore_reset(osal_semaphore_t sem_hdl);

    osal_mutex_t osal_mutex_create(osal_mutex_def_t* mdef);
    bool osal_mutex_delete(osal_mutex_t mutex_hdl)
    bool osal_mutex_lock (osal_mutex_t sem_hdl, uint32_t msec);
    bool osal_mutex_unlock(osal_mutex_t mutex_hdl);

    osal_queue_t osal_queue_create(osal_queue_def_t* qdef);
    bool osal_queue_delete(osal_queue_t qhdl);
    bool osal_queue_receive(osal_queue_t qhdl, void* data, uint32_t msec);
    bool osal_queue_send(osal_queue_t qhdl, void const * data, bool in_isr);
    bool osal_queue_empty(osal_queue_t qhdl);
--------------------------------------------------------------------------*/


#ifdef __cplusplus
 }
#endif

#endif
