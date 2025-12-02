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


static void build_inserttuple(Relation rel, uint64 tidnum,
							   ItemPointerData ht_ctid, TupleDesc tupDesc, 
							   Datum *attdata, bool *nulls, BMBuildState *state);



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



/*
 * build_inserttuple() -- insert a new tuple into the bitmap index
 *	during the bitmap index construction.
 *
 * Each new tuple has an assigned number -- tidnum, called a
 * tid location, which represents the bit location for this tuple in
 * a bitmap vector. To speed up the construction, this function does not
 * write this tid location into its bitmap vector immediately. We maintain
 * a buffer -- BMTidBuildBuf to keep an array of tid locations
 * for each distinct attribute value.
 *
 * If this insertion causes the buffer to overflow, we write tid locations
 * for enough distinct values to disk to accommodate this new tuple.
 */
static void
build_inserttuple(Relation rel, uint64 tidnum,
				  ItemPointerData ht_ctid  pg_attribute_unused(), TupleDesc tupDesc,
				  Datum *attdata, bool *nulls, BMBuildState *state)
{
	Buffer 			metabuf;
	BlockNumber		lovBlock;
	OffsetNumber	lovOffset;
	bool			blockNull;
	bool			offsetNull;
	BMTidBuildBuf *tidLocsBuffer;
	int				attno;
	bool			allNulls = true;
	BMBuildHashKey  *entry;

	CHECK_FOR_INTERRUPTS();

	tidLocsBuffer = state->bm_tidLocsBuffer;

	/* Check if all attributes have value of NULL. */
	for (attno = 0; attno < tupDesc->natts; attno++)
	{
		if (!nulls[attno])
		{
			allNulls = false;
			break;
		}
	}

	metabuf = _bitmap_getbuf(rel, BM_METAPAGE, BM_WRITE);
	
	/*
	 * if the inserting tuple has the value of NULL, then
	 * the corresponding tid array is the first.
	 */
	if (allNulls)
	{
		lovBlock = BM_LOV_STARTPAGE;
		lovOffset = 1;
	}
	else
	{
		bool found;
		BMBuildLovData *lov;

		if (state->lovitem_hash)
		{
		    BMBuildHashKey toLookup;
		    toLookup.attributeValueArr = attdata;
		    toLookup.isNullArr = nulls;

			/* look up the hash to see if we can find the lov data that way */
			entry = (BMBuildHashKey *)hash_search(state->lovitem_hash,
												  (void*)&toLookup,
												  HASH_ENTER, &found);
			if (!found)
			{
				/* Copy the key values in case someone modifies them */
				for(attno = 0; attno < tupDesc->natts; attno++)
				{
					Form_pg_attribute at = TupleDescAttr(tupDesc, attno);

					if (entry->isNullArr[attno])
						entry->attributeValueArr[attno] = 0;
					else
						entry->attributeValueArr[attno] = datumCopy(entry->attributeValueArr[attno], at->attbyval,
																	at->attlen);
				}

				/*
				 * If the inserting tuple has a new value, then we create a new
				 * LOV item.
				 */
				create_lovitem(rel, metabuf, tidnum, tupDesc, attdata, 
							   nulls, state->bm_lov_heap, state->bm_lov_index,
							   &lovBlock, &lovOffset, state->use_wal);

				lov = (BMBuildLovData *) (((char*)entry) + state->lovitem_hashKeySize );
				lov->lov_block = lovBlock;
				lov->lov_off = lovOffset;
			}

			else
			{
				lov = (BMBuildLovData *) (((char*)entry) + state->lovitem_hashKeySize );
				lovBlock = lov->lov_block;
				lovOffset = lov->lov_off;
			}
		}

		else {
			/*
			 * Search the btree to find the right bitmap vector to append
			 * this bit. Here, we reset the scan key and call index_rescan.
			 */
			for (attno = 0; attno<tupDesc->natts; attno++)
			{
				ScanKey theScanKey = (ScanKey)(((char*)state->bm_lov_scanKeys) +
											   attno * sizeof(ScanKeyData));
				if (nulls[attno])
				{
					theScanKey->sk_flags = SK_ISNULL;
					theScanKey->sk_argument = attdata[attno];
				}
				else
				{
					theScanKey->sk_flags = 0;
					theScanKey->sk_argument = attdata[attno];
				}
			}

			index_rescan(state->bm_lov_scanDesc, state->bm_lov_scanKeys, tupDesc->natts, NULL, 0);

			found = _bitmap_findvalue(state->bm_lov_heap, state->bm_lov_index,
									  state->bm_lov_scanKeys, state->bm_lov_scanDesc,
									  &lovBlock, &blockNull, &lovOffset, &offsetNull);

			if (!found)
			{
				/*
				 * If the inserting tuple has a new value, then we create a new
				 * LOV item.
				 */
				create_lovitem(rel, metabuf, tidnum, tupDesc, attdata, 
							   nulls, state->bm_lov_heap, state->bm_lov_index,
							   &lovBlock, &lovOffset, state->use_wal);
			}
		}
	}

	buf_add_tid(rel, tidLocsBuffer, tidnum, state, lovBlock, lovOffset);
	_bitmap_wrtbuf(metabuf);

	CHECK_FOR_INTERRUPTS();
}

