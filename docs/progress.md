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
| 5 data nodes, 1M-row load, first lookup benchmark | done | full suite on 5 nodes (65), `bench/` | 2026-11-01 |
| Phase 2: cost model, online build, async mode, demo trace, benchmark harness | **done** | 80/80 tests on 5 and 3 nodes, checker clean; benchmarks in phase2_results.md | 2026-11-05 |

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

### 5 data nodes, 1M rows, first lookup benchmark (2026-11-01)

**What changed:**
- Cluster: 1 coordinator + **5 data nodes** (`docker-compose.yml`, `init/coordinator.sql`: `MODULUS 5`, nodes 0-4). Tests read the node count from `conftest.NNODES` instead of assuming 3 (fixed hard-coded `3` in test_m2, m4, m5, m6, m8; test_m5 had passed by luck with the wrong modulus).
- New GUC `gsi.enable_lookup` (default on, `PGC_USERSET`), like `enable_indexscan`: `off` makes the planner use the normal Append over all shards. Used for a same-query GSI vs broadcast comparison. Test: `test_enable_lookup_off_broadcasts`.
- `bench/load_users.sh`: loads N rows through the real write path with parallel sessions (GSIs on `email` unique and `name` non-unique). `bench/run_lookups.sh` + `lookup_email.sql` / `lookup_name.sql`: pgbench GSI vs broadcast. `bench/README.md`: data, commands, M9 plan.
- `checker.py`: placement check is one query per node (was one per entry), so it scales to 1M rows.

**How it was verified:**
- fresh 5-node cluster: `python -m pytest -q .` -> **65 passed in 95.3s**; `python checker.py` -> `users_email: rows=97 missing=0 orphaned=0 misplaced=0 prepared=0`
- `bash bench/load_users.sh 1000000 8` -> `loaded in 81s (12345 rows/s)` (3M remote writes under 2PC); partitions 199,134 / 200,642 / 199,715 / 200,325 / 200,184 rows; 1,600 distinct names
- `python tests/checker.py` on 1M rows -> `users_email: rows=1000000 missing=0 orphaned=0 misplaced=0 prepared=0`, `users_name: rows=1000000 missing=0 orphaned=0 misplaced=0 prepared=0` (1m48s)
- EXPLAIN ANALYZE: email lookup `GSI: users_email`, `Shards Fetched: 1`, rows=1; name lookup `GSI: users_name`, `Shards Fetched: 5`, rows=625; with `gsi.enable_lookup = off`: Append over 5 foreign scans
- `bash bench/run_lookups.sh 15 4` (pgbench, 4 clients, 15 s each, one laptop, 16 CPUs):

| Query | Shards | GSI | Broadcast |
|---|---|---|---|
| `email = ?` (unique) | no local index | 1.558 ms, 2,568 tps | 57.506 ms, 70 tps |
| `name = ?` (~625 rows) | no local index | 6.334 ms, 631 tps | 60.294 ms, 66 tps |
| `email = ?` (unique) | local index per shard (`LOCAL_INDEX=1`) | 1.464 ms, 2,733 tps | 3.245 ms, 1,232 tps |
| `name = ?` (~625 rows) | local index per shard | 6.614 ms, 605 tps | **5.228 ms, 765 tps** |

**Reading:** for a selective key the GSI wins even against per-shard indexes (2 round trips instead of 5). For a key whose rows sit on every shard, the GSI still contacts all 5 shards plus the index node, so broadcast with local indexes is faster. This is the case the planned selectivity-based cost model must detect (today's fixed 0.1x cost always picks the GSI).

**Known issues:** single-laptop numbers (all containers share one machine; no network latency between nodes); write-cost and node-count sweeps not run yet.

### Phase 2: cost model, online build, async GSIs, demo additions (2026-11-04)

**Status: implemented and verified (2026-11-05).** Written while Docker Desktop could not start (the Windows hypervisor was switched off: `bcdedit` `hypervisorlaunchtype Off`); built and tested once it was back on. Testing found 3 bugs and 1 test-infrastructure problem, listed below, all fixed. Benchmarks: [phase2_results.md](phase2_results.md).

**What changed** (design and decisions: [phase2_plan.md](phase2_plan.md)):
- Cost model (§1, D1.1-D1.4): `gsi_scan.c` `cost_gsi_lookup` / `estimate_entries` replace the fixed `0.1 x cheapest` cost: k from the table's inherited statistics (unique: 1), shards `S = N(1-(1-1/N)^k)` (Cardenas), both hops costed with postgres_fdw's constants. EXPLAIN shows `Estimated Entries` / `Estimated Shards`. New GUC `gsi.force_lookup` (benchmarks). `use_remote_estimate 'true'` on every data-node server.
- Online build (§7, D7.1-D7.4): new `gsi_build.c`; `gsi_create` is now a procedure, `CALL gsi_create(table, col, unique, mode)`, always online: write-only, `WaitForLockers`, chunked backfill (FOR SHARE + 2PC per chunk, progress in `gsi.gsi_build_progress`, resumable), validation (count + hash per side, under `ShareLock`), ready, ANALYZE. Duplicate keys in a unique build drop the GSI. GUCs `gsi.build_chunk_size`, `gsi.build_chunk_delay`. The write path reads the GSI catalog with the latest snapshot (so REPEATABLE READ writers see a new write-only index).
- Async GSIs (§4, D4.1-D4.6): new `gsi_async.c`; `mode => 'async'` (not for unique GSIs); the write path adds an outbox row on the base node; a transaction that only fed async GSIs and touched one node commits without 2PC; `gsi applier <i>` background workers (one per node, `gsi.applier_workers`) apply outboxes idempotently in seq order; `gsi_lag()`.
- Demo (§6, D6.1-D6.2): `gsi.trace` (2PC steps as NOTICEs), `gsi.pause_point` / `gsi.pause_seconds`.
- Cluster: `docker/init/coordinator.sh` replaces `coordinator.sql` and takes `NNODES` (2..5, D5.3); data nodes get `gsi_outbox`; `NET_ADMIN` + iproute2 for `bench/net_delay.sh` (D5.2). Tests read `NNODES` from the environment.
- Tests: `tests/test_phase2.py` (cost model, online build under concurrent writes and after a cancel, async, trace); `test_m2_catalog.py` updated (gsi_create on a full table now backfills). Callers changed from `SELECT gsi_create` to `CALL gsi_create`.
- Benchmarks: `bench/experiments.py` (E1, E2, E4-E7), `bench/node_sweep.sh`, `bench/net_delay.sh`; `run_lookups.sh` uses `-M prepared` and forces each plan.

**How it was verified so far:** `gcc -fsyntax-only -Wall -Wextra` of the new and changed C files against PostgreSQL 17.2 server headers (MSYS2 package, extracted, not installed): 0 errors, 0 warnings in `gsi.c`, `gsi_scan.c`, `gsi_build.c`, `gsi_async.c`, `gsi_resolver.c`. (Upstream `connection.c` / `postgres_fdw.c` show only the known PG16 -> 17 API differences, e.g. `create_foreignscan_path` arity; the build target is PG16.) `python -m py_compile` and `bash -n` on all scripts.

**Verified in the cluster** (PostgreSQL 16, Docker Desktop, after the fixes below):
1. `docker compose down -v && docker compose up -d --build`: the extension compiles against PG16 with no warnings; coordinator runs 1 `gsi resolver` + 5 `gsi applier` workers; every server has `use_remote_estimate=true`.
2. 5 data nodes, fresh cluster: `python -m pytest -q .` -> **80 passed** (186 s); `python checker.py` -> `users_email: rows=91 missing=0 orphaned=0 misplaced=0 prepared=0`.
3. `NNODES=3`, fresh cluster: **80 passed** (186 s); checker `rows=96 missing=0 orphaned=0 misplaced=0 prepared=0`.
4. The race-sensitive build tests (`concurrent`, `resumes`, `cached`) passed 5 runs in a row.
5. Benchmarks: [phase2_results.md](phase2_results.md).

**Bugs found by running it (2026-11-05, Docker working again; all fixed):**
1. *Cost model, k far too high* (`gsi_scan.c` `estimate_entries`): for a name occurring once, `Estimated Entries: 250` instead of 1, so the planner broadcast. eqsel turns a negative `n_distinct` (a fraction of the rows) into a count with `rel->tuples`, which for a partitioned table at this point is only the ~20 filtered rows: 0.5005 × 20 ≈ 10 distinct, 8 of them non-MCV, 0.5 / 8 × 4000 = 250. Fix: `rel->tuples` set to the table's row count for the selectivity call, and the bare clause passed (no cached selectivity reused).
2. *Online build, index table not there yet* (`gsi_build.c`): a concurrent writer failed with `relation "public.gsi_users_name" does not exist`. The remote CREATE TABLE ran in the same transaction as the catalog insert, and the coordinator commits locally (the 2PC decision) before COMMIT PREPARED reaches the nodes, so the write-only row was visible first. Fix: create the tables and commit, then insert the catalog row in a second transaction; leftovers of a build that failed between the two (empty, unreferenced) are dropped.
3. *Online build, UPDATE planned before the build* (`gsi.c` `gsi_begin_modify`): `could not find GSI junk column "gsi_old_id"`: a writer planned before the write-only row existed executed after it. Such a writer holds its lock since planning, so `WaitForLockers` waits for it and the backfill covers its rows: it now skips a write-only GSI its plan doesn't know (a ready one must still be planned). Also: a cached generic plan made before the build would have skipped the GSI for good; creating, readying and dropping a GSI now invalidate the base table's relcache entry so cached plans are replanned (new test `test_cached_plan_is_replanned_after_build`).
4. *Test infrastructure:* in the full 5-node run two coordinator-crash tests failed to reconnect: one restart took 67 s (> the tests' 60 s), all of it PostgreSQL fsyncing every data file before crash recovery, slow on Docker Desktop's disk. `recovery_init_sync_method=syncfs` on all nodes (docker-compose.yml). Both tests passed on rerun before the change.

**Known issues / deviations:**
- The outbox row is a separate statement on the base connection (one extra round trip to the same node), not merged into postgres_fdw's prepared INSERT as the plan suggested: merging would mean rewriting postgres_fdw's statement.
- Validation of an online build briefly pauses new writes (`ShareLock`); see `gsi_create` and E6.
- With `use_remote_estimate`, planning a statement on `users` opens remote transactions on every planned shard; inside a write transaction these become read-only 2PC participants (PREPAREd too). Prepared statements avoid repeated planning.

### Phase 2 follow-ups from the benchmarks (2026-11-05)

Chosen by the user from the "possible next steps" in [phase2_results.md](phase2_results.md):

1. **Parallel hop 2** (`gsi_scan.c` `gsi_fetch`): the per-shard SELECTs are all sent (`PQsendQueryParams`) before any result is read, so hop 2 waits about one round trip instead of one per shard. Small choices:
   - *Results are collected in shard order*, so a lookup returns rows in the same order as before. Waiting on the slowest shard first or last costs the same total time.
   - *No new connection handling.* Each shard is a different server, so it has its own cached connection and two queries never share one. If something throws while queries are still running (a cancel, a remote error), postgres_fdw's abort cleanup already cancels busy connections (`pgfdw_abort_cleanup`, `PQTRANS_ACTIVE`).
   - *Hop 1 stays one query to one node*, and hop 2 starts after it (it needs hop 1's ids), so a lookup is two round trips at any shard count.
2. **Coordinator xid only for 2PC** (`gsi.c` `gsi_begin_modify`): no longer calls `GetTopFullTransactionId()` for every GSI write; `gsi_log_xid()` assigns the xid at pre-commit when the commit is 2PC (any sync GSI write, or async writes spanning several nodes). A write that fed only async GSIs on one node has no coordinator xid, so no local commit record and no WAL flush. Checked against PG16 `xact.c`: PRE_COMMIT callbacks run while the state is still `TRANS_INPROGRESS`, so assigning an xid there is allowed (the only runtime check forbids it during parallel operations), and `RecordTransactionCommit` writes and flushes a commit record only if an xid was assigned.
3. **`gsi.build_chunk_size` default 10,000** (was 1,000; `gsi_build.c`): E6 measured 27 s instead of 118 s per 1M rows, at the price of longer `FOR SHARE` lock hold per chunk for concurrent writers.

New tests (`tests/test_phase2.py`): a lookup spanning every shard returns all rows (`Shards Fetched: N`); one shard blocked by a lock while others answer, the statement cancelled by `statement_timeout` with queries in flight, then the same session's next lookup is correct and nothing is left running on the shards; an async-only one-node write has no coordinator xid (`pg_current_xact_id_if_assigned()` is NULL).

**Verified:** image rebuilt (no compiler warnings); fresh 5-node cluster: `python -m pytest -q .` **83 passed** (152 s), checker `users_email: rows=87 missing=0 orphaned=0 misplaced=0 prepared=0`. Coordinator WAL flushes per single-row insert (`pg_stat_wal`): async 0.02 (noise), sync 1.95. Benchmarks E1 (0/1 ms), E2, E7 rerun: see phase2_results.md; the sequential-hop-2 results are kept in `bench/results/sequential_hop2/`. Note: E7 leaves a `customer` table with a GSI in the catalog, and the tests expect only the `users` GSIs (9 failed for that reason when run right after the benchmarks), so run the tests on a fresh cluster after E7.

**Bench file names (2026-11-05):** renamed so each name says what it is. Scripts: `phase2.py` → `experiments.py`, `e3_nodes.sh` → `node_sweep.sh`, `netem.sh` → `net_delay.sh`. Commands: `e1`/`e2`/`e4`/`e5`/`e6`/`e7` → `lookups`/`writes`/`hot_keys`/`async_lag`/`build`/`tpcc`. Results: `<experiment>[_<N>nodes][_<tag>].csv`, e.g. `lookups_5nodes_1ms.csv`; the new `--tag` option sets the suffix (before, the 0 ms / 1 ms copies were made by hand). The experiment labels E1-E7 stay as section names in the plan and results docs; `bench/README.md` maps each to its command and files. Docs updated to match.
