#include <ScriptDetector.h>

#include <cassert>

int main() {
  using ScriptDetector::containsArabic;
  using ScriptDetector::isArabicCodepoint;
  using ScriptDetector::isRtlLanguageTag;

  assert(isArabicCodepoint(0x0627));
  assert(isArabicCodepoint(0x0750));
  assert(isArabicCodepoint(0xFE8E));
  assert(!isArabicCodepoint('A'));
  assert(!isArabicCodepoint(0x4E00));

  assert(containsArabic(u8"السلام عليكم"));
  assert(containsArabic(u8"Latin ثم Arabic"));
  assert(containsArabic(u8"123 مرحبا"));
  assert(!containsArabic("Plain ASCII text"));
  assert(!containsArabic(u8"Русский текст"));
  assert(!containsArabic(nullptr));

  assert(isRtlLanguageTag("ar"));
  assert(isRtlLanguageTag("ar-SA"));
  assert(isRtlLanguageTag("fa"));
  assert(isRtlLanguageTag("ur_PK"));
  assert(!isRtlLanguageTag("en"));
  assert(!isRtlLanguageTag("as"));
  assert(!isRtlLanguageTag(nullptr));

  return 0;
}
