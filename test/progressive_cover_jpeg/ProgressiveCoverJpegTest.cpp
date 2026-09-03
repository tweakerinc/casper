#include <jpgd.h>
#include <jpgd_spill.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <sys/types.h>
#include <vector>

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

int spillPread(void* ctx, uint64_t offset, void* buf, int n) {
  auto* f = static_cast<FILE*>(ctx);
  if (!f || n <= 0) return 0;
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return 0;
  return static_cast<int>(fread(buf, 1, static_cast<size_t>(n), f));
}

int spillPwrite(void* ctx, uint64_t offset, const void* buf, int n) {
  auto* f = static_cast<FILE*>(ctx);
  if (!f || n <= 0) return 0;
  if (fseeko(f, static_cast<off_t>(offset), SEEK_SET) != 0) return 0;
  const int w = static_cast<int>(fwrite(buf, 1, static_cast<size_t>(n), f));
  if (fflush(f) != 0) return 0;
  return w;
}

bool decodeLuma(const std::vector<uint8_t>& jpeg, const bool spill, std::vector<uint8_t>& luma, int& w, int& h) {
  luma.clear();
  jpgd::jpeg_decoder_mem_stream stream(jpeg.data(), static_cast<unsigned>(jpeg.size()));
  FILE* spillFile = nullptr;
  if (spill) {
    spillFile = std::tmpfile();
    if (!spillFile) return false;
    jpgd::jpeg_decoder_spill_io io{};
    io.ctx = spillFile;
    io.pread = spillPread;
    io.pwrite = spillPwrite;
    if (!jpgd::jpgd_spill_begin(&io)) {
      std::fclose(spillFile);
      return false;
    }
  }

  jpgd::jpeg_decoder decoder(&stream, jpgd::jpeg_decoder::cFlagDisableSIMD | jpgd::jpeg_decoder::cFlagCoverDecode);
  const bool okOpen = decoder.get_error_code() == jpgd::JPGD_SUCCESS;
  bool ok = false;
  if (okOpen && decoder.is_progressive() && decoder.begin_decoding() == jpgd::JPGD_SUCCESS) {
    w = decoder.get_width();
    h = decoder.get_height();
    luma.assign(static_cast<size_t>(w) * static_cast<size_t>(h), 128);
    uint8_t block[64];
    const int bxMax = decoder.luma_blocks_x();
    const int byMax = decoder.luma_blocks_y();
    ok = true;
    for (int by = 0; by < byMax && ok; ++by) {
      for (int bx = 0; bx < bxMax && ok; ++bx) {
        if (!decoder.copy_luma_block(bx, by, block)) {
          ok = false;
          break;
        }
        for (int r = 0; r < 8; ++r) {
          const int y = by * 8 + r;
          if (y >= h) break;
          const int dstX = bx * 8;
          const int copyW = (dstX + 8 <= w) ? 8 : (w - dstX);
          if (copyW > 0) {
            std::memcpy(luma.data() + static_cast<size_t>(y) * static_cast<size_t>(w) + dstX, block + r * 8,
                        static_cast<size_t>(copyW));
          }
        }
      }
    }
  }

  if (spill) {
    jpgd::jpgd_spill_end();
    std::fclose(spillFile);
  }
  return ok;
}

bool decodeScanlineY(const std::vector<uint8_t>& jpeg, std::vector<uint8_t>& luma, int& w, int& h) {
  luma.clear();
  jpgd::jpeg_decoder_mem_stream stream(jpeg.data(), static_cast<unsigned>(jpeg.size()));
  jpgd::jpeg_decoder decoder(&stream, jpgd::jpeg_decoder::cFlagDisableSIMD);
  if (decoder.get_error_code() != jpgd::JPGD_SUCCESS) return false;
  if (decoder.begin_decoding() != jpgd::JPGD_SUCCESS) return false;
  w = decoder.get_width();
  h = decoder.get_height();
  luma.resize(static_cast<size_t>(w) * static_cast<size_t>(h));
  for (int y = 0; y < h; ++y) {
    const void* line = nullptr;
    jpgd::uint len = 0;
    if (decoder.decode(&line, &len) != jpgd::JPGD_SUCCESS || !line) return false;
    const auto* p = static_cast<const uint8_t*>(line);
    if (decoder.get_bytes_per_pixel() == 1) {
      std::memcpy(luma.data() + static_cast<size_t>(y) * static_cast<size_t>(w), p, static_cast<size_t>(w));
    } else {
      for (int x = 0; x < w; ++x) {
        luma[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = p[x * 4];
      }
    }
  }
  return true;
}

int meanAbsDiff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b) {
  if (a.size() != b.size() || a.empty()) return 255;
  uint64_t sum = 0;
  for (size_t i = 0; i < a.size(); ++i) {
    sum += static_cast<uint64_t>(a[i] > b[i] ? a[i] - b[i] : b[i] - a[i]);
  }
  return static_cast<int>(sum / a.size());
}

const char* kFixtureDir = "test/progressive_cover_jpeg/fixtures";

std::string fixturePath(const char* name) {
  std::string direct = std::string(kFixtureDir) + "/" + name;
  std::ifstream in(direct, std::ios::binary);
  if (in) return direct;
  return std::string("/workspace/") + kFixtureDir + "/" + name;
}

void expectCheckerContrast(const std::vector<uint8_t>& luma, int w, int h) {
  ASSERT_EQ(w, 128);
  ASSERT_EQ(h, 192);
  const int dark = luma[static_cast<size_t>(12) * w + 12];
  const int light = luma[static_cast<size_t>(12) * w + 20];
  EXPECT_LT(dark, 80);
  EXPECT_GT(light, 175);
  EXPECT_GE(light - dark, 100);
}

}  // namespace

TEST(ProgressiveCoverJpeg, DecoderFitsC3CoverHeap) {
  // YCbCr LUTs live in the object unless cover-decode skips creating scanline
  // buffers. Keep the object itself well under the Home FB-loan maxAlloc (~69 KB).
  EXPECT_LT(sizeof(jpgd::jpeg_decoder), static_cast<size_t>(32 * 1024));
}

TEST(ProgressiveCoverJpeg, SpillMatchesRam) {
  const auto jpeg = readFile(fixturePath("cover_progressive.jpg"));
  ASSERT_FALSE(jpeg.empty());

  std::vector<uint8_t> ram;
  std::vector<uint8_t> spilled;
  int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
  ASSERT_TRUE(decodeLuma(jpeg, /*spill=*/false, ram, w1, h1));
  ASSERT_TRUE(decodeLuma(jpeg, /*spill=*/true, spilled, w2, h2));
  EXPECT_EQ(w1, 128);
  EXPECT_EQ(h1, 192);
  EXPECT_EQ(w1, w2);
  EXPECT_EQ(h1, h2);
  EXPECT_EQ(ram, spilled);
}

TEST(ProgressiveCoverJpeg, FullDecodeKeepsCheckerContrast) {
  const auto jpeg = readFile(fixturePath("cover_progressive.jpg"));
  ASSERT_FALSE(jpeg.empty());
  std::vector<uint8_t> luma;
  int w = 0, h = 0;
  ASSERT_TRUE(decodeLuma(jpeg, /*spill=*/true, luma, w, h));
  expectCheckerContrast(luma, w, h);
}

TEST(ProgressiveCoverJpeg, GrayProgressiveDecodes) {
  const auto jpeg = readFile(fixturePath("cover_progressive_gray.jpg"));
  ASSERT_FALSE(jpeg.empty());
  std::vector<uint8_t> luma;
  int w = 0, h = 0;
  ASSERT_TRUE(decodeLuma(jpeg, /*spill=*/true, luma, w, h));
  expectCheckerContrast(luma, w, h);
}

TEST(ProgressiveCoverJpeg, GrayProgressiveMatchesBaseline) {
  const auto progressive = readFile(fixturePath("cover_progressive_gray.jpg"));
  const auto baseline = readFile(fixturePath("cover_baseline_gray.jpg"));
  ASSERT_FALSE(progressive.empty());
  ASSERT_FALSE(baseline.empty());

  std::vector<uint8_t> progLuma;
  std::vector<uint8_t> baseLuma;
  int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
  ASSERT_TRUE(decodeLuma(progressive, /*spill=*/true, progLuma, w1, h1));
  ASSERT_TRUE(decodeScanlineY(baseline, baseLuma, w2, h2));
  ASSERT_EQ(w1, w2);
  ASSERT_EQ(h1, h2);
  expectCheckerContrast(progLuma, w1, h1);
  expectCheckerContrast(baseLuma, w2, h2);
  // Lossy encoders differ; 1/8 DC-only would be ~80+ MAD on this checker.
  EXPECT_LT(meanAbsDiff(progLuma, baseLuma), 30);
}
