/*-------------------------------------------------------------------------
 *
 * gsi_async.c
 *		  Async GSIs: the outbox applier workers and gsi_lag()
 *
 * A write to a table with an async GSI adds one row per index to
 * public.gsi_outbox on the base node, in the base write's transaction
 * (gsi.c).  "gsi applier i" workers drain the outboxes: worker i owns the
 * nodes with node_id % gsi.applier_workers = i.  Per pass and node:
 *	1. read up to gsi.applier_batch outbox rows, in seq order;
 *	2. on each index node touched, in one transaction: delete old_key's
 *	   entry, insert new_key's (ON CONFLICT DO NOTHING); commit;
 *	3. delete exactly the outbox rows read.
 * Ordering by seq is enough: all changes of one row happen on its base
 * node under its row lock, so a later change of a row has a larger seq.
 * Deleting the rows read, not "seq <= max", never skips a row whose smaller
 * seq committed late.  A crash between 2 and 3 replays the batch, which is
 * harmless: replaying the same ops in the same order gives the same result.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/xact.h"
#include "catalog/namespace.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "postgres_fdw.h"
#include "gsi.h"
#include "postmaster/bgworker.h"
#include "postmaster/interrupt.h"
#include "storage/latch.h"
#include "tcop/tcopprot.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(gsi_lag);

PGDLLEXPORT void gsi_applier_main(Datum main_arg);

static int	gsi_applier_workers = 5;
static int	gsi_applier_naptime = 100;
static int	gsi_applier_batch = 1000;

/* Connections kept across passes, by node id; NULL = not connected */
#define MAX_NODES 64
static PGconn *node_conns[MAX_NODES];

static void
disconnect_all(void)
{
	for (int i = 0; i < MAX_NODES; i++)
		if (node_conns[i])
		{
			gsi_disconnect_raw(node_conns[i]);
			node_conns[i] = NULL;
		}
}

static PGconn *
conn_for(Oid *servers, int node)
{
	if (node_conns[node] == NULL || PQstatus(node_conns[node]) != CONNECTION_OK)
	{
		if (node_conns[node])
			gsi_disconnect_raw(node_conns[node]);
		node_conns[node] = NULL;
		node_conns[node] = gsi_connect_raw(GetForeignServer(servers[node]),
										   GetUserMapping(GetUserId(), servers[node]));
	}
	return node_conns[node];
}

static void
exec_ok(PGconn *conn, const char *sql, int nparams, const char *const *values)
{
	PGresult   *res = PQexecParams(conn, sql, nparams, NULL, values, NULL, NULL, 0);

	if (PQresultStatus(res) != PGRES_COMMAND_OK && PQresultStatus(res) != PGRES_TUPLES_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);
	PQclear(res);
}

/* Cache of the async GSIs met in one pass, by name */
static GsiIndex *
lookup_index(List **cache, const char *name)
{
	ListCell   *lc;
	GsiIndex   *idx;

	foreach(lc, *cache)
	{
		idx = lfirst(lc);
		if (strcmp(idx->name, name) == 0)
			return idx;
	}
	idx = gsi_index_by_name(name);
	if (idx != NULL && idx->is_async)
	{
		*cache = lappend(*cache, idx);
		return idx;
	}
	return NULL;				/* dropped, or no longer async */
}

/* Apply one batch of node `node`'s outbox; returns rows consumed. */
static int
apply_node(Oid *servers, int nnodes, int node)
{
	PGconn	   *base = conn_for(servers, node);
	PGresult   *batch;
	bool	   *in_xact = palloc0(sizeof(bool) * nnodes);
	List	   *cache = NIL;
	StringInfoData seqs;
	char	   *sql;
	int			n;
	const char *values[3];

	sql = psprintf("SELECT seq, index_name, old_key, new_key, id FROM public.gsi_outbox "
				   "ORDER BY seq LIMIT %d", gsi_applier_batch);
	batch = PQexec(base, sql);
	if (PQresultStatus(batch) != PGRES_TUPLES_OK)
	{
		char	   *state = PQresultErrorField(batch, PG_DIAG_SQLSTATE);

		/* no async GSI was ever created on this cluster */
		if (state && strcmp(state, "42P01") == 0)
		{
			PQclear(batch);
			return 0;
		}
		pgfdw_report_error(ERROR, batch, base, true, sql);
	}
	n = PQntuples(batch);
	if (n == 0)
	{
		PQclear(batch);
		return 0;
	}

	initStringInfo(&seqs);
	appendStringInfoChar(&seqs, '{');
	for (int r = 0; r < n; r++)
	{
		GsiIndex   *idx = lookup_index(&cache, PQgetvalue(batch, r, 1));

		appendStringInfo(&seqs, "%s%s", r > 0 ? "," : "", PQgetvalue(batch, r, 0));
		if (idx == NULL)
			continue;
		values[1] = PQgetvalue(batch, r, 4);
		for (int op = 0; op < 2; op++)
		{
			int			col = op == 0 ? 2 : 3;	/* old_key first, then new_key */
			int			target;
			PGconn	   *conn;

			if (PQgetisnull(batch, r, col))
				continue;
			values[0] = PQgetvalue(batch, r, col);
			target = gsi_node_for_datum(gsi_key_from_text(idx, values[0]),
										idx->typid, idx->collid, nnodes);
			conn = conn_for(servers, target);
			if (!in_xact[target])
			{
				exec_ok(conn, "BEGIN", 0, NULL);
				in_xact[target] = true;
			}
			if (op == 0)
				exec_ok(conn, psprintf("DELETE FROM %s WHERE key = $1 AND id = $2",
									   idx->remote_table), 2, values);
			else
			{
				values[2] = psprintf("%d", node);
				exec_ok(conn, psprintf("INSERT INTO %s (key, id, shard) VALUES ($1, $2, $3) "
									   "ON CONFLICT (key, id) DO NOTHING",
									   idx->remote_table), 3, values);
			}
		}
	}
	appendStringInfoChar(&seqs, '}');
	PQclear(batch);

	for (int target = 0; target < nnodes; target++)
		if (in_xact[target])
			exec_ok(node_conns[target], "COMMIT", 0, NULL);

	values[0] = seqs.data;
	exec_ok(base, "DELETE FROM public.gsi_outbox WHERE seq = ANY ($1::pg_catalog.int8[])",
			1, values);
	return n;
}

/* One pass over this worker's nodes in its own transaction; returns rows applied. */
static int
applier_pass(int worker)
{
	MemoryContext cxt = CurrentMemoryContext;
	int			applied = 0;

	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "gsi applier pass");

	PG_TRY();
	{
		if (OidIsValid(get_namespace_oid("gsi", true)))
		{
			MemoryContext xact_cxt = CurrentMemoryContext;
			Oid		   *servers;
			int			nnodes;

			SPI_connect();
			servers = gsi_load_nodes(&nnodes, xact_cxt);	/* outlives SPI_finish */
			SPI_finish();
			for (int node = worker; node < Min(nnodes, MAX_NODES); node += gsi_applier_workers)
				applied += apply_node(servers, nnodes, node);
		}
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		MemoryContextSwitchTo(cxt);
		EmitErrorReport();
		FlushErrorState();
		AbortCurrentTransaction();
		/* index-node transactions may be open; start clean next pass */
		disconnect_all();
		applied = 0;
	}
	PG_END_TRY();

	pgstat_report_activity(STATE_IDLE, NULL);
	return applied;
}

void
gsi_applier_main(Datum main_arg)
{
	int			worker = DatumGetInt32(main_arg);

	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(gsi_resolver_database, NULL, 0);

	for (;;)
	{
		int			applied;

		CHECK_FOR_INTERRUPTS();
		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		applied = applier_pass(worker);

		/* a full batch means more is waiting: go again at once */
		if (applied < gsi_applier_batch)
		{
			(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
							 gsi_applier_naptime, PG_WAIT_EXTENSION);
			ResetLatch(MyLatch);
		}
	}
}

/*
 * gsi_lag() returns (node int, pending bigint, oldest_secs float8): outbox
 * rows not yet applied per node, and the age of the oldest one (how stale an
 * async GSI lookup can be right now).
 */
Datum
gsi_lag(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Oid		   *servers;
	int			nnodes;
	const char *sql = "SELECT count(*), coalesce(extract(epoch FROM now() - min(created_at)), 0) "
		"FROM public.gsi_outbox";

	InitMaterializedSRF(fcinfo, 0);
	SPI_connect();
	servers = gsi_load_nodes(&nnodes, CurrentMemoryContext);
	for (int node = 0; node < nnodes; node++)
	{
		PGconn	   *conn = gsi_connect_raw(GetForeignServer(servers[node]),
										   GetUserMapping(GetUserId(), servers[node]));
		PGresult   *res = PQexec(conn, sql);
		Datum		values[3];
		bool		nulls[3] = {false, false, false};

		if (PQresultStatus(res) != PGRES_TUPLES_OK)
		{
			char	   *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);

			if (!(state && strcmp(state, "42P01") == 0))
			{
				char	   *msg = pstrdup(PQresultErrorMessage(res));

				PQclear(res);
				gsi_disconnect_raw(conn);
				ereport(ERROR,
						(errmsg("gsi_lag: could not read the outbox of node %d: %s", node, msg)));
			}
			values[1] = Int64GetDatum(0);
			values[2] = Float8GetDatum(0);
		}
		else
		{
			values[1] = Int64GetDatum(strtoi64(PQgetvalue(res, 0, 0), NULL, 10));
			values[2] = Float8GetDatum(strtod(PQgetvalue(res, 0, 1), NULL));
		}
		values[0] = Int32GetDatum(node);
		PQclear(res);
		gsi_disconnect_raw(conn);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
	SPI_finish();
	return (Datum) 0;
}

void
gsi_async_init(void)
{
	BackgroundWorker worker;

	DefineCustomIntVariable("gsi.applier_workers",
							"Number of async GSI applier workers; worker i drains nodes i, i+n, ...",
							NULL,
							&gsi_applier_workers,
							5, 0, MAX_NODES,
							PGC_POSTMASTER,
							0,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gsi.applier_naptime",
							"Sleep of an applier worker when its outboxes are empty.",
							NULL,
							&gsi_applier_naptime,
							100, 1, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gsi.applier_batch",
							"Outbox rows an applier worker applies per node and pass.",
							NULL,
							&gsi_applier_batch,
							1000, 1, 1000000,
							PGC_SIGHUP,
							0,
							NULL, NULL, NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	for (int i = 0; i < gsi_applier_workers; i++)
	{
		memset(&worker, 0, sizeof(worker));
		worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
		worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
		worker.bgw_restart_time = 5;
		worker.bgw_main_arg = Int32GetDatum(i);
		snprintf(worker.bgw_library_name, sizeof(worker.bgw_library_name), "gsi_fdw");
		snprintf(worker.bgw_function_name, sizeof(worker.bgw_function_name), "gsi_applier_main");
		snprintf(worker.bgw_name, sizeof(worker.bgw_name), "gsi applier %d", i);
		snprintf(worker.bgw_type, sizeof(worker.bgw_type), "gsi applier");
		RegisterBackgroundWorker(&worker);
	}
}
