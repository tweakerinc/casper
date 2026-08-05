#pragma once

#include <string>

// Visual cue for settings rows that belong to the item above them
// (e.g. Theme → Left Button). Keep punctuation out of i18n strings.
//
// Indent uses UTF-8 NBSP (\xC2\xA0), not ASCII spaces: list wrap splits on
// ' ' and would strip a leading "  · " when Text Wrapping is on, so nested
// rows jumped right when wrapping was turned off (spaces preserved).
namespace NestedMenuLabel {

inline constexpr const char* kPrefix = "\xC2\xA0\xC2\xA0\xC2\xB7 ";

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
