#!/usr/bin/env bash
# Reproduces the v1 read-path bug: two partitions of one table on the same
# data node.  The GSI lookup returns only the rows of one partition per node.
# Run from docker/ on a running cluster (Git Bash on Windows):
#   bash repro_two_partitions.sh          # set up and compare
#   bash repro_two_partitions.sh clean    # remove the test objects
set -e
psql_on() { docker compose exec -T "$1" psql -U postgres -qX "${@:2}"; }

clean() {
    # DROP TABLE t alone would leave the GSI catalog row and index tables behind
    psql_on coordinator -c "SELECT gsi_drop('t') FROM pg_class WHERE oid = to_regclass('t') AND EXISTS (SELECT 1 FROM gsi.gsi_index WHERE base_table = oid)" \
                        -c "DROP TABLE IF EXISTS t" \
                        -c "DELETE FROM gsi.gsi_index WHERE index_name = 't_email'"
    for s in shard1 shard2 shard3; do psql_on "$s" -c "DROP TABLE IF EXISTS gsi_t_email, t_a, t_b"; done
}

clean 2>/dev/null
[ "$1" = "clean" ] && exit 0

for s in shard1 shard2 shard3; do
    psql_on "$s" -c "CREATE TABLE t_a (id bigint PRIMARY KEY, email text NOT NULL)" \
                 -c "CREATE TABLE t_b (id bigint PRIMARY KEY, email text NOT NULL)"
done

psql_on coordinator -v ON_ERROR_STOP=1 <<'EOF'
CREATE TABLE t (id bigint NOT NULL, email text NOT NULL) PARTITION BY HASH (id);
-- 6 partitions on 3 nodes: two per node, each in its own remote table
CREATE FOREIGN TABLE t_p0 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 0) SERVER shard1 OPTIONS (table_name 't_a');
CREATE FOREIGN TABLE t_p1 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 1) SERVER shard2 OPTIONS (table_name 't_a');
CREATE FOREIGN TABLE t_p2 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 2) SERVER shard3 OPTIONS (table_name 't_a');
CREATE FOREIGN TABLE t_p3 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 3) SERVER shard1 OPTIONS (table_name 't_b');
CREATE FOREIGN TABLE t_p4 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 4) SERVER shard2 OPTIONS (table_name 't_b');
CREATE FOREIGN TABLE t_p5 PARTITION OF t FOR VALUES WITH (MODULUS 6, REMAINDER 5) SERVER shard3 OPTIONS (table_name 't_b');
SELECT gsi_create('t', 'email');
INSERT INTO t SELECT g, 'same@x' FROM generate_series(1, 30) g;

\echo 'rows per partition:'
SELECT tableoid::regclass AS partition, count(*) FROM t GROUP BY 1 ORDER BY 1;
\echo 'broadcast (email || '''' is not a plain column, so no GSI): expect 30'
SELECT count(*) FROM t WHERE email || '' = 'same@x';
\echo 'through the GSI: with the bug, only the t_p3..t_p5 rows come back'
EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF, SUMMARY OFF) SELECT * FROM t WHERE email = 'same@x';
SELECT count(*) FROM t WHERE email = 'same@x';
EOF
