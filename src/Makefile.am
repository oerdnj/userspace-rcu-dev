# SPDX-FileCopyrightText: 2023 EfficiOS Inc.
#
# SPDX-License-Identifier: MIT

AM_CPPFLAGS += -I$(top_srcdir)/src

#Add the -version-info directly here since we are only building
# library that use the version-info
AM_LDFLAGS=-version-info $(URCU_LIBRARY_VERSION) $(LT_NO_UNDEFINED)

dist_noinst_HEADERS = urcu-die.h urcu-wait.h compat-getcpu.h \
	urcu-utils.h compat-smp.h bitmap.h

COMPAT = compat_futex.c

FRACTAL_TRIE = fractal-trie/fractal-trie.c fractal-trie/fractal-trie-alloc.c \
	fractal-trie/fractal-trie-internal.h fractal-trie/fractal-trie-trace.h \
	fractal-trie/cds_ft_tp.c fractal-trie/cds_ft_tp.h \
	fractal-trie/ft-helpers.h fractal-trie/ft-txn-hlist.h fractal-trie/ft-trace-helpers.h fractal-trie/ft-lookup-node.h \
	fractal-trie/ft-txn-engine.h \
	fractal-trie/ft-txn-kind-stats.h fractal-trie/ft-txn-rec-dbg.h \
	fractal-trie/ft-dt-arm.h \
	fractal-trie/ft-descent.h fractal-trie/ft-lookup.h \
	fractal-trie/ft-inequality.h fractal-trie/ft-insert.h \
	fractal-trie/ft-remove.h fractal-trie/ft-graft.h \
	fractal-trie/ft-detach.h fractal-trie/ft-merge.h \
	fractal-trie/ft-rekey.h \
	fractal-trie/ft-lifecycle.h fractal-trie/ft-verify.h \
	fractal-trie/ft-compact.h fractal-trie/ft-key.h \
	fractal-trie/ft-lookup-helpers.h fractal-trie/ft-iter.h fractal-trie/ft-delay.h fractal-trie/ft-tables.h \
	fractal-trie/ft-mutation-helpers.h fractal-trie/ft-mutation-node.h \
	fractal-trie/ft-cluster-build.h \
	fractal-trie/ft-ordered-query.h fractal-trie/ft-show.h

RCULFHASH = rculfhash.c rculfhash-mm-order.c rculfhash-mm-chunk.c \
		rculfhash-mm-mmap.c

lib_LTLIBRARIES = liburcu-common.la \
		liburcu.la liburcu-qsbr.la \
		liburcu-mb.la liburcu-bp.la \
		liburcu-memb.la liburcu-cds.la

#
# liburcu-common contains wait-free queues (needed by call_rcu) as well
# as futex fallbacks.  urcu-txn.c holds the transaction engines' shared
# state (the single strong definition of the per-CPU descriptor slabs the
# header-inline engines in <urcu/rcu-txn-mcas.h> / <urcu/rcu-txn-sw.h> use).
#
liburcu_common_la_SOURCES = wfqueue.c wfcqueue.c wfstack.c urcu-txn.c $(COMPAT)

liburcu_la_SOURCES = urcu.c urcu-pointer.c $(COMPAT)
liburcu_la_CFLAGS = -DRCU_MEMBARRIER $(AM_CFLAGS)
liburcu_la_LIBADD = liburcu-common.la

liburcu_memb_la_SOURCES = urcu.c urcu-pointer.c $(COMPAT)
liburcu_memb_la_CFLAGS = -DRCU_MEMBARRIER $(AM_CFLAGS)
liburcu_memb_la_LIBADD = liburcu-common.la

liburcu_qsbr_la_SOURCES = urcu-qsbr.c urcu-pointer.c $(COMPAT)
liburcu_qsbr_la_CFLAGS = -DRCU_QSBR $(AM_CFLAGS)
liburcu_qsbr_la_LIBADD = liburcu-common.la

liburcu_mb_la_SOURCES = urcu.c urcu-pointer.c $(COMPAT)
liburcu_mb_la_CFLAGS = -DRCU_MB $(AM_CFLAGS)
liburcu_mb_la_LIBADD = liburcu-common.la

liburcu_bp_la_SOURCES = urcu-bp.c urcu-pointer.c $(COMPAT)
liburcu_bp_la_LIBADD = liburcu-common.la

liburcu_cds_la_SOURCES = rculfqueue.c rculfstack.c lfstack.c \
	workqueue.c workqueue.h $(FRACTAL_TRIE) $(RCULFHASH) $(COMPAT)
liburcu_cds_la_LIBADD = liburcu-common.la $(FT_TRACING_LIBS)
# Inject the FT lookup-API cluster placement script so the linker
# co-locates each public dispatch entry with its specialized
# inners in a page-aligned cluster.  See ft-lookup-layout.ld at
# the top of the tree for details.  Skipped when configure found
# the linker does not accept the script (non-GNU linkers); the
# library is fully functional without it, only the lookup-cluster
# page placement is lost.
if HAVE_FT_LOOKUP_LAYOUT_LD
FT_LOOKUP_LAYOUT_LDFLAGS = -Wl,-T,$(top_srcdir)/ft-lookup-layout.ld
else
FT_LOOKUP_LAYOUT_LDFLAGS =
endif
liburcu_cds_la_LDFLAGS = $(AM_LDFLAGS) $(FT_LOOKUP_LAYOUT_LDFLAGS)

pkgconfigdir = $(libdir)/pkgconfig
pkgconfig_DATA = liburcu-cds.pc liburcu.pc liburcu-bp.pc liburcu-qsbr.pc \
	liburcu-mb.pc liburcu-memb.pc

EXTRA_DIST = \
	urcu-call-rcu-impl.h \
	urcu-defer-impl.h \
	urcu-poll-impl.h \
	rculfhash-internal.h
