#pragma once

#include <cstdint>
#include <cstdio>
#include <string>

namespace gi {

inline std::string hex(uint64_t v) {
  char b[32];
  std::snprintf(b, sizeof b, "0x%llx", static_cast<unsigned long long>(v));
  return b;
}

inline std::string trim(const std::string &s) {
  size_t a = 0, b = s.size();
  while (a < b && (s[a] == ' ' || s[a] == '\t' || s[a] == '\n' || s[a] == '\r')) ++a;
  while (b > a && (s[b - 1] == ' ' || s[b - 1] == '\t' || s[b - 1] == '\n' || s[b - 1] == '\r')) --b;
  return s.substr(a, b - a);
}

// Accepts decimal (also negative) and 0x-prefixed hexadecimal.
inline bool parseU64(const std::string &text, uint64_t &out) {
  const std::string s = trim(text);
  if (s.empty()) return false;
  try {
    size_t used = 0;
    if (s[0] == '-') {
      out = static_cast<uint64_t>(std::stoll(s, &used, 0));
    } else {
      out = std::stoull(s, &used, 0);
    }
    return used == s.size();
  } catch (...) {
    return false;
  }
}

inline std::string cut(const std::string &s, size_t n) {
  return s.size() <= n ? s : s.substr(0, n - 3) + "...";
}

} // namespace gi
