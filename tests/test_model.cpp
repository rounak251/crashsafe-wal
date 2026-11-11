#include <string>

#include "crash_model.h"
#include "fs.h"
#include "sim_fs.h"
#include "test.h"

using namespace csw;

namespace {

// Choice that applies every window operation in full.
std::vector<uint64_t> all_persist(const CrashModel& m) {
  std::vector<uint64_t> choice;
  for (const WindowOp& w : m.window()) {
    choice.push_back(w.kind == EventKind::kWrite ? w.bytes.size() : 1);
  }
  return choice;
}

}  // namespace

TEST_CASE(unsynced_data_is_not_durable) {
  SimFs fs;
  auto f = fs.open_append("a");
  CHECK(f.status.ok());
  CHECK(f.value->append("x").ok());
  CHECK(fs.sync_dir().ok());  // the name is durable, the data is not
  CHECK(fs.model().durable().count("a") == 1);
  CHECK(fs.model().durable().at("a").empty());
  CHECK(fs.live_image().at("a") == "x");
  CHECK(f.value->sync().ok());
  CHECK(fs.model().durable().at("a") == "x");
}

TEST_CASE(unsynced_name_is_not_durable) {
  SimFs fs;
  auto f = fs.open_append("a");
  CHECK(f.value->append("x").ok());
  CHECK(f.value->sync().ok());
  CHECK(fs.model().durable().empty());  // the data is synced but the name is not
  CHECK(fs.sync_dir().ok());
  CHECK(fs.model().durable().at("a") == "x");
}

TEST_CASE(rename_without_dir_sync_can_be_dropped) {
  SimFs fs;
  CHECK(fs.open_append("tmp").status.ok());
  CHECK(fs.sync_dir().ok());
  auto f = fs.create_truncated("tmp");
  CHECK(f.value->append("new").ok());
  CHECK(f.value->sync().ok());
  CHECK(fs.rename("tmp", "snap").ok());
  CHECK_EQ(fs.model().window().size(), 1u);
  Image dropped = fs.model().image({0});
  Image kept = fs.model().image({1});
  CHECK(dropped.count("tmp") == 1 && dropped.count("snap") == 0);
  CHECK(kept.count("snap") == 1 && kept.count("tmp") == 0);
  CHECK(kept.at("snap") == "new");
}

TEST_CASE(torn_write_outcomes_are_prefixes) {
  SimFs fs;
  auto f = fs.open_append("w");
  CHECK(fs.sync_dir().ok());  // the create is durable, so the window holds only the write
  CHECK(f.value->append("hello").ok());
  CHECK_EQ(fs.model().window().size(), 1u);
  CHECK_EQ(fs.model().window()[0].options(), 6u);
  for (uint64_t k = 0; k <= 5; ++k) {
    CHECK(fs.model().image({k}).at("w") == std::string("hello").substr(0, k));
  }
}

TEST_CASE(failed_fsync_drops_unsynced_data) {
  SimFs fs;
  fs.set_sync_fault(1);
  auto f = fs.open_append("w");
  CHECK(fs.sync_dir().ok());
  CHECK(f.value->append("lost").ok());
  CHECK(!f.value->sync().ok());
  CHECK(fs.fault_fired());
  CHECK(fs.live_image().at("w").empty());
  CHECK(fs.model().window().empty());
  CHECK(f.value->sync().ok());  // a later fsync succeeds, but the data is gone
  CHECK(fs.live_image().at("w").empty());
}

TEST_CASE(all_persisted_image_equals_live_view) {
  // Random histories of every kind of operation. With every window operation
  // applied, the crash image must equal the live view.
  for (uint64_t seed = 1; seed <= 20; ++seed) {
    SimFs fs;
    uint64_t state = seed * 2654435761u;
    auto next = [&state](uint64_t n) {
      state = state * 6364136223846793005ull + 1442695040888963407ull;
      return (state >> 33) % n;
    };
    std::vector<std::unique_ptr<File>> handles;
    for (int step = 0; step < 60; ++step) {
      std::string name = "f" + std::to_string(next(3));
      switch (next(6)) {
        case 0: {
          auto f = fs.open_append(name);
          if (f.status.ok()) handles.push_back(std::move(f.value));
          break;
        }
        case 1:
          if (!handles.empty()) handles[next(handles.size())]->append(std::string(next(9) + 1, 'a'));
          break;
        case 2:
          if (!handles.empty()) handles[next(handles.size())]->sync();
          break;
        case 3:
          if (!handles.empty()) handles[next(handles.size())]->truncate(next(5));
          break;
        case 4:
          fs.rename(name, "f" + std::to_string(next(3)));
          break;
        case 5:
          fs.sync_dir();
          break;
      }
    }
    CHECK(fs.model().image(all_persist(fs.model())) == fs.live_image());
  }
}

TEST_CASE(write_after_dropped_truncate_keeps_its_issued_offset) {
  // The write was issued after the truncate, so it was placed at offset 0 in
  // the live file. If only the write persists, it overwrites the start of the
  // old 100 bytes. The rest of the old bytes stay, because the truncate was dropped.
  SimFs fs;
  auto f = fs.open_append("w");
  CHECK(f.value->append(std::string(100, 'o')).ok());
  CHECK(f.value->sync().ok());
  CHECK(fs.sync_dir().ok());
  CHECK(f.value->truncate(0).ok());
  CHECK(f.value->append("abc").ok());
  CHECK_EQ(fs.model().window().size(), 2u);
  // Outcome 0 drops the truncate, outcome 1 keeps it. The write is the second op.
  Image kept_write_only = fs.model().image({0, 3});
  CHECK_EQ(kept_write_only.at("w").size(), 100u);
  CHECK(kept_write_only.at("w").substr(0, 3) == "abc");
}

TEST_CASE(replay_prefix_matches_incremental_model) {
  SimFs fs;
  auto f = fs.open_append("log");
  CHECK(f.value->append("one").ok());
  CHECK(fs.sync_dir().ok());
  CHECK(f.value->append("two").ok());
  CHECK(f.value->sync().ok());
  CHECK(fs.rename("log", "moved").ok());
  CHECK(fs.sync_dir().ok());
  const std::vector<Event>& trace = fs.trace();
  CrashModel incremental;
  for (size_t n = 0; n <= trace.size(); ++n) {
    CrashModel replayed = replay_prefix(trace, n);
    CHECK(replayed.window().size() == incremental.window().size());
    CHECK(replayed.live() == incremental.live());
    if (n < trace.size()) incremental.apply(trace[n]);
  }
  CHECK(replay_prefix(trace, trace.size()).live() == fs.live_image());
}

TEST_CASE(durable_image_drops_every_window_operation) {
  SimFs fs;
  auto f = fs.open_append("w");
  CHECK(f.value->append("abc").ok());
  CHECK(f.value->sync().ok());
  CHECK(fs.sync_dir().ok());
  CHECK(f.value->append("def").ok());
  CHECK(fs.model().durable().at("w") == "abc");
  CHECK(fs.durable_image().at("w") == "abc");
  CHECK(fs.live_image().at("w") == "abcdef");
}
