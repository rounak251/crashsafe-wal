// Runs the crash checker and writes its JSON report and per-point TSV.
// Exit status: 0 if every crash state passed, 1 if a violation was found,
// 2 if the run itself failed.

#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>

#include "crashcheck.h"

int main(int argc, char** argv) {
  std::string json_path = "bench/crashcheck.json";
  std::string points_path = "bench/crashcheck-points.tsv";
  csw::CheckOptions opts;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--json") == 0 && i + 1 < argc) {
      json_path = argv[++i];
    } else if (std::strcmp(argv[i], "--points") == 0 && i + 1 < argc) {
      points_path = argv[++i];
    } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
      opts.seed = std::stoull(argv[++i]);
    } else if (std::strcmp(argv[i], "--continuation-puts") == 0 && i + 1 < argc) {
      opts.continuation_puts = std::stoi(argv[++i]);
    } else {
      std::fprintf(stderr,
                   "usage: crashcheck [--json FILE] [--points FILE] [--seed N] "
                   "[--continuation-puts N]\n");
      return 2;
    }
  }

  std::ofstream points(points_path);
  points << "trace\tpoint\twindow\tmode\tstates\tops\n";

  csw::RunReport report;
  try {
    report = csw::run_crashcheck(opts, &points);
  } catch (const std::exception& e) {
    std::fprintf(stderr, "crashcheck run failed: %s\n", e.what());
    return 2;
  }
  points.close();

  std::ofstream json(json_path);
  json << csw::report_to_json(report);
  json.close();

  for (const auto& t : report.traces) {
    std::printf(
        "trace %s: events=%llu issued=%llu acked=%llu fault_fired=%s crash_points=%llu "
        "exhaustive=%llu sampled=%llu max_window=%llu states=%llu\n",
        t.name.c_str(), static_cast<unsigned long long>(t.events),
        static_cast<unsigned long long>(t.ops_issued), static_cast<unsigned long long>(t.ops_acked),
        t.fault_fired ? "yes" : "no", static_cast<unsigned long long>(t.crash_points),
        static_cast<unsigned long long>(t.exhaustive_points),
        static_cast<unsigned long long>(t.sampled_points),
        static_cast<unsigned long long>(t.max_window), static_cast<unsigned long long>(t.states));
  }
  std::printf("total states: %llu (window bound W=%d, samples S=%d, seed=%llu, continuation K=%d)\n",
              static_cast<unsigned long long>(report.states_total), opts.window_bound, opts.samples,
              static_cast<unsigned long long>(opts.seed), opts.continuation_puts);
  std::printf("wall time: %.3f s\n", report.wall_seconds);

  if (report.violation) {
    std::printf("VIOLATION %s %s: %s\n", report.violation->state_id.c_str(),
                report.violation->kind.c_str(), report.violation->detail.c_str());
    return 1;
  }
  std::printf("PASS: every enumerated crash state recovered to an acknowledged prefix\n");
  return 0;
}
