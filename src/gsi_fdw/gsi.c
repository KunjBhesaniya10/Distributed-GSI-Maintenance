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
#include "utils/inval.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/typcache.h"

int			gsi_remote_lock_timeout = 5000;
bool		gsi_trace = false;
static char *gsi_fail_point_name = NULL;
static char *gsi_pause_point_name = NULL;
static int	gsi_pause_seconds = 20;

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

	DefineCustomBoolVariable("gsi.trace",
							 "Reports each 2PC step of GSI transactions as a NOTICE.",
							 NULL,
							 &gsi_trace,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);

	DefineCustomStringVariable("gsi.pause_point",
							   "Sleeps gsi.pause_seconds at the named commit step (demo only).",
							   NULL,
							   &gsi_pause_point_name,
							   "",
							   PGC_SUSET,
							   0,
							   NULL, NULL, NULL);

	DefineCustomIntVariable("gsi.pause_seconds",
							"Length of the gsi.pause_point sleep.",
							NULL,
							&gsi_pause_seconds,
							20, 0, 3600,
							PGC_SUSET,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	gsi_resolver_init();
	gsi_scan_init();
	gsi_build_init();
	gsi_async_init();
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

/*
 * Stop at a commit step so a node can be killed by hand in the window.  No
 * CHECK_FOR_INTERRUPTS: some steps run after the commit decision, where an
 * ERROR is not allowed.
 */
void
gsi_pause_point(const char *name)
{
	if (gsi_pause_point_name && strcmp(gsi_pause_point_name, name) == 0)
	{
		ereport(NOTICE,
				(errmsg("gsi: paused at \"%s\" for %d s", name, gsi_pause_seconds)));
		pg_usleep((long) gsi_pause_seconds * 1000000L);
		ereport(NOTICE, (errmsg("gsi: resuming after \"%s\"", name)));
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

/* GsiIndex rows: name, column, unique, ready, async, type, collation */
#define GSI_INDEX_COLUMNS \
	"SELECT i.index_name, i.column_name, i.is_unique, i.status = 'ready', " \
	"i.mode = 'async', a.atttypid, a.attcollation " \
	"FROM gsi.gsi_index i JOIN pg_catalog.pg_attribute a " \
	"ON a.attrelid = i.base_table AND a.attname = i.column_name "

/*
 * Run a gsi.gsi_index query and build a GsiIndex per row, in cxt.  Caller is
 * in SPI.
 *
 * The catalog is read with the latest snapshot, not the transaction's: a
 * REPEATABLE READ writer must see an index created after its snapshot was
 * taken, or it would skip maintaining an index being built (gsi_build.c).
 * Not read_only, so rows written earlier in this transaction are seen too.
 */
static List *
read_indexes(const char *sql, Oid argtype, Datum arg, MemoryContext cxt)
{
	List	   *indexes = NIL;
	SPIPlanPtr	plan = SPI_prepare(sql, 1, &argtype);
	Oid		   *servers;
	int			nnodes;

	if (plan == NULL ||
		SPI_execute_snapshot(plan, &arg, NULL, GetLatestSnapshot(),
							 InvalidSnapshot, false, false, 0) != SPI_OK_SELECT)
		elog(ERROR, "could not read gsi.gsi_index");
	if (SPI_processed == 0)
		return NIL;

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
			idx->is_async = DatumGetBool(SPI_getbinval(tup, desc, 5, &isnull));
			idx->typid = DatumGetObjectId(SPI_getbinval(tup, desc, 6, &isnull));
			idx->collid = DatumGetObjectId(SPI_getbinval(tup, desc, 7, &isnull));
			idx->remote_table = psprintf("public.%s",
										 quote_identifier(psprintf("gsi_%s", idx->name)));
			idx->servers = servers;
			idx->nnodes = nnodes;
			indexes = lappend(indexes, idx);
			MemoryContextSwitchTo(oldcxt);
		}
	}
	return indexes;
}

/*
 * Return the GSIs on relid (a partitioned table or one of its partitions),
 * sync and async, any status, ordered by index name, or NIL.
 */
List *
gsi_indexes_for_rel(Oid relid)
{
	MemoryContext cxt = CurrentMemoryContext;
	List	   *indexes;

	if (!OidIsValid(get_namespace_oid("gsi", true)))
		return NIL;
	if (get_rel_relispartition(relid))
		relid = get_partition_parent(relid, false);

	SPI_connect();
	indexes = read_indexes(GSI_INDEX_COLUMNS
						   "WHERE i.base_table = $1 ORDER BY i.index_name",
						   REGCLASSOID, ObjectIdGetDatum(relid), cxt);
	SPI_finish();
	return indexes;
}

/* The GSI named name, or NULL.  Caller is in a transaction. */
GsiIndex *
gsi_index_by_name(const char *name)
{
	MemoryContext cxt = CurrentMemoryContext;
	List	   *indexes;

	SPI_connect();
	indexes = read_indexes(GSI_INDEX_COLUMNS "WHERE i.index_name = $1",
						   NAMEOID, DirectFunctionCall1(namein, CStringGetDatum(name)),
						   cxt);
	SPI_finish();
	return indexes ? linitial(indexes) : NULL;
}

/* Key value from its text form (as stored in the outbox) */
Datum
gsi_key_from_text(GsiIndex *idx, const char *text)
{
	Oid			typinput;
	Oid			typioparam;

	getTypeInputInfo(idx->typid, &typinput, &typioparam);
	return OidInputFunctionCall(typinput, (char *) text, typioparam, -1);
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
		List	   *planned = NIL;

		/*
		 * Planned before a build's write-only catalog row existed: no junk
		 * columns for that index.  This writer has held its lock since
		 * planning, so gsi_create waits for it before backfilling and the
		 * backfill covers its rows.  A ready index must be in the plan.
		 */
		foreach(lc, indexes)
		{
			GsiIndex   *idx = lfirst(lc);

			if (AttributeNumberIsValid(ExecFindJunkAttributeInTlist(subplan->targetlist,
																	old_key_junk_name(idx))))
				planned = lappend(planned, idx);
			else if (idx->is_ready)
				elog(ERROR, "could not find GSI junk column for \"%s\"", idx->name);
		}
		indexes = planned;
		if (indexes == NIL)
			return NIL;

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
			gs->old_key_junk = ExecFindJunkAttributeInTlist(subplan->targetlist,
															old_key_junk_name(idx));
		if (operation == CMD_UPDATE)
			gs->key_updated = list_member_int(target_attrs, gs->key_attno);
		states = lappend(states, gs);
	}

	/*
	 * No coordinator xid here: only a 2PC commit needs one (its commit record
	 * is the decision), and gsi_log_xid() assigns it at pre-commit.  A write
	 * that only fed async GSIs on one node then commits without a local
	 * commit record and its WAL flush.
	 */
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

/*
 * One outbox row on the base node, in the base write's remote transaction:
 * the applier later deletes old_key's entry and inserts new_key's (either
 * may be NULL).
 */
static void
send_outbox_op(GsiModifyState *gs)
{
	UserMapping *user = GetUserMapping(GetUserId(), gs->idx->servers[gs->base_node]);
	PGconn	   *conn = GetConnection(user, false, NULL);
	const char *values[4];
	const char *sql = "INSERT INTO public.gsi_outbox (index_name, old_key, new_key, id) "
		"VALUES ($1, $2, $3, $4)";
	PGresult   *res;

	values[0] = gs->idx->name;
	values[1] = gs->old_key;
	values[2] = gs->new_key;
	values[3] = psprintf(INT64_FORMAT, gs->new_key ? gs->new_id : gs->old_id);

	if (!PQsendQueryParams(conn, sql, 4, NULL, values, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn, sql);
	if (PQresultStatus(res) != PGRES_COMMAND_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/*
 * The base row is about to be written on conn.  A sync GSI needs 2PC; a
 * write that only feeds async GSIs commits in one phase unless it ends up
 * touching several nodes (connection.c decides at pre-commit).
 */
void
gsi_note_base_write(List *states, PGconn *conn)
{
	ListCell   *lc;

	foreach(lc, states)
	{
		GsiModifyState *gs = lfirst(lc);

		if (!gs->idx->is_async)
		{
			gsi_mark_conn(conn, false);
			return;
		}
	}
	gsi_note_async_write();
}

/* Apply the captured entries of every GSI after the base write succeeded. */
void
gsi_apply_row(List *states)
{
	ListCell   *lc;

	foreach(lc, states)
	{
		GsiModifyState *gs = lfirst(lc);

		if (gs->idx->is_async)
		{
			if (gs->old_key || gs->new_key)
				send_outbox_op(gs);
			continue;
		}
		if (gs->old_key)
			send_index_op(gs, gs->old_node, gs->old_key, gs->old_id, false);
		if (gs->new_key)
			send_index_op(gs, gs->new_node, gs->new_key, gs->new_id, true);
	}
}

/* Run a DDL command on every node, as part of the distributed transaction. */
void
gsi_exec_on_all_nodes(GsiIndex *idx, const char *sql)
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
GsiIndex *
gsi_index_on_column(List *indexes, const char *col)
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
 * Remove one GSI: catalog row, build progress, pending outbox rows and the
 * index tables, all in the current distributed transaction.
 */
void
gsi_drop_index(GsiIndex *idx)
{
	Oid			argtypes[1] = {NAMEOID};
	Datum		args[1];

	args[0] = DirectFunctionCall1(namein, CStringGetDatum(idx->name));
	SPI_connect();
	if (SPI_execute_with_args("DELETE FROM gsi.gsi_index WHERE index_name = $1",
							  1, argtypes, args, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not delete from gsi.gsi_index");
	if (SPI_execute_with_args("DELETE FROM gsi.gsi_build_progress WHERE index_name = $1",
							  1, argtypes, args, NULL, false, 0) != SPI_OK_DELETE)
		elog(ERROR, "could not delete from gsi.gsi_build_progress");
	SPI_finish();

	gsi_exec_on_all_nodes(idx, psprintf("DROP TABLE IF EXISTS %s", idx->remote_table));
	if (idx->is_async)
		gsi_exec_on_all_nodes(idx, psprintf("DELETE FROM public.gsi_outbox WHERE index_name = %s",
											quote_literal_cstr(idx->name)));
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
		GsiIndex   *idx = gsi_index_on_column(indexes, col);

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
		gsi_drop_index(lfirst(lc));

	/* cached plans of the table carry the dropped GSIs */
	CacheInvalidateRelcache(rel);
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
