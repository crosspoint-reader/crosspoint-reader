#pragma once

#include <string>

/**
 * Read and validate the original KOReader document ID from an OPF meta
 * element's null-terminated attribute name/value array.
 */
bool extractOriginalDocumentIdMetadata(const char* const* attributes, std::string& documentId);
