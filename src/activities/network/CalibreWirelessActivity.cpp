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
constexpr uint16_t LOCAL_UDP_PORT = 8134;  // Port to receive responses
}  // namespace

void CalibreWirelessActivity::displayTaskTrampoline(void* param) {
  auto* self = static_cast<CalibreWirelessActivity*>(param);
  self->displayTaskLoop();
}

void CalibreWirelessActivity::networkTaskTrampoline(void* param) {
  auto* self = static_cast<CalibreWirelessActivity*>(param);
  self->networkTaskLoop();
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

  updateRequired = true;

  // Start UDP listener for Calibre responses
  udp.begin(LOCAL_UDP_PORT);

  // Create display task
  xTaskCreate(&CalibreWirelessActivity::displayTaskTrampoline, "CalDisplayTask", 2048, this, 1, &displayTaskHandle);

  // Create network task with larger stack for JSON parsing
  xTaskCreate(&CalibreWirelessActivity::networkTaskTrampoline, "CalNetworkTask", 12288, this, 2, &networkTaskHandle);
}

void CalibreWirelessActivity::onExit() {
  Activity::onExit();

  // Stop network task FIRST before touching any shared state
  // This prevents the task from accessing members while we clean up
  if (networkTaskHandle) {
    vTaskDelete(networkTaskHandle);
    networkTaskHandle = nullptr;
  }

  // Stop display task
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }

  // Now safe to clean up - tasks are stopped
  // Turn off WiFi when exiting
  WiFi.mode(WIFI_OFF);

  // Stop UDP listening
  udp.stop();

  // Close TCP client if connected
  if (tcpClient.connected()) {
    tcpClient.stop();
  }

  // Close any open file
  if (currentFile) {
    currentFile.close();
  }

  // Clear string buffers to free memory
  recvBuffer.clear();
  recvBuffer.shrink_to_fit();
  skipExtractedLpath.clear();
  skipExtractedLpath.shrink_to_fit();

  // Delete mutexes last
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
    onComplete();
    return;
  }
}

void CalibreWirelessActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(50 / portTICK_PERIOD_MS);
  }
}

void CalibreWirelessActivity::networkTaskLoop() {
  while (true) {
    xSemaphoreTake(stateMutex, portMAX_DELAY);
    const auto currentState = state;
    xSemaphoreGive(stateMutex);

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
        // Just wait, user will exit
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CalibreWirelessActivity::listenForDiscovery() {
  // Broadcast "hello" on all UDP discovery ports to find Calibre
  for (const uint16_t port : UDP_PORTS) {
    udp.beginPacket("255.255.255.255", port);
    udp.write(reinterpret_cast<const uint8_t*>("hello"), 5);
    udp.endPacket();
  }

  // Wait for Calibre's response
  vTaskDelay(500 / portTICK_PERIOD_MS);

  // Check for response
  const int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char buffer[256];
    const int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = '\0';

      // Parse Calibre's response format:
      // "calibre wireless device client (on hostname);port,content_server_port"
      // or just the hostname and port info
      std::string response(buffer);

      // Try to extract host and port
      // Format: "calibre wireless device client (on HOSTNAME);PORT,..."
      size_t onPos = response.find("(on ");
      size_t closePos = response.find(')');
      size_t semiPos = response.find(';');
      size_t commaPos = response.find(',', semiPos);

      if (semiPos != std::string::npos) {
        // Get ports after semicolon (format: "port1,port2")
        std::string portStr;
        if (commaPos != std::string::npos && commaPos > semiPos) {
          portStr = response.substr(semiPos + 1, commaPos - semiPos - 1);
          // Get alternative port after comma - parse safely
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

        // Parse main port safely
        uint16_t mainPort = 0;
        for (size_t i = 0; i < portStr.size(); i++) {
          char c = portStr[i];
          if (c >= '0' && c <= '9') {
            mainPort = mainPort * 10 + (c - '0');
          } else if (c != ' ' && c != '\t') {
            break;
          }
        }
        calibrePort = mainPort;

        // Get hostname if present, otherwise use sender IP
        if (onPos != std::string::npos && closePos != std::string::npos && closePos > onPos + 4) {
          calibreHostname = response.substr(onPos + 4, closePos - onPos - 4);
        }
      }

      // Use the sender's IP as the host to connect to
      calibreHost = udp.remoteIP().toString().c_str();
      if (calibreHostname.empty()) {
        calibreHostname = calibreHost;
      }

      if (calibrePort > 0) {
        // Connect to Calibre's TCP server - try main port first, then alt port
        setState(WirelessState::CONNECTING);
        setStatus("Connecting to " + calibreHostname + "...");

        // Small delay before connecting
        vTaskDelay(100 / portTICK_PERIOD_MS);

        bool connected = false;

        // Try main port first
        if (tcpClient.connect(calibreHost.c_str(), calibrePort, 5000)) {
          connected = true;
        }

        // Try alternative port if main failed
        if (!connected && calibreAltPort > 0) {
          vTaskDelay(200 / portTICK_PERIOD_MS);
          if (tcpClient.connect(calibreHost.c_str(), calibreAltPort, 5000)) {
            connected = true;
          }
        }

        if (connected) {
          setState(WirelessState::WAITING);
          setStatus("Connected to " + calibreHostname + "\nWaiting for commands...");
        } else {
          // Don't set error yet, keep trying discovery
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
    // Parse opcode from JSON array format: [opcode, {...}]
    // Find the opcode (first number after '[')
    size_t start = message.find('[');
    if (start != std::string::npos) {
      start++;
      size_t end = message.find(',', start);
      if (end != std::string::npos) {
        // Parse opcode safely without exceptions
        int opcodeInt = 0;
        for (size_t i = start; i < end; i++) {
          char c = message[i];
          if (c >= '0' && c <= '9') {
            opcodeInt = opcodeInt * 10 + (c - '0');
          } else if (c != ' ' && c != '\t') {
            break;  // Invalid character
          }
        }
        if (opcodeInt < 0 || opcodeInt >= OpCode::ERROR) {
          Serial.printf("[%lu] [CAL] Invalid opcode: %d\n", millis(), opcodeInt);
          sendJsonResponse(OpCode::OK, "{}");
          return;
        }
        const auto opcode = static_cast<OpCode>(opcodeInt);

        // Extract data object (everything after the comma until the last ']')
        size_t dataStart = end + 1;
        size_t dataEnd = message.rfind(']');
        std::string data = "";
        if (dataEnd != std::string::npos && dataEnd > dataStart && dataStart < message.size()) {
          size_t len = dataEnd - dataStart;
          if (dataStart + len <= message.size()) {
            data = message.substr(dataStart, len);
          }
        }

        handleCommand(opcode, data);
      }
    }
  }
}

bool CalibreWirelessActivity::readJsonMessage(std::string& message) {
  // Maximum message size we'll buffer in memory
  // Messages larger than this (typically due to base64 covers) are streamed through
  constexpr size_t MAX_BUFFERED_MSG_SIZE = 32768;

  // If in skip mode, consume bytes until we've skipped the full message
  if (inSkipMode) {
    while (skipBytesRemaining > 0) {
      int available = tcpClient.available();
      if (available <= 0) {
        return false;  // Need more data
      }

      // Read and discard in chunks
      uint8_t discardBuf[1024];
      size_t toRead = std::min({static_cast<size_t>(available), sizeof(discardBuf), skipBytesRemaining});
      int bytesRead = tcpClient.read(discardBuf, toRead);
      if (bytesRead > 0) {
        skipBytesRemaining -= bytesRead;
      } else {
        break;
      }
    }

    if (skipBytesRemaining == 0) {
      // Skip complete - if this was SEND_BOOK, construct minimal message
      inSkipMode = false;
      if (skipOpcode == OpCode::SEND_BOOK && !skipExtractedLpath.empty() && skipExtractedLength > 0) {
        // Build minimal JSON that handleSendBook can parse
        message = "[" + std::to_string(skipOpcode) + ",{\"lpath\":\"" + skipExtractedLpath +
                  "\",\"length\":" + std::to_string(skipExtractedLength) + "}]";
        skipOpcode = -1;
        skipExtractedLpath.clear();
        skipExtractedLength = 0;
        return true;
      }
      // For other opcodes, just acknowledge
      if (skipOpcode >= 0) {
        message = "[" + std::to_string(skipOpcode) + ",{}]";
        skipOpcode = -1;
        return true;
      }
    }
    return false;
  }

  // Read available data into buffer (limited to prevent memory issues)
  int available = tcpClient.available();
  if (available > 0) {
    // Only buffer up to a reasonable amount while looking for length prefix
    size_t maxBuffer = MAX_BUFFERED_MSG_SIZE + 20;  // +20 for length prefix digits
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

  // Find '[' which marks the start of JSON
  size_t bracketPos = recvBuffer.find('[');
  if (bracketPos == std::string::npos) {
    if (recvBuffer.size() > 1000) {
      recvBuffer.clear();
    }
    return false;
  }

  // Parse length prefix (digits before '[')
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
    if (bracketPos > 0 && bracketPos < recvBuffer.size()) {
      recvBuffer = recvBuffer.substr(bracketPos);
    }
    return false;
  }

  // Sanity check - reject absurdly large messages
  if (msgLen > 10000000) {
    Serial.printf("[%lu] [CAL] Rejecting message with length %zu (too large)\n", millis(), msgLen);
    recvBuffer.clear();
    return false;
  }

  // For large messages, extract essential fields then skip the rest
  if (msgLen > MAX_BUFFERED_MSG_SIZE) {
    Serial.printf("[%lu] [CAL] Large message detected (%zu bytes), streaming\n", millis(), msgLen);

    // We need to extract: opcode, and for SEND_BOOK: lpath and length
    // These fields appear early in the JSON before the large cover data

    // Parse opcode from what we have buffered
    int opcodeInt = -1;
    size_t opcodeStart = bracketPos + 1;
    size_t commaPos = recvBuffer.find(',', opcodeStart);
    if (commaPos != std::string::npos && commaPos < recvBuffer.size()) {
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

    // For SEND_BOOK, try to extract lpath and length from buffered data
    if (opcodeInt == OpCode::SEND_BOOK) {
      // Extract lpath
      size_t lpathPos = recvBuffer.find("\"lpath\"");
      if (lpathPos != std::string::npos && lpathPos + 7 < recvBuffer.size()) {
        size_t colonPos = recvBuffer.find(':', lpathPos + 7);
        if (colonPos != std::string::npos && colonPos + 1 < recvBuffer.size()) {
          size_t quoteStart = recvBuffer.find('"', colonPos + 1);
          if (quoteStart != std::string::npos && quoteStart + 1 < recvBuffer.size()) {
            size_t quoteEnd = recvBuffer.find('"', quoteStart + 1);
            if (quoteEnd != std::string::npos && quoteEnd > quoteStart + 1) {
              skipExtractedLpath = recvBuffer.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
            }
          }
        }
      }

      // Extract top-level length (track depth to skip nested length fields in cover metadata)
      // Message format is [opcode, {data}], so depth 2 = top level of data object
      int depth = 0;
      const char* lengthKey = "\"length\"";
      const size_t keyLen = 8;
      for (size_t i = bracketPos; i < recvBuffer.size() && i < bracketPos + 2000; i++) {
        char c = recvBuffer[i];
        if (c == '{' || c == '[') {
          depth++;
        } else if (c == '}' || c == ']') {
          depth--;
        } else if (depth == 2 && c == '"' && i + keyLen <= recvBuffer.size()) {
          bool match = true;
          for (size_t j = 0; j < keyLen && match; j++) {
            if (recvBuffer[i + j] != lengthKey[j]) match = false;
          }
          if (match) {
            size_t numStart = i + keyLen;
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

    // Calculate how many bytes we still need to skip
    size_t totalMsgBytes = bracketPos + msgLen;
    size_t alreadyBuffered = recvBuffer.size();
    if (alreadyBuffered >= totalMsgBytes) {
      // Entire message is already buffered - just discard it
      recvBuffer = recvBuffer.substr(totalMsgBytes);
      skipBytesRemaining = 0;
    } else {
      // Need to skip remaining bytes from network
      skipBytesRemaining = totalMsgBytes - alreadyBuffered;
      recvBuffer.clear();
    }

    inSkipMode = true;

    // If skip is already complete, return immediately
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
      return false;
    }
    return false;  // Continue skipping in next iteration
  }

  // Normal path for small messages
  size_t totalNeeded = bracketPos + msgLen;
  if (recvBuffer.size() < totalNeeded) {
    return false;  // Wait for more data
  }

  // Extract the message
  if (bracketPos < recvBuffer.size() && bracketPos + msgLen <= recvBuffer.size()) {
    message = recvBuffer.substr(bracketPos, msgLen);
  } else {
    recvBuffer.clear();
    return false;
  }

  // Keep remainder in buffer
  if (recvBuffer.size() > totalNeeded) {
    recvBuffer = recvBuffer.substr(totalNeeded);
  } else {
    recvBuffer.clear();
  }

  return true;
}

void CalibreWirelessActivity::sendJsonResponse(const OpCode opcode, const std::string& data) {
  // Format: length + [opcode, {data}]
  std::string json = "[" + std::to_string(opcode) + "," + data + "]";
  const std::string lengthPrefix = std::to_string(json.length());
  json.insert(0, lengthPrefix);

  tcpClient.write(reinterpret_cast<const uint8_t*>(json.c_str()), json.length());
  tcpClient.flush();
}

void CalibreWirelessActivity::handleCommand(const OpCode opcode, const std::string& data) {
  Serial.printf("[%lu] [CAL] Received opcode: %d, data size: %zu\n", millis(), opcode, data.size());

  switch (opcode) {
    case OpCode::GET_INITIALIZATION_INFO:
      handleGetInitializationInfo(data);
      break;
    case OpCode::GET_DEVICE_INFORMATION:
      handleGetDeviceInformation();
      break;
    case OpCode::FREE_SPACE:
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
      // These set metadata about the connected Calibre instance.
      // We don't need this info, just acknowledge receipt.
      sendJsonResponse(OpCode::OK, "{}");
      break;
    case OpCode::SET_LIBRARY_INFO:
      // Library metadata (name, UUID) - not needed for receiving books
      sendJsonResponse(OpCode::OK, "{}");
      break;
    case OpCode::SEND_BOOKLISTS:
      // Calibre asking us to send our book list. We report 0 books in
      // handleGetBookCount, so this is effectively a no-op.
      sendJsonResponse(OpCode::OK, "{}");
      break;
    case OpCode::TOTAL_SPACE:
      handleFreeSpace();
      break;
    default:
      Serial.printf("[%lu] [CAL] Unknown opcode: %d\n", millis(), opcode);
      sendJsonResponse(OpCode::OK, "{}");
      break;
  }
}

void CalibreWirelessActivity::handleGetInitializationInfo(const std::string& data) {
  setState(WirelessState::WAITING);
  setStatus("Connected to " + calibreHostname +
            "\nWaiting for transfer...\n\nIf transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice "
            "plugin settings.");

  // Build response with device capabilities
  // Format must match what Calibre expects from a smart device
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
  // ccVersionNumber: Calibre Companion protocol version. 212 matches CC 5.4.20+.
  // Using a known version ensures compatibility with Calibre's feature detection.
  response += "\"ccVersionNumber\":212,";
  // coverHeight: Max cover image height. Set to 0 to prevent Calibre from embedding
  // large base64-encoded covers in SEND_BOOK metadata, which would bloat the JSON.
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
  // TODO: Report actual SD card free space instead of hardcoded value
  // Report 10GB free space for now
  sendJsonResponse(OpCode::OK, "{\"free_space_on_device\":10737418240}");
}

void CalibreWirelessActivity::handleGetBookCount() {
  // We report 0 books - Calibre will send books without checking for duplicates
  std::string response = "{\"count\":0,\"willStream\":true,\"willScan\":false}";
  sendJsonResponse(OpCode::OK, response);
}

void CalibreWirelessActivity::handleSendBook(const std::string& data) {
  // Manually extract lpath and length from SEND_BOOK data
  // Full JSON parsing crashes on large metadata, so we just extract what we need

  // Extract "lpath" field - format: "lpath": "value"
  std::string lpath;
  size_t lpathPos = data.find("\"lpath\"");
  if (lpathPos != std::string::npos && lpathPos + 7 < data.size()) {
    size_t colonPos = data.find(':', lpathPos + 7);
    if (colonPos != std::string::npos && colonPos + 1 < data.size()) {
      size_t quoteStart = data.find('"', colonPos + 1);
      if (quoteStart != std::string::npos && quoteStart + 1 < data.size()) {
        size_t quoteEnd = data.find('"', quoteStart + 1);
        if (quoteEnd != std::string::npos && quoteEnd > quoteStart + 1) {
          // Safe bounds check before substr
          size_t start = quoteStart + 1;
          size_t len = quoteEnd - quoteStart - 1;
          if (start < data.size() && start + len <= data.size()) {
            lpath = data.substr(start, len);
          }
        }
      }
    }
  }

  // Extract top-level "length" field - must track depth to skip nested objects
  // The metadata contains nested "length" fields (e.g., cover image length)
  size_t length = 0;
  int depth = 0;
  const char* lengthKey = "\"length\"";
  const size_t keyLen = 8;

  for (size_t i = 0; i < data.size(); i++) {
    char c = data[i];
    if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    } else if (depth == 1 && c == '"') {
      // At top level, check if this is "length" by comparing directly
      if (i + keyLen <= data.size()) {
        bool match = true;
        for (size_t j = 0; j < keyLen && match; j++) {
          if (data[i + j] != lengthKey[j]) {
            match = false;
          }
        }
        if (match) {
          // Found top-level "length" - extract the number after ':'
          size_t colonPos = i + keyLen;
          while (colonPos < data.size() && data[colonPos] != ':') {
            colonPos++;
          }
          if (colonPos < data.size()) {
            size_t numStart = colonPos + 1;
            while (numStart < data.size() && (data[numStart] == ' ' || data[numStart] == '\t')) {
              numStart++;
            }
            // Parse number safely without exceptions
            size_t parsedLen = 0;
            while (numStart < data.size() && data[numStart] >= '0' && data[numStart] <= '9') {
              parsedLen = parsedLen * 10 + (data[numStart] - '0');
              numStart++;
            }
            if (parsedLen > 0) {
              length = parsedLen;
              break;
            }
          }
        }
      }
    }
  }

  if (lpath.empty() || length == 0) {
    sendJsonResponse(OpCode::ERROR, "{\"message\":\"Invalid book data\"}");
    return;
  }

  // Extract filename from lpath
  std::string filename = lpath;
  const size_t lastSlash = filename.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  // Sanitize and create full path
  currentFilename = "/" + StringUtils::sanitizeFilename(filename);
  if (!StringUtils::checkFileExtension(currentFilename, ".epub")) {
    currentFilename += ".epub";
  }
  currentFileSize = length;
  bytesReceived = 0;

  Serial.printf("[%lu] [CAL] SEND_BOOK: lpath='%s', length=%zu\n", millis(), lpath.c_str(), length);

  setState(WirelessState::RECEIVING);
  setStatus("Receiving: " + filename);

  // Open file for writing
  if (!SdMan.openFileForWrite("CAL", currentFilename.c_str(), currentFile)) {
    setError("Failed to create file");
    sendJsonResponse(OpCode::ERROR, "{\"message\":\"Failed to create file\"}");
    return;
  }

  // Send OK to start receiving binary data
  sendJsonResponse(OpCode::OK, "{}");

  // Switch to binary mode
  inBinaryMode = true;
  binaryBytesRemaining = length;

  // Check if recvBuffer has leftover data (binary file data that arrived with the JSON)
  if (!recvBuffer.empty()) {
    size_t toWrite = std::min(recvBuffer.size(), binaryBytesRemaining);
    size_t written = currentFile.write(reinterpret_cast<const uint8_t*>(recvBuffer.data()), toWrite);
    bytesReceived += written;
    binaryBytesRemaining -= written;
    recvBuffer = recvBuffer.substr(toWrite);
    updateRequired = true;
  }
}

void CalibreWirelessActivity::handleSendBookMetadata(const std::string& data) {
  // We receive metadata after the book - just acknowledge
  sendJsonResponse(OpCode::OK, "{}");
}

void CalibreWirelessActivity::handleDisplayMessage(const std::string& data) {
  // Calibre may send messages to display
  // Check messageKind - 1 means password error
  if (data.find("\"messageKind\":1") != std::string::npos) {
    setError("Password required");
  }
  sendJsonResponse(OpCode::OK, "{}");
}

void CalibreWirelessActivity::handleNoop(const std::string& data) {
  // Check for ejecting flag
  if (data.find("\"ejecting\":true") != std::string::npos) {
    setState(WirelessState::DISCONNECTED);
    setStatus("Calibre disconnected");
  }
  sendJsonResponse(OpCode::NOOP, "{}");
}

void CalibreWirelessActivity::receiveBinaryData() {
  // Read all available data in a loop to prevent TCP backpressure
  // This is important because Calibre sends data continuously
  while (binaryBytesRemaining > 0) {
    const int available = tcpClient.available();
    if (available == 0) {
      // Check if connection is still alive
      if (!tcpClient.connected()) {
        Serial.printf("[%lu] [CAL] Connection lost during binary transfer. Received %zu/%zu bytes\n",
                      millis(), bytesReceived, currentFileSize);
        currentFile.close();
        inBinaryMode = false;
        setError("Transfer interrupted");
      }
      return;  // No data available right now, will continue next iteration
    }

    uint8_t buffer[4096];  // Larger buffer for faster transfer
    const size_t toRead = std::min({sizeof(buffer), binaryBytesRemaining, static_cast<size_t>(available)});
    const size_t bytesRead = tcpClient.read(buffer, toRead);

    if (bytesRead == 0) {
      break;  // No more data to read right now
    }

    currentFile.write(buffer, bytesRead);
    bytesReceived += bytesRead;
    binaryBytesRemaining -= bytesRead;
    updateRequired = true;
  }

  if (binaryBytesRemaining == 0) {
    // Transfer complete - switch back to JSON mode
    // Note: Do NOT send OK here. KOReader doesn't, and sending an extra OK
    // could be misinterpreted as a response to SEND_BOOK_METADATA before
    // we've received it, causing protocol desync.
    currentFile.flush();
    currentFile.close();
    inBinaryMode = false;

    Serial.printf("[%lu] [CAL] Binary transfer complete: %zu bytes received\n", millis(), bytesReceived);

    setState(WirelessState::WAITING);
    setStatus("Received: " + currentFilename + "\nWaiting for more...");
  }
}

void CalibreWirelessActivity::render() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  // Draw header
  renderer.drawCenteredText(UI_12_FONT_ID, 30, "Calibre Wireless", true, EpdFontFamily::BOLD);

  // Draw IP address
  const std::string ipAddr = WiFi.localIP().toString().c_str();
  renderer.drawCenteredText(UI_10_FONT_ID, 60, ("IP: " + ipAddr).c_str());

  // Draw status message
  int statusY = pageHeight / 2 - 40;

  // Split status message by newlines and draw each line
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

  // Draw progress if receiving
  if (state == WirelessState::RECEIVING && currentFileSize > 0) {
    const int barWidth = pageWidth - 100;
    constexpr int barHeight = 20;
    constexpr int barX = 50;
    const int barY = statusY + 20;
    ScreenComponents::drawProgressBar(renderer, barX, barY, barWidth, barHeight, bytesReceived, currentFileSize);
  }

  // Draw error if present
  if (!errorMessage.empty()) {
    renderer.drawCenteredText(UI_10_FONT_ID, pageHeight - 120, errorMessage.c_str());
  }

  // Draw button hints
  const auto labels = mappedInput.mapLabels("Back", "", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}

std::string CalibreWirelessActivity::getDeviceUuid() const {
  // Generate a consistent UUID based on MAC address
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
