/*-------------------------------------------------------------------------
 *
 * bitmapattutil.c
 *	  Defines the routines to maintain all distinct attribute values
 *	  which are indexed in the on-disk bitmap index.
 *
 * Portions Copyright (c) 2007-2010 Greenplum Inc
 * Portions Copyright (c) 2010-2012 EMC Corporation
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/bitmap/bitmapattutil.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/tupdesc.h"
#include "bitmap.h"
#include "bitmap_private.h"
#include "access/heapam.h"
#include "access/heapam_xlog.h"
#include "access/multixact.h"
#include "access/nbtree.h"
#include "access/xact.h"
#include "access/transam.h"
#include "catalog/dependency.h"
#include "catalog/heap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "catalog/namespace.h"
#include "catalog/catalog.h"
#include "catalog/pg_namespace.h"
#include "commands/defrem.h"
#include "commands/tablecmds.h"
#include "nodes/execnodes.h"
#include "nodes/primnodes.h"
#include "nodes/makefuncs.h"
#include "optimizer/clauses.h"
#include "utils/builtins.h"
#include "utils/faultinjector.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "utils/snapmgr.h"


/*
 * _bitmap_findvalue() -- find a row in a given heap using
 *  a given index that satisfies the given scan key.
 *
 * If this value exists, this function returns true. Otherwise,
 * returns false.
 *
 * If this value exists in the heap, this function also returns
 * the block number and the offset number that are stored in the same
 * row with this value. This block number and the offset number
 * are for the LOV item that points the bitmap vector for this value.
 */
bool
_bitmap_findvalue(Relation lovHeap, Relation lovIndex,
				  ScanKey scanKey pg_attribute_unused(), IndexScanDesc scanDesc,
				  BlockNumber *lovBlock, bool *blockNull,
				  OffsetNumber *lovOffset, bool *offsetNull)
{
	TupleDesc		tupDesc;
	bool			found = false;
	TupleTableSlot *slot;

	tupDesc = RelationGetDescr(lovIndex);

	/*
	 * creating a new slot on every call is a bit expensive, but there's no
	 * convenient place to keep it.
	 */
	slot = table_slot_create(lovHeap, NULL);
	if (index_getnext_slot(scanDesc, ForwardScanDirection, slot))
	{
		Datum 		d;

		found = true;

		d = slot_getattr(slot, tupDesc->natts + 1, blockNull);
		*lovBlock =	DatumGetInt32(d);
		d = slot_getattr(slot, tupDesc->natts + 2, offsetNull);
		*lovOffset = DatumGetInt16(d);
	}

	ExecDropSingleTupleTableSlot(slot);

	return found;
}

/*
 * _bitmap_open_lov_heapandindex() -- open the heap relation and the btree
 *		index for LOV.
 */
void
_bitmap_open_lov_heapandindex(Relation rel pg_attribute_unused(), BMMetaPage metapage,
                              Relation *lovHeapP, Relation *lovIndexP,
                              LOCKMODE lockMode)
{
    *lovHeapP = heap_open(metapage->bm_lov_heapId, lockMode);
    *lovIndexP = index_open(metapage->bm_lov_indexId, lockMode);
}

/*
 * _bitmap_close_lov_heapandindex() -- close the heap and the index.
 */
void
_bitmap_close_lov_heapandindex(Relation lovHeap, Relation lovIndex,
                               LOCKMODE lockMode)
{
    heap_close(lovHeap, lockMode);
    index_close(lovIndex, lockMode);
}

/*
 * _bitmap_insert_lov() -- insert a new data into the given heap and index.
 */
void
_bitmap_insert_lov(Relation lovHeap, Relation lovIndex, Datum *datum,
                   bool *nulls, bool use_wal pg_attribute_unused())
{
    TupleDesc	tupDesc;
    HeapTuple	tuple;
    bool		result;
    Datum	   *indexDatum;
    bool	   *indexNulls;

    tupDesc = RelationGetDescr(lovHeap);

    /* insert this tuple into the heap */
    tuple = heap_form_tuple(tupDesc, datum, nulls);
    simple_heap_insert(lovHeap, tuple);

    /* insert a new tuple into the index */
    indexDatum = palloc0((tupDesc->natts - 2) * sizeof(Datum));
    indexNulls = palloc0((tupDesc->natts - 2) * sizeof(bool));
    memcpy(indexDatum, datum, (tupDesc->natts - 2) * sizeof(Datum));
    memcpy(indexNulls, nulls, (tupDesc->natts - 2) * sizeof(bool));
    result = index_insert(lovIndex, indexDatum, indexNulls,
                          &(tuple->t_self), lovHeap, true, false, NULL);

#ifdef FAULT_INJECTOR
    FaultInjector_InjectFaultIfSet(
							"insert_bmlov_before_freeze",
							DDLNotSpecified,
							"", //databaseName
							RelationGetRelationName(lovHeap));
#endif
    /* freeze the tuple */
    heap_freeze_tuple_wal_logged(lovHeap, tuple);

#ifdef FAULT_INJECTOR
    FaultInjector_InjectFaultIfSet(
							"insert_bmlov_after_freeze",
							DDLNotSpecified,
							"", //databaseName
							RelationGetRelationName(lovHeap));
#endif
    pfree(indexDatum);
    pfree(indexNulls);
    Assert(result);

    heap_freetuple(tuple);
}