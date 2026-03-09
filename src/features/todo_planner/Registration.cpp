#include "features/todo_planner/Registration.h"

#include <FeatureFlags.h>
#include <HalStorage.h>

#include "activities/todo/TodoActivity.h"
#include "activities/todo/TodoPlannerStorage.h"
#include "core/features/FeatureCatalog.h"
#include "core/registries/HomeActionRegistry.h"
#include "util/DateUtils.h"

namespace features::todo_planner {
namespace {

#if ENABLE_TODO_PLANNER
static bool shouldExposeTodoPlannerHomeAction(core::HomeActionEntry::HomeActionContext ctx) {
  (void)ctx;
  return core::FeatureCatalog::isEnabled("todo_planner");
}

static Activity* createTodoPlannerHomeActionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                                     void* callbackCtx, void (*onBack)(void* ctx)) {
  const bool markdownEnabled = core::FeatureCatalog::isEnabled("markdown");
  const std::string today = DateUtils::currentDate();

  std::string filePath;
  std::string dateTitle;
  if (today.empty()) {
    filePath = std::string("/daily/undated") + (markdownEnabled ? ".md" : ".txt");
    dateTitle = "Undated";
  } else {
    const std::string markdownPath = "/daily/" + today + ".md";
    const std::string textPath = "/daily/" + today + ".txt";
    const bool markdownExists = Storage.exists(markdownPath.c_str());
    const bool textExists = Storage.exists(textPath.c_str());
    filePath = TodoPlannerStorage::dailyPath(today, markdownEnabled, markdownExists, textExists);
    dateTitle = today;
  }

  return new TodoActivity(renderer, mappedInput, filePath, dateTitle, callbackCtx, onBack);
}
#endif

}  // namespace

void registerFeature() {
#if ENABLE_TODO_PLANNER
  core::HomeActionEntry homeEntry{};
  homeEntry.actionId = "todo_planner";
  homeEntry.shouldExpose = shouldExposeTodoPlannerHomeAction;
  homeEntry.create = createTodoPlannerHomeActionActivity;
  core::HomeActionRegistry::add(homeEntry);
#endif
}

}  // namespace features::todo_planner
