#pragma once

// Host-test stub for FreeRTOS's semaphore handle type. The firmware only needs
// the type to declare HalStorage::storageMutex; no semaphore API is exercised.

using SemaphoreHandle_t = void*;
