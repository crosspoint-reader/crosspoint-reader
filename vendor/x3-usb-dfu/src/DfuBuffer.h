#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
// Bind before TinyUSB init; unbind only after TinyUSB deinit. Passing NULL,0
// clears the pointer. The caller owns the aligned storage for the entire run.
bool su_dfu_bind_buffer(uint8_t* buffer, size_t size);
#ifdef __cplusplus
}
#endif
