#pragma once
// Host stub for the firmware logging macros.
//
// Silent by default so a boundary sweep (hundreds of converts) does not drown
// the test output. Define CASPER_TEST_VERBOSE_LOG to see the real messages when
// chasing a specific failure.
//
// Both forms consume every argument, so variables that exist only to be logged
// do not trip -Wunused under -Wall -Wextra.

#include <cstdio>

namespace casper_test_log {
inline void sink(const char*, ...) {}
}  // namespace casper_test_log

#ifdef CASPER_TEST_VERBOSE_LOG
#define CASPER_TEST_LOG(tag, ...)   \
  do {                              \
    std::printf("[%s] ", (tag));    \
    std::printf(__VA_ARGS__);       \
    std::printf("\n");              \
  } while (0)
#else
#define CASPER_TEST_LOG(tag, ...)              \
  do {                                         \
    if (false) {                               \
      casper_test_log::sink((tag), ##__VA_ARGS__); \
    }                                          \
  } while (0)
#endif

#define LOG_DBG(tag, ...) CASPER_TEST_LOG(tag, __VA_ARGS__)
#define LOG_INF(tag, ...) CASPER_TEST_LOG(tag, __VA_ARGS__)
#define LOG_ERR(tag, ...) CASPER_TEST_LOG(tag, __VA_ARGS__)
