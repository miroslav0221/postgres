#include "postgres.h"

#include "access/amapi.h"
#include "access/amvalidate.h"
#include "access/genam.h"
#include "bitmap.h"
#include "bitmap_private.h"
#include "access/reloptions.h"
#include "access/nbtree.h"
#include "access/tableam.h"
#include "access/xact.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_opclass.h"
#include "miscadmin.h"
#include "nodes/execnodes.h"
#include "nodes/pathnodes.h"
#include "tidbitmap.h"
#include "storage/lmgr.h"
#include "storage/smgr.h"
#include "parser/parse_oper.h"
#include "utils/memutils.h"
#include "utils/index_selfuncs.h"
#include "utils/syscache.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bmhandler);

static void bmbuildCallback(Relation index,	ItemPointer tupleId, Datum *attdata,
							bool *nulls, bool tupleIsAlive,	void *state);


/*
 * Bitmap index handler function: return IndexAmRoutine with access method parameters
 * and callbacks.
 */
Datum
bmhandler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *amroutine = makeNode(IndexAmRoutine);

	/* these are mostly the same as B-tree */
	amroutine->amstrategies = BTMaxStrategyNumber;
	amroutine->amsupport = BTNProcs;
	amroutine->amcanorder = false;
	amroutine->amcanorderbyop = false;
	amroutine->amcanbackward = false;
	amroutine->amcanunique = true;
	amroutine->amcanmulticol = true;
	amroutine->amcanparallel = false;
	amroutine->amoptionalkey = true;
	amroutine->amsearcharray = false;
	amroutine->amsearchnulls = false;
	amroutine->amstorage = false;
	amroutine->amclusterable = false;
	amroutine->ampredlocks = false;
	amroutine->amkeytype = InvalidOid;

	amroutine->ambuild = bmbuild;
	amroutine->ambuildempty = bmbuildempty;
	amroutine->aminsert = bminsert;
	// amroutine->ambulkdelete = bmbulkdelete;
	// amroutine->amvacuumcleanup = bmvacuumcleanup;
	amroutine->amcanreturn = NULL;
	// amroutine->amcostestimate = bmcostestimate;
	// amroutine->amoptions = bmoptions;
	amroutine->amproperty = NULL;
	// amroutine->amvalidate = bmvalidate;
	// amroutine->ambeginscan = bmbeginscan;
	// amroutine->amrescan = bmrescan;
	// amroutine->amgettuple = bmgettuple;
	// amroutine->amgetbitmap = bmgetbitmap;
	// //amroutine->amendscan = bmendscan;
	// amroutine->ammarkpos = bmmarkpos;
	// amroutine->amrestrpos = bmrestrpos;

	PG_RETURN_POINTER(amroutine);
}



/*
 * bmbuild() -- Build a new bitmap index.
 */

IndexBuildResult *
bmbuild(Relation heap, Relation index, IndexInfo *indexInfo)
{
	double      reltuples;
	BMBuildState bmstate;
	IndexBuildResult *result;
	TupleDesc	tupDesc;

	if (indexInfo->ii_Concurrent)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("CONCURRENTLY is not supported when creating bitmap indexes")));

	/* We expect this to be called exactly once. */
	if (RelationGetNumberOfBlocks(index) != 0)
		ereport (ERROR,
				(errcode(ERRCODE_INDEX_CORRUPTED),
				errmsg("index \"%s\" already contains data",
				RelationGetRelationName(index))));

	tupDesc = RelationGetDescr(index);

	/* initialize the bitmap index for MAIN_FORKNUM. */
	_bitmap_init(index, RelationNeedsWAL(index), false);

	/* initialize the build state. */
	_bitmap_init_buildstate(index, &bmstate);

	/* do the heap scan */
	reltuples = table_index_build_scan(heap, index, indexInfo, false, true,
									   bmbuildCallback, (void *) &bmstate,
									   NULL);
	/* clean up the build state */
	_bitmap_cleanup_buildstate(index, &bmstate);
	
	/* return statistics */
	result = (IndexBuildResult *) palloc0(sizeof(IndexBuildResult));

	result->heap_tuples = reltuples;
	result->index_tuples = bmstate.ituples;

	return result;
}

/*
 * Per-tuple callback from IndexBuildHeapScan
 */
static void
bmbuildCallback(Relation index, ItemPointer tupleId, Datum *attdata,
				bool *nulls, bool tupleIsAlive pg_attribute_unused(),	void *state)
{
	BMBuildState *bstate = (BMBuildState *) state;

	_bitmap_buildinsert(index, *tupleId, attdata, nulls, bstate);
	bstate->ituples += 1;

	if (((int)bstate->ituples) % 1000 == 0)
		CHECK_FOR_INTERRUPTS();
}