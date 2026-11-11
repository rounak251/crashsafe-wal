#pragma once

// A snapshot is one frame whose payload is the full key/value state at a
// sequence number. It is valid only if the frame covers the whole file.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>

namespace csw {

using KvMap = std::map<std::string, std::string>;

std::string encode_snapshot(uint64_t seq, const KvMap& state);
bool decode_snapshot(std::string_view file, uint64_t* seq, KvMap* state);

}  // namespace csw
