#if defined(FT_DEBUG_DEL_TOMB) || defined(FT_DEBUG_PAIR_STORE)
#include <execinfo.h>
#endif
// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-helpers.h
 *
 * Userspace RCU library - Fractal Trie: general helpers.
 *
 * This unit holds:
 *   - tag, node and metadata accessors;
 *   - key conversion and compare;
 *   - publish helpers;
 *   - flip-latch helpers;
 *   - ordinal-cell and skip-compressed primitives;
 *   - allocation glue.
 *
 * Implementation unit.  fractal-trie.c includes it once, in dependency
 * order, into a single translation unit.  This preserves cross-module
 * inlining.  It is not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-helpers.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * ★ WHAT MAKES THIS WRITE LEGAL?  Exactly one of three regimes covers every
 * writer of a shared word in this trie.  The regime is a FACT ABOUT THE SITE.
 * Until now the code carried that fact only in prose.  Naming it in the call
 * makes the answer local and checkable.  It no longer sits in a comment three
 * functions away.  The regimes are:
 *
 *   HIDDEN  The target is build-invisible: THIS op allocated it and has not
 *           published it yet.  No reader and no peer can reach it.  A plain
 *           store is legal because there is nobody to race.  ("An interior
 *           write into an unpublished body: nothing to record.")
 *   MW_CAS  The engine arbitrates the word.  The write is a transacted
 *           record.  Its expected-old makes a concurrent change lose.  No lock
 *           is held and none is needed.
 *   LOCKED  The op HOLDS the word's owner.  The owner is a per-node lock the
 *           op acquired, or the FT-wide writer lock.  Peers are excluded up
 *           front.
 *
 * ☞ THE DECLARATION IS TESTABLE, which is the point.  The audit does not
 * believe it.  A site that declares LOCKED goes through the witness ladder.
 * A miss there is a REAL violation: the code says it holds the owner and it
 * does not.  A HIDDEN site is bucketed as owing nothing.  So the annotation
 * documents the design AND fails loudly when the design drifts away from it.
 * This tree already uses that pattern when it writes an invariant as a rule
 * you can run.
 *
 * ☠ THE WORD CANNOT ANSWER THIS, ONLY THE CALLER CAN.  The tempting local test
 * is to compare what the store is about to overwrite.  It is wrong twice:
 *
 *   - The store is a bare rcu_assign_pointer.  A read before it is not atomic
 *     with it, so a peer may change the word in between.
 *   - A fresh node's parent word is NOT reliably NULL.  A cluster wires
 *     fresh-to-fresh, and recompact's reparent loop re-wires a node more than
 *     once.  So "overwrote something different" does not mean "live".
 *
 * At the sites that matter the caller already branches on exactly this fact:
 *
 *   - ft_node_recompact tests @pending_del_replace_fresh;
 *   - ft_split_compressed_insert stores the fresh top eagerly and PARKS the
 *     live old child.
 *
 * The knowledge exists.  It is simply not passed on.
 *
 * ☠ UNDECLARED IS THE ZERO VALUE ON PURPOSE.  An unconverted site keeps its own
 * bucket.  It is not attributed to a regime it may not belong to.  So the
 * undeclared count is the remaining work, in the open.  Never convert a site
 * by guessing.  A wrong declaration turns the audit into a confident wrong
 * zero.  That is worse than the honest "not yet classified" it replaced.
 *
 * Costs nothing: only the debug audit consumes the value, and the value
 * compiles out with it.
 */
enum ft_word_excl {
	FT_EXCL_UNDECLARED = 0,	/* not yet classified -- counted, never scored */
	FT_EXCL_HIDDEN,		/* unpublished: no reader, no peer, nothing to race */
	FT_EXCL_MW_CAS,		/* transacted: the record's expected-old arbitrates */
	FT_EXCL_LOCKED,		/* the op holds the owner (per-node or FT-wide) */
};

/*
 * THE RAW-PRODUCER CANARY TAG.  It is defined here and not beside the table in
 * ft-txn-hlist.h.  The producers it tags live in files included BEFORE that
 * one.  A macro cannot be forward declared, unlike the audit arms above.  The
 * single TU resolves the function.
 */
#ifdef FT_DEBUG_CHAIN_CANARY
static void ft_chain_canary_stamp_id(const char *fn, int line, void **slot,
		bool raw, unsigned char id);
# define FT_CHAIN_CANARY_RAW(slot, id)					\
	ft_chain_canary_stamp_id(__func__, __LINE__, (void **) (slot), true, (id))
#else
# define FT_CHAIN_CANARY_RAW(slot, id)	do { (void) (slot); } while (0)
#endif

#ifdef FT_DEBUG_CHAIN_HOLD
/*
 * The duplicate-chain hold audit lives in ft-mutation-helpers.h.  It needs the
 * per-thread hold ledger defined there.  That file is included AFTER this one.
 * Declare the head-word arm here and let the single TU resolve it.
 * ft_hlist_store_mw_at already uses the same arrangement for the audit's
 * coarse arm.
 */
static void ft_ch_audit_head_at(const char *fn, int line,
		const struct cds_ft *ft, struct cds_ft_node *head,
		struct cds_ft_inode_flag *owner_flag, enum ft_word_excl excl);
struct ft_flip_txn;
struct ft_lock_ctx;
/*
 * The node-body arm: a live node's own bitmap + child-slot words.
 *
 * ☞ IT TAKES ALL THREE WITNESSES.  Asking only ft_hold_trace_holds() here read
 * 3,956,849 "violations" on the refused path.  That witness DROPS its entry
 * the moment a release is recorded (@a5e1b0ce).  So a site that acquired and
 * already recorded its release reads EMPTY.  The registry and the wide ctx
 * live one frame up, in ft_node_set_nth_rec.  FT_CH_TXN_PARAM carries them
 * down.  The verdict is then a bracket and not one biased witness.
 */
static void ft_ch_audit_body_at(const char *fn, int line,
		const struct cds_ft *ft, struct cds_ft_metadata *owner,
		enum ft_word_excl excl, const struct ft_flip_txn *t,
		const struct ft_lock_ctx *ctx);
/*
 * Debug-only pass-through of the op's txn + lock ctx.  It mirrors
 * FT_BE_SITE_PARAM.  Release signatures are untouched, so this costs nothing
 * outside the audit build.
 */
# define FT_CH_TXN_PARAM	, const struct ft_flip_txn *dbg_ch_t,	\
				const struct ft_lock_ctx *dbg_ch_ctx
# define FT_CH_TXN_ARG(t, c)	, (t), (c)
/* Forward an already-received pair (cannot go through FT_CH_TXN_ARG: macro
 * arguments are split before expansion, so a 2-in-1 token is ONE argument). */
# define FT_CH_TXN_FWD		, dbg_ch_t, dbg_ch_ctx
/* No witness to offer: the caller builds a FRESH node (defer_parent), so the
 * audit buckets it HIDDEN and never asks. */
# define FT_CH_TXN_NONE		, NULL, NULL
# define FT_CH_TXN_USE		dbg_ch_t, dbg_ch_ctx
/* The metadata.parent_word arm (the table's FT-SLOT-3 row); same arrangement. */
static void ft_ch_audit_parent_at(const char *fn, int line,
		const struct cds_ft *ft, const struct cds_ft_metadata *child_meta,
		struct cds_ft_inode_flag *owner_flag, enum ft_word_excl excl);
# define ft_ch_audit_head(ft, head, owner)				\
	ft_ch_audit_head_at(__func__, __LINE__, (ft), (head), (owner),	\
		FT_EXCL_UNDECLARED)
#else
# define ft_ch_audit_head(ft, head, owner)	do { } while (0)
/* The _at spelling is called directly by ft_set_parent_at, which forwards its
 * caller's location -- so it needs a no-op too, or a non-debug build breaks. */
/*
 * ☠ VARIADIC ON PURPOSE.  These take a pass-through pair that expands from ONE
 * macro token (FT_CH_TXN_USE -> "NULL, NULL").  A function call expands, then
 * splits.  A fixed-arity macro splits BEFORE expanding and reports "requires
 * 7 arguments, but only 6 given".  A variadic no-op accepts any arity.
 */
# define ft_ch_audit_head_at(...)	do { } while (0)
# define ft_ch_audit_parent_at(...)	do { } while (0)
# define ft_ch_audit_body_at(...)	do { } while (0)
# define FT_CH_TXN_PARAM
# define FT_CH_TXN_ARG(t, c)
# define FT_CH_TXN_FWD
# define FT_CH_TXN_NONE
# define FT_CH_TXN_USE		NULL, NULL
#endif

static inline __attribute__((unused))
void static_array_size_check(void)
{
	CAA_BUILD_BUG_ON(CAA_ARRAY_SIZE(ft_types) < FT_TYPE_MAX_NR);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	/*
	 * parent_slot_offset is 8 bits and stores byte_offset / sizeof(void *).
	 * The largest node must fit.  It is the pigeon node: 2^11 = 2048 bytes.
	 * 2048 / sizeof(void *) = 256 slots, so the max index is 255.  This is
	 * only enabled on 64-bit architectures.  There sizeof(void *) == 8 and
	 * the quotient is exactly 256.
	 */
	CAA_BUILD_BUG_ON((1U << 11) / sizeof(void *) > 256);
#endif
	/*
	 * Metadata packed bitfield must fit in a uint32_t.
	 * Layout: nr_child(9) + [parent_slot_offset(8)] + alloc_index
	 *         (near: FT_ALLOC_INDEX_BITS + 3; far: a separate uint32_t).
	 */
	CAA_BUILD_BUG_ON(9
#ifndef FT_FAR_METADATA
		/* far-metadata stores alloc_index as its own uint32_t. */
		+ (FT_ALLOC_INDEX_BITS + 3)
#endif
#ifdef FEATURE_FT_SKIP_COMPRESSED
		+ 8
#endif
		> 32);
}

/*
 * Reader-side helpers for the cds_ft_node.next removal tombstone (bit 1,
 * see CDS_FT_NODE_REMOVED_FLAG).  These run under the writer mutex, or under
 * the RCU read lock on the chain-walk side.  A plain masked load is therefore
 * sufficient.  Readers use cds_ft_node_next_rcu() instead.
 *
 *   ft_node_next        masked successor (the actual chain link)
 *   ft_node_is_removed  has @node been removed from the trie?
 *
 * SETTING the tombstone is a COMMITTED flip edge.  The freed node's own next
 * is a word that a concurrent duplicate-append CASes.  So the mark rides the
 * descriptor protocol like every other reader-visible store.  See
 * ft_node_mark_removed_flip / ft_chain_mark_removed_flip in
 * ft-mutation-helpers.h, and doc/design/mcas-multiwriter-readiness.md §4
 * refinement-1 site 2.
 */
/*
 * May a mutation write these words of a LIVE node in place?
 *
 *   - a child pointer;
 *   - the occupancy bitmap;
 *   - nr_child.
 *
 * The alternative is to route through a whole-node recompact.
 *
 * There are TWO predicates because the tree has two in-place tiers.  Each
 * tier has its own safety argument.  A site must say which one it stands on.
 *
 * ft_in_place_ok(ft) is the POINT-OP tier.
 *
 *   The build flag is the OPT-IN.  The SAFETY CONDITION is the CALLER's: it
 *   must HOLD the node before it writes.  What it holds depends on the
 *   strategy:
 *
 *     - FINE, per-node spacing: the node's own DLM lock;
 *     - FINE, exponential or root-only spacing: the node's anchor;
 *     - COARSE: the FT-wide writer lock, which every point op already holds.
 *
 *   Under FINE the lock is taken BEFORE the reserve.  The same commit that
 *   publishes the edge releases it (ft_attach_node).
 *
 *   ☐ The delete side (ft_detach_node's in-place arm) never stores raw at
 *   all -- its slot NULL and nr_child-- are RECORDS in the commit, arbitrated
 *   on the holder's state word -- but the point removes still vouch only the
 *   exclusive tier below: one op at a time, and the delete's own validation
 *   step is next.
 *
 *   The predicate answers true on EVERY trie type: shared or exclusive,
 *   coarse or fine.  So it is the build flag alone.  The caller's vouch is
 *   what a site passes down as @in_place.
 *
 * ft_in_place_excl_ok(ft) is the legacy EXCLUSIVE-ONLY tier.
 *
 *   It serves the sites that have NOT been converted to lock-before-write:
 *
 *     - the bulk reserves (a graft's / rekey's dst attach parent, ft-graft.h);
 *     - the build-path wrapper ft_node_set_nth;
 *     - the bulk detaches.
 *
 *   cds_ft_attr_set_exclusive declares "single-writer, no concurrent
 *   readers".  That is what makes an unlocked in-place store sound there
 *   (mcas-multiwriter-readiness.md §5.2).
 *
 *   It ALSO keeps the same-trie move's reader-coherence witness honest.  A
 *   rekey's dst attach parent must RELOCATE on a shared trie.  Only then does
 *   the address fold of ft_lookup_two_descents perturb
 *   (ft-reintroduce-in-place-mutations.md §3).  This predicate is false on a
 *   shared trie.
 *
 * WHY THE POINT-OP TIER IS SOUND AGAINST READERS: an in-place mutation NEVER
 * REMOVES A BITMAP BIT, so no existing entry's rank moves.
 *
 *   - The insert refuses (-ERANGE -> recompact) unless the new bit is strictly
 *     the highest.  It publishes the slot release-first and the gating bitmap
 *     bit last.
 *   - The delete leaves the bit STICKY and only NULLs the slot.  Every reader
 *     already treats a set bit over a NULL slot as "not present".  A reserved
 *     byte reads as the same hole between its reserve and its commit.
 *
 * Concurrent readers were supported with exactly this protocol before the
 * MW-with-CAS model withdrew it.  The MW model needed a whole-node
 * replacement, so that one CAS could arbitrate every word.  The DLM locks now
 * provide the same by exclusion.
 *
 * PER TIER.  The insert tier (FEATURE_FT_INSERT_IN_PLACE) and the delete tier
 * (FEATURE_FT_DELETE_IN_PLACE) are separate build switches.  Each has its own
 * predicate.  A site that EDITS in place asks the tier of the edit it makes.
 *
 * ft_in_place_ok / ft_in_place_excl_ok answer "EITHER tier".  They are for the
 * sites that refuse or re-route a shape (the same-trie rekey's gates).  Such
 * a site does so because an in-place tier would edit a node where it stands.
 * It must refuse as soon as either tier could.
 *
 * With both tiers compiled out (-DNO_FEATURE_FT_{INSERT,DELETE}_IN_PLACE)
 * every predicate answers no and every caller recompacts.
 */
/*
 * THE SAME-TRIE REKEY RUNS WITHOUT THE IN-PLACE TIERS, on the thread running it
 * (ft_rekey_dispatch holds this non-zero).
 *
 * The rekey's folds assume that every edit is a RECORD in the descriptor or a
 * build-invisible copy.  Its gates refuse a shape outright wherever an
 * in-place tier could edit a node where it stands (ft_in_place_excl_ok, an
 * exclusive trie's bulk tier).  So with a tier compiled in, an EXCLUSIVE
 * trie's rekey refused moves that the same build without the tier serves.
 * This was measured with test_rekey_exclusive_drain_after_loss on nocompress,
 * with either tier.
 *
 * A bulk op already pays grace periods, so the in-place stores buy it nothing.
 *
 * The counter is thread-local.  An exclusive trie has one writer, and a shared
 * trie never reaches the exclusive tier.  So no other op can observe the
 * difference.
 */
static __thread unsigned int ft_tls_in_place_off;

static inline
bool ft_in_place_insert_ok(const struct cds_ft *ft)
{
	(void) ft;
#ifdef FEATURE_FT_INSERT_IN_PLACE
	return !ft_tls_in_place_off;
#else
	return false;
#endif
}

static inline
bool ft_in_place_delete_ok(const struct cds_ft *ft)
{
	(void) ft;
#ifdef FEATURE_FT_DELETE_IN_PLACE
	return !ft_tls_in_place_off;
#else
	return false;
#endif
}

static inline
bool ft_in_place_ok(const struct cds_ft *ft)
{
	return ft_in_place_insert_ok(ft) || ft_in_place_delete_ok(ft);
}

static inline
bool ft_in_place_insert_excl_ok(const struct cds_ft *ft)
{
	return ft_in_place_insert_ok(ft) && ft->exclusive;
}

static inline
bool ft_in_place_delete_excl_ok(const struct cds_ft *ft)
{
	return ft_in_place_delete_ok(ft) && ft->exclusive;
}

static inline
bool ft_in_place_excl_ok(const struct cds_ft *ft)
{
	return ft_in_place_ok(ft) && ft->exclusive;
}

static inline
struct cds_ft_node *ft_node_next(const struct cds_ft_node *node)
{
	return (struct cds_ft_node *) ((uintptr_t) node->next &
			~CDS_FT_NODE_REMOVED_FLAG);
}

static inline
bool ft_node_is_removed(const struct cds_ft_node *node)
{
	return ((uintptr_t) node->next & CDS_FT_NODE_REMOVED_FLAG) != 0;
}

/*
 * Iterate through duplicates returned by cds_ft_lookup*()
 * Receives a struct cds_ft_node * as parameter, which is used as start
 * of duplicate list and loop cursor.
 *
 * ☠ RESOLVES THE ENGINE PROXY, and must.  Bit 0 of cds_ft_node.next is the
 * transactional engine's in-band proxy tag.  It is parked while a concurrent
 * commit is in flight on this chain.  CDS_FT_NODE_TXN_PROXY_TAG's own header
 * describes exactly this.
 *
 * ft_node_next() masks ONLY the removal tombstone (bit 1).  A walk built on it
 * hands the raw PROXY back as if it were a successor.  The next hop then
 * dereferences a DESCRIPTOR as a node.
 *
 * MEASURED: inv_concurrent_same_key_removes SEGVs 5/5 in _cds_ft_insert's
 * "find last duplicate" walk.  The fault is on a value read out of a
 * descriptor and not out of a chain (1 << 57, no node there).  It happens the
 * moment the in-place delete tier makes such a commit overlap the walk.
 *
 * cds_ft_node_next_rcu() is the accessor that answers this correctly.  It
 * resolves a parked proxy to the committed successor, then strips the
 * tombstone.  The PUBLIC cds_ft_for_each_duplicate_rcu() has always used it.
 * ft_node_next() stays for the single-hop liveness tests.  There the question
 * is about THIS node's own word and not about a successor to follow.
 */
#define cds_ft_for_each_duplicate(pos)				\
       for (; (pos) != NULL; (pos) = cds_ft_node_next_rcu(pos))

enum ft_recompact {
	FT_RECOMPACT_ADD_SAME,
	FT_RECOMPACT_ADD_NEXT,
	FT_RECOMPACT_DEL,
	/*
	 * Pure relocation.  The node keeps its type and child set.  It is
	 * copied verbatim into a fresh allocation, so it gets a new address.
	 * Its children are reparented.  It is republished into the parent
	 * slot, and into the skip slot via the shared publish path.
	 * Used by cds_ft_compact() to defragment the node arenas.
	 */
	FT_RECOMPACT_RELOCATE,
};

enum ft_lookup_inequality {
	FT_LOOKUP_GE,
	FT_LOOKUP_LE,
	FT_LOOKUP_GT,
	FT_LOOKUP_LT,
};

enum ft_lookup_limit {
	FT_LOOKUP_LIMIT_NONE,
	FT_LOOKUP_LIMIT_FIRST,
	FT_LOOKUP_LIMIT_LAST,
};

enum ft_direction {
	FT_LEFT,
	FT_RIGHT,
	FT_LEFTMOST,
	FT_RIGHTMOST,
};

/*
 * Fractal Trie iterator object. Can be used to keep backtracking state
 * across API calls. Path use for backtracking requires to keep RCU
 * read-side lock held across calls.
 *
 * The iterator lifetime is bound to the Trie. The Trie must not be
 * destroyed while iterators to that trie exist.
 *
 * The @prefix_len is the length of the key prefix within the key for
 * traversal under a given key prefix. Iterate over the entire Trie when
 * @prefix_len=0.
 */
struct cds_ft_iter {
	struct cds_ft *ft;		/* Point to the associated Fractal Trie. */
	struct cds_ft_node *node;	/* Current external node. */
	size_t path_len;		/* Key-path length of the cached position. */
	size_t key_len;			/* Key length of the current node. */
	size_t prefix_len;		/* Key prefix length. */
	enum cds_ft_status status;	/* Iteration status. */
	enum cds_ft_iter_cache_mode cache_mode;	/* Position-reuse mode (CACHED/UNCACHED). */
	bool cache_valid;		/* Whether the cached position is valid. */
	/*
	 * Byte offset within the @data buffer at which the current-position key
	 * begins.
	 *
	 *   - 0 for a descent / set_key key, which is filled at the front.
	 *   - (max_key_len - key_len) for the structural up-walk, which fills
	 *     the key at the TAIL.
	 *
	 * With this offset ft_iter_read_key returns the right pointer even
	 * after the cached position is invalidated (bind / UNCACHED).  No copy
	 * is needed to normalize the key to the front.
	 */
	size_t key_off;

	/*
	 * Ordinal-cell walk cursor.
	 *
	 * @ord_cell caches the cell of the current head.  cds_ft_next /
	 * cds_ft_prev then advance via cell->ord_next/prev without re-loading
	 * the head's leaf each step.
	 *
	 * @ord_cell_node records the node the cell was cached for.  The cache
	 * is honoured only while @ord_cell_node == iter->node.  A point lookup
	 * or a descent that re-seeded iter->node leaves a mismatch.  The first
	 * step then re-enters the walk via iter->node->prev.  That is the one
	 * leaf touch per walk entry.
	 *
	 * No stale cell is dereferenced: validity is a node-pointer compare,
	 * not a cell read.
	 */
	struct ft_ord_cell *ord_cell;
	struct cds_ft_node *ord_cell_node;

	/*
	 * CARRIED POSITION KEY (in-trie move coherence).  When @pos_key_node ==
	 * @node, iter_key() holds a key at offset 0, of length @key_len.  It is
	 * the key this position had when the last coherent step CONFIRMED it.
	 * It is spelled as that step's own traversal spelled it.
	 *
	 * A continuation step taken while a move is in flight is defined against
	 * that KEY and re-descends from it.  It does not hop the ordered-list
	 * cells.  A move rewrites the moved run's outer cell links IN PLACE.  A
	 * walker parked on one hops into the run's new neighbourhood and skips
	 * every key in between.  No reader can detect that, because nothing it
	 * can observe changed address.  The tree path CAN be detected: the move
	 * COWs the moved subtree's top.  The two-pass rests on that.
	 *
	 * The node pointer makes the carried key SELF-VALIDATING (usable only
	 * while it still describes @node), so no other path has to clear it.
	 */
	struct cds_ft_node *pos_key_node;

#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH
	struct urcu_gp_poll_state gp_state;	/* GP snapshot when path was populated. */
	bool gp_state_valid;			/* Whether gp_state holds a meaningful value. */
#endif

	/*
	 * Trailing buffer holding the ordinal key bytes of the current
	 * iterator position.  The going-up backtrack recovers per-level
	 * nodes from the live parent chain, so no path-node array is kept.
	 *
	 * Flexible array member, pointer-aligned.
	 */
	char data[] __attribute__((__aligned__(sizeof(struct cds_ft_inode_flag *))));
};

/* Start of the uint8_t key array. */
#define iter_key(iter) \
	((uint8_t *)((iter)->data))

/*
 * Validate the iterator-based lookup contract: @ft must be the trie the
 * iterator was created for (cds_ft_iter_create).  The descent uses @ft while
 * key handling uses iter->ft, so passing a different trie mixes their key
 * mappings and produces undefined results.  Debug-only; compiled out under
 * NDEBUG.
 */
static inline
void ft_iter_assert_bound(const struct cds_ft *ft __attribute__((unused)),
		const struct cds_ft_iter *iter __attribute__((unused)))
{
	assert(ft == iter->ft);
}

/*
 * Debug helpers for detecting stale cached iterator paths.
 *
 * Three entry-point roles mirror the rculfhash pattern:
 *
 *  iter_debug_path_snapshot() -- unconditionally captures a fresh
 *      grace-period poll state.  Called at the entry of every
 *      fresh-population operation (lookup, longest-match lookup, and
 *      the slow-path / early-exit branches of inequality lookup).
 *      It always overwrites the snapshot.  So an iterator that is
 *      reused across RCU read-side critical sections gets a current
 *      baseline.  This prevents false positives on the next check.
 *
 *  iter_debug_path_check() -- polls the existing snapshot.  Called at
 *      continuation entry points that consume a previously populated
 *      cached path (inequality fast-path, replace, remove).  The check
 *      aborts if a full grace period has elapsed since the snapshot.
 *      In that case the RCU read-side lock must have been dropped, and
 *      the cached pointers may reference freed memory.
 *
 *  iter_debug_path_update() -- invalidates the snapshot when the path
 *      becomes invalid (node not found / end of traversal).  It never
 *      captures a new snapshot.  The one taken at the operation's entry
 *      point persists as long as the path remains valid.  This gives a
 *      tighter detection window.
 *
 *  iter_debug_path_clear() -- unconditionally resets the snapshot
 *      validity.  Used by iter_auto_invalidate_cache() and by
 *      operations that structurally modify the trie (replace, remove),
 *      after which the cached path is stale regardless of RCU state.
 */
#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH

/*
 * Unconditionally capture a fresh grace-period snapshot.  Called at
 * the entry of fresh-population operations so that any prior stale
 * state left by iterator reuse is replaced.
 */
static inline
void iter_debug_path_snapshot(struct cds_ft_iter *iter)
{
	const struct rcu_flavor_struct *flavor = iter->ft->group->flavor;

	iter->gp_state = flavor->update_start_poll_synchronize_rcu();
	iter->gp_state_valid = true;
}

/*
 * Validate that the RCU read-side lock has been held continuously
 * since the snapshot was captured.  Called at continuation entry
 * points before reusing a cached path.
 */
static inline
void iter_debug_path_check(const struct cds_ft_iter *iter)
{
	const struct rcu_flavor_struct *flavor = iter->ft->group->flavor;

	if (iter->cache_mode != CDS_FT_ITER_CACHED)
		return;
	if (!iter->cache_valid)
		return;
	if (!iter->gp_state_valid)
		return;
	if (caa_unlikely(flavor->update_poll_state_synchronize_rcu(
				iter->gp_state))) {
		fprintf(stderr,
			"[Fatal] Fractal Trie: cached iterator path "
			"used after a grace period elapsed (RCU "
			"read-side lock was likely dropped). "
			"%s:%d\n", __FILE__, __LINE__);
		abort();
	}
}

/*
 * Update the snapshot validity after populating the iterator.
 *
 * When the path is no longer valid (node not found or end of traversal),
 * clear the snapshot.  iter_debug_path_check then detects any later
 * misuse.
 *
 * When the path is valid, keep the grace-period snapshot that
 * iter_debug_path_snapshot captured at the operation's entry point.  It
 * remains current because the RCU read-side lock must be held
 * continuously.
 */
static inline
void iter_debug_path_update(struct cds_ft_iter *iter)
{
	if (!iter->cache_valid)
		iter->gp_state_valid = false;
}

static inline
void iter_debug_path_clear(struct cds_ft_iter *iter)
{
	iter->gp_state_valid = false;
}
#else
static inline
void iter_debug_path_snapshot(struct cds_ft_iter *iter __attribute__((unused)))
{
}

static inline
void iter_debug_path_check(const struct cds_ft_iter *iter __attribute__((unused)))
{
}

static inline
void iter_debug_path_update(struct cds_ft_iter *iter __attribute__((unused)))
{
}

static inline
void iter_debug_path_clear(struct cds_ft_iter *iter __attribute__((unused)))
{
}
#endif

/*
 * Snapshot the iterator's current result key into the iterator's own buffer.
 * Do so only when that key is a live reference into the matched leaf (a
 * lazy-ref ordinal-cell group).  A later re-descent then reads a stable key
 * and not the soon-to-be-reclaimed leaf.
 *
 * This is a no-op for groups whose key is already a value in iter_key(iter).
 * Either the descent filled it, or the group is not an ordered-list group.
 * The next re-descent uses that buffer directly.
 *
 * Defined after ft_speculative_keycopy_unconditional.
 */
static inline void ft_iter_materialize_key(struct cds_ft_iter *iter);
/* Defined in ft-iter.h: is the key a reference INTO @iter->node's leaf? */
static inline bool ft_iter_key_referenced(const struct cds_ft_iter *iter);

/*
 * Drop the cached POSITION while preserving the KEY, for a site that is about
 * to re-descend from that key.
 */
static inline
void ft_iter_drop_position_keep_key(struct cds_ft_iter *iter)
{
	/*
	 * DROP THE CACHED POSITION, KEEP THE KEY.
	 *
	 * ☠ A BARE `cache_valid = false` CAN DESTROY THE KEY.  A keycopy trie
	 * has an ordered list, a speculative_key_offset and an identity key
	 * map.  On such a trie the key is not stored in the iterator at all:
	 *
	 *   - ft_iter_key_referenced() is true;
	 *   - ft_iter_read_key() returns @iter->node + speculative_key_offset.
	 *
	 * That predicate requires @cache_valid.  Clearing it makes read_key
	 * fall through to iter_key(iter), a buffer nothing ever wrote.  The key
	 * then reads back as ZEROS.  The same applies to the eager ordered-list
	 * up-walk arm, which also tests @cache_valid.
	 *
	 * So any site that drops the position INTENDING to re-descend from the
	 * key must materialize first.
	 *
	 * MEASURED: cds_ft_remove_all's exponential retriable bail dropped the
	 * cache "so the next attempt re-seeds through a fresh lookup".  The
	 * next attempt re-seeded from key 0.  It reported NOT_FOUND for a key
	 * that was present and reachable (inv_compact_keycopy_terminates, key
	 * 248 of 2000, exponential only).
	 *
	 * iter_auto_invalidate_cache does the same materialize-then-clear for
	 * the UNCACHED contract.  The reason is identical, so the order is
	 * identical.
	 */
	/*
	 * ☠ ONLY THE LEAF-REFERENCED KEY IS MATERIALIZED.  The gate is not
	 * caution.  It is the difference between reading a live leaf and
	 * WALKING A DETACHED SUBTREE.  ft_iter_read_key has two lazy arms:
	 *
	 *  - key REFERENCED (this one): a single load from @iter->node at
	 *    @speculative_key_offset.  The node is alive here even after a
	 *    detach, because the caller is handed it to free.  So the read is
	 *    sound.  It is also the only arm whose key the cache drop would
	 *    destroy.
	 *  - the eager ordered-list arm: an O(depth) PARENT UP-WALK
	 *    (ft_rebuild_key_upwalk).  Callers reach this helper AFTER the
	 *    structure changed.  An up-walk from a node the op has just
	 *    unlinked runs off an external node.  This helper used to
	 *    materialize unconditionally.  MEASURED at PER-NODE spacing:
	 *    ft_unit test 187 hit a SEGV on release, and
	 *    `Assertion !(nf) || !ft_node_external(nf)` on --enable-rcu-debug.
	 *
	 * The second arm needs no up-walk here.  Its key was already read
	 * (that is how the caller knows it), so it already sits in the buffer.
	 * But it sits at the TAIL, at @key_off.  The re-descent reads its
	 * search key from the FRONT, so move it there.  Left at the tail, the
	 * retry searched other bytes.  It reported NOT_FOUND for a present key
	 * after a contended remove_all on a variable-length list-on trie.
	 */
	if (ft_iter_key_referenced(iter)) {
		ft_iter_materialize_key(iter);
	} else if (iter->key_off &&
			iter->key_len <= iter->ft->group->max_key_len -
				iter->key_off) {
		memmove(iter_key(iter), iter_key(iter) + iter->key_off,
			iter->key_len);
		iter->key_off = 0;
	}
	iter->cache_valid = false;
	iter_debug_path_clear(iter);
	iter->path_len = 0;
}

/*
 * Discard the cached position if the iterator is in uncached mode.
 * Called at the end of each public iterator-based operation.
 * Preserves iter->node so the caller can read the result.
 */
static inline
void iter_auto_invalidate_cache(struct cds_ft_iter *iter)
{
	/*
	 * ☞ AND UNCACHED FOR EVERYONE WHILE A MOVE IS IN FLIGHT (MATHIEU).  In the
	 * rekey-coherent double-descent mode (ft_move_active) a cached position
	 * can straddle the move.  It can then name a place where its key no
	 * longer is.  The key is the absolute truth of the iterator's position.
	 * So keep the key and drop the position.  The next op re-descends by
	 * key, coherently.  The gate's grace period drained every section older
	 * than the flag, so no cache older than the move survives into it.
	 */
	if (iter->cache_mode == CDS_FT_ITER_UNCACHED ||
			ft_move_active(iter->ft)) {
		/*
		 * Materialize a live leaf-referenced key BEFORE clearing, so the
		 * next uncached re-descent reads the saved key, not a stale leaf.
		 */
		ft_iter_materialize_key(iter);
		iter->cache_valid = false;
		iter->path_len = 0;
		iter_debug_path_clear(iter);
		/* The cached cell too: see cds_ft_iter_bind_key. */
		iter->ord_cell = NULL;
		iter->ord_cell_node = NULL;
	}
}

static
size_t ft_key_len(const struct cds_ft *ft, size_t key_len)
{
	struct cds_ft_group *ft_group = ft->group;

	if (key_len == CDS_FT_LEN_DEFAULT) {
		if (ft_group->key_len == CDS_FT_LEN_VARIABLE)
			return CDS_FT_LEN_ERROR;
		return ft_group->key_len;
	}
	/* Validate that explicit and implicit key lengths match for fixed length Fractal Trie. */
	if (ft_group->key_len != CDS_FT_LEN_VARIABLE && key_len != ft_group->key_len)
		return CDS_FT_LEN_ERROR;
	return key_len;
}

uint64_t cds_ft_key_to_u64(const struct cds_ft *ft, const uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	union {
		uint64_t v64;
		uint8_t array[8];
	} u;

	if (key_len == CDS_FT_LEN_ERROR || key_len > 8)
		return 0;
	u.v64 = 0;
	/* Copy len LSB. */
	memcpy(u.array + sizeof(u.array) - key_len , key, key_len);
	/* Big endian to host endianness. */
	return be64toh(u.v64);
}

void cds_ft_u64_to_key(const struct cds_ft *ft, uint64_t v, uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	union {
		uint64_t v64;
		uint8_t array[8];
	} u;

	if (key_len == CDS_FT_LEN_ERROR || key_len > 8)
		return;
	/* Host endianness to big endian. */
	u.v64 = htobe64(v);
	/* Copy len LSB. */
	memcpy(key, u.array + sizeof(u.array) - key_len , key_len);
}

uint32_t cds_ft_key_to_u32(const struct cds_ft *ft, const uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	union {
		uint32_t v32;
		uint8_t array[4];
	} u;

	if (key_len == CDS_FT_LEN_ERROR || key_len > 4)
		return 0;
	u.v32 = 0;
	/* Copy len LSB. */
	memcpy(u.array + sizeof(u.array) - key_len , key, key_len);
	/* Big endian to host endianness. */
	return be32toh(u.v32);
}

void cds_ft_u32_to_key(const struct cds_ft *ft, uint32_t v, uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	union {
		uint32_t v32;
		uint8_t array[4];
	} u;

	if (key_len == CDS_FT_LEN_ERROR || key_len > 4)
		return;
	/* Host endianness to big endian. */
	u.v32 = htobe32(v);
	/* Copy len LSB. */
	memcpy(key, u.array + sizeof(u.array) - key_len , key_len);
}

/*
 * Signed integer key helpers.
 *
 * Signed integers need a sign-bit flip: an XOR with the MSB of the
 * key-width value.  With it, the big-endian byte ordering used by the
 * Fractal Trie preserves the natural signed ordering.
 *
 * When the key is the full width of the integer type (e.g. 8 bytes
 * for int64_t), the mapping is:
 *
 *   INT64_MIN  -> 0x0000000000000000   (sorts first)
 *   -1         -> 0x7FFFFFFFFFFFFFFF
 *    0         -> 0x8000000000000000
 *   INT64_MAX  -> 0xFFFFFFFFFFFFFFFF   (sorts last)
 *
 * The same principle applies to 32-bit signed integers.
 *
 * The key may be narrower than the integer type.  An example is a
 * 2-byte key that represents a signed 16-bit range within a 64-bit
 * integer.  The sign bit is then at position (key_len * 8 - 1), not at
 * the MSB of the full integer.  The key-to-integer direction therefore
 * sign-extends
 * from the key's MSB to fill the integer.
 */

int64_t cds_ft_key_to_s64(const struct cds_ft *ft, const uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	unsigned int shift;
	uint64_t u;

	if (key_len == 0 || key_len > 8)
		return 0;
	u = cds_ft_key_to_u64(ft, key, _key_len);
	shift = key_len * 8;
	/* Flip sign bit (MSB of key-width value) to recover signed encoding. */
	u ^= 1ULL << (shift - 1);
	/* Sign-extend from key width to 64 bits. */
	if (shift < 64) {
		uint64_t sign_bit = 1ULL << (shift - 1);

		if (u & sign_bit)
			u |= ~((1ULL << shift) - 1);
	}
	return (int64_t) u;
}

void cds_ft_s64_to_key(const struct cds_ft *ft, int64_t v, uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	unsigned int shift;

	if (key_len == 0 || key_len > 8)
		return;
	shift = key_len * 8;
	/* Flip sign bit so that negative values sort before positive. */
	cds_ft_u64_to_key(ft, (uint64_t) v ^ ( 1ULL << (shift - 1)), key, _key_len);
}

int32_t cds_ft_key_to_s32(const struct cds_ft *ft, const uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	unsigned int shift;
	uint32_t u;

	if (key_len == 0 || key_len > 4)
		return 0;
	u = cds_ft_key_to_u32(ft, key, _key_len);
	shift = key_len * 8;
	/* Flip sign bit (MSB of key-width value) to recover signed encoding. */
	u ^= 1U << (shift - 1);
	/* Sign-extend from key width to 32 bits. */
	if (shift < 32) {
		uint32_t sign_bit = 1U << (shift - 1);

		if (u & sign_bit)
			u |= ~((1U << shift) - 1);
	}
	return (int32_t) u;
}

void cds_ft_s32_to_key(const struct cds_ft *ft, int32_t v, uint8_t *key,
		size_t _key_len)
{
	size_t key_len = ft_key_len(ft, _key_len);
	unsigned int shift;

	if (key_len == 0 || key_len > 4)
		return;
	shift = key_len * 8;
	/* Flip sign bit so that negative values sort before positive. */
	cds_ft_u32_to_key(ft, (uint32_t) v ^ (1U << (shift - 1)), key, _key_len);
}

static inline_lookup
uint8_t key_to_ordinal(uint8_t key,
		const struct cds_ft_key_map *km)
{
	if (caa_likely(km->identity))
		return key;
	return km->key_to_ordinal[key];
}

static inline_lookup
uint8_t ordinal_to_key(const struct cds_ft *ft, uint8_t ordinal)
{
	if (caa_likely(ft->group->key_map.identity))
		return ordinal;
	return ft->group->key_map.ordinal_to_key[ordinal];
}

/*
 * Bulk key-to-ordinal conversion.  Converts @len external key bytes
 * into ordinals in @dst.  Identity maps short-circuit to memcpy.
 */
#include "ft-key.h"

static
struct cds_ft_inode_flag *ft_node_flag(struct cds_ft_inode *node,
		unsigned long type)
{
	assert(type < (1UL << FT_TYPE_BITS));
	return (struct cds_ft_inode_flag *) (((unsigned long) node) |
		(type << FT_INTERNAL_BITS) |
		FT_INTERNAL_MASK);
}

/*
 * Test whether @node has the external tag (bits 0-1 == 0b00).
 * This matches both non-NULL external leaf pointers AND NULL,
 * since NULL has tag bits 0b00.  Callers that need to distinguish
 * NULL from a valid external node should also check ft_node_ptr().
 */
static inline_lookup
bool ft_node_external(struct cds_ft_inode_flag *node)
{
	return ((unsigned long) node & FT_TAG_MASK) == 0;
}

#ifdef FEATURE_FT_COMPRESS
static inline_lookup
bool ft_node_compressed(struct cds_ft_inode_flag *node)
{
	return ((unsigned long) node & FT_TAG_MASK) == FT_COMPRESSED_MASK;
}
#else
static
bool ft_node_compressed(struct cds_ft_inode_flag *node __attribute__((unused)))
{
	return false;
}
#endif

/*
 * ft_metadata_set_external_nodes: Phase 1 -- set the cluster-internal
 * forward pointer (metadata->external_nodes) on a freshly-built node.
 * Asserts that the node is not a compressed node (compressed nodes
 * must not carry metadata->external_nodes).
 *
 * This is the cluster-init step.  It intentionally does NOT do the
 * matching back-channel publish (external_nodes->prev = node_flag).
 * Setting prev makes the cluster reachable to up-walkers via the live
 * external's back-pointer.  So it must follow the wiring of node_flag's
 * own parent.  Use ft_publish_external_nodes_prev for that.  Order it
 * after the cluster top's parent is set, and immediately before (or as
 * part of) the forward publish.
 *
 * @node_flag: tagged pointer to the node (used for type check).
 * @metadata: the node's metadata.
 * @external_nodes: the external node list to set (may be NULL).
 */
static inline
void ft_metadata_set_external_nodes(struct cds_ft_inode_flag *node_flag,
		struct cds_ft_metadata *metadata,
		struct cds_ft_node *external_nodes)
{
	if (ft_node_compressed(node_flag)) {
		fprintf(stderr, "BUG: ft_metadata_set_external_nodes called on compressed node %p\n", node_flag);
		abort();
	}
	metadata->external_nodes = external_nodes;
	FT_TP(metadata_set_external_nodes, (const void *) node_flag,
		(const void *) external_nodes);
}

/*
 * Pointer unmasking via speculative mask + conditional select.
 *
 * Exploit the fact that each internal node type's allocation order
 * equals 4 + type_idx (type 0 is 16B-aligned, type 1 is 32B, etc.)
 * to compute the internal-node mask speculatively, in parallel with
 * the bit-0 test:
 *
 *   mask_internal = (~15UL) << ((v >> 1) & 7)
 *                 = ~0UL << (4 + type_idx)
 *
 * This clears all tag bits that sit below the type's alignment
 * boundary.  The shift amount is derived purely from bits 1-3 with
 * no dependency on bit 0.
 *
 * Non-internal nodes have bit 0 clear:
 *   - external nodes are >= 8-byte aligned (bits 0-2 zero);
 *   - compressed nodes are >= 16-byte aligned, with the tag in bit 1.
 * A fixed ~7UL mask suffices for both.
 *
 * The conditional select lets the two mask computations run in
 * parallel; the compiler emits a CMOV, keeping the critical path
 * to 4 cycles.
 */
/*
 * nr_keys is an MCAS-transacted scalar.  On the rank-stats-ON path the
 * order-statistics count is folded into the op's flip-txn.  So it is exact
 * under concurrent writers, and not a drifting approximate aggregate.
 *
 * Like the per-node state word, nr_keys therefore reserves its LOW bit for
 * the engine's in-band proxy marker:
 *
 *   - the logical count is stored as (count << 1);
 *   - bit 0 = FT_NR_KEYS_PROXY_TAG carries a parked proxy for the duration of
 *     a commit.
 *
 * Bit 0 is used rather than a high bit so the reservation is valid on 32-bit
 * too.  Access nr_keys ONLY through these helpers.  Never read or write the
 * field directly.
 */
#define FT_NR_KEYS_PROXY_TAG	1UL

/* Writer-side read of an owned / quiescent node (never a mid-commit proxy). */
static inline
unsigned long ft_nr_keys_get(const struct cds_ft_metadata *m)
{
	return m->nr_keys >> 1;
}

/*
 * Reader-side count read: acquire-load nr_keys and resolve a parked proxy to
 * its committed logical value.  urcu_txn_read short-circuits to a plain
 * acquire load whenever nr_keys holds no proxy.  That is the common case, and
 * always the case under writer exclusion.  So the resolve costs nothing
 * outside the commit window.
 */
static inline
unsigned long ft_nr_keys_load(const struct cds_ft_metadata *m)
{
	return (unsigned long) ft_txn_read(
			(void **) (uintptr_t) &m->nr_keys,
			FT_NR_KEYS_PROXY_TAG) >> 1;
}

/*
 * Store a node's order-statistics key count.  This is a no-op unless the trie
 * maintains order statistics (cds_ft_group_attr_set_rank_stats).
 *
 * This is the single write chokepoint, and it is gated on @ft->rank_stats.
 * So a default (rank-stats-off) trie touches the nr_keys field nowhere:
 *
 *   - no per-node init;
 *   - no propagation;
 *   - no root-ward count contention.
 *
 * @ft is read-only.  On cross-trie ops src and dst share a group (enforced)
 * and thus the same flag, so any in-scope trie works.
 */
static inline
void ft_nr_keys_store(const struct cds_ft *ft, struct cds_ft_metadata *m,
		unsigned long val, int mo)
{
	if (ft->rank_stats)
		uatomic_store(&m->nr_keys, val << 1, mo);
}

/*
 * Reader-side read of a node's live-child count.  It resolves a mid-commit
 * proxy on the state word.
 *
 * Once atomic detach is wired, the LIVE->DEAD tombstone rides an MCAS edge on
 * state.  That edge transiently parks a proxy: a full pointer with bit 0 =
 * FT_STATE_PROXY set.  Without the resolve, the proxy would corrupt the
 * nr_child bits for a concurrent reader.
 *
 * urcu_txn_read short-circuits to a plain acquire load when state holds no
 * proxy, which is always so under writer exclusion.  So it costs nothing
 * outside the commit window.
 *
 * The writer-owned ft_meta_nr_child (direct read) stays for reads of a node
 * that the caller owns or that is quiescent.
 */
static inline
unsigned int ft_meta_nr_child_load(const struct cds_ft_metadata *meta)
{
	return (unsigned int) (((uintptr_t) ft_txn_read(
			(void **) (uintptr_t) &meta->state,
			FT_STATE_PROXY) >> FT_STATE_NR_CHILD_SHIFT)
			& FT_STATE_NR_CHILD_VALMASK);
}

/*
 * Proxy-resolving read of a node's parent-slot offset.  This is the Phase 4.3
 * mirror of ft_meta_nr_child_load, anticipated at the declaration of
 * ft_meta_parent_slot_offset.
 *
 * parent_slot_offset shares the state word with the flip proxy.  A RAW read
 * of a live peer-owned node mid-commit returns the proxy pointer's bits as
 * the offset.  That value is arbitrary, up to FT_STATE_PSO_VALMASK.
 * ft_get_parent_slot would then compute ptr(parent) + garbage*8, a WILD
 * address.  It would fault on the dereference, before any commit guard runs.
 *
 * urcu_txn_read resolves the proxy to the real state word first.  It
 * short-circuits to a plain load when no proxy is parked.  That is always so
 * under writer exclusion, so the read is free outside the commit window.
 *
 * The direct ft_meta_parent_slot_offset stays for a node that the caller owns
 * or that is quiescent.
 */
static inline
unsigned int ft_meta_parent_slot_offset_load(const struct cds_ft_metadata *meta)
{
	return FT_PSO_DECODE(ft_txn_read(
			(void **) (uintptr_t) &meta->parent_slot_offset,
			FT_STATE_PROXY));
}

/*
 * Flip-proxy tag (see the full encoding note above ft_node_flip_proxy's original
 * home, further down).  Hoisted here because the tag-stripping helpers below
 * must be able to assert against it.
 */
#define FT_FLIP_PROXY_TYPE	7U
#define FT_FLIP_PROXY_TAG	(FT_INTERNAL_MASK | (FT_FLIP_PROXY_TYPE << FT_INTERNAL_BITS))

/*
 * The parent slot's tag width must cover the proxy that parks in it.  A trie
 * pointer is told apart from every other parent value by clearing exactly
 * this mask (see ft_parent_is_trie).
 */
urcu_static_assert(FT_FLIP_PROXY_TAG <= FT_PARENT_TAG_MASK,
		"the flip-proxy tag must fit within FT_PARENT_TAG_MASK",
		ft_parent_tag_covers_proxy);

static inline_lookup
bool ft_node_flip_proxy(struct cds_ft_inode_flag *node)
{
	return ((unsigned long) node & (FT_INTERNAL_MASK | FT_TYPE_MASK))
		== FT_FLIP_PROXY_TAG;
}

/*
 * ft_head_parent_word: return the value to store into an external head's
 * back-edge word.  That word is cell->parent with the list on, and node->prev
 * with the list off.  With this value the head names @parent_nf and answers
 * the shape question itself (FT_PARENT_PREFIX_HEAD).
 *
 * @prefix: true iff the head is, or is becoming, @parent_nf's external_nodes.
 *
 * ☠ THE BIT ONLY EXISTS ON AN INTERNAL NODE'S FLAG.  An internal flag is the
 * one form whose low nibble has bit 0 set and is not the type-7 flip proxy.
 * Every other form that can sit in a parent word uses bit 4 as address or
 * tag:
 *
 *   - a COMPRESSED node (bit 0 clear, 16-byte aligned);
 *   - a root-position TRIE stamp;
 *   - a parked PROXY.
 *
 * So the bit is DROPPED for those forms rather than refused.  The drop is the
 * right answer:
 *
 *   - A head under a compressed node is never a prefix head.  A compressed
 *     node carries no external_nodes.
 *   - A proxy is transient.  The real re-parent that replaces it re-derives
 *     the answer.
 *   - Every reader resolves a proxy before reading the bit.
 *
 * ☠ AND THE TEST IS bit 0, NOT (tag == FT_INTERNAL_MASK).  Bits 1..3 are the
 * node's TYPE, not spare.  An internal node of type 1 carries 0b011.  A
 * whole-nibble compare against FT_INTERNAL_MASK rejects it.  That mistake
 * silently drops the bit on every non-type-0 holder.  cds_ft_verify's at-rest
 * check caught it on the first run.
 */
static inline
struct cds_ft_inode_flag *ft_head_parent_word(struct cds_ft_inode_flag *parent_nf,
		bool prefix)
{
	/*
	 * A PROXY passes VERBATIM.  "Dropped" means not SET.  Stripping the
	 * bit would clear bit 4 of a record ADDRESS.  The proxy's 0xF nibble
	 * has bit 0 set, and that is all ft_parent_prefix_strip tests.
	 * Half the records of a 0x30-stride descriptor have bit 4 set.  The
	 * result would be a proxy naming the middle of the record before.
	 *
	 * No caller passes a proxy today: both ft_set_parent_raw callers hand
	 * it the node being built.  The comment there that says otherwise is
	 * older than they are.
	 */
	if (ft_node_flip_proxy(parent_nf))
		return parent_nf;
	if (!prefix || !((uintptr_t) parent_nf & FT_INTERNAL_MASK))
		return ft_parent_prefix_strip(parent_nf);
	return (struct cds_ft_inode_flag *)
		((uintptr_t) parent_nf | FT_PARENT_PREFIX_HEAD);
}

/*
 * ft_head_parent_word_carry: return the same value for a park or a restore.
 * Such a write does NOT decide the shape, so the head keeps whatever it was.
 * The answer comes from @old_word, the head's current back-edge value.
 * @old_word is RESOLVED.  The callers that read a transacted word read it
 * through urcu_txn_load.  The plain-store callers resolve a parked proxy
 * first.
 */
static inline
struct cds_ft_inode_flag *ft_head_parent_word_carry(
		struct cds_ft_inode_flag *parent_nf, const void *old_word)
{
	return ft_head_parent_word(parent_nf,
		ft_parent_prefix_head((const struct cds_ft_inode_flag *) old_word));
}

/*
 * RESOLVED-POINTER CONTRACT.
 *
 * A slot under an in-flight commit does not hold a node.  It holds a pointer
 * to the transaction's MCAS record, tagged FT_FLIP_PROXY_TAG (low nibble 0xF).
 * Every structural predicate MISREADS such a flag and does not reject it:
 *
 *	ft_node_external(proxy)   -> (0xF & 0b11) == 0b00  -> false
 *	ft_node_compressed(proxy) -> (0xF & 0b11) == 0b10  -> false
 *
 * So a proxy silently dispatches as "an internal node of type 7".  The
 * tag-stripping helpers below then mint a wild node pointer out of the
 * record's address.  The fault surfaces frames later, in
 * cds_ft_item_to_metadata(), as a segfault on a garbage range.  Nothing then
 * shows who failed to resolve.
 *
 * The contract is therefore: RESOLVE FIRST (ft_resolve_flip_proxy), THEN
 * dispatch on kind.  ft_assert_resolved() traps a violation at its first use,
 * where the culprit is still on the stack.  Build with -DFT_DEBUG_PROXY_ASSERT;
 * it compiles to nothing otherwise.
 *
 * Deliberately NOT asserted: _ft_node_mask_ptr() and ft_flip_proxy_ptr(), which
 * exist precisely to strip a proxy's tag, and ft_node_flip_proxy() itself.
 */
#ifdef FT_DEBUG_PROXY_ASSERT
#include <stdio.h>
#include <stdlib.h>

__attribute__((noinline, cold))
static void ft_proxy_assert_fail(const char *fn, const void *p)
{
	fprintf(stderr, "FT_PROXY_ASSERT: %s() got a parked flip proxy %p\n",
		fn, p);
	fflush(stderr);
	abort();
}

# define ft_assert_resolved(node)					\
	do {								\
		if (caa_unlikely(ft_node_flip_proxy(			\
				(struct cds_ft_inode_flag *) (node))))	\
			ft_proxy_assert_fail(__func__,			\
				(const void *) (node));			\
	} while (0)
#else
# define ft_assert_resolved(node)	((void) 0)
#endif

static inline_lookup
struct cds_ft_inode *ft_node_ptr(struct cds_ft_inode_flag *node)
{
	unsigned long v = (unsigned long) node;

	ft_assert_resolved(node);

	/*
	 * Compute the mask from the original pointer.  The skip-compressed
	 * length bits (57-63) don't affect bits 0-3, which the type
	 * dispatch uses.  So this runs in parallel with the ADDR_MASK AND
	 * below (full ILP).
	 */
	unsigned long mask_internal = (~15UL) << ((v >> 1) & 7);
	unsigned long mask = (v & 1) ? mask_internal : ~7UL;

#ifdef FEATURE_FT_SKIP_COMPRESSED
	/*
	 * Clear the top FT_SKIP_LEN_BITS (7 bits).  In a hot loop
	 * the compiler hoists FT_ADDR_MASK into a register.  This is
	 * then a single 1-cycle AND that runs in parallel with the
	 * mask chain above.
	 */
	v &= FT_ADDR_MASK;
#endif

	return (struct cds_ft_inode *) (v & mask);
}

/*
 * Identity-only variant: mask a RAW slot value to a comparable address WITHOUT
 * asserting it is resolved.  A slot under an in-flight commit legitimately
 * holds a parked flip proxy.  Some conflict checks only compare the masked
 * value against a descent-captured node (proxy != captured -> retry).  Such a
 * check never dereferences the value.  Use this variant at those sites.  Use
 * ft_node_ptr(), which asserts, everywhere the result is dereferenced.  Never
 * dereference the result of this variant.
 */
static inline_lookup
struct cds_ft_inode *ft_node_ptr_raw(struct cds_ft_inode_flag *node)
{
	unsigned long v = (unsigned long) node;
	unsigned long mask_internal = (~15UL) << ((v >> 1) & 7);
	unsigned long mask = (v & 1) ? mask_internal : ~7UL;

#ifdef FEATURE_FT_SKIP_COMPRESSED
	v &= FT_ADDR_MASK;
#endif

	return (struct cds_ft_inode *) (v & mask);
}

/*
 * Lookup-hot variant.  The caller has already established that the
 * internal-flag bit is set.  For example, ft_node_get_nth_skip checks
 * !(tag & FT_INTERNAL_MASK) and returns NULL before this call.
 * This variant skips the (v & 1) ? ... : ~7UL branch of ft_node_ptr()
 * above.  That removes the cmov/branch from the per-visit dependency
 * chain on the lookup hot path.
 */
static inline_lookup
struct cds_ft_inode *ft_node_ptr_internal(struct cds_ft_inode_flag *node)
{
	unsigned long v = (unsigned long) node;
	unsigned long mask = (~15UL) << ((v >> 1) & 7);

#ifdef FEATURE_FT_SKIP_COMPRESSED
	v &= FT_ADDR_MASK;
#endif

	assert((v & FT_INTERNAL_MASK) || node == NULL);
	return (struct cds_ft_inode *) (v & mask);
}

static
struct cds_ft_inode *_ft_node_mask_ptr(struct cds_ft_inode_flag *node)
{
	unsigned long v = (unsigned long) node;

#ifdef FEATURE_FT_SKIP_COMPRESSED
	v = (v << FT_SKIP_LEN_BITS) >> FT_SKIP_LEN_BITS;
#endif
	return (struct cds_ft_inode *) (v & FT_PTR_MASK);
}

static inline_lookup
bool ft_node_internal(struct cds_ft_inode_flag *node)
{
	return (unsigned long) node & FT_INTERNAL_MASK;
}

static inline_lookup
unsigned long ft_node_type(struct cds_ft_inode_flag *node)
{
	unsigned long type;

	ft_assert_resolved(node);

	if (_ft_node_mask_ptr(node) == NULL) {
		return NODE_INDEX_NULL;
	}
	/* Compressed nodes don't have a type index. */
	assert(!ft_node_compressed(node));
	type = (unsigned int) (((unsigned long) node & FT_TYPE_MASK) >> FT_INTERNAL_BITS);
	assert(type < (1UL << FT_TYPE_BITS));
	return type;
}

/*
 * Flip-proxy encoding (see <urcu/rcu-txn-sw.h>).  cds_ft_merge_at needs
 * to switch a whole set of back-pointers (and the merge-point forward
 * slot) from their old to their new target with no mixed-regime window.
 * Each such slot transiently holds a tagged pointer to a MCAS proxy
 * latch; a single MCAS flip commit store flips them all atomically.
 *
 * A proxy is tagged as a synthetic INTERNAL node of type-index 7, the
 * maximal tag value (low nibble (FT_INTERNAL_MASK | FT_TYPE_MASK) == 0xF).
 * ft_types[7] is FT_NULL on every arch:
 *
 *   - on 64-bit it is the canonical NULL slot (NODE_INDEX_NULL == 7);
 *   - on 32-bit it is reserved padding (NODE_INDEX_NULL == 6 and real
 *     types stop at 5).
 *
 * So no real node ever carries type 7 in either tier.  A flag whose low
 * nibble is 0xF is thus a proxy and nothing else.  External, compressed,
 * NULL and skip pointers never set all of bits 0..3.
 *
 * The proxy is 16-byte aligned, which leaves the low 4 bits free for the
 * tag.  It lives at a userspace address with the skip-len high bits
 * clear.  So _ft_node_mask_ptr recovers it exactly.  The same encoding is
 * valid in both forward child slots and parent slots, since both resolve
 * a flag through this dispatch.
 */
/* FT_FLIP_PROXY_TYPE / FT_FLIP_PROXY_TAG / ft_node_flip_proxy(): hoisted above
 * ft_node_ptr(), so the tag-stripping helpers can assert against the tag. */

static inline_lookup
struct ft_txn_parked *ft_flip_proxy_ptr(struct cds_ft_inode_flag *node)
{
	return (struct ft_txn_parked *) _ft_node_mask_ptr(node);
}

/*
 * Resolve a possibly-proxied flag to its current target.  This sits right
 * after a parent / root pointer load on the read side.  In the common case
 * no mutation is in flight, and the cost is a single predicted-not-taken
 * mask-compare (ft_node_flip_proxy).  The parked-record dereference is
 * reached only during a commit's brief install-to-settle window.  A parked
 * record carries FT's own 0xF tag (see URCU_TXN_PROXY_* in
 * fractal-trie-internal.h).  So this masks the tag off and resolves the proxy
 * through its commit's decision.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_resolve_flip_proxy(struct cds_ft_inode_flag *node)
{
	if (caa_unlikely(ft_node_flip_proxy(node)))
		return (struct cds_ft_inode_flag *)
			ft_txn_parked_resolve(ft_flip_proxy_ptr(node));
	return node;
}

/*
 * A parent word, proxy-resolved, as a NODE -- NULL at a root.
 *
 * RESOLVE FIRST, THEN LAUNDER.  ft_parent_node() maps a root's trie stamp to
 * NULL, but it passes a parked flip proxy through untouched.  So the reverse
 * order hands back whatever the record holds.  That is a trie stamp when the
 * record moves a node into or out of a root position.  A stamp passes
 * ft_node_external() (its tag bits are 0), so the caller walks a struct
 * cds_ft as a node.  A stamp is never a proxy, so resolving first is the
 * identity for it.  (No test reaches a proxied stamp today; this is the
 * order that stays right when one does.)
 */
static inline_lookup
struct cds_ft_inode_flag *ft_parent_node_resolved(struct cds_ft_inode_flag *raw)
{
	return ft_parent_node(ft_resolve_flip_proxy(raw));
}

/*
 * Explicit acquire-load for child pointer dereference.
 *
 * Count-based readers (lookup_nth, skip, count_keys) need acquire
 * ordering on child pointer loads to pair with the writer's
 * rcu_assign_pointer (release) during removal.  This ensures that
 * if a reader sees a detached pointer, it also sees the preceding
 * nr_keys decrement (undercount guarantee on weakly-ordered
 * architectures).
 *
 * Current toolchains already compile rcu_dereference (CMM_CONSUME)
 * as CMM_ACQUIRE; this macro makes the acquire unconditional,
 * removing the dependency on the URCU_DEREFERENCE_USE_VOLATILE
 * escape hatch.
 */
static inline_lookup
void ft_maybe_prefetch(const void *ptr)
{
	/*
	 * Prefetch the RAW pointer without clearing the skip-compressed
	 * length high bits.  __builtin_prefetch doesn't fault on
	 * non-canonical addresses (it's a hint that silently drops invalid
	 * loads), so:
	 *   - clean child (~97% on dns): canonical -> prefetch fires with
	 *     zero added latency on the common path;
	 *   - skip-encoded child (~3%): non-canonical -> prefetch dropped.
	 *
	 * Clearing the bits first is a NET LOSS (measured on dns ft_specv,
	 * 2026-05-23).  Two shapes were tried: an unconditional mask
	 * (& 57-bit imm) and an unconditional double-shift.  BOTH were ~2%
	 * slower.  The clear sits ahead of the prefetch in the dep chain, so
	 * it delays the common-case prefetch issue.  A
	 * raw-prefetch-then-conditional-clear shape keeps the common case
	 * fast but only TIES no-clear: prefetching the rare 3% skip children
	 * buys nothing measurable.  So: prefetch raw, accept the dropped 3%.
	 * Do NOT re-add the clear without a skip-heavy workload that shows a
	 * real win.
	 *
	 * Prefetch INTERNAL (tagged) children only.  External (leaf) children
	 * (tag bits clear) are random and use-once.  Prefetching them is at
	 * best useless and at worst harmful (measured, EPYC 9654 / dns
	 * load-names):
	 *   - under 4 KiB leaf pages it is a no-op: the leaf-arena TLB miss
	 *     drops the prefetch before its translation resolves;
	 *   - under 2 MiB leaf pages the translation resolves.  The
	 *     prefetches then flood the memory controller (~5x more
	 *     software-prefetch fills reach DRAM) and inflate demand-load
	 *     latency: -27% throughput.
	 * Removing it is neutral at 4 KiB (it was dropped anyway) and removes
	 * that 2 MiB foot-gun.  Internal nodes have descent locality + reuse and
	 * do not flood, so they keep a temporal prefetch.
	 */
	if (((unsigned long) ptr & FT_TAG_MASK) != 0)
		__builtin_prefetch(ptr);
}

/*
 * Non-temporal variant for stream-once spatial prefetch.  The use is the
 * inequality adjacent-sibling: a near-future iteration target that is read
 * once, and not reused like a descent node.  prefetchnta fills with minimal
 * cache-level allocation, so the streamed siblings do not evict the hot
 * working set.  It is aimed at the extra LLC traffic that the temporal
 * adjacent prefetch adds.  It has the same internal-only FT_TAG_MASK guard:
 * leaves are the random/use-once 2 MiB-page foot-gun (see above).
 */
static inline_lookup
void ft_maybe_prefetch_nta(const void *ptr)
{
	if (((unsigned long) ptr & FT_TAG_MASK) != 0)
		__builtin_prefetch(ptr, 0, 0);
}

/*
 * ft_dereference_prefetch: for tagged FT node pointers.  It prefetches the
 * raw pointer via ft_maybe_prefetch, which does NOT clear the
 * skip-compressed length bits.  See there: a skip-encoded pointer is
 * non-canonical and its prefetch is silently dropped.  Keeping the
 * common-case prefetch un-delayed beats prefetching the rare skip child.
 *
 * ft_dereference_external: for external (leaf) pointers like
 * external_nodes.  External children are deliberately NOT prefetched.  See
 * ft_maybe_prefetch: random/use-once leaves drop the prefetch on a 4 KiB TLB
 * miss, or flood the memory controller under 2 MiB.
 *
 * It also RESOLVES a parked flip proxy.  A "new key at an existing internal
 * node" publish parks a proxy in external_nodes.  The proxy lets the publish
 * commit atomically with the ordinal-cell splice, in one MCAS flip commit.
 * So a reader loading external_nodes must resolve it to the old/new head,
 * exactly as it does for a child slot.  In the common case the word holds a
 * real head with the proxy tag clear.  The cost is then a single
 * predicted-not-taken tag test.
 */
#define ft_dereference_prefetch(p)		\
	({							\
		__typeof__(p) __ft_tmp = rcu_dereference(p);	\
		ft_maybe_prefetch(__ft_tmp);			\
		__ft_tmp;					\
	})

#define ft_dereference_external(p)					\
	((__typeof__(p)) ft_resolve_flip_proxy(				\
		(struct cds_ft_inode_flag *) rcu_dereference(p)))

#define ft_dereference_acquire_prefetch(p)	\
	({							\
		__typeof__(p) __ft_tmp =			\
			(__typeof__(p)) uatomic_load(&(p),	\
						     CMM_ACQUIRE); \
		ft_maybe_prefetch(__ft_tmp);			\
		__ft_tmp;					\
	})

#define ft_dereference_acquire(p)	\
	(__typeof__(p)) uatomic_load(&(p), CMM_ACQUIRE)

/*
 * Acquire-ordered external-head dereference that ALSO resolves a flip proxy.
 * The ordered-query / rank-select readers (cds_ft_lookup_nth and friends) load
 * metadata->external_nodes with acquire ordering.  This orders the load
 * against the metadata key counts they consume.  A concurrent one-commit
 * splice (ft_insert_park_external_nodes) parks a flip proxy in that slot.
 * Resolve it exactly as ft_dereference_external does for the consume-ordered
 * descent readers.  Otherwise a tagged proxy would be mistaken for the
 * external head.  The common case (no splice in flight, tag clear) is one
 * predicted-not-taken test.
 */
#define ft_dereference_external_acquire(p)				\
	((__typeof__(p)) ft_resolve_flip_proxy(				\
		(struct cds_ft_inode_flag *) ft_dereference_acquire(p)))

/*
 * Root-slot dereference for read-side descents.  It is like
 * ft_dereference_*(ft->root), and it also resolves a flip-proxy.  A
 * cds_ft_merge_at commit may transiently install one at the merge-point
 * forward slot (here, the root).  The resolved flag then flows into the
 * cached path, the key_len==0 / longest-match metadata access, and the
 * child dispatch.  The common case (no merge in flight) is a single
 * predicted-not-taken mask-compare in ft_resolve_flip_proxy.
 */
/*
 * Root-is-internal invariant enforcement.  No mutator ever publishes a
 * compressed / skip-compressed node at the trie root.  A would-be compressed
 * root is re-internalized build-invisibly BEFORE publish:
 *
 *   - on the re-root/graft-swap side, by ft_make_root_internal_glue;
 *   - on the detach side, by the compressed -> fresh-internal replace.
 *
 * So a reader must never observe a compressed root.
 * ft_root_assert_not_compressed() (defined after the skip-compressed helpers
 * below) asserts it on EVERY reader root load.  The assert runs after
 * flip-proxy resolution.  A merge/graft proxy resolves to an internal node in
 * both commit phases, so it does not trip the assert.  A stray compressed
 * root is then caught at first observation, not as downstream descent
 * corruption.  A NULL root (untagged) passes.  The assert is compiled out
 * under NDEBUG.
 */
#define ft_root_dereference_prefetch(ft)				\
	ft_root_assert_not_compressed(					\
		ft_resolve_flip_proxy(ft_dereference_prefetch((ft)->root)))
#define ft_root_dereference_acquire_prefetch(ft)			\
	ft_root_assert_not_compressed(					\
		ft_resolve_flip_proxy(ft_dereference_acquire_prefetch((ft)->root)))
#define ft_root_dereference(ft)						\
	ft_root_assert_not_compressed(					\
		ft_resolve_flip_proxy(rcu_dereference((ft)->root)))

/*
 * cn->child dereference for read-side descents.  It is like
 * ft_dereference_acquire_prefetch(cn->child), and it also resolves a
 * flip-proxy.  The recompaction (or external promote) commit of a
 * key-disappearing remove transiently installs one at cn->child.  That commit
 * flips this forward edge together with the ordered-cell unsplice.  EVERY
 * reader that descends through a compressed node's child must resolve it.
 * The common case (no such remove in flight) is a single
 * predicted-not-taken mask-compare.
 */
#define ft_cn_child_dereference_acquire_prefetch(cn)			\
	ft_resolve_flip_proxy(ft_dereference_acquire_prefetch((cn)->child))

/*
 * Per-caller prefetch hint for ft_node_get_nth_skip / ft_node_get_nth
 * and the underlying scanners.  Compile-time constant at each call
 * site -- the branches inside ft_maybe_prefetch_hint fold away, leaving
 * at most a single prefetch per caller.
 *
 *   FT_PF_NONE:        no prefetch.
 *   FT_PF_DATA:        prefetch child's data (node body).  Right for
 *                      candidate lookup and non-skip exact lookup
 *                      that traverse the returned child's data next.
 */
enum ft_pf_target {
	FT_PF_NONE,
	FT_PF_DATA,
};

static inline_lookup
void ft_maybe_prefetch_hint(const void *ptr, enum ft_pf_target hint)
{
	switch (hint) {
	case FT_PF_NONE:
		break;
	case FT_PF_DATA:
		ft_maybe_prefetch(ptr);
		break;
	}
}

#define ft_dereference_acquire_prefetch_hint(p, hint)			\
	({								\
		__typeof__(p) __ft_tmp =				\
			(__typeof__(p)) uatomic_load(&(p),		\
						     CMM_ACQUIRE);	\
		ft_maybe_prefetch_hint(__ft_tmp, (hint));		\
		__ft_tmp;						\
	})

#define ft_dereference_prefetch_hint(p, hint)				\
	({								\
		__typeof__(p) __ft_tmp = rcu_dereference(p);		\
		ft_maybe_prefetch_hint(__ft_tmp, (hint));		\
		__ft_tmp;						\
	})

/*
 * Return @node's external_nodes (the dup-chain head hanging off an
 * internal or compressed node).  @node must be internal or compressed.
 * Used to recover a cached iterator position's deepest trie node without
 * re-descending: a prefix key sits at an internal/compressed node
 * whose external_nodes == iter->node.
 */
static inline_lookup
struct cds_ft_node *ft_node_external_nodes(struct cds_ft_inode_flag *node)
{
	struct cds_ft_metadata *metadata;

	assert(!ft_node_external(node));
	if (ft_node_compressed(node))
		metadata = cds_ft_item_to_metadata(ft_node_ptr(node));
	else {
		const struct cds_ft_type *type = &ft_types[ft_node_type(node)];

		metadata = cds_ft_item_to_metadata_fast(ft_node_ptr(node),
				type->order);
	}
	return ft_dereference_external(metadata->external_nodes);
}

/*
 * Speculative inequality result-key capture.
 *
 * A group can be configured for speculative skip-compressed lookup with a
 * leaf-key offset.  The matched leaf @leaf then stores the full result key at
 * that offset, in the byte order the application passed to cds_ft_insert().
 * Transform @level bytes of it to the iterator's ordinal (trie) order into
 * @dst and return true.  The caller then need not rebuild the key from the
 * descent's compressed-node bytes.
 *
 * ft_key_to_ordinals applies the group's key map: a plain copy for an
 * identity map, a per-byte remap otherwise.  So the fast path covers
 * non-identity maps too (no identity restriction).
 *
 * Return false and copy nothing on groups without the offset /
 * skip-compressed encoding.  The caller then falls back to the descent-built
 * ordinal_key accumulation.
 *
 * This is the result-key source on a configured speculative group.  The
 * min-descent intentionally leaves dispatch-irrelevant holes in ordinal_key:
 * it follows skip pointers without filling the spanned bytes.  So the leaf
 * copy carries the full result key, and ordinal_key does not.  The
 * ordered-iteration / relational invariant tests validate this end to end.
 */
static inline_lookup
bool ft_speculative_keycopy(const struct cds_ft *ft,
		const struct cds_ft_node *leaf,
		uint8_t *dst, ssize_t level)
{
	const struct cds_ft_group *group = ft->group;
	const uint8_t *leaf_key;

	if (!leaf || level < 0)
		return false;
	/*
	 * Per-trie gate.  A trie can opt out of speculative keys
	 * (cds_ft_attr_set_speculative_keys false).  Its leaves may then hold a
	 * key that does not match their position.  Such a trie must NOT copy
	 * from the leaf's stored key here.  The caller falls back to the
	 * descent-built ordinal_key.
	 */
	if (!ft->speculative_key_offset_active || !group->speculative ||
			!(group->flags & CDS_FT_FLAG_SKIP_COMPRESSED))
		return false;
	leaf_key = (const uint8_t *) leaf + group->speculative_key_offset;
	ft_key_to_ordinals(dst, leaf_key, (size_t) level, &group->key_map);
	return true;
}

/*
 * Unconditional variant of ft_speculative_keycopy.  It is for callers that
 * have already resolved use_keycopy at compile time.  The leaf-copy config
 * gate (speculative_key_offset_set && speculative && SKIP_COMPRESSED) is then
 * known to hold, so the runtime re-check folds away.
 *
 * Copy @level ordinal bytes of @leaf's stored key into @dst.  Copy nothing
 * for a NULL @leaf (NOT_FOUND) or @level < 0: the result key is then unused.
 */
static inline_lookup
void ft_speculative_keycopy_unconditional(const struct cds_ft *ft,
		const struct cds_ft_node *leaf, uint8_t *dst, ssize_t level)
{
	const struct cds_ft_group *group = ft->group;
	const uint8_t *leaf_key;

	if (!leaf || level < 0)
		return;
	leaf_key = (const uint8_t *) leaf + group->speculative_key_offset;
	ft_key_to_ordinals(dst, leaf_key, (size_t) level, &group->key_map);
}

/*
 * Ordinal-cell tag + accessors (cell-always model).
 *
 * Every duplicate-chain HEAD has a library-owned ordinal cell (struct
 * ft_ord_cell), and the head's cds_ft_node.prev points to it.  The head's
 * flagged parent is relocated into ft_ord_cell.parent; the cell is the
 * external head's metadata record, peer to internal/compressed metadata.
 *
 * The cell pointer is tagged with FT_INTERNAL_MASK (bit 0).  This preserves
 * the head-vs-dup test ft_node_external(prev)==false: a non-head dup's prev
 * is an untagged external cds_ft_node (bits 0-1 == 0).  Bit 0 is the ONLY
 * tag.  In a cell build a head's prev is ALWAYS a cell, so there is nothing
 * to distinguish.  No per-pointer marker is needed (32-bit safe).  Cells are
 * >= 2-byte aligned, so bit 0 is free.
 *
 * The DOWNWARD child slots still point straight at the external node; the cell
 * is interposed only on the UPWARD walk (parent recovery) and ordered
 * traversal.  Every reader of a head's prev-as-parent resolves through
 * ft_resolve_head_prev (identity outside the feature).
 */
#define FT_ORD_CELL_TAG		FT_INTERNAL_MASK

#include "ft-lookup-helpers.h"

/*
 * ft_node_holder: write-side resolution of a node's holder (the slot owner
 * "above" it), independent of the cell relocation.
 *
 *   - non-head duplicate: prev is the predecessor cds_ft_node (external).
 *   - head: prev is the flagged parent directly (non-cell build) or the
 *     cell whose ->parent holds the flagged parent (cell build).
 *   - never-inserted (prev NULL): returns NULL.
 *
 * Mutex-held callers (remove / replace / locate-chain-head) that previously
 * read node->prev as the holder route through this so the cell indirection
 * is transparent.  Identity in non-cell builds.
 */
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
/*
 * WHICH refusal.  cds_ft_remove has six distinct "not found" exits, and the
 * status code cannot tell them apart.  A refusal is a CLAIM ABOUT THE TRIE,
 * not a return value.  For example, an idempotent miss and a derivation that
 * lost its holder to a peer report the same thing to the caller.  Each site
 * stamps its own __LINE__ here, and the violation event carries it.
 */
static __thread unsigned int ft_dbg_rm_site;
#endif

static inline
struct cds_ft_inode_flag *ft_node_holder(struct cds_ft *ft,
		const struct cds_ft_node *node)
{
	/*
	 * Resolve a parked flip proxy at the load.  Since Phase 4.3, a
	 * head-promote folds its prev-inherit store onto the commit flip-txn.
	 * This word then transiently carries FT's type-7 proxy.  A raw
	 * external/cell classification of the proxy value would mis-derive the
	 * holder.
	 */
	void *prev = (void *) ft_resolve_flip_proxy((struct cds_ft_inode_flag *)
			rcu_dereference(((struct cds_ft_node *) node)->prev));

	if (ft_node_external((struct cds_ft_inode_flag *) prev))
		return (struct cds_ft_inode_flag *) prev;
	return ft_resolve_head_prev(ft, prev);
}

/*
 * ft_chain_head_is_removed: is @node's CHAIN retired, as opposed to @node
 * itself?  Walk prev to the head (the same walk ft_chain_head_holder does) and
 * ask ft_node_is_removed of THAT.
 *
 * ★ WHY A CHAIN NEEDS A LIVENESS ANSWER ITS MEMBERS CANNOT GIVE.  A
 * whole-chain displacement is cds_ft_insert_replace swinging the anchor slot
 * to a fresh head.  It retires every node at once.  But a txn is bounded and
 * a chain is not.  So only the HEAD's freeze can ride the displacing commit
 * (ft_hlist_freeze_prepare).  The members are therefore RETIRED BUT UNMARKED.
 * ft_node_is_removed is a per-node test of node->next, so it answers "live"
 * for every one of them.  An op that believes it goes on to unlink a ghost.
 * Its @pred comes from a stale prev.  The `pred->next: elem -> next` edge it
 * records can never match.  That is a livelock, not an error.
 *
 * ☠ MARKING THE MEMBERS INSTEAD IS THE WRONG CURE.  It was tried.  A
 * post-commit sweep is a BARE CAS by a non-owner on cds_ft_node.next.  The
 * word-kind table gives that word to the chain HOLDER.  The CAS lands
 * underneath ops that already validated.  It turns their loaded next into
 * MARK(NULL), the bare value 2.  A backward edge then records slot
 * &((struct cds_ft_node *) 2)->prev.  Measured 8/8 SEGV.  Asking the head
 * costs a walk the caller is already making, and writes nothing.
 *
 * HOLDER-LOCK CALLERS ONLY, exactly as ft_chain_head_holder: the prev walk is
 * not stable under a concurrent relink.  Returns false for a never-inserted
 * node (prev NULL).
 */
static inline
bool ft_chain_head_is_removed(struct cds_ft_node *node)
{
	struct cds_ft_node *cur = node;

	for (;;) {
		void *prev = (void *) ft_resolve_flip_proxy(
			(struct cds_ft_inode_flag *)
			rcu_dereference(cur->prev));

		if (!prev)
			return false;
		if (!ft_node_external((struct cds_ft_inode_flag *) prev))
			break;			/* @cur is the head */
		cur = (struct cds_ft_node *) prev;
	}
	return ft_node_is_removed(cur);
}

/*
 * ft_chain_head_holder: resolve the trie HOLDER of @node's duplicate chain.
 * The holder is the head's IMMEDIATE PARENT.  It is the single lockable
 * state-word node every op on the chain serialises on (MW LOCK_FINE holder
 * lock).
 *
 * @node may be:
 *
 *   - the head: its prev is the cell / flagged parent.  Resolve directly, in
 *     one iteration.
 *   - an interior duplicate: its prev is a predecessor external.  Walk prev
 *     up to the head, whose prev is NOT external.
 *
 * A flip proxy parked on prev mid-commit resolves at each load, as in
 * ft_node_holder.
 *
 * HOLDER-LOCK / mutex-held callers only: the prev walk is not stable under a
 * concurrent chain relink.  An interior op can only find its head's holder to
 * lock while some coarser exclusion still holds.  Today that is the FT-wide
 * writer_lock.  That chicken-and-egg is resolved with the FT-wide-lock drop.
 *
 * Returns NULL for a never-inserted node (prev NULL).
 */
static inline
struct cds_ft_inode_flag *ft_chain_head_holder(struct cds_ft *ft,
		struct cds_ft_node *node)
{
	struct cds_ft_node *cur = node;

	for (;;) {
		void *prev = (void *) ft_resolve_flip_proxy(
			(struct cds_ft_inode_flag *)
			rcu_dereference(cur->prev));

		if (!prev)
			return NULL;
		if (!ft_node_external((struct cds_ft_inode_flag *) prev))
			return ft_resolve_head_prev(ft, prev);
		cur = (struct cds_ft_node *) prev;
	}
}

/*
 * ft_head_parent_word_slot: the generic re-parent form.  It serves
 * ft_set_parent and its two transacted twins.  Every restructure funnels its
 * child re-homes through them, and they already carry the answer in @slot.
 *
 * ★ @slot IS THE SHAPE, so no site has to be told it twice.  @slot is the
 * address of the word in @parent_nf that holds the head:
 *
 *   - &parent_meta->external_nodes: a PREFIX head, key = path(parent), the key
 *     ENDS at the parent.  The rekey S_top COW, the remove head-slot resolver
 *     and the insert replace arms all name that word explicitly.
 *   - NULL: also a prefix head.  ft-remove.h's head-slot resolver spells the
 *     convention out in as many words ("external_nodes, not a body slot").
 *     Every re-parent that hands a head to its new holder as external_nodes
 *     passes NULL:
 *       - the insert attach and its cluster-leaf branch (ic->live_slot);
 *       - the merge's M_ext;
 *       - the graft's displaced external.
 *   - any other word: a BODY SLOT, so a slot head, key = path(parent) + its
 *     own edge byte.
 *
 * Three NULL-slot sites have a NON-external child: a junction, a branch, a
 * compressed node's own child.  They never reach here, because this form is
 * consulted only on the external branch of those dispatchers.  A compressed
 * @parent_nf drops the bit by construction anyway.
 */
static inline
struct cds_ft_inode_flag *ft_head_parent_word_slot(
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **slot)
{
	bool prefix = true;

	if (slot && ((uintptr_t) parent_nf & FT_INTERNAL_MASK) &&
	    !ft_node_flip_proxy(parent_nf))
		prefix = ((void *) slot == (void *)
			&cds_ft_item_to_metadata(ft_node_ptr(parent_nf))->external_nodes);
	return ft_head_parent_word(parent_nf, prefix);
}

/*
 * ft_head_is_prefix: the head's OWN answer to the shape question.  Is it the
 * prefix head (external_nodes) of the parent its back-edge word names?
 *
 * The answer is read from that one word in both modes: cell->parent with the
 * list on, prev with the list off.  So it stays consistent with the pointer
 * it rides on.  A parked flip proxy is resolved first, because its bit 4 is
 * address.
 *
 * Write-side use: a REPLACE installs a new head in the old one's place.  It
 * must inherit the shape, not re-derive it from a holder mid-op.
 */
static inline
bool ft_head_is_prefix(const struct cds_ft *ft, struct cds_ft_node *head)
{
	void *prev;

	if (!head)
		return false;
	prev = ft_dereference_prev_resolved(head);
	if (!ft->ordered_list)
		return ft_parent_prefix_head((struct cds_ft_inode_flag *) prev);
	return ft_parent_prefix_head(ft_resolve_flip_proxy(
		rcu_dereference(ft_ord_cell_ptr(prev)->parent)));
}

/*
 * Record a fresh head's flagged parent.
 *
 *   - Non-cell builds store it directly into the (pre-publish) head's prev.
 *   - Cell builds store it into the head's pre-wired cell and leave prev
 *     intact: node->prev already carries the cell.
 *
 * For the fresh-head wiring sites only: the subsequent forward publish orders
 * this store.  Existing-head re-parents use ft_set_parent or the
 * external-nodes choke point.  Those resolve the cell themselves.
 *
 * @prefix says which SHAPE the head is being wired into.  It is true when the
 * head becomes @parent's external_nodes (its key ENDS at @parent).  It is
 * false for a body slot.  The word carries that answer from here on (see
 * FT_PARENT_PREFIX_HEAD).  So cds_ft_verify catches a site that gets it wrong
 * at rest.  Otherwise it would show as a rare wrong-node lookup.
 */
#define ft_external_head_set_parent(ft, node, parent, prefix)		\
	do {								\
		struct cds_ft_inode_flag *_hpw = ft_head_parent_word(	\
			(struct cds_ft_inode_flag *) (parent), (prefix)); \
									\
		/*						\
		 * The [debt] HEAD-WORD class, same as			\
		 * ft_publish_external_nodes_prev and ft_set_parent's	\
		 * external arm.  This spelling had NO audit arm.  So	\
		 * it could not appear in the class's zeros either way.	\
		 *						\
		 * ☞ DECLARED HIDDEN, AND THAT IS A CHECKED CLAIM here,	\
		 * not a believed one.  Every caller of this macro	\
		 * writes the back edge of a FRESH node it is still	\
		 * building.  `node->next = NULL` follows immediately,	\
		 * and the external_nodes publish is parked for a later	\
		 * commit.  So no reader can reach the word.		\
		 *						\
		 * The audit's HIDDEN arm scores that against		\
		 * ft_ch_head_reachable.  Its positive control fires in	\
		 * the same run: HEAD-CONTROL live removed leaf,	\
		 * reachable 3,514,638 of 3,514,798.  So a wrong claim	\
		 * here reads as hw_hidden_live.			\
		 *						\
		 * UNDECLARED was the wrong spelling.  With a non-NULL	\
		 * owner the audit counts it and returns WITHOUT	\
		 * scoring.  That left ~4M writes per leg visible but	\
		 * unverdicted.						\
		 */						\
		ft_ch_audit_head_at(__func__, __LINE__, (ft), (node),	\
			(struct cds_ft_inode_flag *) (parent),		\
			FT_EXCL_HIDDEN);				\
		if ((ft)->ordered_list)					\
			ft_ord_cell_set_parent((node), _hpw);		\
		else							\
			(node)->prev = _hpw;				\
	} while (0)

/*
 * ft_publish_external_nodes_prev: Phase 2 -- publish the back-channel pointer
 * up from the displaced/transferred external head @external_nodes to its
 * (re-)parent @node_flag via rcu_assign_pointer.
 *
 * Call it AFTER node_flag's own parent is wired.  An up-walker arriving via
 * the new back-channel then lands on a parent-wired cluster top, not on a
 * NULL parent.  Call it at or just before the forward publish that makes the
 * cluster reachable through node_flag's slot.  No-op when @external_nodes is
 * NULL (callers commonly guard on metadata->external_nodes).
 *
 * Ordered list on: @external_nodes is an existing head, so its prev already
 * carries its cell.  Record the new parent into cell->parent.  The head's
 * prev (the cell pointer) is unchanged.  All choke-point callers re-parent an
 * existing head, so the cell is guaranteed present.  A fresh head's parent is
 * wired by ft_set_parent via ft_node_set_nth instead.
 *
 * List off / non-cell: the head's prev IS the flagged parent, so re-parent it
 * directly.
 */
static inline
void ft_publish_external_nodes_prev(struct cds_ft *ft,
		struct cds_ft_inode_flag *node_flag,
		struct cds_ft_node *external_nodes)
{
	struct cds_ft_inode_flag *word;

	if (!external_nodes)
		return;
	/*
	 * By definition of this helper, @external_nodes is @node_flag's
	 * external_nodes.  So the word it publishes is a PREFIX-HEAD one: the
	 * head's key ENDS at @node_flag, and no edge byte separates them
	 * (FT_PARENT_PREFIX_HEAD).  Every caller pairs this with the matching
	 * ft_metadata_set_external_nodes.
	 */
	word = ft_head_parent_word(node_flag, /*prefix=*/ true);
	/*
	 * ☞ THE SECOND [debt] WORD CLASS.  This site writes BOTH spellings of
	 * it: ft_ord_cell.parent (list on) and cds_ft_node.prev of a HEAD
	 * (list off).  The owner is "the holder P" in the word-kind table: the
	 * node this head hangs under.  ft_chain_head_holder derives exactly
	 * that node, so the chain audit's predicate answers for it unchanged.
	 * Both stores are RAW rcu_assign_pointer with no txn ("MW + raw" in
	 * the table), so nothing can abort them.  If the holder is not held
	 * here, the write lands regardless.
	 */
	ft_ch_audit_head(ft, external_nodes, node_flag);
	if (ft->ordered_list) {
		ft_ord_cell_set_parent(external_nodes, word);
	} else {
		FT_CHAIN_CANARY_RAW(&external_nodes->prev, 1);
		rcu_assign_pointer(external_nodes->prev, word);
	}
	FT_TP(set_parent, (const void *) external_nodes, (const void *) word);
}

static
struct cds_ft_inode_flag *ft_compressed_node_flag(
		struct cds_ft_compressed_node *node)
{
	return (struct cds_ft_inode_flag *)
		(((unsigned long) node) | FT_COMPRESSED_MASK);
}

static inline_lookup
struct cds_ft_compressed_node *ft_compressed_node_ptr(
		struct cds_ft_inode_flag *node)
{
	ft_assert_resolved(node);
	return (struct cds_ft_compressed_node *)
		(((unsigned long) node) & ~(unsigned long) FT_TAG_MASK);
}

#ifdef FT_DEBUG_PARENT_VIOLATION
#include <stdio.h>
/*
 * The same violation the ordered up-walk dumps (ft-iter.h), at the OTHER load
 * that carries the invariant.  A bare assert here names only the READER: the
 * thread that followed the link.  The question is about the node whose
 * @parent_word produced the illegal value.  At -O1, nothing on this frame
 * survives into a core to name that node.
 *
 * @node is that node: the one suspected of having been retired and freed
 * while still referenced.  Print its metadata, state and rcu_head words at the
 * point of detection instead of reconstructing them afterwards.
 */
__attribute__((noinline, cold, unused))
static void ft_parent_rcu_violation(struct cds_ft *ft,
		struct cds_ft_inode_flag *node,
		struct cds_ft_inode_flag *bad)
{
	struct cds_ft_metadata *m = NULL;

	if (!ft_node_external(node))
		m = ft_node_compressed(node) ?
			cds_ft_item_to_metadata((struct cds_ft_inode *)
				ft_compressed_node_ptr(node)) :
			cds_ft_item_to_metadata(ft_node_ptr(node));
	fprintf(stderr, "FT_PARENT_RCU_VIOLATION: illegal parent %p\n",
		(void *) bad);
	fprintf(stderr, "  ft=%p node=%p meta=%p\n",
		(void *) ft, (void *) node, (void *) m);
	if (m) {
		fprintf(stderr, "  node parent_word=%p state=0x%lx\n",
			(void *) m->parent_word, (unsigned long) m->state);
		fprintf(stderr, "  node rcu_head words: %p %p\n",
			((void **) m)[-2], ((void **) m)[-1]);
	}
	fprintf(stderr, "  bad rcu_head words: %p %p\n",
		((void **) bad)[0], ((void **) bad)[1]);
	fflush(stderr);
	abort();
}
# define ft_parent_rcu_check(ft, node, parent)				\
	do {								\
		if (caa_unlikely((parent) && ft_node_external(parent)))	\
			ft_parent_rcu_violation((ft), (node), (parent));	\
	} while (0)
#else
# define ft_parent_rcu_check(ft, node, parent)				\
	do {								\
		(void) (node);						\
		assert(!(parent) || !ft_node_external(parent));		\
	} while (0)
#endif

/*
 * ft_get_parent_rcu: read the parent pointer of @node via
 * rcu_dereference.
 *
 * For external (leaf) nodes: returns cds_ft_node.prev.  @node must
 * be the head of its duplicate chain (non-head duplicates' prev
 * points to the preceding node in the chain, not to the parent).
 * Iterators and lookups maintain this invariant by convention --
 * iter->node always refers to the chain head.
 *
 * For internal/compressed nodes: returns metadata->parent.
 *
 * Returns NULL in two cases:
 *
 *   - @node is at the root position;
 *   - a concurrent detach / graft_swap orphaned @node and cleared
 *     its parent link.
 *
 * A read-side parent-pointer walk that observes NULL terminates
 * cleanly in either case.
 *
 * Read-side safe; the caller must be in an RCU read-side critical
 * section (or QSBR equivalent).
 *
 * An assertion verifies the returned parent is never external: an
 * external result would mean the caller passed a non-head duplicate
 * chain entry (whose prev points at the preceding duplicate, not
 * at the parent).
 *
 * Config-agnostic: it uses only the flip-proxy resolve (a merge
 * primitive) and basic accessors.  It is kept here, ahead of the
 * FEATURE_FT_SKIP_COMPRESSED block, so that cds_ft_merge_at can use it
 * in all build configs.
 */
/*
 * Member-to-head hops an up-walk tolerates before declaring the chain
 * pathological.  A duplicate chain is unbounded; a cycle is not a shape any
 * writer publishes, so this is a defensive bound, not a design limit.
 */
#define FT_EXT_CHAIN_HOPS_MAX	(1UL << 20)

/*
 * ft_ext_head_word: from an external @ext, return the RAW parent word of its
 * chain's HEAD.  The prefix-head bit is kept.  A parked flip proxy is resolved
 * at the load.  Return NULL for a never-inserted node, or for a chain the hop
 * bound gave up on.  *@head_ret (optional) is the head itself.
 *
 * ☠ AN UP-WALK MAY START ON A MEMBER, NOT A HEAD.  A skip word names a head at
 * the instant it is encoded.  Every up-walk from an external assumed it still
 * was one.  The diagram shows how that assumption failed.
 *
 *   cds_ft_rekey_merge                 reader
 *   ------------------                 ------
 *                                      is on the retired copy of the
 *                                      source junction
 *   splice the moved head onto the
 *   destination's duplicate chain:
 *   it is now an interior member,
 *   its prev is its PREDECESSOR NODE
 *                                      follow that copy's stale skip
 *                                      word straight to the member
 *                                      read the predecessor as a
 *                                      parent word: decode it as a
 *                                      cell (list mode) or as an
 *                                      internal
 *                                      walk into a NULL parent
 *
 * The result was ft_skip_reanchor's assert, in a reader thread, on every
 * served merge whose source head is a skip target.
 *
 * The discriminator is the chain's own (ft_chain_head_holder).  A head's prev
 * is a cell or a flagged internal, never an external.  A member's prev is an
 * external.  Hop to the head first.
 *
 * A member and its head carry the SAME key, so the head's position is the
 * member's.  A MOVED head's chain runs up the destination path.  The descent
 * that continues from there is a torn pass, and the two-descent witness
 * discards it.  The witness already does exactly that for a grafted internal
 * node.
 *
 * Every prev load resolves a parked record.  A prev record carries the
 * flip-proxy tag (FT_HLIST_PREV_TAG).  So the one resolver covers the head's
 * word and a member's alike, and no parked value is bit-identical to a cell.
 */
/*
 * @count: bump the DEBUG_COUNTERS member-hop counter when a hop happened.  ONLY
 * the read-side skip re-anchor passes true.  The counter is the same-parent
 * splice oracle's positive control ("a READER walked up from a spliced
 * member").  The write path takes these up-walks too: ft_reanchor_flag, the
 * count walk, the builders' ft_skip_to_compressed.  Counting their hops would
 * make a non-zero count prove nothing about readers.
 */
static inline
struct cds_ft_inode_flag *ft_ext_head_word(const struct cds_ft *ft,
		struct cds_ft_node *ext, struct cds_ft_node **head_ret, bool count)
{
	struct cds_ft_node *cur = ext;
	void *prev = ft_dereference_prev_resolved(cur);
	unsigned long hops = 0;

	while (prev && ft_node_external((struct cds_ft_inode_flag *) prev)) {
		if (++hops > FT_EXT_CHAIN_HOPS_MAX) {
			prev = NULL;
			break;
		}
		cur = (struct cds_ft_node *) prev;
		prev = ft_dereference_prev_resolved(cur);
	}
#ifdef DEBUG_COUNTERS
	if (hops && count)
		uatomic_inc(&ft->group->nr_ext_member_hops);
#else
	(void) count;
#endif
	if (head_ret)
		*head_ret = cur;
	if (!prev)
		return NULL;
	if (ft->ordered_list)
		return ft_resolve_flip_proxy(rcu_dereference(
			ft_ord_cell_ptr(prev)->parent));
	return (struct cds_ft_inode_flag *) prev;
}

static inline
struct cds_ft_inode_flag *ft_get_parent_rcu(struct cds_ft *ft,
		struct cds_ft_inode_flag *node)
{
	struct cds_ft_inode_flag *parent;

	/*
	 * The parent word may hold a flip proxy during a commit.  Resolve it
	 * to the parent this reader's view sees.  ft_ext_head_word resolves
	 * before the prefix strip.  The other two resolve before
	 * ft_parent_node: a resolved word can be a trie stamp, which must
	 * become NULL (see ft_parent_node_resolved).
	 */
	if (ft_node_external(node))
		parent = ft_parent_prefix_strip(ft_ext_head_word(ft,
			(struct cds_ft_node *) node, NULL, false));
	else if (ft_node_compressed(node))
		/*
		 * A compressed node's metadata lives at a FT_TAG_MASK-cleared
		 * offset, not the FT_TYPE_MASK-cleared one ft_node_ptr uses.
		 * A referenced compressed child re-parented by a merge reaches
		 * here.  The caller first resolves any skip form to its raw
		 * compressed flag.
		 */
		parent = ft_parent_node_resolved(rcu_dereference(
			cds_ft_item_to_metadata((struct cds_ft_inode *)
				ft_compressed_node_ptr(node))->parent_word));
	else
		parent = ft_parent_node_resolved(rcu_dereference(
			cds_ft_item_to_metadata(ft_node_ptr(node))->parent_word));
#ifdef FT_ENABLE_TRACING
	/*
	 * FIRE AT DETECTION, not from a signal handler.  The prior rig recorded
	 * that a SIGSEGV-handler snapshot lands long after the ring has wrapped.
	 * This is the same instant the assert below would abort on, but with the
	 * window still intact.
	 *
	 * A freelist link is 8-mod-16, so it clears the EXTERNAL tag.  That is
	 * why a reclaimed parent reads as an external node here.  The round
	 * trip (item -> metadata -> item) is the discriminator.  Equal means a
	 * valid live object with a wrong link.  Unequal means recycled memory.
	 */
	/*
	 * POSITIVE CONTROL for the whole emit -> freeze -> stop -> snapshot ->
	 * decode chain.  A violation site that has never been shown to LAND in
	 * a readable trace is an instrument on trust.  This one has already
	 * produced two aborts whose snapshots contained no violation event.
	 * FT_TRACE_SELFTEST=1 fires the identical sequence on the first call,
	 * from a perfectly healthy state.  A snapshot WITHOUT the event then
	 * indicts the rig, and one WITH it clears the rig.
	 */
	{
		static int selftest = -1;

		if (caa_unlikely(selftest < 0))
			selftest = getenv("FT_TRACE_SELFTEST") ? 1 : 0;
		if (caa_unlikely(selftest == 1)) {
			selftest = 0;
			FT_TP(parent_external_violation, node, parent, parent,
				0xdeadbeefUL);
			ft_trace_capture();
			fprintf(stderr, "FT_TRACE_SELFTEST: fired\n");
			abort();
		}
	}
	if (caa_unlikely(parent && ft_node_external(parent))) {
		const void *pp = ft_node_ptr(parent);
		const void *rt = NULL;

		rt = cds_ft_metadata_to_item(cds_ft_item_to_metadata(
			(struct cds_ft_inode *) pp));
		FT_TP(parent_external_violation, node, pp, rt,
			(unsigned long) rcu_dereference(cds_ft_item_to_metadata(
				ft_node_ptr(node))->parent_word));
		/*
		 * STOP FIRST, then snapshot.  system() is a fork+exec costing
		 * milliseconds.  This workload emits millions of events per
		 * second, so the 64 KiB ring wraps several times during it.  A
		 * plain "snapshot record" here then dumps a window that no
		 * longer contains the violation that triggered it.  Measured:
		 * the snapshot was written and the event was already gone.
		 * lttng stop freezes every buffer before the dump.  It also
		 * ends tracing for the run.  That is what we want: this
		 * process is about to abort anyway.
		 */
		ft_trace_capture();
		abort();
	}
#endif
	ft_parent_rcu_check(ft, node, parent);
	return parent;
}

/* Skip-compressed pointer helpers. */

#ifdef FEATURE_FT_SKIP_COMPRESSED
static inline
bool ft_node_skip_compressed(struct cds_ft_inode_flag *node)
{
	return ((unsigned long) node >> FT_SKIP_LEN_SHIFT) != 0;
}

static inline
unsigned int ft_skip_len(struct cds_ft_inode_flag *node)
{
	return (unsigned long) node >> FT_SKIP_LEN_SHIFT;
}

/*
 * ft_skip_child_ptr: extract the child tagged pointer from a skip
 * pointer by clearing the skip-length bits.
 */
static inline
struct cds_ft_inode_flag *ft_skip_child_ptr(struct cds_ft_inode_flag *node)
{
	ft_assert_resolved(node);
	return (struct cds_ft_inode_flag *) ((unsigned long) node & FT_ADDR_MASK);
}

/*
 * ft_skip_compressed_flag: encode a skip pointer from a child pointer
 * and the compressed path length.
 *
 * ☞ THE VALUE THIS BUILDS LIVES IN THE GRANDPARENT'S BODY.  The register in
 * fractal-trie-internal.h lists that word as GP-owned, recorded MW by every
 * producer (class DUAL).  It is MW as DEBT: the owner is DERIVED here from
 * cn's back-pointer, and only the op can vouch for holding it.
 *
 * ☠ ONE WORD, TWO ROLES.
 *
 *   - An op that holds GP can republish the slot as an ordinary FORWARD edge.
 *     To that op this is a structural slot of GP, and it PARKS SW when armed.
 *   - To every dual refresh it is a CAS.
 *
 * SW xor MW is a per-slot, cross-thread invariant.  So a dual refresh that
 * does not hold GP races that park.  Hold GP (ft_lock_skip_dual_gp) before
 * recording here.  Remember that "the recompact took {C,P,GP}" is a PLAN-TIME
 * claim, while this owner is derived at PUBLISH time.
 */
static
struct cds_ft_inode_flag *ft_skip_compressed_flag(
		struct cds_ft_inode_flag *child, unsigned int len)
{
	assert(len > 0 && len <= FT_SKIP_LEN_MAX);
	/*
	 * The encoding ORs len into the high bits of child.  If child
	 * already carries skip-length bits, it is itself a skip-compressed
	 * pointer.  The OR then conflicts with len.  It produces a
	 * corrupted nested encoding from which neither len nor child can
	 * be recovered cleanly.  Chain-compress canonicalization must
	 * ensure that cn->child is never skip-compressed at publish time
	 * (the "no two adjacent compresseds" invariant).  Assert the
	 * invariant here.  Any future regression then fails loudly under
	 * -UNDEBUG smoke tests, instead of silently corrupting the trie.
	 */
	assert(((unsigned long) child >> FT_SKIP_LEN_SHIFT) == 0);
	return (struct cds_ft_inode_flag *)
		((unsigned long) child |
		 ((unsigned long) len << FT_SKIP_LEN_SHIFT));
}

/*
 * ft_skip_to_compressed: recover the compressed node from a skip
 * pointer by following the child's parent back-pointer.
 *
 * For internal/compressed children: uses metadata->parent.
 * For external (leaf) children: uses cds_ft_node.prev (which points
 * to the parent for the head of a duplicate chain).
 *
 * No validation against the slot's skip_len: callers get whatever
 * the back-pointer currently says.  This includes writers in
 * mid-mutation, where the back-pointer and the slot value are
 * intentionally inconsistent for a brief window.  Reader paths that
 * must observe a self-consistent slot+cn pair re-anchor via
 * ft_skip_reanchor.  It walks the skip child's live parent chain to
 * the trie position that the slot's skip_len encodes.
 *
 * Read-side safe (rcu_dereference on both fields).  Callers must be
 * in an RCU read-side critical section (or QSBR equivalent).
 */
static inline
struct cds_ft_compressed_node *ft_skip_to_compressed(const struct cds_ft *ft,
		struct cds_ft_inode_flag *skip_ptr)
{
	struct cds_ft_inode_flag *child = ft_skip_child_ptr(skip_ptr);
	struct cds_ft_inode_flag *parent;

	if (ft_node_external(child))
		/*
		 * @child is an external.  ft_ext_head_word hops to its chain
		 * head and resolves the head's flagged parent.  It branches on
		 * the trie's ordered_list mode (cell-indirect vs prev-direct).
		 * A structural tag test is NOT usable here.  A skip pointer
		 * with a STALE target can transiently make this head's parent
		 * an internal node: a concurrent split/merge moved the encoded
		 * position.  An internal node carries FT_INTERNAL_MASK, bit 0,
		 * the same tag a cell carries.  So only the mode flag
		 * disambiguates safely.
		 */
		parent = ft_parent_prefix_strip(ft_ext_head_word(ft,
			(struct cds_ft_node *) child, NULL, false));
	else
		parent = ft_parent_node(rcu_dereference(cds_ft_item_to_metadata(
			ft_node_ptr(child))->parent_word));
	/*
	 * The recovered back-pointer may transiently be a flip proxy during a
	 * cds_ft_merge_at commit.  A dst-origin subtree root wrapped under a
	 * freshly-built compressed node in the merged cluster has its parent
	 * staged through the latch.  Resolve the proxy before masking to the
	 * compressed node.  Otherwise the proxy's tag bits fold into a bogus
	 * pointer.  This mirrors ft_get_parent_rcu and the descent's
	 * resolve-then-skip order.  A concurrent reader up-walking through the
	 * merge's flip window resolves via the shared selector.  It gets the
	 * consistent old-or-merged view.  The resolve is a no-op when no merge
	 * is in flight.
	 */
	return ft_compressed_node_ptr(ft_resolve_flip_proxy(parent));
}


/*
 * ft_upwalk_edge_bytes: the key bytes an up-walk consumes on the hop from @cur
 * to its INTERNAL parent.
 *
 *   - One byte for a child in a branch slot.
 *   - ZERO for a PREFIX head.  This is an external head hanging at the
 *     parent's external_nodes.  Its key ENDS at the parent.  So it sits AT
 *     the parent's position, and no edge byte separates them.
 *
 * ★ THE HEAD ANSWERS FOR ITSELF, from @head_parent_word -- the very word the
 * caller loaded to find the parent (FT_PARENT_PREFIX_HEAD).  Parent and shape
 * therefore come from ONE load, in BOTH list modes, and cannot disagree.
 *
 * ☠ What this replaces was the holder's LIVE external_nodes.  That answer is
 * DESTROYED IN PLACE by the very removes an up-walk must survive:
 *
 *   - a key-disappearing remove of a prefix head clears the holder's
 *     external_nodes while a parked reader still holds the head;
 *   - a replace leaves it naming a DIFFERENT head.
 *
 * The dead prefix head then read as a SLOT head.  It was charged a byte it
 * never consumed, and the descent resumed one level off.  A present key
 * answered NOT_FOUND.  A key ABSENT at every instant answered with a
 * NEIGHBOUR'S NODE.  The ordered list could patch it with a mark on the
 * retired cell.  List-off had no cell and no patch.  That is why the answer
 * had to move onto the head's own word.
 *
 * A skip slot never NAMES a prefix head at rest (it names the compressed node's
 * child, whose parent is that compressed node), so a non-zero answer here is
 * reached only through a STALE slot whose target a concurrent split has
 * re-homed as a fresh internal node's prefix head -- exactly the case
 * ft_skip_reanchor exists to resolve.
 *
 * Only an EXTERNAL @cur can be a prefix head, so an internal / compressed @cur
 * takes the constant-1 arm without consulting the word at all.
 */
static inline
unsigned int ft_upwalk_edge_bytes(struct cds_ft_inode_flag *cur,
		struct cds_ft_inode_flag *head_parent_word)
{
	if (!ft_node_external(cur))
		return 1U;
	return ft_parent_prefix_head(head_parent_word) ? 0U : 1U;
}

/*
 * ft_skip_reanchor: the single concurrency-handling mechanism for skip-
 * compressed pointers.  A skip slot encodes a length (skip_len).  The live
 * compressed node is recovered via the skip child's back-pointer.  It may no
 * longer match that length: a concurrent split/merge changed the path between
 * the slot and the child.  So readers resolve the slot by re-anchoring.  They
 * do not trust the recovered node directly.
 *
 * Spinning to re-read the slot does NOT converge when both of these hold:
 *
 *   - the slot lives on a node that was recompacted away (frozen-stale);
 *   - meanwhile a concurrent split/merge reparented the skip child to a
 *     different-length compressed.
 *
 * The frozen slot is never republished, so the reader would loop forever.
 * The skip child @G, however, is reachable in the LIVE trie (a live leaf or
 * live internal).  So its parent chain runs through live nodes that converge.
 * Walk it up and accumulate the consumed path length:
 *
 *   - a compressed spans its len;
 *   - an internal spans one byte, or NONE when the child is that internal
 *     node's PREFIX head (see ft_upwalk_edge_bytes).
 *
 * Stop when the accumulated length reaches the slot's skip_len.  That locates
 * the live tree position the failing slot encoded.
 *
 *   - split: the live path was lengthened into prefix+branch+suffix at the
 *     same total length.  The accumulation lands exactly, @*rewind == 0.
 *     Re-anchor at the live node at the same depth.
 *   - merge: the live path was shortened by absorbing the slot's level into a
 *     longer compressed.  The first hop already exceeds skip_len.  The
 *     encoded position is now interior to that compressed.  Re-anchor
 *     shallower (its parent) and have the caller rewind its descent cursor by
 *     @*rewind bytes.
 *
 * @skip_ptr: the failing skip pointer (encodes child @G + skip_len).
 * @rewind:   out -- bytes the caller must back its descent cursor/level up by.
 * @at_pos:   out (may be NULL) -- the live node spanning/at the encoded position
 *            (the merge target for rewind > 0).  Accumulator walkers (nth /
 *            iter_skip) descend INTO it on rewind > 0, because re-scanning the
 *            shallower holder would re-count its already-counted contributions.
 *            Idempotent walkers (inequality minmax/sibling) and the precise
 *            lookup ignore it and just re-scan / re-read the returned holder.
 *
 * Returns the live node holding the slot equivalent to the failing one.  The
 * caller re-anchors its descent there and re-reads / re-descends.
 *
 * Return NULL when the slot is stale: a rekey moved @G to a shorter key
 * after the caller loaded the slot, so its chain no longer covers the
 * encoded length.  Only a reader in a move can see it.  The caller then
 * fails its pass, and its two-pass retries.  Guard exhaustion (a cycle)
 * also returns NULL.
 *
 * Read-side only (rcu_dereference on every back-pointer).  The caller must be
 * in an RCU read-side critical section.
 */
static
struct cds_ft_inode_flag *ft_skip_reanchor_impl(struct cds_ft *ft,
		struct cds_ft_inode_flag *skip_ptr,
		unsigned int *rewind, struct cds_ft_inode_flag **at_pos,
		bool reader)
{
	unsigned int want = ft_skip_len(skip_ptr);
	unsigned int acc = 0;
	struct cds_ft_inode_flag *cur = ft_skip_child_ptr(skip_ptr);	/* G */
	int guard;

	*rewind = 0;
	if (at_pos)
		*at_pos = NULL;
	FT_TP(reanchor_enter, (const void *) skip_ptr, (const void *) cur, want);
	for (guard = 0; guard < (int) FT_MAX_DEPTH + 2; guard++) {
		struct cds_ft_inode_flag *parent;
		struct cds_ft_inode_flag *head_word = NULL;
		void *pitem;

		if (ft_node_external(cur)) {
			/*
			 * Start from the chain HEAD.  @cur may be an interior
			 * member (ft_ext_head_word), and the head's position
			 * is the member's.  ONE load gives both halves of
			 * this hop: the parent the head names, AND whether
			 * it is that parent's prefix head
			 * (ft_upwalk_edge_bytes).  Keep the raw word.  The
			 * two halves then cannot come from different
			 * observations of a live re-home.  Stripping is what
			 * ft_resolve_head_prev does for callers that only
			 * want the node.
			 */
			struct cds_ft_node *head;

			head_word = ft_ext_head_word(ft,
				(struct cds_ft_node *) cur, &head, reader);
			cur = (struct cds_ft_inode_flag *) head;
			parent = ft_parent_prefix_strip(head_word);
		} else {
			parent = ft_parent_node(rcu_dereference(
				cds_ft_item_to_metadata(
				ft_node_ptr(cur))->parent_word));
		}
		/* A picked child's parent may be a flip-proxy mid-merge. */
		parent = ft_resolve_flip_proxy(parent);
		FT_TP(reanchor_walk, (const void *) cur, (const void *) parent, acc);
		if (caa_unlikely(!parent)) {
			/*
			 * THE SLOT IS STALE.  The walk ran out of parents before
			 * it covered the encoded length.  The only node with no
			 * parent is the root, and no skip pointer sits at the
			 * root.  So the chain from @G is now shorter than when
			 * the caller loaded the slot: a rekey moved @G to a
			 * shorter key since then.  This happens to a moved BARE
			 * KEY.  Its leaf is app-owned, so the rekey re-parents
			 * it in place instead of copying it (ft_rekey_cow_stop
			 * copies every other moved top, and the old copy keeps
			 * its chain).
			 *
			 * Only a reader can see this.  A writer is excluded from
			 * a rekey's commit by the FT-wide lock and the move
			 * gate's grace period.  A reader can see it only during
			 * a move, so it is on a two-pass path.  Return NULL: the
			 * caller fails its pass, and the two-pass retries.
			 */
			return NULL;
		}
		pitem = ft_node_compressed(parent) ?
			(void *) ft_compressed_node_ptr(parent) :
			(void *) ft_node_ptr(parent);
		acc += ft_node_compressed(parent) ?
			ft_compressed_node_ptr(parent)->len :
			ft_upwalk_edge_bytes(cur, head_word);
		if (acc >= want) {
			/*
			 * @parent is the node at the failing slot's encoded
			 * position.  It spans the position for rewind > 0 (a
			 * merge), and sits at it for rewind == 0.  Return the
			 * node HOLDING the slot: its parent.  For rewind == 0
			 * this is the scanned node's live version.  Also hand
			 * back @parent itself via @at_pos.  Accumulator
			 * walkers can then descend INTO it for rewind > 0,
			 * where re-scanning the shallower holder would
			 * double-count.
			 */
			struct cds_ft_inode_flag *holder;

			*rewind = acc - want;
			if (at_pos)
				*at_pos = parent;
			/* Same flip-proxy resolve as the up-walk read above. */
			holder = ft_parent_node_resolved(
				rcu_dereference(cds_ft_item_to_metadata(
				(struct cds_ft_inode *) pitem)->parent_word));
			/*
			 * The holder is NULL only if @parent is the root.
			 * The encoded position is then the root itself: a
			 * root-level skip pointer, which mutators never
			 * produce.  A stale slot ends here too, when the
			 * shorter chain covers the length exactly at the
			 * root: report it like the walk that ran out.
			 */
			return holder;
		}
		cur = parent;
	}
	return NULL;	/* pathological (cycle?): caller re-descends from root */
}

/* The read-side entry: the descent, the inequality and ordered-query walkers. */
static
struct cds_ft_inode_flag *ft_skip_reanchor(struct cds_ft *ft,
		struct cds_ft_inode_flag *skip_ptr,
		unsigned int *rewind, struct cds_ft_inode_flag **at_pos)
{
	return ft_skip_reanchor_impl(ft, skip_ptr, rewind, at_pos, true);
}

static inline
bool ft_group_skip_compressed(const struct cds_ft_group *group)
{
	return group->flags & CDS_FT_FLAG_SKIP_COMPRESSED;
}
#else
static inline
bool ft_node_skip_compressed(struct cds_ft_inode_flag *node __attribute__((unused)))
{
	return false;
}

static inline
unsigned int ft_skip_len(struct cds_ft_inode_flag *node __attribute__((unused)))
{
	return 0;
}

static inline
struct cds_ft_inode_flag *ft_skip_child_ptr(struct cds_ft_inode_flag *node)
{
	return node;
}

static
struct cds_ft_inode_flag *ft_skip_compressed_flag(
		struct cds_ft_inode_flag *child,
		unsigned int len __attribute__((unused)))
{
	return child;
}

static inline
struct cds_ft_compressed_node *ft_skip_to_compressed(const struct cds_ft *ft,
		struct cds_ft_inode_flag *skip_ptr)
{
	(void) ft;
	return ft_compressed_node_ptr(skip_ptr);
}


static inline
bool ft_group_skip_compressed(const struct cds_ft_group *group __attribute__((unused)))
{
	return false;
}

#endif /* FEATURE_FT_SKIP_COMPRESSED */

/*
 * Assert the root-is-internal invariant on a reader's root load.  See the
 * ft_root_dereference_* macros above: no mutator ever publishes a compressed /
 * skip-compressed node at the trie root, so a reader must never observe one.
 * Defined here (not at the macros) because it needs ft_node_skip_compressed,
 * whose real definition and no-skip stub both live in the block above.
 * Compiled out under NDEBUG.
 */
static inline
struct cds_ft_inode_flag *ft_root_assert_not_compressed(
		struct cds_ft_inode_flag *root)
{
	assert(!ft_node_compressed(root) && !ft_node_skip_compressed(root));
	return root;
}

/*
 * ft_set_parent_slot: record a node's slot within its parent as a
 * pointer-stride offset (parent_slot_offset).  The raw byte offset is
 * divided by sizeof(void *) (always 8 on 64-bit).  The 8-bit field can
 * then cover the full pigeon node: 2048 bytes / 8 = 256 slots, max
 * index 255.
 *
 * The offset is maintained for every internal/compressed node, not just
 * skip-compressed ones:
 *
 *   - it lets the parent-pointer backtrack recover a node's parent slot
 *     in O(1) without re-descending;
 *   - it backs the skip-compressed dual-pointer publish / chain-merge
 *     canonicalization.
 *
 * When parent is NULL (root's child), the offset is unused --
 * ft_get_parent_slot recovers &ft->root.
 */
/* Recover the branch byte indexing @slot in internal parent @node (defined
 * after the popcount layout helpers). */
static uint8_t ft_slot_to_byte(const struct cds_ft_type *type,
		struct cds_ft_inode *node, struct cds_ft_inode_flag **slot);

static inline
void ft_set_parent_slot(struct cds_ft_metadata *meta,
		struct cds_ft_inode_flag *parent,
		struct cds_ft_inode_flag **slot)
{
	struct cds_ft_inode_flag *p;
	bool parent_compressed;

	if (!slot)
		return;	/* Slot unknown -- preserve existing offset. */
	if (!parent) {
		ft_meta_parent_slot_offset_set(meta, 0);
		return;
	}
#ifdef FT_DEBUG_DEL_TOMB
	/*
	 * PROBE for the torn-pair producer.  Take a RAW offset store onto a
	 * node whose parent word already names a DIFFERENT real parent.  It is
	 * a re-parent that publishes its (parent, offset) pair in two separate
	 * stores.  Name the site.
	 */
	{
		struct cds_ft_inode_flag *oldp = ft_parent_node(
			(struct cds_ft_inode_flag *) CMM_LOAD_SHARED(meta->parent_word));
		static unsigned long ft_dt_raw_reparent;

		if (oldp && !ft_node_flip_proxy(oldp) &&
				ft_node_ptr(oldp) != ft_node_ptr(parent) &&
				uatomic_add_return(&ft_dt_raw_reparent, 1) <= 12) {
			void *bt[20];
			int nbt = backtrace(bt, 20);

			fprintf(stderr, "FT RAW-REPARENT-OFFSET meta=%p old_parent=%p new_parent=%p old_off=%u new_off=%u state=%#lx\n",
				(void *) meta, (void *) oldp, (void *) parent,
				(unsigned int) FT_PSO_DECODE(CMM_LOAD_SHARED(meta->parent_slot_offset)),
				(unsigned int) (((char *) slot - (char *) ft_node_ptr(parent)) / sizeof(void *)),
				(unsigned long) CMM_LOAD_SHARED(meta->state));
			backtrace_symbols_fd(bt, nbt, 2);
		}
	}
#endif
	ft_meta_parent_slot_offset_set(meta, (unsigned int)((char *) slot -
		(char *) ft_node_ptr(parent)) / sizeof(void *));
	/*
	 * Record this node's incoming branch byte for the up-walk key rebuild
	 * (ft_rebuild_key_upwalk).  This is THE central populate point for
	 * every slot-placed node (internal + compressed).  It runs from
	 * ft_set_parent AND ft_publish_to_parent.  So all publish paths are
	 * covered without threading the byte to each call site.  Derive the
	 * byte by inverting @slot against the parent's bitmap (cold path).
	 *
	 * The byte is only meaningful when the parent is an internal
	 * (slot-array) node.  A compressed parent has no slot array: the edge
	 * byte lives in its key_bytes.  So skip it.  The up-walk likewise
	 * skips a node whose parent is compressed.
	 */
	p = parent;
	parent_compressed = ft_node_compressed(p);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	parent_compressed = parent_compressed || ft_node_skip_compressed(p);
#endif
	if (!parent_compressed)
		meta->incoming_byte = FT_DT_IB_STORE(meta, ft_slot_to_byte(
			&ft_types[ft_node_type(p)], ft_node_ptr(p), slot));
}

/*
 * ft_get_parent_slot: recover a node's parent-slot address from the
 * stored pointer-stride offset.
 *
 * The node body IS the packed child-pointer array (metadata lives in a
 * sibling page).  So offset 0 is a valid slot: the node's first/lowest
 * child.  A placed non-root child therefore always has a meaningful
 * offset, including 0.  The "no recorded slot" state is fully captured by
 * parent == NULL (the root's child, recovered as &ft->root below).
 *
 * Do NOT treat offset 0 as "unset".  That aliases the lowest child of
 * every node and silently drops its skip re-encode.  ft_publish_to_parent
 * / ft_node_recompact would skip it on a child-change.  A stale skip
 * pointer would then stay in the parent slot.
 *
 * @ft is needed for the root case (parent == NULL).
 */
#ifdef FT_DEBUG_PAIR_STORE
/*
 * PROBE, SAME-OP TORN WINDOW.  ft_glue_record_back_edge used to store a LIVE
 * child's offset PLAIN and only RECORD its parent word.  So until the commit,
 * this very op's non-RYW ft_resolve_parent_slot would pair the OLD parent
 * with the NEW offset.  It now records both words, so it no longer feeds
 * this ring (ft_dt_glue_note has no caller).  The probe is kept for a future
 * site that splits the pair again.
 */
/* The ring itself sits in fractal-trie-internal.h: the writer scope clears it. */
static unsigned long ft_dt_glue_noted, ft_dt_glue_torn_read;

static inline
void ft_dt_glue_note(const struct cds_ft_metadata *meta,
		struct cds_ft_inode_flag *new_parent)
{
	unsigned int i = ft_dt_glue_ring_n++ % FT_DT_GLUE_RING;

	ft_dt_glue_ring[i].meta = meta;
	ft_dt_glue_ring[i].new_parent = new_parent;
	uatomic_inc(&ft_dt_glue_noted);
}

__attribute__((noinline))
static void ft_dt_glue_check(const struct cds_ft_metadata *meta,
		struct cds_ft_inode_flag *parent_seen)
{
	unsigned int i;

	for (i = 0; i < FT_DT_GLUE_RING; i++) {
		if (ft_dt_glue_ring[i].meta != meta)
			continue;
		if (ft_node_ptr(ft_parent_node(parent_seen)) ==
				ft_node_ptr(ft_dt_glue_ring[i].new_parent))
			return;		/* the window has closed */
		if (uatomic_add_return(&ft_dt_glue_torn_read, 1) <= 8) {
			void *bt[24];
			int nbt = backtrace(bt, 24);

			fprintf(stderr, "FT GLUE-TORN-READ meta=%p seen_parent=%p glue_parent=%p\n",
				(void *) meta, (void *) parent_seen,
				(void *) ft_dt_glue_ring[i].new_parent);
			backtrace_symbols_fd(bt, nbt, 2);
		}
		return;
	}
}

static void ft_dt_glue_report(void) __attribute__((destructor));
static void ft_dt_glue_report(void)
{
	fprintf(stderr, "FT GLUE-WINDOW noted=%lu torn_read=%lu\n",
		ft_dt_glue_noted, ft_dt_glue_torn_read);
}
#endif

/*
 * ft_resolve_parent_slot: recover a node's parent AND its parent-slot address as
 * a CONSISTENT snapshot.  It tolerates a mid-commit atomic re-home (Phase 4.3).
 *
 * A re-home commits @meta->parent_word (a type-7 flip-proxy) and
 * @meta->parent_slot_offset (FT_STATE_PROXY) as ONE 2-edge MCAS txn.
 * Resolving each field with a SEPARATE status load can tear across the
 * commit's status flip:
 *
 *   resolver                           re-home commit
 *   --------                           --------------
 *   read parent -> OLD
 *                                      flip the status
 *   read offset -> NEW
 *   compute a slot address off the
 *   wrong parent body
 *   ft_slot_to_byte: OOB
 *
 * So the two edges must be driven from a SINGLE status snapshot.  When
 * @meta->parent_word carries the proxy, take its commit's decision @t and
 * read it ONCE (ft_txn_decision_committed).  Resolve BOTH edges through it.
 * A re-home that begins mid-snapshot is caught by the coherence re-read of
 * @meta->parent_word and retried.
 *
 * The §8.3 split SHARPENED this.  The offset now has its own word, written
 * ONLY by a re-home.  So a proxy parked there is unambiguously @t's.  While
 * the offset shared @state, freezes and tombstones also parked on that word.
 * Those edges leave the offset untouched.  So the resolver had to tell a
 * co-committed offset edge apart from an unrelated state edge.  The
 * same-descriptor check below is still needed against a re-home racing the
 * snapshot.  But it no longer has to disambiguate two different KINDS of
 * parker.
 *
 * With no re-home in flight (single writer, or between commits), the fast
 * path is a plain parent load + a proxy-tolerant offset load.  Its behaviour
 * is identical to the pre-4.3 raw recovery.  @parent_out (optional) receives
 * the parent that matches the returned slot.  Call from within an RCU
 * read-side section.
 */
static inline
struct cds_ft_inode_flag **ft_resolve_parent_slot(
		const struct cds_ft_metadata *meta, struct cds_ft *ft,
		struct cds_ft_inode_flag **parent_out)
{
	struct cds_ft_inode_flag *parent;
	void *state;

	for (;;) {
		/*
		 * ACQUIRE, not rcu_dereference.  The offset load below does not
		 * depend on this value, so only an acquire orders it after this
		 * one.  Without it, this load can see a re-home's settled NEW
		 * parent and the offset load the OLD offset.  The re-read below
		 * then accepts the torn pair.  An acquire on the offset load
		 * orders nothing before it.
		 */
		struct cds_ft_inode_flag *praw =
			ft_dereference_acquire(meta->parent_word);
		void *sraw = uatomic_load(
				(void **) (uintptr_t) &meta->parent_slot_offset,
				CMM_ACQUIRE);

		if (caa_likely(!ft_node_flip_proxy(praw))) {
			/*
			 * No re-home parking @meta->parent.  The state word may
			 * still carry an unrelated proxy (a freeze that leaves the
			 * offset unchanged); resolve it independently.  A re-home
			 * that STARTS after this load is caught by the coherence
			 * re-read below.
			 */
			parent = praw;
			state = ft_txn_resolve(sraw, FT_STATE_PROXY);
		} else {
			struct ft_txn_parked *rp = ft_flip_proxy_ptr(praw);
			const struct ft_txn_decision *t =
				ft_txn_parked_decision(rp);
			bool committed = ft_txn_decision_committed(t);

			parent = (struct cds_ft_inode_flag *)
				ft_txn_parked_value(rp, committed);
			if (caa_likely(ft_txn_is_proxy(sraw, FT_STATE_PROXY))) {
				struct ft_txn_parked *rs =
					ft_txn_parked_of(sraw, FT_STATE_PROXY);

				if (caa_unlikely(ft_txn_parked_decision(rs) != t))
					continue;	/* stale offset edge: re-snapshot */
				state = ft_txn_parked_value(rs, committed);
			} else {
				/*
				 * Parent parked, offset plain.  The commit parks
				 * the parent word first, so the offset may not
				 * be parked yet.  It may also be unrecorded (the
				 * slot index did not change) or already settled.
				 * None of these can be told from here, so wait
				 * for the parent word.
				 */
				continue;
			}
		}
		/*
		 * Coherence guard: the snapshot is torn only if a re-home landed
		 * on @meta->parent between the two loads above.  A stable parent
		 * edge means (parent, offset) came from one consistent view.
		 */
		if (caa_likely(rcu_dereference(meta->parent_word) == praw))
			break;
	}

	/*
	 * The owner stamp is a WITNESS, never a navigation aid.  The slot
	 * comes from the caller's @ft, exactly as it did when a root's parent
	 * was NULL.  A cross-trie move legitimately leaves the stamp naming
	 * the PREVIOUS owner until it commits.  Deriving the slot from the
	 * stamp would then answer with a slot in the wrong trie mid-move.
	 * @ft is the trie the caller is actually operating on.  Reading
	 * ownership is cds_ft_verify's job, and it reads the word itself.
	 *
	 * @parent_out is the parent NODE, so a root still yields NULL.
	 */
#ifdef FT_DEBUG_PAIR_STORE
	if (caa_unlikely(ft_dt_glue_ring_n))
		ft_dt_glue_check(meta, parent);
#endif
	if (parent_out)
		*parent_out = ft_parent_node(parent);
	if (ft_parent_is_root_position(parent))
		return &ft->root;
	return (struct cds_ft_inode_flag **)
		((char *) ft_node_ptr(parent) +
		 FT_PSO_DECODE(state) * sizeof(void *));
}

static inline
struct cds_ft_inode_flag **ft_get_parent_slot(const struct cds_ft_metadata *meta,
		struct cds_ft *ft)
{
	return ft_resolve_parent_slot(meta, ft, NULL);
}

/*
 * ft_txn_parent_slot: @meta's parent slot as @mtxn WILL LEAVE IT.
 *
 * ft_resolve_parent_slot above answers from the words as they stand.  It
 * resolves a PEER's parked re-home through the flip proxy.  But an edge this
 * op has merely RECORDED is not parked yet.  It lives only in the descriptor,
 * so a raw derivation cannot see it.  Take an op that re-parents @meta and
 * then derives @meta's parent slot in the same attempt.  It gets the PRE-OP
 * slot.  When the re-parent came from a recompaction, that slot sits inside
 * the copy this very commit retires.
 *
 * So when the txn recorded either word, read both READ-YOUR-OWN-WRITES.  The
 * (parent, offset) pair then needs no coherence re-read.  A re-parent records
 * them together.  The op that recorded one holds the lock that keeps a peer
 * off the other.
 *
 * When it recorded neither, the pair is a PEER's to change, and a peer's
 * re-home may be parked on both words right now.  Two independent loads can
 * then tear.  The MCAS engine waits each one out until that commit decides.
 * But an SW proxy flips at any instant, so the parent could resolve old and
 * the offset new.  So take the reader's snapshot (ft_resolve_parent_slot).
 * It resolves both through one decision and re-reads the parent word.
 *
 * @mtxn NULL answers exactly as ft_get_parent_slot does.
 */
static inline
struct cds_ft_inode_flag **ft_txn_parent_slot_at(const struct cds_ft_metadata *meta,
		struct cds_ft *ft, struct urcu_txn *mtxn,
		struct cds_ft_inode_flag **parent_out)
{
	struct cds_ft_inode_flag *parent;
	void *state;

	if (parent_out)
		*parent_out = NULL;
	if (!mtxn
#ifndef FT_DEBUG_PARENT_SLOT_TWO_LOADS
	    || (!ft_txn_recorded(mtxn,
			(void **) (uintptr_t) &meta->parent_word) &&
		!ft_txn_recorded(mtxn,
			(void **) (uintptr_t) &meta->parent_slot_offset))
#endif
	   ) {
#ifdef FT_DEBUG_FOREIGN_PARKED
		if (mtxn)
			uatomic_inc(&ft_fp_ps_snapshot);
#endif
		return ft_resolve_parent_slot(meta, ft, parent_out);
	}
#ifdef FT_DEBUG_FOREIGN_PARKED
	uatomic_inc(&ft_fp_ps_ryw);
#endif
	parent = ft_txn_load(mtxn,
		(void **) (uintptr_t) &meta->parent_word, FT_FLIP_PROXY_TAG);
	state = ft_txn_load(mtxn,
		(void **) (uintptr_t) &meta->parent_slot_offset, FT_STATE_PROXY);
	if (ft_parent_is_root_position(parent))
		return &ft->root;
	/*
	 * @parent_out is the node the returned slot LIVES IN: the word that
	 * owns that slot (§8.2: a node's body is its own).  It is handed back,
	 * not re-derived by the caller.  Only the RYW load above can see a
	 * re-parent this very commit recorded.
	 */
	if (parent_out)
		*parent_out = ft_parent_node(parent);
	return (struct cds_ft_inode_flag **)
		((char *) ft_node_ptr(parent) +
		 FT_PSO_DECODE(state) * sizeof(void *));
}

#define ft_txn_parent_slot(meta, ft, mtxn)				\
	ft_txn_parent_slot_at((meta), (ft), (mtxn), NULL)

#ifdef FEATURE_FT_SKIP_COMPRESSED
/*
 * ☠ DECIDE THE SKIP_X DUAL UNDER ITS OWNER, NOT BEFORE IT.
 *
 * A compressed P published in skip form has a second word to keep in step:
 * the SKIP_X word in GP's slot.  An op decides whether it must refresh that
 * word by reading the slot, and the slot is GP's (§8.2).  Read without GP
 * held, the slot can hold a PEER's parked proxy.  That is a commit that
 * published or re-encoded the dual and has not settled yet.  GP is that
 * peer's until after its settle (locks are released after the txn).  So the
 * proxy's settled value is unknown here, and a proxy is not skip-encoded.
 *
 * Reading the proxy as "no dual" dropped the refresh.  The peer then settled
 * SKIP_X(H) while this op moved H under a new node.  The dual then named a
 * compressed node that no longer held H.  MEASURED on
 * inv_owned_prefix_dense_remove_all: an insert committed H -> prefix head of
 * N, 74 us into a peer's 1.9 ms park-to-settle window on the dual slot.  The
 * stale dual then failed cds_ft_verify, and remove_all spun on the torn
 * parentage.
 *
 * Counting a proxy as a possible dual is not enough either.  A slot that
 * holds a plain value at the read can be parked on by a peer right after it.
 * The recording site then meets the proxy (measured: 17 of 17 through
 * cds_ft_insert_replace -> ft_insert_compressed_past_child).  Nothing about
 * the slot can be decided before GP is held.
 *
 * So a taker takes GP whenever P is compressed and has a node GP.  The
 * recompact already does this up front.  The dual is decided under GP.
 * There, a peer that parks on the slot must hold GP, and has therefore
 * settled.  The only cost is a GP hold for a compressed P whose dual turns
 * out not to exist.
 *
 * The recording side of the same rule: a site that decides the dual while it
 * HOLDS GP must find the slot SETTLED.  A peer parked there would hold GP
 * too.  This is asked only where the op holds GP.  An attempt whose GP take
 * was refused is doomed (acquire_miss, discarded unpublished).  It may still
 * meet the holder's proxy on its way to that discard.
 */
#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG)
# include <execinfo.h>
__attribute__((noinline, cold, noreturn))
static void ft_dual_slot_unsettled(const char *fn, int line, void *slot, void *v)
{
	void *bt[24];
	int n = backtrace(bt, 24);

	fprintf(stderr, "FT DUAL SLOT UNSETTLED: %s:%d decides the SKIP_X dual "
		"on slot %p holding a parked proxy %p\n", fn, line, slot, v);
	backtrace_symbols_fd(bt, n, 2);
	fflush(stderr);
	abort();
}
# define FT_DUAL_SLOT_SETTLED_CHECK(slot, held)				\
	do {								\
		if (caa_unlikely((held) && (slot) &&			\
				ft_node_flip_proxy(CMM_LOAD_SHARED(*(slot))))) \
			ft_dual_slot_unsettled(__func__, __LINE__,	\
				(void *) (slot),			\
				(void *) CMM_LOAD_SHARED(*(slot)));	\
	} while (0)
#else
# define FT_DUAL_SLOT_SETTLED_CHECK(slot, held)	do { } while (0)
#endif
#endif /* FEATURE_FT_SKIP_COMPRESSED */

/*
 * ft_slot_in_node: is @slot one of @node_flag's OWN child slots?
 *
 * This is the inverse of ft_get_parent_slot, which computes every slot as the
 * node body plus a pointer-stride offset.  The node body IS the packed
 * child-pointer array.  It is (1 << type->order) bytes long and naturally
 * aligned.  So containment is an address range test.  A compressed node
 * carries exactly one child slot, @cn->child.
 *
 * Use this as the pairing test for a plan that holds two things:
 *   - a slot address INSIDE one node;
 *   - a separately-resolved pointer TO that node.
 * Each can be coherent on its own while the PAIR is not.  A peer can
 * republish the node between the descent that produced the slot and the load
 * that resolved the pointer.  The slot then addresses the retired body, and
 * the pointer names the fresh copy.  No expected-old on the holder slot can
 * see that, because the value there matches itself.  What is stale is the
 * premise that the descent established about the node.
 *
 * @node_flag must already be resolved (no flip proxy).  A skip form resolves
 * to the compressed node it names.  Return false for a value that holds no
 * child slot at all (NULL, an external chain).
 */
static inline
bool ft_slot_in_node(struct cds_ft_inode_flag *node_flag,
		struct cds_ft_inode_flag **slot)
{
	const char *body;

	if (!node_flag || !slot)
		return false;
	/* Skip before external: a skip pointer's low bits read as external. */
	node_flag = ft_skip_child_ptr(node_flag);
	if (ft_node_compressed(node_flag))
		return slot == &ft_compressed_node_ptr(node_flag)->child;
	if (!ft_node_internal(node_flag))
		return false;
	body = (const char *) ft_node_ptr(node_flag);
	if (!body)
		return false;
	return (const char *) slot >= body &&
		(const char *) slot < body +
			((size_t) 1 << ft_types[ft_node_type(node_flag)].order);
}

#if defined(FT_DEBUG_DEL_TOMB) || defined(FT_DEBUG_PAIR_STORE)
/*
 * ft_dt_pso_store_probe: measure the rule stated in the header of
 * ft_meta_parent_slot_offset_set.
 *
 * The rule: PLAIN stores may rewrite the (parent_word, parent_slot_offset)
 * PAIR only while the child is still INVISIBLE.  An invisible child is a
 * fresh recompaction copy that nobody can reach yet.  A live child that
 * readers can reach re-homes through ft_reparent_record_meta.  That function
 * commits both words in one flip, so that no reader can observe half of the
 * pair.
 *
 * The probe tells the two cases apart by the child's OWN reachability.  It
 * needs no clock:
 *   - a fresh copy inherits the pair of the node it REPLACES, so the slot
 *     that pair names still holds the OLD node;
 *   - a live child's pair names a slot that holds the child ITSELF.
 * So "the slot already holds me" means "a reader can reach me through this
 * very pair".  Rewriting the pair here takes one plain store per word.
 * Between the two stores, that reader sees a parent from before and an
 * offset from after.
 *
 * The probe returns silently when the question cannot be asked cleanly:
 *   - there is no parent yet;
 *   - a re-home is already parked on either word;
 *   - the child is a root child, whose offset is unused;
 *   - the store is a same-value republish, which changes nothing (the hot
 *     case of the in-place reserve);
 *   - the pair is ALREADY incoherent, which is another probe's business.
 * So a hit is a positive answer, never an artefact of the reading.
 */
static unsigned long ft_dt_pso_asked, ft_dt_pso_live, ft_dt_ib_asked,
	ft_dt_ib_changed, ft_dt_ib_live, ft_dt_pair_undecided, ft_dt_ib_ctl_live;

/*
 * Return true if a reader can REACH @meta.  The slot named by its CURRENT
 * (parent_word, offset) pair must hold it.  The same must hold at every hop
 * up to the root.  One hop is not enough.  A fresh cluster is
 * built bottom-up, so its fresh parent has a slot that holds the fresh child.
 * Nobody can reach either until the top is published.  The top's pair names
 * the LIVE parent, whose slot still holds the node being replaced.
 *
 * Return false (or undecided) whenever a hop cannot be read cleanly.  So a
 * true answer is a positive one.
 */
static inline
bool ft_dt_live_via_pair(const struct cds_ft_metadata *meta,
		bool *first_parent_internal)
{
	const struct cds_ft_metadata *m = meta;
	int hops;

	*first_parent_internal = false;
	for (hops = 0; hops < FT_MAX_DEPTH; hops++) {
		struct cds_ft_inode_flag *praw, *parent, *val;
		struct cds_ft_inode_flag **slot;
		uintptr_t cur;

		praw = (struct cds_ft_inode_flag *) CMM_LOAD_SHARED(m->parent_word);
		if (!praw || ft_node_flip_proxy(praw))
			return false;
		if (ft_parent_is_root_position(praw)) {
			struct cds_ft *owner;
			struct cds_ft_inode_flag *rv;

			/*
			 * Every hop below held its child.  At hop 0 @meta IS
			 * a root: its offset and byte are unused, so there is
			 * no question to answer.
			 */
			if (!hops)
				return false;
			/*
			 * ☠ A root-position WORD is not a root: a fresh node is
			 * stamped with its owning trie before anything publishes
			 * it.  The trie's root slot must hold it, and the trie
			 * must be one readers can enter.
			 */
			owner = ft_parent_trie(praw);
			if (owner->exclusive)
				return false;
			rv = (struct cds_ft_inode_flag *) CMM_LOAD_SHARED(owner->root);
			return rv && !ft_node_flip_proxy(rv) &&
				ft_node_ptr(ft_skip_child_ptr(rv)) ==
				cds_ft_metadata_to_item((struct cds_ft_metadata *) m);
		}
		parent = ft_parent_node(praw);
		if (!parent || ft_node_flip_proxy(parent) ||
				ft_node_external(parent))
			return false;
		if (!hops)
			*first_parent_internal = !ft_node_compressed(parent)
#ifdef FEATURE_FT_SKIP_COMPRESSED
				&& !ft_node_skip_compressed(parent)
#endif
				;
		cur = CMM_LOAD_SHARED(m->parent_slot_offset);
		if (cur & FT_STATE_PROXY)
			return false;
		slot = (struct cds_ft_inode_flag **) ((char *) ft_node_ptr(parent) +
				FT_PSO_DECODE(cur) * sizeof(void *));
		if (!ft_slot_in_node(parent, slot))
			return false;
		val = (struct cds_ft_inode_flag *) CMM_LOAD_SHARED(*slot);
		if (!val || ft_node_flip_proxy(val))
			return false;
		if (ft_node_ptr(ft_skip_child_ptr(val)) !=
				cds_ft_metadata_to_item((struct cds_ft_metadata *) m))
			return false;	/* the slot holds another node: invisible */
		m = ft_node_compressed(parent) ?
			cds_ft_item_to_metadata((struct cds_ft_inode *)
				ft_compressed_node_ptr(parent)) :
			cds_ft_item_to_metadata(ft_node_ptr(parent));
	}
	uatomic_inc(&ft_dt_pair_undecided);
	return false;
}

/* Hits per call site: the setter's caller, symbolized offline. */
#define FT_DT_PAIR_SITES	64
static struct {
	const void *ra;
	int kind;		/* 0 offset, 1 incoming_byte */
	unsigned long n;
} ft_dt_pair_sites[FT_DT_PAIR_SITES];
static pthread_mutex_t ft_dt_pair_lock = PTHREAD_MUTEX_INITIALIZER;

static void ft_dt_pair_hit(const void *ra, int kind)
{
	int i;

	pthread_mutex_lock(&ft_dt_pair_lock);
	for (i = 0; i < FT_DT_PAIR_SITES; i++) {
		if (!ft_dt_pair_sites[i].ra) {
			ft_dt_pair_sites[i].ra = ra;
			ft_dt_pair_sites[i].kind = kind;
		}
		if (ft_dt_pair_sites[i].ra == ra &&
				ft_dt_pair_sites[i].kind == kind) {
			ft_dt_pair_sites[i].n++;
			break;
		}
	}
	pthread_mutex_unlock(&ft_dt_pair_lock);
}

__attribute__((noinline))
static void ft_dt_pso_store_probe(const struct cds_ft_metadata *meta, unsigned int off)
{
	uintptr_t cur;
	bool internal;

	uatomic_inc(&ft_dt_pso_asked);
	cur = CMM_LOAD_SHARED(meta->parent_slot_offset);
	if (!(cur & FT_STATE_PROXY) && FT_PSO_DECODE(cur) == (uintptr_t) off)
		return;			/* same-value republish: the pair does not move */
	if (!ft_dt_live_via_pair(meta, &internal))
		return;
	uatomic_inc(&ft_dt_pso_live);
	ft_dt_pair_hit(__builtin_return_address(0), 0);
}

/*
 * ft_dt_ib_store_probe: a PLAIN store of @incoming_byte into a REACHABLE node,
 * CHANGING the byte.  The up-walk loads the parent and this byte separately, and
 * nothing parks this field.  Returns @byte.
 */
__attribute__((noinline))
static unsigned int ft_dt_ib_store_probe(const struct cds_ft_metadata *meta,
		unsigned int byte)
{
	bool internal;

	uatomic_inc(&ft_dt_ib_asked);
	if (meta->incoming_byte == byte) {
		/*
		 * POSITIVE CONTROL for the climb.  A same-value store is often
		 * the republish of a live node.  So a climb that can never
		 * answer "reachable" reads 0 here too.
		 */
		if (ft_dt_live_via_pair(meta, &internal) && internal)
			uatomic_inc(&ft_dt_ib_ctl_live);
		return byte;		/* same value: nothing a reader can see */
	}
	uatomic_inc(&ft_dt_ib_changed);
	/*
	 * Only a node reached through an INTERNAL parent: a compressed parent's
	 * child has no edge byte of its own, and readers skip it there.
	 */
	if (!ft_dt_live_via_pair(meta, &internal) || !internal)
		return byte;
	uatomic_inc(&ft_dt_ib_live);
	ft_dt_pair_hit(__builtin_return_address(0), 1);
	/* WHO: the setter's caller is out of reach of one return address. */
	if (uatomic_read(&ft_dt_ib_live) <= 8) {
		void *bt[24];
		int nbt = backtrace(bt, 24);

		fprintf(stderr, "FT PAIR-STORE-IB meta=%p byte %u -> %u\n",
			(void *) meta, (unsigned int) meta->incoming_byte, byte);
		backtrace_symbols_fd(bt, nbt, 2);
	}
	return byte;
}

static void ft_dt_pair_report(void) __attribute__((destructor));
static void ft_dt_pair_report(void)
{
	int i;

	fprintf(stderr, "FT PAIR-STORE offset: asked=%lu LIVE=%lu | incoming_byte: "
		"asked=%lu changed=%lu LIVE=%lu | undecided=%lu | control: same-value "
		"ib reachable=%lu (ra base: cds_ft_create=%p)\n", ft_dt_pso_asked,
		ft_dt_pso_live, ft_dt_ib_asked, ft_dt_ib_changed, ft_dt_ib_live,
		ft_dt_pair_undecided, ft_dt_ib_ctl_live, (void *) &cds_ft_create);
	for (i = 0; i < FT_DT_PAIR_SITES && ft_dt_pair_sites[i].ra; i++)
		fprintf(stderr, "FT PAIR-STORE-SITE kind=%s ra=%p n=%lu\n",
			ft_dt_pair_sites[i].kind ? "incoming_byte" : "offset",
			ft_dt_pair_sites[i].ra, ft_dt_pair_sites[i].n);
}
#endif


#ifdef FT_ENABLE_TRACING
#include <stdio.h>
#include <stdlib.h>
/*
 * Flight-recorder WRITE-side mis-wire validator (tracing builds only).
 *
 * The rule: a forward-slot publish whose NEW value is a PLAIN compressed
 * flag must name a live cn.  The (parent, PSO) pair of that cn must derive
 * the very slot being written.  Every legit producer (fresh publish,
 * recompact republish) wires the pair before publishing.  The SKIP_X dual
 * writes the skip FORM, which the plain-only filter exempts.
 *
 * These are mis-wires, caught AT CREATION:
 *   - a plain cn flag published into any OTHER slot;
 *   - a target that is tombstoned or has len==0.
 * The abort core shows exactly who built the edge, and the snapshot carries
 * the window.
 *
 * The check is wired at the _ft_publish_to_parent(_meta) entries, which are
 * the canonical forward-slot recorders.  It is NOT wired at the txn record
 * choke point.  Back-pointer FIELDS legitimately hold plain cn flags (a
 * child's meta->parent naming its fresh cn parent).  The check would fire
 * falsely there.
 */
static __attribute__((unused))
void ft_trace_pub_check(struct cds_ft *ft,
		struct cds_ft_inode_flag **slot, struct cds_ft_inode_flag *nf,
		unsigned int site)
{
	struct cds_ft_compressed_node *cn;
	struct cds_ft_metadata *meta;
	uintptr_t state;
	struct cds_ft_inode_flag *rt_parent;
	struct cds_ft_inode_flag **rt_slotp;

	if (!nf || !ft_node_compressed(nf))
		return;
	cn = ft_compressed_node_ptr(nf);
	meta = cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
	state = (uintptr_t) ft_txn_read((void **) &meta->state,
			FT_STATE_PROXY);
	rt_parent = ft_parent_node_resolved(
			rcu_dereference(meta->parent_word));
	rt_slotp = rt_parent ? ft_get_parent_slot(meta, ft) : NULL;
	if (caa_likely(cn->len != 0 && !(state & FT_STATE_TOMBSTONE) &&
			rt_slotp == slot))
		return;
	/*
	 * ☞ FREEZE BEFORE THE SNAPSHOT, as ft_trace_miswire_check does.  Peers
	 * that keep tracing while `lttng snapshot record` runs wrap every
	 * per-CPU ring.  They overwrite the culprit's window.
	 */
	FT_TRACE_FREEZE();
	FT_TP(miswire, site, (const void *) nf, (const void *) cn,
		(unsigned int) cn->len, state, (const void *) rt_parent,
		(const void *) rt_slotp);
	fprintf(stderr, "FT PUB-MISWIRE site %u slot %p new %p target %p "
		"len %u state %#lx rt_parent %p rt_slot %p\n",
		site, (void *) slot, (void *) nf, (void *) cn,
		(unsigned int) cn->len, (unsigned long) state,
		(void *) rt_parent, (void *) rt_slotp);
	(void) system("lttng snapshot record 1>&2");
	abort();
}
#ifndef FT_LIGHT_TRACING	/* see FT_TRACE_MISWIRE: drop the per-publish
				 * round-trip validator, keep the tracepoints. */
#define FT_TRACE_PUB_CHECK(ft, slot, nf, site) \
	ft_trace_pub_check(ft, slot, nf, site)
#else
#define FT_TRACE_PUB_CHECK(ft, slot, nf, site) do { (void) (ft); (void) (slot); (void) (nf); (void) (site); } while (0)
#endif
#else
#define FT_TRACE_PUB_CHECK(ft, slot, nf, site) do { } while (0)
#endif	/* FT_ENABLE_TRACING */

/*
 * ft_flag_to_metadata: get the metadata for any node flag, including
 * skip-compressed pointers.  For skip pointers, returns the
 * compressed node's metadata.  For all others, returns
 * cds_ft_item_to_metadata(ft_node_ptr(nf)).
 *
 * Caller must ensure nf is not NULL and not external.
 */
static inline
struct cds_ft_metadata *ft_flag_to_metadata(const struct cds_ft *ft,
		struct cds_ft_inode_flag *nf)
{
	(void) ft;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(nf)) {
		struct cds_ft_compressed_node *cn =
			ft_skip_to_compressed(ft, nf);
		return cds_ft_item_to_metadata(
			(struct cds_ft_inode *) cn);
	}
#endif
	return cds_ft_item_to_metadata(ft_node_ptr(nf));
}

/*
 * ft_flag_tombstoned: has this node flag been RETIRED?
 *
 * A retired node keeps its body readable (RCU).  But its state word carries
 * FT_STATE_TOMBSTONE, and every fence primitive refuses it.  So an op whose
 * plan names a retired node can never make progress.  It must re-derive its
 * plan, not retry.  An EXTERNAL head has no state word and is never
 * "tombstoned" in this sense.  Its removal is signalled on node->next (see
 * ft_node_is_removed).
 */
static inline
bool ft_flag_tombstoned(const struct cds_ft *ft,
		struct cds_ft_inode_flag *nf)
{
	struct cds_ft_metadata *meta;

	if (!nf || ft_node_external(nf))
		return false;
	meta = ft_flag_to_metadata(ft, nf);
	return meta != NULL &&
		(CMM_LOAD_SHARED(meta->state) & FT_STATE_TOMBSTONE) != 0;
}

/*
 * If @nf is a skip-compressed pointer, return the underlying
 * compressed node's flag pointer.  Otherwise return @nf unchanged.
 *
 * Use to "see through" the skip-compressed encoding when about to
 * inspect or recurse into the underlying compressed node.  No-op for
 * non-skip pointers; on archs without FEATURE_FT_SKIP_COMPRESSED the
 * check is constant-folded to false and the call collapses to a copy.
 */
static inline
struct cds_ft_inode_flag *ft_resolve_skip_compressed(const struct cds_ft *ft,
		struct cds_ft_inode_flag *nf)
{
	(void) ft;
	if (ft_node_skip_compressed(nf))
		return ft_compressed_node_flag(ft_skip_to_compressed(ft, nf));
	return nf;
}

/*
 * ft_reanchor_flag: resolve a child flag that may be skip-compressed to a LIVE
 * node.  The flag must already be flip-proxy-resolved.
 *
 * Resolve via the read side's ft_skip_reanchor, not via the unvalidated
 * one-hop ft_skip_to_compressed.  ft_skip_reanchor walks the skip child's
 * live parent chain to the trie position that skip_len encodes.
 *
 * This is the MW-safe replacement for ft_resolve_skip_compressed on the WRITE
 * path.  Under mutual exclusion a writer could trust the one-hop
 * back-pointer: no peer could move the structure under its own descent.
 * Under MW a peer split/merge can tear that back-pointer.  It then leads to
 * one of:
 *   - internal memory (type confusion: cn->len reads a bitmap byte);
 *   - a stale-length node (mis-file).
 * The read side already tolerates exactly this by re-anchoring.  The update
 * side uses that same mechanism here.
 *
 * Set @*rewind_ret > 0 iff a concurrent chain-merge moved the encoded
 * position shallower than the dispatched child.  A caller that captured the
 * raw slot then finds it at the wrong level, and re-descends.  Return a
 * non-skip @child unchanged, with rewind 0.  On a stale slot (see
 * ft_skip_reanchor_impl), return NULL with @*rewind_ret set to
 * FT_REANCHOR_TORN.  RCU-read-side safe.
 */
/* The rewind ft_reanchor_flag reports for a stale slot. */
#define FT_REANCHOR_TORN	UINT_MAX

static inline
struct cds_ft_inode_flag *ft_reanchor_flag(struct cds_ft *ft,
		struct cds_ft_inode_flag *child, unsigned int *rewind_ret)
{
	*rewind_ret = 0;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (caa_unlikely(child && ft_node_skip_compressed(child))) {
		struct cds_ft_inode_flag *at_pos, *anchor;

		/* Write path: its hops are not the oracle's positive control. */
		anchor = ft_skip_reanchor_impl(ft, child, rewind_ret, &at_pos,
				false);
		if (caa_unlikely(!anchor)) {
			/*
			 * A stale slot.  Every caller treats a non-zero
			 * rewind as "re-descend" or as a conflict to retry.
			 */
			*rewind_ret = FT_REANCHOR_TORN;
			return NULL;
		}
		return at_pos;
	}
#else
	(void) ft;
#endif
	return child;
}

#ifdef FEATURE_FT_SKIP_COMPRESSED
/*
 * ft_skip_to_compressed_meta: shorthand to get the compressed node's
 * metadata from a skip pointer.
 */
static inline
struct cds_ft_metadata *ft_skip_to_compressed_meta(struct cds_ft *ft,
		struct cds_ft_inode_flag *skip_ptr)
{
	return cds_ft_item_to_metadata(
		(struct cds_ft_inode *) ft_skip_to_compressed(ft, skip_ptr));
}
#endif

/*
 * ft_publish_to_parent: atomically publish @new_child into @parent_slot.
 *
 * If the parent is a compressed node, also update the skip pointer
 * at *skip_slot (if one exists) BEFORE writing *parent_slot.  Candidate
 * readers follow the skip pointer.  Exact/inequality readers follow
 * cn->child.  This order makes candidate readers see the new child first.
 *
 * For compressed-form @new_child (SKIP_X or plain COMPRESSED), also
 * maintain the underlying compressed node's parent_slot_offset.  It
 * records @parent_slot's offset in @parent_nf.  ft_get_parent_slot
 * lookups require it:
 *   - the dual-pointer update above;
 *   - the chain-merge canonicalization in ft_detach_node, which publishes
 *     a replacement at the cn's same grandparent slot.
 * Without this, compressed nodes installed via ft_publish_to_parent rather
 * than via ft_node_set_nth -> ft_set_parent leave parent_slot_offset == 0.
 * That latent gap silently disabled the dual-pointer SKIP_X update and
 * tripped chain-merge.
 *
 * This intentionally does NOT update @new_child's parent linkage.  Callers
 * manage that via their own ft_set_parent, or by direct meta->parent
 * assignment.  The semantics vary across call sites.
 *
 * This function centralizes the dual-pointer RCU publication pattern.  So
 * every write to cn->child automatically maintains the skip pointer.
 *
 * When @rec is non-NULL, the reader-visible stores are RECORDED into @rec
 * and not performed.  There are one or two of them: the forward parent
 * slot, and a compressed parent's SKIP_X dual pointer.  A key-disappearing
 * remove can then commit them in one flip with the dead head cell's
 * ordered-list unsplice (ft_detach_node).  All bookkeeping that readers do
 * not see (parent-slot offset, trace events) still runs.  The public
 * ft_publish_to_parent wrapper passes NULL (direct stores, original
 * behaviour).
 */
#ifdef FT_DEBUG_DUAL_SITE
/*
 * WHICH PRODUCER STILL SAYS "owner NAMED, not held" (build knob;
 * -DFT_DEBUG_DUAL_SITE).
 *
 * The DUAL_NAMED column is the SKIP_X dual's whole conversion surface.  It is
 * recorded in THREE places, which all key on @owner_held.  None of them can
 * name the PRODUCER that decided the answer.  Editing a producer without
 * first attributing the population is editing blind.  So attribute it here.
 * @fn/@line are the CALLER's, exactly as ft_hlist_store_mw_at does it for
 * chain words.
 */
# define FT_DUAL_SITE_MAX	24
static struct {
	const char *fn;
	int line;
	unsigned long named_unheld, named_held, unnamed, root;
} ft_dual_site[FT_DUAL_SITE_MAX];
static unsigned int ft_dual_site_n;

static
void ft_dual_site_tally(const char *fn, int line, bool root,
		struct cds_ft_metadata *owner, bool owner_held)
{
	unsigned int i;

	for (i = 0; i < ft_dual_site_n; i++)
		if (ft_dual_site[i].fn == fn && ft_dual_site[i].line == line)
			break;
	if (i == ft_dual_site_n) {
		if (ft_dual_site_n == FT_DUAL_SITE_MAX)
			return;
		ft_dual_site[i].fn = fn;
		ft_dual_site[i].line = line;
		ft_dual_site_n++;
	}
	if (root)
		uatomic_inc(&ft_dual_site[i].root);
	else if (!owner)
		uatomic_inc(&ft_dual_site[i].unnamed);
	else if (owner_held)
		uatomic_inc(&ft_dual_site[i].named_held);
	else
		uatomic_inc(&ft_dual_site[i].named_unheld);
}

static void ft_dual_site_report(void) __attribute__((destructor));
static void ft_dual_site_report(void)
{
	unsigned int i;

	fprintf(stderr, "FT DUAL-SITE (per ft_pub_rec_add producer)\n");
	for (i = 0; i < ft_dual_site_n; i++)
		fprintf(stderr, "  %-44s:%-5d named_unheld=%lu named_held=%lu "
			"unnamed=%lu root=%lu\n",
			ft_dual_site[i].fn, ft_dual_site[i].line,
			uatomic_read(&ft_dual_site[i].named_unheld),
			uatomic_read(&ft_dual_site[i].named_held),
			uatomic_read(&ft_dual_site[i].unnamed),
			uatomic_read(&ft_dual_site[i].root));
}
#endif /* FT_DEBUG_DUAL_SITE */

/*
 * ☞ THE RECORD KIND ASKS ONE QUESTION: is this op EXCLUDED from every other
 * writer of @slot?  SW is legal exactly when the answer is yes.  The answer
 * has two independent sources:
 *   - a lock this op took;
 *   - a mode in which no peer writer can exist at all.
 *
 * ☑ COARSE: excluded ALREADY, whatever the word is.  A coarse trie takes the
 * FT-wide @writer_lock at its outermost writer scope.  So every writer of
 * every slot is serialised.  This INCLUDES &ft->root, which owns no node and
 * can never be locked.  The root slot's MW is a property of FINE locking, not
 * of the root.
 *
 * ☠ FINE / EXPONENTIAL: the FT-wide lock is dropped all-at-once.  So
 * exclusion comes only from the per-node lock the op took, and &ft->root HAS
 * NO NODE TO TAKE.  There the root slot stays MW.  Every other word answers
 * with the op's own acquire (@owner_held).
 *
 * Passing this answer, and not a constant, lets the producers convert one by
 * one.  That holds ONLY because of what it ASKS.  MW-under-lock and
 * SW-under-lock do serialise.  So a site spelled SW never races one still
 * spelled MW *that holds the word*.
 *
 * ☠ THAT QUALIFIER IS THE WHOLE SAFETY ARGUMENT.  Without it, this is not a
 * licence.  An MW record whose op holds NOTHING is arbitrated against
 * nothing.  A peer's SW park is a plain store, so the two really race.
 * Converting per-site is safe here because a site that cannot vouch answers
 * false and STAYS MW.  It is never safe because "MW and SW serialise" on its
 * own.  For a word that carries no lock in its own bits, urcu_txn_store_sw
 * in <urcu/rcu-txn.h> is categorical: "a slot is SW xor MW, globally."
 */
#ifdef FT_DEBUG_DUAL_SITE
/*
 * WHY did this slot come out excluded?  The DUAL-SITE table's named_held column
 * mixes three different answers.  Only ONE of them is a fine-trie SW park
 * earned by @owner_held.  The others are modes in which no peer writer exists
 * at all.  A conversion argument that reads the merged column cannot tell
 * "this producer vouches" from "this row happened to run coarse".  That is
 * the one-bucket-zero trap this file has been caught by before.
 */
unsigned long ft_pub_excl_coarse, ft_pub_excl_wlock, ft_pub_excl_owner,
	ft_pub_excl_none;
static void ft_pub_excl_report(void) __attribute__((destructor));
static void ft_pub_excl_report(void)
{
	fprintf(stderr, "FT PUB-EXCL coarse=%lu wlock=%lu owner_FINE=%lu not_excluded=%lu\n",
		uatomic_read(&ft_pub_excl_coarse),
		uatomic_read(&ft_pub_excl_wlock),
		uatomic_read(&ft_pub_excl_owner),
		uatomic_read(&ft_pub_excl_none));
}
#endif

static inline
bool ft_pub_slot_excluded(const struct cds_ft *ft,
		struct cds_ft_inode_flag **slot, bool owner_held)
{
	if (!ft->lock_fine) {
#ifdef FT_DEBUG_DUAL_SITE
		uatomic_inc(&ft_pub_excl_coarse);
#endif
		return true;		/* FT-wide writer lock serialises all */
	}
	/*
	 * ☑ AND A FINE TRIE INSIDE A BULK WINDOW.  G5.25 has a fine trie
	 * RE-TAKE the FT-wide @writer_lock while a bulk op is live.  Point ops
	 * take it too for the duration.  So an op holding it excludes every
	 * other writer of every slot.  That is the same exclusion coarse mode
	 * has, for as long as the window lasts.  The glue publish makes this
	 * statement in prose ("its exclusion over the SKIP_X dual's
	 * grandparent is the FT-WIDE WRITER LOCK").  Asking @ft_wlock_held
	 * makes it a QUERY every producer answers for itself, not a per-site
	 * claim.
	 *
	 * ☞ It is also the exact predicate FT_OWNER_ASSERT_OWNED was taught to
	 * accept.  So a true here is checked by the same test that would trap
	 * it.
	 *
	 * ☠☠ AND IT RESTS ON ft_bulk_gate_enter's GRACE PERIOD, not on the flag.
	 * The POINT OP takes @bulk_active with a PLAIN LOAD at its writer-scope
	 * enter.  So a point op that sampled the gate CLEAR skipped the FT-wide
	 * lock, and is NOT excluded against the bulk op.  That is a mixed
	 * regime, and holding the lock would prove nothing on its own.  The
	 * gate's own discipline closes it: "publish, one full GP, only THEN
	 * mutate".  The gate's comment names this exact population.  The GP
	 * cannot complete while such a point op is still inside its read
	 * section.  So by the time a bulk op publishes anything:
	 *   - pre-flip ops have DRAINED;
	 *   - post-flip ops OBSERVE the gate and take the lock.
	 *
	 * ⇒ THIS CLAUSE SILENTLY BECOMES UNSOUND if that
	 * update_synchronize_rcu() is ever removed, made conditional, or moved
	 * after the first mutation.  An SW park would then sit beside a
	 * concurrent point op's MW CAS, with no assert to catch it.  The
	 * dependency is one-way and cross-file.  So it is written here, not
	 * left to be re-derived.
	 */
	if (ft_wlock_held == (struct cds_ft *) ft) {
#ifdef FT_DEBUG_DUAL_SITE
		uatomic_inc(&ft_pub_excl_wlock);
#endif
		return true;
	}
#ifdef FT_DEBUG_DUAL_SITE
	if (slot != &ft->root && owner_held)
		uatomic_inc(&ft_pub_excl_owner);
	else
		uatomic_inc(&ft_pub_excl_none);
#endif
	return slot != &ft->root && owner_held;
}

static
void ft_pub_rec_add_at(const char *fn, int line,
		struct ft_pub_rec *rec, struct cds_ft_inode_flag **slot,
		struct cds_ft_inode_flag *expected_old,
		struct cds_ft_inode_flag *new_val, bool root,
		struct cds_ft_metadata *owner, bool owner_held)
{
	assert(rec->n < 3);
#ifdef FT_DEBUG_DUAL_SITE
	ft_dual_site_tally(fn, line, root, owner, owner_held);
#else
	(void) fn; (void) line;
#endif
	/*
	 * @owner_held: does the OP hold @owner's lock?  It is separate from
	 * @owner because a NULL @owner does NOT fail closed.  The dispatching
	 * recorder branches on the txn's structural_sw alone.  So naming no
	 * owner and holding no owner are the same to it.  false records MW.
	 */
	rec->owner_held[rec->n] = owner_held;
	rec->slot[rec->n] = slot;
	/*
	 * @owner: the node whose lock excludes every other writer of @slot.
	 * Every slot here is a BODY slot: a forward child pointer or a SKIP_X
	 * dual.  mw-writer-lock-escalation-model.md §8.2 puts a node's body
	 * under that node's own lock.  So the owner is the node the slot LIVES
	 * IN, never the child it points at.
	 *
	 * @owner is NULL in two cases:
	 *   - the producer cannot name an owner;
	 *   - the slot is a ROOT slot.  It has no owning node at all, and
	 *     takes the always-MW route through @root instead.
	 * A producer that leaves @owner unset only declines to convert (see
	 * struct ft_pub_rec).  So a missing owner is safe and merely counts
	 * OWN_MISS.  EVERY producer did that before this parameter existed.
	 */
	rec->owner[rec->n] = owner;
	/*
	 * @root: a TRIE ROOT slot, which every replay must record MW (see
	 * struct ft_pub_rec).  It is a parameter, not a re-derivation.  Only
	 * the caller holds the trie the slot would be compared against, and a
	 * cross-trie op holds two.
	 */
	rec->root[rec->n] = root;
	/*
	 * @expected_old is the value the slot held in the snapshot the
	 * publishing PLAN was derived from.  It is NOT a fresh *slot re-read
	 * at record time.  A peer may publish into this slot between the plan
	 * and the record.  Recording the plan snapshot makes the commit-time
	 * MCAS reject (abort) that case.  A fresh raw capture would match the
	 * peer's value and ratify the stale plan (CORE_682870 defect #1:
	 * "stale plan ratified by fresh expected-old").
	 */
	rec->old_val[rec->n] = expected_old;
	rec->new_val[rec->n] = new_val;
	rec->n++;
}

#define ft_pub_rec_add(rec, slot, expected_old, new_val, root, owner, held) \
	ft_pub_rec_add_at(__func__, __LINE__, (rec), (slot), (expected_old), \
		(new_val), (root), (owner), (held))


/*
 * IS THE SKIP_X DUAL'S HOME A NODE THIS COMMIT BUILT?
 *
 * The dual lives in @cn's OWN parent.  The publish resolves that parent
 * READ-YOUR-OWN-WRITES.  So when the SAME commit re-parents @cn, the dual's
 * home MOVES.  It moves from the live node the descent fenced to the fresh
 * copy this op built.  A fresh copy is BUILD-INVISIBLE: no peer can reach
 * it, so no lock is needed and none is taken.  A transacted record that
 * names it as owner names a word the commit cannot be shown to own.  No
 * owner encoding fixes that.  The txn publishes REACHABILITY, not INTERIORS.
 * A private node's body is wired by PLAIN STORES, and the dual write is
 * exactly that.
 *
 * ☠ THE STORE IS DEMOTED, NEVER DROPPED.  The recompact copy loop resolved the
 * old home's slots to their COMMITTED values.  So the fresh copy is born
 * holding the skip pointer to the OLD child.  That pointer is stale the
 * instant the forward publish lands.  Skipping the write would leave a
 * candidate reader a shortcut straight to a retired node.
 *
 * ☞ THE TEST IS "DID THIS TXN RE-HOME @cn", asked of the DESCRIPTOR.
 * urcu_txn_load cannot answer it.  With no record on the slot it falls
 * through to a fresh read, which looks the same as "not re-homed".  The
 * divergence of the two derivations is a weaker witness of the same fact.
 *
 * A ROOT dual (&ft->root) is excluded whatever the descriptor says:
 *   - it lives in no node;
 *   - it is reader-visible at all times;
 *   - it takes the always-MW root route.
 *
 * ☠ THIS LEANS ON ONE INVARIANT: a re-parent target is a FRESH cluster, never
 * a live node.  ft_reparent_record_meta says so, and every producer in the
 * tree obeys it today.  A future re-homer that targets a LIVE node would
 * silently turn this dispatch into a plain store into a live body.  The
 * debug arm below would catch it: a re-home that did not MOVE the home
 * cannot be private.
 */
static inline
bool ft_dual_home_is_private(struct cds_ft *ft, const struct ft_pub_rec *rec,
		struct cds_ft_metadata *cn_meta,
		struct cds_ft_inode_flag **skip_slot,
		struct cds_ft_inode_flag *skip_owner_nf)
{
	if (!rec || !rec->mtxn || skip_slot == &ft->root || !skip_owner_nf)
		return false;
	if (!ft_txn_recorded(rec->mtxn,
			(void **) (uintptr_t) &cn_meta->parent_word))
		return false;
#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG)
	{
		/*
		 * THE INVARIANT, CHECKED WHERE IT IS CHEAP.  A re-home that did
		 * not MOVE the home cannot be private.  This arm would notice a
		 * future re-parent target that is a LIVE node.  It is guarded
		 * on the same pair urcu_assert_debug itself is.  So the raw
		 * re-resolution costs a release build nothing.
		 */
		struct cds_ft_inode_flag *raw_owner = NULL;

		(void) ft_txn_parent_slot_at(cn_meta, ft, NULL, &raw_owner);
		urcu_assert_debug(raw_owner != skip_owner_nf);
	}
#endif
	return true;
}

#ifdef FT_DEBUG_DUAL_DROP
/*
 * THE SKIP_X DUAL INVARIANT DETECTOR (build knob; -DFT_DEBUG_DUAL_DROP).
 *
 * THE INVARIANT: while a compressed node @cn is reached through a SKIP_X word
 * in its own parent, that word encodes cn->child.  Every op that republishes
 * cn->child must refresh BOTH the forward slot and the dual.  It must do so
 * in ONE commit, and into the LIVE home.
 *
 * DUAL-STALE: the dual is about to be emitted and the word ALREADY names a
 * child other than the plan's expected old.  The invariant is broken, and
 * every later attempt on this chain can only abort.  The op then retries
 * obstruction-free forever, and allocates a descriptor per attempt.  The
 * probe ABORTS, because that is the shape a regression takes and a livelock
 * reports nothing.
 *
 * ☞ THIS PREDICATE IS SPACING-INDEPENDENT: it reads the DUAL WORD itself, not
 * a lock word.  An earlier version of this probe also counted "the dual was
 * recorded while GP's own state word carried FT_STATE_LOCK" as the exposure
 * figure.  That IS a wrong zero above per-node spacing.  There, the lock the
 * op would contend for lives on GP's ANCHOR, and GP's own word reads clean.
 * The arm yield for the exclusion now lives where the exclusion does:
 * ft_lock_skip_dual_gp.
 */
# include <stdio.h>
# include <stdlib.h>
# include <execinfo.h>

static
void ft_dbg_dual_probe(struct cds_ft *ft,
		struct cds_ft_compressed_node *cn,
		struct cds_ft_metadata *cn_meta,
		struct cds_ft_inode_flag **skip_slot,
		struct cds_ft_inode_flag *skip_owner_nf,
		struct cds_ft_inode_flag *expected_old,
		struct cds_ft_inode_flag *new_child)
{
	struct cds_ft_inode_flag *raw = skip_slot ? *skip_slot : NULL;

	(void) ft;
	if (!skip_slot || !ft_node_skip_compressed(raw))
		return;			/* no dual is emitted */
	if (ft_skip_child_ptr(raw) == expected_old)
		return;			/* the invariant holds */
	fprintf(stderr,
		"FT DUAL-STALE cn=%p len=%u child=%p slot=%p raw=%p owner=%p exp_old=%p new=%p pw=%p off=%u\n",
		(void *) cn, cn->len, (void *) cn->child, (void *) skip_slot,
		(void *) raw, (void *) skip_owner_nf, (void *) expected_old,
		(void *) new_child, (void *) cn_meta->parent_word,
		ft_meta_parent_slot_offset_load(cn_meta));
	{
		void *bt[24];
		int n = backtrace(bt, 24);

		backtrace_symbols_fd(bt, n, 2);
	}
	fflush(stderr);
	abort();
}
#endif /* FT_DEBUG_DUAL_DROP */

static
void _ft_publish_to_parent_meta_at(const char *pub_fn, int pub_line,
		struct cds_ft *ft,
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **parent_slot,
		struct cds_ft_inode_flag *new_child,
		struct cds_ft_inode_flag *expected_old,
		struct cds_ft_metadata *new_child_meta,
		void *folded_child_prev,
		struct ft_pub_rec *rec,
		struct cds_ft_inode_flag *slot_owner_nf,
		bool dual_owner_held)
{
	/*
	 * @slot_owner_nf: the node @parent_slot LIVES IN, i.e. the word that owns
	 * the forward edge (§8.2: a node's body is its own).  It is almost
	 * always @parent_nf, which is why _ft_publish_to_parent defaults it.
	 * But NOT always, and the difference cannot be derived here.
	 * @parent_nf has an OTHER job: it decides whether a compressed
	 * parent's SKIP_X dual is re-emitted.  A caller may pass @parent_nf
	 * for that job while it publishes into a slot that lives somewhere
	 * else entirely (ft_store_at_graft_point_commit's relocation
	 * republish).  Deriving the owner from @parent_nf there names a node
	 * the op does not hold.  The record then reports an exclusion gap
	 * that is not real.
	 */
	/*
	 * @expected_old: the value @parent_slot held in the snapshot the
	 * caller's publish plan was derived from.  Only the RECORDED path
	 * uses it.  The direct rec==NULL arms below republish a same value
	 * and ignore it.  The compressed-parent SKIP_X dual mirrors the same
	 * child.  So its plan-snapshot value is
	 * ft_skip_compressed_flag(expected_old, cn->len).  Both edges commit
	 * against the plan, not against a record-time re-read.
	 */
	/*
	 * @new_child_meta (optional): @new_child's metadata, supplied by the
	 * caller so we DON'T recover it from the slot value.  It is required
	 * when the caller defers @new_child's back-pointer into the same
	 * flip-txn as this publish.  A SKIP_X @new_child is resolved to its
	 * compressed node via ft_skip_to_compressed.  That reads the parent
	 * of new_child's child, which is precisely the deferred
	 * (not-yet-stored) back-edge.  Passing the metadata directly avoids
	 * that stale read.  NULL = recover as before (the back-edge was
	 * wired up front).
	 *
	 * @folded_child_prev (optional): the EXTERNAL analogue.  An external
	 * @new_child carries no metadata: its parent resolves through
	 * prev -> cell -> parent.  The caller may fold @new_child's prev into
	 * this publish's flip-txn (a head promote / swap).  The
	 * forward-before-parent check below would then read the
	 * not-yet-stored prev.  Passing the prev's intended value lets the
	 * check validate the folded parent instead.  NULL = read
	 * new_child->prev as before.  Debug-check only.
	 *
	 * Publication-ordering invariant: a child becomes observable by
	 * downward traversal the instant it is published into a live parent
	 * slot.  So its parent back-pointer MUST already be wired.  Otherwise
	 * a concurrent reader that descends to it and walks back up
	 * (ft_skip_reanchor / ordered up-walk) reads a NULL/uninitialized
	 * parent.  This applies to ALL child kinds (external -> prev,
	 * internal/compressed -> metadata->parent).  The root slot is the
	 * sole exception: the root has no parent.  The check below catches
	 * forward-before-parent bugs at their source.
	 */
	/* Flight-recorder write-side mis-wire validator (tracing builds only). */
	FT_TRACE_PUB_CHECK(ft, parent_slot, new_child, 4);
#ifndef NDEBUG
	if (new_child && parent_slot != &ft->root) {
		struct cds_ft_inode_flag *cp;

		/*
		 * Check skip-compressed FIRST.  A SKIP_X flag carries its
		 * (external) child's low tag bits.  So ft_node_external() would
		 * misclassify it and dereference the tagged flag as a node.
		 * The caller-supplied metadata short-circuits the SKIP_X
		 * recovery, which would read the deferred back-edge.
		 */
		if (new_child_meta) {
			cp = ft_parent_node(new_child_meta->parent_word);
		} else
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_node_skip_compressed(new_child)) {
			cp = ft_parent_node(cds_ft_item_to_metadata(
				(struct cds_ft_inode *)
				ft_skip_to_compressed(ft, new_child))->parent_word);
		} else
#endif
		if (ft_node_external(new_child)) {
			/*
			 * Cell-always: prev is the (non-NULL) cell pointer even
			 * when the parent is unset.  So resolve through the
			 * cell to preserve the forward-before-parent check on
			 * cell->parent.  A caller that folds the prev into this
			 * publish's txn supplies the intended value
			 * (@folded_child_prev).  We then validate the folded
			 * parent, not the not-yet-stored slot.
			 */
			cp = ft_resolve_head_prev(ft, folded_child_prev ?
				folded_child_prev :
				((struct cds_ft_node *) new_child)->prev);
		} else {
			cp = ft_parent_node(cds_ft_item_to_metadata(
				ft_node_ptr(new_child))->parent_word);
		}
		assert(cp != NULL);
	}
#endif /* !NDEBUG */
	(void) folded_child_prev;	/* debug-check only (see above) */
	/*
	 * Maintain @new_child's parent-slot offset (parent_slot_offset) so
	 * that it records the slot holding it within its parent node.
	 * ft_get_parent_slot(child_meta) recovers this value later.  It has
	 * three users:
	 *   - the parent-pointer backtrack, to find a node's slot in O(1)
	 *     without re-descending;
	 *   - dual-pointer publishes from cn->child (ft_publish_to_parent
	 *     itself, when called with parent_nf = cn);
	 *   - chain-merge canonicalization (ft_detach_node), which publishes
	 *     a replacement at the same slot.
	 *
	 * The offset is maintained for EVERY internal/compressed child, not
	 * just compressed: the field is no longer skip-specific.  Here is
	 * what the store prevents:
	 *   - On skip-on builds, compressed nodes installed via
	 *     ft_publish_to_parent (rather than via ft_node_set_nth, which
	 *     routes through ft_set_parent) would leave
	 *     parent_slot_offset == 0.  That latent gap silently disabled the
	 *     dual-pointer SKIP_X update at the grandparent slot.  It also
	 *     tripped chain-merge, which *needs* the slot.
	 *   - On plain-internal builds, the same gap would break the
	 *     parent-pointer backtrack's O(1) slot recovery.
	 *
	 * Externals carry no metadata / offset; skip them.  Test
	 * skip-compressed FIRST: a SKIP_X flag carries its external child's
	 * low tag bits, so ft_node_external() would misclassify it.
	 *
	 * We do NOT touch @new_child's parent linkage here.  Callers manage
	 * that via their own ft_set_parent (or by direct meta->parent
	 * assignment) before calling us.  ft_set_parent_slot computes the
	 * offset relative to child_meta->parent.  Callers have already
	 * pointed that at @parent_nf (the node holding @parent_slot).
	 *
	 * Skip the update at the root slot (&ft->root).  Root nodes have no
	 * parent, and ft_set_parent_slot's offset computation assumes that
	 * the slot lives inside a node-arena chunk.
	 */
	if (new_child && parent_slot != &ft->root) {
		struct cds_ft_metadata *child_meta = new_child_meta;

		if (!child_meta) {
			if (ft_node_skip_compressed(new_child))
				child_meta = cds_ft_item_to_metadata(
					(struct cds_ft_inode *)
						ft_skip_to_compressed(ft, new_child));
			else if (!ft_node_external(new_child))
				child_meta = cds_ft_item_to_metadata(
					ft_node_ptr(new_child));
		}
		if (child_meta && ft_parent_node(child_meta->parent_word)) {
			/*
			 * The caller's contract above: the parent word already
			 * names the node that holds @parent_slot.  A child whose
			 * re-home is only recorded would still name its old
			 * parent, and the offset below would be computed off
			 * the wrong body.
			 */
			urcu_assert_debug(ft_slot_in_node(
				ft_parent_node(child_meta->parent_word),
				parent_slot));
			ft_set_parent_slot(child_meta,
				ft_parent_node(child_meta->parent_word),
				parent_slot);
		}
	}

	if (parent_nf && ft_node_compressed(parent_nf)) {
		struct cds_ft_compressed_node *cn =
			ft_compressed_node_ptr(parent_nf);
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata(
				(struct cds_ft_inode *) cn);

		/* Consumed via FEATURE_FT_SKIP_COMPRESSED and FT_TP only. */
		(void) cn;
		(void) cn_meta;

#ifdef FEATURE_FT_SKIP_COMPRESSED
		{
			/*
			 * READ-YOUR-OWN-WRITES: a raw derivation does not see
			 * a recorded re-parent of @cn.  This op may be
			 * relocating @cn's parent in the same commit that
			 * refreshes the dual.  See ft_txn_parent_slot.
			 */
			struct cds_ft_inode_flag *skip_owner_nf = NULL;
			struct cds_ft_inode_flag **skip_slot =
				ft_txn_parent_slot_at(cn_meta, ft,
					rec ? rec->mtxn : NULL,
					&skip_owner_nf);
#ifdef FT_DEBUG_DUAL_DROP
			ft_dbg_dual_probe(ft, cn, cn_meta, skip_slot,
				skip_owner_nf, expected_old, new_child);
#endif
			FT_DUAL_SLOT_SETTLED_CHECK(skip_slot, dual_owner_held);
			if (skip_slot &&
			    ft_node_skip_compressed(*skip_slot)) {
				struct cds_ft_inode_flag *skip_new =
					ft_skip_compressed_flag(new_child,
						cn->len);

				if (rec && !ft_dual_home_is_private(ft, rec,
						cn_meta, skip_slot,
						skip_owner_nf))
					/* A COMPRESSED ROOT's dual slot IS
					 * &ft->root (ft_txn_parent_slot's root
					 * arm), so ask rather than assume. */
					/*
					 * ☠ THE DUAL'S OWNER IS DERIVED HERE,
					 * NOT DECLARED BY THE CALLER.  It is
					 * the GRANDPARENT, reached through
					 * @cn_meta's back-pointer.  This
					 * frame cannot know whether the op
					 * acquired it.  So the held answer
					 * is the caller's: @dual_owner_held.
					 * Its default is false, which
					 * records MW.
					 */
					/*
					 * @pub_fn/@pub_line are the
					 * caller's, not this frame's.  The
					 * caller DECIDED the answer below.
					 * An attribution that names
					 * _ft_publish_to_parent_meta would
					 * name a function that only
					 * forwards the answer.
					 */
					ft_pub_rec_add_at(pub_fn, pub_line,
						rec, skip_slot,
						ft_skip_compressed_flag(
							expected_old, cn->len),
						skip_new,
						skip_slot == &ft->root,
						skip_slot == &ft->root ||
						!skip_owner_nf ? NULL :
						ft_flag_to_metadata(ft,
							skip_owner_nf),
						ft_pub_slot_excluded(ft,
							skip_slot,
							dual_owner_held));
				else if (*skip_slot != skip_new)
					rcu_assign_pointer(*skip_slot, skip_new);
			}
		}
#endif
		/*
		 * Re-emit compressed_publish so consumers tracking
		 * cn -> child relationships pick up the new subtree
		 * attached under this compressed node.  The initial
		 * creation-time compressed_publish event has
		 * parent = NULL, because the compressed node is not
		 * yet attached.  Here we report cn_meta->parent: the
		 * compressed node is already in the trie.
		 */
		FT_TP(compressed_publish,
			(const void *) ft_compressed_node_flag(cn),
			cn->len,
			cn->key_bytes,
			(const void *) new_child,
			(const void *) ft_parent_node(CMM_LOAD_SHARED(cn_meta->parent_word)));
	}
	FT_TP(publish_to_parent, (const void *) parent_nf,
		(const void *) parent_slot,
		(const void *) *parent_slot,
		(const void *) new_child);
	/*
	 * When parent_slot points at ft->root, emit root_publish so
	 * consumers can track the top of the trie through root
	 * rewrites that have no structural parent node.
	 */
	if (parent_slot == &ft->root)
		FT_TP(root_publish, (const void *) ft,
			(const void *) new_child);
	if (rec)
		/*
		 * The FORWARD edge's owner is the caller's own @slot_owner_nf
		 * declaration: the node it says @parent_slot lives in.  So
		 * naming it IS the held answer here.  The record-time owner
		 * assert checks it.  Only the DUAL above needs a separate
		 * word, because only its owner is derived.
		 */
		ft_pub_rec_add(rec, parent_slot, expected_old, new_child,
			parent_slot == &ft->root,
			parent_slot == &ft->root || !slot_owner_nf ? NULL :
				ft_flag_to_metadata(ft, slot_owner_nf),
			ft_pub_slot_excluded(ft, parent_slot,
				slot_owner_nf != NULL));
	else if (*parent_slot != new_child)
		/*
		 * Direct (rec == NULL) publish.  It has only two callers:
		 *   - the in-place relocation else in ft_attach_node;
		 *   - graft's no-recompact else.
		 * Both republish the value the slot already holds
		 * (Invariant-1: no live reader-visible bare publish remains).
		 * The SKIP_X dual above is likewise a no-op in lockstep.
		 * Eliding the redundant store and its release fence is
		 * invisible to readers.  The guard keeps the store correct
		 * should a value ever differ.  The recorded (rec) path captures
		 * the edge for the flip-txn instead of storing here.
		 */
		rcu_assign_pointer(*parent_slot, new_child);
}

/*
 * Publish, and recover @new_child's metadata from the slot value.  The
 * back-pointer was wired up front.  This is the original behaviour.
 */
static
void _ft_publish_to_parent_at(const char *pub_fn, int pub_line,
		struct cds_ft *ft,
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **parent_slot,
		struct cds_ft_inode_flag *new_child,
		struct cds_ft_inode_flag *expected_old,
		struct ft_pub_rec *rec,
		bool dual_owner_held)
{
	_ft_publish_to_parent_meta_at(pub_fn, pub_line, ft, parent_nf,
		parent_slot, new_child, expected_old, NULL, NULL, rec,
		/*slot_owner_nf=*/ parent_nf, dual_owner_held);
}

#define _ft_publish_to_parent(ft, parent_nf, parent_slot, new_child,	\
		expected_old, rec, dual_owner_held)			\
	_ft_publish_to_parent_at(__func__, __LINE__, (ft), (parent_nf),	\
		(parent_slot), (new_child), (expected_old), (rec),	\
		(dual_owner_held))

#define _ft_publish_to_parent_meta(ft, parent_nf, parent_slot, new_child, \
		expected_old, new_child_meta, folded_child_prev, rec,	\
		slot_owner_nf, dual_owner_held)				\
	_ft_publish_to_parent_meta_at(__func__, __LINE__, (ft),		\
		(parent_nf), (parent_slot), (new_child), (expected_old),	\
		(new_child_meta), (folded_child_prev), (rec),		\
		(slot_owner_nf), (dual_owner_held))

/*
 * Direct publish (original behaviour): perform the stores immediately.
 * rec == NULL, so @expected_old is unused: the direct arm republishes the
 * value already present.  Pass the live slot value to satisfy the interface.
 */
static
void ft_publish_to_parent(struct cds_ft *ft,
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **parent_slot,
		struct cds_ft_inode_flag *new_child)
{
	_ft_publish_to_parent(ft, parent_nf, parent_slot, new_child,
		*parent_slot, NULL, false);
}

/*
 * ft_publish_compressed: convert a compressed node flag to a skip
 * pointer when both conditions hold:
 *   - skip-compressed mode is enabled;
 *   - the path length fits (cn->len <= FT_SKIP_LEN_MAX).
 * Otherwise return @cflag unchanged.
 *
 * Call AFTER ft_set_parent has been done with the real compressed
 * flag (@cflag).  Publish or store the returned value in parent child
 * slots.
 */
static
struct cds_ft_inode_flag *ft_publish_compressed(struct cds_ft *ft,
		struct cds_ft_compressed_node *cn,
		struct cds_ft_inode_flag *cflag)
{
	/*
	 * Emit creation-time compressed_publish.  Trace consumers then
	 * learn the cn->child binding for every newly-allocated
	 * compressed node.  This covers every creation path
	 * (ft_build_compressed_node, compressed-split sfx/pfx/nb,
	 * graft-split suffix/prefix).  parent is NULL here: the cn
	 * is about to be returned to the caller for attachment, and
	 * cn_meta->parent is still unset.  A subsequent
	 * ft_publish_to_parent / ft_node_set_nth on the slot that
	 * holds this cn fires tree_edge_set, with the cn as child.
	 * The two events give the consumer both ends: the parent->cn
	 * edge and the cn->child edge.
	 */
	FT_TP(compressed_publish,
		(const void *) ft_compressed_node_flag(cn),
		cn->len,
		cn->key_bytes,
		(const void *) cn->child,
		(const void *) NULL);
	if (ft_group_skip_compressed(ft->group) &&
	    cn->len <= FT_SKIP_LEN_MAX) {
		return ft_skip_compressed_flag(cn->child, cn->len);
	}
	return cflag;
}

/*
 * ft_set_parent: set the parent pointer in child's metadata.
 * Skips NULL children.
 *
 * External (leaf) nodes: sets cds_ft_node.prev (head of duplicate chain).
 *
 * For skip-compressed pointers: the skip pointer represents a
 * compressed node in the trie.  Set the compressed node's parent.
 * Do not set the parent of the compressed node's child: that parent
 * is the compressed node itself, and was set at creation time.
 *
 * Check skip-compressed before external.  A skip pointer whose child
 * is external has low tag bits == 0, so ft_node_external would match
 * on the raw value.
 *
 * Write-side only (mutex-held).
 */
/*
 * ft_head_stamp_incoming_byte: maintain an EXTERNAL head's up-walk edge byte at
 * a child placement.  This is the external counterpart of what ft_set_parent's
 * plain internal branch and ft_reparent_record_meta already do for a node.
 *
 * ☠ THE FIELD'S CONTRACT IS "MAINTAINED AT EVERY CHILD PLACEMENT"
 * (fractal-trie-internal.h, @incoming_byte).  For a head the record is its
 * CELL: "external head: stored in the head's CELL metadata ... set to the
 * key's last byte at insert".  Both placement primitives used to set only the
 * head's PARENT word.  They left the byte at whatever the INSERT stamped.
 * The argument, written at ft_reparent_record's external arm, was that a head
 * has "no metadata / offset".  It has no NODE metadata.  The cell is its
 * metadata, and ft_rebuild_key_upwalk reads exactly this field out of it.
 *
 * MEASURED, single-threaded on a quiet trie, ordered list ON, default build,
 * release included.  The steps:
 *   1. move a BARE EXTERNAL HEAD "q" with cds_ft_rekey_merge onto an empty
 *      destination;
 *   2. do a plain forward walk (cds_ft_lookup_first + cds_ft_next).
 * The walk emits a key THAT IS NOT IN THE TRIE.  The key has the destination's
 * bytes for every level but the last, and the SOURCE's last byte at the end:
 *   dst "z" -> "q", dst "ax" -> "aq", dst "axy" -> "axq".
 * cds_ft_verify is CLEAN and cds_ft_count_keys is right, so only a lookup of
 * the emitted key can see it.  Five of the six graft legs did it, the BRANCH
 * leg included.  The survivors were LATENT, not correct.  dst "axy" walks
 * fine on the default build only because the head hangs under a COMPRESSED
 * parent, whose key_bytes cover the byte.  Insert "axz" to split that run,
 * and the same trie starts spelling "axq".
 *
 * ☠ STAMP IT WHETHER OR NOT THE CURRENT PARENT READS IT.  Under a COMPRESSED
 * parent the walk takes the byte from key_bytes and never reads this field.
 * But an ordinary insert can SPLIT the run later.  The split drops the head
 * into a body slot of a fresh internal node, and re-homes nothing it would
 * have to stamp.  So the walk then reads whatever was left behind.  That is
 * not hypothetical: with only the plain-parent arm stamped, dst "axy" walked
 * clean and then `insert "axz"` made the same trie spell "axq".  The value is
 * the last byte of the head's KEY in both cases:
 *   - under a run, the run's last key byte;
 *   - under a plain parent, the slot byte.
 * The two cross-trie sites stamp that value directly, as
 * okey_dst[dst_key_len - 1].
 *
 * There are two real exclusions:
 *   - @ft->ordered_list is off.  There is then no cell to hold the byte, and
 *     the walk uses the iterator's own descent buffer instead.
 *   - A PREFIX head.  Its key ends AT the parent, so it HAS no edge byte.
 *     ft_head_parent_word_slot reads the shape from @slot in the same way.
 *     The test is kept in one predicate here, so the two cannot disagree
 *     about which placement they are describing.
 *
 * ☞ BYTE BEFORE PARENT.  ft_set_parent's internal branch already argues for
 * this order at its own stamp: "Publish the up-walk key byte BEFORE the
 * parent pointer ... a concurrent up-walk that follows the new parent would
 * read the still-stale byte".  A reader that reaches the head through its NEW
 * parent must not find the OLD byte.
 *
 * ★★★ WHY A PLAIN STORE IS THE RIGHT SHAPE HERE, AND WHY THE BYTE MUST NOT BE
 * PUT IN THE TXN.  THE RULE A REKEY WORKS BY IS THAT THE MOVED TOP GETS A FRESH
 * ADDRESS.  That is ft_rekey_cow_stop's whole purpose.  So every back edge
 * written on the top is a plain store into an object NO READER CAN SEE YET.
 * This holds for the parent word and for this byte alike.  The one commit
 * publishes that object atomically.  That is what makes txn membership
 * unnecessary.
 * ft_store_at_graft_point_commit states it at the sibling store: "A plain
 * store, because the payload is INVISIBLE until the forward publish below
 * -- ... the rekey fold hands over a fresh COW copy."  A byte that rode the
 * commit would solve a problem the COW has already solved for every top that
 * gets a copy.
 *
 * ☠ THE KEY AT THE SOURCE PREFIX GETS NO COPY.  Its leaf is APP-OWNED.  It
 * is either the BARE EXTERNAL HEAD (@s_top_prime == @s_top) or the prefix
 * head of the copied top.  The copy takes over that prefix head, but not its
 * leaf or its cell.  It stays reachable at the SOURCE for the whole build
 * window.  A store into its live cell would be reader-visible ahead of the
 * publish: a reader could pair the new parent with the old byte, and an
 * abort would not undo it.  Skipping the store left the source byte at the
 * new key, for the walk to spell after a later collapse.
 *
 * ☞ SO THE REKEY GIVES IT A FRESH CELL.  The byte lives in the head's
 * ORDERED CELL, which is library-allocated and can be copied.  The rekey
 * allocates a fresh cell and stamps it while invisible.  One commit
 * swaps it into the list and into head->prev, and the old cell is retired
 * after a grace period (ft-rekey.h, step 2).  ft_reparent_record sends the
 * head's back edge to the fresh cell, so this function never sees that head.
 * Transacting @incoming_byte would not work.  It shares a 32-bit word with
 * @alloc_index, with no room for a parked descriptor pointer, and the
 * up-walk reads it raw.  A cds_ft_rekey_merge that demotes the head onto a
 * chain drops the fresh cell: a demoted head owns no cell.
 */
static inline
void ft_head_stamp_incoming_byte(const struct cds_ft *ft,
		struct cds_ft_node *en,
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **slot)
{
	struct cds_ft_inode *parent;
	void *prev;

	struct cds_ft_compressed_node *cn = NULL;
	uint8_t byte;

	if (!ft->ordered_list || !slot || !parent_nf)
		return;
	if (ft_node_flip_proxy(parent_nf))
		return;
	/*
	 * KIND DISPATCH, SKIP FIRST and BEFORE the FT_INTERNAL_MASK test.  A
	 * compressed flag has that bit CLEAR.  So testing it up front would
	 * drop exactly the run-parent arm below.
	 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(parent_nf))
		cn = ft_skip_to_compressed(ft, parent_nf);
	else
#endif
	if (ft_node_compressed(parent_nf))
		cn = ft_compressed_node_ptr(parent_nf);
	else if (!((uintptr_t) parent_nf & FT_INTERNAL_MASK))
		return;
	if (cn) {
		/*
		 * A COMPRESSED parent's key_bytes already cover this byte, so the
		 * walk does not read it HERE.  But it will the moment a plain
		 * insert SPLITS the run and drops the head into a body slot of
		 * the fresh internal node.  That split re-homes nothing it
		 * would have to stamp.  It finds whatever was left behind.
		 * So the byte is maintained at every placement, whether or not
		 * the CURRENT parent consults it.  It is the last byte of the
		 * head's key either way.  Under a run, that is the run's last
		 * key byte.
		 */
		if (!cn->len)
			return;
		byte = cn->key_bytes[cn->len - 1];
	} else {
		parent = ft_node_ptr(parent_nf);
		if ((void *) slot == (void *)
				&cds_ft_item_to_metadata(parent)->external_nodes)
			return;		/* a PREFIX head: its key ends AT the parent */
		byte = ft_slot_to_byte(&ft_types[ft_node_type(parent_nf)],
				parent, slot);
	}
	prev = ft_dereference_prev_resolved(en);
	/*
	 * FT_ORD_CELL_TAG: only a duplicate-chain HEAD owns a cell.  A demoted
	 * duplicate's prev names its chain predecessor LEAF.  It must not be
	 * read as a cell.  See the same guard at the rekey's own head stamp.
	 */
	if (!prev || !((uintptr_t) prev & FT_ORD_CELL_TAG))
		return;
	cds_ft_item_to_metadata(ft_ord_cell_ptr(prev))->incoming_byte = byte;
}

/*
 * ft_set_parent: set the parent pointer in child's metadata,
 * and optionally record the slot for skip-compressed children.
 *
 * @child_nf:  child node flag (may be skip-compressed, external, etc.)
 * @parent_nf: parent node flag to record.
 * @slot:      address of the slot in the parent that holds @child_nf.
 *             When @child_nf is skip-compressed and @slot is non-NULL,
 *             the compressed node's parent_slot_offset records @slot.
 *             Pass NULL when the slot is unknown or irrelevant.
 */
static
void ft_set_parent_at(const char *fn, int line, struct cds_ft *ft,
		struct cds_ft_inode_flag *child_nf,
		struct cds_ft_inode_flag *parent_nf,
		struct cds_ft_inode_flag **slot, enum ft_word_excl excl)
{
	(void) excl;	/* consumed by the word-class audits only */
	/*
	 * @fn/@line are the CALLER's, so the head parent-word audit below gets
	 * one row per CALL SITE.  ft_set_parent has 34 callers.  The external
	 * arm is the only one that writes a head's parent word, and far fewer
	 * callers reach it.  Which ones do is a question to MEASURE, not to
	 * classify by inspection.
	 */
	/*
	 * A NULL @parent_nf is the root position: the publish goes into
	 * &ft->root.  Store the OWNING TRIE there, not NULL.  The back-edge
	 * then identifies its owner at depth 0 as it does everywhere else.
	 * @parent_nf itself stays as passed for two uses:
	 *   - the slot offset (a root has none);
	 *   - an external child, whose holder is always a real node.
	 */
	struct cds_ft_inode_flag *stored_parent = parent_nf ?
		parent_nf : ft_trie_parent(ft);

	if (!child_nf)
		return;
	/*
	 * A type-7 flip proxy is a transient slot VALUE (a one-commit insert
	 * or merge flip in progress), not a node.  The parking mutator itself
	 * wires the REAL child's back-pointer.  So this is a no-op.  The
	 * generic re-parent loops (recompact's child sweep, set_nth's
	 * post-store wiring) then flow over a parked slot unharmed.
	 * Dispatching below would misread the proxy latch as internal-node
	 * metadata.
	 */
	if (caa_unlikely(ft_node_flip_proxy(child_nf)))
		return;
	FT_TP(set_parent, (const void *) child_nf, (const void *) parent_nf);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(child_nf)) {
		struct cds_ft_compressed_node *cn =
			ft_skip_to_compressed(ft, child_nf);
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata(
				(struct cds_ft_inode *) cn);
		ft_ch_audit_parent_at(fn, line, ft, cn_meta, parent_nf, excl);
		rcu_assign_pointer(cn_meta->parent_word, stored_parent);
		ft_set_parent_slot(cn_meta, parent_nf, slot);
		return;
	}
	if (ft_node_compressed(child_nf)) {
		/*
		 * Plain COMPRESSED form (no SKIP_X wrap): typically
		 * arises when ft_publish_compressed gates SKIP-X off
		 * for a non-spec EXT child.  Maintain the cn's
		 * parent_slot_offset just like the SKIP_X branch above so
		 * later ft_publish_to_parent / chain-merge calls can
		 * recover the slot in cn's parent via
		 * ft_get_parent_slot.
		 */
		struct cds_ft_compressed_node *cn =
			ft_compressed_node_ptr(child_nf);
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata(
				(struct cds_ft_inode *) cn);
		ft_ch_audit_parent_at(fn, line, ft, cn_meta, parent_nf, excl);
		rcu_assign_pointer(cn_meta->parent_word, stored_parent);
		ft_set_parent_slot(cn_meta, parent_nf, slot);
		return;
	}
#endif
	if (ft_node_external(child_nf)) {
		/*
		 * Ordered list on: the head carries its cell in prev.  Record
		 * the parent into cell->parent.  The cell is there in both
		 * cases: a fresh head has it pre-wired at insert, and an
		 * existing head being re-parented already has one.
		 * List off / non-cell: the parent is the head's prev directly.
		 * rcu_assign either way: ft_set_parent re-parents live heads on
		 * the restructure path.
		 */
		struct cds_ft_node *en = (struct cds_ft_node *) child_nf;
		/*
		 * @slot settles the head's shape:
		 *   - a BODY SLOT in @parent_nf makes it a slot head;
		 *   - the external_nodes word (named, or NULL) makes it a
		 *     prefix head.
		 * See ft_head_parent_word_slot's header for why NULL means a
		 * prefix head here (FT_PARENT_PREFIX_HEAD).
		 */
		struct cds_ft_inode_flag *word =
			ft_head_parent_word_slot(parent_nf, slot);

		/* The up-walk edge byte, BEFORE the parent word: see the helper. */
		ft_head_stamp_incoming_byte(ft, en, parent_nf, slot);
		/* Same word class as the prefix-head store above. */
		ft_ch_audit_head_at(fn, line, ft, en, parent_nf, excl);
		if (ft->ordered_list) {
			ft_ord_cell_set_parent(en, word);
		} else {
			FT_CHAIN_CANARY_RAW(&en->prev, 2);
			rcu_assign_pointer(en->prev, word);
		}
		return;
	}
	{
		/*
		 * Plain internal child: record its parent AND its slot offset
		 * within the parent.  The parent-pointer backtrack can then
		 * recover the slot in O(1) (ft_get_parent_slot) without
		 * re-descending.  ft_set_parent_slot reads meta->parent, so set
		 * it first.
		 */
		struct cds_ft_metadata *meta =
			cds_ft_item_to_metadata(ft_node_ptr(child_nf));

		/*
		 * Publish the up-walk key byte BEFORE the parent pointer.  A node
		 * re-homed from a COMPRESSED parent (which skips incoming_byte,
		 * leaving it 0) to an INTERNAL parent gets its real branch byte
		 * here.  ft_set_parent_slot needs meta->parent published first
		 * to compute the offset.  But if we published it before the
		 * byte, a concurrent up-walk that follows the new parent would
		 * read the still-stale 0 byte.  It would reconstruct a key with
		 * a hole at this level.  So pre-store the byte under the
		 * explicit @parent_nf, and let the rcu_assign release order it.
		 * ft_set_parent_slot below recomputes the same byte
		 * (idempotent) plus the offset.
		 */
		if (slot && parent_nf && !ft_node_compressed(parent_nf)
#ifdef FEATURE_FT_SKIP_COMPRESSED
				&& !ft_node_skip_compressed(parent_nf)
#endif
		   )
			meta->incoming_byte = FT_DT_IB_STORE(meta, ft_slot_to_byte(
				&ft_types[ft_node_type(parent_nf)],
				ft_node_ptr(parent_nf), slot));
		ft_ch_audit_parent_at(fn, line, ft, meta, parent_nf, excl);
		rcu_assign_pointer(meta->parent_word, stored_parent);
		ft_set_parent_slot(meta, parent_nf, slot);
	}
}

#define ft_set_parent(ft, child_nf, parent_nf, slot)			\
	ft_set_parent_at(__func__, __LINE__, (ft), (child_nf),		\
		(parent_nf), (slot), FT_EXCL_UNDECLARED)
/*
 * The declaring spelling: the caller states WHICH REGIME makes this write legal
 * (enum ft_word_excl).  Only convert a site whose answer the code ITSELF
 * establishes.
 */
#define ft_set_parent_excl(ft, child_nf, parent_nf, slot, excl)		\
	ft_set_parent_at(__func__, __LINE__, (ft), (child_nf),		\
		(parent_nf), (slot), (excl))

/*
 * Return codes for compressed node traversal helpers.
 * Used to tell callers which loop control action to take.
 */
enum ft_descent_action {
	FT_DESCENT_CONTINUE,		/* Continue loop iteration. */
	FT_DESCENT_BREAK,		/* Break from loop. */
	FT_DESCENT_END,		/* Jump to function end (status set). */
	FT_DESCENT_GOING_UP,		/* Jump to going_up backtracking. */
	FT_DESCENT_DESCEND_CHILDREN,	/* Jump to descend_children. */
	FT_DESCENT_FOUND_MINMAX,	/* Jump to found_minmax label. */
};

/*
 * Compare @cmp key bytes starting at @key against the compressed
 * node's path.  Returns the number of matching bytes.  A return
 * value == @cmp means full match; < @cmp means divergence at that
 * position.
 */
static inline
unsigned int ft_match_compressed_key(const uint8_t *key,
		const struct cds_ft_compressed_node *cn,
		unsigned int cmp)
{
	unsigned int pos;

	if (ft_key_cmp_ordinals(key, cn->key_bytes, cmp, cmp, false, &pos) != 0)
		return pos;
	return cmp;
}

/*
 * Fill ordinal_key for every level spanned by a compressed node.
 * Read-side descent loops (lookup_nth, minmax, etc.) use this to record
 * the ordinal key bytes through compressed nodes.  The going-up backtrack
 * recovers per-level nodes from the live parent chain, not a path array.
 */
static inline
void ft_fill_compressed_path(struct cds_ft_compressed_node *cn,
		uint8_t *ordinal_key, int base)
{
	int j;

	for (j = 0; j < cn->len; j++)
		ordinal_key[base + j] = cn->key_bytes[j];
}

static
bool valid_external_node(struct cds_ft_node *node)
{
	return node != NULL && ft_node_external((struct cds_ft_inode_flag *) node);
}

/*
 * Return the metadata of the root node.
 *
 * ft->root always points to an arena-allocated internal node, even
 * when the trie is empty (nr_child == 0).  The node itself may be
 * replaced by graft or graft-swap, but the invariant on the slot
 * is maintained across all operations.  Its metadata holds:
 *   - nr_child:       number of children in the root node.
 *   - external_nodes: list of NIL-key (key_len == 0) entries.
 *
 * The root is a regular internal node whose metadata is accessed the
 * same way as any other node's.  Its metadata carries the NIL-key
 * entries, so transplanting a root node between tries is a single
 * pointer swap with no metadata relocation.
 *
 * This function is only meant to be used from update functions, _not_
 * safe for use by read-side.
 */
static inline
struct cds_ft_metadata *ft_root_metadata(const struct cds_ft *ft)
{
	/*
	 * Resolve a peer's parked flip proxy on the root slot (Phase 4.3, a
	 * root recompact mid-commit).  ft_node_ptr on the raw proxy would mask
	 * the tag and return a RECORD address as a node.
	 */
	return cds_ft_item_to_metadata(ft_node_ptr(
		ft_resolve_flip_proxy(rcu_dereference(ft->root))));
}

static
bool valid_key_len(struct cds_ft *ft, size_t key_len)
{
	size_t max_key_len = ft->group->max_key_len;

	assert(max_key_len != CDS_FT_MAX_LEN_UNLIMITED);
	if (key_len == CDS_FT_LEN_ERROR || key_len > max_key_len)
		return false;
	return true;
}

static
struct cds_ft_inode *alloc_cds_ft_node(struct cds_ft *ft,
		const struct cds_ft_type *ft_type,
		struct cds_ft_metadata **_metadata)
{
	struct cds_ft_metadata *metadata;
	void *p;

	metadata = cds_ft_alloc_item(ft, ft_type->order, ft_type->bitmap);
	if (!metadata) {
		return NULL;
	}
	/* A NEW INCARNATION of this address begins here (see FT_LL_REUSE). */
	FT_LL_MARK_REUSE(metadata);
	p = cds_ft_metadata_to_item(metadata);
	FT_TP(item_alloc, (const void *) p, 0, ft_type->order);
#ifdef FT_DEBUG_DEL_TOMB
	{
		static unsigned long ft_dt_alloc_stale;

		if (CMM_LOAD_SHARED(metadata->state) != 0 &&
				uatomic_add_return(&ft_dt_alloc_stale, 1) <= 20)
			fprintf(stderr, "FT DT-ALLOC-STALE node=%p meta=%p state=%#lx\n",
				p, (void *) metadata,
				(unsigned long) CMM_LOAD_SHARED(metadata->state));
	}
#endif
	/*
	 * Popcount node data[] starts with a presence bitmap, followed
	 * by the pointer table.  The allocator returns zeroed memory.
	 * That is the initial "no children" state: bitmap = 0, so all
	 * lookups return NULL, and nr_child derived from popcount is 0.
	 */
	if (ft_debug_counters()) {
		uatomic_inc(&ft->group->nr_nodes_allocated);
		uatomic_inc(&ft->group->nr_internal_alloc);
	}
	*_metadata = metadata;
	return p;
}

static
void free_cds_ft_node(struct cds_ft *ft, struct cds_ft_inode *node)
{
	struct cds_ft_metadata *metadata = cds_ft_item_to_metadata(node);

	FT_LL_MARK_FREED(metadata);

	FT_TP(item_free, (const void *) node, 0);
	FT_TP(item_retire, (const void *) node, __builtin_return_address(0));

#ifdef FT_DEBUG_TOMBSTONE_AUDIT
	/*
	 * Freeze-on-free guard (doc §4.B, STEP 3).  Every PUBLISHED node
	 * retired through this path must carry its one-way LIVE->DEAD
	 * tombstone.  The tombstone is set BEFORE the unlink commit that
	 * detached the node.  Abandoned fresh (never-reader-visible) nodes use
	 * free_cds_ft_node_unpublished and do not reach here.  Build with
	 * -DFT_DEBUG_TOMBSTONE_AUDIT to enforce that no retire site is added
	 * without a mark (a no-op under one writer).
	 */
	assert(ft_meta_tombstone(metadata));
#endif
	cds_ft_free_item(ft, metadata);
	if (ft_debug_counters() && node) {
		uatomic_inc(&ft->group->nr_nodes_freed);
		uatomic_inc(&ft->group->nr_internal_freed);
	}
}

/*
 * Immediate-free variant for internal nodes that never escape the
 * writer's stack.  Example: a node built by an attach/split/recompact
 * helper, then freed by an -ENOMEM error path before publication.
 * See cds_ft_free_item_unpublished for the safety contract.
 */
static
void free_cds_ft_node_unpublished(struct cds_ft *ft, struct cds_ft_inode *node)
{
	struct cds_ft_metadata *metadata;

	/*
	 * ☠ FREEING NOTHING IS NOT FREEING ADDRESS ZERO.  The counter update
	 * below already guards on @node, so this function was always meant to
	 * tolerate a NULL.  But the metadata derivation used to run first and
	 * unconditionally.  cds_ft_item_to_metadata(NULL) does not fault on
	 * the pointer.  It indexes the metadata array of the range that WOULD
	 * contain address 0, and reads 0x200010.
	 *
	 * ft_node_recompact's @abandon_fresh unwind reaches here with
	 * @new_node == NULL whenever the attempt was sized to NODE_INDEX_NULL
	 * and then bailed.  That sizing is a DEL whose post-lock re-read
	 * leaves the copy no children.  Three places bail to the unwind: the
	 * two plan checks in the DEL block, and the NODE_INDEX_NULL refusal
	 * beside them.  MEASURED: 8 SIGSEGVs in 200 short runs of
	 * inv_concurrent_remove_all_nolist, all at
	 * cds_ft_item_to_metadata(p=0x0) from this call.
	 */
	if (!node)
		return;
	metadata = cds_ft_item_to_metadata(node);

	FT_TP(item_free, (const void *) node, 2);
	cds_ft_free_item_unpublished(ft, metadata);
	if (ft_debug_counters() && node) {
		uatomic_inc(&ft->group->nr_nodes_freed);
		uatomic_inc(&ft->group->nr_internal_freed);
	}
}

/*
 * Compute the arena allocation order for a compressed node with
 * @path_len key bytes.  The compressed node layout is:
 *   [child pointer] [len byte] [key_bytes...]
 */
static
unsigned int ft_compressed_order(uint8_t path_len)
{
	size_t size = offsetof(struct cds_ft_compressed_node, key_bytes) + path_len;
	int order = urcu_get_count_order_ulong(size);

	if (order < 4)
		order = 4;	/* Minimum arena order. */
	return (unsigned int) order;
}

static
struct cds_ft_compressed_node *alloc_compressed_node(struct cds_ft *ft,
		uint8_t path_len,
		struct cds_ft_metadata **_metadata)
{
	struct cds_ft_metadata *metadata;
	void *p;
	unsigned int order = ft_compressed_order(path_len);

	metadata = cds_ft_alloc_compressed_item(ft, order);
	if (!metadata)
		return NULL;
	p = cds_ft_metadata_to_item(metadata);
	FT_TP(item_alloc, (const void *) p, 1, path_len);
	if (ft_debug_counters()) {
		uatomic_inc(&ft->group->nr_nodes_allocated);
		uatomic_inc(&ft->group->nr_compressed_alloc);
	}
	*_metadata = metadata;
	return p;
}

static
void free_compressed_node(struct cds_ft *ft,
		struct cds_ft_compressed_node *node)
{
	struct cds_ft_metadata *metadata =
		cds_ft_item_to_metadata((struct cds_ft_inode *) node);

	FT_TP(item_free, (const void *) node, 1);

#ifdef FT_DEBUG_TOMBSTONE_AUDIT
	/* See free_cds_ft_node: freeze-on-free guard (doc §4.B). */
	assert(ft_meta_tombstone(metadata));
#endif
	FT_TP(compressed_free, (const void *) ft_compressed_node_flag(node));
	cds_ft_free_item(ft, metadata);
	if (ft_debug_counters() && node) {
		uatomic_inc(&ft->group->nr_nodes_freed);
		uatomic_inc(&ft->group->nr_compressed_freed);
	}
}

/*
 * Immediate-free variant for compressed nodes that never escape the
 * writer's stack (e.g., -ENOMEM error paths in build/split helpers).
 * See cds_ft_free_item_unpublished for the safety contract.
 */
static
void free_compressed_node_unpublished(struct cds_ft *ft,
		struct cds_ft_compressed_node *node)
{
	FT_TP(item_free, (const void *) node, 3);
	struct cds_ft_metadata *metadata =
		cds_ft_item_to_metadata((struct cds_ft_inode *) node);

	FT_TP(compressed_free, (const void *) ft_compressed_node_flag(node));
	cds_ft_free_item_unpublished(ft, metadata);
	if (ft_debug_counters() && node) {
		uatomic_inc(&ft->group->nr_nodes_freed);
		uatomic_inc(&ft->group->nr_compressed_freed);
	}
}

#define __FT_ALIGN_MASK(v, mask)	(((v) + (mask)) & ~(mask))
#define FT_ALIGN(v, align)		__FT_ALIGN_MASK(v, (typeof(v)) (align) - 1)
#define __FT_FLOOR_MASK(v, mask)	((v) & ~(mask))
#define FT_FLOOR(v, align)		__FT_FLOOR_MASK(v, (typeof(v)) (align) - 1)

/*
 * Push a node and its depth onto the snapshot stack, maintaining the
 * parallel snapshot_depth[] array alongside snapshot[].
 */
#define ft_snapshot_push(snap, snap_depth, nr, node_flag, depth)	\
	do {								\
		(snap_depth)[(nr)] = (depth);				\
		(snap)[(nr)++] = (node_flag);				\
	} while (0)


/* Human-readable name for a cds_ft_status code (diagnostics, tests). */
const char *cds_ft_status_to_string(enum cds_ft_status status)
{
	switch (status) {
	/* Success return codes (>= 0). */
	case CDS_FT_STATUS_OK:
		return "Operation completed successfully";
	case CDS_FT_STATUS_NOT_FOUND:
		return "No node found";
	case CDS_FT_STATUS_DUPLICATE_FOUND:
		return "Duplicate node exists";
	case CDS_FT_STATUS_INTERNAL_MATCH:
		return "Match ends at an internal node";

	/* Error return codes (< 0). */
	case CDS_FT_STATUS_INVALID_ARGUMENT_ERROR:
		return "Invalid argument";
	case CDS_FT_STATUS_MEMORY_ERROR:
		return "Memory allocation failure";
	case CDS_FT_STATUS_OVERFLOW_ERROR:
		return "Buffer too small for key length";
	case CDS_FT_STATUS_BUSY_ERROR:
		return "Resource busy";
	case CDS_FT_STATUS_POPULATED_ERROR:
		return "Destination already populated";
	case CDS_FT_STATUS_INTEGRITY_ERROR:
		return "Integrity verification failure";
	case CDS_FT_STATUS_NOT_SUPPORTED:
		/*
		 * The enum documents BOTH halves ("unavailable for this trie's
		 * configuration ... or feature not compiled in").  The string
		 * used to name only the second.  So a SHAPE refusal read as a
		 * build problem, and the reader looked for a missing -D.
		 */
		return "Unsupported for this trie or shape";

	default:
		return "Unknown status value";
	}
}

/* Emit @level indentation tabs to @out (shared by the show + stats renderers). */
static
void print_indent(FILE *out, int level)
{
	int i;

	for (i = 0; i < level; i++)
		fprintf(out, "	");
}
