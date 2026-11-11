#pragma once

// A small test runner: TEST_CASE registers a function, CHECK reports a failure
// by throwing, and the runner keeps going with the next test.

#include <stdexcept>
#include <string>
#include <vector>

namespace testing {

struct Case {
  const char* name;
  void (*fn)();
};

std::vector<Case>& registry();

struct Registrar {
  Registrar(const char* name, void (*fn)()) { registry().push_back(Case{name, fn}); }
};

[[noreturn]] void fail(const char* file, int line, const std::string& what);

}  // namespace testing

#define TEST_CASE(name)                                          \
  static void name();                                            \
  static ::testing::Registrar name##_registrar(#name, &name);    \
  static void name()

#define CHECK(cond)                                                       \
  do {                                                                    \
    if (!(cond)) ::testing::fail(__FILE__, __LINE__, "CHECK(" #cond ")"); \
  } while (0)

#define CHECK_EQ(a, b)                                                            \
  do {                                                                            \
    auto&& check_a_ = (a);                                                        \
    auto&& check_b_ = (b);                                                        \
    if (!(check_a_ == check_b_)) ::testing::fail(__FILE__, __LINE__, "CHECK_EQ(" #a ", " #b ")"); \
  } while (0)
