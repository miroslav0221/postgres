/*-------------------------------------------------------------------------
 *
 * bitmaputil.c
 *	  Utility routines for on-disk bitmap index access method.
 *
 * Portions Copyright (c) 2007-2010 Greenplum Inc
 * Portions Copyright (c) 2010-2012 EMC Corporation
 * Portions Copyright (c) 2012-Present VMware, Inc. or its affiliates.
 * Portions Copyright (c) 2006-2008, PostgreSQL Global Development Group
 *
 *
 * IDENTIFICATION
 *	  src/backend/access/bitmap/bitmaputil.c
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "bitmap.h"
#include "bitmap_private.h"
#include "bitmap_xlog.h"
#include "access/heapam.h"
#include "access/reloptions.h"
#include "access/relscan.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"

/*
 * _bitmap_get_metapage_data() -- return the metadata info stored
 * in the given metapage buffer.
 */
BMMetaPage
_bitmap_get_metapage_data(Relation rel, Buffer metabuf)
{
    Page page;
    BMMetaPage metapage;

    page = BufferGetPage(metabuf);
    metapage = (BMMetaPage)PageGetContents(page);

    /*
     * If this metapage is from the pre 3.4 version of the bitmap
     * index, we print "require to reindex" message, and error
     * out.
     */
    if (metapage->bm_version != BITMAP_VERSION)
    {
        ereport(ERROR,
                (errcode(ERRCODE_INTERNAL_ERROR),
                        errmsg("the disk format for \"%s\" is not valid for this version of Apache Cloudberry",
                               RelationGetRelationName(rel)),
                        errhint("Use REINDEX to update this index.")));
    }

    return metapage;
}

/*
 * _bitmap_formitem() -- construct a LOV entry.
 *
 * If the given tid number is greater than BM_HRL_WORD_SIZE, we
 * construct the first fill word for this bitmap vector.
 */
BMLOVItem
_bitmap_formitem(uint64 currTidNumber)
{
    BMLOVItem	bmitem;

    bmitem = (BMLOVItem) palloc(sizeof(BMLOVItemData));

    bmitem->bm_lov_head = bmitem->bm_lov_tail = InvalidBlockNumber;
    bmitem->bm_last_setbit = 0;
    bmitem->bm_last_compword = LITERAL_ALL_ONE;
    bmitem->bm_last_word = LITERAL_ALL_ZERO;
    bmitem->lov_words_header = 0;
    bmitem->bm_last_tid_location = 0;

    /* fill up all existing bits with 0. */
    if (currTidNumber <= BM_HRL_WORD_SIZE)
    {
        bmitem->bm_last_compword = LITERAL_ALL_ONE;
        bmitem->bm_last_word = LITERAL_ALL_ZERO;
        bmitem->lov_words_header = 0;
        bmitem->bm_last_tid_location = 0;
    }
    else
    {
        uint64		numOfTotalFillWords;
        BM_HRL_WORD	numOfFillWords;

        numOfTotalFillWords = (currTidNumber-1)/BM_HRL_WORD_SIZE;

        numOfFillWords = (numOfTotalFillWords >= MAX_FILL_LENGTH) ?
                         MAX_FILL_LENGTH : numOfTotalFillWords;

        bmitem->bm_last_compword = BM_MAKE_FILL_WORD(0, numOfFillWords);
        bmitem->bm_last_word = LITERAL_ALL_ZERO;
        bmitem->lov_words_header = 2;
        bmitem->bm_last_tid_location = numOfFillWords * BM_HRL_WORD_SIZE;

        bmitem->bm_last_setbit = numOfFillWords*BM_HRL_WORD_SIZE;
    }

    return bmitem;
}



/*
 * _bitmap_cleanup_batchwords() -- release spaces allocated for the BMBatchWords.
 */
void _bitmap_cleanup_batchwords(BMBatchWords* words)
{
	if (words == NULL)
		return;

	if (words->hwords)
		pfree(words->hwords);
	if (words->cwords)
		pfree(words->cwords);
}

/*
 * _bitmap_cleanup_scanpos() -- release space allocated for
 * 	BMVector.
 */
void
_bitmap_cleanup_scanpos(BMVector bmScanPos, uint32 numBitmapVectors)
{
	uint32 keyNo;

	if (numBitmapVectors == 0)
	{
		return;
	}
		
	for (keyNo=0; keyNo<numBitmapVectors; keyNo++)
	{
		if (BufferIsValid((bmScanPos[keyNo]).bm_lovBuffer))
			ReleaseBuffer((bmScanPos[keyNo]).bm_lovBuffer);

		_bitmap_cleanup_batchwords((bmScanPos[keyNo]).bm_batchWords);
		if (bmScanPos[keyNo].bm_batchWords != NULL)
			pfree((bmScanPos[keyNo]).bm_batchWords);
	}

	pfree(bmScanPos);
}

/*
 * _bitmap_log_lovitem() -- log adding a new lov item to a lov page.
 */
// void
// _bitmap_log_lovitem(Relation rel, ForkNumber fork, Buffer lovBuffer, OffsetNumber offset,
//                     BMLOVItem lovItem, Buffer metabuf, bool is_new_lov_blkno)
// {
//     Page lovPage = BufferGetPage(lovBuffer);

//     xl_bm_lovitem	xlLovItem;
//     XLogRecPtr		recptr;

//     Assert(BufferGetBlockNumber(lovBuffer) > 0);

//     xlLovItem.bm_node = rel->rd_node;
//     xlLovItem.bm_fork = fork;
//     xlLovItem.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
//     xlLovItem.bm_lov_offset = offset;
//     memcpy(&(xlLovItem.bm_lovItem), lovItem, sizeof(BMLOVItemData));
//     xlLovItem.bm_is_new_lov_blkno = is_new_lov_blkno;

//     XLogBeginInsert();
//     XLogRegisterData((char*)&xlLovItem, sizeof(xl_bm_lovitem));
//     XLogRegisterBuffer(0, lovBuffer, REGBUF_STANDARD);

//     if (is_new_lov_blkno)
//         XLogRegisterBuffer(1, metabuf, 0);

//     recptr = XLogInsert(RM_BITMAP_ID,
//                         XLOG_BITMAP_INSERT_LOVITEM);

//     if (is_new_lov_blkno)
//     {
//         Page metapage = BufferGetPage(metabuf);

//         PageSetLSN(metapage, recptr);
//     }

//     PageSetLSN(lovPage, recptr);

//     elog(DEBUG1, "Insert a new lovItem at (blockno, offset): (%d,%d)",
//          BufferGetBlockNumber(lovBuffer), offset);
// }

/*
 * _bitmap_log_bitmap_lastwords() -- log the last two words in a bitmap.
 */
// void
// _bitmap_log_bitmap_lastwords(Relation rel, Buffer lovBuffer, 
// 							 OffsetNumber lovOffset, BMLOVItem lovItem)
// {
// 	xl_bm_bitmap_lastwords	xlLastwords;
// 	XLogRecPtr				recptr;

// 	xlLastwords.bm_node = rel->rd_node;
// 	xlLastwords.bm_last_compword = lovItem->bm_last_compword;
// 	xlLastwords.bm_last_word = lovItem->bm_last_word;
// 	xlLastwords.lov_words_header = lovItem->lov_words_header;
// 	xlLastwords.bm_last_setbit = lovItem->bm_last_setbit;
// 	xlLastwords.bm_last_tid_location = lovItem->bm_last_tid_location;
// 	xlLastwords.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
// 	xlLastwords.bm_lov_offset = lovOffset;

// 	XLogBeginInsert();
// 	XLogRegisterData((char*)&xlLastwords, sizeof(xl_bm_bitmap_lastwords));
// 	XLogRegisterBuffer(0, lovBuffer, REGBUF_STANDARD);

// 	recptr = XLogInsert(RM_BITMAP_ID, XLOG_BITMAP_INSERT_BITMAP_LASTWORDS);

// 	PageSetLSN(BufferGetPage(lovBuffer), recptr);

// 	/*
// 	 * WAL consistency checking
// 	 */
// #ifdef DUMP_BITMAPAM_INSERT_RECORDS
// 	_dump_page("insert", XactLastRecEnd, &rel->rd_node, lovBuffer);
// #endif
// }

/*
 * _bitmap_log_bitmapwords() -- log new bitmap words to be inserted.
 */
// void
// _bitmap_log_bitmapwords(Relation rel,
// 						BMTIDBuffer *buf,
// 						bool init_first_page, List *xl_bm_bitmapword_pages, List *bitmapBuffers,
// 						Buffer lovBuffer, OffsetNumber lovOffset, uint64 tidnum)
// {
// 	XLogRecPtr	recptr;
// 	int			rdata_no = 0;
// 	Page		lovPage = BufferGetPage(lovBuffer);
// 	xl_bm_bitmapwords xlBitmapWords;
// 	ListCell   *lcp;
// 	ListCell   *lcb;
// 	bool		init_page;
// 	int			num_bm_pages = list_length(xl_bm_bitmapword_pages);
// 	int 		current_page = 0;

// 	Assert(list_length(bitmapBuffers) == num_bm_pages);
// 	if (num_bm_pages > MAX_BITMAP_PAGES_PER_INSERT)
// 		elog(ERROR, "too many bitmap pages in one insert batch");

// 	MemSet(&xlBitmapWords, 0, sizeof(xlBitmapWords));

// 	xlBitmapWords.bm_node = rel->rd_node;
// 	xlBitmapWords.bm_num_pages = num_bm_pages;
// 	xlBitmapWords.bm_init_first_page = init_first_page;

// 	xlBitmapWords.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
// 	xlBitmapWords.bm_lov_offset = lovOffset;
// 	xlBitmapWords.bm_last_compword = buf->last_compword;
// 	xlBitmapWords.bm_last_word = buf->last_word;
// 	xlBitmapWords.lov_words_header =
// 		(buf->is_last_compword_fill) ? 2 : 0;
// 	xlBitmapWords.bm_last_setbit = tidnum;

// 	XLogBeginInsert();
// 	XLogRegisterData((char *) &xlBitmapWords, sizeof(xl_bm_bitmapwords));
// 	XLogRegisterBuffer(0, lovBuffer, REGBUF_STANDARD);

// 	rdata_no = 1;

// 	/* Write per-page structs */
// 	init_page = init_first_page;
// 	forboth(lcp, xl_bm_bitmapword_pages, lcb, bitmapBuffers)
// 	{
// 		xl_bm_bitmapwords_perpage *xlBitmapwordsPage = lfirst(lcp);
// 		Buffer		bitmapBuffer = lfirst_int(lcb);
// 		Page		bitmapPage = BufferGetPage(bitmapBuffer);
// 		BMBitmap	bitmap;

// 		bitmap = (BMBitmap) PageGetContentsMaxAligned(bitmapPage);

// 		Assert(BufferIsValid(bitmapBuffer));

// 		/* fill bm_next_blkno field */
// 		if (current_page + 1 < num_bm_pages)
// 		{
// 			xl_bm_bitmapwords_perpage *next_xl_bm_bitmapwords_perpage = lfirst(lnext(xl_bm_bitmapword_pages, lcp));
// 			xlBitmapwordsPage->bm_next_blkno = next_xl_bm_bitmapwords_perpage->bmp_blkno;
// 		}

// 		XLogRegisterBuffer(rdata_no, bitmapBuffer, 0);

// 		XLogRegisterBufData(rdata_no, (char *) xlBitmapwordsPage, sizeof(xl_bm_bitmapwords_perpage));
// 		XLogRegisterBufData(rdata_no, (char *) &bitmap->hwords[xlBitmapwordsPage->bmp_start_hword_no],
// 							xlBitmapwordsPage->bmp_num_hwords * sizeof(BM_HRL_WORD));
// 		XLogRegisterBufData(rdata_no, (char *) &bitmap->cwords[xlBitmapwordsPage->bmp_start_cword_no],
// 							xlBitmapwordsPage->bmp_num_cwords * sizeof(BM_HRL_WORD));
// 		rdata_no++;
// 		current_page++;
// 	}

// 	recptr = XLogInsert(RM_BITMAP_ID, XLOG_BITMAP_INSERT_WORDS);

// 	foreach(lcb, bitmapBuffers)
// 	{
// 		Buffer		bitmapBuffer = lfirst_int(lcb);

// 		PageSetLSN(BufferGetPage(bitmapBuffer), recptr);
// 	}
// 	PageSetLSN(lovPage, recptr);

// 	/*
// 	 * WAL consistency checking
// 	 */
// #ifdef DUMP_BITMAPAM_INSERT_RECORDS
// 	_dump_page("insert", XactLastRecEnd, &rel->rd_node, lovBuffer);
// 	foreach(lcb, bitmapBuffers)
// 	{
// 		_dump_page("insert", XactLastRecEnd, &rel->rd_node, (Buffer) lfirst_int(lcb));
// 	}
// #endif
// }
