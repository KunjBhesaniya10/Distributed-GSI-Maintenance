# Benchmarks

All commands run from the repo root with the cluster up (`cd docker && docker compose up -d --build`).
On Windows use Git Bash. **`pytest` truncates `users`**, so load data after running the tests.

## Data

`bench/load_users.sh [rows] [sessions]` (default 1,000,000 rows, 8 sessions) loads `users` through
the real write path (base row + GSI entries, 2PC), with two GSIs:

| Column | Values | GSI |
|---|---|---|
| `email` | `user<id>@<one of 5 domains>`, unique | `users_email` (unique) |
| `name` | `<first> <last>` from 40 × 40 common names: 1,600 distinct, ~625 rows each at 1M | `users_name` (non-unique) |

The data is generated deterministically in SQL, so every laptop gets the same table. Afterwards
`python tests/checker.py` verifies both GSIs (about 2 minutes at 1M rows).

`NAMES=sweep bash bench/load_users.sh` loads names for the selectivity sweep instead: every name
`k<K>_<n>` matches exactly K rows (K = 1, 10, 100, 1000), and `name` gets a large statistics
target so ANALYZE knows each K. Needed by `experiments.py lookups`.

## Lookups: GSI vs broadcast

`bash bench/run_lookups.sh [seconds] [clients]` runs pgbench (`-M prepared`) inside the coordinator
with `lookup_email.sql` (unique key) and `lookup_name.sql` (non-unique key), once with each plan
forced: `gsi.force_lookup = on` (GsiLookup) and `gsi.enable_lookup = off` (Append over every
shard). `LOCAL_INDEX=1` first creates an index on `email` and `name` on every shard, so broadcast
uses index scans: the fair baseline.

## Phase 2 experiments

What each one measures, the commands, and the results: [docs/phase2_results.md](../docs/phase2_results.md)
(sections E1-E7). CSVs go to `bench/results/`; `--tag` adds a suffix to the file names (e.g. the
network delay, which the script can't detect).

| File / command | Experiment | Result files |
|---|---|---|
| `python bench/experiments.py lookups` | E1: GSI speedup vs selectivity, planner choice, planning time | `lookups_<N>nodes[_tag].csv`, `planning_<N>nodes[_tag].csv` |
| `python bench/experiments.py writes` | E2: write cost, none / sync / async | `writes.csv` |
| `bash bench/node_sweep.sh` | E3: `lookups` on 2, 3, 5 nodes (fresh cluster each) | `lookups_<N>nodes.csv` |
| `python bench/experiments.py hot_keys` | E4: hot keys (Zipf) under sync writes | `hot_keys.csv` |
| `python bench/experiments.py async_lag` | E5: async lag and missed reads | `async_lag.csv` |
| `python bench/experiments.py build` | E6: online build time and impact on writes | `build.csv` |
| `python bench/experiments.py tpcc` | E7: TPC-C-derived customer workload | `tpcc_<N>nodes.csv` |
| `bash bench/net_delay.sh <ms>` | adds a round-trip delay between the containers (0 removes it) | - |

`bench/results/sequential_hop2/` holds the first round, measured before hop 2 was parallel.
