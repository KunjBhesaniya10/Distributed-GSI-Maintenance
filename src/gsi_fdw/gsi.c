/*-------------------------------------------------------------------------
 *
 * gsi.c
 *		  GSI catalog access, placement, and synchronous index maintenance
 *
 * Catalog tables live in schema gsi on the coordinator:
 *	gsi.gsi_node(node_id, server_name)	maps node ids 0..N-1 to servers
 *	gsi.gsi_index(index_name, ...)		one row per GSI
 *
 * Each data node holds public.gsi_<index_name>(key, id, shard).
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/partition.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "nodes/makefuncs.h"
#include "optimizer/appendinfo.h"
#include "parser/parsetree.h"
#include "partitioning/partbounds.h"
#include "postgres_fdw.h"
#include "gsi.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

int			gsi_remote_lock_timeout = 5000;
static char *gsi_fail_point_name = NULL;

PG_FUNCTION_INFO_V1(gsi_create);
PG_FUNCTION_INFO_V1(gsi_drop);
PG_FUNCTION_INFO_V1(gsi_node_for);

void
gsi_init(void)
{
	DefineCustomIntVariable("gsi.remote_lock_timeout",
							"Sets lock_timeout on remote sessions; bounds cross-node deadlocks.",
							NULL,
							&gsi_remote_lock_timeout,
							5000, 0, INT_MAX,
							PGC_USERSET,
							GUC_UNIT_MS,
							NULL, NULL, NULL);

	DefineCustomStringVariable("gsi.fail_point",
							   "Crashes the backend at the named commit step (testing only).",
							   NULL,
							   &gsi_fail_point_name,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	gsi_resolver_init();
	gsi_scan_init();
	MarkGUCPrefixReserved("gsi");
}

/*
 * Simulate a crash at a commit step.  abort() makes the postmaster run crash
 * recovery, exactly like a real backend crash.
 */
void
gsi_fail_point(const char *name)
{
	if (gsi_fail_point_name && strcmp(gsi_fail_point_name, name) == 0)
	{
		ereport(LOG,
				(errmsg("gsi fail point \"%s\" reached, aborting process", name)));
		abort();
	}
}

/* Server OIDs of all nodes, ordered by node id. Caller must be in SPI. */
Oid *
gsi_load_nodes(int *nnodes, MemoryContext cxt)
{
	Oid		   *servers;

	if (SPI_execute("SELECT s.oid FROM gsi.gsi_node n "
					"JOIN pg_catalog.pg_foreign_server s ON s.srvname = n.server_name "
					"ORDER BY n.node_id", true, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read gsi.gsi_node");
	if (SPI_processed == 0)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("gsi.gsi_node is empty")));

	*nnodes = SPI_processed;
	servers = MemoryContextAlloc(cxt, sizeof(Oid) * SPI_processed);
	for (uint64 i = 0; i < SPI_processed; i++)
	{
		bool		isnull;

		servers[i] = DatumGetObjectId(SPI_getbinval(SPI_tuptable->vals[i],
													SPI_tuptable->tupdesc,
													1, &isnull));
	}
	return servers;
}

/*
 * Return the sync GSIs on relid (a partitioned table or one of its
 * partitions), ordered by index name, or NIL.
 */
List *
gsi_indexes_for_rel(Oid relid)
{
	MemoryContext cxt = CurrentMemoryContext;
	List	   *indexes = NIL;
	Oid		   *servers = NULL;
	int			nnodes = 0;
	Oid			argtypes[1] = {REGCLASSOID};
	Datum		args[1];

	if (!OidIsValid(get_namespace_oid("gsi", true)))
		return NIL;
	if (get_rel_relispartition(relid))
		relid = get_partition_parent(relid, false);
	args[0] = ObjectIdGetDatum(relid);

	/* not read_only: also see catalog rows written earlier in this xact */
	SPI_connect();
	if (SPI_execute_with_args("SELECT i.index_name, i.column_name, i.is_unique, "
							  "i.status = 'ready', a.atttypid, a.attcollation "
							  "FROM gsi.gsi_index i JOIN pg_catalog.pg_attribute a "
							  "ON a.attrelid = i.base_table AND a.attname = i.column_name "
							  "WHERE i.base_table = $1 AND i.mode = 'sync' "
							  "ORDER BY i.index_name",
							  1, argtypes, args, NULL, false, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read gsi.gsi_index");

	if (SPI_processed > 0)
	{
		SPITupleTable *tuptable = SPI_tuptable;
		uint64		nrows = SPI_processed;

		servers = gsi_load_nodes(&nnodes, cxt);
		for (uint64 r = 0; r < nrows; r++)
		{
			HeapTuple	tup = tuptable->vals[r];
			TupleDesc	desc = tuptable->tupdesc;
			bool		isnull;
			MemoryContext oldcxt = MemoryContextSwitchTo(cxt);
			GsiIndex   *idx = palloc0(sizeof(GsiIndex));

			idx->name = pstrdup(NameStr(*DatumGetName(SPI_getbinval(tup, desc, 1, &isnull))));
			idx->column = pstrdup(NameStr(*DatumGetName(SPI_getbinval(tup, desc, 2, &isnull))));
			idx->is_unique = DatumGetBool(SPI_getbinval(tup, desc, 3, &isnull));
			idx->is_ready = DatumGetBool(SPI_getbinval(tup, desc, 4, &isnull));
			idx->typid = DatumGetObjectId(SPI_getbinval(tup, desc, 5, &isnull));
			idx->collid = DatumGetObjectId(SPI_getbinval(tup, desc, 6, &isnull));
			idx->remote_table = psprintf("public.%s",
										 quote_identifier(psprintf("gsi_%s", idx->name)));
			idx->servers = servers;
			idx->nnodes = nnodes;
			indexes = lappend(indexes, idx);
			MemoryContextSwitchTo(oldcxt);
		}
	}
	SPI_finish();

	return indexes;
}

/*
 * Index placement: the type's extended hash with the hash-partitioning seed,
 * mod N.  Every component (write, read, checker) must use this function.
 */
int
gsi_node_for_datum(Datum value, Oid typid, Oid collid, int nnodes)
{
	TypeCacheEntry *tce = lookup_type_cache(typid, TYPECACHE_HASH_EXTENDED_PROC_FINFO);
	uint64		h;

	if (!OidIsValid(tce->hash_extended_proc_finfo.fn_oid))
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_FUNCTION),
				 errmsg("type %s has no extended hash function", format_type_be(typid))));

	h = DatumGetUInt64(FunctionCall2Coll(&tce->hash_extended_proc_finfo, collid,
										 value, UInt64GetDatum(HASH_PARTITION_SEED)));
	return (int) (h % (uint64) nnodes);
}

int
gsi_server_node(GsiIndex *idx, Oid serverid)
{
	for (int i = 0; i < idx->nnodes; i++)
		if (idx->servers[i] == serverid)
			return i;
	elog(ERROR, "foreign server %u is not listed in gsi.gsi_node", serverid);
	return -1;					/* keep compiler quiet */
}

static void
add_junk_column(PlannerInfo *root, Index rtindex, Relation rel,
				const char *colname, const char *junkname)
{
	AttrNumber	attnum = get_attnum(RelationGetRelid(rel), colname);
	Form_pg_attribute attr;

	if (attnum == InvalidAttrNumber)
		elog(ERROR, "column \"%s\" not found in \"%s\"",
			 colname, RelationGetRelationName(rel));
	attr = TupleDescAttr(RelationGetDescr(rel), attnum - 1);
	add_row_identity_var(root,
						 makeVar(rtindex, attnum, attr->atttypid, attr->atttypmod,
								 attr->attcollation, 0),
						 rtindex, junkname);
}

/* Junk column holding one GSI's old key */
static char *
old_key_junk_name(GsiIndex *idx)
{
	return psprintf("gsi_old_key_%s", idx->name);
}

/*
 * UPDATE/DELETE fetch the old key of every GSI and the old id, so the old
 * entries can be removed.
 */
void
gsi_add_update_targets(PlannerInfo *root, Index rtindex, Relation rel,
					   List *indexes)
{
	ListCell   *lc;

	foreach(lc, indexes)
	{
		GsiIndex   *idx = lfirst(lc);

		add_junk_column(root, rtindex, rel, idx->column, old_key_junk_name(idx));
	}
	add_junk_column(root, rtindex, rel, "id", "gsi_old_id");
}

/*
 * May postgres_fdw push this UPDATE/DELETE down as one remote statement?
 * Only if it can't change an index entry: the table has no GSI, or it's an
 * UPDATE that sets no indexed column and not id.  Otherwise the per-row
 * path must run so the indexes are maintained.
 */
bool
gsi_direct_modify_ok(PlannerInfo *root, Index rtindex, CmdType operation)
{
	Oid			relid = planner_rt_fetch(rtindex, root)->relid;
	List	   *indexes = gsi_indexes_for_rel(relid);
	List	   *tlist = NIL;
	List	   *update_attrs = NIL;
	ListCell   *lc;

	if (indexes == NIL)
		return true;
	if (operation != CMD_UPDATE)
		return false;
	get_translated_update_targetlist(root, rtindex, &tlist, &update_attrs);
	if (list_member_int(update_attrs, get_attnum(relid, "id")))
		return false;
	foreach(lc, indexes)
	{
		GsiIndex   *idx = lfirst(lc);

		if (list_member_int(update_attrs, get_attnum(relid, idx->column)))
			return false;
	}
	return true;
}

/* Write state for every GSI on rel, or NIL if it has none. */
List *
gsi_begin_modify(Relation rel, CmdType operation, Plan *subplan,
				 List *target_attrs)
{
	Oid			relid = RelationGetRelid(rel);
	List	   *indexes = gsi_indexes_for_rel(relid);
	List	   *states = NIL;
	AttrNumber	id_attno = get_attnum(relid, "id");
	AttrNumber	old_id_junk = InvalidAttrNumber;
	ListCell   *lc;

	if (indexes == NIL)
		return NIL;

	if (operation == CMD_UPDATE || operation == CMD_DELETE)
	{
		old_id_junk = ExecFindJunkAttributeInTlist(subplan->targetlist,
												   "gsi_old_id");
		if (!AttributeNumberIsValid(old_id_junk))
			elog(ERROR, "could not find GSI junk column \"gsi_old_id\"");
	}

	/* foreign partitions don't move rows, so a new id could land on the wrong shard */
	if (operation == CMD_UPDATE && list_member_int(target_attrs, id_attno))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("cannot update column \"id\" of a table with a GSI")));

	foreach(lc, indexes)
	{
		GsiIndex   *idx = lfirst(lc);
		GsiModifyState *gs = palloc0(sizeof(GsiModifyState));
		Oid			typoutput;
		bool		typisvarlena;

		gs->idx = idx;
		gs->base_node = gsi_server_node(idx, GetForeignTable(relid)->serverid);
		gs->key_attno = get_attnum(relid, idx->column);
		gs->id_attno = id_attno;
		gs->old_id_junk = old_id_junk;
		getTypeOutputInfo(idx->typid, &typoutput, &typisvarlena);
		fmgr_info(typoutput, &gs->key_out);

		if (operation == CMD_UPDATE || operation == CMD_DELETE)
		{
			gs->old_key_junk = ExecFindJunkAttributeInTlist(subplan->targetlist,
															old_key_junk_name(idx));
			if (!AttributeNumberIsValid(gs->old_key_junk))
				elog(ERROR, "could not find GSI junk column for \"%s\"", idx->name);
		}
		if (operation == CMD_UPDATE)
			gs->key_updated = list_member_int(target_attrs, gs->key_attno);
		states = lappend(states, gs);
	}

	/* The coordinator's commit record is the 2PC decision; make sure it exists */
	(void) GetTopFullTransactionId();

	return states;
}

static char *
key_text(GsiModifyState *gs, Datum value, int *node)
{
	*node = gsi_node_for_datum(value, gs->idx->typid, gs->idx->collid,
							   gs->idx->nnodes);
	return OutputFunctionCall(&gs->key_out, value);
}

/*
 * Record one GSI's entries to delete/insert for one row.  Done before the
 * base write because RETURNING replaces the slot contents.  For UPDATE the
 * slot holds only assigned columns, so unassigned ones come from the junk
 * (old) values.
 */
static void
capture_one(GsiModifyState *gs, CmdType operation,
			TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	Datum		d;
	bool		isnull;

	gs->old_key = gs->new_key = NULL;

	if (operation != CMD_INSERT)
	{
		d = ExecGetJunkAttribute(planSlot, gs->old_key_junk, &isnull);
		if (!isnull)
			gs->old_key = key_text(gs, d, &gs->old_node);
		gs->old_id = DatumGetInt64(ExecGetJunkAttribute(planSlot, gs->old_id_junk,
														&isnull));
	}
	if (operation == CMD_DELETE)
		return;

	if (operation == CMD_INSERT || gs->key_updated)
	{
		d = slot_getattr(slot, gs->key_attno, &isnull);
		if (!isnull)
			gs->new_key = key_text(gs, d, &gs->new_node);
	}
	else if (gs->old_key)
	{
		gs->new_key = gs->old_key;
		gs->new_node = gs->old_node;
	}

	if (operation == CMD_INSERT)
		gs->new_id = DatumGetInt64(slot_getattr(slot, gs->id_attno, &isnull));
	else
		gs->new_id = gs->old_id;

	/* UPDATE that keeps the key leaves the entry alone */
	if (operation == CMD_UPDATE && gs->old_key && gs->new_key &&
		strcmp(gs->old_key, gs->new_key) == 0)
		gs->old_key = gs->new_key = NULL;
}

/* Record the entries to delete/insert in every GSI for one row. */
void
gsi_capture_row(List *states, CmdType operation,
				TupleTableSlot *slot, TupleTableSlot *planSlot)
{
	ListCell   *lc;

	foreach(lc, states)
		capture_one(lfirst(lc), operation, slot, planSlot);
}

/* Run one index statement on the node's connection, inside its remote xact. */
static void
send_index_op(GsiModifyState *gs, int node, const char *key, int64 id,
			  bool insert)
{
	UserMapping *user = GetUserMapping(GetUserId(), gs->idx->servers[node]);
	PGconn	   *conn = GetConnection(user, false, NULL);
	const char *values[3];
	char	   *sql;
	PGresult   *res;

	gsi_mark_conn(conn, true);

	values[0] = key;
	values[1] = psprintf(INT64_FORMAT, id);
	values[2] = psprintf("%d", gs->base_node);
	if (insert)
		sql = psprintf("INSERT INTO %s (key, id, shard) VALUES ($1, $2, $3)",
					   gs->idx->remote_table);
	else
		sql = psprintf("DELETE FROM %s WHERE key = $1 AND id = $2",
					   gs->idx->remote_table);

	if (!PQsendQueryParams(conn, sql, insert ? 3 : 2, NULL, values,
						   NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);

	/*
	 * We hold the base row's lock, so its committed entry exists.  Missing it
	 * means this node's REPEATABLE READ snapshot predates that commit.
	 */
	if (!insert && strcmp(PQcmdTuples(res), "1") != 0)
	{
		PQclear(res);
		ereport(ERROR,
				(errcode(ERRCODE_T_R_SERIALIZATION_FAILURE),
				 errmsg("could not serialize access due to concurrent update of GSI \"%s\"",
						gs->idx->name)));
	}
	PQclear(res);
}

/* Apply the captured entries of every GSI after the base write succeeded. */
void
gsi_apply_row(List *states)
{
	ListCell   *lc;

	foreach(lc, states)
	{
		GsiModifyState *gs = lfirst(lc);

		if (gs->old_key)
			send_index_op(gs, gs->old_node, gs->old_key, gs->old_id, false);
		if (gs->new_key)
			send_index_op(gs, gs->new_node, gs->new_key, gs->new_id, true);
	}
}

/* Run a DDL command on every node, as part of the distributed transaction. */
static void
exec_on_all_nodes(GsiIndex *idx, const char *sql)
{
	for (int i = 0; i < idx->nnodes; i++)
	{
		PGconn	   *conn;

		conn = GetConnection(GetUserMapping(GetUserId(), idx->servers[i]),
							 false, NULL);
		gsi_mark_conn(conn, true);
		do_sql_command(conn, sql);
	}
}

/* The GSI on column col, or NULL */
static GsiIndex *
index_on_column(List *indexes, const char *col)
{
	ListCell   *lc;

	foreach(lc, indexes)
	{
		GsiIndex   *idx = lfirst(lc);

		if (strcmp(idx->column, col) == 0)
			return idx;
	}
	return NULL;
}

/*
 * gsi_create(base regclass, col name, is_unique bool) returns text
 *
 * A table can have one GSI per column.  v1 builds the index only on an
 * empty table (no backfill).
 */
Datum
gsi_create(PG_FUNCTION_ARGS)
{
	Oid			base = PG_GETARG_OID(0);
	char	   *col = NameStr(*PG_GETARG_NAME(1));
	bool		is_unique = PG_GETARG_BOOL(2);
	Relation	rel;
	AttrNumber	attnum;
	char	   *name;
	char	   *relname;
	GsiIndex   *idx;
	Oid			argtypes[4] = {NAMEOID, REGCLASSOID, NAMEOID, BOOLOID};
	Datum		args[4];

	rel = table_open(base, AccessExclusiveLock);
	if (rel->rd_rel->relkind != RELKIND_PARTITIONED_TABLE)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a partitioned table",
						RelationGetRelationName(rel))));
	attnum = get_attnum(base, col);
	if (attnum == InvalidAttrNumber)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_COLUMN),
				 errmsg("column \"%s\" does not exist", col)));
	if (get_atttype(base, get_attnum(base, "id")) != INT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("GSI tables need a bigint column \"id\"")));
	if (index_on_column(gsi_indexes_for_rel(base), col) != NULL)
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("column \"%s\" of table \"%s\" already has a GSI",
						col, RelationGetRelationName(rel))));

	relname = quote_qualified_identifier(get_namespace_name(RelationGetNamespace(rel)),
										 RelationGetRelationName(rel));
	name = psprintf("%s_%s", RelationGetRelationName(rel), col);

	SPI_connect();
	if (SPI_execute(psprintf("SELECT 1 FROM %s LIMIT 1", relname), true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not scan \"%s\"", relname);
	if (SPI_processed > 0)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("gsi_create requires an empty table")));

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(name));
	args[1] = ObjectIdGetDatum(base);
	args[2] = DirectFunctionCall1(namein, CStringGetDatum(col));
	args[3] = BoolGetDatum(is_unique);
	if (SPI_execute_with_args("INSERT INTO gsi.gsi_index "
							  "(index_name, base_table, column_name, is_unique) "
							  "VALUES ($1, $2, $3, $4)",
							  4, argtypes, args, NULL, false, 0) != SPI_OK_INSERT)
		elog(ERROR, "could not insert into gsi.gsi_index");
	SPI_finish();

	idx = index_on_column(gsi_indexes_for_rel(base), col);
	exec_on_all_nodes(idx,
					  psprintf("CREATE TABLE %s (key %s NOT NULL, id bigint NOT NULL, "
							   "shard int NOT NULL, PRIMARY KEY (key, id))",
							   idx->remote_table,
							   format_type_be_qualified(idx->typid)));
	if (is_unique)
		exec_on_all_nodes(idx, psprintf("CREATE UNIQUE INDEX ON %s (key)",
										idx->remote_table));

	table_close(rel, NoLock);
	PG_RETURN_TEXT_P(cstring_to_text(name));
}

/*
 * gsi_drop(base regclass, col name DEFAULT NULL)
 *
 * Drops the GSI on col, or every GSI of the table when col is NULL.
 */
Datum
gsi_drop(PG_FUNCTION_ARGS)
{
	Oid			base;
	List	   *indexes;
	Oid			argtypes[1] = {NAMEOID};
	Relation	rel;
	ListCell   *lc;

	if (PG_ARGISNULL(0))
		ereport(ERROR,
				(errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
				 errmsg("table must not be NULL")));
	base = PG_GETARG_OID(0);

	rel = table_open(base, AccessExclusiveLock);
	indexes = gsi_indexes_for_rel(base);
	if (!PG_ARGISNULL(1))
	{
		char	   *col = NameStr(*PG_GETARG_NAME(1));
		GsiIndex   *idx = index_on_column(indexes, col);

		if (idx == NULL)
			ereport(ERROR,
					(errcode(ERRCODE_UNDEFINED_OBJECT),
					 errmsg("column \"%s\" of table \"%s\" has no GSI",
							col, RelationGetRelationName(rel))));
		indexes = list_make1(idx);
	}
	else if (indexes == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("table \"%s\" has no GSI", RelationGetRelationName(rel))));

	foreach(lc, indexes)
	{
		GsiIndex   *idx = lfirst(lc);
		Datum		args[1];

		args[0] = DirectFunctionCall1(namein, CStringGetDatum(idx->name));
		SPI_connect();
		if (SPI_execute_with_args("DELETE FROM gsi.gsi_index WHERE index_name = $1",
								  1, argtypes, args, NULL, false, 0) != SPI_OK_DELETE)
			elog(ERROR, "could not delete from gsi.gsi_index");
		SPI_finish();

		exec_on_all_nodes(idx, psprintf("DROP TABLE IF EXISTS %s", idx->remote_table));
	}

	table_close(rel, NoLock);
	PG_RETURN_VOID();
}

/* gsi_node_for(anyelement) returns the node id holding that key's entries */
Datum
gsi_node_for(PG_FUNCTION_ARGS)
{
	Oid			typid = get_fn_expr_argtype(fcinfo->flinfo, 0);
	int			nnodes;

	SPI_connect();
	(void) gsi_load_nodes(&nnodes, CurrentMemoryContext);
	SPI_finish();

	PG_RETURN_INT32(gsi_node_for_datum(PG_GETARG_DATUM(0), typid,
									   PG_GET_COLLATION(), nnodes));
}
