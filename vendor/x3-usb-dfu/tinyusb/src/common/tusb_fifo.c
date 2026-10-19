/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-FileCopyrightText: Copyright (c) 2020 Reinhard Panhuber
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#include "osal/osal.h"
#include "tusb_fifo.h"

#define TU_FIFO_DBG 0


#if OSAL_MUTEX_REQUIRED

TU_ATTR_ALWAYS_INLINE static inline void ff_lock(osal_mutex_t mutex) {
  if (mutex != NULL) {
    osal_mutex_lock(mutex, OSAL_TIMEOUT_WAIT_FOREVER);
  }
}

TU_ATTR_ALWAYS_INLINE static inline void ff_unlock(osal_mutex_t mutex) {
  if (mutex != NULL) {
    osal_mutex_unlock(mutex);
  }
}

#else
  #define ff_lock(_mutex)
  #define ff_unlock(_mutex)

#endif

//--------------------------------------------------------------------+
// Setup API
//--------------------------------------------------------------------+
bool tu_fifo_config(tu_fifo_t *f, void *buffer, uint16_t depth, bool overwritable) {
  // Limit index space to 2*depth - this allows for a fast "modulo" calculation
  // but limits the maximum depth to 2^16/2 = 2^15 and buffer overflows are detectable
  // only if overflow happens once (important for unsupervised DMA applications)
  if (depth > 0x8000) {
    return false;
  }

  ff_lock(f->mutex_wr);
  ff_lock(f->mutex_rd);

  f->buffer       = (uint8_t *)buffer;
  f->depth        = depth;
  f->overwritable = overwritable;
  f->rd_idx       = 0u;
  f->wr_idx       = 0u;

  ff_unlock(f->mutex_wr);
  ff_unlock(f->mutex_rd);

  return true;
}

// clear fifo by resetting read and write indices
void tu_fifo_clear(tu_fifo_t *f) {
  ff_lock(f->mutex_wr);
  ff_lock(f->mutex_rd);

  f->rd_idx = 0;
  f->wr_idx = 0;

  ff_unlock(f->mutex_wr);
  ff_unlock(f->mutex_rd);
}

// Change the fifo overwritable mode
void tu_fifo_set_overwritable(tu_fifo_t *f, bool overwritable) {
  if (f->overwritable == overwritable) {
    return;
  }

  ff_lock(f->mutex_wr);
  ff_lock(f->mutex_rd);

  f->overwritable = overwritable;

  ff_unlock(f->mutex_wr);
  ff_unlock(f->mutex_rd);
}

//--------------------------------------------------------------------+
// Hardware FIFO API
// Support different data access width and address increment scheme
// Can support multiple i.e both 16 and 32-bit data access if needed
//--------------------------------------------------------------------+
#if CFG_TUSB_FIFO_HWFIFO_API
  #if CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE > 0
    #define HWFIFO_ADDR_NEXT_N(_hwfifo, _const, _n) _hwfifo = (_const volatile void *)((uintptr_t)(_hwfifo) + _n)
  #else
    #define HWFIFO_ADDR_NEXT_N(_hwfifo, _const, _n)
  #endif

  #define HWFIFO_ADDR_NEXT(_hwfifo, _const) HWFIFO_ADDR_NEXT_N(_hwfifo, _const, CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE)

  // the fixed ratio works since in the only case of dynamic/multiple data_stride (rusb2): addr_stride is 0
  #define HWFIFO_ADDR_DATA_RATIO (CFG_TUSB_FIFO_HWFIFO_ADDR_STRIDE / CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE)

//------------- Write -------------//
  #ifndef CFG_TUSB_FIFO_HWFIFO_CUSTOM_WRITE
TU_ATTR_ALWAYS_INLINE static inline void stride_write(volatile void *hwfifo, const void *src, uint8_t data_stride) {
  (void)data_stride; // possible unused
    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE & 4
      #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE != 4
  if (data_stride == 4)
      #endif
  {
    *((volatile uint32_t *)hwfifo) = tu_unaligned_read32(src);
  }
    #endif

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE & 2
      #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE != 2
  if (data_stride == 2)
      #endif
  {
    *((volatile uint16_t *)hwfifo) = tu_unaligned_read16(src);
  }
    #endif

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE == 1
  *((volatile uint8_t *)hwfifo) = *(const uint8_t *)src;
    #endif
}

// Copy from fifo to fixed address buffer (usually a tx register) with TU_FIFO_FIXED_ADDR_RW32 mode
void tu_hwfifo_write(volatile void *hwfifo, const uint8_t *src, uint16_t len, const tu_hwfifo_access_t *access_mode) {
  // Write full available 16/32 bit words to dest
  const uint8_t data_stride = (access_mode != NULL) ? access_mode->data_stride : CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE;
  while (len >= data_stride) {
    stride_write(hwfifo, src, data_stride);
    src += data_stride;
    len -= data_stride;
    HWFIFO_ADDR_NEXT(hwfifo, );
  }

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE > 1
      #ifdef CFG_TUSB_FIFO_HWFIFO_DATA_ODD_16BIT_ACCESS
  // 16-bit access is allowed for odd bytes
  if (len >= 2) {
    *((volatile uint16_t *)hwfifo) = tu_unaligned_read16(src);
    src += 2;
    len -= 2;
    HWFIFO_ADDR_NEXT_N(hwfifo, , 2);
  }
      #endif

      #ifdef CFG_TUSB_FIFO_HWFIFO_DATA_ODD_8BIT_ACCESS
  // 8-bit access is allowed for odd bytes
  while (len > 0) {
    *((volatile uint8_t *)hwfifo) = *src++;
    len--;
    HWFIFO_ADDR_NEXT_N(hwfifo, , 1);
  }
      #else

  // Write odd bytes i.e 1 byte for 16 bit or 1-3 bytes for 32 bit
  if (len > 0) {
    uint32_t tmp = 0u;
    memcpy(&tmp, src, len);
    stride_write(hwfifo, &tmp, data_stride);
    HWFIFO_ADDR_NEXT(hwfifo, );
  }
      #endif
    #endif
}
  #endif

//------------- Read -------------//
  #ifndef CFG_TUSB_FIFO_HWFIFO_CUSTOM_READ
TU_ATTR_ALWAYS_INLINE static inline void stride_read(const volatile void *hwfifo, void *dest, uint8_t data_stride) {
  (void)data_stride; // possible unused

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE & 4
      #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE != 4
  if (data_stride == 4)
      #endif
  {
    tu_unaligned_write32(dest, *((const volatile uint32_t *)hwfifo));
  }
    #endif

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE & 2
      #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE != 2
  if (data_stride == 2)
      #endif
  {
    tu_unaligned_write16(dest, *((const volatile uint16_t *)hwfifo));
  }
    #endif

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE == 1
  *(uint8_t *)dest = *((const volatile uint8_t *)hwfifo);
    #endif
}

void tu_hwfifo_read(const volatile void *hwfifo, uint8_t *dest, uint16_t len, const tu_hwfifo_access_t *access_mode) {
  // Reading full available 16/32-bit hwfifo and write to fifo
  const uint8_t data_stride = (access_mode != NULL) ? access_mode->data_stride : CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE;
  while (len >= data_stride) {
    stride_read(hwfifo, dest, data_stride);
    dest += data_stride;
    len -= data_stride;
    HWFIFO_ADDR_NEXT(hwfifo, const);
  }

    #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE > 1
      #ifdef CFG_TUSB_FIFO_HWFIFO_DATA_ODD_16BIT_ACCESS
  // 16-bit access is allowed for odd bytes
  if (len >= 2) {
    tu_unaligned_write16(dest, *((const volatile uint16_t *)hwfifo));
    dest += 2;
    len -= 2;
    HWFIFO_ADDR_NEXT_N(hwfifo, const, 2);
  }
      #endif

      #ifdef CFG_TUSB_FIFO_HWFIFO_DATA_ODD_8BIT_ACCESS
  // 8-bit access is allowed for odd bytes
  while (len > 0) {
    *dest++ = *((const volatile uint8_t *)hwfifo);
    len--;
    HWFIFO_ADDR_NEXT_N(hwfifo, const, 1);
  }
      #else
  // Read odd bytes i.e 1 byte for 16 bit or 1-3 bytes for 32 bit
  if (len > 0) {
    uint32_t tmp;
    stride_read(hwfifo, &tmp, data_stride);
    memcpy(dest, &tmp, len);
    HWFIFO_ADDR_NEXT(hwfifo, const);
  }
      #endif
    #endif
}
  #endif

// push to sw fifo from hwfifo
static void hwff_push_n(const tu_fifo_t *f, const void *app_buf, uint16_t n, uint16_t wr_ptr,
                        const tu_hwfifo_access_t *access_mode) {
  uint16_t lin_bytes  = f->depth - wr_ptr;
  uint16_t wrap_bytes = n - lin_bytes;
  uint8_t *ff_buf     = f->buffer + wr_ptr;

  const volatile void *hwfifo = (const volatile void *)app_buf;
  if (n <= lin_bytes) {
    // Linear only case
    tu_hwfifo_read(hwfifo, ff_buf, n, access_mode);
  } else {
    // Wrap around case
  #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE == 1
    tu_hwfifo_read(hwfifo, ff_buf, lin_bytes, access_mode);     // linear part
    HWFIFO_ADDR_NEXT_N(hwfifo, const, lin_bytes);
    tu_hwfifo_read(hwfifo, f->buffer, wrap_bytes, access_mode); // wrapped part
  #else
    // Write full words to the linear part of the buffer
    const uint8_t  data_stride = access_mode->data_stride;
    const uint32_t odd_mask    = data_stride - 1;
    uint16_t       lin_even    = (uint16_t)(lin_bytes & ~odd_mask);
    tu_hwfifo_read(hwfifo, ff_buf, lin_even, access_mode);
    HWFIFO_ADDR_NEXT_N(hwfifo, const, lin_even * HWFIFO_ADDR_DATA_RATIO);
    ff_buf += lin_even;

    // There could be an odd 1 byte (16bit) or 1-3 bytes (32bit) before the wrap-around boundary
    // combine it with the wrapped part to form a full word for data stride
    const uint8_t lin_odd = (uint8_t)(lin_bytes & odd_mask);
    if (lin_odd > 0) {
      const uint8_t wrap_odd = (uint8_t)tu_min16(wrap_bytes, data_stride - lin_odd);
      uint8_t       buf_temp[4];
      tu_hwfifo_read(hwfifo, buf_temp, lin_odd + wrap_odd, access_mode);
      HWFIFO_ADDR_NEXT(hwfifo, const);

      for (uint8_t i = 0; i < lin_odd; ++i) {
        ff_buf[i] = buf_temp[i];
      }
      for (uint8_t i = 0; i < wrap_odd; ++i) {
        f->buffer[i] = buf_temp[lin_odd + i];
      }

      wrap_bytes -= wrap_odd;
      ff_buf = f->buffer + wrap_odd; // wrap around
    } else {
      ff_buf = f->buffer;            // wrap around to beginning
    }

    // Write data wrapped part
    if (wrap_bytes > 0) {
      tu_hwfifo_read(hwfifo, ff_buf, wrap_bytes, access_mode);
    }
  #endif
  }
}

// pull from sw fifo to hwfifo
static void hwff_pull_n(const tu_fifo_t *f, void *app_buf, uint16_t n, uint16_t rd_ptr,
                        const tu_hwfifo_access_t *access_mode) {
  uint16_t       lin_bytes  = f->depth - rd_ptr;
  uint16_t       wrap_bytes = n - lin_bytes; // only used if wrapped
  const uint8_t *ff_buf     = f->buffer + rd_ptr;

  volatile void *hwfifo = (volatile void *)app_buf;

  if (n <= lin_bytes) {
    // Linear only case
    tu_hwfifo_write(hwfifo, ff_buf, n, access_mode);
  } else {
    // Wrap around case
  #if CFG_TUSB_FIFO_HWFIFO_DATA_STRIDE == 1
    tu_hwfifo_write(hwfifo, ff_buf, lin_bytes, access_mode);     // linear part
    HWFIFO_ADDR_NEXT_N(hwfifo, , lin_bytes);
    tu_hwfifo_write(hwfifo, f->buffer, wrap_bytes, access_mode); // wrapped part
  #else
    // Read full words from linear part
    const uint8_t  data_stride = access_mode->data_stride;
    const uint32_t odd_mask    = data_stride - 1;
    uint16_t       lin_even    = (uint16_t)(lin_bytes & ~odd_mask);
    tu_hwfifo_write(hwfifo, ff_buf, lin_even, access_mode);
    HWFIFO_ADDR_NEXT_N(hwfifo, , lin_even * HWFIFO_ADDR_DATA_RATIO);
    ff_buf += lin_even;

    // There could be odd 1 byte (16bit) or 1-3 bytes (32bit) before the wrap-around boundary
    const uint8_t lin_odd = (uint8_t)(lin_bytes & odd_mask);
    if (lin_odd > 0) {
      const uint8_t wrap_odd = (uint8_t)tu_min16(wrap_bytes, data_stride - lin_odd);

      uint8_t buf_temp[4];
      for (uint8_t i = 0; i < lin_odd; ++i) {
        buf_temp[i] = ff_buf[i];
      }
      for (uint8_t i = 0; i < wrap_odd; ++i) {
        buf_temp[lin_odd + i] = f->buffer[i];
      }

      tu_hwfifo_write(hwfifo, buf_temp, lin_odd + wrap_odd, access_mode);
      HWFIFO_ADDR_NEXT(hwfifo, );

      wrap_bytes -= wrap_odd;
      ff_buf = f->buffer + wrap_odd; // wrap around
    } else {
      ff_buf = f->buffer;            // wrap around to beginning
    }

    // Read data wrapped part
    if (wrap_bytes > 0) {
      tu_hwfifo_write(hwfifo, ff_buf, wrap_bytes, access_mode);
    }
  #endif
  }
}
#endif

//--------------------------------------------------------------------+
// Pull & Push
// copy data to/from fifo without updating read/write pointers
//--------------------------------------------------------------------+
// send n items to fifo WITHOUT updating write pointer
static void ff_push_n(const tu_fifo_t *f, const void *app_buf, uint16_t n, uint16_t wr_ptr) {
  uint16_t lin_bytes  = f->depth - wr_ptr;
  uint16_t wrap_bytes = n - lin_bytes;
  uint8_t *ff_buf     = f->buffer + wr_ptr;

  if (n <= lin_bytes) {
    // Linear only case
    memcpy(ff_buf, app_buf, n);
  } else {
    // Wrap around case
    memcpy(ff_buf, app_buf, lin_bytes);                                    // linear part
    memcpy(f->buffer, ((const uint8_t *)app_buf) + lin_bytes, wrap_bytes); // wrapped part
  }
}

// get n items from fifo WITHOUT updating read pointer
static void ff_pull_n(const tu_fifo_t *f, void *app_buf, uint16_t n, uint16_t rd_ptr) {
  uint16_t       lin_bytes  = f->depth - rd_ptr;
  uint16_t       wrap_bytes = n - lin_bytes; // only used if wrapped
  const uint8_t *ff_buf     = f->buffer + rd_ptr;

  // single byte access
  if (n <= lin_bytes) {
    // Linear only
    memcpy(app_buf, ff_buf, n);
  } else {
    // Wrap around
    memcpy(app_buf, ff_buf, lin_bytes);                            // linear part
    memcpy((uint8_t *)app_buf + lin_bytes, f->buffer, wrap_bytes); // wrapped part
  }
}

//--------------------------------------------------------------------+
// Index Helper
//--------------------------------------------------------------------+

// Advance an absolute index
// "absolute" index is only in the range of [0..2*depth)
static uint16_t advance_index(uint16_t depth, uint16_t idx, uint16_t offset) {
  // We limit the index space of p such that a correct wrap around happens
  // Check for a wrap around or if we are in unused index space - This has to be checked first!!
  // We are exploiting the wrap around to the correct index
  uint16_t new_idx = (uint16_t)(idx + offset);
  if ((idx > new_idx) || (new_idx >= 2 * depth)) {
    const uint16_t non_used_index_space = (uint16_t)(UINT16_MAX - (2 * depth - 1));
    new_idx                             = (uint16_t)(new_idx + non_used_index_space);
  }

  return new_idx;
}

// index to pointer (0..depth-1), simply a modulo with minus.
TU_ATTR_ALWAYS_INLINE static inline uint16_t idx2ptr(uint16_t depth, uint16_t idx) {
  // Only run at most 3 times since index is limit in the range of [0..2*depth)
  while (idx >= depth) {
    idx -= depth;
  }
  return idx;
}

// Works on local copies of w
// When an overwritable fifo is overflowed, rd_idx will be re-index so that it forms a full fifo
static uint16_t correct_read_index(tu_fifo_t *f, uint16_t wr_idx) {
  uint16_t rd_idx;
  if (wr_idx >= f->depth) {
    rd_idx = wr_idx - f->depth;
  } else {
    rd_idx = wr_idx + f->depth;
  }

  f->rd_idx = rd_idx;
  return rd_idx;
}

//--------------------------------------------------------------------+
// n-API
//--------------------------------------------------------------------+

// Works on local copies of w and r
// Must be protected by read mutex since in case of an overflow read pointer gets modified
uint16_t tu_fifo_peek_n_access_mode(tu_fifo_t *f, void *p_buffer, uint16_t n, uint16_t wr_idx, uint16_t rd_idx,
                                    const tu_hwfifo_access_t *access_mode) {
  uint16_t count = tu_ff_overflow_count(f->depth, wr_idx, rd_idx);
  if (count == 0) {
    return 0; // nothing to peek
  }

  // Check overflow and correct if required
  if (count > f->depth) {
    rd_idx = correct_read_index(f, wr_idx);
    count  = f->depth;
  }

  if (count < n) {
    n = count; // limit to available count
  }

  const uint16_t rd_ptr = idx2ptr(f->depth, rd_idx);

#if CFG_TUSB_FIFO_HWFIFO_API
  if (access_mode != NULL) {
    hwff_pull_n(f, p_buffer, n, rd_ptr, access_mode);
  } else
#endif
  {
    (void)access_mode;
    ff_pull_n(f, p_buffer, n, rd_ptr);
  }

  return n;
}

// Read n items without removing it from the FIFO, correct read pointer if overflowed
uint16_t tu_fifo_peek_n(tu_fifo_t *f, void *p_buffer, uint16_t n) {
  ff_lock(f->mutex_rd);
  const uint16_t wr_idx = f->wr_idx;
  const uint16_t rd_idx = f->rd_idx;
  const uint16_t ret = tu_fifo_peek_n_access_mode(f, p_buffer, n, wr_idx, rd_idx, NULL);
  ff_unlock(f->mutex_rd);
  return ret;
}

// Read n items from fifo with access mode
uint16_t tu_fifo_read_n_access_mode(tu_fifo_t *f, void *buffer, uint16_t n, const tu_hwfifo_access_t *access_mode) {
  ff_lock(f->mutex_rd);

  // Peek the data: f->rd_idx might get modified in case of an overflow so we can not use a local variable
  const uint16_t wr_idx = f->wr_idx;
  n         = tu_fifo_peek_n_access_mode(f, buffer, n, wr_idx, f->rd_idx, access_mode);
  f->rd_idx = advance_index(f->depth, f->rd_idx, n);

  ff_unlock(f->mutex_rd);
  return n;
}

// Write n items to fifo with access mode
uint16_t tu_fifo_write_n_access_mode(tu_fifo_t *f, const void *data, uint16_t n,
                                     const tu_hwfifo_access_t *access_mode) {
  if (n == 0) {
    return 0;
  }

  ff_lock(f->mutex_wr);

  uint16_t wr_idx = f->wr_idx;
  uint16_t rd_idx = f->rd_idx;

  const uint8_t *buf8 = (const uint8_t *)data;

  TU_LOG(TU_FIFO_DBG, "rd = %3u, wr = %3u, count = %3u, remain = %3u, n = %3u:  ", rd_idx, wr_idx,
         tu_ff_overflow_count(f->depth, wr_idx, rd_idx), tu_ff_remaining_local(f->depth, wr_idx, rd_idx), n);

  if (!f->overwritable) {
    // limit up to full
    const uint16_t remain = tu_ff_remaining_local(f->depth, wr_idx, rd_idx);
    n                     = tu_min16(n, remain);
  } else {
    // In over-writable mode, fifo_write() is allowed even when fifo is full. In such case,
    // oldest data in fifo i.e. at read pointer data will be overwritten
    // Note: we can modify read buffer contents however we must not modify the read index itself within a write
    // function! Since it would end up in a race condition with read functions!
    if (n >= f->depth) {
      // Only copy last part
      if (access_mode == NULL) {
        buf8 += (n - f->depth);
      } else {
        // TODO should read from hw fifo to discard data, however reading an odd number could
        // accidentally discard data.
      }

      n = f->depth;

      // We start writing at the read pointer's position since we fill the whole buffer
      wr_idx = rd_idx;
    } else {
      const uint16_t overflowable_count = tu_ff_overflow_count(f->depth, wr_idx, rd_idx);
      if (overflowable_count + n >= 2 * f->depth) {
        // Double overflowed
        // Index is bigger than the allowed range [0,2*depth)
        // re-position write index to have a full fifo after pushed
        wr_idx = advance_index(f->depth, rd_idx, f->depth - n);

        // TODO we should also shift out n bytes from read index since we avoid changing rd index !!
        // However memmove() is expensive due to actual copying + wrapping consideration.
        // Also race condition could happen anyway if read() is invoke while moving result in corrupted memory
        // currently deliberately not implemented --> result in incorrect data read back
      } else {
        // normal + single overflowed:
        // Index is in the range of [0,2*depth) and thus detect and recoverable. Recovering is handled in read()
        // Therefore we just increase write index
        // we will correct (re-position) read index later on in fifo_read() function
      }
    }
  }

  if (n) {
    const uint16_t wr_ptr = idx2ptr(f->depth, wr_idx);
    TU_LOG(TU_FIFO_DBG, "actual_n = %u, wr_ptr = %u", n, wr_ptr);

#if CFG_TUSB_FIFO_HWFIFO_API
    if (access_mode != NULL) {
      hwff_push_n(f, buf8, n, wr_ptr, access_mode);
    } else
#endif
    {
      ff_push_n(f, buf8, n, wr_ptr);
    }
    f->wr_idx = advance_index(f->depth, wr_idx, n);

    TU_LOG(TU_FIFO_DBG, "\tnew_wr = %u\r\n", f->wr_idx);
  }

  ff_unlock(f->mutex_wr);

  return n;
}

uint16_t tu_fifo_discard_n(tu_fifo_t *f, uint16_t n) {
  const uint16_t count = tu_min16(n, tu_fifo_count(f)); // limit to available count
  ff_lock(f->mutex_rd);
  f->rd_idx = advance_index(f->depth, f->rd_idx, count);
  ff_unlock(f->mutex_rd);

  return count;
}

//--------------------------------------------------------------------+
// One API
//--------------------------------------------------------------------+

// peek() using local write/read index, correct read index if overflowed
// Be careful, caller must not lock mutex, since this Will also try to lock mutex
static bool ff_peek_local(tu_fifo_t *f, void *buf, uint16_t wr_idx, uint16_t rd_idx) {
  const uint16_t ovf_count = tu_ff_overflow_count(f->depth, wr_idx, rd_idx);
  if (ovf_count == 0) {
    return false; // nothing to peek
  }

  // Correct read index if overflow
  if (ovf_count > f->depth) {
    ff_lock(f->mutex_rd);
    rd_idx = correct_read_index(f, wr_idx);
    ff_unlock(f->mutex_rd);
  }

  const uint16_t rd_ptr = idx2ptr(f->depth, rd_idx);
  memcpy(buf, f->buffer + rd_ptr, 1);

  return true;
}

// Read one element out of the buffer, correct read index if overflowed
bool tu_fifo_read(tu_fifo_t *f, void *buffer) {
  // Peek the data
  // f->rd_idx might get modified in case of an overflow so we can not use a local variable
  const uint16_t wr_idx = f->wr_idx;
  const bool ret = ff_peek_local(f, buffer, wr_idx, f->rd_idx);
  if (ret) {
    ff_lock(f->mutex_rd);
    f->rd_idx = advance_index(f->depth, f->rd_idx, 1);
    ff_unlock(f->mutex_rd);
  }

  return ret;
}

// Read one item without removing it from the FIFO, correct read index if overflowed
bool tu_fifo_peek(tu_fifo_t *f, void *p_buffer) {
  const uint16_t wr_idx = f->wr_idx;
  const uint16_t rd_idx = f->rd_idx;
  return ff_peek_local(f, p_buffer, wr_idx, rd_idx);
}

// Write one element into the buffer
bool tu_fifo_write(tu_fifo_t *f, const void *data) {
  bool ret;
  ff_lock(f->mutex_wr);

  const uint16_t wr_idx = f->wr_idx;

  if (tu_fifo_full(f) && !f->overwritable) {
    ret = false;
  } else {
    const uint16_t wr_ptr = idx2ptr(f->depth, wr_idx);
    memcpy(f->buffer + wr_ptr, data, 1);
    f->wr_idx = advance_index(f->depth, wr_idx, 1);
    ret       = true;
  }

  ff_unlock(f->mutex_wr);

  return ret;
}

//--------------------------------------------------------------------+
// Index API
//--------------------------------------------------------------------+

/******************************************************************************/
/*!
    @brief Advance write pointer - intended to be used in combination with DMA.
    It is possible to fill the FIFO by use of a DMA in circular mode. Within
    DMA ISRs you may update the write pointer to be able to read from the FIFO.
    As long as the DMA is the only process writing into the FIFO this is safe
    to use.

    USE WITH CARE - WE DO NOT CONDUCT SAFETY CHECKS HERE!

    @param[in]  f
                Pointer to the FIFO buffer to manipulate
    @param[in]  n
                Number of items the write pointer moves forward
 */
/******************************************************************************/
void tu_fifo_advance_write_pointer(tu_fifo_t *f, uint16_t n) {
  f->wr_idx = advance_index(f->depth, f->wr_idx, n);
}

// Correct the read index in case tu_fifo_overflow() returned true!
void tu_fifo_correct_read_pointer(tu_fifo_t *f) {
  ff_lock(f->mutex_rd);
  correct_read_index(f, f->wr_idx);
  ff_unlock(f->mutex_rd);
}

/******************************************************************************/
/*!
    @brief Advance read pointer - intended to be used in combination with DMA.
    It is possible to read from the FIFO by use of a DMA in linear mode. Within
    DMA ISRs you may update the read pointer to be able to again write into the
    FIFO. As long as the DMA is the only process reading from the FIFO this is
    safe to use.

    USE WITH CARE - WE DO NOT CONDUCT SAFETY CHECKS HERE!

    @param[in]  f
                Pointer to the FIFO buffer to manipulate
    @param[in]  n
                Number of items the read pointer moves forward
 */
/******************************************************************************/
void tu_fifo_advance_read_pointer(tu_fifo_t *f, uint16_t n) {
  f->rd_idx = advance_index(f->depth, f->rd_idx, n);
}

/******************************************************************************/
/*!
   @brief Get read info

   Returns the length and pointer from which bytes can be read in a linear manner.
   This is of major interest for DMA transmissions. If returned length is zero the
   corresponding pointer is invalid.
   The read pointer does NOT get advanced, use tu_fifo_advance_read_pointer() to
   do so!
   @param[in]       f
                    Pointer to FIFO
   @param[out]      *info
                    Pointer to struct which holds the desired infos
 */
/******************************************************************************/
void tu_fifo_get_read_info(tu_fifo_t *f, tu_fifo_buffer_info_t *info) {
  // Operate on temporary values in case they change in between
  uint16_t wr_idx = f->wr_idx;
  uint16_t rd_idx = f->rd_idx;

  uint16_t cnt = tu_ff_overflow_count(f->depth, wr_idx, rd_idx);

  // Check overflow and correct if required - may happen in case a DMA wrote too fast
  if (cnt > f->depth) {
    ff_lock(f->mutex_rd);
    rd_idx = correct_read_index(f, wr_idx);
    ff_unlock(f->mutex_rd);

    cnt = f->depth;
  }

  // Check if fifo is empty
  if (cnt == 0) {
    info->linear.len  = 0;
    info->wrapped.len = 0;
    info->linear.ptr  = NULL;
    info->wrapped.ptr = NULL;
    return;
  }

  // Get relative pointers
  uint16_t wr_ptr = idx2ptr(f->depth, wr_idx);
  uint16_t rd_ptr = idx2ptr(f->depth, rd_idx);

  // Copy pointer to buffer to start reading from
  info->linear.ptr = &f->buffer[rd_ptr];

  // Check if there is a wrap around necessary
  if (wr_ptr > rd_ptr) {
    // Non wrapping case
    info->linear.len = cnt;

    info->wrapped.len = 0;
    info->wrapped.ptr = NULL;
  } else {
    info->linear.len = f->depth - rd_ptr; // Also the case if FIFO was full

    info->wrapped.len = cnt - info->linear.len;
    info->wrapped.ptr = f->buffer;
  }
}

/******************************************************************************/
/*!
   @brief Get linear write info

   Returns the length and pointer to which bytes can be written into FIFO in a linear manner.
   This is of major interest for DMA transmissions not using circular mode. If a returned length is zero the
   corresponding pointer is invalid. The returned lengths summed up are the currently free space in the FIFO.
   The write pointer does NOT get advanced, use tu_fifo_advance_write_pointer() to do so!
   TAKE CARE TO NOT OVERFLOW THE BUFFER MORE THAN TWO TIMES THE FIFO DEPTH - IT CAN NOT RECOVERE OTHERWISE!
   @param[in]       f
                    Pointer to FIFO
   @param[out]      *info
                    Pointer to struct which holds the desired infos
 */
/******************************************************************************/
void tu_fifo_get_write_info(tu_fifo_t *f, tu_fifo_buffer_info_t *info) {
  uint16_t wr_idx = f->wr_idx;
  uint16_t rd_idx = f->rd_idx;
  uint16_t remain = tu_ff_remaining_local(f->depth, wr_idx, rd_idx);

  if (remain == 0) {
    info->linear.len  = 0;
    info->wrapped.len = 0;
    info->linear.ptr  = NULL;
    info->wrapped.ptr = NULL;
    return;
  }

  // Get relative pointers
  uint16_t wr_ptr = idx2ptr(f->depth, wr_idx);
  uint16_t rd_ptr = idx2ptr(f->depth, rd_idx);

  // Copy pointer to buffer to start writing to
  info->linear.ptr = &f->buffer[wr_ptr];

  if (wr_ptr < rd_ptr) {
    // Non wrapping case
    info->linear.len  = rd_ptr - wr_ptr;
    info->wrapped.len = 0;
    info->wrapped.ptr = NULL;
  } else {
    info->linear.len  = f->depth - wr_ptr;
    info->wrapped.len = remain - info->linear.len; // Remaining length - n already was limited to remain or FIFO depth
    info->wrapped.ptr = f->buffer;                 // Always start of buffer
  }
}
