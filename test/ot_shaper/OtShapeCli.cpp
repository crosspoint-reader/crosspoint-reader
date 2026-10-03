// Shapes lines of text with lib/OtShaper and prints glyphs and positions, for
// test/ot_shaper/compare_harfbuzz.py to diff against HarfBuzz.
//
//   ot_shape_cli <font.ttf|layout blob> <scale> <ppem> <bcp47 or -> < lines
//
// Each output line: "gid+xAdvance+xOffset+yOffset" per glyph, space separated,
// or "FAIL".

#include <IndicScripts.h>
#include <OtShaper.h>

#include <cstdio>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

namespace {

std::vector<uint32_t> decodeUtf8(const std::string& s) {
  std::vector<uint32_t> out;
  for (size_t i = 0; i < s.size();) {
    const auto c = static_cast<uint8_t>(s[i]);
    uint32_t cp;
    int n;
    if (c < 0x80) {
      cp = c;
      n = 1;
    } else if ((c >> 5) == 6) {
      cp = c & 0x1F;
      n = 2;
    } else if ((c >> 4) == 14) {
      cp = c & 0x0F;
      n = 3;
    } else {
      cp = c & 0x07;
      n = 4;
    }
    for (int k = 1; k < n && i + k < s.size(); k++) cp = (cp << 6) | (static_cast<uint8_t>(s[i + k]) & 0x3F);
    out.push_back(cp);
    i += n;
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 5) {
    fprintf(stderr, "usage: %s font scale ppem lang < lines\n", argv[0]);
    return 2;
  }
  std::ifstream f(argv[1], std::ios::binary);
  const std::vector<uint8_t> font((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  ot::FaceTables tables;
  if (!ot::tablesFromSfnt(font.data(), static_cast<uint32_t>(font.size()), &tables)) {
    fprintf(stderr, "bad font\n");
    return 1;
  }
  ot::Face face;
  if (!face.init(tables)) {
    fprintf(stderr, "bad font\n");
    return 1;
  }
  ot::Scale scale;
  scale.set(std::atoi(argv[2]), static_cast<unsigned>(std::atoi(argv[3])), face.upem());
  const uint32_t* languages = ot::languageTagsFor(argv[4][0] == '-' ? "" : argv[4]);

  ot::Plan plans[10];
  bool built[10] = {};
  ot::Buffer buffer;
  std::string line;
  while (std::getline(std::cin, line)) {
    const std::vector<uint32_t> cps = decodeUtf8(line);
    ot::Script script = ot::Script::Devanagari;
    // Shared codepoints (dandas, joiners) belong to the surrounding script, as in ComplexShaper.
    for (const uint32_t cp : cps) {
      if (const indic::ScriptInfo* info = indic::scriptOf(cp)) {
        script = static_cast<ot::Script>(indic::indexOf(*info));
        break;
      }
    }
    const auto si = static_cast<size_t>(script);
    if (!built[si]) {
      // .cpfont layout fonts carry compiled plans; TTF/OTF files are planned here.
      if (!plans[si].load(face, script, languages)) plans[si].build(face, script, languages);
      built[si] = true;
    }
    if (!ot::shape(face, scale, plans[si], cps.data(), static_cast<unsigned>(cps.size()), buffer)) {
      printf("FAIL\n");
      continue;
    }
    for (unsigned i = 0; i < buffer.len(); i++) {
      printf("%s%u+%d+%d+%d", i ? " " : "", buffer.info[i].codepoint, buffer.pos[i].xAdvance, buffer.pos[i].xOffset,
             buffer.pos[i].yOffset);
    }
    printf("\n");
  }
  return 0;
}
