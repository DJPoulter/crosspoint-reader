#include "KoboSyncSettingsActivity.h"

#include <GfxRenderer.h>

#include <cstring>

#include "KoboSyncTokenStore.h"
#include "MappedInputManager.h"
#include "activities/util/KeyboardEntryActivity.h"
#include "components/UITheme.h"
#include "fontIds.h"

namespace {
constexpr int MENU_ITEMS = 1;
const char* menuNames[MENU_ITEMS] = {"Sync Token"};
}  // namespace

void KoboSyncSettingsActivity::taskTrampoline(void* param) {
  auto* self = static_cast<KoboSyncSettingsActivity*>(param);
  self->displayTaskLoop();
}

void KoboSyncSettingsActivity::onEnter() {
  ActivityWithSubactivity::onEnter();

  renderingMutex = xSemaphoreCreateMutex();
  selectedIndex = 0;
  updateRequired = true;

  xTaskCreate(&KoboSyncSettingsActivity::taskTrampoline, "KoboSyncSettingsTask",
              4096,               // Stack size
              this,               // Parameters
              1,                  // Priority
              &displayTaskHandle  // Task handle
  );
}

void KoboSyncSettingsActivity::onExit() {
  ActivityWithSubactivity::onExit();

  xSemaphoreTake(renderingMutex, portMAX_DELAY);
  if (displayTaskHandle) {
    vTaskDelete(displayTaskHandle);
    displayTaskHandle = nullptr;
  }
  vSemaphoreDelete(renderingMutex);
  renderingMutex = nullptr;
}

void KoboSyncSettingsActivity::loop() {
  if (subActivity) {
    subActivity->loop();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Back)) {
    onBack();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Confirm)) {
    handleSelection();
    return;
  }

  if (mappedInput.wasPressed(MappedInputManager::Button::Up) ||
      mappedInput.wasPressed(MappedInputManager::Button::Left)) {
    selectedIndex = (selectedIndex + MENU_ITEMS - 1) % MENU_ITEMS;
    updateRequired = true;
  } else if (mappedInput.wasPressed(MappedInputManager::Button::Down) ||
             mappedInput.wasPressed(MappedInputManager::Button::Right)) {
    selectedIndex = (selectedIndex + 1) % MENU_ITEMS;
    updateRequired = true;
  }
}

void KoboSyncSettingsActivity::handleSelection() {
  xSemaphoreTake(renderingMutex, portMAX_DELAY);

  if (selectedIndex == 0) {
    exitActivity();
    enterNewActivity(new KeyboardEntryActivity(
        renderer, mappedInput, "Kobo Sync Token", KOBO_TOKEN_STORE.getToken(), 10,
        63,     // maxLength (fits in store)
        false,  // not password
        [this](const std::string& token) {
          KOBO_TOKEN_STORE.setToken(token);
          KOBO_TOKEN_STORE.saveToFile();
          exitActivity();
          updateRequired = true;
        },
        [this]() {
          exitActivity();
          updateRequired = true;
        }));
  }

  xSemaphoreGive(renderingMutex);
}

void KoboSyncSettingsActivity::displayTaskLoop() {
  while (true) {
    if (updateRequired && !subActivity) {
      updateRequired = false;
      xSemaphoreTake(renderingMutex, portMAX_DELAY);
      render();
      xSemaphoreGive(renderingMutex);
    }
    vTaskDelay(10 / portTICK_PERIOD_MS);
  }
}

void KoboSyncSettingsActivity::render() {
  renderer.clearScreen();

  const auto pageWidth = renderer.getScreenWidth();

  renderer.drawCenteredText(UI_12_FONT_ID, 15, "Kobo Sync", true, EpdFontFamily::BOLD);

  renderer.fillRect(0, 60 + selectedIndex * 30 - 2, pageWidth - 1, 30);

  for (int i = 0; i < MENU_ITEMS; i++) {
    const int settingY = 60 + i * 30;
    const bool isSelected = (i == selectedIndex);

    renderer.drawText(UI_10_FONT_ID, 20, settingY, menuNames[i], !isSelected);

    const char* status = KOBO_TOKEN_STORE.getToken().empty() ? "[Not Set]" : "[Set]";
    const auto width = renderer.getTextWidth(UI_10_FONT_ID, status);
    renderer.drawText(UI_10_FONT_ID, pageWidth - 20 - width, settingY, status, !isSelected);
  }

  const auto labels = mappedInput.mapLabels("« Back", "Select", "", "");
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);

  renderer.displayBuffer();
}
