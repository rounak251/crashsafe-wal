#pragma once

#include <cstdint>
#include <string_view>

namespace csw {

// CRC32C (Castagnoli). crc32c_extend(crc32c_extend(0, a), b) == crc32c(a + b).
uint32_t crc32c_extend(uint32_t crc, std::string_view data);

inline uint32_t crc32c(std::string_view data) { return crc32c_extend(0, data); }

}  // namespace csw
