# Implementation Plan (locked)

This is the build plan for `gsi_fdw`. It's written so that a team member or a coding agent can pick up any milestone and implement it without other context.

- **Why** the design is the way it is: [architecture_plan.md](architecture_plan.md).
- **What to build, in what order, with what exact interfaces:** this file.
- If this file and the architecture plan disagree, **this file wins**. Change both together.

PostgreSQL version: **16** everywhere. All PostgreSQL function names below are from `REL_16_STABLE`. Where a signature is marked *(verify)*, check it against the PG 16 headers before relying on it.

---

## 0. Invariants (never break these)

| # | Invariant | Where it's enforced |
|---|---|---|
| I1 | The coordinator's local commit record is the **only** decision point of a distributed transaction. | §5.2: forced xid, PREPARE before local commit |
| I2 | In every commit pass (the backend and the resolver), servers whose GID ends in `_i` get `COMMIT PREPARED` **before** servers ending in `_b`. | §5.4, §6 |
| I3 | Every GSI write runs on a postgres_fdw connection, inside that connection's remote transaction, and is covered by the same 2PC. | §4.3 |
| I4 | The read path never returns a row without rechecking the original quals on the fetched base row. | §8.4 |
| I5 | A single C helper computes index placement; every path (write, read, backfill, checker) uses it or an exact equivalent. | §3.3 |
| I6 | Nothing in `XACT_EVENT_COMMIT` or `XACT_EVENT_ABORT` may `ereport(ERROR)`. Use `WARNING`. | §5.4, §5.5 |
| I7 | The coordinator runs with `synchronous_commit = on`, so a returned commit is a flushed commit record. | §1.2 |

---

## 1. Cluster

### 1.1 Containers
`docker/docker-compose.yml` defines four services from one image (`docker/Dockerfile`):

| Service | Role | Host port |
|---|---|---|
| `coordinator` | planner hook, FDW, 2PC, resolver | 5432 |
| `shard1` | node 0: base partition 0 + index partition 0 | 5433 |
| `shard2` | node 1 | 5434 |
| `shard3` | node 2 | 5435 |

They reach each other through Docker DNS names (`shard1:5432`, ...). No static IPs.

**Dockerfile:** `FROM postgres:16` (Debian), install `build-essential postgresql-server-dev-16`, copy `src/gsi_fdw`, run `make USE_PGXS=1 && make USE_PGXS=1 install`. The same image runs on every node; only the coordinator loads the library.

### 1.2 Settings
`docker/conf/common.conf` (all nodes):
```
listen_addresses = '*'
max_prepared_transactions = 100      # >= max concurrent writers
max_connections = 100
```
`docker/conf/coordinator.conf` (added on top):
```
shared_preload_libraries = 'gsi_fdw'
synchronous_commit = on              # I7
gsi.resolver_database = 'postgres'
```
Async stretch only, on data nodes: `wal_level = logical`, `max_replication_slots = 10`, `max_wal_senders = 10`.

`pg_hba.conf`: `host all all all trust`. This is a private test network.

### 1.3 Init SQL
- `docker/init/datanode.sql`, run on every shard:
  ```sql
  CREATE TABLE users_shard (id bigint PRIMARY KEY, email text NOT NULL, name text);
  ```
  Index tables are created by `gsi_create()` (§7), not here.
- `docker/init/coordinator.sql`:
  ```sql
  CREATE EXTENSION gsi_fdw;
  CREATE SERVER shard1 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard1', port '5432', dbname 'postgres');
  CREATE SERVER shard2 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard2', port '5432', dbname 'postgres');
  CREATE SERVER shard3 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard3', port '5432', dbname 'postgres');
  CREATE USER MAPPING FOR PUBLIC SERVER shard1 OPTIONS (user 'postgres');
  CREATE USER MAPPING FOR PUBLIC SERVER shard2 OPTIONS (user 'postgres');
  CREATE USER MAPPING FOR PUBLIC SERVER shard3 OPTIONS (user 'postgres');

  INSERT INTO gsi.gsi_node VALUES (0, 'shard1'), (1, 'shard2'), (2, 'shard3');

  CREATE TABLE users (id bigint NOT NULL, email text NOT NULL, name text) PARTITION BY HASH (id);
  CREATE FOREIGN TABLE users_p0 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 0)
      SERVER shard1 OPTIONS (table_name 'users_shard');
  CREATE FOREIGN TABLE users_p1 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 1)
      SERVER shard2 OPTIONS (table_name 'users_shard');
  CREATE FOREIGN TABLE users_p2 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 2)
      SERVER shard3 OPTIONS (table_name 'users_shard');
  ```
  A foreign-table partition can't have a primary key on the coordinator. Uniqueness of `id` is enforced by each shard's PK, and hash routing sends a given id to one shard.

---

## 2. Repository layout

```
docker/
  Dockerfile
  docker-compose.yml
  conf/{common.conf, coordinator.conf, pg_hba.conf}
  init/{coordinator.sql, datanode.sql}
src/gsi_fdw/                 copy of contrib/postgres_fdw @ REL_16_STABLE, then modified
  Makefile  gsi_fdw.control  gsi_fdw--1.0.sql
  postgres_fdw.h             (kept name; add GSI declarations here or in gsi.h)
  connection.c               2PC changes (§5)
  postgres_fdw.c             write interception (§4)
  deparse.c  option.c  shippable.c      (rename only)
  gsi.h                      shared GSI types and prototypes
  gsi_catalog.c              catalog lookups and placement hash (§3)
  gsi_modify.c               per-row index maintenance (§4.3)
  gsi_resolver.c             gsi_resolve() + background worker (§6)
  gsi_failpoint.c            gsi.fail_point GUC (§9.2)
  gsi_planner.c              set_rel_pathlist_hook + CustomPath (§8)
  gsi_scan.c                 CustomScan executor (§8)
  gsi_admin.c                gsi_create / gsi_drop (§7)
src/gsi_decoder/             stretch: logical decoding plugin
tests/
  conftest.py                psycopg fixtures for the 4 nodes
  test_*.py                  pytest, one file per milestone
  checker.py                 consistency checker (§9.1)
benchmarks/                  pgbench scripts + runner
docs/
```

**Style:** follow postgres_fdw's code style (PostgreSQL C conventions, tabs, `ereport`, palloc). Keep the internal `postgres*` C function names in the copied files so diffs against upstream stay readable. Rename only SQL-visible names and GUCs.

---

## 3. Catalog and placement

### 3.1 Extension SQL (`gsi_fdw--1.0.sql`)
Start from `postgres_fdw--1.0.sql` plus the `1.0--1.1` upgrade content, merged into one `1.0` script. Rename the handler and validator to `gsi_fdw_handler` / `gsi_fdw_validator` and the wrapper to `gsi_fdw`. Then add:

```sql
CREATE SCHEMA gsi;

CREATE TABLE gsi.gsi_node (
    node_id     int PRIMARY KEY,          -- 0..N-1, dense
    server_name name NOT NULL UNIQUE      -- a gsi_fdw foreign server
);

CREATE TABLE gsi.gsi_index (
    index_name  name PRIMARY KEY,         -- data-node table is 'gsi_' || index_name
    base_table  regclass NOT NULL,        -- the partitioned parent, e.g. users
    column_name name NOT NULL,
    is_unique   boolean NOT NULL DEFAULT false,
    mode        text NOT NULL DEFAULT 'sync'  CHECK (mode IN ('sync','async')),
    status      text NOT NULL DEFAULT 'ready' CHECK (status IN ('write-only','ready')),
    UNIQUE (base_table, column_name)
);

CREATE FUNCTION gsi_create(base regclass, col name, is_unique boolean DEFAULT false,
                           mode text DEFAULT 'sync', online boolean DEFAULT false)
    RETURNS name AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION gsi_drop(index_name name) RETURNS void AS 'MODULE_PATHNAME' LANGUAGE C;
CREATE FUNCTION gsi_resolve() RETURNS int AS 'MODULE_PATHNAME' LANGUAGE C;   -- returns # resolved
CREATE FUNCTION gsi_node_for(val anyelement) RETURNS int AS 'MODULE_PATHNAME' LANGUAGE C;
```

`gsi_node_for` exposes the placement hash to SQL so tests and the checker use exactly the same function (I5).

**v1 restrictions** (checked in `gsi_create`, otherwise `ERROR`):
- one indexed column;
- base table partitioned by HASH;
- every partition is a `gsi_fdw` foreign table whose server is in `gsi_node`;
- the indexed column type has a default hash opclass;
- the primary key column is named `id`, of type `bigint`.

### 3.2 Catalog access (`gsi_catalog.c`)
```c
typedef struct GsiIndexInfo
{
    char       *index_name;        /* e.g. "users_email" */
    char       *remote_table;      /* "gsi_users_email" */
    Oid         base_relid;        /* partitioned parent */
    AttrNumber  attnum;            /* indexed column, in parent's numbering */
    Oid         atttype;
    Oid         attcollation;
    bool        is_unique;
    bool        is_async;
    bool        is_ready;          /* status == 'ready' */
} GsiIndexInfo;

/* NULL if relid (parent or partition) has no GSI. Allocated in CurrentMemoryContext. */
GsiIndexInfo *gsi_get_index_for_rel(Oid relid);

int         gsi_num_nodes(void);
Oid         gsi_node_server(int node_id);          /* ForeignServer oid */
int         gsi_server_node(Oid serverid);         /* -1 if not a GSI node */
```
- **v1 implementation:** SPI query per call, no cache. It's called once per partition per statement (`BeginForeignModify`) and once per planning of the parent rel, which is cheap enough.
- If `relid` is a partition, map it to its parent with `get_partition_parent(relid, false)`.
- Wrap SPI in `SPI_connect()` / `SPI_finish()`; it's legal inside a running query.
- *Later optimization:* a backend-local hash cache invalidated with `CacheRegisterRelcacheCallback`. Not needed for checkpoint 1.

### 3.3 Placement hash (I5)
```c
#include "partitioning/partbounds.h"     /* HASH_PARTITION_SEED */

int
gsi_node_for_datum(Datum value, Oid typid, Oid collid, int nnodes)
{
    TypeCacheEntry *tce = lookup_type_cache(typid, TYPECACHE_HASH_EXTENDED_PROC_FINFO);
    uint64      h;

    if (!OidIsValid(tce->hash_extended_proc_finfo.fn_oid))
        elog(ERROR, "type %u has no extended hash function", typid);
    h = DatumGetUInt64(FunctionCall2Coll(&tce->hash_extended_proc_finfo, collid,
                                         value, UInt64GetDatum(HASH_PARTITION_SEED)));
    return (int) (h % (uint64) nnodes);
}
```
`NULL` keys aren't indexed (the column is `NOT NULL` in v1; if `NULL` ever appears, skip index maintenance for that row).

---

## 4. Synchronous write path (`postgres_fdw.c`, `gsi_modify.c`)

### 4.1 Disable pushdown and batching
- `postgresPlanDirectModify(root, plan, resultRelation, subplan_index)`: at the top, look up the RTE's relid. If `gsi_get_index_for_rel(relid) != NULL`, `return false`.
- `postgresGetForeignModifyBatchSize(resultRelInfo)`: if the relation has a GSI, `return 1`. This covers INSERT into the parent and `COPY`, both of which go through tuple routing and `BeginForeignInsert`.

### 4.2 Fetch the old key (`postgresAddForeignUpdateTargets`)
PG 16 signature:
```c
static void postgresAddForeignUpdateTargets(PlannerInfo *root, Index rtindex,
                                            RangeTblEntry *target_rte,
                                            Relation target_relation);
```
After the existing `ctid` row-identity var, if the relation has a GSI:
```c
Var *var = makeVar(rtindex, gsi->attnum_in_this_partition, atttype, atttypmod, attcollation, 0);
add_row_identity_var(root, var, rtindex, "gsi_old_key");
```
- Map the attnum to the partition's numbering by **column name** (`get_attnum(RelationGetRelid(target_relation), column_name)`). Partition attnums can differ from the parent's.
- The row-identity var makes the remote fetch include the column: `SELECT ..., email, ctid FROM users_shard ... FOR UPDATE`.

In `create_foreign_modify(...)` (called by both `BeginForeignModify` and `BeginForeignInsert`), for UPDATE/DELETE:
```c
fmstate->gsiOldKeyAttno = ExecFindJunkAttributeInTlist(subplan->targetlist, "gsi_old_key");
```
This is the same pattern postgres_fdw already uses for `ctidAttno`.

### 4.3 Per-row maintenance
Add to `PgFdwModifyState`:
```c
GsiIndexInfo *gsi;              /* NULL if no GSI or gsi->is_async */
int         gsiBaseNode;        /* node id of this partition's server */
AttrNumber  gsiNewKeyAttno;     /* indexed column in this partition */
AttrNumber  gsiOldKeyAttno;     /* junk attno in planSlot (UPDATE/DELETE) */
```
In `create_foreign_modify`, if `gsi` is set: fill these fields and call `GetTopFullTransactionId()` (forced xid, §5.2).

After the existing base write succeeds in each callback (`execute_foreign_modify` returns), call:

| Callback | Call |
|---|---|
| `postgresExecForeignInsert` | `gsi_index_insert(fmstate, new_key, id)` |
| `postgresExecForeignDelete` | `gsi_index_delete(fmstate, old_key, id)` |
| `postgresExecForeignUpdate` | if `old_key` and `new_key` differ (`datumIsEqual` / type equality): delete old, insert new. An `id` change (moving partitions) is executed by PostgreSQL as DELETE + INSERT, so it's already covered. |

Values:
- `new_key`, `id`: from the result `slot` (`slot_getattr`).
- `old_key`: from `planSlot` at `gsiOldKeyAttno`.
- For DELETE, `id` also comes from `planSlot`: add `id` as a second row-identity var `gsi_old_id` the same way.

`gsi_modify.c`:
```c
void gsi_index_insert(PgFdwModifyState *fm, Datum key, int64 id);
void gsi_index_delete(PgFdwModifyState *fm, Datum key, int64 id);
```
Each one:
1. `node = gsi_node_for_datum(key, ...)`, `server = gsi_node_server(node)`.
2. `user = GetUserMapping(GetUserId(), server)`, `conn = GetConnection(user, false, NULL)`. This starts the remote transaction on that server if needed, at the right savepoint depth.
3. `gsi_mark_conn_index(user->umid)`: sets `entry->gsi_role = 'i'` and `entry->gsi_dirty = true` (§5.1).
4. Send with `PQsendQueryParams`, then `pgfdw_get_result(conn, sql)` *(verify the PG 16 signature)*. On a non-OK result, call `pgfdw_report_error(ERROR, res, conn, true, sql)`.
   - insert: `INSERT INTO gsi_users_email (key, id, shard) VALUES ($1, $2, $3)`
   - delete: `DELETE FROM gsi_users_email WHERE key = $1 AND id = $2`
   - The key is sent as text through the type's output function, with an explicit cast in SQL (`$1::text`, using `format_type_be(atttype)`).
5. A unique violation surfaces here as a normal `ERROR` (SQLSTATE 23505) at the user's statement.

The base write path also marks its connection dirty: in `execute_foreign_modify`, call `gsi_mark_conn_dirty(fmstate->conn)`. Its role stays `'b'` unless an index write also used that connection.

### 4.4 Remote session settings
In `configure_remote_session()` (connection.c), add:
```sql
SET lock_timeout = '<gsi.remote_lock_timeout>'     -- GUC, default '5s'
```
Each node sees only its own wait-for graph, so a deadlock that spans two nodes is **never detected**. Example: T1 holds a base row on shard1 and waits for an index key on shard3, while T2 holds the index key and waits for T1's row. The lock timeout turns that into an `ERROR`, the transaction aborts and the client retries. The same timeout bounds the wait on locks held by a prepared transaction whose coordinator is down.

---

## 5. Two-phase commit (`connection.c`)

### 5.1 State
Add to `ConnCacheEntry`:
```c
bool        gsi_dirty;          /* wrote something in this xact */
char        gsi_role;           /* 'i' if any index write, else 'b' */
bool        gsi_prepared;       /* PREPARE TRANSACTION succeeded */
char        gsi_gid[GIDSIZE];
```
Plus a file-level `static bool gsi_xact_2pc = false;`, set by `gsi_mark_conn_*`. If it's false at commit, postgres_fdw's original path runs unchanged, so tables without a GSI behave exactly like postgres_fdw.

Exported helpers: `gsi_mark_conn_index(Oid umid)`, `gsi_mark_conn_dirty(PGconn *conn)` (look up the entry by conn, scanning the hash; N is tiny).

**Connections are cached per user mapping, not per server.** With `CREATE USER MAPPING FOR PUBLIC` (§1.3) that's one per server. Tests must not create per-user mappings.

### 5.2 Forced xid and GID
- **Forced xid:** `GetTopFullTransactionId()` is called at the first GSI write (§4.3), so the coordinator always writes a commit record.
- **GID:**
  ```c
  snprintf(entry->gsi_gid, GIDSIZE, "gsi_" UINT64_FORMAT "_%s_%c",
           U64FromFullTransactionId(GetTopFullTransactionId()),
           server->servername, entry->gsi_role);
  ```
  Example: `gsi_5001_shard3_i`. Server names must match `[a-z0-9]+`; `gsi_create` checks this.

### 5.3 PRE_COMMIT: prepare
In `pgfdw_xact_callback`, `case XACT_EVENT_PRE_COMMIT`, when `gsi_xact_2pc` is set, for each entry with `xact_depth > 0`:
```
if (!entry->gsi_dirty)
    run postgres_fdw's normal "COMMIT TRANSACTION" for it    -- read-only participant, nothing to protect
else
    do_sql_command(conn, "PREPARE TRANSACTION '<gid>'")      -- ERROR on failure → whole xact aborts
    entry->gsi_prepared = true
```
After the loop: `gsi_fail_point("after_prepare")`.

- Ignore postgres_fdw's `parallel_commit` option when `gsi_xact_2pc` is set; v1 prepares sequentially.
- Keep postgres_fdw's `changing_xact_state` bookkeeping around each command.

### 5.4 COMMIT: ordered commit
`case XACT_EVENT_COMMIT` runs **after** the local commit record is flushed. Never `ERROR` here (I6).
```
gsi_fail_point("after_local_commit")
for pass in ('i', 'b'):                                    -- I2
    for each entry with gsi_prepared && gsi_role == pass:
        ok = pgfdw_exec_cleanup_query(conn, "COMMIT PREPARED '<gid>'", true)   (verify)
        if (!ok) ereport(WARNING, "left for resolver: %s", gid)
    if (pass == 'i') gsi_fail_point("mid_commit_prepared")
```
`pgfdw_exec_cleanup_query` is postgres_fdw's non-throwing, timeout-bounded helper, used for abort cleanup *(verify the name and signature in PG 16)*. If it doesn't fit, write a small equivalent: `PQsendQuery`, then `pgfdw_get_cleanup_result` with a 30 s timeout.

### 5.5 ABORT: roll back
`case XACT_EVENT_ABORT`. Never `ERROR` here (I6).
```
for each entry with xact_depth > 0 or gsi_prepared:
    if (entry->gsi_prepared)
        pgfdw_exec_cleanup_query(conn, "ROLLBACK PREPARED '<gid>'", true)   -- failure → resolver
    else
        existing pgfdw_abort_cleanup(entry, true)
```

### 5.6 Reset
After COMMIT and ABORT, for every entry:
- clear `gsi_dirty`, `gsi_role = 'b'`, `gsi_prepared`, `gsi_gid`;
- set `xact_depth = 0`, which postgres_fdw already does in `pgfdw_reset_xact_state`.

After PREPARE the remote session is **outside** any transaction, so the next use runs `BEGIN` again. Then set `gsi_xact_2pc = false`.

### 5.7 Client-issued PREPARE
`XACT_EVENT_PRE_PREPARE` keeps postgres_fdw's existing `ERROR` ("cannot PREPARE a transaction that has operated on postgres_fdw foreign tables"). We don't support nested 2PC.

### 5.8 Subtransactions
Nothing new is needed. Index writes go through `GetConnection`, so postgres_fdw's `pgfdw_subxact_callback` already creates and rolls back remote savepoints for them. `gsi_role` and `gsi_dirty` may stay set after a subtransaction rolls back; that's harmless, because a PREPARE with nothing in it is still valid.

---

## 6. Resolver (`gsi_resolver.c`)

### 6.1 Core routine
```c
int gsi_resolve_all(int grace_seconds);   /* returns number of GIDs resolved */
```
1. For each node in `gsi.gsi_node`, open a **separate libpq connection, outside any remote transaction**. `COMMIT PREPARED` can't run inside a transaction block, so `GetConnection()` (which sends `BEGIN`) must not be used. Export a wrapper around connection.c's static `connect_pg_server(server, user)`, e.g. `PGconn *gsi_connect_raw(ForeignServer *, UserMapping *)`, and `PQfinish` it after use.
2. On each node:
   ```sql
   SELECT gid FROM pg_prepared_xacts
   WHERE gid LIKE 'gsi\_%' AND prepared < now() - make_interval(secs => $grace)
   ```
3. Parse `gsi_<fxid>_<server>_<role>`. Group the GIDs by `fxid`.
4. For each `fxid`, get the status on the coordinator with SPI: `SELECT pg_xact_status($1::xid8)`.
   - `committed`: `COMMIT PREPARED` for all `_i` GIDs of this fxid, then all `_b` GIDs (I2).
   - `aborted`: `ROLLBACK PREPARED` for all of them.
   - `in progress`: skip.
   - `NULL` (xid too old, CLOG truncated) or an error (xid in the future, e.g. GIDs from a different coordinator): `WARNING` and skip. These need a human; they can't be decided safely.
5. Treat "prepared transaction ... does not exist" (SQLSTATE 42704) as success: a live backend finished it first.
6. `LOG` every action: `gsi resolver: COMMIT PREPARED gsi_5001_shard2_b (coordinator committed)`.

- `gsi_resolve()` is the SQL function `gsi_resolve_all(0)`, for tests and manual use. It's milestone M6.
- **Why "in progress" can't get stuck:** a crashed backend makes the postmaster restart the instance, and crash recovery marks unfinished xids as aborted.

### 6.2 Background worker (M7)
Registered in `_PG_init` only when `process_shared_preload_libraries_in_progress`:
```c
BackgroundWorker w = {0};
w.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
w.bgw_start_time = BgWorkerStart_RecoveryFinished;
w.bgw_restart_time = 5;
snprintf(w.bgw_library_name, BGW_MAXLEN, "gsi_fdw");
snprintf(w.bgw_function_name, BGW_MAXLEN, "gsi_resolver_main");
snprintf(w.bgw_name, BGW_MAXLEN, "gsi resolver");
RegisterBackgroundWorker(&w);
```
`gsi_resolver_main`:
1. `pqsignal(SIGTERM, die)`, then `BackgroundWorkerUnblockSignals()`.
2. `BackgroundWorkerInitializeConnection(gsi_resolver_database, NULL, 0)`.
3. Loop:
   - `StartTransactionCommand()`, `PushActiveSnapshot(GetTransactionSnapshot())`;
   - `gsi_resolve_all(gsi_resolver_grace_s)` inside `PG_TRY`, logging errors and continuing;
   - `PopActiveSnapshot()`, `CommitTransactionCommand()`;
   - `WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH, gsi_resolver_interval_ms, PG_WAIT_EXTENSION)`, `ResetLatch`, `CHECK_FOR_INTERRUPTS()`.

### 6.3 GUCs (define in `_PG_init`, prefix `gsi.`)
| GUC | Type | Default | Context |
|---|---|---|---|
| `gsi.resolver_database` | string | `postgres` | `PGC_POSTMASTER` |
| `gsi.resolver_interval_ms` | int | 5000 | `PGC_SIGHUP` |
| `gsi.resolver_grace_s` | int | 10 | `PGC_SIGHUP` |
| `gsi.remote_lock_timeout` | string | `5s` | `PGC_USERSET` |
| `gsi.fail_point` | string | `''` | `PGC_SUSET` |

Rename postgres_fdw's own GUC (`postgres_fdw.application_name`) to `gsi_fdw.application_name`.

---

## 7. Admin functions (`gsi_admin.c`)

`gsi_create(base, col, is_unique, mode, online)`:
1. Validate the v1 restrictions (§3.1).
2. Name it `<table>_<col>`; the remote table is `gsi_<table>_<col>`.
3. On **every** node, over a `GetConnection` connection so it's covered by 2PC:
   ```sql
   CREATE TABLE gsi_users_email (key <coltype> NOT NULL, id bigint NOT NULL, shard int NOT NULL,
                                 PRIMARY KEY (key, id));
   CREATE UNIQUE INDEX gsi_users_email_key ON gsi_users_email (key);   -- if is_unique
   ```
   Mark each connection as index-role and dirty, so DDL commits through 2PC as well. PostgreSQL supports PREPARE after DDL on ordinary tables.
4. Insert the catalog row:
   - `online = false`: status `ready`. **Precondition:** the base table is empty (check `SELECT 1 FROM base LIMIT 1`, else `ERROR "table not empty; use online => true"`).
   - `online = true`: status `write-only`, then run the online build (§10.2, stretch).

`gsi_drop(name)`: delete the catalog row, then `DROP TABLE` on every node, in one distributed transaction.

---

## 8. Read path (`gsi_planner.c`, `gsi_scan.c`)

### 8.1 Hook
In `_PG_init`: save `prev_set_rel_pathlist_hook`, then install `gsi_set_rel_pathlist`. Always chain to the previous hook first.

### 8.2 When to add the path
All of these must hold:
- `rte->rtekind == RTE_RELATION && rte->relkind == RELKIND_PARTITIONED_TABLE`;
- `gsi_get_index_for_rel(rte->relid)` exists, `is_ready`, and isn't async;
- `root->parse->resultRelation != rti`: not the target of UPDATE/DELETE, since v1 doesn't support that;
- `root->parse->rowMarks == NIL`: no `FOR UPDATE` / `FOR SHARE`. v1 falls back to the normal plan, which locks on the shards;
- `rel->baserestrictinfo` has a clause `OpExpr(Var(attnum) = Const|Param)` (either side), where:
  - the Var's `varno == rti` and its `varattno` is the indexed column;
  - the operator is the type's default btree equality (`get_opfamily_member` / `op_hashjoinable` check);
  - the Const is not null.

### 8.3 Path and plan
- `CustomPath`, with `path.pathtype = T_CustomScan`, `parent = rel`, `pathtarget = rel->reltarget`.
- `custom_private = list_make2(gsi index name as String, the matched clause)`.
- **Cost:** `startup = 0`, `total = 0.1 * min(total_cost of paths already in rel->pathlist)`. This always wins when it applies, and it's simple and deterministic. It's fine for v1 and should be documented in the report.
- `rows = rel->rows`.
- `PlanCustomPath`: build a `CustomScan` with `scan.scanrelid = rel->relid`, `scan.plan.targetlist = tlist`, and `scan.plan.qual = extract_actual_clauses(clauses, false)`. **All** quals are kept, and `ExecScan` evaluates them on every fetched row, which is the recheck (I4). Put the key expression in `custom_exprs`.
- Name it `"GsiLookup"` in `CustomPathMethods`, `CustomScanMethods` and `CustomExecMethods`. Register with `RegisterCustomScanMethods` in `_PG_init`.

### 8.4 Executor
State: the list of partitions (`find_inheritance_children(base_relid, NoLock)`), each with its foreign server, remote table name (`table_name` / `schema_name` options) and remote column names (`column_name` option or attname); the result buffer; and the current position.

`BeginCustomScan`: `ExecInitExpr` the key expression. The scan slot uses the parent's tuple descriptor.

On the first `ExecCustomScan` call, via `ExecScan(node, gsi_next, gsi_recheck)`:
1. Evaluate the key, then `node = gsi_node_for_datum(...)`.
2. **Hop 1:** on `GetConnection` for that node: `SELECT id, shard FROM gsi_<name> WHERE key = $1::<type>`.
3. Group the ids by `shard` (node id). Map node id to server to the partition on that server.
4. **Hop 2:** for each group: `SELECT <cols in parent attnum order> FROM <remote table> WHERE id = ANY($1::bigint[])`.
5. Convert each result row to a virtual tuple with each column type's input function (`InputFunctionCall`) and buffer it.

`gsi_next` returns buffered tuples one by one. `ExecScan` applies `scan.plan.qual` (the recheck). `ReScanCustomScan` clears the buffer.

- Reads use `GetConnection`, so they join the session's remote transactions (REPEATABLE READ per server, same as postgres_fdw scans).
- `EXPLAIN` should show `Custom Scan (GsiLookup) on users` plus the index name, via `ExplainCustomScan`.

### 8.5 Not in v1
`IN (...)` lists, range predicates, joins where `users` is parameterized (`Param` from an outer rel works only if it's in `baserestrictinfo`), and `FOR UPDATE`. Ranges are optional later: query all index nodes, then the same hop 2.

---

## 9. Testing and fault injection

### 9.1 Consistency checker (`tests/checker.py`)
Connect to each shard directly.
```
base  = { (id, email, node) for each node, for row in users_shard }
index = { (key, id, shard)  for each node, for row in gsi_users_email }
check every index row lives on node gsi_node_for(key)   (call SELECT gsi_node_for(key) on the coordinator)
assert set((e, i, n) for i, e, n in base) == index
assert no rows in pg_prepared_xacts LIKE 'gsi_%' on any node
```
It prints the missing and orphaned entries on failure, and is run at the end of every test.

### 9.2 Fail points (`gsi_failpoint.c`)
```c
void gsi_fail_point(const char *name)
{
    if (gsi_fail_point_guc && strcmp(gsi_fail_point_guc, name) == 0)
    {
        ereport(LOG, (errmsg("gsi fail point \"%s\" hit; aborting process", name)));
        abort();               /* simulates a crash: postmaster runs crash recovery */
    }
}
```
Points: `after_prepare` (§5.3), `after_local_commit` (§5.4), `mid_commit_prepared` (§5.4).

Test pattern:
1. `SET gsi.fail_point = 'X'` in one session (requires superuser).
2. Run a GSI write and `COMMIT`. The connection drops.
3. Wait for the coordinator to come back.
4. Call `gsi_resolve()` (M6) or wait for the background worker (M7).
5. Run the checker. Expected outcome per point:
   - `after_prepare`: the row is absent;
   - `after_local_commit`: the row is present;
   - `mid_commit_prepared`: the row is present.

Abort-path test: `docker pause shard2` during a transaction's PREPARE; expect an error, then `docker unpause`, run the resolver, run the checker.

### 9.3 Test files (pytest)
| File | Covers |
|---|---|
| `test_m1_cluster.py` | the four nodes are reachable; `gsi_fdw` is loaded; broadcast select works |
| `test_m3_write.py` | insert / update (key changed and unchanged) / delete, each followed by the checker; `EXPLAIN UPDATE` shows no direct modify |
| `test_m4_2pc.py` | `pg_prepared_xacts` is empty after commit; client `ROLLBACK` leaves nothing; an error in mid-transaction leaves nothing |
| `test_m5_unique.py` | a duplicate key with a different id is rejected; concurrent duplicates from two sessions: exactly one wins; non-unique GSI accepts duplicates |
| `test_m6_resolver.py` | the three fail points plus the paused shard |
| `test_m8_read.py` | `EXPLAIN` shows GsiLookup; results equal broadcast results on random keys; stale index row (insert one by hand) is filtered by the recheck |
| `test_stress.py` | 8 concurrent clients running random DML for 60 s (retrying on serialization and lock-timeout errors), then the checker |

---

## 10. Stretch work (only after M9)

### 10.1 Async mode
Follow the design in architecture_plan §9. Key points:
- `gsi_decoder` callbacks: `_PG_output_plugin_init` sets `startup_cb`, `begin_cb`, `change_cb`, `commit_cb`, emitting one text line per change.
- The applier is a background worker per node using `START_REPLICATION SLOT ... LOGICAL` over a replication connection.
- It confirms the slot position only after the index write commits.
- Async GSIs have `is_async = true`, so §4.3 skips index maintenance for them.
- `is_unique` must be false for async; `gsi_create` checks this.

### 10.2 Online build
Follow architecture_plan §10:
1. Status `write-only`.
2. Wait for older transactions, using `GetCurrentVirtualXIDs` + `VirtualXactLock`, the same approach as `CREATE INDEX CONCURRENTLY`.
3. Backfill chunks with `SELECT ... FOR SHARE` plus `INSERT ... ON CONFLICT (key, id) DO NOTHING`, one distributed transaction per chunk.
4. Run the checker for this index.
5. Status `ready`.

For the online build, the planner check (§8.2) and the write path (§4.3) must treat `write-only` correctly: the write path maintains the index, and the planner doesn't use it.

---

## 11. Milestones

Each milestone is done when its tests pass and the checker is clean. Do them in order; the dependencies are real.

| M | Deliverable | Depends on | Done when |
|---|---|---|---|
| **M1** | Docker cluster, init SQL, the unmodified postgres_fdw copy renamed to `gsi_fdw`, builds and loads | none | `test_m1` passes; `SELECT * FROM users` broadcasts |
| **M2** | Catalog tables, `gsi_node_for`, `gsi_create`/`gsi_drop` for empty tables (non-online) | M1 | the index table exists on 3 nodes; `gsi_node_for` agrees between SQL and C |
| **M3** | Write interception §4: no direct modify, no batching, old key, per-row index writes, `lock_timeout` | M2 | `test_m3` passes. Commits are still plain postgres_fdw commits at this point |
| **M4** | 2PC §5: forced xid, GIDs, PREPARE, ordered COMMIT PREPARED, abort path | M3 | `test_m4` passes |
| **M5** | Unique GSI tests | M4 | `test_m5` passes |
| **M6** | `gsi_resolve()` SQL function + fail points | M4 | `test_m6` passes with a manual resolve |
| **M7** | Resolver background worker | M6 | `test_m6` passes without a manual call |
| **M8** | Planner hook + GsiLookup CustomScan | M3 | `test_m8` passes |
| **M9** | Stress test + benchmarks (write: no index vs sync; read: GSI vs broadcast at 2/3/4 nodes) | M7, M8 | results and plots in `benchmarks/results/` |
| S1 | Async mode | M9 | checker is clean once the lag drains |
| S2 | Online build | M9 | building on a populated table under the stress workload ends clean |

**Checkpoint 1 = M1 to M6**, with M7 if time allows. M8 can be developed in parallel by one person after M3.

**Owners** (suggested):

| Person | Milestones |
|---|---|
| A | M1, M2, checker |
| B | M3 |
| C | M4, M5 |
| D | M6, M7, then M8 |

---

## 12. Known pitfalls (read before coding)

1. **Direct modify is the default path.** If index entries aren't changing on UPDATE, check that `postgresPlanDirectModify` returns false (`EXPLAIN VERBOSE` shows `Foreign Update` with a remote SQL that has no `ctid`).
2. **Partition attnums can differ from the parent's.** Always map columns by name.
3. **`COMMIT PREPARED` inside `BEGIN` fails.** The resolver needs raw connections (§6.1). In the backend, after PREPARE the session is already outside a transaction.
4. **An `ERROR` after the local commit makes PostgreSQL PANIC** ("cannot abort transaction, it was already committed"). Use `WARNING` only in COMMIT and ABORT callbacks (I6).
5. **A coordinator with only remote writes has no xid.** Forget `GetTopFullTransactionId()` and the resolver sees `pg_xact_status` of an invalid xid. Rule: force the xid at the first GSI write.
6. **Distributed deadlocks are invisible.** Rely on `gsi.remote_lock_timeout`, and make tests retry.
7. **`max_prepared_transactions` too low** makes PREPARE fail under load ("maximum number of prepared transactions reached"). Keep it ≥ client count.
8. **Leftover prepared transactions hold locks and block VACUUM.** If a test leaves any, the next test hangs. The checker asserts none remain.
9. **`synchronous_commit = off` on the coordinator breaks I1.** A crash could lose a commit the client was told about while shards were already committed. Keep it on.
