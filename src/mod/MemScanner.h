#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

namespace gi {

enum class ValType { Int32 = 0, Float = 1, Double = 2 };
enum class ScanMode { Exact = 0, Changed = 1, Unchanged = 2, Increased = 3, Decreased = 4 };

struct Row {
  uintptr_t addr{};
  std::string current;
  std::string previous;
  bool frozen{};
};

// In-process "Cheat Engine" style scanner. The game and this mod share one
// address space, so we only read/write our own memory through /proc/self/mem.
class MemScanner {
public:
  static MemScanner &get();

  // Returns number of hits, or -1 when the value text is invalid.
  long firstScan(ValType type, const std::string &value);
  // Filters the current hit list. `value` is only used by ScanMode::Exact.
  long nextScan(ScanMode mode, const std::string &value);
  void reset();
  void cancel() { mCancel = true; }

  size_t count() const;
  ValType type() const;
  std::vector<Row> rows(size_t start, size_t n) const;
  bool addressAt(size_t index, uintptr_t &addr) const;
  size_t dumpHits(const std::string &path, size_t maxRows) const;

  bool readValue(uintptr_t addr, ValType type, std::string &out) const;
  bool writeValue(uintptr_t addr, ValType type, const std::string &value);

  // Freeze: keep re-writing `value` at `addr` (call applyFrozen() from a ticker).
  bool setFrozen(uintptr_t addr, ValType type, const std::string &value, bool on);
  bool isFrozen(uintptr_t addr) const;
  void applyFrozen();
  void clearFrozen();
  size_t frozenCount() const;

  std::string progress() const;

private:
  MemScanner() = default;

  struct Hit {
    uintptr_t addr;
    uint64_t prev;
  };
  struct Frozen {
    uintptr_t addr;
    ValType type;
    uint64_t bits;
  };

  mutable std::mutex mMutex;
  std::vector<Hit> mHits;
  ValType mType{ValType::Float};

  mutable std::mutex mFrozenMutex;
  std::vector<Frozen> mFrozen;

  std::atomic<bool> mCancel{false};
  std::atomic<size_t> mRegionsDone{0};
  std::atomic<size_t> mRegionsTotal{0};
};

} // namespace gi
