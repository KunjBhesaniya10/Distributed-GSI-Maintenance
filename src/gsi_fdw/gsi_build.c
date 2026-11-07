/*-------------------------------------------------------------------------
 *
 * gsi_build.c
 *		  CALL gsi_create(): build a GSI online, on an empty or a full table
 *
 * Like CREATE INDEX CONCURRENTLY, in several transactions, never blocking
 * reads or writes of the table (docs/phase2_plan.md §7):
 *	1. create the index tables, catalog row with status 'write-only'; commit.
 *	   From then on every write maintains the index, the planner ignores it.
 *	2. wait for writers that started before step 1 (they don't know the
 *	   index), so every row is either written after step 1 or visible to 3.
 *	3. backfill: per shard, chunks of rows by id, each chunk one distributed
 *	   transaction that locks its rows FOR SHARE and inserts their entries.
 *	4. validate: entries = base rows (count and hash per index), with new
 *	   writes paused for the few seconds it takes (see gsi_create).
 *	5. status 'ready'; ANALYZE the table for the planner's estimates.
 * One coordinator sees every write, so step 2 replaces F1's delete-only
 * state.  Progress is committed with each chunk; calling gsi_create again
 * on a 'write-only' index resumes it.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_inherits.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/parsenodes.h"
#include "postgres_fdw.h"
#include "gsi.h"
#include "storage/lmgr.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"

PG_FUNCTION_INFO_V1(gsi_create);

static int	gsi_build_chunk_size = 10000;
static int	gsi_build_chunk_delay = 0;

/* SQLSTATEs a chunk retries after: serialization, deadlock, lock timeout */
#define CHUNK_MAX_RETRIES	100

typedef enum ChunkResult
{
	CHUNK_DONE,					/* rows backfilled, more may follow */
	CHUNK_LAST,					/* no rows left on this node */
	CHUNK_RETRY,				/* transient conflict: roll back, retry */
	CHUNK_DUPLICATE				/* unique GSI: two existing rows share a key */
} ChunkResult;

/* Run a query on a remote connection of the current distributed xact. */
static PGresult *
run_remote(PGconn *conn, const char *sql, int nparams, const char *const *values)
{
	if (!PQsendQueryParams(conn, sql, nparams, NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	return pgfdw_get_result(conn, sql);
}

static PGconn *
node_conn(GsiIndex *idx, int node)
{
	return GetConnection(GetUserMapping(GetUserId(), idx->servers[node]), false, NULL);
}

/* Append one element to a text-form array literal, quoted. */
static void
append_array_elem(StringInfo buf, const char *value)
{
	appendStringInfoChar(buf, buf->len == 0 ? '{' : ',');
	appendStringInfoChar(buf, '"');
	for (const char *p = value; *p; p++)
	{
		if (*p == '"' || *p == '\\')
			appendStringInfoChar(buf, '\\');
		appendStringInfoChar(buf, *p);
	}
	appendStringInfoChar(buf, '"');
}

/* Remote name of the base table on a node, from its foreign partition */
static char *
base_table_sql(Oid base, int node, GsiIndex *idx, char **key_col)
{
	List	   *children = find_inheritance_children(base, NoLock);
	ListCell   *lc;

	foreach(lc, children)
	{
		Oid			partid = lfirst_oid(lc);
		ForeignTable *ft = GetForeignTable(partid);
		char	   *nsp = get_namespace_name(get_rel_namespace(partid));
		char	   *relname = get_rel_name(partid);
		ListCell   *lo;

		if (ft->serverid != idx->servers[node])
			continue;
		foreach(lo, ft->options)
		{
			DefElem    *def = lfirst(lo);

			if (strcmp(def->defname, "schema_name") == 0)
				nsp = defGetString(def);
			else if (strcmp(def->defname, "table_name") == 0)
				relname = defGetString(def);
		}
		*key_col = (char *) quote_identifier(idx->column);
		return psprintf("%s.%s", quote_identifier(nsp), quote_identifier(relname));
	}
	return NULL;
}

/*
 * Backfill one chunk on base node `node`: rows with id > *last_id, locked
 * FOR SHARE until the chunk's 2PC commit, so a concurrent UPDATE/DELETE of
 * them waits and then fixes the entry itself.  ON CONFLICT DO NOTHING skips
 * entries that concurrent writers already made.
 */
static ChunkResult
backfill_chunk(GsiIndex *idx, int node, const char *base_sql, const char *key_col,
			   int64 *last_id, int64 *rows)
{
	PGconn	   *base_conn = node_conn(idx, node);
	StringInfoData *keys = palloc0(sizeof(StringInfoData) * idx->nnodes);
	StringInfoData *ids = palloc0(sizeof(StringInfoData) * idx->nnodes);
	const char *values[3];
	char	   *sql;
	PGresult   *res;
	int			n;
	char	   *state;

	sql = psprintf("SELECT id, %s::text FROM %s WHERE id > $1 ORDER BY id LIMIT %d FOR SHARE",
				   key_col, base_sql, gsi_build_chunk_size);
	values[0] = psprintf(INT64_FORMAT, *last_id);
	res = run_remote(base_conn, sql, 1, values);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
	{
		state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
		if (state && (strcmp(state, "40001") == 0 || strcmp(state, "40P01") == 0 ||
					  strcmp(state, "55P03") == 0))
		{
			PQclear(res);
			return CHUNK_RETRY;
		}
		pgfdw_report_error(ERROR, res, base_conn, true, sql);
	}
	n = PQntuples(res);
	if (n == 0)
	{
		PQclear(res);
		return CHUNK_LAST;
	}
	gsi_mark_conn(base_conn, false);

	for (int i = 0; i < n; i++)
	{
		int			target;

		if (PQgetisnull(res, i, 1))
			continue;
		target = gsi_node_for_datum(gsi_key_from_text(idx, PQgetvalue(res, i, 1)),
									idx->typid, idx->collid, idx->nnodes);
		if (keys[target].data == NULL)
		{
			initStringInfo(&keys[target]);
			initStringInfo(&ids[target]);
		}
		append_array_elem(&keys[target], PQgetvalue(res, i, 1));
		append_array_elem(&ids[target], PQgetvalue(res, i, 0));
	}
	*last_id = strtoi64(PQgetvalue(res, n - 1, 0), NULL, 10);
	*rows += n;
	PQclear(res);

	/* one INSERT per index node: unnest the keys and ids of this chunk */
	sql = psprintf("INSERT INTO %s (key, id, shard) "
				   "SELECT k, i, $3 FROM unnest($1::%s[], $2::pg_catalog.int8[]) AS u(k, i) "
				   "ON CONFLICT (key, id) DO NOTHING",
				   idx->remote_table, format_type_be_qualified(idx->typid));
	for (int target = 0; target < idx->nnodes; target++)
	{
		PGconn	   *conn;

		if (keys[target].data == NULL)
			continue;
		appendStringInfoChar(&keys[target], '}');
		appendStringInfoChar(&ids[target], '}');
		values[0] = keys[target].data;
		values[1] = ids[target].data;
		values[2] = psprintf("%d", node);
		conn = node_conn(idx, target);
		gsi_mark_conn(conn, true);
		res = run_remote(conn, sql, 3, values);
		if (PQresultStatus(res) != PGRES_COMMAND_OK)
		{
			state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
			if (state && strcmp(state, "23505") == 0)
			{
				PQclear(res);
				return CHUNK_DUPLICATE;
			}
			if (state && (strcmp(state, "40001") == 0 || strcmp(state, "40P01") == 0 ||
						  strcmp(state, "55P03") == 0))
			{
				PQclear(res);
				return CHUNK_RETRY;
			}
			pgfdw_report_error(ERROR, res, conn, true, sql);
		}
		PQclear(res);
	}
	return CHUNK_DONE;
}

/* Last backfilled id on a node, from gsi.gsi_build_progress */
static int64
progress_get(const char *name, int node)
{
	Oid			argtypes[2] = {NAMEOID, INT4OID};
	Datum		args[2];
	int64		last = PG_INT64_MIN;

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(name));
	args[1] = Int32GetDatum(node);
	SPI_connect();
	if (SPI_execute_with_args("SELECT last_id, done FROM gsi.gsi_build_progress "
							  "WHERE index_name = $1 AND node = $2",
							  2, argtypes, args, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not read gsi.gsi_build_progress");
	if (SPI_processed > 0)
	{
		bool		isnull;

		if (DatumGetBool(SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 2, &isnull)))
			last = PG_INT64_MAX;
		else
			last = DatumGetInt64(SPI_getbinval(SPI_tuptable->vals[0],
											   SPI_tuptable->tupdesc, 1, &isnull));
	}
	SPI_finish();
	return last;
}

/* Record progress in the same transaction as the chunk it describes */
static void
progress_set(const char *name, int node, int64 last_id, bool done)
{
	Oid			argtypes[4] = {NAMEOID, INT4OID, INT8OID, BOOLOID};
	Datum		args[4];

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(name));
	args[1] = Int32GetDatum(node);
	args[2] = Int64GetDatum(last_id);
	args[3] = BoolGetDatum(done);
	SPI_connect();
	if (SPI_execute_with_args("INSERT INTO gsi.gsi_build_progress (index_name, node, last_id, done) "
							  "VALUES ($1, $2, $3, $4) ON CONFLICT (index_name, node) "
							  "DO UPDATE SET last_id = $3, done = $4",
							  4, argtypes, args, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not write gsi.gsi_build_progress");
	SPI_finish();
}

/* The index as stored now, or ERROR if it was dropped meanwhile */
static GsiIndex *
current_index(Oid base, const char *col, const char *name)
{
	GsiIndex   *idx = gsi_index_on_column(gsi_indexes_for_rel(base), col);

	if (idx == NULL || strcmp(idx->name, name) != 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("GSI \"%s\" was dropped during its build", name)));
	return idx;
}

/*
 * Step 3, for every node.  Each chunk commits on its own (2PC through the
 * normal commit path, so a crash mid-chunk is an ordinary in-doubt
 * transaction for the resolver).
 */
static int64
backfill(Oid base, const char *col, const char *name)
{
	int64		total = 0;
	int			nnodes = current_index(base, col, name)->nnodes;

	/* everything a chunk allocates; reset after each (1M rows = ~1,000 chunks) */
	MemoryContext chunk_cxt = AllocSetContextCreate(CurrentMemoryContext,
													"gsi backfill chunk",
													ALLOCSET_DEFAULT_SIZES);

	for (int node = 0; node < nnodes; node++)
	{
		int64		last_id = progress_get(name, node);
		int64		node_rows = 0;
		int			retries = 0;
		char	   *base_sql;
		char	   *key_col;

		if (last_id == PG_INT64_MAX)
			continue;			/* finished before a resume */

		for (;;)
		{
			GsiIndex   *idx;
			ChunkResult r;
			int64		chunk_last = last_id;
			int64		chunk_rows = 0;

			CHECK_FOR_INTERRUPTS();
			MemoryContextReset(chunk_cxt);
			MemoryContextSwitchTo(chunk_cxt);
			/* blocks DDL on the table (and gsi_drop) while the chunk runs, not writes */
			LockRelationOid(base, ShareUpdateExclusiveLock);
			idx = current_index(base, col, name);
			base_sql = base_table_sql(base, node, idx, &key_col);
			if (base_sql == NULL)
				break;			/* no partition on this node */

			r = backfill_chunk(idx, node, base_sql, key_col, &chunk_last, &chunk_rows);
			if (r == CHUNK_RETRY)
			{
				SPI_rollback();
				if (++retries > CHUNK_MAX_RETRIES)
					ereport(ERROR,
							(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
							 errmsg("gsi_create: chunk on node %d failed %d times on conflicts; call again to resume",
									node, retries)));
				pg_usleep(10000L);
				continue;
			}
			if (r == CHUNK_DUPLICATE)
			{
				/* D7.3: never leave a write-only index behind; drop it */
				SPI_rollback();
				LockRelationOid(base, ShareUpdateExclusiveLock);
				gsi_drop_index(current_index(base, col, name));
				SPI_commit();
				ereport(ERROR,
						(errcode(ERRCODE_UNIQUE_VIOLATION),
						 errmsg("could not create unique GSI \"%s\": existing rows have duplicate keys",
								name),
						 errdetail("The GSI was dropped.")));
			}

			retries = 0;
			if (r == CHUNK_LAST)
			{
				progress_set(name, node, last_id, true);
				SPI_commit();
				break;
			}
			last_id = chunk_last;
			node_rows += chunk_rows;
			progress_set(name, node, last_id, false);
			SPI_commit();
			if (gsi_build_chunk_delay > 0)
				pg_usleep((long) gsi_build_chunk_delay * 1000L);
		}
		MemoryContextSwitchTo(MemoryContextGetParent(chunk_cxt));
		ereport(NOTICE,
				(errmsg("gsi_create: node %d backfilled (" INT64_FORMAT " rows)", node, node_rows)));
		total += node_rows;
	}
	MemoryContextDelete(chunk_cxt);
	return total;
}

/* count and sum of 64-bit hashes of "key:id:shard", as numeric text */
static void
fingerprint(PGconn *conn, const char *sql, char **count, char **sum)
{
	PGresult   *res = run_remote(conn, sql, 0, NULL);

	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	*count = pstrdup(PQgetvalue(res, 0, 0));
	*sum = pstrdup(PQgetvalue(res, 0, 1));
	PQclear(res);
}

/*
 * Step 4: compare the multiset of (key, id, shard) built from the base rows
 * with the index entries: count plus sum of 64-bit hashes per side, summed
 * as numeric on the coordinator.  Avoids shipping every row; a mismatch
 * that keeps both count and hash sum equal is vanishingly unlikely.
 * Placement (entry on node hash(key) mod N) holds by construction.
 */
static void
validate(Oid base, const char *col, const char *name)
{
	GsiIndex   *idx = current_index(base, col, name);
	Datum		base_cnt = DirectFunctionCall3(numeric_in, CStringGetDatum("0"), 0, -1);
	Datum		base_sum = base_cnt;
	Datum		idx_cnt = base_cnt;
	Datum		idx_sum = base_cnt;

	for (int node = 0; node < idx->nnodes; node++)
	{
		PGconn	   *conn = node_conn(idx, node);
		char	   *key_col;
		char	   *base_sql = base_table_sql(base, node, idx, &key_col);
		char	   *c,
				   *s;

		if (base_sql)
		{
			fingerprint(conn,
						psprintf("SELECT count(*), coalesce(sum(hashtextextended("
								 "%s::text || ':' || id || ':' || %d, 0)::numeric), 0) "
								 "FROM %s WHERE %s IS NOT NULL",
								 key_col, node, base_sql, key_col),
						&c, &s);
			base_cnt = DirectFunctionCall2(numeric_add, base_cnt,
										   DirectFunctionCall3(numeric_in, CStringGetDatum(c), 0, -1));
			base_sum = DirectFunctionCall2(numeric_add, base_sum,
										   DirectFunctionCall3(numeric_in, CStringGetDatum(s), 0, -1));
		}
		fingerprint(conn,
					psprintf("SELECT count(*), coalesce(sum(hashtextextended("
							 "key::text || ':' || id || ':' || shard, 0)::numeric), 0) FROM %s",
							 idx->remote_table),
					&c, &s);
		idx_cnt = DirectFunctionCall2(numeric_add, idx_cnt,
									  DirectFunctionCall3(numeric_in, CStringGetDatum(c), 0, -1));
		idx_sum = DirectFunctionCall2(numeric_add, idx_sum,
									  DirectFunctionCall3(numeric_in, CStringGetDatum(s), 0, -1));
	}

	if (!DatumGetBool(DirectFunctionCall2(numeric_eq, base_cnt, idx_cnt)) ||
		!DatumGetBool(DirectFunctionCall2(numeric_eq, base_sum, idx_sum)))
		ereport(ERROR,
				(errcode(ERRCODE_DATA_CORRUPTED),
				 errmsg("gsi_create: validation of \"%s\" failed: %s base rows, %s entries",
						name,
						DatumGetCString(DirectFunctionCall1(numeric_out, base_cnt)),
						DatumGetCString(DirectFunctionCall1(numeric_out, idx_cnt))),
				 errhint("The GSI stays write-only; drop it with gsi_drop().")));
}

/*
 * Async GSI: wait (up to 5 min) until the outbox holds no rows for it.  Own
 * autocommit connections, so every look sees the applier's progress while
 * the caller keeps its lock.
 */
static void
wait_for_outbox(Oid base, const char *col, const char *name)
{
	GsiIndex   *idx = current_index(base, col, name);
	const char *sql = "SELECT 1 FROM public.gsi_outbox WHERE index_name = $1 LIMIT 1";
	const char *values[1] = {name};

	for (int i = 0; i < 3000; i++)
	{
		bool		empty = true;

		for (int node = 0; node < idx->nnodes && empty; node++)
		{
			PGconn	   *conn = gsi_connect_raw(GetForeignServer(idx->servers[node]),
											   GetUserMapping(GetUserId(), idx->servers[node]));
			PGresult   *res = PQexecParams(conn, sql, 1, NULL, values, NULL, NULL, 0);
			bool		ok = PQresultStatus(res) == PGRES_TUPLES_OK;

			empty = ok && PQntuples(res) == 0;
			PQclear(res);
			gsi_disconnect_raw(conn);
			if (!ok)
				ereport(ERROR,
						(errmsg("gsi_create: could not read the outbox of node %d", node)));
		}
		if (empty)
			return;
		CHECK_FOR_INTERRUPTS();
		pg_usleep(100000L);
	}
	ereport(ERROR,
			(errmsg("gsi_create: outbox of \"%s\" did not drain; is the applier running?", name)));
}

/*
 * Step 1a: the index tables on every node.  Committed before the catalog row
 * exists: the coordinator commits locally before COMMIT PREPARED reaches the
 * nodes (connection.c), so in one transaction a writer could see the row and
 * not yet the table.  Without a catalog row nothing uses these tables, so
 * leftovers of a build that failed before step 1b are dropped.
 */
static void
create_index_tables(Relation rel, const char *col, const char *name,
					bool is_unique, bool is_async)
{
	MemoryContext cxt = CurrentMemoryContext;
	GsiIndex   *idx = palloc0(sizeof(GsiIndex));

	idx->name = pstrdup(name);
	idx->column = pstrdup(col);
	idx->typid = get_atttype(RelationGetRelid(rel),
							 get_attnum(RelationGetRelid(rel), col));
	idx->remote_table = psprintf("public.%s",
								 quote_identifier(psprintf("gsi_%s", name)));
	SPI_connect();
	idx->servers = gsi_load_nodes(&idx->nnodes, cxt);
	SPI_finish();

	gsi_exec_on_all_nodes(idx, psprintf("DROP TABLE IF EXISTS %s", idx->remote_table));
	gsi_exec_on_all_nodes(idx,
						  psprintf("CREATE TABLE %s (key %s NOT NULL, id bigint NOT NULL, "
								   "shard int NOT NULL, PRIMARY KEY (key, id))",
								   idx->remote_table,
								   format_type_be_qualified(idx->typid)));
	if (is_unique)
		gsi_exec_on_all_nodes(idx, psprintf("CREATE UNIQUE INDEX ON %s (key)",
											idx->remote_table));
	if (is_async)
		gsi_exec_on_all_nodes(idx,
							  "CREATE TABLE IF NOT EXISTS public.gsi_outbox ("
							  "seq bigserial PRIMARY KEY, index_name text NOT NULL, "
							  "old_key text, new_key text, id bigint NOT NULL, "
							  "created_at timestamptz NOT NULL DEFAULT now())");
}

/* Step 1b: the catalog row in status 'write-only'; writes maintain it from now on */
static void
insert_write_only(Oid base, const char *col, const char *name,
				  bool is_unique, const char *mode)
{
	Oid			argtypes[5] = {NAMEOID, REGCLASSOID, NAMEOID, BOOLOID, TEXTOID};
	Datum		args[5];

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(name));
	args[1] = ObjectIdGetDatum(base);
	args[2] = DirectFunctionCall1(namein, CStringGetDatum(col));
	args[3] = BoolGetDatum(is_unique);
	args[4] = CStringGetTextDatum(mode);
	SPI_connect();
	if (SPI_execute_with_args("INSERT INTO gsi.gsi_index "
							  "(index_name, base_table, column_name, is_unique, mode, status) "
							  "VALUES ($1, $2, $3, $4, $5, 'write-only')",
							  5, argtypes, args, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not insert into gsi.gsi_index");
	SPI_finish();

	/* cached plans of the table were made without this GSI: replan them */
	CacheInvalidateRelcacheByRelid(base);
}

static void
set_ready(const char *name)
{
	Oid			argtypes[1] = {NAMEOID};
	Datum		args[1];

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(name));
	SPI_connect();
	if (SPI_execute_with_args("UPDATE gsi.gsi_index SET status = 'ready' WHERE index_name = $1",
							  1, argtypes, args, NULL, false, 0) != SPI_OK_UPDATE)
		elog(ERROR, "could not update gsi.gsi_index");
	if (SPI_execute_with_args("DELETE FROM gsi.gsi_build_progress WHERE index_name = $1",
							  1, argtypes, args, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not delete from gsi.gsi_build_progress");
	SPI_finish();
}

/*
 * CALL gsi_create(base regclass, col name, is_unique bool DEFAULT false,
 *				   mode text DEFAULT 'sync')
 *
 * One GSI per column.  A procedure because every step must commit before
 * the next one; like CREATE INDEX CONCURRENTLY it can't run inside a
 * transaction block.
 */
Datum
gsi_create(PG_FUNCTION_ARGS)
{
	Oid			base;
	char	   *col;
	bool		is_unique;
	char	   *mode;
	char	   *name;
	Relation	rel;
	GsiIndex   *existing;
	int64		rows;

	if (fcinfo->context == NULL || !IsA(fcinfo->context, CallContext) ||
		((CallContext *) fcinfo->context)->atomic)
		ereport(ERROR,
				(errcode(ERRCODE_ACTIVE_SQL_TRANSACTION),
				 errmsg("gsi_create cannot run inside a transaction block"),
				 errhint("Run CALL gsi_create(...) on its own.")));
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2) || PG_ARGISNULL(3))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("gsi_create arguments must not be NULL")));
	base = PG_GETARG_OID(0);
	col = pstrdup(NameStr(*PG_GETARG_NAME(1)));
	is_unique = PG_GETARG_BOOL(2);
	mode = text_to_cstring(PG_GETARG_TEXT_PP(3));

	if (strcmp(mode, "sync") != 0 && strcmp(mode, "async") != 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("mode must be 'sync' or 'async'")));
	/* D4.3: a duplicate would only show up after both rows committed */
	if (is_unique && strcmp(mode, "async") == 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("a unique GSI must be sync")));

	SPI_connect_ext(SPI_OPT_NONATOMIC);

	/* writes go on; DDL on the table and other gsi_create/gsi_drop wait */
	rel = table_open(base, ShareUpdateExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a partitioned table",
						RelationGetRelationName(rel))));
	if (get_attnum(base, col) == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist", col)));
	if (get_atttype(base, get_attnum(base, "id")) != INT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GSI tables need a bigint column \"id\"")));
	name = psprintf("%s_%s", RelationGetRelationName(rel), col);

	existing = gsi_index_on_column(gsi_indexes_for_rel(base), col);
	if (existing != NULL && existing->is_ready)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("column \"%s\" of table \"%s\" already has a GSI",
						col, RelationGetRelationName(rel))));
	if (existing != NULL)
	{
		if (existing->is_unique != is_unique || existing->is_async != (strcmp(mode, "async") == 0))
			ereport(ERROR,
					(errcode(ERRCODE_DUPLICATE_OBJECT),
					 errmsg("an unfinished build of GSI \"%s\" with other options exists", name),
					 errhint("Drop it with gsi_drop() first.")));
		ereport(NOTICE, (errmsg("gsi_create: resuming the build of \"%s\"", name)));
	}
	else
	{
		create_index_tables(rel, col, name, is_unique, strcmp(mode, "async") == 0);
		table_close(rel, NoLock);
		SPI_commit();
		/* a build that started meanwhile fails here on the catalog's keys */
		rel = table_open(base, ShareUpdateExclusiveLock);
		insert_write_only(base, col, name, is_unique, mode);
	}
	table_close(rel, NoLock);
	SPI_commit();

	/* step 2: writers that might not know the index yet must finish */
	{
		LOCKTAG		tag;

		SET_LOCKTAG_RELATION(tag, MyDatabaseId, base);
		WaitForLockers(tag, ShareLock, true);
	}

	rows = backfill(base, col, name);

	/*
	 * Steps 4-5 hold ShareLock: new writes wait (reads don't), and writes in
	 * flight finish their 2PC first.  Without a global snapshot, a write
	 * committing between the per-node reads could make a correct index look
	 * wrong.  Only for the validation, a few seconds per million rows.
	 */
	LockRelationOid(base, ShareLock);
	if (strcmp(mode, "async") == 0)
		wait_for_outbox(base, col, name);
	validate(base, col, name);
	set_ready(name);
	CacheInvalidateRelcacheByRelid(base);	/* cached SELECTs may use it now */
	SPI_commit();

	/* fresh statistics for the cost model; failing here leaves a ready GSI */
	if (SPI_execute(psprintf("ANALYZE %s",
							 quote_qualified_identifier(get_namespace_name(get_rel_namespace(base)),
														get_rel_name(base))),
					false, 0) != SPI_OK_UTILITY)
		elog(ERROR, "could not ANALYZE the table");
	SPI_commit();

	ereport(NOTICE,
			(errmsg("gsi_create: \"%s\" ready (" INT64_FORMAT " rows backfilled)", name, rows)));
	SPI_finish();
	PG_RETURN_VOID();
}

void
gsi_build_init(void)
{
	DefineCustomIntVariable("gsi.build_chunk_size",
							"Rows per backfill transaction of gsi_create.",
							NULL,
							&gsi_build_chunk_size,
							10000, 1, 1000000,
							PGC_USERSET,
							0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gsi.build_chunk_delay",
							"Pause between backfill transactions of gsi_create (throttling).",
							NULL,
							&gsi_build_chunk_delay,
							0, 0, 60000,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
}
