"""Consistency checker: every GSI must hold exactly one correctly placed entry
per base row (rows with a NULL key have none), and no gsi_ prepared
transaction may be left on any node.

Run standalone:  python tests/checker.py
"""
import sys

from conftest import COORD, SHARDS, connect, q


def gsis(coord, base="users"):
    """(index_name, column_name) of every GSI on base."""
    return q(coord, "SELECT index_name, column_name FROM gsi.gsi_index "
                    "WHERE base_table = %s::regclass ORDER BY index_name", (base,))


def check(coord, shards, index_table="gsi_users_email", column="email"):
    base = set()
    index = set()
    misplaced = []
    leftover = []
    for node, s in enumerate(shards):
        base |= {(k, i, node) for i, k in q(s, f"SELECT id, {column}::text FROM users_shard "
                                               f"WHERE {column} IS NOT NULL")}
        for key, i, shard in q(s, f"SELECT key::text, id, shard FROM {index_table}"):
            index.add((key, i, shard))
            if q(coord, f"SELECT gsi_node_for(%s::text::{type_of(coord, column)})", (key,))[0][0] != node:
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


def type_of(coord, column, base="users"):
    return q(coord, "SELECT format_type(atttypid, atttypmod) FROM pg_attribute "
                    "WHERE attrelid = %s::regclass AND attname = %s", (base, column))[0][0]


def check_all(coord, shards):
    """{index_name: check result} for every GSI on users."""
    return {name: check(coord, shards, f"gsi_{name}", col) for name, col in gsis(coord)}


def assert_consistent(coord, shards):
    """Assert every GSI is exact; return the base row count."""
    rows = sum(q(s, "SELECT count(*) FROM users_shard")[0][0] for s in shards)
    for name, r in check_all(coord, shards).items():
        bad = {k: v for k, v in r.items() if k != "rows" and v}
        assert not bad, f"GSI {name} inconsistent: {bad}"
    return rows


if __name__ == "__main__":
    c = connect(COORD)
    ss = [connect(p) for p in SHARDS]
    ok = True
    for name, r in check_all(c, ss).items():
        print(f"{name}: rows={r['rows']} missing={len(r['missing'])} orphaned={len(r['orphaned'])} "
              f"misplaced={len(r['misplaced'])} prepared={len(r['prepared'])}")
        ok = ok and not any(r[k] for k in ("missing", "orphaned", "misplaced", "prepared"))
    sys.exit(0 if ok else 1)
