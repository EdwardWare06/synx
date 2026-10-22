// Minimal test helpers. No framework dependency.
#pragma once

#include <cstdio>
#include <string>

namespace synx_test {

inline int g_failures = 0;
inline int g_checks = 0;

#define CHECK(cond)                                                        \
  do {                                                                     \
    ++synx_test::g_checks;                                                 \
    if (!(cond)) {                                                         \
      ++synx_test::g_failures;                                             \
      std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);          \
    }                                                                      \
  } while (0)

#define CHECK_EQ(a, b)                                                     \
  do {                                                                     \
    ++synx_test::g_checks;                                                 \
    auto _va = (a);                                                        \
    auto _vb = (b);                                                        \
    if (!(_va == _vb)) {                                                   \
      ++synx_test::g_failures;                                             \
      std::printf("FAIL %s:%d: %s == %s\n", __FILE__, __LINE__, #a, #b);   \
    }                                                                      \
  } while (0)

inline int finish(const char* name) {
  std::printf("%s: %d checks, %d failures\n", name, g_checks, g_failures);
  return g_failures == 0 ? 0 : 1;
}

}  // namespace synx_test
