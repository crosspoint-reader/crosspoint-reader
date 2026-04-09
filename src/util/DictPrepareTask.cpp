#include "DictPrepareTask.h"

#include "../activities/settings/DictPrepareActivity.h"

void DictPrepareTask::run() { owner.runSteps(); }
