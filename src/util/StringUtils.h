#pragma once
#include <string>

namespace StringUtils {

/**
 * Sanitize a string for use as a filename.
 * Replaces invalid characters with underscores, trims spaces/dots,
 * and limits length to maxLength characters.
 */
std::string sanitizeFilename(const std::string& name, size_t maxLength = 100);

/**
 * Check if a file path has the given extension (case-insensitive).
 * @param path The file path to check
 * @param extension The extension to check for (should include the dot, e.g. ".epub")
 * @return true if the path ends with the extension (case-insensitive)
 */
bool checkFileExtension(const std::string& path, const std::string& extension);

}  // namespace StringUtils
