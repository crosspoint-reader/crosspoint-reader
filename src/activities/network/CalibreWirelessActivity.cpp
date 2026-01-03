#include "CalibreWirelessActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <WiFi.h>

#include <cstring>

#include "CrossPointSettings.h"
#include "MappedInputManager.h"
#include "fontIds.h"

// Define static constexpr members
constexpr uint16_t CalibreWirelessActivity::UDP_PORTS[];

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

  state = CalibreWirelessState::DISCOVERING;
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

  updateRequired = true;

  // Start UDP listener for Calibre responses
  udp.begin(LOCAL_UDP_PORT);
  Serial.printf("[%lu] [CAL] UDP listener started on port %d\n", millis(), LOCAL_UDP_PORT);

  // Create display task
  xTaskCreate(&CalibreWirelessActivity::displayTaskTrampoline, "CalDisplayTask", 2048, this, 1, &displayTaskHandle);

  // Create network task with larger stack for JSON parsing
  xTaskCreate(&CalibreWirelessActivity::networkTaskTrampoline, "CalNetworkTask", 12288, this, 2, &networkTaskHandle);
}

void CalibreWirelessActivity::onExit() {
  Activity::onExit();

  Serial.printf("[%lu] [CAL] Exiting CalibreWirelessActivity\n", millis());

  // Always turn off the setting when exiting so it shows OFF in settings
  SETTINGS.calibreWirelessEnabled = 0;
  SETTINGS.saveToFile();

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

  // Delete network task first (it may be blocked on network operations)
  if (networkTaskHandle) {
    vTaskDelete(networkTaskHandle);
    networkTaskHandle = nullptr;
  }

  // Acquire mutex before deleting display task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  vSemaphoreDelete(stateMutex);
  stateMutex = nullptr;

  Serial.printf("[%lu] [CAL] Cleanup complete\n", millis());
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
      case CalibreWirelessState::DISCOVERING:
        listenForDiscovery();
        break;

      case CalibreWirelessState::CONNECTING:
      case CalibreWirelessState::WAITING:
      case CalibreWirelessState::RECEIVING:
        handleTcpClient();
        break;

      case CalibreWirelessState::COMPLETE:
      case CalibreWirelessState::DISCONNECTED:
      case CalibreWirelessState::ERROR:
        // Just wait, user will exit
        vTaskDelay(100 / portTICK_PERIOD_MS);
        break;
    }

    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void CalibreWirelessActivity::listenForDiscovery() {
  // Broadcast "hello" on all UDP discovery ports to find Calibre
  for (size_t i = 0; i < UDP_PORT_COUNT; i++) {
    udp.beginPacket("255.255.255.255", UDP_PORTS[i]);
    udp.write(reinterpret_cast<const uint8_t*>("hello"), 5);
    udp.endPacket();
  }
  Serial.printf("[%lu] [CAL] Broadcast 'hello' on discovery ports\n", millis());

  // Wait for Calibre's response
  vTaskDelay(500 / portTICK_PERIOD_MS);

  // Check for response
  const int packetSize = udp.parsePacket();
  if (packetSize > 0) {
    char buffer[256];
    const int len = udp.read(buffer, sizeof(buffer) - 1);
    if (len > 0) {
      buffer[len] = '\0';
      Serial.printf("[%lu] [CAL] UDP response received: %s\n", millis(), buffer);

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
          // Get alternative port after comma
          std::string altPortStr = response.substr(commaPos + 1);
          // Trim whitespace and non-digits from alt port
          size_t altEnd = 0;
          while (altEnd < altPortStr.size() && altPortStr[altEnd] >= '0' && altPortStr[altEnd] <= '9') {
            altEnd++;
          }
          if (altEnd > 0) {
            calibreAltPort = static_cast<uint16_t>(std::stoi(altPortStr.substr(0, altEnd)));
          }
        } else {
          portStr = response.substr(semiPos + 1);
        }

        // Trim whitespace from main port
        while (!portStr.empty() && (portStr[0] == ' ' || portStr[0] == '\t')) {
          portStr = portStr.substr(1);
        }

        if (!portStr.empty()) {
          calibrePort = static_cast<uint16_t>(std::stoi(portStr));
        }

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

      Serial.printf("[%lu] [CAL] Parsed: host=%s, port=%d, altPort=%d, name=%s\n", millis(), calibreHost.c_str(),
                    calibrePort, calibreAltPort, calibreHostname.c_str());

      if (calibrePort > 0) {
        // Connect to Calibre's TCP server - try main port first, then alt port
        setState(CalibreWirelessState::CONNECTING);
        setStatus("Connecting to " + calibreHostname + "...");

        // Small delay before connecting
        vTaskDelay(100 / portTICK_PERIOD_MS);

        bool connected = false;

        // Try main port first
        Serial.printf("[%lu] [CAL] Trying main port %s:%d\n", millis(), calibreHost.c_str(), calibrePort);
        if (tcpClient.connect(calibreHost.c_str(), calibrePort, 5000)) {
          connected = true;
          Serial.printf("[%lu] [CAL] TCP connected to %s:%d\n", millis(), calibreHost.c_str(), calibrePort);
        } else {
          Serial.printf("[%lu] [CAL] Main port %d failed\n", millis(), calibrePort);
        }

        // Try alternative port if main failed
        if (!connected && calibreAltPort > 0) {
          vTaskDelay(200 / portTICK_PERIOD_MS);
          Serial.printf("[%lu] [CAL] Trying alt port %s:%d\n", millis(), calibreHost.c_str(), calibreAltPort);
          if (tcpClient.connect(calibreHost.c_str(), calibreAltPort, 5000)) {
            connected = true;
            Serial.printf("[%lu] [CAL] TCP connected to %s:%d (alt)\n", millis(), calibreHost.c_str(), calibreAltPort);
          } else {
            Serial.printf("[%lu] [CAL] Alt port %d also failed\n", millis(), calibreAltPort);
          }
        }

        if (connected) {
          setState(CalibreWirelessState::WAITING);
          setStatus("Connected to " + calibreHostname + "\nWaiting for commands...");
        } else {
          Serial.printf("[%lu] [CAL] All TCP connection attempts failed\n", millis());
          // Don't set error yet, keep trying discovery
          setState(CalibreWirelessState::DISCOVERING);
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
    Serial.printf("[%lu] [CAL] TCP client disconnected\n", millis());
    setState(CalibreWirelessState::DISCONNECTED);
    setStatus("Calibre disconnected");
    return;
  }

  // Check if we're receiving binary data
  if (inBinaryMode) {
    receiveBinaryData();
    return;
  }

  // Read JSON message
  std::string message;
  if (readJsonMessage(message)) {
    // Parse opcode from JSON array format: [opcode, {...}]
    // Find the opcode (first number after '[')
    size_t start = message.find('[');
    if (start != std::string::npos) {
      start++;
      size_t end = message.find(',', start);
      if (end != std::string::npos) {
        const int opcode = std::stoi(message.substr(start, end - start));

        // Extract data object (everything after the comma until the last ']')
        size_t dataStart = end + 1;
        size_t dataEnd = message.rfind(']');
        std::string data = "";
        if (dataEnd != std::string::npos && dataEnd > dataStart) {
          data = message.substr(dataStart, dataEnd - dataStart);
        }

        Serial.printf("[%lu] [CAL] Received opcode %d\n", millis(), opcode);
        handleCommand(opcode, data);
      }
    }
  }
}

bool CalibreWirelessActivity::readJsonMessage(std::string& message) {
  if (!tcpClient.available()) {
    return false;
  }

  // Protocol: 4-byte length prefix (as string) followed by JSON
  // Actually, Calibre uses variable-length ASCII number followed by JSON array
  // Read until we get a '[' character

  // Read length prefix (digits until we hit '[')
  std::string lengthStr;
  while (tcpClient.available()) {
    const char c = tcpClient.read();
    if (c == '[') {
      // Start of JSON
      message = "[";
      break;
    } else if (c >= '0' && c <= '9') {
      lengthStr += c;
    } else {
      // Unexpected character, skip
    }
  }

  if (message.empty()) {
    return false;
  }

  // Parse expected length
  const size_t expectedLen = lengthStr.empty() ? 0 : std::stoul(lengthStr);

  // Read rest of the JSON message
  // We already read '[', so we need expectedLen - 1 more chars (if length was specified)
  // But Calibre's length includes the '[', so read expectedLen - 1 more
  size_t bytesToRead = expectedLen > 0 ? expectedLen - 1 : 4096;
  size_t bytesRead = 0;

  const unsigned long timeout = millis() + 5000;
  while (bytesRead < bytesToRead && millis() < timeout) {
    if (tcpClient.available()) {
      const char c = tcpClient.read();
      message += c;
      bytesRead++;

      // If no length specified, check for end of JSON
      if (expectedLen == 0 && c == ']') {
        // Check if this is the matching closing bracket
        int depth = 0;
        for (char ch : message) {
          if (ch == '[' || ch == '{')
            depth++;
          else if (ch == ']' || ch == '}')
            depth--;
        }
        if (depth == 0) {
          break;
        }
      }
    } else {
      vTaskDelay(1);
    }
  }

  Serial.printf("[%lu] [CAL] Read JSON (%zu bytes): %.100s...\n", millis(), message.length(), message.c_str());
  return !message.empty();
}

void CalibreWirelessActivity::sendJsonResponse(int opcode, const std::string& data) {
  // Format: length + [opcode, {data}]
  std::string json = "[" + std::to_string(opcode) + "," + data + "]";
  std::string packet = std::to_string(json.length()) + json;

  Serial.printf("[%lu] [CAL] Sending packet (%zu bytes): %s\n", millis(), packet.length(), packet.c_str());

  const size_t written = tcpClient.write(reinterpret_cast<const uint8_t*>(packet.c_str()), packet.length());
  tcpClient.flush();

  Serial.printf("[%lu] [CAL] Wrote %zu bytes, client connected: %d\n", millis(), written, tcpClient.connected());
}

void CalibreWirelessActivity::handleCommand(int opcode, const std::string& data) {
  Serial.printf("[%lu] [CAL] handleCommand: opcode=%d, data_len=%zu\n", millis(), opcode, data.length());

  switch (opcode) {
    case OP_GET_INITIALIZATION_INFO:
      handleGetInitializationInfo(data);
      break;
    case OP_GET_DEVICE_INFORMATION:
      handleGetDeviceInformation();
      break;
    case OP_FREE_SPACE:
      handleFreeSpace();
      break;
    case OP_GET_BOOK_COUNT:
      handleGetBookCount();
      break;
    case OP_SEND_BOOK:
      handleSendBook(data);
      break;
    case OP_SEND_BOOK_METADATA:
      handleSendBookMetadata(data);
      break;
    case OP_DISPLAY_MESSAGE:
      handleDisplayMessage(data);
      break;
    case OP_NOOP:
      handleNoop(data);
      break;
    case OP_SET_CALIBRE_DEVICE_INFO:
    case OP_SET_CALIBRE_DEVICE_NAME:
      // Just acknowledge
      sendJsonResponse(OP_OK, "{}");
      break;
    case OP_SET_LIBRARY_INFO:
      // Calibre sends library info - acknowledge
      Serial.printf("[%lu] [CAL] SET_LIBRARY_INFO received\n", millis());
      sendJsonResponse(OP_OK, "{}");
      break;
    case OP_SEND_BOOKLISTS:
      // Calibre sends book lists for sync - acknowledge
      Serial.printf("[%lu] [CAL] SEND_BOOKLISTS received\n", millis());
      sendJsonResponse(OP_OK, "{}");
      break;
    case OP_TOTAL_SPACE:
      // Same as FREE_SPACE
      handleFreeSpace();
      break;
    default:
      Serial.printf("[%lu] [CAL] Unknown opcode: %d\n", millis(), opcode);
      sendJsonResponse(OP_OK, "{}");
      break;
  }
}

void CalibreWirelessActivity::handleGetInitializationInfo(const std::string& data) {
  // Log the full received data for debugging
  Serial.printf("[%lu] [CAL] GET_INITIALIZATION_INFO data: %s\n", millis(), data.c_str());

  setState(CalibreWirelessState::WAITING);
  setStatus("Connected to " + calibreHostname + "\nWaiting for transfer...\n\nIf transfer fails, enable\n'Ignore free space' in Calibre's\nSmartDevice plugin settings.");

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
  response += "\"ccVersionNumber\":212,";  // Match a known CC version
  response += "\"coverHeight\":240,";
  response += "\"deviceKind\":\"CrossPoint\",";
  response += "\"deviceName\":\"CrossPoint\",";
  response += "\"extensionPathLengths\":{\"epub\":37},";
  response += "\"maxBookContentPacketLen\":4096,";
  response += "\"passwordHash\":\"\",";
  response += "\"useUuidFileNames\":false,";
  response += "\"versionOK\":true";
  response += "}";

  Serial.printf("[%lu] [CAL] Sending init response: %s\n", millis(), response.c_str());
  sendJsonResponse(OP_OK, response);
}

void CalibreWirelessActivity::handleGetDeviceInformation() {
  std::string response = "{";
  response += "\"device_info\":{";
  response += "\"device_store_uuid\":\"" + getDeviceUuid() + "\",";
  response += "\"device_name\":\"CrossPoint Reader\",";
  response += "\"device_version\":\"1.0\"";
  response += "},";
  response += "\"version\":1,";
  response += "\"device_version\":\"1.0\"";
  response += "}";

  sendJsonResponse(OP_OK, response);
}

void CalibreWirelessActivity::handleFreeSpace() {
  Serial.printf("[%lu] [CAL] handleFreeSpace called\n", millis());

  // Report 10GB free space - hardcoded to avoid any number formatting issues
  // Using a string literal to ensure the JSON is exactly what we expect
  std::string response = "{\"free_space_on_device\":10737418240}";  // 10GB

  Serial.printf("[%lu] [CAL] FREE_SPACE response: %s\n", millis(), response.c_str());
  sendJsonResponse(OP_OK, response);
}

void CalibreWirelessActivity::handleGetBookCount() {
  // We report 0 books - Calibre will send books without checking for duplicates
  std::string response = "{\"count\":0,\"willStream\":true,\"willScan\":false}";
  sendJsonResponse(OP_OK, response);
}

void CalibreWirelessActivity::handleSendBook(const std::string& data) {
  // Parse the SEND_BOOK data to get lpath and length
  // Format: {"lpath": "path/to/book.epub", "length": 12345, ...}

  // Simple JSON parsing for lpath and length
  std::string lpath;
  size_t length = 0;

  // Find lpath
  size_t lpathPos = data.find("\"lpath\"");
  if (lpathPos != std::string::npos) {
    size_t colonPos = data.find(':', lpathPos);
    size_t quoteStart = data.find('"', colonPos);
    size_t quoteEnd = data.find('"', quoteStart + 1);
    if (quoteStart != std::string::npos && quoteEnd != std::string::npos) {
      lpath = data.substr(quoteStart + 1, quoteEnd - quoteStart - 1);
    }
  }

  // Find length
  size_t lengthPos = data.find("\"length\"");
  if (lengthPos != std::string::npos) {
    size_t colonPos = data.find(':', lengthPos);
    size_t numStart = colonPos + 1;
    while (numStart < data.length() && (data[numStart] == ' ' || data[numStart] == '\t')) {
      numStart++;
    }
    size_t numEnd = numStart;
    while (numEnd < data.length() && data[numEnd] >= '0' && data[numEnd] <= '9') {
      numEnd++;
    }
    if (numEnd > numStart) {
      length = std::stoul(data.substr(numStart, numEnd - numStart));
    }
  }

  if (lpath.empty() || length == 0) {
    Serial.printf("[%lu] [CAL] Invalid SEND_BOOK data\n", millis());
    sendJsonResponse(OP_ERROR, "{\"message\":\"Invalid book data\"}");
    return;
  }

  Serial.printf("[%lu] [CAL] SEND_BOOK: %s (%zu bytes)\n", millis(), lpath.c_str(), length);

  // Extract filename from lpath
  std::string filename = lpath;
  const size_t lastSlash = filename.rfind('/');
  if (lastSlash != std::string::npos) {
    filename = filename.substr(lastSlash + 1);
  }

  // Sanitize and create full path
  currentFilename = "/" + sanitizeFilename(filename);
  if (currentFilename.find(".epub") == std::string::npos) {
    currentFilename += ".epub";
  }
  currentFileSize = length;
  bytesReceived = 0;

  setState(CalibreWirelessState::RECEIVING);
  setStatus("Receiving: " + filename);

  // Open file for writing
  if (!SdMan.openFileForWrite("CAL", currentFilename.c_str(), currentFile)) {
    Serial.printf("[%lu] [CAL] Failed to open file for writing: %s\n", millis(), currentFilename.c_str());
    setError("Failed to create file");
    sendJsonResponse(OP_ERROR, "{\"message\":\"Failed to create file\"}");
    return;
  }

  // Send OK to start receiving binary data
  sendJsonResponse(OP_OK, "{}");

  // Switch to binary mode
  inBinaryMode = true;
  binaryBytesRemaining = length;
}

void CalibreWirelessActivity::handleSendBookMetadata(const std::string& data) {
  // We receive metadata after the book - just acknowledge
  sendJsonResponse(OP_OK, "{}");
}

void CalibreWirelessActivity::handleDisplayMessage(const std::string& data) {
  // Calibre may send messages to display
  // Check messageKind - 1 means password error
  if (data.find("\"messageKind\":1") != std::string::npos) {
    setError("Password required");
  }
  sendJsonResponse(OP_OK, "{}");
}

void CalibreWirelessActivity::handleNoop(const std::string& data) {
  // Check for ejecting flag
  if (data.find("\"ejecting\":true") != std::string::npos) {
    Serial.printf("[%lu] [CAL] Calibre is ejecting\n", millis());
    setState(CalibreWirelessState::DISCONNECTED);
    setStatus("Calibre disconnected");
  }
  sendJsonResponse(OP_NOOP, "{}");
}

void CalibreWirelessActivity::receiveBinaryData() {
  const int available = tcpClient.available();
  if (available == 0) {
    // Check if connection is still alive
    if (!tcpClient.connected()) {
      Serial.printf("[%lu] [CAL] TCP disconnected during transfer! Received %zu/%zu bytes\n",
                    millis(), bytesReceived, currentFileSize);
      currentFile.close();
      inBinaryMode = false;
      setError("Transfer interrupted");
    }
    return;
  }

  uint8_t buffer[1024];  // Smaller buffer to avoid stack overflow
  const size_t toRead = std::min(sizeof(buffer), binaryBytesRemaining);
  const size_t bytesRead = tcpClient.read(buffer, toRead);

  if (bytesRead > 0) {
    const size_t written = currentFile.write(buffer, bytesRead);
    if (written != bytesRead) {
      Serial.printf("[%lu] [CAL] Write error! Tried %zu, wrote %zu\n", millis(), bytesRead, written);
    }
    bytesReceived += bytesRead;
    binaryBytesRemaining -= bytesRead;
    updateRequired = true;

    // Log progress every ~10%
    if (currentFileSize > 0) {
      const int percent = static_cast<int>((bytesReceived * 100) / currentFileSize);
      const int prevPercent = static_cast<int>(((bytesReceived - bytesRead) * 100) / currentFileSize);
      if (percent / 10 != prevPercent / 10 || percent == 100) {
        Serial.printf("[%lu] [CAL] Transfer progress: %zu/%zu bytes (%d%%), remaining=%zu\n",
                      millis(), bytesReceived, currentFileSize, percent, binaryBytesRemaining);
      }
    }

    if (binaryBytesRemaining == 0) {
      // Transfer complete
      currentFile.flush();
      currentFile.close();
      inBinaryMode = false;

      Serial.printf("[%lu] [CAL] Book transfer complete: %s (%zu bytes)\n", millis(), currentFilename.c_str(),
                    bytesReceived);

      setState(CalibreWirelessState::WAITING);
      setStatus("Received: " + currentFilename + "\nWaiting for more...");

      // Send OK to acknowledge completion
      sendJsonResponse(OP_OK, "{}");
    }
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
  if (state == CalibreWirelessState::RECEIVING && currentFileSize > 0) {
    const int percent = static_cast<int>((bytesReceived * 100) / currentFileSize);

    // Progress bar
    const int barWidth = pageWidth - 100;
    const int barHeight = 20;
    const int barX = 50;
    const int barY = statusY + 20;

    renderer.drawRect(barX, barY, barWidth, barHeight);
    renderer.fillRect(barX + 2, barY + 2, (barWidth - 4) * percent / 100, barHeight - 4);

    // Percentage text
    const std::string percentText = std::to_string(percent) + "%";
    renderer.drawCenteredText(UI_10_FONT_ID, barY + barHeight + 15, percentText.c_str());
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

std::string CalibreWirelessActivity::sanitizeFilename(const std::string& name) const {
  std::string result;
  result.reserve(name.size());

  for (char c : name) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
      result += '_';
    } else if (c >= 32 && c < 127) {
      result += c;
    }
  }

  // Trim leading/trailing spaces and dots
  size_t start = 0;
  while (start < result.size() && (result[start] == ' ' || result[start] == '.')) {
    start++;
  }
  size_t end = result.size();
  while (end > start && (result[end - 1] == ' ' || result[end - 1] == '.')) {
    end--;
  }

  return result.substr(start, end - start);
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

void CalibreWirelessActivity::setState(CalibreWirelessState newState) {
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
  setState(CalibreWirelessState::ERROR);
}
