#pragma once

// The crash model from docs/crash-model.md, as code. The model keeps two
// views of the file system: the live view (every operation applied) and the
// durable view (only operations that a successful fsync covered). The
// operations that are neither are the window. A crash image is the durable
// view with a chosen outcome for each window operation.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "record.h"

namespace csw {

enum class EventKind {
  kCreate,
  kWrite,
  kTruncate,
  kRename,
  kSync,
  kSyncDir,
  kIssue,  // marker: record of this seq and op is about to be written
  kAck,    // marker: commit for this seq has returned success
};

struct Event {
  EventKind kind = EventKind::kCreate;
  uint64_t inode = 0;   // Create, Write, Truncate, Sync
  uint64_t offset = 0;  // Write
  uint64_t size = 0;    // Truncate
  std::string name;     // Create: the name. Rename: the source name.
  std::string dst;      // Rename: the destination name
  std::string bytes;    // Write: the bytes written
  bool ok = true;       // Sync, SyncDir: false when the fsync failed
  uint64_t seq = 0;     // Issue, Ack
  Op op;                // Issue
};

using Image = std::map<std::string, std::string>;  // name -> content

// One operation that is not yet durable.
struct WindowOp {
  EventKind kind = EventKind::kWrite;
  uint64_t inode = 0;
  uint64_t offset = 0;
  uint64_t size = 0;
  std::string name;
  std::string dst;
  std::string bytes;

  // Number of outcomes: a write of L bytes has L+1 (prefix lengths 0..L);
  // every other window operation has 2 (applied or not).
  uint64_t options() const;
};

class CrashModel {
 public:
  // Applies one file-system event. Marker events are ignored.
  void apply(const Event& e);

  // Puts a file in both views. Used to build a crash image.
  void install(const std::string& name, uint64_t inode, const std::string& content);

  // The running program's view: everything applied.
  Image live() const;
  // The strictest crash: every window operation dropped.
  Image durable() const;
  // Crash image for one outcome per window operation. choice[i] is in
  // [0, window()[i].options()).
  Image image(const std::vector<uint64_t>& choice) const;

  const std::vector<WindowOp>& window() const { return window_; }

  bool live_has(const std::string& name) const { return live_ns_.count(name) != 0; }
  uint64_t live_inode(const std::string& name) const { return live_ns_.at(name); }
  uint64_t live_size(uint64_t inode) const;
  std::string live_content(uint64_t inode) const;

 private:
  std::map<std::string, uint64_t> live_ns_;
  std::map<std::string, uint64_t> durable_ns_;
  std::map<uint64_t, std::string> live_data_;
  std::map<uint64_t, std::string> durable_data_;
  std::vector<WindowOp> window_;
};

}  // namespace csw
