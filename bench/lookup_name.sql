-- Lookup on the non-unique name GSI (about rows/1600 matches, spread over all shards).
\set id random(1, 1600)
SELECT * FROM users WHERE name = demo_name(:id);
