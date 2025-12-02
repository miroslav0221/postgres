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


static void _bitmap_write_new_bitmapwords(Relation rel,
							  Buffer lovBuffer, OffsetNumber lovOffset,
							  BMTIDBuffer* buf, bool use_wal);

static uint16 buf_extend(BMTIDBuffer *buf);

static void build_inserttuple(Relation rel, uint64 tidnum,
							   ItemPointerData ht_ctid, TupleDesc tupDesc, 
							   Datum *attdata, bool *nulls, BMBuildState *state);

static uint16 buf_ensure_head_space(Relation rel, BMTIDBuffer *buf,
								   Buffer lovBuffer, OffsetNumber off,
								   bool use_wal);
                                   
								   
static uint16 _bitmap_free_tidbuf(BMTIDBuffer* buf);
static void updatesetbit(Relation rel, 
						 Buffer lovBuffer, OffsetNumber lovOffset,
						 uint64 tidnum, bool use_wal);
static void updatesetbit_inword(BM_HRL_WORD word, uint64 updateBitLoc,
								uint64 firstTid, BMTIDBuffer* buf);
static void updatesetbit_inpage(Relation rel, uint64 tidnum,
								Buffer lovBuffer, OffsetNumber lovOffset,
								Buffer bitmapBuffer, uint64 firstTidNumber,
								bool use_wal);
static void insertsetbit(Relation rel, BlockNumber lovBlock, OffsetNumber lovOffset,
			 			 uint64 tidnum, BMTIDBuffer *buf, bool use_wal);


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

	// if(use_wal)
	// 	_bitmap_log_lovitem(rel, MAIN_FORKNUM, currLovBuffer, *lovOffsetP, lovitem,
	// 						metabuf, is_new_lov_blkno);

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

	// if (Debug_bitmap_print_insert)
	// 	elog(LOG, "Bitmap Insert: create a lov item: "
	// 		 "lovBlock=%d, lovOffset=%d, is_new_lovblock=%d, idxrelid=%u",
	// 		 *lovBlockP, *lovOffsetP, is_new_lov_blkno, RelationGetRelid(rel));

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

/*
 * _bitmap_write_new_bitmapwords() -- write a given buffer of new bitmap words
 * 	into the end of bitmap page(s).
 *
 * If the last bitmap page does not have enough space for all these new
 * words, new pages will be allocated here.
 *
 * We consider a write to one bitmap page as one atomic-action WAL
 * record. The WAL record for the write to the last bitmap page also
 * includes updates on the lov item. Writes to the non-last
 * bitmap page are not self-consistent. We need to do some fix-up
 * during WAL logic replay.
 */
static void
_bitmap_write_new_bitmapwords(Relation rel,
							  Buffer lovBuffer, OffsetNumber lovOffset,
							  BMTIDBuffer* buf, bool use_wal)
{
	Page		lovPage;
	BMLOVItem	lovItem;
	bool		first_page_needs_init = false;
	List	   *perpage_buffers = NIL;
	List	   *perpage_xlrecs = NIL;
	ListCell   *lcb;

	lovPage = BufferGetPage(lovBuffer);
	lovItem = (BMLOVItem) PageGetItem(lovPage,
									  PageGetItemId(lovPage, lovOffset));

	/*
	 * Write changes to bitmap pages, if needed. (We might get away by
	 * updating just the last words stored on the LOV item.)
	 */
	if (buf->curword > 0)
	{
		ListCell   *lcp;
		BlockNumber first_blkno;
		BlockNumber last_blkno;
		List	   *perpage_tmppages = NIL;
		bool		is_first;
		ListCell   *buffer_cell;
		int			start_wordno;

		/*
		 * Write bitmap words, one page at a time, allocating new pages as
		 * required.
		 */
		is_first = true;
		start_wordno = 0;
		do
		{
			Buffer		bitmapBuffer;
			bool		bitmapBufferNeedsInit = false;
			Page		bitmapPage;
			BMBitmapOpaque	bitmapPageOpaque;
			uint32		numFreeWords;
			uint32		words_written;
			xl_bm_bitmapwords_perpage *xlrec_perpage;
			Page		tmppage = NULL;

			if (is_first && lovItem->bm_lov_head != InvalidBlockNumber)
			{
				bitmapBuffer = _bitmap_getbuf(rel, lovItem->bm_lov_tail, BM_WRITE);

				/* Append to an existing LOV page as much as fits */
				bitmapPage = BufferGetPage(bitmapBuffer);
				bitmapPageOpaque =
					(BMBitmapOpaque) PageGetSpecialPointer(bitmapPage);

				numFreeWords = BM_NUM_OF_HRL_WORDS_PER_PAGE -
					bitmapPageOpaque->bm_hrl_words_used;
			}
			else
			{
				/* Allocate new page */
				bitmapBuffer = _bitmap_getbuf(rel, P_NEW, BM_WRITE);
				bitmapBufferNeedsInit = true;
				numFreeWords = BM_NUM_OF_HRL_WORDS_PER_PAGE;
			}

			/*
			 * Remember information about the first page, needed
			 * for updating the LOV and for the WAL record.
			 */
			if (is_first)
			{
				first_blkno = BufferGetBlockNumber(bitmapBuffer);
				first_page_needs_init = bitmapBufferNeedsInit;
			}

			if (use_wal)
			{
				xlrec_perpage = palloc0(sizeof(xl_bm_bitmapwords_perpage));
				xlrec_perpage->bmp_blkno = BufferGetBlockNumber(bitmapBuffer);
			}
			else
				xlrec_perpage = NULL;
			perpage_buffers = lappend_int(perpage_buffers, bitmapBuffer);
			perpage_xlrecs = lappend(perpage_xlrecs, xlrec_perpage);

			if (list_length(perpage_buffers) > MAX_BITMAP_PAGES_PER_INSERT)
				elog(ERROR, "too many bitmap pages in one insert batch into bitmap index %u"
					 " (relfilenode %u/%u/%u, LOV block %d, LOV offset %d)",
					 RelationGetRelid(rel),
					 rel->rd_node.spcNode, rel->rd_node.dbNode, rel->rd_node.relNode,
					 BufferGetBlockNumber(lovBuffer), lovOffset);

			/*
			 * Allocate a new temporary page to operate on, in case we fail
			 * half-way through the updates (because of running out of memory
			 * or disk space, most likely). If this is the last bitmap page,
			 * i.e. we can fit all the remaining words on this bitmap page,
			 * though, we can skip that, and modify the page directly.
			 *
			 * If this is not the last page, we will need to allocate more
			 * pages. That in turn might fail, so we must not modify the
			 * existing pages yet.
			 */
			if (numFreeWords < buf->curword - start_wordno)
			{
				/*
				 * Does not fit, we will need to expand.
				 *
				 * Note: we don't write to the page until we're sure we get
				 * all of them. We do all the action on temp copies.
				 */
				if (bitmapBufferNeedsInit)
				{
					tmppage = palloc(BLCKSZ);
					_bitmap_init_bitmappage(tmppage);
				}
				else
					tmppage = PageGetTempPageCopy(BufferGetPage(bitmapBuffer));

				bitmapPage = tmppage;

				perpage_tmppages = lappend(perpage_tmppages, tmppage);
			}
			else
			{
				/*
				 * This is the last page. Now that we have successfully
				 * fetched/allocated it, none of the things we do should
				 * ereport(), so we can make the changes directly to the
				 * buffer.
				 */
				bitmapPage = BufferGetPage(bitmapBuffer);
				START_CRIT_SECTION();
				if (bitmapBufferNeedsInit)
					_bitmap_init_bitmappage(bitmapPage);
			}

			words_written =
				_bitmap_write_bitmapwords_on_page(bitmapPage, buf,
												  start_wordno, xlrec_perpage);
			Assert(is_first || words_written > 0);
			start_wordno += words_written;

			if (bitmapPage != tmppage)
				MarkBufferDirty(bitmapBuffer);

			last_blkno = BufferGetBlockNumber(bitmapBuffer);
			is_first = false;
		} while (buf->curword - start_wordno > 0);
		Assert(start_wordno == buf->curword);

		/*
		 * Ok, we have locked all the pages we need. Apply any changes we had made on
		 * temporary pages.
		 *
		 * NOTE: there is one fewer temppage.
		 */
		buffer_cell = list_head(perpage_buffers);
		foreach(lcp, perpage_tmppages)
		{
			Page		tmppage = (Page) lfirst(lcp);
			Buffer		buffer = (Buffer) lfirst_int(buffer_cell);
			Page		page = BufferGetPage(buffer);
			BlockNumber nextBlkNo;
			BMBitmapOpaque	bitmapPageOpaque;

			PageRestoreTempPage(tmppage, page);
			MarkBufferDirty(buffer);

			/* Update the 'next' pointer on this page, before moving on */
			buffer_cell = lnext(perpage_buffers, buffer_cell);
			Assert(buffer_cell);
			nextBlkNo = BufferGetBlockNumber((Buffer) lfirst_int(buffer_cell));

			bitmapPageOpaque =
				(BMBitmapOpaque) PageGetSpecialPointer(page);

			bitmapPageOpaque->bm_bitmap_next = nextBlkNo;
		}
		list_free(perpage_tmppages);

		/* Update the bitmap page pointers in the LOV item */
		if (first_page_needs_init)
			lovItem->bm_lov_head = first_blkno;
		lovItem->bm_lov_tail = last_blkno;
	}
	else
	{
		START_CRIT_SECTION();
	}

	/* Update LOV item (lov_head/tail were updated above already) */
	lovItem->bm_last_compword = buf->last_compword;
	lovItem->bm_last_word = buf->last_word;
	lovItem->lov_words_header = (buf->is_last_compword_fill) ? 2 : 0;
	lovItem->bm_last_setbit = buf->last_tid;
	lovItem->bm_last_tid_location = buf->last_tid - buf->last_tid % BM_HRL_WORD_SIZE;
	MarkBufferDirty(lovBuffer);

	/* Write WAL record */
	// if (use_wal)
	// {
	// 	if (buf->curword > 0)
	// 		_bitmap_log_bitmapwords(rel, buf,
	// 								first_page_needs_init, perpage_xlrecs, perpage_buffers,
	// 								lovBuffer, lovOffset, buf->last_tid);
	// 	else
	// 		_bitmap_log_bitmap_lastwords(rel, lovBuffer, lovOffset, lovItem);
	// }

	END_CRIT_SECTION();

	// if (Debug_bitmap_print_insert)
	// 	elog(LOG, "Bitmap Insert: write bitmapwords: numwords=%d"
	// 		 ", last_tid=" INT64_FORMAT
	// 		 ", lov_blkno=%d, lov_offset=%d, lovItem->bm_last_setbit=" INT64_FORMAT
	// 		 ", lovItem->bm_last_tid_location=" INT64_FORMAT
	// 		 ", idxrelid=%u",
	// 		 (int) buf->curword, buf->last_tid,
	// 		 BufferGetBlockNumber(lovBuffer), lovOffset,
	// 		 lovItem->bm_last_setbit, lovItem->bm_last_tid_location,
	// 		 RelationGetRelid(rel));

	/* release all bitmap buffers. */
	foreach(lcb, perpage_buffers)
	{
		UnlockReleaseBuffer((Buffer) lfirst_int(lcb));
	}
}

/*
 * _bitmap_free_tidbuf() -- release the space.
 */
static uint16
_bitmap_free_tidbuf(BMTIDBuffer* buf)
{
	uint16 bytes_freed = 0;

	if (buf->last_tids)
		pfree(buf->last_tids);
	if (buf->cwords)
		pfree(buf->cwords);

	bytes_freed = buf->num_cwords * sizeof(BM_HRL_WORD) +
		buf->num_cwords * sizeof(uint64);

	buf->num_cwords = 0;
	buf->curword = 0;
	/* Paranoia */
	MemSet(buf->hwords, 0, sizeof(BM_HRL_WORD) * BM_NUM_OF_HEADER_WORDS);

	return bytes_freed;
}

/*
 * updatesetbit() -- update a set bit in a bitmap.
 *
 * This function finds the bit in a given bitmap vector whose bit location is
 * equal to tidnum, and changes this bit to 1.
 *
 * If this bit is already 1, then we are done. Otherwise, there are
 * two possibilities:
 * (1) This bit appears in a literal word. In this case, we simply change
 *     it to 1.
 * (2) This bit appears in a fill word with bit 0. In this case, this word
 *     may generate two or three words after changing the corresponding bit
 *     to 1, depending on the position of this bit.
 *
 * Case (2) will make the corresponding bitmap page to grow. The words after
 * this affected word in this bitmap page are shifted right to accommodate
 * the newly generated words. If this bitmap page does not have enough space
 * to hold all these words, the last few words will be shifted out of this
 * page. In this case, the next bitmap page is checked to see if there are
 * enough space for these extra words. If so, these extra words are inserted
 * into the next page. Otherwise, we create a new bitmap page to hold
 * these extra words.
 */
static void
updatesetbit(Relation rel, Buffer lovBuffer, OffsetNumber lovOffset,
			 uint64 tidnum, bool use_wal)
{
	Page		lovPage;
	BMLOVItem	lovItem;
		
	uint64	tidLocation;
	uint16	insertingPos;

	uint64	firstTidNumber = 1;
	Buffer	bitmapBuffer = InvalidBuffer;

	lovPage = BufferGetPage(lovBuffer);
	lovItem = (BMLOVItem) PageGetItem(lovPage, 
		PageGetItemId(lovPage, lovOffset));

	/* Calculate the tid location in the last bitmap page. */
	tidLocation = lovItem->bm_last_tid_location;
	if (BM_LAST_COMPWORD_IS_FILL(lovItem))
		tidLocation -= (FILL_LENGTH(lovItem->bm_last_compword) *
					    BM_HRL_WORD_SIZE);
	else
		tidLocation -= BM_HRL_WORD_SIZE;

	/*
	 * If tidnum is in either bm_last_compword or bm_last_word,
	 * and this does not generate any new words, we simply
	 * need to update the lov item.
	 */
	if ((tidnum > lovItem->bm_last_tid_location) ||
		((tidnum > tidLocation) &&
		 ((lovItem->lov_words_header == 0) ||
		  (FILL_LENGTH(lovItem->bm_last_compword) == 1))))
	{
		START_CRIT_SECTION();

		MarkBufferDirty(lovBuffer);

		if (tidnum > lovItem->bm_last_tid_location)   /* bm_last_word */
		{
			insertingPos = (tidnum-1)%BM_HRL_WORD_SIZE;
			lovItem->bm_last_word |= (((BM_HRL_WORD)1)<<insertingPos);
			
			// if (Debug_bitmap_print_insert)
			// 	elog(LOG, "Bitmap Insert: updated a set bit in lovItem->bm_last_word"
			// 		 " pos %d"
			// 		 ", lovBlock=%d, lovOffset=%d"
			// 		 ", tidnum=" INT64_FORMAT,
			// 		 insertingPos,
			// 		 BufferGetBlockNumber(lovBuffer),
			// 		 lovOffset,
			// 		 tidnum);
		}
		else /* bm_last_compword */
		{
			if (BM_LAST_COMPWORD_IS_FILL(lovItem))
			{
				if (GET_FILL_BIT(lovItem->bm_last_compword) == 1)
					lovItem->bm_last_compword = LITERAL_ALL_ONE;
				else
					lovItem->bm_last_compword = 0;
			}

			insertingPos = (tidnum - 1) % BM_HRL_WORD_SIZE;
			lovItem->bm_last_compword |= (((BM_HRL_WORD)1) << insertingPos);
			if (lovItem->bm_last_compword == LITERAL_ALL_ONE)
			{
				lovItem->lov_words_header = 2;
				lovItem->bm_last_compword = BM_MAKE_FILL_WORD(1, 1);
			}
			else
				lovItem->lov_words_header = 0;

			// if (Debug_bitmap_print_insert)
			// 	elog(LOG, "Bitmap Insert: updated a set bit in lovItem->bm_last_compword"
			// 		 " pos %d"
			// 		 ", lovBlock=%d, lovOffset=%d"
			// 		 ", tidnum=" INT64_FORMAT,
			// 		 insertingPos,
			// 		 BufferGetBlockNumber(lovBuffer),
			// 		 lovOffset,
			// 		 tidnum);
		}

		// if (use_wal)
		// 	_bitmap_log_bitmap_lastwords(rel, lovBuffer, lovOffset, lovItem);

		END_CRIT_SECTION();

		return;
	}

	/*
	 * Here, if tidnum is still in bm_last_compword, we know that
	 * bm_last_compword is a fill zero words with fill length greater
	 * than 1. This update will generate new words, we insert new words
	 * into the last bitmap page and update the lov item.
	 */
	if ((tidnum > tidLocation) && (lovItem->lov_words_header >= 2))
	{
		/*
		 * We know that bm_last_compwords will be split into two
		 * or three words, depending on the splitting position.
		 */
		BMTIDBuffer buf;
		MemSet(&buf, 0, sizeof(buf));
		buf_extend(&buf);

		updatesetbit_inword(lovItem->bm_last_compword,
							tidnum - tidLocation - 1,
							tidLocation + 1, &buf);

		/* set the last_compword and last_word */
		buf.last_compword = buf.cwords[buf.curword-1];
		buf.is_last_compword_fill = IS_FILL_WORD(buf.hwords, buf.curword-1);
		buf.curword--;
		buf.last_word = lovItem->bm_last_word;
		buf.last_tid = lovItem->bm_last_setbit;
		_bitmap_write_new_bitmapwords(rel, lovBuffer, lovOffset,
									  &buf, use_wal);

		// if (Debug_bitmap_print_insert)
		// 	verify_bitmappages(rel, lovItem);
		
		_bitmap_free_tidbuf(&buf);

		return;
	}

	/*
	 * Now, tidnum is in the middle of the bitmap vector.
	 * We try to find the bitmap page that contains this bit,
	 * and update the bit.
	 */
	/* find the page that contains this bit. */
	findbitmappage(rel, lovItem, tidnum,
				   &bitmapBuffer, &firstTidNumber);

	updatesetbit_inpage(rel, tidnum, lovBuffer, lovOffset,
						bitmapBuffer, firstTidNumber, use_wal);

	_bitmap_relbuf(bitmapBuffer);

	// if (Debug_bitmap_print_insert)
	// 	verify_bitmappages(rel, lovItem);
}
