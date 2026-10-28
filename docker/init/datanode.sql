-- Base partition; index tables are created by gsi_create().
CREATE TABLE users_shard (id bigint PRIMARY KEY, email text NOT NULL, name text);
