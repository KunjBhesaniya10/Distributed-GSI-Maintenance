"""Concurrent random DML from many clients; the GSI must stay exact.

Expected errors are retried/ignored: serialization failures (remote
REPEATABLE READ), lock timeouts (cross-node deadlocks), unique violations.
"""
import random
import threading
import time

import psycopg2

from checker import assert_consistent
from conftest import COORD, connect, q

CLIENTS = 8
SECONDS = 20
EXPECTED = {"40001", "40P01", "55P03", "23505"}


def run_workload(unique_keys):
    stats = {"commits": 0, "aborts": 0}
    unexpected = []
    lock = threading.Lock()
    stop = time.time() + SECONDS

    def client(seed):
        rnd = random.Random(seed)
        c = connect(COORD, autocommit=False)
        while time.time() < stop:
            try:
                for _ in range(rnd.randint(1, 4)):          # multi-statement transactions
                    i = rnd.randint(1, 200)
                    key = f"k{rnd.randint(1, 150 if unique_keys else 30)}@x"
                    op = rnd.random()
                    if op < 0.4:
                        q(c, "INSERT INTO users SELECT %s, %s, 'n' "
                             "WHERE NOT EXISTS (SELECT 1 FROM users WHERE id = %s)", (i, key, i))
                    elif op < 0.8:
                        q(c, "UPDATE users SET email = %s WHERE id = %s", (key, i))
                    else:
                        q(c, "DELETE FROM users WHERE id = %s", (i,))
                if rnd.random() < 0.1:
                    c.rollback()
                else:
                    c.commit()
                with lock:
                    stats["commits"] += 1
            except psycopg2.Error as e:
                c.rollback()
                with lock:
                    stats["aborts"] += 1
                    if e.pgcode not in EXPECTED:
                        unexpected.append((e.pgcode, str(e).splitlines()[0]))
        c.close()

    threads = [threading.Thread(target=client, args=(s,)) for s in range(CLIENTS)]
    for t in threads:
        t.start()
    for t in threads:
        t.join()
    return stats, unexpected


def test_stress_non_unique(gsi, shards):
    stats, unexpected = run_workload(unique_keys=False)
    print(stats)
    assert not unexpected, unexpected[:5]
    assert stats["commits"] > 100
    assert_consistent(gsi, shards)


def test_stress_unique(ugsi, shards):
    stats, unexpected = run_workload(unique_keys=True)
    print(stats)
    assert not unexpected, unexpected[:5]
    assert stats["commits"] > 100
    assert_consistent(ugsi, shards)
    keys = [k for s in shards for (k,) in q(s, "SELECT key FROM gsi_users_email")]
    assert len(keys) == len(set(keys))
    emails = [e for s in shards for (e,) in q(s, "SELECT email FROM users_shard")]
    assert len(emails) == len(set(emails))           # globally unique in the base table too
