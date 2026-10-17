#include "ImageDecoderFactory.h"

#include <FsHelpers.h>
#include <Logging.h>
#include <ZipFile.h>

#include <memory>
#include <string>

#include "GifToFramebufferConverter.h"
#include "JpegToFramebufferConverter.h"
#include "PngToFramebufferConverter.h"

std::unique_ptr<JpegToFramebufferConverter> ImageDecoderFactory::jpegDecoder = nullptr;
std::unique_ptr<PngToFramebufferConverter> ImageDecoderFactory::pngDecoder = nullptr;
std::unique_ptr<GifToFramebufferConverter> ImageDecoderFactory::gifDecoder = nullptr;

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string& imagePath) {
  std::string ext = imagePath;
  size_t dotPos = ext.rfind('.');
  if (dotPos != std::string::npos) {
    ext = ext.substr(dotPos);
    for (auto& c : ext) {
      c = tolower(c);
    }
  } else {
    ext = "";
  }

  ImageFormatDetector::Format format = ImageFormatDetector::Format::Unknown;
  if (JpegToFramebufferConverter::supportsFormat(ext)) {
    format = ImageFormatDetector::Format::Jpeg;
  } else if (PngToFramebufferConverter::supportsFormat(ext)) {
    format = ImageFormatDetector::Format::Png;
  } else if (GifToFramebufferConverter::supportsFormat(ext)) {
    format = ImageFormatDetector::Format::Gif;
  }

  ImageToFramebufferDecoder* decoder = getDecoder(format);
  if (!decoder) LOG_ERR("DEC", "No decoder found for image: %s", imagePath.c_str());
  return decoder;
}

ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const ImageFormatDetector::Format format) {
  switch (format) {
    case ImageFormatDetector::Format::Jpeg:
      if (!jpegDecoder) {
        jpegDecoder.reset(new JpegToFramebufferConverter());
      }
      return jpegDecoder.get();
    case ImageFormatDetector::Format::Png:
      if (!pngDecoder) {
        pngDecoder.reset(new PngToFramebufferConverter());
      }
      return pngDecoder.get();
    case ImageFormatDetector::Format::Gif:
      if (!gifDecoder) {
        gifDecoder.reset(new GifToFramebufferConverter());
      }
      return gifDecoder.get();
    case ImageFormatDetector::Format::Unknown:
      return nullptr;
  }
  return nullptr;
}

bool ImageDecoderFactory::isFormatSupported(const std::string& imagePath) { return getDecoder(imagePath) != nullptr; }

bool ImageDecoderFactory::getDimensionsFromZipEntry(const std::string& epubFilePath, const std::string& entryPath,
                                                    ImageDimensions& out) {
  // PNG needs only 24 bytes; JPEG SOF is typically within 4 KB.
  // Heap-allocate to avoid blowing the ~8 KB ActivityManager task stack during createSectionFile.
  constexpr size_t kHeaderBufSize = 4 * 1024;
  auto* buf = static_cast<uint8_t*>(malloc(kHeaderBufSize));
  if (!buf) {
    LOG_ERR("DEC", "getDimensionsFromZipEntry: failed to alloc %u bytes", static_cast<unsigned>(kHeaderBufSize));
    return false;
  }

  const std::string normalised = FsHelpers::normalisePath(entryPath);
  LOG_DBG("DEC", "getDimensionsFromZipEntry: epub=%s request=%s normalized=%s", epubFilePath.c_str(), entryPath.c_str(),
          normalised.c_str());
  const size_t bytesRead = ZipFile(epubFilePath).readBytesFromEntry(normalised.c_str(), buf, kHeaderBufSize);
  if (bytesRead == 0) {
    free(buf);
    LOG_ERR("DEC", "getDimensionsFromZipEntry: failed to read header from %s:%s (normalized=%s)", epubFilePath.c_str(),
            entryPath.c_str(), normalised.c_str());
    return false;
  }

  bool ok = false;
  if (bytesRead >= 2 && buf[0] == 0xFF && buf[1] == 0xD8) {
    ok = JpegToFramebufferConverter::getDimensionsFromBuffer(buf, bytesRead, out);
    if (!ok) {
      // SOF can sit behind large Exif/XMP/ICC metadata beyond the header buffer
      // (Photoshop exports are common offenders). Stream the entry to find it.
      ok = JpegToFramebufferConverter::getDimensionsFromZipEntryStreaming(epubFilePath, normalised, out);
    }
  } else if (bytesRead >= 8 && buf[0] == 0x89 && buf[1] == 0x50) {
    ok = PngToFramebufferConverter::getDimensionsFromBuffer(buf, bytesRead, out);
  } else if (bytesRead >= 10 && buf[0] == 'G' && buf[1] == 'I' && buf[2] == 'F') {
    ok = GifToFramebufferConverter::getDimensionsFromBuffer(buf, bytesRead, out);
  } else {
    uint32_t sig = 0;
    if (bytesRead >= 4) {
      sig = (static_cast<uint32_t>(buf[0]) << 24) | (static_cast<uint32_t>(buf[1]) << 16) |
            (static_cast<uint32_t>(buf[2]) << 8) | static_cast<uint32_t>(buf[3]);
    }
    LOG_ERR("DEC", "getDimensionsFromZipEntry: unrecognised format for %s (bytes=%u, sig=0x%08X)", entryPath.c_str(),
            static_cast<unsigned>(bytesRead), sig);
  }

  free(buf);
  return ok;
}
