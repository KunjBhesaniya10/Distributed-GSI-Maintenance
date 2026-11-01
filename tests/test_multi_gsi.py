"""Several GSIs on one table: a unique GSI on email and a non-unique one on name."""
import random

import psycopg2
import pytest

from checker import assert_consistent, check_all
from conftest import q, reset_cluster


@pytest.fixture
def mgsi(coord, shards):
    reset_cluster(coord, shards, unique=True)
    q(coord, "SELECT gsi_create('users', 'name')")
    return coord


def entries(shards, table):
    return sorted(r for s in shards for r in q(s, f"SELECT key, id FROM {table}"))


def plan(c, sql):
    return "\n".join(r[0] for r in q(c, "EXPLAIN (COSTS OFF) " + sql))


def test_catalog_and_tables(mgsi, shards):
    assert q(mgsi, "SELECT index_name, column_name, is_unique FROM gsi.gsi_index ORDER BY 1") == \
        [("users_email", "email", True), ("users_name", "name", False)]
    for s in shards:
        assert q(s, "SELECT to_regclass('public.gsi_users_email') IS NOT NULL, "
                    "to_regclass('public.gsi_users_name') IS NOT NULL") == [(True, True)]


def test_same_column_twice_rejected(mgsi):
    with pytest.raises(psycopg2.errors.DuplicateObject):
        q(mgsi, "SELECT gsi_create('users', 'name')")


def test_writes_maintain_every_gsi(mgsi, shards):
    q(mgsi, "INSERT INTO users SELECT g, 'u' || g || '@x', "
            "CASE WHEN g % 5 = 0 THEN NULL ELSE 'n' || (g % 4) END FROM generate_series(1, 40) g")
    assert len(entries(shards, "gsi_users_email")) == 40
    assert len(entries(shards, "gsi_users_name")) == 32        # NULL names are not indexed
    q(mgsi, "UPDATE users SET email = 'e' || id || '@x' WHERE id <= 10")          # email only
    q(mgsi, "UPDATE users SET name = 'renamed' WHERE id BETWEEN 11 AND 20")      # name only
    q(mgsi, "UPDATE users SET email = 'b' || id || '@x', name = NULL WHERE id > 35")  # both
    q(mgsi, "DELETE FROM users WHERE id % 7 = 0")
    assert_consistent(mgsi, shards)
    assert ("renamed", 11) in entries(shards, "gsi_users_name")


def test_update_of_any_indexed_column_not_pushed_down(mgsi):
    p = "\n".join(r[0] for r in q(mgsi, "EXPLAIN VERBOSE UPDATE users SET name = 'z' WHERE id < 5"))
    assert "WHERE ctid = $1" in p


def test_unique_violation_rolls_back_other_gsi(mgsi, shards):
    q(mgsi, "INSERT INTO users VALUES (1, 'a@x', 'one'), (2, 'b@x', 'two')")
    with pytest.raises(psycopg2.errors.UniqueViolation):
        q(mgsi, "UPDATE users SET email = 'a@x', name = 'clash' WHERE id = 2")
    assert entries(shards, "gsi_users_name") == [("one", 1), ("two", 2)]
    assert_consistent(mgsi, shards)


def test_planner_picks_matching_gsi(mgsi):
    assert "GSI: users_name" in plan(mgsi, "SELECT * FROM users WHERE name = 'n1'")
    assert "GSI: users_email" in plan(mgsi, "SELECT * FROM users WHERE email = 'a@x'")
    # both columns constrained: the unique GSI is preferred
    assert "GSI: users_email" in plan(mgsi, "SELECT * FROM users WHERE name = 'n1' AND email = 'a@x'")


def test_lookup_results_match_broadcast(mgsi):
    q(mgsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' || (g % 6) FROM generate_series(1, 60) g")
    for k in [f"n{i}" for i in range(6)] + ["nobody"]:
        got = sorted(q(mgsi, "SELECT * FROM users WHERE name = %s", (k,)))
        assert got == sorted(q(mgsi, "SELECT * FROM users WHERE name || '' = %s", (k,)))


def test_drop_one_gsi_keeps_the_other(mgsi, shards):
    q(mgsi, "SELECT gsi_drop('users', 'name')")
    assert q(mgsi, "SELECT index_name FROM gsi.gsi_index") == [("users_email",)]
    for s in shards:
        assert q(s, "SELECT to_regclass('public.gsi_users_name')") == [(None,)]
    q(mgsi, "INSERT INTO users VALUES (1, 'a@x', 'x')")
    assert "GSI: users_email" in plan(mgsi, "SELECT * FROM users WHERE email = 'a@x'")
    assert "GsiLookup" not in plan(mgsi, "SELECT * FROM users WHERE name = 'x'")
    assert_consistent(mgsi, shards)


def test_random_workload_two_gsis(mgsi, shards):
    rnd = random.Random(632)
    for _ in range(300):
        i = rnd.randint(1, 50)
        op = rnd.random()
        name = rnd.choice([None, "a", "b", "c", "d"])
        try:
            if op < 0.35:
                if not q(mgsi, "SELECT 1 FROM users WHERE id = %s", (i,)):
                    q(mgsi, "INSERT INTO users VALUES (%s, %s, %s)", (i, f"m{i}@x", name))
            elif op < 0.55:
                q(mgsi, "UPDATE users SET email = %s WHERE id = %s", (f"k{rnd.randint(1, 80)}@x", i))
            elif op < 0.8:
                q(mgsi, "UPDATE users SET name = %s WHERE id = %s", (name, i))
            else:
                q(mgsi, "DELETE FROM users WHERE id = %s", (i,))
        except psycopg2.errors.UniqueViolation:
            pass
    results = check_all(mgsi, shards)
    assert set(results) == {"users_email", "users_name"}
    assert_consistent(mgsi, shards)
