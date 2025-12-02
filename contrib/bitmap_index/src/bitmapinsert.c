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


static uint16 buf_extend(BMTIDBuffer *buf);

static void build_inserttuple(Relation rel, uint64 tidnum,
							   ItemPointerData ht_ctid, TupleDesc tupDesc, 
							   Datum *attdata, bool *nulls, BMBuildState *state);

static uint16 buf_ensure_head_space(Relation rel, BMTIDBuffer *buf,
								   Buffer lovBuffer, OffsetNumber off,
								   bool use_wal);


static uint16 buf_free_mem_block(Relation rel, BMTIDBuffer *buf,
			  			         Buffer lovBuffer, OffsetNumber off,
						         bool use_wal);                         

#define BUF_INIT_WORDS 8 /* as good a point as any */


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
 * create_lovitem() -- create a new LOV item.
 *
 * Create a new LOV item and append this item into the last LOV page.
 * Each LOV item is associated with one distinct value for attributes
 * to be indexed. This function also inserts this distinct value along
 * with this new LOV item's block number and offsetnumber into the
 * auxiliary heap and its b-tree of this bitmap index.
 *
 * This function returns the block number and offset number of this
 * new LOV item.
 *
 * The caller should have an exclusive lock on metabuf.
 */
static void
create_lovitem(Relation rel, Buffer metabuf, uint64 tidnum,
			   TupleDesc tupDesc, Datum *attdata, bool *nulls,
			   Relation lovHeap, Relation lovIndex, BlockNumber *lovBlockP, 
			   OffsetNumber *lovOffsetP, bool use_wal)
{
	BMMetaPage		metapage;
	Buffer			currLovBuffer;
	Page			currLovPage;
	Datum*			lovDatum;
	bool*			lovNulls;
	OffsetNumber	itemSize;
	BMLOVItem		lovitem;
	int				numOfAttrs;
	bool			is_new_lov_blkno = false;

	numOfAttrs = tupDesc->natts;

	/* Get the last LOV page. Meta page should be locked. */
	metapage = _bitmap_get_metapage_data(rel, metabuf);
	*lovBlockP = metapage->bm_lov_lastpage;

	currLovBuffer = _bitmap_getbuf(rel, *lovBlockP, BM_WRITE);
	currLovPage = BufferGetPage(currLovBuffer);

	lovitem = _bitmap_formitem(tidnum);

	*lovOffsetP = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));
	itemSize = sizeof(BMLOVItemData);

	/*
	 * If there is not enough space in the last LOV page for
	 * a new item, create a new LOV page, and update the metapage.
	 */
	if (itemSize > PageGetFreeSpace(currLovPage))
	{
		Buffer		newLovBuffer;

		/* create a new LOV page */
		newLovBuffer = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
		_bitmap_init_lovpage(rel, newLovBuffer);

		_bitmap_relbuf(currLovBuffer);

		currLovBuffer = newLovBuffer;
		currLovPage = BufferGetPage(currLovBuffer);

		is_new_lov_blkno = true;
	}

	/* First create the LOV item. */
	START_CRIT_SECTION();

	if (is_new_lov_blkno)
	{
		MarkBufferDirty(metabuf);

		metapage->bm_lov_lastpage = BufferGetBlockNumber(currLovBuffer);

		*lovOffsetP = OffsetNumberNext(PageGetMaxOffsetNumber(currLovPage));
		*lovBlockP = BufferGetBlockNumber(currLovBuffer);
	}

	MarkBufferDirty(currLovBuffer);

	if (PageAddItem(currLovPage, (Item)lovitem, itemSize, *lovOffsetP,
					false, false) == InvalidOffsetNumber)
		ereport(ERROR,
				(errcode(ERRCODE_INTERNAL_ERROR),
				 errmsg("failed to add LOV item in bitmap index \"%s\""
						" (relfilenode %u/%u/%u, LOV block %d, LOV offset %d)",
						RelationGetRelationName(rel),
						rel->rd_node.spcNode, rel->rd_node.dbNode, rel->rd_node.relNode,
						*lovBlockP, *lovOffsetP)));

	if(use_wal)
		_bitmap_log_lovitem(rel, MAIN_FORKNUM, currLovBuffer, *lovOffsetP, lovitem,
							metabuf, is_new_lov_blkno);

	END_CRIT_SECTION();

	/*
	 * .. and then create the entry in the auxiliary LOV heap and index for it.
	 *
	 * This could still fail for various reasons, e.g. if you run out of disk
	 * space. In that case, we'll leave behind an "orphan" LOV item, with no
	 * corresponding item in the LOV heap. That's a bit sloppy and leaky, but
	 * harmless; the orphaned LOV item won't be encountered by any scans.
	 */
	lovDatum = palloc0((numOfAttrs + 2) * sizeof(Datum));
	lovNulls = palloc0((numOfAttrs + 2) * sizeof(bool));
	memcpy(lovDatum, attdata, numOfAttrs * sizeof(Datum));
	memcpy(lovNulls, nulls, numOfAttrs * sizeof(bool));
	lovDatum[numOfAttrs] = Int32GetDatum(*lovBlockP);
	lovNulls[numOfAttrs] = false;
	lovDatum[numOfAttrs + 1] = Int16GetDatum(*lovOffsetP);
	lovNulls[numOfAttrs + 1] = false;

	_bitmap_insert_lov(lovHeap, lovIndex, lovDatum, lovNulls, use_wal);

	_bitmap_relbuf(currLovBuffer);

	if (Debug_bitmap_print_insert)
		elog(LOG, "Bitmap Insert: create a lov item: "
			 "lovBlock=%d, lovOffset=%d, is_new_lovblock=%d, idxrelid=%u",
			 *lovBlockP, *lovOffsetP, is_new_lov_blkno, RelationGetRelid(rel));

	pfree(lovitem);
	pfree(lovDatum);
	pfree(lovNulls);
}



/*
 * When building an index we try and buffer calls to write tids to disk
 * as it will result in lots of I/Os.
 */

static void
buf_add_tid(Relation rel, BMTidBuildBuf *tids, uint64 tidnum, 
			BMBuildState *state, BlockNumber lov_block, OffsetNumber off)
{
	BMTIDBuffer *buf;
	BMTIDLOVBuffer *lov_buf = NULL;

	/* If we surpass maintenance_work_mem, free some space from the buffer */
	if (tids->byte_size >= maintenance_work_mem * 1024L)
		buf_make_space(rel, tids, state->use_wal);

	/*
	 * tids is lazily initialized. If we do not have a current LOV block 
	 * buffer, initialize one.
	 */
	if (!BlockNumberIsValid(tids->max_lov_block) || 
		tids->max_lov_block < lov_block)
	{
		/*
		 * XXX: We're currently not including the size of this data structure
		 * in out byte_size count... should we?
		 */
		lov_buf = palloc(sizeof(BMTIDLOVBuffer));
		lov_buf->lov_block = lov_block;
		MemSet(lov_buf->bufs, 0, BM_MAX_LOVITEMS_PER_PAGE * sizeof(BMTIDBuffer *));
		tids->max_lov_block = lov_block;
		
		/*
		 * Add the new LOV buffer to the list head. It seems reasonable that
		 * future calls to this function will want this lov_block rather than
		 * older lov_blocks.
		 */
		tids->lov_blocks = lcons(lov_buf, tids->lov_blocks);
	}
	else
	{
		ListCell *cell;
		
		foreach(cell, tids->lov_blocks)
		{
			BMTIDLOVBuffer *tmp = lfirst(cell);
			if(tmp->lov_block == lov_block)
			{
				lov_buf = tmp;
				break;
			}
		}
	}
	
	Assert(lov_buf);
	Assert(off - 1 < BM_MAX_LOVITEMS_PER_PAGE);

	if (lov_buf->bufs[off - 1])
	{

		buf = lov_buf->bufs[off - 1];

		Buffer lovbuf = _bitmap_getbuf(rel, lov_block, BM_WRITE);

		if (tidnum < buf->last_tid)
		{
			/*
			 * Usually, tidnum is greater than lovItem->bm_last_setbit.
			 * However, if we build bitmap index on a heap table, there could
			 * have HOT-chain, and it'll return the root tuple's tid, which could
			 * lead to the TIDs we scanned are not in order, so we need to scan
			 * through the bitmap vector, and update the bit in tidnum directly.
			 */
			_bitmap_write_new_bitmapwords(rel, lovbuf, off, buf, state->use_wal);
			_bitmap_free_tidbuf(buf);

			updatesetbit(rel, lovbuf, off, tidnum, state->use_wal);
		}
		else
			buf_add_tid_with_fill(rel, buf, lovbuf, off, tidnum, state->use_wal);

		_bitmap_relbuf(lovbuf);
	}
	else
	{
		/* no pre-existing buffer found, create a new one */
		Buffer lovbuf;
		Page page;
		BMLOVItem lovitem;
		uint16 bytes_added;
		
		buf = (BMTIDBuffer *)palloc0(sizeof(BMTIDBuffer));
		
		lovbuf = _bitmap_getbuf(rel, lov_block, BM_WRITE);
		page = BufferGetPage(lovbuf);
		lovitem = (BMLOVItem)PageGetItem(page, PageGetItemId(page, off));

		buf->last_tid = lovitem->bm_last_setbit;
		buf->last_compword = lovitem->bm_last_compword;
		buf->last_word = lovitem->bm_last_word;
		buf->is_last_compword_fill = (lovitem->lov_words_header == 2);

		MemSet(buf->hwords, 0, BM_NUM_OF_HEADER_WORDS * sizeof(BM_HRL_WORD));

		bytes_added = buf_extend(buf);

		buf->curword = 0;

		buf_add_tid_with_fill(rel, buf, lovbuf, off, tidnum, state->use_wal);

		_bitmap_relbuf(lovbuf);

		lov_buf->bufs[off - 1] = buf;
		tids->byte_size += bytes_added;
	}
}

/*
 * buf_add_tid_with_fill() -- Worker for buf_add_tid().
 *
 * Return how many bytes are used. Since we move words to disk when
 * there is no space left for new header words, this returning number
 * can be negative.
 */
static int16
buf_add_tid_with_fill(Relation rel, BMTIDBuffer *buf,
					  Buffer lovBuffer, OffsetNumber off,
					  uint64 tidnum, bool use_wal)
{
	int64 zeros;
	uint16 inserting_pos;
	int16 bytes_used = 0;

	/*
	 * Compute how many zeros between this set bit and the last inserted
	 * set bit.
	 */
	zeros = tidnum - buf->last_tid - 1;

	if (zeros > 0)
	{
		uint64 zerosNeeded;
		uint64 numOfTotalFillWords;

		/* 
		 * Calculate how many bits are needed to fill up the existing last
		 * bitmap word.
		 */
		zerosNeeded =
			BM_HRL_WORD_SIZE - ((buf->last_tid - 1) % BM_HRL_WORD_SIZE) - 1;

		if (zerosNeeded > 0 && zeros >= zerosNeeded)
		{
			/*
			 * The last bitmap word is complete now. We merge it with the
			 * last bitmap complete word.
			 */
			bytes_used -=
				buf_ensure_head_space(rel, buf, lovBuffer, off, use_wal);

			bytes_used += mergewords(buf, false);
			zeros -= zerosNeeded;
		}

		/*
		 * If the remaining zeros are more than BM_HRL_WORD_SIZE,
		 * We construct the last bitmap word to be a fill word, and merge it
		 * with the last complete bitmap word.
		 */
		numOfTotalFillWords = zeros/BM_HRL_WORD_SIZE;

		while (numOfTotalFillWords > 0)
		{
			BM_HRL_WORD 	numOfFillWords;

			CHECK_FOR_INTERRUPTS();

			if (numOfTotalFillWords >= MAX_FILL_LENGTH)
				numOfFillWords = MAX_FILL_LENGTH;
			else
				numOfFillWords = numOfTotalFillWords;

			buf->last_word = BM_MAKE_FILL_WORD(0, numOfFillWords);

			bytes_used -= 
				buf_ensure_head_space(rel, buf, lovBuffer, off, use_wal);
			bytes_used += mergewords(buf, true);

			numOfTotalFillWords -= numOfFillWords;
			zeros -= ((uint64)numOfFillWords * BM_HRL_WORD_SIZE);
		}
	}

	Assert((zeros >= 0) && (zeros<BM_HRL_WORD_SIZE));
	
	inserting_pos = (tidnum-1)%BM_HRL_WORD_SIZE;
	buf->last_word |= (((BM_HRL_WORD)1) << inserting_pos);

	if (tidnum % BM_HRL_WORD_SIZE == 0)
	{
		bool lastWordFill = false;

		if (buf->last_word == LITERAL_ALL_ZERO)
		{
			buf->last_word = BM_MAKE_FILL_WORD(0, 1);
			lastWordFill = true;
		}

		else if (buf->last_word == LITERAL_ALL_ONE)
		{
			buf->last_word = BM_MAKE_FILL_WORD(1, 1);
			lastWordFill = true;
		}

		bytes_used -=
			buf_ensure_head_space(rel, buf, lovBuffer, off, use_wal);
		bytes_used += mergewords(buf, lastWordFill);
	}

	buf->last_tid = tidnum;

	return bytes_used;
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



/*
 * buf_ensure_head_space() -- If there is no space in the header words,
 * move words in the given buffer to disk and free the existing space,
 * and then allocate new space for future new words.
 *
 * The number of bytes freed are returned.
 */
static uint16
buf_ensure_head_space(Relation rel, BMTIDBuffer *buf, 
					  Buffer lovBuffer, OffsetNumber off, bool use_wal)
{
	uint16 bytes_freed = 0;

	if (buf->curword >= (BM_NUM_OF_HEADER_WORDS * BM_HRL_WORD_SIZE))
	{
		bytes_freed = buf_free_mem_block(rel, buf, lovBuffer, off, use_wal);
		bytes_freed -= buf_extend(buf);
	}

	return bytes_freed;
}

/*
 * buf_extend() -- Enlarge the memory allocated to a buffer.
 * Return how many bytes are added to the buffer.
 */
static uint16
buf_extend(BMTIDBuffer *buf)
{
	uint16 bytes;
	uint16 size;
	
	if (buf->num_cwords > 0 && buf->curword < buf->num_cwords - 1)
		return 0; /* already large enough */

	if(buf->num_cwords == 0)
	{
		size = BUF_INIT_WORDS;
		buf->cwords = (BM_HRL_WORD *)
			palloc0(BUF_INIT_WORDS * sizeof(BM_HRL_WORD));
		buf->last_tids = (uint64 *)palloc0(BUF_INIT_WORDS * sizeof(uint64));
		bytes = BUF_INIT_WORDS * sizeof(BM_HRL_WORD) +
			BUF_INIT_WORDS * sizeof(uint64);
	}
	else
	{
		size = buf->num_cwords;
		buf->cwords = repalloc(buf->cwords, 2 * size * sizeof(BM_HRL_WORD));
		MemSet(buf->cwords + size, 0, size * sizeof(BM_HRL_WORD));
		buf->last_tids = repalloc(buf->last_tids, 2 * size * sizeof(uint64));
		MemSet(buf->last_tids + size, 0, size * sizeof(uint64));
		bytes = 2 * size * sizeof(BM_HRL_WORD) +
			2 * size * sizeof(uint64);
	}
	buf->num_cwords += size;
	return bytes;
}