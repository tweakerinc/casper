#include "jpgd_spill.h"

#include <cstring>
#include <memory>
#include <new>

namespace jpgd {
namespace {

constexpr int kSlots = 4;

struct Slot {
  int64_t region_ofs = -1;
  int block_y = -1;
  int block_size = 0;
  int nx = 0;
  int row_bytes = 0;
  bool dirty = false;
  std::unique_ptr<uint8_t[]> data;
  int data_cap = 0;
};

jpeg_decoder_spill_io g_io;
bool g_active = false;
uint64_t g_bump = 0;
Slot g_slots[kSlots];
int g_clock = 0;

bool io_read(uint64_t offset, void* buf, int n) {
  if (!g_io.pread || n <= 0) return n == 0;
  return g_io.pread(g_io.ctx, offset, buf, n) == n;
}

bool io_write(uint64_t offset, const void* buf, int n) {
  if (!g_io.pwrite || n <= 0) return n == 0;
  return g_io.pwrite(g_io.ctx, offset, buf, n) == n;
}

bool flush_slot(Slot& s) {
  if (!s.dirty || s.region_ofs < 0 || !s.data) {
    s.dirty = false;
    return true;
  }
  const uint64_t off = static_cast<uint64_t>(s.region_ofs) + static_cast<uint64_t>(s.block_y) * static_cast<uint64_t>(s.row_bytes);
  if (!io_write(off, s.data.get(), s.row_bytes)) return false;
  s.dirty = false;
  return true;
}

Slot* find_slot(int64_t region_ofs, int block_y) {
  for (int i = 0; i < kSlots; ++i) {
    if (g_slots[i].region_ofs == region_ofs && g_slots[i].block_y == block_y) return &g_slots[i];
  }
  return nullptr;
}

Slot* evict_slot(int needed_row_bytes) {
  for (int i = 0; i < kSlots; ++i) {
    if (g_slots[i].region_ofs < 0) return &g_slots[i];
  }
  // Interleaved DC needs 4 tiny rows (Y/Y/Cb/Cr). Y AC rows are ~16 KB on a
  // 1000-wide jacket — keep at most two of those so C3 maxAlloc (~69 KB) holds.
  constexpr int kLargeRow = 1024;
  if (needed_row_bytes >= kLargeRow) {
    int largeIdx[kSlots];
    int nLarge = 0;
    for (int i = 0; i < kSlots; ++i) {
      if (g_slots[i].row_bytes >= kLargeRow) largeIdx[nLarge++] = i;
    }
    if (nLarge >= 2) {
      Slot& s = g_slots[largeIdx[g_clock++ % nLarge]];
      if (!flush_slot(s)) return nullptr;
      s.region_ofs = -1;
      s.block_y = -1;
      return &s;
    }
  }
  Slot& s = g_slots[g_clock++ % kSlots];
  if (!flush_slot(s)) return nullptr;
  s.region_ofs = -1;
  s.block_y = -1;
  return &s;
}

}  // namespace

bool jpgd_spill_begin(const jpeg_decoder_spill_io* io) {
  jpgd_spill_end();
  if (!io || !io->pread || !io->pwrite || !io->ctx) return false;
  g_io = *io;
  g_active = true;
  g_bump = 0;
  g_clock = 0;
  return true;
}

bool jpgd_spill_active() { return g_active; }

void jpgd_spill_flush() {
  for (int i = 0; i < kSlots; ++i) flush_slot(g_slots[i]);
}

void jpgd_spill_end() {
  jpgd_spill_flush();
  for (int i = 0; i < kSlots; ++i) {
    g_slots[i] = Slot{};
  }
  g_io = {};
  g_active = false;
  g_bump = 0;
}

int64_t jpgd_spill_alloc_region(size_t bytes) {
  if (!g_active) return -1;
  constexpr uint64_t align = 256;
  const uint64_t ofs = (g_bump + (align - 1)) & ~(align - 1);
  g_bump = ofs + ((bytes + (align - 1)) & ~(align - 1));
  return static_cast<int64_t>(ofs);
}

jpgd_block_coeff_t* jpgd_spill_getp(int64_t region_ofs, int block_size, int nx, int ny, int bx, int by, bool writable) {
  if (!g_active || region_ofs < 0 || block_size <= 0 || nx <= 0 || ny <= 0) return nullptr;
  if (bx < 0 || by < 0 || bx >= nx || by >= ny) return nullptr;

  const int row_bytes = block_size * nx;
  Slot* s = find_slot(region_ofs, by);
  if (!s) {
    s = evict_slot(row_bytes);
    if (!s) return nullptr;
    if (s->data_cap < row_bytes) {
      s->data.reset(new (std::nothrow) uint8_t[static_cast<size_t>(row_bytes)]);
      if (!s->data) return nullptr;
      s->data_cap = row_bytes;
    }
    s->region_ofs = region_ofs;
    s->block_y = by;
    s->block_size = block_size;
    s->nx = nx;
    s->row_bytes = row_bytes;
    s->dirty = false;
    memset(s->data.get(), 0, static_cast<size_t>(row_bytes));
    const uint64_t off = static_cast<uint64_t>(region_ofs) + static_cast<uint64_t>(by) * static_cast<uint64_t>(row_bytes);
    io_read(off, s->data.get(), row_bytes);
  }

  if (writable) s->dirty = true;
  return reinterpret_cast<jpgd_block_coeff_t*>(s->data.get() + bx * s->block_size);
}

}  // namespace jpgd
