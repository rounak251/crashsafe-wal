#pragma once

#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "fs.h"
#include "record.h"
#include "snapshot.h"
#include "status.h"
#include "wal_scan.h"
#include "wal_writer.h"

namespace csw {

// The crash checker watches the store through this. Callers without a checker pass
// nullptr. issued() is called before the record's bytes are written, so the
// checker never sees a record on disk that it does not know was issued.
class Observer {
 public:
  virtual ~Observer() = default;
  virtual void issued(uint64_t seq, const Op& op) = 0;
  virtual void acked(uint64_t seq) = 0;
};

enum class SyncPolicy {
  kGroup,  // writers that overlap share one fsync
  kPerOp,  // every commit runs its own fsync while holding the mutex
};

// What recovery does with a bad frame (one that fails its CRC or does not parse).
enum class DamagePolicy {
  // Cut the log at the bad frame and drop every byte after it. This is the
  // right answer for a crash, which can leave a hole followed by unacknowledged
  // data. It also silently drops acknowledged records if a synced record
  // rots; the store cannot tell those two cases apart.
  kTruncate,
  // Refuse to open if a bad frame has any bytes after its header. Nothing is
  // dropped, but a crash that left a hole also stops the store from opening.
  kRefuse,
};

struct StoreOptions {
  SyncPolicy policy = SyncPolicy::kGroup;
  DamagePolicy damage = DamagePolicy::kTruncate;
  Observer* observer = nullptr;
};

// Largest key or value accepted. Keeps every field well inside the u32 length
// fields of the frame format.
constexpr size_t kMaxFieldBytes = 1u << 20;

struct StoreStats {
  uint64_t commits = 0;     // commit() calls that returned success
  uint64_t records = 0;     // records appended to the WAL
  uint64_t syncs = 0;       // fsync calls on the WAL, successful or not
  uint64_t checkpoints = 0;
};

struct RecoveryInfo {
  uint64_t snapshot_seq = 0;
  uint64_t records_replayed = 0;
  uint64_t bytes_truncated = 0;  // torn or corrupt tail removed on open
  StopReason stop = StopReason::kEnd;
};

class Store {
 public:
  // Runs recovery. Fails with kCorrupt if a snapshot exists and does not parse.
  static Result<std::unique_ptr<Store>> open(FileSystem& fs, StoreOptions opts = {});

  // Writes the ops as consecutive records, then returns once they are durable.
  // Returns kPoisoned if an earlier fsync failed.
  Status commit(const std::vector<Op>& ops);
  Status put(const std::string& key, const std::string& value);
  Status del(const std::string& key);

  std::optional<std::string> get(const std::string& key) const;
  // Every key that starts with prefix, in key order.
  KvMap scan_prefix(const std::string& prefix) const;
  Status checkpoint();

  uint64_t last_seq() const;
  KvMap contents() const;
  bool poisoned() const;
  StoreStats stats() const;
  RecoveryInfo recovery() const;

 private:
  Store(FileSystem& fs, StoreOptions opts);

  Status recover();
  Status poison(Status st);
  Status write_snapshot(const std::string& bytes);
  void apply_locked(const std::vector<Op>& ops, uint64_t last);
  Status wait_for_sync(std::unique_lock<std::mutex>& lk, uint64_t first, uint64_t last);

  FileSystem& fs_;
  StoreOptions opts_;
  std::unique_ptr<WalWriter> wal_;

  KvMap state_;
  uint64_t last_seq_ = 0;      // last sequence number appended
  uint64_t synced_seq_ = 0;    // every record up to this one is durable
  uint64_t applied_seq_ = 0;   // every record up to this one is in state_
  bool sync_running_ = false;  // a group leader is inside fsync
  bool poisoned_ = false;
  StoreStats stats_;
  RecoveryInfo recovery_;

  mutable std::mutex mu_;
  std::condition_variable cv_;
};

}  // namespace csw
