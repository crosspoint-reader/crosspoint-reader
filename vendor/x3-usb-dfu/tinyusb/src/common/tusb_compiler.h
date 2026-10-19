/*
 * SPDX-FileCopyrightText: Copyright (c) 2019 Ha Thach (tinyusb.org)
 * SPDX-License-Identifier: MIT
 *
 * This file is part of the TinyUSB stack.
 */

#pragma once

#define TU_TOKEN(x)           x
#define TU_STRING(x)          #x                  ///< stringify without expand
#define TU_XSTRING(x)         TU_STRING(x)        ///< expand then stringify
#define TU_STRCAT(a, b)       a##b                ///< concat without expand
#define TU_STRCAT3(a, b, c)   a##b##c             ///< concat without expand
#define TU_XSTRCAT(a, b)      TU_STRCAT(a, b)     ///< expand then concat
#define TU_XSTRCAT3(a, b, c)  TU_STRCAT3(a, b, c) ///< expand then concat 3 tokens

#define TU_INCLUDE_PATH(_dir,_file) TU_XSTRING( TU_TOKEN(_dir)TU_TOKEN(_file) )

#if defined __COUNTER__ && __COUNTER__ != __COUNTER__
  #define TU_COUNTER __COUNTER__
#else
  #define TU_COUNTER __LINE__
#endif

// Compile-time Assert
#if defined (__cplusplus) && __cplusplus >= 201103L
  #define TU_VERIFY_STATIC   static_assert
#elif defined (__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
  #define TU_VERIFY_STATIC   _Static_assert
#elif defined(__CCRX__)
  #define TU_VERIFY_STATIC(const_expr, _mess) typedef char TU_XSTRCAT(_verify_static_, TU_COUNTER)[(const_expr) ? 1 : 0];
#else
  #define TU_VERIFY_STATIC(const_expr, _mess) enum { TU_XSTRCAT(_verify_static_, TU_COUNTER) = 1/(!!(const_expr)) }
#endif

/* --------------------- Fuzzing types -------------------------------------- */
#ifdef _FUZZ
  #define tu_static static __thread
#else
  #define tu_static static
#endif

// for declaration of reserved field, make use of TU_COUNTER
#define TU_RESERVED           TU_XSTRCAT(reserved, TU_COUNTER)

#define TU_LITTLE_ENDIAN (0x12u)
#define TU_BIG_ENDIAN (0x21u)

#define TU_BITFIELD_LE (0x34u)
#define TU_BITFIELD_BE (0x43u)

/*------------------------------------------------------------------*/
/* Count number of arguments of __VA_ARGS__
 * - reference www.stackoverflow.com/questions/2124339/c-preprocessor-va-args-number-of-arguments
 * - TU_GET_NTH_ARG() takes args >= N (64) but only expand to Nth one (64th)
 * - TU_NARG_RSEQ_N() is reverse sequential to N to add padding to have
 * Nth position is the same as the number of arguments
 * - ##__VA_ARGS__ is used to deal with 0 paramerter (swallows comma)
 *------------------------------------------------------------------*/
#if defined(__CCRX__)
#define TU_ARGS_NUM(...)   TU_NARG_IMPL(_0, __VA_ARGS__, TU_NARG_RSEQ_N())
#else
#define TU_ARGS_NUM(...)   TU_NARG_IMPL(_0, ##__VA_ARGS__, TU_NARG_RSEQ_N())
#endif

#define TU_NARG_IMPL(...)      TU_GET_NTH_ARG(__VA_ARGS__)
#define TU_GET_NTH_ARG( \
          _1, _2, _3, _4, _5, _6, _7, _8, _9,_10, \
         _11,_12,_13,_14,_15,_16,_17,_18,_19,_20, \
         _21,_22,_23,_24,_25,_26,_27,_28,_29,_30, \
         _31,_32,_33,_34,_35,_36,_37,_38,_39,_40, \
         _41,_42,_43,_44,_45,_46,_47,_48,_49,_50, \
         _51,_52,_53,_54,_55,_56,_57,_58,_59,_60, \
         _61,_62,_63,N,...) N
#define TU_NARG_RSEQ_N() \
         62,61,60,                      \
         59,58,57,56,55,54,53,52,51,50, \
         49,48,47,46,45,44,43,42,41,40, \
         39,38,37,36,35,34,33,32,31,30, \
         29,28,27,26,25,24,23,22,21,20, \
         19,18,17,16,15,14,13,12,11,10, \
         9,8,7,6,5,4,3,2,1,0

// Apply a macro X to each of the arguments with a separation/delimiter
#define TU_ARGS_APPLY(_X, _s, ...)   TU_XSTRCAT(TU_ARGS_APPLY_, TU_ARGS_NUM(__VA_ARGS__))(_X, _s, __VA_ARGS__)

#define TU_ARGS_APPLY_1(_X, _s, _a1)                                    _X(_a1)
#define TU_ARGS_APPLY_2(_X, _s, _a1, _a2)                               _X(_a1) _s _X(_a2)
#define TU_ARGS_APPLY_3(_X, _s, _a1, _a2, _a3)                          _X(_a1) _s TU_ARGS_APPLY_2(_X, _s, _a2, _a3)
#define TU_ARGS_APPLY_4(_X, _s, _a1, _a2, _a3, _a4)                     _X(_a1) _s TU_ARGS_APPLY_3(_X, _s, _a2, _a3, _a4)
#define TU_ARGS_APPLY_5(_X, _s, _a1, _a2, _a3, _a4, _a5)                _X(_a1) _s TU_ARGS_APPLY_4(_X, _s, _a2, _a3, _a4, _a5)
#define TU_ARGS_APPLY_6(_X, _s, _a1, _a2, _a3, _a4, _a5, _a6)           _X(_a1) _s TU_ARGS_APPLY_5(_X, _s, _a2, _a3, _a4, _a5, _a6)
#define TU_ARGS_APPLY_7(_X, _s, _a1, _a2, _a3, _a4, _a5, _a6, _a7)      _X(_a1) _s TU_ARGS_APPLY_6(_X, _s, _a2, _a3, _a4, _a5, _a6, _a7)
#define TU_ARGS_APPLY_8(_X, _s, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8) _X(_a1) _s TU_ARGS_APPLY_7(_X, _s, _a2, _a3, _a4, _a5, _a6, _a7, _a8)

// Apply a macro X to each of the arguments and expand the result with comma
#define TU_ARGS_APPLY_EXPAND(_X, ...)   TU_XSTRCAT(TU_ARGS_APPLY_EXPAND_, TU_ARGS_NUM(__VA_ARGS__))(_X, __VA_ARGS__)

#define TU_ARGS_APPLY_EXPAND_1(_X, _a1)                                    _X(_a1)
#define TU_ARGS_APPLY_EXPAND_2(_X, _a1, _a2)                               _X(_a1), _X(_a2)
#define TU_ARGS_APPLY_EXPAND_3(_X, _a1, _a2, _a3)                          _X(_a1), TU_ARGS_APPLY_EXPAND_2(_X, _a2, _a3)
#define TU_ARGS_APPLY_EXPAND_4(_X, _a1, _a2, _a3, _a4)                     _X(_a1), TU_ARGS_APPLY_EXPAND_3(_X, _a2, _a3, _a4)
#define TU_ARGS_APPLY_EXPAND_5(_X, _a1, _a2, _a3, _a4, _a5)                _X(_a1), TU_ARGS_APPLY_EXPAND_4(_X, _a2, _a3, _a4, _a5)
#define TU_ARGS_APPLY_EXPAND_6(_X, _a1, _a2, _a3, _a4, _a5, _a6)           _X(_a1), TU_ARGS_APPLY_EXPAND_5(_X, _a2, _a3, _a4, _a5, _a6)
#define TU_ARGS_APPLY_EXPAND_7(_X, _a1, _a2, _a3, _a4, _a5, _a6, _a7)      _X(_a1), TU_ARGS_APPLY_EXPAND_6(_X, _a2, _a3, _a4, _a5, _a6, _a7)
#define TU_ARGS_APPLY_EXPAND_8(_X, _a1, _a2, _a3, _a4, _a5, _a6, _a7, _a8) _X(_a1), TU_ARGS_APPLY_EXPAND_7(_X, _a2, _a3, _a4, _a5, _a6, _a7, _a8)

//--------------------------------------------------------------------+
// Macro for function default arguments
//--------------------------------------------------------------------+
#define TU_GET_3RD_ARG(arg1, arg2, arg3, ...)        arg3

// function expand with number of arguments
#define TU_FUNC_OPTIONAL_ARG(func, ...)   TU_XSTRCAT(func##_arg, TU_ARGS_NUM(__VA_ARGS__))(__VA_ARGS__)

//--------------------------------------------------------------------+
// Compiler Attribute Abstraction
//--------------------------------------------------------------------+
#if defined(__GNUC__) || defined(__ICCARM__) || defined(__TI_COMPILER_VERSION__)
  #if defined(__ICCARM__)
    #include <intrinsics.h> // for builtin functions
  #endif

  #define TU_ATTR_ALIGNED(Bytes)    __attribute__((aligned(Bytes)))
  #define TU_ATTR_SECTION(sec_name) __attribute__((section(#sec_name)))
  #define TU_ATTR_PACKED            __attribute__((packed))
  #define TU_ATTR_WEAK              __attribute__((weak))
// #define TU_ATTR_WEAK_ALIAS(f)         __attribute__ ((weak, alias(#f)))
  #ifndef TU_ATTR_ALWAYS_INLINE                                            // allow to override for debug
    #define TU_ATTR_ALWAYS_INLINE __attribute__((always_inline))
  #endif
  #define TU_ATTR_DEPRECATED(mess)      __attribute__ ((deprecated(mess))) // warn if function with this attribute is used
  #define TU_ATTR_UNUSED                __attribute__ ((unused))           // Function/Variable is meant to be possibly unused
  #define TU_ATTR_USED                  __attribute__ ((used))             // Function/Variable is meant to be used

  #define TU_ATTR_PACKED_BEGIN
  #define TU_ATTR_PACKED_END
  #define TU_ATTR_BIT_FIELD_ORDER_BEGIN
  #define TU_ATTR_BIT_FIELD_ORDER_END

  #if (defined(__has_attribute) && __has_attribute(__fallthrough__)) || defined(__TI_COMPILER_VERSION__)
    #define TU_ATTR_FALLTHROUGH __attribute__((fallthrough))
  #else
    #define TU_ATTR_FALLTHROUGH     \
      do {                          \
      } while (0) /* fallthrough */
  #endif

// Endian conversion use well-known host to network (big endian) naming
// For TI ARM compiler, __BYTE_ORDER__ is not defined for MSP430 but still LE
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ || defined(__MSP430__)
    #define TU_BYTE_ORDER TU_LITTLE_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_LE
  #else
    #define TU_BYTE_ORDER TU_BIG_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_BE
  #endif

  // Unfortunately XC16 doesn't provide builtins for 32bit endian conversion
  #if defined(__XC16)
    #define TU_BSWAP16(u16) (__builtin_swap(u16))
    #define TU_BSWAP32(u32) ((((u32) & 0xff000000) >> 24) |  \
                            (((u32) & 0x00ff0000) >> 8)  |  \
                            (((u32) & 0x0000ff00) << 8)  |  \
                            (((u32) & 0x000000ff) << 24))
  #else
    #define TU_BSWAP16(u16) (__builtin_bswap16(u16))
    #define TU_BSWAP32(u32) (__builtin_bswap32(u32))
  #endif

  // List of obsolete callback function that is renamed and should not be defined.
  // Put it here since only gcc support this pragma
  #if !defined(__ARMCC_VERSION) && !defined(__ICCARM__)
    #pragma GCC poison tud_vendor_control_request_cb
  #endif

#elif defined(__ICCARM__)
  #include <intrinsics.h>
  #define TU_ATTR_ALIGNED(Bytes)        __attribute__ ((aligned(Bytes)))
  #define TU_ATTR_SECTION(sec_name)     __attribute__ ((section(#sec_name)))
  #define TU_ATTR_PACKED                __attribute__ ((packed))
  #define TU_ATTR_WEAK                  __attribute__ ((weak))
  // #define TU_ATTR_WEAK_ALIAS(f)         __attribute__ ((weak, alias(#f)))
  #ifndef TU_ATTR_ALWAYS_INLINE // allow to override for debug
    #define TU_ATTR_ALWAYS_INLINE         __attribute__ ((always_inline))
  #endif
  #define TU_ATTR_DEPRECATED(mess)      __attribute__ ((deprecated(mess))) // warn if function with this attribute is used
  #define TU_ATTR_UNUSED                __attribute__ ((unused))           // Function/Variable is meant to be possibly unused
  #define TU_ATTR_USED                  __attribute__ ((used))             // Function/Variable is meant to be used
  #define TU_ATTR_FALLTHROUGH           do {} while (0)  /* fallthrough */

  #define TU_ATTR_PACKED_BEGIN
  #define TU_ATTR_PACKED_END
  #define TU_ATTR_BIT_FIELD_ORDER_BEGIN
  #define TU_ATTR_BIT_FIELD_ORDER_END

  // Endian conversion use well-known host to network (big endian) naming
  #if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    #define TU_BYTE_ORDER TU_LITTLE_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_LE
  #else
    #define TU_BYTE_ORDER TU_BIG_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_BE
  #endif

  #define TU_BSWAP16(u16) (__iar_builtin_REV16(u16))
  #define TU_BSWAP32(u32) (__iar_builtin_REV(u32))

#elif defined(__CCRX__)
  #define TU_ATTR_ALIGNED(Bytes)
  #define TU_ATTR_SECTION(sec_name)
  #define TU_ATTR_PACKED
  #define TU_ATTR_WEAK
  // #define TU_ATTR_WEAK_ALIAS(f)
  #define TU_ATTR_ALWAYS_INLINE
  #define TU_ATTR_DEPRECATED(mess)
  #define TU_ATTR_UNUSED
  #define TU_ATTR_USED
  #define TU_ATTR_FALLTHROUGH           do {} while (0)  /* fallthrough */

  #define TU_ATTR_PACKED_BEGIN          _Pragma("pack")
  #define TU_ATTR_PACKED_END            _Pragma("packoption")
  #define TU_ATTR_BIT_FIELD_ORDER_BEGIN _Pragma("bit_order right")
  #define TU_ATTR_BIT_FIELD_ORDER_END   _Pragma("bit_order")

  // Endian conversion use well-known host to network (big endian) naming
  #if defined(__LIT)
    #define TU_BYTE_ORDER TU_LITTLE_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_LE
  #else
    #define TU_BYTE_ORDER TU_BIG_ENDIAN
    #define TU_BITFIELD_ORDER TU_BITFIELD_BE
  #endif

  #define TU_BSWAP16(u16) ((unsigned short)_builtin_revw((unsigned long)u16))
  #define TU_BSWAP32(u32) (_builtin_revl(u32))

#else
  #error "Compiler attribute porting is required"
#endif


#if (TU_BYTE_ORDER == TU_LITTLE_ENDIAN)

  #define tu_htons(u16)  (TU_BSWAP16(u16))
  #define tu_ntohs(u16)  (TU_BSWAP16(u16))

  #define tu_htonl(u32)  (TU_BSWAP32(u32))
  #define tu_ntohl(u32)  (TU_BSWAP32(u32))

  #define tu_htole16(u16) (u16)
  #define tu_le16toh(u16) (u16)

  #define tu_htole32(u32) (u32)
  #define tu_le32toh(u32) (u32)

#elif (TU_BYTE_ORDER == TU_BIG_ENDIAN)

  #define tu_htons(u16)  (u16)
  #define tu_ntohs(u16)  (u16)

  #define tu_htonl(u32)  (u32)
  #define tu_ntohl(u32)  (u32)

  #define tu_htole16(u16) (TU_BSWAP16(u16))
  #define tu_le16toh(u16) (TU_BSWAP16(u16))

  #define tu_htole32(u32) (TU_BSWAP32(u32))
  #define tu_le32toh(u32) (TU_BSWAP32(u32))

#else
  #error Byte order is undefined
#endif
