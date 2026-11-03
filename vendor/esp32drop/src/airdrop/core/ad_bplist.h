/* ad_bplist.h -- write the one shape of binary plist AirDrop needs: a flat dictionary.
 *
 * WHY THIS EXISTS. The name shown on the sender's share sheet is the
 * ReceiverComputerName field of the bplist we return from /Discover -- measured, by giving
 * three candidate name fields three different values and looking at the tile. Which means
 * AirDrop.begin("My Badge") cannot be a strcpy into a constant: bplist strings are
 * LENGTH-PREFIXED, and changing a length moves every entry of the offset table and the
 * three 64-bit fields in the trailer. The response has to be built.
 *
 * SCOPE, deliberately small: one dictionary, string or data values, no nesting, no
 * integers, no references shared between values. That is exactly what /Discover and /Ask
 * return. A general plist writer would be more code and more ways to be wrong, and none of
 * the extra generality has a caller.
 *
 * UTF-16 IS NOT OPTIONAL. bplist's 0x5 string type is ASCII only. A device called
 * "リビングのバッジ" has to go out as 0x6 (UTF-16 big-endian) or not at all, and getting
 * that wrong means the name silently becomes mojibake on someone else's phone. The writer
 * picks per string: ASCII when every byte is < 0x80, UTF-16BE otherwise.
 *
 * Byte-exactness is checked against Python's plistlib in tools/test-bplist.sh -- goldens
 * produced by an implementation that is not this one, which is the only kind worth having.
 */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum { AD_BP_STR = 0, AD_BP_DATA = 1, AD_BP_INT = 2 };

struct AdBplistKV {
  const char* key; /* ASCII, NUL-terminated                                    */
  int type;        /* AD_BP_STR (UTF-8 in), AD_BP_DATA, or AD_BP_INT           */
  const void* val; /* for AD_BP_INT: ignored -- the number goes in `num`        */
  uint32_t len;    /* bytes of val; for AD_BP_STR, strlen if 0 is passed       */
  int64_t num;     /* AD_BP_INT only                                           */
};

/* --- UTF-8 -> UTF-16BE, returning code units, or -1 on malformed input ------------
 * Malformed input is refused rather than substituted: a name is a thing a user typed,
 * and quietly replacing part of it with U+FFFD is worse than telling them it was wrong. */
static int ad_bp_utf16_len(const uint8_t* s, uint32_t n) {
  int units = 0;
  for (uint32_t i = 0; i < n;) {
    uint8_t c = s[i];
    uint32_t cp, need;
    if (c < 0x80) {
      cp = c;
      need = 0;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      need = 2;
    } else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      need = 3;
    } else
      return -1;
    if (i + need >= n + 0 && need > 0 && i + need > n - 1) return -1;
    for (uint32_t k = 1; k <= need; k++) {
      if ((s[i + k] & 0xC0) != 0x80) return -1;
      cp = (cp << 6) | (uint32_t)(s[i + k] & 0x3F);
    }
    if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return -1;
    units += (cp >= 0x10000) ? 2 : 1;
    i += need + 1;
  }
  return units;
}

static uint8_t* ad_bp_put_utf16(uint8_t* p, const uint8_t* s, uint32_t n) {
  for (uint32_t i = 0; i < n;) {
    uint8_t c = s[i];
    uint32_t cp, need;
    if (c < 0x80) {
      cp = c;
      need = 0;
    } else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      need = 1;
    } else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      need = 2;
    } else {
      cp = c & 0x07;
      need = 3;
    }
    for (uint32_t k = 1; k <= need; k++) cp = (cp << 6) | (uint32_t)(s[i + k] & 0x3F);
    i += need + 1;
    if (cp >= 0x10000) {
      uint32_t v = cp - 0x10000;
      uint16_t hi = (uint16_t)(0xD800 + (v >> 10)), lo = (uint16_t)(0xDC00 + (v & 0x3FF));
      *p++ = (uint8_t)(hi >> 8);
      *p++ = (uint8_t)hi;
      *p++ = (uint8_t)(lo >> 8);
      *p++ = (uint8_t)lo;
    } else {
      *p++ = (uint8_t)(cp >> 8);
      *p++ = (uint8_t)cp;
    }
  }
  return p;
}

/* marker | count, with the 0x1F + 1-byte-int escape when count >= 15. Lengths above 255
   are refused: nothing AirDrop returns is that large, and a silent 2-byte-int path would
   be untested code on a wire format. */
static uint8_t* ad_bp_marker(uint8_t* p, uint8_t base, uint32_t count) {
  if (count < 15) {
    *p++ = (uint8_t)(base | count);
  } else {
    *p++ = (uint8_t)(base | 0x0F);
    *p++ = 0x10;
    *p++ = (uint8_t)count;
  }
  return p;
}

/* Serialise {key: value, ...} as bplist00. Returns bytes written, or 0 on any refusal
 * (too many pairs, a length that needs more than one byte, malformed UTF-8, no room).
 *
 * KEYS ARE SORTED HERE, not by the caller, because plistlib sorts them and a receiver
 * that binary-compares would see a different file otherwise -- and because a caller
 * cannot be expected to know that. */
#define AD_BP_MAX_PAIRS 8

static uint32_t ad_bplist_dict(uint8_t* out, uint32_t cap, const struct AdBplistKV* kv_in, uint32_t n) {
  if (n == 0 || n > AD_BP_MAX_PAIRS) return 0;

  struct AdBplistKV kv[AD_BP_MAX_PAIRS];
  for (uint32_t i = 0; i < n; i++) {
    kv[i] = kv_in[i];
    if (!kv[i].key) return 0;
    if (kv[i].type != AD_BP_INT && !kv[i].val) return 0;
    if (kv[i].type == AD_BP_STR && kv[i].len == 0) kv[i].len = (uint32_t)strlen((const char*)kv[i].val);
    if (kv[i].len > 255 || strlen(kv[i].key) > 255) return 0;
  }
  for (uint32_t i = 1; i < n; i++) { /* insertion sort, n <= 8 */
    struct AdBplistKV t = kv[i];
    uint32_t j = i;
    while (j > 0 && strcmp(kv[j - 1].key, t.key) > 0) {
      kv[j] = kv[j - 1];
      j--;
    }
    kv[j] = t;
  }

  /* Object 0 is the dict, then the n keys, then the n values -- the order plistlib's
     flattener produces, which is what makes a byte-exact golden possible. */
  const uint32_t nobj = 1 + 2 * n;
  if (nobj > 255) return 0;
  uint32_t off[1 + 2 * AD_BP_MAX_PAIRS];
  uint8_t *p = out, *end = out + cap;
  if (cap < 8 + 32) return 0;

  memcpy(p, "bplist00", 8);
  p += 8;

  off[0] = (uint32_t)(p - out);
  if (end - p < (ptrdiff_t)(3 + 2 * n)) return 0;
  p = ad_bp_marker(p, 0xD0, n);
  for (uint32_t i = 0; i < n; i++) *p++ = (uint8_t)(1 + i);     /* key refs   */
  for (uint32_t i = 0; i < n; i++) *p++ = (uint8_t)(1 + n + i); /* value refs */

  for (uint32_t i = 0; i < n; i++) { /* the keys   */
    uint32_t kl = (uint32_t)strlen(kv[i].key);
    off[1 + i] = (uint32_t)(p - out);
    if (end - p < (ptrdiff_t)(3 + kl)) return 0;
    p = ad_bp_marker(p, 0x50, kl);
    memcpy(p, kv[i].key, kl);
    p += kl;
  }

  for (uint32_t i = 0; i < n; i++) { /* the values */
    off[1 + n + i] = (uint32_t)(p - out);
    const uint8_t* v = (const uint8_t*)kv[i].val;
    uint32_t vl = kv[i].len;
    if (kv[i].type == AD_BP_INT) {
      /* bplist integers are 1/2/4/8 bytes big-endian, tagged 0x1n where n is log2 of the
       * width. plistlib emits the SMALLEST width that holds the value, and a receiver
       * that byte-compares would see a different file otherwise -- the same reason the
       * keys are sorted here. Negative values are always 8 bytes in Apple's format, and
       * this refuses them rather than guessing: nothing this writer emits needs one, and
       * an untested encoding on the wire is how a plist "works" until it does not.
       *
       * MEASURED, and it is why this type exists at all: a real macOS sender's /Discover
       * request body is {SenderRecordData: <data 3802>, DeviceSupportFlags: 111611},
       * measured off the air. Without integers the send path cannot
       * produce the second key. */
      int64_t x = kv[i].num;
      if (x < 0) return 0;
      int w = (x <= 0xffLL) ? 1 : (x <= 0xffffLL) ? 2 : (x <= 0xffffffffLL) ? 4 : 8;
      if (end - p < (ptrdiff_t)(1 + w)) return 0;
      *p++ = (uint8_t)(0x10 | (w == 1 ? 0 : w == 2 ? 1 : w == 4 ? 2 : 3));
      for (int k = w - 1; k >= 0; k--) *p++ = (uint8_t)((uint64_t)x >> (8 * k));
    } else if (kv[i].type == AD_BP_DATA) {
      if (end - p < (ptrdiff_t)(3 + vl)) return 0;
      p = ad_bp_marker(p, 0x40, vl);
      memcpy(p, v, vl);
      p += vl;
    } else {
      bool ascii = true;
      for (uint32_t k = 0; k < vl; k++)
        if (v[k] & 0x80) {
          ascii = false;
          break;
        }
      if (ascii) {
        if (end - p < (ptrdiff_t)(3 + vl)) return 0;
        p = ad_bp_marker(p, 0x50, vl);
        memcpy(p, v, vl);
        p += vl;
      } else {
        int units = ad_bp_utf16_len(v, vl);
        if (units < 0 || units > 255) return 0;
        if (end - p < (ptrdiff_t)(3 + 2 * units)) return 0;
        p = ad_bp_marker(p, 0x60, (uint32_t)units);
        p = ad_bp_put_utf16(p, v, vl);
      }
    }
  }

  uint32_t table_off = (uint32_t)(p - out);
  if (end - p < (ptrdiff_t)(nobj + 32)) return 0;
  for (uint32_t i = 0; i < nobj; i++) *p++ = (uint8_t)off[i]; /* 1-byte offsets */

  memset(p, 0, 5);
  p += 5;   /* unused                                    */
  *p++ = 0; /* sort version                              */
  *p++ = 1; /* offset int size                           */
  *p++ = 1; /* object ref size                           */
  /* The trailer's three counts are 64-bit big-endian. Shifting a uint32_t by 56 is
     UNDEFINED, not merely wide -- and it does not fail loudly: the compiler here reduced
     the shift modulo 32, so num_objects came out as 00 00 00 07 00 00 00 07 and the
     plist was structurally wrong in a way only a byte-exact golden would show.
     UBSan named the line; the golden named the byte. */
  {
    uint64_t n64 = nobj, t64 = table_off;
    for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(n64 >> (8 * i)); /* num objects */
    for (int i = 0; i < 8; i++) *p++ = 0;                          /* root = 0    */
    for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(t64 >> (8 * i));
  } /* table offset*/
  return (uint32_t)(p - out);
}

/* ===================================================================================
 * THE NESTED WRITER -- for the SEND path
 * ===================================================================================
 *
 * ad_bplist_dict() above writes one flat dictionary, which is all a RECEIVER ever
 * returns. A SENDER cannot use it: a real /Ask request body is a dictionary whose `Files`
 * value is an ARRAY OF DICTIONARIES, measured off the air from a real macOS sender.
 * The flat writer is left exactly as it is -- it has
 * 28 byte-exact goldens and the receiver depends on every one of them.
 *
 * BUILD A TREE, THEN SERIALISE. bplist is not a streaming format: the trailer carries the
 * object count and the offset-table position, and every value is addressed by an index
 * into a table that cannot be written until every object's size is known. So nodes are
 * appended to a fixed pool and the whole graph is emitted at the end. No allocation.
 *
 * ⚠️ TWO THINGS MUST MATCH plistlib EXACTLY OR THE GOLDENS ARE WORTHLESS, and neither is
 * guessable from the format:
 *
 *   ORDER. plistlib's _flatten appends the container itself, then recurses over ALL KEYS
 *   and only then over ALL VALUES (keys sorted). Not key,value,key,value -- and the
 *   difference is invisible until a nested plist is compared byte for byte.
 *
 *   DEDUPLICATION. plistlib keeps an object table and reuses the ref for an identical
 *   scalar. Two occurrences of "M5 Badge" are ONE object referenced twice. Without this
 *   the object count differs, which moves the offset table, which moves the trailer.
 *
 * Both are implemented below and both are pinned by goldens in tools/test-bplist.sh.
 */

#define AD_BPW_NODES 64 /* objects, including dedup: refs stay one byte (<=255) */
#define AD_BPW_KIDS 128 /* child slots across all containers                    */

enum { AD_BPW_STR = 0, AD_BPW_DATA, AD_BPW_INT, AD_BPW_BOOL, AD_BPW_DICT, AD_BPW_ARRAY };

struct AdBpwNode {
  uint8_t type;
  const uint8_t* p; /* STR / DATA: borrowed, must outlive ad_bpw_finish() */
  uint32_t len;
  int64_t num;    /* INT / BOOL                                          */
  int kid0, nkid; /* DICT / ARRAY: slice of w->kid[]                      */
};

struct AdBpw {
  struct AdBpwNode node[AD_BPW_NODES];
  int kid[AD_BPW_KIDS];
  int n_nodes, n_kids;
  bool overflow; /* sticky: one check at finish() instead of at every call */
};

static void ad_bpw_init(struct AdBpw* w) {
  w->n_nodes = 0;
  w->n_kids = 0;
  w->overflow = false;
}

static int ad_bpw_new(struct AdBpw* w, uint8_t type) {
  if (w->n_nodes >= AD_BPW_NODES) {
    w->overflow = true;
    return -1;
  }
  int i = w->n_nodes++;
  struct AdBpwNode* n = &w->node[i];
  n->type = type;
  n->p = NULL;
  n->len = 0;
  n->num = 0;
  n->kid0 = 0;
  n->nkid = 0;
  return i;
}

/* Scalars are DEDUPLICATED, because plistlib deduplicates and a golden is a byte
   comparison. Linear search over at most AD_BPW_NODES entries; the graphs here are tens
   of objects, and a hash table would be more code than the thing it speeds up. */
static int ad_bpw_find(struct AdBpw* w, uint8_t type, const uint8_t* p, uint32_t len, int64_t num) {
  for (int i = 0; i < w->n_nodes; i++) {
    const struct AdBpwNode* n = &w->node[i];
    if (n->type != type) continue;
    if (type == AD_BPW_INT || type == AD_BPW_BOOL) {
      if (n->num == num) return i;
      continue;
    }
    if (n->len == len && (len == 0 || memcmp(n->p, p, len) == 0)) return i;
  }
  return -1;
}

static int ad_bpw_blob(struct AdBpw* w, uint8_t type, const void* p, uint32_t len) {
  int i = ad_bpw_find(w, type, (const uint8_t*)p, len, 0);
  if (i >= 0) return i;
  i = ad_bpw_new(w, type);
  if (i < 0) return -1;
  w->node[i].p = (const uint8_t*)p;
  w->node[i].len = len;
  return i;
}

/* `s` is BORROWED and must stay alive until ad_bpw_finish() returns. */
static int ad_bpw_str(struct AdBpw* w, const char* s) { return ad_bpw_blob(w, AD_BPW_STR, s, (uint32_t)strlen(s)); }
static int ad_bpw_strn(struct AdBpw* w, const char* s, uint32_t n) { return ad_bpw_blob(w, AD_BPW_STR, s, n); }
static int ad_bpw_data(struct AdBpw* w, const void* p, uint32_t n) { return ad_bpw_blob(w, AD_BPW_DATA, p, n); }
static int ad_bpw_int(struct AdBpw* w, int64_t v) {
  int i = ad_bpw_find(w, AD_BPW_INT, NULL, 0, v);
  if (i >= 0) return i;
  i = ad_bpw_new(w, AD_BPW_INT);
  if (i >= 0) w->node[i].num = v;
  return i;
}
static int ad_bpw_bool(struct AdBpw* w, bool v) {
  int i = ad_bpw_find(w, AD_BPW_BOOL, NULL, 0, v ? 1 : 0);
  if (i >= 0) return i;
  i = ad_bpw_new(w, AD_BPW_BOOL);
  if (i >= 0) w->node[i].num = v ? 1 : 0;
  return i;
}
static int ad_bpw_dict(struct AdBpw* w) { return ad_bpw_new(w, AD_BPW_DICT); }
static int ad_bpw_array(struct AdBpw* w) { return ad_bpw_new(w, AD_BPW_ARRAY); }

/* Children of one container must be appended CONSECUTIVELY: kid0/nkid is a slice, not a
   list. Interleaving two open containers silently interleaves their members, so this
   refuses rather than producing a plist whose shape is not the one the caller wrote. */
static bool ad_bpw_kid(struct AdBpw* w, int parent, int child) {
  if (parent < 0 || child < 0 || w->overflow) {
    w->overflow = true;
    return false;
  }
  if (w->n_kids >= AD_BPW_KIDS) {
    w->overflow = true;
    return false;
  }
  struct AdBpwNode* n = &w->node[parent];
  if (n->nkid == 0)
    n->kid0 = w->n_kids;
  else if (n->kid0 + n->nkid != w->n_kids) {
    w->overflow = true;
    return false;
  }
  w->kid[w->n_kids++] = child;
  n->nkid++;
  return true;
}
/* A dict stores key,value,key,value ... in kid[]; the emitter separates them. */
static bool ad_bpw_set(struct AdBpw* w, int dict, const char* key, int val) {
  int k = ad_bpw_str(w, key);
  return ad_bpw_kid(w, dict, k) && ad_bpw_kid(w, dict, val);
}
static bool ad_bpw_push(struct AdBpw* w, int arr, int val) { return ad_bpw_kid(w, arr, val); }

/* --- serialise -------------------------------------------------------------------
 *
 * plistlib's _flatten, reproduced: append the object, then recurse over ALL KEYS and only
 * then over ALL VALUES. The ref of an object is its position in that walk, so getting the
 * order wrong renumbers everything downstream of the first container.
 *
 * Keys are sorted here, as plistlib does with sort_keys=True (its default) -- and as
 * ad_bplist_dict() above already does, for the same reason: a caller cannot be expected to
 * know that the format's canonical form is sorted.
 */
static void ad_bpw_flat(struct AdBpw* w, int node, int* order, int* n, int* ref) {
  if (node < 0 || *n >= AD_BPW_NODES) return;
  if (ref[node] >= 0) return; /* already placed: dedup, exactly as plistlib */
  ref[node] = (*n);
  order[(*n)++] = node;
  const struct AdBpwNode* c = &w->node[node];
  if (c->type == AD_BPW_DICT) {
    int np = c->nkid / 2;
    for (int i = 0; i < np; i++) ad_bpw_flat(w, w->kid[c->kid0 + 2 * i], order, n, ref);
    for (int i = 0; i < np; i++) ad_bpw_flat(w, w->kid[c->kid0 + 2 * i + 1], order, n, ref);
  } else if (c->type == AD_BPW_ARRAY) {
    for (int i = 0; i < c->nkid; i++) ad_bpw_flat(w, w->kid[c->kid0 + i], order, n, ref);
  }
}

/* Sort a dict's pairs by key. Insertion sort over the kid[] slice, two slots at a time. */
static void ad_bpw_sort(struct AdBpw* w, int node) {
  struct AdBpwNode* c = &w->node[node];
  if (c->type != AD_BPW_DICT) return;
  int np = c->nkid / 2;
  for (int i = 1; i < np; i++) {
    int k = w->kid[c->kid0 + 2 * i], v = w->kid[c->kid0 + 2 * i + 1];
    const struct AdBpwNode* kn = &w->node[k];
    int j = i;
    while (j > 0) {
      const struct AdBpwNode* pn = &w->node[w->kid[c->kid0 + 2 * (j - 1)]];
      uint32_t m = pn->len < kn->len ? pn->len : kn->len;
      int cmp = m ? memcmp(pn->p, kn->p, m) : 0;
      if (cmp == 0) cmp = (pn->len < kn->len) ? -1 : (pn->len > kn->len) ? 1 : 0;
      if (cmp <= 0) break;
      w->kid[c->kid0 + 2 * j] = w->kid[c->kid0 + 2 * (j - 1)];
      w->kid[c->kid0 + 2 * j + 1] = w->kid[c->kid0 + 2 * (j - 1) + 1];
      j--;
    }
    w->kid[c->kid0 + 2 * j] = k;
    w->kid[c->kid0 + 2 * j + 1] = v;
  }
}

/* token|size, with plistlib's escape forms for size >= 15. */
static uint8_t* ad_bpw_size(uint8_t* p, uint8_t token, uint32_t size) {
  if (size < 15) {
    *p++ = (uint8_t)(token | size);
    return p;
  }
  *p++ = (uint8_t)(token | 0x0F);
  if (size < 0x100) {
    *p++ = 0x10;
    *p++ = (uint8_t)size;
  } else if (size < 0x10000) {
    *p++ = 0x11;
    *p++ = (uint8_t)(size >> 8);
    *p++ = (uint8_t)size;
  } else {
    *p++ = 0x12;
    for (int i = 3; i >= 0; i--) *p++ = (uint8_t)(size >> (8 * i));
  }
  return p;
}
static uint8_t* ad_bpw_ref(uint8_t* p, int r, int rs) {
  for (int i = rs - 1; i >= 0; i--) *p++ = (uint8_t)((unsigned)r >> (8 * i));
  return p;
}

/* Returns bytes written, or 0 on any refusal: pool overflow, a container assembled
 * non-consecutively, more than 255 objects, a value too long, or no room. Never a
 * truncated plist -- a short plist is not a smaller plist, it is a broken one. */
static uint32_t ad_bpw_finish(struct AdBpw* w, int root, uint8_t* out, uint32_t cap) {
  if (w->overflow || root < 0 || cap < 48) return 0;
  for (int i = 0; i < w->n_nodes; i++) ad_bpw_sort(w, i);

  int ref[AD_BPW_NODES], order[AD_BPW_NODES], n = 0;
  for (int i = 0; i < w->n_nodes; i++) ref[i] = -1;
  ad_bpw_flat(w, root, order, &n, ref);
  if (n == 0 || n > 255) return 0; /* one-byte refs only; see the header */
  const int rs = 1;

  uint32_t off[AD_BPW_NODES];
  uint8_t *p = out, *end = out + cap;
  memcpy(p, "bplist00", 8);
  p += 8;

  for (int i = 0; i < n; i++) {
    const struct AdBpwNode* c = &w->node[order[i]];
    off[i] = (uint32_t)(p - out);
    if (end - p < 16) return 0;
    switch (c->type) {
      case AD_BPW_BOOL:
        *p++ = c->num ? 0x09 : 0x08;
        break;
      case AD_BPW_INT: {
        if (c->num < 0) return 0; /* see ad_bplist_dict's note */
        int64_t x = c->num;
        int wd = (x <= 0xffLL) ? 1 : (x <= 0xffffLL) ? 2 : (x <= 0xffffffffLL) ? 4 : 8;
        *p++ = (uint8_t)(0x10 | (wd == 1 ? 0 : wd == 2 ? 1 : wd == 4 ? 2 : 3));
        for (int k = wd - 1; k >= 0; k--) *p++ = (uint8_t)((uint64_t)x >> (8 * k));
        break;
      }
      case AD_BPW_DATA:
        if (end - p < (ptrdiff_t)(6 + c->len)) return 0;
        p = ad_bpw_size(p, 0x40, c->len);
        memcpy(p, c->p, c->len);
        p += c->len;
        break;
      case AD_BPW_STR: {
        bool ascii = true;
        for (uint32_t k = 0; k < c->len; k++)
          if (c->p[k] & 0x80) {
            ascii = false;
            break;
          }
        if (ascii) {
          if (end - p < (ptrdiff_t)(6 + c->len)) return 0;
          p = ad_bpw_size(p, 0x50, c->len);
          memcpy(p, c->p, c->len);
          p += c->len;
        } else {
          int units = ad_bp_utf16_len(c->p, c->len);
          if (units < 0) return 0;
          if (end - p < (ptrdiff_t)(6 + 2 * units)) return 0;
          p = ad_bpw_size(p, 0x60, (uint32_t)units);
          p = ad_bp_put_utf16(p, c->p, c->len);
        }
        break;
      }
      case AD_BPW_DICT: {
        int np = c->nkid / 2;
        if (end - p < (ptrdiff_t)(6 + 2 * np * rs)) return 0;
        p = ad_bpw_size(p, 0xD0, (uint32_t)np);
        for (int k = 0; k < np; k++) p = ad_bpw_ref(p, ref[w->kid[c->kid0 + 2 * k]], rs);
        for (int k = 0; k < np; k++) p = ad_bpw_ref(p, ref[w->kid[c->kid0 + 2 * k + 1]], rs);
        break;
      }
      case AD_BPW_ARRAY:
        if (end - p < (ptrdiff_t)(6 + c->nkid * rs)) return 0;
        p = ad_bpw_size(p, 0xA0, (uint32_t)c->nkid);
        for (int k = 0; k < c->nkid; k++) p = ad_bpw_ref(p, ref[w->kid[c->kid0 + k]], rs);
        break;
      default:
        return 0;
    }
  }

  uint32_t table = (uint32_t)(p - out);
  const int os = (table < 0x100) ? 1 : (table < 0x10000) ? 2 : 4;
  if (end - p < (ptrdiff_t)(n * os + 32)) return 0;
  for (int i = 0; i < n; i++)
    for (int k = os - 1; k >= 0; k--) *p++ = (uint8_t)(off[i] >> (8 * k));

  memset(p, 0, 5);
  p += 5;
  *p++ = 0;           /* sort version   */
  *p++ = (uint8_t)os; /* offset int size */
  *p++ = (uint8_t)rs; /* object ref size */
  /* 64-bit big-endian, and the shift is done on a uint64_t: shifting a uint32_t by 56 is
     UNDEFINED and does not fail loudly -- see the note in ad_bplist_dict(). */
  {
    uint64_t n64 = (uint64_t)n, t64 = table;
    for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(n64 >> (8 * i));
    for (int i = 0; i < 8; i++) *p++ = 0; /* root object = 0 */
    for (int i = 7; i >= 0; i--) *p++ = (uint8_t)(t64 >> (8 * i));
  }
  return (uint32_t)(p - out);
}
