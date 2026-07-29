#pragma once

// PDF -> EPUB transmuter. Converts a text-based (non-scanned, non-encrypted)
// PDF into a minimal reflowed EPUB2 on the SD card, then the stock EPUB
// pipeline (Epub/Section/ChapterHtmlSlimParser) opens it natively — TOC, page
// cache, progress and fonts all work unchanged. Conversion runs once per book
// and is cached under /.crosspoint/pdf_<hash>/book.epub keyed on source file
// size.
//
// The extractor parses the xref (classic + 1.5 xref streams), walks the page
// tree, interprets each page's content stream (BT/Tj/TJ/Td/Tm + CTM), decodes
// show strings through /ToUnicode CMaps or simple-font encodings, and reflows
// the positioned runs into paragraphs (line/paragraph breaks from Y deltas,
// hyphen joining, gap-driven spaces). Chapters are 10-page groups.
//
// Image XObjects are emitted in reading order as OEBPS/images/imgNNNN.{jpg,png}
// (see PdfImage), stored once per object number however many pages draw them. A
// PDF with no text but usable images converts to an image-only book, which is
// what a scanned or comic PDF is.

#include <string>

class PdfToEpub {
 public:
  // Returns the converted EPUB path, or "" on failure (errorOut gets a
  // user-showable reason). Reuses the cached conversion when valid.
  static std::string ensureConverted(const std::string& pdfPath, std::string* errorOut = nullptr);
};
