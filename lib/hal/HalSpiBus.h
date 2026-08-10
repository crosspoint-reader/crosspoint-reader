#pragma once

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

// Serializes access to the SPI bus shared by the e-ink panel and the SD card.
// HalStorage already serializes SD access with storageMutex, but the display
// and storage both drive the same physical SPI bus; without a bus-level lock a
// display refresh and a concurrent SD transfer can interleave and confuse
// SdSpiCard's unsynchronised m_spiActive state machine (see CLAUDE.md / SdFat
// issue #518), manifesting as corrupted reads or a FreeRTOS panic.
//
// The mutex is recursive so a storage path that already holds the bus lock (via
// HalStorage::StorageLock, which takes it as its outermost member) can re-enter
// without self-deadlock. Lock ordering is SPI-outer, storage-inner: every
// StorageLock acquires the SPI lock before storageMutex, and display code takes
// only the SPI lock, so the global order is consistent and deadlock-free.
class HalSpiBus {
 public:
  class Lock {
   public:
    Lock();
    ~Lock();
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;

   private:
    bool acquired = false;
  };

  static HalSpiBus& getInstance();

 private:
  HalSpiBus();

  SemaphoreHandle_t mutex = nullptr;

  friend class Lock;
};
