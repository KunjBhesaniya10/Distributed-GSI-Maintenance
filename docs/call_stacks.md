# Call Stacks per Statement Type

What runs, in what order, for each kind of statement, from the coordinator's point of view. Data nodes run stock PostgreSQL and only receive the SQL shown in `code`.

**Legend**
- **[GSI]**: our code (new, or a GSI change inside the copied postgres_fdw).
- **[fdw]**: unchanged postgres_fdw code (copied, in `src/gsi_fdw/`).
- **[PG]**: PostgreSQL core. Links go to `REL_16_STABLE` on GitHub.

Line numbers refer to the current tree. Nesting means "calls".

---

## 0. Module load (coordinator start)

- [PG] postmaster loads `shared_preload_libraries = 'gsi_fdw'`
  - [fdw] [`_PG_init`](../src/gsi_fdw/option.c#L569): defines `gsi_fdw.application_name`
    - [GSI] [`gsi_init`](../src/gsi_fdw/gsi.c#L44): defines `gsi.remote_lock_timeout`, `gsi.fail_point`

On the first remote connection in a backend, [fdw] [`GetConnection`](../src/gsi_fdw/connection.c#L185) registers [`pgfdw_xact_callback`](../src/gsi_fdw/connection.c#L994) and [`pgfdw_subxact_callback`](../src/gsi_fdw/connection.c#L1164) with PG's `RegisterXactCallback`. All 2PC logic hangs off these callbacks.

### Opening a remote connection (used by every flow below)
- [fdw] [`GetConnection`](../src/gsi_fdw/connection.c#L185)
  - [fdw] [`make_new_connection`](../src/gsi_fdw/connection.c#L344) (first use only)
    - [fdw] [`connect_pg_server`](../src/gsi_fdw/connection.c#L450)
      - [fdw]+[GSI] [`configure_remote_session`](../src/gsi_fdw/connection.c#L664): `SET search_path`, ... and **[GSI]** [`SET lock_timeout`](../src/gsi_fdw/connection.c#L702)
  - [fdw] [`begin_remote_xact`](../src/gsi_fdw/connection.c#L754): `START TRANSACTION ISOLATION LEVEL REPEATABLE READ` ([L769](../src/gsi_fdw/connection.c#L769)), plus `SAVEPOINT sN` ([L785](../src/gsi_fdw/connection.c#L785)) to match the local subtransaction depth

---

## 1. `SELECT gsi_create('users', 'email', true)`

- [GSI] [`gsi_create`](../src/gsi_fdw/gsi.c#L399)
  - [GSI] [`gsi_index_for_rel`](../src/gsi_fdw/gsi.c#L115): checks there's no GSI yet
  - [PG] SPI [`SELECT 1 FROM users LIMIT 1`](../src/gsi_fdw/gsi.c#L438): emptiness check
    - [fdw] foreign scan of each partition: [`postgresBeginForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1492) → [`GetConnection`](../src/gsi_fdw/postgres_fdw.c#L1535) opens a remote transaction on each shard, then [`postgresIterateForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1597) fetches rows
  - [PG] SPI [`INSERT INTO gsi.gsi_index`](../src/gsi_fdw/gsi.c#L449)
  - [GSI] [`gsi_index_for_rel`](../src/gsi_fdw/gsi.c#L456): reads the row back into a `GsiIndex`
    - [GSI] [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L84)
  - [GSI] [`exec_on_all_nodes`](../src/gsi_fdw/gsi.c#L380) (×2 for a unique GSI)
    - [fdw] `GetConnection`
    - [GSI] [`gsi_mark_conn(conn, true)`](../src/gsi_fdw/connection.c#L2292): enables 2PC, role `i`
    - [fdw] [`do_sql_command`](../src/gsi_fdw/connection.c#L711): `CREATE TABLE public.gsi_users_email (...)`, then `CREATE UNIQUE INDEX ... (key)`
- COMMIT → **§7**: tables and catalog row appear everywhere or nowhere.

`gsi_drop('users')`: [`gsi_drop`](../src/gsi_fdw/gsi.c#L472) → SPI [`DELETE FROM gsi.gsi_index`](../src/gsi_fdw/gsi.c#L489) → [`exec_on_all_nodes`](../src/gsi_fdw/gsi.c#L494) `DROP TABLE IF EXISTS ...` → COMMIT §7.

---

## 2. `INSERT INTO users VALUES (5, 'a@x', 'A')`

### Planning
No GSI code. Partitions are chosen by tuple routing at execution time, so postgres_fdw builds the remote `INSERT` in `postgresBeginForeignInsert` ([`deparseInsertSql`, L2254](../src/gsi_fdw/postgres_fdw.c#L2254)), not in the planner.

### Executor start (once per partition, when the first row is routed to it)
- [PG] [`ExecFindPartition`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/execPartition.c) → `ExecInitRoutingInfo`
  1. [fdw] [`postgresBeginForeignInsert`](../src/gsi_fdw/postgres_fdw.c#L2160)
     - [fdw] [`create_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L3994) → `GetConnection` (base shard)
       - [GSI] [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L219), called at [L4103](../src/gsi_fdw/postgres_fdw.c#L4103); its result is stored in `fmstate->gsi`
         - [GSI] [`gsi_index_for_rel`](../src/gsi_fdw/gsi.c#L115) → [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L84)
         - [GSI] [`server_node`](../src/gsi_fdw/gsi.c#L183): this partition's `base_node`
         - [PG] [`GetTopFullTransactionId`](../src/gsi_fdw/gsi.c#L261): forces a coordinator xid
  2. [fdw]+[GSI] [`postgresGetForeignModifyBatchSize`](../src/gsi_fdw/postgres_fdw.c#L2032): [GSI check, L2048](../src/gsi_fdw/postgres_fdw.c#L2048) reads `fmstate->gsi` and returns **1**. In this routed path `fmstate` always exists. The `gsi_index_for_rel` fallback is for `ExecInitModifyTable` under plain `EXPLAIN` of an INSERT aimed directly at a partition (e.g. `users_p0`), where `BeginForeignModify` skips creating `fmstate`.

### Per row
- [PG] [`ExecInsert`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/nodeModifyTable.c)
  - [fdw] [`postgresExecForeignInsert`](../src/gsi_fdw/postgres_fdw.c#L1970)
    - [fdw] [`execute_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4116)
      - [fdw] [`prepare_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4246) (first row): libpq `PQsendPrepare` of `INSERT INTO users_shard ...` (a protocol-level prepared statement, unrelated to `PREPARE TRANSACTION`)
      - [GSI] [`gsi_mark_conn(base_conn, false)`](../src/gsi_fdw/postgres_fdw.c#L4187): enables 2PC
      - [GSI] [`gsi_capture_row`](../src/gsi_fdw/gsi.c#L281), called at [L4188](../src/gsi_fdw/postgres_fdw.c#L4188)
        - [GSI] [`key_text`](../src/gsi_fdw/gsi.c#L267) → [`gsi_node_for_datum`](../src/gsi_fdw/gsi.c#L167): `new_key`, `new_node`
      - [fdw] [**base write**](../src/gsi_fdw/postgres_fdw.c#L4194): `PQsendQueryPrepared` runs the insert on the base shard
      - [GSI] [`gsi_apply_row`](../src/gsi_fdw/gsi.c#L370), called at [L4231](../src/gsi_fdw/postgres_fdw.c#L4231)
        - [GSI] [`send_index_op(insert)`](../src/gsi_fdw/gsi.c#L325)
          - [fdw] `GetConnection` (index node; opens a remote transaction there if needed)
          - [GSI] `gsi_mark_conn(idx_conn, true)` ([L334](../src/gsi_fdw/gsi.c#L334)): role `i`
          - [GSI] [`PQsendQueryParams`](../src/gsi_fdw/gsi.c#L346): `INSERT INTO public.gsi_users_email (key, id, shard) VALUES (...)`
          - [fdw] [`pgfdw_get_result`](../src/gsi_fdw/connection.c#L873). On error: [`pgfdw_report_error`](../src/gsi_fdw/connection.c#L938), e.g. **23505** for a unique GSI, which goes to the abort path in §8

`COPY users FROM ...` and `INSERT ... SELECT` follow the same per-row path.

---

## 3. `UPDATE users SET email = 'b@x' WHERE name = 'A'` (key changes)

### Planning
- [PG] `query_planner` → `add_other_rels_to_query` → [`expand_inherited_rtentry`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/inherit.c) → `expand_single_inheritance_child` → [`add_row_identity_columns`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/appendinfo.c) (once per partition)
  - [fdw]+[GSI] [`postgresAddForeignUpdateTargets`](../src/gsi_fdw/postgres_fdw.c#L1740): `ctid` (fdw), then [L1766](../src/gsi_fdw/postgres_fdw.c#L1766):
    - [GSI] [`gsi_add_update_targets`](../src/gsi_fdw/gsi.c#L211)
      - [GSI] [`add_junk_column`](../src/gsi_fdw/gsi.c#L193) ×2 → [PG] [`add_row_identity_var`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/appendinfo.c): `gsi_old_key`, `gsi_old_id`
- [PG] [`make_modifytable`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/plan/createplan.c)
  - [fdw]+[GSI] [`postgresPlanDirectModify`](../src/gsi_fdw/postgres_fdw.c#L2451): [GSI check, L2481](../src/gsi_fdw/postgres_fdw.c#L2481) returns **false**, so no pushdown
  - [fdw] [`postgresPlanForeignModify`](../src/gsi_fdw/postgres_fdw.c#L1774): deparses the per-row `UPDATE users_shard SET ... WHERE ctid = $1`
- Result: remote scan `SELECT ..., email, id, ctid FROM users_shard WHERE name = 'A' FOR UPDATE`

### Executor start (per unpruned partition, even if it ends up with no rows)
- [PG] `ExecInitModifyTable`
  - [fdw] [`postgresBeginForeignModify`](../src/gsi_fdw/postgres_fdw.c#L1913) → [`create_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L3994)
    - [GSI] [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L219)
      - [PG] [`ExecFindJunkAttributeInTlist`](../src/gsi_fdw/gsi.c#L241) ×2: positions of the junk columns
      - `key_updated = true`; an `id` assignment raises an [ERROR](../src/gsi_fdw/gsi.c#L257) (see §3b)
      - [PG] `GetTopFullTransactionId`

### Per row
- [PG] `ExecModifyTable` pulls a row from the subplan
  - [fdw] [`postgresIterateForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1597): the row comes back already locked by `FOR UPDATE`
- [PG] [`ExecUpdate`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/nodeModifyTable.c)
  - [fdw] [`postgresExecForeignUpdate`](../src/gsi_fdw/postgres_fdw.c#L2104) → [`execute_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4116)
    - [GSI] `gsi_mark_conn(base_conn, false)`
    - [GSI] [`gsi_capture_row`](../src/gsi_fdw/gsi.c#L281): `old_key` and `old_id` from `planSlot` junk, `new_key` from `slot`, each via `key_text` → `gsi_node_for_datum`
    - [fdw] **base write**: `UPDATE users_shard SET email = $2 WHERE ctid = $1`
    - [GSI] [`gsi_apply_row`](../src/gsi_fdw/gsi.c#L370)
      - [GSI] `send_index_op(delete)`: `DELETE FROM public.gsi_users_email WHERE key = $1 AND id = $2` on `hash(old_key)`
        - [GSI] [rows ≠ 1 → **40001**](../src/gsi_fdw/gsi.c#L357) (stale remote snapshot), which goes to §8
      - [GSI] `send_index_op(insert)`: `INSERT (...)` on `hash(new_key)` (23505 possible)

### 3a. `UPDATE users SET name = 'Z' ...` (key unchanged)
Same path, but `key_updated = false`. [`gsi_capture_row`](../src/gsi_fdw/gsi.c#L317) clears both keys, so `gsi_apply_row` sends nothing.

### 3b. `UPDATE users SET id = ...`
Stops in [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L257): `ERROR: cannot update column "id"`, then §8.

---

## 4. `DELETE FROM users WHERE id = 5`

- Planning: as in §3. PG also prunes the partitions, so only one partition is planned.
- Executor start: as in §3, giving `gsi_begin_modify` for that partition.
- Per row:
  - [PG] [`ExecDelete`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/nodeModifyTable.c)
    - [fdw] [`postgresExecForeignDelete`](../src/gsi_fdw/postgres_fdw.c#L2123) → [`execute_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4116)
      - [GSI] `gsi_capture_row`: `old_*` only
      - [fdw] **base write**: `DELETE FROM users_shard WHERE ctid = $1`
      - [GSI] `gsi_apply_row` → `send_index_op(delete)`, with the 40001 check

---

## 5. `SELECT * FROM users WHERE email = 'a@x'` (read path not implemented yet)

- [PG] planner, per partition:
  - `set_append_rel_size` → [fdw] [`postgresGetForeignRelSize`](../src/gsi_fdw/postgres_fdw.c#L626)
  - `set_append_rel_pathlist` → [fdw] [`postgresGetForeignPaths`](../src/gsi_fdw/postgres_fdw.c#L1014)
  - `create_plan` → [fdw] [`postgresGetForeignPlan`](../src/gsi_fdw/postgres_fdw.c#L1223)
- [PG] executor: `Append`, then per partition:
  - [fdw] [`postgresBeginForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1492) → `GetConnection`
  - [fdw] [`postgresIterateForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1597) → [`create_cursor`](../src/gsi_fdw/postgres_fdw.c#L3746) / [`fetch_more_data`](../src/gsi_fdw/postgres_fdw.c#L3822): `DECLARE c1 CURSOR FOR SELECT ... WHERE email = 'a@x'`, then `FETCH`, on **every** shard

No GSI code runs. M8 adds `set_rel_pathlist_hook` → `GsiLookup` CustomScan.

---

## 6. Any statement on a table without a GSI

Every [GSI] check ([L2048](../src/gsi_fdw/postgres_fdw.c#L2048), [L2481](../src/gsi_fdw/postgres_fdw.c#L2481), [L1766](../src/gsi_fdw/postgres_fdw.c#L1766), [L4103](../src/gsi_fdw/postgres_fdw.c#L4103)) gets `NULL` from `gsi_index_for_rel`, so `gsi_xact_2pc` stays false. The behaviour is exactly upstream postgres_fdw: direct modify, batching and a plain `COMMIT TRANSACTION` per shard.

---

## 7. `COMMIT` (transaction wrote a GSI table)

- [PG] [`CommitTransaction`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/access/transam/xact.c)
  - [PG] `CallXactCallbacks(XACT_EVENT_PRE_COMMIT)`
    - [fdw]+[GSI] [`pgfdw_xact_callback`](../src/gsi_fdw/connection.c#L994)
      - [GSI] [`gsi_log_xid`](../src/gsi_fdw/connection.c#L2320), called at [L1015](../src/gsi_fdw/connection.c#L1015): [`XLogFlush(LogLogicalMessage(...))`](../src/gsi_fdw/connection.c#L2323), making the xid durable
      - for each open remote transaction ([`case XACT_EVENT_PRE_COMMIT`](../src/gsi_fdw/connection.c#L1039)):
        - [GSI] [`gsi_prepare_remote_xact`](../src/gsi_fdw/connection.c#L2327), called at [L1053](../src/gsi_fdw/connection.c#L1053)
          - [`PREPARE TRANSACTION 'gsi_<fxid>_<server>_<i|b>'`](../src/gsi_fdw/connection.c#L2336) via [fdw] [`pgfdw_exec_query`](../src/gsi_fdw/connection.c#L845)
          - [status check](../src/gsi_fdw/connection.c#L2342): a reply of `ROLLBACK` instead of `PREPARE` raises ERROR, which goes to §8
        - [fdw] [`pgfdw_reset_xact_state`](../src/gsi_fdw/connection.c#L1353) ([L1125](../src/gsi_fdw/connection.c#L1125)): the remote session is now outside any transaction
      - [GSI] [`gsi_fail_point("after_prepare")`](../src/gsi_fdw/connection.c#L1147) → [`gsi_fail_point`](../src/gsi_fdw/gsi.c#L72)
  - [PG] `RecordTransactionCommit`: **the commit record is flushed. This is the decision.**
  - [PG] `CallXactCallbacks(XACT_EVENT_COMMIT)` (interrupts are held)
    - [fdw]+[GSI] `pgfdw_xact_callback`, at [L1005](../src/gsi_fdw/connection.c#L1005):
      - [GSI] [`gsi_finish_prepared(true)`](../src/gsi_fdw/connection.c#L2355)
        - [`gsi_fail_point("after_local_commit")`](../src/gsi_fdw/connection.c#L2362)
        - pass 0, role `i`: [`COMMIT PREPARED`](../src/gsi_fdw/connection.c#L2375) via [fdw] [`pgfdw_exec_cleanup_query`](../src/gsi_fdw/connection.c#L1494). Failure → WARNING + disconnect, left for §10
        - [`gsi_fail_point("mid_commit_prepared")`](../src/gsi_fdw/connection.c#L2385)
        - pass 1, role `b`: `COMMIT PREPARED`
        - [reset](../src/gsi_fdw/connection.c#L2394): `gsi_xact_2pc = false`
      - then the callback quick-exits: PRE_COMMIT already reset every remote transaction, so the fdw loop has nothing left to do

---

## 8. `ROLLBACK`, or any ERROR (before the decision)

- [PG] [`AbortTransaction`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/access/transam/xact.c) → `CallXactCallbacks(XACT_EVENT_ABORT)`
  - [fdw]+[GSI] [`pgfdw_xact_callback`](../src/gsi_fdw/connection.c#L994)
    - [GSI] [`gsi_finish_prepared(false)`](../src/gsi_fdw/connection.c#L1007): `ROLLBACK PREPARED` for connections already prepared. That happens when the error comes in PRE_COMMIT during or after our prepares (e.g. a failed PREPARE, or a later pre-commit step) but before the commit record. For errors during statements nothing is prepared yet, so it just resets state
    - [fdw] [`case XACT_EVENT_ABORT`](../src/gsi_fdw/connection.c#L1109) → [`pgfdw_abort_cleanup`](../src/gsi_fdw/connection.c#L1669): `ABORT TRANSACTION` on every still-open remote transaction

`ROLLBACK TO SAVEPOINT`: [PG] `CallSubXactCallbacks` → [fdw] [`pgfdw_subxact_callback`](../src/gsi_fdw/connection.c#L1164) → [`pgfdw_abort_cleanup(entry, false)`](../src/gsi_fdw/connection.c#L1234): `ROLLBACK TO SAVEPOINT sN`. This covers index connections too.

---

## 9. Crash during commit (`SET gsi.fail_point = '...'`)

- [GSI] [`gsi_fail_point`](../src/gsi_fdw/gsi.c#L72): `LOG`, then `abort()`
- [PG] postmaster: the child died, so it terminates all backends and runs crash recovery. The xid becomes *aborted* unless its commit record was flushed.
- Remote prepared transactions stay on the shards, holding their locks, until §10.

---

## 10. `SELECT gsi_resolve(0)`

- [GSI] [`gsi_resolve`](../src/gsi_fdw/gsi_resolver.c#L191) → [`gsi_resolve_all`](../src/gsi_fdw/gsi_resolver.c#L138)
  - [GSI] [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L84)
  - per node, [GSI] [`collect_node`](../src/gsi_fdw/gsi_resolver.c#L70)
    - [GSI] [`gsi_connect_raw`](../src/gsi_fdw/connection.c#L2399) → [fdw] `connect_pg_server` (no transaction, not cached)
    - [`SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'gsi\_%' AND prepared < now() - grace`](../src/gsi_fdw/gsi_resolver.c#L98)
    - [GSI] [`parse_gid`](../src/gsi_fdw/gsi_resolver.c#L57): fxid and role
  - pass 0 (`i`), then pass 1 (`b`), per GID:
    - [GSI] [`xact_status`](../src/gsi_fdw/gsi_resolver.c#L37): SPI [`pg_xact_status(fxid::xid8)`](../src/gsi_fdw/gsi_resolver.c#L48) on the coordinator
    - committed or aborted → [GSI] [`finish`](../src/gsi_fdw/gsi_resolver.c#L120): `COMMIT PREPARED` / `ROLLBACK PREPARED`. [42704](../src/gsi_fdw/gsi_resolver.c#L128) (already finished by its backend) is logged as success, not as a warning, but isn't counted in the returned number
    - in progress or unknown → skip
  - [`gsi_disconnect_raw`](../src/gsi_fdw/gsi_resolver.c#L181) for every connection

---

## 11. Known bypass: `TRUNCATE users`

- [PG] `ExecuteTruncate` → [fdw] [`postgresExecForeignTruncate`](../src/gsi_fdw/postgres_fdw.c#L3007): `TRUNCATE users_shard` on each shard. **No GSI code runs**, so the index entries become orphaned. Planned fix: reject TRUNCATE on GSI tables, or also truncate `gsi_<name>` on every node.
