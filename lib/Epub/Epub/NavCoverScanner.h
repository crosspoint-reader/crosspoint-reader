#pragma once

#include <Print.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

// Finds the cover landmark of an EPUB 3 navigation document as the document streams through it,
// never holding it whole: the href of the first tag whose epub:type lists "cover", which in a nav
// document is the <a epub:type="cover"> of <nav epub:type="landmarks">.
//
// Attributes are matched as text (name="value" or name='value'), as CoverImageRefScanner matches
// them. A value longer than MAX_REF bytes is passed over. The scan is done at the first cover link
// or after MAX_SCAN_BYTES of document (the landmarks follow the whole table of contents, so the
// budget is larger than a cover page's); hand stopFlag() to the stream so the rest is not inflated.
//
// After witchling-reader commit 5b62bbee (syfq91), which reads the landmark with the nav parser;
// a text scan needs no second ~10 KB SAX parser beside the OPF parser still alive at this point.
class NavCoverScanner final : public Print {
 public:
  static constexpr size_t MAX_REF = 255;
  static constexpr size_t MAX_SCAN_BYTES = 256 * 1024;

  size_t write(const uint8_t c) override {
    feed(static_cast<char>(c));
    return 1;
  }
  size_t write(const uint8_t* buffer, const size_t size) override {
    for (size_t i = 0; i < size; i++) feed(static_cast<char>(buffer[i]));
    return size;
  }

  // The cover link's href as written in the document (fragment and all); empty if none.
  std::string_view result() const { return found_ ? std::string_view{href_, hrefLen_} : std::string_view{}; }
  const bool* stopFlag() const { return &done_; }

 private:
  enum class Attr : uint8_t { Other, Type, Href };

  void feed(const char c) {
    if (done_) return;
    if (++scanned_ >= MAX_SCAN_BYTES) done_ = true;

    if (quote_ != 0) {
      // A '<' cannot stand in an attribute value, so a quote opened by an apostrophe in a comment
      // ends here instead of swallowing the rest of the document.
      if (c != '<') {
        if (c == quote_) {
          endValue();
        } else if (valueLen_ < MAX_REF) {
          value_[valueLen_++] = c;
        } else {
          valueTooLong_ = true;
        }
        return;
      }
      quote_ = 0;
    }

    if (c == '<') {
      inTag_ = true;
      tagIsCover_ = false;
      hrefLen_ = 0;
      tailLen_ = 0;
      return;
    }
    if (!inTag_) return;
    if (c == '>') {
      inTag_ = false;
      if (tagIsCover_ && hrefLen_ > 0) found_ = done_ = true;
      return;
    }
    if (c == '"' || c == '\'') {
      quote_ = c;
      attr_ = attributeBeforeQuote();
      valueLen_ = 0;
      valueTooLong_ = false;
      return;
    }
    pushTail(c);
  }

  void endValue() {
    quote_ = 0;
    tailLen_ = 0;
    if (valueTooLong_) return;
    const std::string_view value{value_, valueLen_};
    if (attr_ == Attr::Type) {
      tagIsCover_ = tagIsCover_ || listsCover(value);
    } else if (attr_ == Attr::Href) {
      memcpy(href_, value_, valueLen_);
      hrefLen_ = valueLen_;
    }
  }

  // epub:type is a space-separated list of terms.
  static bool listsCover(std::string_view value) {
    while (!value.empty()) {
      const size_t end = value.find_first_of(" \t\r\n");
      if (value.substr(0, end) == "cover") return true;
      if (end == std::string_view::npos) break;
      value.remove_prefix(end + 1);
    }
    return false;
  }

  Attr attributeBeforeQuote() const {
    const std::string_view tail{tail_, tailLen_};
    const auto endsWith = [&](const std::string_view s) {
      return tail.size() >= s.size() && tail.substr(tail.size() - s.size()) == s;
    };
    if (endsWith("epub:type=")) return Attr::Type;
    if (endsWith("href=")) return Attr::Href;
    return Attr::Other;
  }

  void pushTail(const char c) {
    if (tailLen_ == sizeof(tail_)) {
      memmove(tail_, tail_ + 1, sizeof(tail_) - 1);
      tailLen_--;
    }
    tail_[tailLen_++] = c;
  }

  char tail_[10] = {};  // strlen("epub:type=")
  uint8_t tailLen_ = 0;
  char quote_ = 0;  // the quote of the value being read, 0 between values
  Attr attr_ = Attr::Other;
  char value_[MAX_REF] = {};
  size_t valueLen_ = 0;
  bool valueTooLong_ = false;
  char href_[MAX_REF] = {};  // the href of the tag being read
  size_t hrefLen_ = 0;
  bool inTag_ = false;
  bool tagIsCover_ = false;
  bool found_ = false;
  size_t scanned_ = 0;
  bool done_ = false;
};
