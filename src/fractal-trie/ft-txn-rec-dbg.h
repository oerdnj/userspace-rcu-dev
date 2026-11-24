// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_REC_DBG_H
#define _FT_TXN_REC_DBG_H

#include "ft-dt-arm.h"		/* FT_DT_ARMED, shared with the allocator's TU */

/*
 * ft-txn-rec-dbg: the debug instruments that the txn engine calls back.  The
 * engine calls them as it creates a record, and around each store it makes:
 *   - the lock-discipline detectors, armed by --enable-rcu-debug
 *     (FT_DT_ARMED);
 *   - -DFT_DEBUG_HIDDEN_FILL, which names the producer of each record;
 *   - -DFT_DEBUG_SLOT_HIST, a per-slot history of the engine's stores.
 * Each is defined in ft-mutation-helpers.h, where the record type is
 * complete.
 */

#if defined(FT_DEBUG_HIDDEN_FILL) && !defined(URCU_TXN_REC_DBG_STAMP)
/*
 * ITEM 4: WHO CREATED THIS RECORD?  The hidden-fill scan runs at COMMIT.  So
 * its backtrace names the committer, never the producer of the one record that
 * fills a hidden body.  The engine stamps every record as it is created, which
 * is the right moment.  Park the creating pc in a small table keyed by the
 * record's own address, and let the scan look it up.  Records are reused
 * across transactions.  But a stamp is overwritten on every reuse and read
 * within the SAME commit.  So the answer belongs to this record.
 */
struct urcu_txn_record;
static void ft_hf_pc_note(const struct urcu_txn_record *r, const void *pc);
/*
 * ☠ THE STAMP IS INSIDE THE ENGINE.  So return_address(0) is always
 * urcu_txn_store_mw, and every producer looks the same.  Measured: 673468
 * fills, ONE "site".  return_address(1) is not the answer either.  The add is
 * inlined into the store, so level 1 walks off the end and SEGVs.  It does so
 * with and without -fno-omit-frame-pointer (measured twice).  The note itself
 * unwinds instead, and only for the records that can BE a fill.
 */
# define URCU_TXN_REC_DBG_STAMP(r)					\
	ft_hf_pc_note((r), NULL)
#endif

/*
 * ☠ THE ENGINE OFFERS ONE STORE HOOK AND TWO INSTRUMENTS WANT IT.
 *
 * Both of the blocks below used to claim URCU_TXN_REC_WROTE behind a
 * `!defined(URCU_TXN_REC_WROTE)` guard.  Whichever was parsed first won, and
 * the other went SILENTLY blind.  That was harmless only while both were
 * opt-in and nobody passed them together.  FT_DT_ARMED now follows
 * --enable-rcu-debug.  So that collision would be the DEFAULT on every debug
 * build carrying -DFT_DEBUG_SLOT_HIST.  A slot history that quietly records
 * nothing is a wrong zero waiting to be believed.  They CHAIN instead.
 */
#if FT_DT_ARMED
/*
 * The engine writes state words too.  Its settles are how a recorded release
 * actually clears FT_STATE_LOCK.  A detector keyed on the BIT has to see
 * them.  Defined in ft-mutation-helpers.h, where the record type is complete.
 */
struct urcu_txn_record;
static void ft_dt_wrote(const struct urcu_txn_record *r, void *v);
#endif

#ifdef FT_DEBUG_SLOT_HIST
/*
 * -DFT_DEBUG_SLOT_HIST: a per-SLOT history of every engine store.  The stores
 * are plant, park, settle, abort restore and lone edge.  A writer that stalls
 * replays the history.  So a TORN multi-word edit names the commit that wrote
 * (or failed to write) each of its words.  Tag the producer, not the
 * consumer.  Defined in ft-mutation-helpers.h, where the record type is
 * complete.
 */
struct urcu_txn_record;
static void ft_sh_note(const struct urcu_txn_record *r, void *v);
static void ft_sh_clobber(const struct urcu_txn_record *r, void *prev);
# define URCU_TXN_PARK_CLOBBER_NOTE(r, prev)	ft_sh_clobber((r), (prev))
#endif

#if FT_DT_ARMED
/*
 * ☠ A RELEASE-SHAPED RECORD ON A WORD ANOTHER THREAD TOOK.  It is checked
 * where the record is CREATED (or chained onto).  That runs on the producer's
 * own stack, so the report names the recording site.  The commit-time settle
 * can only name the committer.  Defined in ft-mutation-helpers.h.
 */
struct urcu_txn_record;
static void ft_dt_rec_check(const struct urcu_txn_record *r);
/*
 * THE STALE-PLAN AUDIT.  An SW record is CREATED by an op that already holds
 * the lock covering its word.  So the word's committed value must equal the
 * record's expected-old.  An SW park and settle store BLIND, and will not
 * find out otherwise.  CREATION only: a chained record's old_ptr is this
 * txn's own earlier value.  Defined in ft-mutation-helpers.h.
 */
static void ft_sw_stale_check(const struct urcu_txn_record *r);
# if defined(URCU_TXN_REC_DBG_STAMP) || defined(URCU_TXN_REC_DBG_CHAIN)
#  warning "FT_DT_ARMED: record hooks already claimed; the record-time release check is BLIND in this build"
# else
#  define URCU_TXN_REC_DBG_STAMP(r)					\
	do {								\
		ft_dt_rec_check(r);					\
		ft_sw_stale_check(r);					\
	} while (0)
#  define URCU_TXN_REC_DBG_CHAIN(r)	ft_dt_rec_check(r)
# endif
/*
 * The detector's engine half runs BEFORE a plain store, and after a CAS store
 * only.  Before a plain store, the owner it reads is still the one the store
 * displaces.  See URCU_TXN_REC_WILL_WRITE.
 */
static void ft_dt_will_write(const struct urcu_txn_record *r, void *v);
# define URCU_TXN_REC_WILL_WRITE(r, v)	ft_dt_will_write((r), (void *) (v))
#endif

#ifndef URCU_TXN_REC_WROTE
# if FT_DT_ARMED && defined(FT_DEBUG_SLOT_HIST)
#  define URCU_TXN_REC_WROTE(r, v)					\
	do {								\
		ft_dt_wrote((r), (void *) (v));				\
		ft_sh_note((r), (void *) (v));				\
	} while (0)
# elif FT_DT_ARMED
#  define URCU_TXN_REC_WROTE(r, v)	ft_dt_wrote((r), (void *) (v))
# elif defined(FT_DEBUG_SLOT_HIST)
#  define URCU_TXN_REC_WROTE(r, v)	ft_sh_note((r), (void *) (v))
# endif
#endif

#endif /* _FT_TXN_REC_DBG_H */
