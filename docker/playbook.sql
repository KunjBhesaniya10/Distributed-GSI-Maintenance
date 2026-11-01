-- playbook.sql: set up the GSI and load sample rows for live testing.
--
--   docker compose exec -T coordinator psql -U postgres -X -f - < playbook.sql
--
-- Safe to re-run: it drops and recreates the GSI and reloads the data.

-- users and the gsi catalog come from init/coordinator.sql (first start only)
SELECT to_regclass('public.users') IS NOT NULL AND to_regclass('gsi.gsi_index') IS NOT NULL AS setup_ok \gset
\if :setup_ok
\else
\echo 'users / gsi catalog missing: the init SQL did not run.'
\echo 'Recreate the cluster:  docker compose down -v && docker compose up -d'
\quit
\endif

-- fresh unique GSI on users.email (drop first: TRUNCATE does not maintain the index)
SELECT gsi_drop('users') WHERE EXISTS (SELECT 1 FROM gsi.gsi_index WHERE base_table = 'users'::regclass);
TRUNCATE users;
SELECT gsi_create('users', 'email', true);

INSERT INTO users VALUES
    (1, 'frank@x', 'Frank'),
    (2, 'alice@x', 'Alice'),
    (3, 'bob@x',   'Bob'),
    (4, 'ivan@x',  'Ivan'),
    (5, 'judy@x',  'Judy'),
    (6, 'peggy@x', 'Peggy'),
    (7, 'sybil@x', 'Sybil'),
    (8, 'heidi@x', 'Heidi');

-- where each row lives: users_pN is on node N (0 = shard1, 1 = shard2, 2 = shard3)
SELECT id, email, tableoid::regclass AS base_partition, gsi_node_for(email) AS index_node
FROM users ORDER BY id;

-- log statements on the shards, so the 2PC commands show up in their logs
\connect postgres postgres shard1 5432
ALTER SYSTEM SET log_statement = 'all';
SELECT pg_reload_conf();
\connect postgres postgres shard2 5432
ALTER SYSTEM SET log_statement = 'all';
SELECT pg_reload_conf();
\connect postgres postgres shard3 5432
ALTER SYSTEM SET log_statement = 'all';
SELECT pg_reload_conf();

\echo
\echo 'Ready.  Open a session:  docker compose exec coordinator psql -U postgres'
\echo 'Example queries are at the end of playbook.sql.'

/*
-------------------------------------------------------------------------------
-- 1. Read uses the GSI path
EXPLAIN SELECT * FROM users WHERE email = 'alice@x';             -- Custom Scan (GsiLookup)
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF)
    SELECT * FROM users WHERE email = 'alice@x';                 -- Index Node, Shards Fetched: 1
EXPLAIN SELECT * FROM users WHERE name = 'Alice';                -- not indexed: Append (broadcast)

-- 2. UPDATE / DELETE keep table and index in sync
UPDATE users SET email = 'robert@x' WHERE id = 3;                -- key change: entry moves node
UPDATE users SET name = 'Rob' WHERE id = 3;                      -- non-key change: entry untouched
DELETE FROM users WHERE id = 1;                                  -- row and entry removed
SELECT id, email, tableoid::regclass, gsi_node_for(email) FROM users ORDER BY id;
--   these two tables exist only on the shards, not the coordinator:
--   switch with \c postgres postgres shard1 5432  (shard2, shard3; back: \c postgres postgres localhost 5432)
SELECT * FROM users_shard;
SELECT * FROM gsi_users_email;                                   -- (key, id, shard of the base row)

-- 3. 2PC
--   on the coordinator:
INSERT INTO users VALUES (9, 'zoe@x', 'Zoe');
--   in a shell (docker/ folder):
--   docker compose logs --since 1m shard1 shard2 shard3 | grep -E "PREPARE TRANSACTION|COMMIT PREPARED"
--   -> PREPARE TRANSACTION 'gsi_<xid>_shardN_i|b' on each participant,
--      then COMMIT PREPARED on the _i participants before the _b ones.
INSERT INTO users VALUES (10, 'alice@x', 'Fake');                -- duplicate: whole txn rolls back
SELECT * FROM users WHERE id = 10;                               -- 0 rows
-------------------------------------------------------------------------------
*/
