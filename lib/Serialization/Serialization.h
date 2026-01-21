#pragma once
#include <SdFat.h>

#include <cstring>
#include <iostream>

// Debug logging for serialization corruption investigation
// Set to 1 to enable validation of Thai UTF-8 strings during serialize/deserialize
#define SERIALIZATION_THAI_VALIDATION 0

#if SERIALIZATION_THAI_VALIDATION
#include <Arduino.h>
#endif

namespace serialization {

#if SERIALIZATION_THAI_VALIDATION
// Check if a byte sequence contains Thai UTF-8 with potential corruption
// Returns true if corruption is detected (null bytes within Thai sequences)
static bool checkThaiCorruption(const std::string& s, const char* context) {
  const uint8_t* ptr = reinterpret_cast<const uint8_t*>(s.data());
  const uint8_t* end = ptr + s.size();

  while (ptr < end) {
    // Check for Thai UTF-8 start byte (0xE0 for Thai block U+0E00-U+0EFF)
    if (*ptr == 0xE0 && (ptr + 3) <= end) {
      // Thai characters are 0xE0 0xB8/0xB9 XX
      if (ptr[1] == 0x00 || ptr[2] == 0x00) {
        // Null byte in Thai sequence = corruption
        Serial.printf("[SER] %s CORRUPT Thai @ offset %u: %02X %02X %02X\n",
                      context, (uint32_t)(ptr - reinterpret_cast<const uint8_t*>(s.data())),
                      ptr[0], ptr[1], ptr[2]);
        return true;
      }
    }
    ptr++;
  }
  return false;
}
#endif
template <typename T>
static void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
static void writePod(FsFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
static void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
static void readPod(FsFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

static void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

static void writeString(FsFile& file, const std::string& s) {
  const uint32_t len = s.size();
#if SERIALIZATION_THAI_VALIDATION
  // Check for corruption BEFORE writing to file
  checkThaiCorruption(s, "WRITE_BEFORE");
#endif
  // Remember position for read-back verification
  uint64_t writePos = file.curPosition();

  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);

#if SERIALIZATION_THAI_VALIDATION
  // Read-back verification: immediately read what we just wrote to detect SD card issues
  if (len > 0 && len <= 64) {
    uint64_t afterWritePos = file.curPosition();
    file.seek(writePos + sizeof(uint32_t));  // Skip the length prefix

    uint8_t verifyBuf[64];
    size_t bytesRead = file.read(verifyBuf, len);

    // Check if read-back matches what we wrote
    bool mismatch = false;
    if (bytesRead == len) {
      for (size_t i = 0; i < len; i++) {
        if (verifyBuf[i] != static_cast<uint8_t>(s[i])) {
          mismatch = true;
          Serial.printf("[SER] READBACK MISMATCH @ %u: wrote %02X, read %02X\n",
                        (uint32_t)i, static_cast<uint8_t>(s[i]), verifyBuf[i]);
        }
      }
    } else {
      Serial.printf("[SER] READBACK: partial read %u/%u\n", (uint32_t)bytesRead, len);
    }

    // Restore file position
    file.seek(afterWritePos);
  }
#endif
}

static void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

static void readString(FsFile& file, std::string& s) {
  uint32_t len;
  readPod(file, len);

  // Sanity check: prevent unreasonably large allocations (max 64KB per string)
  if (len > 65536) {
    Serial.printf("[SER] readString: length %u exceeds maximum, truncating\n", len);
    len = 0;
  }

  s.resize(len);
  if (len > 0) {
#if SERIALIZATION_THAI_VALIDATION
    // Use a temporary stack buffer to isolate file.read() from std::string memory
    // This helps determine if corruption is in file.read() or in memory management
    uint8_t tempBuf[64];
    bool useTempBuf = (len <= sizeof(tempBuf));

    if (useTempBuf) {
      // Read into temporary buffer first
      size_t bytesRead = file.read(tempBuf, len);
      if (bytesRead != len) {
        Serial.printf("[SER] readString: partial read %u/%u bytes\n", (uint32_t)bytesRead, len);
      }

      // Check temp buffer IMMEDIATELY after read
      bool corruptInTemp = false;
      for (size_t i = 0; i + 2 < len; i++) {
        if (tempBuf[i] == 0xE0 && (tempBuf[i+1] == 0x00 || tempBuf[i+2] == 0x00)) {
          Serial.printf("[SER] READ_TEMPBUF CORRUPT @ %u: %02X %02X %02X\n",
                        (uint32_t)i, tempBuf[i], tempBuf[i+1], tempBuf[i+2]);
          corruptInTemp = true;
        }
      }

      // Copy to string
      memcpy(&s[0], tempBuf, len);

      // Check string after copy
      if (!corruptInTemp) {
        checkThaiCorruption(s, "READ_AFTER_COPY");
      }
    } else {
      // Large string - read directly
      size_t bytesRead = file.read(&s[0], len);
      if (bytesRead != len) {
        Serial.printf("[SER] readString: partial read %u/%u bytes\n", (uint32_t)bytesRead, len);
        if (bytesRead < len) {
          memset(&s[bytesRead], 0, len - bytesRead);
        }
      }
      checkThaiCorruption(s, "READ_AFTER");
    }
#else
    size_t bytesRead = file.read(&s[0], len);
    if (bytesRead != len) {
      Serial.printf("[SER] readString: partial read %u/%u bytes\n", (uint32_t)bytesRead, len);
      // Zero-fill any unread portion to avoid garbage data
      if (bytesRead < len) {
        memset(&s[bytesRead], 0, len - bytesRead);
      }
    }
#endif
  }
}
}  // namespace serialization
