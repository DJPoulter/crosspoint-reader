#pragma once
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include <functional>
#include <string>
#include <vector>

#include "../Activity.h"

class FileSelectionActivity final : public Activity {
  enum class State {
    BROWSING,
    DELETE_CONFIRM
  };

  TaskHandle_t displayTaskHandle = nullptr;
  SemaphoreHandle_t renderingMutex = nullptr;
  std::string basepath = "/";
  std::vector<std::string> files;
  size_t selectorIndex = 0;
  bool updateRequired = false;
  State state = State::BROWSING;
  int deleteConfirmSelection = 0;  // 0 = Yes, 1 = No
  std::string fileToDelete;
  const std::function<void(const std::string&)> onSelect;
  const std::function<void()> onGoHome;

  static void taskTrampoline(void* param);
  [[noreturn]] void displayTaskLoop();
  void render() const;
  void renderDeleteConfirm() const;
  void loadFiles();
  void deleteFile(const std::string& filePath);

  size_t findEntry(const std::string& name) const;

 public:
  explicit FileSelectionActivity(GfxRenderer& renderer, MappedInputManager& mappedInput,
                                 const std::function<void(const std::string&)>& onSelect,
                                 const std::function<void()>& onGoHome, std::string initialPath = "/")
      : Activity("FileSelection", renderer, mappedInput),
        basepath(initialPath.empty() ? "/" : std::move(initialPath)),
        onSelect(onSelect),
        onGoHome(onGoHome) {}
  void onEnter() override;
  void onExit() override;
  void loop() override;
};
