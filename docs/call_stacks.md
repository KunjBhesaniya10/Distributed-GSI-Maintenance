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

- [GSI] [`gsi_create`](../src/gsi_fdw/gsi.c#L511)
  - [GSI] [`gsi_indexes_for_rel`](../src/gsi_fdw/gsi.c#L117) + [`index_on_column`](../src/gsi_fdw/gsi.c#L540): checks this column has no GSI yet (other columns may)
  - [PG] SPI [`SELECT 1 FROM users LIMIT 1`](../src/gsi_fdw/gsi.c#L551): emptiness check
    - [fdw] foreign scan of each partition: [`postgresBeginForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1492) → [`GetConnection`](../src/gsi_fdw/postgres_fdw.c#L1535) opens a remote transaction on each shard, then [`postgresIterateForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1597) fetches rows
  - [PG] SPI [`INSERT INTO gsi.gsi_index`](../src/gsi_fdw/gsi.c#L562)
  - [GSI] [`gsi_indexes_for_rel`](../src/gsi_fdw/gsi.c#L569): reads the row back into a `GsiIndex`
    - [GSI] [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L86)
  - [GSI] [`exec_on_all_nodes`](../src/gsi_fdw/gsi.c#L475) (×2 for a unique GSI)
    - [fdw] `GetConnection`
    - [GSI] [`gsi_mark_conn(conn, true)`](../src/gsi_fdw/connection.c#L2292): enables 2PC, role `i`
    - [fdw] [`do_sql_command`](../src/gsi_fdw/connection.c#L711): `CREATE TABLE public.gsi_users_email (...)`, then `CREATE UNIQUE INDEX ... (key)`
- COMMIT → **§7**: tables and catalog row appear everywhere or nowhere.

A table can have several GSIs, one per column (e.g. `gsi_create('users', 'name')` next to the email GSI); every flow below then runs its GSI steps once per GSI.

`gsi_drop('users', 'email')` drops one GSI; `gsi_drop('users')` drops all of them. Per GSI: [`gsi_drop`](../src/gsi_fdw/gsi.c#L588) → SPI [`DELETE FROM gsi.gsi_index WHERE index_name = ...`](../src/gsi_fdw/gsi.c#L629) → [`exec_on_all_nodes`](../src/gsi_fdw/gsi.c#L634) `DROP TABLE IF EXISTS ...` → COMMIT §7.

---

## 2. `INSERT INTO users VALUES (5, 'a@x', 'A')`

### Planning
No GSI code. Partitions are chosen by tuple routing at execution time, so postgres_fdw builds the remote `INSERT` in `postgresBeginForeignInsert` ([`deparseInsertSql`, L2254](../src/gsi_fdw/postgres_fdw.c#L2254)), not in the planner.

### Executor start (once per partition, when the first row is routed to it)
- [PG] [`ExecFindPartition`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/execPartition.c) → `ExecInitRoutingInfo`
  1. [fdw] [`postgresBeginForeignInsert`](../src/gsi_fdw/postgres_fdw.c#L2160)
     - [fdw] [`create_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L3994) → `GetConnection` (base shard)
       - [GSI] [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L283), called at [L4103](../src/gsi_fdw/postgres_fdw.c#L4103); its result (a List with one `GsiModifyState` per GSI) is stored in `fmstate->gsi`
         - [GSI] [`gsi_indexes_for_rel`](../src/gsi_fdw/gsi.c#L117) → [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L86)
         - [GSI] [`gsi_server_node`](../src/gsi_fdw/gsi.c#L319): this partition's `base_node`
         - [PG] [`GetTopFullTransactionId`](../src/gsi_fdw/gsi.c#L339): forces a coordinator xid
  2. [fdw]+[GSI] [`postgresGetForeignModifyBatchSize`](../src/gsi_fdw/postgres_fdw.c#L2032): [GSI check, L2048](../src/gsi_fdw/postgres_fdw.c#L2048) reads `fmstate->gsi` and returns **1**. In this routed path `fmstate` always exists. The `gsi_indexes_for_rel` fallback is for `ExecInitModifyTable` under plain `EXPLAIN` of an INSERT aimed directly at a partition (e.g. `users_p0`), where `BeginForeignModify` skips creating `fmstate`.

### Per row
- [PG] [`ExecInsert`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/nodeModifyTable.c)
  - [fdw] [`postgresExecForeignInsert`](../src/gsi_fdw/postgres_fdw.c#L1970)
    - [fdw] [`execute_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4116)
      - [fdw] [`prepare_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4246) (first row): libpq `PQsendPrepare` of `INSERT INTO users_shard ...` (a protocol-level prepared statement, unrelated to `PREPARE TRANSACTION`)
      - [GSI] [`gsi_mark_conn(base_conn, false)`](../src/gsi_fdw/postgres_fdw.c#L4187): enables 2PC
      - [GSI] [`gsi_capture_row`](../src/gsi_fdw/gsi.c#L402), called at [L4188](../src/gsi_fdw/postgres_fdw.c#L4188), runs [`capture_one`](../src/gsi_fdw/gsi.c#L358) per GSI
        - [GSI] [`key_text`](../src/gsi_fdw/gsi.c#L344) → [`gsi_node_for_datum`](../src/gsi_fdw/gsi.c#L181): `new_key`, `new_node` (a NULL key gets no entry)
      - [fdw] [**base write**](../src/gsi_fdw/postgres_fdw.c#L4194): `PQsendQueryPrepared` runs the insert on the base shard
      - [GSI] [`gsi_apply_row`](../src/gsi_fdw/gsi.c#L458), called at [L4231](../src/gsi_fdw/postgres_fdw.c#L4231), per GSI:
        - [GSI] [`send_index_op(insert)`](../src/gsi_fdw/gsi.c#L413)
          - [fdw] `GetConnection` (index node; opens a remote transaction there if needed)
          - [GSI] `gsi_mark_conn(idx_conn, true)` ([L423](../src/gsi_fdw/gsi.c#L423)): role `i`
          - [GSI] [`PQsendQueryParams`](../src/gsi_fdw/gsi.c#L435): `INSERT INTO public.gsi_users_email (key, id, shard) VALUES (...)`
          - [fdw] [`pgfdw_get_result`](../src/gsi_fdw/connection.c#L873). On error: [`pgfdw_report_error`](../src/gsi_fdw/connection.c#L938), e.g. **23505** for a unique GSI, which goes to the abort path in §8

`COPY users FROM ...` and `INSERT ... SELECT` follow the same per-row path.

---

## 3. `UPDATE users SET email = 'b@x' WHERE name = 'A'` (key changes)

### Planning
- [PG] `query_planner` → `add_other_rels_to_query` → [`expand_inherited_rtentry`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/inherit.c) → `expand_single_inheritance_child` → [`add_row_identity_columns`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/appendinfo.c) (once per partition)
  - [fdw]+[GSI] [`postgresAddForeignUpdateTargets`](../src/gsi_fdw/postgres_fdw.c#L1740): `ctid` (fdw), then [L1766](../src/gsi_fdw/postgres_fdw.c#L1766):
    - [GSI] [`gsi_add_update_targets`](../src/gsi_fdw/gsi.c#L236)
      - [GSI] [`add_junk_column`](../src/gsi_fdw/gsi.c#L207) once per GSI plus once for id → [PG] [`add_row_identity_var`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/util/appendinfo.c): `gsi_old_key_users_email`, ..., `gsi_old_id`
- [PG] [`make_modifytable`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/optimizer/plan/createplan.c)
  - [fdw]+[GSI] [`postgresPlanDirectModify`](../src/gsi_fdw/postgres_fdw.c#L2451): [GSI check, L2481](../src/gsi_fdw/postgres_fdw.c#L2481) → [`gsi_direct_modify_ok`](../src/gsi_fdw/gsi.c#L257): the UPDATE sets an indexed column, so it returns **false** and there is no pushdown
  - [fdw] [`postgresPlanForeignModify`](../src/gsi_fdw/postgres_fdw.c#L1774): deparses the per-row `UPDATE users_shard SET ... WHERE ctid = $1`
- Result: remote scan `SELECT ..., email, id, ctid FROM users_shard WHERE name = 'A' FOR UPDATE`

### Executor start (per unpruned partition, even if it ends up with no rows)
- [PG] `ExecInitModifyTable`
  - [fdw] [`postgresBeginForeignModify`](../src/gsi_fdw/postgres_fdw.c#L1913) → [`create_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L3994)
    - [GSI] [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L283)
      - [PG] [`ExecFindJunkAttributeInTlist`](../src/gsi_fdw/gsi.c#L299): position of `gsi_old_id`, then [one per GSI](../src/gsi_fdw/gsi.c#L328) for its old key
      - an `id` assignment raises an [ERROR](../src/gsi_fdw/gsi.c#L309) (see §3b); `key_updated = true` for the email GSI
      - [PG] `GetTopFullTransactionId`

### Per row
- [PG] `ExecModifyTable` pulls a row from the subplan
  - [fdw] [`postgresIterateForeignScan`](../src/gsi_fdw/postgres_fdw.c#L1597): the row comes back already locked by `FOR UPDATE`
- [PG] [`ExecUpdate`](https://github.com/postgres/postgres/blob/REL_16_STABLE/src/backend/executor/nodeModifyTable.c)
  - [fdw] [`postgresExecForeignUpdate`](../src/gsi_fdw/postgres_fdw.c#L2104) → [`execute_foreign_modify`](../src/gsi_fdw/postgres_fdw.c#L4116)
    - [GSI] `gsi_mark_conn(base_conn, false)`
    - [GSI] [`gsi_capture_row`](../src/gsi_fdw/gsi.c#L402), per GSI: `old_key` and `old_id` from `planSlot` junk, `new_key` from `slot` (or the old key if this GSI's column isn't assigned), each via `key_text` → `gsi_node_for_datum`
    - [fdw] **base write**: `UPDATE users_shard SET email = $2 WHERE ctid = $1`
    - [GSI] [`gsi_apply_row`](../src/gsi_fdw/gsi.c#L458), per GSI whose key changed:
      - [GSI] `send_index_op(delete)`: `DELETE FROM public.gsi_users_email WHERE key = $1 AND id = $2` on `hash(old_key)`
        - [GSI] [rows ≠ 1 → **40001**](../src/gsi_fdw/gsi.c#L450) (stale remote snapshot), which goes to §8
      - [GSI] `send_index_op(insert)`: `INSERT (...)` on `hash(new_key)` (23505 possible)

### 3a. `UPDATE users SET name = 'Z' ...` (no indexed column assigned)
If no GSI covers `name`, [`gsi_direct_modify_ok`](../src/gsi_fdw/gsi.c#L257) returns **true**: the UPDATE can't change an index entry, so postgres_fdw pushes it down whole (`UPDATE users_shard SET name = 'Z' WHERE ...`) with no per-row callbacks, exactly as upstream. If the statement also needs a local filter or a join, postgres_fdw falls back to the per-row path; there [`capture_one`](../src/gsi_fdw/gsi.c#L398) sees an unchanged key, clears it, and `gsi_apply_row` sends nothing. A pushed-down UPDATE commits with plain `COMMIT` unless the same transaction wrote a GSI, in which case §7 prepares its shards too.

### 3b. `UPDATE users SET id = ...`
Stops in [`gsi_begin_modify`](../src/gsi_fdw/gsi.c#L309): `ERROR: cannot update column "id"`, then §8.

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

## 5. `SELECT * FROM users WHERE email = 'a@x'` (GSI read path, M8)

### Planning
- [PG] planner: each partition is planned as usual ([fdw] `postgresGetForeignRelSize`, `postgresGetForeignPaths`), and the broadcast Append is built
- [PG] `set_rel_pathlist` for the partitioned parent, then calls `set_rel_pathlist_hook`
  - [GSI] [`gsi_set_rel_pathlist`](../src/gsi_fdw/gsi_scan.c#L137-L201)
    - checks: partitioned base rel, not dummy, not a write target, no `FOR UPDATE`
    - [GSI] [`gsi_indexes_for_rel`](../src/gsi_fdw/gsi.c#L117), then for each ready GSI [`find_key_clause`](../src/gsi_fdw/gsi_scan.c#L102-L135): `email = Const/Param`. If several GSIs match (e.g. `email = ... AND name = ...`), a unique one wins
    - [PG] `add_path`: CustomPath, cost 0.1 × the Append; `custom_private` = key + chosen index name
    - clears `rel->part_scheme`, so [PG] `apply_scanjoin_target_to_paths` keeps our path instead of rebuilding the Append
- [PG] `create_plan` → `create_customscan_plan` → [GSI] [`gsi_plan_path`](../src/gsi_fdw/gsi_scan.c#L203-L218): `CustomScan`, with every qual as the filter

### Executor start
- [PG] `ExecInitCustomScan` (opens `users`, makes the scan slot) → [GSI] [`gsi_create_scan_state`](../src/gsi_fdw/gsi_scan.c#L220-L228), then [`gsi_begin_scan`](../src/gsi_fdw/gsi_scan.c#L266-L320)
  - `ExecInitExpr(key)`; [`gsi_indexes_for_rel`](../src/gsi_fdw/gsi.c#L117), picking the plan's GSI by [name](../src/gsi_fdw/gsi_scan.c#L92-L96)
  - `find_inheritance_children` → per partition [`remote_table`](../src/gsi_fdw/gsi_scan.c#L247-L264) / [`remote_column`](../src/gsi_fdw/gsi_scan.c#L231-L244) → `SELECT <cols> FROM public.users_shard WHERE id = ANY($1)`

### Per row
- [PG] `ExecCustomScan` → [GSI] [`gsi_exec_scan`](../src/gsi_fdw/gsi_scan.c#L442-L446) → [PG] `ExecScan(gsi_next, gsi_recheck)`
  - [GSI] [`gsi_next`](../src/gsi_fdw/gsi_scan.c#L416-L433), first call: [`gsi_fetch`](../src/gsi_fdw/gsi_scan.c#L339-L414)
    - evaluate the key → [`gsi_node_for_datum`](../src/gsi_fdw/gsi.c#L181) → index node
    - **hop 1**, [`remote_query`](../src/gsi_fdw/gsi_scan.c#L323-L336) → [fdw] `GetConnection` → `SELECT id, shard FROM public.gsi_users_email WHERE key = $1`
    - group the ids by `shard`
    - **hop 2**, for each listed shard only, all sent before any result is read: [fdw] `GetConnection` → `PQsendQueryParams` `SELECT ... WHERE id = ANY('{...}')`; then `pgfdw_get_result` per shard, in shard order
    - [fdw] `set_transmission_modes`; [PG] `BuildTupleFromCStrings` → buffer
  - each call returns the next buffered row (`ExecForceStoreHeapTuple`)
  - [PG] `ExecScan` evaluates the plan filter `email = 'a@x'`: **the recheck**, which drops stale entries

### Not routed through the GSI
`UPDATE/DELETE ... WHERE email = ?` (the table is the write target), `FOR UPDATE`, non-equality predicates, and join-parameterized keys all fall back to the broadcast Append, and their results are still correct.

---

## 6. Any statement on a table without a GSI

Every [GSI] check ([L2048](../src/gsi_fdw/postgres_fdw.c#L2048), [L2481](../src/gsi_fdw/postgres_fdw.c#L2481), [L1764](../src/gsi_fdw/postgres_fdw.c#L1764), [L4103](../src/gsi_fdw/postgres_fdw.c#L4103)) gets `NIL` from `gsi_indexes_for_rel`, so `gsi_xact_2pc` stays false. The behaviour is exactly upstream postgres_fdw: direct modify, batching and a plain `COMMIT TRANSACTION` per shard.

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
      - [GSI] [`gsi_fail_point("after_prepare")`](../src/gsi_fdw/connection.c#L1147) → [`gsi_fail_point`](../src/gsi_fdw/gsi.c#L74)
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

- [GSI] [`gsi_fail_point`](../src/gsi_fdw/gsi.c#L75): `LOG`, then `abort()`
- [PG] postmaster: the child died, so it terminates all backends and runs crash recovery. The xid becomes *aborted* unless its commit record was flushed.
- Remote prepared transactions stay on the shards, holding their locks, until §10.

---

## 10. `SELECT gsi_resolve(0)`

- [GSI] [`gsi_resolve`](../src/gsi_fdw/gsi_resolver.c#L209) → [`gsi_resolve_all`](../src/gsi_fdw/gsi_resolver.c#L156)
  - [GSI] [`gsi_load_nodes`](../src/gsi_fdw/gsi.c#L86)
  - per node, [GSI] [`collect_node`](../src/gsi_fdw/gsi_resolver.c#L88)
    - [GSI] [`gsi_connect_raw`](../src/gsi_fdw/connection.c#L2399) → [fdw] `connect_pg_server` (no transaction, not cached)
    - [`SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'gsi\_%' AND prepared < now() - grace`](../src/gsi_fdw/gsi_resolver.c#L116)
    - [GSI] [`parse_gid`](../src/gsi_fdw/gsi_resolver.c#L75): fxid and role
  - pass 0 (`i`), then pass 1 (`b`), per GID:
    - [GSI] [`xact_status`](../src/gsi_fdw/gsi_resolver.c#L55): SPI [`pg_xact_status(fxid::xid8)`](../src/gsi_fdw/gsi_resolver.c#L66) on the coordinator
    - committed or aborted → [GSI] [`finish`](../src/gsi_fdw/gsi_resolver.c#L138): `COMMIT PREPARED` / `ROLLBACK PREPARED`. [42704](../src/gsi_fdw/gsi_resolver.c#L146) (already finished by its backend) is logged as success, not as a warning, but isn't counted in the returned number
    - in progress or unknown → skip
  - [`gsi_disconnect_raw`](../src/gsi_fdw/gsi_resolver.c#L199) for every connection

---

## 10b. Background resolver (M7)
- [PG] postmaster starts the `gsi resolver` worker (registered by [`gsi_resolver_init`](../src/gsi_fdw/gsi_resolver.c#L266-L308)), and restarts it 5 s after any crash
  - [GSI] [`gsi_resolver_main`](../src/gsi_fdw/gsi_resolver.c#L242-L264): connect to `gsi.resolver_database`; loop `WaitLatch(gsi.resolver_interval)`
    - [GSI] [`resolver_pass`](../src/gsi_fdw/gsi_resolver.c#L215-L240): transaction + snapshot → [`gsi_resolve_all(gsi.resolver_grace)`](../src/gsi_fdw/gsi_resolver.c#L156), exactly as in §10, but only for GIDs older than the grace period, so it never races a live commit

---

## 11. Known bypass: `TRUNCATE users`

- [PG] `ExecuteTruncate` → [fdw] [`postgresExecForeignTruncate`](../src/gsi_fdw/postgres_fdw.c#L3007): `TRUNCATE users_shard` on each shard. **No GSI code runs**, so the index entries become orphaned. Planned fix: reject TRUNCATE on GSI tables, or also truncate `gsi_<name>` on every node.
