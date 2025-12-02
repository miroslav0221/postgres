/*-------------------------------------------------------------------------
 *
 * bitmapinsert.c
 *	  Tuple insertion in the on-disk bitmap index.
 *
 * Portions Copyright (c) 2007-2010 Greenplum Inc
 * Portions Copyright (c) 2010-2012 EMC Corporation
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/bitmap/bitmapinsert.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/tupdesc.h"
#include "access/heapam.h"
#include "bitmap.h"
#include "bitmap_private.h"
#include "access/transam.h"
#include "parser/parse_oper.h"
#include "storage/bufmgr.h"
#include "utils/builtins.h"
#include "utils/datum.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/snapmgr.h"
#include "utils/faultinjector.h"

/*
 * BMTIDLOVBuffer represents those bitmap vectors whose LOV item would be
 * stored on the specified lov_block. The array bufs stores the TIDs for
 * a distinct vector (see above). The index of the array we're up to tell
 * us the offset number of the LOV item on the lov_block.
 */

typedef struct BMTIDLOVBuffer
{
    BlockNumber lov_block;
    BMTIDBuffer *bufs[BM_MAX_LOVITEMS_PER_PAGE];
} BMTIDLOVBuffer;

/*
 * _bitmap_buildinsert() -- insert an index tuple during index creation.
 */
void
_bitmap_buildinsert(Relation rel, ItemPointerData ht_ctid, Datum *attdata,
                    bool *nulls, BMBuildState *state)
{
    TupleDesc	tupDesc;
    uint64		tidOffset;

    tidOffset = BM_IPTR_TO_INT(&ht_ctid);

    tupDesc = RelationGetDescr(rel);

    /* insert a new bit into the corresponding bitmap */
    build_inserttuple(rel, tidOffset, ht_ctid,
                      tupDesc, attdata, nulls, state);
}

/*
 * _bitmap_doinsert() -- insert an index tuple for a given tuple.
 */
void
_bitmap_doinsert(Relation rel, ItemPointerData ht_ctid, Datum *attdata,
                 bool *nulls)
{
    uint64			tidOffset;
    TupleDesc		tupDesc;
    Buffer			metabuf;
    BMMetaPage		metapage;
    Relation		lovHeap, lovIndex;
    ScanKey			scanKeys;
    IndexScanDesc	scanDesc;
    int				attno;

    tupDesc = RelationGetDescr(rel);
    if (tupDesc->natts <= 0)
        return ;

    tidOffset = BM_IPTR_TO_INT(&ht_ctid);

    /* insert a new bit into the corresponding bitmap using the HRL scheme */
    metabuf = _bitmap_getbuf(rel, BM_METAPAGE, BM_READ);
    metapage = _bitmap_get_metapage_data(rel, metabuf);
    _bitmap_open_lov_heapandindex(rel, metapage, &lovHeap, &lovIndex,
                                  RowExclusiveLock);

    LockBuffer(metabuf, BUFFER_LOCK_UNLOCK);

    scanKeys = (ScanKey) palloc0(tupDesc->natts * sizeof(ScanKeyData));

    for (attno = 0; attno < tupDesc->natts; attno++)
    {
        Oid			eq_opr;
        RegProcedure opfuncid;
        ScanKey		scanKey;

        get_sort_group_operators(TupleDescAttr(tupDesc, attno)->atttypid,
                                 false, true, false,
                                 NULL, &eq_opr, NULL, NULL);
        opfuncid = get_opcode(eq_opr);

        scanKey = (ScanKey) (((char *)scanKeys) + attno * sizeof(ScanKeyData));

        ScanKeyEntryInitialize(scanKey,
                               nulls[attno] ? SK_ISNULL : 0,
                               attno + 1,
                               BTEqualStrategyNumber,
                               InvalidOid,
                               lovIndex->rd_indcollation[attno],
                               opfuncid,
                               attdata[attno]);
    }

    scanDesc = index_beginscan(lovHeap, lovIndex, GetActiveSnapshot(),
                               tupDesc->natts, 0);
    index_rescan(scanDesc, scanKeys, tupDesc->natts, NULL, 0);

    /* insert this new tuple into the bitmap index. */
    inserttuple(rel, metabuf, tidOffset, ht_ctid, tupDesc, attdata, nulls,
                lovHeap, lovIndex, scanKeys, scanDesc, RelationNeedsWAL(rel));

    index_endscan(scanDesc);
    _bitmap_close_lov_heapandindex(lovHeap, lovIndex, RowExclusiveLock);

    ReleaseBuffer(metabuf);
    pfree(scanKeys);
}