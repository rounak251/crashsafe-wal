# Design

## Layout

| Path                    | Role                                                       |
|-------------------------|------------------------------------------------------------|
| `include/`              | Headers: `status.h`, `fs.h`, `store.h`, `le.h`, `crc32c.h`, `record.h`, `wal_scan.h`, `snapshot.h`, `posix_fs.h`, `fault_fs.h`, `sim_fs.h`, `crash_model.h`, `crashcheck.h`, `mutant.h` |
| `src/`                  | Implementation of the headers above                        |
| `tools/`                | `crashcheck_main`, `bench_main`, `kv_main`, `wal_inspect`, `trace_dump` |
| `tests/`                | Unit and integration tests (not counted in the line budget)|
| `scripts/`              | `loc.sh`, `recount.sh`, `run_mutants.sh`, `blind_mutants.py`, `bench.sh`, `source_sha256.sh` |
| `bench/`                | Output of `make crashcheck`, `make mutants` and `make bench` |
| `docs/`                 | This file, `crash-model.md`, `mutants.md`                  |

## Record format

A WAL record and a snapshot are both stored as frames:

```
frame  = length (u32 LE) | crc (u32 LE) | payload (length bytes)
crc    = crc32c( length bytes ++ payload )
```

The CRC covers the length field. This matters because a zero-filled region
(a hole left by a dropped write) reads as length 0 with CRC field 0. The CRC
over an empty payload is 0, so a CRC over the payload alone would accept such
a hole as a valid empty frame. The CRC over the length is CRC32C of four zero
bytes, which is 0x48674BC7 and not 0. `zero_bytes_have_nonzero_crc` checks that
exact value, and `crc32c_known_vector` checks the standard check value
0xE3069283 for "123456789". Both were also computed by a separate Python
implementation.

An empty payload is a skip frame. The writer never emits one. The reader
skips it only when its CRC matches, which zeros never do.

Record payload: `seq (u64) | op (u8: 1 put, 2 del) | klen (u32) | key | vlen (u32) | value`.
A delete has vlen 0. A key or value over 1 MiB (`kMaxFieldBytes`) is refused
before anything is written, so every length fits its u32 field.

Snapshot payload: `seq (u64) | count (u32) | { klen | key | vlen | value } * count`.
A snapshot is valid only if its frame covers the whole file and the payload
parses exactly.

Frame reading lives in one function, `read_frame_checked`, which returns why it
stopped (`kOk`, `kTorn`, `kBadCrc`). Recovery and `wal_inspect` both use
`scan_wal`, which calls it, so the inspector shows what recovery accepts.

## Durability policy

- **Group commit (default).** `Store::commit` appends the records of one call
  under a mutex. Then one caller at a time becomes the sync leader, runs
  fsync outside the mutex, and marks everything appended so far as synced.
  Other callers wait on a condition variable until their sequence number is
  synced. Writers that share a sync are the group.
- **Per-op.** Each commit appends and fsyncs while holding the mutex. This is
  the baseline for `make bench`.
- **macOS.** `F_FULLFSYNC` is used. If `fcntl` fails, the code logs the error
  and falls back to `fsync`. The fallback count is printed by the benchmark.
- **Other systems.** `fsync`.

## Apply order

Records are applied to the memtable in sequence order. In group commit, a
caller waits until the previous sequence numbers are applied before it applies
its own. The memtable is a `std::map`.

## Checkpoint

Under the store mutex, with no sync in progress:

1. Create or truncate `snapshot.tmp`, append the snapshot frame, fsync it.
2. Rename `snapshot.tmp` to `snapshot`, fsync the directory.
3. Truncate `wal.log` to zero, fsync it.

A crash at any point leaves either the old snapshot with the full WAL, or the
new snapshot with a WAL whose records are all at or below the snapshot's
sequence number. Recovery skips records at or below the snapshot sequence.

`create_truncated` is required in step 1. A leftover complete `snapshot.tmp`
from a crash before step 2 must be overwritten, not appended to. The unit test
`checkpoint_replaces_a_leftover_snapshot_tmp` checks this directly.

## Recovery

1. If `snapshot` exists, read it. An invalid snapshot is an error, not an
   empty state. Silently ignoring it would lose data.
2. Scan `wal.log` frame by frame. Stop at the first frame that is incomplete,
   fails its CRC or does not parse. Apply each record whose sequence number is
   above the current one.
3. If the scan stopped at a bad frame (not an incomplete one) with bytes after
   its header, the damage policy decides. See below.
4. Otherwise, if the scan stopped before the end of the file, truncate the WAL
   to the end of the last good frame and fsync it.
5. Open the WAL for appending. If it was created, fsync the directory.

The reader does not check that sequence numbers are contiguous. The CRC over
the length is what stops at a zero hole. The checker's oracle checks that the
state is a prefix.

### What recovery does with damage

A crash can leave a zeroed hole followed by unacknowledged records, because
unsynced writes may persist out of order. Recovery must cut the log at the hole.
Damage to a record that was synced and acknowledged looks the same in the
bytes: a bad frame, then more data. Recovery cannot tell these apart from the
frame alone. So the store has two policies (`DamagePolicy` in `store.h`):

- **kTruncate (default).** Cut the log at the bad frame. A crash is handled
  correctly. Damage to an early synced record also drops every later
  acknowledged record. `damage_before_later_records_is_truncated_by_default`
  shows this, and `RecoveryInfo` reports the bytes cut and the stop reason.
  The store does not detect bit rot.
- **kRefuse.** Fail to open with `kCorrupt` if a bad frame has bytes after its
  header. Nothing is dropped. The cost is that a crash that left a hole also
  stops the store from opening, and an operator has to decide. A torn tail is
  still cut under this policy, because a torn tail has no later bytes.

The crash checker runs the default policy, because its model produces holes.

## Poisoned writer

A failed fsync poisons the store. Every later commit returns `kPoisoned`
without touching the file, and the sync call count does not change. The caller
must close the store and reopen it, which runs recovery on what is durable.
The writer never retries an fsync. After a failed fsync the kernel may have
dropped the dirty pages, so a retry that succeeds says nothing about the data.

This holds on both sync paths. `per_op_sync_failure_poisons_writer_without_retry`
covers per-op, and `sync_failure_poisons_writer_without_retry` covers group
commit. The second also checks that a refused put adds no trace events, so no
write reaches the file.

## Crash checker

`src/crashcheck.cpp` runs the workload on `SimFs`, takes the trace, walks the
crash points, builds each crash image with `CrashModel`, runs recovery and the
oracle, and then the two continuation checks. The model and the counting rule
are in `docs/crash-model.md`.

Three traces run: A (group commit with checkpoints, no fault), B (group
commit, fault in the 5th fsync), C (per-op commit, fault in the 5th fsync).
The fault traces also run driver checks: the failed commit must return an
error, the store must be poisoned, the next put must return `kPoisoned`, and
that put must add no write event.

## Measured versus claimed

- Measured by `make test`: the frame format and its exact CRC values, recovery
  of torn tails at every byte offset of the last record, the poisoned-writer
  rule on both sync paths, the damage policies, the snapshot CRC against a
  flipped byte, the leftover `snapshot.tmp` case, the size cap, group commit
  with eight writer threads on the POSIX backend, and the error paths through
  `FaultFs`.
- Measured by `make crashcheck` and `make mutants`: the enumerated crash-state
  count and its independent recount, the oracle verdict on the correct store,
  and the verdict on each of the ten seeded bugs, with and without the
  continuation check.
- Measured by `scripts/blind_mutants.py`: the reviewer's eight blind mutants,
  on the first run and after the fixes. The results are in the README.
- Not measured: power-loss behaviour. The `F_FULLFSYNC` guarantee is taken from
  the macOS documentation. This project measured its latency, not its
  durability.

## Interview questions

**Why does a rename need a directory fsync to be durable, and what does SimFs
model about it?**
A rename changes a directory entry. The data of the renamed file and the
directory entry are separate. Until the directory is fsynced, the rename may
or may not survive a crash, while the file's own fsync says nothing about the
name. SimFs keeps namespace operations in their own window, separate from the
data windows, and lets each rename survive or not, independently of the data
it moves. So the checker can produce "new name, old data" and "old name, new
data" states.

**What does F_FULLFSYNC guarantee that fsync does not on macOS, and what did
you measure?**
On macOS, `fsync` asks the kernel to write the data to the drive, but the
drive may keep it in its volatile cache. `F_FULLFSYNC` also asks the drive to
flush that cache. This comes from the macOS `fcntl(2)` documentation. The
project measured the latency of each policy, not the durability difference;
there is no power-cut test.

**What breaks if the CRC covers only the payload and not the length?**
A zero-filled hole reads as a frame with length 0 and CRC 0. The CRC over an
empty payload is 0, so the frame passes. The reader skips it and goes on to
the next record, which is then applied while the one before it is missing.
This is mutant 5. The checker reports it as a non-prefix state, at crash point
84 of trace A, where the window holds two writes of 40 and 42 bytes, so 1,763
outcome assignments exist and the first bad one is number 1,722.

**Which of your 10 mutants was hardest to catch, and what did the first checker
version miss?**
Mutant 7 (torn tail not truncated) is the hardest. Its first recovery is
correct. The bug only shows when the store writes after recovery and crashes
again: the new records go after the garbage and the second recovery stops at
the garbage. Without the continuation check (`bench/mutants-no-continuation.txt`)
the oracle alone misses it. The first version of the checker also had no
second-crash check after a checkpoint, so it missed the leftover `snapshot.tmp`
bug; see the review section below.

**Why must the writer not retry after a failed sync?**
After a failed fsync, the kernel may mark the dirty pages clean and drop them.
A later fsync can then succeed without writing anything, so success does not
mean the data reached the disk. Mutant 10 shows the loss: the retry succeeds,
the batch is acknowledged, and the checker finds the acknowledged records
missing.

**What in your crash model could be wrong about real disks?**
Real disks can tear at sector size, not at any byte. Holes may contain stale
data, not zeros. Drive caches can reorder writes across files in ways the model
does not allow. The model also assumes that a failed fsync loses all unsynced
data of that file, which is a worst case, not a measured fact about any disk.
Bit rot in synced records is outside the model entirely; the damage policy
section above says what the store does about it.

## Changes after first run

### Change 1: checkpoint waits for applies, not for the sync

The first draft of `Store::checkpoint` waited until every appended record was
synced and applied. Under mutant 8 (acknowledge before fsync) no commit ever
runs the group sync, so the wait never ended, and `make mutants` hung on that
build. The wait now covers only the in-flight leader sync and the applies. The
snapshot is still fsynced before the WAL is cut, so a record that is in the
snapshot but not yet synced is not lost. The correct build behaves the same:
an applied record is always synced, so the change does not alter the state
machine of the correct store.

### Change 2: the shared WAL scanner and the window histogram

Recovery and `wal_inspect` use `scan_wal`, so the inspector shows exactly what
recovery accepts. The checker's JSON report has a per-trace `window_histogram`,
so the state-count formula can be checked by hand against the number of crash
points in each window size. Neither change alters the crash model or the count
formula.

## Changes after the independent review

An independent review of the first version listed blockers and majors. Each
one was checked against the code. The table gives the decision and the
evidence.

| Review item | Decision | What changed and how it is checked |
|-------------|----------|------------------------------------|
| Recovery silently drops acknowledged records after a bad frame (major) | Confirmed as a real trade-off, not as a checker gap. Kept the default, added an explicit policy. | `DamagePolicy::kRefuse`, tests `damage_before_later_records_is_truncated_by_default` and `..._refuses_under_refuse_policy`. The default stays because the crash model produces holes that must be cut. Stated in the README and in "Not modelled". |
| Continuation never checkpoints, so mutant B2 (append to `snapshot.tmp`) survives (major) | Confirmed. | The continuation now runs a second pass that checkpoints, on the same image. The first pass still reopens without a checkpoint, because a checkpoint hides torn tails (see the next note). Test `checkpoint_replaces_a_leftover_snapshot_tmp`. |
| Per-op sync failure does not poison; B4 hangs the next put (major) | Confirmed. | The driver runs a fault trace on per-op commits (trace C) and checks poisoning before it issues another put, so a bug fails instead of hanging. Test `per_op_sync_failure_poisons_writer_without_retry`. |
| Poisoned-writer rule is checked only through its side effect; B5 is caught only indirectly (major) | Confirmed. | Driver check `write-after-poison` and the trace check in `sync_failure_poisons_writer_without_retry`. |
| CRC constant in `design.md` is wrong (major) | Confirmed. 4 zero bytes give 0x48674BC7; 0x8A9136AA is the 32-byte value. | Corrected. `zero_bytes_have_nonzero_crc` now checks the exact value. Checked with a separate Python implementation. |
| README claims a GCC 12 Debian measurement that is not in the repo (major) | Confirmed. | README now states only what was measured: Apple clang 21 on this machine. |
| `recount.sh` re-sums the enumerator's own columns (minor) | Confirmed. | It rebuilds every row from its window in the TSV and the formula, and checks each row's window, mode and states. Under mutant B8 the enumerator reports `model-outcome-count` first, so `make crashcheck` fails before recount runs. Recount alone does not catch B8, because the window text does not depend on the outcome count. |
| Torn writes are prefixes only; snapshot CRC (B7) unexercised (minor) | Confirmed for the checker. Disagreed with the fix the review implies. | The model cannot produce a wrong-length snapshot, so B7 is outside what the enumerator can see. A unit test flips a snapshot byte instead. Stated in "Not modelled". |
| Write placement uses the absolute offset, so a dropped truncate overwrites old bytes (minor) | Disagreed, with evidence. | Replaying writes at end-of-file (the reviewer's suggestion) breaks the correct build: a dropped middle write lets the later frame move up into the gap, and recovery applies it out of order (`crashcheck` reported `non-prefix-state` at `A/c13/v1457`). The issued offset is what a file system does for a write that was placed when it was issued. Test `write_after_dropped_truncate_keeps_its_issued_offset`. In the second review this was re-run in a scratch copy with end-of-file placement, and `crashcheck` reported `non-prefix-state` at `A/c13/v1457` (recovered=4, acked=2, issued=4). |
| Checkpoint cadence and Ack/sync wording in `crash-model.md` (minor) | Confirmed, both fixed. | Cadence is "first batch boundary with 75 or more since the last checkpoint": after acknowledged 82, 164, 246. The Ack paragraph now says the two oracle forms agree on the correct build. |
| Mutants 2 and 8 reported at c0 without measuring the oracle alone (minor) | Confirmed. | `make mutants` now writes a second table with `--continuation-puts 0`. Its numbers are in `docs/mutants.md`. |
| `SimFs` has no prefix replay (minor) | Confirmed. | `replay_prefix` in `sim_fs.cpp`, with test `replay_prefix_matches_incremental_model`. |
| Duplicated helpers, and a `wal_scan.h` comment that overclaimed (minor) | Confirmed. | `include/le.h` holds the integer helpers. `read_frame_checked` is the one frame reader; `scan_wal` calls it. |
| VIOLATION line has no separator (cosmetic) | Confirmed. | Fixed in `tools/crashcheck_main.cpp`. |
| Unbounded field length could wrap a u32 length (not reproduced by the review) | Agreed it is a real hazard, though it needs 4 GiB. | `kMaxFieldBytes` refuses fields over 1 MiB before anything is written. Test `oversized_value_is_refused_before_anything_is_written`. |
| The spec's replay, cloc and spread criteria | Not changed here. | See the README: the spread criterion is deferred by the amendment, and cloc is absent, so `loc.sh` is used. |
| Blind mutants B1 to B8 (the review's set) | First run: 4 of 8 caught (B1, B3, B5, B6), as the reviewer reported. After the fixes: 8 of 8 caught by `make blind`. | B2 and B4 needed checker changes: the continuation checkpoint pass, and trace C with its driver checks. B8 needed the outcome-count check. B7 is caught only by the unit test `flipped_byte_in_snapshot_is_detected`. The first-run numbers were not re-run, because the pre-fix tree no longer exists. |

### Why the continuation has two passes

The first version of the continuation put keys, checkpointed, crashed and
reopened. Adding a checkpoint at every state made the reviewer's B2 case visible,
but it also hid mutant 7. A checkpoint rewrites the whole state and cuts the
log, so the garbage that the reopen should have hit disappears. So the log pass
(no checkpoint) runs first, and the checkpoint pass runs on the same image.
Both must pass.
