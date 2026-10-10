#include "LanguageTag.h"

#include <LanguageCode.h>
#include <Utf8.h>

#include <cstdint>
#include <cstring>

#include "I18nKeys.h"

namespace {

static_assert(LANGUAGE_TAG_BUFFER_SIZE >= PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE,
              "a full tag buffer holds a primary subtag");

std::string_view firstSubtag(const std::string_view tags) { return tags.substr(0, tags.find_first_of("-_")); }

// The primary subtag is already lowercase; match the second without case sensitivity.
const char* distinguishedTag(const char* primary, const std::string_view second) {
  const size_t primaryLength = strlen(primary);
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    const char* candidate = LANGUAGE_BCP47[i];
    if (strncmp(candidate, primary, primaryLength) != 0 || candidate[primaryLength] != '-') continue;
    if (asciiEqualsIgnoreCase(second, candidate + primaryLength + 1)) return candidate;
  }
  return nullptr;
}

}  // namespace

bool normaliseLanguageTag(const std::string_view tag, char* out, const size_t outSize) {
  if (out == nullptr || outSize == 0) return false;
  out[0] = '\0';
  if (outSize < LANGUAGE_TAG_BUFFER_SIZE) return false;

  char primary[PRIMARY_LANGUAGE_SUBTAG_BUFFER_SIZE];
  std::string_view rest;
  if (!normalisePrimaryLanguageSubtag(tag, primary, sizeof(primary), &rest)) return false;

  const std::string_view second = firstSubtag(rest);
  const char* distinguished = second.empty() ? nullptr : distinguishedTag(primary, second);
  const char* result = distinguished != nullptr && strlen(distinguished) < outSize ? distinguished : primary;
  strncpy(out, result, outSize - 1);
  out[outSize - 1] = '\0';
  return true;
}

const char* languageNameForTag(const char* tag) {
  if (tag == nullptr || tag[0] == '\0') return nullptr;
  for (uint8_t i = 0; i < getLanguageCount(); i++) {
    if (strcmp(tag, LANGUAGE_BCP47[i]) == 0) return LANGUAGE_NAMES[i];
  }
  return nullptr;
}
