#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gi {

// What the hooked function returns. The detour must forward the right
// register, otherwise the game would read a wrong return value.
enum class RetType { Void = 0, Integer = 1, Float = 2, Double = 3 };

struct MonitorInfo {
  int slot{};
  std::string label;
  uintptr_t target{};
  RetType ret{};
  uint64_t calls{};
  uint64_t args[8]{};
  uint64_t floatArgs[2]{};
  int pendingReplays{};
};

// Counts calls of chosen game functions and can re-run them (with the last
// captured arguments) from inside the game's own thread.
class FuncMonitor {
public:
  static constexpr int kSlots = 32;

  static FuncMonitor &get();

  // Returns the slot used, or -1 on failure (message in `error`).
  int install(uintptr_t target, RetType ret, const std::string &label, std::string &error);
  void remove(int slot);
  void removeAll();

  std::vector<MonitorInfo> snapshot() const;
  bool snapshotSlot(int slot, MonitorInfo &out) const;

  // Next time the game calls this function, run it `times` extra times
  // with the same arguments (safe: it happens on the game thread).
  bool requestReplay(int slot, int times);

  // Direct call with explicit integer arguments (advanced: may crash the game
  // when the function expects a valid `this` or runs on another thread).
  static std::string callDirect(uintptr_t fn, const uint64_t args[8], RetType ret);
};

} // namespace gi
