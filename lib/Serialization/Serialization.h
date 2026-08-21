#pragma once
#include <HalStorage.h>
#include <SerializedStringBound.h>

#include <iostream>
#include <limits>

namespace serialization {
template <typename T>
void writePod(std::ostream& os, const T& value) {
  os.write(reinterpret_cast<const char*>(&value), sizeof(T));
}

template <typename T>
void writePod(HalFile& file, const T& value) {
  file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T));
}

template <typename T>
bool tryWritePod(HalFile& file, const T& value) {
  return file.write(reinterpret_cast<const uint8_t*>(&value), sizeof(T)) == sizeof(T);
}

template <typename T>
void readPod(std::istream& is, T& value) {
  is.read(reinterpret_cast<char*>(&value), sizeof(T));
}

template <typename T>
void readPod(HalFile& file, T& value) {
  file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T));
}

template <typename T>
bool tryReadPod(HalFile& file, T& value) {
  return file.read(reinterpret_cast<uint8_t*>(&value), sizeof(T)) == static_cast<int>(sizeof(T));
}

inline void writeString(std::ostream& os, const std::string& s) {
  const uint32_t len = s.size();
  writePod(os, len);
  os.write(s.data(), len);
}

inline void writeString(HalFile& file, const std::string& s) {
  const uint32_t len = s.size();
  writePod(file, len);
  file.write(reinterpret_cast<const uint8_t*>(s.data()), len);
}

inline bool tryWriteString(HalFile& file, const std::string& s) {
  const uint32_t len = static_cast<uint32_t>(s.size());
  return tryWritePod(file, len) && (len == 0 || file.write(reinterpret_cast<const uint8_t*>(s.data()), len) == len);
}

inline void readString(std::istream& is, std::string& s) {
  uint32_t len;
  readPod(is, len);
  s.resize(len);
  is.read(&s[0], len);
}

// Reads a length-prefixed string, refusing any length the file cannot actually
// contain and any length above kMaxSerializedStringBytes.
//
// The old bound was `s.max_size()`, which on a 32-bit target is on the order of
// a gigabyte — so a corrupt or truncated file could still hand back a length of
// e.g. 0x40000000, sail past the check, and reach std::string::resize(). Under
// -fno-exceptions that throw becomes abort(), i.e. the device reboots because a
// cache file on the SD card went bad. A torn concurrent seek on a large book.bin
// has the same shape: the length still "fits in the file" but resize() aborts.
inline bool tryReadString(HalFile& file, std::string& s) {
  uint32_t len = 0;
  if (!tryReadPod(file, len)) {
    return false;
  }
  const size_t pos = file.position();
  const size_t total = file.size();
  const size_t remaining = (total > pos) ? (total - pos) : 0;
  if (!serializedStringFits(len, remaining)) {
    s.clear();
    return false;
  }
  if (static_cast<size_t>(len) > s.max_size() || len > static_cast<uint32_t>(std::numeric_limits<int>::max())) {
    s.clear();
    return false;
  }
  s.resize(len);
  const int readLen = static_cast<int>(len);
  return len == 0 || file.read(&s[0], readLen) == readLen;
}

inline void readString(HalFile& file, std::string& s) {
  if (!tryReadString(file, s)) {
    s.clear();
  }
}
}  // namespace serialization
