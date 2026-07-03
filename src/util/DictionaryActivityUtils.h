#pragma once
#include "activities/Activity.h"

namespace DictUtils {

// D-006: Back-cancel pattern — sets isCancelled=true and finishes the activity.
inline void cancelAndFinish(Activity& act) {
  ActivityResult r;
  r.isCancelled = true;
  act.setResult(std::move(r));
  act.finish();
}

}  // namespace DictUtils
