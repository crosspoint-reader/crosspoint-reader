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
