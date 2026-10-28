"""Shared fixtures: connections to the 4 nodes and a fresh GSI per test."""
import time

import psycopg2
import pytest

COORD = dict(host="localhost", port=5432, user="postgres", dbname="postgres")
SHARDS = [dict(COORD, port=p) for p in (5433, 5434, 5435)]


def connect(params, autocommit=True):
    for _ in range(60):
        try:
            conn = psycopg2.connect(**params)
            conn.autocommit = autocommit
            return conn
        except psycopg2.OperationalError:
            time.sleep(1)
    raise RuntimeError(f"cannot connect to {params}")


def q(conn, sql, args=None):
    with conn.cursor() as cur:
        cur.execute(sql, args)
        return cur.fetchall() if cur.description else None


@pytest.fixture
def coord():
    conn = connect(COORD)
    yield conn
    conn.close()


@pytest.fixture
def shards():
    conns = [connect(s) for s in SHARDS]
    yield conns
    for c in conns:
        c.close()


def reset_cluster(coord, shards, unique=False):
    for s in shards:
        for (gid,) in q(s, "SELECT gid FROM pg_prepared_xacts WHERE gid LIKE 'gsi\\_%'"):
            q(s, f"ROLLBACK PREPARED '{gid}'")
        q(s, "TRUNCATE users_shard")
    if q(coord, "SELECT 1 FROM gsi.gsi_index WHERE base_table = 'users'::regclass"):
        q(coord, "SELECT gsi_drop('users')")
    q(coord, "SELECT gsi_create('users', 'email', %s)", (unique,))


@pytest.fixture
def gsi(coord, shards):
    """Empty users table with a non-unique GSI on email."""
    reset_cluster(coord, shards)
    return coord


@pytest.fixture
def ugsi(coord, shards):
    """Empty users table with a unique GSI on email."""
    reset_cluster(coord, shards, unique=True)
    return coord
