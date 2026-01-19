#pragma once

#include <expat.h>
#include <HardwareSerial.h>

#include <climits>
#include <functional>
#include <memory>

#include "../ParsedText.h"
#include "../blocks/TextBlock.h"

class Page;
class PageLine;
class GfxRenderer;

#define MAX_WORD_SIZE 200

class ChapterHtmlSlimParser {
  const std::string& filepath;
  GfxRenderer& renderer;
  std::function<void(std::unique_ptr<Page>)> completePageFn;
  std::function<void(int)> progressFn;  // Progress callback (0-100)
  int depth = 0;
  int skipUntilDepth = INT_MAX;
  int boldUntilDepth = INT_MAX;
  int italicUntilDepth = INT_MAX;
  int headerUntilDepth = INT_MAX;  // Track if we're inside a header tag
  // buffer for building up words from characters, will auto break if longer than this
  // leave one char at end for null pointer
  char partWordBuffer[MAX_WORD_SIZE + 1] = {};
  int partWordBufferIndex = 0;
  std::unique_ptr<ParsedText> currentTextBlock = nullptr;
  std::unique_ptr<ParsedText> previousTextBlock = nullptr;  // Track previous block to combine split headers
  std::unique_ptr<Page> currentPage = nullptr;
  int16_t currentPageNextY = 0;
  bool needsDropCap = false;  // Track if next paragraph should have a drop cap (after header)
  bool dropCapAdded = false;  // Track if we've already added the drop cap for the current paragraph
  int dropCapLineCount = 0;  // Track how many lines have been added for the current drop cap paragraph
  std::shared_ptr<PageLine> firstDropCapLine = nullptr;  // Track first line of drop cap paragraph for single-line adjustment
  int fontId;
  float lineCompression;
  bool extraParagraphSpacing;
  uint8_t paragraphAlignment;
  uint16_t viewportWidth;
  uint16_t viewportHeight;
  bool standardizeFormatting;

  void startNewTextBlock(TextBlock::Style style);
  void startNewTextBlock(TextBlock::Style style, bool isHeader);
  void makePages();
  // XML callbacks
  static void XMLCALL startElement(void* userData, const XML_Char* name, const XML_Char** atts);
  static void XMLCALL characterData(void* userData, const XML_Char* s, int len);
  static void XMLCALL endElement(void* userData, const XML_Char* name);

 public:
  explicit ChapterHtmlSlimParser(const std::string& filepath, GfxRenderer& renderer, const int fontId,
                                 const float lineCompression, const bool extraParagraphSpacing,
                                 const uint8_t paragraphAlignment, const uint16_t viewportWidth,
                                 const uint16_t viewportHeight,
                                 const std::function<void(std::unique_ptr<Page>)>& completePageFn,
                                 const std::function<void(int)>& progressFn = nullptr,
                                 const bool standardizeFormatting = false)
      : filepath(filepath),
        renderer(renderer),
        fontId(fontId),
        lineCompression(lineCompression),
        extraParagraphSpacing(extraParagraphSpacing),
        paragraphAlignment(paragraphAlignment),
        viewportWidth(viewportWidth),
        viewportHeight(viewportHeight),
        completePageFn(completePageFn),
        progressFn(progressFn),
        standardizeFormatting(standardizeFormatting) {}
  ~ChapterHtmlSlimParser() = default;
  bool parseAndBuildPages();
  void addLineToPage(std::shared_ptr<TextBlock> line);
};
