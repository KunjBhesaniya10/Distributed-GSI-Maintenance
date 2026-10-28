"""M6: a coordinator crash at any commit step is repaired by gsi_resolve().

Each test inserts one row whose base node differs from its index node, so
the transaction has one index participant ('_i') and one base-only
participant ('_b'), then kills the backend at a gsi.fail_point.
"""
import time

import psycopg2
import pytest

from checker import assert_consistent
from conftest import COORD, connect, q


def pick_row(c):
    """An (id, email, base_node, index_node) with base_node != index_node."""
    for i in range(1, 100):
        base = next(r for r in range(3)
                    if q(c, "SELECT satisfies_hash_partition('users'::regclass, 3, %s, %s::bigint)",
                         (r, i))[0][0])
        key = f"crash{i}@x"
        node = q(c, "SELECT gsi_node_for(%s::text)", (key,))[0][0]
        if node != base:
            return i, key, base, node
    raise AssertionError("no suitable row")


def crash_at(fail_point, row):
    c = connect(COORD, autocommit=False)
    q(c, "SET gsi.fail_point = %s", (fail_point,))
    pid = q(c, "SELECT pg_backend_pid()")[0][0]
    q(c, "INSERT INTO users VALUES (%s, %s, 'n')", row[:2])
    with pytest.raises(psycopg2.OperationalError):
        c.commit()
    # Reconnecting can beat the postmaster's crash handling; until it runs,
    # the dead backend still looks "in progress".  Wait for its pid to go.
    for _ in range(100):
        try:
            c = connect(COORD)
            if not q(c, "SELECT 1 FROM pg_stat_activity WHERE pid = %s", (pid,)):
                time.sleep(0.5)
                c = connect(COORD)
                q(c, "SELECT 1")
                return c
        except psycopg2.OperationalError:
            pass
        time.sleep(0.1)
    raise AssertionError("coordinator did not recover")


def prepared(shards):
    return sorted((n, g.rsplit("_", 1)[1]) for n, s in enumerate(shards)
                  for (g,) in q(s, "SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'gsi\\_%'"))


def has_base(shards, row):
    return bool(q(shards[row[2]], "SELECT 1 FROM users_shard WHERE id = %s", (row[0],)))


def has_entry(shards, row):
    return bool(q(shards[row[3]], "SELECT 1 FROM gsi_users_email WHERE id = %s", (row[0],)))


def test_crash_after_prepare_rolls_back(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("after_prepare", row)
    assert prepared(shards) == sorted([(row[2], "b"), (row[3], "i")])     # both in doubt
    assert q(c, "SELECT gsi_resolve(0)") == [(2,)]
    assert not has_base(shards, row) and not has_entry(shards, row)
    assert_consistent(c, shards)


def test_crash_after_local_commit_rolls_forward(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("after_local_commit", row)
    assert prepared(shards) == sorted([(row[2], "b"), (row[3], "i")])
    assert q(c, "SELECT gsi_resolve(0)") == [(2,)]
    assert has_base(shards, row) and has_entry(shards, row)
    assert_consistent(c, shards)


def test_crash_mid_commit_finishes_base(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("mid_commit_prepared", row)
    # index committed first: entry visible, base row still prepared (invisible)
    assert prepared(shards) == [(row[2], "b")]
    assert has_entry(shards, row) and not has_base(shards, row)
    assert q(c, "SELECT gsi_resolve(0)") == [(1,)]
    assert has_base(shards, row)
    assert_consistent(c, shards)


def test_grace_period_skips_recent(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("after_local_commit", row)
    assert q(c, "SELECT gsi_resolve(3600)") == [(0,)]
    assert len(prepared(shards)) == 2
    assert q(c, "SELECT gsi_resolve(0)") == [(2,)]
    assert_consistent(c, shards)


def test_resolve_is_idempotent(gsi, shards):
    q(gsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    assert q(gsi, "SELECT gsi_resolve(0)") == [(0,)]
    assert assert_consistent(gsi, shards) == 1
