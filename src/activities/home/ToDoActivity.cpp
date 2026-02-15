#include "ToDoActivity.h"
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <HalDisplay.h>
#include "components/UITheme.h"
#include "fontIds.h"

ToDoActivity::ToDoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onBack)
    : Activity("ToDo", renderer, mappedInput), onBack(onBack) {}

void ToDoActivity::taskTrampoline(void* param) {
    auto* self = static_cast<ToDoActivity*>(param);
    self->displayTaskLoop();
}

void ToDoActivity::onEnter() {
    Activity::onEnter();
    renderingMutex = xSemaphoreCreateMutex();
    loadTasks();
    selectorIndex = 0;
    updateRequired = true;

    // Create the background rendering task
    xTaskCreate(&ToDoActivity::taskTrampoline, "ToDoTask", 4096, this, 1, &displayTaskHandle);
}

void ToDoActivity::onExit() {
  Activity::onExit();

  // Just stop the task so we don't crash while deleting the activity
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void ToDoActivity::loadTasks() {
    tasks.clear();
    // Open from root as requested so it's visible in the file manager
    auto file = Storage.open("/todo.txt");
    if (file) {
        std::string line;
        while (file.available()) {
            char c = file.read();
            if (c == '\n') {
                if (!line.empty()) tasks.push_back(line);
                line = "";
            } else if (c != '\r') { 
                line += c; 
            }
        }
        if (!line.empty()) tasks.push_back(line);
        file.close();
    }
}

void ToDoActivity::saveTasks() {
    // Delete the old version first to prevent leftover data from doubling
    Storage.remove("/todo.txt"); 

    auto file = Storage.open("/todo.txt", FILE_WRITE);
    if (file) {
        for (const auto& t : tasks) {
            file.println(t.c_str()); // println adds the \n for you
        }
        file.close();
    } else {
        LOG_ERR("TODO", "Failed to open todo.txt for writing!");
    }
}

void ToDoActivity::loop() {
    if (tasks.empty()) {
        if (mappedInput.wasReleased(MappedInputManager::Button::Back)) onBack();
        return;
    }

    int listSize = static_cast<int>(tasks.size());

    // Navigation using the project's standard ButtonNavigator
    buttonNavigator.onNextRelease([this, listSize] {
        selectorIndex = ButtonNavigator::nextIndex(selectorIndex, listSize);
        updateRequired = true;
    });

    buttonNavigator.onPreviousRelease([this, listSize] {
        selectorIndex = ButtonNavigator::previousIndex(selectorIndex, listSize);
        updateRequired = true;
    });

    // Toggle Checkmark ONLY logic
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
        std::string& t = tasks[selectorIndex];
        bool changed = false;

        if (t.find("[ ]") == 0) {
            t.replace(0, 3, "[x]");
            changed = true;
        } else if (t.find("[x]") == 0) {
            t.replace(0, 3, "[ ]");
            changed = true;
        }

        if (changed) {
            saveTasks();
            updateRequired = true;
        }
    }

    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
        updateRequired = true; 
        onBack();
    }
}

void ToDoActivity::displayTaskLoop() {
    while (true) {
        if (updateRequired) {
            updateRequired = false;
            xSemaphoreTake(renderingMutex, portMAX_DELAY);
            render();
            xSemaphoreGive(renderingMutex);
        }
        vTaskDelay(10 / portTICK_PERIOD_MS);
    }
}

void ToDoActivity::render() const {
    renderer.clearScreen();
    const auto pageWidth = renderer.getScreenWidth();
    const auto pageHeight = renderer.getScreenHeight();
    auto metrics = UITheme::getInstance().getMetrics();

    GUI.drawHeader(renderer, Rect{0, metrics.topPadding, pageWidth, metrics.headerHeight}, "To-Do List");

    const int contentTop = metrics.topPadding + metrics.headerHeight + metrics.verticalSpacing;
    const int contentHeight = pageHeight - contentTop - metrics.buttonHintsHeight - metrics.verticalSpacing;

    if (tasks.empty()) {
        renderer.drawText(UI_10_FONT_ID, metrics.contentSidePadding, contentTop + 20, "No todo.txt found in root");
    } else {
        GUI.drawList(
            renderer, Rect{0, contentTop, pageWidth, contentHeight}, tasks.size(), selectorIndex,
            [this](int index) { return tasks[index]; }, nullptr, nullptr, nullptr);
    }

    const auto labels = mappedInput.mapLabels("« Home", tasks.empty() ? "" : "Toggle", "Up", "Down");
    GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

    // No arguments here = Full Refresh. 
    // This matches SettingsActivity.cpp and stops the burn-in.
    renderer.displayBuffer(); 
}