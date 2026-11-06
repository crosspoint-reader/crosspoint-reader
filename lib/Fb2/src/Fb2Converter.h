#pragma once

// FB2 (FictionBook 2.0) to EPUB converter.
//
// Streams the source .fb2 XML with expat and writes a synthetic EPUB
// container (stored, uncompressed ZIP) that the stock Epub engine opens
// like any other book: chapters, TOC, images, internal links and footnote
// bodies all flow through the normal reading pipeline.
//
// Conversion strategy (adapted for CrossPoint from the FB2 work in
// papyrix-reader, https://github.com/bigbag/papyrix-reader):
//   pass 1 - description/title-info metadata (parse stops at </description>)
//   pass 2 - structure scan: logical chapter numbering for every id= anchor,
//            the <binary> image registry, chapter count
//   pass 3 - emission: chapters as XHTML files, base64 binaries decoded to
//            image entries, nav.xhtml + content.opf

#include <cstdint>
#include <string>
#include <vector>

#include "Fb2Passes.h"

class Fb2Converter {
 public:
  // Moved to Fb2Passes.h (host-testable); kept under the class name for callers.
  using Metadata = fb2passes::Metadata;

  // Convert the FB2 at fb2Path into an EPUB container at outPath.
  // On failure returns false and fills `error` with a short reason.
  static bool convert(const std::string& fb2Path, const std::string& outPath, std::string& error);

  // Read only description/title-info (fast; stops parsing at </description>).
  // Used by the library index for title/author rows.
  static bool readMetadata(const std::string& fb2Path, Metadata& meta, std::string& error);

  // content-type -> ".jpg"/".png"/".gif"; "" when the firmware cannot render it
  static const char* imageExtensionForContentType(const char* contentType);
};
