#include "Epub.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <JpegToBmpConverter.h>
#include <Logging.h>
#include <Memory.h>
#include <PngToBmpConverter.h>
#include <Utf8.h>
#include <ZipFile.h>

#include <algorithm>
#include <cctype>
#include <functional>

#include "Epub/parsers/ContainerParser.h"
#include "Epub/parsers/ContentOpfParser.h"
#include "Epub/parsers/TocNavParser.h"
#include "Epub/parsers/TocNcxParser.h"

Epub::Epub(std::string filepath, const std::string& cacheDir) : filepath(std::move(filepath)) {
  // v0.1.8 layout (unchanged): /.crosspoint/epub_<std::hash(path)>/
  cachePath = cacheDir + "/epub_" + std::to_string(std::hash<std::string>{}(this->filepath));
}

namespace {

// Calibre / Kindle often pack series into one dc:title:
//   "The Butcher's Masquerade: Dungeon Crawler Carl Book 5"
// Recents and home lists are more readable as the primary title only. Match the
// shorter names users saw on 0.1.7 (filename / short OPF), without losing books
// that intentionally use a colon in the real title (require series-like suffix).
std::string primaryBookTitle(std::string title) {
  // Trim ends.
  while (!title.empty() && (title.back() == ' ' || title.back() == '\t')) title.pop_back();
  size_t start = 0;
  while (start < title.size() && (title[start] == ' ' || title[start] == '\t')) ++start;
  if (start > 0) title = title.substr(start);

  const size_t colon = title.find(':');
  if (colon == std::string::npos || colon < 3) return title;

  std::string primary = title.substr(0, colon);
  while (!primary.empty() && (primary.back() == ' ' || primary.back() == '\t')) primary.pop_back();
  if (primary.size() < 3) return title;

  std::string rest = title.substr(colon + 1);
  while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t')) rest.erase(rest.begin());
  if (rest.size() < 4) return title;

  std::string restLower = rest;
  for (char& c : restLower) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
  }
  const bool seriesLike = restLower.find("book ") != std::string::npos ||
                          restLower.find(" book") != std::string::npos || restLower.find("vol.") != std::string::npos ||
                          restLower.find("volume ") != std::string::npos ||
                          restLower.find("series") != std::string::npos ||
                          // Long suffix after colon is usually series branding, not subtitle prose.
                          rest.size() >= 16;
  return seriesLike ? primary : title;
}

// Calibre often stores HTML in dc:description. Strip tags + a few entities for e-ink.
std::string stripHtmlToPlainText(const std::string& html) {
  std::string out;
  out.reserve(html.size());
  bool inTag = false;
  for (size_t i = 0; i < html.size(); ++i) {
    const char c = html[i];
    if (c == '<') {
      inTag = true;
      // Treat block tags as paragraph breaks when closing isn't visible.
      continue;
    }
    if (c == '>') {
      inTag = false;
      continue;
    }
    if (inTag) {
      continue;
    }
    if (c == '&') {
      if (html.compare(i, 5, "&amp;") == 0) {
        out.push_back('&');
        i += 4;
        continue;
      }
      if (html.compare(i, 4, "&lt;") == 0) {
        out.push_back('<');
        i += 3;
        continue;
      }
      if (html.compare(i, 4, "&gt;") == 0) {
        out.push_back('>');
        i += 3;
        continue;
      }
      if (html.compare(i, 6, "&nbsp;") == 0) {
        out.push_back(' ');
        i += 5;
        continue;
      }
      if (html.compare(i, 6, "&quot;") == 0) {
        out.push_back('"');
        i += 5;
        continue;
      }
      if (html.compare(i, 6, "&apos;") == 0) {
        out.push_back('\'');
        i += 5;
        continue;
      }
    }
    if (c == '\r') {
      continue;
    }
    out.push_back(c);
  }
  // Collapse runs of blank lines a bit.
  std::string collapsed;
  collapsed.reserve(out.size());
  int newlines = 0;
  for (const char c : out) {
    if (c == '\n') {
      if (++newlines <= 2) {
        collapsed.push_back(c);
      }
    } else {
      newlines = 0;
      collapsed.push_back(c);
    }
  }
  // Trim ends.
  size_t start = 0;
  while (start < collapsed.size() &&
         (collapsed[start] == ' ' || collapsed[start] == '\n' || collapsed[start] == '\t')) {
    ++start;
  }
  size_t end = collapsed.size();
  while (end > start && (collapsed[end - 1] == ' ' || collapsed[end - 1] == '\n' || collapsed[end - 1] == '\t')) {
    --end;
  }
  return collapsed.substr(start, end - start);
}

// v2 keeps Calibre HTML so the Description screen can render <p>/<b>/<i>.
// Older description.txt was plain-stripped; ignore it so we re-extract OPF HTML.
constexpr char kDescriptionCacheName[] = "/description.html";

bool writeDescriptionCache(const std::string& cachePath, const std::string& html) {
  if (html.empty()) {
    return false;
  }
  HalFile descFile;
  const std::string descPath = cachePath + kDescriptionCacheName;
  if (!Storage.openFileForWrite("EBP", descPath, descFile)) {
    return false;
  }
  descFile.write(reinterpret_cast<const uint8_t*>(html.data()), html.size());
  descFile.close();
  return true;
}

std::string readDescriptionCache(const std::string& cachePath) {
  const std::string descPath = cachePath + kDescriptionCacheName;
  if (!Storage.exists(descPath.c_str())) {
    return {};
  }
  HalFile descFile;
  if (!Storage.openFileForRead("EBP", descPath, descFile)) {
    return {};
  }
  const size_t size = descFile.size();
  if (size == 0 || size > 32 * 1024) {
    descFile.close();
    return {};
  }
  std::string text;
  text.resize(size);
  const size_t n = descFile.read(reinterpret_cast<uint8_t*>(&text[0]), size);
  descFile.close();
  text.resize(n);
  return text;
}

}  // namespace

bool Epub::findContentOpfFile(std::string* contentOpfFile) const {
  const auto containerPath = "META-INF/container.xml";
  size_t containerSize;

  // Get file size without loading it all into heap
  if (!getItemSize(containerPath, &containerSize)) {
    LOG_ERR("EBP", "Could not find or size META-INF/container.xml");
    return false;
  }

  ContainerParser containerParser(containerSize);

  if (!containerParser.setup()) {
    return false;
  }

  // Stream read (reusing your existing stream logic)
  if (!readItemContentsToStream(containerPath, containerParser, 512)) {
    LOG_ERR("EBP", "Could not read META-INF/container.xml");
    return false;
  }

  // Extract the result
  if (containerParser.fullPath.empty()) {
    LOG_ERR("EBP", "Could not find valid rootfile in container.xml");
    return false;
  }

  *contentOpfFile = std::move(containerParser.fullPath);
  return true;
}

bool Epub::parseContentOpf(BookMetadataCache::BookMetadata& bookMetadata, const bool writeSpineEntries) {
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    LOG_ERR("EBP", "Could not find content.opf in zip");
    return false;
  }

  contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  LOG_DBG("EBP", "Parsing content.opf: %s", contentOpfFilePath.c_str());

  size_t contentOpfSize;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    LOG_ERR("EBP", "Could not get size of content.opf");
    return false;
  }

  ContentOpfParser opfParser(getCachePath(), getBasePath(), contentOpfSize,
                             writeSpineEntries ? bookMetadataCache.get() : nullptr,
                             /*metadataOnly=*/!writeSpineEntries);
  if (!opfParser.setup()) {
    LOG_ERR("EBP", "Could not setup content.opf parser");
    return false;
  }

  const bool streamOk = readItemContentsToStream(contentOpfFilePath, opfParser, 1024,
                                                 /*allowEarlyStop=*/!writeSpineEntries);
  if (!streamOk) {
    // Metadata-only extract may still have captured description/title before stop.
    if (writeSpineEntries || (opfParser.description.empty() && opfParser.title.empty())) {
      LOG_ERR("EBP", "Could not read content.opf");
      return false;
    }
    LOG_DBG("EBP", "content.opf stream ended early after metadata (description extract)");
  }

  // Grab data from opfParser into epub. Normalize titles to NFC so NFD (combining
  // mark) text renders correctly — the device fonts have no mark positioning.
  // primaryBookTitle drops Calibre series packing ("Title: Series Book N").
  bookMetadata.title = primaryBookTitle(utf8ComposeNfc(opfParser.title));
  bookMetadata.author = opfParser.author;
  bookMetadata.language = opfParser.language;
  bookMetadata.coverItemHref = opfParser.coverItemHref;

  // Synopsis is not part of book.bin (stable format); cache as a side file.
  // Keep original HTML (Calibre often uses <p>/<b>/<i>) so the Description
  // screen can render paragraphs and emphasis; layout strips/styles at display.
  if (!opfParser.description.empty()) {
    writeDescriptionCache(cachePath, opfParser.description);
  }

  // Guide-based cover fallback only on full index (needs more zip reads).
  if (!writeSpineEntries) {
    return true;
  }

  // Guide-based cover fallback: if no cover found via metadata/properties,
  // try extracting the image reference from the guide's cover page XHTML
  if (bookMetadata.coverItemHref.empty() && !opfParser.guideCoverPageHref.empty()) {
    LOG_DBG("EBP", "No cover from metadata, trying guide cover page: %s", opfParser.guideCoverPageHref.c_str());
    size_t coverPageSize;
    uint8_t* coverPageData = readItemContentsToBytes(opfParser.guideCoverPageHref, &coverPageSize, true);
    if (coverPageData) {
      const std::string coverPageHtml(reinterpret_cast<char*>(coverPageData), coverPageSize);
      free(coverPageData);

      // Determine base path of the cover page for resolving relative image references
      std::string coverPageBase;
      const auto lastSlash = opfParser.guideCoverPageHref.rfind('/');
      if (lastSlash != std::string::npos) {
        coverPageBase = opfParser.guideCoverPageHref.substr(0, lastSlash + 1);
      }

      // Search for image references: xlink:href="..." (SVG) and src="..." (img)
      std::string imageRef;
      for (const char* pattern : {"xlink:href=\"", "src=\""}) {
        auto pos = coverPageHtml.find(pattern);
        while (pos != std::string::npos) {
          pos += strlen(pattern);
          const auto endPos = coverPageHtml.find('"', pos);
          if (endPos != std::string::npos) {
            const auto ref = std::string_view{coverPageHtml}.substr(pos, endPos - pos);
            // Cover BMP generation supports JPG/PNG only; skip GIF so an unsupported wrapper image
            // does not block a later supported cover reference.
            if (FsHelpers::hasPngExtension(ref) || FsHelpers::hasJpgExtension(ref)) {
              imageRef = ref;
              break;
            }
          }
          pos = coverPageHtml.find(pattern, pos);
        }
        if (!imageRef.empty()) break;
      }

      if (!imageRef.empty()) {
        bookMetadata.coverItemHref = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(coverPageBase + imageRef));
        LOG_DBG("EBP", "Found cover image from guide: %s", bookMetadata.coverItemHref.c_str());
      }
    }
  }

  bookMetadata.textReferenceHref = opfParser.textReferenceHref;

  if (!opfParser.tocNcxPath.empty()) {
    tocNcxItem = opfParser.tocNcxPath;
  }

  if (!opfParser.tocNavPath.empty()) {
    tocNavItem = opfParser.tocNavPath;
  }

  if (!opfParser.cssFiles.empty()) {
    cssFiles = opfParser.cssFiles;
  }

  LOG_DBG("EBP", "Successfully parsed content.opf");
  return true;
}

bool Epub::parseTocNcxFile() const {
  // the ncx file should have been specified in the content.opf file
  if (tocNcxItem.empty()) {
    LOG_DBG("EBP", "No ncx file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc ncx file: %s", tocNcxItem.c_str());

  size_t ncxSize;
  if (!getItemSize(tocNcxItem, &ncxSize)) {
    LOG_ERR("EBP", "Could not get size of toc ncx file");
    return false;
  }

  TocNcxParser ncxParser(contentBasePath, ncxSize, bookMetadataCache.get());

  if (!ncxParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc ncx parser");
    return false;
  }

  // Stream the decompressed NCX straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNcxItem, ncxParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc ncx file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC items");
  return true;
}

bool Epub::parseTocNavFile() const {
  // the nav file should have been specified in the content.opf file (EPUB 3)
  if (tocNavItem.empty()) {
    LOG_DBG("EBP", "No nav file specified");
    return false;
  }

  LOG_DBG("EBP", "Parsing toc nav file: %s", tocNavItem.c_str());

  size_t navSize;
  if (!getItemSize(tocNavItem, &navSize)) {
    LOG_ERR("EBP", "Could not get size of toc nav file");
    return false;
  }

  // Note: We can't use `contentBasePath` here as the nav file may be in a different folder to the content.opf
  // and the HTMLX nav file will have hrefs relative to itself
  const std::string navContentBasePath = tocNavItem.substr(0, tocNavItem.find_last_of('/') + 1);
  TocNavParser navParser(navContentBasePath, navSize, bookMetadataCache.get());

  if (!navParser.setup()) {
    LOG_ERR("EBP", "Could not setup toc nav parser");
    return false;
  }

  // Stream the decompressed nav document straight into the parser instead of round-tripping
  // through a temp file on the SD card (decompress -> write -> reopen -> reread -> delete).
  if (!readItemContentsToStream(tocNavItem, navParser, 1024)) {
    LOG_ERR("EBP", "Could not read toc nav file");
    return false;
  }

  LOG_DBG("EBP", "Parsed TOC nav items");
  return true;
}

void Epub::discoverCssFilesFromZip() {
  const std::string& opfDir = contentBasePath;
  ZipFile zf(filepath);

  if (!zf.enumerateFilePaths([&](std::string_view filePath) {
        if (!opfDir.empty() && filePath.find(opfDir) != 0) {
          return;
        }

        if (!FsHelpers::hasCssExtension(filePath)) {
          return;
        }

        if (std::find(cssFiles.begin(), cssFiles.end(), filePath) != cssFiles.end()) {
          return;
        }

        LOG_DBG("EBP", "Discovered CSS file via ZIP enumeration: %.*s", (int)filePath.size(), filePath.data());
        cssFiles.push_back(std::string{filePath});
      })) {
    LOG_ERR("EBP", "Failed to enumerate ZIP file paths for CSS discovery");
  }
}

void Epub::parseCssFiles() const {
  // Maximum CSS file size we'll attempt to parse (uncompressed)
  // Larger files risk memory exhaustion on ESP32
  constexpr size_t MAX_CSS_FILE_SIZE = 128 * 1024;  // 128KB
  // Minimum heap required before attempting CSS parsing
  // 48KB: God Emperor often sits ~60KB free after book.bin; 64KB was skipping style.css.
  constexpr size_t MIN_HEAP_FOR_CSS_PARSING = 48 * 1024;

  if (cssFiles.empty()) {
    LOG_DBG("EBP", "No CSS files to parse, but CssParser created for inline styles");
  }

  LOG_DBG("EBP", "CSS files to parse: %zu", cssFiles.size());

  // See if we have a cached version of the CSS rules
  if (cssParser->hasCache()) {
    LOG_DBG("EBP", "CSS cache exists, skipping parseCssFiles");
    return;
  }

  // Some converters emit one byte-identical stylesheet per chapter (100+ .css
  // entries), and each parse costs a zip locate plus an SD extract round-trip.
  // Map every CSS path to its central-directory (CRC32, compressed size) in a
  // single scan and parse only the first of each identical pair. Rules merge
  // into one global set, so dropping exact duplicates cannot lose styles. A
  // path that never matches a directory entry keeps key 0 and always parses.
  std::vector<uint64_t> dedupKeys(cssFiles.size(), 0);
  if (cssFiles.size() > 1) {
    std::unordered_map<std::string, size_t> pathToIndex;
    pathToIndex.reserve(cssFiles.size());
    for (size_t i = 0; i < cssFiles.size(); i++) {
      pathToIndex.emplace(FsHelpers::normalisePath(cssFiles[i]), i);
    }
    ZipFile(filepath).enumerateFileEntries([&](std::string_view entryPath, uint32_t crc32, uint32_t compressedSize) {
      if (!FsHelpers::hasCssExtension(entryPath)) {
        return;
      }
      const auto it = pathToIndex.find(std::string{entryPath});
      if (it != pathToIndex.end()) {
        dedupKeys[it->second] = (static_cast<uint64_t>(crc32) << 32) | compressedSize;
      }
    });
  }
  std::vector<uint64_t> seenKeys;
  seenKeys.reserve(cssFiles.size());
  size_t skippedDuplicates = 0;

  // No cache yet - parse CSS files
  for (size_t cssIndex = 0; cssIndex < cssFiles.size(); cssIndex++) {
    const auto& cssPath = cssFiles[cssIndex];
    const uint64_t dedupKey = dedupKeys[cssIndex];
    if (dedupKey != 0) {
      if (std::find(seenKeys.begin(), seenKeys.end(), dedupKey) != seenKeys.end()) {
        skippedDuplicates++;
        continue;
      }
      seenKeys.push_back(dedupKey);
    }
    LOG_DBG("EBP", "Parsing CSS file: %s", cssPath.c_str());

    // Check heap before parsing - CSS parsing allocates heavily
    const uint32_t freeHeap = ESP.getFreeHeap();
    if (freeHeap < MIN_HEAP_FOR_CSS_PARSING) {
      LOG_ERR("EBP", "Insufficient heap for CSS parsing (%u bytes free, need %zu), skipping: %s", freeHeap,
              MIN_HEAP_FOR_CSS_PARSING, cssPath.c_str());
      continue;
    }

    // Check CSS file size before decompressing - skip files that are too large
    size_t cssFileSize = 0;
    if (getItemSize(cssPath, &cssFileSize)) {
      if (cssFileSize > MAX_CSS_FILE_SIZE) {
        LOG_ERR("EBP", "CSS file too large (%zu bytes > %zu max), skipping: %s", cssFileSize, MAX_CSS_FILE_SIZE,
                cssPath.c_str());
        continue;
      }
    }

    // Extract CSS file to temp location
    const auto tmpCssPath = getCachePath() + "/.tmp.css";
    HalFile tempCssFile;
    if (!Storage.openFileForWrite("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not create temp CSS file");
      continue;
    }
    if (!readItemContentsToStream(cssPath, tempCssFile, 1024)) {
      LOG_ERR("EBP", "Could not read CSS file: %s", cssPath.c_str());
      // Explicitly close() file before calling Storage.remove()
      tempCssFile.close();
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    // Explicitly close() file before reopening for reading
    tempCssFile.close();

    // Parse the CSS file
    if (!Storage.openFileForRead("EBP", tmpCssPath, tempCssFile)) {
      LOG_ERR("EBP", "Could not open temp CSS file for reading");
      Storage.remove(tmpCssPath.c_str());
      continue;
    }
    cssParser->loadFromStream(tempCssFile);
    // Explicitly close() file before calling Storage.remove()
    tempCssFile.close();
    Storage.remove(tmpCssPath.c_str());
  }

  // Save to cache for next time
  if (!cssParser->saveToCache()) {
    LOG_ERR("EBP", "Failed to save CSS rules to cache");
  }

  LOG_DBG("EBP", "Loaded %zu CSS style rules from %zu files (%zu identical duplicates skipped)", cssParser->ruleCount(),
          cssFiles.size(), skippedDuplicates);
  cssParser->clear();
}

// load in the meta data for the epub file
bool Epub::load(const bool buildIfMissing, const bool skipLoadingCss) {
  LOG_DBG("EBP", "Loading ePub: %s", filepath.c_str());

  // Initialize spine/TOC cache.
  // nothrow throughout: bare `new` abort()s under -fno-exceptions, and book open
  // runs with the home cover, font cache and framebuffer all still resident.
  // Failing the load lets the caller show "could not open" instead of rebooting.
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "OOM: BookMetadataCache for %s", filepath.c_str());
    return false;
  }
  // Always create CssParser - needed for inline style parsing even without CSS files
  cssParser = makeUniqueNoThrow<CssParser>(cachePath);
  if (!cssParser) {
    LOG_ERR("EBP", "OOM: CssParser for %s", filepath.c_str());
    bookMetadataCache.reset();
    return false;
  }

  // Try to load existing cache first
  if (bookMetadataCache->load()) {
    if (!skipLoadingCss) {
      // Rebuild CSS cache when missing or when cache version changed (loadFromCache removes stale file)
      if (!cssParser->hasCache() || !cssParser->loadFromCache()) {
        LOG_DBG("EBP", "CSS rules cache missing or stale, attempting to parse CSS files");
        cssParser->deleteCache();

        BookMetadataCache::BookMetadata cachedMetadata = bookMetadataCache->coreMetadata;
        if (!parseContentOpf(cachedMetadata, /*writeSpineEntries=*/false)) {
          LOG_ERR("EBP", "Could not parse content.opf from cached bookMetadata for CSS files");
          // continue anyway - book will work without CSS and we'll still load any inline style CSS
        } else {
          discoverCssFilesFromZip();
        }
        bookMetadataCache.reset();
        parseCssFiles();
        bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
        if (!bookMetadataCache) {
          LOG_ERR("EBP", "OOM: BookMetadataCache reload after CSS rebuild");
          return false;
        }
        if (!bookMetadataCache->load()) {
          LOG_ERR("EBP", "Failed to reload cache after CSS rebuild");
          return false;
        }
        // Invalidate section caches so they are rebuilt with the new CSS
        Storage.removeDir((cachePath + "/sections").c_str());
      }
    }
    // Release the resolved CSS rule map: it is only needed transiently while building
    // section caches, and createSectionFile reloads it from cache on demand. Holding it
    // resident pins tens of KB for the whole reading session (more on warm resume into
    // an already-cached chapter, where createSectionFile never runs to clear it).
    cssParser->clear();
    LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
    return true;
  }

  // If we didn't load from cache above and we aren't allowed to build, fail now
  if (!buildIfMissing) {
    return false;
  }

  // Cache doesn't exist or is invalid, build it
  LOG_DBG("EBP", "Cache not found, building spine/TOC cache");
  setupCacheDir();

  const uint32_t indexingStart = millis();

  // Begin building cache - stream entries to disk immediately
  if (!bookMetadataCache->beginWrite()) {
    LOG_ERR("EBP", "Could not begin writing cache");
    return false;
  }

  // OPF Pass
  const uint32_t opfStart = millis();
  BookMetadataCache::BookMetadata bookMetadata;
  if (!bookMetadataCache->beginContentOpfPass()) {
    LOG_ERR("EBP", "Could not begin writing content.opf pass");
    return false;
  }
  if (!parseContentOpf(bookMetadata)) {
    LOG_ERR("EBP", "Could not parse content.opf");
    return false;
  }
  discoverCssFilesFromZip();
  if (!bookMetadataCache->endContentOpfPass()) {
    LOG_ERR("EBP", "Could not end writing content.opf pass");
    return false;
  }
  LOG_DBG("EBP", "OPF pass completed in %lu ms", millis() - opfStart);

  // TOC Pass - try EPUB 3 nav first, fall back to NCX
  const uint32_t tocStart = millis();
  if (!bookMetadataCache->beginTocPass()) {
    LOG_ERR("EBP", "Could not begin writing toc pass");
    return false;
  }

  bool tocParsed = false;

  // Try EPUB 3 nav document first (preferred)
  if (!tocNavItem.empty()) {
    LOG_DBG("EBP", "Attempting to parse EPUB 3 nav document");
    tocParsed = parseTocNavFile();
  }

  // Fall back to NCX if nav parsing failed or wasn't available
  if (!tocParsed && !tocNcxItem.empty()) {
    LOG_DBG("EBP", "Falling back to NCX TOC");
    tocParsed = parseTocNcxFile();
  }

  if (!tocParsed) {
    LOG_ERR("EBP", "Warning: Could not parse any TOC format");
    // Continue anyway - book will work without TOC
  }

  if (!bookMetadataCache->endTocPass()) {
    LOG_ERR("EBP", "Could not end writing toc pass");
    return false;
  }
  LOG_DBG("EBP", "TOC pass completed in %lu ms", millis() - tocStart);

  // Close the cache files
  if (!bookMetadataCache->endWrite()) {
    LOG_ERR("EBP", "Could not end writing cache");
    return false;
  }

  // Build final book.bin
  const uint32_t buildStart = millis();
  if (!bookMetadataCache->buildBookBin(filepath, bookMetadata)) {
    LOG_ERR("EBP", "Could not update mappings and sizes");
    return false;
  }
  LOG_DBG("EBP", "buildBookBin completed in %lu ms", millis() - buildStart);
  LOG_DBG("EBP", "Total indexing completed in %lu ms", millis() - indexingStart);

  if (!bookMetadataCache->cleanupTmpFiles()) {
    LOG_DBG("EBP", "Could not cleanup tmp files - ignoring");
  }

  if (!skipLoadingCss) {
    // Parse CSS before reloading book.bin to leave more heap for CSS rule-table growth.
    bookMetadataCache.reset();
    parseCssFiles();
    Storage.removeDir((cachePath + "/sections").c_str());
  }

  // Reload the cache from disk so it's in the correct state
  bookMetadataCache = makeUniqueNoThrow<BookMetadataCache>(cachePath);
  if (!bookMetadataCache) {
    LOG_ERR("EBP", "OOM: BookMetadataCache reload after writing");
    return false;
  }
  if (!bookMetadataCache->load()) {
    LOG_ERR("EBP", "Failed to reload cache after writing");
    return false;
  }

  LOG_DBG("EBP", "Loaded ePub: %s", filepath.c_str());
  return true;
}

bool Epub::clearCache() const {
  if (!Storage.exists(cachePath.c_str())) {
    LOG_DBG("EPB", "Cache does not exist, no action needed");
    return true;
  }

  if (!Storage.removeDir(cachePath.c_str())) {
    LOG_ERR("EPB", "Failed to clear cache");
    return false;
  }

  LOG_DBG("EPB", "Cache cleared successfully");
  return true;
}

void Epub::setupCacheDir() const {
  if (Storage.exists(cachePath.c_str())) {
    return;
  }
  Storage.ensureDirectoryExists(cachePath.c_str());
}

const std::string& Epub::getCachePath() const { return cachePath; }

const std::string& Epub::getPath() const { return filepath; }

const std::string& Epub::getTitle() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.title;
}

const std::string& Epub::getAuthor() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.author;
}

const std::string& Epub::getLanguage() const {
  static std::string blank;
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return blank;
  }

  return bookMetadataCache->coreMetadata.language;
}

std::string Epub::getDescription() {
  const uint32_t t0 = millis();
  std::string cached = readDescriptionCache(cachePath);
  if (!cached.empty()) {
    LOG_DBG("EBP", "getDescription: cache hit %u bytes in %lums", static_cast<unsigned>(cached.size()),
            static_cast<unsigned long>(millis() - t0));
    return cached;
  }

  // Works without a full book index: open the EPUB zip, parse OPF until
  // dc:description (or </metadata>), then early-stop ZIP inflate — no spine/TOC.
  setupCacheDir();
  BookMetadataCache::BookMetadata meta;
  const uint32_t tOpf = millis();
  if (!parseContentOpf(meta, /*writeSpineEntries=*/false)) {
    LOG_DBG("EBP", "getDescription: metadata-only OPF parse failed for %s (%lums)", filepath.c_str(),
            static_cast<unsigned long>(millis() - t0));
    return {};
  }
  cached = readDescriptionCache(cachePath);
  if (cached.empty()) {
    LOG_DBG("EBP", "getDescription: no dc:description in OPF for %s (opf %lums total %lums)", filepath.c_str(),
            static_cast<unsigned long>(millis() - tOpf), static_cast<unsigned long>(millis() - t0));
  } else {
    LOG_DBG("EBP", "getDescription: OPF extract %u bytes opf=%lums total=%lums", static_cast<unsigned>(cached.size()),
            static_cast<unsigned long>(millis() - tOpf), static_cast<unsigned long>(millis() - t0));
  }
  return cached;
}

std::string Epub::getCoverBmpPath(bool cropped) const {
  const auto coverFileName = std::string("cover") + (cropped ? "_crop" : "");
  return cachePath + "/" + coverFileName + ".bmp";
}

bool Epub::generateCoverBmp(bool cropped) const {
  // Already generated, return true
  if (Storage.exists(getCoverBmpPath(cropped).c_str())) {
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate cover BMP, cache not loaded");
    return false;
  }

  auto tryGenerateFromHref = [&](const std::string& coverImageHref) -> bool {
    if (coverImageHref.empty()) {
      return false;
    }
    size_t itemSize = 0;
    if (!getItemSize(coverImageHref, &itemSize) || itemSize == 0) {
      LOG_DBG("EBP", "Cover item missing in zip: %s", coverImageHref.c_str());
      return false;
    }

    if (FsHelpers::hasJpgExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating BMP from JPG cover image (%s mode): %s", cropped ? "cropped" : "fit",
              coverImageHref.c_str());
      const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

      HalFile coverJpg;
      if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
        return false;
      }
      if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
        coverJpg.close();
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }
      coverJpg.close();

      if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }

      HalFile coverBmp;
      if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
        coverJpg.close();
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }
      const bool success = JpegToBmpConverter::jpegFileToBmpStream(coverJpg, coverBmp, cropped);
      coverJpg.close();
      coverBmp.close();
      Storage.remove(coverJpgTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate BMP from cover image");
        Storage.remove(getCoverBmpPath(cropped).c_str());
      }
      return success;
    }

    if (FsHelpers::hasPngExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating BMP from PNG cover image (%s mode): %s", cropped ? "cropped" : "fit",
              coverImageHref.c_str());
      const auto coverPngTempPath = getCachePath() + "/.cover.png";

      HalFile coverPng;
      if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
        return false;
      }
      if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
        coverPng.close();
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }
      coverPng.close();

      if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }

      HalFile coverBmp;
      if (!Storage.openFileForWrite("EBP", getCoverBmpPath(cropped), coverBmp)) {
        coverPng.close();
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }
      const bool success = PngToBmpConverter::pngFileToBmpStream(coverPng, coverBmp, cropped);
      coverPng.close();
      coverBmp.close();
      Storage.remove(coverPngTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate BMP from PNG cover image");
        Storage.remove(getCoverBmpPath(cropped).c_str());
      }
      return success;
    }

    LOG_ERR("EBP", "Cover image is not a supported format: %s", coverImageHref.c_str());
    return false;
  };

  if (tryGenerateFromHref(bookMetadataCache->coreMetadata.coverItemHref)) {
    return true;
  }

  std::string freshHref;
  if (resolveCoverItemHrefFromOpf(freshHref) && tryGenerateFromHref(freshHref)) {
    const_cast<BookMetadataCache&>(*bookMetadataCache).coreMetadata.coverItemHref = std::move(freshHref);
    return true;
  }

  LOG_ERR("EBP", "No known cover image");
  return false;
}

// c30: CrossPoint v0.1.3 home cover recipe — 2-bit balanced Atkinson + mild lift.
// Stats + Stats-Life share one height key (same thumb file). Bare 420×560 1:1.
// c24 (480×640) forced Bare to scale down and looked griddy — do not reintroduce.
std::string Epub::getThumbBmpPath() const { return cachePath + "/thumb_c30_[HEIGHT].bmp"; }
std::string Epub::getThumbBmpPath(int height) const {
  return cachePath + "/thumb_c30_" + std::to_string(height) + ".bmp";
}

// Re-read OPF (full, including manifest) so cover href is current. book.bin can
// hold a stale path after an EPUB is replaced on the SD card (e.g. cover.jpg vs cover.jpeg).
bool Epub::resolveCoverItemHrefFromOpf(std::string& outHref) const {
  outHref.clear();
  std::string contentOpfFilePath;
  if (!findContentOpfFile(&contentOpfFilePath)) {
    return false;
  }
  // Parser normalizes hrefs with contentBasePath.
  const_cast<Epub*>(this)->contentBasePath = contentOpfFilePath.substr(0, contentOpfFilePath.find_last_of('/') + 1);

  size_t contentOpfSize = 0;
  if (!getItemSize(contentOpfFilePath, &contentOpfSize)) {
    return false;
  }

  // metadataOnly=false so the manifest is scanned; cache=null so no spine rewrite.
  ContentOpfParser opfParser(getCachePath(), const_cast<Epub*>(this)->getBasePath(), contentOpfSize, nullptr,
                             /*metadataOnly=*/false);
  if (!opfParser.setup()) {
    return false;
  }
  if (!readItemContentsToStream(contentOpfFilePath, opfParser, 1024, /*allowEarlyStop=*/false)) {
    return false;
  }
  outHref = opfParser.coverItemHref;
  return !outHref.empty();
}

bool Epub::generateThumbBmp(int height) const {
  // Already generated — but only trust files that look like real BMPs.
  // A truncated/corrupt file (e.g. mid-write OOM) used to block regen forever.
  // Probe by open first: exists() false-negatives after the reader used to
  // fall through into a 10–30s JPEG decode on every Home return.
  const std::string existingPath = getThumbBmpPath(height);
  HalFile probe;
  bool opened = false;
  bool valid = false;
  if (Storage.openFileForRead("EBP", existingPath, probe)) {
    opened = true;
    char sig[2] = {};
    const size_t n = probe.read(sig, 2);
    const size_t sz = probe.size();
    probe.close();
    valid = (n == 2 && sig[0] == 'B' && sig[1] == 'M' && sz > 62);
  }
  if (opened && valid) {
    LOG_DBG("EBP", "thumb cache hit %s", existingPath.c_str());
    return true;
  }
  if (opened && !valid) {
    LOG_ERR("EBP", "Removing corrupt thumb: %s", existingPath.c_str());
    Storage.remove(existingPath.c_str());
  } else if (!opened && Storage.exists(existingPath.c_str())) {
    // Open-fail after the reader is SD-busy, not a missing file.
    LOG_DBG("EBP", "thumb cache hit %s opened=0", existingPath.c_str());
    return true;
  }

  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "Cannot generate thumb BMP, cache not loaded");
    return false;
  }

  auto tryGenerateFromHref = [&](const std::string& coverImageHref) -> bool {
    if (coverImageHref.empty()) {
      return false;
    }
    // Skip stale book.bin paths that no longer exist inside the EPUB zip.
    size_t itemSize = 0;
    if (!getItemSize(coverImageHref, &itemSize) || itemSize == 0) {
      LOG_DBG("EBP", "Cover item missing in zip: %s", coverImageHref.c_str());
      return false;
    }

    if (FsHelpers::hasJpgExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating thumb BMP from JPG cover image: %s", coverImageHref.c_str());
      const auto coverJpgTempPath = getCachePath() + "/.cover.jpg";

      HalFile coverJpg;
      if (!Storage.openFileForWrite("EBP", coverJpgTempPath, coverJpg)) {
        return false;
      }
      if (!readItemContentsToStream(coverImageHref, coverJpg, 1024)) {
        coverJpg.close();
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }
      coverJpg.close();

      if (!Storage.openFileForRead("EBP", coverJpgTempPath, coverJpg)) {
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }

      HalFile thumbBmp;
      if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
        coverJpg.close();
        Storage.remove(coverJpgTempPath.c_str());
        return false;
      }
      // Wide bounding box + contain-fit (c19): full jacket, no side crop.
      // Box is ~3:4 so typical covers keep more horizontal detail than 2:3 crop.
      const int THUMB_TARGET_HEIGHT = height;
      const int THUMB_TARGET_WIDTH = std::max(1, (height * 3 + 1) / 4);
      LOG_DBG("EBP", "Thumb JPG free heap before decode: %u", static_cast<unsigned>(ESP.getFreeHeap()));
      const bool success = JpegToBmpConverter::jpegFileToHighQualityCoverThumbBmpStreamWithSize(
          coverJpg, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT);
      coverJpg.close();
      thumbBmp.close();
      Storage.remove(coverJpgTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate thumb BMP from JPG cover image (heap=%u)",
                static_cast<unsigned>(ESP.getFreeHeap()));
        Storage.remove(getThumbBmpPath(height).c_str());
      }
      return success;
    }

    if (FsHelpers::hasPngExtension(coverImageHref)) {
      LOG_DBG("EBP", "Generating thumb BMP from PNG cover image: %s", coverImageHref.c_str());
      const auto coverPngTempPath = getCachePath() + "/.cover.png";

      HalFile coverPng;
      if (!Storage.openFileForWrite("EBP", coverPngTempPath, coverPng)) {
        return false;
      }
      if (!readItemContentsToStream(coverImageHref, coverPng, 1024)) {
        coverPng.close();
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }
      coverPng.close();

      if (!Storage.openFileForRead("EBP", coverPngTempPath, coverPng)) {
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }

      HalFile thumbBmp;
      if (!Storage.openFileForWrite("EBP", getThumbBmpPath(height), thumbBmp)) {
        coverPng.close();
        Storage.remove(coverPngTempPath.c_str());
        return false;
      }
      const int THUMB_TARGET_HEIGHT = height;
      const int THUMB_TARGET_WIDTH = std::max(1, (height * 3 + 1) / 4);
      LOG_DBG("EBP", "Thumb PNG free heap before decode: %u", static_cast<unsigned>(ESP.getFreeHeap()));
      const bool success = PngToBmpConverter::pngFileToCoverThumbBmpStreamWithSize(
          coverPng, thumbBmp, THUMB_TARGET_WIDTH, THUMB_TARGET_HEIGHT);
      coverPng.close();
      thumbBmp.close();
      Storage.remove(coverPngTempPath.c_str());

      if (!success) {
        LOG_ERR("EBP", "Failed to generate thumb BMP from PNG cover image");
        Storage.remove(getThumbBmpPath(height).c_str());
      }
      return success;
    }

    LOG_ERR("EBP", "Cover image is not a supported format: %s", coverImageHref.c_str());
    return false;
  };

  // 1) Try path stored in book.bin (fast when still valid).
  if (tryGenerateFromHref(bookMetadataCache->coreMetadata.coverItemHref)) {
    return true;
  }

  // 2) Re-parse live OPF — fixes caches that still point at cover.jpg after the
  //    EPUB was updated to cover.jpeg (e.g. Dungeon Crawler Carl, Gate of the Feral Gods).
  std::string freshHref;
  if (resolveCoverItemHrefFromOpf(freshHref)) {
    LOG_DBG("EBP", "Cover href from OPF: %s (cached was: %s)", freshHref.c_str(),
            bookMetadataCache->coreMetadata.coverItemHref.c_str());
    if (tryGenerateFromHref(freshHref)) {
      // Keep session cache current so cover BMP / other heights skip OPF reparse.
      const_cast<BookMetadataCache&>(*bookMetadataCache).coreMetadata.coverItemHref = std::move(freshHref);
      return true;
    }
  } else {
    LOG_DBG("EBP", "No known cover image for thumbnail");
  }

  Storage.remove(getThumbBmpPath(height).c_str());
  return false;
}

uint8_t* Epub::readItemContentsToBytes(const std::string& itemHref, size_t* size, const bool trailingNullByte) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return nullptr;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);

  const auto content = ZipFile(filepath).readFileToMemory(path.c_str(), size, trailingNullByte);
  if (!content) {
    LOG_DBG("EBP", "Failed to read item %s", path.c_str());
    return nullptr;
  }

  return content;
}

bool Epub::readItemContentsToStream(const std::string& itemHref, Print& out, const size_t chunkSize,
                                    const bool allowEarlyStop) const {
  if (itemHref.empty()) {
    LOG_DBG("EBP", "Failed to read item, empty href");
    return false;
  }

  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).readFileToStream(path.c_str(), out, chunkSize, allowEarlyStop);
}

bool Epub::extractItemToFile(const std::string& itemHref, const std::string& destPath) const {
  HalFile out;
  if (!Storage.openFileForWrite("EBP", destPath, out)) {
    return false;
  }
  const bool ok = readItemContentsToStream(itemHref, out, 4096);
  out.flush();
  out.close();
  if (!ok) {
    Storage.remove(destPath.c_str());
  }
  return ok;
}

bool Epub::getItemSize(const std::string& itemHref, size_t* size) const {
  const std::string path = FsHelpers::normalisePath(itemHref);
  return ZipFile(filepath).getInflatedFileSize(path.c_str(), size);
}

int Epub::getSpineItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }
  return bookMetadataCache->getSpineCount();
}

size_t Epub::getCumulativeSpineItemSize(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return 0;
  return bookMetadataCache->getSpineCumulativeSize(spineIndex);
}

BookMetadataCache::SpineEntry Epub::getSpineItem(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineItem called but cache not loaded");
    return {};
  }

  if (spineIndex < 0 || spineIndex >= bookMetadataCache->getSpineCount()) {
    LOG_ERR("EBP", "getSpineItem index:%d is out of range", spineIndex);
    return bookMetadataCache->getSpineEntry(0);
  }

  return bookMetadataCache->getSpineEntry(spineIndex);
}

BookMetadataCache::TocEntry Epub::getTocItem(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_DBG("EBP", "getTocItem called but cache not loaded");
    return {};
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_DBG("EBP", "getTocItem index:%d is out of range", tocIndex);
    return {};
  }

  return bookMetadataCache->getTocEntry(tocIndex);
}

int Epub::getTocItemsCount() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    return 0;
  }

  return bookMetadataCache->getTocCount();
}

// work out the section index for a toc index
int Epub::getSpineIndexForTocIndex(const int tocIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex called but cache not loaded");
    return 0;
  }

  if (tocIndex < 0 || tocIndex >= bookMetadataCache->getTocCount()) {
    LOG_ERR("EBP", "getSpineIndexForTocIndex: tocIndex %d out of range", tocIndex);
    return 0;
  }

  const int spineIndex = bookMetadataCache->getTocEntry(tocIndex).spineIndex;
  if (spineIndex < 0) {
    LOG_DBG("EBP", "Section not found for TOC index %d", tocIndex);
    return 0;
  }

  return spineIndex;
}

int Epub::getTocIndexForSpineIndex(const int spineIndex) const { return getSpineItem(spineIndex).tocIndex; }

size_t Epub::getBookSize() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded() || bookMetadataCache->getSpineCount() == 0) {
    return 0;
  }
  return getCumulativeSpineItemSize(getSpineItemsCount() - 1);
}

int Epub::getSpineIndexForTextReference() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) {
    LOG_ERR("EBP", "getSpineIndexForTextReference called but cache not loaded");
    return 0;
  }
  LOG_DBG("EBP", "Core Metadata: cover(%d)=%s, textReference(%d)=%s",
          bookMetadataCache->coreMetadata.coverItemHref.size(), bookMetadataCache->coreMetadata.coverItemHref.c_str(),
          bookMetadataCache->coreMetadata.textReferenceHref.size(),
          bookMetadataCache->coreMetadata.textReferenceHref.c_str());

  if (bookMetadataCache->coreMetadata.textReferenceHref.empty()) {
    // there was no textReference in epub, so we return 0 (the first chapter)
    return 0;
  }

  // loop through spine items to get the correct index matching the text href
  for (size_t i = 0; i < getSpineItemsCount(); i++) {
    if (getSpineItem(i).href == bookMetadataCache->coreMetadata.textReferenceHref) {
      LOG_DBG("EBP", "Text reference %s found at index %d", bookMetadataCache->coreMetadata.textReferenceHref.c_str(),
              i);
      return i;
    }
  }
  // This should not happen, as we checked for empty textReferenceHref earlier
  LOG_DBG("EBP", "Section not found for text reference");
  return 0;
}

namespace {
// Basename of a spine href, lowercased, extension stripped, _ → - for matching.
std::string spineHrefBaseName(const std::string& href) {
  std::string path = href;
  const size_t hash = path.find('#');
  if (hash != std::string::npos) path.resize(hash);
  const size_t slash = path.find_last_of("/\\");
  if (slash != std::string::npos) path = path.substr(slash + 1);
  const size_t dot = path.find_last_of('.');
  if (dot != std::string::npos) path.resize(dot);
  for (char& c : path) {
    if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    if (c == '_') c = '-';
  }
  return path;
}

bool baseNameLooksCoverOnly(const std::string& base) {
  if (base.empty()) return false;
  // Exact short names publishers use for image-only / near-empty front matter.
  if (base == "cover" || base == "cover-page" || base == "coverpage" || base == "book-cover" || base == "title" ||
      base == "title-page" || base == "titlepage" || base == "half-title" || base == "halftitle" ||
      base == "half-title-page" || base == "frontispiece" || base == "jacket") {
    return true;
  }
  // cover01, cover-01, cover_image, etc. — keep short so "coverage" / "recover" stay out.
  if (base.rfind("cover", 0) == 0 && base.size() <= 16) return true;
  if (base.find("titlepage") != std::string::npos) return true;
  if (base.find("title-page") != std::string::npos) return true;
  if (base.find("coverpage") != std::string::npos) return true;
  if (base.find("cover-page") != std::string::npos) return true;
  return false;
}
}  // namespace

bool Epub::spineLooksCoverOnly(const int spineIndex) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return false;
  const int n = getSpineItemsCount();
  if (spineIndex < 0 || spineIndex >= n) return false;

  const auto entry = getSpineItem(spineIndex);
  const std::string base = spineHrefBaseName(entry.href);
  if (!baseNameLooksCoverOnly(base)) return false;

  // Large spine with a cover-ish name is still real content — do not skip.
  const uint32_t cum = entry.cumulativeSize;
  const uint32_t prev = spineIndex > 0 ? getSpineItem(spineIndex - 1).cumulativeSize : 0;
  const uint32_t bytes = cum >= prev ? (cum - prev) : cum;
  constexpr uint32_t kMaxCoverOnlyBytes = 48 * 1024;  // inflated XHTML; pure img wrappers are tiny
  if (bytes > kMaxCoverOnlyBytes) {
    LOG_DBG("EBP", "Spine %d name looks cover (%s) but size %u — treating as content", spineIndex, base.c_str(),
            static_cast<unsigned>(bytes));
    return false;
  }
  return true;
}

int Epub::getFirstOpenSpineIndex() const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return 0;

  // 1) Publisher-declared body start (OPF guide text/start).
  const int textRef = getSpineIndexForTextReference();
  if (textRef > 0) {
    LOG_DBG("EBP", "First-open land: OPF text reference spine %d", textRef);
    return textRef;
  }

  // 2) Skip only cover/title-like spines at the front (href name + small size).
  // Cap so we never walk deep into the book if naming is weird.
  const int n = getSpineItemsCount();
  if (n <= 1) return 0;
  constexpr int kMaxCoverSkips = 3;
  const int limit = std::min(n - 1, kMaxCoverSkips);
  for (int i = 0; i <= limit; ++i) {
    if (!spineLooksCoverOnly(i)) {
      if (i > 0) {
        LOG_DBG("EBP", "First-open land: skipped %d cover-like spine(s) → spine %d", i, i);
      }
      return i;
    }
  }
  LOG_DBG("EBP", "First-open land: first %d spines look cover-like — staying at 0", limit + 1);
  return 0;
}

// Calculate progress in book (returns 0.0-1.0)
float Epub::calculateProgress(const int currentSpineIndex, const float currentSpineRead) const {
  const size_t bookSize = getBookSize();
  if (bookSize == 0) {
    return 0.0f;
  }
  const size_t prevChapterSize = (currentSpineIndex >= 1) ? getCumulativeSpineItemSize(currentSpineIndex - 1) : 0;
  const size_t curChapterSize = getCumulativeSpineItemSize(currentSpineIndex) - prevChapterSize;
  const float sectionProgSize = currentSpineRead * static_cast<float>(curChapterSize);
  const float totalProgress = static_cast<float>(prevChapterSize) + sectionProgSize;
  return totalProgress / static_cast<float>(bookSize);
}

int Epub::resolveHrefToSpineIndex(const std::string& href) const {
  if (!bookMetadataCache || !bookMetadataCache->isLoaded()) return -1;

  // Split before decoding so escaped '#' characters in filenames stay part of the path.
  const size_t hashPos = href.find('#');
  const std::string rawTarget = hashPos != std::string::npos ? href.substr(0, hashPos) : href;
  const std::string target = FsHelpers::normalisePath(FsHelpers::decodeUriEscapes(rawTarget));

  // Same-file reference (anchor-only)
  if (target.empty()) return -1;

  // Extract just the filename for comparison
  size_t targetSlash = target.find_last_of('/');
  std::string targetFilename = (targetSlash != std::string::npos) ? target.substr(targetSlash + 1) : target;

  for (int i = 0; i < getSpineItemsCount(); i++) {
    const auto& spineHref = getSpineItem(i).href;
    // Try exact match first
    if (spineHref == target) return i;
    // Then filename-only match
    size_t spineSlash = spineHref.find_last_of('/');
    std::string spineFilename = (spineSlash != std::string::npos) ? spineHref.substr(spineSlash + 1) : spineHref;
    if (spineFilename == targetFilename) return i;
  }
  return -1;
}
