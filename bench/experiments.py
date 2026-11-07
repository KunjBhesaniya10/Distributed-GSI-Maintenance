"""GSI benchmarks (docs/phase2_plan.md §5; results in docs/phase2_results.md).
Run from the repo root with the cluster up; results go to
bench/results/<experiment>_<...>.csv and stdout.

  python bench/experiments.py lookups   [--secs 10] [--clients 4]  E1: GSI speedup vs selectivity + planner choice
                                                                     (needs NAMES=sweep bash bench/load_users.sh)
  python bench/experiments.py writes    [--secs 15] [--clients 8]  E2: write cost, none / sync / async
  python bench/experiments.py hot_keys  [--secs 15] [--clients 8]  E4: hot keys (Zipf) under sync writes
  python bench/experiments.py async_lag [--secs 20]                E5: async lag and missed reads
  python bench/experiments.py build                                E6: online build time and impact
  python bench/experiments.py tpcc      [--secs 15] [--clients 8]  E7: TPC-C-derived customer workload
E3 (node counts) is bench/node_sweep.sh, which reruns `lookups` for NNODES=2,3,5.

--tag adds a suffix to the result file names, e.g. `lookups --tag 1ms` writes
lookups_5nodes_1ms.csv (the script can't tell which network delay is set).

Latency is measured per statement on the client (this host).  Each client
runs its own connection in a thread; Python's overhead (~0.1 ms) is the same
for every plan compared.
"""
import argparse
import csv
import os
import random
import statistics
import sys
import threading
import time

import psycopg2

sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
from conftest import COORD, NNODES, SHARDS, connect, q  # noqa: E402

RESULTS = os.path.join(os.path.dirname(__file__), "results")
RETRYABLE = (psycopg2.errors.SerializationFailure, psycopg2.errors.LockNotAvailable,
             psycopg2.errors.DeadlockDetected)


# --- helpers -----------------------------------------------------------------

def pct(xs, p):
    xs = sorted(xs)
    return xs[min(len(xs) - 1, int(p / 100 * len(xs)))] if xs else float("nan")


def run_clients(n, secs, make_step, setup=None):
    """Run n client threads for secs; make_step(conn, i) -> step() runs one
    operation.  Returns (latencies_ms, retryable_errors, other_errors)."""
    lat, errs, other = [], [0], []
    lock = threading.Lock()
    stop = time.time() + secs

    def client(i):
        c = connect(COORD)
        if setup:
            setup(c)
        step = make_step(c, i)
        mine, e = [], 0
        while time.time() < stop:
            t = time.perf_counter()
            try:
                step()
                mine.append((time.perf_counter() - t) * 1000)
            except RETRYABLE:
                e += 1
            except psycopg2.Error as ex:            # unique violations etc.: count, go on
                with lock:
                    other.append(type(ex).__name__)
        with lock:
            lat.extend(mine)
            errs[0] += e
        c.close()

    ts = [threading.Thread(target=client, args=(i,)) for i in range(n)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
    return lat, errs[0], other


def summary(lat, secs):
    return dict(tps=round(len(lat) / secs, 1), avg_ms=round(statistics.mean(lat), 3) if lat else None,
                p50_ms=round(pct(lat, 50), 3), p95_ms=round(pct(lat, 95), 3),
                p99_ms=round(pct(lat, 99), 3))


TAG = ""   # --tag: suffix for result file names


def save(name, rows):
    if TAG:
        name = f"{name}_{TAG}"
    os.makedirs(RESULTS, exist_ok=True)
    path = os.path.join(RESULTS, f"{name}.csv")
    with open(path, "w", newline="") as f:
        w = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        w.writeheader()
        w.writerows(rows)
    print(f"-> {path}")
    for r in rows:
        print("  " + "  ".join(f"{k}={v}" for k, v in r.items()))


def shard_conns():
    return [connect(s) for s in SHARDS[:NNODES]]


def set_local_index(on):
    for s in shard_conns():
        if on:
            q(s, "CREATE INDEX IF NOT EXISTS users_shard_email ON users_shard (email)")
            q(s, "CREATE INDEX IF NOT EXISTS users_shard_name ON users_shard (name)")
        else:
            q(s, "DROP INDEX IF EXISTS users_shard_email, users_shard_name")
        q(s, "ANALYZE users_shard")
        s.close()


def reset_users(c, gsis):
    """Empty users; create the given GSIs [(col, unique, mode)]."""
    if q(c, "SELECT 1 FROM gsi.gsi_index WHERE base_table = 'users'::regclass"):
        q(c, "SELECT gsi_drop('users')")
    for s in shard_conns():
        q(s, "TRUNCATE users_shard, gsi_outbox")
        s.close()
    for col, unique, mode in gsis:
        q(c, "CALL gsi_create('users', %s, %s, %s)", (col, unique, mode))


def wait_drained(c, timeout=600):
    t = time.time()
    while q(c, "SELECT coalesce(sum(pending), 0) FROM gsi_lag()")[0][0] > 0:
        if time.time() - t > timeout:
            raise RuntimeError("outbox did not drain")
        time.sleep(0.1)
    return time.time() - t


# --- E1: speedup vs selectivity, and does the planner pick the faster plan? -----

def lookups(a):
    c = connect(COORD)
    nrows = q(c, "SELECT count(*) FROM users")[0][0]
    if not q(c, "SELECT 1 FROM users WHERE name = 'k1_1'"):
        sys.exit("e1 needs: NAMES=sweep bash bench/load_users.sh")
    groups = {  # K -> (name prefix, first group number, last group number)
        1: ("k1_", 1, nrows // 10),
        10: ("k10_", nrows // 10 // 10 + 1, nrows * 3 // 10 // 10 - 1),
        100: ("k100_", nrows * 3 // 10 // 100 + 1, nrows * 6 // 10 // 100 - 1),
        1000: ("k1000_", nrows * 6 // 10 // 1000 + 1, nrows // 1000 - 1),
    }
    rows = []
    for local in (False, True):
        set_local_index(local)
        q(c, "ANALYZE users")
        keys = [("email", "SELECT * FROM users WHERE email = $1", _email_value(nrows))] +             [(k, "SELECT * FROM users WHERE name = $1",
              (lambda p=prefix, l=lo, h=hi: f"{p}{random.randint(l, h)}"))
             for k, (prefix, lo, hi) in groups.items()]
        for k, sql, val in keys:
            # planner's own choice for 20 sample values (custom plans, like an ad hoc query)
            picks, est = [], []
            for _ in range(20):
                v = val()
                p = "\n".join(r[0] for r in q(c, "EXPLAIN " + sql.replace("$1", "%s"), (v,)))
                picks.append("gsi" if "GsiLookup" in p else "broadcast")
                # the GSI path's estimate, also when the planner rejected it
                q(c, "SET gsi.force_lookup = on")
                p = "\n".join(r[0] for r in q(c, "EXPLAIN " + sql.replace("$1", "%s"), (v,)))
                q(c, "RESET gsi.force_lookup")
                est.append(float(p.split("Estimated Entries: ")[1].split()[0]))
            choice = max(set(picks), key=picks.count)
            res = {}
            for mode, opts in (("gsi", "SET gsi.force_lookup = on"),
                               ("broadcast", "SET gsi.enable_lookup = off")):
                def setup(conn, opts=opts):
                    q(conn, opts)
                    q(conn, "SET plan_cache_mode = force_generic_plan")   # time execution, not planning
                    q(conn, f"PREPARE l(text) AS {sql}")

                lat, _, _ = run_clients(a.clients, a.secs,
                                        lambda conn, i: (lambda: q(conn, "EXECUTE l(%s)", (val(),))),
                                        setup)
                res[mode] = summary(lat, a.secs)
            faster = "gsi" if res["gsi"]["avg_ms"] < res["broadcast"]["avg_ms"] else "broadcast"
            rows.append(dict(shard_index=local, key=k, est_entries=round(statistics.mean(est), 1) if est else "",
                             gsi_ms=res["gsi"]["avg_ms"], gsi_p99=res["gsi"]["p99_ms"],
                             bcast_ms=res["broadcast"]["avg_ms"], bcast_p99=res["broadcast"]["p99_ms"],
                             speedup=round(res["broadcast"]["avg_ms"] / res["gsi"]["avg_ms"], 2),
                             planner=choice, faster=faster, correct=choice == faster))
    set_local_index(False)
    save(f"lookups_{NNODES}nodes", rows)
    planning(c)


def _email_value(nrows):
    doms = ["example.com", "mail.org", "iitb.ac.in", "corp.net", "post.io"]

    def v():
        i = random.randint(1, nrows)
        return f"user{i}@{doms[i % 5]}"
    return v


def planning(c, n=50):
    """Planning time of one lookup with use_remote_estimate on and off."""
    rows = []
    for on in ("true", "false"):
        for i in range(1, NNODES + 1):
            q(c, f"ALTER SERVER shard{i} OPTIONS (SET use_remote_estimate '{on}')")
        times = []
        for j in range(n):
            out = q(c, "EXPLAIN (ANALYZE, TIMING OFF) SELECT * FROM users WHERE name = %s",
                    (f"k10_{20000 + j}",))
            times += [float(r[0].split(":")[1].split()[0]) for r in out if r[0].startswith("Planning Time")]
        rows.append(dict(use_remote_estimate=on, planning_ms_avg=round(statistics.mean(times), 3),
                         planning_ms_p99=round(pct(times, 99), 3)))
    for i in range(1, NNODES + 1):
        q(c, f"ALTER SERVER shard{i} OPTIONS (SET use_remote_estimate 'true')")
    save(f"planning_{NNODES}nodes", rows)


# --- E2: write cost ---------------------------------------------------------------

WRITE_CONFIGS = [
    ("none", []),
    ("1 sync", [("email", False, "sync")]),
    ("2 sync", [("email", False, "sync"), ("name", False, "sync")]),
    ("1 async", [("email", False, "async")]),
    ("2 async", [("email", False, "async"), ("name", False, "async")]),
]


def writes(a):
    c = connect(COORD)
    rows = []
    for label, gsis in WRITE_CONFIGS:
        reset_users(c, gsis)
        ids = iter(range(1, 10 ** 9))
        lock = threading.Lock()

        def nxt():
            with lock:
                return next(ids)

        def insert(conn, i):
            q(conn, "PREPARE w(bigint) AS INSERT INTO users VALUES ($1, 'e' || $1 || '@x', 'n' || $1 % 1000)")
            return lambda: q(conn, "EXECUTE w(%s)", (nxt(),))

        lat, retry, other = run_clients(a.clients, a.secs, insert)
        rows.append(dict(config=label, op="insert", **summary(lat, a.secs), retries=retry, errors=len(other)))
        n = len(lat)

        def update(conn, i):
            q(conn, "PREPARE u(bigint) AS UPDATE users SET email = 'u' || $1 || '_' || "
                    "floor(random() * 1e6) || '@x' WHERE id = $1")
            return lambda: q(conn, "EXECUTE u(%s)", (random.randint(1, max(1, n)),))

        lat, retry, other = run_clients(a.clients, a.secs, update)
        drain = wait_drained(c) if "async" in label else 0
        rows.append(dict(config=label, op="key update", **summary(lat, a.secs), retries=retry,
                         errors=len(other)))
        rows[-1]["drain_s"] = rows[-2]["drain_s"] = round(drain, 2)
        check(c)
    save("writes", rows)


def check(c):
    """Every GSI exact after the run (tests/checker.py logic)."""
    sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tests"))
    from checker import check_all
    ss = shard_conns()
    for name, r in check_all(c, ss).items():
        bad = {k: len(v) for k, v in r.items() if k != "rows" and v}
        print(f"  checker {name}: rows={r['rows']} {'OK' if not bad else bad}")
    for s in ss:
        s.close()


# --- E4: hot keys -------------------------------------------------------------------

def hot_keys(a):
    c = connect(COORD)
    reset_users(c, [("email", True, "sync"), ("name", False, "sync")])
    q(c, "INSERT INTO users SELECT g, 'h' || g || '@x', 'n' || g % 100 FROM generate_series(1, 10000) g")
    q(c, "ANALYZE users")
    rows = []
    for zipf in (0.0, 0.99):
        weights = [1 / (i ** zipf) for i in range(1, 10001)]

        def mixed(conn, i):
            q(conn, "PREPARE r(text) AS SELECT * FROM users WHERE email = $1")
            q(conn, "PREPARE u(bigint, text) AS UPDATE users SET email = $2 WHERE id = $1")

            def step():
                rid = random.choices(range(1, 10001), weights)[0]
                if random.random() < 0.5:
                    q(conn, "EXECUTE r(%s)", (f"h{rid}@x",))
                else:      # move the key away and back: two GSI changes, same final state
                    q(conn, "EXECUTE u(%s, %s)", (rid, f"tmp{rid}_{i}@x"))
                    q(conn, "EXECUTE u(%s, %s)", (rid, f"h{rid}@x"))
            return step

        lat, retry, other = run_clients(a.clients, a.secs, mixed)
        rows.append(dict(zipf=zipf, **summary(lat, a.secs), retryable=retry, other_errors=len(other),
                         abort_pct=round(100 * (retry + len(other)) / max(1, len(lat) + retry + len(other)), 2)))
    check(c)
    save("hot_keys", rows)


# --- E5: async lag and missed reads -------------------------------------------------

def async_lag(a):
    c = connect(COORD)
    reset_users(c, [("email", False, "async")])
    rows = []
    for writers in (1, 4, 8, 16):
        lags, stop = [], threading.Event()

        def sample():
            s = connect(COORD)
            while not stop.is_set():
                lags.append(max(r[0] for r in q(s, "SELECT oldest_secs FROM gsi_lag()")) * 1000)
                time.sleep(0.2)
            s.close()

        ids = iter(range(writers * 10 ** 7, 10 ** 12))
        lock = threading.Lock()

        def nxt():
            with lock:
                return next(ids)

        def writer(conn, i):
            return lambda: q(conn, "INSERT INTO users VALUES (%s, 'a' || %s || '@x', 'x')", (nxt(), i))

        probe_miss, probe_delay = [0], []

        def probe():
            """Insert a row, look it up through the GSI right away, then until it shows up."""
            p = connect(COORD)
            q(p, "SET gsi.force_lookup = on")
            while not stop.is_set():
                i = nxt()
                q(p, "INSERT INTO users VALUES (%s, %s, 'probe')", (i, f"probe{i}@x"))
                t = time.perf_counter()
                if not q(p, "SELECT 1 FROM users WHERE email = %s", (f"probe{i}@x",)):
                    probe_miss[0] += 1
                    while not q(p, "SELECT 1 FROM users WHERE email = %s", (f"probe{i}@x",)):
                        time.sleep(0.002)
                probe_delay.append((time.perf_counter() - t) * 1000)
                time.sleep(0.05)
            p.close()

        ts = [threading.Thread(target=sample), threading.Thread(target=probe)]
        for t in ts:
            t.start()
        lat, _, _ = run_clients(writers, a.secs, writer)
        stop.set()
        for t in ts:
            t.join()
        rows.append(dict(writers=writers, write_tps=round(len(lat) / a.secs, 1),
                         lag_p50_ms=round(pct(lags, 50), 1), lag_p99_ms=round(pct(lags, 99), 1),
                         reads_missed_pct=round(100 * probe_miss[0] / max(1, len(probe_delay)), 1),
                         visible_after_p50_ms=round(pct(probe_delay, 50), 1),
                         visible_after_p99_ms=round(pct(probe_delay, 99), 1),
                         drain_s=round(wait_drained(c), 2)))
    check(c)
    save("async_lag", rows)


# --- E6: online build -----------------------------------------------------------------

def build(a):
    c = connect(COORD)
    nrows = q(c, "SELECT count(*) FROM users")[0][0]
    if nrows < 100000:
        sys.exit("e6 needs data: bash bench/load_users.sh")
    rows = []
    for chunk, writes in ((1000, False), (1000, True), (10000, True), (50000, True)):
        if q(c, "SELECT 1 FROM gsi.gsi_index WHERE column_name = 'name'"):
            q(c, "SELECT gsi_drop('users', 'name')")
        stop, per_sec = threading.Event(), {}
        phase = ["before"]

        def writer():
            w = connect(COORD)
            q(w, "PREPARE w(bigint) AS UPDATE users SET email = 'b' || $1 || '_' || "
                 "floor(random() * 1e6) || '@x' WHERE id = $1")
            while not stop.is_set():
                t = time.perf_counter()
                try:
                    q(w, "EXECUTE w(%s)", (random.randint(1, nrows),))
                except RETRYABLE:
                    continue
                per_sec.setdefault(phase[0], []).append((time.perf_counter() - t) * 1000)
            w.close()

        ws = [threading.Thread(target=writer) for _ in range(4 if writes else 0)]
        for t in ws:
            t.start()
        time.sleep(5 if writes else 0)
        phase[0] = "during"
        t0 = time.time()
        q(c, f"SET gsi.build_chunk_size = {chunk}")
        q(c, "CALL gsi_create('users', 'name')")
        build_s = time.time() - t0
        phase[0] = "after"
        time.sleep(5 if writes else 0)
        stop.set()
        for t in ws:
            t.join()
        row = dict(chunk_size=chunk, concurrent_writes=writes, build_s=round(build_s, 1))
        for ph in ("before", "during", "after"):
            lat = per_sec.get(ph, [])
            dur = 5 if ph != "during" else build_s
            row[f"{ph}_tps"] = round(len(lat) / dur, 1) if writes else ""
            row[f"{ph}_p99_ms"] = round(pct(lat, 99), 2) if lat else ""
        rows.append(row)
        check(c)
    save("build", rows)


# --- E7: TPC-C-derived customer workload -----------------------------------------------

LAST = ["BAR", "OUGHT", "ABLE", "PRI", "PRES", "ESE", "ANTI", "CALLY", "ATION", "EING"]


def c_last(n):
    return LAST[n // 100] + LAST[(n // 10) % 10] + LAST[n % 10]


def nurand(a, x, y, c=123):
    return (((random.randint(0, a) | random.randint(x, y)) + c) % (y - x + 1)) + x


def tpcc_setup(c, warehouses):
    """customer(id, w, d, c_last, c_key, c_first, c_balance), hash-partitioned by id like
    users; c_key = 'w:d:c_last' so one GSI serves TPC-C's (w, d, c_last) lookup."""
    for i, s in enumerate(shard_conns(), 1):
        q(s, "DROP TABLE IF EXISTS customer_shard")
        q(s, "CREATE TABLE customer_shard (id bigint PRIMARY KEY, c_w_id int, c_d_id int, "
             "c_last text, c_key text, c_first text, c_balance numeric)")
        s.close()
    q(c, "DROP TABLE IF EXISTS customer CASCADE")
    q(c, "CREATE TABLE customer (id bigint NOT NULL, c_w_id int, c_d_id int, c_last text, "
         "c_key text, c_first text, c_balance numeric) PARTITION BY HASH (id)")
    for i in range(NNODES):
        q(c, f"CREATE FOREIGN TABLE customer_p{i} PARTITION OF customer "
             f"FOR VALUES WITH (MODULUS {NNODES}, REMAINDER {i}) "
             f"SERVER shard{i + 1} OPTIONS (table_name 'customer_shard')")
    # TPC-C: 10 districts x 3,000 customers per warehouse; c_id <= 1000 gets
    # last name number c_id - 1, the rest NURand(255, 0, 999)
    q(c, "CREATE OR REPLACE FUNCTION tpcc_last(n int) RETURNS text IMMUTABLE LANGUAGE sql AS $$ "
         "SELECT (ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[n / 100 + 1] || "
         "(ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[(n / 10) % 10 + 1] || "
         "(ARRAY['BAR','OUGHT','ABLE','PRI','PRES','ESE','ANTI','CALLY','ATION','EING'])[n % 10 + 1] $$")
    q(c, f"""INSERT INTO customer
             SELECT g, w, d, l, w || ':' || d || ':' || l, 'F' || g, 0
             FROM (SELECT g, (g - 1) / 30000 + 1 AS w, ((g - 1) / 3000) % 10 + 1 AS d,
                          tpcc_last(CASE WHEN (g - 1) % 3000 < 1000 THEN (g - 1) % 3000
                                         ELSE ((((random() * 255)::int | (random() * 999)::int) + 123) % 1000)
                                    END) AS l
                   FROM generate_series(1, {warehouses * 30000}) g) s""")
    q(c, "ANALYZE customer")


def tpcc(a):
    c = connect(COORD)
    tpcc_setup(c, a.warehouses)
    rows = []
    for label, mode in (("broadcast (no GSI)", None), ("sync GSI", "sync"), ("async GSI", "async")):
        if q(c, "SELECT 1 FROM gsi.gsi_index WHERE base_table = 'customer'::regclass"):
            q(c, "SELECT gsi_drop('customer')")
        if mode:
            q(c, "CALL gsi_create('customer', 'c_key', false, %s)", (mode,))

        def txn(conn, i):
            q(conn, "PREPARE f(text) AS SELECT id, c_first FROM customer WHERE c_key = $1 ORDER BY c_first")
            q(conn, "PREPARE p(bigint, numeric) AS UPDATE customer SET c_balance = c_balance - $2 WHERE id = $1")

            def step():
                key = f"{random.randint(1, a.warehouses)}:{random.randint(1, 10)}:{c_last(nurand(255, 0, 999))}"
                found = q(conn, "EXECUTE f(%s)", (key,))
                if found and random.random() < 0.5:      # Payment by name; else Order-Status by name
                    q(conn, "EXECUTE p(%s, %s)", (found[len(found) // 2][0], random.randint(1, 5000)))
            return step

        lat, retry, other = run_clients(a.clients, a.secs, txn)
        rows.append(dict(config=label, **summary(lat, a.secs), retries=retry, errors=len(other)))
    save(f"tpcc_{NNODES}nodes", rows)


EXPERIMENTS = {"lookups": lookups, "writes": writes, "hot_keys": hot_keys,
               "async_lag": async_lag, "build": build, "tpcc": tpcc}

if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("experiment", choices=list(EXPERIMENTS))
    ap.add_argument("--secs", type=float, default=10)
    ap.add_argument("--clients", type=int, default=4)
    ap.add_argument("--warehouses", type=int, default=10)
    ap.add_argument("--tag", default="", help="suffix for result file names, e.g. 1ms")
    args = ap.parse_args()
    TAG = args.tag
    EXPERIMENTS[args.experiment](args)
