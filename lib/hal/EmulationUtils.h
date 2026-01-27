#pragma once

#include <Arduino.h>
#include <vector>

namespace EmulationUtils {

// Packet format (from device to host):
// $$CMD_(COMMAND):[(ARG0)][:(ARG1)][:(ARG2)]$$\n
// Host response with either a base64-encoded payload or int64, terminated by newline:
// (BASE64_ENCODED_PAYLOAD)\n
// 123\n

static const char* CMD_DISPLAY  = "DISPLAY";  // arg0: base64-encoded buffer -- no return value
static const char* CMD_FS_LIST  = "FS_LIST";  // arg0: path, arg1: max files -- returns list of filenames, one per line, terminated by empty line
static const char* CMD_FS_READ  = "FS_READ";  // arg0: path, arg1: offset, arg2: length (-1 means read all) -- returns base64-encoded file contents
static const char* CMD_FS_STAT  = "FS_STAT";  // arg0: path -- return file size int64_t; -1 means not found; -2 means is a directory
static const char* CMD_FS_WRITE = "FS_WRITE"; // arg0: path, arg1: base64-encoded data, arg2: offset, arg3: is inplace (0/1) -- return int64_t bytes written
static const char* CMD_FS_MKDIR = "FS_MKDIR"; // arg0: path -- return int64_t: 0=success
static const char* CMD_FS_RM    = "FS_RM";    // arg0: path -- return int64_t: 0=success
static const char* CMD_BUTTON   = "BUTTON";   // arg0: action ("read") -- return int64_t: HalInput state bitmask

static std::string base64_encode(const char* buf, unsigned int bufLen);
static std::vector<uint8_t> base64_decode(const char* encoded_string, unsigned int in_len);

static SemaphoreHandle_t sendMutex = xSemaphoreCreateMutex();

static void sendCmd(const char* cmd, const char* arg0 = nullptr, const char* arg1 = nullptr, const char* arg2 = nullptr, const char* arg3 = nullptr) {
  xSemaphoreTake(sendMutex, portMAX_DELAY);
  if (cmd != CMD_BUTTON) {
    Serial.printf("[%lu] [EMU] Sending command: %s\n", millis(), cmd);
  }
  Serial.print("$$CMD_");
  Serial.print(cmd);
  if (arg0 != nullptr) {
    Serial.print(":");
    Serial.print(arg0);
  } else {
    Serial.print(":"); // ensure at least one colon
  }
  if (arg1 != nullptr) {
    Serial.print(":");
    Serial.print(arg1);
  }
  if (arg2 != nullptr) {
    Serial.print(":");
    Serial.print(arg2);
  }
  if (arg3 != nullptr) {
    Serial.print(":");
    Serial.print(arg3);
  }
  Serial.print("$$\n");
  xSemaphoreGive(sendMutex);
}

static String recvRespStr() {
  unsigned long startMillis = millis();
  String line;
  const unsigned long timeoutMs = 5000;
  while (millis() - startMillis < (unsigned long)timeoutMs) {
    if (Serial.available()) {
      char c = Serial.read();
      if (c == '\n') {
        return line;
      } else {
        line += c;
      }
    }
  }
  // should never reach here
  Serial.println("FATAL: Timeout waiting for response");
  return String();
}

static std::vector<uint8_t> recvRespBuf() {
  String respStr = recvRespStr();
  return base64_decode(respStr.c_str(), respStr.length());
}

static int64_t recvRespInt64() {
  String respStr = recvRespStr();
  return respStr.toInt();
}

static const std::string base64_chars =
             "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
             "abcdefghijklmnopqrstuvwxyz"
             "0123456789+/";

static inline bool is_base64(char c) {
  return (isalnum(c) || (c == '+') || (c == '/'));
}

static std::string base64_encode(const char* buf, unsigned int bufLen) {
  std::string ret;
  ret.reserve(bufLen * 4 / 3 + 4); // reserve enough space
  int i = 0;
  int j = 0;
  char char_array_3[3];
  char char_array_4[4];

  while (bufLen--) {
    char_array_3[i++] = *(buf++);
    if (i == 3) {
      char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
      char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
      char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
      char_array_4[3] = char_array_3[2] & 0x3f;

      for(i = 0; (i < 4) ; i++) {
        ret += base64_chars[char_array_4[i]];
      }
      i = 0;
    }
  }

  if (i) {
    for(j = i; j < 3; j++) {
      char_array_3[j] = '\0';
    }

    char_array_4[0] = (char_array_3[0] & 0xfc) >> 2;
    char_array_4[1] = ((char_array_3[0] & 0x03) << 4) + ((char_array_3[1] & 0xf0) >> 4);
    char_array_4[2] = ((char_array_3[1] & 0x0f) << 2) + ((char_array_3[2] & 0xc0) >> 6);
    char_array_4[3] = char_array_3[2] & 0x3f;

    for (j = 0; (j < i + 1); j++) {
      ret += base64_chars[char_array_4[j]];
    }

    while((i++ < 3)) {
      ret += '=';
    }
  }

  return ret;
}

static std::vector<uint8_t> base64_decode(const char* encoded_string, unsigned int in_len) {
  int i = 0;
  int j = 0;
  int in_ = 0;
  char char_array_4[4], char_array_3[3];
  std::vector<uint8_t> ret;

  while (in_len-- && ( encoded_string[in_] != '=') && is_base64(encoded_string[in_])) {
    char_array_4[i++] = encoded_string[in_]; in_++;
    if (i ==4) {
      for (i = 0; i < 4; i++) {
        char_array_4[i] = base64_chars.find(char_array_4[i]);
      }

      char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
      char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
      char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

      for (i = 0; (i < 3); i++) {
        ret.push_back(char_array_3[i]);
      }
      i = 0;
    }
  }

  if (i) {
    for (j = i; j < 4; j++) {
      char_array_4[j] = 0;
    }

    for (j = 0; j < 4; j++) {
      char_array_4[j] = base64_chars.find(char_array_4[j]);
    }

    char_array_3[0] = (char_array_4[0] << 2) + ((char_array_4[1] & 0x30) >> 4);
    char_array_3[1] = ((char_array_4[1] & 0xf) << 4) + ((char_array_4[2] & 0x3c) >> 2);
    char_array_3[2] = ((char_array_4[2] & 0x3) << 6) + char_array_4[3];

    for (j = 0; (j < i - 1); j++) {
      ret.push_back(char_array_3[j]);
    }
  }

  return ret;
}

} // namespace EmulationUtils
