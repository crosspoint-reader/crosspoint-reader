// Pocket Library for CrossPoint
// Copyright (C) 2026 Pocket Library contributors
// SPDX-License-Identifier: GPL-3.0-or-later
//
// This program is free software: you can redistribute it and/or modify it under
// the terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or (at your option) any later
// version. See LICENSE-GPL-3.0 at the repository root.

// Compiled only into Pocket Library builds; stock envs see an empty unit.
#ifdef POCKET_LIBRARY

#include "DiagnosticsActivity.h"

#include <Arduino.h>
#include <BoardConfig.h>
#include <GfxRenderer.h>
#include <HalMemory.h>
#include <HalStorage.h>
#include <Logging.h>
#include <esp_psram.h>
#include <esp_random.h>
#include <esp_timer.h>

#include <algorithm>
#include <cstdio>
#include <cstring>

#if FREEINK_SD_SDMMC
#include <driver/sdmmc_host.h>
#endif

#include "MappedInputManager.h"
#include "components/UITheme.h"

namespace fui = freeink::ui;

namespace {
enum Row {
  ROW_PSRAM = 0,
  ROW_PSRAM_FREE,
  ROW_INTERNAL_FREE,
  ROW_FLASH,
  ROW_SD_BUS,
  ROW_RUN,
  ROW_FILE,
  ROW_SEQ,
  ROW_RAND_AVG,
  ROW_RAND_TAIL,
  ROW_WRITE,
};

// Diagnostic labels stay English on every unit, like AboutActivity: they are
// read from screenshots and serial logs, not by the reader.
const char* const rowNames[DiagnosticsActivity::ITEM_COUNT] = {
    "PSRAM",       "PSRAM free",  "Internal RAM free", "Flash",          "SD bus",        "SD benchmark",
    "Bench file",  "Sequential",  "Random 4 KB avg",   "Random p95/max", "Seq. write",
};

constexpr size_t kMiB = 1024u * 1024u;
// A file at least this large is used as-is; otherwise a scratch file is written.
constexpr uint64_t kMinBenchFileBytes = 16 * kMiB;
constexpr size_t kScratchFileBytes = 32 * kMiB;
constexpr const char* kScratchDir = "/.pocketlib";
constexpr const char* kScratchPath = "/.pocketlib/bench.bin";
constexpr size_t kChunkBytes = 64 * 1024;
constexpr size_t kSeqReadBytes = 16 * kMiB;
constexpr size_t kRandomReadBytes = 4096;
constexpr int kRandomReads = 64;
constexpr int kMaxPath = 192;

double elapsedMs(int64_t startUs) { return static_cast<double>(esp_timer_get_time() - startUs) / 1000.0; }

std::string heapLine(const HalMemory::HeapStats& h) {
  char buf[48];
  snprintf(buf, sizeof(buf), "%u KB (largest %u KB)", static_cast<unsigned>(h.freeBytes / 1024),
           static_cast<unsigned>(h.largestBlockBytes / 1024));
  return buf;
}

// Largest regular file directly inside `dir` or one level below it. The
// library's ZIM parts are the realistic case: big, and deep into FAT chains.
void findLargestFile(const char* dir, int depth, char* bestPath, uint64_t& bestSize) {
  auto d = Storage.open(dir);
  if (!d || !d.isDirectory()) return;
  d.rewindDirectory();
  char name[96];
  for (auto entry = d.openNextFile(); entry; entry = d.openNextFile()) {
    entry.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) continue;
    char path[kMaxPath];
    snprintf(path, sizeof(path), "%s%s%s", dir, (dir[strlen(dir) - 1] == '/') ? "" : "/", name);
    if (entry.isDirectory()) {
      if (depth > 0) findLargestFile(path, depth - 1, bestPath, bestSize);
      continue;
    }
    const uint64_t size = entry.fileSize64();
    if (size > bestSize) {
      bestSize = size;
      snprintf(bestPath, kMaxPath, "%s", path);
    }
  }
}
}  // namespace

DiagnosticsActivity::DiagnosticsActivity(GfxRenderer& renderer, MappedInputManager& mappedInput)
    : UiListActivity("Diagnostics", renderer, mappedInput) {}

void DiagnosticsActivity::onEnter() {
  UiListActivity::onEnter();
  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].label = rowNames[i];
    rowItems_[i].actionValue = static_cast<int16_t>(i);
  }

  char buf[48];
  snprintf(buf, sizeof(buf), "%u MB", static_cast<unsigned>(esp_psram_get_size() / kMiB));
  rowValues_[ROW_PSRAM] = buf;
  snprintf(buf, sizeof(buf), "%u MB @ %u MHz", static_cast<unsigned>(ESP.getFlashChipSize() / kMiB),
           static_cast<unsigned>(ESP.getFlashChipSpeed() / 1000000u));
  rowValues_[ROW_FLASH] = buf;

#if FREEINK_SD_SDMMC
  int khz = 0;
  const int slot = SDMMC_HOST_SLOT_1;
  if (sdmmc_host_get_real_freq(slot, &khz) == ESP_OK) {
    snprintf(buf, sizeof(buf), "SDMMC %u-bit @ %.1f MHz", static_cast<unsigned>(sdmmc_host_get_slot_width(slot)),
             khz / 1000.0);
  } else {
    snprintf(buf, sizeof(buf), "SDMMC (clock unknown)");
  }
#else
  snprintf(buf, sizeof(buf), "SPI");
#endif
  rowValues_[ROW_SD_BUS] = buf;
  rowValues_[ROW_RUN] = "Tap to run";
  refreshMemoryRows();

  LOG_INF("DIAG", "PSRAM %s, flash %s, SD %s", rowValues_[ROW_PSRAM].c_str(), rowValues_[ROW_FLASH].c_str(),
          rowValues_[ROW_SD_BUS].c_str());
  LOG_INF("DIAG", "PSRAM free %s; internal free %s", rowValues_[ROW_PSRAM_FREE].c_str(),
          rowValues_[ROW_INTERNAL_FREE].c_str());
}

void DiagnosticsActivity::refreshMemoryRows() {
  rowValues_[ROW_PSRAM_FREE] = heapLine(HalMemory::getPsramHeap());
  rowValues_[ROW_INTERNAL_FREE] = heapLine(HalMemory::getInternalHeap());
}

void DiagnosticsActivity::activateIndex(int index) {
  if (index != ROW_RUN || benchmarkPending_) return;
  benchmarkPending_ = true;
  rowValues_[ROW_RUN] = "Running... (up to a minute)";
  // Paint "Running" first; the benchmark itself runs on the next loop pass.
  requestUpdate(true);
}

bool DiagnosticsActivity::handleCustomInput() {
  if (!benchmarkPending_) return false;
  runBenchmark();
  benchmarkPending_ = false;
  refreshMemoryRows();
  requestUpdate();
  return true;
}

void DiagnosticsActivity::runBenchmark() {
  char buf[64];
  // The buffer lives in PSRAM on purpose: that is where the ZIM reader will
  // put cluster data, so the numbers include the same copy path.
  auto buffer = HalMemory::allocatePsram(kChunkBytes);
  if (!buffer) {
    rowValues_[ROW_RUN] = "Failed: no PSRAM";
    LOG_ERR("DIAG", "PSRAM allocation of %u bytes failed", static_cast<unsigned>(kChunkBytes));
    return;
  }

  char path[kMaxPath] = {0};
  uint64_t fileSize = 0;
  findLargestFile("/library", 2, path, fileSize);
  if (fileSize < kMinBenchFileBytes) findLargestFile("/", 1, path, fileSize);

  rowValues_[ROW_WRITE] = "-";
  if (fileSize < kMinBenchFileBytes) {
    // No large file on the card yet: write our own scratch file.
    Storage.ensureDirectoryExists(kScratchDir);
    HalFile out;
    if (!Storage.openFileForWrite("DIAG", kScratchPath, out)) {
      rowValues_[ROW_RUN] = "Failed: cannot write scratch file";
      return;
    }
    for (size_t i = 0; i < kChunkBytes; i++) buffer[i] = static_cast<uint8_t>(esp_random());
    const int64_t t0 = esp_timer_get_time();
    for (size_t written = 0; written < kScratchFileBytes; written += kChunkBytes) {
      if (out.write(buffer.get(), kChunkBytes) != kChunkBytes) {
        rowValues_[ROW_RUN] = "Failed: write error";
        return;
      }
    }
    out.flush();
    const double ms = elapsedMs(t0);
    out.close();
    snprintf(buf, sizeof(buf), "%.2f MB/s", (kScratchFileBytes / static_cast<double>(kMiB)) / (ms / 1000.0));
    rowValues_[ROW_WRITE] = buf;
    snprintf(path, sizeof(path), "%s", kScratchPath);
    fileSize = kScratchFileBytes;
  }

  HalFile in;
  if (!Storage.openFileForRead("DIAG", path, in)) {
    rowValues_[ROW_RUN] = "Failed: cannot open file";
    return;
  }
  const char* base = strrchr(path, '/');
  snprintf(buf, sizeof(buf), "%s (%u MB)", base ? base + 1 : path, static_cast<unsigned>(fileSize / kMiB));
  rowValues_[ROW_FILE] = buf;

  // Sequential: 64 KB reads from the start of the file.
  const size_t seqBytes = static_cast<size_t>(std::min<uint64_t>(fileSize, kSeqReadBytes));
  in.seek64(0);
  int64_t t0 = esp_timer_get_time();
  size_t done = 0;
  while (done < seqBytes) {
    const int n = in.read(buffer.get(), kChunkBytes);
    if (n <= 0) break;
    done += static_cast<size_t>(n);
  }
  const double seqMs = elapsedMs(t0);
  snprintf(buf, sizeof(buf), "%.2f MB/s (%u MB)", (done / static_cast<double>(kMiB)) / (seqMs / 1000.0),
           static_cast<unsigned>(done / kMiB));
  rowValues_[ROW_SEQ] = buf;

  // Random: 4 KB-aligned reads anywhere in the file, including backward seeks,
  // which on FAT32 can re-walk the cluster chain from the start of the file.
  float samples[kRandomReads];
  const uint64_t slots = fileSize / kRandomReadBytes;
  for (int i = 0; i < kRandomReads; i++) {
    const uint64_t r = (static_cast<uint64_t>(esp_random()) << 32) | esp_random();
    const uint64_t offset = (r % slots) * kRandomReadBytes;
    t0 = esp_timer_get_time();
    in.seek64(offset);
    in.read(buffer.get(), kRandomReadBytes);
    samples[i] = static_cast<float>(elapsedMs(t0));
  }
  double sum = 0;
  for (float s : samples) sum += s;
  std::sort(samples, samples + kRandomReads);
  snprintf(buf, sizeof(buf), "%.1f ms", sum / kRandomReads);
  rowValues_[ROW_RAND_AVG] = buf;
  snprintf(buf, sizeof(buf), "%.1f / %.1f ms", samples[(kRandomReads * 95) / 100], samples[kRandomReads - 1]);
  rowValues_[ROW_RAND_TAIL] = buf;

  rowValues_[ROW_RUN] = "Done (tap to rerun)";
  LOG_INF("DIAG", "bench file %s, %llu bytes", path, static_cast<unsigned long long>(fileSize));
  LOG_INF("DIAG", "sequential %s; random 4K avg %s, p95/max %s; write %s", rowValues_[ROW_SEQ].c_str(),
          rowValues_[ROW_RAND_AVG].c_str(), rowValues_[ROW_RAND_TAIL].c_str(), rowValues_[ROW_WRITE].c_str());
}

void DiagnosticsActivity::buildScreen(UiScreen& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.setContentMarginFromScreen(fui::Insets{static_cast<int16_t>(metrics.topPadding + metrics.headerHeight), 0,
                                                static_cast<int16_t>(metrics.buttonHintsHeight), 0});
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));

  for (int i = 0; i < ITEM_COUNT; i++) {
    rowItems_[i].value = rowValues_[i].c_str();
  }

  fui::ListProps props;
  props.items = rowItems_;
  props.count = ITEM_COUNT;
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;
  props.valueInset = 8;
  props.labelText = screen.theme().smallText;
  props.labelText.maxLines = 1;
  syncListViewport(screen, props);
  screen.list(props);
}

#endif  // POCKET_LIBRARY
