# Crash model

This document fixes what the checker assumes about a crash. The checker in
`src/crashcheck.cpp` and `src/crash_model.cpp` must follow it. Where a later
review or run showed the first version was wrong, the change is recorded in
`docs/design.md` under "Changes after the independent review".

## Where the trace comes from

Every file operation of the store goes through `SimFs` (`src/sim_fs.h`). A
`SimFs` call does two things: it updates the live file contents seen by the
running program, and it appends one event to a trace. The event kinds are:

| Event      | Meaning                                              |
|------------|------------------------------------------------------|
| Create     | a new name appears, bound to a new inode (empty)     |
| Write      | bytes written at the offset the live file had when the write was issued |
| Truncate   | an inode is cut to a size (used for WAL reset)       |
| Rename     | a name is moved to another name, replacing it        |
| Sync       | fsync of one inode (ok or failed)                    |
| SyncDir    | fsync of the directory (ok or failed)                |
| Issue      | marker: the store has appended the record of seq n   |
| Ack        | marker: the store has returned success for seq n     |

Markers do not change files. They let the checker know, at each point of the
trace, how many operations were issued and how many were acknowledged.

`replay_prefix` in `src/sim_fs.cpp` rebuilds the model after the first n events
of a trace from scratch. A unit test checks that it agrees with the model the
checker builds one event at a time.

## What survives a crash

A crash happens between two events. Take the trace prefix up to that point.

1. **Durable state.** Every Sync that succeeded made the data of its inode
   durable. Every SyncDir that succeeded made all namespace changes durable.
   Durable data and names survive the crash.
2. **Window.** Everything else is in the window. The window is the list of
   operations that are not yet durable, in trace order:
   - data operations (Write, Truncate) on an inode since the last successful
     Sync of that inode;
   - namespace operations (Create, Rename) since the last successful SyncDir.
3. **Failed fsync.** A Sync that fails removes that inode's unsynced
   operations from the window and reverts the live contents to the durable
   contents. The data is gone; a later successful fsync does not bring it
   back. This is the model behind the rule "never retry a failed fsync".

## Outcome of each window operation

Each window operation independently takes one outcome:

- **Write of L bytes:** a prefix of length k is applied, for any k in
  0..L. k = 0 is a dropped write, k = L is a complete write, anything between
  is a torn write. The bytes land at the offset the live file had when the
  write was issued. Bytes between the end of the durable data and that offset
  that no surviving write covers are zeros (a hole).
- **Truncate:** applied or not (2 outcomes).
- **Create:** the name exists or does not (2 outcomes).
- **Rename:** applied or not (2 outcomes). An applied rename of a missing
  source name does nothing.

Why a write keeps its issued offset and is not appended at replay time: the
store's WAL has no sequence check across frames. If a dropped earlier write
were followed by an appended later write, the later frame would move up into
the gap and pass its CRC, and recovery would apply it out of order. A hole of
zeros fails the CRC instead, so recovery stops there. The consequence of
keeping the issued offset is that a dropped truncate followed by a persisted
write overwrites the start of the old bytes, and the rest of the old bytes
remain. The model keeps that case, because a file system that places writes
by logical offset does the same.

Data operations and namespace operations are independent of each other, so a
rename can persist while the data it moves is not yet durable. This is the
case that makes the snapshot checkpoint need an fsync before its rename.

Operations of one inode are applied in trace order. A data operation whose
inode is no longer named in the final namespace has no visible effect.

The crash image is the durable state plus the chosen outcomes. The checker
never reads the live state to build a crash image.

## Count of crash states

Notation:

- crash points: c = 0..n, where n is the number of trace events. Point c is
  the state after the first c events. Marker events are crash points too, so
  an acknowledged-but-not-synced state is visited.
- W(c): the window at point c.
- o(op): number of outcomes of one window operation. o = L + 1 for a write of
  L bytes; o = 2 for a truncate, a create or a rename.
- Bound W = 2 (window size bound for exhaustive enumeration).
- Sample count S = 64.

Formula:

```
states(c) = product over op in W(c) of o(op)    if |W(c)| <= 2   (exhaustive)
states(c) = S = 64                              if |W(c)| >  2   (sampled)

total states of one trace = sum over c = 0..n of states(c)
total states of the run   = sum over the traces of the run
```

For an empty window the product is 1. The count is of outcome assignments.
Different assignments can give the same disk image, so the count is not the
number of distinct images.

Sampled points use a generator seeded from the run seed, the trace name and c.
The number of samples, the seed and the sampled flag are written to the output.
A sampled point is not an exhaustive check of that point.

`bench/crashcheck-points.tsv` has one row per crash point: trace, point, window
size, mode, states, and the window itself (`w<L>` for a write of L bytes, `t`,
`c`, `r` for the other kinds, `-` for an empty window). `scripts/recount.sh`
rebuilds each row's states from its window with the formula above, checks the
row's window size and mode, and sums the rows. It does not read the enumerator's
states column except to compare. `make crashcheck` fails if any row disagrees or
the two totals differ. The enumerator also checks each window operation's
outcome count against the rule above, and reports `model-outcome-count` if the
code disagrees.

## Oracle

For each crash state, after recovery:

Let I be the highest sequence number issued by the crash point (Issue
markers) and A the highest sequence number acknowledged (Ack markers).
Recovery returns the highest sequence number j it applied and the key/value
state. The state must satisfy:

1. A <= j <= I. Every acknowledged operation survives. No operation that was
   never issued appears.
2. The state equals the state after applying operations 1..j of the issued
   sequence (a prefix of the issued operations).

Condition 1 is the form that the spec's wording "every operation acknowledged
before the last durable sync boundary" reduces to in this store. The writer
returns success only after the sync that covers the operation, so in a correct
build every acknowledged operation is durable at every crash point. The two
forms agree for the correct build. The stronger form is the one that catches a
build that acknowledges early, which is why it is written this way.

Operations that were issued but not acknowledged may or may not survive. They
may survive only as a prefix: a later issued operation cannot survive while an
earlier one is lost.

### Driver checks (fault traces)

Traces B and C inject a failed fsync. The driver checks, on the workload and
not on a crash state, that:

1. the commit that hit the failed fsync returns an error (`fault-not-reported`);
2. the store is poisoned (`fault-not-poisoned`);
3. the next put returns the poisoned code (`poisoned-code-missing`);
4. that put adds no write event to the trace (`write-after-poison`).

Their violation IDs are `<trace>/fault`. The driver stops at the first failed
check and does not issue the put if the store is not poisoned. A build that
does not poison would otherwise block in the put.

### Continuation check

A state that passes the oracle is then continued, because a torn tail that
recovery leaves behind only hurts after the next write:

The check runs in two passes on the same crash image. Each pass does the
following:

1. Open the store on the crash image (recovery again). Issue K = 2 puts. Each
   must return success.
2. Take the durable image of the untraced continuation file system. Unsynced
   data is dropped. This is the strictest choice.
3. Open the store on that image. It must hold exactly the state of prefix j
   plus the K new puts, and last sequence number j + K.

The first pass does not checkpoint. A checkpoint rewrites the whole state and
cuts the log, which would hide a torn tail that the reopen should hit (mutant
7). The second pass runs the same steps with a checkpoint after the puts. A
leftover complete `snapshot.tmp` from a crash before the rename is overwritten
there, so a checkpoint that appends to it is caught (mutant B2 in the blind
set).

With K = 0 (`--continuation-puts 0`) the puts are skipped and the second pass
does not run. The first pass then checks only the recovered state.
`docs/mutants.md` reports the mutant table with the continuation check off, to
show which bugs need it.

### Violation names

| Name                       | Condition that failed                          |
|----------------------------|------------------------------------------------|
| recovery-failed            | open returned an error (for example, corrupt snapshot) |
| acked-op-lost              | j < A                                          |
| recovered-unissued         | j > I                                          |
| non-prefix-state           | state differs from prefix j                    |
| model-outcome-count        | WindowOp::options() disagrees with the rule above |
| continuation-write-failed  | a put or the checkpoint after recovery did not return success |
| continuation-lost          | after the second crash the state differs from prefix j + K |
| fault-not-reported, fault-not-poisoned, poisoned-code-missing, write-after-poison | a driver check on a fault trace failed (`<trace>/fault`) |

Each crash-state violation is printed with an ID of the form
`<trace>/c<point>/v<index>` for exhaustive states and
`<trace>/c<point>/s<sample>` for sampled states.

## Trace set of a run

Three traces are enumerated by `make crashcheck`. All use the same seed rule.

- **Trace A:** 300 seeded operations. Batches of 1, 2, 4 or 8 operations
  share one group sync. A checkpoint runs at the first batch boundary where
  at least 75 operations have passed since the last checkpoint. With seed 1 that
  is after acknowledged sequence numbers 82, 164 and 246. No faults.
- **Trace B:** 40 seeded operations, group commit, with the 5th file fsync
  failing (counted over File::sync calls; directory fsync is not counted).
  No checkpoint.
- **Trace C:** 40 seeded operations, per-op commit (one fsync inside the
  mutex for each commit), with the 5th file fsync failing. No checkpoint.

## Not modelled

- Real disks: write caches with unknown reordering, sector tearing below one
  byte, bit flips, misdirected writes, firmware bugs.
- Damage to bytes that were synced. The model only produces states where
  unsynced operations are missing or partial. A flipped bit in a synced record
  is outside it, and the store has no way to tell it from a crash hole. See
  `DamagePolicy` in `include/store.h`.
- Torn writes at sector size. The workload's values are at most 24 bytes, so
  no record spans a sector; the model's torn writes are byte prefixes only.
- Torn writes inside one byte, and holes filled with anything other than
  zeros. (Real file systems may return stale data instead of zeros.)
- Non-prefix damage inside one write. A snapshot is one write, so the model
  can only tear it into a prefix, and a prefix never has the right length.
  The snapshot's CRC is therefore checked by a unit test that flips a byte,
  not by the enumerator.
- APFS internals, journal behaviour, and the exact meaning of F_FULLFSYNC
  beyond what the operating system documents.
- Power loss. There is no power-cut test. Every crash is a state of the model.
- Concurrency. The enumerator runs one thread. The concurrency test uses real
  threads but does not enumerate their crash states.
- Errors other than fsync failure. Failed writes and failed directory fsync
  are not injected.
