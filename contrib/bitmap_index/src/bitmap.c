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
#include "utils/selfuncs.h"
#include "parser/parsetree.h"

PG_MODULE_MAGIC;

PG_FUNCTION_INFO_V1(bmhandler);

static void bmbuildCallback(Relation index,	ItemPointer tupleId, Datum *attdata,
							bool *nulls, bool tupleIsAlive,	void *state);

static void cleanup_pos(BMScanPosition pos);

void
bmcostestimate(struct PlannerInfo *root,
               struct IndexPath *path,
               double loop_count,
               Cost *indexStartupCost,
               Cost *indexTotalCost,
               Selectivity *indexSelectivity,
               double *indexCorrelation,
               double *indexPages)
{
    IndexOptInfo *index = path->indexinfo;
    RelOptInfo *baserel = index->rel;
    RangeTblEntry *rte PG_USED_FOR_ASSERTS_ONLY = planner_rt_fetch(baserel->relid, root);
    GenericCosts costs;

    Assert(rte->rtekind == RTE_RELATION);
    Assert(rte->relid != InvalidOid);

    /*
     * Now do generic index cost estimation.
     */
    MemSet(&costs, 0, sizeof(costs));

    /*
     * We create a LOV for each distinct key in bitmap index. And the LOV point
     * to the bitmap vector pages. Since each bitmap vector has the same length,
     * although we do compress for the bits, but we can assume each distinct
     * key has approximately same number of bitmap vector pages(although there
     * must be some counterexamples). So the indexPages should be:
     * selectedDistinctValues / numDistinctValues * index->pages.
     *
     * But the issue is we can't estimate both of the distinct values from stats
     * through estimate_num_groups since it produces larger estimates. Especially
     * for selectedDistinctValues.
     *
     * Image below cases:
     * 1. indexSelectivity also correspond to how may distinct values get selected.
     * Then the result of genericcostestimate's indexPages will be accurate.
     * 2. indexSelectivity is high but only match a small number of distinct values.
     * This means the bitmap vector is sparse. So the total index pages number should
     * be small.
     * 3. indexSelectivity is low but match lots of distinct values. This also means
     * the bitmap vector is sparse, and the total index pages number should be small.
     *
     * The estimate in genericcostestimate should works fine for above cases although
     * it's not accurate.
     */

    genericcostestimate(root, path, loop_count, &costs);

    *indexStartupCost = costs.indexStartupCost;
    *indexTotalCost = costs.indexTotalCost;
#ifdef FAULT_INJECTOR
    /* Simulate an bitmapAnd plan by changing bitmap cost. */
		if (FaultInjector_InjectFaultIfSet("simulate_bitmap_and",
									DDLNotSpecified,
									"",
									"") == FaultInjectorTypeSkip)
		{
			*indexTotalCost = 0;
		}
#endif
    *indexSelectivity = costs.indexSelectivity;
    *indexCorrelation = costs.indexCorrelation;
    *indexPages = costs.numIndexPages;
}

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
	amroutine->ambulkdelete = bmbulkdelete;
	amroutine->amvacuumcleanup = bmvacuumcleanup;
	amroutine->amcanreturn = NULL;
	amroutine->amcostestimate = bmcostestimate; //
	amroutine->amoptions = bmoptions; //
	amroutine->amproperty = NULL;
	amroutine->amvalidate = bmvalidate; //
	amroutine->ambeginscan = bmbeginscan;
	amroutine->amrescan = bmrescan;
	amroutine->amgettuple = bmgettuple; 
	amroutine->amgetbitmap = bmgetbitmap; //
	amroutine->amendscan = bmendscan; 
	amroutine->ammarkpos = bmmarkpos; //
	amroutine->amrestrpos = bmrestrpos;

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
 *	bmbuildempty() -- build an empty bitmap index in the initialization fork
 */
void
bmbuildempty(Relation indexrel)
{
    /* initialize meta page and first LOV page for INIT_FORKNUM */
    _bitmap_init(indexrel, true, true);
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

/*
 * bminsert() -- insert an index tuple into a bitmap index.
 */
bool
bminsert(Relation rel, Datum *values, bool *isnull,
         ItemPointer ht_ctid, Relation heapRel,
         IndexUniqueCheck checkUnique,
         bool indexUnchanged,
         IndexInfo *indexInfo)
{
    _bitmap_doinsert(rel, *ht_ctid, values, isnull);
    return true;
}

/*
 * GetBitmapIndexAuxOids - Given an open index, fetch and return the oids for
 * the bitmap subobjects (pg_bm_xxxx + pg_bm_xxxx_index).
 *
 * Note: Currently this information is not stored directly in the catalog, but
 * is hidden away inside the metadata page of the index.  Future versions should
 * move this information into the catalog.
 */
void
GetBitmapIndexAuxOids(Relation index, Oid *heapId, Oid *indexId)
{
	Buffer     metabuf;
	BMMetaPage metapage;


	/* Only Bitmap Indexes have bitmap related sub-objects */
	// if (!RelationIsBitmapIndex(index))
	// {
	// 	*heapId = InvalidOid;
	// 	*indexId = InvalidOid;
	// 	return;
	// }

	metabuf = _bitmap_getbuf(index, BM_METAPAGE, BM_READ);
	metapage = _bitmap_get_metapage_data(index, metabuf);

	*heapId  = metapage->bm_lov_heapId;
	*indexId = metapage->bm_lov_indexId;

	_bitmap_relbuf(metabuf);
}

/*
 * bmvacuumcleanup() -- post-vacuum cleanup.
 *
 * We do nothing useful here.
 */
IndexBulkDeleteResult *
bmvacuumcleanup(IndexVacuumInfo *info,
                IndexBulkDeleteResult *stats)
{
    Relation	rel = info->index;

    if(stats == NULL)
        stats = (IndexBulkDeleteResult *)palloc0(sizeof(IndexBulkDeleteResult));

    /* update statistics */
    stats->num_pages = RelationGetNumberOfBlocks(rel);
    stats->pages_deleted = 0;
    stats->pages_free = 0;
    /* XXX: dodgy hack to shutup index_scan() and vacuum_index() */
    stats->num_index_tuples = info->num_heap_tuples;

    return stats;
}

/*
 * bmbulkdelete() -- bulk delete index entries
 *
 * Re-index is performed before retrieving the number of tuples
 * indexed in this index.
 */
IndexBulkDeleteResult *
bmbulkdelete(IndexVacuumInfo *info,
             IndexBulkDeleteResult *stats,
             IndexBulkDeleteCallback callback,
             void *callback_state)
{
    Relation	rel = info->index;
    ReindexParams reindex_params = {0};

    /* allocate stats if first time through, else re-use existing struct */
    if (stats == NULL)
        stats = (IndexBulkDeleteResult *)
                palloc0(sizeof(IndexBulkDeleteResult));

    reindex_index(NULL, RelationGetRelid(rel), true, rel->rd_rel->relpersistence, &reindex_params);

    CommandCounterIncrement();

    stats->num_pages = RelationGetNumberOfBlocks(rel);
    /* Since we re-build the index, set this to number of heap tuples. */
    stats->num_index_tuples = info->num_heap_tuples;
    stats->tuples_removed = 0;

    return stats;
}

bytea *
bmoptions(Datum reloptions, bool validate)
{
//    return default_reloptions(reloptions, validate, RELOPT_KIND_BITMAP);
    return (bytea *)1;
}

/*
 * Ask appropriate access method to validate the specified opclass.
 */
bool
bmvalidate(Oid opclassoid)
{
    /*
     * Bitmap indexes use the same opclass support functions and strategies
     * as B-tree indexes. In fact, we use a real B-tree index for the LOV
     * tree. So borrow B-tree's validate function.
     */
//    return btree_or_bitmap_validate(opclassoid, "bitmap");
    return true;
}

/*
 * bmbeginscan() -- start a scan on the bitmap index.
 */
IndexScanDesc
bmbeginscan(Relation rel, int nkeys, int norderbys)
{
    IndexScanDesc scan;
    BMScanOpaque	so;

    /* no order by operators allowed */
    Assert(norderbys == 0);

    /* get the scan */
    scan = RelationGetIndexScan(rel, nkeys, norderbys);

    /* allocate private workspace */
    so = (BMScanOpaque) palloc(sizeof(BMScanOpaqueData));
    so->bm_currPos = NULL;
    so->bm_markPos = NULL;
    so->cur_pos_valid = false;
    so->mark_pos_valid = false;

    scan->xs_itupdesc = RelationGetDescr(rel);

    scan->opaque = so;

    return scan;
}



/*
 * bmgettuple() -- return the next tuple in a scan.
 */
bool
bmgettuple(IndexScanDesc scan, ScanDirection dir)
{
	BMScanOpaque  so = (BMScanOpaque) scan->opaque;
	bool		res;

	/* This implementation of a bitmap index is never lossy */
	scan->xs_recheck = false;

	/* 
	 * If we have already begun our scan, continue in the same direction.
	 * Otherwise, start up the scan.
	 */
	if (so->bm_currPos && so->cur_pos_valid)
		res = _bitmap_next(scan, dir);
	else
		res = _bitmap_first(scan, dir);

	return res;
}
	

/*
 * bmgetbitmap() -- return a stream bitmap.
 
 */
int64
bmgetbitmap(IndexScanDesc scan, Node **bmNodeP)
{
    return 1;
}


/*
 * bmmarkpos() -- save the current scan position.
 */
void
bmmarkpos(IndexScanDesc scan)
{
}

/*
 * bmrestrpos() -- restore a scan to the last saved position.
 */
void
bmrestrpos(IndexScanDesc scan)
{
}

/*
 * bmrescan() -- restart a scan on the bitmap index.
 */
void
bmrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys,
		 ScanKey orderbys, int norderbys)
{
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	if (so->bm_currPos != NULL)
	{
		cleanup_pos(so->bm_currPos);
		MemSet(so->bm_currPos, 0, sizeof(BMScanPositionData));
		so->cur_pos_valid = false;
	}

	if (so->bm_markPos != NULL)
	{
		cleanup_pos(so->bm_markPos);
		MemSet(so->bm_markPos, 0, sizeof(BMScanPositionData));
		so->cur_pos_valid = false;
	}
	/* reset the scan key */
	if (scankey && scan->numberOfKeys > 0)
		memmove(scan->keyData, scankey,
				scan->numberOfKeys * sizeof(ScanKeyData));
}


/*
 * bmendscan() -- close a scan.
 */
void
bmendscan(IndexScanDesc scan)
{
	BMScanOpaque	so = (BMScanOpaque) scan->opaque;

	/* free the space */
	if (so->bm_currPos != NULL)
	{
		/*
		 * release the buffers that have been stored for each related 
		 * bitmap vector.
		 */
		cleanup_pos(so->bm_currPos);
		pfree(so->bm_currPos);
		so->bm_currPos = NULL;
	}

	if (so->bm_markPos != NULL)
	{
		cleanup_pos(so->bm_markPos);
		pfree(so->bm_markPos);
		so->bm_markPos = NULL;
	}

	pfree(so);
	scan->opaque = NULL;
}

static void
cleanup_pos(BMScanPosition pos) 
{
	if (pos->nvec == 0)
		return;
	
	/*
	 * Only cleanup bm_batchWords if we have more than one vector since
	 * _bitmap_cleanup_scanpos() will clean it up for the single vector
	 * case.
	 */
	if (pos->nvec > 1)
	{
		_bitmap_cleanup_batchwords(pos->bm_batchWords);
		if (pos->bm_batchWords != NULL)
			pfree(pos->bm_batchWords);
	}
	_bitmap_cleanup_scanpos(pos->posvecs, pos->nvec);
}
