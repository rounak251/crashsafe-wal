#include "record.h"

#include "crc32c.h"
#include "le.h"
#include "mutant.h"

namespace csw {
namespace {

// The CRC has to cover the length field. Otherwise a zero-filled hole, whose
// CRC field and payload are both zero, passes as an empty frame.
uint32_t frame_crc(uint32_t length, std::string_view payload) {
  if (mutant_is(5)) return crc32c(payload);  // mutant: length not checksummed
  std::string len_bytes;
  put_u32(len_bytes, length);
  return crc32c_extend(crc32c(len_bytes), payload);
}

}  // namespace

std::string encode_frame(std::string_view payload) {
  uint32_t length = static_cast<uint32_t>(payload.size());
  std::string out;
  out.reserve(kFrameHeaderSize + payload.size());
  put_u32(out, length);
  put_u32(out, frame_crc(length, payload));
  out.append(payload.data(), payload.size());
  return out;
}

FrameRead read_frame_checked(std::string_view file, size_t offset, std::string_view* payload,
                             size_t* next_offset) {
  if (offset > file.size() || file.size() - offset < kFrameHeaderSize) return FrameRead::kTorn;
  uint32_t length = get_u32(file, offset);
  uint32_t crc = get_u32(file, offset + 4);
  size_t body = offset + kFrameHeaderSize;
  if (length > file.size() - body) return FrameRead::kTorn;
  std::string_view p = file.substr(body, length);
  if (!mutant_is(6) && frame_crc(length, p) != crc) return FrameRead::kBadCrc;
  *payload = p;
  *next_offset = body + length;
  return FrameRead::kOk;
}

bool read_frame(std::string_view file, size_t offset, std::string_view* payload,
                size_t* next_offset) {
  return read_frame_checked(file, offset, payload, next_offset) == FrameRead::kOk;
}

std::string encode_record(const Record& rec) {
  std::string out;
  put_u64(out, rec.seq);
  out.push_back(static_cast<char>(rec.op.kind));
  put_u32(out, static_cast<uint32_t>(rec.op.key.size()));
  out += rec.op.key;
  put_u32(out, static_cast<uint32_t>(rec.op.value.size()));
  out += rec.op.value;
  return out;
}

bool decode_record(std::string_view payload, Record* out) {
  // seq (8) + kind (1) + klen (4) + vlen (4) is the fixed part.
  if (payload.size() < 17) return false;
  out->seq = get_u64(payload, 0);
  uint8_t kind = static_cast<uint8_t>(payload[8]);
  if (kind != static_cast<uint8_t>(Op::Kind::kPut) && kind != static_cast<uint8_t>(Op::Kind::kDel)) {
    return false;
  }
  out->op.kind = static_cast<Op::Kind>(kind);
  size_t klen = get_u32(payload, 9);
  size_t pos = 13;
  if (klen > payload.size() - pos - 4) return false;
  out->op.key.assign(payload.substr(pos, klen));
  pos += klen;
  size_t vlen = get_u32(payload, pos);
  pos += 4;
  if (vlen != payload.size() - pos) return false;
  out->op.value.assign(payload.substr(pos, vlen));
  if (out->op.kind == Op::Kind::kDel && vlen != 0) return false;
  return true;
}

}  // namespace csw
