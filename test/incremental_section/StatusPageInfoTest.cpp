#include <cassert>
#include <cstring>

#include "../../src/components/themes/StatusPageInfo.h"

static_assert(shouldDrawChapterProgressBar(StatusPageInfo{0, 10, true, false, false}));
static_assert(!shouldDrawChapterProgressBar(StatusPageInfo{0, 10, false, true, false}));
static_assert(shouldDrawCurrentIndexingIndicator(StatusPageInfo{0, 10, false, true, false}));
static_assert(!shouldDrawCurrentIndexingIndicator(StatusPageInfo{0, 0, false, false, false}));
static_assert(shouldDrawFutureIndexingIndicator(StatusPageInfo{0, 10, true, false, true}));

int main() {
  char buffer[16] = {};

  formatStatusPageText(buffer, sizeof(buffer), StatusPageInfo{3, 12, true, false, false});
  assert(std::strcmp(buffer, "4/12") == 0);

  formatStatusPageText(buffer, sizeof(buffer), StatusPageInfo{3, 8, false, true, false});
  assert(std::strcmp(buffer, "4/8+") == 0);

  formatStatusPageText(buffer, sizeof(buffer), StatusPageInfo{0, 0, false, false, false});
  assert(std::strcmp(buffer, "1/?") == 0);

  char tiny[2] = {};
  formatStatusPageText(tiny, sizeof(tiny), StatusPageInfo{99, 100, true, false, false});
  assert(tiny[sizeof(tiny) - 1] == '\0');
}
