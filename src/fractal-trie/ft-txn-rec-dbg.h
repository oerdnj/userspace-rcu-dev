// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_REC_DBG_H
#define _FT_TXN_REC_DBG_H

#include "ft-dt-arm.h"		/* FT_DT_ARMED, shared with the allocator's TU */

/*
 * ft-txn-rec-dbg: WHICH RECORD LOST, when a commit aborts.
 *
 * ft-txn-kind-stats.h counts what each txn RECORDS and what its commit
 * RETURNS, per creation site.  Neither answers the question that lies between
 * the two.  A content txn carries three things in ONE descriptor: a structural
 * edge, an ordered-cell edge and a §4.B guard.  So its ABORT tells you nothing
 * about which of them a peer beat.  The engine knows (`t->recs[planted]` in
 * urcu_txn_desc_commit), but did not report it.  It does now, through the
 * default-inert URCU_TXN_STAT_ABORT hook.
 *
 * ☠ THE PROXY TAG CANNOT SUBSTITUTE FOR THIS.  The near-miss is worth
 * recording.  FT_STATE_PROXY, FT_NR_KEYS_PROXY_TAG and URCU_TXN_TAG are ALL 1.
 * The tag separates a structural edge from everything else and nothing more.
 *
 * WHAT IT IS FOR -- two questions, one instrument:
 *
 *   1. G4's input.  What fraction of aborts is the ORDERED-CELL lane, against
 *      which a per-cell lock would be argued?  (The record volume said 9%;
 *      volume is not the abort rate.)
 *   2. ★ A DETECTOR.  A structural MW record whose owner the op HOLDS should
 *      never lose its CAS.  The DLM lock over the slot's owner is exactly
 *      what excludes every peer writer of that word.  If one loses, the abort
 *      is not contention to be converted away.  It is a lock that did not
 *      exclude somebody.  The conversion would turn that word into an
 *      unarbitrated SW park.  FT_AB_OWN_HELD on a structural class is that
 *      event.  It is counted rather than asserted first
 *      ([[feedback_claim_without_the_arm]]).  -DFT_ABORT_CLAIM turns the
 *      count into an abort with the record dumped.
 *
 * BUILD.  Needs BOTH defines:
 * - -DFT_DEBUG_TXN_KIND: the class the FT stamps;
 * - -DURCU_TXN_REC_DBG: the field on the engine record that carries it.
 * The second is a compilation-wide define on purpose.  The descriptor is
 * allocated by urcu-txn.c's slab and filled here.  So a field only one TU
 * can see is a layout split.  With only one of the two, the dump says so
 * rather than printing zeros
 * ([[feedback_verify_the_mechanism_ran_before_believing_a_zero]]).
 */

#if defined(FT_WINNER_DBG) && \
	!(defined(FT_DEBUG_TXN_KIND) && defined(URCU_TXN_REC_DBG))
#error "-DFT_WINNER_DBG needs both -DFT_DEBUG_TXN_KIND and -DURCU_TXN_REC_DBG"
#endif

#if defined(FT_DEBUG_TXN_KIND) && defined(URCU_TXN_REC_DBG)

#define FT_ABORT_ATTRIB	1

/*
 * The label packed into urcu_txn_record.dbg_embedder.  It holds one CLASS.
 * For the classes that a lock could cover, it also holds WHO witnessed the
 * hold.  "A structural record lost" and "a structural record the op OWNS
 * lost" are different findings.
 */
enum ft_ab_cls {
	FT_AB_UNSET = 0,	/* added by a path that stamps nothing */
	FT_AB_SW,		/* an SW park: it cannot lose, so seeing one is itself a finding */
	FT_AB_MW_STRUCT,	/* ★ a structural edge, MW because unarmed: THE DETECTOR'S SUBJECT */
	FT_AB_MW_LOCK,		/* the DLM lock take -- losing IS its job */
	FT_AB_VALIDATE,		/* a read-set guard */
	FT_AB_CELL_HANDLE,	/* an ordered-cell / hlist store recorded straight on the handle */
	FT_AB_CLS_NR,
};

/* Which witness saw the hold, for the classes where a lock could cover it. */
enum ft_ab_own {
	FT_AB_OWN_NA = 0,	/* no lock could cover this word */
	FT_AB_OWN_HELD,		/* ★ the txn registry names the owner */
	FT_AB_OWN_LEDGER,	/* only this thread's hold ledger does */
	FT_AB_OWN_MISS,		/* neither */
	FT_AB_OWN_NR,
};

#define FT_AB_CLS_SHIFT		0
#define FT_AB_CLS_MASK		0xffu
#define FT_AB_OWN_SHIFT		8
#define FT_AB_OWN_MASK		0x300u
/*
 * Two DIFFERENT labels folded onto one record by read-your-own-writes chaining.
 * The record is then one word written by two FT concerns.  No single class is
 * true of it.  So it is counted apart instead of credited to either.
 */
#define FT_AB_MIXED		0x400u

#define ft_ab_code(cls, own)	\
	((unsigned int) (cls) | ((unsigned int) (own) << FT_AB_OWN_SHIFT))
#define ft_ab_code_cls(c)	((c) & FT_AB_CLS_MASK)
#define ft_ab_code_own(c)	(((c) & FT_AB_OWN_MASK) >> FT_AB_OWN_SHIFT)

/*
 * The label the NEXT engine record add will carry.  It is a thread-local
 * hand-off rather than an argument threaded through urcu_txn_store_*.  The
 * store API is the engine's and is shared with every other embedder.  This
 * label must not appear in it.  Set it immediately before the store.  The
 * stamp hook inside the store consumes it.
 */
static __thread unsigned int ft_ab_pending = ft_ab_code(FT_AB_UNSET, FT_AB_OWN_NA);
/* The label of the record that lost the last aborted commit. */
static __thread unsigned int ft_ab_lost;
static __thread int ft_ab_lost_valid;
/*
 * ...and its SLOT TAG, which the class alone does not give.  A structural MW
 * record writes one of two very different words:
 * - a child POINTER slot (FT_FLIP_PROXY_TAG).  Only a holder of the owning
 *   node's lock may write it.
 * - the node's packed STATE word (FT_STATE_PROXY).  By design it also has
 *   writers that hold nothing: the re-parent sweep's guard and the §4.B
 *   validates.
 * Losing a CAS means opposite things on the two.  So the alarm must not pool
 * them.
 */
static __thread uintptr_t ft_ab_lost_tag;
#ifdef FT_ABORT_CLAIM
static __thread int ft_ab_claim_armed;
#endif
#if defined(FT_ABORT_CLAIM) || defined(FT_WINNER_DBG)
/*
 * SLOT -> the OWNER the record NAMED.  The engine record has no room for a
 * pointer, and this is not worth growing it for.  A ring walked BACKWARDS
 * finds the most recent naming of a slot.  That is the one the losing record
 * made.  Sized far above the largest observed graft descriptor.  It has two
 * consumers:
 * - the claim's dump (the loser's own naming);
 * - the winner ledger.  The writer stamps ITS naming as it wins the word, on
 *   its own thread, from its own ring.
 */
#define FT_AB_RING_NR	256
static __thread struct {
	void **slot;
	const void *owner;
	const void *ra;		/* the record's caller, for addr2line */
} ft_ab_ring[FT_AB_RING_NR];
static __thread unsigned int ft_ab_ring_n;

#define FT_AB_NOTE_OWNER(slot_, owner_)					\
	do {								\
		unsigned int k_ = ft_ab_ring_n++ % FT_AB_RING_NR;	\
									\
		ft_ab_ring[k_].slot = (void **) (slot_);		\
		ft_ab_ring[k_].owner = (const void *) (owner_);		\
		ft_ab_ring[k_].ra = __builtin_return_address(0);		\
	} while (0)

static inline
const void *ft_ab_owner_of(void **slot)
{
	unsigned int i, n = ft_ab_ring_n < FT_AB_RING_NR ?
		ft_ab_ring_n : FT_AB_RING_NR;

	for (i = 0; i < n; i++) {
		unsigned int k = (ft_ab_ring_n - 1 - i) % FT_AB_RING_NR;

		if (ft_ab_ring[k].slot == slot)
			return ft_ab_ring[k].owner;
	}
	return NULL;
}

static inline
const void *ft_ab_ra_of(void **slot)
{
	unsigned int i, n = ft_ab_ring_n < FT_AB_RING_NR ?
		ft_ab_ring_n : FT_AB_RING_NR;

	for (i = 0; i < n; i++) {
		unsigned int k = (ft_ab_ring_n - 1 - i) % FT_AB_RING_NR;

		if (ft_ab_ring[k].slot == slot)
			return ft_ab_ring[k].ra;
	}
	return NULL;
}
#else
#define FT_AB_NOTE_OWNER(slot_, owner_)	do { } while (0)
#endif

#define FT_AB_ARM(cls, own)	\
	do { ft_ab_pending = ft_ab_code((cls), (own)); } while (0)

/*
 * ☠ The chain hook fires on a store that folds onto an EXISTING record.  So the
 * disagreement it must resolve is between two labels on one word.  Resolve it
 * PESSIMISTICALLY in both fields:
 * - a differing class becomes MIXED (credited to neither);
 * - the ownership degrades to the weakest witness of the two.
 * So the detector fires only where EVERY store to that word claimed the hold.
 */
#define URCU_TXN_REC_DBG_STAMP(r)	\
	do { (r)->dbg_embedder = ft_ab_pending; } while (0)
#define URCU_TXN_REC_DBG_CHAIN(r)					\
	do {								\
		unsigned int had_ = (r)->dbg_embedder;			\
		unsigned int got_ = ft_ab_pending;			\
									\
		if (ft_ab_code_cls(had_) != ft_ab_code_cls(got_))	\
			had_ |= FT_AB_MIXED;				\
		if (ft_ab_code_own(got_) > ft_ab_code_own(had_))	\
			had_ = (had_ & ~(unsigned int) FT_AB_OWN_MASK) | \
				(got_ & FT_AB_OWN_MASK);		\
		(r)->dbg_embedder = had_;				\
	} while (0)
/*
 * @r is NULL where the abort has no losing record (a poisoned descriptor).
 * Such an abort is recorded, not dropped.  An unattributed abort that vanishes
 * would make the attributed ones look complete.
 *
 * ☠ A FUNCTION, not a ternary in the macro.  The hook is expanded with a
 * literal NULL at the poisoned exit.  `NULL ? NULL->dbg_embedder : x` is a type
 * error, even in the arm that cannot run.  The function is declared here
 * against an INCOMPLETE type: by design, this header is parsed before the
 * engine's.  It is defined in ft-txn-kind-stats.h, where the record is complete
 * and the counters exist.
 */
struct urcu_txn_record;
struct urcu_txn_desc;
static void ft_ab_note_lost(const struct urcu_txn_desc *t,
		const struct urcu_txn_record *r);

#define URCU_TXN_STAT_ABORT(t, r)	ft_ab_note_lost((t), (r))

#ifdef FT_DEBUG_SAME_SLOT
/*
 * THE SAME-SLOT CENSUS (-DFT_DEBUG_SAME_SLOT, on top of the two defines above).
 * It lists every record FT adds onto a slot its descriptor already holds.  The
 * MW front end fuses the pair: a read-your-own-writes chain at age 1+, an
 * escalation at age 0.  So the DEBUG_RCU duplicate scan at commit never sees
 * it.  The SW engine (<urcu/rcu-txn-sw.h>) would append it blindly and settle
 * both, last-wins.  This is the list the engine swap must empty or route
 * through urcu_txn_sw_load / urcu_txn_sw_record_chain.  Declared against the
 * incomplete record, defined in ft-txn-kind-stats.h.
 */
static void ft_ss_note(const struct urcu_txn_record *r, void *old_ptr,
		void *new_ptr, int upgrade, unsigned int kind);

#define URCU_TXN_REC_DBG_SAME_SLOT(r, o, n, up, k)	\
	ft_ss_note((r), (o), (n), (up), (k))
/*
 * A DECLARED FOLD: a producer whose write site is COMPUTED.  A second record
 * on one slot is then its design, not an accident.  The producers are:
 * - the rank count walks: several walks add deltas to a shared ancestor's
 *   nr_keys;
 * - the glue's deferred re-home: a back edge is re-aimed after an earlier
 *   step of the same txn aimed it.
 * Those map to the SW engine's fusing pair (urcu_txn_sw_load +
 * urcu_txn_sw_record_chain).  Every other same-slot hit must be made
 * distinct.  The census counts the two apart.
 */
static __thread int ft_ss_fold_depth;
# define FT_SS_FOLD_BEGIN()	do { ft_ss_fold_depth++; } while (0)
# define FT_SS_FOLD_END()	do { ft_ss_fold_depth--; } while (0)
#endif

#ifdef FT_WINNER_DBG
/*
 * THE WINNER LEDGER (-DFT_WINNER_DBG, on top of the two defines above).  The
 * alarm's remaining question is about the WINNER, not the loser.  The op holds
 * P's DLM lock and writes a child slot inside P.  A peer still wins that word.
 * So WHO writes a child slot of P without holding P?  Every engine write of a
 * slot lands here via URCU_TXN_REC_WROTE, with the record's label.  That
 * covers the proxy plant, the park, the settle and the lone-edge fast paths.
 * Each write is filed in a global slot-keyed ledger.  The entry also holds
 * the writer's tid and the owner this writer NAMED.  That owner comes from
 * the writer's own ring, on its own thread.
 * The loser consults the ledger at the alarm.
 *
 * Declared against the incomplete engine type for the same reason as
 * ft_ab_note_lost above.  Defined in ft-txn-kind-stats.h, where the counters
 * and the ledger live.
 */
static void ft_win_note(void **slot, void *val, unsigned int code);
static void ft_win_lost(const struct urcu_txn_record *rec, void *seen);

#define URCU_TXN_REC_WROTE(r, v)	\
	ft_win_note((r)->slot, (v), (r)->dbg_embedder)
/*
 * ★ THE PRIMARY WITNESS.  The engine captures the value the losing CAS
 * OBSERVED, at the loss itself.  That is race-free, unlike any later re-read
 * of the slot.  A proxy here IS the winning record.  A plain value is matched
 * against the ledger for corroboration only.
 */
#define URCU_TXN_REC_LOST(r, seen)	ft_win_lost((r), (seen))
/*
 * A NON-ENGINE writer of a live slot: the lone-edge release-store lane
 * (ft_ord_cell_flip_one and friends).  Its exclusion is the caller's lock,
 * not an expected-old.  It is filed in the same ledger under a synthetic
 * class, so an alarmed loser can name it.  Without these notes a raw winner
 * is UNPROVABLE.  It surfaces as a ledger mismatch, indistinguishable from
 * ledger lag.
 */
#define FT_WIN_NOTE_RAW(slot_, val_, k_)				\
	ft_win_note((void **) (slot_), (void *) (val_),			\
		FT_AB_CLS_NR + (unsigned int) (k_))
#endif	/* FT_WINNER_DBG */

#else	/* the instrument is not built */

#ifdef FT_DEBUG_SAME_SLOT
#error "-DFT_DEBUG_SAME_SLOT needs both -DFT_DEBUG_TXN_KIND and -DURCU_TXN_REC_DBG"
#endif
#define FT_AB_ARM(cls, own)		do { } while (0)
#define FT_AB_NOTE_OWNER(slot_, owner_)	do { } while (0)

#endif	/* FT_DEBUG_TXN_KIND && URCU_TXN_REC_DBG */

#ifndef FT_WIN_NOTE_RAW
#define FT_WIN_NOTE_RAW(slot_, val_, k_)	do { } while (0)
#endif

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

#ifndef FT_SS_FOLD_BEGIN
# define FT_SS_FOLD_BEGIN()	do { } while (0)
# define FT_SS_FOLD_END()	do { } while (0)
#endif

#endif /* _FT_TXN_REC_DBG_H */
