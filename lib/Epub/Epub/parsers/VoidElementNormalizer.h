#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// Closes unclosed HTML void elements so an XHTML document that a browser would
// accept can still be parsed as XML.
//
// EPUB requires XHTML, i.e. well-formed XML, but real-world content is routinely
// produced by HTML-shaped tooling that writes <br> rather than <br/>. expat then
// reports "mismatched tag" and the whole section fails to render.
//
// This is deliberately NOT general HTML tolerance. Void elements are the one class
// where a missing "/" is unambiguous: the spec says they can never have children,
// so <br> and <br/> denote exactly the same node and closing it changes no meaning.
// Every other malformed shape -- crossed nesting, a stray '<', an unclosed <p> --
// still fails exactly as before. Nothing here weakens validation; it canonicalises
// a closed, known-safe set.
//
// It matters beyond any one catalogue: a server-side repair cannot reach a book the
// user sideloaded over SD or the web-upload page, and cannot reach a catalogue book
// a device already downloaded. Those only get fixed here.
//
// Written as a character-at-a-time state machine that persists across calls, so a
// tag, comment or CDATA section split over a read boundary is not a special case.
// Pure, and free of Arduino/ESP-IDF types, so the tests exercise the exact code the
// firmware links.
namespace void_elements {

// Longest single tag this will buffer. A tag longer than this is emitted verbatim
// and left unclosed -- a missed fix, never a corrupted one. Sized well past a real
// <img src="..." alt="..."> so that does not happen in practice.
inline constexpr size_t MAX_TAG = 512;

// Worst case growth: "<br>" (4 bytes) -> "<br/>" (5), so one extra byte per four.
inline constexpr size_t growthBound(const size_t inLen) { return inLen / 4 + 1; }

inline bool isNameChar(const char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

inline bool isVoidName(const char* name, const size_t len) {
  static constexpr const char* kVoid[] = {"area", "base", "br",    "col",    "embed", "hr",  "img",
                                          "link", "meta", "param", "source", "track", "wbr", "input"};
  for (const char* v : kVoid) {
    if (strlen(v) != len) continue;
    size_t i = 0;
    for (; i < len; ++i) {
      char a = name[i];
      if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
      if (a != v[i]) break;
    }
    if (i == len) return true;
  }
  return false;
}

// Carried between calls. One instance per parse; zero-initialised is the correct
// starting state.
struct State {
  enum class Mode : uint8_t { Text, Tag, Comment, Cdata };

  // The tag being accumulated, '<' included. Only valid in Mode::Tag.
  char tag[MAX_TAG] = {};
  size_t tagLen = 0;
  // Open quote inside the current tag, or 0. '>' is legal inside an attribute
  // value (only '<' and '&' must be escaped there), so the tag does not end until
  // a '>' is seen outside quotes.
  char quote = 0;
  // Previous two characters, for matching a "-->" / "]]>" terminator that may
  // straddle a read boundary.
  char prev0 = 0;
  char prev1 = 0;
  Mode mode = Mode::Text;
};

namespace detail {

inline constexpr char kCommentOpen[] = "<!--";
inline constexpr char kCdataOpen[] = "<![CDATA[";

// Whether buf is buf-length worth of the start of `lit` (a complete match counts).
inline bool isPrefixOf(const char* buf, const size_t len, const char* lit) {
  const size_t litLen = strlen(lit);
  if (len > litLen) return false;
  return memcmp(buf, lit, len) == 0;
}

}  // namespace detail

/**
 * Copies [in, in+inLen) to out, inserting "/" before ">" on any unclosed void
 * element, and leaving everything else byte-identical.
 *
 * `st` must be the same State across every call of one document. `done` marks the
 * final chunk and flushes any partially-buffered tag verbatim.
 *
 * outCap must be at least inLen + MAX_TAG + growthBound(inLen + MAX_TAG): a tag
 * buffered during an earlier call is emitted during this one, on top of this
 * chunk's own bytes.
 *
 * Returns bytes written to out.
 */
inline size_t normalize(const char* in, const size_t inLen, char* out, const size_t outCap, State& st,
                        const bool done) {
  size_t o = 0;
  const auto emit = [&](const char c) {
    if (o < outCap) out[o++] = c;
  };
  const auto emitTag = [&]() {
    for (size_t k = 0; k < st.tagLen; ++k) emit(st.tag[k]);
    st.tagLen = 0;
  };

  // Rewrites the buffered tag if it is an unclosed void element, then emits it.
  // st.tag holds the whole tag, '<' through '>'.
  const auto finishTag = [&]() {
    const size_t len = st.tagLen;
    bool rewrite = false;
    if (len >= 3 && isNameChar(st.tag[1])) {
      size_t n = 1;
      while (n < len - 1 && isNameChar(st.tag[n])) n++;
      if (isVoidName(st.tag + 1, n - 1)) {
        // Already self-closing? Then leave it exactly as it is.
        size_t last = len - 2;  // char before '>'
        while (last > 0 &&
               (st.tag[last] == ' ' || st.tag[last] == '\t' || st.tag[last] == '\n' || st.tag[last] == '\r')) {
          last--;
        }
        rewrite = st.tag[last] != '/';
      }
    }
    if (!rewrite) {
      emitTag();
      return;
    }
    for (size_t k = 0; k + 1 < len; ++k) emit(st.tag[k]);
    emit('/');
    emit('>');
    st.tagLen = 0;
  };

  for (size_t i = 0; i < inLen; ++i) {
    const char c = in[i];

    switch (st.mode) {
      case State::Mode::Text:
        if (c == '<') {
          st.tagLen = 0;
          st.tag[st.tagLen++] = c;
          st.quote = 0;
          st.mode = State::Mode::Tag;
        } else {
          emit(c);
        }
        break;

      case State::Mode::Tag: {
        if (st.tagLen >= MAX_TAG) {
          // Implausibly long tag: give up on it rather than grow, and copy through.
          emitTag();
          emit(c);
          st.mode = State::Mode::Text;
          break;
        }
        st.tag[st.tagLen++] = c;

        // Comments and CDATA are copied verbatim, so their contents can never be
        // rewritten. That matters for CDATA especially, where a "<br>" is text the
        // document means to show, not markup to canonicalise.
        if (st.tagLen == strlen(detail::kCommentOpen) && memcmp(st.tag, detail::kCommentOpen, st.tagLen) == 0) {
          emitTag();
          st.prev0 = st.prev1 = 0;
          st.mode = State::Mode::Comment;
          break;
        }
        if (st.tagLen == strlen(detail::kCdataOpen) && memcmp(st.tag, detail::kCdataOpen, st.tagLen) == 0) {
          emitTag();
          st.prev0 = st.prev1 = 0;
          st.mode = State::Mode::Cdata;
          break;
        }
        // While the buffer could still become one of those openers, a '>' cannot
        // be this tag's end -- "<!-->" must not be split at its own '>'.
        if (detail::isPrefixOf(st.tag, st.tagLen, detail::kCommentOpen) ||
            detail::isPrefixOf(st.tag, st.tagLen, detail::kCdataOpen)) {
          break;
        }

        if (st.quote != 0) {
          if (c == st.quote) st.quote = 0;
        } else if (c == '"' || c == '\'') {
          st.quote = c;
        } else if (c == '>') {
          finishTag();
          st.mode = State::Mode::Text;
        }
        break;
      }

      case State::Mode::Comment:
      case State::Mode::Cdata: {
        emit(c);
        const char* term = st.mode == State::Mode::Comment ? "-->" : "]]>";
        if (st.prev0 == term[0] && st.prev1 == term[1] && c == term[2]) {
          st.mode = State::Mode::Text;
          st.prev0 = st.prev1 = 0;
        } else {
          st.prev0 = st.prev1;
          st.prev1 = c;
        }
        break;
      }
    }
  }

  // End of document with a tag still open: it was never well-formed, so pass it
  // through untouched and let the XML parser report it.
  if (done && st.mode == State::Mode::Tag) {
    emitTag();
    st.mode = State::Mode::Text;
  }

  return o;
}

}  // namespace void_elements
