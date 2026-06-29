#pragma once
#include "Task.h"

class DictionaryLookupController;

class DictLookupTask : public Task {
  DictionaryLookupController& owner;

 public:
  explicit DictLookupTask(DictionaryLookupController& o) : owner(o) {}

 protected:
  void run() override;
};
