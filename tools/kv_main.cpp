// A small command-line front end to the store, on a real directory.
//
//   kv DIR put KEY VALUE
//   kv DIR get KEY
//   kv DIR scan PREFIX
//   kv DIR del KEY
//   kv DIR checkpoint
//   kv DIR stats
//
// Every command opens the store, which runs recovery, and closes it on exit.
// Each put or del is durable when the command prints "ok".

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

#include "posix_fs.h"
#include "store.h"

namespace {

int usage() {
  std::fprintf(stderr,
               "usage: kv DIR put KEY VALUE | get KEY | scan PREFIX | del KEY | checkpoint | stats\n");
  return 2;
}

void print_status(const csw::Status& st) {
  if (st.ok()) {
    std::printf("ok\n");
  } else {
    std::printf("error: %s\n", st.message.c_str());
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) return usage();
  std::string dir = argv[1];
  std::string cmd = argv[2];
  std::filesystem::create_directories(dir);

  csw::PosixFs file_system(dir);
  auto opened = csw::Store::open(file_system);
  if (!opened.status.ok()) {
    std::printf("error: cannot open store: %s\n", opened.status.message.c_str());
    return 1;
  }
  csw::Store& store = *opened.value;

  if (cmd == "put" && argc == 5) {
    csw::Status st = store.put(argv[3], argv[4]);
    print_status(st);
    return st.ok() ? 0 : 1;
  }
  if (cmd == "get" && argc == 4) {
    auto value = store.get(argv[3]);
    if (!value) {
      std::printf("not found\n");
      return 1;
    }
    std::printf("%s\n", value->c_str());
    return 0;
  }
  if (cmd == "scan" && argc == 4) {
    for (const auto& [key, value] : store.scan_prefix(argv[3])) {
      std::printf("%s\t%s\n", key.c_str(), value.c_str());
    }
    return 0;
  }
  if (cmd == "del" && argc == 4) {
    csw::Status st = store.del(argv[3]);
    print_status(st);
    return st.ok() ? 0 : 1;
  }
  if (cmd == "checkpoint" && argc == 3) {
    csw::Status st = store.checkpoint();
    print_status(st);
    return st.ok() ? 0 : 1;
  }
  if (cmd == "stats" && argc == 3) {
    csw::StoreStats s = store.stats();
    csw::RecoveryInfo r = store.recovery();
    std::printf("keys: %zu\nlast seq: %llu\n", store.contents().size(),
                static_cast<unsigned long long>(store.last_seq()));
    std::printf("recovery: snapshot seq %llu, replayed %llu, cut %llu bytes (stop: %s)\n",
                static_cast<unsigned long long>(r.snapshot_seq),
                static_cast<unsigned long long>(r.records_replayed),
                static_cast<unsigned long long>(r.bytes_truncated), csw::stop_reason_name(r.stop));
    std::printf("since open: commits %llu, records %llu, syncs %llu, checkpoints %llu\n",
                static_cast<unsigned long long>(s.commits),
                static_cast<unsigned long long>(s.records),
                static_cast<unsigned long long>(s.syncs),
                static_cast<unsigned long long>(s.checkpoints));
    return 0;
  }
  return usage();
}
