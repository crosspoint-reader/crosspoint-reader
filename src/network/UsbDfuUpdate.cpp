#include <sdkconfig.h>
#if CONFIG_IDF_TARGET_ESP32C3 || CROSSPOINT_NATIVE_DFU
#include <Arduino.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>
#include <esp_ota_ops.h>

#include <algorithm>
#include <cstring>

#include "UsbDfuUpdate.h"
#include "UsbFlashWriter.h"
#include "UsbUpdater.h"

namespace usb_dfu {
namespace {
constexpr const char* BUILD = CROSSPOINT_VERSION;
class FlashSink final : public usb_update::Sink {
  // Allocated only after the reader activity has released its book/cache.
  // Heap allocation avoids permanently reserving scarce C3 reading memory.
  static constexpr size_t CAPACITY = 16384;
  std::unique_ptr<uint8_t[]> buffer;
  usb_update::FlashWriter writer;
  size_t buffered = 0;
  uint32_t declared = 0;
  Prepare prepare;
  uint32_t maxUiMs = 0, totalUiMs = 0, uiPaints = 0;
  Progress progress;
  void* context;

  void show(Stage stage, size_t value, size_t total) {
    const uint32_t started = millis();
    if (progress) progress(stage, value, total, context);
    const uint32_t elapsed = uint32_t(millis() - started);
    maxUiMs = std::max(maxUiMs, elapsed);
    totalUiMs += elapsed;
    if (progress) ++uiPaints;
  }
  bool flush() {
    if (!buffered) return true;
    usb_update::suspendCache();
    const bool ok = writer.append(buffer.get(), buffered);
    const bool resumed = usb_update::resumeCache();
    if (!ok || !resumed) return false;
    buffered = 0;
    return true;
  }

 public:
  explicit FlashSink(Progress cb, void* ctx, Prepare prep = nullptr) : prepare(prep), progress(cb), context(ctx) {}
  bool begin(uint32_t size, const uint8_t sha[32]) override {
    abort();
    maxUiMs = totalUiMs = uiPaints = 0;
    declared = size;
    if (prepare && !prepare(context)) return false;
    buffer = makeUniqueNoThrow<uint8_t[]>(CAPACITY);
    if (!buffer) {
      LOG_ERR("DFU", "Transfer buffer allocation failed: free=%u max=%u", ESP.getFreeHeap(), ESP.getMaxAllocHeap());
      return false;
    }
    if (!writer.begin(esp_ota_get_next_update_partition(nullptr), size, sha)) return false;
    declared = size;
    show(Stage::Receiving, 0, declared);
    return true;
  }
  bool append(const uint8_t* data, size_t size) override {
    if (!buffer || size > CAPACITY - buffered) return false;
    memcpy(buffer.get() + buffered, data, size);
    buffered += size;
    return buffered < CAPACITY || flush();
  }
  uint32_t pollTimeout(bool manifest, uint32_t done, uint32_t total, uint16_t block) const override {
    if (manifest) return 10000;
    if (!total) return prepare ? 8000 : 1500;
    const size_t incoming = std::min<uint32_t>(block, total - done);
    if (buffered + incoming < CAPACITY) return 2;
    uint32_t wait = writer.writeCount ? writer.maxWriteMs * 5 / 4 + 10 : 150;
    if (writer.needsErase(CAPACITY)) wait += writer.eraseCount ? writer.maxEraseMs * 5 / 4 + 20 : 500;
    return wait;
  }
  usb_update::Error finish() override {
    if (!flush()) return usb_update::Error::Write;
    buffer.reset();  // Reuse this memory for the readback validator.
    show(Stage::Verifying, writer.bytes(), declared);
    usb_update::suspendCache();
    const bool ok = writer.finish();
    usb_update::resumeCache();
    return ok ? usb_update::Error::None : usb_update::Error::Verify;
  }
  void abort() override {
    buffered = 0;
    buffer.reset();
    writer.abort();
    if (declared && progress) progress(Stage::Waiting, 0, 0, context);
    declared = 0;
  }
  uint32_t uiTimeMs() const { return totalUiMs; }
  uint32_t uiCount() const { return uiPaints; }
  uint32_t uiMaxMs() const { return maxUiMs; }
  const usb_update::FlashWriter& timings() const { return writer; }
};
}  // namespace

bool run(Progress progress, void* context, Prepare prepare, bool (*cancelled)(), bool background,
         void (*linkState)(bool)) {
  const auto* running = esp_ota_get_running_partition();
  const auto* destination = esp_ota_get_next_update_partition(nullptr);
  if (!running || !destination || running->address == destination->address) return false;
  FlashSink sink(progress, context, prepare);
  usb_update::Session session(sink, destination->size, 65536, 10000, 1000, usb_update::transferBlockSize);
  // No connected-host idle timeout. A stopped SOF stream means unplug/suspend.
  const auto report = usb_update::run(session, cancelled, 0, 3000, background, linkState);
  if (background) {
    HalFile connectionLog;
    if (Storage.openFileForWrite("DFU", "/usb-connection-result.json", connectionLog)) {
      char text[240];
      snprintf(text, sizeof(text),
               "{\"build\":\"%s\",\"mounted\":%u,\"address\":%u,\"sof\":%u,\"resets\":%u,"
               "\"error\":%u,\"mismatch\":%u,\"exit\":%u,\"received\":%u,\"max_service_us\":%u,\"workspace_bytes\":%u}",
               BUILD, report.mounted, report.address, report.sof, report.resets, report.error, report.mismatch,
               report.exitReason, report.received, report.maxServiceUs, report.workspaceBytes);
      connectionLog.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
    }
  }
  const auto& timing = sink.timings();
  HalFile log;
  if (report.total && Storage.openFileForWrite("DFU", "/usb-update-result.json", log)) {
    char text[256];
    snprintf(text, sizeof(text),
             "{\"build\":\"%s\",\"completed\":%u,\"received\":%u,\"total\":%u,"
             "\"error\":%u,\"session_error\":%u,\"erase_ms\":%u,\"write_ms\":%u,\"verify_ms\":%u",
             BUILD, report.completed, report.received, report.total, report.error, report.sessionError, timing.eraseMs,
             timing.writeMs, timing.verifyMs);
    log.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
    snprintf(text, sizeof(text), ",\"ui_ms\":%u,\"ui_callbacks\":%u,\"ui_max_ms\":%u}", sink.uiTimeMs(), sink.uiCount(),
             sink.uiMaxMs());
    log.write(reinterpret_cast<const uint8_t*>(text), strlen(text));
  }
  LOG_INF("DFU", "complete=%u received=%u error=%u session=%u", report.completed, report.received, report.error,
          report.sessionError);
  return session.complete();
}
}  // namespace usb_dfu
#endif
