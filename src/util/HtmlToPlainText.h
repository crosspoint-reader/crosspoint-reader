#pragma once

// Ported from crosspoint-reader (PR #2836 by Uri Tauber).

#include <string>

// Convert an HTML fragment to readable plain text. This intentionally ignores
// styling; block elements become line breaks and HTML entities are decoded.
//
// The fallback for a dictionary definition whose HTML the layout path could not
// use -- too damaged to parse, too large to keep resident as laid-out Pages, or
// laid out on a heap that could not spare the room. See DictHtmlPages.h.
//
// Converts in place: the result never needs more bytes than the markup it
// replaces, so the definition's own buffer holds it and no second copy of a
// large definition is ever allocated.
void htmlToPlainTextInPlace(std::string& text);
