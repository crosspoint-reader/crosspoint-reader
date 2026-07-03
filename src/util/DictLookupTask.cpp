#include "DictLookupTask.h"

#include "DictionaryLookupController.h"

void DictLookupTask::run() { owner.runLookup(); }
