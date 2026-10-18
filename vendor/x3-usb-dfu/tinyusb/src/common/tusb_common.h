/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#ifndef TUSB_COMMON_H_
#define TUSB_COMMON_H_

#ifdef __cplusplus
 extern "C" {
#endif

//--------------------------------------------------------------------+
// Macros Helper
//--------------------------------------------------------------------+
#define TU_ARRAY_SIZE(_arr)           ( sizeof(_arr) / sizeof(_arr[0]) )
#define TU_FIELD_SIZE(_type, _field)  (sizeof(((_type *)0)->_field))
#define TU_MIN(_x, _y)                ( ( (_x) < (_y) ) ? (_x) : (_y) )
#define TU_MAX(_x, _y)                ( ( (_x) > (_y) ) ? (_x) : (_y) )
#define TU_DIV_CEIL(n, d)             (((n) + (d) - 1) / (d))
#define TU_DIV_ROUND_NEAREST(v, d)    (((v) + (d)/2) / (d) ) // round to nearest integer

#define TU_U16(_high, _low)           ((uint16_t) ((((uint16_t) (_high)) << 8) | ((uint16_t) (_low))))
#define TU_U16_HIGH(_u16)             ((uint8_t) (((uint16_t) (_u16) >> 8) & 0x00ffu))
#define TU_U16_LOW(_u16)              ((uint8_t) ((uint16_t) (_u16) & 0x00ffu))
#define U16_TO_U8S_BE(_u16)           TU_U16_HIGH(_u16), TU_U16_LOW(_u16)
#define U16_TO_U8S_LE(_u16)           TU_U16_LOW(_u16), TU_U16_HIGH(_u16)

#define TU_U24(_high, _mid, _low)     ((uint32_t) ((((uint32_t) (_high)) << 16) | (((uint32_t) (_mid)) << 8) | ((uint32_t) (_low))))
#define TU_U24_HIGH(_u24)             ((uint8_t) (((uint32_t) (_u24) >> 16) & 0x0000ffu))
#define TU_U24_MID(_u24)              ((uint8_t) (((uint32_t) (_u24) >>  8) & 0x0000ffu))
#define TU_U24_LOW(_u24)              ((uint8_t) ((uint32_t) (_u24) & 0x0000ffu))
#define U24_TO_U8S_BE(_u24)           TU_U24_HIGH(_u24), TU_U24_MID(_u24), TU_U24_LOW(_u24)
#define U24_TO_U8S_LE(_u24)           TU_U24_LOW(_u24), TU_U24_MID(_u24), TU_U24_HIGH(_u24)

#define TU_U32_BYTE3(_u32)            ((uint8_t) ((((uint32_t) _u32) >> 24) & 0x000000ff)) // MSB
#define TU_U32_BYTE2(_u32)            ((uint8_t) ((((uint32_t) _u32) >> 16) & 0x000000ff))
#define TU_U32_BYTE1(_u32)            ((uint8_t) ((((uint32_t) _u32) >>  8) & 0x000000ff))
#define TU_U32_BYTE0(_u32)            ((uint8_t) (((uint32_t)  _u32)        & 0x000000ff)) // LSB

#define U32_TO_U8S_BE(_u32)           TU_U32_BYTE3(_u32), TU_U32_BYTE2(_u32), TU_U32_BYTE1(_u32), TU_U32_BYTE0(_u32)
#define U32_TO_U8S_LE(_u32)           TU_U32_BYTE0(_u32), TU_U32_BYTE1(_u32), TU_U32_BYTE2(_u32), TU_U32_BYTE3(_u32)

#define TU_BIT(n)                     (1UL << (n))

// Generate a mask with bit from high (31) to low (0) set, e.g TU_GENMASK(3, 0) = 0b1111
#define TU_GENMASK(h, l)              ( (UINT32_MAX << (l)) & (UINT32_MAX >> (31 - (h))) )

//--------------------------------------------------------------------+
// Includes
//--------------------------------------------------------------------+

// Standard Headers
#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stddef.h>
#include <string.h>

// Tinyusb Common Headers
#include "tusb_option.h"
#include "tusb_compiler.h"
#include "tusb_verify.h"
#include "tusb_types.h"
#include "tusb_debug.h"

//--------------------------------------------------------------------+
// API implemented by application if needed
// TODO move to a more obvious place/file
//--------------------------------------------------------------------+

// Get current milliseconds, required by some port/configuration without RTOS
extern uint32_t tusb_time_millis_api(void);

// Delay in milliseconds, use tusb_time_millis_api() by default. required by some port/configuration with no RTOS
extern void tusb_time_delay_ms_api(uint32_t ms);

// flush data cache
extern void tusb_app_dcache_flush(uintptr_t addr, uint32_t data_size);

// invalidate data cache
extern void tusb_app_dcache_invalidate(uintptr_t addr, uint32_t data_size);

// Optional physical <-> virtual address translation
extern void* tusb_app_virt_to_phys(void *virt_addr);
extern void* tusb_app_phys_to_virt(void *phys_addr);

//--------------------------------------------------------------------+
// Internal Inline Functions
//--------------------------------------------------------------------+

//------------- Mem -------------//
#define tu_memclr(buffer, size)  (void) memset((buffer), 0, (size))
#define tu_varclr(_var)          tu_memclr(_var, sizeof(*(_var)))

// This is a backport of memset_s from c11
TU_ATTR_ALWAYS_INLINE static inline int tu_memset_s(void *dest, size_t destsz, int ch, size_t count) {
  // Validate parameters
  if (dest == NULL) {
    return -1;
  }

  if (count == 0u) {
    return 0;
  }

  if (count > destsz) {
    return -1;
  }

  (void) memset(dest, ch, count);
  return 0;
}

// This is a backport of memcpy_s from c11
TU_ATTR_ALWAYS_INLINE static inline int tu_memcpy_s(void *dest, size_t destsz, const void *src, size_t count) {
  if (dest == NULL) {
    return -1;
  }

  if (count == 0u) {
    return 0;
  }

  if (src == NULL) {
    return -1;
  }

  if (count > destsz) {
    return -1;
  }

  (void) memcpy(dest, src, count);
  return 0;
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_mem_is_zero(const void *buffer, size_t size) {
  const uint8_t* buf8 = (const uint8_t*) buffer;
  for (size_t i = 0; i < size; i++) {
    if (buf8[i] != 0) { return false; }
  }
  return true;
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_mem_is_ff(const void *buffer, size_t size) {
  const uint8_t* buf8 = (const uint8_t*) buffer;
  for (size_t i = 0; i < size; i++) {
    if (buf8[i] != 0xff) { return false; }
  }
  return true;
}


//------------- Bytes -------------//
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_u32(uint8_t b3, uint8_t b2, uint8_t b1, uint8_t b0) {
  return (((uint32_t)b3) << 24) | (((uint32_t)b2) << 16) | (((uint32_t)b1) << 8) | b0;
}

TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_u32_from_u16(uint16_t high, uint16_t low) {
  return (((uint32_t)high) << 16) | low;
}

TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_u16(uint8_t high, uint8_t low) {
  return (uint16_t)((((uint16_t)high) << 8) | low);
}

TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u32_byte3(uint32_t ui32) { return TU_U32_BYTE3(ui32); }
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u32_byte2(uint32_t ui32) { return TU_U32_BYTE2(ui32); }
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u32_byte1(uint32_t ui32) { return TU_U32_BYTE1(ui32); }
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u32_byte0(uint32_t ui32) { return TU_U32_BYTE0(ui32); }

TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_u32_high16(uint32_t ui32) { return (uint16_t) (ui32 >> 16); }
TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_u32_low16 (uint32_t ui32) { return (uint16_t) (ui32 & 0x0000ffffu); }

TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u16_high(uint16_t ui16) { return TU_U16_HIGH(ui16); }
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_u16_low (uint16_t ui16) { return TU_U16_LOW(ui16); }

//------------- Bits -------------//
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_bit_set  (uint32_t value, uint8_t pos) { return value | TU_BIT(pos); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_bit_clear(uint32_t value, uint8_t pos) { return value & (~TU_BIT(pos)); }
TU_ATTR_ALWAYS_INLINE static inline bool     tu_bit_test (uint32_t value, uint8_t pos) { return (value & TU_BIT(pos)) ? true : false; }

//------------- Min -------------//
TU_ATTR_ALWAYS_INLINE static inline uint8_t  tu_min8  (uint8_t  x, uint8_t y ) { return (x < y) ? x : y; }
TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_min16 (uint16_t x, uint16_t y) { return (x < y) ? x : y; }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_min32 (uint32_t x, uint32_t y) { return (x < y) ? x : y; }

//------------- Max -------------//
TU_ATTR_ALWAYS_INLINE static inline uint8_t  tu_max8  (uint8_t  x, uint8_t y ) { return (x > y) ? x : y; }
TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_max16 (uint16_t x, uint16_t y) { return (x > y) ? x : y; }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_max32 (uint32_t x, uint32_t y) { return (x > y) ? x : y; }

//------------- Align -------------//
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align(uint32_t value, uint32_t alignment) {
  return value & ((uint32_t) ~(alignment-1));
}

TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align4  (uint32_t value) { return (value & 0xFFFFFFFCUL); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align8  (uint32_t value) { return (value & 0xFFFFFFF8UL); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align16 (uint32_t value) { return (value & 0xFFFFFFF0UL); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align32 (uint32_t value) { return (value & 0xFFFFFFE0UL); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_align4k (uint32_t value) { return (value & 0xFFFFF000UL); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_offset4k(uint32_t value) { return (value & 0xFFFUL); }

TU_ATTR_ALWAYS_INLINE static inline bool tu_is_aligned32(uint32_t value) { return (value & 0x1FUL) == 0; }
TU_ATTR_ALWAYS_INLINE static inline bool tu_is_aligned64(uint64_t value) { return (value & 0x3FUL) == 0; }

//------------- Mathematics -------------//
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_div_ceil(uint32_t v, uint32_t d) { return TU_DIV_CEIL(v, d); }
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_div_round_nearest(uint32_t v, uint32_t d) { return TU_DIV_ROUND_NEAREST(v, d); }

TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_round_up(uint32_t v, uint32_t f) { return tu_div_ceil(v, f) * f; }

// log2 of a value is its MSB's position
// TODO use clz TODO remove
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_log2(uint32_t value) {
  uint8_t result = 0;
  while ((value >>= 1u) != 0u) {
    result++;
  }
  return result;
}

//static inline uint8_t tu_log2(uint32_t value)
//{
//   return sizeof(uint32_t) * CHAR_BIT - __builtin_clz(x) - 1;
//}

TU_ATTR_ALWAYS_INLINE static inline bool tu_is_power_of_two(uint32_t value) {
   return (value != 0) && ((value & (value - 1)) == 0);
}

//------------- Unaligned Access -------------//
#if TUP_ARCH_STRICT_ALIGN

// Rely on compiler to generate correct code for unaligned access
typedef struct { uint16_t val; } TU_ATTR_PACKED tu_unaligned_uint16_t;
typedef struct { uint32_t val; } TU_ATTR_PACKED tu_unaligned_uint32_t;

TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_unaligned_read32(const void *mem) {
  tu_unaligned_uint32_t const *ua32 = (tu_unaligned_uint32_t const *) mem;
  return ua32->val;
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write32(void *mem, uint32_t value) {
  tu_unaligned_uint32_t *ua32 = (tu_unaligned_uint32_t *) mem;
  ua32->val = value;
}

TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_unaligned_read16(const void *mem) {
  tu_unaligned_uint16_t const *ua16 = (tu_unaligned_uint16_t const *) mem;
  return ua16->val;
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write16(void *mem, uint16_t value) {
  tu_unaligned_uint16_t *ua16 = (tu_unaligned_uint16_t *) mem;
  ua16->val = value;
}

#elif TUP_MCU_STRICT_ALIGN

// MCU such as LPC_IP3511 Highspeed cannot access unaligned memory on USB_RAM although it is ARM M4.
// We have to manually pick up bytes since tu_unaligned_uint32_t will still generate unaligned code
// NOTE: volatile cast to memory to prevent compiler to optimize and generate unaligned code
// TODO Big Endian may need minor changes
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_unaligned_read32(const void* mem) {
  volatile uint8_t const* buf8 = (uint8_t const*) mem;
  return tu_u32(buf8[3], buf8[2], buf8[1], buf8[0]);
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write32(void* mem, uint32_t value) {
  volatile uint8_t* buf8 = (uint8_t*) mem;
  buf8[0] = tu_u32_byte0(value);
  buf8[1] = tu_u32_byte1(value);
  buf8[2] = tu_u32_byte2(value);
  buf8[3] = tu_u32_byte3(value);
}

TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_unaligned_read16(const void* mem) {
  volatile uint8_t const* buf8 = (uint8_t const*) mem;
  return tu_u16(buf8[1], buf8[0]);
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write16(void* mem, uint16_t value) {
  volatile uint8_t* buf8 = (uint8_t*) mem;
  buf8[0] = tu_u16_low(value);
  buf8[1] = tu_u16_high(value);
}

#else

// MCU that could access unaligned memory natively
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_unaligned_read32(const void *mem) {
  return *((uint32_t const *) mem);
}

TU_ATTR_ALWAYS_INLINE static inline uint16_t tu_unaligned_read16(const void *mem) {
  return *((uint16_t const *) mem);
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write32(void *mem, uint32_t value) {
  *((uint32_t *) mem) = value;
}

TU_ATTR_ALWAYS_INLINE static inline void tu_unaligned_write16(void *mem, uint16_t value) {
  *((uint16_t *) mem) = value;
}

#endif

// scatter read 4 bytes from two buffers (LE). Parameter are not checked
TU_ATTR_ALWAYS_INLINE static inline uint32_t tu_scatter_read32(const uint8_t *buf1, uint8_t len1, const uint8_t *buf2,
                                                               uint8_t len2) {
  uint32_t result = 0;
  uint8_t  shift  = 0;

  for (uint8_t i = 0; i < len1; ++i) {
    result |= ((uint32_t)buf1[i]) << shift;
    shift += 8;
  }

  for (uint8_t i = 0; i < len2; ++i) {
    result |= ((uint32_t)buf2[i]) << shift;
    shift += 8;
  }

  return result;
}

// scatter write 4 bytes (LE) to two buffers. Parameter are not checked
TU_ATTR_ALWAYS_INLINE static inline void tu_scatter_write32(uint32_t value, uint8_t *buf1, uint8_t len1,
                                                            uint8_t *buf2, uint8_t len2) {
  for (uint8_t i = 0; i < len1; ++i) {
    buf1[i] = (uint8_t)(value & 0xFF);
    value >>= 8;
  }

  for (uint8_t i = 0; i < len2; ++i) {
    buf2[i] = (uint8_t)(value & 0xFF);
    value >>= 8;
  }
}

//--------------------------------------------------------------------+
// Descriptor helper
//--------------------------------------------------------------------+

// return next descriptor
TU_ATTR_ALWAYS_INLINE static inline uint8_t const * tu_desc_next(void const* desc) {
  uint8_t const* desc8 = (uint8_t const*) desc;
  return desc8 + desc8[DESC_OFFSET_LEN];
}

// get descriptor length
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_desc_len(void const* desc) {
  return ((uint8_t const*) desc)[DESC_OFFSET_LEN];
}

// get descriptor type
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_desc_type(void const* desc) {
  return ((uint8_t const*) desc)[DESC_OFFSET_TYPE];
}

// get descriptor subtype
TU_ATTR_ALWAYS_INLINE static inline uint8_t tu_desc_subtype(void const* desc) {
  return ((uint8_t const*) desc)[DESC_OFFSET_SUBTYPE];
}

TU_ATTR_ALWAYS_INLINE static inline bool tu_desc_in_bounds(const uint8_t *p_desc, const uint8_t *desc_end) {
  return p_desc < desc_end && tu_desc_next(p_desc) <= desc_end;
}

// find descriptor that match byte1 (type)
uint8_t const * tu_desc_find(uint8_t const* desc, uint8_t const* end, uint8_t byte1);

// find descriptor that match byte1 (type) and byte2
uint8_t const * tu_desc_find2(uint8_t const* desc, uint8_t const* end, uint8_t byte1, uint8_t byte2);

// find descriptor that match byte1 (type) and byte2
uint8_t const * tu_desc_find3(uint8_t const* desc, uint8_t const* end, uint8_t byte1, uint8_t byte2, uint8_t byte3);

#ifdef __cplusplus
 }
#endif

#endif /* TUSB_COMMON_H_ */
