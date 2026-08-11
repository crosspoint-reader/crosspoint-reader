#include "NoteListActivity.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Memory.h>

#include <algorithm>

#include "MappedInputManager.h"
#include "NoteEditorActivity.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr const char* NOTES_DIR = "/Notes";
constexpr size_t NAME_BUFFER_SIZE = 160;
}

void NoteListActivity::onEnter() {
  Activity::onEnter();
  nameBuffer_ = makeUniqueNoThrow<char[]>(NAME_BUFFER_SIZE);
  loadNotes();
  requestUpdate();
}

void NoteListActivity::onExit() {
  Activity::onExit();
  notes_.clear();
  nameBuffer_.reset();
}

std::string NoteListActivity::normalizeNoteName(std::string name) {
  while (!name.empty() && (name.back() == ' ' || name.back() == '\t' || name.back() == '\r' || name.back() == '\n')) {
    name.pop_back();
  }
  while (!name.empty() && (name.front() == ' ' || name.front() == '\t' || name.front() == '\r' || name.front() == '\n')) {
    name.erase(name.begin());
  }
  if (name.empty()) name = "Notiz";
  const auto slash = name.find_last_of('/');
  if (slash != std::string::npos) name = name.substr(slash + 1);
  const auto dot = name.find_last_of('.');
  if (dot != std::string::npos) name = name.substr(0, dot);
  name = StringUtils::sanitizeFilename(name, 96);
  if (name.empty()) name = "Notiz";
  return name + ".md";
}

std::string NoteListActivity::notesPathForName(const std::string& name) { return std::string(NOTES_DIR) + "/" + name; }

void NoteListActivity::loadNotes() {
  notes_.clear();
  if (!Storage.exists(NOTES_DIR)) Storage.mkdir(NOTES_DIR);
  auto dir = Storage.open(NOTES_DIR);
  if (!dir || !dir.isDirectory() || !nameBuffer_) return;
  dir.rewindDirectory();
  for (auto f = dir.openNextFile(); f; f = dir.openNextFile()) {
    f.getName(nameBuffer_.get(), NAME_BUFFER_SIZE);
    if (!f.isDirectory()) {
      std::string_view name{nameBuffer_.get()};
      if (FsHelpers::hasMarkdownExtension(name)) notes_.emplace_back(name);
    }
  }
  dir.close();
  FsHelpers::sortFileList(notes_);
  selectedIndex_ = std::min<int>(selectedIndex_, std::max<int>(0, notes_.size()));
}

void NoteListActivity::createNote() {
  inNameEntry_ = true;
  startActivityForResult(std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_NOTE_NAME), "", 96),
                         [this](const ActivityResult& result) {
                           inNameEntry_ = false;
                           if (result.isCancelled) {
                             requestUpdate();
                             return;
                           }
                           std::string name = normalizeNoteName(std::get<KeyboardResult>(result.data).text);
                           std::string path = notesPathForName(name);
                           int suffix = 2;
                           while (Storage.exists(path.c_str()) && suffix < 1000) {
                             const auto base = name.substr(0, name.size() - 3);
                             name = base + "-" + std::to_string(suffix++) + ".md";
                             path = notesPathForName(name);
                           }
                           loadNotes();
                           startActivityForResult(std::make_unique<NoteEditorActivity>(renderer, mappedInput, path, true),
                                                  [this](const ActivityResult&) {
                                                    loadNotes();
                                                    requestUpdate();
                                                  });
                         });
}

void NoteListActivity::openSelected() {
  if (selectedIndex_ == 0) {
    createNote();
    return;
  }
  const int noteIndex = selectedIndex_ - 1;
  if (noteIndex < 0 || noteIndex >= static_cast<int>(notes_.size())) return;
  const std::string path = notesPathForName(notes_[noteIndex]);
  startActivityForResult(std::make_unique<NoteEditorActivity>(renderer, mappedInput, path, false),
                         [this](const ActivityResult&) {
                           loadNotes();
                           requestUpdate();
                         });
}

void NoteListActivity::loop() {
  if (inNameEntry_) return;
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    finish();
    return;
  }
  const int count = static_cast<int>(notes_.size()) + 1;
  buttonNavigator_.onNextRelease([this, count] {
    selectedIndex_ = ButtonNavigator::nextIndex(selectedIndex_, count);
    requestUpdate();
  });
  buttonNavigator_.onPreviousRelease([this, count] {
    selectedIndex_ = ButtonNavigator::previousIndex(selectedIndex_, count);
    requestUpdate();
  });
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) openSelected();
}

void NoteListActivity::render(RenderLock&&) {
  renderer.clearScreen();
  const auto& metrics = UITheme::getInstance().getMetrics();
  const int width = renderer.getScreenWidth();
  const int height = renderer.getScreenHeight();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, width, metrics.headerHeight}, tr(STR_NOTES), CROSSPOINT_VERSION);

  const int listTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int count = static_cast<int>(notes_.size()) + 1;
  GUI.drawList(renderer,
               Rect{0, listTop, width, height - listTop - metrics.buttonHintsHeight - metrics.verticalSpacing}, count,
               selectedIndex_,
               [this](int i) {
                 if (i == 0) return std::string(tr(STR_NEW_NOTE));
                 return notes_[i - 1];
               },
               nullptr, nullptr, nullptr, true);

  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer();
}
