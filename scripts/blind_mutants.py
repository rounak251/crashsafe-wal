#!/usr/bin/env python3
"""Blind-mutant regression set for crashsafe-wal.

Each entry is one source edit, given as an exact old string and new string.
The old string must occur exactly once in the named file. The runner copies
the project into build/blind/<id>, applies the edit there, and runs two
detectors on the copy:

  unit      make test (AddressSanitizer build of the tests)
  crashcheck the crashcheck binary on the seeded traces

A mutant is CAUGHT when either detector fails. A timeout counts as a catch
(a hang is a visible failure) and is reported as HANG.

Usage: python3 scripts/blind_mutants.py [--root DIR] [--only ID ...]
"""
import os
import shutil
import subprocess
import sys

MUTANTS = [
    ("B1", "store.cpp",
     "  Status st = write_snapshot(encode_snapshot(last_seq_, state_));\n"
     "  if (st.ok()) st = wal_->cut(0);\n"
     "  if (st.ok()) st = wal_->sync();",
     "  Status st = wal_->cut(0);\n"
     "  if (st.ok()) st = wal_->sync();\n"
     "  if (st.ok()) st = write_snapshot(encode_snapshot(last_seq_, state_));",
     "checkpoint cuts the WAL before the snapshot is durable"),
    ("B2", "store.cpp",
     'auto tmp = fs_.create_truncated("snapshot.tmp");',
     'auto tmp = fs_.open_append("snapshot.tmp");',
     "snapshot.tmp is appended to instead of recreated"),
    ("B3", "store.cpp",
     "encode_snapshot(last_seq_, state_)",
     "encode_snapshot(last_seq_ - 1, state_)",
     "snapshot records one sequence number too few"),
    ("B4", "store.cpp",
     "if (!st.ok()) return poison(st);\n    synced_seq_ = last;",
     "if (!st.ok()) return st;\n    synced_seq_ = last;",
     "per-op policy does not poison after a failed fsync"),
    ("B5", "store.cpp",
     "  if (poisoned_) return poisoned_status();\n  if (ops.empty()) return Status::Ok();",
     "  if (ops.empty()) return Status::Ok();",
     "commit no longer refuses writes after poisoning"),
    ("B6", "store.cpp",
     "  uint64_t last = snap_seq;",
     "  uint64_t last = 0;",
     "recovery forgets the snapshot sequence number"),
    ("B7", "snapshot.cpp",
     "  if (!read_frame(file, 0, &payload, &next)) return false;",
     "  if (file.size() < 8) return false;\n"
     "  size_t len = get_u32(file, 0);\n"
     "  if (len > file.size() - 8) return false;\n"
     "  payload = file.substr(8, len);\n"
     "  next = 8 + len;",
     "snapshot decoder skips its frame CRC check"),
    ("B8", "crash_model.cpp",
     "if (kind == EventKind::kWrite) return bytes.size() + 1;",
     "if (kind == EventKind::kWrite) return bytes.size();",
     "checker: a write has one outcome fewer (full write never enumerated)"),
]

CHECK_DIRS = ["Makefile", "include", "src", "tools", "tests", "scripts"]


def run(cmd, cwd, timeout):
    try:
        p = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, p.stdout + p.stderr
    except subprocess.TimeoutExpired as e:
        out = (e.stdout or b"").decode() if isinstance(e.stdout, bytes) else (e.stdout or "")
        return "timeout", out


def main(argv):
    root = os.getcwd()
    only = None
    if "--root" in argv:
        root = os.path.abspath(argv[argv.index("--root") + 1])
    if "--only" in argv:
        i = argv.index("--only") + 1
        only = set()
        while i < len(argv) and not argv[i].startswith("--"):
            only.add(argv[i])
            i += 1

    base = os.path.join(root, "build", "blind")
    os.makedirs(base, exist_ok=True)
    rows = []
    for mid, rel, old, new, desc in MUTANTS:
        if only and mid not in only:
            continue
        dest = os.path.join(base, mid)
        shutil.rmtree(dest, ignore_errors=True)
        os.makedirs(dest)
        for d in CHECK_DIRS:
            src = os.path.join(root, d)
            if os.path.isdir(src):
                shutil.copytree(src, os.path.join(dest, d))
            elif os.path.isfile(src):
                shutil.copy(src, dest)

        path = os.path.join(dest, "src", rel)
        text = open(path).read()
        count = text.count(old)
        if count != 1:
            rows.append((mid, "PATCH-FAIL", "old string found %d times in %s" % (count, rel), desc))
            continue
        open(path, "w").write(text.replace(old, new))

        caught_by = []
        rc, out = run(["make", "test"], dest, 900)
        if rc == "timeout":
            caught_by.append("unit:HANG")
        elif rc != 0:
            fails = [l for l in out.splitlines() if l.startswith("FAIL ")]
            caught_by.append("unit:%d-failed" % len(fails))

        rc, _ = run(["make", "-s", "BUILD=build/release", "build/release/crashcheck"], dest, 600)
        if rc != 0:
            caught_by.append("crashcheck:build-fail")
        else:
            rc, out = run(
                ["./build/release/crashcheck", "--json", "cc.json", "--points", "cc.tsv"], dest, 900
            )
            viol = [l for l in out.splitlines() if l.startswith("VIOLATION")]
            if rc == "timeout":
                caught_by.append("crashcheck:HANG")
            elif rc == 1 and viol:
                caught_by.append("crashcheck:" + viol[0].split()[1])
            elif rc != 0:
                caught_by.append("crashcheck:exit-%s" % rc)

        status = "CAUGHT" if caught_by else "MISSED"
        rows.append((mid, status, ", ".join(caught_by) or "-", desc))
        print("%-3s %-8s %s | %s" % (mid, status, ", ".join(caught_by) or "-", desc), flush=True)

    caught = sum(1 for r in rows if r[1] == "CAUGHT")
    print("caught %d of %d" % (caught, len(rows)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
