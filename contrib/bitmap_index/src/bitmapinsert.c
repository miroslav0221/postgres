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
 * The following structure along with BMTIDBuffer are used to buffer
 * words for tids * during index create -- bmbuild().
 */

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
