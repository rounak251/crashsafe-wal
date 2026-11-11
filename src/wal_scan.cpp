#include "wal_scan.h"

namespace csw {

const char* stop_reason_name(StopReason r) {
  switch (r) {
    case StopReason::kEnd:
      return "end";
    case StopReason::kTornFrame:
      return "torn-frame";
    case StopReason::kBadFrame:
      return "bad-frame-crc";
    case StopReason::kBadRecord:
      return "bad-record";
  }
  return "unknown";
}

WalScan scan_wal(std::string_view wal, const FrameVisitor& visit) {
  WalScan scan;
  size_t offset = 0;
  while (offset < wal.size()) {
    std::string_view payload;
    size_t next = 0;
    FrameRead read = read_frame_checked(wal, offset, &payload, &next);
    if (read == FrameRead::kTorn) {
      scan.stop = StopReason::kTornFrame;
      break;
    }
    if (read == FrameRead::kBadCrc) {
      scan.stop = StopReason::kBadFrame;
      break;
    }

    FrameInfo info;
    info.offset = offset;
    info.length = payload.size();
    Record rec;
    if (payload.empty()) {
      info.skip = true;
    } else if (decode_record(payload, &rec)) {
      info.record = &rec;
    } else {
      scan.stop = StopReason::kBadRecord;
      break;
    }
    if (visit) visit(info);

    offset = next;
    scan.good_end = next;
    scan.frames++;
  }
  return scan;
}

}  // namespace csw
