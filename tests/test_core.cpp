#include <string>

#include "crc32c.h"
#include "wal_scan.h"
#include "record.h"
#include "snapshot.h"
#include "test.h"

using namespace csw;

TEST_CASE(crc32c_known_vector) {
  // Standard CRC32C check value for the ASCII digits 1..9.
  CHECK_EQ(crc32c("123456789"), 0xE3069283u);
}

TEST_CASE(crc32c_extend_matches_concatenation) {
  CHECK_EQ(crc32c_extend(crc32c("abc"), "def"), crc32c("abcdef"));
}

TEST_CASE(zero_bytes_have_nonzero_crc) {
  // Why the frame CRC covers the length: a zero hole has length 0, and an empty
  // payload has CRC 0. The CRC over the four length bytes is not 0, so a hole fails.
  // The exact value is checked against an independent implementation.
  std::string zeros(4, '\0');
  CHECK_EQ(crc32c(zeros), 0x48674BC7u);
  CHECK(crc32c(zeros) != 0);
}

TEST_CASE(frame_round_trip) {
  std::string frame = encode_frame("hello");
  std::string_view payload;
  size_t next = 0;
  CHECK(read_frame(frame, 0, &payload, &next));
  CHECK_EQ(std::string(payload), std::string("hello"));
  CHECK_EQ(next, frame.size());
}

TEST_CASE(frame_rejects_torn_frame) {
  std::string frame = encode_frame("hello world");
  for (size_t cut = 0; cut < frame.size(); ++cut) {
    std::string_view payload;
    size_t next = 0;
    CHECK(!read_frame(std::string_view(frame).substr(0, cut), 0, &payload, &next));
  }
}

TEST_CASE(frame_rejects_flipped_length_byte) {
  std::string frame = encode_frame("hello world");
  frame[0] ^= 0x01;
  std::string_view payload;
  size_t next = 0;
  CHECK(!read_frame(frame, 0, &payload, &next));
}

TEST_CASE(zero_region_is_not_a_frame) {
  std::string zeros(32, '\0');
  std::string_view payload;
  size_t next = 0;
  CHECK(!read_frame(zeros, 0, &payload, &next));
}

TEST_CASE(record_round_trip) {
  Record put{7, Op::Put("key", "value")};
  Record del{8, Op::Del("key")};
  Record out;
  CHECK(decode_record(encode_record(put), &out));
  CHECK_EQ(out.seq, 7u);
  CHECK(out.op.kind == Op::Kind::kPut);
  CHECK_EQ(out.op.key, std::string("key"));
  CHECK_EQ(out.op.value, std::string("value"));
  CHECK(decode_record(encode_record(del), &out));
  CHECK(out.op.kind == Op::Kind::kDel);
  CHECK(out.op.value.empty());
}

TEST_CASE(record_rejects_bad_fields) {
  std::string bytes = encode_record(Record{1, Op::Put("k", "v")});
  Record out;
  std::string bad_kind = bytes;
  bad_kind[8] = 9;
  CHECK(!decode_record(bad_kind, &out));
  CHECK(!decode_record(bytes.substr(0, bytes.size() - 1), &out));
  CHECK(!decode_record(bytes + "x", &out));
}

TEST_CASE(snapshot_round_trip) {
  KvMap state{{"a", "1"}, {"b", ""}, {"c", std::string(300, 'x')}};
  std::string bytes = encode_snapshot(42, state);
  uint64_t seq = 0;
  KvMap back;
  CHECK(decode_snapshot(bytes, &seq, &back));
  CHECK_EQ(seq, 42u);
  CHECK(back == state);
  CHECK(!decode_snapshot(bytes + "x", &seq, &back));
  CHECK(!decode_snapshot(bytes.substr(0, bytes.size() - 1), &seq, &back));
  CHECK(!decode_snapshot("", &seq, &back));
}

TEST_CASE(scan_reports_why_it_stopped) {
  std::string good = encode_frame(encode_record(Record{1, Op::Put("a", "1")}));
  std::string torn = good.substr(0, good.size() - 1);
  std::string bad = good;
  bad[bad.size() - 1] ^= 0x40;  // corrupt the payload, so the CRC fails

  size_t seen = 0;
  WalScan clean = scan_wal(good + good, [&](const FrameInfo& f) {
    CHECK(!f.skip);
    CHECK(f.record != nullptr);
    ++seen;
  });
  CHECK_EQ(seen, 2u);
  CHECK(clean.stop == StopReason::kEnd);
  CHECK_EQ(clean.good_end, good.size() * 2);

  WalScan cut = scan_wal(good + torn, nullptr);
  CHECK(cut.stop == StopReason::kTornFrame);
  CHECK_EQ(cut.good_end, good.size());

  WalScan crc = scan_wal(bad, nullptr);
  CHECK(crc.stop == StopReason::kBadFrame);
  CHECK_EQ(crc.good_end, 0u);
}

TEST_CASE(scan_treats_empty_payload_as_skip_frame) {
  std::string skip = encode_frame("");
  size_t skips = 0;
  WalScan scan = scan_wal(skip, [&](const FrameInfo& f) {
    if (f.skip) ++skips;
  });
  CHECK_EQ(skips, 1u);
  CHECK(scan.stop == StopReason::kEnd);
}
