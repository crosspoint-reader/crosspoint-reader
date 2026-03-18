#include "NotesActivity.h"

#include <GfxRenderer.h>
#include <HalStorage.h>
#include <I18n.h>
#include <Logging.h>

#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

void NotesActivity::onEnter() {
  Activity::onEnter();
  loadNotes();
  selectedIndex = 0;
  dirty = false;
  requestUpdate();
}

void NotesActivity::onExit() {
  if (dirty) {
    saveNotes();
  }
  Activity::onExit();
}

void NotesActivity::loadNotes() {
  notes.clear();

  // Heap-allocate read buffer to stay within stack limits
  constexpr size_t BUF_SIZE = 4096;
  auto* buf = static_cast<char*>(malloc(BUF_SIZE));
  if (!buf) {
    LOG_ERR("NOTES", "malloc failed for notes read");
    return;
  }

  const size_t bytesRead = Storage.readFileToBuffer(NOTES_FILE, buf, BUF_SIZE);
  if (bytesRead > 0) {
    notes.reserve(16);
    const char* p = buf;
    const char* end = buf + bytesRead;
    while (p < end && static_cast<int>(notes.size()) < MAX_NOTES) {
      const char* lineEnd = p;
      while (lineEnd < end && *lineEnd != '\n' && *lineEnd != '\r') {
        lineEnd++;
      }
      if (lineEnd > p) {
        notes.emplace_back(p, lineEnd - p);
      }
      p = lineEnd;
      while (p < end && (*p == '\n' || *p == '\r')) {
        p++;
      }
    }
  }

  free(buf);
  LOG_DBG("NOTES", "Loaded %zu notes", notes.size());
}

void NotesActivity::saveNotes() const {
  String content;
  content.reserve(notes.size() * 40);
  for (const auto& note : notes) {
    content += note.c_str();
    content += '\n';
  }
  if (!Storage.writeFile(NOTES_FILE, content)) {
    LOG_ERR("NOTES", "Failed to save notes");
  } else {
    LOG_DBG("NOTES", "Saved %zu notes", notes.size());
  }
}

void NotesActivity::addNote() {
  startActivityForResult(
      std::make_unique<KeyboardEntryActivity>(renderer, mappedInput, tr(STR_ADD_NOTE), "", MAX_NOTE_LENGTH, true),
      [this](const ActivityResult& result) {
        if (!result.isCancelled) {
          const auto& text = std::get<KeyboardResult>(result.data).text;
          if (!text.empty()) {
            notes.insert(notes.begin(), text);  // newest at top
            if (static_cast<int>(notes.size()) > MAX_NOTES) {
              notes.resize(MAX_NOTES);
            }
            selectedIndex = 0;
            dirty = true;
            saveNotes();
            dirty = false;
          }
        }
        requestUpdate();
      });
}

void NotesActivity::deleteSelectedNote() {
  if (notes.empty()) return;
  notes.erase(notes.begin() + selectedIndex);
  if (selectedIndex >= static_cast<int>(notes.size()) && selectedIndex > 0) {
    selectedIndex--;
  }
  dirty = true;
  saveNotes();
  dirty = false;
  requestUpdate();
}

void NotesActivity::loop() {
  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    activityManager.goHome();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    addNote();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Left) && !notes.empty()) {
    deleteSelectedNote();
    return;
  }

  const int count = static_cast<int>(notes.size()) + 1;  // +1 for "Add Note" entry
  buttonNavigator.onNext([this, count] {
    selectedIndex = ButtonNavigator::nextIndex(selectedIndex, count);
    requestUpdate();
  });
  buttonNavigator.onPrevious([this, count] {
    selectedIndex = ButtonNavigator::previousIndex(selectedIndex, count);
    requestUpdate();
  });
}

void NotesActivity::render(RenderLock&&) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, tr(STR_NOTES), nullptr);

  const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
  const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

  if (notes.empty()) {
    const int midY = contentTop + contentHeight / 2 - renderer.getLineHeight(UI_10_FONT_ID) / 2;
    renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_NOTES_EMPTY));
  } else {
    // Render notes as a scrollable list; include the "+ Add Note" entry at index 0 visually
    const int itemCount = static_cast<int>(notes.size()) + 1;
    GUI.drawList(
        renderer, Rect{0, contentTop, pageWidth, contentHeight}, itemCount, selectedIndex,
        [this](int index) -> std::string {
          if (index == 0) return tr(STR_ADD_NOTE);
          return notes[index - 1];
        },
        nullptr, nullptr, nullptr);
  }

  const char* deleteLabel = (selectedIndex > 0 && !notes.empty()) ? tr(STR_DELETE) : "";
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), tr(STR_ADD_NOTE), deleteLabel, "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
