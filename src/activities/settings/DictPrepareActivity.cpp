#include "DictPrepareActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>

#include <memory>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "Memory.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/DictPrepareTask.h"
#include "util/Dictionary.h"
#include "util/DictionaryActivityUtils.h"

// ---------------------------------------------------------------------------
// uzlib read callback context
//
// InflateReader MUST be the first member so that the uzlib_uncomp* pointer
// received in the callback can be cast to DecompCtx* (reader.decomp is at
// offset 0 within InflateReader, which is at offset 0 within DecompCtx).
// HalFile is stored as a pointer (non-owning) to keep DecompCtx standard-layout.
// ---------------------------------------------------------------------------
struct PrepDecompCtx {
  InflateReader reader;  // MUST be first
  HalFile* file;         // non-owning; caller owns the HalFile instance
  uint8_t chunkBuf[512];
};

static int dictPrepReadCallback(struct uzlib_uncomp* u) {
  PrepDecompCtx* ctx = reinterpret_cast<PrepDecompCtx*>(u);
  int n = ctx->file->read(ctx->chunkBuf, sizeof(ctx->chunkBuf));
  if (n <= 0) return -1;
  u->source = reinterpret_cast<const unsigned char*>(ctx->chunkBuf + 1);
  u->source_limit = reinterpret_cast<const unsigned char*>(ctx->chunkBuf + n);
  return static_cast<int>(ctx->chunkBuf[0]);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* DictPrepareActivity::stepLabel(StepType type) {
  switch (type) {
    case StepType::EXTRACT_DICT:
      return tr(STR_DICT_STEP_EXTRACT_DICT);
    case StepType::EXTRACT_SYN:
      return tr(STR_DICT_STEP_EXTRACT_SYN);

    case StepType::GEN_FPI:
      return tr(STR_DICT_STEP_GEN_FPI);
    case StepType::GEN_SYN_FPI:
      return tr(STR_DICT_STEP_GEN_SYN_FPI);
  }
  return "";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

DictPrepareActivity::DictPrepareActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string folderPath,
                                         bool forceRebuild)
    : Activity("DictPrepare", renderer, mappedInput),
      folderPath(std::move(folderPath)),
      forceRebuild(forceRebuild),
      steps{} {}

DictPrepareActivity::~DictPrepareActivity() = default;

bool DictPrepareActivity::updateProgressIfPercentChanged(Step& step, size_t progress, int& lastPercent) {
  if (step.total == 0) return false;
  const int percent = static_cast<int>((static_cast<uint64_t>(progress) * 100) / step.total);
  if (percent == lastPercent) return false;
  lastPercent = percent;
  step.progress = progress;
  requestUpdate(true);
  return true;
}

void DictPrepareActivity::onEnter() {
  Activity::onEnter();
  state = State::CONFIRM;
  prepareDone = false;
  prepareSucceeded = false;
  cancelRequested = false;
  task.reset();
  currentStep = 0;

  // Extract the folder name from folderPath for display.
  // folderPath is e.g. "/dictionary/pr-857/dictionary"; we want "pr-857"
  // (the parent directory's last component, one level up from the base name).
  {
    const char* path = folderPath.c_str();
    const char* lastSlash = strrchr(path, '/');
    const char* end = lastSlash ? lastSlash : path + strlen(path);
    const char* prevSlash = nullptr;
    for (const char* p = path; p < end; p++) {
      if (*p == '/') prevSlash = p;
    }
    const char* nameStart = prevSlash ? prevSlash + 1 : path;
    snprintf(dictName, sizeof(dictName), "%.*s", (int)(end - nameStart), nameStart);
  }

  detectSteps();

  // No preparation needed — signal success immediately.
  if (stepCount == 0) {
    finish();
    return;
  }

  requestUpdate();
}

void DictPrepareActivity::detectSteps() {
  stepCount = 0;

  DictPaths dp(folderPath);
  const bool dictExists = Storage.exists(dp.dict().c_str());
  const bool dzExists = Storage.exists(dp.dictDz().c_str());
  const bool idxExists = Storage.exists(dp.idx().c_str());

  if (!dictExists && dzExists) steps[stepCount++].type = StepType::EXTRACT_DICT;
  if (!synExists && synDzExists) steps[stepCount++].type = StepType::EXTRACT_SYN;
  const bool synWillExist = synExists || synDzExists;

  // .idx.fpi / .syn.fpi supersede .idx.oft/.idx.oft.cspt and .syn.oft/.syn.oft.cspt —
  // neither .oft nor .cspt is generated anymore. Each .fpi's per-group ordinal field
  // also covers Dictionary::wordAtOrdinal (resolveAltForm) / findSimilar's
  // ordinal/neighbourhood access, which used to require .oft.
  const bool idxFpiExists = Storage.exists(dp.idxFpi().c_str());
  if (idxExists && !idxFpiExists) steps[stepCount++].type = StepType::GEN_FPI;

  const bool synFpiExists = Storage.exists(dp.synFpi().c_str());
  if (synWillExist && !synFpiExists) steps[stepCount++].type = StepType::GEN_SYN_FPI;
}

void DictPrepareActivity::onExit() {
  if (task) {
    task->stop();
    task->wait();
    task.reset();
  }
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// Input / state machine
// ---------------------------------------------------------------------------

void DictPrepareActivity::loop() {
  if (state == State::CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      DictUtils::cancelAndFinish(*this);
      return;
    }
    if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
      state = State::PROCESSING;
      prepareDone = false;
      prepareSucceeded = false;
      currentStep = 0;
      for (int i = 0; i < stepCount; i++) {
        steps[i].status = StepStatus::PENDING;
        steps[i].progress = 0;
        steps[i].total = 0;
      }
      requestUpdateAndWait();
      task = makeUniqueNoThrow<DictPrepareTask>(*this);
      if (!task) {
        LOG_ERR("DICT_PREP", "OOM: DictPrepareTask");
        state = State::FAILED;
        requestUpdate();
        return;
      }
      task->start("DictPrep", 4096, 1);
      return;
    }
    return;
  }

  if (state == State::PROCESSING) {
    if (prepareDone) {
      state = cancelRequested ? State::CANCELLED : (prepareSucceeded ? State::SUCCESS : State::FAILED);
      requestUpdate();
    } else if (!cancelRequested && mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      cancelRequested = true;
      requestUpdate();  // re-render immediately to remove the Cancel button hint
    }
    return;
  }

  if (state == State::SUCCESS || state == State::FAILED || state == State::CANCELLED) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      if (state == State::FAILED || state == State::CANCELLED) {
        ActivityResult r;
        r.isCancelled = true;
        setResult(std::move(r));
      }
      // SUCCESS: default result (isCancelled=false) signals caller to apply selection.
      finish();
    }
    return;
  }
}

// ---------------------------------------------------------------------------
// Step execution (runs on FreeRTOS task)
// ---------------------------------------------------------------------------

void DictPrepareActivity::runSteps() {
  DictPaths dp(folderPath);

  for (int i = 0; i < stepCount; i++) {
    if (cancelRequested) break;

    currentStep = i;
    steps[i].status = StepStatus::IN_PROGRESS;
    requestUpdate(true);

    bool ok = false;

    switch (steps[i].type) {
      case StepType::EXTRACT_DICT:
        ok = extractFile(dp.dictDz().c_str(), dp.dict().c_str(), steps[i]);
        if (!ok) Storage.remove(dp.dict().c_str());
        break;

      case StepType::EXTRACT_SYN:
        ok = extractFile(dp.synDz().c_str(), dp.syn().c_str(), steps[i]);
        if (!ok) Storage.remove(dp.syn().c_str());
        break;

      case StepType::GEN_FPI:
        ok = generateFpi(dp.idx().c_str(), dp.idxFpi().c_str(), 8, steps[i]);
        if (!ok) Storage.remove(dp.idxFpi().c_str());
        break;

      case StepType::GEN_SYN_FPI:
        ok = generateFpi(dp.syn().c_str(), dp.synFpi().c_str(), 4, steps[i]);
        if (!ok) Storage.remove(dp.synFpi().c_str());
        break;
    }

    steps[i].status = ok ? StepStatus::COMPLETE : StepStatus::FAILED;
    requestUpdate(true);

    if (!ok) {
      LOG_ERR("DICT_PREP", "Step %d failed, aborting preparation", i);
      prepareSucceeded = false;
      prepareDone = true;
      requestUpdate(true);
      return;
    }
  }

  // Reached here either all steps succeeded or cancelRequested broke the loop.
  prepareSucceeded = !cancelRequested;
  prepareDone = true;
  requestUpdate(true);
}

// ---------------------------------------------------------------------------
// File extraction (gzip decompression)
// ---------------------------------------------------------------------------

bool DictPrepareActivity::extractFile(const char* dzPath, const char* outPath, Step& step) {
  HalFile inputFile;
  auto ctx = makeUniqueNoThrow<PrepDecompCtx>();
  if (!ctx) {
    LOG_ERR("DICT_PREP", "OOM: PrepDecompCtx");
    return false;
  }
  ctx->file = &inputFile;

  constexpr size_t OUT_BUF_SIZE = 4096;
  auto outBuf = makeUniqueNoThrow<uint8_t[]>(OUT_BUF_SIZE);
  if (!outBuf) {
    LOG_ERR("DICT_PREP", "OOM: decompression buffer");
    return false;
  }

  auto fail = [&] {
    inputFile.close();
    step.progress = 0;
  };

  if (!Storage.openFileForRead("DICT_PREP", dzPath, inputFile)) {
    LOG_ERR("DICT_PREP", "Failed to open: %s", dzPath);
    fail();
    return false;
  }

  step.total = inputFile.fileSize();
  step.progress = 0;

  // Validate gzip magic bytes
  uint8_t magic[2];
  if (inputFile.read(magic, 2) != 2 || magic[0] != 0x1F || magic[1] != 0x8B) {
    LOG_ERR("DICT_PREP", "Not a gzip file: %s", dzPath);
    fail();
    return false;
  }
  inputFile.seekSet(0);

  if (!ctx->reader.init(true)) {
    LOG_ERR("DICT_PREP", "InflateReader init failed");
    fail();
    return false;
  }
  ctx->reader.setReadCallback(dictPrepReadCallback);

  if (!ctx->reader.skipGzipHeader()) {
    LOG_ERR("DICT_PREP", "Invalid gzip header: %s", dzPath);
    fail();
    return false;
  }

  HalFile outFile;
  if (!Storage.openFileForWrite("DICT_PREP", outPath, outFile)) {
    LOG_ERR("DICT_PREP", "Failed to open for write: %s", outPath);
    fail();
    return false;
  }

  constexpr size_t PROGRESS_INTERVAL = 65536;
  size_t lastProgressPos = 0;
  int lastPercent = -1;
  InflateStatus status;
  bool writeError = false;

  do {
    size_t produced;
    status = ctx->reader.readAtMost(outBuf.get(), OUT_BUF_SIZE, &produced);

    if (produced > 0) {
      if (outFile.write(outBuf.get(), produced) != produced) {
        LOG_ERR("DICT_PREP", "Write error: %s", outPath);
        writeError = true;
        break;
      }
    }

    const size_t pos = inputFile.position();
    if (pos - lastProgressPos >= PROGRESS_INTERVAL) {
      lastProgressPos = pos;
      updateProgressIfPercentChanged(step, static_cast<uint32_t>(pos), lastPercent);
      vTaskDelay(1);
      if (cancelRequested) {
        writeError = true;
        break;
      }
    }
  } while (status == InflateStatus::Ok);

  outFile.close();
  inputFile.close();

  if (writeError || status == InflateStatus::Error) {
    LOG_ERR("DICT_PREP", "Extraction failed: %s", outPath);
    return false;
  }

  step.progress = step.total.load();
  return true;
}

// ---------------------------------------------------------------------------
// .fpi (Fenced Prefix Index) generation — thin Step/cancellation wrapper around the
// shared single-pass implementation in Dictionary::generateFpi.
// ---------------------------------------------------------------------------

bool DictPrepareActivity::generateFpi(const char* srcPath, const char* fpiPath, uint8_t skipPerEntry, Step& step) {
  // Plain-function-pointer callback context (no std::function — see CLAUDE.md's
  // template/std::function bloat rule and the Callback{ctx,fn} pattern it prescribes).
  struct ProgressCtx {
    DictPrepareActivity* self;
    Step* step;
    int lastPercent;
  };
  ProgressCtx pctx{this, &step, -1};

  step.total = 0;
  step.progress = 0;

  return Dictionary::generateFpi(
      srcPath, fpiPath, skipPerEntry,
      [](void* ctxPtr, uint32_t bytesDone, uint32_t bytesTotal) {
        auto* c = static_cast<ProgressCtx*>(ctxPtr);
        c->step->total = bytesTotal;
        c->self->updateProgressIfPercentChanged(*c->step, bytesDone, c->lastPercent);
      },
      [](void* ctxPtr) -> bool {
        auto* c = static_cast<ProgressCtx*>(ctxPtr);
        vTaskDelay(1);
        return c->self->cancelRequested.load();
      },
      &pctx);
}

// ---------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------

void DictPrepareActivity::render(RenderLock&&) {
  renderer.clearScreen();

  const int pageWidth = renderer.getScreenWidth();
  const auto& metrics = UITheme::getInstance().getMetrics();

  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_DICT_PREPARE_TITLE));

  const int lineHeight = renderer.getLineHeight(UI_10_FONT_ID);
  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  constexpr int BAR_HEIGHT = 16;
  constexpr int STEP_SPACING = 6;
  constexpr int BAR_MARGIN = 40;

  if (state == State::CONFIRM) {
    int y = contentTop;

    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, dictName);
    y += lineHeight + STEP_SPACING;

    // List required steps
    for (int i = 0; i < stepCount; i++) {
      renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, stepLabel(steps[i].type));
      y += lineHeight + STEP_SPACING;
    }

    y += metrics.verticalSpacing;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_DICT_PREPARE_WARN_1));
    y += lineHeight + STEP_SPACING;
    renderer.drawCenteredText(UI_10_FONT_ID, y, tr(STR_DICT_PREPARE_WARN_2));

    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), tr(STR_CONFIRM), "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
    renderer.displayBuffer();
    return;
  }

  // PROCESSING / SUCCESS / FAILED / CANCELLED — show per-step status with always-visible indicators.
  // Status prefix column: 5 chars wide so step labels align.
  // Bold: current (IN_PROGRESS) and failed steps only. Completed steps use regular weight.
  int y = contentTop;

  renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, dictName);
  y += lineHeight + STEP_SPACING;

  for (int i = 0; i < stepCount; i++) {
    const auto& step = steps[i];
    const bool complete = step.status == StepStatus::COMPLETE;
    const bool failed = step.status == StepStatus::FAILED;
    const bool inProgress = step.status == StepStatus::IN_PROGRESS;

    const char* prefix = complete ? "[OK] " : (failed ? "[!!] " : (inProgress ? "[ > ] " : "[   ] "));
    const EpdFontFamily::Style style = (inProgress || failed) ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    char labelBuf[64];
    snprintf(labelBuf, sizeof(labelBuf), "%s%s", prefix, stepLabel(step.type));
    renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, y, labelBuf, true, style);
    y += lineHeight;

    if (inProgress && step.total > 0) {
      // Custom inline progress bar: percentage drawn to the right of the bar (not below).
      const int percent = static_cast<int>((static_cast<uint64_t>(step.progress) * 100) / step.total);
      char pctBuf[8];
      snprintf(pctBuf, sizeof(pctBuf), "%d%%", percent);
      const int pctWidth = renderer.getTextWidth(UI_10_FONT_ID, pctBuf);
      const int pctX = pageWidth - BAR_MARGIN - pctWidth;
      const int barRight = pctX - 4;
      const int barWidth = barRight - BAR_MARGIN;
      if (barWidth > 4) {
        renderer.drawRect(BAR_MARGIN, y, barWidth, BAR_HEIGHT);
        const int fillWidth = (barWidth - 4) * percent / 100;
        if (fillWidth > 0) renderer.fillRect(BAR_MARGIN + 2, y + 2, fillWidth, BAR_HEIGHT - 4);
      }
      const int pctY = y + (BAR_HEIGHT - lineHeight) / 2;
      renderer.drawText(UI_10_FONT_ID, pctX, pctY, pctBuf, true);
      y += BAR_HEIGHT + STEP_SPACING;
    } else {
      y += STEP_SPACING;
    }
  }

  if (state == State::PROCESSING && !cancelRequested) {
    const auto labels = mappedInput.mapLabels(tr(STR_CANCEL), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  if (state == State::SUCCESS || state == State::FAILED || state == State::CANCELLED) {
    y += metrics.verticalSpacing;
    const char* msg;
    if (state == State::SUCCESS)
      msg = tr(STR_DICT_PREPARE_SUCCESS);
    else if (state == State::CANCELLED)
      msg = tr(STR_DICT_PREPARE_CANCELLED);
    else
      msg = tr(STR_DICT_PREPARE_FAILED);
    renderer.drawCenteredText(UI_10_FONT_ID, y, msg, true, EpdFontFamily::BOLD);

    if (state == State::SUCCESS) {
      y += lineHeight + STEP_SPACING;
      char timeBuf[16];
      char elapsedBuf[28];
      snprintf(elapsedBuf, sizeof(elapsedBuf), "Elapsed: %s", fmtElapsed(timeBuf, sizeof(timeBuf), prepareElapsedMs));
      renderer.drawCenteredText(UI_10_FONT_ID, y, elapsedBuf);
    }
    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
