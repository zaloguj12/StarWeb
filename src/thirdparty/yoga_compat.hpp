#pragma once

// Polyfill for std::bit_cast on toolchains whose libc++ predates its
// introduction (e.g. Xcode 14 Command Line Tools), even when compiling with
// -std=c++20. Yoga's vendored sources use std::bit_cast unconditionally.
#include <version>

#ifndef __cpp_lib_bit_cast
#include <type_traits>

namespace std {
template <class To, class From>
constexpr To bit_cast(const From& src) noexcept {
  static_assert(sizeof(To) == sizeof(From));
  static_assert(std::is_trivially_copyable_v<To>);
  static_assert(std::is_trivially_copyable_v<From>);
  return __builtin_bit_cast(To, src);
}
} // namespace std
#endif
