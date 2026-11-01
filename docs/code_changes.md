# Our Code Changes: Reference

Every line we added or changed in `src/gsi_fdw/`, compared with upstream postgres_fdw (`REL_16_STABLE` commit `17c5a06`, see [UPSTREAM](../src/gsi_fdw/UPSTREAM)). All links are clickable and include line ranges.

**Summary**

| File | Status | Size of our change |
|---|---|---|
| [gsi.h](../src/gsi_fdw/gsi.h) | **new** | 90 lines |
| [gsi.c](../src/gsi_fdw/gsi.c) | **new** | 654 lines |
| [gsi_resolver.c](../src/gsi_fdw/gsi_resolver.c) | **new** | 308 lines |
| [gsi_scan.c](../src/gsi_fdw/gsi_scan.c) | **new** (M8 read path) | 481 lines |
| [gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql), [gsi_fdw.control](../src/gsi_fdw/gsi_fdw.control), [Makefile](../src/gsi_fdw/Makefile) | **new** (replace upstream equivalents) | 104 lines |
| [connection.c](../src/gsi_fdw/connection.c) | modified | +181 / −4 |
| [postgres_fdw.c](../src/gsi_fdw/postgres_fdw.c) | modified | +30 |
| [option.c](../src/gsi_fdw/option.c) | modified | +5 / −2 |
| `deparse.c`, `shippable.c`, `postgres_fdw.h` | **unchanged** | 0 |

Everything else in the modified files is upstream code we didn't touch.

---

## 1. Changes inside upstream files

### 1.1 [connection.c](../src/gsi_fdw/connection.c): two-phase commit

| Lines | What | Note |
|---|---|---|
| [L17](../src/gsi_fdw/connection.c#L17), [L21](../src/gsi_fdw/connection.c#L21), [L27](../src/gsi_fdw/connection.c#L27) | `#include` `access/xlog.h`, `gsi.h`, `replication/message.h` | needed for `XLogFlush`, the GSI API and `LogLogicalMessage` |
| [L75-78](../src/gsi_fdw/connection.c#L75-L78) | new fields in `ConnCacheEntry`: `gsi_index`, `gsi_prepared`, `gsi_gid` | per-connection 2PC state for the current transaction: role (`i`/`b`), whether it's prepared, and its GID |
| [L93-94](../src/gsi_fdw/connection.c#L93-L94) | `static bool gsi_xact_2pc` | per-transaction flag: "this transaction wrote a GSI table, so commit with 2PC" |
| [L167-169](../src/gsi_fdw/connection.c#L167-L169) | prototypes | `gsi_log_xid`, `gsi_prepare_remote_xact`, `gsi_finish_prepared` |
| [L365-366](../src/gsi_fdw/connection.c#L365-L366) | in `make_new_connection` | a new connection starts with no GSI state |
| [L532](../src/gsi_fdw/connection.c#L532) | `fallback_application_name = "gsi_fdw"` | rename only |
| [L696-704](../src/gsi_fdw/connection.c#L696-L704) | in `configure_remote_session` | `SET lock_timeout` on every remote session, so cross-node deadlocks end in an error instead of hanging |
| [L1001-1009](../src/gsi_fdw/connection.c#L1001-L1009) | top of `pgfdw_xact_callback` | **phase 2**: on `COMMIT` or `ABORT`, call `gsi_finish_prepared(true/false)` |
| [L1014-1016](../src/gsi_fdw/connection.c#L1014-L1016) | in `pgfdw_xact_callback` | on `PRE_COMMIT`, `gsi_log_xid()` before any PREPARE |
| [L1047-1061](../src/gsi_fdw/connection.c#L1047-L1061) | `case XACT_EVENT_PRE_COMMIT` | **phase 1**: GSI transaction → `gsi_prepare_remote_xact`; otherwise upstream `COMMIT TRANSACTION` |
| [L1146-1148](../src/gsi_fdw/connection.c#L1146-L1148) | end of the callback loop | fail point `after_prepare` |
| [L2276-2407](../src/gsi_fdw/connection.c#L2276-L2407) | **new GSI section** at the end of the file | see the table below |

New functions in that section:

| Function | Lines | Does |
|---|---|---|
| block comment | [L2278-2289](../src/gsi_fdw/connection.c#L2278-L2289) | the 2PC protocol in one paragraph |
| `gsi_mark_conn` | [L2292-2311](../src/gsi_fdw/connection.c#L2292-L2311) | called by the write path; sets `gsi_xact_2pc`, and marks the connection role `i` when `is_index` |
| `gsi_log_xid` | [L2313-2324](../src/gsi_fdw/connection.c#L2313-L2324) | forces the xid, writes and flushes a transactional WAL message so the xid can't be reused after a crash |
| `gsi_prepare_remote_xact` | [L2327-2352](../src/gsi_fdw/connection.c#L2327-L2352) | builds `gsi_<fxid>_<server>_<i/b>`, sends `PREPARE TRANSACTION`, rejects a `ROLLBACK` reply |
| `gsi_finish_prepared` | [L2355-2395](../src/gsi_fdw/connection.c#L2355-L2395) | `COMMIT/ROLLBACK PREPARED`: pass 0 = role `i`, pass 1 = role `b`; fail points `after_local_commit` and `mid_commit_prepared`; failures → WARNING; resets state |
| `gsi_connect_raw` / `gsi_disconnect_raw` | [L2398-2408](../src/gsi_fdw/connection.c#L2398-L2408) | a connection outside the cache and outside any transaction, for the resolver |

### 1.2 [postgres_fdw.c](../src/gsi_fdw/postgres_fdw.c): write interception

| Lines | Upstream function | What | Note |
|---|---|---|---|
| [L44](../src/gsi_fdw/postgres_fdw.c#L44) | n/a | `#include "gsi.h"` | |
| [L215-216](../src/gsi_fdw/postgres_fdw.c#L215-L216) | `PgFdwModifyState` struct | field `List *gsi` | one `GsiModifyState` per GSI on the table; NIL when it has none |
| [L1746](../src/gsi_fdw/postgres_fdw.c#L1746), [L1763-1766](../src/gsi_fdw/postgres_fdw.c#L1763-L1766) | `postgresAddForeignUpdateTargets` (planner) | add junk columns: the old key of every GSI, and the old id | so UPDATE and DELETE know which entries to remove |
| [L2047-2050](../src/gsi_fdw/postgres_fdw.c#L2047-L2050) | `postgresGetForeignModifyBatchSize` | return 1 for a table with a GSI | no batched INSERT; index maintenance is per row |
| [L2480-2482](../src/gsi_fdw/postgres_fdw.c#L2480-L2482) | `postgresPlanDirectModify` (planner) | return false unless `gsi_direct_modify_ok` | pushdown only for writes that can't change an index entry: tables without a GSI, and UPDATEs that set no indexed column and not `id`. DELETE and key UPDATEs run per row |
| [L4103-4104](../src/gsi_fdw/postgres_fdw.c#L4103-L4104) | `create_foreign_modify` (executor start) | `fmstate->gsi = gsi_begin_modify(...)` | builds per-partition state for every GSI |
| [L4184-4190](../src/gsi_fdw/postgres_fdw.c#L4184-L4190) | `execute_foreign_modify` (per row, before the base write) | `gsi_mark_conn(base, false)` + `gsi_capture_row` | records old and new entries before `RETURNING` can overwrite the slot |
| [L4229-4231](../src/gsi_fdw/postgres_fdw.c#L4229-L4231) | `execute_foreign_modify` (after the base write) | `gsi_apply_row` | sends the index DELETE/INSERT, only if the base row was actually changed |

### 1.3 [option.c](../src/gsi_fdw/option.c): module init

| Lines | What |
|---|---|
| [L23](../src/gsi_fdw/option.c#L23) | `#include "gsi.h"` |
| [L579](../src/gsi_fdw/option.c#L579) | GUC renamed `postgres_fdw.application_name` → `gsi_fdw.application_name` |
| [L590-592](../src/gsi_fdw/option.c#L590-L592) | reserve the `gsi_fdw` prefix; call `gsi_init()` |

---

## 2. New files

### 2.1 [gsi.h](../src/gsi_fdw/gsi.h)

| Lines | What |
|---|---|
| [L1-13](../src/gsi_fdw/gsi.h#L1-L13) | header comment: what a GSI is and how it's maintained |
| [L23-34](../src/gsi_fdw/gsi.h#L23-L34) | `GsiIndex`: one global index (name, remote table, column type and collation, unique, `servers[node_id]`). A table can have several, one per column |
| [L36-58](../src/gsi_fdw/gsi.h#L36-L58) | `GsiModifyState`: per-partition write state of one GSI plus the current row's old and new entry; a table with several GSIs gets a List |
| [L60-90](../src/gsi_fdw/gsi.h#L60-L90) | prototypes, grouped by file (incl. `gsi_server_node`, `gsi_resolver_init`, `gsi_scan_init`) |

### 2.2 [gsi.c](../src/gsi_fdw/gsi.c)

| Function | Lines | Called from | Does |
|---|---|---|---|
| `gsi_init` | [L44-68](../src/gsi_fdw/gsi.c#L44-L68) | `_PG_init` | defines GUCs `gsi.remote_lock_timeout` and `gsi.fail_point`; calls `gsi_resolver_init()` and `gsi_scan_init()` |
| `gsi_fail_point` | [L74-83](../src/gsi_fdw/gsi.c#L74-L83) | commit callback | `abort()` if `gsi.fail_point` equals the given name (tests only) |
| `gsi_load_nodes` | [L86-111](../src/gsi_fdw/gsi.c#L86-L111) | many | SPI: `gsi.gsi_node` → array of server OIDs ordered by node id |
| `gsi_indexes_for_rel` | [L113-175](../src/gsi_fdw/gsi.c#L113-L175) | all hooks | SPI: catalog → List of `GsiIndex` (every sync GSI of the table or its parent, by name), or NIL |
| `gsi_node_for_datum` | [L177-195](../src/gsi_fdw/gsi.c#L177-L195) | write path, `gsi_node_for` | **placement**: the type's extended hash with the partition seed, `mod N` |
| `gsi_server_node` | [L197-205](../src/gsi_fdw/gsi.c#L197-L205) | `gsi_begin_modify` | server OID → node id |
| `add_junk_column`, `old_key_junk_name` | [L207-229](../src/gsi_fdw/gsi.c#L207-L229) | `gsi_add_update_targets` | `makeVar` + `add_row_identity_var`; junk name `gsi_old_key_<index>` |
| `gsi_add_update_targets` | [L231-248](../src/gsi_fdw/gsi.c#L231-L248) | `postgresAddForeignUpdateTargets` | junk `gsi_old_key_<index>` per GSI and one `gsi_old_id` |
| `gsi_direct_modify_ok` | [L250-280](../src/gsi_fdw/gsi.c#L250-L280) | `postgresPlanDirectModify` | pushdown allowed only if no GSI, or an UPDATE that sets no indexed column and not `id` |
| `gsi_begin_modify` | [L282-342](../src/gsi_fdw/gsi.c#L282-L342) | `create_foreign_modify` | one `GsiModifyState` per GSI; finds the junk positions; rejects `UPDATE id`; forces the xid |
| `key_text` | [L344-350](../src/gsi_fdw/gsi.c#L344-L350) | capture | key Datum → node id + text form |
| `capture_one`, `gsi_capture_row` | [L352-410](../src/gsi_fdw/gsi.c#L352-L410) | `execute_foreign_modify` | for every GSI, works out which entry to delete and which to insert for this row; skips an unchanged or NULL key |
| `send_index_op` | [L412-455](../src/gsi_fdw/gsi.c#L412-L455) | `gsi_apply_row` | runs the index INSERT/DELETE on the node's connection; a DELETE that misses raises **40001** |
| `gsi_apply_row` | [L457-472](../src/gsi_fdw/gsi.c#L457-L472) | `execute_foreign_modify` | for every GSI: deletes the old entry, then inserts the new one |
| `exec_on_all_nodes` | [L474-487](../src/gsi_fdw/gsi.c#L474-L487) | create/drop | the same DDL on every node, inside the 2PC transaction |
| `index_on_column` | [L489-503](../src/gsi_fdw/gsi.c#L489-L503) | create/drop | the table's GSI on a given column |
| `gsi_create` | [L505-581](../src/gsi_fdw/gsi.c#L505-L581) | SQL | validates the table, refuses a second GSI on the same column, checks the table is empty, inserts the catalog row, creates the index tables |
| `gsi_drop` | [L583-639](../src/gsi_fdw/gsi.c#L583-L639) | SQL | `gsi_drop(table, col)`: drops that GSI; `gsi_drop(table)`: drops all of the table's GSIs |
| `gsi_node_for` | [L641-654](../src/gsi_fdw/gsi.c#L641-L654) | SQL (tests, checker) | exposes the placement function to SQL |

### 2.3 [gsi_resolver.c](../src/gsi_fdw/gsi_resolver.c)

| Function | Lines | Does |
|---|---|---|
| file comment | [L1-16](../src/gsi_fdw/gsi_resolver.c#L1-L16) | resolution rule |
| `GsiPending` struct | [L45-51](../src/gsi_fdw/gsi_resolver.c#L45-L51) | one leftover prepared transaction: connection, GID, fxid, role |
| `xact_status` | [L55-71](../src/gsi_fdw/gsi_resolver.c#L55-L71) | coordinator `pg_xact_status(fxid)`; NULL for a future xid |
| `parse_gid` | [L75-85](../src/gsi_fdw/gsi_resolver.c#L75-L85) | `gsi_<fxid>_<server>_<i/b>` → fxid and role |
| `collect_node` | [L88-135](../src/gsi_fdw/gsi_resolver.c#L88-L135) | raw connection to one node (skipped if the node is down); lists `gsi_%` prepared transactions older than the grace period |
| `finish` | [L138-152](../src/gsi_fdw/gsi_resolver.c#L138-L152) | `COMMIT/ROLLBACK PREPARED`; 42704 ("already finished") is logged as success |
| `gsi_resolve_all` | [L156-205](../src/gsi_fdw/gsi_resolver.c#L156-L205) | collects from all nodes, then two passes (`i`, then `b`), and closes connections in `PG_FINALLY` |
| `gsi_resolve` | [L209-212](../src/gsi_fdw/gsi_resolver.c#L209-L212) | SQL wrapper `gsi_resolve(grace_secs)` |
| `resolver_pass` (M7) | [L215-240](../src/gsi_fdw/gsi_resolver.c#L215-L240) | one worker pass: own transaction and snapshot; skips if `CREATE EXTENSION` hasn't run; errors are logged and the transaction aborted, so the worker keeps going |
| `gsi_resolver_main` (M7) | [L242-264](../src/gsi_fdw/gsi_resolver.c#L242-L264) | background-worker entry point: signals, connect to `gsi.resolver_database`, loop every `gsi.resolver_interval`, reload config on SIGHUP |
| `gsi_resolver_init` (M7) | [L266-308](../src/gsi_fdw/gsi_resolver.c#L266-L308) | GUCs `gsi.resolver_database`, `gsi.resolver_interval` (5 s), `gsi.resolver_grace` (10 s); `RegisterBackgroundWorker` when preloaded |

### 2.4 [gsi_scan.c](../src/gsi_fdw/gsi_scan.c): read path (M8)

| Part | Lines | Does |
|---|---|---|
| file comment | [L1-16](../src/gsi_fdw/gsi_scan.c#L1-L16) | the two-hop lookup, the recheck, and which GSI is used when several match |
| `GsiPart`, `GsiScanState` | [L40-57](../src/gsi_fdw/gsi_scan.c#L40-L57) | per-shard remote `SELECT`; scan state (key expression, fetched rows, EXPLAIN counters) |
| method tables | [L72-89](../src/gsi_fdw/gsi_scan.c#L72-L89) | `CustomPathMethods`, `CustomScanMethods`, `CustomExecMethods` named `GsiLookup` |
| `plan_index_name` | [L91-96](../src/gsi_fdw/gsi_scan.c#L91-L96) | the GSI a plan uses, stored in `custom_private` |
| `find_key_clause` | [L98-135](../src/gsi_fdw/gsi_scan.c#L98-L135) | finds `indexed_col = Const/Param` (the type's default `=`) in `baserestrictinfo` |
| `gsi_set_rel_pathlist` | [L137-201](../src/gsi_fdw/gsi_scan.c#L137-L201) | **planner hook**: for a plain SELECT on a partitioned table, picks a ready GSI whose column has an equality qual (a unique one if several match), adds the GsiLookup path (cost 0.1 × cheapest), then clears `part_scheme` so `apply_scanjoin_target_to_paths` doesn't throw the path away |
| `gsi_plan_path` | [L203-218](../src/gsi_fdw/gsi_scan.c#L203-L218) | path → `CustomScan`; key in `custom_exprs`, index name in `custom_private`; **all quals become the plan's filter**, which is the recheck |
| `gsi_create_scan_state` | [L220-228](../src/gsi_fdw/gsi_scan.c#L220-L228) | allocates `GsiScanState` |
| `remote_column`, `remote_table` | [L230-264](../src/gsi_fdw/gsi_scan.c#L230-L264) | honour the `column_name`, `schema_name`, `table_name` FDW options (like `deparse.c`) |
| `gsi_begin_scan` | [L266-320](../src/gsi_fdw/gsi_scan.c#L266-L320) | init the key expression; look up the plan's GSI by name; build one `SELECT <cols> FROM <remote> WHERE id = ANY($1)` per partition |
| `remote_query` | [L322-336](../src/gsi_fdw/gsi_scan.c#L322-L336) | parameterized query on a node's fdw connection, inside its remote transaction |
| `gsi_fetch` | [L338-414](../src/gsi_fdw/gsi_scan.c#L338-L414) | **hop 1**: `SELECT id, shard ... WHERE key = $1` on `hash(key)`; group the ids by shard; **hop 2**: fetch only those shards; text → tuples |
| `gsi_next`, `gsi_recheck`, `gsi_exec_scan` | [L416-446](../src/gsi_fdw/gsi_scan.c#L416-L446) | `ExecScan` loop: return buffered rows; ExecScan applies the filter |
| `gsi_rescan`, `gsi_end_scan` | [L448-460](../src/gsi_fdw/gsi_scan.c#L448-L460) | refetch on rescan (e.g. a new parameter) |
| `gsi_explain_scan` | [L462-473](../src/gsi_fdw/gsi_scan.c#L462-L473) | EXPLAIN shows `GSI: <name>`; ANALYZE also shows `Index Node` and `Shards Fetched` |
| `gsi_scan_init` | [L475-481](../src/gsi_fdw/gsi_scan.c#L475-L481) | registers the scan methods and installs `set_rel_pathlist_hook` |

### 2.5 Build and SQL files

| File | Lines | What |
|---|---|---|
| [gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql#L6-L36) | L6-36 | upstream objects, renamed (`gsi_fdw_handler`, `gsi_fdw_validator`, `FOREIGN DATA WRAPPER gsi_fdw`, ...). C symbols unchanged |
| [gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql#L38-L55) | L38-55 | **new** catalog: schema `gsi`, tables `gsi.gsi_node` and `gsi.gsi_index` (`UNIQUE (base_table, column_name)`: one GSI per column, many per table) |
| [gsi_fdw--1.0.sql](../src/gsi_fdw/gsi_fdw--1.0.sql#L57-L76) | L57-76 | **new** SQL functions `gsi_create(base, col, is_unique)`, `gsi_drop(base, col DEFAULT NULL)`, `gsi_node_for`, `gsi_resolve` |
| [gsi_fdw.control](../src/gsi_fdw/gsi_fdw.control) | all | extension metadata |
| [Makefile](../src/gsi_fdw/Makefile) | all | PGXS build; adds `gsi.o`, `gsi_resolver.o` and `gsi_scan.o` |

---

## 3. Outside `src/`
- [docker/](../docker/): image that builds the extension; 6-container compose (coordinator + 5 data nodes); init SQL.
- [tests/](../tests/): pytest per milestone plus [checker.py](../tests/checker.py).

*Line ranges are a snapshot. Regenerate them with `diff -U0 <upstream>/connection.c src/gsi_fdw/connection.c` after edits.*
