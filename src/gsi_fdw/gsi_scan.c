/*-------------------------------------------------------------------------
 *
 * gsi_scan.c
 *		  GSI read path: planner hook and the GsiLookup custom scan
 *
 * For "indexed_col = Const/Param" on a partitioned table with a ready GSI,
 * set_rel_pathlist_hook adds a GsiLookup path that beats the Append over
 * all partitions.  With several matching GSIs it uses one (a unique one if
 * any); the plan records which.  At run time it does two hops:
 *	1. ask the one index node hash(key) mod N for (id, shard);
 *	2. fetch "WHERE id = ANY(...)" from only the shards it returned.
 * All original quals are rechecked on the fetched rows, so a stale or
 * uncommitted index entry never produces a wrong row.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

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
#include "optimizer/pathnode.h"
#include "optimizer/paths.h"
#include "optimizer/restrictinfo.h"
#include "postgres_fdw.h"
#include "gsi.h"
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
 * default equality operator, or NULL.
 */
static Expr *
find_key_clause(RelOptInfo *rel, Index rti, GsiIndex *idx, AttrNumber attnum)
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
			return (Expr *) r;
	}
	return NULL;
}

static void
gsi_set_rel_pathlist(PlannerInfo *root, RelOptInfo *rel, Index rti,
					 RangeTblEntry *rte)
{
	GsiIndex   *idx = NULL;
	Expr	   *key = NULL;
	CustomPath *cpath;
	Cost		cheapest = 0;
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

		if (!cand->is_ready || (idx != NULL && (idx->is_unique || !cand->is_unique)))
			continue;
		cand_key = find_key_clause(rel, rti, cand, get_attnum(rte->relid, cand->column));
		if (cand_key != NULL)
		{
			idx = cand;
			key = cand_key;
		}
	}
	if (idx == NULL)
		return;

	foreach(lc, rel->pathlist)
	{
		Path	   *p = lfirst(lc);

		if (cheapest == 0 || p->total_cost < cheapest)
			cheapest = p->total_cost;
	}

	cpath = makeNode(CustomPath);
	cpath->path.pathtype = T_CustomScan;
	cpath->path.parent = rel;
	cpath->path.pathtarget = rel->reltarget;
	cpath->path.rows = rel->rows;
	/* two round trips instead of N; always cheaper than the broadcast */
	cpath->path.startup_cost = 0;
	cpath->path.total_cost = cheapest > 0 ? cheapest * 0.1 : 1;
	cpath->custom_private = list_make2(key, makeString(pstrdup(idx->name)));
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
	cscan->custom_private = list_make1(lsecond(best_path->custom_private));	/* index name */
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

	/* hop 2: only the shards that own candidate rows */
	for (int shard = 0; shard < idx->nnodes; shard++)
	{
		AttInMetadata *attinmeta;
		char	  **values;

		if (ids[shard].data == NULL || gss->parts[shard].select_sql == NULL)
			continue;
		appendStringInfoChar(&ids[shard], '}');
		gss->shards_fetched++;
		res = remote_query(gss->parts[shard].serverid,
						   gss->parts[shard].select_sql, ids[shard].data);

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

	ExplainPropertyText("GSI", plan_index_name((CustomScan *) node->ss.ps.plan), es);
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
	RegisterCustomScanMethods(&gsi_scan_methods);
	prev_set_rel_pathlist_hook = set_rel_pathlist_hook;
	set_rel_pathlist_hook = gsi_set_rel_pathlist;
}
