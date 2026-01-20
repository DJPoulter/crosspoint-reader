#include "GfxRenderer.h"

#include <SDCardManager.h>
#include <SdFat.h>
#include <Utf8.h>

#include "../../src/fontIds.h"

// GfxRendererDrawer implementation
GfxRendererDrawer::GfxRendererDrawer(GfxRenderer* r, bool b) : renderer(r), black(b) {}

void GfxRendererDrawer::drawPixel(int32_t x, int32_t y, uint16_t color) {
  // OpenFontRender passes color, but we only care about black/white
  // For e-ink, we use the 'black' parameter
  renderer->drawPixel(x, y, black);
}

void GfxRendererDrawer::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
  // Draw horizontal line using drawPixel
  for (int32_t i = 0; i < w; i++) {
    renderer->drawPixel(x + i, y, black);
  }
}

void GfxRendererDrawer::startWrite() {
  // No-op for e-ink display
}

void GfxRendererDrawer::endWrite() {
  // No-op for e-ink display
}

void GfxRenderer::insertFont(const int fontId, EpdFontFamily font) { fontMap.insert({fontId, font}); }

void GfxRenderer::rotateCoordinates(const int x, const int y, int* rotatedX, int* rotatedY) const {
  switch (orientation) {
    case Portrait: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees clockwise
      *rotatedX = y;
      *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - x;
      break;
    }
    case LandscapeClockwise: {
      // Logical landscape (800x480) rotated 180 degrees (swap top/bottom and left/right)
      *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - x;
      *rotatedY = EInkDisplay::DISPLAY_HEIGHT - 1 - y;
      break;
    }
    case PortraitInverted: {
      // Logical portrait (480x800) → panel (800x480)
      // Rotation: 90 degrees counter-clockwise
      *rotatedX = EInkDisplay::DISPLAY_WIDTH - 1 - y;
      *rotatedY = x;
      break;
    }
    case LandscapeCounterClockwise: {
      // Logical landscape (800x480) aligned with panel orientation
      *rotatedX = x;
      *rotatedY = y;
      break;
    }
  }
}

// Helper function to swap pixel values for dark mode: 0↔3 (black↔white), 1↔2 (dark grey↔light grey)
static inline uint8_t swapPixelValueForDarkMode(uint8_t val) {
  if (val == 0) return 3;  // black → white
  if (val == 3) return 0;  // white → black
  if (val == 1) return 2;  // dark grey → light grey
  if (val == 2) return 1;  // light grey → dark grey
  return val;  // Should never happen, but return unchanged
}

void GfxRenderer::drawPixel(const int x, const int y, const bool state) const {
  uint8_t* frameBuffer = einkDisplay.getFrameBuffer();

  // Early return if no framebuffer is set
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer\n", millis());
    return;
  }

  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(x, y, &rotatedX, &rotatedY);

  // Bounds checking against physical panel dimensions
  if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
      rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
    Serial.printf("[%lu] [GFX] !! Outside range (%d, %d) -> (%d, %d)\n", millis(), x, y, rotatedX, rotatedY);
    return;
  }

  // Calculate byte position and bit position
  const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
  const uint8_t bitPosition = 7 - (rotatedX % 8);  // MSB first

  // Note: Dark mode inversion is handled at the drawing primitive level (renderChar, drawLine, etc.)
  // For grayscale modes, state=false is used to mark pixels for grayscale rendering
  if (state) {
    frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit (black)
  } else {
    frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit (white or grayscale mark)
  }
}

int GfxRenderer::getTextWidth(const int fontId, const char* text, const EpdFontFamily::Style style) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  int w = 0, h = 0;
  fontMap.at(fontId).getTextDimensions(text, &w, &h, style);
  return w;
}

void GfxRenderer::drawCenteredText(const int fontId, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int x = (getScreenWidth() - getTextWidth(fontId, text, style)) / 2;
  drawText(fontId, x, y, text, black, style);
}

void GfxRenderer::drawText(const int fontId, const int x, const int y, const char* text, const bool black,
                           const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;

  // cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // no printable characters
  if (!font.hasPrintableChars(text, style)) {
    return;
  }

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    renderChar(font, cp, &xpos, &yPos, black, style);
  }
}

void GfxRenderer::drawTextScaled2x(const int fontId, const int x, const int y, const char* text, const bool black,
                                   const EpdFontFamily::Style style) const {
  const int yPos = y + getFontAscenderSize(fontId);
  int xpos = x;

  if (text == nullptr || *text == '\0') return;
  if (fontMap.count(fontId) == 0) return;

  const auto& font = fontMap.at(fontId);
  if (!font.hasPrintableChars(text, style)) return;

  bool actualBlack = black;
  if (darkModeEnabled && renderMode == BW) {
    actualBlack = !black;
  }

  uint32_t cp;
  const uint8_t* textPtr = reinterpret_cast<const uint8_t*>(text);
  
  const int scale = 3; 

  const auto* fontData = font.getData(style);
  if (!fontData) return;
  const int is2Bit = fontData->is2Bit;
  const uint8_t* baseBitmap = fontData->bitmap;

  while ((cp = utf8NextCodepoint(&textPtr))) {
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) glyph = font.getGlyph('?', style);
    if (!glyph) continue;

    const uint32_t offset = glyph->dataOffset;
    const uint8_t width = glyph->width;
    const uint8_t height = glyph->height;
    const int left = glyph->left;
    const int top = glyph->top;
    
    const uint8_t* bitmap = &baseBitmap[offset];
    
    const int maxByteIndex = is2Bit ? ((width * height + 3) / 4) : ((width * height + 7) / 8);

    if (bitmap != nullptr) {
      for (int y = 0; y < height; y++) {
        for (int x = 0; x < width; x++) {
          const int pPos = y * width + x;
          const int bIndex = is2Bit ? (pPos / 4) : (pPos / 8);
          
          if (bIndex >= maxByteIndex) continue;
          
          bool shouldDraw = false;
          
          if (is2Bit) {
            const uint8_t b = bitmap[bIndex];
            const uint8_t shift = (3 - pPos % 4) * 2;
            uint8_t val = 3 - ((b >> shift) & 0x3);
            if (darkModeEnabled) val = swapPixelValueForDarkMode(val);
            if (renderMode == BW) {
              const uint8_t skipColor = darkModeEnabled ? 0 : 3;
              shouldDraw = (val != skipColor);
            } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
              shouldDraw = true;
            } else if (renderMode == GRAYSCALE_LSB && val == 1) {
              shouldDraw = true;
            }
          } else {
            const uint8_t b = bitmap[bIndex];
            const uint8_t bit_index = 7 - (pPos % 8);
            shouldDraw = ((b >> bit_index) & 1) != 0;
          }
          
          if (shouldDraw) {
            // Simple 3x scaling: draw each pixel as a 3x3 block
            for (int sy = 0; sy < scale; sy++) {
              for (int sx = 0; sx < scale; sx++) {
                drawPixel(xpos + left * scale + (x * scale + sx),
                          yPos - top * scale + (y * scale + sy),
                          actualBlack);
              }
            }
          }
        }
      }
    }
    xpos += glyph->advanceX * scale;
  }
}

void GfxRenderer::drawLine(int x1, int y1, int x2, int y2, const bool state) const {
  // Invert color for dark mode in BW mode
  bool actualState = state;
  if (darkModeEnabled && renderMode == BW) {
    actualState = !state;
  }

  if (x1 == x2) {
    if (y2 < y1) {
      std::swap(y1, y2);
    }
    for (int y = y1; y <= y2; y++) {
      drawPixel(x1, y, actualState);
    }
  } else if (y1 == y2) {
    if (x2 < x1) {
      std::swap(x1, x2);
    }
    for (int x = x1; x <= x2; x++) {
      drawPixel(x, y1, actualState);
    }
  } else {
    // TODO: Implement
    Serial.printf("[%lu] [GFX] Line drawing not supported\n", millis());
  }
}

void GfxRenderer::drawRect(const int x, const int y, const int width, const int height, const bool state) const {
  drawLine(x, y, x + width - 1, y, state);
  drawLine(x + width - 1, y, x + width - 1, y + height - 1, state);
  drawLine(x + width - 1, y + height - 1, x, y + height - 1, state);
  drawLine(x, y, x, y + height - 1, state);
}

void GfxRenderer::fillRect(const int x, const int y, const int width, const int height, const bool state) const {
  for (int fillY = y; fillY < y + height; fillY++) {
    drawLine(x, fillY, x + width - 1, fillY, state);
  }
}

void GfxRenderer::drawImage(const uint8_t bitmap[], const int x, const int y, const int width, const int height) const {
  // TODO: Rotate bits
  int rotatedX = 0;
  int rotatedY = 0;
  rotateCoordinates(x, y, &rotatedX, &rotatedY);
  einkDisplay.drawImage(bitmap, rotatedX, rotatedY, width, height);
}

void GfxRenderer::drawBitmap(const Bitmap& bitmap, const int x, const int y, const int maxWidth, const int maxHeight,
                             const float cropX, const float cropY) const {
  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(bitmap.getWidth() * cropX / 2.0f);
  int cropPixY = std::floor(bitmap.getHeight() * cropY / 2.0f);

  if (maxWidth > 0 && (1.0f - cropX) * bitmap.getWidth() > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>((1.0f - cropX) * bitmap.getWidth());
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * bitmap.getHeight() > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>((1.0f - cropY) * bitmap.getHeight()));
    isScaled = true;
  }

  // Calculate output row size (2 bits per pixel, packed into bytes)
  // IMPORTANT: Use int, not uint8_t, to avoid overflow for images > 1020 pixels wide
  const int outputRowSize = (bitmap.getWidth() + 3) / 4;
  const int rowBytes = bitmap.getRowBytes();
  
  // Try large buffer first (64 rows), fall back to smaller if needed
  int ROWS_PER_READ = 64;
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* multiRowBuffer = static_cast<uint8_t*>(malloc(rowBytes * ROWS_PER_READ));

  // If large buffer fails, try smaller buffers (16, 8, 4, 1 rows)
  if (!outputRow || !multiRowBuffer) {
    free(outputRow);
    free(multiRowBuffer);
    
    // Try progressively smaller buffers
    const int fallbackSizes[] = {16, 8, 4, 1};
    bool allocated = false;
    for (int fallbackSize : fallbackSizes) {
      outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
      multiRowBuffer = static_cast<uint8_t*>(malloc(rowBytes * fallbackSize));
      if (outputRow && multiRowBuffer) {
        ROWS_PER_READ = fallbackSize;
        allocated = true;
        break;
      }
      free(outputRow);
      free(multiRowBuffer);
    }
    
    if (!allocated) {
      return;
    }
  }

  const int totalRows = bitmap.getHeight() - cropPixY;
  int bmpY = 0;
  
  while (bmpY < totalRows) {
    // Calculate how many rows to read in this batch (up to ROWS_PER_READ)
    const int rowsToRead = std::min(ROWS_PER_READ, totalRows - bmpY);
    
    const int rowsRead = bitmap.readMultipleRows(multiRowBuffer, rowsToRead);
    
    if (rowsRead <= 0) {
      break;
    }
    
    // Process each row from the batch buffer
    for (int batchRow = 0; batchRow < rowsRead; batchRow++) {
      const int currentBmpY = bmpY + batchRow;
      
      // Process this row from the raw buffer
      const uint8_t* rawRowBuffer = multiRowBuffer + (batchRow * rowBytes);
      if (bitmap.processRowFromBuffer(outputRow, rawRowBuffer) != BmpReaderError::Ok) {
        continue;
      }
      
    // The BMP's (0, 0) is the bottom-left corner (if the height is positive, top-left if negative).
    // Screen's (0, 0) is the top-left corner.
      int screenY = -cropPixY + (bitmap.isTopDown() ? currentBmpY : bitmap.getHeight() - 1 - currentBmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;  // the offset should not be scaled
    if (screenY >= getScreenHeight()) {
        bmpY = totalRows;  // Exit outer loop
      break;
    }

      if (currentBmpY < cropPixY) {
      // Skip the row if it's outside the crop area
      continue;
    }

      // Optimized: Get framebuffer pointer once and write directly
      uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
      if (!frameBuffer) continue;

      // Optimized: Calculate rotation once per row based on orientation
      // This avoids calling rotateCoordinates for every pixel (saves 610ms!)
      int rotatedX_base = 0;
      int rotatedY_base = 0;
      int xOffset = 0;
      int yOffset = 0;
      
      // Calculate rotation formula based on orientation
      switch (orientation) {
        case Portrait: {
          // rotatedX = screenY (constant), rotatedY = DISPLAY_HEIGHT - 1 - screenX
          rotatedX_base = screenY;
          rotatedY_base = EInkDisplay::DISPLAY_HEIGHT - 1;
          xOffset = 0;   // rotatedX is constant
          yOffset = -1;  // rotatedY decreases as screenX increases
          break;
        }
        case LandscapeClockwise: {
          // rotatedX = DISPLAY_WIDTH - 1 - screenX, rotatedY = DISPLAY_HEIGHT - 1 - screenY
          rotatedX_base = EInkDisplay::DISPLAY_WIDTH - 1;
          rotatedY_base = EInkDisplay::DISPLAY_HEIGHT - 1 - screenY;
          xOffset = -1;  // rotatedX decreases as screenX increases
          yOffset = 0;   // rotatedY is constant
          break;
        }
        case PortraitInverted: {
          // rotatedX = DISPLAY_WIDTH - 1 - screenY, rotatedY = screenX
          rotatedX_base = EInkDisplay::DISPLAY_WIDTH - 1 - screenY;
          rotatedY_base = 0;
          xOffset = 0;   // rotatedX is constant
          yOffset = 1;   // rotatedY increases as screenX increases
          break;
        }
        case LandscapeCounterClockwise: {
          // rotatedX = screenX, rotatedY = screenY (no rotation)
          rotatedX_base = 0;
          rotatedY_base = screenY;
          xOffset = 1;   // rotatedX increases as screenX increases
          yOffset = 0;   // rotatedY is constant
          break;
        }
      }

      // Process pixels from the processed row data
      for (int bmpX = cropPixX; bmpX < bitmap.getWidth() - cropPixX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;  // the offset should not be scaled
      if (screenX >= getScreenWidth()) {
        break;
      }

      uint8_t val = outputRow[bmpX / 4] >> (6 - ((bmpX * 2) % 8)) & 0x3;
      
      // Swap pixel values for dark mode: 0↔3, 1↔2
      if (darkModeEnabled) {
        val = swapPixelValueForDarkMode(val);
      }

        // Optimized: Calculate rotated coordinates without function call
        int rotatedX = rotatedX_base + (xOffset * screenX);
        int rotatedY = rotatedY_base + (yOffset * screenX);

        // Bounds check
        if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
            rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
          continue;
        }

        // Calculate byte position and bit position
        const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
        const uint8_t bitPosition = 7 - (rotatedX % 8);  // MSB first

        // Write directly to framebuffer (avoiding drawPixel overhead)
        if (renderMode == BW) {
          // Determine the "background" color we should skip drawing
          // Light mode: Skip 3 (White). Dark mode: Skip 0 (Black).
          const uint8_t skipColor = darkModeEnabled ? 0 : 3;
          
          if (val != skipColor) {
            // Map val to pixel state: 0/1 -> Black (clear bit), 2/3 -> White (set bit)
            if (val < 2) {
              frameBuffer[byteIndex] &= ~(1 << bitPosition);  // Clear bit (black)
            } else {
              frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit (white)
            }
          }
        } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
          frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit (mark for grayscale)
        } else if (renderMode == GRAYSCALE_LSB && val == 1) {
          frameBuffer[byteIndex] |= 1 << bitPosition;  // Set bit (mark for dark gray)
        }
      }
    }
    
    bmpY += rowsRead;
  }

  free(outputRow);
  free(multiRowBuffer);
}

uint8_t* GfxRenderer::cacheBitmap(const Bitmap& bitmap, int* outWidth, int* outHeight, int* outRowSize) const {
  const int width = bitmap.getWidth();
  const int height = bitmap.getHeight();
  const int rowBytes = bitmap.getRowBytes();
  const int outputRowSize = (width + 3) / 4;  // 2 bits per pixel, packed into bytes
  
  const size_t cacheSize = static_cast<size_t>(outputRowSize) * height;
  
  // Allocate cache buffer FIRST (when we have the most contiguous memory)
  // This avoids fragmentation from temp buffer allocation
  uint8_t* cachedData = static_cast<uint8_t*>(malloc(cacheSize));
  if (!cachedData) {
    return nullptr;
  }
  
  // Now allocate minimal temp buffers (single row only to reduce memory pressure)
  // After cache allocation, we have less contiguous memory, so use row-by-row reading
  auto* outputRow = static_cast<uint8_t*>(malloc(outputRowSize));
  auto* singleRowBuffer = static_cast<uint8_t*>(malloc(rowBytes));  // Only one row buffer (~1.4KB)
  
  if (!outputRow || !singleRowBuffer) {
    free(cachedData);  // Free cache since we can't use it
    free(outputRow);
    free(singleRowBuffer);
    return nullptr;
  }
  
  // Rewind to start of pixel data
  if (bitmap.rewindToData() != BmpReaderError::Ok) {
    free(cachedData);
    free(outputRow);
    free(singleRowBuffer);
    return nullptr;
  }
  
  // Read and process all rows one at a time (minimal memory usage)
  bool cacheSuccess = true;
  for (int bmpY = 0; bmpY < height; bmpY++) {
    // Read one row at a time using readNextRow (which handles row buffer internally)
    if (bitmap.readNextRow(outputRow, singleRowBuffer) != BmpReaderError::Ok) {
      cacheSuccess = false;
      break;
    }
    
    // Store processed row in cache
    uint8_t* cacheRow = cachedData + (bmpY * outputRowSize);
    memcpy(cacheRow, outputRow, outputRowSize);
  }
  
  // Clean up temporary buffers
  free(outputRow);
  free(singleRowBuffer);
  
  if (!cacheSuccess) {
    free(cachedData);
    return nullptr;
  }
  
  *outWidth = width;
  *outHeight = height;
  *outRowSize = outputRowSize;
  return cachedData;
}

void GfxRenderer::drawCachedBitmap(const uint8_t* cachedData, int cachedWidth, int cachedHeight, int cachedRowSize,
                                   int x, int y, int maxWidth, int maxHeight, float cropX, float cropY, bool isTopDown) const {
  if (!cachedData) {
    return;
  }
  
  float scale = 1.0f;
  bool isScaled = false;
  int cropPixX = std::floor(cachedWidth * cropX / 2.0f);
  int cropPixY = std::floor(cachedHeight * cropY / 2.0f);
  
  if (maxWidth > 0 && (1.0f - cropX) * cachedWidth > maxWidth) {
    scale = static_cast<float>(maxWidth) / static_cast<float>((1.0f - cropX) * cachedWidth);
    isScaled = true;
  }
  if (maxHeight > 0 && (1.0f - cropY) * cachedHeight > maxHeight) {
    scale = std::min(scale, static_cast<float>(maxHeight) / static_cast<float>((1.0f - cropY) * cachedHeight));
    isScaled = true;
  }
  
  const int totalRows = cachedHeight - cropPixY;
  uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  if (!frameBuffer) return;
  
  // Process each row from cache (no SD card reads!)
  for (int bmpY = cropPixY; bmpY < cachedHeight; bmpY++) {
    // Get cached row data
    const uint8_t* cachedRow = cachedData + (bmpY * cachedRowSize);
    
    // Fast row-level check: skip entire row if it contains no pixels we need
    if (renderMode == GRAYSCALE_LSB || renderMode == GRAYSCALE_MSB) {
      bool rowHasNeededPixels = false;
      const int rowBytes = cachedRowSize;
      
      // Quick scan: check if any byte contains pixels we need
      for (int byteIdx = 0; byteIdx < rowBytes; byteIdx++) {
        const uint8_t packedByte = cachedRow[byteIdx];
        
        if (renderMode == GRAYSCALE_LSB) {
          // Need val == 1 (binary 01 = 0b01)
          // Check all 4 pixel values in the byte
          if ((packedByte & 0xC0) == 0x40 ||  // val0 == 01 (bits 7-6: 01)
              (packedByte & 0x30) == 0x10 ||  // val1 == 01 (bits 5-4: 01)
              (packedByte & 0x0C) == 0x04 ||  // val2 == 01 (bits 3-2: 01)
              (packedByte & 0x03) == 0x01) {  // val3 == 01 (bits 1-0: 01)
            rowHasNeededPixels = true;
            break;
          }
        } else if (renderMode == GRAYSCALE_MSB) {
          // Need val == 1 (01) or val == 2 (10)
          // Check all 4 pixel values in the byte
          const uint8_t val0 = (packedByte >> 6) & 0x3;
          const uint8_t val1 = (packedByte >> 4) & 0x3;
          const uint8_t val2 = (packedByte >> 2) & 0x3;
          const uint8_t val3 = packedByte & 0x3;
          
          if (val0 == 1 || val0 == 2 || val1 == 1 || val1 == 2 ||
              val2 == 1 || val2 == 2 || val3 == 1 || val3 == 2) {
            rowHasNeededPixels = true;
            break;
          }
        }
      }
      
      if (!rowHasNeededPixels) {
        continue;  // Skip entire row - no pixels we need here
      }
    }
    
    // Calculate screen Y coordinate (reverse for bottom-up bitmaps, same as drawBitmap)
    int screenY = -cropPixY + (isTopDown ? bmpY : cachedHeight - 1 - bmpY);
    if (isScaled) {
      screenY = std::floor(screenY * scale);
    }
    screenY += y;
    if (screenY >= getScreenHeight()) {
      break;
    }
    
    // Calculate rotation base values for this row
    int rotatedX_base = 0;
    int rotatedY_base = 0;
    int xOffset = 0;
    int yOffset = 0;
    
    switch (orientation) {
      case Portrait: {
        rotatedX_base = screenY;
        rotatedY_base = EInkDisplay::DISPLAY_HEIGHT - 1;
        xOffset = 0;
        yOffset = -1;
        break;
      }
      case LandscapeClockwise: {
        rotatedX_base = EInkDisplay::DISPLAY_WIDTH - 1;
        rotatedY_base = EInkDisplay::DISPLAY_HEIGHT - 1 - screenY;
        xOffset = -1;
        yOffset = 0;
        break;
      }
      case PortraitInverted: {
        rotatedX_base = EInkDisplay::DISPLAY_WIDTH - 1 - screenY;
        rotatedY_base = 0;
        xOffset = 0;
        yOffset = 1;
        break;
      }
      case LandscapeCounterClockwise: {
        rotatedX_base = 0;
        rotatedY_base = screenY;
        xOffset = 1;
        yOffset = 0;
        break;
      }
    }
    
    // Process pixels from cached row - optimized to process 4 pixels at a time
    const int rowStartX = cropPixX;
    const int rowEndX = cachedWidth - cropPixX;
    
    // Align start to byte boundary for 4-pixel processing
    int bmpX = rowStartX;
    const int byteStart = bmpX / 4;
    const int pixelOffsetInByte = bmpX % 4;
    
    // Process any leading pixels that don't align to byte boundary
    for (int i = 0; i < pixelOffsetInByte && bmpX < rowEndX; i++, bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;
      if (screenX >= getScreenWidth()) {
        bmpX = rowEndX;  // Exit
        break;
      }
      
      const uint8_t packedByte = cachedRow[bmpX / 4];
      const uint8_t val = (packedByte >> (6 - ((bmpX * 2) % 8))) & 0x3;
      
      // Early skip if pixel value doesn't match render mode
      if (renderMode == GRAYSCALE_LSB && val != 1) continue;
      if (renderMode == GRAYSCALE_MSB && val != 1 && val != 2) continue;
      if (renderMode == BW && val == 3) continue;
      
      int rotatedX = rotatedX_base + (xOffset * screenX);
      int rotatedY = rotatedY_base + (yOffset * screenX);
      
      if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
          rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
        continue;
      }
      
      const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
      const uint8_t bitPosition = 7 - (rotatedX % 8);
      
      if (renderMode == BW && val < 3) {
        frameBuffer[byteIndex] &= ~(1 << bitPosition);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        frameBuffer[byteIndex] |= 1 << bitPosition;
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        frameBuffer[byteIndex] |= 1 << bitPosition;
      }
    }
    
    // Process 4 pixels at a time (one byte) for the aligned portion
    for (int byteIdx = byteStart + (pixelOffsetInByte > 0 ? 1 : 0); bmpX + 3 < rowEndX; byteIdx++, bmpX += 4) {
      const uint8_t packedByte = cachedRow[byteIdx];
      
      // Fast byte-level check: skip entire byte if it contains no pixels we need
      if (renderMode == GRAYSCALE_LSB) {
        // For LSB, we only need val == 1 (binary 01)
        // Check if byte contains any 01 patterns: (packedByte & 0xAA) == 0xAA means all bits set, but we need 01
        // Better: check if any of the 2-bit pairs is 01 (0b01 = 1)
        // 01 pattern: bit7=0, bit6=1 OR bit5=0, bit4=1 OR bit3=0, bit2=1 OR bit1=0, bit0=1
        // This is complex, so just extract and check
      } else if (renderMode == GRAYSCALE_MSB) {
        // For MSB, we need val == 1 (01) or val == 2 (10)
        // 01 = 0b01, 10 = 0b10
        // If byte is 0x00 or 0xFF, skip (but 0xFF = all 11, which is white, so skip)
        if (packedByte == 0xFF) continue;  // All white pixels, skip
      } else if (renderMode == BW) {
        // For BW, we need val < 3 (not white)
        // If byte is 0xFF, all white, skip
        if (packedByte == 0xFF) continue;  // All white pixels, skip
      }
      
      // Extract all 4 pixel values at once
      const uint8_t val0 = (packedByte >> 6) & 0x3;
      const uint8_t val1 = (packedByte >> 4) & 0x3;
      const uint8_t val2 = (packedByte >> 2) & 0x3;
      const uint8_t val3 = packedByte & 0x3;
      
      // Process each of the 4 pixels
      for (int i = 0; i < 4; i++) {
        const uint8_t val = (i == 0) ? val0 : (i == 1) ? val1 : (i == 2) ? val2 : val3;
        
        // Early skip if pixel value doesn't match render mode
        if (renderMode == GRAYSCALE_LSB && val != 1) continue;
        if (renderMode == GRAYSCALE_MSB && val != 1 && val != 2) continue;
        if (renderMode == BW && val == 3) continue;
        
        int screenX = (bmpX + i) - cropPixX;
        if (isScaled) {
          screenX = std::floor(screenX * scale);
        }
        screenX += x;
        if (screenX >= getScreenWidth()) {
          bmpX = rowEndX;  // Exit outer loop
          break;
        }
        
        int rotatedX = rotatedX_base + (xOffset * screenX);
        int rotatedY = rotatedY_base + (yOffset * screenX);
        
        if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
            rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
          continue;
        }
        
        const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
        const uint8_t bitPosition = 7 - (rotatedX % 8);
        
        if (renderMode == BW && val < 3) {
          frameBuffer[byteIndex] &= ~(1 << bitPosition);
        } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
          frameBuffer[byteIndex] |= 1 << bitPosition;
        } else if (renderMode == GRAYSCALE_LSB && val == 1) {
          frameBuffer[byteIndex] |= 1 << bitPosition;
        }
      }
      
      if (bmpX >= rowEndX) break;
    }
    
    // Process any trailing pixels
    for (; bmpX < rowEndX; bmpX++) {
      int screenX = bmpX - cropPixX;
      if (isScaled) {
        screenX = std::floor(screenX * scale);
      }
      screenX += x;
      if (screenX >= getScreenWidth()) {
        break;
      }
      
      const uint8_t packedByte = cachedRow[bmpX / 4];
      const uint8_t val = (packedByte >> (6 - ((bmpX * 2) % 8))) & 0x3;
      
      // Early skip if pixel value doesn't match render mode
      if (renderMode == GRAYSCALE_LSB && val != 1) continue;
      if (renderMode == GRAYSCALE_MSB && val != 1 && val != 2) continue;
      if (renderMode == BW && val == 3) continue;
      
      int rotatedX = rotatedX_base + (xOffset * screenX);
      int rotatedY = rotatedY_base + (yOffset * screenX);
      
      if (rotatedX < 0 || rotatedX >= EInkDisplay::DISPLAY_WIDTH || rotatedY < 0 ||
          rotatedY >= EInkDisplay::DISPLAY_HEIGHT) {
        continue;
      }
      
      const uint16_t byteIndex = rotatedY * EInkDisplay::DISPLAY_WIDTH_BYTES + (rotatedX / 8);
      const uint8_t bitPosition = 7 - (rotatedX % 8);
      
      if (renderMode == BW && val < 3) {
        frameBuffer[byteIndex] &= ~(1 << bitPosition);
      } else if (renderMode == GRAYSCALE_MSB && (val == 1 || val == 2)) {
        frameBuffer[byteIndex] |= 1 << bitPosition;
      } else if (renderMode == GRAYSCALE_LSB && val == 1) {
        frameBuffer[byteIndex] |= 1 << bitPosition;
      }
    }
  }
}

void GfxRenderer::clearScreen(const uint8_t color) const {
  // When dark mode is enabled, clear to black (0x00) instead of white (0xFF)
  // This ensures the background is black in dark mode
  uint8_t fillColor = color;
  if (darkModeEnabled && color == 0xFF) {
    fillColor = 0x00;  // Black instead of white
  }
  einkDisplay.clearScreen(fillColor);
}

void GfxRenderer::invertScreen() const {
  uint8_t* buffer = einkDisplay.getFrameBuffer();
  if (!buffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in invertScreen\n", millis());
    return;
  }
  for (int i = 0; i < EInkDisplay::BUFFER_SIZE; i++) {
    buffer[i] = ~buffer[i];
  }
}

void GfxRenderer::displayBuffer(const EInkDisplay::RefreshMode refreshMode) const {
  // Dark mode is now handled at the pixel level during rendering, so no need to invert here
  einkDisplay.displayBuffer(refreshMode);
}

std::string GfxRenderer::truncatedText(const int fontId, const char* text, const int maxWidth,
                                       const EpdFontFamily::Style style) const {
  std::string item = text;
  int itemWidth = getTextWidth(fontId, item.c_str(), style);
  while (itemWidth > maxWidth && item.length() > 8) {
    item.replace(item.length() - 5, 5, "...");
    itemWidth = getTextWidth(fontId, item.c_str(), style);
  }
  return item;
}

// Note: Internal driver treats screen in command orientation; this library exposes a logical orientation
int GfxRenderer::getScreenWidth() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 480px wide in portrait logical coordinates
      return EInkDisplay::DISPLAY_HEIGHT;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 800px wide in landscape logical coordinates
      return EInkDisplay::DISPLAY_WIDTH;
  }
  return EInkDisplay::DISPLAY_HEIGHT;
}

int GfxRenderer::getScreenHeight() const {
  switch (orientation) {
    case Portrait:
    case PortraitInverted:
      // 800px tall in portrait logical coordinates
      return EInkDisplay::DISPLAY_WIDTH;
    case LandscapeClockwise:
    case LandscapeCounterClockwise:
      // 480px tall in landscape logical coordinates
      return EInkDisplay::DISPLAY_HEIGHT;
  }
  return EInkDisplay::DISPLAY_WIDTH;
}

int GfxRenderer::getSpaceWidth(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getGlyph(' ', EpdFontFamily::REGULAR)->advanceX;
}

int GfxRenderer::getFontAscenderSize(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  const auto* fontData = fontMap.at(fontId).getData(EpdFontFamily::REGULAR);
  if (fontData == nullptr) {
    Serial.printf("[%lu] [GFX] Font %d data is null\n", millis(), fontId);
    return 0;
  }

  return fontData->ascender;
}

int GfxRenderer::getLineHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }

  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->advanceY;
}

void GfxRenderer::drawButtonHints(const int fontId, const char* btn1, const char* btn2, const char* btn3,
                                  const char* btn4) const {
  const int pageHeight = getScreenHeight();
  constexpr int buttonWidth = 106;
  constexpr int buttonHeight = 40;
  constexpr int buttonY = 40;     // Distance from bottom
  constexpr int textYOffset = 7;  // Distance from top of button to text baseline
  constexpr int buttonPositions[] = {25, 130, 245, 350};
  const char* labels[] = {btn1, btn2, btn3, btn4};

  for (int i = 0; i < 4; i++) {
    // Only draw if the label is non-empty
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int x = buttonPositions[i];
      drawRect(x, pageHeight - buttonY, buttonWidth, buttonHeight);
      const int textWidth = getTextWidth(fontId, labels[i]);
      const int textX = x + (buttonWidth - 1 - textWidth) / 2;
      drawText(fontId, textX, pageHeight - buttonY + textYOffset, labels[i]);
    }
  }
}

void GfxRenderer::drawSideButtonHints(const int fontId, const char* topBtn, const char* bottomBtn) const {
  const int screenWidth = getScreenWidth();
  constexpr int buttonWidth = 40;   // Width on screen (height when rotated)
  constexpr int buttonHeight = 80;  // Height on screen (width when rotated)
  constexpr int buttonX = 5;        // Distance from right edge
  // Position for the button group - buttons share a border so they're adjacent
  constexpr int topButtonY = 345;  // Top button position

  const char* labels[] = {topBtn, bottomBtn};

  // Draw the shared border for both buttons as one unit
  const int x = screenWidth - buttonX - buttonWidth;

  // Draw top button outline (3 sides, bottom open)
  if (topBtn != nullptr && topBtn[0] != '\0') {
    drawLine(x, topButtonY, x + buttonWidth - 1, topButtonY);                                       // Top
    drawLine(x, topButtonY, x, topButtonY + buttonHeight - 1);                                      // Left
    drawLine(x + buttonWidth - 1, topButtonY, x + buttonWidth - 1, topButtonY + buttonHeight - 1);  // Right
  }

  // Draw shared middle border
  if ((topBtn != nullptr && topBtn[0] != '\0') || (bottomBtn != nullptr && bottomBtn[0] != '\0')) {
    drawLine(x, topButtonY + buttonHeight, x + buttonWidth - 1, topButtonY + buttonHeight);  // Shared border
  }

  // Draw bottom button outline (3 sides, top is shared)
  if (bottomBtn != nullptr && bottomBtn[0] != '\0') {
    drawLine(x, topButtonY + buttonHeight, x, topButtonY + 2 * buttonHeight - 1);  // Left
    drawLine(x + buttonWidth - 1, topButtonY + buttonHeight, x + buttonWidth - 1,
             topButtonY + 2 * buttonHeight - 1);                                                             // Right
    drawLine(x, topButtonY + 2 * buttonHeight - 1, x + buttonWidth - 1, topButtonY + 2 * buttonHeight - 1);  // Bottom
  }

  // Draw text for each button
  for (int i = 0; i < 2; i++) {
    if (labels[i] != nullptr && labels[i][0] != '\0') {
      const int y = topButtonY + i * buttonHeight;

      // Draw rotated text centered in the button
      const int textWidth = getTextWidth(fontId, labels[i]);
      const int textHeight = getTextHeight(fontId);

      // Center the rotated text in the button
      const int textX = x + (buttonWidth - textHeight) / 2;
      const int textY = y + (buttonHeight + textWidth) / 2;

      drawTextRotated90CW(fontId, textX, textY, labels[i]);
    }
  }
}

int GfxRenderer::getTextHeight(const int fontId) const {
  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return 0;
  }
  return fontMap.at(fontId).getData(EpdFontFamily::REGULAR)->ascender;
}

void GfxRenderer::drawTextRotated90CW(const int fontId, const int x, const int y, const char* text, const bool black,
                                      const EpdFontFamily::Style style) const {
  // Cannot draw a NULL / empty string
  if (text == nullptr || *text == '\0') {
    return;
  }

  if (fontMap.count(fontId) == 0) {
    Serial.printf("[%lu] [GFX] Font %d not found\n", millis(), fontId);
    return;
  }
  const auto font = fontMap.at(fontId);

  // No printable characters
  if (!font.hasPrintableChars(text, style)) {
    return;
  }

  // For 90° clockwise rotation:
  // Original (glyphX, glyphY) -> Rotated (glyphY, -glyphX)
  // Text reads from bottom to top

  int yPos = y;  // Current Y position (decreases as we draw characters)

  uint32_t cp;
  while ((cp = utf8NextCodepoint(reinterpret_cast<const uint8_t**>(&text)))) {
    const EpdGlyph* glyph = font.getGlyph(cp, style);
    if (!glyph) {
      glyph = font.getGlyph('?', style);
    }
    if (!glyph) {
      continue;
    }

    const int is2Bit = font.getData(style)->is2Bit;
    const uint32_t offset = glyph->dataOffset;
    const uint8_t width = glyph->width;
    const uint8_t height = glyph->height;
    const int left = glyph->left;
    const int top = glyph->top;

    const uint8_t* bitmap = &font.getData(style)->bitmap[offset];

    if (bitmap != nullptr) {
      for (int glyphY = 0; glyphY < height; glyphY++) {
        for (int glyphX = 0; glyphX < width; glyphX++) {
          const int pixelPosition = glyphY * width + glyphX;

          // 90° clockwise rotation transformation:
          // screenX = x + (ascender - top + glyphY)
          // screenY = yPos - (left + glyphX)
          const int screenX = x + (font.getData(style)->ascender - top + glyphY);
          const int screenY = yPos - left - glyphX;

          // Invert black parameter for dark mode (used by both 1-bit and 2-bit fonts)
          bool actualBlack = black;
          if (darkModeEnabled && renderMode == BW) {
            actualBlack = !black;
          }

          if (is2Bit) {
            const uint8_t byte = bitmap[pixelPosition / 4];
            const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
            uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;
            
            // Swap pixel values for dark mode: 0↔3, 1↔2
            if (darkModeEnabled) {
              bmpVal = swapPixelValueForDarkMode(bmpVal);
            }

            if (renderMode == BW) {
              // Determine the "background" color we should skip drawing
              // Light mode: Skip 3 (White). Dark mode: Skip 0 (Black).
              const uint8_t skipColor = darkModeEnabled ? 0 : 3;
              
              if (bmpVal != skipColor) {
                // For non-background pixels, use the text color (actualBlack)
                // which is already inverted for dark mode
                drawPixel(screenX, screenY, actualBlack);
              }
            } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
              drawPixel(screenX, screenY, false);
            } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
              drawPixel(screenX, screenY, false);
            }
          } else {
            const uint8_t byte = bitmap[pixelPosition / 8];
            const uint8_t bit_index = 7 - (pixelPosition % 8);

            if ((byte >> bit_index) & 1) {
              drawPixel(screenX, screenY, actualBlack);
            }
          }
        }
      }
    }

    // Move to next character position (going up, so decrease Y)
    yPos -= glyph->advanceX;
  }
}

uint8_t* GfxRenderer::getFrameBuffer() const { return einkDisplay.getFrameBuffer(); }

size_t GfxRenderer::getBufferSize() { return EInkDisplay::BUFFER_SIZE; }

void GfxRenderer::grayscaleRevert() const { einkDisplay.grayscaleRevert(); }

void GfxRenderer::copyGrayscaleLsbBuffers() const { einkDisplay.copyGrayscaleLsbBuffers(einkDisplay.getFrameBuffer()); }

void GfxRenderer::copyGrayscaleMsbBuffers() const { einkDisplay.copyGrayscaleMsbBuffers(einkDisplay.getFrameBuffer()); }

void GfxRenderer::displayGrayBuffer() const { einkDisplay.displayGrayBuffer(); }

void GfxRenderer::freeBwBufferChunks() {
  for (auto& bwBufferChunk : bwBufferChunks) {
    if (bwBufferChunk) {
      free(bwBufferChunk);
      bwBufferChunk = nullptr;
    }
  }
}

/**
 * This should be called before grayscale buffers are populated.
 * A `restoreBwBuffer` call should always follow the grayscale render if this method was called.
 * Uses chunked allocation to avoid needing 48KB of contiguous memory.
 * Returns true if buffer was stored successfully, false if allocation failed.
 */
bool GfxRenderer::storeBwBuffer() {
  const uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in storeBwBuffer\n", millis());
    return false;
  }

  // Allocate and copy each chunk
  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if any chunks are already allocated
    if (bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! BW buffer chunk %zu already stored - this is likely a bug, freeing chunk\n",
                    millis(), i);
      free(bwBufferChunks[i]);
      bwBufferChunks[i] = nullptr;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    bwBufferChunks[i] = static_cast<uint8_t*>(malloc(BW_BUFFER_CHUNK_SIZE));

    if (!bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! Failed to allocate BW buffer chunk %zu (%zu bytes)\n", millis(), i,
                    BW_BUFFER_CHUNK_SIZE);
      // Free previously allocated chunks
      freeBwBufferChunks();
      return false;
    }

    memcpy(bwBufferChunks[i], frameBuffer + offset, BW_BUFFER_CHUNK_SIZE);
  }

  Serial.printf("[%lu] [GFX] Stored BW buffer in %zu chunks (%zu bytes each)\n", millis(), BW_BUFFER_NUM_CHUNKS,
                BW_BUFFER_CHUNK_SIZE);
  return true;
}

/**
 * This can only be called if `storeBwBuffer` was called prior to the grayscale render.
 * It should be called to restore the BW buffer state after grayscale rendering is complete.
 * Uses chunked restoration to match chunked storage.
 */
void GfxRenderer::restoreBwBuffer() {
  // Check if any all chunks are allocated
  bool missingChunks = false;
  for (const auto& bwBufferChunk : bwBufferChunks) {
    if (!bwBufferChunk) {
      missingChunks = true;
      break;
    }
  }

  if (missingChunks) {
    freeBwBufferChunks();
    return;
  }

  uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  if (!frameBuffer) {
    Serial.printf("[%lu] [GFX] !! No framebuffer in restoreBwBuffer\n", millis());
    freeBwBufferChunks();
    return;
  }

  for (size_t i = 0; i < BW_BUFFER_NUM_CHUNKS; i++) {
    // Check if chunk is missing
    if (!bwBufferChunks[i]) {
      Serial.printf("[%lu] [GFX] !! BW buffer chunks not stored - this is likely a bug\n", millis());
      freeBwBufferChunks();
      return;
    }

    const size_t offset = i * BW_BUFFER_CHUNK_SIZE;
    memcpy(frameBuffer + offset, bwBufferChunks[i], BW_BUFFER_CHUNK_SIZE);
  }

  einkDisplay.cleanupGrayscaleBuffers(frameBuffer);

  freeBwBufferChunks();
  Serial.printf("[%lu] [GFX] Restored and freed BW buffer chunks\n", millis());
}

void GfxRenderer::freeBwBuffer() {
  freeBwBufferChunks();
  Serial.printf("[%lu] [GFX] Freed BW buffer chunks (48KB freed)\n", millis());
}

/**
 * Cleanup grayscale buffers using the current frame buffer.
 * Use this when BW buffer was re-rendered instead of stored/restored.
 */
void GfxRenderer::cleanupGrayscaleWithFrameBuffer() const {
  uint8_t* frameBuffer = einkDisplay.getFrameBuffer();
  if (frameBuffer) {
    einkDisplay.cleanupGrayscaleBuffers(frameBuffer);
  }
}

void GfxRenderer::renderChar(const EpdFontFamily& fontFamily, const uint32_t cp, int* x, const int* y,
                             const bool pixelState, const EpdFontFamily::Style style) const {
  const EpdGlyph* glyph = fontFamily.getGlyph(cp, style);
  if (!glyph) {
    // TODO: Replace with fallback glyph property?
    glyph = fontFamily.getGlyph('?', style);
  }

  // no glyph?
  if (!glyph) {
    Serial.printf("[%lu] [GFX] No glyph for codepoint %d\n", millis(), cp);
    return;
  }

  const int is2Bit = fontFamily.getData(style)->is2Bit;
  const uint32_t offset = glyph->dataOffset;
  const uint8_t width = glyph->width;
  const uint8_t height = glyph->height;
  const int left = glyph->left;

  const uint8_t* bitmap = nullptr;
  bitmap = &fontFamily.getData(style)->bitmap[offset];

  // Invert the "ink" color for dark mode
  bool actualPixelState = pixelState;
  if (darkModeEnabled && renderMode == BW) {
    actualPixelState = !pixelState;
  }

  if (bitmap != nullptr) {
    for (int glyphY = 0; glyphY < height; glyphY++) {
      const int screenY = *y - glyph->top + glyphY;
      for (int glyphX = 0; glyphX < width; glyphX++) {
        const int pixelPosition = glyphY * width + glyphX;
        const int screenX = *x + left + glyphX;

        if (is2Bit) {
          const uint8_t byte = bitmap[pixelPosition / 4];
          const uint8_t bit_index = (3 - pixelPosition % 4) * 2;
          // the direct bit from the font is 0 -> white, 1 -> light gray, 2 -> dark gray, 3 -> black
          // we swap this to better match the way images and screen think about colors:
          // 0 -> black, 1 -> dark grey, 2 -> light grey, 3 -> white
          uint8_t bmpVal = 3 - (byte >> bit_index) & 0x3;
          
          // Swap pixel values for dark mode: 0↔3, 1↔2
          if (darkModeEnabled) {
            bmpVal = swapPixelValueForDarkMode(bmpVal);
          }

          if (renderMode == BW) {
            // Determine the "background" color we should skip drawing
            // Light mode: Skip 3 (White). Dark mode: Skip 0 (Black).
            const uint8_t skipColor = darkModeEnabled ? 0 : 3;
            
            if (bmpVal != skipColor) {
              // For non-background pixels, use the text color (actualPixelState)
              // which is already inverted for dark mode in renderChar
              drawPixel(screenX, screenY, actualPixelState);
            }
          } else if (renderMode == GRAYSCALE_MSB && (bmpVal == 1 || bmpVal == 2)) {
            // Light gray (also mark the MSB if it's going to be a dark gray too)
            // We have to flag pixels in reverse for the gray buffers, as 0 leave alone, 1 update
            drawPixel(screenX, screenY, false);
          } else if (renderMode == GRAYSCALE_LSB && bmpVal == 1) {
            // Dark gray
            drawPixel(screenX, screenY, false);
          }
        } else {
          const uint8_t byte = bitmap[pixelPosition / 8];
          const uint8_t bit_index = 7 - (pixelPosition % 8);

          // For 1-bit fonts, use the inverted pixelState for dark mode
          if ((byte >> bit_index) & 1) {
            drawPixel(screenX, screenY, actualPixelState);
          }
        }
      }
    }
  }

  *x += glyph->advanceX;
}

void GfxRenderer::getOrientedViewableTRBL(int* outTop, int* outRight, int* outBottom, int* outLeft) const {
  switch (orientation) {
    case Portrait:
      *outTop = VIEWABLE_MARGIN_TOP;
      *outRight = VIEWABLE_MARGIN_RIGHT;
      *outBottom = VIEWABLE_MARGIN_BOTTOM;
      *outLeft = VIEWABLE_MARGIN_LEFT;
      break;
    case LandscapeClockwise:
      *outTop = VIEWABLE_MARGIN_LEFT;
      *outRight = VIEWABLE_MARGIN_TOP;
      *outBottom = VIEWABLE_MARGIN_RIGHT;
      *outLeft = VIEWABLE_MARGIN_BOTTOM;
      break;
    case PortraitInverted:
      *outTop = VIEWABLE_MARGIN_BOTTOM;
      *outRight = VIEWABLE_MARGIN_LEFT;
      *outBottom = VIEWABLE_MARGIN_TOP;
      *outLeft = VIEWABLE_MARGIN_RIGHT;
      break;
    case LandscapeCounterClockwise:
      *outTop = VIEWABLE_MARGIN_RIGHT;
      *outRight = VIEWABLE_MARGIN_BOTTOM;
      *outBottom = VIEWABLE_MARGIN_LEFT;
      *outLeft = VIEWABLE_MARGIN_TOP;
      break;
  }
}

void GfxRenderer::drawDropCapTTF(int x, int y, const char* text, int fontSize) {
  if (!SdMan.ready()) return;
  if (text == nullptr || *text == '\0') return;

  // 1. ALLOCATE: Open file & malloc buffer
  const char* fontPath = "/fonts/bookerly.ttf";
  FsFile f;
  if (!SdMan.openFileForRead("SD", fontPath, f)) return;
  
  size_t size = f.fileSize();
  
  // SAFETY CHECK: Reject large fonts that will crash the ESP32
  if (size > 50 * 1024) {
    f.close();
    return;
  }
  
  uint8_t* fontData = static_cast<uint8_t*>(malloc(size));
  
  if (!fontData) {
    f.close();
    return; // RAM full, skip drop cap
  }
  
  f.read(fontData, size);
  f.close();

  // 2. LOAD
  if (ofr.loadFont(fontData, size) == 0) {
    ofr.setFontSize(fontSize);
    ofr.setFontColor(0, 255); // Black on Transparent
    
    // 3. DRAW
    // Set cursor position (logical coordinates, drawPixel handles rotation)
    ofr.setCursor(x, y);
    
    // Create drawer object
    GfxRendererDrawer drawer(this, true); // true = black
    ofr.setDrawer(drawer);
    
    ofr.printf("%s", text);
    
    // 4. UNLOAD
    ofr.unloadFont(); 
  }

  // 5. FREE
  free(fontData); 
}
