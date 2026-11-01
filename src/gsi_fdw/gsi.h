/*-------------------------------------------------------------------------
 *
 * gsi.h
 *		  Global secondary index (GSI) maintenance for gsi_fdw
 *
 * A GSI on column c of a hash-partitioned table stores one entry
 * (key, id, shard) per row in table gsi_<index> on node hash(key) mod N.
 * Index writes run on postgres_fdw connections inside the remote
 * transactions, and any transaction that wrote a GSI table commits all
 * remote transactions with two-phase commit (see connection.c).
 *
 *-------------------------------------------------------------------------
 */
#ifndef GSI_H
#define GSI_H

#include "libpq-fe.h"
#include "foreign/foreign.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "utils/relcache.h"

typedef struct GsiIndex
{
	char	   *name;			/* e.g. users_email; one GSI per column */
	char	   *remote_table;	/* quoted public.gsi_<name> */
	char	   *column;			/* indexed column name */
	Oid			typid;
	Oid			collid;
	bool		is_unique;
	bool		is_ready;
	int			nnodes;
	Oid		   *servers;		/* foreign server of each node id */
} GsiIndex;

/*
 * Per-partition write state for one GSI, built by create_foreign_modify.
 * A table with several GSIs gets a List of these.
 */
typedef struct GsiModifyState
{
	GsiIndex   *idx;
	int			base_node;		/* node id of this partition's server */
	AttrNumber	key_attno;		/* indexed column, partition numbering */
	AttrNumber	id_attno;
	AttrNumber	old_key_junk;	/* junk columns in planSlot (UPDATE/DELETE) */
	AttrNumber	old_id_junk;
	bool		key_updated;	/* UPDATE assigns the indexed column */
	FmgrInfo	key_out;

	/* current row, captured before the base write */
	char	   *old_key;		/* NULL if no entry to delete */
	char	   *new_key;		/* NULL if no entry to insert */
	int			old_node;
	int			new_node;
	int64		old_id;
	int64		new_id;
} GsiModifyState;

/* gsi.c */
extern int	gsi_remote_lock_timeout;
extern void gsi_init(void);
extern Oid *gsi_load_nodes(int *nnodes, MemoryContext cxt);
extern List *gsi_indexes_for_rel(Oid relid);
extern int	gsi_node_for_datum(Datum value, Oid typid, Oid collid, int nnodes);
extern int	gsi_server_node(GsiIndex *idx, Oid serverid);
extern void gsi_add_update_targets(PlannerInfo *root, Index rtindex,
								   Relation rel, List *indexes);
extern bool gsi_direct_modify_ok(PlannerInfo *root, Index rtindex,
								 CmdType operation);
extern List *gsi_begin_modify(Relation rel, CmdType operation,
							  Plan *subplan, List *target_attrs);
extern void gsi_capture_row(List *states, CmdType operation,
							TupleTableSlot *slot, TupleTableSlot *planSlot);
extern void gsi_apply_row(List *states);
extern void gsi_fail_point(const char *name);

/* gsi_resolver.c */
extern int	gsi_resolve_all(int grace_secs);
extern void gsi_resolver_init(void);

/* gsi_scan.c */
extern void gsi_scan_init(void);

/* connection.c */
extern void gsi_mark_conn(PGconn *conn, bool is_index);
extern PGconn *gsi_connect_raw(ForeignServer *server, UserMapping *user);
extern void gsi_disconnect_raw(PGconn *conn);

#endif							/* GSI_H */
