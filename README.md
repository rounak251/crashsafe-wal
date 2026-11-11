# crashsafe-wal

A write-ahead-logged key-value store in C++20, with a crash checker that
enumerates the crash states of its recorded file operations and tests recovery
on each one. To run the checker, run `make crashcheck`. It needs a C++20
compiler and `make`, nothing else.

The store supports put, delete and get over an in-memory map. It is rebuilt on
open from a snapshot plus the log. Writes are acknowledged only after an fsync
covers them. The checker runs the same store code on a simulated file system
that records every write, fsync, rename and directory fsync. It then tries
each state that the crash model in `docs/crash-model.md` allows.

## Try the store by hand

```
make tools
./build/release/kv build/demo put user:1 ann
./build/release/kv build/demo scan user:
./build/release/kv build/demo stats
./build/release/wal_inspect build/demo
```

`wal_inspect` prints the frames of the WAL with the same scanner that recovery
uses, and says where a torn tail would be cut.

## Commands

| Command            | What it does                                                       |
|--------------------|--------------------------------------------------------------------|
| `make test`        | Unit and integration tests under AddressSanitizer and UBSan.       |
| `make crashcheck`  | Enumerates crash states of three seeded traces. Writes `bench/crashcheck.json` and `bench/crashcheck-points.tsv`. Exits 1 on the first bad state. |
| `make mutants`     | Builds the store with each of ten seeded bugs and checks that `crashcheck` catches each one. Writes `bench/mutants.txt` and `bench/mutants-no-continuation.txt`. |
| `make blind`       | Applies eight blind mutants (reviewer's set, `scripts/blind_mutants.py`) to copies of the source and checks that `make test` or `crashcheck` fails on each. Writes into `build/blind`. |
| `make bench`       | Put throughput and recovery time. `QUICK=1` runs the smoke mode. Writes `bench/results.json`. |
| `make loc`         | Counts non-test C++ lines with `find` and `wc`.                    |

Requires a C++20 compiler and GNU make. The numbers below were measured with
Apple clang 21 on macOS. `.github/workflows/ci.yml` is written for
`ubuntu-24.04` with the system compiler, but that workflow has not been run.
No third-party libraries.

## What the checker establishes

For each crash state, after recovery:

1. The recovered state contains every acknowledged operation.
2. It contains no operation that was never issued.
3. It equals the state after applying operations 1..j of the issued sequence
   for some j.
4. Continuation: two more puts succeed after recovery and a second crash keeps
   them, first without a checkpoint and then with one. The second pass checks
   that a leftover `snapshot.tmp` is replaced, not appended to.

For the two traces with an injected fsync failure, the driver also checks that
the failed commit returns an error, that the store is poisoned, and that the
next put is refused with the poisoned code and adds no write to the trace.

Enumeration is exhaustive for crash points whose unsynced window has at most
2 operations. Larger windows get 64 seeded samples each. The state count
formula and the sampling rule are in `docs/crash-model.md`. `scripts/recount.sh`
rebuilds each row's state count from its window text and the formula, and
`make crashcheck` fails if any row or the total disagrees.

Measured with seed 1 on the macOS build (Apple clang 21): 231,498 crash states
over the three traces. Trace A (300 operations, checkpoints, no fault) has
200,718 states; trace B (group commit, fault) has 16,518; trace C (per-op
commit, fault) has 14,262. The wall time of one run of `make crashcheck` on this
machine was 16.3 s in one run and 35.6 s in another run later on the same
day, while other work was using the machine. The per-point breakdown is in `bench/crashcheck.json` and
`bench/crashcheck-points.tsv`.

## Seeded bugs

`docs/mutants.md` lists ten bugs compiled into the store one at a time. The
table below is the output of `make mutants` on the macOS build.

| ID | Name | Result | First violation reported |
|----|------|--------|--------------------------|
| 0  | baseline (correct store) | PASS | none |
| 1  | no-dirsync-after-rename | CAUGHT | A/c279/s5 acked-op-lost |
| 2  | no-dirsync-after-create | CAUGHT | A/c0/v0 continuation-lost |
| 3  | snapshot-in-place | CAUGHT | A/c275/v1 recovery-failed |
| 4  | snapshot-no-temp-fsync | CAUGHT | A/c277/s9 recovery-failed |
| 5  | crc-payload-only | CAUGHT | A/c84/v1722 non-prefix-state |
| 6  | no-frame-crc | CAUGHT | A/c13/v75 non-prefix-state |
| 7  | torn-tail-not-truncated | CAUGHT | A/c4/v1 continuation-lost |
| 8  | ack-before-fsync | CAUGHT | A/c0/v0 continuation-lost |
| 9  | sync-error-ignored | CAUGHT | B/c75/v0 acked-op-lost |
| 10 | sync-retry | CAUGHT | B/c76/v0 acked-op-lost |

Each caught bug prints the ID of the crash state where it was found. The
checker stops at that state, so the states after it are not checked. With the
continuation check off (`--continuation-puts 0`), mutant 7 is missed and the
other nine are caught; `bench/mutants-no-continuation.txt` has that table.
The predictions made before the run, and where they differed, are in
`docs/mutants.md`.

## Independent review

The reviewer was a separate AI agent that had not seen how the code was written. No person has reviewed this project.

The reviewer read the first version and ran a set of eight blind
mutants (`B1` to `B8`, in `scripts/blind_mutants.py`). Each one is a single
source edit that the reviewer did not share with the checker's author.

- **First run, before the fixes:** 4 of 8 caught (B1, B3, B5, B6). B5 was
  caught only indirectly, as a non-prefix state at `B/c76/v37`. B2, B4, B7
  and B8 survived. The reviewer's numbers come from the reviewer's run; the
  pre-fix tree no longer exists, so they were not re-run here.
- **After the fixes:** 8 of 8 caught by `make blind` on this tree. B2 and B4
  now fail the crash check (`A/c276/v3` and `C/fault`). B8 fails the checker's
  own outcome-count check (`A/c4/model`). B1, B3, B5 and B6 are caught by the
  unit tests and by the crash check. B7 is caught only by the unit test
  `flipped_byte_in_snapshot_is_detected`.
- **Not covered:** the enumerator cannot produce B7's case, a damaged snapshot
  with the wrong length, because the model only tears writes into prefixes.
  The blind set has eight edits, all written by the reviewer. It does not
  show that the checker finds other bugs.

The review's other findings and what was done about each are in
`docs/design.md`, under "Changes after the independent review".

## Limits

- The crash model is an assumption. Real disks tear at sector size, not at any
  byte, and holes may hold stale data rather than zeros. The workload's values
  are at most 24 bytes, so no record crosses a sector. Section "Not modelled"
  in `docs/crash-model.md` lists the rest.
- Bit rot in a record that was synced is outside the model. The default damage
  policy (`kTruncate`) cuts the log at a bad frame. If an early record is
  damaged, every later acknowledged record is dropped from disk without an
  error. The `kRefuse` policy stops the store from opening instead, but a
  crash that left a hole also stops it from opening. `DamagePolicy` in
  `include/store.h` explains the trade-off.
- A frame's length is a u32. Keys and values are capped at 1 MiB, but the total
  size of a snapshot payload is not checked. A snapshot or record of 4 GiB or
  more would wrap its length field. This was not reproduced, and testing it
  needs about 4 GiB of memory.
- Nothing here is a power-cut test. Every crash is a state of the model.
- The durability difference between `fsync` and `F_FULLFSYNC` on macOS comes
  from the operating system's documentation. This project measures what the
  call costs, not what it guarantees on a given drive.
- The enumerator checks one thread. The concurrency test uses real threads but
  does not enumerate their interleavings.
- The store has no transactions, no compaction, no replication, and checkpoint
  rewrites the whole snapshot.
- The Linux build (`CI`) has not been run. The `F_FULLFSYNC` path is compiled
  only under `__APPLE__`.

## Benchmarks

Measured with `make bench` on one MacBook Air (Apple M2, Mac14,2, 16 GB RAM,
macOS 26.5.1, APFS, native disk). 4,096 puts per cell. The machine was not
quiet: other processes kept the load average near 6. Each cell shows both runs
of one `make bench`, and the raw data is in `bench/results.json`.

| Policy | Writers | ops/s | p50 ms | p99 ms | Puts per sync |
|---|---|---|---|---|---|
| per-op F_FULLFSYNC | 1 | 330, 235 | 3.0, 4.0 | 5.1, 9.5 | 1.0 |
| per-op F_FULLFSYNC | 8 | 349, 222 | 3.0, 4.0 | 4.8, 12.6 | 1.0 |
| per-op F_FULLFSYNC | 64 | 355, 237 | 3.0, 4.0 | 3,708, 6,116 | 1.0 |
| group commit | 1 | 347, 236 | 3.0, 4.0 | 4.7, 7.2 | 1.0 |
| group commit | 8 | 1,438, 996 | 5.9, 8.0 | 8.3, 16.6 | 4.2, 4.2 |
| group commit | 64 | 2,236, 1,473 | 28.8, 43.2 | 59.2, 59.0 | 6.7, 6.3 |

The spread between the two runs was 34 to 45 percent per configuration, so the
absolute ops/s figures are not precise. The ratio is steadier: group commit
gave 4.1 and 4.5 times the per-op rate at 8 writers, and 6.3 and 6.2 times at
64. That ratio follows the number of puts sharing each sync, which is what
group commit is meant to change. One writer gains nothing, as expected, since
there is nothing to share a sync with.

Per-op p99 at 64 writers is seconds while its p50 stays at 3 to 4 ms. My
reading is that writers queue on one mutex and it is not fair, so a few writers
wait through many syncs. That explanation has not been measured.

Recovery time grew about linearly with WAL size:

| Records | WAL size | Open and replay, ms |
|---|---|---|
| 10,000 | 0.4 MB | 2.8, 2.7 |
| 100,000 | 4.2 MB | 31, 34 |
| 1,000,000 | 43.8 MB | 371, 469 |

## Prior art

- ALICE (Pillai et al., "All File Systems Are Not Created Equal", OSDI 2014)
  records the system calls of real applications and explores their
  reorderings under a model of file-system behaviour. It checks each
  application against its own invariants.
- CrashMonkey (Mohan et al., "Finding Crash-Consistency Bugs with Bounded
  Black-Box Crash Testing", OSDI 2018) records block-level I/O of a real file
  system and replays bounded workloads with crashes at chosen points.

This project does less than both. The file system is a model I wrote, not a
real one. The store is one design with one workload family. The enumeration
is bounded, and above the bound it samples. What it adds is a short model that
the reader can check against the code, and seeded bugs that show the checker
distinguishes correct and broken recovery. I have not run ALICE or CrashMonkey
on this store, and no comparison of results is made here.

## Layout

| Path        | Contents                                                  |
|-------------|-----------------------------------------------------------|
| `include/`  | Public headers: store, file-system interface, framing, model, checker |
| `src/`      | Implementation                                            |
| `tools/`    | `crashcheck`, `bench`, `kv`, `wal_inspect` and `trace_dump` command-line programs |
| `tests/`    | Unit and integration tests (45 tests under `make test`)   |
| `docs/`     | `crash-model.md`, `mutants.md`, `design.md`               |
| `scripts/`  | Line count, recount, mutant runner, blind-mutant runner, benchmark wrapper |
| `bench/`    | Checker and benchmark output                              |
| `.github/`  | CI workflow (`make test`, `make crashcheck`, `make mutants`, `make loc`) |

Design decisions, the record format, the checkpoint order and answers to the
main interview questions are in `docs/design.md`.
