#pragma once
#include <SdFat.h>

#include <utility>
#include <vector>

#include "blocks/TextBlock.h"

enum PageElementTag : uint8_t {
  TAG_PageLine = 1,
  TAG_DropCap = 2,
};

// represents something that has been added to a page
class PageElement {
 public:
  int16_t xPos;
  int16_t yPos;
  explicit PageElement(const int16_t xPos, const int16_t yPos) : xPos(xPos), yPos(yPos) {}
  virtual ~PageElement() = default;
  virtual void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) = 0;
  virtual bool serialize(FsFile& file) = 0;
  virtual PageElementTag getTag() const = 0;  // Return the tag type for serialization
};

// a line from a block element
class PageLine final : public PageElement {
  std::shared_ptr<TextBlock> block;

 public:
  PageLine(std::shared_ptr<TextBlock> block, const int16_t xPos, const int16_t yPos)
      : PageElement(xPos, yPos), block(std::move(block)) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_PageLine; }
  static std::unique_ptr<PageLine> deserialize(FsFile& file);
};

// a drop cap character (large first character spanning 3 lines)
class DropCapElement final : public PageElement {
  std::string character;
  int fontId;
  EpdFontFamily::Style style;

 public:
  DropCapElement(const std::string& character, const int16_t xPos, const int16_t yPos, int fontId, EpdFontFamily::Style style)
      : PageElement(xPos, yPos), character(character), fontId(fontId), style(style) {}
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) override;
  bool serialize(FsFile& file) override;
  PageElementTag getTag() const override { return TAG_DropCap; }
  static std::unique_ptr<DropCapElement> deserialize(FsFile& file);
};

class Page {
 public:
  // the list of block index and line numbers on this page
  std::vector<std::shared_ptr<PageElement>> elements;
  void render(GfxRenderer& renderer, int fontId, int xOffset, int yOffset) const;
  bool serialize(FsFile& file) const;
  static std::unique_ptr<Page> deserialize(FsFile& file);
};
