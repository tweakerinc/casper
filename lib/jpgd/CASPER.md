# jpgd (Casper)

Vendored from [jpeg-compressor](https://github.com/richgel999/jpeg-compressor) (`jpgd.h` / `jpgd.cpp`).
Public Domain or Apache 2.0 (see the header in `jpgd.cpp`).

Casper uses this only for **progressive EPUB sleep/home jacket** decode.
In-chapter images stay on JPEGDEC (including its 1/8 DC-only progressive path).

## Why

JPEGDEC forces progressive streams to `JPEG_SCALE_EIGHTH`. A 1000×1504 jacket
becomes 125×188, then bilinear-upscaled — muddy vs a baseline cover of the same
size (Empire of the Vampire vs DCC *This Inevitable Ruin*).

Full progressive decode needs the DCT coefficient arrays (~3–4.5 MB for that
jacket). ESP32-C3 cannot hold that in SRAM. `jpgd_spill.cpp` keeps a 4-slot
block-row cache and spills the rest to `/.crosspoint/.jpgd_coeff.tmp`.

## Casper patches

- `JPGD_IN_BUF_SIZE` 2048; `alloc()` chunks 2 KB
- `JPGD_USE_SSE2 0`; `cFlagDisableSIMD`
- `cFlagCoverDecode`: skip RGBA sample/scanline buffers; luma via `copy_luma_block()`
- `coeff_buf.spill_ofs` + `jpgd_spill_*` when spill is active
- Yield once per MCU column in `decode_scan`
- Arduino: `assert` compiled out
- Cover decode skips YCbCr lookup tables and MCU coefficient scratch
