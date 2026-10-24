#pragma once

#include <cstdint>

// Character categories, visual positions, syllable types and per-script
// settings of the Indic shaper and the Universal Shaping Engine, with
// HarfBuzz's names and values (hb-ot-shaper-indic*.hh, hb-ot-shaper-use*.hh),
// which OtUnicodeData.h and OtSyllableMachines.h are generated with. The
// category names keep HarfBuzz's mixed case (MPst, Repha, ...) so they read
// the same as the grammars they come from.
namespace ot {

constexpr uint64_t flag(const unsigned n) { return 1ull << n; }

// Indic character categories (indic_syllable_machine_ex_*).
namespace icat {
constexpr uint8_t X = 0, C = 1, V = 2, N = 3, H = 4, ZWNJ = 5, ZWJ = 6, M = 7, SM = 8, A = 9, VD = 9, PLACEHOLDER = 10,
                  DOTTEDCIRCLE = 11, RS = 12, MPst = 13, Repha = 14, Ra = 15, CM = 16, Symbol = 17, CS = 18, SMPst = 57;
}

// Visual positions in an Indic syllable (ot_position_t).
namespace ipos {
constexpr uint8_t START = 0, RA_TO_BECOME_REPH = 1, PRE_M = 2, PRE_C = 3, BASE_C = 4, AFTER_MAIN = 5, ABOVE_C = 6,
                  BEFORE_SUB = 7, BELOW_C = 8, AFTER_SUB = 9, BEFORE_POST = 10, POST_C = 11, AFTER_POST = 12, SMVD = 13,
                  END = 14;
}

// Indic syllable types (indic_syllable_type_t).
namespace isyl {
constexpr uint8_t CONSONANT = 0, VOWEL = 1, STANDALONE = 2, SYMBOL = 3, BROKEN = 4, NON_INDIC = 5;
}

// USE categories (use_syllable_machine_ex_*).
namespace ucat {
constexpr uint8_t O = 0, B = 1, N = 4, GB = 5, CGJ = 6, SUB = 11, H = 12, HN = 13, ZWNJ = 14, WJ = 16, R = 18,
                  VPre = 22, VMPre = 23, FAbv = 24, FBlw = 25, FPst = 26, MAbv = 27, MBlw = 28, MPst = 29, MPre = 30,
                  CMAbv = 31, CMBlw = 32, VAbv = 33, VBlw = 34, VPst = 35, VMAbv = 37, VMBlw = 38, VMPst = 39,
                  SMAbv = 41, SMBlw = 42, CS = 43, IS = 44, FMAbv = 45, FMBlw = 46, FMPst = 47, Sk = 48, G = 49, J = 50,
                  SB = 51, SE = 52, HVM = 53, HM = 54, HR = 55, RK = 56;
}

// USE syllable types (use_syllable_type_t).
namespace usyl {
constexpr uint8_t VIRAMA_TERMINATED = 0, SAKOT_TERMINATED = 1, STANDARD = 2, NUMBER_JOINER_TERMINATED = 3, NUMERAL = 4,
                  SYMBOL = 5, HIEROGLYPH = 6, BROKEN = 7, NON_CLUSTER = 8;
}

// How an Indic script forms reph and below-base forms (indic_config_t).
struct IndicConfig {
  enum RephMode : uint8_t {
    REPH_IMPLICIT,   // Ra,H forms reph
    REPH_EXPLICIT,   // Ra,H,ZWJ forms reph
    REPH_LOG_REPHA,  // reph is encoded (Malayalam dot reph) and reordered
  };
  enum BlwfMode : uint8_t {
    BLWF_PRE_AND_POST,  // below-base forms before and after the base
    BLWF_POST_ONLY,     // below-base forms only after the base
  };
  uint32_t virama;
  uint8_t rephPos;  // the ipos:: position reph moves to
  RephMode rephMode;
  BlwfMode blwfMode;
};

// Per script, in Unicode block order from Devanagari to Malayalam. (Sinhala
// always shapes with the Universal Shaping Engine.)
inline constexpr IndicConfig INDIC_CONFIGS[] = {
    {0x094D, ipos::BEFORE_POST, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},  // Devanagari
    {0x09CD, ipos::AFTER_SUB, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},    // Bengali
    {0x0A4D, ipos::BEFORE_SUB, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},   // Gurmukhi
    {0x0ACD, ipos::BEFORE_POST, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},  // Gujarati
    {0x0B4D, ipos::AFTER_MAIN, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},   // Oriya
    {0x0BCD, ipos::AFTER_POST, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_PRE_AND_POST},   // Tamil
    {0x0C4D, ipos::AFTER_POST, IndicConfig::REPH_EXPLICIT, IndicConfig::BLWF_POST_ONLY},      // Telugu
    {0x0CCD, ipos::AFTER_POST, IndicConfig::REPH_IMPLICIT, IndicConfig::BLWF_POST_ONLY},      // Kannada
    {0x0D4D, ipos::AFTER_MAIN, IndicConfig::REPH_LOG_REPHA, IndicConfig::BLWF_PRE_AND_POST},  // Malayalam
};

}  // namespace ot
