#include "ChapterHtmlSlimParser.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <SDCardManager.h>
#include <expat.h>

#include "../Page.h"

const char* HEADER_TAGS[] = {"h1", "h2", "h3", "h4", "h5", "h6"};
constexpr int NUM_HEADER_TAGS = sizeof(HEADER_TAGS) / sizeof(HEADER_TAGS[0]);

// Minimum file size (in bytes) to show progress bar - smaller chapters don't benefit from it
constexpr size_t MIN_SIZE_FOR_PROGRESS = 50 * 1024;  // 50KB

const char* BLOCK_TAGS[] = {"p", "li", "div", "br", "blockquote"};
constexpr int NUM_BLOCK_TAGS = sizeof(BLOCK_TAGS) / sizeof(BLOCK_TAGS[0]);

const char* BOLD_TAGS[] = {"b", "strong"};
constexpr int NUM_BOLD_TAGS = sizeof(BOLD_TAGS) / sizeof(BOLD_TAGS[0]);

const char* ITALIC_TAGS[] = {"i", "em"};
constexpr int NUM_ITALIC_TAGS = sizeof(ITALIC_TAGS) / sizeof(ITALIC_TAGS[0]);

const char* IMAGE_TAGS[] = {"img"};
constexpr int NUM_IMAGE_TAGS = sizeof(IMAGE_TAGS) / sizeof(IMAGE_TAGS[0]);

const char* SKIP_TAGS[] = {"head", "table"};
constexpr int NUM_SKIP_TAGS = sizeof(SKIP_TAGS) / sizeof(SKIP_TAGS[0]);

bool isWhitespace(const char c) { return c == ' ' || c == '\r' || c == '\n' || c == '\t'; }

// given the start and end of a tag, check to see if it matches a known tag
bool matches(const char* tag_name, const char* possible_tags[], const int possible_tag_count) {
  for (int i = 0; i < possible_tag_count; i++) {
    if (strcmp(tag_name, possible_tags[i]) == 0) {
      return true;
    }
  }
  return false;
}

// start a new text block if needed
void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::Style style) {
  startNewTextBlock(style, false);  // Default: not a header
}

void ChapterHtmlSlimParser::startNewTextBlock(const TextBlock::Style style, const bool isHeader) {
  if (currentTextBlock) {
    // already have a text block running and it is empty - just reuse it
    if (currentTextBlock->isEmpty()) {
      currentTextBlock->setStyle(style);
      currentTextBlock->setIsHeader(isHeader);  // Preserve isHeader flag when reusing
      return;
    }

    // Check if current block is just "Chapter" - if so, don't process it yet (wait to see if next is a number)
    bool isJustChapter = false;
    if (currentTextBlock->getWordCount() == 1) {
      std::string firstWord = currentTextBlock->getFirstWord();
      std::string firstWordLower = firstWord;
      std::transform(firstWordLower.begin(), firstWordLower.end(), firstWordLower.begin(), ::tolower);
      if (firstWordLower == "chapter") {
        isJustChapter = true;
      }
    }

    if (!isJustChapter) {
      // Check if the block we just processed was a header - if so, next paragraph needs drop cap
      // This check happens here because endElement (where header is detected) may be called
      // after startElement for the next paragraph, so we check the current block before moving it
      if (currentTextBlock && currentTextBlock->isHeaderBlock()) {
        Serial.printf("[%lu] [CHP] Header block detected, setting needsDropCap=true\n", millis());
        needsDropCap = true;
      }
      
      // Log block info for debugging
      if (currentTextBlock) {
        Serial.printf("[%lu] [CHP] Processing block: hasDropCapFlag=%d, isEmpty=%d, isEffectivelyEmpty=%d, wordCount=%d\n", 
                      millis(), currentTextBlock->hasDropCapFlag(), currentTextBlock->isEmpty(), 
                      currentTextBlock->isEffectivelyEmpty(), currentTextBlock->getWordCount());
        if (currentTextBlock->getWordCount() > 0 && currentTextBlock->getWordCount() <= 3) {
          // Log first few words for debugging
          std::string firstWord = currentTextBlock->getFirstWord();
          Serial.printf("[%lu] [CHP] First word: '%s' (len=%d)\n", millis(), firstWord.c_str(), firstWord.length());
        }
      }
      
      // Check if the block is effectively empty (only whitespace)
      // If so, delete it completely BEFORE calling makePages() to avoid adding it to pages
      bool shouldSkipBlock = false;
      if (currentTextBlock && currentTextBlock->isEffectivelyEmpty()) {
        Serial.printf("[%lu] [CHP] Block is effectively empty - DELETING it\n", millis());
        // Block was effectively empty (only whitespace)
        // If it had drop cap flag, restore needsDropCap for the next paragraph
        if (currentTextBlock->hasDropCapFlag()) {
          Serial.printf("[%lu] [CHP] Block had drop cap flag, restoring needsDropCap=true\n", millis());
          needsDropCap = true;  // Restore it for the next paragraph
        }
        shouldSkipBlock = true;  // Mark to skip processing
      }
      
      if (shouldSkipBlock) {
        // Don't call makePages() - just delete the block completely
        currentTextBlock.reset();  // Completely delete the empty paragraph
        Serial.printf("[%lu] [CHP] Block deleted, needsDropCap=%d\n", millis(), needsDropCap);
      } else {
        // Process the current block into pages BEFORE moving it
        makePages();
        
        // After makePages(), check if block had drop cap flag but no drop cap character was extracted
        // This can happen if the block only had whitespace words (drop cap extraction failed)
        if (currentTextBlock && currentTextBlock->hasDropCapFlag() && !currentTextBlock->hasDropCapChar()) {
          Serial.printf("[%lu] [CHP] Block had drop cap flag but no drop cap char extracted - effectively empty\n", millis());
          // Block had drop cap flag but no drop cap character was extracted - it was effectively empty
          // Note: Pages were already created, but that's okay - the block had no content anyway
          // Restore needsDropCap for the next paragraph
          needsDropCap = true;  // Restore it for the next paragraph
          // Don't delete here - pages were already created, just mark that we didn't use the drop cap
        } else if (currentTextBlock && currentTextBlock->hasDropCapFlag() && currentTextBlock->hasDropCapChar()) {
          Serial.printf("[%lu] [CHP] Block has drop cap char: '%s' - drop cap used successfully\n", 
                        millis(), currentTextBlock->getDropCapChar().c_str());
          // Block had drop cap flag and a valid drop cap character - we've used it successfully
          dropCapAdded = false;  // Reset flag (needsDropCap was already reset when block was created)
        }
      }
    }
    // Now save it as previous block (for combining split headers) - but only if it wasn't deleted
    if (currentTextBlock) {
      previousTextBlock = std::move(currentTextBlock);
    }
  }
  
  // Create new block - only set hasDropCap if needsDropCap is true AND this is not a header
  // Headers should never have drop caps
  const bool shouldHaveDropCap = needsDropCap && !isHeader;
  
  // If we're creating a block with drop cap flag, reset needsDropCap immediately
  // This prevents it from propagating to subsequent blocks
  // If the block turns out to be empty, we'll set needsDropCap back to true when we process it
  if (shouldHaveDropCap) {
    Serial.printf("[%lu] [CHP] Creating new block with drop cap flag, resetting needsDropCap=false\n", millis());
    needsDropCap = false;  // Reset immediately - we've "consumed" it for this block
    dropCapAdded = false;  // Reset flag for new paragraph
    dropCapLineCount = 0;  // Reset line count for new drop cap paragraph
    firstDropCapLine = nullptr;  // Reset first line reference
  } else {
    Serial.printf("[%lu] [CHP] Creating new block without drop cap (needsDropCap=%d, isHeader=%d)\n", 
                  millis(), needsDropCap, isHeader);
  }
  
  currentTextBlock.reset(new ParsedText(style, extraParagraphSpacing, isHeader, shouldHaveDropCap));
}

void XMLCALL ChapterHtmlSlimParser::startElement(void* userData, const XML_Char* name, const XML_Char** atts) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    self->depth += 1;
    return;
  }

  if (matches(name, IMAGE_TAGS, NUM_IMAGE_TAGS)) {
    // TODO: Start processing image tags
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  if (matches(name, SKIP_TAGS, NUM_SKIP_TAGS)) {
    // start skip
    self->skipUntilDepth = self->depth;
    self->depth += 1;
    return;
  }

  // Skip blocks with role="doc-pagebreak" and epub:type="pagebreak"
  if (atts != nullptr) {
    for (int i = 0; atts[i]; i += 2) {
      if (strcmp(atts[i], "role") == 0 && strcmp(atts[i + 1], "doc-pagebreak") == 0 ||
          strcmp(atts[i], "epub:type") == 0 && strcmp(atts[i + 1], "pagebreak") == 0) {
        self->skipUntilDepth = self->depth;
        self->depth += 1;
        return;
      }
    }
  }

  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    Serial.printf("[%lu] [CHP] Header tag opened: %s\n", millis(), name);
    // Mark that we're inside a header tag
    self->headerUntilDepth = std::min(self->headerUntilDepth, self->depth);
    // Headers should always be center-aligned, even when standardizing
    if (self->standardizeFormatting) {
      self->startNewTextBlock(TextBlock::CENTER_ALIGN, true);  // isHeader=true, always center
    } else {
      self->startNewTextBlock(TextBlock::CENTER_ALIGN, true);  // isHeader=true
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }
  } else if (matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS)) {
    if (strcmp(name, "p") == 0) {
      Serial.printf("[%lu] [CHP] Paragraph tag opened\n", millis());
    } else if (strcmp(name, "br") == 0) {
      Serial.printf("[%lu] [CHP] BR tag (line break)\n", millis());
    }
    if (strcmp(name, "br") == 0) {
      self->startNewTextBlock(self->currentTextBlock->getStyle());
    } else {
      self->startNewTextBlock((TextBlock::Style)self->paragraphAlignment);
    }
  } else if (matches(name, BOLD_TAGS, NUM_BOLD_TAGS)) {
    // When standardizing, ignore bold/italic styling
    if (!self->standardizeFormatting) {
      self->boldUntilDepth = std::min(self->boldUntilDepth, self->depth);
    }
  } else if (matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS)) {
    // When standardizing, ignore bold/italic styling
    if (!self->standardizeFormatting) {
      self->italicUntilDepth = std::min(self->italicUntilDepth, self->depth);
    }
  }

  self->depth += 1;
}

void XMLCALL ChapterHtmlSlimParser::characterData(void* userData, const XML_Char* s, const int len) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Middle of skip
  if (self->skipUntilDepth < self->depth) {
    return;
  }

  EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
  if (self->standardizeFormatting) {
    // When standardizing, always use REGULAR style (ignore bold/italic)
    fontStyle = EpdFontFamily::REGULAR;
  } else {
    // Normal mode: respect bold/italic tags
    if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
      fontStyle = EpdFontFamily::BOLD_ITALIC;
    } else if (self->boldUntilDepth < self->depth) {
      fontStyle = EpdFontFamily::BOLD;
    } else if (self->italicUntilDepth < self->depth) {
      fontStyle = EpdFontFamily::ITALIC;
    }
  }

  for (int i = 0; i < len; i++) {
    // Inside a header, convert newlines to spaces to keep header on one line
    const bool isInsideHeader = self->headerUntilDepth < self->depth;
    
    // For headers, convert newlines to regular spaces
    char c = s[i];
    if (isInsideHeader && c == '\n') {
      c = ' ';  // Replace newline with space
    }
    
    if (isWhitespace(c)) {
      // Currently looking at whitespace, if there's anything in the partWordBuffer, flush it
      if (self->partWordBufferIndex > 0) {
        self->partWordBuffer[self->partWordBufferIndex] = '\0';
        self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
        self->partWordBufferIndex = 0;
      }
      // Skip whitespace (now including newlines that were converted to spaces)
      continue;
    }

    // Skip soft-hyphen with UTF-8 representation (U+00AD) = 0xC2 0xAD
    const XML_Char SHY_BYTE_1 = static_cast<XML_Char>(0xC2);
    const XML_Char SHY_BYTE_2 = static_cast<XML_Char>(0xAD);
    // 1. Check for the start of the 2-byte Soft Hyphen sequence
    if (s[i] == SHY_BYTE_1) {
      // 2. Check if the next byte exists AND if it completes the sequence
      //    We must check i + 1 < len to prevent reading past the end of the buffer.
      if ((i + 1 < len) && (s[i + 1] == SHY_BYTE_2)) {
        // Sequence 0xC2 0xAD found!
        // Skip the current byte (0xC2) and the next byte (0xAD)
        i++;       // Increment 'i' one more time to skip the 0xAD byte
        continue;  // Skip the rest of the loop and move to the next iteration
      }
    }

    // If we're about to run out of space, then cut the word off and start a new one
    if (self->partWordBufferIndex >= MAX_WORD_SIZE) {
      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
      self->partWordBufferIndex = 0;
    }

    self->partWordBuffer[self->partWordBufferIndex++] = c;
  }

  // If we have > 750 words buffered up, perform the layout and consume out all but the last line
  // There should be enough here to build out 1-2 full pages and doing this will free up a lot of
  // memory.
  // Spotted when reading Intermezzo, there are some really long text blocks in there.
  if (self->currentTextBlock->size() > 750) {
    self->currentTextBlock->layoutAndExtractLines(
        self->renderer, self->fontId, self->viewportWidth,
        [self](const std::shared_ptr<TextBlock>& textBlock) { self->addLineToPage(textBlock); }, false);
  }
}

void XMLCALL ChapterHtmlSlimParser::endElement(void* userData, const XML_Char* name) {
  auto* self = static_cast<ChapterHtmlSlimParser*>(userData);

  // Clear header flag when exiting header tag
  if (matches(name, HEADER_TAGS, NUM_HEADER_TAGS)) {
    if (self->headerUntilDepth == self->depth) {
      self->headerUntilDepth = INT_MAX;
      // Style HTML header tags the same way as detected chapter headers: center, bold, larger font
      if (self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
        // Ensure it's marked as a header
        self->currentTextBlock->setIsHeader(true);
        // Style as center-aligned
        self->currentTextBlock->setStyle(TextBlock::CENTER_ALIGN);
        // Make all words bold
        for (auto& style : self->currentTextBlock->getWordStyles()) {
          if (style == EpdFontFamily::REGULAR) {
            style = EpdFontFamily::BOLD;
          } else if (style == EpdFontFamily::ITALIC) {
            style = EpdFontFamily::BOLD_ITALIC;
          }
        }
        // Set needsDropCap for next paragraph
        Serial.printf("[%lu] [CHP] Header tag closed, setting needsDropCap=true\n", millis());
        self->needsDropCap = true;
      }
    }
  }

  if (self->partWordBufferIndex > 0) {
    // Only flush out part word buffer if we're closing a block tag or are at the top of the HTML file.
    // We don't want to flush out content when closing inline tags like <span>.
    // Currently this also flushes out on closing <b> and <i> tags, but they are line tags so that shouldn't happen,
    // text styling needs to be overhauled to fix it.
    const bool shouldBreakText =
        matches(name, BLOCK_TAGS, NUM_BLOCK_TAGS) || matches(name, HEADER_TAGS, NUM_HEADER_TAGS) ||
        matches(name, BOLD_TAGS, NUM_BOLD_TAGS) || matches(name, ITALIC_TAGS, NUM_ITALIC_TAGS) || self->depth == 1;

    if (shouldBreakText) {
      EpdFontFamily::Style fontStyle = EpdFontFamily::REGULAR;
      if (self->standardizeFormatting) {
        // When standardizing, always use REGULAR style (ignore bold/italic)
        fontStyle = EpdFontFamily::REGULAR;
      } else {
        // Normal mode: respect bold/italic tags
        if (self->boldUntilDepth < self->depth && self->italicUntilDepth < self->depth) {
          fontStyle = EpdFontFamily::BOLD_ITALIC;
        } else if (self->boldUntilDepth < self->depth) {
          fontStyle = EpdFontFamily::BOLD;
        } else if (self->italicUntilDepth < self->depth) {
          fontStyle = EpdFontFamily::ITALIC;
        }
      }

      self->partWordBuffer[self->partWordBufferIndex] = '\0';
      self->currentTextBlock->addWord(self->partWordBuffer, fontStyle);
      self->partWordBufferIndex = 0;
    }
  }

  // Check if paragraph looks like a chapter header ("Chapter X") - AFTER flushing buffer
  if (strcmp(name, "p") == 0 && self->currentTextBlock && !self->currentTextBlock->isEmpty()) {
    Serial.printf("[%lu] [CHP] Checking paragraph for chapter header, wordCount=%d\n", 
                  millis(), self->currentTextBlock->getWordCount());
    self->currentTextBlock->setIsHeaderIfChapter();
    if (self->currentTextBlock->isHeaderBlock()) {
      Serial.printf("[%lu] [CHP] Paragraph detected as chapter header!\n", millis());
    }
    
    // Check if we need to combine with previous paragraph (e.g., "Chapter" + "34")
    if (self->previousTextBlock && !self->previousTextBlock->isEmpty() && 
        self->previousTextBlock->getWordCount() == 1 && self->currentTextBlock->getWordCount() == 1) {
      std::string prevWord = self->previousTextBlock->getFirstWord();
      std::string currWord = self->currentTextBlock->getFirstWord();
      
      // Convert to lowercase for comparison
      std::string prevWordLower = prevWord;
      std::transform(prevWordLower.begin(), prevWordLower.end(), prevWordLower.begin(), ::tolower);
      
      // Check if previous is "Chapter" and current is a number
      bool currIsNumber = !currWord.empty();
      for (char c : currWord) {
        if (!std::isdigit(c)) {
          currIsNumber = false;
          break;
        }
      }
      
      if (prevWordLower == "chapter" && currIsNumber) {
        // Merge current word into previous block (which hasn't been processed yet)
        self->previousTextBlock->addWord(currWord, self->currentTextBlock->getFirstWordStyle());
        self->previousTextBlock->setIsHeaderIfChapter();  // Ensure it's marked as header
        // Style the merged header: center, bold, larger font
        self->previousTextBlock->setStyle(TextBlock::CENTER_ALIGN);
        for (auto& style : self->previousTextBlock->getWordStyles()) {
          if (style == EpdFontFamily::REGULAR) {
            style = EpdFontFamily::BOLD;
          } else if (style == EpdFontFamily::ITALIC) {
            style = EpdFontFamily::BOLD_ITALIC;
          }
        }
        // Now swap: make the merged previous block the current one (it will be processed on next paragraph)
        self->currentTextBlock = std::move(self->previousTextBlock);
      }
    }
    
    if (self->currentTextBlock->isHeaderBlock()) {
      // Style chapter headers: center-aligned, bold, larger font
      self->currentTextBlock->setStyle(TextBlock::CENTER_ALIGN);
      // Make all words bold
      for (auto& style : self->currentTextBlock->getWordStyles()) {
        if (style == EpdFontFamily::REGULAR) {
          style = EpdFontFamily::BOLD;
        } else if (style == EpdFontFamily::ITALIC) {
          style = EpdFontFamily::BOLD_ITALIC;
        }
        // BOLD and BOLD_ITALIC already bold, leave as is
      }
      // Note: needsDropCap will be set in startNewTextBlock when this header block is processed
      // (before the next paragraph's block is created), so we don't set it here
    }
  }

  self->depth -= 1;

  // Leaving skip
  if (self->skipUntilDepth == self->depth) {
    self->skipUntilDepth = INT_MAX;
  }

  // Leaving bold
  if (self->boldUntilDepth == self->depth) {
    self->boldUntilDepth = INT_MAX;
  }

  // Leaving italic
  if (self->italicUntilDepth == self->depth) {
    self->italicUntilDepth = INT_MAX;
  }
}

bool ChapterHtmlSlimParser::parseAndBuildPages() {
  startNewTextBlock((TextBlock::Style)this->paragraphAlignment);

  const XML_Parser parser = XML_ParserCreate(nullptr);
  int done;

  if (!parser) {
    return false;
  }

  FsFile file;
  if (!SdMan.openFileForRead("EHP", filepath, file)) {
    XML_ParserFree(parser);
    return false;
  }

  // Get file size for progress calculation
  const size_t totalSize = file.size();
  size_t bytesRead = 0;
  int lastProgress = -1;

  XML_SetUserData(parser, this);
  XML_SetElementHandler(parser, startElement, endElement);
  XML_SetCharacterDataHandler(parser, characterData);

  do {
    void* const buf = XML_GetBuffer(parser, 1024);
    if (!buf) {
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    const size_t len = file.read(buf, 1024);

    if (len == 0 && file.available() > 0) {
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }

    // Update progress (call every 10% change to avoid too frequent updates)
    // Only show progress for larger chapters where rendering overhead is worth it
    bytesRead += len;
    if (progressFn && totalSize >= MIN_SIZE_FOR_PROGRESS) {
      const int progress = static_cast<int>((bytesRead * 100) / totalSize);
      if (lastProgress / 10 != progress / 10) {
        lastProgress = progress;
        progressFn(progress);
      }
    }

    done = file.available() == 0;

    if (XML_ParseBuffer(parser, static_cast<int>(len), done) == XML_STATUS_ERROR) {
      XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
      XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
      XML_SetCharacterDataHandler(parser, nullptr);
      XML_ParserFree(parser);
      file.close();
      return false;
    }
  } while (!done);

  XML_StopParser(parser, XML_FALSE);                // Stop any pending processing
  XML_SetElementHandler(parser, nullptr, nullptr);  // Clear callbacks
  XML_SetCharacterDataHandler(parser, nullptr);
  XML_ParserFree(parser);
  file.close();

  // Process last page if there is still text
  if (currentTextBlock) {
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
  }
  
  // Also process any deferred previousTextBlock (e.g., "Chapter" that wasn't followed by a number)
  if (previousTextBlock && !previousTextBlock->isEmpty()) {
    currentTextBlock = std::move(previousTextBlock);
    makePages();
    completePageFn(std::move(currentPage));
    currentPage.reset();
    currentTextBlock.reset();
  }

  return true;
}

void ChapterHtmlSlimParser::addLineToPage(std::shared_ptr<TextBlock> line) {
  // Check if this is a header line (center-aligned) to apply special styling
  const bool isHeaderLine = line->getStyle() == TextBlock::CENTER_ALIGN && 
                            currentTextBlock && currentTextBlock->isHeaderBlock();
  
  // Use larger font for headers (already calculated during layout, but we need it for line height)
  const int actualFontId = isHeaderLine ? currentTextBlock->getHeaderFontId(fontId) : fontId;
  
  const int lineHeight = renderer.getLineHeight(actualFontId) * lineCompression;
  
  // Add padding before headers (top padding)
  const int headerPadding = isHeaderLine ? 30 : 0;
  
  if (currentPageNextY + lineHeight + headerPadding > viewportHeight) {
    completePageFn(std::move(currentPage));
    currentPage.reset(new Page());
    currentPageNextY = 0;
    dropCapAdded = false;  // Reset flag when starting new page
    dropCapLineCount = 0;  // Reset line count when starting new page
    firstDropCapLine = nullptr;  // Reset first line reference when starting new page
  }
  
  // Add top padding for headers
  if (headerPadding > 0) {
    currentPageNextY += headerPadding;
  }

  // Add drop cap element only on the first line of the first paragraph after a header
  // Simple check: if this paragraph has a drop cap and we haven't added it yet, add it now
  if (currentTextBlock && currentTextBlock->hasDropCapChar() && !dropCapAdded) {
    // Get drop cap details from the text block
    const std::string dropCapChar = currentTextBlock->getDropCapChar();
    
    // Use the same font ID, just scale it 3x bigger during rendering
    const int dropCapFontId = fontId;  // Keep the same font, scale it 3x in rendering
    const EpdFontFamily::Style dropCapStyle = currentTextBlock->getDropCapStyle();
    
    // Position drop cap at the baseline of the first line, dropped down
    // The y position should be adjusted so the drop cap aligns with the text baseline
    // drawText adds getFontAscenderSize, so we need to subtract it to align baselines
    const int dropCapAscender = renderer.getFontAscenderSize(dropCapFontId);
    const int textAscender = renderer.getFontAscenderSize(actualFontId);
    const int lineHeight = renderer.getLineHeight(actualFontId) * lineCompression;
    const int dropCapY = currentPageNextY - (dropCapAscender - textAscender) + lineHeight;  // Drop down by one line height
    
    // Create a DropCapElement and add it to the page
    auto dropCapElement = std::make_shared<DropCapElement>(dropCapChar, 0, dropCapY, dropCapFontId, dropCapStyle);
    currentPage->elements.push_back(dropCapElement);
    dropCapAdded = true;  // Mark as added so we don't add it again
  }

  // Track lines for drop cap paragraphs
  const bool isDropCapParagraph = currentTextBlock && currentTextBlock->hasDropCapChar();
  std::shared_ptr<PageLine> pageLine = std::make_shared<PageLine>(line, 0, currentPageNextY);
  
  if (isDropCapParagraph) {
    dropCapLineCount++;
    if (dropCapLineCount == 1) {
      // Store reference to first line for potential single-line adjustment
      firstDropCapLine = pageLine;
    }
  }
  
  currentPage->elements.push_back(pageLine);
  currentPageNextY += lineHeight;
  
  // Add bottom padding for headers
  if (isHeaderLine) {
    currentPageNextY += 30;  // Bottom padding
  }
}

void ChapterHtmlSlimParser::makePages() {
  if (!currentTextBlock) {
    return;
  }

  if (!currentPage) {
    currentPage.reset(new Page());
    currentPageNextY = 0;
  }

  const int lineHeight = renderer.getLineHeight(fontId) * lineCompression;
  currentTextBlock->layoutAndExtractLines(
      renderer, fontId, viewportWidth,
      [this](const std::shared_ptr<TextBlock>& textBlock) { addLineToPage(textBlock); });
  
  // If this was a drop cap paragraph with only 1 line, adjust it to line 2
  if (currentTextBlock && currentTextBlock->hasDropCapChar() && dropCapLineCount == 1 && firstDropCapLine) {
    // Adjust the Y position of the single line to be on line 2 (add one line height)
    const int adjustedY = firstDropCapLine->yPos + lineHeight;
    firstDropCapLine->yPos = adjustedY;
    // Also update currentPageNextY to account for the adjustment (so next paragraph doesn't overlap)
    currentPageNextY += lineHeight;
    Serial.printf("[%lu] [CHP] Single-line drop cap paragraph detected, adjusted Y position to %d, currentPageNextY=%d\n", 
                  millis(), adjustedY, currentPageNextY);
  }
  
  // Reset drop cap tracking for next paragraph
  dropCapLineCount = 0;
  firstDropCapLine = nullptr;
  
  // Extra paragraph spacing if enabled
  if (extraParagraphSpacing) {
    currentPageNextY += lineHeight / 2;
  }
}
