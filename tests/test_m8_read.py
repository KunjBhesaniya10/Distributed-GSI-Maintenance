"""M8: point lookups on the indexed column go through the GSI (two hops)."""
import random

from conftest import COORD, connect, q


def plan(c, sql, args=None, analyze=False):
    return "\n".join(r[0] for r in q(c, ("EXPLAIN (ANALYZE, COSTS OFF, TIMING OFF) " if analyze
                                         else "EXPLAIN (COSTS OFF) ") + sql, args))


def load(c, n=60):
    q(c, "INSERT INTO users SELECT g, 'k' || (g %% 20) || '@x', 'n' || g FROM generate_series(1, %s) g", (n,))


def broadcast(c, key):
    # email || '' is not a plain column, so the GSI is not used
    return sorted(q(c, "SELECT * FROM users WHERE email || '' = %s", (key,)))


def test_plan_uses_gsi(gsi):
    p = plan(gsi, "SELECT * FROM users WHERE email = 'a@x'")
    assert "Custom Scan (GsiLookup) on users" in p, p
    assert "Append" not in p
    assert "GSI: users_email" in p


def test_plan_without_key_qual_broadcasts(gsi):
    p = plan(gsi, "SELECT * FROM users WHERE name = 'a'")
    assert "GsiLookup" not in p and "Append" in p


def test_results_match_broadcast(gsi):
    load(gsi)
    for k in [f"k{i}@x" for i in range(20)] + ["missing@x"]:
        assert sorted(q(gsi, "SELECT * FROM users WHERE email = %s", (k,))) == broadcast(gsi, k)


def test_unique_key_touches_one_shard(ugsi):
    q(ugsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
    p = plan(ugsi, "SELECT * FROM users WHERE email = 'u7@x'", analyze=True)
    assert "Shards Fetched: 1" in p and "rows=1" in p, p


def test_non_unique_key_spans_shards(gsi):
    q(gsi, "INSERT INTO users SELECT g, 'same@x', 'n' FROM generate_series(1, 30) g")
    assert q(gsi, "SELECT count(*) FROM users WHERE email = 'same@x'") == [(30,)]
    assert "Shards Fetched: 3" in plan(gsi, "SELECT * FROM users WHERE email = 'same@x'", analyze=True)


def test_extra_quals_and_projection(gsi):
    load(gsi)
    got = q(gsi, "SELECT name FROM users WHERE email = 'k3@x' AND id > 30 ORDER BY name")
    want = q(gsi, "SELECT name FROM users WHERE email || '' = 'k3@x' AND id > 30 ORDER BY name")
    assert got == want and got


def test_stale_entry_filtered_by_recheck(gsi, shards):
    q(gsi, "INSERT INTO users VALUES (1, 'real@x', 'a')")
    node = q(gsi, "SELECT gsi_node_for('ghost@x'::text)")[0][0]
    base = next(n for n, s in enumerate(shards) if q(s, "SELECT 1 FROM users_shard WHERE id = 1"))
    q(shards[node], "INSERT INTO gsi_users_email VALUES ('ghost@x', 1, %s)", (base,))   # lying entry
    assert q(gsi, "SELECT * FROM users WHERE email = 'ghost@x'") == []
    q(shards[node], "DELETE FROM gsi_users_email WHERE key = 'ghost@x'")


def test_prepared_statement_param(gsi):
    load(gsi)
    c = connect(COORD)
    q(c, "SET plan_cache_mode = force_generic_plan")
    q(c, "PREPARE p(text) AS SELECT * FROM users WHERE email = $1")
    assert "GsiLookup" in "\n".join(r[0] for r in q(c, "EXPLAIN (COSTS OFF) EXECUTE p('k1@x')"))
    for k in ("k1@x", "k2@x", "none@x"):
        assert sorted(q(c, "EXECUTE p(%s)", (k,))) == broadcast(gsi, k)


def test_join_and_subquery(gsi):
    load(gsi)
    got = q(gsi, "SELECT count(*) FROM (VALUES ('k1@x'), ('k2@x')) v(e) "
                 "JOIN LATERAL (SELECT * FROM users WHERE email = v.e) u ON true")
    assert got == [(6,)]


def test_reads_own_writes_in_transaction(gsi):
    c = connect(COORD, autocommit=False)
    q(c, "INSERT INTO users VALUES (1, 'mine@x', 'a')")
    assert len(q(c, "SELECT * FROM users WHERE email = 'mine@x'")) == 1
    c.rollback()
    assert q(gsi, "SELECT * FROM users WHERE email = 'mine@x'") == []


def test_update_and_for_update_use_normal_plan(gsi):
    assert "GsiLookup" not in plan(gsi, "UPDATE users SET name = 'z' WHERE email = 'a@x'")
    assert "GsiLookup" not in plan(gsi, "SELECT * FROM users WHERE email = 'a@x' FOR UPDATE")


def test_random_reads_after_random_writes(gsi):
    rnd = random.Random(8)
    load(gsi)
    for _ in range(100):
        i = rnd.randint(1, 60)
        q(gsi, "UPDATE users SET email = %s WHERE id = %s", (f"k{rnd.randint(0, 25)}@x", i))
    for k in [f"k{i}@x" for i in range(26)]:
        assert sorted(q(gsi, "SELECT * FROM users WHERE email = %s", (k,))) == broadcast(gsi, k)
