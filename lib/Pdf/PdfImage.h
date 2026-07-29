#pragma once

#include <cstdint>
#include <vector>

#include "PdfDoc.h"

// Image XObject extraction for the PDF -> EPUB converter.
//
// Two output paths, both producing bytes the reader's own decoders already
// handle:
//   * /DCTDecode          -> the embedded JPEG passed through byte-for-byte.
//   * raw / Flate samples -> re-encoded as PNG (lib/ImageTranscode/PngWriter),
//                            for /DeviceGray, /DeviceRGB and /Indexed at 1, 2,
//                            4 or 8 bits per component, plus /ImageMask.
//
// Everything else — JPXDecode, CCITTFaxDecode, LZWDecode, DeviceCMYK, Lab,
// Separation/DeviceN, 16-bit samples, inline BI/ID/EI images — is skipped with
// one log line per format per conversion. An image never fails the document.
// /SMask and /Mask transparency is ignored (the target is a 1-bit e-ink panel).
namespace PdfImage {

// Exactly one of jpeg/png is non-empty on success.
struct Decoded {
  ByteBuf jpeg;              // pass through as image/jpeg
  std::vector<uint8_t> png;  // re-encoded, image/png
  uint32_t width = 0;
  uint32_t height = 0;
  bool isJpeg() const { return !jpeg.empty(); }
};

// Untrusted-input sanity caps. The real limiter on device is the nothrow
// allocation failing first; these stop absurd headers before any arithmetic.
constexpr uint64_t MAX_PIXELS = 16u * 1024 * 1024;
constexpr uint64_t MAX_SAMPLE_BYTES = 8u * 1024 * 1024;

// Decodes one image XObject. `res` is the resource dict the XObject was invoked
// from — named /ColorSpace entries resolve through it; may be null.
// False = skip this image (already logged); the caller continues the page.
bool decode(PdfDoc& doc, const PdfObj& img, const PdfObj* res, Decoded& out);

// Re-arms the once-per-format skip logging. Called at the start of a conversion
// so the second book of a session still reports why its images were dropped.
void resetSkipLog();

}  // namespace PdfImage
