/* bidiclasses.t — bidi class table for CrossPoint epub rendering.
 *
 * Coverage rationale:
 *   Hebrew + English is the primary target. However, CrossPoint renders
 *   Latin and Cyrillic scripts for many other languages, so these MUST be
 *   classified as L (not fall through to ON) to avoid regression when they
 *   appear adjacent to Hebrew runs.
 *
 *   Arabic is fully classified (AL, NSM, AN) to enable correct RTL
 *   reordering and Arabic joining via ArabicShaper.  The Arabic Presentation
 *   Forms (U+FB50-U+FDFF, U+FE70-U+FEFE) produced by ArabicShaper are also
 *   classified as AL so that do_bidi() places them in the right visual order.
 *
 *   Scripts NOT in this table fall through to ON — correct per UAX#9.
 *
 * Entries MUST be sorted ascending by first (binary search requirement).
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

/* ── Arabic ──────────────────────────────────────────────────────────── */
/* All entries below are ordered ascending by first. */

/* Arabic number sign / sign sanah / footnote marker / sign safha (AN) */
{0x0600, 0x0605, AN},
/* Arabic extended non-spacing marks (harakat, shadda, ...)
   0x0606-0x060F fall through to ON (not letter/diacritic/number). */
{0x0610, 0x061A, NSM},
/* Arabic Letter Mark: BN per UAX#9 (invisible direction mark) */
{0x061C, 0x061C, BN},
/* Core Arabic letters: hamza (U+0620) through ghain (U+063F) */
{0x0620, 0x063F, AL},
/* Tatweel (U+0640) + Arabic letters fa through yeh (U+0641-U+064A) */
{0x0640, 0x064A, AL},
/* Arabic harakat / diacritics: transparent non-spacing marks */
{0x064B, 0x065F, NSM},
/* Arabic-Indic digits */
{0x0660, 0x0669, AN},
/* Arabic percent sign */
{0x066A, 0x066A, ET},
/* Arabic decimal separator + thousands separator (AN) */
{0x066B, 0x066C, AN},
/* Arabic five-pointed star + dotless letters */
{0x066D, 0x066F, AL},
/* Arabic superscript alef (non-spacing mark used over alef variants) */
{0x0670, 0x0670, NSM},
/* Extended Arabic letters (Persian/Urdu/Pashto): U+0671-U+06D5 */
{0x0671, 0x06D5, AL},
/* Quranic combined annotation marks (NSM) */
{0x06D6, 0x06DC, NSM},
/* End of Ayah (AN per UAX#9) */
{0x06DD, 0x06DD, AN},
/* Start of Rub el Hizb (ON) */
{0x06DE, 0x06DE, ON},
/* Quranic non-spacing marks */
{0x06DF, 0x06E4, NSM},
/* Arabic small medial Waw / Yeh */
{0x06E5, 0x06E6, AL},
/* Quranic non-spacing marks */
{0x06E7, 0x06E8, NSM},
/* Arabic place of sajdah (ON) */
{0x06E9, 0x06E9, ON},
/* Remaining Quranic non-spacing marks */
{0x06EA, 0x06ED, NSM},
/* Extended Arabic letters (Waw/Yeh variants) */
{0x06EE, 0x06EF, AL},
/* Extended Arabic-Indic digits */
{0x06F0, 0x06F9, AN},
/* Remaining extended Arabic letters through U+06FF */
{0x06FA, 0x06FF, AL},

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

/* ── Arabic Presentation Forms-A (U+FB50-U+FDFF) ───────────────────── */
/* Emitted by ArabicShaper for extended Arabic (Persian/Urdu/Pashto).
   Must be AL so do_bidi() reorders them correctly as RTL script.
   U+FD3E/FD3F (ornate parentheses) would ideally be ON, but they're
   uncommon in EPUBs and a single range is simpler on this microcontroller. */
{0xFB50, 0xFDFD, AL},

/* ── Arabic Presentation Forms-B (U+FE70-U+FEFE) ───────────────────── */
/* Emitted by ArabicShaper for standard Arabic letters. Must be AL.
   U+FEFF (BOM) is listed separately below as BN. */
{0xFE70, 0xFEFE, AL},

/* ── Byte Order Mark ────────────────────────────────────────────────── */
{0xFEFF, 0xFEFF, BN},
