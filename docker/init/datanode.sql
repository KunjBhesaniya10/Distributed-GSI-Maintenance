-- Base partition; index tables are created by gsi_create().
CREATE TABLE users_shard (id bigint PRIMARY KEY, email text NOT NULL, name text);

-- Async GSIs: changes waiting for the applier workers (gsi_async.c)
CREATE TABLE gsi_outbox (
    seq        bigserial PRIMARY KEY,
    index_name text NOT NULL,
    old_key    text,
    new_key    text,
    id         bigint NOT NULL,
    created_at timestamptz NOT NULL DEFAULT now()
);
