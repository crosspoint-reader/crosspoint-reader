#include "BengaliReorder.h"

#include <cstring>

#include "Utf8.h"

namespace {
constexpr uint32_t kNukta = 0x09BC;
constexpr uint32_t kHalant = 0x09CD;
constexpr uint32_t kRa = 0x09B0;
constexpr uint32_t kVowelSignE = 0x09C7;
constexpr uint32_t kZwj = 0x200D;
constexpr uint32_t kZwnj = 0x200C;

using Cursor = const unsigned char*;

constexpr bool isConsonant(const uint32_t cp) {
  return (cp >= 0x0995 && cp <= 0x09B9) || cp == 0x09DC || cp == 0x09DD || cp == 0x09DF || cp == 0x09F0 || cp == 0x09F1;
}

// Dependent vowel signs, the au length mark, and candrabindu/anusvara/visarga:
// everything that follows a consonant cluster within one syllable.
constexpr bool isSyllableTail(const uint32_t cp) {
  return (cp >= 0x0981 && cp <= 0x0983) || (cp >= 0x09BE && cp <= 0x09CC) || cp == 0x09D7 || cp == 0x09E2 ||
         cp == 0x09E3;
}

// Vowel sign drawn in front of the cluster, or 0. The two-part vowels ো/ৌ
// contribute their ে half here.
constexpr uint32_t preBasePart(const uint32_t cp) {
  switch (cp) {
    case 0x09BF:
    case 0x09C8:
      return cp;
    case 0x09C7:
    case 0x09CB:
    case 0x09CC:
      return kVowelSignE;
    default:
      return 0;
  }
}

// Vowel sign drawn after the cluster, or 0 when the sign is entirely pre-base.
constexpr uint32_t postBasePart(const uint32_t cp) {
  switch (cp) {
    case 0x09BF:
    case 0x09C7:
    case 0x09C8:
      return 0;
    case 0x09CB:
      return 0x09BE;
    case 0x09CC:
      return 0x09D7;
    default:
      return cp;
  }
}

constexpr uint32_t composeNukta(const uint32_t base) {
  switch (base) {
    case 0x09A1:
      return 0x09DC;
    case 0x09A2:
      return 0x09DD;
    case 0x09AF:
      return 0x09DF;
    default:
      return 0;
  }
}

uint32_t peek(Cursor p) { return utf8NextCodepoint(&p); }

// Advances past one consonant and an optional nukta.
Cursor skipConsonant(Cursor p) {
  utf8NextCodepoint(&p);
  Cursor q = p;
  return utf8NextCodepoint(&q) == kNukta ? q : p;
}

// End of the consonant cluster starting at `p`: C[N] (H [ZWJ] C[N])* [H [ZWJ|ZWNJ]].
Cursor clusterEnd(Cursor p) {
  p = skipConsonant(p);
  while (peek(p) == kHalant) {
    Cursor q = p;
    utf8NextCodepoint(&q);
    Cursor afterJoiner = q;
    if (utf8NextCodepoint(&afterJoiner) != kZwj) afterJoiner = q;
    if (isConsonant(peek(afterJoiner))) {
      p = skipConsonant(afterJoiner);
      continue;
    }
    // Syllable-final hasant, optionally with an explicit joiner.
    const uint32_t joiner = peek(q);
    if (joiner == kZwj || joiner == kZwnj) utf8NextCodepoint(&q);
    return q;
  }
  return p;
}

Cursor tailEnd(Cursor p) {
  while (isSyllableTail(peek(p))) utf8NextCodepoint(&p);
  return p;
}

// Copies [begin, end), composing nukta letters. Sets `changed` when it does.
void appendCluster(Cursor begin, const Cursor end, std::string& out, bool& changed) {
  while (begin < end) {
    const Cursor start = begin;
    const uint32_t cp = utf8NextCodepoint(&begin);
    if (const uint32_t composed = composeNukta(cp); composed != 0 && begin < end && peek(begin) == kNukta) {
      utf8NextCodepoint(&begin);
      utf8AppendCodepoint(composed, out);
      changed = true;
      continue;
    }
    out.append(reinterpret_cast<const char*>(start), begin - start);
  }
}

bool containsBengali(const char* text) {
  // U+0980-U+09FF encode as E0 A6 xx / E0 A7 xx.
  for (Cursor p = reinterpret_cast<Cursor>(text); *p; ++p) {
    if (p[0] == 0xE0 && (p[1] == 0xA6 || p[1] == 0xA7)) return true;
  }
  return false;
}
}  // namespace

bool bengaliReorderForDisplay(const char* text, std::string& out) {
  if (text == nullptr || !containsBengali(text)) return false;

  out.clear();
  out.reserve(strlen(text) + 8);
  bool changed = false;

  Cursor p = reinterpret_cast<Cursor>(text);
  while (*p) {
    const uint32_t cp = peek(p);
    if (!isConsonant(cp)) {
      const Cursor start = p;
      utf8NextCodepoint(&p);
      out.append(reinterpret_cast<const char*>(start), p - start);
      continue;
    }

    // Without a reph glyph, র্ stays a spacing prefix; the vowel sign
    // belongs in front of the consonant it is pronounced with.
    Cursor clusterBegin = p;
    {
      Cursor q = p;
      if (utf8NextCodepoint(&q) == kRa && utf8NextCodepoint(&q) == kHalant && isConsonant(peek(q))) {
        out.append(reinterpret_cast<const char*>(p), q - p);
        clusterBegin = q;
      }
    }

    const Cursor clusterStop = clusterEnd(clusterBegin);
    const Cursor tailStop = tailEnd(clusterStop);

    for (Cursor t = clusterStop; t < tailStop;) {
      if (const uint32_t pre = preBasePart(utf8NextCodepoint(&t)); pre != 0) {
        utf8AppendCodepoint(pre, out);
        changed = true;
      }
    }
    appendCluster(clusterBegin, clusterStop, out, changed);
    for (Cursor t = clusterStop; t < tailStop;) {
      if (const uint32_t post = postBasePart(utf8NextCodepoint(&t)); post != 0) utf8AppendCodepoint(post, out);
    }
    p = tailStop;
  }
  return changed;
}
