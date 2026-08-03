#pragma once

#include <cstddef>
#include <cstdint>

/// Thai word-boundary matching backed by the flash-resident ICU wordlist
/// (ThaiDictData.h, ~132 KB of static const data — DROM, no DRAM cost).
///
/// Used by layout code to break Thai runs at real word boundaries instead of
/// character-cluster boundaries. Callers walk a codepoint array left to right:
/// the longest dictionary word starting at each position wins; positions where
/// no word matches fall back to the caller's cluster rules.
namespace ThaiDict {

/// Longest dictionary word that is a prefix of cps[0..count).
/// Returns its length in codepoints, or 0 if no word matches.
/// Codepoints outside the Thai block never match.
size_t matchLongest(const uint32_t* cps, size_t count);

}  // namespace ThaiDict
