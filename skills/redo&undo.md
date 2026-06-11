# Redo & Undo

Goal: help agents use precise recovery terminology when designing durable state changes, transactions, rollback paths, and crash recovery protocols.

## 1. Summary

| Concept | Problem Solved | Core Semantics |
| --- | --- | --- |
| redo | Committed results must not be lost | Replay changes that are committed or must be completed after recovery |
| undo | Uncommitted results must be reverted | Roll back incomplete, aborted, or uncommitted changes |
| checkpoint | Recovery must be bounded | Mark state that has already been safely persisted |
| idempotency | Recovery may repeat work | Reapplying redo must not create duplicate side effects |

Key points:

- Redo does not, by itself, provide atomicity. It mainly supports durability and crash recovery.
- Undo is not just a reverse-operation stack. In database systems, it also supports rollback and may support consistent reads.
- Atomicity usually comes from the combination of transaction state, redo records, undo records, a commit point, and a recovery protocol.

## 2. Agent Design Rules

- Define operation states first: `planned`, `running`, `committed`, `rolled_back`.
- Use redo records to describe how to complete changes that are already decided to take effect.
- Use undo records to describe how to revert changes that have not reached the commit point.
- Make redo operations idempotent. Replaying the same record must not create extra side effects.
- Make undo operations idempotent where possible, or at least able to detect already-reverted state.
- Define the persistence order explicitly: write recovery metadata before making state externally visible.
- During recovery, inspect transaction state first, then decide whether to redo or undo.

## 3. Generic Flow

```text
+----------------------+
| Build execution plan |
+----------+-----------+
           |
           v
+----------------------+
| Write redo/undo logs |
+----------+-----------+
           |
           v
+----------------------+
| Apply state changes  |
+----------+-----------+
           |
           v
+----------------------+---- fail ---->+----------------------+
| Commit point reached?|               | Roll back with undo  |
+----------+-----------+               +----------------------+
           | yes
           v
+----------------------+
| Mark committed       |
+----------+-----------+
           |
           v
+----------------------+
| Redo after crash     |
+----------------------+
```

## 4. Recovery Decision

```text
+----------------------+
| Start crash recovery |
+----------+-----------+
           |
           v
+----------------------+
| Read logs and states |
+----------+-----------+
           |
           v
+----------------------+---- committed ---->+----------------------+
| Transaction committed?|                    | Redo committed work |
+----------+-----------+                    +----------------------+
           |
           | not committed
           v
+----------------------+
| Undo uncommitted work|
+----------------------+
```

## 5. MySQL InnoDB Semantics

Redo and undo in MySQL InnoDB are often misunderstood because they serve different parts of the transaction and recovery model.

| Mechanism | Primary Purpose | Typical Content |
| --- | --- | --- |
| redo log | Crash recovery and durability for committed changes | Physical change records for database pages |
| undo log | Transaction rollback and MVCC consistent reads | Previous row versions for rollback and visibility reconstruction |
| binlog | Replication and point-in-time recovery | Logical change events |
| doublewrite buffer | Protection against partial page writes | A safe intermediate copy of data pages |

### 5.1 Redo Log

The redo log ensures that committed changes can be recovered even if dirty pages have not yet been flushed to data files.

```text
+-------------------------+
| Modify buffer pool page |
+-----------+-------------+
            |
            v
+-------------------------+
| Write redo log buffer   |
+-----------+-------------+
            |
            v
+-------------------------+
| Flush redo at commit    |
+-----------+-------------+
            |
            v
+-------------------------+
| Replay redo after crash |
+-------------------------+
```

Redo log semantics:

- Preserve durability of committed transactions.
- Allow data pages to be flushed lazily for performance.
- Reconstruct committed page changes that had not reached data files before a crash.

### 5.2 Undo Log

The undo log stores the previous row versions needed to roll back uncommitted changes and to serve MVCC consistent reads.

```text
+-------------------------+
| Update a row            |
+-----------+-------------+
            |
            v
+-------------------------+
| Store old row in undo   |
+-----------+-------------+
            |
            v
+-------------------------+---- rollback ---->+-------------------------+
| Write new row version   |                    | Restore from undo      |
+-----------+-------------+                    +-------------------------+
            |
            v
+-------------------------+
| Read old version if needed |
+-------------------------+
```

Undo log semantics:

- Roll back uncommitted transactions.
- Support MVCC consistent reads through old row versions.
- Help crash recovery clean up transactions that did not commit.

## 6. MySQL Cases

### 6.1 Committed Transaction, Dirty Page Not Flushed

| Phase | State |
| --- | --- |
| Transaction modifies a page | The page becomes dirty in the buffer pool |
| Commit | Redo records are durable |
| Data page | The page may still not be written to the data file |
| Crash recovery | Redo replay restores the committed page changes |

Conclusion: redo ensures committed transaction changes are not lost.

### 6.2 Crash with an Uncommitted Transaction

| Phase | State |
| --- | --- |
| Transaction modifies data | The new row version may exist in the buffer pool |
| Undo | The previous row version is recorded |
| Commit | Commit has not happened |
| Crash recovery | Recovery identifies the uncommitted transaction and rolls it back using undo |

Conclusion: undo removes uncommitted changes so they do not become durable visible results.

### 6.3 Consistent Read

| Scenario | Behavior |
| --- | --- |
| Transaction A updates a row but has not committed | A new row version is created and the previous version is kept in undo |
| Transaction B performs a consistent read | InnoDB checks visibility with a Read View |
| The new version is not visible | InnoDB follows undo information to reconstruct a visible older version |

Conclusion: undo is also an MVCC mechanism, not only a rollback mechanism.

## 7. Common Misconceptions

| Misconception | More Accurate Statement |
| --- | --- |
| Redo guarantees atomicity | Redo mainly supports durability. Atomicity needs transaction state and undo as well |
| Undo is just a reverse stack after crash | Undo supports rollback and MVCC; recovery applies it based on transaction state |
| Commit must flush data pages immediately | Commit usually requires durable redo; data pages may be flushed later |
| Redo log and binlog are the same thing | Redo is for InnoDB crash recovery; binlog is for replication and logical recovery |

## 8. Agent Checklist

- Is the pre-commit recovery path different from the post-commit recovery path?
- Are redo operations idempotent?
- Can undo revert all uncommitted externally visible state?
- Is the commit point explicitly defined?
- Does the design avoid claiming that redo alone provides atomicity?
- Is there a checkpoint mechanism to avoid unbounded recovery replay?
