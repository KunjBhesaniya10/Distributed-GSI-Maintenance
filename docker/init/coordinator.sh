#!/bin/bash
# Coordinator setup for NNODES data nodes (default 5): one foreign server and
# one hash partition per node.  Run once by the postgres image on an empty
# data volume; changing NNODES needs `docker compose down -v`.
#
# use_remote_estimate: the planner asks each shard (EXPLAIN) for the cost of
# the broadcast scan, so it sees shard-local indexes when it compares that
# plan with a GSI lookup (docs/phase2_plan.md D1.2).
N=${NNODES:-5}

{
    echo "CREATE EXTENSION gsi_fdw;"
    for i in $(seq 1 "$N"); do
        echo "CREATE SERVER shard$i FOREIGN DATA WRAPPER gsi_fdw
                  OPTIONS (host 'shard$i', port '5432', dbname 'postgres', use_remote_estimate 'true');"
        echo "CREATE USER MAPPING FOR PUBLIC SERVER shard$i OPTIONS (user 'postgres');"
        echo "INSERT INTO gsi.gsi_node VALUES ($((i - 1)), 'shard$i');"
    done
    cat <<'SQL'
-- Foreign partitions can't carry a PK; each shard's PK enforces id uniqueness.
-- ids come from a coordinator sequence (the default is applied before tuple
-- routing); PG16 has no identity columns on partitioned tables.
CREATE SEQUENCE users_id_seq;
CREATE TABLE users (id bigint NOT NULL DEFAULT nextval('users_id_seq'), email text NOT NULL, name text)
    PARTITION BY HASH (id);
ALTER SEQUENCE users_id_seq OWNED BY users.id;
SQL
    for i in $(seq 1 "$N"); do
        echo "CREATE FOREIGN TABLE users_p$((i - 1)) PARTITION OF users
                  FOR VALUES WITH (MODULUS $N, REMAINDER $((i - 1)))
                  SERVER shard$i OPTIONS (table_name 'users_shard');"
    done
} | psql -v ON_ERROR_STOP=1 --username "$POSTGRES_USER" --dbname "$POSTGRES_DB" -q
