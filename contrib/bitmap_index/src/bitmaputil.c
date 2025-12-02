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
