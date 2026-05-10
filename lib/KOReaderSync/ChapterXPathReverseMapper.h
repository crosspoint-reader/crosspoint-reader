#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

// outListItemIndex (when non-null) receives the running <li> count at the matched
// element's position whenever the target XPath's deepest element is /li[N]. Set to
// 0 if the target wasn't <li>-anchored or no match was found.
bool findProgressForXPathInternal(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                  float& outIntraSpineProgress, bool& outExactMatch,
                                  uint16_t* outListItemIndex = nullptr);

}  // namespace ChapterXPathIndexerInternal
