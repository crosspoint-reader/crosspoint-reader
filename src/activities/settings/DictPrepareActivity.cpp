#include "DictPrepareActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <InflateReader.h>
#include <Logging.h>

#include <memory>

#include "I18nKeys.h"
#include "MappedInputManager.h"
#include "components/UITheme.h"
#include "fontIds.h"

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
// .idx.oft header constant (verified against real StarDict .oft files)
// "StarDict's Cache, Version: 0.2" (30 bytes) + fixed 8-byte magic
// ---------------------------------------------------------------------------
static constexpr uint8_t OFT_HEADER[38] = {'S', 't', 'a', 'r', 'D',  'i',  'c',  't',  '\'', 's',  ' ',  'C', 'a',
                                           'c', 'h', 'e', ',', ' ',  'V',  'e',  'r',  's',  'i',  'o',  'n', ':',
                                           ' ', '0', '.', '2', 0xc1, 0xd1, 0xa4, 0x51, 0x00, 0x00, 0x00, 0x00};

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

const char* DictPrepareActivity::stepLabel(StepType type) {
  switch (type) {
    case StepType::EXTRACT_DICT:
      return tr(STR_DICT_STEP_EXTRACT_DICT);
    case StepType::EXTRACT_SYN:
      return tr(STR_DICT_STEP_EXTRACT_SYN);
    case StepType::GEN_IDX:
      return tr(STR_DICT_STEP_GEN_IDX);
    case StepType::GEN_SYN:
      return tr(STR_DICT_STEP_GEN_SYN);
  }
  return "";
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

void DictPrepareActivity::onEnter() {
  Activity::onEnter();
  state = State::CONFIRM;
  prepareDone = false;
  prepareSucceeded = false;
  cancelRequested = false;
  taskHandle = nullptr;
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
  requestUpdate();
}

void DictPrepareActivity::detectSteps() {
  stepCount = 0;

  // Single buffer reused for all path checks — 7 x char[520] simultaneously would waste 3120 bytes.
  char pathBuf[520];

  snprintf(pathBuf, sizeof(pathBuf), "%s.dict", folderPath.c_str());
  const bool dictExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.dict.dz", folderPath.c_str());
  const bool dzExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.syn", folderPath.c_str());
  const bool synExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.syn.dz", folderPath.c_str());
  const bool synDzExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.idx", folderPath.c_str());
  const bool idxExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.idx.oft", folderPath.c_str());
  const bool idxOftExists = Storage.exists(pathBuf);
  snprintf(pathBuf, sizeof(pathBuf), "%s.syn.oft", folderPath.c_str());
  const bool synOftExists = Storage.exists(pathBuf);

  if (!dictExists && dzExists) steps[stepCount++] = {StepType::EXTRACT_DICT};
  if (!synExists && synDzExists) steps[stepCount++] = {StepType::EXTRACT_SYN};
  if (idxExists && !idxOftExists) steps[stepCount++] = {StepType::GEN_IDX};
  const bool synWillExist = synExists || synDzExists;
  if (synWillExist && !synOftExists) steps[stepCount++] = {StepType::GEN_SYN};
}

void DictPrepareActivity::onExit() {
  if (taskHandle != nullptr) {
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
  }
  Activity::onExit();
}

// ---------------------------------------------------------------------------
// FreeRTOS task entry
// ---------------------------------------------------------------------------

void DictPrepareActivity::taskEntry(void* param) {
  DictPrepareActivity* self = static_cast<DictPrepareActivity*>(param);
  self->runSteps();
  self->taskHandle = nullptr;
  vTaskDelete(nullptr);
}

// ---------------------------------------------------------------------------
// Input / state machine
// ---------------------------------------------------------------------------

void DictPrepareActivity::loop() {
  if (state == State::CONFIRM) {
    if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
      ActivityResult r;
      r.isCancelled = true;
      setResult(std::move(r));
      finish();
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
      xTaskCreate(taskEntry, "DictPrep", 4096, this, 1, &taskHandle);
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
  for (int i = 0; i < stepCount; i++) {
    if (cancelRequested) break;

    currentStep = i;
    steps[i].status = StepStatus::IN_PROGRESS;
    requestUpdate(true);

    char srcPath[520], dstPath[520];
    bool ok = false;

    switch (steps[i].type) {
      case StepType::EXTRACT_DICT:
        snprintf(srcPath, sizeof(srcPath), "%s.dict.dz", folderPath.c_str());
        snprintf(dstPath, sizeof(dstPath), "%s.dict", folderPath.c_str());
        ok = extractFile(srcPath, dstPath, steps[i]);
        if (!ok) Storage.remove(dstPath);
        break;

      case StepType::EXTRACT_SYN:
        snprintf(srcPath, sizeof(srcPath), "%s.syn.dz", folderPath.c_str());
        snprintf(dstPath, sizeof(dstPath), "%s.syn", folderPath.c_str());
        ok = extractFile(srcPath, dstPath, steps[i]);
        if (!ok) Storage.remove(dstPath);
        break;

      case StepType::GEN_IDX:
        snprintf(srcPath, sizeof(srcPath), "%s.idx", folderPath.c_str());
        snprintf(dstPath, sizeof(dstPath), "%s.idx.oft", folderPath.c_str());
        ok = generateOft(srcPath, dstPath, 8, steps[i]);  // .idx: 4-byte offset + 4-byte size
        if (!ok) Storage.remove(dstPath);
        break;

      case StepType::GEN_SYN:
        snprintf(srcPath, sizeof(srcPath), "%s.syn", folderPath.c_str());
        snprintf(dstPath, sizeof(dstPath), "%s.syn.oft", folderPath.c_str());
        ok = generateOft(srcPath, dstPath, 4, steps[i]);  // .syn: 4-byte original_word_index
        if (!ok) Storage.remove(dstPath);
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
  std::unique_ptr<PrepDecompCtx> ctx(new PrepDecompCtx{});
  ctx->file = &inputFile;

  constexpr size_t OUT_BUF_SIZE = 4096;
  std::unique_ptr<uint8_t[]> outBuf(new uint8_t[OUT_BUF_SIZE]);

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
      step.progress = pos;
      requestUpdate(true);
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

  step.progress = step.total;
  return true;
}

// ---------------------------------------------------------------------------
// .oft index generation
// ---------------------------------------------------------------------------

bool DictPrepareActivity::generateOft(const char* srcPath, const char* oftPath, uint8_t skipPerEntry, Step& step) {
  HalFile src;
  if (!Storage.openFileForRead("DICT_PREP", srcPath, src)) {
    LOG_ERR("DICT_PREP", "Failed to open: %s", srcPath);
    return false;
  }

  step.total = src.fileSize();
  step.progress = 0;

  HalFile oft;
  if (!Storage.openFileForWrite("DICT_PREP", oftPath, oft)) {
    src.close();
    LOG_ERR("DICT_PREP", "Failed to open for write: %s", oftPath);
    return false;
  }

  // Write the 38-byte header.
  if (oft.write(OFT_HEADER, sizeof(OFT_HEADER)) != sizeof(OFT_HEADER)) {
    src.close();
    oft.close();
    LOG_ERR("DICT_PREP", "Header write failed: %s", oftPath);
    return false;
  }

  constexpr size_t PROGRESS_INTERVAL = 65536;
  size_t lastProgressPos = 0;
  uint32_t entryCount = 0;
  bool error = false;
  uint8_t skipBuf[8];  // large enough for skipPerEntry (max 8)

  while (true) {
    // Record the byte position of this entry before parsing it.
    const uint32_t entryPos = static_cast<uint32_t>(src.position());

    if (entryPos >= step.total) break;

    // Every 32 entries starting from entry 32, write entryPos to the .oft file.
    // Entry N in .oft = byte offset of word at ordinal N*32.
    if (entryCount > 0 && entryCount % 32 == 0) {
      if (oft.write(&entryPos, 4) != 4) {
        LOG_ERR("DICT_PREP", "Write failed: %s", oftPath);
        error = true;
        break;
      }
    }

    // Skip null-terminated word string. StarDict .syn files may contain long phrases;
    // use a generous limit (4096) to accommodate them without allocating a stack buffer.
    bool foundNull = false;
    for (int b = 0; b < 4096; b++) {
      int ch = src.read();
      if (ch < 0) {
        goto done;
      }
      if (ch == 0) {
        foundNull = true;
        break;
      }
    }
    if (!foundNull) {
      LOG_ERR("DICT_PREP", "Word too long or read error in %s", srcPath);
      error = true;
      break;
    }

    // Skip fixed-size suffix (offset+size for .idx, or original_word_index for .syn).
    if (src.read(skipBuf, skipPerEntry) != static_cast<int>(skipPerEntry)) break;

    entryCount++;

    // Periodic progress update.
    const uint32_t pos = static_cast<uint32_t>(src.position());
    if (pos - lastProgressPos >= PROGRESS_INTERVAL) {
      lastProgressPos = pos;
      step.progress = pos;
      requestUpdate(true);
      vTaskDelay(1);
      if (cancelRequested) {
        error = true;
        break;
      }
    }
  }

done:
  if (!error) {
    const uint32_t sentinel = static_cast<uint32_t>(step.total);
    if (oft.write(&sentinel, 4) != 4) {
      LOG_ERR("DICT_PREP", "Sentinel write failed: %s", oftPath);
      error = true;
    }
  }
  src.close();
  oft.close();

  if (error) {
    LOG_ERR("DICT_PREP", "Generation failed: %s", oftPath);
    return false;
  }

  step.progress = step.total;
  return true;
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

    const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  }

  renderer.displayBuffer();
}
