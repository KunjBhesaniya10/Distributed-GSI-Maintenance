"""M4: GSI writes commit atomically with two-phase commit."""
import psycopg2
import pytest

from checker import assert_consistent
from conftest import COORD, connect, q


def test_commit_leaves_no_prepared(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
    assert assert_consistent(gsi, shards) == 30


def test_rollback_undoes_base_and_index(gsi, shards):
    c = connect(COORD, autocommit=False)
    q(c, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
    c.rollback()
    assert assert_consistent(gsi, shards) == 0
    assert sum(q(s, "SELECT count(*) FROM gsi_users_email")[0][0] for s in shards) == 0


def test_error_mid_transaction_undoes_everything(gsi, shards):
    c = connect(COORD, autocommit=False)
    q(c, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    with pytest.raises(psycopg2.errors.UniqueViolation):
        q(c, "INSERT INTO users VALUES (1, 'b@x', 'b')")      # shard PK violation
    c.rollback()
    assert assert_consistent(gsi, shards) == 0


def test_failed_prepare_aborts_all(gsi, shards):
    """A participant that fails PREPARE must abort every participant.

    A deferred unique constraint on shard1 is only checked at PREPARE time."""
    q(shards[0], "CREATE TABLE IF NOT EXISTS defer_t (x int UNIQUE DEFERRABLE INITIALLY DEFERRED)")
    q(shards[0], "TRUNCATE defer_t")
    q(shards[0], "CREATE OR REPLACE FUNCTION bad() RETURNS trigger LANGUAGE plpgsql AS "
                 "$$BEGIN INSERT INTO public.defer_t VALUES (1), (1); RETURN NEW; END$$")
    q(shards[0], "CREATE OR REPLACE TRIGGER bad AFTER INSERT ON users_shard FOR EACH ROW "
                 "WHEN (NEW.email = 'boom@x') EXECUTE FUNCTION bad()")
    try:
        c = connect(COORD, autocommit=False)
        q(c, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
        q(c, "INSERT INTO users SELECT min(g), 'boom@x', 'n' FROM generate_series(1000, 1100) g "
             "WHERE satisfies_hash_partition('users'::regclass, 3, 0, g::bigint)")
        with pytest.raises(psycopg2.errors.UniqueViolation):
            c.commit()
        assert assert_consistent(gsi, shards) == 0
    finally:
        q(shards[0], "DROP TRIGGER IF EXISTS bad ON users_shard")


def test_savepoint_rollback(gsi, shards):
    c = connect(COORD, autocommit=False)
    q(c, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    q(c, "SAVEPOINT s")
    q(c, "INSERT INTO users VALUES (2, 'b@x', 'b')")
    q(c, "ROLLBACK TO SAVEPOINT s")
    c.commit()
    assert assert_consistent(gsi, shards) == 1
