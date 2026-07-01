#include <Logging.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cassert>

extern "C" void vApplicationStackOverflowHook(TaskHandle_t task, char* taskName) {
  (void)task;
  const char* name = taskName ? taskName : "(unknown)";
  LOG_ERR("RTOS", "Stack overflow in task: %s", name);
  assert(false && "Stack overflow");
}
