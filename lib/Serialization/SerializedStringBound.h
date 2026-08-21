#pragma once

#include <cstddef>
#include <cstdint>

namespace serialization {

// Metadata strings (spine href, TOC title) are short. A torn concurrent
// seek on book.bin can hand back a 32-bit length that still "fits in the
// file" on a multi-megabyte cache, after which std::string::resize() calls
// abort() under -fno-exceptions (v51 device: persistHomeProgress →
// calculateProgress → getSpineItem → readString while the render task held
// the same HalFile).
inline constexpr uint32_t kMaxSerializedStringBytes = 16 * 1024;

inline constexpr bool serializedStringFits(const uint32_t len, const size_t remaining) {
  return len <= kMaxSerializedStringBytes && static_cast<size_t>(len) <= remaining;
}

}  // namespace serialization
