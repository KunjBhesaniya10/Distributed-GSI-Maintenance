# Call Graph: gsi_fdw

How PostgreSQL core, postgres_fdw and our GSI code call each other for the main statements. Detailed per-statement stacks with line links: [call_stacks.md](call_stacks.md).

**Colors:** grey = PostgreSQL core, blue = unchanged postgres_fdw, orange = our GSI code (new, or a GSI change inside postgres_fdw). IDs (A1, D5, ...) match the walkthrough. Delete any node you don't want, along with its arrows.

Diagrams are [Mermaid](https://mermaid.js.org/). They render on GitHub; in VS Code install "Markdown Preview Mermaid Support" and open the preview (Ctrl+Shift+V).

---

## 1. Startup and hooks

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111

    PM["postmaster loads<br/>shared_preload_libraries = gsi_fdw"]:::pg
    A1["A1 _PG_init → gsi_init<br/>option.c:569, gsi.c:45<br/>GUCs: remote_lock_timeout, fail_point"]:::gsi
    A2["A2 gsi_resolver_init<br/>gsi_resolver.c:267<br/>RegisterBackgroundWorker"]:::gsi
    A3["A3 gsi_scan_init<br/>gsi_scan.c:481<br/>install set_rel_pathlist_hook"]:::gsi
    A4["A4 GetConnection (first call in a backend)<br/>connection.c:185<br/>RegisterXactCallback(pgfdw_xact_callback)"]:::fdw

    PM --> A1
    A1 --> A2
    A1 --> A3
    A2 -. "worker started after recovery" .-> I2["I2 gsi_resolver_main"]:::gsi
    A3 -. "planner calls" .-> F2["F2 gsi_set_rel_pathlist"]:::gsi
    A4 -. "every COMMIT / ABORT" .-> G2["G2 pgfdw_xact_callback"]:::fdw
```

---

## 2. Connections and remote transactions

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111

    C1["postgresBeginForeignScan<br/>(scan of a partition)"]:::fdw
    C2["create_foreign_modify<br/>(write to a partition)"]:::fdw
    C3["D7 send_index_op<br/>(index write, per row)"]:::gsi
    C4["F6 remote_query<br/>(GSI lookup hops)"]:::gsi
    C5["C2 exec_on_all_nodes<br/>(gsi_create / gsi_drop DDL)"]:::gsi

    GC["A4 GetConnection<br/>connection.c:185<br/>cache keyed by user mapping"]:::fdw
    NC["make_new_connection → connect_pg_server<br/>only if no cached conn (once per session)"]:::fdw
    BX["begin_remote_xact<br/>connection.c:754<br/>START TRANSACTION REPEATABLE READ<br/>only once per coordinator xact"]:::fdw

    C1 --> GC
    C2 --> GC
    C3 --> GC
    C4 --> GC
    C5 --> GC
    GC --> NC
    GC --> BX
```

---

## 3. Write path: INSERT / UPDATE / DELETE

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111

    subgraph PLAN["Planning (UPDATE / DELETE only)"]
        E1["E1 postgresAddForeignUpdateTargets<br/>postgres_fdw.c:1740"]:::fdw
        E1g["gsi_add_update_targets<br/>gsi.c:236<br/>junk cols: gsi_old_key_*, gsi_old_id"]:::gsi
        E2["E2 postgresPlanDirectModify<br/>postgres_fdw.c:2451"]:::fdw
        E2g["gsi_direct_modify_ok<br/>gsi.c:257<br/>false if an index entry can change"]:::gsi
        E1 --> E1g
        E2 --> E2g
    end

    subgraph INIT["Executor start (per partition)"]
        D1["INSERT: ExecFindPartition (first row routed)<br/>UPDATE/DELETE: ExecInitModifyTable"]:::pg
        D2["postgresBeginForeignInsert / BeginForeignModify<br/>→ create_foreign_modify<br/>postgres_fdw.c:3994"]:::fdw
        D3["D3 gsi_begin_modify<br/>gsi.c:284<br/>per-GSI state, reject SET id,<br/>force coordinator xid"]:::gsi
        D4["D4 postgresGetForeignModifyBatchSize<br/>returns 1 for GSI tables"]:::fdw
        D1 --> D2 --> D3
        D1 --> D4
    end

    subgraph ROW["Per row"]
        X["ExecInsert / ExecUpdate / ExecDelete"]:::pg
        X2["postgresExecForeign{Insert,Update,Delete}"]:::fdw
        D5["D5 execute_foreign_modify<br/>postgres_fdw.c:4116"]:::fdw
        G1["G1 gsi_mark_conn(base, false)<br/>gsi_xact_2pc = true"]:::gsi
        D6["D6 gsi_capture_row → capture_one<br/>gsi.c:403<br/>old/new key, node = hash(key) mod N"]:::gsi
        BW["base write on shard<br/>PQsendQueryPrepared"]:::fdw
        D7["D7 gsi_apply_row → send_index_op<br/>gsi.c:459, gsi.c:414<br/>DELETE old entry, INSERT new entry<br/>on the index node"]:::gsi
        ERR["23505 duplicate (unique GSI)<br/>40001 entry missing on delete<br/>→ abort"]:::gsi
        X --> X2 --> D5
        D5 -->|"1"| G1
        D5 -->|"2"| D6
        D5 -->|"3"| BW
        D5 -->|"4 (if row changed)"| D7
        D7 -.-> ERR
    end

    B3["B3 gsi_node_for_datum<br/>gsi.c:182<br/>hash_extended(key, seed) mod N"]:::gsi
    D6 --> B3

    PLAN --> INIT --> ROW
```

---

## 4. Commit and abort (2PC)

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111
    classDef decision fill:#fee2e2,stroke:#dc2626,color:#111

    CT["CommitTransaction"]:::pg
    PRE["XACT_EVENT_PRE_COMMIT<br/>→ pgfdw_xact_callback<br/>connection.c:994"]:::fdw
    LX["gsi_log_xid<br/>connection.c:2320<br/>flush WAL record with the xid"]:::gsi
    PR["G2 gsi_prepare_remote_xact (each participant)<br/>connection.c:2327<br/>PREPARE TRANSACTION 'gsi_#lt;fxid#gt;_#lt;server#gt;_#lt;i|b#gt;'"]:::gsi
    FP1["fail point: after_prepare"]:::gsi
    RC["G3 RecordTransactionCommit<br/>DECISION POINT"]:::decision
    POST["XACT_EVENT_COMMIT<br/>→ pgfdw_xact_callback"]:::fdw
    FIN["G4 gsi_finish_prepared(true)<br/>connection.c:2355"]:::gsi
    P0["pass 0: COMMIT PREPARED on index nodes (i)"]:::gsi
    P1["pass 1: COMMIT PREPARED on base shards (b)"]:::gsi
    W["failure → WARNING + disconnect<br/>left for the resolver"]:::gsi

    CT --> PRE --> LX --> PR --> FP1 --> RC --> POST --> FIN
    FIN -->|"fail point: after_local_commit"| P0
    P0 -->|"fail point: mid_commit_prepared"| P1
    FIN -.-> W

    AB["AbortTransaction (ERROR or ROLLBACK)"]:::pg
    ABE["XACT_EVENT_ABORT → pgfdw_xact_callback"]:::fdw
    H1a["H1 gsi_finish_prepared(false)<br/>ROLLBACK PREPARED (already prepared ones)"]:::gsi
    H1b["pgfdw_abort_cleanup<br/>ABORT TRANSACTION (still open ones)"]:::fdw
    PR -. "PREPARE fails" .-> AB
    AB --> ABE --> H1a
    ABE --> H1b
```

---

## 5. Read path: `SELECT ... WHERE email = 'a@x'`

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111

    subgraph PLAN["Planning"]
        F1["set_rel_pathlist(users)<br/>partition paths + broadcast Append"]:::pg
        F2["F2 gsi_set_rel_pathlist (hook)<br/>gsi_scan.c:142<br/>find_key_clause, prefer unique GSI,<br/>CustomPath cost = 0.1 × cheapest"]:::gsi
        F3["F3 gsi_plan_path<br/>gsi_scan.c:209<br/>CustomScan, all quals kept as filter"]:::gsi
        F1 --> F2 --> F3
    end

    subgraph EXEC["Execution"]
        F4["F4 gsi_begin_scan<br/>gsi_scan.c:272<br/>precompute per-shard SELECT ... id = ANY($1)"]:::gsi
        F5["F5 ExecCustomScan → gsi_exec_scan<br/>→ ExecScan(gsi_next, gsi_recheck)"]:::pg
        F6["F6 gsi_next → gsi_fetch<br/>gsi_scan.c:345"]:::gsi
        H1["hop 1: index node = hash(key) mod N<br/>SELECT id, shard FROM gsi_users_email WHERE key = $1"]:::gsi
        H2["hop 2: only listed shards<br/>SELECT ... WHERE id = ANY('{...}')"]:::gsi
        RK["ExecScan evaluates email = 'a@x'<br/>RECHECK drops stale entries"]:::pg
        F7["F7 gsi_explain_scan<br/>Index Node, Shards Fetched"]:::gsi
        F4 --> F5 --> F6 --> H1 --> H2 --> RK
        F5 -.-> F7
    end

    B3["B3 gsi_node_for_datum"]:::gsi
    H1 --> B3
    PLAN --> EXEC
```

---

## 6. Crash recovery

```mermaid
flowchart TD
    classDef pg fill:#e5e7eb,stroke:#6b7280,color:#111
    classDef fdw fill:#dbeafe,stroke:#2563eb,color:#111
    classDef gsi fill:#ffedd5,stroke:#ea580c,color:#111

    I1["I1 gsi_fail_point<br/>gsi.c:75<br/>abort() at the named step"]:::gsi
    CR["postmaster crash recovery<br/>xid aborted unless commit record flushed"]:::pg
    I2["I2 gsi_resolver_main (bgworker)<br/>gsi_resolver.c:243<br/>WaitLatch(resolver_interval)"]:::gsi
    RP["resolver_pass<br/>gsi_resolver.c:216<br/>own transaction"]:::gsi
    MAN["SELECT gsi_resolve(0)<br/>(manual)"]:::gsi
    I3["I3 gsi_resolve_all<br/>gsi_resolver.c:156"]:::gsi
    CN["collect_node (each node)<br/>raw connection, pg_prepared_xacts<br/>gid LIKE 'gsi_%' older than grace"]:::gsi
    XS["xact_status<br/>pg_xact_status(fxid) on coordinator"]:::gsi
    FN["finish<br/>COMMIT / ROLLBACK PREPARED<br/>pass i, then pass b"]:::gsi

    I1 --> CR
    CR -. "prepared xacts left on shards" .-> I2
    I2 --> RP --> I3
    MAN --> I3
    I3 --> CN --> XS --> FN
```
