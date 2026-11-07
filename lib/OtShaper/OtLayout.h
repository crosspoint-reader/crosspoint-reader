#pragma once

#include <cstdint>
#include <vector>

#include "OtBuffer.h"
#include "OtFace.h"

// Applying GSUB and GPOS lookups (see OtLayoutInternal.h for how the work is
// split across files).
namespace ot {

// Largest value a feature can carry (HB_OT_MAP_MAX_VALUE); the 'rand'
// feature uses it to ask for a random alternate.
constexpr unsigned MAX_FEATURE_VALUE = 255;

// Per-lookup settings from the feature map.
struct LookupSettings {
  // Glyphs the lookup can start at (lookupDigest()); lets application skip
  // the others without reading the lookup's subtables.
  bool hasDigest = false;
  Digest digest;
  // Per subtable, the glyphs it can start at: DIGEST_BYTES-byte records.
  // nullptr: subtables are not filtered.
  const uint8_t* subtableDigests = nullptr;
  uint32_t mask = 0;
  bool autoZwnj = true;
  bool autoZwj = true;
  bool random = false;
  bool perSyllable = false;
};

// Union of the coverage of every subtable of a lookup: the glyphs it can
// start at.
Digest lookupDigest(const Face& face, int table, uint32_t lookupIndex);
// Appends the same digest for each subtable of the lookup, as records.
void appendSubtableDigests(const Face& face, int table, uint32_t lookupIndex, std::vector<uint8_t>& out);

// Applies one GSUB or GPOS lookup across the buffer. GPOS requires buffer.pos
// to be sized.
void applyLookup(const Face& face, const Scale& scale, Buffer& buffer, int table, uint32_t lookupIndex,
                 const LookupSettings& settings);

// Whether GSUB lookup `lookupIndex` would substitute `glyphs` (HarfBuzz's
// hb_ot_layout_lookup_would_substitute). `digest` is lookupDigest().
bool wouldSubstitute(const Face& face, uint32_t lookupIndex, const Digest& digest, const uint32_t* glyphs,
                     unsigned count, bool zeroContext);

// Joins mark and cursive attachment offsets once GPOS is done.
void propagateAttachmentOffsets(Buffer& buffer);

}  // namespace ot
