#!/usr/bin/env bash
# Lookup latency: GSI vs broadcast (every shard), with pgbench inside the
# coordinator container.  Needs data from bench/load_users.sh.
#
#   bash bench/run_lookups.sh [seconds] [clients]     default: 20 s, 4 clients
#
# Broadcast runs the same query with SET gsi.enable_lookup = off.  -M prepared:
# with use_remote_estimate every planning asks each shard (EXPLAIN), so a
# per-statement plan would mostly measure planning.  The shards
# have no local index on email/name, so broadcast means a scan on every shard;
# run with LOCAL_INDEX=1 to add per-shard indexes first (fairer baseline).
set -e
export MSYS_NO_PATHCONV=1       
SECS=${1:-20}
CLIENTS=${2:-4}
cd "$(dirname "$0")/../docker"
NNODES=$(docker compose exec -T coordinator psql -U postgres -tAqX -c "SELECT count(*) FROM gsi.gsi_node")
ROWS=$(docker compose exec -T coordinator psql -U postgres -tAqX -c "SELECT count(*) FROM users")
echo "nodes=$NNODES rows=$ROWS clients=$CLIENTS duration=${SECS}s"

for i in $(seq 1 "$NNODES"); do
    if [ "${LOCAL_INDEX:-0}" = 1 ]; then
        docker compose exec -T "shard$i" psql -U postgres -qX \
            -c "CREATE INDEX IF NOT EXISTS users_shard_email ON users_shard (email)" \
            -c "CREATE INDEX IF NOT EXISTS users_shard_name ON users_shard (name)"
    else
        docker compose exec -T "shard$i" psql -U postgres -qX \
            -c "DROP INDEX IF EXISTS users_shard_email, users_shard_name"
    fi
done

for script in lookup_email lookup_name; do
    docker compose cp "../bench/$script.sql" "coordinator:/tmp/$script.sql" >/dev/null
    for mode in on off; do
        label=$([ $mode = on ] && echo "GSI      " || echo "broadcast")
        # force each plan: with the cost model the planner may pick either one
        opt=$([ $mode = on ] && echo "gsi.force_lookup=on" || echo "gsi.enable_lookup=off")
        out=$(docker compose exec -T -e PGOPTIONS="-c $opt" coordinator \
              pgbench -U postgres -n -M prepared -c "$CLIENTS" -j "$CLIENTS" -T "$SECS" \
                      -D rows="$ROWS" -f "/tmp/$script.sql" postgres 2>&1)
        tps=$(echo "$out" | sed -n 's/^tps = \([0-9.]*\).*/\1/p')
        lat=$(echo "$out" | sed -n 's/^latency average = \(.*\)$/\1/p')
        printf '%-13s %s  tps=%-10s avg latency=%s\n' "$script" "$label" "$tps" "$lat"
    done
done
