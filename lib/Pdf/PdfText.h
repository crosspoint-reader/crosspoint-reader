#pragma once

#include <string>

#include "PdfDoc.h"
#include "PdfFont.h"

// Content-stream text extraction: interprets one page's content (plus Form
// XObjects it invokes), decodes show strings through the fonts, and reflows
// the result into XHTML <p> paragraphs using position heuristics.
namespace PdfText {

// Appends the page's paragraphs to out. anyText is set when at least one
// visible character was produced. Returns false only on hard page failure
// (missing/corrupt page object) — the caller degrades to an empty page.
bool extractPage(PdfDoc& doc, const PdfDoc::Page& page, PdfFontCache& fonts, std::string& out, bool* anyText);

}  // namespace PdfText
