# Phase 2 Plan (after checkpoint 1)

Plan for the work between checkpoint 1 (viva done) and the final presentation.

> **Implementation status (2026-11-04):** items 1, 4, 5 (harness), 6 and 7 are implemented as decided below, but **not yet built or tested** (Docker unavailable); see [progress.md](progress.md) for what changed and the verification still to do, and [phase2_results.md](phase2_results.md) for the experiments. Two deviations: the async outbox row is its own statement (not merged into postgres_fdw's INSERT), and online-build validation briefly pauses new writes (`ShareLock`).

- **Decisions marked `D<n>.<m>` are yours.** Each one lists the options, the trade-offs, and a recommendation. Write the choice in the `Choice:` line; nothing in that section gets implemented before it's filled in.
- **Small choices** (marked *why:*) are ones I made inside a plan. Each says why; overrule any of them.
- When an item is done it gets a [progress.md](progress.md) entry with verification evidence, as in phase 1.

| # | Item | In scope? |
|---|---|---|
| 1 | Cost estimation for the GSI path | yes |
| 2 | Locks instead of recheck / global snapshot | **no** (optional) |
| 3 | Parallel / distributed reads | discussion for the presentation; implementation is D3.1 |
| 4 | Async index maintenance + sync vs async trade-offs | yes |
| 5 | Benchmarking | yes |
| 6 | Manual live demo | yes: [demo.md](demo.md) |
| 7 | Online index build | yes |

---

## 1. Cost estimation

### 1.1 Problem

[gsi_scan.c:194](../src/gsi_fdw/gsi_scan.c#L194) sets the GsiLookup path cost to `0.1 × cheapest other path`, so the GSI wins every time. The 1M-row benchmark ([progress.md](progress.md), 2026-11-01) shows when that's wrong: `name = ?` matches ~625 rows spread over all 5 shards, and with a local index on each shard, broadcast (5.2 ms) beats the GSI (6.6 ms).

The cost has to answer: **how many rows match, how many shards do they live on, and what does each trip cost**, in the same units PostgreSQL uses for the broadcast path, so `add_path()` can compare them honestly.

### 1.2 Notation and where each number comes from

| Symbol | Meaning | Source in PostgreSQL |
|---|---|---|
| `N` | number of data nodes | `idx->nnodes` (from `gsi.gsi_node`) |
| `T` | rows in the table | sum of `reltuples` of the partitions, i.e. `rel->tuples` of the partitioned rel after `set_append_rel_size` |
| `sel` | selectivity of `col = value` | `clauselist_selectivity(root, {key qual}, rti, JOIN_INNER, NULL)`: for `=` this is `eqsel`, which uses the **inherited** stats of `users` (`pg_statistic` with `stainherit = true`): the MCV list if the value is a common one, else `(1 − ΣMCV) / (n_distinct − #MCV)`. For a generic-plan `$1`, `1 / n_distinct`. |
| `k` | expected matching rows | `k = 1` for a unique GSI (guaranteed by the index); else `clamp_row_est(sel × T)` |
| `S` | expected shards touched in hop 2 | formula below |
| `c_rt` | cost of one remote round trip | the server's `fdw_startup_cost` (default 100), read from the foreign server options the same way postgres_fdw does |
| `c_tup` | cost of shipping one row | `fdw_tuple_cost` (default 0.01) + `cpu_tuple_cost` |
| `c_probe` | cost of one remote B-tree probe + heap fetch | `random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost` (4 + 0.005 + 0.01 by default) |
| `c_qual` | recheck cost per row | `cost_qual_eval` on `rel->baserestrictinfo` |

*Why these units:* postgres_fdw costs the broadcast children the same way (`fdw_startup_cost` per remote query, `fdw_tuple_cost` per row, and page/CPU costs for the remote work; see `estimate_path_cost_size`, [postgres_fdw.c:3480-3580](../src/gsi_fdw/postgres_fdw.c#L3480-L3580)). If both paths use the same constants, changing a constant (e.g. a slower network) moves both consistently.

### 1.3 Shards touched: `S = N · (1 − (1 − 1/N)^k)`

Rows are placed by `hash(id) mod N`, and `id` is unrelated to the indexed column. So each of the `k` matching rows lands on any given shard with probability `1/N`, independently. The probability that a given shard gets none of them is `(1 − 1/N)^k`; summing over `N` shards gives the expected number of shards that get at least one.

This is **Cardenas' formula** (Cardenas 1975, "Analysis and performance of inverted data base structures", CACM), the standard estimate of "blocks touched when fetching k random records from m blocks". *Why not Yao's formula* (the exact version, sampling without replacement): it differs only when a shard holds few rows compared to `k`; our shards hold ~200,000 rows each, so the difference is negligible and Cardenas is cheaper to compute.

Checks: `k = 1 → S = 1`; `k = 625, N = 5 → S ≈ 5`; `k = 3, N = 5 → S ≈ 2.4`.

*Why expected value, not worst case:* the planner compares expected costs everywhere else, so mixing in a worst case would bias it against the GSI.

### 1.4 The GSI path cost

```
hop 1 (one index node):  C1 = c_rt + c_probe·⌈k / e⌉ + k·c_tup
hop 2 (S base shards):   C2 = S·c_rt + k·c_probe + k·c_tup
recheck on coordinator:  C3 = k·c_qual
total:                   C_gsi = C1 + C2 + C3      startup = c_rt (first answer needs hop 1)
```

- **Hop 1** reads the `k` index entries for one key. They are adjacent in the index table's B-tree on `(key, id)`, so they need about one probe plus `⌈k / e⌉` leaf/heap pages, where `e` = index entries per 8 kB page. *Why:* the index table is narrow (`key, id, shard`), so `e` is large (~150 for a short text key); I use `e = BLCKSZ / (width of the index table row)`, with the width from the column's `avg_width` in the stats. If the heap of the index table is not clustered, each entry may still be on a different heap page; this is option (b) in D1.3.
- **Hop 2** sends one `WHERE id = ANY(...)` per touched shard (`S·c_rt`). Each of the `k` ids is a primary-key probe on its shard (`k·c_probe`); ids of different rows sit on unrelated pages, so each costs one random page.
- **Recheck** evaluates all original quals on the `k` rows.

Sanity check against the benchmark, with PG defaults (`c_rt = 100`, `c_probe ≈ 4`):

| Query | GSI estimate | Broadcast estimate | Model picks | Measured winner |
|---|---|---|---|---|
| `email` (k=1), no shard index | ≈ 210 | ≈ 5 × (100 + seq scan of 200k rows ≈ 4,000) ≈ 20,000 | GSI | GSI (1.6 vs 57 ms) ✓ |
| `email` (k=1), shard index | ≈ 210 | ≈ 5 × (100 + 4) ≈ 520 | GSI | GSI (1.5 vs 3.2 ms) ✓ |
| `name` (k=625), no shard index | ≈ 3,100 | ≈ 20,000 | GSI | GSI (6.3 vs 60 ms) ✓ |
| `name` (k=625), shard index | ≈ 3,100 | ≈ 5 × (100 + 125 × 4) ≈ 3,000 | broadcast | broadcast (5.2 vs 6.6 ms) ✓ |

With a shard index on both sides, both plans do about `k` row probes, so the difference is only round trips: `1 + S` for the GSI vs `N` for broadcast. The GSI wins while `1 + S < N`, i.e. `S < 4` for `N = 5`, which with Cardenas' formula is `k < 7`. Above that, `S → N` and the GSI pays one extra hop for nothing. (The `name` row is close, 3,100 vs 3,000, and so is the measurement, 6.6 vs 5.2 ms.) The k-sweep in §5 (E1) checks where the real crossover is.

### 1.5 The broadcast path cost (already built by PostgreSQL)

We don't compute it; it is the cheapest path PostgreSQL already put in `rel->pathlist` (the Append over the foreign scans). What it contains depends on one setting, which is D1.2:

- `use_remote_estimate = false` (current): postgres_fdw estimates each shard **locally** as a sequential scan (`seq_page_cost × pages + cpu_tuple_cost × tuples`, [postgres_fdw.c:3485](../src/gsi_fdw/postgres_fdw.c#L3485)). It cannot know that a shard has a local index, so it overestimates broadcast in the "shard index" rows above and the GSI would still win the `name` case.
- `use_remote_estimate = true`: postgres_fdw runs `EXPLAIN` on every shard at plan time and gets the real remote cost, index or not.

### 1.6 Decisions

**D1.1 Where `sel` and `T` come from**
| Option | Pros | Cons |
|---|---|---|
| (a) Coordinator `ANALYZE users` (inherited stats, sampled from shards through the FDW) | Standard PG machinery (`eqsel`, MCVs, generic plans all work); zero cost at plan time | The stats are a snapshot taken when `ANALYZE` runs; inserts/updates/deletes after that don't change them. **Autovacuum never analyzes partitioned parents or foreign tables** (the shards' autovacuum updates only the shards' own stats, which the coordinator never reads), so without another `ANALYZE` the row count `T` and `n_distinct` stay frozen. The loader runs it once after bulk load. |
| (a+) (a), plus the coordinator re-runs `ANALYZE` by itself: the GSI write path counts rows written per table, and the background worker runs `ANALYZE` when the count passes `50 + 10 % of T` (autovacuum's default rule for normal tables) | Same as (a), but stays fresh without anyone remembering | A little more code in the worker; `ANALYZE` samples ~30,000 rows from the shards (a few seconds on 1M rows) |
| (b) Read `pg_stats` of the index table from the index node at plan time | Fresh, exact for the indexed column | One extra round trip per planning; needs a cache; reimplements `eqsel` |
| (c) Keep a per-index row count / distinct-key count in the coordinator catalog, updated by the write path | Always current | Write overhead on every insert/delete; exact `n_distinct` is hard to maintain on delete |

Recommendation: **(a+)**: (a)'s machinery, with the "frozen stats" problem removed the way autovacuum removes it for normal tables. Also `gsi_create` runs `ANALYZE` at the end. Plain (a) is fine if we only benchmark after a bulk load.
Choice: __(a)__

**D1.2 Broadcast cost: `use_remote_estimate` on the data-node servers?**

This only matters if shards have **local indexes** on the indexed column. By default they don't: `users_shard` has only its primary key on `id` ([datanode.sql](../docker/init/datanode.sql)); the `email`/`name` shard indexes exist only when created by hand (`LOCAL_INDEX=1` in `bench/run_lookups.sh`, or the demo setup). Without them, every shard really does a full scan, postgres_fdw's local guess is right, and "off" costs nothing in accuracy. The model still matters then: a very non-selective key (`k` above roughly 5,000 rows, i.e. `k·c_probe` > the cost of scanning all shards) makes broadcast cheaper even without shard indexes. So the real question is whether we support or benchmark the "shards have local indexes" setup, which is the fair baseline for a GSI.

| Option | Pros | Cons |
|---|---|---|
| (a) on | Broadcast cost is real (sees shard indexes), so GSI vs broadcast is a fair comparison | `N` remote `EXPLAIN`s per planning (~N round trips); doesn't matter for prepared/generic plans |
| (b) off, accept the bias | No planning overhead | Planner never picks broadcast when shards have local indexes; the `name` row stays wrong |
| (c) off, but pass a "shard has an index on col" flag per GSI (catalog option) and cost the broadcast child as an index scan ourselves | No planning trips | We re-cost postgres_fdw's paths; a manual flag can be wrong |

Recommendation: **(a)** for correctness of the comparison; planning time measured separately.
Choice: __(a)__ (changed from (b), 2026-11-04)

What (a) costs, and how we handle it:
- **Slower planning.** Each time a query on `users` is planned, postgres_fdw sends `EXPLAIN` to every shard, one after another (at least one per shard, more if it considers several sort orders). That's ≥ 5 extra round trips per planning, about as much as a broadcast execution, paid even when the GSI wins and for writes that scan partitions. With the 1 ms network delay of D5.2 it's ≥ 5 ms per planning, more than the 1.5 ms GSI lookup itself.
- **Plans that are reused don't pay it.** A prepared statement is planned once (generic plan) or per execution (custom plan, the first 5 runs); only planning pays the `EXPLAIN`s.
- **Planning needs every shard up.** A down shard now fails planning too, not only execution (no practical difference: the broadcast would fail anyway).
- **Shard stats must be fresh.** The answer comes from each shard's own stats; those are kept up to date by the shards' autovacuum, so nothing to do.

Small choices that follow:
- *Set it per server* (`ALTER SERVER shardN OPTIONS (ADD use_remote_estimate 'true')` in `docker/init/coordinator.sql`). *why:* it's a property of the connection to a node, the same for every table on it.
- *Benchmarks switch from `pgbench -M simple` to `-M prepared`* ([bench/run_lookups.sh](../bench/run_lookups.sh)). *why:* with `-M simple` every statement is re-planned, so each lookup would include the 5 `EXPLAIN`s and E1 would mostly measure planning. Planning time is measured on its own (`EXPLAIN (ANALYZE)` prints `Planning Time`), for `use_remote_estimate` on and off.

**D1.3 How hop costs are estimated**
| Option | Pros | Cons |
|---|---|---|
| (a) Local formula above (§1.4) | No round trips at plan time; easy to explain | Approximate (assumes PK probe = 1 random page, entries clustered) |
| (b) Same formula, but assume index-table heap is unclustered (`k` random pages in hop 1) | Pessimistic, safer for big `k` | Overestimates; crossover moves to smaller `k` |
| (c) Remote `EXPLAIN` of the hop-1 query on the index node | Exact | Needs the key value at plan time (no help for generic plans); one more trip |

Recommendation: **(a)**; E1 tells us if (b) fits the measurements better.
Choice: __(a)__

**D1.4 `c_rt` (`fdw_startup_cost`) value**
| Option | Pros | Cons |
|---|---|---|
| (a) Keep PG's default 100 | Standard, nothing to justify | 100 is arbitrary; on a real network a round trip costs relatively more |
| (b) Calibrate: measure one empty remote query and one 1-row seq-scan-free query; set `fdw_startup_cost` so the ratio of round trip to page read matches | Crossover matches the hardware | One more experiment; value is specific to the setup (and to the netem delay, see D5.2) |

Recommendation: **(b)**, done once per network setting and recorded.
Choice: __(a) (we are currently doing the exp with docker)__

With D5.2 = (b): `c_rt` stays 100 for every delay setting, so the model's crossover doesn't move while the real one does. Accepted; E1 shows where the fixed constant is right and where the delay makes it pick the slower plan.

### 1.7 Implementation steps
1. In `gsi_set_rel_pathlist`: compute `sel`, `k`, `S` (§1.2-1.3), cost the path (§1.4), set `path.rows = k`. Delete the `0.1` shortcut.
2. Read `fdw_startup_cost` / `fdw_tuple_cost` of the index node's server and of the base servers (*why:* they can differ per server, and postgres_fdw reads them per server too).
3. EXPLAIN shows `Estimated Rows` and `Estimated Shards` next to the existing `Shards Fetched` (*why:* shows estimate vs actual, in the demo and in E1).
4. `gsi_create` ends with `ANALYZE` of the base table (if D1.1 = a).
5. Tests: unique key → GSI; low-selectivity key with shard indexes → broadcast; no stats → still a sane plan; generic plan (`$1`) uses `1/n_distinct`; `gsi.enable_lookup = off` still forces broadcast.
6. Verification: E1 (§5) — the planner's choice matches the faster plan at every `k`.

---

## 2. Locks / global snapshot

Out of scope (optional). Summary of the discussion: strict two-phase locking on reads (`FOR SHARE` on index entries and base rows, or a shared lock on the key) plus our 2PC gives serializable results without a global snapshot, at the cost of read throughput and possible cross-node deadlocks; Spanner avoids read locks for read-only transactions by reading every shard at one TrueTime timestamp, which vanilla PostgreSQL can't do. No code.

---

## 3. Parallel / distributed reads

**Presentation answer** (what the professor asked): PostgreSQL is centralized in query processing. One server plans and runs each query; its parallel query (Gather + worker processes, since 9.6) is inside one machine only; replicas scale reads by running *different* queries on different copies. Distributed SQL / MPP systems (Greenplum, Citus, Postgres-XL, Spanner, CockroachDB, TiDB) split one plan into fragments that run on many nodes and exchange rows between nodes. Our system distributes storage and 2PC writes, but read execution stays on the coordinator; the gap to real distributed execution is node-to-node data exchange, which is outside what an FDW can do.

**D3.1 Implement anything?**
| Option | Effort | What we can show |
|---|---|---|
| (a) Discussion only | none | slide with the comparison above |
| (b) (a) + send hop-2 queries to all shards at once and collect after (latency `max` instead of `sum`), and turn on `async_capable` for broadcast | small (~1 day) | latency vs shards-touched graph, sequential vs concurrent |
| (c) (b) + verify aggregate/join pushdown (`enable_partitionwise_aggregate`) on our tables | +½ day | "work moves to the shards" demo query |

Recommendation: **(b)**: cheap, and without it the cost model (§1) and the benchmark compare a sequential GSI against a sequential broadcast, which understates both. If (b), `S·c_rt` in §1.4 becomes `c_rt` (one parallel wave) for latency, while total work stays `S·c_rt`; I'd keep costing *work* (PG's convention) and say so.
Choice: __(no need to do this as of now we will see this later)__

---

## 4. Async index maintenance

### 4.1 What changes

Sync (today): base row and index entries are written in one transaction, committed with 2PC across the base node and the index node(s). Async: the user's transaction writes **only the base node** (one participant → ordinary one-phase commit, no PREPARE); the index entries are written later by a background applier. This is the model of DynamoDB GSIs (eventually consistent) and of Diff-Index's `async-simple` scheme (Tan et al., EDBT 2014), which reported 60-80 % lower index-update latency than `sync-full` at the price of temporary inconsistency.

Catalog already has `gsi.gsi_index.mode IN ('sync','async')` ([gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql)); today only `'sync'` is read.

**What async saves, and what it doesn't.** It doesn't save the index write: the entry is still written, later, by the applier, so total work stays the same or grows (with an outbox: + one outbox insert and delete). What it removes from the user's transaction is the **distributed part**. One insert with the base row on node B and the entry on node I, today vs async:

| | Sync (today) | Async with outbox (D4.1 a) | Async with WAL capture (D4.1 b) |
|---|---|---|---|
| Nodes in the transaction | B and I (2PC) | B only | B only |
| Round trips | ~6: INSERT on B, INSERT on I, PREPARE ×2, COMMIT PREPARED ×2 | ~2: INSERT row + outbox row (one statement), COMMIT | ~2: INSERT, COMMIT |
| Disk flushes (fsync) | PREPARE on B and I, coordinator commit, COMMIT PREPARED on B and I | one commit on B | one commit on B |
| Extra local write | — | one small outbox row on B, same commit | none (the WAL is written anyway) |
| Locks held across the network | yes, until COMMIT PREPARED | no | no |
| In-doubt state after a crash | possible (resolver) | no | no |
| Writes if node I is down | fail | succeed | succeed |

*why one statement for row + outbox:* a CTE `WITH r AS (INSERT INTO users_shard ... RETURNING id) INSERT INTO gsi_outbox ...` sends both in one round trip; the outbox write then costs only local CPU/disk on B, in the same commit.

**What other systems do**
- **DynamoDB GSIs** are updated **asynchronously** after the base-table write, and reads on a GSI are only eventually consistent. Each GSI has its own partitions and write capacity; if a GSI can't keep up, DynamoDB **throttles writes to the base table** ("GSI back-pressure") so the index lag stays bounded. AWS doesn't publish how the change is captured internally. Its *local* secondary indexes (same partition as the base item) are synchronous and strongly consistent: the "index on the same node" case, needing no distributed commit.
- **Spanner, CockroachDB, TiDB, YugabyteDB** keep GSIs **synchronous**: index entries are written in the same distributed transaction (2PC), like our sync mode; they pay the write latency to keep reads exact and to enforce uniqueness.
- **Cassandra materialized views**: the base replica updates the view replicas after its local write, through a local batchlog (a durable outbox), so views can lag.
- **Diff-Index on HBase** (EDBT 2014): offers both; `async-simple` queues index updates after the base write and reported 60-80 % lower index-update latency than `sync-full`.

Our async plan is DynamoDB's model. A back-pressure rule (block or slow writers when outbox lag passes a limit) would be the natural follow-up; it's a small extension of `gsi_lag()`.

### 4.2 What async gives up

| Property | Sync | Async |
|---|---|---|
| Write latency / throughput | 2PC: PREPARE + COMMIT PREPARED on ≥ 2 nodes | one local commit on one node |
| Lookup sees every committed row | yes | no: rows committed but not yet applied are **missed** (false negatives) |
| Lookup returns a wrong row | no (recheck) | no (recheck still removes stale entries) |
| Unique GSI | enforced at commit | can't be enforced (a duplicate is found only after both writes committed) |
| Index node down | writes to the table fail | writes continue; lag grows; applier catches up |
| New failure modes | in-doubt 2PC (resolver) | applier crash/replay, lag, ordering |

### 4.3 Decisions

**D4.1 How changes are captured**
| Option | How | Pros | Cons |
|---|---|---|---|
| (a) **Outbox row written by the coordinator** | The write path, instead of writing the index node, inserts `(seq, index, op, old_key, new_key, id)` into `gsi_outbox` **on the base node, in the same remote transaction** | Same code path we already have (we know old/new key there); atomic with the base write; works on vanilla data nodes | One extra row written per change on the base node; writes that bypass the coordinator aren't captured (already true for sync) |
| (b) Logical decoding on each data node (`test_decoding` or a small output plugin; base tables `REPLICA IDENTITY FULL`) | Applier reads each node's replication slot | No extra writes in the user's transaction; strict commit order for free | Slot holds WAL while the applier lags (disk can fill); parsing decoder output; per-node setup; more code; harder to demo |
| (c) Trigger on the base shard writing the outbox | Same as (a) but on the data node | Captures every write, even direct ones | Needs our code (trigger function) installed on data nodes; duplicate logic with the coordinator |

Recommendation: **(a)**. It reuses the write path, keeps data nodes stock PostgreSQL, and the outbox doubles as the lag measurement (`count(*)`, oldest row's age).
Choice: _(a)___

**D4.2 Sync/async per index or global**
| Option | Pros | Cons |
|---|---|---|
| (a) Per index: `gsi_create(..., mode => 'async')` (catalog column exists) | One table can have a sync unique GSI and an async non-unique one — the realistic setup and a nice experiment | A write may then touch both paths (2PC for the sync one, outbox for the async one) |
| (b) Global GUC | Simplest | Can't mix; unique GSIs force everything to sync |

Recommendation: **(a)**.
Choice: __(a)__

**D4.3 Unique GSI in async mode**
| Option | Pros | Cons |
|---|---|---|
| (a) Reject (`gsi_create` errors for `is_unique AND mode = 'async'`) | Honest; same as DynamoDB (GSIs aren't unique) | — |
| (b) Allow; the applier reports duplicates it finds (log + a `gsi_conflicts` table) | Shows the anomaly in the demo | "Unique" that isn't unique; the base rows are already committed |

Recommendation: **(a)**.
Choice: __(a) (no mention of b required)__

**D4.4 Applier placement**
| Option | Pros | Cons |
|---|---|---|
| (a) One background worker on the coordinator, looping over the nodes | Same pattern as the resolver; one place for logging | Throughput bounded by one process |
| (b) One worker per data node (on the coordinator) | Parallel apply; lag isolated per node | `N` workers; more slots in `max_worker_processes` |

Recommendation: **(b)** if we want lag numbers under heavy write load (benchmark E5); (a) is enough for the demo. They share all code except the loop.
Choice: __(b)__

**D4.5 Read freshness option**
| Option | Pros | Cons |
|---|---|---|
| (a) Plain eventual consistency | Simple | A session may not see its own write through the GSI |
| (b) GUC `gsi.read_your_writes`: before a lookup, wait until the outbox rows this session wrote are applied (like Diff-Index `async-session`) | Shows the classic middle ground; cheap to explain | Extra code in the read path; waiting costs latency |

Recommendation: **(a)** now, (b) only if time is left.
Choice: __(a)__

**D4.6 Bound the outbox (back-pressure)?**
Back-pressure = when the applier can't keep up, writers are slowed or rejected so the backlog can't grow without limit (DynamoDB throttles base-table writes when a GSI falls behind). Without a bound nothing is lost (the outbox is a durable table and the applier catches up when load drops); what grows is staleness, disk use and catch-up time. A bound only bites under **sustained** overload or when an index node is down; short bursts are absorbed either way.
| Option | Pros | Cons |
|---|---|---|
| (a) No bound; report lag (`gsi_lag()`) and measure it (E5) | Writes never blocked by the index; keeps async's "index node down → writes still work" | Lag and outbox size unbounded under overload or a long index-node outage |
| (b) Bound by **age**: if the oldest unapplied row is older than `gsi.max_lag` (e.g. 10 s), new async writes wait (up to a timeout) then fail with "index lagging, retry" | Guarantees how stale a lookup can be, the property users care about | Writes slow down when the index is overloaded; during an index-node outage writes fail after `max_lag`, so async is only available for that long |
| (c) Bound by **size**: same, but on outbox row count | Bounds disk use directly | The number of rows doesn't tell how stale reads are (depends on apply rate) |

Recommendation: **(a)**; (b) only if time is left, as an extra E5 curve (lag with/without the bound).
Choice: __(a)__

### 4.4 Design (assuming D4.1 = a), with the small choices

- **Outbox table** on each data node: `gsi_outbox(seq bigserial PRIMARY KEY, index_name name, op char, old_key text, new_key text, id bigint, created_at timestamptz DEFAULT now())`. Created by `gsi_create` like the index tables.
  - *why text keys:* the write path already renders keys as text for the index-node SQL; the applier sends them back the same way.
- **Write path:** for an async index, `gsi_apply_row` inserts one outbox row on the base node instead of writing the index node(s). Only the base node is a participant, so if no sync index was touched the transaction commits with one phase.
  - *why skip 2PC then:* one participant has nothing to agree with; `PREPARE` would be a wasted round trip and a needless in-doubt state. Needs a check "participants > 1 or a sync index was written" before preparing.
- **Applier loop** (per node): `SELECT ... FROM gsi_outbox ORDER BY seq LIMIT batch`; apply each op to the index node(s) idempotently (insert: `INSERT ... ON CONFLICT (key, id) DO NOTHING`; delete: `DELETE WHERE key = old AND id = id`; update = delete + insert); then `DELETE FROM gsi_outbox WHERE seq = ANY(<seqs it read>)`, i.e. exactly the rows it applied (not `seq <= last`, see below).
  - *why ordering by `seq` is enough:* all changes to one row happen on its base node, under that row's lock, so a later change to the same row gets a larger `seq` and commits later. Different rows' entries are independent (keyed by `(key, id)`).
  - *why delete processed rows instead of keeping a high-water mark:* `seq` is assigned before commit, so a smaller `seq` can commit after a larger one; a high-water mark would skip it. Deleting exactly what was read can't skip anything.
  - *why idempotent ops:* a crash between "applied on index node" and "deleted from outbox" replays the batch; replaying the same sequence in the same order gives the same result.
  - *batch size:* 1,000 rows per round, sleep `gsi.applier_naptime` (default 100 ms) when the outbox is empty. *why:* big enough to amortize round trips, small enough to keep transactions short.
- **Lag:** `gsi_lag()` returns, per node, outbox row count and age of the oldest row. *why age, not count:* "how stale can a read be" is the question users ask.
- **Checker:** "consistent after drain": wait until all outboxes are empty, then the existing check.
- **Tests:** async insert/update/delete eventually visible; read before apply misses the row (expected); applier crash mid-batch → no loss, no duplicates; index node down → writes succeed, lag grows, recovers; unique + async rejected.

---

## 5. Benchmarking

### 5.1 What we need to show

Three main results; everything else is secondary.

| Main result | Question | Headline number |
|---|---|---|
| **R1 Speedup due to the GSI** | How much faster is a lookup by a secondary key with the GSI than without it (broadcast), and when does the GSI stop helping? Also validates the cost model (§1). | speedup = broadcast latency / GSI latency, vs key selectivity `k` and vs number of nodes `N` |
| **R2 Sync vs async trade-off** | What does each mode cost on writes, and what does async give up on reads? | write latency/tps (none, sync, async) vs index lag and % of lookups that miss a committed row |
| **R3 Online build** | How long does building a GSI on a full table take, and how much does it slow the normal workload meanwhile? | build time; foreground write tps/latency before / during / after |

Secondary: write behaviour under hot keys (E4, existing 2PC write locks only; reads take no locks since read locking is out of scope). Correctness: `checker.py` after every run, for every result (a fast wrong index is worthless).

**"Scale with nodes"** means: repeat the measurement with a different number of data nodes (2, 3, 5) and see how the result changes. It is part of R1, not a separate result: broadcast asks every node, so its cost grows with `N`, while a lookup by a unique key always asks 2 nodes (index node + one shard). So the GSI speedup should **grow with the number of nodes**; showing that curve is the strongest argument for a GSI in a sharded system. For writes (R2) the number of nodes barely matters (a write touches its base node and its index nodes, whatever `N` is), which is also worth one sentence. Whether to run it is D5.3.

### 5.2 Which benchmark — candidates

| Benchmark | Fit | Why |
|---|---|---|
| **TPC-C (full)** (via BenchBase / HammerDB) | poor | Designed so ~90 % of transactions stay in one warehouse; 9 tables with composite keys `(w_id, d_id, c_id)`, FKs, joins. Our limits (one partitioned table per GSI, `id bigint` PK, one partition level) mean porting it is a project of its own, and most of its load wouldn't touch a GSI. |
| **TPC-C, customer-by-last-name part** | good, realistic | In Payment and Order-Status the customer is selected **by last name** 60 % of the time (TPC-C spec §2.5.1.2, §2.6.1.2), ~3 customers per name per district — a textbook non-unique secondary-key lookup on a table partitioned by something else. |
| **YCSB** (Cooper et al., SoCC 2010) | good, controllable | The standard key-value benchmark with fixed read/write mixes (A 50/50, B 95/5, C 100/0) and Zipfian keys; used for index papers on NoSQL stores (Diff-Index measured its sync vs async schemes on HBase). But YCSB itself only accesses by primary key; the secondary-key access has to be added. |
| TPC-H / TPC-DS | no | Analytical scans; no point lookups by secondary key. |
| SmallBank / LinkBench | no | No secondary-key lookups that a GSI would serve. |

### 5.3 Decisions

**D5.1 Workloads**
| Option | Pros | Cons |
|---|---|---|
| (a) **YCSB-style on `users`, with secondary-key operations**, written as pgbench scripts (extends what `bench/` has) | Every knob R1-R3 needs (mix, selectivity, skew via `random_zipfian`, nodes); same tool as today | "Our own" benchmark: must document it carefully so it's reproducible |
| (b) (a) + **TPC-C-derived customer workload**: `customer(id, c_w_id, c_d_id, c_last, c_balance, ...)`, partitioned by `id`, non-unique GSI on `c_last`; transactions = Payment-by-name (lookup + balance update) and Order-Status-by-name (lookup), names from TPC-C's `NURand` last-name generator | A recognisable, standard-derived workload ("TPC-C-inspired") | +1-2 days; must say clearly it's not spec-compliant TPC-C |
| (c) Full TPC-C via BenchBase | "Real" TPC-C numbers | Not feasible with our schema limits (see 5.2) |

Recommendation: **(b)**: (a) for the controlled sweeps, the TPC-C-derived one as the realistic workload.
Choice: __(b)__

**D5.2 Network latency between containers**
Everything runs on one laptop, so a round trip costs ~0.1 ms and the main GSI benefit (fewer round trips) is understated.
| Option | Pros | Cons |
|---|---|---|
| (a) None | Simple | Results don't transfer to a real cluster |
| (b) `tc qdisc add dev eth0 root netem delay Xms` in each container, X ∈ {0, 1, 5} ms (needs `cap_add: NET_ADMIN` and `iproute2` in the image) | Shows how the trade-off moves with network distance; real-world-like | Extra setup; must also re-calibrate `c_rt` (D1.4) per X |

Recommendation: **(b)** with X = 0 and 1 ms at least (1 ms ≈ same datacenter, different racks).
Choice: __(b)__

**D5.3 Node counts**
| Option | Cost |
|---|---|
| (a) 5 only (current) | none |
| (b) 2, 3, 5 | one `docker compose down -v`, edit modulus, reload per size; the compose file and init SQL need a node-count parameter |

Recommendation: **(b)**: the "speedup grows with nodes" curve in R1 needs at least 3 points.
Choice: __(b) (can we take no of nodes as parameter ?)__

### 5.4 Experiments (all with pgbench, 3 runs each, median of the runs, p50/p95/p99 latency; checker after every run)

| ID | Result | Vary | Fixed | Measure |
|---|---|---|---|---|
| E1 | R1 speedup vs selectivity + cost model | keys matching k ∈ {1, 10, 100, 1000} rows; with/without shard index; `gsi.enable_lookup` on/off | 1M rows, 5 nodes | latency, tps, speedup; **which plan the planner picked** vs which was faster |
| E3 | R1 speedup vs nodes | nodes 2, 3, 5 (D5.3) | unique-key and non-unique-key lookups, 1M rows | speedup per `N` |
| E2 | R2 write cost | no GSI / 1 sync / 2 sync / 1 async / 2 async | 100 % inserts, then 100 % key updates | write tps, p50/p99 latency, remote round trips per statement |
| E5 | R2 price of async | write rate | async, readers looking up just-written keys | lag p50/p99 (`gsi_lag()`), % lookups that miss a committed row |
| E6 | R3 online build | build on 1M rows with/without concurrent writes; chunk size `gsi.build_chunk_size` ∈ {1,000, 10,000, 50,000} (D7.2); `gsi.build_chunk_delay` (D7.4) | online build | build time; foreground write tps/latency before / during / after |
| E7 | R1 + R2 on the TPC-C-derived workload (D5.1 = b) | sync vs async GSI on `c_last`; nodes | Payment-by-name + Order-Status-by-name | tps, latency, speedup vs broadcast |
| E4 | secondary: hot keys | Zipf parameter (uniform, 0.99) over the written keys | 50/50 mix, sync, lock-free reads (as today) | tps, abort / lock-timeout rate of writes |

(IDs kept from the first draft so earlier references stay valid.)

*Why pgbench:* already in the image, supports custom scripts, per-statement latency (`-r`), `random_zipfian`, and `--log` for percentiles; BenchBase would need a Java runtime and its own schema loader.
*Why 3 runs and medians:* one laptop shares CPU between 6 containers; run-to-run noise is large.

---

## 6. Live demo

The demo itself (manual, terminal by terminal) is in [demo.md](demo.md). It works with today's code; the lines marked `[after D6.x]` need the small additions below.

**D6.1 How 2PC shows up on screen**
| Option | Pros | Cons |
|---|---|---|
| (a) Statement logging on the shards (`log_statement = all`) + `docker compose logs -f` in a terminal | Works today; shows the real SQL each node receives (`PREPARE TRANSACTION 'gsi_744_shard3_i'`, `COMMIT PREPARED ...`) | Shows nodes' view only; noisy (every statement, also from the tests) |
| (b) New GUC `gsi.trace = on` on the coordinator: `NOTICE`s printed in the psql session that ran the transaction (participants and roles, each PREPARE ok/failed, decision, each COMMIT/ROLLBACK PREPARED) | The coordinator's view, in the same terminal as the query; shows the *decision*, which logs can't | New code (~40 lines, `ereport(NOTICE)` at the existing commit steps) |
| (c) Both | Two terminals side by side: decision on the left, what nodes received on the right | — |

Recommendation: **(c)**.
Choice: __(c)__

**D6.2 Controlled data-node failure** — the hard part is timing a failure *between* PREPARE and COMMIT PREPARED by hand.
| Option | Pros | Cons |
|---|---|---|
| (a) `docker compose stop/pause shardX` before `COMMIT` | Works today; shows "a participant fails before prepare → everyone rolls back" | Can't hit the window after PREPARE |
| (b) New GUC `gsi.pause_point = 'after_prepare' / 'mid_commit_prepared'` + `gsi.pause_seconds`: the coordinator prints a NOTICE and sleeps there; the presenter kills a shard in another terminal, then the commit continues | Shows "participant crashes after PREPARE → prepared transaction survives its restart → resolver finishes it" — the core 2PC guarantee | New code (~20 lines next to `gsi_fail_point`) |

Recommendation: **(a) + (b)**.
Choice: __(a) + (b)__

**Resolver during the demo** (your question: automatic or manual?): both exist. The background worker runs every `gsi.resolver_interval` (5 s) but leaves prepared transactions younger than `gsi.resolver_grace` (10 s) alone. For the manual part we set `gsi.resolver_grace = '10min'`, show the leftovers, run `SELECT gsi_resolve(0)` by hand, then set the grace back to 10 s and show the worker doing it alone.

---

## 7. Online index build

### 7.1 How other systems do it

| System | Approach | Key idea |
|---|---|---|
| **PostgreSQL** `CREATE INDEX CONCURRENTLY` | Catalog entry first (invalid, not used by queries, but maintained by new writers); **wait** for transactions that might not know about it; build from a snapshot; **wait** again; a second "validate" scan adds rows missed by the first; wait for old snapshots; mark valid. On failure the index stays `INVALID` and must be dropped. | Writers learn about the index *before* the scan, so anything the scan misses is covered by either the writers or the validation pass |
| **Google F1** (Rae et al., VLDB 2013) | Index goes through states `absent → delete-only → write-only → (backfill) → public`. Servers may be one state apart at any moment (schema leases), so each step only allows states that are safe together; the backfill runs once every server is write-only. | With many servers you can't switch all at once, so `delete-only` prevents orphan entries from servers that don't insert yet |
| **CockroachDB** | F1's protocol (DELETE_ONLY → WRITE_AND_DELETE_ONLY → backfill → public); backfill reads at a fixed MVCC timestamp and bulk-ingests (AddSSTable) | Timestamp-based backfill: one consistent snapshot, concurrent writes handled by the write-only state |
| **TiDB** | F1-based states, "reorg" phase backfills in batches by handle range, resumable | Batched, resumable backfill |
| **MySQL InnoDB online DDL** | Build from a scan while concurrent changes go to an in-memory/on-disk **row log**; replay the log; short exclusive lock at the end to apply the tail | Change log instead of making writers maintain the new index |
| **DynamoDB GSI** | New GSI is `CREATING` while a backfill copies existing items; writes continue; the GSI can't be queried until `ACTIVE` | Backfill in the background, invisible to reads until done |

### 7.2 Requirements (what "online" must guarantee)

1. **Reads and writes on the table keep working** during the build; at most a very short blocking step.
2. **Correct when it becomes usable:** index = `{(key, id, shard)}` of the base rows. No entry missing (a lookup would miss a row), no orphan (harmless thanks to recheck, but wasted).
3. **Queries don't use the index before it's complete.**
4. **Unique index:** duplicates already in the data make the build fail cleanly; concurrent duplicates are rejected.
5. **Crash-safe / resumable:** a crash mid-build leaves a state that can be finished or dropped, never a "ready" partial index.

**Effects:** the build is slower than an offline build (small batches, extra validation); while `write-only`, every write already pays the index-maintenance cost (2PC to the index node); extra load on all nodes during backfill; a failed unique build leaves work to clean up.

### 7.3 Our situation, and the main simplification

- All writes go through **one coordinator**. So we don't need F1's `delete-only` state: F1 needs it because different servers can be in different states at the same time; we can make *every* writer switch at one point by waiting on the coordinator, exactly like PostgreSQL's CIC does. *(If we ever had several coordinators, we would need F1's states and leases.)*
- The catalog already has `status IN ('write-only', 'ready')`, the planner only uses `ready` indexes, and the write path maintains all sync indexes regardless of status. So the states exist; what's missing is the backfill and the switch.
- Today `gsi_create` takes `AccessExclusiveLock` and requires an empty table ([gsi.c:509](../src/gsi_fdw/gsi.c#L509)).

### 7.4 Steps

1. **Create, `write-only`, commit.** Create the index tables on all nodes (as today, via 2PC), insert the catalog row with `status = 'write-only'`, commit.
2. **Wait for writers that started before step 1** to finish: `WaitForLockers` on the base table for `ShareLock`, i.e. wait for every transaction holding `RowExclusiveLock` (all writers) acquired before (same as CIC phase 1). After this, every write maintains the new index.
   - *small fix needed:* the write path reads the GSI catalog with the transaction snapshot ([gsi.c:118-140](../src/gsi_fdw/gsi.c#L118-L140)); a `REPEATABLE READ` writer whose snapshot is older than step 1 wouldn't see the index. Read the catalog with a fresh catalog snapshot instead (*why:* PG itself reads its catalogs with a catalog snapshot, not the transaction's, for the same reason).
3. **Backfill** existing rows (method = D7.2).
4. **Validate** with the checker logic for this index only (entries = base rows); for a unique index also check no duplicate keys.
5. **Mark `ready`**, commit; next statements' plans can use it.

### 7.5 Decisions

**D7.1 Interface**
| Option | Pros | Cons |
|---|---|---|
| (a) `CALL gsi_create_concurrently(table, col, unique)` — a procedure, runs in the caller's session, commits between steps (`SPI_commit`), progress as NOTICEs | Same model as `CREATE INDEX CONCURRENTLY` (blocks only the caller); easy to demo; easy to test | The session is busy for the whole build; if it dies, the index stays `write-only` until resumed |
| (b) `gsi_create(..., concurrently => true)` starts a background worker job and returns at once; `gsi_build_progress()` shows progress | Caller is free; survives client disconnect | More code (job queue, worker), harder to test and to show errors |

*why a procedure in (a):* each step must commit before the next (other sessions must see `write-only` before we wait), and functions can't commit; procedures can. CIC also runs as several transactions internally.
Recommendation: **(a)**; resumable with `CALL gsi_create_concurrently(...)` again.
Choice: __one common function: `CALL gsi_create(table, col, unique)`, always the online protocol__

Agreed form (2026-11-04): no separate offline path and no "is the table empty?" inference. On an empty table the backfill has 0 chunks, so it finishes almost at once. *why not infer:* the online build must commit between steps, so `gsi_create` becomes a procedure either way; and an emptiness check is racy unless writers are blocked during it, which would need a second (offline) code path. Consequences: `SELECT gsi_create(...)` in tests, the loader and the docs becomes `CALL gsi_create(...)`; like `CREATE INDEX CONCURRENTLY`, it can't run inside `BEGIN ... COMMIT`.

**D7.2 Backfill method**
| Option | How | Pros | Cons |
|---|---|---|---|
| (a) **Chunked distributed transactions** (design in [architecture_plan.md §10](architecture_plan.md)) | Per shard, by `id` ranges of ~1,000: `SELECT id, col FROM shard WHERE id BETWEEN .. FOR SHARE`, insert the entries with `ON CONFLICT (key, id) DO NOTHING` on their index nodes, commit with the normal 2PC | Reuses write path, 2PC and resolver; locks make every race safe (table in the architecture plan); no validation repair needed; resumable per chunk | Every chunk is a 2PC (slower); `FOR SHARE` briefly blocks writers on those 1,000 rows |
| (b) **Snapshot scan + validation pass** (PG CIC style) | Scan each shard without locks, insert entries; then a second pass compares base and index and fixes differences (insert missing, delete orphaned) | No locks on base rows; simple bulk inserts | A row deleted between the scan and its insert leaves an orphan until validation; the validation pass itself races with writers (needs its own care) |
| (c) **Bulk per-shard copy**: each shard exports `(key, id, shard)`, the coordinator streams it to index nodes with `COPY` | Fastest for big tables | Same races as (b) plus ordering; most new code |

Recommendation: **(a)**: correctness argument already written; speed is fine for 1M rows (~1,000 chunks).
Choice: __(a) (with exp on changing chunk size)__

**D7.3 Unique build finds duplicates**
| Option | Pros | Cons |
|---|---|---|
| (a) Stop, report the key, **drop** the index automatically | Nothing left behind | Work lost; user can't inspect |
| (b) Stop, leave it `write-only` (like CIC's `INVALID`), user runs `gsi_drop` | Same as PostgreSQL; inspectable | Writers keep paying maintenance until dropped |

Recommendation: **(a)**: a `write-only` leftover silently slows every write.
Choice: __(a) (though no need to test because ideally in practical setting this should not happen because if you want something to be unique you would not apply the solution for it later on after filling the database so this is kind of just a sanity check and will not happen in practice)__

**D7.4 Throttling**
| Option | Pros | Cons |
|---|---|---|
| (a) None (full speed) | Fastest build | Foreground write latency may jump during build (E6 measures it) |
| (b) GUC `gsi.build_chunk_delay` (ms between chunks) | Shows the build-time vs foreground-impact trade-off as one more E6 curve | Tiny amount of code |

Recommendation: **(b)**, default 0.
Choice: __(b)__

### 7.6 Small choices inside the plan
- *Chunks by `id` range per shard, in `id` order:* the shard's PK index serves the range scan, and "last finished id" per shard is all the resume state we need (stored in a `gsi.gsi_build_progress(index, node, last_id)` table).
- *Chunk size 1,000 rows, GUC `gsi.build_chunk_size`:* keeps `FOR SHARE` locks short; 1M rows = ~1,000 chunks. (Changed to 10,000 after E6, 2026-11-05: see progress.md.)
- *The current blocking, empty-table-only `gsi_create` is replaced* by the procedure (D7.1); its index-table creation and catalog insert become step 1.
- *Tests:* build under a concurrent insert/update/delete workload then checker = clean; crash mid-build then resume; a concurrent duplicate insert during a unique build is rejected; planner doesn't use the index until `ready`.

---

## 8. Order of work

| Step | Item | Depends on |
|---|---|---|
| 1 | Demo additions (D6.1 trace, D6.2 pause points) | — (small, helps debugging everything else) |
| 2 | Cost model (§1) | D1.x |
| 3 | Concurrent hop 2 (if D3.1 = b/c) | — |
| 4 | Online build (§7) | D7.x |
| 5 | Async maintenance (§4) | D4.x |
| 6 | Benchmark harness + E1-E7 (§5) | 2-5, D5.x |

## References
- A. F. Cárdenas, "Analysis and performance of inverted data base structures", CACM 18(5), 1975.
- S. B. Yao, "Approximating block accesses in database organizations", CACM 20(4), 1977.
- W. Tan et al., "Diff-Index: Differentiated Index in Distributed Log-Structured Data Stores", EDBT 2014.
- I. Rae et al., "Online, Asynchronous Schema Change in F1", VLDB 2013.
- B. F. Cooper et al., "Benchmarking Cloud Serving Systems with YCSB", SoCC 2010.
- TPC Benchmark C, Standard Specification, rev. 5.11 (§2.5 Payment, §2.6 Order-Status).
- PostgreSQL 16 docs: CREATE INDEX (Building Indexes Concurrently); postgres_fdw (cost estimation options).
- CockroachDB blog, "How online schema changes are possible in CockroachDB".
