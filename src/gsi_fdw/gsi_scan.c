/*-------------------------------------------------------------------------
 *
 * gsi_scan.c
 *		  GSI read path: planner hook and the GsiLookup custom scan
 *
 * For "indexed_col = Const/Param" on a partitioned table with a ready GSI,
 * set_rel_pathlist_hook adds a GsiLookup path, costed from the expected
 * number of matching entries and shards (cost_gsi_lookup); the planner keeps
 * it only if it is cheaper than the Append over all partitions.  With
 * several matching GSIs it uses one (a unique one if any); the plan records
 * which.  At run time it does two hops:
 *	1. ask the one index node hash(key) mod N for (id, shard);
 *	2. fetch "WHERE id = ANY(...)" from only the shards it returned.
 * All original quals are rechecked on the fetched rows, so a stale or
 * uncommitted index entry never produces a wrong row.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <math.h>

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_inherits.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/value.h"
#include "optimizer/cost.h"
#include "optimizer/optimizer.h"
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "postgres_fdw.h"
#include "gsi.h"
#include "storage/itemid.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/typcache.h"

/* One partition = one base shard */
typedef struct GsiPart
{
	Oid			serverid;
	char	   *select_sql;		/* SELECT <cols> FROM <remote> WHERE id = ANY($1) */
} GsiPart;

typedef struct GsiScanState
{
	CustomScanState css;
	ExprState  *key_expr;
	GsiIndex   *idx;
	GsiPart    *parts;			/* indexed by node id */
	List	   *rows;			/* fetched HeapTuples, in es_query_cxt */
	ListCell   *next;
	bool		fetched;
	int			index_node;		/* for EXPLAIN ANALYZE */
	int			shards_fetched;
} GsiScanState;

static set_rel_pathlist_hook_type prev_set_rel_pathlist_hook = NULL;

/* gsi.enable_lookup: like enable_indexscan, for comparing against broadcast */
static bool gsi_enable_lookup = true;

/* gsi.force_lookup: cost the GsiLookup path 0 (benchmarks time both plans) */
static bool gsi_force_lookup = false;

static Plan *gsi_plan_path(PlannerInfo *root, RelOptInfo *rel,
						   CustomPath *best_path, List *tlist,
						   List *clauses, List *custom_plans);
static Node *gsi_create_scan_state(CustomScan *cscan);
static void gsi_begin_scan(CustomScanState *node, EState *estate, int eflags);
static TupleTableSlot *gsi_exec_scan(CustomScanState *node);
static void gsi_end_scan(CustomScanState *node);
static void gsi_rescan(CustomScanState *node);
static void gsi_explain_scan(CustomScanState *node, List *ancestors,
							 ExplainState *es);

static CustomPathMethods gsi_path_methods = {
	.CustomName = "GsiLookup",
	.PlanCustomPath = gsi_plan_path,
};

static CustomScanMethods gsi_scan_methods = {
	.CustomName = "GsiLookup",
	.CreateCustomScanState = gsi_create_scan_state,
};

static CustomExecMethods gsi_exec_methods = {
	.CustomName = "GsiLookup",
	.BeginCustomScan = gsi_begin_scan,
	.ExecCustomScan = gsi_exec_scan,
	.EndCustomScan = gsi_end_scan,
	.ReScanCustomScan = gsi_rescan,
	.ExplainCustomScan = gsi_explain_scan,
};

/* Name of the GSI a GsiLookup plan uses */
static const char *
plan_index_name(CustomScan *cscan)
{
	return strVal(linitial(cscan->custom_private));
}

/*
 * Return the Const/Param compared with the indexed column by the type's
 * default equality operator, or NULL.  *rinfo gets the qual.
 */
static Expr *
find_key_clause(RelOptInfo *rel, Index rti, GsiIndex *idx, AttrNumber attnum,
				RestrictInfo **rinfo)
{
	TypeCacheEntry *tce = lookup_type_cache(idx->typid, TYPECACHE_EQ_OPR);
	ListCell   *lc;

	foreach(lc, rel->baserestrictinfo)
	{
		RestrictInfo *ri = lfirst_node(RestrictInfo, lc);
		OpExpr	   *op = (OpExpr *) ri->clause;
		Node	   *l,
				   *r;

		if (!IsA(op, OpExpr) || list_length(op->args) != 2 ||
			op->opno != tce->eq_opr)
			continue;
		l = linitial(op->args);
		r = lsecond(op->args);
		if (!IsA(l, Var))
		{
			Node	   *tmp = l;

			l = r;
			r = tmp;
		}
		if (!IsA(l, Var) || ((Var *) l)->varno != rti ||
			((Var *) l)->varattno != attnum || ((Var *) l)->varlevelsup != 0)
			continue;
		if ((IsA(r, Const) && !((Const *) r)->constisnull) ||
			(IsA(r, Param) && ((Param *) r)->paramkind == PARAM_EXTERN))
		{
			*rinfo = ri;
			return (Expr *) r;
		}
	}
	return NULL;
}

/*
 * Mean fdw_startup_cost and fdw_tuple_cost over the nodes, with
 * postgres_fdw's defaults.  The mean because at plan time the index node of
 * a $1 key is unknown; in practice all nodes have the same settings.
 */
static void
server_costs(GsiIndex *idx, Cost *startup, Cost *per_tuple)
{
	*startup = 0;
	*per_tuple = 0;
	for (int i = 0; i < idx->nnodes; i++)
	{
		Cost		s = 100.0;		/* DEFAULT_FDW_STARTUP_COST */
		Cost		t = 0.01;		/* DEFAULT_FDW_TUPLE_COST */
		ListCell   *lc;

		foreach(lc, GetForeignServer(idx->servers[i])->options)
		{
			DefElem    *def = lfirst(lc);

			if (strcmp(def->defname, "fdw_startup_cost") == 0)
				s = strtod(defGetString(def), NULL);
			else if (strcmp(def->defname, "fdw_tuple_cost") == 0)
				t = strtod(defGetString(def), NULL);
		}
		*startup += s / idx->nnodes;
		*per_tuple += t / idx->nnodes;
	}
}

/*
 * Cost of a GsiLookup that matches k index entries (docs/phase2_plan.md §1):
 *
 *	S  = N (1 - (1 - 1/N)^k)	expected shards holding the k rows (Cardenas):
 *								rows are placed by hash(id), unrelated to the key
 *	C1 = c_rt + c_probe ceil(k/e) + k c_tup		hop 1: one index node; the k
 *								entries are adjacent in its (key, id) B-tree
 *	C2 = S c_rt + k c_probe + k c_tup			hop 2: one query per touched shard,
 *								one primary-key probe per row
 *	C3 = k qual					recheck of every qual on the coordinator
 *
 * c_rt = fdw_startup_cost (one remote query), c_tup = fdw_tuple_cost +
 * cpu_tuple_cost (ship one row), c_probe = random_page_cost +
 * cpu_index_tuple_cost + cpu_tuple_cost, e = index entries per page.  Same
 * constants as postgres_fdw uses for the broadcast Append it is compared with.
 */
static void
cost_gsi_lookup(PlannerInfo *root, RelOptInfo *rel, GsiIndex *idx,
				double k, double shards, Cost *startup, Cost *total)
{
	Cost		c_rt;
	Cost		c_tup;
	Cost		c_probe = random_page_cost + cpu_index_tuple_cost + cpu_tuple_cost;
	double		width = get_typavgwidth(idx->typid, -1) + sizeof(int64) + sizeof(int32) +
		MAXALIGN(SizeofHeapTupleHeader) + sizeof(ItemIdData);
	double		per_page = Max(1.0, floor(BLCKSZ / width));
	QualCost	qual;

	server_costs(idx, &c_rt, &c_tup);
	c_tup += cpu_tuple_cost;
	cost_qual_eval(&qual, rel->baserestrictinfo, root);

	/* first row needs hop 1 and one shard */
	*startup = c_rt * Min(2.0, 1.0 + shards) + qual.startup + rel->reltarget->cost.startup;
	*total = c_rt + c_probe * ceil(k / per_page) + k * c_tup	/* hop 1 */
		+ shards * c_rt + k * c_probe + k * c_tup	/* hop 2 */
		+ k * qual.per_tuple						/* recheck */
		+ qual.startup + rel->reltarget->cost.startup +
		rel->reltarget->cost.per_tuple * rel->rows;
}

/*
 * Expected index entries for "col = key": 1 for a unique GSI, else the
 * selectivity from the table's inherited statistics (ANALYZE on the
 * partitioned table) times its row count.
 */
static double
estimate_entries(PlannerInfo *root, RelOptInfo *rel, Oid relid, GsiIndex *idx,
				 RestrictInfo *rinfo)
{
	Relation	r;
	double		tuples;
	double		saved_tuples;
	Selectivity sel;

	if (idx->is_unique)
		return 1;

	/*
	 * Row count: ANALYZE of the partitioned table stores it in the parent's
	 * reltuples (rel->tuples of an appendrel is only its filtered row count).
	 * Never analyzed: add up what the partitions know.
	 */
	r = table_open(relid, NoLock);
	tuples = r->rd_rel->reltuples;
	table_close(r, NoLock);
	if (tuples < 0)
	{
		int			i = -1;

		tuples = 0;
		while ((i = bms_next_member(rel->live_parts, i)) >= 0)
		{
			RelOptInfo *child = rel->part_rels[i];

			if (child != NULL && !IS_DUMMY_REL(child))
				tuples += child->tuples;
		}
	}
	if (tuples <= 0)
		tuples = rel->rows;

	/*
	 * eqsel scales a negative n_distinct (a fraction of the rows) by
	 * rel->tuples, so give it the table's row count for the call.  The bare
	 * clause, not the RestrictInfo, so no selectivity cached with the
	 * filtered count is reused.
	 */
	saved_tuples = rel->tuples;
	rel->tuples = tuples;
	sel = clause_selectivity(root, (Node *) rinfo->clause, 0, JOIN_INNER, NULL);
	rel->tuples = saved_tuples;

	return clamp_row_est(sel * tuples);
}

static void
gsi_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					 RangeTblEntry *rte)
{
	GsiIndex   *idx = NULL;
	Expr	   *key = NULL;
	RestrictInfo *key_rinfo = NULL;
	CustomPath *cpath;
	double		k;
	double		shards;
	ListCell   *lc;

	if (prev_set_rel_pathlist_hook)
		prev_set_rel_pathlist_hook(root, rel, rti, rte);

	/* SELECT on a partitioned table, not a write target, no row locks */
	if (!gsi_enable_lookup ||
		rte->rtekind != RTE_RELATION || rte->relkind != RELKIND_PARTITIONED_TABLE ||
		rel->reloptkind != RELOPT_BASEREL || IS_DUMMY_REL(rel) ||
		root->parse->resultRelation == rti || root->parse->rowMarks != NIL)
		return;

	/* pick a ready GSI whose column has an equality qual; prefer a unique one */
	foreach(lc, gsi_indexes_for_rel(rte->relid))
	{
		GsiIndex   *cand = lfirst(lc);
		Expr	   *cand_key;
		RestrictInfo *cand_rinfo = NULL;

		if (!cand->is_ready || (idx != NULL && (idx->is_unique || !cand->is_unique)))
			continue;
		cand_key = find_key_clause(rel, rti, cand, get_attnum(rte->relid, cand->column),
								   &cand_rinfo);
		if (cand_key != NULL)
		{
			idx = cand;
			key = cand_key;
			key_rinfo = cand_rinfo;
		}
	}
	if (idx == NULL)
		return;

	k = estimate_entries(root, rel, rte->relid, idx, key_rinfo);
	shards = idx->nnodes * (1.0 - pow(1.0 - 1.0 / idx->nnodes, k));

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.rows = rel->rows;
	cost_gsi_lookup(root, rel, idx, k, shards,
					&cpath->path.startup_cost, &cpath->path.total_cost);
	if (gsi_force_lookup)
		cpath->path.startup_cost = cpath->path.total_cost = 0;
	/* the estimates ride along for EXPLAIN */
	cpath->custom_private = list_make4(key, makeString(pstrdup(idx->name)),
									   makeFloat(psprintf("%.0f", k)),
									   makeFloat(psprintf("%.2f", shards)));
	cpath->methods = &gsi_path_methods;
	add_path(rel, &cpath->path);

	/*
	 * apply_scanjoin_target_to_paths() drops all paths of a partitioned rel
	 * and rebuilds the Append from its children.  Pruning is already done,
	 * so treat the rel as unpartitioned from here on; this only gives up
	 * partitionwise join/aggregate for it.
	 */
	rel->part_scheme = NULL;
}

static Plan *
gsi_plan_path(PlannerInfo *root, RelOptInfo *rel, CustomPath *best_path,
			  List *tlist, List *clauses, List *custom_plans)
{
	CustomScan *cscan = makeNode(CustomScan);

	cscan->scan.plan.targetlist = tlist;
	/* every qual is rechecked on the fetched rows */
	cscan->scan.plan.qual = extract_actual_clauses(clauses, false);
	cscan->scan.scanrelid = rel->relid;
	cscan->flags = best_path->flags;
	cscan->custom_exprs = list_make1(linitial(best_path->custom_private));
	/* index name, estimated entries, estimated shards */
	cscan->custom_private = list_copy_tail(best_path->custom_private, 1);
	cscan->methods = &gsi_scan_methods;
	return &cscan->scan.plan;
}

static Node *
gsi_create_scan_state(CustomScan *cscan)
{
	GsiScanState *gss = palloc0(sizeof(GsiScanState));

	NodeSetTag(gss, T_CustomScanState);
	gss->css.methods = &gsi_exec_methods;
	return (Node *) gss;
}

/* Remote column name of a partition column: column_name option or attname. */
static char *
remote_column(Oid partid, const char *attname)
{
	ListCell   *lc;

	foreach(lc, GetForeignColumnOptions(partid, get_attnum(partid, attname)))
	{
		DefElem    *def = lfirst(lc);

		if (strcmp(def->defname, "column_name") == 0)
			return defGetString(def);
	}
	return pstrdup(attname);
}

/* Like deparse.c's deparseRelation: schema_name/table_name options win. */
static char *
remote_table(Oid partid)
{
	char	   *nspname = get_namespace_name(get_rel_namespace(partid));
	char	   *relname = get_rel_name(partid);
	ListCell   *lc;

	foreach(lc, GetForeignTable(partid)->options)
	{
		DefElem    *def = lfirst(lc);

		if (strcmp(def->defname, "schema_name") == 0)
			nspname = defGetString(def);
		else if (strcmp(def->defname, "table_name") == 0)
			relname = defGetString(def);
	}
	return psprintf("%s.%s", quote_identifier(nspname), quote_identifier(relname));
}

static void
gsi_begin_scan(CustomScanState *node, EState *estate, int eflags)
{
	GsiScanState *gss = (GsiScanState *) node;
	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;
	Relation	rel = node->ss.ss_currentRelation;
	TupleDesc	desc = RelationGetDescr(rel);
	List	   *children;
	ListCell   *lc;

	gss->key_expr = ExecInitExpr(linitial(cscan->custom_exprs), &node->ss.ps);
	if (eflags & EXEC_FLAG_EXPLAIN_ONLY)
		return;

	foreach(lc, gsi_indexes_for_rel(RelationGetRelid(rel)))
	{
		GsiIndex   *idx = lfirst(lc);

		if (strcmp(idx->name, plan_index_name(cscan)) == 0)
			gss->idx = idx;
	}
	if (gss->idx == NULL)
		elog(ERROR, "GSI \"%s\" disappeared", plan_index_name(cscan));
	gss->parts = palloc0(sizeof(GsiPart) * gss->idx->nnodes);

	/* remote SELECT per partition, columns in the parent's order */
	children = find_inheritance_children(RelationGetRelid(rel), NoLock);
	foreach(lc, children)
	{
		Oid			partid = lfirst_oid(lc);
		Oid			serverid = GetForeignTable(partid)->serverid;
		StringInfoData sql;

		initStringInfo(&sql);
		appendStringInfoString(&sql, "SELECT ");
		for (int i = 0; i < desc->natts; i++)
		{
			Form_pg_attribute att = TupleDescAttr(desc, i);

			if (i > 0)
				appendStringInfoString(&sql, ", ");
			if (att->attisdropped)
				appendStringInfoString(&sql, "NULL");
			else
				appendStringInfoString(&sql,
									   quote_identifier(remote_column(partid, NameStr(att->attname))));
		}
		appendStringInfo(&sql, " FROM %s WHERE %s = ANY ($1::pg_catalog.int8[])",
						 remote_table(partid),
						 quote_identifier(remote_column(partid, "id")));

		gss->parts[gsi_server_node(gss->idx, serverid)].serverid = serverid;
		gss->parts[gsi_server_node(gss->idx, serverid)].select_sql = sql.data;
	}
}

/* Run a parameterized query on a GSI node's connection; caller PQclears. */
static PGresult *
remote_query(Oid serverid, const char *sql, const char *param)
{
	PGconn	   *conn = GetConnection(GetUserMapping(GetUserId(), serverid),
									 false, NULL);
	PGresult   *res;

	if (!PQsendQueryParams(conn, sql, 1, NULL, &param, NULL, NULL, 0))
		pgfdw_report_error(ERROR, NULL, conn, false, sql);
	res = pgfdw_get_result(conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	return res;
}

/* Hop 1 + hop 2: fill gss->rows with the candidate base rows. */
static void
gsi_fetch(GsiScanState *gss)
{
	ExprContext *econtext = gss->css.ss.ps.ps_ExprContext;
	EState	   *estate = gss->css.ss.ps.state;
	TupleDesc	desc = RelationGetDescr(gss->css.ss.ss_currentRelation);
	GsiIndex   *idx = gss->idx;
	StringInfoData *ids;
	PGconn	  **conns;
	Oid			typoutput;
	bool		typisvarlena;
	Datum		key;
	bool		isnull;
	char	   *sql;
	PGresult   *res;
	MemoryContext oldcxt;
	int			nestlevel;

	gss->fetched = true;
	gss->rows = NIL;
	key = ExecEvalExprSwitchContext(gss->key_expr, econtext, &isnull);
	if (isnull)
		return;

	/* hop 1: one index node */
	gss->index_node = gsi_node_for_datum(key, idx->typid, idx->collid, idx->nnodes);
	getTypeOutputInfo(idx->typid, &typoutput, &typisvarlena);
	sql = psprintf("SELECT id, shard FROM %s WHERE key = $1", idx->remote_table);
	res = remote_query(idx->servers[gss->index_node], sql,
					   OidOutputFunctionCall(typoutput, key));

	ids = palloc0(sizeof(StringInfoData) * idx->nnodes);
	for (int i = 0; i < PQntuples(res); i++)
	{
		int			shard = atoi(PQgetvalue(res, i, 1));

		if (shard < 0 || shard >= idx->nnodes)
			continue;
		if (ids[shard].data == NULL)
		{
			initStringInfo(&ids[shard]);
			appendStringInfoChar(&ids[shard], '{');
		}
		else
			appendStringInfoChar(&ids[shard], ',');
		appendStringInfoString(&ids[shard], PQgetvalue(res, i, 0));
	}
	PQclear(res);

	/*
	 * hop 2: only the shards that own candidate rows, all at once.  Every
	 * query is sent before any result is read, so the wait is about one round
	 * trip instead of one per shard.  Each shard is a different server and so
	 * a different connection.  If anything throws while queries are still
	 * running, abort cleanup cancels them (pgfdw_abort_cleanup), as for any
	 * busy postgres_fdw connection.
	 */
	conns = palloc0(sizeof(PGconn *) * idx->nnodes);
	for (int shard = 0; shard < idx->nnodes; shard++)
	{
		const char *param;

		if (ids[shard].data == NULL || gss->parts[shard].select_sql == NULL)
			continue;
		appendStringInfoChar(&ids[shard], '}');
		param = ids[shard].data;
		conns[shard] = GetConnection(GetUserMapping(GetUserId(),
													gss->parts[shard].serverid),
									 false, NULL);
		if (!PQsendQueryParams(conns[shard], gss->parts[shard].select_sql,
							   1, NULL, &param, NULL, NULL, 0))
			pgfdw_report_error(ERROR, NULL, conns[shard], false,
							   gss->parts[shard].select_sql);
		gss->shards_fetched++;
	}

	/* collect in shard order, so rows come back in the same order as before */
	for (int shard = 0; shard < idx->nnodes; shard++)
	{
		AttInMetadata *attinmeta;
		char	  **values;

		if (conns[shard] == NULL)
			continue;
		res = pgfdw_get_result(conns[shard], gss->parts[shard].select_sql);
		if (PQresultStatus(res) != PGRES_TUPLES_OK)
			pgfdw_report_error(ERROR, res, conns[shard], true,
							   gss->parts[shard].select_sql);

		attinmeta = TupleDescGetAttInMetadata(desc);
		values = palloc(sizeof(char *) * desc->natts);
		nestlevel = set_transmission_modes();
		oldcxt = MemoryContextSwitchTo(estate->es_query_cxt);
		for (int i = 0; i < PQntuples(res); i++)
		{
			for (int j = 0; j < desc->natts; j++)
				values[j] = PQgetisnull(res, i, j) ? NULL : PQgetvalue(res, i, j);
			gss->rows = lappend(gss->rows, BuildTupleFromCStrings(attinmeta, values));
		}
		MemoryContextSwitchTo(oldcxt);
		reset_transmission_modes(nestlevel);
		PQclear(res);
	}
}

static TupleTableSlot *
gsi_next(ScanState *ss)
{
	GsiScanState *gss = (GsiScanState *) ss;
	TupleTableSlot *slot = ss->ss_ScanTupleSlot;

	if (!gss->fetched)
	{
		gsi_fetch(gss);
		gss->next = list_head(gss->rows);
	}
	if (gss->next == NULL)
		return ExecClearTuple(slot);

	ExecForceStoreHeapTuple(lfirst(gss->next), slot, false);
	gss->next = lnext(gss->rows, gss->next);
	return slot;
}

/* Rows are fetched fresh; only ExecScan's qual recheck applies. */
static bool
gsi_recheck(ScanState *ss, TupleTableSlot *slot)
{
	return true;
}

static TupleTableSlot *
gsi_exec_scan(CustomScanState *node)
{
	return ExecScan(&node->ss, gsi_next, gsi_recheck);
}

static void
gsi_rescan(CustomScanState *node)
{
	GsiScanState *gss = (GsiScanState *) node;

	gss->fetched = false;
	gss->shards_fetched = 0;
}

static void
gsi_end_scan(CustomScanState *node)
{
}

static void
gsi_explain_scan(CustomScanState *node, List *ancestors, ExplainState *es)
{
	GsiScanState *gss = (GsiScanState *) node;

	CustomScan *cscan = (CustomScan *) node->ss.ps.plan;

	ExplainPropertyText("GSI", plan_index_name(cscan), es);
	ExplainPropertyFloat("Estimated Entries", NULL,
						 floatVal(lsecond(cscan->custom_private)), 0, es);
	ExplainPropertyFloat("Estimated Shards", NULL,
						 floatVal(lthird(cscan->custom_private)), 1, es);
	if (es->analyze && gss->fetched)
	{
		ExplainPropertyInteger("Index Node", NULL, gss->index_node, es);
		ExplainPropertyInteger("Shards Fetched", NULL, gss->shards_fetched, es);
	}
}

void
gsi_scan_init(void)
{
	DefineCustomBoolVariable("gsi.enable_lookup",
							 "Lets the planner use GsiLookup for indexed-column lookups.",
							 NULL,
							 &gsi_enable_lookup,
							 true,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	DefineCustomBoolVariable("gsi.force_lookup",
							 "Makes the planner always pick GsiLookup when it applies (benchmarks).",
							 NULL,
							 &gsi_force_lookup,
							 false,
							 PGC_USERSET,
							 0,
							 NULL, NULL, NULL);
	RegisterCustomScanMethods(&gsi_scan_methods);
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gsi_set_rel_pathlist;
}
