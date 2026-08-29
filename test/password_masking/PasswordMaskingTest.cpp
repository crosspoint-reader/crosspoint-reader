#include "activities/util/PasswordMasking.h"

#include <gtest/gtest.h>

#include <string>

namespace {

constexpr size_t kRevealNothing = std::string::npos;

// The property that matters for a password field: what gets drawn is not the
// secret. KeyboardEntryActivity applies this whenever InputType::Password is
// active and the user has not toggled visibility -- which is why the Wi-Fi
// password prompt must ask for InputType::Password rather than InputType::Text.
TEST(PasswordMasking, MasksEveryCharacterWhenNothingIsRevealed) {
  EXPECT_EQ(maskPasswordText("hunter2", kRevealNothing), "*******");
}

TEST(PasswordMasking, LeavesNoOriginalCharacterVisible) {
  const std::string secret = "correct horse";
  const std::string masked = maskPasswordText(secret, kRevealNothing);
  ASSERT_EQ(masked.length(), secret.length());
  for (const char c : masked) {
    EXPECT_EQ(c, '*');
  }
}

// One character stays legible so the user can confirm what they just typed.
TEST(PasswordMasking, RevealsExactlyTheRequestedPosition) {
  EXPECT_EQ(maskPasswordText("abcde", 0), "a****");
  EXPECT_EQ(maskPasswordText("abcde", 2), "**c**");
  EXPECT_EQ(maskPasswordText("abcde", 4), "****e");
}

TEST(PasswordMasking, RevealPositionPastTheEndMasksEverything) {
  // KeyboardEntryActivity passes text.length() in cursor mode, which is one
  // past the last byte -- nothing should be revealed in the drawn string.
  EXPECT_EQ(maskPasswordText("abc", 3), "***");
  EXPECT_EQ(maskPasswordText("abc", 99), "***");
}

TEST(PasswordMasking, EmptyTextStaysEmpty) {
  EXPECT_EQ(maskPasswordText("", kRevealNothing), "");
  EXPECT_EQ(maskPasswordText("", 0), "");
}

// Byte-wise by design: the caller's cursor bookkeeping is in bytes, so a
// multi-byte character masks to one '*' per byte and the drawn width still
// hides how many code points the secret has.
TEST(PasswordMasking, MasksMultiByteInputPerByte) {
  const std::string utf8 = "\xC3\xA9\xC3\xA8";  // "éè", two code points, four bytes
  EXPECT_EQ(maskPasswordText(utf8, kRevealNothing), "****");
}

}  // namespace
