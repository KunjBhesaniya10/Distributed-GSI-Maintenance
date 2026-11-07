"""Phase 2: cost model, online build (CALL gsi_create), async GSIs, 2PC trace."""
import random
import threading
import time

import psycopg2
import pytest

from checker import assert_consistent, wait_drained
from conftest import COORD, NNODES, connect, q, reset_cluster


def plan(c, sql):
    return "\n".join(r[0] for r in q(c, "EXPLAIN " + sql))


# --- cost model -------------------------------------------------------------

@pytest.fixture
def skewed(coord, shards):
    """Unique GSI on email, GSI on name; 4,000 rows: names n0/n1 1,000 rows
    each (in ANALYZE's most-common values), m<id> one row each."""
    reset_cluster(coord, shards, unique=True)
    q(coord, "CALL gsi_create('users', 'name')")
    q(coord, "INSERT INTO users SELECT g, 'u' || g || '@x', "
             "CASE WHEN g <= 2000 THEN 'n' || (g % 2) ELSE 'm' || g END "
             "FROM generate_series(1, 4000) g")
    q(coord, "ANALYZE users")
    return coord


def test_selective_key_uses_gsi(skewed):
    p = plan(skewed, "SELECT * FROM users WHERE email = 'u7@x'")
    assert "GsiLookup" in p and "Estimated Entries: 1" in p and "Estimated Shards: 1.0" in p, p


def test_unselective_key_broadcasts(skewed):
    # ~1,000 rows per name: a PK probe per row costs more than scanning 5 small shards
    p = plan(skewed, "SELECT * FROM users WHERE name = 'n1'")
    assert "GsiLookup" not in p and "Append" in p, p


def test_estimates_follow_statistics(skewed):
    # same column, different values: the planner's choice follows the stats
    p = plan(skewed, "SELECT * FROM users WHERE name = 'm2500'")
    assert "GsiLookup" in p and "GSI: users_name" in p and "Estimated Entries: 1" in p, p
    assert "GsiLookup" not in plan(skewed, "SELECT * FROM users WHERE name = 'n0'")


def test_enable_lookup_off_still_broadcasts(skewed):
    q(skewed, "SET gsi.enable_lookup = off")
    try:
        assert "GsiLookup" not in plan(skewed, "SELECT * FROM users WHERE email = 'u7@x'")
    finally:
        q(skewed, "RESET gsi.enable_lookup")


# --- online build -------------------------------------------------------------

def writer(stop, errors, first_id):
    """Random inserts, key updates and deletes until stop is set."""
    c = connect(COORD)
    i = first_id
    try:
        while not stop.is_set():
            op = random.random()
            try:
                if op < 0.5:
                    q(c, "INSERT INTO users VALUES (%s, %s, %s)", (i, f"w{i}@x", f"w{i % 7}"))
                    i += 1
                elif op < 0.8:
                    q(c, "UPDATE users SET email = 'up' || id || '@x', name = 'up' "
                         "WHERE id = %s", (random.randint(1, i),))
                else:
                    q(c, "DELETE FROM users WHERE id = %s", (random.randint(1, i),))
            except (psycopg2.errors.SerializationFailure, psycopg2.errors.LockNotAvailable,
                    psycopg2.errors.DeadlockDetected):
                pass                                         # retryable, expected under load
    except Exception as e:                                   # noqa: BLE001 - report in main thread
        errors.append(e)
    finally:
        c.close()


@pytest.mark.parametrize("unique", [False, True])
def test_build_under_concurrent_writes(coord, shards, unique):
    reset_cluster(coord, shards)
    q(coord, "SELECT gsi_drop('users')")
    q(coord, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 9) "
             "FROM generate_series(1, 3000) g")
    stop, errors = threading.Event(), []
    t = threading.Thread(target=writer, args=(stop, errors, 100000))
    t.start()
    try:
        q(coord, "SET gsi.build_chunk_size = 50")
        q(coord, "CALL gsi_create('users', 'name', false)")
        q(coord, "CALL gsi_create('users', 'email', %s)", (unique,))
    finally:
        stop.set()
        t.join()
    assert not errors, errors
    assert q(coord, "SELECT count(*) FROM gsi.gsi_index WHERE status = 'ready'") == [(2,)]
    assert q(coord, "SELECT count(*) FROM gsi.gsi_build_progress") == [(0,)]
    assert assert_consistent(coord, shards) > 3000


def test_write_only_index_is_not_read_and_build_resumes(coord, shards):
    reset_cluster(coord, shards)
    q(coord, "SELECT gsi_drop('users')")
    q(coord, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 500) g")
    builder = connect(COORD)
    pid = q(builder, "SELECT pg_backend_pid()")[0][0]
    q(builder, "SET gsi.build_chunk_size = 10")
    q(builder, "SET gsi.build_chunk_delay = '50ms'")
    result = []
    t = threading.Thread(target=lambda: result.append(_call_build(builder)))
    t.start()

    # while building: write-only, maintained by writes, not used by the planner
    deadline = time.time() + 20
    while q(coord, "SELECT count(*) FROM gsi.gsi_build_progress") == [(0,)]:
        assert time.time() < deadline
        time.sleep(0.05)
    assert q(coord, "SELECT status FROM gsi.gsi_index") == [("write-only",)]
    assert "GsiLookup" not in plan(coord, "SELECT * FROM users WHERE email = 'u3@x'")
    q(coord, "INSERT INTO users VALUES (9001, 'during@x', 'n')")

    # interrupt the build, then resume it
    q(coord, "SELECT pg_cancel_backend(%s)", (pid,))
    t.join()
    builder.close()
    assert isinstance(result[0], psycopg2.errors.QueryCanceled), result
    assert q(coord, "SELECT status FROM gsi.gsi_index") == [("write-only",)]
    q(coord, "CALL gsi_create('users', 'email')")
    assert q(coord, "SELECT status FROM gsi.gsi_index") == [("ready",)]
    assert "GsiLookup" in plan(coord, "SELECT * FROM users WHERE email = 'during@x'")
    assert assert_consistent(coord, shards) == 501


def test_cached_plan_is_replanned_after_build(coord, shards):
    # a generic plan made before the GSI existed must not skip it afterwards
    reset_cluster(coord, shards)
    q(coord, "SELECT gsi_drop('users')")
    q(coord, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 10) g")
    q(coord, "SET plan_cache_mode = force_generic_plan")
    q(coord, "PREPARE up(bigint) AS UPDATE users SET name = 'p' || id WHERE id = $1")
    try:
        q(coord, "EXECUTE up(1)")
        q(coord, "CALL gsi_create('users', 'name')")
        q(coord, "EXECUTE up(2)")
    finally:
        q(coord, "DEALLOCATE up")
        q(coord, "RESET plan_cache_mode")
    assert assert_consistent(coord, shards) == 10
    assert q(coord, "SELECT id FROM users WHERE name = 'p2'") == [(2,)]


def _call_build(c):
    try:
        q(c, "CALL gsi_create('users', 'email')")
        return None
    except psycopg2.Error as e:
        return e


# --- async GSIs -------------------------------------------------------------

@pytest.fixture
def agsi(coord, shards):
    """Sync unique GSI on email, async GSI on name."""
    reset_cluster(coord, shards, unique=True)
    q(coord, "CALL gsi_create('users', 'name', false, 'async')")
    return coord


def test_async_catalog_and_lag(agsi):
    assert q(agsi, "SELECT index_name, mode FROM gsi.gsi_index ORDER BY 1") == \
        [("users_email", "sync"), ("users_name", "async")]
    assert len(q(agsi, "SELECT * FROM gsi_lag()")) == NNODES


def test_unique_async_rejected(agsi):
    with pytest.raises(psycopg2.errors.FeatureNotSupported):
        q(agsi, "CALL gsi_create('users', 'email', true, 'async')")


def test_async_writes_reach_the_index(agsi, shards):
    q(agsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 3) "
            "FROM generate_series(1, 300) g")
    q(agsi, "UPDATE users SET name = 'moved' WHERE id <= 30")
    q(agsi, "DELETE FROM users WHERE id BETWEEN 31 AND 40")
    wait_drained(agsi)
    assert assert_consistent(agsi, shards) == 290
    assert q(agsi, "SELECT count(*) FROM users WHERE name = 'moved'") == [(30,)]


def test_async_only_write_is_eventually_visible(coord, shards):
    reset_cluster(coord, shards)
    q(coord, "SELECT gsi_drop('users')")
    q(coord, "CALL gsi_create('users', 'name', false, 'async')")
    q(coord, "INSERT INTO users VALUES (1, 'a@x', 'late')")
    deadline = time.time() + 10
    while q(coord, "SELECT count(*) FROM users WHERE name = 'late'") != [(1,)]:
        assert time.time() < deadline, "async entry never applied"
        time.sleep(0.05)
    assert assert_consistent(coord, shards) == 1


def test_async_build_on_full_table(coord, shards):
    reset_cluster(coord, shards)
    q(coord, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 5) "
             "FROM generate_series(1, 1000) g")
    q(coord, "CALL gsi_create('users', 'name', false, 'async')")
    assert assert_consistent(coord, shards) == 1000


# --- demo trace ---------------------------------------------------------------

def test_trace_reports_2pc_steps(gsi):
    gsi.notices.clear()
    q(gsi, "SET gsi.trace = on")
    q(gsi, "INSERT INTO users VALUES (1, 'trace@x', 't')")
    q(gsi, "RESET gsi.trace")
    text = "".join(gsi.notices)
    for step in ("participants:", "PREPARE TRANSACTION", "is the decision: commit",
                 "COMMIT PREPARED"):
        assert step in text, text


# --- parallel hop 2 -----------------------------------------------------------

@pytest.fixture
def spread(gsi):
    """60 rows with the same email, so one lookup touches every shard."""
    q(gsi, "INSERT INTO users SELECT g, 'dup@x', 'n' || g FROM generate_series(1, 60) g")
    q(gsi, "SET gsi.force_lookup = on")
    return gsi


def test_parallel_hop2_reads_every_shard(spread):
    p = "\n".join(r[0] for r in q(spread, "EXPLAIN ANALYZE SELECT * FROM users WHERE email = 'dup@x'"))
    assert "GsiLookup" in p and f"Shards Fetched: {NNODES}" in p, p
    assert q(spread, "SELECT array_agg(id ORDER BY id) FROM users WHERE email = 'dup@x'") == \
        [(list(range(1, 61)),)]


def test_parallel_hop2_error_leaves_session_usable(spread, shards):
    """One shard blocks while the others answer; the statement is cancelled
    with queries in flight, and the same session's next lookup is correct."""
    blocker = connect(dict(COORD, port=5433), autocommit=False)
    try:
        q(blocker, "LOCK TABLE users_shard IN ACCESS EXCLUSIVE MODE")
        q(spread, "SET statement_timeout = '1s'")
        with pytest.raises(psycopg2.errors.QueryCanceled):
            q(spread, "SELECT count(*) FROM users WHERE email = 'dup@x'")
    finally:
        blocker.rollback()
        blocker.close()
    q(spread, "RESET statement_timeout")
    assert q(spread, "SELECT count(*) FROM users WHERE email = 'dup@x'") == [(60,)]
    # nothing left running on any shard for this session
    for s in shards:
        assert q(s, "SELECT count(*) FROM pg_stat_activity WHERE state = 'active' AND pid <> pg_backend_pid() "
                    "AND query LIKE 'SELECT %users_shard%ANY%'") == [(0,)]


# --- coordinator xid only for 2PC -----------------------------------------------

def test_async_only_write_assigns_no_coordinator_xid(coord, shards):
    reset_cluster(coord, shards)
    q(coord, "SELECT gsi_drop('users')")
    q(coord, "CALL gsi_create('users', 'name', false, 'async')")
    q(coord, "BEGIN")
    q(coord, "INSERT INTO users VALUES (1, 'a@x', 'one')")
    assert q(coord, "SELECT pg_current_xact_id_if_assigned()") == [(None,)]
    q(coord, "COMMIT")
    wait_drained(coord)
    assert assert_consistent(coord, shards) == 1
