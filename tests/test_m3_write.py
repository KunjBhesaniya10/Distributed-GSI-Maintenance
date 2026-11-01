"""M3: INSERT/UPDATE/DELETE keep the GSI exact."""
import random

import psycopg2
import pytest

from checker import assert_consistent
from conftest import q


def entries(shards):
    return sorted(r for s in shards for r in q(s, "SELECT key, id FROM gsi_users_email"))


def test_insert(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 50) g")
    assert assert_consistent(gsi, shards) == 50


def test_entry_placed_independently_of_base_row(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 50) g")
    base_node = {i: n for n, s in enumerate(shards) for (i,) in q(s, "SELECT id FROM users_shard")}
    index_node = {i: n for n, s in enumerate(shards) for (i,) in q(s, "SELECT id FROM gsi_users_email")}
    shard_col = {i: sh for s in shards for i, sh in q(s, "SELECT id, shard FROM gsi_users_email")}
    assert shard_col == base_node                      # entry points at the base row's node
    assert any(index_node[i] != base_node[i] for i in base_node)


def test_update_key(gsi, shards):
    q(gsi, "INSERT INTO users VALUES (1, 'old@x', 'a'), (2, 'b@x', 'b')")
    q(gsi, "UPDATE users SET email = 'new@x' WHERE id = 1")
    assert entries(shards) == [("b@x", 2), ("new@x", 1)]
    assert_consistent(gsi, shards)


def test_update_other_column_keeps_entry(gsi, shards):
    q(gsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    q(gsi, "UPDATE users SET name = 'z' WHERE id = 1")
    assert entries(shards) == [("a@x", 1)]


def test_update_by_non_key_predicate(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 2) FROM generate_series(1, 20) g")
    q(gsi, "UPDATE users SET email = email || '.new' WHERE name = 'n0'")
    assert len([k for k, _ in entries(shards) if k.endswith(".new")]) == 10
    assert_consistent(gsi, shards)


def test_update_id_rejected(gsi, shards):
    """Foreign partitions don't move rows on UPDATE, so changing id is refused."""
    q(gsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    with pytest.raises(psycopg2.errors.FeatureNotSupported):
        q(gsi, "UPDATE users SET id = 2 WHERE id = 1")
    assert entries(shards) == [("a@x", 1)]
    assert_consistent(gsi, shards)


def test_delete(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 20) g")
    q(gsi, "DELETE FROM users WHERE id % 2 = 0")
    assert assert_consistent(gsi, shards) == 10


def test_no_direct_modify(gsi):
    plan = "\n".join(r[0] for r in q(gsi, "EXPLAIN VERBOSE UPDATE users SET email = 'x' WHERE name = 'a'"))
    assert "WHERE ctid = $1" in plan                   # row-by-row, not pushed down
    assert "Foreign Scan on public.users_p0" in plan


def explain(conn, sql):
    return "\n".join(r[0] for r in q(conn, "EXPLAIN VERBOSE " + sql))


def test_non_key_update_pushed_down(gsi):
    """An UPDATE that can't change an index entry keeps postgres_fdw's pushdown."""
    plan = explain(gsi, "UPDATE users SET name = 'z' WHERE name = 'a'")
    assert "WHERE ctid = $1" not in plan
    assert "Foreign Scan" not in plan
    assert "UPDATE public.users_shard SET name" in plan


def test_delete_not_pushed_down(gsi):
    plan = explain(gsi, "DELETE FROM users WHERE name = 'a'")
    assert "WHERE ctid = $1" in plan


def test_non_key_update_keeps_index(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 2) FROM generate_series(1, 30) g")
    before = entries(shards)
    q(gsi, "UPDATE users SET name = 'z' WHERE name = 'n0'")
    assert q(gsi, "SELECT count(*) FROM users WHERE name = 'z'")[0][0] == 15
    assert entries(shards) == before
    assert_consistent(gsi, shards)


def test_mixed_xact_pushdown_and_key_update(gsi, shards):
    """A pushed-down UPDATE and a key UPDATE in one transaction commit together."""
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
    gsi.autocommit = False
    q(gsi, "UPDATE users SET name = 'z' WHERE id <= 20")
    q(gsi, "UPDATE users SET email = 'moved@x' WHERE id = 25")
    gsi.commit()
    gsi.autocommit = True
    assert q(gsi, "SELECT count(*) FROM users WHERE name = 'z'")[0][0] == 20
    assert ("moved@x", 25) in entries(shards)
    assert_consistent(gsi, shards)


def test_random_workload(gsi, shards):
    rnd = random.Random(631)
    for _ in range(300):
        i = rnd.randint(1, 60)
        op = rnd.random()
        key = f"k{rnd.randint(1, 40)}@x"
        if op < 0.4:
            if not q(gsi, "SELECT 1 FROM users WHERE id = %s", (i,)):
                q(gsi, "INSERT INTO users VALUES (%s, %s, 'n')", (i, key))
        elif op < 0.75:
            q(gsi, "UPDATE users SET email = %s WHERE id = %s", (key, i))
        else:
            q(gsi, "DELETE FROM users WHERE id = %s", (i,))
    assert_consistent(gsi, shards)
