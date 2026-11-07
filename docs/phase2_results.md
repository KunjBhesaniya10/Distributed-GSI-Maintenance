# Phase 2 Results

Measurements for the phase 2 features ([phase2_plan.md](phase2_plan.md)): cost model, async index maintenance, online build. One section per experiment of [phase2_plan.md §5](phase2_plan.md#5-benchmarking): how to run it, what was predicted, the numbers, and what they show. Raw numbers: `bench/results/*.csv`.

> **Status (2026-11-05): all experiments run.** Correctness first: 80/80 tests and the consistency checker pass on fresh 5-node and 3-node clusters ([progress.md](progress.md)), and every experiment below ends with the checker on the indexes it used: all `OK` (no missing, orphaned or misplaced entries).
>
> **Update (2026-11-05, follow-ups):** after the first round, three of the possible next steps were implemented ([progress.md](progress.md), "Phase 2 follow-ups"): **parallel hop 2**, a **coordinator xid only for 2PC commits**, and **`gsi.build_chunk_size` default 10,000**. E1 (0 and 1 ms), E2 and E7 were rerun on 5 nodes with that code; the numbers below are the new ones, with the old (sequential hop 2) ones where the comparison matters. Old CSVs: `bench/results/sequential_hop2/`. E3 (2 and 3 nodes), E4, E5 and E6 were not rerun: E3's headline column (a unique key) touches one shard, so parallel hop 2 doesn't change it; E4-E6 don't depend on the changes, except E6's default, which E6 itself measured.
>
> **Which E1 results the project uses (user's decision, 2026-11-05):** only the runs **without shard-local indexes**. With a local index on every shard, broadcast is cheap on 5 nodes in one laptop, and the GSI's gain over it comes mainly from round trips, which matter when network delays are significant (as between real machines) or with many nodes (say > 30). Neither is the case in this setup, so those runs are kept below as reference only.

## Setup

- 1 coordinator + `NNODES` data nodes (default 5), PostgreSQL 16, Docker Desktop on one Windows laptop (WSL2). All containers share the laptop's CPU and one virtual disk. `use_remote_estimate` on for every data node (D1.2).
- Fresh cluster: `cd docker && docker compose down -v && docker compose up -d --build`.
- Latency: per statement, on the client; Python's overhead (~0.1 ms) is the same for every plan compared. Lookups use prepared statements with a generic plan (`plan_cache_mode = force_generic_plan`), so they time execution, not planning; planning is measured separately (E1b).
- Network delay (D5.2): `bash bench/net_delay.sh 1` adds a 1 ms round trip between all containers (each delays its outgoing packets by 0.5 ms), `bash bench/net_delay.sh 0` removes it. Client ↔ coordinator is delayed the same way, for every plan alike.
- Disk: `pg_test_fsync` in a data node: `fdatasync` of one 8 kB WAL write takes **2.8 ms (~350/s)**, and the six instances share that disk. This explains most of the write-side numbers (E2, E6).

## Summary

| question | answer (this setup) |
|---|---|
| GSI speedup (no shard-local index, the project's setup) | **12.8-36x** at 0 ms, **5.2-9.9x** at 1 ms delay (E1) |
| Does the planner pick the faster plan? (no shard index) | **yes, 5 of 5** keys at each delay (E1) |
| Parallel hop 2 | multi-shard lookups **28-36 % faster** at 0 ms, **26-33 %** at 1 ms; one-shard lookups unchanged (E1) |
| Speedup vs nodes (unique key, no shard index) | 37.4x (2) / 36.9x (3) / 35.3-36.3x (5): flat, set by broadcast's full scans (E3) |
| Price of `use_remote_estimate` | +0.9 ms planning at 0 ms, **+13.7 ms** at 1 ms (E1b) |
| TPC-C customer-by-last-name | **6.3x** throughput with a GSI (1,651 vs 263 tps) (E7) |
| Write cost, sync | 1 sync GSI: **6.5-24x** fewer inserts/s than none, varying with the shared disk between runs (E2) |
| Write cost, async | 1 async GSI: 13-14 ms, **566-605 tps**: 3.4-6.2x sync (E2); no coordinator WAL flush any more |
| Price of async | index visible after ~56-119 ms (p50); read-your-writes through the GSI misses ~100 % (E5) |
| Hot keys | Zipf 0.99: 3.9 % retryable aborts, uniform 0 % (E4) |
| Online build, 1M rows | 20-118 s depending on chunk size (27 s at the new default 10,000); writes continue; index exact (E6) |
| Reference only: with shard-local indexes | 1.2-2.2x at 0 ms, 1.9-3.8x at 1 ms; planner wrong for K = 100 / 1000 (E1) |

## R1. Speedup due to the GSI

### E1. Speedup vs selectivity, and does the planner pick the faster plan?

```
NAMES=sweep bash bench/load_users.sh      # 1M rows; name = k<K>_<n>, each name matches exactly K rows
python bench/experiments.py lookups --secs 10 --clients 4 --tag 0ms
bash bench/net_delay.sh 1 && python bench/experiments.py lookups --secs 10 --clients 4 --tag 1ms && bash bench/net_delay.sh 0
```
For each key (`email` = unique, then names matching K = 1, 10, 100, 1000 rows), with and without a local index on every shard: average/p99 latency of the GSI plan (`gsi.force_lookup = on`) and of the broadcast plan (`gsi.enable_lookup = off`), 4 clients, 10 s each; the plan the planner picks by itself (majority of 20 sample values) and whether that is the faster one. Load: 1M rows in 90 s through the real write path (2PC + 2 GSIs, 8 sessions, ~11,000 rows/s), 199k-201k rows per shard.

*Predicted:* without shard indexes the GSI wins for every K; with shard indexes the planner switches to broadcast at about K ≈ 7 ([phase2_plan.md §1.4](phase2_plan.md#14-the-gsi-path-cost)). With D1.4 = (a) the round-trip constant doesn't follow the network delay.

Each lookup's speedup = broadcast ms / GSI ms. "Before" is the same experiment with sequential hop 2 (first round, `bench/results/sequential_hop2/`); broadcast is unaffected by the change, so only the GSI side is compared.

**No shard-local index (the project's results)**

| delay | key | est. entries | GSI ms (p99) | GSI ms before | broadcast ms (p99) | speedup | speedup before | planner | correct |
|---|---|---|---|---|---|---|---|---|---|
| 0 ms | email | 1 | 1.64 (2.6) | 1.99 | 59.5 (80.1) | **36.3x** | 34.2x | gsi | ✓ |
| 0 ms | K=1 | 2.0 | 1.71 (2.7) | 1.98 | 59.5 (78.2) | **34.8x** | 35.6x | gsi | ✓ |
| 0 ms | K=10 | 4.8 | 2.46 (3.8) | 3.36 | 64.8 (86.4) | **26.4x** | 22.7x | gsi | ✓ |
| 0 ms | K=100 | 100 | 2.89 (4.3) | 4.03 | 64.6 (84.3) | **22.3x** | 17.5x | gsi | ✓ |
| 0 ms | K=1000 | 1000 | 5.16 (8.1) | 7.15 | 66.0 (85.2) | **12.8x** | 10.4x | gsi | ✓ |
| 1 ms | email | 1 | 8.52 (9.6) | 8.87 | 84.2 (107.0) | **9.9x** | 10.7x | gsi | ✓ |
| 1 ms | K=1 | 2.0 | 8.49 (9.6) | 8.83 | 84.0 (106.2) | **9.9x** | 10.4x | gsi | ✓ |
| 1 ms | K=10 | 4.8 | 15.2 (17.0) | 20.6 | 88.4 (117.4) | **5.8x** | 4.9x | gsi | ✓ |
| 1 ms | K=100 | 100 | 17.0 (18.0) | 24.2 | 88.1 (117.9) | **5.2x** | 4.2x | gsi | ✓ |
| 1 ms | K=1000 | 1000 | 17.6 (19.2) | 25.5 | 96.1 (119.7) | **5.5x** | 4.3x | gsi | ✓ |

Files: `bench/results/lookups_5nodes_0ms.csv`, `lookups_5nodes_1ms.csv` (rows with `shard_index=False`). The 0 ms run was made after `VACUUM ANALYZE` and `CHECKPOINT` on every node: a first 0 ms run straight after the 1M-row load had GSI p99s of 58-103 ms (autovacuum and checkpoints after the load competing for the disk) and was discarded.

**Reference only: with a local index on `email` and `name` on every shard**

| delay | key | GSI ms (p99) | broadcast ms (p99) | speedup | planner | faster | correct |
|---|---|---|---|---|---|---|---|
| 0 ms | email | 1.65 (2.6) | 3.68 (5.9) | 2.23x | gsi | gsi | ✓ |
| 0 ms | K=1 | 1.80 (3.1) | 3.72 (6.3) | 2.07x | gsi | gsi | ✓ |
| 0 ms | K=10 | 2.51 (4.2) | 3.78 (6.4) | 1.51x | gsi | gsi | ✓ |
| 0 ms | K=100 | 2.96 (4.7) | 4.07 (6.8) | 1.37x | broadcast | gsi | ✗ |
| 0 ms | K=1000 | 5.34 (9.8) | 6.60 (12.1) | 1.24x | broadcast | gsi | ✗ |
| 1 ms | email | 8.52 (9.6) | 32.5 (33.5) | 3.82x | gsi | gsi | ✓ |
| 1 ms | K=1 | 8.49 (9.6) | 32.6 (33.6) | 3.84x | gsi | gsi | ✓ |
| 1 ms | K=10 | 15.3 (16.9) | 33.0 (33.9) | 2.15x | gsi | gsi | ✓ |
| 1 ms | K=100 | 17.0 (18.0) | 33.1 (35.7) | 1.94x | broadcast | gsi | ✗ |
| 1 ms | K=1000 | 17.7 (19.2) | 41.7 (44.1) | 2.35x | broadcast | gsi | ✗ |

**Observations**

1. *Without shard-local indexes the GSI wins by 12.8-36x at 0 ms and 5.2-9.9x at 1 ms*: broadcast sequentially scans ~200k rows on every shard (~60-65 ms whatever the key), the GSI reads one index node and the shards that hold the rows. The speedup falls with K because the GSI fetches K rows by primary key, but stays ≥ 12x (0 ms) / ≥ 5x (1 ms) up to K = 1000. At 1 ms it is lower than at 0 ms because both plans pay the added round trips, but broadcast's cost is mostly scanning, the GSI's mostly round trips.
2. *The planner picks the GSI for every key without shard indexes, correctly (5/5 at each delay).*
3. *Parallel hop 2*: multi-shard lookups got 28-36 % faster at 0 ms (K = 10: 3.36 → 2.46 ms, K = 1000: 7.15 → 5.16 ms) and 26-33 % at 1 ms (K = 10: 20.6 → 15.2 ms, K = 100: 24.2 → 17.0 ms). One-shard lookups (email, K = 1) are unchanged within noise, as expected: they send one hop-2 query either way.
4. *What still grows with the number of shards touched* (at 1 ms): 8.5 ms for 1 shard, 15.2 ms for ~4.5 shards (K = 10), 17.0 ms for 5, i.e. ~2 ms (≈ 2 round trips) per extra shard, down from ~3.4-3.9 ms before. The hop-2 queries themselves now overlap; what is still sequential is postgres_fdw's connection handling around them: the first use of a shard in a statement sends `START TRANSACTION ISOLATION LEVEL REPEATABLE READ` and waits for the reply (`connection.c` `begin_remote_xact`, called from `GetConnection`), and at the end the remote `COMMIT`s are sent one shard after another. Broadcast pays the same per shard. Possible next step, for a decision: overlap these too (postgres_fdw's existing `parallel_commit` server option for the commits; sending `START TRANSACTION` together with the first query for the begins).
5. *Estimates.* Unique key: 1, exactly. K = 100 / 1000: exact (these names are in the most-common-values list, statistics target 10,000). K = 1: 2.0 and K = 10: 2.8-5.2, because only part of the K = 10 names fit in the MCV list and the rest share an average of the leftover rows (~2). Before the fix in [progress.md](progress.md) (bug 1) these estimates were off by up to 250x on small tables.
6. *Reference, with shard-local indexes:* the GSI was faster for every K measured (1.24-2.23x at 0 ms, 1.94-3.84x at 1 ms); the prediction "broadcast wins from K ≈ 7" did not hold. The planner chose broadcast for K = 100 and 1000, wrongly, at both delays. Why: the model prices a round trip (`fdw_startup_cost` = 100) at only ~25 primary-key probes (≈ 4.0 each), while here a probe is a cached page (microseconds) and a round trip 0.5-2 ms; and broadcast's Append runs a cursor per shard (`START TRANSACTION`, `DECLARE`, `FETCH`, `CLOSE`) one shard after another, ~6 round trips per shard, where the model charges one. Since parallel hop 2, the model's hop-2 term (`S × c_rt`) is also pessimistic for the GSI, so these misses cost more than before (GSI 1.9-2.4x faster at 1 ms where broadcast was chosen). The formula was left as it is: the remaining per-shard `START TRANSACTION` / `COMMIT` (obs. 4) still cost about a round trip per shard, and a recalibration is the separate cost-model decision (D1.4 (b), or a higher `fdw_startup_cost`) that is not part of the chosen follow-ups.

### E1b. Planning time with `use_remote_estimate` (the price of D1.2)

At the end of E1: 50 plannings of a K = 10 name lookup with the option on and off (`EXPLAIN ANALYZE`'s `Planning Time`).

*Predicted:* "on" adds about one round trip per shard (≥ 5 × RTT): ~0.5 ms at 0 ms delay, ≥ 5 ms at 1 ms.

| delay | use_remote_estimate | planning ms avg | p99 |
|---|---|---|---|
| 0 ms | on | 2.87 | 25.2 |
| 0 ms | off | 1.95 | 2.5 |
| 1 ms | on | **15.75** | 90.0 |
| 1 ms | off | 2.09 | 2.6 |

(Rerun with the follow-up code; the first round gave 3.32 / 2.23 ms at 0 ms and 15.26 / 2.03 ms at 1 ms. Planning doesn't use hop 2, so the change is noise.)

**Observations:** the remote estimate costs +0.9 ms per planning at 0 ms and **+13.7 ms at 1 ms**: about 13 round trips, ~2.6 per shard (`START TRANSACTION` + `EXPLAIN`), more than the predicted one per shard. That is larger than the whole GSI lookup it helps choose (9 ms at 1 ms delay). So with remote estimates the planner must not plan every execution: prepared statements / generic plans are needed, as assumed. With the estimates on, the planner still chose the slower plan in the 2 shard-index cases of E1 (reference), so the remote estimate improves the broadcast side's row counts, but the round-trip constant (observation 4) is what decides these cases.

### E3. Speedup vs number of data nodes

```
bash bench/node_sweep.sh 10        # NNODES = 2, 3, 5: fresh cluster, load, E1 each (~20 min)
```
*Predicted:* broadcast asks every node, the unique-key GSI lookup always 2, so the speedup for `email` grows with N.

Each N: fresh cluster, the same 1M rows (75-82 s load), E1 at 0 ms delay (`bench/results/sequential_hop2/lookups_<N>nodes.csv`; measured before parallel hop 2 and not rerun: the project's column, a unique key without shard indexes, touches one shard and so is not affected). Speedup = broadcast ms / GSI ms. The shard-index columns are reference only.

| nodes | email, no shard index | email, shard index | K=10, shard index | K=100, shard index | K=1000, shard index | planner correct |
|---|---|---|---|---|---|---|
| 2 | 37.4x | **1.17x** | 0.97x | 0.94x | 0.89x | 9/10 |
| 3 | 36.9x | **1.52x** | 1.06x | 1.03x | 1.04x | 8/10 |
| 5 | 35.3x | **2.24x** | 1.27x | 1.11x | 1.00x | 9/10 |

Planning with `use_remote_estimate` on / off: 1.53 / 1.24 ms (2 nodes), 2.03 / 1.59 ms (3), 2.72 / 1.96 ms (5), growing with N as predicted (one remote EXPLAIN per shard).

**Observations**

1. *Reference (shard-local indexes):* the unique-key speedup grows with the number of nodes, 1.17x → 1.52x → 2.24x, because broadcast's cost grows with N (one remote query per shard) while the GSI's stays at 2. Extrapolating, a GSI matters more the larger the cluster.
2. *Without shard-local indexes (the project's setup) the speedup is ~35-37x at every N*: broadcast's cost there is scanning 1M rows in total, which the same laptop does at about the same speed however the rows are split. On separate machines the shards would scan in parallel and broadcast would get faster with N, so this flat line is specific to one laptop.
3. *Reference: the crossover the cost model predicted exists at small N with shard indexes*: with 2 nodes broadcast is faster for K ≥ 10 (0.89-0.97x), and the planner chose broadcast for K = 100 and 1000, correctly. Its one miss there (K = 10, GSI 3 % slower) is within noise. As N grows, the real crossover moves to larger K faster than the model's (at 5 nodes K = 100 was still 11 % faster with the GSI), the round-trip undervaluation of E1 obs. 4.
4. *Reproducibility:* the 5-node row was measured twice (E1 above, and here on a fresh cluster with a fresh load): speedups agree within 0.04x for selective keys (2.28 vs 2.24, 2.20 vs 2.21); for K = 1000 with shard indexes 1.14x vs 1.00x, so the planner's choice there (broadcast) was "wrong" in one run and "right" in the other, i.e. the two plans are equally fast.

### E7. TPC-C-derived customer workload

```
python bench/experiments.py tpcc --secs 15 --clients 8 --warehouses 10
```
300k customers (10 warehouses × 10 districts × 3,000), partitioned by id; GSI on `c_key = 'w:d:c_last'` (TPC-C looks customers up by warehouse, district and last name; our GSI has one column, so the three go into one; ~3 customers per key). Transaction: Order-Status-by-name (lookup) or Payment-by-name (lookup + balance update), 50/50; last names from TPC-C's NURand(255, 0, 999). No shard-local indexes (as for a freshly sharded table). 8 clients, 15 s, 0 ms delay.

| config | tps | avg ms | p50 | p95 | p99 ms | retries |
|---|---|---|---|---|---|---|
| broadcast (no GSI) | 263 | 30.5 | 29.8 | 37.5 | 50.8 | 0 |
| sync GSI | **1,651** | 4.8 | 5.2 | 7.5 | 10.2 | 1 |
| async GSI | 1,608 | 5.0 | 5.3 | 7.8 | 10.5 | 0 |

(First round, sequential hop 2: 247 / 1,460 / 1,426 tps. The GSI configs gained 13 %, broadcast 6 %; with ~3 customers per key a lookup often touches 2-3 shards, so part of the GSI's gain is parallel hop 2 and part run-to-run noise.)

**Observations:** the GSI gives **6.3x the throughput** (5.9x in the first round) and a 5x lower p99 on a standard-shaped OLTP workload. Sync and async are the same here because Payment updates `c_balance`, not the indexed `c_key`: such an UPDATE can't change an index entry, so `gsi_direct_modify_ok` lets postgres_fdw push it down to the shard, and in neither mode does it write an index entry, an outbox row or need 2PC. The cost of sync maintenance only shows when indexed columns change (E2).

## R2. Sync vs async

### E2. Write cost

```
python bench/experiments.py writes --secs 15 --clients 8       # empties users itself
```
Inserts, then key updates (new email), 8 clients, 15 s each, for: no GSI, 1 sync, 2 sync, 1 async, 2 async (non-unique GSIs; a unique GSI can't be async, D4.3). For async, `drain s` is how long the outbox took to empty after the run.

*Predicted:* sync = base write + one remote write per index + 2PC (PREPARE and COMMIT PREPARED on 2+ nodes); async = base write + outbox row on the same node, one-phase commit, so close to "none".

Rerun with the follow-up code, after `VACUUM ANALYZE` + `CHECKPOINT` on every node:

| config | op | tps | avg ms | p50 | p99 | retries | drain s |
|---|---|---|---|---|---|---|---|
| none | insert | 1,158 | 6.9 | 6.5 | 14.6 | 0 | - |
| none | key update | 759 | 10.5 | 8.0 | 44.1 | 3 | - |
| 1 sync | insert | 177 | 45.3 | 38.9 | 206.6 | 0 | - |
| 1 sync | key update | 131 | 61.1 | 53.7 | 215.5 | 8 | - |
| 2 sync | insert | 114 | 70.4 | 52.5 | 262.3 | 0 | - |
| 2 sync | key update | 136 | 58.8 | 56.0 | 124.1 | 8 | - |
| 1 async | insert | **605** | 13.2 | 11.2 | 44.0 | 0 | 0.28 |
| 1 async | key update | 585 | 13.7 | 12.2 | 42.7 | 11 | 0.28 |
| 2 async | insert | 587 | 13.8 | 12.5 | 34.5 | 0 | 0.15 |
| 2 async | key update | 479 | 16.7 | 13.7 | 73.9 | 2 | 0.15 |

**Run-to-run variation** (insert tps; three runs of the same experiment on the same laptop):

| run | none | 1 sync | 2 sync | 1 async | 2 async |
|---|---|---|---|---|---|
| first round (old code) | 2,148 | 91 | 77 | 566 | 475 |
| rerun 1 (new code, right after other benchmarks) | 1,055 | 128 | 90 | 577 | 378 |
| rerun 2 (new code, after vacuum + checkpoint; table above) | 1,158 | 177 | 114 | 605 | 587 |

The "none" and sync numbers moved by 2x between runs although the sync path didn't change: they are set by how fast the one shared virtual disk flushes at that moment. Async was the most stable (566-605). So the ratios below are given as ranges.

**Coordinator WAL flushes per write** (500 single-row inserts, one client, `pg_stat_wal.wal_sync` on the coordinator; a deterministic check of the xid change):

| GSI | WAL fsyncs per insert | WAL records per insert |
|---|---|---|
| none | 0.00 | 0.01 |
| 1 async (one node) | **0.02** | 0.07 |
| 1 sync (2PC) | 1.95 | 2.00 |

Before the change an async write assigned a coordinator xid, so `RecordTransactionCommit` wrote and flushed a commit record for every write (≥ 1 per insert); now the async row is background noise (other activity on the coordinator). A sync write still flushes twice: the xid record before PREPARE (`gsi_log_xid`) and the commit record, the 2PC decision.

**Observations**

1. *Sync maintenance is expensive here: 1 sync GSI cuts insert throughput 6.5-24x* (rerun 2: 1,158 → 177 tps). The cause is the disk, not the network: a sync write commits with 2PC, i.e. PREPARE on 2 nodes, the coordinator's xid and commit records, COMMIT PREPARED on 2 nodes, each a WAL flush (~2.8 ms here), several in sequence. All six instances share one virtual disk, so this is a worst case: on separate machines each node flushes on its own disk. With 8 clients, group commit merges some flushes, which is one reason the rate varies between runs. The *shape* holds anywhere: sync = several durable round trips per write.
2. *A second sync GSI adds less than the first* (177 → 114 tps): it adds one more participant (PREPARE + COMMIT PREPARED), but the coordinator's commit and the base node's flushes are shared.
3. *Async is 3.4-6.2x faster than sync* (rerun 2: 605 vs 177 tps) and a second async GSI costs little (both outbox rows go to the same base node, in the same local transaction).
4. *Async is still not "close to none"*: 13.2 ms vs 6.9 ms (rerun 2). The coordinator flush is gone (table above), but E2 shows no clear latency gain from it (14.1 → 13.2-13.9 ms, within the run-to-run variation): with 8 clients the coordinator's commit flushes were probably already shared by group commit, so each write paid only part of one. What remains: the outbox row is a second statement to the base node (an extra round trip, a deviation listed in progress.md), and the appliers' own transactions (index entries on the index nodes, outbox deletes) add flushes to the same shared disk while the writers run. The second is a likely explanation, not measured separately.
5. The appliers keep up: after 15 s of 480-605 writes/s the outbox was empty within 0.15-0.28 s. Every checker run: exact.

### E5. The price of async: lag and missed reads

```
python bench/experiments.py async_lag --secs 20
```
Writers insert as fast as they can (1, 4, 8, 16 clients) into a table with an async GSI; a sampler reads `gsi_lag()` every 0.2 s (age of the oldest unapplied outbox row, max over nodes); a probe inserts a row and immediately looks it up through the GSI, counting misses and the time until the row shows up. Appliers: `gsi.applier_naptime` = 100 ms, `gsi.applier_batch` = 1000.

| writers | write tps | lag p50 ms | lag p99 ms | reads missed % | visible after p50 ms | p99 ms | drain s |
|---|---|---|---|---|---|---|---|
| 1 | 312 | 89 | 120 | 99.4 | 56 | 112 | 0.03 |
| 4 | 1,230 | 111 | 126 | 100 | 67 | 126 | 0.15 |
| 8 | 788 | 175 | 298 | 100 | 95 | 194 | 0.03 |
| 16 | 1,057 | 232 | 375 | 100 | 119 | 254 | 0.03 |

**Observations**

1. *Read-your-writes through an async GSI fails essentially always* (99-100 % of probes): the lookup runs a few ms after the commit, the applier polls every 100 ms. This is the price of async that D4 accepted; it is why unique GSIs must be sync (D4.3) and why the async mode is for indexes that can be briefly stale.
2. *Staleness is bounded and small*: a new entry is visible after 56 ms (p50) with 1 writer, about half the 100 ms nap plus the apply, rising to 119 ms (p50) / 254 ms (p99) with 16 writers. The lag is dominated by `gsi.applier_naptime` (a polling interval), so it can be traded against applier load by that setting; notifying the applier on commit would cut it further.
3. *No backlog*: at up to 1,230 writes/s the outbox drained within 0.15 s after the run, so the appliers (one per node, 1,000-row batches) were never the bottleneck. (Write tps varies non-monotonically, 1,230 at 4 writers vs 788 at 8, because the laptop's CPU and disk are shared by all containers; treat it as noise.)

### E4. Hot keys (secondary)

```
python bench/experiments.py hot_keys --secs 15 --clients 8
```
50 % lookups, 50 % key updates (each moves a key away and back: two GSI changes) over 10,000 rows with a unique sync GSI on email and a sync GSI on name; keys uniform vs Zipf(0.99). Reads take no locks; this measures conflicts between writes (2PC holds row and unique-index locks until COMMIT PREPARED).

| zipf | ops/s | avg ms | p50 | p99 ms | retryable aborts | other errors | abort % |
|---|---|---|---|---|---|---|---|
| 0 (uniform) | 162 | 49.3 | 28.9 | 186.3 | 0 | 0 | 0.0 |
| 0.99 | 264 | 29.6 | 3.5 | 143.6 | 159 | 0 | **3.86** |

**Observations:** with skewed keys 3.9 % of operations abort with a retryable error (serialization failure / lock not available on a hot row or its GSI entry); uniform keys never conflict. The higher ops/s and lower p50 under Zipf are an artifact, not a speedup: aborted updates fail fast and are not counted, and hot rows are cached; the abort rate is the result. The checker found both GSIs exact afterwards.

## R3. Online build

### E6. Build time and impact on writes

```
bash bench/load_users.sh                  # 1M rows (here: the E1 sweep data)
python bench/experiments.py build
```
`CALL gsi_create('users', 'name')` on the full 1M-row table: without concurrent writes, then with 4 writer threads (key updates on email, random rows), for chunk sizes 1,000 / 10,000 / 50,000 (D7.2). Write throughput and p99 latency 5 s before, during, and 5 s after the build.

| chunk size | concurrent writes | build s | before tps (p99 ms) | during tps (p99 ms) | after tps (p99 ms) |
|---|---|---|---|---|---|
| 1,000 | no | 55.2 | - | - | - |
| 1,000 | yes | 118.3 | 114 (67) | 66 (232) | 65 (316) |
| 10,000 | yes | 27.4 | 239 (58) | 89 (154) | 96 (77) |
| 50,000 | yes | **20.2** | 97 (72) | 199 (164) | 261 (29) |

Every build ended `ready` and the checker found both GSIs exact (1,000,000 entries, nothing missing, orphaned or misplaced).

**Observations**

1. *The build is online and correct under concurrent writes*: writers kept running during every build, and the index was exact afterwards (also covered by the tests, including cancel-and-resume).
2. *Chunk size dominates build time*: 1M rows take 118 s in 1,000-row chunks, 27 s in 10,000, 20 s in 50,000. Each chunk is one 2PC transaction (backfill reads the chunk `FOR SHARE` on the base node and inserts the entries on the index nodes): at 1,000 rows that is 1,000 commits, ~55 ms each without concurrent writes, again mostly WAL flushes on the shared disk (E2). Larger chunks amortise the commit.
3. *The price of a large chunk is lock time*: a chunk holds `FOR SHARE` on its rows until it commits, so a writer hitting one waits longer. Writes during the build had p99 154-232 ms vs 58-72 ms before, which also includes the short validation phase, when new writes wait for `ShareLock`. Per-phase tps from 5-second windows on a shared laptop is noisy (the "before" phase alone ranged 97-239 tps between identical runs), so only the p99 increase during the build and the build times are conclusions; the tps columns are not.
4. With D7.2 (a) the chunk size is a setting (`gsi.build_chunk_size`). These numbers suggested a larger default: **the default is now 10,000** (was 1,000; follow-up chosen 2026-11-05), which cut the build 4x here (118 → 27 s with concurrent writes) at a modest latency cost for concurrent writers (~0.3 s of `FOR SHARE` per chunk).

## Observations across experiments

- **The GSI pays off on reads everywhere we measured** (no shard-local indexes, the project's setup): 12.8-36x at 0 ms, 5.2-9.9x at 1 ms, 6.3x on the TPC-C-style workload, and the planner picked it every time. With parallel hop 2, a lookup whose rows sit on several shards costs ~26-36 % less than before.
- **Round trips are what is left to save on the read path**: per extra shard a lookup still pays ~2 sequential round trips for postgres_fdw's remote `START TRANSACTION` and `COMMIT` (E1 obs. 4).
- **Sync maintenance costs several durable round trips per write**; on this single-disk setup that is a 6.5-24x throughput drop, bounded by the disk's flush rate. **Async recovers most of it** (3.4-6.2x sync), at the price of ~0.1 s staleness and no read-your-writes through the index. An async write on one node no longer flushes anything on the coordinator.
- **The online build works**: correct under concurrent writes, resumable, and its speed is set by the chunk size; the default is now 10,000 rows per chunk.
- **Done after the first round** (chosen 2026-11-05): parallel hop 2, coordinator xid only for 2PC, chunk size default 10,000.
- **Possible next steps still open** (none implemented; each is a decision): overlap postgres_fdw's per-shard `START TRANSACTION` / `COMMIT` (E1 obs. 4); a lower or event-driven applier nap (E5 obs. 2); a cost model that charges round trips realistically (E1 obs. 6, matters for the shard-index case).
- **Caveat for all numbers**: one laptop, six PostgreSQL instances sharing 1 CPU package and 1 virtual disk, client on the same machine. Ratios and trends are the result; absolute milliseconds will differ on real hardware, and the write-side numbers vary up to 2x between runs (E2).
