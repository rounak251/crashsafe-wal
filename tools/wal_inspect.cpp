// Prints the snapshot and the WAL of a store directory, frame by frame, using
// the same scanner as recovery. Read-only: it never truncates or repairs.
//
//   wal_inspect DIR

#include <cstdio>
#include <string>

#include "posix_fs.h"
#include "snapshot.h"
#include "wal_scan.h"

namespace {

const char* op_name(csw::Op::Kind k) { return k == csw::Op::Kind::kPut ? "put" : "del"; }

int inspect_snapshot(csw::FileSystem& fs) {
  if (!fs.exists("snapshot")) {
    std::printf("snapshot: none\n");
    return 0;
  }
  auto r = fs.read_all("snapshot");
  if (!r.status.ok()) {
    std::printf("snapshot: unreadable (%s)\n", r.status.message.c_str());
    return 1;
  }
  uint64_t seq = 0;
  csw::KvMap state;
  if (!csw::decode_snapshot(r.value, &seq, &state)) {
    std::printf("snapshot: %zu bytes, INVALID (recovery refuses to open this store)\n",
                r.value.size());
    return 1;
  }
  std::printf("snapshot: %zu bytes, seq=%llu, keys=%zu\n", r.value.size(),
              static_cast<unsigned long long>(seq), state.size());
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 2) {
    std::fprintf(stderr, "usage: wal_inspect DIR\n");
    return 2;
  }
  csw::PosixFs fs(argv[1]);
  int status = inspect_snapshot(fs);

  auto r = fs.read_all("wal.log");
  if (!r.status.ok()) {
    std::printf("wal.log: %s\n", r.status.message.c_str());
    return 2;
  }
  const std::string& wal = r.value;
  csw::WalScan scan = csw::scan_wal(wal, [](const csw::FrameInfo& f) {
    if (f.skip) {
      std::printf("%8zu  skip frame (%zu bytes)\n", f.offset, f.length);
      return;
    }
    std::printf("%8zu  seq=%-8llu %-3s len=%-6zu key=%s\n", f.offset,
                static_cast<unsigned long long>(f.record->seq), op_name(f.record->op.kind), f.length,
                f.record->op.key.c_str());
  });
  std::printf("wal.log: %zu bytes, %zu valid frames, valid up to byte %zu, stop: %s\n", wal.size(),
              scan.frames, scan.good_end, csw::stop_reason_name(scan.stop));
  if (scan.good_end < wal.size()) {
    std::printf("tail of %zu bytes would be cut on open\n", wal.size() - scan.good_end);
  }
  return status;
}
