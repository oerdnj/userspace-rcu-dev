// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie.c
 *
 * Userspace RCU library - Fractal Trie
 *
 * ===================== High-level architecture =====================
 *
 * The Fractal Trie is a concurrent, RCU-protected ordered map from
 * opaque byte keys to application-owned nodes.  Lookups and ordered
 * traversals are wait-free under the RCU read-side lock.  They run
 * concurrently with mutations.  Writers are serialized by a
 * caller-provided mutex.
 *
 * The public contract lives in <urcu/fractal-trie.h>: semantics,
 * guarantees, locking rules and error codes.  This comment is the
 * implementation map.
 *
 * Source layout
 * -------------
 *   fractal-trie.c          core: read descent; point / range / rank
 *                           lookups; ordered iteration; insert / remove /
 *                           replace; the bulk ops; compaction.
 *   fractal-trie-internal.h struct layouts, tagged-pointer encodings, the
 *                           node-type table, and inline helpers.
 *   fractal-trie-alloc.c    the strided internal-node allocator and the
 *                           external (leaf) buddy arena.
 *   <urcu/rcu-txn.h>        the atomic multi-pointer "flip" primitive
 *   <urcu/rcu-txn-list.h>   (concurrent MCAS transaction + coherent bidir
 *                           list) used to commit a set of edges at once.
 *
 * Node model
 * ----------
 * Internal nodes adapt to their child density:
 *   - cascaded popcount bitmaps for small and medium fan-out;
 *   - a 256-entry "pigeon" array for dense nodes.
 * Internal nodes are sized in powers of two.  The low (tag) bits of the
 * child pointer encode the node type and configuration.  So the read
 * path dispatches with no extra load.
 *
 * External (leaf) nodes are application-owned: they embed struct
 * cds_ft_node.  Same-key duplicates form a next-linked chain off the
 * head.
 *
 * Path compression
 * ----------------
 * Single-child chains collapse into compressed (Patricia / ART-style)
 * nodes that store the shared bytes inline.
 *
 * The "skip-compressed" encoding (FEATURE_FT_SKIP_COMPRESSED) goes
 * further on 64-bit arches with free high pointer bits.  It packs the
 * skip length and the child pointer into the parent slot.  A speculative
 * descent then bypasses the compressed node's cache line entirely.
 *
 * -DNO_FEATURE_FT_COMPRESS disables compression altogether.
 *
 * Memory layout
 * -------------
 * The internal-node allocator strides item data and metadata onto
 * separate cache lines.  The read hot path touches only the dense item
 * region.  This is why resident memory overstates the cache-hot working
 * set (see fractal-trie-alloc.c).  Internal nodes are reclaimed via
 * call_rcu.
 *
 * Ordered iteration
 * -----------------
 * When enabled (the default), the library threads the duplicate-chain
 * heads into a key-ordered list of "ordinal cells".  The cells are small
 * and library-owned.  They are kept off the descent hot path.
 * cds_ft_next / cds_ft_prev and the batched cell walk step that list.
 *
 * Disabling it (cds_ft_group_attr_set_ordered_list false) drops the
 * per-key cell.  Memory use is lower and mutations are faster, at the
 * cost of ordered iteration.
 *
 * Read descent
 * ------------
 * There are two descent encodings per group (enum
 * cds_ft_lookup_optimization):
 *   - SPECULATIVE skips the per-node byte comparison.  It returns a
 *     candidate that the caller (or the speculative-lookup wrapper)
 *     validates.
 *   - EAGER compares exactly at each step.
 * Both return verified results.
 *
 * Inequality and rank / skip queries use per-node key counters to skip
 * whole subtrees in O(depth).
 *
 * next / prev / remove and key reconstruction go back up the trie.  They
 * follow parent back-pointers (and the cell list for ordered walks), not
 * a recorded descent path.
 *
 * Mutation and concurrency model
 * ------------------------------
 * Writers are serialized by a caller-provided mutex.
 * CDS_FT_SCOPED_WRITER only VALIDATES that exclusion; it is not itself a
 * lock.
 *
 * Every change to published state follows a build-invisibly -> publish
 * -> reclaim discipline:
 *   1. Assemble a new node cluster where readers cannot reach it.
 *   2. Make it visible by a single release store.  A multi-edge commit,
 *      such as a non-empty merge, instead uses one urcu-flip-latch commit
 *      that flips all affected edges at once.
 *   3. Free the displaced nodes after a grace period.
 * A reader therefore always observes a complete prior-or-result state,
 * never a partial one.  The rcu-mutation rules spell out the discipline.
 * The verify-at-mutation build option (-DFEATURE_FT_VERIFY_AT_MUTATION)
 * checks it.
 *
 * The bulk ops move or combine whole sub-tries between tries of one
 * group.  They are graft, graft-swap, detach, merge and merge_at.  The
 * union cases spine-copy the overlap and commit through the flip latch.
 *
 * A trie is either exclusive or concurrent:
 *   - exclusive: no concurrent readers.  Reclaim is synchronous and a
 *     source graft skips its drain.
 *   - concurrent: RCU readers are permitted.
 *
 * Reclamation
 * -----------
 * Deferred frees go through call_rcu; a fully-drained allocator range
 * releases its pages (MADV_DONTNEED on Linux) from inside the callback.
 * cds_ft_compact relocates live nodes into dense fresh ranges to recover
 * fragmentation; the emptied ranges reclaim after a grace period.
 * ===================================================================
 */

#define _LGPL_SOURCE
#include <stdint.h>
#include <errno.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <endian.h>
#include <stdbool.h>
#include <sys/mman.h>
#include <urcu/fractal-trie.h>
#include <urcu/compiler.h>
#include <urcu/arch.h>
#include <urcu-pointer.h>
#include <urcu/uatomic.h>
#include "urcu-utils.h"

/*
 * ☠ Include this header BEFORE fractal-trie-internal.h, which pulls
 * <urcu/rcu-txn.h>.  This header DEFINES the engine's abort-attribution
 * hooks.  The engine's static inlines are compiled where <urcu/rcu-txn.h> is
 * parsed.  If this header came later, the hooks would expand to the engine's
 * own inert defaults.  The instrument would then build clean and count
 * nothing.
 */
#include "ft-txn-rec-dbg.h"
/*
 * The bulk-window dump belongs to the TU that owns the FT bodies.
 * fractal-trie-alloc.c includes the same header.  Without this define it would
 * emit a second report that is permanently EMPTY.  An empty report reads
 * exactly like a configuration miss.  That is the one misread the instrument
 * exists to prevent.
 */
#define FT_BW_OWNS_DUMP
#include "fractal-trie-internal.h"

#ifdef FEATURE_FT_PROBE_REANCHOR
/*
 * Reanchor level-move coverage.
 *
 * A descent raises skip_conflict only when a reanchor lands SHALLOWER
 * (rewind != 0).  This happens because a peer chain-merge moved the encoded
 * position.  Graft and insert bail on this condition; ft-merge.h does not.
 *
 * Measured over the suite, the rewind counters read ZERO against millions of
 * reanchors.  So no test validates the existing bails, or a new one.  The
 * counters are here so that an oracle built to drive that race can say when
 * it finally does.
 */
unsigned long ft_probe_mrg_desc[3], ft_probe_mrg_conf[3];
unsigned long ft_probe_ranch[2], ft_probe_rewind[2];
unsigned long ft_probe_mspin[5];
unsigned long ft_probe_rspin[6];
unsigned long ft_probe_rspin_x[3], ft_probe_rspin_n[3];
unsigned long ft_probe_rspin_e[3][2];
static const char *const ft_probe_mrg_name[3] = { "src", "mergepoint", "dst" };
static __attribute__((destructor))
void ft_probe_mrg_report(void)
{
	unsigned int i;

	fprintf(stderr, "MRGPROBE");
	for (i = 0; i < 3; i++)
		fprintf(stderr, " %s: descents=%lu conflicts=%lu",
			ft_probe_mrg_name[i], ft_probe_mrg_desc[i],
			ft_probe_mrg_conf[i]);
	fprintf(stderr, " | reanchor compressed=%lu rewinds=%lu slot=%lu rewinds=%lu",
		ft_probe_ranch[0], ft_probe_rewind[0],
		ft_probe_ranch[1], ft_probe_rewind[1]);
	fprintf(stderr, " | mergespin ops=%lu contended=%lu (exclsrc=%lu livesrc=%lu) levelmove=%lu deepest=%lu\n",
		ft_probe_mspin[0], ft_probe_mspin[1], ft_probe_mspin[4],
		ft_probe_mspin[1] - ft_probe_mspin[4], ft_probe_mspin[2],
		ft_probe_mspin[3]);
	/*
	 * Each loop's retries are split by the contract.  The contract decides
	 * whether a persistent-handle bracket is expressible there at all:
	 *   excl = the retries that landed under it;
	 *   live = the retries that did not.
	 */
	fprintf(stderr, "RSPIN retry_merge=%lu deepest=%lu (excl=%lu live=%lu offc=%lu)"
		" | retry_attach=%lu deepest=%lu (excl=%lu live=%lu offc=%lu)"
		" | retry_swap=%lu deepest=%lu (excl=%lu live=%lu offc=%lu)\n",
		ft_probe_rspin[0], ft_probe_rspin[1],
		ft_probe_rspin_x[0], ft_probe_rspin[0] - ft_probe_rspin_x[0],
		ft_probe_rspin_n[0],
		ft_probe_rspin[2], ft_probe_rspin[3],
		ft_probe_rspin_x[1], ft_probe_rspin[2] - ft_probe_rspin_x[1],
		ft_probe_rspin_n[1],
		ft_probe_rspin[4], ft_probe_rspin[5],
		ft_probe_rspin_x[2], ft_probe_rspin[4] - ft_probe_rspin_x[2],
		ft_probe_rspin_n[2]);
	/*
	 * Slot 0 is shared by TWO loops.  Its offc cannot say which one ran,
	 * nor whether either ran at all.  Total ENTRIES per site, split by the
	 * contract, answers both.
	 */
	fprintf(stderr, "RSPIN-ENTRIES merge_subpos{oncontract=%lu offcontract=%lu}"
		" rekey_subpos{oncontract=%lu offcontract=%lu}"
		" graft_swap{oncontract=%lu offcontract=%lu}\n",
		ft_probe_rspin_e[0][1], ft_probe_rspin_e[0][0],
		ft_probe_rspin_e[1][1], ft_probe_rspin_e[1][0],
		ft_probe_rspin_e[2][1], ft_probe_rspin_e[2][0]);
}
#endif

#include "fractal-trie-trace.h"

#include "bitmap.h"

/*
 * The implementation is split into per-module units.  They are #included
 * below, in dependency order, into this single translation unit.  So the
 * compiler still inlines across module boundaries (e.g. the descent into each
 * lookup), exactly as when this was one 25k-line file.  FRACTAL_TRIE_IMPL
 * gates each unit against stray standalone inclusion.
 */
#define FRACTAL_TRIE_IMPL
#include "ft-tables.h"
#include "ft-delay.h"
#include "ft-helpers.h"
#include "ft-txn-kind-stats.h"	/* -DFT_DEBUG_TXN_KIND record-kind counters */
#include "ft-txn-hlist.h"	/* duplicate-chain transactional hlist */
#include "ft-trace-helpers.h"
#include "ft-lookup-node.h"
#include "ft-descent.h"
#include "ft-iter.h"
#include "ft-lookup.h"

/*
 * With FEATURE_INLINE_INEQUALITY_LOOKUP off, the inequality descent is shared.
 * It is then itself a cold tier-2 path.  So route its up/down scanners through
 * the same shared copies that the write path uses.  The whole library then has
 * one get_direction and one get_minmax, with no extra inlined copy in
 * ft_ineq_descend.  With the flag on, the descent inlines them for speed.
 */
#ifndef FEATURE_INLINE_INEQUALITY_LOOKUP
#define ft_node_get_direction	ft_node_get_direction_shared
#define ft_node_get_minmax	ft_node_get_minmax_shared
#endif
#include "ft-inequality.h"
#ifndef FEATURE_INLINE_INEQUALITY_LOOKUP
#undef ft_node_get_direction
#undef ft_node_get_minmax
#endif

/*
 * Redirect the write path to the shared, non-inlined copies of the large
 * read-path helpers:
 *   - the node scanners (ft-lookup-node.h);
 *   - the inequality descent (ft-inequality.h).
 * Otherwise the write path would force-inline them at every site.  The
 * mutation modules call each helper once instead of duplicating it.  Together
 * this is about -23% .text.  The read modules above keep the inlined
 * originals.
 *
 * The redirect stays open through every module below.  Those are all cold
 * paths or write paths: the mutation path, compaction and the ft-show.h
 * display renderers.  So they all share the out-of-line copies.  Nothing hot
 * follows, so the redirect is never closed.
 */
#define ft_node_get_nth           ft_node_get_nth_shared
#define ft_node_get_nth_skip      ft_node_get_nth_skip_shared
#define ft_node_get_nth_reanchor  ft_node_get_nth_reanchor_shared
#define ft_node_get_direction     ft_node_get_direction_shared
#define ft_node_get_minmax        ft_node_get_minmax_shared
#define cds_ft_lookup_inequality_impl cds_ft_lookup_inequality_impl_shared
#include "ft-mutation-helpers.h"
#include "ft-mutation-node.h"
#include "ft-cluster-build.h"
#include "ft-insert.h"
#include "ft-remove.h"

#ifdef FEATURE_FT_FAULT_INJECT
extern long cds_ft_fault_commit_countdown;
#endif

#include "ft-graft.h"
#include "ft-detach.h"
#include "ft-merge.h"
#include "ft-rekey.h"	/* same-trie move: see the unit header */
#include "ft-ordered-query.h"
#include "ft-lifecycle.h"
#include "ft-verify.h"
#include "ft-compact.h"
#include "ft-show.h"

/*
 * Public API back-end for cds_ft_node_next_rcu()'s proxy path (declared in
 * <urcu/fractal-trie.h>).
 *
 * The raw "next" value of a duplicate successor can carry the engine proxy
 * tag.  The tag means that a bulk commit is mid-flight on the slot, e.g. a
 * merge appending a src run at this chain's tail.  Resolve such a value to
 * the committed successor, with the removal tombstone stripped.
 *
 * This is kept out of line so that the transactional engine and its headers
 * stay opaque to API users.  A duplicate-chain walk is not a fast path, so
 * the cost of the call does not matter.
 */
urcu_static_assert(CDS_FT_NODE_TXN_PROXY_TAG == URCU_TXN_TAG,
		"FT public duplicate-next proxy tag must equal the engine proxy tag",
		ft_node_next_proxy_tag_matches_engine);
struct cds_ft_node *cds_ft_node_next_resolve(void *raw)
{
	return ft_hlist_resolve(raw);
}

/*
 * TEST/DEBUG: hold the move mode gate open without performing a move.  A
 * SINGLE-THREADED test needs this to exercise the coherent reader path at all.
 * With no mover the gate is closed, and readers correctly take the fast path.
 * The coherence tests would then assert against dead code.  The blocking
 * contract is the same as for a real move: do not call this from an RCU
 * read-side critical section.
 */
/*
 * Bulk-op nesting depth for THIS thread.  ft_dlm_acquire_set_at reads it to
 * tell two callers apart:
 *   - a point op, which must widen;
 *   - the bulk op that is holding the gate, which must not widen against
 *     itself.
 * It is a depth count because bulk bodies nest.
 */
__thread unsigned long ft_bulk_self_depth;
/* Escalation lane depth for THIS thread: see ft_op_in_lane. */
__thread unsigned int ft_op_lane_depth;

/*
 * G5.5's ancestor ledger (ft-mutation-helpers.h): the descent path for a point
 * op that must widen.  One block per thread; only touched while a bulk op is
 * live.
 */
#ifdef FT_ANC_LEDGER
__thread struct ft_anc_ledger ft_anc_ledger;
#endif

#ifdef FT_DEBUG_FORCE_ACQ_HEAP
unsigned long ft_acq_heap_taken;

__attribute__((destructor))
static void ft_acq_heap_dump(void)
{
	fprintf(stderr, "FT ACQHEAP: overflow allocations taken = %lu%s\n",
		ft_acq_heap_taken, ft_acq_heap_taken ? "" :
		"  <-- PATH NEVER RAN: this build proves nothing");
}
#endif

#ifdef FT_DEBUG_ANC_LEDGER
unsigned long ft_anc_rec_descents, ft_anc_rec_entries;
unsigned long ft_anc_rec_overflow, ft_anc_rec_disorder;
unsigned long ft_anc_rec_copy_push;

__attribute__((destructor))
static void ft_anc_rec_dump(void)
{
	if (!ft_anc_rec_descents) {
		fprintf(stderr, "FT ANCLEDGER: NEVER RECORDED -- no descent ran "
			"while a bulk op was live.  A CONFIGURATION MISS, not "
			"a measurement: run an arm with concurrent bulk and "
			"point ops.\n");
		return;
	}
	fprintf(stderr, "FT ANCLEDGER: recording descents=%lu entries=%lu "
		"overflow=%lu disorder=%lu copy_push=%lu\n", ft_anc_rec_descents,
		ft_anc_rec_entries, ft_anc_rec_overflow, ft_anc_rec_disorder,
		ft_anc_rec_copy_push);
}
#endif

#ifdef FT_DEBUG_WIDEN_OWNER
struct ft_wo_site ft_wo_site_tbl[FT_WO_SITES];
unsigned long ft_wo_site_overflow;
unsigned long ft_wo_acq_total, ft_wo_gate_enters;
unsigned long ft_wo_live, ft_wo_self_bulk;
unsigned long ft_wo_txn_top, ft_wo_txn_chain, ft_wo_txn_none;
unsigned long ft_wo_led_ok, ft_wo_led_nodescent, ft_wo_led_norec;
unsigned long ft_wo_led_superseded, ft_wo_led_overflow;
unsigned long ft_wo_led_nr_max;
unsigned long ft_wo_joint[4];
unsigned long ft_bl_acq, ft_bl_root, ft_bl_members;
unsigned long ft_bl_found, ft_bl_undatable, ft_bl_led_empty;
unsigned long ft_bl_win_ok, ft_bl_win_no;
unsigned long ft_bl_up_ok, ft_bl_up_refused, ft_bl_up_capped;
unsigned long ft_bl_up_agree, ft_bl_up_disagree;
unsigned long ft_wo_nod_remove_locked, ft_wo_nod_chain_head;

__attribute__((destructor))
static void ft_wo_dump(void)
{
	unsigned int i;

	/*
	 * THREE ZEROS LOOK ALIKE AND MEAN DIFFERENT THINGS.  They are
	 * separated here, not left for a reader to guess:
	 *   no acquire            The choke point never ran.  This is a
	 *                         configuration miss (not a lock_fine trie).
	 *                         Everything below is vacuous.
	 *   no gate enter         No bulk op ran at all.  This is a statement
	 *                         about the ARM.
	 *   gates but no overlap  Bulk ops ran and never coincided with an
	 *                         acquire.  This is a statement about the
	 *                         WORKLOAD.  It is the only one of the three
	 *                         that is a result.
	 */
	if (!ft_wo_acq_total) {
		fprintf(stderr, "FT WIDENOWNER: THE ACQUIRE CHOKE POINT NEVER "
			"RAN -- this build proves nothing.  Run a lock_fine "
			"trie.\n");
		return;
	}
	if (!ft_wo_gate_enters) {
		fprintf(stderr, "FT WIDENOWNER: %lu acquires, and NO BULK OP "
			"EVER ENTERED THE GATE -- a CONFIGURATION MISS, not a "
			"measurement: run concurrent bulk and point ops.\n",
			ft_wo_acq_total);
		return;
	}
	if (!ft_wo_live) {
		fprintf(stderr, "FT WIDENOWNER: %lu acquires, %lu gate enters, "
			"but NONE OVERLAPPED (self_bulk %lu) -- the widening "
			"would never have fired in this workload.\n",
			ft_wo_acq_total, ft_wo_gate_enters, ft_wo_self_bulk);
		return;
	}
	fprintf(stderr,
		"FT WIDENOWNER: acquires=%lu gate_enters=%lu live=%lu "
		"self_bulk=%lu\n"
		"  release owner (LOWER BOUND -- a site's txn need not be in "
		"any ctx frame):\n"
		"                  txn_top=%lu txn_chain=%lu txn_none=%lu\n"
		"  ledger verdict: ok=%lu | no_descent=%lu not_recording=%lu "
		"superseded=%lu overflow=%lu\n"
		"  ledger nr max=%lu  <-- KEY DEPTH, *not* a lock-set size "
		"(every gate entry passes level 0)\n"
		"  site table:     overflow=%lu\n",
		ft_wo_acq_total, ft_wo_gate_enters, ft_wo_live, ft_wo_self_bulk,
		ft_wo_txn_top, ft_wo_txn_chain, ft_wo_txn_none,
		ft_wo_led_ok, ft_wo_led_nodescent, ft_wo_led_norec,
		ft_wo_led_superseded, ft_wo_led_overflow,
		ft_wo_led_nr_max, ft_wo_site_overflow);
	/*
	 * THE JOINT, because two equal marginals do not make one.  Separate
	 * led_ok / txn_none totals cannot show that "every acquire that can
	 * widen lacks an owner".  An INDEPENDENT population that merely
	 * happened to be equinumerous would give the same totals.
	 */
	fprintf(stderr,
		"  joint led_ok x txn:  ok+txn=%lu ok+none=%lu bad+txn=%lu "
		"bad+none=%lu\n"
		"  no-descent blamed on: remove_locked=%lu chain_head=%lu "
		"(of %lu)\n",
		ft_wo_joint[3], ft_wo_joint[2], ft_wo_joint[1], ft_wo_joint[0],
		ft_wo_nod_remove_locked, ft_wo_nod_chain_head,
		ft_wo_led_nodescent);
	fprintf(stderr,
		"  BULK-OP ACQUIRES: %lu (%lu members) | reaching the ROOT: "
		"%lu%s\n",
		ft_bl_acq, ft_bl_members, ft_bl_root,
		ft_bl_acq ? "" : "  <-- none seen");
	fprintf(stderr,
		"  BULK members datable from the op's OWN ledger: %lu | "
		"UNDATABLE: %lu | acquires with no ledger: %lu\n",
		ft_bl_found, ft_bl_undatable, ft_bl_led_empty);
	fprintf(stderr,
		"  BULK members datable by the REAL path (descent WINDOW, "
		"ft_descent_depth_of): %lu | NOT: %lu\n",
		ft_bl_win_ok, ft_bl_win_no);
	fprintf(stderr,
		"  BULK members datable by the UP-WALK (parent_word to the "
		"trie): %lu | refused(NULL, re-home): %lu | capped: %lu\n"
		"    cross-check vs the window: agree=%lu DISAGREE=%lu\n",
		ft_bl_up_ok, ft_bl_up_refused, ft_bl_up_capped,
		ft_bl_up_agree, ft_bl_up_disagree);
	for (i = 0; i < FT_WO_SITES; i++) {
		struct ft_wo_site *s = &ft_wo_site_tbl[i];

		if (!s->fn || !s->live)
			continue;
		fprintf(stderr, "  SITE %-34s:%-5d live=%-10lu led_ok=%-10lu "
			"fine=%lu\n",
			s->fn, s->line, s->live, s->led_ok, s->fine);
	}
}
#endif

void _cds_ft_debug_move_gate_enter(struct cds_ft *ft)
{
	ft_move_gate_enter(ft);
}

void _cds_ft_debug_move_gate_exit(struct cds_ft *ft)
{
	ft_move_gate_exit(ft);
}

/*
 * TEST/DEBUG (NOT public API).  Raise and lower the gate that detach, graft,
 * graft_swap and merge_at raise.  Only the gate: not the bulk lock that
 * serializes their bodies.  A thread that holds it stands in for such an op
 * waiting for its turn, so another bulk op can still run.  Call both from
 * the same thread.
 */
void _cds_ft_debug_bulk_gate_enter(struct cds_ft *ft)
{
	ft_bulk_gate_enter_gp(ft, FT_BULK_WRITER_ONLY);
}

void _cds_ft_debug_bulk_gate_exit(struct cds_ft *ft)
{
	ft_bulk_gate_exit(ft, FT_BULK_WRITER_ONLY);
}
/*
 * TEST/DEBUG (coherent-rekey sub-step 2, NOT public API).
 *
 * Return the trie root as an opaque address.  A test uses it to observe that
 * a COW relocation moved the root's identity.
 */
void *_cds_ft_debug_root(struct cds_ft *ft)
{
	return (void *) rcu_dereference(ft->root);
}

/*
 * TEST/DEBUG (coherent-rekey sub-step 2, NOT public API).
 *
 * Drive ft_rekey_cow_stop on the trie ROOT as a self-contained in-place
 * clone.  All as ONE mixed SW/MW commit:
 *   - build a fresh-address copy of the root;
 *   - re-parent its children onto the copy;
 *   - retire the old root;
 *   - republish at &ft->root.
 *
 * This is the smallest complete consumer of the S_top COW primitive.  The
 * root has no parent to lock: its forward slot is auto-guarded by its own
 * CAS.  So this isolates the COW + interior re-parent + retire from the rest
 * of a rekey stitch.
 *
 * SINGLE-WRITER use only (no concurrent root relocation).
 *
 * Returns:
 *   0        The root is now at a fresh address, contents unchanged.
 *   -EINVAL  The root is not an internal POPCOUNT/PIGEON node without a
 *            co-located external list (sub-step-2 scope).
 *   another negative errno on a bail (not expected single-threaded).
 */
int _cds_ft_debug_cow_replace_root(struct cds_ft *ft)
{
	struct cds_ft_inode_flag *root = rcu_dereference(ft->root);
	struct cds_ft_inode_flag *root_prime;
	struct cds_ft_metadata *old_root_meta;
	struct ft_flip_txn *txn;
	struct ft_held_anchor marks[FT_ENTRY_PER_NODE + 1];
	unsigned int nr_marks = 0, i, ti;
	bool marks_consumed = false;
	enum urcu_txn_status st;
	int ret;

	if (!root || ft_node_flip_proxy(root))
		return -EINVAL;
	ti = ft_node_type(root);
	if (ft_types[ti].type_class != FT_POPCOUNT &&
			ft_types[ti].type_class != FT_PIGEON)
		return -EINVAL;
	old_root_meta = cds_ft_item_to_metadata(ft_node_ptr(root));
	if (old_root_meta->external_nodes)
		return -EINVAL;			/* sub-step-2 scope */

	txn = ft_flip_txn_create(ft);
	if (!txn)
		return -ENOMEM;
	/* PHASE B, STEP B6: the one door, at creation (see the rekey writer). */
	ft_flip_txn_arm_structural(ft, txn);

	/* The ROOT is its own anchor under every spacing: byte-depth 0. */
	ret = ft_rekey_cow_stop(ft, NULL, txn, root, 0, 0 /*cut*/,
			false /*ryw*/, NULL, &root_prime,
			marks, &nr_marks);
	if (ret) {
		ft_flip_txn_destroy(txn);	/* pre-commit bail: destroy caller-owned txn */
		goto sweep;
	}

	/* Forward publish ft->root: root -> root', under @root_lock like every
	 * other root writer (+1 reserved for the lock's release terminal). */
	if (!ft_flip_txn_reserve_extra(txn, 1 + FT_ROOT_LOCK_MAX_RECORDS)) {
		free_cds_ft_node_unpublished(ft, ft_node_ptr(root_prime));
		ft_flip_txn_destroy(txn);
		ret = -ENOMEM;
		goto sweep;
	}
	/*
	 * @root_prime becomes @ft's root: name the owner while it is still
	 * unpublished (the bail above frees it as such).
	 */
	cds_ft_item_to_metadata(ft_node_ptr(root_prime))->parent_word =
		ft_trie_parent(ft);
	(void) ft_flip_txn_lock_root(txn, NULL, (void **) &ft->root, root);
	ft_flip_txn_record_root(txn, (void **) &ft->root, root, root_prime);

	st = ft_flip_txn_commit(ft, txn);		/* consumes txn */
	if (st == URCU_TXN_STATUS_OK) {
		marks_consumed = true;	/* every mark released by its state edge */
		cds_ft_free_item_deferred(ft, old_root_meta);	/* old root freed after GP */
		ret = 0;
	} else {
		/*
		 * Abort: a peer won a raced slot.  The SW parks never
		 * flipped, so root_prime stayed unpublished.  Reclaim it.
		 * Its shared interior children are untouched and still owned
		 * by the live old root.
		 *
		 * This is unreachable under the contract here, which is
		 * single-writer, all-SW and pre-reserved: the commit is
		 * deterministically OK.  It is kept leak-free for a future
		 * concurrent reuse of this path.
		 */
		free_cds_ft_node_unpublished(ft, ft_node_ptr(root_prime));
		ret = -EAGAIN;
	}

sweep:
	/*
	 * The caller-owned release, on bail/abort paths ONLY.  A successful
	 * commit already released every mark through its state edge.  Clearing
	 * again would take a peer's fresh mark off a node that is LIVE and
	 * CLEAN by then.
	 */
	if (!marks_consumed)
		for (i = 0; i < nr_marks; i++)
			/*
			 * ☠ @txn_owned is skipped: ft_rekey_cow_stop registers
			 * its marks now, and registration TRANSFERS the clear to
			 * the txn's terminals.  ONE OWNER PER FENCE.
			 */
			if (!marks[i].shared && !marks[i].txn_owned) {
				/*
				 * STRICT: @marks_consumed above already
				 * excludes the committed case that the
				 * comment at sweep: describes.  So reaching
				 * here means that the op bailed and still
				 * holds the mark.  Asking the word instead
				 * is what took the peer's fresh mark that
				 * the comment warns about.
				 */
				ft_meta_lock_release(marks[i].lock);
				/*
				 * SCRUB only RELEASED-LIVE (finding A).  A
				 * TOMBSTONED word is a CONSUMED fence and must
				 * keep answering holds().  Dedupe-on-dead is
				 * the designed flow.  Taking a dead word
				 * hard-refuses forever (the exp-MW storm).
				 */
				if (!(CMM_LOAD_SHARED(marks[i].lock->state) &
						FT_STATE_TOMBSTONE))
					marks[i].shared = true;
			}
	return ret;
}

/*
 * TEST/DEBUG (coherent-rekey sub-step 3, NOT public API).
 *
 * Descend @key (converted to ordinal) and return the node flag AT that key as
 * an opaque address.  A test uses it to observe that a rekey moved the
 * moved-subtree top (S_top) to a FRESH address.  Return NULL if the key is
 * absent or if its path traverses a non-plain-internal node.
 */
/*
 * Does @flag (a value from _cds_ft_debug_child_at) carry a CO-LOCATED external
 * chain, i.e. a key ending exactly at this node's position?
 *
 * Test-only, for the same reason as the compressed query beside it.  A test
 * builds this shape to reach a branch gated on it.  The test must be able to
 * say that it built the shape.  Otherwise asserting the outcome alone passes
 * on any other refusal.
 */
int _cds_ft_debug_flag_has_external_chain(struct cds_ft *ft, void *flag)
{
	struct cds_ft_inode_flag *nf = (struct cds_ft_inode_flag *) flag;
	struct cds_ft_metadata *meta;

	if (!nf || ft_node_external(nf))
		return 0;
	nf = ft_resolve_flip_proxy(nf);
	if (ft_node_compressed(nf))
		return 0;	/* a compressed node never carries one */
	meta = ft_flag_to_metadata(ft, nf);
	return meta && meta->external_nodes ? 1 : 0;
}

/*
 * Is path compression compiled in?
 *
 * Test-only.  A test that builds a compressed shape must tell two cases
 * apart:
 *   - "this build cannot make one": skip;
 *   - "this build should have made one and did not": fail.
 * A bare structural check collapses the two.  It would skip silently through
 * a regression.
 */
int _cds_ft_debug_compress_enabled(void)
{
#ifdef FEATURE_FT_COMPRESS
	return 1;
#else
	return 0;
#endif
}

/*
 * Is the SKIP-COMPRESSED encoding compiled in?
 *
 * This is distinct from path compression: a build can keep compressed runs
 * and still spell every slot plainly.
 *
 * The legality of some shapes DEPENDS on skip mode.  For example, a one-child
 * keyless internal is canonical without it and illegal with it.  A test of
 * such a shape must ask this, not _cds_ft_debug_compress_enabled.  That one
 * answers a different question.  It would let such a test skip silently on
 * the wrong build.
 *
 * Test-only.  Ask the LIBRARY; do not reproduce this as a #ifdef in the test.
 * The test object and the library can be compiled with different flags.
 */
int _cds_ft_debug_skip_compressed_enabled(void)
{
#ifdef FEATURE_FT_SKIP_COMPRESSED
	return 1;
#else
	return 0;
#endif
}

/*
 * Is the in-place occupancy-bitmap tier compiled in?
 *
 * Test-only.  It must be ASKED OF THE LIBRARY, not reproduced as a test-side
 * #ifdef.  A test that maintains its own copy of a build flag reports on its
 * own copy.  A build whose library and tests disagree then reads as a pass.
 *
 * ☠ THE FLAG IS ONLY HALF THE CONDITION FOR THE BULK ARMS.  A graft's
 * reserve vouches ft_in_place_excl_ok(), which also requires an EXCLUSIVE
 * trie.  So a test that wants ft_store_at_graft_point_commit's in-place arm
 * must ALSO create an exclusive trie.  This is why an in-place build alone
 * never reached that arm.
 *
 * This answers for the INSERT tier (the graft reserve's).  The delete tier
 * has its own switch, FEATURE_FT_DELETE_IN_PLACE.  The point ops' tier
 * (ft_in_place_ok) is the flag alone.
 */
int _cds_ft_debug_in_place_enabled(void)
{
#ifdef FEATURE_FT_INSERT_IN_PLACE
	return 1;
#else
	return 0;
#endif
}

/*
 * Is @flag (a value from _cds_ft_debug_child_at) a COMPRESSED node?
 *
 * Test-only shape introspection.  A test builds a geometry to reach a
 * kind-specific branch.  It has to be able to say that it built it.  Without
 * this, asserting only the outcome passes whenever ANY other branch produces
 * the same outcome.
 */
int _cds_ft_debug_flag_is_compressed(void *flag)
{
	struct cds_ft_inode_flag *nf = (struct cds_ft_inode_flag *) flag;

	if (!nf)
		return 0;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(nf))
		return 1;
#endif
	return ft_node_compressed(nf) ? 1 : 0;
}

/*
 * Is @flag (a value from _cds_ft_debug_child_at) an EXTERNAL head?
 *
 * The SKIP test comes first and is load-bearing.  A skip pointer onto an
 * external leaf carries low tag bits 0, and ft_node_external() matches those
 * on the raw value.  Every kind-testing site in the trie uses the same
 * dispatch order.
 */
int _cds_ft_debug_flag_is_external(void *flag)
{
	struct cds_ft_inode_flag *nf = (struct cds_ft_inode_flag *) flag;

	if (!nf)
		return 0;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(nf))
		return 0;
#endif
	return ft_node_external(nf) ? 1 : 0;
}

void *_cds_ft_debug_child_at(struct cds_ft *ft, const uint8_t *key,
		size_t key_len)
{
	uint8_t ord[FT_MAX_KEY_LEN];
	struct ft_descent d;
	const uint8_t *ik = ord;

	if (key_len == 0 || key_len > FT_MAX_KEY_LEN)
		return NULL;
	ft_key_to_ordinals(ord, key, key_len, &ft->group->key_map);
	ft_descent_init(&d, ft);
	while (d.depth < key_len) {
		if (!d.nf || ft_node_external(d.nf) || ft_node_compressed(d.nf))
			return NULL;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(d.nf))
			return NULL;
#endif
		ft_descent_step(ft, &d, *(ik++));
	}
	return (void *) d.nf;
}



/*
 * TEST/DEBUG (coherent-rekey sub-step 3, NOT public API): the SIMPLEST
 * complete one-decide rekey-graft.
 *
 * Move the subtree "S_top" hanging at @src_key to the ABSENT @dst_key, as ONE
 * mixed SW/MW flip-txn:
 *   1. COW S_top to a fresh S_top' (SW re-parents + retire, locked);
 *   2. graft S_top' at the dst point (MW forward publish, dst parent
 *      unlocked);
 *   3. clear the src slot (MW detach, unlocked).
 * All three are recorded into one txn and committed once.  So a reader sees
 * the subtree at src XOR dst atomically.
 *
 * This is the analog of _cds_ft_debug_cow_replace_root for the full stitch.
 * It is scoped to the simplest shape, so it needs no cell / GLUE / merge
 * machinery:
 *   - S_top is a plain internal POPCOUNT/PIGEON node with no co-located
 *     external list (ft_rekey_cow_stop's sub-step-2 scope), hanging at
 *     @src_key.
 *   - The SRC JUNCTION BP (= S_top's parent) and the DST PARENT are DISTINCT
 *     and do not alias each other's parent (the shape gate below).  They MAY
 *     share a parent or not.  In the default, concurrent-safe build, EVERY
 *     popcount delete recompacts.  So BP is rebuilt on the removal and
 *     republished into its parent.  That parent is one of:
 *       - the node whose lock the graft's dst-parent recompaction already
 *         holds.  The lock is REUSED via @src_held_hint, not re-acquired.
 *       - a node that this detach acquires itself, guarded.
 *     Both are try-locks that abort rather than block, so their order is
 *     deadlock-free.
 *   - @dst_key is ABSENT.  It is reached by a NOSPLIT graft into a spare slot
 *     with append room (no compressed-divergence GLUE split).
 *   - The trie's ordered list may be ON.  The moved subtree's contiguous cell
 *     run unsplices from the src ordered position and re-splices at the dst
 *     position.  This is SIX recorded cell edges in the SAME commit.  They
 *     are all MW, so a peer's cell conflict aborts clean.  The dst splice
 *     position must be clear of the run's own ordered neighbourhood
 *     (adjacency guard below).
 *
 * structural_sw STAYS TRUE the whole txn.  These all hold their DLM node
 * locks:
 *   - cow_stop's S_top edges;
 *   - the graft's dst-parent recompaction;
 *   - the detach's BP recompaction.  BP's parent is held by the graft via
 *     @src_held_hint, or acquired here.
 * So every structural edge is legitimately SW.  The mixed engine still sorts
 * the (MW) count edges first.
 *
 * @src_key and @dst_key are APPLICATION keys (converted to ordinal
 * internally).  They are DISJOINT (neither a prefix of the other) and of
 * EQUAL length, on a FIXED-length group.  See the scope checks at the top of
 * the body for why each is required.
 *
 * CONCURRENCY.  Abort-clean: every bail leaves the trie byte-for-byte as
 * before.  This function is ONE ATTEMPT.  ft_rekey_graft_simple_locked wraps
 * it in the retry loop that owns the persistent handle and the escalation
 * turn.  That loop ABSORBS the transient codes:
 *   -EIO     the graft's up-front acquire lost a race;
 *   -EAGAIN  a cow_stop / detach acquire, an incoherently derived splice
 *            position, or the final commit aborted.
 * Callers therefore see only:
 *   0
 *   -EINVAL  the controlled shape above is not met;
 *   -ENOMEM  reserve.
 *
 * Retrying externally is NOT equivalent.  It was the old arrangement.  A
 * fresh handle per call never ages, so a contended writer never qualifies
 * for its turn.  The concurrent oracles exercise exactly that contract:
 * inv_rekey_graft_{disjoint,shared,coherent_readers}.
 *
 * One case is NOT yet multi-writer safe in general: a dst splice neighbour
 * INTERIOR to a peer's concurrently-moving run (see the splice position
 * validation below).  It is out of reach in the tested layouts.  Only the
 * per-FT move seqcount / relational coherence work closes it.
 */


/*
 * TEST-ONLY: strip @leaf's holder of every child, and leave the holder itself
 * WIRED where it is.  This is the "dead interior node" shape.
 *
 * Why a hook, and not a test that provokes the shape: the shape is the output
 * of a library DEFECT.  Once the defect is fixed, no sequence of public calls
 * produces it.  The reader code that copes with it then becomes untestable.
 *
 * It was untested.  A counter on ft-inequality.h's empty-subtree arm reads
 * ZERO across ft_unit and ft_inv in every list mode.  That arm decides what
 * an ordered walk does when it descends into a childless internal.  An
 * unexercised arm is where the next defect of this class hides.  The
 * compressed-parent one hid behind a comment claiming zero hits.
 *
 * The holder's external children are handed back through @out, not freed.
 * They are CALLER-OWNED nodes (the application allocated them), so the
 * library must not free them.  The test needs them to reclaim its own
 * memory.
 *
 * Returns 0 with *@out_n set.  Returns -1 if:
 *   - the holder is not an internal node;
 *   - @out is too small;
 *   - the trie is empty.
 *
 * Leaves the trie in a state cds_ft_verify REJECTS, on purpose.  Callers are
 * expected to walk it, assert whatever they are testing, and destroy it.
 */
int _cds_ft_debug_empty_holder(struct cds_ft *ft, struct cds_ft_node *leaf,
		struct cds_ft_node **out, unsigned int out_max,
		unsigned int *out_n)
{
	struct cds_ft_inode_flag *holder_flag;
	struct cds_ft_metadata *meta;
	unsigned int b, n = 0;

	if (!ft || !leaf || !out || !out_n)
		return -1;
	holder_flag = ft_node_holder(ft, leaf);
	if (!holder_flag || !ft_node_internal(holder_flag))
		return -1;
	meta = cds_ft_item_to_metadata(ft_node_ptr(holder_flag));

	/*
	 * Collect first, clear after.  On a popcount layout, the slot index is
	 * a rank over the occupancy bitmap.  A slot cleared mid-scan would
	 * change what the remaining ft_node_get_nth_skip calls see there.
	 */
	for (b = 0; b < 256; b++) {
		struct cds_ft_inode_flag **slot = NULL;
		struct cds_ft_inode_flag *child =
			ft_node_get_nth_skip(holder_flag, &slot, (uint8_t) b,
					FT_PF_NONE);

		if (!child || !slot)
			continue;
		if (!ft_node_external(child))
			return -1;	/* subtree, not a leaf: caller picked wrong */
		if (n >= out_max)
			return -1;
		out[n++] = (struct cds_ft_node *) ft_node_ptr(child);
	}
	for (b = 0; b < 256; b++) {
		struct cds_ft_inode_flag **slot = NULL;

		if (!ft_node_get_nth_skip(holder_flag, &slot, (uint8_t) b,
				FT_PF_NONE) || !slot)
			continue;
		rcu_assign_pointer(*slot, NULL);
	}
	if (meta->external_nodes) {
		if (n >= out_max)
			return -1;
		out[n++] = meta->external_nodes;
		rcu_assign_pointer(meta->external_nodes, NULL);
	}
	ft_meta_nr_child_set(meta, 0);
	*out_n = n;
	return 0;
}

#ifdef FT_ENABLE_TRACING
/*
 * Harness-facing violation hook (two-writer external-head campaign, 2026-09-04).
 *
 * A standalone reproducer cannot reach FT_TRACE_FREEZE.  A bare
 * `lttng snapshot record` from the harness is a fork+exec: MILLISECONDS.  In
 * that time the ring wraps past the very window the violation names (see the
 * FAST STOP note in fractal-trie-trace.h).  So route it through the library:
 * emit the violation FIRST, then freeze, stop and snapshot.
 *
 * It lives HERE, not in fractal-trie-alloc.c.  The question the trace has to
 * answer is "where is this node hanging".  The resolvers that answer it are
 * in this translation unit: ft_node_holder across the cell indirection, and
 * the metadata behind a tagged flag.  @node's own words say nothing on their
 * own.  A lost key's node is typically untouched.  It is the HOLDER's history
 * that carries the loss.
 *
 * Defensive by construction.  A lost key's holder may be retired.  So every
 * dereference past @node->prev is guarded and reports 0 rather than
 * faulting.  A fault here would take the snapshot with it.
 */
void cds_ft_debug_ext_violation(unsigned int kind, struct cds_ft *ft,
		struct cds_ft_node *node, uint64_t key0);
void cds_ft_debug_ext_violation(unsigned int kind, struct cds_ft *ft,
		struct cds_ft_node *node, uint64_t key0)
{
	void *prev = NULL;
	struct cds_ft_inode_flag *holder = NULL;
	struct cds_ft_node *hext = NULL;
	uintptr_t hstate = 0;
	int anc_tomb = -1, anc_root = -1;

	if (node) {
		prev = CMM_LOAD_SHARED(node->prev);
		holder = ft_node_holder(ft, node);
	}
	if (holder && !ft_node_external(holder)) {
		struct cds_ft_metadata *hm = ft_flag_to_metadata(ft, holder);

		if (hm)
			hstate = CMM_LOAD_SHARED(hm->state);
		/*
		 * ☠ THE HEAD IS NOT ALWAYS IN THE SAME FIELD.  A COMPRESSED
		 * holder keeps its single external child in cn->child;
		 * meta->external_nodes is legitimately NULL there.  Reading the
		 * metadata field unconditionally reported "the holder's head is
		 * empty" for every compressed holder.  That manufactured a
		 * whole SHAPE that does not exist: 1 of 4 rows in the first
		 * histogram.  Dispatch on the holder kind exactly as
		 * cds_ft_remove's own four-way branch does.
		 */
		if (ft_node_compressed(holder) || ft_node_skip_compressed(holder)) {
			struct cds_ft_compressed_node *cn =
				ft_node_skip_compressed(holder) ?
					ft_skip_to_compressed(ft, holder) :
					ft_compressed_node_ptr(holder);

			if (cn)
				hext = (struct cds_ft_node *)
					ft_node_ptr(ft_resolve_flip_proxy(
						CMM_LOAD_SHARED(cn->child)));
		} else if (hm) {
			hext = hm->external_nodes;
		}
	}
	/*
	 * @self ANCHORS THE ADDRESS SPACE.  item_retire and friends carry a raw
	 * __builtin_return_address.  ASLR makes that unresolvable after the
	 * process is gone.  So this function emits its own address.  The
	 * analysis can then recover the .so load base (self - nm offset) and
	 * turn every caller in the trace into a file:line.
	 */
	/*
	 * Bounded up-walk.  It runs at abort time on a structure that a peer
	 * may be mutating, so it must not fault and must not spin.  Every
	 * dereference is guarded and the hop count is capped at FT_MAX_DEPTH.
	 */
	if (holder && !ft_node_external(holder)) {
		struct cds_ft_inode_flag *nf = holder;
		int hops;

		for (hops = 0; hops <= FT_MAX_DEPTH; hops++) {
			struct cds_ft_metadata *m;

			if (!nf || ft_node_external(nf))
				break;
			m = ft_flag_to_metadata(ft, nf);
			if (!m)
				break;
			if (anc_tomb < 0 &&
			    (CMM_LOAD_SHARED(m->state) & FT_STATE_TOMBSTONE))
				anc_tomb = hops;
			if (ft_parent_is_trie(CMM_LOAD_SHARED(m->parent_word))) {
				anc_root = hops;
				break;
			}
			nf = ft_parent_node(CMM_LOAD_SHARED(m->parent_word));
		}
	}
	FT_TP(ext_violation, kind, node, holder, hext, (uint64_t) hstate,
		ft_dbg_rm_site, anc_tomb, anc_root,
		(const void *) &cds_ft_debug_ext_violation, key0);
	/*
	 * ALSO ON STDERR.  The shape of a loss is (holder tombstoned?, is the
	 * node still its head?, which refusal site?).  All three are right
	 * here, so a plain un-traced run can be classified too.
	 *
	 * That matters because the reproducer cannot go GREEN while any shape
	 * remains.  So the metric for a candidate fix is a SHAPE HISTOGRAM
	 * over many runs.  Paying for a flight-recorder session per sample
	 * would cost more than the samples are worth.
	 */
	fprintf(stderr, "EXTVIOL kind=%u node=%p prev=%p holder=%p hext=%p "
		"hstate=0x%lx rmsite=%u tomb=%d headis=%s anctomb=%d ancroot=%d "
		"stale_ext=%lu stale_det=%lu stale_orph=%lu fold=%lu alone=%lu rmstale=%lu\n",
		kind, (void *) node, prev, (void *) holder, (void *) hext,
		(unsigned long) hstate, ft_dbg_rm_site,
		(hstate & FT_STATE_TOMBSTONE) ? 1 : 0,
		hext == node ? "node" : (hext ? "other" : "null"),
		anc_tomb, anc_root,
		uatomic_load(&ft_dbg_plan_stale_ext, CMM_RELAXED),
		uatomic_load(&ft_dbg_plan_stale_detach, CMM_RELAXED),
		uatomic_load(&ft_dbg_orphan_walk_stale, CMM_RELAXED),
		uatomic_load(&ft_dbg_plan_stale_fold, CMM_RELAXED),
		uatomic_load(&ft_dbg_plan_stale_alone, CMM_RELAXED),
		uatomic_load(&ft_dbg_rm_stale_holder, CMM_RELAXED));
	ft_trace_capture();
}
#endif
