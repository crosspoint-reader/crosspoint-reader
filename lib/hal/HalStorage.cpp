#include "HalStorage.h"
#include <SDCardManager.h>

HalStorage HalStorage::instance;

HalStorage::HalStorage() {}

bool HalStorage::begin() {
  return SdMan.begin();
}

bool HalStorage::ready() const {
  return SdMan.ready();
}

std::vector<String> HalStorage::listFiles(const char* path, int maxFiles) {
  return SdMan.listFiles(path, maxFiles);
}

String HalStorage::readFile(const char* path) {
  return SdMan.readFile(path);
}

bool HalStorage::readFileToStream(const char* path, Print& out, size_t chunkSize) {
  return SdMan.readFileToStream(path, out, chunkSize);
}

size_t HalStorage::readFileToBuffer(const char* path, char* buffer, size_t bufferSize, size_t maxBytes) {
  return SdMan.readFileToBuffer(path, buffer, bufferSize, maxBytes);
}

bool HalStorage::writeFile(const char* path, const String& content) {
  return SdMan.writeFile(path, content);
}

bool HalStorage::ensureDirectoryExists(const char* path) {
  return SdMan.ensureDirectoryExists(path);
}

File HalStorage::open(const char* path, const oflag_t oflag) {
  return SdMan.open(path, oflag);
}

bool HalStorage::mkdir(const char* path, const bool pFlag) {
  return SdMan.mkdir(path, pFlag);
}

bool HalStorage::exists(const char* path) {
  return SdMan.exists(path);
}

bool HalStorage::remove(const char* path) {
  return SdMan.remove(path);
}

bool HalStorage::rmdir(const char* path) {
  return SdMan.rmdir(path);
}

bool HalStorage::openFileForRead(const char* moduleName, const char* path, File& file) {
  return SdMan.openFileForRead(moduleName, path, file);
}

bool HalStorage::openFileForRead(const char* moduleName, const std::string& path, File& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForRead(const char* moduleName, const String& path, File& file) {
  return openFileForRead(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const char* path, File& file) {
  return SdMan.openFileForWrite(moduleName, path, file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const std::string& path, File& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::openFileForWrite(const char* moduleName, const String& path, File& file) {
  return openFileForWrite(moduleName, path.c_str(), file);
}

bool HalStorage::removeDir(const char* path) {
  return SdMan.removeDir(path);
}