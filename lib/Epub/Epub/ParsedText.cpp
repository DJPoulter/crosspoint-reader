#include "ParsedText.h"

#include <GfxRenderer.h>
#include <HardwareSerial.h>
#include <HardwareSerial.h>
#include <Utf8.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <functional>
#include <limits>
#include <vector>

constexpr int MAX_COST = std::numeric_limits<int>::max();

void ParsedText::addWord(std::string word, const EpdFontFamily::Style fontStyle) {
  if (word.empty()) return;

  words.push_back(std::move(word));
  wordStyles.push_back(fontStyle);
}

bool ParsedText::isEffectivelyEmpty() const {
  Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Checking block with %d words\n", millis(), words.size());
  if (words.empty()) {
    Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Words list is empty, returning true\n", millis());
    return true;
  }
  
  // Check if all words are whitespace-only
  int wordIndex = 0;
  for (const std::string& word : words) {
    Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Word[%d]: len=%d, empty=%d\n", millis(), wordIndex, word.length(), word.empty());
    if (!word.empty()) {
      bool isWhitespace = true;
      const unsigned char* utf8Ptr = reinterpret_cast<const unsigned char*>(word.c_str());
      const unsigned char* utf8End = utf8Ptr + word.length();
      
      while (utf8Ptr < utf8End) {
        uint32_t cp = utf8NextCodepoint(&utf8Ptr);
        if (cp == 0) break;
        
        // Check if codepoint is whitespace (including non-breaking space 0xA0)
        if (cp != ' ' && cp != '\t' && cp != '\n' && cp != '\r' && cp != 0xA0) {
          Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Word[%d] has non-whitespace codepoint: 0x%04X\n", 
                        millis(), wordIndex, cp);
          isWhitespace = false;
          break;
        }
      }
      
      if (!isWhitespace) {
        Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Found non-whitespace word, returning false\n", millis());
        return false;  // Found a non-whitespace word
      } else {
        Serial.printf("[%lu] [PTX] isEffectivelyEmpty: Word[%d] is whitespace-only\n", millis(), wordIndex);
      }
    }
    wordIndex++;
  }
  Serial.printf("[%lu] [PTX] isEffectivelyEmpty: All words are whitespace or empty, returning true\n", millis());
  return true;  // All words are whitespace or empty
}

void ParsedText::setIsHeaderIfChapter() {
  // Check if this looks like a chapter header: "Chapter" followed by a number
  // Must have 2-4 words, first word is "Chapter" (case-insensitive), second word is a number
  if (words.empty()) {
    return;
  }
  
  auto it = words.begin();
  std::string firstWord = *it;
  std::string secondWord = (++it != words.end()) ? *it : "";
  
  // Convert to lowercase for comparison
  std::string firstWordLower = firstWord;
  std::transform(firstWordLower.begin(), firstWordLower.end(), firstWordLower.begin(), ::tolower);
  
  // Check if it's just "Chapter" (1 word) - might be split across paragraphs
  if (firstWordLower == "chapter" && words.size() == 1) {
    isHeader = true;
    return;
  }
  
  // Check if it's just a number (1 word) - might be the second part of "Chapter X"
  if (words.size() == 1) {
    bool isNumber = !firstWord.empty();
    for (char c : firstWord) {
      if (!std::isdigit(c)) {
        isNumber = false;
        break;
      }
    }
    if (isNumber) {
      isHeader = true;
      return;
    }
  }
  
  if (firstWordLower == "chapter" && words.size() >= 2 && words.size() <= 4) {
    // Check if second word is a number (all digits)
    bool isNumber = !secondWord.empty();
    for (char c : secondWord) {
      if (!std::isdigit(c)) {
        isNumber = false;
        break;
      }
    }
    if (isNumber) {
      isHeader = true;
    }
  }
}

void ParsedText::mergeFrom(const ParsedText& other) {
  // Merge all words and styles from other into this
  for (const auto& word : other.words) {
    words.push_back(word);
  }
  for (const auto& style : other.wordStyles) {
    wordStyles.push_back(style);
  }
  // If other is a header, this should also be a header
  if (other.isHeader) {
    isHeader = true;
  }
}

int ParsedText::getHeaderFontId(int baseFontId) const {
  // Map base font ID to a larger version for headers
  // This is a simple mapping - in practice, you might want a more sophisticated approach
  // For now, we'll try to use the next size up, or fall back to a fixed larger font
  
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

int ParsedText::getDropCapFontId(int baseFontId) const {
  // Get font ID that's much larger for drop cap (should span 2 lines)
  // Go to the largest available font size to minimize pixelation
  // We'll render it at normal size (not scaled) for better quality
  Serial.printf("[%lu] [PTX] getDropCapFontId: starting with baseFontId=%d\n", millis(), baseFontId);
  int fontId = baseFontId;
  // Keep going up until we reach the largest size
  int prevFontId = fontId;
  for (int i = 0; i < 10; i++) {  // Max 10 iterations to prevent infinite loop
    prevFontId = fontId;
    fontId = getHeaderFontId(fontId);
    Serial.printf("[%lu] [PTX] getDropCapFontId: iteration %d, prevFontId=%d, newFontId=%d\n", millis(), i, prevFontId, fontId);
    if (fontId == prevFontId) {
      // Reached the largest size, stop
      Serial.printf("[%lu] [PTX] getDropCapFontId: reached largest size, stopping\n", millis());
      break;
    }
  }
  Serial.printf("[%lu] [PTX] getDropCapFontId: returning fontId=%d\n", millis(), fontId);
  return fontId;
}

// Consumes data to minimize memory usage
void ParsedText::layoutAndExtractLines(const GfxRenderer& renderer, const int fontId, const uint16_t viewportWidth,
                                       const std::function<void(std::shared_ptr<TextBlock>)>& processLine,
                                       const bool includeLastLine) {
  if (words.empty()) {
    return;
  }

  // Use larger font for headers
  const int actualFontId = isHeader ? getHeaderFontId(fontId) : fontId;

  // Extract drop cap character if needed
  int dropCapWidth = 0;
  int dropCapFontId = actualFontId;
  if (hasDropCap && !words.empty()) {
    Serial.printf("[%lu] [PTX] layoutAndExtractLines: hasDropCap=true, wordCount=%d\n", millis(), words.size());
    // Find the first non-whitespace word for the drop cap
    auto wordIt = words.begin();
    auto styleIt = wordStyles.begin();
    std::string* firstWord = nullptr;
    EpdFontFamily::Style firstWordStyle = EpdFontFamily::REGULAR;
    
    // Skip whitespace-only words
    int wordIndex = 0;
    while (wordIt != words.end()) {
      std::string& word = *wordIt;
      Serial.printf("[%lu] [PTX] Checking word[%d] for drop cap: len=%d\n", millis(), wordIndex, word.length());
      if (!word.empty()) {
        // Check if word contains only whitespace
        bool isWhitespace = true;
        for (char c : word) {
          if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && static_cast<unsigned char>(c) != 0xA0) {
            isWhitespace = false;
            break;
          }
        }
        if (!isWhitespace) {
          Serial.printf("[%lu] [PTX] Found non-whitespace word[%d] for drop cap: '%s'\n", millis(), wordIndex, word.c_str());
          firstWord = &word;
          firstWordStyle = (styleIt != wordStyles.end()) ? *styleIt : EpdFontFamily::REGULAR;
          break;
        } else {
          Serial.printf("[%lu] [PTX] Word[%d] is whitespace-only, skipping\n", millis(), wordIndex);
        }
      }
      ++wordIt;
      if (styleIt != wordStyles.end()) ++styleIt;
      wordIndex++;
    }
    
    if (firstWord == nullptr) {
      Serial.printf("[%lu] [PTX] No non-whitespace word found for drop cap - all words were whitespace\n", millis());
    }
    
    if (firstWord != nullptr && !firstWord->empty()) {
      Serial.printf("[%lu] [PTX] Extracting drop cap from word: '%s'\n", millis(), firstWord->c_str());
      // Extract first character (handle UTF-8)
      const unsigned char* utf8Start = reinterpret_cast<const unsigned char*>(firstWord->c_str());
      const unsigned char* utf8End = utf8Start + firstWord->length();
      const unsigned char* utf8Ptr = utf8Start;
      
      uint32_t cp = utf8NextCodepoint(&utf8Ptr);
      
      // Bounds check: ensure utf8Ptr didn't go past the end
      if (utf8Ptr > utf8End || cp == 0) {
        // Skip drop cap extraction to avoid crash
        Serial.printf("[%lu] [PTX] Drop cap extraction failed: bounds check failed or cp==0\n", millis());
        hasDropCap = false;
      } else {
        Serial.printf("[%lu] [PTX] Drop cap codepoint: 0x%04X\n", millis(), cp);
        // Check if it's a whitespace character - if so, skip drop cap
        if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0) {
          Serial.printf("[%lu] [PTX] Drop cap codepoint is whitespace, skipping\n", millis());
          hasDropCap = false;
        } else {
          Serial.printf("[%lu] [PTX] Drop cap codepoint is valid, encoding to UTF-8\n", millis());
          
          // Check if first character is a quotation mark
          bool isQuote = (cp == 0x0022 || cp == 0x0027 ||  // " and '
                          cp == 0x201C || cp == 0x201D ||  // " and "
                          cp == 0x2018 || cp == 0x2019);   // ' and '
          
          // Calculate bytes to skip for first character
          const size_t firstCharBytes = utf8Ptr - utf8Start;
          
          // Encode first character to UTF-8
          char utf8Char1[5] = {0};
          uint8_t* out1 = reinterpret_cast<uint8_t*>(utf8Char1);
          if (cp < 0x80) {
            out1[0] = static_cast<uint8_t>(cp);
            out1[1] = 0;
          } else if (cp < 0x800) {
            out1[0] = static_cast<uint8_t>(0xC0 | (cp >> 6));
            out1[1] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
            out1[2] = 0;
          } else if (cp < 0x10000) {
            out1[0] = static_cast<uint8_t>(0xE0 | (cp >> 12));
            out1[1] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
            out1[2] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
            out1[3] = 0;
          } else {
            out1[0] = static_cast<uint8_t>(0xF0 | (cp >> 18));
            out1[1] = static_cast<uint8_t>(0x80 | ((cp >> 12) & 0x3F));
            out1[2] = static_cast<uint8_t>(0x80 | ((cp >> 6) & 0x3F));
            out1[3] = static_cast<uint8_t>(0x80 | (cp & 0x3F));
            out1[4] = 0;
          }
          
          std::string dropCapString = utf8Char1;
          size_t bytesToSkip = firstCharBytes;
          
          // If it's a quote, also extract the next character
          if (isQuote && utf8Ptr < utf8End) {
            const unsigned char* utf8Ptr2 = utf8Ptr;
            uint32_t cp2 = utf8NextCodepoint(&utf8Ptr2);
            
            if (cp2 != 0 && utf8Ptr2 <= utf8End) {
              Serial.printf("[%lu] [PTX] First char is quote, also extracting next char: 0x%04X\n", millis(), cp2);
              
              // Encode second character to UTF-8
              char utf8Char2[5] = {0};
              uint8_t* out2 = reinterpret_cast<uint8_t*>(utf8Char2);
              if (cp2 < 0x80) {
                out2[0] = static_cast<uint8_t>(cp2);
                out2[1] = 0;
              } else if (cp2 < 0x800) {
                out2[0] = static_cast<uint8_t>(0xC0 | (cp2 >> 6));
                out2[1] = static_cast<uint8_t>(0x80 | (cp2 & 0x3F));
                out2[2] = 0;
              } else if (cp2 < 0x10000) {
                out2[0] = static_cast<uint8_t>(0xE0 | (cp2 >> 12));
                out2[1] = static_cast<uint8_t>(0x80 | ((cp2 >> 6) & 0x3F));
                out2[2] = static_cast<uint8_t>(0x80 | (cp2 & 0x3F));
                out2[3] = 0;
              } else {
                out2[0] = static_cast<uint8_t>(0xF0 | (cp2 >> 18));
                out2[1] = static_cast<uint8_t>(0x80 | ((cp2 >> 12) & 0x3F));
                out2[2] = static_cast<uint8_t>(0x80 | ((cp2 >> 6) & 0x3F));
                out2[3] = static_cast<uint8_t>(0x80 | (cp2 & 0x3F));
                out2[4] = 0;
              }
              
              dropCapString += utf8Char2;
              bytesToSkip = utf8Ptr2 - utf8Start;
            }
          }
          
          dropCapChar = dropCapString;
          dropCapStyle = firstWordStyle;
          
          // Use the same font ID, scale it 3x during rendering
          // Calculate width using the base font, then multiply by 3 for the scaled size
          dropCapFontId = actualFontId;  // Use same font, will be scaled 3x
          const int baseWidth = renderer.getTextWidth(dropCapFontId, dropCapChar.c_str(), dropCapStyle);
          dropCapWidth = baseWidth * 3;  // Scale 3x
          
          // Only proceed if we have a valid width
          Serial.printf("[%lu] [PTX] Drop cap width: %d\n", millis(), dropCapWidth);
          if (dropCapWidth > 0) {
            Serial.printf("[%lu] [PTX] Drop cap successfully extracted: '%s'\n", millis(), dropCapChar.c_str());
            // Remove first character from first word
            if (bytesToSkip > 0 && bytesToSkip < firstWord->length()) {
              *firstWord = firstWord->substr(bytesToSkip);
            } else if (bytesToSkip >= firstWord->length() || bytesToSkip == 0) {
              // Entire word was consumed or no bytes to skip, remove it
              firstWord->clear();
            }
            if (firstWord->empty()) {
              // If first word is now empty, remove it
              words.erase(wordIt);
              if (styleIt != wordStyles.end()) {
                wordStyles.erase(styleIt);
              }
            }
          } else {
            // No width means invalid character, skip drop cap
            hasDropCap = false;
          }
        }
      }
    } else {
      // No valid word found, skip drop cap
      hasDropCap = false;
    }
  }

  const int pageWidth = viewportWidth;
  const int spaceWidth = renderer.getSpaceWidth(actualFontId);
  const auto wordWidths = calculateWordWidths(renderer, actualFontId);
  
  // Adjust page width for first 2 lines if we have a drop cap
  // Since drop cap is rendered at 3x scale, dropCapWidth already includes 3x scaling
  const int scaledDropCapWidth = (hasDropCap && dropCapWidth > 0) ? dropCapWidth : 0;
  // Increase gap if drop cap contains a quotation mark (to push text further right)
  int dropCapGap = 5;  // Default gap between drop cap and text
  if (hasDropCap && !dropCapChar.empty()) {
    // Check if drop cap starts with a quote (first character is quote)
    const unsigned char* utf8Ptr = reinterpret_cast<const unsigned char*>(dropCapChar.c_str());
    uint32_t firstCp = utf8NextCodepoint(&utf8Ptr);
    if (firstCp == 0x0022 || firstCp == 0x0027 ||  // " and '
        firstCp == 0x201C || firstCp == 0x201D ||  // " and "
        firstCp == 0x2018 || firstCp == 0x2019) {   // ' and '
      dropCapGap = 8;  // Slightly larger gap for quotes
    }
  }
  const int effectivePageWidth = (hasDropCap && dropCapWidth > 0) 
                                 ? pageWidth - scaledDropCapWidth - dropCapGap
                                 : pageWidth;
  
  // For drop cap paragraphs: use effectivePageWidth for line breaking (so lines 1-2 fit correctly)
  // For non-drop-cap paragraphs: use full pageWidth
  const int breakWidth = (hasDropCap && dropCapWidth > 0) ? effectivePageWidth : pageWidth;
  const auto lineBreakIndices = computeLineBreaks(breakWidth, spaceWidth, wordWidths);
  
  const size_t lineCount = includeLastLine ? lineBreakIndices.size() : lineBreakIndices.size() - 1;

  for (size_t i = 0; i < lineCount; ++i) {
    // Adjust x position and page width for first 2 lines if we have a drop cap
    const bool isDropCapLine = (hasDropCap && dropCapWidth > 0 && i < 2);
    const int lineXOffset = isDropCapLine 
                           ? scaledDropCapWidth + dropCapGap  // Offset text to the right of drop cap (2x width + gap)
                           : 0;
    
    // For lines 1-2 with drop cap: use effectivePageWidth
    // For lines 3+: use full pageWidth so text extends to full screen width
    // (The line breaks were computed with effectivePageWidth, but we'll use full width for spacing)
    const int linePageWidth = isDropCapLine ? effectivePageWidth : pageWidth;
    extractLine(i, linePageWidth, spaceWidth, wordWidths, lineBreakIndices, processLine, actualFontId, lineXOffset);
  }
}

std::vector<uint16_t> ParsedText::calculateWordWidths(const GfxRenderer& renderer, const int fontId) {
  const size_t totalWordCount = words.size();

  std::vector<uint16_t> wordWidths;
  wordWidths.reserve(totalWordCount);

  // add em-space at the beginning of first word in paragraph to indent
  if (!extraParagraphSpacing) {
    std::string& first_word = words.front();
    first_word.insert(0, "\xe2\x80\x83");
  }

  auto wordsIt = words.begin();
  auto wordStylesIt = wordStyles.begin();

  while (wordsIt != words.end()) {
    wordWidths.push_back(renderer.getTextWidth(fontId, wordsIt->c_str(), *wordStylesIt));

    std::advance(wordsIt, 1);
    std::advance(wordStylesIt, 1);
  }

  return wordWidths;
}

std::vector<size_t> ParsedText::computeLineBreaks(const int pageWidth, const int spaceWidth,
                                                  const std::vector<uint16_t>& wordWidths) const {
  const size_t totalWordCount = words.size();

  // If this is a header, keep all words on one line (no breaks)
  if (isHeader && totalWordCount > 0) {
    std::vector<size_t> lineBreakIndices;
    lineBreakIndices.push_back(totalWordCount);  // All words on one line
    return lineBreakIndices;
  }

  // DP table to store the minimum badness (cost) of lines starting at index i
  std::vector<int> dp(totalWordCount);
  // 'ans[i]' stores the index 'j' of the *last word* in the optimal line starting at 'i'
  std::vector<size_t> ans(totalWordCount);

  // Base Case
  dp[totalWordCount - 1] = 0;
  ans[totalWordCount - 1] = totalWordCount - 1;

  for (int i = totalWordCount - 2; i >= 0; --i) {
    int currlen = -spaceWidth;
    dp[i] = MAX_COST;

    for (size_t j = i; j < totalWordCount; ++j) {
      // Current line length: previous width + space + current word width
      currlen += wordWidths[j] + spaceWidth;

      if (currlen > pageWidth) {
        break;
      }

      int cost;
      if (j == totalWordCount - 1) {
        cost = 0;  // Last line
      } else {
        const int remainingSpace = pageWidth - currlen;
        // Use long long for the square to prevent overflow
        const long long cost_ll = static_cast<long long>(remainingSpace) * remainingSpace + dp[j + 1];

        if (cost_ll > MAX_COST) {
          cost = MAX_COST;
        } else {
          cost = static_cast<int>(cost_ll);
        }
      }

      if (cost < dp[i]) {
        dp[i] = cost;
        ans[i] = j;  // j is the index of the last word in this optimal line
      }
    }

    // Handle oversized word: if no valid configuration found, force single-word line
    // This prevents cascade failure where one oversized word breaks all preceding words
    if (dp[i] == MAX_COST) {
      ans[i] = i;  // Just this word on its own line
      // Inherit cost from next word to allow subsequent words to find valid configurations
      if (i + 1 < static_cast<int>(totalWordCount)) {
        dp[i] = dp[i + 1];
      } else {
        dp[i] = 0;
      }
    }
  }

  // Stores the index of the word that starts the next line (last_word_index + 1)
  std::vector<size_t> lineBreakIndices;
  size_t currentWordIndex = 0;

  while (currentWordIndex < totalWordCount) {
    size_t nextBreakIndex = ans[currentWordIndex] + 1;

    // Safety check: prevent infinite loop if nextBreakIndex doesn't advance
    if (nextBreakIndex <= currentWordIndex) {
      // Force advance by at least one word to avoid infinite loop
      nextBreakIndex = currentWordIndex + 1;
    }

    lineBreakIndices.push_back(nextBreakIndex);
    currentWordIndex = nextBreakIndex;
  }

  return lineBreakIndices;
}

void ParsedText::extractLine(const size_t breakIndex, const int pageWidth, const int spaceWidth,
                             const std::vector<uint16_t>& wordWidths, const std::vector<size_t>& lineBreakIndices,
                             const std::function<void(std::shared_ptr<TextBlock>)>& processLine, const int fontId, const int xOffset) {
  const size_t lineBreak = lineBreakIndices[breakIndex];
  const size_t lastBreakAt = breakIndex > 0 ? lineBreakIndices[breakIndex - 1] : 0;
  const size_t lineWordCount = lineBreak - lastBreakAt;

  // Calculate total word width for this line
  int lineWordWidthSum = 0;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    lineWordWidthSum += wordWidths[i];
  }

  // Calculate spacing
  const int spareSpace = pageWidth - lineWordWidthSum;

  int spacing = spaceWidth;
  const bool isLastLine = breakIndex == lineBreakIndices.size() - 1;

  if (style == TextBlock::JUSTIFIED && !isLastLine && lineWordCount >= 2) {
    spacing = spareSpace / (lineWordCount - 1);
  }

  // Calculate initial x position
  uint16_t xpos = 0;
  if (style == TextBlock::RIGHT_ALIGN) {
    xpos = spareSpace - (lineWordCount - 1) * spaceWidth;
  } else if (style == TextBlock::CENTER_ALIGN) {
    xpos = (spareSpace - (lineWordCount - 1) * spaceWidth) / 2;
  }

  // Pre-calculate X positions for words (with drop cap offset if needed)
  std::list<uint16_t> lineXPos;
  for (size_t i = lastBreakAt; i < lineBreak; i++) {
    const uint16_t currentWordWidth = wordWidths[i];
    lineXPos.push_back(xpos + xOffset);  // Add xOffset for drop cap spacing
    xpos += currentWordWidth + spacing;
  }

  // Iterators always start at the beginning as we are moving content with splice below
  auto wordEndIt = words.begin();
  auto wordStyleEndIt = wordStyles.begin();
  std::advance(wordEndIt, lineWordCount);
  std::advance(wordStyleEndIt, lineWordCount);

  // *** CRITICAL STEP: CONSUME DATA USING SPLICE ***
  std::list<std::string> lineWords;
  lineWords.splice(lineWords.begin(), words, words.begin(), wordEndIt);
  std::list<EpdFontFamily::Style> lineWordStyles;
  lineWordStyles.splice(lineWordStyles.begin(), wordStyles, wordStyles.begin(), wordStyleEndIt);

  processLine(std::make_shared<TextBlock>(std::move(lineWords), std::move(lineXPos), std::move(lineWordStyles), style));
}
