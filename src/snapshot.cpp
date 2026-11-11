#include "snapshot.h"

#include "le.h"
#include "record.h"

namespace csw {

std::string encode_snapshot(uint64_t seq, const KvMap& state) {
  std::string payload;
  put_u64(payload, seq);
  put_u32(payload, static_cast<uint32_t>(state.size()));
  for (const auto& [key, value] : state) {
    put_u32(payload, static_cast<uint32_t>(key.size()));
    payload += key;
    put_u32(payload, static_cast<uint32_t>(value.size()));
    payload += value;
  }
  return encode_frame(payload);
}

bool decode_snapshot(std::string_view file, uint64_t* seq, KvMap* state) {
  std::string_view payload;
  size_t next = 0;
  if (!read_frame(file, 0, &payload, &next)) return false;
  if (next != file.size()) return false;  // trailing bytes: not a snapshot we wrote
  if (payload.size() < 12) return false;

  *seq = get_u64(payload, 0);
  uint32_t count = get_u32(payload, 8);
  size_t pos = 12;
  KvMap out;
  for (uint32_t i = 0; i < count; ++i) {
    if (payload.size() - pos < 4) return false;
    size_t klen = get_u32(payload, pos);
    pos += 4;
    if (payload.size() - pos < klen + 4) return false;
    std::string key(payload.substr(pos, klen));
    pos += klen;
    size_t vlen = get_u32(payload, pos);
    pos += 4;
    if (payload.size() - pos < vlen) return false;
    out.emplace(std::move(key), std::string(payload.substr(pos, vlen)));
    pos += vlen;
  }
  if (pos != payload.size()) return false;
  *state = std::move(out);
  return true;
}

}  // namespace csw
