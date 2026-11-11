#include "store.h"

#include <algorithm>

#include "mutant.h"

namespace csw {
namespace {

const char kPoisonedMessage[] = "writer is poisoned after a failed sync; close and reopen the store";

Status poisoned_status() { return Status::Error(Code::kPoisoned, kPoisonedMessage); }

}  // namespace

Store::Store(FileSystem& fs, StoreOptions opts) : fs_(fs), opts_(opts) {}

Result<std::unique_ptr<Store>> Store::open(FileSystem& fs, StoreOptions opts) {
  Result<std::unique_ptr<Store>> r;
  std::unique_ptr<Store> store(new Store(fs, opts));
  r.status = store->recover();
  if (r.status.ok()) r.value = std::move(store);
  return r;
}

Status Store::recover() {
  uint64_t snap_seq = 0;
  if (fs_.exists("snapshot")) {
    auto snap = fs_.read_all("snapshot");
    if (!snap.status.ok()) return snap.status;
    // An invalid snapshot is an error. Ignoring it would silently drop data.
    if (!decode_snapshot(snap.value, &snap_seq, &state_)) {
      return Status::Error(Code::kCorrupt, "snapshot does not parse");
    }
  }

  uint64_t last = snap_seq;
  bool wal_existed = fs_.exists("wal.log");
  std::string wal;
  if (wal_existed) {
    auto r = fs_.read_all("wal.log");
    if (!r.status.ok()) return r.status;
    wal = std::move(r.value);
  }

  // Records at or below the snapshot sequence are already in the snapshot.
  // The scan stops at the first frame that is torn or fails its check.
  WalScan scan = scan_wal(wal, [&](const FrameInfo& f) {
    if (!f.record) return;
    if (f.record->seq <= last) return;
    if (f.record->op.kind == Op::Kind::kPut) {
      state_[f.record->op.key] = f.record->op.value;
    } else {
      state_.erase(f.record->op.key);
    }
    last = f.record->seq;
    recovery_.records_replayed++;
  });
  recovery_.snapshot_seq = snap_seq;
  recovery_.stop = scan.stop;
  recovery_.bytes_truncated = wal.size() - scan.good_end;

  // A torn tail is always cut. A bad frame with bytes after its header is
  // only cut under kTruncate, and kRefuse reports it instead of dropping data.
  bool bad_frame = scan.stop == StopReason::kBadFrame || scan.stop == StopReason::kBadRecord;
  if (opts_.damage == DamagePolicy::kRefuse && bad_frame &&
      wal.size() > scan.good_end + kFrameHeaderSize) {
    return Status::Error(Code::kCorrupt, "WAL has a bad frame at byte " +
                                             std::to_string(scan.good_end) +
                                             " with data after it; refusing to drop it");
  }

  last_seq_ = synced_seq_ = applied_seq_ = last;

  auto wal_file = fs_.open_append("wal.log");
  if (!wal_file.status.ok()) return wal_file.status;
  wal_ = std::make_unique<WalWriter>(std::move(wal_file.value));

  // A torn tail left in place would sit in front of every later append and
  // hide them from the next recovery.
  if (wal_existed && scan.good_end < wal.size() && !mutant_is(7)) {
    Status st = wal_->cut(scan.good_end);
    if (st.ok()) st = wal_->sync();
    if (!st.ok()) return st;
  }
  // A WAL that was just created is only reachable after its name is durable.
  if (!wal_existed && !mutant_is(2)) return fs_.sync_dir();
  return Status::Ok();
}

Status Store::poison(Status st) {
  poisoned_ = true;
  cv_.notify_all();
  return st;
}

Status Store::commit(const std::vector<Op>& ops) {
  std::unique_lock<std::mutex> lk(mu_);
  if (poisoned_) return poisoned_status();
  if (ops.empty()) return Status::Ok();
  // Checked before any record is written, so a refused batch leaves no trace.
  for (const Op& op : ops) {
    if (op.key.size() > kMaxFieldBytes || op.value.size() > kMaxFieldBytes) {
      return Status::Error(Code::kInvalidArgument, "key or value is larger than 1 MiB");
    }
  }

  // Records are appended under the mutex, so sequence numbers are in file order.
  uint64_t first = last_seq_ + 1;
  for (const Op& op : ops) {
    uint64_t seq = ++last_seq_;
    if (opts_.observer) opts_.observer->issued(seq, op);
    Status st = wal_->append(Record{seq, op});
    if (!st.ok()) return poison(st);
    stats_.records++;
  }
  uint64_t last = last_seq_;

  if (opts_.policy == SyncPolicy::kPerOp) {
    Status st = wal_->sync();
    stats_.syncs++;
    if (!st.ok()) return poison(st);
    synced_seq_ = last;
  } else if (!mutant_is(8)) {
    Status st = wait_for_sync(lk, first, last);
    if (!st.ok()) return st;
  }

  // Apply in sequence order. An earlier commit may still be waiting to apply.
  while (applied_seq_ + 1 < first && !poisoned_) cv_.wait(lk);
  if (applied_seq_ + 1 != first) return poisoned_status();
  apply_locked(ops, last);

  stats_.commits++;
  if (opts_.observer) {
    for (size_t i = 0; i < ops.size(); ++i) opts_.observer->acked(first + i);
  }
  return Status::Ok();
}

Status Store::wait_for_sync(std::unique_lock<std::mutex>& lk, uint64_t, uint64_t last) {
  Status failure;  // set only by the leader whose fsync failed
  while (synced_seq_ < last && !poisoned_) {
    if (sync_running_) {
      cv_.wait(lk);
      continue;
    }
    // This thread leads the group: one fsync covers everything appended so far.
    sync_running_ = true;
    uint64_t target = last_seq_;
    lk.unlock();
    Status st = wal_->sync();
    if (!st.ok() && mutant_is(10)) st = wal_->sync();  // mutant: retry once
    if (!st.ok() && mutant_is(9)) st = Status::Ok();   // mutant: ignore the error
    lk.lock();
    sync_running_ = false;
    stats_.syncs++;
    if (st.ok()) {
      synced_seq_ = std::max(synced_seq_, target);
    } else {
      failure = st;
      poisoned_ = true;
    }
    cv_.notify_all();
  }
  if (synced_seq_ < last) return failure.ok() ? poisoned_status() : failure;
  return Status::Ok();
}

void Store::apply_locked(const std::vector<Op>& ops, uint64_t last) {
  for (const Op& op : ops) {
    if (op.kind == Op::Kind::kPut) {
      state_[op.key] = op.value;
    } else {
      state_.erase(op.key);
    }
  }
  applied_seq_ = last;
  cv_.notify_all();
}

Status Store::put(const std::string& key, const std::string& value) {
  return commit({Op::Put(key, value)});
}

Status Store::del(const std::string& key) { return commit({Op::Del(key)}); }

std::optional<std::string> Store::get(const std::string& key) const {
  std::lock_guard<std::mutex> lk(mu_);
  auto it = state_.find(key);
  if (it == state_.end()) return std::nullopt;
  return it->second;
}

KvMap Store::scan_prefix(const std::string& prefix) const {
  std::lock_guard<std::mutex> lk(mu_);
  KvMap out;
  for (auto it = state_.lower_bound(prefix); it != state_.end(); ++it) {
    if (it->first.compare(0, prefix.size(), prefix) != 0) break;
    out.insert(*it);
  }
  return out;
}

Status Store::checkpoint() {
  std::unique_lock<std::mutex> lk(mu_);
  // Wait for in-flight commits to finish applying, so the snapshot holds every
  // record the WAL holds. Waiting for the sync is not needed: the snapshot is
  // synced before the WAL is cut, so a record only in the snapshot is not lost.
  // Waiting for the sync would also hang when a mutant never syncs.
  while (!poisoned_ && (sync_running_ || applied_seq_ < last_seq_)) {
    cv_.wait(lk);
  }
  if (poisoned_) return poisoned_status();

  Status st = write_snapshot(encode_snapshot(last_seq_, state_));
  if (st.ok()) st = wal_->cut(0);
  if (st.ok()) st = wal_->sync();
  stats_.syncs++;
  if (!st.ok()) return poison(st);
  stats_.checkpoints++;
  return Status::Ok();
}

// The order is the point: the snapshot's data is fsynced before the rename,
// and the rename is fsynced before the caller truncates the WAL.
Status Store::write_snapshot(const std::string& bytes) {
  if (mutant_is(3)) {  // mutant: write the snapshot in place
    auto snap = fs_.create_truncated("snapshot");
    if (!snap.status.ok()) return snap.status;
    Status st = snap.value->append(bytes);
    if (st.ok()) st = snap.value->sync();
    return st;
  }
  auto tmp = fs_.create_truncated("snapshot.tmp");
  if (!tmp.status.ok()) return tmp.status;
  Status st = tmp.value->append(bytes);
  if (st.ok() && !mutant_is(4)) st = tmp.value->sync();
  if (st.ok()) st = fs_.rename("snapshot.tmp", "snapshot");
  if (st.ok() && !mutant_is(1)) st = fs_.sync_dir();
  return st;
}

uint64_t Store::last_seq() const {
  std::lock_guard<std::mutex> lk(mu_);
  return applied_seq_;
}

KvMap Store::contents() const {
  std::lock_guard<std::mutex> lk(mu_);
  return state_;
}

bool Store::poisoned() const {
  std::lock_guard<std::mutex> lk(mu_);
  return poisoned_;
}

StoreStats Store::stats() const {
  std::lock_guard<std::mutex> lk(mu_);
  return stats_;
}

RecoveryInfo Store::recovery() const {
  std::lock_guard<std::mutex> lk(mu_);
  return recovery_;
}

}  // namespace csw
