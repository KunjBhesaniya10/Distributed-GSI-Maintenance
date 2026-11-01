# Hands-On Playbook

A step-by-step script to bring up the cluster yourself and watch every feature work: writes, 2PC, uniqueness, the GSI read path, crash recovery (manual and automatic), stress and consistency checking.

Commands work in **PowerShell** and **Git Bash**. Run everything from the `docker/` folder unless stated otherwise. Each step says **what to expect**. If you see something else, check §11.

---

## Quick run (everything in one go)

[docker/playbook.sql](../docker/playbook.sql) does the setup: it (re)creates the unique GSI on `users.email`, loads 8 sample rows, prints where each row and index entry lives, and turns on statement logging on the shards (to see the 2PC commands). Example queries for live testing (read path, UPDATE/DELETE, 2PC) are in a comment at the end of the file.

```
cd docker
docker compose down -v
docker compose up -d --build
docker compose exec -T coordinator psql -U postgres -X -f - < playbook.sql      # §3–§8, ~1 min
docker compose pause shard2                                                     # §9 (needs docker)
docker compose exec -T coordinator psql -U postgres -c "SET statement_timeout='5s'" -c "INSERT INTO users VALUES (7,'pause@x','X')"
docker compose unpause shard2
cd ../tests && python checker.py && python -m pytest -v .                     # §10
```
PowerShell has no `<` redirection, so there pipe the file in instead: `Get-Content playbook.sql | docker compose exec -T coordinator psql -U postgres -X -f -`.

The sections below explain each feature in more detail, if you want to go further (uniqueness races, crash recovery).

---

## 0. Prerequisites (once)

| Need | Check |
|---|---|
| Docker Desktop **running** | `docker info` prints a server version |
| Python 3 + packages | `pip install psycopg2-binary pytest` |
| Free ports 5432–5435 | stop any local PostgreSQL first |

```
cd C:\Users\Kunj\Desktop\cs631\Distributed-GSI-Maintenance\docker
```

---

## 1. Build and start the cluster

```
docker compose down -v                # wipe any old cluster (init SQL only runs on empty volumes)
docker compose up -d --build          # builds the image (compiles gsi_fdw), starts 4 nodes
docker compose ps
```
**Expect:** four containers, `coordinator`, `shard1`, `shard2`, `shard3`, all `(healthy)`. The build prints gcc lines with no `warning:`.

Optional, to see the compile: `docker compose build --no-cache --progress=plain 2>&1 | findstr gcc` (PowerShell) or `| grep gcc` (bash).

---

## 2. Open sessions

Open **4 terminals** (all in `docker/`). Keep them open for the whole playbook.

| Terminal | Command | Prompt |
|---|---|---|
| **C** coordinator | `docker compose exec coordinator psql -U postgres` | `postgres=#` |
| **S1** shard1 | `docker compose exec shard1 psql -U postgres` | |
| **S2** shard2 | `docker compose exec shard2 psql -U postgres` | |
| **S3** shard3 | `docker compose exec shard3 psql -U postgres` | |

Host ports, if you prefer a GUI client such as pgAdmin or DBeaver (user `postgres`, no password): coordinator 5432, shard1 5433, shard2 5434, shard3 5435.

---

## 3. Inspect the setup

**C:**
```sql
\dx gsi_fdw                                  -- extension installed
SHOW shared_preload_libraries;               -- gsi_fdw
SELECT * FROM gsi.gsi_node;                  -- 0 shard1 | 1 shard2 | 2 shard3
\d+ users                                    -- partitioned by hash(id), 3 foreign partitions
SELECT backend_type, state FROM pg_stat_activity WHERE backend_type = 'gsi resolver';
```
**Expect:** the extension listed, three nodes, `users` with partitions `users_p0..p2`, and **one `gsi resolver` row** (the M7 background worker).

**S1 / S2 / S3:**
```sql
\dt                                          -- only users_shard (no index table yet)
```

---

## 4. Create the global index

**C:**
```sql
SELECT gsi_create('users', 'email', true);   -- unique GSI on email
SELECT * FROM gsi.gsi_index;
```
**Expect:** `users_email`, and a catalog row with `is_unique = t`, `status = ready`.

**S1/S2/S3:** `\d gsi_users_email`. Every node now has `gsi_users_email(key, id, shard)` with PK `(key, id)` and a unique index on `key`. All three appeared atomically through 2PC.

### Turn on statement logging on the shards (to watch 2PC later)
**S1, S2, S3** (each):
```sql
ALTER SYSTEM SET log_statement = 'all';
SELECT pg_reload_conf();
```

---

## 5. Writes: base row and index entry on different nodes

**C:**
```sql
INSERT INTO users VALUES (2, 'alice@x', 'Alice'), (3, 'bob@x', 'Bob'), (1, 'frank@x', 'Frank');

-- where did each row go (base) and where does its entry live (index)?
SELECT id, email, tableoid::regclass AS base_partition, gsi_node_for(email) AS index_node FROM users;
```
**Expect** (`users_pN` lives on node N; node 0 = shard1, 1 = shard2, 2 = shard3):
```
 id |  email  | base_partition | index_node
  2 | alice@x | users_p0       |          2     base on shard1, entry on shard3
  3 | bob@x   | users_p1       |          2     base on shard2, entry on shard3
  1 | frank@x | users_p2       |          0     base on shard3, entry on shard1
```
Base row and index entry live on **different nodes**. That's what makes the index *global*.

**S1/S2/S3:**
```sql
SELECT * FROM users_shard;        -- base rows on this node
SELECT * FROM gsi_users_email;    -- index entries on this node; "shard" = node of the base row
```
**Expect:** every row appears exactly once in some `users_shard`, and exactly once in some `gsi_users_email`, on the node `gsi_node_for(email)` printed above.

### Watch the 2PC in the logs
In a **5th terminal** (in `docker/`):
```
docker compose logs --since 2m shard1 shard2 shard3 | findstr /i "PREPARE"       (PowerShell)
docker compose logs --since 2m shard1 shard2 shard3 | grep -i "PREPARE"          (bash)
```
**Expect:** lines like:
```
shard1 | LOG: statement: PREPARE TRANSACTION 'gsi_744_shard1_i'
shard2 | LOG: statement: PREPARE TRANSACTION 'gsi_744_shard2_b'
shard3 | LOG: statement: PREPARE TRANSACTION 'gsi_744_shard3_i'
shard1 | LOG: statement: COMMIT PREPARED 'gsi_744_shard1_i'      <- index participants first
shard3 | LOG: statement: COMMIT PREPARED 'gsi_744_shard3_i'
shard2 | LOG: statement: COMMIT PREPARED 'gsi_744_shard2_b'      <- base-only participant last
```
(The order among the `_i` participants can vary; every `_i` comes before every `_b`. Earlier lines with all-`_i` GIDs come from `gsi_create`, whose DDL also goes through 2PC.)
These show the same coordinator xid in every GID, PREPARE on every participant, and `COMMIT PREPARED` to the `_i` participants before the `_b` ones. A node that is both base and index for the transaction is `_i`.

### UPDATE and DELETE
**C:**
```sql
UPDATE users SET email = 'robert@x' WHERE id = 3;   -- key change: old entry deleted, new one inserted
UPDATE users SET name  = 'Rob'      WHERE id = 3;   -- non-key change: index untouched
DELETE FROM users WHERE id = 1;
EXPLAIN VERBOSE UPDATE users SET email = 'z' WHERE name = 'x';
```
**Expect:**
- On the shards, `bob@x` is gone, `robert@x` sits on `gsi_node_for('robert@x')`, and `frank@x` is gone everywhere.
- The `EXPLAIN` shows `Remote SQL: UPDATE public.users_shard SET email = $2 WHERE ctid = $1`, a per-row update rather than a pushed-down one. That's what lets us maintain the index. An UPDATE that sets no indexed column (e.g. `SET name = 'z'` when only email has a GSI) is pushed down whole, as in plain postgres_fdw: `EXPLAIN VERBOSE` shows `UPDATE public.users_shard SET name = ...` with no `ctid = $1`.

```sql
UPDATE users SET id = 999 WHERE id = 2;
```
**Expect:** `ERROR: cannot update column "id" of a table with a GSI` (documented v1 restriction).

---

## 6. Global uniqueness

**C:**
```sql
INSERT INTO users VALUES (200, 'alice@x', 'Fake');          -- duplicate email, different id/shard
```
**Expect:** `ERROR: duplicate key value violates unique constraint ...`. Run `SELECT * FROM users WHERE id = 200;` and you get 0 rows: the base insert on the other shard was rolled back too.

### Concurrent duplicates (two sessions)
Open a second coordinator session **C2** (`docker compose exec coordinator psql -U postgres`).

| Step | C | C2 |
|---|---|---|
| 1 | `BEGIN;` | `BEGIN;` |
| 2 | `INSERT INTO users VALUES (300, 'race@x', 'one');` | |
| 3 | | `INSERT INTO users VALUES (301, 'race@x', 'two');` → **blocks** |
| 4 | `COMMIT;` | → `ERROR: duplicate key ...` |
| 5 | | `ROLLBACK;` |

**Expect:** C2 waits on C's uncommitted index entry (the same index node, with PostgreSQL's unique-index lock), then fails. Only id 300 exists. If you `ROLLBACK` in C instead, C2's insert succeeds.

---

## 7. Read path (GsiLookup)

**C:**
```sql
EXPLAIN SELECT * FROM users WHERE email = 'alice@x';
```
**Expect:**
```
Custom Scan (GsiLookup) on users
  Filter: (email = 'alice@x'::text)        <- the recheck
  GSI: users_email
```
There's no `Append` over three foreign scans.

```sql
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) SELECT * FROM users WHERE email = 'alice@x';
SELECT * FROM users WHERE email = 'alice@x';
```
**Expect:** `Index Node: N` (= `gsi_node_for('alice@x')`), `Shards Fetched: 1`, one row.

### Compare with the broadcast plan
```sql
EXPLAIN SELECT * FROM users WHERE name = 'Alice';           -- not indexed: Append over all 3 shards
EXPLAIN SELECT * FROM users WHERE email || '' = 'alice@x';  -- expression: also broadcast
```

### Recheck protects against a wrong entry
**On the index node of `ghost@x`** (find it with `SELECT gsi_node_for('ghost@x'::text);` on **C**; 0 → S1, 1 → S2, 2 → S3). Insert a fake entry pointing at Alice's row (id 2, base on node 0):
```sql
INSERT INTO gsi_users_email VALUES ('ghost@x', 2, 0);   -- Alice's row: id 2, base node 0
```
**C:**
```sql
SELECT * FROM users WHERE email = 'ghost@x';
```
**Expect:** **0 rows**. The lookup fetches row 2, but the filter `email = 'ghost@x'` rejects it. `EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)` on the same query shows `Rows Removed by Filter: 1` and `Shards Fetched: 1`. Delete the fake entry afterwards (`DELETE FROM gsi_users_email WHERE key = 'ghost@x';`).

### Prepared statement with a parameter
```sql
SET plan_cache_mode = force_generic_plan;
PREPARE p(text) AS SELECT * FROM users WHERE email = $1;
EXPLAIN EXECUTE p('alice@x');       -- GsiLookup
EXECUTE p('robert@x');
```

---

## 8. Crash recovery (fault injection)

The fail point kills the coordinator backend at a chosen commit step, like a real crash. It's only active in the session that sets it.

**Give yourself time to look** before the background worker fixes things. **C:**
```sql
ALTER SYSTEM SET gsi.resolver_grace = '120s';
SELECT pg_reload_conf();
```

### 8a. Crash after the commit decision → roll forward
**C:**
```sql
SET gsi.fail_point = 'after_local_commit';
INSERT INTO users VALUES (400, 'crash@x', 'X');
```
**Expect:** `server closed the connection unexpectedly`. psql reconnects on the next command. The coordinator log (`docker compose logs --tail 20 coordinator`) shows `gsi fail point "after_local_commit" reached` and crash recovery.

**S1/S2/S3:**
```sql
SELECT gid, prepared FROM pg_prepared_xacts;
```
**Expect:** two leftovers, `gsi_<xid>_shardX_i` and `gsi_<xid>_shardY_b`. The row is invisible and locked.

**C** (new connection):
```sql
SELECT gsi_resolve(0);                         -- 2
SELECT * FROM users WHERE email = 'crash@x';   -- the row is there
```
**Expect:** 2 resolved; the prepared transactions are gone on every shard. Coordinator log: `gsi resolver: COMMIT PREPARED ..._i`, then `..._b`.

### 8b. Crash before the decision → roll back
```sql
SET gsi.fail_point = 'after_prepare';
INSERT INTO users VALUES (401, 'lost@x', 'X');
-- reconnect, then
SELECT gsi_resolve(0);                         -- 2
SELECT * FROM users WHERE id = 401;            -- 0 rows
```

### 8c. Crash between index and base commit → finish the base
```sql
SET gsi.fail_point = 'mid_commit_prepared';
INSERT INTO users VALUES (402, 'half@x', 'X');
```
**Before resolving**, look at the "harmless window":
- the index node of `half@x` already has the entry;
- the base shard still shows `gsi_..._b` in `pg_prepared_xacts`;
- **C:** `SELECT * FROM users WHERE email = 'half@x';` returns **0 rows**. The index points at a row that isn't visible yet, and the recheck and visibility handle it. No wrong row appears.

```sql
SELECT gsi_resolve(0);                         -- 1
SELECT * FROM users WHERE email = 'half@x';    -- now 1 row
```

### 8d. The background worker does it by itself (M7)
**C:**
```sql
ALTER SYSTEM SET gsi.resolver_grace = '10s';
SELECT pg_reload_conf();
SET gsi.fail_point = 'after_local_commit';
INSERT INTO users VALUES (403, 'auto@x', 'X');
```
**Don't** call `gsi_resolve`. Watch a shard:
```sql
SELECT gid FROM pg_prepared_xacts \watch 2
```
**Expect:** the GIDs disappear within about 10–15 s (10 s grace, worker runs every 5 s). The coordinator log shows `gsi resolver: COMMIT PREPARED ...` from the worker. Press Ctrl+C to stop `\watch`.

---

## 9. Shard failure during commit

```
docker compose pause shard2
```
**C:** insert a row whose base row lands on shard2 (id 7 hashes to `users_p1` = shard2):
```sql
SET statement_timeout = '5s';
INSERT INTO users VALUES (7, 'pause@x', 'X');
```
**Expect:** `ERROR: canceling statement due to statement timeout` after about 5 s. The transaction aborts and nothing is committed on the other shards.
```
docker compose unpause shard2
```
Then, on **C**, `SELECT gsi_resolve(0);` (cleans up anything left prepared; usually 0) and `SELECT * FROM users WHERE id = 7;` returns 0 rows.

---

## 10. Consistency checker, test suite, stress

From the **repo root**, in a new terminal:
```
cd tests
python checker.py
```
**Expect:** `rows=N missing=0 orphaned=0 misplaced=0 prepared=0`. This compares every base row with every index entry, checks each entry is on its hash node, and checks no 2PC is left over.

Full automated suite. **It resets the cluster data** (truncates `users`, re-creates the GSI):
```
python -m pytest -v .
```
**Expect:** `64 passed` in about 1.5 min. To see the stress numbers:
```
python -m pytest -s test_stress.py
```
**Expect:** `{'commits': ~9000+, 'aborts': ...}` for each run, and passing. The aborts are expected: retryable conflicts and unique violations.

Individual milestones: `python -m pytest -v test_m8_read.py`, `test_m7_worker.py`, and so on.

---

## 11. Clean up / reset

```
docker compose down          # stop, keep data
docker compose down -v       # stop and wipe everything (next "up" re-runs the init SQL)
```
To undo the settings from this playbook without wiping: on each shard `ALTER SYSTEM RESET log_statement; SELECT pg_reload_conf();`, and on the coordinator `ALTER SYSTEM RESET gsi.resolver_grace; SELECT pg_reload_conf();`.

---

## 12. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `error during connect ... docker_engine` | Docker Desktop isn't running; start it and wait for "Engine running" |
| port 5432 already in use | a local PostgreSQL is running; stop it, or change the host ports in `docker-compose.yml` |
| `relation "users" does not exist` | the volume was created by an older run without init; `docker compose down -v` then `up -d` |
| `column "email" of table "users" already has a GSI` | that column's GSI exists; `SELECT gsi_drop('users', 'email');` first (`gsi_drop('users')` drops all GSIs of the table) |
| `gsi_create requires an empty table` | `SELECT gsi_drop('users'); TRUNCATE users;` then create |
| statements hang | a prepared transaction from a crash holds locks: on **C**, `SELECT gsi_resolve(0);` |
| `could not serialize access ... GSI` (40001) | expected under concurrency (stale remote snapshot); retry the transaction |
| `canceling statement due to lock timeout` | cross-node lock wait hit `gsi.remote_lock_timeout` (5 s); retry |
| what happened? | `docker compose logs --tail 50 coordinator` (errors show `remote SQL command: ...`) |
| checker shows prepared > 0 | wait ~15 s for the worker, or run `SELECT gsi_resolve(0);` |
