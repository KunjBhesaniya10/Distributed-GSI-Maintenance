# gsi_fdw: How It Works

A guide to the `gsi_fdw` extension: how PostgreSQL loads and calls it, what each source file does (with the parts that matter to us), what the `.sql`, `.control` and `Makefile` files are for, and how to build, run and trace it.

- For **only our changes**, with line ranges, see [code_changes.md](code_changes.md).
- For **call stacks per statement**, see [call_stacks.md](call_stacks.md).

---

## 1. The big picture

```
client ──SQL──► coordinator (PostgreSQL 16 + gsi_fdw loaded)
                 │  users            partitioned table (routing shell, no data)
                 │   ├ users_p0 ─ FOREIGN TABLE ─► shard1.users_shard
                 │   ├ users_p1 ─ FOREIGN TABLE ─► shard2.users_shard
                 │   └ users_p2 ─ FOREIGN TABLE ─► shard3.users_shard
                 │  gsi.gsi_index, gsi.gsi_node   GSI catalog
                 │
                 └──libpq (ordinary SQL)──► shard1..3: stock PostgreSQL 16
                                              users_shard      base rows   (by hash(id))
                                              gsi_users_email  index rows  (by hash(email))
```

- **`gsi_fdw` runs only on the coordinator.** Data nodes are unmodified PostgreSQL and just receive SQL.
- It's a **copy of `postgres_fdw`** with GSI maintenance added:
  1. every write to a GSI table also writes its index entry on the node `hash(key) mod N`;
  2. the transaction then commits on all nodes with **two-phase commit** (2PC);
  3. `gsi_resolve()` finishes in-doubt transactions after a crash.

---

## 2. How PostgreSQL uses an FDW

An FDW is a shared library (`gsi_fdw.so`) plus SQL objects. PostgreSQL calls into it at fixed points:

| Mechanism | How it's wired | What it gives us |
|---|---|---|
| **Handler function** | `CREATE FOREIGN DATA WRAPPER gsi_fdw HANDLER gsi_fdw_handler`. The handler returns a struct `FdwRoutine` of function pointers ([postgres_fdw_handler, postgres_fdw.c:556](../src/gsi_fdw/postgres_fdw.c#L556)) | PG calls these pointers to plan and execute scans and writes on foreign tables |
| **Validator** | `VALIDATOR gsi_fdw_validator` ([option.c:73](../src/gsi_fdw/option.c#L73)) | checks `OPTIONS (host ..., table_name ...)` in `CREATE SERVER` and `CREATE FOREIGN TABLE` |
| **`_PG_init`** | runs when the `.so` is loaded (`shared_preload_libraries`) ([option.c:569](../src/gsi_fdw/option.c#L569)) | defines GUCs; we also call `gsi_init()` |
| **Planner hook + CustomScan** | `set_rel_pathlist_hook = gsi_set_rel_pathlist`, `RegisterCustomScanMethods` (in `gsi_scan_init`) | adds the `GsiLookup` plan for `col = const` |
| **Background worker** | `RegisterBackgroundWorker` in `gsi_resolver_init` (only when preloaded) | the `gsi resolver` process |
| **Transaction callbacks** | `RegisterXactCallback` / `RegisterSubXactCallback`, done on the first connection ([connection.c:185](../src/gsi_fdw/connection.c#L185)) | runs code at `PRE_COMMIT`, `COMMIT`, `ABORT`. **This is where 2PC lives** |
| **SQL-callable C functions** | `CREATE FUNCTION ... AS 'MODULE_PATHNAME'`, with `PG_FUNCTION_INFO_V1` in C | `gsi_create`, `gsi_drop`, `gsi_node_for`, `gsi_resolve` |

### The FDW callbacks that matter for us
`FdwRoutine` has about 30 callbacks. The ones on our path:

| Phase | Callback | Purpose | GSI involvement |
|---|---|---|---|
| plan (scan) | `GetForeignRelSize`, `GetForeignPaths`, `GetForeignPlan` | estimate size and cost, build a `ForeignScan` | none; our **`set_rel_pathlist_hook`** then adds the cheaper GsiLookup path on the parent |
| plan (write) | `AddForeignUpdateTargets` | ask for extra "junk" columns from the scan | **adds the old key of every GSI, and the old id** |
| plan (write) | `PlanDirectModify` | try to push the whole `UPDATE/DELETE` to the remote | **disabled when the write can change an index entry** (DELETE, or UPDATE of an indexed column) |
| plan (write) | `PlanForeignModify` | build the per-row remote `UPDATE ... WHERE ctid=$1` | none |
| exec start | `BeginForeignScan` / `BeginForeignModify` / `BeginForeignInsert` | open connections, prepare statements | **builds a `GsiModifyState` per GSI** |
| exec | `IterateForeignScan` | return the next remote row | none |
| exec | `ExecForeignInsert/Update/Delete` | write one row remotely | **index entries written here (one per GSI)** |
| exec | `GetForeignModifyBatchSize` | allow batched INSERT | **forced to 1 for GSI tables** |
| exec end | `EndForeignScan` / `EndForeignModify` | clean up | none |

The PostgreSQL executor node that calls the write callbacks is **`ModifyTable`**. It sits at the top of every INSERT/UPDATE/DELETE plan and calls `ExecForeign*` once per row.

---

## 3. Files

```
src/gsi_fdw/
├── postgres_fdw.c     FDW callbacks: planning, scanning, writing      (modified)
├── connection.c       connection cache, remote transactions, 2PC      (modified)
├── deparse.c          builds remote SQL text from plan trees           (unchanged)
├── option.c           option validation, _PG_init                      (modified, tiny)
├── shippable.c        which functions/operators may be sent remotely   (unchanged)
├── postgres_fdw.h     shared upstream declarations                     (unchanged)
├── gsi.h              GSI structs and prototypes                       (new)
├── gsi.c              GSI catalog, placement, index maintenance, admin (new)
├── gsi_resolver.c     crash recovery: gsi_resolve() + background worker (new)
├── gsi_scan.c         read path: planner hook + GsiLookup CustomScan   (new)
├── gsi_fdw--1.0.sql   SQL objects created by CREATE EXTENSION          (new)
├── gsi_fdw.control    extension metadata                               (new)
├── Makefile           PGXS build                                       (new)
└── UPSTREAM           upstream commit this was copied from
```

### 3.1 [postgres_fdw.c](../src/gsi_fdw/postgres_fdw.c): the FDW callbacks (~7.9k lines)
The core of postgres_fdw: every `FdwRoutine` callback.

| Part | Where | What it does | Relevance |
|---|---|---|---|
| `PgFdwScanState` | [L139-177](../src/gsi_fdw/postgres_fdw.c#L139-L177) | runtime state of one foreign scan (connection, cursor, fetched rows) | used by reads |
| `PgFdwModifyState` (`fmstate`) | [L182-217](../src/gsi_fdw/postgres_fdw.c#L182-L217) | runtime state of writes to one foreign partition: connection, prepared remote statement, parameter conversion, **our `gsi` field** | central for writes |
| `postgres_fdw_handler` | [L556](../src/gsi_fdw/postgres_fdw.c#L556) | fills `FdwRoutine` with all callbacks | entry point |
| scan planning | [`GetForeignRelSize` L626](../src/gsi_fdw/postgres_fdw.c#L626), [`GetForeignPaths` L1014](../src/gsi_fdw/postgres_fdw.c#L1014), [`GetForeignPlan` L1223](../src/gsi_fdw/postgres_fdw.c#L1223), [`estimate_path_cost_size` L3113](../src/gsi_fdw/postgres_fdw.c#L3113) | costs, and which `WHERE` clauses can be pushed to the remote | broadcast reads today |
| scan execution | [`BeginForeignScan` L1492](../src/gsi_fdw/postgres_fdw.c#L1492), [`IterateForeignScan` L1597](../src/gsi_fdw/postgres_fdw.c#L1597), [`make_tuple_from_result_row` L7477](../src/gsi_fdw/postgres_fdw.c#L7477) | remote `DECLARE CURSOR` + `FETCH`, text → tuples | also fetches the rows UPDATE/DELETE will change |
| **`AddForeignUpdateTargets`** | [L1740](../src/gsi_fdw/postgres_fdw.c#L1740) | asks for `ctid` (and our `gsi_old_key_<index>` per GSI, `gsi_old_id`) | **GSI** |
| `PlanForeignModify` | [L1774](../src/gsi_fdw/postgres_fdw.c#L1774) | deparses the per-row INSERT/UPDATE/DELETE | |
| `BeginForeignModify` / `BeginForeignInsert` | [L1913](../src/gsi_fdw/postgres_fdw.c#L1913) / [L2160](../src/gsi_fdw/postgres_fdw.c#L2160) | executor start for writes (insert via tuple routing uses the second) | lead to `create_foreign_modify` |
| `ExecForeignInsert/Update/Delete` | [L1970](../src/gsi_fdw/postgres_fdw.c#L1970) / [L2104](../src/gsi_fdw/postgres_fdw.c#L2104) / [L2123](../src/gsi_fdw/postgres_fdw.c#L2123) | thin wrappers around `execute_foreign_modify` | |
| **`GetForeignModifyBatchSize`** | [L2032](../src/gsi_fdw/postgres_fdw.c#L2032) | batch size for INSERT | **GSI: 1** |
| **`PlanDirectModify`** | [L2451](../src/gsi_fdw/postgres_fdw.c#L2451) | pushdown of whole UPDATE/DELETE | **GSI: disabled unless `gsi_direct_modify_ok`** (no indexed column or `id` assigned) |
| `ExecForeignTruncate` | [L3007](../src/gsi_fdw/postgres_fdw.c#L3007) | remote TRUNCATE | known gap: bypasses the GSI |
| **`create_foreign_modify`** | [L3994](../src/gsi_fdw/postgres_fdw.c#L3994) | builds `fmstate`: `GetConnection` for the partition's server, parameter setup | **GSI: `gsi_begin_modify`** |
| **`execute_foreign_modify`** | [L4116](../src/gsi_fdw/postgres_fdw.c#L4116) | per row: prepare once, convert params, `PQsendQueryPrepared`, check the result | **GSI: capture before, apply after** |
| `prepare_foreign_modify`, `convert_prep_stmt_params` | [L4246](../src/gsi_fdw/postgres_fdw.c#L4246), [L4301](../src/gsi_fdw/postgres_fdw.c#L4301) | libpq prepared statement; slot values → text parameters | |
| join and aggregate pushdown | e.g. [`GetForeignJoinPaths` L6272](../src/gsi_fdw/postgres_fdw.c#L6272) | push joins and aggregates to the remote | not used by us |

### 3.2 [connection.c](../src/gsi_fdw/connection.c): connections and remote transactions (~2.4k lines)

| Part | Where | What it does | Relevance |
|---|---|---|---|
| `ConnCacheEntry` | [L56-79](../src/gsi_fdw/connection.c#L56-L79) | one cached libpq connection per user mapping (≈ per server): `xact_depth`, error flags, **our `gsi_index`, `gsi_prepared`, `gsi_gid`** | the unit of 2PC participation |
| `GetConnection` | [L185](../src/gsi_fdw/connection.c#L185) | find or create the connection; registers the xact callbacks the first time; opens the remote transaction | used by both base and index writes |
| `make_new_connection`, `connect_pg_server` | [L344](../src/gsi_fdw/connection.c#L344), [L450](../src/gsi_fdw/connection.c#L450) | build the conninfo from server and user-mapping options, connect | `gsi_connect_raw` reuses `connect_pg_server` |
| `configure_remote_session` | [L664](../src/gsi_fdw/connection.c#L664) | `SET search_path = pg_catalog`, timezone, datestyle… **+ `lock_timeout`** | why our SQL schema-qualifies `public.` |
| `begin_remote_xact` | [L754](../src/gsi_fdw/connection.c#L754) | `START TRANSACTION ISOLATION LEVEL REPEATABLE READ`, plus savepoints | remote snapshot rules (the 40001 fix) |
| `do_sql_command`, `pgfdw_exec_query`, `pgfdw_get_result` | [L711](../src/gsi_fdw/connection.c#L711), [L845](../src/gsi_fdw/connection.c#L845), [L873](../src/gsi_fdw/connection.c#L873) | send SQL and wait for the result, staying responsive to interrupts | used by our code |
| `pgfdw_report_error` | [L938](../src/gsi_fdw/connection.c#L938) | turns a remote error into a local `ereport` with the same SQLSTATE | 23505 / 40001 reach the client |
| **`pgfdw_xact_callback`** | [L994](../src/gsi_fdw/connection.c#L994) | end of the local transaction: commit or abort every remote transaction | **2PC lives here** |
| `pgfdw_subxact_callback` | [L1164](../src/gsi_fdw/connection.c#L1164) | `RELEASE` / `ROLLBACK TO SAVEPOINT` remotely | covers index writes too |
| `pgfdw_reset_xact_state` | [L1353](../src/gsi_fdw/connection.c#L1353) | marks the connection out of transaction; drops broken connections | |
| `pgfdw_exec_cleanup_query`, `pgfdw_abort_cleanup` | [L1494](../src/gsi_fdw/connection.c#L1494), [L1669](../src/gsi_fdw/connection.c#L1669) | non-throwing, time-limited commands for abort paths | we use the first for `COMMIT PREPARED` |
| **GSI 2PC section** | [L2276-2408](../src/gsi_fdw/connection.c#L2276-L2408) | `gsi_mark_conn`, `gsi_log_xid`, `gsi_prepare_remote_xact`, `gsi_finish_prepared`, raw connections | ours |

### 3.3 [deparse.c](../src/gsi_fdw/deparse.c): SQL generation (unchanged, ~4k lines)
Turns PostgreSQL plan expressions back into SQL text for the remote server.

| Function | Where | Use |
|---|---|---|
| `classifyConditions`, `is_foreign_expr`, `foreign_expr_walker` | [L214](../src/gsi_fdw/deparse.c#L214), [L240](../src/gsi_fdw/deparse.c#L240), [L308](../src/gsi_fdw/deparse.c#L308) | decide which `WHERE` clauses are safe to send to the remote (builtin, immutable, ...) |
| `deparseSelectStmtForRel` | [L1229](../src/gsi_fdw/deparse.c#L1229) | the remote `SELECT` of a foreign scan (including `FOR UPDATE` and our junk columns) |
| `deparseInsertSql` / `deparseUpdateSql` / `deparseDeleteSql` | [L1942](../src/gsi_fdw/deparse.c#L1942) / [L2075](../src/gsi_fdw/deparse.c#L2075) / [L2220](../src/gsi_fdw/deparse.c#L2220) | per-row remote writes (`... WHERE ctid = $1`) |
| `deparseDirectUpdateSql` / `deparseDirectDeleteSql` | [L2135](../src/gsi_fdw/deparse.c#L2135) / [L2249](../src/gsi_fdw/deparse.c#L2249) | pushed-down whole statement; on GSI tables only for UPDATEs that assign no indexed column |
| `deparseRelation`, `deparseExpr`, `deparseStringLiteral` | [L2667](../src/gsi_fdw/deparse.c#L2667), [L2742](../src/gsi_fdw/deparse.c#L2742), [L2707](../src/gsi_fdw/deparse.c#L2707) | helpers: remote table name, expressions, quoting |

Our index statements don't use `deparse.c`. They're simple fixed SQL built with `psprintf` and sent with parameters in [`send_index_op`](../src/gsi_fdw/gsi.c#L327-L368).

### 3.4 [option.c](../src/gsi_fdw/option.c): options and module init

| Function | Where | Use |
|---|---|---|
| `postgres_fdw_validator` | [L73](../src/gsi_fdw/option.c#L73) | validates `OPTIONS (...)` on the server, user mapping and foreign table |
| `InitPgFdwOptions`, `is_valid_option` | [L241](../src/gsi_fdw/option.c#L241), [L375](../src/gsi_fdw/option.c#L375) | the list of allowed options (libpq ones plus `table_name`, `batch_size`, `parallel_commit`, …) |
| `ExtractConnectionOptions` | [L415](../src/gsi_fdw/option.c#L415) | options → libpq keyword and value arrays |
| `_PG_init` | [L569](../src/gsi_fdw/option.c#L569) | module load: `gsi_fdw.application_name`, **`gsi_init()`** |

### 3.5 [shippable.c](../src/gsi_fdw/shippable.c): pushdown safety (unchanged)
[`is_shippable`](../src/gsi_fdw/shippable.c#L162) / [`is_builtin`](../src/gsi_fdw/shippable.c#L152) decide whether a function or operator may be sent to the remote server (builtins, or listed `extensions`). It only matters for pushing `WHERE` clauses down.

### 3.6 [postgres_fdw.h](../src/gsi_fdw/postgres_fdw.h) (unchanged)
Upstream shared declarations: `PgFdwRelationInfo` ([L30-127](../src/gsi_fdw/postgres_fdw.h#L30-L127), planner info per foreign relation), `PgFdwConnState`, and prototypes of `connection.c` ([L154](../src/gsi_fdw/postgres_fdw.h#L154)), `option.c` and `deparse.c` functions that our code calls (`GetConnection`, `do_sql_command`, `pgfdw_get_result`, `pgfdw_report_error`).

### 3.7 [gsi.h](../src/gsi_fdw/gsi.h), [gsi.c](../src/gsi_fdw/gsi.c), [gsi_resolver.c](../src/gsi_fdw/gsi_resolver.c) (ours)

| File | Main contents |
|---|---|
| `gsi.h` | `GsiIndex` (one global index; a table can have one per column), `GsiModifyState` (per GSI, per partition being written), prototypes |
| `gsi.c` | **catalog** (`gsi_indexes_for_rel`, `gsi_load_nodes`) · **placement** (`gsi_node_for_datum`) · **planner hooks** (`gsi_add_update_targets`, `gsi_direct_modify_ok`) · **write path** (`gsi_begin_modify`, `gsi_capture_row`, `gsi_apply_row`, `send_index_op`) · **admin** (`gsi_create`, `gsi_drop`, `gsi_node_for`) · **GUCs and fail points** |
| `gsi_resolver.c` | `gsi_resolve()`: scans `pg_prepared_xacts` on every node and commits or rolls back by the coordinator's `pg_xact_status`, index participants first. Also the **`gsi resolver` background worker**, which does this every 5 s for transactions older than 10 s |
| `gsi_scan.c` | **read path**: `set_rel_pathlist_hook` adds a `GsiLookup` CustomPath for `col = const` on any ready GSI (a unique one if several match); the CustomScan does hop 1 (index node) and hop 2 (only the owning shards), and the plan filter rechecks every row |

Function-by-function details are in [code_changes.md §2](code_changes.md#2-new-files).

### 3.8 [gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql): what `CREATE EXTENSION gsi_fdw` runs
An **extension script**. PostgreSQL executes it once, inside `CREATE EXTENSION`, and records every object it creates as owned by the extension (so `DROP EXTENSION` removes them).

| Section | Creates |
|---|---|
| upstream objects (renamed) | `gsi_fdw_handler()` and `gsi_fdw_validator()` (C functions; `AS 'MODULE_PATHNAME', 'postgres_fdw_handler'` maps the SQL name to the unchanged C symbol), `FOREIGN DATA WRAPPER gsi_fdw`, connection helpers `gsi_fdw_get_connections()` etc. |
| GSI catalog | schema `gsi`; `gsi.gsi_node(node_id, server_name)`; `gsi.gsi_index(index_name, base_table, column_name, is_unique, mode, status)`, one row per GSI, unique on `(base_table, column_name)` |
| GSI functions | `gsi_create(base, col, is_unique)`, `gsi_drop(base, col DEFAULT NULL)` (NULL = all GSIs of the table), `gsi_node_for(anyelement)`, `gsi_resolve(grace_secs)` |

`MODULE_PATHNAME` is replaced at install time by the `module_pathname` from the control file (`$libdir/gsi_fdw`), so each `CREATE FUNCTION` points at the C function in `gsi_fdw.so`.

### 3.9 [gsi_fdw.control](../src/gsi_fdw/gsi_fdw.control): extension metadata
Read by `CREATE EXTENSION` *before* running the script:
```
comment = '...'                          shown in \dx
default_version = '1.0'                  → runs gsi_fdw--1.0.sql
module_pathname = '$libdir/gsi_fdw'      substituted for MODULE_PATHNAME
relocatable = false                      objects stay in their schema (we hard-code schema gsi)
```
Upstream had version `1.1` (a `1.0` script plus a `1.0--1.1` upgrade). We merged both into a single `1.0` script.

### 3.10 [Makefile](../src/gsi_fdw/Makefile): PGXS build
Uses PostgreSQL's **PGXS** build system (`pg_config --pgxs`) to build outside the PostgreSQL source tree:
- `MODULE_big = gsi_fdw` with `OBJS = ... gsi.o gsi_resolver.o ...` produces `gsi_fdw.so`;
- `SHLIB_LINK_INTERNAL = $(libpq)` links libpq, which the coordinator uses to talk to the shards;
- `EXTENSION` and `DATA` install `gsi_fdw.control` and `gsi_fdw--1.0.sql` into `share/extension/`;
- `make install` copies `gsi_fdw.so` into `$libdir`.

---

## 4. Running it

### 4.1 Prerequisites
Docker Desktop running, plus Python 3 with `psycopg2` and `pytest`.

### 4.2 Commands
```bash
cd docker
docker compose up -d --build          # build image (compiles gsi_fdw), start 6 containers (coordinator + 5 data nodes)
docker compose ps                     # all four "healthy"

cd ../tests
python -m pytest -v .                 # all milestone tests (about 1 minute)
python checker.py                     # consistency: missing/orphaned/misplaced/prepared

# play by hand
docker compose -f ../docker/docker-compose.yml exec coordinator psql -U postgres
docker compose -f ../docker/docker-compose.yml exec shard1 psql -U postgres

docker compose -f ../docker/docker-compose.yml down -v   # stop and wipe all data
```

Ports on the host: coordinator 5432, shard1 5433, shard2 5434, shard3 5435.

### 4.3 Dry run: what happens, in order

```
docker compose up -d --build
├── docker build (docker/Dockerfile)
│   ├── FROM postgres:16-bookworm
│   ├── apt install build-essential postgresql-server-dev-16 libkrb5-dev
│   ├── COPY src/gsi_fdw → /build/gsi_fdw
│   └── make && make install           (PGXS)
│        ├── gcc *.c → *.o → gsi_fdw.so        → $libdir
│        └── gsi_fdw.control, gsi_fdw--1.0.sql → share/extension/
│
├── start shard1 ... shard5             (postgres -c max_prepared_transactions=100)
│   └── first start: initdb, then docker/init/datanode.sql
│        └── CREATE TABLE users_shard (id bigint PRIMARY KEY, email text, name text)
│
└── start coordinator (after the shards are healthy)
    ├── postgres -c shared_preload_libraries=gsi_fdw ...
    │    └── load gsi_fdw.so → _PG_init (option.c) → gsi_init (gsi.c): GUCs,
    │         register the "gsi resolver" worker, install set_rel_pathlist_hook
    ├── postmaster starts "gsi resolver" (every 5 s: gsi_resolve_all(grace 10 s))
    └── first start: docker/init/coordinator.sql
         ├── CREATE EXTENSION gsi_fdw
         │    ├── read gsi_fdw.control
         │    └── run gsi_fdw--1.0.sql: FDW, handler, validator, schema gsi, catalog, functions
         ├── CREATE SERVER shard1..3 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host ...)
         │    └── gsi_fdw_validator checks the options
         ├── CREATE USER MAPPING FOR PUBLIC SERVER shard1..3
         ├── INSERT INTO gsi.gsi_node VALUES (0,'shard1'),(1,'shard2'),(2,'shard3')
         └── CREATE TABLE users PARTITION BY HASH(id)
              + CREATE FOREIGN TABLE users_p0..p2 PARTITION OF users SERVER shardN

psql (coordinator)> SELECT gsi_create('users', 'email', true);
└── gsi_create (gsi.c)
    ├── validate users; SELECT 1 FROM users LIMIT 1   (fdw scans every shard: must be empty)
    ├── INSERT INTO gsi.gsi_index ...
    ├── on shard1..3: CREATE TABLE public.gsi_users_email (key, id, shard, PK(key,id))
    │                 CREATE UNIQUE INDEX ON public.gsi_users_email (key)
    └── COMMIT → 2PC: PREPARE on shard1..3 → local commit → COMMIT PREPARED

psql (coordinator)> INSERT INTO users VALUES (101, 'bob@x', 'Bob');
├── PG routes the row: hash(101) → users_p1 (shard2)
├── postgresBeginForeignInsert → create_foreign_modify
│    ├── GetConnection(shard2) → START TRANSACTION ISOLATION LEVEL REPEATABLE READ
│    └── gsi_begin_modify → GsiModifyState{base_node = 1}; force xid
├── postgresExecForeignInsert → execute_foreign_modify
│    ├── gsi_mark_conn(shard2, false)                    2PC on
│    ├── gsi_capture_row: new_key='bob@x', new_node = hash('bob@x') mod 3 = 2
│    ├── shard2: INSERT INTO users_shard VALUES (101, 'bob@x', 'Bob')
│    └── gsi_apply_row → send_index_op
│         ├── GetConnection(shard3) → START TRANSACTION ...
│         ├── gsi_mark_conn(shard3, true)                role i
│         └── shard3: INSERT INTO public.gsi_users_email VALUES ('bob@x', 101, 1)
└── (autocommit) COMMIT
     ├── PRE_COMMIT:  gsi_log_xid (WAL flush)
     │                shard2: PREPARE TRANSACTION 'gsi_5001_shard2_b'
     │                shard3: PREPARE TRANSACTION 'gsi_5001_shard3_i'
     ├── local commit record flushed            ← DECISION
     └── COMMIT:      shard3: COMMIT PREPARED 'gsi_5001_shard3_i'   (index first)
                      shard2: COMMIT PREPARED 'gsi_5001_shard2_b'

psql (coordinator)> SELECT * FROM users WHERE email = 'bob@x';
├── planner: Append over 3 foreign scans, then set_rel_pathlist_hook adds GsiLookup (cheaper)
└── Custom Scan (GsiLookup) on users   Filter: (email = 'bob@x')
     ├── hop 1, shard3: SELECT id, shard FROM public.gsi_users_email WHERE key = 'bob@x'  → (101, 1)
     ├── hop 2, shard2 only: SELECT id, email, name FROM public.users_shard WHERE id = ANY('{101}')
     └── Filter (recheck) keeps the row → (101, bob@x, Bob)

psql> SET gsi.fail_point = 'after_local_commit'; INSERT ...;   -- coordinator crashes
├── backend abort() → postmaster crash recovery → coordinator back up
├── shard2, shard3: gsi_<xid>_* still listed in pg_prepared_xacts, rows locked
├── within ~15 s the gsi resolver worker (or psql> SELECT gsi_resolve(0) right away)
│    └── per node: list gsi_% → pg_xact_status(xid) = committed
│         → COMMIT PREPARED _i, then _b
└── consistent again (checker.py: all zeros)
```

### 4.4 Where to look when something goes wrong

| Symptom | Look at |
|---|---|
| build fails | `docker compose build` output (gcc errors point to `src/gsi_fdw/*.c`) |
| an error from a statement | `docker compose logs coordinator`; remote errors show `remote SQL command: ...` |
| what ran on a shard | `docker compose logs shard2` |
| stuck locks after a crash | `SELECT * FROM pg_prepared_xacts` on each shard, then `SELECT gsi_resolve(0)` |
| index and base disagree | `python tests/checker.py` |
