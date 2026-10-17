#pragma once

#include <FsHelpers.h>
#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Finds the picture a cover document points at -- the guide's cover XHTML page, or an SVG wrapper
// named as the cover image -- as the document streams through it, never holding the document
// whole: the first JPEG/PNG value of a prefixed href attribute (xlink:href, an SVG <image>), else
// of a src attribute (an <img>), else of a plain href (an SVG 2 <image>). Other images are passed
// over, so a GIF ornament ahead of the picture cannot shadow it: cover generation decodes JPEG and
// PNG only.
//
// Attributes are matched as text (name="value" or name='value'), as the guide scan always did. A
// value longer than MAX_REF bytes is passed over. The scan is done at the first xlink:href picture
// or after MAX_SCAN_BYTES of document; hand stopFlag() to the stream so the rest is not inflated.
class CoverImageRefScanner final : public Print {
 public:
  static constexpr size_t MAX_REF = 255;
  static constexpr size_t MAX_SCAN_BYTES = 64 * 1024;

  size_t write(const uint8_t c) override {
    feed(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* buffer, const size_t size) override {
    for (size_t i = 0; i < size; i++) feed(static_cast<char>(buffer[i]));
    return size;
  }

  // The reference as written in the document, not yet resolved against its folder; empty if none.
  std::string_view result() const { return {best_, bestLen_}; }
  const bool* stopFlag() const { return &done_; }

 private:
  // Lower is preferred.
  enum class Kind : uint8_t { PrefixedHref, Src, Href, None };

  void feed(const char c) {
    if (done_) return;
    if (++scanned_ >= MAX_SCAN_BYTES) done_ = true;

    if (quote_ != 0) {
      if (c == quote_) {
        endValue();
      } else if (valueLen_ < MAX_REF) {
        value_[valueLen_++] = c;
      } else {
        valueTooLong_ = true;
      }
      return;
    }

    if (c == '"' || c == '\'') {
      const Kind kind = attributeBeforeQuote();
      if (kind < bestKind_) {
        quote_ = c;
        valueKind_ = kind;
        valueLen_ = 0;
        valueTooLong_ = false;
        return;
      }
    }
    pushTail(c);
  }

  void endValue() {
    quote_ = 0;
    tailLen_ = 0;
    if (valueTooLong_ || valueLen_ == 0) return;
    const std::string_view ref{value_, valueLen_};
    if (!FsHelpers::hasJpgExtension(ref) && !FsHelpers::hasPngExtension(ref)) return;
    memcpy(best_, value_, valueLen_);
    bestLen_ = valueLen_;
    bestKind_ = valueKind_;
    if (bestKind_ == Kind::PrefixedHref) done_ = true;
  }

  Kind attributeBeforeQuote() const {
    const std::string_view tail{tail_, tailLen_};
    const auto endsWith = [&](const std::string_view s) {
      return tail.size() >= s.size() && tail.substr(tail.size() - s.size()) == s;
    };
    if (endsWith(":href=")) return Kind::PrefixedHref;
    if (endsWith("src=")) return Kind::Src;
    if (endsWith("href=")) return Kind::Href;
    return Kind::None;
  }

  void pushTail(const char c) {
    if (tailLen_ == sizeof(tail_)) {
      memmove(tail_, tail_ + 1, sizeof(tail_) - 1);
      tailLen_--;
    }
    tail_[tailLen_++] = c;
  }

  char tail_[6] = {};  // strlen(":href=")
  uint8_t tailLen_ = 0;
  char quote_ = 0;  // the quote of the value being read, 0 between values
  char value_[MAX_REF] = {};
  size_t valueLen_ = 0;
  bool valueTooLong_ = false;
  Kind valueKind_ = Kind::None;
  char best_[MAX_REF] = {};
  size_t bestLen_ = 0;
  Kind bestKind_ = Kind::None;
  size_t scanned_ = 0;
  bool done_ = false;
};
