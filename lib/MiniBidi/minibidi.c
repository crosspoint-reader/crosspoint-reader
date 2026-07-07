/*
 * minibidi.c — Unicode Bidirectional Algorithm (UAX #9) for CrossPoint/ESP32C3
 *
 * Original author:  Ahmad Khalifa (www.arabeyes.org, MIT licence)
 * Mintty changes:   Thomas Wolff (rules N0, W7/L1/X9 fixes, isolates)
 *
 * UAX #9: https://www.unicode.org/reports/tr9/
 */

#include "minibidi.h"

#define leastGreaterOdd(x) (((x) + 1) | 1)
#define leastGreaterEven(x) (((x) + 2) & ~1)

/* ═══════════════════════════════════════════════════════════════════════
 * flip_runs / find_run  (UAX#9 rule L2)
 * ═══════════════════════════════════════════════════════════════════════ */

static int find_run(uchar* levels, int start, int count, int tlevel) {
  for (int i = start; i < count; i++)
    if (tlevel <= levels[i]) return i;
  return count;
}

static void flip_runs(bidi_char* from, uchar* levels, int tlevel, int count) {
  int i = 0, j = 0;
  while (i < count && j < count) {
    i = j = find_run(levels, i, count, tlevel);
    while (i < count && tlevel <= levels[i]) i++;
    for (int k = i - 1; k > j; k--, j++) {
      bidi_char tmp = from[k];
      from[k] = from[j];
      from[j] = tmp;
    }
  }
}

/* ═══════════════════════════════════════════════════════════════════════
 * bidi_class()
 * ═══════════════════════════════════════════════════════════════════════ */

uchar bidi_class(ucschar ch) {
  static const struct {
    ucschar first, last;
    uchar type;
  } lookup[] = {
#include "bidiclasses.t"
  };

  int i = -1, j = lengthof(lookup);
  while (j - i > 1) {
    int k = (i + j) / 2;
    if (ch < lookup[k].first)
      j = k;
    else if (ch > lookup[k].last)
      i = k;
    else
      return lookup[k].type;
  }
  return ON; /* correct UAX#9 fallback for unlisted characters */
}

/* ═══════════════════════════════════════════════════════════════════════
 * Character class predicates
 * ═══════════════════════════════════════════════════════════════════════ */

bool is_rtl_class(uchar bc) {
  const int mask = (1 << R) | (1 << AL) | (1 << RLE) | (1 << RLO) | (1 << RLI) | (1 << FSI);
  return (mask >> bc) & 1;
}

static inline bool is_NI(uchar bc) {
  const int mask = (1 << B) | (1 << S) | (1 << WS) | (1 << ON) | (1 << FSI) | (1 << LRI) | (1 << RLI) | (1 << PDI);
  return (mask >> bc) & 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Unified bracket + mirror table (bidi_pairs.t)
 *
 * Replaces both brackets.t and mirroring.t.  canonical.t is dropped.
 * ═══════════════════════════════════════════════════════════════════════ */

enum { BRACKx = 0, BRACKo = 1, BRACKc = 2 };

typedef struct {
  ucschar from, to;
  uchar bracket; /* BRACKo / BRACKc / BRACKx */
} bidi_pair;

static const bidi_pair pairs[] = {
#include "bidi_pairs.t"
};

/* Binary search over the pairs table */
static const bidi_pair* find_pair(ucschar c) {
  int i = -1, j = lengthof(pairs);
  while (j - i > 1) {
    int k = (i + j) / 2;
    if (c == pairs[k].from)
      return &pairs[k];
    else if (c < pairs[k].from)
      j = k;
    else
      i = k;
  }
  return NULL;
}

/*
 * bracket(c):
 *   0        → not a bracket
 *   c        → opening bracket
 *   opener   → closing bracket (returns the matching opener)
 */
static ucschar bracket(ucschar c) {
  const bidi_pair* p = find_pair(c);
  if (!p || p->bracket == BRACKx) return 0;
  return (p->bracket == BRACKo) ? c : p->to;
}

/*
 * mirror(c): returns the mirrored form for rule L4,
 *            or c unchanged if not in the table.
 */
ucschar mirror(ucschar c) {
  const bidi_pair* p = find_pair(c);
  return p ? p->to : c;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Arabic contextual shaping — do_shape()
 *
 * Ported from mintty src/minibidi.c (https://github.com/mintty/mintty),
 * original author Ahmad Khalifa (www.arabeyes.org), maintained by
 * Thomas Wolff.  MIT licence.
 *
 * CrossPoint deviations from the upstream code, each marked inline:
 *   1. Joining-context lookups skip NSM marks (harakat).  mintty stores
 *      combining marks out-of-line in terminal cells, so they never appear
 *      in its bidi_char array; in CrossPoint they are real array entries
 *      and must be transparent for joining (Unicode joining type T).
 *   2. The Alef absorbed into a Lam-Alef ligature is overwritten with
 *      LIGATURE_PLACEHOLDER instead of a space: a terminal must keep the
 *      cell, a proportional-text renderer must drop the character.
 *   3. The STYPE/SISOLATED macros became functions backed by a second
 *      lookup table covering Perso-Arabic letters outside mintty's native
 *      U+0621–U+064A range (Farsi پ چ ژ گ, Urdu ٹ ڈ ڑ ں ہ ے, plus Sindhi/
 *      Pashto/Kurdish letters).  Joining types are sourced from Unicode
 *      ArabicShaping.txt and presentation forms from UnicodeData.txt
 *      (Arabic Presentation Forms-A), both Unicode 17.0.0.  Letters with a
 *      joining type but no presentation-form codepoints keep their base
 *      codepoint (neighbours still shape correctly around them).
 *      U+200C/U+200D also get their ArabicShaping.txt types (U and C) so
 *      that in-stream ZWJ/ZWNJ — which mintty never has in its array —
 *      affect adjacency the same way the joiners flags do.
 * ═══════════════════════════════════════════════════════════════════════ */

/* Shaping Types (mintty) */
enum {
  SL, /* Left-Joining, doesn't exist in U+0600 - U+06FF */
  SR, /* Right-Joining, i.e. has Isolated, Final */
  SD, /* Dual-Joining, i.e. has Isolated, Final, Initial, Medial */
  SU, /* Non-Joining */
  SC  /* Join-Causing, like U+0640 (TATWEEL) */
};

typedef struct {
  uchar type;
  uchar form_b; /* isolated form = 0xFE00 + form_b (Presentation Forms-B) */
} shape_node;

/* Kept near the actual table, for verification. (mintty) */
enum { SHAPE_FIRST = 0x621, SHAPE_LAST = 0x64A };

/* mintty's shapetypes[] — verbatim */
static const shape_node shapetypes[] = {
    /* index, Typ, Iso, Ligature Index */
    /* 621 */ {SU, 0x80},
    /* 622 */ {SR, 0x81},
    /* 623 */ {SR, 0x83},
    /* 624 */ {SR, 0x85},
    /* 625 */ {SR, 0x87},
    /* 626 */ {SD, 0x89},
    /* 627 */ {SR, 0x8D},
    /* 628 */ {SD, 0x8F},
    /* 629 */ {SR, 0x93},
    /* 62A */ {SD, 0x95},
    /* 62B */ {SD, 0x99},
    /* 62C */ {SD, 0x9D},
    /* 62D */ {SD, 0xA1},
    /* 62E */ {SD, 0xA5},
    /* 62F */ {SR, 0xA9},
    /* 630 */ {SR, 0xAB},
    /* 631 */ {SR, 0xAD},
    /* 632 */ {SR, 0xAF},
    /* 633 */ {SD, 0xB1},
    /* 634 */ {SD, 0xB5},
    /* 635 */ {SD, 0xB9},
    /* 636 */ {SD, 0xBD},
    /* 637 */ {SD, 0xC1},
    /* 638 */ {SD, 0xC5},
    /* 639 */ {SD, 0xC9},
    /* 63A */ {SD, 0xCD},
    /* 63B */ {SU, 0x0},
    /* 63C */ {SU, 0x0},
    /* 63D */ {SU, 0x0},
    /* 63E */ {SU, 0x0},
    /* 63F */ {SU, 0x0},
    /* 640 */ {SC, 0x0},
    /* 641 */ {SD, 0xD1},
    /* 642 */ {SD, 0xD5},
    /* 643 */ {SD, 0xD9},
    /* 644 */ {SD, 0xDD},
    /* 645 */ {SD, 0xE1},
    /* 646 */ {SD, 0xE5},
    /* 647 */ {SD, 0xE9},
    /* 648 */ {SR, 0xED},
    /* 649 */ {SR, 0xEF},
    /* 64A */ {SD, 0xF1}};

/* ── CrossPoint extension: joining types outside U+0621–U+064A ──────────
 * Source: Unicode ArabicShaping.txt (joining type column).  Ranges of
 * letters sharing one type are collapsed.  Sorted ascending (binary
 * search).  Anything not listed here or in shapetypes[] is SU. */
static const struct {
  ucschar first, last;
  uchar type;
} xjointypes[] = {
    {0x0620, 0x0620, SD}, /* kashmiri yeh */
    {0x066E, 0x066F, SD}, /* dotless beh/qaf */
    {0x0671, 0x0673, SR}, /* alef wasla + wavy-hamza alefs */
    {0x0675, 0x0677, SR}, /* high-hamza alef/waw */
    {0x0678, 0x0687, SD}, /* high-hamza yeh, beh group (ٹ ٺ ٻ … پ), hah group (چ ڇ) */
    {0x0688, 0x0699, SR}, /* dal group (ڈ ڊ ڌ …), reh group (ڑ ړ ژ …) */
    {0x069A, 0x06BF, SD}, /* seen/sad/feh/qaf/kaf/gaf/lam/noon groups (ک گ ں ھ …) */
    {0x06C0, 0x06C0, SR}, /* heh with yeh above */
    {0x06C1, 0x06C2, SD}, /* heh goal (ہ) */
    {0x06C3, 0x06CB, SR}, /* teh marbuta goal, waw group (ۆ ۇ ۋ …) */
    {0x06CC, 0x06CC, SD}, /* farsi yeh (ی) */
    {0x06CD, 0x06CD, SR}, /* yeh with tail */
    {0x06CE, 0x06CE, SD}, /* farsi yeh with V (Kurdish ێ) */
    {0x06CF, 0x06CF, SR}, /* waw with dot above */
    {0x06D0, 0x06D1, SD}, /* e (Pashto ې), yeh three dots below */
    {0x06D2, 0x06D3, SR}, /* yeh barree (ے ۓ) */
    {0x06D5, 0x06D5, SR}, /* ae (Kurdish ە) */
    {0x06EE, 0x06EF, SR}, /* dal/reh with inverted V */
    {0x06FA, 0x06FC, SD}, /* sheen/dad/ghain with dot below */
    {0x06FF, 0x06FF, SD}, /* heh with inverted V */
    {0x200C, 0x200C, SU}, /* ZWNJ — joining type U per ArabicShaping.txt */
    {0x200D, 0x200D, SC}, /* ZWJ  — joining type C per ArabicShaping.txt */
};

/* ── CrossPoint extension: presentation forms outside U+0621–U+064A ─────
 * Source: UnicodeData.txt, Arabic Presentation Forms-A (U+FB50–U+FBFF).
 * forms = number of consecutive presentation forms allocated for the
 * letter, always in the order isolated, final, initial, medial (matching
 * the SHAPE_* offsets below): 2 = isolated+final, 4 = all.
 * Letters absent here have no presentation forms and keep their base
 * codepoint.  Sorted ascending (binary search). */
static const struct {
  ucschar cp;
  ucschar isolated;
  uchar forms;
} xshapeforms[] = {
    {0x0671, 0xFB50, 2}, /* alef wasla */
    {0x0679, 0xFB66, 4}, /* tteh (Urdu ٹ) */
    {0x067A, 0xFB5E, 4}, /* tteheh */
    {0x067B, 0xFB52, 4}, /* beeh (Sindhi ٻ) */
    {0x067E, 0xFB56, 4}, /* peh (Farsi پ) */
    {0x067F, 0xFB62, 4}, /* teheh */
    {0x0680, 0xFB5A, 4}, /* beheh (Sindhi ڀ) */
    {0x0683, 0xFB76, 4}, /* nyeh (Sindhi ڃ) */
    {0x0684, 0xFB72, 4}, /* dyeh (Sindhi ڄ) */
    {0x0686, 0xFB7A, 4}, /* tcheh (Farsi چ) */
    {0x0687, 0xFB7E, 4}, /* tcheheh (Sindhi ڇ) */
    {0x0688, 0xFB88, 2}, /* ddal (Urdu ڈ) */
    {0x068C, 0xFB84, 2}, /* dahal (Sindhi ڌ) */
    {0x068D, 0xFB82, 2}, /* ddahal (Sindhi ڍ) */
    {0x068E, 0xFB86, 2}, /* dul (Sindhi ڎ) */
    {0x0691, 0xFB8C, 2}, /* rreh (Urdu ڑ) */
    {0x0698, 0xFB8A, 2}, /* jeh (Farsi ژ) */
    {0x06A4, 0xFB6A, 4}, /* veh (Kurdish ڤ) */
    {0x06A6, 0xFB6E, 4}, /* peheh (Sindhi ڦ) */
    {0x06A9, 0xFB8E, 4}, /* keheh (Farsi/Urdu ک) */
    {0x06AD, 0xFBD3, 4}, /* ng */
    {0x06AF, 0xFB92, 4}, /* gaf (Farsi/Urdu گ) */
    {0x06B1, 0xFB9A, 4}, /* ngoeh (Sindhi ڱ) */
    {0x06B3, 0xFB96, 4}, /* gueh (Sindhi ڳ) */
    {0x06BA, 0xFB9E, 2}, /* noon ghunna (Urdu ں) — dual-joining but only
                            isolated+final forms exist; initial/medial
                            contexts keep the base codepoint */
    {0x06BB, 0xFBA0, 4}, /* rnoon (Sindhi ڻ) */
    {0x06BE, 0xFBAA, 4}, /* heh doachashmee (Urdu ھ) */
    {0x06C0, 0xFBA4, 2}, /* heh with yeh above */
    {0x06C1, 0xFBA6, 4}, /* heh goal (Urdu ہ) */
    {0x06C5, 0xFBE0, 2}, /* kirghiz oe */
    {0x06C6, 0xFBD9, 2}, /* oe (Kurdish ۆ) */
    {0x06C7, 0xFBD7, 2}, /* u (ۇ) */
    {0x06C8, 0xFBDB, 2}, /* yu */
    {0x06C9, 0xFBE2, 2}, /* kirghiz yu */
    {0x06CB, 0xFBDE, 2}, /* ve */
    {0x06CC, 0xFBFC, 4}, /* farsi yeh (Farsi/Urdu ی) */
    {0x06D0, 0xFBE4, 4}, /* e (Pashto ې) */
    {0x06D2, 0xFBAE, 2}, /* yeh barree (Urdu ے) */
    {0x06D3, 0xFBB0, 2}, /* yeh barree with hamza above (ۓ) */
};

/* Contextual form offsets from the isolated form — identical ordering in
   Presentation Forms-A and -B, matching mintty's SFINAL/SINITIAL/SMEDIAL
   (+1/+2/+3) macros. */
enum { SHAPE_ISOLATED = 0, SHAPE_FINAL = 1, SHAPE_INITIAL = 2, SHAPE_MEDIAL = 3 };

/* STYPE equivalent (mintty macro → function to add the extended table) */
static uchar stype(ucschar c) {
  if (c >= SHAPE_FIRST && c <= SHAPE_LAST) return shapetypes[c - SHAPE_FIRST].type;

  int i = -1, j = lengthof(xjointypes);
  while (j - i > 1) {
    int k = (i + j) / 2;
    if (c < xjointypes[k].first)
      j = k;
    else if (c > xjointypes[k].last)
      i = k;
    else
      return xjointypes[k].type;
  }
  return SU;
}

/* SISOLATED/SFINAL/SINITIAL/SMEDIAL equivalent.
   form is one of the SHAPE_* offsets; returns c unchanged when the letter
   has no presentation form allocated for that context. */
static ucschar shape_form(ucschar c, uchar form) {
  if (c >= SHAPE_FIRST && c <= SHAPE_LAST) {
    const uchar form_b = shapetypes[c - SHAPE_FIRST].form_b;
    return form_b ? 0xFE00 + form_b + form : c;
  }

  int i = -1, j = lengthof(xshapeforms);
  while (j - i > 1) {
    int k = (i + j) / 2;
    if (c < xshapeforms[k].cp)
      j = k;
    else if (c > xshapeforms[k].cp)
      i = k;
    else
      return form < xshapeforms[k].forms ? xshapeforms[k].isolated + form : c;
  }
  return c;
}

/* CrossPoint deviation 1: NSM marks (harakat) sit between letters in our
   array but are transparent for joining (Unicode joining type T).  These
   helpers find the effective joining neighbour. */
static int next_non_nsm(const bidi_char* line, int i, int count) {
  int j = i + 1;
  while (j < count && bidi_class(line[j].wc) == NSM) j++;
  return j;
}

static int prev_non_nsm(const bidi_char* line, int i) {
  int j = i - 1;
  while (j >= 0 && bidi_class(line[j].wc) == NSM) j--;
  return j;
}

/* The Main shaping function (mintty, structure preserved).
 *
 * line: visual-order buffer — must have been passed through do_bidi() first
 * to:   output buffer for the shaped data
 * count: number of characters in line
 */
int do_shape(bidi_char* line, bidi_char* to, int count) {
  for (int i = 0; i < count; i++) {
    to[i] = line[i];
    int tempShape = stype(line[i].wc);
    switch (tempShape) {
      case SR: {                                     /* Right-Joining, i.e. has Isolated, Final */
        const int nx = next_non_nsm(line, i, count); /* deviation 1: was i + 1 */
        tempShape = (nx < count) ? stype(line[nx].wc) : SU;
        if (tempShape == SL || tempShape == SD || tempShape == SC)
          to[i].wc = shape_form(line[i].wc, SHAPE_FINAL);
        else
          to[i].wc = shape_form(line[i].wc, SHAPE_ISOLATED);
        break;
      }
      case SD: {                                     /* Dual-Joining, i.e. has Isolated, Final, Initial, Medial */
        const int nx = next_non_nsm(line, i, count); /* deviation 1: was i + 1 */
        const int pv = prev_non_nsm(line, i);        /* deviation 1: was i - 1 */

        /* Make Ligatures */
        tempShape = (nx < count) ? stype(line[nx].wc) : SU;
        if (line[i].wc == 0x644) { /* Lam: the Alef variant is at pv (visually left) */
          int ligFlag = 0;
          switch (pv >= 0 ? line[pv].wc : 0) {
            case 0x622: /* Alef with Madda above → لآ */
              ligFlag = 1;
              to[i].wc = (tempShape == SL || tempShape == SD || tempShape == SC) ? 0xFEF6 : 0xFEF5;
              break;
            case 0x623: /* Alef with Hamza above → لأ */
              ligFlag = 1;
              to[i].wc = (tempShape == SL || tempShape == SD || tempShape == SC) ? 0xFEF8 : 0xFEF7;
              break;
            case 0x625: /* Alef with Hamza below → لإ */
              ligFlag = 1;
              to[i].wc = (tempShape == SL || tempShape == SD || tempShape == SC) ? 0xFEFA : 0xFEF9;
              break;
            case 0x627: /* Alef → لا */
              ligFlag = 1;
              to[i].wc = (tempShape == SL || tempShape == SD || tempShape == SC) ? 0xFEFC : 0xFEFB;
              break;
          }
          if (ligFlag) {
            to[pv].wc = LIGATURE_PLACEHOLDER; /* deviation 2: mintty writes 0x20 */
            break;
          }
        }

        /* Arabic joining formatters: adapt forms (mintty) */
        const uchar joiners = line[i].joiners & 0xF;
        const uchar prevjoiners = line[i].joiners >> 4;
        if (prevjoiners == ZWNJ) {
          /* backward join blocked; initial only if the visually-right
             (logically next) neighbour joins, else isolated */
          tempShape = (pv >= 0) ? stype(line[pv].wc) : SU;
          if (tempShape == SR || tempShape == SD || tempShape == SC)
            to[i].wc = shape_form(line[i].wc, SHAPE_INITIAL);
          else
            to[i].wc = shape_form(line[i].wc, SHAPE_ISOLATED);
        } else if (prevjoiners == (ZWJ | ZWNJ)) {
          to[i].wc = shape_form(line[i].wc, SHAPE_MEDIAL);
        } else if (prevjoiners == ZWJ) {
          to[i].wc = shape_form(line[i].wc, SHAPE_FINAL);
        } else if (joiners & ZWNJ) {
          /* forward join blocked; final only if the visually-left
             (logically previous) neighbour joins, else isolated —
             tempShape still holds stype(nx) from above */
          if (tempShape == SL || tempShape == SD || tempShape == SC)
            to[i].wc = shape_form(line[i].wc, SHAPE_FINAL);
          else
            to[i].wc = shape_form(line[i].wc, SHAPE_ISOLATED);
        } else if (tempShape == SL || tempShape == SD || tempShape == SC) {
          /* visually-right neighbour joins → final, or medial if the
             visually-left neighbour joins too */
          tempShape = (pv >= 0) ? stype(line[pv].wc) : SU;
          if (tempShape == SR || tempShape == SD || tempShape == SC)
            to[i].wc = shape_form(line[i].wc, SHAPE_MEDIAL);
          else
            to[i].wc = shape_form(line[i].wc, SHAPE_FINAL);
        } else {
          /* visually-right neighbour doesn't join → isolated, or initial
             if the visually-left neighbour joins */
          tempShape = (pv >= 0) ? stype(line[pv].wc) : SU;
          if (tempShape == SR || tempShape == SD || tempShape == SC)
            to[i].wc = shape_form(line[i].wc, SHAPE_INITIAL);
          else
            to[i].wc = shape_form(line[i].wc, SHAPE_ISOLATED);
        }
        break;
      }
      default:
        /* SU, SL, SC and non-Arabic characters pass through unchanged */
        break;
    }
  }
  return 1;
}

/* ═══════════════════════════════════════════════════════════════════════
 * Directional Status Stack
 * (replaces GCC nested functions — ESP32C3 has no executable stack)
 * ═══════════════════════════════════════════════════════════════════════ */

typedef struct {
  uchar emb[BIDI_MAX_LINE + 1];
  uchar ovr[BIDI_MAX_LINE + 1];
  bool isol[BIDI_MAX_LINE + 1];
  int top;
} DirStatusStack;

static inline void dss_init(DirStatusStack* s) { s->top = -1; }
static inline int dss_count(const DirStatusStack* s) { return s->top + 1; }

static inline void dss_push(DirStatusStack* s, uchar emb, uchar ovr, bool isol) {
  if (s->top < BIDI_MAX_LINE) {
    ++s->top;
    s->emb[s->top] = emb;
    s->ovr[s->top] = ovr;
    s->isol[s->top] = isol;
  }
}

static inline void dss_pop(DirStatusStack* s, uchar* emb, uchar* ovr, bool* isol) {
  if (s->top >= 0) s->top--;
  if (s->top >= 0) {
    *emb = s->emb[s->top];
    *ovr = s->ovr[s->top];
    *isol = s->isol[s->top];
  } else {
    /* Stack underflow: return safe defaults (should not happen in valid input) */
    *emb = 0;      /* LTR base level */
    *ovr = ON;     /* No override */
    *isol = false; /* No isolate */
  }
}

/* ═══════════════════════════════════════════════════════════════════════
 * do_bidi()  — The main UAX#9 algorithm
 * ═══════════════════════════════════════════════════════════════════════ */

int do_bidi(bool autodir, int paragraphLevel, bidi_char* line, int count) {
  if (count > BIDI_MAX_LINE) count = BIDI_MAX_LINE;

  uchar currentEmbedding, currentOverride;
  bool currentIsolate;
  int i, j;

  /* Fixed-size working arrays — no VLAs, no heap */
  uchar types[BIDI_MAX_LINE];
  uchar levels[BIDI_MAX_LINE];
  bool skip[BIDI_MAX_LINE];

  /* ── P2/P3: detect paragraph level ── */
  int isolateLevel = 0, resLevel = -1;
  bool hasRTL = false;

  for (i = 0; i < count; i++) {
    uchar type = bidi_class(line[i].wc);
    if (type == LRI || type == RLI || type == FSI) {
      hasRTL = true;
      isolateLevel++;
    } else if (type == PDI) {
      hasRTL = true;
      if (isolateLevel > 0) isolateLevel--;
    } else if (isolateLevel == 0) {
      if (type == R || type == AL) {
        hasRTL = true;
        if (resLevel < 0) resLevel = 1;
        break;
      } else if (type == RLE || type == LRE || type == RLO || type == LRO || type == PDF) {
        hasRTL = true;
        if (resLevel >= 0) break;
      } else if (type == L) {
        if (resLevel < 0) resLevel = 0;
      } else if (type == AN)
        hasRTL = true;
    }
  }

  if (autodir) {
    if (resLevel >= 0) paragraphLevel = resLevel;
  } else
    resLevel = paragraphLevel;

  /* Fast path: pure LTR line with LTR paragraph — nothing to reorder */
  if (!hasRTL && !paragraphLevel) return 0;

  /* ── X1–X8: compute embedding levels ── */
  currentEmbedding = (uchar)paragraphLevel;
  currentOverride = ON;
  currentIsolate = false;
  isolateLevel = 0;

  DirStatusStack dss;
  dss_init(&dss);
  dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);

  for (i = 0; i < count; i++) {
    uchar tempType = bidi_class(line[i].wc);
    levels[i] = currentEmbedding;

    /* FSI: look-ahead to resolve direction */
    if (tempType == FSI) {
      int lvl = 0;
      tempType = LRI;
      for (int k = i + 1; k < count; k++) {
        uchar kt = bidi_class(line[k].wc);
        if (kt == FSI || kt == RLI || kt == LRI)
          lvl++;
        else if (kt == PDI) {
          if (lvl)
            lvl--;
          else
            break;
        } else if (kt == R || kt == AL) {
          tempType = RLI;
          break;
        } else if (kt == L)
          break;
      }
    }

    switch (tempType) {
      when RLE : currentEmbedding = leastGreaterOdd(currentEmbedding);
      currentOverride = ON;
      currentIsolate = false;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when LRE : currentEmbedding = leastGreaterEven(currentEmbedding);
      currentOverride = ON;
      currentIsolate = false;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when RLO : currentEmbedding = leastGreaterOdd(currentEmbedding);
      currentOverride = R;
      currentIsolate = false;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when LRO : currentEmbedding = leastGreaterEven(currentEmbedding);
      currentOverride = L;
      currentIsolate = false;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when RLI : if (currentOverride != ON) tempType = currentOverride;
      currentEmbedding = leastGreaterOdd(currentEmbedding);
      isolateLevel++;
      currentOverride = ON;
      currentIsolate = true;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when LRI : if (currentOverride != ON) tempType = currentOverride;
      currentEmbedding = leastGreaterEven(currentEmbedding);
      isolateLevel++;
      currentOverride = ON;
      currentIsolate = true;
      dss_push(&dss, currentEmbedding, currentOverride, currentIsolate);
      when PDF : if (!currentIsolate && dss_count(&dss) >= 2)
                     dss_pop(&dss, &currentEmbedding, &currentOverride, &currentIsolate);
      levels[i] = currentEmbedding;
      when PDI : if (isolateLevel > 0) {
        while (!currentIsolate && dss_count(&dss) > 0)
          dss_pop(&dss, &currentEmbedding, &currentOverride, &currentIsolate);
        dss_pop(&dss, &currentEmbedding, &currentOverride, &currentIsolate);
        isolateLevel--;
      }
      if (currentOverride != ON) tempType = currentOverride;
      levels[i] = currentEmbedding;
    when WS : case S:
      if (currentOverride != ON) tempType = currentOverride;
      otherwise : if (currentOverride != ON) tempType = currentOverride;
    }
    types[i] = tempType;
  }

  /* ── X9: mask format chars as NSM (Wolff fix: NSM not BN) ── */
  for (i = 0; i < count; i++) {
    switch (types[i]) {
    when RLE : case LRE:
    case RLO:
    case LRO:
    case PDF:
    case BN:
      types[i] = NSM;
      skip[i] = true;
    otherwise:
      skip[i] = false;
    }
  }

  /* ── W1: NSM inherits type of previous char (or sor) ── */
  if (types[0] == NSM) types[0] = (paragraphLevel & 1) ? R : L;
  for (i = 1; i < count; i++) {
    if (types[i] == NSM) {
      switch (types[i - 1]) {
      when LRI : case RLI:
      case FSI:
      case PDI:
        types[i] = ON;
        otherwise : types[i] = types[i - 1];
      }
    }
  }

  /* ── W2: EN after AL → AN ── */
  for (i = 0; i < count; i++) {
    if (types[i] == EN) {
      for (j = i - 1; j >= 0; j--) {
        uchar t = types[j];
        if (t == AL) {
          types[i] = AN;
          break;
        }
        if (t == R || t == L) break;
      }
    }
  }

  /* ── W3: AL → R ── */
  for (i = 0; i < count; i++)
    if (types[i] == AL) types[i] = R;

  /* ── W4: single ES/CS between same numerals → that numeral type ── */
  for (i = 1; i + 1 < count; i++) {
    if (types[i] == ES || types[i] == CS) {
      int prev = i - 1;
      while (prev >= 0 && skip[prev]) prev--;
      int next = i + 1;
      while (next < count && skip[next]) next++;
      if (prev >= 0 && next < count) {
        if (types[i] == ES && types[prev] == EN && types[next] == EN) types[i] = EN;
        if (types[i] == CS) {
          if (types[prev] == EN && types[next] == EN) types[i] = EN;
          if (types[prev] == AN && types[next] == AN) types[i] = AN;
        }
      }
    }
  }

  /* ── W5: ET adjacent to EN → EN (forward pass) ── */
  for (i = 0; i < count; i++) {
    if (skip[i] || types[i] != ET) continue;
    for (j = i; j < count; j++) {
      if (skip[j]) continue;
      if (types[j] == ET) continue;
      if (types[j] == EN) types[i] = EN;
      break;
    }
  }
  /* W5 backward pass */
  for (i = count - 1; i >= 0; i--) {
    if (skip[i] || types[i] != ET) continue;
    for (j = i; j >= 0; j--) {
      if (skip[j]) continue;
      if (types[j] == ET) continue;
      if (types[j] == EN) types[i] = EN;
      break;
    }
  }

  /* ── W6: remaining ES, ET, CS → ON ── */
  for (i = 0; i < count; i++)
    if (types[i] == ES || types[i] == ET || types[i] == CS) types[i] = ON;

  /* ── W7: EN after last strong L (back to sor) → L ── */
  {
    uchar last_strong = (paragraphLevel & 1) ? R : L;
    for (i = 0; i < count; i++) {
      if (skip[i]) continue;
      if (types[i] == L || types[i] == R) last_strong = types[i];
      if (types[i] == EN && last_strong == L) types[i] = L;
    }
  }

  /* ── N0: bracket pair handling ── */
  {
    uchar e = (paragraphLevel & 1) ? R : L;
    uchar o = (e == L) ? R : L;
#define BRACKET_STACK 63
    struct {
      ucschar opener;
      int pos;
    } openers[BRACKET_STACK];
    int opener_top = 0;

    for (i = 0; i < count; i++) {
      if (skip[i]) continue;
      ucschar bc = bracket(line[i].wc);
      if (!bc) continue;

      if (bc == line[i].wc) {
        /* Opening bracket */
        if (opener_top < BRACKET_STACK) {
          openers[opener_top].opener = line[i].wc;
          openers[opener_top].pos = i;
          opener_top++;
        }
      } else {
        /* Closing bracket: find matching opener */
        int k;
        for (k = opener_top - 1; k >= 0; k--)
          if (openers[k].opener == bc) break;
        if (k < 0) continue;

        int open_pos = openers[k].pos;
        opener_top = k;

        bool found_e = false, found_o = false;
        for (int m = open_pos + 1; m < i; m++) {
          if (skip[m]) continue;
          uchar t = types[m];
          if (t == EN || t == AN) t = R;
          if (t == R || t == AL) {
            if (e == R)
              found_e = true;
            else
              found_o = true;
          } else if (t == L) {
            if (e == L)
              found_e = true;
            else
              found_o = true;
          }
        }

        uchar dir;
        if (found_e) {
          dir = e;
        } else if (found_o) {
          uchar ctx = e;
          for (int m = open_pos - 1; m >= 0; m--) {
            if (skip[m]) continue;
            uchar t = types[m];
            if (t == EN || t == AN) t = R;
            if (t == R || t == AL) {
              ctx = R;
              break;
            } else if (t == L) {
              ctx = L;
              break;
            }
          }
          dir = (ctx == o) ? o : e;
        } else {
          continue;
        }

        types[open_pos] = dir;
        types[i] = dir;
        for (int m = open_pos + 1; m < i; m++)
          if (is_NI(types[m])) types[m] = dir;
      }
    }
#undef BRACKET_STACK
  }

  /* ── N1: NI between same-direction strongs → that direction ── */
  for (i = 0; i < count; i++) {
    if (skip[i] || !is_NI(types[i])) continue;
    int end = i;
    while (end + 1 < count && (skip[end + 1] || is_NI(types[end + 1]))) end++;

    uchar prev_strong = (paragraphLevel & 1) ? R : L;
    for (j = i - 1; j >= 0; j--) {
      if (skip[j]) continue;
      uchar t = types[j];
      if (t == EN || t == AN) t = R;
      if (t == R || t == L) {
        prev_strong = t;
        break;
      }
    }
    uchar next_strong = (paragraphLevel & 1) ? R : L;
    for (j = end + 1; j < count; j++) {
      if (skip[j]) continue;
      uchar t = types[j];
      if (t == EN || t == AN) t = R;
      if (t == R || t == L) {
        next_strong = t;
        break;
      }
    }
    if (prev_strong == next_strong)
      for (j = i; j <= end; j++) types[j] = prev_strong;
    i = end;
  }

  /* ── N2: remaining NI → embedding direction ── */
  for (i = 0; i < count; i++)
    if (is_NI(types[i])) types[i] = (levels[i] & 1) ? R : L;

  /* ── I1/I2: adjust levels ── */
  for (i = 0; i < count; i++) {
    if (skip[i]) continue;
    if ((levels[i] & 1) == 0) {
      if (types[i] == R)
        levels[i] += 1;
      else if (types[i] == AN || types[i] == EN)
        levels[i] += 2;
    } else {
      if (types[i] == L || types[i] == EN || types[i] == AN) levels[i] += 1;
    }
  }

  /* ── L1: reset trailing/segment whitespace to paragraph level ── */
  for (i = count - 1; i >= 0; i--) {
    if (skip[i]) continue;
    uchar t = types[i];
    if (t == WS || t == S || t == B)
      levels[i] = (uchar)paragraphLevel;
    else
      break;
  }
  for (i = 0; i < count; i++) {
    if (types[i] == S) {
      levels[i] = (uchar)paragraphLevel;
      for (j = i - 1; j >= 0; j--) {
        if (skip[j]) continue;
        if (types[j] == WS || types[j] == BN)
          levels[j] = (uchar)paragraphLevel;
        else
          break;
      }
    }
  }

  /* ── L2: reverse from highest level down to lowest odd ── */
  uchar max_level = (uchar)paragraphLevel, min_odd = 255;
  for (i = 0; i < count; i++) {
    if (levels[i] > max_level) max_level = levels[i];
    if ((levels[i] & 1) && levels[i] < min_odd) min_odd = levels[i];
  }
  for (int level = max_level; level >= (int)min_odd; level--) flip_runs(line, levels, level, count);

  /* ── L4: mirror characters in RTL runs ── */
  for (i = 0; i < count; i++)
    if (levels[i] & 1) line[i].wc = mirror(line[i].wc);

  return paragraphLevel;
}
