"""M1: cluster is up, gsi_fdw is loaded and routes base-table writes."""
from conftest import q


def test_extension_loaded(coord):
    assert q(coord, "SHOW shared_preload_libraries") == [("gsi_fdw",)]
    assert q(coord, "SELECT extname FROM pg_extension WHERE extname = 'gsi_fdw'")
    assert q(coord, "SHOW max_prepared_transactions") == [("100",)]


def test_shards_configured(shards):
    for s in shards:
        assert q(s, "SHOW max_prepared_transactions") == [("100",)]


def test_partition_routing(gsi, shards):
    q(gsi, "INSERT INTO users SELECT g, 'u' || g || '@x', 'n' FROM generate_series(1, 30) g")
    counts = [q(s, "SELECT count(*) FROM users_shard")[0][0] for s in shards]
    assert sum(counts) == 30 and all(c > 0 for c in counts)
    assert q(gsi, "SELECT count(*) FROM users") == [(30,)]
