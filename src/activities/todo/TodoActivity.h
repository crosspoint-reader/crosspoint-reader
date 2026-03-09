#pragma once

#include <GfxRenderer.h>

#include <string>
#include <vector>

#include "../ActivityWithSubactivity.h"

struct TodoItem {
  std::string text;
  bool checked;
  bool isHeader;  // For headers/notes that aren't tasks
};

class TodoActivity final : public ActivityWithSubactivity {
 public:
  explicit TodoActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string filePath,
                        std::string dateTitle, void* onBackCtx, void (*onBack)(void*));

  void onEnter() override;
  void onExit() override;
  void loop() override;
  void render(Activity::RenderLock&& lock) override;

 private:
  std::string filePath;
  std::string dateTitle;
  void* onBackCtx;
  void (*onBack)(void*);

  std::vector<TodoItem> items;
  int selectedIndex = 0;
  int scrollOffset = 0;  // Index of first visible item
  bool skipInitialInput = true;

  void loadTasks();
  void processTaskLine(std::string& line);
  void saveTasks();
  void toggleCurrentTask();
  void addNewEntry(bool agendaEntry);
  void editCurrentEntry();

  void renderScreen();
  void renderItem(int y, const TodoItem& item, bool isSelected) const;
};
