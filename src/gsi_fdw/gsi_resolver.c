/*-------------------------------------------------------------------------
 *
 * gsi_resolver.c
 *		  Finish in-doubt GSI prepared transactions after a crash
 *
 * Every leftover 'gsi_<fxid>_<server>_<role>' prepared transaction on a node
 * follows the coordinator's status of fxid: committed -> COMMIT PREPARED
 * (index participants first), aborted -> ROLLBACK PREPARED, in progress ->
 * leave it to the backend that owns it.  The resolver only reads the
 * decision, so it can never disagree with the original transaction.
 *
 * It runs as SQL gsi_resolve(grace) and, when gsi_fdw is preloaded, as the
 * "gsi resolver" background worker every gsi.resolver_interval.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "access/xact.h"
#include "catalog/namespace.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
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
#include "utils/snapmgr.h"

PG_FUNCTION_INFO_V1(gsi_resolve);

PGDLLEXPORT void gsi_resolver_main(Datum main_arg);

char	   *gsi_resolver_database = NULL;
static int	gsi_resolver_interval = 5000;
static int	gsi_resolver_grace = 10;

typedef struct GsiPending
{
	PGconn	   *conn;
	char	   *gid;
	uint64		fxid;
	char		role;
} GsiPending;

/* Coordinator status of fxid, or NULL if it can't be decided here. */
static char *
xact_status(uint64 fxid)
{
	Oid			argtypes[1] = {TEXTOID};
	Datum		args[1];
	bool		isnull;
	Datum		d;

	if (fxid >= U64FromFullTransactionId(ReadNextFullTransactionId()))
		return NULL;			/* not ours: from another coordinator? */

	args[0] = CStringGetTextDatum(psprintf(UINT64_FORMAT, fxid));
	if (SPI_execute_with_args("SELECT pg_catalog.pg_xact_status($1::pg_catalog.xid8)",
							  1, argtypes, args, NULL, true, 1) != SPI_OK_SELECT)
		elog(ERROR, "could not run pg_xact_status");
	d = SPI_getbinval(SPI_tuptable->vals[0], SPI_tuptable->tupdesc, 1, &isnull);
	return isnull ? NULL : TextDatumGetCString(d);
}

/* Parse 'gsi_<fxid>_<server>_<role>'. */
static bool
parse_gid(const char *gid, uint64 *fxid, char *role)
{
	char	   *end;
	size_t		len = strlen(gid);

	if (strncmp(gid, "gsi_", 4) != 0 || len < 8 || gid[len - 2] != '_')
		return false;
	*fxid = strtou64(gid + 4, &end, 10);
	*role = gid[len - 1];
	return end != gid + 4 && *end == '_' && (*role == 'i' || *role == 'b');
}

static void
collect_node(Oid serverid, int grace_secs, List **conns, List **pending)
{
	MemoryContext cxt = CurrentMemoryContext;
	PGconn	   *conn = NULL;
	PGresult   *res;
	char	   *sql;

	PG_TRY();
	{
		conn = gsi_connect_raw(GetForeignServer(serverid),
							   GetUserMapping(GetUserId(), serverid));
	}
	PG_CATCH();
	{
		ErrorData  *edata;

		MemoryContextSwitchTo(cxt);
		edata = CopyErrorData();
		FlushErrorState();
		ereport(WARNING,
				(errmsg("gsi resolver skipped server \"%s\": %s",
						GetForeignServer(serverid)->servername, edata->message)));
		FreeErrorData(edata);
		return;
	}
	PG_END_TRY();

	*conns = lappend(*conns, conn);
	sql = psprintf("SELECT gid FROM pg_catalog.pg_prepared_xacts "
				   "WHERE gid LIKE 'gsi\\_%%' "
				   "AND prepared < now() - interval '%d seconds'", grace_secs);
	res = PQexec(conn, sql);
	if (PQresultStatus(res) != PGRES_TUPLES_OK)
		pgfdw_report_error(ERROR, res, conn, true, sql);

	for (int i = 0; i < PQntuples(res); i++)
	{
		GsiPending *p = palloc(sizeof(GsiPending));

		p->conn = conn;
		p->gid = pstrdup(PQgetvalue(res, i, 0));
		if (parse_gid(p->gid, &p->fxid, &p->role))
			*pending = lappend(*pending, p);
		else
			ereport(WARNING, (errmsg("gsi resolver ignored malformed gid \"%s\"", p->gid)));
	}
	PQclear(res);
}

static bool
finish(GsiPending *p, bool commit)
{
	char	   *sql = psprintf("%s PREPARED '%s'", commit ? "COMMIT" : "ROLLBACK", p->gid);
	PGresult   *res = PQexec(p->conn, sql);
	char	   *state = PQresultErrorField(res, PG_DIAG_SQLSTATE);
	bool		ok = PQresultStatus(res) == PGRES_COMMAND_OK;

	/* 42704: already finished by the owning backend */
	if (ok || (state && strcmp(state, "42704") == 0))
		ereport(LOG, (errmsg("gsi resolver: %s", sql)));
	else
		pgfdw_report_error(WARNING, res, p->conn, false, sql);
	PQclear(res);
	return ok;
}

/* Resolve gsi_ prepared xacts older than grace_secs; returns # finished. */
int
gsi_resolve_all(int grace_secs)
{
	List	   *volatile conns = NIL;
	List	   *pending = NIL;
	int			nnodes;
	Oid		   *servers;
	int			resolved = 0;

	SPI_connect();
	PG_TRY();
	{
		servers = gsi_load_nodes(&nnodes, CurrentMemoryContext);
		for (int i = 0; i < nnodes; i++)
			collect_node(servers[i], grace_secs, (List **) &conns, &pending);

		/* index participants before base-only ones, across all nodes */
		for (int pass = 0; pass < 2; pass++)
		{
			ListCell   *lc;

			foreach(lc, pending)
			{
				GsiPending *p = lfirst(lc);
				char	   *status;

				if (p->role != (pass == 0 ? 'i' : 'b'))
					continue;
				status = xact_status(p->fxid);
				if (status == NULL)
					ereport(WARNING,
							(errmsg("gsi resolver cannot decide \"%s\"", p->gid)));
				else if (strcmp(status, "committed") == 0)
					resolved += finish(p, true);
				else if (strcmp(status, "aborted") == 0)
					resolved += finish(p, false);
			}
		}
	}
	PG_FINALLY();
	{
		ListCell   *lc;

		foreach(lc, conns)
			gsi_disconnect_raw(lfirst(lc));
	}
	PG_END_TRY();
	SPI_finish();

	return resolved;
}

/* gsi_resolve(grace_secs int) returns int */
Datum
gsi_resolve(PG_FUNCTION_ARGS)
{
	PG_RETURN_INT32(gsi_resolve_all(PG_GETARG_INT32(0)));
}

/* One resolver pass in its own transaction; errors are logged, not fatal. */
static void
resolver_pass(void)
{
	SetCurrentStatementStartTimestamp();
	StartTransactionCommand();
	PushActiveSnapshot(GetTransactionSnapshot());
	pgstat_report_activity(STATE_RUNNING, "gsi resolver pass");

	PG_TRY();
	{
		/* nothing to do until CREATE EXTENSION has run in this database */
		if (OidIsValid(get_namespace_oid("gsi", true)))
			(void) gsi_resolve_all(gsi_resolver_grace);
		PopActiveSnapshot();
		CommitTransactionCommand();
	}
	PG_CATCH();
	{
		EmitErrorReport();
		AbortCurrentTransaction();
		FlushErrorState();
	}
	PG_END_TRY();

	pgstat_report_activity(STATE_IDLE, NULL);
}

void
gsi_resolver_main(Datum main_arg)
{
	pqsignal(SIGHUP, SignalHandlerForConfigReload);
	pqsignal(SIGTERM, die);
	BackgroundWorkerUnblockSignals();
	BackgroundWorkerInitializeConnection(gsi_resolver_database, NULL, 0);

	for (;;)
	{
		(void) WaitLatch(MyLatch, WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH,
						 gsi_resolver_interval, PG_WAIT_EXTENSION);
		ResetLatch(MyLatch);
		CHECK_FOR_INTERRUPTS();

		if (ConfigReloadPending)
		{
			ConfigReloadPending = false;
			ProcessConfigFile(PGC_SIGHUP);
		}
		resolver_pass();
	}
}

void
gsi_resolver_init(void)
{
	BackgroundWorker worker;

	DefineCustomStringVariable("gsi.resolver_database",
							   "Database the GSI resolver worker connects to.",
							   NULL,
							   &gsi_resolver_database,
							   "postgres",
							   PGC_POSTMASTER,
							   0,
							   NULL, NULL, NULL);
	DefineCustomIntVariable("gsi.resolver_interval",
							"Time between GSI resolver passes.",
							NULL,
							&gsi_resolver_interval,
							5000, 100, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_MS,
							NULL, NULL, NULL);
	DefineCustomIntVariable("gsi.resolver_grace",
							"Minimum age of a prepared transaction before the worker resolves it.",
							NULL,
							&gsi_resolver_grace,
							10, 0, INT_MAX,
							PGC_SIGHUP,
							GUC_UNIT_S,
							NULL, NULL, NULL);

	if (!process_shared_preload_libraries_in_progress)
		return;

	memset(&worker, 0, sizeof(worker));
	worker.bgw_flags = BGWORKER_SHMEM_ACCESS | BGWORKER_BACKEND_DATABASE_CONNECTION;
	worker.bgw_start_time = BgWorkerStart_RecoveryFinished;
	worker.bgw_restart_time = 5;
	snprintf(worker.bgw_library_name, sizeof(worker.bgw_library_name), "gsi_fdw");
	snprintf(worker.bgw_function_name, sizeof(worker.bgw_function_name), "gsi_resolver_main");
	snprintf(worker.bgw_name, sizeof(worker.bgw_name), "gsi resolver");
	snprintf(worker.bgw_type, sizeof(worker.bgw_type), "gsi resolver");
	RegisterBackgroundWorker(&worker);
}
