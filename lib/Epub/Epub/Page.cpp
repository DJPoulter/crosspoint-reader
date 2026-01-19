#include "Page.h"

#include <EpdFontFamily.h>
#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <Serialization.h>

// Helper function to get header font ID (larger version of base font)
static int getHeaderFontId(int baseFontId) {
  // Map base font ID to a larger version for headers
  // Check if it's a Bookerly font
  if (baseFontId == -142329172) return 104246423;  // 12 -> 14
  if (baseFontId == 104246423) return 1909382491;   // 14 -> 16
  if (baseFontId == 1909382491) return 2056549737; // 16 -> 18
  if (baseFontId == 2056549737) return 2056549737; // 18 -> 18 (already largest)
  
  // Check if it's a NotoSans font
  if (baseFontId == -1646794343) return -890242897;  // 12 -> 14
  if (baseFontId == -890242897) return 241925189;    // 14 -> 16
  if (baseFontId == 241925189) return 1503221336;    // 16 -> 18
  if (baseFontId == 1503221336) return 1503221336;  // 18 -> 18 (already largest)
  
  // Check if it's an OpenDyslexic font
  if (baseFontId == 875216341) return -1234231183;  // 8 -> 10
  if (baseFontId == -1234231183) return 1682200414; // 10 -> 12
  if (baseFontId == 1682200414) return -1851285286; // 12 -> 14
  if (baseFontId == -1851285286) return -1851285286; // 14 -> 14 (already largest)
  
  // Default: return base font (no change)
  return baseFontId;
}

void PageLine::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // If this is a center-aligned block (likely a header), use larger font
  const int actualFontId = (block->getStyle() == TextBlock::CENTER_ALIGN) 
                          ? getHeaderFontId(fontId) 
                          : fontId;
  block->render(renderer, actualFontId, xPos + xOffset, yPos + yOffset);
}

bool PageLine::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);

  // serialize TextBlock pointed to by PageLine
  return block->serialize(file);
}

std::unique_ptr<PageLine> PageLine::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);

  auto tb = TextBlock::deserialize(file);
  return std::unique_ptr<PageLine>(new PageLine(std::move(tb), xPos, yPos));
}

void Page::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) const {
  for (auto& element : elements) {
    element->render(renderer, fontId, xOffset, yOffset);
  }
}

bool Page::serialize(FsFile& file) const {
  const uint16_t count = elements.size();
  serialization::writePod(file, count);

  for (const auto& el : elements) {
    // Get the tag from the element itself (no RTTI needed)
    const uint8_t tag = static_cast<uint8_t>(el->getTag());
    serialization::writePod(file, tag);
    if (!el->serialize(file)) {
      return false;
    }
  }

  return true;
}

void DropCapElement::render(GfxRenderer& renderer, const int fontId, const int xOffset, const int yOffset) {
  // Use TTF rendering for drop cap - calculate font size to span 2 lines
  // Get line height from the base font to calculate appropriate drop cap size
  const int lineHeight = renderer.getLineHeight(fontId);
  
  // Drop cap should span 2 lines, so make it approximately 2x the line height
  // Add some extra for visual impact (about 2.5x)
  const int dropCapFontSize = (lineHeight * 5) / 2;  // 2.5x line height
  
  // Render using TTF (smooth, scalable)
  renderer.drawTextTTF(xPos + xOffset, yPos + yOffset, character.c_str(), dropCapFontSize, true);
}

bool DropCapElement::serialize(FsFile& file) {
  serialization::writePod(file, xPos);
  serialization::writePod(file, yPos);
  serialization::writePod(file, fontId);
  serialization::writePod(file, static_cast<uint8_t>(style));
  serialization::writeString(file, character);
  return true;
}

std::unique_ptr<DropCapElement> DropCapElement::deserialize(FsFile& file) {
  int16_t xPos;
  int16_t yPos;
  int fontId;
  uint8_t styleByte;
  std::string character;
  
  serialization::readPod(file, xPos);
  serialization::readPod(file, yPos);
  serialization::readPod(file, fontId);
  serialization::readPod(file, styleByte);
  serialization::readString(file, character);
  
  return std::unique_ptr<DropCapElement>(new DropCapElement(character, xPos, yPos, fontId, static_cast<EpdFontFamily::Style>(styleByte)));
}

std::unique_ptr<Page> Page::deserialize(FsFile& file) {
  auto page = std::unique_ptr<Page>(new Page());

  uint16_t count;
  serialization::readPod(file, count);

  for (uint16_t i = 0; i < count; i++) {
    uint8_t tag;
    serialization::readPod(file, tag);

    if (tag == TAG_PageLine) {
      auto pl = PageLine::deserialize(file);
      page->elements.push_back(std::move(pl));
    } else if (tag == TAG_DropCap) {
      auto dc = DropCapElement::deserialize(file);
      page->elements.push_back(std::move(dc));
    } else {
      Serial.printf("[%lu] [PGE] Deserialization failed: Unknown tag %u\n", millis(), tag);
      return nullptr;
    }
  }

  return page;
}
