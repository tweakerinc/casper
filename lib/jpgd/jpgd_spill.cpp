#include "jpgd_spill.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <memory>
#include <new>

namespace jpgd {
namespace {

// AC scans are non-interleaved: one full coefficient row is sufficient.
// Interleaved DC scans revisit small Y/Cb/Cr rows within the same MCU row.
// Keep those rows in a separate bank so they cannot thrash the large AC row.
constexpr int kSmallSlots = 8;
constexpr int kSlots = kSmallSlots + 1;
constexpr size_t kSmallRow = 1024;
constexpr int kMaxRegions = JPGD_MAX_COMPONENTS * 2;
constexpr uint64_t kMaxStore = 32ULL * 1024ULL * 1024ULL;
constexpr uint32_t kUnwritten = std::numeric_limits<uint32_t>::max();

struct Region {
  int64_t logical = -1;
  size_t bytes = 0;
  int rows = 0;
  size_t rowBytes = 0;
  // Only the row directory is resident. Coefficients append to SD on first
  // write instead of materializing megabytes of unused logical-address gaps.
  std::unique_ptr<uint32_t[]> physicalRows;
};

struct Slot {
  int region = -1;
  int row = -1;
  bool dirty = false;
  uint64_t used = 0;
  std::unique_ptr<uint8_t[]> data;
  size_t capacity = 0;
};

jpeg_decoder_spill_io g_io;
bool g_active = false;
uint64_t g_logicalEnd = 0;
uint64_t g_physicalEnd = 0;
uint64_t g_clock = 0;
int g_regionCount = 0;
Region g_regions[kMaxRegions];
Slot g_slots[kSlots];

bool flush_slot(Slot& s) {
  if (!s.dirty || s.region < 0) return true;
  Region& region = g_regions[s.region];
  const uint32_t saved = region.physicalRows[s.row];
  const uint64_t offset = saved == kUnwritten ? g_physicalEnd : saved;
  if (offset > kMaxStore || region.rowBytes > kMaxStore - offset) return false;
  if (g_io.pwrite(g_io.ctx, offset, s.data.get(), static_cast<int>(region.rowBytes)) !=
      static_cast<int>(region.rowBytes)) return false;
  // Publish only complete rows. Short/failed writes must not become valid data.
  if (saved == kUnwritten) {
    region.physicalRows[s.row] = static_cast<uint32_t>(offset);
    g_physicalEnd += region.rowBytes;
  }
  s.dirty = false;
  return true;
}

int find_region(int64_t logical) {
  for (int i = 0; i < g_regionCount; ++i) {
    if (g_regions[i].logical == logical) return i;
  }
  return -1;
}

Slot* acquire_slot(int regionIndex, int row, size_t rowBytes) {
  const int begin = rowBytes <= kSmallRow ? 0 : kSmallSlots;
  const int end = rowBytes <= kSmallRow ? kSmallSlots : kSlots;
  Slot* victim = nullptr;
  for (int i = begin; i < end; ++i) {
    Slot& s = g_slots[i];
    if (s.region == regionIndex && s.row == row) {
      s.used = ++g_clock;
      return &s;
    }
    if (!victim || (s.region < 0 && victim->region >= 0) ||
        (s.region >= 0 && victim->region >= 0 && s.used < victim->used)) victim = &s;
  }
  if (!victim || !flush_slot(*victim)) return nullptr;
  victim->region = -1;
  victim->row = -1;
  if (victim->capacity < rowBytes) {
    victim->data.reset();
    victim->capacity = 0;
    victim->data.reset(new (std::nothrow) uint8_t[rowBytes]);
    if (!victim->data) return nullptr;
    victim->capacity = rowBytes;
  }
  Region& region = g_regions[regionIndex];
  const uint32_t physical = region.physicalRows[row];
  if (physical == kUnwritten) {
    std::memset(victim->data.get(), 0, rowBytes);
  } else if (g_io.pread(g_io.ctx, physical, victim->data.get(), static_cast<int>(rowBytes)) !=
             static_cast<int>(rowBytes)) {
    return nullptr;
  }
  victim->region = regionIndex;
  victim->row = row;
  victim->dirty = false;
  victim->used = ++g_clock;
  return victim;
}

}  // namespace

bool jpgd_spill_begin(const jpeg_decoder_spill_io* io) {
  jpgd_spill_end();
  if (!io || !io->pread || !io->pwrite || !io->ctx) return false;
  g_io = *io;
  g_active = true;
  return true;
}

bool jpgd_spill_active() { return g_active; }

bool jpgd_spill_flush() {
  bool ok = true;
  for (auto& slot : g_slots) if (!flush_slot(slot)) ok = false;
  return ok;
}

void jpgd_spill_end() {
  if (g_active) (void)jpgd_spill_flush();
  for (auto& slot : g_slots) slot = Slot{};
  for (auto& region : g_regions) region = Region{};
  g_io = {};
  g_active = false;
  g_logicalEnd = g_physicalEnd = g_clock = 0;
  g_regionCount = 0;
}

int64_t jpgd_spill_alloc_region(size_t bytes) {
  if (!g_active || !bytes || g_regionCount >= kMaxRegions || bytes > kMaxStore) return -1;
  constexpr uint64_t alignment = 256;
  const uint64_t rounded = (static_cast<uint64_t>(bytes) + alignment - 1) & ~(alignment - 1);
  if (rounded > kMaxStore - g_logicalEnd) return -1;
  Region& region = g_regions[g_regionCount++];
  region.logical = static_cast<int64_t>(g_logicalEnd);
  region.bytes = bytes;
  g_logicalEnd += rounded;
  return region.logical;
}

jpgd_block_coeff_t* jpgd_spill_getp(int64_t logical, int blockSize, int nx, int ny, int bx, int by, bool writable) {
  if (!g_active || logical < 0 || blockSize <= 0 || nx <= 0 || ny <= 0 || bx < 0 || by < 0 || bx >= nx || by >= ny)
    return nullptr;
  const int ri = find_region(logical);
  if (ri < 0) return nullptr;
  Region& region = g_regions[ri];
  // Validate geometry before multiplication, allocation, or a backing-store call.
  if (static_cast<size_t>(nx) > region.bytes / static_cast<size_t>(blockSize)) return nullptr;
  const size_t rowBytes = static_cast<size_t>(blockSize) * static_cast<size_t>(nx);
  if (static_cast<size_t>(ny) != region.bytes / rowBytes || region.bytes % rowBytes ||
      rowBytes > static_cast<size_t>(std::numeric_limits<int>::max())) return nullptr;
  if (!region.physicalRows) {
    region.physicalRows.reset(new (std::nothrow) uint32_t[static_cast<size_t>(ny)]);
    if (!region.physicalRows) return nullptr;
    region.rows = ny;
    region.rowBytes = rowBytes;
    std::fill_n(region.physicalRows.get(), ny, kUnwritten);
  } else if (region.rows != ny || region.rowBytes != rowBytes) {
    return nullptr;
  }
  Slot* slot = acquire_slot(ri, by, rowBytes);
  if (!slot) return nullptr;
  if (writable) slot->dirty = true;
  return reinterpret_cast<jpgd_block_coeff_t*>(slot->data.get() + static_cast<size_t>(bx) * blockSize);
}

}  // namespace jpgd
