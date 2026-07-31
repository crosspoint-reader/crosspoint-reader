#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace ClipTextBuilder {

namespace {

bool hasEmSpace(const std::string& w) {
  return w.size() >= 3 && static_cast<unsigned char>(w[0]) == 0xE2 && static_cast<unsigned char>(w[1]) == 0x80 &&
         static_cast<unsigned char>(w[2]) == 0x83;
}

std::string stripEmSpace(const std::string& w) { return hasEmSpace(w) ? w.substr(3) : w; }

std::string stripTrailingHyphen(std::string w) {
  while (!w.empty() && w.back() == '-') w.pop_back();
  return w;
}

}  // namespace

ClippingResult build(const std::vector<WordRef>& words, int from, int to, int total, int startPageInSection) {
  std::string text;

  constexpr int ANCHOR_WORDS = 4;
  std::string startAnchor;
  int anchorCount = 0;

  for (int i = from; i <= to; ++i) {
    const auto& wtext = stripEmSpace(words[i].text);
    const bool yGap =
        i > from && words[i].pageIdx == words[i - 1].pageIdx && words[i].y > words[i - 1].y + words[i - 1].h;
    const bool paragraphStart = (i > from) && (hasEmSpace(words[i].text) || words[i].paragraphStart || yGap);
    if (paragraphStart) {
      LOG_DBG("CLIP", "NL w[%d] em=%d ps=%d yGap=%d text=%.30s", i, hasEmSpace(words[i].text), words[i].paragraphStart,
              yGap, wtext.c_str());
    }
    if (i > from && !text.empty() && !paragraphStart) {
      const auto& prev = words[i - 1].text;
      const auto& prevStripped = stripEmSpace(prev);
      if (prevStripped.size() >= 1 && prevStripped.back() == '-' && !wtext.empty() &&
          !std::isspace(static_cast<unsigned char>(wtext[0])) && !std::ispunct(static_cast<unsigned char>(wtext[0])) &&
          prevStripped.find('-') == prevStripped.size() - 1) {
        LOG_DBG("CLIP", "MERGE w[%d] \"%.15s\"+\"%.15s\"", i - 1, prevStripped.c_str(), wtext.c_str());
        text.pop_back();
        text += wtext;
        continue;
      }
    }
    if (paragraphStart) {
      text += '\n';
    } else if (!text.empty()) {
      const bool attached = (words[i].y == words[i - 1].y) && (words[i].x <= words[i - 1].x + words[i - 1].w + 2);
      LOG_DBG("CLIP", "%s w[%d] gap=%d text=%.30s", attached ? "ATTACH" : "SEP", i,
              words[i].x - (words[i - 1].x + words[i - 1].w), words[i].text.c_str());
      if (!attached) {
        text += ' ';
      }
    }
    text += wtext;

    if (anchorCount < ANCHOR_WORDS) {
      if (!startAnchor.empty()) startAnchor += ' ';
      startAnchor += stripTrailingHyphen(wtext);
      anchorCount++;
    }
  }

  std::string endAnchorFull;
  anchorCount = 0;
  for (int i = to; i >= from && anchorCount < ANCHOR_WORDS; --i) {
    const auto wtext = stripTrailingHyphen(stripEmSpace(words[i].text));
    endAnchorFull = endAnchorFull.empty() ? wtext : wtext + ' ' + endAnchorFull;
    anchorCount++;
  }

  constexpr int CONTEXT_WORDS = 3;
  std::string beforeStart;
  for (int i = from - 1; i >= 0 && (from - i) <= CONTEXT_WORDS; --i) {
    const auto stripped = stripTrailingHyphen(stripEmSpace(words[i].text));
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    beforeStart = beforeStart.empty() ? stripped : stripped + ' ' + beforeStart;
  }
  std::string afterEnd;
  for (int i = to + 1; i < total && (i - to) <= CONTEXT_WORDS; ++i) {
    const auto stripped = stripTrailingHyphen(stripEmSpace(words[i].text));
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    afterEnd = afterEnd.empty() ? stripped : afterEnd + ' ' + stripped;
  }

  std::string midText;
  {
    constexpr int MID_WORDS = 4;
    int midStart = (from + to) / 2 - (MID_WORDS / 2);
    int midEnd = midStart + MID_WORDS - 1;
    if (midStart < from) midStart = from;
    if (midEnd > to) midEnd = to;
    for (int i = midStart; i <= midEnd; ++i) {
      const auto wtext = stripTrailingHyphen(stripEmSpace(words[i].text));
      if (!midText.empty()) midText += ' ';
      midText += wtext;
    }
  }

  LOG_DBG("CLIP", "Anchors: start=\"%.40s\" end=\"%.40s\" ctx=[\"%.20s\"] [\"%.20s\"] wc=%d", startAnchor.c_str(),
          endAnchorFull.c_str(), beforeStart.c_str(), afterEnd.c_str(), to - from + 1);

  return ClippingResult{std::move(text),
                        from,
                        to,
                        static_cast<uint16_t>(startPageInSection + words[from].pageIdx),
                        static_cast<uint16_t>(startPageInSection + words[to].pageIdx),
                        std::move(startAnchor),
                        std::move(endAnchorFull),
                        std::move(beforeStart),
                        std::move(afterEnd),
                        std::move(midText),
                        static_cast<uint16_t>(to - from + 1)};
}

}  // namespace ClipTextBuilder
