#pragma once
// Small shims for toolchains that stop at GCC 4.6 (Symbian's GCGE is 4.6.3):
//
//   * std::to_string / std::stoi / std::stoull  — libstdc++ only has the C++11
//     string conversions from GCC 4.9 on
//   * std::chrono                               — only used to seed the RNG
//
// Everything here is header-only and inline, so no build system changes are
// needed for the Symbian qmake project. On a modern toolchain the std:: forms
// work fine; these are used everywhere anyway to keep one code path.

#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <stdint.h>   // <cstdint> does not exist in STLport 5
#include <string>
#include <vector>

// Toolchain floor: this project is C++11. GCC 4.6 is the oldest compiler known
// to handle it (Symbian GCGE 4.6.3); GCC 4.4 (MinGW in the Qt Simulator kit of
// the Qt SDK) has no nullptr, lambdas, auto, range-for or enum class.
#if defined(__GNUC__) && !defined(__clang__) && \
    (__GNUC__ < 4 || (__GNUC__ == 4 && __GNUC_MINOR__ < 6))
#  error "openknot-qt needs GCC 4.6 or newer (Symbian GCGE 4.6.3, MinGW 4.9+, GCC 5+). This kit's GCC is too old - select the Symbian Device kit, or build the desktop version with Qt 5."
#endif


namespace wa {
namespace compat {

// ── Container helpers ───────────────────────────────────────────────────────
// `compat::bytes(0x05)` and `compat::move(x)` need C++11 *library* support that STLport 5
// (the STL shipped with the Symbian SDK) does not have. These are the
// replacements used throughout the protocol code.
inline std::vector<uint8_t> bytes(uint8_t a) { return std::vector<uint8_t>(1, a); }

inline std::vector<uint8_t> bytes(uint8_t a, uint8_t b) {
    std::vector<uint8_t> out;
    out.push_back(a);
    out.push_back(b);
    return out;
}

inline std::vector<uint8_t> bytes(uint8_t a, uint8_t b, uint8_t c) {
    std::vector<uint8_t> out;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    return out;
}

inline std::vector<uint8_t> bytes(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    std::vector<uint8_t> out;
    out.push_back(a);
    out.push_back(b);
    out.push_back(c);
    out.push_back(d);
    return out;
}

// std::vector::data() is C++11 and STLport 5.2 does not have it (it also has
// no emplace_back or initializer lists). std::string::data() is fine, so both
// are covered here.
template <typename T> inline T *dataOf(std::vector<T> &v) {
    return v.empty() ? 0 : &v[0];
}

template <typename T> inline const T *dataOf(const std::vector<T> &v) {
    return v.empty() ? 0 : &v[0];
}

inline const char *dataOf(const std::string &s) { return s.data(); }

inline std::vector<uint8_t> bytesFromText(const char *text) {
    std::vector<uint8_t> out;
    while (text && *text) out.push_back(uint8_t(*text++));
    return out;
}

// Stand-in for std::move: it copies. The call sites only used it to avoid a
// copy right before the source is cleared, so plain assignment is correct.
template <typename T> T &move(T &value) { return value; }

inline std::string toString(int v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%d", v);
    return std::string(buf);
}
inline std::string toString(unsigned int v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%u", v);
    return std::string(buf);
}
inline std::string toString(long v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%ld", v);
    return std::string(buf);
}
inline std::string toString(unsigned long v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%lu", v);
    return std::string(buf);
}
inline std::string toString(long long v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%lld", v);
    return std::string(buf);
}
inline std::string toString(unsigned long long v) {
    char buf[24];
    snprintf(buf, sizeof buf, "%llu", v);
    return std::string(buf);
}

// std::stoull equivalent: returns false instead of throwing. Templated so a
// uint64_t& (which is `unsigned long` on some ABIs) binds without a cast.
template <typename T>
inline bool parseUnsigned(const std::string &text, T &out) {
    if (text.empty()) return false;
    errno = 0;
    char *end = 0;
    const unsigned long long value = strtoull(text.c_str(), &end, 10);
    if (errno != 0 || end == text.c_str()) return false;
    while (end && *end == ' ') ++end;
    if (end && *end != '\0') return false;
    out = static_cast<T>(value);
    return true;
}

// std::stoi(..., 16) equivalent for one byte worth of hex.
inline int hexNibble(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Entropy source for the RNG seed on toolchains without <chrono>.
inline unsigned long long clockTicks() {
    return static_cast<unsigned long long>(time(0)) ^
           (static_cast<unsigned long long>(clock()) << 20);
}

} // namespace compat
} // namespace wa
