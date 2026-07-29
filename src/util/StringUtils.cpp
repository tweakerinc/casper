#include "StringUtils.h"

#include <Utf8.h>

namespace StringUtils {

std::string sanitizeFilename(const std::string& name, size_t maxBytes) {
  std::string result;
  result.reserve(std::min(name.size(), maxBytes));

  const auto* text = reinterpret_cast<const unsigned char*>(name.c_str());

  // Skip leading spaces and dots so they don't consume the byte budget
  while (*text == ' ' || *text == '.') {
    text++;
  }

  // Process full UTF-8 codepoints to avoid trimming in the middle of a multibyte sequence
  while (*text != 0) {
    const auto* cpStart = text;
    uint32_t cp = utf8NextCodepoint(&text);

    if (cp == '/' || cp == '\\' || cp == ':' || cp == '*' || cp == '?' || cp == '"' || cp == '<' || cp == '>' ||
        cp == '|') {
      // Replace illegal and control characters with '_'
      if (result.length() + 1 > maxBytes) break;
      result += '_';
    } else if (cp >= 128 || (cp >= 32 && cp < 127)) {
      const size_t cpBytes = text - cpStart;
      if (result.length() + cpBytes > maxBytes) break;
      result.append(reinterpret_cast<const char*>(cpStart), cpBytes);
    }
  }

  // Trim trailing spaces and dots
  size_t end = result.find_last_not_of(" .");
  if (end != std::string::npos) {
    result.resize(end + 1);
  } else {
    result.clear();
  }

  return result.empty() ? "book" : result;
}

namespace {

std::string trimCopy(const std::string& s) {
  size_t start = 0;
  while (start < s.size() && (s[start] == ' ' || s[start] == '\t')) ++start;
  size_t end = s.size();
  while (end > start && (s[end - 1] == ' ' || s[end - 1] == '\t')) --end;
  return s.substr(start, end - start);
}

// "Last, First" or "Last, First Middle" → "First Last" / "First Middle Last".
// Leaves strings with no comma, or more than one comma, unchanged.
std::string flipLastFirstSegment(const std::string& segment) {
  const std::string s = trimCopy(segment);
  if (s.empty()) return s;

  const size_t comma = s.find(',');
  if (comma == std::string::npos) return s;
  // More than one comma (e.g. "Last, First, Jr.") — do not guess.
  if (s.find(',', comma + 1) != std::string::npos) return s;

  const std::string last = trimCopy(s.substr(0, comma));
  const std::string first = trimCopy(s.substr(comma + 1));
  if (last.empty() || first.empty()) return s;
  return first + " " + last;
}

}  // namespace

std::string formatAuthorDisplayName(const std::string& author) {
  const std::string input = trimCopy(author);
  if (input.empty()) return input;

  // Split multi-author strings on common separators, flip each, rejoin with " & ".
  std::string out;
  size_t pos = 0;
  while (pos < input.size()) {
    size_t sep = std::string::npos;
    size_t sepLen = 0;
    const size_t amp = input.find(" & ", pos);
    const size_t andWord = input.find(" and ", pos);
    const size_t semi = input.find(';', pos);
    if (amp != std::string::npos) {
      sep = amp;
      sepLen = 3;
    }
    if (andWord != std::string::npos && (sep == std::string::npos || andWord < sep)) {
      sep = andWord;
      sepLen = 5;
    }
    if (semi != std::string::npos && (sep == std::string::npos || semi < sep)) {
      sep = semi;
      sepLen = 1;
    }

    const std::string part =
        flipLastFirstSegment(sep == std::string::npos ? input.substr(pos) : input.substr(pos, sep - pos));
    if (!part.empty()) {
      if (!out.empty()) out += " & ";
      out += part;
    }
    if (sep == std::string::npos) break;
    pos = sep + sepLen;
  }
  return out.empty() ? input : out;
}

}  // namespace StringUtils
