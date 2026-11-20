#include "TxtToHtml.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <Memory.h>

#include <algorithm>
#include <cctype>
#include <cstring>

const char* TxtToHtml::cacheVersionTag(std::string_view filename) {
  return FsHelpers::hasMarkdownExtension(filename) ? "<!-- MD_CACHE_VERSION: 3 -->" : "<!-- TXT_CACHE_VERSION: 1 -->";
}

size_t TxtToHtml::escapedHtmlSize(const std::string_view text) {
  size_t size = 0;
  for (const char c : text) {
    size += c == '&' ? 5 : (c == '<' || c == '>') ? 4 : 1;
  }
  return size;
}

void TxtToHtml::appendEscapedHtml(std::string& out, const std::string_view text) {
  TxtToHtml::streamEscapedHtml([&out](std::string_view fragment) { out.append(fragment); }, text);
}

bool TxtToHtml::stream(std::string_view filename, void* readerCtx, int (*readFn)(void* ctx, uint8_t* buf, size_t size),
                       Print& out) {
  constexpr size_t IN_BUF_SIZE = 8192;
  constexpr size_t OUT_BUF_SIZE = 8192;

  auto inBuf = makeUniqueNoThrow<uint8_t[]>(IN_BUF_SIZE);
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(OUT_BUF_SIZE);
  if (!inBuf || !outBuf) {
    LOG_ERR("TXT", "OOM: TXT/MD HTML streaming buffers");
    return false;
  }

  size_t outPos = 0;
  bool outputOk = true;
  auto flushOut = [&]() {
    if (outPos > 0) {
      const size_t written = out.write(outBuf.get(), outPos);
      outputOk = outputOk && (written == outPos);
      outPos = 0;
    }
  };

  auto writeByte = [&](uint8_t b) {
    outBuf[outPos++] = b;
    if (outPos == OUT_BUF_SIZE) flushOut();
  };

  auto writeStr = [&](std::string_view s) {
    for (char c : s) {
      writeByte(static_cast<uint8_t>(c));
    }
  };

  writeStr("<?xml version=\"1.0\" encoding=\"utf-8\"?>\n");
  writeStr(cacheVersionTag(filename));
  writeByte('\n');
  writeStr("<!DOCTYPE html>\n<html>\n<head><title>");
  std::string title = FsHelpers::getFileNameWithoutExtension(filename);
  TxtToHtml::streamEscapedHtml(writeStr, title);
  writeStr("</title></head>\n<body>\n");

  const bool markdown = FsHelpers::hasMarkdownExtension(filename);
  // Reuse half the input buffer for a bounded Markdown line; TXT keeps its full read size.
  const size_t readSize = markdown ? IN_BUF_SIZE / 2 : IN_BUF_SIZE;
  char* line = reinterpret_cast<char*>(inBuf.get() + readSize);
  size_t lineSize = 0;
  bool literalLine = false;

  bool leadingSpace = true, previousSpace = false;
  auto writeLiteral = [&](std::string_view text) {
    for (char c : text) {
      if (c == ' ' && (leadingSpace || previousSpace))
        writeStr("&#160;");
      else
        TxtToHtml::streamEscapedHtml(writeStr, std::string_view(&c, 1));
      leadingSpace = leadingSpace && c == ' ';
      previousSpace = c == ' ';
    }
  };

  auto writeInline = [&](std::string_view text) {
    char previous = ' ';
    auto word = [](unsigned char c) {
      return c >= 128 || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
    };
    while (!text.empty()) {
      const char marker = text.front();
      if (marker == '\\' && text.size() > 1 && std::ispunct(static_cast<unsigned char>(text[1]))) {
        text.remove_prefix(1);
        previous = text.front();
        writeLiteral(text.substr(0, 1));
        text.remove_prefix(1);
        continue;
      }
      const size_t width = (marker == '*' || marker == '_') && text.size() > 1 && text[1] == marker ? 2 : 1;
      const bool delimiter = marker == '*' || marker == '`' || (marker == '_' && !word(previous));
      const size_t end = delimiter ? text.find(text.substr(0, width), width) : std::string_view::npos;
      if (end != std::string_view::npos && end > width && text[width] != ' ' && text[end - 1] != ' ' &&
          (marker != '_' || end + width == text.size() || !word(text[end + width]))) {
        const char* tag = marker == '`' ? "code" : width == 2 ? "b" : "i";
        writeStr("<");
        writeStr(tag);
        writeStr(">");
        // shortcut: inline spans are flat; add nesting only if reading needs justify it.
        writeLiteral(text.substr(width, end - width));
        writeStr("</");
        writeStr(tag);
        writeStr(">");
        previous = marker;
        text.remove_prefix(end + width);
      } else {
        writeLiteral(text.substr(0, width));
        previous = text[width - 1];
        text.remove_prefix(width);
      }
    }
  };

  enum class List { None, Unordered, Ordered };
  List list = List::None;
  auto changeList = [&](List next) {
    static constexpr const char* OPEN[] = {"", "<ul>", "<ol>"};
    static constexpr const char* CLOSE[] = {"", "</ul>", "</ol>"};
    if (list == next) return;
    writeStr(CLOSE[static_cast<size_t>(list)]);
    list = next;
    writeStr(OPEN[static_cast<size_t>(list)]);
  };

  auto writeLine = [&]() {
    std::string_view text(line, lineSize);
    leadingSpace = true;
    previousSpace = false;
    const bool sceneBreak = !text.empty() && (text.front() == '*' || text.front() == '-') &&
                            std::count(text.begin(), text.end(), text.front()) >= 3 &&
                            text.find_first_not_of(text.front() == '*' ? " *" : " -") == std::string_view::npos;
    List next = List::None;
    size_t prefix = 0;
    if (!sceneBreak && text.size() >= 2 && text[1] == ' ' &&
        text.substr(0, 1).find_first_of("-*+") != std::string_view::npos) {
      next = List::Unordered;
      prefix = 2;
    } else {
      while (prefix < text.size() && text[prefix] >= '0' && text[prefix] <= '9') ++prefix;
      if (prefix && prefix <= 9 && text.substr(prefix, 2) == ". ") next = List::Ordered;
      prefix = next == List::Ordered ? prefix + 2 : 0;
    }
    changeList(next);
    const bool quote = text.substr(0, 2) == "> ";
    if (quote) prefix = 2;
    const char* open = quote ? "<blockquote style=\"margin-left: 1em\">" : next != List::None ? "<li>" : "";
    const char* close = quote ? "</blockquote>" : next != List::None ? "</li>" : "";
    writeStr(open);
    size_t heading = 0;
    while (heading < text.size() && text[heading] == '#') ++heading;
    if (heading > 6 || heading == text.size() || text[heading] != ' ') heading = 0;
    char tag[] = "<h1>";
    tag[2] += heading ? heading - 1 : 0;
    if (heading) writeStr(tag);
    if (sceneBreak)
      writeLiteral(text);
    else
      writeInline(text.substr(heading ? heading + 1 : prefix));
    if (heading) {
      writeStr("</");
      writeStr(std::string_view(tag + 1, 3));
    }
    writeStr(close);
    lineSize = 0;
    return heading != 0 || next != List::None || quote;
  };

  bool isStart = true;
  bool previousCR = false;
  bool atLineStart = true;
  size_t pendingSpaces = 0;
  int bytesRead = 0;

  while ((bytesRead = readFn(readerCtx, inBuf.get(), readSize)) > 0) {
    int startIdx = 0;
    if (isStart) {
      isStart = false;
      while (bytesRead < 3) {
        const int n = readFn(readerCtx, inBuf.get() + bytesRead, 3 - bytesRead);
        if (n <= 0) {
          if (n < 0) bytesRead = n;
          break;
        }
        bytesRead += n;
      }
      if (bytesRead < 0) break;
      if (bytesRead >= 3 && inBuf[0] == 0xEF && inBuf[1] == 0xBB && inBuf[2] == 0xBF) {
        startIdx = 3;
      }
    }

    for (int i = startIdx; i < bytesRead; i++) {
      uint8_t b = inBuf[i];
      if (markdown) {
        const bool skipLF = previousCR && b == '\n';
        previousCR = b == '\r';
        if (skipLF) continue;
        if (b == '\r') b = '\n';
      } else if (b == '\r')
        continue;

      if (markdown) {
        if (b == '\n') {
          if (literalLine || !writeLine()) writeStr("<br />");
          lineSize = 0;
          literalLine = false;
          leadingSpace = true;
          previousSpace = false;
        } else {
          if (lineSize == readSize) {
            // shortcut: overlong lines stay literal; keep conversion memory independent of file size.
            changeList(List::None);
            writeLiteral(std::string_view(line, lineSize));
            lineSize = 0;
            literalLine = true;
          }
          const char c = b < 0x20 && b != '\t' ? ' ' : static_cast<char>(b);
          if (literalLine) {
            writeLiteral(std::string_view(&c, 1));
          } else {
            line[lineSize++] = c;
          }
        }
        continue;
      }

      if (b == '\n') {
        pendingSpaces = 0;
        writeStr("<br />");
        atLineStart = true;
        continue;
      }

      if (b == ' ') {
        if (atLineStart) {
          writeStr("&#160;");
        } else {
          pendingSpaces++;
        }
        continue;
      }

      if (pendingSpaces > 0) {
        for (size_t s = 0; s < pendingSpaces - 1; s++) {
          writeStr("&#160;");
        }
        writeByte(' ');
        pendingSpaces = 0;
      }
      atLineStart = false;

      if (b == '&' || b == '<' || b == '>') {
        const char special = static_cast<char>(b);
        TxtToHtml::streamEscapedHtml(writeStr, std::string_view(&special, 1));
      } else if (b < 0x20 && b != '\t') {
        writeByte(' ');
      } else {
        writeByte(b);
      }
    }
  }

  if (bytesRead < 0) {
    LOG_ERR("TXT", "Read error while streaming TXT/MD: %.*s", static_cast<int>(filename.size()), filename.data());
    return false;
  }

  if (markdown && lineSize) writeLine();
  changeList(List::None);

  writeStr("\n</body>\n</html>\n");
  flushOut();
  if (!outputOk) {
    LOG_ERR("TXT", "Failed to stream complete HTML (write error or disk full)");
    return false;
  }
  return true;
}

bool TxtToHtml::stream(std::string_view filename, std::string_view content, Print& out) {
  struct ViewReader {
    std::string_view s;
    size_t pos = 0;
  } reader{content, 0};

  return stream(
      filename, &reader,
      [](void* ctx, uint8_t* buf, size_t size) -> int {
        auto* r = static_cast<ViewReader*>(ctx);
        if (r->pos >= r->s.size()) return 0;
        const size_t n = std::min(size, r->s.size() - r->pos);
        memcpy(buf, r->s.data() + r->pos, n);
        r->pos += n;
        return static_cast<int>(n);
      },
      out);
}
