#include "HttpClientStream.h"

#include <climits>

HttpClientStream::HttpClientStream(esp_http_client_handle_t client, int64_t contentLength)
    : client(client), contentLength(contentLength) {}

int HttpClientStream::available() {
  if (hasError() || endOfStream) {
    return 0;
  }
  if (contentLength < 0) {
    return 1;
  }
  const int64_t remaining = contentLength - bytesRead;
  if (remaining <= 0) {
    return 0;
  }
  return remaining > INT_MAX ? INT_MAX : static_cast<int>(remaining);
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
