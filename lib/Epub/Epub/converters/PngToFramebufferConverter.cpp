#include "PngToFramebufferConverter.h"

#include <FontCacheManager.h>
#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <HalStorage.h>
#include <InflateStream.h>
#include <Logging.h>
#include <Memory.h>
#include <PNGdec.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>

#include "DirectPixelWriter.h"
#include "DitherUtils.h"
#include "PixelCache.h"

namespace {

// Context struct passed through PNGdec callbacks to avoid global mutable state.
// The draw callback receives this via pDraw->pUser (set by png.decode()).
// The file I/O callbacks receive the HalFile* via pFile->fHandle (set by pngOpen()).
struct PngContext {
  GfxRenderer* renderer{nullptr};
  const RenderConfig* config{nullptr};
  int screenWidth{0};
  int screenHeight{0};

  // Scaling state
  float scale{1.f};
  int srcWidth{0};
  int srcHeight{0};
  int dstWidth{0};
  int dstHeight{0};
  int lastEmittedDstY{-1};  // last fully emitted output row

  // FreeInk-style box filter: accumulate source rows into the current output
  // band (downscale). Sums/counts sized to dstWidth; emitted when the source
  // band for that dstY is complete.
  uint32_t* bandSums{nullptr};
  uint16_t* bandCounts{nullptr};
  int bandDstY{-1};  // which output row is currently accumulating (-1 = none)

  PixelCache cache;
  bool caching{false};

  uint8_t* grayLineBuffer{nullptr};
};

// File I/O callbacks use pFile->fHandle to access the HalFile*,
// avoiding the need for global file state.
void* pngOpenWithHandle(const char* filename, int32_t* size) {
  HalFile* f = new HalFile();
  if (!Storage.openFileForRead("PNG", std::string(filename), *f)) {
    delete f;
    return nullptr;
  }
  *size = f->size();
  return f;
}

void pngCloseWithHandle(void* handle) {
  HalFile* f = reinterpret_cast<HalFile*>(handle);
  if (f) {
    f->close();
    delete f;
  }
}

int32_t pngReadWithHandle(PNGFILE* pFile, uint8_t* pBuf, int32_t len) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return 0;
  return f->read(pBuf, len);
}

int32_t pngSeekWithHandle(PNGFILE* pFile, int32_t pos) {
  HalFile* f = reinterpret_cast<HalFile*>(pFile->fHandle);
  if (!f) return -1;
  // PNGdec ignores the return value but file I/O must actually seek. HalFile::seek
  // returns bool — map to 0 success / -1 failure (matches libc fseek convention).
  return f->seek(static_cast<size_t>(pos)) ? 0 : -1;
}

// PNGdec's PNGIMAGE embeds ~32 KB zlib + row buffers (~50 KB with 8200 row buf).
// BSS-static fixed Alice but permanently stole ~60 KB — God Emperor then stuck
// on Loading (free~27KB maxAlloc~12KB). Heap alloc + free after each decode
// restores text-book headroom. When alloc still fails, decodePngViaTinfl uses
// InflateStream (max contiguous ~32 KB LZ window) for small letter/float PNGs.
constexpr size_t MIN_FREE_HEAP_FOR_FILE_BUF = 24 * 1024;

PNG* g_sharedPng = nullptr;
// >0 while section parse / batch precache holds the decoder across images.
int g_pngSectionWarmRefs = 0;

PNG* sharedPngDecoder() {
  if (g_sharedPng) return g_sharedPng;
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  g_sharedPng = new (std::nothrow) PNG();
  if (!g_sharedPng) {
    LOG_ERR("PNG", "Failed to allocate PNG decoder (heap=%u maxAlloc=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));
  } else {
    LOG_DBG("PNG", "PNG decoder allocated (heap=%u maxAlloc=%u)", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
  }
  return g_sharedPng;
}

void releaseSharedPngDecoder() {
  if (g_pngSectionWarmRefs > 0) return;  // held for multi-image section warm
  delete g_sharedPng;
  g_sharedPng = nullptr;
}

// PNGdec keeps TWO scanlines in its internal ucPixels buffer (current + previous)
// and each scanline includes a leading filter byte.
// Required storage is therefore approximately: 2 * (pitch + 1) + alignment slack.
// If PNG_MAX_BUFFERED_PIXELS is smaller than this requirement for a given image,
// PNGdec can overrun its internal buffer before our draw callback executes.
int bytesPerPixelFromType(int pixelType) {
  switch (pixelType) {
    case PNG_PIXEL_TRUECOLOR:
      return 3;
    case PNG_PIXEL_GRAY_ALPHA:
      return 2;
    case PNG_PIXEL_TRUECOLOR_ALPHA:
      return 4;
    case PNG_PIXEL_GRAYSCALE:
    case PNG_PIXEL_INDEXED:
    default:
      return 1;
  }
}

int packedRowBytes(int srcWidth, int bitsPerSample) { return (srcWidth * bitsPerSample + 7) / 8; }

int requiredPngInternalBufferBytes(int srcWidth, int pixelType, int bitsPerSample) {
  // +1 filter byte per scanline, *2 for current+previous lines, +32 for alignment margin.
  int pitch = srcWidth * bytesPerPixelFromType(pixelType);
  if ((pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED) && bitsPerSample < 8) {
    pitch = packedRowBytes(srcWidth, bitsPerSample);
  }
  return ((pitch + 1) * 2) + 32;
}

bool isSupportedBitDepth(int pixelType, int bitsPerSample) {
  if (bitsPerSample == 8) return true;
  if (bitsPerSample != 1 && bitsPerSample != 2 && bitsPerSample != 4) return false;
  return pixelType == PNG_PIXEL_GRAYSCALE || pixelType == PNG_PIXEL_INDEXED;
}

uint8_t readPackedSample(const uint8_t* pixels, int x, int bitsPerSample) {
  if (bitsPerSample == 8) return pixels[x];

  const int bitOffset = x * bitsPerSample;
  const int shift = 8 - bitsPerSample - (bitOffset & 7);
  const uint8_t mask = (1U << bitsPerSample) - 1;
  return (pixels[bitOffset >> 3] >> shift) & mask;
}

uint8_t expandSampleToByte(uint8_t sample, int bitsPerSample) {
  if (bitsPerSample == 8) return sample;
  const uint8_t maxSample = (1U << bitsPerSample) - 1;
  return static_cast<uint8_t>((sample * 255U) / maxSample);
}

// Convert entire source line to grayscale with alpha blending to white background.
// Low-bit-depth grayscale/indexed scanlines are packed most-significant sample first.
// For indexed PNGs with tRNS chunk, alpha values are stored at palette[768] onwards.
// Processing the whole line at once improves cache locality and reduces per-pixel overhead.
void convertLineToGray(const uint8_t* pPixels, uint8_t* grayLine, int width, int pixelType, int bitsPerSample,
                       uint8_t* palette, int hasAlpha) {
  switch (pixelType) {
    case PNG_PIXEL_GRAYSCALE:
      if (bitsPerSample == 8) {
        memcpy(grayLine, pPixels, width);
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_TRUECOLOR:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 3];
        grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      }
      break;

    case PNG_PIXEL_INDEXED:
      if (palette) {
        if (hasAlpha) {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
            uint8_t alpha = palette[768 + idx];
            grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
          }
        } else {
          for (int x = 0; x < width; x++) {
            uint8_t idx = readPackedSample(pPixels, x, bitsPerSample);
            uint8_t* p = &palette[idx * 3];
            grayLine[x] = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
          }
        }
      } else {
        for (int x = 0; x < width; x++) {
          grayLine[x] = expandSampleToByte(readPackedSample(pPixels, x, bitsPerSample), bitsPerSample);
        }
      }
      break;

    case PNG_PIXEL_GRAY_ALPHA:
      for (int x = 0; x < width; x++) {
        uint8_t gray = pPixels[x * 2];
        uint8_t alpha = pPixels[x * 2 + 1];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    case PNG_PIXEL_TRUECOLOR_ALPHA:
      for (int x = 0; x < width; x++) {
        const uint8_t* p = &pPixels[x * 4];
        uint8_t gray = (uint8_t)((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        uint8_t alpha = p[3];
        grayLine[x] = (uint8_t)((gray * alpha + 255 * (255 - alpha)) / 255);
      }
      break;

    default:
      memset(grayLine, 128, width);
      break;
  }
}

// Emit one fully-accumulated destination row (box-filtered gray → dither → FB/cache).
void emitPngDstRow(PngContext* ctx, int dstY, const uint8_t* grayRow) {
  if (dstY < 0 || dstY >= ctx->dstHeight) return;
  if (dstY <= ctx->lastEmittedDstY) return;
  ctx->lastEmittedDstY = dstY;

  const int outY = ctx->config->y + dstY;
  // Always advance/write the pixel cache so .pxc stays complete even when a
  // row is clipped off the panel (float + margin edge cases). FB writes are
  // optional (layout precache uses cache-only mode).
  const bool wantFb = ctx->config->writeToFramebuffer;
  const bool onScreen = wantFb && (outY >= 0 && outY < ctx->screenHeight);

  DirectPixelWriter pw;
  if (onScreen) {
    pw.init(*ctx->renderer);
    pw.beginRow(outY);
  }

  bool caching = ctx->caching;
  DirectCacheWriter cw;
  if (caching) {
    if (!ctx->cache.advanceTo(dstY)) {
      caching = false;
      ctx->caching = false;
    } else {
      cw.init(ctx->cache.buffer, ctx->cache.bytesPerRow, ctx->cache.bandRows, ctx->cache.originX);
      // Cache band row index is relative to image, not screen — use dstY as the
      // logical cache row; DirectCacheWriter maps via origin.
      cw.beginRow(ctx->config->y + dstY, ctx->config->y + ctx->cache.bandStart);
    }
  }

  const int outXBase = ctx->config->x;
  const int screenWidth = ctx->screenWidth;
  const bool useDithering = ctx->config->useDithering;
  const EinkImageTone tone = ctx->config->resolvedTone();
  const int dstWidth = ctx->dstWidth;

  for (int dstX = 0; dstX < dstWidth; dstX++) {
    const int outX = outXBase + dstX;
    const uint8_t gray = grayRow[dstX];
    const uint8_t dithered =
        useDithering ? applyBayerDither4Level(gray, outX, outY, tone) : quantizeGray4LevelNoDither(gray, tone);
    if (onScreen && outX >= 0 && outX < screenWidth) {
      pw.writePixel(outX, dithered);
    }
    if (caching) cw.writePixel(outX, dithered);
  }
}

// Horizontal box-filter: average every source sample that maps into each dstX.
void accumulatePngSourceRow(PngContext* ctx, const uint8_t* srcGray) {
  const int srcW = ctx->srcWidth;
  const int dstW = ctx->dstWidth;
  if (!ctx->bandSums || !ctx->bandCounts || dstW <= 0 || srcW <= 0) return;

  if (dstW >= srcW) {
    // 1:1 or upscale: each source sample contributes to its mapped dst column
    // (vertical upscale handled by emitting the same band across multiple rows).
    for (int sx = 0; sx < srcW; ++sx) {
      const int dx = (sx * dstW) / srcW;
      if (dx >= 0 && dx < dstW) {
        ctx->bandSums[dx] += srcGray[sx];
        ctx->bandCounts[dx] += 1;
      }
    }
    // Fill any empty dst columns by nearest src (rare on exact ratios).
    for (int dx = 0; dx < dstW; ++dx) {
      if (ctx->bandCounts[dx] == 0) {
        int sx = (dx * srcW) / dstW;
        if (sx >= srcW) sx = srcW - 1;
        if (sx < 0) sx = 0;
        ctx->bandSums[dx] = srcGray[sx];
        ctx->bandCounts[dx] = 1;
      }
    }
  } else {
    // Downscale: each dest column averages its source footprint.
    for (int dx = 0; dx < dstW; ++dx) {
      int sx0 = (dx * srcW) / dstW;
      int sx1 = ((dx + 1) * srcW) / dstW;
      if (sx1 <= sx0) sx1 = sx0 + 1;
      if (sx0 < 0) sx0 = 0;
      if (sx1 > srcW) sx1 = srcW;
      for (int sx = sx0; sx < sx1; ++sx) {
        ctx->bandSums[dx] += srcGray[sx];
        ctx->bandCounts[dx] += 1;
      }
    }
  }
}

void finalizePngBandIfReady(PngContext* ctx, int nextSrcY) {
  // A band for bandDstY covers source rows [y0, y1). Emit when nextSrcY >= y1.
  if (ctx->bandDstY < 0 || !ctx->bandSums || !ctx->bandCounts) return;
  const int dstY = ctx->bandDstY;
  const int y1 = ((dstY + 1) * ctx->srcHeight) / ctx->dstHeight;
  if (nextSrcY < y1 && nextSrcY < ctx->srcHeight) return;

  // Materialize averaged gray row into grayLineBuffer prefix (reuse as dst buffer).
  auto* out = ctx->grayLineBuffer;
  for (int dx = 0; dx < ctx->dstWidth; ++dx) {
    out[dx] = ctx->bandCounts[dx] ? static_cast<uint8_t>(ctx->bandSums[dx] / ctx->bandCounts[dx]) : 0xFF;
    ctx->bandSums[dx] = 0;
    ctx->bandCounts[dx] = 0;
  }
  emitPngDstRow(ctx, dstY, out);
  ctx->bandDstY = -1;
}

int pngDrawCallback(PNGDRAW* pDraw) {
  PngContext* ctx = reinterpret_cast<PngContext*>(pDraw->pUser);
  if (!ctx || !ctx->config || !ctx->renderer || !ctx->grayLineBuffer) return 0;

  const int srcY = pDraw->y;
  const int srcWidth = ctx->srcWidth;

  // Convert entire source line to grayscale (improves cache locality)
  convertLineToGray(pDraw->pPixels, ctx->grayLineBuffer, srcWidth, pDraw->iPixelType, pDraw->iBpp, pDraw->pPalette,
                    pDraw->iHasAlpha);

  // 1:1 fast path — Alice drop-cap letter PNGs (71×75, 4-bit palette) and other
  // native-size figures. Avoids the box-filter band state machine entirely so a
  // regression there cannot blank small images.
  if (ctx->srcWidth == ctx->dstWidth && ctx->srcHeight == ctx->dstHeight) {
    if (srcY >= 0 && srcY < ctx->dstHeight) {
      emitPngDstRow(ctx, srcY, ctx->grayLineBuffer);
    }
    return 1;
  }

  // Which output row(s) does this source row feed?
  // Downscale: one band dstY = srcY * dstH / srcH (multiple src rows → one dst).
  // Upscale: one src row may fill several consecutive dst rows.
  if (ctx->dstHeight > ctx->srcHeight) {
    // Upscale path: flush any open downscale band first (shouldn't exist).
    finalizePngBandIfReady(ctx, ctx->srcHeight);

    int firstDstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
    int endDstY = ((srcY + 1) * ctx->dstHeight) / ctx->srcHeight;
    if (firstDstY <= ctx->lastEmittedDstY) firstDstY = ctx->lastEmittedDstY + 1;
    if (endDstY > ctx->dstHeight) endDstY = ctx->dstHeight;
    if (firstDstY >= endDstY) return 1;

    // Build a horizontally scaled row once, then stamp it across the range.
    memset(ctx->bandSums, 0, static_cast<size_t>(ctx->dstWidth) * sizeof(uint32_t));
    memset(ctx->bandCounts, 0, static_cast<size_t>(ctx->dstWidth) * sizeof(uint16_t));
    ctx->bandDstY = firstDstY;
    accumulatePngSourceRow(ctx, ctx->grayLineBuffer);
    auto* out = ctx->grayLineBuffer;
    for (int dx = 0; dx < ctx->dstWidth; ++dx) {
      out[dx] = ctx->bandCounts[dx] ? static_cast<uint8_t>(ctx->bandSums[dx] / ctx->bandCounts[dx]) : 0xFF;
    }
    for (int dstY = firstDstY; dstY < endDstY; ++dstY) {
      emitPngDstRow(ctx, dstY, out);
    }
    ctx->bandDstY = -1;
    return 1;
  }

  // Downscale or 1:1: box-filter accumulate into band for this srcY's dst.
  const int dstY = (srcY * ctx->dstHeight) / ctx->srcHeight;
  if (dstY < 0 || dstY >= ctx->dstHeight) return 1;

  // Flush previous band if we've moved on.
  if (ctx->bandDstY >= 0 && ctx->bandDstY != dstY) {
    finalizePngBandIfReady(ctx, srcY);
  }
  if (ctx->bandDstY != dstY) {
    // Start new band
    memset(ctx->bandSums, 0, static_cast<size_t>(ctx->dstWidth) * sizeof(uint32_t));
    memset(ctx->bandCounts, 0, static_cast<size_t>(ctx->dstWidth) * sizeof(uint16_t));
    ctx->bandDstY = dstY;
  }
  accumulatePngSourceRow(ctx, ctx->grayLineBuffer);

  // Emit when this is the last source row that maps into dstY, or last image row.
  const int y1 = ((dstY + 1) * ctx->srcHeight) / ctx->dstHeight;
  if (srcY + 1 >= y1 || srcY + 1 >= ctx->srcHeight) {
    finalizePngBandIfReady(ctx, srcY + 1);
  }

  return 1;
}

// ---- tinfl fallback (no PNGdec object) ------------------------------------
// PNGdec needs one contiguous ~50KB block. Serial on Alice: free~47KB maxAlloc
// ~36–49KB → alloc always fails. InflateStream needs ~11KB state + optional 32KB
// window as separate allocs; window alone fits when maxAlloc is ~36KB+.
// Used for non-interlaced drop-cap / float letter PNGs (and any moderate PNG
// when PNGdec is unavailable).

uint32_t readBe32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
  const int p = int(a) + int(b) - int(c);
  const int pa = std::abs(p - int(a));
  const int pb = std::abs(p - int(b));
  const int pc = std::abs(p - int(c));
  if (pa <= pb && pa <= pc) return a;
  if (pb <= pc) return b;
  return c;
}

void unfilterPngRow(uint8_t filter, uint8_t* row, const uint8_t* prev, size_t len, int bpp) {
  const size_t ib = static_cast<size_t>(bpp);
  switch (filter) {
    case 0:
      break;
    case 1:  // Sub
      for (size_t i = ib; i < len; ++i) row[i] = static_cast<uint8_t>(row[i] + row[i - ib]);
      break;
    case 2:  // Up
      if (prev) {
        for (size_t i = 0; i < len; ++i) row[i] = static_cast<uint8_t>(row[i] + prev[i]);
      }
      break;
    case 3:  // Average
      for (size_t i = 0; i < len; ++i) {
        const uint8_t a = (i >= ib) ? row[i - ib] : 0;
        const uint8_t b = prev ? prev[i] : 0;
        row[i] = static_cast<uint8_t>(row[i] + static_cast<uint8_t>((uint16_t(a) + uint16_t(b)) / 2));
      }
      break;
    case 4:  // Paeth
      for (size_t i = 0; i < len; ++i) {
        const uint8_t a = (i >= ib) ? row[i - ib] : 0;
        const uint8_t b = prev ? prev[i] : 0;
        const uint8_t c = (prev && i >= ib) ? prev[i - ib] : 0;
        row[i] = static_cast<uint8_t>(row[i] + paethPredictor(a, b, c));
      }
      break;
    default:
      break;
  }
}

int pngBytesPerPixel(uint8_t colorType, uint8_t bitDepth) {
  int ch = 1;
  switch (colorType) {
    case 0:
      ch = 1;
      break;
    case 2:
      ch = 3;
      break;
    case 3:
      ch = 1;
      break;
    case 4:
      ch = 2;
      break;
    case 6:
      ch = 4;
      break;
    default:
      return 0;
  }
  if (bitDepth < 8) return 1;  // packed; filter bpp is 1
  return (ch * bitDepth) / 8;
}

bool grayFromPngPixel(const uint8_t* row, int x, uint8_t colorType, uint8_t bitDepth, const uint8_t* palRgb,
                      const uint8_t* palA, uint8_t& outGray) {
  switch (colorType) {
    case 0: {  // greyscale
      uint8_t g;
      if (bitDepth == 8) {
        g = row[x];
      } else if (bitDepth == 16) {
        g = row[x * 2];
      } else if (bitDepth == 1 || bitDepth == 2 || bitDepth == 4) {
        const int bitOffset = x * bitDepth;
        const int shift = 8 - bitDepth - (bitOffset & 7);
        const uint8_t mask = static_cast<uint8_t>((1u << bitDepth) - 1u);
        const uint8_t sample = (row[bitOffset >> 3] >> shift) & mask;
        g = expandSampleToByte(sample, bitDepth);
      } else {
        return false;
      }
      outGray = g;
      return true;
    }
    case 2: {  // RGB
      if (bitDepth == 8) {
        const uint8_t* p = &row[x * 3];
        outGray = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        return true;
      }
      if (bitDepth == 16) {
        const uint8_t* p = &row[x * 6];
        outGray = static_cast<uint8_t>((p[0] * 77 + p[2] * 150 + p[4] * 29) >> 8);
        return true;
      }
      return false;
    }
    case 3: {  // palette
      uint8_t idx;
      if (bitDepth == 8) {
        idx = row[x];
      } else if (bitDepth == 1 || bitDepth == 2 || bitDepth == 4) {
        const int bitOffset = x * bitDepth;
        const int shift = 8 - bitDepth - (bitOffset & 7);
        const uint8_t mask = static_cast<uint8_t>((1u << bitDepth) - 1u);
        idx = (row[bitOffset >> 3] >> shift) & mask;
      } else {
        return false;
      }
      if (!palRgb) {
        outGray = idx;
        return true;
      }
      const uint8_t* p = &palRgb[idx * 3];
      uint8_t g = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
      if (palA) {
        const uint8_t a = palA[idx];
        g = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
      }
      outGray = g;
      return true;
    }
    case 4: {  // grey+alpha
      if (bitDepth == 8) {
        const uint8_t g = row[x * 2];
        const uint8_t a = row[x * 2 + 1];
        outGray = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
        return true;
      }
      if (bitDepth == 16) {
        const uint8_t g = row[x * 4];
        const uint8_t a = row[x * 4 + 2];
        outGray = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
        return true;
      }
      return false;
    }
    case 6: {  // RGBA
      if (bitDepth == 8) {
        const uint8_t* p = &row[x * 4];
        const uint8_t g = static_cast<uint8_t>((p[0] * 77 + p[1] * 150 + p[2] * 29) >> 8);
        outGray = static_cast<uint8_t>((g * p[3] + 255 * (255 - p[3])) / 255);
        return true;
      }
      if (bitDepth == 16) {
        const uint8_t* p = &row[x * 8];
        const uint8_t g = static_cast<uint8_t>((p[0] * 77 + p[2] * 150 + p[4] * 29) >> 8);
        const uint8_t a = p[6];
        outGray = static_cast<uint8_t>((g * a + 255 * (255 - a)) / 255);
        return true;
      }
      return false;
    }
    default:
      return false;
  }
}

// Concatenated IDAT fill for InflateStream.
struct IdatFill {
  const uint8_t* data{nullptr};
  size_t len{0};
  size_t pos{0};
};

size_t idatFillFn(void* ctx, const uint8_t** out) {
  auto* id = static_cast<IdatFill*>(ctx);
  if (id->pos >= id->len) {
    *out = nullptr;
    return 0;
  }
  *out = id->data + id->pos;
  const size_t n = id->len - id->pos;
  id->pos = id->len;
  return n;
}

bool decodePngViaTinfl(const std::string& imagePath, GfxRenderer& renderer, const RenderConfig& config) {
  HalFile inFile;
  if (!Storage.openFileForRead("PNG", imagePath, inFile)) {
    LOG_ERR("PNG", "tinfl: cannot open %s", imagePath.c_str());
    return false;
  }
  const size_t fileSize = inFile.size();
  // Drop-cap letters are a few KB; refuse huge files on this path.
  if (fileSize < 33 || fileSize > 96 * 1024) {
    inFile.close();
    LOG_DBG("PNG", "tinfl: skip size %u", static_cast<unsigned>(fileSize));
    return false;
  }
  auto fileBuf = makeUniqueNoThrow<uint8_t[]>(fileSize);
  if (!fileBuf) {
    inFile.close();
    LOG_ERR("PNG", "tinfl: OOM file buf %u free=%u maxAlloc=%u", static_cast<unsigned>(fileSize),
            static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  if (inFile.read(fileBuf.get(), static_cast<int>(fileSize)) != static_cast<int>(fileSize)) {
    inFile.close();
    LOG_ERR("PNG", "tinfl: short read %s", imagePath.c_str());
    return false;
  }
  inFile.close();

  const uint8_t* p = fileBuf.get();
  const uint8_t* end = p + fileSize;
  static const uint8_t kSig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (fileSize < 8 || memcmp(p, kSig, 8) != 0) {
    LOG_ERR("PNG", "tinfl: bad signature");
    return false;
  }
  p += 8;

  uint32_t srcW = 0, srcH = 0;
  uint8_t bitDepth = 0, colorType = 0, interlace = 0;
  // Palette tables on heap — keep SAX-nested stack under ~300B (was ~1.2KB).
  auto palRgb = makeUniqueNoThrow<uint8_t[]>(256 * 3);
  auto palA = makeUniqueNoThrow<uint8_t[]>(256);
  if (!palRgb || !palA) {
    LOG_ERR("PNG", "tinfl: OOM palette free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  memset(palRgb.get(), 0, 256 * 3);
  memset(palA.get(), 255, 256);
  bool hasPal = false;
  bool hasTrns = false;

  // Collect concatenated IDAT into a growable buffer (letter PNGs are small).
  std::unique_ptr<uint8_t[]> idat;
  size_t idatLen = 0;
  size_t idatCap = 0;

  while (p + 8 <= end) {
    const uint32_t clen = readBe32(p);
    const char* ctype = reinterpret_cast<const char*>(p + 4);
    p += 8;
    if (p + clen + 4 > end) {
      LOG_ERR("PNG", "tinfl: truncated chunk");
      return false;
    }
    const uint8_t* cdata = p;
    p += clen + 4;  // data + CRC

    if (memcmp(ctype, "IHDR", 4) == 0) {
      if (clen < 13) return false;
      srcW = readBe32(cdata);
      srcH = readBe32(cdata + 4);
      bitDepth = cdata[8];
      colorType = cdata[9];
      interlace = cdata[12];
      if (srcW == 0 || srcH == 0 || srcW > 2048 || srcH > 4096) {
        LOG_ERR("PNG", "tinfl: bad dims %ux%u", static_cast<unsigned>(srcW), static_cast<unsigned>(srcH));
        return false;
      }
      if (interlace != 0) {
        LOG_DBG("PNG", "tinfl: interlaced not supported");
        return false;
      }
    } else if (memcmp(ctype, "PLTE", 4) == 0) {
      if (clen > 256 * 3 || (clen % 3) != 0) return false;
      memcpy(palRgb.get(), cdata, clen);
      hasPal = true;
    } else if (memcmp(ctype, "tRNS", 4) == 0) {
      if (colorType == 3) {
        const size_t n = std::min(static_cast<size_t>(clen), size_t{256});
        memcpy(palA.get(), cdata, n);
        hasTrns = true;
      }
    } else if (memcmp(ctype, "IDAT", 4) == 0) {
      if (clen == 0) continue;
      if (idatLen + clen > idatCap) {
        size_t want = idatCap ? idatCap * 2 : 4096;
        while (want < idatLen + clen) want *= 2;
        auto next = makeUniqueNoThrow<uint8_t[]>(want);
        if (!next) {
          LOG_ERR("PNG", "tinfl: OOM IDAT %u", static_cast<unsigned>(want));
          return false;
        }
        if (idat && idatLen) memcpy(next.get(), idat.get(), idatLen);
        idat = std::move(next);
        idatCap = want;
      }
      memcpy(idat.get() + idatLen, cdata, clen);
      idatLen += clen;
    } else if (memcmp(ctype, "IEND", 4) == 0) {
      break;
    }
  }

  if (srcW == 0 || !idat || idatLen == 0) {
    LOG_ERR("PNG", "tinfl: missing IHDR/IDAT");
    return false;
  }
  (void)hasPal;

  const int filterBpp = pngBytesPerPixel(colorType, bitDepth);
  if (filterBpp <= 0) {
    LOG_ERR("PNG", "tinfl: unsupported type=%u depth=%u", colorType, bitDepth);
    return false;
  }
  size_t rawRowBytes = 0;
  if (bitDepth < 8) {
    rawRowBytes = (static_cast<size_t>(srcW) * bitDepth + 7) / 8;
  } else {
    rawRowBytes = static_cast<size_t>(srcW) * static_cast<size_t>(filterBpp);
  }
  const size_t rowStride = rawRowBytes + 1;  // filter byte
  if (rowStride > 16 * 1024) {
    LOG_ERR("PNG", "tinfl: row too wide %u", static_cast<unsigned>(rowStride));
    return false;
  }

  // Heap-allocate decode state: this runs under SAX startElement (~1KB stack
  // already) after TextSettings reflow. A stack PngContext + InflateStream
  // pushed the frame past ~1.4KB and corrupted adjacent heap → free() assert
  // on the next vector growth in placeFloatImage.
  auto ctxHolder = makeUniqueNoThrow<PngContext>();
  if (!ctxHolder) {
    LOG_ERR("PNG", "tinfl: OOM PngContext free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  PngContext& ctx = *ctxHolder;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();
  ctx.srcWidth = static_cast<int>(srcW);
  ctx.srcHeight = static_cast<int>(srcH);
  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = static_cast<float>(ctx.dstWidth) / static_cast<float>(ctx.srcWidth);
  } else {
    float scaleX = static_cast<float>(config.maxWidth) / static_cast<float>(ctx.srcWidth);
    float scaleY = static_cast<float>(config.maxHeight) / static_cast<float>(ctx.srcHeight);
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.f) ctx.scale = 1.f;
    ctx.dstWidth = static_cast<int>(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = static_cast<int>(ctx.srcHeight * ctx.scale);
  }
  if (ctx.dstWidth <= 0 || ctx.dstHeight <= 0) return false;

  // Prefer 1:1 path for drop-caps (exact display size matches source).
  const bool oneToOne = (ctx.srcWidth == ctx.dstWidth && ctx.srcHeight == ctx.dstHeight);

  auto grayLine = makeUniqueNoThrow<uint8_t[]>(static_cast<size_t>(std::max(ctx.srcWidth, ctx.dstWidth)));
  auto curRow = makeUniqueNoThrow<uint8_t[]>(rowStride);
  auto prevRow = makeUniqueNoThrow<uint8_t[]>(rowStride);
  if (!grayLine || !curRow || !prevRow) {
    LOG_ERR("PNG", "tinfl: OOM rows free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  memset(prevRow.get(), 0, rowStride);

  // One-shot inflate into a full raw buffer when small enough; else stream.
  const size_t rawTotal = rowStride * static_cast<size_t>(srcH);
  std::unique_ptr<uint8_t[]> rawAll;
  bool usedOneShot = false;
  if (rawTotal <= 48 * 1024 && rawTotal + 4096 < ESP.getMaxAllocHeap()) {
    rawAll = makeUniqueNoThrow<uint8_t[]>(rawTotal);
    if (rawAll) {
      // Heap-allocate the stream too — tinfl_decompressor is ~11KB when claimed
      // from heap (not stack), but the InflateStream object itself is small;
      // still avoid nesting large RAII on the SAX stack.
      auto infl = makeUniqueNoThrow<InflateStream>();
      if (infl && infl->init(/*streaming=*/false)) {
        infl->setZlibWrapped();
        infl->setSource(idat.get(), idatLen);
        if (infl->read(rawAll.get(), rawTotal)) {
          usedOneShot = true;
        }
        infl->deinit();
      }
      if (!usedOneShot) rawAll.reset();
    }
  }

  std::unique_ptr<InflateStream> streamInfl;
  IdatFill fill{idat.get(), idatLen, 0};
  if (!usedOneShot) {
    // Free IDAT is still needed for fill; drop fileBuf to free headroom for window.
    fileBuf.reset();
    streamInfl = makeUniqueNoThrow<InflateStream>();
    if (!streamInfl || !streamInfl->init(/*streaming=*/true)) {
      LOG_ERR("PNG", "tinfl: InflateStream init failed free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
              static_cast<unsigned>(ESP.getMaxAllocHeap()));
      return false;
    }
    streamInfl->setZlibWrapped();
    streamInfl->setFill(idatFillFn, &fill);
  } else {
    fileBuf.reset();
    idat.reset();  // rawAll holds decompressed data
  }

  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "tinfl: cache begin failed");
      ctx.caching = false;
    }
  }

  LOG_DBG("PNG", "tinfl decode %ux%u -> %dx%d type=%u depth=%u oneshot=%d free=%u maxAlloc=%u",
          static_cast<unsigned>(srcW), static_cast<unsigned>(srcH), ctx.dstWidth, ctx.dstHeight, colorType, bitDepth,
          usedOneShot ? 1 : 0, static_cast<unsigned>(ESP.getFreeHeap()), static_cast<unsigned>(ESP.getMaxAllocHeap()));

  const uint8_t* palRgbPtr = hasPal ? palRgb.get() : nullptr;
  const uint8_t* palAPtr = hasTrns ? palA.get() : nullptr;
  for (uint32_t y = 0; y < srcH; ++y) {
    const uint8_t* rawRow;
    if (usedOneShot) {
      rawRow = rawAll.get() + static_cast<size_t>(y) * rowStride;
      memcpy(curRow.get(), rawRow, rowStride);
    } else {
      if (!streamInfl->read(curRow.get(), rowStride)) {
        LOG_ERR("PNG", "tinfl: inflate row %u failed", static_cast<unsigned>(y));
        if (ctx.caching) ctx.cache.abort();
        streamInfl->deinit();
        return false;
      }
    }
    const uint8_t filter = curRow[0];
    uint8_t* pixels = curRow.get() + 1;
    unfilterPngRow(filter, pixels, y ? prevRow.get() + 1 : nullptr, rawRowBytes, filterBpp);
    memcpy(prevRow.get(), curRow.get(), rowStride);

    // Convert source row → gray, optionally nearest-neighbour scale to dst.
    if (oneToOne) {
      for (int x = 0; x < ctx.srcWidth; ++x) {
        uint8_t g = 128;
        if (!grayFromPngPixel(pixels, x, colorType, bitDepth, palRgbPtr, palAPtr, g)) g = 128;
        grayLine[x] = g;
      }
      emitPngDstRow(&ctx, static_cast<int>(y), grayLine.get());
    } else {
      for (int dx = 0; dx < ctx.dstWidth; ++dx) {
        const int sx = (dx * ctx.srcWidth) / ctx.dstWidth;
        uint8_t g = 128;
        if (!grayFromPngPixel(pixels, sx, colorType, bitDepth, palRgbPtr, palAPtr, g)) g = 128;
        grayLine[dx] = g;
      }
      // Map source row y → dest row(s) by nearest.
      const int y0 = static_cast<int>((static_cast<int64_t>(y) * ctx.dstHeight) / ctx.srcHeight);
      const int y1 = static_cast<int>((static_cast<int64_t>(y + 1) * ctx.dstHeight) / ctx.srcHeight);
      for (int dy = y0; dy < y1 && dy < ctx.dstHeight; ++dy) {
        emitPngDstRow(&ctx, dy, grayLine.get());
      }
    }
  }

  if (streamInfl) streamInfl->deinit();

  const int rowsEmitted = ctx.lastEmittedDstY + 1;
  if (rowsEmitted < ctx.dstHeight) {
    LOG_ERR("PNG", "tinfl incomplete: %d/%d rows", rowsEmitted, ctx.dstHeight);
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  if (ctx.caching) ctx.cache.finalize();
  LOG_DBG("PNG", "tinfl OK %d rows for %s", rowsEmitted, imagePath.c_str());
  return true;
}

}  // namespace

bool PngToFramebufferConverter::warmSharedDecoder() {
  // Prefer a live decoder. If section warm is not active, still allocate once so
  // the next decode skips cold new() under a fragmented heap; decode's release
  // frees it unless beginSectionWarm() is holding.
  return sharedPngDecoder() != nullptr;
}

void PngToFramebufferConverter::beginSectionWarm() {
  // PNGdec is ~50 KB. Holding it across a full section on ESP32-C3 (no PSRAM)
  // with free~85 KB leaves maxAlloc ~8 KB → thousands of PTX word skips, ZIP
  // extract failures, empty image boxes, and "3-page" Alice chapters.
  const uint32_t freeHeap = ESP.getFreeHeap();
  const uint32_t maxAlloc = ESP.getMaxAllocHeap();
  if (freeHeap < 96 * 1024 || maxAlloc < 56 * 1024) {
    LOG_DBG("PNG", "section warm skipped (heap=%u maxAlloc=%u)", static_cast<unsigned>(freeHeap),
            static_cast<unsigned>(maxAlloc));
    return;
  }
  ++g_pngSectionWarmRefs;
  if (!sharedPngDecoder()) {
    // Alloc failed — do not keep a warm ref that blocks releaseShared.
    --g_pngSectionWarmRefs;
    return;
  }
  LOG_DBG("PNG", "section warm begin refs=%d heap=%u", g_pngSectionWarmRefs, static_cast<unsigned>(ESP.getFreeHeap()));
}

void PngToFramebufferConverter::endSectionWarm() {
  if (g_pngSectionWarmRefs > 0) --g_pngSectionWarmRefs;
  if (g_pngSectionWarmRefs == 0) {
    delete g_sharedPng;
    g_sharedPng = nullptr;
    LOG_DBG("PNG", "section warm end (decoder released) heap=%u", static_cast<unsigned>(ESP.getFreeHeap()));
  }
}

void PngToFramebufferConverter::releaseWarmIfHeapTight(const size_t minMaxAllocBytes) {
  if (g_pngSectionWarmRefs <= 0 && !g_sharedPng) return;
  if (ESP.getMaxAllocHeap() >= minMaxAllocBytes) return;
  g_pngSectionWarmRefs = 0;
  delete g_sharedPng;
  g_sharedPng = nullptr;
  LOG_DBG("PNG", "section warm force-released (maxAlloc=%u need=%u)", static_cast<unsigned>(ESP.getMaxAllocHeap()),
          static_cast<unsigned>(minMaxAllocBytes));
}

bool PngToFramebufferConverter::getDimensionsStatic(const std::string& imagePath, ImageDimensions& out) {
  PNG* png = sharedPngDecoder();
  if (!png) return false;
  // open() returns 0 on success AND on missing file — check dimensions.
  (void)png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                  nullptr);
  out.width = png->getWidth();
  out.height = png->getHeight();
  png->close();
  releaseSharedPngDecoder();
  if (out.width <= 0 || out.height <= 0) {
    LOG_ERR("PNG", "Failed to open PNG for dimensions: %s", imagePath.c_str());
    return false;
  }
  return true;
}

bool PngToFramebufferConverter::decodeToFramebuffer(const std::string& imagePath, GfxRenderer& renderer,
                                                    const RenderConfig& config) {
  LOG_DBG("PNG", "Decoding PNG: %s heap=%u maxAlloc=%u", imagePath.c_str(), static_cast<unsigned>(ESP.getFreeHeap()),
          static_cast<unsigned>(ESP.getMaxAllocHeap()));

  // Only wipe font cache when PNGdec's ~50KB block will not fit otherwise.
  // Unconditional clear forced SD advance-table reloads on every image page
  // (multi-second SDCF spam) and was a major regression vs early rivulet builds.
  auto maybeClearFontsForPng = [&]() {
    if (ESP.getMaxAllocHeap() >= 52 * 1024) return;
    if (FontCacheManager* fcm = renderer.getFontCacheManager()) {
      if (!fcm->isScanning()) fcm->clearCache();
    }
  };

  // Prefer tinfl when the contiguous block is clearly too small for PNGdec —
  // avoids clearing fonts and the failed-alloc log spam. Remember a failed
  // tinfl so we do not re-enter it after a second PNGdec alloc miss (double
  // OOM work under maxAlloc~16KB left the heap worse and delayed recovery).
  bool tinflAlreadyTried = false;
  if (ESP.getMaxAllocHeap() < 48 * 1024) {
    LOG_DBG("PNG", "maxAlloc low; tinfl first for %s", imagePath.c_str());
    tinflAlreadyTried = true;
    if (decodePngViaTinfl(imagePath, renderer, config)) return true;
  }

  maybeClearFontsForPng();

  // Always free the decoder when this function returns (success or fail).
  struct PngRelease {
    ~PngRelease() { releaseSharedPngDecoder(); }
  } pngRelease;

  PNG* png = sharedPngDecoder();
  if (!png) {
    if (tinflAlreadyTried) {
      LOG_ERR("PNG", "PNGdec unavailable and tinfl already failed for %s", imagePath.c_str());
      return false;
    }
    // Contiguous block for PNGdec missing — try tinfl (smaller peak alloc).
    LOG_DBG("PNG", "PNGdec unavailable; trying tinfl fallback for %s", imagePath.c_str());
    return decodePngViaTinfl(imagePath, renderer, config);
  }

  HalFile inFile;
  if (!Storage.openFileForRead("PNG", imagePath, inFile)) {
    LOG_ERR("PNG", "Cannot open file: %s", imagePath.c_str());
    return false;
  }
  const size_t fileSize = inFile.size();
  if (fileSize == 0 || fileSize > 512 * 1024) {
    inFile.close();
    LOG_ERR("PNG", "Bad PNG size %u for %s", static_cast<unsigned>(fileSize), imagePath.c_str());
    return false;
  }

  // Prefer whole-file RAM decode (no SD seeks during inflate).
  std::unique_ptr<uint8_t[]> fileBuf;
  const bool canHoldFile =
      (fileSize + MIN_FREE_HEAP_FOR_FILE_BUF < ESP.getFreeHeap()) && (fileSize + 4096 < ESP.getMaxAllocHeap());
  if (canHoldFile) {
    fileBuf = makeUniqueNoThrow<uint8_t[]>(fileSize);
    if (fileBuf) {
      const int n = inFile.read(fileBuf.get(), static_cast<int>(fileSize));
      inFile.close();
      if (n != static_cast<int>(fileSize)) {
        LOG_ERR("PNG", "Short read %d/%u for %s", n, static_cast<unsigned>(fileSize), imagePath.c_str());
        return false;
      }
    } else {
      inFile.close();
    }
  } else {
    inFile.close();
    LOG_DBG("PNG", "File too large for RAM decode (%u); using SD callbacks", static_cast<unsigned>(fileSize));
  }

  // Heap PngContext: same deep-stack hazard as tinfl path when called from layout.
  auto ctxHolder = makeUniqueNoThrow<PngContext>();
  if (!ctxHolder) {
    LOG_ERR("PNG", "OOM PngContext free=%u maxAlloc=%u", static_cast<unsigned>(ESP.getFreeHeap()),
            static_cast<unsigned>(ESP.getMaxAllocHeap()));
    return false;
  }
  PngContext& ctx = *ctxHolder;
  ctx.renderer = &renderer;
  ctx.config = &config;
  ctx.screenWidth = renderer.getScreenWidth();
  ctx.screenHeight = renderer.getScreenHeight();

  int rc;
  if (fileBuf) {
    rc = png->openRAM(fileBuf.get(), static_cast<int>(fileSize), pngDrawCallback);
  } else {
    rc = png->open(imagePath.c_str(), pngOpenWithHandle, pngCloseWithHandle, pngReadWithHandle, pngSeekWithHandle,
                   pngDrawCallback);
  }
  struct PngCloser {
    PNG* p;
    ~PngCloser() {
      if (p) p->close();
    }
  } closer{png};

  if (png->getWidth() <= 0 || png->getHeight() <= 0) {
    LOG_ERR("PNG", "Open produced no dimensions for %s (rc=%d err=%d ram=%d)", imagePath.c_str(), rc,
            png->getLastError(), fileBuf ? 1 : 0);
    return false;
  }

  if (!validateImageDimensions(png->getWidth(), png->getHeight(), "PNG")) {
    return false;
  }

  // Calculate output dimensions
  ctx.srcWidth = png->getWidth();
  ctx.srcHeight = png->getHeight();

  if (config.useExactDimensions && config.maxWidth > 0 && config.maxHeight > 0) {
    // Use exact dimensions as specified (avoids rounding mismatches with pre-calculated sizes)
    ctx.dstWidth = config.maxWidth;
    ctx.dstHeight = config.maxHeight;
    ctx.scale = (float)ctx.dstWidth / ctx.srcWidth;
  } else {
    // Calculate scale factor to fit within maxWidth/maxHeight
    float scaleX = (float)config.maxWidth / ctx.srcWidth;
    float scaleY = (float)config.maxHeight / ctx.srcHeight;
    ctx.scale = (scaleX < scaleY) ? scaleX : scaleY;
    if (ctx.scale > 1.0f) ctx.scale = 1.0f;  // Don't upscale

    ctx.dstWidth = (int)(ctx.srcWidth * ctx.scale);
    ctx.dstHeight = (int)(ctx.srcHeight * ctx.scale);
  }
  ctx.lastEmittedDstY = -1;
  ctx.bandDstY = -1;

  const int pixelType = png->getPixelType();
  const int bitsPerSample = png->getBpp();
  LOG_DBG("PNG", "PNG %dx%d -> %dx%d (scale %.2f), type: %d, bpp: %d", ctx.srcWidth, ctx.srcHeight, ctx.dstWidth,
          ctx.dstHeight, ctx.scale, pixelType, bitsPerSample);

  const int requiredInternal = requiredPngInternalBufferBytes(ctx.srcWidth, pixelType, bitsPerSample);
  if (requiredInternal > PNG_MAX_BUFFERED_PIXELS) {
    LOG_ERR(
        "PNG",
        "PNG row buffer too small: need %d bytes for width=%d type=%d bpp=%d, configured PNG_MAX_BUFFERED_PIXELS=%d",
        requiredInternal, ctx.srcWidth, pixelType, bitsPerSample, PNG_MAX_BUFFERED_PIXELS);
    LOG_ERR("PNG", "Aborting decode to avoid PNGdec internal buffer overflow");
    return false;
  }

  if (!isSupportedBitDepth(pixelType, bitsPerSample)) {
    warnUnsupportedFeature(
        "bit depth (" + std::to_string(bitsPerSample) + "bpp) for pixel type " + std::to_string(pixelType), imagePath);
    return false;
  }

  // Scratch holds a full source gray row, then is reused as the averaged
  // destination row (box filter) — size must cover max(src, dst) width.
  constexpr size_t MAX_GRAY_LINE_BUFFER_BYTES = PNG_MAX_BUFFERED_PIXELS / 2;
  const size_t grayBufSize = static_cast<size_t>(std::max(ctx.srcWidth, ctx.dstWidth));
  if (grayBufSize > MAX_GRAY_LINE_BUFFER_BYTES) {
    LOG_ERR("PNG", "Expanded gray row too wide: need %u bytes for src=%d dst=%d, max=%u",
            static_cast<unsigned>(grayBufSize), ctx.srcWidth, ctx.dstWidth,
            static_cast<unsigned>(MAX_GRAY_LINE_BUFFER_BYTES));
    return false;
  }

  auto grayLineBuffer = makeUniqueNoThrow<uint8_t[]>(grayBufSize);
  if (!grayLineBuffer) {
    LOG_ERR("PNG", "Failed to allocate gray line buffer");
    return false;
  }
  ctx.grayLineBuffer = grayLineBuffer.get();

  // Box-filter accumulators only needed when scaling (1:1 uses the fast path).
  const bool needsBand = !(ctx.srcWidth == ctx.dstWidth && ctx.srcHeight == ctx.dstHeight);
  std::unique_ptr<uint32_t[]> bandSums;
  std::unique_ptr<uint16_t[]> bandCounts;
  if (needsBand) {
    const size_t bandBytes = static_cast<size_t>(ctx.dstWidth);
    bandSums = makeUniqueNoThrow<uint32_t[]>(bandBytes);
    bandCounts = makeUniqueNoThrow<uint16_t[]>(bandBytes);
    if (!bandSums || !bandCounts) {
      LOG_ERR("PNG", "Failed to allocate scale band buffers");
      return false;
    }
    memset(bandSums.get(), 0, bandBytes * sizeof(uint32_t));
    memset(bandCounts.get(), 0, bandBytes * sizeof(uint16_t));
    ctx.bandSums = bandSums.get();
    ctx.bandCounts = bandCounts.get();
  }

  // Stream the pixel cache to disk. PNGdec delivers source scanlines top to
  // bottom and we emit at most one (downscaled) output row per callback, so the
  // band only needs a single row. Streaming keeps the working set tiny, so
  // unlike the old full-image buffer it neither competes with the ~44KB decoder
  // nor forces larger images to skip caching - which previously meant a full
  // re-decode on every one of an image page's ~14 render passes.
  ctx.caching = !config.cachePath.empty();
  if (ctx.caching) {
    if (!ctx.cache.begin(config.cachePath, ctx.dstWidth, ctx.dstHeight, config.x, config.y, 1)) {
      LOG_ERR("PNG", "Failed to start cache stream, continuing without caching");
      ctx.caching = false;
    }
  }

  unsigned long decodeStart = millis();
  rc = png->decode(&ctx, 0);
  // Flush any trailing accumulation band (last rows of tall images).
  if (rc == PNG_SUCCESS && ctx.bandDstY >= 0) {
    finalizePngBandIfReady(&ctx, ctx.srcHeight);
  }
  unsigned long decodeTime = millis() - decodeStart;

  const int rowsEmitted = ctx.lastEmittedDstY + 1;
  ctx.grayLineBuffer = nullptr;
  ctx.bandSums = nullptr;
  ctx.bandCounts = nullptr;

  if (rc != PNG_SUCCESS) {
    LOG_ERR("PNG", "Decode failed: %d (err=%d)", rc, png->getLastError());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }
  if (rowsEmitted < ctx.dstHeight) {
    LOG_ERR("PNG", "Decode incomplete: %d/%d rows for %s", rowsEmitted, ctx.dstHeight, imagePath.c_str());
    if (ctx.caching) ctx.cache.abort();
    return false;
  }

  LOG_DBG("PNG", "PNG decoding complete - %d rows in %lu ms", rowsEmitted, decodeTime);

  // Finalize the streamed cache (caching may have been cleared on a flush error).
  if (ctx.caching) {
    ctx.cache.finalize();
  }

  return true;
}

bool PngToFramebufferConverter::supportsFormat(const std::string& extension) {
  return FsHelpers::hasPngExtension(extension);
}
