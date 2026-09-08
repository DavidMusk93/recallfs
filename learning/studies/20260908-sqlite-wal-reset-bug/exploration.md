# Exploration Log

## 1. Research Decisions

| Step | Decision | Reason |
| --- | --- | --- |
| 1 | Treat SQLite documentation and source as authoritative | The incident article explains discovery but not every version or lock boundary |
| 2 | Compare 3.51.2 with 3.51.3 | They are adjacent vulnerable/fixed releases and minimize unrelated behavior changes |
| 3 | Use the later natural reproducer | It exercises released SQLite without a private VFS or test-only callback |
| 4 | Track acknowledged writes separately | A lost transaction can leave `integrity_check` reporting `ok` |
| 5 | Test serialization and rollback mode on 3.51.2 | The user specifically needs controls that do not require a SQLite upgrade |
| 6 | Run on macOS and the project EPYC/Linux target | The bug is timing and VFS sensitive |

## 2. Source Inspection

The Tailscale article, SQLite WAL documentation, SQLite testing documentation,
and Phil Eaton's natural reproducer were opened on 2026-09-08. Raw HTML was
downloaded only under `.tmp/`; digests are in `evidence/source-digests.txt`.

The SQLite Git mirror was inspected at:

```text
Git commit:     72a14a575e72b44d6401107039f387f944d7b647
Fossil checkin: 7168988acbec2d8d51106a263e553f8942b8b23d983dbbe5028e0f9be68cbb83
Date:           2026-03-03 19:43:19 UTC
Subject:        Avoid an obscure race condition between a checkpointer and
                a writer wrapping around to the start of the wal file.
```

The exact `src/wal.c` and `test/walrestart.test` diff is retained as
`evidence/upstream-fix.diff`.

## 3. Fault Sequence

The old `walCheckpoint()` reads `pWal->hdr.mxFrame` before it obtains
`WAL_READ_LOCK(0)`. The state can then evolve as follows:

```text
checkpointer A                 writer B                  shared wal-index
     |                            |                     nBackfill = L
     | read cached header         |                     salt = S
     | mxFrame = L                |
     |                            | reset WAL
     |                            | write m frames       nBackfill = 0
     |                            | commit               salt = S + 1
     | lock read slot 0           |
     | stale check: 0 < L         |
     | backfill old range         |
     | store nBackfill = L        |                     nBackfill = L
     |                            | append until mxFrame > L
     | later checkpoint skips frames 1..L
     v
acknowledged transaction is absent from the database file
```

`WAL_CKPT_LOCK` prevents two checkpoints from running simultaneously. It does
not prevent a PASSIVE checkpoint from overlapping a writer. The fixed code
therefore rereads the live WAL-index header after acquiring read-lock 0 and
compares its salt with the cached header. A changed salt means the WAL wrapped;
the checkpoint skips the stale backfill work.

## 4. Why Existing Tests Missed It

SQLite documents four test harnesses, 100% branch coverage for deployed
configurations, `mptester`, `threadtest3`, crash simulation, and extensive fault
testing. None of those facts imply coverage of all concurrent schedules.

The missing test dimension was a specific interleaving:

1. a complete checkpoint leaves a resettable WAL;
2. another checkpoint caches the old `mxFrame`;
3. a different connection resets and writes the WAL before read-lock 0;
4. the stale checkpoint publishes the old progress;
5. a later checkpoint consumes that false progress.

The March fix added a new 87-line Unix regression test,
`test/walrestart.test`. It installs a test-control fault callback at
`sqlite3FaultSim(660)` and runs a second connection's `UPDATE` at the exact
checkpoint boundary. This is schedule injection, not ordinary branch coverage.

SQLite initially stated that the race could not be reproduced organically.
On 2026-08-24, its WAL documentation linked Phil Eaton's no-hook reproducer.
That reproducer makes a large database mapping, then exploits the relatively
slow `munmap` between cached-header read and backfill to widen the race window.

Two properties also weaken ordinary detection:

- the bad `nBackfill` value is planted in one checkpoint and consumed later;
- a skipped committed transaction can leave a structurally consistent database,
  so `integrity_check` can still return `ok`.

Tailscale's aggressive manual checkpointing and fleet scale amplified a path
that normal auto-checkpoint workloads exercise far less often. Once the precise
root cause was found, the delay to the durable patch release was short:
2026-03-03 fix check-in to 2026-03-13 SQLite 3.51.3. The 16-year delay was
primarily a detection and reproducibility failure, not an ignored known bug.

## 5. Demo Construction

The C11 demo adapts the natural reproducer but adds:

- explicit runtime version output;
- atomic acknowledged-write and writer-error counters;
- independent final row-count and integrity oracles;
- `unsafe`, `serialized`, and `rollback` modes;
- CLI arguments so FIL-C's isolated runner can select small smoke inputs;
- deterministic database and sidecar cleanup.

The unsafe mode keeps the original scheduling shape. An attempted startup
barrier waited for one write before checkpointing; 1,200 vulnerable iterations
then showed no loss because the barrier moved the reset outside the critical
window. The barrier was removed from unsafe mode. It remains in mitigation
modes only to ensure those controls actually commit data during short runs.

## 6. FIL-C

The first FIL-C command passed both the 9 MB amalgamation and demo source to an
`-O1 -g` invocation. The compiler frontend was killed by the isolation
environment's memory limit.

The successful path compiled SQLite and the demo as separate objects at `-O0`
and linked them with FIL-C:

```text
clang version 20.1.8
Fil-C 0.684
target aarch64-unknown-linux-gnu
mode=serialized
attempts=3
committed=6
recovered=6
lost=0
integrity=ok
```

FIL-C validates the executed code for memory safety and undefined behavior. It
does not prove the absence of a timing race, and its timing is not performance
evidence.

## 7. Native Results

### macOS

Hardware was an Apple M5 Pro running Darwin 25.6.0 with Apple Clang 21.

| SQLite | Mode | Runs/attempts | Acknowledged writes | Result |
| --- | --- | ---: | ---: | --- |
| 3.51.2 | unsafe | 3 runs, failed at attempts 17/17/4 | 618/632/106 | loss in 3/3 |
| 3.51.3 | unsafe | 400 attempts | 15,102 | no loss observed |
| 3.51.2 | serialized | 400 attempts | 992 | no loss observed |
| 3.51.2 | rollback | 400 attempts | 804 | no loss observed |

The third vulnerable run is the critical oracle result:

```text
committed=106
recovered=104
lost=2
integrity=ok
```

### EPYC/Linux Target

Target `dc02-pe-t137-n047` runs Debian 12 / Linux 5.15 on two AMD EPYC 7Y83
sockets, 128 physical cores and SMT2. The test database used local ext4 on
NVMe. GCC 12.2 built the exact source digest
`70c10489af0c1cfdfa6122eb7b888c454369fcd9a182efe5cd2e98d5c4fd34f2`.

| SQLite | Mode | Runs/attempts | Acknowledged writes | Result |
| --- | --- | ---: | ---: | --- |
| 3.51.2 | unsafe | 3 runs, failed at attempts 6/7/4 | 671/692/447 | loss in 3/3 |
| 3.51.3 | unsafe | 400 attempts | 68,200 | no loss observed |
| 3.51.2 | serialized | 400 attempts | 6,106 | no loss observed |
| 3.51.2 | rollback | 400 attempts | 800 | no loss observed |

These are correctness stress runs, not performance benchmarks. No latency or
throughput conclusion is drawn.

## 8. Evidence Boundary

- Repeated loss on 3.51.2 and no observed loss on adjacent 3.51.3 strongly
  reproduces the upstream bug and fix.
- No observed loss in finite mitigation runs is not proof. The stronger
  argument is that serialization removes write/checkpoint overlap and rollback
  mode removes WAL entirely.
- The demo's process-local mutex is insufficient when other processes can write.
- The study did not test every historical vulnerable version or every VFS.
