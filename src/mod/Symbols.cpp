#include "Symbols.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>

#include <cxxabi.h>
#include <dlfcn.h>
#include <elf.h>
#include <link.h>

#include "Util.h"

namespace gi {
namespace {

struct Ctx {
  uintptr_t bias{0};
  const ElfW(Phdr) *phdr{nullptr};
  int count{0};
  bool found{false};
};

int phdrCallback(struct dl_phdr_info *info, size_t, void *data) {
  auto *c = static_cast<Ctx *>(data);
  if (info->dlpi_name && std::strstr(info->dlpi_name, "libminecraftpe.so")) {
    c->bias = static_cast<uintptr_t>(info->dlpi_addr);
    c->phdr = info->dlpi_phdr;
    c->count = info->dlpi_phnum;
    c->found = true;
    return 1;
  }
  return 0;
}

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
  return s;
}

} // namespace

Symbols &Symbols::get() {
  static Symbols inst;
  return inst;
}

bool Symbols::load() {
  if (mLoaded) return !mSyms.empty() || mBase != 0;
  mLoaded = true;

  Ctx ctx;
  dl_iterate_phdr(phdrCallback, &ctx);
  if (!ctx.found) return false;
  mBase = ctx.bias;

  const ElfW(Dyn) *dyn = nullptr;
  for (int i = 0; i < ctx.count; ++i) {
    if (ctx.phdr[i].p_type == PT_DYNAMIC) {
      dyn = reinterpret_cast<const ElfW(Dyn) *>(mBase + ctx.phdr[i].p_vaddr);
      break;
    }
  }
  if (!dyn) return true;

  auto fix = [&](uintptr_t p) { return p < mBase ? mBase + p : p; };
  uintptr_t symtab = 0, strtab = 0, hash = 0, gnuHash = 0;
  for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
    switch (d->d_tag) {
    case DT_SYMTAB: symtab = fix(d->d_un.d_ptr); break;
    case DT_STRTAB: strtab = fix(d->d_un.d_ptr); break;
    case DT_HASH: hash = fix(d->d_un.d_ptr); break;
    case DT_GNU_HASH: gnuHash = fix(d->d_un.d_ptr); break;
    default: break;
    }
  }
  if (!symtab || !strtab) return true;

  size_t symCount = 0;
  if (hash) {
    symCount = reinterpret_cast<const uint32_t *>(hash)[1];
  } else if (gnuHash) {
    const auto *w = reinterpret_cast<const uint32_t *>(gnuHash);
    const uint32_t nbuckets = w[0];
    const uint32_t symoffset = w[1];
    const uint32_t bloomSize = w[2];
    const auto *bloom = reinterpret_cast<const ElfW(Addr) *>(w + 4);
    const auto *buckets = reinterpret_cast<const uint32_t *>(bloom + bloomSize);
    const auto *chain = buckets + nbuckets;
    uint32_t maxIdx = 0;
    for (uint32_t i = 0; i < nbuckets; ++i) maxIdx = std::max(maxIdx, buckets[i]);
    if (maxIdx < symoffset) {
      symCount = symoffset;
    } else {
      while (!(chain[maxIdx - symoffset] & 1u)) ++maxIdx;
      symCount = static_cast<size_t>(maxIdx) + 1;
    }
  }

  const auto *syms = reinterpret_cast<const ElfW(Sym) *>(symtab);
  const char *strs = reinterpret_cast<const char *>(strtab);
  for (size_t i = 1; i < symCount; ++i) {
    const ElfW(Sym) &s = syms[i];
    if (s.st_shndx == SHN_UNDEF || s.st_value == 0) continue;
    if (ELF64_ST_TYPE(s.st_info) != STT_FUNC) continue;
    mSyms.push_back({std::string(strs + s.st_name), static_cast<uintptr_t>(s.st_value)});
  }
  return true;
}

std::string Symbols::demangle(const std::string &mangled) {
  int status = 0;
  char *out = abi::__cxa_demangle(mangled.c_str(), nullptr, nullptr, &status);
  if (status != 0 || !out) return mangled;
  std::string s(out);
  std::free(out);
  return s;
}

std::vector<size_t> Symbols::search(const std::string &needle, size_t cap) const {
  std::vector<size_t> out;
  const std::string n = lower(trim(needle));
  if (n.empty()) return out;
  for (size_t i = 0; i < mSyms.size() && out.size() < cap; ++i) {
    if (lower(mSyms[i].name).find(n) != std::string::npos ||
        lower(demangle(mSyms[i].name)).find(n) != std::string::npos) {
      out.push_back(i);
    }
  }
  return out;
}

uintptr_t Symbols::resolveName(const std::string &name) const {
  for (const Sym &s : mSyms) {
    if (s.name == name) return mBase + s.offset;
  }
  void *p = dlsym(RTLD_DEFAULT, name.c_str());
  return reinterpret_cast<uintptr_t>(p);
}

size_t Symbols::dump(const std::string &path) const {
  std::ofstream f(path);
  if (!f) return 0;
  f << "# offset  mangled  demangled   (base=" << hex(mBase) << ")\n";
  for (const Sym &s : mSyms) {
    f << hex(s.offset) << "  " << s.name << "  " << demangle(s.name) << "\n";
  }
  return mSyms.size();
}

} // namespace gi
