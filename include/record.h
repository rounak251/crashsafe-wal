#pragma once

// Framing of WAL records and snapshots: length | crc | payload.
// See docs/design.md for why the CRC covers the length field.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace csw {

struct Op {
  enum class Kind : uint8_t { kPut = 1, kDel = 2 };
  Kind kind = Kind::kPut;
  std::string key;
  std::string value;  // empty for a delete

  static Op Put(std::string k, std::string v) { return Op{Kind::kPut, std::move(k), std::move(v)}; }
  static Op Del(std::string k) { return Op{Kind::kDel, std::move(k), std::string()}; }
};

struct Record {
  uint64_t seq = 0;
  Op op;
};

// Wraps a payload in a frame. An empty payload is a legal skip frame.
std::string encode_frame(std::string_view payload);

// Bytes in a frame header: length (4) and crc (4).
constexpr size_t kFrameHeaderSize = 8;

// Why a frame read failed. kTorn: the file ends inside the frame. kBadCrc: the
// frame is complete but its check fails.
enum class FrameRead { kOk, kTorn, kBadCrc };

// Reads the frame that starts at offset and says why it stopped if it did.
FrameRead read_frame_checked(std::string_view file, size_t offset, std::string_view* payload,
                             size_t* next_offset);

// True only if read_frame_checked returns kOk.
bool read_frame(std::string_view file, size_t offset, std::string_view* payload,
                size_t* next_offset);

std::string encode_record(const Record& rec);
bool decode_record(std::string_view payload, Record* out);

}  // namespace csw
