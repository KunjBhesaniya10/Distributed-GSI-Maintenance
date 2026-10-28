/* src/gsi_fdw/gsi_fdw--1.0.sql */

-- complain if script is sourced in psql, rather than via CREATE EXTENSION
\echo Use "CREATE EXTENSION gsi_fdw" to load this file. \quit

-- postgres_fdw 1.1 objects, renamed

CREATE FUNCTION gsi_fdw_handler()
RETURNS fdw_handler
AS 'MODULE_PATHNAME', 'postgres_fdw_handler'
LANGUAGE C STRICT;

CREATE FUNCTION gsi_fdw_validator(text[], oid)
RETURNS void
AS 'MODULE_PATHNAME', 'postgres_fdw_validator'
LANGUAGE C STRICT;

CREATE FOREIGN DATA WRAPPER gsi_fdw
  HANDLER gsi_fdw_handler
  VALIDATOR gsi_fdw_validator;

CREATE FUNCTION gsi_fdw_get_connections (OUT server_name text,
    OUT valid boolean)
RETURNS SETOF record
AS 'MODULE_PATHNAME', 'postgres_fdw_get_connections'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION gsi_fdw_disconnect (text)
RETURNS bool
AS 'MODULE_PATHNAME', 'postgres_fdw_disconnect'
LANGUAGE C STRICT PARALLEL RESTRICTED;

CREATE FUNCTION gsi_fdw_disconnect_all ()
RETURNS bool
AS 'MODULE_PATHNAME', 'postgres_fdw_disconnect_all'
LANGUAGE C STRICT PARALLEL RESTRICTED;

-- GSI catalog

CREATE SCHEMA gsi;

CREATE TABLE gsi.gsi_node (
    node_id     int PRIMARY KEY CHECK (node_id >= 0),
    server_name name NOT NULL UNIQUE
);

CREATE TABLE gsi.gsi_index (
    index_name  name PRIMARY KEY,
    base_table  regclass NOT NULL UNIQUE,
    column_name name NOT NULL,
    is_unique   boolean NOT NULL DEFAULT false,
    mode        text NOT NULL DEFAULT 'sync' CHECK (mode IN ('sync', 'async')),
    status      text NOT NULL DEFAULT 'ready' CHECK (status IN ('write-only', 'ready'))
);

CREATE FUNCTION gsi_create(base regclass, col name, is_unique boolean DEFAULT false)
RETURNS text
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION gsi_drop(base regclass)
RETURNS void
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;

CREATE FUNCTION gsi_node_for(anyelement)
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT STABLE;

CREATE FUNCTION gsi_resolve(grace_secs int DEFAULT 0)
RETURNS int
AS 'MODULE_PATHNAME'
LANGUAGE C STRICT;
