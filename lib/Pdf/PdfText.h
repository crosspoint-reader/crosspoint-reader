#pragma once

#include <cstdint>
#include <string>

#include "PdfDoc.h"
#include "PdfFont.h"

// Content-stream text extraction: interprets one page's content (plus Form
// XObjects it invokes), decodes show strings through the fonts, and reflows
// the result into XHTML <p> paragraphs using position heuristics. Image
// XObjects are handed to an optional sink at the point the content stream
// invokes them, so illustrations land in reading order.
namespace PdfText {

// Called for each image XObject the content stream invokes (`Do`), in reading
// order, including inside Form XObjects. `objNum` is the XObject's object
// number for de-duplication, or 0 when it is not an indirect object. `res` is
// the resource dict the XObject was invoked from — named /ColorSpace entries
// resolve through it — and may be null. The sink appends its own markup to the
// same output string; the interpreter has already closed any open paragraph.
struct ImageSink {
  void* ctx = nullptr;
  void (*emit)(void* ctx, PdfDoc& doc, const PdfObj& img, uint32_t objNum, const PdfObj* res) = nullptr;
};

// Appends the page's paragraphs to out. anyText is set when at least one
// visible character was produced. Returns false only on hard page failure
// (missing/corrupt page object) — the caller degrades to an empty page.
bool extractPage(PdfDoc& doc, const PdfDoc::Page& page, PdfFontCache& fonts, std::string& out, bool* anyText,
                 const ImageSink* images = nullptr);

}  // namespace PdfText
