#include <Logging.h>
#include <emscripten/emscripten.h>
#include <emscripten/fiber.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <algorithm>
#include <cassert>
#include <limits>

namespace {
constexpr size_t MAX_TASKS = 4;
constexpr size_t MAX_SEMAPHORES = 32;
constexpr size_t TASK_STACK_BYTES = 1024 * 1024;
constexpr size_t ASYNC_STACK_BYTES = 256 * 1024;
constexpr double NEVER = std::numeric_limits<double>::infinity();
enum class State { Free, Ready, Waiting };
}  // namespace

struct BrowserTask {
  emscripten_fiber_t fiber{};
  void (*entry)(void*) = nullptr;
  void* argument = nullptr;
  const char* name = "browser";
  State state = State::Free;
  uint32_t notifications = 0;
  double wakeAt = NEVER;
  const void* waitObject = nullptr;
};

struct BrowserSemaphore {
  bool used = false;
  bool binary = false;
  bool recursive = false;
  uint32_t count = 0;
  TaskHandle_t owner = nullptr;
};

namespace {
// Browser-only fixed pools avoid allocation during task creation or switching.
BrowserTask tasks[MAX_TASKS];
alignas(16) uint8_t taskStacks[MAX_TASKS][TASK_STACK_BYTES];
alignas(16) uint8_t asyncStacks[MAX_TASKS][ASYNC_STACK_BYTES];
BrowserSemaphore semaphores[MAX_SEMAPHORES];
BrowserTask rootTask;
emscripten_fiber_t rootFiber;
alignas(16) uint8_t rootAsyncStack[ASYNC_STACK_BYTES];
TaskHandle_t currentTask = nullptr;
size_t cursor = 0;
unsigned int criticalDepth = 0;
bool stopped = false;

double deadline(uint32_t ticks) {
  return ticks == portMAX_DELAY ? NEVER : emscripten_get_now() + static_cast<double>(ticks) * portTICK_PERIOD_MS;
}

void suspend(double wakeAt, const void* object) {
  assert(currentTask && criticalDepth == 0);
  currentTask->wakeAt = wakeAt;
  currentTask->waitObject = object;
  if (currentTask->state != State::Free) currentTask->state = State::Waiting;
  emscripten_fiber_swap(&currentTask->fiber, &rootFiber);
}

void wake(const void* object) {
  for (auto& task : tasks) {
    if (task.state == State::Waiting && task.waitObject == object) task.state = State::Ready;
  }
}

void enterTask(void* argument) {
  auto* task = static_cast<BrowserTask*>(argument);
  task->entry(task->argument);
  vTaskDelete(nullptr);
}

SemaphoreHandle_t createSemaphore(bool binary, bool recursive) {
  for (auto& sem : semaphores) {
    if (sem.used) continue;
    sem = {true, binary, recursive, 0, nullptr};
    return &sem;
  }
  LOG_ERR("PREVIEW", "Semaphore pool exhausted");
  return nullptr;
}

bool available(SemaphoreHandle_t sem) {
  return sem->binary ? sem->count != 0 : !sem->owner || (sem->recursive && sem->owner == xTaskGetCurrentTaskHandle());
}
}  // namespace

namespace browser {
int stepTasks() {
  if (stopped) return 16;
  assert(!currentTask);
  emscripten_fiber_init_from_current_context(&rootFiber, rootAsyncStack, sizeof(rootAsyncStack));
  const double end = emscripten_get_now() + 4;
  for (unsigned int switches = 0; switches < 32 && emscripten_get_now() < end; ++switches) {
    TaskHandle_t next = nullptr;
    for (size_t i = 0; i < MAX_TASKS; ++i) {
      auto& task = tasks[cursor];
      cursor = (cursor + 1) % MAX_TASKS;
      if (task.state == State::Waiting && task.wakeAt <= emscripten_get_now()) task.state = State::Ready;
      if (task.state == State::Ready) {
        next = &task;
        break;
      }
    }
    if (!next) break;
    currentTask = next;
    emscripten_fiber_swap(&rootFiber, &next->fiber);
    currentTask = nullptr;
  }
  double delay = 16;
  for (const auto& task : tasks) {
    if (task.state == State::Ready) return 0;
    if (task.state == State::Waiting) delay = std::min(delay, task.wakeAt - emscripten_get_now());
  }
  return static_cast<int>(std::max(0.0, delay));
}
void stopTasks() { stopped = true; }
}  // namespace browser

BaseType_t xTaskCreate(void (*fn)(void*), const char* name, uint32_t, void* param, BaseType_t, TaskHandle_t* handle) {
  if (handle) *handle = nullptr;
  for (size_t i = 0; i < MAX_TASKS; ++i) {
    auto& task = tasks[i];
    if (task.state != State::Free) continue;
    task.entry = fn;
    task.argument = param;
    task.name = name ? name : "task";
    task.notifications = 0;
    task.state = State::Ready;
    task.waitObject = nullptr;
    task.wakeAt = NEVER;
    emscripten_fiber_init(&task.fiber, enterTask, &task, taskStacks[i], TASK_STACK_BYTES, asyncStacks[i],
                          ASYNC_STACK_BYTES);
    if (handle) *handle = &task;
    return pdPASS;
  }
  LOG_ERR("PREVIEW", "Task pool exhausted");
  return pdFALSE;
}
TaskHandle_t xTaskGetCurrentTaskHandle() { return currentTask ? currentTask : &rootTask; }
const char* pcTaskGetName(TaskHandle_t handle) { return (handle ? handle : xTaskGetCurrentTaskHandle())->name; }
void vTaskDelay(uint32_t ticks) { suspend(deadline(ticks), nullptr); }
void vTaskDelete(TaskHandle_t handle) {
  if (!handle) handle = currentTask;
  if (!handle) return;
  handle->state = State::Free;
  if (handle == currentTask) suspend(NEVER, nullptr);
}
uint32_t ulTaskNotifyTake(int clearOnExit, uint32_t ticksToWait) {
  const auto task = xTaskGetCurrentTaskHandle();
  const double until = deadline(ticksToWait);
  while (!task->notifications) {
    if (emscripten_get_now() >= until) return 0;
    suspend(until, task);
  }
  const uint32_t count = task->notifications;
  task->notifications = clearOnExit ? 0 : count - 1;
  return count;
}
void xTaskNotify(TaskHandle_t handle, uint32_t, int) {
  if (!handle || handle->state == State::Free) return;
  ++handle->notifications;
  wake(handle);
}
void taskENTER_CRITICAL(portMUX_TYPE*) { ++criticalDepth; }
void taskEXIT_CRITICAL(portMUX_TYPE*) {
  assert(criticalDepth);
  --criticalDepth;
}
SemaphoreHandle_t xSemaphoreCreateMutex() { return createSemaphore(false, false); }
SemaphoreHandle_t xSemaphoreCreateRecursiveMutex() { return createSemaphore(false, true); }
SemaphoreHandle_t xSemaphoreCreateBinary() { return createSemaphore(true, false); }
void vSemaphoreDelete(SemaphoreHandle_t sem) {
  if (sem) sem->used = false;
}
bool xSemaphoreTake(SemaphoreHandle_t sem, uint32_t ticksToWait) {
  if (!sem || !sem->used) return false;
  const double until = deadline(ticksToWait);
  while (!available(sem)) {
    if (emscripten_get_now() >= until) return false;
    suspend(until, sem);
  }
  if (sem->binary) {
    sem->count = 0;
  } else {
    sem->owner = xTaskGetCurrentTaskHandle();
    ++sem->count;
  }
  return true;
}
bool xSemaphoreGive(SemaphoreHandle_t sem) {
  if (!sem || !sem->used) return false;
  if (sem->binary) {
    if (sem->count) return false;
    sem->count = 1;
  } else {
    if (sem->owner != xTaskGetCurrentTaskHandle() || !sem->count) return false;
    if (--sem->count) return true;
    sem->owner = nullptr;
  }
  wake(sem);
  return true;
}
TaskHandle_t xSemaphoreGetMutexHolder(SemaphoreHandle_t sem) { return sem && !sem->binary ? sem->owner : nullptr; }
int xQueuePeek(SemaphoreHandle_t sem, void*, uint32_t ticksToWait) {
  if (!sem || !sem->used) return pdFALSE;
  const double until = deadline(ticksToWait);
  while (sem->binary ? !sem->count : sem->owner != nullptr) {
    if (emscripten_get_now() >= until) return pdFALSE;
    suspend(until, sem);
  }
  return pdTRUE;
}
