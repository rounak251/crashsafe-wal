#include "crc32c.h"

#include <array>

namespace csw {
namespace {

// Reflected Castagnoli polynomial. A table of 256 entries is enough here; the
// frames are short and the table is simple to explain in an interview.
std::array<uint32_t, 256> make_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

const std::array<uint32_t, 256>& table() {
  static const std::array<uint32_t, 256> t = make_table();
  return t;
}

}  // namespace

uint32_t crc32c_extend(uint32_t crc, std::string_view data) {
  uint32_t c = ~crc;
  for (unsigned char b : data) {
    c = table()[(c ^ b) & 0xFFu] ^ (c >> 8);
  }
  return ~c;
}

}  // namespace csw
