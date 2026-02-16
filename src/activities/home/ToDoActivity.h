#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <vector>
#include <string>
#include "../Activity.h"
#include "../../util/ButtonNavigator.h"

class ToDoActivity final : public Activity {
private:
    std::vector<std::string> tasks;
    int selectorIndex = 0;
    bool updateRequired = false;
    const std::function<void()> onBack;
    
    // Rendering task members (matching MyLibrary)
    TaskHandle_t displayTaskHandle = nullptr;
    SemaphoreHandle_t renderingMutex = nullptr;
    ButtonNavigator buttonNavigator;

    static void taskTrampoline(void* param);
    void displayTaskLoop();
    
    void loadTasks();
    void saveTasks();
    void toggleTask(int index);
    void render() const; // Note the 'const' to match GUI calls

public:
    ToDoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::function<void()> onBack);
    void onEnter() override;
    void onExit() override;
    void loop() override;
};