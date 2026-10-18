/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_OSAL_NONE_H_
#define TUSB_OSAL_NONE_H_

#ifdef __cplusplus
extern "C" {
#endif

// osal_time_millis() is not provided, tusb_time_millis_api() must be implemented by user application

//--------------------------------------------------------------------+
// TASK API
//--------------------------------------------------------------------+
// Bare-metal single context: return a non-NULL sentinel so equality compares true.
typedef void* osal_task_handle_t;

TU_ATTR_ALWAYS_INLINE static inline osal_task_handle_t osal_task_get_current_handle(void) {
  return (osal_task_handle_t) 1;
}

// Bare-metal has no scheduler to yield to; this is dead code in practice because
// callers gate it on running outside the host task, which can't happen here.
TU_ATTR_ALWAYS_INLINE static inline void osal_task_delay(uint32_t msec) {
  (void) msec;
}

//--------------------------------------------------------------------+
// Spinlock API
//--------------------------------------------------------------------+
// Note: This implementation is designed for bare-metal single-core systems without RTOS.
// - Supports nested locking within the same execution context
// - NOT suitable for true SMP (Symmetric Multi-Processing) systems
// - NOT thread-safe for multi-threaded environments
// - Primarily manages interrupt enable/disable state for critical sections
typedef struct {
  void (* interrupt_set)(bool enabled);
  uint32_t nested_count;
} osal_spinlock_t;

// For SMP, spinlock must be locked by hardware, cannot just use interrupt
#define OSAL_SPINLOCK_DEF(_name, _int_set) \
  osal_spinlock_t _name = { .interrupt_set = _int_set, .nested_count = 0 }

TU_ATTR_ALWAYS_INLINE static inline void osal_spin_init(osal_spinlock_t *ctx) {
  (void) ctx;
}

TU_ATTR_ALWAYS_INLINE static inline void osal_spin_deinit(osal_spinlock_t *ctx) {
  (void) ctx;
}

TU_ATTR_ALWAYS_INLINE static inline void osal_spin_lock(osal_spinlock_t *ctx, bool in_isr) {
  // Disable interrupts first to make nested_count increment atomic
  if (!in_isr && ctx->nested_count == 0) {
    ctx->interrupt_set(false);
  }
  ctx->nested_count++;
}

TU_ATTR_ALWAYS_INLINE static inline void osal_spin_unlock(osal_spinlock_t *ctx, bool in_isr) {
  if (ctx->nested_count == 0) {
    return; // spin is not locked to begin with
  }

  ctx->nested_count--;

  // Only re-enable interrupts when fully unlocked
  if (!in_isr && ctx->nested_count == 0) {
    ctx->interrupt_set(true);
  }
}

//--------------------------------------------------------------------+
// Binary Semaphore API
//--------------------------------------------------------------------+
typedef struct {
  volatile uint16_t count;
} osal_semaphore_def_t;

typedef osal_semaphore_def_t* osal_semaphore_t;

TU_ATTR_ALWAYS_INLINE static inline osal_semaphore_t osal_semaphore_create(osal_semaphore_def_t* semdef) {
  semdef->count = 0;
  return semdef;
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_semaphore_delete(osal_semaphore_t semd_hdl) {
  (void) semd_hdl;
  return true; // nothing to do
}


TU_ATTR_ALWAYS_INLINE static inline bool osal_semaphore_post(osal_semaphore_t sem_hdl, bool in_isr) {
  (void) in_isr;
  sem_hdl->count++;
  return true;
}

// TODO blocking for now
TU_ATTR_ALWAYS_INLINE static inline bool osal_semaphore_wait(osal_semaphore_t sem_hdl, uint32_t msec) {
  (void) msec;

  while (sem_hdl->count == 0) {}
  sem_hdl->count--;

  return true;
}

TU_ATTR_ALWAYS_INLINE static inline void osal_semaphore_reset(osal_semaphore_t sem_hdl) {
  sem_hdl->count = 0;
}

//--------------------------------------------------------------------+
// MUTEX API
// Within tinyusb, mutex is never used in ISR context
//--------------------------------------------------------------------+
typedef osal_semaphore_def_t osal_mutex_def_t;
typedef osal_semaphore_t osal_mutex_t;

#if OSAL_MUTEX_REQUIRED
// Note: multiple cores MCUs usually do provide IPC API for mutex
// or we can use std atomic function

TU_ATTR_ALWAYS_INLINE static inline osal_mutex_t osal_mutex_create(osal_mutex_def_t* mdef) {
  mdef->count = 1;
  return mdef;
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_mutex_delete(osal_mutex_t mutex_hdl) {
  (void) mutex_hdl;
  return true; // nothing to do
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_mutex_lock (osal_mutex_t mutex_hdl, uint32_t msec) {
  return osal_semaphore_wait(mutex_hdl, msec);
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_mutex_unlock(osal_mutex_t mutex_hdl) {
  return osal_semaphore_post(mutex_hdl, false);
}

#else

#define osal_mutex_create(_mdef)          (NULL)
#define osal_mutex_lock(_mutex_hdl, _ms)  (true)
#define osal_mutex_unlock(_mutex_hdl)     (true)

#endif

//--------------------------------------------------------------------+
// QUEUE API
//--------------------------------------------------------------------+
#include "common/tusb_fifo.h"

typedef struct {
  void (* interrupt_set)(bool enabled);
  uint16_t  item_size;
  tu_fifo_t ff;
} osal_queue_def_t;

typedef osal_queue_def_t* osal_queue_t;

// _int_set is used as mutex in OS NONE (disable/enable USB ISR)
#define OSAL_QUEUE_DEF(_int_set, _name, _depth, _type)                                                 \
  uint8_t          _name##_buf[_depth * sizeof(_type)];                                                \
  osal_queue_def_t _name = {.interrupt_set = _int_set,                                                 \
                            .item_size     = sizeof(_type),                                            \
                            .ff            = TU_FIFO_INIT(_name##_buf, _depth * sizeof(_type), false)}

TU_ATTR_ALWAYS_INLINE static inline osal_queue_t osal_queue_create(osal_queue_def_t* qdef) {
  tu_fifo_clear(&qdef->ff);
  return (osal_queue_t) qdef;
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_queue_delete(osal_queue_t qhdl) {
  (void) qhdl;
  return true; // nothing to do
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_queue_receive(osal_queue_t qhdl, void* data, uint32_t msec) {
  (void) msec; // not used, always behave as msec = 0

  qhdl->interrupt_set(false);
  const bool success = (tu_fifo_read_n(&qhdl->ff, data, qhdl->item_size) > 0);
  qhdl->interrupt_set(true);

  return success;
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_queue_send(osal_queue_t qhdl, void const* data, bool in_isr) {
  if (!in_isr) {
    qhdl->interrupt_set(false);
  }

  const bool success = (tu_fifo_write_n(&qhdl->ff, data, qhdl->item_size) > 0);

  if (!in_isr) {
    qhdl->interrupt_set(true);
  }

  return success;
}

TU_ATTR_ALWAYS_INLINE static inline bool osal_queue_empty(osal_queue_t qhdl) {
  // Skip queue lock/unlock since this function is primarily called
  // with interrupt disabled before going into low power mode
  return tu_fifo_empty(&qhdl->ff);
}

#ifdef __cplusplus
}
#endif

#endif
