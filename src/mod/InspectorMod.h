#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <pl/Mod.hpp>
#include <pl/ModMenu.hpp>

#include "MemScanner.h"

namespace gi {

class InspectorMod {
public:
  static InspectorMod &instance();

  InspectorMod();

  [[nodiscard]] ll::mod::NativeMod &getSelf() const { return mSelf; }

  bool load();
  bool enable();
  bool disable();
  bool unload();

private:
  struct Fav {
    int number{};
    std::string moduleId;
    std::string buttonId;
    uintptr_t addr{};
    ValType type{ValType::Float};
  };

  // menu
  bool registerScanModule();
  bool registerFuncModule();
  void onConfig(const std::string &moduleId, const std::string &key, const std::string &value);
  void onScanConfig(const std::string &key);
  void onFuncConfig(const std::string &key);

  // scanner actions
  void actionFirstScan();
  void actionNextScan();
  void actionWrite();
  void actionFreeze();
  void actionFavorite();
  void actionDump();
  size_t selectedIndex() const;
  void addFavorite(uintptr_t addr, ValType type, const std::string &value);
  void applyFavorite(const std::string &moduleId);

  // function actions
  void actionSearchSymbols();
  void actionPickSymbol();
  void actionHook();
  void actionReplay();
  void actionUnhook(bool all);
  void actionCall();
  void actionDumpSymbols();
  bool resolveTarget(const std::string &text, uintptr_t &addr, std::string &label, std::string &error);

  // overlay / threads
  void tickLoop();
  void refreshOverlays();
  std::vector<std::string> buildScanLines() const;
  std::vector<std::string> buildFuncLines() const;
  void submitPanel(const std::string &moduleId, const std::vector<std::string> &lines, float x, float y);
  void loadFont();

  // helpers
  std::string cfgGet(const std::string &module, const std::string &key, const std::string &def) const;
  int cfgInt(const std::string &module, const std::string &key, int def) const;
  void cfgSet(const std::string &module, const std::string &key, const std::string &value);
  void setStatus(const std::string &text);
  std::string status() const;
  bool runAsync(const std::string &title, std::function<void()> fn);
  void stopAll();
  std::string dataPath(const std::string &file) const;

  ll::mod::NativeMod &mSelf;

  mutable std::mutex mCfgMutex;
  std::map<std::string, std::string> mCfg;

  mutable std::mutex mStatusMutex;
  std::string mStatus{"listo"};

  std::atomic<bool> mBusy{false};
  std::thread mWorker;

  std::atomic<bool> mRunning{false};
  std::thread mTicker;

  std::string mFontId;
  bool mScanRegistered{false};
  bool mFuncRegistered{false};

  mutable std::mutex mFuncMutex;
  std::vector<size_t> mMatches;
  uintptr_t mPickedAddr{0};
  std::string mPickedLabel;

  mutable std::mutex mFavMutex;
  std::vector<Fav> mFavs;
  int mFavCounter{0};
};

} // namespace gi
