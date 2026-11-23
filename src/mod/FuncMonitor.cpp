#include "FuncMonitor.h"

#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <type_traits>
#include <utility>

#include <pl/memory/Hook.hpp>

#include "Util.h"

namespace gi {
namespace {

struct Slot {
  std::atomic<bool> used{false};
  std::atomic<uint64_t> calls{0};
  std::atomic<uint64_t> args[8]{};
  std::atomic<uint64_t> floatArgs[2]{};
  std::atomic<int> replays{0};
  void *orig{nullptr};
  uintptr_t target{0};
  RetType ret{RetType::Void};
  std::string label;
  pl::memory::HookHandle handle;
};

Slot gSlots[FuncMonitor::kSlots];
std::mutex gMutex;

#define GI_PARAMS                                                                              \
  uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4, uint64_t a5, uint64_t a6,   \
      uint64_t a7, double d0, double d1, double d2, double d3, double d4, double d5, double d6, \
      double d7
#define GI_ARGS a0, a1, a2, a3, a4, a5, a6, a7, d0, d1, d2, d3, d4, d5, d6, d7

// Every hooked function is forwarded with the full AArch64 register argument
// set (x0-x7 and d0-d7), so integer, pointer and float arguments all survive.
// Functions with more than 8 integer arguments (stack arguments) or that return
// big structs through x8 are not supported.
template <int N, typename R> R detour(GI_PARAMS) {
  Slot &s = gSlots[N];
  s.calls.fetch_add(1, std::memory_order_relaxed);
  s.args[0].store(a0, std::memory_order_relaxed);
  s.args[1].store(a1, std::memory_order_relaxed);
  s.args[2].store(a2, std::memory_order_relaxed);
  s.args[3].store(a3, std::memory_order_relaxed);
  s.args[4].store(a4, std::memory_order_relaxed);
  s.args[5].store(a5, std::memory_order_relaxed);
  s.args[6].store(a6, std::memory_order_relaxed);
  s.args[7].store(a7, std::memory_order_relaxed);
  uint64_t b0, b1;
  std::memcpy(&b0, &d0, 8);
  std::memcpy(&b1, &d1, 8);
  s.floatArgs[0].store(b0, std::memory_order_relaxed);
  s.floatArgs[1].store(b1, std::memory_order_relaxed);

  using Fn = R (*)(GI_PARAMS);
  const auto orig = reinterpret_cast<Fn>(s.orig);

  int extra = s.replays.exchange(0, std::memory_order_acq_rel);
  while (extra-- > 0) orig(GI_ARGS);

  if constexpr (std::is_void_v<R>) {
    orig(GI_ARGS);
  } else {
    return orig(GI_ARGS);
  }
}

template <size_t... I> std::array<std::array<void *, 4>, sizeof...(I)> buildTable(std::index_sequence<I...>) {
  return {{std::array<void *, 4>{reinterpret_cast<void *>(&detour<static_cast<int>(I), void>),
                                 reinterpret_cast<void *>(&detour<static_cast<int>(I), uint64_t>),
                                 reinterpret_cast<void *>(&detour<static_cast<int>(I), float>),
                                 reinterpret_cast<void *>(&detour<static_cast<int>(I), double>)}...}};
}

const auto &table() {
  static const auto t = buildTable(std::make_index_sequence<FuncMonitor::kSlots>{});
  return t;
}

} // namespace

FuncMonitor &FuncMonitor::get() {
  static FuncMonitor inst;
  return inst;
}

int FuncMonitor::install(uintptr_t target, RetType ret, const std::string &label, std::string &error) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (target == 0) {
    error = "direccion invalida";
    return -1;
  }
  int free = -1;
  for (int i = 0; i < kSlots; ++i) {
    if (gSlots[i].used && gSlots[i].target == target) {
      error = "ya esta vigilada (slot " + std::to_string(i) + ")";
      return -1;
    }
    if (!gSlots[i].used && free < 0) free = i;
  }
  if (free < 0) {
    error = "no quedan slots (max 32)";
    return -1;
  }

  Slot &s = gSlots[free];
  s.calls = 0;
  for (auto &a : s.args) a = 0;
  for (auto &a : s.floatArgs) a = 0;
  s.replays = 0;
  s.orig = nullptr;
  s.target = target;
  s.ret = ret;
  s.label = label;
  s.handle = pl::memory::HookHandle(reinterpret_cast<void *>(target), table()[free][static_cast<int>(ret)], &s.orig);
  if (!s.handle.installed() || s.orig == nullptr) {
    s.handle.reset();
    error = "no se pudo instalar el hook";
    return -1;
  }
  s.used = true;
  return free;
}

void FuncMonitor::remove(int slot) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (slot < 0 || slot >= kSlots || !gSlots[slot].used) return;
  gSlots[slot].handle.reset();
  gSlots[slot].used = false;
}

void FuncMonitor::removeAll() {
  std::lock_guard<std::mutex> lock(gMutex);
  for (Slot &s : gSlots) {
    if (!s.used) continue;
    s.handle.reset();
    s.used = false;
  }
}

static void fill(int i, MonitorInfo &m) {
  const Slot &s = gSlots[i];
  m.slot = i;
  m.label = s.label;
  m.target = s.target;
  m.ret = s.ret;
  m.calls = s.calls.load(std::memory_order_relaxed);
  for (int k = 0; k < 8; ++k) m.args[k] = s.args[k].load(std::memory_order_relaxed);
  for (int k = 0; k < 2; ++k) m.floatArgs[k] = s.floatArgs[k].load(std::memory_order_relaxed);
  m.pendingReplays = s.replays.load(std::memory_order_relaxed);
}

std::vector<MonitorInfo> FuncMonitor::snapshot() const {
  std::lock_guard<std::mutex> lock(gMutex);
  std::vector<MonitorInfo> out;
  for (int i = 0; i < kSlots; ++i) {
    if (!gSlots[i].used) continue;
    MonitorInfo m;
    fill(i, m);
    out.push_back(std::move(m));
  }
  return out;
}

bool FuncMonitor::snapshotSlot(int slot, MonitorInfo &out) const {
  std::lock_guard<std::mutex> lock(gMutex);
  if (slot < 0 || slot >= kSlots || !gSlots[slot].used) return false;
  fill(slot, out);
  return true;
}

bool FuncMonitor::requestReplay(int slot, int times) {
  std::lock_guard<std::mutex> lock(gMutex);
  if (slot < 0 || slot >= kSlots || !gSlots[slot].used) return false;
  gSlots[slot].replays.fetch_add(times, std::memory_order_acq_rel);
  return true;
}

std::string FuncMonitor::callDirect(uintptr_t fn, const uint64_t a[8], RetType ret) {
  if (fn == 0) return "direccion invalida";
  char buf[64];
  switch (ret) {
  case RetType::Void: {
    using F = void (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    reinterpret_cast<F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    return "ok (void)";
  }
  case RetType::Integer: {
    using F = uint64_t (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const uint64_t r = reinterpret_cast<F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    std::snprintf(buf, sizeof buf, "ret=%s (%lld)", hex(r).c_str(), static_cast<long long>(r));
    return buf;
  }
  case RetType::Float: {
    using F = float (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const float r = reinterpret_cast<F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    std::snprintf(buf, sizeof buf, "ret=%g", static_cast<double>(r));
    return buf;
  }
  case RetType::Double: {
    using F = double (*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
    const double r = reinterpret_cast<F>(fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]);
    std::snprintf(buf, sizeof buf, "ret=%g", r);
    return buf;
  }
  }
  return "?";
}

} // namespace gi
