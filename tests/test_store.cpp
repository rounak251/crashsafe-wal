#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "crc32c.h"
#include "snapshot.h"
#include "fault_fs.h"
#include "posix_fs.h"
#include "record.h"
#include "sim_fs.h"
#include "store.h"
#include "wal_scan.h"
#include "test.h"

using namespace csw;
namespace fs = std::filesystem;

namespace {

// Reopens a store on a copy of the durable image, as after a crash.
std::unique_ptr<Store> reopen_durable(SimFs& source, SimFs& target) {
  target.load(source.durable_image());
  auto r = Store::open(target);
  CHECK(r.status.ok());
  return std::move(r.value);
}

}  // namespace

TEST_CASE(put_get_del_and_reopen) {
  SimFs disk;
  auto opened = Store::open(disk);
  CHECK(opened.status.ok());
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  CHECK(s.del("a").ok());
  CHECK(s.get("b").value() == "2");
  CHECK(!s.get("a").has_value());

  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK_EQ(again->last_seq(), 3u);
  CHECK(again->get("b").value() == "2");
  CHECK(!again->get("a").has_value());
}

TEST_CASE(checkpoint_then_tail_recovers) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  for (int i = 0; i < 10; ++i) CHECK(s.put("k" + std::to_string(i), "v").ok());
  CHECK(s.checkpoint().ok());
  for (int i = 5; i < 15; ++i) CHECK(s.put("k" + std::to_string(i), "w").ok());

  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK_EQ(again->last_seq(), 20u);
  CHECK(again->contents() == s.contents());
}

TEST_CASE(group_commit_shares_one_sync) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  uint64_t before = disk.file_sync_calls();
  std::vector<Op> batch;
  for (int i = 0; i < 8; ++i) batch.push_back(Op::Put("k" + std::to_string(i), "v"));
  CHECK(s.commit(batch).ok());
  CHECK_EQ(disk.file_sync_calls() - before, 1u);
}

TEST_CASE(per_op_policy_syncs_each_put) {
  SimFs disk;
  StoreOptions opts;
  opts.policy = SyncPolicy::kPerOp;
  auto opened = Store::open(disk, opts);
  Store& s = *opened.value;
  uint64_t before = disk.file_sync_calls();
  for (int i = 0; i < 8; ++i) CHECK(s.put("k" + std::to_string(i), "v").ok());
  CHECK_EQ(disk.file_sync_calls() - before, 8u);
}

TEST_CASE(torn_tail_at_every_offset_recovers_preceding_records) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "one").ok());
  CHECK(s.put("b", "two").ok());
  CHECK(s.put("c", "three").ok());
  std::string wal = disk.durable_image().at("wal.log");

  // The last record starts where the second frame ends.
  std::string_view view(wal);
  std::string_view payload;
  size_t after_first = 0;
  size_t after_second = 0;
  CHECK(read_frame(view, 0, &payload, &after_first));
  CHECK(read_frame(view, after_first, &payload, &after_second));
  KvMap expect_two{{"a", "one"}, {"b", "two"}};

  for (size_t cut = after_second; cut < wal.size(); ++cut) {
    Image image = disk.durable_image();
    image["wal.log"] = wal.substr(0, cut);
    SimFs crashed;
    crashed.load(image);
    auto r = Store::open(crashed);
    CHECK(r.status.ok());
    CHECK_EQ(r.value->last_seq(), 2u);
    CHECK(r.value->contents() == expect_two);
    // Recovery cuts the torn bytes off the file.
    CHECK_EQ(crashed.live_image().at("wal.log").size(), after_second);
  }
}

TEST_CASE(sync_failure_poisons_writer_without_retry) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK_EQ(disk.file_sync_calls(), 1u);

  disk.set_sync_fault(2);
  Status failed = s.put("b", "2");
  CHECK(failed.code == Code::kIoError);
  CHECK(disk.fault_fired());
  CHECK_EQ(disk.file_sync_calls(), 2u);
  CHECK(s.poisoned());

  size_t trace_before = disk.trace().size();
  for (int i = 0; i < 3; ++i) {
    Status st = s.put("c", "3");
    CHECK(st.code == Code::kPoisoned);
  }
  // Refused writes do not reach the file system at all: no new sync calls and
  // no new trace events, so no write, sync or rename was issued.
  CHECK_EQ(disk.file_sync_calls(), 2u);
  CHECK_EQ(disk.trace().size(), trace_before);
}

TEST_CASE(per_op_sync_failure_poisons_writer_without_retry) {
  // Per-op commits fsync inside the mutex. A failed fsync must poison the store
  // the same way, or the next put waits forever for a sequence number that
  // never applies. The checks stop before that put, so a bug fails instead of hanging.
  SimFs disk;
  StoreOptions opts;
  opts.policy = SyncPolicy::kPerOp;
  auto opened = Store::open(disk, opts);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  disk.set_sync_fault(2);
  Status failed = s.put("b", "2");
  CHECK(failed.code == Code::kIoError);
  CHECK(s.poisoned());
  CHECK(s.put("c", "3").code == Code::kPoisoned);
  CHECK_EQ(disk.file_sync_calls(), 2u);
}

TEST_CASE(checkpoint_replaces_a_leftover_snapshot_tmp) {
  // A crash between the temp file's fsync and the rename leaves a complete
  // snapshot.tmp behind. The next checkpoint must overwrite it, not append to it.
  SimFs disk;
  disk.load(Image{{"snapshot.tmp", encode_snapshot(1, KvMap{{"old", "x"}})}});
  auto opened = Store::open(disk);
  CHECK(opened.status.ok());
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.checkpoint().ok());
  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK(again->get("a").value() == "1");
  CHECK(!again->get("old").has_value());
}

TEST_CASE(flipped_byte_in_snapshot_is_detected) {
  // The snapshot's CRC is the only thing that catches damage inside the value
  // bytes. The shape of the frame stays valid, so only the check can fail.
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  CHECK(s.checkpoint().ok());
  std::string snap = disk.durable_image().at("snapshot");
  snap.back() ^= 0x01;
  SimFs bad;
  bad.load(Image{{"snapshot", snap}});
  auto r = Store::open(bad);
  CHECK(r.status.code == Code::kCorrupt);
}

TEST_CASE(damage_before_later_records_is_truncated_by_default) {
  // Documented loss: a flipped byte in an early record makes recovery cut the
  // log there, which also drops the later, acknowledged records.
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  CHECK(s.put("c", "3").ok());
  Image image = disk.durable_image();
  image["wal.log"][kFrameHeaderSize] ^= 0x01;  // a payload byte of the first record
  SimFs crashed;
  crashed.load(image);
  auto r = Store::open(crashed);
  CHECK(r.status.ok());
  CHECK_EQ(r.value->last_seq(), 0u);
  CHECK_EQ(r.value->recovery().bytes_truncated, image["wal.log"].size());
}

TEST_CASE(damage_before_later_records_refuses_under_refuse_policy) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  Image image = disk.durable_image();
  image["wal.log"][kFrameHeaderSize] ^= 0x01;
  SimFs crashed;
  crashed.load(image);
  StoreOptions strict;
  strict.damage = DamagePolicy::kRefuse;
  auto r = Store::open(crashed, strict);
  CHECK(r.status.code == Code::kCorrupt);
  // Nothing was cut: the file is exactly as it was.
  CHECK_EQ(crashed.live_image().at("wal.log").size(), image["wal.log"].size());
}

TEST_CASE(torn_tail_is_cut_even_under_refuse_policy) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  Image image = disk.durable_image();
  image["wal.log"] += std::string("\x05\x00", 2);
  SimFs crashed;
  crashed.load(image);
  StoreOptions strict;
  strict.damage = DamagePolicy::kRefuse;
  auto r = Store::open(crashed, strict);
  CHECK(r.status.ok());
  CHECK_EQ(r.value->last_seq(), 1u);
}

TEST_CASE(oversized_value_is_refused_before_anything_is_written) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  size_t trace_before = disk.trace().size();
  std::string big(kMaxFieldBytes + 1, 'x');
  CHECK(s.put("a", big).code == Code::kInvalidArgument);
  CHECK_EQ(disk.trace().size(), trace_before);
  CHECK(s.put("a", std::string(kMaxFieldBytes, 'x')).ok());
}

TEST_CASE(unacknowledged_record_is_not_visible_after_reopen) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  disk.set_sync_fault(2);
  CHECK(!s.put("b", "2").ok());
  // The failed fsync dropped the unsynced record, so the reopened store lacks it.
  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK(again->get("a").value() == "1");
  CHECK(!again->get("b").has_value());
}

TEST_CASE(corrupt_snapshot_is_an_error) {
  SimFs disk;
  disk.load(Image{{"snapshot", "not a snapshot"}});
  auto r = Store::open(disk);
  CHECK(r.status.code == Code::kCorrupt);
}

TEST_CASE(concurrent_writers_on_posix_keep_every_acknowledged_put) {
  std::string dir = "build/test-data/concurrent";
  fs::remove_all(dir);
  fs::create_directories(dir);
  const int kThreads = 8;
  const int kPerThread = 50;
  {
    PosixFs file_system(dir);
    auto opened = Store::open(file_system);
    CHECK(opened.status.ok());
    Store& s = *opened.value;
    std::vector<std::thread> workers;
    std::vector<Status> results(kThreads * kPerThread);
    for (int t = 0; t < kThreads; ++t) {
      workers.emplace_back([&, t] {
        for (int i = 0; i < kPerThread; ++i) {
          results[static_cast<size_t>(t * kPerThread + i)] =
              s.put("t" + std::to_string(t) + "-" + std::to_string(i), std::to_string(i));
        }
      });
    }
    for (auto& w : workers) w.join();
    for (const Status& st : results) CHECK(st.ok());
  }
  PosixFs file_system(dir);
  auto reopened = Store::open(file_system);
  CHECK(reopened.status.ok());
  CHECK_EQ(reopened.value->last_seq(), static_cast<uint64_t>(kThreads * kPerThread));
  CHECK_EQ(reopened.value->contents().size(), static_cast<size_t>(kThreads * kPerThread));
  fs::remove_all(dir);
}

TEST_CASE(posix_checkpoint_and_reopen) {
  std::string dir = "build/test-data/posix";
  fs::remove_all(dir);
  fs::create_directories(dir);
  {
    PosixFs file_system(dir);
    auto opened = Store::open(file_system);
    Store& s = *opened.value;
    CHECK(s.put("x", "1").ok());
    CHECK(s.put("y", "2").ok());
    CHECK(s.checkpoint().ok());
    CHECK(s.put("z", "3").ok());
    CHECK(s.del("x").ok());
  }
  PosixFs file_system(dir);
  auto reopened = Store::open(file_system);
  CHECK(reopened.status.ok());
  CHECK(!reopened.value->get("x").has_value());
  CHECK(reopened.value->get("y").value() == "2");
  CHECK(reopened.value->get("z").value() == "3");
  fs::remove_all(dir);
}

TEST_CASE(stats_count_commits_records_and_syncs) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  std::vector<Op> batch{Op::Put("b", "2"), Op::Put("c", "3"), Op::Del("a")};
  CHECK(s.commit(batch).ok());
  CHECK(s.checkpoint().ok());
  StoreStats st = s.stats();
  CHECK_EQ(st.commits, 2u);
  CHECK_EQ(st.records, 4u);
  CHECK_EQ(st.syncs, 3u);  // two commits and the checkpoint's WAL cut
  CHECK_EQ(st.checkpoints, 1u);
}

TEST_CASE(recovery_reports_replayed_records_and_cut_tail) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  CHECK(s.checkpoint().ok());
  CHECK(s.put("c", "3").ok());
  std::string wal = disk.durable_image().at("wal.log");
  Image image = disk.durable_image();
  image["wal.log"] = wal + std::string("\x05\x00", 2);  // a torn header after a good record

  SimFs crashed;
  crashed.load(image);
  auto r = Store::open(crashed);
  CHECK(r.status.ok());
  RecoveryInfo info = r.value->recovery();
  CHECK_EQ(info.snapshot_seq, 2u);
  CHECK_EQ(info.records_replayed, 1u);
  CHECK_EQ(info.bytes_truncated, 2u);
  CHECK(info.stop == StopReason::kTornFrame);
}

TEST_CASE(failed_append_poisons_writer_and_is_not_recovered) {
  SimFs disk;
  FaultFs faulty(disk);
  auto opened = Store::open(faulty);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  faulty.fail_nth(FaultOp::kAppend, faulty.calls(FaultOp::kAppend) + 1);
  CHECK(s.put("b", "2").code == Code::kIoError);
  CHECK(s.put("c", "3").code == Code::kPoisoned);
  // The failed append never reached the disk, so it is not recovered.
  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK(again->get("a").value() == "1");
  CHECK(!again->get("b").has_value());
  CHECK(!again->get("c").has_value());
}

TEST_CASE(failed_snapshot_rename_keeps_every_acknowledged_put) {
  SimFs disk;
  FaultFs faulty(disk);
  auto opened = Store::open(faulty);
  Store& s = *opened.value;
  CHECK(s.put("a", "1").ok());
  CHECK(s.put("b", "2").ok());
  faulty.fail_nth(FaultOp::kRename, faulty.calls(FaultOp::kRename) + 1);
  CHECK(!s.checkpoint().ok());
  CHECK(s.poisoned());
  // The WAL was not cut, so the old state is still there after a crash.
  SimFs copy;
  auto again = reopen_durable(disk, copy);
  CHECK_EQ(again->last_seq(), 2u);
  CHECK(again->get("a").value() == "1");
  CHECK(again->get("b").value() == "2");
}

TEST_CASE(failed_directory_sync_at_open_is_reported) {
  SimFs disk;
  FaultFs faulty(disk);
  faulty.fail_nth(FaultOp::kSyncDir, 1);
  auto r = Store::open(faulty);
  CHECK(r.status.code == Code::kIoError);
}

TEST_CASE(scan_prefix_returns_matching_keys_in_order) {
  SimFs disk;
  auto opened = Store::open(disk);
  Store& s = *opened.value;
  CHECK(s.put("user:2", "b").ok());
  CHECK(s.put("order:1", "x").ok());
  CHECK(s.put("user:1", "a").ok());
  CHECK(s.put("usz", "z").ok());
  KvMap users = s.scan_prefix("user:");
  CHECK_EQ(users.size(), 2u);
  CHECK(users.begin()->first == "user:1");
  CHECK(s.scan_prefix("nothing").empty());
  CHECK_EQ(s.scan_prefix("").size(), 4u);
}
