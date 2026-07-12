#pragma once

// BootSwitch is shared with the vendored Biscuit firmware (biscuit/), which
// must not inherit lib/Logging: that header aliases Serial to an HWCDC and
// hijacks the Serial symbol via macro, both of which are CrossPoint build
// assumptions. Biscuit builds define BOOTSWITCH_USE_ESP_LOG to route through
// the IDF logger instead; CrossPoint builds keep the mandated LOG_* macros.
#if defined(BOOTSWITCH_USE_ESP_LOG)
#include <esp_log.h>
#define BOOTSWITCH_LOG_ERR(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)
#define BOOTSWITCH_LOG_INF(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#else
#include <Logging.h>
#define BOOTSWITCH_LOG_ERR(tag, fmt, ...) LOG_ERR(tag, fmt, ##__VA_ARGS__)
#define BOOTSWITCH_LOG_INF(tag, fmt, ...) LOG_INF(tag, fmt, ##__VA_ARGS__)
#endif
