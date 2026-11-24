// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_KIND_STATS_H
#define _FT_TXN_KIND_STATS_H

/*
 * ft-txn-kind-stats: count the records each flip-txn plants, and what its
 * commit returns.  The counts are per TXN CREATION SITE.  Built only under
 * -DFT_DEBUG_TXN_KIND.  Otherwise it is byte-neutral: the struct fields, the
 * increments and this whole unit compile out.
 *
 * Every record is SW.  A record that the op neither owns nor excludes aborts
 * the process (ft_flip_txn_struct_mw_abort), so there is no MW class to count.
 * The table gives, per site:
 *   - the txns created, and those armed SW at creation;
 *   - the SW records;
 *   - the commit outcomes (enum ft_tk_end_class).
 *
 * The duplicate-chain stores record straight onto the engine handle
 * (ft-txn-hlist.h).  They hold no back-pointer to their flip-txn, so they are
 * counted GLOBALLY rather than per site.
 *
 * COUNTERS ARE PER THREAD, summed at the dump.  A shared counter on the hot
 * path would be a contended cacheline, and it would disturb the fairness this
 * unit measures.  Each thread's block is registered on a global list and is
 * never freed, so a thread that exits before the dump still reports.
 */

#ifndef FRACTAL_TRIE_IMPL
#error "ft-txn-kind-stats.h is an implementation unit; #include it from fractal-trie.c only"
#endif

#ifdef FT_DEBUG_TXN_KIND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

enum ft_tk_rec_class {
	FT_TK_SW = 0,
	FT_TK_REC_NR,
};

/* See the door-1/3 exclusion probe at the SW park in __ft_flip_txn_record_tag_ctx. */
extern unsigned long ft_tk_d13_body, ft_tk_d13_excl, ft_tk_d13_coarse,
	ft_tk_d13_wlock, ft_tk_d13_none;
unsigned long ft_tk_d13_body, ft_tk_d13_excl, ft_tk_d13_coarse,
	ft_tk_d13_wlock, ft_tk_d13_none;
extern unsigned long ft_tk_d3_excl, ft_tk_d3_coarse, ft_tk_d3_wlock,
	ft_tk_d3_fine;
unsigned long ft_tk_d3_excl, ft_tk_d3_coarse, ft_tk_d3_wlock, ft_tk_d3_fine;
/* See the lock-held probe in ft_flip_txn_record_tombstone. */
extern unsigned long ft_tk_ret_held_us, ft_tk_ret_held_other,
	ft_tk_ret_unlocked_fine, ft_tk_ret_unlocked_coarse,
	ft_tk_ret_unlocked_excl, ft_tk_ret_unlocked_wlock;
unsigned long ft_tk_ret_held_us, ft_tk_ret_held_other,
	ft_tk_ret_unlocked_fine, ft_tk_ret_unlocked_coarse,
	ft_tk_ret_unlocked_excl, ft_tk_ret_unlocked_wlock;

enum ft_tk_end_class {
	FT_TK_OK = 0,		/* urcu_txn_commit_flavor -> OK */
	FT_TK_ABORT,		/* a peer won an MW record or a guard */
	FT_TK_MEMERR,		/* sticky ENOMEM: nothing was parked */
	FT_TK_MISS,		/* acquire_miss: discarded before the engine */
	FT_TK_BAILED,		/* ft_flip_txn_destroy: no commit attempted */
	FT_TK_END_NR,
};

/*
 * The last id is the OVERFLOW bucket.  A site that finds the table full still
 * counts, into a row named as such, instead of counting nowhere.
 */
#define FT_TK_MAX_SITES		256
#define FT_TK_OVERFLOW_ID	(FT_TK_MAX_SITES - 1)

/*
 * One txn creation site.  It lives as a function-static at the site itself
 * (FT_TK_SITE_HERE).  So its identity is the source line, and the site does
 * not have to be told anything.
 */
struct ft_tk_site {
	const char *file;
	int line;
	const char *what;	/* which constructor */
	int id;			/* -1 until registered */
};

#ifdef FT_WINNER_DBG
/* Raw-note sites (FT_WIN_NOTE_RAW): the non-engine live-slot writers. */
#define FT_WIN_RAW_NR	2
#endif

struct ft_tk_tls {
	struct ft_tk_tls *next;
	unsigned long rec[FT_TK_MAX_SITES][FT_TK_REC_NR];
	unsigned long end[FT_TK_MAX_SITES][FT_TK_END_NR];
	unsigned long created[FT_TK_MAX_SITES];
	unsigned long armed[FT_TK_MAX_SITES];
	/* the duplicate-chain stores, not site-attributed: see the header */
	unsigned long chain_sw;
#ifdef FT_ABORT_ATTRIB
	/*
	 * WHICH CLASS OF RECORD LOST, per txn creation site.  It is per site
	 * for two reasons.  A class that only ever loses at one site is a
	 * different problem from one that loses everywhere.  And every other
	 * row here is keyed by the site.  The OWNERSHIP witness is kept
	 * GLOBALLY beside it.
	 * It splits one class only (the structural surface).  A full
	 * site x class x witness cube would be 4x this, for a column that is
	 * zero almost everywhere.
	 */
	unsigned long ab[FT_TK_MAX_SITES][FT_AB_CLS_NR];
	unsigned long ab_own[FT_AB_CLS_NR][FT_AB_OWN_NR];
	unsigned long ab_mixed;		/* two classes chained onto the losing word */
	unsigned long ab_unattrib;	/* an abort no hook could name (poisoned) */
	unsigned long ab_alarm[FT_TK_MAX_SITES][2];	/* [0] state word, [1] pointer slot */
	/*
	 * ft_slot_in_node(g->publish_parent, g->publish_slot) at the glue
	 * publish.  The question: is the slot this commit writes really one of
	 * the child slots of the named parent?  The record names that parent as
	 * the slot's OWNER.  The record-time owner check asks only whether the
	 * op HOLDS the parent.  It never asks whether the parent OWNS the word.
	 * [0] no, [1] yes.
	 */
	unsigned long pub_pair[2];
#ifdef FT_WINNER_DBG
	/*
	 * WHO WON the word an alarmed record lost.  The PRIMARY witness is the
	 * value the losing CAS OBSERVED (URCU_TXN_REC_LOST).  It is race-free.
	 *   - A proxy there decodes to the winning record exactly
	 *     (win_beater_proxy).
	 *   - A plain value is matched against the winner ledger, for
	 *     CORROBORATION only (win_beater_ledger).  Under ABA, a value-match
	 *     can name an older write of the same value.  So it is a
	 *     population claim, never proof of the one write.
	 * The other counters:
	 *   - win_raw names the non-engine release-store lane by its note
	 *     site.
	 *   - win_owner compares the owner named by the winner with the
	 *     loser's.  The winner is the ledger-corroborated one.  [0] same,
	 *     [1] ☠ DIFFERENT (two metadata each believed to own the word),
	 *     [2] unknown.
	 *   - win_owner_tomb and win_owner_unparented are LOSER-side probes of
	 *     candidate 2.  They count the registered owner found TOMBSTONED,
	 *     or mid re-home (parent_word NULL), at the alarm itself.
	 */
	unsigned long win_beater[FT_AB_CLS_NR][FT_AB_OWN_NR];
	unsigned long win_beater_proxy;
	unsigned long win_beater_ledger;
	unsigned long win_raw[FT_WIN_RAW_NR];
	unsigned long win_ledger_mismatch;	/* plain beater; ledger has the slot, another value */
	unsigned long win_ledger_absent;	/* plain beater; no ledger entry (eviction, or an unhooked writer) */
	unsigned long win_seen_none;	/* the loss exit had no observed value in hand */
	unsigned long win_drift;	/* the slot changed again between the loss and this lookup */
	unsigned long win_owner[3];
	unsigned long win_tid_same;	/* the corroborated winner was THIS thread's own write */
	unsigned long win_owner_tomb;
	unsigned long win_owner_unparented;
	unsigned long win_torn;		/* the ledger entry would not settle under 4 seq retries */
	unsigned long win_note;		/* reach: ledger writes by this thread */
	unsigned long win_note_skip;	/* ledger writes skipped on entry contention */
#endif
#endif
};

static pthread_mutex_t ft_tk_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ft_tk_site *ft_tk_sites[FT_TK_MAX_SITES];
static int ft_tk_nr_sites;
static struct ft_tk_tls *ft_tk_threads;
static __thread struct ft_tk_tls *ft_tk_self;

static
struct ft_tk_site ft_tk_overflow_site = { "<sites overflowed>", 0, "-", FT_TK_OVERFLOW_ID };

/*
 * Assign @s a stable id.  Two threads reaching a cold site both take the lock;
 * the second finds the id already published and uses it.
 */
static
int ft_tk_site_register(struct ft_tk_site *s)
{
	int id;

	pthread_mutex_lock(&ft_tk_lock);
	if (s->id >= 0) {
		id = s->id;
		goto unlock;
	}
	if (ft_tk_nr_sites >= FT_TK_OVERFLOW_ID) {
		ft_tk_sites[FT_TK_OVERFLOW_ID] = &ft_tk_overflow_site;
		id = FT_TK_OVERFLOW_ID;
	} else {
		id = ft_tk_nr_sites++;
		ft_tk_sites[id] = s;
	}
	CMM_STORE_SHARED(s->id, id);
unlock:
	pthread_mutex_unlock(&ft_tk_lock);
	return id;
}

static inline
int ft_tk_site_id(struct ft_tk_site *s)
{
	int id = CMM_LOAD_SHARED(s->id);

	if (caa_likely(id >= 0))
		return id;
	return ft_tk_site_register(s);
}

static
struct ft_tk_tls *ft_tk_tls_create(void)
{
	struct ft_tk_tls *tls = (struct ft_tk_tls *) calloc(1, sizeof(*tls));

	if (!tls)
		abort();	/* a debug build that cannot count is a lie */
	pthread_mutex_lock(&ft_tk_lock);
	tls->next = ft_tk_threads;
	ft_tk_threads = tls;
	pthread_mutex_unlock(&ft_tk_lock);
	ft_tk_self = tls;
	return tls;
}

static inline
struct ft_tk_tls *ft_tk_tls_get(void)
{
	struct ft_tk_tls *tls = ft_tk_self;

	if (caa_likely(tls != NULL))
		return tls;
	return ft_tk_tls_create();
}

/*
 * @site NULL means the txn was built by a path that predates this instrument.
 * There is none today.  Count it in the overflow row rather than nowhere.
 */
static inline
void ft_tk_count_rec(struct ft_tk_site *site, enum ft_tk_rec_class c)
{
	/*
	 * ☞ A CLASS OUT OF RANGE IS NOT A COUNTING ERROR, IT IS A SPRAY.  It would
	 * index past this row, into whatever .bss follows it.  An uninitialised
	 * bool once did that here, and corrupted the neighbouring counters.  The
	 * bound costs a predicted compare in a debug build.  It turns that failure
	 * into an immediate, located abort.
	 */
	if (caa_unlikely((unsigned int) c >= (unsigned int) FT_TK_REC_NR)) {
		fprintf(stderr, "FT_TK BOUNDS: class=%d site=%s:%d ra=%p\n",
			(int) c, site ? site->file : "?",
			site ? site->line : -1,
			__builtin_return_address(0));
		abort();
	}
	ft_tk_tls_get()->rec[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID][c]++;
}

static inline
void ft_tk_count_end(struct ft_tk_site *site, enum ft_tk_end_class c)
{
	ft_tk_tls_get()->end[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID][c]++;
}

static inline
void ft_tk_count_created(struct ft_tk_site *site)
{
	ft_tk_tls_get()->created[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID]++;
}

static inline
void ft_tk_count_armed(struct ft_tk_site *site)
{
	ft_tk_tls_get()->armed[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID]++;
}

static inline
void ft_tk_count_chain_sw(void)
{
	ft_tk_tls_get()->chain_sw++;
}

#ifdef FT_ABORT_ATTRIB
static const char * const ft_ab_names[FT_AB_CLS_NR] = {
	"UNATTRIB", "SW", "MW_STRUCT", "MW_LOCK", "VALIDATE", "CELL_HANDLE",
};

static
const char *ft_ab_cls_name(unsigned int cls)
{
	return cls < FT_AB_CLS_NR ? ft_ab_names[cls] : "?";
}

/*
 * The engine's abort hook, declared in ft-txn-rec-dbg.h and defined here where
 * struct urcu_txn_record is complete.  It only STASHES: the class is counted in
 * ft_flip_txn_commit, which still has the txn's creation site in hand.
 */
static const char *ft_ab_cls_name(unsigned int cls);
static int ft_ab_is_alarm(unsigned int cls, unsigned int own);

#ifdef FT_WINNER_DBG
/*
 * THE WINNER LEDGER.  A global slot-keyed table of the LAST ENGINE WRITE to
 * each slot.  An entry says who wrote and what value.  "Who" is the label, the
 * named owner, the record call site and the tid.  The ledger is filled from
 * URCU_TXN_REC_WROTE on the writer's own thread.  That is what lets it read
 * the writer's ring for the owner it named.  It is read by an aborting peer
 * whose alarmed record just lost that slot.
 *
 * ☠ THE LEDGER IS A CLAIM ABOUT A POPULATION, NOT ABOUT ONE WRITE.  The entry
 * the loser reads is only "the last stamped write".  The alarmed slot's value
 * is known to cycle among three pointers.  Under that ABA, a value-match may
 * name an OLDER write of the same value.  A racing stamp is skipped outright.
 * The ledger is usable anyway, because every stamped writer of the slot IS a
 * writer of that slot.  So the HISTOGRAM over tens of thousands of alarms
 * describes the slot's writer population.  That is the question: "who writes
 * a child slot of P without holding P".  The value-match (win_attr vs
 * win_stale) and the reach counters bound the noise instead of hiding it.
 *
 * Entries are seq-versioned (even = stable, odd = writer active).  A writer
 * TRY-claims with one CAS and SKIPS on contention.  The ledger must never add
 * a wait to the engine's install path.  The skip is counted.
 */
#define FT_WIN_BITS	19
#define FT_WIN_SIZE	(1UL << FT_WIN_BITS)
#define FT_WIN_MASK	(FT_WIN_SIZE - 1)
#define FT_WIN_WAYS	4

struct ft_win_ent {
	unsigned long seq;
	void **slot;
	void *val;
	const void *owner;	/* the owner the WINNER named (its ring), or NULL */
	const void *ra;		/* the winner's record call site, or NULL */
	unsigned long tid;
	unsigned int code;	/* the winner's dbg_embedder label */
};

static struct ft_win_ent ft_win_tbl[FT_WIN_SIZE];

static inline
unsigned long ft_win_hash(void **slot)
{
	uintptr_t a = (uintptr_t) slot >> 3;

	a ^= a >> 33;
	a *= 0xff51afd7ed558ccdULL;
	a ^= a >> 29;
	return (unsigned long) a & FT_WIN_MASK;
}

static inline
unsigned long ft_win_tid(void)
{
	return (unsigned long) pthread_self();
}

/* Declared in ft-txn-rec-dbg.h; the engine's URCU_TXN_REC_WROTE lands here. */
static
void ft_win_note(void **slot, void *val, unsigned int code)
{
	struct ft_tk_tls *tls = ft_tk_tls_get();
	unsigned long h = ft_win_hash(slot);
	struct ft_win_ent *e = NULL;
	unsigned long s;
	unsigned int i, cls = ft_ab_code_cls(code);

	for (i = 0; i < FT_WIN_WAYS; i++) {
		struct ft_win_ent *c = &ft_win_tbl[(h + i) & FT_WIN_MASK];

		if (CMM_LOAD_SHARED(c->slot) == slot) {
			e = c;
			break;
		}
		if (!e && !CMM_LOAD_SHARED(c->slot))
			e = c;
	}
	if (!e)
		e = &ft_win_tbl[h];	/* all ways foreign: evict the primary */
	s = uatomic_load(&e->seq, CMM_RELAXED);
	if ((s & 1) || uatomic_cmpxchg(&e->seq, s, s + 1) != s) {
		tls->win_note_skip++;
		return;
	}
	CMM_STORE_SHARED(e->slot, slot);
	CMM_STORE_SHARED(e->val, val);
	e->code = code;
	/*
	 * The ring notes owners only where FT_AB_NOTE_OWNER runs -- the
	 * lock-coverable classes.  Asking it for any other class would return
	 * a STALE note from an earlier record on this thread that happened to
	 * share the slot.
	 */
	if (cls == FT_AB_MW_STRUCT || cls == FT_AB_MW_LOCK) {
		e->owner = ft_ab_owner_of(slot);
		e->ra = ft_ab_ra_of(slot);
	} else if (cls >= FT_AB_CLS_NR) {
		/* A raw note: the return address names the storing lane
		 * (addr2line -i walks the inline chain to the caller). */
		e->owner = NULL;
		e->ra = __builtin_return_address(0);
	} else {
		e->owner = NULL;
		e->ra = NULL;
	}
	e->tid = ft_win_tid();
	uatomic_store(&e->seq, s + 2, CMM_RELEASE);
	tls->win_note++;
}

enum ft_win_state {
	FT_WIN_NONE = 0,	/* no observed value at the loss exit */
	FT_WIN_ATTR,	/* plain beater, ledger-corroborated (population claim) */
	FT_WIN_PROXY,	/* ★ the observed value IS the winning record (exact) */
	FT_WIN_STALE,	/* ledger names the slot, another value: lag or an unhooked writer */
	FT_WIN_MISS,	/* no ledger entry: eviction, or an unhooked writer */
	FT_WIN_TORN,	/* the entry would not settle */
	FT_WIN_RAW,	/* ☠ the beater was a NON-ENGINE release store, by note site */
};

/* The observed value of the last lost CAS (URCU_TXN_REC_LOST). */
static __thread struct {
	const struct urcu_txn_record *rec;
	void *seen;
	int valid;
} ft_win_seen_tls;

static
void ft_win_lost(const struct urcu_txn_record *rec, void *seen)
{
	ft_win_seen_tls.rec = rec;
	ft_win_seen_tls.seen = seen;
	ft_win_seen_tls.valid = 1;
}

/* The last lookup's answer, for the -DFT_ABORT_CLAIM dump. */
static __thread struct {
	enum ft_win_state state;
	unsigned int code;
	const void *owner;
	const void *ra;
	unsigned long tid;
	void *now;
	void *seen;
	void *val;
	int have;
	int owner_tomb;
	int owner_unparented;
} ft_win_last;

static
void ft_win_lookup(const struct urcu_txn_record *r)
{
	struct ft_tk_tls *tls = ft_tk_tls_get();
	int have = ft_win_seen_tls.valid && ft_win_seen_tls.rec == r;
	void *seen = have ? ft_win_seen_tls.seen : NULL;
	void *now = CMM_LOAD_SHARED(*r->slot);
	unsigned long h;
	struct ft_win_ent snap;
	int found = 0, torn = 0;
	unsigned int i;

	ft_win_seen_tls.valid = 0;
	memset(&ft_win_last, 0, sizeof(ft_win_last));
	ft_win_last.now = now;
	ft_win_last.seen = seen;
	ft_win_last.have = have;
	/*
	 * LOSER-SIDE probes of the re-home candidate.  They need no
	 * cooperation from the winner.  They look at the owner this op
	 * registered (and still holds).  Is it already TOMBSTONED?  Is it mid
	 * re-home?  (The parent_word NULL transient belongs to detach /
	 * graft_swap.)  Either found true at an alarm says the word left the
	 * registered owner's coverage while the lock was held.
	 */
	{
		const struct cds_ft_metadata *own =
			(const struct cds_ft_metadata *)
				ft_ab_owner_of(r->slot);

		if (own) {
			if (CMM_LOAD_SHARED(own->state) & FT_STATE_TOMBSTONE) {
				tls->win_owner_tomb++;
				ft_win_last.owner_tomb = 1;
			}
			if (!CMM_LOAD_SHARED(own->parent_word)) {
				tls->win_owner_unparented++;
				ft_win_last.owner_unparented = 1;
			}
		}
	}
	if (!have || !seen) {
		/* No observed value (or a genuine NULL beater): unattributable. */
		tls->win_seen_none++;
		ft_win_last.state = FT_WIN_NONE;
		return;
	}
	if (now != seen)
		tls->win_drift++;	/* the window a re-read design would misread */
	/*
	 * ★ THE EXACT ARM.  A proxy observed AT the losing CAS is the winning
	 * record itself.  RCU keeps its descriptor alive for this reader.  Its
	 * label needs no ledger.  The winner's ring is another thread's, so
	 * its named owner stays unknown here.
	 */
	if (urcu_txn_is_proxy(seen, r->proxy_tag)) {
		const struct urcu_txn_record *wr =
			(const struct urcu_txn_record *)
				urcu_txn_untag(seen, r->proxy_tag);
		unsigned int wcode = CMM_LOAD_SHARED(wr->dbg_embedder);
		unsigned int wcls = ft_ab_code_cls(wcode);

		if (wcls >= FT_AB_CLS_NR)
			wcls = FT_AB_UNSET;
		tls->win_beater[wcls][ft_ab_code_own(wcode)]++;
		tls->win_beater_proxy++;
		ft_win_last.state = FT_WIN_PROXY;
		ft_win_last.code = wcode;
		return;
	}
	/* A plain beater: the ledger is corroboration, never proof. */
	h = ft_win_hash(r->slot);
	for (i = 0; i < FT_WIN_WAYS && !found && !torn; i++) {
		struct ft_win_ent *e = &ft_win_tbl[(h + i) & FT_WIN_MASK];
		unsigned int tries;

		if (CMM_LOAD_SHARED(e->slot) != r->slot)
			continue;
		torn = 1;
		for (tries = 0; tries < 4; tries++) {
			unsigned long s0 = uatomic_load(&e->seq, CMM_ACQUIRE);

			if (s0 & 1) {
				caa_cpu_relax();
				continue;
			}
			snap = *e;
			cmm_smp_rmb();
			if (uatomic_load(&e->seq, CMM_RELAXED) == s0) {
				found = snap.slot == r->slot;
				torn = 0;
				break;
			}
		}
	}
	if (torn) {
		tls->win_torn++;
		ft_win_last.state = FT_WIN_TORN;
		return;
	}
	if (found && snap.val == seen) {
		/*
		 * ☠ Compare the CLASS FIELD, not the code.  The code carries
		 * the ownership witness at bit 8.  So an engine MW_STRUCT/held
		 * code (0x102) is numerically past FT_AB_CLS_NR.  A full-code
		 * compare misfiled every such winner as raw.  ONE claim sample
		 * caught it: it printed the fields the counter had already
		 * binned.
		 */
		if (ft_ab_code_cls(snap.code) >= FT_AB_CLS_NR) {
			unsigned int k = ft_ab_code_cls(snap.code) - FT_AB_CLS_NR;

			tls->win_raw[k < FT_WIN_RAW_NR ? k : 0]++;
			ft_win_last.state = FT_WIN_RAW;
		} else {
			unsigned int wcls = ft_ab_code_cls(snap.code);
			const void *lown = ft_ab_owner_of(r->slot);

			if (wcls >= FT_AB_CLS_NR)
				wcls = FT_AB_UNSET;
			tls->win_beater[wcls][ft_ab_code_own(snap.code)]++;
			tls->win_beater_ledger++;
			if (!snap.owner || !lown)
				tls->win_owner[2]++;
			else if (snap.owner == lown)
				tls->win_owner[0]++;
			else
				tls->win_owner[1]++;
			if (snap.tid == ft_win_tid())
				tls->win_tid_same++;
			ft_win_last.state = FT_WIN_ATTR;
		}
		ft_win_last.code = snap.code;
		ft_win_last.owner = snap.owner;
		ft_win_last.ra = snap.ra;
		ft_win_last.tid = snap.tid;
		ft_win_last.val = snap.val;
		return;
	}
	if (found) {
		tls->win_ledger_mismatch++;
		ft_win_last.state = FT_WIN_STALE;
		ft_win_last.code = snap.code;
		ft_win_last.owner = snap.owner;
		ft_win_last.ra = snap.ra;
		ft_win_last.tid = snap.tid;
		ft_win_last.val = snap.val;
		return;
	}
	tls->win_ledger_absent++;
	ft_win_last.state = FT_WIN_MISS;
}
#endif	/* FT_WINNER_DBG */

#ifdef FT_DEBUG_LIVE_VALIDATE
#ifndef FT_WINNER_DBG
#error "-DFT_DEBUG_LIVE_VALIDATE needs -DFT_WINNER_DBG (the value the losing CAS observed)"
#endif
/*
 * THE LIVE VALIDATION, MEASURED.  ft_reparent_record_meta records a re-homed
 * child's state word {live -> live} MW whenever the op does not hold that
 * word.  This is a read check.  It aborts the commit if the child was locked,
 * retired or changed meanwhile.  The counts are per re-home CALL SITE:
 *   - how many were recorded;
 *   - whether the child is its own anchor, and whether the op holds the
 *     child's anchor (under -DFT_DEBUG_STRUCT_ANCHOR);
 *   - when one LOSES, what beat it.  This is read from the value the losing
 *     CAS observed (URCU_TXN_REC_LOST).
 */
enum ft_lv_cause {
	FT_LV_SEEN_NONE = 0,	/* no observed value */
	FT_LV_PROXY_LOCKTAKE,	/* a peer's parked lock take (MW_LOCK) */
	FT_LV_PROXY_SW,		/* a holder's SW park */
	FT_LV_PROXY_OTHER,
	FT_LV_LOCK_MEMBER,	/* LOCK set, the child is a MEMBER some op claims */
	FT_LV_LOCK_ANCHOR,	/* LOCK set, nobody claims the child: held as an ANCHOR */
	FT_LV_TOMB,		/* retired */
	FT_LV_NRCHILD,		/* only nr_child moved */
	FT_LV_OTHER,
	FT_LV_NR,
};
static const char * const ft_lv_cause_name[FT_LV_NR] = {
	"seen_none", "proxy_locktake", "proxy_sw",
	"proxy_other", "LOCK_member", "LOCK_anchor", "TOMB", "nr_child",
	"other",
};
#define FT_LV_SITES	64
static struct ft_lv_site {
	const void *ra;
	unsigned long rec, rec_self, rec_notself, rec_held, rec_notheld;
	unsigned long lost;
	unsigned long cause[FT_LV_NR];
} ft_lv_sites[FT_LV_SITES];
static unsigned long ft_lv_site_overflow;
/* The holder's acquire site, for LOCK_member. */
#define FT_LV_FNLINE	32
static struct {
	const char *fn;
	int line;
	unsigned long n;
} ft_lv_fnline[FT_LV_FNLINE];
static pthread_mutex_t ft_lv_lock = PTHREAD_MUTEX_INITIALIZER;
/* slot -> site, this thread's recent validations (the loser is one of them). */
#define FT_LV_RING	256
static __thread struct {
	void **slot;
	int site;
} ft_lv_ring[FT_LV_RING];
static __thread unsigned int ft_lv_ring_n;

static int ft_lv_site_of(const void *ra)
{
	int i;

	for (i = 0; i < FT_LV_SITES; i++) {
		const void *cur = CMM_LOAD_SHARED(ft_lv_sites[i].ra);

		if (cur == ra)
			return i;
		if (!cur)
			break;
	}
	pthread_mutex_lock(&ft_lv_lock);
	for (i = 0; i < FT_LV_SITES; i++) {
		if (ft_lv_sites[i].ra == ra)
			break;
		if (!ft_lv_sites[i].ra) {
			CMM_STORE_SHARED(ft_lv_sites[i].ra, ra);
			break;
		}
	}
	pthread_mutex_unlock(&ft_lv_lock);
	if (i < FT_LV_SITES)
		return i;
	uatomic_inc(&ft_lv_site_overflow);
	return -1;
}

/* @self / @held: 1, 0, or -1 when not asked. */
static void ft_lv_note(void **slot, const void *ra, int self, int held)
{
	int si = ft_lv_site_of(ra);
	struct ft_lv_site *st;

	ft_lv_ring[ft_lv_ring_n % FT_LV_RING].slot = slot;
	ft_lv_ring[ft_lv_ring_n % FT_LV_RING].site = si;
	ft_lv_ring_n++;
	if (si < 0)
		return;
	st = &ft_lv_sites[si];
	uatomic_inc(&st->rec);
	if (self == 1)
		uatomic_inc(&st->rec_self);
	else if (self == 0)
		uatomic_inc(&st->rec_notself);
	if (held == 1)
		uatomic_inc(&st->rec_held);
	else if (held == 0)
		uatomic_inc(&st->rec_notheld);
}

static void ft_lv_lost(const struct urcu_txn_record *r)
{
	unsigned int i, n = ft_lv_ring_n < FT_LV_RING ? ft_lv_ring_n : FT_LV_RING;
	int si = -1, found = 0;
	void *seen;
	enum ft_lv_cause c;

	for (i = 0; i < n; i++) {
		unsigned int k = (ft_lv_ring_n - 1 - i) % FT_LV_RING;

		if (ft_lv_ring[k].slot == r->slot) {
			si = ft_lv_ring[k].site;
			found = 1;
			break;
		}
	}
	if (!found)
		return;		/* not a live validation this thread recorded */
	seen = ft_win_seen_tls.valid && ft_win_seen_tls.rec == r ?
		ft_win_seen_tls.seen : NULL;
	if (!seen) {
		c = FT_LV_SEEN_NONE;
	} else if (urcu_txn_is_proxy(seen, FT_STATE_PROXY)) {
		const struct urcu_txn_record *wr =
			urcu_txn_untag(seen, FT_STATE_PROXY);
		unsigned int wcls = ft_ab_code_cls(
			CMM_LOAD_SHARED(wr->dbg_embedder));

		c = wcls == FT_AB_MW_LOCK ? FT_LV_PROXY_LOCKTAKE :
			wcls == FT_AB_SW ? FT_LV_PROXY_SW : FT_LV_PROXY_OTHER;
	} else {
		uintptr_t sv = (uintptr_t) seen;
		uintptr_t diff = sv ^ (uintptr_t) r->old_ptr;

		if (sv & FT_STATE_LOCK) {
#ifdef FEATURE_FT_HOLD_TRACE
			const struct cds_ft_metadata *m = caa_container_of(
				(uintptr_t *) r->slot,
				struct cds_ft_metadata, state);

			if (CMM_LOAD_SHARED(m->dbg_owner_tid)) {
				const char *fn = m->dbg_owner_fn;
				int line = m->dbg_owner_line;

				c = FT_LV_LOCK_MEMBER;
				pthread_mutex_lock(&ft_lv_lock);
				for (i = 0; i < FT_LV_FNLINE; i++) {
					if (!ft_lv_fnline[i].fn) {
						ft_lv_fnline[i].fn = fn;
						ft_lv_fnline[i].line = line;
					}
					if (ft_lv_fnline[i].fn == fn &&
							ft_lv_fnline[i].line == line) {
						ft_lv_fnline[i].n++;
						break;
					}
				}
				pthread_mutex_unlock(&ft_lv_lock);
			} else {
				c = FT_LV_LOCK_ANCHOR;
			}
#else
			c = FT_LV_LOCK_ANCHOR;
#endif
		} else if (sv & FT_STATE_TOMBSTONE) {
			c = FT_LV_TOMB;
		} else if (!(diff & ~FT_STATE_NR_CHILD_MASK)) {
			c = FT_LV_NRCHILD;
		} else {
			c = FT_LV_OTHER;
		}
	}
	if (si < 0)
		return;
	uatomic_inc(&ft_lv_sites[si].lost);
	uatomic_inc(&ft_lv_sites[si].cause[c]);
}

static void ft_lv_report(void) __attribute__((destructor));
static void ft_lv_report(void)
{
	int i, j;

	fprintf(stderr, "\n=== FT_LIVE_VALIDATE: {live -> live} child-state checks, per re-home call site (ra base: cds_ft_create=%p) ===\n",
		(void *) &cds_ft_create);
	for (i = 0; i < FT_LV_SITES; i++) {
		const struct ft_lv_site *st = &ft_lv_sites[i];

		if (!st->ra)
			continue;
		fprintf(stderr, "FT_LV ra=%p rec=%lu self=%lu notself=%lu "
			"held=%lu notheld=%lu lost=%lu", st->ra, st->rec,
			st->rec_self, st->rec_notself, st->rec_held,
			st->rec_notheld, st->lost);
		for (j = 0; j < FT_LV_NR; j++)
			if (st->cause[j])
				fprintf(stderr, " %s=%lu", ft_lv_cause_name[j],
					st->cause[j]);
		fprintf(stderr, "\n");
	}
	for (i = 0; i < FT_LV_FNLINE && ft_lv_fnline[i].fn; i++)
		fprintf(stderr, "FT_LV LOCK_member holder %s:%d n=%lu\n",
			ft_lv_fnline[i].fn, ft_lv_fnline[i].line,
			ft_lv_fnline[i].n);
	fprintf(stderr, "FT_LV site_overflow=%lu\n", ft_lv_site_overflow);
}
#endif	/* FT_DEBUG_LIVE_VALIDATE */

static
void ft_ab_note_lost(const struct urcu_txn_desc *t,
		const struct urcu_txn_record *r)
{
	ft_ab_lost = r ? r->dbg_embedder :
		ft_ab_code(FT_AB_UNSET, FT_AB_OWN_NA);
	ft_ab_lost_tag = r ? r->proxy_tag : 0;
	ft_ab_lost_valid = r != NULL;
#ifdef FT_DEBUG_LIVE_VALIDATE
	if (r && r->proxy_tag == FT_STATE_PROXY && r->old_ptr == r->new_ptr)
		ft_lv_lost(r);
#endif
#ifdef FT_WINNER_DBG
	/*
	 * The winner lookup runs HERE, at the first instant after the lost
	 * CAS.  Every microsecond of delay lets more writes land on the slot
	 * and turns ATTRIBUTED into STALE.
	 */
	ft_win_last.state = FT_WIN_NONE;
	if (r && ft_ab_is_alarm(ft_ab_code_cls(ft_ab_lost),
			ft_ab_code_own(ft_ab_lost)))
		ft_win_lookup(r);
	/*
	 * Consume the stash on EVERY abort.  Records live in a reused slab.
	 * Otherwise a stale {rec, seen} pair could match a LATER alarm's
	 * record by address, and attribute an old beater to it.
	 */
	ft_win_seen_tls.valid = 0;
#endif
#ifdef FT_ABORT_CLAIM
	/*
	 * THE DESCRIPTOR IS STILL INTACT HERE and is freed on the way out.  So
	 * this is the only place the losing record can be read against its
	 * SIBLINGS, and that is how it is read.  "This CAS lost" is not a
	 * diagnosis.  "These two records disagree about whether the op holds
	 * that word" is one.  `now=` is what makes a mismatch a diagnosis: it
	 * says which of the two is lying.
	 */
	if (r && ft_ab_is_alarm(ft_ab_code_cls(ft_ab_lost),
			ft_ab_code_own(ft_ab_lost))) {
		unsigned int i;

		fprintf(stderr,
"\n[FT_ABORT_CLAIM] a record that CANNOT lose, lost.  nr=%u nr_mw=%u retry=%u\n"
"    ra base: cds_ft_create=%p  (file offset of ra = nm(cds_ft_create) + ra - this)\n",
			t->nr, t->nr_mw, t->retry, (void *) &cds_ft_create);
		for (i = 0; i < t->nr; i++) {
			const struct urcu_txn_record *q = &t->recs[i];

			fprintf(stderr,
"    rec[%2u] %-14s %s%s slot=%p old=%p new=%p now=%p tag=0x%lx owner=%p ra=%p%s\n",
				i, ft_ab_cls_name(ft_ab_code_cls(q->dbg_embedder)),
				q->kind == URCU_TXN_KIND_SW ? "SW" : "MW",
				ft_ab_code_own(q->dbg_embedder) == FT_AB_OWN_HELD ?
					"/held" : (ft_ab_code_own(q->dbg_embedder) ==
						FT_AB_OWN_MISS ? "/MISS " : "     "),
				(void *) q->slot, q->old_ptr, q->new_ptr,
				(void *) CMM_LOAD_SHARED(*q->slot),
				(unsigned long) q->proxy_tag,
				ft_ab_owner_of(q->slot), ft_ab_ra_of(q->slot),
				q == r ? "   <== LOST" : "");
		}
#ifdef FT_WINNER_DBG
		{
			static const char * const wst[] = {
				"NONE (no observed value)",
				"ledger-corroborated",
				"★ PROXY-DECODED (exact)",
				"ledger STALE",
				"no ledger entry",
				"ledger TORN",
				"☠ RAW-LANE store",
			};

			fprintf(stderr,
"    beater: %s  seen=%p now=%p  cls=%s own=%u owner=%p ra=%p tid=0x%lx%s%s\n",
				wst[ft_win_last.state],
				ft_win_last.seen, ft_win_last.now,
				ft_ab_cls_name(ft_ab_code_cls(ft_win_last.code)),
				ft_ab_code_own(ft_win_last.code),
				ft_win_last.owner, ft_win_last.ra,
				ft_win_last.tid,
				ft_win_last.owner_tomb ?
					"  ☠ REGISTERED OWNER TOMBSTONED" : "",
				ft_win_last.owner_unparented ?
					"  ☠ REGISTERED OWNER MID RE-HOME (parent_word NULL)" : "");
		}
#endif
	}
#endif
}

#ifdef FT_DEBUG_SAME_SLOT
#include <execinfo.h>
#include <dlfcn.h>
/*
 * THE SAME-SLOT CENSUS.  It is defined here, where the record is complete.
 * The hook and its reason are in ft-txn-rec-dbg.h.  Each hit is classified by
 * what the SW engine would need to know about it:
 *
 *   had / got   the FT class of the record already there and of the new one
 *               (FT_AB_MIXED when two classes were already folded onto it);
 *   kinds       SW or MW, for both;
 *   validate    old == new for the one already there / the new one is a
 *               load-validate or a same-value store;
 *   chained     the new expected-old IS the pending new value -- a fusable
 *               read-your-own-writes store.  Unchained, the MW engine POISONS
 *               the descriptor (the commit aborts), so those are the ones a
 *               swap would turn from an abort into a silent last-wins.
 *
 * A sampled backtrace names the producer.  The sample is the first 64 hits
 * per thread, then 1 in 256.  The backtrace gives module+offset, for
 * addr2line -f -i against that module.
 */
#define FT_SS_HAD	(FT_AB_CLS_NR + 1)	/* + MIXED */
#define FT_SS_STACKS	512
#define FT_SS_DEPTH	12

static unsigned long ft_ss_tab[FT_SS_HAD][FT_AB_CLS_NR][2][2][2][2][2];
static unsigned long ft_ss_total, ft_ss_declared;
/*
 * The LOCK bit's transition on each side of the pair.  A rank count carries
 * no lock bit, but a large count can set the same bit and land here too:
 *
 *   0  no lock bit
 *   1  RELEASE (L -> ~L)
 *   2  edit UNDER the lock (L -> L)
 *   3  TAKE (~L -> L)
 *
 * A pair with a RELEASE on either side is a chain.  The split-out release
 * removes that chain by itself.  The rest are content edits the caller must
 * combine.
 */
static unsigned long ft_ss_lock[4][4];

static inline
unsigned int ft_ss_lock_shape(const void *o, const void *n)
{
	uintptr_t ov = (uintptr_t) o, nv = (uintptr_t) n;

	if (!(ov & FT_STATE_LOCK))
		return (nv & FT_STATE_LOCK) ? 3 : 0;
	return (nv & FT_STATE_LOCK) ? 2 : 1;
}

struct ft_ss_stack {
	unsigned int key;
	int depth;
	unsigned long n;
	void *pc[FT_SS_DEPTH];
};
static struct ft_ss_stack ft_ss_stacks[FT_SS_STACKS];
static unsigned int ft_ss_nstacks;
static unsigned long ft_ss_overflow;
static pthread_mutex_t ft_ss_mutex = PTHREAD_MUTEX_INITIALIZER;
static __thread unsigned long ft_ss_tcount;

static __attribute__((noinline))
void ft_ss_note(const struct urcu_txn_record *r, void *old_ptr,
		void *new_ptr, int upgrade, unsigned int kind)
{
	unsigned int had = r->dbg_embedder, got = ft_ab_pending;
	unsigned int hc = (had & FT_AB_MIXED) ? FT_AB_CLS_NR :
		ft_ab_code_cls(had);
	unsigned int gc = ft_ab_code_cls(got);
	unsigned int hk = r->kind == URCU_TXN_KIND_MW;
	unsigned int gk = kind == URCU_TXN_KIND_MW;
	unsigned int hv = r->old_ptr == r->new_ptr;
	unsigned int gv = !upgrade || old_ptr == new_ptr;
	unsigned int ch = r->new_ptr == old_ptr;
	unsigned int key, i;
	void *pc[FT_SS_DEPTH + 1];
	int depth;

	if (ft_ss_fold_depth) {
		uatomic_inc(&ft_ss_declared);
		return;
	}
	if (hc >= FT_SS_HAD)
		hc = FT_AB_UNSET;
	if (gc >= FT_AB_CLS_NR)
		gc = FT_AB_UNSET;
	uatomic_inc(&ft_ss_tab[hc][gc][hk][gk][hv][gv][ch]);
	uatomic_inc(&ft_ss_total);
	uatomic_inc(&ft_ss_lock[ft_ss_lock_shape(r->old_ptr, r->new_ptr)]
		[ft_ss_lock_shape(old_ptr, new_ptr)]);
	if (ft_ss_tcount++ >= 64 && (ft_ss_tcount & 255))
		return;
	key = (((((hc * FT_AB_CLS_NR + gc) * 2 + hk) * 2 + gk) * 2 + hv) * 2
		+ gv) * 2 + ch;
	depth = backtrace(pc, FT_SS_DEPTH + 1) - 1;	/* drop this frame */
	if (depth < 0)
		depth = 0;
	pthread_mutex_lock(&ft_ss_mutex);
	for (i = 0; i < ft_ss_nstacks; i++) {
		struct ft_ss_stack *s = &ft_ss_stacks[i];

		if (s->key == key && s->depth == depth &&
				!memcmp(s->pc, pc + 1, depth * sizeof(void *))) {
			s->n++;
			goto out;
		}
	}
	if (ft_ss_nstacks < FT_SS_STACKS) {
		struct ft_ss_stack *s = &ft_ss_stacks[ft_ss_nstacks++];

		s->key = key;
		s->depth = depth;
		s->n = 1;
		memcpy(s->pc, pc + 1, depth * sizeof(void *));
	} else {
		ft_ss_overflow++;
	}
out:
	pthread_mutex_unlock(&ft_ss_mutex);
}

static
const char *ft_ss_had_name(unsigned int hc)
{
	return hc == FT_AB_CLS_NR ? "MIXED" : ft_ab_cls_name(hc);
}

static void ft_ss_report(void) __attribute__((destructor));
static
void ft_ss_report(void)
{
	unsigned int hc, gc, hk, gk, hv, gv, ch, i, j;

	fprintf(stderr, "=== FT_DEBUG_SAME_SLOT: records added onto a slot the "
		"descriptor already holds: %lu, plus %lu DECLARED folds "
		"(the SW engine's record_chain sites) ===\n", ft_ss_total,
		ft_ss_declared);
	fprintf(stderr, "  %-14s %-14s had got  had-val got-val chained %12s\n",
		"had", "got", "n");
	for (hc = 0; hc < FT_SS_HAD; hc++)
	for (gc = 0; gc < FT_AB_CLS_NR; gc++)
	for (hk = 0; hk < 2; hk++)
	for (gk = 0; gk < 2; gk++)
	for (hv = 0; hv < 2; hv++)
	for (gv = 0; gv < 2; gv++)
	for (ch = 0; ch < 2; ch++) {
		unsigned long n = ft_ss_tab[hc][gc][hk][gk][hv][gv][ch];

		if (!n)
			continue;
		fprintf(stderr, "  %-14s %-14s  %s  %s   %d       %d       %d   "
			"%12lu\n", ft_ss_had_name(hc), ft_ab_cls_name(gc),
			hk ? "MW" : "SW", gk ? "MW" : "SW", hv, gv, ch, n);
	}
	{
		static const char * const ls[4] = {
			"nolock", "RELEASE", "under-lock", "TAKE" };

		fprintf(stderr, "  LOCK-bit shape, had x got:\n");
		for (hc = 0; hc < 4; hc++)
			for (gc = 0; gc < 4; gc++)
				if (ft_ss_lock[hc][gc])
					fprintf(stderr, "    %-10s -> %-10s %12lu\n",
						ls[hc], ls[gc],
						ft_ss_lock[hc][gc]);
	}
	fprintf(stderr, "  sampled stacks: %u (overflow %lu)\n", ft_ss_nstacks,
		ft_ss_overflow);
	for (i = 0; i < ft_ss_nstacks; i++) {
		const struct ft_ss_stack *s = &ft_ss_stacks[i];
		unsigned int k = s->key;

		ch = k & 1; k >>= 1; gv = k & 1; k >>= 1; hv = k & 1; k >>= 1;
		gk = k & 1; k >>= 1; hk = k & 1; k >>= 1;
		gc = k % FT_AB_CLS_NR; hc = k / FT_AB_CLS_NR;
		fprintf(stderr, "FT_SS_STACK n=%lu %s->%s %s->%s val=%d/%d "
			"chained=%d :", s->n, ft_ss_had_name(hc),
			ft_ab_cls_name(gc), hk ? "MW" : "SW", gk ? "MW" : "SW",
			hv, gv, ch);
		for (j = 0; j < (unsigned int) s->depth; j++) {
			Dl_info di;

			if (dladdr(s->pc[j], &di) && di.dli_fname)
				fprintf(stderr, " %s+%#lx",
					strrchr(di.dli_fname, '/') ?
					strrchr(di.dli_fname, '/') + 1 :
					di.dli_fname,
					(unsigned long) ((char *) s->pc[j] -
						(char *) di.dli_fbase));
			else
				fprintf(stderr, " %p", s->pc[j]);
		}
		fprintf(stderr, "\n");
	}
}
#endif	/* FT_DEBUG_SAME_SLOT */

/*
 * ★ THE ALARM.  An SW park cannot lose a CAS, because it does not do one.  A
 * STRUCTURAL MW record whose owner the op HOLDS should not lose one either.
 * The DLM lock over the slot's owner is precisely what excludes every peer
 * writer of that word.  Either sighting says the exclusion is not what it
 * claims.  That is the opposite of a conversion opportunity.  Arming such a
 * site would turn a contested word into an unarbitrated plain store.
 *
 * ☠ NOT AN ASSERT, YET.  It is counted first.  A detector that has never
 * been shown to reach its subject is not coverage
 * ([[feedback_an_assert_config_that_never_fires_is_not_coverage]]).
 * -DFT_ABORT_CLAIM turns it into an abort at the first sighting.
 */
static
int ft_ab_is_alarm(unsigned int cls, unsigned int own)
{
	if (cls == FT_AB_SW)
		return 1;
	return cls == FT_AB_MW_STRUCT && own == FT_AB_OWN_HELD;
}

static inline
void ft_ab_count_pub_pair(int in_node)
{
	ft_tk_tls_get()->pub_pair[!!in_node]++;
}

static inline
void ft_ab_count_lost(struct ft_tk_site *site)
{
	struct ft_tk_tls *tls = ft_tk_tls_get();
	int id = site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID;
	unsigned int cls, own;

	if (!ft_ab_lost_valid) {
		tls->ab_unattrib++;
		return;
	}
	cls = ft_ab_code_cls(ft_ab_lost);
	own = ft_ab_code_own(ft_ab_lost);
	if (cls >= FT_AB_CLS_NR)
		cls = FT_AB_UNSET;	/* a stamp this build does not know */
	tls->ab[id][cls]++;
	tls->ab_own[cls][own]++;
	if (ft_ab_lost & FT_AB_MIXED)
		tls->ab_mixed++;
	if (ft_ab_is_alarm(cls, own)) {
		int ptr = ft_ab_lost_tag == FT_FLIP_PROXY_TAG;

		tls->ab_alarm[id][ptr]++;
#ifdef FT_ABORT_CLAIM
		fprintf(stderr,
"    at %s:%d %s -- class=%s witness=%u word=%s%s\n",
			site ? site->file : "?", site ? site->line : 0,
			site ? site->what : "?", ft_ab_cls_name(cls), own,
			ptr ? "CHILD POINTER SLOT" : "packed state word",
			(ft_ab_lost & FT_AB_MIXED) ? " MIXED" : "");
		ft_ab_claim_armed = 1;	/* the caller prints its registry, then aborts */
#endif
	}
}
#endif /* FT_ABORT_ATTRIB */

/*
 * The site object is a function-static: one per expansion of the constructor
 * macro, which is one per source line that creates a txn.
 */
#define FT_TK_SITE_HERE(what_)						\
	__extension__ ({						\
		static struct ft_tk_site __ft_tk_site = {		\
			__FILE__, __LINE__, (what_), -1,		\
		};							\
		&__ft_tk_site;						\
	})

/*
 * THE SITE PARAMETER EXISTS ONLY IN THE INSTRUMENTED BUILD.  It is spelled
 * into the constructors' signatures rather than added unconditionally.  So a
 * build without the knob is not merely "cheap" but IDENTICAL.  An extra
 * always-NULL argument is free at runtime.  But it still moves the
 * compiler's inlining and layout decisions.  This instrument must not
 * perturb the code paths whose latency it is used to compare.
 */
#define FT_TK_SITE_PARAM	struct ft_tk_site *dbg_site,
#define FT_TK_SITE_FWD		dbg_site,

struct ft_tk_row {
	const struct ft_tk_site *site;
	unsigned long rec[FT_TK_REC_NR];
	unsigned long end[FT_TK_END_NR];
	unsigned long created;
	unsigned long armed;
};

static
int ft_tk_row_cmp(const void *a, const void *b)
{
	const struct ft_tk_row *ra = (const struct ft_tk_row *) a;
	const struct ft_tk_row *rb = (const struct ft_tk_row *) b;

	if (ra->end[FT_TK_ABORT] != rb->end[FT_TK_ABORT])
		return ra->end[FT_TK_ABORT] > rb->end[FT_TK_ABORT] ? -1 : 1;
	return strcmp(ra->site->file, rb->site->file);
}

static
void ft_tk_dump(void)
{
	struct ft_tk_row *rows;
	struct ft_tk_tls *tls;
	unsigned long chain_sw = 0;
#ifdef FT_ABORT_ATTRIB
	unsigned long ab[FT_AB_CLS_NR], ab_own[FT_AB_CLS_NR][FT_AB_OWN_NR];
	unsigned long ab_alarm_site[FT_TK_MAX_SITES][2];
	unsigned long ab_tot = 0, ab_alarm[2] = { 0, 0 }, pub_pair[2] = { 0, 0 };
	unsigned long ab_mixed = 0, ab_unattrib = 0;
#ifdef FT_WINNER_DBG
	unsigned long win_beater[FT_AB_CLS_NR][FT_AB_OWN_NR];
	unsigned long win_raw[FT_WIN_RAW_NR];
	unsigned long win_owner[3] = { 0, 0, 0 };
	unsigned long win_beater_proxy = 0, win_beater_ledger = 0;
	unsigned long win_ledger_mismatch = 0, win_ledger_absent = 0;
	unsigned long win_seen_none = 0, win_drift = 0;
	unsigned long win_tid_same = 0, win_torn = 0;
	unsigned long win_owner_tomb = 0, win_owner_unparented = 0;
	unsigned long win_note = 0, win_note_skip = 0;
#endif
#endif
	struct ft_tk_row tot;
	int nr, i, threads = 0;
	char name[96];

	pthread_mutex_lock(&ft_tk_lock);
	nr = ft_tk_nr_sites;
	if (ft_tk_sites[FT_TK_OVERFLOW_ID])
		nr = FT_TK_MAX_SITES;
#ifdef FT_ABORT_ATTRIB
	memset(ab, 0, sizeof(ab));
	memset(ab_own, 0, sizeof(ab_own));
	memset(ab_alarm_site, 0, sizeof(ab_alarm_site));
#ifdef FT_WINNER_DBG
	memset(win_beater, 0, sizeof(win_beater));
	memset(win_raw, 0, sizeof(win_raw));
#endif
#endif
	rows = (struct ft_tk_row *) calloc(nr ? nr : 1, sizeof(*rows));
	if (!rows) {
		pthread_mutex_unlock(&ft_tk_lock);
		return;
	}
	for (i = 0; i < nr; i++)
		rows[i].site = ft_tk_sites[i];
	for (tls = ft_tk_threads; tls; tls = tls->next) {
		int c;

		threads++;
		chain_sw += tls->chain_sw;
#ifdef FT_ABORT_ATTRIB
		{
			int c2, o2;

			for (c2 = 0; c2 < FT_AB_CLS_NR; c2++) {
				for (i = 0; i < nr; i++)
					ab[c2] += tls->ab[i][c2];
				for (o2 = 0; o2 < FT_AB_OWN_NR; o2++)
					ab_own[c2][o2] += tls->ab_own[c2][o2];
			}
			for (i = 0; i < nr; i++) {
				for (o2 = 0; o2 < 2; o2++) {
					ab_alarm[o2] += tls->ab_alarm[i][o2];
					ab_alarm_site[i][o2] += tls->ab_alarm[i][o2];
				}
			}
			ab_mixed += tls->ab_mixed;
			ab_unattrib += tls->ab_unattrib;
			pub_pair[0] += tls->pub_pair[0];
			pub_pair[1] += tls->pub_pair[1];
#ifdef FT_WINNER_DBG
			for (c2 = 0; c2 < FT_AB_CLS_NR; c2++)
				for (o2 = 0; o2 < FT_AB_OWN_NR; o2++)
					win_beater[c2][o2] += tls->win_beater[c2][o2];
			for (o2 = 0; o2 < FT_WIN_RAW_NR; o2++)
				win_raw[o2] += tls->win_raw[o2];
			for (o2 = 0; o2 < 3; o2++)
				win_owner[o2] += tls->win_owner[o2];
			win_beater_proxy += tls->win_beater_proxy;
			win_beater_ledger += tls->win_beater_ledger;
			win_ledger_mismatch += tls->win_ledger_mismatch;
			win_ledger_absent += tls->win_ledger_absent;
			win_seen_none += tls->win_seen_none;
			win_drift += tls->win_drift;
			win_tid_same += tls->win_tid_same;
			win_torn += tls->win_torn;
			win_owner_tomb += tls->win_owner_tomb;
			win_owner_unparented += tls->win_owner_unparented;
			win_note += tls->win_note;
			win_note_skip += tls->win_note_skip;
#endif
		}
#endif
		for (i = 0; i < nr; i++) {
			if (!rows[i].site)
				continue;
			for (c = 0; c < FT_TK_REC_NR; c++)
				rows[i].rec[c] += tls->rec[i][c];
			for (c = 0; c < FT_TK_END_NR; c++)
				rows[i].end[c] += tls->end[i][c];
			rows[i].created += tls->created[i];
			rows[i].armed += tls->armed[i];
		}
	}
	pthread_mutex_unlock(&ft_tk_lock);

	/* Drop the rows that never registered (a full table has holes). */
	for (i = 0; i < nr; ) {
		if (rows[i].site) {
			i++;
			continue;
		}
		rows[i] = rows[--nr];
	}
	qsort(rows, nr, sizeof(*rows), ft_tk_row_cmp);

	memset(&tot, 0, sizeof(tot));
	fprintf(stderr,
"\n=== FT_DEBUG_TXN_KIND: record kind + commit outcome, per txn creation site ===\n"
"    threads=%d sites=%d\n"
"%-44s %9s %7s %10s %10s %9s %8s %7s %8s\n",
		threads, nr,
		"site", "created", "armSW", "SW",
		"OK", "ABORT", "MEMERR", "MISS", "BAILED");
	for (i = 0; i < nr; i++) {
		int c;

		if (rows[i].site->line)
			snprintf(name, sizeof(name), "%s:%d %s",
				rows[i].site->file, rows[i].site->line,
				rows[i].site->what);
		else
			snprintf(name, sizeof(name), "%s", rows[i].site->file);
		fprintf(stderr,
"%-44s %9lu %7lu %10lu %10lu %9lu %8lu %7lu %8lu\n",
			name, rows[i].created, rows[i].armed,
			rows[i].rec[FT_TK_SW],
			rows[i].end[FT_TK_OK], rows[i].end[FT_TK_ABORT],
			rows[i].end[FT_TK_MEMERR], rows[i].end[FT_TK_MISS],
			rows[i].end[FT_TK_BAILED]);
		for (c = 0; c < FT_TK_REC_NR; c++)
			tot.rec[c] += rows[i].rec[c];
		for (c = 0; c < FT_TK_END_NR; c++)
			tot.end[c] += rows[i].end[c];
		tot.created += rows[i].created;
		tot.armed += rows[i].armed;
	}
	fprintf(stderr,
"%-44s %9lu %7lu %10lu %10lu %9lu %8lu %7lu %8lu\n",
		"TOTAL", tot.created, tot.armed,
		tot.rec[FT_TK_SW],
		tot.end[FT_TK_OK], tot.end[FT_TK_ABORT],
		tot.end[FT_TK_MEMERR], tot.end[FT_TK_MISS],
		tot.end[FT_TK_BAILED]);
	fprintf(stderr,
"    DUPLICATE-CHAIN stores (cds_ft_node.next/.prev; straight on the engine handle, not site-attributed): SW=%lu\n",
		chain_sw);

#ifdef FT_ABORT_ATTRIB
	/*
	 * WHICH RECORD LOST, over every aborted commit.  The rows sum to the
	 * ABORT column above.  This self-check also proves that the engine
	 * hooks cover every abort EXIT.  There are three, and the lone-MW-edge one builds
	 * no descriptor at all.
	 */
	{
		for (i = 0; i < FT_AB_CLS_NR; i++)
			ab_tot += ab[i];
		fprintf(stderr,
"\n    ABORT attributed to the LOSING RECORD's class (%lu aborted commits):\n",
			ab_tot);
		for (i = 0; i < FT_AB_CLS_NR; i++) {
			if (!ab[i])
				continue;
			fprintf(stderr,
"      %-16s %12lu  %5.1f%%   held %lu / ledger %lu / miss %lu / n-a %lu\n",
				ft_ab_names[i], ab[i],
				ab_tot ? 100.0 * (double) ab[i] / (double) ab_tot : 0.0,
				ab_own[i][FT_AB_OWN_HELD], ab_own[i][FT_AB_OWN_LEDGER],
				ab_own[i][FT_AB_OWN_MISS], ab_own[i][FT_AB_OWN_NA]);
		}
		fprintf(stderr,
"      %-16s %12lu  (ABORT column %lu%s)\n"
"      mixed-class losing words: %lu   aborts no hook could name: %lu\n",
			"sum", ab_tot, tot.end[FT_TK_ABORT],
			ab_tot + ab_unattrib == tot.end[FT_TK_ABORT] ? "" :
				" -- ☠ MISMATCH: an abort EXIT is unhooked",
			ab_mixed, ab_unattrib);
		/*
		 * ☠ SPLIT BY THE WORD, because the two halves are not the
		 * same finding.  A child POINTER slot may only be written by
		 * a holder of the owning node's lock.  So an owner-held
		 * record losing one is an exclusion that did not exclude.
		 * The packed STATE word has lock-free writers BY DESIGN: the
		 * re-parent sweep's guard and the §4.B validates.  So losing
		 * one says only that a non-holder wrote another field of the
		 * same word.  Pooling them would manufacture a defect.
		 */
		fprintf(stderr,
"    glue publish PAIRING (ft_slot_in_node(publish_parent, publish_slot)):"
" in %lu / ☠ NOT-IN %lu\n",
			pub_pair[1], pub_pair[0]);
		fprintf(stderr,
"    ★ ALARM (a record that cannot lose, lost): pointer-slot %lu / state-word %lu\n"
"      %s\n",
			ab_alarm[1], ab_alarm[0],
			ab_alarm[1] ?
				"☠ POINTER SLOT: an OWNER-HELD structural edge lost a CAS on a word only a holder may write" :
				(ab_alarm[0] ?
					"state word only: a non-holder wrote another field of the same packed word (by design)" :
					"(none -- the exclusion held everywhere it was claimed)"));
		/*
		 * ☠ BY SITE ID, not by row index.  The rows were sorted
		 * above, so row i is no longer site i.
		 */
		for (i = 0; (ab_alarm[0] || ab_alarm[1]) && i < nr; i++) {
			int id = rows[i].site->id;

			if (id >= 0 && id < FT_TK_MAX_SITES &&
					(ab_alarm_site[id][0] || ab_alarm_site[id][1]))
				fprintf(stderr,
					"        %s:%d %s  ptr %lu / state %lu\n",
					rows[i].site->file, rows[i].site->line,
					rows[i].site->what,
					ab_alarm_site[id][1], ab_alarm_site[id][0]);
		}
#ifdef FT_WINNER_DBG
		/*
		 * WHO BEAT the alarmed word.  The primary witness is the
		 * value that the losing CAS OBSERVED (URCU_TXN_REC_LOST).  A
		 * proxy there decodes to the winning record EXACTLY.  A plain
		 * value is only ledger-CORROBORATED.  That is a population
		 * claim: ABA can name an older write of the same value.  The
		 * rows are the WINNER's label.  The loser is always the
		 * owner-held structural record above.  The reach line is the
		 * proof the stamps ran at all
		 * ([[feedback_verify_the_mechanism_ran_before_believing_a_zero]]).
		 */
		{
			static const char * const raw_names[FT_WIN_RAW_NR] = {
				"lone-edge flip (ft_ord_cell_flip_one)",
				"remove head-promote back store",
			};
			unsigned long win_tot = 0, raw_tot = 0;
			int c3, o3;

			for (c3 = 0; c3 < FT_AB_CLS_NR; c3++)
				for (o3 = 0; o3 < FT_AB_OWN_NR; o3++)
					win_tot += win_beater[c3][o3];
			for (o3 = 0; o3 < FT_WIN_RAW_NR; o3++)
				raw_tot += win_raw[o3];
			fprintf(stderr,
"\n    ★ BEATER of the alarmed word (-DFT_WINNER_DBG):\n"
"      ledger reach: %lu writes stamped / %lu skipped on entry contention\n"
"      engine winner %lu = ★ proxy-decoded (exact) %lu + ledger-corroborated %lu (self-tid %lu)\n"
"      ☠ raw-lane winner %lu / plain unmatched: ledger-mismatch %lu + no-entry %lu / torn %lu\n"
"      no observed value at the loss exit: %lu    drift (slot moved again before lookup): %lu\n"
"      corroborated winner's named owner vs the loser's: same %lu / ☠ DIFFERENT %lu / unknown %lu\n"
"      loser's registered owner AT the alarm: ☠ TOMBSTONED %lu / ☠ MID RE-HOME (parent_word NULL) %lu\n",
				win_note, win_note_skip,
				win_tot, win_beater_proxy, win_beater_ledger,
				win_tid_same,
				raw_tot, win_ledger_mismatch, win_ledger_absent,
				win_torn, win_seen_none, win_drift,
				win_owner[0], win_owner[1], win_owner[2],
				win_owner_tomb, win_owner_unparented);
			for (c3 = 0; c3 < FT_AB_CLS_NR; c3++) {
				for (o3 = 0; o3 < FT_AB_OWN_NR; o3++) {
					if (!win_beater[c3][o3])
						continue;
					fprintf(stderr,
"        %-16s witness=%s %12lu  %5.1f%%\n",
						ft_ab_cls_name(c3),
						o3 == FT_AB_OWN_HELD ? "held" :
						(o3 == FT_AB_OWN_LEDGER ? "ledger" :
						(o3 == FT_AB_OWN_MISS ? "MISS" : "n/a")),
						win_beater[c3][o3],
						win_tot ? 100.0 *
							(double) win_beater[c3][o3] /
							(double) win_tot : 0.0);
				}
			}
			for (o3 = 0; o3 < FT_WIN_RAW_NR; o3++)
				if (win_raw[o3])
					fprintf(stderr,
"        ☠ raw: %-40s %12lu\n",
						raw_names[o3], win_raw[o3]);
		}
#endif
	}
#endif
	{
		fprintf(stderr,
"    DOOR 1/3 SW parks, is the trie-wide exclusion REALLY in force?\n"
"      sw_body(door 3)=%lu  exclusive=%lu  coarse=%lu  FT-wide-lock=%lu  ☠ NONE(must be 0)=%lu\n",
			uatomic_read(&ft_tk_d13_body),
			uatomic_read(&ft_tk_d13_excl),
			uatomic_read(&ft_tk_d13_coarse),
			uatomic_read(&ft_tk_d13_wlock),
			uatomic_read(&ft_tk_d13_none));
		fprintf(stderr,
"      ☞ door 1's zero above is TAUTOLOGICAL (its arm predicate IS !lock_fine||exclusive).\n"
"      DOOR 3 (@sw_body) by mode -- the only licence here that no predicate checks:\n"
"        exclusive=%lu  coarse=%lu  FT-wide-lock=%lu  ☠ FINE, self-fenced only=%lu\n",
			uatomic_read(&ft_tk_d3_excl),
			uatomic_read(&ft_tk_d3_coarse),
			uatomic_read(&ft_tk_d3_wlock),
			uatomic_read(&ft_tk_d3_fine));
		fprintf(stderr,
"    RETIRE, is the LOCK-BEARING word held at the tombstone?  held-by-US=%lu  held-by-OTHER=%lu\n"
"      unlocked: FINE(the only one owed an answer)=%lu  coarse=%lu  exclusive=%lu  FT-wide-lock=%lu\n",
			uatomic_read(&ft_tk_ret_held_us),
			uatomic_read(&ft_tk_ret_held_other),
			uatomic_read(&ft_tk_ret_unlocked_fine),
			uatomic_read(&ft_tk_ret_unlocked_coarse),
			uatomic_read(&ft_tk_ret_unlocked_excl),
			uatomic_read(&ft_tk_ret_unlocked_wlock));
	}
	free(rows);
}

static __attribute__((destructor))
void ft_tk_dump_at_exit(void)
{
	ft_tk_dump();
}

/*
 * What a struct ft_flip_txn carries for this instrument, and how the record
 * helpers reach it.  @dbg_lock_take is the one piece the dispatch cannot
 * infer.  The DLM lock take goes through the SAME structural record helper as
 * every content edge.  Only its caller knows it is the arbitration point.
 */
#define FT_TK_TXN_FIELDS						\
	struct ft_tk_site *dbg_site;					\
	bool dbg_lock_take;						\
	bool dbg_ended;

/*
 * ☠ FIRST, before ANY other counting call on @t.  Every other FT_TK_* macro
 * reads @t->dbg_site to find the row it credits.  A constructor's @t comes
 * from malloc.  So a count taken before this one dereferences uninitialised
 * memory as a struct ft_tk_site *.  The resulting fault does not look like an
 * instrumentation bug either.  It happens inside the trie's own hot path, on
 * a thread doing ordinary work.  FT_TK_COUNT_ARMED is the one that will find
 * it, because arming is decided inside the constructor itself.
 */
#define FT_TK_TXN_INIT(t, site)						\
	do {								\
		(t)->dbg_site = (site);					\
		(t)->dbg_lock_take = false;				\
		(t)->dbg_ended = false;					\
		ft_tk_count_created(site);				\
	} while (0)

#define FT_TK_TXN_SITE(t)		((t)->dbg_site)
#define FT_TK_TXN_IS_TAKE(t)		((t)->dbg_lock_take)
#define FT_TK_TXN_SET_TAKE(t, v)	do { (t)->dbg_lock_take = (v); } while (0)
#define FT_TK_COUNT_REC(t, c)		ft_tk_count_rec((t)->dbg_site, (c))
#define FT_TK_COUNT_ARMED(t)		ft_tk_count_armed((t)->dbg_site)
#define FT_TK_COUNT_CHAIN_SW()		ft_tk_count_chain_sw()
#ifdef FT_ABORT_ATTRIB
#define FT_AB_LOST_RESET()		do { ft_ab_lost_valid = 0; } while (0)
#define FT_AB_COUNT_LOST(t)		ft_ab_count_lost((t)->dbg_site)
#define FT_AB_COUNT_PUB_PAIR(ok)	ft_ab_count_pub_pair(ok)
#ifdef FT_ABORT_CLAIM
/*
 * THE OTHER HALF OF THE CLAIM.  It has to be a macro expanded at the call
 * site.  The alarm is an accusation about the op's LOCK REGISTRY, and struct
 * ft_flip_txn is not defined yet where ft_ab_count_lost is.  The engine
 * printed the descriptor.  This prints WHO THIS OP THOUGHT IT HELD.  The
 * diagnosis lives in that pair.
 */
#define FT_AB_CLAIM_REPORT(t_)						\
	do {								\
		unsigned int i_;					\
									\
		if (!ft_ab_claim_armed)					\
			break;						\
		fprintf(stderr,						\
			"    registry: nr_locks=%u sw=%d miss=%d\n",	\
			(t_)->nr_locks, (int) (t_)->structural_sw,	\
			(int) (t_)->acquire_miss);			\
		for (i_ = 0; i_ < (t_)->nr_locks; i_++)			\
			fprintf(stderr, "      held[%u] meta=%p snap=0x%lx\n", \
				i_, (void *) (t_)->locks[i_].meta,	\
				(unsigned long) (t_)->locks[i_].snap);	\
		abort();						\
	} while (0)
#else
#define FT_AB_CLAIM_REPORT(t_)		do { } while (0)
#endif
#else
#define FT_AB_LOST_RESET()		do { } while (0)
#define FT_AB_COUNT_LOST(t)		do { } while (0)
#define FT_AB_CLAIM_REPORT(t)		do { } while (0)
#define FT_AB_COUNT_PUB_PAIR(ok)	do { } while (0)
#endif
/*
 * An outcome is recorded ONCE per txn.  ft_flip_txn_commit's acquire-miss arm
 * reports MISS and then destroys the handle.  The destroy must not also
 * report BAILED for it.
 */
#define FT_TK_COUNT_END(t, c)						\
	do {								\
		if (!(t)->dbg_ended) {					\
			(t)->dbg_ended = true;				\
			ft_tk_count_end((t)->dbg_site, (c));		\
		}							\
	} while (0)

#else	/* !FT_DEBUG_TXN_KIND */

struct ft_tk_site;	/* incomplete: the NULL the constructors take */

#define FT_TK_SITE_PARAM
#define FT_TK_SITE_FWD
#define FT_TK_TXN_FIELDS
#define FT_TK_TXN_INIT(t, site)		do { } while (0)
#define FT_TK_TXN_IS_TAKE(t)		0
#define FT_TK_TXN_SET_TAKE(t, v)	do { } while (0)
#define FT_TK_COUNT_REC(t, c)		do { } while (0)
#define FT_TK_COUNT_END(t, c)		do { } while (0)
#define FT_TK_COUNT_ARMED(t)		do { } while (0)
#define FT_TK_COUNT_CHAIN_SW()		do { } while (0)
#define FT_AB_LOST_RESET()		do { } while (0)
#define FT_AB_COUNT_LOST(t)		do { } while (0)
#define FT_AB_CLAIM_REPORT(t)		do { } while (0)
#define FT_AB_COUNT_PUB_PAIR(ok)	do { } while (0)

#endif	/* FT_DEBUG_TXN_KIND */

#endif /* _FT_TXN_KIND_STATS_H */
