/* bidiclasses.t — bidi class table for CrossPoint Hebrew/English epub.
 *
 * Coverage rationale:
 *   Hebrew + English is the primary target. However, CrossPoint renders
 *   Latin and Cyrillic scripts for many other languages, so these MUST be
 *   classified as L (not fall through to ON) to avoid regression when they
 *   appear adjacent to Hebrew runs.
 *
 *   Scripts NOT in this table fall through to ON — correct per UAX#9 for
 *   scripts CrossPoint's fonts don't support (CJK, Arabic, Devanagari, etc.)
 *   ON is the right class for "unknown" — it behaves neutrally.
 *
 * Entries sorted ascending by first (binary search requirement).
 */

/* ── ASCII C0 controls ────────────────────────────────────────────────── */
{0x0000, 0x0008, BN},
{0x0009, 0x0009, S},
{0x000A, 0x000A, B},
{0x000B, 0x000B, S},
{0x000C, 0x000C, WS},
{0x000D, 0x000D, B},
{0x000E, 0x001B, BN},
{0x001C, 0x001E, B},
{0x001F, 0x001F, S},
{0x0020, 0x0020, WS},

/* ── ASCII punctuation: number-adjacent classes ─────────────────────── */
{0x0023, 0x0025, ET},   /* # $ % */
{0x002B, 0x002B, ES},   /* + */
{0x002C, 0x002C, CS},   /* , */
{0x002D, 0x002D, ES},   /* - */
{0x002E, 0x002F, CS},   /* . / */
{0x0030, 0x0039, EN},   /* 0-9 */
{0x003A, 0x003A, CS},   /* : */

/* ── Basic Latin letters ─────────────────────────────────────────────── */
{0x0041, 0x005A, L},    /* A-Z */
{0x0061, 0x007A, L},    /* a-z */

/* ── C1 / BN ────────────────────────────────────────────────────────── */
{0x007F, 0x0084, BN},
{0x0085, 0x0085, B},
{0x0086, 0x009F, BN},

/* ── Latin-1 supplement ─────────────────────────────────────────────── */
{0x00A0, 0x00A0, CS},   /* non-breaking space */
{0x00A2, 0x00A5, ET},   /* ¢ £ ¤ ¥ */
{0x00AA, 0x00AA, L},
{0x00AD, 0x00AD, BN},   /* soft hyphen */
{0x00B0, 0x00B1, ET},   /* ° ± */
{0x00B2, 0x00B3, EN},   /* ² ³ */
{0x00B5, 0x00B5, L},
{0x00B9, 0x00B9, EN},   /* ¹ */
{0x00BA, 0x00BA, L},
{0x00C0, 0x00D6, L},
{0x00D8, 0x00F6, L},
{0x00F8, 0x02B8, L},    /* Latin Extended-A/B, IPA, Spacing Modifiers
                           covers: Polish, Czech, Slovak, Turkish, etc. */

/* ── Combining Diacritical Marks (NSM) ──────────────────────────────── */
/* Needed for decomposed Latin characters (some epubs use NFD/NFKD form) */
{0x0300, 0x036F, NSM},

/* ── Cyrillic (L) ────────────────────────────────────────────────────── */
/* Required: CrossPoint supports Russian, Ukrainian, Bulgarian, etc.
   Without these, Cyrillic chars fall to ON, breaking mixed Hebrew+Russian. */
{0x0400, 0x04FF, L},    /* Cyrillic */
{0x0500, 0x052F, L},    /* Cyrillic Supplement */

/* ── Hebrew vowel points / cantillation (NSM) ───────────────────────── */
/* Do NOT remove: niqqud must be NSM or pointed Hebrew breaks after reorder */
{0x0591, 0x05A1, NSM},
{0x05A3, 0x05B9, NSM},
{0x05BB, 0x05BD, NSM},
{0x05BE, 0x05BE, R},    /* maqaf (Hebrew hyphen) */
{0x05BF, 0x05BF, NSM},
{0x05C0, 0x05C0, R},    /* paseq */
{0x05C1, 0x05C2, NSM},
{0x05C3, 0x05C3, R},    /* sof pasuq */
{0x05C4, 0x05C4, NSM},

/* ── Hebrew letters ─────────────────────────────────────────────────── */
{0x05D0, 0x05EA, R},    /* alef … tav */
{0x05F0, 0x05F4, R},    /* alternative forms + geresh/gershayim */

/* ── Latin Extended Additional (L) ─────────────────────────────────── */
/* Covers accented chars for Vietnamese, Welsh, Romanian, etc.
   Not currently rendered by CrossPoint fonts, but costs only 2 table rows. */
{0x1E00, 0x1EFF, L},

/* ── Unicode directional format characters ─────────────────────────── */
/* All must be present — UBA X-rules depend on them */
{0x200B, 0x200D, BN},   /* ZWSP, ZWNJ, ZWJ */
{0x200E, 0x200E, L},    /* LEFT-TO-RIGHT MARK */
{0x200F, 0x200F, R},    /* RIGHT-TO-LEFT MARK */
{0x2028, 0x2028, WS},
{0x2029, 0x2029, B},
{0x202A, 0x202A, LRE},
{0x202B, 0x202B, RLE},
{0x202C, 0x202C, PDF},
{0x202D, 0x202D, LRO},
{0x202E, 0x202E, RLO},
{0x202F, 0x202F, WS},   /* narrow no-break space */
{0x2060, 0x2063, BN},
/* Unicode 6.3 isolate markers */
{0x2066, 0x2066, LRI},
{0x2067, 0x2067, RLI},
{0x2068, 0x2068, FSI},
{0x2069, 0x2069, PDI},
{0x206A, 0x206F, BN},


/* ── Byte Order Mark ────────────────────────────────────────────────── */
{0xFEFF, 0xFEFF, BN},
