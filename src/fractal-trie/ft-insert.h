// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-insert.h
 *
 * Userspace RCU library - Fractal Trie: the key-insert path.
 *
 * This unit holds:
 *   - the descent to the attach point and ft_attach_node;
 *   - the one-commit ordered-list publish (struct ft_insert_commit,
 *     ft_insert_one_commit);
 *   - the compressed-path split.
 *
 * The flip-batch, node-reserve and node-cluster builders it uses now live
 * in their own shared modules (ft-mutation-helpers.h, ft-cluster-build.h).
 *
 * Implementation unit: fractal-trie.c #includes it once, in dependency
 * order, into a single translation unit.  This preserves cross-module
 * inlining.  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-insert.h is an implementation unit; #include it from fractal-trie.c only"
#endif

#ifdef FT_DEBUG_INPLACE_HOIST
/*
 * Size the two halves of the in-place ACQUIRE HOIST
 * (doc/design/ft-reintroduce-in-place-mutations.md §4c):
 *   - The reserve stays IN PLACE.  The hoist is then a clean MOVE of the
 *     :2266 acquire.
 *   - The reserve RELOCATES.  The hoisted lock is then on a node the op
 *     RETIRES.  That needs the anchored retire, not a plain one.
 *
 * Both counts are printed at exit.
 */
extern unsigned long ft_ip_reserve_inplace, ft_ip_reserve_reloc;
unsigned long ft_ip_reserve_inplace, ft_ip_reserve_reloc;
static void ft_ip_reserve_report(void) __attribute__((destructor));
static void ft_ip_reserve_report(void)
{
	unsigned long ip = ft_ip_reserve_inplace, rl = ft_ip_reserve_reloc;

	if (!ip && !rl)
		return;
	fprintf(stderr,
		"\nFT_INPLACE_HOIST  reserve IN PLACE=%lu  RELOCATED=%lu  (reloc %.2f%%)\n",
		ip, rl, rl * 100.0 / (double) (ip + rl));
}
#endif

#ifdef FT_DEBUG_INPLACE_DISCARD
static unsigned long ft_ipd_injected;

static __attribute__((destructor)) void ft_ipd_report(void)
{
	fprintf(stderr, "FT INPLACE DISCARD: %lu in-place reserves discarded after their raw stores\n",
		ft_ipd_injected);
}
#endif

/*
 * One-commit insert state (ordered-list fresh-head insert).
 *
 * The attach machinery parks a flip proxy in the structural slot.  The proxy
 * resolves to the OLD value, so the key stays invisible.  The attach
 * machinery records the settle information here.  insert_done then adds the
 * ordered-list neighbour edges to the SAME batch.  One MCAS flip commit makes
 * the key reachable in the structural index AND splices it into the cell
 * list.  A reader can never observe the fresh head without its cell in the
 * list (2026-06 review, 2.13).
 *
 * @batch == NULL: direct publish (ordered list off, or a shape not yet
 * converted).
 */
struct ft_insert_commit {
	struct ft_flip_txn *txn;		/* armed at the publish site */
	/*
	 * The op's PERSISTENT engine handle (ft_txn_op_init'd before the op's
	 * restart_attempt loop, doc §11): the arm binds @txn to it, so retry
	 * aging, the FIFO escalation turn, and the learned descriptor size span
	 * the whole retry loop.  NULL for an op not yet migrated to the
	 * persistent handle (_cds_ft_insert_replace) -- the arm then creates a
	 * standalone per-attempt txn as before.
	 */
	struct ft_op *op;
	struct cds_ft_inode_flag **slot;	/* forward-publish sentinel (one-commit
						 * parked) -- the txn settles the edges */
	/*
	 * Base of the count propagation for the post-commit +1.  The key
	 * counts only once it is reachable.  This is the deepest node whose
	 * nr_keys must reflect the fresh key.  NULL = the caller's *d.pnfp.
	 */
	struct cds_ft_inode_flag *count_from;
	/*
	 * Order-statistics count fold (rank stats ON).  When set:
	 *   - the +1 key-count propagation is recorded INTO @txn, as nr_keys
	 *     value-CAS edges on the STABLE ancestor chain from @count_from
	 *     to the root.  It is folded atomically with the structural
	 *     publish;
	 *   - insert_done skips the standalone post-commit root-ward count
	 *     walk.
	 *
	 * A shape opts in only once both hold:
	 *   - its @count_from is the stable base;
	 *   - every fresh node on the new key's path was built with its full
	 *     post-commit count.
	 *
	 * The arm must have reserved the extra per-ancestor edges (actual
	 * descent depth).
	 */
	bool count_folded;
	/*
	 * Old compressed node replaced by the parked publish.  Readers keep
	 * resolving the proxy to it until the commit.  Its free is deferred
	 * by a grace period, and must be queued only AFTER the commit.  A
	 * free queued before the commit would not cover readers that pick the
	 * proxy up later.
	 *
	 * ☞ @ctx: the op's lock context.  It serves only the BACK-EDGE CLAIM's
	 * wide witness (ft_park_live_parent_edge -> FT_BACK_EDGE_CLAIM).  It
	 * is NULL unless @actx has actually been ft_lock_ctx_init'd on this
	 * path.  @actx is a function-scope local declared UNINITIALISED.
	 * Handing &actx over unconditionally would walk a garbage held set
	 * and answer confidently from stack noise.  Set @ctx beside each
	 * init, nowhere else.
	 */
	const struct ft_lock_ctx *ctx;
	/*
	 * ☞ WHICH BUILDER deferred @live_child, for the back-edge claim's
	 * per-producer breakdown.  Zero-initialised with @ic.  Zero is the
	 * undifferentiated row, so an untagged producer is visible as such.
	 */
	FT_BE_SRC_FIELD
	/*
	 * @free_old_cn_held: the lock-set member the split builder acquired
	 * at ENTRY for this cn (F2 fence extended to the split-retire
	 * family).  It names the word actually CAS'd, and carries the clean
	 * snapshots of that word and of the cn.
	 *
	 * The builder derives its whole plan from the fenced cn: the diverge
	 * slicing, the cn->child snapshot and the deferred-edge captures.
	 * ft_insert_one_commit records the retire from this member
	 * (ft_flip_txn_record_retire_anchored).  A peer may change the state
	 * of the SAME cn under the fence (a concurrent split, collapse or
	 * re-home).  That change then aborts exactly one side.  A ratified
	 * stale plan does not erase it.
	 */
	struct ft_held_anchor free_old_cn_held;
	struct cds_ft_compressed_node *free_old_cn;
	/*
	 * Old internal node replaced by a recompact-relocation forward
	 * publish folded into the one-commit (ft_attach_node).  The parked
	 * grandparent proxy resolves to it until the commit flips.  Like
	 * free_old_cn, its grace-period-deferred free must be queued only
	 * AFTER the commit.  NULL = no relocation (in-place reserve).
	 */
	struct cds_ft_inode *free_old_node;
	/*
	 * Deferred LIVE re-parent edge (split-compressed one-commit).  The
	 * live old child's back-pointer is the back-channel publish: it
	 * exposes the fresh cluster to reanchor up-walkers.  For a parked
	 * commit the forward publish IS the single commit.  This edge must
	 * therefore be wired THERE, after the cell links are set, and not
	 * during the build.  Otherwise a reader that reanchors through the
	 * re-parented child reaches the fresh head before its cell is
	 * spliced (the inv_insert_splice_window race).  NULL = none.
	 */
	struct cds_ft_inode_flag *live_child;
	struct cds_ft_inode_flag *live_parent;
	struct cds_ft_inode_flag **live_slot;
	/*
	 * ☞ THE FRESH SPINE @live_child is re-homed under: the cluster top
	 * down to @live_parent.  The lock words of these nodes are published
	 * LOCKED (ft_flip_txn_lock_born_spine).  The producer that sets
	 * @live_parent sets this too: only it knows the depths and spans of
	 * the fresh nodes.
	 */
	struct ft_born_spine_node born[FT_BORN_SPINE_MAX];
	unsigned int nr_born;
	bool publish_to_parent;			/* settle via ft_publish_to_parent */
	/*
	 * Concurrent-writer commit-outcome scope (see the deferred-action
	 * model in <urcu/rcu-txn.h>):
	 *   - @ft anchors the finalize/rollback callbacks.
	 *   - @created[0..nr_created) are this attempt's fresh cluster nodes,
	 *     still unpublished.  The on-abort rollback frees them when a
	 *     peer writer wins the commit.  The build's own error path frees
	 *     them before the commit.
	 *   - The on-commit finalize frees the retired free_old_* above.
	 */
	struct cds_ft *ft;
	struct cds_ft_inode_flag *created[FT_MAX_DEPTH];
	int nr_created;
	/*
	 * DLM Step 1 compressed-split acquire (see
	 * doc/design/mw-writer-lock-escalation-model.md).  P is CN's parent,
	 * the value-swap forward-publish target.  A split builder may
	 * pre-acquire P as part of its one-commit lock-set {CN, P}.  Then:
	 *   - @parent_locked_holder is P's metadata;
	 *   - @parent_locked_snap is the clean state word captured at the
	 *     acquire.
	 *
	 * ft_insert_publish_or_park then does not re-mark P.  It records P's
	 * RELEASE terminal ({LOCK|snap -> snap}) from that held snap, via
	 * ft_flip_txn_hold_or_lock_parent, and registers it into @txn.  The
	 * commit/abort of @txn then owns P's fence.
	 *
	 * NULL means no pre-acquire (past-child {CN}, non-split shapes).
	 * publish_or_park then takes the incremental lock_or_guard.  A
	 * pre-publish bail releases the holder explicitly.
	 *
	 * @parent_lock_shared marks a THIRD state, which a NULL holder would
	 * otherwise hide.  P was a lock-set member, but coarsening mapped it
	 * onto the SAME anchor as CN.  The op holds P's lock already, and
	 * the release was recorded once, with CN's.  publish_or_park must
	 * then only guard:
	 *   - an acquire would fail against the op's OWN hold and self-abort;
	 *   - a second release would double-record the one word.
	 *
	 * Never set at per-node granularity, where two distinct nodes cannot
	 * share a word.
	 */
	struct cds_ft_metadata *parent_locked_holder;
	uintptr_t parent_locked_snap;
	bool parent_lock_shared;
	/*
	 * The split's LIVE CHILD is CN's only child.  The split re-homes it
	 * under its fresh junction or suffix.  It is held the same way as P:
	 *   - it is taken in the one lock set;
	 *   - it is carried here until ft_insert_publish_or_park registers
	 *     its release into @txn and clears this field;
	 *   - a pre-publish bail releases it.
	 *
	 * NULL when the child was not taken (not internal, not fine) or
	 * shares a word the op already holds.
	 *
	 * WHY TAKE IT: the re-home moves a boundary.  The child's anchor for
	 * everything below it moves too.  An op that holds the child as that
	 * anchor must exclude the split (doc/design/ft-lockset-inventory.md
	 * §5, §8).  That exclusion used to be OBSERVED, by a {live -> live}
	 * MW check on the child's state word.  It is now TAKEN.  One child,
	 * not a fan.
	 */
	struct cds_ft_metadata *child_locked_holder;
	/* The member that holder was taken for: the child (ft_held_anchor). */
	struct cds_ft_metadata *child_locked_member;
	uintptr_t child_locked_snap;
};

/* Append a fresh spine node (top first) for ft_flip_txn_lock_born_spine. */
static inline
void ft_ic_born_add(struct ft_insert_commit *ic, struct cds_ft_metadata *meta,
		unsigned int start, unsigned int span)
{
	assert(ic->nr_born < FT_BORN_SPINE_MAX);
	ic->born[ic->nr_born].meta = meta;
	ic->born[ic->nr_born].start = start;
	ic->born[ic->nr_born].span = span;
	ic->nr_born++;
}

static void ft_free_unpublished_split_cluster(struct cds_ft *ft,
		struct cds_ft_inode_flag *const *created, int nr_created);

/*
 * On-commit finalize.  The commit installed, so the nodes this op retired
 * are now unreachable.  Queue their grace-period-deferred free.
 * ft_insert_one_commit calls this when the commit returns
 * URCU_TXN_STATUS_OK.
 */
static
void ft_insert_finalize_cb(void *arg)
{
	struct ft_insert_commit *ic = arg;

	if (ic->free_old_cn)
		free_compressed_node(ic->ft, ic->free_old_cn);
	if (ic->free_old_node)
		free_cds_ft_node(ic->ft, ic->free_old_node);
}

/*
 * On-abort rollback.  A peer writer won the commit, or the commit hit ENOMEM.
 * NOTHING was installed, so no reader ever reached the fresh cluster this
 * attempt built.  Free it at once (no grace period needed), so that the
 * re-descend starts clean.  The retire targets stay live and untouched.  The
 * caller resets @node's list linkage before retrying.
 */
static
void ft_insert_abort_cb(void *arg)
{
	struct ft_insert_commit *ic = arg;

	ft_free_unpublished_split_cluster(ic->ft, ic->created, ic->nr_created);
}

/*
 * Park the deferred LIVE re-parent edge (split-compressed one-commit) into
 * @txn.  The live old child's back-pointer then flips to its new cluster
 * parent at the single MCAS flip commit.  It flips ATOMICALLY with the
 * forward structural edge and the ordered-list neighbour edges.
 *
 * That back-pointer is the only field a reanchor up-walk reads to recover a
 * skip-compressed node (ft_skip_reanchor, which resolves flip proxies on the
 * parent read).  Until the commit a reader resolves it to the OLD parent and
 * never enters the fresh cluster.  This closes the window where the head was
 * tree-reachable (via the re-parented child) but not yet in the ordered list.
 *
 * For a metadata-bearing child the (parent, slot-offset) bookkeeping rides
 * @txn as a CO-COMMITTED pair (ft_reparent_record_meta):
 *   - the parent pointer via a structural edge;
 *   - the offset via an FT_STATE_PROXY state edge.
 *
 * A commit ABORT therefore discards BOTH.  The live child never carries a
 * torn (old parent, new-cluster offset) pair.  incoming_byte stays a plain
 * same-value store (key-invariant re-home).  External heads carry only the
 * parent edge.
 */
static
void ft_park_live_parent_edge(struct cds_ft *ft,
		struct cds_ft_inode_flag *child,
		struct cds_ft_inode_flag *new_parent,
		struct cds_ft_inode_flag **slot,
		struct ft_flip_txn *txn,
		const struct ft_lock_ctx *ctx FT_BE_SRC_PARAM)
{
	struct cds_ft_metadata *meta = NULL;
	struct cds_ft_inode_flag **field;

	/*
	 * @child was captured from a live slot (e.g. cn->child).  A peer may
	 * be mid-flip on that slot (Phase 4.3).  Resolve a transient type-7
	 * flip proxy to its committed-or-old target before the kind dispatch
	 * below.  Otherwise a 0xF-tagged proxy falls through to the internal
	 * branch, and item_to_metadata faults on the proxy latch.  A
	 * conflicting peer commit is caught later by this txn's MCAS
	 * expected-old check.
	 */
	child = ft_resolve_flip_proxy(child);
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(child))
		meta = cds_ft_item_to_metadata((struct cds_ft_inode *)
			ft_skip_to_compressed(ft, child));
	else
#endif
	if (ft_node_compressed(child))
		meta = cds_ft_item_to_metadata((struct cds_ft_inode *)
			ft_compressed_node_ptr(child));
	else if (!ft_node_external(child))
		meta = cds_ft_item_to_metadata(ft_node_ptr(child));

	if (meta) {
		/*
		 * Metadata-bearing child: the (parent, slot-offset) pair rides
		 * @txn CO-COMMITTED (ft_reparent_record_meta).  This replaces
		 * an eager ft_set_parent_slot plus a parent-only record.
		 *
		 * The eager offset store was unobservable in flight.  Readers
		 * were gated at the OLD parent, which is always compressed
		 * here, and they skip incoming_byte.  But the store SURVIVED
		 * a commit ABORT.  The discarded parent record left the live
		 * child with a torn (old parent, new-cluster offset) pair.
		 * That pair fed ft_resolve_parent_slot and the split (parent,
		 * slot) guards after the fresh cluster was freed.  The MW
		 * retry loop makes ABORT routine.
		 *
		 * Ask THIS txn's registry whether the op holds the child.  A
		 * split that took its live child
		 * (ft_insert_dlm_acquire_split) registered it here.  A held
		 * child needs no live check: the lock is the exclusion the
		 * check approximated.  @ctx cannot answer the question: it is
		 * NULL on the compressed-split paths.
		 */
		struct ft_lock_ctx hctx;

		(void) ctx;
		ft_lock_ctx_init(&hctx, NULL, txn, NULL);
		ft_reparent_record_meta(ft, txn, meta, new_parent, slot,
			/*child_marked=*/ false, &hctx,
#ifdef FT_RED_SPLIT_NO_CHILD_LOCK
			/*check_child=*/ false);	/* RED: neither taken nor checked */
#else
			/*check_child=*/ true);
#endif
		return;
	}
	if (ft->ordered_list) {
		/*
		 * External head, ordered list ON: its parent lives in the cell
		 * carried by node->prev.
		 */
		field = &ft_ord_cell_ptr(ft_dereference_prev_resolved(
			(struct cds_ft_node *) child))->parent;	/* resolved: see ft_ord_cell_set_parent */
	} else {
		/*
		 * External head, ordered list OFF: there is no cell.  The
		 * parent is stored directly in node->prev (see ft_set_parent's
		 * external branch, which rcu_assigns prev = parent).  Re-parent
		 * that field.  This matches the immediate ft_set_parent the
		 * non-parked path would have done.
		 */
		field = (struct cds_ft_inode_flag **)
			&((struct cds_ft_node *) child)->prev;
	}
	/*
	 * External head: @field is cell->parent or node->prev -- the back channel
	 * ft_flip_txn_record_head_back_edge records always-MW, and its header
	 * carries the why.  The metadata-bearing children took the
	 * ft_reparent_record_meta arm above, which names its owner.
	 */
	/*
	 * ☠ THE EXPECTED-OLD IS READ THROUGH THE TXN, NEVER RAW.  @field is a
	 * TRANSACTED word: a concurrent op can have a transient proxy parked
	 * in it.  A raw `*field` then hands that descriptor POINTER to the
	 * engine as the expected-old.  A debug build traps on it in
	 * urcu_txn_add ("Debug builds still trap, to name the raw read").
	 * Any other build silently records a value no reader ever sees.
	 * ft_record_child_back_edge reads the same class of word the same
	 * way.
	 */
	/*
	 * @slot settles the head's shape (ft_head_parent_word_slot):
	 *   - the split's deferred live child arrives with its slot;
	 *   - the attach and cluster-leaf arms pass NULL.  There the head
	 *     becomes the fresh cluster top's external_nodes.
	 */
	{
		/* One load, and the owner FT-SLOT-3 names, derived from it. */
		void *old_pw = ft_txn_load(ft_flip_txn_handle(txn),
			(void **) field, FT_FLIP_PROXY_TAG);

		ft_flip_txn_record_head_back_edge_owned(txn, (void **) field,
			old_pw, ft_head_parent_word_slot(new_parent, slot),
			ft_back_edge_owner(old_pw)
			FT_BE_SITE_R(FT_BE_SRC_USE, ctx, FT_BE_SRC_RELOC));
	}
}

/*
 * One-commit insert tail (see struct ft_insert_commit).
 *
 * On entry the structural slot is recorded as a txn edge.  The fresh head is
 * invisible: the slot still reads its old value.  The head's parent chain is
 * fully wired.  The splice-position search therefore runs exactly as the
 * post-publish splice did.  Its from-head seed walks the parent chain, never
 * the recorded slot.
 *
 * Ordered list ON (@cell != NULL): park the <= 4 ordered-list neighbour edges
 * into the SAME txn, then commit once.  For every reader, the head becomes
 * reachable in the structural index AND spliced into the cell list atomically.
 *
 * Ordered list OFF (@cell == NULL): there is no cell to splice.  The txn
 * carries only the structural edges: the slot publish recorded at the publish
 * site, plus any split-compressed live re-parent.  The single commit below
 * still makes that structural publish atomic and freeze-before-install.
 *
 * Either way the commit then settles all slots to their direct values.  The
 * real top's writer-only wiring (parent, slot offset, incoming_byte) was done
 * at record time.
 */
static
enum urcu_txn_status ft_insert_one_commit(struct cds_ft *ft, const uint8_t *key,
		size_t key_len, struct ft_ord_cell *cell,
		struct ft_insert_commit *ic)
{
	enum urcu_txn_status st;

	if (cell) {
		struct ft_ord_cell *pred, *pred2, *succ0;
		struct urcu_txn_list_node *pred_lnode;

		/*
		 * Locate the predecessor.  From-HEAD seeds at the fresh head
		 * and walks the live parent chain up.  It is the fast default,
		 * and the only safe choice for the attach shapes.  There a
		 * from-root LT would descend through the attach's own
		 * mid-build re-parent edge and loop in the reanchor.  Two
		 * shapes need from-ROOT instead, and neither loops there:
		 *   - split-compressed (ic->live_child): the deferred live
		 *     edge is parked, so a from-head LT would reanchor
		 *     through the edge that is not wired yet.  The old
		 *     compressed node is intact, so from-root descends it
		 *     cleanly.
		 *   - external_nodes prefix key (!ic->publish_to_parent): the
		 *     head sits at an internal node's external_nodes and
		 *     sorts BEFORE its extensions, so a from-head seed
		 *     mis-locates it.  From-root descends only live nodes
		 *     (the parked external_nodes resolves to "no head"), and
		 *     meets no live re-parent.
		 * TODO(perf): the from-root cases re-descend; revisit if they show up hot.
		 */
		if (ft_ord_cell_find_pred_from_head(ft, key, key_len, cell,
				ic->live_child != NULL || !ic->publish_to_parent,
				&pred) < 0)
			goto splice_conflict;
		/*
		 * ORDER-INTENT capture + confirm (concurrent writers).  The
		 * search decided by KEY order that @cell belongs between
		 * @pred and @pred's successor.  A peer key committed after
		 * the search can interpose between @pred and ours.  An
		 * insert-after-@pred would then land BEFORE the interposed
		 * key.  The list stays internally consistent, in the wrong
		 * key order.  Nothing is dead, so neither the deletion mark
		 * nor any expected-old catches it.
		 *
		 * Close this in two steps:
		 *   1. Capture the successor in @succ0.
		 *   2. RE-RUN the predecessor search (seeded, cheap).
		 *
		 * When the search returns @pred again, no key sat between
		 * @pred and ours at an instant AFTER the capture.  The
		 * insert_between prepare below records @succ0 as the
		 * expected old of &pred->next.  Any interposition after that
		 * instant then fails the commit's value CAS.
		 *
		 * A recycled @pred (freed and re-bound in the window) has
		 * its pred->next re-initialized.  That mismatches @succ0 the
		 * same way.  The two searches never diverge under a single
		 * writer.
		 */
		succ0 = ft_ord_cell_resolve_ord(pred ?
			&pred->lnode.next : &ft->ord_sentinel.node.next);
		if (ft_ord_cell_find_pred_from_head(ft, key, key_len, cell,
				ic->live_child != NULL || !ic->publish_to_parent,
				&pred2) < 0)
			goto splice_conflict;
		/*
		 * Splice @cell between @pred and @succ0 via the composable
		 * op.  It is recorded straight into the structural commit
		 * txn.  FT's type-7 proxy tag applies, so a bidirectional
		 * ordered reader sees the splice atomic with the structural
		 * publish.
		 *
		 * Sentinel topology:
		 *   - a new MINIMUM (no predecessor) splices after the
		 *     sentinel node;
		 *   - a new maximum lands before the sentinel naturally
		 *     (@succ0 is the sentinel pseudo-cell).
		 *
		 * The prepare records pred->next: succ0 -> cell and
		 * succ0->prev: pred -> cell.  Either neighbour may be the
		 * sentinel.
		 */
		pred_lnode = pred ? ft_ord_cell_lnode(pred) : &ft->ord_sentinel.node;
		/*
		 * A FAILING confirm or prepare is a peer conflict, not a soft
		 * no-op:
		 *   - pred2 != pred: a key interposed, or @pred's key
		 *     vanished, around the capture;
		 *   - -ENOENT: @pred died (its next carries the fused
		 *     unsplice's deletion mark);
		 *   - -EAGAIN: the order intent went stale, or the successor
		 *     is mid-deletion.
		 *
		 * The code used to splice anyway (a void cast).  That
		 * produced the resurrected / out-of-order cell class the
		 * ord-verify catches at rest.
		 *
		 * Unwind exactly as a commit ABORT would.  Nothing is
		 * installed:
		 *   - the records are discarded with the descriptor;
		 *   - the destroy clears the registered fences;
		 *   - the fresh cluster is freed, as the on-abort rollback
		 *     does.
		 *
		 * Age the handle first, exactly as a real commit ABORT ages
		 * it.  Then let the caller's ABORT path re-descend.
		 */
		/*
		 * ☞ TAKE THE TWO CELL WORDS THE SPLICE WILL WRITE.
		 *
		 * They are &pred->lnode.next (owner pred) and
		 * &succ0->lnode.prev (owner succ0).  Where a neighbour is the
		 * SENTINEL, the owner is a list END lock.  That lock is
		 * ord_begin_lock for the .next side and ord_end_lock for the
		 * .prev side.  This is why the plan carries the pseudo-cell
		 * rather than NULL.  The new cell needs no lock: it is
		 * unpublished.
		 *
		 * The insert-shaped validation re-checks pred->next == succ0
		 * UNDER the locks.  @pred2 answers the same question
		 * unlocked.  Once the check passes under the locks, the
		 * answer cannot change before the commit: every writer of
		 * those words needs one of the locks.
		 *
		 * splice_conflict: already ends in
		 * ft_flip_txn_destroy(ic->txn).  That terminal releases
		 * whatever was registered.
		 */
		{
			struct ft_cell_plan plan = {
				.cell = NULL,		/* insert shape */
				.pred = ft_ord_cell_of(pred_lnode),
				.succ = succ0,
			};

			if (ft_cell_lockset_take(ft, NULL, ic->txn, &plan))
				goto splice_conflict;
		}
		if (pred2 == pred &&
		    ft_txn_list_insert_between_prepare(ft, ic->txn,
				ft_ord_cell_lnode(cell), pred_lnode,
				ft_ord_cell_lnode(succ0)) >= 0)
			goto spliced;
splice_conflict:
		ft_free_unpublished_split_cluster(ft, ic->created,
			ic->nr_created);
		ft_flip_txn_conflict(ic->txn);
		ft_flip_txn_destroy(ic->txn);
		ic->txn = NULL;
		return URCU_TXN_STATUS_ABORT;
spliced:;
	}

	/*
	 * Record the deferred LIVE re-parent edge (split-compressed shapes)
	 * into the SAME txn.  The live old child's back-pointer is the
	 * back-channel a reanchor up-walk follows into the fresh cluster.
	 * The one commit flips it together with the forward edge (recorded
	 * at the publish site) and the neighbour edges.  Structural
	 * reachability and the ordered-list splice are then a single atomic
	 * publication for every reader.
	 */
	if (ic->live_child)
		ft_park_live_parent_edge(ft, ic->live_child,
			ic->live_parent, ic->live_slot, ic->txn,
			ic->ctx FT_BE_SRC_ARG(ic));
	/*
	 * ...and the fresh parent it now names is published LOCKED.  So is
	 * the spine node that is the child's own lock word, if that node is
	 * fresh too (ft_flip_txn_lock_born).  The edge just parked settles
	 * after the decide, and nobody may lock its owner before then.  This
	 * runs after the park, so the park's records keep the kinds they
	 * had.  It runs before any guard on those words.
	 */
	if (ic->live_child && ft->lock_fine && !ft->exclusive)
		ft_flip_txn_lock_born_spine(ft, ic->txn, ic->born,
			ic->nr_born);

	/*
	 * Freeze-on-free (doc §4.B).  This commit retires an old compressed
	 * or internal node.  Its one-way LIVE->DEAD tombstone is recorded
	 * INTO the same txn as the structural unlink.  The mark and the
	 * unlink then flip atomically (atomic detach).  A concurrent writer
	 * that targets the node fails its validate-live CAS.  It never sees
	 * a torn "unlinked but still live" window.  A no-op under one
	 * writer.  Recorded here, after the last abort point: all recording
	 * into the pre-reserved txn is infallible.
	 */
	if (ic->free_old_cn)
		/*
		 * FENCED retire (F2 split extension).  The expected old is
		 * the builder's entry-mark snapshot.  The commit therefore
		 * ratifies exactly the cn state the split plan was derived
		 * from.  The fence itself was registered with @txn at arm
		 * time.  Every non-commit outcome clears it.  On commit,
		 * this transition consumes it.  One record while the lock
		 * sits on the cn itself, two once coarsening moved it to a
		 * surviving ancestor.
		 */
		/*
		 * NULL ledger: the commit runs past the split's lock context.
		 * @free_old_cn is the node this op RETIRES.  Nothing of this
		 * op anchors on a word it is about to tombstone.  The arm
		 * that needs the ledger therefore cannot apply.
		 */
		ft_flip_txn_record_retire_anchored(ic->txn, NULL,
			&ic->free_old_cn_held,
			cds_ft_item_to_metadata(
				(struct cds_ft_inode *) ic->free_old_cn));
	/*
	 * @free_old_node needs NO record here.  Its sole producer is the
	 * ft_attach_node relocation.  That recompact runs with @ic->txn as
	 * the retire txn and records the fenced {LOCK|s -> TOMBSTONE|s}
	 * transition itself.  A second raw {s -> s|TOMBSTONE} record on the
	 * same word would same-slot-UPGRADE the recompact's new value.  It
	 * would also leak the fence bit into the dead word.  The deferred
	 * free below still covers @free_old_node.
	 */
	/*
	 * Order-statistics count fold (rank stats ON).  Record the +1
	 * key-count propagation as nr_keys value-CAS edges on the STABLE
	 * ancestor chain from @count_from to the root.  The edges are folded
	 * into THIS commit, so the count flips atomically with the
	 * structural publish.  A no-op when rank stats are off or the shape
	 * did not opt in.  The arm reserved the per-ancestor edges.
	 */
	if (ic->count_folded)
		ft_flip_txn_record_count_parent(ft, ic->txn, ic->count_from, 1);
	/*
	 * THE commit.  It installs every recorded edge:
	 *   - the forward structural publish (the forward slot plus any
	 *     compressed-parent skip-slot dual, or the set_nth slot proxy);
	 *   - the ordered-list neighbour edges;
	 *   - the live re-parent edge.
	 *
	 * It then flips the group and settles each slot to its direct value,
	 * all atomically.  The real top's writer-only wiring (parent, slot
	 * offset, incoming_byte) was done at record time.  The top was
	 * still invisible then.
	 */
	/*
	 * Set the scope of the commit-outcome cleanup, then commit.  The
	 * cleanup is dispatched below, by outcome:
	 *   - the finalize runs IFF the commit lands.  It grace-frees the
	 *     retired free_old_* nodes: readers resolved the parked proxy
	 *     to them until now;
	 *   - the rollback runs otherwise (a peer writer won the forward
	 *     slot or froze a guarded node).  It frees the fresh
	 *     unpublished cluster.
	 *
	 * The outcome thus settles both hazards:
	 *   - the retire target is still live on abort: do not free it;
	 *   - the fresh cluster would leak on abort: do free it.
	 *
	 * @ic outlives the commit (the caller's retry frame).
	 */
	ic->ft = ft;
	st = ft_flip_txn_commit(ft, ic->txn);
	ic->txn = NULL;
#ifdef FT_DEBUG_DEL_TOMB
	if (st == URCU_TXN_STATUS_OK && ft_dt_last_reserve_nf)
		ft_dt_check_flag("ins-commit", 0, ft_dt_last_reserve_nf, 0, NULL);
	ft_dt_last_reserve_nf = NULL;
#endif
	/*
	 * Dispatch the commit-outcome cleanup inline: the engine has no
	 * defer-on-commit/abort mechanism.
	 *   - OK: the retired free_old_* nodes are now unreachable.  The
	 *     finalize queues their grace-period-deferred free.
	 *   - ABORT or MEMORY_ERROR: nothing was installed.  The rollback
	 *     frees the fresh unpublished cluster.
	 *
	 * Both callbacks only touch memory whose reachability the commit
	 * outcome has already settled.  Running them here is therefore
	 * equivalent to running them inside the commit.
	 */
	if (st == URCU_TXN_STATUS_OK)
		ft_insert_finalize_cb(ic);
	else
		ft_insert_abort_cb(ic);
	return st;
}

#ifdef FEATURE_FT_PROBE_EMPTY_INSERT
/*
 * A/B CANDIDATE, insert side.  Refuse to publish an EMPTY non-root internal
 * into a live parent slot.  Count every refusal.
 *
 * WHY HERE, AND WHY A BAIL RATHER THAN A PROBE.  The residual is a live
 * reachable internal with:
 *   - nr_child == 0 and no external_nodes;
 *   - a CLEAN state word;
 *   - the smallest internal type;
 *   - an incoming_byte that agrees with the parent slot it hangs from.
 *
 * It is correctly formed and correctly wired.  Only its CONTENT is missing.
 * That fits a fresh node PUBLISHED empty far better than a node emptied in
 * place.  Every removal-side arm has now been refuted by an A/B: its bail
 * never fired while the defect kept happening.  Those arms are:
 *   - the detach boundary re-validation;
 *   - the shape-D fusion gate;
 *   - the FT_RECOMPACT_DEL sizing;
 *   - the recompact copy loop's NULL-child skip.
 *
 * Six earlier instrumentation designs SUPPRESSED the defect.  One of them was
 * the tracing build with every event disabled.  A passive counter at a shared
 * chokepoint is therefore not a legitimate experiment here.  What remains
 * legitimate is an A/B of a candidate fix that CARRIES ITS OWN BAIL COUNTER.
 * It is placed on the INSERT side, not in the hot path that suppressed the
 * defect last time.  The rule that settles it: a change that makes the defect
 * vanish while its bail NEVER FIRES has not fixed anything.
 *
 * Compiled out entirely by default.  The baseline arm of the A/B is thus the
 * ordinary build, not a differently-shaped one.
 */
extern unsigned long cds_ft_probe_empty_publish_split;
extern unsigned long cds_ft_probe_empty_publish_attach;
/*
 * REACH controls.  "empty == 0" is evidence only if the site RUNS.  The same
 * trap let the replace family's abort arms sit unexecutable.  These count
 * every visit to the two guarded publishes.  A zero empty-count can then be
 * read as "this family is innocent", not as "this instrument never fired".
 */
extern unsigned long cds_ft_probe_reach_split;
extern unsigned long cds_ft_probe_reach_attach;

static inline
bool ft_probe_internal_is_empty(struct cds_ft *ft,
		struct cds_ft_inode_flag *nf)
{
	struct cds_ft_metadata *meta;

	if (!nf || ft_node_flip_proxy(nf) || ft_node_external(nf))
		return false;
	if (ft_node_compressed(nf))
		return false;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(nf))
		return false;
#endif
	(void) ft;
	meta = cds_ft_item_to_metadata(ft_node_ptr(nf));
	return ft_meta_nr_child(meta) == 0 && !meta->external_nodes;
}
#endif /* FEATURE_FT_PROBE_EMPTY_INSERT */

/*
 * Publish @new_top into @slot (owned by @parent_nf).  The publish is direct
 * (ft_publish_to_parent) or RECORDED (one-commit insert, @ic armed).
 *
 * The recorded form puts the forward publish into @ic->txn: the slot store,
 * plus a compressed parent's dual skip-slot store.  Both are captured via
 * _ft_publish_to_parent, which also runs @new_top's writer-only parent-slot
 * bookkeeping.  The forward edge then commits atomically with the
 * ordered-list splice at insert_done's single flip.
 *
 * The caller must have wired @new_top's parent back-pointer already
 * (parent-before-publish).  With a recorded forward edge the cluster only
 * becomes reachable at the commit.  The wiring is complete by then either
 * way.
 */
/*
 * §9.3, the GP member: "Lock-set: {C, P} (+ {GP} iff P compressed)".
 *
 * A publish under a COMPRESSED parent writes TWO slots, because a skip pointer
 * is a shortcut that names the FAR END.  Besides @parent_nf's own child slot,
 * _ft_publish_to_parent re-encodes the SKIP_X dual.  That slot lives in GP's
 * BODY, so §8.2 makes GP its owner.  The op must therefore HOLD GP, not
 * merely write through it.
 *
 * ☠ THE CONDITION MIRRORS THE PRODUCER EXACTLY.  This includes the mtxn the
 * slot is resolved with (NULL, as ft_insert_publish_or_park's @rec carries
 * none).  A condition that differs fails in one of two ways:
 *   - an acquire taken on a different derivation than the record locks the
 *     wrong word;
 *   - an acquire taken where no dual is recorded is pure contention on the
 *     hottest insert path.
 *
 * A COMPRESSED ROOT's dual slot IS &ft->root, which has no owning node and
 * takes the always-MW ft_flip_txn_record_root route -- nothing to acquire.
 *
 * Registering hands the fence to @ic->txn, so every pre-publish bail that
 * destroys the txn clears it.  A miss sets @acquire_miss and the commit
 * aborts all-or-none, exactly as the P acquire beside it does.
 */
#ifdef FT_DEBUG_DUAL_DROP
/*
 * ☠ A THRESHOLD PRINT IS NOT A COUNT.  This started as a "print every 256th
 * acquire" line copied from ft_lock_skip_dual_gp.  A run with 1..255 acquires
 * printed NOTHING.  That reads exactly like zero, and it is how the insert
 * lane was first reported as inert.  Report the TOTAL at exit so that a small
 * non-zero count cannot hide.
 */
unsigned long ft_dual_gp_acq_insert;
/*
 * Count WHERE it returns, not just whether it acquires.  "0 acquires" has
 * four very different causes here, and they are not the same finding:
 *   - the function is never called at all;
 *   - it is called with a non-compressed publish parent;
 *   - it is called, but the parent's own slot is not skip-encoded;
 *   - the dual is a ROOT dual.
 *
 * Only the first would mean the site is dead code.
 */
unsigned long ft_dual_ins_calls, ft_dual_ins_notcomp, ft_dual_ins_noskip,
	ft_dual_ins_root;
static void ft_dual_gp_acq_report(void) __attribute__((destructor));
static void ft_dual_gp_acq_report(void)
{
	fprintf(stderr, "FT DUAL-GP-INSERT calls=%lu notcompressed=%lu "
		"noskipslot=%lu rootdual=%lu ACQUIRED=%lu\n",
		uatomic_read(&ft_dual_ins_calls),
		uatomic_read(&ft_dual_ins_notcomp),
		uatomic_read(&ft_dual_ins_noskip),
		uatomic_read(&ft_dual_ins_root),
		uatomic_read(&ft_dual_gp_acq_insert));
}
#endif

/*
 * Return true iff this call actually ACQUIRED the dual's grandparent.  The
 * publish must pass that value as @dual_owner_held.
 *
 * Every early return below is a shape where no owned dual edge follows:
 *   - no compressed parent;
 *   - no skip-encoded slot ("no dual edge will be recorded");
 *   - a ROOT dual, whose record names no owner at all.
 *
 * Claiming ownership on those paths would hand FT_OWNER_ASSERT_OWNED a NULL
 * owner.  ft_flip_txn_owns answers false for it.  The assert would then fail
 * on a shape that is perfectly correct.
 */
static
bool ft_insert_lock_skip_dual_gp(struct cds_ft *ft,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag *parent_nf,
		struct ft_insert_commit *ic)
{
#ifdef FEATURE_FT_SKIP_COMPRESSED
	struct cds_ft_compressed_node *cn;
	struct cds_ft_metadata *cn_meta;
	struct cds_ft_inode_flag *gp_nf = NULL;
	struct cds_ft_inode_flag **skip_slot;

#ifdef FT_DEBUG_DUAL_DROP
	uatomic_inc(&ft_dual_ins_calls);
#endif
	if (!parent_nf || !ft_node_compressed(parent_nf)) {
#ifdef FT_DEBUG_DUAL_DROP
		uatomic_inc(&ft_dual_ins_notcomp);
#endif
		return false;
	}
	cn = ft_compressed_node_ptr(parent_nf);
	cn_meta = cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
	skip_slot = ft_txn_parent_slot_at(cn_meta, ft, NULL, &gp_nf);
	/* The dual is decided under GP, never before it (ft-helpers.h). */
	if (!skip_slot) {
#ifdef FT_DEBUG_DUAL_DROP
		uatomic_inc(&ft_dual_ins_noskip);
#endif
		return false;		/* no dual edge will be recorded */
	}
	if (skip_slot == &ft->root || !gp_nf) {
#ifdef FT_DEBUG_DUAL_DROP
		uatomic_inc(&ft_dual_ins_root);
#endif
		return false;		/* root dual: no owning node */
	}
#ifdef FT_DEBUG_DUAL_DROP
	{
		/*
		 * ☠ THE INSERT LANE HAD NO ARM YIELD, and its absence read as a
		 * clean zero.  ft_lock_skip_dual_gp carries this counter.  This
		 * function is a SEPARATE copy for the insert path.  A
		 * -DFT_DEBUG_DUAL_DROP run therefore reported 0 acquires on
		 * the insert rows, and that 0 said nothing about this lane.
		 * Same counter, same reason: "a green run cannot be read as
		 * 'the fix works' when it is really 'the site never ran'".
		 */
		uatomic_inc(&ft_dual_gp_acq_insert);
	}
#endif
	/*
	 * Use the assert's OWN predicate.  ft_lock_skip_dual_gp spells out
	 * the reason.  lock_or_guard's @shared and MISS exits are
	 * indistinguishable from here.  And "I called the acquire" is not
	 * "the txn owns the word".  Narrow on purpose: a hold filed outside
	 * this txn's registry answers false, and the dual stays MW.
	 */
	{
		enum ft_lock_or_guard_exit ex2 = FT_LOG_EXIT_NOT_FINE;

		/* Re-ask through the reporting entry, same reasoning. */
		ft_flip_txn_lock_or_guard_parent_ex(__func__, __LINE__, ft,
			ic->txn, ctx, gp_nf, FT_DEPTH_FROM_DESCENT, &ex2);
		return ex2 == FT_LOG_EXIT_REGISTERED ||
			ex2 == FT_LOG_EXIT_SHARED;
	}
#else
	(void) ft; (void) ctx; (void) parent_nf; (void) ic;
	return false;
#endif
}

static
void ft_insert_publish_or_park(struct cds_ft *ft,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag *parent_nf,
		unsigned int parent_depth,
		struct cds_ft_inode_flag **slot,
		struct cds_ft_inode_flag *new_top,
		struct cds_ft_inode_flag *expected_old,
		struct ft_insert_commit *ic)
{
	struct ft_pub_rec rec = { .ctx = ctx, .n = 0 };
	bool dual_gp_held;

	/*
	 * @expected_old: the plan-snapshot value of *slot (the old subtree
	 * this divergence replaces).  The caller captures it BEFORE its
	 * build, so that the commit CAS rejects a peer that raced the slot.
	 * See ft_pub_rec_add.
	 */

	/*
	 * @ic is mandatory and armed by the caller.  A bulk insert is a graft
	 * / merge_at, not this path.  The forward edge therefore always rides
	 * the txn.
	 */
	assert(ic && ic->txn);
	/*
	 * VALIDATE (§4.B) / LOCK (§9.1, LOCK_FINE).  @parent_nf is the
	 * compressed-split lock-set member this publish writes into:
	 *   - I-4a diverge / key-shorter publishes into CN's parent P;
	 *   - I-4b past-child publishes into CN itself.
	 *
	 * In every shape @parent_nf SURVIVES the commit, and its slot is a
	 * same-slot VALUE swap.  Its own body is never copied under the
	 * lock.  It is thus the value-swap publish target that
	 * ft_flip_txn_lock_or_guard_parent handles.  Acquire it as a RELEASE
	 * lock under LOCK_FINE, or fall back to the §4.B guard on a miss.
	 * All shapes are mutually exclusive with the ft_attach_node
	 * relocation guard.  The one reserved guard/release slot in ic->txn
	 * therefore covers whichever fires.
	 *
	 * DLM Step 1: a split builder may have pre-acquired @parent_nf as P
	 * in its one-commit lock-set (ic->parent_locked_holder set).  Then
	 * record P's RELEASE from the held snap instead of re-marking P.  A
	 * re-mark would MISS on the op's OWN still-set fence and self-abort
	 * via the guard fallback.  A NULL holder (past-child {CN}, non-split
	 * shapes) routes to the incremental lock_or_guard.  That route
	 * behaves exactly as non-DLM does.
	 *
	 * Coarsening adds the SHARED case.  P's lock rode CN's anchor, so
	 * the op holds it.  Its release is already recorded (with CN's, at
	 * registration).  Only the §4.B guard is left to plant:
	 *   - a re-acquire would miss on the op's own hold, exactly as a
	 *     re-mark would;
	 *   - a second release would double-record the one word.
	 */
	if (ic->parent_lock_shared) {
		/*
		 * P's word deduped onto CN's anchor, so the acquire filed
		 * nothing for P.  That acquire ran before @txn existed, so
		 * it could not cover P either.  Say so here: the op DOES
		 * hold the word that excludes P's writers (ft_flip_txn's
		 * @covered).
		 */
		ft_flip_txn_cover_member(ic->txn,
			ft_flag_to_metadata(ft, parent_nf));
		ft_flip_txn_guard_parent_ctx(ft, ic->txn, parent_nf, ctx);
	}
	else
		ft_flip_txn_hold_or_lock_parent(ft, ic->txn, ctx, parent_nf,
			parent_depth, ic->parent_locked_holder,
			ic->parent_locked_snap);
	/*
	 * The split's live child has been held since the lock set.  Hand its
	 * release to the txn, whose terminals now own it.  Do this BEFORE the
	 * arm below: the arm must follow the op's last register.  The field
	 * is cleared, so no bail releases the child twice.
	 */
	/* Held since the lock set, whether or not its word was its own. */
	ft_flip_txn_cover_member(ic->txn, ic->child_locked_member);
	if (ic->child_locked_holder) {
		ft_flip_txn_lock_register_member(ic->txn,
			ic->child_locked_holder, ic->child_locked_snap,
			ic->child_locked_member);
		/*
		 * Release it through the txn, as P's release is.  A fixed
		 * {LOCK|snap -> snap} would silently abort every commit the
		 * day another edge of this txn lands on the child's word.
		 */
		ft_flip_txn_record_anchor_release_held(ic->txn,
			ic->child_locked_holder);
		ic->child_locked_holder = NULL;
	}
	dual_gp_held = ft_insert_lock_skip_dual_gp(ft, ctx, parent_nf, ic);
	/*
	 * PHASE B, STEP B1 -- THE ARM.  This is the only point in the op
	 * where it is legal.  ft_flip_txn_arm_per_op's contract is "after
	 * the op's LAST ft_flip_txn_lock_register".  A lock registered later
	 * still protects its word.  But a record planted in between would be
	 * checked against a registry that does not yet name its owner.  The
	 * two lines above ARE the last two registers:
	 *   - P's release (or its guard on the shared/miss arms);
	 *   - when P is compressed, the SKIP_X dual's GP (§9.3's third
	 *     member).
	 *
	 * ☞ WHAT IT CONVERTS, AND WHAT IT DELIBERATELY DOES NOT.  Kind
	 * dispatch happens at RECORD time.  The arm therefore covers the
	 * records planted from here on: the forward publish below, and the
	 * retire / count / re-parent edges ft_insert_one_commit adds later.
	 * Records planted EARLIER on this same txn stay MW.  These are the
	 * recompact edges of the ft_attach_node relocation.  MW is STRICTER
	 * and always sound.  Converting them needs the arm moved above the
	 * recompact.  The recompact's own {C, P, GP} acquire has not
	 * finished at that point.  That is a later step, not a gap this one
	 * leaves open.
	 *
	 * The helper refuses an empty registry, a non-FINE trie, and a trie
	 * whose constructor already armed it trie-wide.  A shape that
	 * acquired nothing thus keeps today's all-MW behaviour.  It does not
	 * park on an exclusion it never took.
	 */
	ft_flip_txn_arm_per_op(ft, ic->txn);
	/*
	 * ☑ @dual_owner_held TRUE: this lane HOLDS the SKIP_X dual's
	 * grandparent.  ft_insert_lock_skip_dual_gp above acquires the
	 * DERIVED owner, by the same derivation _ft_publish_to_parent will
	 * name.  An acquire MISS sets @acquire_miss, so ft_flip_txn_commit
	 * discards the attempt UNPUBLISHED.  ft_lock_skip_dual_gp's own
	 * header draws the conclusion: "on every path that actually
	 * publishes the op HOLDS GP and an SW park on that body word would
	 * be legal."  Saying false here was the record disagreeing with the
	 * acquire two lines up.
	 *
	 * ☠ WHY THIS IS SAFE TO FLIP NOW, when the word-kind table's note
	 * (1) says the dual waits for every producer.  These producers share
	 * this word:
	 *   - the remove lane, which acquires GP the same way;
	 *   - the GLUE publishes.  A glue is a BULK op: it holds the FT-wide
	 *     writer lock, and the bulk gate parks point ops.  So "no peer
	 *     recompaction can be copying the SKIP_X dual's owner under it";
	 *   - the COMPACTION lane.
	 *
	 * Compaction is NOT in the validated set.  Its concurrency is the
	 * LAST step of the transition, after rekey (see enum
	 * cds_ft_writer_strategy).  It requires the caller's exclusion
	 * today.  It therefore never runs beside this op, and the two cannot
	 * disagree about a word's kind.
	 *
	 * The park still only happens where the txn is ARMED: @structural_sw
	 * decides the kind.  This hands the arm a true answer and forces
	 * nothing.
	 */
	/*
	 * &ft->root is the slot P would own if the root had a parent.  Its
	 * lock is therefore taken where P's is, but AFTER the arm, not
	 * beside P above.  An insert into an empty trie holds no node lock.
	 * Registering the root's lock first would arm a txn whose other
	 * records own nothing.  Taken last, the root lock still precedes the
	 * one record it protects.  It parks that record SW only where the
	 * node locks already armed the txn.  Otherwise the record is held
	 * MW, which is correct: the lock is what excludes.
	 */
	if (slot == &ft->root)
		ft_flip_txn_lock_root(ic->txn, ctx, (void **) slot,
			expected_old);
	_ft_publish_to_parent(ft, parent_nf, slot, new_top, expected_old, &rec,
		dual_gp_held);
	ft_flip_txn_record_pub_rec(ic->txn, &rec);
	ic->slot = slot;	/* sentinel: one-commit forward recorded */
	ic->publish_to_parent = true;
}

/*
 * DLM Step 1 (see doc/design/mw-writer-lock-escalation-model.md).  Acquire
 * the compressed-split lock-set {CN, P} in ONE all-or-none MCAS up front.
 * This replaces the incremental CN lock-acquire here plus the P
 * lock_or_guard inside ft_insert_publish_or_park.  It mirrors the recompact
 * hoist ({C,P}).
 *
 * P is CN's parent, the value-swap forward-publish target.  It is resolved
 * racy.  The read-set guard CN.parent==P validates it in the SAME commit.  A
 * peer that re-homes CN between the plan read and the acquire therefore
 * aborts the acquire, and the caller re-plans.
 *
 * On OK:
 *   - @*held names the word the acquire actually CAS'd for CN.  That is
 *     CN's own metadata under per-node granularity, and its ANCHOR's under
 *     a coarser one.  @*held also carries the clean snapshots the caller's
 *     release and retire terminals need (struct ft_held_anchor).
 *   - ic->parent_locked_holder/_snap carry P's held release.
 *     ft_insert_publish_or_park then records it
 *     (ft_flip_txn_hold_or_lock_parent) instead of re-marking.
 *
 * Under coarsening CN's own word was never locked.  The caller must
 * therefore name @held->lock, never @cn_meta, on every bail and registration.
 *
 * P == NULL (CN at the root): the lock-set is {CN} only, with no guard.
 * publish_or_park's NULL @parent_nf then no-ops as today.
 *
 * Return one of:
 *   - 0: the whole set is acquired;
 *   - -EAGAIN: a member is held, or a re-home aborted the commit.  NOTHING
 *     is acquired (abort-and-regrow);
 *   - -ENOMEM.
 */
#ifdef FT_DEBUG_LIVE_VALIDATE
/*
 * Liveness of the split's child lock: acquires, those with a child member,
 * refusals, and the longest run of consecutive refusals on one thread.
 */
static unsigned long ft_split_acq_calls, ft_split_acq_child,
	ft_split_acq_refused, ft_split_acq_reread, ft_split_acq_max_streak;
static __thread unsigned long ft_split_acq_streak;
static void ft_split_acq_report(void) __attribute__((destructor));
static void ft_split_acq_report(void)
{
	fprintf(stderr, "FT_SPLIT_ACQ calls=%lu child=%lu refused=%lu "
		"child_reread_refused=%lu max_streak=%lu%s\n",
		ft_split_acq_calls, ft_split_acq_child, ft_split_acq_refused,
		ft_split_acq_reread, ft_split_acq_max_streak,
#ifdef FT_RED_SPLIT_NO_CHILD_LOCK
		" [RED: no child lock]"
#else
		""
#endif
		);
}
static void ft_split_acq_note(bool child, int outcome)
{
	uatomic_inc(&ft_split_acq_calls);
	if (child)
		uatomic_inc(&ft_split_acq_child);
	if (!outcome) {
		ft_split_acq_streak = 0;
		return;
	}
	uatomic_inc(outcome == 2 ? &ft_split_acq_reread : &ft_split_acq_refused);
	if (++ft_split_acq_streak > CMM_LOAD_SHARED(ft_split_acq_max_streak))
		CMM_STORE_SHARED(ft_split_acq_max_streak, ft_split_acq_streak);
}
#endif

static inline
int ft_insert_dlm_acquire_split(struct cds_ft *ft,
		const struct ft_descent *d, struct cds_ft_inode_flag *cn_flag,
		unsigned int cn_depth,
		struct cds_ft_metadata *cn_meta, struct ft_held_anchor *held,
		struct ft_insert_commit *ic)
{
	struct cds_ft_inode_flag *pf_p = NULL;
	struct ft_dlm_member set[3];
	struct ft_lock_ctx lctx;
	unsigned int p_depth = 0;
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(cn_flag);
	struct cds_ft_inode_flag *child_plan;
	int dret, i;

	/* PLAN (read-only, racy): resolve CN's parent P. */
	(void) ft_resolve_parent_slot(cn_meta, ft, &pf_p);

	set[0] = (struct ft_dlm_member){ .nf = cn_flag, .node = cn_meta,
		.depth = cn_depth,
		/*
		 * The guard is a read-set validation of CN's OWN back-edge:
		 * the plan resolved P through it.  It therefore names
		 * @cn_meta even where the lock goes to an ancestor.
		 * Anchoring moves the exclusion, not the edge being
		 * validated.
		 */
		.guard_child = pf_p ? cn_meta : NULL, .guard_pf = pf_p };
	set[1] = (struct ft_dlm_member){ .nf = pf_p,
		.node = pf_p ? ft_flag_to_metadata(ft, pf_p) : NULL,
		.depth = p_depth };
	/*
	 * CN's only child, when it carries a state word.  The split re-homes
	 * it, and the boundary move re-anchors it (see
	 * @child_locked_holder).  CN's child is never compressed (no two
	 * adjacent compressed nodes), so only an internal child qualifies.
	 * It is planned from a racy read and re-read under CN's lock below.
	 */
	child_plan = ft_resolve_flip_proxy(rcu_dereference(cn->child));
	set[2] = (struct ft_dlm_member){ .nf = NULL };
#ifndef FT_RED_SPLIT_NO_CHILD_LOCK
	if (child_plan && ft_node_internal(child_plan)) {
		set[2].nf = child_plan;
		set[2].node = cds_ft_item_to_metadata(ft_node_ptr(child_plan));
		set[2].depth = cn_depth + cn->len;
	}
#endif

	/*
	 * ACQUIRE {CN, P} + the read-set guard CN.parent==P in ONE MCAS.
	 * The set primitive does two more things:
	 *   - it samples and guards CN's OWN word where coarsening left it
	 *     unlocked.  The split still RETIRES CN against that word;
	 *   - it dedupes the two members where one anchor covers both.
	 *
	 * The acquire is bound to the op's PERSISTENT handle when there is
	 * one.  It then ages and takes its lane turn (struct
	 * ft_lock_ctx.op).  @ic->txn is not live at this point (the commit
	 * is armed later), so the op carries only this txn's records here.
	 */
	ft_lock_ctx_init(&lctx, d, NULL, ic ? ic->op : NULL);
	dret = ft_dlm_acquire_set(ft, &lctx, set, 3);
#ifdef FT_DEBUG_LIVE_VALIDATE
	if (dret)
		ft_split_acq_note(set[2].nf != NULL, 1);
#endif
	if (dret)
		return dret == -ENOMEM ? -ENOMEM : -EAGAIN;
	/*
	 * CN is held now, so CN->child is stable.  A child that moved
	 * between the plan and the acquire is not the one locked.  Release
	 * and re-plan.  Do so ALSO when no child was planned (it was
	 * external or empty then).  A child that became internal since
	 * would otherwise be re-homed unlocked.  The live check would then
	 * be its only exclusion.
	 */
	if (ft_resolve_flip_proxy(rcu_dereference(cn->child)) != child_plan) {
		for (i = 0; i < 3; i++)
			if (set[i].nf && !set[i].held.shared)
				ft_meta_lock_release(set[i].held.lock);
#ifdef FT_DEBUG_LIVE_VALIDATE
		ft_split_acq_note(set[2].nf != NULL, 2);
#endif
		return -EAGAIN;
	}
#ifdef FT_DEBUG_LIVE_VALIDATE
	ft_split_acq_note(set[2].nf != NULL, 0);
#endif

	*held = set[0].held;
	/*
	 * P's held release, threaded to ft_insert_publish_or_park.  A P that
	 * shares CN's anchor was acquired WITH it, and its release is
	 * already recorded.  The publish is therefore told that P is held
	 * and accounted for, not absent.
	 */
	ic->parent_lock_shared = pf_p && set[1].held.shared;
	ic->parent_locked_holder = (pf_p && !set[1].held.shared) ?
		set[1].held.lock : NULL;
	ic->parent_locked_snap = pf_p ? set[1].held.lock_snap : 0;
	ic->child_locked_holder = (set[2].nf && !set[2].held.shared) ?
		set[2].held.lock : NULL;
	ic->child_locked_member = set[2].nf ? set[2].held.member : NULL;
	ic->child_locked_snap = set[2].nf ? set[2].held.lock_snap : 0;
	return 0;
}

/*
 * Release a compressed-split lock-set acquired by ft_insert_dlm_acquire_split
 * on a PRE-PUBLISH bail.  At that point CN's parent P is held but not yet
 * registered into ic->txn: publish_or_park registers it.
 *
 * The caller clears the CN fence itself, by ft_meta_lock_release(held.lock)
 * or the ic->txn drain, exactly as the incremental scheme did.  This
 * function only lifts the EXTRA word the DLM hoist acquired for P.  Where
 * coarsening mapped P onto CN's anchor there is no extra word: the caller's
 * own release lifts the one they share.  The shared marker is then simply
 * dropped.
 */
static inline
void ft_insert_dlm_release_parent(struct ft_insert_commit *ic)
{
	if (ic->parent_locked_holder) {
		ft_meta_lock_release(ic->parent_locked_holder);
		ic->parent_locked_holder = NULL;
	}
	ic->parent_lock_shared = false;
	/* The live child's lock, taken in the same set (see the field). */
	if (ic->child_locked_holder) {
		ft_meta_lock_release(ic->child_locked_holder);
		ic->child_locked_holder = NULL;
	}
}

/*
 * Arm the one-commit txn just before a fresh-head publish.  This can fail.
 * The caller's error unwind then runs with nothing published.  @ic is
 * mandatory: a bulk insert is a graft / merge_at, not this path.
 *
 * The txn is armed for BOTH ordered-list states.  The structural slot
 * publish commits through the txn either way (freeze-before-install, MCAS
 * Invariant-1):
 *   - with the list on, the txn additionally carries the cell-splice edges;
 *   - with the list off, it carries only the structural edges, and
 *     insert_done commits it with @cell == NULL.
 *
 * Return 0, or -ENOMEM.
 */
static
int ft_insert_commit_arm(struct cds_ft *ft, struct ft_insert_commit *ic,
		unsigned int count_edges)
{
	assert(ic);
	/*
	 * Forward publish (<=2: the slot store + a compressed parent's skip-slot
	 * dual) + <=4 cell neighbour edges (list-on) + the live re-parent edge.
	 */
	/*
	 * Edge budget.  The txn reserves room for all of these:
	 *   - the forward publish (<=2: the slot plus a compressed parent's
	 *     skip-slot dual).  On the attach path it is instead the new
	 *     key's reserved slot edge (1), plus the recompact-relocation
	 *     grandparent publish (<=2) folded in;
	 *   - <=4 cell neighbour edges (list-on) and the live re-parent edge
	 *     (<=7);
	 *   - the <=2 freeze-on-free tombstone edges (an old compressed
	 *     and/or old internal node this commit retires).  They are now
	 *     fused into the same flip as the unlink (atomic detach, doc
	 *     §4.B);
	 *   - 1 VALIDATE freeze guard (§4.B validate).  It guards the
	 *     relocation's live grandparent, or the in-place external-head
	 *     park holder.  These two insert shapes are mutually exclusive;
	 *   - 1 VALIDATE freeze guard on the IN-PLACE attach holder.  The
	 *     reserved-byte edge below stores a slot INSIDE the live attach
	 *     node.  This is NOT mutually exclusive with the external-head
	 *     park holder above, so it needs its own reservation;
	 *   - @count_edges nr_keys count edges (rank-stats-ON count fold),
	 *     one per STABLE ancestor from the count base to the root.  The
	 *     caller sizes it to the ACTUAL descent depth (0 when the shape
	 *     does not fold its count);
	 *   - 1 for the live re-parent's paired state edge.
	 *     ft_reparent_record_meta records parent AND slot-offset, two
	 *     records, when the parked live child bears metadata;
	 *   - 1 for the SKIP_X dual's GP release terminal (§9.3's GP
	 *     member).  A publish under a COMPRESSED parent re-encodes a
	 *     dual slot living in GP's body.  The op therefore acquires GP
	 *     (ft_insert_lock_skip_dual_gp), and its release rides this
	 *     commit;
	 *   - FT_ROOT_LOCK_MAX_RECORDS for @root_lock's release terminal,
	 *     when the publish lands in &ft->root (ft_flip_txn_lock_root);
	 *   - 2 for the fresh spine's born-lock releases.  These are the
	 *     lock words of the live re-home's new parent and of the child
	 *     itself, at most two nodes (ft_flip_txn_lock_born_spine).
	 */
	/*
	 * The content txn is STANDALONE.  Binding it to the op handle
	 * (ft_flip_txn_create_bounded_on) shares the op's descriptor and its
	 * install lane across attempts.  That sharing MANUFACTURES the very
	 * conflicts the retry loop then absorbs (see the commit log).  The
	 * op still carries @ic->op for enrolment.  It must not commit
	 * through it.
	 */
	ic->txn = ft_flip_txn_create_bounded(ft, 15 + count_edges
			+ FT_ROOT_LOCK_MAX_RECORDS + 2);
	if (!ic->txn)
		return -ENOMEM;
	return 0;
}

/*
 * Park a "new key at an existing internal node" publish into the one-commit
 * batch.  The head is published by storing it into @metadata->external_nodes
 * (not a child slot).  So park a flip proxy THERE: readers resolve it via
 * ft_dereference_external.  The structural publish then commits atomically
 * with the ordinal-cell splice at the single MCAS flip commit.  There is no
 * transient half-spliced list state.  The slot settles direct
 * (publish_to_parent false: there is no parent child-slot to re-encode).
 * @ic must be armed.
 *
 * @plan_old is the value the caller's arm was CHOSEN on.  It is NULL for a
 * fresh key (a reader resolves the proxy to "no head" until the commit), or
 * the chain being replaced.
 *
 * ☠ THE PLAN VALUE IS CHECKED UNDER THE HOLDER'S LOCK, not re-read into the
 * record.  The caller read @metadata->external_nodes with nothing held and
 * picked its arm from it.  Recording the POST-lock value as the expected-old
 * pinned nothing.  The record is a blind SW store once the holder is held
 * anyway.  This lost an insert (the prefix-key twin of the pigeon FENCED
 * ADD, 0187c60c):
 *   1. Two same-key inserts ending at one internal node both saw NULL.
 *   2. The first parked its head.
 *   3. The second overwrote it.
 *   4. Both reported OK.  The first head and its cell were orphaned.
 *
 * Return -EAGAIN (re-descend) when the value under the lock is not
 * @plan_old.  The caller's done handler destroys ic->txn, which releases the
 * holder.
 */
static
int ft_insert_park_external_nodes(struct cds_ft *ft,
		const struct ft_descent *d,
		struct cds_ft_metadata *metadata, struct cds_ft_node *node,
		struct cds_ft_node *plan_old, struct ft_insert_commit *ic)
{
	struct ft_lock_ctx lctx;

	/*
	 * ☠ STEP 1 FIRST: TAKE the holder, do not merely OBSERVE it.
	 *
	 * §8.2 puts the entry list in the HOLDER's own field, so @metadata
	 * owns the word this parks into.  Owning it is not enough: the op
	 * has to HOLD it.  A §4.B VALIDATE on @metadata->state stood here
	 * instead.  It is sound only while every writer of that word is MW.
	 * An MW guard arbitrates against an MW peer through the
	 * expected-old.  It does not arbitrate against an SW park at all.
	 * "A slot is SW xor MW, globally" (urcu_txn_store_sw).
	 *
	 * The moment ANY insert shape arms per-op, its lock releases and
	 * nr_child increments park @metadata->state SW.  This shape, holding
	 * nothing, would then be exactly the far-side op
	 * ft_flip_txn_record_state warns about: "an op that writes or
	 * validates this word having never performed step 1 at all...
	 * Ownership is TAKEN, never observed."
	 *
	 * So acquire and REGISTER the holder:
	 *   - On a hit, the stronger {LOCK|s -> s} release terminal replaces
	 *     the guard.  A concurrent remove can no longer freeze the
	 *     holder at all.  Before, it was only detected after the fact.
	 *   - On a miss, ft_flip_txn_lock_or_guard_parent sets
	 *     @acquire_miss.  The commit ABORTS, all-or-none, and the op
	 *     re-plans.  It deliberately does not spin.
	 *
	 * ★ BEFORE the record, never after.  The record asks
	 * ft_flip_txn_owns who owns the word it writes, so the registry has
	 * to name the holder already.
	 */
	ft_lock_ctx_init(&lctx, d, ic->txn, ic->op);
	ft_flip_txn_lock_or_guard_parent(ft, ic->txn, &lctx, d->nf,
		FT_DEPTH_FROM_DESCENT);
#ifndef FT_DEBUG_PARK_MISS_CARRIES_ON
	/*
	 * A MISSED TAKE ENDS THE ATTEMPT HERE, through the plan check's exit
	 * below: the caller destroys the txn and restarts.  The code used to
	 * carry on.  It parked the head and let the caller build the rest of
	 * a commit that could only be discarded.  That cost ~5 records per
	 * miss, with 118-158k misses per ft_inv leg.  An allocation failure
	 * inside the acquire stays a memory error.
	 */
	if (ic->txn && caa_unlikely(ic->txn->acquire_miss))
		return ic->txn->acquire_enomem ? -ENOMEM : -EAGAIN;
#endif
#ifndef FT_DEBUG_PARK_NO_PLAN_CHECK
	if (ft_dereference_external(metadata->external_nodes) != plan_old)
		return -EAGAIN;
#endif
	ft_flip_txn_record_reserved(ic->txn, /*owner=*/ metadata,
		(void **) &metadata->external_nodes,
		(void *) plan_old, (void *) node);
	ic->slot = (struct cds_ft_inode_flag **) &metadata->external_nodes;
	ic->publish_to_parent = false;
	return 0;
}

/*
 * Split a compressed node during insert when the new key diverges
 * from the compressed path at position @diverge_pos.
 *
 * Builds the following structure bottom-up:
 *
 *   [prefix compressed/internal] -> [branch internal]
 *                                    +- old_ordinal -> [suffix compressed/internal] -> old_child
 *                                    `- new_ordinal -> [new branch compressed/internal] -> new_leaf
 *
 * If diverge_pos == 0, no prefix is needed.  If the suffix or new
 * branch is 0 bytes, the child is placed directly.  If 1 byte, a
 * single-child internal node is used.  If >= 2 bytes, a compressed
 * node is created.
 *
 * The old compressed node's external_nodes (if any) are preserved
 * at the prefix level (or the branch if no prefix).
 *
 * Publishes the result at @parent_slot via rcu_assign_pointer and
 * frees the old compressed node.  Returns 0 on success, -ENOMEM
 * on allocation failure (compressed node left in place).
 */

/*
 * Free a freshly-built, never-published split cluster: the nodes tracked in
 * @created[0 .. @nr_created).  Two kinds of paths share it:
 *   - the -ENOMEM error paths of both compressed-split helpers;
 *   - the abort-fully path in ft_insert_compressed_key_shorter, when the
 *     one-commit arm fails after a successful build.
 *
 * The live old child (cn->child) is never tracked in @created.
 * free_*_unpublished free only the node, not its children.  The live subtree
 * is therefore never touched.
 *
 * Nodes are freed PARENT-FIRST, the reverse of the bottom-up creation order.
 * A skip-encoded top is resolved to its compressed node via its child's
 * back-pointer (ft_skip_to_compressed reads child->parent).  The child must
 * therefore still be live when the parent is freed.  A partial-build error
 * path never has both.  The post-build abort frees the whole cluster, so
 * there the order matters.
 */
static
void ft_free_unpublished_split_cluster(struct cds_ft *ft,
		struct cds_ft_inode_flag *const *created, int nr_created)
{
	int i;

	for (i = nr_created - 1; i >= 0; i--) {
		if (ft_node_compressed(created[i]))
			free_compressed_node_unpublished(ft,
				ft_compressed_node_ptr(created[i]));
		else if (ft_node_skip_compressed(created[i]))
			free_compressed_node_unpublished(ft,
				ft_skip_to_compressed(ft, created[i]));
		else
			free_cds_ft_node_unpublished(ft,
				ft_node_ptr(created[i]));
	}
}

static
int ft_split_compressed_insert(struct cds_ft *ft,
		const struct ft_descent *dsc,	/* anchor source for the lock-set */
		struct cds_ft_inode_flag **parent_slot,
		struct cds_ft_inode_flag *compressed_flag,
		const uint8_t *iter_key,	/* key bytes at compressed node's depth */
		unsigned int remaining_key,	/* key bytes remaining from compressed depth */
		unsigned int diverge_pos,	/* position within compressed path */
		struct cds_ft_node *child_node,	/* new external node to insert */
		unsigned int node_depth,	/* depth of the compressed node */
		struct ft_insert_commit *ic)	/* one-commit insert (mandatory) */
{
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(compressed_flag);
	struct cds_ft_metadata *cn_meta =
		cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
	/*
	 * F2 node lock, split-retire extension.  This split retires @cn and
	 * derives its WHOLE plan from it: the diverge slicing over
	 * cn->key_bytes, the cn->child snapshot and the deferred-edge
	 * captures.  So fence @cn BEFORE the first read, exactly as a
	 * recompact fences the node it copies.
	 *
	 * This gives single-splitter exclusion.  A peer split / collapse /
	 * retire of the SAME cn does one of two things:
	 *   - it holds the fence.  The mark here then fails with -EAGAIN,
	 *     and this op re-descends;
	 *   - it aborts at commit, against the precise expected old of the
	 *     fenced tombstone (ft_insert_one_commit records the retire from
	 *     @held).
	 *
	 * The fence is cleared locally on every bail until the arm registers
	 * it with @ic->txn.  From then on every terminal path of that txn
	 * owns the outcome.
	 *
	 * @held names the word the acquire CAS'd.  That is @cn_meta only at
	 * per-node granularity.  Every release and registration below goes
	 * through @held.lock, so that it lifts the lock this op actually
	 * took.
	 */
	struct ft_held_anchor held;
	int fret;
	struct cds_ft_inode_flag *fwd_expected_old;	/* set post-fence */

	/*
	 * DLM Step 1.  Under LOCK_FINE, acquire the whole split lock-set
	 * {CN, P} in one all-or-none MCAS up front.  P is CN's parent, the
	 * value-swap publish target ft_insert_publish_or_park writes into
	 * below.  This replaces the incremental CN mark here plus the P lock
	 * inside publish_or_park.  Non-DLM / non-lock_fine keeps the single
	 * CN lock-acquire (byte-identical).
	 */
	if (ft->lock_fine) {
		fret = ft_insert_dlm_acquire_split(ft, dsc, compressed_flag,
				node_depth, cn_meta, &held, ic);
	} else {
		struct ft_lock_ctx lctx;

		ft_lock_ctx_init(&lctx, dsc, ic ? ic->txn : NULL, ic ? ic->op : NULL);
		fret = ft_acquire_member(ft, &lctx, compressed_flag, cn_meta,
			node_depth, &held);
	}

	if (fret)
		return fret;	/* -EAGAIN: peer owns @cn/P; nothing built */

	/*
	 * Plan-snapshot the raw grandparent slot NOW (post-fence,
	 * pre-build).  *parent_slot is the skip-PRESERVED,
	 * flip-proxy-resolved reference to @cn.  Under skip-compression it
	 * is a SKIP_X flag, NOT the skip-resolved @compressed_flag.  That is
	 * exactly what the forward publish CAS must match.  The two
	 * alternatives are wrong:
	 *   - passing @compressed_flag would mismatch the stored SKIP_X
	 *     forever (livelock);
	 *   - re-reading at publish time would ratify a peer that raced
	 *     @cn's grandparent slot during the build.
	 */
	fwd_expected_old = ft_resolve_flip_proxy(*parent_slot);

	/*
	 * Compressed metadata never carries external_nodes
	 * (ft_metadata_set_external_nodes aborts on a compressed target).
	 * The prefix builders below rely on it.
	 */
	assert(!cn_meta->external_nodes);
	FT_TP(split_compressed_insert_enter, (const void *) cn,
		cn->len, diverge_pos);
	struct cds_ft_inode_flag *old_suffix_flag, *new_branch_flag;
	struct cds_ft_inode_flag *branch_flag, *top_flag;
	struct cds_ft_inode_flag *created[FT_MAX_DEPTH];
	int nr_created = 0;
	unsigned int suffix_len = cn->len - diverge_pos - 1;
	unsigned int new_len = remaining_key - diverge_pos - 1;
	uint8_t old_ordinal = cn->key_bytes[diverge_pos];
	uint8_t new_ordinal = iter_key[diverge_pos];
	unsigned long old_child_nr_keys;
	int ret;
	/*
	 * Two-phase publish.  First build the cluster invisibly.  At the
	 * end, wire the deferred back-pointers and swing the parent slot.
	 * See the rcu-mutation build-invisible pattern.
	 *
	 * suffix_len >= 1: the branch's children (suffix, new) are new
	 * cluster nodes, so set their back-pointers normally.  Only the edge
	 * of the live old child into the new suffix (cn->child -> sfx) is
	 * deferred (deferred edge 1).
	 *
	 * suffix_len == 0: the branch is a cluster-leaf.  Its old direction
	 * is the live cn->child, and its new direction is the new subtree.
	 * set_nth defers BOTH its children.  They are wired here at publish
	 * to the final branch_flag (deferred edges 1 and 2).
	 */
	struct cds_ft_inode_flag *sfx_skip_flag = NULL;
	struct cds_ft_inode_flag *deferred_child = NULL;
	struct cds_ft_inode_flag *deferred_parent = NULL;
	/* The fresh prefix / suffix metadata, for the born-lock spine. */
	struct cds_ft_metadata *spine_pfx_meta = NULL, *spine_sfx_meta = NULL;
	struct cds_ft_inode_flag **deferred_slot = NULL;
	struct cds_ft_inode_flag *deferred_child2 = NULL;
	struct cds_ft_inode_flag **deferred_slot2 = NULL;
	struct cds_ft_inode_flag *cur_parent;
	bool branch_cluster_leaf = (suffix_len == 0);

	unsigned int junction_depth = node_depth + diverge_pos;

	/*
	 * ONE resolved snapshot of the live cn->child (§9).  The slot may
	 * carry a peer's parked flip proxy:
	 *   - kind-dispatching the raw latch faults (item_to_metadata on the
	 *     latch memory);
	 *   - multiple raw re-reads could tear across the peer's flip.
	 *
	 * Every use below consumes THIS value: the sizing, the fresh
	 * suffix's forward wiring and the deferred-edge captures.  A peer
	 * commit after the snapshot is caught at commit time by our forward
	 * CAS / §4.B guard (ABORT -> re-descend).
	 */
	struct cds_ft_inode_flag *old_child_flag =
		ft_cn_child_dereference_acquire_prefetch(cn);

	/* Compute old child's nr_keys for the new nodes. */
	if (!ft_node_external(old_child_flag)) {
		struct cds_ft_metadata *cm =
			cds_ft_item_to_metadata(ft_node_ptr(old_child_flag));
		old_child_nr_keys = ft_nr_keys_get(cm);
	} else if (old_child_flag) {
		old_child_nr_keys = 1;	/* external leaf */
	} else {
		old_child_nr_keys = 0;
	}

	/* 1. Build old suffix -> old child. */
	if (suffix_len >= 1) {
		struct cds_ft_compressed_node *sfx;
		struct cds_ft_metadata *sfx_meta;

		sfx = alloc_compressed_node(ft, suffix_len, &sfx_meta);
		if (!sfx) goto error;
		spine_sfx_meta = sfx_meta;
		sfx->child = old_child_flag;
		sfx->len = suffix_len;
		memcpy(sfx->key_bytes, &cn->key_bytes[diverge_pos + 1], suffix_len);
		ft_meta_nr_child_set(sfx_meta, 1);
		ft_nr_keys_store(ft,sfx_meta, old_child_nr_keys, CMM_RELAXED);
		old_suffix_flag = ft_compressed_node_flag(sfx);	/* PLAIN: install + recover sfx directly */
		sfx_skip_flag = ft_publish_compressed(ft, sfx, old_suffix_flag);	/* skip form for the slot */
		created[nr_created++] = old_suffix_flag;	/* track PLAIN so the error path frees sfx directly */
		/*
		 * Defer re-parenting the live old child to publish.  Writing
		 * cn->child's back-pointer now would make this unpublished
		 * cluster observable from the bottom.  The branch install
		 * below would also recover sfx through it.  sfx->child
		 * already points at cn->child (a write into the new sfx only).
		 */
		deferred_child = old_child_flag;
		deferred_parent = old_suffix_flag;
		deferred_slot = &sfx->child;
	} else {
		/* suffix_len == 0: old child directly. */
		old_suffix_flag = old_child_flag;
	}

	/* 2. Build new branch -> new leaf. */
	if (new_len >= 1) {
		struct cds_ft_compressed_node *nb;
		struct cds_ft_metadata *nb_meta;

		nb = alloc_compressed_node(ft, new_len, &nb_meta);
		if (!nb) goto error;
		nb->child = (struct cds_ft_inode_flag *) child_node;
		nb->len = new_len;
		{
			unsigned int k;

			for (k = 0; k < new_len; k++)
				nb->key_bytes[k] = iter_key[diverge_pos + 1 + k];
		}
		ft_meta_nr_child_set(nb_meta, 1);
		ft_nr_keys_store(ft,nb_meta, 1, CMM_RELAXED);
		new_branch_flag = ft_compressed_node_flag(nb);
		/*
		 * HIDDEN: no reader or peer can reach either node until this
		 * cluster publishes.  @child_node is the caller's "new
		 * external node to insert" (this function's own parameter
		 * comment).  @nb was allocated three lines up.
		 */
		ft_set_parent_excl(ft, nb->child, new_branch_flag, NULL,
			FT_EXCL_HIDDEN);
		new_branch_flag = ft_publish_compressed(ft, nb, new_branch_flag);
		created[nr_created++] = new_branch_flag;
	} else {
		/* new_len == 0: child_node directly. */
		new_branch_flag = (struct cds_ft_inode_flag *) child_node;
	}

	/* 3. Build branch node with both children. */
	{
		struct cds_ft_inode_flag *dest = NULL;
		struct cds_ft_metadata *branch_meta;

		/* First child: old direction. */
		ret = ft_node_set_nth(ft, &dest, old_ordinal, old_suffix_flag, NULL, NULL,
				junction_depth, branch_cluster_leaf);
		if (ret) goto error;
		created[nr_created++] = dest;
		branch_flag = dest;

		/* Second child: new direction. */
		{
			struct cds_ft_inode *old_recompacted = NULL;

			branch_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
			ret = ft_node_set_nth(ft, &dest, new_ordinal, new_branch_flag,
					&old_recompacted, branch_meta, junction_depth,
					branch_cluster_leaf);
			if (ret) goto error;
			if (old_recompacted) {
				free_cds_ft_node(ft, old_recompacted);
				/* Update created entry to the recompacted node. */
				created[nr_created - 1] = dest;
			}
		}

		branch_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
		ft_nr_keys_store(ft,branch_meta, old_child_nr_keys + 1,
				CMM_RELAXED);
		branch_flag = dest;

		/*
		 * sfx was installed via its PLAIN flag, so that ft_set_parent
		 * could recover it directly.  It did not have to read
		 * cn->child's back-pointer, which still points at the old cn.
		 * Now re-encode the old-direction slot to sfx's skip form.
		 * This is a value write into the still-unpublished branch.
		 * The slot becomes recoverable once cn->child's back-pointer
		 * is set at publish.
		 */
		if (suffix_len >= 1 && sfx_skip_flag != old_suffix_flag) {
			struct cds_ft_inode_flag **oslot = NULL;

			ft_node_get_nth_skip(branch_flag, &oslot, old_ordinal,
					FT_PF_NONE);
			if (oslot)
				*oslot = sfx_skip_flag;
		}

		/*
		 * suffix_len == 0: the branch is a cluster-leaf, so set_nth left
		 * both of its children unparented.  Record both deferred edges
		 * (old = live cn->child, new = the new subtree) against the now-
		 * final branch_flag; phase 2 wires them.  The forward slots are
		 * already correct (value-copied by set_nth / recompaction).
		 */
		if (branch_cluster_leaf) {
			ft_node_get_nth_skip(branch_flag, &deferred_slot,
					old_ordinal, FT_PF_NONE);
			ft_node_get_nth_skip(branch_flag, &deferred_slot2,
					new_ordinal, FT_PF_NONE);
			deferred_child = old_child_flag;
			deferred_parent = branch_flag;
			deferred_child2 = new_branch_flag;
		}
	}

	/*
	 * 4. Build prefix -> branch (if needed).  @cn carries no
	 * external_nodes (asserted at entry), so the prefix is always a
	 * plain compressed run over key_bytes[0 .. diverge_pos).
	 */
	if (diverge_pos >= 1) {
		struct cds_ft_compressed_node *pfx;
		struct cds_ft_metadata *pfx_meta;
		struct cds_ft_inode_flag *pfx_child;

		pfx = alloc_compressed_node(ft, diverge_pos, &pfx_meta);
		if (!pfx) goto error;
		spine_pfx_meta = pfx_meta;
		pfx->child = branch_flag;
		pfx->len = diverge_pos;
		memcpy(pfx->key_bytes, cn->key_bytes, diverge_pos);
		ft_meta_nr_child_set(pfx_meta, 1);
		ft_nr_keys_store(ft,pfx_meta, ft_nr_keys_get(cn_meta) + 1, CMM_RELAXED);
		pfx_child = ft_compressed_node_flag(pfx);
		/* HIDDEN: @branch_flag is this split's fresh branch and @pfx was
		 * allocated seven lines up; the cluster publishes later. */
		ft_set_parent_excl(ft, branch_flag, pfx_child, &pfx->child,
			FT_EXCL_HIDDEN);
		pfx_child = ft_publish_compressed(ft, pfx, pfx_child);
		created[nr_created++] = pfx_child;
		top_flag = pfx_child;
	} else {
		/* diverge_pos == 0: branch IS the top. */
		struct cds_ft_metadata *branch_meta =
			cds_ft_item_to_metadata(ft_node_ptr(branch_flag));
		ft_nr_keys_store(ft,branch_meta, ft_nr_keys_get(cn_meta) + 1,
				CMM_RELAXED);
		top_flag = branch_flag;
	}

	/* 5. Publish the split structure, replacing the compressed node. */
	FT_TP(compressed_split, "insert", (const void *) cn, cn->len,
		(const void *) top_flag, diverge_pos);
	/*
	 * Compressed-split replaces the compressed node in its parent's
	 * slot via a direct ft_publish_to_parent call, bypassing
	 * ft_node_set_nth.  Emit tree_edge_set explicitly so consumers
	 * see the (parent, key_byte, top_flag) structural edge.  The
	 * compressed node sits at node_depth.  iter_key points at the
	 * key bytes starting at that depth.  iter_key[-1] is therefore
	 * the parent's key_byte that led to the compressed node.  This
	 * is safe for node_depth >= 1, which always holds since
	 * compressed nodes are never at the root.
	 */
	FT_TP(tree_edge_set, (const void *) ft,
		(const void *) ft_parent_node(CMM_LOAD_SHARED(cn_meta->parent_word)),
		(unsigned int) (node_depth - 1),
		(uint8_t) iter_key[-1],
		(const void *) top_flag);
	/*
	 * Phase 2 (publish) -- no failures past here.  Wire every
	 * back-pointer before swinging the parent's forward slot.  An
	 * up-walk that lands on the new cluster from either direction then
	 * sees the back-pointers wired before the cluster becomes
	 * reader-reachable.
	 *
	 * ORDER among the deferred edges matters:
	 *   - deferred_child is always the LIVE old child (cn->child),
	 *     re-parented into the cluster.  Setting its back-pointer is
	 *     itself a back-channel publish.  A reader up-walking from
	 *     cn->child immediately enters the new cluster, and can then
	 *     scan the cluster's other (sibling) slots.
	 *   - deferred_child2 is the FRESH new subtree, observable only
	 *     through the cluster.
	 *
	 * So wire the cluster top's own back-pointer and the fresh edge
	 * FIRST, and the live edge LAST.  Otherwise a reader that enters
	 * via the live child reads a sibling slot pointing at the fresh
	 * subtree, whose parent is not yet set.  The reader's consume chain
	 * is anchored at the live back-pointer store.  It has no
	 * happens-before edge to the later fresh-parent store.  The reader
	 * therefore observes a stale NULL parent (ft_skip_reanchor holder
	 * == NULL).
	 *
	 * suffix_len >= 1: only the live edge exists (cn->child -> sfx).
	 * suffix_len == 0: cluster-leaf branch's two children (live cn->child
	 * and fresh new subtree), both -> branch_flag.
	 */
	/*
	 * I3 count fold reservation.  The diverge caller records the +1
	 * count walk from the STABLE parent of the split compressed node up
	 * to the root.  That parent is d->pnf, and its slot @parent_slot is
	 * what this commit flips.  The walk takes at most @node_depth
	 * ancestor edges.  Every fresh split node already carries its full
	 * post-commit count, so the walk never touches one.  Reserve by the
	 * actual node depth (0 when rank stats are off).
	 */
	/*
	 * Concurrent-writer conflict check (Phase 4.3).  @parent_slot was
	 * recorded at descent as cn's slot in its parent.  A peer writer may
	 * since have re-parented cn: it split cn's parent and re-homed cn
	 * under a fresh junction.  cn_meta->parent and @parent_slot then
	 * disagree.  ft_set_parent below would invert that (parent, slot)
	 * pair against the wrong node's bitmap and fault.
	 *
	 * cn's CURRENT slot (recomputed from cn_meta) must therefore still
	 * be @parent_slot.  If not, signal a re-descend.  The fresh cluster
	 * is unreachable, so free it now.
	 *
	 * A commit-time window remains: cn re-parented AFTER this check but
	 * before the forward install.  The forward slot's expected-value CAS
	 * and the §4.B freeze guard catch it.  ft_insert_publish_or_park
	 * records both, and the conflict surfaces as ABORT.
	 *
	 * Capture @cur_parent from the SAME resolved snapshot that validates
	 * the slot.  A peer re-home commits cn_meta->parent and the
	 * state-word slot offset as ONE co-committed MCAS pair.  A separate
	 * raw re-read of the parent below could return a peer's freshly
	 * re-homed parent P2 while @parent_slot still indexes the old parent
	 * P1.  ft_set_parent would then invert (P2, @parent_slot) and fault
	 * ft_slot_to_byte with an out-of-range rank against P2's bitmap.
	 * One snapshot keeps the pair consistent.  A re-home that lands
	 * AFTER it is caught by the commit CAS.
	 */
	if (ft_resolve_parent_slot(cn_meta, ft, &cur_parent) != parent_slot) {
		ft_free_unpublished_split_cluster(ft, created, nr_created);
		ft_meta_lock_release(held.lock);
		ft_insert_dlm_release_parent(ic);	/* release P held up front */
		return -EAGAIN;
	}
	/*
	 * Hand the fresh cluster to the op scope.  The commit's on-abort
	 * rollback then frees it if a peer writer wins the forward slot at
	 * commit time.
	 */
	memcpy(ic->created, created, (size_t) nr_created * sizeof(created[0]));
	ic->nr_created = nr_created;

	ret = ft_insert_commit_arm(ft, ic,
		ft->rank_stats ? node_depth + 2 : 0);
	if (ret)
		goto error;
	/*
	 * The armed txn now owns the fence outcome:
	 *   - the fence is registered, so every non-commit terminal (destroy
	 *     / ABORT / MEMORY_ERROR) clears it;
	 *   - on commit, the fenced tombstone transition consumes it.
	 *
	 * Past this point the local error path must NOT clear it.  The
	 * caller's unwind destroys @ic->txn, which drains the registry.
	 */
	ft_flip_txn_lock_register_held(ic->txn, &held);
	ft_flip_txn_record_anchor_release(ic->txn, &held, cn_meta);
	ic->free_old_cn_held = held;
	/*
	 * @cur_parent came from the guard's ft_resolve_parent_slot snapshot.
	 * It is consistent with @parent_slot: a peer re-home commits the
	 * parent and its slot offset as one MCAS pair.  The pair is resolved
	 * from a single status snapshot, so it cannot tear.
	 *
	 * Do NOT re-read cn_meta->parent here.  A raw reload could observe a
	 * peer's re-homed parent while @parent_slot still indexes the old
	 * one.  That faults ft_slot_to_byte on the mismatched bitmap.  A
	 * flip proxy is already resolved by ft_resolve_parent_slot (never
	 * the type-7 latch itself).  If the peer re-homes cn AFTER the
	 * guard, our forward CAS / §4.B guard aborts and this fresh cluster
	 * is discarded.
	 */
	/* HIDDEN: @top_flag is this build's fresh cluster top -- the note above
	 * ends "this fresh cluster is discarded" on the abort path. */
	ft_set_parent_excl(ft, top_flag, cur_parent, parent_slot,
		FT_EXCL_HIDDEN);
	if (deferred_child2)
		/*
		 * HIDDEN: "deferred_child2 is the FRESH new subtree, observable
		 * only through the cluster" (the ordering note above).  Its
		 * sibling @deferred_child is "always the LIVE old child" and is
		 * NOT stored here -- it is parked into the one-commit below.
		 */
		ft_set_parent_excl(ft, deferred_child2, deferred_parent,
			deferred_slot2, FT_EXCL_HIDDEN);
	if (deferred_child) {
		/*
		 * deferred_child is the LIVE old child (cn->child).  Setting
		 * its back-pointer is the back-channel publish that exposes
		 * this fresh cluster to a reanchor up-walker.  So defer it to
		 * the parked one-commit: ft_insert_one_commit parks it into
		 * the SAME flip batch.  Structure, cell and this edge then
		 * flip atomically.  The splice search runs from the root, so
		 * it does not need the edge wired early.
		 */
		ic->live_child = deferred_child;
		ic->live_parent = deferred_parent;
		ic->live_slot = deferred_slot;
		/* The fresh spine, top first: [prefix] branch [suffix]. */
		ic->nr_born = 0;
		if (spine_pfx_meta)
			ft_ic_born_add(ic, spine_pfx_meta, node_depth,
				diverge_pos);
		ft_ic_born_add(ic, cds_ft_item_to_metadata(
				ft_node_ptr(branch_flag)),
			node_depth + diverge_pos, 1);
		if (deferred_parent == old_suffix_flag && spine_sfx_meta)
			ft_ic_born_add(ic, spine_sfx_meta,
				node_depth + diverge_pos + 1, suffix_len);
		FT_BE_SRC_SET(ic, FT_BE_PARK_SPLIT);
	}
#ifdef FEATURE_FT_PROBE_EMPTY_INSERT
	/* A/B: never wire an empty internal under a live parent (see the note). */
	__atomic_fetch_add(&cds_ft_probe_reach_split, 1, __ATOMIC_RELAXED);
	if (cur_parent && ft_probe_internal_is_empty(ft, top_flag)) {
		__atomic_fetch_add(&cds_ft_probe_empty_publish_split, 1,
			__ATOMIC_RELAXED);
		ret = -EAGAIN;
		goto error;
	}
#endif
	{
		struct ft_lock_ctx pctx;

		/*
		 * @cur_parent came from CN's back-pointer, so the descent's
		 * window is what dates it.
		 */
		ft_lock_ctx_init(&pctx, dsc, ic ? ic->txn : NULL, ic ? ic->op : NULL);
		ft_insert_publish_or_park(ft, &pctx, cur_parent,
			FT_DEPTH_FROM_DESCENT, parent_slot, top_flag,
			fwd_expected_old, ic);
	}

	/*
	 * 7. Free the old compressed node.  Parked publish: readers resolve
	 * the proxy to @cn until the commit, so defer the free past it.
	 */
	ic->free_old_cn = cn;

	return 0;

error:
	/*
	 * Reached only BEFORE the post-arm registration: on a build
	 * allocation failure, or when the arm itself fails with @ic->txn
	 * never created.  The fence is therefore still locally owned: lift
	 * it with the cluster teardown.  The DLM hoist also holds P up front
	 * (publish_or_park is not reached here), so release it too.
	 */
	ft_free_unpublished_split_cluster(ft, created, nr_created);
	ft_meta_lock_release(held.lock);
	ft_insert_dlm_release_parent(ic);
	return -ENOMEM;
}


/*
 * Split a compressed node when the insert key is shorter than the
 * compressed path (key terminates within the path).
 *
 * Builds: [prefix] -> [junction] -> [suffix] -> old_child
 *
 * The junction is an internal node at the key endpoint depth with
 * one child (the suffix direction).  The caller stores the new
 * node as external_nodes on the junction and handles publication,
 * propagation, and freeing of the old compressed node.
 *
 * @parent_slot: address of the slot in cn's parent that holds cn.  Used
 * to wire top_flag's own back-pointer into the live parent BEFORE the
 * deferred (back-channel) re-parent of cn->child into the new suffix.
 * Otherwise an up-walk from cn->child enters the new cluster and walks
 * up to top_flag, which would have parent == NULL.
 *
 * On success, sets *top_ret to the topmost node (prefix or junction)
 * and *jct_ret to the junction node.  Returns 0.
 * On failure, frees any partially created nodes and returns -ENOMEM.
 */
static
int ft_split_compressed_key_shorter(struct cds_ft *ft,
		struct cds_ft_inode_flag *compressed_flag,
		struct cds_ft_inode_flag **parent_slot,
		unsigned int remaining,
		struct cds_ft_inode_flag **top_ret,
		struct cds_ft_inode_flag **jct_ret,
		unsigned int node_depth,
		/*
		 * Out: the LIVE re-parent edge of the old child (cn->child into
		 * the new suffix/junction).  It is RETURNED, not wired.  The
		 * caller does one of two things:
		 *  - defer it to the parked one-commit, so that it flips
		 *    atomically with the cell;
		 *  - wire it directly (list-off / duplicate).
		 * See the same shape in ft_split_compressed_insert.
		 * NULL out = no live edge.
		 */
		struct cds_ft_inode_flag **live_child_ret,
		struct cds_ft_inode_flag **live_parent_ret,
		struct cds_ft_inode_flag ***live_slot_ret,
		struct ft_born_spine_node *born_ret,
		unsigned int *nr_born_ret,
		/*
		 * Out: the nodes of the fresh cluster, still unpublished.  The
		 * caller can free them if it aborts after this function
		 * returns.  Optional (NULL).
		 */
		struct cds_ft_inode_flag **created_ret,
		int *nr_created_ret)
{
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(compressed_flag);
	struct cds_ft_metadata *cn_meta =
		cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
	unsigned int suffix_len = cn->len - remaining - 1;

	/* Compressed metadata never carries external_nodes (see
	 * ft_split_compressed_insert); the prefix builders rely on it. */
	assert(!cn_meta->external_nodes);
	struct cds_ft_inode_flag *suffix_flag;
	struct cds_ft_inode_flag *jct_flag;
	struct cds_ft_inode_flag *top_flag;
	struct cds_ft_inode_flag *created[FT_MAX_DEPTH];
	int nr_created = 0;
	unsigned long child_nr_keys;
	int ret;
	/*
	 * Build-invisible / publish / reclaim (see rcu-mutation pattern).
	 * The live old child (cn->child) is re-parented into the new suffix.
	 * For suffix_len == 0 it goes straight into the junction.  Defer that
	 * single back-pointer to the failure-free tail.  A later allocation
	 * failure then frees a cluster that no reader has observed, and
	 * leaves cn->child untouched.  The caller publishes the top right
	 * after we return.  So this deferred (bottom) publish precedes the
	 * top forward publish.
	 */
	uint8_t jct_ordinal = cn->key_bytes[remaining];
	bool jct_cluster_leaf = (suffix_len == 0);
	struct cds_ft_inode_flag *sfx_skip_flag = NULL;
	struct cds_ft_inode_flag *deferred_child = NULL;
	struct cds_ft_inode_flag *deferred_parent = NULL;
	/* The fresh spine's metadata and spans, top first on return. */
	struct cds_ft_metadata *sp_pfx_meta = NULL, *sp_jct_meta = NULL,
		*sp_sfx_meta = NULL;
	unsigned int sp_pfx_span = 0, sp_sfx_span = 1;
	struct cds_ft_inode_flag **deferred_slot = NULL;

	/*
	 * Take ONE resolved snapshot of the live cn->child (§9); see
	 * ft_split_compressed_insert.  A raw read may hand a peer's parked
	 * flip proxy to the kind dispatch.  Repeated reads could tear across
	 * the peer's flip.  All uses below consume this value.  If a peer
	 * commits after the snapshot, the forward CAS aborts our commit.
	 */
	struct cds_ft_inode_flag *old_child_flag =
		ft_cn_child_dereference_acquire_prefetch(cn);

	if (!ft_node_external(old_child_flag)) {
		struct cds_ft_metadata *cm =
			cds_ft_item_to_metadata(ft_node_ptr(old_child_flag));
		child_nr_keys = ft_nr_keys_get(cm);
	} else if (old_child_flag) {
		child_nr_keys = 1;
	} else {
		child_nr_keys = 0;
	}

	/*
	 * Build suffix -> old child.
	 *
	 * Under SKIP_COMPRESSED, suffix_len >= 1 must produce a
	 * compressed (skip-encoded) node.  A 1-child internal at this
	 * level would violate the chain-compress invariant, and verify
	 * rejects it.  Under non-SC mode, suffix_len == 1 historically
	 * produced an internal node.  That is still acceptable: the
	 * invariant only applies in skip mode.
	 */
	if (suffix_len >= 2
#ifdef FEATURE_FT_SKIP_COMPRESSED
			|| (suffix_len == 1 && ft_group_skip_compressed(ft->group))
#endif
	   ) {
		struct cds_ft_compressed_node *sfx;
		struct cds_ft_metadata *sfx_meta;

		sfx = alloc_compressed_node(ft, suffix_len, &sfx_meta);
		if (!sfx) goto error;
		sfx->child = old_child_flag;
		sfx->len = suffix_len;
		memcpy(sfx->key_bytes, &cn->key_bytes[remaining + 1],
			suffix_len);
		sp_sfx_meta = sfx_meta;
		sp_sfx_span = suffix_len;
		ft_meta_nr_child_set(sfx_meta, 1);
		ft_nr_keys_store(ft,sfx_meta, child_nr_keys,
			CMM_RELAXED);
		suffix_flag = ft_compressed_node_flag(sfx);	/* PLAIN: install + recover sfx directly */
		sfx_skip_flag = ft_publish_compressed(ft, sfx, suffix_flag);	/* skip form for the slot */
		created[nr_created++] = suffix_flag;	/* track PLAIN so the error path frees sfx directly */
		/*
		 * Defer re-parenting the live old child into the new suffix.
		 * Writing cn->child's back-pointer now would expose the
		 * unpublished cluster from below.  The junction install
		 * below would also recover sfx through it.  sfx->child
		 * already points at cn->child; that is a write into the new
		 * sfx only.
		 */
		deferred_child = old_child_flag;
		deferred_parent = suffix_flag;	/* PLAIN sfx flag */
		deferred_slot = &sfx->child;
	} else if (suffix_len == 1) {
		struct cds_ft_inode_flag *dest = NULL;

		/*
		 * 1-child internal suffix (non-SC).  The live cn->child is its
		 * only child, so this node is a cluster-leaf.  Defer
		 * cn->child's back-pointer.
		 */
		ret = ft_node_set_nth(ft, &dest,
			cn->key_bytes[remaining + 1],
			old_child_flag, NULL, NULL,
			node_depth + remaining + 1, true);
		if (ret) goto error;
		{
			struct cds_ft_metadata *m =
				cds_ft_item_to_metadata(ft_node_ptr(dest));
			ft_nr_keys_store(ft,m, child_nr_keys,
				CMM_RELAXED);
		}
		suffix_flag = dest;
		created[nr_created++] = dest;
		sp_sfx_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
		sp_sfx_span = 1;
		deferred_child = old_child_flag;
		deferred_parent = dest;
		ft_node_get_nth_skip(dest, &deferred_slot,
			cn->key_bytes[remaining + 1], FT_PF_NONE);
	} else {
		suffix_flag = old_child_flag;
	}

	/* Junction: internal node with suffix child. */
	{
		struct cds_ft_inode_flag *dest = NULL;
		struct cds_ft_metadata *jct_meta;

		ret = ft_node_set_nth(ft, &dest,
			jct_ordinal,
			suffix_flag, NULL, NULL,
			node_depth + remaining, jct_cluster_leaf);
		if (ret) goto error;
		jct_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
		sp_jct_meta = jct_meta;
		/*
		 * I4 count fold.  The junction holds the NEW key as its
		 * external_nodes; the caller attaches it before publish.  So
		 * build the junction with its FULL post-commit count: the
		 * suffix subtree (child_nr_keys) PLUS that new key.  The bare
		 * suffix count used to rely on a post-commit +1 walk from the
		 * junction.  The junction and the prefix below are both
		 * built full, so the fresh cluster is off the count walk.
		 * The +1 rides the commit as edges on the STABLE ancestors
		 * from d->pnf up.
		 */
		ft_nr_keys_store(ft,jct_meta, child_nr_keys + 1,
			CMM_RELAXED);
		jct_flag = dest;
		created[nr_created++] = dest;

		if (jct_cluster_leaf) {
			/*
			 * suffix_len == 0: the junction is the cluster-leaf and
			 * its suffix-direction child is the live cn->child.
			 * Record the deferred edge against the final junction.
			 */
			deferred_child = old_child_flag;
			deferred_parent = jct_flag;
			ft_node_get_nth_skip(jct_flag, &deferred_slot,
				jct_ordinal, FT_PF_NONE);
		} else if (sfx_skip_flag && sfx_skip_flag != suffix_flag) {
			/*
			 * suffix_len >= 1 compressed.  sfx was installed via
			 * its PLAIN flag, so ft_set_parent recovered it
			 * directly.  Re-encode the junction's slot to sfx's
			 * skip form.
			 * This is a value write into the junction, which is
			 * still unpublished.  The slot resolves once
			 * cn->child's back-pointer is set at the tail.
			 */
			struct cds_ft_inode_flag **oslot = NULL;

			ft_node_get_nth_skip(jct_flag, &oslot, jct_ordinal,
				FT_PF_NONE);
			if (oslot)
				*oslot = sfx_skip_flag;
		}
	}

	/* Prefix -> junction (no external_nodes on @cn: asserted at entry). */
	if (remaining >= 2) {
		struct cds_ft_compressed_node *pfx;
		struct cds_ft_metadata *pfx_meta;

		pfx = alloc_compressed_node(ft, remaining, &pfx_meta);
		if (!pfx) goto error;
		sp_pfx_meta = pfx_meta;
		sp_pfx_span = remaining;
		pfx->child = jct_flag;
		pfx->len = remaining;
		memcpy(pfx->key_bytes, cn->key_bytes, remaining);
		ft_meta_nr_child_set(pfx_meta, 1);
		ft_nr_keys_store(ft,pfx_meta, ft_nr_keys_get(cn_meta) + 1,
			CMM_RELAXED);
		top_flag = ft_compressed_node_flag(pfx);
		/* HIDDEN: fresh junction under this build's fresh prefix. */
		ft_set_parent_excl(ft, jct_flag, top_flag, NULL,
			FT_EXCL_HIDDEN);
		top_flag = ft_publish_compressed(ft, pfx, top_flag);
		created[nr_created++] = top_flag;
	} else if (remaining == 1) {
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_group_skip_compressed(ft->group)) {
			/*
			 * 1-byte prefix: emit a 1-byte compressed instead of a
			 * 1-child internal (canonical form under
			 * SKIP_COMPRESSED).
			 */
			struct cds_ft_compressed_node *pfx;
			struct cds_ft_metadata *pfx_meta;

			pfx = alloc_compressed_node(ft, 1, &pfx_meta);
			if (!pfx) goto error;
			sp_pfx_meta = pfx_meta;
			sp_pfx_span = 1;
			pfx->child = jct_flag;
			pfx->len = 1;
			pfx->key_bytes[0] = cn->key_bytes[0];
			ft_meta_nr_child_set(pfx_meta, 1);
			ft_nr_keys_store(ft,pfx_meta, ft_nr_keys_get(cn_meta) + 1,
				CMM_RELAXED);
			top_flag = ft_compressed_node_flag(pfx);
			/* HIDDEN: fresh junction under a prefix allocated here. */
			ft_set_parent_excl(ft, jct_flag, top_flag, &pfx->child,
				FT_EXCL_HIDDEN);
			top_flag = ft_publish_compressed(ft, pfx, top_flag);
			created[nr_created++] = top_flag;
		} else
#endif
		{
			struct cds_ft_inode_flag *dest = NULL;
			struct cds_ft_metadata *pfx_meta;

			ret = ft_node_set_nth(ft, &dest, cn->key_bytes[0],
				jct_flag, NULL, NULL, node_depth, false);
			if (ret) goto error;
			pfx_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
			sp_pfx_meta = pfx_meta;
			sp_pfx_span = 1;
			ft_nr_keys_store(ft,pfx_meta, ft_nr_keys_get(cn_meta) + 1,
				CMM_RELAXED);
			top_flag = dest;
			created[nr_created++] = dest;
		}
	} else {
		/* remaining == 0: no prefix, junction IS the top. */
		top_flag = jct_flag;
	}

	/*
	 * Failure-free tail.  No allocation happens past here.  The caller
	 * publishes the top forward immediately after we return.  The order
	 * is:
	 *  1. wire top_flag's own back-pointer into cn's live parent;
	 *  2. THEN wire the single deferred back-pointer (the live old child
	 *     into the new suffix / junction).
	 * An up-walk can enter the cluster via the deferred back-channel
	 * below.  With this order it finds a parent-wired top.
	 *
	 * Wire top_flag's back-pointer from ONE resolved (parent, slot)
	 * snapshot (§9 / see ft_split_compressed_insert).  A peer re-home
	 * commits cn_meta->parent and its slot offset as one MCAS pair.  So a
	 * raw re-read of the parent could pair a re-homed parent P2 with
	 * @parent_slot, which still indexes the old parent P1.  That would
	 * fault ft_slot_to_byte with an out-of-range rank against P2's
	 * bitmap.
	 *
	 * Only wire when the resolved slot still IS @parent_slot.  That is the
	 * consistent, non-re-homed pair, with any flip proxy already resolved.
	 * A re-home makes the pair disagree.  Then:
	 *  - the wiring is skipped, and the cluster stays build-invisible;
	 *  - the caller's post-build (parent, slot) guard unwinds the whole
	 *    insert with -EAGAIN before any publish.
	 * A re-home AFTER that guard is caught by the forward CAS / §4.B as
	 * ABORT.
	 */
	{
		struct cds_ft_inode_flag *cur_parent;

		if (ft_resolve_parent_slot(cn_meta, ft, &cur_parent) == parent_slot)
			/* HIDDEN: @top_flag is this build's cluster top; the note
			 * above says a mismatch leaves "the cluster ...
			 * build-invisible". */
			ft_set_parent_excl(ft, top_flag, cur_parent,
				parent_slot, FT_EXCL_HIDDEN);
	}
	/* Return the live edge; the caller defers (parked) or wires (direct). */
	*live_child_ret = deferred_child;
	*live_parent_ret = deferred_parent;
	*live_slot_ret = deferred_slot;
	/* The fresh spine, top first: [prefix] junction [suffix]. */
	*nr_born_ret = 0;
	if (deferred_child) {
		if (sp_pfx_meta) {
			born_ret[*nr_born_ret] = (struct ft_born_spine_node){
				sp_pfx_meta, node_depth, sp_pfx_span };
			(*nr_born_ret)++;
		}
		born_ret[*nr_born_ret] = (struct ft_born_spine_node){
			sp_jct_meta, node_depth + remaining, 1 };
		(*nr_born_ret)++;
		if (deferred_parent != jct_flag) {
			born_ret[*nr_born_ret] = (struct ft_born_spine_node){
				sp_sfx_meta, node_depth + remaining + 1,
				sp_sfx_span };
			(*nr_born_ret)++;
		}
	}
	/*
	 * Hand the built cluster back; it is still unpublished.  The caller
	 * can then free it if it must abort after we return (a one-commit arm
	 * -ENOMEM).  created[] holds exactly the fresh cluster nodes, never
	 * the live cn->child.
	 */
	if (created_ret) {
		memcpy(created_ret, created,
			(size_t) nr_created * sizeof(created[0]));
		*nr_created_ret = nr_created;
	}

	FT_TP(compressed_split, "key_shorter", (const void *) cn, cn->len,
		(const void *) top_flag, remaining);
	*top_ret = top_flag;
	*jct_ret = jct_flag;
	return 0;

error:
	ft_free_unpublished_split_cluster(ft, created, nr_created);
	return -ENOMEM;
}

/*
 * We reached an unpopulated node. Create it and the children we need,
 * and then attach the entire branch to the current node. This may
 * trigger recompaction of the current node.
 *
 * ft_attach_node() ensures that a lookup will _never_ see a branch that
 * leads to a dead-end. It first populates the entire content of the new
 * branch, which creates a cluster. Only then does it attach the cluster
 * to the rest of the trie, which makes it visible to lookups.
 *
 * @external_node argument is either NULL or a pointer to the external
 * node we are replacing at the attachment location. In that case, we
 * need to chain this external node in the external node list of the
 * topmost internal node.
 */
static
int ft_attach_node(struct cds_ft *ft,
		struct cds_ft_inode_flag **attach_node_flag_ptr,
		struct cds_ft_inode_flag *attach_node_flag,
		struct cds_ft_inode_flag **old_node_flag_ptr,
		struct cds_ft_inode_flag *old_node_flag,
		const uint8_t *key,
		size_t key_len,
		unsigned int level,
		struct cds_ft_node *child_node,
		struct cds_ft_node *external_nodes,
		struct ft_insert_commit *ic,
		const struct ft_lock_ctx *ctx)
{
	struct cds_ft_metadata *metadata = NULL;
	struct cds_ft_inode_flag *iter_node_flag, *iter_dest_node_flag,
				*created_nodes[FT_MAX_DEPTH];
	struct cds_ft_inode *old_recompacted_node = NULL;
	/* Nested held-set frame for the reserve; see its use below. */
	struct ft_lock_ctx rctx;
	int ret, i, nr_created_nodes = 0;
	const uint8_t *iter_key = key + key_len;

	FT_TP(attach_node_enter, (const void *) attach_node_flag,
		(const void *) old_node_flag, level);

	dbg_printf("Attach node at level %u (old_node_flag %p, attach_node_flag_ptr %p attach_node_flag %p)\n",
		level, old_node_flag, attach_node_flag_ptr, attach_node_flag);

	assert(!old_node_flag || external_nodes);
	assert(level > 0);	/* Root is always internal; level 0 is handled directly. */
	if (attach_node_flag)
		metadata = cds_ft_item_to_metadata(ft_node_ptr(attach_node_flag));

	/*
	 * Concurrent-writer conflict detection.  This was a single-writer
	 * "concurrent update prevented by mutual exclusion" assert, which is
	 * false under MW.  A peer mutated the old / attach slot between this
	 * writer's descent and here:
	 *  - the old slot now holds a live node with nowhere to chain it; or
	 *  - the attach slot no longer matches the descent-captured node.  A
	 *    peer's flip proxy also reads != the captured node here, so it
	 *    too routes to retry.
	 * Bail to the from-root restart_attempt rather than build against a
	 * stale slot.  Nothing is armed or built yet (nr_created_nodes == 0),
	 * so check_error is a clean unwind.  The commit's forward-CAS + parent
	 * guard would surface the same conflict as ABORT.  Failing fast here
	 * just skips a doomed build.  No-op under retained exclusion.
	 */
	if ((old_node_flag_ptr && ft_node_ptr_raw(*old_node_flag_ptr) && !external_nodes) ||
			(attach_node_flag_ptr && ft_node_ptr_raw(*attach_node_flag_ptr) !=
				ft_node_ptr_raw(attach_node_flag))) {
		ret = -EAGAIN;
		goto check_error;
	}

	/* Create new branch, starting from bottom */
	iter_node_flag = (struct cds_ft_inode_flag *) child_node;

	{
		struct cds_ft_inode_flag *compressed;
		/*
		 * Compressed nodes must not carry metadata->external_nodes.
		 * When external_nodes exist at @level, compress from
		 * level+1 (one byte shorter).  The loop below then
		 * creates an internal node at @level that holds the
		 * external_nodes.
		 */
		unsigned int compress_level = external_nodes ? level + 1 : level;

		compressed = ft_try_compress_chain(ft, key, key_len,
			compress_level, iter_node_flag, NULL, NULL);
		if (compressed == (void *) (long) -ENOMEM) {
			ret = -ENOMEM;
			goto check_error;
		}
		if (compressed) {
			iter_node_flag = compressed;
			/*
			 * Track the PLAIN compressed flag.  Never track the
			 * skip form that ft_try_compress_chain returns in
			 * skip-compressed groups.  A SKIP pointer encodes the
			 * CHILD's address; for a leaf attach that is the
			 * application's external node.  The kind dispatch in
			 * check_error's unwind would misread it as a plain
			 * node, and run arena arithmetic on the application's
			 * pointer.  Same convention as the glue builders (see
			 * ft_try_compress_chain's glue arm).
			 */
			created_nodes[nr_created_nodes++] =
				ft_node_skip_compressed(compressed) ?
				ft_compressed_node_flag(
					ft_skip_to_compressed(ft, compressed)) :
				compressed;
			iter_key = key + compress_level;
			/*
			 * When external_nodes exist, compress_level = level + 1.
			 * iter_key points to key + level + 1.  The loop below
			 * runs one iteration to create an internal node at
			 * @level that dispatches on key[level] with the
			 * compressed node as child.  The loop then places
			 * external_nodes on this internal node.
			 */
		}
	}
	if ((!ft_node_compressed(iter_node_flag) &&
	     !ft_node_skip_compressed(iter_node_flag)) ||
	    external_nodes) {
		for (i = (ft_node_compressed(iter_node_flag) ||
			  ft_node_skip_compressed(iter_node_flag)) ?
				(int) level + 1 : (int) key_len;
		     i > (int) level; i--) {
			uint8_t key_value;

			key_value = *(--iter_key);
			dbg_printf("branch creation level %d, key %u\n",
					i, (unsigned int) key_value);
			iter_dest_node_flag = NULL;
			ret = ft_node_set_nth(ft, &iter_dest_node_flag, key_value, iter_node_flag, NULL, NULL,
					i - 1, false);
			if (ret) {
				dbg_printf("branch creation error %d\n", ret);
				goto check_error;
			}
			{
				struct cds_ft_metadata *branch_meta =
					cds_ft_item_to_metadata(ft_node_ptr(iter_dest_node_flag));
				ft_nr_keys_store(ft,branch_meta, 1, CMM_RELAXED);
			}
			created_nodes[nr_created_nodes++] = iter_dest_node_flag;
			iter_node_flag = iter_dest_node_flag;
		}

		if (external_nodes) {
			struct cds_ft_metadata *iter_node_metadata;

			iter_node_metadata = cds_ft_item_to_metadata(ft_node_ptr(iter_node_flag));
			/*
			 * Phase 1 (build-invisible): write the cluster top's
			 * cluster-internal external_nodes pointer.  The
			 * back-channel publish (external_nodes->prev =
			 * iter_node_flag) is deferred to Phase 2 below, after
			 * set_nth wires iter_node_flag's parent.
			 * external_nodes is still reachable through the old
			 * slot at attach_node_flag_ptr.  Without the deferral,
			 * an up-walk from it lands on iter_node_flag with
			 * parent == NULL.
			 */
			ft_metadata_set_external_nodes(iter_node_flag,
				iter_node_metadata, external_nodes);
			ft_nr_keys_store(ft,iter_node_metadata,
				ft_nr_keys_get(iter_node_metadata) + 1, CMM_RELAXED);
		}
	}

	/* Publish branch. */
	{
		uint8_t key_value;
		/*
		 * Accumulates the one-commit forward edges for a reserve that
		 * relocates by recompaction:
		 *  - the reserve set_nth records a compressed parent's SKIP_X
		 *    dual here;
		 *  - the forward fold below adds the grandparent slot edge.
		 * So both flip ATOMICALLY in ic->txn.
		 */
		struct ft_pub_rec rec = { .ctx = ctx, .n = 0 };

		key_value = *(--iter_key);
		dbg_printf("publish branch at level %d, key %u\n", level - 1, (unsigned int) key_value);

		/*
		 * Reserve the I6 count-fold edges optimistically.  In the
		 * common case this attach stays in place
		 * (iter_dest_node_flag == attach_node_flag below).  Then the +1
		 * key-count walk from the stable attach node up to the root
		 * rides this txn.  The bound is the ACTUAL descent depth, never
		 * FT_MAX_DEPTH.  @level >= the attach node's depth, and +2 is
		 * slack for the root.  A relocation (I7) records no count
		 * edges here (count_folded stays false) so the slack is simply
		 * unused.  A no-op reserve (0) when order statistics are off.
		 */
		ret = ft_insert_commit_arm(ft, ic,
			ft->rank_stats ? level + 2 : 0);
		if (ret)
			goto check_error;

		/* We need to use set_nth on the previous level. */
		iter_dest_node_flag = attach_node_flag;
		/*
		 * Every ft_attach_node caller passes a non-NULL @ic.
		 * ft_insert_commit_arm has armed ic->txn above; an arm failure
		 * jumps to check_error.  So the one-commit txn is always
		 * present here, and the reserved-byte publish below is
		 * unconditional.
		 */
		assert(ic && ic->txn);
		{
			struct cds_ft_inode_flag **slot_ptr = NULL;
			/*
			 * The reserve below sets this iff it added the byte's
			 * occupancy IN PLACE on the LIVE attach node.  This op
			 * then still owes that node an nr_child++.  A reserve
			 * that RELOCATED (recompact) built the count into its
			 * fresh copy instead, and leaves this false.
			 */
			bool count_deferred = false;
			/*
			 * May the reserve below write the LIVE attach node IN
			 * PLACE?  Only once this op HOLDS it -- see the hoist.
			 */
			bool in_place = false;

			/*
			 * ★ THE OP'S HELD SET, MADE VISIBLE TO THE RESERVE.
			 *
			 * The caller built @ctx with
			 *	ft_lock_ctx_init(&actx, &d, ic.txn, ...)
			 * BEFORE ft_insert_commit_arm created the txn above.
			 * So @ctx->held.txn is NULL, and the commit's registry
			 * is invisible through @ctx.
			 *
			 * The reserve hands its ctx to ft_node_recompact, whose
			 * lock-set acquire dedupes via ft_lock_ctx_holds.  Take
			 * a word THIS op registered in ic->txn: the hoisted
			 * attach-node lock, next.  Through @ctx alone:
			 *  - the word would read as "held by someone else";
			 *  - ft_dlm_lock would refuse it;
			 *  - the op would spend an -EAGAIN refusing its OWN
			 *    lock;
			 *  - the op would destroy the txn, re-descend and spin
			 *    for ever.
			 * Measured at @70a1e20f: unit row 160 / inv row 80,
			 * 101% CPU.
			 *
			 * Chain a nested frame rather than mutate @ctx: @ctx is
			 * const, and the caller's frame is still live.
			 * @held.outer is exactly the documented "CALLER's held
			 * set, when this one belongs to a nested step of the
			 * same op".  ft_held_set_snap / ft_dlm_acquire_set
			 * already walk it.  So the reserve sees the commit
			 * registry AND everything the caller held, as ONE held
			 * set.
			 */
			ft_lock_ctx_init(&rctx, ft_lock_ctx_descent(ctx), ic->txn,
				ctx ? ctx->op : NULL);
			rctx.held.outer = ctx ? &ctx->held : NULL;

			/*
			 * ★ LOCK BEFORE WRITE: the in-place reserve's acquire.
			 *
			 * An in-place reserve mutates @attach_node_flag's LIVE
			 * body: a bitmap bit plus a child slot.  These are RAW
			 * stores that no commit can arbitrate or roll back.  So
			 * the op must hold that node BEFORE the store.  This
			 * acquire used to sit beside the count edge, ~80 lines
			 * down.  Measured there with all three witnesses: held
			 * 0 of 4,040,923 at the reserve.  Take it FIRST.
			 *
			 *  - FINE strategy: the node's own DLM lock (per-node
			 *    spacing) or its ANCHOR (exponential / root-only).
			 *    It is REGISTERED in ic->txn with its
			 *    {LOCK|s -> s} release.  So the commit that
			 *    publishes the edge is the one that drops the lock.
			 *    A MISS (a peer holds it) bails -EAGAIN right here,
			 *    BEFORE anything is written.  The commit would only
			 *    abort later.  By then the raw store would already
			 *    have landed unlocked.
			 *  - COARSE strategy: the FT-wide writer lock this op
			 *    already holds is the exclusion.  Nothing per-node
			 *    is taken.  The §4.B guard below keeps its old
			 *    place.
			 *
			 * ☞ THE RELOCATION ARM IS WHY @rctx EXISTS.  The
			 * reserve cannot always stay in place (-ERANGE non-tail
			 * byte, -ENOSPC).  Then ft_node_recompact acquires
			 * {C, P, GP} with C = this very node.  Through @rctx
			 * that dedupes SHARED.  The fused arm of
			 * ft_flip_txn_record_retire_anchored then does one of
			 * two things:
			 *  - at per-node spacing, it chains C's tombstone onto
			 *    the release recorded here:
			 *    {LOCK|s -> s} + {s -> TOMBSTONE|s} = one
			 *    {LOCK|s -> TOMBSTONE|s} record;
			 *  - at a coarser spacing, it tombstones C's own word
			 *    beside the anchor's release.
			 * So the hoisted lock is either the in-place store's
			 * protection or the retire's fence.  It is never a
			 * second acquire on a word this op holds.
			 *
			 * ☞ ONE ACQUIRE, NOT TWO.  The later lock_or_guard that
			 * this acquire replaces is GONE for the fine strategy.
			 * When it was kept, its SHARED exit at a coarse spacing
			 * planted an MW {s -> s} guard on C's OWN word.  The SW
			 * nr_child++ that follows sits beside it: one slot, two
			 * kinds (@898c08ce's control).  The guard only ever
			 * stood for two things, and both remain covered:
			 *  - the count edge's own expected-old validates C's
			 *    word at commit;
			 *  - the anchor excludes every point-op peer of C.
			 *
			 * @ctx, NOT @rctx, is handed to lock_or_guard.
			 * lock_or_guard builds its own lctx with
			 * .held.txn = ic->txn, and copies .extra/.glue/.outer
			 * from what it is given.  So @ctx already carries every
			 * frame, and @rctx would only add this txn a second
			 * time.
			 */
			if (!old_node_flag && ft_in_place_insert_ok(ft)) {
				/*
				 * THE ACQUIRE SEAM (-DFT_DELAY_INJECT,
				 * FT_DELAY_MODE=acquire).  @old_node_flag and the
				 * descent that chose this attach node were
				 * sampled BEFORE the lock taken just below.
				 * Everything the reserve then writes in place
				 * stands on that sample.  Injecting here lets a
				 * peer invalidate the sample while this op is
				 * between the two.  This is the insert tier's
				 * counterpart to the seam in
				 * ft_node_recompact.  Inert unless asked for.
				 */
				ft_delay_seam(FT_DELAY_SITE_INSERT);
				if (ft->lock_fine) {
					enum ft_lock_or_guard_exit ex;

					ft_flip_txn_lock_or_guard_parent_ex(
						__func__, __LINE__, ft, ic->txn,
						ctx, attach_node_flag,
						FT_DEPTH_FROM_DESCENT, &ex);
					if (ex == FT_LOG_EXIT_MISS) {
						ret = -EAGAIN;
						goto check_error;
					}
					/* REGISTERED, or SHARED through the op's own held set. */
					in_place = true;
				} else {
					in_place = true;	/* the FT-wide lock */
				}
			} else if (old_node_flag && ft->lock_fine) {
				/*
				 * THE OCCUPIED SLOT TAKES THE SAME LOCK
				 * (doc/design/ft-lockset-inventory.md §1, row 3).
				 *
				 * The slot already holds the displaced external
				 * head.  So there is no reserve, and the hoist
				 * above never ran.  Yet the record below still
				 * rewrites a child slot of the LIVE attach
				 * node.  Until now this op recorded and
				 * committed it holding nothing but the
				 * grandparent it acquires later (~13k commits
				 * per ft_inv run, every spacing).  The attach
				 * node is the slot's owner.  So it is acquired
				 * here, before the record, whatever the
				 * in-place feature says.  No store happens in
				 * place on this path; the lock is owed by the
				 * record.
				 *
				 * Derive, acquire, RE-VALIDATE.  The descent
				 * sampled @old_node_flag with nothing held.  So
				 * re-read the slot under the lock, and
				 * re-descend if a peer replaced the head in
				 * between.  A registered lock is released by
				 * check_error's destroy of @ic->txn.
				 */
				enum ft_lock_or_guard_exit ex;

				ft_delay_seam(FT_DELAY_SITE_INSERT);
				ft_flip_txn_lock_or_guard_parent_ex(__func__,
					__LINE__, ft, ic->txn, ctx,
					attach_node_flag, FT_DEPTH_FROM_DESCENT,
					&ex);
				if (ex == FT_LOG_EXIT_MISS) {
					ret = -EAGAIN;
					goto check_error;
				}
				if (!old_node_flag_ptr ||
						ft_node_ptr_raw(rcu_dereference(
						*old_node_flag_ptr)) !=
						ft_node_ptr_raw(old_node_flag)) {
					ret = -EAGAIN;
					goto check_error;
				}
			}

			/*
			 * One-commit insert (reserved-byte model).  Occupy
			 * key_value's slot, but leave it resolving to its OLD
			 * value.  Then RECORD the slot edge (old -> the fresh
			 * top) into ic->txn.  The edge then settles atomically
			 * with the ordered-list edges at insert_done.  Nothing
			 * foreign is stored in the slot during the build.  The
			 * fresh key stays invisible because the slot still
			 * reads its old value: NULL for a new byte, or the
			 * displaced external.
			 *
			 *  - new byte (old_node_flag == NULL): RESERVE it via a
			 *    set_nth with a NULL child.  This sets the bitmap
			 *    bit and leaves a bit-set+NULL slot, which reads as
			 *    not-present.  It may recompact + relocate the
			 *    node; the reserved byte rides along, and the edge
			 *    is recorded against the FINAL slot below.
			 *    Its nr_child++ is NOT applied in place.  An
			 *    in-place reserve hands it back via
			 *    @count_deferred, and it is recorded into ic->txn
			 *    below.  So it goes live with the publish, and is
			 *    discarded with an aborted attempt (see
			 *    ft_flip_txn_record_nr_child_inc).
			 *    An abort does NOT roll back the bitmap bit itself.
			 *    A bit-set+NULL slot is the same hole a soft-delete
			 *    leaves.  It reads as not-present, and the retry
			 *    refills it in place (Case 1 refill).
			 *  - displaced external (old_node_flag != NULL): the slot
			 *    already holds it; no set_nth (a NULL store would drop
			 *    the live external before the commit).
			 *
			 * Then fully wire the fresh top NOW: parent, slot
			 * offset, incoming_byte.  It is invisible while the
			 * slot reads old.  The splice-position search at
			 * insert_done can then rebuild the new key through the
			 * parent chain.
			 */
			if (!old_node_flag) {
				ret = ft_node_set_nth_rec(ft, &iter_dest_node_flag,
					key_value, NULL, &old_recompacted_node,
					metadata, level - 1, false, &rec, ic->txn,
					NULL, &rctx, in_place, &count_deferred);
				if (ret) {
					dbg_printf("branch publish error %d\n", ret);
					goto check_error;
				}
#ifdef FT_DEBUG_INPLACE_DISCARD
				/*
				 * RED-STYLE INJECTION, never shipping.  Discard
				 * 1 in 64 attempts whose reserve landed IN
				 * PLACE, AFTER the raw bitmap/slot stores.  A
				 * later missed take or a commit abort does the
				 * same.  The count rides the txn and is
				 * dropped.  The bit stays over an empty slot,
				 * i.e. a soft-deleted HOLE.  The node format
				 * already carries such holes: the refill arm,
				 * verify counting non-NULL slots, capacity by
				 * popcount.  The in-place doc's §5 question,
				 * made measurable.
				 */
				if (iter_dest_node_flag == attach_node_flag &&
						in_place && ic->txn) {
					static unsigned long ipd_n;

					if (!(uatomic_add_return(&ipd_n, 1) & 63)) {
						ic->txn->acquire_miss = true;
						uatomic_inc(&ft_ipd_injected);
					}
				}
#endif
#ifdef FT_DEBUG_INPLACE_HOIST
				/*
				 * The in-place / relocation split of the reserve,
				 * on the config where in-place can actually fire.
				 */
				if (iter_dest_node_flag == attach_node_flag)
					__atomic_fetch_add(&ft_ip_reserve_inplace,
						1, __ATOMIC_RELAXED);
				else
					__atomic_fetch_add(&ft_ip_reserve_reloc,
						1, __ATOMIC_RELAXED);
#endif
			}
			/*
			 * The in-place reserve owes @metadata (the LIVE attach
			 * node) its nr_child++.  Record it as an edge in
			 * ic->txn, immediately after the §4.B guard below and
			 * on the SAME state word.  The two then chain into ONE
			 * record, which the arm's guard reservation already
			 * covers.  Deferring is what makes reserve-then-ABORT
			 * idempotent.  The count is dropped with the attempt.
			 * It does not leak a phantom child that the retry
			 * double-counts.
			 */
			assert(!count_deferred ||
				iter_dest_node_flag == attach_node_flag);
			ft_node_get_nth_skip(iter_dest_node_flag, &slot_ptr,
				key_value, FT_PF_NONE);
			assert(slot_ptr);
			/*
			 * HIDDEN: @iter_node_flag is the cluster this function
			 * builds bottom-up from the caller's unpublished
			 * @child_node.  Phase 2 below calls it "the fresh
			 * cluster top".  The LIVE displaced external head is
			 * the OTHER edge.  It is parked (@ic->live_child), not
			 * stored here.
			 */
			ft_set_parent_excl(ft, iter_node_flag,
				iter_dest_node_flag, slot_ptr,
				FT_EXCL_HIDDEN);
			/*
			 * §4.B VALIDATE (Phase 4.3, MW), COARSE strategy only.
			 * The reserved-byte edge below stores @slot_ptr, a slot
			 * INSIDE @iter_dest_node_flag.  When the reserve stayed
			 * IN PLACE (iter_dest == attach node), that slot lives
			 * in the LIVE, reader-reachable attach node.  Yet the
			 * forward CAS below validates only the slot VALUE (NULL
			 * for a new byte).  Without a guard:
			 *  - a peer recompacts/relocates the attach node
			 *    through its grandparent slot;
			 *  - that leaves the slot value intact in the retired
			 *    copy, so the CAS still matches;
			 *  - the CAS commits the new key into a reclaimed node
			 *    (UAF + lost insert).
			 * So guard the holder's state word.  A peer's
			 * freeze-on-free (LOCK/TOMBSTONE) between this descent
			 * and the commit then ABORTs instead.  This is
			 * symmetric with the split path
			 * (ft_insert_publish_or_park) and the external-clear
			 * guards in ft-remove.h.
			 *
			 * Under the FINE strategy the attach node is this op's
			 * lock-set member.  It was ACQUIRED above the reserve
			 * (the hoist), which records the {LOCK|s -> s} release.
			 * This guard would only approximate that release.  A
			 * guard here on top of it is the mixed-kind shape that
			 * the hoist's comment names.  A RELOCATION reserve
			 * publishes a build-invisible fresh copy.  That copy is
			 * guarded at its grandparent slot below
			 * (iter_dest != attach), so it needs nothing here
			 * either.
			 *
			 * ORDER IS LOAD-BEARING.  The hoist's release (fine) or
			 * this guard (coarse) is recorded BEFORE
			 * @count_deferred's nr_child++ edge, since both target
			 * this one state word.
			 *  - release-then-count chains to a single
			 *    {LOCK|s -> s+1}: release AND increment in one
			 *    record.
			 *  - count-then-release would arrive with expected old
			 *    LOCK|s against a pending s+1, and POISON the
			 *    descriptor.
			 * The ordering rule is spelled out at
			 * ft_flip_txn_record_release_lock.
			 */
			if (iter_dest_node_flag == attach_node_flag && !ft->lock_fine)
				ft_flip_txn_guard_parent(ft, ic->txn,
					iter_dest_node_flag);
			if (count_deferred)
				ft_flip_txn_record_nr_child_inc(ic->txn, metadata);
			/*
			 * @slot_ptr is a child slot inside
			 * @iter_dest_node_flag, whose @metadata is the node the
			 * two lines above lock-or-guard and count against.
			 */
			if (iter_dest_node_flag != attach_node_flag) {
				/*
				 * ☞ A HIDDEN BODY IS WRITTEN PLAIN (item 4).
				 * The destination is the copy this op just
				 * built by relocation.  No reader can reach it
				 * until the forward publish below flips
				 * @attach_node_flag_ptr onto it.  That publish
				 * IS this slot's release.  No peer can write
				 * the copy, because no peer has its address.
				 * An abort frees the body unpublished, so there
				 * is nothing to roll back either.  Recording
				 * the store cost one descriptor entry per
				 * relocating insert and bought nothing.
				 * MEASURED at 673468 of ft_unit's fills and
				 * 98% of them (-DFT_DEBUG_HIDDEN_FILL).
				 *
				 * The IN-PLACE arm below is the opposite case:
				 * a slot in the LIVE node.  There the record's
				 * expected-old is what rejects a peer, so that
				 * arm keeps its record.
				 */
				*slot_ptr = iter_node_flag;
			} else {
				ft_flip_txn_record_reserved(ic->txn,
					/*owner=*/ metadata, (void **) slot_ptr,
					(void *) old_node_flag,
					(void *) iter_node_flag);
			}
			ic->slot = slot_ptr;
#ifdef FT_DEBUG_DEL_TOMB
			if (iter_dest_node_flag == attach_node_flag)
				ft_dt_check_flag("ins-reserve-inplace",
					count_deferred ? 0 : 1, iter_dest_node_flag,
					key_value, (const void *) attach_node_flag);
			else
				ft_dt_check_flag("ins-reserve-reloc", 1,
					iter_dest_node_flag, key_value,
					(const void *) attach_node_flag);
			ft_dt_last_reserve_nf = iter_dest_node_flag;
#endif
			FT_TP(tree_edge_set, (const void *) ft,
				(const void *) iter_dest_node_flag,
				(unsigned int) (level - 1), (uint8_t) key_value,
				(const void *) iter_node_flag);
		}
		/*
		 * Phase 2: iter_node_flag's parent is now wired.  This was
		 * done by ft_node_set_nth above (either in-place or via
		 * recompact's reparent loop), or by the explicit raw store
		 * for a one-commit insert.  Re-parent the LIVE displaced
		 * external head onto the fresh cluster top iter_node_flag.
		 *
		 * external_nodes is reader-reachable through its old slot
		 * until the forward publish.  So an up-walk would follow its
		 * prev / cell->parent INTO the not-yet-published cluster,
		 * and from there its build-invisible internals.  So this
		 * re-parent must flip ATOMICALLY with the forward publish
		 * below, not before it.  Park it into the one-commit
		 * (ic->live_child).  ft_insert_one_commit replays it via
		 * ft_park_live_parent_edge, which resolves the external
		 * head's cell->parent / prev.  The cluster then becomes
		 * reachable via BOTH its forward slot and this back-pointer
		 * in one flip.
		 */
		if (external_nodes) {
			/* ic->txn is always armed here (see the assert above). */
			ic->live_child =
				(struct cds_ft_inode_flag *) external_nodes;
			ic->live_parent = iter_node_flag;
			ic->live_slot = NULL;
			/* Fresh spine: the cluster top alone, at @level. */
			ic->nr_born = 0;
			ft_ic_born_add(ic, cds_ft_item_to_metadata(
					ft_node_ptr(iter_node_flag)), level, 1);
			FT_BE_SRC_SET(ic, FT_BE_PARK_ATTACH);
		}
		/* Attach branch (unlink the old node from the trie).
		 * ft_publish_to_parent handles skip pointer update
		 * if the attach target is a compressed node's child.
		 */
		if (iter_dest_node_flag != attach_node_flag) {
			struct cds_ft_metadata *idest_meta =
				cds_ft_item_to_metadata(
					ft_node_ptr(iter_dest_node_flag));

			/*
			 * Phase 4.3 atomic re-home.  The fresh copy inherited
			 * the attach node's (parent, offset) as a CONSISTENT
			 * snapshot (ft_node_recompact).  A peer may have
			 * re-homed the attach node since this op's descent, by
			 * recompacting its grandparent.  Then the inherited
			 * slot no longer matches the descent-captured
			 * @attach_node_flag_ptr.  Publishing @iter_dest at the
			 * stale slot would invert it against the fresh copy's
			 * NEW parent and fault ft_slot_to_byte (the dominant
			 * FT_INV_MW crash).  Re-descend rather than publish a
			 * torn (parent, slot) pair.
			 *
			 * ft_get_parent_slot is consistent now (A.1
			 * pair-resolver + A.2 atomic reparent).  So this check
			 * can no longer pass spuriously on a torn
			 * back-pointer.  No-op under exclusion.
			 *
			 * @iter_dest is a successfully-built fresh copy that is
			 * NOT tracked in created_nodes[].  So free it here,
			 * before the shared unwind.
			 */
			if (ft_get_parent_slot(idest_meta, ft) != attach_node_flag_ptr) {
				free_cds_ft_node_unpublished(ft,
					ft_node_ptr(iter_dest_node_flag));
				ret = -EAGAIN;
				goto check_error;
			}

			/*
			 * One-commit AND the reserve recompacted the attach
			 * node.  The relocation swaps the OLD attach node for
			 * the fresh copy at its grandparent slot.  @rec already
			 * holds a compressed parent's SKIP_X dual.  The reserve
			 * ft_node_set_nth_rec above recorded it, instead of a
			 * premature bare store.  Add the forward grandparent
			 * slot edge here.  The relocation then flips
			 * ATOMICALLY with the new key's slot edge in ic->txn:
			 * "relocate + new key" is one publication.  ic->txn was
			 * armed before the build, so there is no allocation
			 * here, hence no failure.  The old node stays
			 * resolved-to via the parked grandparent proxy until
			 * the commit.  So defer its free past insert_done.
			 */
			/*
			 * VALIDATE (§4.B): guard the LIVE grandparent that this
			 * relocation republishes into.  A concurrent remover
			 * that froze it aborts this commit (Phase 4.3
			 * load-bearing; no-op under exclusion).  Use the fresh
			 * copy's inherited (resolved) parent.  It is the same
			 * consistent snapshot that the publish slot below is
			 * verified against.
			 *
			 * LOCK_FINE (§9.3): this grandparent is recompact's P.
			 * The reserve's ft_node_recompact already LOCKED it,
			 * and recorded its {LOCK|s -> s} release on this very
			 * word.  The release record IS the guard: same word,
			 * same abort on a peer state change.  It is strictly
			 * stronger, so the conversion REPLACES the guard here.
			 * Leaving the guard would not corrupt anything.
			 * Release-then-guard chains as a read-your-writes
			 * no-op (see the ordering rule at
			 * ft_flip_txn_record_release_lock).  The guard would
			 * just be a vacuous record that re-validates the value
			 * the release already pins.
			 */
			if (!ft->lock_fine)
				ft_flip_txn_guard_parent(ft, ic->txn,
					ft_parent_node(idest_meta->parent_word));
#ifdef FEATURE_FT_PROBE_EMPTY_INSERT
			__atomic_fetch_add(&cds_ft_probe_reach_attach, 1,
				__ATOMIC_RELAXED);
			if (ft_probe_internal_is_empty(ft, iter_dest_node_flag)) {
				__atomic_fetch_add(
					&cds_ft_probe_empty_publish_attach, 1,
					__ATOMIC_RELAXED);
				ret = -EAGAIN;
				goto check_error;
			}
#endif
			_ft_publish_to_parent(ft, attach_node_flag,
				attach_node_flag_ptr, iter_dest_node_flag,
				attach_node_flag, &rec, false);
			ft_flip_txn_record_pub_rec(ic->txn, &rec);
			ic->free_old_node = old_recompacted_node;
			old_recompacted_node = NULL;
			/*
			 * The relocated fresh copy is unpublished until the
			 * commit flips the grandparent slot.  Track it with the
			 * cluster from here on.  The success handoff below then
			 * covers it for the commit-ABORT rollback.  The earlier
			 * -EAGAIN guard freed it separately because it was NOT
			 * yet tracked.
			 */
			created_nodes[nr_created_nodes++] = iter_dest_node_flag;
			/*
			 * I7 count fold (rank stats ON).  The reserve
			 * recompacted the attach node.  The relocated copy
			 * iter_dest_node_flag is a fresh node published at the
			 * grandparent slot.  It carries the new key: the fresh
			 * branch is wired into its reserved slot.  But the
			 * recompact copied only the OLD subtree count
			 * (ft_node_recompact, mutation-node.h).  So:
			 *  1. Bump the copy +1 to its FULL post-commit count.
			 *     This is a build-invisible plain store on the
			 *     fresh copy, published atomically at the commit.
			 *     The copy is then off the count walk.
			 *  2. Record the +1 walk from metadata->parent up to
			 *     the root.  metadata->parent is the STABLE
			 *     grandparent whose slot this relocation
			 *     republishes into.  It is VALIDATE-guarded live
			 *     just above, and commit-invariant.
			 * The retired old attach node is NOT on the walk.  The
			 * relocation is single-level: ft_node_set_nth_rec
			 * rebuilds only the attach node, and the grandparent is
			 * not relocated.  This is equivalent to the old
			 * post-commit walk from *d.pnfp.  Post-commit, that
			 * walk is exactly this relocated copy (+1), then its
			 * stable ancestors.
			 *
			 * Reader-safety of this pre-commit +1.  Before the
			 * commit, the fresh copy is reachable ONLY as an
			 * ANCESTOR: recompact reparents its children's parent
			 * pointers to it.  It is never reachable through its
			 * own forward slot: the grandparent slot still resolves
			 * to the old node until the flip.  Its new-key slot
			 * reads NULL until the commit.  So its own aggregate
			 * transiently overcounts what is reachable from it.
			 * This is invisible, for two reasons:
			 *  - a count reader reads a node's OWN nr_keys only at
			 *    a root-DESCENDED position, which lands on the old
			 *    node via the proxy;
			 *  - the skip up-walk reads a node's CHILDREN's counts,
			 *    not the node's own.
			 * Do not add a reader that reads an up-walked
			 * ancestor's own nr_keys without revisiting this fold.
			 */
			{
				struct cds_ft_metadata *reloc_meta =
					cds_ft_item_to_metadata(
						ft_node_ptr(iter_dest_node_flag));

				ft_nr_keys_store(ft, reloc_meta,
					ft_nr_keys_get(reloc_meta) + 1,
					CMM_RELAXED);
			}
			ic->count_from = ft_parent_node(metadata->parent_word);
			ic->count_folded = true;
		} else {
			/*
			 * In-place reserve (dest == attach node): republish the
			 * attach node at its OWN grandparent slot.  The store
			 * is a same-value forward store.  Its real work is
			 * ft_set_parent_slot's parent_slot_offset /
			 * incoming_byte maintenance and, for a compressed
			 * grandparent, the SKIP dual.
			 *
			 * Under MW it must NOT be a bare direct store.  A peer
			 * may have re-homed the attach node since this op's
			 * descent, by recompacting its grandparent.  Then
			 * @attach_node_flag_ptr is a STALE slot in the retired
			 * grandparent.  A direct ft_publish_to_parent would:
			 *  (a) drive ft_set_parent_slot's offset /
			 *      ft_slot_to_byte against the fresh grandparent
			 *      from the stale slot.  That gives a garbage
			 *      parent_slot_offset, the dominant FT_INV_MW
			 *      fault.
			 *  (b) fire its `*slot != new_child` store.  That
			 *      resurrects the retired attach node over the
			 *      peer's copy, and a commit ABORT does not roll
			 *      it back.
			 * Mirror the relocation branch above.  Re-descend if
			 * the attach node moved.  ACQUIRE the live grandparent,
			 * so that a peer freeze/relocate of it ABORTS this
			 * commit.
			 *
			 * ☠ AND RESOLVE (GRANDPARENT, SLOT) EXACTLY ONCE,
			 * through the proxy-aware resolver.  Read nothing raw
			 * afterwards.  The attach node is LIVE and this op
			 * holds its state word.  But that hold does not freeze
			 * its PARENT word.  A peer recompacting the
			 * grandparent re-homes the attach node by flipping
			 * @attach_meta->parent_word.  Mid-flip, that word
			 * carries the peer's PROXY.  A raw ft_parent_node() of
			 * it then returns a txn RECORD in place of a node.
			 * Both the acquire and ft_set_parent_slot's
			 * ft_slot_to_byte receive that record.  This was
			 * measured: the type assert of
			 * ft_popcount_node_get_ith_pos fired in
			 * inv_writer_progress_chainmerge.  By the time the
			 * core was read, @parent_word had settled to the
			 * peer's fresh copy.
			 *
			 * ft_resolve_parent_slot answers from ONE coherent
			 * snapshot: the re-home is resolved through the
			 * descriptor's status.  A re-home can still land
			 * between the resolve and the acquire.  The acquire
			 * itself catches it: the grandparent it names is
			 * either tombstoned (a MISS) or held by the peer (a
			 * MISS).
			 *
			 * There is NO same-value republish here any more.
			 * This arm used to mirror one from the relocation
			 * branch.  That republish wrote nothing, because the
			 * node did not move.  These are all unchanged:
			 *  - its parent slot;
			 *  - its slot offset;
			 *  - its incoming byte;
			 *  - a compressed grandparent's SKIP dual.
			 * The republish existed to VALIDATE the grandparent
			 * edge at commit.  The acquire below already does
			 * that, strictly more strongly.  The republish also
			 * re-read the live parent word raw inside
			 * _ft_publish_to_parent_meta_at.  That is the read
			 * that landed on the proxy.
			 */
			struct cds_ft_metadata *attach_meta =
				cds_ft_item_to_metadata(
					ft_node_ptr(attach_node_flag));
			struct cds_ft_inode_flag *gp_nf = NULL;

			if (ft_resolve_parent_slot(attach_meta, ft, &gp_nf) !=
					attach_node_flag_ptr) {
				ret = -EAGAIN;
				goto check_error;
			}
			/*
			 * The reserved slot is INSIDE the attach node, whose
			 * position in the trie is the grandparent's slot.  The
			 * grandparent is a lock-set member, so ACQUIRE it
			 * rather than merely guard it (I-1's second half).  It
			 * is a value-swap target: the op does not change its
			 * nr_child, and never copies its body under the lock.
			 * So the release terminal is clean, and no count edge
			 * shares the word.  A miss aborts and re-descends
			 * (f7cc59f9).  The resolver mismatch just above
			 * already does the same for the stale-slot case.  NULL
			 * @gp_nf is the root position: there is no node to
			 * acquire, and the root slot CAS auto-guards it.
			 */
			ft_flip_txn_lock_or_guard_parent(ft, ic->txn, ctx,
				gp_nf, FT_DEPTH_FROM_DESCENT);
#ifndef FT_DEBUG_ATTACH_MISS_CARRIES_ON
			/*
			 * ...and it re-descends HERE, not at the commit.  This
			 * is the resolver mismatch's own exit.  It comes before
			 * the count walk below is recorded into a txn that can
			 * only be discarded.
			 */
			if (ic->txn && caa_unlikely(ic->txn->acquire_miss)) {
				ret = ic->txn->acquire_enomem ? -ENOMEM : -EAGAIN;
				goto check_error;
			}
#endif
#ifdef FEATURE_FT_PROBE_EMPTY_INSERT
			__atomic_fetch_add(&cds_ft_probe_reach_attach, 1,
				__ATOMIC_RELAXED);
			if (ft_probe_internal_is_empty(ft, iter_dest_node_flag)) {
				__atomic_fetch_add(
					&cds_ft_probe_empty_publish_attach, 1,
					__ATOMIC_RELAXED);
				ret = -EAGAIN;
				goto check_error;
			}
#endif
			/*
			 * I6 count fold (rank stats ON).  The attach node stays
			 * in place, so its metadata->parent chain up to the
			 * root is commit-invariant.  Record the +1 key-count
			 * walk from it into THIS txn.  The base is
			 * attach_node_flag, the stable node that owns the new
			 * key's reserved forward slot.  The count then flips
			 * ATOMICALLY with the structural publish, and
			 * insert_done skips the standalone post-commit
			 * root-ward count walk.  The fresh branch below
			 * carries its +1 from build (build-invisible plain
			 * stores).  So the recorded walk never touches it.
			 * Under the retained writer exclusion, the behaviour
			 * is identical to the old post-commit walk from
			 * *d.pnfp, which in place is exactly attach_node_flag.
			 * The relocation branch above (I7) also sets
			 * count_folded: it records its walk from the
			 * grandparent.
			 */
			ic->count_from = attach_node_flag;
			ic->count_folded = true;
		}

		/* Reclaim safely after unlink (deferred to the commit when the
		 * relocation was folded into ic->txn above). */
		if (old_recompacted_node)
			free_cds_ft_node(ft, old_recompacted_node);
	}

	/* Success */
	ret = 0;
	/*
	 * Hand the fresh cluster to the op scope.  It stays unpublished until
	 * the one-commit flips the parked slots.  So a commit ABORT's rollback
	 * (ft_insert_abort_cb) must free it.  Without this handoff, every
	 * commit-time ABORT of the attach shapes leaked the whole fresh
	 * branch.  Only the split builders handed their clusters over.
	 */
	if (ic->txn) {
		memcpy(ic->created, created_nodes,
			(size_t) nr_created_nodes * sizeof(created_nodes[0]));
		ic->nr_created = nr_created_nodes;
	}

check_error:
	if (ret) {
		/*
		 * All goto-check_error paths in this function are before
		 * ft_publish_to_parent.  So created_nodes[] never escaped
		 * the writer's stack, and immediate-free is safe.
		 *
		 * The armed one-commit txn MAY already carry recorded edges.
		 * A reserve recompact records the external-head back-channel,
		 * reparent pairs and the retire tombstone.  A later step can
		 * then bail -EAGAIN.  Records are PREPARE-state only: nothing
		 * is installed in a live slot during the build.  The
		 * UNCONDITIONAL destroy below discards them coherently.  Do
		 * not "optimize" the destroy behind a records-empty check.
		 */
		if (ic->txn) {
			ft_flip_txn_destroy(ic->txn);
			ic->txn = NULL;
		}
		for (i = 0; i < nr_created_nodes; i++) {
			if (ft_node_compressed(created_nodes[i]))
				free_compressed_node_unpublished(ft,
					ft_compressed_node_ptr(created_nodes[i]));
			else
				free_cds_ft_node_unpublished(ft, ft_node_ptr(created_nodes[i]));
		}
	}
	FT_TP(attach_node_exit, (int) ret);
	return ret;
}

static
int ft_chain_node(struct cds_ft *ft, struct cds_ft_node *last_node,
		struct cds_ft_node *node)
{
	struct ft_flip_txn *t;
	enum urcu_txn_status st;

	FT_TP(chain_node, (const void *) last_node, (const void *) node);
	/*
	 * Add node to the tail of the duplicate chain.  RCU traversals then
	 * always see either the prior node or the newly added one under a
	 * concurrent add-then-del on the same key.  Safe against concurrent
	 * RCU read traversals.
	 *
	 * The tail append is the sole reader-visible publish of a duplicate
	 * insert.  Record it as an hlist insert-after edge
	 * (last_node->next: NULL -> node), folded into a single-edge
	 * flip-txn instead of a bare store.  The edge is a recorded CAS: a
	 * concurrent freeze of the tail would abort it.  @node is built
	 * invisibly, with next = NULL and prev = last_node.  Both next and
	 * prev ride the txn.  The commit is the engine's single-edge fast
	 * path.  Fallible: the txn allocation can OOM, and the caller then
	 * unwinds @node.
	 *
	 * CONCURRENT WRITERS (Phase 4.3): a peer freezing/removing
	 * @last_node conflicts here, at one of two times:
	 *  - at prepare time: -ENOENT (tail already marked) or -EAGAIN
	 *    (neighbour mid-deletion).  Nothing recorded installs without a
	 *    commit.
	 *  - at commit time: ABORT.  The expected-value CAS on
	 *    last_node->next lost, and nothing is installed.
	 * Both map to -EAGAIN.  The callers route to insert_done.  Its
	 * -EAGAIN handler ages the op's persistent handle and re-descends
	 * from root; restart_attempt resets @node's prev/next.  Previously
	 * ABORT was folded into 0: a silently LOST duplicate under
	 * contention.
	 */
	t = ft_flip_txn_create_bounded(ft, FT_HLIST_INSERT_AFTER_MAX_EDGES);
	if (!t)
		return -ENOMEM;
	ft_ch_audit(ft, t, last_node);
	if (ft_hlist_insert_after_prepare(ft, ft_flip_txn_handle(t), node,
			last_node)) {
		ft_flip_txn_destroy(t);
		return -EAGAIN;
	}
	st = ft_flip_txn_commit(ft, t);
	if (st < 0)
		return -ENOMEM;
	return st > 0 ? -EAGAIN : 0;
}

/*
 * There are a few cases to cover for add:
 *
 * 1) There is already an external node at that key. Chain this new node
 *    with the existing node (duplicate).
 * 2) There is already an internal node with associated external node at
 *    that key. Chain this new node with the existing node (duplicate).
 * 3) The traversal ends before reaching the end of the lookup key:
 *    3.1) The last node encountered during traversal is an internal
 *         node. Attach a new cluster as child of this internal node.
 *    3.2) The last node encountered during traversal is an external
 *         node. Transform this external node into an internal node
 *         with associated external node. Attach a new cluster as
 *         child of this internal node. Populate this new internal
 *         node into the trie to replace the prior external node.
 */

/*
 * ft_insert_compressed_past_child: key continues past a compressed
 * node's external child.  Build a branch below the child and propagate
 * density / external count through the snapshot.
 *
 * Returns 0 on success, -ENOMEM on allocation failure, -EAGAIN when the
 * caller must re-descend.
 */
static
int ft_insert_compressed_past_child(struct cds_ft *ft,
		struct ft_descent *d,
		const uint8_t *key, size_t key_len,
		struct cds_ft_compressed_node *cn,
		struct cds_ft_node *node,
		struct ft_insert_commit *ic)
{
	struct cds_ft_inode_flag *branch;
	struct cds_ft_metadata *br_meta;
	int ret;

	/*
	 * Reserve the I5 count-fold edges.  The +1 key-count walk climbs
	 * from the STABLE compressed node @d->nf up to the root.  This commit
	 * flips the child slot of @d->nf to the fresh branch.  The walk needs
	 * at most @d->depth + 2 ancestor edges: the actual descent depth,
	 * never FT_MAX_DEPTH.  No-op reserve (0) when order statistics are
	 * off.
	 */
	ret = ft_insert_commit_arm(ft, ic,
		ft->rank_stats ? d->depth + 2 : 0);
	if (ret)
		return ret;	/* nothing built yet */

	/*
	 * ☠ TAKE @cn FIRST, THEN READ ITS CHILD.  This is the split arm's
	 * order: ft_insert_dlm_acquire_split re-reads cn->child against its
	 * plan under the lock.  This arm lacked it.
	 *
	 * The dispatcher classified cn->child EXTERNAL from an unheld read.
	 * A peer past-child insert of a SIBLING extension republishes that
	 * slot IN PLACE, under @cn's lock, with a junction carrying the same
	 * head.  @cn's state word does not move: a compressed node always
	 * has one child.  Read before the lock, the snapshot was one of:
	 *  - that junction.  It was wired below as the branch's
	 *    external_nodes: an INTERNAL node posing as a prefix head.
	 *  - the old head.  Its expected-old no longer arbitrates anything.
	 *    The op holds @cn by the time it records the slot.  So the
	 *    record is a blind SW store (ft_flip_txn_record_tag's held_sw),
	 *    and it overwrites the peer's junction, key and all.
	 * MEASURED with inv_prefix_siblings_compressed_holder, 16 of 16 runs,
	 * pre-existing at adeef9a9:
	 *  - lost inserts;
	 *  - "ord-cell list longer than trie";
	 *  - hangs.
	 *
	 * @cn is held here and handed to ft_insert_publish_or_park, exactly
	 * as the split hands P over (@parent_locked_holder / _snap /
	 * _shared).  So the publish registers the release instead of taking
	 * the word again.
	 */
	struct cds_ft_inode_flag *old_child_flag;

#ifndef FT_DEBUG_PAST_CHILD_NO_PREHOLD
	if (ft->lock_fine) {
		struct ft_lock_ctx actx;
		struct ft_held_anchor held;

		ft_lock_ctx_init(&actx, d, NULL, ic->op);
		if (ft_acquire_member(ft, &actx, d->nf,
				cds_ft_item_to_metadata((struct cds_ft_inode *) cn),
				d->depth, &held)) {
			ret = -EAGAIN;
			goto arm_unwind;	/* nothing acquired */
		}
		old_child_flag = ft_cn_child_dereference_acquire_prefetch(cn);
		if (!old_child_flag || ft_node_skip_compressed(old_child_flag) ||
				!ft_node_external(old_child_flag)) {
			if (!held.shared)
				ft_meta_lock_release(held.lock);
			ret = -EAGAIN;
			goto arm_unwind;	/* stale plan: re-descend */
		}
		ic->parent_lock_shared = held.shared;
		ic->parent_locked_holder = held.shared ? NULL : held.lock;
		ic->parent_locked_snap = held.lock_snap;
	} else
#endif
	/*
	 * ONE resolved snapshot of the live external child.  Both the
	 * branch's external_nodes wiring and the parked live re-parent below
	 * consume it.  A peer may park a flip-proxy on it (re-parenting that
	 * external head).  A RAW read would then publish the proxy as the
	 * branch's external head / @ic->live_child.  At commit,
	 * ft_park_live_parent_edge would dereference the latch memory.
	 * Resolve once, like the sibling split builders
	 * (ft_split_compressed_diverge / _key_shorter).
	 */
	old_child_flag = ft_cn_child_dereference_acquire_prefetch(cn);

	/*
	 * Case 1 (external at END of compressed path): build a
	 * branch for the continuing key.  Its top is an internal node
	 * at d->depth + cn->len.  That node holds the old external
	 * child as external_nodes and dispatches the next key byte.
	 */
	{
		unsigned int br_start = d->depth + cn->len;
		struct cds_ft_inode_flag *inner;
		struct cds_ft_inode_flag *dest = NULL;

		inner = ft_build_branch(ft, key,
			br_start + 1, key_len,
			(struct cds_ft_inode_flag *) node, 1, false, NULL);
		if (!inner) {
			ret = -ENOMEM;
			goto arm_unwind;
		}
		ret = ft_node_set_nth(ft, &dest, key[br_start],
			inner, NULL, NULL, br_start, false);
		if (ret) {
			/*
			 * Free the built branch (it was leaked before): the
			 * cluster is writer-private, nothing was published.
			 * insert_done resets node->prev for the retry.
			 */
			ft_free_branch_unpublished(ft, inner,
				(struct cds_ft_inode_flag *) node);
			ret = -ENOMEM;
			goto arm_unwind;
		}
		branch = dest;
		br_meta = cds_ft_item_to_metadata(ft_node_ptr(branch));
		/*
		 * Phase 1 (build-invisible): wire branch's own parent and its
		 * cluster-internal external_nodes pointer.  The back-channel
		 * publish (cn->child->prev = branch) is deferred to Phase 2
		 * below.  cn->child is still reachable through the unmodified
		 * cn.  Without the deferral, an up-walk from it lands on
		 * branch with parent NULL.
		 */
		/* HIDDEN: the comment above says it -- "Phase 1
		 * (build-invisible)"; @branch was just built here. */
		ft_set_parent_excl(ft, branch, d->nf, &cn->child,
			FT_EXCL_HIDDEN);
		ft_metadata_set_external_nodes(branch, br_meta,
			(struct cds_ft_node *) old_child_flag);
		/*
		 * I5 count fold: build the branch with its FULL post-commit
		 * count.  That is the pre-existing key (old external from the
		 * compressed child) PLUS the new key carried by @inner below
		 * it (built full by ft_build_branch).  So this fresh node is
		 * off the count walk.  The +1 for the new key then rides the
		 * commit as edges on the STABLE ancestors from @d->nf up
		 * (recorded below).  It is not a post-commit root-ward count
		 * walk.
		 */
		ft_nr_keys_store(ft,br_meta, 2, CMM_RELAXED);
	}
	/*
	 * Phase 2: park the LIVE displaced-external-head re-parent into the
	 * one-commit.  That edge is the old external cn->child's
	 * cell->parent / prev -> branch.  It then flips ATOMICALLY with the
	 * forward publish below.  A reader never sees cn->child re-parented
	 * onto branch while the grandparent slot still points at cn, or vice
	 * versa.  ft_insert_one_commit replays it via
	 * ft_park_live_parent_edge, which resolves the external head's
	 * cell->parent / prev.
	 */
	ic->live_child = old_child_flag;
	FT_BE_SRC_SET(ic, FT_BE_PARK_PAST);
	ic->live_parent = branch;
	ic->live_slot = NULL;
	/* Fresh spine: @branch alone, just below @cn's run. */
	ic->nr_born = 0;
	ft_ic_born_add(ic, br_meta, d->depth + cn->len, 1);
	/* &cn->child's plan-snapshot old is the displaced child == ic->live_child. */
	{
		struct ft_lock_ctx pctx;

		ft_lock_ctx_init(&pctx, d, ic ? ic->txn : NULL, ic ? ic->op : NULL);
		ft_insert_publish_or_park(ft, &pctx, d->nf, d->depth,
			&cn->child, branch, ic->live_child, ic);
	}
	/*
	 * I5 count fold.  @d->nf is the STABLE compressed node whose child
	 * slot (&cn->child) this commit flips to the fresh branch.  The
	 * branch was built with its full post-commit count above.  So the +1
	 * walk starts at d->nf and touches only commit-invariant ancestors.
	 * The subtree of d->nf grows from the old external's 1 key to the
	 * branch's 2.  Record the walk into the commit txn (reserved at arm)
	 * and skip the post-commit propagate.
	 */
	ic->count_from = d->nf;
	ic->count_folded = true;
	return 0;
arm_unwind:
	/* The pre-held @cn, not yet registered into ic->txn (a no-op if not held). */
	ft_insert_dlm_release_parent(ic);
	if (ic->txn) {
		ft_flip_txn_destroy(ic->txn);
		ic->txn = NULL;
	}
	return ret;
}

/*
 * ft_insert_compressed_diverge: key diverges from the compressed path
 * at position @j.  Split the compressed node and insert the new key.
 *
 * Returns 0 on success, negative errno on failure.
 */
static
int ft_insert_compressed_diverge(struct cds_ft *ft,
		struct ft_descent *d,
		const uint8_t *iter_key,
		unsigned int remaining, unsigned int j,
		struct cds_ft_node *node,
		struct ft_insert_commit *ic)
{
	int dret;

	dret = ft_split_compressed_insert(ft, d,
		d->nfp, d->nf, iter_key, remaining,
		j, node, d->depth, ic);
	if (dret)
		return dret;
	/*
	 * ft_split_compressed_insert wires top_flag's back-pointer into the
	 * live parent before publishing the cluster's forward slot, so the
	 * caller does not need to set the parent here.
	 *
	 * I3 count fold: @d->pnf is the STABLE parent.  This commit flips its
	 * child slot (d->nfp) to the fresh split cluster.  Every fresh split
	 * node was built with its full post-commit count.  So the +1 count
	 * walk starts at d->pnf and touches only commit-invariant ancestors.
	 * Record it into the commit txn (reserved at arm) and skip the
	 * post-commit propagate.
	 */
	ic->count_from = d->pnf;
	ic->count_folded = true;
	return 0;
}

/*
 * ft_insert_compressed_key_shorter: key ends before the compressed
 * path.  Split the compressed node into prefix -> junction -> suffix,
 * then attach the new external node at the junction.
 *
 * Returns 0 on success, or a negative errno on failure (-EAGAIN when a
 * peer holds a word it needs).  It never returns -EEXIST: a key that ends
 * inside a compressed path is always a new key.  @unique_node_ret is
 * unused.
 */
static
int ft_insert_compressed_key_shorter(struct cds_ft *ft,
		struct ft_descent *d,
		unsigned int remaining,
		struct cds_ft_node *node,
		struct cds_ft_node **unique_node_ret,
		struct ft_insert_commit *ic)
{
	struct cds_ft_inode_flag *top_flag, *jct_flag;
	struct cds_ft_metadata *jct_meta;
	struct cds_ft_inode_flag *live_child, *live_parent;
	struct cds_ft_inode_flag **live_slot;
	struct cds_ft_inode_flag *split_created[FT_MAX_DEPTH];
	int split_nr_created = 0;
	struct cds_ft_metadata *cn_meta = cds_ft_item_to_metadata(
		(struct cds_ft_inode *) ft_compressed_node_ptr(d->nf));
	struct ft_held_anchor held;
	int sret;
	struct cds_ft_inode_flag *fwd_expected_old;	/* set post-fence */

	/*
	 * A key-shorter insert is always a NEW key.  A key that ends inside a
	 * compressed path has no node at that depth that could already hold
	 * it.  A real duplicate is caught on descent, before this split.  So
	 * the freshly-built junction always takes a fresh head: there is no
	 * duplicate case here.
	 */
	(void) unique_node_ret;

	/*
	 * F2 node lock, split-retire extension (see
	 * ft_split_compressed_insert).  The key-shorter split retires @d->nf
	 * and plans from its body + child.  So fence it before the builder's
	 * first read.  ft_insert_one_commit records the retire from @held.
	 */
	/*
	 * DLM Step 1: under LOCK_FINE, acquire the whole split lock-set
	 * {CN, P} up front, in one all-or-none MCAS.  P is CN's parent
	 * @d->pnf, the value-swap publish target below.  This mirrors the
	 * diverge builder.  Non-DLM / non-lock_fine keeps the single CN
	 * lock-acquire (byte-identical).
	 */
	if (ft->lock_fine) {
		sret = ft_insert_dlm_acquire_split(ft, d, d->nf, d->depth,
				cn_meta, &held, ic);
	} else {
		struct ft_lock_ctx lctx;

		ft_lock_ctx_init(&lctx, d, ic ? ic->txn : NULL, ic ? ic->op : NULL);
		sret = ft_acquire_member(ft, &lctx, d->nf, cn_meta, d->depth,
			&held);
	}
	if (sret)
		return sret;	/* -EAGAIN: peer owns the cn/P; nothing built */

	/*
	 * Plan-snapshot the raw parent slot NOW (post-fence, pre-build).  The
	 * forward publish CAS must match this reference to @d->nf.  It is
	 * skip-PRESERVED and flip-proxy-resolved.  @d->nf is skip-RESOLVED:
	 * under skip-compression it differs from the stored SKIP_X, and
	 * passing it would livelock.  Capturing pre-build also avoids
	 * ratifying a peer that raced the slot during the split builder.
	 */
	fwd_expected_old = ft_resolve_flip_proxy(*d->nfp);

	sret = ft_split_compressed_key_shorter(ft,
		d->nf, d->nfp, remaining, &top_flag, &jct_flag, d->depth,
		&live_child, &live_parent, &live_slot,
		ic->born, &ic->nr_born,
		split_created, &split_nr_created);
	if (sret) {
		ft_meta_lock_release(held.lock);
		ft_insert_dlm_release_parent(ic);	/* release P held up front */
		return sret;
	}
	jct_meta = cds_ft_item_to_metadata(ft_node_ptr(jct_flag));
	assert(!ft_node_compressed(jct_flag));
	assert(jct_meta->external_nodes == NULL);	/* always a fresh head */
	/*
	 * Attach the fresh head to the junction NOW, while the whole cluster
	 * is still invisible.  The parked one-commit publish below then makes
	 * the structural attach and the ordered-list splice atomic.
	 */
	ft_external_head_set_parent(ft, node, jct_flag, /*prefix=*/ true);
	node->next = NULL;
	/* Cluster-internal store: the junction is unpublished. */
	jct_meta->external_nodes = node;
	/*
	 * Reserve the I4 count-fold edges.  The +1 key-count walk climbs from
	 * the STABLE parent d->pnf up to the root.  This commit flips d->pnf's
	 * child slot d->nfp to the fresh split cluster.  The walk takes at
	 * most d->depth + 2 ancestor edges (actual descent depth, never
	 * FT_MAX_DEPTH).  The reserve is a no-op (0) when order statistics
	 * are off.
	 */
	sret = ft_insert_commit_arm(ft, ic,
		ft->rank_stats ? d->depth + 2 : 0);
	if (sret) {
		/*
		 * Arm failed (-ENOMEM): abort the whole insert.  The split
		 * cluster is still build-invisible:
		 *  - nothing is published;
		 *  - cn->child is untouched;
		 *  - @d->nf is still reader-reachable at @d->nfp.
		 * So roll the head attach back and TEAR THE CLUSTER DOWN.
		 * The structure is left byte-for-byte unchanged.
		 *
		 * Do NOT publish a key-neutral restructure here.  The forward
		 * publish and the live old-child re-parent must flip
		 * ATOMICALLY.  The re-parent's source is the live cn->child,
		 * so wiring it before the forward exposes the unpublished
		 * cluster to an up-walk.  An atomic flip needs the one-commit
		 * txn we just failed to allocate.  There is no
		 * MCAS-expressible publish on this path: a bare forward store
		 * would sit outside the descriptor set.  Only a clean abort
		 * remains.  The caller surfaces MEMORY_ERROR.  insert_done
		 * resets node->prev for a retry.
		 */
		jct_meta->external_nodes = NULL;
		node->next = NULL;
		ft_free_unpublished_split_cluster(ft, split_created,
			split_nr_created);
		ft_meta_lock_release(held.lock);
		ft_insert_dlm_release_parent(ic);	/* release P held up front */
		return sret;
	}
	/*
	 * The armed txn now owns the fence outcome (registered clear on every
	 * non-commit terminal; consumed by the fenced tombstone on commit).
	 */
	ft_flip_txn_lock_register_held(ic->txn, &held);
	ft_flip_txn_record_anchor_release(ic->txn, &held, cn_meta);
	ic->free_old_cn_held = held;
	/*
	 * Concurrent-writer conflict check (mirror of the diverge builder's
	 * pre-arm guard).  @d->nfp was recorded at descent as the cn's slot
	 * in its parent.  A peer may since have re-homed the cn.  Consuming
	 * the stale (parent, slot) pair below would invert it against the
	 * wrong node's body and fault ft_slot_to_byte mid-build.  The fence
	 * does not cover this: a re-home committed BEFORE the mark leaves the
	 * cn clean.  Re-derive from the cn's own resolved (parent, offset)
	 * pair.  On a disagreement, unwind exactly as the arm-failure above
	 * (nothing published) and re-descend.
	 */
	if (ft_get_parent_slot(cn_meta, ft) != d->nfp) {
		jct_meta->external_nodes = NULL;
		node->next = NULL;
		ft_free_unpublished_split_cluster(ft, split_created,
			split_nr_created);
		/* Registered above: the txn owns CN's fence; discard both. */
		ft_flip_txn_destroy(ic->txn);
		ic->txn = NULL;
		/* P was held up front, not yet registered (publish not reached). */
		ft_insert_dlm_release_parent(ic);
		return -EAGAIN;
	}
	/*
	 * Hand the fresh cluster to the op scope.  The commit's on-abort
	 * rollback (ft_insert_abort_cb) frees it if a peer writer wins at
	 * commit time.  Without this, every commit ABORT of the key-shorter
	 * shape leaked the unpublished split cluster.
	 */
	memcpy(ic->created, split_created,
		(size_t) split_nr_created * sizeof(split_created[0]));
	ic->nr_created = split_nr_created;
	/*
	 * top_flag's own back-pointer was wired in the split (it is a fresh
	 * cluster node).  The LIVE old-child re-parent is the back-channel
	 * that exposes the cluster to a reanchor up-walk.  So defer it to the
	 * parked one-commit.  There it flips atomically with the forward
	 * publish and the cell (fresh head + ordered list).
	 */
	ic->live_child = live_child;
	ic->live_parent = live_parent;
	ic->live_slot = live_slot;
	FT_BE_SRC_SET(ic, FT_BE_PARK_KSHORT);
	{
		struct ft_lock_ctx pctx;

		ft_lock_ctx_init(&pctx, d, ic ? ic->txn : NULL, ic ? ic->op : NULL);
		ft_insert_publish_or_park(ft, &pctx, d->pnf, d->pdepth,
			d->nfp, top_flag, fwd_expected_old, ic);
	}
	/*
	 * I4 count fold: @d->pnf is the STABLE parent.  This commit flips its
	 * child slot (d->nfp) from the old compressed node to the fresh split
	 * cluster.  Every fresh split node (junction + prefix) was built
	 * above with its full post-commit count.  So the +1 walk starts at
	 * d->pnf and touches only commit-invariant ancestors.  Record it into
	 * the commit txn (reserved at arm) and skip the post-commit
	 * propagate.  The old compressed node's free follows the commit at
	 * insert_done.  Until then a reader resolves the parked proxy to it.
	 */
	ic->count_from = d->pnf;
	ic->count_folded = true;
	ic->free_old_cn = ft_compressed_node_ptr(d->nf);
	return 0;
}

/*
 * Handle a compressed node during insert descent.
 *
 * Full match + internal/compressed child: traverse through.
 * Full match + external child at end of key: break for duplicate handling.
 * Full match + external child, key continues: build branch inline.
 * Key diverges: split via ft_split_compressed_insert.
 * Key shorter: split via ft_split_compressed_key_shorter.
 *
 * Returns CONTINUE, BREAK, or END (with ret set via *ret_p).
 * On END, the caller should goto insert_done.
 * On error, returns END with *ret_p < 0.
 */
static
enum ft_descent_action ft_insert_compressed(struct cds_ft *ft,
		struct ft_descent *d, const uint8_t **iter_key_p,
		const uint8_t *key, size_t key_len,
		unsigned int key_depth,
		struct cds_ft_node *node,
		struct cds_ft_node **unique_node_ret,
		struct cds_ft_inode_flag **snapshot,
		unsigned int *snapshot_depth,
		int *nr_snapshot_p,
		int *ret_p,
		struct ft_insert_commit *ic)
{
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(d->nf);
	unsigned int remaining = key_depth - 1 - d->depth;
	unsigned int cmp;
	unsigned int j;

	/* Flight-recorder mis-wire detector (no-op without FT_ENABLE_TRACING). */
	FT_TRACE_MISWIRE(ft, d->nf, 1);
	cmp = cn->len < remaining ? cn->len : remaining;
	j = ft_match_compressed_key(*iter_key_p, cn, cmp);
	if (j == cmp && cn->len <= remaining) {
		/*
		 * ONE RESOLVED READ classifies the child.  A peer's parked flip
		 * proxy carries FT_INTERNAL_MASK, so a raw cn->child read as
		 * "internal" whatever it resolves to.
		 */
		struct cds_ft_inode_flag *child =
			ft_resolve_flip_proxy(rcu_dereference(cn->child));

		/* Full match.  Traverse through if child is internal,
		 * compressed or skip-compressed.  A skip-compressed child
		 * encodes another compressed node deeper in the chain. */
		if (child &&
		    (ft_node_skip_compressed(child) ||
		     ft_node_internal(child) ||
		     ft_node_compressed(child))) {
			ft_snapshot_push(snapshot, snapshot_depth,
				*nr_snapshot_p, d->nf, d->depth);
			ft_descent_traverse_compressed(ft, d, cn, iter_key_p);
			/*
			 * ☠ THE TRAVERSAL READS cn->child AGAIN.  In between,
			 * a sibling's remove may fold its junction back to
			 * the head.  The cursor is then on an EXTERNAL leaf
			 * with the key continuing.  This is the past-child
			 * shape, which only this dispatcher can route.
			 * Returned as CONTINUE, it makes the caller's loop
			 * break on the external and attach under d->pnf,
			 * which is this COMPRESSED node.  A compressed node
			 * has no byte slot (ft_attach_node's `slot_ptr`
			 * assert, MEASURED under two sibling-extension
			 * writers).  Nothing is built yet: re-descend.
			 */
#ifndef FT_DEBUG_NO_TRAVERSE_RECHECK
			if (d->nf && !ft_node_skip_compressed(d->nf) &&
					ft_node_external(d->nf) &&
					cn->len < remaining) {
				*ret_p = -EAGAIN;
				return FT_DESCENT_END;
			}
#endif
			return FT_DESCENT_CONTINUE;
		}
		if (!child) {
			struct cds_ft_metadata *cn_meta =
				cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
			fprintf(stderr, "BUG: cn->child NULL, cn=%p cn->len=%u depth=%u external_nodes=%p nr_child=%u\n",
				cn, cn->len, d->depth, cn_meta->external_nodes, (unsigned)ft_meta_nr_child(cn_meta));
			abort();
		}
		if (cn->len == remaining) {
			/* Key ends at external child: duplicate. */
			ft_snapshot_push(snapshot, snapshot_depth,
				*nr_snapshot_p, d->nf, d->depth);
			ft_descent_traverse_compressed(ft, d, cn, iter_key_p);
			return FT_DESCENT_BREAK;
		}
		/* Key continues past external child: build branch. */
		*ret_p = ft_insert_compressed_past_child(ft, d, key,
			key_len, cn, node, ic);
		return FT_DESCENT_END;
	}
	if (j < cmp) {
		/* Key diverges: split at position j. */
		*ret_p = ft_insert_compressed_diverge(ft, d,
			*iter_key_p, remaining, j, node, ic);
		return FT_DESCENT_END;
	}
	/* Key shorter: split into prefix -> junction -> suffix. */
	*ret_p = ft_insert_compressed_key_shorter(ft, d, remaining,
		node, unique_node_ret, ic);
	return FT_DESCENT_END;
}

static
int _cds_ft_insert(struct cds_ft *ft,
		const uint8_t *_key, size_t _key_len,
		struct cds_ft_node *node,
		struct cds_ft_node **unique_node_ret)
{
	unsigned int key_depth;
	struct ft_descent d;
	size_t key_len = ft_key_len(ft, _key_len);
	const struct cds_ft_key_map *km = &ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *key;
	const uint8_t *iter_key;
	struct cds_ft_inode_flag *snapshot[FT_MAX_DEPTH];
	unsigned int snapshot_depth[FT_MAX_DEPTH]; /* parallel depth tracking */
	int nr_snapshot = 0;
	int ret;
	struct ft_ord_cell *precell;
	void *cell = NULL;			/* @precell's carrier; reused across retries */
	enum urcu_txn_status cst = URCU_TXN_STATUS_OK;	/* last commit outcome */
	FT_SH_STALL_DECL;
	struct ft_insert_commit ic = { 0 };
	/*
	 * The attach's recompactions lock {C, P, GP}; @d dates them and @ic.txn
	 * names what this op already holds.  Re-initialised at each attach,
	 * because the txn is armed part-way through.
	 */
	struct ft_lock_ctx actx;
	struct ft_op optxn;		/* persistent handle spanning the retry loop */

	if (!valid_external_node(node) || !valid_key_len(ft, key_len))
		return -EINVAL;
	if (caa_likely(km->identity)) {
		key = _key;
	} else {
		ft_key_to_ordinals(ordinal_buf, _key, key_len, km);
		key = ordinal_buf;
	}
	iter_key = key;
	/* Expect zeroed prev/next pointers. This catches some double-insert misuses. */
	if (node->prev || ft_node_next(node))
		return -EINVAL;

	/*
	 * Ordered-list trie: pre-wire @node's ordinal cell before any
	 * structural mutation.  The only failure-prone allocation then
	 * happens up front: a clean -ENOMEM, with nothing to roll back.
	 * node->prev already carries the cell.  So every fresh-head wiring
	 * site downstream just records the flagged parent into the cell.
	 *
	 * The unused @precell is freed at insert_done in two cases:
	 *  - @node ends up a duplicate (chained, not a head);
	 *  - the insert fails.
	 * A chained @node has its prev repointed at the predecessor.  That
	 * loses the cell from node->prev, so the handle is kept here.
	 *
	 * List off: no cell, which saves the per-key cell.  @node behaves
	 * like a non-cell build: the fresh-head sites wire its prev to the
	 * flagged parent directly.  @precell stays NULL and the cell paths
	 * below no-op.
	 */
	precell = NULL;
	if (ft->ordered_list) {
		cell = ft_ord_cell_alloc(ft, node, NULL);

		if (!cell)
			return -ENOMEM;
		precell = ft_ord_cell_ptr(cell);
	}

	key_depth = key_len + 1;

	/*
	 * The op's persistent engine handle (doc §11) is initialized ONCE.
	 * So these span every attempt of the loop below:
	 *  - contention aging (txn->retry);
	 *  - the FIFO fair-mutex escalation turn;
	 *  - the learned descriptor size.
	 * Each attempt is bracketed by urcu_txn_begin()/urcu_txn_end(): the
	 * FT owns its read-side section.  On a concurrent trie the descent
	 * must run inside one.  Otherwise a peer writer's call_rcu-deferred
	 * frees could reclaim nodes under it.  A caller-held section merely
	 * nests.  On an exclusive trie the bracket opens nothing (NULL
	 * flavor) and the domain is NULL (never escalates).  The behavior is
	 * identical to the pre-bracket path.
	 */
	ft_txn_op_init(ft, &optxn);

restart_attempt:
	FT_SH_STALL_TICK(ft, "_cds_ft_insert");
	ft_op_begin(&optxn);
	/*
	 * Per-attempt setup.  It is re-entered on a concurrent-writer
	 * conflict (a pre-commit -EAGAIN or a commit ABORT).  The failed
	 * attempt published nothing and its fresh cluster was already rolled
	 * back.  So:
	 *  - re-arm the caller's node linkage;
	 *  - clear the commit scope;
	 *  - re-descend from the root against the now-current tree.
	 * @precell is allocated once (above) and REUSED across attempts.  It
	 * is spliced only by a committing attempt.
	 *
	 * Head's last edge byte for the up-walk key rebuild: the cell is the
	 * head's metadata record and @key is ordinal here.  So
	 * key[key_len - 1] is the byte the head hangs under.  The up-walk
	 * ignores it when the head's parent is a compressed node, whose
	 * key_bytes already span the head's position.
	 */
	ic = (struct ft_insert_commit){ 0 };
	ic.ft = ft;
	ic.op = &optxn;
	cst = URCU_TXN_STATUS_OK;
	node->prev = cell;			/* NULL when the list is off */
	node->next = NULL;
	if (precell && key_len)
		cds_ft_item_to_metadata(precell)->incoming_byte =
			(uint8_t) key[key_len - 1];
	iter_key = key;
	nr_snapshot = 0;

	dbg_printf("cds_ft_insert attempt: node %p\n", node);
	ft_descent_init(&d, ft);

	for (; d.depth < key_depth - 1; ) {
		uint8_t key_value;

		/*
		 * The last reanchoring step landed shallower (rewind > 0: a
		 * peer chain-merge moved the encoded position up).  The
		 * captured publish slot d.nfp is then at the wrong level.
		 * Re-descend against the now-current tree.  Shared read/write
		 * skip concurrency: the step detects the shift, the mutator
		 * reacts (a reader rewinds).
		 */
		if (caa_unlikely(d.skip_conflict)) {
			ret = -EAGAIN;
			goto insert_done;
		}
		if (!d.nf)
			break;
		/*
		 * Resolve skip-compressed pointer.  Convert to the
		 * underlying compressed flag so the compressed handler
		 * below processes it correctly.
		 */
		d.nf = ft_resolve_skip_compressed(ft, d.nf);
		/* Found external node. */
		if (ft_node_external(d.nf))
			break;
		/* Decompress compressed node before continuing descent. */
		/*
		 * Compressed node: compare remaining key bytes with
		 * the compressed path.  If they match, traverse
		 * through to the child.  If they diverge, decompress
		 * at this point and restart.
		 */
		if (ft_node_compressed(d.nf)) {
			enum ft_descent_action act;

			act = ft_insert_compressed(ft, &d, &iter_key,
				key, key_len, key_depth, node,
				unique_node_ret, snapshot, snapshot_depth,
				&nr_snapshot, &ret, &ic);
			if (act == FT_DESCENT_END)
				goto insert_done;
			if (act == FT_DESCENT_BREAK)
				break;
			continue;
		}
		dbg_printf("cds_ft_insert iter ppnf %p pnf %p nfp %p nf %p\n",
				d.ppnf, d.pnf, d.nfp, d.nf);
		ft_snapshot_push(snapshot, snapshot_depth,
			nr_snapshot, d.nf, d.depth);
		key_value = *(iter_key++);
		ft_descent_step(ft, &d, key_value);
	}

	/*
	 * A final reanchoring step that reached key_depth - 1 exited the loop
	 * without re-entering its top: honour a rewind > 0 conflict here too.
	 */
	if (caa_unlikely(d.skip_conflict)) {
		ret = -EAGAIN;
		goto insert_done;
	}
	/*
	 * Resolve any skip-compressed pointer left in d.nf by the descent
	 * loop's final step.  For example, ft_descent_traverse_compressed
	 * sets d.nf to cn->child raw, which may be skip-compressed.  The
	 * loop body resolves at the top of each iteration, so only when the
	 * loop iterates again.  A traverse that pushes d.depth to
	 * key_depth - 1 exits the loop without re-entering.
	 */
	d.nf = ft_resolve_skip_compressed(ft, d.nf);

	if (d.depth == key_depth - 1) {
		/* Found either an internal, external node or NULL at end of key. */
		if (!d.nf) {
			dbg_printf("cds_ft_insert NULL ppnf %p pnf %p nfp %p nf %p\n",
					d.ppnf, d.pnf, d.nfp, d.nf);

			ft_lock_ctx_init(&actx, &d, ic.txn, ic.op);
	ic.ctx = &actx;
			ret = ft_attach_node(ft, d.pnfp, d.pnf,
					d.nfp, d.nf, key, key_len, d.depth, node,
					NULL, &ic, &actx);
			if (ret == 0) {
				/*
				 * One-commit insert (ic.slot parked): the +1 count
				 * folds into insert_done's commit (an early +1 here
				 * would overcount -- the inverse of the nr_keys
				 * undercount discipline).  With order-statistics on
				 * the insert always parks, so no standalone walk
				 * remains.
				 */
				assert(ic.slot || !ft->rank_stats);
				if (d.depth >= 2)
					FT_TP(tree_edge_set, (const void *) ft,
						(const void *) d.ppnf,
						(unsigned int) (d.depth - 2),
						(uint8_t) key[d.depth - 2],
						(const void *) *d.pnfp);
			}

		} else if (ft_node_compressed(d.nf)) {
			/*
			 * Key ends at a compressed node's depth.
			 * Split: internal(external_nodes) + compressed(len-1).
			 */
			ret = ft_insert_compressed_key_shorter(ft, &d, 0,
				node, unique_node_ret, &ic);
		} else if (!ft_node_external(d.nf)) {
			struct cds_ft_node *external_nodes;
			struct cds_ft_metadata *metadata;

			assert(!ft_node_compressed(d.nf));
			metadata = cds_ft_item_to_metadata(ft_node_ptr(d.nf));
			/*
			 * RESOLVED, as every other external_nodes reader.  A
			 * peer's in-flight head park or swap parks a flip
			 * proxy here.  A raw proxy taken as the chain head
			 * sends the tail walk (and the verdict) into
			 * descriptor memory.
			 */
			external_nodes = ft_dereference_external(
				metadata->external_nodes);
			/*
			 * CAPTURE -> ACQUIRE window (-DFT_DELAY_INJECT only).
			 * @external_nodes is read here with NOTHING held; the holder
			 * acquire and the tail walk that starts FROM this value are
			 * both below.  A peer's same-key SOLE remove landing in the
			 * gap retires this very head, and the walk then ends on a
			 * RETIRED node -- FT-SLOT-2.  Widen the gap so the shape is
			 * reachable under test rather than only on paper.
			 */
			ft_delay_writer();
			if (external_nodes) {
				struct cds_ft_node *iter_node, *last_node = NULL;
				struct cds_ft_metadata *dup_hmeta = NULL;
				uintptr_t dup_hsnap = 0;

				/*
				 * MW LOCK_FINE (Step A, holder lock).  The
				 * chain walk + tail append serialise on
				 * @metadata.  It is the head's holder: the
				 * internal node whose external_nodes root
				 * this chain.  Acquire its node lock BEFORE
				 * the walk and release it after the append.
				 * Otherwise a peer relinking the chain would
				 * send the walk into freed memory.
				 * ft_chain_node touches only last_node->next,
				 * never holder->state, so the held fence
				 * composes.  FT-wide lock makes the miss
				 * unreachable in soak (fault injection drives
				 * the bail).
				 */
				if (ft->lock_fine) {
#ifdef FEATURE_FT_FAULT_INJECT
					if (cds_ft_fault_lock_countdown >= 0) {
						if (cds_ft_fault_lock_countdown == 0) {
							cds_ft_fault_lock_countdown = -1;
							ret = -EAGAIN;
							goto insert_done;
						}
						cds_ft_fault_lock_countdown--;
					}
#endif
					{
						struct ft_lock_ctx hctx;
						struct ft_held_anchor hh;

						ft_lock_ctx_init(&hctx, &d,
							ic.txn, ic.op);
						if (ft_acquire_member(ft, &hctx,
								d.nf, metadata,
								d.depth, &hh)) {
							FT_DBG_UNIQUE_ACQFAIL(
								unique_node_ret,
								metadata,
								external_nodes);
							ret = -EAGAIN;
							goto insert_done;
						}
						/*
						 * SHARED is a SUCCESS: the word is
						 * already in this op's held set, so
						 * the chain walk is excluded and the
						 * FIRST acquire owns the release.
						 * Leaving @dup_hmeta NULL is exactly
						 * "held, owing no release".  Folding
						 * it into the miss above would refuse
						 * this op's OWN mark, which no retry
						 * can clear.
						 */
						if (!hh.shared) {
							dup_hmeta = hh.lock;
							dup_hsnap = hh.lock_snap;
						}
					}
					/*
					 * RE-VALIDATE THE CAPTURE
					 * UNDER THE LOCK.  This is
					 * the PREFIX-HEAD twin of the
					 * external-chain arm below,
					 * with the same window.
					 * @external_nodes was read
					 * with NOTHING held.  A
					 * same-key cds_ft_remove can
					 * have retired that head (or
					 * emptied the chain) before
					 * this acquire landed.  The
					 * walk would then start from
					 * a RETIRED head and append
					 * behind a frozen next.  The
					 * holder here is @d.nf
					 * itself.  So there is no
					 * NULL holder to catch, only
					 * the stale head.
					 * ☠ FIX BOTH ARMS OR NEITHER:
					 * they are the same defect in
					 * two shapes.
					 */
					{
						bool stale__ =
							ft_dereference_external(
								metadata->external_nodes) !=
							external_nodes ||
							ft_node_is_removed(
								external_nodes);

						/*
						 * The one place the question is
						 * DECIDABLE: under the lock, was
						 * the unheld capture already wrong?
						 */
						FT_DBG_UNIQUE(stale__,
							unique_node_ret);
						if (caa_unlikely(stale__)) {
							if (dup_hmeta)
								ft_meta_lock_release(
									dup_hmeta);
							ret = -EAGAIN;
							goto insert_done;
						}
					}
				}
				/*
				 * ★ THE -EEXIST VERDICT, TAKEN UNDER THE LOCK.
				 *
				 * This return used to sit at the CAPTURE
				 * above, where @external_nodes is read with
				 * NOTHING held.  So cds_ft_insert_unique
				 * answered "a duplicate exists, here it is"
				 * from a word no exclusion covered.  It also
				 * jumped out BEFORE the acquire and the
				 * re-validate that the plain-insert path goes
				 * on to make.  The fix needed no new code.
				 * The walk already needs the re-validation
				 * (@external_nodes unmoved AND not retired).
				 * That is exactly the proof this verdict
				 * needs.  So the verdict moved BELOW it.
				 *
				 * MEASURED before: ~40 DUPLICATE_FOUND per run
				 * whose node was not on the key's chain.
				 * COARSE gave zero from a sample at least as
				 * large (inv_concurrent_insert_unique_nolist).
				 * A peer's same-key sole remove retires the
				 * captured head in the gap.  The caller is
				 * then handed "your insert failed, here is the
				 * existing node" for a node that has left the
				 * trie.
				 *
				 * ☞ UNIQUENESS WAS NEVER THE BROKEN PART.
				 * That was measured too, with a red control
				 * that forced the detector to fire.  So the
				 * zero is meaningful.  The empty-key race is
				 * already arbitrated: the loser's parent-slot
				 * CAS fails and its retry then sees the
				 * duplicate.  This is why the fix is a
				 * re-validation and NOT new exclusion around
				 * the insert.
				 *
				 * A stale capture now answers -EAGAIN from
				 * the re-validate above, and the wrapper
				 * re-descends.  So the holder lock covers the
				 * verdict that a caller receives.
				 * ☠ FIX BOTH ARMS OR NEITHER: the
				 * external-chain twin below is the same
				 * defect.
				 */
				if (unique_node_ret) {
					*unique_node_ret = external_nodes;
					ret = -EEXIST;
					if (dup_hmeta)
						ft_meta_lock_release(dup_hmeta);
					goto insert_done;
				}
				/* Find last duplicate */
				iter_node = external_nodes;
				cds_ft_for_each_duplicate(iter_node)
					last_node = iter_node;

				dbg_printf("cds_ft_insert duplicate internal ppnf %p pnf %p nfp %p nf %p\n",
						d.ppnf, d.pnf, d.nfp, d.nf);

				/* Adding duplicate at existing key: no key count change. */
				ret = ft_chain_node(ft, last_node, node);
				if (dup_hmeta)
					ft_meta_lock_release(dup_hmeta);
				if (ret)
					goto insert_done;
			} else {
				/* New key at this internal node. */
				ft_external_head_set_parent(ft, node, d.nf, /*prefix=*/ true);
				node->next = NULL;
				/*
				 * Park the external_nodes publish into the
				 * one-commit batch.  Readers resolve the
				 * proxy via ft_dereference_external.
				 * insert_done's single flip then commits
				 * these together:
				 *  - the structural publish;
				 *  - the ordinal-cell splice (list on);
				 *  - the +1 key count (rank stats);
				 *  - the §4.B VALIDATE guard on the LIVE
				 *    holder @metadata, which
				 *    ft_insert_park_external_nodes records.
				 * The guard makes a concurrent remove that
				 * froze the holder abort this publish.
				 *
				 * List off + rank off carries only the
				 * forward external_nodes edge + that guard.
				 * This is a 2-record MCAS commit.  It is
				 * slab-allocated and past the engine's nr==1
				 * fast path (no malloc).  That is the
				 * multi-writer cost of guarding this in-place
				 * publish: it is no longer a bare lone flip.
				 */
				ret = ft_insert_commit_arm(ft, &ic,
					ft->rank_stats ? d.depth + 2 : 0);
				if (ret)
					goto insert_done;
				ret = ft_insert_park_external_nodes(ft, &d,
					metadata, node, NULL, &ic);
				if (ret)
					goto insert_done;
				ic.count_from = d.nf;
				/*
				 * I1 (list on) / I2 (list off) count fold.
				 * @d.nf is the STABLE existing internal node.
				 * The new key's external_nodes publish lands
				 * on it, so its parent chain is
				 * commit-invariant.  Record the +1 count walk
				 * into the same txn (no fresh nodes on the
				 * path) and skip the post-commit propagate.
				 * (Rank stats off: the record is a no-op and
				 * the flag skips a no-op walk.)
				 */
				ic.count_folded = true;
				ret = 0;
			}
		} else {
			struct cds_ft_node *iter_node, *last_node = NULL;
			struct cds_ft_node *dup_head =
				(struct cds_ft_node *) ft_node_ptr(d.nf);
			struct cds_ft_metadata *dup_hmeta = NULL;
			uintptr_t dup_hsnap = 0;

			/*
			 * DESCENT -> ACQUIRE window (-DFT_DELAY_INJECT only).
			 * This is the EXTERNAL-chain twin of the prefix-head
			 * site above.  @dup_head is the descent's own
			 * landing, taken with nothing held.  The holder
			 * acquire and the tail walk that starts FROM it are
			 * both below.  A peer's same-key SOLE remove in the
			 * gap retires this head -- FT-SLOT-2.
			 */
			ft_delay_writer();

			/*
			 * MW LOCK_FINE (Step A, holder lock): serialise the
			 * chain walk + tail append on the external head's
			 * holder.  The holder is its immediate parent.
			 * ft_chain_head_holder resolves it from the head
			 * itself: one hop, prev is the cell / flagged
			 * parent.  Acquire BEFORE the walk, release after
			 * the append (ft_chain_node touches only
			 * last_node->next).  FT-wide lock makes the miss
			 * unreachable in soak (fault injection drives the
			 * bail).
			 *
			 * ☠ A NULL HOLDER IS A STALE DERIVATION, NOT AN
			 * IMPOSSIBILITY.  @dup_head is the DESCENT's
			 * landing, taken with nothing held.  Under LOCK_FINE
			 * a same-key cds_ft_remove is concurrent by contract
			 * (fractal-trie.h: "including on the same key").  It
			 * can retire that head before this resolve runs.
			 * The head's prev then names no holder and this walk
			 * returns NULL.
			 *
			 * The text that stood here said the shape "cannot be
			 * here", produced "only by ft-insert's own unwind
			 * paths on UNPUBLISHED nodes".  It cited "0 NULL in
			 * 491532 ft_chain_head_holder calls across ft_unit
			 * and ft_inv's three list modes".  THAT ZERO WAS A
			 * WRONG ZERO.  Every one of those oracles gives each
			 * writer a DISJOINT key range.  So no two writers
			 * ever met on one duplicate chain, and the
			 * measurement could not see this shape.
			 * MEASURED now, with one appender and one same-key
			 * remover: the assert fires at ZERO delay injection.
			 * COARSE adjudicates it: serialised writers are GREEN
			 * over 200000 appends.  So the class is the FINE
			 * point-vs-point contract.
			 *
			 * So BAIL RETRIABLY.  The remove side's routing
			 * re-validation takes the same disposition.  -EAGAIN
			 * frees the fresh cluster, ages the conflict and
			 * re-descends against the current tree.  Asserting
			 * instead was not merely noisy.  A release build
			 * runs straight on into ft_flag_to_metadata(ft, NULL)
			 * and hands the result to ft_acquire_member as a
			 * lock word.
			 */
			if (ft->lock_fine) {
				struct cds_ft_inode_flag *holder_flag =
					ft_chain_head_holder(ft, dup_head);

				if (caa_unlikely(!holder_flag)) {
					ret = -EAGAIN;
					goto insert_done;
				}
				{
					struct cds_ft_metadata *hm =
						ft_flag_to_metadata(ft, holder_flag);
#ifdef FEATURE_FT_FAULT_INJECT
					if (cds_ft_fault_lock_countdown >= 0) {
						if (cds_ft_fault_lock_countdown == 0) {
							cds_ft_fault_lock_countdown = -1;
							ret = -EAGAIN;
							goto insert_done;
						}
						cds_ft_fault_lock_countdown--;
					}
#endif
					{
						struct ft_lock_ctx hctx;
						struct ft_held_anchor hh;
						unsigned int hd;

						/*
						 * The head's holder IS the
						 * descent's parent (§5.2), so
						 * the window dates it.
						 */
						ft_lock_ctx_init(&hctx, &d,
							ic.txn, ic.op);
						if (!ft_lock_ctx_depth_of(ft,
								&hctx,
								holder_flag,
								&hd) ||
							ft_acquire_member(ft,
								&hctx,
								holder_flag,
								hm, hd, &hh)) {
							FT_DBG_UNIQUE_ACQFAIL(
								unique_node_ret, hm,
								dup_head);
							ret = -EAGAIN;
							goto insert_done;
						}
						/* Held already: no release owed. */
						if (!hh.shared) {
							dup_hmeta = hh.lock;
							dup_hsnap = hh.lock_snap;
						}
					}
					/*
					 * AND RE-VALIDATE THE
					 * DERIVATION UNDER THE LOCK.
					 * The bail above catches a
					 * retire that was already
					 * done at the resolve.  This
					 * check catches the rest.  A
					 * peer can retire @dup_head
					 * between that resolve and
					 * the moment this acquire
					 * lands.  The walk below
					 * would then start from a
					 * RETIRED head and append
					 * behind a frozen next (the
					 * chain the key no longer
					 * names).  Once we hold the
					 * holder, the chain is
					 * stable, so one check here
					 * is enough.  The head must
					 * still be live AND still
					 * answer with the holder we
					 * took.  Same disposition and
					 * same reason as the remove
					 * side's ft_unchain_kind
					 * re-validation.
					 */
					{
						bool stale__ =
							ft_node_is_removed(dup_head) ||
							ft_chain_head_holder(ft,
								dup_head) !=
							holder_flag;

						/* Decidable here, and only here. */
						FT_DBG_UNIQUE(stale__,
							unique_node_ret);
						if (caa_unlikely(stale__)) {
							if (dup_hmeta)
								ft_meta_lock_release(
									dup_hmeta);
							ret = -EAGAIN;
							goto insert_done;
						}
					}
				}
			}
			/*
			 * ★ THE -EEXIST VERDICT, TAKEN UNDER THE LOCK.  This
			 * is the external-chain twin of the prefix-head arm
			 * above.  It moved for the same reason and carries
			 * the same proof.  @dup_head was the DESCENT's
			 * landing, taken with nothing held.  The re-validate
			 * just above establishes that it is neither retired
			 * nor re-homed.  It also re-derives the holder.  The
			 * prefix arm does not need that: there the holder is
			 * @d.nf itself.  Below the re-validate, the holder
			 * lock covers the verdict.  Above it, the verdict
			 * was a guess.
			 */
			if (unique_node_ret) {
				*unique_node_ret = dup_head;
				ret = -EEXIST;
				if (dup_hmeta)
					ft_meta_lock_release(dup_hmeta);
				goto insert_done;
			}
			/* Find last duplicate */
			iter_node = dup_head;
			cds_ft_for_each_duplicate(iter_node)
				last_node = iter_node;

			dbg_printf("cds_ft_insert duplicate external ppnf %p pnf %p nfp %p nf %p\n",
					d.ppnf, d.pnf, d.nfp, d.nf);

			/* Adding duplicate at existing key: no key count change. */
			ret = ft_chain_node(ft, last_node, node);
			if (dup_hmeta)
				ft_meta_lock_release(dup_hmeta);
			if (ret)
				goto insert_done;
		}
	} else {
		/* Found NULL node or external node before end of key. */

		/*
		 * If the last node encountered during traversal is an
		 * external node:
		 *  - transform this external node into an internal node
		 *    with associated external node;
		 *  - attach a new cluster as child of this internal node;
		 *  - populate this new internal node into the trie to
		 *    replace the prior external node.
		 * It's the same for NULL node, only that there is no need to
		 * chain any external node.
		 */

		dbg_printf("cds_ft_insert NULL or external ppnf %p pnf %p nfp %p nf %p\n",
				d.ppnf, d.pnf, d.nfp, d.nf);

		ft_lock_ctx_init(&actx, &d, ic.txn, ic.op);
	ic.ctx = &actx;
		ret = ft_attach_node(ft, d.pnfp, d.pnf,
				d.nfp, d.nf, key, key_len, d.depth, node,
				(struct cds_ft_node *) ft_node_ptr(d.nf), &ic,
				&actx);
		if (ret == 0) {
			/* One-commit: +1 count folds into the commit (see above). */
			assert(ic.slot || !ft->rank_stats);
			if (d.depth >= 2)
				FT_TP(tree_edge_set, (const void *) ft,
					(const void *) d.ppnf,
					(unsigned int) (d.depth - 2),
					(uint8_t) key[d.depth - 2],
					(const void *) *d.pnfp);
		}
	}

insert_done:
	/*
	 * Concurrent-writer pre-commit conflict (-EAGAIN).  The build found
	 * that a peer writer moved its descended position.  It bailed BEFORE
	 * arming or committing anything, and freed its own fresh cluster.
	 * @precell is untouched (reused); re-descend against the now-current
	 * tree.
	 */
	if (ret == -EAGAIN) {
		if (ic.txn) {
			ft_flip_txn_destroy(ic.txn);
			ic.txn = NULL;
		}
		/*
		 * Age the pre-commit conflict (ft_op_conflict).  Without
		 * it, a writer that keeps bailing on the same hot slot never
		 * advances txn->retry and never escalates.  It can livelock.
		 * Aging is all it does here.  The bail forfeits the FIFO
		 * turn, because the re-descend below asks a peer for the
		 * position it just lost.
		 */
		ft_txn_attempt_bail(&optxn, true);
		goto restart_attempt;
	}
	/*
	 * @node became a fresh head iff node->prev is still its (cell)
	 * carrier, i.e. not external.  A duplicate append or a failed insert
	 * leaves @precell orphaned: free it.  (In a duplicate append,
	 * ft_chain_node repointed node->prev at the predecessor.)  On
	 * failure, also restore node->prev to its zeroed state so the
	 * application may retry.  Otherwise splice the kept cell into the
	 * ordered list.
	 *
	 * List off: no cell was allocated.  @node->prev is the flagged
	 * parent (fresh head) or the predecessor (dup).  So there is nothing
	 * to free or splice.  A FAILED insert may still have wired
	 * node->prev early.  The build paths set the raw parent before their
	 * fallible publish (e.g. ft_try_compress_chain -> ft_set_parent, the
	 * split branch builders).  The failure unwind frees that cluster.
	 * So reset node->prev: the dangling pointer must not leak into a
	 * retry.  Otherwise the zeroed-prev check at entry would reject the
	 * node with -EINVAL forever.
	 */
	if (!ft->ordered_list) {
		if (ret != 0) {
			node->prev = NULL;
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ic.slot) {
			/*
			 * One-commit: the structural slot publish was recorded
			 * into ic.txn (the key is invisible -- the slot reads
			 * its old value).  No cell to splice (list off), so
			 * commit the structural edges alone (@cell == NULL); a
			 * reader flips from not-present to the new key
			 * atomically.  The +1 count folds into that commit.
			 */
			cst = ft_insert_one_commit(ft, _key, _key_len, NULL, &ic);
			assert(ic.count_folded || !ft->rank_stats);
		} else if (ic.txn) {
			/* Armed but nothing parked (duplicate append): no
			 * structural publish to commit. */
			ft_flip_txn_destroy(ic.txn);
			ic.txn = NULL;
		}
	} else {
		if (ret != 0) {
			node->prev = NULL;
			ft_ord_cell_free_unpublished(ft, precell);
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ft_node_external((struct cds_ft_inode_flag *) node->prev)) {
			ft_ord_cell_free_unpublished(ft, precell);
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ic.slot) {
			/*
			 * One-commit: the structural slot is parked as a flip
			 * proxy (the key is still invisible).  Splice position
			 * + cell links first.  Then ONE commit flips the
			 * structural slot AND the ordered-list edges.  So a
			 * reader never sees the head without its cell in the
			 * list.  The +1 count (the key only now counts) folds
			 * into that commit, from the shape's recorded base.
			 */
			cst = ft_insert_one_commit(ft, _key, _key_len, precell, &ic);
			assert(ic.count_folded || !ft->rank_stats);
		}
		/*
		 * No final else.  After the one-commit conversion, every
		 * published fresh head here parks a structural slot
		 * (ic.slot).  So the old B-lite (ic.spliced) and
		 * post-publish-splice fallbacks are unreachable.
		 * ft_insert_one_commit above always splices the cell
		 * atomically.
		 */
	}
	/*
	 * Concurrent-writer commit conflict.  At commit time, a peer writer
	 * won the forward slot's expected-value CAS (or froze a
	 * §4.B-guarded node).  So nothing published, and the txn's on-abort
	 * action rolled the fresh cluster back.  @precell was not spliced
	 * (the commit is atomic); re-descend and retry.
	 */
	if (cst == URCU_TXN_STATUS_ABORT) {
		/*
		 * A BAIL, AND IT AGES THE OP.  The content txn is standalone
		 * (ft_insert_commit_arm).  So its commit aged its own handle,
		 * never @optxn.  Restarting on a bare end left the op at the
		 * same age however often it lost.  An insert that kept losing
		 * its commit then never escalated into the lane.  Every retry
		 * edge of a loop that commits through a per-attempt txn is a
		 * bail (ft_txn_attempt_bail).
		 */
		FT_OP_LANE_STAT(cabort);
#ifdef FT_DEBUG_INSERT_ABORT_UNAGED
		ft_op_end(&optxn);
#else
		ft_txn_attempt_bail(&optxn, true);
#endif
		goto restart_attempt;
	}
	if (caa_unlikely(cst == URCU_TXN_STATUS_MEMORY_ERROR && ret == 0)) {
		/*
		 * Unreachable today: the one-commit txn is pre-reserved, so
		 * its commit is infallible ("reserved => infallible").
		 * Defensive: should that invariant ever break, the failed
		 * commit published NOTHING (freeze-before-install; its
		 * on-abort action already freed the fresh cluster).  Falling
		 * through with ret == 0 would then silently LOSE the insert.
		 * Unwind like a failed attempt and surface the error instead.
		 */
		node->prev = NULL;
		if (ft->ordered_list)
			ft_ord_cell_free_unpublished(ft, precell);
		ret = -ENOMEM;
	}
	/*
	 * Terminal outcome (success or error): close this attempt's read-side
	 * section and release the escalation turn if held.
	 */
	ft_op_end(&optxn);
	if (ret == 0) {
		if (key_len > uatomic_load(&ft->max_used_key_len, CMM_RELAXED))
			uatomic_store(&ft->max_used_key_len, key_len, CMM_RELAXED);
	}

	return ret;
}

enum cds_ft_status cds_ft_insert(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct cds_ft_node *node)
{
	int ret;

	CDS_FT_SCOPED_WRITER(ft);
	FT_TP_KEY(insert_enter, ft, key, key_len);
	ret = _cds_ft_insert(ft, key, key_len, node, NULL);

	if (ret == 0) {
		FT_TP(insert_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}
	if (ret == -EINVAL) {
		FT_TP(insert_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	FT_TP(insert_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
	return CDS_FT_STATUS_MEMORY_ERROR;
}

enum cds_ft_status cds_ft_insert_unique(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct cds_ft_node *node,
		struct cds_ft_node **result_node)
{
	int ret;
	struct cds_ft_node *ret_node = NULL;

	CDS_FT_SCOPED_WRITER(ft);
	FT_TP_KEY(insert_unique_enter, ft, key, key_len);
	ret = _cds_ft_insert(ft, key, key_len, node, &ret_node);
	if (ret == -EEXIST) {
		*result_node = ret_node;
		FT_TP(insert_unique_exit, (int) CDS_FT_STATUS_DUPLICATE_FOUND);
		return CDS_FT_STATUS_DUPLICATE_FOUND;
	}
	if (ret == -EINVAL) {
		*result_node = NULL;
		FT_TP(insert_unique_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (ret) {
		*result_node = NULL;
		FT_TP(insert_unique_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	*result_node = node;
	FT_TP(insert_unique_exit, (int) CDS_FT_STATUS_OK);
	return CDS_FT_STATUS_OK;
}

/*
 * The leaf replace's reader-visible edges:
 *  - the forward edge at @nfp; or
 *  - for a leaf reached through a SKIP_X suffix, the grandparent's dual slot
 *    and cn->child.
 * Called AFTER the arm's acquires ({P}, and GP by ft_lock_skip_dual_gp).  It
 * uses the derivation that acquire used.  So the dual names the grandparent
 * that the lock set holds, not the one seen by a plan made before the acquire.
 */
/*
 * THE LEAF SLOT, RE-READ UNDER THE LOCK.  @nf is the DESCENT's sample of
 * *@nfp, taken holding nothing.  ft_insert_replace_leaf_sedges plans the
 * forward edge {@nf -> node} from it.  It names @d.pnf, held by now, as the
 * edge's owner.  A held owner records SW (__ft_flip_txn_record_tag_ctx's
 * @held_sw): a plain store with no expected-old check.  The install CAS used
 * to reject a peer's republish of this slot between the descent and the
 * acquire.  That CAS is gone.  So the plan has to be the slot's value UNDER
 * the lock.  The order is derive, acquire, RE-VALIDATE.  ft_attach_node's
 * occupied-slot arm does exactly the same.
 *
 * MEASURED without it (SW on held owners), two failures:
 *  - This was the SW stale-old audit's only site on a non-in-place ft_inv
 *    leg: 742 (per-node) / 473 (exponential) records whose expected-old was
 *    not the live child.
 *  - inv_concurrent_insert_replace_nolist WEDGED at root-only.  A peer
 *    replace looped in ft_anchor_descend over the structure the blind store
 *    left.
 *
 * Identity only (ft_node_ptr_raw): a parked flip proxy compares unequal and
 * retries, which is the conservative answer.
 */
static inline
bool ft_insert_replace_leaf_plan_ok(const struct cds_ft *ft,
		struct cds_ft_inode_flag **nfp, struct cds_ft_inode_flag *nf,
		bool head_prefix)
{
	if (ft_node_ptr_raw(rcu_dereference(*nfp)) != ft_node_ptr_raw(nf))
		return false;
#ifndef FT_DEBUG_IR_HEAD_PREFIX_UNCHECKED
	/*
	 * THE SLOT'S VALUE IS NOT THE HEAD'S SHAPE.  The new head inherits
	 * the old one's prefix answer, read from its parent word at plan
	 * time with no lock held.  A peer can split the old head into a
	 * fresh node's prefix key and then promote it back.  That leaves the
	 * SAME head in the SAME slot, so the compare above passes.  But the
	 * answer this op read in between was the split's.  LTTng-traced: the
	 * new head published into a body slot carrying prefix-head=1, which
	 * cds_ft_verify refuses.  The holder is held here, so ask again.
	 */
	if (ft_head_is_prefix(ft, (struct cds_ft_node *) ft_node_ptr(nf)) !=
			head_prefix)
		return false;
#endif
	return true;
}

/*
 * Return the displaced head's cell, for insert_replace's ordered-list arms.
 * Read it UNDER the holder's lock, never at plan time.
 *
 * A peer promoting @head (removing the head before it in its chain) parks a
 * flip proxy on @head->prev for the length of its commit.  The past bug, with
 * a raw read at plan time:
 *  - ft_ord_cell_ptr() masked the proxy's tag and returned DESCRIPTOR memory
 *    as a cell;
 *  - the plan's re-checks passed once the peer committed;
 *  - the cell lock take then followed that "cell"'s neighbours into unmapped
 *    memory (10 of 10 cores of inv_prefix_shape_zoo: an old_cell ending in
 *    0xe).
 * Every producer of a parked prev holds the chain holder.  The chain holder
 * is held here, so this read is settled.  Resolve it anyway, as
 * cds_ft_replace does.
 *
 * Return NULL when @head's prev names a predecessor: it is not a head any
 * more, and the caller re-plans.
 */
static inline
struct ft_ord_cell *ft_insert_replace_head_cell(struct cds_ft_node *head)
{
	void *prev = ft_dereference_prev_resolved(head);

	if (ft_node_external((struct cds_ft_inode_flag *) prev))
		return NULL;
	return ft_ord_cell_ptr(prev);
}

static
unsigned int ft_insert_replace_leaf_sedges(struct cds_ft *ft,
		struct urcu_txn *mtxn, struct cds_ft_inode_flag *pnf,
		struct cds_ft_inode_flag **nfp, struct cds_ft_inode_flag *nf,
		struct cds_ft_node *node, struct ft_ord_cell_edge *sedges,
		bool gp_held)
{
	/*
	 * ☞ NAME THE OWNER OF EVERY EDGE.  These were the last DUAL_UNNAMED
	 * records in the tree.  This producer alone filed 338,783 / 346,689 /
	 * 324,799 per ft_inv leg at the three spacings.  The class legend
	 * says what that means: "owner UNNAMED (NULL) ... PLUMBING, not a
	 * lock."  The owners were already derived HERE, but only inside the
	 * debug-only FT_IR_SEDGE_LANE_ASK below.  Production filed NULL and
	 * took the hardcoded-MW arm.
	 *
	 * ☞ AND NAMING IS SAFE BY CONSTRUCTION: ft_flip_txn_record_tag routes
	 * to the per-record gate, which RE-ASKS ft_flip_txn_owns.  A named
	 * owner the op does not hold answers MW, exactly as today; only a held
	 * one parks.  The producer's @owner_held selects the route, it does not
	 * decide the kind.
	 *
	 * ☞ AND IT MAKES THIS SITE AGREE WITH THE SLOT'S OTHER WRITERS.  The
	 * dual's other producers already record it owner-keyed
	 * (ft_node_recompact's dual holds its owner 490,411 of 490,411).  So
	 * THIS site's unconditional MW was the odd kind on a slot the others
	 * park.
	 */
	sedges[0].slot = (struct ft_ord_cell **) nfp;
	sedges[0].old_target = (struct ft_ord_cell *) nf;
	sedges[0].new_target = (struct ft_ord_cell *) node;
	/* @nfp is a slot in @pnf's body: {P}, taken just above this call. */
	sedges[0].owner = ft_flag_to_metadata(ft, pnf);
	sedges[0].owner_held = sedges[0].owner != NULL;

#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_compressed(pnf)) {
		struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(pnf);
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
		struct cds_ft_inode_flag **sslot =
			ft_txn_parent_slot(cn_meta, ft, mtxn);

		FT_DUAL_SLOT_SETTLED_CHECK(sslot, gp_held);
		if (sslot && ft_node_skip_compressed(*sslot)) {
			/*
			 * ☠ BOTH EXPECTED-OLDS ARE RESOLVED, and both used to
			 * be RAW loads.  The acquires do not make a raw load
			 * safe:
			 *  - a MISS still records (the commit discards it);
			 *  - a coarse trie or a root dual holds nothing
			 *    here.
			 * A same-key peer's commit can then have a flip proxy
			 * PARKED on either word at the instant it is read.
			 * Handing that descriptor to the engine as an
			 * expected-old trips its own
			 * raw-read-of-a-parked-slot self-check.  Measured:
			 * `urcu_txn_add: Assertion
			 * `!urcu_txn_is_proxy(old_ptr, tag)' failed` (tag 15,
			 * kind MW), reproducible in ~2.5 min by
			 * inv_concurrent_insert_replace_nolist.
			 *
			 * ☠ AND A RELEASE BUILD HAS NO SUCH ASSERT.  The op
			 * would POISON the descriptor and its own retry loop
			 * would absorb it.  So the arm reads green and is
			 * wrong.  Resolved, each value is the word before or
			 * after that peer's commit, never a descriptor.  A
			 * STALE-but-valid one simply fails this MW edge's
			 * install CAS.  That is an ABORT, and the retry loop
			 * re-derives from it.  cds_ft_remove's cell capture
			 * and ft_graft's publish slot follow the same
			 * discipline.
			 */
			/*
			 * Edge 0 MOVES: its slot is now the SKIP_X dual in
			 * the GRANDPARENT's body.  So its owner is cn's
			 * parent, not @pnf.  @gp_held is what
			 * ft_lock_skip_dual_gp answered for exactly that
			 * node.
			 */
			sedges[0].owner = ft_owner_of_parent_word(ft,
				cn_meta->parent_word);
			sedges[0].owner_held = gp_held && sedges[0].owner;
			sedges[0].slot = (struct ft_ord_cell **) sslot;
			sedges[0].old_target = (struct ft_ord_cell *)
				ft_resolve_flip_proxy(*sslot);
			sedges[0].new_target = (struct ft_ord_cell *)
				ft_skip_compressed_flag(
					(struct cds_ft_inode_flag *) node,
					cn->len);
			/* edge 1: cn->child forward (exact descent) -- the word
			 * lives in cn, which IS @pnf: {P}. */
			sedges[1].owner = cn_meta;
			sedges[1].owner_held = true;
			sedges[1].slot = (struct ft_ord_cell **) &cn->child;
			sedges[1].old_target = (struct ft_ord_cell *)
				ft_resolve_flip_proxy(cn->child);
			sedges[1].new_target = (struct ft_ord_cell *) node;
			return 2;
		}
	}
#else
	(void) ft; (void) mtxn; (void) pnf;
#endif
	return 1;
}

#ifdef FT_DEBUG_STRUCT_ANCHOR
/*
 * Owners of the leaf replace's structural edges, filed as DUAL_UNNAMED:
 *  - edge 0 lives in @pnf or, in the skip variant, in cn's parent;
 *  - edge 1 lives in cn (= @pnf).
 * A macro so each arm keeps its own call-site pc.
 */
#define FT_IR_SEDGE_LANE_ASK(ft, txn, actx, pnf, n_sedge)		\
	do {								\
		struct cds_ft_metadata *pm__ =				\
			ft_flag_to_metadata(ft, pnf);			\
									\
		if ((n_sedge) == 1) {					\
			ft_sa_lane_ask(txn, actx, pm__, NULL);		\
		} else {						\
			ft_sa_lane_ask(txn, actx,			\
				ft_sa_owner_of_parent_word(ft,		\
					rcu_dereference(pm__->parent_word)), \
				NULL);					\
			ft_sa_lane_ask(txn, actx, pm__, NULL);		\
		}							\
	} while (0)
#endif

/*
 * Insert a node, replacing the entire existing duplicate chain at the
 * same key if one exists.
 *
 * On success, *@old_node_ret is set to the head of the replaced chain
 * (or NULL if no prior node existed). The caller must wait for a grace
 * period before reclaiming the old chain.
 *
 * Returns 0 on success, -EINVAL on bad arguments, or a negative errno
 * on memory allocation failure.
 */
#ifdef FT_DEBUG_IR_HOLDER
/*
 * PROBE (B2): does insert_replace LOCK the word the chain's other writers lock?
 * Appenders and removers take the node named by the HEAD's back-pointer
 * (ft_chain_head_holder / ft_node_holder); these arms take the DESCENT's node.
 * Compared at the under-lock check, i.e. while the lock is held.
 */
static unsigned long ft_irh_n[4], ft_irh_mismatch[4], ft_irh_kind[4][3][3];
/* On a mismatch: [locked retired][head-holder retired][slot still names head] */
static unsigned long ft_irh_cls[4][2][2][3], ft_irh_live_miss;

static inline unsigned int ft_irh_k(const struct cds_ft_inode_flag *nf)
{
	if (ft_node_skip_compressed((struct cds_ft_inode_flag *) nf))
		return 2;
	if (ft_node_compressed((struct cds_ft_inode_flag *) nf))
		return 1;
	return 0;
}

static inline void ft_irh_probe(struct cds_ft *ft, unsigned int arm,
		struct cds_ft_inode_flag *locked, struct cds_ft_node *displaced,
		struct cds_ft_inode_flag **slot, const struct ft_flip_txn *t)
{
	struct cds_ft_inode_flag *hh = ft_chain_head_holder(ft, displaced);

	uatomic_inc(&ft_irh_n[arm]);
	if (!hh || !locked)
		return;
	if (ft_flag_to_metadata(ft, locked) == ft_flag_to_metadata(ft, hh))
		return;
	uatomic_inc(&ft_irh_mismatch[arm]);
	uatomic_inc(&ft_irh_kind[arm][ft_irh_k(locked)][ft_irh_k(hh)]);
	{
		unsigned int lr = !!ft_flag_tombstoned(ft, locked);
		unsigned int hr = !!ft_flag_tombstoned(ft, hh);
		unsigned int sn = 2;	/* no slot to ask */

		if (slot)
			sn = (struct cds_ft_node *) ft_node_ptr(
				ft_resolve_flip_proxy(CMM_LOAD_SHARED(*slot)))
				== displaced;
		uatomic_inc(&ft_irh_cls[arm][lr][hr][sn]);
		if (!lr && !hr) {
			static unsigned long printed;

			if (t && t->acquire_miss)
				uatomic_inc(&ft_irh_live_miss);
			if (uatomic_add_return(&printed, 1) <= 6)
				fprintf(stderr, "FT IR HOLDER LIVE/LIVE: head %p prev raw %p | locked X %p state %#lx | head-holder Y %p state %#lx | X slot raw %p | txn acquire_miss=%d\n",
					(void *) displaced,
					CMM_LOAD_SHARED(displaced->prev),
					(void *) locked,
					(unsigned long) CMM_LOAD_SHARED(
						ft_flag_to_metadata(ft, locked)->state),
					(void *) hh,
					(unsigned long) CMM_LOAD_SHARED(
						ft_flag_to_metadata(ft, hh)->state),
					slot ? (void *) CMM_LOAD_SHARED(*slot) : NULL,
					t ? (int) t->acquire_miss : -1);
		}
	}
}

static __attribute__((destructor)) void ft_irh_report(void)
{
	static const char *kn[3] = { "internal", "compressed", "SKIP" };
	unsigned int a, x, y;

	for (a = 0; a < 4; a++) {
		fprintf(stderr, "FT IR HOLDER arm %u: %lu of %lu locked a DIFFERENT word than the head's back-pointer names (live/live with acquire_miss set: %lu, all arms)\n",
			a, ft_irh_mismatch[a], ft_irh_n[a], ft_irh_live_miss);
		for (x = 0; x < 3; x++)
			for (y = 0; y < 3; y++)
				if (ft_irh_kind[a][x][y])
					fprintf(stderr, "    locked %-10s vs head-holder %-10s: %lu\n",
						kn[x], kn[y], ft_irh_kind[a][x][y]);
		for (x = 0; x < 2; x++)
			for (y = 0; y < 2; y++)
				for (unsigned int z = 0; z < 3; z++)
					if (ft_irh_cls[a][x][y][z])
						fprintf(stderr, "    locked %s, head-holder %s, locked's slot %s: %lu\n",
							x ? "RETIRED" : "live",
							y ? "RETIRED" : "live",
							z == 2 ? "n/a" : z ? "STILL NAMES the head" : "no longer names it",
							ft_irh_cls[a][x][y][z]);
	}
}
# define FT_IRH_PROBE(ft, arm, locked, displaced, slot)			\
	ft_irh_probe((ft), (arm), (locked), (displaced), (slot), (txn))
#else
# define FT_IRH_PROBE(ft, arm, locked, displaced, slot)	do { } while (0)
#endif

static
int _cds_ft_insert_replace(struct cds_ft *ft,
		const uint8_t *_key, size_t _key_len,
		struct cds_ft_node *node,
		struct cds_ft_node **old_node_ret)
{
	unsigned int key_depth;
	struct ft_descent d;
	const uint8_t *iter_key;
	size_t key_len = ft_key_len(ft, _key_len);
	const struct cds_ft_key_map *km = &ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *key;
	struct cds_ft_inode_flag *snapshot[FT_MAX_DEPTH];
	unsigned int snapshot_depth[FT_MAX_DEPTH];
	int nr_snapshot = 0;
	/* Start at 0.  The retry tick at the loop head reads @ret as the
	 * outcome of the PREVIOUS attempt, and the first attempt has none. */
	int ret = 0;
	struct ft_ord_cell *precell;
	void *cell = NULL;		/* @precell's carrier; reused across retries */
	struct ft_op optxn;
	/*
	 * ★ Retry accounting for the loop at restart_replace_attempt.  That
	 * loop used to carry no ft_op_retry_tick, so nothing ever reported a
	 * livelock in it.  An attempt that could never converge was an
	 * INVISIBLE SPIN: no cap, no violation event and no descriptor dump.
	 * The suite simply never finished.  Every other retrying point op has
	 * a tick: cds_ft_insert, cds_ft_remove, cds_ft_replace and
	 * cds_ft_remove_all.
	 */
	struct ft_op_retry op_retry;
	/*
	 * ☠ WHAT THIS OP DISPLACED.  It is NOT the same as @old_node_ret.
	 * The compressed key-shorter arm sets @old_node_ret to a chain it left
	 * IN THE TRIE: it reports the key as already present and leaves @node
	 * uninstalled.  Tombstoning @old_node_ret blindly would freeze that
	 * LIVE chain.  Only the two arms that unlink a chain record it here.
	 */
	struct cds_ft_node *displaced = NULL;
	/* The displaced head's prefix answer the new head inherits. */
	bool head_prefix = false;
	struct ft_insert_commit ic = { 0 };
	/*
	 * The recompactions of the attach lock {C, P, GP}.  @d dates them and
	 * @ic.txn names what this op already holds.  @actx is initialised again
	 * at each attach, because the txn is armed part-way through.
	 */
	struct ft_lock_ctx actx;
	enum urcu_txn_status cst = URCU_TXN_STATUS_OK;	/* one-commit outcome */

	*old_node_ret = NULL;
	ft_op_retry_init(&op_retry, FT_OP_INSERT_REPLACE, _key, key_len);

	if (!valid_external_node(node) || !valid_key_len(ft, key_len))
		return -EINVAL;
	if (caa_likely(km->identity)) {
		key = _key;
	} else {
		ft_key_to_ordinals(ordinal_buf, _key, key_len, km);
		key = ordinal_buf;
	}
	/* Expect zeroed prev/next pointers. */
	if (node->prev || ft_node_next(node))
		return -EINVAL;

	/* Ordered-list trie: pre-wire @node's cell (see _cds_ft_insert).  On
	 * success a replace always lands @node as the sole head, so the cell is
	 * kept.  The cell is freed below in two cases: the insert fails, or the
	 * key_shorter path finds the key and leaves @node uninstalled.
	 * List off: no cell. */
	precell = NULL;
	if (ft->ordered_list) {
		cell = ft_ord_cell_alloc(ft, node, NULL);

		if (!cell)
			return -ENOMEM;
		node->prev = cell;
		precell = ft_ord_cell_ptr(cell);
		/*
		 * Record the head's last edge byte for the up-walk key rebuild.
		 * The cell is the head's metadata record and @key is ordinal
		 * here.  So key[key_len - 1] is the byte the head hangs under.
		 * The up-walk ignores it when the head's parent is a compressed
		 * node: the key_bytes of that node already span the head's
		 * position.
		 */
		if (key_len)
			cds_ft_item_to_metadata(precell)->incoming_byte =
				(uint8_t) key[key_len - 1];
	}

	/*
	 * The op's persistent engine handle.  It is initialised ONCE, exactly
	 * as _cds_ft_insert does.  Contention aging, the FIFO escalation turn
	 * and the learned descriptor size then span every attempt (doc §11).
	 * @cell and @precell are allocated above and REUSED.  Only a committing
	 * attempt splices the cell, so a bailed attempt leaves it ours.
	 */
	ft_txn_op_init(ft, &optxn);

restart_replace_attempt:
	ft_op_retry_tick(ft, &op_retry, ret);
	ft_op_begin(&optxn);
	/*
	 * Per-attempt state.  A bailed attempt published nothing and its fresh
	 * cluster was already rolled back.  So re-arm @node's linkage and the
	 * commit scope.  Then re-descend from the root against the current
	 * tree.
	 */
	ic = (struct ft_insert_commit){ 0 };
	ic.op = &optxn;
	cst = URCU_TXN_STATUS_OK;
	ret = 0;
	nr_snapshot = 0;
	*old_node_ret = NULL;
	displaced = NULL;
	node->prev = cell;		/* NULL when the list is off */
	node->next = NULL;
	if (precell && key_len)
		cds_ft_item_to_metadata(precell)->incoming_byte =
			(uint8_t) key[key_len - 1];

	key_depth = key_len + 1;

	dbg_printf("_cds_ft_insert_replace attempt: node %p\n", node);
	iter_key = key;
	ft_descent_init(&d, ft);

	for (; d.depth < key_depth - 1; ) {
		uint8_t key_value;

#ifndef FT_DEBUG_IR_IGNORE_SKIP_CONFLICT
		/*
		 * Take the same re-descend as cds_ft_insert.  A reanchoring
		 * step landed shallower, because a peer chain-merge moved the
		 * encoded position up.  This left d.nf and d.depth at different
		 * levels.
		 *
		 * Carrying on used to mis-file the key.  A descent read a stale
		 * skip pointer (length 3) and the merged run behind it (length
		 * 4).  It arrived one level too deep and found "the key ends
		 * HERE".  It then parked a leaf key's node as the external of
		 * an internal node one byte shorter.  insert_replace reported
		 * OK with no prior entry, while the key's own node sat
		 * untouched one level down.  This was seen with the two-writer
		 * owned-key harness, exponential spacing, LTTng-traced.
		 */
		if (caa_unlikely(d.skip_conflict)) {
			ret = -EAGAIN;
			goto insert_replace_done;
		}
#endif
		if (!d.nf)
			break;
		/* Resolve skip-compressed pointer. */
		d.nf = ft_resolve_skip_compressed(ft, d.nf);
		if (ft_node_external(d.nf))
			break;
		if (ft_node_compressed(d.nf)) {
			enum ft_descent_action act;

			act = ft_insert_compressed(ft, &d, &iter_key,
				key, key_len, key_depth, node,
				NULL, snapshot, snapshot_depth,
				&nr_snapshot, &ret, &ic);
			if (act == FT_DESCENT_END)
				goto insert_replace_done;
			if (act == FT_DESCENT_BREAK)
				break;
			continue;
		}
		dbg_printf("_cds_ft_insert_replace iter ppnf %p pnf %p nfp %p nf %p\n",
				d.ppnf, d.pnf, d.nfp, d.nf);
		ft_snapshot_push(snapshot, snapshot_depth,
			nr_snapshot, d.nf, d.depth);
		key_value = *(iter_key++);
		ft_descent_step(ft, &d, key_value);
	}
#ifndef FT_DEBUG_IR_IGNORE_SKIP_CONFLICT
	/* A final reanchoring step that exited the loop: honour it here too. */
	if (caa_unlikely(d.skip_conflict)) {
		ret = -EAGAIN;
		goto insert_replace_done;
	}
#endif

	/*
	 * Resolve any skip-compressed pointer left in d.nf by a final
	 * traverse that exited the loop without re-entering the loop's
	 * resolve step.
	 */
	d.nf = ft_resolve_skip_compressed(ft, d.nf);

	if (d.depth == key_depth - 1) {
		/* Found either an internal, external node or NULL at end of key. */
		if (!d.nf) {
			/* No existing node. Regular attach. */
			dbg_printf("_cds_ft_insert_replace NULL at end of key\n");

			ft_lock_ctx_init(&actx, &d, ic.txn, ic.op);
	ic.ctx = &actx;
			ret = ft_attach_node(ft, d.pnfp, d.pnf,
					d.nfp, d.nf, key, key_len, d.depth, node,
					NULL, &ic, &actx);
			if (ret == 0) {
				/* Parked one-commit: +1 folds into the commit. */
				assert(ic.slot || !ft->rank_stats);
				if (d.depth >= 2)
					FT_TP(tree_edge_set, (const void *) ft,
						(const void *) d.ppnf,
						(unsigned int) (d.depth - 2),
						(uint8_t) key[d.depth - 2],
						(const void *) *d.pnfp);
			}
		} else if (ft_node_compressed(d.nf)) {
			/*
			 * Key ends at a compressed node's depth.
			 * Split: internal(external_nodes) + compressed(len-1).
			 */
			/*
			 * A key that ends inside a compressed path is always a
			 * NEW key, so key_shorter never answers -EEXIST (see
			 * its header).  There is no chain to replace here.
			 */
			ret = ft_insert_compressed_key_shorter(ft, &d, 0,
				node, old_node_ret, &ic);
			assert(ret != -EEXIST);
		} else if (!ft_node_external(d.nf)) {
			struct cds_ft_node *external_nodes;
			struct cds_ft_metadata *metadata;

			assert(!ft_node_compressed(d.nf));
			metadata = cds_ft_item_to_metadata(ft_node_ptr(d.nf));
			/*
			 * ☠ RESOLVED, NOT RAW.  A concurrent same-key insert_replace
			 * or head park parks a flip proxy in external_nodes for the
			 * length of its commit.  A raw read took that proxy
			 * as the "displaced chain".  ft_hlist_chain_len then
			 * walked descriptor memory, and the proxy was handed
			 * back as *@old_node_ret.  MEASURED: SIGSEGV at 0xf08
			 * in 3 of 3 runs of inv_prefix_shape_zoo restricted
			 * to insert_replace.
			 */
			external_nodes = ft_dereference_external(
				metadata->external_nodes);
			if (external_nodes) {
				dbg_printf("_cds_ft_insert_replace: replacing internal metadata chain %p\n",
						external_nodes);
				/* Replace existing chain: key count unchanged. */
				*old_node_ret = external_nodes;
				displaced = external_nodes;
				ft_external_head_set_parent(ft, node, d.nf, /*prefix=*/ true);
				node->next = NULL;
				/*
				 * Ordered list on: publish the new head into
				 * external_nodes AND swap its cell in ONE
				 * atomic flip.  external_nodes is a plain
				 * pointer slot.  Readers resolve a parked
				 * proxy via
				 * ft_dereference_external.
				 * List off: a plain publish, no cell.
				 */
				if (ft->ordered_list) {
#ifdef FT_DEBUG_IR_PLAN_CELL
					struct ft_ord_cell *old_cell =
						ft_ord_cell_ptr(external_nodes->prev);
#else
					struct ft_ord_cell *old_cell = NULL;
#endif
					struct ft_ord_cell_edge sedge = {
						.slot = (struct ft_ord_cell **)
							&metadata->external_nodes,
						.old_target = (struct ft_ord_cell *)
							external_nodes,
						.new_target = (struct ft_ord_cell *)
							node,
					};
					unsigned int nr_disp = displaced ?
						ft_hlist_chain_len(displaced) : 0;
					struct ft_flip_txn *txn =
						ft_flip_txn_create_bounded(ft,
						FT_ORD_CELL_SWAP_PUBLISH_MAX_EDGES + 1 +
						nr_disp * FT_HLIST_FREEZE_MAX_EDGES);

					/*
					 * The flip is the op's sole
					 * side-effect: the new head is
					 * fresh.  On OOM nothing is applied
					 * and the old chain is intact.  Do
					 * NOT free its cell.  The replace
					 * aborts; it can be retried.
					 *
					 * Arm a pre-reserved txn.  The §4.B
					 * VALIDATE guard on the LIVE holder
					 * @d.nf then rides the swap flip.  A
					 * concurrent remove that froze the
					 * holder aborts this replace.
					 * ft_ord_cell_swap_publish_multi
					 * then fuses the external_nodes
					 * publish with the head cell's swap,
					 * and commits it infallibly.
					 */
					if (!txn) {
						ret = -ENOMEM;
						goto insert_replace_done;
					}
					/*
					 * ★ ACQUIRE THE NEAREST ANCHOR
					 * LOCK.  Do not merely VALIDATE it.
					 *
					 * This op used to §4.B-guard the
					 * holder at all four of its publish
					 * sites.  It never took it.  Its
					 * converted siblings ACQUIRE it:
					 * - cds_ft_replace hoists the chain
					 *   holder above its routing
					 *   derivation (@9e62dc3b);
					 * - the INTERIOR unlink of
					 *   cds_ft_remove holds it too
					 *   (ft_chain_head_holder +
					 *   ft_acquire_member, dated
					 *   FT_DEPTH_FROM_DESCENT through
					 *   ft_lock_ctx_depth_of).
					 *
					 * ☠ A VALIDATOR AND A LOCKER ON ONE
					 * WORD DO NOT EXCLUDE EACH OTHER.
					 * A remove could relink the interior
					 * of a chain under the holder's lock
					 * while this op displaced the whole
					 * chain.  Two symptoms came of that:
					 * - a remove that spun forever on a
					 *   stale back-edge;
					 * - an append onto a chain that no
					 *   longer existed.
					 * Tombstoning the displaced chain is
					 * the interop invariant that
					 * ft-txn-hlist.h demands.  It makes
					 * those states DETECTABLE.  Holding
					 * this word prevents them.
					 *
					 * FT_DEPTH_FROM_DESCENT: the descent
					 * window dates the acquire, exactly
					 * as the plain insert's publish
					 * does.  Acquire BEFORE the record,
					 * never after.  The record asks
					 * ft_flip_txn_owns who owns the word
					 * it writes.  So the registry must
					 * already name the holder.
					 */
					ft_lock_ctx_init(&actx, &d, txn, &optxn);
					ft_flip_txn_lock_or_guard_parent(ft, txn, &actx, d.nf,
						FT_DEPTH_FROM_DESCENT);
					/*
					 * ☞ A MISSED TAKE ENDS THE ATTEMPT
					 * HERE, as in the leaf arms
					 * (dbc41961).  Carrying on would
					 * build the swap and the chain
					 * freeze into a txn that the commit
					 * is bound to discard.  The chain
					 * records of the freeze are SW,
					 * whoever holds the word.  So each
					 * one is a stale plan against the
					 * lock holder's own in-flight freeze
					 * of the same chain.  The audit
					 * reported it on
					 * inv_prefix_shape_zoo: the peer's
					 * record on the same slot SUCCEEDED.
					 * Nothing is recorded on @txn yet.
					 */
					if (caa_unlikely(txn->acquire_miss)) {
						(void) ft_flip_txn_commit(ft, txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					ft_replace_fault_arm_abort(txn);
					/*
					 * ☞ NAME THE HOLDER.  The holder
					 * owns this edge:
					 * - the edge writes the holder's own
					 *   external_nodes;
					 * - the holder is the word this arm
					 *   just took;
					 * - the displaced head is the edge's
					 *   expected-old, and it is checked
					 *   again under that lock below.
					 * Left unnamed, the edge replay
					 * recorded it as an ownerless dual,
					 * MW.  That was the largest producer
					 * of FT_DEBUG_MW_KEPT left on fine
					 * tries (DUAL_UNNAMED, ~233k per
					 * ft_inv MW run).  Named and held,
					 * the per-record gate parks it.
					 * -DFT_DEBUG_IR_PREFIX_UNNAMED
					 * leaves it unnamed.
					 */
#ifndef FT_DEBUG_IR_PREFIX_UNNAMED
					sedge.owner = metadata;
					sedge.owner_held = ft_flip_txn_owns(txn, metadata);
#endif
					/*
					 * On a peer-conflict ABORT the
					 * commit installs NOTHING: the old
					 * chain and its cell stay LIVE.
					 * Dropping the status here used to
					 * report the replace as OK and then
					 * free @old_cell.  That cell was
					 * live and still linked in the
					 * ordered list.  So surface -EAGAIN.
					 * The pre-commit test of the done
					 * handler then re-attempts and
					 * KEEPS @precell: nothing was
					 * published, so it is still ours.
					 * The head arms below do the same.
					 */
					/*
					 * ☑ FREEZE THE DISPLACED HEAD **IN
					 * THIS COMMIT**, while the holder is
					 * held.  Do not sweep it afterwards.
					 *
					 * The doc of ft_hlist_freeze_prepare
					 * gives the reason.  Folding the
					 * mark into the head op's structural
					 * flip-txn "makes the freeze and the
					 * anchor edge commit atomically -- a
					 * reader is never shown @node's head
					 * anchor promoted away while @node
					 * is still unmarked".
					 * cds_ft_replace has always done
					 * this for the one node it
					 * displaces.  This op displaces a
					 * whole chain and did neither.  It
					 * marked the chain with a BARE CAS,
					 * after the commit had already
					 * released the holder.
					 *
					 * ☠ THAT SWEEP WAS THE UNHELD
					 * PRODUCER.  cds_ft_node.next is
					 * owned by the chain HOLDER (the
					 * word-kind table,
					 * fractal-trie-internal.h).  An
					 * unheld bare CAS on an owned word
					 * is the "ownership OBSERVED, never
					 * TAKEN" case that the table
					 * forbids.  The failure sequence:
					 * - a del_prepare passes its entry
					 *   validate;
					 * - the bare CAS lands underneath it
					 *   and turns the loaded next into
					 *   MARK(NULL), the bare value 2;
					 * - the backward edge records slot
					 *   &((struct cds_ft_node *) 2)->prev;
					 * - the commit faults.
					 * Measured 8/8.  It is also what
					 * stands between
					 * cds_ft_node.next/.prev and the SW
					 * park that their [debt] row is
					 * destined for.
					 *
					 * ☑ THE WHOLE CHAIN, not the head
					 * only.  This comment used to read
					 * "the HEAD only: a chain is
					 * unbounded and a txn is not".  That
					 * premise is FALSE.
					 * ft_flip_txn_create_bounded takes a
					 * RUNTIME cap, so @nr_disp edges can
					 * be reserved.  The leaf and prefix
					 * arms of cds_ft_remove_all now
					 * reserve theirs the same way.
					 *
					 * ☠ WHAT FREEZING THE HEAD ALONE
					 * COST.  Every further member was
					 * left LIVE and UNREACHABLE, with no
					 * tombstone anywhere.  So its
					 * removal had to be INFERRED.  The
					 * "does my predecessor still point
					 * at me?" test of cds_ft_remove
					 * exists for exactly these nodes,
					 * and names this op as the reason.
					 * That inference is fallible.  It
					 * read @node->prev and @pred->next
					 * unheld.  It answered a TERMINAL
					 * NOT_FOUND for a live node that a
					 * peer was PROMOTING.  This leaked
					 * the node and livelocked the next
					 * insert of it (230,977,777 retries
					 * in one call).  MEASURED exposure
					 * before this change, on
					 * inv_concurrent_insert_replace_nolist:
					 * 233,577 displacements left 50,661
					 * members unmarked (21.7%).
					 *
					 * The DERIVATION is @nr_disp, and
					 * the walk is BOUND by it.  A
					 * duplicate appended since then
					 * tears the derived tail's NULL.
					 * This ABORTS the commit, and the
					 * retry derives again.  The
					 * head-only freeze would have marked
					 * the head and orphaned that fresh
					 * duplicate behind it.
					 */
					if (displaced) {
						ft_ch_audit(ft, txn, displaced);
						/*
						 * ★ VALIDATE THE PLAN UNDER
						 * THE LOCK (Mathieu,
						 * 2026-09-19).  @nr_disp
						 * was counted at the top of
						 * this block, BEFORE the
						 * acquires above.  That
						 * count had no exclusion.
						 * Until now the derived
						 * tail NULL of the freeze
						 * stood in for it.
						 * MEASURED at this arm's
						 * derivation site: 13,683
						 * (per-node) / 14,337
						 * (exponential) tail
						 * disagreements per ft_inv
						 * leg, the largest of them.
						 *
						 * The holder is held HERE.
						 * So ask the word, instead
						 * of letting the install
						 * CAS arbitrate it.  It is
						 * the same decision, taken
						 * before any record is
						 * filed.
						 * ☠ This SHRINKS the window
						 * the CAS arbitrates.  It
						 * does not replace the CAS.
						 * An UNDECIDED peer proxy
						 * that outlasts the
						 * patience window of
						 * urcu_txn_read still reads
						 * as NULL.  See the header
						 * of ft_hlist_chain_plan_ok.
						 *
						 * ☠ Nothing is live-stored
						 * yet on this arm:
						 * - the txn is pre-reserved
						 *   "BEFORE any live
						 *   store";
						 * - the sedges are records;
						 * - the cell flip below is
						 *   the first install, and
						 *   its own failure path is
						 *   this very bail.
						 * The retry re-descends
						 * against an untouched
						 * structure.
						 */
						FT_IRH_PROBE(ft, 0, d.nf, displaced, NULL);
#ifndef FT_DEBUG_NO_DISPLACED_HEAD_RECHECK
						/*
						 * ☠ THE DISPLACED HEAD IS
						 * A PLAN READ.  @displaced
						 * was read with nothing
						 * held.  The holder is held
						 * now.  The swap below
						 * records @displaced as the
						 * expected-old of
						 * external_nodes: a blind
						 * SW store.
						 *
						 * This is what went wrong.
						 * A same-key replace
						 * committed in between.  It
						 * installed its own head
						 * and froze this chain.
						 * The chain check below
						 * ACCEPTS a frozen tail by
						 * design.  So the swap
						 * overwrote the peer's
						 * head, and orphaned it
						 * and its ordered cell.
						 * MEASURED on
						 * inv_prefix_shape_zoo with
						 * four writers of
						 * insert_replace: the
						 * stale-old audit named the
						 * second freeze of one
						 * chain in 3 of 3 runs.
						 *
						 * So the holder must still
						 * name @displaced, and
						 * @displaced must be live.
						 */
						if (ft_dereference_external(
								metadata->external_nodes) !=
								displaced ||
						    ft_node_is_removed(displaced)) {
							FT_HLIST_PLAN_BAIL();
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
#endif
#ifndef FT_DEBUG_IR_PLAN_CELL
						/* Its cell, now that the holder is held. */
						old_cell = ft_insert_replace_head_cell(
							displaced);
						if (!old_cell) {
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
#endif
						if (!ft_hlist_chain_plan_ok(ft,
								ft_flip_txn_handle(txn),
								displaced, nr_disp)) {
							FT_HLIST_PLAN_BAIL();
							/*
							 * ☠ THE COMMIT IS
							 * ALSO THE CLEANUP,
							 * and this bail is
							 * PRE-commit.  @txn
							 * already carries:
							 * - the holder's
							 *   registered lock
							 *   (the acquire
							 *   above);
							 * - the sedge
							 *   records.
							 * The -EAGAIN arm of
							 * insert_replace_done
							 * destroys @ic.txn,
							 * a DIFFERENT
							 * handle.  Leaving
							 * without a terminal
							 * here leaks
							 * FT_STATE_LOCK on
							 * the holder.  Every
							 * peer is then
							 * refused it
							 * forever.
							 *
							 * MEASURED exactly
							 * that.  The first
							 * spelling of this
							 * bail wedged
							 * inv_concurrent_insert_replace_nolist
							 * past 120 s against
							 * a 4.7 s control.
							 * The ledger printed
							 * "FT REFUSED
							 * (LOCK, unknown holder)" in a loop.
							 *
							 * ft_flip_txn_destroy
							 * is the pre-commit
							 * terminal that
							 * drains the
							 * registry.
							 */
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
						ft_hlist_freeze_chain_prepare_checked(ft,
							ft_flip_txn_handle(txn),
							displaced, nr_disp);
					}
#ifdef FT_DEBUG_STRUCT_ANCHOR
					/* &metadata->external_nodes: the holder's own slot. */
					ft_sa_lane_ask(txn, &actx, metadata, NULL);
#endif
					if (ft_ord_cell_swap_publish_multi(ft, old_cell,
							precell, &sedge, 1, txn) != 0) {
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					ft_ord_cell_free(ft, old_cell);
				} else {
					/*
					 * List off replace: external_nodes
					 * is the single reader-visible slot.
					 * Readers resolve it via
					 * ft_dereference_external.
					 *
					 * Publish external_nodes from the
					 * old chain to @node on a guarded
					 * STANDALONE txn.  The §4.B VALIDATE
					 * guard on the LIVE holder @d.nf
					 * then rides the flip.  Do NOT use
					 * the park.  A replace changes no
					 * key count, so it must NOT enter
					 * the count-folding one-commit path.
					 * The callers of
					 * ft_insert_one_commit assert
					 * count_folded when rank stats are
					 * on.
					 *
					 * This mirrors the list-on chain
					 * replace above, minus the cell.
					 * Two edges: the forward
					 * external_nodes store and the
					 * guard.
					 */
					struct ft_ord_cell_edge sedge = {
						.slot = (struct ft_ord_cell **)
							&metadata->external_nodes,
						.old_target = (struct ft_ord_cell *)
							external_nodes,
						.new_target = (struct ft_ord_cell *)
							node,
					};
					unsigned int nr_disp = displaced ?
						ft_hlist_chain_len(displaced) : 0;
					struct ft_flip_txn *txn =
						ft_flip_txn_create_bounded(ft, 2 +
							nr_disp * FT_HLIST_FREEZE_MAX_EDGES);

					if (!txn) {
						ret = -ENOMEM;
						goto insert_replace_done;
					}
					/*
					 * ★ ACQUIRE THE NEAREST ANCHOR
					 * LOCK.  Do not merely VALIDATE it.
					 *
					 * This op used to §4.B-guard the
					 * holder at all four of its publish
					 * sites.  It never took it.  Its
					 * converted siblings ACQUIRE it:
					 * - cds_ft_replace hoists the chain
					 *   holder above its routing
					 *   derivation (@9e62dc3b);
					 * - the INTERIOR unlink of
					 *   cds_ft_remove holds it too
					 *   (ft_chain_head_holder +
					 *   ft_acquire_member, dated
					 *   FT_DEPTH_FROM_DESCENT through
					 *   ft_lock_ctx_depth_of).
					 *
					 * ☠ A VALIDATOR AND A LOCKER ON ONE
					 * WORD DO NOT EXCLUDE EACH OTHER.
					 * A remove could relink the interior
					 * of a chain under the holder's lock
					 * while this op displaced the whole
					 * chain.  Two symptoms came of that:
					 * - a remove that spun forever on a
					 *   stale back-edge;
					 * - an append onto a chain that no
					 *   longer existed.
					 * Tombstoning the displaced chain is
					 * the interop invariant that
					 * ft-txn-hlist.h demands.  It makes
					 * those states DETECTABLE.  Holding
					 * this word prevents them.
					 *
					 * FT_DEPTH_FROM_DESCENT: the descent
					 * window dates the acquire, exactly
					 * as the plain insert's publish
					 * does.  Acquire BEFORE the record,
					 * never after.  The record asks
					 * ft_flip_txn_owns who owns the word
					 * it writes.  So the registry must
					 * already name the holder.
					 */
					ft_lock_ctx_init(&actx, &d, txn, &optxn);
					ft_flip_txn_lock_or_guard_parent(ft, txn, &actx, d.nf,
						FT_DEPTH_FROM_DESCENT);
					/*
					 * ☞ A MISSED TAKE ENDS THE ATTEMPT
					 * HERE, as in the leaf arms
					 * (dbc41961).  Carrying on would
					 * build the swap and the chain
					 * freeze into a txn that the commit
					 * is bound to discard.  The chain
					 * records of the freeze are SW,
					 * whoever holds the word.  So each
					 * one is a stale plan against the
					 * lock holder's own in-flight freeze
					 * of the same chain.  The audit
					 * reported it on
					 * inv_prefix_shape_zoo: the peer's
					 * record on the same slot SUCCEEDED.
					 * Nothing is recorded on @txn yet.
					 */
					if (caa_unlikely(txn->acquire_miss)) {
						(void) ft_flip_txn_commit(ft, txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					ft_replace_fault_arm_abort(txn);
					/*
					 * ☞ NAME THE HOLDER.  The holder
					 * owns this edge:
					 * - the edge writes the holder's own
					 *   external_nodes;
					 * - the holder is the word this arm
					 *   just took;
					 * - the displaced head is the edge's
					 *   expected-old, and it is checked
					 *   again under that lock below.
					 * Left unnamed, the edge replay
					 * recorded it as an ownerless dual,
					 * MW.  That was the largest producer
					 * of FT_DEBUG_MW_KEPT left on fine
					 * tries (DUAL_UNNAMED, ~233k per
					 * ft_inv MW run).  Named and held,
					 * the per-record gate parks it.
					 * -DFT_DEBUG_IR_PREFIX_UNNAMED
					 * leaves it unnamed.
					 */
#ifndef FT_DEBUG_IR_PREFIX_UNNAMED
					sedge.owner = metadata;
					sedge.owner_held = ft_flip_txn_owns(txn, metadata);
#endif
					/*
					 * The flip installs nothing on
					 * either failure, as in the head
					 * arm:
					 * - ABORT gives -EAGAIN: retry;
					 * - MEMORY_ERROR gives -ENOMEM: do
					 *   not retry.
					 */
					/*
					 * ☑ FREEZE THE DISPLACED HEAD **IN
					 * THIS COMMIT**, while the holder is
					 * held.  Do not sweep it afterwards.
					 *
					 * The doc of ft_hlist_freeze_prepare
					 * gives the reason.  Folding the
					 * mark into the head op's structural
					 * flip-txn "makes the freeze and the
					 * anchor edge commit atomically -- a
					 * reader is never shown @node's head
					 * anchor promoted away while @node
					 * is still unmarked".
					 * cds_ft_replace has always done
					 * this for the one node it
					 * displaces.  This op displaces a
					 * whole chain and did neither.  It
					 * marked the chain with a BARE CAS,
					 * after the commit had already
					 * released the holder.
					 *
					 * ☠ THAT SWEEP WAS THE UNHELD
					 * PRODUCER.  cds_ft_node.next is
					 * owned by the chain HOLDER (the
					 * word-kind table,
					 * fractal-trie-internal.h).  An
					 * unheld bare CAS on an owned word
					 * is the "ownership OBSERVED, never
					 * TAKEN" case that the table
					 * forbids.  The failure sequence:
					 * - a del_prepare passes its entry
					 *   validate;
					 * - the bare CAS lands underneath it
					 *   and turns the loaded next into
					 *   MARK(NULL), the bare value 2;
					 * - the backward edge records slot
					 *   &((struct cds_ft_node *) 2)->prev;
					 * - the commit faults.
					 * Measured 8/8.  It is also what
					 * stands between
					 * cds_ft_node.next/.prev and the SW
					 * park that their [debt] row is
					 * destined for.
					 *
					 * ☑ THE WHOLE CHAIN, not the head
					 * only.  This comment used to read
					 * "the HEAD only: a chain is
					 * unbounded and a txn is not".  That
					 * premise is FALSE.
					 * ft_flip_txn_create_bounded takes a
					 * RUNTIME cap, so @nr_disp edges can
					 * be reserved.  The leaf and prefix
					 * arms of cds_ft_remove_all now
					 * reserve theirs the same way.
					 *
					 * ☠ WHAT FREEZING THE HEAD ALONE
					 * COST.  Every further member was
					 * left LIVE and UNREACHABLE, with no
					 * tombstone anywhere.  So its
					 * removal had to be INFERRED.  The
					 * "does my predecessor still point
					 * at me?" test of cds_ft_remove
					 * exists for exactly these nodes,
					 * and names this op as the reason.
					 * That inference is fallible.  It
					 * read @node->prev and @pred->next
					 * unheld.  It answered a TERMINAL
					 * NOT_FOUND for a live node that a
					 * peer was PROMOTING.  This leaked
					 * the node and livelocked the next
					 * insert of it (230,977,777 retries
					 * in one call).  MEASURED exposure
					 * before this change, on
					 * inv_concurrent_insert_replace_nolist:
					 * 233,577 displacements left 50,661
					 * members unmarked (21.7%).
					 *
					 * The DERIVATION is @nr_disp, and
					 * the walk is BOUND by it.  A
					 * duplicate appended since then
					 * tears the derived tail's NULL.
					 * This ABORTS the commit, and the
					 * retry derives again.  The
					 * head-only freeze would have marked
					 * the head and orphaned that fresh
					 * duplicate behind it.
					 */
					if (displaced) {
						ft_ch_audit(ft, txn, displaced);
						/*
						 * ★ VALIDATE THE PLAN UNDER
						 * THE LOCK (Mathieu,
						 * 2026-09-19).  @nr_disp
						 * was counted at the top of
						 * this block, BEFORE the
						 * acquires above.  That
						 * count had no exclusion.
						 * Until now the derived
						 * tail NULL of the freeze
						 * stood in for it.
						 * MEASURED at this arm's
						 * derivation site: 13,683
						 * (per-node) / 14,337
						 * (exponential) tail
						 * disagreements per ft_inv
						 * leg, the largest of them.
						 *
						 * The holder is held HERE.
						 * So ask the word, instead
						 * of letting the install
						 * CAS arbitrate it.  It is
						 * the same decision, taken
						 * before any record is
						 * filed.
						 * ☠ This SHRINKS the window
						 * the CAS arbitrates.  It
						 * does not replace the CAS.
						 * An UNDECIDED peer proxy
						 * that outlasts the
						 * patience window of
						 * urcu_txn_read still reads
						 * as NULL.  See the header
						 * of ft_hlist_chain_plan_ok.
						 *
						 * ☠ Nothing is live-stored
						 * yet on this arm:
						 * - the txn is pre-reserved
						 *   "BEFORE any live
						 *   store";
						 * - the sedges are records;
						 * - the cell flip below is
						 *   the first install, and
						 *   its own failure path is
						 *   this very bail.
						 * The retry re-descends
						 * against an untouched
						 * structure.
						 */
						FT_IRH_PROBE(ft, 1, d.nf, displaced, NULL);
#ifndef FT_DEBUG_NO_DISPLACED_HEAD_RECHECK
						/*
						 * ☠ THE DISPLACED HEAD IS
						 * A PLAN READ.  @displaced
						 * was read with nothing
						 * held.  The holder is held
						 * now.  The swap below
						 * records @displaced as the
						 * expected-old of
						 * external_nodes: a blind
						 * SW store.
						 *
						 * This is what went wrong.
						 * A same-key replace
						 * committed in between.  It
						 * installed its own head
						 * and froze this chain.
						 * The chain check below
						 * ACCEPTS a frozen tail by
						 * design.  So the swap
						 * overwrote the peer's
						 * head, and orphaned it
						 * and its ordered cell.
						 * MEASURED on
						 * inv_prefix_shape_zoo with
						 * four writers of
						 * insert_replace: the
						 * stale-old audit named the
						 * second freeze of one
						 * chain in 3 of 3 runs.
						 *
						 * So the holder must still
						 * name @displaced, and
						 * @displaced must be live.
						 */
						if (ft_dereference_external(
								metadata->external_nodes) !=
								displaced ||
						    ft_node_is_removed(displaced)) {
							FT_HLIST_PLAN_BAIL();
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
#endif
						if (!ft_hlist_chain_plan_ok(ft,
								ft_flip_txn_handle(txn),
								displaced, nr_disp)) {
							FT_HLIST_PLAN_BAIL();
							/*
							 * ☠ THE COMMIT IS
							 * ALSO THE CLEANUP,
							 * and this bail is
							 * PRE-commit.  @txn
							 * already carries:
							 * - the holder's
							 *   registered lock
							 *   (the acquire
							 *   above);
							 * - the sedge
							 *   records.
							 * The -EAGAIN arm of
							 * insert_replace_done
							 * destroys @ic.txn,
							 * a DIFFERENT
							 * handle.  Leaving
							 * without a terminal
							 * here leaks
							 * FT_STATE_LOCK on
							 * the holder.  Every
							 * peer is then
							 * refused it
							 * forever.
							 *
							 * MEASURED exactly
							 * that.  The first
							 * spelling of this
							 * bail wedged
							 * inv_concurrent_insert_replace_nolist
							 * past 120 s against
							 * a 4.7 s control.
							 * The ledger printed
							 * "FT REFUSED
							 * (LOCK, unknown holder)" in a loop.
							 *
							 * ft_flip_txn_destroy
							 * is the pre-commit
							 * terminal that
							 * drains the
							 * registry.
							 */
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
						ft_hlist_freeze_chain_prepare_checked(ft,
							ft_flip_txn_handle(txn),
							displaced, nr_disp);
					}
#ifdef FT_DEBUG_STRUCT_ANCHOR
					/* &metadata->external_nodes: the holder's own slot. */
					ft_sa_lane_ask(txn, &actx, metadata, NULL);
#endif
					ret = ft_flip_status_to_errno(
						ft_ord_cell_flip_into(ft, txn,
							&sedge, 1));
					if (ret)
						goto insert_replace_done;
				}
			} else {
				/* No external nodes yet. New key at this node. */
				ft_external_head_set_parent(ft, node, d.nf, /*prefix=*/ true);
				node->next = NULL;
				/*
				 * Park external_nodes.  Readers resolve the
				 * proxy via ft_dereference_external.  The
				 * park does two things:
				 * - the publish commits atomically with the
				 *   ordinal-cell splice (list on);
				 * - the §4.B VALIDATE guard on the LIVE
				 *   holder @metadata rides the flip
				 *   (ft_insert_park_external_nodes records
				 *   it).
				 * List off + rank off carries only the
				 * forward edge and that guard.  This is a
				 * 2-record slab-allocated MCAS commit, not
				 * a bare lone flip.  It is the multi-writer
				 * cost of guarding this in-place publish.
				 */
				ret = ft_insert_commit_arm(ft, &ic,
					ft->rank_stats ? d.depth + 2 : 0);
				if (ret)
					goto insert_replace_done;
				ret = ft_insert_park_external_nodes(ft, &d,
					metadata, node, NULL, &ic);
				if (ret)
					goto insert_replace_done;
				ic.count_from = d.nf;
				/* I1 (list on) / I2 (list off) count fold: see cds_ft_insert. */
				ic.count_folded = true;
			}
			ret = 0;
		} else {
			dbg_printf("_cds_ft_insert_replace: replacing external chain %p\n",
					ft_node_ptr(d.nf));
			/* External node at end of key. Replace chain: key count unchanged. */
			*old_node_ret = (struct cds_ft_node *) ft_node_ptr(d.nf);
			displaced = *old_node_ret;
			/*
			 * Replace the head of an EXTERNAL chain.  @node takes
			 * the old head's place, whichever shape that was.
			 * So INHERIT the old head's own answer.  Do not
			 * derive it again from @d.pnf mid-op
			 * (FT_PARENT_PREFIX_HEAD).
			 */
			head_prefix = ft_head_is_prefix(ft, displaced);
			ft_external_head_set_parent(ft, node, d.pnf, head_prefix);
			node->next = NULL;
			{
				/*
				 * Atomic replace: publish the new head into
				 * every reader-visible slot in ONE flip.
				 *
				 * The plain case is a single forward edge at
				 * d.nfp.  That is a plain external child, or
				 * the cn->child of a plainly-reached
				 * compressed parent.
				 *
				 * A leaf reached through a SKIP_X suffix is
				 * a DUAL.  BOTH of these slots name the
				 * leaf:
				 * - the exact descent reads cn->child;
				 * - the candidate descent reads the
				 *   grandparent skip slot.
				 * The new head must appear at both
				 * atomically.  Otherwise a reader sees them
				 * disagree, and the old leaf is freed after
				 * this.  So cn->child is recorded as a
				 * second flip edge, not as a separate bare
				 * store.  This fuses the pair.  It mirrors
				 * the remove external-promote dual
				 * (ft_remove_commit_rec).
				 *
				 * Both ordered-list states share this sedge
				 * model.  The cell swap rides the same flip
				 * when the list is on.
				 */
				struct ft_ord_cell_edge sedges[2] = { 0 };
				unsigned int n_sedge;
				bool gp_held;

				if (ft->ordered_list) {
					/*
					 * Swap the structural slot(s) AND
					 * the head's cell in one flip.
					 *
					 * MW: the forward edge stores into
					 * the child slot of the LIVE holder
					 * @d.pnf.  So guard it §4.B.  A
					 * {live->live} VALIDATE on @d.pnf
					 * rides the same flip.  A peer that
					 * relocated or froze @d.pnf then
					 * ABORTS this commit.  Otherwise the
					 * store would land in a stale or
					 * freed slot.  The grandparent slot
					 * of the SKIP dual resolves off
					 * @d.pnf, and the guard covers the
					 * liveness of @d.pnf.
					 *
					 * Pre-reserve the txn (guard +
					 * swap/publish edges) BEFORE any
					 * live store, so that the flip is
					 * OOM-infallible.  On create failure
					 * nothing is applied.  The old
					 * head's cell is NOT freed and the
					 * replace aborts; it can be retried.
					 */
#ifdef FT_DEBUG_IR_PLAN_CELL
					struct ft_ord_cell *old_cell =
						ft_ord_cell_ptr((*old_node_ret)->prev);
#else
					struct ft_ord_cell *old_cell = NULL;
#endif
					unsigned int nr_disp = displaced ?
						ft_hlist_chain_len(displaced) : 0;
					struct ft_flip_txn *txn =
						ft_flip_txn_create_bounded(ft,
							FT_ORD_CELL_SWAP_PUBLISH_MAX_EDGES +
							1 /* §4.B parent guard */ +
							1 /* §9.3 dual GP: release-lock XOR guard */ +
							nr_disp * FT_HLIST_FREEZE_MAX_EDGES);

					if (!txn) {
						ret = -ENOMEM;
						goto insert_replace_done;
					}
					/*
					 * ★ ACQUIRE THE NEAREST ANCHOR
					 * LOCK.  Do not merely VALIDATE it.
					 *
					 * This op used to §4.B-guard the
					 * holder at all four of its publish
					 * sites.  It never took it.  Its
					 * converted siblings ACQUIRE it:
					 * - cds_ft_replace hoists the chain
					 *   holder above its routing
					 *   derivation (@9e62dc3b);
					 * - the INTERIOR unlink of
					 *   cds_ft_remove holds it too
					 *   (ft_chain_head_holder +
					 *   ft_acquire_member, dated
					 *   FT_DEPTH_FROM_DESCENT through
					 *   ft_lock_ctx_depth_of).
					 *
					 * ☠ A VALIDATOR AND A LOCKER ON ONE
					 * WORD DO NOT EXCLUDE EACH OTHER.
					 * A remove could relink the interior
					 * of a chain under the holder's lock
					 * while this op displaced the whole
					 * chain.  Two symptoms came of that:
					 * - a remove that spun forever on a
					 *   stale back-edge;
					 * - an append onto a chain that no
					 *   longer existed.
					 * Tombstoning the displaced chain is
					 * the interop invariant that
					 * ft-txn-hlist.h demands.  It makes
					 * those states DETECTABLE.  Holding
					 * this word prevents them.
					 *
					 * FT_DEPTH_FROM_DESCENT: the descent
					 * window dates the acquire, exactly
					 * as the plain insert's publish
					 * does.  Acquire BEFORE the record,
					 * never after.  The record asks
					 * ft_flip_txn_owns who owns the word
					 * it writes.  So the registry must
					 * already name the holder.
					 */
					ft_lock_ctx_init(&actx, &d, txn, &optxn);
					ft_flip_txn_lock_or_guard_parent(ft, txn, &actx, d.pnf,
						FT_DEPTH_FROM_DESCENT);
					/*
					 * ☞ A MISSED TAKE ENDS THE ATTEMPT
					 * HERE.  The acquire latches
					 * @acquire_miss and returns.
					 * Carrying on would build the dual
					 * GP acquire, the sedges and the
					 * chain freeze into @txn.  But
					 * ft_flip_txn_commit is bound to
					 * DISCARD that txn: it tests the
					 * miss first and installs nothing.
					 * Measured: ~28k such attempts per
					 * ft_inv leg on the list-off leaf
					 * arm.  Every stale SW record the
					 * audit saw there was among them.
					 *
					 * So run that discard NOW.  It is
					 * the same call, with the same
					 * aging, trace and destroy.  It
					 * gives the same -EAGAIN that the
					 * flip below would return, only
					 * without the records.  Nothing has
					 * been recorded or stored on @txn
					 * yet: it was created just above.
					 */
					if (caa_unlikely(txn->acquire_miss)) {
						(void) ft_flip_txn_commit(ft, txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					/*
					 * §9.3's THIRD LOCK-SET MEMBER, as
					 * cds_ft_replace takes it.  Under a
					 * compressed @d.pnf the SKIP_X dual
					 * lives in the GRANDPARENT's body.
					 * {P} does not cover that body.
					 * -DFT_DEBUG_STRUCT_ANCHOR measured
					 * it: 15,729 (per-node) / 10,279
					 * (exponential) dual records per
					 * ft_inv leg committed with GP not
					 * held.  A GP recompact copies that
					 * slot and trusts its own lock.
					 *
					 * The edges are derived after both
					 * acquires, by
					 * ft_insert_replace_leaf_sedges.
					 * That NAMES this node as the owner
					 * of the dual edge, and passes the
					 * acquire's own answer as @gp_held.
					 * The per-record gate then decides
					 * the kind.
					 */
					gp_held = ft_lock_skip_dual_gp(ft, &actx, txn, d.pnf,
						txn->mtxn);
					/* The pre-commit terminal, as the chain-plan bail below. */
					if (!ft_insert_replace_leaf_plan_ok(ft, d.nfp,
							d.nf, head_prefix)) {
						ft_flip_txn_destroy(txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
#ifndef FT_DEBUG_IR_PLAN_CELL
					/* Its cell, now that the holder is held. */
					old_cell = ft_insert_replace_head_cell(
						*old_node_ret);
					if (!old_cell) {
						ft_flip_txn_destroy(txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
#endif
					n_sedge = ft_insert_replace_leaf_sedges(ft,
						txn->mtxn, d.pnf, d.nfp, d.nf, node, sedges,
						gp_held);
					ft_replace_fault_arm_abort(txn);
					/*
					 * On a peer-conflict ABORT the
					 * commit installs nothing: the old
					 * head and its cell stay live.  So
					 * surface -EAGAIN.  The pre-commit
					 * test of the done handler then
					 * re-attempts, and @precell is KEPT
					 * for the next attempt.  Do NOT free
					 * @old_cell: the swap did not
					 * happen.
					 */
					/*
					 * ☑ FREEZE THE DISPLACED HEAD **IN
					 * THIS COMMIT**, while the holder is
					 * held.  Do not sweep it afterwards.
					 *
					 * The doc of ft_hlist_freeze_prepare
					 * gives the reason.  Folding the
					 * mark into the head op's structural
					 * flip-txn "makes the freeze and the
					 * anchor edge commit atomically -- a
					 * reader is never shown @node's head
					 * anchor promoted away while @node
					 * is still unmarked".
					 * cds_ft_replace has always done
					 * this for the one node it
					 * displaces.  This op displaces a
					 * whole chain and did neither.  It
					 * marked the chain with a BARE CAS,
					 * after the commit had already
					 * released the holder.
					 *
					 * ☠ THAT SWEEP WAS THE UNHELD
					 * PRODUCER.  cds_ft_node.next is
					 * owned by the chain HOLDER (the
					 * word-kind table,
					 * fractal-trie-internal.h).  An
					 * unheld bare CAS on an owned word
					 * is the "ownership OBSERVED, never
					 * TAKEN" case that the table
					 * forbids.  The failure sequence:
					 * - a del_prepare passes its entry
					 *   validate;
					 * - the bare CAS lands underneath it
					 *   and turns the loaded next into
					 *   MARK(NULL), the bare value 2;
					 * - the backward edge records slot
					 *   &((struct cds_ft_node *) 2)->prev;
					 * - the commit faults.
					 * Measured 8/8.  It is also what
					 * stands between
					 * cds_ft_node.next/.prev and the SW
					 * park that their [debt] row is
					 * destined for.
					 *
					 * ☑ THE WHOLE CHAIN, not the head
					 * only.  This comment used to read
					 * "the HEAD only: a chain is
					 * unbounded and a txn is not".  That
					 * premise is FALSE.
					 * ft_flip_txn_create_bounded takes a
					 * RUNTIME cap, so @nr_disp edges can
					 * be reserved.  The leaf and prefix
					 * arms of cds_ft_remove_all now
					 * reserve theirs the same way.
					 *
					 * ☠ WHAT FREEZING THE HEAD ALONE
					 * COST.  Every further member was
					 * left LIVE and UNREACHABLE, with no
					 * tombstone anywhere.  So its
					 * removal had to be INFERRED.  The
					 * "does my predecessor still point
					 * at me?" test of cds_ft_remove
					 * exists for exactly these nodes,
					 * and names this op as the reason.
					 * That inference is fallible.  It
					 * read @node->prev and @pred->next
					 * unheld.  It answered a TERMINAL
					 * NOT_FOUND for a live node that a
					 * peer was PROMOTING.  This leaked
					 * the node and livelocked the next
					 * insert of it (230,977,777 retries
					 * in one call).  MEASURED exposure
					 * before this change, on
					 * inv_concurrent_insert_replace_nolist:
					 * 233,577 displacements left 50,661
					 * members unmarked (21.7%).
					 *
					 * The DERIVATION is @nr_disp, and
					 * the walk is BOUND by it.  A
					 * duplicate appended since then
					 * tears the derived tail's NULL.
					 * This ABORTS the commit, and the
					 * retry derives again.  The
					 * head-only freeze would have marked
					 * the head and orphaned that fresh
					 * duplicate behind it.
					 */
					if (displaced) {
						ft_ch_audit(ft, txn, displaced);
						/*
						 * ★ VALIDATE THE PLAN UNDER
						 * THE LOCK (Mathieu,
						 * 2026-09-19).  @nr_disp
						 * was counted at the top of
						 * this block, BEFORE the
						 * acquires above.  That
						 * count had no exclusion.
						 * Until now the derived
						 * tail NULL of the freeze
						 * stood in for it.
						 * MEASURED at this arm's
						 * derivation site: 13,683
						 * (per-node) / 14,337
						 * (exponential) tail
						 * disagreements per ft_inv
						 * leg, the largest of them.
						 *
						 * The holder is held HERE.
						 * So ask the word, instead
						 * of letting the install
						 * CAS arbitrate it.  It is
						 * the same decision, taken
						 * before any record is
						 * filed.
						 * ☠ This SHRINKS the window
						 * the CAS arbitrates.  It
						 * does not replace the CAS.
						 * An UNDECIDED peer proxy
						 * that outlasts the
						 * patience window of
						 * urcu_txn_read still reads
						 * as NULL.  See the header
						 * of ft_hlist_chain_plan_ok.
						 *
						 * ☠ Nothing is live-stored
						 * yet on this arm:
						 * - the txn is pre-reserved
						 *   "BEFORE any live
						 *   store";
						 * - the sedges are records;
						 * - the cell flip below is
						 *   the first install, and
						 *   its own failure path is
						 *   this very bail.
						 * The retry re-descends
						 * against an untouched
						 * structure.
						 */
						FT_IRH_PROBE(ft, 2, d.pnf, displaced, d.nfp);
						if (!ft_hlist_chain_plan_ok(ft,
								ft_flip_txn_handle(txn),
								displaced, nr_disp)) {
							FT_HLIST_PLAN_BAIL();
							/*
							 * ☠ THE COMMIT IS
							 * ALSO THE CLEANUP,
							 * and this bail is
							 * PRE-commit.  @txn
							 * already carries:
							 * - the holder's
							 *   registered lock
							 *   (the acquire
							 *   above);
							 * - the sedge
							 *   records.
							 * The -EAGAIN arm of
							 * insert_replace_done
							 * destroys @ic.txn,
							 * a DIFFERENT
							 * handle.  Leaving
							 * without a terminal
							 * here leaks
							 * FT_STATE_LOCK on
							 * the holder.  Every
							 * peer is then
							 * refused it
							 * forever.
							 *
							 * MEASURED exactly
							 * that.  The first
							 * spelling of this
							 * bail wedged
							 * inv_concurrent_insert_replace_nolist
							 * past 120 s against
							 * a 4.7 s control.
							 * The ledger printed
							 * "FT REFUSED
							 * (LOCK, unknown holder)" in a loop.
							 *
							 * ft_flip_txn_destroy
							 * is the pre-commit
							 * terminal that
							 * drains the
							 * registry.
							 */
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
						ft_hlist_freeze_chain_prepare_checked(ft,
							ft_flip_txn_handle(txn),
							displaced, nr_disp);
					}
#ifdef FT_DEBUG_STRUCT_ANCHOR
					FT_IR_SEDGE_LANE_ASK(ft, txn, &actx, d.pnf,
						n_sedge);
#endif
					if (ft_ord_cell_swap_publish_multi(ft, old_cell,
							precell, sedges, n_sedge,
							txn) != 0) {
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					ft_ord_cell_free(ft, old_cell);
				} else {
					/*
					 * List off: no cell.  The forward
					 * edge stores into the child slot of
					 * the LIVE holder @d.pnf.  Guard it
					 * §4.B.  Force the edge through a
					 * PRE-RESERVED txn, even when it is
					 * alone.  The guard can then ABORT
					 * on a peer conflict.  A bare lone
					 * store commits infallibly but
					 * cannot detect one (the not-yet-MW
					 * lone-store residue).
					 *
					 * Pre-reserve BEFORE any live store.
					 * On create failure nothing is
					 * applied.  The old head is NOT
					 * freed and the replace aborts; it
					 * can be retried.
					 */
					unsigned int nr_disp = displaced ?
						ft_hlist_chain_len(displaced) : 0;
					struct ft_flip_txn *txn =
						ft_flip_txn_create_bounded(ft,
							FT_PUB_SEDGE_MAX_EDGES +
							1 /* §4.B parent guard */ +
							1 /* §9.3 dual GP: release-lock XOR guard */ +
							nr_disp * FT_HLIST_FREEZE_MAX_EDGES);

					if (!txn) {
						ret = -ENOMEM;
						goto insert_replace_done;
					}
					/*
					 * ★ ACQUIRE THE NEAREST ANCHOR
					 * LOCK.  Do not merely VALIDATE it.
					 *
					 * This op used to §4.B-guard the
					 * holder at all four of its publish
					 * sites.  It never took it.  Its
					 * converted siblings ACQUIRE it:
					 * - cds_ft_replace hoists the chain
					 *   holder above its routing
					 *   derivation (@9e62dc3b);
					 * - the INTERIOR unlink of
					 *   cds_ft_remove holds it too
					 *   (ft_chain_head_holder +
					 *   ft_acquire_member, dated
					 *   FT_DEPTH_FROM_DESCENT through
					 *   ft_lock_ctx_depth_of).
					 *
					 * ☠ A VALIDATOR AND A LOCKER ON ONE
					 * WORD DO NOT EXCLUDE EACH OTHER.
					 * A remove could relink the interior
					 * of a chain under the holder's lock
					 * while this op displaced the whole
					 * chain.  Two symptoms came of that:
					 * - a remove that spun forever on a
					 *   stale back-edge;
					 * - an append onto a chain that no
					 *   longer existed.
					 * Tombstoning the displaced chain is
					 * the interop invariant that
					 * ft-txn-hlist.h demands.  It makes
					 * those states DETECTABLE.  Holding
					 * this word prevents them.
					 *
					 * FT_DEPTH_FROM_DESCENT: the descent
					 * window dates the acquire, exactly
					 * as the plain insert's publish
					 * does.  Acquire BEFORE the record,
					 * never after.  The record asks
					 * ft_flip_txn_owns who owns the word
					 * it writes.  So the registry must
					 * already name the holder.
					 */
					ft_lock_ctx_init(&actx, &d, txn, &optxn);
					ft_flip_txn_lock_or_guard_parent(ft, txn, &actx, d.pnf,
						FT_DEPTH_FROM_DESCENT);
					/*
					 * ☞ A MISSED TAKE ENDS THE ATTEMPT
					 * HERE.  The acquire latches
					 * @acquire_miss and returns.
					 * Carrying on would build the dual
					 * GP acquire, the sedges and the
					 * chain freeze into @txn.  But
					 * ft_flip_txn_commit is bound to
					 * DISCARD that txn: it tests the
					 * miss first and installs nothing.
					 * Measured: ~28k such attempts per
					 * ft_inv leg on the list-off leaf
					 * arm.  Every stale SW record the
					 * audit saw there was among them.
					 *
					 * So run that discard NOW.  It is
					 * the same call, with the same
					 * aging, trace and destroy.  It
					 * gives the same -EAGAIN that the
					 * flip below would return, only
					 * without the records.  Nothing has
					 * been recorded or stored on @txn
					 * yet: it was created just above.
					 */
					if (caa_unlikely(txn->acquire_miss)) {
						(void) ft_flip_txn_commit(ft, txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					/*
					 * §9.3's THIRD LOCK-SET MEMBER, as
					 * cds_ft_replace takes it.  Under a
					 * compressed @d.pnf the SKIP_X dual
					 * lives in the GRANDPARENT's body.
					 * {P} does not cover that body.
					 * -DFT_DEBUG_STRUCT_ANCHOR measured
					 * it: 15,729 (per-node) / 10,279
					 * (exponential) dual records per
					 * ft_inv leg committed with GP not
					 * held.  A GP recompact copies that
					 * slot and trusts its own lock.
					 *
					 * The edges are derived after both
					 * acquires, by
					 * ft_insert_replace_leaf_sedges.
					 * That NAMES this node as the owner
					 * of the dual edge, and passes the
					 * acquire's own answer as @gp_held.
					 * The per-record gate then decides
					 * the kind.
					 */
					gp_held = ft_lock_skip_dual_gp(ft, &actx, txn, d.pnf,
						txn->mtxn);
					/* The pre-commit terminal, as the chain-plan bail below. */
					if (!ft_insert_replace_leaf_plan_ok(ft, d.nfp,
							d.nf, head_prefix)) {
						ft_flip_txn_destroy(txn);
						ret = -EAGAIN;
						goto insert_replace_done;
					}
					n_sedge = ft_insert_replace_leaf_sedges(ft,
						txn->mtxn, d.pnf, d.nfp, d.nf, node, sedges,
						gp_held);
					ft_replace_fault_arm_abort(txn);
					/* -EAGAIN on a peer-conflict ABORT (nothing
					 * installed); the op's own retry loop re-descends. */
					/*
					 * ☑ FREEZE THE DISPLACED HEAD **IN
					 * THIS COMMIT**, while the holder is
					 * held.  Do not sweep it afterwards.
					 *
					 * The doc of ft_hlist_freeze_prepare
					 * gives the reason.  Folding the
					 * mark into the head op's structural
					 * flip-txn "makes the freeze and the
					 * anchor edge commit atomically -- a
					 * reader is never shown @node's head
					 * anchor promoted away while @node
					 * is still unmarked".
					 * cds_ft_replace has always done
					 * this for the one node it
					 * displaces.  This op displaces a
					 * whole chain and did neither.  It
					 * marked the chain with a BARE CAS,
					 * after the commit had already
					 * released the holder.
					 *
					 * ☠ THAT SWEEP WAS THE UNHELD
					 * PRODUCER.  cds_ft_node.next is
					 * owned by the chain HOLDER (the
					 * word-kind table,
					 * fractal-trie-internal.h).  An
					 * unheld bare CAS on an owned word
					 * is the "ownership OBSERVED, never
					 * TAKEN" case that the table
					 * forbids.  The failure sequence:
					 * - a del_prepare passes its entry
					 *   validate;
					 * - the bare CAS lands underneath it
					 *   and turns the loaded next into
					 *   MARK(NULL), the bare value 2;
					 * - the backward edge records slot
					 *   &((struct cds_ft_node *) 2)->prev;
					 * - the commit faults.
					 * Measured 8/8.  It is also what
					 * stands between
					 * cds_ft_node.next/.prev and the SW
					 * park that their [debt] row is
					 * destined for.
					 *
					 * ☑ THE WHOLE CHAIN, not the head
					 * only.  This comment used to read
					 * "the HEAD only: a chain is
					 * unbounded and a txn is not".  That
					 * premise is FALSE.
					 * ft_flip_txn_create_bounded takes a
					 * RUNTIME cap, so @nr_disp edges can
					 * be reserved.  The leaf and prefix
					 * arms of cds_ft_remove_all now
					 * reserve theirs the same way.
					 *
					 * ☠ WHAT FREEZING THE HEAD ALONE
					 * COST.  Every further member was
					 * left LIVE and UNREACHABLE, with no
					 * tombstone anywhere.  So its
					 * removal had to be INFERRED.  The
					 * "does my predecessor still point
					 * at me?" test of cds_ft_remove
					 * exists for exactly these nodes,
					 * and names this op as the reason.
					 * That inference is fallible.  It
					 * read @node->prev and @pred->next
					 * unheld.  It answered a TERMINAL
					 * NOT_FOUND for a live node that a
					 * peer was PROMOTING.  This leaked
					 * the node and livelocked the next
					 * insert of it (230,977,777 retries
					 * in one call).  MEASURED exposure
					 * before this change, on
					 * inv_concurrent_insert_replace_nolist:
					 * 233,577 displacements left 50,661
					 * members unmarked (21.7%).
					 *
					 * The DERIVATION is @nr_disp, and
					 * the walk is BOUND by it.  A
					 * duplicate appended since then
					 * tears the derived tail's NULL.
					 * This ABORTS the commit, and the
					 * retry derives again.  The
					 * head-only freeze would have marked
					 * the head and orphaned that fresh
					 * duplicate behind it.
					 */
					if (displaced) {
						ft_ch_audit(ft, txn, displaced);
						/*
						 * ★ VALIDATE THE PLAN UNDER
						 * THE LOCK (Mathieu,
						 * 2026-09-19).  @nr_disp
						 * was counted at the top of
						 * this block, BEFORE the
						 * acquires above.  That
						 * count had no exclusion.
						 * Until now the derived
						 * tail NULL of the freeze
						 * stood in for it.
						 * MEASURED at this arm's
						 * derivation site: 13,683
						 * (per-node) / 14,337
						 * (exponential) tail
						 * disagreements per ft_inv
						 * leg, the largest of them.
						 *
						 * The holder is held HERE.
						 * So ask the word, instead
						 * of letting the install
						 * CAS arbitrate it.  It is
						 * the same decision, taken
						 * before any record is
						 * filed.
						 * ☠ This SHRINKS the window
						 * the CAS arbitrates.  It
						 * does not replace the CAS.
						 * An UNDECIDED peer proxy
						 * that outlasts the
						 * patience window of
						 * urcu_txn_read still reads
						 * as NULL.  See the header
						 * of ft_hlist_chain_plan_ok.
						 *
						 * ☠ Nothing is live-stored
						 * yet on this arm:
						 * - the txn is pre-reserved
						 *   "BEFORE any live
						 *   store";
						 * - the sedges are records;
						 * - the cell flip below is
						 *   the first install, and
						 *   its own failure path is
						 *   this very bail.
						 * The retry re-descends
						 * against an untouched
						 * structure.
						 */
						FT_IRH_PROBE(ft, 3, d.pnf, displaced, d.nfp);
						if (!ft_hlist_chain_plan_ok(ft,
								ft_flip_txn_handle(txn),
								displaced, nr_disp)) {
							FT_HLIST_PLAN_BAIL();
							/*
							 * ☠ THE COMMIT IS
							 * ALSO THE CLEANUP,
							 * and this bail is
							 * PRE-commit.  @txn
							 * already carries:
							 * - the holder's
							 *   registered lock
							 *   (the acquire
							 *   above);
							 * - the sedge
							 *   records.
							 * The -EAGAIN arm of
							 * insert_replace_done
							 * destroys @ic.txn,
							 * a DIFFERENT
							 * handle.  Leaving
							 * without a terminal
							 * here leaks
							 * FT_STATE_LOCK on
							 * the holder.  Every
							 * peer is then
							 * refused it
							 * forever.
							 *
							 * MEASURED exactly
							 * that.  The first
							 * spelling of this
							 * bail wedged
							 * inv_concurrent_insert_replace_nolist
							 * past 120 s against
							 * a 4.7 s control.
							 * The ledger printed
							 * "FT REFUSED
							 * (LOCK, unknown holder)" in a loop.
							 *
							 * ft_flip_txn_destroy
							 * is the pre-commit
							 * terminal that
							 * drains the
							 * registry.
							 */
							ft_flip_txn_destroy(txn);
							ret = -EAGAIN;
							goto insert_replace_done;
						}
#if FT_DT_ARMED
						ft_sws_doomed = txn->acquire_miss;
#endif
						ft_hlist_freeze_chain_prepare_checked(ft,
							ft_flip_txn_handle(txn),
							displaced, nr_disp);
#if FT_DT_ARMED
						ft_sws_doomed = 0;
#endif
					}
#ifdef FT_DEBUG_STRUCT_ANCHOR
					FT_IR_SEDGE_LANE_ASK(ft, txn, &actx, d.pnf,
						n_sedge);
#endif
					if (ft_ord_cell_flip_into(ft, txn, sedges,
							n_sedge) != 0) {
						ret = -EAGAIN;
						goto insert_replace_done;
					}
				}
			}
			ret = 0;
		}
	} else {
		/*
		 * Found NULL node or external node before end of key.
		 * Attach a new branch, displacing any shorter-key
		 * external node into the new branch's metadata.
		 */
		dbg_printf("_cds_ft_insert_replace: attach before end of key\n");

		ft_lock_ctx_init(&actx, &d, ic.txn, ic.op);
	ic.ctx = &actx;
		ret = ft_attach_node(ft, d.pnfp, d.pnf,
				d.nfp, d.nf, key, key_len, d.depth, node,
				(struct cds_ft_node *) ft_node_ptr(d.nf), &ic,
				&actx);
		if (ret == 0) {
			/* Parked one-commit: +1 folds into the commit. */
			assert(ic.slot || !ft->rank_stats);
			if (d.depth >= 2)
				FT_TP(tree_edge_set, (const void *) ft,
					(const void *) d.ppnf,
					(unsigned int) (d.depth - 2),
					(uint8_t) key[d.depth - 2],
					(const void *) *d.pnfp);
		}
	}

insert_replace_done:
	/*
	 * Concurrent-writer PRE-COMMIT conflict.  The build found that its
	 * descended position moved under a peer.  It bailed before arming or
	 * committing anything, and freed its own fresh cluster.  This test
	 * comes FIRST, ahead of the cleanup below.  That cleanup frees
	 * @precell, and a retry must keep it.
	 */
	if (ret == -EAGAIN) {
		if (ic.txn) {
			ft_flip_txn_destroy(ic.txn);
			ic.txn = NULL;
		}
		ft_txn_attempt_bail(&optxn, true);
		goto restart_replace_attempt;
	}
	/*
	 * @node became the installed head iff node->prev still carries its
	 * pre-wired cell (not external).  A duplicate append (descent through a
	 * compressed node chained @node), the uninstalled key_shorter -EEXIST
	 * path (prev NULLed above), or a failed insert leaves @precell orphaned
	 * -- free it, and on failure restore node->prev to its zeroed state.
	 * List off: there is no cell, so nothing to free or splice (the swap
	 * sites above are gated too).  But a FAILED insert may have wired
	 * node->prev early, in a build path whose cluster the unwind then
	 * freed.  Reset it, so that the retry does not hit the zeroed-prev
	 * entry check (see _cds_ft_insert).
	 */
	if (!ft->ordered_list) {
		if (ret != 0) {
			node->prev = NULL;
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ic.slot) {
			/*
			 * One-commit (list off): commit the recorded structural
			 * publish alone (@cell == NULL); the +1 count folds into
			 * that commit.  A pure replace does not park (ic.slot
			 * unset) and falls through untouched.
			 */
			ft_replace_fault_arm_abort(ic.txn);
			cst = ft_insert_one_commit(ft, _key, _key_len, NULL, &ic);
			assert(ic.count_folded || !ft->rank_stats);
		} else if (ic.txn) {
			/* Armed but nothing parked (duplicate append): no
			 * structural publish to commit. */
			ft_flip_txn_destroy(ic.txn);
			ic.txn = NULL;
		}
	} else {
		if (ret != 0) {
			node->prev = NULL;
			ft_ord_cell_free_unpublished(ft, precell);
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ft_node_external((struct cds_ft_inode_flag *) node->prev)) {
			/* Duplicate append / -EEXIST (prev NULL): cell orphaned. */
			ft_ord_cell_free_unpublished(ft, precell);
			if (ic.txn)
				ft_flip_txn_destroy(ic.txn);
		} else if (ic.slot) {
			/*
			 * Fresh head, parked one-commit: structural publish +
			 * ordinal-cell splice flip atomically.  A replace
			 * (*old_node_ret set) does not park.  It already
			 * swapped @precell into the replaced head's list
			 * slot at the replace site.  So it falls through
			 * here untouched.
			 */
			ft_replace_fault_arm_abort(ic.txn);
			cst = ft_insert_one_commit(ft, _key, _key_len, precell, &ic);
			assert(ic.count_folded || !ft->rank_stats);
		}
	}
	/*
	 * The one-commit's status is the op's whole outcome and MUST be checked on
	 * both arms above.  On an ABORT nothing is published, and the txn's
	 * on-abort action rolls back the fresh cluster.  Discarding the status
	 * would turn that ABORT into OK or DUPLICATE_FOUND with the key absent:
	 * - a lost insert is reported as success;
	 * - @precell is leaked;
	 * - @node keeps a stale ->prev, and the caller's retry then fails entry
	 *   validation with -EINVAL forever.
	 *
	 * _cds_ft_insert has always captured this status and re-descended.
	 * This op does the same on ABORT: see restart_replace_attempt below.
	 * MEMORY_ERROR stays defensive-only, exactly as in _cds_ft_insert: the
	 * one-commit txn is pre-reserved, so its commit cannot allocate.
	 */
	if (caa_unlikely(cst == URCU_TXN_STATUS_ABORT) && ret == 0) {
		/*
		 * A peer won the one-commit's expected-value CAS.  Nothing was
		 * published.  The txn's on-abort action rolled back the fresh
		 * cluster.  @precell was not spliced: the commit is atomic.
		 *
		 * Re-descend, as a BAIL that ages the op.  The content txn is
		 * standalone, so its commit aged its own handle, never @optxn
		 * (see _cds_ft_insert's twin edge).
		 */
		FT_OP_LANE_STAT(cabort);
#ifdef FT_DEBUG_INSERT_ABORT_UNAGED
		ft_txn_attempt_end(&optxn, true);
#else
		ft_txn_attempt_bail(&optxn, true);
#endif
		goto restart_replace_attempt;
	}
	if (caa_unlikely(cst != URCU_TXN_STATUS_OK) && ret == 0) {
		/*
		 * Defensive only, as in _cds_ft_insert: the one-commit txn is
		 * pre-reserved, so its commit cannot allocate.  Publishing
		 * nothing while reporting OK would lose the insert.
		 */
		node->prev = NULL;
		if (ft->ordered_list)
			ft_ord_cell_free_unpublished(ft, precell);
		*old_node_ret = NULL;
		ret = -ENOMEM;
	}
	if (ret == 0) {
		/*
		 * ☞ NO TOMBSTONE SWEEP HERE, DELIBERATELY.  The freeze of the
		 * displaced HEAD rides the displacing commit instead, under
		 * the holder.
		 *
		 * A sweep here was the first cure, and it was the wrong one.
		 * It ran AFTER the commit had released the holder.  So it was
		 * a bare CAS by a non-owner on cds_ft_node.next, a word that
		 * the word-kind table gives to the chain HOLDER.  That is
		 * "ownership OBSERVED, never TAKEN".  The CAS landed
		 * underneath del_prepare / replace_prepare calls that had
		 * already passed their entry validate: 8/8 SEGV.
		 *
		 * ☐ WHAT THIS LEAVES OWED.  Only the head is marked, so a displaced
		 * MEMBER still reads as live by ft_node_is_removed (which is a
		 * per-node test of node->next).  Under the holder the head is always
		 * reachable from a member -- ft_chain_head_holder already walks prev
		 * to it -- so the paired half is a liveness predicate that answers
		 * "own next marked OR HEAD marked", called at the validate points
		 * that already hold the holder.  Until that lands, an op handed a
		 * displaced member can still act on a chain that has left the trie.
		 * That is a smaller and quieter wrong than an unheld writer on an
		 * owned word, but it is not nothing, and it is the next step.
		 */
		if (key_len > uatomic_load(&ft->max_used_key_len, CMM_RELAXED))
			uatomic_store(&ft->max_used_key_len, key_len, CMM_RELAXED);
	}

	ft_op_end(&optxn);
	return ret;
}

enum cds_ft_status cds_ft_insert_replace(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct cds_ft_node *node,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *old_node = NULL;
	int ret;

	CDS_FT_SCOPED_WRITER(ft);
	FT_TP_KEY(insert_replace_enter, ft, key, key_len);
	ret = _cds_ft_insert_replace(ft, key, key_len, node, &old_node);
	if (ret == -EINVAL) {
		*result_node = NULL;
		FT_TP(insert_replace_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (ret == -EAGAIN) {
		/*
		 * Defensive: the inner function retries every -EAGAIN and
		 * every commit ABORT itself (b18a23ba), so this arm is not
		 * reached today.  If it ever is, nothing was published and
		 * @node is reusable.  A peer conflict must not be reported as
		 * an allocation failure by the catch-all below.
		 */
		*result_node = NULL;
		FT_TP(insert_replace_exit, (int) CDS_FT_STATUS_BUSY_ERROR);
		return CDS_FT_STATUS_BUSY_ERROR;
	}
	if (ret) {
		*result_node = NULL;
		FT_TP(insert_replace_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	*result_node = old_node;
	if (old_node) {
		FT_TP(insert_replace_exit, (int) CDS_FT_STATUS_DUPLICATE_FOUND);
		return CDS_FT_STATUS_DUPLICATE_FOUND;
	}
	FT_TP(insert_replace_exit, (int) CDS_FT_STATUS_OK);
	return CDS_FT_STATUS_OK;
}

/*
 * Return true if the holder hold {L} (@hm) excludes every writer of
 * @parent_nf's word.  The §4.B guard on that word is then subsumed.  The guard
 * is the MW-CAS era's arbitration.  Under lock-before-write it is owed only
 * where the op's lock does NOT reach the word.
 *
 * With per-node locking {L} is the holder's own word, so it covers exactly the
 * holder.  Planting the guard there would abort every commit: the guard's
 * clean-LIVE expectation masks the LOCK this op set.  That is the measured
 * livelock this predicate first existed to stop.
 */
static inline
bool ft_replace_hold_covers(struct cds_ft *ft, const struct cds_ft_metadata *hm,
		struct cds_ft_inode_flag *parent_nf)
{
	return hm && ft_flag_to_metadata(ft, parent_nf) == hm;
}

/*
 * Tell a content txn that the op holds {L}'s member.  The member is the holder
 * whose slots the head arms republish.
 *
 * {L} lives on the ACQUIRE txn (hctx's @extra), so the content txn's registry
 * never names it.  The op vouched for structural edges through {L}
 * (ft_ord_cell_flip_into's @owner_held arm).  The registry-only record gate
 * refused every such edge its SW park.  MEASURED: 370k MW_STRUCT records per
 * ft_inv FT_INV_MW=1 run at per-node, all of them on a word this op holds.
 *
 * @covered answers ft_flip_txn_owns without taking on a release.  That is
 * exactly {L}'s shape:
 *  - the routing was derived UNDER {L}, so the plan needs no re-validation;
 *  - ft_replace_exit releases {L} only after the commit returned, settle
 *    included.
 *
 * Cover only the member: under a coarse spacing the anchor ancestor's own
 * words keep their CAS.
 */
static inline
void ft_replace_cover_hold(struct ft_flip_txn *txn,
		const struct cds_ft_metadata *hm, const struct ft_held_anchor *hh)
{
#ifndef FT_DEBUG_REPLACE_HOLD_UNCOVERED
	if (txn && hm)
		ft_flip_txn_cover_member(txn, hh->member);
#else
	(void) txn; (void) hm; (void) hh;
#endif
}

/*
 * cds_ft_replace's SINGLE EXIT for the holder hold {L}.
 *
 * The hold is taken ONCE, above the routing derivation.  Every exit of this op
 * is past it:
 *  - three NOT_FOUNDs from the kind dispatch;
 *  - an OOM on the fresh cell;
 *  - each arm's MEMORY_ERROR and retriable ABORT.
 *
 * One missed release leaves a word that nobody will ever clear.  That is the
 * livelock this conversion exists to remove, on the other bit.  So no exit
 * spells the release itself: they all return through here.
 *
 * The release is idempotent (@*hm is cleared).  So it composes with the arm
 * that must release EARLY.  That arm is the non-head swap: its commit has
 * settled its links before the status plumbing runs.
 */
static inline
enum cds_ft_status ft_replace_exit(struct cds_ft_metadata **hm,
		enum cds_ft_status s)
{
	if (*hm) {
		ft_meta_lock_release(*hm);
		*hm = NULL;
	}
	FT_TP(replace_exit, (int) s);
	return s;
}

/*
 * ★ THE ARM YIELD for the replace lane's §9.3 third member.  A green suite is
 * not evidence for a site that never ran.  And "0 acquires" has FOUR causes:
 *  - never called;
 *  - publish parent not compressed;
 *  - slot not skip-encoded;
 *  - root dual.
 * Only the first means dead code.  Split them.  Report the TOTAL at exit
 * rather than every Nth call: a threshold print reads 1..N-1 as zero.
 */
#ifdef FT_DEBUG_DUAL_SITE
static unsigned long ft_repl_dual_calls, ft_repl_dual_notcompressed,
	ft_repl_dual_acquired, ft_repl_dual_unheld;
static void ft_repl_dual_report(void) __attribute__((destructor));
static void ft_repl_dual_report(void)
{
	fprintf(stderr, "FT REPLACE-DUAL calls=%lu notcompressed=%lu "
		"ACQUIRED=%lu unheld=%lu\n",
		uatomic_read(&ft_repl_dual_calls),
		uatomic_read(&ft_repl_dual_notcompressed),
		uatomic_read(&ft_repl_dual_acquired),
		uatomic_read(&ft_repl_dual_unheld));
}
# define FT_REPL_DUAL_TALLY(parent_nf_, held_)					do {										uatomic_inc(&ft_repl_dual_calls);					if (!(parent_nf_) || !ft_node_compressed(parent_nf_))				uatomic_inc(&ft_repl_dual_notcompressed);			else if (held_)									uatomic_inc(&ft_repl_dual_acquired);				else										uatomic_inc(&ft_repl_dual_unheld);			} while (0)
#else
# define FT_REPL_DUAL_TALLY(parent_nf_, held_)					do { (void) (parent_nf_); (void) (held_); } while (0)
#endif

static
enum cds_ft_status _cds_ft_replace_locked(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node *old_node,
		struct cds_ft_node *new_node,
		bool *need_retry,
		struct ft_op *op)
{
	struct cds_ft_inode_flag *holder_flag;
	struct cds_ft_inode_flag **pub_slot;
	struct cds_ft_compressed_node *cn = NULL;
	struct cds_ft_inode_flag *lock_nf = NULL;	/* the word {L} holds */
	struct cds_ft_metadata *hm = NULL;		/* the hold, released once */
	/*
	 * {L}'s descent, context and held anchor.  They are at FUNCTION scope
	 * because the PUBLISH needs them.  §9.3's third lock-set member (the
	 * SKIP_X dual's grandparent, below) is acquired down in the two publish
	 * arms.  That acquire needs BOTH of the things this block already
	 * derives.
	 *
	 *  - @hctx carries the DESCENT, which is the only source of a byte-depth
	 *    at EXPONENTIAL spacing.  Without it ft_lock_ctx_depth_of fails and
	 *    the choke point sets @acquire_miss.  The commit then aborts and
	 *    the replace retries into the identical shape forever.  That
	 *    livelock is visible at one spacing only.
	 *  - @hh must be REACHABLE AS AN EXTRA, so the dual acquire can dedupe
	 *    against {L}.  At ROOT_ONLY spacing every member anchors on the root,
	 *    so the dual's anchor IS {L}'s word.  An acquire that cannot see
	 *    the op's own hold misses against itself.  That is the same
	 *    livelock, reached another way.  (The note at the acquire below
	 *    predicted precisely this case: "should this path ever gain a
	 *    registry ...
	 *    @shared must then become the 'held, owing no release' arm".)
	 */
	struct ft_lock_ctx hctx;
	struct ft_held_anchor hh;
	bool have_hctx = false;
	bool dual_gp_held = false;
	const uint8_t *iter_key;
	size_t key_len = ft_key_len(ft, ft_iter_resolve_key_len(iter));
	enum cds_ft_status s;

	FT_TP_ITER_KEY(replace_enter, iter);

	/*
	 * If the iterator has a valid path, the RCU read-side lock must
	 * be held.
	 */
	if (iter->cache_valid)
		CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED(ft);
	iter_debug_path_check(iter);

	if (!valid_external_node(old_node) || !valid_external_node(new_node)
			|| !valid_key_len(ft, key_len)) {
		s = CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		return ft_replace_exit(&hm, s);
	}
	/* Expect zeroed next and prev pointers on new_node. */
	if (ft_node_next(new_node) || new_node->prev) {
		s = CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		return ft_replace_exit(&hm, s);
	}

	iter_key = ft_iter_read_key(iter);

	dbg_printf("cds_ft_replace: old_node %p new_node %p\n", old_node, new_node);

	/*
	 * No top-down descent.  As in cds_ft_remove, @old_node is
	 * application-owned and -- with the RCU read-side lock held
	 * continuously since it was obtained -- alive.
	 *
	 * ☠ AND NOTHING FREEZES THE STRUCTURE.  This comment used to claim "the
	 * writer mutex held here freezes the structure, so @old_node->prev is a
	 * settled live pointer to its holder".  That premise is FALSE under
	 * CDS_FT_WRITER_LOCK_FINE: the FT-wide mutex is dropped for a FINE trie,
	 * this op acquires NO node lock, and cds_ft_replace is one of the point
	 * ops the fine-locking transition has not converted (see the TODO at
	 * enum cds_ft_writer_strategy).  So @old_node->prev is a LIVE word with
	 * concurrent writers, and the only safe reads of it are (a) through
	 * ft_dereference_prev_resolved, which strips a parked flip proxy, and
	 * (b) ONCE -- two observations of a live re-home disagree.  Both rules
	 * are now obeyed below (@old_prev); what is NOT yet obeyed is the third
	 * half of the same rule (ft_dereference_prev_resolved's sweep note): a
	 * read taken before the holder acquire is STALE-POSSIBLE and the routing
	 * it derives must be re-validated under that lock.  That re-validation
	 * is the conversion, and it is owed.
	 *
	 * @new_node takes @old_node's exact place in the duplicate chain, so
	 * the key count and trie shape are unchanged.  Only the chain link (or
	 * head slot) that points at @old_node is repointed at @new_node.  That
	 * slot is derived from the holder.  For a non-head duplicate it is the
	 * predecessor's next.  Otherwise it is the head slot cds_ft_remove
	 * recovers, one of:
	 *  - a compressed holder's &cn->child;
	 *  - an internal holder's external_nodes;
	 *  - an internal body slot keyed by the last key byte.
	 */
	if (ft_node_is_removed(old_node)) {
		/* Already unlinked from the trie. */
		s = CDS_FT_STATUS_NOT_FOUND;
		return ft_replace_exit(&hm, s);
	}
	holder_flag = ft_node_holder(ft, old_node);
	if (!holder_flag) {
		/* Never inserted (a freshly-initialized node). */
		s = CDS_FT_STATUS_NOT_FOUND;
		return ft_replace_exit(&hm, s);
	}

	/*
	 * {L} THE LOCALIZED HOLDER LOCK -- TAKEN FIRST, SO THE ROUTING IS
	 * DERIVED UNDER IT AND NEEDS NO RE-VALIDATION.
	 *
	 * This is the fine-locking conversion the header TODO at
	 * enum cds_ft_writer_strategy owed for cds_ft_replace.  It is
	 * DELIBERATELY SMALLER THAN cds_ft_remove's.
	 *
	 * Remove re-validates its ROUTING under the lock (★ RE-VALIDATE THE
	 * CALLER'S ROUTING, ft-remove.h).  Its three arms are three DIFFERENT
	 * structural transforms: interior relink, promote, and
	 * clear-and-nr_child--.  So a decision taken from an unheld read
	 * publishes an ILLEGAL SHAPE (a one-child internal, a compressed node
	 * with a NULL child).
	 *
	 * Replace has ONE transform in every arm: repoint the single slot that
	 * holds @old_node.  _ft_publish_to_parent records that edge as a
	 * value-CAS with expected_old == @old_node.  So a stale @pub_slot whose
	 * VALUE moved simply ABORTS the commit.  The expected-old IS the
	 * routing validation, and nothing needs re-deriving for it.  There is
	 * no descent either: neither this check nor remove's is a
	 * re-validation from the root.
	 *
	 * What the value-CAS canNOT catch is a stale slot ADDRESS.  A
	 * recompaction COWs the holder and retires the old copy.  The body of
	 * the old copy STILL CONTAINS @old_node.  So the CAS succeeds into a
	 * dead node, and the update is lost with no abort anywhere.  That is a
	 * TOMBSTONE question, not a routing question.  Two things answer it:
	 *
	 *   - ft_dlm_lock hard-refuses PROXY | TOMBSTONE | LOCK, so the acquire
	 *     below IS the holder's tombstone validate.  Once the word carries
	 *     this op's LOCK, no peer can retire it or edit its body in place.
	 *   - @old_node's OWN tombstone is re-tested under the lock.  A peer
	 *     remove can freeze it between the pre-lock test above and this
	 *     acquire.  The pre-lock test is only the fast path.
	 *
	 * AND WHY IT HAD TO BE A HOLD, not the §4.B guard the head arms used to
	 * carry alone.  MEASURED on inv_concurrent_same_key_replace_nolist
	 * (FINE): a same-key cds_ft_remove livelocked 50,001 attempts.  Every
	 * one of its ft_dlm_lock refusals was a PROXY on the holder's state
	 * word.  The PROXY came from a DIFFERENT descriptor (streak=1, switches
	 * == the refusal count).  It carried an all-MW {0x20 -> 0x20} record.
	 * That record is ft_flip_txn_guard_parent's {live -> live} state guard,
	 * parked once per replace commit.
	 *
	 * A locker must find the word CLEAN.  A validator that parks that word
	 * starves the locker forever.  Symmetrically the guard loses to
	 * remove's {LOCK|s -> s}, so replace livelocked too (op=3, same row).
	 * MW settles the record KIND, never the EXCLUSION.  The two ops have to
	 * queue on ONE primitive.  Then the op txn's aging and the per-trie
	 * FIFO fallback drain it.
	 *
	 * Bulk peers need nothing here.  G5.25 has a FINE trie RE-TAKE the
	 * FT-wide lock for the whole window a bulk op is live.  So
	 * bulk-vs-point is already arbitrated (see the
	 * enum cds_ft_writer_strategy TODO).
	 */
	if (ft->lock_fine) {

		/*
		 * The PLAN's only job is to name a word to lock.  Walk prev to
		 * the chain's trie holder.  It is the single lockable state
		 * word that every op on this chain serialises on.  The
		 * duplicate append (ft_chain_node's caller) and the interior
		 * unchain (ft_unchain_node) take the same word.
		 *
		 * ☠ A NULL HERE IS A STALE DERIVATION, NOT A NEVER-INSERTED
		 * NODE.  @old_node's own never-inserted case already returned
		 * NOT_FOUND above, off its own prev word.  A same-key peer can
		 * re-head or retire this chain between that read and this walk.
		 * The walk then ends on a NULL prev.  Bail retriably, exactly
		 * as ft_unchain_node does for the same shape.  (Its assert was
		 * a wrong zero from disjoint-range oracles.)
		 */
		lock_nf = ft_chain_head_holder(ft, old_node);
		if (caa_unlikely(!lock_nf)) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			return ft_replace_exit(&hm, s);
		}
		ft_lock_ctx_init(&hctx, NULL, NULL, op);
		/*
		 * @hh.shared is DEAD here, not defensive.  This context is
		 * built with a NULL txn and fills in no extra / glue / outer.
		 * So its held set is EMPTY and the acquire has nothing to
		 * dedupe against.  Should this path ever gain a registry (a
		 * txn, a glue), the fold below is the self-refusal livelock
		 * it is elsewhere in this file.  @shared must then become the
		 * "held, owing no release" arm the two dup-chain acquires
		 * take.
		 */
		if (ft_acquire_member(ft, &hctx, lock_nf,
					ft_flag_to_metadata(ft, lock_nf),
					0, &hh) ||
				hh.shared) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			return ft_replace_exit(&hm, s);
		}
		hm = hh.lock;
		FT_DBG_HELD_AT(hm);
		/*
		 * THE REGISTRY THIS PATH JUST GAINED.  The order matters: it is
		 * wired AFTER the acquire above, never before.  The acquire's
		 * own dedupe must keep seeing an EMPTY held set.  That is what
		 * makes its @hh.shared bail sound.  Every LATER acquire in this
		 * op must see {L} and answer SHARED rather than miss against a
		 * word its own op holds.  Today the later acquire is the SKIP_X
		 * dual's grandparent, in both publish arms.
		 */
		hctx.held.extra = &hh;
		hctx.held.nr_extra = 1;
		have_hctx = true;

#ifdef FT_DEBUG_LATE_SETTLE
		/*
		 * PROBE: does the holder lock ever come back FREE while a word it
		 * protects still carries the releasing commit's proxy?  The
		 * engine settles every record tagged like the lock word in ONE
		 * late pass, in plant order, and FT_HLIST_TAG == FT_STATE_PROXY.
		 */
		{
			static unsigned long ft_ls_checked, ft_ls_proxy,
				ft_ls_proxy_marked;
			void *raw = CMM_LOAD_SHARED(old_node->next);

			uatomic_inc(&ft_ls_checked);
			if (ft_txn_is_proxy(raw, FT_HLIST_TAG)) {
				void *res = ft_txn_resolve(raw, FT_HLIST_TAG);
				unsigned long c = uatomic_add_return(
					&ft_ls_proxy, 1);

				if ((uintptr_t) res & CDS_FT_NODE_REMOVED_FLAG)
					uatomic_inc(&ft_ls_proxy_marked);
				if (c <= 4 || !(c & (c - 1)))
					fprintf(stderr, "FT LATE-SETTLE replace: "
						"holder acquired, old->next still "
						"a PROXY (resolves %p%s) checked=%lu "
						"proxy=%lu proxy_marked=%lu\n",
						res, ((uintptr_t) res &
						CDS_FT_NODE_REMOVED_FLAG) ?
						" = MARKED" : "",
						uatomic_read(&ft_ls_checked), c,
						uatomic_read(&ft_ls_proxy_marked));
			}
		}
#endif
		/* @old_node's OWN tombstone, now that a peer cannot set it. */
		if (caa_unlikely(ft_node_is_removed(old_node))) {
			s = CDS_FT_STATUS_NOT_FOUND;
			return ft_replace_exit(&hm, s);
		}
		/*
		 * ★ THE ONE CHECK THE HOLD ITSELF CANNOT MAKE: that the word
		 * locked is still the word this chain serialises on.  @lock_nf
		 * came off an unheld prev walk.  A peer that inserts a key
		 * EXTENDING this chain's key re-homes the head onto a fresh
		 * junction in one commit, and retires nothing.  So the acquire
		 * can succeed on a live, untombstoned word that now arbitrates
		 * a DIFFERENT chain.
		 *
		 * This op then holds the wrong word and the peer holds the
		 * right one.  Both publish into one slot: one in the
		 * lock-holder's plain-store lane, one as an MW value-CAS.  The
		 * engine's "SW xor MW per slot" rule arbitrates neither ->
		 * LOST UPDATE.
		 *
		 * Mirrors ft_unchain_node's ★ RE-DERIVE and the engine's own
		 * ft_glue_acquire_splice_holders ("re-parented under us: stale
		 * lock").  One prev hop, not a descent.
		 */
		if (caa_unlikely(ft_chain_head_holder(ft, old_node) != lock_nf)) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			return ft_replace_exit(&hm, s);
		}
		/*
		 * FROM HERE THE DERIVATION IS A FACT, not a plan.  @hm freezes
		 * @old_node's prev / next and the holder's body.  So this
		 * re-derivation of the holder is final.  Everything below is
		 * computed once, under the lock.  This covers the kind
		 * dispatch, @pub_slot's address, @is_head and @old_cell.
		 */
		holder_flag = ft_node_holder(ft, old_node);
		if (caa_unlikely(!holder_flag)) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			return ft_replace_exit(&hm, s);
		}
	}

	if (ft_node_external(holder_flag)) {
		/* Non-head duplicate: repoint the predecessor's next. */
		pub_slot = (struct cds_ft_inode_flag **)
			&((struct cds_ft_node *) holder_flag)->next;
	} else if (ft_node_compressed(holder_flag) ||
		   ft_node_skip_compressed(holder_flag)) {
		/* Compressed holder: @old_node is its single external child. */
		cn = ft_node_skip_compressed(holder_flag) ?
			ft_skip_to_compressed(ft, holder_flag) :
			ft_compressed_node_ptr(holder_flag);
		if ((struct cds_ft_node *) ft_node_ptr(cn->child) != old_node) {
			s = CDS_FT_STATUS_NOT_FOUND;
			return ft_replace_exit(&hm, s);
		}
		pub_slot = &cn->child;
	} else if (ft_node_external_nodes(holder_flag) ==
			(struct cds_ft_node *) old_node) {
		/* Internal holder: @old_node heads its external_nodes chain. */
		pub_slot = (struct cds_ft_inode_flag **)
			&cds_ft_item_to_metadata(ft_node_ptr(holder_flag))->external_nodes;
	} else {
		/* Internal holder: @old_node is a body child (leaf key). */
		struct cds_ft_inode_flag *child;

		child = ft_node_get_nth_skip(holder_flag, &pub_slot,
			iter_key[key_len - 1], FT_PF_NONE);
		if (!child ||
		    (struct cds_ft_node *) ft_node_ptr(child) != old_node) {
			s = CDS_FT_STATUS_NOT_FOUND;
			return ft_replace_exit(&hm, s);
		}
	}

	/*
	 * Splice @new_node into @old_node's chain position.  There are three
	 * cases:
	 *
	 *  - @old_node is a chain HEAD with the ordered list on.  Publish a
	 *    FRESH cell for @new_node.  Its node field is set while the cell is
	 *    hidden: cell->node stays WRITE-ONCE, the way the public
	 *    cds_ft_cell_node reads it as a plain pointer.  Swap the fresh cell
	 *    in for @old_node's cell in ONE flip.  The flip is FUSED with the
	 *    structural forward publish and, for a compressed holder, the
	 *    grandparent SKIP_X dual.  So a reader never sees @new_node at one
	 *    index but @old_node (about to be freed) at another.  Nor does it
	 *    see a stale skip target into @old_node (the dangling-skip UAF).
	 *  - @old_node is a non-head duplicate (no cell).  This is a single
	 *    predecessor->next store.
	 *  - @old_node is a list-off head.  A list-off head inherits the
	 *    flagged parent and flips the structural + SKIP_X dual.
	 *
	 * Mirrors ft_promote_head.
	 */
	{
		/*
		 * ONE RESOLVED OBSERVATION of @old_node->prev for this whole arm.
		 *
		 * Three raw reads used to live here:
		 *
		 *   - the @is_head test;
		 *   - the @old_cell derivation;
		 *   - the list-off head's "new_node->prev = old_node->prev"
		 *     inherit.
		 *
		 * They relied on the freeze premise refuted above.  Each was a
		 * defect of its own under FINE:
		 *
		 *   - RAW.  A head-promote / swap / prev-retarget folds its prev
		 *     store onto the commit flip-txn.  So this word transiently
		 *     carries FT's type-7 proxy (all of FT_PARENT_TAG_MASK set).
		 *     ft_ord_cell_ptr strips only FT_ORD_CELL_TAG (bit 0).
		 *     That leaves a descriptor-record pointer with bits 1-3
		 *     still set.  cds_ft_item_to_metadata then builds a range
		 *     header from foreign memory -> SIGSEGV.
		 *     MEASURED, not theorised.  A same-key replace against an
		 *     insert/remove peer faults inside a second, at
		 *     cds_ft_item_to_metadata(p=0x7fff348003de).  The 0xe low
		 *     nibble is 0xf with the cell bit already masked off.  The
		 *     assert at that very site tests the WHOLE tag, and the
		 *     caller has already broken it.  That is why the assert
		 *     cannot fire.
		 *     ft_dereference_prev_resolved exists for exactly this.
		 *     The holder derivation above (ft_node_holder) already
		 *     used it.  So one function was resolving the word and
		 *     then re-reading it raw three times.
		 *   - TWICE.  @is_head said "head, so prev is a cell" from read
		 *     1.  Read 2 could already be a peer's new value.  So the
		 *     two halves of one decision came from different trie
		 *     states.
		 *   - INHERITED.  The list-off arm stored the raw word into the
		 *     fresh head's prev, publishing a parked proxy as a live
		 *     back-edge.
		 */
		void *old_prev = ft_dereference_prev_resolved(old_node);
		bool is_head = !ft_node_external(
			(struct cds_ft_inode_flag *) old_prev);
		struct cds_ft_inode_flag *parent_nf = cn ?
			ft_compressed_node_flag(cn) : holder_flag;
		struct ft_ord_cell *old_cell = (ft->ordered_list && is_head) ?
			ft_ord_cell_ptr(old_prev) : NULL;
		void *new_cell_flag = NULL;
		struct ft_pub_rec rec = { .ctx = have_hctx ? &hctx : NULL,
			.n = 0 };
		struct ft_ord_cell_edge sedges[2] = { 0 };
		unsigned int n_s;
		int r;			/* head-arm commit outcome */

		/*
		 * Alloc the fresh cell BEFORE any mutation so OOM aborts cleanly
		 * (@new_node still carries its zeroed links, @old_node is intact).
		 */
		if (old_cell) {
			/* Resolved, not raw: see ft_promote_head's copy. */
#ifndef FT_DEBUG_NO_CELL_PARENT_RESOLVE
			new_cell_flag = ft_ord_cell_alloc(ft, new_node,
				ft_resolve_flip_proxy(
					rcu_dereference(old_cell->parent)));
#else
			new_cell_flag = ft_ord_cell_alloc(ft, new_node,
				old_cell->parent);
#endif
			if (!new_cell_flag) {
				s = CDS_FT_STATUS_MEMORY_ERROR;
				return ft_replace_exit(&hm, s);
			}
		}
		/* @new_node's successor link is build-invisible (it is fresh). */
		new_node->next = ft_node_next(old_node);
		if (!is_head) {
			/*
			 * Non-head duplicate: atomic replace.
			 * ft_hlist_replace_prepare does all of this in ONE
			 * commit:
			 *  - it marks @old_node (freeze);
			 *  - it swings the predecessor's next and the
			 *    successor's prev from @old_node to @new_node;
			 *  - it builds @new_node's links.
			 * The freeze rides the swap (doc §4.B).  So a racing
			 * del/insert_after fails its old-value check once
			 * this is a concurrent engine.  Multi-edge: readers
			 * resolve the transient interior-next proxies via
			 * cds_ft_node_next_rcu.  It marks @old_node, so the
			 * common freeze below is skipped for this case.
			 * Abortable cleanly: on a txn-alloc OOM nothing is
			 * published and @old_node / @new_node are intact.
			 */
			struct ft_flip_txn *txn;
			enum urcu_txn_status cst;
			bool hm_consumed;

			/*
			 * {L}: a duplicate chain is owned by its
			 * HEAD-HOLDER's node lock.  ft_hlist_replace_prepare
			 * states that contract.  It DROPPED its multi-writer
			 * arbitration on the strength of it.  That acquire
			 * USED TO LIVE HERE, in this arm alone.  It is now
			 * hoisted above the routing derivation and covers
			 * every arm (see the {L} block there).  @hm is the
			 * hold, or NULL on an exclusive trie.  This txn
			 * writes only hlist links, never the holder's state
			 * word, so the plain clear after the commit composes.
			 */
			txn = ft_flip_txn_create_bounded(ft,
					FT_HLIST_REPLACE_MAX_EDGES);
			FT_DBG_HELD_AT(hm);
			if (!txn) {
				new_node->next = NULL;
				s = CDS_FT_STATUS_MEMORY_ERROR;
				return ft_replace_exit(&hm, s);
			}
			ft_ch_audit(ft, txn, old_node);
			(void) ft_hlist_replace_prepare(ft, ft_flip_txn_handle(txn),
				old_node, new_node);
			FT_DBG_HELD_AT(hm);
			/*
			 * ☠ SAMPLED BEFORE THE COMMIT, for two reasons.  The
			 * commit consumes the txn wrapper, so the question
			 * cannot be asked afterwards.  And after a SUCCESSFUL
			 * commit the word may already belong to a peer.
			 */
			hm_consumed = hm && ft_lock_terminal_drops_lock(txn, hm);
			cst = ft_flip_txn_commit(ft, txn);
			FT_DBG_HELD_AT(hm);
			/*
			 * EARLY, and CLEARED.  The commit has settled every
			 * link this arm writes, so the chain is consistent.
			 * The next chain op may have the word back without
			 * waiting for the status plumbing below.
			 * ft_replace_exit then finds @hm NULL and releases
			 * nothing.
			 *
			 * ...BUT ONLY IF THE FENCE IS STILL OURS.  "The commit
			 * settled every link this arm writes" says nothing about
			 * OUR LOCK.  Where a committed terminal drops the lock,
			 * the word is free from that instant.  A peer can then
			 * take it, and this release would clear the PEER's
			 * fence.  Measured as the
			 * largest single source of stolen locks left after the
			 * clear-if-held removal.  Ask the record, never the word.
			 */
			if (hm) {
				if (cst == URCU_TXN_STATUS_OK && hm_consumed) {
					FT_ORPHAN_CONSUMED();
					FT_ORPHAN_PREMISE_CHECK(hm);
				} else {
					ft_meta_lock_release(hm);
				}
				hm = NULL;
			}
			FT_DBG_HELD_AT(hm);
			if (cst < 0) {
				new_node->next = NULL;
				s = CDS_FT_STATUS_MEMORY_ERROR;
				return ft_replace_exit(&hm, s);
			}
			if (cst > 0) {
				/*
				 * Contention ABORT: NOTHING was installed.  This
				 * used to fall through as SUCCESS.  The test
				 * was "< 0", which catches only MEMORY_ERROR.
				 * So a lost replace was silently reported as
				 * OK.  It was unreachable while the op required
				 * caller exclusion.  Making the op concurrent
				 * makes it live, so it must re-derive and
				 * re-attempt.
				 *
				 * BOTH links must be reset, not just next.
				 * ft_hlist_replace_prepare already stored
				 * @new_node->prev = pred as a plain build store
				 * on the (still invisible) fresh node.  Leaving
				 * it set would make the next attempt fail entry
				 * validation with INVALID_ARGUMENT_ERROR rather
				 * than retry.
				 */
				new_node->next = NULL;
				new_node->prev = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
				return ft_replace_exit(&hm, s);
			}
		} else if (old_cell) {
			/*
			 * Head, list on: fresh-cell swap fused with the publish.
			 * The successor's prev is a LIVE settled store.  It
			 * cannot ride the flip, because a skip resolution
			 * reads a head's prev raw.  So make the flip
			 * infallible: pre-reserve its txn BEFORE that store.
			 * On OOM, abort with the successor untouched and the
			 * replace retriable.
			 */
			struct ft_ord_cell *new_cell =
				ft_ord_cell_ptr(new_cell_flag);
			struct ft_flip_txn *txn =
				ft_flip_txn_create_bounded(ft,
					FT_ORD_CELL_SWAP_PUBLISH_MAX_EDGES +
					FT_HLIST_FREEZE_MAX_EDGES +
					1 /* §4.B parent guard */ +
					1 /* §9.3 dual GP: release-lock XOR guard */);

			if (!txn) {
				new_node->next = NULL;
				ft_ord_cell_free_unpublished(ft, new_cell);
				s = CDS_FT_STATUS_MEMORY_ERROR;
				return ft_replace_exit(&hm, s);
			}
			ft_replace_cover_hold(txn, hm, &hh);
			if (new_node->next)
				new_node->next->prev = new_node;
			cds_ft_item_to_metadata(new_cell)->incoming_byte =
				cds_ft_item_to_metadata(old_cell)->incoming_byte;
			/* Fresh @new_node -> cell: build-invisible. */
			new_node->prev = new_cell_flag;
			/*
			 * VALIDATE (§4.B): guard the LIVE holder parent_nf --
			 * UNLESS {L} ALREADY HOLDS THAT WORD.
			 *
			 * ☠ THE GUARD AND THE HOLD CANNOT COEXIST ON ONE WORD.
			 * ft_flip_txn_guard_parent's expectation is clean-LIVE:
			 * it masks FT_STATE_LOCK out of the value it validates.
			 * Once this op has set that very bit, the guard names a
			 * value the word has not carried since the mark landed.
			 * The commit then aborts UNCONDITIONALLY.
			 *
			 * MEASURED the moment {L} was hoisted.  On
			 * inv_concurrent_same_key_replace_nolist, 49966 of
			 * 49974 aborts were attributed to the VALIDATE class
			 * (100.0%).  Replace livelocked at this arm's ABORT
			 * exit.  (-DFT_WINNER_DBG names the losing record's
			 * class.  The retry cap now exit()s so its
			 * destructor dump survives.)
			 *
			 * The predicate is the one
			 * ft_flip_txn_lock_or_guard_parent_at already applies
			 * on its @held.shared path.  It is spelled against
			 * @hm because {L} lives on the acquire txn rather
			 * than this content txn.  The guard is owed only
			 * where the hold does NOT cover this word.  Where the
			 * hold covers the word, it subsumes the guard.  A
			 * retire needs this word's lock, and the acquire
			 * refused a TOMBSTONE.  So neither case the guard
			 * exists for can arise: "retired during the window"
			 * and "already retired".
			 *
			 * At a COARSER spacing the hold anchors on an
			 * ANCESTOR, so @hm names a different word.
			 * ft_replace_hold_covers answers whether it still
			 * covers this one.
			 */
			if (!ft_replace_hold_covers(ft, hm, parent_nf))
				ft_flip_txn_guard_parent_ctx(ft, txn, parent_nf,
					&hctx);
			/*
			 * §9.3's THIRD LOCK-SET MEMBER.  A COMPRESSED
			 * @parent_nf makes this publish write TWO reader-visible
			 * words:
			 *  - the holder's own child slot;
			 *  - the SKIP_X dual, which lives in the GRANDPARENT's
			 *    body.
			 * So §8.2 puts that second word under GP's lock, and
			 * {L} alone does not cover it.  Acquire it from the
			 * SAME derivation the record will name
			 * (ft_lock_skip_dual_gp).  Only that form of the
			 * answer is true at publish time rather than at plan
			 * time.
			 *
			 * @hctx supplies the descent (a depth at EXPONENTIAL
			 * spacing) and {L} as an extra (the dedupe at ROOT_ONLY);
			 * see their declaration.  A non-compressed parent, a
			 * non-skip slot, a root dual or a coarse trie all return
			 * false having planted nothing.
			 *
			 * The kind stays MW here: a slot is SW xor MW GLOBALLY,
			 * so every producer of this word flips in one commit or
			 * none does.  This site's job is to stop being the one
			 * that CANNOT answer.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft,
				have_hctx ? &hctx : NULL, txn, parent_nf,
				txn->mtxn);
			FT_REPL_DUAL_TALLY(parent_nf, dual_gp_held);
			_ft_publish_to_parent(ft, parent_nf, pub_slot,
				(struct cds_ft_inode_flag *) new_node,
				(struct cds_ft_inode_flag *) old_node, &rec, dual_gp_held);
			n_s = ft_pub_rec_sedges(&rec, sedges);
			/*
			 * Fuse @old_node's freeze (mark old_node->next, target
			 * preserved) into the swap commit (doc §4.B); the reservation
			 * above carries the extra edge.
			 */
			ft_ch_audit(ft, txn, old_node);
			ft_hlist_freeze_prepare(ft, ft_flip_txn_handle(txn), old_node);
			ft_replace_fault_arm_abort(txn);
			r = ft_ord_cell_swap_publish_multi(ft, old_cell, new_cell,
				sedges, n_s, txn);
			if (caa_unlikely(r)) {
				/*
				 * ABORT: nothing installed, @old_node and @old_cell
				 * still LIVE.  Dropping this status freed
				 * @old_cell, a cell still linked in the ordered
				 * list, and reported the replace as OK.  The
				 * non-head arm above already had this exact
				 * fix.  The head arms did not.  Yet
				 * ft_ord_cell_flip_into's contract says a
				 * retry-enabled op MUST propagate ABORT.  This
				 * op IS retry-enabled (cds_ft_replace's
				 * need_retry loop).
				 *
				 * The successor's prev is the one LIVE store
				 * this arm makes BEFORE the flip.  It cannot
				 * ride the commit, because a skip resolution
				 * reads a head's prev raw.  So it is also the
				 * one thing the unwind must put back: point it
				 * at @old_node again, which is what it held.
				 * Then reset BOTH of @new_node's links.
				 * Otherwise the next attempt fails entry
				 * validation with INVALID_ARGUMENT_ERROR
				 * instead of retrying.
				 */
				if (new_node->next)
					new_node->next->prev = old_node;
				new_node->next = NULL;
				new_node->prev = NULL;
				ft_ord_cell_free_unpublished(ft, new_cell);
				if (r == -EAGAIN) {
					FT_DBG_RETRY_SITE();
					*need_retry = true;
					s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
				} else {
					s = CDS_FT_STATUS_MEMORY_ERROR;
				}
				return ft_replace_exit(&hm, s);
			}
			ft_ord_cell_free(ft, old_cell);
		} else {
			/*
			 * Head, list off: no cell; flip the structural + SKIP_X
			 * dual.  The successor's prev back-edge is a LIVE settled
			 * store.  (Uniform pre-reserve: no flip proxy on any
			 * prev slot, matching the list-on head.)  So make the
			 * flip infallible: PRE-RESERVE its bounded txn BEFORE
			 * that store.  On OOM, abort with the successor
			 * untouched and the replace retriable (@new_node
			 * restored to its fresh state).
			 */
			struct ft_flip_txn *txn =
				ft_flip_txn_create_bounded(ft, FT_PUB_SEDGE_MAX_EDGES +
					FT_HLIST_FREEZE_MAX_EDGES +
					1 /* §4.B parent guard */ +
					1 /* §9.3 dual GP: release-lock XOR guard */);

			if (!txn) {
				new_node->next = NULL;
				s = CDS_FT_STATUS_MEMORY_ERROR;
				return ft_replace_exit(&hm, s);
			}
			ft_replace_cover_hold(txn, hm, &hh);
			if (new_node->next)
				new_node->next->prev = new_node;
			new_node->prev = old_prev;	/* resolved, see @old_prev */
			/*
			 * VALIDATE (§4.B): guard the LIVE holder parent_nf --
			 * UNLESS {L} ALREADY HOLDS THAT WORD.
			 *
			 * ☠ THE GUARD AND THE HOLD CANNOT COEXIST ON ONE WORD.
			 * ft_flip_txn_guard_parent's expectation is clean-LIVE:
			 * it masks FT_STATE_LOCK out of the value it validates.
			 * Once this op has set that very bit, the guard names a
			 * value the word has not carried since the mark landed.
			 * The commit then aborts UNCONDITIONALLY.
			 *
			 * MEASURED the moment {L} was hoisted.  On
			 * inv_concurrent_same_key_replace_nolist, 49966 of
			 * 49974 aborts were attributed to the VALIDATE class
			 * (100.0%).  Replace livelocked at this arm's ABORT
			 * exit.  (-DFT_WINNER_DBG names the losing record's
			 * class.  The retry cap now exit()s so its
			 * destructor dump survives.)
			 *
			 * The predicate is the one
			 * ft_flip_txn_lock_or_guard_parent_at already applies
			 * on its @held.shared path.  It is spelled against
			 * @hm because {L} lives on the acquire txn rather
			 * than this content txn.  The guard is owed only
			 * where the hold does NOT cover this word.  Where the
			 * hold covers the word, it subsumes the guard.  A
			 * retire needs this word's lock, and the acquire
			 * refused a TOMBSTONE.  So neither case the guard
			 * exists for can arise: "retired during the window"
			 * and "already retired".
			 *
			 * At a COARSER spacing the hold anchors on an
			 * ANCESTOR, so @hm names a different word.
			 * ft_replace_hold_covers answers whether it still
			 * covers this one.
			 */
			if (!ft_replace_hold_covers(ft, hm, parent_nf))
				ft_flip_txn_guard_parent_ctx(ft, txn, parent_nf,
					&hctx);
			/*
			 * §9.3's THIRD LOCK-SET MEMBER.  A COMPRESSED
			 * @parent_nf makes this publish write TWO reader-visible
			 * words:
			 *  - the holder's own child slot;
			 *  - the SKIP_X dual, which lives in the GRANDPARENT's
			 *    body.
			 * So §8.2 puts that second word under GP's lock, and
			 * {L} alone does not cover it.  Acquire it from the
			 * SAME derivation the record will name
			 * (ft_lock_skip_dual_gp).  Only that form of the
			 * answer is true at publish time rather than at plan
			 * time.
			 *
			 * @hctx supplies the descent (a depth at EXPONENTIAL
			 * spacing) and {L} as an extra (the dedupe at ROOT_ONLY);
			 * see their declaration.  A non-compressed parent, a
			 * non-skip slot, a root dual or a coarse trie all return
			 * false having planted nothing.
			 *
			 * The kind stays MW here: a slot is SW xor MW GLOBALLY,
			 * so every producer of this word flips in one commit or
			 * none does.  This site's job is to stop being the one
			 * that CANNOT answer.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft,
				have_hctx ? &hctx : NULL, txn, parent_nf,
				txn->mtxn);
			FT_REPL_DUAL_TALLY(parent_nf, dual_gp_held);
			_ft_publish_to_parent(ft, parent_nf, pub_slot,
				(struct cds_ft_inode_flag *) new_node,
				(struct cds_ft_inode_flag *) old_node, &rec, dual_gp_held);
			n_s = ft_pub_rec_sedges(&rec, sedges);
			/* Fuse @old_node's freeze into the structural publish (doc §4.B). */
			ft_ch_audit(ft, txn, old_node);
			ft_hlist_freeze_prepare(ft, ft_flip_txn_handle(txn), old_node);
			ft_replace_fault_arm_abort(txn);
			if (caa_unlikely(ft_ord_cell_flip_into(ft, txn, sedges, n_s)
					!= URCU_TXN_STATUS_OK)) {
				/*
				 * ABORT: nothing installed.  Same unwind as the list-on
				 * head arm, minus the cell.  Restore the
				 * successor's prev (the one live pre-flip
				 * store) and reset both of @new_node's links.
				 */
				if (new_node->next)
					new_node->next->prev = old_node;
				new_node->next = NULL;
				new_node->prev = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				s = CDS_FT_STATUS_OK;	/* discarded by the retry loop */
				return ft_replace_exit(&hm, s);
			}
		}
	}

	/*
	 * @old_node has left the trie (replaced by @new_node).  It was frozen
	 * atomically inside its own commit above, so there is no separate
	 * tombstone pass:
	 *  - the non-head swap freezes it via ft_hlist_replace_prepare;
	 *  - each head case freezes it via ft_hlist_freeze_prepare, fused with
	 *    the structural publish.
	 *
	 * The trie structure is unchanged (no recompaction), so the iterator
	 * path remains valid in cached mode.
	 */
	iter_auto_invalidate_cache(iter);
	s = CDS_FT_STATUS_OK;
	return ft_replace_exit(&hm, s);
}

/*
 * Public entry.  It adds the FT-owned per-op read-side bracket and the retry
 * identity.  It mirrors cds_ft_remove, its structural twin: both take the
 * target node explicitly and derive position from node->prev with NO
 * re-descent.  The two were asymmetric.  Remove absorbed contention internally
 * while replace had no retry path at all.  So a caller could not write the
 * same loop around both.
 *
 * RETRY: an attempt publishes NOTHING and signals @need_retry when it loses
 * the chain's holder lock, or when its commit returns ABORT.  The loop then
 * re-derives from node->prev against the current tree and re-attempts.  Aging
 * is carried on the PERSISTENT @optxn via ft_op_conflict.  After its lane
 * budget of conflicts, this writer escalates into the per-trie FIFO fair-mutex
 * lane.  That lane drains the contention so the retry TERMINATES (no
 * livelock).  An exclusive trie opens nothing and never conflicts.
 *
 * Every retrying path resets @new_node->next before returning.  The entry
 * validation rejects a @new_node whose links are non-NULL.  Without that reset
 * the second attempt would fail with INVALID_ARGUMENT_ERROR instead of
 * retrying.
 */
enum cds_ft_status cds_ft_replace(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node *old_node,
		struct cds_ft_node *new_node)
{
	struct ft_op optxn;
	enum cds_ft_status s;
	bool need_retry;
	struct ft_op_retry retry;

	CDS_FT_SCOPED_WRITER(ft);
	if (caa_unlikely(ft_bulk_active(ft))) {
		/* See ft_iter_redescend_node: the gate is set, so re-derive. */
		s = ft_iter_redescend_node(ft, iter, old_node);
		if (s != CDS_FT_STATUS_OK)
			return s;
	}
	ft_op_retry_init(&retry, FT_OP_REPLACE, NULL, 0);
	ft_txn_op_init(ft, &optxn);
	for (;;) {
		need_retry = false;
		ft_op_retry_tick(ft, &retry, 0);
		ft_op_begin(&optxn);
		s = _cds_ft_replace_locked(ft, iter, old_node, new_node,
				&need_retry, &optxn);
		if (!need_retry)
			break;
		/* Age the conflict, keep the FIFO turn, close the attempt. */
		ft_txn_attempt_bail(&optxn, true);
	}
	ft_op_end(&optxn);
	return s;
}
