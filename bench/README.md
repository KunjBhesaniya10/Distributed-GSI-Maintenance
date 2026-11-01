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

## Lookups: GSI vs broadcast

`bash bench/run_lookups.sh [seconds] [clients]` runs pgbench inside the coordinator with
`lookup_email.sql` (unique key) and `lookup_name.sql` (non-unique key), each with
`gsi.enable_lookup = on` (GsiLookup) and `off` (Append over every shard). `LOCAL_INDEX=1` first
creates an index on `email` and `name` on every shard, so broadcast uses index scans: the fair
baseline.

## Planned (M9, after checkpoint 1)

| Experiment | Measures | Variable |
|---|---|---|
| Lookup latency | p50/p99 latency, tps | nodes 2..5, rows 100k..1M, unique vs non-unique key, with/without shard indexes |
| Write cost | insert/update tps and latency | no GSI vs 1 vs 2 sync GSIs; clients 1..16 |
| Selectivity crossover | latency vs matching rows | rows per key 1..10k; input for the cost model |
| Recovery | time until the checker is clean | crash point (`gsi.fail_point`), resolver interval |
| Async lag (stretch) | WAL lag, staleness | write rate |

A TPC-C-style `customer` lookup by last name (non-unique, the case TPC-C's Payment and
Order-Status transactions use) is the planned standard workload next to the synthetic `users` data.
