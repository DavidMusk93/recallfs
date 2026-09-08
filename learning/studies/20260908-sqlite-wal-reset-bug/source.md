# Source

| Field | Value |
| --- | --- |
| Primary URL | `https://tailscale.com/blog/sqlite-wal-reset-bug` |
| Title | How we tracked down a 16-year-old SQLite bug |
| Author | Alex Chan |
| Published | 2026-08-12 |
| Access date | 2026-09-08 |
| Archive | `learning/sources/20260908-sqlite-wal-reset-bug.md` |
| Upstream fix | Fossil `7168988acbec2d8d`; Git mirror `72a14a575e72b44d6401107039f387f944d7b647` |
| Topic | SQLite WAL reset, checkpoint/write race, committed-write loss |

## Primary Evidence

| Reference | Role |
| --- | --- |
| [Tailscale incident report](https://tailscale.com/blog/sqlite-wal-reset-bug) | Production symptoms, investigation path, checkpoint pattern, rollout |
| [SQLite WAL-reset documentation](https://sqlite.org/wal.html#the_wal_reset_bug) | Authoritative affected versions, trigger sequence, probability assessment |
| [SQLite fix check-in](https://sqlite.org/src/info/7168988acbec2d8d) | Salt revalidation fix and forced interleaving test |
| [SQLite 3.51.3 release](https://sqlite.org/releaselog/3_51_3.html) | First non-withdrawn release carrying the fix |
| [SQLite testing](https://sqlite.org/testing.html) | Branch coverage, stress harnesses, crash and fault testing context |
| [Natural reproducer](https://theconsensus.dev/p/2026/08/23/another-look-at-sqlite-wal-reset.html) | No-hook workload that widens the race with a large `mmap`/`munmap` |
| [Checkpoint API](https://sqlite.org/c3ref/wal_checkpoint_v2.html) | Locking and PASSIVE fallback semantics |
| [SQLite corruption guide](https://sqlite.org/howtocorrupt.html) | Backup and locking hazards |

## Version Boundary

SQLite documents the bug as likely present from `3.7.0` through `3.51.2`.
It is fixed in `3.51.3` and later. Official backport releases exist for
`3.44.6` and `3.50.7`. The withdrawn `3.52.0` also contained the WAL fix but
must not be selected as the remediation target.

The demo uses the official `3.51.2` and `3.51.3` amalgamations. Their downloaded
ZIP digests are:

```text
3.51.2  6e2a845a493026bdbad0618b2b5a0cf48584faab47384480ed9f592d912f23ec
3.51.3  acb1e6f5d832484bf6d32b681e858c38add8b2acdfd42ac5df24b8afb46552b4
```

## Source Limitations

- Source pages were fetched successfully. Raw HTML remains temporary; its
  digests are recorded in `evidence/source-digests.txt`.
- The study does not infer the runtime SQLite version from a package manifest
  or system CLI. Applications must query each actual connection with
  `sqlite_version()` and `sqlite_source_id()`.
- Absence of loss in a finite run is evidence, not proof. The mitigation
  argument depends on removing a documented trigger precondition.
