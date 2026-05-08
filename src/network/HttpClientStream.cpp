#include "HttpClientStream.h"

#include <climits>

HttpClientStream::HttpClientStream(esp_http_client_handle_t client, int64_t contentLength, size_t maxBytes)
    : client(client), contentLength(contentLength), maxBytes(maxBytes) {}

int HttpClientStream::available() {
  if (hasError() || endOfStream) {
    return 0;
  }
  if (contentLength < 0) {
    if (maxBytes > 0 && bytesRead >= maxBytes) {
      return 0;
    }
    return 1;
  }
  const int64_t remaining = contentLength - bytesRead;
  if (remaining <= 0) {
    return 0;
  }
  size_t availableBytes = remaining > INT_MAX ? INT_MAX : static_cast<size_t>(remaining);
  if (maxBytes > 0) {
    const size_t remainingLimit = bytesRead >= maxBytes ? 0 : maxBytes - bytesRead;
    if (availableBytes > remainingLimit) {
      availableBytes = remainingLimit;
    }
  }
  return static_cast<int>(availableBytes);
}

int HttpClientStream::read() {
  uint8_t byte = 0;
  const size_t readCount = readBytes(reinterpret_cast<char*>(&byte), 1);
  return readCount == 1 ? byte : -1;
}

int HttpClientStream::peek() { return -1; }

size_t HttpClientStream::write(uint8_t) { return 0; }

size_t HttpClientStream::readBytes(char* buffer, size_t length) {
  if (buffer == nullptr || length == 0) {
    return 0;
  }

  if (maxBytes > 0) {
    if (bytesRead >= maxBytes) {
      limitExceeded = true;
      endOfStream = true;
      return 0;
    }
    const size_t remainingLimit = maxBytes - bytesRead;
    if (length > remainingLimit) {
      length = remainingLimit;
    }
  }

  const int readLen = esp_http_client_read(client, buffer, static_cast<int>(length));
  if (readLen == 0) {
    endOfStream = true;
    return 0;
  }
  if (readLen < 0) {
    lastReadError = readLen;
    endOfStream = true;
    return 0;
  }
  bytesRead += readLen;
  return static_cast<size_t>(readLen);
}
