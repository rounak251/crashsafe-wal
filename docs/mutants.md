# Seeded mutants

Ten deliberate bugs are compiled into the store, one at a time, with
`-DMUTANT_ID=n`. The switch is `mutant_is(n)` in `include/mutant.h`. Each mutant
must make `make crashcheck` fail with a violation line that names a crash
state. The unmutated build (MUTANT_ID 0) must pass.

This list was written before the first run of the checker against any
mutant. The file's history is in the project's file timestamps only; there is
no version control in this build, so the list cannot prove its own ordering.

## The ten mutants

| ID | Name                     | What the mutant does                                              | Where the bug lives        |
|----|--------------------------|-------------------------------------------------------------------|----------------------------|
| 1  | no-dirsync-after-rename  | Checkpoint renames the snapshot but never fsyncs the directory.   | `Store::checkpoint`        |
| 2  | no-dirsync-after-create  | A new WAL file is created but the directory is not fsynced.       | `Store::open`              |
| 3  | snapshot-in-place        | Checkpoint writes `snapshot` directly, with no temp file and no rename. | `Store::checkpoint`   |
| 4  | snapshot-no-temp-fsync   | Checkpoint renames the temp snapshot before its data is fsynced.  | `Store::checkpoint`        |
| 5  | crc-payload-only         | Frame CRC covers the payload only, not the length field.          | `record.cpp` encode/decode |
| 6  | no-frame-crc             | Recovery does not check the frame CRC at all.                     | `record.cpp` decode        |
| 7  | torn-tail-not-truncated  | Recovery leaves a torn or corrupt tail in the WAL.                | `Store::open`              |
| 8  | ack-before-fsync         | `commit` returns success before the sync covering its records.    | `Store::commit`            |
| 9  | sync-error-ignored       | A failed group fsync is ignored and the batch is acknowledged.    | `Store::commit`            |
| 10 | sync-retry               | A failed group fsync is retried once and the batch is acknowledged if the retry succeeds. | `Store::commit` |

## Expected detection

For each mutant, the note says which crash states should show the bug. The
run is the evidence; these notes are predictions made before the run.

1. After the WAL is truncated and synced, the snapshot rename is still in the
   window. Dropping the rename loses every record up to the last checkpoint.
   Expected violation: acked-op-lost.
2. The first group sync makes the data durable, but the WAL name is still in
   the window. Dropping the name loses the acknowledged records. Expected:
   acked-op-lost.
3. A crash during the in-place snapshot write leaves a torn snapshot. Expected:
   recovery-failed.
4. After the directory fsync, the rename is durable but the temp data is not.
   A dropped write leaves an empty snapshot. Expected: recovery-failed.
5. A batch of two records where the first is dropped leaves a zero hole
   followed by the second record. The zero frame has CRC 0 over an empty
   payload, so it is accepted as an empty frame and skipped, and the second
   record is applied. Expected: non-prefix-state.
6. Same zero-hole case as 5, with no CRC check at all. Expected:
   non-prefix-state.
7. A torn record stays at the end of the WAL. Recovery is correct for that
   crash, so the first check passes. The continuation writes go after the
   garbage, and the second recovery stops at the garbage. Expected:
   continuation-lost.
8. Acknowledged records are only in the window until the next checkpoint.
   Expected: acked-op-lost.
9. The failed batch is acknowledged but its data was discarded by the failed
   fsync. Expected: acked-op-lost in trace B.
10. The retry succeeds on an empty window, so the batch is acknowledged while
    its data is gone. Expected: acked-op-lost in trace B.

## Mutants the model cannot distinguish

None are known at the time of writing. If the run shows one, it is recorded in
`docs/design.md` under "Changes after first run".

## Observed in the run

The predictions above were written before the first run. They are left as
written. The run's outcome is below. Each row is the first violation the
checker reported for that build, in trace order.

| ID | Observed first violation | Predicted | Matches? |
|----|--------------------------|-----------|----------|
| 1  | A/c279/s5 acked-op-lost | acked-op-lost | yes |
| 2  | A/c0/v0 continuation-lost | acked-op-lost | kind differs; caught at the empty window |
| 3  | A/c275/v1 recovery-failed | recovery-failed | yes |
| 4  | A/c277/s9 recovery-failed | recovery-failed | yes |
| 5  | A/c84/v1722 non-prefix-state | non-prefix-state | yes |
| 6  | A/c13/v75 non-prefix-state | non-prefix-state | yes |
| 7  | A/c4/v1 continuation-lost | continuation-lost | yes |
| 8  | A/c0/v0 continuation-lost | acked-op-lost | kind differs; caught at the empty window |
| 9  | B/c75/v0 acked-op-lost | acked-op-lost | yes |
| 10 | B/c76/v0 acked-op-lost | acked-op-lost | yes |

The table above is the full run, `bench/mutants.txt`. The second table is the
same run with `--continuation-puts 0`, from `bench/mutants-no-continuation.txt`.
The oracle alone (no continuation) gives these first violations:

| ID | Without continuation | Matches the prediction? |
|----|----------------------|-------------------------|
| 1  | A/c279/s5 acked-op-lost | yes |
| 2  | A/c7/v0 acked-op-lost | yes |
| 3  | A/c275/v1 recovery-failed | yes |
| 4  | A/c277/s9 recovery-failed | yes |
| 5  | A/c84/v1722 non-prefix-state | yes |
| 6  | A/c13/v75 non-prefix-state | yes |
| 7  | MISSED (no violation in traces A, B or C) | as expected: only the continuation check finds it |
| 8  | A/c7/v0 acked-op-lost | yes |
| 9  | B/c75/v0 acked-op-lost | yes |
| 10 | B/c76/v0 acked-op-lost | yes |

Mutants 2 and 8 are caught at crash point 0 in the full run. The continuation
check is what finds them there. On the empty image, the first puts after
recovery are not durable at the second crash: mutant 2 never makes the new WAL
name durable, and mutant 8 never syncs the puts. The oracle alone catches both
at crash point 7, after the first acknowledged batch. So both are caught by
the oracle too; the continuation check only reports them earlier.

Mutant 7 is the only bug that the oracle alone misses. Its first recovery is
correct, and the bad state only shows on a later write. The continuation check
is the only check in this set that writes after recovery.
