#pragma once
#include <cstddef>
#include <cstdint>

namespace casper_memory {
// Length-aware decoding: never reads past end and always advances malformed
// input. Invalid scalar values are represented by U+FFFD, not encoded again.
inline uint32_t nextUtf8(const char*& p, const char* end) {
  if (!p || p >= end) return 0;
  const uint8_t lead = static_cast<uint8_t>(*p++);
  if (lead < 0x80) return lead;
  unsigned count = 0;
  uint32_t cp = 0, minimum = 0;
  if (lead >= 0xC2 && lead <= 0xDF) { count = 1; cp = lead & 31; minimum = 0x80; }
  else if (lead >= 0xE0 && lead <= 0xEF) { count = 2; cp = lead & 15; minimum = 0x800; }
  else if (lead >= 0xF0 && lead <= 0xF4) { count = 3; cp = lead & 7; minimum = 0x10000; }
  else return 0xFFFD;
  if (static_cast<size_t>(end - p) < count) return 0xFFFD;
  const char* q = p;
  for (unsigned i = 0; i < count; ++i) {
    const uint8_t b = static_cast<uint8_t>(*q++);
    if ((b & 0xC0) != 0x80) return 0xFFFD;
    cp = (cp << 6) | (b & 63);
  }
  if (cp < minimum || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0xFFFD;
  p = q;
  return cp;
}
inline char* encodeUtf8(char* out, uint32_t cp) {
  if (cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) cp = 0xFFFD;
  if (cp < 0x80) *out++ = static_cast<char>(cp);
  else if (cp < 0x800) { *out++ = static_cast<char>(0xC0 | (cp >> 6)); *out++ = static_cast<char>(0x80 | (cp & 63)); }
  else if (cp < 0x10000) { *out++ = static_cast<char>(0xE0 | (cp >> 12)); *out++ = static_cast<char>(0x80 | ((cp >> 6) & 63)); *out++ = static_cast<char>(0x80 | (cp & 63)); }
  else { *out++ = static_cast<char>(0xF0 | (cp >> 18)); *out++ = static_cast<char>(0x80 | ((cp >> 12) & 63)); *out++ = static_cast<char>(0x80 | ((cp >> 6) & 63)); *out++ = static_cast<char>(0x80 | (cp & 63)); }
  return out;
}
// Largest prefix ending at a complete UTF-8 scalar, within limit bytes.
inline size_t utf8Prefix(const char* text, size_t length, size_t limit) {
  if (limit >= length) return length;
  size_t n = limit;
  while (n && (static_cast<uint8_t>(text[n]) & 0xC0) == 0x80) --n;
  return n;
}
}  // namespace casper_memory
