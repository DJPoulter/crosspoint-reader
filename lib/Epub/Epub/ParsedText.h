#pragma once

#include <EpdFontFamily.h>

#include <functional>
#include <list>
#include <memory>
#include <string>
#include <vector>

#include "blocks/TextBlock.h"

class GfxRenderer;

class ParsedText {
  std::list<std::string> words;
  std::list<EpdFontFamily::Style> wordStyles;
  TextBlock::Style style;
  bool extraParagraphSpacing;
  bool isHeader;  // If true, prevent line breaks (keep all words on one line) and use header styling
  bool hasDropCap;  // If true, first character should be rendered as drop cap spanning 3 lines
  std::string dropCapChar;  // The drop cap character (first character of first word)
  EpdFontFamily::Style dropCapStyle;  // Style for drop cap (usually bold)

  std::vector<size_t> computeLineBreaks(int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths) const;
  void extractLine(size_t breakIndex, int pageWidth, int spaceWidth, const std::vector<uint16_t>& wordWidths,
                   const std::vector<size_t>& lineBreakIndices,
                   const std::function<void(std::shared_ptr<TextBlock>)>& processLine, int fontId, int xOffset = 0);
  std::vector<uint16_t> calculateWordWidths(const GfxRenderer& renderer, int fontId);

 public:
  explicit ParsedText(const TextBlock::Style style, const bool extraParagraphSpacing, const bool isHeader = false, const bool hasDropCap = false)
      : style(style), extraParagraphSpacing(extraParagraphSpacing), isHeader(isHeader), hasDropCap(hasDropCap), dropCapStyle(EpdFontFamily::REGULAR) {}
  ~ParsedText() = default;

  void addWord(std::string word, EpdFontFamily::Style fontStyle);
  void setStyle(const TextBlock::Style style) { this->style = style; }
  TextBlock::Style getStyle() const { return style; }
  size_t size() const { return words.size(); }
  bool isEmpty() const { return words.empty(); }
  bool isEffectivelyEmpty() const;  // Check if block only contains whitespace words
  void setIsHeaderIfChapter();  // Check if this looks like "Chapter X" and set isHeader=true
  void setIsHeader(bool value) { isHeader = value; }  // Set the isHeader flag
  bool isHeaderBlock() const { return isHeader; }  // Check if this is marked as a header
  std::string getFirstWord() const { return words.empty() ? "" : words.front(); }  // Get first word for header detection
  EpdFontFamily::Style getFirstWordStyle() const { return wordStyles.empty() ? EpdFontFamily::REGULAR : wordStyles.front(); }  // Get first word style
  size_t getWordCount() const { return words.size(); }
  void mergeFrom(const ParsedText& other);  // Merge words from another ParsedText into this one
  std::list<EpdFontFamily::Style>& getWordStyles() { return wordStyles; }  // Access word styles for header styling
  void layoutAndExtractLines(const GfxRenderer& renderer, int fontId, uint16_t viewportWidth,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                             bool includeLastLine = true);
  int getHeaderFontId(int baseFontId) const;  // Get larger font ID for headers
  bool hasDropCapFlag() const { return hasDropCap; }  // Check if block was created with drop cap flag
  bool hasDropCapChar() const { return hasDropCap && !dropCapChar.empty(); }
  std::string getDropCapChar() const { return dropCapChar; }
  EpdFontFamily::Style getDropCapStyle() const { return dropCapStyle; }
  int getDropCapFontId(int baseFontId) const;  // Get font ID for drop cap (3 sizes up)
};
