#include "JsonStringStreamer.h"

#include <cstring>

namespace readwise {

JsonStringStreamer::JsonStringStreamer(const char* key, Sink sink)
    : key_(key), keyLen_(std::strlen(key)), sink_(std::move(sink)) {}

bool JsonStringStreamer::feed(const char* data, size_t len) {
  for (size_t i = 0; i < len && ok_ && state_ != State::Done; ++i) {
    const char c = data[i];
    switch (state_) {
      case State::Scan:
        scan(c);
        break;
      case State::InString:
        inString(c);
        break;
      case State::AfterColon:
        if (c == '"') {
          found_ = true;
          state_ = State::InTarget;
        } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
          // Key present but not a string (null): nothing to stream.
          state_ = State::Scan;
          scan(c);
        }
        break;
      case State::InTarget:
        inTarget(c);
        break;
      case State::Done:
        break;
    }
  }
  if (state_ == State::Done || !ok_) flush();
  return ok_;
}

void JsonStringStreamer::scan(const char c) {
  if (c == '"') {
    strLen_ = 0;
    strOverflow_ = false;
    escaped_ = false;
    state_ = State::InString;
  } else if (c == ':') {
    if (lastStringIsKey_) state_ = State::AfterColon;
    lastStringIsKey_ = false;
  } else if (c != ' ' && c != '\t' && c != '\n' && c != '\r') {
    lastStringIsKey_ = false;
  }
}

void JsonStringStreamer::inString(const char c) {
  // Escapes inside skipped strings only matter for finding the end quote;
  // keys we compare are plain ASCII.
  if (escaped_) {
    escaped_ = false;
    strOverflow_ = true;  // an escaped key never equals ours
    return;
  }
  if (c == '\\') {
    escaped_ = true;
    return;
  }
  if (c == '"') {
    lastStringIsKey_ = !strOverflow_ && strLen_ == keyLen_ && std::memcmp(str_, key_, keyLen_) == 0;
    state_ = State::Scan;
    return;
  }
  if (strLen_ < kKeyMax) {
    str_[strLen_++] = c;
  } else {
    strOverflow_ = true;
  }
}

void JsonStringStreamer::inTarget(const char c) {
  if (hexLeft_ > 0) {
    uint16_t v;
    if (c >= '0' && c <= '9') {
      v = static_cast<uint16_t>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      v = static_cast<uint16_t>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      v = static_cast<uint16_t>(c - 'A' + 10);
    } else {
      ok_ = false;
      return;
    }
    hex_ = static_cast<uint16_t>((hex_ << 4) | v);
    if (--hexLeft_ == 0) {
      if (hex_ >= 0xD800 && hex_ <= 0xDBFF) {
        if (highSurrogate_) emitUtf8(0xFFFD);
        highSurrogate_ = hex_;
      } else if (hex_ >= 0xDC00 && hex_ <= 0xDFFF) {
        if (highSurrogate_) {
          emitUtf8(0x10000 + ((static_cast<uint32_t>(highSurrogate_) - 0xD800) << 10) + (hex_ - 0xDC00));
          highSurrogate_ = 0;
        } else {
          emitUtf8(0xFFFD);
        }
      } else {
        if (highSurrogate_) {
          emitUtf8(0xFFFD);
          highSurrogate_ = 0;
        }
        emitUtf8(hex_);
      }
    }
    return;
  }

  if (escaped_) {
    escaped_ = false;
    if (c == 'u') {
      hexLeft_ = 4;
      hex_ = 0;
      return;
    }
    if (highSurrogate_) {
      emitUtf8(0xFFFD);
      highSurrogate_ = 0;
    }
    char ch;
    switch (c) {
      case 'n':
        ch = '\n';
        break;
      case 't':
        ch = '\t';
        break;
      case 'r':
        ch = '\r';
        break;
      case 'b':
        ch = '\b';
        break;
      case 'f':
        ch = '\f';
        break;
      default:
        ch = c;
        break;  // \" \\ \/
    }
    emit(&ch, 1);
    return;
  }

  if (c == '\\') {
    escaped_ = true;
    return;
  }
  if (highSurrogate_) {
    emitUtf8(0xFFFD);
    highSurrogate_ = 0;
  }
  if (c == '"') {
    complete_ = true;
    state_ = State::Done;
    return;
  }
  emit(&c, 1);
}

void JsonStringStreamer::emitUtf8(const uint32_t cp) {
  char u[4];
  size_t n;
  if (cp < 0x80) {
    u[0] = static_cast<char>(cp);
    n = 1;
  } else if (cp < 0x800) {
    u[0] = static_cast<char>(0xC0 | (cp >> 6));
    u[1] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 2;
  } else if (cp < 0x10000) {
    u[0] = static_cast<char>(0xE0 | (cp >> 12));
    u[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    u[2] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 3;
  } else {
    u[0] = static_cast<char>(0xF0 | (cp >> 18));
    u[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    u[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    u[3] = static_cast<char>(0x80 | (cp & 0x3F));
    n = 4;
  }
  emit(u, n);
}

void JsonStringStreamer::emit(const char* s, size_t n) {
  while (n > 0 && ok_) {
    if (bufLen_ == kOutBuf && !flush()) return;
    const size_t take = (kOutBuf - bufLen_ < n) ? kOutBuf - bufLen_ : n;
    std::memcpy(buf_ + bufLen_, s, take);
    bufLen_ += take;
    s += take;
    n -= take;
  }
}

bool JsonStringStreamer::flush() {
  if (bufLen_ > 0 && ok_) {
    ok_ = sink_(buf_, bufLen_);
    bufLen_ = 0;
  }
  return ok_;
}

}  // namespace readwise
