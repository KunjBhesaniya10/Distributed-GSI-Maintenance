# Project context: Distributed Global Secondary Index (GSI) Maintenance for Sharded PostgreSQL

This file gives an AI assistant the background for this repository. Read it before making changes.

## Course and team

- **Course:** CS 631, advanced database internals (implementation of relational databases). The team already did the core course (CS 317), so the project is expected to be **above** the instructor's suggested-project baseline: real changes to PostgreSQL internals, touching at least two layers, with a correctness argument.
- **Team:** Kunj Bhesaniya (23B0995), Dhruvraj Merchant (23B1041), Dipen Sojitra (23B0913), Harshil Solanki (23B1016).
- **Last semester:** the same team built a single-node GSI over append-only Parquet files. This project is deliberately different: the headline is **keeping a partitioned index consistent across nodes** under concurrent updates, deletes and node failures (2PC, crash recovery, consistency). Do not frame the work as "last semester's index, sharded."

## The problem

When a table is sharded across PostgreSQL servers by primary key, a query on another column (`WHERE email = ?`) must go to every shard, and `UNIQUE(email)` cannot be enforced, since each shard sees only its own rows. Spanner, CockroachDB, DynamoDB, YugabyteDB and TiDB solve this with global secondary indexes. Core PostgreSQL has no distributed database support and no global indexes (not even for single-node partitioned tables), and Citus has only shard-local indexes.

## Core idea

- There is **one GSI per indexed column**, split into pieces across nodes. The entry for key `k` lives on index node `hash(k) mod N`, independently of where the base row lives. That is what makes it *global*.
- Entries are **one per row**: `(key, id, shard)`, not `key -> set of shards`. The per-row form gives exact primary-key fetches, enables global uniqueness via a unique B-tree, and makes delete and update maintenance exact.
- A lookup is two hops: hash the key, ask one index node, then fetch `WHERE id = ANY(...)` from only the shards it returned.
- Every write touches two nodes (base shard and index node), so they must be kept consistent. That is the central challenge.

## Scope (agreed priorities)

**Core deliverable (must work):**
1. Cluster: Docker Compose with 1 coordinator and 3 data nodes, `max_prepared_transactions > 0` everywhere.
2. Synchronous GSI maintenance with two-phase commit, the crash resolver, and global uniqueness.
3. Query routing through the planner (CustomScan).
4. Evaluation: benchmarks, consistency checker, fault injection.

**Stretch goals (only once the core works; drop if time runs short):**
5. Asynchronous maintenance via logical decoding.
6. Online index build (F1-style states).

The submitted proposal lists all of these as goals, but in practice sync is the core and async plus the online build are stretch.

## Architecture and design decisions

### Packaging
- All code is **one C module: a modified copy of `postgres_fdw`, called `gsi_fdw`**, plus (for async) a small logical-decoding output plugin on the data nodes.
- Building inside postgres_fdw reuses its connection cache and remote-transaction handling, which both 2PC and index writes need.
- On the coordinator, `users` is hash-partitioned by `id`; each partition is a `gsi_fdw` foreign table on one data node. PostgreSQL partition routing already handles base-table reads and writes.

### Metadata and index storage
- Coordinator catalog: `gsi_index(name, base_table, column, mode, status)`, with mode `sync | async` and status `write-only | ready` (building states).
- On every data node: `gsi_users_email(key, id, shard)` with a B-tree on `key`, `UNIQUE` for a unique GSI.
- Placement: `hash(key) mod N` using PostgreSQL's type hash functions (the same ones hash partitioning uses).

### Synchronous write path
- `ExecForeignInsert/Update/Delete` call the original postgres_fdw routine, then queue an index operation (insert new key; delete old key on UPDATE or DELETE).
- `AddForeignUpdateTargets` also fetches the **old value of the indexed column**, so UPDATE and DELETE know which entry to remove.
- **Direct modify and batch insert are disabled** for GSI tables (`PlanDirectModify` returns false). Otherwise postgres_fdw pushes the whole `UPDATE ... WHERE` to the remote node and the per-row callbacks never run.
- Queued index ops go to the owning index node over a postgres_fdw connection, inside that connection's remote transaction.

### Atomic commit (2PC)
postgres_fdw normally sends `COMMIT` to each remote separately in its pre-commit callback. We change that callback:
1. **Pre-commit:** `PREPARE TRANSACTION 'gsi_<coordinator xid>_<node>'` on every participant (base shards and index nodes). Any failure aborts everything.
2. **Decision point:** the coordinator's own local commit. Its xid is embedded in every prepared transaction's name, so whether it committed is the global decision.
3. **After commit:** `COMMIT PREPARED` to **index nodes first, then base shards**. On abort, `ROLLBACK PREPARED`.

### Crash recovery (resolver)
A background worker on the coordinator periodically reads `pg_prepared_xacts` on each node. For each leftover `gsi_*` transaction it checks `pg_xact_status(xid)` on the coordinator and commits or rolls it back.

### Uniqueness
The unique B-tree on the index node enforces it. A duplicate fails the index insert, which aborts the whole distributed transaction. PostgreSQL's unique-index locking handles concurrent inserts.

### Read path (query routing)
- `set_rel_pathlist_hook` on the partitioned `users` table: if a qual is `email = const` and a `ready` GSI exists, add a **`GsiLookup` CustomPath** costed below the Append-over-all-shards path.
- Executor: hash the key, query one index node for `(id, shard)`, group ids by shard, fetch `WHERE id = ANY(...)` from only those shards, then **recheck `email = const`**.
- The index is treated as a hint: the recheck means a stale or uncommitted entry never yields a wrong row. Committing index nodes first means a row is never silently missed once its base row is visible.
- Range predicates: a hash-partitioned index can't route a range to one node, so query all index nodes in parallel (still cheaper than scanning every shard).

### Asynchronous mode (stretch)
- The FDW does not touch the index during writes.
- Base shards use `REPLICA IDENTITY FULL` so old values appear in the change stream.
- Output plugin (C, logical decoding callbacks) emits `(op, old_key, new_key, id)` for GSI tables.
- Applier: coordinator background worker, one per data node. Reads the node's replication slot, applies to index nodes, confirms the slot position only after applying. At-least-once delivery, so applies are idempotent (upsert; delete only if the entry matches).
- Lag = current WAL position minus confirmed slot position, plus commit timestamps.
- Global uniqueness **cannot** be enforced in async mode. This is the documented trade-off.

### Online index build (stretch, from F1)
1. `gsi_create('users','email','sync')` sets status **write-only**: new writes maintain the index, queries don't use it.
2. **Backfill:** scan each shard, insert entries with `ON CONFLICT DO NOTHING`.
3. **Validation:** remove entries whose base row no longer exists (deletes that raced with backfill).
4. Status becomes **ready**; the planner starts using it.

## Known limitations (state them in the report, don't try to solve them)
- Fully consistent cross-node reads during the brief commit window would need global snapshots. Out of scope.
- No global uniqueness in async mode.

## Evaluation plan
- **Correctness:** a checker comparing the full index against the base table after randomized workloads.
- **Fault injection:** a GUC like `gsi.fail_point = after_prepare | after_local_commit | mid_commit_prepared` that kills the process at that step; the resolver must restore consistency.
- **Benchmarks (pgbench custom scripts):** write throughput and latency with no index vs sync (vs async); lookup latency GSI vs broadcast with 2, 3 and 4 shards; async lag under load.

## Where the risk is
The two hard parts, to prototype first:
1. Patching postgres_fdw's commit callback for 2PC.
2. Getting old values on UPDATE/DELETE with direct modify disabled.

Everything else builds on these.

## References
1. W. Tan, S. Tata, Y. Tang, L. Fong. *Diff-Index: Differentiated Index in Distributed Log-Structured Data Stores.* EDBT 2014. Closest prior work; compares sync vs async GSI maintenance.
2. I. Rae et al. *Online, Asynchronous Schema Change in F1.* VLDB 2013. Source of the online index build states.
3. J. C. Corbett et al. *Spanner: Google's Globally-Distributed Database.* OSDI 2012. Reference design for synchronous index maintenance with 2PC.
4. Background: *Database System Concepts*, Ch. 21–23 (sharding, distributed query processing, distributed transactions and 2PC recovery).

## Notes for the assistant
- Target PostgreSQL's C extension APIs (FDW callbacks, planner hooks, CustomScan, background workers, logical decoding). Match postgres_fdw's code style when editing the copied module.
- Keep sync mode working before touching stretch goals.
- Any change to commit ordering or the resolver must keep the invariant: the coordinator's local commit is the single decision point, and index nodes commit before base shards.
