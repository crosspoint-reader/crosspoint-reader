#pragma once
#include "DfuSession.h"
namespace usb_update {
constexpr uint16_t transferBlockSize = 4096;
#ifdef USB_UPDATER_DIAGNOSTICS
struct Trace {
  uint32_t start, finish, serviceEnd, firstPid, secondPid, firstError, secondError, firstCount, secondCount, response,
      epBefore, epAfter, parseEnd, commitEnd, dcdEnd, taskEnd;
};
constexpr unsigned traceCapacity = 96;
#endif
struct Report {
  uint32_t error = 0, restored = 0, loops = 0, valid = 0, invalid = 0, commits = 0, mismatch = 0, resets = 0, sof = 0,
           mounted = 0, address = 0, received = 0, total = 0, completed = 0, hostConfirmed = 0, cancelled = 0,
           sessionError = 0, resetsDuringDownload = 0, lastResetBytes = 0, maxServiceUs = 0, exitReason = 0,
           workspaceBytes = 0;
#ifdef USB_UPDATER_DIAGNOSTICS
  uint32_t traceCount = 0;
  Trace trace[traceCapacity]{};
#endif
};
Report run(Session& session, bool (*cancelled)() = nullptr, uint32_t durationMs = 3600000, uint32_t hostIdleMs = 0,
           bool background = false, void (*linkState)(bool) = nullptr);
// Flash operations invalidate instruction cache. The storage sink brackets
// those operations, then reloads/locks the timing-sensitive packet engine.
void suspendCache();
bool resumeCache();
}  // namespace usb_update
