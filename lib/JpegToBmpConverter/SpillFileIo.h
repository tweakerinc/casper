#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace coverdecode {
// SdFat cannot seek beyond EOF. Progressive coefficient regions are sparse in
// their logical address space, so materialize a gap in bounded pieces first.
// The store is disposable; durability is not needed on every coefficient row.
// A negative read means a real I/O failure, zero means an untouched region.
template<class File>
int readSpill(File& f, uint64_t offset, void* out, int bytes) {
  if (!f || !out || bytes <= 0) return -1;
  const uint64_t size = f.fileSize64();
  if (offset >= size) return 0;
  if (!f.seek64(offset)) return -1;
  const auto wanted = static_cast<size_t>(std::min<uint64_t>(static_cast<unsigned>(bytes), size-offset));
  const int got = f.read(out, wanted);
  return got == static_cast<int>(wanted) ? got : -1;
}

template<class File>
int writeSpill(File& f, uint64_t offset, const void* data, int bytes, void (*service)() = nullptr) {
  // Current converter caps source dimensions at 2048 x 3072. Bound disk use
  // even for malformed dimensions/offset arithmetic passed by a decoder.
  constexpr uint64_t kMaxStore = 32ULL * 1024ULL * 1024ULL;
  if (!f || !data || bytes <= 0 || offset > kMaxStore || uint64_t(bytes) > kMaxStore-offset) return -1;
  uint64_t size = f.fileSize64();
  if (offset > size) {
    if (!f.seek64(size)) return -1;
    const uint8_t zero[512]{};
    while (size < offset) {
      const size_t n = static_cast<size_t>(std::min<uint64_t>(sizeof(zero), offset-size));
      if (f.write(zero, n) != n) return -1;
      size += n;
      if (service) service();
    }
  }
  if (!f.seek64(offset)) return -1;
  const size_t wrote = f.write(data, static_cast<size_t>(bytes));
  if (service) service();
  return wrote == static_cast<size_t>(bytes) ? bytes : -1;
}
} // namespace coverdecode
