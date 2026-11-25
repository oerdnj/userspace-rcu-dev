// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_REC_DBG_H
#define _FT_TXN_REC_DBG_H

#include "ft-dt-arm.h"		/* FT_DT_ARMED, shared with the allocator's TU */

/*
 * ft-txn-rec-dbg: the debug instruments that watch a txn's records.  FT
 * calls them itself, from ft_txn_record() and ft_txn_commit()
 * (ft-txn-engine.h), so they do not depend on the engine:
 *   - the lock-discipline detectors, armed by --enable-rcu-debug
 *     (FT_DT_ARMED);
 *   - -DFT_DEBUG_HIDDEN_FILL, which names the producer of each record;
 *   - -DFT_DEBUG_SLOT_HIST, a per-slot history of the commits' stores.
 * Each is defined in ft-mutation-helpers.h.
 */

/* One record, as the instruments see it. */
struct ft_rec_dbg {
	void **slot;
	void *old_ptr;		/* the value the record expects */
	void *new_ptr;		/* the value the commit stores */
	uintptr_t tag;		/* the slot's proxy tag */
	const void *txn;	/* the recording txn, for the reports */
};

#if FT_DT_ARMED || defined(FT_DEBUG_HIDDEN_FILL) || defined(FT_DEBUG_SLOT_HIST)
# define FT_REC_DBG	1
#else
# define FT_REC_DBG	0
#endif

#if FT_DT_ARMED
/*
 * ☠ A RELEASE-SHAPED RECORD ON A WORD ANOTHER THREAD TOOK, or a clean SW
 * record on a word a peer holds locked.  Checked where the record is
 * CREATED or merged.  That runs on the producer's own stack, so the report
 * names the recording site.  The commit can only name the committer.
 */
static void ft_dt_rec_check(const struct ft_rec_dbg *r);
/*
 * THE STALE-PLAN AUDIT.  An SW record is CREATED by an op that already holds
 * the lock covering its word.  So the word's committed value must equal the
 * record's expected-old.  An SW park and settle store BLIND, and will not
 * find out otherwise.  CREATION only: a merged record's old_ptr is this
 * txn's own earlier value.
 */
static void ft_sw_stale_check(const struct ft_rec_dbg *r);
/*
 * The same audit at the commit, just before the store: a word can still
 * change after the record was created.
 */
static void ft_sw_commit_check(const struct ft_rec_dbg *r);
/*
 * The detector's store half: the commit is about to store @r->new_ptr.  The
 * owner it reads is still the one that store displaces.
 */
static void ft_dt_will_write(const struct ft_rec_dbg *r);
#endif

#ifdef FT_DEBUG_HIDDEN_FILL
/*
 * WHO CREATED THIS RECORD?  The hidden-fill scan runs at COMMIT.  So its
 * backtrace names the committer, never the producer of the one record that
 * fills a hidden body.  Note the creating pc as the record is created, keyed
 * by the txn and the slot, and let the scan look it up.
 */
static void ft_hf_pc_note(const struct ft_rec_dbg *r);
#endif

#ifdef FT_DEBUG_SLOT_HIST
/*
 * -DFT_DEBUG_SLOT_HIST: a per-SLOT history of every commit store.  A writer
 * that stalls replays the history.  So a TORN multi-word edit names the
 * commit that wrote (or failed to write) each of its words.  Tag the
 * producer, not the consumer.  A store over a value its record did not
 * expect is reported as a clobber.
 */
static void ft_sh_note(const struct ft_rec_dbg *r, void *v);
static void ft_sh_clobber(const struct ft_rec_dbg *r, void *prev);
#endif

/* @r was created, or merged onto the txn's record for its slot. */
static inline
void ft_rec_dbg_recorded(const struct ft_rec_dbg *r, bool merged)
{
#ifdef FT_DEBUG_HIDDEN_FILL
	if (!merged)
		ft_hf_pc_note(r);
#endif
#if FT_DT_ARMED
	ft_dt_rec_check(r);
	if (!merged)
		ft_sw_stale_check(r);
#endif
	(void) r;
	(void) merged;
}

/* The commit is about to store @r->new_ptr into @r->slot. */
static inline
void ft_rec_dbg_will_commit(const struct ft_rec_dbg *r)
{
#ifdef FT_DEBUG_SLOT_HIST
	void *cur = rcu_dereference(*r->slot);

	if (cur != r->old_ptr)
		ft_sh_clobber(r, cur);
	ft_sh_note(r, r->new_ptr);
#endif
#if FT_DT_ARMED
	ft_sw_commit_check(r);
	ft_dt_will_write(r);
#endif
	(void) r;
}

#endif /* _FT_TXN_REC_DBG_H */
