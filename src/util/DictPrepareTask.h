#pragma once
#include "util/Task.h"

class DictPrepareActivity;

class DictPrepareTask : public Task {
  DictPrepareActivity& owner;

 public:
  explicit DictPrepareTask(DictPrepareActivity& o) : owner(o) {}

 protected:
  void run() override;
};
