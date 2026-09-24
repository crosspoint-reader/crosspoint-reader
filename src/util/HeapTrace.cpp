#include "HeapTrace.h"

#ifdef CROSSPOINT_HEAP_TRACE

#include <Arduino.h>
#include <Logging.h>
#include <esp_attr.h>
#include <esp_heap_caps.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <sdkconfig.h>
#include <soc/soc.h>

#include <cstdlib>
#include <cstring>
#include <new>

#if !CONFIG_HEAP_USE_HOOKS
#error "CROSSPOINT_HEAP_TRACE requires CONFIG_HEAP_USE_HOOKS=y (build the heaptrace env)"
#endif
#if !CONFIG_ESP_SYSTEM_USE_FRAME_POINTER
#error "CROSSPOINT_HEAP_TRACE requires CONFIG_ESP_SYSTEM_USE_FRAME_POINTER=y (build the heaptrace env)"
#endif

#ifndef HEAP_TRACE_RING_BYTES
#define HEAP_TRACE_RING_BYTES 8192
#endif
#ifndef HEAP_TRACE_STACK_DEPTH
#define HEAP_TRACE_STACK_DEPTH 6
#endif

// Stream format (little-endian records, packed into base64 `@HT <seq> <ms> <b64> <fletcher16>` lines):
//   'A' ptr size task n caller[n]  allocation, callers innermost first
//   'F' ptr                        free
//   'D' dropped                    running total of records lost to a full ring
//   'S' snapId version depth       snapshot start
//   'H' start end                  heap region in the snapshot
//   'P' snapId heapStart from      pass marker, pushed through the ring under the heap lock
//   'B' ptr size|used<<31          walked block (block pointer, not the user pointer)
//   'Q' end count                  pass end; the pass covered [from, end)
//   'N' task len name[len]         task name
//   'E' snapId free largest minFree allocatedBlocks freeBlocks
//   'X' snapId                     snapshot aborted
namespace {

constexpr uint32_t RING_BYTES = HEAP_TRACE_RING_BYTES;
static_assert((RING_BYTES & (RING_BYTES - 1)) == 0, "HEAP_TRACE_RING_BYTES must be a power of two");
constexpr int STACK_DEPTH = HEAP_TRACE_STACK_DEPTH;
// Allocator frames (IRAM) precede the first recorded caller.
constexpr int MAX_UNWIND_STEPS = STACK_DEPTH + 8;
constexpr uint8_t FORMAT_VERSION = 1;

constexpr size_t ALLOC_HEADER_BYTES = 14;
constexpr size_t MAX_RECORD_BYTES = ALLOC_HEADER_BYTES + 4 * STACK_DEPTH;
constexpr size_t PASS_MARKER_BYTES = 13;
// 150 raw bytes encode to a 200-character payload; lines stay under the CDC ring item size.
constexpr size_t LINE_RAW_BYTES = 150;
constexpr size_t PASS_CAPACITY = 96;
constexpr size_t MAX_TASKS = 24;
constexpr uint32_t AUTO_SNAPSHOT_INTERVAL_MS = 2000;
constexpr uint32_t DISCONNECT_DISCARD_MS = 1000;

// Mutable statics live in internal DRAM (.bss/.data), which the IRAM hooks can
// read even while the flash cache is disabled.
uint8_t ring[RING_BYTES];
uint32_t ringHead = 0;  // Producer position; both positions increase monotonically.
uint32_t ringTail = 0;  // Consumer position.
uint32_t droppedRecords = 0;
volatile bool recording = false;
portMUX_TYPE ringMux = portMUX_INITIALIZER_UNLOCKED;

StaticSemaphore_t outputMutexStorage;
SemaphoreHandle_t outputMutex = nullptr;
volatile bool snapshotRequested = true;
bool needSnapshot = true;
// Why resynchronizing snapshots were scheduled, reported by `CMD:HEAPTRACE`.
uint32_t resyncDisconnect = 0;
uint32_t resyncShortWrite = 0;
uint32_t resyncCorruptRing = 0;
uint32_t resyncDrops = 0;

uint8_t lineRaw[LINE_RAW_BYTES];
size_t lineRawLen = 0;
char lineText[40 + (LINE_RAW_BYTES + 2) / 3 * 4];
uint32_t lineSeq = 0;

struct Block {
  uint32_t ptr;
  uint32_t sizeUsed;
};
Block passBlocks[PASS_CAPACITY];
TaskStatus_t taskStatus[MAX_TASKS];

IRAM_ATTR inline void put32(uint8_t* out, uint32_t value) {
  out[0] = value;
  out[1] = value >> 8;
  out[2] = value >> 16;
  out[3] = value >> 24;
}

IRAM_ATTR bool pushRecord(const uint8_t* record, size_t len, uint32_t* endPosition = nullptr) {
  bool stored;
  portENTER_CRITICAL_SAFE(&ringMux);
  stored = RING_BYTES - (ringHead - ringTail) >= len;
  if (stored) {
    for (size_t i = 0; i < len; ++i) ring[(ringHead + i) & (RING_BYTES - 1)] = record[i];
    ringHead += len;
    if (endPosition) *endPosition = ringHead;
  } else {
    ++droppedRecords;
  }
  portEXIT_CRITICAL_SAFE(&ringMux);
  return stored;
}

// Walks the RISC-V frame-pointer chain (saved ra at fp[-1], caller fp at fp[-2]).
// Leading IRAM frames are the hook and allocator internals. The chain ends at
// the first frame that is not code, which is where code built without frame
// pointers (ROM, prebuilt libraries) breaks it.
IRAM_ATTR __attribute__((noinline)) uint8_t captureCallers(uint8_t* out) {
  auto* fp = static_cast<uint32_t*>(__builtin_frame_address(0));
  uint8_t count = 0;
  bool leading = true;
  for (int step = 0; step < MAX_UNWIND_STEPS && count < STACK_DEPTH; ++step) {
    const uint32_t frame = reinterpret_cast<uint32_t>(fp);
    if ((frame & 3) != 0 || frame < SOC_DRAM_LOW + 8 || frame > SOC_DRAM_HIGH) break;
    const uint32_t pc = fp[-1];
    const uint32_t next = fp[-2];
    const bool inIram = pc >= SOC_IRAM_LOW && pc < SOC_IRAM_HIGH;
    const bool inFlash = pc >= SOC_IROM_LOW && pc < SOC_IROM_HIGH;
    if (!inIram && !inFlash) break;
    if (!leading || !inIram) {
      leading = false;
      put32(out + 4 * count, pc);
      ++count;
    }
    if (next <= frame) break;
    fp = reinterpret_cast<uint32_t*>(next);
  }
  return count;
}

bool outputReady() { return static_cast<bool>(logSerial); }

void flushLine() {
  if (lineRawLen == 0) return;
  static constexpr char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  uint32_t sum1 = 0;
  uint32_t sum2 = 0;
  for (size_t i = 0; i < lineRawLen; ++i) {
    sum1 = (sum1 + lineRaw[i]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  int len = snprintf(lineText, sizeof(lineText), "@HT %lu %lu ", static_cast<unsigned long>(lineSeq),
                     static_cast<unsigned long>(millis()));
  for (size_t i = 0; i < lineRawLen; i += 3) {
    const uint32_t chunk =
        (lineRaw[i] << 16) | (i + 1 < lineRawLen ? lineRaw[i + 1] << 8 : 0) | (i + 2 < lineRawLen ? lineRaw[i + 2] : 0);
    lineText[len++] = B64[(chunk >> 18) & 63];
    lineText[len++] = B64[(chunk >> 12) & 63];
    lineText[len++] = i + 1 < lineRawLen ? B64[(chunk >> 6) & 63] : '=';
    lineText[len++] = i + 2 < lineRawLen ? B64[chunk & 63] : '=';
  }
  len += snprintf(lineText + len, sizeof(lineText) - len, " %04lx\n", static_cast<unsigned long>((sum2 << 8) | sum1));
  lineRawLen = 0;
  ++lineSeq;

  xSemaphoreTake(outputMutex, portMAX_DELAY);
  const size_t written = logSerial.write(reinterpret_cast<const uint8_t*>(lineText), len);
  if (written != static_cast<size_t>(len)) {
    // A short write leaves a partial line; terminate it and resynchronize.
    logSerial.write(reinterpret_cast<const uint8_t*>("\n"), 1);
    needSnapshot = true;
    ++resyncShortWrite;
  }
  xSemaphoreGive(outputMutex);
}

void emit(const uint8_t* record, size_t len) {
  if (lineRawLen + len > LINE_RAW_BYTES) flushLine();
  memcpy(lineRaw + lineRawLen, record, len);
  lineRawLen += len;
}

size_t recordLength(uint32_t position) {
  switch (ring[position & (RING_BYTES - 1)]) {
    case 'A':
      return ALLOC_HEADER_BYTES + 4 * ring[(position + 13) & (RING_BYTES - 1)];
    case 'F':
      return 5;
    case 'P':
      return PASS_MARKER_BYTES;
    default:
      return 0;
  }
}

// Emits ring records up to position `*until`, or everything queued when null.
void drainRing(const uint32_t* until = nullptr) {
  uint8_t record[MAX_RECORD_BYTES];
  for (;;) {
    portENTER_CRITICAL(&ringMux);
    const uint32_t head = ringHead;
    portEXIT_CRITICAL(&ringMux);
    const uint32_t tail = ringTail;
    if (tail == head || (until && static_cast<int32_t>(tail - *until) >= 0)) return;
    const size_t len = recordLength(tail);
    if (len == 0 || len > MAX_RECORD_BYTES || len > head - tail) {
      // Unreachable unless the ring is corrupt; discard it and resynchronize.
      portENTER_CRITICAL(&ringMux);
      ringTail = ringHead;
      portEXIT_CRITICAL(&ringMux);
      needSnapshot = true;
      ++resyncCorruptRing;
      return;
    }
    for (size_t i = 0; i < len; ++i) record[i] = ring[(tail + i) & (RING_BYTES - 1)];
    portENTER_CRITICAL(&ringMux);
    ringTail = tail + len;
    portEXIT_CRITICAL(&ringMux);
    emit(record, len);
  }
}

void discardRing() {
  portENTER_CRITICAL(&ringMux);
  ringTail = ringHead;
  portEXIT_CRITICAL(&ringMux);
  lineRawLen = 0;
}

struct HeapList {
  intptr_t start[8];
  intptr_t end[8];
  uint8_t count;
};

bool heapListWalker(walker_heap_into_t heap, walker_block_info_t, void* user) {
  auto* list = static_cast<HeapList*>(user);
  for (uint8_t i = 0; i < list->count; ++i) {
    if (list->start[i] == heap.start) return false;
  }
  if (list->count < 8) {
    list->start[list->count] = heap.start;
    list->end[list->count] = heap.end;
    ++list->count;
  }
  return false;
}

struct PassState {
  intptr_t heapStart;
  uint32_t snapId;
  uint32_t from;
  uint32_t end;
  uint32_t markerEnd;
  uint16_t count;
  bool markerPushed;
  bool markerFailed;
};

// Runs under the heap lock, so the pass marker orders this walk exactly
// against the hook records of the same heap.
bool passWalker(walker_heap_into_t heap, walker_block_info_t block, void* user) {
  auto* pass = static_cast<PassState*>(user);
  if (heap.start != pass->heapStart) return false;
  if (!pass->markerPushed) {
    pass->markerPushed = true;
    uint8_t marker[PASS_MARKER_BYTES];
    marker[0] = 'P';
    put32(marker + 1, pass->snapId);
    put32(marker + 5, static_cast<uint32_t>(pass->heapStart));
    put32(marker + 9, pass->from);
    if (!pushRecord(marker, sizeof(marker), &pass->markerEnd)) {
      pass->markerFailed = true;
      return false;
    }
  }
  const auto ptr = reinterpret_cast<uint32_t>(block.ptr);
  if (ptr < pass->from) return true;
  if (pass->count == PASS_CAPACITY) {
    pass->end = ptr;
    return false;
  }
  passBlocks[pass->count++] = {ptr, static_cast<uint32_t>(block.size) | (block.used ? 0x80000000u : 0)};
  return true;
}

void emitTaskNames() {
  const UBaseType_t count = uxTaskGetSystemState(taskStatus, MAX_TASKS, nullptr);
  for (UBaseType_t i = 0; i < count; ++i) {
    uint8_t record[6 + configMAX_TASK_NAME_LEN];
    const size_t nameLen = strnlen(taskStatus[i].pcTaskName, configMAX_TASK_NAME_LEN);
    record[0] = 'N';
    put32(record + 1, reinterpret_cast<uint32_t>(taskStatus[i].xHandle));
    record[5] = static_cast<uint8_t>(nameLen);
    memcpy(record + 6, taskStatus[i].pcTaskName, nameLen);
    emit(record, 6 + nameLen);
  }
}

void snapshot() {
  static uint32_t snapId = 0;
  ++snapId;
  uint8_t record[25];

  record[0] = 'S';
  put32(record + 1, snapId);
  record[5] = FORMAT_VERSION;
  record[6] = STACK_DEPTH;
  emit(record, 7);
  emitTaskNames();

  HeapList heaps{};
  heap_caps_walk(MALLOC_CAP_INTERNAL, heapListWalker, &heaps);
  for (uint8_t h = 0; h < heaps.count; ++h) {
    record[0] = 'H';
    put32(record + 1, heaps.start[h]);
    put32(record + 5, heaps.end[h]);
    emit(record, 9);

    uint32_t from = heaps.start[h];
    while (from < static_cast<uint32_t>(heaps.end[h])) {
      // Leave room for the pass marker behind the records already queued.
      drainRing();
      PassState pass{heaps.start[h], snapId, from, static_cast<uint32_t>(heaps.end[h]), 0, 0, false, false};
      heap_caps_walk(MALLOC_CAP_INTERNAL, passWalker, &pass);
      if (!pass.markerPushed || pass.markerFailed) {
        record[0] = 'X';
        put32(record + 1, snapId);
        emit(record, 5);
        flushLine();
        needSnapshot = true;
        return;
      }
      drainRing(&pass.markerEnd);
      for (uint16_t i = 0; i < pass.count; ++i) {
        record[0] = 'B';
        put32(record + 1, passBlocks[i].ptr);
        put32(record + 5, passBlocks[i].sizeUsed);
        emit(record, 9);
      }
      record[0] = 'Q';
      put32(record + 1, pass.end);
      put32(record + 5, pass.count);
      emit(record, 9);
      from = pass.end;
    }
  }

  multi_heap_info_t info;
  heap_caps_get_info(&info, MALLOC_CAP_INTERNAL);
  record[0] = 'E';
  put32(record + 1, snapId);
  put32(record + 5, info.total_free_bytes);
  put32(record + 9, info.largest_free_block);
  put32(record + 13, info.minimum_free_bytes);
  put32(record + 17, info.allocated_blocks);
  put32(record + 21, info.free_blocks);
  emit(record, 25);
  flushLine();
  needSnapshot = false;
}

void drainTask(void*) {
  uint32_t reportedDrops = 0;
  uint32_t lastSnapshotMs = 0;
  uint32_t notReadySinceMs = 0;
  bool notReady = false;
  for (;;) {
    // The CDC connected flag can drop briefly while the host is idle, so only
    // a sustained loss discards the queued records.
    if (!outputReady()) {
      const uint32_t now = millis();
      if (!notReady) {
        notReady = true;
        notReadySinceMs = now;
      } else if (now - notReadySinceMs >= DISCONNECT_DISCARD_MS) {
        portENTER_CRITICAL(&ringMux);
        const bool queued = ringHead != ringTail;
        portEXIT_CRITICAL(&ringMux);
        if (queued) discardRing();
        if (queued && !needSnapshot) ++resyncDisconnect;
        needSnapshot = needSnapshot || queued;
      }
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    notReady = false;

    portENTER_CRITICAL(&ringMux);
    const uint32_t drops = droppedRecords;
    portEXIT_CRITICAL(&ringMux);
    if (drops != reportedDrops) {
      uint8_t record[5];
      record[0] = 'D';
      put32(record + 1, drops);
      emit(record, 5);
      reportedDrops = drops;
      needSnapshot = true;
      ++resyncDrops;
    }

    drainRing();
    const uint32_t now = millis();
    if (snapshotRequested || (needSnapshot && now - lastSnapshotMs >= AUTO_SNAPSHOT_INTERVAL_MS)) {
      snapshotRequested = false;
      lastSnapshotMs = now;
      snapshot();
    }
    flushLine();

    portENTER_CRITICAL(&ringMux);
    const bool empty = ringHead == ringTail;
    portEXIT_CRITICAL(&ringMux);
    vTaskDelay(pdMS_TO_TICKS(empty ? 5 : 1));
  }
}

}  // namespace

// ESP-IDF calls these weak hooks after each successful allocation or free,
// outside the heap lock (components/heap/heap_caps_base.c).
extern "C" IRAM_ATTR void esp_heap_trace_alloc_hook(void* ptr, size_t size, uint32_t) {
  if (!recording) return;
  uint8_t record[MAX_RECORD_BYTES];
  record[0] = 'A';
  put32(record + 1, reinterpret_cast<uint32_t>(ptr));
  put32(record + 5, size);
  put32(record + 9, reinterpret_cast<uint32_t>(xTaskGetCurrentTaskHandle()));
  record[13] = captureCallers(record + ALLOC_HEADER_BYTES);
  pushRecord(record, ALLOC_HEADER_BYTES + 4 * record[13]);
}

extern "C" IRAM_ATTR void esp_heap_trace_free_hook(void* ptr) {
  if (!recording) return;
  uint8_t record[5];
  record[0] = 'F';
  put32(record + 1, reinterpret_cast<uint32_t>(ptr));
  pushRecord(record, sizeof(record));
}

// libstdc++'s operator new is built without frame pointers, which hides the
// allocating function from the unwinder. These replacements keep the chain.
void* operator new(size_t size) {
  void* ptr = malloc(size ? size : 1);
  if (!ptr) abort();  // Matches the -fno-exceptions behavior of the library version.
  return ptr;
}
void* operator new[](size_t size) {
  void* ptr = malloc(size ? size : 1);
  if (!ptr) abort();
  return ptr;
}
void* operator new(size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }
void* operator new[](size_t size, const std::nothrow_t&) noexcept { return malloc(size ? size : 1); }

namespace HeapTrace {

void begin() {
  if (outputMutex) return;
  outputMutex = xSemaphoreCreateMutexStatic(&outputMutexStorage);
  // Same priority as the loop and render tasks, so time slicing drains the
  // ring during long renders without preempting them. The 3 KiB stack is
  // allocated once for the life of the trace.
  if (xTaskCreate(drainTask, "HeapTrace", 3072, nullptr, 1, nullptr) != pdPASS) {
    LOG_ERR("HTRACE", "Failed to create drain task; heap trace disabled");
    return;
  }
  recording = true;
  LOG_INF("HTRACE", "Heap trace on: ring=%u depth=%d", static_cast<unsigned>(RING_BYTES), STACK_DEPTH);
}

void requestSnapshot() { snapshotRequested = true; }

void handleCommand(const char* arg) {
  if (arg && strcmp(arg, "SNAP") == 0) {
    requestSnapshot();
    return;
  }
  portENTER_CRITICAL(&ringMux);
  const uint32_t drops = droppedRecords;
  const uint32_t used = ringHead - ringTail;
  portEXIT_CRITICAL(&ringMux);
  LOG_INF("HTRACE", "recording=%d dropped=%lu ring_used=%lu lines=%lu resync: drops=%lu short=%lu disc=%lu corrupt=%lu",
          recording ? 1 : 0, static_cast<unsigned long>(drops), static_cast<unsigned long>(used),
          static_cast<unsigned long>(lineSeq), static_cast<unsigned long>(resyncDrops),
          static_cast<unsigned long>(resyncShortWrite), static_cast<unsigned long>(resyncDisconnect),
          static_cast<unsigned long>(resyncCorruptRing));
}

void lockOutput() {
  if (outputMutex) xSemaphoreTake(outputMutex, portMAX_DELAY);
}

void unlockOutput() {
  if (outputMutex) xSemaphoreGive(outputMutex);
}

}  // namespace HeapTrace

#endif
