#!/usr/bin/env bash
# E3: GSI speedup vs number of data nodes.  For each N: fresh cluster with
# NNODES=N, 1M rows with the selectivity-sweep names, then E1 (`lookups`).
# Results: bench/results/lookups_<N>nodes.csv.  Takes ~5 min per N.
#
#   bash bench/node_sweep.sh [secs per run]     default 10
set -e
SECS=${1:-10}
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
for n in 2 3 5; do
    echo "=== NNODES=$n ==="
    (cd "$ROOT/docker" && docker compose down -v && NNODES=$n docker compose up -d --wait)
    NAMES=sweep bash "$ROOT/bench/load_users.sh"
    NNODES=$n python "$ROOT/bench/experiments.py" lookups --secs "$SECS"
done
# leave the default 5-node cluster behind (data must be reloaded)
(cd "$ROOT/docker" && docker compose down -v && docker compose up -d --wait)
