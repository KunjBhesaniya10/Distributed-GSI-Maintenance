"""M7: the background resolver repairs a crash without any manual call."""
import time

from checker import assert_consistent
from conftest import q
from test_m6_resolver import crash_at, has_base, has_entry, pick_row, prepared


def wait_resolved(shards, timeout=40):
    end = time.time() + timeout
    while time.time() < end:
        if not prepared(shards):
            return True
        time.sleep(1)
    return False


def test_worker_running(coord):
    assert q(coord, "SELECT count(*) FROM pg_stat_activity WHERE backend_type = 'gsi resolver'") == [(1,)]


def test_worker_rolls_forward(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("after_local_commit", row)
    assert len(prepared(shards)) == 2
    assert wait_resolved(shards)            # grace 10 s + interval 5 s
    assert has_base(shards, row) and has_entry(shards, row)
    assert_consistent(c, shards)


def test_worker_rolls_back(gsi, shards):
    row = pick_row(gsi)
    c = crash_at("after_prepare", row)
    assert wait_resolved(shards)
    assert not has_base(shards, row) and not has_entry(shards, row)
    assert_consistent(c, shards)
