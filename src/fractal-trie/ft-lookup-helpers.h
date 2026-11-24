// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-lookup-helpers.h
 *
 * Userspace RCU library - Fractal Trie: read-path (lookup / ordered-iteration)
 * helpers.  These are the ordinal-cell ACCESS primitives:
 *  - the ordered sibling-list cell struct;
 *  - its flag / pointer / ord-resolution accessors;
 *  - alloc / free.
 * Both the write path and the ordered read path share them.  The ordered read
 * path is cds_ft_next, the iterator and the inequality lookups.  So they live
 * early, beside the other read helpers.
 *
 * NOTE: this is NOT every ord_cell function, only the read-accessible
 * primitives.  The ordered-list MAINTENANCE operations (splice / unsplice /
 * swap / flip / run / find) live in ft-mutation-helpers.h on the write side.
 * They mutate the cell list and depend on the relational descent and node
 * scanners.  Batched cell iteration lives with the iterator (ft-iter.h).
 *
 * Implementation unit: #included once into the fractal-trie.c translation
 * unit (preserves cross-module inlining).  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-lookup-helpers.h is an implementation unit; #include it from fractal-trie.c only"
#endif

static inline_lookup
void *ft_ord_cell_flag(struct ft_ord_cell *cell)
{
	return (void *) ((unsigned long) cell | FT_ORD_CELL_TAG);
}

static inline_lookup
struct ft_ord_cell *ft_ord_cell_ptr(const void *prev)
{
	return (struct ft_ord_cell *)
		((unsigned long) prev & ~(unsigned long) FT_ORD_CELL_TAG);
}

/*
 * Reader-side load of a node's back-reference (prev).  Strip a parked
 * structural flip-proxy before the value is interpreted.  It is interpreted as:
 *  - a head cell (ordered-list on, via ft_ord_cell_ptr);
 *  - or a flagged parent (ordered-list off, via ft_resolve_head_prev).
 * A head-cell promote / swap / prev-retarget can fold its prev store onto the
 * commit flip-txn.  Then this slot can transiently hold FT's type-7 proxy.  A
 * raw ft_ord_cell_ptr() would mask the tag and dereference the descriptor as
 * a cell.  This mirrors the forward-edge readers (ft_root_dereference /
 * ft_cn_child_dereference_acquire_prefetch): resolve the proxy at the load.
 *
 * Under the retained single-writer exclusion, no proxy is ever at rest.  So
 * ft_resolve_flip_proxy takes its predicted-not-taken branch, and the result
 * is byte-identical.  It becomes load-bearing only once the prev stores fold
 * onto the txn (Phase 4.3).  Writer-owned / quiescent prev reads keep the
 * direct rcu_dereference.  This is the same split as ft_meta_nr_child vs
 * ft_meta_nr_child_load.
 *
 * WHEN A WRITER'S PREV READ IS OWNED (the 2026-09-12 sweep's rule).  These
 * ops produce a parked value on a head's or a member's prev:
 *  - a head promote;
 *  - a member unlink;
 *  - a re-parent;
 *  - the merge's demotion record;
 *  - the plain park.
 * Each holds the chain holder's lock.  Or it is a bulk op under the FT-wide
 * writer lock, with point ops parked.  So a point op owns a prev word from its
 * acquire of that holder on.  A peer's commit settles its lock release AFTER
 * every structural word (ft_flip_txn_commit's late tag).  So nothing is
 * parked when the acquire succeeds.  A read taken BEFORE the acquire is not
 * owned, and must come through here (cds_ft_remove's cell capture).  The
 * routing it derives is re-validated under the lock (ft_unchain_kind).  A
 * stale-but-valid value is the other half of the same window.
 *
 * ☞ WHICH HOLDER, WHEN THE RE-HOME IS INTO A FRESH ONE.  A recompact re-homes
 * the holder's heads from the old copy into a FRESH copy.  A head's back edge
 * then names the fresh copy from the decide on.  That back edge is
 * cell->parent with the list on, and the prev itself with the list off.  It
 * stays parked until the settle.  So the re-homer PUBLISHES the fresh copy
 * LOCKED, and releases it after the commit (ft_flip_txn_lock_born).  This rule
 * then holds for the fresh copy too: whoever acquires it finds every adopted
 * back edge already settled.
 */
static inline_lookup
void *ft_dereference_prev_resolved(struct cds_ft_node *node)
{
	return ft_resolve_flip_proxy((struct cds_ft_inode_flag *)
			rcu_dereference(node->prev));
}

/*
 * Resolve a head's flagged parent from its (already-rcu_dereference'd) prev.
 *  - When the group runs an ordinal-cell list, prev is a cell.  The parent is
 *    rcu_dereference(cell->parent).
 *  - Otherwise prev IS the flagged parent (no cell interposed).
 * @ft selects the mode.  The runtime cell-optional gate is added later.  For
 * now the cell branch is unconditional in cell builds.  Valid only for a
 * head.  A non-head dup's prev is the preceding node (callers gate on
 * ft_node_external like before).
 */
/*
 * ft_head_parent_word_raw: return an external head's back-edge word, RAW.  It
 * holds two things, read from one load, in either list mode:
 *  - the parent it names;
 *  - the prefix-head bit that says which shape it has (FT_PARENT_PREFIX_HEAD).
 * A parked flip proxy is resolved (its bit 4 is address).  The prefix bit is
 * NOT stripped.
 *
 * ft_resolve_head_prev is the same load for callers that only want the node.
 * An up-walk wants both halves.  It must not take them from two observations
 * of a live re-home.  So it starts here and strips for itself.
 */
static inline_lookup
struct cds_ft_inode_flag *ft_head_parent_word_raw(const struct cds_ft *ft,
		struct cds_ft_node *head)
{
	void *prev = ft_dereference_prev_resolved(head);

	if (!ft->ordered_list)
		return (struct cds_ft_inode_flag *) prev;
	return ft_resolve_flip_proxy(
		rcu_dereference(ft_ord_cell_ptr(prev)->parent));
}

static inline_lookup
struct cds_ft_inode_flag *ft_resolve_head_prev(const struct cds_ft *ft, void *prev)
{
	if (ft->ordered_list)
		/*
		 * cell->parent can carry an in-flight flip-proxy.  A re-parent
		 * can fold the cell->parent store INTO its structural commit,
		 * so that the back-edge flips ATOMICALLY with the forward
		 * publish (e.g. the detach external-promote).  It then
		 * transiently installs a descriptor here.  Every up-walk parent
		 * read passes through this single choke point, so resolve it
		 * here.  A concurrent reader mid-commit then sees the
		 * view-appropriate parent, not the raw descriptor.  This is the
		 * identity when no commit is in flight (ft_resolve_flip_proxy
		 * no-ops a plain pointer).
		 *
		 * A PREFIX head's parent word carries FT_PARENT_PREFIX_HEAD in
		 * BOTH modes: cell->parent here, prev below.  The holder it
		 * names is unchanged.  So strip the bit for every caller that
		 * wants the node.  ft_rebuild_key_upwalk / ft_upwalk_edge_bytes
		 * read the word itself: the bit is their answer.
		 */
		return ft_parent_prefix_strip(ft_resolve_flip_proxy(
			rcu_dereference(ft_ord_cell_ptr(prev)->parent)));
	return ft_parent_prefix_strip((struct cds_ft_inode_flag *) prev);
}

/*
 * Read an ordinal-cell ord_next / ord_prev slot, resolving an in-flight
 * flip-proxy.  The slots hold RAW (untagged) ft_ord_cell pointers.  But a
 * point-op splice transiently installs a tagged flip-proxy.  Then the two
 * directional edges flip atomically for a bidirectional ordered reader.
 *
 * The ordered-cell list rides <urcu/rcu-txn-list.h>.  So its edges carry the
 * concurrent list's ENGINE proxy tag (URCU_TXN_TAG, bit 0), NOT FT's type-7
 * structural tag.  A logically-deleted node carries the list deletion MARK
 * (bit 1).  ft_txn_list_resolve strips both and resolves the proxy.
 * Raw cells are >= 8-byte aligned (bits 0-1 clear), so the resolve is
 * unambiguous.  Under writer exclusion no proxy is installed at rest (a no-op
 * on the write side).
 */
static inline_lookup
struct ft_ord_cell *ft_ord_cell_resolve_ord(struct urcu_txn_list_node *const *slot)
{
	struct urcu_txn_list_node *p = rcu_dereference(*slot);

	p = ft_txn_list_resolve((void *) p);
	/*
	 * Circular topology: a link "off the end" resolves to the per-trie
	 * sentinel node, NOT NULL.  The returned value is then a pseudo-cell
	 * (ft_ord_cell_of(&sentinel.node)).  Its ONLY valid use is the
	 * ft_ord_is_end() boundary test.  It is a bare urcu_txn_list_node, not
	 * a full cell, so never deref its node/parent.  Use ft_ord_first /
	 * ft_ord_last when a NULL-if-empty endpoint is wanted.
	 */
	return ft_ord_cell_of(p);
}

/*
 * The ordinal-cell list's circular sentinel as a pseudo-cell: lnode is the
 * first field of struct ft_ord_cell (offset 0), so ft_ord_cell_of(&sentinel.node)
 * is bit-identical to &ft->ord_sentinel.node.  Valid ONLY as the ft_ord_is_end
 * comparison value and as a flip-edge old/new target denoting the list boundary.
 */
static inline_lookup
struct ft_ord_cell *ft_ord_sentinel_cell(const struct cds_ft *ft)
{
	return ft_ord_cell_of(&ft->ord_sentinel.node);
}

/*
 * True when @c is a resolved forward / backward link that points "off the end"
 * of @ft's ordinal-cell list: @ft's own circular sentinel, OR NULL.
 *
 * NULL is kept as a UNIVERSAL (trie-agnostic) end marker.  A cross-trie bulk
 * move cannot point a moved run's outer link at the DESTINATION trie's
 * sentinel.  That foreign sentinel would become reachable to a SOURCE-trie
 * reader straddling the move.  The source reader would not recognise another
 * trie's sentinel, and would dereference it as a cell.
 *
 * So a run handed to an exclusive transient trie (ft_ord_cell_run_install)
 * terminates at NULL.  Every trie's reader treats NULL as the end, exactly as
 * the pre-sentinel NULL-terminated list did.  The cells regain a real sentinel
 * link when re-homed into a live trie (run-splice / interleave overwrite their
 * outer links).  So the in-trie remove folding is unaffected.  It needs
 * first->prev == sentinel.
 */
static inline_lookup
bool ft_ord_is_end(const struct cds_ft *ft, const struct ft_ord_cell *c)
{
	return !c || &c->lnode == &ft->ord_sentinel.node;
}

/*
 * ☞ THE SENTINEL, TOLD APART FROM "NO CELL AT ALL".
 *
 * ft_ord_is_end() above folds the two together.  For a TRAVERSAL that is
 * exactly right: a NULL link and the trie's own sentinel both mean "off the
 * end".  For LOCKING they stopped being the same answer when the sentinel got
 * lock words of its own (@ord_begin_lock / @ord_end_lock).  A NULL owner means
 * "not a cell link, nothing to lock".  The sentinel is a perfectly lockable
 * owner.  Folding them would silently drop every head- and tail-of-key-order
 * splice out of the lock set.
 *
 * This is a separate predicate, rather than a change to ft_ord_is_end.  The
 * NULL-folding of ft_ord_is_end is load-bearing for the traversal callers.  It
 * is also load-bearing for the cross-trie run moves that NULL-terminate a
 * moved run's outer links.
 */
static inline_lookup
bool ft_ord_is_sentinel(const struct cds_ft *ft, const struct ft_ord_cell *c)
{
	return c && &c->lnode == &ft->ord_sentinel.node;
}

/*
 * Return the first cell of @ft's ordinal-cell list in key order, or NULL when
 * the list is empty.  This is the sentinel-model replacement for reading
 * ord_cell_head.  Resolve a flip proxy: a concurrent splice/run move flips the
 * sentinel's next edge.
 */
static inline_lookup
struct ft_ord_cell *ft_ord_first(const struct cds_ft *ft)
{
	struct ft_ord_cell *c =
		ft_ord_cell_resolve_ord(&ft->ord_sentinel.node.next);

	return ft_ord_is_end(ft, c) ? NULL : c;
}

/* Last cell of @ft's ordinal-cell list, or NULL when empty (old ord_cell_tail). */
static inline_lookup
struct ft_ord_cell *ft_ord_last(const struct cds_ft *ft)
{
	struct ft_ord_cell *c =
		ft_ord_cell_resolve_ord(&ft->ord_sentinel.node.prev);

	return ft_ord_is_end(ft, c) ? NULL : c;
}

/* True when @ft's ordinal-cell list is empty (the sentinel points at itself). */
static inline_lookup
bool ft_ord_empty(const struct cds_ft *ft)
{
	return ft_ord_is_end(ft,
		ft_ord_cell_resolve_ord(&ft->ord_sentinel.node.next));
}

/*
 * Cell lifecycle (FT-allocator arena).
 *
 * The cell is an item of the group's dedicated cell arena (a uniform 32 B
 * item region, FT_ORD_CELL_ALLOC_ORDER).  So cells pack contiguously for the
 * dense ord-walk, and cds_ft_compact can RELOCATE them.  The paired metadata
 * slot is unused except for its rcu_head.  cds_ft_free_item reuses that
 * rcu_head to defer the free past a grace period.  cds_ft_metadata_to_item /
 * _item_to_metadata map between the cell and its metadata via the range
 * header.
 */

/*
 * A FRESH CELL HANDED A PARKED PROXY AS ITS PARENT.  The copiers (a head
 * promote, a head replace) load the old cell's parent PLAIN under the chain
 * holder's lock.  That is sound only while no re-home into that holder is
 * still settling.  Publishing a fresh parent LOCKED guarantees this
 * (ft_flip_txn_lock_born).  Once copied, the proxy names a record of ANOTHER
 * slot, and no settle rewrites it.  So the new cell's parent stays a proxy for
 * ever (inv_concurrent_same_key_removes, before the born lock).
 *
 * The check is armed on every --enable-rcu-debug build.  It is also armed by
 * -DFT_DEBUG_CELL_PARENT_COPY, which adds a backtrace (glibc <execinfo.h>,
 * hence not on the portable arm).  It names the record's slot and its
 * descriptor's status, i.e. the commit the value came from.
 */
#if defined(FT_DEBUG_CELL_PARENT_COPY) || defined(DEBUG_RCU) || \
		defined(CONFIG_RCU_DEBUG)
# define FT_CELL_PARENT_COPY_CHECK
# ifdef FT_DEBUG_CELL_PARENT_COPY
#  include <execinfo.h>
# endif
static __attribute__((noinline, cold, noreturn))
void ft_cell_parent_copy_fail(struct cds_ft_inode_flag *parent, const void *site)
{
	struct ft_txn_parked *r = ft_flip_proxy_ptr(parent);
	const struct ft_txn_decision *d = ft_txn_parked_decision(r);

	fprintf(stderr, "FT CELL PARENT COPY: fresh cell built with a parked "
		"proxy %p (old %p new %p, decision %p committed %d) "
		"by %p\n", (void *) parent, ft_txn_parked_value(r, false),
		ft_txn_parked_value(r, true), (const void *) d,
		d ? (int) ft_txn_decision_committed(d) : -1,
		site);
	fflush(stderr);
# ifdef FT_DEBUG_CELL_PARENT_COPY
	{
		void *bt[16];

		backtrace_symbols_fd(bt, backtrace(bt, 16), 2);
	}
# endif
	abort();
}
#endif

/*
 * Allocate a head's cell and wire it to @node with parent @parent.  @parent
 * may be NULL (a root head), or set later via ft_ord_cell_set_parent.  The
 * ord_prev / ord_next list links start empty.  The ordered-list splice
 * (runtime-gated by ordered_list_set) populates them later.  Return the
 * cell-tagged pointer to store into node->prev.  Return NULL on allocation
 * failure (the caller fails the insert before mutating the trie).
 */
static
void *ft_ord_cell_alloc(struct cds_ft *ft, struct cds_ft_node *node,
		struct cds_ft_inode_flag *parent)
{
	struct cds_ft_metadata *meta;
	struct ft_ord_cell *cell;

#ifdef FT_CELL_PARENT_COPY_CHECK
	if (caa_unlikely(ft_node_flip_proxy(parent)))
		ft_cell_parent_copy_fail(parent, __builtin_return_address(0));
#endif
	meta = cds_ft_alloc_cell_item(ft);
	if (!meta)
		return NULL;
	cell = (struct ft_ord_cell *) cds_ft_metadata_to_item(meta);
	cell->lnode.prev = NULL;
	cell->lnode.next = NULL;
	cell->node = node;
	cell->parent = parent;
	if (ft_debug_counters())
		uatomic_inc(&ft->group->nr_cells_allocated);
	return ft_ord_cell_flag(cell);
}

/*
 * Release a head's cell after its key leaves the trie.  Route through
 * cds_ft_free_item, which does three things:
 *  - in concurrent mode, it defers the free past a grace period.  So an
 *    in-flight reader parked on a head it up-walks (head->prev -> cell ->
 *    cell->parent) never dereferences freed memory;
 *  - in exclusive mode, it frees synchronously;
 *  - it drains the cell range's nr_live for reclaim.
 */
static
void ft_ord_cell_free(struct cds_ft *ft, struct ft_ord_cell *cell)
{
	if (ft_debug_counters())
		uatomic_inc(&ft->group->nr_cells_freed);
	cds_ft_free_item(ft, cds_ft_item_to_metadata(cell));
}

/*
 * Immediately free a cell that was never published.  This is an insert that
 * ended a duplicate, or failed before @node became reachable.  No reader can
 * hold a reference, so the grace-period defer would only delay the free.
 */
static inline
void ft_ord_cell_free_unpublished(struct cds_ft *ft, struct ft_ord_cell *cell)
{
	if (ft_debug_counters())
		uatomic_inc(&ft->group->nr_cells_freed);
	cds_ft_free_item_unpublished(ft, cds_ft_item_to_metadata(cell));
}

/*
 * Set a head's relocated parent.  In the cell-always model, head->prev is the
 * cell (set once at alloc), and the flagged parent lives in cell->parent.
 * Use rcu_assign_pointer for the read-side up-walk (ft_resolve_head_prev does
 * rcu_dereference(cell->parent)).  @head must already carry its cell.
 */
static inline
void ft_ord_cell_set_parent(struct cds_ft_node *head,
		struct cds_ft_inode_flag *parent)
{
	/*
	 * Resolve, do not read raw.  A head's prev can carry a parked flip
	 * proxy.  It has two producers:
	 *  - a head promote parks its own proxy there;
	 *  - a merge's recorded demotion of the head
	 *    (ft_glue_record_splices, FT_HLIST_PREV_TAG).
	 * The resolve costs one predicted branch.  Other writer sites still
	 * read a head's prev raw.  Nobody has yet argued whether each of them
	 * is excluded from meeting a parked proxy (☐ sweep).
	 */
	rcu_assign_pointer(ft_ord_cell_ptr(
		ft_dereference_prev_resolved(head))->parent, parent);
}
