#include "InspectorMod.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <pl/memory/Signature.hpp>

#include "FuncMonitor.h"
#include "Symbols.h"
#include "Util.h"

namespace gi {
namespace {

constexpr const char *kScan = "gi.scan";
constexpr const char *kFunc = "gi.func";
constexpr const char *kFavPrefix = "gi.fav.";
constexpr const char *kModule = "libminecraftpe.so";
constexpr uint32_t kWhite = 0xFFFFFFFFu;
constexpr uint32_t kPanel = 0xB0000000u;
constexpr size_t kRowsShown = 10;

const char *typeName(ValType t) {
  switch (t) {
  case ValType::Int32: return "int";
  case ValType::Float: return "float";
  case ValType::Double: return "double";
  }
  return "?";
}

const char *modeName(int m) {
  static const char *const n[] = {"igual", "cambio", "sin cambio", "subio", "bajo"};
  return n[std::clamp(m, 0, 4)];
}

const char *retName(RetType r) {
  switch (r) {
  case RetType::Void: return "void";
  case RetType::Integer: return "int/ptr";
  case RetType::Float: return "float";
  case RetType::Double: return "double";
  }
  return "?";
}

bool isPress(const std::string &value) { return value != "0" && value != "false"; }

std::string floatFromBits(uint64_t bits) {
  double d;
  std::memcpy(&d, &bits, 8);
  char b[32];
  std::snprintf(b, sizeof b, "%.4g", d);
  return b;
}

} // namespace

InspectorMod &InspectorMod::instance() {
  static InspectorMod inst;
  return inst;
}

InspectorMod::InspectorMod() : mSelf(*ll::mod::NativeMod::current()) {}

// ---------------------------------------------------------------- lifecycle

bool InspectorMod::load() {
  mSelf.getLogger().info("Game Inspector cargado");
  std::error_code ec;
  std::filesystem::create_directories(mSelf.getDataDir(), ec);
  return true;
}

bool InspectorMod::enable() {
  try {
    loadFont();
    Symbols::get().load();
    mScanRegistered = registerScanModule();
    mFuncRegistered = registerFuncModule();
    if (!mScanRegistered || !mFuncRegistered) {
      mSelf.getLogger().warn("No se pudo registrar algun modulo del menu");
    }
    mRunning = true;
    mTicker = std::thread([this] { tickLoop(); });
    mSelf.getLogger().info("Game Inspector activo ({} simbolos)", Symbols::get().size());
  } catch (const std::exception &e) {
    mSelf.getLogger().error("enable fallo: {}", e.what());
    return false;
  }
  return true;
}

void InspectorMod::stopAll() {
  mRunning = false;
  MemScanner::get().cancel();
  if (mTicker.joinable()) mTicker.join();
  if (mWorker.joinable()) mWorker.join();
  mBusy = false;

  FuncMonitor::get().removeAll();
  MemScanner::get().clearFrozen();

  {
    std::lock_guard<std::mutex> lock(mFavMutex);
    for (const Fav &f : mFavs) {
      pl::modmenu::unregisterButton(f.buttonId);
      pl::modmenu::unregisterModule(f.moduleId);
    }
    mFavs.clear();
  }
  if (mScanRegistered) pl::modmenu::unregisterModule(kScan);
  if (mFuncRegistered) pl::modmenu::unregisterModule(kFunc);
  mScanRegistered = mFuncRegistered = false;
}

bool InspectorMod::disable() {
  stopAll();
  return true;
}

bool InspectorMod::unload() {
  stopAll();
  return true;
}

// ----------------------------------------------------------------- helpers

std::string InspectorMod::cfgGet(const std::string &module, const std::string &key, const std::string &def) const {
  std::lock_guard<std::mutex> lock(mCfgMutex);
  const auto it = mCfg.find(module + "/" + key);
  return it == mCfg.end() ? def : it->second;
}

int InspectorMod::cfgInt(const std::string &module, const std::string &key, int def) const {
  const std::string v = cfgGet(module, key, std::to_string(def));
  try {
    return static_cast<int>(std::stod(v));
  } catch (...) {
    return def;
  }
}

void InspectorMod::cfgSet(const std::string &module, const std::string &key, const std::string &value) {
  std::lock_guard<std::mutex> lock(mCfgMutex);
  mCfg[module + "/" + key] = value;
}

void InspectorMod::setStatus(const std::string &text) {
  std::lock_guard<std::mutex> lock(mStatusMutex);
  mStatus = text;
  mSelf.getLogger().info("{}", text);
}

std::string InspectorMod::status() const {
  std::lock_guard<std::mutex> lock(mStatusMutex);
  return mStatus;
}

bool InspectorMod::runAsync(const std::string &title, std::function<void()> fn) {
  bool expected = false;
  if (!mBusy.compare_exchange_strong(expected, true)) {
    setStatus("ocupado, espera a que termine la tarea anterior");
    return false;
  }
  if (mWorker.joinable()) mWorker.join();
  setStatus(title + "...");
  mWorker = std::thread([this, fn = std::move(fn)] {
    try {
      fn();
    } catch (const std::exception &e) {
      setStatus(std::string("error: ") + e.what());
    } catch (...) {
      setStatus("error desconocido");
    }
    mBusy = false;
  });
  return true;
}

std::string InspectorMod::dataPath(const std::string &file) const {
  std::error_code ec;
  std::filesystem::create_directories(mSelf.getDataDir(), ec);
  return (mSelf.getDataDir() / file).string();
}

void InspectorMod::loadFont() {
  static const char *const kFonts[] = {"/system/fonts/Roboto-Regular.ttf", "/system/fonts/RobotoStatic-Regular.ttf",
                                       "/system/fonts/NotoSans-Regular.ttf", "/system/fonts/DroidSans.ttf"};
  for (const char *path : kFonts) {
    std::ifstream f(path, std::ios::binary);
    if (!f) continue;
    std::vector<unsigned char> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (data.empty()) continue;
    if (pl::modmenu::registerFont("gi.font", data)) {
      mFontId = "gi.font";
      return;
    }
  }
  mSelf.getLogger().warn("No se encontro una fuente del sistema; el panel puede no mostrar texto");
}

// -------------------------------------------------------------------- menu

bool InspectorMod::registerScanModule() {
  using pl::modmenu::ConfigType;
  return pl::modmenu::ModuleBuilder(kScan, "Inspector: escaner de valores")
      .modId(mSelf.getId())
      .description("Busca valores en la memoria del juego (como Cheat Engine), cambialos o congelalos.")
      .defaultEnabled(true)
      .onConfigChanged([this](std::string_view m, std::string_view k, std::string_view v) {
        onConfig(std::string(m), std::string(k), std::string(v));
      })
      .config("valueText", "1) Valor a buscar", ConfigType::Text, "1.8")
      .config("valueType", "1) Tipo: 0=int 1=float 2=double", ConfigType::SliderInt, "1", "0", "2")
      .config("btnFirst", "1) PRIMERA BUSQUEDA", ConfigType::Button)
      .config("filterMode", "2) Filtro: 0=igual 1=cambio 2=sin cambio 3=subio 4=bajo", ConfigType::SliderInt, "0",
              "0", "4")
      .config("filterValue", "2) Valor para filtro 'igual'", ConfigType::Text, "1.5")
      .config("btnNext", "2) FILTRAR RESULTADOS", ConfigType::Button)
      .config("btnReset", "Reiniciar busqueda", ConfigType::Button)
      .config("btnCancel", "Cancelar tarea en curso", ConfigType::Button)
      .config("resIndex", "3) Resultado seleccionado (0-999)", ConfigType::SliderInt, "0", "0", "999")
      .config("resBlock", "3) Bloque de 1000 resultados", ConfigType::SliderInt, "0", "0", "300")
      .config("newValue", "4) Nuevo valor", ConfigType::Text, "1.0")
      .config("btnWrite", "4) ESCRIBIR valor en el resultado", ConfigType::Button)
      .config("btnFreeze", "4) CONGELAR / descongelar resultado", ConfigType::Button)
      .config("btnFav", "5) Guardar resultado como boton propio", ConfigType::Button)
      .config("btnDump", "Volcar resultados a archivo", ConfigType::Button)
      .registerModule();
}

bool InspectorMod::registerFuncModule() {
  using pl::modmenu::ConfigType;
  return pl::modmenu::ModuleBuilder(kFunc, "Inspector: funciones del juego")
      .modId(mSelf.getId())
      .description("Busca funciones de libminecraftpe.so, vigila cuantas veces se ejecutan y vuelve a ejecutarlas.")
      .defaultEnabled(true)
      .onConfigChanged([this](std::string_view m, std::string_view k, std::string_view v) {
        onConfig(std::string(m), std::string(k), std::string(v));
      })
      .config("searchText", "1) Buscar funcion (ej: sneak, Player, Camera)", ConfigType::Text, "")
      .config("btnSearch", "1) BUSCAR en los simbolos", ConfigType::Button)
      .config("matchIndex", "1) Coincidencia seleccionada (0-199)", ConfigType::SliderInt, "0", "0", "199")
      .config("btnPick", "1) Usar la coincidencia seleccionada", ConfigType::Button)
      .config("targetText", "2) Objetivo manual: nombre, +0xOFFSET o firma (vacio = coincidencia)", ConfigType::Text,
              "")
      .config("retType", "2) Devuelve: 0=void 1=int/puntero 2=float 3=double", ConfigType::SliderInt, "0", "0", "3")
      .config("btnHook", "2) VIGILAR funcion (cuenta llamadas y args)", ConfigType::Button)
      .config("monSlot", "3) Funcion vigilada (slot 0-31)", ConfigType::SliderInt, "0", "0", "31")
      .config("replayTimes", "3) Veces a re-ejecutar", ConfigType::SliderInt, "1", "1", "20")
      .config("btnReplay", "3) RE-EJECUTAR en el proximo hit del juego", ConfigType::Button)
      .config("btnUnhook", "3) Dejar de vigilar este slot", ConfigType::Button)
      .config("btnUnhookAll", "Dejar de vigilar todo", ConfigType::Button)
      .config("argsText", "4) Args para llamada directa (hasta 8)", ConfigType::Text, "0 0 0 0")
      .config("btnCall", "4) LLAMAR funcion directo (avanzado, puede cerrar el juego)", ConfigType::Button)
      .config("btnDumpSyms", "Volcar simbolos a archivo (para Ghidra)", ConfigType::Button)
      .registerModule();
}

void InspectorMod::onConfig(const std::string &moduleId, const std::string &key, const std::string &value) {
  if (moduleId.rfind(kFavPrefix, 0) == 0) {
    if (key == "value") {
      cfgSet(moduleId, key, value);
      Fav fav;
      bool found = false;
      {
        std::lock_guard<std::mutex> lock(mFavMutex);
        for (const Fav &f : mFavs) {
          if (f.moduleId == moduleId) {
            fav = f;
            found = true;
          }
        }
      }
      if (found && MemScanner::get().isFrozen(fav.addr)) MemScanner::get().setFrozen(fav.addr, fav.type, value, true);
    } else if (key == "apply" && isPress(value)) {
      applyFavorite(moduleId);
    }
    return;
  }

  cfgSet(moduleId, key, value);
  if (key.rfind("btn", 0) != 0 || !isPress(value)) return;
  if (moduleId == kScan) onScanConfig(key);
  else if (moduleId == kFunc) onFuncConfig(key);
}

void InspectorMod::onScanConfig(const std::string &key) {
  if (key == "btnFirst") actionFirstScan();
  else if (key == "btnNext") actionNextScan();
  else if (key == "btnReset") {
    MemScanner::get().reset();
    setStatus("busqueda reiniciada");
  } else if (key == "btnCancel") {
    MemScanner::get().cancel();
    setStatus("cancelando...");
  } else if (key == "btnWrite") actionWrite();
  else if (key == "btnFreeze") actionFreeze();
  else if (key == "btnFav") actionFavorite();
  else if (key == "btnDump") actionDump();
}

void InspectorMod::onFuncConfig(const std::string &key) {
  if (key == "btnSearch") actionSearchSymbols();
  else if (key == "btnPick") actionPickSymbol();
  else if (key == "btnHook") actionHook();
  else if (key == "btnReplay") actionReplay();
  else if (key == "btnUnhook") actionUnhook(false);
  else if (key == "btnUnhookAll") actionUnhook(true);
  else if (key == "btnCall") actionCall();
  else if (key == "btnDumpSyms") actionDumpSymbols();
}

// ------------------------------------------------------------ scanner actions

size_t InspectorMod::selectedIndex() const {
  const int block = std::max(0, cfgInt(kScan, "resBlock", 0));
  const int idx = std::max(0, cfgInt(kScan, "resIndex", 0));
  return static_cast<size_t>(block) * 1000 + static_cast<size_t>(idx);
}

void InspectorMod::actionFirstScan() {
  const auto type = static_cast<ValType>(std::clamp(cfgInt(kScan, "valueType", 1), 0, 2));
  const std::string value = cfgGet(kScan, "valueText", "1.8");
  runAsync("Primera busqueda de " + value + " (" + typeName(type) + ")", [this, type, value] {
    const long n = MemScanner::get().firstScan(type, value);
    if (n < 0) setStatus("valor invalido para el tipo " + std::string(typeName(type)));
    else setStatus(std::to_string(n) + " resultados. Cambia algo en el juego y pulsa FILTRAR");
  });
}

void InspectorMod::actionNextScan() {
  const int modeInt = std::clamp(cfgInt(kScan, "filterMode", 0), 0, 4);
  const std::string value = cfgGet(kScan, "filterValue", "1.5");
  runAsync(std::string("Filtrando: ") + modeName(modeInt), [this, modeInt, value] {
    const long n = MemScanner::get().nextScan(static_cast<ScanMode>(modeInt), value);
    if (n < 0) setStatus("valor invalido para el filtro");
    else setStatus(std::to_string(n) + " resultados restantes");
  });
}

void InspectorMod::actionWrite() {
  uintptr_t addr = 0;
  if (!MemScanner::get().addressAt(selectedIndex(), addr)) {
    setStatus("no hay resultado seleccionado");
    return;
  }
  const std::string v = cfgGet(kScan, "newValue", "1.0");
  const bool ok = MemScanner::get().writeValue(addr, MemScanner::get().type(), v);
  setStatus(ok ? "escrito " + v + " en " + hex(addr) : "no se pudo escribir en " + hex(addr));
}

void InspectorMod::actionFreeze() {
  uintptr_t addr = 0;
  if (!MemScanner::get().addressAt(selectedIndex(), addr)) {
    setStatus("no hay resultado seleccionado");
    return;
  }
  auto &sc = MemScanner::get();
  if (sc.isFrozen(addr)) {
    sc.setFrozen(addr, sc.type(), "", false);
    setStatus("descongelado " + hex(addr));
    return;
  }
  const std::string v = cfgGet(kScan, "newValue", "1.0");
  setStatus(sc.setFrozen(addr, sc.type(), v, true) ? "congelado " + hex(addr) + " en " + v : "valor invalido");
}

void InspectorMod::actionFavorite() {
  uintptr_t addr = 0;
  if (!MemScanner::get().addressAt(selectedIndex(), addr)) {
    setStatus("no hay resultado seleccionado");
    return;
  }
  addFavorite(addr, MemScanner::get().type(), cfgGet(kScan, "newValue", "1.0"));
}

void InspectorMod::actionDump() {
  const std::string path = dataPath("resultados.txt");
  const size_t n = MemScanner::get().dumpHits(path, 20000);
  setStatus("volcados " + std::to_string(n) + " resultados en " + path);
}

void InspectorMod::addFavorite(uintptr_t addr, ValType type, const std::string &value) {
  Fav fav;
  {
    std::lock_guard<std::mutex> lock(mFavMutex);
    if (mFavs.size() >= 12) {
      setStatus("maximo 12 botones propios");
      return;
    }
    fav.number = ++mFavCounter;
    fav.moduleId = std::string(kFavPrefix) + std::to_string(fav.number);
    fav.buttonId = "gi.favbtn." + std::to_string(fav.number);
    fav.addr = addr;
    fav.type = type;
  }
  cfgSet(fav.moduleId, "value", value);

  using pl::modmenu::ConfigType;
  const std::string name = "Valor " + std::to_string(fav.number) + " @" + hex(addr) + " (" + typeName(type) + ")";
  const std::string moduleId = fav.moduleId;
  const bool moduleOk =
      pl::modmenu::ModuleBuilder(fav.moduleId, name)
          .modId(mSelf.getId())
          .description("Activa el modulo para congelar el valor; el boton 'Aplicar' lo escribe una vez.")
          .defaultEnabled(false)
          .onToggle([this](std::string_view m, bool enabled) {
            const std::string id(m);
            Fav f;
            bool found = false;
            {
              std::lock_guard<std::mutex> lock(mFavMutex);
              for (const Fav &x : mFavs) {
                if (x.moduleId == id) {
                  f = x;
                  found = true;
                }
              }
            }
            if (!found) return;
            if (enabled) MemScanner::get().setFrozen(f.addr, f.type, cfgGet(id, "value", "0"), true);
            else MemScanner::get().setFrozen(f.addr, f.type, "", false);
          })
          .onConfigChanged([this](std::string_view m, std::string_view k, std::string_view v) {
            onConfig(std::string(m), std::string(k), std::string(v));
          })
          .config("value", "Valor", ConfigType::Text, value)
          .config("apply", "Aplicar ahora", ConfigType::Button)
          .registerModule();

  const bool buttonOk =
      pl::modmenu::ButtonBuilder(fav.buttonId, "Aplicar valor " + std::to_string(fav.number))
          .modId(mSelf.getId())
          .moduleId(fav.moduleId)
          .label("V" + std::to_string(fav.number))
          .androidKeyCode(0)
          .behavior(pl::modmenu::ButtonBehavior::Click)
          .onEvent([this, moduleId](std::string_view, pl::modmenu::ButtonEvent event, float) {
            if (event == pl::modmenu::ButtonEvent::Click) applyFavorite(moduleId);
          })
          .registerButton();

  {
    std::lock_guard<std::mutex> lock(mFavMutex);
    mFavs.push_back(fav);
  }
  setStatus(std::string("boton propio V") + std::to_string(fav.number) + (moduleOk ? "" : " (modulo fallo)") +
            (buttonOk ? "" : " (boton flotante fallo)"));
}

void InspectorMod::applyFavorite(const std::string &moduleId) {
  Fav fav;
  bool found = false;
  {
    std::lock_guard<std::mutex> lock(mFavMutex);
    for (const Fav &f : mFavs) {
      if (f.moduleId == moduleId) {
        fav = f;
        found = true;
      }
    }
  }
  if (!found) return;
  const std::string v = cfgGet(moduleId, "value", "0");
  const bool ok = MemScanner::get().writeValue(fav.addr, fav.type, v);
  setStatus(ok ? "V" + std::to_string(fav.number) + " = " + v : "no se pudo escribir V" + std::to_string(fav.number));
}

// ----------------------------------------------------------- function actions

bool InspectorMod::resolveTarget(const std::string &text, uintptr_t &addr, std::string &label, std::string &error) {
  Symbols &syms = Symbols::get();
  syms.load();
  const std::string t = trim(text);

  if (t.empty()) {
    std::lock_guard<std::mutex> lock(mFuncMutex);
    if (mPickedAddr == 0) {
      error = "escribe un objetivo o elige una coincidencia";
      return false;
    }
    addr = mPickedAddr;
    label = mPickedLabel;
    return true;
  }

  if (t[0] == '+' || (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'X'))) {
    const uint64_t v = std::strtoull(t[0] == '+' ? t.c_str() + 1 : t.c_str(), nullptr, 16);
    if (t[0] == '+' || v < 0x40000000ull) {
      if (syms.base() == 0) {
        error = "no se encontro libminecraftpe.so";
        return false;
      }
      addr = syms.base() + static_cast<uintptr_t>(v);
      label = "+" + hex(v);
    } else {
      addr = static_cast<uintptr_t>(v);
      label = hex(v);
    }
    return true;
  }

  if (t.find(' ') != std::string::npos || t.find('?') != std::string::npos) {
    addr = pl::memory::resolveSignature(t, kModule);
    if (addr == 0) {
      error = "firma no encontrada";
      return false;
    }
    label = "firma@" + hex(addr - syms.base());
    return true;
  }

  addr = syms.resolveName(t);
  if (addr == 0) {
    error = "simbolo no encontrado: " + cut(t, 40);
    return false;
  }
  label = cut(Symbols::demangle(t), 60);
  return true;
}

void InspectorMod::actionSearchSymbols() {
  Symbols &syms = Symbols::get();
  syms.load();
  if (syms.size() == 0) {
    setStatus("libminecraftpe.so no exporta simbolos: usa +0xOFFSET (Ghidra) o una firma de bytes");
    return;
  }
  auto found = syms.search(cfgGet(kFunc, "searchText", ""), 200);
  const size_t n = found.size();
  {
    std::lock_guard<std::mutex> lock(mFuncMutex);
    mMatches = std::move(found);
  }
  setStatus(std::to_string(n) + " coincidencias (mostrando hasta 200)");
}

void InspectorMod::actionPickSymbol() {
  const size_t idx = static_cast<size_t>(std::max(0, cfgInt(kFunc, "matchIndex", 0)));
  std::lock_guard<std::mutex> lock(mFuncMutex);
  if (idx >= mMatches.size()) {
    setStatus("no hay esa coincidencia");
    return;
  }
  const Sym &s = Symbols::get().at(mMatches[idx]);
  mPickedAddr = Symbols::get().base() + s.offset;
  mPickedLabel = cut(Symbols::demangle(s.name), 60);
  setStatus("objetivo: " + mPickedLabel);
}

void InspectorMod::actionHook() {
  uintptr_t addr = 0;
  std::string label, error;
  if (!resolveTarget(cfgGet(kFunc, "targetText", ""), addr, label, error)) {
    setStatus(error);
    return;
  }
  const auto ret = static_cast<RetType>(std::clamp(cfgInt(kFunc, "retType", 0), 0, 3));
  const int slot = FuncMonitor::get().install(addr, ret, label, error);
  if (slot < 0) setStatus("no se pudo vigilar: " + error);
  else setStatus("vigilando [" + std::to_string(slot) + "] " + label + " (devuelve " + retName(ret) + ")");
}

void InspectorMod::actionReplay() {
  const int slot = cfgInt(kFunc, "monSlot", 0);
  const int times = std::max(1, cfgInt(kFunc, "replayTimes", 1));
  if (FuncMonitor::get().requestReplay(slot, times)) {
    setStatus("se re-ejecutara " + std::to_string(times) + "x en el proximo uso de [" + std::to_string(slot) + "]");
  } else {
    setStatus("ese slot no esta vigilando nada");
  }
}

void InspectorMod::actionUnhook(bool all) {
  if (all) {
    FuncMonitor::get().removeAll();
    setStatus("ya no se vigila ninguna funcion");
  } else {
    const int slot = cfgInt(kFunc, "monSlot", 0);
    FuncMonitor::get().remove(slot);
    setStatus("slot " + std::to_string(slot) + " liberado");
  }
}

void InspectorMod::actionCall() {
  uintptr_t addr = 0;
  std::string label, error;
  if (!resolveTarget(cfgGet(kFunc, "targetText", ""), addr, label, error)) {
    setStatus(error);
    return;
  }
  uint64_t args[8] = {};
  std::string text = cfgGet(kFunc, "argsText", "");
  std::replace(text.begin(), text.end(), ',', ' ');
  std::istringstream in(text);
  std::string token;
  for (int i = 0; i < 8 && (in >> token); ++i) {
    if (!parseU64(token, args[i])) {
      setStatus("argumento invalido: " + cut(token, 20));
      return;
    }
  }
  const auto ret = static_cast<RetType>(std::clamp(cfgInt(kFunc, "retType", 0), 0, 3));
  runAsync("Llamando " + label, [this, addr, args, ret, label] {
    uint64_t copy[8];
    std::copy(std::begin(args), std::end(args), copy);
    setStatus(label + ": " + FuncMonitor::callDirect(addr, copy, ret));
  });
}

void InspectorMod::actionDumpSymbols() {
  Symbols::get().load();
  const std::string path = dataPath("simbolos.txt");
  const size_t n = Symbols::get().dump(path);
  setStatus(n == 0 ? "no hay simbolos exportados para volcar" : "volcados " + std::to_string(n) + " simbolos en " + path);
}

// ------------------------------------------------------------------ overlay

std::vector<std::string> InspectorMod::buildScanLines() const {
  auto &sc = MemScanner::get();
  std::vector<std::string> lines;
  lines.push_back("[ESCANER] tipo=" + std::string(typeName(sc.type())) + "  resultados=" + std::to_string(sc.count()) +
                  "  congelados=" + std::to_string(sc.frozenCount()));
  std::string st = status();
  if (mBusy) st += "  [" + sc.progress() + "]";
  lines.push_back("estado: " + cut(st, 70));

  const size_t sel = selectedIndex();
  const auto rows = sc.rows(sel, kRowsShown);
  if (rows.empty()) lines.push_back("(sin resultados en esta posicion)");
  for (size_t i = 0; i < rows.size(); ++i) {
    const Row &r = rows[i];
    lines.push_back(std::string(i == 0 ? "> #" : "  #") + std::to_string(sel + i) + "  " + hex(r.addr) + "  = " +
                    r.current + "  (antes " + r.previous + ")" + (r.frozen ? "  [CONGELADO]" : ""));
  }
  return lines;
}

std::vector<std::string> InspectorMod::buildFuncLines() const {
  Symbols &syms = Symbols::get();
  std::vector<std::string> lines;
  lines.push_back("[FUNCIONES] base=" + hex(syms.base()) + "  simbolos=" + std::to_string(syms.size()));

  {
    std::lock_guard<std::mutex> lock(mFuncMutex);
    if (mPickedAddr != 0) lines.push_back("objetivo: " + mPickedLabel);
    const size_t first = static_cast<size_t>(std::max(0, cfgInt(kFunc, "matchIndex", 0)));
    for (size_t i = 0; i < kRowsShown && first + i < mMatches.size(); ++i) {
      const Sym &s = syms.at(mMatches[first + i]);
      lines.push_back(std::string(i == 0 ? "> #" : "  #") + std::to_string(first + i) + "  +" + hex(s.offset) + "  " +
                      cut(Symbols::demangle(s.name), 56));
    }
  }

  const int selSlot = cfgInt(kFunc, "monSlot", 0);
  const auto mons = FuncMonitor::get().snapshot();
  if (mons.empty()) lines.push_back("(ninguna funcion vigilada)");
  for (const MonitorInfo &m : mons) {
    std::string line = std::string(m.slot == selSlot ? "* [" : "  [") + std::to_string(m.slot) + "] " + cut(m.label, 34) +
                       "  llamadas=" + std::to_string(m.calls) + "  x0=" + hex(m.args[0]) + " x1=" + hex(m.args[1]) +
                       " x2=" + hex(m.args[2]) + "  d0=" + floatFromBits(m.floatArgs[0]);
    if (m.pendingReplays > 0) line += "  (replay x" + std::to_string(m.pendingReplays) + ")";
    lines.push_back(std::move(line));
  }
  return lines;
}

void InspectorMod::submitPanel(const std::string &moduleId, const std::vector<std::string> &lines, float x, float y) {
  constexpr float kSize = 13.0f;
  constexpr float kLine = 16.0f;
  size_t longest = 0;
  for (const auto &l : lines) longest = std::max(longest, l.size());

  std::vector<pl::modmenu::DrawCommand> commands;
  commands.push_back({.type = pl::modmenu::DrawCommandType::RectFilled,
                      .x = x - 6.0f,
                      .y = y - 4.0f,
                      .w = static_cast<float>(longest) * kSize * 0.55f + 12.0f,
                      .h = static_cast<float>(lines.size()) * kLine + 8.0f,
                      .color = kPanel});
  float cy = y;
  for (const auto &l : lines) {
    commands.push_back({.type = pl::modmenu::DrawCommandType::Text,
                        .x = x,
                        .y = cy,
                        .color = kWhite,
                        .size = kSize,
                        .text = l,
                        .fontId = mFontId});
    cy += kLine;
  }
  pl::modmenu::submitDrawCommands(moduleId, commands);
}

void InspectorMod::refreshOverlays() {
  const auto surface = pl::modmenu::getHudSurfaceSize();
  const float width = std::max(surface.width, 900.0f);
  if (mScanRegistered) submitPanel(kScan, buildScanLines(), 12.0f, 70.0f);
  if (mFuncRegistered) submitPanel(kFunc, buildFuncLines(), width * 0.5f, 70.0f);
}

void InspectorMod::tickLoop() {
  int counter = 0;
  while (mRunning) {
    MemScanner::get().applyFrozen();
    if (++counter >= 25) {
      counter = 0;
      try {
        refreshOverlays();
      } catch (...) {
      }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
}

} // namespace gi
