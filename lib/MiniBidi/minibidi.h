#ifndef MINIBIDI_H
#define MINIBIDI_H

/*
 * minibidi.h — standalone header for ESP32C3 Hebrew/English BiDi
 *
 * Derived from mintty/src/minibidi.c (Thomas Wolff, MIT licence).
 * Stripped of: Arabic shaping, box-drawing mirror, terminal dependencies,
 * GCC nested functions, VLAs, and non-Hebrew/English Unicode data.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ── Basic types ─────────────────────────────────────────────────────── */
typedef uint8_t uchar;
typedef uint32_t ucschar; /* Unicode codepoint; BMP-only content fits uint16_t
                             but uint32_t is safer and ESP32C3 is 32-bit anyway */

/* ── Convenience macros ──────────────────────────────────────────────── */
#define lengthof(a) ((int)(sizeof(a) / sizeof(*(a))))

/* PuTTY/mintty switch-case style — kept for readability of algorithm */
#define when \
  break;     \
  case
#define otherwise \
  break;          \
  default

/* Maximum line length the algorithm will process.
   Adjust to your actual screen width.  Stack cost = ~5×MAX bytes. */
#define BIDI_MAX_LINE 128

/* ── bidi_char ───────────────────────────────────────────────────────── */
/* origwc: the codepoint as it came from the epub text stream
   wc:     working codepoint (may be replaced by mirrored form after do_bidi)
   index:  original logical position, so the caller can reorder glyphs */
typedef struct {
  ucschar origwc;
  ucschar wc;
  uint16_t index;
} bidi_char;

/* ── Bidi character classes (UAX #9) ────────────────────────────────── */
enum {
  L,   /* Left-to-Right */
  LRE, /* Left-to-Right Embedding */
  LRO, /* Left-to-Right Override */
  R,   /* Right-to-Left */
  AL,  /* Right-to-Left Arabic */
  RLE, /* Right-to-Left Embedding */
  RLO, /* Right-to-Left Override */
  PDF, /* Pop Directional Format */
  EN,  /* European Number */
  ES,  /* European Number Separator */
  ET,  /* European Number Terminator */
  AN,  /* Arabic Number */
  CS,  /* Common Number Separator */
  NSM, /* Non-Spacing Mark */
  BN,  /* Boundary Neutral */
  B,   /* Paragraph Separator */
  S,   /* Segment Separator */
  WS,  /* Whitespace */
  ON,  /* Other Neutrals */
  /* Unicode 6.3 isolate types */
  LRI, /* Left-to-Right Isolate */
  RLI, /* Right-to-Left Isolate */
  FSI, /* First Strong Isolate */
  PDI, /* Pop Directional Isolate */
};

/* ── Public API ──────────────────────────────────────────────────────── */

/*
 * bidi_class(ch)
 *   Returns the UAX#9 bidi class of Unicode codepoint ch.
 *   Unknown characters return ON (correct per spec).
 */
uchar bidi_class(ucschar ch);

/*
 * is_rtl_class(bc)
 *   Returns true if bidi class bc can cause RTL reordering.
 *   Use to fast-skip lines with no RTL content.
 */
bool is_rtl_class(uchar bc);

/*
 * mirror(ch)
 *   Returns the mirrored form of Unicode codepoint ch for UAX#9 rule L4.
 *   If no mirror exists, returns ch unchanged.
 */
ucschar mirror(ucschar ch);

/*
 * do_bidi(autodir, paragraphLevel, line, count)
 *
 *   Applies UAX#9 Bidirectional Algorithm (rules P–L) to `line[0..count-1]`.
 *   Reorders the array in-place; sets line[i].wc to the mirrored form where
 *   required (rule L4).  Returns the resolved paragraph level (0=LTR, 1=RTL),
 *   or 0 if the line was left-to-right and no reordering was done.
 *
 *   autodir:        true  → detect paragraph direction from content (P2/P3)
 *                   false → use paragraphLevel as-is
 *   paragraphLevel: 0 = LTR, 1 = RTL.  Ignored when autodir=true unless
 *                   the content has no strong type (used as fallback).
 *
 *   count must be ≤ BIDI_MAX_LINE; lines longer than that are silently
 *   truncated to BIDI_MAX_LINE before processing.
 */
int do_bidi(bool autodir, int paragraphLevel, bidi_char* line, int count);

#endif /* MINIBIDI_H */
