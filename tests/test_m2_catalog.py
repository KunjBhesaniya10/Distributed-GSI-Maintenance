"""M2: catalog, index tables on every node, placement hash."""
import psycopg2
import pytest

from conftest import q


def test_index_tables_created(gsi, shards):
    assert q(gsi, "SELECT index_name, column_name, is_unique, status FROM gsi.gsi_index") == \
        [("users_email", "email", False, "ready")]
    for s in shards:
        assert q(s, "SELECT to_regclass('public.gsi_users_email') IS NOT NULL") == [(True,)]


def test_unique_index_created(ugsi, shards):
    for s in shards:
        assert q(s, "SELECT count(*) FROM pg_indexes WHERE tablename = 'gsi_users_email' "
                    "AND indexdef LIKE 'CREATE UNIQUE INDEX%%(key)'") == [(1,)]


def test_placement_spreads_keys(gsi):
    nodes = [n for (n,) in q(gsi, "SELECT gsi_node_for('k' || g) FROM generate_series(1, 300) g")]
    counts = [nodes.count(i) for i in range(3)]
    assert all(c > 50 for c in counts), counts


def test_create_requires_empty_table(gsi):
    q(gsi, "SELECT gsi_drop('users')")
    q(gsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    with pytest.raises(psycopg2.errors.FeatureNotSupported):
        q(gsi, "SELECT gsi_create('users', 'email')")


def test_drop_removes_tables(gsi, shards):
    q(gsi, "SELECT gsi_drop('users')")
    assert q(gsi, "SELECT count(*) FROM gsi.gsi_index") == [(0,)]
    for s in shards:
        assert q(s, "SELECT to_regclass('public.gsi_users_email')") == [(None,)]
