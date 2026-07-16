#ifndef MINIBIDI_H
#define MINIBIDI_H

/*
 * minibidi.h — standalone header for ESP32C3 BiDi calculations
 *
 * Derived from [mintty](https://github.com/mintty/mintty/) (Thomas Wolff, MIT licence).
 * Includes: UAX#9 bidi (do_bidi) and Arabic contextual shaping (do_shape),
 * both ported from mintty src/minibidi.c (Ahmad Khalifa, Thomas Wolff).
 * Stripped of: box-drawing mirror, terminal dependencies, GCC nested
 * functions, VLAs, and Unicode data for scripts CrossPoint doesn't render.
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
/* origwc:  the codepoint as it came from the epub text stream
   wc:      working codepoint (may be replaced by mirrored form after
            do_bidi, or by an Arabic contextual form after do_shape)
   index:   original logical position, so the caller can reorder glyphs
   joiners: ZWJ/ZWNJ context for Arabic shaping, mintty layout:
            low nibble  = joiners that logically FOLLOW this character,
            high nibble = joiners that logically PRECEDE this character */
typedef struct {
  ucschar origwc;
  ucschar wc;
  uint16_t index;
  uint8_t joiners;
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

/* ── Arabic joining formatter flags (bidi_char.joiners nibbles) ─────── */
/* Values match mintty's minibidi.h: ZWNJ 0x01, ZWJ 0x02.  do_shape()
   compares nibble values directly, so these must not be changed. */
enum {
  ZWNJ = 0x01, /* U+200C ZERO WIDTH NON-JOINER */
  ZWJ = 0x02,  /* U+200D ZERO WIDTH JOINER */
};

/* Sentinel written by do_shape() over the Alef absorbed into a Lam-Alef
   ligature.  Callers must filter it out when emitting shaped text.
   (Upstream mintty writes a space instead — a terminal must keep the cell;
   a proportional-text renderer must drop the character entirely.) */
#define LIGATURE_PLACEHOLDER 0xFFFFu

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
 * do_shape(line, to, count)
 *
 *   Applies Arabic contextual shaping (and Lam-Alef ligation) to
 *   `line[0..count-1]`, writing the result to `to[0..count-1]`.
 *
 *   MUST be called AFTER do_bidi(): the algorithm resolves joining from
 *   VISUAL adjacency (line[i-1] is the visually-left neighbour, line[i+1]
 *   the visually-right one), exactly like upstream mintty.
 *
 *   line:  visual-order input; the joiners field must be populated by the
 *          caller (in logical order, before do_bidi) for ZWJ/ZWNJ support
 *   to:    output buffer, same size as line; non-Arabic entries are copied
 *          through unchanged.  An Alef absorbed by a Lam-Alef ligature is
 *          replaced with LIGATURE_PLACEHOLDER — filter it on emission.
 *   count: number of characters (≤ BIDI_MAX_LINE)
 *
 *   Returns 1.
 *
 *   Ported from mintty src/minibidi.c (Ahmad Khalifa, Thomas Wolff,
 *   MIT licence), https://github.com/mintty/mintty — with CrossPoint
 *   extensions for Perso-Arabic letters and in-stream diacritics, see
 *   minibidi.c for details.
 */
int do_shape(bidi_char* line, bidi_char* to, int count);

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
