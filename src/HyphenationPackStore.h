#pragma once

#include <Epub/hyphenation/LanguageRegistry.h>

#include <cstddef>
#include <cstdint>

namespace HyphenationPackStore {

enum class InstallResult : uint8_t { OK, INVALID_FILE, UNSUPPORTED, NO_SPACE, IO_ERROR, FLASH_ERROR };

bool begin();
bool lookup(const char* primaryTag, ExternalHyphenationPatterns& out);
bool isInstalled(const char* primaryTag);
size_t installedCount();
bool installedAt(size_t index, char code[3], uint32_t& size);
InstallResult installFromSd(const char* path);
bool remove(const char* primaryTag);

}  // namespace HyphenationPackStore
