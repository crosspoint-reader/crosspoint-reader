#include "ArabicShaper.h"
#include <Memory.h>
#include <Logging.h>
#include <Utf8.h>

// ---------------------------------------------------------------------------
// Joining-type constants (UAX#9 §9.2)
// ---------------------------------------------------------------------------
static constexpr uint8_t JT_U = 0;  // Non-joining  (ء, digits, punctuation)
static constexpr uint8_t JT_R = 1;  // Right-joining (ا, و, د, ذ, ر, ز, ة…)
static constexpr uint8_t JT_D = 2;  // Dual-joining  (ب, ت, ث, ج, … ي)
static constexpr uint8_t JT_T = 3;  // Transparent   (harakat, shadda, …)
static constexpr uint8_t JT_C = 4;  // Join-causing  (tatweel U+0640)

// Presentation-form index offsets stored at isolatedForm + offset:
//   0 = isolated, 1 = final, 2 = initial, 3 = medial
static constexpr uint8_t FORM_ISOLATED = 0;
static constexpr uint8_t FORM_FINAL    = 1;
static constexpr uint8_t FORM_INITIAL  = 2;
static constexpr uint8_t FORM_MEDIAL   = 3;

// ---------------------------------------------------------------------------
// Per-character info: base codepoint → presentation forms
// ---------------------------------------------------------------------------
struct CharInfo {
    uint32_t cp;            // Base Unicode codepoint
    uint8_t  joinType;      // JT_U / JT_R / JT_D / JT_C
    uint16_t isolatedForm;  // First presentation-form codepoint (isolated)
    uint8_t  numForms;      // 1=iso only, 2=iso+fin, 4=all four forms
};

// Table must be sorted ascending by cp (binary search).
//
// Standard Arabic (U+0621-U+064A) → Presentation Forms-B (U+FE80-U+FEF4)
// Extended Arabic (common in Persian/Urdu) → Presentation Forms-A (U+FB50-U+FBFF)
static constexpr CharInfo kArabicChars[] = {
    // cp       jt     isoForm   n
    {0x0621, JT_U, 0xFE80, 1},  // HAMZA ء
    {0x0622, JT_R, 0xFE81, 2},  // ALEF WITH MADDA ABOVE آ
    {0x0623, JT_R, 0xFE83, 2},  // ALEF WITH HAMZA ABOVE أ
    {0x0624, JT_R, 0xFE85, 2},  // WAW WITH HAMZA ABOVE ؤ
    {0x0625, JT_R, 0xFE87, 2},  // ALEF WITH HAMZA BELOW إ
    {0x0626, JT_D, 0xFE89, 4},  // YEH WITH HAMZA ABOVE ئ
    {0x0627, JT_R, 0xFE8D, 2},  // ALEF ا
    {0x0628, JT_D, 0xFE8F, 4},  // BEH ب
    {0x0629, JT_R, 0xFE93, 2},  // TEH MARBUTA ة
    {0x062A, JT_D, 0xFE95, 4},  // TEH ت
    {0x062B, JT_D, 0xFE99, 4},  // THEH ث
    {0x062C, JT_D, 0xFE9D, 4},  // JEEM ج
    {0x062D, JT_D, 0xFEA1, 4},  // HAH ح
    {0x062E, JT_D, 0xFEA5, 4},  // KHAH خ
    {0x062F, JT_R, 0xFEA9, 2},  // DAL د
    {0x0630, JT_R, 0xFEAB, 2},  // THAL ذ
    {0x0631, JT_R, 0xFEAD, 2},  // REH ر
    {0x0632, JT_R, 0xFEAF, 2},  // ZAIN ز
    {0x0633, JT_D, 0xFEB1, 4},  // SEEN س
    {0x0634, JT_D, 0xFEB5, 4},  // SHEEN ش
    {0x0635, JT_D, 0xFEB9, 4},  // SAD ص
    {0x0636, JT_D, 0xFEBD, 4},  // DAD ض
    {0x0637, JT_D, 0xFEC1, 4},  // TAH ط
    {0x0638, JT_D, 0xFEC5, 4},  // ZAH ظ
    {0x0639, JT_D, 0xFEC9, 4},  // AIN ع
    {0x063A, JT_D, 0xFECD, 4},  // GHAIN غ
    {0x0640, JT_C, 0x0640, 1},  // TATWEEL ـ (join-causing, no form substitution)
    {0x0641, JT_D, 0xFED1, 4},  // FA ف
    {0x0642, JT_D, 0xFED5, 4},  // QAF ق
    {0x0643, JT_D, 0xFED9, 4},  // KAF ك
    {0x0644, JT_D, 0xFEDD, 4},  // LAM ل
    {0x0645, JT_D, 0xFEE1, 4},  // MEEM م
    {0x0646, JT_D, 0xFEE5, 4},  // NOON ن
    {0x0647, JT_D, 0xFEE9, 4},  // HEH ه
    {0x0648, JT_R, 0xFEED, 2},  // WAW و
    // U+0649 ALEF MAKSURA: Unicode joining type is D but standard fonts only
    // include 2 presentation forms, so treat as R to avoid invalid form lookups.
    {0x0649, JT_R, 0xFEEF, 2},  // ALEF MAKSURA ى
    {0x064A, JT_D, 0xFEF1, 4},  // YEH ي
    // Extended Arabic – common in Persian, Urdu, Pashto
    // (requires font to include U+FB50-U+FBFF)
    {0x0671, JT_R, 0xFB50, 2},  // ALEF WASLA ٱ
    {0x067E, JT_D, 0xFB56, 4},  // PEH پ
    {0x0686, JT_D, 0xFB7A, 4},  // TCHEH چ
    {0x0688, JT_R, 0xFB88, 2},  // DDAL ڈ
    {0x0698, JT_R, 0xFB8A, 2},  // JEH ژ
    {0x06A4, JT_D, 0xFB6A, 4},  // VEH ڤ
    {0x06A9, JT_D, 0xFB8E, 4},  // KEHEH ک
    {0x06AF, JT_D, 0xFB92, 4},  // GAF گ
    {0x06BA, JT_R, 0xFB9E, 2},  // NOON GHUNNA ں
    {0x06BE, JT_D, 0xFBAA, 4},  // HEH DOACHASHMEE ھ
    {0x06CC, JT_D, 0xFBFC, 4},  // FARSI YEH ی
    {0x06D2, JT_R, 0xFBAE, 2},  // YEH BARREE ے
};
static constexpr size_t kArabicCharsCount = sizeof(kArabicChars) / sizeof(kArabicChars[0]);

// Lam-Alef mandatory ligature mapping: (alef_cp → ligature base).
// Ligature base + 0 = isolated form; base + 1 = final form.
struct LamAlefEntry { uint32_t alefCp; uint16_t ligBase; };
static constexpr LamAlefEntry kLamAlef[] = {
    {0x0622, 0xFEF5},  // LAM + ALEF WITH MADDA  → ﻵ / ﻶ
    {0x0623, 0xFEF7},  // LAM + ALEF WITH HAMZA  → ﻷ / ﻸ
    {0x0625, 0xFEF9},  // LAM + ALEF HAMZA BELOW → ﻹ / ﻺ
    {0x0627, 0xFEFB},  // LAM + ALEF             → ﻻ / ﻼ
};
static constexpr size_t kLamAlefCount = sizeof(kLamAlef) / sizeof(kLamAlef[0]);

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

static const CharInfo* findCharInfo(uint32_t cp) {
    // Binary search in kArabicChars (sorted ascending by cp)
    int lo = 0, hi = static_cast<int>(kArabicCharsCount) - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cp == kArabicChars[mid].cp) return &kArabicChars[mid];
        if (cp < kArabicChars[mid].cp) hi = mid - 1;
        else                           lo = mid + 1;
    }
    return nullptr;
}

// Returns the effective joining type of a codepoint for shaping purposes.
// Transparent characters (harakat) do not break joining chains.
static uint8_t joinType(uint32_t cp) {
    const CharInfo* info = findCharInfo(cp);
    if (info) return info->joinType;

    // Arabic combining diacritics (harakat, shadda, etc.): transparent
    if ((cp >= 0x064B && cp <= 0x065F) ||  // harakat + shadda + misc
        (cp >= 0x0610 && cp <= 0x061A) ||  // Arabic extended marks
        (cp >= 0x06D6 && cp <= 0x06DC) ||  // Quranic combining marks
        (cp >= 0x06DF && cp <= 0x06E4) ||  // more Quranic marks
        (cp >= 0x06E7 && cp <= 0x06E8) ||  // more Quranic marks
        (cp >= 0x06EA && cp <= 0x06ED) ||  // more Quranic marks
        cp == 0x0670) {                    // Arabic superscript alef
        return JT_T;
    }

    // Everything else is non-joining (including Arabic digits, punctuation)
    return JT_U;
}

// Returns true if the codepoint is in the Arabic block or extended Arabic
// (i.e., a character that belongs to Arabic script shaping).
static bool isArabicScript(uint32_t cp) {
    return (cp >= 0x0600 && cp <= 0x06FF);
}

// Find the lam-alef ligature base for a given alef codepoint.
// Returns 0 if no ligature exists.
static uint16_t findLamAlefLig(uint32_t alefCp) {
    for (size_t i = 0; i < kLamAlefCount; ++i) {
        if (kLamAlef[i].alefCp == alefCp) return kLamAlef[i].ligBase;
    }
    return 0;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

namespace ArabicShaper {

const char* shape(const char* text, std::string& shaped) {
    if (!text || !*text) return text;

    // Fast path: check for Arabic UTF-8 lead bytes (U+0600-U+07FF → 0xD8-0xDF).
    // We only shape when base Arabic characters are present.
    bool hasArabic = false;
    for (const unsigned char* q = reinterpret_cast<const unsigned char*>(text); *q; ++q) {
        if (*q >= 0xD8 && *q <= 0xDF) {
            hasArabic = true;
            break;
        }
    }
    if (!hasArabic) return text;

    // Decode input into a codepoint array (needed for look-ahead / look-behind).
    size_t count = 0;
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
        while (utf8NextCodepoint(&p)) ++count;
    }
    if (count == 0) return text;

    auto cps = makeUniqueNoThrow<uint32_t[]>(count);
    if (!cps) {
        LOG_ERR("ARABIC", "OOM: %zu codepoints", count);
        return text;
    }
    {
        const unsigned char* p = reinterpret_cast<const unsigned char*>(text);
        for (size_t i = 0; i < count; ++i) cps[i] = utf8NextCodepoint(&p);
    }

    shaped.clear();
    shaped.reserve(count * 3);  // Arabic presentation forms: 3 bytes each in UTF-8

    // prevEffJT: joining type of the most recent non-transparent Arabic character
    // (reset to JT_U on any non-Arabic character or at start).
    uint8_t prevEffJT = JT_U;

    for (size_t i = 0; i < count; ++i) {
        const uint32_t cp = cps[i];

        // Non-Arabic: pass through and reset the joining chain.
        if (!isArabicScript(cp)) {
            utf8AppendCodepoint(cp, shaped);
            prevEffJT = JT_U;
            continue;
        }

        const uint8_t jt = joinType(cp);

        // Transparent (harakat/diacritics): pass through, do NOT update prevEffJT.
        if (jt == JT_T) {
            utf8AppendCodepoint(cp, shaped);
            continue;
        }

        // Non-joining Arabic (U): output isolated form if available, reset chain.
        if (jt == JT_U) {
            const CharInfo* info = findCharInfo(cp);
            utf8AppendCodepoint(info ? static_cast<uint32_t>(info->isolatedForm) : cp, shaped);
            prevEffJT = JT_U;
            continue;
        }

        // Find the next effective (non-transparent) codepoint type.
        uint8_t nextEffJT = JT_U;
        for (size_t j = i + 1; j < count; ++j) {
            if (!isArabicScript(cps[j])) break;  // non-Arabic breaks the chain
            const uint8_t njt = joinType(cps[j]);
            if (njt != JT_T) { nextEffJT = njt; break; }
        }

        // Determine joining on each side (UAX#9 §9.2):
        //   joins_right = can connect to the preceding character (right in visual RTL)
        //   joins_left  = can connect to the following character (left in visual RTL)
        const bool canRight = (jt == JT_D || jt == JT_R || jt == JT_C);
        const bool canLeft  = (jt == JT_D || jt == JT_C);
        const bool prevJoins = (prevEffJT == JT_D || prevEffJT == JT_C);  // prev can extend left
        const bool nextJoins = (nextEffJT == JT_D || nextEffJT == JT_R || nextEffJT == JT_C);

        const bool joinsRight = canRight && prevJoins;
        const bool joinsLeft  = canLeft  && nextJoins;

        // --- Lam-Alef mandatory ligature ---
        // When LAM (U+0644) can connect to the left (joinsLeft) and the next
        // non-transparent character is an Alef variant, emit the combined ligature.
        if (cp == 0x0644 && joinsLeft) {
            // Find the next non-transparent codepoint and its index.
            uint32_t nextCp = 0;
            size_t   nextIdx = count;  // index of the alef
            for (size_t j = i + 1; j < count; ++j) {
                if (!isArabicScript(cps[j])) break;
                if (joinType(cps[j]) != JT_T) { nextCp = cps[j]; nextIdx = j; break; }
            }

            const uint16_t ligBase = findLamAlefLig(nextCp);
            if (ligBase != 0) {
                // joinsRight == true → LAM has a right connection → use FINAL ligature
                // joinsRight == false → LAM is at start of word → use ISOLATED ligature
                const uint32_t ligCp = ligBase + (joinsRight ? 1u : 0u);
                utf8AppendCodepoint(ligCp, shaped);

                // Output any transparent characters that were between LAM and ALEF.
                for (size_t j = i + 1; j < nextIdx; ++j) {
                    utf8AppendCodepoint(cps[j], shaped);
                }

                // Skip to alef (loop increment will advance past it).
                i = nextIdx;

                // The alef is R-joining; after the ligature the chain is broken
                // (alef cannot connect to the next character).
                prevEffJT = JT_R;
                continue;
            }
        }

        // --- Normal form selection ---
        uint8_t form;
        if      (joinsRight && joinsLeft) form = FORM_MEDIAL;
        else if (joinsRight)              form = FORM_FINAL;
        else if (joinsLeft)               form = FORM_INITIAL;
        else                              form = FORM_ISOLATED;

        // Tatweel: always output as-is (join-causing, no form substitution).
        if (jt == JT_C) {
            utf8AppendCodepoint(cp, shaped);
        } else {
            const CharInfo* info = findCharInfo(cp);
            if (info && form < info->numForms) {
                utf8AppendCodepoint(info->isolatedForm + form, shaped);
            } else {
                utf8AppendCodepoint(cp, shaped);  // fallback: base codepoint
            }
        }

        prevEffJT = jt;
    }

    return shaped.c_str();
}

}  // namespace ArabicShaper
