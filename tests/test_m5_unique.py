"""M5: a unique GSI enforces uniqueness across shards."""
import threading

import psycopg2
import pytest

from checker import assert_consistent
from conftest import COORD, connect, q


def base_node(shards, i):
    return next(n for n, s in enumerate(shards) if q(s, "SELECT 1 FROM users_shard WHERE id = %s", (i,)))


def test_duplicate_rejected_across_shards(ugsi, shards):
    q(ugsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    other = next(i for i in range(2, 50)
                 if q(ugsi, "SELECT NOT satisfies_hash_partition('users'::regclass, 3, %s, %s::bigint)",
                      (base_node(shards, 1), i))[0][0])
    with pytest.raises(psycopg2.errors.UniqueViolation):
        q(ugsi, "INSERT INTO users VALUES (%s, 'a@x', 'b')", (other,))
    assert assert_consistent(ugsi, shards) == 1        # nothing left of the failed insert


def test_failed_statement_rolls_back_all_rows(ugsi, shards):
    q(ugsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    with pytest.raises(psycopg2.errors.UniqueViolation):
        q(ugsi, "INSERT INTO users VALUES (2, 'b@x', 'b'), (3, 'c@x', 'c'), (4, 'a@x', 'd')")
    assert assert_consistent(ugsi, shards) == 1


def test_update_into_duplicate_rejected(ugsi, shards):
    q(ugsi, "INSERT INTO users VALUES (1, 'a@x', 'a'), (2, 'b@x', 'b')")
    with pytest.raises(psycopg2.errors.UniqueViolation):
        q(ugsi, "UPDATE users SET email = 'a@x' WHERE id = 2")
    assert assert_consistent(ugsi, shards) == 2


def test_key_reuse_after_delete(ugsi, shards):
    q(ugsi, "INSERT INTO users VALUES (1, 'a@x', 'a')")
    q(ugsi, "DELETE FROM users WHERE id = 1")
    q(ugsi, "INSERT INTO users VALUES (2, 'a@x', 'b')")
    assert assert_consistent(ugsi, shards) == 1


def test_swap_keys_in_one_transaction(ugsi, shards):
    q(ugsi, "INSERT INTO users VALUES (1, 'a@x', 'a'), (2, 'b@x', 'b')")
    c = connect(COORD, autocommit=False)
    q(c, "UPDATE users SET email = 'tmp@x' WHERE id = 1")
    q(c, "UPDATE users SET email = 'a@x' WHERE id = 2")
    q(c, "UPDATE users SET email = 'b@x' WHERE id = 1")
    c.commit()
    assert assert_consistent(ugsi, shards) == 2


def test_non_unique_allows_duplicates(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'same@x', 'n' FROM generate_series(1, 10) g")
    assert assert_consistent(gsi, shards) == 10


def test_concurrent_duplicates_exactly_one_wins(ugsi, shards):
    results = []
    barrier = threading.Barrier(8)

    def worker(i):
        c = connect(COORD)
        barrier.wait()
        try:
            q(c, "INSERT INTO users VALUES (%s, 'race@x', 'n')", (i,))
            results.append("ok")
        except psycopg2.Error as e:
            results.append(e.pgcode)
        c.close()

    threads = [threading.Thread(target=worker, args=(i,)) for i in range(1, 9)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    assert results.count("ok") == 1, results
    assert set(results) <= {"ok", "23505"}, results
    assert assert_consistent(ugsi, shards) == 1
