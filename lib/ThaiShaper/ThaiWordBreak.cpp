#include "ThaiWordBreak.h"

#include <Arduino.h>
#include <Utf8.h>

#include <cstring>

#include "ThaiCharacter.h"

namespace ThaiShaper {

size_t ThaiWordBreak::nextClusterBoundary(const char* text, size_t startOffset) {
  if (text == nullptr) {
    return 0;
  }

  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(text + startOffset);

  if (*ptr == '\0') {
    return startOffset;
  }

  // Get first codepoint
  const uint8_t* startPtr = ptr;
  uint32_t cp = utf8NextCodepoint(&ptr);

  // Non-Thai: just return next codepoint boundary
  if (!isThaiCodepoint(cp)) {
    return reinterpret_cast<const char*>(ptr) - text;
  }

  // For Thai leading vowels, include the following consonant and marks
  ThaiCharType type = getThaiCharType(cp);
  if (type == ThaiCharType::LEADING_VOWEL) {
    // Consume the leading vowel, continue to get consonant + marks
    if (*ptr != '\0') {
      cp = utf8NextCodepoint(&ptr);
    }
  }

  // Now consume any combining marks that follow
  while (*ptr != '\0') {
    const uint8_t* peekPtr = ptr;
    cp = utf8NextCodepoint(&peekPtr);

    if (!isThaiCodepoint(cp)) {
      break;
    }

    type = getThaiCharType(cp);

    // These types combine with the base - continue consuming
    if (type == ThaiCharType::ABOVE_VOWEL || type == ThaiCharType::BELOW_VOWEL || type == ThaiCharType::TONE_MARK ||
        type == ThaiCharType::NIKHAHIT || type == ThaiCharType::YAMAKKAN || type == ThaiCharType::FOLLOW_VOWEL) {
      ptr = peekPtr;
    } else {
      // New cluster starts (consonant, leading vowel, digit, etc.)
      break;
    }
  }

  return reinterpret_cast<const char*>(ptr) - text;
}

// Maximum size for the static text buffer used in segmentWords
// Thai EPUBs typically have words up to a few hundred bytes
static constexpr size_t MAX_SEGMENT_TEXT_SIZE = 2048;

// Static buffer to hold text copy - avoids heap corruption issues
// Safe because ESP32 runs single-threaded for this code path
static char s_segmentTextBuffer[MAX_SEGMENT_TEXT_SIZE];

std::vector<std::string> ThaiWordBreak::segmentWords(const char* text) {
  std::vector<std::string> segments;

  if (text == nullptr || *text == '\0') {
    return segments;
  }

  size_t textLen = strlen(text);

  // CRITICAL FIX: Copy input to STATIC buffer to avoid heap corruption.
  // On ESP32, heap allocations during string creation can corrupt the input
  // pointer's memory. Using a static buffer ensures the source data is
  // protected from heap fragmentation issues.
  if (textLen >= MAX_SEGMENT_TEXT_SIZE) {
    // Text too long for static buffer - truncate to prevent overflow
    textLen = MAX_SEGMENT_TEXT_SIZE - 1;
  }
  memcpy(s_segmentTextBuffer, text, textLen);
  s_segmentTextBuffer[textLen] = '\0';

  // Structure to hold segment boundaries (POD - no heap allocation)
  struct SegmentBounds {
    uint16_t offset;
    uint16_t length;
  };

  // Use a static array for bounds to avoid heap allocation during parsing
  static SegmentBounds s_bounds[512];
  size_t boundsCount = 0;

  size_t offset = 0;

  // Safety limit to prevent infinite loops
  size_t maxIterations = textLen + 1;
  size_t iterations = 0;

  // Phase 1: Collect all segment boundaries WITHOUT any heap allocation
  while (offset < textLen && iterations < maxIterations && boundsCount < 512) {
    iterations++;

    // Handle whitespace - preserve as separate segment
    if (s_segmentTextBuffer[offset] == ' ' || s_segmentTextBuffer[offset] == '\n' || s_segmentTextBuffer[offset] == '\t') {
      s_bounds[boundsCount++] = {static_cast<uint16_t>(offset), 1};
      offset++;
      continue;
    }

    // Get next cluster boundary
    size_t nextBoundary = nextClusterBoundary(s_segmentTextBuffer, offset);

    // Safety: ensure we always advance
    if (nextBoundary <= offset) {
      nextBoundary = offset + 1;
      // Skip to valid UTF-8 boundary
      while (nextBoundary < textLen && (s_segmentTextBuffer[nextBoundary] & 0xC0) == 0x80) {
        nextBoundary++;
      }
    }

    // Record segment bounds
    if (nextBoundary > offset) {
      s_bounds[boundsCount++] = {static_cast<uint16_t>(offset), static_cast<uint16_t>(nextBoundary - offset)};
    }

    offset = nextBoundary;
  }

  // Phase 2: Create strings from static buffer
  segments.reserve(boundsCount);
  for (size_t i = 0; i < boundsCount; i++) {
    segments.emplace_back(s_segmentTextBuffer + s_bounds[i].offset, s_bounds[i].length);
  }

  return segments;
}

}  // namespace ThaiShaper
