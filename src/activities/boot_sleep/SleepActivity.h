#pragma once
#include "../Activity.h"

class Bitmap;
class FsFile;

class SleepActivity final : public Activity {
  bool isOnBook = false;  // Track if we're currently on a book
  std::string previousActivityName;  // Name of the activity before sleep

 public:
  explicit SleepActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, const std::string& prevActivityName = "")
      : Activity("Sleep", renderer, mappedInput), previousActivityName(prevActivityName) {}
  void onEnter() override;

 private:
  void renderDefaultSleepScreen() const;
  void renderCustomSleepScreen() const;
  void renderCoverSleepScreen() const;
  void renderOverlaySleepScreen() const;
  void renderBitmapSleepScreen(const Bitmap& bitmap, bool clearBeforeDraw = true) const;
  void renderBlankSleepScreen() const;

  // Shared helpers for custom and overlay sleep images
  bool openSleepImage(FsFile& outFile) const;
  void getSleepBitmapLayout(const Bitmap& bitmap, int& x, int& y, float& cropX, float& cropY) const;
};
