#include "QuizFormatReader.h"

#include <algorithm>
#include <cstring>

namespace quiz {
namespace {

constexpr uint32_t kHeaderMagicOffset = 0;
constexpr uint32_t kHeaderVersionOffset = 8;
constexpr uint32_t kHeaderSizeOffset = 10;
constexpr uint32_t kHeaderFlagsOffset = 12;
constexpr uint32_t kHeaderDeclaredFileSizeOffset = 16;
constexpr uint32_t kHeaderQuestionCountOffset = 20;
constexpr uint32_t kHeaderIndexOffsetOffset = 24;
constexpr uint32_t kHeaderIndexBytesOffset = 28;
constexpr uint32_t kHeaderRecordsOffsetOffset = 32;
constexpr uint32_t kHeaderIndexEntrySizeOffset = 36;
constexpr uint32_t kHeaderTitleLengthOffset = 38;
constexpr uint32_t kHeaderDeckIdentityOffset = 40;
constexpr uint32_t kHeaderRevisionIdentityOffset = 56;
constexpr uint32_t kHeaderTitleStorageOffset = 72;
constexpr uint32_t kHeaderReservedOffset = 168;
constexpr uint32_t kRecordHeaderSizeOffset = 0;
constexpr uint32_t kRecordChoiceCountOffset = 2;
constexpr uint32_t kRecordCorrectChoiceOffset = 3;
constexpr uint32_t kRecordPromptLengthOffset = 4;
constexpr uint32_t kRecordExplanationLengthOffset = 6;
constexpr uint32_t kRecordChoiceLengthsOffset = 8;
constexpr uint32_t kRecordPayloadLengthOffset = 20;

struct Utf8State {
  uint32_t codePoint = 0;
  uint32_t minimumCodePoint = 0;
  uint8_t remainingBytes = 0;
};

struct QuestionMeta {
  uint16_t promptBytes = 0;
  uint16_t explanationBytes = 0;
  uint16_t choiceBytes[kQuizMaxChoices] = {};
  uint32_t payloadBytes = 0;
  uint8_t choiceCount = 0;
  uint8_t correctChoiceIndex = 0;
};

uint16_t le16(const uint8_t* raw) {
  return static_cast<uint16_t>(raw[0]) | (static_cast<uint16_t>(raw[1]) << 8);
}

uint32_t le32(const uint8_t* raw) {
  return static_cast<uint32_t>(raw[0]) | (static_cast<uint32_t>(raw[1]) << 8) | (static_cast<uint32_t>(raw[2]) << 16) |
         (static_cast<uint32_t>(raw[3]) << 24);
}

bool addWithin(uint32_t start, uint32_t bytes, uint32_t limit, uint32_t* end) {
  if (start > limit || bytes > limit - start) {
    return false;
  }
  *end = start + bytes;
  return true;
}

bool readExact(QuizInput& input, uint32_t offset, void* dst, size_t len) {
  const uint64_t size = input.size();
  return static_cast<uint64_t>(offset) <= size && len <= size - offset && input.read(offset, dst, len);
}

bool feedUtf8(Utf8State* state, const uint8_t* bytes, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    const uint8_t byte = bytes[i];
    if (byte == 0) {
      return false;
    }

    if (state->remainingBytes == 0) {
      if (byte < 0x80) {
        continue;
      }
      if ((byte & 0xE0) == 0xC0) {
        if (byte < 0xC2) {
          return false;
        }
        state->codePoint = byte & 0x1F;
        state->minimumCodePoint = 0x80;
        state->remainingBytes = 1;
        continue;
      }
      if ((byte & 0xF0) == 0xE0) {
        state->codePoint = byte & 0x0F;
        state->minimumCodePoint = 0x800;
        state->remainingBytes = 2;
        continue;
      }
      if ((byte & 0xF8) == 0xF0) {
        if (byte > 0xF4) {
          return false;
        }
        state->codePoint = byte & 0x07;
        state->minimumCodePoint = 0x10000;
        state->remainingBytes = 3;
        continue;
      }
      return false;
    }

    if ((byte & 0xC0) != 0x80) {
      return false;
    }

    state->codePoint = (state->codePoint << 6) | (byte & 0x3F);
    --state->remainingBytes;
    if (state->remainingBytes == 0 &&
        (state->codePoint < state->minimumCodePoint || state->codePoint > 0x10FFFF ||
         (state->codePoint >= 0xD800 && state->codePoint <= 0xDFFF))) {
      return false;
    }
  }

  return true;
}

bool validateUtf8(const uint8_t* bytes, size_t len) {
  Utf8State state;
  return feedUtf8(&state, bytes, len) && state.remainingBytes == 0;
}

bool validateText(QuizInput& input, uint32_t offset, uint32_t len) {
  uint8_t buffer[128];
  Utf8State state;
  for (uint32_t readBytes = 0; readBytes < len;) {
    const uint32_t chunkBytes = std::min<uint32_t>(len - readBytes, sizeof(buffer));
    if (!readExact(input, offset + readBytes, buffer, chunkBytes) || !feedUtf8(&state, buffer, chunkBytes)) {
      return false;
    }
    readBytes += chunkBytes;
  }
  return state.remainingBytes == 0;
}

template <size_t N>
bool loadText(QuizInput& input, uint32_t offset, uint16_t len, std::array<char, N>& out) {
  if (static_cast<size_t>(len) + 1 > out.size() || !readExact(input, offset, out.data(), len) ||
      !validateUtf8(reinterpret_cast<const uint8_t*>(out.data()), len)) {
    return false;
  }
  out[len] = '\0';
  return true;
}

bool readIndexEntry(QuizInput& input, uint32_t indexOffset, uint32_t ordinal, uint32_t* recordOffset, uint32_t* recordBytes) {
  uint8_t raw[kQuizIndexEntryBytes];
  const uint32_t entryOffset = indexOffset + ordinal * kQuizIndexEntryBytes;
  if (!readExact(input, entryOffset, raw, sizeof(raw))) {
    return false;
  }
  *recordOffset = le32(raw);
  *recordBytes = le32(raw + 4);
  return true;
}

bool allZero(const uint8_t* raw, size_t len) {
  for (size_t i = 0; i < len; ++i) {
    if (raw[i] != 0) {
      return false;
    }
  }
  return true;
}

QuizFormatError parseQuestionMeta(QuizInput& input, uint32_t offset, uint32_t recordBytes, QuestionMeta* out) {
  uint8_t raw[kQuizRecordHeaderBytes];
  if (!readExact(input, offset, raw, sizeof(raw))) {
    return QuizFormatError::ReadError;
  }

  const uint16_t recordHeaderSize = le16(raw + kRecordHeaderSizeOffset);
  out->choiceCount = raw[kRecordChoiceCountOffset];
  out->correctChoiceIndex = raw[kRecordCorrectChoiceOffset];
  out->promptBytes = le16(raw + kRecordPromptLengthOffset);
  out->explanationBytes = le16(raw + kRecordExplanationLengthOffset);
  out->payloadBytes = le32(raw + kRecordPayloadLengthOffset);

  if (recordHeaderSize != kQuizRecordHeaderBytes || out->choiceCount < kQuizMinChoices || out->choiceCount > kQuizMaxChoices ||
      out->correctChoiceIndex >= out->choiceCount || out->promptBytes == 0 || out->promptBytes > kQuizMaxPromptBytes ||
      out->explanationBytes > kQuizMaxExplanationBytes) {
    return QuizFormatError::InvalidQuestion;
  }

  uint32_t expectedPayloadBytes = out->promptBytes + out->explanationBytes;
  for (uint8_t i = 0; i < kQuizMaxChoices; ++i) {
    out->choiceBytes[i] = le16(raw + kRecordChoiceLengthsOffset + i * sizeof(uint16_t));
    if (i < out->choiceCount) {
      if (out->choiceBytes[i] == 0 || out->choiceBytes[i] > kQuizMaxChoiceBytes) {
        return QuizFormatError::InvalidQuestion;
      }
      expectedPayloadBytes += out->choiceBytes[i];
    } else if (out->choiceBytes[i] != 0) {
      return QuizFormatError::InvalidQuestion;
    }
  }

  if (out->payloadBytes != expectedPayloadBytes || recordBytes != kQuizRecordHeaderBytes + out->payloadBytes) {
    return QuizFormatError::InvalidQuestion;
  }

  return QuizFormatError::Ok;
}

QuizFormatError parseDeck(QuizInput& input, QuizDeckInfo* out) {
  if (out == nullptr) {
    return QuizFormatError::InvalidHeader;
  }

  const uint64_t inputSize = input.size();
  if (inputSize < kQuizHeaderBytes) {
    return QuizFormatError::ReadError;
  }
  if (inputSize > kQuizMaxFileBytes) {
    return QuizFormatError::InvalidHeader;
  }

  uint8_t raw[kQuizHeaderBytes];
  if (!readExact(input, 0, raw, sizeof(raw))) {
    return QuizFormatError::ReadError;
  }
  if (std::memcmp(raw + kHeaderMagicOffset, kQuizMagicBytes.data(), kQuizMagicBytes.size()) != 0) {
    return QuizFormatError::InvalidHeader;
  }
  if (le16(raw + kHeaderVersionOffset) != kQuizFormatVersion) {
    return QuizFormatError::UnsupportedVersion;
  }

  out->declaredFileSize = le32(raw + kHeaderDeclaredFileSizeOffset);
  out->questionCount = le32(raw + kHeaderQuestionCountOffset);
  out->indexOffset = le32(raw + kHeaderIndexOffsetOffset);
  out->indexBytes = le32(raw + kHeaderIndexBytesOffset);
  out->recordsOffset = le32(raw + kHeaderRecordsOffsetOffset);
  out->titleBytes = le16(raw + kHeaderTitleLengthOffset);

  const uint16_t headerSize = le16(raw + kHeaderSizeOffset);
  const uint32_t flags = le32(raw + kHeaderFlagsOffset);
  const uint16_t indexEntrySize = le16(raw + kHeaderIndexEntrySizeOffset);
  if (out->questionCount == 0 || out->questionCount > kQuizMaxQuestions) {
    return QuizFormatError::InvalidHeader;
  }

  const uint32_t expectedIndexBytes = out->questionCount * kQuizIndexEntryBytes;
  const uint32_t expectedRecordsOffset = kQuizHeaderBytes + expectedIndexBytes;

  if (headerSize != kQuizHeaderBytes || flags != 0 || out->declaredFileSize != inputSize || out->indexOffset != kQuizHeaderBytes ||
      out->indexBytes != expectedIndexBytes || out->recordsOffset != expectedRecordsOffset ||
      indexEntrySize != kQuizIndexEntryBytes || out->titleBytes == 0 ||
      out->titleBytes > kQuizMaxTitleBytes || allZero(raw + kHeaderDeckIdentityOffset, kQuizIdentityBytes) ||
      allZero(raw + kHeaderRevisionIdentityOffset, kQuizIdentityBytes) ||
      !allZero(raw + kHeaderTitleStorageOffset + out->titleBytes, kQuizMaxTitleBytes - out->titleBytes) ||
      !allZero(raw + kHeaderReservedOffset, kQuizHeaderBytes - kHeaderReservedOffset)) {
    return QuizFormatError::InvalidHeader;
  }

  if (out->recordsOffset > out->declaredFileSize || !validateUtf8(raw + kHeaderTitleStorageOffset, out->titleBytes)) {
    return QuizFormatError::InvalidHeader;
  }

  out->recordsBytes = out->declaredFileSize - out->recordsOffset;
  if (out->recordsBytes == 0) {
    return QuizFormatError::InvalidHeader;
  }

  std::memcpy(out->deckId.data(), raw + kHeaderDeckIdentityOffset, kQuizIdentityBytes);
  std::memcpy(out->revisionId.data(), raw + kHeaderRevisionIdentityOffset, kQuizIdentityBytes);
  std::memcpy(out->title.data(), raw + kHeaderTitleStorageOffset, out->titleBytes);
  out->title[out->titleBytes] = '\0';
  return QuizFormatError::Ok;
}

QuizFormatError validateQuestion(QuizInput& input, uint32_t offset, uint32_t recordBytes) {
  QuestionMeta meta;
  QuizFormatError error = parseQuestionMeta(input, offset, recordBytes, &meta);
  if (error != QuizFormatError::Ok) {
    return error;
  }

  uint32_t textOffset = offset + kQuizRecordHeaderBytes;
  if (!validateText(input, textOffset, meta.promptBytes)) {
    return QuizFormatError::InvalidText;
  }
  textOffset += meta.promptBytes;

  for (uint8_t i = 0; i < meta.choiceCount; ++i) {
    if (!validateText(input, textOffset, meta.choiceBytes[i])) {
      return QuizFormatError::InvalidText;
    }
    textOffset += meta.choiceBytes[i];
  }

  if (meta.explanationBytes != 0 && !validateText(input, textOffset, meta.explanationBytes)) {
    return QuizFormatError::InvalidText;
  }
  return QuizFormatError::Ok;
}

}  // namespace

QuizFormatError QuizFormatReader::validate(QuizDeckInfo* outDeck) {
  QuizFormatError error = parseDeck(input_, outDeck);
  if (error != QuizFormatError::Ok) {
    return error;
  }

  const uint32_t recordsLimit = outDeck->recordsOffset + outDeck->recordsBytes;
  uint32_t nextRecordOffset = outDeck->recordsOffset;
  for (uint32_t ordinal = 0; ordinal < outDeck->questionCount; ++ordinal) {
    uint32_t recordOffset = 0;
    uint32_t recordBytes = 0;
    if (!readIndexEntry(input_, outDeck->indexOffset, ordinal, &recordOffset, &recordBytes)) {
      return QuizFormatError::ReadError;
    }

    uint32_t recordEnd = 0;
    if (recordOffset != nextRecordOffset || !addWithin(recordOffset, recordBytes, recordsLimit, &recordEnd)) {
      return QuizFormatError::InvalidIndex;
    }

    error = validateQuestion(input_, recordOffset, recordBytes);
    if (error != QuizFormatError::Ok) {
      return error;
    }
    nextRecordOffset = recordEnd;
  }

  return nextRecordOffset == recordsLimit ? QuizFormatError::Ok : QuizFormatError::InvalidIndex;
}

QuizFormatError QuizFormatReader::loadQuestion(const QuizDeckInfo& deck, uint32_t ordinal, QuizQuestionData* outQuestion) {
  if (outQuestion == nullptr || ordinal >= deck.questionCount) {
    return QuizFormatError::InvalidIndex;
  }

  uint32_t recordOffset = 0;
  uint32_t recordBytes = 0;
  if (!readIndexEntry(input_, deck.indexOffset, ordinal, &recordOffset, &recordBytes)) {
    return QuizFormatError::ReadError;
  }

  uint32_t recordEnd = 0;
  if (recordOffset < deck.recordsOffset || !addWithin(recordOffset, recordBytes, deck.recordsOffset + deck.recordsBytes, &recordEnd) ||
      recordEnd <= recordOffset) {
    return QuizFormatError::InvalidIndex;
  }

  QuestionMeta meta;
  QuizFormatError error = parseQuestionMeta(input_, recordOffset, recordBytes, &meta);
  if (error != QuizFormatError::Ok) {
    return error;
  }

  *outQuestion = {};
  outQuestion->promptBytes = meta.promptBytes;
  outQuestion->explanationBytes = meta.explanationBytes;
  outQuestion->choiceCount = meta.choiceCount;
  outQuestion->correctChoiceIndex = meta.correctChoiceIndex;

  uint32_t textOffset = recordOffset + kQuizRecordHeaderBytes;
  if (!loadText(input_, textOffset, meta.promptBytes, outQuestion->prompt)) {
    return QuizFormatError::InvalidText;
  }
  textOffset += meta.promptBytes;

  for (uint8_t i = 0; i < meta.choiceCount; ++i) {
    outQuestion->choiceBytes[i] = meta.choiceBytes[i];
    if (!loadText(input_, textOffset, meta.choiceBytes[i], outQuestion->choices[i])) {
      return QuizFormatError::InvalidText;
    }
    textOffset += meta.choiceBytes[i];
  }

  if (meta.explanationBytes != 0 && !loadText(input_, textOffset, meta.explanationBytes, outQuestion->explanation)) {
    return QuizFormatError::InvalidText;
  }
  return QuizFormatError::Ok;
}

}  // namespace quiz
