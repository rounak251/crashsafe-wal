#pragma once

// One parser for the WAL. Recovery and the wal_inspect tool both use it, so
// the inspector shows exactly what recovery would accept.

#include <cstddef>
#include <functional>
#include <string_view>

#include "record.h"

namespace csw {

enum class StopReason {
  kEnd,        // every byte belonged to a valid frame
  kTornFrame,  // the file ends inside a frame header or payload
  kBadFrame,   // a frame's CRC does not match
  kBadRecord,  // a frame's CRC matches but its payload is not a record
};

const char* stop_reason_name(StopReason r);

struct FrameInfo {
  size_t offset = 0;
  size_t length = 0;
  bool skip = false;             // empty payload: a skip frame, carries no record
  const Record* record = nullptr;  // set for every non-skip frame
};

struct WalScan {
  size_t good_end = 0;  // byte offset just past the last valid frame
  size_t frames = 0;    // valid frames seen, skip frames included
  StopReason stop = StopReason::kEnd;
};

using FrameVisitor = std::function<void(const FrameInfo&)>;

// Visits valid frames in file order and stops at the first one that is not.
WalScan scan_wal(std::string_view wal, const FrameVisitor& visit);

}  // namespace csw
