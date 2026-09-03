#pragma once

#include "jpgd.h"

#include <cstddef>
#include <cstdint>

namespace jpgd {

// Random-access backing store for progressive DCT coefficients.
// Cover generation on ESP32-C3 cannot hold a 1000×1504 coeff array in SRAM
// (~3–4.5 MB). Each scan is sequential, so one/few cached block-rows suffice.
struct jpeg_decoder_spill_io {
  void* ctx = nullptr;
  int (*pread)(void* ctx, uint64_t offset, void* buf, int n) = nullptr;
  int (*pwrite)(void* ctx, uint64_t offset, const void* buf, int n) = nullptr;
};

bool jpgd_spill_begin(const jpeg_decoder_spill_io* io);
bool jpgd_spill_active();
void jpgd_spill_end();
void jpgd_spill_flush();

// Bump-allocate a zero-on-first-touch region. Returns byte offset, or -1.
int64_t jpgd_spill_alloc_region(size_t bytes);

// Pointer to the block_size-byte block at (bx, by). writable=false is IDCT/read.
jpgd_block_coeff_t* jpgd_spill_getp(int64_t region_ofs, int block_size, int nx, int ny, int bx, int by,
                                    bool writable = true);

}  // namespace jpgd
