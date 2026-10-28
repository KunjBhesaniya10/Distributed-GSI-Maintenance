CREATE EXTENSION gsi_fdw;

CREATE SERVER shard1 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard1', port '5432', dbname 'postgres');
CREATE SERVER shard2 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard2', port '5432', dbname 'postgres');
CREATE SERVER shard3 FOREIGN DATA WRAPPER gsi_fdw OPTIONS (host 'shard3', port '5432', dbname 'postgres');
CREATE USER MAPPING FOR PUBLIC SERVER shard1 OPTIONS (user 'postgres');
CREATE USER MAPPING FOR PUBLIC SERVER shard2 OPTIONS (user 'postgres');
CREATE USER MAPPING FOR PUBLIC SERVER shard3 OPTIONS (user 'postgres');

INSERT INTO gsi.gsi_node VALUES (0, 'shard1'), (1, 'shard2'), (2, 'shard3');

-- Foreign partitions can't carry a PK; each shard's PK enforces id uniqueness.
CREATE TABLE users (id bigint NOT NULL, email text NOT NULL, name text) PARTITION BY HASH (id);
CREATE FOREIGN TABLE users_p0 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 0)
    SERVER shard1 OPTIONS (table_name 'users_shard');
CREATE FOREIGN TABLE users_p1 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 1)
    SERVER shard2 OPTIONS (table_name 'users_shard');
CREATE FOREIGN TABLE users_p2 PARTITION OF users FOR VALUES WITH (MODULUS 3, REMAINDER 2)
    SERVER shard3 OPTIONS (table_name 'users_shard');
