#include "ArabicShaper.h"

#include <ScriptDetector.h>
#include <Utf8.h>

#include <array>
#include <vector>

namespace ArabicShaper {
namespace {

constexpr size_t kMaxClusterMarks = 8;

enum class ClusterClass : uint8_t {
  Arabic,
  LatinOrDigit,
  Neutral,
};

struct ArabicShapeEntry {
  uint32_t base;
  uint32_t isolated;
  uint32_t final;
  uint32_t initial;
  uint32_t medial;
  bool canConnectPrev;
  bool canConnectNext;
};

struct Cluster {
  uint32_t sourceBase = 0;
  uint32_t base = 0;
  uint32_t lamAlefAlef = 0;
  std::array<uint32_t, kMaxClusterMarks> marks{};
  uint8_t markCount = 0;
  ClusterClass type = ClusterClass::Neutral;
};

constexpr ArabicShapeEntry kShapeTable[] = {
    {0x0621, 0xFE80, 0xFE80, 0xFE80, 0xFE80, false, false}, {0x0622, 0xFE81, 0xFE82, 0xFE81, 0xFE82, true, false},
    {0x0623, 0xFE83, 0xFE84, 0xFE83, 0xFE84, true, false},  {0x0624, 0xFE85, 0xFE86, 0xFE85, 0xFE86, true, false},
    {0x0625, 0xFE87, 0xFE88, 0xFE87, 0xFE88, true, false},  {0x0626, 0xFE89, 0xFE8A, 0xFE8B, 0xFE8C, true, true},
    {0x0627, 0xFE8D, 0xFE8E, 0xFE8D, 0xFE8E, true, false},  {0x0628, 0xFE8F, 0xFE90, 0xFE91, 0xFE92, true, true},
    {0x0629, 0xFE93, 0xFE94, 0xFE93, 0xFE94, true, false},  {0x062A, 0xFE95, 0xFE96, 0xFE97, 0xFE98, true, true},
    {0x062B, 0xFE99, 0xFE9A, 0xFE9B, 0xFE9C, true, true},   {0x062C, 0xFE9D, 0xFE9E, 0xFE9F, 0xFEA0, true, true},
    {0x062D, 0xFEA1, 0xFEA2, 0xFEA3, 0xFEA4, true, true},   {0x062E, 0xFEA5, 0xFEA6, 0xFEA7, 0xFEA8, true, true},
    {0x062F, 0xFEA9, 0xFEAA, 0xFEA9, 0xFEAA, true, false},  {0x0630, 0xFEAB, 0xFEAC, 0xFEAB, 0xFEAC, true, false},
    {0x0631, 0xFEAD, 0xFEAE, 0xFEAD, 0xFEAE, true, false},  {0x0632, 0xFEAF, 0xFEB0, 0xFEAF, 0xFEB0, true, false},
    {0x0633, 0xFEB1, 0xFEB2, 0xFEB3, 0xFEB4, true, true},   {0x0634, 0xFEB5, 0xFEB6, 0xFEB7, 0xFEB8, true, true},
    {0x0635, 0xFEB9, 0xFEBA, 0xFEBB, 0xFEBC, true, true},   {0x0636, 0xFEBD, 0xFEBE, 0xFEBF, 0xFEC0, true, true},
    {0x0637, 0xFEC1, 0xFEC2, 0xFEC3, 0xFEC4, true, true},   {0x0638, 0xFEC5, 0xFEC6, 0xFEC7, 0xFEC8, true, true},
    {0x0639, 0xFEC9, 0xFECA, 0xFECB, 0xFECC, true, true},   {0x063A, 0xFECD, 0xFECE, 0xFECF, 0xFED0, true, true},
    {0x0640, 0x0640, 0x0640, 0x0640, 0x0640, true, true},   {0x0641, 0xFED1, 0xFED2, 0xFED3, 0xFED4, true, true},
    {0x0642, 0xFED5, 0xFED6, 0xFED7, 0xFED8, true, true},   {0x0643, 0xFED9, 0xFEDA, 0xFEDB, 0xFEDC, true, true},
    {0x0644, 0xFEDD, 0xFEDE, 0xFEDF, 0xFEE0, true, true},   {0x0645, 0xFEE1, 0xFEE2, 0xFEE3, 0xFEE4, true, true},
    {0x0646, 0xFEE5, 0xFEE6, 0xFEE7, 0xFEE8, true, true},   {0x0647, 0xFEE9, 0xFEEA, 0xFEEB, 0xFEEC, true, true},
    {0x0648, 0xFEED, 0xFEEE, 0xFEED, 0xFEEE, true, false},  {0x0649, 0xFEEF, 0xFEF0, 0xFEEF, 0xFEF0, true, false},
    {0x064A, 0xFEF1, 0xFEF2, 0xFEF3, 0xFEF4, true, true},   {0x067E, 0xFB56, 0xFB57, 0xFB58, 0xFB59, true, true},
    {0x0686, 0xFB7A, 0xFB7B, 0xFB7C, 0xFB7D, true, true},   {0x0698, 0xFB8A, 0xFB8B, 0xFB8A, 0xFB8B, true, false},
    {0x06A9, 0xFB8E, 0xFB8F, 0xFB90, 0xFB91, true, true},   {0x06AF, 0xFB92, 0xFB93, 0xFB94, 0xFB95, true, true},
    {0x06CC, 0xFBFC, 0xFBFD, 0xFBFE, 0xFBFF, true, true},
};

const ArabicShapeEntry* findShapeEntry(const uint32_t codepoint) {
  for (const auto& entry : kShapeTable) {
    if (entry.base == codepoint) {
      return &entry;
    }
  }
  return nullptr;
}

bool isArabicTransparentMark(const uint32_t codepoint) {
  return (codepoint >= 0x0610 && codepoint <= 0x061A) || (codepoint >= 0x064B && codepoint <= 0x065F) ||
         codepoint == 0x0670 || (codepoint >= 0x06D6 && codepoint <= 0x06DC) ||
         (codepoint >= 0x06DF && codepoint <= 0x06E8) || (codepoint >= 0x06EA && codepoint <= 0x06ED);
}

bool isAsciiDigit(const uint32_t codepoint) { return codepoint >= '0' && codepoint <= '9'; }

bool isAsciiAlpha(const uint32_t codepoint) {
  return (codepoint >= 'A' && codepoint <= 'Z') || (codepoint >= 'a' && codepoint <= 'z');
}

bool isWhitespace(const uint32_t codepoint) {
  return codepoint == ' ' || codepoint == '\t' || codepoint == '\n' || codepoint == '\r';
}

bool isOpeningBracket(const uint32_t codepoint) {
  return codepoint == '(' || codepoint == '[' || codepoint == '{' || codepoint == '<';
}

uint32_t matchingClosingBracket(const uint32_t codepoint) {
  switch (codepoint) {
    case '(':
      return ')';
    case '[':
      return ']';
    case '{':
      return '}';
    case '<':
      return '>';
    default:
      return 0;
  }
}

bool isDoubleAngleOpenAt(const std::vector<Cluster>& logical, const size_t index) {
  return index + 1 < logical.size() && logical[index].base == '<' && logical[index + 1].base == '<';
}

bool isDoubleAngleCloseAt(const std::vector<Cluster>& logical, const size_t index) {
  return index + 1 < logical.size() && logical[index].base == '>' && logical[index + 1].base == '>';
}

bool isArabicStrong(const Cluster& cluster) { return cluster.type == ClusterClass::Arabic; }

bool isLtrStrong(const Cluster& cluster) {
  return cluster.type == ClusterClass::LatinOrDigit && !isAsciiDigit(cluster.base);
}

bool isLtrSpanCluster(const Cluster& cluster) { return cluster.type == ClusterClass::LatinOrDigit; }

bool clusterCanConnectPrev(const Cluster& cluster) {
  if (cluster.lamAlefAlef != 0) {
    return true;
  }

  const ArabicShapeEntry* entry = findShapeEntry(cluster.sourceBase);
  return entry != nullptr && entry->canConnectPrev;
}

bool clusterCanConnectNext(const Cluster& cluster) {
  if (cluster.lamAlefAlef != 0) {
    return false;
  }

  const ArabicShapeEntry* entry = findShapeEntry(cluster.sourceBase);
  return entry != nullptr && entry->canConnectNext;
}

ClusterClass classifyCodepoint(const uint32_t codepoint) {
  if (ScriptDetector::isArabicCodepoint(codepoint)) {
    return ClusterClass::Arabic;
  }
  if (isAsciiDigit(codepoint) || isAsciiAlpha(codepoint) || codepoint >= 0x0080) {
    return ClusterClass::LatinOrDigit;
  }
  if (isWhitespace(codepoint)) {
    return ClusterClass::Neutral;
  }
  return ClusterClass::Neutral;
}

void appendCodepoint(Cluster& cluster, const uint32_t codepoint) {
  if (cluster.markCount < kMaxClusterMarks) {
    cluster.marks[cluster.markCount++] = codepoint;
  }
}

std::vector<Cluster> decodeClusters(const char* text) {
  std::vector<Cluster> clusters;
  if (text == nullptr) {
    return clusters;
  }

  const unsigned char* cursor = reinterpret_cast<const unsigned char*>(text);
  while (*cursor != 0) {
    const uint32_t codepoint = utf8NextCodepoint(&cursor);
    if (isArabicTransparentMark(codepoint) || utf8IsCombiningMark(codepoint)) {
      if (!clusters.empty()) {
        appendCodepoint(clusters.back(), codepoint);
        continue;
      }
    }

    Cluster cluster;
    cluster.sourceBase = codepoint;
    cluster.base = codepoint;
    cluster.type = classifyCodepoint(codepoint);
    clusters.push_back(cluster);
  }

  return clusters;
}

bool isLamAlefPair(const uint32_t lam, const uint32_t alef) {
  return lam == 0x0644 && (alef == 0x0622 || alef == 0x0623 || alef == 0x0625 || alef == 0x0627);
}

uint32_t lamAlefLigature(const uint32_t alef, const bool connectPrev) {
  switch (alef) {
    case 0x0622:
      return connectPrev ? 0xFEF6 : 0xFEF5;
    case 0x0623:
      return connectPrev ? 0xFEF8 : 0xFEF7;
    case 0x0625:
      return connectPrev ? 0xFEFA : 0xFEF9;
    case 0x0627:
    default:
      return connectPrev ? 0xFEFC : 0xFEFB;
  }
}

std::vector<Cluster> mergeLamAlef(const std::vector<Cluster>& input) {
  std::vector<Cluster> merged;
  merged.reserve(input.size());
  for (size_t i = 0; i < input.size(); ++i) {
    const Cluster& current = input[i];
    if (i + 1 < input.size() && isLamAlefPair(current.base, input[i + 1].base)) {
      Cluster ligature = current;
      ligature.sourceBase = current.sourceBase;
      ligature.base = current.base;
      ligature.lamAlefAlef = input[i + 1].sourceBase;
      ligature.type = ClusterClass::Arabic;
      for (uint8_t mark = 0; mark < input[i + 1].markCount; ++mark) {
        appendCodepoint(ligature, input[i + 1].marks[mark]);
      }
      merged.push_back(ligature);
      ++i;
      continue;
    }
    merged.push_back(current);
  }
  return merged;
}

uint32_t shapeCodepoint(const uint32_t codepoint, const bool connectPrev, const bool connectNext) {
  const ArabicShapeEntry* entry = findShapeEntry(codepoint);
  if (entry == nullptr) {
    return codepoint;
  }

  if (connectPrev && connectNext && entry->medial != 0) {
    return entry->medial;
  }
  if (connectPrev && entry->final != 0) {
    return entry->final;
  }
  if (connectNext && entry->initial != 0) {
    return entry->initial;
  }
  return entry->isolated;
}

std::vector<Cluster> shapeLogicalClusters(const std::vector<Cluster>& decoded) {
  std::vector<Cluster> shaped = mergeLamAlef(decoded);
  for (size_t i = 0; i < shaped.size(); ++i) {
    Cluster& cluster = shaped[i];
    if (!ScriptDetector::isArabicCodepoint(cluster.sourceBase)) {
      continue;
    }

    const bool prevArabic = i > 0 && ScriptDetector::isArabicCodepoint(shaped[i - 1].sourceBase);
    const bool nextArabic = i + 1 < shaped.size() && ScriptDetector::isArabicCodepoint(shaped[i + 1].sourceBase);

    const ArabicShapeEntry* entry = findShapeEntry(cluster.sourceBase);
    const bool connectPrev =
        prevArabic && entry != nullptr && clusterCanConnectNext(shaped[i - 1]) && clusterCanConnectPrev(cluster);
    const bool connectNext =
        nextArabic && entry != nullptr && clusterCanConnectNext(cluster) && clusterCanConnectPrev(shaped[i + 1]);

    if (cluster.lamAlefAlef != 0) {
      cluster.base = lamAlefLigature(cluster.lamAlefAlef, connectPrev);
      continue;
    }

    if (entry != nullptr) {
      cluster.base = shapeCodepoint(cluster.sourceBase, connectPrev, connectNext);
    }
  }
  return shaped;
}

void reverseRange(std::vector<Cluster>& clusters, const size_t begin, const size_t end) {
  size_t left = begin;
  size_t right = end;
  while (left < right) {
    --right;
    if (left >= right) {
      break;
    }
    const Cluster temp = clusters[left];
    clusters[left] = clusters[right];
    clusters[right] = temp;
    ++left;
  }
}

std::vector<Cluster> reorderVisual(const std::vector<Cluster>& logical) {
  std::vector<Cluster> visual;
  visual.reserve(logical.size());

  bool baseRtl = false;
  for (const Cluster& cluster : logical) {
    if (isArabicStrong(cluster)) {
      baseRtl = true;
      break;
    }
    if (isLtrStrong(cluster)) {
      break;
    }
  }

  size_t i = 0;
  while (i < logical.size()) {
    if (baseRtl && isDoubleAngleOpenAt(logical, i) && i + 2 < logical.size() && isArabicStrong(logical[i + 2])) {
      size_t arabicEnd = i + 2;
      while (arabicEnd < logical.size() && isArabicStrong(logical[arabicEnd])) {
        ++arabicEnd;
      }
      if (arabicEnd + 1 < logical.size() && isDoubleAngleCloseAt(logical, arabicEnd)) {
        visual.push_back(logical[i]);
        visual.push_back(logical[i + 1]);
        for (size_t index = arabicEnd; index > i + 2; --index) {
          visual.push_back(logical[index - 1]);
        }
        visual.push_back(logical[arabicEnd]);
        visual.push_back(logical[arabicEnd + 1]);
        i = arabicEnd + 2;
        continue;
      }
    }

    if (baseRtl && isOpeningBracket(logical[i].base) && i + 1 < logical.size() && isArabicStrong(logical[i + 1])) {
      size_t arabicEnd = i + 1;
      while (arabicEnd < logical.size() && isArabicStrong(logical[arabicEnd])) {
        ++arabicEnd;
      }
      if (arabicEnd < logical.size() && logical[arabicEnd].base == matchingClosingBracket(logical[i].base)) {
        visual.push_back(logical[i]);
        for (size_t index = arabicEnd; index > i + 1; --index) {
          visual.push_back(logical[index - 1]);
        }
        visual.push_back(logical[arabicEnd]);
        i = arabicEnd + 1;
        continue;
      }
    }

    if (!isArabicStrong(logical[i])) {
      visual.push_back(logical[i]);
      ++i;
      continue;
    }

    size_t arabicEnd = i + 1;
    while (arabicEnd < logical.size() && isArabicStrong(logical[arabicEnd])) {
      ++arabicEnd;
    }

    size_t neutralStart = arabicEnd;
    while (neutralStart < logical.size() && !isArabicStrong(logical[neutralStart]) &&
           !isLtrStrong(logical[neutralStart])) {
      ++neutralStart;
    }

    size_t ltrEnd = neutralStart;
    while (ltrEnd < logical.size() && isLtrSpanCluster(logical[ltrEnd])) {
      ++ltrEnd;
    }

    size_t trailingNeutralEnd = ltrEnd;
    while (trailingNeutralEnd < logical.size() && !isArabicStrong(logical[trailingNeutralEnd]) &&
           !isLtrStrong(logical[trailingNeutralEnd])) {
      ++trailingNeutralEnd;
    }

    if (baseRtl && neutralStart > arabicEnd && ltrEnd > neutralStart) {
      const bool wrappedDoubleAngleLtr = neutralStart > arabicEnd + 1 && logical[neutralStart - 2].base == '<' &&
                                         logical[neutralStart - 1].base == '<' && trailingNeutralEnd > ltrEnd + 1 &&
                                         logical[ltrEnd].base == '>' && logical[ltrEnd + 1].base == '>';
      if (wrappedDoubleAngleLtr) {
        visual.push_back(logical[neutralStart - 2]);
        visual.push_back(logical[neutralStart - 1]);
        for (size_t index = neutralStart; index < ltrEnd; ++index) {
          visual.push_back(logical[index]);
        }
        visual.push_back(logical[ltrEnd]);
        visual.push_back(logical[ltrEnd + 1]);
        for (size_t index = arabicEnd; index < neutralStart - 2; ++index) {
          visual.push_back(logical[index]);
        }
        for (size_t index = arabicEnd; index > i; --index) {
          visual.push_back(logical[index - 1]);
        }
        for (size_t index = ltrEnd + 2; index < trailingNeutralEnd; ++index) {
          visual.push_back(logical[index]);
        }
        i = trailingNeutralEnd;
        continue;
      }

      const bool wrappedLtr = neutralStart > arabicEnd && isOpeningBracket(logical[neutralStart - 1].base) &&
                              trailingNeutralEnd > ltrEnd &&
                              logical[ltrEnd].base == matchingClosingBracket(logical[neutralStart - 1].base);
      if (wrappedLtr) {
        visual.push_back(logical[neutralStart - 1]);
        for (size_t index = neutralStart; index < ltrEnd; ++index) {
          visual.push_back(logical[index]);
        }
        visual.push_back(logical[ltrEnd]);
        for (size_t index = arabicEnd; index < neutralStart - 1; ++index) {
          visual.push_back(logical[index]);
        }
        for (size_t index = arabicEnd; index > i; --index) {
          visual.push_back(logical[index - 1]);
        }
        for (size_t index = ltrEnd + 1; index < trailingNeutralEnd; ++index) {
          visual.push_back(logical[index]);
        }
        i = trailingNeutralEnd;
        continue;
      }

      for (size_t index = neutralStart; index < ltrEnd; ++index) {
        visual.push_back(logical[index]);
      }
      for (size_t index = arabicEnd; index < neutralStart; ++index) {
        visual.push_back(logical[index]);
      }
      for (size_t index = arabicEnd; index > i; --index) {
        visual.push_back(logical[index - 1]);
      }
      i = ltrEnd;
      continue;
    }

    if (!baseRtl) {
      for (size_t index = arabicEnd; index > i; --index) {
        visual.push_back(logical[index - 1]);
      }
      i = arabicEnd;
      continue;
    }

    size_t runEnd = i + 1;
    while (runEnd < logical.size() && !isLtrStrong(logical[runEnd])) {
      ++runEnd;
    }

    const size_t visualStart = visual.size();
    for (size_t index = runEnd; index > i; --index) {
      visual.push_back(logical[index - 1]);
    }

    size_t spanStart = visualStart;
    for (size_t index = visualStart; index <= visual.size(); ++index) {
      const bool inLtrSpan = index < visual.size() && isLtrSpanCluster(visual[index]);
      if (inLtrSpan) {
        continue;
      }
      if (spanStart < index) {
        reverseRange(visual, spanStart, index);
      }
      spanStart = index + 1;
    }

    i = runEnd;
  }

  return visual;
}

void flatten(const std::vector<Cluster>& clusters, std::vector<uint32_t>& output) {
  for (const Cluster& cluster : clusters) {
    output.push_back(cluster.base);
    for (uint8_t i = 0; i < cluster.markCount; ++i) {
      output.push_back(cluster.marks[i]);
    }
  }
}

}  // namespace

void shape(const char* text, std::vector<uint32_t>& output) {
  output.clear();
  const std::vector<Cluster> decoded = decodeClusters(text);
  output.reserve(decoded.size() * 2);
  const std::vector<Cluster> logical = shapeLogicalClusters(decoded);
  const std::vector<Cluster> visual = reorderVisual(logical);
  flatten(visual, output);
}

std::vector<uint32_t> shape(const char* text) {
  std::vector<uint32_t> output;
  shape(text, output);
  return output;
}

}  // namespace ArabicShaper
