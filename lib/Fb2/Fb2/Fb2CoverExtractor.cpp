#include "Fb2CoverExtractor.h"

#include <HalStorage.h>
#include <HardwareSerial.h>
#include <JpegToBmpConverter.h>
#include <expat.h>

#include <cstring>

namespace {
// Base64 decoding table
constexpr int8_t B64_INVALID = -1;

int8_t base64DecodeChar(char c) {
  if (c >= 'A' && c <= 'Z') return c - 'A';
  if (c >= 'a' && c <= 'z') return c - 'a' + 26;
  if (c >= '0' && c <= '9') return c - '0' + 52;
  if (c == '+') return 62;
  if (c == '/') return 63;
  return B64_INVALID;
}

struct ExtractState {
  const std::string& targetId;
  FsFile* outputFile;
  bool foundTarget;
  bool inBinary;
  bool done;
  // Base64 streaming state
  uint8_t b64Buf[3];
  int b64Pending;

  explicit ExtractState(const std::string& id, FsFile* out)
      : targetId(id), outputFile(out), foundTarget(false), inBinary(false), done(false), b64Pending(0) {}

  void decodeBase64Chunk(const char* data, int len) {
    for (int i = 0; i < len; i++) {
      char c = data[i];
      if (c == '\n' || c == '\r' || c == ' ' || c == '\t') continue;
      if (c == '=') {
        // Padding - flush remaining
        if (b64Pending == 2) {
          outputFile->write(b64Buf, 1);
        } else if (b64Pending == 3) {
          outputFile->write(b64Buf, 2);
        }
        b64Pending = 0;
        continue;
      }

      int8_t val = base64DecodeChar(c);
      if (val == B64_INVALID) continue;

      switch (b64Pending) {
        case 0:
          b64Buf[0] = static_cast<uint8_t>(val << 2);
          b64Pending = 1;
          break;
        case 1:
          b64Buf[0] |= static_cast<uint8_t>(val >> 4);
          b64Buf[1] = static_cast<uint8_t>((val & 0x0F) << 4);
          b64Pending = 2;
          break;
        case 2:
          b64Buf[1] |= static_cast<uint8_t>(val >> 2);
          b64Buf[2] = static_cast<uint8_t>((val & 0x03) << 6);
          b64Pending = 3;
          break;
        case 3:
          b64Buf[2] |= static_cast<uint8_t>(val);
          outputFile->write(b64Buf, 3);
          b64Pending = 0;
          break;
      }
    }
  }
};

const char* stripNs(const char* name) {
  const char* colon = strchr(name, ':');
  return colon ? colon + 1 : name;
}

void XMLCALL onStart(void* userData, const char* name, const char** atts) {
  auto* state = static_cast<ExtractState*>(userData);
  if (state->done) return;

  const char* tag = stripNs(name);
  if (strcmp(tag, "binary") == 0 && atts) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "id") == 0 && state->targetId == atts[i + 1]) {
        state->foundTarget = true;
        state->inBinary = true;
        state->b64Pending = 0;
        return;
      }
    }
  }
}

void XMLCALL onEnd(void* userData, const char* name) {
  auto* state = static_cast<ExtractState*>(userData);
  const char* tag = stripNs(name);
  if (state->inBinary && strcmp(tag, "binary") == 0) {
    // Flush any remaining base64 data
    if (state->b64Pending == 2) {
      state->outputFile->write(state->b64Buf, 1);
    } else if (state->b64Pending == 3) {
      state->outputFile->write(state->b64Buf, 2);
    }
    state->inBinary = false;
    state->done = true;
  }
}

void XMLCALL onCharData(void* userData, const char* s, int len) {
  auto* state = static_cast<ExtractState*>(userData);
  if (state->inBinary) {
    state->decodeBase64Chunk(s, len);
  }
}
}  // namespace

bool Fb2CoverExtractor::extractBinaryToJpeg(const std::string& tempJpegPath) const {
  FsFile jpegFile;
  if (!Storage.openFileForWrite("FB2", tempJpegPath, jpegFile)) {
    return false;
  }

  ExtractState state(binaryId, &jpegFile);

  XML_Parser xmlParser = XML_ParserCreate(nullptr);
  if (!xmlParser) {
    jpegFile.close();
    return false;
  }

  XML_SetUserData(xmlParser, &state);
  XML_SetElementHandler(xmlParser, onStart, onEnd);
  XML_SetCharacterDataHandler(xmlParser, onCharData);

  FsFile fb2File;
  if (!Storage.openFileForRead("FB2", filepath, fb2File)) {
    XML_ParserFree(xmlParser);
    jpegFile.close();
    return false;
  }

  bool success = true;
  int done;
  do {
    void* buf = XML_GetBuffer(xmlParser, 1024);
    if (!buf) {
      success = false;
      break;
    }

    size_t len = fb2File.read(buf, 1024);
    if (len == 0 && fb2File.available() > 0) {
      success = false;
      break;
    }

    done = fb2File.available() == 0;

    if (XML_ParseBuffer(xmlParser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      // Parse errors in binary extraction are common due to entity issues; check if we got data
      if (state.foundTarget && jpegFile.size() > 0) {
        break;  // We got enough data
      }
      success = false;
      break;
    }

    // Early exit once we've extracted the binary
    if (state.done) break;
  } while (!done);

  XML_ParserFree(xmlParser);
  fb2File.close();
  jpegFile.close();

  if (!state.foundTarget || !success) {
    Storage.remove(tempJpegPath.c_str());
    return false;
  }

  return true;
}

bool Fb2CoverExtractor::extract() const {
  const auto tempJpegPath = outputBmpPath.substr(0, outputBmpPath.rfind('/')) + "/.cover.jpg";

  if (!extractBinaryToJpeg(tempJpegPath)) {
    Serial.printf("[%lu] [FB2] Failed to extract cover binary\n", millis());
    return false;
  }

  FsFile coverJpg;
  if (!Storage.openFileForRead("FB2", tempJpegPath, coverJpg)) {
    Storage.remove(tempJpegPath.c_str());
    return false;
  }

  FsFile coverBmp;
  if (!Storage.openFileForWrite("FB2", outputBmpPath, coverBmp)) {
    coverJpg.close();
    Storage.remove(tempJpegPath.c_str());
    return false;
  }

  const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp);
  coverJpg.close();
  coverBmp.close();
  Storage.remove(tempJpegPath.c_str());

  if (!success) {
    Serial.printf("[%lu] [FB2] Failed to convert cover JPEG to BMP\n", millis());
    Storage.remove(outputBmpPath.c_str());
  } else {
    Serial.printf("[%lu] [FB2] Generated cover BMP\n", millis());
  }
  return success;
}

bool Fb2CoverExtractor::extractThumb(const std::string& thumbPath, int height) const {
  const auto cacheDir = thumbPath.substr(0, thumbPath.rfind('/'));
  const auto tempJpegPath = cacheDir + "/.cover.jpg";

  if (!extractBinaryToJpeg(tempJpegPath)) {
    Serial.printf("[%lu] [FB2] Failed to extract cover binary for thumbnail\n", millis());
    return false;
  }

  FsFile coverJpg;
  if (!Storage.openFileForRead("FB2", tempJpegPath, coverJpg)) {
    Storage.remove(tempJpegPath.c_str());
    return false;
  }

  FsFile thumbBmp;
  if (!Storage.openFileForWrite("FB2", thumbPath, thumbBmp)) {
    coverJpg.close();
    Storage.remove(tempJpegPath.c_str());
    return false;
  }

  int thumbWidth = static_cast<int>(height * 0.6);
  const bool success = JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(coverJpg, thumbBmp, thumbWidth, height);
  coverJpg.close();
  thumbBmp.close();
  Storage.remove(tempJpegPath.c_str());

  if (!success) {
    Serial.printf("[%lu] [FB2] Failed to generate thumbnail BMP\n", millis());
    Storage.remove(thumbPath.c_str());
  } else {
    Serial.printf("[%lu] [FB2] Generated thumbnail BMP\n", millis());
  }
  return success;
}
