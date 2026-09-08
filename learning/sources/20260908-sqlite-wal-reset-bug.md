# Source Archive: SQLite WAL-Reset Bug

## Metadata

| Field | Value |
| --- | --- |
| URL | `https://tailscale.com/blog/sqlite-wal-reset-bug` |
| Page title | How Tailscale helped find the SQLite WAL-Reset bug |
| Article heading | How we tracked down a 16-year-old SQLite bug |
| Author | Alex Chan |
| Published | 2026-08-12 |
| Accessed | 2026-09-08 |
| Local raw HTML SHA-256 | `d2e2c74814e5511e6bcc080dc32e172bf8b5a73bf309dbc8be6436a12c5e7b80` |

The full copyrighted page is not committed. This archive preserves source
identity, the claims used by the study, and links to primary evidence.

## Claims Preserved

1. Tailscale had 19 SQLite database corruption incidents over six months.
2. Each control-plane shard had its own SQLite database, accessed exclusively
   by one Go process.
3. Tailscale manually controlled checkpointing and ran checkpoints
   aggressively to support frequent, consistent backups.
4. The incidents had no reliable shard, customer, feature, load, or time
   correlation, so Tailscale deployed passive production forensics.
5. Transaction replay showed that some successfully committed writes became
   invisible to later transactions.
6. SQLite's `tmstmpvfs` tracing exposed a race between checkpoint startup and a
   concurrent transaction that reset the WAL.
7. SQLite fixed the race by checking whether the WAL salt changed after the
   checkpoint read its cached header.
8. The first release carrying the fix, 3.52.0, was withdrawn after an unrelated
   text-to-floating-point change caused stale expression indexes. SQLite then
   shipped the WAL fix in 3.51.3.
9. Tailscale later instrumented its driver and observed the exact overlap in
   production without a corruption incident, providing positive evidence that
   the patched condition occurred and was survived.

## Primary Follow-Up Sources

- [SQLite WAL documentation, section 11](https://sqlite.org/wal.html#the_wal_reset_bug)
- [SQLite fix check-in](https://sqlite.org/src/info/7168988acbec2d8d)
- [SQLite 3.51.3 release notes](https://sqlite.org/releaselog/3_51_3.html)
- [SQLite testing documentation](https://sqlite.org/testing.html)
- [Natural reproducer by Phil Eaton](https://theconsensus.dev/p/2026/08/23/another-look-at-sqlite-wal-reset.html)
- [Tailscale diagnostic logging patch](https://github.com/tailscale/sqlite/commit/070c921dea03f793cbbbd9d39551d0ad26136113)

## Archive Limitations

- The Tailscale page was readable both in a browser and as prerendered HTML.
- The raw page was retained only under `.tmp/`; it is represented here by its
  digest and a focused summary.
- The article is an incident narrative, not the authoritative affected-version
  matrix. Version boundaries and backports come from SQLite's own documentation.
