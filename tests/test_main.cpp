#include <cstdio>

#include "test.h"

namespace testing {

std::vector<Case>& registry() {
  static std::vector<Case> cases;
  return cases;
}

void fail(const char* file, int line, const std::string& what) {
  throw std::runtime_error(std::string(file) + ":" + std::to_string(line) + ": " + what);
}

}  // namespace testing

int main() {
  int failed = 0;
  for (const auto& c : testing::registry()) {
    try {
      c.fn();
      std::printf("PASS %s\n", c.name);
    } catch (const std::exception& e) {
      ++failed;
      std::printf("FAIL %s: %s\n", c.name, e.what());
    }
  }
  std::printf("%zu tests, %d failed\n", testing::registry().size(), failed);
  return failed == 0 ? 0 : 1;
}
