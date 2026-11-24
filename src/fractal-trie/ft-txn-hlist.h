// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_HLIST_H
#define _FT_TXN_HLIST_H

/*
 * ft-txn-hlist: the fractal trie's duplicate chain, expressed as a small set of
 * TRANSACTIONAL link primitives on the RCU MCAS engine.  It is an FT-PRIVATE
 * counterpart to the generic concurrent <urcu/rcu-txn-hlist.h>, WITHOUT that
 * header's multi-writer arbitration: every chain mutation runs under the
 * head-holder's lock (MW LOCK_FINE Step A -- one writer per chain), so the
 * neighbour-mid-deletion load-validate and the marked-target -ENOENT/-EAGAIN
 * bails in insert_after/del/replace are dead and gone.  Two things remain
 * from that header: the forward slot as the sole serializer, and the
 * "next"-only mark.  The mark stays because it has two
 * jobs.  It is the reader-visible logical-delete (readers mask it).  It is
 * also the freeze half that the head ops fold into their structural
 * flip-txn (ft_hlist_freeze_prepare).  The MCAS engine (proxy on bit 0) also
 * stays.  The chain edits still FOLD into the host op's flip-txn.  So a
 * chain edit and the trie<->head anchor edge commit atomically.  This does
 * NOT reshape the chain into the kernel hlist `**pprev' encoding.  It keeps
 * FT's duplicate chain EXACTLY as it is.  It only gives its link maintenance
 * a clean abstraction.
 *
 * Representation (struct cds_ft_node, unchanged, see <urcu/fractal-trie.h>):
 *   next : cds_ft_node *   forward link; reader-visible; MARK-able
 *                          (CDS_FT_NODE_REMOVED_FLAG on bit 1); the engine proxy
 *                          rides bit 0.  Transacted under FT_HLIST_TAG.
 *   prev : cds_ft_node *   back link to the PREDECESSOR NODE (an interior
 *                          duplicate's predecessor).  Transacted too (the
 *                          coherent-both-directions edge); under the single
 *                          writer a del's re-read of the predecessor is stable.
 *
 * Why prev is a node and not a pprev.  This is the FT-specific reason a
 * generic reuse fails.  FT overloads cds_ft_node.prev:
 *   - for a chain HEAD it is the cell/parent (the holder route, dereferenced
 *     as prev->parent);
 *   - for a duplicate it is the predecessor node.
 * ft_node_external(prev) decides headness: a duplicate's prev is an external
 * node, a head's prev is not.  The kernel hlist's pprev would make a
 * duplicate's prev a raw slot address (&pred->next).  That address is not an
 * external node.  It would break headness detection and the holder route
 * across ~20 readers.  Keeping prev a node leaves all of that untouched.  The
 * primitives just compute the forward slot as &pred->next when they need it.
 *
 * Head boundary (the "Option B" cut).  These ops own strictly the INTERIOR of a
 * chain: the head node's `next' inward, H -> D1 -> ... -> tail.  The
 * trie<->head anchor slot (external_nodes) is a STRUCTURAL trie slot.  It
 * uses FT_FLIP_PROXY_TAG and the FT descent resolver.  Holder recompaction
 * relocates it and external-promote flips it.  FT structural code manages
 * it, never these ops.  A duplicate is inserted after the head node H (on
 * H->next), never "at head".  The head is removed structurally.  So these
 * ops never touch external_nodes.
 * The structural head-remove must uphold one interop invariant: it records
 * MARK(H->next) in its own commit.  A concurrent insert_after(H) onto a
 * sole-node chain then sees the mark and aborts.  Interior del/insert already
 * give one another the same guard.
 *
 * Bits:
 *   - a live "next" carries the deletion MARK (bit 1) and the engine proxy
 *     TAG (bit 0);
 *   - a transacted "prev" carries only the proxy (never marked).
 * cds_ft_node is naturally (pointer) aligned.  So (value & TAG) != TAG for
 * every live next/prev/NULL.  FT's writer-side headness/holder logic reads
 * prev with a plain (unresolved) load.  This is correct under the current
 * retained caller exclusion (no proxy at rest).  Those reads gain a resolve
 * when concurrent writers are enabled.
 *
 * Only the composable *_prepare primitives are provided: insert-after, del
 * and replace.  FT folds each into the surrounding op's MCAS txn.  The
 * ordered-cell list folds its splices the same way, via
 * urcu_txn_list_*_prepare.  So FT never needs a self-contained _rcu bracket,
 * and it never head-inserts a duplicate.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <urcu/compiler.h>
#include <urcu/uatomic.h>
#include <urcu/rcu-txn-mcas.h>
#include <urcu/rcu-txn.h>
#include <urcu-pointer.h>

#include <urcu/fractal-trie.h>	/* struct cds_ft_node, CDS_FT_NODE_REMOVED_FLAG */

/*
 * Engine proxy tag for every interior chain slot (a node next/prev).  The
 * interior chain holds only duplicate leaves.  It never holds the structural
 * children that carry FT_FLIP_PROXY_TAG.  So the plain bit-0 engine tag is
 * enough.  In one TU it coexists with the ordered-cell list's URCU_TXN_TAG
 * (the slots never overlap) and with the trie's FT_FLIP_PROXY_TAG structural
 * edges.
 */
#define FT_HLIST_TAG	URCU_TXN_TAG

/* Logical-deletion mark: the public cds_ft_node removal tombstone (bit 1). */
#define FT_HLIST_MARK	CDS_FT_NODE_REMOVED_FLAG
/*
 * ☠ A prev WORD IS READ BY READERS.  A record parked on it must not be
 * bit-identical to a value the word can legitimately hold.
 *
 * Every up-walk from an external loads prev through
 * ft_dereference_prev_resolved.  The up-walks are the skip re-anchor,
 * ft_skip_to_compressed and ft_get_parent_rcu.  That helper resolves the FT
 * flip-proxy tag (low nibble 0xF) and nothing else.  In list mode a HEAD's
 * prev is an ordinal cell pointer tagged FT_ORD_CELL_TAG.  That tag is BIT 0,
 * the same bit as URCU_TXN_TAG.
 *
 * So a reader hopping a duplicate chain would read a prev record with the
 * hlist tag as a cell.  A member's prev is its predecessor, a raw external
 * pointer.  The hop stops at the first non-external value and takes it for
 * the head's word.
 *
 * So prev-word records carry the flip-proxy tag.  No prev value has that
 * tag, because nodes and cells are 16-byte aligned.  The reader's one
 * resolver covers these records.  next-word records keep the hlist tag: next
 * is read through ft_hlist_resolve.
 */
#define FT_HLIST_PREV_TAG	FT_FLIP_PROXY_TAG

/*
 * Worst-case MCAS edge counts, for the caller's txn reservation:
 *   - A tail append (succ == NULL) records only the single pos->next edge.
 *   - A mid-chain insert also records the succ->next load-validate guard and
 *     the succ->prev back-edge.
 *   - del and replace touch elem->next (mark), pred->next and next->prev,
 *     plus the next->next guard.
 *   - A freeze records only the elem->next mark (one edge).  It is folded
 *     into a host op's structural flip-txn.  A chain head leaves through its
 *     FT-structural anchor, not through a predecessor->next store.
 */
#define FT_HLIST_INSERT_AFTER_MAX_EDGES	3
#define FT_HLIST_DEL_MAX_EDGES		4
#define FT_HLIST_REPLACE_MAX_EDGES	4
#define FT_HLIST_FREEZE_MAX_EDGES	1

/*
 * Every chain edge below goes through here.  There is one reason beyond the
 * store.  These records carry no back-pointer to the flip-txn they fold into.
 * So the per-creation-site table in ft-txn-kind-stats.h cannot attribute
 * them.  They are counted here instead, as one global class.  One class is
 * all they need: a chain edge is MW because the chain is not covered by the
 * structural node locks.
 *
 * ☞ This comment used to say "MW ON PURPOSE ... not part of the
 * conservative-MW conversion".  That is no longer true.
 * cds_ft_node.next/.prev are [debt] in the word-kind table: they have a named
 * owner and are bound for SW under the nearest ancestor lock.
 * ft_hlist_store_sw below is the first site to make the trip.  The counter is
 * split MW/SW for the same reason.  Without the split, a conversion would
 * read as the site going quiet.
 */
/*
 * The duplicate-chain hold audit lives in ft-mutation-helpers.h.  It needs
 * the per-thread hold ledger, which is defined there, and this header is
 * included well before it.  Declare the coarse arm so that
 * ft_hlist_store_chain_at can report.  This is one TU, so the later
 * definition resolves it.
 */
static inline
int ft_hlist_store_sw_at(const char *fn, int line, const struct cds_ft *ft,
		struct ft_txn *txn, void **slot, void *old_ptr, void *new_ptr,
		uintptr_t tag);

/*
 * Did the coarse dispatch above actually FIRE?  A green run where it never
 * ran looks the same as a run where it is wrong.  So count both sides.
 */
#ifdef FT_DEBUG_DUAL_SITE
unsigned long ft_hlist_coarse[2];
static void ft_hlist_coarse_report(void) __attribute__((destructor));
static void ft_hlist_coarse_report(void)
{
	fprintf(stderr, "FT HLIST KIND coarse_sw=%lu fine_mw=%lu\n",
		uatomic_read(&ft_hlist_coarse[1]),
		uatomic_read(&ft_hlist_coarse[0]));
}
# define FT_HLIST_COARSE_TALLY(c)	uatomic_inc(&ft_hlist_coarse[(c)])
#else
# define FT_HLIST_COARSE_TALLY(c)	do { (void) (c); } while (0)
#endif


#ifdef FT_DEBUG_CHAIN_HOLD
static inline void ft_ch_audit_coarse_at(const char *fn, int line);
# define FT_CH_COARSE(fn, line)	ft_ch_audit_coarse_at((fn), (line))
#else
# define FT_CH_COARSE(fn, line)	do { (void) (fn); (void) (line); } while (0)
#endif

/*
 * ☞ THE DUPLICATE-CHAIN CANARY  (-DFT_DEBUG_CHAIN_CANARY)
 *
 * The governing rule for this class is "the duplicate chain is protected by the
 * NEAREST ANCESTOR LOCK, period".  The library cannot tell from inside whether
 * a given op HELD that lock.  There are three witnesses: the txn registry,
 * the per-thread hold ledger and the lock ctx.  Each sees a different subset,
 * and the narrow ones produce WRONG ZEROS.  The chain hold audit measures
 * ft_detach_node at REG=2,579,201 / LED=0.  A ledger-only verdict calls
 * every one of those holds a violation.
 *
 * So stop interrogating the CONSUMER and tag the PRODUCER.  For each chain
 * WORD, record the (fn, line) of the site that last stored it.  A reader may
 * then find a value it did not expect.  Example: the tail of
 * ft_hlist_freeze_chain_prepare finds an appended duplicate where it derived
 * NULL.  The word then names the site that put the value there.  No witness,
 * no inference.
 *
 * ☠ KEYED ON THE SLOT, NOT ON A FIELD INSIDE THE NODE.  That is not a
 * stylistic choice.  The first spelling put the tag in struct cds_ft_node and
 * recovered the node from the slot with caa_container_of.  It corrupted the
 * heap within seven tests ("corrupted size vs. prev_size").  The cause: a
 * chain store's slot is not always inside a live cds_ft_node.
 * ft_hlist_del_prepare derives @pred by LOADING @elem->prev.  With a stale
 * or head @pred, &pred->next points into something else entirely.  The
 * existing code survives that because the store is RECORDED: the
 * expected-old check rejects it at commit.  An immediate debug write has no
 * such protection.  (☞ That is worth remembering on its own: the MW CAS is
 * load-bearing for more than arbitration here.)  A side table keyed by
 * address writes only its own memory.  It can tag any word, node or not.
 *
 * Lossy by construction: one open-addressed probe, last writer wins, no
 * locking.  An entry is a name for a human to read after the fact.  Nothing
 * branches on it.  So a collision costs a misattributed row, never
 * correctness.
 *
 * A build that defines the flag pays 32 MiB of BSS.  A build that does not
 * pays nothing at all: the stamp compiles to `do { } while (0)`.
 */
#ifdef FT_DEBUG_CHAIN_CANARY
#define FT_CANARY_SLOTS		(1u << 20)
struct ft_canary_ent {
	void **slot;
	const char *fn;
	int line;
	bool raw;		/* last writer bypassed the record layer */
	unsigned char raw_id;	/* which raw producer, when @raw */
	unsigned long tid;
};
extern struct ft_canary_ent ft_canary_tab[FT_CANARY_SLOTS];
struct ft_canary_ent ft_canary_tab[FT_CANARY_SLOTS];

static inline
unsigned int ft_canary_hash(const void *slot)
{
	uintptr_t v = (uintptr_t) slot;

	v ^= v >> 33;
	v *= 0xff51afd7ed558ccdULL;
	v ^= v >> 33;
	return (unsigned int) (v & (FT_CANARY_SLOTS - 1));
}

/*
 * ☞ THE SW-vs-RAW SLOT MIX.  This is the question the flip actually turns on.
 *
 * rcu-txn.h's rule is "a slot is SW xor MW, GLOBALLY".  A RAW store is one
 * that never reaches the engine.  It is outside that rule entirely, because
 * it cannot be parked.  Suppose the same SLOT is also written through the
 * record layer.  The day that layer parks SW, the raw store and the settle
 * race on one word.  Reading the call graph cannot answer this, because a
 * node's role changes.  The same cds_ft_node.prev is a HEAD's back edge
 * (written raw) at one moment.  At another moment it is a chain MEMBER's
 * prev (recorded).
 *
 * The canary is keyed BY SLOT, so it can answer directly.  Tag each entry
 * with whether its last writer was raw, and count the transitions.  A
 * non-zero count in either direction means one address is written both ways:
 * the flip's hazard is LIVE.  Zero means the two populations are disjoint.
 *
 * ☠ Counted only on a canary HIT (e->slot == slot).  The table is lossy by
 * design: one open-addressed probe, last writer wins.  So a miss is an
 * evicted entry, not a transition.  Scoring it would manufacture a mix.
 */
/*
 * ☞ SPLIT BY PRODUCER.  "Is one slot written both ways" and "does that
 * MATTER" are different questions.  Only the second decides the flip.  Each
 * raw producer has a very different exposure, already measured by the
 * head-word audit.  Mathieu's question was: are these raw stores on HIDDEN
 * nodes?  Attributing a transition to one producer answers that question
 * instead of inferring the answer:
 *
 *   0 MARK_REMOVED  ft_node_mark_removed_flip, &node->next on a LIVE node it is
 *                   tombstoning.  NOT hidden.  It is the only one that is not,
 *                   and it is proxy-aware (it spins out a parked FT_HLIST_TAG
 *                   rather than trampling it).
 *   1 PUB_EXT_PREV  ft_publish_external_nodes_prev, &head->prev list-off.
 *                   Audited 33,667/33,667 under the FT-wide writer lock.
 *   2 SET_PARENT    ft_set_parent's external arm.  Its callers' audit rows read
 *                   HIDDEN with LIVE=0 (ft_attach_node 1.5M, the splits, the
 *                   recompact), i.e. FRESH nodes no reader can reach.
 *   3 SET_PARENT_RAW  ft_set_parent_raw's external arm.  Audited 25,021/25,195
 *                   wlock-held, 174 NOHOLDER.
 */
#define FT_CANARY_RAW_NR	4
extern unsigned long ft_canary_mix_raw_after_rec[FT_CANARY_RAW_NR],
	ft_canary_mix_rec_after_raw[FT_CANARY_RAW_NR],
	ft_canary_raw_stores[FT_CANARY_RAW_NR], ft_canary_rec_stores;
unsigned long ft_canary_mix_raw_after_rec[FT_CANARY_RAW_NR],
	ft_canary_mix_rec_after_raw[FT_CANARY_RAW_NR],
	ft_canary_raw_stores[FT_CANARY_RAW_NR], ft_canary_rec_stores;

static inline
void ft_chain_canary_stamp_id(const char *fn, int line, void **slot, bool raw,
		unsigned char id)
{
	struct ft_canary_ent *e = &ft_canary_tab[ft_canary_hash(slot)];

	if (raw)
		uatomic_inc(&ft_canary_raw_stores[id]);
	else
		uatomic_inc(&ft_canary_rec_stores);
	if (e->slot == slot && e->raw != raw) {
		if (raw)
			uatomic_inc(&ft_canary_mix_raw_after_rec[id]);
		else
			uatomic_inc(&ft_canary_mix_rec_after_raw[e->raw_id]);
	}
	e->slot = slot;
	e->fn = fn;
	e->line = line;
	e->raw = raw;
	e->raw_id = raw ? id : e->raw_id;
	e->tid = (unsigned long) pthread_self();
}

static inline
void ft_chain_canary_stamp_at(const char *fn, int line, void **slot, bool raw)
{
	ft_chain_canary_stamp_id(fn, line, slot, raw, 0);
}

static inline
void ft_chain_canary_stamp(const char *fn, int line, void **slot)
{
	ft_chain_canary_stamp_at(fn, line, slot, false);
}

static void ft_canary_mix_report(void) __attribute__((destructor));
static void ft_canary_mix_report(void)
{
	{
		static const char * const nm[FT_CANARY_RAW_NR] = {
			"MARK_REMOVED(live node)", "PUB_EXT_PREV(wlock)",
			"SET_PARENT(hidden)", "SET_PARENT_RAW(wlock)" };
		unsigned int i;

		fprintf(stderr, "FT CHAIN SLOT-MIX by raw producer "
			"(recorded=%lu):\n",
			uatomic_read(&ft_canary_rec_stores));
		for (i = 0; i < FT_CANARY_RAW_NR; i++)
			fprintf(stderr, "  %-26s raw=%-10lu "
				"raw_after_rec=%-8lu rec_after_raw=%lu\n",
				nm[i], uatomic_read(&ft_canary_raw_stores[i]),
				uatomic_read(&ft_canary_mix_raw_after_rec[i]),
				uatomic_read(&ft_canary_mix_rec_after_raw[i]));
	}
}

/* NULL when the word has never been stored through a chain store helper. */
static inline
const struct ft_canary_ent *ft_chain_canary_of(void **slot)
{
	const struct ft_canary_ent *e = &ft_canary_tab[ft_canary_hash(slot)];

	return e->slot == slot ? e : NULL;
}

# define FT_CHAIN_CANARY_STAMP(fn, line, slot)				\
	ft_chain_canary_stamp((fn), (line), (void **) (slot))
/* FT_CHAIN_CANARY_RAW is declared in ft-helpers.h: the raw producers live in
 * files this one is included AFTER, so the macro has to exist before here. */
#else
# define FT_CHAIN_CANARY_STAMP(fn, line, slot)		do { } while (0)
#endif

static inline
int ft_hlist_store_chain_at(const char *fn, int line, const struct cds_ft *ft,
		struct ft_txn *txn,
		void **slot, void *old_ptr, void *new_ptr, uintptr_t tag)
{
	/*
	 * ☑ THIS CLASS PARKS.  cds_ft_node.next / .prev are SW.
	 *
	 * It is the largest conversion in the tree: 14.5M stores per ft_inv
	 * leg.  That is an order of magnitude above every row of the §11.6
	 * residue.  It is ~4.5x the ordered-cell list it used to be counted
	 * inside.  It was ATTEMPTED AND WITHDRAWN TWICE before this.  Both
	 * attempts gated GREEN.  That is the single most important thing to
	 * know before touching any of it.  On this surface, an instrument that
	 * could not see has twice produced a green gate.  So the conversion
	 * rests on the six refutations below being PAID, each by a
	 * measurement.  The gate is only the last check, not the argument.
	 *
	 *   1 KIND PER SLOT, NOT PER TXN.  The dispatch is unconditional.  The
	 *     2026-09-17 attempt asked ft_flip_txn_owns per prepare.  The three
	 *     point ops take the holder into a STACK ft_held_anchor BEFORE the
	 *     content txn exists.  Their registry was empty, so they could only
	 *     answer MW, while the structural sites answered SW.  That is one
	 *     slot with two kinds, by construction.  It breaks rcu-txn.h's "a
	 *     slot is SW xor MW, GLOBALLY".  The engine's own kind-conflict
	 *     assert is silent under --enable-rcu-debug across six legs.
	 *
	 *   2 THE TREE'S GATE WAS A COUNTER READING ZERO, never "this is safe".
	 *     Every converted freeze site's plan-stale counter reads 0.  The
	 *     two sites that are not converted are measured UNREACHED (0 of 0).
	 *     That is a coverage statement and is labelled as one.
	 *
	 *   3 AN SW ABORT WRITES.  urcu_txn_settle stores old_ptr back BLIND.
	 *     So an expected-old that is a caller's BELIEF becomes a published
	 *     lie.  FT_CHAIN_OLD converts the five of them to loads.
	 *
	 *   4 THE SERIALIZING NULL ON THE APPEND SIDE (ft_hlist_append_run_
	 *     prepare) was missed entirely last time.  It had ZERO concurrent
	 *     coverage until a colliding-key merge test was written for it.  It
	 *     now runs ~19k times per leg with 0 stale plans.  Every run is
	 *     inside a bulk window with the FT-wide lock held.
	 *
	 *   5 A RE-READ UNDER THE LOCK MUST NOT BE READ-YOUR-OWN-WRITES.
	 *     ft_hlist_chain_plan_ok uses ft_txn_load_committed.  The
	 *     previous attempt's re-read used urcu_txn_load.  In a fused commit
	 *     it read the txn's own pending value, i.e. nothing.
	 *
	 *   6 A RAW STORE CANNOT BE PARKED.  The slot mix was measured by
	 *     producer.  Every transition comes from writes to HIDDEN nodes (a
	 *     fresh node's prev, written before publication).  The one raw
	 *     producer that touches a LIVE chain word is measured unreachable.
	 *     ☠ That is a coverage statement.  If the producer ever becomes
	 *     reachable it must be converted to a recorded store, NOT
	 *     re-argued.  See ft_node_mark_removed_flip's header for the exact
	 *     ordering hazard.
	 *
	 * ☞ The ordinal CELL list (ft_ord_cell.lnode) does not come through
	 * this function at all.
	 *
	 * ☞ COARSE RECORDS SW, WHATEVER THE WORD IS.  A coarse trie takes the
	 * FT-wide @writer_lock at its outermost writer scope.  So every writer
	 * of every chain and cell word is serialised behind one mutex.  There
	 * is no peer CAS for an SW park to race.  That arm predates the flip
	 * and is unchanged by it.
	 */
	if (ft && !ft->lock_fine) {
		FT_HLIST_COARSE_TALLY(1);
		return ft_hlist_store_sw_at(fn, line, ft, txn, slot,
			old_ptr, new_ptr, tag);
	}
	FT_HLIST_COARSE_TALLY(0);
	/*
	 * ☑ THE COUNTER SAYS "CHAIN", BECAUSE THIS IS NOT A CELL.  Every store
	 * here writes a DUPLICATE-CHAIN word (cds_ft_node.next/.prev).  The
	 * ordinal CELL list (ft_ord_cell.lnode) does not come through this
	 * function.
	 *
	 * ☠ AND THE TAG CANNOT DO THIS JOB.  FT_HLIST_TAG *IS* URCU_TXN_TAG
	 * (1), the ordered-cell tag.  So a tag test classes a chain word as a
	 * cell.  The PRODUCER is what names the class.
	 *
	 * @fn/@line are the CALLER's, so every chain-word store gets its own
	 * audit row.  That is the whole point, since the question is per SITE.
	 */
	FT_CH_COARSE(fn, line);
	FT_CHAIN_CANARY_STAMP(fn, line, slot);
	/*
	 * ★ THE FLIP.  It is unconditional, and that is the
	 * point.  The 2026-09-17 attempt dispatched on a per-TXN predicate.
	 * That made the kind a property of the ASKING TRANSACTION rather than
	 * of the SLOT.  The three point ops take the holder into a STACK
	 * ft_held_anchor before the content txn exists.  So their registry is
	 * empty and they could only ever answer MW, while the structural
	 * sites answered SW.  That is one slot with two kinds, by
	 * construction.  It breaks rcu-txn.h's "a slot is SW xor MW,
	 * GLOBALLY".  There is no predicate here.
	 *
	 * What the six refutations needed.  Each is now discharged by a
	 * measurement rather than by an argument:
	 *   1 kind per slot ....... no predicate (this arm)
	 *   2 the tree's gate ..... plan-stale counters read 0 at every
	 *                           converted site; the two unconverted ones
	 *                           are measured UNREACHED, not argued safe
	 *   3 SW abort WRITES ..... the five belief expected-olds become loads
	 *                           with FT_CHAIN_OLD
	 *   4 append_run missed ... covered at last by the colliding-key merge
	 *                           test, 0 stale of ~19k concurrent events
	 *   5 the re-read was RYW .. ft_hlist_chain_plan_ok uses
	 *                           ft_txn_load_committed
	 *   6 raw producers ....... the slot mix is ALL hidden-node writes; the
	 *                           one raw producer on a live chain word is
	 *                           measured unreachable (see
	 *                           ft_node_mark_removed_flip's header)
	 */
	FT_TK_COUNT_CHAIN_SW();
	return ft_txn_record(txn, slot, old_ptr, new_ptr, tag) ? 0 : -ENOMEM;
}

#define ft_hlist_store_chain(ft, txn, slot, old_ptr, new_ptr, tag)	\
	ft_hlist_store_chain_at(__func__, __LINE__, (ft), (txn), (slot),\
		(old_ptr), (new_ptr), (tag))

/*
 * ☞ AN EXPECTED-OLD MUST DESCRIBE THE SLOT ONCE THE RECORD PARKS.
 *
 * Five chain stores pass an expected-old that the caller BELIEVES.  It is an
 * ARGUMENT, not a value read from the word:
 *   - succ->prev <- pos;
 *   - pred->next <- elem and next->prev <- elem, in the unlink;
 *   - the two in replace.
 * Under MW that is harmless and slightly stronger than a load: a wrong
 * belief just fails the install CAS.  ☠ Under SW it is a WRITE.
 * urcu_txn_settle stores old_ptr back BLIND on an abort.  So a belief that
 * never matched the word is published into a live slot.  That was refutation
 * #3 of the 2026-09-17 attempt.
 *
 * MEASURED with -DFT_DEBUG_HLIST_EXPECTED_OLD: these five disagree with the
 * word ZERO times over both suites and all three spacings.  So the
 * conversion is value-neutral.  Under SW, a belief would be a blind write.
 *
 * ☠ ft_txn_load, not _load_committed: this IS the read-your-own-writes case
 * the engine documents.  A fused commit chains several edges on one chain.
 * The expected-old owed here is the value the slot will hold at install time
 * FROM THIS TXN'S POINT OF VIEW.  When the txn already wrote that word, that
 * value is its own pending write, not the pre-commit value.  (The plan check
 * in ft_hlist_chain_plan_ok is the opposite case.  It uses _committed for
 * exactly the opposite reason.)
 */
#define FT_CHAIN_OLD(txn, slot, believed, tag)				\
	ft_txn_load((txn), (void **) (slot), (tag))


/*
 * THE SW TWIN.  It is for a chain word whose exclusion is ESTABLISHED, not
 * merely arbitrated by the expected-old.
 *
 * rcu-txn.h states the rule this has to answer: "a slot is SW xor MW,
 * GLOBALLY.  If any other transaction may store_mw() the same slot, this park
 * races that CAS."  So a site may spell itself SW only where no peer writer of
 * that slot can be running.  That is a claim about the OP's exclusion, not
 * about this word.  ft_glue_record_splices is the first such site.  It
 * writes a head's prev inside a BULK WINDOW.  There the FT-wide writer lock
 * is held, and the gate has flipped every point op onto that same lock.
 * Measured: 5401 of 5401 stores wlock-held and inside a bulk body, 0 drain
 * seams in-window.
 *
 * ☑ AND THE WHOLE-CLASS STEP HAS NOW HAPPENED.  This header used to forbid
 * copying the spelling to a point-op chain site.  Converting ONE site while
 * its peers stayed MW is exactly the SW-park-races-an-MW-CAS that the rule
 * above describes.  The duplicate chain migrated as a CLASS instead
 * (ft_hlist_store_chain_at), so every writer of a chain word now parks.
 * This function remains only as the direct spelling for one kind of site.
 * That site is excluded by the FT-wide writer lock, not by a node lock.
 */
static inline
int ft_hlist_store_sw_at(const char *fn, int line, const struct cds_ft *ft,
		struct ft_txn *txn,
		void **slot, void *old_ptr, void *new_ptr, uintptr_t tag)
{
	FT_TK_COUNT_CHAIN_SW();
	FT_CH_COARSE(fn, line);
	FT_CHAIN_CANARY_STAMP(fn, line, slot);
	return ft_txn_record(txn, slot, old_ptr, new_ptr, tag) ? 0 : -ENOMEM;
}

#define ft_hlist_store_sw(ft, txn, slot, old_ptr, new_ptr, tag)		\
	ft_hlist_store_sw_at(__func__, __LINE__, (ft), (txn), (slot),	\
		(old_ptr), (new_ptr), (tag))

static inline
void *ft_hlist_set_mark(struct cds_ft_node *n)
{
	return (void *) ((uintptr_t) n | FT_HLIST_MARK);
}

static inline
struct cds_ft_node *ft_hlist_unmark(void *v)
{
	return (struct cds_ft_node *) ((uintptr_t) v & ~(uintptr_t) FT_HLIST_MARK);
}

/*
 * Resolve a raw "next" slot value: strip the engine proxy, then the mark.
 * Fast path: a clean value is returned untouched.  A clean value is neither
 * a proxy under FT_HLIST_TAG nor MARK-ed.  So a live-node traversal never
 * runs the unmark AND, and the pointer stays out of the load-to-use
 * dependency chain.  Only a tagged value takes the slow path: an in-flight
 * proxy, or a ghost's marked "next".  prev is
 * writer-only, so it has no reader-side resolve.
 */
static inline
struct cds_ft_node *ft_hlist_resolve(void *raw)
{
	uintptr_t v = (uintptr_t) raw;

	if (caa_unlikely(v & (FT_HLIST_TAG | FT_HLIST_MARK)))
		return ft_hlist_unmark(ft_txn_resolve(raw, FT_HLIST_TAG));
	return (struct cds_ft_node *) raw;
}

/* Resolved forward step (call within an RCU read-side section). */
static inline
struct cds_ft_node *ft_hlist_next_rcu(struct cds_ft_node *node)
{
	return ft_hlist_resolve((void *) rcu_dereference(node->next));
}

#ifdef FT_DEBUG_MARK_REFUSAL
/*
 * TEMPORARY (validated-set probe): which test rows actually REACH the two
 * FT_HLIST_MARK refusals?  A row with a non-zero count here must be in the
 * validated set for this site.  A row reading 0 proves nothing about the
 * refusal and does not belong in the set.
 */
static unsigned long ft_mark_refuse_ins, ft_mark_refuse_del;
# define FT_MARK_REFUSE_TALLY(which)	do {				\
		if ((which) == 0) __atomic_fetch_add(&ft_mark_refuse_ins,\
				1, __ATOMIC_RELAXED);			\
		else __atomic_fetch_add(&ft_mark_refuse_del, 1,		\
				__ATOMIC_RELAXED);			\
	} while (0)
static void ft_mark_refuse_report(void) __attribute__((destructor));
static void ft_mark_refuse_report(void)
{
	fprintf(stderr, "FT_MARK_REFUSE  insert_after=%lu  del=%lu\n",
		ft_mark_refuse_ins, ft_mark_refuse_del);
}
#else
# define FT_MARK_REFUSE_TALLY(which)	do { } while (0)
#endif

/*
 * ft_hlist_insert_after_prepare: record an insert of @newp immediately after
 * @pos, WITHOUT committing.
 *
 * @pos is the predecessor NODE: the head node H for a first duplicate, or an
 * interior duplicate.  The forward slot &pos->next changes from its current
 * successor @succ to @newp.  @newp is built invisibly (next = @succ, prev =
 * @pos).  Two edges are recorded:
 *   pos->next: succ -> newp
 *   succ->prev: pos -> newp
 * A tail append (@pos == the walked tail, @succ == NULL) is a 1-edge insert
 * with no backward fixup.  This is FT's ft_chain_node idiom.  OOM is sticky
 * to the commit.
 *
 * Single-writer per chain (MW LOCK_FINE Step A).  Every chain mutation runs
 * under the head-holder's node lock, or under the FT-wide lock before that
 * lock drops.  A disjoint-key optimistic writer owns its own chain.  So @pos
 * is never concurrently deleted and @succ is never a neighbour mid-deletion.
 * Those cases needed this multi-writer arbitration:
 *   - bail -ENOENT on a marked @pos;
 *   - load-validate &succ->next;
 *   - retry -EAGAIN on a marked neighbour.
 * That arbitration is dead and dropped.
 *
 * ☠ DEFECT FT-SLOT-2: THAT DROP CONTRADICTS THE INTEROP INVARIANT THIS
 * FILE'S HEADER STATES.
 * The header promises this: a structural head-remove's MARK(H->next) is what
 * makes "a concurrent insert_after(H) onto a sole-node chain see the mark and
 * abort".  The check below was deleted, so for a while nothing looked at the
 * mark.  The remove side upholds its half: ft_hlist_freeze_sole_prepare
 * marks a derived NULL.  The insert side no longer checks.  A MARK(NULL)
 * @pos->next reads back as the bare value 2.  That value passes
 * `succ != NULL` and makes the second store record slot
 * &((struct cds_ft_node *) 2)->prev.  Under LOCK_FINE, insert and remove are
 * concurrent on the same key by contract.  The append's head is derived
 * before the holder acquire, so this is reachable in principle.  It is the
 * mirror of the remove-side routing defect.
 * ☑ IT IS NO LONGER "UNPROVEN BY TEST".  The shape is reachable and
 * inv_concurrent_insert_replace_nolist reaches it.  The check is RESTORED
 * below as a refusal.
 *
 * Return 0, or -ENOENT when @pos is a retired head.  The int return was
 * retained for exactly this.
 */
static inline
int ft_hlist_insert_after_prepare(const struct cds_ft *ft, struct ft_txn *txn,
		struct cds_ft_node *newp,
		struct cds_ft_node *pos)
{
	struct cds_ft_node *succ = (struct cds_ft_node *)
			ft_txn_load(txn, (void **) &pos->next, FT_HLIST_TAG);

	/*
	 * ☑ THE MARK CHECK IS BACK.  It is a REFUSAL now, not an assert.
	 *
	 * @pos->next MARKED means @pos is a RETIRED head.  This append is
	 * building onto a chain that no longer exists.  Left unchecked it is a
	 * LOST INSERT.  MARK(NULL) reads back as the bare value 2 and passes
	 * `succ != NULL` below.  The second store then records slot
	 * &((struct cds_ft_node *) 2)->prev.  This file's header states the
	 * interop invariant in as many words.  The structural head-remove
	 * marks H->next in its own commit "so a concurrent insert_after(H)
	 * onto a sole-node chain sees the mark and ABORTS".  Seeing it is this
	 * line.
	 *
	 * ☠ IT WAS UNPROVEN AND IS NOW PROVEN.  The obituary above asked for
	 * "either restoring the check or proving the shape unreachable".  The
	 * shape is reachable, and cds_ft_insert_replace is the producer.  It
	 * displaces a whole chain, and it now TOMBSTONES what it displaces.
	 * Its sibling always did so for the single node it displaces.  An
	 * append that derived @pos before the displacement therefore lands
	 * here every time.  Reached by inv_concurrent_insert_replace_nolist.
	 *
	 * -ENOENT is the caller's documented code for "tail already marked".
	 * The caller maps the refusal to -EAGAIN and re-descends from the
	 * root.  That is the correct answer: the chain this append aimed at is
	 * gone, so the position must be derived again.
	 */
	if (caa_unlikely((uintptr_t) succ & FT_HLIST_MARK)) {
		FT_MARK_REFUSE_TALLY(0);
		return -ENOENT;
	}

	/* Build the fresh node invisibly. */
	newp->next = succ;
	newp->prev = pos;

	/* pos->next: succ -> newp ; succ->prev: pos -> newp. */
	ft_hlist_store_chain(ft, txn, (void **) &pos->next, succ, newp, FT_HLIST_TAG);
	if (succ != NULL)
		ft_hlist_store_chain(ft, txn, (void **) &succ->prev,
			FT_CHAIN_OLD(txn, &succ->prev, pos, FT_HLIST_PREV_TAG),
			newp, FT_HLIST_PREV_TAG);
	return 0;
}

/*
 * ft_hlist_append_run_prepare: record the append of a whole null-terminated RUN
 * at a chain @tail, WITHOUT committing.  The caller walked the chain and
 * found tail->next == NULL.  Unlike ft_hlist_insert_after_prepare this does
 * NOT touch @run_head->next.  So @run_head's own chain (run_head->next ->
 * ...) rides along unmodified.  This is a run-splice, not a single insert: a
 * merge concatenating a src duplicate run onto a dst tail.
 * The forward link tail->next: NULL -> run_head
 * is the lone recorded edge, and it is the serializing one.  Its CAS old is
 * NULL, so a concurrent freeze of the tail fails this commit.  So only the
 * tail carries a proxy while the commit is in flight.  The interior of
 * neither run is disturbed.
 *
 * ★ @run_head->prev IS RECORDED BY THE CALLER, not stored here.  It used to
 * be a plain store.  The claim was: "writer-only; readers never read prev, and
 * @run_head is unreachable to readers because a merge detaches and drains the
 * src side before appending it".  That was true of ft_merge_spine_copy, the
 * only caller when it was written.  It is FALSE for the one-decide fold.
 * The fold records the src detach and this splice into ONE txn, with the src
 * side still LIVE.
 * And prev IS read.  Every up-walk from an external starts at it.  A reader
 * on the retired copy of the src junction follows that copy's stale skip
 * word straight to @run_head.  A plain store there was the one torn state
 * the reader's two-descent witness cannot repair: BOTH descents see the same
 * half-moved head before the flip.  It also survived an abort, which the
 * caller had to undo by hand.  The store is now recorded with
 * FT_HLIST_PREV_TAG, beside the forward link, in ft_glue_record_splices.  It
 * flips atomically with the rest of the move and is discarded with an abort.
 * The reservation is two records per splice.
 */
static inline
void ft_hlist_append_run_prepare(const struct cds_ft *ft, struct ft_txn *txn,
		struct cds_ft_node *tail,
		struct cds_ft_node *run_head)
{
	int ret;

	ret = ft_hlist_store_chain(ft, txn, (void **) &tail->next, NULL, run_head,
			FT_HLIST_TAG);
	assert(!ret);			/* caller reserved the edge up front */
	(void) ret;
}

/*
 * ft_hlist_del_prepare: record the unlink of @elem into @txn WITHOUT committing.
 * Three edges are recorded:
 *   - elem->next: next -> MARK(next).  This marks @elem (logical delete).
 *     The target is preserved, so a reader parked on @elem still follows the
 *     chain to the successor / end.
 *   - pred->next: elem -> next.  This unlinks @elem forward.
 *   - next->prev: elem -> pred.  This unlinks @elem backward.
 * On a committed OK, THIS call removed @elem.  Reclaim it after a grace
 * period.  OOM is sticky to the commit.
 *
 * Single-writer per chain (see ft_hlist_insert_after_prepare) covers the
 * NEIGHBOURS.  @next is never a neighbour mid-deletion.  @pred, as read in
 * this attempt, is stable: no peer re-links it.  So that half of the
 * multi-writer arbitration stays dropped: load-validate &next->next, and
 * retry -EAGAIN on a marked successor.
 *
 * ☠ IT DOES NOT COVER @elem ITSELF.  A structural head-remove marks a head it
 * retires from OUTSIDE this chain's holder.  cds_ft_insert_replace
 * tombstones the chain it displaces, so an already-marked @elem does arrive
 * here.  The -ENOENT refusal below is therefore LIVE, not dead.
 *
 * Return 0, or -ENOENT when @elem is already logically deleted.
 */
static inline
int ft_hlist_del_prepare(const struct cds_ft *ft, struct ft_txn *txn, struct cds_ft_node *elem)
{
	struct cds_ft_node *next = (struct cds_ft_node *)
			ft_txn_load(txn, (void **) &elem->next, FT_HLIST_TAG);
	struct cds_ft_node *pred = (struct cds_ft_node *)
			ft_txn_load(txn, (void **) &elem->prev, FT_HLIST_PREV_TAG);

	/*
	 * ☠ ALREADY MARKED means @elem is ALREADY logically deleted.  Building
	 * this delete on top of it is not merely redundant: it FAULTS.  A
	 * MARK(NULL) @elem->next reads back as the bare value 2.  That value
	 * passes the `next != NULL` test below.  The backward edge then
	 * records slot &((struct cds_ft_node *) 2)->prev.  The commit installs
	 * into address 2 and SEGVs.  This is byte-for-byte the failure that
	 * the mark check in ft_hlist_insert_after_prepare exists to stop, one
	 * function over.  It became reachable here for the same reason.
	 * Nothing used to tombstone a chain displaced by
	 * cds_ft_insert_replace, so a marked node could not arrive at a del.
	 * Now one can.
	 *
	 * Return -ENOENT.  The caller's comment already names this case
	 * ("@node or a NEIGHBOUR mid-deletion").  The caller destroys the txn
	 * and retries from a fresh position derivation.  There the wrapper's
	 * tombstone test answers NOT_FOUND.  Nothing is recorded, so nothing
	 * installs.
	 */
	if (caa_unlikely((uintptr_t) next & FT_HLIST_MARK)) {
		FT_MARK_REFUSE_TALLY(1);
		return -ENOENT;
	}

	/*
	 * Mark elem (logical delete), unlink forward (pred->next: elem -> next)
	 * and backward (next->prev: elem -> pred).  When next is NULL the backward
	 * edge vanishes: a 2-edge delete storing MARK(NULL).  The mark on
	 * &elem->next is retained.  Readers mask it (ft_hlist_resolve).  The
	 * head ops fold it (ft_hlist_freeze_prepare) for atomicity with the
	 * structural anchor edge.
	 */
	ft_hlist_store_chain(ft, txn, (void **) &elem->next, next,
			ft_hlist_set_mark(next), FT_HLIST_TAG);
	ft_hlist_store_chain(ft, txn, (void **) &pred->next,
		FT_CHAIN_OLD(txn, &pred->next, elem, FT_HLIST_TAG),
		next, FT_HLIST_TAG);
	if (next != NULL)
		ft_hlist_store_chain(ft, txn, (void **) &next->prev,
			FT_CHAIN_OLD(txn, &next->prev, elem, FT_HLIST_PREV_TAG),
			pred, FT_HLIST_PREV_TAG);
	return 0;
}

/*
 * ft_hlist_replace_prepare: record the in-place replacement of @old by @newp
 * into @txn WITHOUT committing.  @newp takes @old's position: pred->next and
 * next->prev swing to @newp.  @old is logically removed: its next is marked
 * exactly as del does.  Argument order is (old, new).  On commit reclaim
 * @old after a grace period.  OOM is sticky to the commit.  Used for a
 * non-head duplicate replace.  A head replace is FT-structural: it swaps the
 * anchor slot.
 *
 * Single-writer per chain (see ft_hlist_insert_after_prepare).  This
 * multi-writer arbitration is dead and dropped:
 *   - -ENOENT on a marked @old;
 *   - load-validate &next->next;
 *   - retry -EAGAIN on a marked successor.
 * Always returns 0.  The int is retained for caller-shape parity.
 */
static inline
int ft_hlist_replace_prepare(const struct cds_ft *ft, struct ft_txn *txn,
		struct cds_ft_node *old, struct cds_ft_node *newp)
{
	struct cds_ft_node *next = (struct cds_ft_node *)
			ft_txn_load(txn, (void **) &old->next, FT_HLIST_TAG);
	struct cds_ft_node *pred = (struct cds_ft_node *)
			ft_txn_load(txn, (void **) &old->prev, FT_HLIST_PREV_TAG);

	/* Build @newp's links invisibly, then swing pred->next and next->prev. */
	newp->next = next;
	newp->prev = pred;

	ft_hlist_store_chain(ft, txn, (void **) &old->next, next,
			ft_hlist_set_mark(next), FT_HLIST_TAG);
	ft_hlist_store_chain(ft, txn, (void **) &pred->next,
		FT_CHAIN_OLD(txn, &pred->next, old, FT_HLIST_TAG),
		newp, FT_HLIST_TAG);
	if (next != NULL)
		ft_hlist_store_chain(ft, txn, (void **) &next->prev,
			FT_CHAIN_OLD(txn, &next->prev, old, FT_HLIST_PREV_TAG),
			newp, FT_HLIST_PREV_TAG);
	return 0;
}

/*
 * ft_hlist_freeze_prepare: record ONLY the logical-deletion mark of @node's
 * forward slot (node->next: succ -> MARK(succ)) into @txn WITHOUT committing.
 * This is the freeze half of a del, with no chain unlink.
 *
 * A chain HEAD leaves the trie through its FT-structural anchor edge (the
 * parent slot re-point / clear).  It does not leave through a
 * predecessor->next store.  So only the mark rides the hlist.  Folding the
 * mark into the head op's structural flip-txn makes the freeze and the
 * anchor edge commit atomically.  A reader is never shown @node's head
 * anchor promoted away while @node is still unmarked.
 *
 * The target is preserved: MARK(succ), or MARK(NULL) for a head with no
 * successor.  So a reader parked on @node still follows the chain to the
 * promoted new head / end.  One edge is recorded.  The caller reserves
 * FT_HLIST_FREEZE_MAX_EDGES on top of the host op's footprint.
 */
static inline
void ft_hlist_freeze_prepare(const struct cds_ft *ft, struct ft_txn *txn, struct cds_ft_node *node)
{
	void *en = ft_txn_load(txn, (void **) &node->next, FT_HLIST_TAG);
	int ret;

	ret = ft_hlist_store_chain(ft, txn, (void **) &node->next, en,
			ft_hlist_set_mark((struct cds_ft_node *) en), FT_HLIST_TAG);
	assert(!ret);			/* caller reserved the edge up front */
	(void) ret;
}

/*
 * ft_hlist_freeze_sole_prepare: freeze a head that the caller DERIVED to be
 * its key's SOLE entry.  The callers are the key-disappearing lanes.  They
 * prune the branch around the head.  They are a detach and the fused
 * ft_remove_one_commit.
 *
 * The expected-old is that derivation, NULL.  It is never the slot re-loaded
 * here.  The derivation was made with nothing held.  A same-key
 * cds_ft_insert can have APPENDED a duplicate to @node since.  Under
 * LOCK_FINE such an insert is concurrent with a remove in contract.
 * Re-loading would mark that duplicate into the tombstone (MARK(N)).  The
 * prune would then orphan it behind a retired head.  The results:
 *   - an OK insert whose key never resolves again;
 *   - a later remove of it that never terminates.
 * This was measured: "key LOST after an OK concurrent insert", in both list
 * modes.
 * Recorded against NULL, the append fails this commit's install CAS
 * instead -- the record is MW, so a mismatch is an ABORT, which every
 * key-disappearing caller already routes to a re-derivation that then finds
 * the successor and PROMOTES it.
 */
static inline
void ft_hlist_freeze_sole_prepare(const struct cds_ft *ft, struct ft_txn *txn, struct cds_ft_node *node)
{
	int ret;

	ret = ft_hlist_store_chain(ft, txn, (void **) &node->next, NULL,
			ft_hlist_set_mark(NULL), FT_HLIST_TAG);
	assert(!ret);			/* caller reserved the edge up front */
	(void) ret;
}


/*
 * ft_hlist_chain_len: return how many nodes a freeze of the chain at @head
 * would record.  Each node costs ONE edge (FT_HLIST_FREEZE_MAX_EDGES), so
 * this is the caller's reservation.
 *
 * ☠ CALL IT UNDER THE CHAIN HOLDER.  That is what makes a walked count safe
 * to size a reservation with.  Every producer that could APPEND takes the
 * holder first, so the chain cannot grow between this count and the record
 * walk.  Counted without the holder, the freeze would under-reserve and trip
 * ft_hlist_store_chain's assert at the extra node.  Already-marked members
 * are counted too: they still cost the edge that re-records their mark.
 */
/*
 * ☞ WHY DOES THE TAIL DISAGREE?  (-DFT_DEBUG_HLIST_TAIL_WHY, with
 * -DFEATURE_FT_HOLD_TRACE)
 *
 * ft_hlist_freeze_chain_prepare records its LAST node against the caller's
 * derived NULL.  That derivation disagrees with the word 24,676 times per
 * ft_inv per-node leg (36,427 at exponential).  The other six derived
 * expected-olds in this file disagree ZERO times.
 *
 * The governing rule is: every chain mutation runs under the nearest ancestor
 * lock.  If it holds, that number should be ZERO too, because no append can
 * land while we hold the holder.  It is not zero.  So exactly one of two
 * things is true.  They have entirely different fixes:
 *
 *   STALE PLAN   the op DOES hold the holder, but @len was counted BEFORE the
 *                acquire, so the append landed legitimately, before our
 *                exclusion existed.  ft_hlist_chain_len's own header already
 *                forbids this ("☠ CALL IT UNDER THE CHAIN HOLDER"), so the fix
 *                is at the naming site: derive under the lock, or bail.
 *   UNHELD       the op does NOT hold the holder here, and the lock-set
 *                transition is simply not done for this path.  The fix is an
 *                acquire, and nothing in this file changes.
 *
 * So report the HOLDER witness and the CALLER that counted @len.  The caller
 * is interned by (fn, line).  The chain hold audit cannot answer this.  It
 * scores the moment of the STORE.  This is a question about the moment of
 * the DERIVATION.
 *
 * ☑ ANSWERED, AND THE FIRST ANSWER WAS THE WRONG ONE.  Asked of the hold
 * ledger's stamp alone, the table read US = 0 everywhere.  That was recorded
 * as UNHELD.  But the stamp cannot see a REGISTERED hold at all (see the
 * withdrawal in ft_hlist_store_chain_at's parked note).  So the three witness
 * columns are now read TOGETHER, at the same event.  For cds_ft_remove's
 * sole-entry path the answer is STALE PLAN or an appender gap, never UNHELD:
 * the registry owns the holder at 100% of its disagreements.
 *
 * ☠ WHICH IS WHY @reg_own EXISTS AND WHY THE STAMP IS KEPT BESIDE IT.  A
 * single-witness verdict on this question has now been wrong in both
 * directions.  The columns are only meaningful as a set.
 */
#ifdef FT_DEBUG_HLIST_TAIL_WHY
#define FT_HLIST_WHY_MAX	16
struct ft_hlist_why_site {
	const char *fn;
	int line;
	unsigned long tails, disagree;
	/*
	 * ☞ THE EXCLUSION ORACLE'S ANSWER, read at the moment of the
	 * disagreement.  The hold ledger CLAIMS cds_ft_metadata.dbg_owner_tid
	 * when an op takes a member, and YIELDS it when the op drops the
	 * member.  So it is a POSITIVE statement of who covers the chain's
	 * holder right now.  It is not the query that reads zero for ops whose
	 * hold only the registry can see.
	 *
	 *   own      we hold the holder => the APPENDER did not  => its gap
	 *   foreign  another op holds it => WE do not            => our gap
	 *   none     nobody claims it    => neither is covered, or the op's
	 *            hold never reached the ledger (an ENTRY-less claim)
	 */
	unsigned long excl_own, excl_foreign, excl_none;
	/*
	 * ☠☠ AND THE SAME EVENT ASKED OF THE OTHER TWO WITNESSES.  The stamp
	 * above CANNOT SEE A CONVERTED HOLD, and the tree says so twice:
	 *
	 *   - ft_flip_txn_record_release_lock calls ft_hold_trace_drop() the
	 *     moment it RECORDS the release.  See ft-mutation-helpers.h: "the
	 *     ledger is not a hold count -- it is an OUTSTANDING-RELEASE
	 *     count".  That drop YIELDS the stamp.  Some sites acquire a word
	 *     and immediately plant its {LOCK|s -> s} terminal.  The
	 *     lock-or-guard hoists do that.  Every such site therefore reads
	 *     US = 0 from then on.  Yet the word still carries FT_STATE_LOCK
	 *     and is still owned, by the commit rather than by the frame.
	 *   - The chain canary's own header already prices it.  The chain hold
	 *     audit measures ft_detach_node at REG = 2,579,201 / LED = 0.
	 *
	 * So report the REGISTRY (@reg_own) beside it, at the SAME event.  It
	 * comes from the ft_flip_txn whose handle this freeze was given.  A
	 * park is licensed by that witness and by no other (§11.9), so it is
	 * the one the flip actually needs.  US = 0 with @reg_own large is the
	 * wrong zero, not a gap.  @led_own is the ledger asked directly.  It
	 * shows that the stamp and the ledger agree and that the split is
	 * REGISTRY-vs-rest.
	 */
	unsigned long reg_own, led_own, reg_or_led, ftxn_none, ftxn_stale;
	/*
	 * ☠ THE CONTRADICTION COLUMN.  @excl_foreign says a PEER positively
	 * claims the chain's holder.  @reg_own says THIS txn's registry holds
	 * the same word.  Both cannot be true: ft_meta_lock_acquire refuses a
	 * word that already carries FT_STATE_LOCK.  So an overlap is one of
	 * two things:
	 *   - a real exclusion violation;
	 *   - an artefact of one of the two witnesses.  Either a ledger entry
	 *     leaked past its op, or a registry entry's release is recorded
	 *     but its commit has not landed.
	 * The overlap is counted rather than assumed away.  "A peer holds it"
	 * is the one column in this table that a blindness argument does NOT
	 * explain.  It only carries that weight if it is disjoint from our own
	 * hold.
	 */
	unsigned long reg_and_peer;
	/*
	 * ☠ AND THE MODE.  "The registry does not own the holder" is only a
	 * gap under FINE.  A COARSE trie serialises every writer behind the
	 * FT-wide mutex, and an EXCLUSIVE trie has one writer.  So neither
	 * takes a per-node lock at all.  Scoring those as unprotected would
	 * manufacture a gap out of a configuration that has no locks by
	 * design.  Only @unreg_fine is owed an explanation.
	 */
	unsigned long unreg_fine, unreg_coarse, unreg_excl, unreg_wlock;
#define FT_HLIST_WHY_PROD	8
	/* WHO stored the surprise: read off the duplicate's own canary. */
	const char *prod_fn[FT_HLIST_WHY_PROD];
	int prod_line[FT_HLIST_WHY_PROD];
	unsigned long prod_tid[FT_HLIST_WHY_PROD];
	unsigned long prod_n[FT_HLIST_WHY_PROD];
	unsigned int nr_prod;
	unsigned long prod_overflow;
};
extern struct ft_hlist_why_site ft_hlist_why_sites[FT_HLIST_WHY_MAX];
extern unsigned int ft_hlist_why_n;
extern __thread const char *ft_hlist_why_fn;
extern __thread int ft_hlist_why_line;
struct ft_hlist_why_site ft_hlist_why_sites[FT_HLIST_WHY_MAX];
unsigned int ft_hlist_why_n;
__thread const char *ft_hlist_why_fn;
__thread int ft_hlist_why_line;
/*
 * THE REGISTRY WITNESS' SOURCE.  A freeze helper is handed the ENGINE handle
 * (struct ft_txn *).  The lock registry lives one level up, in the
 * ft_flip_txn that owns it.  So the wrapper cannot be recovered from the
 * handle.  Every caller of a *_prepare form reaches it through
 * ft_flip_txn_handle().  So stamp the wrapper THERE, in the one place that
 * converts one into the other, and verify it here.  The classifier counts
 * @ftxn_stale whenever the stamped wrapper's handle is not the handle this
 * record is going into.  A mis-stamp then reads as its own column instead of
 * silently answering for the wrong transaction.
 */
struct ft_flip_txn;
extern __thread struct ft_flip_txn *ft_hlist_why_ftxn;
__thread struct ft_flip_txn *ft_hlist_why_ftxn;
static inline bool ft_flip_txn_owns(const struct ft_flip_txn *t,
		const struct cds_ft_metadata *owner);
static inline struct ft_txn *ft_flip_txn_handle(struct ft_flip_txn *t);
static bool ft_hold_trace_holds(const struct cds_ft_metadata *lock);
/*
 * ☞ COUNT THE BAIL, DO NOT JUST PREVENT THE EVENT.  ft_hlist_chain_plan_ok
 * turns a stale plan into a retry BEFORE the freeze records anything.  So
 * the DISAGREE rows above go quiet.  A quiet row looks the same as a path
 * that stopped being taken.  This counter tells them apart: the traffic that
 * used to land in DISAGREE has to reappear here, one for one.
 */
extern unsigned long ft_hlist_plan_bail;
unsigned long ft_hlist_plan_bail;
/*
 * WHY the plan was refused:
 *   - SHORT: the walk hit the end before @len.
 *   - TAIL_MARKED: the tail is already logically deleted but still linked.
 *     So ft_hlist_chain_len counted it while its word is MARK(NULL), not
 *     NULL.
 *   - TAIL_LIVE: a duplicate really was appended.
 * Only the last is the race the check exists for.  The other two are
 * disagreements between the COUNT's notion of "end of chain" and the
 * RECORD's.  A refusal on either is stable across a retry, i.e. a livelock,
 * not an arbitration.
 */
extern unsigned long ft_hlist_plan_why[3];
unsigned long ft_hlist_plan_why[3];
/*
 * ☞ OBSERVE-ONLY ARM.  It is for the fold / glue / merge freeze sites.  Their
 * @len is their CALLER's.  A bail there would need its own pre-commit
 * terminal.  Adding four more bails is the change that already
 * wedged this tree once by leaking a lock.  Before adding them, ASK WHETHER
 * THEY CAN EVER FIRE.  The tree's own gate for the SW flip is a counter
 * reading ZERO across every spacing.  It is never an argument that a path is
 * safe.  So this is the measurement that gate wants.
 * Indices:
 *   0 = the bulk fold
 *   1 = the external-promote arm
 *   2 = the src_cn retire arm
 *   3 = the merge collapse
 *   4 = ft_glue_record_splices' run append.  This is
 *       ft_hlist_append_run_prepare's literal NULL: the same serializing
 *       word from the APPEND side.  It is the site the 2026-09-17 flip
 *       attempt missed entirely.
 */
extern unsigned long ft_hlist_plan_obs[5], ft_hlist_plan_obs_n[5];
unsigned long ft_hlist_plan_obs[5], ft_hlist_plan_obs_n[5];

static inline
void ft_hlist_plan_snap(void)
{
	unsigned long n = uatomic_add_return(&ft_hlist_plan_bail, 1);

	if (n <= 8 || !(n % 4096))
		fprintf(stderr, "FT HLIST PLAN-BAIL %lu  SHORT=%lu "
			"TAIL_LIVE=%lu (noted: TAIL_MARKED=%lu)\n", n,
			uatomic_read(&ft_hlist_plan_why[0]),
			uatomic_read(&ft_hlist_plan_why[2]),
			uatomic_read(&ft_hlist_plan_why[1]));
}

static void ft_hlist_why_report(void) __attribute__((destructor));
static void ft_hlist_why_report(void)
{
	unsigned int i;

	fprintf(stderr, "FT HLIST PLAN-BAIL (stale plan refused UNDER THE LOCK, "
		"before any record): %lu   BAILED: SHORT=%lu TAIL_LIVE(the race "
		"this check exists for)=%lu | PASSED-but-noted: TAIL_MARKED=%lu\n",
		uatomic_read(&ft_hlist_plan_bail),
		uatomic_read(&ft_hlist_plan_why[0]),
		uatomic_read(&ft_hlist_plan_why[2]),
		uatomic_read(&ft_hlist_plan_why[1]));
	fprintf(stderr, "FT HLIST PLAN-OBSERVE (no bail; must read 0 before these "
		"sites convert): fold=%lu/%lu promote=%lu/%lu src_cn=%lu/%lu "
		"merge=%lu/%lu append_run=%lu/%lu\n",
		uatomic_read(&ft_hlist_plan_obs[0]), uatomic_read(&ft_hlist_plan_obs_n[0]),
		uatomic_read(&ft_hlist_plan_obs[1]), uatomic_read(&ft_hlist_plan_obs_n[1]),
		uatomic_read(&ft_hlist_plan_obs[2]), uatomic_read(&ft_hlist_plan_obs_n[2]),
		uatomic_read(&ft_hlist_plan_obs[3]), uatomic_read(&ft_hlist_plan_obs_n[3]),
		uatomic_read(&ft_hlist_plan_obs[4]), uatomic_read(&ft_hlist_plan_obs_n[4]));
	fprintf(stderr, "FT HLIST TAIL-WHY  (ledger %s)  (canary %s)  sites=%u/%u\n",
#ifdef FEATURE_FT_HOLD_TRACE
		"ON -- PERTURBING, see below",
#else
		"off -- registry-only, the low-perturbation rig",
#endif
#ifdef FT_DEBUG_CHAIN_CANARY
		"ON",
#else
		"OFF -- no producer can be named (rebuild with -DFT_DEBUG_CHAIN_CANARY)",
#endif
		ft_hlist_why_n, FT_HLIST_WHY_MAX);
	for (i = 0; i < ft_hlist_why_n; i++) {
		struct ft_hlist_why_site *e = &ft_hlist_why_sites[i];
		unsigned int k;

		if (!e->tails)
			continue;
		fprintf(stderr, "  len derived at %-28s:%-5d tails=%-10lu DISAGREE=%lu\n",
			e->fn, e->line, e->tails, e->disagree);
		if (e->disagree) {
			fprintf(stderr, "      REGISTRY owns=%lu  (no ftxn stamped=%lu,"
				" stale=%lu)\n",
				e->reg_own, e->ftxn_none, e->ftxn_stale);
			fprintf(stderr, "      NOT owned, by mode: FINE(a real gap)=%lu  "
				"coarse=%lu  exclusive=%lu  FT-wide-lock-held=%lu\n",
				e->unreg_fine, e->unreg_coarse, e->unreg_excl,
				e->unreg_wlock);
#ifdef FEATURE_FT_HOLD_TRACE
			fprintf(stderr, "      LEDGER (perturbing; metadata 48->88 B, "
				"2 atomics/acquire): holds=%lu either=%lu | stamp "
				"US=%lu PEER=%lu NOBODY=%lu | ☠ reg+peer (must be 0)=%lu\n",
				e->led_own, e->reg_or_led, e->excl_own,
				e->excl_foreign, e->excl_none, e->reg_and_peer);
#endif
		}
		for (k = 0; k < e->nr_prod; k++)
			fprintf(stderr, "      <- stored by %-34s:%-5d n=%-8lu tid=%lx\n",
				e->prod_fn[k], e->prod_line[k], e->prod_n[k],
				e->prod_tid[k]);
		if (e->prod_overflow)
			fprintf(stderr, "      <- (%lu more, producer table full)\n",
				e->prod_overflow);
	}
}

static inline
void ft_hlist_why_tail(struct cds_ft *ft, struct ft_txn *txn,
		struct cds_ft_node *head)
{
	void *live = (void *) CMM_LOAD_SHARED(head->next);

	struct ft_hlist_why_site *e = NULL;
	const char *fn = ft_hlist_why_fn ? ft_hlist_why_fn : "(unstamped)";
	int line = ft_hlist_why_line;
	unsigned int i;

	(void) ft;
	for (i = 0; i < ft_hlist_why_n; i++)
		if (ft_hlist_why_sites[i].line == line &&
				ft_hlist_why_sites[i].fn == fn) {
			e = &ft_hlist_why_sites[i];
			break;
		}
	if (!e) {
		if (ft_hlist_why_n >= FT_HLIST_WHY_MAX)
			return;
		e = &ft_hlist_why_sites[ft_hlist_why_n++];
		e->fn = fn;
		e->line = line;
	}
	uatomic_inc(&e->tails);
	if (!live)
		return;
	uatomic_inc(&e->disagree);
	{
		/*
		 * ☞ THE WHOLE VERDICT RUNS ONLY HERE, ON A DISAGREEMENT.
		 * That is about 1.4% of tails.  So the prev-walk in
		 * ft_chain_head_holder and the registry scan are paid on a
		 * thousandth of the traffic.  They are not paid on every
		 * chain store.
		 */
		struct cds_ft_inode_flag *hf = ft_chain_head_holder(ft, head);
		struct cds_ft_metadata *hm = hf ?
			ft_flag_to_metadata(ft, hf) : NULL;
		struct ft_flip_txn *ftxn = ft_hlist_why_ftxn;
		bool reg, led;

		/*
		 * ★ THE REGISTRY WITNESS COSTS NOTHING THE WORKLOAD CAN FEEL,
		 * and that is why it is the one kept outside -DFEATURE_FT_HOLD_TRACE.
		 * It reads the txn's own lock array, which every build already
		 * maintains.  It writes nothing.  It takes no atomic on a trie
		 * word.  It changes no struct.
		 *
		 * ☠ THE LEDGER IS THE OPPOSITE ON ALL THREE COUNTS.  So a
		 * finding that needs it is measured on a different data
		 * structure:
		 *   - FEATURE_FT_HOLD_TRACE adds five fields to cds_ft_metadata
		 *     (48 -> 88 bytes, +83%).  So slab packing and cacheline
		 *     sharing change for every node in the trie.
		 *   - ft_owner_stamp_claim / _yield put an atomic xchg AND an
		 *     atomic cmpxchg per acquire on the node's own metadata
		 *     line.  Peers CAS @state on that same line.
		 * ⇒ read this classifier WITHOUT the ledger by default.  Turn
		 * it on only to compare.
		 */
		if (!ftxn)
			uatomic_inc(&e->ftxn_none);
		else if (ft_flip_txn_handle(ftxn) != txn) {
			uatomic_inc(&e->ftxn_stale);
			ftxn = NULL;
		}
		reg = ftxn && hm && ft_flip_txn_owns(ftxn, hm);
		led = hm && ft_hold_trace_holds(hm);	/* false without the ledger */
		if (reg)
			uatomic_inc(&e->reg_own);
		if (led)
			uatomic_inc(&e->led_own);
		if (reg || led)
			uatomic_inc(&e->reg_or_led);
		/*
		 * THE MODE SPLIT BELONGS TO THE REGISTRY VERDICT, not to the
		 * stamp: "nobody owns the holder" is only owed an explanation
		 * under FINE.  A COARSE trie serialises every writer behind the
		 * FT-wide mutex.  An EXCLUSIVE one has a single writer.
		 * Neither has per-node locks, by design.  Scoring either as a
		 * gap would manufacture one.
		 */
		if (!reg) {
			if (ft->exclusive)
				uatomic_inc(&e->unreg_excl);
			else if (!ft->lock_fine)
				uatomic_inc(&e->unreg_coarse);
			else if (ft_wlock_held == ft)
				uatomic_inc(&e->unreg_wlock);
			else
				uatomic_inc(&e->unreg_fine);
		}
#ifdef FEATURE_FT_HOLD_TRACE
		{
			unsigned long owner = hm ?
				CMM_LOAD_SHARED(hm->dbg_owner_tid) : 0;

			if (!owner)
				uatomic_inc(&e->excl_none);
			else if (owner == (unsigned long) pthread_self())
				uatomic_inc(&e->excl_own);
			else
				uatomic_inc(&e->excl_foreign);
			if (reg && owner &&
					owner != (unsigned long) pthread_self())
				uatomic_inc(&e->reg_and_peer);
		}
#endif
	}
#ifdef FT_DEBUG_CHAIN_CANARY
	/*
	 * ☞ ASK THE NODE WHO PUT IT THERE.  @live is the duplicate that appeared
	 * where the caller derived NULL.  &head->next is the word it landed in.
	 * So that SLOT's canary names the site that stored it.  This is a fact
	 * the table carries, not an inference from a witness that can read
	 * zero.
	 */
	{
		const struct ft_canary_ent *c = ft_chain_canary_of(
			(void **) &head->next);
		const char *pfn = c ? c->fn :
			"(no canary -- evicted, or never stored through a chain helper)";
		int pline = c ? c->line : 0;
		unsigned int k;

		for (k = 0; k < e->nr_prod; k++)
			if (e->prod_fn[k] == pfn && e->prod_line[k] == pline) {
				uatomic_inc(&e->prod_n[k]);
				return;
			}
		if (e->nr_prod >= FT_HLIST_WHY_PROD) {
			uatomic_inc(&e->prod_overflow);
			return;
		}
		k = e->nr_prod++;
		e->prod_fn[k] = pfn;
		e->prod_line[k] = pline;
		e->prod_tid[k] = c ? c->tid : 0;
		uatomic_inc(&e->prod_n[k]);
	}
#endif
	/*
	 * ☠ AND CLEAR IT.  The stamp is a THREAD-LOCAL.  It is set at the
	 * derivation and read at the record.  Nothing in between bounds it.
	 * Suppose it is left standing.  The next freeze on this thread may have
	 * a LITERAL @len (cds_ft_remove's "1 = SOLE entry", the detach's
	 * forwarded count).  That freeze inherits whatever site last called
	 * ft_hlist_chain_len, and is filed under ITS name.  The table cannot
	 * show that misattribution.  So consume the stamp here.  A genuinely
	 * unstamped site then reads "(unstamped)".
	 */
	ft_hlist_why_fn = NULL;
	ft_hlist_why_line = 0;
}

# define FT_HLIST_WHY_STAMP()						\
	((void) (ft_hlist_why_fn = __func__),				\
	 (void) (ft_hlist_why_line = __LINE__))
# define FT_HLIST_WHY_TAIL(ft, txn, head)	ft_hlist_why_tail((ft), (txn), (head))
/*
 * ☠ atexit IS NOT ENOUGH.  A livelock is killed by a timeout or a memcg.
 * Neither runs a destructor.  So the classification of the ONE run that
 * reproduces would be lost exactly when it is needed.  Snapshot
 * periodically.  The last line in the log is the reading.
 */
static inline void ft_hlist_plan_snap(void);
# define FT_HLIST_PLAN_BAIL()			ft_hlist_plan_snap()
# define FT_HLIST_PLAN_WHY(i)			uatomic_inc(&ft_hlist_plan_why[(i)])
# define FT_HLIST_PLAN_OBSERVE(ft, txn, head, len, i)	do {		\
		uatomic_inc(&ft_hlist_plan_obs_n[(i)]);			\
		if (!ft_hlist_chain_plan_ok((ft), (txn), (head), (len)))	\
			uatomic_inc(&ft_hlist_plan_obs[(i)]);		\
	} while (0)
#else
# define FT_HLIST_WHY_STAMP()			((void) 0)
# define FT_HLIST_WHY_TAIL(ft, txn, head)	do { (void) (head); } while (0)
# define FT_HLIST_PLAN_BAIL()			do { } while (0)
# define FT_HLIST_PLAN_WHY(i)			do { (void) (i); } while (0)
# define FT_HLIST_PLAN_OBSERVE(ft, txn, head, len, i)	do { } while (0)
#endif

static inline
unsigned int ft_hlist_chain_len_at(struct cds_ft_node *head)
{
	unsigned int n = 0;

	while (head) {
		n++;
		head = ft_hlist_resolve((void *) CMM_LOAD_SHARED(head->next));
	}
	return n;
}

/*
 * This question is about the naming site, so stamp it here.  The classifier
 * above reports the DERIVATION's (fn, line), not the record's.
 */
#define ft_hlist_chain_len(head)					\
	(FT_HLIST_WHY_STAMP(), ft_hlist_chain_len_at(head))

/*
 * ft_hlist_chain_plan_ok: is the caller's @len still the truth about @head's
 * chain?  Answer it HERE, where the caller holds the chain's holder, so the
 * freeze below records a CHECKED value instead of an arbitration.
 *
 * ★ MATHIEU: "if we have proper locking, a load and check should be the same as
 * an MW record."  Exactly so, and that is the whole point of this function.
 * ft_hlist_freeze_chain_prepare's tail is the one expected-old in this file
 * that is DERIVED rather than loaded.  It has to be derived, but not because
 * of a property of the record.  It is because @len is counted BEFORE the
 * op's exclusion exists.  ft_hlist_chain_len's header already forbids that:
 * "☠ CALL IT UNDER THE CHAIN HOLDER".  Close that window, and three things
 * follow:
 *   - the derived NULL and a committed load agree by construction;
 *   - the MW CAS arbitrates nobody;
 *   - the record becomes parkable.
 *
 * ☠ COMMITTED, NOT ft_txn_load.  The plain load is READ-YOUR-OWN-WRITES.
 * This is a FUSED commit: the freeze rides ft_detach_node's structural txn.
 * In a fused commit the plain load returns this txn's own pending value.  It
 * never reads the word at all.  That was refutation #5 of the 2026-09-17
 * flip attempt: the "re-read under the lock" it added read nothing.
 *
 * ☠☠ AND THIS CHECK IS NOT BY ITSELF A LICENCE TO PARK.  "A load and check is
 * the same as an MW record" holds ONLY where the holder lock really excludes
 * every writer of the chain.  The check cannot substitute for the exclusion.
 * It can only be exact once the exclusion is.  A writer that skipped the
 * holder would have its in-flight append read as NULL here: its commit has
 * not flipped, and ft_txn_load_committed resolves its proxy to the
 * PRE-COMMIT value.  This check would answer "plan ok", and the SW record
 * would then store blind over that append, losing it.
 *
 * The lock set is what keeps such a writer out.  Its completeness for chain
 * words is MEASURED, not proven:
 *   - the chain-hold audit (-DFT_DEBUG_CHAIN_HOLD) reports no unheld chain
 *     store;
 *   - the one raw producer on a live chain word is measured unreachable (see
 *     ft_node_mark_removed_flip's header).
 *
 * Under the MCAS engine the install CAS caught such a writer, and this check
 * cut the appended-duplicate race from ~12k-15k per ft_inv leg to the sliver
 * its patience window left.
 *
 * ☠ VALIDATE EXACTLY WHAT THE WALK WILL RECORD, which is why the tail is
 * compared against a BARE NULL and not against ft_hlist_unmark(raw): the walk
 * records {NULL -> MARK(NULL)} there, so an already-marked tail fails that CAS
 * today.  Answering false here reproduces that outcome through the caller's
 * retry instead of through an abort -- same decision, made before any
 * structural work rather than after it.
 *
 * A SHORTER chain matters too.  The freeze walk stops at NULL (`i < len &&
 * head`).  So a chain that lost members since the count would silently
 * freeze fewer than planned.  Both directions are the same staleness.  Both
 * answer false.
 *
 * Returns true when the chain is EXACTLY @len nodes and the last one's forward
 * word is still NULL.  The caller must treat false as RETRIABLE -- the plan is
 * stale, not wrong -- and must not have made any reader-visible change yet.
 */
static inline
bool ft_hlist_chain_plan_ok(const struct cds_ft *ft, struct ft_txn *txn,
		struct cds_ft_node *head, unsigned int len)
{
	unsigned int i;

	(void) ft;
	if (!len)
		return true;		/* nothing planned, nothing to check */
	for (i = 0; i < len; i++) {
		void *raw;

		if (!head) {
			FT_HLIST_PLAN_WHY(0);
			return false;	/* shorter than @len: stale */
		}
		raw = ft_txn_load_committed(txn, (void **) &head->next,
				FT_HLIST_TAG);
		if (i + 1 == len) {
			/*
			 * ☠ END-OF-CHAIN MEANS WHAT THE COUNT MEANT BY IT.
			 * ft_hlist_chain_len walks with ft_hlist_resolve, which
			 * UNMARKS.  Take a tail that is already logically
			 * deleted but still linked (frozen without an unlink).
			 * The count takes it as the last node, and its word
			 * holds MARK(NULL).  A check that demanded a bare NULL
			 * here refused that shape.  The refusal is STABLE
			 * across the retry, because nothing about it changes.
			 * It was MEASURED as a livelock in
			 * inv_concurrent_insert_replace_nolist.  The
			 * classifier read TAIL_MARKED=8, TAIL_LIVE=0.  So
			 * every refusal was this disagreement.  NOT ONE was
			 * the appended duplicate the check exists for.
			 *
			 * So agree with the count: the plan is stale only when
			 * the chain really continues.
			 *
			 * ☠ AND A MARKED TAIL IS A SEPARATE, PRE-EXISTING
			 * DEFECT.  It is recorded here because this check
			 * found it.  ft_hlist_freeze_chain_prepare FORCES its
			 * tail's expected-old to NULL (`succ = NULL` at
			 * i + 1 == len).  That also defeats its own
			 * already-marked skip.  So on that shape it records
			 * {NULL -> MARK(NULL)} against a word already holding
			 * MARK(NULL).  Under MW the CAS just fails and the op
			 * retries.  ☠☠ Under SW that record PARKS, and its
			 * ABORT writes old_ptr back BLIND -- UNMARKING a node a
			 * peer froze.  The flip cannot happen until the tail
			 * reads its word instead of assuming it.
			 */
			if (ft_hlist_unmark(raw) == NULL) {
				if (raw != NULL)
					FT_HLIST_PLAN_WHY(1);
				return true;
			}
			FT_HLIST_PLAN_WHY(2);
			return false;
		}
		head = ft_hlist_unmark(raw);
	}
	return false;			/* unreachable */
}

/*
 * ft_hlist_freeze_chain_prepare: freeze the chain at @head into @txn WITHOUT
 * committing.  Record one {v -> MARK(v)} edge for each of @len nodes.  This
 * is the whole-key counterpart of ft_hlist_freeze_sole_prepare.  It is for
 * the lane that retires a chain ENTIRE (cds_ft_remove_all), not only its
 * last entry.
 *
 * For @len == 1 this is byte-identical to ft_hlist_freeze_sole_prepare.  The
 * one node is the tail, and its record is {NULL -> MARK(NULL)}.
 *
 * ☠ @len IS THE CALLER'S DERIVATION, AND THE WALK MUST NOT OUTRUN IT.  That is
 * the whole arbitration.  ft_hlist_freeze_sole_prepare spells out the same
 * one for its derived NULL.  The derivation is made with NOTHING HELD.  So a
 * same-key cds_ft_insert can APPEND a duplicate before this commit.  Under
 * LOCK_FINE such an insert is concurrent with a remove in contract.
 * Stopping at @len records the derived tail against NULL, so that append
 * fails this commit's install CAS and the whole detach ABORTS, and the
 * caller re-derives and finds the longer chain.
 *
 * RE-LOADING THE TAIL INSTEAD WOULD LOSE THE KEY.  A fresh load would find the
 * appended node.  It would mark it into the tombstone with the rest and
 * prune the branch around it.  The result is an insert that returned OK
 * whose key never resolves again.  This was measured in both list modes:
 * "key LOST after an OK concurrent insert".  So @len is a BOUND, never a
 * hint.  Do not re-walk to the real end.
 *
 * The successor is UNMARKED before advancing.  A member already logically
 * deleted carries the mark in its own next.  Following the raw value would
 * walk off by FT_HLIST_MARK into nothing.  Such a member owes no second mark.
 */
static inline
void ft_hlist_freeze_chain_prepare_at(const struct cds_ft *ft,
		struct ft_txn *txn, struct cds_ft_node *head,
		unsigned int len, bool plan_checked)
{
	unsigned int i;

	for (i = 0; i < len && head; i++) {
		struct cds_ft_node *succ;
		struct cds_ft_node *next;
		int ret;

		/*
		 * THE LAST NODE OF THE DERIVATION IS THE APPEND POINT.  Its
		 * expected-old is the derived NULL, not a load.  Every earlier
		 * node is interior, where no append can land.  So its successor
		 * is read here.
		 *
		 * ☠ THE DERIVED NULL IS AN ARBITRATION, NOT A DESCRIPTION, and
		 * it is the one expected-old in this file that is.  @len is the
		 * CALLER's, counted before this op's exclusion exists, so a
		 * same-key cds_ft_insert can APPEND a duplicate in the gap.
		 * Recorded against NULL, that append makes this commit LOSE its
		 * install CAS and the caller re-derives; re-loading instead
		 * would mark the duplicate into the tombstone and the prune
		 * would orphan it ("key LOST after an OK concurrent insert",
		 * measured in both list modes).
		 *
		 * ☞ MEASURED, per ft_inv leg: the tail disagrees with the
		 * derivation 19,196 times at per-node and 18,712 at exponential,
		 * so this arbitration is live -- and it is a REASON THE CLASS
		 * CANNOT PARK YET, because an SW record always writes.  See
		 * ft_hlist_store_chain_at's parked note.
		 */
		if (i + 1 == len) {
			next = NULL;
			/*
			 * ☠☠ THE TAIL MUST READ ITS WORD, NOT ASSUME IT.
			 * But it may do so only once the plan has been
			 * CHECKED under the holder.
			 *
			 * Forcing @succ to NULL here also defeats this walk's
			 * own already-marked skip below.  Take a tail that is
			 * frozen but still LINKED.  Its word holds MARK(NULL).
			 * ft_hlist_chain_len counts it as the end of the chain,
			 * because ft_hlist_resolve unmarks.  On that tail this
			 * records {NULL -> MARK(NULL)} against a word already
			 * holding MARK(NULL).  Under MW that CAS merely fails
			 * and the op retries -- which is why it has been
			 * invisible.  ☠ Under
			 * SW the record PARKS and its abort writes @old_ptr back
			 * BLIND, UNMARKING a node a peer froze.  This was
			 * MEASURED once the appended-duplicate race was closed.
			 * It is the entire residual tail-disagreement
			 * population: 11,425 / 10,772 / 12,722 per ft_inv leg
			 * at the three spacings.
			 *
			 * ft_hlist_chain_plan_ok has just proved unmark(word) is
			 * NULL.  So this load can only answer NULL or
			 * MARK(NULL).  NULL records as before.  MARK(NULL)
			 * takes the skip.
			 * ☠ WITHOUT that proof the load is the "re-loading the
			 * tail LOSES THE KEY" hazard in the header above.  It
			 * would mark a duplicate appended since the count, and
			 * the prune would orphan it.  So an UNCHECKED caller
			 * keeps the derived NULL and its arbitration.
			 */
			succ = NULL;
			if (plan_checked) {
				void *raw = ft_txn_load_committed(txn,
					(void **) &head->next, FT_HLIST_TAG);

				/*
				 * ☠☠ ONLY AN ALREADY-MARKED TAIL MAY BE READ
				 * BACK.  The plan check and this load are TWO
				 * READS.  A writer that skipped the holder
				 * (see ft_hlist_chain_plan_ok's header) could
				 * flip between them.  Its append would read
				 * as the pre-commit value there, and as a
				 * LIVE duplicate here.
				 * Taking that value would mark the freshly
				 * appended duplicate and let the prune orphan
				 * it: "key LOST after an OK concurrent
				 * insert".  Under the MCAS engine it was
				 * MEASURED at 16 / 15 / 32 events per ft_inv
				 * leg, before the chain words were held.
				 *
				 * So take the read ONLY where it can lose
				 * nothing.  A MARK means the node is already
				 * frozen, and the skip below is right.
				 * Anything else keeps the derived NULL.
				 */
				/*
				 * ☠ AND THE MARK ALONE IS NOT THE CONDITION.
				 * MARK(NULL) IS.  Take a tail word of MARK(X)
				 * with X live.  The node is already frozen AND
				 * the chain continues past @len.  Taking it
				 * would skip the record and let the commit
				 * proceed.  It would freeze fewer nodes than
				 * the chain has, where the derived NULL aborts
				 * instead.  So accept exactly the
				 * already-frozen genuine tail and nothing else.
				 * With that condition the read cannot lose
				 * anything, whether or not the plan was
				 * checked.  The gate above limits the blast
				 * radius.  It is not what makes this safe.
				 */
				if (raw != NULL && ft_hlist_unmark(raw) == NULL)
					succ = (struct cds_ft_node *) raw;
			}
		} else {
			succ = (struct cds_ft_node *) ft_txn_load(txn,
				(void **) &head->next, FT_HLIST_TAG);
			next = ft_hlist_unmark(succ);
		}
		if (!((uintptr_t) succ & FT_HLIST_MARK)) {
			if (i + 1 == len)
				FT_HLIST_WHY_TAIL((struct cds_ft *) ft, txn, head);
			ret = ft_hlist_store_chain(ft, txn, (void **) &head->next,
					succ, ft_hlist_set_mark(succ),
					FT_HLIST_TAG);
			assert(!ret);	/* caller reserved one edge per node */
			(void) ret;
		}
		head = next;
	}
}

/*
 * The two entry points:
 *   - The plain form keeps the derived tail NULL and the arbitration that
 *     goes with it.
 *   - The _checked form is for a caller that has just run
 *     ft_hlist_chain_plan_ok under the chain's holder.
 * Only the _checked form may read the tail's word.
 */
#define ft_hlist_freeze_chain_prepare(ft, txn, head, len)		\
	ft_hlist_freeze_chain_prepare_at((ft), (txn), (head), (len), false)
#define ft_hlist_freeze_chain_prepare_checked(ft, txn, head, len)	\
	ft_hlist_freeze_chain_prepare_at((ft), (txn), (head), (len), true)

#endif	/* _FT_TXN_HLIST_H */
