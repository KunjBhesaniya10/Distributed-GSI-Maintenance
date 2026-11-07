# Hands-On Playbook

How to set up, test, load and benchmark the cluster, and what to do when something breaks.

**The feature walkthrough (writes, 2PC, uniqueness, read path, failures, crash recovery) is in [demo.md](demo.md)**, written as the final-presentation demo.

Commands work in **PowerShell** and **Git Bash**. Run everything from the `docker/` folder unless stated otherwise. Each step says **what to expect**. If you see something else, check §4 (troubleshooting).

---

## New laptop: verify, test, load 1M rows, benchmark (~7 min)

Run from the repo root in **Git Bash** (the `bench/` scripts are bash). The cluster is 1 coordinator + 5 data nodes.

**0. Start the cluster** (first time on this laptop, or after the node count changed):
```
git pull
cd docker
docker compose down -v                 # wipes old volumes, e.g. from the old 3-node setup
docker compose up -d --build
```

**1. Check the cluster** (from `docker/`):
```
docker compose ps
docker compose exec -T coordinator psql -U postgres -c "SELECT * FROM gsi.gsi_node"
```
Expect 6 containers, all `(healthy)`, and **5 rows** (`shard1` … `shard5`). Only 3 rows means old volumes: `docker compose down -v`, then `docker compose up -d --build`.

**2. Test suite** (~1.5 min). Proves the build works on this laptop:
```
pip install psycopg2-binary pytest      # once per laptop
cd tests
python -m pytest -q .
python checker.py
```
Expect `0 failed` (every test passes) and `users_email: rows=... missing=0 orphaned=0 misplaced=0 prepared=0`.

**3. Demo data: 1M rows** (~80 s). Run it **after** the tests, because they truncate `users`:
```
cd ..
bash bench/load_users.sh
python tests/checker.py
```
Expect `loaded in ~80s` (slower on a weaker laptop), then `users_email` and `users_name` both with `rows=1000000` and all zeros (the checker takes ~2 min).

**4. Benchmark: GSI vs broadcast** (~2 min each):
```
bash bench/run_lookups.sh 15 4                  # shards without a local index
LOCAL_INDEX=1 bash bench/run_lookups.sh 15 4    # each shard has its own index (fair baseline)
```
Reference on a 16-CPU laptop: unique email GSI ~1.5 ms vs broadcast ~57 ms (no shard index) and ~3.2 ms (with shard indexes). Details and the full table: [progress.md](progress.md), [bench/README.md](../bench/README.md).

**5. Phase 2 benchmarks** (cost model, sync vs async, online build): see [phase2_results.md](phase2_results.md) for the commands and results; the harness is `bench/experiments.py`.

**6. Demo by hand:** [demo.md](demo.md) (acts 1-6; act 2 is the GSI vs broadcast comparison, act 6 crash recovery).

**If a script fails** with `$'\r': command not found` (Windows line endings), run `sed -i 's/\r$//' bench/*.sh docker/*.sh` once from the repo root.

### When to run `docker compose` again

| Situation | Command (from `docker/`) | Data kept? |
|---|---|---|
| Normal use: psql, tests, benchmarks | nothing | yes |
| After a reboot or quitting Docker Desktop | `docker compose up -d` | yes |
| After changing C code in `src/gsi_fdw` | `docker compose up -d --build` | yes (rebuilds the image, same volumes) |
| After changing `docker-compose.yml`, `docker/init/*`, or the extension SQL (`gsi_fdw--1.0.sql`), or to start clean | `docker compose down -v` then `docker compose up -d --build` | **no**, wiped |
| Use 2 or 3 data nodes instead of 5 | `docker compose down -v` then `NNODES=3 docker compose up -d --build`; run tests with `NNODES=3 python -m pytest -q .` | **no**, wiped |

The init scripts run only on an empty data volume, which is why a topology change needs `down -v`. `pytest` truncates `users`; rerun `bash bench/load_users.sh` to get the 1M rows back.

---

## Quick run (everything in one go)

[docker/playbook.sql](../docker/playbook.sql) does the setup: it (re)creates the unique GSI on `users.email`, loads 8 sample rows, prints where each row and index entry lives, and turns on statement logging on the shards (to see the 2PC commands). Example queries for live testing (read path, UPDATE/DELETE, 2PC) are in a comment at the end of the file.

```
cd docker
docker compose down -v
docker compose up -d --build
docker compose exec -T coordinator psql -U postgres -X -f - < playbook.sql      # setup + sample rows, ~1 min
docker compose pause shard2                                                     # shard hang (needs docker)
docker compose exec -T coordinator psql -U postgres -c "SET statement_timeout='5s'" -c "INSERT INTO users VALUES (7,'pause@x','X')"
docker compose unpause shard2
cd ../tests && python checker.py && python -m pytest -v .                     # §2
```
PowerShell has no `<` redirection, so there pipe the file in instead: `Get-Content playbook.sql | docker compose exec -T coordinator psql -U postgres -X -f -`.

Each feature, step by step, with what to expect: [demo.md](demo.md).

---

## 0. Prerequisites (once)

| Need | Check |
|---|---|
| Docker Desktop **running** | `docker info` prints a server version |
| Python 3 + packages | `pip install psycopg2-binary pytest` |
| Free ports 5432–5437 | stop any local PostgreSQL first |

```
cd C:\Users\Kunj\Desktop\cs631\Distributed-GSI-Maintenance\docker
```

---

## 1. Build and start the cluster

```
docker compose down -v                # wipe any old cluster (init SQL only runs on empty volumes)
docker compose up -d --build          # builds the image (compiles gsi_fdw), starts 6 containers
docker compose ps
```
**Expect:** six containers, `coordinator` and `shard1` ... `shard5`, all `(healthy)`. The build prints gcc lines with no `warning:`.

Optional, to see the compile: `docker compose build --no-cache --progress=plain 2>&1 | findstr gcc` (PowerShell) or `| grep gcc` (bash).

---

## 2. Consistency checker, test suite, stress

From the **repo root**, in a new terminal:
```
cd tests
python checker.py
```
**Expect:** `rows=N missing=0 orphaned=0 misplaced=0 prepared=0`. This compares every base row with every index entry, checks each entry is on its hash node, and checks no 2PC is left over.

Full automated suite. **It resets the cluster data** (truncates `users`, re-creates the GSI):
```
python -m pytest -v .
```
**Expect:** all passed in about 2-3 min. To see the stress numbers:
```
python -m pytest -s test_stress.py
```
**Expect:** `{'commits': ~9000+, 'aborts': ...}` for each run, and passing. The aborts are expected: retryable conflicts and unique violations.

Individual milestones: `python -m pytest -v test_m8_read.py`, `test_m7_worker.py`, and so on.

---

## 3. Clean up / reset

```
docker compose down          # stop, keep data
docker compose down -v       # stop and wipe everything (next "up" re-runs the init SQL)
```
To undo the demo settings without wiping, see the end of [demo.md](demo.md).

---

## 4. Troubleshooting

| Symptom | Cause / fix |
|---|---|
| `error during connect ... docker_engine` | Docker Desktop isn't running; start it and wait for "Engine running" |
| port 5432 already in use | a local PostgreSQL is running; stop it, or change the host ports in `docker-compose.yml` |
| `relation "users" does not exist` | the volume was created by an older run without init; `docker compose down -v` then `up -d` |
| `column "email" of table "users" already has a GSI` | that column's GSI exists; `SELECT gsi_drop('users', 'email');` first (`gsi_drop('users')` drops all GSIs of the table) |
| `gsi_create cannot run inside a transaction block` | `CALL gsi_create(...)` on its own, not inside `BEGIN ... COMMIT` (it commits between its steps) |
| `gsi_create` interrupted (cancel, crash) | the GSI stays `write-only`; `CALL gsi_create(...)` again resumes it, or `SELECT gsi_drop('users', 'col')` |
| `function gsi_create(...) does not exist` | old cluster from before phase 2: `docker compose down -v`, `docker compose up -d --build` |
| statements hang | a prepared transaction from a crash holds locks: on **C**, `SELECT gsi_resolve(0);` |
| `could not serialize access ... GSI` (40001) | expected under concurrency (stale remote snapshot); retry the transaction |
| `canceling statement due to lock timeout` | cross-node lock wait hit `gsi.remote_lock_timeout` (5 s); retry |
| what happened? | `docker compose logs --tail 50 coordinator` (errors show `remote SQL command: ...`) |
| checker shows prepared > 0 | wait ~15 s for the worker, or run `SELECT gsi_resolve(0);` |
