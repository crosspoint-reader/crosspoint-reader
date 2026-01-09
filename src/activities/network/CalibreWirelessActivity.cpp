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

  // Acquire stateMutex before deleting network task to avoid race condition
  xSemaphoreTake(stateMutex, portMAX_DELAY);
  if (networkTaskHandle) {
    vTaskDelete(networkTaskHandle);
    networkTaskHandle = nullptr;
  }
  xSemaphoreGive(stateMutex);

  // Acquire renderingMutex before deleting display task
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;

  vSemaphoreDelete(stateMutex);
  stateMutex = nullptr;
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
        const int opcodeInt = std::stoi(message.substr(start, end - start));
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
        if (dataEnd != std::string::npos && dataEnd > dataStart) {
          data = message.substr(dataStart, dataEnd - dataStart);
        }

        handleCommand(opcode, data);
      }
    }
  }
}

bool CalibreWirelessActivity::readJsonMessage(std::string& message) {
  // Read available data into buffer
  int available = tcpClient.available();
  if (available > 0) {
    // Limit buffer growth to prevent memory issues
    if (recvBuffer.size() > 100000) {
      recvBuffer.clear();
      return false;
    }
    // Read in chunks
    char buf[1024];
    while (available > 0) {
      int toRead = std::min(available, static_cast<int>(sizeof(buf)));
      int bytesRead = tcpClient.read(reinterpret_cast<uint8_t*>(buf), toRead);
      if (bytesRead > 0) {
        recvBuffer.append(buf, bytesRead);
        available -= bytesRead;
      } else {
        break;
      }
    }
  }

  if (recvBuffer.empty()) {
    return false;
  }

  // Find '[' which marks the start of JSON
  size_t bracketPos = recvBuffer.find('[');
  if (bracketPos == std::string::npos) {
    // No '[' found - if buffer is getting large, something is wrong
    if (recvBuffer.size() > 1000) {
      recvBuffer.clear();
    }
    return false;
  }

  // Try to extract length from digits before '['
  // Calibre ALWAYS sends a length prefix, so if it's not valid digits, it's garbage
  size_t msgLen = 0;
  bool validPrefix = false;

  if (bracketPos > 0 && bracketPos <= 12) {
    // Check if prefix is all digits
    bool allDigits = true;
    for (size_t i = 0; i < bracketPos; i++) {
      char c = recvBuffer[i];
      if (c < '0' || c > '9') {
        allDigits = false;
        break;
      }
    }
    if (allDigits) {
      msgLen = std::stoul(recvBuffer.substr(0, bracketPos));
      validPrefix = true;
    }
  }

  if (!validPrefix) {
    // Not a valid length prefix - discard everything up to '[' and treat '[' as start
    if (bracketPos > 0) {
      recvBuffer = recvBuffer.substr(bracketPos);
    }
    // Without length prefix, we can't reliably parse - wait for more data
    // that hopefully starts with a proper length prefix
    return false;
  }

  // Sanity check the message length
  if (msgLen > 1000000) {
    recvBuffer = recvBuffer.substr(bracketPos + 1);  // Skip past this '[' and try again
    return false;
  }

  // Check if we have the complete message
  size_t totalNeeded = bracketPos + msgLen;
  if (recvBuffer.size() < totalNeeded) {
    // Not enough data yet - wait for more
    return false;
  }

  // Extract the message
  message = recvBuffer.substr(bracketPos, msgLen);

  // Keep the rest in buffer (may contain binary data or next message)
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
  // coverHeight: Max cover image height. We don't process covers, so this is informational only.
  response += "\"coverHeight\":800,";
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

  // Extract top-level "length" field - must track depth to skip nested objects
  // The metadata contains nested "length" fields (e.g., cover image length)
  size_t length = 0;
  int depth = 0;
  for (size_t i = 0; i < data.size(); i++) {
    char c = data[i];
    if (c == '{' || c == '[') {
      depth++;
    } else if (c == '}' || c == ']') {
      depth--;
    } else if (depth == 1 && c == '"') {
      // At top level, check if this is "length"
      if (i + 9 < data.size() && data.substr(i, 8) == "\"length\"") {
        // Found top-level "length" - extract the number after ':'
        size_t colonPos = data.find(':', i + 8);
        if (colonPos != std::string::npos) {
          size_t numStart = colonPos + 1;
          while (numStart < data.size() && (data[numStart] == ' ' || data[numStart] == '\t')) {
            numStart++;
          }
          size_t numEnd = numStart;
          while (numEnd < data.size() && data[numEnd] >= '0' && data[numEnd] <= '9') {
            numEnd++;
          }
          if (numEnd > numStart) {
            length = std::stoul(data.substr(numStart, numEnd - numStart));
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
  const int available = tcpClient.available();
  if (available == 0) {
    // Check if connection is still alive
    if (!tcpClient.connected()) {
      currentFile.close();
      inBinaryMode = false;
      setError("Transfer interrupted");
    }
    return;
  }

  uint8_t buffer[1024];
  const size_t toRead = std::min(sizeof(buffer), binaryBytesRemaining);
  const size_t bytesRead = tcpClient.read(buffer, toRead);

  if (bytesRead > 0) {
    currentFile.write(buffer, bytesRead);
    bytesReceived += bytesRead;
    binaryBytesRemaining -= bytesRead;
    updateRequired = true;

    if (binaryBytesRemaining == 0) {
      // Transfer complete
      currentFile.flush();
      currentFile.close();
      inBinaryMode = false;

      setState(WirelessState::WAITING);
      setStatus("Received: " + currentFilename + "\nWaiting for more...");

      // Send OK to acknowledge completion
      sendJsonResponse(OpCode::OK, "{}");
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
