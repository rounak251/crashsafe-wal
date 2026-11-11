#pragma once

// Little-endian integer helpers for the frame, record and snapshot formats.
// Kept in one place so the three formats cannot disagree about byte order.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace csw {

inline void put_u32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

inline void put_u64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>((v >> (8 * i)) & 0xFFu));
}

inline uint32_t get_u32(std::string_view s, size_t at) {
  uint32_t v = 0;
  for (int i = 0; i < 4; ++i) {
    v |= static_cast<uint32_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
  }
  return v;
}

inline uint64_t get_u64(std::string_view s, size_t at) {
  uint64_t v = 0;
  for (int i = 0; i < 8; ++i) {
    v |= static_cast<uint64_t>(static_cast<unsigned char>(s[at + i])) << (8 * i);
  }
  return v;
}

}  // namespace csw
