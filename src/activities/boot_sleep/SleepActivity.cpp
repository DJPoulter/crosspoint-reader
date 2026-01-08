#include "SleepActivity.h"

#include <cstdlib>

#include <Epub.h>
#include <GfxRenderer.h>
#include <SDCardManager.h>
#include <Xtc.h>

#include "CrossPointSettings.h"
#include "CrossPointState.h"
#include "fontIds.h"
#include "images/CrossLarge.h"
#include "util/StringUtils.h"

void SleepActivity::onEnter() {
  Activity::onEnter();

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::OVERLAY) {
    Serial.printf("[%lu] [SLP] Entering overlay sleep mode\n", millis());
    // For overlay mode: store framebuffer (book content), show popup, then restore and draw overlay
    // Store the framebuffer before popup overwrites it
    // Note: storeBwBuffer() will succeed for any rendered content, not just books
    // This means if you're on home screen or other UI, it will also store the framebuffer
    isOnBook = renderer.storeBwBuffer();
    Serial.printf("[%lu] [SLP] Overlay mode: isOnBook=%d (from storeBwBuffer)\n", millis(), isOnBook);
    if (!isOnBook) {
      // Not on a book - clear screen to white before showing overlay
      Serial.printf("[%lu] [SLP] Not on book - clearing screen to white\n", millis());
      renderer.clearScreen();
    } else {
      Serial.printf("[%lu] [SLP] On book - framebuffer stored, will restore before overlay\n", millis());
    }
    renderPopup("Entering Sleep...");
    return renderOverlaySleepScreen();
  }

  renderPopup("Entering Sleep...");

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::BLANK) {
    return renderBlankSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::CUSTOM) {
    return renderCustomSleepScreen();
  }

  if (SETTINGS.sleepScreen == CrossPointSettings::SLEEP_SCREEN_MODE::COVER) {
    return renderCoverSleepScreen();
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderPopup(const char* message) const {
  const bool wasDarkMode = renderer.isDarkModeEnabled();
  renderer.setDarkModeEnabled(false);
  const int textWidth = renderer.getTextWidth(UI_12_FONT_ID, message, EpdFontFamily::BOLD);
  constexpr int margin = 20;
  const int x = (renderer.getScreenWidth() - textWidth - margin * 2) / 2;
  constexpr int y = 117;
  const int w = textWidth + margin * 2;
  const int h = renderer.getLineHeight(UI_12_FONT_ID) + margin * 2;
  // renderer.clearScreen();
  renderer.fillRect(x - 5, y - 5, w + 10, h + 10, true);
  renderer.fillRect(x + 5, y + 5, w - 10, h - 10, false);
  renderer.drawText(UI_12_FONT_ID, x + margin, y + margin, message, true, EpdFontFamily::BOLD);
  renderer.displayBuffer();
  renderer.setDarkModeEnabled(wasDarkMode);
}

void SleepActivity::renderCustomSleepScreen() const {
  // Check if we have a /sleep directory
  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (filename.substr(filename.length() - 4) != ".bmp") {
        Serial.printf("[%lu] [SLP] Skipping non-.bmp file name: %s\n", millis(), name);
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        Serial.printf("[%lu] [SLP] Skipping invalid BMP file: %s\n", millis(), name);
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 1 and numFiles
      const auto randomFileIndex = random(numFiles);
      const auto filename = "/sleep/" + files[randomFileIndex];
      FsFile file;
      if (SdMan.openFileForRead("SLP", filename, file)) {
        Serial.printf("[%lu] [SLP] Randomly loading: /sleep/%s\n", millis(), files[randomFileIndex].c_str());
        delay(100);
        Bitmap bitmap(file);
        if (bitmap.parseHeaders() == BmpReaderError::Ok) {
          renderBitmapSleepScreen(bitmap);
          dir.close();
          return;
        }
      }
    }
  }
  if (dir) dir.close();

  // Look for sleep.bmp on the root of the sd card to determine if we should
  // render a custom sleep screen instead of the default.
  FsFile file;
  if (SdMan.openFileForRead("SLP", "/sleep.bmp", file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      Serial.printf("[%lu] [SLP] Loading: /sleep.bmp\n", millis());
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderDefaultSleepScreen() const {
  const bool wasDarkMode = renderer.isDarkModeEnabled();
  renderer.setDarkModeEnabled(false);
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();

  renderer.clearScreen();
  renderer.drawImage(CrossLarge, (pageWidth + 128) / 2, (pageHeight - 128) / 2, 128, 128);
  renderer.drawCenteredText(UI_10_FONT_ID, pageHeight / 2 + 70, "CrossPoint", true, EpdFontFamily::BOLD);
  renderer.drawCenteredText(SMALL_FONT_ID, pageHeight / 2 + 95, "SLEEPING");

  // Make sleep screen dark unless light is selected in settings
  if (SETTINGS.sleepScreen != CrossPointSettings::SLEEP_SCREEN_MODE::LIGHT) {
    renderer.invertScreen();
  }

  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
  renderer.setDarkModeEnabled(wasDarkMode);
}

void SleepActivity::renderBitmapSleepScreen(const Bitmap& bitmap) const {
  int x, y;
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  float cropX = 0, cropY = 0;

  Serial.printf("[%lu] [SLP] bitmap %d x %d, screen %d x %d\n", millis(), bitmap.getWidth(), bitmap.getHeight(),
                pageWidth, pageHeight);
  if (bitmap.getWidth() > pageWidth || bitmap.getHeight() > pageHeight) {
    // image will scale, make sure placement is right
    float ratio = static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
    const float screenRatio = static_cast<float>(pageWidth) / static_cast<float>(pageHeight);

    Serial.printf("[%lu] [SLP] bitmap ratio: %f, screen ratio: %f\n", millis(), ratio, screenRatio);
    if (ratio > screenRatio) {
      // image wider than viewport ratio, scaled down image needs to be centered vertically
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropX = 1.0f - (screenRatio / ratio);
        Serial.printf("[%lu] [SLP] Cropping bitmap x: %f\n", millis(), cropX);
        ratio = (1.0f - cropX) * static_cast<float>(bitmap.getWidth()) / static_cast<float>(bitmap.getHeight());
      }
      x = 0;
      y = std::round((static_cast<float>(pageHeight) - static_cast<float>(pageWidth) / ratio) / 2);
      Serial.printf("[%lu] [SLP] Centering with ratio %f to y=%d\n", millis(), ratio, y);
    } else {
      // image taller than viewport ratio, scaled down image needs to be centered horizontally
      if (SETTINGS.sleepScreenCoverMode == CrossPointSettings::SLEEP_SCREEN_COVER_MODE::CROP) {
        cropY = 1.0f - (ratio / screenRatio);
        Serial.printf("[%lu] [SLP] Cropping bitmap y: %f\n", millis(), cropY);
        ratio = static_cast<float>(bitmap.getWidth()) / ((1.0f - cropY) * static_cast<float>(bitmap.getHeight()));
      }
      x = std::round((pageWidth - pageHeight * ratio) / 2);
      y = 0;
      Serial.printf("[%lu] [SLP] Centering with ratio %f to x=%d\n", millis(), ratio, x);
    }
  } else {
    // center the image
    x = (pageWidth - bitmap.getWidth()) / 2;
    y = (pageHeight - bitmap.getHeight()) / 2;
  }

  const bool wasDarkMode = renderer.isDarkModeEnabled();
  renderer.setDarkModeEnabled(false);
  Serial.printf("[%lu] [SLP] drawing to %d x %d\n", millis(), x, y);
  renderer.clearScreen();
  renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
  renderer.setDarkModeEnabled(wasDarkMode);

  if (bitmap.hasGreyscale()) {
    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleLsbBuffers();

    bitmap.rewindToData();
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawBitmap(bitmap, x, y, pageWidth, pageHeight, cropX, cropY);
    renderer.copyGrayscaleMsbBuffers();

    renderer.displayGrayBuffer();
    renderer.setRenderMode(GfxRenderer::BW);
  }
}

void SleepActivity::renderCoverSleepScreen() const {
  if (APP_STATE.openEpubPath.empty()) {
    return renderDefaultSleepScreen();
  }

  std::string coverBmpPath;

  if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtc") ||
      StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".xtch")) {
    // Handle XTC file
    Xtc lastXtc(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastXtc.load()) {
      Serial.println("[SLP] Failed to load last XTC");
      return renderDefaultSleepScreen();
    }

    if (!lastXtc.generateCoverBmp()) {
      Serial.println("[SLP] Failed to generate XTC cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastXtc.getCoverBmpPath();
  } else if (StringUtils::checkFileExtension(APP_STATE.openEpubPath, ".epub")) {
    // Handle EPUB file
    Epub lastEpub(APP_STATE.openEpubPath, "/.crosspoint");
    if (!lastEpub.load()) {
      Serial.println("[SLP] Failed to load last epub");
      return renderDefaultSleepScreen();
    }

    if (!lastEpub.generateCoverBmp()) {
      Serial.println("[SLP] Failed to generate cover bmp");
      return renderDefaultSleepScreen();
    }

    coverBmpPath = lastEpub.getCoverBmpPath();
  } else {
    return renderDefaultSleepScreen();
  }

  FsFile file;
  if (SdMan.openFileForRead("SLP", coverBmpPath, file)) {
    Bitmap bitmap(file);
    if (bitmap.parseHeaders() == BmpReaderError::Ok) {
      renderBitmapSleepScreen(bitmap);
      return;
    }
  }

  renderDefaultSleepScreen();
}

void SleepActivity::renderOverlaySleepScreen() const {
  Serial.printf("[%lu] [SLP] renderOverlaySleepScreen: isOnBook=%d\n", millis(), isOnBook);
  // If we're on a book, restore the book content (this removes the popup from framebuffer)
  // If not on a book, the screen is already cleared to white
  if (isOnBook) {
    Serial.printf("[%lu] [SLP] Restoring book content framebuffer\n", millis());
    renderer.restoreBwBuffer();
  } else {
    // Not on a book - clear screen to white so white pixels in overlay appear white
    Serial.printf("[%lu] [SLP] Clearing screen to white (not on book)\n", millis());
    renderer.clearScreen();
  }

  // Look for sleep.bmp or sleep folder
  FsFile overlayFile;
  bool foundOverlay = false;
  
  // First check if we have a /sleep directory
  auto dir = SdMan.open("/sleep");
  if (dir && dir.isDirectory()) {
    std::vector<std::string> files;
    char name[500];
    // collect all valid BMP files
    for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
      if (file.isDirectory()) {
        file.close();
        continue;
      }
      file.getName(name, sizeof(name));
      auto filename = std::string(name);
      if (filename[0] == '.') {
        file.close();
        continue;
      }

      if (filename.substr(filename.length() - 4) != ".bmp") {
        file.close();
        continue;
      }
      Bitmap bitmap(file);
      if (bitmap.parseHeaders() != BmpReaderError::Ok) {
        file.close();
        continue;
      }
      files.emplace_back(filename);
      file.close();
    }
    const auto numFiles = files.size();
    if (numFiles > 0) {
      // Generate a random number between 0 and numFiles-1
      const auto randomFileIndex = random(numFiles);
      const auto filename = "/sleep/" + files[randomFileIndex];
      if (SdMan.openFileForRead("SLP", filename, overlayFile)) {
        Serial.printf("[%lu] [SLP] Randomly loading: /sleep/%s\n", millis(), files[randomFileIndex].c_str());
        foundOverlay = true;
      }
    }
  }
  if (dir) dir.close();

  // If not found in /sleep folder, try /sleep.bmp on root
  if (!foundOverlay) {
    if (SdMan.openFileForRead("SLP", "/sleep.bmp", overlayFile)) {
      Serial.printf("[%lu] [SLP] Loading: /sleep.bmp\n", millis());
      foundOverlay = true;
    }
  }

  if (!foundOverlay) {
    Serial.printf("[%lu] [SLP] ERROR: Failed to find sleep.bmp or sleep folder, falling back to default sleep screen\n", millis());
    renderDefaultSleepScreen();
    return;
  }
  Serial.printf("[%lu] [SLP] Successfully opened sleep overlay file\n", millis());

  Bitmap overlay(overlayFile);
  const BmpReaderError overlayError = overlay.parseHeaders();
  if (overlayError != BmpReaderError::Ok) {
    Serial.printf("[%lu] [SLP] ERROR: Failed to parse sleep overlay headers (error=%d), falling back to default\n", 
                  millis(), static_cast<int>(overlayError));
    overlayFile.close();
    renderDefaultSleepScreen();
    return;
  }
  Serial.printf("[%lu] [SLP] Overlay parsed: %dx%d, hasGreyscale=%d\n", millis(), 
                overlay.getWidth(), overlay.getHeight(), overlay.hasGreyscale());

  // Framebuffer now contains either book content (if on book) or white background (if not on book)
  // Calculate position to center the overlay
  const auto pageWidth = renderer.getScreenWidth();
  const auto pageHeight = renderer.getScreenHeight();
  int x = (pageWidth - overlay.getWidth()) / 2;
  int y = (pageHeight - overlay.getHeight()) / 2;

  // Ensure coordinates are non-negative
  if (x < 0) x = 0;
  if (y < 0) y = 0;
  Serial.printf("[%lu] [SLP] Overlay position: x=%d, y=%d (screen: %dx%d)\n", millis(), 
                x, y, pageWidth, pageHeight);

  // Cache the bitmap once to avoid reading from SD card 4 times
  // Free BW buffer first to free up 48KB RAM for caching
  Serial.printf("[%lu] [SLP] Freeing BW buffer to make room for overlay cache\n", millis());
  renderer.freeBwBuffer();  // Free 48KB - we don't need to restore since we're going to sleep
  
  Serial.printf("[%lu] [SLP] Attempting to cache overlay bitmap\n", millis());
  int cachedWidth, cachedHeight, cachedRowSize;
  uint8_t* cachedOverlay = renderer.cacheBitmap(overlay, &cachedWidth, &cachedHeight, &cachedRowSize);
  if (!cachedOverlay) {
    Serial.printf("[%lu] [SLP] Cache failed - will draw overlay directly from file (slower)\n", millis());
    // Keep file open - don't close it yet, bitmap needs it
    // Rewind bitmap to start of pixel data for first draw
    if (overlay.rewindToData() != BmpReaderError::Ok) {
      overlayFile.close();
      renderDefaultSleepScreen();
      return;
    }
    Serial.printf("[%lu] [SLP] Drawing BW overlay from file\n", millis());
    renderer.setRenderMode(GfxRenderer::BW);
    renderer.drawBitmap(overlay, x, y, pageWidth, pageHeight, 0, 0);
    if (overlay.hasGreyscale()) {
      Serial.printf("[%lu] [SLP] Overlay has grayscale - displaying BW first, then grayscale passes\n", millis());
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
      renderer.storeBwBuffer();
      if (overlay.rewindToData() != BmpReaderError::Ok) {
        overlayFile.close();
        return;
      }
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
      renderer.drawBitmap(overlay, x, y, pageWidth, pageHeight, 0, 0);
      renderer.copyGrayscaleLsbBuffers();
      if (overlay.rewindToData() != BmpReaderError::Ok) {
        overlayFile.close();
        return;
      }
      renderer.clearScreen(0x00);
      renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
      renderer.drawBitmap(overlay, x, y, pageWidth, pageHeight, 0, 0);
      renderer.copyGrayscaleMsbBuffers();
      renderer.displayGrayBuffer();
      renderer.setRenderMode(GfxRenderer::BW);
      // Don't restore BW buffer - we freed it for caching, and we're going to sleep anyway
      // Just re-draw the overlay on the current framebuffer (which has book + overlay from grayscale)
      if (overlay.rewindToData() != BmpReaderError::Ok) {
        overlayFile.close();
        return;
      }
      renderer.drawBitmap(overlay, x, y, pageWidth, pageHeight, 0, 0);
      renderer.cleanupGrayscaleWithFrameBuffer();
      Serial.printf("[%lu] [SLP] Completed grayscale overlay rendering from file\n", millis());
    } else {
      Serial.printf("[%lu] [SLP] Displaying BW overlay (no grayscale)\n", millis());
      renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    }
    overlayFile.close();
    Serial.printf("[%lu] [SLP] Overlay rendering complete (from file)\n", millis());
    return;
  }
  
  Serial.printf("[%lu] [SLP] Overlay cached successfully: %dx%d, rowSize=%d\n", millis(), 
                cachedWidth, cachedHeight, cachedRowSize);
  // Draw BW overlay from cache (no SD reads!)
  Serial.printf("[%lu] [SLP] Drawing BW overlay from cache\n", millis());
  renderer.setRenderMode(GfxRenderer::BW);
  renderer.drawCachedBitmap(cachedOverlay, cachedWidth, cachedHeight, cachedRowSize, x, y, pageWidth, pageHeight, 0, 0, overlay.isTopDown());

  // If overlay has grayscale, display BW first, then apply grayscale
  if (overlay.hasGreyscale()) {
    Serial.printf("[%lu] [SLP] Overlay has grayscale - displaying BW first, then grayscale passes\n", millis());
    // Display BW overlay with half refresh
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
    
    // Store BW buffer (book + overlay) before clearing for grayscale passes
    renderer.storeBwBuffer();
    
    // Do LSB grayscale pass from cache (no SD reads!)
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_LSB);
    renderer.drawCachedBitmap(cachedOverlay, cachedWidth, cachedHeight, cachedRowSize, x, y, pageWidth, pageHeight, 0, 0, overlay.isTopDown());
    renderer.copyGrayscaleLsbBuffers();

    // Do MSB grayscale pass from cache (no SD reads!)
    renderer.clearScreen(0x00);
    renderer.setRenderMode(GfxRenderer::GRAYSCALE_MSB);
    renderer.drawCachedBitmap(cachedOverlay, cachedWidth, cachedHeight, cachedRowSize, x, y, pageWidth, pageHeight, 0, 0, overlay.isTopDown());
    renderer.copyGrayscaleMsbBuffers();

    // Apply grayscale LUT (this happens immediately after BW, so it looks like one refresh)
    renderer.displayGrayBuffer();
    
    renderer.setRenderMode(GfxRenderer::BW);
    
    // Don't restore BW buffer - we freed it for caching, and we're going to sleep anyway
    // Just re-draw the overlay from cache on the current framebuffer (which has book + overlay from grayscale)
    renderer.drawCachedBitmap(cachedOverlay, cachedWidth, cachedHeight, cachedRowSize, x, y, pageWidth, pageHeight, 0, 0, overlay.isTopDown());
    renderer.cleanupGrayscaleWithFrameBuffer();
    Serial.printf("[%lu] [SLP] Completed grayscale overlay rendering from cache\n", millis());
  } else {
    // No grayscale - just display BW overlay
    Serial.printf("[%lu] [SLP] Displaying BW overlay (no grayscale)\n", millis());
    renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
  }

  // Free cached overlay data
  Serial.printf("[%lu] [SLP] Freeing cached overlay data\n", millis());
  free(cachedOverlay);
  overlayFile.close();
  Serial.printf("[%lu] [SLP] Overlay rendering complete (from cache)\n", millis());
}

void SleepActivity::renderBlankSleepScreen() const {
  const bool wasDarkMode = renderer.isDarkModeEnabled();
  renderer.setDarkModeEnabled(false);
  renderer.clearScreen();
  renderer.displayBuffer(EInkDisplay::HALF_REFRESH);
  renderer.setDarkModeEnabled(wasDarkMode);
}
