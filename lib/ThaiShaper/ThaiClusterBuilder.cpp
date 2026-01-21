#include "ThaiClusterBuilder.h"

#include <Utf8.h>

// Debug logging for Thai rendering investigation
// Set to 1 to enable verbose cluster building logging
#define THAI_CLUSTER_DEBUG_LOGGING 0

#if THAI_CLUSTER_DEBUG_LOGGING
#include <Arduino.h>
#endif

namespace ThaiShaper {

bool ThaiClusterBuilder::isAscenderConsonant(uint32_t cp) {
  // Thai consonants with tall ascenders that may affect mark positioning
  // These have parts that extend above the normal consonant height
  switch (cp) {
    case 0x0E1B:  // PO PLA (ป)
    case 0x0E1D:  // FO FA (ฝ)
    case 0x0E1F:  // FO FAN (ฟ)
    case 0x0E2C:  // LO CHULA (ฬ)
      return true;
    default:
      return false;
  }
}

bool ThaiClusterBuilder::isDescenderConsonant(uint32_t cp) {
  // Thai consonants with descenders that extend below the baseline
  // These may affect below-vowel positioning
  switch (cp) {
    case 0x0E0E:  // DO CHADA (ฎ)
    case 0x0E0F:  // TO PATAK (ฏ)
    case 0x0E24:  // RU (ฤ)
    case 0x0E26:  // LU (ฦ)
      return true;
    default:
      return false;
  }
}

std::vector<ThaiCluster> ThaiClusterBuilder::buildClusters(const char* text) {
  std::vector<ThaiCluster> clusters;

  if (text == nullptr || *text == '\0') {
    return clusters;
  }

#if THAI_CLUSTER_DEBUG_LOGGING
  Serial.printf("[THAI] buildClusters input bytes: ");
  const uint8_t* debugPtr = reinterpret_cast<const uint8_t*>(text);
  for (int i = 0; i < 32 && debugPtr[i] != '\0'; i++) {
    Serial.printf("%02X ", debugPtr[i]);
  }
  Serial.printf("\n");
#endif

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text);

  while (*ptr != '\0') {
    ThaiCluster cluster = buildNextCluster(&ptr);
    if (!cluster.glyphs.empty()) {
      clusters.push_back(std::move(cluster));
    }
  }

#if THAI_CLUSTER_DEBUG_LOGGING
  Serial.printf("[THAI] Built %zu clusters\n", clusters.size());
#endif

  return clusters;
}

ThaiCluster ThaiClusterBuilder::buildNextCluster(const uint8_t** text) {
  ThaiCluster cluster;

  if (*text == nullptr || **text == '\0') {
    return cluster;
  }

#if THAI_CLUSTER_DEBUG_LOGGING
  // Log raw bytes at current position
  Serial.printf("[THAI] buildNextCluster at ptr=%p, bytes: ", (void*)*text);
  for (int i = 0; i < 6 && (*text)[i] != '\0'; i++) {
    Serial.printf("%02X ", (*text)[i]);
  }
  Serial.printf("\n");
#endif

  // Peek at first codepoint to determine cluster type
  const uint8_t* peekPtr = *text;
  uint32_t firstCp = utf8NextCodepoint(&peekPtr);

#if THAI_CLUSTER_DEBUG_LOGGING
  Serial.printf("[THAI] First codepoint: U+%04X\n", firstCp);
#endif

  // Non-Thai character: return as single-glyph cluster
  if (!isThaiCodepoint(firstCp)) {
    utf8NextCodepoint(text);  // Consume the codepoint
    PositionedGlyph glyph;
    glyph.codepoint = firstCp;
    glyph.xOffset = 0;
    glyph.yOffset = 0;
    glyph.zeroAdvance = false;
    cluster.glyphs.push_back(glyph);
#if THAI_CLUSTER_DEBUG_LOGGING
    Serial.printf("[THAI] Non-Thai cluster: U+%04X\n", firstCp);
#endif
    return cluster;
  }

  // Collect all codepoints that form this Thai cluster
  uint32_t leadingVowel = 0;
  uint32_t baseConsonant = 0;
  uint32_t aboveVowel = 0;
  uint32_t belowVowel = 0;
  uint32_t toneMark = 0;
  uint32_t followVowel = 0;
  uint32_t thanthakhat = 0;  // ์ or ํ (nikhahit)

  // Parse the cluster: consume codepoints until we hit a cluster boundary
  while (**text != '\0') {
    peekPtr = *text;
    uint32_t cp = utf8NextCodepoint(&peekPtr);

    if (!isThaiCodepoint(cp)) {
      break;  // Non-Thai ends the cluster
    }

    ThaiCharType type = getThaiCharType(cp);

    switch (type) {
      case ThaiCharType::LEADING_VOWEL:
        if (leadingVowel != 0 || baseConsonant != 0) {
          // Another leading vowel or we already have base = new cluster
          goto done_parsing;
        }
        leadingVowel = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::CONSONANT:
        if (baseConsonant != 0) {
          // Second consonant = new cluster
          goto done_parsing;
        }
        baseConsonant = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::ABOVE_VOWEL:
        if (aboveVowel != 0) {
          // Multiple above vowels - take first, new cluster for next
          goto done_parsing;
        }
        aboveVowel = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::BELOW_VOWEL:
        if (belowVowel != 0) {
          goto done_parsing;
        }
        belowVowel = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::TONE_MARK:
        if (toneMark != 0) {
          goto done_parsing;
        }
        toneMark = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::FOLLOW_VOWEL:
        if (followVowel != 0) {
          goto done_parsing;
        }
        followVowel = cp;
        utf8NextCodepoint(text);
        // Follow vowel typically ends the syllable
        goto done_parsing;

      case ThaiCharType::NIKHAHIT:
      case ThaiCharType::YAMAKKAN:
        if (thanthakhat != 0) {
          goto done_parsing;
        }
        thanthakhat = cp;
        utf8NextCodepoint(text);
        break;

      case ThaiCharType::THAI_DIGIT:
      case ThaiCharType::THAI_SYMBOL:
        // Digits and symbols are standalone clusters
        if (leadingVowel == 0 && baseConsonant == 0) {
          // Start of cluster with digit/symbol
          utf8NextCodepoint(text);
          PositionedGlyph glyph;
          glyph.codepoint = cp;
          glyph.xOffset = 0;
          glyph.yOffset = 0;
          glyph.zeroAdvance = false;
          cluster.glyphs.push_back(glyph);
          return cluster;
        }
        // Otherwise end current cluster
        goto done_parsing;

      default:
        // Unknown Thai character - treat as cluster boundary
        goto done_parsing;
    }
  }

done_parsing:
  // Now build positioned glyphs from collected codepoints

  // 1. Leading vowel (if any) - rendered FIRST but stored after consonant in Unicode
  if (leadingVowel != 0) {
    PositionedGlyph glyph;
    glyph.codepoint = leadingVowel;
    glyph.xOffset = 0;
    glyph.yOffset = 0;
    glyph.zeroAdvance = false;  // Leading vowel has its own advance
    cluster.glyphs.push_back(glyph);
  }

  // 2. Base consonant
  if (baseConsonant != 0) {
    PositionedGlyph glyph;
    glyph.codepoint = baseConsonant;
    glyph.xOffset = 0;
    glyph.yOffset = 0;
    glyph.zeroAdvance = false;
    cluster.glyphs.push_back(glyph);

    // Check if this is an ascender consonant for mark positioning
    bool hasAscender = isAscenderConsonant(baseConsonant);

    // 3. Above vowel (positioned above base)
    if (aboveVowel != 0) {
      PositionedGlyph aboveGlyph;
      aboveGlyph.codepoint = aboveVowel;
      aboveGlyph.xOffset = hasAscender ? ThaiOffset::ASCENDER_X_SHIFT : 0;
      aboveGlyph.yOffset = ThaiOffset::ABOVE_VOWEL;
      aboveGlyph.zeroAdvance = true;  // Above vowel doesn't advance cursor
      cluster.glyphs.push_back(aboveGlyph);
    }

    // 4. Below vowel (positioned below base)
    if (belowVowel != 0) {
      PositionedGlyph belowGlyph;
      belowGlyph.codepoint = belowVowel;
      belowGlyph.xOffset = 0;
      belowGlyph.yOffset = ThaiOffset::BELOW_VOWEL;
      belowGlyph.zeroAdvance = true;
      cluster.glyphs.push_back(belowGlyph);
    }

    // 5. Tone mark (positioned above everything else)
    if (toneMark != 0) {
      PositionedGlyph toneGlyph;
      toneGlyph.codepoint = toneMark;
      toneGlyph.xOffset = hasAscender ? ThaiOffset::ASCENDER_X_SHIFT : 0;
      // Tone mark goes above above-vowel if present, otherwise just above base
      toneGlyph.yOffset = aboveVowel != 0 ? ThaiOffset::TONE_MARK : ThaiOffset::TONE_MARK_ALONE;
      toneGlyph.zeroAdvance = true;
      cluster.glyphs.push_back(toneGlyph);
    }

    // 6. Thanthakhat/Nikhahit (positioned above)
    if (thanthakhat != 0) {
      PositionedGlyph thanGlyph;
      thanGlyph.codepoint = thanthakhat;
      thanGlyph.xOffset = 0;
      // Position depends on what's already above
      if (toneMark != 0) {
        thanGlyph.yOffset = ThaiOffset::TONE_MARK - 2;  // Above tone mark
      } else if (aboveVowel != 0) {
        thanGlyph.yOffset = ThaiOffset::TONE_MARK;  // Above above-vowel
      } else {
        thanGlyph.yOffset = ThaiOffset::TONE_MARK_ALONE;
      }
      thanGlyph.zeroAdvance = true;
      cluster.glyphs.push_back(thanGlyph);
    }
  }

  // 7. Follow vowel (displayed after base)
  if (followVowel != 0) {
    PositionedGlyph glyph;
    glyph.codepoint = followVowel;
    glyph.xOffset = 0;
    glyph.yOffset = 0;
    glyph.zeroAdvance = false;  // Follow vowel advances cursor
    cluster.glyphs.push_back(glyph);
  }

  // Handle edge case: leading vowel with no consonant (shouldn't happen in valid Thai)
  if (leadingVowel != 0 && baseConsonant == 0) {
    // Just the leading vowel by itself
    // Already added above
  }

#if THAI_CLUSTER_DEBUG_LOGGING
  Serial.printf("[THAI] Cluster built with %zu glyphs: ", cluster.glyphs.size());
  for (const auto& g : cluster.glyphs) {
    Serial.printf("U+%04X ", g.codepoint);
  }
  Serial.printf("(lead=%04X base=%04X above=%04X below=%04X tone=%04X follow=%04X)\n",
                leadingVowel, baseConsonant, aboveVowel, belowVowel, toneMark, followVowel);
#endif

  return cluster;
}

}  // namespace ThaiShaper
