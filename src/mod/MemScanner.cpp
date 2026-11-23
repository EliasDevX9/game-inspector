#include "MemScanner.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <string>

#include <fcntl.h>
#include <unistd.h>

#include "Util.h"

namespace gi {
namespace {

constexpr size_t kMaxHits = 3'000'000;
constexpr size_t kChunk = 1u << 20;
constexpr size_t kPage = 4096;

int sizeOf(ValType t) { return t == ValType::Double ? 8 : 4; }

uint64_t maskOf(ValType t) { return t == ValType::Double ? ~0ull : 0xFFFFFFFFull; }

bool parseValue(ValType t, const std::string &text, uint64_t &bits) {
  const std::string s = trim(text);
  if (s.empty()) return false;
  bits = 0;
  try {
    size_t used = 0;
    if (t == ValType::Int32) {
      const long long v = std::stoll(s, &used, 0);
      const int32_t x = static_cast<int32_t>(v);
      std::memcpy(&bits, &x, 4);
    } else if (t == ValType::Float) {
      const float x = std::stof(s, &used);
      std::memcpy(&bits, &x, 4);
    } else {
      const double x = std::stod(s, &used);
      std::memcpy(&bits, &x, 8);
    }
    return used == s.size();
  } catch (...) {
    return false;
  }
}

std::string formatValue(ValType t, uint64_t bits) {
  char b[64];
  if (t == ValType::Int32) {
    int32_t x;
    std::memcpy(&x, &bits, 4);
    std::snprintf(b, sizeof b, "%d", x);
  } else if (t == ValType::Float) {
    float x;
    std::memcpy(&x, &bits, 4);
    std::snprintf(b, sizeof b, "%.5g", static_cast<double>(x));
  } else {
    double x;
    std::memcpy(&x, &bits, 8);
    std::snprintf(b, sizeof b, "%.8g", x);
  }
  return b;
}

bool numEqual(ValType t, uint64_t a, uint64_t b) {
  if (t == ValType::Int32) return static_cast<uint32_t>(a) == static_cast<uint32_t>(b);
  if (t == ValType::Float) {
    float x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    if (std::isnan(x) || std::isnan(y)) return false;
    return std::fabs(x - y) <= 1e-4f + 1e-6f * std::fabs(y);
  }
  double x, y;
  std::memcpy(&x, &a, 8);
  std::memcpy(&y, &b, 8);
  if (std::isnan(x) || std::isnan(y)) return false;
  return std::fabs(x - y) <= 1e-9 + 1e-12 * std::fabs(y);
}

bool numGreater(ValType t, uint64_t a, uint64_t b) {
  if (t == ValType::Int32) {
    int32_t x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x > y;
  }
  if (t == ValType::Float) {
    float x, y;
    std::memcpy(&x, &a, 4);
    std::memcpy(&y, &b, 4);
    return x > y;
  }
  double x, y;
  std::memcpy(&x, &a, 8);
  std::memcpy(&y, &b, 8);
  return x > y;
}

bool matches(ValType t, ScanMode m, uint64_t cur, uint64_t prev, uint64_t target) {
  const uint64_t mask = maskOf(t);
  switch (m) {
  case ScanMode::Exact: return numEqual(t, cur, target);
  case ScanMode::Changed: return (cur & mask) != (prev & mask);
  case ScanMode::Unchanged: return (cur & mask) == (prev & mask);
  case ScanMode::Increased: return numGreater(t, cur, prev);
  case ScanMode::Decreased: return numGreater(t, prev, cur);
  }
  return false;
}

struct Region {
  uintptr_t begin;
  uintptr_t end;
};

bool skipPath(const std::string &p) {
  static const char *const kBad[] = {"/dev/",  "/system/", "/apex/",  "/vendor/", "/proc/",
                                      "/sys/",  "/memfd:",  "dalvik",  ".oat",     ".odex",
                                      ".vdex",  ".art",     "[vvar",   "[vdso",    "[stack",
                                      "/product/", "/odm/",   "kgsl",    "mali",     "ashmem"};
  for (const char *b : kBad) {
    if (p.find(b) != std::string::npos) return true;
  }
  return false;
}

std::vector<Region> writableRegions() {
  std::vector<Region> out;
  std::ifstream f("/proc/self/maps");
  std::string line;
  while (std::getline(f, line)) {
    unsigned long long a = 0, b = 0;
    char perms[8] = {};
    char path[512] = {};
    const int n = std::sscanf(line.c_str(), "%llx-%llx %7s %*s %*s %*s %511[^\n]", &a, &b, perms, path);
    if (n < 3) continue;
    if (perms[0] != 'r' || perms[1] != 'w') continue;
    if (skipPath(path)) continue;
    if (b <= a || (b - a) > (768ull << 20)) continue;
    out.push_back({static_cast<uintptr_t>(a), static_cast<uintptr_t>(b)});
  }
  return out;
}

bool isWritableAddr(uintptr_t addr, size_t len) {
  std::ifstream f("/proc/self/maps");
  std::string line;
  while (std::getline(f, line)) {
    unsigned long long a = 0, b = 0;
    char perms[8] = {};
    if (std::sscanf(line.c_str(), "%llx-%llx %7s", &a, &b, perms) < 3) continue;
    if (addr >= a && addr + len <= b) return perms[0] == 'r' && perms[1] == 'w';
  }
  return false;
}

int memFd(bool write) {
  static int rd = open("/proc/self/mem", O_RDONLY | O_CLOEXEC);
  static int rw = open("/proc/self/mem", O_RDWR | O_CLOEXEC);
  return write ? rw : rd;
}

bool readRaw(uintptr_t addr, void *dst, size_t len) {
  const int fd = memFd(false);
  if (fd >= 0) {
    const ssize_t n = pread(fd, dst, len, static_cast<off_t>(addr));
    if (n == static_cast<ssize_t>(len)) return true;
  }
  return false;
}

bool writeRaw(uintptr_t addr, const void *src, size_t len) {
  const int fd = memFd(true);
  if (fd >= 0) {
    const ssize_t n = pwrite(fd, src, len, static_cast<off_t>(addr));
    if (n == static_cast<ssize_t>(len)) return true;
  }
  if (isWritableAddr(addr, len)) {
    std::memcpy(reinterpret_cast<void *>(addr), src, len);
    return true;
  }
  return false;
}

} // namespace

MemScanner &MemScanner::get() {
  static MemScanner inst;
  return inst;
}

long MemScanner::firstScan(ValType type, const std::string &value) {
  uint64_t target = 0;
  if (!parseValue(type, value, target)) return -1;

  mCancel = false;
  const int sz = sizeOf(type);
  const auto regions = writableRegions();
  mRegionsTotal = regions.size();
  mRegionsDone = 0;

  std::vector<Hit> hits;
  hits.reserve(4096);
  std::vector<uint8_t> buf(kChunk);
  const uintptr_t bufBegin = reinterpret_cast<uintptr_t>(buf.data());
  const uintptr_t bufEnd = bufBegin + buf.size();

  for (const Region &r : regions) {
    if (mCancel) break;
    for (uintptr_t off = r.begin; off < r.end && hits.size() < kMaxHits; off += kChunk) {
      if (mCancel) break;
      const size_t n = std::min<size_t>(kChunk, r.end - off);
      size_t got = 0;
      if (readRaw(off, buf.data(), n)) {
        got = n;
      } else {
        // Some pages inside a region may be unreadable: retry page by page.
        for (size_t p = 0; p < n; p += kPage) {
          const size_t pn = std::min(kPage, n - p);
          if (readRaw(off + p, buf.data() + p, pn)) {
            got = p + pn;
          } else {
            std::memset(buf.data() + p, 0xA5, pn);
            got = p + pn;
          }
        }
      }
      for (size_t i = 0; i + sz <= got; i += sz) {
        uint64_t cur = 0;
        std::memcpy(&cur, buf.data() + i, sz);
        if (!numEqual(type, cur, target)) continue;
        const uintptr_t addr = off + i;
        if (addr >= bufBegin && addr < bufEnd) continue;
        hits.push_back({addr, cur});
        if (hits.size() >= kMaxHits) break;
      }
    }
    ++mRegionsDone;
  }

  std::lock_guard<std::mutex> lock(mMutex);
  mHits = std::move(hits);
  mType = type;
  return static_cast<long>(mHits.size());
}

long MemScanner::nextScan(ScanMode mode, const std::string &value) {
  std::vector<Hit> old;
  ValType type;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    old = mHits;
    type = mType;
  }
  uint64_t target = 0;
  if (mode == ScanMode::Exact && !parseValue(type, value, target)) return -1;

  mCancel = false;
  const int sz = sizeOf(type);
  std::vector<Hit> kept;
  kept.reserve(old.size() / 4 + 16);

  uint8_t page[kPage];
  uintptr_t pageBase = ~uintptr_t{0};
  mRegionsTotal = old.size();
  mRegionsDone = 0;

  for (const Hit &h : old) {
    if (mCancel) break;
    ++mRegionsDone;
    const uintptr_t base = h.addr & ~(kPage - 1);
    if (base != pageBase) {
      pageBase = readRaw(base, page, kPage) ? base : ~uintptr_t{0};
    }
    if (pageBase != base) continue;
    uint64_t cur = 0;
    std::memcpy(&cur, page + (h.addr - base), sz);
    if (matches(type, mode, cur, h.prev, target)) kept.push_back({h.addr, cur});
  }

  std::lock_guard<std::mutex> lock(mMutex);
  mHits = std::move(kept);
  return static_cast<long>(mHits.size());
}

void MemScanner::reset() {
  std::lock_guard<std::mutex> lock(mMutex);
  mHits.clear();
  mHits.shrink_to_fit();
}

size_t MemScanner::count() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mHits.size();
}

ValType MemScanner::type() const {
  std::lock_guard<std::mutex> lock(mMutex);
  return mType;
}

bool MemScanner::addressAt(size_t index, uintptr_t &addr) const {
  std::lock_guard<std::mutex> lock(mMutex);
  if (index >= mHits.size()) return false;
  addr = mHits[index].addr;
  return true;
}

std::vector<Row> MemScanner::rows(size_t start, size_t n) const {
  std::vector<Hit> slice;
  ValType type;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    type = mType;
    for (size_t i = start; i < mHits.size() && i < start + n; ++i) slice.push_back(mHits[i]);
  }
  std::vector<Row> out;
  for (const Hit &h : slice) {
    Row r;
    r.addr = h.addr;
    uint64_t cur = 0;
    r.current = readRaw(h.addr, &cur, sizeOf(type)) ? formatValue(type, cur) : "??";
    r.previous = formatValue(type, h.prev);
    r.frozen = isFrozen(h.addr);
    out.push_back(std::move(r));
  }
  return out;
}

size_t MemScanner::dumpHits(const std::string &path, size_t maxRows) const {
  std::vector<Hit> hits;
  ValType type;
  {
    std::lock_guard<std::mutex> lock(mMutex);
    hits = mHits;
    type = mType;
  }
  std::ofstream f(path);
  if (!f) return 0;
  f << "# address  current  previous\n";
  size_t n = 0;
  for (const Hit &h : hits) {
    if (n >= maxRows) break;
    uint64_t cur = 0;
    const bool ok = readRaw(h.addr, &cur, sizeOf(type));
    f << hex(h.addr) << "  " << (ok ? formatValue(type, cur) : "??") << "  " << formatValue(type, h.prev) << "\n";
    ++n;
  }
  return n;
}

bool MemScanner::readValue(uintptr_t addr, ValType type, std::string &out) const {
  uint64_t cur = 0;
  if (!readRaw(addr, &cur, sizeOf(type))) return false;
  out = formatValue(type, cur);
  return true;
}

bool MemScanner::writeValue(uintptr_t addr, ValType type, const std::string &value) {
  uint64_t bits = 0;
  if (!parseValue(type, value, bits)) return false;
  return writeRaw(addr, &bits, sizeOf(type));
}

bool MemScanner::setFrozen(uintptr_t addr, ValType type, const std::string &value, bool on) {
  std::lock_guard<std::mutex> lock(mFrozenMutex);
  auto it = std::find_if(mFrozen.begin(), mFrozen.end(), [&](const Frozen &f) { return f.addr == addr; });
  if (!on) {
    if (it != mFrozen.end()) mFrozen.erase(it);
    return true;
  }
  uint64_t bits = 0;
  if (!parseValue(type, value, bits)) return false;
  if (it != mFrozen.end()) {
    it->type = type;
    it->bits = bits;
  } else {
    mFrozen.push_back({addr, type, bits});
  }
  return true;
}

bool MemScanner::isFrozen(uintptr_t addr) const {
  std::lock_guard<std::mutex> lock(mFrozenMutex);
  return std::any_of(mFrozen.begin(), mFrozen.end(), [&](const Frozen &f) { return f.addr == addr; });
}

void MemScanner::applyFrozen() {
  std::vector<Frozen> copy;
  {
    std::lock_guard<std::mutex> lock(mFrozenMutex);
    copy = mFrozen;
  }
  for (const Frozen &f : copy) writeRaw(f.addr, &f.bits, sizeOf(f.type));
}

void MemScanner::clearFrozen() {
  std::lock_guard<std::mutex> lock(mFrozenMutex);
  mFrozen.clear();
}

size_t MemScanner::frozenCount() const {
  std::lock_guard<std::mutex> lock(mFrozenMutex);
  return mFrozen.size();
}

std::string MemScanner::progress() const {
  const size_t total = mRegionsTotal.load();
  if (total == 0) return "";
  return std::to_string(mRegionsDone.load()) + "/" + std::to_string(total);
}

} // namespace gi
