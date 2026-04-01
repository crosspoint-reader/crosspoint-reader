#pragma once

#include <Epub.h>

#include <memory>
#include <string>

namespace ChapterXPathIndexerInternal {

bool findProgressForXPathInternal(const std::shared_ptr<Epub>& epub, int spineIndex, const std::string& xpath,
                                  float& outIntraSpineProgress, bool& outExactMatch);

}  // namespace ChapterXPathIndexerInternal
