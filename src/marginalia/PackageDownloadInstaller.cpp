#include "PackageDownloadInstaller.h"

#include <HalStorage.h>
#include <Logging.h>
#include <mbedtls/sha256.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <memory>

#include "PackageArchiveInstaller.h"
#include "PackageStore.h"
#include "network/HttpDownloader.h"
#include "util/UrlUtils.h"

namespace Marginalia {

namespace {

constexpr size_t MAX_DOWNLOAD_ARCHIVE_BYTES = 512 * 1024;
constexpr size_t HASH_CHUNK_BYTES = 4096;

PackageDownloadInstallResult resultError(const std::string& error) {
  PackageDownloadInstallResult result;
  result.error = error;
  return result;
}

bool looksLikePackageUrl(const std::string& url) {
  if (url.length() < 8 || url.length() > 512) return false;
  return url.rfind("http://", 0) == 0 || UrlUtils::isHttpsUrl(url);
}

bool isSha256Hex(const std::string& value) {
  if (value.length() != 64) return false;
  return std::all_of(value.begin(), value.end(), [](unsigned char c) { return std::isxdigit(c); });
}

std::string lowercase(const std::string& value) {
  std::string out = value;
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return out;
}

char hexNibble(const uint8_t value) { return value < 10 ? static_cast<char>('0' + value) : static_cast<char>('a' + value - 10); }

std::string bytesToHex(const uint8_t* bytes, const size_t size) {
  std::string out;
  out.resize(size * 2);
  for (size_t i = 0; i < size; i++) {
    out[i * 2] = hexNibble(bytes[i] >> 4);
    out[i * 2 + 1] = hexNibble(bytes[i] & 0x0f);
  }
  return out;
}

bool sha256File(const std::string& path, std::string& outHex) {
  FsFile file;
  if (!Storage.openFileForRead("MPKG", path.c_str(), file) || !file) {
    return false;
  }

  std::unique_ptr<uint8_t[]> buffer(new (std::nothrow) uint8_t[HASH_CHUNK_BYTES]);
  if (!buffer) {
    file.close();
    return false;
  }

  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);

  while (file.available()) {
    const int read = file.read(buffer.get(), HASH_CHUNK_BYTES);
    if (read < 0) {
      mbedtls_sha256_free(&ctx);
      file.close();
      return false;
    }
    if (read == 0) break;
    mbedtls_sha256_update(&ctx, buffer.get(), static_cast<size_t>(read));
  }

  uint8_t digest[32];
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  file.close();

  outHex = bytesToHex(digest, sizeof(digest));
  return true;
}

}  // namespace

PackageDownloadInstallResult downloadPackageArchiveToInbox(const std::string& url, const std::string& expectedSha256,
                                                           const size_t expectedSize) {
  const std::string normalizedSha256 = lowercase(expectedSha256);
  if (!looksLikePackageUrl(url)) {
    return resultError("invalid package URL");
  }
  if (!normalizedSha256.empty() && !isSha256Hex(normalizedSha256)) {
    return resultError("invalid package checksum");
  }
  if (expectedSize > MAX_DOWNLOAD_ARCHIVE_BYTES) {
    return resultError("package archive is too large");
  }
  if (!ensurePackageBaseDirectories()) {
    return resultError("could not create package directories");
  }

  const std::string downloadPath = std::string(PACKAGE_STAGING_ROOT) + "/download.mpkg.zip";
  if (Storage.exists(downloadPath.c_str())) Storage.remove(downloadPath.c_str());

  const auto downloadResult = HttpDownloader::downloadToFile(url, downloadPath);
  if (downloadResult != HttpDownloader::OK) {
    Storage.remove(downloadPath.c_str());
    return resultError("package download failed");
  }

  FsFile file;
  if (!Storage.openFileForRead("MPKG", downloadPath.c_str(), file) || !file) {
    Storage.remove(downloadPath.c_str());
    return resultError("downloaded package could not be opened");
  }
  const size_t downloadedSize = file.fileSize();
  file.close();

  if (downloadedSize == 0 || downloadedSize > MAX_DOWNLOAD_ARCHIVE_BYTES) {
    Storage.remove(downloadPath.c_str());
    return resultError("downloaded package has invalid size");
  }
  if (expectedSize > 0 && downloadedSize != expectedSize) {
    Storage.remove(downloadPath.c_str());
    return resultError("downloaded package size mismatch");
  }

  if (!normalizedSha256.empty()) {
    std::string actualSha256;
    if (!sha256File(downloadPath, actualSha256)) {
      Storage.remove(downloadPath.c_str());
      return resultError("could not verify package checksum");
    }
    if (actualSha256 != normalizedSha256) {
      Storage.remove(downloadPath.c_str());
      return resultError("package checksum mismatch");
    }
  }

  const auto extractResult = extractPackageArchiveToInbox(downloadPath);
  Storage.remove(downloadPath.c_str());
  if (!extractResult.ok) {
    return resultError(extractResult.error);
  }

  PackageDownloadInstallResult result;
  result.ok = true;
  result.packageId = extractResult.packageId;
  result.packageName = extractResult.packageName;
  return result;
}

}  // namespace Marginalia
