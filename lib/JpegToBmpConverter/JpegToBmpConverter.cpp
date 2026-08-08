#include "JpegToBmpConverter.h"

#include <HalDisplay.h>
#include <HalStorage.h>
#include <JPEGDEC.h>
#include <Logging.h>
#include <Memory.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstdio>
#include <cstring>

#include "BitmapHelpers.h"

// ============================================================================
// IMAGE PROCESSING OPTIONS - Toggle these to test different configurations
// ============================================================================
constexpr bool USE_8BIT_OUTPUT = false;  // true: 8-bit grayscale (no quantization), false: 2-bit (4 levels)
// Dithering method selection (only one should be true, or all false for simple quantization):
constexpr bool USE_ATKINSON = true;          // Atkinson dithering (cleaner than F-S, less error diffusion)
constexpr bool USE_FLOYD_STEINBERG = false;  // Floyd-Steinberg error diffusion (can cause "worm" artifacts)
constexpr bool USE_NOISE_DITHERING = false;  // Hash-based noise dithering (good for downsampling)
// Pre-resize to target display size (CRITICAL: avoids dithering artifacts from post-downsampling)
constexpr bool USE_PRESCALE = true;  // true: scale image to target size before dithering
// ============================================================================

inline void write16(Print& out, const uint16_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
}

inline void write32(Print& out, const uint32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

inline void write32Signed(Print& out, const int32_t value) {
  out.write(value & 0xFF);
  out.write((value >> 8) & 0xFF);
  out.write((value >> 16) & 0xFF);
  out.write((value >> 24) & 0xFF);
}

// Helper function: Write BMP header with 8-bit grayscale (256 levels)
void writeBmpHeader8bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 3) / 4 * 4;  // 8 bits per pixel, padded
  const int imageSize = bytesPerRow * height;
  const uint32_t paletteSize = 256 * 4;  // 256 colors * 4 bytes (BGRA)
  const uint32_t fileSize = 14 + 40 + paletteSize + imageSize;

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);
  write32(bmpOut, 0);                      // Reserved
  write32(bmpOut, 14 + 40 + paletteSize);  // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 8);              // Bits per pixel (8 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 256);   // colorsUsed
  write32(bmpOut, 256);   // colorsImportant

  // Color Palette (256 grayscale entries x 4 bytes = 1024 bytes)
  for (int i = 0; i < 256; i++) {
    bmpOut.write(static_cast<uint8_t>(i));  // Blue
    bmpOut.write(static_cast<uint8_t>(i));  // Green
    bmpOut.write(static_cast<uint8_t>(i));  // Red
    bmpOut.write(static_cast<uint8_t>(0));  // Reserved
  }
}

// Helper function: Write BMP header with 1-bit color depth (black and white)
static void writeBmpHeader1bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width + 31) / 32 * 4;  // 1 bit per pixel, round up to 4-byte boundary
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 62 + imageSize;  // 14 (file header) + 40 (DIB header) + 8 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 62);        // Offset to pixel data (14 + 40 + 8)

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 1);              // Bits per pixel (1 bit)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 2);     // colorsUsed
  write32(bmpOut, 2);     // colorsImportant

  // Color Palette (2 colors x 4 bytes = 8 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  // Note: In 1-bit BMP, palette index 0 = black, 1 = white
  uint8_t palette[8] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0xFF, 0xFF, 0xFF, 0x00   // Color 1: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

// Helper function: Write BMP header with 2-bit color depth
static void writeBmpHeader2bit(Print& bmpOut, const int width, const int height) {
  // Calculate row padding (each row must be multiple of 4 bytes)
  const int bytesPerRow = (width * 2 + 31) / 32 * 4;  // 2 bits per pixel, round up
  const int imageSize = bytesPerRow * height;
  const uint32_t fileSize = 70 + imageSize;  // 14 (file header) + 40 (DIB header) + 16 (palette) + image

  // BMP File Header (14 bytes)
  bmpOut.write('B');
  bmpOut.write('M');
  write32(bmpOut, fileSize);  // File size
  write32(bmpOut, 0);         // Reserved
  write32(bmpOut, 70);        // Offset to pixel data

  // DIB Header (BITMAPINFOHEADER - 40 bytes)
  write32(bmpOut, 40);
  write32Signed(bmpOut, width);
  write32Signed(bmpOut, -height);  // Negative height = top-down bitmap
  write16(bmpOut, 1);              // Color planes
  write16(bmpOut, 2);              // Bits per pixel (2 bits)
  write32(bmpOut, 0);              // BI_RGB (no compression)
  write32(bmpOut, imageSize);
  write32(bmpOut, 2835);  // xPixelsPerMeter (72 DPI)
  write32(bmpOut, 2835);  // yPixelsPerMeter (72 DPI)
  write32(bmpOut, 4);     // colorsUsed
  write32(bmpOut, 4);     // colorsImportant

  // Color Palette (4 colors x 4 bytes = 16 bytes)
  // Format: Blue, Green, Red, Reserved (BGRA)
  uint8_t palette[16] = {
      0x00, 0x00, 0x00, 0x00,  // Color 0: Black
      0x55, 0x55, 0x55, 0x00,  // Color 1: Dark gray (85)
      0xAA, 0xAA, 0xAA, 0x00,  // Color 2: Light gray (170)
      0xFF, 0xFF, 0xFF, 0x00   // Color 3: White
  };
  for (const uint8_t i : palette) {
    bmpOut.write(i);
  }
}

namespace {

// Max MCU height supported by any JPEG (4:2:0 chroma = 16 rows, 4:4:4 = 8 rows)
constexpr int MAX_MCU_HEIGHT = 16;
constexpr size_t JPEG_DECODER_SIZE = 20 * 1024;
constexpr size_t MIN_FREE_HEAP = JPEG_DECODER_SIZE + 32 * 1024;
constexpr uint32_t FP_ONE = 1UL << 16;

// Static file pointer for JPEGDEC open callback.
// Safe in single-threaded embedded context; never accessed concurrently.
static HalFile* s_jpegFile = nullptr;
static uint8_t s_jpegIoSinceYield = 0;

static void yieldToIdle() { vTaskDelay(1); }

static void yieldDuringJpegIo() {
  if (++s_jpegIoSinceYield < 4) return;
  s_jpegIoSinceYield = 0;
  yieldToIdle();
}

void* bmpJpegOpen(const char* /*filename*/, int32_t* size) {
  if (!s_jpegFile || !*s_jpegFile) return nullptr;
  s_jpegIoSinceYield = 0;
  s_jpegFile->seek(0);
  *size = static_cast<int32_t>(s_jpegFile->size());
  yieldDuringJpegIo();
  return s_jpegFile;
}

void bmpJpegClose(void* /*handle*/) {
  // Caller owns the file — do not close it here
}

int32_t bmpJpegRead(JPEGFILE* pFile, uint8_t* pBuf, int32_t len) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  int32_t n = f->read(pBuf, len);
  if (n < 0) n = 0;
  pFile->iPos += n;
  yieldDuringJpegIo();
  return n;
}

int32_t bmpJpegSeek(JPEGFILE* pFile, int32_t pos) {
  auto* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f || !f->seek(pos)) return -1;
  pFile->iPos = pos;
  yieldDuringJpegIo();
  return pos;
}

// Context passed to the JPEGDEC draw callback via setUserPointer()
struct BmpConvertCtx {
  Print* bmpOut;
  int srcWidth;
  int srcHeight;
  int outWidth;
  int outHeight;
  bool oneBit;
  bool coverHighQuality = false;  // home cover: 2-bit balanced Atkinson + mild lift
  int bytesPerRow;
  bool needsScaling;
  uint32_t scaleX_fp;  // source pixels per output pixel, 16.16 fixed-point
  uint32_t scaleY_fp;
  bool smoothUpscale;
  uint32_t smoothScaleX_fp;
  uint32_t smoothScaleY_fp;

  // Accumulates one MCU row (up to MAX_MCU_HEIGHT source rows × srcWidth pixels)
  // Filled column-by-column as JPEGDEC callbacks arrive for the same MCU row
  std::unique_ptr<uint8_t[]> mcuBuf;
  int mcuRowBaseY;        // blockY of MCU row currently being assembled (-1 = none)
  int mcuRowMaxH;         // max blockH seen across columns in this MCU row
  int lastSrcYProcessed;  // last source Y fully emitted (-1 = none)

  // Y-axis area averaging accumulators (needsScaling only)
  int currentOutY;
  uint32_t nextOutY_srcStart;  // 16.16 fixed-point boundary for the next output row
  std::unique_ptr<uint32_t[]> rowAccum;
  std::unique_ptr<uint32_t[]> rowCount;

  int smoothNextOutY;
  int smoothPrevY;
  std::unique_ptr<uint8_t[]> smoothRows;
  uint8_t* smoothPrevRow;
  uint8_t* smoothCurrRow;
  uint8_t* smoothOutRow;

  std::unique_ptr<uint8_t[]> bmpRow;
  // Scratch grayscale row (outWidth) used before dither / seam filter
  std::unique_ptr<uint8_t[]> grayRow;
  // Last good scaled gray row — empty Y-bins copy this instead of solid white/black
  // (solid fills become full-width horizontal hairlines after dither).
  std::unique_ptr<uint8_t[]> lastGoodGray;
  bool hasLastGoodGray = false;

  // Legacy seam fields kept zeroed (c29+ disabled the delay hairline filter).
  std::unique_ptr<uint8_t[]> seamA;
  std::unique_ptr<uint8_t[]> seamB;
  int seamAY;
  int seamBY;
  int seamHeld;  // 0, 1, or 2 rows buffered

  std::unique_ptr<AtkinsonDitherer> atkinsonDitherer;
  std::unique_ptr<FloydSteinbergDitherer> fsDitherer;
  std::unique_ptr<Atkinson1BitDitherer> atkinson1BitDitherer;

  uint8_t rowsSinceYield;
  uint8_t blocksSinceYield;
  bool error;
};

static void yieldDuringDecode(BmpConvertCtx* ctx) {
  if (++ctx->rowsSinceYield < 8) return;
  ctx->rowsSinceYield = 0;
  yieldToIdle();
}

static void yieldDuringDecodeBlock(BmpConvertCtx* ctx) {
  if (++ctx->blocksSinceYield < 16) return;
  ctx->blocksSinceYield = 0;
  yieldToIdle();
}

// Build grayscale for one output row into dst (length outWidth).
// Cover/2-bit/8-bit: apply contrast lift here. 1-bit leaves raw — Atkinson1Bit /
// quantize1bit apply their own lift internally.
static void buildAdjustedGrayRow(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dst) {
  if (ctx->oneBit) {
    memcpy(dst, srcRow, static_cast<size_t>(ctx->outWidth));
  } else if (USE_8BIT_OUTPUT) {
    for (int x = 0; x < ctx->outWidth; x++) {
      dst[x] = adjustPixel(srcRow[x]);
    }
  } else if (ctx->coverHighQuality) {
    for (int x = 0; x < ctx->outWidth; x++) {
      dst[x] = static_cast<uint8_t>(adjustPixelCoverThumb(srcRow[x]));
    }
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      dst[x] = static_cast<uint8_t>(adjustPixel(srcRow[x]));
    }
  }
}

// Dither (if needed) and write one already-adjusted grayscale row to the BMP.
static void ditherAndWriteGrayRow(BmpConvertCtx* ctx, const uint8_t* grayRow, int outY) {
  memset(ctx->bmpRow.get(), 0, ctx->bytesPerRow);

  if (USE_8BIT_OUTPUT && !ctx->oneBit) {
    memcpy(ctx->bmpRow.get(), grayRow, static_cast<size_t>(ctx->outWidth));
  } else if (ctx->oneBit) {
    for (int x = 0; x < ctx->outWidth; x++) {
      const uint8_t bit = ctx->atkinson1BitDitherer ? ctx->atkinson1BitDitherer->processPixel(grayRow[x], x)
                                                    : quantize1bit(grayRow[x], x, outY);
      ctx->bmpRow[x / 8] |= (bit << (7 - (x % 8)));
    }
    if (ctx->atkinson1BitDitherer) ctx->atkinson1BitDitherer->nextRow();
  } else {
    for (int x = 0; x < ctx->outWidth; x++) {
      uint8_t twoBit;
      if (ctx->fsDitherer) {
        twoBit = ctx->fsDitherer->processPixel(grayRow[x], x);
      } else if (ctx->atkinsonDitherer) {
        twoBit = ctx->atkinsonDitherer->processPixel(grayRow[x], x);
      } else {
        twoBit = quantize(grayRow[x], x, outY);
      }
      ctx->bmpRow[(x * 2) / 8] |= (twoBit << (6 - ((x * 2) % 8)));
    }
    if (ctx->fsDitherer)
      ctx->fsDitherer->nextRow();
    else if (ctx->atkinsonDitherer)
      ctx->atkinsonDitherer->nextRow();
  }

  ctx->bmpOut->write(ctx->bmpRow.get(), ctx->bytesPerRow);
  yieldDuringDecode(ctx);
}

// c29: no row-delay hairline filter. Prior filters (c26–c28) either missed seams
// or blended real texture into new horizontal bands. Perfect recipe is MCU
// max-height assembly + empty-bin white fill + balanced Atkinson only.
static void pushGrayRow(BmpConvertCtx* ctx, const uint8_t* grayRow, int outY) {
  ditherAndWriteGrayRow(ctx, grayRow, outY);
}

static void finishSeamBuffer(BmpConvertCtx* /*ctx*/) {
  // No delayed rows when hairline filter is disabled.
}

// Write a fully-assembled source row (length outWidth or raw src for 1:1) to BMP.
static void writeOutputRow(BmpConvertCtx* ctx, const uint8_t* srcRow, int outY) {
  uint8_t* gray = ctx->grayRow.get();
  if (!gray) {
    // Fallback without scratch: dither path used to adjust in-place — shouldn't happen
    // when grayRow is allocated at setup.
    ditherAndWriteGrayRow(ctx, srcRow, outY);
    return;
  }
  buildAdjustedGrayRow(ctx, srcRow, gray);
  pushGrayRow(ctx, gray, outY);
}

// Matches the progressive-JPEG smoothing used by JpegToFramebufferConverter, but stays
// local because cover generation streams dithered BMP rows instead of framebuffer pixels.
static uint32_t interpolationStep(const int srcSize, const int outSize) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  return (static_cast<uint32_t>(srcSize - 1) << 16) / static_cast<uint32_t>(outSize - 1);
}

static uint32_t interpolatedSourceFp(const int outIndex, const int outSize, const int srcSize, const uint32_t step) {
  if (srcSize <= 1 || outSize <= 1) return 0;
  if (outIndex >= outSize - 1) return static_cast<uint32_t>(srcSize - 1) << 16;
  return static_cast<uint32_t>(outIndex) * step;
}

static void scaleRowLinear(BmpConvertCtx* ctx, const uint8_t* srcRow, uint8_t* dstRow) {
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    const uint32_t srcX_fp = interpolatedSourceFp(outX, ctx->outWidth, ctx->srcWidth, ctx->smoothScaleX_fp);
    const int x0 = srcX_fp >> 16;
    const int x1 = (x0 + 1 < ctx->srcWidth) ? (x0 + 1) : x0;
    const uint32_t fx = srcX_fp & (FP_ONE - 1);
    dstRow[outX] = static_cast<uint8_t>((srcRow[x0] * (FP_ONE - fx) + srcRow[x1] * fx) >> 16);
  }
}

static void writeBlendedRow(BmpConvertCtx* ctx, const uint8_t* row0, const uint8_t* row1, const uint32_t fy,
                            const int outY) {
  const uint32_t invFy = FP_ONE - fy;
  for (int outX = 0; outX < ctx->outWidth; outX++) {
    ctx->smoothOutRow[outX] = static_cast<uint8_t>((row0[outX] * invFy + row1[outX] * fy) >> 16);
  }
  writeOutputRow(ctx, ctx->smoothOutRow, outY);
}

static void processSmoothSourceRow(BmpConvertCtx* ctx, const uint8_t* srcRow, const int srcY) {
  scaleRowLinear(ctx, srcRow, ctx->smoothCurrRow);

  if (ctx->smoothPrevY < 0) {
    uint8_t* tmp = ctx->smoothPrevRow;
    ctx->smoothPrevRow = ctx->smoothCurrRow;
    ctx->smoothCurrRow = tmp;
    ctx->smoothPrevY = srcY;
    if (ctx->srcHeight <= 1) {
      while (ctx->smoothNextOutY < ctx->outHeight) {
        writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
        ctx->smoothNextOutY++;
      }
      return;
    }
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    const uint32_t srcY_fp =
        interpolatedSourceFp(ctx->smoothNextOutY, ctx->outHeight, ctx->srcHeight, ctx->smoothScaleY_fp);
    const int y0 = srcY_fp >> 16;
    const int y1 = (y0 + 1 < ctx->srcHeight) ? (y0 + 1) : y0;
    if (y1 > srcY) break;

    const uint8_t* row0 = (y0 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    const uint8_t* row1 = (y1 == srcY) ? ctx->smoothCurrRow : ctx->smoothPrevRow;
    writeBlendedRow(ctx, row0, row1, srcY_fp & (FP_ONE - 1), ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }

  uint8_t* tmp = ctx->smoothPrevRow;
  ctx->smoothPrevRow = ctx->smoothCurrRow;
  ctx->smoothCurrRow = tmp;
  ctx->smoothPrevY = srcY;
}

static void finishSmoothUpscale(BmpConvertCtx* ctx) {
  if (ctx->smoothPrevY < 0) {
    LOG_ERR("JPG", "No progressive rows decoded for smoothing");
    ctx->error = true;
    return;
  }

  while (ctx->smoothNextOutY < ctx->outHeight) {
    writeOutputRow(ctx, ctx->smoothPrevRow, ctx->smoothNextOutY);
    ctx->smoothNextOutY++;
  }
}

// Flush one scaled output row from Y-axis accumulators and advance currentOutY
static void flushScaledRow(BmpConvertCtx* ctx) {
  // Empty bins used to emit black (raw=0) — hard horizontal hairlines on jackets.
  // White fill (c26–c29) still left a visible band after dither. Carry forward the
  // previous good gray sample per column so a missed Y-boundary blends with neighbors.
  const int emptyFill = ctx->coverHighQuality ? 255 : 128;
  uint8_t* gray = ctx->grayRow.get();
  if (!gray) {
    ctx->error = true;
    return;
  }

  bool anySample = false;
  for (int x = 0; x < ctx->outWidth; x++) {
    int raw;
    if (ctx->rowCount[x] > 0) {
      raw = static_cast<int>(ctx->rowAccum[x] / ctx->rowCount[x]);
      anySample = true;
    } else if (ctx->hasLastGoodGray && ctx->lastGoodGray) {
      // Reuse last good *raw-ish* via inverse is unavailable — use last adjusted gray
      // as the source sample so the miss matches neighboring texture, not a flat strip.
      raw = ctx->lastGoodGray[x];
    } else {
      raw = emptyFill;
    }
    if (ctx->oneBit) {
      // 1-bit ditherers apply their own lift — keep raw gray.
      gray[x] = static_cast<uint8_t>(raw);
    } else if (ctx->coverHighQuality) {
      // lastGoodGray stores post-lift values; only re-lift true source samples.
      if (ctx->rowCount[x] > 0) {
        gray[x] = static_cast<uint8_t>(adjustPixelCoverThumb(raw));
      } else {
        gray[x] = static_cast<uint8_t>(raw);
      }
    } else if (USE_8BIT_OUTPUT) {
      gray[x] = (ctx->rowCount[x] > 0) ? adjustPixel(raw) : static_cast<uint8_t>(raw);
    } else {
      gray[x] = (ctx->rowCount[x] > 0) ? static_cast<uint8_t>(adjustPixel(raw)) : static_cast<uint8_t>(raw);
    }
  }

  if (ctx->lastGoodGray && (anySample || ctx->hasLastGoodGray)) {
    memcpy(ctx->lastGoodGray.get(), gray, static_cast<size_t>(ctx->outWidth));
    ctx->hasLastGoodGray = true;
  }

  pushGrayRow(ctx, gray, ctx->currentOutY);
  ctx->currentOutY++;
}

// JPEGDEC draw callback — receives one MCU-width × MCU-height block at a time,
// in left-to-right, top-to-bottom order (baseline JPEG).
// Accumulates columns into mcuBuf; once the last column arrives (completing the MCU
// row), applies scaling + dithering and writes packed BMP rows to bmpOut.
//
// Important: do NOT process using only the last column's blockH. JPEGDEC can report a
// shorter iHeight on a right-edge chunk; earlier columns in the same MCU row may have
// filled more rows. Using max height across the row avoids skipping source lines
// (which produces a full-width horizontal seam after scale/dither).
int bmpDrawCallback(JPEGDRAW* pDraw) {
  auto* ctx = reinterpret_cast<BmpConvertCtx*>(pDraw->pUser);
  if (!ctx || ctx->error) return 0;
  yieldDuringDecodeBlock(ctx);

  const uint8_t* pixels = reinterpret_cast<uint8_t*>(pDraw->pPixels);
  const int stride = pDraw->iWidth;
  const int validW = pDraw->iWidthUsed;
  const int blockH = pDraw->iHeight;
  const int blockX = pDraw->x;
  const int blockY = pDraw->y;

  // Guard against unexpected callback geometry so we never index past row buffers.
  if (blockX < 0 || blockY < 0 || blockX >= ctx->srcWidth || blockY >= ctx->srcHeight) {
    LOG_ERR("JPG", "Unexpected JPEG block origin (%d,%d) for decode grid %dx%d", blockX, blockY, ctx->srcWidth,
            ctx->srcHeight);
    ctx->error = true;
    return 0;
  }
  if (stride <= 0 || blockH <= 0 || validW <= 0) return 1;

  // New MCU row: clear buffer first so missing columns never leave black leftovers.
  // Mid-gray fill on covers makes any rare gap blend instead of a hard hairline.
  if (ctx->mcuRowBaseY != blockY) {
    const uint8_t fill = ctx->coverHighQuality ? 128 : 0;
    memset(ctx->mcuBuf.get(), fill, static_cast<size_t>(MAX_MCU_HEIGHT) * static_cast<size_t>(ctx->srcWidth));
    ctx->mcuRowBaseY = blockY;
    ctx->mcuRowMaxH = 0;
  }

  int h = blockH;
  if (h > MAX_MCU_HEIGHT) h = MAX_MCU_HEIGHT;
  if (h > ctx->mcuRowMaxH) ctx->mcuRowMaxH = h;

  // Copy block pixels into MCU row buffer (row-relative to mcuRowBaseY).
  for (int r = 0; r < h; r++) {
    const int bufRow = (blockY - ctx->mcuRowBaseY) + r;
    if (bufRow < 0 || bufRow >= MAX_MCU_HEIGHT) continue;
    const int copyW = (blockX + validW <= ctx->srcWidth) ? validW : (ctx->srcWidth - blockX);
    if (copyW <= 0) continue;
    memcpy(ctx->mcuBuf.get() + bufRow * ctx->srcWidth + blockX, pixels + r * stride, copyW);
  }

  // Wait for the last MCU column before processing any rows
  if (blockX + validW < ctx->srcWidth) return 1;

  // Process every source row covered by the tallest block in this MCU row.
  const int processH = ctx->mcuRowMaxH;
  const int endRow = ctx->mcuRowBaseY + processH;

  for (int y = ctx->mcuRowBaseY; y < endRow && y < ctx->srcHeight; y++) {
    // Skip already-emitted rows (defensive; should not happen with continuous decode).
    if (y <= ctx->lastSrcYProcessed) continue;

    const uint8_t* srcRow = ctx->mcuBuf.get() + (y - ctx->mcuRowBaseY) * ctx->srcWidth;

    if (ctx->smoothUpscale) {
      processSmoothSourceRow(ctx, srcRow, y);
    } else if (!ctx->needsScaling) {
      // 1:1 — outWidth == srcWidth, write directly
      writeOutputRow(ctx, srcRow, y);
      ctx->currentOutY++;
    } else {
      // Fixed-point area averaging on X axis
      for (int outX = 0; outX < ctx->outWidth; outX++) {
        const int srcXStart = (static_cast<uint32_t>(outX) * ctx->scaleX_fp) >> 16;
        int srcXEnd = (static_cast<uint32_t>(outX + 1) * ctx->scaleX_fp) >> 16;
        // Ensure every output column samples at least one source pixel (avoids
        // black hairlines from empty bins at fixed-point boundaries).
        if (srcXEnd <= srcXStart) srcXEnd = srcXStart + 1;
        int sum = 0;
        int count = 0;
        for (int srcX = srcXStart; srcX < srcXEnd && srcX < ctx->srcWidth; srcX++) {
          sum += srcRow[srcX];
          count++;
        }
        if (count == 0 && srcXStart < ctx->srcWidth) {
          sum = srcRow[srcXStart];
          count = 1;
        } else if (count == 0 && ctx->srcWidth > 0) {
          sum = srcRow[ctx->srcWidth - 1];
          count = 1;
        }
        ctx->rowAccum[outX] += sum;
        ctx->rowCount[outX] += count;
      }

      // Flush output row(s) whose Y boundary we've crossed
      const uint32_t srcY_fp = static_cast<uint32_t>(y + 1) << 16;
      while (srcY_fp >= ctx->nextOutY_srcStart && ctx->currentOutY < ctx->outHeight) {
        flushScaledRow(ctx);
        ctx->nextOutY_srcStart = static_cast<uint32_t>(ctx->currentOutY + 1) * ctx->scaleY_fp;
        if (srcY_fp >= ctx->nextOutY_srcStart) continue;
        memset(ctx->rowAccum.get(), 0, ctx->outWidth * sizeof(uint32_t));
        memset(ctx->rowCount.get(), 0, ctx->outWidth * sizeof(uint32_t));
      }
    }

    ctx->lastSrcYProcessed = y;
  }

  // MCU row complete — reset assembly state for the next row.
  ctx->mcuRowBaseY = -1;
  ctx->mcuRowMaxH = 0;

  return ctx->error ? 0 : 1;
}

}  // namespace

// Internal implementation with configurable target size and bit depth
bool JpegToBmpConverter::jpegFileToBmpStreamInternal(HalFile& jpegFile, Print& bmpOut, int targetWidth,
                                                     int targetHeight, bool oneBit, bool crop, bool coverHighQuality) {
  // Home cover path (c22): 2-bit balanced Atkinson — far less visible dither than 1-bit.
  // Display uses grayscale multipass on home (same idea as sleep covers).
  if (coverHighQuality) {
    oneBit = false;
  }
  LOG_DBG("JPG", "Converting JPEG to %s BMP (target: %dx%d%s)", oneBit ? "1-bit" : "2-bit", targetWidth, targetHeight,
          coverHighQuality ? ", cover 2-bit Atkinson" : "");

  if (ESP.getFreeHeap() < MIN_FREE_HEAP) {
    LOG_ERR("JPG", "Not enough heap for JPEG decoder (%u free, need %u)", ESP.getFreeHeap(), MIN_FREE_HEAP);
    return false;
  }

  s_jpegFile = &jpegFile;

  const auto jpeg = makeUniqueNoThrow<JPEGDEC>();
  if (!jpeg) {
    LOG_ERR("JPG", "OOM: JPEG decoder");
    return false;
  }

  int rc = jpeg->open("", bmpJpegOpen, bmpJpegClose, bmpJpegRead, bmpJpegSeek, bmpDrawCallback);
  if (rc != 1) {
    LOG_ERR("JPG", "JPEG open failed (err=%d)", jpeg->getLastError());
    return false;
  }

  const ScopedCleanup cleanup{[&jpeg]() { jpeg->close(); }};

  const int srcWidth = jpeg->getWidth();
  const int srcHeight = jpeg->getHeight();
  const bool progressiveDecode = (jpeg->getJPEGType() == JPEG_MODE_PROGRESSIVE);
  // JPEGDEC forces progressive streams to JPEG_SCALE_EIGHTH in DecodeJPEG,
  // so callback coordinates and MCU buffering must use the reduced decode grid.
  // c30 / v0.1.3: smooth bilinear upscale of that grid (not blocky full-size DC).
  const int decodedSrcWidth = progressiveDecode ? ((srcWidth + 7) >> 3) : srcWidth;
  const int decodedSrcHeight = progressiveDecode ? ((srcHeight + 7) >> 3) : srcHeight;

  LOG_DBG("JPG", "JPEG dimensions: %dx%d", srcWidth, srcHeight);
  if (progressiveDecode) {
    LOG_DBG("JPG", "Progressive JPEG decode uses 1/8 source: %dx%d", decodedSrcWidth, decodedSrcHeight);
  }

  constexpr int MAX_IMAGE_WIDTH = 2048;
  constexpr int MAX_IMAGE_HEIGHT = 3072;

  if (srcWidth <= 0 || srcHeight <= 0 || srcWidth > MAX_IMAGE_WIDTH || srcHeight > MAX_IMAGE_HEIGHT) {
    LOG_DBG("JPG", "Image too large or invalid (%dx%d), max supported: %dx%d", srcWidth, srcHeight, MAX_IMAGE_WIDTH,
            MAX_IMAGE_HEIGHT);
    return false;
  }

  // Calculate output dimensions (pre-scale to fit display exactly)
  int outWidth = srcWidth;
  int outHeight = srcHeight;
  if (targetWidth <= 0 || targetHeight <= 0) {
    // Without an explicit target, keep decoder-native dimensions.
    outWidth = decodedSrcWidth;
    outHeight = decodedSrcHeight;
  }

  const int scaleSrcWidth = decodedSrcWidth;
  const int scaleSrcHeight = decodedSrcHeight;

  uint32_t scaleX_fp = 65536;  // 1.0 in 16.16 fixed point
  uint32_t scaleY_fp = 65536;
  bool needsScaling = false;

  if (targetWidth > 0 && targetHeight > 0 && (srcWidth != targetWidth || srcHeight != targetHeight)) {
    const float scaleToFitWidth = static_cast<float>(targetWidth) / srcWidth;
    const float scaleToFitHeight = static_cast<float>(targetHeight) / srcHeight;
    float scale = 1.0f;
    if (crop) {
      scale = (scaleToFitWidth > scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    } else {
      scale = (scaleToFitWidth < scaleToFitHeight) ? scaleToFitWidth : scaleToFitHeight;
    }

    outWidth = static_cast<int>(srcWidth * scale);
    outHeight = static_cast<int>(srcHeight * scale);
    if (outWidth < 1) outWidth = 1;
    if (outHeight < 1) outHeight = 1;

    LOG_DBG("JPG", "Scaling source %dx%d (decode grid %dx%d) -> %dx%d (target %dx%d)", srcWidth, srcHeight,
            scaleSrcWidth, scaleSrcHeight, outWidth, outHeight, targetWidth, targetHeight);
  }

  if (scaleSrcWidth != outWidth || scaleSrcHeight != outHeight) {
    scaleX_fp = (static_cast<uint32_t>(scaleSrcWidth) << 16) / outWidth;
    scaleY_fp = (static_cast<uint32_t>(scaleSrcHeight) << 16) / outHeight;
    needsScaling = true;
  }

  const bool smoothUpscale =
      progressiveDecode && needsScaling && scaleSrcWidth <= outWidth && scaleSrcHeight <= outHeight;

  // Write BMP header with output dimensions
  int bytesPerRow;
  if (USE_8BIT_OUTPUT && !oneBit) {
    writeBmpHeader8bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 3) / 4 * 4;
  } else if (oneBit) {
    writeBmpHeader1bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth + 31) / 32 * 4;
  } else {
    writeBmpHeader2bit(bmpOut, outWidth, outHeight);
    bytesPerRow = (outWidth * 2 + 31) / 32 * 4;
  }

  BmpConvertCtx ctx = {};
  ctx.bmpOut = &bmpOut;
  ctx.srcWidth = scaleSrcWidth;
  ctx.srcHeight = scaleSrcHeight;
  ctx.outWidth = outWidth;
  ctx.outHeight = outHeight;
  ctx.oneBit = oneBit;
  ctx.bytesPerRow = bytesPerRow;
  ctx.needsScaling = needsScaling;
  ctx.scaleX_fp = scaleX_fp;
  ctx.scaleY_fp = scaleY_fp;
  ctx.smoothUpscale = smoothUpscale;
  ctx.smoothScaleX_fp = interpolationStep(ctx.srcWidth, outWidth);
  ctx.smoothScaleY_fp = interpolationStep(ctx.srcHeight, outHeight);
  ctx.smoothNextOutY = 0;
  ctx.smoothPrevY = -1;
  ctx.mcuRowBaseY = -1;
  ctx.mcuRowMaxH = 0;
  ctx.lastSrcYProcessed = -1;
  ctx.seamHeld = 0;
  ctx.seamAY = 0;
  ctx.seamBY = 0;
  ctx.rowsSinceYield = 0;
  ctx.blocksSinceYield = 0;
  ctx.error = false;

  // MCU row buffer: MAX_MCU_HEIGHT rows × decoded srcWidth columns of grayscale
  ctx.mcuBuf = makeUniqueNoThrow<uint8_t[]>(MAX_MCU_HEIGHT * ctx.srcWidth);
  if (!ctx.mcuBuf) {
    LOG_ERR("JPG", "OOM: MCU buffer (%d bytes)", MAX_MCU_HEIGHT * ctx.srcWidth);
    return false;
  }
  memset(ctx.mcuBuf.get(), coverHighQuality ? 128 : 0, MAX_MCU_HEIGHT * ctx.srcWidth);

  ctx.bmpRow = makeUniqueNoThrow<uint8_t[]>(bytesPerRow);
  if (!ctx.bmpRow) {
    LOG_ERR("JPG", "OOM: BMP row buffer");
    return false;
  }

  ctx.grayRow = makeUniqueNoThrow<uint8_t[]>(outWidth);
  if (!ctx.grayRow) {
    LOG_ERR("JPG", "OOM: gray row buffer");
    return false;
  }

  // c30: carry-forward buffer for empty Y-scale bins (avoids solid hairline strips).
  // Soft-fail: without it empty bins fall back to white/mid fill (still valid BMP).
  if (needsScaling && !smoothUpscale) {
    ctx.lastGoodGray = makeUniqueNoThrow<uint8_t[]>(outWidth);
    if (ctx.lastGoodGray) {
      memset(ctx.lastGoodGray.get(), coverHighQuality ? 255 : 128, static_cast<size_t>(outWidth));
      ctx.hasLastGoodGray = false;
    } else {
      LOG_DBG("JPG", "No lastGoodGray buffer (heap); empty bins use solid fill");
      ctx.hasLastGoodGray = false;
    }
  }

  // c29+: no hairline delay buffers (saves RAM; filters caused banding).

  if (smoothUpscale) {
    // One contiguous allocation avoids three heap blocks while keeping smoothing line-buffered.
    const size_t smoothRowsBytes = static_cast<size_t>(outWidth) * 3;
    ctx.smoothRows = makeUniqueNoThrow<uint8_t[]>(smoothRowsBytes);
    if (!ctx.smoothRows) {
      LOG_ERR("JPG", "OOM: progressive smoothing buffers");
      return false;
    }
    ctx.smoothPrevRow = ctx.smoothRows.get();
    ctx.smoothCurrRow = ctx.smoothPrevRow + outWidth;
    ctx.smoothOutRow = ctx.smoothCurrRow + outWidth;
    LOG_DBG("JPG", "Progressive smoothing: %dx%d -> %dx%d, buffers=%u bytes", ctx.srcWidth, ctx.srcHeight, outWidth,
            outHeight, static_cast<unsigned>(smoothRowsBytes));
  } else if (needsScaling) {
    ctx.rowAccum = makeUniqueNoThrow<uint32_t[]>(outWidth);
    ctx.rowCount = makeUniqueNoThrow<uint32_t[]>(outWidth);
    if (!ctx.rowAccum || !ctx.rowCount) {
      LOG_ERR("JPG", "OOM: scaling buffers");
      return false;
    }
    ctx.nextOutY_srcStart = scaleY_fp;
  }

  ctx.coverHighQuality = coverHighQuality;

  if (oneBit) {
    ctx.atkinson1BitDitherer = makeUniqueNoThrow<Atkinson1BitDitherer>(outWidth);
    if (!ctx.atkinson1BitDitherer || !ctx.atkinson1BitDitherer->isValid()) {
      LOG_ERR("JPG", "OOM: Atkinson1BitDitherer");
      ctx.atkinson1BitDitherer.reset();
      return false;
    }
  } else if (!USE_8BIT_OUTPUT) {
    if (coverHighQuality) {
      // Equal 0/85/170/255 — smooth midtones with fine dither (not 1-bit crosshatch).
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth, /*balancedLevels=*/true);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer (cover)");
        ctx.atkinsonDitherer.reset();
        return false;
      }
    } else if (USE_FLOYD_STEINBERG) {
      ctx.fsDitherer = makeUniqueNoThrow<FloydSteinbergDitherer>(outWidth);
      if (!ctx.fsDitherer || !ctx.fsDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: FloydSteinbergDitherer");
        ctx.fsDitherer.reset();
        return false;
      }
    } else if (USE_ATKINSON) {
      ctx.atkinsonDitherer = makeUniqueNoThrow<AtkinsonDitherer>(outWidth, /*balancedLevels=*/false);
      if (!ctx.atkinsonDitherer || !ctx.atkinsonDitherer->isValid()) {
        LOG_ERR("JPG", "OOM: AtkinsonDitherer");
        ctx.atkinsonDitherer.reset();
        return false;
      }
    }
  }

  jpeg->setPixelType(EIGHT_BIT_GRAYSCALE);
  jpeg->setUserPointer(&ctx);

  rc = jpeg->decode(0, 0, 0);

  if (rc == 1 && ctx.smoothUpscale && !ctx.error) {
    finishSmoothUpscale(&ctx);
  }

  // Finish any remaining scaled rows (last source rows can leave the Y accum
  // short of outHeight). Pad so the BMP height is complete — a short write used
  // to leave a trash strip that looked like a line.
  if (rc == 1 && !ctx.error && ctx.needsScaling && !ctx.smoothUpscale) {
    while (ctx.currentOutY < ctx.outHeight) {
      flushScaledRow(&ctx);
      memset(ctx.rowAccum.get(), 0, ctx.outWidth * sizeof(uint32_t));
      memset(ctx.rowCount.get(), 0, ctx.outWidth * sizeof(uint32_t));
    }
  }
  // 1:1 path: if MCU assembly skipped source rows, pad remaining with last good /
  // mid-gray so height matches header (avoids bottom trash line).
  if (rc == 1 && !ctx.error && !ctx.needsScaling && !ctx.smoothUpscale) {
    while (ctx.currentOutY < ctx.outHeight) {
      uint8_t* gray = ctx.grayRow.get();
      if (!gray) break;
      if (ctx.hasLastGoodGray && ctx.lastGoodGray) {
        memcpy(gray, ctx.lastGoodGray.get(), static_cast<size_t>(ctx.outWidth));
      } else {
        memset(gray, ctx.coverHighQuality ? 255 : 128, static_cast<size_t>(ctx.outWidth));
      }
      pushGrayRow(&ctx, gray, ctx.currentOutY);
      ctx.currentOutY++;
    }
  }

  // Drain cover hairline delay buffer (last 1–2 rows).
  if (rc == 1 && !ctx.error) {
    finishSeamBuffer(&ctx);
  }

  if (rc != 1 || ctx.error) {
    LOG_ERR("JPG", "JPEG decode failed (rc=%d, err=%d)", rc, jpeg->getLastError());
    return false;
  }

  LOG_DBG("JPG", "Successfully converted JPEG to BMP");
  return true;
}

// Core function: Convert JPEG file to 2-bit BMP (uses default target size)
bool JpegToBmpConverter::jpegFileToBmpStream(HalFile& jpegFile, Print& bmpOut, bool crop) {
  // Use runtime display dimensions (swapped for portrait cover sizing)
  const int targetWidth = display.getDisplayHeight();
  const int targetHeight = display.getDisplayWidth();
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetWidth, targetHeight, false, crop);
}

// Convert with custom target size (for thumbnails, 2-bit)
bool JpegToBmpConverter::jpegFileToBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                     int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, false);
}

// Convert to 1-bit BMP (black and white only, no grays) for fast home screen rendering
bool JpegToBmpConverter::jpegFileTo1BitBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut, int targetMaxWidth,
                                                         int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, true, true, false);
}

// Home covers (c30 / v0.1.3): 2-bit balanced Atkinson + mild lift at gen.
// Shared by Bare / Stats / Stats-Life. MCU max-height + empty-bin carry-forward.
// Pair with home grayscale multipass for clean midtones (minimal dither grain).
bool JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(HalFile& jpegFile, Print& bmpOut,
                                                                          int targetMaxWidth, int targetMaxHeight) {
  return jpegFileToBmpStreamInternal(jpegFile, bmpOut, targetMaxWidth, targetMaxHeight, /*oneBit=*/false,
                                     /*crop=*/false, /*coverHighQuality=*/true);
}
