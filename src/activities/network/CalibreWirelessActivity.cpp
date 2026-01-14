#include "CalibreWirelessActivity.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <WiFi.h>

#include <cstring>

#include "MappedInputManager.h"
#include "ScreenComponents.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr uint16_t UDP_PORTS[] = {54982, 48123, 39001, 44044, 59678};
constexpr uint16_t LOCAL_UDP_PORT = 8134;
}  // namespace

void CalibreWirelessActivity::displayTaskTrampoline(void* param) {
  static_cast<CalibreWirelessActivity*>(param)->displayTaskLoop();
}

void CalibreWirelessActivity::networkTaskTrampoline(void* param) {
  static_cast<CalibreWirelessActivity*>(param)->networkTaskLoop();
}

void CalibreWirelessActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  stateMutex = xSemaphoreCreateMutex();

  state = WirelessState::DISCOVERING;
  statusMessage = "Discovering Calibre...";
  errorMessage.clear();
  calibreHostname.clear();
  calibreHost.clear();
  calibrePort = 0;
  calibreAltPort = 0;
  currentFilename.clear();
  currentFileSize = 0;
  bytesReceived = 0;
  inBinaryMode = false;
  recvBuffer.clear();
  inSkipMode = false;
  skipBytesRemaining = 0;
  skipOpcode = -1;
  skipExtractedLpath.clear();
  skipExtractedLength = 0;
  shouldExit = false;

  updateRequired = true;

  udp.begin(LOCAL_UDP_PORT);

  xTaskCreate(&CalibreWirelessActivity::displayTaskTrampoline, "CalDisplayTask", 2048, this, 1, &displayTaskHandle);
  xTaskCreate(&CalibreWirelessActivity::networkTaskTrampoline, "CalNetworkTask", 12288, this, 2, &networkTaskHandle);
}

void CalibreWirelessActivity::onExit() {
  Activity::onExit();

  shouldExit = true;
  vTaskDelay(50 / portTICK_PERIOD_MS);

  if (tcpClient.connected()) {
    tcpClient.stop();
  }
  udp.stop();

  vTaskDelay(250 / portTICK_PERIOD_MS);

  networkTaskHandle = nullptr;
  displayTaskHandle = nullptr;

  WiFi.mode(WIFI_OFF);

  if (currentFile) {
    currentFile.close();
  }

  recvBuffer.clear();
  recvBuffer.shrink_to_fit();
  skipExtractedLpath.clear();
  skipExtractedLpath.shrink_to_fit();

  if (renderingMutex) {
    vSemaphoreDelete(renderingMutex);
    renderingMutex = nullptr;
  }

  if (stateMutex) {
    vSemaphoreDelete(stateMutex);
    stateMutex = nullptr;
  }
}

void CalibreWirelessActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onCompleteCallback();
    return;
  }
}

void CalibreWirelessActivity::displayTaskLoop() {
  while (!shouldExit) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      if (!shouldExit) {
        render();
      }
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
  vTaskDelete(nullptr);
}

void CalibreWirelessActivity::networkTaskLoop() {
  while (!shouldExit) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto currentState = state;
    xSemaphoreGive(stateMutex);

    if (shouldExit) break;

    switch (currentState) {
      case WirelessState::DISCOVERING:
        listenForDiscovery();
        break;
      case WirelessState::CONNECTING:
      case WirelessState::WAITING:
      case WirelessState::RECEIVING:
        handleTcpClient();
        break;
      case WirelessState::COMPLETE:
      case WirelessState::DISCONNECTED:
      case WirelessState::ERROR:
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
  vTaskDelete(nullptr);
}

void CalibreWirelessActivity::listenForDiscovery() {
  for (const uint16_t port : UDP_PORTS) {
    udp.beginPacket("255.255.255.255", port);
    udp.write(reinterpret_cast<const uint8_t*>("hello"), 5);
    udp.endPacket();
  }

  vTaskDelay(500 / portTICK_PERIOD_MS);

  const int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char buffer[256];
    const int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = '\0';
      std::string response(buffer);

      size_t onPos = response.find("(on ");
      size_t closePos = response.find(')');
      size_t semiPos = response.find(';');
      size_t commaPos = response.find(',', semiPos);

      if (semiPos != std::string::npos) {
        std::string portStr;
        if (commaPos != std::string::npos && commaPos > semiPos) {
          portStr = response.substr(semiPos + 1, commaPos - semiPos - 1);
          uint16_t altPort = 0;
          for (size_t i = commaPos + 1; i < response.size(); i++) {
            char c = response[i];
            if (c >= '0' && c <= '9') {
              altPort = altPort * 10 + (c - '0');
            } else {
              break;
            }
          }
          calibreAltPort = altPort;
        } else {
          portStr = response.substr(semiPos + 1);
        }

        uint16_t mainPort = 0;
        for (char c : portStr) {
          if (c >= '0' && c <= '9') {
            mainPort = mainPort * 10 + (c - '0');
          } else if (c != ' ' && c != '\t') {
            break;
          }
        }
        calibrePort = mainPort;

        if (onPos != std::string::npos && closePos != std::string::npos && closePos > onPos + 4) {
          calibreHostname = response.substr(onPos + 4, closePos - onPos - 4);
        }
      }

      calibreHost = udp.remoteIP().toString().c_str();
      if (calibreHostname.empty()) {
        calibreHostname = calibreHost;
      }

      if (calibrePort > 0) {
        setState(WirelessState::CONNECTING);
        setStatus("Connecting to " + calibreHostname + "...");

        vTaskDelay(100 / portTICK_PERIOD_MS);

        Serial.printf("[%lu] [CAL] Connecting to %s:%d\n", millis(), calibreHost.c_str(), calibrePort);
        if (tcpClient.connect(calibreHost.c_str(), calibrePort, 5000)) {
          Serial.printf("[%lu] [CAL] Connected!\n", millis());
          setState(WirelessState::WAITING);
          setStatus("Connected to " + calibreHostname + "\nWaiting for commands...");
        } else if (calibreAltPort > 0 && tcpClient.connect(calibreHost.c_str(), calibreAltPort, 5000)) {
          Serial.printf("[%lu] [CAL] Connected on alt port!\n", millis());
          setState(WirelessState::WAITING);
          setStatus("Connected to " + calibreHostname + "\nWaiting for commands...");
        } else {
          Serial.printf("[%lu] [CAL] Connection failed\n", millis());
          setState(WirelessState::DISCOVERING);
          setStatus("Discovering Calibre...\n(Connection failed, retrying)");
          calibrePort = 0;
          calibreAltPort = 0;
        }
      }
    }
  }
}

void CalibreWirelessActivity::handleTcpClient() {
  if (!tcpClient.connected()) {
    setState(WirelessState::DISCONNECTED);
    setStatus("Calibre disconnected");
    return;
  }

  if (inBinaryMode) {
    receiveBinaryData();
    return;
  }

  std::string message;
  if (readJsonMessage(message)) {
    size_t start = message.find('[');
    if (start != std::string::npos) {
      start++;
      size_t end = message.find(',', start);
      if (end != std::string::npos) {
        int opcodeInt = 0;
        for (size_t i = start; i < end; i++) {
          char c = message[i];
          if (c >= '0' && c <= '9') {
            opcodeInt = opcodeInt * 10 + (c - '0');
          } else if (c != ' ' && c != '\t') {
            break;
          }
        }

        if (opcodeInt >= 0 && opcodeInt <= OpCode::ERROR) {
          auto opcode = static_cast<OpCode>(opcodeInt);
          size_t dataStart = end + 1;
          size_t dataEnd = message.rfind(']');
          std::string data;
          if (dataEnd != std::string::npos && dataEnd > dataStart) {
            data = message.substr(dataStart, dataEnd - dataStart);
          }
          handleCommand(opcode, data);
        }
      }
    }
  }
}

bool CalibreWirelessActivity::readJsonMessage(std::string& message) {
  constexpr size_t MAX_BUFFERED_MSG_SIZE = 32768;

  // Handle skip mode for large messages
  if (inSkipMode) {
    while (skipBytesRemaining > 0 && tcpClient.available() > 0) {
      uint8_t discardBuf[1024];
      size_t toRead = std::min({static_cast<size_t>(tcpClient.available()), sizeof(discardBuf), skipBytesRemaining});
      int bytesRead = tcpClient.read(discardBuf, toRead);
      if (bytesRead > 0) {
        skipBytesRemaining -= bytesRead;
      } else {
        break;
      }
    }

    if (skipBytesRemaining == 0) {
      inSkipMode = false;
      if (skipOpcode == OpCode::SEND_BOOK && !skipExtractedLpath.empty() && skipExtractedLength > 0) {
        message = "[" + std::to_string(skipOpcode) + ",{\"lpath\":\"" + skipExtractedLpath +
                  "\",\"length\":" + std::to_string(skipExtractedLength) + "}]";
        skipOpcode = -1;
        skipExtractedLpath.clear();
        skipExtractedLength = 0;
        return true;
      }
      if (skipOpcode >= 0) {
        message = "[" + std::to_string(skipOpcode) + ",{}]";
        skipOpcode = -1;
        return true;
      }
    }
    return false;
  }

  // Read available data into buffer
  int available = tcpClient.available();
  if (available > 0) {
    size_t maxBuffer = MAX_BUFFERED_MSG_SIZE + 20;
    if (recvBuffer.size() < maxBuffer) {
      char buf[1024];
      size_t spaceLeft = maxBuffer - recvBuffer.size();
      while (available > 0 && spaceLeft > 0) {
        int toRead = std::min({available, static_cast<int>(sizeof(buf)), static_cast<int>(spaceLeft)});
        int bytesRead = tcpClient.read(reinterpret_cast<uint8_t*>(buf), toRead);
        if (bytesRead > 0) {
          recvBuffer.append(buf, bytesRead);
          available -= bytesRead;
          spaceLeft -= bytesRead;
        } else {
          break;
        }
      }
    }
  }

  if (recvBuffer.empty()) {
    return false;
  }

  size_t bracketPos = recvBuffer.find('[');
  if (bracketPos == std::string::npos) {
    if (recvBuffer.size() > 1000) {
      recvBuffer.clear();
    }
    return false;
  }

  size_t msgLen = 0;
  bool validPrefix = false;

  if (bracketPos > 0 && bracketPos <= 12) {
    bool allDigits = true;
    size_t parsedLen = 0;
    for (size_t i = 0; i < bracketPos; i++) {
      char c = recvBuffer[i];
      if (c >= '0' && c <= '9') {
        parsedLen = parsedLen * 10 + (c - '0');
      } else {
        allDigits = false;
        break;
      }
    }
    if (allDigits) {
      msgLen = parsedLen;
      validPrefix = true;
    }
  }

  if (!validPrefix) {
    if (bracketPos > 0) {
      recvBuffer = recvBuffer.substr(bracketPos);
    }
    return false;
  }

  if (msgLen > 10000000) {
    recvBuffer.clear();
    return false;
  }

  // Handle large messages
  if (msgLen > MAX_BUFFERED_MSG_SIZE) {
    Serial.printf("[%lu] [CAL] Large message (%zu bytes), streaming\n", millis(), msgLen);

    int opcodeInt = -1;
    size_t opcodeStart = bracketPos + 1;
    size_t commaPos = recvBuffer.find(',', opcodeStart);
    if (commaPos != std::string::npos) {
      opcodeInt = 0;
      for (size_t i = opcodeStart; i < commaPos; i++) {
        char c = recvBuffer[i];
        if (c >= '0' && c <= '9') {
          opcodeInt = opcodeInt * 10 + (c - '0');
        } else if (c != ' ' && c != '\t') {
          break;
        }
      }
    }

    skipOpcode = opcodeInt;
    skipExtractedLpath.clear();
    skipExtractedLength = 0;

    if (opcodeInt == OpCode::SEND_BOOK) {
      size_t lpathPos = recvBuffer.find("\"lpath\"");
      if (lpathPos != std::string::npos) {
        size_t colonPos = recvBuffer.find(':', lpathPos + 7);
        if (colonPos != std::string::npos) {
          size_t quoteStart = recvBuffer.find('"', colonPos + 1);
          if (quoteStart != std::string::npos) {
            size_t quoteEnd = recvBuffer.find('"', quoteStart + 1);
            if (quoteEnd != std::string::npos) {
              skipExtractedLpath = recvBuffer.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
          }
        }
      }

      int depth = 0;
      const char* lengthKey = "\"length\"";
      for (size_t i = bracketPos; i < recvBuffer.size() && i < bracketPos + 2000; i++) {
        char c = recvBuffer[i];
        if (c == '{' || c == '[') depth++;
        else if (c == '}' || c == ']') depth--;
        else if (depth == 2 && c == '"' && i + 8 <= recvBuffer.size()) {
          bool match = true;
          for (size_t j = 0; j < 8 && match; j++) {
            if (recvBuffer[i + j] != lengthKey[j]) match = false;
          }
          if (match) {
            size_t numStart = i + 8;
            while (numStart < recvBuffer.size() && (recvBuffer[numStart] == ':' || recvBuffer[numStart] == ' ')) {
              numStart++;
            }
            while (numStart < recvBuffer.size() && recvBuffer[numStart] >= '0' && recvBuffer[numStart] <= '9') {
              skipExtractedLength = skipExtractedLength * 10 + (recvBuffer[numStart] - '0');
              numStart++;
            }
            break;
          }
        }
      }
    }

    size_t totalMsgBytes = bracketPos + msgLen;
    if (recvBuffer.size() >= totalMsgBytes) {
      recvBuffer = recvBuffer.substr(totalMsgBytes);
      if (skipOpcode == OpCode::SEND_BOOK && !skipExtractedLpath.empty() && skipExtractedLength > 0) {
        message = "[" + std::to_string(skipOpcode) + ",{\"lpath\":\"" + skipExtractedLpath +
                  "\",\"length\":" + std::to_string(skipExtractedLength) + "}]";
        skipOpcode = -1;
        skipExtractedLpath.clear();
        skipExtractedLength = 0;
        return true;
      }
      if (skipOpcode >= 0) {
        message = "[" + std::to_string(skipOpcode) + ",{}]";
        skipOpcode = -1;
        return true;
      }
    } else {
      skipBytesRemaining = totalMsgBytes - recvBuffer.size();
      recvBuffer.clear();
      inSkipMode = true;
    }
    return false;
  }

  size_t totalNeeded = bracketPos + msgLen;
  if (recvBuffer.size() < totalNeeded) {
    return false;
  }

  message = recvBuffer.substr(bracketPos, msgLen);
  recvBuffer = recvBuffer.size() > totalNeeded ? recvBuffer.substr(totalNeeded) : "";

  return true;
}

void CalibreWirelessActivity::sendJsonResponse(const OpCode opcode, const std::string& data) {
  std::string json = "[" + std::to_string(opcode) + "," + data + "]";
  std::string msg = std::to_string(json.length()) + json;
  tcpClient.write(reinterpret_cast<const uint8_t*>(msg.c_str()), msg.length());
  tcpClient.flush();
}

void CalibreWirelessActivity::handleCommand(const OpCode opcode, const std::string& data) {
  Serial.printf("[%lu] [CAL] Command: %d, data size: %zu\n", millis(), opcode, data.size());

  switch (opcode) {
    case OpCode::GET_INITIALIZATION_INFO:
      handleGetInitializationInfo(data);
      break;
    case OpCode::GET_DEVICE_INFORMATION:
      handleGetDeviceInformation();
      break;
    case OpCode::FREE_SPACE:
    case OpCode::TOTAL_SPACE:
      handleFreeSpace();
      break;
    case OpCode::GET_BOOK_COUNT:
      handleGetBookCount();
      break;
    case OpCode::SEND_BOOK:
      handleSendBook(data);
      break;
    case OpCode::SEND_BOOK_METADATA:
      handleSendBookMetadata(data);
      break;
    case OpCode::DISPLAY_MESSAGE:
      handleDisplayMessage(data);
      break;
    case OpCode::NOOP:
      handleNoop(data);
      break;
    case OpCode::SET_CALIBRE_DEVICE_INFO:
    case OpCode::SET_CALIBRE_DEVICE_NAME:
    case OpCode::SET_LIBRARY_INFO:
    case OpCode::SEND_BOOKLISTS:
      sendJsonResponse(OpCode::OK, "{}");
      break;
    default:
      sendJsonResponse(OpCode::OK, "{}");
      break;
  }
}

void CalibreWirelessActivity::receiveBinaryData() {
  // KOReader-style: read all available data, write only what we need to file,
  // put excess (next JSON message) back into buffer.

  int available = tcpClient.available();
  if (available <= 0) {
    // Brief wait for more data
    vTaskDelay(1);
    return;
  }

  uint8_t buffer[4096];
  int bytesRead = tcpClient.read(buffer, std::min(sizeof(buffer), static_cast<size_t>(available)));

  if (bytesRead <= 0) {
    return;
  }

  // Write only what we need (like KOReader's data:sub(1, to_write_bytes))
  size_t toWrite = std::min(static_cast<size_t>(bytesRead), binaryBytesRemaining);

  if (toWrite > 0) {
    currentFile.write(buffer, toWrite);
    bytesReceived += toWrite;
    binaryBytesRemaining -= toWrite;
    updateRequired = true;
  }

  // If we read more than needed, it's the next JSON message (like KOReader's buffer handling)
  if (static_cast<size_t>(bytesRead) > toWrite) {
    size_t excess = bytesRead - toWrite;
    recvBuffer.assign(reinterpret_cast<char*>(buffer + toWrite), excess);
    Serial.printf("[%lu] [CAL] Binary done, %zu excess bytes -> buffer\n", millis(), excess);
  }

  // Progress logging
  static unsigned long lastLog = 0;
  unsigned long now = millis();
  if (now - lastLog > 500) {
    Serial.printf("[%lu] [CAL] Binary: %zu/%zu (%.1f%%)\n", now, bytesReceived, currentFileSize,
                  currentFileSize > 0 ? (100.0 * bytesReceived / currentFileSize) : 0.0);
    lastLog = now;
  }

  // Check completion
  if (binaryBytesRemaining == 0) {
    currentFile.flush();
    currentFile.close();
    inBinaryMode = false;

    Serial.printf("[%lu] [CAL] File complete: %zu bytes\n", millis(), bytesReceived);
    setState(WirelessState::WAITING);
    setStatus("Received: " + currentFilename + "\nWaiting for more...");
  }
}

void CalibreWirelessActivity::handleGetInitializationInfo(const std::string& data) {
  setState(WirelessState::WAITING);
  setStatus("Connected to " + calibreHostname +
            "\nWaiting for transfer...\n\nIf transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice "
            "plugin settings.");

  std::string response = "{";
  response += "\"appName\":\"CrossPoint\",";
  response += "\"acceptedExtensions\":[\"epub\"],";
  response += "\"cacheUsesLpaths\":true,";
  response += "\"canAcceptLibraryInfo\":true,";
  response += "\"canDeleteMultipleBooks\":true,";
  response += "\"canReceiveBookBinary\":true,";
  response += "\"canSendOkToSendbook\":true,";
  response += "\"canStreamBooks\":true,";
  response += "\"canStreamMetadata\":true,";
  response += "\"canUseCachedMetadata\":true,";
  response += "\"ccVersionNumber\":212,";
  response += "\"coverHeight\":0,";
  response += "\"deviceKind\":\"CrossPoint\",";
  response += "\"deviceName\":\"CrossPoint\",";
  response += "\"extensionPathLengths\":{\"epub\":37},";
  response += "\"maxBookContentPacketLen\":4096,";
  response += "\"passwordHash\":\"\",";
  response += "\"useUuidFileNames\":false,";
  response += "\"versionOK\":true";
  response += "}";

  sendJsonResponse(OpCode::OK, response);
}

void CalibreWirelessActivity::handleGetDeviceInformation() {
  std::string response = "{";
  response += "\"device_info\":{";
  response += "\"device_store_uuid\":\"" + getDeviceUuid() + "\",";
  response += "\"device_name\":\"CrossPoint Reader\",";
  response += "\"device_version\":\"" CROSSPOINT_VERSION "\"";
  response += "},";
  response += "\"version\":1,";
  response += "\"device_version\":\"" CROSSPOINT_VERSION "\"";
  response += "}";

  sendJsonResponse(OpCode::OK, response);
}

void CalibreWirelessActivity::handleFreeSpace() {
  sendJsonResponse(OpCode::OK, "{\"free_space_on_device\":10737418240}");
}

void CalibreWirelessActivity::handleGetBookCount() {
  sendJsonResponse(OpCode::OK, "{\"count\":0,\"willStream\":true,\"willScan\":false}");
}

void CalibreWirelessActivity::handleSendBook(const std::string& data) {
  Serial.printf("[%lu] [CAL] SEND_BOOK (first 500): %.500s\n", millis(), data.c_str());

  // Extract lpath
  std::string lpath;
  size_t lpathPos = data.find("\"lpath\"");
  if (lpathPos != std::string::npos) {
    size_t colonPos = data.find(':', lpathPos + 7);
    if (colonPos != std::string::npos) {
      size_t quoteStart = data.find('"', colonPos + 1);
      if (quoteStart != std::string::npos) {
        size_t quoteEnd = data.find('"', quoteStart + 1);
        if (quoteEnd != std::string::npos) {
          lpath = data.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
        }
      }
    }
  }

  // Extract top-level length
  size_t length = 0;
  int depth = 0;
  const char* lengthKey = "\"length\"";

  for (size_t i = 0; i < data.size(); i++) {
    char c = data[i];
    if (c == '{' || c == '[') depth++;
    else if (c == '}' || c == ']') depth--;
    else if (depth == 1 && c == '"' && i + 8 <= data.size()) {
      bool match = true;
      for (size_t j = 0; j < 8 && match; j++) {
        if (data[i + j] != lengthKey[j]) match = false;
      }
      if (match) {
        size_t colonPos = i + 8;
        while (colonPos < data.size() && data[colonPos] != ':') colonPos++;
        if (colonPos < data.size()) {
          size_t numStart = colonPos + 1;
          while (numStart < data.size() && (data[numStart] == ' ' || data[numStart] == '\t')) numStart++;
          while (numStart < data.size() && data[numStart] >= '0' && data[numStart] <= '9') {
            length = length * 10 + (data[numStart] - '0');
            numStart++;
          }
          if (length > 0) {
            Serial.printf("[%lu] [CAL] Extracted length=%zu\n", millis(), length);
            break;
          }
        }
      }
    }
  }

  if (lpath.empty() || length == 0) {
    sendJsonResponse(OpCode::ERROR, "{\"message\":\"Invalid book data\"}");
    return;
  }

  std::string filename = lpath;
  size_t lastSlash = filename.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  currentFilename = "/" + StringUtils::sanitizeFilename(filename);
  if (!StringUtils::checkFileExtension(currentFilename, ".epub")) {
    currentFilename += ".epub";
  }
  currentFileSize = length;
  bytesReceived = 0;
  binaryBytesRemaining = length;

  Serial.printf("[%lu] [CAL] File: %s, size: %zu, buffer: %zu\n", millis(), currentFilename.c_str(), length,
                recvBuffer.size());

  setState(WirelessState::RECEIVING);
  setStatus("Receiving: " + filename);

  if (!SdMan.openFileForWrite("CAL", currentFilename.c_str(), currentFile)) {
    setError("Failed to create file");
    sendJsonResponse(OpCode::ERROR, "{\"message\":\"Failed to create file\"}");
    return;
  }

  // Send OK - Calibre will start sending binary
  sendJsonResponse(OpCode::OK, "{}");

  // Switch to binary mode
  inBinaryMode = true;

  // Process any data already in buffer (like KOReader)
  if (!recvBuffer.empty()) {
    size_t toWrite = std::min(recvBuffer.size(), binaryBytesRemaining);
    Serial.printf("[%lu] [CAL] Writing %zu from buffer\n", millis(), toWrite);
    currentFile.write(reinterpret_cast<const uint8_t*>(recvBuffer.data()), toWrite);
    bytesReceived += toWrite;
    binaryBytesRemaining -= toWrite;

    if (recvBuffer.size() > toWrite) {
      recvBuffer = recvBuffer.substr(toWrite);
    } else {
      recvBuffer.clear();
    }
    updateRequired = true;

    if (binaryBytesRemaining == 0) {
      currentFile.flush();
      currentFile.close();
      inBinaryMode = false;
      Serial.printf("[%lu] [CAL] File complete: %zu bytes\n", millis(), bytesReceived);
      setState(WirelessState::WAITING);
      setStatus("Received: " + currentFilename + "\nWaiting for more...");
    }
  }
}

void CalibreWirelessActivity::handleSendBookMetadata(const std::string& data) {
  Serial.printf("[%lu] [CAL] SEND_BOOK_METADATA\n", millis());
  sendJsonResponse(OpCode::OK, "{}");
}

void CalibreWirelessActivity::handleDisplayMessage(const std::string& data) {
  if (data.find("\"messageKind\":1") != std::string::npos) {
    setError("Password required");
  }
  sendJsonResponse(OpCode::OK, "{}");
}

void CalibreWirelessActivity::handleNoop(const std::string& data) {
  if (data.find("\"ejecting\":true") != std::string::npos) {
    setState(WirelessState::DISCONNECTED);
    setStatus("Calibre disconnected");
  }
  sendJsonResponse(OpCode::NOOP, "{}");
}

void CalibreWirelessActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.drawCenteredText(UI_12_FONT_ID, 30, "Calibre Wireless", true, EpdFontFamily::BOLD);

  const std::string ipAddr = WiFi.localIP().toString().c_str();
  renderer.drawCenteredText(UI_10_FONT_ID, 60, ("IP: " + ipAddr).c_str());

  int statusY = pageHeight / 2 - 40;
  std::string status = statusMessage;
  size_t pos = 0;
  while ((pos = status.find('\n')) != std::string::npos) {
    renderer.drawCenteredText(UI_10_FONT_ID, statusY, status.substr(0, pos).c_str());
    statusY += 25;
    status = status.substr(pos + 1);
  }
  if (!status.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, statusY, status.c_str());
    statusY += 25;
  }

  if (state == WirelessState::RECEIVING && currentFileSize > 0) {
    const int barWidth = pageWidth - 100;
    constexpr int barHeight = 20;
    constexpr int barX = 50;
    const int barY = statusY + 20;
    ScreenComponents::drawProgressBar(renderer, barX, barY, barWidth, barHeight, bytesReceived, currentFileSize);
  }

  if (!errorMessage.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 120, errorMessage.c_str());
  }

  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string CalibreWirelessActivity::getDeviceUuid() const {
  uint8_t mac[6];
  WiFi.macAddress(mac);
  char uuid[37];
  snprintf(uuid, sizeof(uuid), "%02x%02x%02x%02x-%02x%02x-4000-8000-%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2],
           mac[3], mac[4], mac[5], mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
  return std::string(uuid);
}

void CalibreWirelessActivity::setState(WirelessState newState) {
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  state = newState;
  xSemaphoreGive(stateMutex);
  updateRequired = true;
}

void CalibreWirelessActivity::setStatus(const std::string& message) {
  statusMessage = message;
  updateRequired = true;
}

void CalibreWirelessActivity::setError(const std::string& message) {
  errorMessage = message;
  setState(WirelessState::ERROR);
}
