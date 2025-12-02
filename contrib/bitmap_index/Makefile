MODULE_big = bitmap_index

OBJS = \
    src/bitmap.o \


EXTENSION = bitmap_index
DATA = bitmap_index--1.0.sql

PGFILEDESC = "bitmap index - Bitmap index access method"

ifdef USE_PGXS
PG_CONFIG = pg_config
PGXS := $(shell $(PG_CONFIG) --pgxs)
include $(PGXS)
else
subdir = contrib/bitmap_index
top_builddir = ../..
include $(top_builddir)/src/Makefile.global
include $(top_srcdir)/contrib/contrib-global.mk
endif