#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace gi {

struct Sym {
  std::string name; // mangled
  uintptr_t offset; // relative to the module load address
};

// Reads the dynamic symbol table of libminecraftpe.so straight from memory.
class Symbols {
public:
  static Symbols &get();

  // Loads once. Returns false when the module/symbols are not available.
  bool load();
  uintptr_t base() const { return mBase; }
  size_t size() const { return mSyms.size(); }
  const Sym &at(size_t i) const { return mSyms[i]; }

  // Case-insensitive substring search on mangled and demangled names.
  std::vector<size_t> search(const std::string &needle, size_t cap) const;
  // Exact (mangled) match first, then dlsym.
  uintptr_t resolveName(const std::string &name) const;

  static std::string demangle(const std::string &mangled);
  size_t dump(const std::string &path) const;

private:
  bool mLoaded{false};
  uintptr_t mBase{0};
  std::vector<Sym> mSyms;
};

} // namespace gi
