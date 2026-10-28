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
| M7 resolver background worker | not started | `test_m6_resolver.py` (no manual call) | |
| M8 planner hook + GsiLookup | not started | `test_m8_read.py`, EXPLAIN output | |
| M9 stress + benchmarks | stress done, benchmarks not started | `test_stress.py` (2) | 2026-10-28 |
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
