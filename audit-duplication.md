# Dictionary Feature Code Duplication Audit

**Date:** 2026-04-09  
**Scope:** All dictionary-related source files in `src/` and `src/util/`

---

## D-001: UTF-8 Codepoint Re-encoding (Verbatim Duplication)

**Locations:**
- `src/util/IpaUtils.h:46-60`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:151-166` (in `breakToken()`)

**Evidence:**
```cpp
// IpaUtils.h:46-60
if (cp < 0x80) {
  current += static_cast<char>(cp);
} else if (cp < 0x800) {
  current += static_cast<char>(0xC0 | (cp >> 6));
  current += static_cast<char>(0x80 | (cp & 0x3F));
} else if (cp < 0x10000) {
  current += static_cast<char>(0xE0 | (cp >> 12));
  current += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  current += static_cast<char>(0x80 | (cp & 0x3F));
} else {
  current += static_cast<char>(0xF0 | (cp >> 18));
  current += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  current += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  current += static_cast<char>(0x80 | (cp & 0x3F));
}

// DictionaryDefinitionActivity.cpp:152-165
char buf[5] = {};
if (cp < 0x80) {
  buf[0] = static_cast<char>(cp);
} else if (cp < 0x800) {
  buf[0] = static_cast<char>(0xC0 | (cp >> 6));
  buf[1] = static_cast<char>(0x80 | (cp & 0x3F));
} else if (cp < 0x10000) {
  buf[0] = static_cast<char>(0xE0 | (cp >> 12));
  buf[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[2] = static_cast<char>(0x80 | (cp & 0x3F));
} else {
  buf[0] = static_cast<char>(0xF0 | (cp >> 18));
  buf[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
  buf[2] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
  buf[3] = static_cast<char>(0x80 | (cp & 0x3F));
}
```

---

## D-002: Soft-Hyphen Detection (Intra-file Duplication)

**Location:** `src/activities/reader/DictionaryWordSelectActivity.cpp` — appears **twice**

**Evidence:**
```cpp
// Lines 165-171
bool endsWithHyphen = false;
if (lastWord[lastLen - 1] == '-') {
  endsWithHyphen = true;
} else if (lastLen >= 2 && static_cast<uint8_t>(lastWord[lastLen - 2]) == 0xC2 &&
           static_cast<uint8_t>(lastWord[lastLen - 1]) == 0xAD) {
  endsWithHyphen = true;
}

// Lines 200-206 — IDENTICAL
bool endsWithHyphen = false;
if (lastWord[lastLen - 1] == '-') {
  endsWithHyphen = true;
} else if (lastLen >= 2 && static_cast<uint8_t>(lastWord[lastLen - 2]) == 0xC2 &&
           static_cast<uint8_t>(lastWord[lastLen - 1]) == 0xAD) {
  endsWithHyphen = true;
}
```

---

## D-003: Soft-Hyphen Removal (Intra-file Duplication)

**Location:** `src/activities/reader/DictionaryWordSelectActivity.cpp` — appears **twice**

**Evidence:**
```cpp
// Lines 178-183
if (firstPart.back() == '-') {
  firstPart.pop_back();
} else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
           static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
  firstPart.erase(firstPart.size() - 2);
}

// Lines 209-213 — IDENTICAL
if (firstPart.back() == '-') {
  firstPart.pop_back();
} else if (firstPart.size() >= 2 && static_cast<uint8_t>(firstPart[firstPart.size() - 2]) == 0xC2 &&
           static_cast<uint8_t>(firstPart[firstPart.size() - 1]) == 0xAD) {
  firstPart.erase(firstPart.size() - 2);
}
```

---

## D-004: Controller Switch Block in loop()

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:228-260`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:411-444`
- `src/activities/reader/LookedUpWordsActivity.cpp:43-77`

**Evidence:** All three have near-identical structure:
```cpp
if (controller.isActive()) {
  switch (controller.handleInput()) {
    case DictionaryLookupController::LookupEvent::FoundDefinition:
      LookupHistory::addWord(cachePath, controller.getLookupWord(),
                             DictionaryLookupController::toHistStatus(controller.getFoundStatus()));
      startActivityForResult(
          std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, controller.getFoundWord(),
                                                         controller.getFoundLocation(), true, cachePath),
          [this](const ActivityResult& result) {
            if (!result.isCancelled) {
              setResult(ActivityResult{});
              finish();
            } else {
              requestUpdate();
            }
          });
      break;
    case DictionaryLookupController::LookupEvent::NotFoundDismissedBack:
      requestUpdate();
      break;
    case DictionaryLookupController::LookupEvent::NotFoundDismissedDone:
      setResult(ActivityResult{});
      finish();
      break;
    case DictionaryLookupController::LookupEvent::Cancelled:
      requestUpdate();
      break;
    default:
      break;
  }
  return;
}
```

Minor variations:
- DictionaryDefinitionActivity: has `chainWords.push_back()`, `chainBackNavInProgress` logic
- LookedUpWordsActivity: reloads `entries` in callback

---

## D-005: Status Code Parsing (Intra-file Duplication)

**Location:** `src/util/LookupHistory.cpp` — Lines 44-57 and 89-107

**Evidence:**
```cpp
// Appears twice in readAll() — once for mid-file lines, once for trailing line without newline
switch (code) {
  case 'D':
    e.status = Status::Direct;
    break;
  case 'T':
    e.status = Status::Stem;
    break;
  case 'Y':
    e.status = Status::AltForm;
    break;
  case 'S':
    e.status = Status::Suggestion;
    break;
  default:
    e.status = Status::NotFound;
    break;
}
```

---

## D-006: Back-Button Cancel Pattern

**Locations:** (6 occurrences)
- `src/activities/reader/DictionarySuggestionsActivity.cpp:20-26`
- `src/activities/reader/DictionaryWordSelectActivity.cpp:263-269`
- `src/activities/reader/DictionaryWordSelectActivity.cpp:288-294`
- `src/activities/reader/LookedUpWordsActivity.cpp:81-86`
- `src/activities/reader/LookedUpWordsActivity.cpp:148-154`
- `src/activities/settings/DictPrepareActivity.cpp:157-162`

**Evidence:**
```cpp
if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
  ActivityResult r;
  r.isCancelled = true;
  setResult(std::move(r));
  finish();
  return;
}
```

---

## D-007: Word Selection Confirm Handler

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:281-285`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:456-460`

**Evidence:**
```cpp
if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
  const auto* sel = navigator.getSelected();
  if (!sel) return;
  controller.lookupOrPopup(navigator.getLookup(*sel));
  return;
}
```

---

## D-008: IPA Mixed Width Calculation

**Location:** `src/activities/reader/DictionaryDefinitionActivity.cpp` — two lambdas

**Evidence:**
```cpp
// Lines 125-131 (wrapHtml)
auto getMixedWidth = [&](const char* text, EpdFontFamily::Style style) -> int {
  ipaRuns.clear();
  splitIpaRuns(text, ipaRuns);
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum +
           renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId(), run.text.c_str(), style);
  });
};

// Lines 262-268 (wrapPlain) — same logic, style defaults to REGULAR
auto getMixedWidthPlain = [&](const std::string& text) -> int {
  ipaRuns.clear();
  splitIpaRuns(text.c_str(), ipaRuns);
  return std::accumulate(ipaRuns.begin(), ipaRuns.end(), 0, [&](int sum, const IpaTextSpan& run) {
    return sum + renderer.getTextWidth(run.isIpa ? IPA_FONT_ID : SETTINGS.getDefinitionFontId(), run.text.c_str());
  });
};
```

---

## D-009: Long-Press Exit-All Pattern

**Location:** `src/activities/reader/DictionaryDefinitionActivity.cpp` — appears **twice**

**Evidence:**
```cpp
// Lines 464-469 (word-select mode)
if (mappedInput.isPressed(MappedInputManager::Button::Back) &&
    mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
  setResult(ActivityResult{});
  finish();
  return;
}

// Lines 515-519 (view mode) — nearly identical, adds `showLookupButton &&`
if (showLookupButton && mappedInput.isPressed(MappedInputManager::Button::Back) &&
    mappedInput.getHeldTime() >= Dictionary::LONG_PRESS_MS) {
  setResult(ActivityResult{});  // Done: isCancelled=false — exit all the way
  finish();
  return;
}
```

---

## D-010: lineHeight Calculation

**Location:** `src/activities/reader/DictionaryDefinitionActivity.cpp` — appears **3 times**

**Evidence:**
```cpp
// Lines 59-60
const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                        SETTINGS.getDefinitionLineCompression());

// Lines 342-343 — IDENTICAL
const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                        SETTINGS.getDefinitionLineCompression());

// Lines 548-549 — IDENTICAL
const int lineHeight = static_cast<int>(renderer.getLineHeight(SETTINGS.getDefinitionFontId()) *
                                        SETTINGS.getDefinitionLineCompression());
```

---

## D-011: ButtonNavigator Wiring

**Locations:**
- `src/activities/settings/DictionarySelectActivity.cpp:346-365`
- `src/activities/reader/LookedUpWordsActivity.cpp:126-141`

**Evidence:**
```cpp
buttonNavigator.onNextRelease([this] {
  selectedIndex = ButtonNavigator::nextIndex(selectedIndex, totalItems);
  requestUpdate();
});

buttonNavigator.onPreviousRelease([this] {
  selectedIndex = ButtonNavigator::previousIndex(selectedIndex, totalItems);
  requestUpdate();
});

buttonNavigator.onNextContinuous([this, pageItems] {
  selectedIndex = ButtonNavigator::nextPageIndex(selectedIndex, totalItems, pageItems);
  requestUpdate();
});

buttonNavigator.onPreviousContinuous([this, pageItems] {
  selectedIndex = ButtonNavigator::previousPageIndex(selectedIndex, totalItems, pageItems);
  requestUpdate();
});
```

---

## D-012: FreeRTOS Task Entry Trampoline

**Locations:**
- `src/activities/settings/DictPrepareActivity.cpp:144-149`
- `src/util/DictionaryLookupController.cpp:231-236`

**Evidence:**
```cpp
// DictPrepareActivity.cpp:144-149
void DictPrepareActivity::taskEntry(void* param) {
  DictPrepareActivity* self = static_cast<DictPrepareActivity*>(param);
  self->runSteps();
  self->taskHandle = nullptr;
  vTaskDelete(nullptr);
}

// DictionaryLookupController.cpp:231-236
void DictionaryLookupController::taskEntry(void* param) {
  DictionaryLookupController* self = static_cast<DictionaryLookupController*>(param);
  self->runLookup();
  self->taskHandle = nullptr;
  vTaskDelete(nullptr);
}
```

---

## D-013: onExit Task Cleanup

**Locations:**
- `src/activities/settings/DictPrepareActivity.cpp:132-138`
- `src/util/DictionaryLookupController.cpp:43-48`

**Evidence:**
```cpp
// DictPrepareActivity.cpp:132-138
void DictPrepareActivity::onExit() {
  if (taskHandle != nullptr) {
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
  }
  Activity::onExit();
}

// DictionaryLookupController.cpp:43-48
void DictionaryLookupController::onExit() {
  if (taskHandle != nullptr) {
    vTaskDelete(taskHandle);
    taskHandle = nullptr;
  }
}
```

---

## D-014: startActivityForResult(DictionaryDefinitionActivity)

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:234-244`
- `src/activities/reader/LookedUpWordsActivity.cpp:49-60`

**Evidence:**
```cpp
// DictionaryWordSelectActivity.cpp:234-244
startActivityForResult(
    std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, controller.getFoundWord(),
                                                   controller.getFoundLocation(), true, cachePath),
    [this](const ActivityResult& result) {
      if (!result.isCancelled) {
        setResult(ActivityResult{});
        finish();
      } else {
        requestUpdate();
      }
    });

// LookedUpWordsActivity.cpp:49-60 — nearly identical, only difference is reloading entries
startActivityForResult(
    std::make_unique<DictionaryDefinitionActivity>(renderer, mappedInput, controller.getFoundWord(),
                                                   controller.getFoundLocation(), true, cachePath),
    [this](const ActivityResult& result) {
      entries = LookupHistory::load(cachePath);  // <-- only difference
      if (!result.isCancelled) {
        setResult(ActivityResult{});
        finish();
      } else {
        requestUpdate();
      }
    });
```

---

## D-015: Controller onExit() Calls

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:32-35`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:31-34`
- `src/activities/reader/LookedUpWordsActivity.cpp:38-41`

**Evidence:**
```cpp
void XXXActivity::onExit() {
  controller.onExit();
  Activity::onExit();
}
```

---

## D-016: `controller.render()` Guard

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:299`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:545`
- `src/activities/reader/LookedUpWordsActivity.cpp:159`

**Evidence:**
```cpp
if (controller.render()) return;
```

---

## D-017: Empty List Back Handler

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:262-270`
- `src/activities/reader/LookedUpWordsActivity.cpp:80-88`

**Evidence:**
```cpp
// DictionaryWordSelectActivity.cpp:262-270
if (navigator.isEmpty()) {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
  }
  return;
}

// LookedUpWordsActivity.cpp:80-88
if (entries.empty()) {
  if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    ActivityResult r;
    r.isCancelled = true;
    setResult(std::move(r));
    finish();
  }
  return;
}
```

---

## D-018: LookupHistory::addWord() Call Pattern

**Locations:**
- `src/activities/reader/DictionaryWordSelectActivity.cpp:232-233`
- `src/activities/reader/DictionaryDefinitionActivity.cpp:417-418`
- `src/activities/reader/LookedUpWordsActivity.cpp:47-48`

**Evidence:**
```cpp
LookupHistory::addWord(cachePath, controller.getLookupWord(),
                       DictionaryLookupController::toHistStatus(controller.getFoundStatus()));
```

---

## D-019: Dictionary File Path Construction

**Location:** `src/util/Dictionary.cpp` — scattered throughout (~15 occurrences)

**Evidence:** Repeated `folderPath + ".idx"`, `folderPath + ".dict"`, `folderPath + ".syn"`, `folderPath + ".idx.oft"`, `folderPath + ".syn.oft"` constructions at:
- Lines 91-94 (exists)
- Lines 100-101 (hasAltForms)
- Lines 107-110 (isValidDictionary)
- Line 350 (readDefinition)
- Line 374 (locate)
- Line 382 (locate)
- Lines 441-443 (wordAtOrdinal)
- Lines 450-451 (wordAtOrdinal)
- Lines 485-486 (resolveAltForm)
- Lines 495-496 (resolveAltForm)
- Line 738 (findSimilar)
- Line 746 (findSimilar)

Also in `src/activities/settings/DictPrepareActivity.cpp:110-123`.

---

## D-020: appendMixed / getMixedWidth IPA Pattern

**Location:** `src/activities/reader/DictionaryDefinitionActivity.cpp`

**Evidence:** `getMixedWidth`, `getMixedWidthPlain`, and `appendMixed` lambdas all follow:
```cpp
ipaRuns.clear();
splitIpaRuns(text, ipaRuns);
for (const auto& run : ipaRuns) {
  // process run
}
```

Occurrences:
- Lines 125-131 (getMixedWidth)
- Lines 134-141 (appendMixed)
- Lines 262-268 (getMixedWidthPlain)
- Lines 270-278 (flushLine in wrapPlain)

---

## D-021: Empty State Rendering

**Locations:**
- `src/activities/reader/LookedUpWordsActivity.cpp:169-176`
- `src/activities/settings/DictionarySelectActivity.cpp:534-538`

**Evidence:**
```cpp
// LookedUpWordsActivity.cpp:169-176
if (entries.empty()) {
  const int midY = contentTop + (pageHeight - contentTop - metrics.buttonHintsHeight) / 2;
  renderer.drawCenteredText(UI_10_FONT_ID, midY, tr(STR_LOOKUP_HISTORY_EMPTY));
  const auto labels = mappedInput.mapLabels(tr(STR_BACK), "", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
  renderer.displayBuffer(HalDisplay::FAST_REFRESH);
  return;
}

// DictionarySelectActivity.cpp:534-538
if (dictFolders.empty()) {
  const int textY = contentTop + contentHeight / 3;
  renderer.drawCenteredText(UI_10_FONT_ID, textY, tr(STR_DICT_NONE_FOUND));
}
```

---

## Summary

| ID | Description | Occurrences | Severity | Status |
|----|-------------|-------------|----------|--------|
| D-001 | UTF-8 re-encoding | 2 | High (verbatim) | Fixed |
| D-002 | Soft-hyphen detection | 2 | High (intra-file) | Fixed |
| D-003 | Soft-hyphen removal | 2 | High (intra-file) | Fixed |
| D-004 | Controller switch block | 3 | High | Deferred |
| D-005 | Status code parsing | 2 | High (intra-file) | Fixed |
| D-006 | Back-cancel pattern | 6 | Medium | Deferred |
| D-007 | Word-select confirm | 2 | Medium | Fixed |
| D-008 | IPA mixed width | 2 | Medium | Fixed |
| D-009 | Long-press exit-all | 2 | Medium | Fixed |
| D-010 | lineHeight calc | 3 | Medium | Fixed |
| D-011 | ButtonNavigator wiring | 2 | Medium | Deferred |
| D-012 | Task entry trampoline | 2 | Medium | Non-applicable — wholly different uses, zero overlap |
| D-013 | Task cleanup in onExit | 2 | Medium | |
| D-014 | startActivityForResult | 2 | Medium | |
| D-015 | controller.onExit() | 3 | Low | |
| D-016 | controller.render() | 3 | Low | |
| D-017 | Empty list back handler | 2 | Low | |
| D-018 | addWord() call | 3 | Low | |
| D-019 | Path construction | ~15 | Low | |
| D-020 | IPA split pattern | 4 | Low | |
| D-021 | Empty state render | 2 | Low | |

---

## Files Audited

- `src/util/Dictionary.h`
- `src/util/Dictionary.cpp`
- `src/util/DictionaryLookupController.h`
- `src/util/DictionaryLookupController.cpp`
- `src/util/IpaUtils.h`
- `src/util/LookupHistory.h`
- `src/util/LookupHistory.cpp`
- `src/util/WordSelectNavigator.h`
- `src/util/WordSelectNavigator.cpp`
- `src/activities/settings/DictionarySelectActivity.h`
- `src/activities/settings/DictionarySelectActivity.cpp`
- `src/activities/settings/DictPrepareActivity.h`
- `src/activities/settings/DictPrepareActivity.cpp`
- `src/activities/reader/DictionaryDefinitionActivity.h`
- `src/activities/reader/DictionaryDefinitionActivity.cpp`
- `src/activities/reader/DictionaryWordSelectActivity.h`
- `src/activities/reader/DictionaryWordSelectActivity.cpp`
- `src/activities/reader/DictionarySuggestionsActivity.h`
- `src/activities/reader/DictionarySuggestionsActivity.cpp`
- `src/activities/reader/LookedUpWordsActivity.h`
- `src/activities/reader/LookedUpWordsActivity.cpp`
