// Put throughput and recovery time on the POSIX backend. Prints one JSON object
// on stdout; scripts/bench.sh adds the machine and source fingerprint.
//
//   bench [--quick] [--runs N] [--dir DIR]
//
// --quick is the smoke mode. It uses small counts and is not for the README.

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "posix_fs.h"
#include "store.h"

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

namespace {

struct PutResult {
  std::string policy;
  int threads = 0;
  uint64_t ops = 0;
  double ops_per_s = 0;
  double p50_us = 0;
  double p99_us = 0;
  uint64_t fullsync_fallbacks = 0;
  uint64_t syncs = 0;  // WAL fsyncs; group commit makes this smaller than ops
};

struct RecoveryResult {
  uint64_t records = 0;
  uint64_t wal_bytes = 0;
  uint64_t replayed = 0;
  double open_ms = 0;
};

struct Config {
  bool quick = false;
  std::string dir = "build/bench-data";
};

double percentile(const std::vector<double>& sorted, double p) {
  if (sorted.empty()) return 0;
  size_t idx = static_cast<size_t>(p * static_cast<double>(sorted.size() - 1));
  return sorted[idx];
}

void die(const std::string& what, const csw::Status& st) {
  std::fprintf(stderr, "bench failed: %s: %s\n", what.c_str(), st.message.c_str());
  std::exit(2);
}

// Each thread does its share of puts. Latency is measured per put, so p99 is
// the cost a single caller sees, including time spent waiting for a group.
PutResult run_put(const Config& cfg, csw::SyncPolicy policy, const char* policy_name, int threads,
                  uint64_t total_ops) {
  std::string dir = cfg.dir + "/put-" + policy_name + "-" + std::to_string(threads);
  fs::remove_all(dir);
  fs::create_directories(dir);
  csw::PosixFs file_system(dir);
  csw::StoreOptions opts;
  opts.policy = policy;
  auto opened = csw::Store::open(file_system, opts);
  if (!opened.status.ok()) die("open", opened.status);
  csw::Store& store = *opened.value;

  uint64_t per_thread = total_ops / static_cast<uint64_t>(threads);
  std::vector<std::vector<double>> latencies(static_cast<size_t>(threads));
  std::vector<csw::Status> errors(static_cast<size_t>(threads));
  std::vector<std::thread> workers;

  auto wall_start = Clock::now();
  for (int t = 0; t < threads; ++t) {
    workers.emplace_back([&, t] {
      auto& lat = latencies[static_cast<size_t>(t)];
      lat.reserve(per_thread);
      for (uint64_t i = 0; i < per_thread; ++i) {
        std::string key = "k" + std::to_string(t) + "-" + std::to_string(i);
        auto t0 = Clock::now();
        csw::Status st = store.put(key, "value-" + std::to_string(i));
        auto t1 = Clock::now();
        if (!st.ok()) {
          errors[static_cast<size_t>(t)] = st;
          return;
        }
        lat.push_back(std::chrono::duration<double, std::micro>(t1 - t0).count());
      }
    });
  }
  for (auto& w : workers) w.join();
  double wall_s = std::chrono::duration<double>(Clock::now() - wall_start).count();

  for (const auto& st : errors) {
    if (!st.ok()) die("put", st);
  }

  std::vector<double> all;
  for (const auto& lat : latencies) all.insert(all.end(), lat.begin(), lat.end());
  std::sort(all.begin(), all.end());

  PutResult r;
  r.policy = policy_name;
  r.threads = threads;
  r.ops = per_thread * static_cast<uint64_t>(threads);
  r.ops_per_s = static_cast<double>(r.ops) / wall_s;
  r.p50_us = percentile(all, 0.50);
  r.p99_us = percentile(all, 0.99);
  r.fullsync_fallbacks = csw::PosixFs::fullsync_fallbacks();
  r.syncs = store.stats().syncs;
  fs::remove_all(dir);
  return r;
}

// Writes records in batches of 64 with group commit, closes the store, and
// times the reopen. The timing covers recovery only.
RecoveryResult run_recovery(const Config& cfg, uint64_t records) {
  std::string dir = cfg.dir + "/recovery-" + std::to_string(records);
  fs::remove_all(dir);
  fs::create_directories(dir);
  RecoveryResult r;
  r.records = records;
  {
    csw::PosixFs file_system(dir);
    auto opened = csw::Store::open(file_system);
    if (!opened.status.ok()) die("open", opened.status);
    uint64_t done = 0;
    while (done < records) {
      std::vector<csw::Op> batch;
      for (uint64_t i = 0; i < 64 && done < records; ++i, ++done) {
        batch.push_back(csw::Op::Put("k" + std::to_string(done), "value-" + std::to_string(done)));
      }
      csw::Status st = opened.value->commit(batch);
      if (!st.ok()) die("commit", st);
    }
  }
  r.wal_bytes = fs::file_size(dir + "/wal.log");

  csw::PosixFs file_system(dir);
  auto t0 = Clock::now();
  auto reopened = csw::Store::open(file_system);
  auto t1 = Clock::now();
  if (!reopened.status.ok()) die("reopen", reopened.status);
  if (reopened.value->last_seq() != records) {
    std::fprintf(stderr, "bench failed: recovered %llu records, wrote %llu\n",
                 static_cast<unsigned long long>(reopened.value->last_seq()),
                 static_cast<unsigned long long>(records));
    std::exit(2);
  }
  r.replayed = reopened.value->recovery().records_replayed;
  r.open_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
  fs::remove_all(dir);
  return r;
}

struct Run {
  std::vector<PutResult> puts;
  std::vector<RecoveryResult> recovery;
};

Run one_run(const Config& cfg) {
  Run run;
  uint64_t ops = cfg.quick ? 512 : 4096;
  const int thread_counts[] = {1, 8, 64};
  for (int threads : thread_counts) {
    run.puts.push_back(run_put(cfg, csw::SyncPolicy::kPerOp, "per-op", threads, ops));
    run.puts.push_back(run_put(cfg, csw::SyncPolicy::kGroup, "group", threads, ops));
  }
  std::vector<uint64_t> sizes = cfg.quick ? std::vector<uint64_t>{10000}
                                          : std::vector<uint64_t>{10000, 100000, 1000000};
  for (uint64_t n : sizes) run.recovery.push_back(run_recovery(cfg, n));
  return run;
}

void print_run(const Run& run) {
  std::printf("{\"puts\": [");
  for (size_t i = 0; i < run.puts.size(); ++i) {
    const PutResult& p = run.puts[i];
    std::printf(
        "%s{\"policy\": \"%s\", \"threads\": %d, \"ops\": %llu, \"ops_per_s\": %.1f, "
        "\"p50_us\": %.1f, \"p99_us\": %.1f, \"syncs\": %llu, \"fullsync_fallbacks\": %llu}",
        i ? ", " : "", p.policy.c_str(), p.threads, static_cast<unsigned long long>(p.ops),
        p.ops_per_s, p.p50_us, p.p99_us, static_cast<unsigned long long>(p.syncs),
        static_cast<unsigned long long>(p.fullsync_fallbacks));
  }
  std::printf("], \"recovery\": [");
  for (size_t i = 0; i < run.recovery.size(); ++i) {
    const RecoveryResult& r = run.recovery[i];
    std::printf("%s{\"records\": %llu, \"wal_bytes\": %llu, \"replayed\": %llu, \"open_ms\": %.2f}",
                i ? ", " : "", static_cast<unsigned long long>(r.records),
                static_cast<unsigned long long>(r.wal_bytes),
                static_cast<unsigned long long>(r.replayed), r.open_ms);
  }
  std::printf("]}");
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  int runs = 2;
  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--quick") == 0) {
      cfg.quick = true;
    } else if (std::strcmp(argv[i], "--runs") == 0 && i + 1 < argc) {
      runs = std::atoi(argv[++i]);
    } else if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
      cfg.dir = argv[++i];
    } else {
      std::fprintf(stderr, "usage: bench [--quick] [--runs N] [--dir DIR]\n");
      return 2;
    }
  }
  if (runs < 1) runs = 1;

  std::vector<Run> all;
  for (int r = 0; r < runs; ++r) all.push_back(one_run(cfg));
  fs::remove_all(cfg.dir);

  // Spread across the runs, per put configuration, as a percentage of the mean
  // of the lowest and highest throughput.
  std::printf("{\"quick\": %s, \"runs\": [", cfg.quick ? "true" : "false");
  for (size_t r = 0; r < all.size(); ++r) {
    if (r) std::printf(", ");
    print_run(all[r]);
  }
  std::printf("], \"spread_pct\": [");
  for (size_t i = 0; i < all[0].puts.size(); ++i) {
    double lo = all[0].puts[i].ops_per_s;
    double hi = lo;
    for (const Run& r : all) {
      lo = std::min(lo, r.puts[i].ops_per_s);
      hi = std::max(hi, r.puts[i].ops_per_s);
    }
    double mean = (lo + hi) / 2.0;
    std::printf("%s%.1f", i ? ", " : "", mean > 0 ? 100.0 * (hi - lo) / mean : 0.0);
  }
  std::printf("]}\n");
  return 0;
}
