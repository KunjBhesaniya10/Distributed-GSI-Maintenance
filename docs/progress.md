# Progress and Verification Log

This is the record we show the professor at each checkpoint: what was built, how it was verified, and what comes next. Every milestone from [implementation_plan.md](implementation_plan.md) gets one entry, added **when it is finished**, not planned.

**Rules for anyone updating this file (people and agents):**
- A milestone is "done" only when its tests pass on a fresh cluster (`docker compose down -v && up`) and the consistency checker is clean.
- Record the evidence: the exact command, the test names and the key output lines. No "works on my machine".
- Note any deviation from the implementation plan and why. Update the plan in the same commit.
- Known bugs and limitations go in the entry too. Honest gaps are better than surprises in the demo.

---

## Status summary

| Milestone | Status | Verified by | Date |
|---|---|---|---|
| Design: architecture + implementation plan | done | team review | 2026-10-28 |
| M1 cluster + gsi_fdw fork loads | done | `test_m1_cluster.py` (3) | 2026-10-28 |
| M2 catalog, placement hash, gsi_create | done | `test_m2_catalog.py` (5) | 2026-10-28 |
| M3 write interception | done | `test_m3_write.py` (9) + checker | 2026-10-28 |
| M4 2PC | done | `test_m4_2pc.py` (5) + checker | 2026-10-28 |
| M5 global uniqueness | done | `test_m5_unique.py` (7) | 2026-10-28 |
| M6 resolver function + fail points | done | `test_m6_resolver.py` (5) | 2026-10-28 |
| M7 resolver background worker | done | `test_m7_worker.py` (3) | 2026-10-29 |
| M8 planner hook + GsiLookup | done | `test_m8_read.py` (12), EXPLAIN output | 2026-10-29 |
| M9 stress + benchmarks | stress done, quick lookup timing done, full benchmarks not started | `test_stress.py` (2) | 2026-10-29 |
| Per-statement pushdown; several GSIs per table | done | `test_m3_write.py` (+4), `test_multi_gsi.py` (9) | 2026-11-01 |
| S1 async mode | not started | | |
| S2 online build | not started | | |

---

## Entry template

```
### Mx: <name>  (YYYY-MM-DD, <who>)

**What changed:** files and functions, 3-6 bullets. Link the commits.

**PostgreSQL internals touched:** e.g. FDW callback postgresPlanDirectModify, XactCallback.

**How it was verified:**
- command: `pytest tests/test_mx_*.py -v`  ->  N passed
- checker: `python tests/checker.py`  ->  0 missing, 0 orphaned, 0 prepared xacts left
- manual evidence: EXPLAIN output / pg_prepared_xacts / log lines (paste the relevant lines)

**Correctness argument:** why this can't leave base and index inconsistent (which invariant I1-I7 it relies on).

**Deviations from plan:** none / what and why.

**Known issues:** none / list.
```

---

## Entries

### Design (2026-10-28)

**What was produced:** `docs/architecture_plan.md` (design and rationale) and `docs/implementation_plan.md` (locked interfaces, milestones, invariants I1-I7).

**How the design was checked:**
- Walked through every crash point of the commit protocol (before PREPARE, after PREPARE, after local commit, between index and base COMMIT PREPARED) and confirmed the resolver's action for each. See the table in architecture_plan §8.2.
- Argued commit ordering: index-first leaves only harmless states (architecture_plan §7.4).
- Found and fixed design gaps before coding:
  - forced coordinator xid;
  - 64-bit xid in GIDs;
  - GID role tag so ordering survives a node being both base and index;
  - lock_timeout for cross-node deadlocks;
  - resolver needs connections outside a transaction block;
  - no ERROR after local commit;
  - `synchronous_commit = on`.

**Next:** M1-M6 for checkpoint 1.

### M1-M6 + stress test (2026-10-28)

**What changed** (diff against upstream postgres_fdw `REL_16_STABLE` 17c5a06, recorded in `src/gsi_fdw/UPSTREAM`):
- `connection.c` (+181/-4): 2PC in `pgfdw_xact_callback`, forced durable xid (`gsi_log_xid`), ordered `COMMIT PREPARED`, `lock_timeout` in `configure_remote_session`, raw connections for the resolver.
- `postgres_fdw.c` (+30): `PlanDirectModify` returns false and batch size is 1 for GSI tables; `AddForeignUpdateTargets` adds the old key and id as junk columns; `execute_foreign_modify` captures the row before and applies index ops after the base write.
- `option.c` (+5/-2): GUC renamed to `gsi_fdw.*`, calls `gsi_init()`.
- New: `gsi.c` (catalog, placement hash, index maintenance, `gsi_create`/`gsi_drop`/`gsi_node_for`, fail points), `gsi_resolver.c` (`gsi_resolve(grace)`), `gsi_fdw--1.0.sql`.
- `docker/`: 4-node compose cluster; `tests/`: pytest suites + `checker.py`.

**PostgreSQL internals touched:** FDW callbacks (`PlanDirectModify`, `GetForeignModifyBatchSize`, `AddForeignUpdateTargets`, `ExecForeign*` via `execute_foreign_modify`), row-identity vars (`add_row_identity_var`), `XactCallback` (PRE_COMMIT / COMMIT / ABORT), WAL (`LogLogicalMessage` + `XLogFlush`), CLOG status (`pg_xact_status`), SPI, type hash support functions.

**How it was verified** (fresh cluster: `docker compose down -v && docker compose up -d --build`):
- `cd tests && python -m pytest -v .` -> **36 passed in 57.7s**; the full suite passed 3 more times in a row.
- `python tests/checker.py` -> `rows=94 missing=0 orphaned=0 misplaced=0 prepared=0`.
- Fault injection: `gsi.fail_point` = `after_prepare` / `after_local_commit` / `mid_commit_prepared` kill the backend with `abort()`. In each case the resolver restored consistency (rolled back / rolled forward / finished the base participant). In the mid-commit case the test also asserts the harmless window: entry visible, base row not yet visible.
- Stress: 8 clients, 20 s, random multi-statement INSERT/UPDATE/DELETE: ~9,900 commits (non-unique) and ~9,400 commits (unique GSI), checker clean. The unique run also verifies that the base table has no duplicate emails.
- Compiles with no warnings (`-Wall -Wmissing-prototypes ...`).

**Correctness argument:** I1 (forced, WAL-logged xid; the local commit record decides), I2 (two passes in `gsi_finish_prepared`: `_i` before `_b`, and the same in the resolver), I3 (index ops on `GetConnection` connections), I6 (only WARNING after the decision).

**Bugs found by the tests and fixed:**
1. *Xid reuse after crash* (found by the `after_prepare` fault test): a remote-only transaction left no WAL record with its xid, so after a crash the xid could be reused and a later commit misread as the decision. Fix: `gsi_log_xid()` writes and flushes a transactional logical message before any PREPARE.
2. *Stale index snapshot* (found by the stress test): the remote REPEATABLE READ snapshot on an index node could predate the commit of the entry being deleted, so the DELETE matched 0 rows and left an orphan. Fix: an index delete must match exactly one row, otherwise raise a serialization failure (40001) and the client retries.
3. *Catalog visibility in `gsi_create`*: a read-only SPI lookup didn't see the just-inserted catalog row (segfault). Fix: the lookup is not read-only.

**Deviations from plan:**
- `UPDATE` of `id` is rejected on GSI tables: postgres_fdw doesn't move rows between foreign partitions, so a changed id would land on the wrong shard (upstream behaviour, found by test).
- `gsi_create(base, col, is_unique)` has no mode/online arguments yet; `gsi_drop` takes the table, not the index name; `gsi_resolve(grace_secs)`.
- Every participant of a GSI transaction is prepared, including read-only ones (simpler; no dirty tracking).

**Known issues / not done:** background-worker resolver (M7), read path (M8), benchmarks (M9), stretch goals. Remote index table schema is fixed to `public`; the base table needs a `bigint id`.

### M7 + M8 (2026-10-29)

**What changed:**
- `gsi_resolver.c` (+114 lines): the `gsi resolver` background worker. `gsi_resolver_init` registers it when gsi_fdw is preloaded; `gsi_resolver_main` loops every `gsi.resolver_interval` (5 s); `resolver_pass` runs `gsi_resolve_all(gsi.resolver_grace)` (10 s) in its own transaction and logs errors instead of dying.
- New `gsi_scan.c` (457 lines): the read path.
  - `set_rel_pathlist_hook` adds a `GsiLookup` CustomPath for `indexed_col = Const/Param` on a partitioned table with a ready GSI.
  - The CustomScan does hop 1 (`SELECT id, shard` on node `hash(key) mod N`), then hop 2 (`WHERE id = ANY(...)` on only the shards returned). All quals stay in the plan's filter, which is the recheck.
  - EXPLAIN ANALYZE shows `Index Node` and `Shards Fetched`.
- `gsi.c`: `gsi_init` calls `gsi_resolver_init()` and `gsi_scan_init()`; `server_node` exported as `gsi_server_node`. `Makefile`: `gsi_scan.o`.
- Fixed an accidental paste in upstream `postgres_fdw.c` (a URL inside `fsplan->fdw_exprs`, around L1581); restored to upstream.

**PostgreSQL internals touched:** `set_rel_pathlist_hook`, CustomPath/CustomScan API (`PlanCustomPath`, `CustomExecMethods`, `ExecScan`), `RegisterCustomScanMethods`, background workers (`RegisterBackgroundWorker`, latch loop, SIGHUP reload), planner internals (`apply_scanjoin_target_to_paths`).

**How it was verified** (fresh cluster, `docker compose build --no-cache`: 0 compiler warnings):
- `cd tests && python -m pytest -v .` -> **51 passed in 85.1s**, twice in a row.
- `python tests/checker.py` -> `rows=92 missing=0 orphaned=0 misplaced=0 prepared=0`.
- M8 tests (`test_m8_read.py`, 12):
  - EXPLAIN shows `Custom Scan (GsiLookup) on users` with no Append;
  - results equal a broadcast scan for every key (including after random updates);
  - a unique key fetches exactly 1 shard; a key spread over all shards fetches 3;
  - extra quals and projections work;
  - a deliberately lying index entry is filtered by the recheck;
  - generic-plan prepared statements (`Param`) work; LATERAL joins work;
  - a transaction reads its own uncommitted writes;
  - `UPDATE ... WHERE email` and `FOR UPDATE` keep the normal plan.
- M7 tests (`test_m7_worker.py`, 3): the worker is running (`pg_stat_activity.backend_type = 'gsi resolver'`); after crashes at `after_local_commit` and `after_prepare`, the in-doubt transactions are rolled forward or back **without any manual call**, and the checker is clean.
- Quick lookup timing (one machine, 3 shards, 20,000 rows, 500 random point lookups on a unique email; the shards have no local email index): **GSI p50 2.36 ms / p99 3.03 ms** vs **broadcast p50 4.89 ms / p99 6.19 ms**.

**Bug found and fixed:** the GSI path was added but never chosen. PostgreSQL's `apply_scanjoin_target_to_paths()` discards every path of a partitioned rel and rebuilds the Append from its children. Fix: after adding the path, clear the rel's `part_scheme` (pruning is already done; this only gives up partitionwise join/aggregate for that rel, which is off by default).

**Correctness argument:** I4 (every returned row passes the original quals on the fetched base row), and index-first commit (I2) means a committed, visible base row always has a visible entry. The worker only resolves GIDs older than the grace period and only follows the coordinator's recorded decision (I1).

**Deviations from plan:** costing is "0.1 × cheapest existing path" (simple; always wins when applicable). The worker's grace and interval are GUCs (`gsi.resolver_grace`, `gsi.resolver_interval`).

**Known issues / not done:** range predicates and `IN (...)` are not routed (they fall back to broadcast); `TRUNCATE` bypasses the GSI; full pgbench benchmarks (M9); online build and async mode are planned after checkpoint 1.

### Per-statement pushdown + several GSIs per table (2026-11-01)

**What changed:**
- **Pushdown only where needed.** `PlanDirectModify` used to refuse pushdown for every UPDATE/DELETE on a GSI table. New `gsi_direct_modify_ok` (gsi.c) allows it when the write can't change an index entry: an UPDATE whose `SET` list (`get_translated_update_targetlist`) has no indexed column and not `id`. Such an UPDATE runs exactly as in postgres_fdw: one remote statement, plain `COMMIT`. If the same transaction also writes a GSI, PRE_COMMIT already prepares every open connection, so it stays atomic. DELETE and key UPDATEs still run per row.
- **Several GSIs per table** (one per column). The catalog was `UNIQUE (base_table)`; it's now `UNIQUE (base_table, column_name)`, as the implementation plan always said.
  - `gsi_indexes_for_rel` returns a List.
  - Write path: one `GsiModifyState` per GSI, one junk column `gsi_old_key_<index>` per GSI plus one `gsi_old_id`; capture/apply loop over the GSIs.
  - `gsi_create` refuses only a second GSI on the same column; `gsi_drop(base, col DEFAULT NULL)` drops one GSI, or all.
  - Read path: the planner picks a ready GSI with an equality qual on its column (a unique one if several match) and stores its name in `custom_private`; the executor looks it up by name. EXPLAIN shows `GSI: <name>`.
- `checker.py` now checks every GSI on `users` (NULL keys have no entry); `playbook.sql` drops all GSIs before recreating the demo one.

**PostgreSQL internals touched:** `get_translated_update_targetlist` (updated columns of a partition, in its own numbering), `planner_rt_fetch`, row-identity vars (one per GSI), `CustomScan.custom_private`.

**How it was verified** (fresh cluster: `docker compose down -v && docker compose build && docker compose up -d`; build has 0 compiler warnings):
- after the pushdown change: `cd tests && python -m pytest -q .` -> **55 passed in 86.8s**; `python checker.py` -> `rows=92 missing=0 orphaned=0 misplaced=0 prepared=0`
- after the multi-GSI change: `python -m pytest -q .` -> **64 passed in 87.8s**; `python checker.py` -> `users_email: rows=95 missing=0 orphaned=0 misplaced=0 prepared=0`
- new tests:
  - `test_m3_write.py`: `EXPLAIN VERBOSE UPDATE users SET name = ...` is pushed down (`UPDATE public.users_shard SET name ...`, no `ctid = $1`, no Foreign Scan); DELETE is still per row; a pushed-down UPDATE leaves the index unchanged; a pushed-down UPDATE and a key UPDATE in one transaction commit together.
  - `test_multi_gsi.py` (unique GSI on email + non-unique GSI on nullable name): both index tables created; same column twice -> `DuplicateObject`; INSERT/UPDATE of either or both columns/DELETE keep both GSIs exact (NULL names not indexed); an UPDATE of `name` is no longer pushed down; a unique violation on email also rolls back the name entry; the planner uses `users_name` for `name = ?`, `users_email` for `email = ?`, and `users_email` when both are constrained; lookup results equal a broadcast; dropping the name GSI leaves the email GSI working; a 300-operation random workload ends with both GSIs exact.
- `playbook.sql` run twice on the cluster, the second time with two GSIs present: exit 0.

**Correctness argument:** a pushed-down UPDATE assigns no indexed column and not `id`, so every entry `(key, id, shard)` it could affect is unchanged; skipping the per-row path loses nothing (I3 holds for every write that touches an index). With several GSIs each one is maintained independently on the same connections and in the same 2PC transaction as the base write, so I1-I3 and I6 apply unchanged; I4 holds because the recheck covers all quals whichever GSI is used.

**Known issues / v1 assumptions found in a code review** (not fixed yet, proposed):
1. One partition per data node: the read path keys partitions by node, so two partitions of one table on the same node would hide rows from lookups.
2. `gsi.gsi_node` must not change while GSIs exist (placement is `hash mod N`, no rebalancing); nothing enforces this.
3. `id` must be globally unique (the table partitioned by `id`); `gsi_create` doesn't check the partition key.
4. Primary key `id bigint`; index tables in schema `public`; one coordinator; no catalog cache; one level of partitioning, every partition a gsi_fdw foreign table (checked only at first use).

