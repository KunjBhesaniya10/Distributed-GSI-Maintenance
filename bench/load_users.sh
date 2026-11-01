#!/usr/bin/env bash
# Load a large users table through the real write path (base row + GSI
# entries, 2PC), using several sessions in parallel.
#
#   bash bench/load_users.sh [rows] [sessions]      default: 1000000 rows, 8 sessions
#
#   id     1..rows
#   email  user<id>@<one of 5 domains>                unique   -> unique GSI users_email
#   name   '<first> <last>' from 40 x 40 common names   1,600 distinct, ~rows/1600 each
#                                                       -> non-unique GSI users_name
# Run from the repo root with the cluster up.
set -e
ROWS=${1:-1000000}
SESSIONS=${2:-8}
CHUNK=25000                     # rows per transaction
cd "$(dirname "$0")/../docker"
psql_c() { docker compose exec -T coordinator psql -U postgres -qX -v ON_ERROR_STOP=1 "$@"; }

psql_c <<'EOF'
SELECT gsi_drop('users') WHERE EXISTS (SELECT 1 FROM gsi.gsi_index WHERE base_table = 'users'::regclass);
TRUNCATE users;
SELECT gsi_create('users', 'email', true);
SELECT gsi_create('users', 'name');
CREATE OR REPLACE FUNCTION demo_name(i bigint) RETURNS text IMMUTABLE LANGUAGE sql AS $$
  SELECT (ARRAY['James','Mary','Robert','Patricia','John','Jennifer','Michael','Linda','David','Elizabeth',
                'William','Barbara','Richard','Susan','Joseph','Jessica','Thomas','Sarah','Charles','Karen',
                'Aarav','Priya','Rohan','Ananya','Vikram','Diya','Arjun','Isha','Kunal','Meera',
                'Wei','Mei','Hiroshi','Yuki','Carlos','Sofia','Ahmed','Fatima','Ivan','Olga'])[1 + i % 40]
         || ' ' ||
         (ARRAY['Smith','Johnson','Williams','Brown','Jones','Garcia','Miller','Davis','Rodriguez','Martinez',
                'Patel','Shah','Mehta','Sharma','Gupta','Singh','Kumar','Das','Iyer','Reddy',
                'Wang','Li','Zhang','Chen','Tanaka','Sato','Kim','Lee','Nguyen','Tran',
                'Muller','Schmidt','Rossi','Silva','Santos','Ivanov','Novak','Khan','Ali','Cohen'])
           [1 + (i / 40) % 40]                  -- all 1,600 first x last combinations
$$;
EOF

echo "loading $ROWS rows with $SESSIONS sessions, $CHUNK rows per transaction ..."
start=$(date +%s)
per=$(( (ROWS + SESSIONS - 1) / SESSIONS ))
for s in $(seq 0 $((SESSIONS - 1))); do
    lo=$(( s * per + 1 )); hi=$(( (s + 1) * per )); [ $hi -gt $ROWS ] && hi=$ROWS
    (
        for c in $(seq $lo $CHUNK $hi); do
            e=$(( c + CHUNK - 1 )); [ $e -gt $hi ] && e=$hi
            psql_c -c "INSERT INTO users SELECT g, 'user' || g || '@' ||
                         (ARRAY['example.com','mail.org','iitb.ac.in','corp.net','post.io'])[1 + g % 5],
                         demo_name(g) FROM generate_series($c, $e) g"
        done
    ) &
done
wait
end=$(date +%s)
secs=$(( end - start )); [ $secs -eq 0 ] && secs=1
echo "loaded in ${secs}s ($(( ROWS / secs )) rows/s)"

psql_c <<'EOF'
-- the loader sets ids explicitly; move the sequence past them (no-op on old clusters without it)
SELECT setval(to_regclass('users_id_seq'), (SELECT max(id) FROM users));
ANALYZE users;
SELECT count(*) AS rows FROM users;
SELECT tableoid::regclass AS partition, count(*) FROM users GROUP BY 1 ORDER BY 1;
SELECT count(DISTINCT name) AS distinct_names FROM users;
EOF
