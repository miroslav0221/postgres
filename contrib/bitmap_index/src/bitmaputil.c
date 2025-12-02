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
#include "access/bitmap.h"
#include "access/bitmap_private.h"
#include "access/bitmap_xlog.h"
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
 * _bitmap_log_lovitem() -- log adding a new lov item to a lov page.
 */
void
_bitmap_log_lovitem(Relation rel, ForkNumber fork, Buffer lovBuffer, OffsetNumber offset,
                    BMLOVItem lovItem, Buffer metabuf, bool is_new_lov_blkno)
{
    Page lovPage = BufferGetPage(lovBuffer);

    xl_bm_lovitem	xlLovItem;
    XLogRecPtr		recptr;

    Assert(BufferGetBlockNumber(lovBuffer) > 0);

    xlLovItem.bm_node = rel->rd_node;
    xlLovItem.bm_fork = fork;
    xlLovItem.bm_lov_blkno = BufferGetBlockNumber(lovBuffer);
    xlLovItem.bm_lov_offset = offset;
    memcpy(&(xlLovItem.bm_lovItem), lovItem, sizeof(BMLOVItemData));
    xlLovItem.bm_is_new_lov_blkno = is_new_lov_blkno;

    XLogBeginInsert();
    XLogRegisterData((char*)&xlLovItem, sizeof(xl_bm_lovitem));
    XLogRegisterBuffer(0, lovBuffer, REGBUF_STANDARD);

    if (is_new_lov_blkno)
        XLogRegisterBuffer(1, metabuf, 0);

    recptr = XLogInsert(RM_BITMAP_ID,
                        XLOG_BITMAP_INSERT_LOVITEM);

    if (is_new_lov_blkno)
    {
        Page metapage = BufferGetPage(metabuf);

        PageSetLSN(metapage, recptr);
    }

    PageSetLSN(lovPage, recptr);

    elog(DEBUG1, "Insert a new lovItem at (blockno, offset): (%d,%d)",
         BufferGetBlockNumber(lovBuffer), offset);
}