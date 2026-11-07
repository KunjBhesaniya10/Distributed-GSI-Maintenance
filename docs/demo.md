# Live Demo (final presentation)

A manual demo: every step is a query or command typed live. No scripts run during the demo. Duration ~15 min (acts 1-6), plus the appendix for questions.

Uses the phase 2 demo additions `gsi.trace` (2PC steps as NOTICEs) and `gsi.pause_point` (stop between PREPARE and COMMIT so a node can be killed by hand). **Dry-run the whole demo on the presentation laptop the day before** and note the actual GIDs/timings.

---

## 0. Before the audience arrives (~10 min)

From the repo root, Git Bash:
```
cd docker
docker compose down -v && docker compose up -d --build
docker compose ps                         # 6 containers, all (healthy)
cd .. && bash bench/load_users.sh         # 1M rows, GSIs on email (unique) and name (~80 s)
```
Give every shard its own local index on `email` and `name` (*why:* the fair baseline; without it broadcast is a full scan of every shard and the comparison in act 2 is trivially won):
```
cd docker
for s in shard1 shard2 shard3 shard4 shard5; do
  docker compose exec -T $s psql -U postgres -c "CREATE INDEX IF NOT EXISTS users_shard_email ON users_shard (email)" -c "CREATE INDEX IF NOT EXISTS users_shard_name ON users_shard (name)" -c "ANALYZE users_shard"
done
cd ..
```
Turn on statement logging on every data node (so the 2PC commands appear in the log terminal), and hold back the background resolver so leftovers stay visible until we resolve them by hand:
```
cd docker
for s in shard1 shard2 shard3 shard4 shard5; do
  docker compose exec -T $s psql -U postgres -c "ALTER SYSTEM SET log_statement = 'all'" -c "SELECT pg_reload_conf()"
done
docker compose exec -T coordinator psql -U postgres -c "ALTER SYSTEM SET gsi.resolver_grace = '10min'" -c "SELECT pg_reload_conf()"
```
*Why `resolver_grace` and not `resolver_interval`:* the worker keeps running every 5 s, but ignores prepared transactions younger than the grace, so it won't "steal" the manual resolve. Act 6 sets it back to 10 s to show the automatic path.

## 1. Terminals (open all four, in `docker/`, side by side)

| Name | Command | Shows |
|---|---|---|
| **C1** | `docker compose exec coordinator psql -U postgres` | the presenter's session |
| **C2** | `docker compose exec coordinator psql -U postgres` | a second client (concurrency) |
| **LOG** | bash: `docker compose logs -f --since 0s coordinator shard1 shard2 shard3 shard4 shard5 \| grep --line-buffered -E "PREPARE\|fail point\|gsi resolver"` <br> PowerShell: `docker compose logs -f --since 0s coordinator shard1 shard2 shard3 shard4 shard5 \| Select-String "PREPARE\|fail point\|gsi resolver"` | what each node receives (2PC), crashes and the resolver |
| **SH** | `docker compose exec shardN psql -U postgres` (N chosen in each act) | one data node from the inside |

Helper queries, used throughout (run in **C1**):
```sql
-- which node holds the index entry of a key (0-based; node i = shard i+1)
SELECT gsi_node_for('demo1@x'::text) + 1 AS index_shard;
-- which node holds the base row of an id
SELECT r + 1 AS base_shard FROM generate_series(0, 4) r
WHERE satisfies_hash_partition('users'::regclass, 5, r, 9000001::bigint);
```
*Why `satisfies_hash_partition`:* it's PostgreSQL's own routing check for hash partitions, so the answer is exactly where the row will go; `gsi_node_for` is the index placement function every component of ours uses.

---

## Act 1. The cluster (1 min)

**C1:**
```sql
SELECT * FROM gsi.gsi_node;                                     -- 5 data nodes
\d+ users                                                       -- hash-partitioned by id, 5 foreign partitions
SELECT index_name, column_name, is_unique, mode, status FROM gsi.gsi_index;
SELECT count(*) FROM users;                                     -- 1000000
```
**Say:** the table is split by `id`. A lookup by `email` or `name` therefore doesn't know which shard to ask. The GSI is a second table, split by the indexed value, that maps `key → (id, shard)`.

## Act 2. The read path: GSI vs broadcast (3 min)

**C1:**
```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'user123456@mail.org';
```
**Expect:** `Custom Scan (GsiLookup)`, `GSI: users_email`, `Index Node: N`, `Shards Fetched: 1`, ~1-2 ms. Two round trips: index node, then one base shard.
```sql
SET gsi.enable_lookup = off;
EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'user123456@mail.org';
RESET gsi.enable_lookup;
```
**Expect:** `Append` over 5 foreign scans; about twice as slow (every shard is asked, each uses its local index: ~3 ms vs ~1.5 ms in the 2026-11-01 benchmark).
```sql
EXPLAIN ANALYZE SELECT * FROM users WHERE name = 'Priya Shah';
```
**Expect:** `Append` over 5 foreign scans, ~625 rows: the cost model expected ~625 entries on all 5 shards (`SET gsi.force_lookup = on` and run it again to see `Estimated Entries: 625`, `Estimated Shards: 5.0`), so the GSI would save nothing over shards that have their own index. **Say:** the planner compares both plans; the GSI wins for selective keys (the email lookup above shows `Estimated Shards: 1.0`).

## Act 3. One write = a distributed transaction (3 min)

**C1:**
```sql
SELECT gsi_node_for('demo1@x'::text) + 1 AS index_shard;        -- e.g. 3
SELECT r + 1 AS base_shard FROM generate_series(0, 4) r
WHERE satisfies_hash_partition('users'::regclass, 5, r, 9000001::bigint);   -- e.g. 2
SET gsi.trace = on;                                             -- 2PC steps as NOTICEs
INSERT INTO users VALUES (9000001, 'demo1@x', 'Demo One');
```
**Expect in LOG** (shard numbers from the two helper queries; also the `name` index's node):
```
shardX | LOG: statement: PREPARE TRANSACTION 'gsi_<xid>_shardX_i'     <- index participant(s)
shardY | LOG: statement: PREPARE TRANSACTION 'gsi_<xid>_shardY_b'     <- base participant
shardX | LOG: statement: COMMIT PREPARED 'gsi_<xid>_shardX_i'         <- index participants first
shardY | LOG: statement: COMMIT PREPARED 'gsi_<xid>_shardY_b'         <- base last
```
**Expect in C1:** `NOTICE: gsi: 2PC for xid ..., participants: shardX (index), shardY (base)`, a `PREPARE TRANSACTION ...` / `... prepared on` pair per node, `all participants prepared; the local commit of xid ... is the decision: commit`, then `COMMIT PREPARED ... ok` per node, index nodes first.

**Say:** same xid in every GID (that's how the resolver finds them later); every node votes with PREPARE; the decision is the coordinator's local commit; index entries are committed before the base row, so a reader never sees a base row whose index entry is missing.

**SH** on the index shard: `SELECT * FROM gsi_users_email WHERE key = 'demo1@x';` → `(demo1@x, 9000001, <base node id>)`.

Key change:
```sql
UPDATE users SET email = 'demo1-new@x' WHERE id = 9000001;      -- old entry deleted, new entry inserted, maybe on another node
SELECT * FROM users WHERE email = 'demo1-new@x';
```

## Act 4. Global uniqueness across shards (2 min)

| Step | C1 | C2 |
|---|---|---|
| 1 | `BEGIN;` | `BEGIN;` |
| 2 | `INSERT INTO users VALUES (9000002, 'race@x', 'one');` | |
| 3 | | `INSERT INTO users VALUES (9000003, 'race@x', 'two');` → **blocks** |
| 4 | `COMMIT;` | → `ERROR: duplicate key value violates unique constraint ...` |
| 5 | | `ROLLBACK;` |

**Say:** the two rows would live on different base shards, so no shard could catch the duplicate alone. Both index entries go to the same node (`gsi_node_for('race@x')`), whose unique index makes C2 wait for C1's outcome.

## Act 5. A data node fails during a transaction (3 min)

### 5a. Fails before the vote → everybody rolls back
**C1:**
```sql
SELECT gsi_node_for('fail@x'::text) + 1 AS index_shard;         -- say K
BEGIN;
INSERT INTO users VALUES (9000004, 'fail@x', 'Fail');           -- index entry now sits uncommitted on shard K
```
Another terminal (`docker/`): `docker compose kill shardK` (a crash, not a clean stop).
**C1:**
```sql
COMMIT;
```
**Expect:** an error about the connection to shard K; the transaction is rolled back. LOG shows `ROLLBACK` on the other participants; no `COMMIT PREPARED` anywhere. With `gsi.trace = on`, C1 shows `PREPARE TRANSACTION ... on shardK ...` with no `prepared` after it, then `decision: abort; rolling back every participant`.

`docker compose start shardK`, wait for `(healthy)`, then **C1:**
```sql
SELECT * FROM users WHERE id = 9000004;                         -- 0 rows
SELECT * FROM users WHERE email = 'fail@x';                     -- 0 rows
```
**SH** (shard K and the base shard): `SELECT gid FROM pg_prepared_xacts;` → empty.

### 5b. Fails after the vote → the prepared transaction survives the crash
**C1:**
```sql
SET gsi.pause_point = 'after_prepare';  SET gsi.pause_seconds = 20;
INSERT INTO users VALUES (9000005, 'survive@x', 'Survive');
```
C1 prints `NOTICE: gsi: paused at "after_prepare" for 20 s`. During the pause: `docker compose kill shardB` (B = base shard of 9000005), then `docker compose start shardB`. The coordinator continues: local commit = decision **commit**; `COMMIT PREPARED` to shard B fails → `WARNING: could not finish prepared transaction ..., left for gsi_resolve()`.

**SH** on shard B after restart: `SELECT gid FROM pg_prepared_xacts;` → `gsi_<xid>_shardB_b` **survived the crash** (PREPARE wrote it to disk). **C1:** `SELECT gsi_resolve(0);` → 1; the row is visible. **Say:** this is the 2PC guarantee: once a node says "prepared", it can always commit, even across its own crash.

## Act 6. The coordinator crashes (3 min)

### 6a. After the decision → roll forward
**C1:**
```sql
SET gsi.fail_point = 'after_local_commit';
INSERT INTO users VALUES (9000006, 'crash@x', 'Crash');
```
**Expect:** `server closed the connection unexpectedly` (the coordinator backend `abort()`ed; PostgreSQL restarts its processes, the container keeps running). LOG: `gsi fail point "after_local_commit" reached`.

**SH** on the index shard and the base shard: `SELECT gid FROM pg_prepared_xacts;` → leftovers `..._i` and `..._b`.

**C1** (psql reconnects by itself on the next command):
```sql
SELECT * FROM users WHERE id = 9000006;      -- 0 rows: still prepared, invisible
SELECT gsi_resolve(0);                       -- one per leftover participant (2-3: base + email/name index nodes)
SELECT * FROM users WHERE id = 9000006;      -- 1 row
```
LOG: `gsi resolver: COMMIT PREPARED ..._i` then `..._b`. **Say:** the resolver doesn't decide anything; it reads the coordinator's commit log for that xid: committed → finish the commit.

### 6b. Before the decision → roll back
```sql
SET gsi.fail_point = 'after_prepare';
INSERT INTO users VALUES (9000007, 'lost@x', 'Lost');
-- reconnects
SELECT gsi_resolve(0);                       -- rolls back the leftovers
SELECT * FROM users WHERE id = 9000007;      -- 0 rows
```
**Say:** no local commit record = no decision = abort is safe.

### 6c. Automatic: the background worker
```sql
ALTER SYSTEM SET gsi.resolver_grace = '10s';  SELECT pg_reload_conf();
SET gsi.fail_point = 'after_local_commit';
INSERT INTO users VALUES (9000008, 'auto@x', 'Auto');
```
Don't resolve. **SH:** `SELECT gid FROM pg_prepared_xacts \watch 2` → the GIDs disappear within ~15 s; LOG shows `gsi resolver: COMMIT PREPARED ...` from the worker.

**Finish:** from the repo root, `python tests/checker.py` → `missing=0 orphaned=0 misplaced=0 prepared=0` for both indexes.

---

## Appendix: for questions

**Recheck protects against a wrong entry.** On the index node of `ghost@x` (`SELECT gsi_node_for('ghost@x'::text) + 1;`), insert a fake entry pointing at an existing row: `INSERT INTO gsi_users_email VALUES ('ghost@x', 9000001, <base node id of 9000001>);`. **C1:** `SELECT * FROM users WHERE email = 'ghost@x';` → 0 rows; `EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)` shows `Rows Removed by Filter: 1`. Delete the fake entry afterwards.

**The crash window between index and base commit.** `SET gsi.fail_point = 'mid_commit_prepared';` then an insert: the index entry is committed, the base row is still prepared. A lookup returns 0 rows (the entry points at an invisible row; the recheck/visibility handle it), never a wrong row. `SELECT gsi_resolve(0);` → 1, then the row is visible.

**Why writes aren't pushed down.** `EXPLAIN VERBOSE UPDATE users SET email = 'z' WHERE name = 'x';` → `UPDATE ... WHERE ctid = $1` per row, so the index can be maintained. An UPDATE that sets no indexed column is pushed down whole.

**Generic plans use the GSI too.** `SET plan_cache_mode = force_generic_plan; PREPARE p(text) AS SELECT * FROM users WHERE email = $1; EXPLAIN EXECUTE p('demo1-new@x');` → GsiLookup.

**Known v1 restriction.** `UPDATE users SET id = 1 WHERE id = 9000001;` → `ERROR: cannot update column "id" of a table with a GSI`.

**A shard that hangs instead of crashing.** `docker compose pause shardK`, then in **C1** `SET statement_timeout = '5s';` and an insert touching shard K → timeout, full rollback; `docker compose unpause shardK`.

## Reset after the demo
```
docker compose exec -T coordinator psql -U postgres -c "ALTER SYSTEM RESET gsi.resolver_grace" -c "SELECT pg_reload_conf()"
for s in shard1 shard2 shard3 shard4 shard5; do docker compose exec -T $s psql -U postgres -c "ALTER SYSTEM RESET log_statement" -c "SELECT pg_reload_conf()"; done
```
or wipe everything: `docker compose down -v`.
