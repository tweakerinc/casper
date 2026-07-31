#pragma once

#include <string>

// Visual cue for settings rows that belong to the item above them
// (e.g. Theme → Left Button). Keep punctuation out of i18n strings.
namespace NestedMenuLabel {

inline constexpr const char* kPrefix = "  · ";

inline std::string format(const char* label, const bool nested) {
  if (!label || !*label) return {};
  if (!nested) return std::string(label);
  return std::string(kPrefix) + label;
}

inline std::string format(const std::string& label, const bool nested) {
  if (!nested) return label;
  return std::string(kPrefix) + label;
}

}  // namespace NestedMenuLabel
