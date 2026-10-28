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
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/transam.h"
#include "catalog/pg_type.h"
#include "executor/spi.h"
#include "fmgr.h"
#include "miscadmin.h"
#include "postgres_fdw.h"
#include "gsi.h"
#include "utils/builtins.h"

PG_FUNCTION_INFO_V1(gsi_resolve);

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
