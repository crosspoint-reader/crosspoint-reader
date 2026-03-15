#include <ArabicShaper.h>

#include <cassert>
#include <vector>

namespace {

void expectEqual(const std::vector<uint32_t>& actual, const std::vector<uint32_t>& expected) {
  assert(actual.size() == expected.size());
  for (size_t i = 0; i < expected.size(); ++i) {
    assert(actual[i] == expected[i]);
  }
}

}  // namespace

int main() {
  expectEqual(ArabicShaper::shape("\xD8\xA8"), {0xFE8F});
  expectEqual(ArabicShaper::shape("\xD9\x84\xD8\xA8"), {0xFE90, 0xFEDF});
  expectEqual(ArabicShaper::shape("\xD8\xA8\xD8\xA7"), {0xFE8E, 0xFE91});
  expectEqual(ArabicShaper::shape("\xD9\x84\xD8\xA8\xD8\xAA"), {0xFE96, 0xFE92, 0xFEDF});

  expectEqual(ArabicShaper::shape("\xD9\x84\xD8\xA7"), {0xFEFB});
  expectEqual(ArabicShaper::shape("\xD8\xA8\xD9\x84\xD8\xA7"), {0xFEFC, 0xFE91});
  expectEqual(ArabicShaper::shape("\xD8\xA7\xD9\x84\xD8\xA5\xD8\xB9\xD8\xAF\xD8\xA7\xD8\xAF\xD8\xA7\xD8\xAA"),
              {0xFE95, 0xFE8D, 0xFEA9, 0xFE8D, 0xFEAA, 0xFECB, 0xFEF9, 0xFE8D});

  expectEqual(ArabicShaper::shape("\xD8\xA8\xD9\x8E"), {0xFE8F, 0x064E});

  const std::vector<uint32_t> mixed = ArabicShaper::shape("abc \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 xyz");
  expectEqual(mixed, {'a', 'b', 'c', ' ', 0xFEE1, 0xFEFC, 0xFEB3, ' ', 'x', 'y', 'z'});

  const std::vector<uint32_t> mixedDigits = ArabicShaper::shape("abc \xD8\xB3\xD9\x84\xD8\xA7\xD9\x85 123");
  expectEqual(mixedDigits, {'a', 'b', 'c', ' ', 0xFEE1, 0xFEFC, 0xFEB3, ' ', '1', '2', '3'});

  const std::vector<uint32_t> mixedArabicEnglish = ArabicShaper::shape("\xD8\xB4\xD8\xA8\xD9\x83\xD8\xA7\xD8\xAA WiFi");
  expectEqual(mixedArabicEnglish, {'W', 'i', 'F', 'i', ' ', 0xFE95, 0xFE8E, 0xFEDC, 0xFE92, 0xFEB7});

  const std::vector<uint32_t> rtlParens = ArabicShaper::shape("(\xD8\xA7\xD9\x84\xD9\x86\xD8\xB5)");
  expectEqual(rtlParens, {'(', 0xFEBA, 0xFEE8, 0xFEDF, 0xFE8D, ')'});

  const std::vector<uint32_t> rtlMixedParens = ArabicShaper::shape("\xD8\xA7\xD9\x84\xD9\x86\xD8\xB5 (test)");
  expectEqual(rtlMixedParens, {'(', 't', 'e', 's', 't', ')', ' ', 0xFEBA, 0xFEE8, 0xFEDF, 0xFE8D});

  const std::vector<uint32_t> rtlDoubleAngles = ArabicShaper::shape("<<\xD8\xA7\xD9\x84\xD9\x86\xD8\xB5>>");
  expectEqual(rtlDoubleAngles, {'<', '<', 0xFEBA, 0xFEE8, 0xFEDF, 0xFE8D, '>', '>'});

  const std::vector<uint32_t> rtlMixedDoubleAngles = ArabicShaper::shape("\xD8\xA7\xD9\x84\xD9\x86\xD8\xB5 <<test>>");
  expectEqual(rtlMixedDoubleAngles, {'<', '<', 't', 'e', 's', 't', '>', '>', ' ', 0xFEBA, 0xFEE8, 0xFEDF, 0xFE8D});

  return 0;
}
