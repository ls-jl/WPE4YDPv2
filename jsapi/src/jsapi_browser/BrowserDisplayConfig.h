#pragma once

#include <string>

namespace browser {
namespace display {

bool parseJsonIntField(const std::string& text, const std::string& key, int& value);
std::string parseJsonStringField(const std::string& text, const std::string& key);
int normalizeRotationValue(int value, int fallback);
std::string currentDrmModeSpec();

} // namespace display
} // namespace browser
