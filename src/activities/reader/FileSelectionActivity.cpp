#include "FileSelectionActivity.h"

#include <GfxRenderer.h>
#include <SDCardManager.h>

#include <functional>
#include <string>

#include "MappedInputManager.h"
#include "fontIds.h"
#include "util/StringUtils.h"

namespace {
constexpr int PAGE_ITEMS = 23;
constexpr int SKIP_PAGE_MS = 700;
constexpr unsigned long GO_HOME_MS = 1000;
constexpr unsigned long DELETE_LONG_PRESS_MS = 1000;
}  // namespace

void sortFileList(std::vector<std::string>& strs) {
  std::sort(begin(strs), end(strs), [](const std::string& str1, const std::string& str2) {
    if (str1.back() == '/' && str2.back() != '/') return true;
    if (str1.back() != '/' && str2.back() == '/') return false;
    return lexicographical_compare(
        begin(str1), end(str1), begin(str2), end(str2),
        [](const char& char1, const char& char2) { return tolower(char1) < tolower(char2); });
  });
}

void FileSelectionActivity::taskTrampoline(void* param) {
  auto* self = static_cast<FileSelectionActivity*>(param);
  self->displayTaskLoop();
}

void FileSelectionActivity::loadFiles() {
  files.clear();

  auto root = SdMan.open(basepath.c_str());
  if (!root || !root.isDirectory()) {
    if (root) root.close();
    return;
  }

  root.rewindDirectory();

  char name[500];
  for (auto file = root.openNextFile(); file; file = root.openNextFile()) {
    file.getName(name, sizeof(name));
    if (name[0] == '.' || strcmp(name, "System Volume Information") == 0) {
      file.close();
      continue;
    }

    if (file.isDirectory()) {
      files.emplace_back(std::string(name) + "/");
    } else {
      auto filename = std::string(name);
      if (StringUtils::checkFileExtension(filename, ".epub") || StringUtils::checkFileExtension(filename, ".xtch") ||
          StringUtils::checkFileExtension(filename, ".xtc")) {
        files.emplace_back(filename);
      }
    }
    file.close();
  }
  root.close();
  sortFileList(files);
}

void FileSelectionActivity::onEnter() {
  Activity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();

  // basepath is set via constructor parameter (defaults to "/" if not specified)
  state = State::BROWSING;
  loadFiles();
  selectorIndex = 0;
  deleteConfirmSelection = 0;
  fileToDelete.clear();

  // Trigger first update
  updateRequired = true;

  xTaskCreate(&FileSelectionActivity::taskTrampoline, "FileSelectionActivityTask",
              2048,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void FileSelectionActivity::onExit() {
  Activity::onExit();

  // Wait until not rendering to delete task to avoid killing mid-instruction to EPD
  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
  files.clear();
}

void FileSelectionActivity::deleteFile(const std::string& filePath) {
  // Check if this is an EPUB or XTC file and delete its cache
  std::string ext4 = filePath.length() >= 4 ? filePath.substr(filePath.length() - 4) : "";
  std::string ext5 = filePath.length() >= 5 ? filePath.substr(filePath.length() - 5) : "";
  
  bool isEpub = (ext5 == ".epub");
  bool isXtc = (ext5 == ".xtch" || ext4 == ".xtc");
  
  if (isEpub || isXtc) {
    // Calculate cache path the same way Epub/Xtc classes do
    const std::string cacheDir = "/.crosspoint";
    const std::string cachePrefix = isEpub ? "epub_" : "xtc_";
    const std::string cachePath = cacheDir + "/" + cachePrefix + std::to_string(std::hash<std::string>{}(filePath));
    
    // Delete cache directory if it exists
    if (SdMan.exists(cachePath.c_str())) {
      if (SdMan.removeDir(cachePath.c_str())) {
        Serial.printf("[%lu] [FileSel] Deleted cache: %s\n", millis(), cachePath.c_str());
      } else {
        Serial.printf("[%lu] [FileSel] Failed to delete cache: %s\n", millis(), cachePath.c_str());
      }
    }
  }
  
  // Delete the actual file
  if (SdMan.remove(filePath.c_str())) {
    Serial.printf("[%lu] [FileSel] Deleted: %s\n", millis(), filePath.c_str());
    loadFiles();
    updateRequired = true;
  } else {
    Serial.printf("[%lu] [FileSel] Failed to delete: %s\n", millis(), filePath.c_str());
  }
}

void FileSelectionActivity::loop() {
  if (state == State::DELETE_CONFIRM) {
    // Handle navigation in delete confirmation
    if (mappedInput.wasReleased(MappedInputManager::Button::Left) ||
        mappedInput.wasReleased(MappedInputManager::Button::Right)) {
      deleteConfirmSelection = (deleteConfirmSelection + 1) % 2;
      updateRequired = true;
    }

    // Handle confirmation
    if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
      if (deleteConfirmSelection == 0) {
        // Yes - delete the file
        deleteFile(fileToDelete);
        state = State::BROWSING;
        fileToDelete.clear();
      } else {
        // No - cancel
        state = State::BROWSING;
        fileToDelete.clear();
      }
      updateRequired = true;
      return;
    }

    // Handle cancel (back button)
    if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
      state = State::BROWSING;
      fileToDelete.clear();
      updateRequired = true;
      return;
    }

    return;
  }

  // Long press BACK (1s+) goes to root folder
  if (mappedInput.isPressed(MappedInputManager::Button::Back) && mappedInput.getHeldTime() >= GO_HOME_MS) {
    if (basepath != "/") {
      basepath = "/";
      loadFiles();
      updateRequired = true;
    }
    return;
  }

  const bool prevReleased = mappedInput.wasReleased(MappedInputManager::Button::Up) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Left);
  const bool nextReleased = mappedInput.wasReleased(MappedInputManager::Button::Down) ||
                            mappedInput.wasReleased(MappedInputManager::Button::Right);

  const bool skipPage = mappedInput.getHeldTime() > SKIP_PAGE_MS;

  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm)) {
    // Check if this was a long press
    if (mappedInput.getHeldTime() >= DELETE_LONG_PRESS_MS) {
      // Long press - show delete confirmation (only for files, not directories)
      if (!files.empty() && files[selectorIndex].back() != '/') {
        if (basepath.back() != '/') basepath += "/";
        fileToDelete = basepath + files[selectorIndex];
        state = State::DELETE_CONFIRM;
        deleteConfirmSelection = 0;  // Default to "Yes"
        updateRequired = true;
      }
    } else {
      // Short press - normal open behavior
      if (files.empty()) {
        return;
      }

      if (basepath.back() != '/') basepath += "/";
      if (files[selectorIndex].back() == '/') {
        basepath += files[selectorIndex].substr(0, files[selectorIndex].length() - 1);
        loadFiles();
        selectorIndex = 0;
        updateRequired = true;
      } else {
        onSelect(basepath + files[selectorIndex]);
      }
    }
  } else if (mappedInput.wasReleased(MappedInputManager::Button::Back)) {
    // Short press: go up one directory, or go home if at root
    if (mappedInput.getHeldTime() < GO_HOME_MS) {
      if (basepath != "/") {
        const std::string oldPath = basepath;

        basepath.replace(basepath.find_last_of('/'), std::string::npos, "");
        if (basepath.empty()) basepath = "/";
        loadFiles();

        const auto pos = oldPath.find_last_of('/');
        const std::string dirName = oldPath.substr(pos + 1) + "/";
        selectorIndex = findEntry(dirName);

        updateRequired = true;
      } else {
        onGoHome();
      }
    }
  } else if (prevReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS - 1) * PAGE_ITEMS + files.size()) % files.size();
    } else {
      selectorIndex = (selectorIndex + files.size() - 1) % files.size();
    }
    updateRequired = true;
  } else if (nextReleased) {
    if (skipPage) {
      selectorIndex = ((selectorIndex / PAGE_ITEMS + 1) * PAGE_ITEMS) % files.size();
    } else {
      selectorIndex = (selectorIndex + 1) % files.size();
    }
    updateRequired = true;
  }
}

void FileSelectionActivity::displayTaskLoop() {
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

void FileSelectionActivity::renderDeleteConfirm() const {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  const auto height = renderer.getLineHeight(UI_10_FONT_ID);
  const auto top = (pageHeight - height * 3) / 2;

  renderer.drawCenteredText(UI_12_FONT_ID, top - 40, "Confirm Delete?", true, EpdFontFamily::BOLD);

  // Show filename (truncate if too long)
  std::string fileName = fileToDelete;
  size_t lastSlash = fileName.find_last_of('/');
  if (lastSlash != std::string::npos) {
    fileName = fileName.substr(lastSlash + 1);
  }
  if (fileName.length() > 30) {
    fileName = fileName.substr(0, 27) + "...";
  }
  renderer.drawCenteredText(UI_10_FONT_ID, top, fileName.c_str());

  // Draw Yes/No buttons
  const int buttonY = top + 80;
  constexpr int buttonWidth = 60;
  constexpr int buttonSpacing = 30;
  constexpr int totalWidth = buttonWidth * 2 + buttonSpacing;
  const int startX = (pageWidth - totalWidth) / 2;

  // Draw "Yes" button
  if (deleteConfirmSelection == 0) {
    renderer.drawText(UI_10_FONT_ID, startX, buttonY, "[Yes]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + 4, buttonY, "Yes");
  }

  // Draw "No" button
  if (deleteConfirmSelection == 1) {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing, buttonY, "[No]");
  } else {
    renderer.drawText(UI_10_FONT_ID, startX + buttonWidth + buttonSpacing + 4, buttonY, "No");
  }

  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight - 30, "LEFT/RIGHT: Select | OK: Confirm");

  renderer.displayBuffer();
}

void FileSelectionActivity::render() const {
  if (state == State::DELETE_CONFIRM) {
    renderDeleteConfirm();
    return;
  }

  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();
  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Books", true, EpdFontFamily::BOLD);

  // Help text
  const auto labels = mappedInput.mapLabels("« Home", "Open", "", "");
  renderer.drawButtonHints(UI_10_FONT_ID, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  if (files.empty()) {
    renderer.drawText(UI_10_FONT_ID, 20, 60, "No books found");
    renderer.displayBuffer();
    return;
  }

  const auto pageStartIndex = selectorIndex / PAGE_ITEMS * PAGE_ITEMS;
  renderer.fillRect(0, 60 + (selectorIndex % PAGE_ITEMS) * 30 - 2, pageWidth - 1, 30);
  for (size_t i = pageStartIndex; i < files.size() && i < pageStartIndex + PAGE_ITEMS; i++) {
    auto item = renderer.truncatedText(UI_10_FONT_ID, files[i].c_str(), renderer.getScreenWidth() - 40);
    renderer.drawText(UI_10_FONT_ID, 20, 60 + (i % PAGE_ITEMS) * 30, item.c_str(), i != selectorIndex);
  }

  renderer.displayBuffer();
}

size_t FileSelectionActivity::findEntry(const std::string& name) const {
  for (size_t i = 0; i < files.size(); i++)
    if (files[i] == name) return i;
  return 0;
}
