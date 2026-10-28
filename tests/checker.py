"""Consistency checker: the GSI must hold exactly one correctly placed entry
per base row, and no gsi_ prepared transaction may be left on any node.

Run standalone:  python tests/checker.py
"""
import sys

from conftest import COORD, SHARDS, connect, q


def check(coord, shards, index_table="gsi_users_email"):
    base = set()
    index = set()
    misplaced = []
    leftover = []
    for node, s in enumerate(shards):
        base |= {(e, i, node) for i, e in q(s, "SELECT id, email FROM users_shard")}
        for key, i, shard in q(s, f"SELECT key, id, shard FROM {index_table}"):
            index.add((key, i, shard))
            if q(coord, "SELECT gsi_node_for(%s::text)", (key,))[0][0] != node:
                misplaced.append((key, i, node))
        leftover += [g for (g,) in q(s, "SELECT gid FROM pg_prepared_xacts "
                                        "WHERE gid LIKE 'gsi\\_%'")]
    return {
        "missing": sorted(base - index),     # base row without entry
        "orphaned": sorted(index - base),    # entry without base row
        "misplaced": misplaced,              # entry on the wrong node
        "prepared": leftover,                # unfinished 2PC
        "rows": len(base),
    }


def assert_consistent(coord, shards):
    r = check(coord, shards)
    bad = {k: v for k, v in r.items() if k != "rows" and v}
    assert not bad, f"GSI inconsistent: {bad}"
    return r["rows"]


if __name__ == "__main__":
    c = connect(COORD)
    ss = [connect(p) for p in SHARDS]
    r = check(c, ss)
    print(f"rows={r['rows']} missing={len(r['missing'])} orphaned={len(r['orphaned'])} "
          f"misplaced={len(r['misplaced'])} prepared={len(r['prepared'])}")
    sys.exit(0 if not any(r[k] for k in ("missing", "orphaned", "misplaced", "prepared")) else 1)
