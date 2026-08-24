#include "FinishedBooks.h"

#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>

#include <cctype>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

#include "CrossPointState.h"
#include "RecentBooksStore.h"
#include "util/CrossPointBookStore.h"
#include "util/CrossPointPaths.h"

namespace FinishedBooks {
namespace {

bool iequals(const char* a, const char* b) {
  while (*a && *b) {
    if (tolower(static_cast<unsigned char>(*a)) != tolower(static_cast<unsigned char>(*b))) {
      return false;
    }
    ++a;
    ++b;
  }
  return *a == '\0' && *b == '\0';
}

bool pathStartsWithFolder(const std::string& path, const char* folder) {
  const size_t n = strlen(folder);
  return path.size() > n && path.compare(0, n, folder) == 0 && path[n] == '/';
}

std::string cachePathForEpub(const std::string& epubPath) {
  // v0.1.8 package dir: /.crosspoint/epub_<std::hash>
  return CrossPointBook::bookDirForPath(epubPath);
}

bool writeOriginSidecar(const std::string& cachePath, const std::string& originPath) {
  if (cachePath.empty() || originPath.empty()) return false;
  HalFile f;
  if (!Storage.openFileForWrite("FIN", cachePath + "/" + ORIGIN_SIDECAR, f)) {
    LOG_ERR("FIN", "Could not write origin sidecar");
    return false;
  }
  const size_t n = f.write(originPath.data(), originPath.size());
  f.flush();
  return n == originPath.size();
}

std::string readOriginSidecar(const std::string& cachePath) {
  HalFile f;
  if (!Storage.openFileForRead("FIN", cachePath + "/" + ORIGIN_SIDECAR, f)) {
    return {};
  }
  char buf[512];
  int n = f.read(buf, sizeof(buf) - 1);
  if (n <= 0) return {};
  buf[n] = '\0';
  std::string out(buf, static_cast<size_t>(n));
  while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' ')) {
    out.pop_back();
  }
  return out;
}

void removeOriginSidecar(const std::string& cachePath) {
  if (cachePath.empty()) return;
  Storage.remove((cachePath + "/" + ORIGIN_SIDECAR).c_str());
}

void repointPaths(const std::string& oldPath, const std::string& newPath, const std::string& oldCache,
                  const std::string& newCache) {
  RECENT_BOOKS.updatePath(oldPath, newPath, oldCache, newCache);
  if (APP_STATE.openEpubPath == oldPath) {
    APP_STATE.openEpubPath = newPath;
    APP_STATE.saveToFile();
  }
}

std::string titleFromFilename(const std::string& filename) {
  std::string t = filename;
  const size_t dot = t.rfind('.');
  if (dot != std::string::npos && dot > 0) {
    t = t.substr(0, dot);
  }
  return t;
}

void appendBooksFromDir(const char* folder, std::vector<FinishedBookEntry>& out) {
  auto dir = Storage.open(folder);
  if (!dir || !dir.isDirectory()) {
    if (dir) dir.close();
    // Trailing slash fallback (some FAT layers).
    std::string withSlash = std::string(folder) + "/";
    dir = Storage.open(withSlash.c_str());
    if (!dir || !dir.isDirectory()) {
      if (dir) dir.close();
      return;
    }
  }
  char nameBuf[256];
  for (auto file = dir.openNextFile(); file; file = dir.openNextFile()) {
    if (file.isDirectory()) continue;
    file.getName(nameBuf, sizeof(nameBuf));
    if (strcmp(nameBuf, ".") == 0 || strcmp(nameBuf, "..") == 0) continue;
    std::string_view filename{nameBuf};
    if (!FsHelpers::hasEpubExtension(filename) && !FsHelpers::hasXtcExtension(filename) &&
        !FsHelpers::hasTxtExtension(filename) && !FsHelpers::hasMarkdownExtension(filename)) {
      continue;
    }
    FinishedBookEntry e;
    e.path = std::string(folder) + "/" + nameBuf;
    e.title = titleFromFilename(nameBuf);
    out.push_back(std::move(e));
  }
  dir.close();
}

}  // namespace

bool isInFinishedFolder(const std::string& path) {
  return pathStartsWithFolder(path, FOLDER) || pathStartsWithFolder(path, ALT_FOLDER);
}

bool isFinishedDirName(const std::string& nameWithOptionalSlash) {
  std::string name = nameWithOptionalSlash;
  if (!name.empty() && name.back() == '/') {
    name.pop_back();
  }
  if (!name.empty() && name[0] == '/') {
    name = name.substr(1);
  }
  return iequals(name.c_str(), "read") || iequals(name.c_str(), "Finished Books");
}

std::string buildDestination(const std::string& srcPath) {
  Storage.mkdir(FOLDER);

  const size_t lastSlash = srcPath.rfind('/');
  const std::string filename = (lastSlash != std::string::npos) ? srcPath.substr(lastSlash + 1) : srcPath;

  std::string dstPath = std::string(FOLDER) + "/" + filename;
  if (!Storage.exists(dstPath.c_str())) {
    return dstPath;
  }

  const size_t dotPos = filename.rfind('.');
  const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
  const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
  int suffix = 2;
  do {
    dstPath = std::string(FOLDER) + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
    suffix++;
  } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  return dstPath;
}

std::string moveToFinished(const std::string& srcPath) {
  if (srcPath.empty() || isInFinishedFolder(srcPath)) {
    return srcPath;
  }
  if (!Storage.exists(srcPath.c_str())) {
    LOG_ERR("FIN", "Source missing: %s", srcPath.c_str());
    return {};
  }

  const std::string dstPath = buildDestination(srcPath);
  const std::string oldCache = cachePathForEpub(srcPath);
  const std::string newCache = cachePathForEpub(dstPath);

  LOG_INF("FIN", "Moving finished epub: %s -> %s", srcPath.c_str(), dstPath.c_str());
  if (!Storage.rename(srcPath.c_str(), dstPath.c_str())) {
    LOG_ERR("FIN", "Failed to move epub to /read");
    return {};
  }

  if (!oldCache.empty() && Storage.exists(oldCache.c_str())) {
    if (Storage.exists(newCache.c_str())) {
      LOG_ERR("FIN", "Target cache already exists: %s (keeping %s)", newCache.c_str(), oldCache.c_str());
    } else if (!Storage.rename(oldCache.c_str(), newCache.c_str())) {
      LOG_ERR("FIN", "Failed to rename cache %s -> %s (non-fatal)", oldCache.c_str(), newCache.c_str());
    }
  }

  const std::string sidecarCache =
      Storage.exists(newCache.c_str()) ? newCache : (Storage.exists(oldCache.c_str()) ? oldCache : newCache);
  if (!sidecarCache.empty()) {
    Storage.mkdir(sidecarCache.c_str());
    writeOriginSidecar(sidecarCache, srcPath);
  }

  repointPaths(srcPath, dstPath, oldCache, newCache);
  return dstPath;
}

std::string restoreFromFinished(const std::string& currentPath) {
  if (currentPath.empty() || !isInFinishedFolder(currentPath)) {
    return currentPath;
  }
  if (!Storage.exists(currentPath.c_str())) {
    LOG_ERR("FIN", "Finished book missing: %s", currentPath.c_str());
    return {};
  }

  const std::string curCache = cachePathForEpub(currentPath);
  std::string origin = readOriginSidecar(curCache);
  if (origin.empty()) {
    const size_t slash = currentPath.rfind('/');
    const std::string filename = (slash != std::string::npos) ? currentPath.substr(slash + 1) : currentPath;
    origin = std::string("/") + filename;
    LOG_DBG("FIN", "No origin sidecar; restoring to %s", origin.c_str());
  }

  std::string dstPath = origin;
  if (Storage.exists(dstPath.c_str()) && dstPath != currentPath) {
    const size_t lastSlash = dstPath.rfind('/');
    const std::string dir = (lastSlash != std::string::npos) ? dstPath.substr(0, lastSlash) : "";
    const std::string filename = (lastSlash != std::string::npos) ? dstPath.substr(lastSlash + 1) : dstPath;
    const size_t dotPos = filename.rfind('.');
    const std::string base = (dotPos != std::string::npos) ? filename.substr(0, dotPos) : filename;
    const std::string ext = (dotPos != std::string::npos) ? filename.substr(dotPos) : "";
    int suffix = 2;
    do {
      dstPath = dir + "/" + base + " (" + std::to_string(suffix) + ")" + ext;
      suffix++;
    } while (Storage.exists(dstPath.c_str()) && suffix < 100);
  }

  {
    const size_t lastSlash = dstPath.rfind('/');
    if (lastSlash != std::string::npos && lastSlash > 0) {
      const std::string parent = dstPath.substr(0, lastSlash);
      for (size_t i = 1; i < parent.size(); ++i) {
        if (parent[i] == '/') {
          Storage.mkdir(parent.substr(0, i).c_str());
        }
      }
      Storage.mkdir(parent.c_str());
    }
  }

  const std::string newCache = cachePathForEpub(dstPath);
  LOG_INF("FIN", "Restoring unfinished epub: %s -> %s", currentPath.c_str(), dstPath.c_str());
  if (!Storage.rename(currentPath.c_str(), dstPath.c_str())) {
    LOG_ERR("FIN", "Failed to restore epub from /read");
    return {};
  }

  if (!curCache.empty() && Storage.exists(curCache.c_str())) {
    if (!Storage.exists(newCache.c_str())) {
      if (!Storage.rename(curCache.c_str(), newCache.c_str())) {
        LOG_ERR("FIN", "Failed to rename cache back %s -> %s", curCache.c_str(), newCache.c_str());
      }
    }
  }

  removeOriginSidecar(Storage.exists(newCache.c_str()) ? newCache : curCache);
  repointPaths(currentPath, dstPath, curCache, newCache);
  return dstPath;
}

void listFinishedBooks(std::vector<FinishedBookEntry>& out) {
  out.clear();
  appendBooksFromDir(FOLDER, out);
  appendBooksFromDir(ALT_FOLDER, out);
  // Prefer recents metadata (title/author) when path matches.
  for (auto& e : out) {
    const RecentBook meta = RECENT_BOOKS.getDataFromBook(e.path);
    if (!meta.path.empty() && !meta.title.empty()) {
      e.title = meta.title;
    }
  }
}

}  // namespace FinishedBooks
