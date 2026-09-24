#include "LibraryFormat.h"

namespace library {

const char* clixValidityName(const ClixValidity v) {
  switch (v) {
    case ClixValidity::Ok:
      return "ok";
    case ClixValidity::BadMagic:
      return "bad magic";
    case ClixValidity::UnknownFormatVersion:
      return "unknown format version";
    case ClixValidity::StaleFoldVersion:
      return "stale fold version";
    case ClixValidity::SizeMismatch:
      return "size mismatch (truncated?)";
    case ClixValidity::CountOutOfRange:
      return "book count out of range";
    case ClixValidity::SectionsInconsistent:
      return "section offsets inconsistent";
  }
  return "unknown";
}

}  // namespace library
