// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-remove.h
 *
 * Userspace RCU library - Fractal Trie: remove / remove_all and the
 * detach-node recompaction.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-remove.h is an implementation unit; #include it from fractal-trie.c only"

#endif

/*
 * ft_detach_node: detach a node from the trie and prune empty
 * single-child ancestors above it.
 *
 * Walk upward from the parent of the detached node through the
 * metadata->parent pointers.  Prune single-child ancestors.  Stop at
 * a node with multiple children, at external nodes, or at the root.
 * Replace the pruned branch with the topmost external nodes found
 * during the walk (or NULL).
 *
 * @detach_node_flag_ptr: slot in parent pointing to the detached node.
 * @detach_parent_flag_ptr: slot in grandparent pointing to the parent.
 * @detach_depth: trie depth of the detached node.
 * @fuse_cell: the dead head's ord cell, when a key-disappearing remove has
 *   the ordered list on.  It is unspliced atomically with the structural
 *   unlink.  NULL otherwise (move/merge callers, or list off).
 * @pub: armed by the in-place (no-recompaction) leaf delete.  The forward
 *   NULL store is then deferred and committed in one flip with @fuse_cell's
 *   unsplice (ft_remove_one_commit).  On return, @pub->armed tells the caller
 *   whether the unsplice was fused here.  True means fused.  False means the
 *   caller must do it separately (recompaction or compressed-parent shape,
 *   still two-commit).  Both are NULL for non-fusing callers.
 */

/*
 * Replace a compressed (or skip-compressed) parent in
 * ft_detach_node's structural-change phase.
 *
 * Two sub-cases:
 *
 *   - @topmost_external_nodes != NULL: the detached child carried
 *     external_nodes that must be promoted to the compressed node's
 *     child slot.  Keep the compressed node (its path is needed for
 *     lookups).  Replace cn->child with the external chain head.
 *
 *   - Otherwise: the compressed parent is no longer needed.
 *     Allocate a fresh empty internal.  It inherits the parent and
 *     skip slot metadata.  Publish it in place of the compressed
 *     node, and free the compressed node.  Returns -ENOMEM if the
 *     fresh allocation failed.
 */
/*
 * @iter_depth is the byte-depth of @iter_node_flag.  @ctx is the op's lock
 * context.  Both acquires below are lock-set members, and a member's acquire
 * goes to its anchor (doc/design/ft-dlm-lock-coarseness.md §2).
 *
 * @freeze_leaf (@freeze_len entries), when set, is the removed external leaf
 * whose chain freezes in @txn.  It is recorded here, after each arm's
 * acquire.  The reason: its chain's holder can be @iter_node_flag itself (a
 * leaf straight under the compressed parent), and only these acquires take
 * that node.
 *
 * History: the caller used to record it first.  -DFT_DEBUG_CHAIN_HOLD then
 * read it holding nothing 4 / 19 / 1 times per ft_inv leg (per-node /
 * exponential / root-only), all in the retiring arm below.
 */
static
int ft_detach_node_replace_compressed_parent(struct cds_ft *ft,
		struct cds_ft_inode_flag *iter_node_flag,
		unsigned int iter_depth,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag **detach_parent_flag_ptr,
		struct cds_ft_node *topmost_external_nodes,
		struct cds_ft_inode_flag *elevated_old_child,
		struct ft_ord_cell *fuse_cell,
		struct ft_remove_pub *pub,
		struct ft_detach_run *run,
		struct ft_flip_txn *txn,
		bool record_only,
		long count_delta,
		struct cds_ft_node *freeze_leaf,
		unsigned int freeze_len)
{
	/*
	 * Does the op hold the SKIP_X dual's derived grandparent?  The acquire
	 * answers this at each of the three publishes below.  The answer is
	 * the return value of ft_lock_skip_dual_gp.  It is true for REGISTERED
	 * or SHARED, which is the assert's own predicate.
	 *
	 * ☠ Do not answer it with ft_skip_dual_gp_held.  That helper asks
	 * @ctx.  A ft_owner_ctx_holds() miss means "the registry cannot see
	 * this hold".  It never means "the op does not hold it".  An acquire
	 * taken through ft_dlm_acquire_set_at registers into no ctx at all,
	 * because it receives the ctx as a const pointer.  Its marks are
	 * invisible to the ask until the owner's commit hands them to a txn.
	 * This is how ft_node_recompact's dual site came to record "0 held of
	 * 805202" about a word it holds on every fine-trie publish.  The ask
	 * is kept below as a measurement of that registry.  Its answer must
	 * not be wired to a record kind.
	 */
	bool dual_gp_held = false;

	/*
	 * @txn is created and reserved by the caller (ft_detach_node).  It is
	 * sized for two things:
	 *  - this publish's structural edges;
	 *  - one freeze-on-free tombstone per node in the orphaned chain the
	 *    caller collected below.
	 * So the sub-case-2 retire here and the caller's entire orphan set
	 * freeze atomically with the publish that unlinks them (atomic detach,
	 * §4.B).
	 *
	 * The caller reserves @txn before any side-effect (its orphan walk is
	 * read-only).  So the pre-flip ft_set_parent here still reaches an
	 * allocation-free point of no return.  It commits through
	 * ft_ord_cell_flip_into, which cannot fail.  On any failure below the
	 * caller destroys @txn.
	 */
	/*
	 * Resolve the retired compressed node once, through the shared
	 * read-side reanchor (MW convergence).
	 *
	 * @iter_node_flag is a raw slot value that may be SKIP-compressed.
	 * ft_compressed_node_ptr does not strip the skip high bits.  A raw
	 * ft_compressed_node_ptr(iter_node_flag) fed to
	 * cds_ft_item_to_metadata() faults on the skip_len bits (the
	 * ft-remove.h detach segv).
	 *
	 * @src_cn is the live compressed node.  The deref sites (src_meta, the
	 * tombstone freeze, the free) must use it.  The raw @iter_node_flag is
	 * kept only as the expected-old of the forward CAS: it must match the
	 * SKIP_X stored in the grandparent slot.
	 *
	 * A rewind > 0 means that a peer chain-merge moved the encoded
	 * position shallower.  The detach premise is then stale.  Bail before
	 * any build, so that the caller re-descends.
	 */
	unsigned int iter_rewind;
	struct cds_ft_compressed_node *src_cn = ft_compressed_node_ptr(
		ft_reanchor_flag(ft, iter_node_flag, &iter_rewind));

	if (caa_unlikely(iter_rewind)) {
		/*
		 * @txn is caller-owned and already holds the orphan
		 * freeze-on-free tombstones.  The caller's cleanup does not
		 * destroy it on -EAGAIN: it assumes a commit consumed it.  So
		 * a pre-commit -EAGAIN must destroy it here, or the descriptor
		 * leaks.  (Pre-existing on this rewind bail.  The DLM
		 * acquire-miss bails below share the same contract.)
		 */
		if (!record_only)
			ft_flip_txn_destroy(txn);
		return -EAGAIN;
	}
	if (topmost_external_nodes) {
		/*
		 * Keep the compressed node -- its path is needed for
		 * lookups to reach the correct depth.  Replace
		 * cn->child with the external node.
		 *
		 * Compressed nodes can have an external child
		 * (cn->child pointing to an external node) but must
		 * NOT have metadata->external_nodes set.
		 */
		struct cds_ft_compressed_node *cn = src_cn;
		const struct ft_glue *split_g = record_only ?
			ft_glue_that_split(ctx, cn) : NULL;

		/*
		 * ☠ This very commit destroys @cn, so do not write it.
		 *
		 * A same-trie rekey whose graft split @cn arrives here in this
		 * state:
		 *  - @cn is on the glue's retire list;
		 *  - @cn's one live child @cn_child is already re-homed, by a
		 *    deferred edge, into the fresh cluster that the forward
		 *    publish installs.
		 *
		 * The promote below would then write @cn twice:
		 *  - into @cn->child, a body nothing will read after the flip;
		 *  - through _ft_publish_to_parent's SKIP_X arm, into @cn's own
		 *    home slot.  That is the word the glue's publish repoints.
		 *
		 * This gives two records on one word whose expected-olds
		 * disagree.  The dual chains {skip(cn_child) -> skip(head)}.
		 * The glue then chains {skip(cn_child) -> top}.
		 * urcu_txn_record_chain finds r->new_ptr != old_ptr and poisons
		 * the descriptor.  The commit aborts and the op re-plans the
		 * identical shape.  Measured on the shipping build: 310,024
		 * poisons in 40 s on one such shape, with a fresh cluster
		 * allocated per attempt.
		 *
		 * ☠ Curing only that collision commits a wrong trie.  Measured
		 * with the collision cured (a read-your-own-writes expected-old
		 * on the glue's publish): the call returns, and cds_ft_verify
		 * is red with
		 * "head cell {parent <cn>} != expected {owner <branch>}".
		 * The next insert then wedges.  The glue's build wired the
		 * branch's old direction to @cn_child, believing it survives.
		 * The promote dissolves @cn_child and puts the external head in
		 * its place.  So the two steps disagree about what lives under
		 * that branch.  The collision is the first symptom of that
		 * disagreement, not the defect.
		 *
		 * Re-home instead of publishing.  Hand the promoted head to the
		 * deferred edge that was going to move @cn_child, and store it
		 * into the fresh cluster's own slot.  Once @cn is retired, both
		 * of the promote's writes belong to that edge:
		 *  - the forward write, because the fresh slot is the successor
		 *    of @cn->child;
		 *  - the head's back-edge, because ft_reparent_record records
		 *    it against the fresh owner with a read-your-own-writes
		 *    expected-old, atomically with the publish.
		 * @cn's home slot is then written once, by the glue.  Its raw
		 * expected-old is correct because nothing else touched the
		 * word.
		 *
		 * The store into the fresh slot is a plain one.  That cluster
		 * is build-invisible until the forward publish, and the txn
		 * publishes reachability, not interiors.
		 */
		if (split_g) {
			struct ft_glue *g = (struct ft_glue *) split_g;
			int idx = g->old_dir_deferred;

			/*
			 * The drop-old-direction arm leaves the caller no detach
			 * to run (@old_dir_dropped).  Reaching here with it set
			 * would mean that two steps both claim the old
			 * direction.
			 */
			urcu_assert_debug(!g->old_dir_dropped);
			/*
			 * The same holds for a substituted old direction.  Two
			 * of the three substitutions leave @old_dir_deferred at
			 * -1, and the refusal below catches them.  The PROMOTE
			 * arm defers a live head, so it hands back a valid
			 * index.  This frame would clobber that index with its
			 * own promoted chain.  The caller that arms
			 * @old_dir_replace runs no detach (the `!done` guard at
			 * its step 3), so this is unreachable.  It is asserted,
			 * not assumed, because the refusal below cannot see
			 * it.
			 */
			urcu_assert_debug(!g->old_dir_replace.done);
			/*
			 * Refuse every shape the re-home is not written for.
			 * Do not guess.  The shapes are:
			 *  - a re-home through a fresh suffix node, whose
			 *    parent slot holds a SKIP form this frame cannot
			 *    re-encode;
			 *  - a build that re-homed nothing;
			 *  - a rank-stats trie: the fresh cluster was sized
			 *    with @cn_child's key count, which the promote
			 *    changes;
			 *  - the fused arm: its cell unsplice rides the
			 *    publish we are removing.
			 * The refusal is terminal: the shape is deterministic,
			 * so -EAGAIN would spin.  It comes before any
			 * side-effect: nothing is acquired, recorded or stored
			 * above this point.
			 */
			if (g->old_dir_via_suffix || idx < 0 || ft->rank_stats ||
					count_delta || fuse_cell || run)
				return -EDOM;
			/* A bulk fold: the FT-wide writer lock is the exclusion. */
			if (freeze_leaf) {
				ft_ch_audit_ctx(ft, txn, ctx, freeze_leaf);
				FT_HLIST_PLAN_OBSERVE(ft,
					ft_flip_txn_handle(txn), freeze_leaf,
					freeze_len, 0);
				ft_hlist_freeze_chain_prepare(ft,
					ft_flip_txn_handle(txn), freeze_leaf,
					freeze_len);
			}
			g->deferred[idx].child =
				(struct cds_ft_inode_flag *) topmost_external_nodes;
			CMM_STORE_SHARED(*g->deferred[idx].slot,
				(struct cds_ft_inode_flag *) topmost_external_nodes);
			return 0;
		}
		/*
		 * DLM Step 1: @cn is the value-swap RELEASE target.  This
		 * external-promote publishes into its child slot.  Acquire @cn
		 * up front (hard, no guard-fallback) and record its
		 * {LOCK|s -> s} release.  Both publish arms below then skip
		 * their lock_or_guard.  An -EAGAIN here comes before any edge
		 * is recorded.  This function then destroys @txn (unless
		 * @record_only) and the caller re-descends.
		 */
		if (ft->lock_fine) {
			struct ft_held_anchor cn_held;

			if (ft_acquire_member(ft, ctx,
					ft_compressed_node_flag(cn),
					cds_ft_item_to_metadata(
						(struct cds_ft_inode *) cn),
					iter_depth, &cn_held)) {
				/* Pre-commit -EAGAIN: destroy the caller-owned txn
				 * (its cleanup skips destroy on -EAGAIN). */
				if (!record_only)
					ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
			/*
			 * A member the op already held rode an earlier acquire,
			 * and that acquire recorded its release.  A second
			 * record would settle the single word twice.
			 */
			if (!cn_held.shared) {
				ft_flip_txn_lock_register_held(txn, &cn_held);
				ft_flip_txn_record_release_lock(txn, cn_held.lock,
					cn_held.lock_snap);
			}
			/*
			 * ☠ @elevated_old_child is a plan read of cn->child,
			 * taken before this lock.  The publish below records
			 * it as the expected-old of a word the op now holds.
			 * That is a blind SW store (ft_flip_txn_record_tag's
			 * held_sw): it pins nothing.  A peer insert of a
			 * longer key republishes cn->child in place under
			 * this very word (ft_insert_compressed_past_child),
			 * without moving cn's state.  So re-read cn->child
			 * here, under the lock.
			 */
#ifndef FT_DEBUG_NO_CN_CHILD_RECHECK
			if (!record_only &&
					ft_resolve_flip_proxy(rcu_dereference(
						cn->child)) != elevated_old_child) {
				FT_DBG_RETRY_SITE();
				ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
#endif
		}
		/* After the acquire: see the function header. */
		if (freeze_leaf) {
			ft_ch_audit_ctx(ft, txn, ctx, freeze_leaf);
			/*
			 * ★ Validate the plan under the lock.  See the twin
			 * in ft_chain_compress_fused.
			 *
			 * @freeze_leaf's holder is held.  It is @cn itself
			 * when no climb happened.  Otherwise it is an orphan
			 * below @cn that ft_detach_node's orphan walk locked
			 * and holds until its `end:` sweep.
			 *
			 * @freeze_len was derived before the acquire, and the
			 * chain parks SW.  A duplicate appended in between
			 * would be frozen over blind and lost.
			 *
			 * History: a note here said "MEASURED ... never
			 * stale, 0 of 65,434".  That was a fact about
			 * unpreempted load.  The chain_compress twin went
			 * stale 9 times in ~6M once the remover was
			 * preempted.
			 *
			 * Nothing is recorded or stored yet on this path.
			 * The unwind is the same as the acquire's bail above.
			 */
			if (ft->lock_fine && !record_only &&
					!ft_hlist_chain_plan_ok(ft,
						ft_flip_txn_handle(txn),
						freeze_leaf, freeze_len)) {
				FT_HLIST_PLAN_BAIL();
				FT_DBG_RETRY_SITE();
				ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
			FT_HLIST_PLAN_OBSERVE(ft, ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len, 1);
			ft_hlist_freeze_chain_prepare_checked(ft,
				ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len);
		}
		/*
		 * Fold the external head's back-edge into @txn.  The back-edge
		 * is cell->parent (list on) or the head's prev (list off).  It
		 * then commits atomically with cn->child below, and not as a
		 * bare store racing ahead of the publish.
		 *
		 * A skip pointer at cn's grandparent slot resolves through
		 * cn->child = topmost_external_nodes, and ft_skip_to_compressed
		 * walks topmost->prev to recover cn.  With the atomic commit,
		 * skip-recovery never observes cn->child = topmost while
		 * topmost's back-edge still names the old holder that is about
		 * to be detached.
		 *
		 * Both slots resolve a flip-proxy on the read side:
		 * cell->parent through ft_resolve_head_prev, and prev through
		 * ft_dereference_prev_resolved.  So the folded descriptor is
		 * safe for concurrent readers.  The caller (ft_detach_node)
		 * reserved the +1 edge when topmost_external_nodes is set.
		 */
		{
			struct cds_ft_inode_flag *cn_flag =
				ft_compressed_node_flag(cn);

			if (ft->ordered_list) {
				struct ft_ord_cell *cell = ft_ord_cell_ptr(
					topmost_external_nodes->prev);

				ft_flip_txn_record_head_back_edge_owned(txn,
					(void **) &cell->parent,
					cell->parent, cn_flag,
					ft_back_edge_owner(cell->parent)
					FT_BE_SITE(FT_BE_DETACH_CN_PARENT, ctx));
			} else {
				ft_flip_txn_record_head_back_edge_owned(txn,
					(void **) &topmost_external_nodes->prev,
					topmost_external_nodes->prev, cn_flag,
					ft_back_edge_owner(
						topmost_external_nodes->prev)
					FT_BE_SITE(FT_BE_DETACH_CN_PARENT, ctx));
			}
		}
		/*
		 * nr_keys fold (LEAF Increment 2).  The kept compressed node
		 * @cn stays in place, and its subtree loses the disappearing
		 * key.  The removed leaf's -1 walk goes from @cn up to root and
		 * rides this promote commit atomically.  It is recorded before
		 * the publish dispatch below, so both commit arms carry it.
		 * The caller's count_reserve reserved it.  It is a no-op (and
		 * skipped) for a count-neutral move detach / !rank_stats.
		 */
		if (count_delta)
			ft_flip_txn_record_count_parent(ft, txn,
				ft_compressed_node_flag(cn), count_delta);
		/*
		 * External-promote publish into cn->child.  This is the moment
		 * the removed leaf key disappears for an exact reader, which
		 * descends through cn->child.
		 *
		 * When fusion is requested, record the publish's
		 * reader-visible edges: cn->child, plus cn's grandparent SKIP_X
		 * dual.  Commit them in one flip with the dead head cell's
		 * unsplice.  Every cn->child reader resolves a flip proxy now.
		 * Signal the fusion through pub->armed.
		 */
		if ((fuse_cell || run) && pub && !pub->armed) {
			struct ft_pub_rec rec = { .ctx = ctx, .n = 0,
				.mtxn = txn ? txn->mtxn : NULL };

			/* The caller folds only with neither. */
			urcu_assert_debug(!record_only);

			/* VALIDATE (§4.B): guard the LIVE kept compressed node cn.
			 * DLM: cn's RELEASE was acquired + recorded up front under
			 * lock_fine, so skip the incremental lock here. */
			if (!ft->lock_fine)
				ft_flip_txn_lock_or_guard_parent(ft, txn, ctx,
					ft_compressed_node_flag(cn), iter_depth);
			/*
			 * ☠ History: §9.3's third member was at first not
			 * taken here, because the attempt hung.  It is taken
			 * now: the next comment gives the real cause.
			 *
			 * This publish parent is a compressed node, so the
			 * republish does emit a SKIP_X dual into cn's own
			 * parent.  That is exactly the shape the other four
			 * producers now acquire.  Adding ft_lock_skip_dual_gp
			 * here wedged test_rekey_same_path_atomic_or_refused:
			 * "child killed by signal 14 (HANG -- the same-path
			 * move did not return)", 2 of 2, standalone, where
			 * HEAD was 2 of 2 green.  It was bisected to these two
			 * calls: the other three acquires were green with
			 * this one removed, and red with it alone.
			 *
			 * The mechanism named then is the one in the
			 * choke-point note at ft_flip_txn_lock_or_guard_parent
			 * -- "an
			 * op can arrive here holding this publish target's lock
			 * already, taken for a DIFFERENT member of its own set.
			 * Re-acquiring then misses against the op's OWN hold,
			 * which sets @acquire_miss and aborts the commit, and the
			 * caller retries into the identical shape: the op waits
			 * on itself, forever."  The line just above says cn's
			 * lock was "acquired + recorded up front under
			 * lock_fine".  So this frame is an op that arrives
			 * holding part of the set.
			 *
			 * ☠ "The op already holds it" measured 0 held of 40894
			 * across all 153 inv rows (ft_skip_dual_gp_held).  Read
			 * that number as a registry answer, not as a lock
			 * answer.  The ask consults @ctx.  An acquire taken
			 * through ft_dlm_acquire_set_at registers into no ctx
			 * at all.  So a hold that lives in a caller's local
			 * anchor array is invisible to the ask.  The identical
			 * 100%/0% split at ft_node_recompact's dual site was
			 * exactly that: 0 of 805202 through @ctx, and 490411 of
			 * 490411 asked of the set the function actually took.
			 *
			 * One reading here may still be right.
			 * ft_detach_node's {C, P, GP} names the grandparent of
			 * the op's iteration parent.  That is a different node
			 * from the dual's owner, which is derived from the
			 * back-pointer.  This instrument did not measure that
			 * reading.
			 *
			 * It does not matter for the kind.  The acquire below
			 * answers the question properly: REGISTERED or SHARED,
			 * the assert's own predicate.  @dual_gp_held now
			 * carries that answer.  The ask stays as a measurement
			 * of the registry's reach.
			 */
			(void) ft_skip_dual_gp_held(ft, ctx,
				ft_compressed_node_flag(cn),
				txn ? txn->mtxn : NULL);	/* measurement */
			/*
			 * §9.3's third member, taken here at last.  It used to
			 * hang, and the cause was never this site.  On a
			 * coarse trie, ft_lock_skip_dual_gp acquired nothing.
			 * It still fell through to the `guard:` tail of
			 * ft_flip_txn_lock_or_guard_parent, and planted a §4.B
			 * guard that the commit could not satisfy.  So every
			 * attempt aborted and the caller retried forever.
			 * Measured: 8,427,520 calls, all FT_LOG_EXIT_NOT_FINE,
			 * while the row never returned.  With the helper's
			 * coarse early-out the same call is green.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn,
				ft_compressed_node_flag(cn),
				txn ? txn->mtxn : NULL);
			_ft_publish_to_parent(ft, ft_compressed_node_flag(cn),
				&cn->child,
				(struct cds_ft_inode_flag *) topmost_external_nodes,
				elevated_old_child, &rec, dual_gp_held);
			if (ft_remove_commit_rec(ft, &rec, fuse_cell, run,
					txn, false) != URCU_TXN_STATUS_OK)
				/* Peer won: nothing installed (txn consumed). */
				return -EAGAIN;
			pub->armed = true;
		} else {
			struct ft_pub_rec rec = { .ctx = ctx, .n = 0,
				.mtxn = txn ? txn->mtxn : NULL };

			/*
			 * Non-fused external-promote.  There is no cell to
			 * unsplice: the list is off, or fusion is not
			 * requested / already armed.  Still route the publish
			 * through the op flip-txn.  The forward slot and a
			 * compressed grandparent's SKIP_X dual then flip
			 * atomically, with no torn forward/skip window.  A
			 * lone edge reduces to a single release store
			 * (ft_ord_cell_flip_one).  So this stays
			 * allocation-free and cannot fail for the common
			 * plain-parent case.
			 */
			/* VALIDATE (§4.B): guard the LIVE kept compressed node cn.
			 * DLM: cn's RELEASE was acquired + recorded up front under
			 * lock_fine, so skip the incremental lock here. */
			if (!ft->lock_fine)
				ft_flip_txn_lock_or_guard_parent(ft, txn, ctx,
					ft_compressed_node_flag(cn), iter_depth);
			/*
			 * ☠ History: §9.3's third member was at first not
			 * taken here, because the attempt hung.  It is taken
			 * now: the next comment gives the real cause.
			 *
			 * This publish parent is a compressed node, so the
			 * republish does emit a SKIP_X dual into cn's own
			 * parent.  That is exactly the shape the other four
			 * producers now acquire.  Adding ft_lock_skip_dual_gp
			 * here wedged test_rekey_same_path_atomic_or_refused:
			 * "child killed by signal 14 (HANG -- the same-path
			 * move did not return)", 2 of 2, standalone, where
			 * HEAD was 2 of 2 green.  It was bisected to these two
			 * calls: the other three acquires were green with
			 * this one removed, and red with it alone.
			 *
			 * The mechanism named then is the one in the
			 * choke-point note at ft_flip_txn_lock_or_guard_parent
			 * -- "an
			 * op can arrive here holding this publish target's lock
			 * already, taken for a DIFFERENT member of its own set.
			 * Re-acquiring then misses against the op's OWN hold,
			 * which sets @acquire_miss and aborts the commit, and the
			 * caller retries into the identical shape: the op waits
			 * on itself, forever."  The line just above says cn's
			 * lock was "acquired + recorded up front under
			 * lock_fine".  So this frame is an op that arrives
			 * holding part of the set.
			 *
			 * ☠ "The op already holds it" measured 0 held of 40894
			 * across all 153 inv rows (ft_skip_dual_gp_held).  Read
			 * that number as a registry answer, not as a lock
			 * answer.  The ask consults @ctx.  An acquire taken
			 * through ft_dlm_acquire_set_at registers into no ctx
			 * at all.  So a hold that lives in a caller's local
			 * anchor array is invisible to the ask.  The identical
			 * 100%/0% split at ft_node_recompact's dual site was
			 * exactly that: 0 of 805202 through @ctx, and 490411 of
			 * 490411 asked of the set the function actually took.
			 *
			 * One reading here may still be right.
			 * ft_detach_node's {C, P, GP} names the grandparent of
			 * the op's iteration parent.  That is a different node
			 * from the dual's owner, which is derived from the
			 * back-pointer.  This instrument did not measure that
			 * reading.
			 *
			 * It does not matter for the kind.  The acquire below
			 * answers the question properly: REGISTERED or SHARED,
			 * the assert's own predicate.  @dual_gp_held now
			 * carries that answer.  The ask stays as a measurement
			 * of the registry's reach.
			 */
			(void) ft_skip_dual_gp_held(ft, ctx,
				ft_compressed_node_flag(cn),
				txn ? txn->mtxn : NULL);	/* measurement */
			/*
			 * §9.3's third member, taken here at last.  It used to
			 * hang, and the cause was never this site.  On a
			 * coarse trie, ft_lock_skip_dual_gp acquired nothing.
			 * It still fell through to the `guard:` tail of
			 * ft_flip_txn_lock_or_guard_parent, and planted a §4.B
			 * guard that the commit could not satisfy.  So every
			 * attempt aborted and the caller retried forever.
			 * Measured: 8,427,520 calls, all FT_LOG_EXIT_NOT_FINE,
			 * while the row never returned.  With the helper's
			 * coarse early-out the same call is green.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn,
				ft_compressed_node_flag(cn),
				txn ? txn->mtxn : NULL);
			_ft_publish_to_parent(ft, ft_compressed_node_flag(cn),
				&cn->child,
				(struct cds_ft_inode_flag *) topmost_external_nodes,
				elevated_old_child, &rec, dual_gp_held);
			if (ft_remove_commit_rec(ft, &rec, NULL, NULL, txn,
					record_only) != URCU_TXN_STATUS_OK)
				/* Peer won: nothing installed (txn consumed). */
				return -EAGAIN;
		}
		return 0;
	}
	{
		struct cds_ft_inode *fresh;
		struct cds_ft_metadata *fresh_meta;
		struct cds_ft_metadata *src_meta;

		fresh = alloc_cds_ft_node(ft, &ft_types[0], &fresh_meta);
		if (!fresh)
			return -ENOMEM;	/* nothing published: caller destroys @txn */
		src_meta = cds_ft_item_to_metadata(
			(struct cds_ft_inode *) src_cn);
		/*
		 * Recover the grandparent (parent, slot) as one coherent
		 * snapshot from src_meta, exactly as the chain-compress
		 * publishes do.  Never pair the descent-captured
		 * @detach_parent_flag_ptr with a fresh src_meta->parent.
		 *
		 * Between the descent and here, a peer can recompact the
		 * grandparent to a larger node type.  Or it can re-home it
		 * (Phase 4.3).  The captured slot then points into the old
		 * parent body, which is now replaced.  But src_meta->parent
		 * already names the new one.  Pairing the stale slot with the
		 * fresh parent indexes the new parent's bitmap out of bounds
		 * (ft_slot_to_byte OOB assert).
		 *
		 * ft_resolve_parent_slot computes the slot as parent_body +
		 * offset, so it is in-bounds by construction.  If the
		 * grandparent or src_cn raced after this snapshot, the txn
		 * guard, the forward CAS and the tombstone freeze below abort
		 * the commit.
		 */
		struct cds_ft_inode_flag *pub_parent;
		struct cds_ft_inode_flag **pub_slot =
			ft_resolve_parent_slot(src_meta, ft, &pub_parent);

		/*
		 * The fresh replacement is empty: nothing below sets a child
		 * on it.  This sub-case exists for the root: "a compressed
		 * root from detach/graft_swap must remain internal" (the
		 * header above).  An empty internal at the root is the empty
		 * trie, which is legal.
		 *
		 * Reached with a non-NULL parent, the same publish wires a
		 * childless internal into a live parent slot, and that node
		 * stays there.  Each of its fields is valid: a count of 0, a
		 * correct back-pointer, the smallest type.  It holds nothing.
		 * It is the "dead interior node" that cds_ft_verify names, and
		 * it breaks ordered navigation.  The empty-subtree arm of the
		 * inequality descent rests on the premise that a slot-emptied
		 * internal is never left in place.  So a walk that reaches it
		 * either re-enters the branch forever, or climbs out and skips
		 * the remaining subtree.  Exact-key lookup still finds every
		 * stranded key, which is why nothing else reported it.
		 *
		 * The plan that led here is stale; the tree is not broken.  So
		 * bail and let the caller re-descend, exactly as the rewind
		 * and DLM acquire-miss bails above do.  This is pre-commit.
		 * Free the unpublished node and destroy the caller-owned txn.
		 * (The caller skips the destroy on -EAGAIN: it assumes a
		 * commit consumed it.)  The structure is left byte-for-byte as
		 * it was.
		 */
		if (pub_parent != NULL) {
			free_cds_ft_node_unpublished(ft, fresh);
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}

		fresh_meta->parent_word = ft_parent_word(ft, pub_parent);
		/*
		 * DLM Step 1: acquire {src_cn (RETIRE), pub_parent (RELEASE)}
		 * in one MCAS up front, with the guard
		 * src_cn.parent==pub_parent.  This replaces the pub_parent
		 * lock_or_guard and the plain src_cn tombstone: a peer state
		 * change under the fence now aborts.  This
		 * compressed->fresh-internal sub-case is test-under-covered,
		 * so it mirrors the chain-compress pattern exactly.  With
		 * pub_parent NULL (compressed root retire), the set is
		 * {src_cn} only.
		 */
		struct cds_ft_metadata *src_cn_meta_a =
			cds_ft_item_to_metadata((struct cds_ft_inode *) src_cn);
		struct ft_held_anchor src_held;
		unsigned int pp_depth = 0;
		bool dlm_a2 = false;

		/*
		 * @pub_parent came from src_cn's back-pointer, which carries no
		 * depth.  The descent's window is what dates it.  A parent this
		 * descent never passed cannot be anchored here at all.  Re-plan
		 * in that case; do not anchor it by another node's depth.
		 */
		if (pub_parent && !ft_lock_ctx_depth_of(ft, ctx, pub_parent,
				&pp_depth)) {
			free_cds_ft_node_unpublished(ft, fresh);
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		if (ft->lock_fine) {
			struct ft_dlm_member set[2];
			int dret;

			set[0] = (struct ft_dlm_member){
				.nf = ft_compressed_node_flag(src_cn),
				.node = src_cn_meta_a,
				.depth = iter_depth,
				.guard_child = src_cn_meta_a,
				.guard_pf = pub_parent };
			set[1] = (struct ft_dlm_member){ .nf = pub_parent,
				.node = pub_parent ?
					ft_flag_to_metadata(ft, pub_parent) : NULL,
				.depth = pp_depth };
			dret = ft_dlm_acquire_set(ft, ctx, set, 2);
			if (dret) {
				free_cds_ft_node_unpublished(ft, fresh);
				/*
				 * Pre-commit bail.  On -EAGAIN the caller does
				 * not destroy the caller-owned txn, so destroy
				 * it here.  On -ENOMEM the caller destroys it
				 * (its != -EAGAIN arm).
				 */
				if (dret == -EAGAIN)
					if (!record_only)
						ft_flip_txn_destroy(txn);
				return dret;	/* nothing acquired (all-or-none) */
			}
			src_held = set[0].held;
			/*
			 * Coarsening put src_cn's lock on an ancestor that
			 * survives this retire.  So the fused
			 * {LOCK|s -> TOMBSTONE|s} splits.  The ancestor is
			 * released here, before any guard lands on it (the
			 * ordering rule).  The node is tombstoned below.  This
			 * is a no-op where the two words coincide.
			 */
			/*
			 * ☠ Not for a shared hold.  set[0] dedupes onto a word
			 * this op already holds.  At root-only, that is an
			 * orphan whose anchor is the root, and on this arm the
			 * root is src_cn.  ft_detach_freeze_one has then
			 * already registered that word in the orphan txn.
			 *
			 * Registering it again here made every pre-commit
			 * terminal release one word twice:
			 * ft_flip_txn_lock_release_all does not dedupe.  The
			 * second release clears a peer's fence if the root was
			 * re-taken in between.
			 *
			 * The helper's own contract says callers gate on
			 * @shared (its assert), exactly as set[1] below does.
			 * The retire below handles @shared itself.
			 *
			 * History: the adversarial review of an under-lock
			 * plan check for this arm's freeze found this.  The
			 * -EAGAIN bail of that check reaches this terminal on
			 * every stale plan.
			 */
			if (!src_held.shared) {
				ft_flip_txn_lock_register_held(txn, &src_held);
				ft_flip_txn_record_anchor_release(txn, &src_held,
					src_cn_meta_a);
			}
			if (pub_parent && !set[1].held.shared) {
				ft_flip_txn_lock_register_held(txn, &set[1].held);
				ft_flip_txn_record_release_lock(txn,
					set[1].held.lock, set[1].held.lock_snap);
			}
			dlm_a2 = true;
			/*
			 * This is the same plan read as the topmost arm's.
			 * Nothing here records cn->child at all.  Retiring
			 * src_cn for an empty fresh node is only right while
			 * its child is still the one the climb condemned.
			 */
#ifndef FT_DEBUG_NO_CN_CHILD_RECHECK
			if (!record_only &&
					ft_resolve_flip_proxy(rcu_dereference(
						src_cn->child)) != elevated_old_child) {
				FT_DBG_RETRY_SITE();
				free_cds_ft_node_unpublished(ft, fresh);
				ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
#endif
		}
		/*
		 * After the acquire: the leaf sits straight under @src_cn, which
		 * this arm retires and has just locked.  See the function header.
		 */
		if (freeze_leaf) {
			ft_ch_audit_ctx(ft, txn, ctx, freeze_leaf);
			/*
			 * ★ Validate the plan under the lock.  The reason, the
			 * holder argument and the disposition are the same as
			 * in the promote arm above.  Only the unpublished
			 * @fresh exists.  The unwind is the same as the
			 * acquire's bail above.
			 */
			if (ft->lock_fine && !record_only &&
					!ft_hlist_chain_plan_ok(ft,
						ft_flip_txn_handle(txn),
						freeze_leaf, freeze_len)) {
				FT_HLIST_PLAN_BAIL();
				FT_DBG_RETRY_SITE();
				free_cds_ft_node_unpublished(ft, fresh);
				ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
			FT_HLIST_PLAN_OBSERVE(ft, ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len, 2);
			ft_hlist_freeze_chain_prepare(ft, ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len);
		}
		/*
		 * nr_keys fold (LEAF Increment 2).  The fresh internal replaces
		 * the retired compressed node, so it carries the retired node's
		 * post-removal count.  That count is get(src) + count_delta,
		 * i.e. src - 1 when this detach retires a key.  It is
		 * typically 0: a compressed root emptied of its lone leaf.
		 * The store is plain and build-invisible, on the fresh node
		 * that is not published yet.  It is a no-op when !rank_stats.
		 * The -1 walk from the fresh node's stable parent rides the
		 * publish below.
		 */
		if (count_delta)
			ft_nr_keys_store(ft, fresh_meta,
				ft_nr_keys_get(src_meta) + count_delta, CMM_RELAXED);
#ifdef FEATURE_FT_SKIP_COMPRESSED
		ft_meta_parent_slot_offset_set(fresh_meta,
			ft_meta_parent_slot_offset(src_meta));
#endif
		{
			struct ft_pub_rec rec = { .ctx = ctx, .n = 0,
				.mtxn = txn ? txn->mtxn : NULL };

			/*
			 * Compressed -> fresh-internal recompaction publish.
			 * Route the forward slot (and a compressed
			 * grandparent's SKIP_X dual) through the op flip-txn,
			 * so that they flip atomically.  A lone edge stays a
			 * single release store.
			 */
			/* VALIDATE (§4.B): lock (or guard-fallback) the
			 * coherently-resolved grandparent, which is the
			 * value-swap target (§10.5).  DLM: pub_parent's
			 * RELEASE was acquired up front under lock_fine. */
			if (!ft->lock_fine)
				ft_flip_txn_lock_or_guard_parent(ft, txn, ctx,
					pub_parent, pp_depth);
			/*
			 * §9.3's third member.  @pub_parent may be compressed.
			 * This publish then also emits its SKIP_X dual into a
			 * grandparent the op holds nothing of.  Acquire it from
			 * the same derivation the record names.  (Kind still
			 * false -- the eight producers flip TOGETHER; see
			 * ft_node_recompact's dual site, where the last of them
			 * was shown to hold its owner after all.)
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn,
				pub_parent, txn ? txn->mtxn : NULL);
			_ft_publish_to_parent(ft, pub_parent,
				pub_slot,
				ft_node_flag(fresh, 0),
				*pub_slot, &rec, dual_gp_held);
			/*
			 * Freeze the retired compressed node dead (§4.B
			 * freeze-on-free).  This compressed->fresh-internal
			 * recompaction retires the old compressed node like
			 * every other retire.  Record the tombstone into @txn,
			 * so that it flips atomically with the publish that
			 * unlinks the node.  An aborted publish leaves the
			 * node live (atomic detach).
			 *
			 * The current test suite does not reach this detach
			 * sub-case (verified: zero hits across ft_unit +
			 * ft_inv, both list modes).  So the freeze-on-free
			 * audit never exercises it.
			 *
			 * DLM: src_cn was LOCK-acquired up front, so record
			 * the fenced terminal: a peer state change aborts.
			 * The terminal has two forms:
			 *  - fused with the release, where the acquire took
			 *    src_cn's own word;
			 *  - plain against src_cn's clean word, where
			 *    coarsening sent the lock to a surviving ancestor.
			 */
			if (dlm_a2)
				ft_flip_txn_record_retire_anchored(txn, ctx,
					&src_held, src_cn_meta_a);
			else
				ft_flip_txn_record_tombstone(txn, cds_ft_item_to_metadata(
					(struct cds_ft_inode *) src_cn));
			/*
			 * nr_keys fold (LEAF Increment 2).  The removed leaf's
			 * -1 walk goes from the fresh node's stable parent up
			 * to root and rides this publish.  The fresh node
			 * itself was baked above.  A compressed root retire
			 * leaves src_meta->parent == NULL.  The walk then
			 * records no ancestor edge, and only the baked root
			 * count changes.
			 */
			if (count_delta)
				ft_flip_txn_record_count_parent(ft, txn,
					pub_parent, count_delta);
			if (ft_remove_commit_rec(ft, &rec, NULL, NULL, txn,
					record_only) != URCU_TXN_STATUS_OK) {
				/*
				 * Peer won: the fresh internal was never
				 * published.  The retired compressed node
				 * stays live and linked: its tombstone was
				 * discarded with the aborted commit.
				 */
				free_cds_ft_node_unpublished(ft, fresh);
				return -EAGAIN;
			}
		}
		free_compressed_node(ft, src_cn);
	}
	return 0;
}

#ifdef FEATURE_FT_SKIP_COMPRESSED
/*
 * ft_canonicalize_chain_compress: collapse a non-root 1-child internal
 * node with no external_nodes into a single compressed node.  Merge it
 * with an adjacent compressed parent/child through the 4-case
 * chain-merge:
 *
 *  - parent non-compressed, child non-compressed:
 *      [iter_internal] -> [new_cn(1 byte)]; child preserved.
 *  - parent non-compressed, child compressed:
 *      [iter_internal, child_cn] -> [new_cn(1 + child_cn.len bytes)];
 *      new_cn.child = child_cn.child.
 *  - parent compressed, child non-compressed:
 *      [parent_cn, iter_internal] -> [new_cn(parent_cn.len + 1 bytes)];
 *      new_cn.child = surviving_child.
 *  - parent compressed, child compressed:
 *      [parent_cn, iter_internal, child_cn] ->
 *      [new_cn(parent_cn.len + 1 + child_cn.len bytes)];
 *      new_cn.child = child_cn.child.
 *
 * This preserves the "no two adjacent compresseds" invariant: the new
 * node absorbs adjacent compressed neighbours.  The merged length is
 * bounded by FT_SKIP_LEN_MAX (uint8_t cn->len).  When it would exceed
 * the bound, the function leaves non-canonical residue.  Subsequent
 * inserts may rebuild canonical form.  Allocation failure takes the
 * same fallback.
 *
 * Preconditions (caller asserts):
 *   - iter_meta nr_child == 1
 *   - @iter_meta->external_nodes == NULL
 *   - @iter_meta->parent != NULL  (non-root)
 *
 * @iter_node_flag: the 1-child internal being collapsed.
 * @iter_meta:      its metadata.
 * @slot_ptr:       parent slot pointing to @iter_node_flag.  Used only
 *                  when the parent is non-compressed; compressed parents
 *                  resolve their own slot via parent_slot_offset.
 */
#endif	/* FEATURE_FT_SKIP_COMPRESSED */
/*
 * Freeze-on-free (doc §4.B) for the ft_detach_node branch-2 orphan chain: mark
 * every collected orphan, and the trailing skip-target, dead.
 *
 * This helper is config-agnostic.  The branch-2 orphan chain forms with or
 * without skip compression, and ft_skip_to_compressed has an unconditional
 * non-skip stub.  The helper is called from a path (ft_detach_node) that is
 * reachable outside the skip block.  So it lives outside the
 * FEATURE_FT_SKIP_COMPRESSED guard.
 *
 * @txn non-NULL records each tombstone into the op's commit txn.  The freeze
 * then flips atomically with the flip that unlinks the chain (atomic detach).
 * @txn NULL falls back to a standalone lone-edge flip.  That is the list-off
 * pub-less lone-store path, whose unlink is not a txn commit; it is a no-op
 * under one writer.  Node resolution mirrors the branch-2 free loop.
 *
 * DLM plan-lock (lock_fine): @held non-NULL means that the caller
 * lock-acquired each orphan at collection, so the collapse decision was made
 * on a frozen state word.  @held is parallel to @orphans, and @trailing_held
 * is for the trailing skip-target.  Record the fenced
 * {LOCK|s -> TOMBSTONE|s} terminal from that snapshot instead of the plain RYW
 * tombstone.  A peer that grew the orphan tears the fenced expected-old and
 * aborts this commit (§9.2 orphan-chain coherence).
 *
 * In practice a peer cannot even get that far.  ft_meta_nr_child_inc spins on
 * FT_STATE_INPLACE_WAIT_MASK, which includes FT_STATE_LOCK unconditionally.
 * So the peer waits for the mark, and the coherence comes from exclusion, not
 * from detect-and-abort.
 *
 * @held NULL keeps the plain path byte-identical.  Under DLM @txn is always
 * non-NULL (commit_txn is FORCE-TXN and pub is never NULL), so the fenced arm
 * never needs the standalone fallback.
 */
/*
 * Freeze one orphan from the acquire that protects it.  Lift the lock off the
 * ancestor that survives the retire (a no-op where the orphan carries its
 * own).  Then tombstone the orphan against its own clean word.  A member that
 * deduped onto a word an earlier orphan took owes no release: one word, one
 * terminal.
 *
 * A surviving anchor is handed to @txn outright, with its release record and
 * its registry entry.  The caller's sweep then stops owning it (struct
 * ft_held_anchor's @txn_owned).  The two owners cannot be reconciled after
 * the fact:
 *  - the release leaves that word clean and live;
 *  - a peer takes it immediately (under a coarse spacing every op wants the
 *    same ancestor);
 *  - a later "release it if it is still locked" then strips the peer's mark.
 *
 * Where the anchor is the retired node, the fused tombstone is its terminal
 * and the word is unlockable afterwards.  So that mark stays with the caller
 * and costs no registry slot.  This keeps a FT_MAX_DEPTH orphan chain inside
 * FT_FLIP_TXN_MAX_LOCKS, and the per-node granularity byte-identical.
 *
 * @h_dies_first: @h is in a frame that returns before @txn's terminal runs.
 * That frame is ft_detach_node's @orphan_held[] under a deferred commit
 * (@record_only: the rekey commits @shared_txn after the detach has returned).
 * The hand-off then links no silencer (ft_flip_txn_lock_own_frame).  Measured
 * with -DFT_DEBUG_STACK_SILENCER: the terminal otherwise wrote through a
 * pointer into the dead frame, once per ft_unit run at exponential spacing.
 */
static inline
void ft_detach_freeze_one(struct ft_flip_txn *txn,
		const struct ft_lock_ctx *ctx,
		struct ft_held_anchor *h, struct cds_ft_metadata *m,
		bool h_dies_first)
{
#ifdef FT_DEBUG_FREEZE_SILENCER_ON_STACK
	h_dies_first = false;	/* red control: link it anyway */
#endif
	if (!h->shared) {
		unsigned int slot = 0;
		bool owned = false;
		bool retires;

		/* Register BEFORE recording: the record asks who owns the word. */
		if (h->lock != m) {
			slot = ft_flip_txn_lock_own_frame(txn, h, h_dies_first);
			owned = true;
		}
		/*
		 * Only the record's own early-out knows whether this word ends
		 * released or retired.  The terminal scrub needs that answer.
		 * It cannot re-derive it from the word afterwards without
		 * racing a peer's retire.
		 */
		retires = ft_flip_txn_record_anchor_release(txn, h, m);
		if (owned && retires)
			txn->locks[slot].tombstone_terminal = true;
#ifndef FT_DEBUG_FREEZE_TOMBSTONE_MW
		/*
		 * ...and where the anchor is the orphan, say that this txn
		 * holds it.
		 *
		 * That mark stays with the caller (no registry slot, see
		 * above).  So the retire's record-time ownership question
		 * answered "no", and the fused {LOCK|s -> TOMBSTONE|s} went MW.
		 * That was 1.33M records per ft_inv FT_INV_MW=1 run at
		 * per-node, every one on a word this op itself took.
		 *
		 * The expected-old check of that record was the MW-CAS era's
		 * detect-and-abort for a peer that grew the orphan.  Under the
		 * lock that peer cannot even start: ft_meta_nr_child_inc waits
		 * on FT_STATE_LOCK.  A peer's {live -> live} validate cannot
		 * land on a locked word either.
		 *
		 * @covered answers ft_flip_txn_owns without taking on a
		 * release (the caller's sweep keeps that), so the terminal
		 * parks SW.  It settles outside the registry's late pass.
		 * That is harmless for a tombstone: the word is unlockable
		 * from the moment it lands.  A full @covered drops the entry
		 * and the record stays MW.
		 */
		if (h->lock == m)
			ft_flip_txn_cover_member(txn, m);
#endif
	}
	ft_flip_txn_record_retire_anchored(txn, ctx, h, m);
}

static
void ft_detach_freeze_orphans(struct cds_ft *ft, struct ft_flip_txn *txn,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag **orphans, int nr_orphans,
		struct cds_ft_inode_flag *trailing_skip_cn_flag,
		struct ft_held_anchor *held,
		struct ft_held_anchor *trailing_held,
		bool held_dies_first)
{
	int i;

	for (i = 0; i < nr_orphans; i++) {
		struct cds_ft_metadata *m = ft_node_compressed(orphans[i])
			? cds_ft_item_to_metadata((struct cds_ft_inode *)
				ft_compressed_node_ptr(orphans[i]))
			: cds_ft_item_to_metadata(ft_node_ptr(orphans[i]));
		/*
		 * An orphan is a link of the condemned chain.  It is keyless
		 * and single-child, or it is the one junction whose head the
		 * climb promoted.  It is held here.  A second child means that
		 * the walk that collected it left the plan, and retiring it
		 * takes a live key with it.  The phase-2 target check in
		 * ft_detach_node is the measured instance.
		 */
		urcu_assert_debug(ft_node_compressed(orphans[i]) ||
			ft_meta_nr_child_load(m) <= 1);

		if (held)
			ft_detach_freeze_one(txn, ctx, &held[i], m,
				held_dies_first);
		else if (txn)
			ft_flip_txn_record_tombstone(txn, m);
		else
			ft_meta_tombstone_set_flip(m);
	}
	if (trailing_skip_cn_flag) {
		struct cds_ft_metadata *m = cds_ft_item_to_metadata(
			(struct cds_ft_inode *) ft_skip_to_compressed(ft,
				trailing_skip_cn_flag));

		if (held)
			ft_detach_freeze_one(txn, ctx, trailing_held, m,
				held_dies_first);
		else if (txn)
			ft_flip_txn_record_tombstone(txn, m);
		else
			ft_meta_tombstone_set_flip(m);
	}
}
/*
 * DLM orphan plan-lock (§9.2, lock_fine): lock-acquire orphan @m at
 * collection.  Record it into ft_detach_node's function-scope lock arrays,
 * which are parallel to its to_free[].
 *
 * @require_single_child validates that the marked snapshot has
 * nr_child == 1.  That is the elevated-ancestor invariant the collapse rests
 * on.  A peer that grew the orphan between the plan climb and this mark then
 * forces a re-descend instead of a silent retire.  A plain compressed node is
 * structurally single-child and skips the check.  A concurrent restructure of
 * it dirties its word and fails the mark.
 *
 * Return -EAGAIN on a dirty word or a grown orphan.  The caller then does
 * `goto end`, whose sweep releases the marks already held.  Return 0 after
 * storing the hold at @locked[*n] and advancing *n.  The snapshot feeds the
 * fenced {LOCK|s -> TOMBSTONE|s} tombstone at freeze.
 */
/*
 * Byte-depth of @parent_nf's immediate child: one key byte past a bitmap node,
 * the whole run past a compressed one.
 */
static inline
unsigned int ft_child_depth_of(const struct cds_ft *ft,
		struct cds_ft_inode_flag *parent_nf, unsigned int parent_depth)
{
	return parent_depth + ft_node_span(ft, parent_nf);
}

/*
 * Extend the descent over one step of a writer walk.  Position the cursor on
 * @nf at @depth, then enter it, so that the anchor table covers the levels its
 * span crosses.  The next node down is then anchored by plain
 * ft_descent_anchor, exactly as if the original descent had walked here.
 * Return the next node's byte-depth.
 *
 * This is a no-op where no descent ran.
 *
 * ☠ It is not a no-op under per-node granularity any more.
 * ft_descent_enter_node returns early there for the anchor table, but the
 * ancestor-ledger push sits before that return.  So this extends the ledger
 * at every spacing.  The descent it is handed must therefore be one that is
 * allowed to write (see ft_detach_node's copy, which is not).
 */
static inline
unsigned int ft_walk_extend(struct ft_descent *d, bool valid,
		struct cds_ft_inode_flag *nf, unsigned int depth,
		unsigned int span)
{
	if (valid) {
		d->nf = nf;
		d->depth = depth;
#ifdef FT_DEBUG_ANC_LEDGER
		if (d->anc_rec)
			uatomic_inc(&ft_anc_rec_copy_push);
#endif
		ft_descent_enter_node(d, nf, depth, span);
		d->depth = depth + span;
	}
	return depth + span;
}

/*
 * Acquire an orphan the detach's downward walk collected.
 *
 * The walk moves below the descent's cursor, so @depth comes from the walk
 * itself.  ft_walk_extend has entered every node above this one, so the
 * anchor table covers it.
 *
 * ☞ An -EAGAIN from here is no longer the whole call failing.  This comment
 * used to say it was: "remove_all has NO retry loop ... so an -EAGAIN from
 * here is the whole CALL failing and the caller's to retry".
 * cds_ft_remove_all now loops.  The refusal ages a conflict on the op's
 * persistent txn and the attempt is re-derived.  So refusing here costs an
 * attempt, not the call.
 *
 * It is not a MEMORY_ERROR either.  The tail maps `-EAGAIN` to that retry and
 * `-ENOMEM` to MEMORY_ERROR.  That mapping only became trustworthy once
 * @acquire_enomem gave an allocation failure inside the acquire its own
 * channel.  Before, such a failure arrived as an -EAGAIN no peer produced.
 *
 * Both sentences are corrected, not deleted.  Each read as an open bug, and
 * sends the next reader after something already fixed.
 */
static inline
int ft_detach_orphan_acquire_at(const char *fn, int line,
		const struct cds_ft *ft,
		const struct ft_lock_ctx *ctx, struct cds_ft_inode_flag *nf,
		unsigned int depth, struct cds_ft_metadata *m,
		struct ft_held_anchor *held)
{
	return ft_acquire_member_at(fn, line, ft, ctx, nf, m, depth, held);
}

#define ft_detach_orphan_acquire(ft, ctx, nf, depth, m, held)		\
	ft_detach_orphan_acquire_at(__func__, __LINE__, (ft), (ctx),	\
		(nf), (depth), (m), (held))

/*
 * Return the link below orphan @nf on a detach's downward walk.  It is a
 * compressed node's child, or an internal node's first (single) child.  Read
 * it only once @nf is marked: the walk's verdict on the next link rests on
 * this value.
 */
static inline
struct cds_ft_inode_flag *ft_detach_walk_next(const struct cds_ft *ft,
		struct cds_ft_inode_flag *nf)
{
	struct cds_ft_inode_flag *next = NULL;
	unsigned int key;

	if (ft_node_compressed(nf))
		return ft_compressed_node_ptr(nf)->child;
	for (key = 0; key < 256; key++) {
		next = ft_node_get_nth(ft, nf, NULL, (uint8_t) key,
			FT_PF_NONE);
		if (next)
			break;
	}
	return next;
}

static inline
int ft_detach_orphan_planlock(const struct cds_ft *ft,
		const struct ft_lock_ctx *ctx, struct cds_ft_inode_flag *nf,
		unsigned int depth, struct cds_ft_metadata *m,
		bool require_single_child,
		struct ft_held_anchor *locked, int *n)
{
	struct ft_held_anchor h;

	if (ft_detach_orphan_acquire(ft, ctx, nf, depth, m, &h))
		return -EAGAIN;
	if (require_single_child && ft_state_nr_child(h.node_snap) != 1) {
		if (!h.shared)
			ft_meta_lock_release(h.lock);
		return -EAGAIN;
	}
	locked[(*n)++] = h;
	return 0;
}
/*
 * What a folded collapse owes its caller.  Under @record_only the collapse is
 * recorded and not committed, so the chain it retires is still live and linked
 * when it returns.  The nodes are handed back for the caller to reclaim after
 * its own commit lands.
 *
 * ☠ This is defined outside the FEATURE_FT_SKIP_COMPRESSED block that holds
 * the collapse itself.  struct ft_detach_recompact_out embeds it by value, and
 * that struct exists in every build.  A build with skip-compression off has
 * no collapse to run, but it still needs the type to be complete.
 */
#ifdef FT_DBG_RA_UNSPLICE
/*
 * Arm and yield counters for cds_ft_remove_all's two-commit ordered-list
 * tail.  They were built because a red control lied.
 *
 * Restoring the discarded-status form of that unsplice left
 * inv_concurrent_remove_all_list green.  That reads as "the drive-forward
 * buys nothing".  The truth was "the branch never ran".  Every removal in
 * that row's leaf shapes fuses the unsplice into its structural flip.  So
 * @pub is always armed and the two-commit tail is dead code there.  Measured
 * with this counter: two_commit_arm=0 over 150 rounds x 64 keys x 3 lanes.
 *
 * Arm presence and arm yield are different facts.  Only the counter tells a
 * green control apart from an unexecuted one.
 */
unsigned long ft_dbg_ra_unsplice_arm, ft_dbg_ra_unsplice_abort;
unsigned long ft_dbg_ra_unsplice_fused;
/* ...and the back-pointer/descent disagreement the exponential bail refuses. */
unsigned long ft_dbg_ra_stale_descent;

__attribute__((destructor))
static void ft_dbg_ra_unsplice_report(void)
{
	fprintf(stderr, "FT RA UNSPLICE: two_commit_arm=%lu abort=%lu fused=%lu\n",
		ft_dbg_ra_unsplice_arm, ft_dbg_ra_unsplice_abort,
		ft_dbg_ra_unsplice_fused);
	fprintf(stderr, "FT RA STALE DESCENT: exponential_bail=%lu\n",
		ft_dbg_ra_stale_descent);
}
# define FT_DBG_RA_UNSPLICE_ARM()	uatomic_inc(&ft_dbg_ra_unsplice_arm)
# define FT_DBG_RA_UNSPLICE_ABORT()	uatomic_inc(&ft_dbg_ra_unsplice_abort)
# define FT_DBG_RA_UNSPLICE_FUSED()	uatomic_inc(&ft_dbg_ra_unsplice_fused)
# define FT_DBG_RA_STALE_DESCENT()	uatomic_inc(&ft_dbg_ra_stale_descent)
#else
# define FT_DBG_RA_UNSPLICE_ARM()	do { } while (0)
# define FT_DBG_RA_UNSPLICE_ABORT()	do { } while (0)
# define FT_DBG_RA_UNSPLICE_FUSED()	do { } while (0)
# define FT_DBG_RA_STALE_DESCENT()	do { } while (0)
#endif

#ifdef FT_ENABLE_TRACING
/*
 * Arm yield, not arm presence.  "The check compiled in" and "the check ever
 * REFUSED anything" are different facts.  Only the second says whether a
 * guard is doing work.  The first attempt at the detach arm was armed ~100k
 * times per run and refused nothing.  Its target had been re-read so late
 * that it was comparing a value against itself.  A guard without a yield
 * counter cannot tell that apart from a race that did not happen.
 */
unsigned long ft_dbg_plan_stale_ext, ft_dbg_plan_stale_detach;
/* ...and the orphan walk's own stale-plan refusal. */
unsigned long ft_dbg_orphan_walk_stale;
/* ...split by arm: the record_only FOLD path is the one whose abort
 * safety rests on the CALLER destroying the shared txn. */
unsigned long ft_dbg_plan_stale_fold, ft_dbg_plan_stale_alone;
#endif

/*
 * What the op intends, as opposed to what the trie currently holds.
 *
 * ☠ The distinction is the whole point.  A collapse rests on a picture of the
 * boundary: how many children it has, which one survives, whether it carries
 * an external head.  The caller used to read that picture, before the callee
 * took the boundary's lock.  Under fine locking the mark is the exclusion, so
 * a fact read before it is worth nothing at commit time.  A peer that legally
 * holds the same lock changes the node in between, and the collapse ratifies
 * a picture that is already false.  Measured: a peer published a fresh
 * junction carrying a live key into a body slot of the boundary and
 * committed.  This collapse re-entered, retired the boundary, and freed the
 * key with it.
 *
 * So the callee derives every trie fact under its own mark and trusts none
 * from the caller.  It cannot derive the op's target.  A query can be
 * hoisted; a target cannot: only the op knows which child it came to remove.
 * This struct carries the target and nothing else.
 *
 * @detach_child / @detach_byte: the body child this commit removes from the
 *   boundary, and its byte.  NULL when the op removes no body child (the
 *   post-removal collapses, whose entry is the EXTERNAL one).
 *   ★ At the SHAPE-D fold it is the climb's own @elevated_old_child with its
 *   byte @n.  That is the pair the climb condemned.  Both are fixed before
 *   the boundary is acquired.  @n is set once (ft_node_find_child on @cur)
 *   after the last elevation.  @detach_node_flag_ptr is a slot inside @cur
 *   throughout.  So the pair names the boundary's own edge.
 *   ☞ ft-remove.h already enforces it PRE-fence (@plan_old_child).  Carrying
 *   it here is that same predicate moved past the acquire.  Only there can it
 *   speak for the commit.
 * @expect_ext: the external head the op's shape says the boundary carries.
 *   It is the head the op is itself removing, or NULL for the callers whose
 *   gate asserts there is none.  It is never a value read speculatively from
 *   the trie: each caller names the head it came to unlink.
 */
struct ft_chain_compress_intent {
	struct cds_ft_inode_flag *detach_child;
	struct cds_ft_node *expect_ext;
	uint8_t detach_byte;
};

/*
 * Check two facts the collapse rests on.  Return true when the plan no longer
 * describes the trie.  The caller must then bail.
 *
 * The first fact is the boundary's external head, which the boundary's own
 * mark did not use to cover.  It is read here, under that mark, and compared
 * against the head the op named.  It is never compared against a picture the
 * caller took before the mark.
 *
 * The second fact is the child this commit detaches.  A peer's publish into
 * that slot is a same-slot value swap: it moves neither the count nor the
 * survivor.  So this is the only word that can see it.
 */
static inline
bool ft_chain_compress_plan_stale(struct cds_ft_inode_flag *iter_node_flag,
		const struct ft_chain_compress_intent *intent)
{
	if (ft_node_external_nodes(iter_node_flag) != intent->expect_ext) {
#ifdef FT_ENABLE_TRACING
		uatomic_inc(&ft_dbg_plan_stale_ext);
#endif
		return true;
	}
	/*
	 * ☠ Compare raw, like for like.  @detach_child is the climb's own slot
	 * word (@elevated_old_child), so it must be compared against the slot
	 * word.  ft_node_get_nth_skip returns that: "the slot value as-is,
	 * including skip-compressed pointers".
	 *
	 * ft_node_get_nth resolves a skip-compressed word to the compressed
	 * node reached through its child's back-pointer.  That is a different
	 * address.  So comparing the two forms is unequal for every
	 * chain-shaped branch and equal for every plain one.  Measured: 347 vs
	 * 441 per run, a shape-determined constant, i.e. a permanent -EAGAIN.
	 * That is what wedged ft_unit at test 2 on the first attempt.
	 */
	if (intent->detach_child &&
			ft_node_get_nth_skip(iter_node_flag, NULL,
				intent->detach_byte, FT_PF_NONE)
					!= intent->detach_child) {
#ifdef FT_ENABLE_TRACING
		uatomic_inc(&ft_dbg_plan_stale_detach);
#endif
		return true;
	}
	return false;
}

struct ft_chain_compress_reclaim {
	/*
	 * Two lifetimes.  Mixing them frees a live node.
	 *  - The first three are the retired chain.  The caller's commit
	 *    unlinks them, so they are freed when it succeeds.
	 *  - @new_cn is the merged node this collapse built and recorded but
	 *    never published.  So it is freed when the caller's commit aborts.
	 * ft_detach_recompact_out draws the same split between @old_node and
	 * @new_flag.
	 */
	struct cds_ft_inode *boundary;		/* the 1-child boundary node */
	struct cds_ft_compressed_node *parent_cn;
	struct cds_ft_compressed_node *child_cn;
	struct cds_ft_compressed_node *new_cn;	/* unpublished: free on ABORT */
};

#ifdef FEATURE_FT_SKIP_COMPRESSED
/*
 * ft_chain_compress_fused: the fused-merge primitive behind the
 * chain-compress canonicalization.
 *
 * Build the merged compressed @new_cn INVISIBLY (rcu-mutation build phase)
 * from the surviving child the caller supplies.  Then commit the whole
 * transition in ONE flip:
 *  - the forward slot publish;
 *  - the surviving child's LIVE re-parent back-edge;
 *  - a compressed grandparent's SKIP_X dual;
 *  - the ordered-cell unsplice of @dead_cell / @run.
 *
 * The merge IS the build-invisible commit.  So the key-disappearing removal
 * that triggers it can publish through this single flip directly.  There is
 * no intermediate recompacted or in-place node, and never a transient
 * non-canonical state.  The merged @new_cn REPLACES the boundary node.  It
 * subsumes the boundary's child-slot clear (shape D) or its external_nodes
 * clear (shape P).
 *
 * @surviving_child / @surviving_byte: the boundary's sole post-removal child
 *   and its incoming byte.  The caller computes them from PRE-commit state,
 *   so this runs as the removal's commit, not as a second flip after it.
 *   Both are stable across the removal: the merge never rebuilds the parent
 *   or the surviving child.
 * @plan_nr_child: the boundary's child count that the caller's plan assumed.
 *   It is 2 for the shape-D fold that retires one of the two, and 1 for the
 *   post-removal collapses.  It is re-validated against the boundary's node
 *   lock snapshot, together with the @surviving_byte -> @surviving_child
 *   mapping.  So a peer commit between the caller's derivation and the fence
 *   mark surfaces as -EAGAIN, instead of a merge built from a stale plan.
 * @dead_cell / @run: the dead key's ordered-list unsplice, folded into the
 *   merge flip.  NULL when the ordered list is off or there is no fusion.
 * @orphans / @nr_orphans / @trailing_orphan: an optional orphan chain that
 *   the SAME unlink strands (ft_detach_node's branch-2 prune).  Each node's
 *   freeze-on-free tombstone is recorded INTO the merge flip, so it freezes
 *   atomically with the collapse.  The txn reservation grows to cover them.
 *   The standalone canonicalize and chain-leaf callers pass NULL/0/NULL:
 *   their flip retires no extra chain.
 *
 * Return:
 *  - 0 when the merge committed.
 *  - -ENOMEM when the txn or @new_cn could not be allocated.  NOTHING was
 *    published.  The caller aborts the whole removal; the structure is
 *    untouched.
 *  - -EAGAIN on a peer conflict: a dirty LOCK mark, a failed plan
 *    re-validation, a parked latch in a captured slot, or a commit ABORT.
 *    Nothing is installed and every fence is cleared.  The caller's retry
 *    re-descends.
 *  - A positive value when the merge does not apply, because the merged
 *    length would exceed the compressed-node bound.  The caller falls back
 *    to the non-fused removal, which leaves the boundary a 1-child internal
 *    as before.
 *
 * The replaced nodes (@boundary, @parent_cn, @child_cn) are enumerated
 * explicitly at the commit.  So a future MCAS can fold each one's sequence
 * counter into the same transaction.
 */
/*
 * Hand a chain-compress RETIRE member's acquire to the commit txn:
 *  - lift the lock off the ancestor that survives (a no-op where the
 *    member's own word carries it);
 *  - register the word, so every unwind path drains it.
 * A member that deduped onto a word the set already took owes neither.  The
 * acquire that first took the word owes both.  The tombstone itself is
 * recorded later, at the commit.
 */
static inline
void ft_chain_compress_register_retire(struct ft_flip_txn *txn,
		const struct ft_held_anchor *h, struct cds_ft_metadata *node)
{
	if (h->shared) {
		/*
		 * Its word was taken by an EARLIER member of this op.  So it
		 * owes no release and no terminal.  But the op DOES hold it,
		 * and the record-time ownership question is asked against
		 * @txn.  Say so (ft_flip_txn's @covered).
		 */
		ft_flip_txn_cover_member(txn, h->member);
		return;
	}
	ft_flip_txn_lock_register_held(txn, h);
	ft_flip_txn_record_anchor_release(txn, h, node);
}

/*
 * Test for the DEEP pending-publish shape.  The caller's shape-D gate and the
 * collapse itself both ask it here, so the two can never drift apart.
 *
 * @pending_child covers the shallow shape: "the BOUNDARY's own survivor slot
 * IS the caller txn's pending forward publish target".  This is the same
 * collision ONE COMPRESSED LEVEL DEEPER.  The survivor is a compressed node,
 * and its OWN child slot is that target.
 *
 * The collapse absorbs that node's run into the merged node.  So the merged
 * node's child is taken from @child_cn->child.  That is precisely the slot
 * the caller's commit is about to overwrite with its fresh cluster top.
 * Fusing around the COMMITTED occupant there does two wrong things:
 *  - it builds the merged run on a node that the same commit retires;
 *  - it publishes the moved cluster into a slot of the node this collapse
 *    replaces.
 * This strands the whole moved subtree.  Measured: rekey_merge over a
 * multi-level survivor chain loses both moved keys.  And the committed
 * occupant's re-parent collides with the glue's free-list tombstone on one
 * state word (MW here, SW there).
 *
 * The test is asked on the RESOLVED compressed pointer, on purpose, not on the
 * slot the boundary holds.  The boundary's flag may be the skip-encoded
 * spelling of the same node.  And the caller armed the node's own child WORD
 * ADDRESS.  A build without FEATURE_FT_COMPRESS has no compressed survivor,
 * so it never takes this arm.  That is why the ablated builds never showed
 * the loss.
 */
static inline
bool ft_chain_compress_deep_pending(struct cds_ft_inode_flag *survivor,
		const struct ft_flip_txn *txn)
{
	struct cds_ft_compressed_node *cn;

	if (!survivor || !txn || !txn->pending_pub_slot)
		return false;
	if (!ft_node_compressed(survivor))
		return false;
	cn = ft_compressed_node_ptr(survivor);
	return &cn->child == txn->pending_pub_slot;
}

/*
 * @iter_depth is the boundary's byte-depth.  @ctx is the op's lock context.
 * The whole collapsed chain is a lock-set, and its members anchor by depth.
 * The set straddles the boundary: parent_CN and pp lie ABOVE it, and the
 * surviving child lies ONE hop below.  That is exactly the shape §7.1
 * describes.
 */
/*
 * @shared_txn / @record_only (FOLD, for the rekey's one-decide writer): record
 * this collapse into the caller's txn instead of committing a second time.
 * Two commits cannot be one decide.  This collapse owning its own txn is
 * exactly what confined ft_detach_node's fold to the SIMPLE shape (BP above
 * min_child).  The rekey's `nr_child < 3` gate restated that limit.
 * @reclaim is REQUIRED when @record_only.
 *
 * @pending_child (FOLD, pending-publish): non-NULL ONLY on the record_only
 *   fold, when the boundary slot that @surviving_byte names is the caller
 *   txn's OWN pending forward publish target (@pending_pub_slot).  That slot
 *   is armed before this fold runs, in one of two ways:
 *    - by ft_glue_set_publish, on the GLUE and merge lanes;
 *    - directly by the NOSPLIT store commit in ft-graft.h.
 *   @pending_child is the FRESH, UNPUBLISHED cluster top that the same commit
 *   installs there.  The merged node must be built around it, not around the
 *   committed occupant.  Fusing around the committed value goes wrong twice:
 *    - it builds new_cn on a node that the same commit retires;
 *    - it lets the commit publish the cluster into a slot of the boundary
 *      this collapse replaces.
 *   This strands the whole moved subtree.  The sibling two-child-BP pin
 *   measures that shape.
 *
 *   The COMMITTED value stays in @surviving_child.  The plan re-validation and
 *   the lock-set are about what the trie holds NOW.  The committed occupant's
 *   retire belongs to the GLUE (its free list), never to this collapse.  That
 *   also keeps the two from colliding on one state word (the measured
 *   MW-PSO-vs-SW-tombstone chain conflict).
 *
 *   ☞ Except that @pending_child may BE the committed occupant.  A
 *   duplicate-chain splice publishes the dst's own app-owned head back into
 *   its slot.  The pending value is then live, not fresh.  Its back edge must
 *   be RECORDED, never stored (see @pending_top_live at the back-edge arm).
 *   The collapse then reports the fold (@pending_pub_folded /
 *   @pending_pub_node).  So the glue commit skips its forward publish and
 *   re-bases its count delta onto the merged node.
 *
 * ☑ A COMPRESSED @pending_child IS ABSORBED, not refused.  The caller used to
 * turn the whole move away here.  Its reason was that "absorbing a fresh
 * unpublished compressed top would retire a node that was never published and
 * re-aim its cluster's deferred edges".
 *  - The first half is the answer, not the obstacle.  A node that was NEVER
 *    PUBLISHED is not retired at all.  It is untracked from the glue and
 *    freed outright, with no grace period, because no reader ever had a path
 *    to it.
 *  - The second half holds for ONE edge.  When the pending top's child is the
 *    FRESH cluster node below it, its back-pointer is a plain store into a
 *    private body, and nothing is queued.  The child can instead be LIVE: the
 *    dst's own head, handed back by a duplicate-chain merge at the run's end.
 *    Its back edge is then a queued glue entry that names the pending top.
 *    The back-edge arm re-aims it at the merged node (ft_glue_reaim_edge).
 *
 * So the run simply grows by the pending top's bytes, and takes the pending
 * top's CHILD.  The bytes are @parent_cn ++ @surviving_byte ++
 * pending->key_bytes.  The DEEP fold already performs the same absorption one
 * level down.  It is what keeps the canonical form.  Both alternatives are
 * illegal:
 *  - fusing AROUND a compressed pending top puts two adjacent compressed
 *    nodes in the trie, which cds_ft_verify rejects;
 *  - falling back to ft_node_replace_ptr publishes a one-child internal,
 *    which skip mode does not allow to persist.
 * This one is the only legal product.
 *
 * The ONE thing still refused is a run that cannot be SPELLED.  An absorbing
 * merge whose @merged_len is past FT_SKIP_LEN_MAX returns -EDOM
 * (FT_REKEY_UNCOVERED), a terminal refusal with no fallback.  That is a
 * property of the trie, not of this writer.
 */
static
int ft_chain_compress_fused(struct cds_ft *ft,
		struct cds_ft_inode_flag *iter_node_flag,
		unsigned int iter_depth,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_metadata *iter_meta,
		struct cds_ft_inode_flag *surviving_child,
		uint8_t surviving_byte,
		unsigned int plan_nr_child,
		struct ft_ord_cell *dead_cell,
		struct ft_detach_run *run,
		struct cds_ft_inode_flag **orphans,
		int nr_orphans,
		struct cds_ft_inode_flag *trailing_orphan,
		struct ft_held_anchor *orphan_held,
		struct ft_held_anchor *trailing_orphan_held,
		struct cds_ft_node *freeze_leaf,
		unsigned int freeze_len,
		long count_delta,
		unsigned int count_reserve,
		struct ft_flip_txn *shared_txn,
		bool record_only,
		struct cds_ft_inode_flag *pending_child,
		const struct ft_chain_compress_intent *intent,
		struct ft_chain_compress_reclaim *reclaim)
{
	struct cds_ft_compressed_node *parent_cn, *child_cn;
	struct cds_ft_metadata *parent_cn_meta;
	unsigned int parent_len, child_len, merged_len;
	struct cds_ft_metadata *new_cn_meta;
	struct cds_ft_compressed_node *new_cn;
	struct cds_ft_inode_flag *new_cn_flag;
	struct cds_ft_inode_flag **publish_slot;
	/*
	 * The publish target is the CALLER's fresh, unpublished body (see the
	 * arm below).  The forward edge is then a plain interior store, not a
	 * txn record.  Neither the §4.B guard nor the dating applies to it.
	 */
	bool pub_home_private = false;
	struct cds_ft_inode_flag *publish_parent;
	unsigned int pub_depth = 0;
	/*
	 * The boundary's parent and ITS parent, dated once.  The acquire arm
	 * needs both as lock-set members.  The publish below needs whichever of
	 * them it republishes into.
	 */
	unsigned int parent_depth = 0, pp_depth = 0;
	struct cds_ft_inode_flag *iter_parent;
	struct ft_flip_txn *txn;
	/*
	 * The three RETIRE members of the collapsed chain.  Each names TWO words
	 * once a coarse spacing splits them:
	 *  - the word its acquire took.  It is released at registration, since
	 *    it is an ancestor that SURVIVES.
	 *  - the node's own word.  It is tombstoned at the commit.  Every plan
	 *    re-validation below must read this one.
	 */
	struct ft_held_anchor iter_held, pcn_held, ccn_held;
	/*
	 * The child the merged node is BUILT around.  It is the pending cluster
	 * top under the fold's substitution (see @pending_child at the header).
	 * Otherwise it is the committed survivor.  Every plan-facing read (the
	 * lock-set, the re-validation) keeps @surviving_child.
	 */
	struct cds_ft_inode_flag *build_child =
		pending_child ? pending_child : surviving_child;
	/*
	 * The DEEP spelling of the same substitution (see
	 * ft_chain_compress_deep_pending).  It is set once the lock-set arm has
	 * derived @child_cn and the plan re-validation has ratified the
	 * survivor.  From there it steers three things:
	 *  - the merged node's child value;
	 *  - the PLAIN back edge that value takes;
	 *  - the fold report to the glue.
	 */
	bool deep_fold = false;
	/*
	 * The pending top when it is a RUN.  Its bytes join the merged run, and
	 * its child becomes the merged node's child.  It is FRESH and
	 * UNPUBLISHED.  So it is untracked from the glue and freed outright
	 * below.  It is never retired, and never on the free list the commit
	 * drains.
	 */
	struct cds_ft_compressed_node *pending_cn = NULL;
	/* ...and the glue that BUILT it, which is the only owner that may free it. */
	struct ft_glue *pending_glue = NULL;
	/*
	 * The absorbed run's CHILD is LIVE (not built by @pending_glue).  Its
	 * back edge is the glue's queued entry, which names the run this fold
	 * frees.  See the back-edge arm.
	 */
	bool absorbed_child_live = false;

	assert(surviving_child);
	/* The substitution exists only on the fold; the caller gates both. */
	assert(!pending_child || record_only);
	/*
	 * A COMPRESSED pending top is ABSORBED into the merged run (see the
	 * header).  Recover its node from either spelling.  ft_glue_set_publish
	 * announces whatever the builder handed it:
	 *  - the PLAIN flag, for a fresh run whose child edge is still
	 *    deferred;
	 *  - the SKIP form, once the split re-encoded it.
	 */
	/*
	 * ☠ NEVER THE COMMITTED OCCUPANT.  This absorption FREES the pending
	 * top unpublished, with no grace period.  It assumes the top is fresh.
	 * A pending value that IS the slot's committed occupant is live and
	 * published (see @pending_top_live below).  Absorbing it would free a
	 * node that readers hold.  No armer hands back a live compressed top
	 * today: the one live-top armer requires an external head.  The
	 * identity test makes that a property of this code, not of the callers.
	 */
	if (pending_child && pending_child != surviving_child &&
			(ft_node_compressed(pending_child) ||
			ft_node_skip_compressed(pending_child))) {
		/*
		 * ☠ THE OWNER MUST BE IN HAND BEFORE THE ABSORPTION, not looked
		 * for afterwards.  Freeing the run without untracking it
		 * leaves it in @built.  ft_glue_abort then frees it a SECOND
		 * time on any later bail.
		 *
		 * The glue armed @pending_pub_slot in the first place
		 * (ft_glue_set_publish).  So it is always there for this shape.
		 * If it somehow is not, the absorption does not happen and the
		 * caller's refusal stands.  That is the only answer this frame
		 * can give without an owner.
		 */
		pending_glue = ft_glue_of_ctx(ctx);
		if (pending_glue)
			pending_cn = ft_node_skip_compressed(pending_child) ?
				ft_skip_to_compressed(ft, pending_child) :
				ft_compressed_node_ptr(pending_child);
	}
	/*
	 * Pre-reserve the commit flip-txn BEFORE any pre-flip side-effect.
	 *
	 * ft_record_child_back_edge below RECORDS the surviving child's
	 * (parent, parent-slot-offset) pair into @txn.  Both edges are
	 * co-committed (ft_reparent_record_meta).  So an aborted flip discards
	 * the pair coherently: no settled offset survives against the
	 * still-old parent.
	 *
	 * With the txn reserved, the publish commits through
	 * ft_ord_cell_flip_into without allocating.  So the only OOM points are
	 * this reservation and the new_cn allocation.  Both come BEFORE the
	 * build's first side-effect.  A concurrent-writer ABORT surfaces as
	 * -EAGAIN with new_cn reclaimed (see the commit site below).
	 *
	 * The txn is created FIRST, since its sizing needs only the caller's
	 * arguments.  So the LOCK fences below can register with it.  Every
	 * bail from here on is a ft_flip_txn_destroy or the commit itself.
	 * Both terminal paths drain the fence registry, so no unwind can leak
	 * a fence.
	 */
	if (record_only) {
		/*
		 * FOLD: the caller's txn.  The caller pre-reserved it for this
		 * collapse's edges too.  It is NEVER destroyed on a bail below:
		 * the caller owns it and may still commit the rest of its plan.
		 */
		txn = shared_txn;
	} else {
		txn = ft_flip_txn_create_bounded(ft, FT_REMOVE_COMMIT_REC_MAX_EDGES + 3
				+ FT_CELL_LOCKSET_MAX_RECORDS
				+ 1 /* §4.B parent guard */
				+ 1 /* back-edge (parent, offset) pair: the state-word edge */
				+ ft_freeze_reserve(ft, (unsigned int) nr_orphans
					+ (trailing_orphan ? 1 : 0))
				+ freeze_len * FT_HLIST_FREEZE_MAX_EDGES
				+ count_reserve /* nr_keys walk from publish_parent (R3 fold) */
				+ 1 /* @new_cn's born-lock release */);
	}
	if (!txn)
		return -ENOMEM;	/* nothing touched: caller aborts */
#ifdef FT_COLLAPSE_CLAIM
	/*
	 * §4 STEP B3's DRY RUN: the a3c75659 tool, aimed at the CHAIN-COMPRESS
	 * collapse.  Point B0's owner assert at this txn.  Every record the txn
	 * plants without owning is then named at an abort.  The build is
	 * otherwise byte-identical to the unarmed one.
	 *
	 * ☞ CLAIMED AT CREATION, EARLIER THAN THE ARM WOULD BE.  A dry run has to
	 * claim BEFORE the records it wants checked.  For this site, that is
	 * before the lock-set acquire below.  So first read a miss as "this
	 * record is planted before its owner is registered".  That ORDERING
	 * class has been the answer more often than a missing acquire.
	 *
	 * ☠ Next, read a miss as "the registry cannot SEE this hold", before
	 * "the op does not HOLD it".  Where a caller's SWEEP owns the mark's
	 * clearing, the absence is by design (ft_flip_txn_owns).
	 *
	 * ☠ @record_only hands us the CALLER's txn.  Claim it anyway.  The
	 * claim sets no record kind, so it is behaviour-neutral.  And the
	 * fold's records are exactly the ones this step has to judge.
	 */
	ft_flip_txn_claim_per_op_armable(ft, txn);
#endif
	FT_TP(chain_compress_enter, (const void *) iter_node_flag,
		(const void *) iter_meta,
		(const void *) CMM_LOAD_SHARED(iter_meta->external_nodes),
		(const void *) freeze_leaf, (const void *) dead_cell,
		plan_nr_child, (int) record_only);
	/*
	 * F2 node lock (doc at ft_meta_lock_acquire).  Fence the whole
	 * collapsed chain BEFORE trusting any of its mutable state.  The chain
	 * is the boundary and the two compressed nodes whose bodies the merged
	 * node subsumes.
	 *
	 * Each mark's clean snapshot feeds that node's tombstone expected-old
	 * at the commit.  So a peer state change under any fence aborts
	 * exactly one side.  A dirty mark bails to the caller's retry.  It can
	 * be a peer proxy, a concurrent copier, or a real retire.  For example,
	 * a peer already re-published the surviving child and tombstoned the
	 * old copy that this plan captured.
	 */
	if (ft->lock_fine) {
		/*
		 * Acquire the WHOLE chain-compress lock-set in ONE all-or-none
		 * MCAS: {B, parent_CN, child_CN, publish_parent}.  It replaces
		 * the three incremental retire-marks and the publish_parent
		 * lock_or_guard.
		 *  - B, parent_CN and child_CN are RETIRE members: their bodies
		 *    subsume into new_cn.
		 *  - publish_parent is the value-swap RELEASE target.
		 * The guards B.parent==iter_parent and
		 * parent_CN.parent==pp ride the same commit.  child_CN needs
		 * no guard.  The post-acquire
		 * surviving_byte->surviving_child re-validation covers its
		 * position.  And a peer split retires it, so ft_dlm_lock
		 * returns -EAGAIN.
		 */
		struct cds_ft_metadata *parent_cn_meta_l, *child_cn_meta_l = NULL;
		struct cds_ft_inode_flag *pp_flag = NULL;
		struct ft_dlm_member set[4];
		int nr_set = 0, si = 0, dret;

		iter_parent = (struct cds_ft_inode_flag *)
			ft_parent_node(rcu_dereference(iter_meta->parent_word));
		if (caa_unlikely(ft_node_flip_proxy(iter_parent))) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		parent_cn = ft_node_compressed(iter_parent)
			? ft_compressed_node_ptr(iter_parent) : NULL;
		parent_cn_meta_l = parent_cn
			? cds_ft_item_to_metadata((struct cds_ft_inode *) parent_cn)
			: NULL;
		/*
		 * Derived from the BUILD child: absorption is about the run the
		 * merged node will actually hold.  The committed occupant's fate
		 * (retire) belongs to the glue, which holds its own lock on it.
		 */
		/*
		 * ☠ NEVER THE PENDING TOP.  @child_cn is the LIVE run below the
		 * boundary.  Everything downstream treats it as one:
		 *  - a §7.1 below-pivot lock member;
		 *  - a retire registration;
		 *  - a @reclaim->child_cn entry, which the commit frees
		 *    through a grace period.
		 * The pending top is FRESH and UNPUBLISHED.  No reader can
		 * reach it.  So there is nothing to lock, nothing to retire,
		 * and no grace period to wait.  It is untracked from the glue
		 * and freed outright where the run absorbs it.  Routing it
		 * here produces the right STRUCTURE and the wrong OWNERSHIP.
		 */
		child_cn = (ft_node_compressed(build_child) &&
				(!pending_cn ||
					ft_compressed_node_ptr(build_child) != pending_cn))
			? ft_compressed_node_ptr(build_child) : NULL;
		if (child_cn)
			child_cn_meta_l = cds_ft_item_to_metadata(
				(struct cds_ft_inode *) child_cn);
		/* Publish grandparent = parent_CN's parent, else B's parent. */
		if (parent_cn_meta_l)
			(void) ft_resolve_parent_slot(parent_cn_meta_l, ft, &pp_flag);
		else
			pp_flag = iter_parent;

		/*
		 * Both members above the boundary were reached by back-pointer, so
		 * neither carries a depth.  Date each from the node BELOW it:
		 *  - the boundary is at @iter_depth;
		 *  - where @pp_flag is the compressed parent's own parent,
		 *    it is one hop above that.
		 * (Where there is no compressed parent, @pp_flag IS
		 * @iter_parent and shares its depth.)  If neither the window
		 * nor the hop can date a member, the plan is void.
		 */
		if (!ft_lock_ctx_depth_of_parent(ft, ctx, iter_parent, iter_depth,
					&parent_depth)) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		if (pp_flag && !(parent_cn_meta_l ?
				ft_lock_ctx_depth_of_parent(ft, ctx, pp_flag,
					parent_depth, &pp_depth) :
				ft_lock_ctx_depth_of(ft, ctx, pp_flag, &pp_depth))) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}

		set[nr_set++] = (struct ft_dlm_member){ .nf = iter_node_flag,
			.node = iter_meta, .depth = iter_depth,
			.guard_child = iter_meta, .guard_pf = iter_parent };
		if (parent_cn_meta_l)
			set[nr_set++] = (struct ft_dlm_member){ .nf = iter_parent,
				.node = parent_cn_meta_l, .depth = parent_depth,
				.guard_child = parent_cn_meta_l, .guard_pf = pp_flag };
		if (child_cn_meta_l)
			/*
			 * The one member BELOW the pivot (§7.1): an immediate child
			 * of the boundary, so it starts one internal-node hop past
			 * it.
			 */
			set[nr_set++] = (struct ft_dlm_member){ .nf = surviving_child,
				.node = child_cn_meta_l,
				.depth = iter_depth + 1 };
		if (pp_flag)
			set[nr_set++] = (struct ft_dlm_member){ .nf = pp_flag,
				.node = ft_flag_to_metadata(ft, pp_flag),
				.depth = pp_depth };

		dret = ft_dlm_acquire_set(ft, ctx, set, nr_set);
		if (dret) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return dret;	/* -EAGAIN / -ENOMEM; nothing acquired */
		}
		parent_cn_meta = parent_cn_meta_l;
		iter_held = set[si++].held;
		ft_chain_compress_register_retire(txn, &iter_held, iter_meta);
		if (parent_cn_meta_l) {
			pcn_held = set[si++].held;
			ft_chain_compress_register_retire(txn, &pcn_held,
				parent_cn_meta_l);
		}
		if (child_cn_meta_l) {
			ccn_held = set[si++].held;
			ft_chain_compress_register_retire(txn, &ccn_held,
				child_cn_meta_l);
		}
		if (pp_flag) {
			/*
			 * publish_parent is the value-swap RELEASE target
			 * (it survives).  Record its {LOCK|s -> s} release,
			 * and register it up front with the acquire.  Then:
			 *  - the publish below skips its lock_or_guard;
			 *  - every pre-publish bail's ft_flip_txn_destroy
			 *    drains it.
			 * So there is no separate publish_parent unwind path.
			 * A member that deduped onto a word the set already
			 * took owes neither.
			 */
			if (!set[si].held.shared) {
				ft_flip_txn_lock_register_held(txn, &set[si].held);
				ft_flip_txn_record_release_lock(txn,
					set[si].held.lock, set[si].held.lock_snap);
			} else {
				/* Held via an earlier member; owes no release. */
				ft_flip_txn_cover_member(txn,
					set[si].held.member);
			}
			si++;
		}
	} else
	{
		if (ft_acquire_member(ft, ctx, iter_node_flag, iter_meta,
				iter_depth, &iter_held)) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		ft_chain_compress_register_retire(txn, &iter_held, iter_meta);
		/*
		 * ONE snapshot of the boundary's parent (latch-checked).
		 * This is the F1 discipline: a peer's parked flip proxy must
		 * be neither classified nor embedded.
		 */
		iter_parent = (struct cds_ft_inode_flag *)
			ft_parent_node(rcu_dereference(iter_meta->parent_word));
		if (caa_unlikely(ft_node_flip_proxy(iter_parent))) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		parent_cn = ft_node_compressed(iter_parent)
			? ft_compressed_node_ptr(iter_parent)
			: NULL;
		parent_cn_meta = parent_cn
			? cds_ft_item_to_metadata((struct cds_ft_inode *) parent_cn)
			: NULL;
		if (parent_cn_meta) {
			if (!ft_lock_ctx_depth_of_parent(ft, ctx, iter_parent,
						iter_depth, &parent_depth) ||
					ft_acquire_member(ft, ctx, iter_parent,
						parent_cn_meta, parent_depth,
						&pcn_held)) {
				if (!record_only)
					ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
			ft_chain_compress_register_retire(txn, &pcn_held,
				parent_cn_meta);
		}
		/* BUILD child, as in the DLM arm above. */
		/* BUILD child, and never the pending top -- see the DLM arm. */
		child_cn = (ft_node_compressed(build_child) &&
				(!pending_cn ||
					ft_compressed_node_ptr(build_child) != pending_cn))
			? ft_compressed_node_ptr(build_child)
			: NULL;
		if (child_cn) {
			/* The §7.1 below-pivot member: one hop past the boundary. */
			if (ft_acquire_member(ft, ctx, surviving_child,
					cds_ft_item_to_metadata(
						(struct cds_ft_inode *) child_cn),
					iter_depth + 1, &ccn_held)) {
				if (!record_only)
					ft_flip_txn_destroy(txn);
				return -EAGAIN;
			}
			ft_chain_compress_register_retire(txn, &ccn_held,
				cds_ft_item_to_metadata(
					(struct cds_ft_inode *) child_cn));
		}
	}
	/*
	 * ★ THE PLAN, UNDER THE FENCE.  Everything the collapse rests on is read
	 * HERE, on the far side of the boundary's own mark.  It is compared
	 * against what the OP said it came to do (@intent).  It is never
	 * compared against a picture the caller took before the mark: a peer
	 * holding the same lock is entitled to invalidate that.  Four things
	 * are checked.  The last two are why keys were being lost:
	 *
	 *  1. the child COUNT (@plan_nr_child: 2 for the shape-D fold that
	 *     retires one of the two, 1 for the post-removal collapses).  This
	 *     check is free: nr_child lives inside the state word, so the
	 *     acquire's own snapshot carries it.
	 *  2. the SURVIVOR still at @surviving_byte.  ☞ This one cannot be
	 *     derived instead of compared.  When the survivor is compressed, it
	 *     is a member of the lock-set above, so it has to be named to be
	 *     locked.  A mismatch bails, which is equivalent.
	 *  3. the EXTERNAL HEAD.  ☠ Never checked before.  A peer's
	 *     ft_insert_park_external_nodes writes exactly this word, under
	 *     this same lock.  It moves neither nr_child nor any slot above.
	 *     So (1) and (2) both pass, and the collapse retires a boundary
	 *     that holds a live key.
	 *  4. the child THIS COMMIT DETACHES.  ☠ Never checked before.  A
	 *     peer's publish into that slot is a SAME-SLOT VALUE SWAP.  The
	 *     count does not move and the survivor does not move.  So the
	 *     collapse drops a freshly published subtree.  This was MEASURED as
	 *     the loss, in the act.
	 *
	 * Never fires single-writer.
	 */
	if (caa_unlikely(ft_state_nr_child(iter_held.node_snap) != plan_nr_child ||
			ft_node_get_nth(ft, iter_node_flag, NULL,
				surviving_byte, FT_PF_NONE)
					!= surviving_child ||
			ft_chain_compress_plan_stale(iter_node_flag, intent))) {
#ifdef FT_ENABLE_TRACING
		if (record_only)
			uatomic_inc(&ft_dbg_plan_stale_fold);
		else
			uatomic_inc(&ft_dbg_plan_stale_alone);
#endif
		if (!record_only)
			ft_flip_txn_destroy(txn);
		return -EAGAIN;
	}
	/*
	 * Ask the DEEP shape only now.  @child_cn is derived by whichever
	 * lock-set arm ran.  The re-validation just above makes
	 * @surviving_child (hence @build_child) the value the trie still holds.
	 *
	 * The two substitutions are mutually exclusive by construction.  One
	 * pending slot cannot be both the boundary's survivor slot and the
	 * survivor's own child slot.  The caller gates them as one if/else.
	 */
	deep_fold = record_only && child_cn &&
		ft_chain_compress_deep_pending(build_child, shared_txn);
	assert(!(deep_fold && pending_child));

	parent_len = parent_cn ? parent_cn->len : 0;
	child_len = child_cn ? child_cn->len : pending_cn ? pending_cn->len : 0;
	merged_len = parent_len + 1 + child_len;

	if (merged_len > FT_SKIP_LEN_MAX) {
		/*
		 * ☠ AN OVER-LONG ABSORPTION HAS NO FALLBACK TO FALL BACK TO.
		 * `return 1` means "merge does not apply, caller falls back".
		 * The caller's fallback is ft_node_replace_ptr.  The
		 * @pending_child contract above calls exactly that product
		 * ILLEGAL for this shape.  It publishes a one-child internal
		 * that skip mode does not allow to persist.  And the run it
		 * should have absorbed is still compressed beneath it.
		 *
		 * MEASURED on a release build with a 126-byte run.  The
		 * bound is exact: merged_len 127 is clean, 128 is red.  The
		 * op returns SUCCESS, and cds_ft_verify answers `compressed
		 * node ... node lock set at rest (leaked lock)`.
		 *
		 * The leaked lock is a PRE-EXISTING defect of that fallback.
		 * The same shape with the dst placed elsewhere is red at the
		 * parent commit too.  But REACHING it from here is new.
		 * Before the absorption existed, this shape took the shape-D
		 * gate's terminal refusal, before any side-effect.  A
		 * pre-existing root cause is not a non-regression.  For the
		 * caller, a clean FT_REKEY_UNCOVERED became SUCCESS on a
		 * corrupt trie.
		 *
		 * So the absorption refuses TERMINALLY instead.  That is what a
		 * run that cannot be spelled deserves: it is a property of the
		 * trie, not of this writer.  Only the absorbing shape takes this
		 * exit.
		 *
		 * ☠ AND THE DEEP FOLD TAKES THE SAME EXIT, for the same reason.
		 * It was left on the fallback because its live-top case was
		 * unreachable.  The through-a-run merge arm made it reachable:
		 * {ab, a+128*'c'}, dst the long key, src "ab".  The src
		 * junction collapses into the dst run, and the merged run
		 * cannot be spelled.  The fallback then published the
		 * one-child internal.  The caller's subsumption reported a
		 * fold that never happened.
		 * MEASURED: verify RED at 128 bytes, clean at 127.  With this
		 * line, the op is refused clean.
		 */
		if (pending_cn || deep_fold)
			return -EDOM;
#ifndef FT_DEBUG_OVERLONG_COLLAPSE_FALLBACK
		/*
		 * ☑ AN ORDINARY OVER-LONG MERGE IS SPELLED, NOT REFUSED.  Its
		 * fallback was never a legal product.  ft_node_replace_ptr
		 * leaves the boundary a one-child keyless internal.
		 * cds_ft_verify refuses that in skip mode, even single-threaded
		 * on the default build: {ab, a+128*'c'}, remove "ab" (127 bytes
		 * is clean).
		 *
		 * But a run longer than a skip pointer can encode is not
		 * unspellable.  cds_ft_insert builds exactly that shape for a
		 * long unique suffix: ONE compressed node, published through a
		 * PLAIN compressed flag.  ft_publish_compressed picks the
		 * encoding by length, and verify accepts it.  So build the same
		 * node here, and let the publish below take the plain flag.
		 * The only bound left is the node's own 8-bit length.  A key
		 * of FT_MAX_KEY_LEN bytes below a non-root boundary cannot
		 * reach it.
		 */
		if (merged_len > UINT8_MAX) {
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return 1;
		}
#else
		/* Merge does not apply: caller falls back (fences cleared). */
		if (!record_only)
			ft_flip_txn_destroy(txn);
		return 1;
#endif
	}
	new_cn = alloc_compressed_node(ft, merged_len, &new_cn_meta);
	if (!new_cn) {
		if (!record_only)
			ft_flip_txn_destroy(txn);	/* PREPARE state: no grace period */
		return -ENOMEM;	/* nothing published: caller aborts */
	}

	/* Compose merged path bytes. */
	if (parent_cn)
		memcpy(new_cn->key_bytes, parent_cn->key_bytes, parent_len);
	new_cn->key_bytes[parent_len] = surviving_byte;
	if (child_cn)
		memcpy(&new_cn->key_bytes[parent_len + 1],
			child_cn->key_bytes, child_len);
	else if (pending_cn)
		memcpy(&new_cn->key_bytes[parent_len + 1],
			pending_cn->key_bytes, child_len);
	new_cn->len = (uint8_t) merged_len;
	if (child_cn) {
		struct cds_ft_inode_flag *child_child =
			rcu_dereference(child_cn->child);

		/*
		 * A peer latch parked on the boundary cn's child must not be
		 * embedded in the merged cn.  The peer settles only the
		 * ORIGINAL slot.  An embedded copy dangles into its reclaimed
		 * descriptor (see the recompact copy-loop bail).  Nothing is
		 * recorded into @txn yet, and new_cn was never published.  So
		 * reclaim both, and let the caller retry after the peer
		 * settles.
		 */
		if (caa_unlikely(ft_node_flip_proxy(child_child))) {
			free_compressed_node_unpublished(ft, new_cn);
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		/*
		 * DEEP FOLD: @child_cn->child IS the caller's pending forward
		 * publish target.  So the merged node must carry the cluster
		 * top that commit installs there.  It must never carry
		 * @child_child, the occupant the same commit retires.  The
		 * proxy check above is kept unconditionally.  A peer parked on
		 * this word is contention, whichever value we are about to
		 * take.
		 */
		new_cn->child = deep_fold ? shared_txn->pending_pub_val
					  : child_child;
	} else if (pending_cn) {
		/*
		 * ABSORBED: the merged run carries the pending top's bytes.
		 * So the child it must hold is the pending top's OWN child.
		 * Most often that is the fresh cluster node below it, which
		 * no reader can reach yet.
		 *
		 * A fresh child needs no special back edge.  @pending_child
		 * non-NULL already routes the wiring to the PLAIN
		 * ft_set_parent arm below.  That is the right arm for a
		 * fresh, unpublished child.
		 *
		 * ☠ AND THE ABSORBED RUN LEAVES THE GLUE'S BUILT SET WITH IT.
		 * It was never published, so it is not retired and owes no
		 * grace period.  But the caller's ft_glue_abort frees
		 * everything in @built on a bail.  This node is about to stop
		 * existing.  So it must be untracked FIRST, or that abort
		 * double-frees it.  ft_glue_untrack's own header names this
		 * pair ("chain-merge absorbs a freshly-built compressed
		 * wrapper").
		 *
		 * The order matters: TAKE THE CHILD FIRST, then untrack, then
		 * free.  All three run before any later bail can run.  So no
		 * path sees the node in a half-owned state.
		 */
		new_cn->child = pending_cn->child;
		/*
		 * ☠ ...EXCEPT WHEN IT IS NOT FRESH.  A duplicate-chain merge
		 * at the run's end (a moved key equal to a dst key) hands
		 * back the dst's own APP-OWNED HEAD as the run's child.  That
		 * head is live, so ft_merge_build_run queues its back edge in
		 * the glue, aimed at the run.  Read it here, before the run
		 * is freed.
		 */
#ifndef FT_DEBUG_ABSORB_LIVE_CHILD_UNAIMED
		absorbed_child_live = !ft_glue_is_fresh(ft, pending_glue,
			new_cn->child);
#endif
		/*
		 * ☠ @built IS NOT THE ONLY NAME THE GLUE KEPT.
		 * ft_glue_set_publish stored the same flag in the glue's
		 * @top and in the txn's @pending_pub_val.  Untracking only
		 * the @built entry leaves both naming freed memory.
		 * Measured: on the PLAIN spelling, the glue reaches
		 * ft_glue_apply_deferred with @top dangling.
		 *
		 * It is LATENT today:
		 *  - @pending_pub_folded skips the forward-publish consumer;
		 *  - the other two consumers are reachable only from the
		 *    legacy KEY_SHORTER graft_swap.
		 * But the fix belongs here, beside the free.  It does not
		 * belong in a caller two files away that happens to skip the
		 * read.
		 */
		ft_glue_untrack(ft, pending_glue, pending_cn);
		if (pending_glue->top == pending_child)
			pending_glue->top = NULL;
		if (shared_txn && shared_txn->pending_pub_val == pending_child) {
			shared_txn->pending_pub_val = NULL;
			shared_txn->pending_pub_plain = NULL;
		}
		free_compressed_node_unpublished(ft, pending_cn);
	} else {
		new_cn->child = build_child;
	}
	ft_meta_nr_child_set(new_cn_meta, 1);
	/*
	 * Build the merged node with its POST-removal count: top + @count_delta
	 * (R3 fold).  When this merge retires a key, that is top - 1: the
	 * collapsed top node's count minus the disappearing key.  A
	 * @count_delta of 0 copies the count verbatim, as before.  That is a
	 * count-neutral canonicalize, or a leaf-detach whose -1 the caller
	 * still pre-decrements.  No-op when !rank_stats.
	 *
	 * ONE OWNER EACH under the @pending_child fold:
	 *  - this store owns ONLY the DETACH's side: the boundary's committed
	 *    count + @count_delta, i.e. minus the moved-out subtree;
	 *  - the GLUE's count arm owns the ATTACH's side.  At commit-record
	 *    time it adds g->count_delta to @pending_pub_node (this merged
	 *    node), and walks the shared stable parent
	 *    (ft_glue_txn_commit_edges' folded-publish arm).
	 * Baking the attach's delta here too would double-count it.
	 */
	ft_nr_keys_store(ft,new_cn_meta,
		ft_nr_keys_get(parent_cn_meta
			? parent_cn_meta
			: iter_meta) + count_delta,
		CMM_RELAXED);

	if (parent_cn) {
		/*
		 * Replace parent_cn at its own slot in the grandparent.
		 * Inherit the grandparent context from parent_cn as ONE
		 * consistent (parent, slot) snapshot (Phase 4.3 atomic
		 * re-home).  A peer that re-homes parent_cn commits its parent
		 * and its state-word offset atomically.  Two raw reads could
		 * tear across that commit.
		 */
		/*
		 * ☠ READ-YOUR-OWN-WRITES.  This is the whole defect when the
		 * collapse is FOLDED into a caller's commit.
		 *
		 * A same-trie rekey grafts first.  Its ADD-recompaction
		 * rebuilds the node that holds @parent_cn.  It RECORDS
		 * @parent_cn's re-home into the very txn this collapse now
		 * publishes through.  A recorded edge is not parked yet: it
		 * lives only in the descriptor.  So a raw derivation answers
		 * with the PRE-OP slot.
		 * That slot sits inside the copy THIS COMMIT RETIRES.
		 * ft_txn_parent_slot_at exists for exactly this, and its header
		 * says so.
		 *
		 * MEASURED, single-threaded, default build.  The two answers
		 * disagree: {old z, slot in old z} vs
		 * {fresh z, slot in fresh z}.  The collapse's publish landed
		 * in the retired copy.  The live trie kept BOTH the merged
		 * node's new home and the uncollapsed chain:
		 *   - one node under two parents;
		 *   - three tombstoned nodes still reachable;
		 *   - the merged node leaked.
		 * The op returned OK on it.
		 */
		publish_slot = ft_txn_parent_slot_at(parent_cn_meta, ft,
			txn ? txn->mtxn : NULL, &publish_parent);
		if (record_only && shared_txn) {
			struct cds_ft_inode_flag *raw_parent = NULL;

			(void) ft_txn_parent_slot_at(parent_cn_meta, ft, NULL,
				&raw_parent);
			/*
			 * ☠ THE HOME THIS COMMIT WILL LEAVE IS A NODE NOBODY
			 * HAS PUBLISHED YET, AND THIS COLLAPSE CANNOT PUBLISH
			 * INTO IT.  The two answers disagree only when the
			 * CALLER re-homed @parent_cn in this very txn.  That
			 * is a same-trie rekey whose graft recompacted the
			 * node that holds it.  So @publish_parent is the
			 * caller's FRESH, BUILD-INVISIBLE copy.  Its slot is
			 * not a word this op holds.  The forward edge's owner
			 * assert says so (measured:
			 * __ft_flip_txn_record_tag_ctx's ft_flip_txn_owns arm
			 * fires on the first attempt).
			 *
			 * A private home wants the PLAIN store that the SKIP_X
			 * dual arm makes for exactly this case
			 * (ft_dual_home_is_private).  The §4.B guard and the
			 * publish-parent dating are dropped with it.  Neither
			 * has anything to say about a node no reader can
			 * reach.  That publish is NOT WRITTEN YET.
			 *
			 * Until it is, REFUSE THE MOVE TERMINALLY.  Use the
			 * rekey's carve-out code, for its stated meaning:
			 * "this writer cannot express this shape".  Never
			 * return -EAGAIN, which would spin on a deterministic
			 * shape.
			 *
			 * ☠ THIS IS NOT THE "BEFORE ANY SIDE-EFFECT" REFUSAL
			 * THE TWO -EDOM ARMS IN ft_detach_node MAKE.  The
			 * difference is worth stating, rather than inheriting
			 * their wording.  By here, this collapse has ALREADY
			 * acquired its lock set.  It has recorded the releases
			 * into the caller's SHARED txn.  MEASURED at this
			 * return: 5 registered locks (every state word reading
			 * LOCK-set) and 8 records in the descriptor.
			 *
			 * The refusal is clean not because nothing happened.
			 * It is clean because nothing is READER-VISIBLE and
			 * nothing is ORPHANED:
			 *   - @new_cn is freed unpublished here;
			 *   - the caller's ft_rekey_graft_simple_attempt
			 *     detach_bail runs ft_flip_txn_destroy on the
			 *     shared txn;
			 *   - that drains the registry
			 *     (ft_flip_txn_lock_release_all, nr_locks=5);
			 *   - every one of those words is left live and
			 *     unlocked.
			 * That is the contract stated at
			 * ft_flip_txn_record_release_lock.  MEASURED end to
			 * end: 40,000 refused moves in one process.  RSS moves
			 * by 0 bytes.  The key count is unchanged.
			 * cds_ft_verify is clean after each.
			 *
			 * ☠ WITHOUT THIS THE OP RETURNS OK ON A CORRUPT TRIE.
			 * The raw derivation aims the publish at the copy the
			 * same commit RETIRES.  So the collapse lands in a
			 * dead node.  The live trie keeps both the
			 * uncollapsed chain AND the merged node's new home:
			 *   - one node sits under two parents;
			 *   - three tombstoned nodes are still reachable;
			 *   - the merged node is leaked;
			 *   - the next insert under that prefix never returns.
			 * MEASURED on 58 of 4328 enumerated shapes of this
			 * family.
			 */
			/*
			 * ☑ SERVED: PUBLISH INTO THE PRIVATE HOME WITH A PLAIN
			 * STORE.  The two answers disagree exactly when the
			 * CALLER re-homed @parent_cn in this very txn.  Then
			 * @publish_parent is its FRESH, BUILD-INVISIBLE copy.
			 * NO READER CAN REACH that node until the caller's one
			 * commit publishes it.
			 *
			 * The txn publishes REACHABILITY, NOT INTERIORS.  A
			 * word inside a private body is an interior write, so
			 * it wants the bare store.  The §4.B guard and the
			 * publish-parent DATING both fall away with it.
			 * Neither has anything to say about a node no reader
			 * can reach.  The dating would also refuse outright,
			 * because the descent never saw that node.
			 *
			 * ☞ THIS IS THE ANSWER THE SKIP_X DUAL ALREADY GIVES
			 * ONE ARM OVER.  Passing @rec = NULL is how it is
			 * said.  _ft_publish_to_parent_meta's forward edge and
			 * its dual BOTH fall to their `else` store.  That is
			 * the same dispatch ft_dual_home_is_private makes for
			 * the dual alone.  The refusal here was the last piece
			 * of that argument left unwritten.
			 *
			 * On ABORT the caller frees the fresh copy and the
			 * store dies with it.  A RECORDED edge could not say
			 * that.  It would aim a CAS at a word the op does not
			 * hold.  That is the owner assert this refusal was
			 * standing in front of.
			 */
			if (caa_unlikely(publish_parent != raw_parent)) {
				pub_home_private = true;
			}
			/*
			 * ☠ THE SAME DISAGREEMENT AS THE PROMOTE ARM'S, ONE ARM
			 * OVER.  The case is a caller's graft that SPLIT
			 * @parent_cn.  This collapse then absorbs a node that
			 * commit RETIRES.  It republishes it at @parent_cn's
			 * home, which is the word the glue's forward publish
			 * repoints.  Meanwhile the split has already re-homed
			 * @parent_cn's child under its fresh cluster.  Two
			 * steps, one word, incompatible plans.
			 *
			 * ☠ AND --enable-rcu-debug CANNOT SEE IT HERE.  MEASURED
			 * on a release probe build: this shape poisons the
			 * descriptor THREE times per attempt.  Every one is
			 * SW-against-SW:
			 *   - the kind assert never fires;
			 *   - @cn's state word is retired twice;
			 *   - a {live -> live} validate chains after a
			 *     tombstone.
			 * The livelock is the only detector.  That is why the
			 * refusal is what makes the shape reportable at all.
			 *
			 * Serving it needs the merged node re-based BELOW the
			 * split point.  That is a different product from the
			 * one this function builds.  So refuse terminally with
			 * the rekey's carve-out code, exactly as the arm above
			 * does, and for the same stated meaning.
			 */
			if (caa_unlikely(ft_glue_that_split(ctx, parent_cn) !=
					NULL)) {
				free_compressed_node_unpublished(ft, new_cn);
				return -EDOM;
			}
		}
		new_cn_meta->parent_word = ft_parent_word(ft, publish_parent);
	} else {
		/*
		 * Replace the boundary at its own slot in iter_parent.  Resolve
		 * that slot COHERENTLY from iter_meta (Phase 4.3 atomic re-home),
		 * exactly as the parent_cn arm resolves parent_cn_meta.  Never
		 * reuse a slot the caller precomputed with ft_get_parent_slot
		 * before this op's fence.
		 *
		 * Between that snapshot and here, a peer can recompact the
		 * boundary's parent (growing it to a larger node type) or
		 * re-home it.  The precomputed slot then points into the OLD,
		 * now-replaced parent body, while iter_meta->parent already
		 * names the NEW parent.  Pairing the stale slot with the fresh
		 * parent indexes the new parent's bitmap with an out-of-bounds
		 * rank (ft_slot_to_byte OOB).
		 *
		 * Require the resolved parent to still be the latch-checked
		 * iter_parent the plan above was validated against.  A re-home
		 * that changed it lands us on an unvalidated parent: reclaim
		 * the unpublished merged node and retry.
		 */
		/*
		 * ☐ UNAUDITED, and named here rather than left silent.  This
		 * arm still derives its slot RAW, and its latch check compares
		 * PARENTS.  So a same-txn re-home to the same parent at a
		 * changed OFFSET would pass it.  No shape reaching it through
		 * a folded rekey has been constructed.  A skeptic built six
		 * candidates (no run above the junction, a root-level
		 * junction).  Every one came back clean or refused-intact.
		 * But "not reproduced" is not "unreachable".  Same class as
		 * the parent_cn arm above.
		 */
		publish_slot = ft_resolve_parent_slot(iter_meta, ft,
			&publish_parent);
		if (caa_unlikely(publish_parent != iter_parent)) {
			free_compressed_node_unpublished(ft, new_cn);
			if (!record_only)
				ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		new_cn_meta->parent_word = ft_parent_word(ft, iter_parent);
	}
	ft_set_parent_slot(new_cn_meta,
			ft_parent_node(new_cn_meta->parent_word), publish_slot);

	/*
	 * The publish target is a lock-set member (the value-swap RELEASE
	 * half).  It was resolved through a back-pointer, so the descent's
	 * window is what dates it.  The check is here rather than at the
	 * acquire below: nothing is reader-visible yet, so a target this
	 * descent cannot date is a clean re-plan.  The target is the parent
	 * of whichever node this publish REPLACES: the compressed parent,
	 * else the boundary.  So the one-hop derivation dates it where the
	 * window cannot.  Under the DLM arm the same node was already
	 * anchored as @pp.
	 */
	if (!pub_home_private &&
			!ft_lock_ctx_depth_of_parent(ft, ctx, publish_parent,
			parent_cn ? parent_depth : iter_depth, &pub_depth)) {
		free_compressed_node_unpublished(ft, new_cn);
		if (!record_only)
			ft_flip_txn_destroy(txn);
		return -EAGAIN;
	}
	/*
	 * ★ VALIDATE THE FREEZE'S PLAN UNDER THE LOCK.  This is the fused
	 * detach's check (ft_detach_node), owed here since the chain parked SW.
	 *
	 * @freeze_len reached this collapse as cds_ft_remove's literal "1 =
	 * this key's SOLE entry", derived from an UNHELD read of @succ_node.
	 * A same-key insert can append a duplicate between that read and the
	 * acquire above.  A remover preempted there is all it takes.  The
	 * freeze below would then record {NULL -> MARK(NULL)} over the live
	 * @freeze_leaf->next.  Under MW the install CAS absorbed that stale
	 * plan.  Under SW the record parks and stores BLIND.  So the appended
	 * node is dropped with the key, UNMARKED: a lost insert.
	 *
	 * MEASURED under preemption (2 cpus, 2 competing spinners), per-node
	 * and root-only: 9 stale plans here of ~5.9M.  That is one per lost
	 * node.  0 with -DNO_FEATURE_FT_CHAIN_SW.  The comment that stood at
	 * the freeze said "MEASURED 0 of 279,029".  That was a fact about
	 * unpreempted load.
	 *
	 * @freeze_leaf's chain holder is HELD here, so the word can be asked:
	 *   - On the prefix arms (cds_ft_remove, cds_ft_remove_all) the leaf
	 *     heads the boundary's external chain.  The holder is the boundary
	 *     itself (@iter_held, acquired above).
	 *   - On ft_detach_node's shape-D call the holder is an orphan below
	 *     the boundary or the trailing skip target.  ft_detach_node locked
	 *     it in its orphan walk and holds it until its `end:` sweep.
	 *
	 * The refusal comes BEFORE any record or store.  Only the unpublished
	 * @new_cn exists, and this is the same unwind as the bail just above.
	 * It is retriable, and it terminates.  The caller re-derives and sees
	 * the successor.  It then takes the promote / unchain lane.
	 *
	 * @record_only is excluded for the txn's ownership: a caller-owned txn
	 * must not be destroyed here.  It is not excluded for exclusion: no
	 * record_only caller passes a @freeze_leaf today (the rekey fold passes
	 * NULL).
	 */
	if (freeze_leaf && !record_only &&
#ifdef FT_RED_NO_CCF_PLAN_CHECK
			/* RED CONTROL: the pre-fix code, for the audit's positive. */
			0 &&
#endif
			!ft_hlist_chain_plan_ok(ft, ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len)) {
		FT_HLIST_PLAN_BAIL();
		FT_DBG_RETRY_SITE();
		free_compressed_node_unpublished(ft, new_cn);
		ft_flip_txn_destroy(txn);
		return -EAGAIN;
	}

	new_cn_flag = ft_compressed_node_flag(new_cn);
	{
		struct ft_pub_rec rec = { .ctx = ctx, .n = 0 };
		struct cds_ft_inode_flag *new_cn_pub;
		/*
		 * Plan snapshot of publish_slot's old value.  The slot still
		 * holds the collapsed chain's reference (parent_cn's
		 * grandparent flag, or @iter_node_flag) resolved just above:
		 * nothing has been stored yet.  The commit CAS rejects a peer
		 * that raced it (see ft_pub_rec_add).
		 *
		 * This is a WAITING load, not a raw one.  @publish_slot is
		 * recorded as an edge of THIS txn a few lines down.  So it
		 * enters the txn's own write set, and the read-policy rule is
		 * to wait out an undecided parker.
		 *
		 * "The commit CAS rejects a peer that raced it" holds for a
		 * stale PLAIN value and not for a parked flip proxy.  A proxy
		 * is a descriptor-record POINTER.  Handing it to the engine
		 * as an expected-old trips urcu_txn_add's !urcu_txn_is_proxy
		 * check under --enable-rcu-debug.  A release build POISONS
		 * the descriptor and the retry loop absorbs it, so the arm is
		 * green and wrong.  Only a coarse spacing exposes it.  Under
		 * per-node this op holds @publish_parent's own word, so no
		 * peer can park here.
		 */
		struct cds_ft_inode_flag *pub_expected_old =
			ft_txn_load(txn->mtxn, (void **) publish_slot,
				FT_FLIP_PROXY_TAG);

		/*
		 * Chain-compress canonicalization publish: the merged compressed
		 * node replaces the collapsed chain at publish_slot.
		 *
		 * Fuse two things into ONE flip:
		 *   - the LIVE child re-parent (new_cn->child -> new_cn);
		 *   - the forward slot (+ a compressed grandparent's SKIP_X
		 *     dual).
		 * A reader then never sees new_cn->child re-parented onto
		 * new_cn while the grandparent slot still points at the
		 * collapsed chain (or vice versa).
		 *
		 * The back-pointer takes the PLAIN compressed flag (as the
		 * prior bare ft_set_parent did).  The forward slot takes the
		 * published (possibly skip-encoded) flag.  The back-edge is
		 * deferred, so the publish takes new_cn_meta explicitly.
		 * Otherwise ft_skip_to_compressed would read the
		 * not-yet-stored back-edge to resolve a SKIP_X forward flag.
		 *
		 * The commit rides the pre-reserved @txn
		 * (ft_ord_cell_flip_into).  So it is allocation-free past
		 * this point and cannot fail.
		 */
		/*
		 * ☠ THE PENDING TOP IS NOT ALWAYS FRESH.  Take a
		 * duplicate-chain SPLICE.  It has one key on each side of a
		 * same-parent rekey_merge: {ab, ac} + rekey_merge(dst "ac",
		 * src "ab").  It hands ft_glue_set_publish the dst's own
		 * APP-OWNED HEAD.  That head is the COMMITTED OCCUPANT of the
		 * slot: ft_merge_build appended the src leaf to its dup chain
		 * and handed it back.  The fold's substitution is then BY
		 * IDENTITY, and the node this arm wires is LIVE.  It is
		 * reader-reachable now, and every ordered up-walk reads its
		 * parent word.
		 *
		 * Two things go wrong with the plain store on it:
		 *   - It is a reader-visible mutation BEFORE the commit, and
		 *     no abort rolls it back.
		 *   - The glue could not take its fresh-child fast path for
		 *     a live top.  So it QUEUED the same edge, aimed at the
		 *     boundary this collapse retires.  It would re-apply the
		 *     dead parent over this store at the flip.
		 * MEASURED in all four rank/list modes:
		 *   - `cn->len 16`, decoded off the freed boundary through
		 *     the head's back-pointer;
		 *   - `head prev != owner`.
		 *
		 * A live pending top takes the RECORD arm below like every other
		 * live child.  ft_glue_apply_deferred drops the glue's queued
		 * entry for it on the strength of @pending_pub_folded.  The
		 * identity test is exact: a committed occupant is reader-reachable
		 * and a fresh top is not, so nothing fresh can compare equal.
		 *
		 * ☞ ONE PRE-COMMIT STORE REMAINS, AND IT IS A NO-OP HERE.  The
		 * record arm's external branch stamps the head's incoming byte
		 * (ft_head_stamp_incoming_byte) before it records.  For a
		 * bare head that store is reader-visible ahead of the
		 * publish, and an abort does not undo it.  Its header says
		 * so.  On this shape the head does not move.  It hangs off
		 * the same byte of the fresh run as it did off the boundary,
		 * so the stamped value is the value already there.  That
		 * stops being true the day this arm serves a head that
		 * changes slot.  It does not today.
		 */
		bool pending_top_live =
			(pending_child && pending_child == surviving_child) ||
			(deep_fold && child_cn &&
				new_cn->child == child_cn->child);

		if (absorbed_child_live && ft_glue_reaim_edge(pending_glue,
				new_cn->child, new_cn_flag, &new_cn->child)) {
			/*
			 * ABSORBED RUN, LIVE CHILD: ONE OWNER, THE GLUE.  The run
			 * this fold freed is still the PARENT of its child's
			 * queued glue entry.  ft_glue_txn_commit_edges records
			 * that entry into this same txn after the fold.  Left
			 * alone, it overwrote this arm's plain store and
			 * aimed a live head's parent word at freed memory.
			 * Seen on the default build, single-threaded, with
			 * {cacb, baca, ccb} + rekey_merge(dst "cc", src "cac"):
			 *   - cds_ft_verify "skip-encoded slot slen 2 !=
			 *     cn->len 1" (the length read off the freed run);
			 *   - both copies of the key lost to lookup.
			 * The plain store was wrong on its own too.  The head
			 * is reader-reachable until the commit, and no abort
			 * would have rolled the store back.  Re-aimed at
			 * @new_cn, the entry records the edge atomically with
			 * this publish, like every other live dst-origin
			 * child.  (Live with no queued entry takes the RECORD
			 * arm below.)
			 */
		} else if ((pending_child || deep_fold) && !pending_top_live &&
				!absorbed_child_live) {
			/*
			 * FOLD substitution: the child is the FRESH, UNPUBLISHED
			 * cluster top.  It is build-invisible until the
			 * caller's one commit, so its back edge is a PLAIN
			 * STORE.  That is exactly the store
			 * ft_glue_defer_edge's fresh-child fast path made when
			 * it aimed it at the boundary this collapse replaces.
			 * That fast path stores and does NOT queue, so nothing
			 * re-applies the old parent over this.  A RECORDED
			 * re-parent here would plant an MW parent+PSO pair on
			 * a node the glue's free-list retire also settles.
			 * That is the measured record_chain collision that
			 * poisons (release) or asserts (rcu-debug).
			 */
			ft_set_parent(ft, new_cn->child, new_cn_flag,
				&new_cn->child);
		} else {
			/*
			 * @ctx is the op's held set.  Handing it over keeps
			 * this site from recording a §4.B MW validate on a
			 * word the op already holds SW.  See the call in
			 * ft_record_child_back_edge for the three records that
			 * collide without it.
			 */
			ft_record_child_back_edge(ft, txn, new_cn->child,
				new_cn_flag, &new_cn->child, ctx);
			/*
			 * ...so @new_cn, the fresh parent that edge now names, is
			 * published LOCKED (ft_flip_txn_lock_born_spine).  The
			 * edge settles after the decide.  It starts where the
			 * node it replaces started.  This is not done on the
			 * FOLD (@record_only).  That is the rekey's bulk
			 * writer, whose FT-wide lock keeps every point writer
			 * out of the commit.
			 */
			if (!record_only && ft->lock_fine && !ft->exclusive) {
				struct ft_born_spine_node sp = {
					.meta = new_cn_meta,
					.start = parent_cn ? parent_depth :
						iter_depth,
					.span = merged_len,
				};

				ft_flip_txn_lock_born_spine(ft, txn, &sp, 1);
			}
		}
		new_cn_pub = ft_publish_compressed(ft, new_cn, new_cn_flag);
		/* VALIDATE (§4.B): lock (or guard-fallback) the LIVE
		 * (great-)grandparent publish_parent.  It is the value-swap
		 * target (§10.5).  DLM: under lock_fine the whole lock-set
		 * (incl. publish_parent's RELEASE) was acquired up front.  So
		 * skip the incremental lock here. */
		if (!ft->lock_fine && !pub_home_private)
			ft_flip_txn_lock_or_guard_parent(ft, txn, ctx,
				publish_parent, pub_depth);
		_ft_publish_to_parent_meta(ft, publish_parent, publish_slot,
			new_cn_pub, pub_expected_old, new_cn_meta, NULL,
			pub_home_private ? NULL : &rec,
			/*slot_owner_nf=*/ publish_parent, false);
		/*
		 * Freeze-on-free (doc §4.B, atomic detach).  This commit
		 * retires the collapsed chain: the 1-child boundary
		 * @iter_node_flag and the old parent/child compressed nodes
		 * new_cn merges (<=3).  Each gets its one-way LIVE->DEAD
		 * tombstone RECORDED INTO @txn (the +3 reserved above).  So
		 * the freeze flips ATOMICALLY with the commit that unlinks
		 * the chain: an aborted commit leaves every node live.  A
		 * no-op under one writer.
		 *
		 * All three are FENCED (marked LOCK above), so each expected
		 * old is its mark's clean snapshot.  The commit ratifies
		 * exactly the chain state this merge was built from.  The
		 * fence is consumed in one of two ways:
		 *   - by the fused {LOCK|s -> TOMBSTONE|s}, where the member
		 *     carries its own lock;
		 *   - by the release recorded at registration, where coarsening
		 *     left the lock on a surviving ancestor.
		 */
		ft_flip_txn_record_retire_anchored(txn, ctx, &iter_held,
			iter_meta);
		if (parent_cn)
			ft_flip_txn_record_retire_anchored(txn, ctx, &pcn_held,
				parent_cn_meta);
		if (child_cn)
			ft_flip_txn_record_retire_anchored(txn, ctx, &ccn_held,
				cds_ft_item_to_metadata(
					(struct cds_ft_inode *) child_cn));
		/*
		 * @orphan_held is only ever ft_detach_node's own array, and
		 * under @record_only @txn is its caller's: the array's frame
		 * returns before this txn's terminal.
		 */
		ft_detach_freeze_orphans(ft, txn, ctx, orphans, nr_orphans,
			trailing_orphan, orphan_held, trailing_orphan_held,
			record_only);
		/*
		 * PHASE B, STEP B3 -- THE ARM.  ft_detach_freeze_orphans above
		 * holds this op's LAST ft_flip_txn_lock_register.  That
		 * register is an orphan chain anchor, where the anchor is not
		 * the retired node itself.  Under lock_fine everything else
		 * was taken in the ONE all-or-none acquire far above.  The
		 * incremental lock_or_guard beside the publish is the
		 * !lock_fine arm, which this helper refuses anyway.  So this
		 * is where ft_flip_txn_arm_per_op's "after the op's last
		 * register" contract puts it.  ft_remove_commit_rec below
		 * plants the first record after it.
		 *
		 * ☞ READINESS: the dry run (-DFT_COLLAPSE_CLAIM) claims at txn
		 * CREATION, which is strictly earlier.  It is clean on both
		 * ft_unit and ft_inv FT_INV_MW=1.  So every record on this txn
		 * already passes the owner check.
		 *
		 * ☞ WHAT IT CONVERTS is the collapse's forward publish.  Its
		 * owner is @publish_parent, a RELEASE member of the same
		 * all-or-none lock-set.  The retires, the orphan freezes and
		 * the child back-edge are planted ABOVE it and stay MW.  MW is
		 * stricter and always sound.  Converting those would mean
		 * arming before this op's own bookkeeping registers.  The
		 * contract does not allow that from here.
		 *
		 * ☠ NOT on the FOLD path (@record_only): @txn is the CALLER's.
		 * The completeness of its registry is the caller's judgement,
		 * not this frame's.
		 */
		if (!record_only)
			ft_flip_txn_arm_per_op(ft, txn);
		/*
		 * The removed external leaf is a single-entry chain, so
		 * freeze_leaf->next == NULL.  It freezes atomically with this
		 * same commit that retires its holder chain (doc §4.B): one
		 * MARK(NULL) edge, the +1 reserved above.  @freeze_leaf is NULL
		 * when the caller is not retiring a leaf through this merge.
		 */
		if (freeze_leaf) {
			ft_ch_audit_ctx(ft, txn, ctx, freeze_leaf);
			/*
			 * The plan was validated under the holder before the
			 * publish (see the bail above @new_cn_flag), which is
			 * what licenses the _checked form.  The observe stays,
			 * so a stale plan past that check still gets counted.
			 */
			FT_HLIST_PLAN_OBSERVE(ft, ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len, 3);
#ifdef FT_DEBUG_FREEZE_STALE_ABORT
			/*
			 * PROBE: the plan was checked under the lock above and
			 * is stale NOW.  So a peer changed the chain while this
			 * op holds what it believes is the holder.  Name the word
			 * we hold and the chain word's raw value (a parked proxy
			 * names the peer's descriptor).  Then end the flight
			 * recorder here and die.  The peer's lock take and
			 * chain store are the last events on its cpu.
			 */
			if (!record_only &&
					!ft_hlist_chain_plan_ok(ft,
						ft_flip_txn_handle(txn),
						freeze_leaf, freeze_len)) {
				void *raw = CMM_LOAD_SHARED(freeze_leaf->next);

				FT_TP(lock_stolen, (const void *) freeze_leaf,
					0u, (const void *) raw, 0u,
					(const void *) iter_held.lock);
				fprintf(stderr, "FT FREEZE STALE AFTER CHECK: leaf %p next raw %p (%s) len %u; this op holds %p (boundary %p meta %p)\n",
					(void *) freeze_leaf, raw,
					ft_txn_is_proxy(raw, FT_HLIST_TAG) ?
						"PARKED PROXY" : "plain",
					freeze_len, (void *) iter_held.lock,
					(void *) iter_node_flag,
					(void *) iter_meta);
				ft_trace_capture();
				abort();
			}
#endif
			ft_hlist_freeze_chain_prepare_checked(ft,
				ft_flip_txn_handle(txn),
				freeze_leaf, freeze_len);
		}
		/*
		 * R3 fold: the retired key's -1 walk goes from the merged
		 * node's stable parent (publish_parent) up to root.  It rides
		 * THIS commit atomically with the collapse (the merged node
		 * itself was built -1 above).  It is reserved by
		 * @count_reserve.  It is skipped (and a no-op) when there is
		 * no count change.
		 */
		if (count_delta)
			ft_flip_txn_record_count_parent(ft, txn, publish_parent,
				count_delta);
		/*
		 * The predicate's word, re-read at the LAST instant before the
		 * collapse commits.  A pair (chain_compress_enter.ext,
		 * chain_compress_exit.ext_now) that DIFFERS is the stale
		 * predicate caught in the act.  One event pair shows it,
		 * without having to reconstruct it from edge_records.
		 */
		FT_TP(chain_compress_exit, (const void *) iter_node_flag,
			(const void *) iter_meta,
			(const void *) CMM_LOAD_SHARED(iter_meta->external_nodes),
			-1);
		if (ft_remove_commit_rec(ft, &rec, dead_cell, run, txn,
				record_only) != URCU_TXN_STATUS_OK) {
			/*
			 * Peer won: NOTHING installed.  The collapsed chain
			 * (boundary + parent_cn/child_cn) is still live and
			 * linked.  Its tombstones, freezes, and count edges
			 * were discarded with the aborted commit.  Only the
			 * never-published merged node is ours to reclaim.  The
			 * caller retries and re-plans against the current tree.
			 */
			free_compressed_node_unpublished(ft, new_cn);
			return -EAGAIN;
		}
	}

	if (record_only) {
		/*
		 * ☠ NOT FREED HERE.  Under the fold nothing has committed
		 * yet, so this chain is still LIVE and LINKED.  These frees
		 * would reclaim nodes the caller's pending commit has not
		 * unlinked, and a reader is entitled to be walking them.
		 * Hand them back instead: the free path is the CALLER's
		 * choice (@913cf0ca).
		 */
		reclaim->boundary = ft_node_ptr(iter_node_flag);
		reclaim->parent_cn = parent_cn;
		reclaim->child_cn = child_cn;
		/*
		 * The merged node is RECORDED, not published.  The caller's
		 * commit is what publishes it, so an abort leaves it ours to
		 * reclaim.  (The non-folded path cannot reach here with it
		 * unpublished: every bail above frees it and returns.)
		 */
		reclaim->new_cn = new_cn;
		/*
		 * REPORT THE FOLD, at the tail where nothing can bail any
		 * more.  The caller's pending forward publish is now live in
		 * the merged node.  So the glue commit must do two things:
		 *   - SKIP its own publish, which would install into the
		 *     boundary this collapse just recorded retired;
		 *   - re-base its order-statistics delta onto the merged
		 *     node (@pending_pub_node; the count arm decodes it
		 *     compressed-aware).
		 * The flags live on the caller's txn and die with it.  Every
		 * abort path destroys the txn before (or with) reclaiming
		 * @new_cn.  Each retry creates a fresh txn with these fields
		 * cleared.
		 */
		if (pending_child || deep_fold) {
			txn->pending_pub_folded = true;
			txn->pending_pub_node = new_cn_flag;
		}
		return 0;
	}
	FT_TP(chain_compress_exit, (const void *) iter_node_flag,
		(const void *) iter_meta,
		(const void *) CMM_LOAD_SHARED(iter_meta->external_nodes), 0);
	free_cds_ft_node(ft, ft_node_ptr(iter_node_flag));
	if (parent_cn)
		free_compressed_node(ft, parent_cn);
	if (child_cn)
		free_compressed_node(ft, child_cn);
	return 0;
}

/*
 * Run the post-removal chain-compress prune as a standalone second flip.
 * Compute the surviving child from the (already-committed) 1-child boundary
 * and fuse no ordered-cell edge.
 *
 * The fused-merge primitive above is the preferred path: the prune rides
 * the removal's own flip.  This thin wrapper remains for the out-of-bound
 * (>FT_SKIP_LEN_MAX) residue case, where the boundary stays a 1-child
 * internal.  An allocation failure is silently skipped: the prune is
 * best-effort once the removal has already published.
 */
static
void ft_canonicalize_chain_compress(struct cds_ft *ft,
		struct cds_ft_inode_flag *iter_node_flag,
		unsigned int iter_depth,
		const struct ft_lock_ctx *ctx,
		struct cds_ft_metadata *iter_meta)
{
	uint8_t surviving_byte = 0;
	struct cds_ft_inode_flag *surviving_child;

	surviving_child = ft_node_get_minmax(ft, iter_node_flag,
		&surviving_byte, FT_LEFTMOST,
		false /* writer; no validation */);
	if (!surviving_child)
		return;
	/*
	 * INTENT: this is a pure canonicalize.  It removes nothing.  The
	 * caller's gate (ft-remove.h, the post-detach fold) has already
	 * asserted the boundary carries NO external head.  So that is what
	 * the callee must still find under the mark, and there is no body
	 * child to detach.
	 */
	{
		const struct ft_chain_compress_intent intent = {
			.detach_child = NULL,
			.expect_ext = NULL,
			.detach_byte = 0,
		};

		(void) ft_chain_compress_fused(ft, iter_node_flag, iter_depth, ctx,
			iter_meta,
			surviving_child, surviving_byte,
			1 /* already-committed 1-child boundary */, NULL, NULL,
			NULL, 0, NULL, NULL, 0 /* no orphan chain */,
			NULL, 0 /* no freeze_leaf */,
			0 /* count-neutral canonicalize */, 0,
			NULL, false, NULL /* no pending publish */,
			&intent, NULL);
	}
}
#endif

/*
 * FOLD (coherent rekey one-decide writer): the record_only detach's
 * src-junction recompaction produces two copies.  The CALLER must reclaim
 * them after its own commit:
 *   - the OLD copy, retired by the flip and freed on commit OK;
 *   - the fresh NEW copy, republished by the flip and freed UNPUBLISHED if
 *     the caller's commit ABORTS.
 * ft_detach_node surfaces both here.  Zeroed means no recompaction happened,
 * or not record_only.  A NULL @recompact_out means the caller does not run
 * a record_only detach.  An ELEVATING detach adds a third group with the
 * OLD copy's lifetime: the orphan chain the upward walk cleared.
 */
struct ft_detach_recompact_out {
	struct cds_ft_inode *old_node;		/* retired copy: free on commit OK */
	struct cds_ft_inode_flag *new_flag;	/* fresh copy: free unpublished on abort */
	/*
	 * A FOLDED COLLAPSE's nodes, when the detach's boundary parent fell to
	 * one child and the collapse was recorded into the caller's txn rather
	 * than committed.  Same two lifetimes as the pair above; @boundary is
	 * NULL when no collapse happened.
	 */
	struct ft_chain_compress_reclaim collapse;
	/*
	 * An ELEVATING detach's orphan chain:
	 *   - the ancestors the upward walk cleared;
	 *   - the chain below the detach target;
	 *   - the trailing skip-compressed target, if there is one.
	 * Same lifetime as @old_node.  The caller's commit is what UNLINKS
	 * them.  So they are freed when it SUCCEEDS, and left alone when it
	 * aborts.  On an abort nothing unlinked them, and a reader is still
	 * entitled to be walking the chain.  Their freeze-on-free tombstones were
	 * recorded into the caller's shared txn, so they flip with that
	 * same unlink.
	 */
	struct cds_ft_inode_flag *orphans[FT_MAX_DEPTH];
	int nr_orphans;
	struct cds_ft_inode_flag *orphan_trailing;
};

#ifdef FT_DEBUG_STACK_SILENCER
static unsigned long ft_stack_silencer_hits, ft_stack_silencer_scans,
	ft_stack_silencer_locks;
static __attribute__((destructor)) void ft_stack_silencer_report(void)
{
	fprintf(stderr, "FT STACK SILENCER: %lu deferred returns scanned, %lu registered locks examined, %lu still pointing into a dead frame\n",
		uatomic_read(&ft_stack_silencer_scans),
		uatomic_read(&ft_stack_silencer_locks),
		uatomic_read(&ft_stack_silencer_hits));
}
#endif

static
int ft_detach_node(struct cds_ft *ft,
		const struct ft_lock_ctx *op_ctx,
		struct cds_ft_inode_flag **detach_node_flag_ptr,
		struct cds_ft_inode_flag **detach_parent_flag_ptr,
		unsigned int detach_depth,
		bool free_detached_subtree,
		struct ft_ord_cell *fuse_cell,
		struct ft_remove_pub *pub,
		struct ft_detach_run *run,
		struct ft_glue *retire_glue,
		struct cds_ft_node *freeze_leaf,
		unsigned int freeze_len,
		long count_delta,
		struct ft_flip_txn *shared_txn,
		bool record_only,
		const struct ft_parent_hint *src_held_hint,
		struct ft_detach_recompact_out *recompact_out,
		bool in_place)
{
	struct cds_ft_metadata *metadata_stack[FT_MAX_DEPTH];
	struct cds_ft_inode_flag *iter_node_flag;
	struct cds_ft_inode *old_recompacted_node = NULL;
	int ret, nr_metadata = 0, nr_clear = 0, nr_branch = 0;
	uint8_t n = 0;
	/*
	 * THE ELEVATED CHAIN: what the climb prunes between the boundary and
	 * the original detach target.  @nr_clear counts its KEYLESS
	 * single-child links.  @climb_promoted says whether the climb also
	 * emptied a KEYED single-child junction and lifted its head into the
	 * boundary's slot.  That junction is unlinked by the very same
	 * publish, so it is an orphan too.  @nr_elevated is the chain's full
	 * length: the budget both orphan walks below are bounded on.
	 */
	bool climb_promoted = false;
	int nr_elevated;
	/*
	 * Pre-reserved commit flip-txn for the plain-branch key-removal
	 * commit (in-place delete OR recompaction publish).  It is reserved
	 * when that commit will be multi-edge: a cell unsplice, or a
	 * compressed-parent SKIP_X dual.  The reservation comes before
	 * ft_node_replace_ptr's pre-flip side-effects (nr_child-- / eager
	 * child re-parent).  A lone edge
	 * stays the infallible on-stack release store.  The txn is consumed
	 * by ft_ord_cell_flip_into at whichever commit fires.  It is freed
	 * at @end if reserved but unused.
	 */
	struct ft_flip_txn *commit_txn = NULL;
	bool commit_txn_used = false;
	/*
	 * Branch-2 (non-compressed parent) orphan chain.  It is freed AFTER
	 * the commit that unlinks it: below the recompaction republish, not
	 * in the pre-republish !ret block.  A recompaction defers its
	 * forward commit (@commit_txn) to the parent-slot block far below.
	 * Deferring the free to there has two effects:
	 *   - every freed node's freeze-on-free tombstone (recorded INTO
	 *     @commit_txn) flips ATOMICALLY with that unlink;
	 *   - the free happens only after the structural unlink, never
	 *     before it.
	 * The set is copied out of the branch-2 to_free[] on a successful
	 * detach.  @free_orphans_pending gates the deferred walk.
	 */
	struct cds_ft_inode_flag *orphan_free[FT_MAX_DEPTH];
	int nr_orphan_free = 0;
	struct cds_ft_inode_flag *orphan_trailing = NULL;
	bool free_orphans_pending = false;
	/*
	 * THE BOUNDARY THE CLIMB ACTUALLY LANDED ON, and its slot.  A caller's
	 * @src_held_hint describes the DETACH TARGET's junction.  That stops
	 * being the recompacted node the moment the climb ELEVATES.  It then
	 * recompacts an ANCESTOR, whose parent is one level higher again.
	 * Forwarding the caller's hint there points the republish at the
	 * wrong node, and the read-set guard that validates it too.  These
	 * name the real pair, re-derived by the climb that moved.
	 */
	struct cds_ft_inode_flag *boundary_parent_nf = NULL;
	bool climbed = false;
	struct ft_parent_hint elevated_hint;
	const struct ft_parent_hint *replace_hint = src_held_hint;
	/*
	 * DLM orphan plan-lock (§9.2).  Under ft->lock_fine, every collected
	 * orphan is lock-acquired at collection.  (Orphans come from Block A
	 * or Block B; the two are mutually exclusive.)  So the nr_child==1
	 * collapse decision is made on a FROZEN state word.  Its clean
	 * snapshot feeds the FENCED {LOCK|s -> TOMBSTONE|s} tombstone at
	 * freeze.  A peer that grows the orphan tears the expected-old and
	 * aborts our commit.
	 *
	 * The marks live in these fn-scope arrays, not in the txn locks[]
	 * registry.  The chain reaches FT_MAX_DEPTH, past
	 * FT_FLIP_TXN_MAX_LOCKS.  So the op OWNS their clearing.  One
	 * unconditional ft_meta_lock_release_if_held sweep at @end does it:
	 *   - it releases every held mark;
	 *   - it no-ops on the marks a successful commit already consumed
	 *     (TOMBSTONE).
	 * So no per-commit "consumed" bookkeeping is needed.
	 *
	 * Sized for the chain PLUS the trailing skip-target, which is one
	 * more orphan and belongs in the same array.  A mark kept in a
	 * variable of its own is a mark no later acquire can see.  A coarse
	 * spacing then lands the next member's anchor on it, and the op
	 * waits on itself.  A self-refusal is the one -EAGAIN no peer will
	 * ever clear.  So under remove_all's retry loop it does not surface
	 * as an error at all: it SPINS.
	 */
	struct ft_held_anchor orphan_held[FT_MAX_DEPTH + 1];
	int nr_orphan_locked = 0;
	/*
	 * Did §9.3's third member -- the SKIP_X dual's derived grandparent --
	 * actually get ACQUIRED at publish?  The republish below owes that
	 * answer to ft_pub_rec_add, and it may only be answered by the acquire
	 * itself (see the note at its use).
	 */
	bool dual_gp_held = false;
	/*
	 * This op's lock context: the caller's anchor source, plus the words
	 * THIS function holds.  The orphan marks above are part of the held
	 * set even though they sit outside any txn registry.  An acquire
	 * that cannot see them re-takes one and waits on the op itself.
	 * @nr_extra is refreshed at each acquire because the orphan walk is
	 * still growing it.
	 */
	struct ft_lock_ctx lctx;
	/*
	 * The op's descent, COPIED so the orphan walks below may EXTEND it.
	 * Those walks move DOWN a chain past the descent's cursor, and their
	 * nodes' depths exist nowhere else.  Feeding each step through
	 * ft_descent_enter_node keeps ONE depth-tracking mechanism and ONE
	 * anchor table for the whole op.  That is what agreement wants
	 * (doc/design/ft-dlm-lock-coarseness.md §9).  It is a copy, not the
	 * caller's, because the extension is this op's business alone.
	 */
	struct ft_descent wd;
	bool wd_valid = false;
	/*
	 * The trailing skip-target's mark, IN @orphan_held (never a copy).
	 * The held set is what the next acquire consults, and the freeze
	 * needs the same entry the release sweep will clear.  NULL until it
	 * is taken.
	 */
	struct ft_held_anchor *orphan_trailing_held = NULL;
	bool retire_glue_fused = false;
	bool freeze_leaf_fused = false;
	struct cds_ft_node *topmost_external_nodes = NULL;
	/* The node @topmost_external_nodes was lifted off (see the orphan walk). */
	struct cds_ft_metadata *topmost_src_meta = NULL;
	bool prev_external_nodes_found = false;
	/*
	 * Set when the pure-delete leaves the surviving boundary a non-root
	 * 1-child no-external internal whose SKIP_X canonical form is a merged
	 * compressed node.  The chain-compress prune is then FUSED into the
	 * key-removal commit: build the merged node, replace the boundary in
	 * ONE flip.  This bypasses ft_node_replace_ptr and the standalone
	 * post-detach canonicalize.  There is no intermediate
	 * recompacted/in-place node and no transient non-canonical state.
	 * See ft_chain_compress_fused.
	 */
	bool boundary_fused = false;
	/*
	 * The in-place leaf delete took its holder ahead of the plan (the hoist
	 * above ft_node_replace_ptr).  Read at the B2 arm, which stays off for it.
	 */
	bool leaf_hoisted = false;
	struct cds_ft_inode_flag *cur;
	unsigned int cur_depth;
	unsigned int cur_span;
	/*
	 * Snapshot of the slot value at the detach point.  Set once the
	 * upward walk finishes elevating @detach_node_flag_ptr, before
	 * ft_node_replace_ptr overwrites the slot.  The free-walk below
	 * starts from this value, so it covers BOTH the elevated
	 * single-child ancestors and the original detach target.  (The
	 * elevation walk only crosses nodes with nr_child==1, so the
	 * underlying chain reaches the original detach child.)
	 *
	 * It is equally the PLAN EXPECTED-OLD handed to ft_node_replace_ptr.
	 * The orphan set, the count fold and the drop all name THIS subtree.
	 * So a DEL recompaction that finds another one at the slot is working
	 * from a plan a peer has already invalidated (-EAGAIN, re-descend).
	 */
	struct cds_ft_inode_flag *elevated_old_child;
	/*
	 * The same slot's value as the CLIMB read it: the plan's
	 * expected-old.
	 *
	 * The climb reads the nr_child of the nodes it walks.  From that it
	 * decides "every level from the holder up to here holds only the
	 * branch we are removing, so prune at this slot".  That verdict is
	 * about the SUBTREE the slot held THEN.  A peer can republish the
	 * slot mid-climb: an insert splitting the chain below, whose fresh
	 * junction carries the removed key AND the peer's own.  That makes
	 * the verdict false.  Re-reading the slot after the climb adopts the
	 * peer's subtree into the plan.  The prune then unlinks a live key,
	 * silently, with a byte and a child count that are both innocent.
	 * So the value is captured WHERE THE CLIMB USES IT:
	 *   - at entry, for a plan that never elevates;
	 *   - at each elevation, for the level it just decided to prune.
	 *
	 * The FIRST elevation is the load-bearing one.  It gets its value
	 * from the SAME load that produced @cur (@entry_holder_raw below),
	 * not from a second read.  The two are ~300 ns apart and a peer
	 * publish lands between them.
	 *
	 * ☠ AND SO DOES EVERY LATER ONE (@pair_held).  Each level's pair
	 * check has just proved that the slot holding @cur resolves to @cur.
	 * That load is the level's plan, not a re-read at the elevation.
	 * The re-read let a peer's commit land between the two (the
	 * two-writer harness: STALE-AFTER-RM, LTTng-traced):
	 *   1. A remove of the junction's prefix key collapsed the junction
	 *      into a skip pointer to its surviving leaf.
	 *   2. The climb read the junction's external (the key that remove
	 *      had just frozen) as @topmost_external_nodes.
	 *   3. The re-read then named the peer's fresh skip node.
	 *   4. So the orphan walk locked the NEW chain, never the junction.
	 *   5. The promote published the frozen key back into the slot.
	 */
	struct cds_ft_inode_flag *plan_old_child;
	/* This level's pair-checked load of the slot holding @cur. */
	struct cds_ft_inode_flag *pair_held = NULL;
	/*
	 * The shape-D collapse predicate as the PLAN evaluated it (unlocked):
	 * see the residue re-check after ft_node_replace_ptr.
	 */
	bool shape_d_plan = false;
	/*
	 * The detach TARGET as the entry read it (@plan_old_child before the
	 * climb re-anchors it).  The orphan walk must arrive at this node
	 * once it has walked the @nr_elevated links the climb counted.
	 */
	struct cds_ft_inode_flag *entry_target;
	/*
	 * The holder slot the climb starts from, and the ONE raw value it
	 * read there: the value @cur was resolved from.  When the climb
	 * elevates, that slot becomes the drop target.  So this pair IS its
	 * plan expected-old.
	 */
	struct cds_ft_inode_flag **entry_holder_slot;
	struct cds_ft_inode_flag *entry_holder_raw;
#ifdef FT_DEBUG_DEL_TOMB
	struct cds_ft_inode_flag **dbg_arg_detach_slot = detach_node_flag_ptr;
	struct cds_ft_inode_flag **dbg_arg_parent_slot = detach_parent_flag_ptr;
#endif
	/*
	 * Snapshot of the holder slot (@detach_parent_flag_ptr).  It is taken
	 * BEFORE ft_node_replace_ptr overwrites @iter_node_flag with the
	 * fresh recompacted copy.  It is the plan-snapshot expected-old for
	 * the forward republish that flips the holder slot to that copy
	 * (defect #1).  In the external-promote sub-case (no recompaction)
	 * it equals the unchanged @iter_node_flag, so the same-value
	 * republish still holds.
	 */
	struct cds_ft_inode_flag *holder_old_flag;
	/*
	 * nr_keys count fold (LEAF Increment 2).  The removed leaf has a
	 * @count_delta: -1, or 0 for a count-neutral move detach.  It rides
	 * the SAME commit that unlinks the leaf, rather than a standalone
	 * pre-decrement walk.  Each per-outcome fold below records the -1
	 * walk from that outcome's stable base into its commit txn:
	 *   - in-place / recompaction: @commit_txn;
	 *   - shape-D: ft_chain_compress_fused's own txn;
	 *   - compressed parent: @orphan_txn.
	 * It then sets @count_folded.  A lone-store (list-off pub-less)
	 * outcome reaches no txn.  Its residual falls back to the standalone
	 * walk at the end.  All no-ops when !rank_stats.
	 */
	bool count_folded = false;
	/*
	 * Force the list-off lone forward store through a commit txn (MW
	 * safety + nr_keys fold).
	 *
	 * The caller supplies no @pub in list-off: there is no ordered-list
	 * cell to fuse.  An in-place external promote would then store bare
	 * into the LIVE holder's child slot, via ft_node_replace_ptr's
	 * lone-edge arm (ft_node_child_edge_flip).  That is a blind
	 * rcu_assign.  Under MW it resurrects a peer's relocated node or
	 * smashes a parked latch.  It is NOT §4.B-guarded: the
	 * concurrent-recompaction premise requires EVERY live child-slot
	 * mutation to be a proxy'd, holder-serialized txn.
	 *
	 * Defer that store into a LOCAL pub instead.  The pub-armed arm
	 * below commits it via ft_remove_one_commit through @commit_txn
	 * (holder guard + structural edge).  With order statistics on, it
	 * also records the -@count_delta walk from the surviving holder.
	 * That walk goes into the SAME commit, exactly as the list-on fused
	 * path does.
	 *
	 * This is the single choke point for every ft_detach_node caller:
	 *   - leaf remove;
	 *   - remove_all;
	 *   - whole-subtree detach;
	 *   - merge src-unlink;
	 *   - graft_swap extract.
	 * Each passes pub == NULL in list-off, and each is now routed
	 * uniformly through the txn.
	 *
	 * A recompaction shape never arms @pub, so it is unaffected: its
	 * rebuilt copy is published by the recompaction path below.  When
	 * !rank_stats, @count_delta is 0.  The fold is then a no-op and only
	 * the guarded structural edge commits.  @local_pub outlives every
	 * use (whole-function scope).  ft_detach_node never returns @pub.
	 */
	struct ft_remove_pub local_pub = { .armed = false };

	if (!pub)
		pub = &local_pub;

	if (ft_lock_ctx_descent(op_ctx)) {
		wd = *ft_lock_ctx_descent(op_ctx);
		wd_valid = true;
		/*
		 * ☠ A COPY MAY READ THE LEDGER, NEVER APPEND TO IT.  The
		 * orphan walk below extends this copy (ft_walk_extend).
		 * ft_descent_enter_node pushes BEFORE its per-node early
		 * return.  So a copy that kept @anc_rec would file the
		 * ORPHANED SUBTREE onto the key path, under the original
		 * generation.  ft_anc_ledger_valid would still vouch for
		 * the result.  @anc_gen is deliberately kept: reading is
		 * exactly what a copy is for.
		 */
#ifdef FT_ANC_LEDGER
		wd.anc_rec = false;
#endif
	}
	ft_lock_ctx_init(&lctx, wd_valid ? &wd : NULL, NULL,
		op_ctx ? op_ctx->op : NULL);
	lctx.held.extra = orphan_held;
	/*
	 * This frame keeps its OWN out-of-registry array (@orphan_held), so the
	 * caller's would be lost: CHAIN to it.  The rekey fold arrives here
	 * holding ft_rekey_cow_stop's marks, and under a coarse spacing S_top's
	 * fence lands on the very node this detach recompacts.
	 */
	lctx.held.outer = op_ctx ? &op_ctx->held : NULL;

	FT_TP(detach_node_enter, (const void *) *detach_node_flag_ptr, detach_depth);

	/*
	 * Check the node being replaced (the child at detach_node_flag_ptr)
	 * for external_nodes.  This detach was triggered by the removal of
	 * the child's last internal child.  The child may still hold
	 * variable-length key entries.  They must be preserved by promoting
	 * them to the replacement slot.
	 *
	 * This is only for a destroy-style detach (free_detached_subtree).
	 * A move-style detach PRESERVES the target subtree: it becomes
	 * another trie's content.  So the target keeps its own
	 * external_nodes, and they must NOT be promoted into the source.
	 * This matters once a move-style caller bootstraps from the target
	 * itself (the climb starts at *detach_node_flag_ptr == the target).
	 * The descent-based callers passed the single-child chain head
	 * here.  That head never carries external_nodes, so this gate is a
	 * no-op for them.
	 */
	if (free_detached_subtree) {
		/*
		 * ☠ ONE RESOLUTION FOR BOTH USES.  The leaf check below and
		 * the external_nodes capture after it must judge the SAME
		 * value.  The slot can hold a peer's parked proxy.  A proxy
		 * resolves through a decision that may flip between two
		 * resolutions:
		 *   1. The check resolved it UNDECIDED, to the old value
		 *      (our leaf), and passed.
		 *   2. The capture resolved it DECIDED, to the peer's fresh
		 *      junction.  It took the junction's external_nodes
		 *      (our leaf) as a chain to promote.
		 *   3. The climb then pruned the boundary.  It published
		 *      the leaf this op removes into the boundary's slot,
		 *      frozen, with its cell already unspliced.
		 *   4. Every later insert whose predecessor search lands on
		 *      it planned against the dead cell and retried for
		 *      ever.
		 * MEASURED (ft_inv 147, inv_prefix_pair_compressed_holder: a
		 * two-insert livelock after a remove committed root[96]:
		 * compressed -> the removed leaf).
		 */
		struct cds_ft_inode_flag *detach_child =
			ft_resolve_flip_proxy(*detach_node_flag_ptr);

		/*
		 * ☠ THE SLOT MUST STILL HOLD THE CALLER'S LEAF.  Every
		 * destroy-style caller is a remove.  It names the chain head
		 * it came to clear (@freeze_leaf), and it checked it at
		 * @detach_node_flag_ptr at an EARLIER instant.  A peer can
		 * insert a LONGER key through that head (past-child:
		 * "zone46" under "zone4").  That republishes the slot with a
		 * fresh junction, which carries the head as its
		 * external_nodes AND the peer's key as a child.  Read here,
		 * that junction is "a detached internal node whose prefix
		 * head must be promoted".  The plan below then drops the
		 * junction, and the peer's key with it.  It republishes the
		 * head this op is REMOVING.  MEASURED: key loss within
		 * seconds with two writers, one key each.  Nothing is built,
		 * locked or reserved yet: re-descend.
		 */
#ifndef FT_DEBUG_NO_LEAF_IDENTITY
		if (freeze_leaf && caa_unlikely((struct cds_ft_node *)
				ft_node_ptr(detach_child) != freeze_leaf))
			return -EAGAIN;
#endif
		/*
		 * RESOLVE-THEN-SKIP, in that order (ft-helpers.h:1870).  The
		 * entry-holder read below at :2373 does exactly this:
		 * ft_reanchor_flag(ft, ft_resolve_flip_proxy(raw), ...).
		 *
		 * ☠ THE FLIP-PROXY HALF WAS MISSING HERE, AND IT FAULTS.  (It
		 * is now done once, at the load above -- see there for why
		 * once.)  This slot can hold a parked proxy: a concurrent
		 * one-commit splice parks a type-7 MCAS descriptor in it.
		 * The low nibble 0xF then fails ft_node_external.  So the
		 * type check below admits it, and ft_flag_to_metadata
		 * computes a metadata address from a DESCRIPTOR pointer.
		 *
		 * Two neighbouring comments already name that outcome:
		 *   - The comment on the very next statement: "the SKIP_X
		 *     form dispatches the 0xF flag as a node
		 *     (ft_node_external fails -> item_to_metadata faults)".
		 *   - The sibling parent-slot read twenty lines down says a
		 *     raw load "feeds that proxy to
		 *     cds_ft_item_to_metadata() ... and faults".
		 * Both guard their slot.  This one did not, and it is a
		 * measured SEGV site.  2 of 5 sampled rc=139 backtraces
		 * crash at the ft_flag_to_metadata below
		 * (doc/design/ft-stale-disposal-predicate.md §5.17).
		 *
		 * Skip-compressed must still be resolved before the type
		 * checks.  A skip pointer with an external child has low
		 * bits == 0.  It falsely matches ft_node_external and skips
		 * the external_nodes preservation entirely.
		 *
		 * It is sound to resolve.  @detach_child is only ever
		 * DEREFERENCED here (its metadata read for external_nodes
		 * preservation).  It is never used as a CAS expected-old.
		 * The entry-holder read below makes the same argument for
		 * @cur.
		 */
		detach_child = ft_resolve_skip_compressed(ft, detach_child);

		if (detach_child && !ft_node_external(detach_child)) {
			struct cds_ft_metadata *child_meta =
				ft_flag_to_metadata(ft, detach_child);
			/*
			 * Resolve a parked flip proxy before capture.  A
			 * concurrent one-commit splice
			 * (ft_insert_park_external_nodes) transiently installs
			 * its descriptor in external_nodes.  The
			 * external-promote below republishes this value RAW
			 * into cn->child + the SKIP_X dual.  Snapshotting a
			 * peer's latch into a structural slot leaves a
			 * dangling descriptor once that txn settles and
			 * reclaims.  And the SKIP_X form dispatches the 0xF
			 * flag as a node (ft_node_external fails ->
			 * item_to_metadata faults).  Mirror every
			 * ft_dereference_external descent reader.  The common
			 * no-splice case is one masked test.
			 */
			if (child_meta && child_meta->external_nodes) {
				topmost_external_nodes = ft_dereference_external(
					child_meta->external_nodes);
				topmost_src_meta = child_meta;
			}
		}
	}

	/*
	 * Walk upward from the parent of the detached node via
	 * metadata->parent.  At each ancestor, check if it has only
	 * one child left.  If so, mark it for pruning and continue.
	 * Stop when reaching a multi-child node, a node with
	 * external_nodes, or the root (parent == NULL).
	 */
	/*
	 * Resolve the initial parent slot.  Two layers:
	 *
	 *  - Flip-proxy: @detach_parent_flag_ptr is a forward child slot (the
	 *    grandparent's slot to the holder).  A concurrent recompaction
	 *    re-home transiently parks a type-7 MCAS proxy in it.  A raw
	 *    load feeds that proxy to cds_ft_item_to_metadata() at the top
	 *    of the up-walk below and faults.  That is the SAME crash the
	 *    loop-body parent read guards against, but on iteration 0.
	 *    Resolve it exactly as cds_ft_remove does at its
	 *    compressed-child identity compare.  @cur is only ever
	 *    dereferenced here (never a CAS expected-old), so resolving is
	 *    sound.
	 *
	 *  - Skip-compressed: the detach can be bootstrapped from a leaf
	 *    whose holder is a compressed node (e.g. cds_ft_remove deriving
	 *    the position from node->prev).  The slot is then skip-encoded
	 *    (SKIP_X(cn->child)).  ft_node_ptr does not strip the skip high
	 *    bits, so resolve to the plain compressed flag.  No-op for a
	 *    plain (descent) slot.
	 */
	{
		/*
		 * Reanchor a skip-compressed initial slot through the shared
		 * read-side primitive (MW convergence), not through the
		 * unvalidated one-hop ft_resolve_skip_compressed.  A peer
		 * split can tear the one-hop recovery to a node whose
		 * ft_meta_nr_child reads a bitmap byte (the skip-gated
		 * ft-remove.h prune-climb 0-child assert).  @cur is only
		 * ever dereferenced (never a CAS expected-old), so using the
		 * reanchored LIVE node is sound.  A rewind is tolerated: the
		 * climb self-corrects / the commit guards it.
		 */
		unsigned int cur_rewind;

		/*
		 * ONE load feeds both @cur and @entry_holder_raw.  The
		 * climb's whole verdict about this level is derived from
		 * @cur.  So the expected-old that says "the slot still holds
		 * what I walked" must be the SAME read that produced it.  A
		 * second load a few hundred nanoseconds later is a
		 * coherent-pair violation, and MEASURED to be one.  A peer's
		 * publish landing between the two is captured as the plan's
		 * own expected value and then pruned.
		 */
		entry_holder_raw = (struct cds_ft_inode_flag *)
			rcu_dereference(*detach_parent_flag_ptr);
		cur = ft_reanchor_flag(ft,
			ft_resolve_flip_proxy(entry_holder_raw),
			&cur_rewind);
	}
	entry_holder_slot = detach_parent_flag_ptr;
	/*
	 * HOLDER IDENTITY: @cur must be the very node @detach_node_flag_ptr
	 * addresses a slot inside.  The caller reached that slot by
	 * descending through the holder.  @cur is a SEPARATE, later load of
	 * the slot that holds the holder.  A peer can republish the holder
	 * between the two: an insert splitting the compressed chain here,
	 * whose fresh copy carries the removed key AND its own.  The pair
	 * then disagrees: the slot addresses the retired body, @cur names
	 * the fresh copy.
	 *
	 * The climb then walks the WRONG node.  It is fatal precisely because
	 * the fresh copy looks prunable.  A compressed node structurally
	 * holds exactly one child.  So the climb scores it a single-child
	 * ancestor, elevates past it, and drops the branch whole.  The
	 * multi-child junction the peer published UNDER it is never looked
	 * at, and the peer's key goes with the prune.  No expected-old on the
	 * holder slot can see this.  That slot's value agrees with itself at
	 * every load: the climb elevates ONTO it and both reads return the
	 * fresh copy.  What is stale is the DESCENT's premise: "the chain
	 * below this holder holds only the key I am removing".  This is
	 * where that premise is checkable.
	 *
	 * Nothing is built, locked or reserved yet: re-descend against the
	 * settled tree.
	 *
	 * ☠ THIS TESTS THE SLOT THE OP READS, NOT THE ONE IT WRITES.  The
	 * slot an elevation clears, and a DEL recompaction publishes into, is
	 * @detach_parent_flag_ptr.  "It is the slot ft_get_parent_slot
	 * recovered INSIDE the new @cur" was an assumption.  The address was
	 * derived at an EARLIER instant.  A peer that republishes @cur's
	 * parent since leaves it pointing into the RETIRED body, while @cur's
	 * word names the fresh copy.  That slot is validated per level inside
	 * the climb below, against the parent @cur's own word names.
	 * MEASURED: the elevated clear landed 0x30 past a 32-byte parent, in
	 * the neighbouring item.  Re-resolving the pair at the refusal finds
	 * it self-coherent EVERY time: it is a stale plan, not a torn read.
	 */
	if (caa_unlikely(!ft_slot_in_node(cur, detach_node_flag_ptr)))
		return -EAGAIN;
	/* Plan expected-old for a detach that never elevates (see @plan_old_child). */
	plan_old_child = (struct cds_ft_inode_flag *)
		rcu_dereference(*detach_node_flag_ptr);
	entry_target = ft_resolve_flip_proxy(plan_old_child);
	FT_DT_INC(ft_dt_cap0_reach);
	if (!plan_old_child)
		FT_DT_INC(ft_dt_cap0_null);
	/*
	 * ☠ A NULL PLAN EXPECTED-OLD IS A STALE PLAN, NOT A VALUE.  The slot
	 * this op came to clear is ALREADY EMPTY.  A peer's in-place delete
	 * landed between the caller's position derivation and this capture.
	 * (The derivation is ft_locate_chain_head / node->prev, both
	 * validated against a slot that then held the chain.)
	 *
	 * The recompacting tier never produced this state.  It retired the
	 * holder with the delete, so the {C,P,GP} acquire refused the
	 * tombstone.  The in-place tier leaves a live holder with a NULL
	 * hole instead.  That is exactly what the equality guard below
	 * cannot see: it compares the re-read against the plan, and NULL
	 * equals NULL.
	 *
	 * Left through, one of two things happens:
	 *   - The plan reaches ft_node_replace_ptr with a NULL expected-old
	 *     (ft_popcount_node_replace_ptr's `*node_flag_ptr != NULL`).
	 *     MEASURED on inv_concurrent_remove_all_nolist: both sides NULL
	 *     at the guard, 1 of ~1000 plans.
	 *   - Past min_child, a DEL recompaction drops a child that is not
	 *     there and folds -1 a second time.
	 *
	 * Nothing is built, locked or reserved yet: re-descend.  The wrapper
	 * re-seeds through a fresh lookup, which cannot return a chain head
	 * from an empty slot.  So this -EAGAIN clears on the next attempt:
	 * NOT_FOUND, or a fresh plan against whatever a re-insert put here.
	 */
#ifndef FT_DEBUG_NO_NULLPLAN
	if (caa_unlikely(!plan_old_child))
		return -EAGAIN;
#endif
	/*
	 * The same identity, on the read the PLAN rests on.  The check above
	 * is a separate, earlier load.  The peer's publish can land between
	 * the two.  Then everything below is coherent with the peer's
	 * junction: the climb, the orphan walk and the replace.  Every
	 * expected-old matches.
	 * Only the caller's leaf says the plan is about the wrong node.
	 */
#ifndef FT_DEBUG_NO_LEAF_IDENTITY
	if (freeze_leaf && caa_unlikely((struct cds_ft_node *)
			ft_node_ptr(ft_resolve_flip_proxy(plan_old_child)) !=
				freeze_leaf))
		return -EAGAIN;
#endif
	/*
	 * @cur HOLDS the detached child's slot, so it starts a full SPAN
	 * above it.  The span is one key byte for a bitmap node and the whole
	 * run for a compressed one.  That is what ft_node_span answers.  A
	 * span wider than the child's own depth means @cur is not the parent
	 * of anything at that depth.  The plan is then stale, so re-descend.
	 * Nothing is built, locked or reserved yet, exactly as the bail
	 * above.
	 */
	cur_span = ft_node_span(ft, cur);
	if (caa_unlikely(cur_span > detach_depth))
		return -EAGAIN;
	cur_depth = detach_depth - cur_span;

	/*
	 * nr_keys count fold (LEAF Increment 2): there is no standalone
	 * pre-decrement here anymore.  Each per-outcome commit below folds
	 * the @count_delta walk onto its own flip.  So the count flips
	 * ATOMICALLY with the unlink (exact under concurrent writers).  See
	 * @count_folded.
	 */

	while (cur) {
		struct cds_ft_metadata *metadata;
		struct cds_ft_inode_flag *resolved_parent;
		unsigned int nr_child;
		bool is_root;

		metadata = cds_ft_item_to_metadata(ft_node_ptr(cur));
		metadata_stack[nr_metadata++] = metadata;
		pair_held = NULL;
		/*
		 * ONE proxy-resolved snapshot of this ancestor's parent
		 * back-pointer.  Every use below consumes it:
		 *   - the is_root boundary test;
		 *   - the boundary parent_meta classify;
		 *   - the climb.
		 * &metadata->parent is a flip-txn parked slot
		 * (ft_reparent_record_meta).  So a concurrent recompaction
		 * re-home transiently parks a type-7 proxy here.  A raw load
		 * fed to cds_ft_item_to_metadata() computes metadata off the
		 * latch/record memory and faults (the dominant FT_INV_MW
		 * up-walk crash).  A raw non-NULL proxy also mis-answers
		 * is_root.  Resolve once, exactly as the reader up-walk
		 * ft_skip_reanchor does.  That is a predicted-not-taken
		 * mask-compare when no re-home is in flight, so
		 * single-writer is unchanged.
		 */
		resolved_parent = ft_parent_node_resolved(
				rcu_dereference(metadata->parent_word));
		is_root = (resolved_parent == NULL);
		boundary_parent_nf = resolved_parent;	/* always names @cur */
		/*
		 * THE PAIR THIS LEVEL WRITES THROUGH MUST BE COHERENT.
		 *
		 * @detach_parent_flag_ptr is the slot this plan may CLEAR (an
		 * elevation) or PUBLISH INTO (a DEL recompaction of @cur).  The
		 * CALLER derived it from @cur's (parent_word,
		 * parent_slot_offset) at an earlier instant.  By the time this
		 * level uses it, a peer may have REPUBLISHED @cur's parent.  (A
		 * recompaction copies the parent body and retires the
		 * original.)  The caller's address then points into the
		 * RETIRED body, while @cur's word names the fresh copy.  Each
		 * value is coherent alone, but they are not a pair.
		 *
		 * Two existing checks cannot see this:
		 *   - ft_resolve_parent_slot: its coherence re-read only
		 *     catches a re-home landing on the parent WORD, and that
		 *     word is stable across both loads.
		 *   - The entry HOLDER IDENTITY test: it checks the slot this
		 *     op READS (@detach_node_flag_ptr inside @cur), not the
		 *     slot it WRITES.
		 *
		 * MEASURED on inv_concurrent_remove_all_nolist, per-node AND
		 * root-only:
		 *   - the parent word resolved to a 32-byte node, while the
		 *     offset read 6 (an order-6 body's);
		 *   - so the elevated clear NULLed a word of the NEIGHBOURING
		 *     item, which held a live external head;
		 *   - and it fused the nr_child-- onto the real parent.
		 * That parent then carries one non-NULL child its count does
		 * not cover.  Every plan built from the count is wrong.  Every
		 * validation against the structure fails.  The node WEDGES
		 * (the memcg-killed legs).
		 *
		 * ☞ That signature was FIRST read as a torn two-store
		 * re-parent.  That reading is REFUTED.  A parent republished
		 * between the caller's derivation and this use produces the
		 * same order-5-word / offset-6 disagreement, with no torn read
		 * at all.  Four independent probes agree it is the republish:
		 *   - Re-resolving the pair AT the refusal finds it
		 *     self-coherent in 100% of why2 hits (28/28 per-node,
		 *     119/119 root-only).
		 *   - Three writer-side probes all read ZERO on this row.
		 *     They count an offset store onto a word naming a
		 *     different parent, a parent-word-only re-parent, and a
		 *     plain offset store onto a child still REACHABLE
		 *     through its own pair.
		 * Nobody tears the pair; the plan simply goes stale.  So this
		 * block is a STALE-PLAN refusal.  No re-parent producer is
		 * owed a fix on its account.
		 *
		 * The witness independent of the torn pair is the parent that
		 * @cur's OWN word names, resolved just above.  Two tests use
		 * it:
		 *   - the slot must lie INSIDE that parent's body (or be the
		 *     trie root slot);
		 *   - what the slot holds must be @cur.
		 * Both are address or identity tests.  This op adopts no value
		 * from them.  Nothing is built, locked or reserved, so
		 * re-descend.  The retry reads the pair after the re-parent's
		 * second store has landed.
		 */
		{
			int pair_bad = 0;
			struct cds_ft_inode_flag *held = NULL, *held_res = NULL;

			if (is_root) {
				if (caa_unlikely(detach_parent_flag_ptr != &ft->root))
					pair_bad = 1;
			} else if (caa_unlikely(!ft_slot_in_node(resolved_parent,
					detach_parent_flag_ptr))) {
				pair_bad = 2;
#ifdef FT_DEBUG_DEL_TOMB
				/*
				 * DISCRIMINATOR for why2.  Two different faults
				 * produce the same signature: "the slot lies
				 * outside the parent the word names".
				 *
				 *  (a) A TORN PAIR.  @cur's own (parent_word,
				 *      parent_slot_offset) were read one after
				 *      the other across a two-store re-parent.
				 *      The pair NEVER described one real edge.
				 *  (b) A STALE PLAN.  The pair is coherent, but
				 *      a peer REPUBLISHED the parent after the
				 *      CALLER derived @detach_parent_flag_ptr.
				 *      The caller's address points into the
				 *      retired body.  The word names the fresh
				 *      copy.
				 *
				 * Re-resolving the pair NOW separates them.  If
				 * the fresh slot lands inside the fresh parent,
				 * the pair is self-coherent, so (b) happened.
				 * The refusal is right either way.  This only
				 * says WHICH defect the -EAGAIN is absorbing,
				 * i.e. whether a producer is still owed a fix.
				 */
				{
					struct cds_ft_inode_flag *fresh_parent = NULL;
					struct cds_ft_inode_flag **fresh_slot =
						ft_resolve_parent_slot(metadata, ft,
							&fresh_parent);

					if (fresh_parent && ft_slot_in_node(
							ft_resolve_flip_proxy(fresh_parent),
							fresh_slot))
						FT_DT_INC(ft_dt_pair_fresh_ok);
					else
						FT_DT_INC(ft_dt_pair_fresh_bad);
				}
#endif
			} else {
				held = ft_resolve_flip_proxy(
					(struct cds_ft_inode_flag *)
					rcu_dereference(*detach_parent_flag_ptr));
				/*
				 * A compressed @cur is named by its parent through
				 * the SKIP_X form of ITS child (an external head's
				 * address with the skip bits set).  So resolve
				 * the skip encoding BEFORE asking whether the
				 * value is a bare external.  Only a bare one is
				 * a foreign word.
				 */
				held_res = held ?
					ft_resolve_skip_compressed(ft, held) : NULL;
				if (caa_unlikely(!held_res ||
						ft_node_external(held_res)))
					pair_bad = 3;
				else if (caa_unlikely(ft_node_ptr(held_res) !=
						ft_node_ptr(cur)))
					pair_bad = 4;
				if (!pair_bad)
					pair_held = held;
			}
			if (caa_unlikely(pair_bad)) {
#ifdef FT_DEBUG_DEL_TOMB
				uatomic_inc(&ft_dt_pair_why[pair_bad]);
				if (uatomic_add_return(&ft_dt_pair_refused, 1) <= 8)
					fprintf(stderr, "FT DT-PAIR-REFUSE why=%d iter=%d cur=%p cur_meta=%p resolved_parent=%p is_root=%d slot=%p slot_val=%p held=%p held_res=%p entry_slot=%p arg_parent_slot=%p climbed=%d\n",
						pair_bad, nr_branch, (void *) cur,
						(void *) metadata, (void *) resolved_parent,
						(int) is_root, (void *) detach_parent_flag_ptr,
						(void *) CMM_LOAD_SHARED(*detach_parent_flag_ptr),
						(void *) held, (void *) held_res,
						(void *) entry_holder_slot,
						(void *) dbg_arg_parent_slot, (int) climbed);
#endif
				return -EAGAIN;	/* torn plan: re-descend */
			}
		}
		FT_DT_INC(ft_dt_pair_checked);
		/*
		 * ONE proxy-resolved snapshot of this ancestor's child count.
		 * The two reasons are exactly those for @resolved_parent just
		 * above.  &metadata->state is the SAME kind of parked slot.
		 *
		 * RESOLVED: a peer's in-flight commit parks an engine record
		 * POINTER in the state word (FT_STATE_PROXY, bit 0).  A raw
		 * ft_meta_nr_child() then decodes bits 2-10 of that pointer as
		 * the count.  That value is arbitrary, and almost always > 1.
		 * It answers YES to "this ancestor is a surviving multi-child
		 * boundary", for a node that in truth holds only the child we
		 * came up from.  The prune then stops one level too low, and
		 * ft_node_replace_ptr DELs the boundary's last child.
		 * ft_node_recompact's NODE_INDEX_NULL assert catches that case
		 * at the dereference site.
		 *
		 * ONCE: the three tests below must agree on one value.  They
		 * are the emptied-ancestor bail, the nr_clear tally and the
		 * boundary stop.  Three separate loads of a word peers mutate
		 * can disagree.  A plan built from two different counts is
		 * incoherent, even when each load is resolved on its own.
		 *
		 * Off the commit window, urcu_txn_read short-circuits to a
		 * plain load, so single-writer is unchanged.
		 */
		nr_child = ft_meta_nr_child_load(metadata);

		/*
		 * A climbed ancestor with nr_child == 0 means a peer is
		 * recompacting or emptying a shared spine node right now.
		 * Skip-compression drives this churn, hence skip-gated.  Our
		 * prune plan is racing that peer.  Bail before any build and
		 * re-descend against the settled tree, exactly as the
		 * flip-proxy holder bail below.
		 *
		 * Under mutual exclusion this is impossible.  A climbed
		 * ancestor always still holds the child we came up from
		 * (nr_child >= 1).  So the pre-MW invariant assert becomes an
		 * MW retry point, not a fatal abort.
		 */
		if (caa_unlikely(nr_child == 0))
			return -EAGAIN;
		/*
		 * ☠ AND A RETIRED ANCESTOR VOIDS THE WHOLE PLAN, at EVERY
		 * level of the climb.  That includes the boundary, since the
		 * stop test below runs on this same @cur.
		 *
		 * The climb reads every ancestor with NOTHING held.  From
		 * those reads it decides:
		 *   - which slot is cleared;
		 *   - which chain is orphaned;
		 *   - where the prune stops.
		 * A peer that RETIRED any node on this path has already
		 * republished a fresh copy elsewhere.  So each of those
		 * decisions names a node that is no longer in the trie.  The
		 * plan then condemns, frees and re-parents around a dead path.
		 * nr_child cannot see it.  A retired node keeps its body
		 * readable (RCU) and its count intact.  Only the TOMBSTONE bit
		 * says the node has left.
		 *
		 * The recompacting delete never needed this either.  Its
		 * {C,P,GP} acquire refuses PROXY|TOMBSTONE|LOCK outright, so a
		 * dead ancestor was rejected at the lock.  The in-place tier
		 * removes that acquire from the common path.  That is why the
		 * check has to be stated here instead.
		 *
		 * Bail before anything is built, locked or reserved, exactly as
		 * the nr_child == 0 arm above.
		 */
		FT_DT_INC(ft_dt_climb_reach);
		if (caa_unlikely(ft_meta_tombstone(metadata))) {
			FT_DT_INC(ft_dt_climb_tomb);
			return -EAGAIN;
		}
		if (!prev_external_nodes_found && (nr_child == 1 && !metadata->external_nodes && !is_root)) {
			nr_clear++;
		}
		nr_branch++;
		/*
		 * Stop the upward prune at a surviving boundary.  A boundary
		 * is one of:
		 *   - a multi-child node;
		 *   - the root;
		 *   - a level past one whose external_nodes were promoted
		 *     (prev_external_nodes_found);
		 *   - a node that keeps its own key (external_nodes), once a
		 *     deeper promotion is already in flight
		 *     (topmost_external_nodes set).
		 *
		 * A SINGLE-child node that carries external_nodes does NOT
		 * stop here.  Pruning its only (on-path) child empties it, so
		 * it cannot stay.  Its external chain is promoted into its own
		 * parent slot, and the climb continues past it (handled just
		 * below).
		 *
		 * The detach may be bootstrapped from a node that already
		 * carried external_nodes (descent callers, topmost set at
		 * start).  Then that node is the one being promoted, and a
		 * further external ancestor is a genuine boundary.  Hence the
		 * `&& topmost_external_nodes` guard.
		 */
		if (prev_external_nodes_found || nr_child > 1 ||
		    (metadata->external_nodes && topmost_external_nodes) ||
		    is_root) {
			if (!is_root) {
				struct cds_ft_metadata *parent_meta;

#ifdef FT_ENABLE_TRACING
				/*
				 * THE CLIMB'S DEREFERENCE, checked before it
				 * happens.  The residual SEGV faults in this
				 * frame.  This signature SURVIVES tracing.  The
				 * reader-side one does not: 0/2100 traced
				 * against ~1%/run untraced.
				 *
				 * A freelist link is 8-mod-16, so it clears the
				 * EXTERNAL tag.  A reclaimed parent then reads
				 * as an external node here.  ft_get_parent_rcu
				 * asserts exactly that invariant, and this
				 * climb never checked it.  Round-trip it, so
				 * the event alone separates recycled memory
				 * from a live object with a bad link.
				 */
				if (caa_unlikely(resolved_parent &&
						ft_node_external(resolved_parent))) {
					const void *pp = ft_node_ptr(resolved_parent);

					FT_TP(parent_external_violation,
						(const void *) cur, pp,
						(const void *) cds_ft_metadata_to_item(
							cds_ft_item_to_metadata(
							(struct cds_ft_inode *) pp)),
						(unsigned long) metadata->parent_word);
					ft_trace_capture();
					abort();
				}
#endif
				parent_meta = cds_ft_item_to_metadata(
						ft_node_ptr(resolved_parent));
				metadata_stack[nr_metadata++] = parent_meta;
			}
			/*
			 * Find the key byte for replace_ptr.  Only needed
			 * for internal parents (compressed handled
			 * separately below).
			 *
			 * ☠ A MISS IS A STALE PLAN, NOT A CAN'T-HAPPEN.  The
			 * reverse lookup compares the slot's live value
			 * against @*detach_node_flag_ptr by RAW IDENTITY.  It
			 * does so across two separate loads of the same word.
			 * This climb runs PRE-FENCE.  A peer commit on that
			 * slot can land between the two loads.  Then every
			 * slot mismatches, and the lookup returns false.  Its
			 * header calls that "should not happen on a
			 * well-formed trie".  That is true single-writer, and
			 * it is why the result went unchecked.
			 *
			 * Left unchecked, the miss is SILENT AND FATAL:
			 *   - @n keeps its initialiser 0.
			 *   - The shape-D survivor scan below skips byte @n to
			 *     exclude the child being detached.  With @n == 0
			 *     it excludes nothing.  It takes the FIRST live
			 *     child: the detached branch itself.
			 *   - The post-fence re-validation cannot catch it.  It
			 *     only asks that @surviving_byte still maps to
			 *     @surviving_child.  That is trivially true of the
			 *     detached child.
			 *   - So the collapse fuses around the very head this
			 *     commit freezes.  The real sibling's key is
			 *     ORPHANED, and a REMOVED node stays reachable.
			 *   - Readers never consult the removed mark, so
			 *     cds_ft_lookup returns that tombstone as live.
			 *   - The next insert onto it records slot (void **) 2
			 *     from a marked `next`, and the MW install
			 *     dereferences it.
			 *
			 * Do NOT retry the lookup in place.  A retried
			 * find_child succeeds (measured: 50,590 of 50,590
			 * misses are transient).  But it then names the
			 * PEER'S NEW child, a subtree this climb never
			 * counted.  Abandoning the plan is the only correct
			 * response.
			 *
			 * MEASURED on a build that widens the miss-to-scan
			 * window: a 10,000-iteration cpu_relax on the miss
			 * path, nothing else.  Exponential spacing, two arms
			 * that differ by this bail alone.  64 of 96 runs SEGV
			 * without it, 0 of 96 with it.
			 */
			if (!ft_node_compressed(cur) &&
					caa_unlikely(!ft_node_find_child(ft, cur,
						*detach_node_flag_ptr, &n, NULL)))
				return -EAGAIN;	/* stale plan: re-descend */
			/*
			 * PLAN -> COMMIT window (-DFT_DELAY_INJECT only, no-op
			 * otherwise).  The decision "this ancestor keeps a child,
			 * so it stays wired" was just made from a count read a
			 * few lines up.  Everything that empties it happens
			 * after this point.  Widening the gap here lets a peer's
			 * removal of the OTHER child land inside the window.
			 * The residual needs that interleaving.  Otherwise it
			 * shows ~1 run in 100.
			 */
#ifndef FT_DELAY_SITE_B_ONLY
			ft_delay_writer();
#endif
			break;
		}
		/*
		 * A single-child node, made childless by the prune, that
		 * carries a shorter key: promote its external chain.  The
		 * deepest such node on the path is the one promoted.
		 * prev_external_nodes_found then stops the climb at the next,
		 * surviving level.
		 */
		/* Resolve a parked splice proxy before republish (see above). */
		if (metadata->external_nodes && !topmost_external_nodes) {
			topmost_external_nodes = ft_dereference_external(
				metadata->external_nodes);
			topmost_src_meta = metadata;
			/*
			 * The junction the prune empties.  Its head takes its
			 * place in the boundary's slot, so the node itself is
			 * unlinked.  It is an orphan the walks below must
			 * budget for.
			 * ☠ It is NOT in @nr_clear (that tally is for keyless
			 * links).  A walk budgeted on @nr_clear alone stops one
			 * node short.  The plain REMOVE hid that: its
			 * target-chain walk picks up whatever the elevated budget
			 * left behind.  A MOVE-style detach runs no such walk.
			 * It leaked the last link of every promote: the junction
			 * under a compressed boundary, else the run under the
			 * junction.
			 */
			climb_promoted = topmost_external_nodes != NULL;
		}
		if (topmost_external_nodes)
			prev_external_nodes_found = true;

		/*
		 * Walk up: the current node becomes the child,
		 * update detach pointers to prune at this level.
		 */
		{
			struct cds_ft_inode_flag *parent_nf = resolved_parent;

			if (!parent_nf)
				break;
#ifdef FT_IMMEDIATE_FREE
			{
				unsigned char *_p = (unsigned char *) ft_node_ptr(parent_nf);
				if (*_p == 0xfe) {
					fprintf(stderr, "ft_detach_node: stale parent detected! "
						"cur=%p cur_depth=%u parent_nf=%p (poisoned) "
						"detach_depth=%u nr_clear=%d\n",
						cur, cur_depth, parent_nf,
						detach_depth, nr_clear);
					abort();
				}
			}
#endif
			/*
			 * Find the slot in the grandparent pointing to
			 * cur, which becomes the new detach_parent_flag_ptr.
			 * Find the slot in cur pointing to its child (the
			 * previous level), which becomes detach_node_flag_ptr.
			 */
			{
				/*
				 * Climb one level.  cur is pruned, with its
				 * child-on-path (already at detach_node_flag_ptr's
				 * level).  So the detach target becomes cur, and its
				 * parent becomes parent_nf.  The new
				 * detach_parent_flag_ptr must therefore point to
				 * PARENT_NF's slot in its own parent.  Then
				 * *detach_parent_flag_ptr == parent_nf == the new
				 * cur.  That slot is recovered in O(1) from
				 * parent_nf's metadata parent-slot offset.
				 * ft_get_parent_slot handles all kinds:
				 *   - root (parent == NULL) -> &ft->root;
				 *   - compressed -> its grandparent slot;
				 *   - plain internal -> the body slot.
				 * (The previous code recovered cur's own slot here.
				 * That aliases detach_parent_flag_ptr, and left
				 * *detach_parent_flag_ptr != cur after the climb.  It
				 * was a latent bug, never reached: descent callers
				 * pass the surviving-ancestor detach point and break
				 * above.)
				 */
				struct cds_ft_metadata *parent_nf_meta =
					cds_ft_item_to_metadata(ft_node_ptr(parent_nf));
				struct cds_ft_inode_flag **new_parent_flag_ptr =
					ft_get_parent_slot(parent_nf_meta, ft);
				/*
				 * Defensive: the recovered slot may alias the
				 * current detach_parent_flag_ptr.  Advancing would
				 * then put both pointers at the same slot.  That
				 * breaks the replace, which assumes
				 * detach_node_flag_ptr is WITHIN iter_node_flag's
				 * child array.
				 *
				 * ☠ A STALE PLAN, NOT A BOUNDARY.  The slot recovered
				 * from @parent_nf's metadata lives in ITS parent's
				 * body.  The slot that holds @cur lives in
				 * @parent_nf's own.  They alias only when the chain
				 * this climb read is not a tree any more: a peer's
				 * relocation seen through reclaimed memory.
				 * Breaking here used to keep the level's verdict
				 * (its @nr_clear tally, and now its promote), while
				 * NOT elevating past it.  Both orphan walks would
				 * then reach one link too far: the detach target
				 * itself, freed while live.  Nothing is built, locked
				 * or reserved yet.  So re-descend, as every other
				 * stale-plan bail in this climb does.
				 */
				if (caa_unlikely(new_parent_flag_ptr ==
						detach_parent_flag_ptr))
					return -EAGAIN;
				detach_node_flag_ptr = detach_parent_flag_ptr;
				detach_parent_flag_ptr = new_parent_flag_ptr;
				/*
				 * Re-anchor the plan's expected-old onto the level
				 * this iteration just decided to prune.  @cur held
				 * only the branch below.  So the slot that holds @cur
				 * is the one the drop targets.  The verdict rests on
				 * the value @cur was resolved from (see
				 * @plan_old_child).  For the entry holder, that value
				 * is already in hand.  Reusing it keeps the pair
				 * coherent.  Deeper levels reach @cur through the
				 * back-pointer chain, and have no earlier read.
				 */
				plan_old_child = detach_node_flag_ptr ==
						entry_holder_slot ?
					entry_holder_raw :
#ifndef FT_DEBUG_CLIMB_PLAN_REREAD
					pair_held;
#else
					(struct cds_ft_inode_flag *)
						rcu_dereference(*detach_node_flag_ptr);
#endif
				FT_DT_INC(ft_dt_cap1_reach);
				if (!plan_old_child)
					FT_DT_INC(ft_dt_cap1_null);
				/*
				 * The level just decided to prune is already
				 * empty.  This is the same stale plan as the
				 * entry capture, with the same answer (see
				 * there).
				 */
#ifndef FT_DEBUG_NO_NULLPLAN
				if (caa_unlikely(!plan_old_child))
					return -EAGAIN;	/* stale plan: re-descend */
#endif
			}
			/*
			 * One hop up moves the byte-depth by the PARENT's span,
			 * never by one.  A compressed parent consumes its whole
			 * run.  Dating the climb by one per ancestor puts every
			 * node it reports at a depth the descent disagrees with.
			 * A lock-set member is then anchored at a level that is
			 * not its own.
			 */
			cur_span = ft_node_span(ft, parent_nf);
			if (caa_unlikely(cur_span > cur_depth))
				return -EAGAIN;	/* stale plan: re-descend */
			cur_depth -= cur_span;
			cur = parent_nf;
			climbed = true;
		}
	}

	iter_node_flag = *detach_parent_flag_ptr;
	elevated_old_child = *detach_node_flag_ptr;
	/*
	 * F1 / RESOLVED-POINTER CONTRACT (ft-helpers.h).  A peer mid-commit
	 * may be recompacting or re-homing the boundary (or the detach
	 * target).  It parks a flip-proxy on that slot.  Left raw, the proxy
	 * is silently misclassified as a type-7 internal node below.  That
	 * happens in ft_node_compressed, the phase-2 free walk and the
	 * shape-D ft_node_ptr.
	 * Its tag is stripped into a wild pointer, which faults frames later
	 * in cds_ft_item_to_metadata.
	 *
	 * Do NOT classify or embed a parked proxy.  Bail, so the wrapper
	 * re-descends and re-derives the slot once the peer's commit
	 * settles.  Resolving in place would be wrong.  A recompaction
	 * relocates the boundary and retires its old parent body, so
	 * detach_parent_flag_ptr can point into freed memory.
	 */
	if (caa_unlikely(ft_node_flip_proxy(iter_node_flag) ||
			ft_node_flip_proxy(elevated_old_child)))
		return -EAGAIN;
	/*
	 * ☠ AN EMPTY HOLDER SLOT IS A STALE PLAN, NOT A NODE.  The climb
	 * validated @detach_parent_flag_ptr against its level's @cur (THE
	 * PAIR THIS LEVEL WRITES THROUGH, above).  But that check and this
	 * READ are two separate instants.  The loop exited, and a peer's
	 * in-place delete can clear the slot in between.  No pair check can
	 * close that, because the validation cannot be held across the read.
	 * So the empty value has to be caught where it is actually observed.
	 * The plan expected-old below catches a republished child the same
	 * way.
	 *
	 * If NULL gets through, nothing downstream rejects it:
	 *   - Every classifier here asks the node's FLAGS.
	 *   - ft_node_compressed(NULL) and ft_node_skip_compressed(NULL) are
	 *     both false.
	 *   - So a NULL holder walks straight into the internal-node arm.
	 *   - It reaches cds_ft_item_to_metadata(NULL).  That indexes the
	 *     metadata array of the range that would contain address 0, and
	 *     FAULTS.
	 * MEASURED on release root-only, inv_concurrent_remove_all_list.  It
	 * gave SIGSEGV at 0x200010, with iter_node_flag = 0x0 in the frame.
	 * That address is the NULL page base plus the range's metadata
	 * offset.  This is the same
	 * failure the flip-proxy bail above prevents, reached by the other
	 * bad value.
	 *
	 * Nothing is built, locked or reserved yet.  So re-descend, exactly
	 * as the two guards either side of this one do.
	 */
	FT_DT_INC(ft_dt_holder_reach);
	if (caa_unlikely(!iter_node_flag)) {
		FT_DT_INC(ft_dt_holder_null_refused);
		return -EAGAIN;	/* holder gone: re-descend */
	}
	/*
	 * ☠ THE HOLDER IS THE NODE THE CLIMB ENDED AT, OR THE PLAN IS VOID.
	 * @detach_node_flag_ptr is a slot INSIDE the climb's boundary (its
	 * last @cur, metadata_stack[nr_branch - 1]).  Every write below goes
	 * through it.  But @iter_node_flag is a FRESH read of the slot above
	 * it.  A peer may have republished the boundary since (a compressed
	 * split's fresh junction, a recompaction's copy).  The read then
	 * hands back a node the slot does not live in.  Nothing downstream
	 * re-asks:
	 *   - the op locks THAT node, which is live;
	 *   - its record lands on the RETIRED boundary's slot;
	 *   - that body still holds the plan's value, so even an MW
	 *     expected-old matches.
	 * The live copy keeps naming the child this commit then retires.
	 *
	 * MEASURED (LTTng, inv_prefix_shape_zoo insert/remove_all):
	 *   - The climb condemned H, under the compressed boundary CN.  H
	 *     was a junction whose only content left was its prefix head N.
	 *   - A peer split CN into a fresh junction J still carrying H, and
	 *     committed.
	 *   - This op read J here and locked it.  It recorded
	 *     CN->child: H -> N, and retired H.
	 *   - J still named H: a tombstoned node reachable from a live one.
	 *     Every remove of N's key then spun on it (50,000 consecutive
	 *     retries).
	 *
	 * The leaf hoist below states this same test for its own arm.  The
	 * promote arm had none.  Once identity is established here, the
	 * holder's acquire keeps it, because the acquire refuses a
	 * tombstone.  Under that lock the boundary is live, so its slot is
	 * too.  Nothing is built, locked or reserved yet: re-descend.
	 *
	 * An EXTERNAL word is the same stale plan: a peer collapsed the
	 * boundary into the bare leaf it carried.  It has no metadata to
	 * compare, so refuse it first (MEASURED: 2 of 12 runs SEGV'd
	 * resolving one).  SKIP before EXTERNAL: a SKIP_X word carries its
	 * child's tag.
	 */
#ifndef FT_DEBUG_NO_HOLDER_IDENTITY
	if (caa_unlikely((!ft_node_skip_compressed(iter_node_flag) &&
				ft_node_external(iter_node_flag)) ||
			ft_flag_to_metadata(ft, iter_node_flag) !=
				metadata_stack[nr_branch - 1]))
		return -EAGAIN;	/* boundary republished: re-descend */
#endif
	/*
	 * PLAN EXPECTED-OLD, enforced (see @plan_old_child).  Everything below
	 * names the subtree the climb condemned:
	 *   - the orphan set walked from @elevated_old_child;
	 *   - the count fold;
	 *   - the drop itself.
	 * A peer that republished this slot since has put a DIFFERENT subtree
	 * here, one no level of the climb ever counted.  So the whole plan is
	 * void.  Nothing is built, locked or reserved yet: re-descend against
	 * the settled tree.
	 */
	FT_DT_INC(ft_dt_guard_reach);
	if (!elevated_old_child && !plan_old_child)
		FT_DT_INC(ft_dt_guard_both_null);
	if (elevated_old_child != plan_old_child) {
		if (!elevated_old_child)
			FT_DT_INC(ft_dt_guard_elev_null_refused);
		if (!plan_old_child)
			FT_DT_INC(ft_dt_guard_plan_null_refused);
	}
	if (caa_unlikely(elevated_old_child != plan_old_child))
		return -EAGAIN;
	/*
	 * The elevated chain from @elevated_old_child down to (excluding) the
	 * original target.  It holds:
	 *   - the keyless links the climb counted;
	 *   - the keyed junction it promoted past.  That junction always sits
	 *     at the chain's TOP, because the climb stops at the very next
	 *     level (prev_external_nodes_found).
	 * Both orphan walks below are budgeted on this.  So a promote's
	 * junction is retired with the rest, whether the target's subtree is
	 * destroyed (remove) or moved (rekey).
	 */
	nr_elevated = nr_clear + (climb_promoted ? 1 : 0);
	/* Plan-snapshot the holder slot before any recompaction overwrite. */
	holder_old_flag = iter_node_flag;

	/*
	 * Replace within parent.  If the parent is a compressed node:
	 *
	 * If topmost_external_nodes is set, the child below the
	 * compressed node has variable-length key entries.  Keep the
	 * compressed node (its path is needed for lookups) and replace
	 * cn->child with the external node directly.
	 *
	 * Otherwise, replace the compressed node with a fresh internal
	 * node (e.g. compressed root from detach/graft_swap must remain
	 * internal).
	 */
	if (ft_node_compressed(iter_node_flag) ||
	    ft_node_skip_compressed(iter_node_flag)) {
		struct cds_ft_inode_flag *to_free[FT_MAX_DEPTH];
		int nr_to_free = 0, fi;
		struct cds_ft_compressed_node *trailing_skip_cn = NULL;
		/*
		 * FOLD: ride the CALLER's commit (see the txn block below); it is
		 * also what decides who FREES the orphan set, at the bottom.
		 */
		bool fold_replace = record_only && shared_txn &&
				!fuse_cell && !run;

		/*
		 * Phase 2-style free walk for the orphaned chain below
		 * the compressed parent.  Say a compressed cn's child
		 * was an internal node N, with N->external_nodes
		 * promoted (or no external_nodes).  Then the chain
		 * [N -> ... -> external_target] is now unreachable.
		 * Walk from @elevated_old_child (the original cn->child)
		 * down the single-child no-ext chain, collecting nodes
		 * to free.  The first iteration is special: the target
		 * itself may carry residual content (external_nodes)
		 * that was promoted as topmost_external_nodes.
		 *
		 * Freeze-on-free (doc §4.B, atomic detach): COLLECT the
		 * orphaned chain HERE.  The per-node freeze-on-free
		 * tombstones are RECORDED into the replace's publish txn
		 * just below.  They flip ATOMICALLY with the commit that
		 * unlinks the chain.  So:
		 *   - a concurrent MCAS writer targeting any of these nodes
		 *     validates the mark (expected = live) and fails;
		 *   - an aborted replace leaves them live and unmarked (no
		 *     stranded dead-but-linked node).
		 * The walk is read-only on the orphan subtree.  The replace
		 * does NOT touch that subtree's internal pointers.  It
		 * rewrites the parent slot and promotes the target's
		 * external_nodes, which the phase2_first special-case
		 * tolerates.  So the set the walk builds is identical before
		 * and after the commit.  The actual free is deferred to after
		 * the commit.
		 *
		 * A MOVE-style detach has @free_detached_subtree false: the
		 * target's subtree is re-attached elsewhere, not destroyed.
		 * It still owns the ELEVATED chain above the target.  That
		 * chain is the keyed junction whose head the replace below
		 * promotes into cn->child, plus any keyless link under it.  Walk exactly
		 * @nr_elevated links for it, and stop short of the target.
		 * The plain remove walks on to the leaf.  A SKIP word stops
		 * the move's walk, because the body reads it as its child.
		 * The elided run is the chain's last link.  The trailing arm
		 * below claims it while the budget is unspent.
		 */
		if (free_detached_subtree || nr_elevated > 0) {
			struct cds_ft_inode_flag *walk_nf = elevated_old_child;
			unsigned int walk_depth = ft_child_depth_of(ft,
				iter_node_flag, cur_depth);
			bool phase2_first = true;
			/*
			 * ☞ ITS OWN DESCENT, EXTENDED PER ORPHAN, and the depth
			 * ADVANCED with it (doc/design/ft-lockset-inventory.md §2
			 * row 6).  This walk used to date every orphan at the
			 * FIRST one's depth.  It anchored each from the op's
			 * descent, whose table ends above the chain.  So an
			 * orphan past the first anchored on ITSELF, by
			 * ft_anchor_meta's on-a-level early return.  Every other
			 * op anchored it on an ancestor (88k of 200k acquires at
			 * exponential).  The phase-2 walk below already extends a
			 * copy of its own.  This one does the same, for the same
			 * reason: the op's descent stays on the KEY path for the
			 * acquires that resolve after it.
			 */
			struct ft_descent fwd;
			struct ft_lock_ctx flctx = lctx;

			if (wd_valid)
				fwd = wd;
			flctx.d = wd_valid ? &fwd : NULL;

			while (walk_nf &&
			       !ft_node_external(walk_nf) &&
			       nr_to_free < FT_MAX_DEPTH &&
			       (free_detached_subtree ||
				(nr_to_free < nr_elevated &&
				 !ft_node_skip_compressed(walk_nf)))) {
				struct cds_ft_inode_flag *next = NULL;
				unsigned int nr_child;
				struct cds_ft_node *ext_nodes;
				struct cds_ft_metadata *ometa;
				struct cds_ft_compressed_node *ocn = NULL;
				struct ft_held_anchor owalk = { 0 };
				uintptr_t osnap = 0;

				if (ft_node_compressed(walk_nf))
					ocn = ft_compressed_node_ptr(
						ft_skip_child_ptr(walk_nf));
				ometa = ocn
					? cds_ft_item_to_metadata(
						(struct cds_ft_inode *) ocn)
					: cds_ft_item_to_metadata(
						ft_node_ptr(walk_nf));
				/*
				 * §9.2 plan-lock: lock acquire the orphan BEFORE
				 * reading its nr_child for the collapse decision.
				 * Then the "retire it" verdict is derived from a
				 * FROZEN word.  The fenced tombstone's expected-old
				 * (this snap) is exactly that state.
				 *
				 * Peers that grow the orphan:
				 *   - AFTER the mark: the growth tears the fenced
				 *     expected-old and aborts our commit (detect).
				 *     In practice it cannot even reach that.
				 *     ft_meta_nr_child_inc spins on
				 *     FT_STATE_INPLACE_WAIT_MASK, which includes
				 *     LOCK unconditionally, so it WAITS for the
				 *     mark.
				 *   - BEFORE the mark: the growth is reflected in
				 *     @osnap, and stops the walk (nr_child > 1).
				 * A dirty mark (peer proxy, concurrent copier, real
				 * retire) bails to the caller's re-descend.
				 */
				if (ft->lock_fine) {
					lctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
					flctx.held.txn = lctx.held.txn;
					flctx.held.nr_extra = lctx.held.nr_extra;
					/*
					 * The copy anchors the walk only because the
					 * op's descent already ENTERED the node the
					 * walk starts under.  A cursor still ON it
					 * would leave its levels uncrossed (skeptic,
					 * 2026-09-17).
					 *
					 * ☠ THAT IS A PLAN, NOT AN INVARIANT.
					 * @walk_depth comes from the climb (key_len
					 * minus the spans it reads LIVE).  The anchors
					 * come from the descent.  A peer split or merge
					 * between the two lets the climb date the walk
					 * deeper than the descent ever went.
					 * ft_anchor_meta then answers from the wrong
					 * path.  The assert fired under rcu-debug.  A
					 * release build anchored a garbage item and
					 * faulted in ft_lock_set_order_by_anchor
					 * (two-writer owned-key harness, exponential
					 * spacing).  Re-descend, as the acquire's own
					 * refusal below does.
					 */
#ifndef FT_DEBUG_ORPHAN_WALK_UNDATED
					if (caa_unlikely(wd_valid &&
							fwd.depth < walk_depth)) {
						ret = -EAGAIN;
						goto end;
					}
#endif
					assert(!wd_valid || fwd.depth >= walk_depth);
					if (ft_detach_orphan_acquire(ft, &flctx,
								walk_nf, walk_depth,
								ometa, &owalk)) {
						ret = -EAGAIN;
						goto end;
					}
					osnap = owalk.node_snap;
					nr_child = ft_state_nr_child(osnap);
				} else
					nr_child = ft_meta_nr_child(ometa);
				ext_nodes = ometa->external_nodes;
				if (ocn) {
					next = ocn->child;
				} else if (nr_child == 1) {
					unsigned int key;

					for (key = 0; key < 256; key++) {
						next = ft_node_get_nth(ft,
							walk_nf, NULL,
							(uint8_t) key,
							FT_PF_NONE);
						if (next)
							break;
					}
				}

				/*
				 * ☠☠ A `break` HERE IS NOT A REFUSAL.  The walk
				 * stops, but everything after it runs.  The replace
				 * unlinks this whole chain from its parent, while
				 * @to_free holds only [0, k).  So orphan @k is
				 * unlinked and NEVER FREED.  It leaks, together
				 * with whatever a peer put on it.  Single-writer,
				 * the walk always reaches the leaf: every chain
				 * node passed the climb's 1-child/no-ext tests.  So
				 * this arm fires ONLY on a plan the peer has
				 * invalidated.  A stale plan must ABORT the op, not
				 * truncate its free list.
				 *
				 * Nothing is built, recorded or reserved yet.  The
				 * walk is read-only, and @orphan_txn does not
				 * exist.  So `goto end` is byte-for-byte clean.
				 * The sweep there releases every mark this op still
				 * holds.  The mismatch is a COMMITTED peer write
				 * sitting in our own locked snapshot.  So the
				 * re-descend sees a settled tree.  That is one
				 * retry per peer event, never a conflict the op
				 * manufactures for itself.
				 *
				 *
				 * ★ AND THE EXEMPTION IS NOW KEYED ON IDENTITY,
				 * not position.  But it is NOT keyed on the
				 * identity the first design assumed.  MEASURED
				 * (ft_unit, single-writer, probe at this line):
				 *   - the first orphan's own head @ext_nodes is
				 *     NULL, while @topmost_external_nodes is
				 *     NON-NULL;
				 *   - this held in every one of 24 samples, all in
				 *     branch 2.
				 * @topmost was promoted from an ancestor ABOVE this
				 * node (@prev_external_nodes_found is already set).
				 * So it is simply not THIS node's head.  Requiring
				 * @ext_nodes == @topmost therefore refuses the
				 * NORMAL shape.  It wedges ft_unit entering
				 * test_density_stress.
				 *
				 * What the climb actually believes about the first
				 * orphan is weaker.  The orphan carries either NO
				 * head of its own, or the one head the climb lifted
				 * off it.  Any other head arrived after the plan (a
				 * peer's park).  Freeing this node would take that
				 * key with it.
				 */
				/*
				 * SKEPTIC'S DISCRIMINATOR (probe only, no behaviour
				 * change).  Two facts about the FIRST orphan:
				 *   - it is exempted from the nr_child > 1 test;
				 *   - its IDENTITY is never compared with the node
				 *     the climb condemned (@elevated_old_child is
				 *     RE-READ after the climb).
				 * Count both, on the oracle that loses keys.
				 */
				if (phase2_first) {
					FT_DT_INC(ft_dt_walk_reach);
					if (nr_child > 1)
						FT_DT_INC(ft_dt_walk_first_multi);
					if (nr_branch >= 2 && ometa !=
							metadata_stack[nr_branch - 2])
						FT_DT_INC(ft_dt_walk_first_ident);
				}
				/*
				 * ☠ AND A HEAD THAT DEPARTED.  The rule above
				 * accepts a first orphan with NO head.  The
				 * promoted head usually came from an ancestor above
				 * it.  But when it came off THIS node, "no head
				 * now" means a peer REMOVED it after the climb read
				 * it, pre-lock.  Promoting it anyway republishes a
				 * removed node into the boundary slot.  MEASURED:
				 * the remove of "...zone46" resurrected a
				 * concurrently removed "...zone4" (next ==
				 * REMOVED).  Both writers then livelocked on it.
				 * Under this orphan's lock, the node it was lifted
				 * off must still carry it.
				 */
				if ((!phase2_first && (nr_child > 1 || ext_nodes)) ||
				    (phase2_first && ext_nodes &&
					    ext_nodes != topmost_external_nodes)
#ifndef FT_DEBUG_NO_DEPARTED_HEAD
				    || (ometa == topmost_src_meta &&
					    ext_nodes != topmost_external_nodes)
#endif
#ifndef FT_DEBUG_NO_FIRST_ORPHAN_COUNT
				    /*
				     * ☠ THE FIRST ORPHAN IS THE TARGET ONLY WHEN
				     * NOTHING WAS ELEVATED.  After a climb it is
				     * the chain's top: a link the climb scored
				     * single-child from an UNHELD read.  An
				     * in-place insert (FEATURE_FT_INSERT_IN_PLACE)
				     * grows it under this very lock, without
				     * retiring it.  The walk then stops (no scan
				     * past one child).  The drop retires the link
				     * with the peer's new child inside.
				     * MEASURED: inv_prefix_siblings_compressed_
				     * holder, in-place legs, "ord-cell list longer
				     * than trie".  Branch 2's phase 1 enforces the
				     * same count under its planlock.
				     */
				    || (phase2_first && nr_elevated > 0 &&
					    nr_child > 1)
#endif
				    ) {
					if (ft->lock_fine && !owalk.shared)
						ft_meta_lock_release(owalk.lock);
#ifdef FT_ENABLE_TRACING
					uatomic_inc(&ft_dbg_orphan_walk_stale);
#endif
					ret = -EAGAIN;
					goto end;
				}
				phase2_first = false;
				to_free[nr_to_free++] = walk_nf;
				if (ft->lock_fine)
					orphan_held[nr_orphan_locked++] = owalk;
				walk_depth = ft_walk_extend(&fwd, wd_valid, walk_nf,
					walk_depth, ocn ? ocn->len : 1);
				walk_nf = next;
			}
			/*
			 * As in the replace-ptr free-walk below.  A
			 * skip-compressed external leaf at the chain end keeps
			 * its path bytes in a separate skip-target compressed
			 * node.  That node is now orphaned, and the walk stops
			 * short of it.  Free it (the external leaf stays
			 * caller-owned).  For a MOVE, the skip-target is an
			 * orphan only while the elevated budget is unspent.  At
			 * the budget, the skip word encodes the target's own
			 * run, which moves with it.
			 */
			if (walk_nf && ft_node_skip_compressed(walk_nf) &&
					(free_detached_subtree ||
					 nr_to_free < nr_elevated)) {
				trailing_skip_cn =
					ft_skip_to_compressed(ft, walk_nf);
				if (ft->lock_fine) {
					struct cds_ft_metadata *tm =
						cds_ft_item_to_metadata(
							(struct cds_ft_inode *)
							trailing_skip_cn);

			/*
			 * The one-hop ft_skip_to_compressed is NOT MW-safe.
			 * A peer split/merge can tear the skip back-pointer.
			 * The resolve then recovers a node that is not this
			 * skip's target at all (type confusion).
			 * ft_reanchor_flag's header states the same hazard
			 * for the descent side.
			 *
			 * Every OTHER orphan is guarded before it is retired:
			 * - the walk breaks on nr_child > 1;
			 * - ft_detach_orphan_planlock refuses a grown one.
			 * The trailing skip-target had no such guard.  It is
			 * retired unconditionally below.
			 *
			 * A compressed node holds exactly ONE child.  A
			 * resolved target with another count is not the node
			 * this plan is about.  Measured: retiring it dropped
			 * a concurrently published 2-child subtree.  That is
			 * SILENT KEY LOSS: an insert reported OK for a key
			 * that no root descent could then find.  Bail to the
			 * op's re-descend.  A dirty mark gets the same answer
			 * one line below.
			 */
					if (ft_meta_nr_child_load(tm) != 1) {
						ret = -EAGAIN;
						goto end;
					}
					lctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
					flctx.held.txn = lctx.held.txn;
					flctx.held.nr_extra = lctx.held.nr_extra;
#ifndef FT_DEBUG_ORPHAN_WALK_UNDATED
					/* A walk the descent never dated: see above. */
					if (caa_unlikely(wd_valid &&
							fwd.depth < walk_depth)) {
						ret = -EAGAIN;
						goto end;
					}
#endif
					assert(!wd_valid || fwd.depth >= walk_depth);
					if (ft_detach_orphan_acquire(ft, &flctx,
							ft_compressed_node_flag(
								trailing_skip_cn),
								walk_depth, tm,
							&orphan_held[nr_orphan_locked])) {
						ret = -EAGAIN;
						goto end;
					}
					orphan_trailing_held =
						&orphan_held[nr_orphan_locked++];
					lctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
				}
			}
		}
		/*
		 * Atomic detach (§4.B).  Create the publish txn and size it
		 * for:
		 * - the replace's structural edges;
		 * - one freeze-on-free tombstone per collected orphan;
		 * - one more for the trailing skip-target.
		 * Record each tombstone.  The whole retired chain then
		 * freezes ATOMICALLY with the replace commit that unlinks it.
		 *
		 * create_bounded mallocs the exact cap.  OOM aborts before
		 * any reader-visible store: the orphan walk above is
		 * read-only.  If the replace fails, we destroy the txn here.
		 */
		{
			/*
			 * FOLD: ride the CALLER's commit.  This arm's own txn
			 * commits strictly earlier.  This arm publishes a
			 * SKIP_X dual.  The publish resolves the dual's home
			 * against the descriptor it is handed.  A private
			 * descriptor cannot see the caller's pending
			 * re-parent of the compressed node.  So the dual was
			 * written into the parent that the caller's commit
			 * then retired.
			 *
			 * The fold is refused when the caller brought a cell
			 * or a run: ft_remove_commit_rec's record-only arm
			 * asserts !run && !dead_cell.
			 */
			struct ft_flip_txn *orphan_txn = fold_replace ? shared_txn :
				ft_flip_txn_create_bounded(ft,
					FT_REMOVE_COMMIT_REC_MAX_EDGES
					+ 1 /* §4.B parent guard (Sites 3+4 excl.) */
					+ ft_freeze_reserve(ft, (unsigned int) nr_to_free
						+ (trailing_skip_cn ? 1 : 0))
					+ (topmost_external_nodes ? 1 : 0) /* folded external back-edge */
					+ (ft->rank_stats ? detach_depth + 1 : 0) /* nr_keys fold walk */
					+ freeze_len * FT_HLIST_FREEZE_MAX_EDGES);

			if (!orphan_txn) {
				ret = -ENOMEM;
				goto end;
			}
			/*
			 * PUBLISH THE WALK'S MARKS BEFORE THE FREEZE READS
			 * THEM.  An orphan whose anchor is its OWN word is
			 * never registered in the txn: its fused tombstone
			 * is the terminal (see ft_detach_freeze_one).  So the
			 * record's ownership witness is the held set.  The
			 * walk above left @nr_extra one short of the last
			 * acquire.
			 *
			 * The fold runs under a per-op-armed CALLER's txn.
			 * There, an unwitnessed tombstone is an abort at the
			 * record.  The plain-parent branch publishes before
			 * its freeze for the same reason.
			 */
			lctx.held.txn = orphan_txn;
			lctx.held.nr_extra = (unsigned int) nr_orphan_locked;
			for (fi = 0; fi < nr_to_free; fi++) {
				struct cds_ft_metadata *m = cds_ft_item_to_metadata(
					ft_node_compressed(to_free[fi])
						? (struct cds_ft_inode *)
						  ft_compressed_node_ptr(
							ft_skip_child_ptr(to_free[fi]))
						: ft_node_ptr(to_free[fi]));
				if (ft->lock_fine)
					ft_detach_freeze_one(orphan_txn, &lctx,
						&orphan_held[fi], m, fold_replace);
				else
					ft_flip_txn_record_tombstone(orphan_txn, m);
			}
			if (trailing_skip_cn) {
				struct cds_ft_metadata *m = cds_ft_item_to_metadata(
					(struct cds_ft_inode *) trailing_skip_cn);
				if (ft->lock_fine)
					ft_detach_freeze_one(orphan_txn, &lctx,
						orphan_trailing_held, m, fold_replace);
				else
					ft_flip_txn_record_tombstone(orphan_txn, m);
			}
			/*
			 * The removed external leaf freezes atomically with
			 * this compressed-parent replace, which unlinks its
			 * chain (doc §4.B).  The freeze is one MARK edge into
			 * @orphan_txn: the +1 reserved above.  The replace
			 * records it after the acquire that holds its chain.
			 */
			if (freeze_leaf)
				freeze_leaf_fused = true;
			/*
			 * @cur_depth dates @iter_node_flag.  The climb tracks
			 * the holder's byte-depth alongside the node itself.
			 * @iter_node_flag is that same holder, re-read from
			 * its slot.
			 */
			lctx.held.txn = orphan_txn;
			lctx.held.nr_extra = (unsigned int) nr_orphan_locked;
			ret = ft_detach_node_replace_compressed_parent(ft,
				iter_node_flag, cur_depth, &lctx,
				detach_parent_flag_ptr,
				topmost_external_nodes, elevated_old_child,
				fuse_cell,
				pub, run, orphan_txn, fold_replace,
				count_delta, freeze_leaf, freeze_len);
			if (ret) {
				/*
				 * -EAGAIN: the commit CONSUMED @orphan_txn
				 * (a peer won mid-flip).  Only a failure
				 * before the commit (-ENOMEM) leaves it live.
				 */
				if (ret != -EAGAIN && !fold_replace)
					ft_flip_txn_destroy(orphan_txn);
				goto end;
			}
		}
		/*
		 * The orphan chain is unlinked: free the set collected
		 * above.  The FOLD is the exception.  There this detach
		 * committed NOTHING: the chain is still live and linked, and
		 * the CALLER's commit is the unlink.  So hand the set out
		 * with the retired copy's lifetime, as the plain-parent
		 * branch does.  The caller reclaims it after its commit
		 * lands, and leaves it alone when that commit aborts.
		 *
		 * The trailing skip-target goes out as its PLAIN compressed
		 * flag, decoded above under the plan.  A one-hop decode of
		 * the skip word after the commit would run through a
		 * back-pointer that this very commit re-points (the promoted
		 * head's).
		 */
		if (fold_replace) {
			if (recompact_out) {
				for (fi = 0; fi < nr_to_free; fi++)
					recompact_out->orphans[fi] = to_free[fi];
				recompact_out->nr_orphans = nr_to_free;
				if (trailing_skip_cn &&
						recompact_out->nr_orphans <
							FT_MAX_DEPTH)
					recompact_out->orphans[
						recompact_out->nr_orphans++] =
						ft_compressed_node_flag(
							trailing_skip_cn);
			}
		} else {
			if (trailing_skip_cn)
				free_compressed_node(ft, trailing_skip_cn);
			for (fi = 0; fi < nr_to_free; fi++) {
				if (ft_node_compressed(to_free[fi]))
					free_compressed_node(ft,
						ft_compressed_node_ptr(
							ft_skip_child_ptr(to_free[fi])));
				else
					free_cds_ft_node(ft,
						ft_node_ptr(to_free[fi]));
			}
		}
	} else {
		struct cds_ft_inode_flag *to_free[FT_MAX_DEPTH];
		int nr_to_free = 0, fi;
		struct cds_ft_inode_flag *trailing_skip_cn_flag = NULL;

		/*
		 * Density was already propagated above (before
		 * structural changes).
		 */
		/*
		 * Freeze-on-free (doc §4.B).  COLLECT the orphaned detach
		 * subtree and tombstone every node HERE, before ANY of this
		 * branch's commits unlinks it.  Those commits are:
		 * - the shape-D ft_chain_compress_fused below;
		 * - ft_node_replace_ptr or ft_remove_one_commit further down.
		 *
		 * The walk is read-only on the orphan subtree.  None of those
		 * commits touch the subtree's internal pointers.  They
		 * rewrite the surviving boundary and its parent slot.  The
		 * detach target's own external_nodes clear / topmost promote
		 * is tolerated by the phase2_first special-case.  So the set
		 * the walk builds is identical before and after the commit.
		 *
		 * The free is deferred to after the commit.  The loop in the
		 * !ret block below consumes @to_free and
		 * @trailing_skip_cn_flag.  On an OOM abort these still-live
		 * nodes are left marked dead.  That is a no-op under one
		 * writer, and a later successful detach corrects it.
		 *
		 * Free walk semantics:
		 *
		 *   Phase 1 -- elevated ancestors.  Each is single-child by
		 *   the upward walk's own invariant.  It is keyless, or it is
		 *   the ONE keyed junction the climb promoted past, at the
		 *   chain's top.  Free @nr_elevated nodes unconditionally.
		 *
		 *   Phase 2 -- target and chain below.  A destroy-style
		 *   detach walks the target's single-child no-external chain
		 *   (descent tracking guarantees this).  The walk stops at:
		 *   - an external;
		 *   - a multi-child node;
		 *   - a node with external_nodes (its content was either
		 *     preserved as topmost_external_nodes or still
		 *     referenced);
		 *   - nr_child == 0.
		 *   A move-style detach stops at once: the target is the new
		 *   trie's root and must be preserved.
		 *
		 * Pointer classification:
		 * - ft_node_external(): an external leaf (caller-reclaimed);
		 * - ft_node_compressed(): plain and high-bit skip-compressed;
		 * - otherwise: an internal node.
		 */
		{
			struct cds_ft_inode_flag *walk_nf = elevated_old_child;
			unsigned int walk_depth = ft_child_depth_of(ft,
				iter_node_flag, cur_depth);
			/*
			 * The walk leaves the key path.  It descends the
			 * elevated chain, then the target's own chain.  So
			 * it extends a descent OF ITS OWN.
			 *
			 * Extending the op's descent would re-answer the
			 * anchor query for every lock set that resolves after
			 * it.  Those are the chain-compress fuse and both
			 * publish guards below.  Their members sit on the KEY
			 * path.  A level whose boundary node is still pending
			 * resolves to the descent's CURSOR.  The walk would
			 * have moved that cursor onto its own branch.
			 */
			struct ft_descent wwd;
			struct ft_lock_ctx wlctx;

			if (wd_valid)
				wwd = wd;
			ft_lock_ctx_init(&wlctx, wd_valid ? &wwd : NULL, NULL,
				op_ctx ? op_ctx->op : NULL);
			wlctx.held.extra = orphan_held;
			/*
			 * CHAIN to the caller's held set, as the detach's own
			 * @lctx does.  ft_lock_ctx_init NULLs .outer.  A walk
			 * that does not restore it makes every orphan acquire
			 * BLIND to the holds this op arrived with.  The
			 * acquire then reads its OWN mark as a peer's.
			 *
			 * ft_held_set_snap recurses through .outer and tests
			 * each level's .glue.  So the one link reaches the
			 * caller's txn registry, its extra array and its
			 * glue.  The rekey fold is the ctx that carries a
			 * glue.  It is also the caller whose detach ELEVATES
			 * into this walk.
			 */
			wlctx.held.outer = op_ctx ? &op_ctx->held : NULL;

			/*
			 * Phase 1: elevated ancestors.
			 *
			 * ☠ SKIP BEFORE EXTERNAL.  The body two lines down
			 * has always said so.  ft_node_external tests the LOW
			 * TAG BITS ONLY, and a SKIP_X word carries its
			 * child's tag.  Without the skip test, a skip pointer
			 * ONTO AN EXTERNAL HEAD reads as external and this
			 * loop never starts.  The body's own first arm is the
			 * skip arm.  The condition disagreed with it.
			 *
			 * What that costs is the skip-TARGET run: the node
			 * holding the path bytes between the boundary and a
			 * bare external head.  A move-style detach has no
			 * other collector for it.  (@free_detached_subtree is
			 * false there, which is what the rekey passes.)  The
			 * trailing skip-target arm is nested inside
			 * `if (free_detached_subtree)`.  ft_rekey_cow_stop
			 * owns nothing for a bare head (@s_top_meta is NULL).
			 * So nobody frees it.
			 *
			 * MEASURED with the drain oracle over the 3000-shape
			 * corpus, on a -DDEBUG_COUNTERS build:
			 * - src is a bare head under a SKIP-encoded boundary
			 *   slot: 310 of 310 seeds leak exactly one
			 *   compressed node;
			 * - src is a bare head under a plain internal
			 *   boundary slot: 0 of 197 seeds leak.
			 * The leak is 100 per cent penetrant.  Every other
			 * oracle misses it: the keys are right and
			 * cds_ft_verify is clean on all of them.  A point
			 * REMOVE of the same key is fine.  It passes
			 * @free_detached_subtree true, and the trailing arm
			 * catches it.
			 *
			 * ☞ ft-rekey.h's own descent comment names this
			 * dispatch-order trap.  It says: "a skip word's low
			 * tag bits are 0, so the external and internal
			 * predicates both read it wrong".
			 */
			while (nr_to_free < nr_elevated &&
			       walk_nf &&
			       (ft_node_skip_compressed(walk_nf) ||
				!ft_node_external(walk_nf)) &&
			       nr_to_free < FT_MAX_DEPTH) {
				struct cds_ft_inode_flag *next = NULL;
				struct cds_ft_metadata *ometa;
				bool require_sc;

				/*
				 * F1 / RESOLVED-POINTER CONTRACT, at every
				 * link.  The walk steps through raw child
				 * slots (@cn->child below, and phase 2's
				 * @ocn->child).  A peer mid-commit on the
				 * chain parks a type-7 flip proxy there.  A
				 * proxy's low nibble reads as an INTERNAL
				 * node.  Left unchecked:
				 * - it is classified as an internal node;
				 * - its tag is stripped into the latch
				 *   address;
				 * - ft_node_get_nth dispatches on
				 *   ft_types[7].
				 * That is the garbage jump named in
				 * ft_node_get_nth's own header.
				 *
				 * Bail rather than resolve, as the three
				 * plan-lock bails in these loops do.  A
				 * parked proxy means a peer commit is in
				 * flight on the chain this free set is
				 * derived from.  So the set is stale.
				 * Nothing is published yet: the to_free[]
				 * walk runs BEFORE the commit, and its nodes
				 * are freed only in the !ret block.  So the
				 * op re-descends and re-derives the set.
				 */
				if (caa_unlikely(ft_node_flip_proxy(walk_nf))) {
					ret = -EAGAIN;
					goto end;
				}

				if (ft_node_skip_compressed(walk_nf)) {
					/*
					 * Skip-encoded elevated link: the slot value
					 * encodes the child BELOW the elided skip-target
					 * compressed node (which carries the path bytes).
					 * The orphaned node is that skip-target; the child
					 * is the next link down.  Queue the target's plain
					 * compressed flag and advance to the child.
					 */
					struct cds_ft_compressed_node *cn =
						ft_skip_to_compressed(ft, walk_nf);
					/*
					 * ☠ THE ONE-HOP RESOLVE IS NOT
					 * MW-SAFE.  "Structurally
					 * single-child" was an ASSUMPTION
					 * about the node this plan MEANS.
					 * It was never a fact about the
					 * node the resolve just GOT.
					 *
					 * ft_skip_to_compressed follows the
					 * skip child's parent back-pointer.
					 * Its own header says callers "get
					 * whatever the back-pointer
					 * currently says".  A concurrent
					 * split re-parents that child onto
					 * the junction it is publishing.
					 * So the resolve recovers the
					 * PEER'S FRESH JUNCTION instead of
					 * the elided skip-target.
					 *
					 * Queueing it puts a LIVE node into
					 * the orphan set.  The set
					 * TOMBSTONES it, which kills every
					 * key beneath it.  Then the node is
					 * freed as a compressed node.
					 *
					 * MEASURED on
					 * inv_sibling_split_compress_unpinned:
					 * - the collect lands 1.7 us AFTER
					 *   the peer's publish_to_parent;
					 * - the freeze and the free follow;
					 * - the parent slot is never
					 *   republished;
					 * - the key is gone for good.
					 * ☠ Guarding the FREE does not
					 * help.  Two predicates were
					 * measured and both left the oracle
					 * red: the TOMBSTONE has already
					 * killed the key.
					 *
					 * A compressed node holds EXACTLY
					 * ONE child.  A resolved target
					 * with another count is not the
					 * node this plan is about.  This is
					 * the trailing skip-target's own
					 * rule (the guard above, added on
					 * the same measured key loss).  It
					 * is applied here, at the one site
					 * that resolves the same way and
					 * never checked it.  Bail to the
					 * op's re-descend.
					 */
					if (ft_meta_nr_child_load(
							cds_ft_item_to_metadata(
							(struct cds_ft_inode *) cn)) != 1) {
						ret = -EAGAIN;
						goto end;
					}
					wlctx.held.txn = lctx.held.txn;
					wlctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
					if (ft->lock_fine && ft_detach_orphan_planlock(
							ft, &wlctx,
							ft_compressed_node_flag(cn),
							walk_depth,
							cds_ft_item_to_metadata(
								(struct cds_ft_inode *) cn),
							false, orphan_held,
							&nr_orphan_locked)) {
						ret = -EAGAIN;
						goto end;
					}
					to_free[nr_to_free++] =
						ft_compressed_node_flag(cn);
					walk_depth = ft_walk_extend(&wwd, wd_valid,
						ft_compressed_node_flag(cn),
						walk_depth, cn->len);
					walk_nf = ft_skip_child_ptr(walk_nf);
					continue;
				}
				if (ft_node_compressed(walk_nf)) {
					ometa = cds_ft_item_to_metadata(
						(struct cds_ft_inode *)
						ft_compressed_node_ptr(walk_nf));
					require_sc = false;	/* compressed: single-child */
				} else {
					ometa = cds_ft_item_to_metadata(
						ft_node_ptr(walk_nf));
					require_sc = true;	/* elevated internal: nr_child==1 */
				}
#ifdef FT_DEBUG_ORPHAN_NEXT_BEFORE_LOCK
				next = ft_detach_walk_next(ft, walk_nf);
#endif
				wlctx.held.txn = lctx.held.txn;
				wlctx.held.nr_extra = (unsigned int) nr_orphan_locked;
				if (ft->lock_fine && ft_detach_orphan_planlock(ft,
						&wlctx, walk_nf, walk_depth, ometa,
						require_sc, orphan_held,
						&nr_orphan_locked)) {
					ret = -EAGAIN;
					goto end;
				}
				/*
				 * ☠ THE NEXT LINK IS READ UNDER THE MARK,
				 * NOT BEFORE IT.  A compressed link's count
				 * never moves: it holds exactly one child.
				 * So the plan-lock above validates nothing
				 * about WHICH child.  A peer insert of a
				 * longer key republishes @cn->child in
				 * place, under this very word.  The old
				 * value is the head this op is removing.
				 * The new value is a fresh junction that
				 * carries that head AND the peer's key.
				 *
				 * Read before the mark, the walk followed
				 * the old head, found it external and
				 * stopped.  It never looked at the junction.
				 * The drop then took the branch whole.
				 * MEASURED: the peer's key was lost within
				 * seconds, with two writers and "...runs" vs
				 * "...runs6" under one compressed node.
				 *
				 * Read after the mark, the walk reaches the
				 * junction and phase 2 refuses its head.
				 * Phase 2 and the compressed-parent walk
				 * already take the mark first.
				 */
#ifndef FT_DEBUG_ORPHAN_NEXT_BEFORE_LOCK
				next = ft_detach_walk_next(ft, walk_nf);
#endif
				/*
				 * A HEAD ON A CHAIN LINK THAT ARRIVED AFTER
				 * THE PLAN.  The keyless links carry no
				 * head.  The chain's top may carry exactly
				 * the one the climb lifted off it (the
				 * promoted junction).  Any other head is a
				 * peer's park.  Retiring the link would take
				 * that key with it.  Phase 2 makes the same
				 * refusal on its first orphan.  The head is
				 * read here under the mark taken just above:
				 * the plan-lock validates the child count
				 * alone.
				 */
				if ((ometa->external_nodes &&
						(nr_to_free != 0 ||
						 ometa->external_nodes !=
							topmost_external_nodes))
#ifndef FT_DEBUG_NO_DEPARTED_HEAD
				    /*
				     * ...and a head that DEPARTED from the
				     * link the climb lifted it off.  This is
				     * the compressed arm's rule.  This walk
				     * lacked it.  (It is the same
				     * resurrection, reached through a plain
				     * internal boundary.)
				     */
				    || (ometa == topmost_src_meta &&
					ometa->external_nodes !=
						topmost_external_nodes)
#endif
				    ) {
					ret = -EAGAIN;
					goto end;
				}
				to_free[nr_to_free++] = walk_nf;
				walk_depth = ft_walk_extend(&wwd, wd_valid, walk_nf,
					walk_depth,
					ft_node_compressed(walk_nf) ?
						ft_compressed_node_ptr(walk_nf)->len :
						1);
				walk_nf = next;
			}

			/* Phase 2: target and chain below. */
			if (free_detached_subtree) {
				bool phase2_first = true;

				/*
				 * ☠ PHASE 2 STARTS AT THE TARGET, OR NOT AT
				 * ALL.  Its first node is exempt from the
				 * nr_child > 1 test below.  That node is the
				 * target, and the detach itself is changing
				 * its count.  But nothing asked whether it
				 * IS the target.
				 *
				 * Phase 1 follows each link's child as read
				 * UNDER that link's lock.  So it walks the
				 * tree as it is NOW, not as the climb
				 * counted it.  A peer insert can split a
				 * compressed link of the chain.  The split
				 * leaves a fresh run over a fresh junction,
				 * which carries the old continuation AND
				 * the peer's key.  Phase 1 walks through
				 * it.  Without this check, the fresh
				 * junction lands here and is exempt.  It
				 * is then retired with both keys inside.
				 *
				 * MEASURED (LTTng + an orphan-count abort,
				 * inv_prefix_shape_zoo insert/remove_all):
				 * 6 of 6 runs, orphan[3] of 4, nr_child 2.
				 * cds_ft_verify then reported "ord-cell list
				 * longer than trie", with a never-removed
				 * key gone.
				 *
				 * Every destroy-style caller is a remove.
				 * Its target is the chain head the entry
				 * pinned (@freeze_leaf).  That is an
				 * EXTERNAL node, where this loop does not
				 * run.  An internal node here that is not
				 * the entry's target means the chain moved
				 * under the walk: re-descend.  Nothing of
				 * this walk is recorded yet.  Its held
				 * links release at @end.
				 */
#ifndef FT_DEBUG_NO_PHASE2_TARGET
				if (walk_nf && !ft_node_external(walk_nf) &&
						walk_nf != entry_target) {
					ret = -EAGAIN;
					goto end;
				}
#endif
#ifndef FT_DEBUG_NO_ARRIVAL_EXTERNAL
				/*
				 * ☠ AND AN EXTERNAL ARRIVAL IS NOT EXEMPT:
				 * it has to be the target too.  The
				 * exemption above assumed that a walk which
				 * reaches an external has reached THE chain
				 * head, because every link it took was a
				 * single child.  The in-place delete tier
				 * breaks that.  A peer's in-place delete
				 * leaves a link LIVE with one child fewer.
				 * So a link this climb counted as "single
				 * child = our path" is single again with a
				 * DIFFERENT child: a sibling key's.  Phase 1
				 * takes the first child under the lock and
				 * walks into it.
				 *
				 * MEASURED (delete tier, skip-compress off,
				 * inv_concurrent_remove_all_nolist, LTTng):
				 * - lane A deleted key 62 in place from a
				 *   two-child holder;
				 * - lane B, planned with key 62 there,
				 *   climbed through the holder and walked
				 *   to key 63's head;
				 * - lane B retired that head (LOST);
				 * - lane B handed key 62's chain back a
				 *   second time (DOUBLE-OWNED).
				 *
				 * With skip-compression on, the in-place
				 * delete that would leave such a holder is
				 * re-planned instead (the shape-D hoist
				 * below).  That is why only a skip-off
				 * build saw it.  Identity is by address: a
				 * parked proxy retries too.
				 */
				if (walk_nf && ft_node_external(walk_nf) &&
						ft_node_ptr_raw(walk_nf) !=
						ft_node_ptr_raw(entry_target)) {
					ret = -EAGAIN;
					goto end;
				}
#endif

				while (walk_nf &&
				       !ft_node_external(walk_nf) &&
				       nr_to_free < FT_MAX_DEPTH) {
					struct cds_ft_inode_flag *next = NULL;
					unsigned int nr_child;
					struct cds_ft_node *ext_nodes;
					struct cds_ft_metadata *ometa;
					struct cds_ft_compressed_node *ocn = NULL;
					struct ft_held_anchor owalk = { 0 };
					uintptr_t osnap = 0;

					/* Same contract as phase 1 above. */
					if (caa_unlikely(ft_node_flip_proxy(walk_nf))) {
						ret = -EAGAIN;
						goto end;
					}

					if (ft_node_compressed(walk_nf))
						ocn = ft_compressed_node_ptr(walk_nf);
					ometa = ocn
						? cds_ft_item_to_metadata(
							(struct cds_ft_inode *) ocn)
						: cds_ft_item_to_metadata(
							ft_node_ptr(walk_nf));
					/*
					 * §9.2 plan-lock (mirrors Block A).
					 * MARK before reading nr_child for
					 * the collapse decision.  The fenced
					 * tombstone's expected-old is then
					 * the exact word the "retire it"
					 * verdict rests on.
					 */
					if (ft->lock_fine) {
						wlctx.held.txn = lctx.held.txn;
						wlctx.held.nr_extra = (unsigned int)
							nr_orphan_locked;
						if (ft_detach_orphan_acquire(ft,
								&wlctx, walk_nf,
								walk_depth, ometa,
								&owalk)) {
							ret = -EAGAIN;
							goto end;
						}
						osnap = owalk.node_snap;
						nr_child = ft_state_nr_child(osnap);
					} else
						nr_child = ft_meta_nr_child(ometa);
					ext_nodes = ometa->external_nodes;
					if (ocn) {
						next = ocn->child;
					} else if (nr_child == 1) {
						unsigned int key;

						for (key = 0; key < 256; key++) {
							next = ft_node_get_nth(ft,
								walk_nf, NULL,
								(uint8_t) key,
								FT_PF_NONE);
							if (next)
								break;
						}
					}

					/* Branch 2's twin of the walk above: same
					 * belief, same identity key, same refusal.
					 */
					if ((!phase2_first &&
					     (nr_child > 1 || ext_nodes)) ||
					    (phase2_first && ext_nodes &&
						ext_nodes != topmost_external_nodes)
#ifndef FT_DEBUG_NO_DEPARTED_HEAD
					    || (ometa == topmost_src_meta &&
						ext_nodes != topmost_external_nodes)
#endif
					    ) {
						if (ft->lock_fine && !owalk.shared)
							ft_meta_lock_release(
								owalk.lock);
#ifdef FT_ENABLE_TRACING
						uatomic_inc(&ft_dbg_orphan_walk_stale);
#endif
						ret = -EAGAIN;
						goto end;
					}
					phase2_first = false;
					to_free[nr_to_free++] = walk_nf;
					if (ft->lock_fine)
						orphan_held[nr_orphan_locked++] =
							owalk;
					walk_depth = ft_walk_extend(&wwd, wd_valid,
						walk_nf, walk_depth,
						ocn ? ocn->len : 1);
					walk_nf = next;
				}
				/*
				 * The chain stops at @walk_nf.  A
				 * skip-compressed external leaf keeps its
				 * path bytes in a separate skip-target
				 * compressed node.  The walk stops short of
				 * that node.  The external leaf is
				 * caller-owned.  The skip-target is
				 * trie-owned and now orphaned.  Queue it.
				 */
				if (walk_nf && ft_node_skip_compressed(walk_nf)) {
					trailing_skip_cn_flag = walk_nf;
					if (ft->lock_fine) {
						struct cds_ft_metadata *tm =
							cds_ft_item_to_metadata(
								(struct cds_ft_inode *)
								ft_skip_to_compressed(ft,
									walk_nf));

						/* See the trailing-target guard above:
						 * the one-hop skip resolver is not
						 * MW-safe, and this target is retired
						 * unguarded otherwise. */
						if (ft_meta_nr_child_load(tm) != 1) {
							ret = -EAGAIN;
							goto end;
						}
						wlctx.held.txn = lctx.held.txn;
						wlctx.held.nr_extra = (unsigned int)
							nr_orphan_locked;
						if (ft_detach_orphan_acquire(ft,
								&wlctx, walk_nf,
								walk_depth, tm,
								&orphan_held[nr_orphan_locked])) {
							ret = -EAGAIN;
							goto end;
						}
						orphan_trailing_held =
							&orphan_held[nr_orphan_locked++];
						wlctx.held.nr_extra = (unsigned int)
							nr_orphan_locked;
					}
				}
			}
			/*
			 * The collected set (+ trailing skip-target) is
			 * tombstoned at the commit that unlinks it, not here.
			 * See the freeze below this branch's commit dispatch:
			 * - shape-D records into its own merge flip;
			 * - an in-place delete records into @commit_txn;
			 * - recompaction / list-off keep a standalone freeze
			 *   before the free.
			 * Deferred to the free walk in the !ret block below.
			 */
		}
#ifdef FEATURE_FT_SKIP_COMPRESSED
		/*
		 * Shape-D fusion.  It applies to a pure delete (no external
		 * promote) that leaves the surviving boundary a non-root
		 * 1-child no-external internal.  The fusion folds the
		 * chain-compress prune INTO the key-removal commit.  Build
		 * the merged compressed node and let it REPLACE the boundary
		 * in ONE flip.  The flip carries:
		 * - the forward;
		 * - the surviving-child re-parent;
		 * - a SKIP_X dual;
		 * - the dead cell's unsplice.
		 * This bypasses ft_node_replace_ptr entirely, so no
		 * intermediate 1-child node is ever published.  It also
		 * bypasses the standalone post-detach canonicalize below.
		 *
		 * The boundary has exactly 2 live children here, and it
		 * drops to 1.  The surviving child is the one NOT being
		 * detached, read from pre-commit state.  If the merged-node
		 * allocation fails, the whole detach aborts before any
		 * reader-visible store (the caller rolls back the count).
		 * Out of bound (merged_len > FT_SKIP_LEN_MAX) falls through
		 * to replace_ptr.
		 */
		{
			struct cds_ft_metadata *bmeta =
				cds_ft_item_to_metadata(ft_node_ptr(iter_node_flag));

			/*
			 * Proxy-resolved, for the reason spelled out at the
			 * up-walk's own snapshot.  @bmeta IS that boundary
			 * node.  A raw read here is a SECOND raw read of the
			 * very word the climb just had to resolve.  Measured
			 * on the shared-destination merge oracle's plan: 711
			 * of 749873 reads here land on a peer's parked state
			 * word.
			 *
			 * The failure is worse than the climb's, because it
			 * is silent.  A garbage count that happens to decode
			 * as 2 enters the shape-D fusion.  The scan below
			 * takes the FIRST surviving child.  It builds a
			 * merged compressed node to REPLACE the boundary.
			 * That drops every other child the node really had.
			 * No assert stands between that and a published
			 * trie.
			 */
			shape_d_plan = ft_group_skip_compressed(ft->group) &&
			    !topmost_external_nodes &&
			    ft_meta_nr_child_load(bmeta) == 2 &&
			    !bmeta->external_nodes &&
			    ft_parent_node(bmeta->parent_word) != NULL;
			if (shape_d_plan) {
				struct cds_ft_inode_flag *s_child = NULL;
				struct cds_ft_inode_flag **s_slot = NULL;
				struct cds_ft_inode_flag *fold_pending = NULL;
				uint8_t s_byte = 0;
				unsigned int b;

				for (b = 0; b < 256; b++) {
					struct cds_ft_inode_flag *c;

					if ((uint8_t) b == n)
						continue;
					c = ft_node_get_nth(ft, iter_node_flag,
						&s_slot, (uint8_t) b, FT_PF_NONE);
					if (c) {
						s_child = c;
						s_byte = (uint8_t) b;
						break;
					}
				}
				/*
				 * ☠ THE SURVIVOR SLOT MAY BE THIS TXN'S OWN
				 * PENDING FORWARD PUBLISH
				 * (@pending_pub_slot).  That slot is armed
				 * before the fold ran:
				 * - by ft_glue_set_publish on the GLUE and
				 *   merge lanes;
				 * - or directly by the NOSPLIT store commit
				 *   (ft-graft.h).
				 * The shared parent of a same-trie SIBLING
				 * move holds exactly the detached child and
				 * the destination.  @s_child is then the
				 * COMMITTED occupant.  The SAME commit
				 * retires that node and replaces it with the
				 * merged cluster.  Fusing around it strands
				 * the whole moved subtree (measured: the
				 * sibling two-child-BP pin, all four faces).
				 *
				 * Build around the PENDING value instead
				 * (@pending_child at the collapse).
				 * ft_node_recompact's copy loops make the
				 * same substitution, by identity, on this
				 * same slot.
				 */
				if (s_child && record_only && shared_txn &&
						shared_txn->pending_pub_slot &&
						s_slot == shared_txn->pending_pub_slot) {
					fold_pending = shared_txn->pending_pub_val;
					/*
					 * A COMPRESSED cluster top cannot be
					 * fused.  Absorbing it would retire a
					 * fresh node that was never published.
					 * It would also re-aim its cluster's
					 * deferred edges.  That is an ownership
					 * question the collapse does not take
					 * on.  Falling back to the recompaction
					 * would publish a 1-child internal.
					 * Skip mode's canonical form does not
					 * allow that to persist.  It is
					 * enforced at every writer-scope exit.
					 * So the SHAPE is uncovered.  Refuse
					 * the whole move, before any
					 * side-effect.  Use the rekey's own
					 * carve-out code
					 * (FT_REKEY_UNCOVERED == -EDOM).  Only
					 * the rekey fold can reach this.  BOTH
					 * armers are gated on @record_only.
					 * ft-rekey.h is its only setter.
					 */
					/*
					 * ☑ ABSORBED, not refused.  ☞ See
					 * the @pending_child contract at
					 * ft_chain_compress_fused.  A
					 * compressed pending top joins the
					 * merged run rather than sitting
					 * under it.  That is the only legal
					 * product:
					 * - fusing AROUND it makes two
					 *   adjacent compressed nodes;
					 * - the replace_ptr fallback
					 *   publishes a one-child internal.
					 * An over-long run is still
					 * refused, by the merge's own
					 * FT_SKIP_LEN_MAX arm.
					 *
					 * ☠ ...BUT ONLY WHERE THE RUN HAS
					 * AN OWNER TO LEAVE.  The absorption
					 * untracks the pending top from the
					 * glue that BUILT it and frees it
					 * unpublished.  With no glue in
					 * this frame's chain, there is
					 * nobody to untrack it from.
					 * Freeing it anyway would leave it
					 * in some other owner's hands.
					 *
					 * The glue is what armed
					 * @pending_pub_slot
					 * (ft_glue_set_publish).  So it is
					 * always present for this shape.
					 * Where it is not, the old refusal
					 * is still the only answer
					 * available.
					 */
					if ((ft_node_compressed(fold_pending) ||
							ft_node_skip_compressed(
								fold_pending)) &&
							!ft_glue_of_ctx(&lctx)) {
						ret = -EDOM;
						goto end;
					}
				} else if (s_child && record_only && shared_txn &&
						ft_chain_compress_deep_pending(
							s_child, shared_txn)) {
					/*
					 * The SAME collision ONE COMPRESSED
					 * LEVEL DEEPER.  The survivor is a
					 * compressed node whose own child
					 * slot is this txn's pending publish
					 * target.  The merged node's child
					 * is normally taken from that very
					 * slot.  Here it must come from the
					 * pending value instead.  The
					 * collapse does that substitution
					 * itself: it is the only frame that
					 * has @child_cn.  What MUST happen
					 * here, before any side-effect, is
					 * the same refusal the shallow arm
					 * makes.
					 *
					 * ☠ A COMPRESSED pending value
					 * cannot be the merged node's
					 * child.  The merged node IS
					 * compressed, and two adjacent
					 * compressed nodes are not a
					 * canonical form (cds_ft_verify
					 * rejects it).  For that reason the
					 * committed occupant this
					 * substitutes for can never be
					 * compressed.  So the substitution
					 * would be the only way to produce
					 * the shape.  Refuse the whole move
					 * terminally with the rekey's
					 * carve-out code, as the shallow
					 * arm does.  Never publish it.
					 */
					/*
					 * ☠ THE ARM IS LIVE; ONLY THIS
					 * SUB-CASE IS UNOBSERVED.  A counter
					 * at the arm fires 1 per 3000
					 * shapes (with a PLAIN pending
					 * value).  A counter at the
					 * COMPRESSED sub-case counts zero
					 * over 6000 runs.  That is evidence
					 * for the canonical-form reading.
					 * To reach the sub-case, the
					 * pending slot must be a RUN'S OWN
					 * CHILD SLOT: the dst descent ends
					 * exactly where a run ends.  The
					 * graft's product there is an
					 * INTERNAL node (a displaced head,
					 * or a recompacted branch), never
					 * a run.
					 *
					 * ☞ SO IT STAYS A GUARD, not an
					 * assert.  An assert one branch off
					 * a LIVE path buys nothing on the
					 * build that ships.  Falling
					 * through lands on
					 * `new_cn->child = pending_pub_val`:
					 * two adjacent compressed nodes,
					 * the form the absorption exists to
					 * avoid.  The shallow arm absorbs
					 * its pending top.  This one
					 * cannot: the value is the merged
					 * node's CHILD, not a run to
					 * concatenate.
					 */
					if (ft_node_compressed(
							shared_txn->pending_pub_val) ||
							ft_node_skip_compressed(
							shared_txn->pending_pub_val)) {
						ret = -EDOM;
						goto end;
					}
				}
				if (s_child) {
					/*
					 * nr_keys fold (LEAF Increment 2).
					 * Shape-D is the R3 chain-compress
					 * fuse.  The merged node is built
					 * with its post-removal count.  The
					 * -1 walk from its stable parent
					 * (publish_parent) rides the merge
					 * flip.  Reserve the walk, which is
					 * bounded by the removed leaf's
					 * depth.  This is a no-op for a
					 * count-neutral move or
					 * !rank_stats.
					 */
						lctx.held.txn = NULL;
					lctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
					/*
					 * INTENT: the boundary carries no external
					 * head (the gate above asserts it), and this
					 * commit detaches exactly the child at @n --
					 * the one the climb is pruning.  @n is the
					 * boundary's own byte for it: the survivor scan
					 * just above skips @n while scanning
					 * @iter_node_flag, which is what makes them the
					 * same node.  Naming the target is the one
					 * thing the callee cannot derive for itself.
					 *
					 * ☠ LIKE FOR LIKE.  Name it the way the callee
					 * will READ it -- through ft_node_get_nth on
					 * the boundary, the accessor the survivor scan
					 * uses -- not by dereferencing the climb's slot
					 * pointer.  A raw slot value and a get_nth
					 * result are not the same encoding, so one
					 * never compares equal to the other: measured
					 * as a PERMANENT -EAGAIN, ft_unit wedged at
					 * test 2 and the memcg SIGKILLing the leak.
					 */
					/*
					 * INTENT: the boundary carries no
					 * external head (the gate above
					 * asserts it).  This commit
					 * detaches the pair the climb
					 * condemned: @elevated_old_child at
					 * byte @n.  Both are fixed before
					 * the boundary is acquired.  Both
					 * are already enforced against the
					 * plan PRE-fence (@plan_old_child).
					 * Carrying them here moves that
					 * predicate past the acquire, where
					 * it can speak for the commit.
					 */
					const struct ft_chain_compress_intent intent = {
						.detach_child = elevated_old_child,
						.expect_ext = NULL,
						.detach_byte = n,
					};
					int cret = ft_chain_compress_fused(ft,
						iter_node_flag, cur_depth, &lctx,
						bmeta,
						s_child, s_byte,
						2 /* shape-D: survivor + the child this commit detaches */,
						fuse_cell, run,
						to_free, nr_to_free,
						trailing_skip_cn_flag,
						ft->lock_fine ? orphan_held : NULL,
						orphan_trailing_held,
						freeze_leaf, freeze_len,
						count_delta,
						ft->rank_stats ? detach_depth + 1 : 0,
				/*
				 * FOLD the collapse into the caller's txn
				 * when this detach is itself being folded.
				 * The whole move is then ONE decide.  Its
				 * retired chain and its unpublished merged
				 * node come back through @recompact_out.
				 * The caller reclaims them on the right
				 * side of its commit.
				 */
				record_only ? shared_txn : NULL, record_only,
				fold_pending, &intent,
				record_only && recompact_out ?
					&recompact_out->collapse : NULL);

					if (cret == 0) {
						ret = 0;
						boundary_fused = true;
						count_folded = true;
						if (freeze_leaf)
							freeze_leaf_fused = true;
						/*
						 * Shape-D already fused
						 * @fuse_cell's unsplice INTO
						 * this commit
						 * (ft_chain_compress_fused ->
						 * ft_remove_commit_rec).  So
						 * signal the caller.  A
						 * standalone two-commit
						 * unsplice would re-record the
						 * (now stale) head/tail
						 * neighbour edge.  That would
						 * reopen a cross-view window:
						 * a reader sees the removed
						 * key's cell as the ordered
						 * min after its structural
						 * removal.
						 *
						 * @pub is non-NULL only on the
						 * fuse_cell point-remove path.
						 * ft_remove_commit_rec itself
						 * arms the bulk @run.
						 */
						if (pub)
							pub->armed = true;
					} else if (cret < 0) {
						ret = cret;
						goto end;
					}
					/* cret > 0: out of bound -- replace_ptr below. */
				}
			}
		}
#endif
		if (!boundary_fused) {
			/*
			 * Pre-reserve the commit txn BEFORE
			 * ft_node_replace_ptr's pre-flip side-effects.  They
			 * are one of:
			 * - the in-place delete's nr_child decrement.  It
			 *   commits the deferred slot store fused with the
			 *   cell unsplice, via ft_remove_one_commit below;
			 * - the recompaction's eager re-parent of the rebuilt
			 *   node's children.  It publishes via the tail
			 *   ft_remove_commit_rec.
			 *
			 * Reserve only when that commit will be multi-edge.
			 * That is the case when:
			 * - a cell unsplice / run is present; or
			 * - the boundary's parent is compressed, so the forward
			 *   publish carries a SKIP_X dual.
			 * A lone edge (non-compressed parent, no cell) stays
			 * the infallible on-stack release store.  No txn is
			 * needed.  An in-place delete with neither is a direct
			 * lone store inside ft_node_replace_ptr.  It never
			 * reaches ft_remove_one_commit.
			 *
			 * A reservation failure aborts before any side-effect
			 * (the upward walk is read-only).  Whichever commit
			 * fires consumes the txn.  If none does (e.g. an n==1
			 * publish, or replace_ptr failed), it is freed unused
			 * at @end.
			 *
			 * @commit_txn is also the retire txn for a DEL
			 * recompaction.  When the boundary shrinks,
			 * ft_node_replace_ptr's recompact records the old
			 * copy's freeze-on-free tombstone INTO @commit_txn.
			 * The tombstone then flips atomically with the
			 * forward republish this same txn commits below
			 * (atomic detach, §4.B).  It is not an early
			 * standalone flip.
			 *
			 * FORCE-TXN: created UNCONDITIONALLY.  The former
			 * residual gate left @commit_txn NULL for a list-off
			 * shrink under a non-compressed parent with no run
			 * and rank-stats off.  That sent the DEL recompaction
			 * down ft_node_recompact's retire_txn-NULL arm.  That
			 * arm does an IMMEDIATE reparent sweep and an
			 * external-head prev store onto the not-yet-published
			 * copy.  Readers can follow both through the
			 * still-reachable children until the republish (the
			 * early live-wire class).  With the txn always
			 * present, every DEL retire records its sweep,
			 * tombstone and forward republish into ONE flip.  A
			 * txn that no commit consumes is freed unused at
			 * @end.  The new -ENOMEM aborts before any
			 * side-effect.
			 */
			if (record_only) {
				/*
				 * FOLD (coherent rekey one-decide writer).
				 * The caller's SHARED mixed txn absorbs this
				 * SIMPLE-case detach's forward-slot clear
				 * and nr_child--.  Both are MW: BP survives
				 * unlocked, and a peer conflict aborts the
				 * fold clean.  They flip atomically with the
				 * dst-attach and the S_top COW that the
				 * caller records into the same txn.  The
				 * caller runs the ONE commit and owns
				 * @commit_txn (never freed here).  The
				 * caller pre-reserved it for the whole fold,
				 * an ELEVATING detach's orphan tombstones
				 * included.
				 *
				 * An ELEVATING detach IS expressible here.
				 * Its orphan chain rides @recompact_out with
				 * the RETIRED copy's lifetime.  The caller's
				 * commit is what unlinks it, so it is freed
				 * when that commit lands.  Every tombstone
				 * freezes into this same shared txn.  So the
				 * freeze flips with the unlink, as it does
				 * when this detach commits for itself.
				 *
				 * What remains out of scope is a
				 * caller-supplied retire set or leaf freeze.
				 * The rekey fold passes neither, and each
				 * would need a reclaim owner of its own.
				 */
				assert(!retire_glue && !freeze_leaf);
				commit_txn = shared_txn;
			} else {
				commit_txn = ft_flip_txn_create_bounded(ft,
					FT_REMOVE_COMMIT_REC_MAX_EDGES
					+ FT_CELL_LOCKSET_MAX_RECORDS
					+ 1 /* §4.B parent guard (Site 1 arms excl.) */
					+ ft_freeze_reserve(ft, (unsigned int) nr_to_free
						+ (trailing_skip_cn_flag ? 1 : 0))
					+ (retire_glue ? retire_glue->cap_free : 0)
					+ (ft->rank_stats ? detach_depth + 1 : 0) /* nr_keys fold walk */
					+ freeze_len * FT_HLIST_FREEZE_MAX_EDGES);
				if (!commit_txn) {
					ret = -ENOMEM;
					goto end;
				}
			}
#ifdef FT_REMOVE_CLAIM
			/*
			 * §4 STEP B2's DRY RUN, the tool 658989ef was for the
			 * rekey writer.  Point B0's owner assert at the
			 * remove commit_rec.  Every record it plants without
			 * owning is then named, at an abort.  The build is
			 * otherwise byte-identical to the unarmed one.
			 *
			 * ☞ CLAIMED HERE, WHICH IS EARLIER THAN THE ARM WOULD
			 * BE.  A dry run has to claim BEFORE the records it
			 * wants checked.  For this site that is before its
			 * acquires finish.  So read a miss FIRST as "this
			 * record is planted before its owner is registered".
			 * That ORDERING class has been the answer more often
			 * than a missing acquire.
			 *
			 * ☠ And read a miss as "the registry cannot SEE this
			 * hold" before "the op does not HOLD it".  Where a
			 * caller's SWEEP owns the mark's clearing, the
			 * absence is by design (ft_flip_txn_owns).
			 */
			ft_flip_txn_claim_per_op_armable(ft, commit_txn);
#endif
#ifdef FEATURE_FT_PROBE_PROMOTE
			/*
			 * §4.B unguarded-promote probe.  It sits at the CALL
			 * SITE, not inside ft_popcount_node_replace_ptr.
			 * That function returns from its `if (pub)` branch
			 * before any counter placed within it.  So an inside
			 * counter reads 0 for BOTH variants.  The arm then
			 * looks dead when only the unguarded one is.
			 */
			if (topmost_external_nodes) {
				if (pub)
					FT_PROMOTE_PROBE_INC(cds_ft_probe_promote_deferred);
				else
					FT_PROMOTE_PROBE_INC(cds_ft_probe_promote_immediate);
			}
#endif
			lctx.held.txn = commit_txn;
			lctx.held.nr_extra = (unsigned int) nr_orphan_locked;
			/*
			 * RE-DERIVE the hint when the climb MOVED.  The
			 * caller's hint names the detach target's junction.
			 * After an elevation the recompacted node is an
			 * ANCESTOR of that junction.  Its own parent is
			 * @boundary_parent_nf, with @detach_parent_flag_ptr
			 * as its slot.  Both are re-derived by the same walk
			 * that moved, which is the only frame that can know
			 * them.  Left stale, the @parent_guard read-set term
			 * validates the recompacted node against a parent
			 * that is not its own.  The acquire's commit then
			 * ABORTS on every attempt, and the caller's -EAGAIN
			 * loop re-derives the identical plan forever.
			 *
			 * @parent_held is FALSE and @gp / @gp_slot are NULL:
			 * the caller's answers were about ITS junction and
			 * say nothing about this one.  A word this op does
			 * happen to hold is still handled: the acquire
			 * DEDUPES against the held set rather than refusing.
			 * A COMPRESSED parent is refused above.  Its SKIP_X
			 * dual would need a great-grandparent this frame has
			 * not derived.
			 */
			/*
			 * ☑ A COMPRESSED LANDING PARENT IS NOT, BY ITSELF, A
			 * SHAPE THIS FRAME CANNOT EXPRESS.  The refusal that
			 * said so was VACUOUS for two thirds of what it
			 * caught.
			 *
			 * It read: "a COMPRESSED landing parent carries a
			 * SKIP_X dual the recompact re-encodes into its OWN
			 * parent's slot, and this walk has not derived that
			 * great-grandparent pair."  Every clause is true and
			 * none of them bites:
			 * - the recompact takes GP only when the hint's @gp
			 *   is non-NULL;
			 * - a DEL never re-encodes the dual itself;
			 * - the detach's own republish records the dual from
			 *   @cn_meta as MW (@dual_owner_held false, the arm
			 *   below).
			 * ☞ THE UN-CLIMBED REKEY PATH IS THE SHIPPED
			 * PRECEDENT.  ft-rekey.h's own elevated hint passes
			 * `.gp = NULL, .gp_slot = NULL` under a compressed
			 * parent, and has done all along.
			 *
			 * MEASURED by ablating it.  It refused 26 shapes on
			 * -DNO_FEATURE_FT_SKIP_COMPRESSED, and 19 of them are
			 * then SERVED (4 of 5 on the default build).
			 * cds_ft_verify is CLEAN and the keys are right, on
			 * every rank x list arm, on --enable-rcu-debug and on
			 * -DFT_REKEY_CLAIM.  This includes the shapes where
			 * the great-grandparent is an INTERNAL node, which
			 * the 3000-shape corpus cannot even produce.
			 *
			 * ☠ WHAT IT WAS REALLY CATCHING is one sub-family and
			 * nothing else: the resting node's run parent is the
			 * very node the CALLER's graft SPLIT.  That is the
			 * collapse-vs-split collision which the arm at the
			 * top of this function refuses, with a second exit.
			 * Ablate this one on that sub-family and the engine
			 * aborts on `r->kind == kind` (rcu-debug), or the op
			 * livelocks (noskip).  So the predicate is narrowed
			 * to the thing that is actually red.  It stays
			 * terminal: the shape is deterministic, so -EAGAIN
			 * would spin.
			 */
			/*
			 * ☠ @split_cn IS THE COMPRESSED NODE POINTER, NOT THE
			 * FLAG.  The arm at the top of this function passes
			 * @parent_cn for the same lookup.  Handing it the
			 * flag compares a tagged word against an untagged
			 * one.  That matches nothing, and lets through the
			 * one shape this guard exists for.  MEASURED: the
			 * engine then aborts on `r->kind == kind` at seed
			 * 2919.
			 */
			if (climbed && src_held_hint && boundary_parent_nf &&
					!topmost_external_nodes &&
					ft_node_compressed(boundary_parent_nf) &&
					ft_glue_that_split(&lctx,
						ft_compressed_node_ptr(
							boundary_parent_nf)) != NULL) {
				ret = -EDOM;
				goto end;
			}
			if (climbed && src_held_hint) {
				elevated_hint = (struct ft_parent_hint){
					.parent = boundary_parent_nf,
					.slot = detach_parent_flag_ptr,
					.gp = NULL, .gp_slot = NULL,
					.parent_held = false,
					.parent_guard = src_held_hint->parent_guard };
				replace_hint = &elevated_hint;
			}
			/*
			 * THE LEAF-DELETE HOIST: LOCK THE HOLDER BEFORE THE
			 * PLAN IS READ (doc/design/ft-lockset-inventory.md
			 * §1, row 1).
			 *
			 * ft_node_replace_ptr's in-place arm decides "stay in
			 * place" from the holder's nr_child.  It defers the
			 * slot clear plus the fused nr_child-- into @pub.
			 * Until now this op held NOTHING at three points:
			 * - when it read that count;
			 * - when it recorded those two words;
			 * - when it committed them.
			 * That is 2.6M commits per ft_inv run, arbitrated
			 * only by the fused CAS.  The promote arm below
			 * acquires the holder, but AFTER the plan.  The leaf
			 * arm never did.
			 *
			 * So take the holder here, ahead of the decision: its
			 * own word at per-node, its ANCHOR above.  The insert
			 * tier's hoist takes the attach node ahead of its
			 * reserve in the same way.  The count replace_ptr
			 * reads is then the count under the lock.  The
			 * freeze_leaf chain word recorded below is under its
			 * nearest ancestor lock.
			 *
			 * The lock is registered in @commit_txn with its
			 * {LOCK|s -> s} release, so every exit reaches a
			 * terminal:
			 * - the commit consumes it;
			 * - @end's destroy of an unused txn releases it.
			 * If replace_ptr routes to a DEL recompaction
			 * instead, its {C,P,GP} acquire finds C held through
			 * @lctx.  It dedupes rather than refusing its own
			 * mark.
			 *
			 * Scope: the point removes on a SHARED trie.  The
			 * other cases are out:
			 * - an exclusive trie has no peer to exclude;
			 * - the bulk callers pass the exclusive-only tier;
			 * - the FOLD's @shared_txn belongs to its caller.
			 * A promote keeps its own acquire below.
			 *
			 * ☞ LOCKS FIRST, SW LATER.  @leaf_hoisted keeps the
			 * B2 arm below from converting this commit's records
			 * to SW.  The transition flips MW to SW once, when
			 * every writer of these words holds its lock.  It
			 * does not flip one producer at a time.
			 */
			if (ft->lock_fine && !ft->exclusive && in_place &&
					!record_only && commit_txn &&
					!topmost_external_nodes) {
				enum ft_lock_or_guard_exit hex;

				/*
				 * LOCK THE HOLDER THE PLAN NAMES.
				 * @iter_node_flag is RE-READ from the parent
				 * slot after the climb.  @metadata_stack is
				 * what the climb derived.  A peer can
				 * republish that slot in between (a
				 * recompaction of the holder into a copy).
				 * The two then name DIFFERENT nodes.
				 * MEASURED 1,819 times per ft_inv run at
				 * per-node, climb or no climb.
				 *
				 * The pre-hoist code carried that torn plan
				 * to its commit and aborted there.  Locking
				 * @iter_node_flag would instead hold the
				 * COPY while the records name the planned
				 * node.  So refuse the torn plan before the
				 * acquire, and re-check the slot under the
				 * lock.
				 */
				if (ft_flag_to_metadata(ft, iter_node_flag) !=
						metadata_stack[nr_branch - 1]) {
					ret = -EAGAIN;
					goto end;
				}
				FT_INTERLEAVE(FT_IL_LEAF_HOIST_PRE);
				lctx.held.txn = commit_txn;
				lctx.held.nr_extra = (unsigned int) nr_orphan_locked;
				ft_flip_txn_lock_or_guard_parent_ex(__func__,
					__LINE__, ft, commit_txn, &lctx,
					iter_node_flag, cur_depth, &hex);
#ifdef FT_DEBUG_STRUCT_ANCHOR
				ft_sa_hoist_exit_count(1, hex);
#endif
				if (hex == FT_LOG_EXIT_MISS) {
					ret = -EAGAIN;
					goto end;
				}
				leaf_hoisted = true;
				/*
				 * Under the lock the holder can no longer be retired
				 * or relocated, so a slot that still names it names a
				 * LIVE, LINKED holder.  A registered lock is released
				 * by @end's destroy of the unused @commit_txn.
				 */
				if (rcu_dereference(*detach_parent_flag_ptr) !=
						iter_node_flag) {
					ret = -EAGAIN;
					goto end;
				}
#if defined(FEATURE_FT_SKIP_COMPRESSED) && !defined(FT_DEBUG_INPLACE_DELETE_RESIDUE)
				/*
				 * THE IN-PLACE TWIN OF THE RECOMPACTION
				 * RE-PLAN below.  This hoist only runs for a
				 * pure delete.  An in-place delete drops the
				 * boundary's child count at its commit, with
				 * no copy for that check to inspect.
				 *
				 * The shape-D test above is a PLAN read.  A
				 * peer may since have removed the boundary's
				 * prefix key (or a third child).  Under the
				 * lock we now hold, that leaves a keyless
				 * 2-child node.  This delete turns it into
				 * the 1-child keyless internal that skip
				 * mode refuses.  Measured on the in-place
				 * build's owned-key row: 5 of 96 runs left
				 * one at root-only and exponential spacing,
				 * 0 of 96 with this re-plan.
				 *
				 * Nothing is recorded yet and the lock rides
				 * @commit_txn, so re-plan.  The next plan
				 * reads the node keyless and collapses it.
				 * As below, re-plan only where this plan did
				 * NOT choose the collapse.  An out-of-bound
				 * shape-D then cannot send it round forever.
				 */
				if (!shape_d_plan &&
				    ft_group_skip_compressed(ft->group)) {
					struct cds_ft_metadata *bm =
						metadata_stack[nr_branch - 1];

					if (ft_meta_nr_child_load(bm) == 2 &&
					    !bm->external_nodes &&
					    ft_parent_node(bm->parent_word) != NULL) {
						ret = -EAGAIN;
						goto end;
					}
				}
#endif
				FT_INTERLEAVE(FT_IL_LEAF_HOIST_POST);
			}
			ret = ft_node_replace_ptr(ft,
				detach_node_flag_ptr,
				elevated_old_child,
				&iter_node_flag,
				&old_recompacted_node,
				metadata_stack[nr_branch - 1],
				n, (struct cds_ft_inode_flag *) topmost_external_nodes,
				in_place,
				detach_parent_flag_ptr == &ft->root,
				cur_depth, pub, commit_txn, replace_hint,
				&lctx);
		}
#if defined(FEATURE_FT_SKIP_COMPRESSED) && !defined(FT_DEBUG_RECOMPACT_RESIDUE)
		/*
		 * A COPY THAT SHOULD HAVE BEEN A COLLAPSE.  The shape-D test
		 * above is a PLAN read.  It saw @iter_node_flag keep its
		 * prefix key (or a third child).  So removing one child left
		 * a legal keyed node and no collapse was owed.
		 *
		 * A peer that removes that prefix key between the plan and
		 * the recompaction's lock changes the answer.  The copy the
		 * recompaction just built from the LOCKED state has one
		 * child and no key.  That is the 1-child keyless internal
		 * which cds_ft_verify refuses in skip mode.  A later remove
		 * below it cannot date its plan on such a node: it livelocks
		 * at exponential spacing.  Measured: 143 such copies in 24
		 * owned-key runs, every one planned with nr_child 2 and a
		 * key.
		 *
		 * Nothing is committed yet: the copy is unpublished, and its
		 * re-parent sweep and retire ride @commit_txn.  So re-plan.
		 * The next attempt sees the node without its key and
		 * collapses it.
		 *
		 * Two limits apply:
		 * - re-plan only where this plan did NOT choose the
		 *   collapse.  A shape-D that refuses as out of bound then
		 *   cannot send it round forever;
		 * - do not re-plan for a bulk fold (@record_only), which no
		 *   peer can race.
		 */
		if (!ret && !boundary_fused && !shape_d_plan && !record_only &&
		    old_recompacted_node && !topmost_external_nodes &&
		    ft_group_skip_compressed(ft->group) &&
		    !ft_node_external(iter_node_flag) &&
		    !ft_node_compressed(iter_node_flag) &&
		    !ft_node_skip_compressed(iter_node_flag)) {
			struct cds_ft_metadata *nm = cds_ft_item_to_metadata(
				ft_node_ptr(iter_node_flag));

			if (ft_meta_nr_child_load(nm) == 1 &&
			    !nm->external_nodes &&
			    ft_parent_node(nm->parent_word) != NULL)
				ret = -EAGAIN;
		}
#endif
		if (!ret) {
			/*
			 * Freeze the collected orphan chain (+ trailing
			 * skip-target) before the free below (doc §4.B,
			 * atomic detach).  replace_ptr has run.  Record the
			 * tombstones INTO the commit that unlinks the chain:
			 * the free is now deferred past ALL of them (below
			 * the republish block).
			 *
			 * pub != NULL always reserves and consumes
			 * @commit_txn, through one of:
			 * - ft_remove_one_commit for an in-place delete;
			 * - the recompaction republish in the parent-slot
			 *   block.
			 * So the freeze flips ATOMICALLY with that unlink,
			 * and an aborted commit discards it.
			 *
			 * The list-off pub-less path does a direct lone store
			 * that never touches @commit_txn.  It keeps a
			 * standalone freeze (NULL txn), which is a no-op
			 * under one writer.  Shape-D already recorded into
			 * its own merge flip.
			 */
			if (!boundary_fused && (nr_to_free > 0 || trailing_skip_cn_flag))
				ft_detach_freeze_orphans(ft,
					(pub && commit_txn) ? commit_txn : NULL,
					&lctx,
					to_free, nr_to_free, trailing_skip_cn_flag,
					ft->lock_fine ? orphan_held : NULL,
					orphan_trailing_held,
					/* @commit_txn IS @shared_txn then */
					record_only);
			/*
			 * @retire_glue is a caller-supplied external retire
			 * set: the merge src-side glue's overlap-spine
			 * free-list.  The caller frees it AFTER this detach
			 * returns.  It freezes atomically with the SAME
			 * unlink that makes it unreachable.
			 *
			 * A LIST-ON src detach rides @commit_txn here, on the
			 * branch-2 commit_txn path.  Empirically that is 100%
			 * of merge stress: branch 2, pub != NULL, commit_txn
			 * present.  It is recorded AFTER replace_ptr, so a
			 * recompaction's own DEL tombstone is already in
			 * @commit_txn.  The order among tombstones is
			 * irrelevant: all commit together.
			 *
			 * A LIST-OFF src detach (pub == NULL) leaves it for
			 * the standalone fallback at @end.  So do shape-D and
			 * a compressed parent (a no-op under one writer).
			 */
			if (!boundary_fused && retire_glue && pub && commit_txn) {
				retire_glue->txn = commit_txn;
				retire_glue->fuse_free_list = true;
				ft_glue_tombstone_free_list(retire_glue);
				retire_glue_fused = true;
			}
			/*
			 * The removed external leaf (single-entry chain,
			 * node->next == NULL) freezes atomically with the
			 * SAME unlink.  It is recorded into @commit_txn
			 * beside the orphan tombstones (the +1 reserved
			 * above).  That happens when the in-place /
			 * recompaction commit consumes the txn (pub &&
			 * commit_txn).
			 *
			 * The list-off pub-less direct-store path (commit_txn
			 * NULL) keeps the standalone fallback at @end, a
			 * no-op under one writer.  Shape-D already froze the
			 * leaf in its merge flip.
			 */
			if (!boundary_fused && freeze_leaf && pub && commit_txn) {
#ifdef FT_DEBUG_CHAIN_HOLD
				/*
				 * THE HEAD-WORD CONTROL.  The removed leaf is a
				 * LIVE head until this commit.  So
				 * ft_ch_head_reachable must read it reachable.
				 * Otherwise its HIDDEN zeros are blind.
				 */
				if (ft->lock_fine && !ft->exclusive &&
						ft_wlock_held != ft)
					ft_ch_head_ctl_count(ft_ch_head_reachable(ft,
						freeze_leaf));
#endif
				ft_ch_audit_ctx(ft, commit_txn, &lctx, freeze_leaf);
				/*
				 * ★ VALIDATE THE PLAN UNDER THE LOCK
				 * (Mathieu, 2026-09-19).  @freeze_len
				 * reached this op as a LITERAL:
				 * cds_ft_remove's "1 = this key's SOLE
				 * entry".  It was derived from a read of
				 * @succ_node taken with NOTHING HELD.  The
				 * chain's holder is not acquired until
				 * ft_node_replace_ptr above.  So a same-key
				 * insert can append a duplicate in that
				 * window.  The freeze's derived NULL has
				 * been standing in for the exclusion the
				 * count did not have.  MEASURED: 10,157
				 * (per-node) / 16,219 (exponential)
				 * disagreements per ft_inv leg.  The txn
				 * registry owned the holder at 100% of them.
				 *
				 * We hold it HERE, so ask the word.  A stale
				 * plan is refused before any record is
				 * filed.  That is the SAME decision the
				 * commit's install CAS makes today.  It is
				 * taken earlier, and mostly without needing
				 * the CAS to make it.
				 *
				 * ☠ NOT "instead of" the CAS: see
				 * ft_hlist_chain_plan_ok's header.  An
				 * UNDECIDED peer proxy that outlasts
				 * urcu_txn_read's patience window still
				 * reads as NULL here.  So the install CAS
				 * remains the backstop until the lock set is
				 * complete.  This shrinks the window the CAS
				 * arbitrates.  It does not on its own make
				 * the record parkable.
				 *
				 * ☠ RETRIABLE, AND IT TERMINATES.  The
				 * caller re-derives and finds the longer
				 * chain.  The next attempt takes the
				 * promote/unchain lane instead of this
				 * prune.  It does not re-enter the same
				 * refusal.
				 *
				 * ☠ AND NOTHING IS READER-VISIBLE YET.  On
				 * this arm, @pub and @commit_txn are both
				 * present.  The forward store, the head back
				 * edge and the recompaction republish are
				 * all RECORDED into @commit_txn.
				 * ft_remove_one_commit does not reach that
				 * txn until below.  The lone bare
				 * rcu_assign_pointer of the back edge is the
				 * @commit_txn-NULL arm, which cannot be this
				 * one.  So @end's sweep returns the
				 * structure byte-for-byte to its pre-op
				 * state, as for the leaf hoist's two -EAGAIN
				 * exits above.
				 */
				if (!ft_hlist_chain_plan_ok(ft,
						ft_flip_txn_handle(commit_txn),
						freeze_leaf, freeze_len)) {
					FT_HLIST_PLAN_BAIL();
					FT_DBG_RETRY_SITE();
					/*
					 * ☠ THE ERRNO IS LOAD-BEARING, not
					 * just a status.  @end's
					 * recompaction arm keys on it
					 * EXACTLY:
					 * - `ret == -EAGAIN` frees the
					 *   UNPUBLISHED fresh copy;
					 * - anything else call_rcu-frees
					 *   @old_recompacted_node, the OLD,
					 *   still-linked node.
					 * -ENOENT is already a legal detach
					 * return elsewhere.  So a later
					 * edit of this bail to any other
					 * negative value is a
					 * use-after-free one token away.
					 */
					ret = -EAGAIN;
					goto end;
				}
				ft_hlist_freeze_chain_prepare_checked(ft,
					ft_flip_txn_handle(commit_txn),
					freeze_leaf, freeze_len);
				freeze_leaf_fused = true;
			}
			/*
			 * In-place key-disappearing remove.  The holder
			 * stayed above min_child, so replace_ptr deferred its
			 * single reader-visible forward store into @pub
			 * instead of doing it.  Commit that store, @old_val
			 * -> @new_val, fused with @fuse_cell's ordered-list
			 * unsplice in ONE flip.  @new_val is:
			 * - NULL for a leaf delete;
			 * - the promoted external chain head for an external
			 *   promote.
			 * A pigeon delete leaves its occupancy bit set.  The
			 * bit is a sticky soft-delete hint, and recompact
			 * rebuilds it clean.  So nothing settles after the
			 * flip.  Recompaction (pub unarmed) published its
			 * rebuilt node itself and stays two-commit: the
			 * caller unsplices.
			 */
			if (!boundary_fused && pub && pub->armed) {
#ifdef FEATURE_FT_PROBE_PROMOTE
				/*
				 * Did the §4.B acquire actually run for a
				 * PROMOTE?  `pub != NULL` is only a proxy:
				 * the guard needs pub->armed too.  A promote
				 * that left @pub unarmed would store without
				 * the acquire, as the pub-less path would.
				 * Count the consequence, not the proxy.
				 */
				if (topmost_external_nodes)
					FT_PROMOTE_PROBE_INC(cds_ft_probe_promote_guarded);
#endif
				/*
				 * nr_keys fold (LEAF Increment 2).  The
				 * in-place delete / external promote leaves
				 * the holder @iter_node_flag in place: its
				 * subtree loses the disappearing key.  So
				 * the -1 walk from @iter_node_flag up to
				 * root rides THIS commit atomically.  It is
				 * recorded before ft_remove_one_commit
				 * consumes @commit_txn.  It is a no-op, or
				 * skipped, for a count-neutral move or
				 * !rank_stats.
				 */
				if (count_delta && commit_txn) {
					ft_flip_txn_record_count_parent(ft, commit_txn,
						iter_node_flag, count_delta);
					count_folded = true;
				}
				/*
				 * External promote: the promoted head's
				 * back-channel re-parent (captured by
				 * replace_ptr into @pub) rides THIS commit.
				 * An ABORT then discards it with the forward
				 * flip.  The @commit_txn-NULL lone-edge
				 * boundary keeps the pre-fusion timing:
				 * apply it just before the lone forward
				 * store (single writer, cannot abort).
				 */
				/*
				 * ☞ THE BACK-EDGE CLAIM READS ZERO ON EVERY
				 * OWNER COLUMN HERE, AND THE OP DOES HOLD
				 * THE OWNER.
				 *
				 * FT_BE_DETACH_UNCHAIN is the one site where
				 * all three report "holds nothing":
				 * - the txn registry;
				 * - the per-thread hold ledger;
				 * - the CALLER's @op_ctx.
				 * MEASURED on a SINGLE-writer run: the
				 * owner's state word carries FT_STATE_LOCK
				 * (0x80004, tombstone clear) in 5/5 samples.
				 * @lctx, THIS function's own lock context,
				 * answers held in 5/5.
				 *
				 * The marks live in @orphan_held.  That is a
				 * fn-scope array, not the txn locks[]
				 * registry.  The orphan chain reaches
				 * FT_MAX_DEPTH, which is past
				 * FT_FLIP_TXN_MAX_LOCKS.  See the DLM orphan
				 * plan-lock note at the top.  So they are
				 * the `extra[]` arm of
				 * the held set.  By construction they are
				 * reachable from @lctx, and invisible to
				 * @op_ctx and to the registry.  Do not read
				 * the claim's zeros here as an unowned
				 * write.  Ask @lctx.
				 */
				if (pub->head_parent_field) {
					if (commit_txn) {
						ft_flip_txn_record_head_back_edge_owned(
							commit_txn,
							(void **) pub->head_parent_field,
							pub->head_parent_old,
							pub->head_parent_new,
							ft_back_edge_owner(
								pub->head_parent_old)
							FT_BE_SITE(FT_BE_DETACH_UNCHAIN, &lctx));
					} else {
						rcu_assign_pointer(*pub->head_parent_field,
							pub->head_parent_new);
						FT_WIN_NOTE_RAW(pub->head_parent_field,
							pub->head_parent_new, 1);
					}
				}
				/*
				 * §4.B VALIDATE (Phase 4.3, MW).
				 * The EXTERNAL-PROMOTE in-place
				 * commit stores pub->slot INSIDE the
				 * live holder @iter_node_flag.  But
				 * it records NO state edge on it.
				 * pub->state_meta is NULL for a
				 * promote.  It is set only for a
				 * pure leaf delete, which fuses a
				 * real nr_child-- that self-guards
				 * the holder.
				 *
				 * The forward CAS below validates
				 * only the slot VALUE (the displaced
				 * old child).  A peer can recompact
				 * or freeze the holder through its
				 * grandparent slot.  That leaves the
				 * value intact in the retired copy.
				 * So the CAS still matches, and the
				 * promoted head is published into a
				 * reclaimed node (UAF).
				 *
				 * So guard the holder's state word.
				 * Such a peer's freeze-on-free then
				 * ABORTs this commit.  This is
				 * symmetric with the external-CLEAR
				 * guards (2547, 3071).  It reuses
				 * the "+1 §4.B parent guard"
				 * reservation on @commit_txn.  It is
				 * mutually exclusive with the
				 * recompaction republish guard,
				 * which fires only on the
				 * pub-UNARMED path.
				 */
				if (commit_txn && !pub->state_meta) {
					lctx.held.txn = commit_txn;
					lctx.held.nr_extra =
						(unsigned int) nr_orphan_locked;
					ft_flip_txn_lock_or_guard_parent(ft,
						commit_txn, &lctx,
						iter_node_flag, cur_depth);
					/*
					 * ☠ A MISSED HOLDER ENDS THE ATTEMPT
					 * HERE.  A peer holds
					 * @iter_node_flag's word (a parked
					 * proxy on it counts).  So the take
					 * latched @acquire_miss, and
					 * @commit_txn is doomed.  Its commit
					 * would discard everything.
					 *
					 * Carrying on would arm the per-op
					 * claim.  It would record the
					 * promote on @pub->slot, a word this
					 * op does not own.  The rcu-debug
					 * owner assert catches that.
					 * MEASURED on:
					 * - inv_prefix_shape_zoo, in-place
					 *   rcu-debug at exponential,
					 *   remove_all's detach;
					 * - 2 of 128 pinned runs.
					 * The dump showed the holder's state
					 * word carrying a peer's proxy.
					 *
					 * Nothing is committed.  @end
					 * destroys the unused txn and
					 * releases what it registered.
					 * insert_replace's and remove_all's
					 * arms do the same (dbc41961,
					 * da4ac288).
					 */
#ifndef FT_DEBUG_DETACH_MISS_CARRIES_ON
					if (caa_unlikely(commit_txn->acquire_miss)) {
						ret = -EAGAIN;
						goto end;
					}
#endif
				}
				/*
				 * PHASE B, STEP B2 -- THE ARM.  The
				 * guard/release above is this path's LAST
				 * ft_flip_txn_lock_register.
				 * ft_remove_one_commit below plants the
				 * first record after it.  So
				 * ft_flip_txn_arm_per_op's "after the op's
				 * last register" contract puts the arm here.
				 * The helper applies the lock_fine,
				 * per-node-spacing and empty-registry
				 * refusals itself.
				 *
				 * ☞ WHAT IT CONVERTS IS THE TAIL.  That is a
				 * property of this op, not a gap.
				 * ft_detach_node interleaves registers and
				 * records.  The orphan freezes and the
				 * recompact/collapse edges are planted
				 * EARLIER on this same txn.  They stay MW,
				 * which is stricter and always sound.
				 * Converting them needs their own arm
				 * points.  Each must sit after its own last
				 * register.  That is a later step, not
				 * something this one leaves half-done.
				 *
				 * ☞ The dry run (-DFT_REMOVE_CLAIM) claims at
				 * txn CREATION, strictly earlier than here.
				 * It is clean on both suites.  So every
				 * record on this txn already passes the
				 * owner check, not only the ones this arm
				 * converts.
				 */
				if (commit_txn && !leaf_hoisted)
					ft_flip_txn_arm_per_op(ft, commit_txn);
#ifdef FT_DEBUG_DEL_TOMB
				/*
				 * PROBE: name the two derivations when the slot this
				 * delete clears is not inside the node whose count it
				 * decrements.
				 */
				if (pub->state_meta) {
					char *sn = (char *) cds_ft_metadata_to_item(
						pub->state_meta);
					char *sl = (char *) pub->slot;
					static unsigned long ft_dt_split_ctx;

					if ((sl < sn || sl >= sn + ((size_t) 1 <<
							cds_ft_item_order(sn))) &&
							uatomic_add_return(&ft_dt_split_ctx, 1) <= 8) {
						unsigned int di;

						fprintf(stderr, "FT DEL-SPLIT-CTX slot=%p state_node=%p nr_branch=%d nr_clear=%d nr_metadata=%d climbed=%d climb_promoted=%d entry_holder_slot=%p entry_holder_raw=%p entry_slot_now=%p holder_old_flag=%p detach_node_flag_ptr=%p detach_parent_flag_ptr=%p iter_node_flag=%p elevated_old_child=%p plan_old_child=%p topmost_ext=%p arg_detach_slot=%p arg_parent_slot=%p\n",
							(void *) pub->slot, (void *) sn,
							nr_branch, nr_clear, nr_metadata,
							(int) climbed, (int) climb_promoted,
							(void *) entry_holder_slot,
							(void *) entry_holder_raw,
							(void *) CMM_LOAD_SHARED(*entry_holder_slot),
							(void *) holder_old_flag,
							(void *) detach_node_flag_ptr,
							(void *) detach_parent_flag_ptr,
							(void *) iter_node_flag,
							(void *) elevated_old_child,
							(void *) plan_old_child,
							(void *) topmost_external_nodes,
							(void *) dbg_arg_detach_slot,
							(void *) dbg_arg_parent_slot);
						for (di = 0; di < nr_metadata; di++)
							fprintf(stderr, "FT DEL-SPLIT-CTX   metadata_stack[%u]=%p item=%p state=%#lx\n",
								di, (void *) metadata_stack[di],
								cds_ft_metadata_to_item(metadata_stack[di]),
								(unsigned long) CMM_LOAD_SHARED(metadata_stack[di]->state));
					}
				}
#endif
				ret = ft_remove_one_commit(ft, pub->slot,
					pub->slot_owner,
					pub->old_val, pub->new_val,
					pub->state_meta,
					fuse_cell, run, commit_txn, NULL, record_only);
				commit_txn_used = (commit_txn != NULL);
			}
			/*
			 * Hand the collected orphan chain off to the deferred free
			 * below the republish block (@orphan_free).  A
			 * recompaction's forward unlink commits down
			 * there.  So the reclaim must wait for it.  A
			 * freed node's freeze-on-free tombstone commits
			 * with that unlink.  The free must follow the
			 * structural unlink, not precede it.  The
			 * tombstones are already recorded above: into
			 * @commit_txn (pub), or standalone (list-off).
			 */
			if (!ret) {
				/*
				 * FOLD: this detach commits NOTHING.  So
				 * the chain is still live and linked when
				 * it returns.  The caller's commit is the
				 * unlink.  Hand the chain out with the
				 * retired copy's lifetime.  Do not arm the
				 * local deferred free below.  It would
				 * reclaim a node a reader is entitled to be
				 * walking.  It would also free it when the
				 * caller's commit ABORTS and nothing
				 * unlinked it.
				 */
				if (record_only) {
					if (recompact_out) {
						for (fi = 0; fi < nr_to_free; fi++)
							recompact_out->orphans[fi] =
								to_free[fi];
						recompact_out->nr_orphans = nr_to_free;
						recompact_out->orphan_trailing =
							trailing_skip_cn_flag;
					}
				} else {
					for (fi = 0; fi < nr_to_free; fi++)
						orphan_free[fi] = to_free[fi];
					nr_orphan_free = nr_to_free;
					orphan_trailing = trailing_skip_cn_flag;
					free_orphans_pending = true;
				}
			}
		}
	}
	if (ret)
		goto end;

	/*
	 * Update address of parent ptr in its parent.
	 * Skip for compressed parents: the replacement was already
	 * published inline above.  Skip entirely when shape-D fusion
	 * already committed the merged compressed node in place of the
	 * boundary.  The boundary is then freed.  There is no forward
	 * republish and no separate prune.
	 */
	if (!boundary_fused &&
	    !ft_node_compressed(iter_node_flag) &&
	    !ft_node_skip_compressed(iter_node_flag)) {
		struct cds_ft_metadata *iter_meta =
			cds_ft_item_to_metadata(ft_node_ptr(iter_node_flag));

		/*
		 * nr_keys fold (LEAF Increment 2).  A recompaction rebuilds the
		 * holder smaller.  The fresh copy (@iter_node_flag) carried the
		 * OLD node's count out of ft_node_recompact.  So bake in its
		 * post-removal count here.  This is a build-invisible plain
		 * store on the not-yet-published fresh copy.
		 *
		 * Reader-safety.  The fresh copy is reachable via up-walk
		 * pre-commit, because recompact re-parents children to it.  But
		 * a node's own nr_keys is only ever read at a root-DESCENDED
		 * position.  That lands on the OLD node via the not-yet-flipped
		 * parent slot.  So the transiently-shifted count is never
		 * observed.  This is the same invariant as the insert I7
		 * relocation fold.
		 *
		 * The -1 walk from the STABLE grandparent iter_meta->parent
		 * rides the republish below.
		 */
		if (old_recompacted_node && count_delta)
			ft_nr_keys_store(ft, iter_meta,
				ft_nr_keys_get(iter_meta) + count_delta,
				CMM_RELAXED);

		dbg_printf("ft_detach_node: publish %p instead of %p\n",
			iter_node_flag, *detach_parent_flag_ptr);
		/*
		 * Recompaction publish.  This arm runs when:
		 * - old_recompacted_node is set: the holder was rebuilt
		 *   smaller;
		 * - topmost_external_nodes is NULL: a pure delete, no
		 *   external-promote.
		 * When fusion is requested, record the publish's 1-2
		 * reader-visible edges:
		 * - the forward slot;
		 * - a compressed grandparent's SKIP_X dual.
		 * Commit them in ONE flip with @fuse_cell's unsplice.
		 * This is the recompaction dual of the in-place fusion
		 * above.  Signal it via pub->armed, so the caller skips
		 * the standalone unsplice.
		 *
		 * A compressed grandparent publishes into cn->child.  Every
		 * reader that descends a compressed child now resolves a flip
		 * proxy there (ft_cn_child_dereference_acquire_prefetch).  So
		 * the parked forward edge is safe.
		 *
		 * Publish directly for an in-place redundant republish, an
		 * external-promote, or list off.
		 */
		if ((fuse_cell || run) && pub && !pub->armed &&
		    old_recompacted_node && !topmost_external_nodes) {
			/*
			 * @mtxn: this publish can carry a compressed
			 * grandparent's SKIP_X dual.  The dual's HOME is
			 * resolved from a parent_word.  THIS op may already
			 * have a pending re-parent for that word: a same-trie
			 * rekey folds its detach into the graft's commit.
			 * Without the handle, ft_dual_home_is_private cannot
			 * see that re-parent.  It answers with the PRE-OP
			 * slot, a word inside the node this very commit
			 * retires.  This is the same defect as the
			 * external-promote arm above, two sites deeper.
			 */
			struct ft_pub_rec rec = { .ctx = &lctx, .n = 0,
				.mtxn = commit_txn ? commit_txn->mtxn : NULL };

			/*
			 * MW consistency.  The up-prune walk recovered
			 * @detach_parent_flag_ptr.  The recompact
			 * inherited @iter_meta->parent and its state-word
			 * slot offset from a later snapshot.  A peer
			 * re-home of the grandparent between the two tears
			 * the pair.  _ft_publish_to_parent_meta would then
			 * compute the fresh copy's offset
			 * (ft_set_parent_slot -> ft_slot_to_byte) off the
			 * wrong parent body.  It would fault out of range
			 * against its bitmap.
			 *
			 * So bail to a re-descend when iter_meta's OWN
			 * resolved slot no longer IS
			 * @detach_parent_flag_ptr.  Nothing is published
			 * yet: this fresh copy is build-invisible.  So the
			 * end: unwind discards the copy and the unconsumed
			 * txn.  This is exactly as the peer-won -EAGAIN
			 * below.
			 */
			if (ft_get_parent_slot(iter_meta, ft) !=
					detach_parent_flag_ptr) {
				ret = -EAGAIN;
				goto end;
			}
			/*
			 * VALIDATE (§4.B): guard the LIVE grandparent iter_meta->parent.
			 *
			 * LOCK_FINE (§9.3).  A recompact ran
			 * (@old_recompacted_node).  So this grandparent is
			 * its P.  It is already LOCKED, with a
			 * {LOCK|s -> s} release recorded on this very word.
			 * The release record IS the guard: same word, same
			 * abort on a peer state change.  It is strictly
			 * stronger, so the conversion REPLACES the guard.
			 * (Leaving the guard would be vacuous, not wrong.
			 * Release-then-guard chains as a read-your-writes
			 * no-op.  See the ordering rule at
			 * ft_flip_txn_record_release_lock.)
			 */
			if (!(ft->lock_fine && old_recompacted_node))
				ft_flip_txn_guard_parent_ctx(ft, commit_txn,
					ft_parent_node(iter_meta->parent_word),
					op_ctx);
			/*
			 * PHASE B, STEP B2 -- THE ARM, RECOMPACTION PUBLISH
			 * (fused).  This is where the remove surface commits
			 * whenever the delete RECOMPACTED.  That happens:
			 * - for every delete, on a build without
			 *   FEATURE_FT_INSERT_IN_PLACE;
			 * - on any build, for a delete that would shrink the
			 *   holder below min_child (@in_place,
			 *   ft_node_replace_ptr).
			 * An in-place delete commits through the pub-armed arm
			 * above instead.
			 *
			 * PLACEMENT.  The op's last ft_flip_txn_lock_register
			 * on @commit_txn is ft_node_recompact's RELEASE-half
			 * loop ({P}, plus {GP} when P is compressed).  That
			 * loop ran inside ft_node_replace_ptr above.  The line
			 * beside this one plants a read-set guard and
			 * registers nothing.  ft_remove_commit_rec below plants
			 * the first record after it.  Measured
			 * (-DFT_ARM_REACH): @nr_locks is NEVER zero here.  So
			 * the helper's empty-registry refusal is not what
			 * decides this site.
			 *
			 * ☞ WHAT IT CONVERTS is the forward publish (+ a
			 * compressed grandparent's SKIP_X dual).  Its owner is
			 * the grandparent this recompact holds and released
			 * through this very txn.  The recompaction's OWN edges
			 * are not converted and must not be.  These are the
			 * reparent sweep and the child state guards.  They
			 * dispatch MW themselves (@child_held false in
			 * ft_flip_txn_record_parent_word /
			 * ft_reparent_record_meta).  The reason is that C's
			 * CHILDREN are never in the DLM set.  The cell / run
			 * edges stay MW_ALWAYS by their own helpers.
			 *
			 * ☠ NOT ON THE FOLD PATH.  @record_only means
			 * @commit_txn is the CALLER's shared txn.  Whether its
			 * registry is complete is the caller's judgement, not
			 * this frame's.  The rekey writer arms it itself.  So
			 * refusing here costs nothing.
			 */
			/*
			 * §9.3's THIRD MEMBER, as on the non-fused twin below.
			 * The dual's grandparent is DERIVED at publish.  So the
			 * only sound way to hold it is to acquire it from that
			 * same derivation.  This sits ABOVE the arm, because it
			 * is a ft_flip_txn_lock_register.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft, &lctx, commit_txn,
				ft_parent_node(iter_meta->parent_word),
				commit_txn ? commit_txn->mtxn : NULL);
			if (commit_txn && !commit_txn_used && !record_only)
				ft_flip_txn_arm_per_op(ft, commit_txn);
			_ft_publish_to_parent(ft, ft_parent_node(iter_meta->parent_word),
				detach_parent_flag_ptr, iter_node_flag,
				holder_old_flag, &rec,
				/*
				 * ☠ FALSE, AND `old_recompacted_node != NULL`
				 * WAS NOT SOUND.  It said "the recompact took
				 * {C,P,GP}, so we hold the dual's owner".  That
				 * is a PLAN-TIME fact:
				 * - ft_node_recompact resolves @pf_gp before
				 *   the acquire;
				 * - it takes GP only `if (pf_gp)`;
				 * - ft_dlm_acquire_set skips the empty member
				 *   (`if (!set[i].nf) continue`), guard
				 *   included.
				 * The dual's owner, meanwhile, is DERIVED FRESH
				 * at publish from cn_meta's back-pointer.  So a
				 * compressed P that was ROOT-ATTACHED at plan
				 * time yields no GP and no guard.  A peer
				 * re-home can land in that window.  The publish
				 * then derives a grandparent this op never
				 * acquired.  That is an SW park on an unowned
				 * word, at an armed site.  It is silent without
				 * rcu-debug.
				 *
				 * No current op live-re-homes a root-attached
				 * compressed node.  Root restructures retire
				 * and rebuild.  So the window is
				 * unproven-reachable.  That is a reason to keep
				 * looking, not a reason to park on it.  The
				 * dual costs one record on the minority of
				 * republishes whose parent is compressed.  MW
				 * is stricter and always sound.
				 *
				 * ☞ To make this true again, the ANSWER must be
				 * publish-time.  Compare the acquired GP
				 * against the derived dual owner, not the
				 * plan's intent.
				 */
				dual_gp_held);
			/*
			 * nr_keys fold (LEAF Increment 2).  The
			 * recompaction's -1 walk from the STABLE
			 * grandparent iter_meta->parent rides this same
			 * commit.  The rebuilt copy itself was baked above.
			 */
			if (count_delta) {
				ft_flip_txn_record_count_parent(ft, commit_txn,
					ft_parent_node(iter_meta->parent_word),
					count_delta);
				count_folded = true;
			}
			/*
			 * Recompaction: its eager child re-parent already
			 * ran in ft_node_replace_ptr.  So the publish
			 * commits through the pre-reserved txn and cannot
			 * fail.  The txn was reserved above; this arm
			 * requires fuse_cell/run.
			 */
			if (ft_remove_commit_rec(ft, &rec, fuse_cell, run,
					commit_txn_used ? NULL : commit_txn,
					record_only) != URCU_TXN_STATUS_OK) {
				/* Peer won: nothing installed (txn consumed). */
				commit_txn_used = (commit_txn != NULL);
				ret = -EAGAIN;
				goto end;
			}
			commit_txn_used = (commit_txn != NULL);
			pub->armed = true;
		} else if (!(pub && pub->armed) &&
		    (old_recompacted_node || topmost_external_nodes)) {
			/* @mtxn: as the fused arm above -- the dual's home must
			 * be resolved against this op's own pending re-parent. */
			struct ft_pub_rec rec = { .ctx = &lctx, .n = 0,
				.mtxn = commit_txn ? commit_txn->mtxn : NULL };

			/*
			 * A real forward-slot change is not yet committed.
			 * It is one of:
			 * - a non-fused recompaction (rebuilt node);
			 * - a non-in-place external promote.
			 * Commit the forward slot + any
			 * compressed-grandparent SKIP_X dual atomically
			 * through @commit_txn.  The recompaction's child
			 * re-parent already ran.  A lone-edge promote
			 * stays one release store.
			 *
			 * The !pub->armed guard excludes the IN-PLACE
			 * external promote.  There the holder never moves.
			 * The promoted head goes into a slot below it.
			 * ft_remove_one_commit already committed that slot,
			 * fused with the cell unsplice, and consumed
			 * @commit_txn.  Re-emitting the unchanged
			 * parent->holder edge here is an old==new no-op.
			 * With @commit_txn spent, it would also trip the
			 * NULL-txn assert(n<=1) once a compressed
			 * grandparent makes it multi-edge.  So it falls
			 * through to the no-op else.  (pub->armed and
			 * old_recompacted_node are mutually exclusive.)
			 */
			/*
			 * MW consistency, RECOMPACTION sub-case ONLY
			 * (old_recompacted_node set).  A non-fused
			 * recompaction publishes a build-invisible fresh
			 * copy.  The recompact inherited its (parent,
			 * offset) as ONE plain snapshot.  So
			 * ft_get_parent_slot(iter_meta) is a STABLE value
			 * no peer can write.  A grandparent re-home can
			 * tear it against @detach_parent_flag_ptr.  That
			 * would fault ft_slot_to_byte in
			 * _ft_publish_to_parent_meta.  So bail to a
			 * re-descend.  The fresh copy is freed at end:, and
			 * the recorded (unconsumed) txn is destroyed.
			 * Nothing is published yet, as at the fused site
			 * above.
			 *
			 * Do NOT guard the external-promote sub-case
			 * (old_recompacted_node == NULL).  It is reachable
			 * list-off with pub == NULL.  There @iter_node_flag
			 * is the LIVE holder.  Its promoted head was
			 * already published reader-visibly above.  So a
			 * -EAGAIN here would STRAND that mutation: the
			 * caller retries as "nothing published".  Its
			 * parent is also peer-writable.  A check-then-use
			 * cannot make the raw re-read in
			 * _ft_publish_to_parent_meta atomic anyway.  That
			 * torn pair is a separate, pre-existing list-off
			 * gap.
			 */
			if (old_recompacted_node &&
			    ft_get_parent_slot(iter_meta, ft) !=
					detach_parent_flag_ptr) {
				ret = -EAGAIN;
				goto end;
			}
			/*
			 * VALIDATE (§4.B): lock (or guard-fallback) the LIVE
			 * grandparent iter_meta->parent.  It is the
			 * value-swap target (§10.5).
			 *
			 * LOCK_FINE (§9.3): skip entirely ONLY when a
			 * recompact actually ran.  Then this grandparent is
			 * its P, already locked.  Its recorded
			 * {LOCK|s -> s} release IS the guard, and is
			 * strictly stronger.  See the ordering rule at
			 * ft_flip_txn_record_release_lock.  The
			 * external-promote sub-case below reaches here with
			 * @old_recompacted_node == NULL and NO recompact.
			 * So there is no lock on this parent.
			 * lock_or_guard acquires it, or falls back to a
			 * guard.
			 */
			if (ft->lock_fine && old_recompacted_node)
				; /* recompact's P already locked; release IS the guard */
			else
				ft_flip_txn_lock_or_guard_parent(ft, commit_txn,
					&lctx,
					ft_parent_node(iter_meta->parent_word),
					FT_DEPTH_FROM_DESCENT);
			/*
			 * §9.3's THIRD MEMBER, and the answer this producer owes
			 * the record below.  The paragraph that used to sit at
			 * the @dual_owner_held argument said exactly how to earn
			 * it.  It read: "the ANSWER must be publish-time: the
			 * acquired GP compared against the derived dual owner,
			 * not the plan's intent".  ft_lock_skip_dual_gp IS
			 * that.  It derives the GP the same way the record
			 * will, and acquires THAT.  So its return is the
			 * honest held answer.  It is not an inference from
			 * `old_recompacted_node != NULL`.
			 *
			 * It is a no-op unless a dual is actually recorded.
			 * @mtxn NULL mirrors this producer's @rec, which
			 * carries none.  It sits ABOVE the arm, because it is a
			 * ft_flip_txn_lock_register.  The arm's contract is
			 * "after the op's LAST register".  The promote arms use
			 * the same placement.
			 */
			dual_gp_held = ft_lock_skip_dual_gp(ft, &lctx, commit_txn,
				ft_parent_node(iter_meta->parent_word),
				commit_txn ? commit_txn->mtxn : NULL);
			/*
			 * PHASE B, STEP B2 -- THE ARM, RECOMPACTION PUBLISH
			 * (non-fused) and the non-in-place external promote.
			 * The argument is the fused arm's above, with one
			 * difference.  That difference decides the placement.
			 * On the else arm, the lock_or_guard beside this line
			 * IS this path's last register.  (The recompact arm's
			 * was ft_node_recompact's release loop.)  So the arm
			 * sits below BOTH.
			 */
			if (commit_txn && !commit_txn_used && !record_only)
				ft_flip_txn_arm_per_op(ft, commit_txn);
			_ft_publish_to_parent(ft, ft_parent_node(iter_meta->parent_word),
				detach_parent_flag_ptr, iter_node_flag,
				holder_old_flag, &rec,
				/*
				 * ☐ STILL false, but for a DIFFERENT reason
				 * than before.  The difference is the whole
				 * finding.  The note that stood here explained
				 * why `old_recompacted_node != NULL` was NOT
				 * sound.  It is a PLAN-TIME fact:
				 * - ft_node_recompact resolves @pf_gp before
				 *   the acquire;
				 * - it takes GP only `if (pf_gp)`;
				 * - ft_dlm_acquire_set skips an empty member.
				 * The dual's owner is DERIVED FRESH at publish
				 * from cn_meta's back-pointer.  So a compressed
				 * P that was ROOT-ATTACHED at plan time yields
				 * no GP.  A peer re-home landing in that window
				 * would have parked SW on a word this op never
				 * acquired.
				 *
				 * ☑ THE ACQUIRE IS NOW TAKEN ANYWAY, above.  It
				 * is worth taking for its own sake, kind aside.
				 * struct ft_pub_rec says "holding GP is what
				 * EXCLUDES a peer recompaction from copying
				 * that body out from under the record, and that
				 * is ft_lock_skip_dual_gp's job, not this word's".
				 * So this site now HOLDS the word it writes.
				 *
				 * ☑ AND THE KIND NOW FOLLOWS @dual_gp_held.  The
				 * note that stood here said the flip waited on
				 * ft_node_recompact ("it holds the DERIVED
				 * grandparent 0 times in 9721").  It then
				 * corrected itself with "the kind is a per-op
				 * claim, not a global one -- the LOCK
				 * serialises an SW park against an MW CAS, so
				 * producers convert INDIVIDUALLY".
				 *
				 * ☠ THAT SECOND SENTENCE IS TRUE ONLY OF AN MW
				 * THAT HOLDS THE WORD.  Stating it unqualified
				 * is how a body word gets read as lock-bearing.
				 * An MW record whose op holds nothing
				 * arbitrates against nothing.  So a peer's SW
				 * park, a plain store, genuinely races it.
				 * The per-site conversion is NOT safe because
				 * "MW and SW serialise".  It is safe because a
				 * site which cannot vouch answers FALSE and
				 * stays MW.  The lock-bearing carve-out belongs
				 * to @metadata.state: an MW record TAKES the
				 * lock, and SW writes follow it.  It does NOT
				 * belong to a grandparent body word, which has
				 * no lock in its bits.
				 *
				 * The 9721 reading was itself a blind query.
				 * That site HELD its word.  It was asked
				 * through a registry ft_dlm_acquire_set can
				 * never populate.
				 * ☞ THE TRANSACTED-SLOT REGISTER.
				 */
				dual_gp_held);
			(void) dual_gp_held;
			/*
			 * nr_keys fold (LEAF Increment 2).  A non-fused
			 * RECOMPACTION folds the -1 walk from the stable
			 * grandparent onto this republish.  The rebuilt
			 * copy was baked above.  A non-in-place external
			 * PROMOTE (old_recompacted_node NULL) does NOT.
			 * The holder never moved, and this republish
			 * re-emits an old==new no-op.  The promote's count
			 * falls to the standalone residual below (base
			 * @iter_node_flag).
			 */
			if (old_recompacted_node && count_delta) {
				ft_flip_txn_record_count_parent(ft, commit_txn,
					ft_parent_node(iter_meta->parent_word),
					count_delta);
				count_folded = true;
			}
			if (ft_remove_commit_rec(ft, &rec, NULL, NULL,
					commit_txn_used ? NULL : commit_txn,
					record_only) != URCU_TXN_STATUS_OK) {
				/* Peer won: nothing installed (txn consumed). */
				commit_txn_used = (commit_txn != NULL);
				ret = -EAGAIN;
				goto end;
			}
			commit_txn_used = (commit_txn != NULL);
		}
		/*
		 * else: in-place redundant republish.  The holder
		 * stayed at its parent slot (a plain leaf delete or
		 * an in-place external promote).  So the forward slot
		 * already holds @iter_node_flag, and so does any
		 * compressed-grandparent SKIP_X dual.  The in-place
		 * forward store committed via ft_remove_one_commit
		 * above.  Re-emitting the unchanged slot(s) is a pure
		 * no-op (old == new), so skip it entirely.  Any
		 * commit_txn left unconsumed here is freed at @end.
		 */

		/*
		 * nr_keys: every outcome now folds its -@count_delta walk
		 * onto the op's own commit:
		 *   - in-place and recompaction fold into @commit_txn;
		 *   - shape-D folds into ft_chain_compress_fused's txn;
		 *   - a compressed parent folds into @orphan_txn;
		 *   - the former list-off pub-less lone store uses the
		 *     @local_pub substitution above.  It arms @pub, so the
		 *     pub-armed fold fires.
		 * There is no standalone post-commit residual left.
		 */
		assert(count_folded || !count_delta || !ft->rank_stats);

#ifdef FEATURE_FT_SKIP_COMPRESSED
		/*
		 * Post-detach canonicalization.  The surviving ancestor may
		 * now be a non-root internal with exactly 1 live child and
		 * no external_nodes attached.  If so, fold it via the
		 * chain-compress 4-case merge (canonical form under
		 * SKIP_COMPRESSED).
		 */
		/*
		 * ☠ NEVER ON THE FOLD (@record_only).  This is a STANDALONE
		 * SECOND FLIP.  Under the fold the caller's one-decide txn
		 * has NOT COMMITTED yet.  Canonicalizing here publishes over
		 * a plan the caller still holds.  Measured:
		 * ft_skip_reanchor's assert(0), the up-walk landing on a
		 * mid-plan parent chain.
		 *
		 * On the fold, this is reachable only through the
		 * out-of-bound residue: merged_len > FT_SKIP_LEN_MAX falls
		 * back to the recompaction above.  The pending-publish
		 * sibling shape does not reach it: it is either FUSED with
		 * the @pending_child substitution or refused whole.
		 *
		 * That rare geometry is a near-max compressed parent run
		 * over a 2-child boundary.  It then keeps its 1-child
		 * internal past the op.  Skip mode's verifier flags that at
		 * the writer-scope exit.
		 *
		 * KNOWN INCOMPLETE: closing it needs the caller to own a
		 * post-commit canonicalize.  That is a rekey-side change,
		 * not a change to this frame.
		 */
		if (!record_only &&
		    ft_meta_nr_child(iter_meta) == 1 &&
		    !iter_meta->external_nodes &&
		    ft_parent_node(iter_meta->parent_word) != NULL) {
			/*
			 * Post-commit: the structural commit consumed its own
			 * registry, so only the orphan marks this op still owns
			 * are held.
			 */
			lctx.held.txn = NULL;
			lctx.held.nr_extra = (unsigned int) nr_orphan_locked;
#ifndef FT_DEBUG_NO_FOLD_RENAME
			if (wd_valid && old_recompacted_node)
				ft_descent_rename(&wd, old_recompacted_node,
					iter_node_flag);
#endif
			ft_canonicalize_chain_compress(ft, iter_node_flag,
				cur_depth, &lctx, iter_meta);
		}
#endif
	}
	/*
	 * Deferred branch-2 orphan free (see @orphan_free).  It runs AFTER the
	 * commit that unlinked the chain: the in-place ft_remove_one_commit or
	 * the recompaction republish above.  So the freeze-on-free tombstone of
	 * every freed node has already committed, and the reclaim follows the
	 * structural unlink.  An abort takes `goto end` above and bypasses
	 * this.  The flag is set only on a branch-2 success.
	 */
	if (free_orphans_pending) {
		int fj;

		if (orphan_trailing)
			free_compressed_node(ft, ft_skip_to_compressed(ft,
				orphan_trailing));
		for (fj = 0; fj < nr_orphan_free; fj++) {
			if (ft_node_compressed(orphan_free[fj]))
				free_compressed_node(ft,
					ft_compressed_node_ptr(orphan_free[fj]));
			else
				free_cds_ft_node(ft, ft_node_ptr(orphan_free[fj]));
		}
	}
end:
	/*
	 * DLM orphan plan-lock cleanup (§9.2): release every orphan lock acquire the
	 * op still holds AND still owns.  What is left here marks the orphan's OWN
	 * word.  So its terminal is the fenced {LOCK|s -> TOMBSTONE|s}
	 * tombstone.
	 *   - A successful detach committed it.  LOCK is dropped.  An acquire
	 *     refuses a TOMBSTONE, so the bit cannot come back.  clear_if_held
	 *     no-ops.
	 *   - On ANY abort or pre-commit bail, the tombstone never applied.
	 *     This includes a mark-miss mid-collection, which jumps here.  The
	 *     mark is still {LOCK|s} and is released here.  The structure
	 *     returns byte-for-byte to its pre-op state.
	 * One sweep covers all of these, with no "consumed" tracking:
	 *   - both Block A and Block B (mutually exclusive);
	 *   - the trailing skip-target (an entry of @orphan_held like any
	 *     other);
	 *   - every commit outcome (in-place / recompaction / shape-D).
	 *
	 * A mark whose anchor SURVIVES the retire is NOT here.
	 * ft_detach_freeze_one handed it to the freezing txn (@txn_owned).  On
	 * that still-lockable word, only that txn can tell a consumed mark from
	 * a peer's fresh one.
	 */
	{
		int oi;

		for (oi = 0; oi < nr_orphan_locked; oi++)
			if (!orphan_held[oi].shared &&
					!orphan_held[oi].txn_owned) {
				/*
				 * ☠ ONLY A TXN THIS DETACH DOES NOT COMMIT is
				 * safe to ask.  A commit CONSUMES the flip-txn
				 * wrapper.  So on every other path @commit_txn
				 * / @shared_txn may already be freed here, and
				 * reading ->mtxn->desc is a use-after-free.
				 * Measured: SEGV at both fine spacings, test 9,
				 * inside urcu_txn_find on a dead descriptor.
				 *
				 * @record_only is exactly the deferred
				 * contract.  The collapse is RECORDED into the
				 * caller's txn and the caller commits it, so
				 * the txn is open here.
				 */
				struct ft_flip_txn *pend_txn = record_only ?
					shared_txn : NULL;
				const struct ft_txn_rec *pend =
					ft_lock_terminal_pending(pend_txn,
						orphan_held[oi].lock);
				/*
				 * ☠ THE TERMINAL HAS NOT RUN YET.  The sweep's
				 * premise is: a successful detach COMMITTED
				 * this word's fused tombstone, an aborted one
				 * recorded none.  It holds only when the detach
				 * commits its own txn.
				 *
				 * Under a DEFERRED commit (@record_only /
				 * @fold_replace), the rekey's one-decide writer
				 * commits @shared_txn later.  The record exists
				 * and is UNDECIDED right here.  Its expected
				 * old is {LOCK|s}.  A plain-CAS release now
				 * would:
				 *   - drop a bit the record still counts on;
				 *   - make the word lockable by a peer;
				 *   - let the SW park that follows write over
				 *     the peer blind.
				 *
				 * So hand the word to the txn that carries the
				 * terminal.  That txn owns the clear on BOTH
				 * outcomes: its commit consumes the bit into
				 * the tombstone, its destroy releases it.
				 *
				 * Measured (-DURCU_TXN_DEBUG_PARK_CLOBBER): the
				 * parks that clobbered {old 0x80004, found 0x4}
				 * were all this case.  It was worst at per-node
				 * spacing.  There the orphan's lock IS its own
				 * word, and the existing hand-off
				 * (h->lock != m) never fires.
				 */
#ifdef FT_RED_ORPHAN_RELEASE_EARLY
				pend = NULL;	/* red control: release anyway */
#endif
				if (pend) {
					FT_ORPHAN_HANDED();
					if (!ft_flip_txn_holds(pend_txn,
							orphan_held[oi].lock)) {
						unsigned int ls;

						/*
						 * ☠ REGISTER, BUT WITHOUT THE
						 * SILENCER.
						 * ft_flip_txn_lock_own links
						 * &h->shared so that the
						 * commit's terminal can scrub
						 * the handing entry.  But
						 * @orphan_held is THIS FRAME'S
						 * STACK.  A deferred commit
						 * runs after ft_detach_node
						 * has returned, so that pointer
						 * would be dangling.  The
						 * terminal writes through it
						 * (*src_shared = true) and the
						 * run SEGVs (measured, both
						 * fine spacings).
						 *
						 * Nothing reads @orphan_held
						 * past this sweep, which is the
						 * frame's last use.  So the
						 * silencer has no job here.
						 */
						ft_flip_txn_lock_register_member(
							pend_txn,
							orphan_held[oi].lock,
							orphan_held[oi].lock_snap,
							orphan_held[oi].member);
						ls = pend_txn->nr_locks - 1;
						if ((uintptr_t) ft_txn_rec_new(pend) &
								FT_STATE_TOMBSTONE)
							pend_txn->locks[ls].
								tombstone_terminal =
								true;
					}
					orphan_held[oi].txn_owned = true;
					continue;
				}
				/*
				 * ☠ THE FENCE MAY ALREADY BE GONE, AND THE WORD
				 * CANNOT SAY SO.  Our own committed terminal
				 * drops the LOCK.  From that instant a peer may
				 * take the word, and a set bit means THEIRS,
				 * not ours.
				 *
				 * The sweep used to ask the word
				 * (clear_if_held), on the premise quoted above:
				 * "an acquire refuses a TOMBSTONE, so the bit
				 * cannot come back".  That holds only for a
				 * terminal that actually leaves the word
				 * TOMBSTONEd.  Where the terminal drops LOCK
				 * without a TOMBSTONE, the word is re-lockable
				 * and the sweep clears a peer's fence.
				 *
				 * So ask what THIS op did:
				 *   - Under @record_only the descriptor is
				 *     still alive and answers exactly (above).
				 *   - Otherwise it is already freed (the UAF
				 *     this block's first comment records), and
				 *     the op's own outcome is the witness.
				 *     @ret == 0 means the detach committed, so
				 *     its fused tombstone consumed the fence.
				 *     Any bail or abort applied nothing, and
				 *     the mark is still {LOCK|s}.
				 */
				if (!pend_txn && !ret) {
					FT_ORPHAN_CONSUMED();
					/*
					 * PREFER A LEAK TO A THEFT, AND
					 * MEASURE IT.  If the premise is ever
					 * false, the fence stays set and the
					 * node becomes unmutable.  That is bad
					 * but bounded and local.  A theft
					 * corrupts a peer's exclusion.
					 *
					 * Record it, do not refuse.  A word
					 * that is still LOCKED and not
					 * TOMBSTONEd here means the premise
					 * failed.  The count is the thing to
					 * look at next.
					 */
					FT_ORPHAN_PREMISE_CHECK(
						orphan_held[oi].lock);
					continue;
				}
				FT_ORPHAN_RELEASED();
				ft_meta_lock_release(orphan_held[oi].lock);
				/*
				 * SCRUB only RELEASED-LIVE (finding A).  A
				 * TOMBSTONED word is a CONSUMED fence and must
				 * keep answering holds().  Dedupe-on-dead is
				 * the designed flow.  Taking a dead word
				 * hard-refuses forever (the exp-MW storm).
				 */
				if (!(CMM_LOAD_SHARED(
						orphan_held[oi].lock->state) &
						FT_STATE_TOMBSTONE))
					orphan_held[oi].shared = true;
			}
	}
	/*
	 * nr_keys fold (LEAF Increment 2): no abort rollback needed.  On the
	 * abort path, every per-outcome count fold rides an UNcommitted txn.
	 * The commit is what would apply it, so an abort applies nothing.  The
	 * standalone residual runs only on success (ret == 0, inside the
	 * parent-slot block).  So a failed detach leaves nr_keys untouched, and
	 * there is no pre-decrement left to undo.
	 */
	/*
	 * Free a pre-reserved commit txn that no commit consumed.  This happens
	 * when the reservation succeeded but ft_node_replace_ptr recompacted or
	 * failed, or when shape-D fused with its own txn.  The txn is in
	 * PREPARE state, so no grace period is needed.
	 *
	 * Under record_only, @commit_txn IS the caller's SHARED txn.  It is
	 * never freed here.  The caller owns it and, on any bail, destroys it
	 * after re-descending.
	 */
#ifdef FT_DEBUG_STACK_SILENCER
	/*
	 * ☠ DOES A TXN THAT OUTLIVES THIS FRAME STILL POINT INTO IT?
	 *
	 * ft_flip_txn_lock_own links &h->shared as the terminal's silencer, and
	 * @orphan_held is THIS frame's stack.  When the txn is the caller's
	 * deferred one (@record_only), its terminal runs after this function has
	 * returned and writes through that pointer.  This is the same dangling
	 * write that SEGV'd the orphan sweep before @c5ab837d.
	 *
	 * The bounds are known exactly here, so this is a measurement, not a
	 * heuristic.  It guesses no address arithmetic.  It only asks "is the
	 * registered silencer inside the array I am about to destroy".
	 */
	if (record_only && shared_txn) {
		unsigned int li;

		/* ARM: a zero above is only evidence if this ran at all. */
		uatomic_inc(&ft_stack_silencer_scans);
		uatomic_add(&ft_stack_silencer_locks, shared_txn->nr_locks);
		for (li = 0; li < shared_txn->nr_locks; li++) {
			const void *sil = (const void *)
				shared_txn->locks[li].src_shared;

			if (sil >= (const void *) &orphan_held[0] &&
					sil < (const void *) &orphan_held[
						FT_MAX_DEPTH + 1]) {
				if (uatomic_add_return(&ft_stack_silencer_hits,
						1) <= 4)
					fprintf(stderr, "FT STACK SILENCER: txn %p lock slot %u points at %p, inside this frame's orphan_held[] -- it outlives the frame\n",
						(void *) shared_txn, li, sil);
			}
		}
	}
#endif
	if (commit_txn && !commit_txn_used && !record_only)
		ft_flip_txn_destroy(commit_txn);
	/*
	 * Reclaim safely after replacement.
	 *
	 * Under record_only, the OLD copy of the src-junction recompaction
	 * stays LIVE until the CALLER's commit publishes the fresh copy.  It is
	 * resolved through the parked grandparent proxy.  So its free is
	 * DEFERRED to the caller.  On a recorded-OK (ret == 0) path, hand it
	 * out through @old_recompacted_out.  The caller frees it (call_rcu)
	 * after its commit succeeds.
	 *
	 * A record_only bail (ret != 0) frees the unpublished fresh copy here,
	 * exactly as the self-committing abort arm.
	 */
	if (record_only && old_recompacted_node && !ret) {
		/*
		 * Hand BOTH copies to the caller:
		 *   - The OLD copy stays LIVE until the caller's commit
		 *     retires it.  It is resolved through the parked
		 *     grandparent proxy.  Free it on commit OK.
		 *   - The fresh NEW copy (@iter_node_flag, republished into
		 *     the shared txn) is UNPUBLISHED until that commit.  Free
		 *     it unpublished if the caller's commit ABORTS.
		 */
		if (recompact_out) {
			recompact_out->old_node = old_recompacted_node;
			recompact_out->new_flag = iter_node_flag;
		}
	} else if (old_recompacted_node) {
		if (ret == -EAGAIN)
			/*
			 * ABORTED republish: the rebuilt copy never published.
			 * Its child re-parent edges and the old copy's
			 * tombstone were recorded into the aborted commit and
			 * discarded with it.  The OLD node stays live and
			 * linked.  Reclaim the never-visible FRESH copy
			 * instead.
			 */
			free_cds_ft_node_unpublished(ft,
				ft_node_ptr(iter_node_flag));
		else
			free_cds_ft_node(ft, old_recompacted_node);
	}

	/*
	 * Standalone fallback for a caller-supplied @retire_glue that no commit
	 * txn absorbed.  Which detach lands here:
	 *   - A LIST-ON src detach does not.  It passes pub != NULL and fuses
	 *     above (probed: 100% branch-2 commit_txn).
	 *   - A LIST-OFF src detach does.  It passes pub == NULL (no cell to
	 *     unsplice, run == NULL), so the freeze block's `pub` gate fails.
	 *     This is the same lone-store residual that the list-off orphan
	 *     chain leaves.  It is to be closed once the lone edge is forced
	 *     through a txn.
	 *   - A theoretical shape-D / compressed-parent detach does too.
	 *
	 * On success, the unlink that stranded the retire set has committed:
	 * freeze the set standalone before the caller frees it.  On an abort
	 * (ret != 0) the caller rolls the whole op back and does NOT free it,
	 * so leave it untouched.  A no-op under one writer either way.
	 */
	if (!ret && retire_glue && !retire_glue_fused) {
		retire_glue->fuse_free_list = false;
		ft_glue_tombstone_free_list(retire_glue);
	}
	/*
	 * Standalone fallback for the removed external leaf when no commit txn
	 * absorbed its freeze.  This is the list-off pub-less direct-store
	 * detach: pub == NULL leaves @commit_txn NULL, so the freeze block's
	 * `pub` gate fails.  It is the same lone-store residual as the list-off
	 * orphan chain, to be closed once the lone edge is forced through a
	 * txn.
	 *
	 * On success, the unlink that stranded @node has committed: freeze
	 * @node before the caller reclaims it.  On an abort (ret != 0) leave it
	 * chained.  Behaviour-identical to the old caller-side mark.  A no-op
	 * under one writer.
	 */
	if (!ret && freeze_leaf && !freeze_leaf_fused) {
		unsigned int fi;
		struct cds_ft_node *fn = freeze_leaf;

		/*
		 * Bounded by @freeze_len for the same reason as the txn arm.
		 * A duplicate appended since the caller's derivation is NOT
		 * this op's to retire (ft_hlist_freeze_chain_prepare).  This
		 * arm has no commit to abort, so only the bound enforces
		 * that.
		 */
		/*
		 * ☠☠ THE LAST RAW PRODUCER ON A CHAIN WORD, AND THE CHAIN NOW
		 * PARKS.  ft_node_mark_removed_flip CASes &fn->next outside
		 * the record layer, so it cannot be SW-parked.  Since
		 * @80e855f5 every other writer of that word parks SW by
		 * default.
		 *
		 * Its loop spins out a parked FT_HLIST_TAG before its CAS.
		 * That covers park-then-CAS, but NOT the reverse.  A CAS that
		 * lands first on a clean word is overwritten by the parker's
		 * blind settle.  The settle republishes the successor with
		 * the TOMBSTONE ERASED.  See that function's header for the
		 * full ordering.
		 *
		 * It is safe today only because this arm is not reached:
		 *   - -DFT_DEBUG_CHAIN_CANARY scores the producer at raw = 0
		 *     in ft_inv AND ft_unit at all three spacings;
		 *   - the chain hold audit gives this fallback no row at all.
		 *
		 * ☠ THAT IS A COVERAGE STATEMENT ABOUT TWO SUITES, NOT A
		 * PROOF, and a stale one would be silent.  So count every
		 * entry and, where the engine's own self-checks are armed,
		 * ABORT.  If this path ever becomes reachable, it must be
		 * converted to a RECORDED store before it runs.  It must
		 * never be re-argued.
		 *
		 * ☞ ASSERT ONLY, no counter.  A counter on a path that never
		 * executes is write-only in a release build (nothing prints
		 * it), so it would be decoration.  The measurement above is
		 * the record.  This assert is what stops a silent regression.
		 */
		urcu_assert_debug(!"ft_detach_node standalone freeze reached: "
			"raw CAS on a chain word that now parks SW -- convert "
			"it to a recorded store");
		for (fi = 0; fi < freeze_len && fn; fi++) {
			struct cds_ft_node *fnext;

			ft_ch_audit(ft, NULL, fn);
			fnext = ft_node_mark_removed_flip(ft, fn);
			fn = fnext;
		}
	}
	/*
	 * A fused @retire_glue->txn now points at the commit_txn that its
	 * commit reclaimed above.  Clear it so that a future free-path reader
	 * of the glue cannot dereference freed memory.  This is defensive:
	 * nothing reads it on the caller's free path today.
	 */
	if (retire_glue)
		retire_glue->txn = NULL;

	/*
	 * Density was already propagated before structural changes
	 * (above), while parent pointers were still valid.
	 */
	FT_TP(detach_node_exit, (int) ret);
	return ret;
}

/*
 * ft_unchain_node: remove @node from its duplicate chain using prev/next.
 *
 * @head_slot: address of the pointer that holds the head of the chain
 *             (e.g. &metadata->external_nodes or the parent's child slot).
 *             Only used when @node is the head of the chain.
 * @node:      the node to remove.
 *
 * For head nodes (node->prev is a flagged internal pointer): updates
 * *head_slot to point to node->next.
 * For non-head nodes (node->prev is a cds_ft_node): updates prev->next
 * to skip over node.
 * In both cases, if node->next exists, its prev pointer inherits
 * node->prev (either the parent pointer or the predecessor node).
 *
 * Ordering: next_node->prev is updated BEFORE the pointer publication
 * (*head_slot or prev_node->next).  Suppose we published first.  A
 * concurrent reader could follow the new head via a skip-compressed pointer
 * and call ft_skip_to_compressed.  It would read the stale prev pointing to
 * @node: the node being removed, a cds_ft_node rather than the flagged
 * parent.  It would return a garbage compressed-node pointer.
 * rcu_assign_pointer on the publication provides release semantics.  They
 * pair with the reader's rcu_dereference of child->prev.
 */
/*
 * Head promotion.  @node is the head of a duplicate chain.  It leaves the
 * trie.  @next_node (its next duplicate, non-NULL) takes its place at the
 * same key.
 *
 * Publish a FRESH cell for @next_node.  Set its node field while the cell is
 * still hidden.  This keeps cell->node WRITE-ONCE: the public
 * cds_ft_cell_node and the internal cell readers load it as a plain pointer,
 * never a flip proxy.  Swap the fresh cell in for @node's cell in ONE flip
 * (ft_ord_cell_swap_publish_multi), FUSED with:
 *   - the structural forward publish (@next_node into *@head_slot);
 *   - for a compressed holder, the grandparent SKIP_X dual.
 * So a reader never observes the promoted head at one index but the old head
 * at another.  It never observes a torn cell->node-vs-external_nodes prefix
 * comparison either (ft_rebuild_key_upwalk).
 *
 * @parent_nf is the holder flag used by _ft_publish_to_parent to locate the
 * SKIP_X dual.  For a compressed holder it is the compressed node's PLAIN
 * flag.  Otherwise it is the internal holder flag (no dual).
 *
 * The list-on promotion is fully abortable.  The fresh-cell alloc AND the
 * flip-txn pre-reservation both fail cleanly on OOM.  They return -ENOMEM.
 * Nothing is published, and the chain is intact and retriable.
 *
 * @next_node's prev (its node->cell link) FOLDS into the swap commit as a
 * proxied edge, so it flips atomically with the forward publish.  A
 * concurrent skip resolution strips the transient proxy at the load
 * (ft_dereference_prev_resolved) before ft_resolve_head_prev interprets it.
 *
 * The flip is still pre-reserved so that the commit cannot OOM.  It is never
 * a bare in-place cell->node retarget under memory pressure.
 *
 * List off (no cell) inherits the flagged parent on the successor.  That is
 * still a SETTLED store there (read raw), see the list-off branch.
 */
static
int ft_promote_head(struct cds_ft *ft, const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag *parent_nf, unsigned int parent_depth,
		struct cds_ft_node **head_slot, struct cds_ft_node *node,
		struct cds_ft_node *next_node,
		struct cds_ft_metadata *held_holder, uintptr_t held_snap)
{
	struct ft_ord_cell *old_cell = ft->ordered_list ?
		ft_ord_cell_ptr(node->prev) : NULL;
	struct ft_pub_rec rec = { .ctx = ctx, .n = 0 };
	struct ft_ord_cell_edge sedges[2] = { 0 };
	unsigned int n_s;
	/*
	 * Did this attempt actually ACQUIRE the SKIP_X dual's grandparent?  The
	 * publish must say so per ATTEMPT, not per call site.
	 * ft_lock_skip_dual_gp returns false on the shapes where it acquires
	 * nothing:
	 *   - no compressed parent;
	 *   - no skip-encoded slot;
	 *   - a ROOT dual whose record names no owner at all.
	 * Claiming ownership there hands FT_OWNER_ASSERT_OWNED a NULL owner.
	 */
	bool dual_gp_held;

	assert(next_node != NULL);
	if (old_cell) {
		/*
		 * Ordered list on: cell->node is write-once, so head promotion
		 * publishes a FRESH cell and swaps it in.  Allocate the cell
		 * AND pre-reserve the flip-txn FIRST, before any
		 * reader-visible change.  On either OOM, abort here.  Nothing
		 * is published and the chain is untouched, so the caller
		 * returns CDS_FT_STATUS_MEMORY_ERROR (the promotion may be
		 * retried).  No bare in-place cell->node retarget on OOM:
		 * that would commit a reader-visible store outside the
		 * descriptor protocol.
		 */
		/*
		 * ☠ RESOLVE THE PARENT BEFORE COPYING IT.  The holder's lock
		 * is held, but a re-home INTO that holder can still be
		 * settling.  The peer that published the holder fresh has
		 * already released its born lock.  Its record on this very
		 * cell's parent still parks a proxy, and its descriptor has
		 * SUCCEEDED.  Copied raw, the proxy names a record of ANOTHER
		 * slot that no settle rewrites.  The new cell's parent then
		 * stays a proxy for ever.
		 *
		 * MEASURED: inv_prefix_dup_promote_vs_extension at
		 * exponential spacing hit the rcu-debug "FT CELL PARENT COPY"
		 * abort in 2 of 4 full-suite runs.  A probe at this line saw:
		 *   - the holder live and held;
		 *   - the head slot naming @node;
		 *   - the record re-homing the head from the compressed node
		 *     to that holder.
		 *
		 * The descriptor is decided and the holder is held, so the
		 * resolved value is the parent.
		 */
#ifndef FT_DEBUG_NO_CELL_PARENT_RESOLVE
		void *new_cell_flag = ft_ord_cell_alloc(ft, next_node,
			ft_resolve_flip_proxy(rcu_dereference(old_cell->parent)));
#else
		void *new_cell_flag = ft_ord_cell_alloc(ft, next_node,
			old_cell->parent);
#endif
		struct ft_ord_cell *new_cell;
		struct ft_flip_txn *txn;

		if (!new_cell_flag) {
			if (held_holder)
				ft_meta_lock_release(held_holder);
			return -ENOMEM;
		}
		txn = ft_flip_txn_create_bounded(ft,
			FT_ORD_CELL_SWAP_PUBLISH_MAX_EDGES +
			FT_HLIST_FREEZE_MAX_EDGES + 3 +
			FT_CELL_LOCKSET_MAX_RECORDS);	/* +1 §4.B parent guard, +1 next_node->prev fold,
							 * +1 the SKIP_X dual GP's release-or-guard */
		if (!txn) {
			ft_ord_cell_free_unpublished(ft,
				ft_ord_cell_ptr(new_cell_flag));
			if (held_holder)
				ft_meta_lock_release(held_holder);
			return -ENOMEM;
		}
#ifdef FT_HLIST_CLAIM_PROMOTE
		/*
		 * §4 STEP B4's DRY RUN.  This is the a3c75659 tool aimed at
		 * the EXTERNAL-HEAD lane.  It points B0's owner assert at this
		 * txn.  Every record the txn plants without owning is then
		 * named at an abort.  The build is otherwise byte-identical
		 * to the unarmed one.
		 *
		 * ☞ CLAIMED AT CREATION, EARLIER THAN THE ARM WOULD BE.  So
		 * read a miss FIRST as "this record is planted before its
		 * owner is registered".  e9268e13 closed that ORDERING class
		 * for this very lane by hoisting the holder's fence ABOVE the
		 * back edge.
		 *
		 * ☠ And read a miss as "the registry cannot SEE this hold"
		 * before "the op does not HOLD it" (ft_flip_txn_owns).
		 *
		 * ☠☠ THE PROMOTE KNOB IS SEPARATE FROM THE HEAD-CLEAR ONE.
		 * BOTH promote arms carry a KNOWN EXCLUSION GAP and the head
		 * clear provably does not.  One knob would mask the armable
		 * lane behind the blocked ones.
		 *
		 * THE GAP was measured with --enable-rcu-debug.  The test is
		 * ft_unit test_dup_chain_head_promotion (the 15th test).
		 * _ft_publish_to_parent_meta emits TWO structural edges when
		 * the holder is skip-encoded:
		 *   - the forward @head_slot, owned by @parent_nf and
		 *     REGISTERED by the fence above;
		 *   - a SKIP_X DUAL into the GRANDPARENT's body, owned by a
		 *     node this op never acquires.  In the repro it is the
		 *     ROOT node's slot at item+8.
		 * ft_ord_cell_flip_into gives both edges the dispatching
		 * recorder, so an armed txn would SW-PARK a word the op does
		 * not exclude.
		 *
		 * ☠ IT IS NOT SPECIFIC TO THIS ARM.  It is SLOT-SHAPED
		 * ([[feedback_a_site_inventory_cannot_cover_a_dynamic_slot]]).
		 * The LIST-OFF twin below carries it too.  Its whole-suite
		 * dry run was CLEAN, yet a constructed public-API repro
		 * aborts it.  A clean dry run is evidence about the SUITE,
		 * not about the lane.
		 * ☞ ft_unchain_node's head clear is the exception, by ROUTING
		 * rather than by luck.  See the detector there.
		 *
		 * ☞ The fix is per EDGE, not per site.  ft_ord_cell_edge
		 * already carries @owner and @root.  The missing third answer
		 * is whether the OP HOLDS that owner.
		 * ft_flip_txn_record_parent_word already takes the same shape
		 * as @child_held.  Where the op does hold the owner
		 * (ft_detach_node's republish holds the recompact's {P,GP}),
		 * the dual is a real conversion.  So a blanket always-MW
		 * would give that back.  There are two options: carry the
		 * flag, or extend these ops' lock-set to the grandparent.
		 * Which one is a DESIGN call, not this step's to assume.
		 */
		ft_flip_txn_claim_per_op_armable(ft, txn);
#endif
		new_cell = ft_ord_cell_ptr(new_cell_flag);
		cds_ft_item_to_metadata(new_cell)->incoming_byte =
			cds_ft_item_to_metadata(old_cell)->incoming_byte;
		/*
		 * Back-pointer (next_node->prev: the promoted successor's
		 * node->cell link).  It is FOLDED into the swap commit as a
		 * proxied edge.  It flips ATOMICALLY with the forward head
		 * publish and @node's freeze.  It is never a pre-commit
		 * reader-visible store.
		 *
		 * A concurrent skip resolution strips the transient proxy at
		 * the load (ft_dereference_prev_resolved) before
		 * ft_resolve_head_prev interprets it, so the slot may carry
		 * the proxy.  The forward publish's forward-before-parent
		 * check reads the folded prev's intended value
		 * (new_cell_flag), not the not-yet-stored slot.  The
		 * reservation above carries this edge.
		 */
		/*
		 * THE HOLDER'S FENCE IS TAKEN BEFORE THE BACK EDGE, NOT AFTER
		 * IT.
		 *
		 * @next_node->prev is a back-channel word of an EXTERNAL
		 * head, and an external carries no state word of its own.  So
		 * the word that excludes a peer here is the HOLDER's.  The
		 * holder is the internal node whose @head_slot chains this
		 * list.  Recording the edge first named no owner at all.  It
		 * left the whole promote lane reading as an exclusion gap.
		 * Taking the fence first makes the holder a REGISTERED lock
		 * on this very txn.  ft_flip_txn_owns then answers with that.
		 *
		 * Safe to move: ft_flip_txn_hold_or_lock_parent returns void.
		 * Its held arm records a RELEASE terminal and registers it.
		 * Its unheld arm acquires-or-guards.  So no bail is
		 * introduced, and every bail that releases @held_holder
		 * explicitly is still ABOVE this point.
		 *
		 * VALIDATE (§4.B): it also guards the LIVE holder this
		 * head-promote publishes into.
		 */
		ft_flip_txn_hold_or_lock_parent(ft, txn, ctx, parent_nf,
			parent_depth, held_holder, held_snap);
		/*
		 * §9.3's THIRD MEMBER: this publish's SKIP_X dual lands in the
		 * GRANDPARENT's body, so acquire GP.  The promote lane never
		 * took it.  Above per-node spacing, a §4.B guard on GP's OWN
		 * word does not replace the acquire.  ☞ ft_lock_skip_dual_gp.
		 *
		 * A no-op unless a dual will actually be recorded.  @mtxn NULL
		 * mirrors this producer's @rec, which carries none.
		 *
		 * Placed with the holder's fence, ABOVE the arm.  It is a
		 * ft_flip_txn_lock_register, and the arm's contract is "after
		 * the op's LAST register".
		 */
		dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn, parent_nf,
			txn ? txn->mtxn : NULL);
		/*
		 * ☠ A PRODUCER THAT CANNOT VOUCH MUST NOT REACH THE PUBLISH --
		 * AND THE BAIL MUST DO THE COMMIT'S CLEANUP.
		 *
		 * On a MISS the arm used to build the publish with
		 * @dual_gp_held false.  That is an MW record on a slot which
		 * every other producer now parks SW.  The arm then CALLED the
		 * commit and relied on @acquire_miss to make it fail.  This
		 * made the slot's kind depend on the ordering inside a
		 * distant function.  And "the abort makes it harmless" is not
		 * safe to say in any case.  An aborted MW txn is not
		 * side-effect-free: its records are installed and then
		 * settled back.
		 *
		 * ☠ A BARE `return -EAGAIN` HERE WEDGES THE TRIE.  The commit
		 * is also this arm's CLEANUP: ft_flip_txn_commit's miss path
		 * runs ft_flip_txn_destroy, hence
		 * ft_flip_txn_lock_release_all.  Returning without it keeps
		 * the op's OWN locks held.  The retry then misses against
		 * them for ever.  ABLATION-MEASURED on
		 * inv_concurrent_same_key_removes at per-node spacing: bare
		 * bail = timeout at 100% CPU, control = 152/152 rc=0.  That
		 * is why every other -EAGAIN in this function sits AFTER the
		 * commit.
		 *
		 * So release exactly what the commit would.  @held_holder is
		 * NOT released here.  ft_flip_txn_hold_or_lock_parent above
		 * REGISTERED it on @txn, so the destroy drops it.  That is
		 * why this file says every bail releasing it explicitly
		 * stays ABOVE that call.
		 */
		if (txn->acquire_miss) {
			ft_flip_txn_destroy(txn);
			ft_ord_cell_free_unpublished(ft, new_cell);
			return -EAGAIN;
		}
		ft_flip_txn_record_reserved(txn,
			ft_flag_to_metadata(ft, parent_nf),
			(void **) &next_node->prev,
			next_node->prev, new_cell_flag);
		/*
		 * PHASE B, STEP B4 -- THE ARM.  ft_flip_txn_hold_or_lock_parent
		 * above is this arm's LAST ft_flip_txn_lock_register, and the
		 * publish below plants the first record after it.
		 *
		 * ☞ WHAT UNBLOCKED IT is the per-edge @owner_held.  This op
		 * holds the HOLDER (the forward @head_slot's owner) and NOT
		 * the grandparent.  So the SKIP_X dual now records MW on its
		 * own account instead of riding the txn's armed mode.  Before
		 * that, arming here would have SW-parked a word the op never
		 * acquired.  That was reproduced through the PUBLIC API, on a
		 * build whose whole-suite dry run was clean.
		 */
		ft_flip_txn_arm_per_op(ft, txn);
		_ft_publish_to_parent_meta(ft, parent_nf,
			(struct cds_ft_inode_flag **) head_slot,
			(struct cds_ft_inode_flag *) next_node,
			(struct cds_ft_inode_flag *) node,
			/*
			 * ☑ THE HONEST ANSWER, AND IT IS NOW THE ONLY ONE LEFT
			 * THAT CAN SAY NO.  @dual_gp_held is
			 * REGISTERED-or-SHARED.  Both mean the op holds the
			 * word.
			 *
			 * This comment used to add: "but every dual producer
			 * passes false until the LAST one acquires, and then
			 * they flip TOGETHER".  It named ft_node_recompact as
			 * the holdout.  That is discharged:
			 *   - ft_node_recompact's relocation site always HELD
			 *     the word and merely could not be asked.  It
			 *     answers from its own acquire now.
			 *   - The glue publishes are excluded by the FT-wide
			 *     lock.
			 *
			 * ☞ THESE TWO ARMS WERE the last producers that could
			 * answer no: ft_promote_head's ordered-list-ON publish
			 * (here) and its list-OFF twin below.  They did so
			 * exactly when the acquire MISSED: their MW count
			 * equalled FT DUAL-GP EXIT MISS to the record.  The
			 * bail at the acquire above now takes that case.  So
			 * this publish is reached only by an op that HOLDS the
			 * word.
			 *
			 * MEASURED AFTER IT (-DFT_DEBUG_DUAL_SITE, ft_inv,
			 * per-node AND exponential): named_unheld = 0 for
			 * EVERY producer.  6304 misses per per-node leg took
			 * the bail instead of entering the engine.
			 * ⇒ Every record on this slot class is made by an op
			 * that holds it.  That is provable HERE, not from what
			 * a later commit does with it.
			 */
			NULL, new_cell_flag, &rec, /*slot_owner_nf=*/ parent_nf,
			/* see above */ dual_gp_held);
		(void) dual_gp_held;
		n_s = ft_pub_rec_sedges(&rec, sedges);
		/*
		 * Fuse @node's freeze (mark node->next, target preserved) into
		 * the swap commit.  A reader then never sees @node's head
		 * anchor promoted away while @node is still unmarked
		 * (doc §4.B).  The reservation above carries the extra edge.
		 */
		/*
		 * The swap's three cell words are taken inside
		 * ft_ord_cell_swap_publish_multi.  Every producer of this
		 * shape reaches it.  See its header.
		 */
		ft_ch_audit(ft, txn, node);
		ft_hlist_freeze_prepare(ft, ft_flip_txn_handle(txn), node);
		if (ft_ord_cell_swap_publish_multi(ft, old_cell, new_cell,
				sedges, n_s, txn)) {
			/*
			 * Peer won the commit: NOTHING installed.  @old_cell
			 * is still the linked head cell and must NOT be freed.
			 * The fresh @new_cell never published.  Reclaim only
			 * ours and retry from a fresh derivation.
			 */
			ft_ord_cell_free_unpublished(ft, new_cell);
			return -EAGAIN;
		}
		ft_ord_cell_free(ft, old_cell);
	} else {
		/*
		 * List off: the head's prev IS the flagged parent.  The
		 * promoted successor inherits it as a FOLDED edge riding the
		 * swap commit.  It flips ATOMICALLY with the forward head
		 * publish and @node's freeze.  It is never a pre-commit
		 * reader-visible store.  It is never a hand-rolled restore on
		 * a peer-won commit either: the remove-retry skeptic flagged
		 * that as assuming one-remover-per-node.
		 *
		 * The prev readers resolve a parked proxy at the load
		 * (ft_dereference_prev_resolved / ft_node_holder), as in the
		 * cell arm above.  The forward publish's forward-before-parent
		 * check reads the folded INTENDED value, not the
		 * not-yet-stored slot.  A peer latch already parked on either
		 * word aborts the attempt before anything is recorded.
		 */
		struct ft_flip_txn *txn =
			ft_flip_txn_create_bounded(ft, FT_PUB_SEDGE_MAX_EDGES +
				FT_HLIST_FREEZE_MAX_EDGES + 3);	/* +1 §4.B parent guard, +1 prev fold,
								 * +1 the SKIP_X dual GP's release-or-guard */

		void *prev_save;
		void *inherit;

		if (!txn) {
			if (held_holder)
				ft_meta_lock_release(held_holder);
			return -ENOMEM;
		}
#ifdef FT_HLIST_CLAIM_PROMOTE
		/*
		 * §4 STEP B4's DRY RUN.  The SAME knob as the list-on arm
		 * above, because this arm carries the SAME gap.  That is
		 * PROVEN by construction, not inferred.  A 60-line public-API
		 * program (list-off trie, four duplicates of one key, remove
		 * the head) aborts at ft-mutation-helpers.h:3108.  It does so
		 * on a build whose whole-suite dry run was CLEAN.  The suite
		 * never builds a skip-encoded holder here.  That is a
		 * COVERAGE ARTIFACT, not an invariant.
		 */
		ft_flip_txn_claim_per_op_armable(ft, txn);
#endif
		prev_save = rcu_dereference(next_node->prev);
		inherit = rcu_dereference(node->prev);
		if (caa_unlikely(ft_node_flip_proxy(
					(struct cds_ft_inode_flag *) prev_save) ||
				ft_node_flip_proxy(
					(struct cds_ft_inode_flag *) inherit))) {
			ft_flip_txn_destroy(txn);	/* PREPARE state: nothing recorded */
			if (held_holder)
				ft_meta_lock_release(held_holder);
			return -EAGAIN;
		}
		/* The holder's fence FIRST -- see the cell arm above for why. */
		ft_flip_txn_hold_or_lock_parent(ft, txn, ctx, parent_nf,
			parent_depth, held_holder, held_snap);
		/*
		 * §9.3's THIRD MEMBER: this publish's SKIP_X dual lands in the
		 * GRANDPARENT's body, so acquire GP.  The promote lane never
		 * took it.  Above per-node spacing, a §4.B guard on GP's OWN
		 * word does not replace the acquire.  ☞ ft_lock_skip_dual_gp.
		 *
		 * A no-op unless a dual will actually be recorded.  @mtxn NULL
		 * mirrors this producer's @rec, which carries none.
		 *
		 * Placed with the holder's fence, ABOVE the arm.  It is a
		 * ft_flip_txn_lock_register, and the arm's contract is "after
		 * the op's LAST register".
		 */
		dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn, parent_nf,
			txn ? txn->mtxn : NULL);
		/*
		 * ☠ A PRODUCER THAT CANNOT VOUCH MUST NOT REACH THE PUBLISH --
		 * AND THE BAIL MUST DO THE COMMIT'S CLEANUP.
		 *
		 * On a MISS the arm used to build the publish with
		 * @dual_gp_held false.  That is an MW record on a slot which
		 * every other producer now parks SW.  The arm then CALLED the
		 * commit and relied on @acquire_miss to make it fail.  This
		 * made the slot's kind depend on the ordering inside a
		 * distant function.  And "the abort makes it harmless" is not
		 * safe to say in any case.  An aborted MW txn is not
		 * side-effect-free: its records are installed and then
		 * settled back.
		 *
		 * ☠ A BARE `return -EAGAIN` HERE WEDGES THE TRIE.  The commit
		 * is also this arm's CLEANUP: ft_flip_txn_commit's miss path
		 * runs ft_flip_txn_destroy, hence
		 * ft_flip_txn_lock_release_all.  Returning without it keeps
		 * the op's OWN locks held.  The retry then misses against
		 * them for ever.  ABLATION-MEASURED on
		 * inv_concurrent_same_key_removes at per-node spacing: bare
		 * bail = timeout at 100% CPU, control = 152/152 rc=0.  That
		 * is why every other -EAGAIN in this function sits AFTER the
		 * commit.
		 *
		 * So release exactly what the commit would.  @held_holder is
		 * NOT released here.  ft_flip_txn_hold_or_lock_parent above
		 * REGISTERED it on @txn, so the destroy drops it.  That is
		 * why this file says every bail releasing it explicitly
		 * stays ABOVE that call.
		 */
		if (txn->acquire_miss) {
			ft_flip_txn_destroy(txn);
			return -EAGAIN;
		}
		ft_flip_txn_record_reserved(txn,
			ft_flag_to_metadata(ft, parent_nf),
			(void **) &next_node->prev, prev_save, inherit);
		/*
		 * PHASE B, STEP B4 -- THE ARM.  See the list-on arm above.
		 * The per-edge @owner_held makes it legal here too.  A
		 * constructed repro exposed the absence of this arm.
		 */
		ft_flip_txn_arm_per_op(ft, txn);
		_ft_publish_to_parent_meta(ft, parent_nf,
			(struct cds_ft_inode_flag **) head_slot,
			(struct cds_ft_inode_flag *) next_node,
			(struct cds_ft_inode_flag *) node,
			NULL, inherit /* folded prev: intended parent value */, &rec,
			/*
			 * ☑ THE HONEST ANSWER, AND IT IS NOW THE ONLY ONE LEFT
			 * THAT CAN SAY NO.  @dual_gp_held is
			 * REGISTERED-or-SHARED.  Both mean the op holds the
			 * word.
			 *
			 * This comment used to add: "but every dual producer
			 * passes false until the LAST one acquires, and then
			 * they flip TOGETHER".  It named ft_node_recompact as
			 * the holdout.  That is discharged:
			 *   - ft_node_recompact's relocation site always HELD
			 *     the word and merely could not be asked.  It
			 *     answers from its own acquire now.
			 *   - The glue publishes are excluded by the FT-wide
			 *     lock.
			 *
			 * ☞ THESE TWO ARMS WERE the last producers that could
			 * answer no: ft_promote_head's ordered-list-ON publish
			 * (here) and its list-OFF twin below.  They did so
			 * exactly when the acquire MISSED: their MW count
			 * equalled FT DUAL-GP EXIT MISS to the record.  The
			 * bail at the acquire above now takes that case.  So
			 * this publish is reached only by an op that HOLDS the
			 * word.
			 *
			 * MEASURED AFTER IT (-DFT_DEBUG_DUAL_SITE, ft_inv,
			 * per-node AND exponential): named_unheld = 0 for
			 * EVERY producer.  6304 misses per per-node leg took
			 * the bail instead of entering the engine.
			 * ⇒ Every record on this slot class is made by an op
			 * that holds it.  That is provable HERE, not from what
			 * a later commit does with it.
			 */
			/*slot_owner_nf=*/ parent_nf, /* see above */ dual_gp_held);
		(void) dual_gp_held;
		n_s = ft_pub_rec_sedges(&rec, sedges);
		/* Fuse @node's freeze into the structural publish (doc §4.B). */
		ft_ch_audit(ft, txn, node);
		ft_hlist_freeze_prepare(ft, ft_flip_txn_handle(txn), node);
		int cret = ft_flip_status_to_errno(
			ft_ord_cell_flip_into(ft, txn, sedges, n_s));

		if (cret) {
			/*
			 * NOTHING installed.  The folded prev edge was
			 * discarded with the failed commit, and @next_node's
			 * prev still names its predecessor @node.  Nothing to
			 * undo.  -EAGAIN retries from a fresh derivation.
			 * -ENOMEM (an acquire that could not allocate) does
			 * not.
			 */
			return cret;
		}
	}
	return 0;
}

#ifdef FT_ENABLE_TRACING
/*
 * How often the chain arm's DERIVED holder had been re-homed between the
 * unlocked ft_chain_head_holder walk and the acquire.  That is how often the
 * re-derive below actually saves a LOST UPDATE.  A fix that never fires is not
 * a fix.  This counter is how that claim is checked.
 */
unsigned long ft_dbg_unchain_rehomed;
/*
 * The firing count of the ft_unchain_kind re-validation.  It counts how often
 * a peer remove on the same chain had overtaken the caller's ROUTING (member /
 * promote / clear).  The window is between the unlocked derivation and the
 * acquire.
 */
unsigned long ft_dbg_unchain_rerouted;
#endif

/*
 * The ROUTING DECISION _cds_ft_remove_locked took for @node.  It is derived
 * from node->prev / node->next with NOTHING held.  ft_unchain_node
 * re-validates it under the holder lock before any arm trusts it:
 *   INTERIOR  @node is a non-head duplicate (prev is a node): relink past it.
 *   PROMOTE   @node heads its chain and has a successor: promote the successor.
 *   CLEAR     @node heads its chain with no successor.  The holder is an
 *             INTERNAL node.  Neither the ordered list nor rank stats are
 *             on.  Clear the head slot.  Two cases do not route here.  A
 *             compressed holder's emptied chain is a DETACH.  A list-on /
 *             rank-on disappearance is the fused ft_remove_one_commit.
 */
enum ft_unchain_kind {
	FT_UNCHAIN_INTERIOR,
	FT_UNCHAIN_PROMOTE,
	FT_UNCHAIN_CLEAR,
	/*
	 * A CLEAR after the fused collapse DECLINED (a run too long to spell).
	 * The 1-child internal it leaves is the designed product.  So the
	 * residue re-route must not send the op round forever.
	 */
	FT_UNCHAIN_CLEAR_KEEP,
};

/*
 * THE COLLAPSE THE PLAN DID NOT SEE, list-off arm.  The caller picked this
 * plain clear over the fused collapse from UNHELD reads of the holder (its
 * nr_child, its parent).  A peer may have removed the holder's second child,
 * or may have been re-parenting the holder while the plan read.  Clearing the
 * holder's last external then leaves a 1-child keyless internal.
 * cds_ft_verify refuses that in skip mode, and a later remove beneath it
 * livelocks on it.  Seen at exponential spacing, owned-key row.  LTTng: five
 * fused attempts were refused on a peer's lock, the sixth took this arm.
 *
 * Under the holder's lock nr_child is exact.  Re-route, and the retry's plan
 * takes the fused collapse.
 */
static inline
bool ft_unchain_clear_leaves_residue(struct cds_ft *ft,
		enum ft_unchain_kind kind, struct cds_ft_inode_flag *holder)
{
#if defined(FEATURE_FT_SKIP_COMPRESSED) && !defined(FT_DEBUG_PREFIX_CLEAR_RESIDUE)
	struct cds_ft_metadata *hm;

	if (kind != FT_UNCHAIN_CLEAR || !holder ||
	    !ft_group_skip_compressed(ft->group) ||
	    ft_node_external(holder) || ft_node_compressed(holder) ||
	    ft_node_skip_compressed(holder))
		return false;
	hm = ft_flag_to_metadata(ft, holder);
	return ft_meta_nr_child_load(hm) == 1 &&
		ft_parent_node(hm->parent_word) != NULL;
#else
	(void) ft; (void) kind; (void) holder;
	return false;
#endif
}

static
int ft_unchain_node(struct cds_ft *ft, const struct ft_lock_ctx *ctx,
		struct cds_ft_inode_flag *parent_nf, unsigned int parent_depth,
		struct cds_ft_node **head_slot, struct cds_ft_node *node,
		enum ft_unchain_kind kind)
{
	struct cds_ft_metadata *hmeta = NULL;	/* MW LOCK_FINE holder lock */
	uintptr_t hsnap = 0;
	struct cds_ft_node *next_node;

	/*
	 * MW LOCK_FINE (Step A, holder lock): serialise concurrent same-key chain
	 * mutation on the head's IMMEDIATE PARENT -- the trie holder.
	 *
	 * Acquire its node lock BEFORE the chain read below.  A peer mid-flip
	 * parks a proxy on node->next / relinks a neighbour's prev, and this
	 * read would deref that.  Hold the lock across the mutate+commit.
	 *
	 * @parent_nf is the holder for a head op.  An interior detach passes
	 * NULL, so derive the head's holder by walking prev
	 * (ft_chain_head_holder).
	 *
	 * Released after the op:
	 *   - Promote / interior: post-commit-clear.  Their commits leave
	 *     holder->state untouched, so a held fence bit composes.  Promote
	 *     is a slot swap + a LOCK-masking guard.  Interior relinks
	 *     neighbours.
	 *   - Head-no-successor records the {LOCK|s -> s} RELEASE + registers
	 *     it.  So it FUSES with the same-word nr_child--.  A masking guard
	 *     would disagree on expected-old and poison.  The txn owns the
	 *     clear: commit consumes it, abort/destroy auto-clears the
	 *     registered fence.
	 *
	 * Under the FT-wide writer_lock no peer contends, so the acquire never
	 * misses in soak.  FEATURE_FT_FAULT_INJECT drives the -EAGAIN bail
	 * (shared cds_ft_fault_lock_countdown).
	 */
	if (ft->lock_fine) {
		struct cds_ft_inode_flag *lock_nf = parent_nf ? parent_nf :
			ft_chain_head_holder(ft, node);

		/*
		 * ☠ A NULL HOLDER IS A STALE DERIVATION HERE TOO.  This is the
		 * exact twin of the duplicate-append arm in ft-insert.h, down
		 * to the refuted sentence.  The text that stood here said:
		 *   - @node "HAS a holder";
		 *   - NULL came "only by ft-insert's unwind paths on
		 *     UNPUBLISHED nodes, which never reach an unchain";
		 *   - the SAME count: "0 NULL in 491532 ft_chain_head_holder
		 *     calls across ft_unit and ft_inv's three list modes".
		 * One wrong zero was used to justify both asserts.  It is
		 * wrong for one reason: every oracle behind that count gives
		 * each writer a DISJOINT key range.  So no two writers ever
		 * met on one duplicate chain.
		 *
		 * Under LOCK_FINE a same-key cds_ft_insert is concurrent by
		 * contract.  It retires or re-heads this chain between the
		 * descent that produced @node and this derivation.
		 * ft_chain_head_holder then answers NULL.  MEASURED:
		 * inv_concurrent_same_key_append_nolist trips this assert on
		 * an UNPINNED run.  Full parallelism is the amplifier here:
		 * pinning to two cpus HID it.
		 *
		 * So bail retriably, as the two acquire failures just below
		 * already do.  The wrapper re-derives against the current
		 * tree.  Asserting was not merely loud: a release build fell
		 * through to ft_flag_to_metadata(ft, NULL) and acquired on
		 * the result.
		 */
		if (caa_unlikely(!lock_nf))
			return -EAGAIN;
		{
			struct ft_held_anchor h;
			unsigned int lock_depth = parent_depth;

			/*
			 * A DERIVED holder (interior detach, @parent_nf NULL)
			 * carries no depth of its own.  The head's holder IS
			 * the descent's parent (§5.2), so let the window date
			 * it.
			 */
			if (!parent_nf)
				lock_depth = FT_DEPTH_FROM_DESCENT;
			if (lock_depth == FT_DEPTH_FROM_DESCENT &&
					!ft_lock_ctx_depth_of(ft, ctx, lock_nf,
						&lock_depth))
				return -EAGAIN;
			if (ft_acquire_member(ft, ctx, lock_nf,
					ft_flag_to_metadata(ft, lock_nf),
					lock_depth, &h))
				return -EAGAIN;
			/*
			 * A holder the op ALREADY held is protected without a
			 * second mark.  Its release belongs to the acquire
			 * that took it.  So leave @hmeta NULL.  The publish
			 * below then routes to the ordinary guard and records
			 * no second terminal on the one word.
			 */
			if (!h.shared) {
				hmeta = h.lock;
				hsnap = h.lock_snap;
			}
			/*
			 * ★ RE-DERIVE AFTER THE ACQUIRE, for a DERIVED holder.
			 *
			 * @lock_nf came from ft_chain_head_holder
			 * (ft-helpers.h), a walk of @node->prev taken with NO
			 * exclusion held.  A peer that inserts a key EXTENDING
			 * this chain's key re-homes the head onto a fresh
			 * junction in one commit, retiring nothing
			 * (ft-insert.h:2099-2141).  It is the same edit that
			 * doc/design/ft-stale-disposal-predicate.md §5.10
			 * roots the bogus NOT_FOUND in.
			 *
			 * Nothing below re-reads the holder, and the commit
			 * "never touches holder->state".  So a stale @lock_nf
			 * is not arbitrated anywhere.  This op and the next
			 * chain op would hold DIFFERENT words and relink one
			 * chain.  The comment above already names the cost:
			 * "once these become sw it is a LOST UPDATE".
			 *
			 * The engine's own splice-holder acquire does exactly
			 * this check (ft_glue_acquire_splice_holders,
			 * ft-mutation-helpers.h:14748: "re-parented under us:
			 * stale lock" -> -EAGAIN).  It is mirrored here,
			 * INCLUDING its skip on a @shared hold.  A word the op
			 * already held was validated by the acquire that took
			 * it.  Re-testing it here would refuse on an outer
			 * frame's still-correct derivation.
			 *
			 * Release only what THIS acquire took.  @shared and
			 * @txn_owned belong to another owner.  ft_unchain_node
			 * already uses this predicate for @hmeta above, and
			 * the remove wrapper uses it on its own exits.
			 */
			if (!parent_nf && !h.shared &&
			    ft_chain_head_holder(ft, node) != lock_nf) {
#ifdef FT_ENABLE_TRACING
				uatomic_inc(&ft_dbg_unchain_rehomed);
#endif
				if (!h.txn_owned)
					ft_meta_lock_release(h.lock);
				return -EAGAIN;
			}
			/*
			 * ★ RE-VALIDATE THE CALLER'S ROUTING, under the lock.
			 * @kind was chosen from node->prev / node->next read
			 * with NOTHING held (see ft_unchain_kind).  The arms
			 * below trust it:
			 *   - the promote arm has no head slot for a member;
			 *   - the clear arm is legal only for an internal
			 *     holder with the list and rank stats off.
			 *
			 * A peer REMOVE on the SAME chain is in contract under
			 * LOCK_FINE.  Between the derivation and this acquire,
			 * it moves the op across those lines:
			 *   - it removes the head.  @node was a member and is
			 *     now the head; or
			 *   - it removes the only other duplicate, and a
			 *     promote finds no successor.
			 *
			 * MEASURED: two writers removing adjacent duplicates
			 * of one key aborted in under a second in both list
			 * modes.  The first case hit the held arm's
			 * assert(parent_nf).  The second hit the clear lane's
			 * no-dual detector on a compressed holder.  A release
			 * build would have turned the second into a compressed
			 * node with a NULL child.
			 *
			 * Both words are FROZEN here.  Every chain mutation
			 * holds this word, and a commit's lock release settles
			 * AFTER its structural words (ft_flip_txn_commit's
			 * late tag).  So a mismatch is a fact about the trie.
			 * Hand the whole op back and let the wrapper derive
			 * once, cleanly, exactly as the holder re-derive above
			 * does.  A hold this op already had (@h.shared) was
			 * validated by the acquire that took it and is not
			 * released here.
			 */
			{
				bool member = ft_node_external(
					(struct cds_ft_inode_flag *)
					rcu_dereference(node->prev));
				bool succ = ft_node_next(node) != NULL;

				/*
				 * A peer removed @node itself since the
				 * derivation: its MARK is on node->next.
				 * Without this test the interior lane would
				 * unlink a ghost.  Its prev still names a
				 * member, and ft_hlist_del_prepare would read
				 * a marked next as a neighbour.  Hand it back
				 * as gone.  The wrapper's re-derivation
				 * answers NOT_FOUND from the top-of-op
				 * tombstone test.
				 */
				if (ft_node_is_removed(node)) {
					if (!h.shared && !h.txn_owned)
						ft_meta_lock_release(h.lock);
					return -ENOENT;
				}
				if ((kind == FT_UNCHAIN_INTERIOR) != member ||
				    (kind == FT_UNCHAIN_PROMOTE && !succ) ||
				    ((kind == FT_UNCHAIN_CLEAR ||
				      kind == FT_UNCHAIN_CLEAR_KEEP) && succ) ||
				    ft_unchain_clear_leaves_residue(ft, kind,
						parent_nf)) {
#ifdef FT_ENABLE_TRACING
					uatomic_inc(&ft_dbg_unchain_rerouted);
#endif
					if (!h.shared && !h.txn_owned)
						ft_meta_lock_release(h.lock);
					return -EAGAIN;
				}
				/*
				 * ☠ THE HEAD SLOT IS A PLAN READ TOO.  The
				 * caller found @node in @head_slot before this
				 * lock.  The promote / clear below records
				 * that slot with @node as its expected-old.
				 * That is a blind SW store now that the holder
				 * is HELD.
				 *
				 * A peer insert of a key extending @node's
				 * republishes the slot IN PLACE under this
				 * very lock.  A compressed holder's child
				 * becomes a junction carrying @node's chain as
				 * its prefix head.  @node itself is untouched,
				 * so the checks above all pass.  The promote
				 * then overwrote the junction, and the peer's
				 * key with it.  MEASURED:
				 * inv_prefix_dup_promote_vs_extension, 16 of
				 * 16 runs.  Re-read the slot here.
				 */
#ifndef FT_DEBUG_NO_HEAD_SLOT_RECHECK
				if (head_slot && kind != FT_UNCHAIN_INTERIOR &&
				    (struct cds_ft_node *) ft_node_ptr(
					ft_resolve_flip_proxy(
					(struct cds_ft_inode_flag *)
					rcu_dereference(*head_slot))) != node) {
					if (!h.shared && !h.txn_owned)
						ft_meta_lock_release(h.lock);
					return -EAGAIN;
				}
#endif
			}
		}
	}

	/*
	 * ☑ IS @node's CHAIN RETIRED, though @node itself is not?  Asked HERE:
	 * after the lock_fine acquire block and before any edge is derived.
	 * This is the one point where the prev walk is stable in BOTH writer
	 * modes.  FINE holds the chain holder by now.  COARSE holds the
	 * FT-wide writer lock throughout.
	 *
	 * ☠ INSIDE the `if (ft->lock_fine)` block it does not run under COARSE
	 * at all, and COARSE then has NO protection.  A whole-chain
	 * displacement freezes only the HEAD (a txn is bounded, a chain is
	 * not).  So a member of a displaced chain is retired and UNMARKED.
	 * The interior lane derives @pred from a stale prev.  The edge it
	 * records has an expected-old the slot can never hold: 50001 laps and
	 * a memcg kill.  Measured: inv_concurrent_insert_replace_coarse died
	 * at ft_inv test 152.
	 *
	 * ☠ AND AT THE ENTRY ARM IT IS UNHELD, which is worse than useless.
	 * The walk then catches the window where a head remove has marked H
	 * but not yet relinked the members onto the promoted successor.  It
	 * calls a chain that is merely losing its head "retired".  That is a
	 * FALSE NOT_FOUND that LOSES A KEY (inv_concurrent_same_key_removes,
	 * count_keys 65 != 64).  ft_chain_head_is_removed says HOLDER-LOCK
	 * CALLERS ONLY for this reason.
	 *
	 * -ESTALE, not -ENOENT.  The wrapper routes -EAGAIN/-ENOENT to RETRY.
	 * A retry only terminates if the ENTRY arm can answer the same
	 * question, and it cannot.  -ESTALE is definitive and answers
	 * NOT_FOUND.
	 */
	if (ft_chain_head_is_removed(node)) {
		if (hmeta)
			ft_meta_lock_release(hmeta);
		return -ESTALE;
	}

	next_node = ft_node_next(node);

	FT_TP(unchain_node, (const void *) head_slot, (const void *) node,
		!ft_node_external((struct cds_ft_inode_flag *) node->prev));
	if (ft_node_external((struct cds_ft_inode_flag *) node->prev)) {
		/*
		 * Non-head interior duplicate (prev is a cds_ft_node): atomic
		 * detach.  ft_hlist_del_prepare does three things in ONE
		 * commit:
		 *   - it freezes @node (marks node->next);
		 *   - it relinks the chain past it (prev_node->next: node ->
		 *     next_node);
		 *   - it fixes the back-link (next_node->prev: node ->
		 *     prev_node).
		 * Freeze and unlink land together (doc §4.B).  So once this
		 * is a concurrent engine, a racing del(node) /
		 * insert_after(node) fails its old-value check.
		 *
		 * Multi-edge: a reader resolves the transient interior-next
		 * proxies via cds_ft_node_next_rcu.  This subsumes the common
		 * ft_node_mark_removed freeze below, so it returns directly.
		 * Abortable cleanly: on a txn-alloc OOM nothing is recorded
		 * or published and @node stays fully chained.
		 */
		struct ft_flip_txn *txn =
			ft_flip_txn_create_bounded(ft, FT_HLIST_DEL_MAX_EDGES);
		enum urcu_txn_status st;

		if (!txn) {
			if (hmeta)
				ft_meta_lock_release(hmeta);
			return -ENOMEM;
		}
		ft_ch_audit(ft, txn, node);
		if (ft_hlist_del_prepare(ft, ft_flip_txn_handle(txn), node)) {
			/*
			 * Peer conflict observed at prepare time (@node or a
			 * neighbour mid-deletion).  Nothing was installed:
			 * records never install without a commit.  Drop the
			 * txn and retry from a fresh position derivation.
			 */
			ft_flip_txn_destroy(txn);
			if (hmeta)
				ft_meta_lock_release(hmeta);
			return -EAGAIN;
		}
		st = ft_flip_txn_commit(ft, txn);
		/*
		 * The interior relink never touches holder->state.  The held
		 * fence is independent of this commit, so drop it directly on
		 * every outcome.
		 */
		if (hmeta)
			ft_meta_lock_release(hmeta);
		if (st < 0)
			return -ENOMEM;
		/* ABORT: peer won, nothing installed, @node fully chained. */
		return st > 0 ? -EAGAIN : 0;
	} else if (next_node) {
		/*
		 * Head with a successor: prev is the cell flag (list on) or
		 * the flagged parent (list off).  Promote @next_node via a
		 * fresh cell swap fused with the structural publish.  @node's
		 * freeze is fused into that same commit (doc §4.B).  A
		 * list-on promotion allocates the fresh cell.  On OOM it
		 * aborts before any reader-visible change: @node stays
		 * chained, not tombstoned.  The caller maps the failure to
		 * CDS_FT_STATUS_MEMORY_ERROR.
		 */
		/*
		 * Hand the held fence to ft_promote_head.  It records the
		 * RELEASE.  The RELEASE composes with its holder guard, which
		 * a still-held LOCK would otherwise self-abort.
		 * ft_promote_head OWNS the fence outcome:
		 *   - commit consumes it;
		 *   - an abort/destroy auto-clears the registered fence;
		 *   - every early-fail path clears it directly.
		 * No post-commit clear here.
		 */
		return ft_promote_head(ft, ctx, parent_nf, parent_depth,
			head_slot, node,
				next_node, hmeta, hsnap);
	} else {
		/*
		 * Head with no successor: the key disappears.  This is only
		 * reached for a list-off internal external_nodes chain that
		 * empties.  A list-on key disappearance routes through the
		 * fused ft_remove_one_commit.  A compressed / body-slot leaf
		 * routes through ft_detach_node.
		 *
		 * Clear the head slot (+ a compressed holder's SKIP_X dual).
		 * Fuse @node's freeze into that same flip-txn (doc §4.B).  The
		 * freeze marks node->next: NULL -> MARK(NULL), preserving the
		 * end-of-chain target.
		 *
		 * Abortable: the flip IS the op's commit, with no pre-flip
		 * reader-visible side-effect.  So on a txn-alloc OOM the
		 * unchain aborts cleanly: nothing published, @node still
		 * chained.  The caller rolls back the -1 count and returns
		 * MEMORY_ERROR.
		 *
		 * Folding the freeze makes this a multi-edge commit (freeze +
		 * >=1 structural).  So it always allocates a txn: there is no
		 * lone-edge on-stack path.  That is one extra alloc per head
		 * clear, the single-writer cost of the atomic detach.
		 */
		struct ft_pub_rec rec = { .ctx = ctx, .n = 0 };
		struct ft_ord_cell_edge sedges[2] = { 0 };
		struct ft_flip_txn *txn;
		unsigned int n_s;

		txn = ft_flip_txn_create_bounded(ft, FT_PUB_SEDGE_MAX_EDGES +
			FT_HLIST_FREEZE_MAX_EDGES +
			1 /* §4.B parent guard */ +
			1 /* the SKIP_X dual GP's release-or-guard */);
		if (!txn) {
			/* Early fence held but not yet handed to the txn. */
			if (hmeta)
				ft_meta_lock_release(hmeta);
			return -ENOMEM;
		}
#ifdef FT_HLIST_CLAIM
		/*
		 * §4 STEP B5's DRY RUN.  It has its OWN knob, apart from the
		 * two promote arms.  This lane does NOT carry their SKIP_X
		 * dual gap, and a shared knob would hide that behind their
		 * abort.
		 */
		ft_flip_txn_claim_per_op_armable(ft, txn);
#endif
		/*
		 * VALIDATE (§4.B): guard the LIVE holder this head-clear
		 * publishes into.  When holding its lock (hmeta), record the
		 * {LOCK|s -> s} RELEASE + register.  It then FUSES with the
		 * fused nr_child-- on the same word.  A masking guard would
		 * disagree on expected-old and poison the descriptor.  The
		 * txn owns the fence outcome: commit consumes it, and an
		 * aborted/destroyed commit auto-clears the registered fence.
		 * So the flip_into abort below needs no manual clear.
		 */
		ft_flip_txn_hold_or_lock_parent(ft, txn, ctx, parent_nf,
			parent_depth, hmeta, hsnap);
		/*
		 * §9.3's third member, for the same reason as the promote
		 * arms.  It is a NO-OP today, by the routing that the detector
		 * below asserts.  A compressed holder never reaches the head
		 * clear.  So no dual is recorded and the helper's own
		 * condition returns early.  It is wired anyway, because that
		 * detector exists precisely because "a ROUTING INVARIANT IS A
		 * CODE FACT, AND CODE MOVES".
		 * ☞ ft_lock_skip_dual_gp.
		 */
		/*
		 * ☞ KEEP THE ANSWER even though this lane emits no dual today.
		 * It costs nothing.  If the routing above changes, the publish
		 * will already spell its own exclusion, not a constant that
		 * nobody re-examines.  Measured: ft_unchain_node has NO row in
		 * the FT_DEBUG_DUAL_SITE census.  That is the detector's claim
		 * restated as a number.
		 */
		{
			bool dual_gp_held = ft_lock_skip_dual_gp(ft, ctx, txn,
				parent_nf, txn ? txn->mtxn : NULL);

			_ft_publish_to_parent(ft, parent_nf,
				(struct cds_ft_inode_flag **) head_slot, NULL,
				(struct cds_ft_inode_flag *) node, &rec,
				dual_gp_held);
		}
		n_s = ft_pub_rec_sedges(&rec, sedges);
		/*
		 * THIS LANE EMITS NO SKIP_X DUAL, and the arm below depends on
		 * it, so a DETECTOR says so rather than a comment.
		 *
		 * The dual arm of _ft_publish_to_parent is gated on
		 * ft_node_compressed(@parent_nf).  This branch is only
		 * reachable with @parent_nf NULL (interior relink) or
		 * INTERNAL.  _cds_ft_remove_locked routes a COMPRESSED holder
		 * by whether @node has a successor:
		 *   - none goes to ft_detach_node;
		 *   - one or more goes to the PROMOTE arm above.
		 * So the head clear never sees a compressed holder, and the
		 * remaining call sites pass an internal holder_flag.  (The
		 * branch comment naming "a compressed holder's SKIP_X dual"
		 * is stale.)
		 *
		 * ☠ A ROUTING INVARIANT IS A CODE FACT, AND CODE MOVES.  The
		 * two promote arms sit one page away and DO emit the dual into
		 * a grandparent they never acquire.  A clean dry run did not
		 * catch that there, so this lane's arm does not rely on one
		 * either.  Armed under --enable-rcu-debug, which the gate
		 * runs at three spacings across four configs.
		 */
		urcu_assert_debug(n_s < 2);
		/*
		 * PHASE B, STEP B5 -- THE ARM.  ft_flip_txn_hold_or_lock_parent
		 * above is this lane's LAST ft_flip_txn_lock_register.  Its
		 * held arm registers @hmeta and records the release.  Its
		 * unheld arm acquires-or-guards @parent_nf.  The publish
		 * below plants the first record after it.
		 *
		 * ☞ WHAT IT CONVERTS is the head-slot clear, owned by
		 * @parent_nf: the word the fence above holds.  The detector
		 * beside it makes that the WHOLE of what it converts.  With
		 * no dual, the publish is a lone structural edge and there is
		 * no second owner to answer for.
		 *
		 * ☞ READINESS: -DFT_HLIST_CLAIM claims at txn CREATION,
		 * strictly earlier, and is clean on ft_unit and ft_inv
		 * FT_INV_MW=1 both.
		 */
		ft_flip_txn_arm_per_op(ft, txn);
		ft_ch_audit(ft, txn, node);
		ft_hlist_freeze_prepare(ft, ft_flip_txn_handle(txn), node);
		int cret = ft_flip_status_to_errno(
			ft_ord_cell_flip_into(ft, txn, sedges, n_s));

		if (cret)
			/* Nothing installed, @node still chained. */
			return cret;
	}
	return 0;
}

/*
 * Called with RCU read lock held.
 *
 * There are a few cases to cover for delete:
 *
 * 1) The node belongs to a list of external nodes duplicates with two
 *    or more items. Remove the node by unlinking it from its list.
 * 2) There is only one external node within this node's list.
 *    2.1) The node is within an external nodes list for which the list
 *         head is a standalone external nodes pointer. The external
 *         nodes list for this key should be removed. Removing an
 *         external nodes list should prune the entire branch leading to
 *         that list, so that no lookup observes empty internal nodes.
 *         This is done by ft_detach_node(). An internal node is empty
 *         when it has no internal and no external node children, *and*
 *         its associated list of external nodes is empty. Detaching an
 *         internal node with no children but with an associated list of
 *         external nodes replaces it by a pointer to the external nodes.
 *    2.2) The node is within an external nodes list which is associated
 *         with an internal node. Unlink the node from its list, leaving
 *         the external nodes list empty.
 */

#ifdef FT_ENABLE_TRACING
/* How often a NOT_FOUND was withdrawn because the holder had moved. */
unsigned long ft_dbg_rm_stale_holder;
#endif

/*
 * ☠ A REFUSAL IS A CLAIM ABOUT THE TRIE, NOT A RETURN CODE.
 *
 * The four-way branch below picks its arm from @holder_flag, derived from
 * @node->prev far above any mark.  A peer may re-home the head in that
 * window.  Then:
 *   - the branch tests the WRONG node's words;
 *   - the arm's identity compare fails;
 *   - the op answers CDS_FT_STATUS_NOT_FOUND.
 * That is a PERMANENT, key-losing answer to a TRANSIENT race.  MEASURED: at
 * that exact return, a fresh ft_node_holder(@node) says the node IS its
 * holder's head.  The holder is alive and untombstoned.
 *
 * So before conceding, ask the question again.  Suppose the holder moved.
 * Then the refusal was about a node this op no longer has any business
 * reading.  Hand it back to the op's own retry instead of reporting the key
 * gone.  That retry is @need_retry, the escape the dead-forward-holder arm
 * already uses.
 *
 * ☞ HISTORICAL, AND STILL LOAD-BEARING -- READ IT WITH ITS RESOLUTION.  What
 * follows is what this disposition measured WITH THE OLD PREDICATE (a bare
 * pointer double-read).  That is why it was opt-in (-DFT_RM_HOLDER_RECHECK)
 * and DEFAULT OFF.  It is now DEFAULT ON.  The reason: the predicate was the
 * defect, not the retry.  The resolution is at ft_rm_holder_rehomed() below.
 * Each objection recorded here is answered inline.  The A/B ran over 20 seeds
 * of the two-writer reproducer.  The source was identical; only this arm
 * differed:
 *
 * ☠☠☠ AND MULTI-WRITER STRESS SAYS IT IS NOT A FIX.  MEASURED 2026-09-06:
 *
 *   - ft_unit 331/331 and ft_inv LIST-OFF 128/128 pass.  But ft_inv LIST-ON
 *     and the FT_INV_MW concurrent-writer leg BOTH HANG at test 44
 *     (inv_insert_replace_splice_window, rc=124 after `ok 43`).  It is the
 *     very test the acquire-first arm (since removed) hung.  The same
 *     alternation is moved up to the wrapper:
 *       - the tombstone arm runs a DESCENT precisely when @node->prev is
 *         lazily stale;
 *       - so @holder_flag is descent-derived, while @fresh here is
 *         prev-derived;
 *       - the two disagree PERMANENTLY;
 *       - every attempt reports "moved", and the op never converges.
 *     Test 44 serialises its writers, so that hang is never contention.
 *
 *     ☑ RESOLVED, and this paragraph is its own proof.  The permanently
 *     disagreeing population is EXACTLY the one where @node->prev is lazily
 *     stale.  That means the holder it names is TOMBSTONED.  That is WHY the
 *     tombstone arm ran at all.  ft_rm_holder_rehomed() returns false on a
 *     tombstoned @fresh.  So that population never retries, and the
 *     alternation cannot form.  The gate ran with it default ON:
 *       - 17 configs, ft_inv on/off/mw 128/128 everywhere, test 44 included;
 *       - plus five configs at all three lock spacings.
 *
 *   - ☠ AND THE OBVIOUS GATE TRADES THE CURE FOR THE HANG.  The gate adds
 *     `!have_descent &&` here: ask @prev only about a holder that came from
 *     @prev.  It clears BOTH hangs: ft_unit 331/331, ft_inv on/off/mw
 *     128/128, all rc=0.  But RM-FAIL then reads 21/30 on the two-key
 *     reproducer, against 21/30 for the control.  THE CURE IS GONE.  So the
 *     cure and the hang are ONE code path.  This arm withdraws the refusal
 *     only by retrying.  It retries on exactly the condition that does not
 *     always terminate.
 *
 *     ☞ Which is why a bare double-read cannot be the fix.  It re-reads
 *     @node->prev with NO LOCK HELD, so it stabilises nothing and compares
 *     across two derivations.  A sound version must take the candidate's lock
 *     FIRST and re-read @prev under it -- prev against prev, never prev
 *     against a descent -- so that "unchanged" means "cannot change".
 *
 *     ☠ THAT PRESCRIPTION WAS REFUTED BY MEASUREMENT (§5.20c).  A load -> lock
 *     -> re-load of ft_node_holder() fires ZERO times in 9.7 M evaluations
 *     (ft_unit + ft_inv on + mw + the reproducer).  It was positive-controlled
 *     to the unit.  For the !have_descent population, the back edge simply
 *     never moves.  For the descent population, the compare is unequal BY
 *     CONSTRUCTION.  A lock cannot stabilise what was never moving.  What the
 *     predicate needed was not stability but the LIVENESS of @fresh.
 *
 *     RM-FAIL (the shape it targets)   11/20  ->   1/20
 *     SEGV                              2/20  ->   2/20   (unchanged: it does
 *                                                          NOT create these)
 *     timeout or memcg kill             0/20  ->   5/20   ☠
 *
 * So the diagnosis is confirmed: withdrawing the refusal all but eliminates
 * the shape.  But handing the op back to @need_retry SPINS.  The caller's loop
 * is an unbounded for(;;).  Under sustained churn @node->prev keeps moving.
 * So each retry re-derives a holder that has moved again.
 * -DFT_DEBUG_REMOVE_RETRY_CAP does NOT catch it.  It reports at
 * 100/1000/10000 attempts, and the arm never passes 100.  The failure is
 * wall-clock and RSS, not attempt count.
 *
 * ☞ WHAT IT NEEDS is one of two bounds:
 *   - a BOUNDED disposition: re-derive and re-run the arm selection IN PLACE
 *     a bounded number of times, rather than restarting the op;
 *   - or a bound on the outer loop.
 * Conceding NOT_FOUND after a bound is still strictly better than today.
 * Today the wrong answer is unconditional.  That is a contract question (may
 * a remove refuse?) and is not settled here.
 */
/*
 * ★ WHAT THE BARE DOUBLE-READ LACKED WAS A PREDICATE, NOT A BOUND.
 *
 * Everything measured above stands, and it indicts the PREDICATE.
 * `fresh != holder_flag` folds TWO causes together and only one is transient:
 *
 *  - @fresh is TOMBSTONED.  The back edge is merely STALE: it names a RETIRED
 *    holder the forward descent above already replaced.  That is PERMANENT.
 *    No retry changes it.  It is also UNCONDITIONAL for the whole
 *    tombstone-arm population.  That arm deliberately replaces @holder_flag
 *    with a descent result, while @node->prev goes on naming the retired
 *    one.  ☠ THAT is the 5/20 wall-clock/RSS spin recorded above.  The old
 *    predicate fired on every single lap.  So no bound was ever needed.
 *    Testing liveness removes the spinning population outright.
 *
 *  - @fresh is LIVE.  A peer RE-HOMED @node under a different, live holder.
 *    An insert of a key that EXTENDS @node's key converted @node's leaf into
 *    a PREFIX HEAD in place.  It chained @node into the new head's
 *    external_nodes (ft-insert.h, ft_attach_node -- "the external node we
 *    are replacing at the attachment location ... chain this external node
 *    in the topmost internal node external node list").  @node is LIVE and
 *    REACHABLE one hop lower, so refusing is the §1 key loss.  This IS
 *    transient.  It is charged to that peer's COMMITTED conversion.  The
 *    retry:
 *      - re-derives against a tree that has already moved on;
 *      - reads @node->prev = the head (live, so the tombstone arm does not
 *        fire);
 *      - and the branch reaches @node by its own external-member condition.
 *
 * MEASURED with the liveness test in (doc/design/ft-stale-disposal-predicate.md
 * §5.24):
 *   - RM-FAIL 2/30 -> 0/30, and 0 across 90 further seeds.  That is 120
 *     seeds, ZERO.
 *   - The spin did NOT return.  Timeouts stayed flat at 9/30, against a
 *     baseline of 8/30.  Suite walls were unchanged (unit 45 s, ion 86,
 *     ioff 86, imw 103).
 *
 * ☞ A genuinely unlinked node is a REAL miss and must NOT retry: tested first.
 */
static inline
bool ft_rm_holder_rehomed(struct cds_ft *ft, const struct cds_ft_node *node,
		struct cds_ft_inode_flag *holder_flag)
{
	struct cds_ft_inode_flag *fresh;

	if (ft_node_is_removed((struct cds_ft_node *) node))
		return false;		/* genuinely gone: the miss is real */
	fresh = ft_node_holder(ft, (struct cds_ft_node *) node);
	if (!fresh || fresh == holder_flag)
		return false;
	if (ft_flag_tombstoned(ft, fresh))
		return false;		/* stale back edge, not a re-home */
#ifdef FT_ENABLE_TRACING
	uatomic_inc(&ft_dbg_rm_stale_holder);
#endif
	return true;
}

/*
 * The other transient miss: the slot came back.  @fresh == @holder_flag says
 * "not re-homed".  It cannot tell a miss from an ABA on the slot itself.
 *
 *   this remove                       peer
 *   -----------                       ----
 *                                     insert a key extending @node:
 *                                     republish @head_slot with a junction
 *                                     carrying @node as its prefix head
 *   identity read: sees the junction
 *                                     remove that key: collapse the
 *                                     junction, put @node back into the
 *                                     same holder's slot
 *
 * Those two commits re-point the back edge and then restore it, so it names
 * @holder_flag again.  @node never left the trie.
 *
 * Measured: two writers toggled "...zone46" / "...zone4" through a compressed
 * holder.  3 of 5 runs refused the live "...zone4" here within 10 s.
 *
 * A live @node back in @head_slot is a retry.  It is charged to the peer's
 * two committed writes.  A removed @node is still the real miss.  A NULL
 * @head_slot means the body arm's byte had no slot at all.  It has nothing to
 * come back to.
 *
 * ☠ This holds only in a live holder.  A retired holder's slots keep whatever
 * they held when it was retired, so its slot "holds @node" forever.  A remove
 * bootstrapped from a stale back edge naming that retired holder would retry
 * on it without end.  Measured: a lost "...zone46" whose remove spun alone at
 * 100% CPU after its peer exited.  ft_rm_holder_rehomed already calls a
 * tombstoned holder the real miss.  This function must agree with it.
 */
static inline
bool ft_rm_slot_regained(const struct cds_ft *ft, const struct cds_ft_node *node,
		struct cds_ft_inode_flag *holder_flag,
		struct cds_ft_inode_flag **head_slot)
{
#ifdef FT_DEBUG_NO_SLOT_REGAINED
	(void) ft; (void) node; (void) holder_flag; (void) head_slot;
	return false;
#else
	if (!head_slot || ft_node_is_removed((struct cds_ft_node *) node) ||
			ft_flag_tombstoned(ft, holder_flag))
		return false;
	return (const struct cds_ft_node *) ft_node_ptr(ft_resolve_flip_proxy(
			rcu_dereference(*head_slot))) == node;
#endif
}

/*
 * ☠ A miss is confirmed under the holder's lock.
 *
 * The identity compare and the two predicates above are unlocked reads.  Each
 * is answered at its own instant.  A peer toggling a key that extends @node
 * can answer each one from a different tree.
 *
 *   this remove                          peer
 *   -----------                          ----
 *                                        publish a junction in the slot
 *   identity compare: sees the junction
 *                                        collapse the junction: @node is
 *                                        back under @holder_flag
 *   back edge read: names @holder_flag
 *                                        publish its next junction
 *   slot read: sees the next junction
 *
 * Each of the three reads is self-consistent.  Yet the verdict is wrong.  It is
 * NOT_FOUND for a key that never left the trie.  Measured: 5 of 32 two-writer
 * runs ("...runs" vs "...runs6" through one compressed holder) refused the live
 * "...runs" at the compressed arm.  A lookup found it both before the
 * remove and after the refusal.
 *
 * The authority is the key's slot in the holder, read under the holder's
 * lock.  It is not @node's back edge.  A back edge is updated lazily.  It can
 * name a retired holder while the forward path still reaches @node (the STALE
 * BACK-EDGE arm in _cds_ft_remove_locked).  While @lock_flag's lock is held,
 * the slot cannot move:
 *
 *  - it holds @node again                     -> retry (the slot came back);
 *  - it holds an INTERNAL node (skip-encoded  -> retry: the key now goes
 *    included: a skip word's low bits read       DEEPER, through a junction
 *    as external, so ask that first)             a peer published here, and
 *                                                 @node may hang below it;
 *  - it is empty, or holds a DIFFERENT leaf   -> @node is not at its key's
 *                                                 position: the miss is real,
 *    unless the back edge names a LIVE other holder (a re-home this compare
 *    cannot see), which is a retry.
 *
 * The slot is re-derived under the lock: from @key_byte, or from the
 * compressed @lock_flag's child when @key_byte is negative.  A packed body
 * shifts its slots in place, so a pre-lock slot address is itself a plan
 * read.  The lock is taken and dropped here, with nothing built or reserved.
 * This is not the op-wide hold measured and refuted above.
 *
 * Return true when the op must retry.
 */
static inline
bool ft_rm_miss_unconfirmed(struct cds_ft *ft, const struct ft_lock_ctx *lctx,
		const struct cds_ft_node *node,
		struct cds_ft_inode_flag *holder_flag,
		struct cds_ft_inode_flag *lock_flag,
		struct cds_ft_metadata *lock_meta, unsigned int holder_depth,
		int key_byte)
{
#ifdef FT_DEBUG_NO_MISS_UNDER_LOCK
	(void) ft; (void) lctx; (void) node; (void) holder_flag;
	(void) lock_flag; (void) lock_meta; (void) holder_depth; (void) key_byte;
	return false;
#else
	struct ft_held_anchor h = { 0 };
	struct cds_ft_inode_flag *cur, **slot = NULL, *fresh;
	bool retry;

	if (!ft->lock_fine ||
			ft_node_is_removed((struct cds_ft_node *) node))
		return false;
	/*
	 * A retired holder decides nothing.  Its slots keep what they held.
	 * @node may live on in its replacement, behind a back edge that is
	 * only lazily updated.  Re-derive.  The retry's STALE BACK-EDGE arm
	 * descends for the live holder by key.  A miss is then confirmed under
	 * that holder's lock.  That word is live, so the retry terminates.
	 */
	if (ft_flag_tombstoned(ft, lock_flag))
		return true;
	/*
	 * Refused: a peer holds the word, or retired it after the test above.
	 * In both cases the tree is moving under the verdict.  Re-derive.
	 */
	if (ft_acquire_member(ft, lctx, lock_flag, lock_meta, holder_depth, &h))
		return true;
	if (ft_node_is_removed((struct cds_ft_node *) node)) {
		retry = false;
	} else {
		if (key_byte < 0)
			cur = ft_compressed_node_ptr(lock_flag)->child;
		else
			cur = ft_node_get_nth_skip(holder_flag, &slot,
				(uint8_t) key_byte, FT_PF_NONE);
		cur = ft_resolve_flip_proxy(cur);
		if (cur && (ft_node_skip_compressed(cur) ||
				!ft_node_external(cur) ||
				(const struct cds_ft_node *) ft_node_ptr(cur) ==
					node)) {
			retry = true;
		} else {
			fresh = ft_node_holder(ft, (struct cds_ft_node *) node);
			retry = fresh && fresh != holder_flag &&
				!ft_flag_tombstoned(ft, fresh);
		}
	}
	if (!h.shared && !h.txn_owned)
		ft_meta_lock_release(h.lock);
	return retry;
#endif
}

static
enum cds_ft_status _cds_ft_remove_locked(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node *node,
		bool *need_retry,
		struct ft_op *op)
{
	/*
	 * Anchor source for the op's lock-sets, populated only where a descent
	 * ran (@have_descent).  Under per-node granularity none does: every
	 * member anchors on itself, so no depth is needed.
	 */
	struct ft_descent d;
	bool have_descent = false;
	unsigned int holder_depth = 0;
	struct cds_ft_inode_flag *holder_flag;
	struct cds_ft_metadata *holder_meta;
	struct cds_ft_inode_flag **head_slot = NULL;
	const uint8_t *iter_key;
	size_t key_len = ft_key_len(ft, ft_iter_resolve_key_len(iter));
	/*
	 * Poisoned, not zeroed.  Every path assigns @ret today, and gcc's
	 * -Wmaybe-uninitialized agrees.  But the terminal switch below ends in
	 * `default: abort()`.  A path added later that forgets to assign @ret
	 * would decide the op's whole outcome from stack garbage:
	 *
	 *  - garbage that lands on 0 reports CDS_FT_STATUS_OK for a removal
	 *    that may not have happened.  @2d3b92ea fixed that defect in
	 *    remove_all;
	 *  - any other value aborts or not, depending on the frame.
	 *
	 * -EINVAL is deliberately not a case in that switch.  An unassigned
	 * @ret therefore reaches `default:` deterministically, not by luck.
	 * The unknown is outside the domain and the consumer already refuses
	 * it.
	 */
	int ret = -EINVAL;

	FT_TP(remove_enter, (const void *) ft, (const void *) iter,
		iter_key(iter), key_len);

	/*
	 * If the iterator has a valid path, the RCU read-side lock must
	 * be held.
	 */
	if (iter->cache_valid)
		CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED(ft);
	iter_debug_path_check(iter);

	if (!valid_external_node(node) || !valid_key_len(ft, key_len)) {
		FT_TP(remove_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}

	iter_key = ft_iter_read_key(iter);
	dbg_printf("cds_ft_remove attempt: node %p\n", node);

	/*
	 * No top-down descent.  @node is application-owned.  The RCU read-side
	 * lock has been held continuously since @node was obtained, so @node
	 * stays alive.  node->prev names the node's holder, and the slot that
	 * holds @node can be derived from it directly.
	 *
	 * ☠ This is not because a writer mutex freezes anything.  This
	 * comment used to say it did: "the writer mutex held here freezes the
	 * structure, so node->prev is a settled live pointer".  That was true
	 * under CDS_FT_WRITER_LOCK_COARSE.  It is false under the fine-grained
	 * default.  There, cds_ft_remove's own header promises that it may run
	 * concurrently with another remove on the same key.  There is no mutex
	 * here, and a back-pointer is stale at rest, unbounded.  A false
	 * premise on a converted path is worse than an unconverted op that
	 * says so, because nothing marks it as owed.
	 *
	 * ☞ What carries it: the derivation is re-validated under the
	 * holder's lock once that lock is taken.  When the holder moved, the op
	 * bails and can be retried.  This is the work of ft_chain_head_holder's
	 * re-check, of ft_unchain_kind's under-lock routing and of the txn's
	 * own expected-old.  The derivation here is a plan, not a conclusion.
	 */
	/*
	 * The slot derivation itself:
	 *
	 *  - INTERNAL ancestors recover their parent slot from their own
	 *    metadata (parent + parent_slot_offset), used by ft_detach_node's
	 *    upward prune walk.
	 *  - the metadata-less EXTERNAL head's slot in its holder is
	 *    re-derived from the key here.  It is a compressed holder's
	 *    &cn->child, or an internal holder's body slot for the key's last
	 *    byte.
	 */

	/*
	 * A removed node carries the tombstone on node->next (set by a prior
	 * unchain/detach).  Re-removing it is an idempotent miss; this also
	 * guards against operating on a node already unlinked from the trie.
	 */
	if (ft_node_is_removed(node)) {
		dbg_printf("cds_ft_remove: node %p already removed\n", node);
		FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
		ft_dbg_rm_site = __LINE__;
#endif
		return CDS_FT_STATUS_NOT_FOUND;
	}
	/*
	 * A member whose predecessor no longer owns it is equally gone, and
	 * no tombstone anywhere says so.
	 *
	 * The mark above is a per-node fact.  A whole-chain displacement
	 * cannot write it on every member.  cds_ft_insert_replace swings the
	 * anchor slot to a fresh head.  In that commit it freezes only the old
	 * head.  A txn is bounded and a chain is not.  Asking the head does
	 * not work either.  The head is a caller-owned node.  The API permits
	 * re-arming it for reuse (cds_ft_node_init) the moment the chain is
	 * handed back.  That erases the mark the question reads.
	 *
	 * So ask the structure, not a flag: an interior member is in a live
	 * chain iff its predecessor still points at it.  A displaced member's
	 * @prev names a node that has been recycled or relinked.  The next of
	 * that node is NULL or someone else.  Under exactly that condition
	 * the interior lane would record `pred->next: elem -> next` against a
	 * slot that can never hold @elem, and spin forever.  Measured: 50001
	 * attempts, then a memcg kill.  Five single-threaded calls reproduce
	 * it.
	 *
	 * Both loads are proxy-resolved and mark-masked.  A peer mid-commit
	 * therefore reads as the word before or after its commit, never as a
	 * descriptor:
	 *
	 *  - a peer unlinking @pred has already relinked its own predecessor
	 *    at @node;
	 *  - a peer removing @node itself is caught by the mark above.
	 *
	 * The loads are unheld, like the mark test they follow.  Both are
	 * statements about the node the caller handed us, and the answer is
	 * NOT_FOUND either way.
	 */
	{
		void *nprev = ft_dereference_prev_resolved(node);

		/*
		 * ☠ The two loads are not mutually consistent, so the
		 * predicate needs a third one.  @node->prev and @pred->next
		 * are read unheld, and a promotion writes both.  A same-key
		 * cds_ft_remove that retires the head promotes @node in its
		 * place.
		 *
		 *   this remove          peer (same-key cds_ft_remove)
		 *   -----------          -----------------------------
		 *   read @node->prev
		 *                        clear the old head's next
		 *                        re-home @node->prev onto the
		 *                        flagged parent
		 *   read @pred->next
		 *
		 * The structure then says "your predecessor does not point at
		 * you" about a node that is not displaced at all.  It is the
		 * new head.
		 *
		 * Measured, 3/3 runs of inv_concurrent_same_key_append_nolist.
		 * At the refusal @pred->next was NULL.  An immediate re-read
		 * of @node->prev already carried the flagged parent
		 * (tag != external), and @node had no tombstone.  The op then
		 * answered NOT_FOUND, a terminal claim, for a live node.
		 * Sometimes that node still had successors of its own.
		 *
		 * ☠ A false NOT_FOUND is not a harmless refusal.  The caller
		 * is entitled to conclude that the node left the trie.  It
		 * is also entitled to re-arm it (cds_ft_node_init).  The API
		 * permits that the moment the chain is handed back.  That wipes
		 * prev/next on a node that is still linked.  The next
		 * cds_ft_insert of it descends onto itself:
		 * ft_chain_head_holder walks a NULL prev, the append bails
		 * -EAGAIN and re-descends onto the same
		 * node forever.  Measured: 230,977,777 retries in one call.
		 * They ran inside the escalation fallback, which then stalls
		 * every grace period.
		 *
		 * So re-validate @node->prev after the @pred->next load, and
		 * refuse only when it is unchanged.  A genuine displacement is
		 * stable: nobody relinks a displaced member.  A promotion
		 * moves @node->prev off the predecessor exactly when it clears
		 * the predecessor's next.  A prev that moved under us means
		 * the node was relinked, not dropped.  Fall through and let
		 * ft_node_holder re-derive it from the word it now carries.
		 */
		if (nprev &&
		    ft_node_external((struct cds_ft_inode_flag *) nprev) &&
		    ft_hlist_next_rcu((struct cds_ft_node *) nprev) != node &&
		    ft_dereference_prev_resolved(node) == nprev) {
			dbg_printf("cds_ft_remove: node %p is not in its predecessor's chain\n",
				node);
			FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
	}

	/*
	 * Resolve @node's holder (its parent).  The cell indirection is
	 * transparent:
	 *
	 *  - a head's prev is its cell (parent in cell->parent);
	 *  - a non-head duplicate's prev is its predecessor.
	 *
	 * NULL => never inserted.
	 */
	holder_flag = ft_node_holder(ft, node);
	if (!holder_flag) {
		/* Never inserted (a freshly-initialized node). */
		dbg_printf("cds_ft_remove: node %p has no parent\n", node);
		FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
		ft_dbg_rm_site = __LINE__;
#endif
		return CDS_FT_STATUS_NOT_FOUND;
	}
	/*
	 * STALE BACK-EDGE (measured: 78% of the cases where this fires).
	 *
	 * The derivation above trusts @node->prev, and a back-pointer is
	 * updated lazily.  A peer that replaced the holder can leave
	 * @node->prev naming the retired old one.  The forward path from the
	 * root still resolves @node correctly.  The op is then derived against
	 * a dead node:
	 *
	 *  - ft_meta_lock_acquire refuses a tombstone word;
	 *  - the caller gets -EAGAIN;
	 *  - the wrapper's retry re-derives the same dead holder forever.  It
	 *    holds the per-trie FIFO fair-mutex turn and denies every other
	 *    writer.
	 *
	 * Measured: 2,000,000+ consecutive attempts.  11 of 12 writer threads
	 * were parked.  Escalation cannot break it: a tombstone is permanent,
	 * not contention.
	 *
	 * The forward path is authoritative.  So re-derive the holder by a
	 * key-guided descent, the same walk ft_detach_at uses, and carry on
	 * with the live parent.  A descent may no longer reach @node.  This is
	 * the minority shape: the key really is gone.  It is an idempotent
	 * miss.  It is reported like the ft_node_is_removed() early-out above.
	 * The op does not spin on it.
	 */
	if (caa_unlikely(ft_flag_tombstoned(ft, holder_flag))) {
		const uint8_t *ik = iter_key;
		struct cds_ft_inode_flag *fwd;
		unsigned int fwd_depth;
		bool prefix;

		ft_anchor_descend(ft, &d, iter_key, key_len, &ik);
		/*
		 * ★ THE DESCENT STOPS IN ONE OF TWO PLACES, and this arm knew
		 * only one of them.  These are the SAME two bullets that the
		 * coarse arm below states and applies (its `prefix` test).  The
		 * key can end AT an internal node whose external_nodes carry
		 * the leaf.  That is a PREFIX KEY.  The holder is then @d.nf at
		 * @d.depth, not @d.pnf.
		 *
		 * ☠ Taking @d.pnf unconditionally loses a key.  A peer may
		 * insert a key that extends @node's.  It converts @node's leaf
		 * into a prefix head in place and retires nothing.  It chains
		 * @node into that head's external_nodes (ft-insert.h,
		 * ft_attach_node).  @node stays live, one hop lower.  Aiming
		 * at the head's parent sends the four-way branch into the
		 * body-slot arm.  The slot of that arm then holds the head,
		 * not @node.  The arm answers a permanent NOT_FOUND for a key
		 * that exists.
		 *
		 * Measured in doc/design/ft-stale-disposal-predicate.md §5.20
		 * and §5.21.  At that refusal @d.nf is the head in 12,450 of
		 * 12,451 samples.  0 were @node.  This `prefix` test fires on
		 * every failing sample (2,226/2,226).  Selecting @d.nf also
		 * makes the op acquire the word its writes touch: the head's,
		 * not its parent's.
		 */
		prefix = d.nf && !ft_node_external(d.nf) &&
			d.depth == key_len;
		fwd = prefix ? d.nf : d.pnf;
		fwd_depth = prefix ? d.depth : d.pdepth;
		/*
		 * ☞ A DEAD FORWARD HOLDER IS A RETRY, NOT A MISS -- the second
		 * half of the same two-arm inconsistency.  The coarse arm below
		 * answers this same predicate with *need_retry.  Its reason is
		 * "nothing is reserved or published yet, so re-derive the whole
		 * position against the settled tree".  This arm answered it
		 * with a PERMANENT NOT_FOUND.  A tombstone on the freshly
		 * descended holder means a peer is retiring it right now.  That
		 * is transient, and the contract forbids refusing for a
		 * transient reason.  Measured: this guard left 2 residual
		 * refusals.  Both were tombstoned(fwd), and neither was !d.nf
		 * (§5.23).
		 */
		if (fwd != NULL && ft_flag_tombstoned(ft, fwd)) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			return CDS_FT_STATUS_OK;	/* wrapper retries */
		}
		/* Only an unreachable key is an idempotent miss. */
		if (!d.nf || fwd == NULL) {
			FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
		holder_flag = fwd;
		holder_depth = fwd_depth;
		have_descent = true;
	}

	/*
	 * The op's lock context.  @d is an anchor source wherever the walk
	 * above actually RAN.  That is every coarse spacing, since the arm that
	 * runs it is gated on exactly that.  @d is NULL under per-node.  There
	 * the walk never runs, and every member anchors on itself anyway.
	 *
	 * ☠ "The walk ran" is not "the walk located the holder".  This flag
	 * used to mean the second.  A holder that the two arms above do not
	 * match then threw a perfectly good anchor table away.  A member dated
	 * by the one-hop rule then reached ft_anchor_meta with no descent to
	 * answer its anchor.  (ft_lock_ctx_depth_of_parent needs no descent to
	 * answer a depth.)  That is the assert, in ft_chain_compress_fused
	 * under CDS_FT_LOCK_SPACING=exponential.
	 *
	 * A descent that does not describe a member reports that per member,
	 * and the caller re-plans.  Withholding the descent turns "this member"
	 * into "every member".
	 */
	struct ft_lock_ctx lctx;

#ifdef FT_DEBUG_WIDEN_OWNER
	if (!have_descent && ft_bulk_active(ft))
		uatomic_inc(&ft_wo_nod_remove_locked);
#endif
	ft_lock_ctx_init(&lctx, have_descent ? &d : NULL, NULL, op);

	/*
	 * ★ DERIVE -> SELECT -> COMPARE window (-DFT_DELAY_INJECT only; a no-op
	 * in every shipping build, fractal-trie-internal.h:4236).
	 *
	 * @holder_flag is now fully derived and nothing is held.  Every word
	 * the four arms below read belongs to that holder: the cell capture,
	 * the kind dispatch and the two identity compares.  A peer insert of a
	 * key that extends @node's converts @node's leaf into a prefix head in
	 * place on that holder.  It flips the holder's slot and @node's
	 * back-edge in one commit.  It retires nothing.  See §5.10 of
	 * doc/design/ft-stale-disposal-predicate.md for this.  Measured on the
	 * wire (§5.12), that window is ten nanoseconds wide.  This is why
	 * the defect needs 80 seeds of a concurrent rig to show up 53 times.
	 *
	 * The delay is placed after the whole derivation and before the first
	 * read of a holder-owned word.  That is exactly the epoch boundary the
	 * defect straddles.
	 *
	 * ☠ Measured not to help for this defect.  This is recorded so that
	 * the next reader does not repeat it.  Against the rig's DETERM=1 case,
	 * 10 seeds each:
	 *
	 *     no delay        13/20 fired, mean 253 ms
	 *     FT_DELAY_US=1    6/10
	 *     FT_DELAY_US=5    5/10
	 *     FT_DELAY_US=20   5/10
	 *     FT_DELAY_US=200  3/5, two runs clean for a full 10 s
	 *
	 * Widening the window makes it worse.  The peer inserts the extension
	 * and removes it again, both inside one sleep.  The holder's slot is
	 * then back to @node by the time the compare runs.  The race heals
	 * itself under a long enough delay.  What buys the speed is the key
	 * shape (DETERM=1), not the timing.
	 */
	ft_delay_writer();

	/*
	 * Cell-always: @node heads its chain iff its prev is the cell (not an
	 * external predecessor).  Capture the head's cell + successor before
	 * the unlink:
	 *
	 *  - a head promotion retargets the cell at the successor (done in
	 *    ft_unchain_node);
	 *  - a key disappearance (no successor) frees the cell after the
	 *    removal commits (ret == 0).
	 */
	/*
	 * Load node->prev once, resolved, and load the successor through the
	 * hlist resolver.  Nothing is held here.  A peer remove on the same
	 * chain can have a flip proxy parked on either word:
	 *
	 *  - its promote records the successor's prev;
	 *  - its member unlink records the successor's next and the
	 *    predecessor's next.
	 *
	 * Two raw loads of prev could disagree with each other and with the
	 * resolved holder derivation above.  A raw ft_ord_cell_ptr() of a
	 * parked proxy is a wild cell.  Resolved, each value is the word
	 * before or after that peer's commit, never a descriptor.  The routing
	 * can still be stale but valid.  ft_unchain_node's under-lock
	 * re-validation (ft_unchain_kind) catches that and retries.
	 */
	void *node_prev = ft_dereference_prev_resolved(node);
	/*
	 * Read the successor once, for both decisions that depend on it:
	 *
	 *  - the lane dispatch below: a head with a successor is promoted,
	 *    one without is pruned / cleared;
	 *  - the fuse decision here: a key that disappears takes its cell
	 *    with it.
	 *
	 * Read twice, at two instants, the two disagreed under a same-key
	 * peer:
	 *
	 *  - A successor was appended between the reads.  A "sole entry"
	 *    (@fuse_remove, unsplice txn reserved) then went down the PROMOTE
	 *    lane, which swapped the head's cell for a fresh one.  The
	 *    two-commit tail then drove the unsplice of the swapped-out cell
	 *    forward forever, because its expected-olds can never match.
	 *    Measured as a memcg kill within seconds.
	 *  - A successor was removed between the reads.  This is the mirror
	 *    image: a key was pruned with its cell left in the list
	 *    (ft_verify "ord-cell list longer than trie").
	 *
	 * The lanes then catch a stale value of this one read.  The promote's
	 * under-lock re-validation (ft_unchain_kind) and the sole-entry
	 * freeze's derived expected-old both retry the op.
	 */
	struct cds_ft_node *succ_node = ft_hlist_next_rcu(node);
	bool cell_was_head = ft->ordered_list &&
		!ft_node_external((struct cds_ft_inode_flag *) node_prev);
	struct ft_ord_cell *dead_cell = cell_was_head ?
		ft_ord_cell_ptr(node_prev) : NULL;
	struct cds_ft_node *cell_succ = cell_was_head ? succ_node : NULL;
	/*
	 * Key-disappearing detach (head with no successor) + ordered list on:
	 * fuse the structural unlink with @dead_cell's unsplice in one flip.
	 * @pub.armed reports whether ft_detach_node fused it (in-place leaf
	 * delete) or left it for the two-commit fallback below.
	 */
	bool fuse_remove = cell_was_head && !cell_succ;
	struct ft_remove_pub pub = { .armed = false };
	struct ft_remove_pub *pubp = fuse_remove ? &pub : NULL;
	struct ft_ord_cell *fuse_cell = fuse_remove ? dead_cell : NULL;
	/*
	 * Pre-reserve the dead cell's standalone-unsplice txn here, in the
	 * fallible prefix, before any structural change.  The structural
	 * commit cannot always fuse the unsplice: a recompaction /
	 * external-promote shape leaves pub unarmed.  The unsplice then runs
	 * after that commit is public and cannot abort, so its flip-txn must
	 * already be reserved.  On OOM here the removal aborts cleanly (key
	 * untouched).  The fused common case frees the txn unused at the tail.
	 */
	struct ft_flip_txn *unsplice_txn = NULL;

	if (fuse_remove) {
		unsplice_txn = ft_flip_txn_create_bounded(ft,
			FT_ORD_CELL_UNSPLICE_MAX_EDGES);
		if (!unsplice_txn) {
			FT_TP(remove_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	if (ft_node_external(holder_flag)) {
		/*
		 * node->prev is a cds_ft_node: @node is a non-head duplicate.
		 * Unlink it from its chain (ft_unchain_node relinks the pinned
		 * predecessor/successor and tombstones @node).  The key count is
		 * unchanged, because other duplicates remain.  The chain head
		 * is untouched, and so is any grandparent skip pointer to it.
		 * So no head slot is needed.
		 */
		ret = ft_unchain_node(ft, &lctx, NULL, 0, NULL, node,
			FT_UNCHAIN_INTERIOR);
	} else if (ft_node_compressed(holder_flag) ||
		   ft_node_skip_compressed(holder_flag)) {
		/*
		 * Compressed holder: @node is its single external child
		 * (cn->child).  Leaf key.
		 */
		struct cds_ft_compressed_node *cn;

		/* Flight-recorder mis-wire detector (no-op without FT_ENABLE_TRACING). */
		FT_TRACE_MISWIRE(ft, holder_flag, 2);
		cn = ft_node_skip_compressed(holder_flag) ?
			ft_skip_to_compressed(ft, holder_flag) :
			ft_compressed_node_ptr(holder_flag);

		holder_meta = cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
		head_slot = &cn->child;
		/*
		 * Resolve a peer's mid-commit flip proxy before the identity
		 * compare (§9): a raw proxy value would spuriously mismatch a
		 * slot that still resolves to @node.  A genuine post-resolve
		 * mismatch (a peer republished the holder) is caught by the
		 * commit's expected-value CAS / §4.B guard as ABORT -> retry.
		 */
		if ((struct cds_ft_node *) ft_node_ptr(ft_resolve_flip_proxy(
				rcu_dereference(*head_slot))) != node) {
			dbg_printf("cds_ft_remove: node %p not at compressed child slot\n", node);
			/* Drop the pre-reserved unsplice txn (nothing published yet). */
			if (unsplice_txn)
				ft_flip_txn_destroy(unsplice_txn);
			/*
			 * RE-HOMED UNDER A LIVE HOLDER -> RETRY THE WHOLE OP.
			 * See ft_rm_holder_rehomed: the liveness test is what
			 * makes this terminate where the bare double-read spun.
			 */
			if (ft_rm_holder_rehomed(ft, node, holder_flag) ||
					ft_rm_slot_regained(ft, node, holder_flag,
						head_slot) ||
					ft_rm_miss_unconfirmed(ft, &lctx, node,
						holder_flag,
						ft_compressed_node_flag(cn),
						holder_meta, holder_depth, -1)) {
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;
			}
			FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
		if (!succ_node) {
			/*
			 * Last/only entry: prune the now-empty branch.
			 * ft_detach_node bootstraps from the holder slot (recovered
			 * from the holder's own metadata offset) and walks up via
			 * metadata->parent.  Propagate -1 before detach, which may
			 * free internal nodes.
			 */
			/*
			 * Name the derivation.  The freeze this detach fuses is
			 * given a literal length, so it never passes through
			 * ft_hlist_chain_len.  The tail-why classifier filed it
			 * as "(unstamped)".  That bucket has to be reasoned
			 * into a site.  It cannot be read off the table.  The
			 * literal is derived here, from the @succ_node read
			 * above, so stamp it here.
			 */
			FT_HLIST_WHY_STAMP();
			ret = ft_detach_node(ft, &lctx, head_slot,
				ft_get_parent_slot(holder_meta, ft),
				key_len, true, fuse_cell, pubp, NULL, NULL, node,
				1 /* @node is this key's SOLE entry */,
				-1 /* leaf key removed: detach owns the -1 */,
				NULL, false, NULL, NULL,
				/* EXPERIMENT: delete tier open, for the trace. */
				ft_in_place_delete_ok(ft));
			/* @node's freeze rode the detach commit (freeze_leaf). */
		} else {
			/*
			 * Removing the head, duplicates remain: key count unchanged.
			 * ft_promote_head fuses three edges in one flip:
			 *
			 *  - the cn->child republish;
			 *  - the grandparent SKIP_X dual (via
			 *    _ft_publish_to_parent on cn's plain flag);
			 *  - the cell swap.
			 *
			 * A candidate descent or an ft_skip_reanchor up-walk
			 * then never follows the stale skip pointer into the
			 * old head, which is about to be freed.
			 */
			ret = ft_unchain_node(ft, &lctx,
				ft_compressed_node_flag(cn), holder_depth,
				(struct cds_ft_node **) head_slot, node,
				FT_UNCHAIN_PROMOTE);
		}
	} else if (ft_node_external_nodes(holder_flag) ==
			(struct cds_ft_node *) node) {
		/*
		 * Internal holder whose external_nodes chain head is @node:
		 * prefix key (the key terminates at an internal node that also
		 * has longer-key children).  The holder stays; unlink @node from
		 * its external_nodes chain.  Propagate -1 only when @node is the
		 * last entry (the chain becomes empty).
		 */
		holder_meta = cds_ft_item_to_metadata(ft_node_ptr(holder_flag));
		if (!succ_node) {
			/*
			 * Last entry: the external chain empties, so the prefix
			 * key disappears (the holder KEEPS its longer-key children
			 * -- prefix-with-siblings).
			 */
			bool last_fused = false, fused_declined = false;
#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * Fuse the chain-compress prune into the key-removal
			 * commit.  The holder may be left a non-root 1-child
			 * no-external internal.  Its SKIP_COMPRESSED canonical
			 * form is then a merged compressed node.  So build that
			 * node and let it replace the holder.  This subsumes
			 * the external_nodes -> NULL clear and carries
			 * @dead_cell's unsplice in the same flip.  There is no
			 * separate clear and no transient non-canonical state.
			 * Allocation failure aborts the whole removal (key not
			 * removed, count rolled back, retriable MEMORY_ERROR).
			 * The surviving body child is the holder's sole
			 * non-NIL branch, because the external entry being
			 * removed is not a body child.  It is stable across the
			 * removal.
			 */
			if (ft_group_skip_compressed(ft->group) &&
			    ft_meta_nr_child(holder_meta) == 1 &&
			    ft_parent_node(holder_meta->parent_word) != NULL) {
				uint8_t s_byte = 0;
				struct cds_ft_inode_flag *s_child =
					ft_node_get_minmax(ft, holder_flag,
						&s_byte, FT_LEFTMOST, false);
				int cret;

				/*
				 * R3 chain-compress fold.  The merged
				 * compressed node replaces the collapsed
				 * 1-child holder.  It is built with the retired
				 * key's -1.  It records the ancestor -1 walk
				 * into its own commit (count_delta -1).  So
				 * there is no pre-decrement here.
				 */
				/*
				 * INTENT: no body child is detached.  The entry
				 * being removed is the external one.  The
				 * branch condition of this arm established
				 * that the head is @node.  So @node is what the
				 * callee must still find under the mark.
				 */
				const struct ft_chain_compress_intent intent = {
					.detach_child = NULL,
					.expect_ext = node,
					.detach_byte = 0,
				};

				/*
				 * No survivor means a peer moved it.  The count
				 * and the scan above are two unlocked reads,
				 * and the scan reads the body one slot at a
				 * time.  With the in-place tiers, a peer can
				 * refill a hole below the child and remove the
				 * child between two of those slot reads.  Each
				 * slot then reads empty, although the count was
				 * right and a child existed at every instant.
				 * Measured in ft_inv 154: the holder went from
				 * {97 hole, 98 live} to {97 live, 98 hole}.
				 * This tripped the collapse's `surviving_child`
				 * assert.
				 *
				 * At rest a count of one is one non-NULL slot,
				 * and a child that stays put is always found.
				 * So a NULL here proves that a peer committed
				 * on the holder since the count was read.
				 * Re-plan, as the non-fused arm's under-lock
				 * re-check below does.  The collapse
				 * re-validates under its locks, but it needs a
				 * survivor to name.
				 */
				if (caa_unlikely(!s_child))
					cret = -EAGAIN;
				else
					cret = ft_chain_compress_fused(ft,
						holder_flag, holder_depth, &lctx,
						holder_meta,
						s_child, s_byte,
						1 /* sole body child; the removed entry is external */,
						fuse_cell, NULL,
						NULL, 0, NULL, NULL, 0 /* no orphan chain */, node,
						1 /* @node is this key's SOLE entry */,
						-1, ft->rank_stats ? key_len + 1 : 0,
						NULL, false,
						NULL /* no pending publish */, &intent, NULL);

				if (cret == 0) {
					/*
					 * @node's freeze rode the fused merge commit
					 * above (freeze_leaf), atomic with the chain
					 * retire -- no separate mark_removed flip.
					 */
					if (fuse_remove)
						pub.armed = true;
					ret = 0;
					last_fused = true;
				} else if (cret < 0) {
					ret = cret;
					last_fused = true;
				}
				/* cret > 0: merge out of bound -- fall back to plain clear. */
				if (cret > 0)
					fused_declined = true;
			}
#endif
			if (!last_fused) {
				/*
				 * Non-fused: clear external_nodes -> NULL on
				 * its own.  When the ordered list is on
				 * (fuse_remove), commit that store together
				 * with @dead_cell's unsplice in one flip.  A
				 * reader then never sees the key gone from the
				 * structural index but present in the ordered
				 * list.  Readers resolve a parked proxy on
				 * external_nodes via ft_dereference_external.
				 * @pub.armed tells the deferred-free block
				 * below that the unsplice happened.
				 */
				/*
				 * Fold the prefix key's -1 onto the clear.  The
				 * holder stays in place and keeps its children,
				 * so its -1 is the holder->root walk: a
				 * multi-edge R1.
				 *
				 * Whenever the count must ride (ordered_list ||
				 * rank_stats), take a caller-reserved bounded
				 * txn and record the walk from holder_flag.
				 * Then let ft_remove_one_commit flip these
				 * together (ft_ord_cell_flip_into, infallible):
				 *
				 *  - the external_nodes -> NULL clear;
				 *  - @dead_cell's unsplice (list on);
				 *  - @node's freeze (freeze_leaf);
				 *  - the count edges.
				 *
				 * A reader then never sees the key gone from
				 * one index but present in another, nor a count
				 * out of step with the structure.  Parked
				 * proxies resolve via ft_dereference_external,
				 * and nr_keys via ft_nr_keys_load.
				 *
				 * There is no pre-decrement and no OOM
				 * rollback.  The pre-reserved txn commits
				 * infallibly.  The only abort is the arm, which
				 * frees the pre-reserved unsplice txn and
				 * leaves the key in place.
				 *
				 * List off + rank stats off: the infallible
				 * lone ft_unchain_node store, unchanged.
				 */
				if (ft->ordered_list || ft->rank_stats) {
					struct ft_flip_txn *txn = ft_flip_txn_create_bounded(ft,
						FT_REMOVE_COMMIT_REC_MAX_EDGES + 1 +
						(ft->rank_stats ? key_len + 1 : 0));

					if (!txn) {
						if (unsplice_txn)
							ft_flip_txn_destroy(unsplice_txn);
						FT_TP(remove_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
						return CDS_FT_STATUS_MEMORY_ERROR;
					}
					/* VALIDATE (§4.B): lock (or guard-fallback) the
					 * LIVE holder whose external_nodes this single-node
					 * clear empties -- value-swap target (§10.5). */
					ft_flip_txn_lock_or_guard_parent(ft, txn, &lctx,
					holder_flag, holder_depth);
#ifndef FT_DEBUG_RM_PREFIX_MISS_CARRIES_ON
					/*
					 * A missed take ends the attempt here.
					 * Carrying on recorded the count walk
					 * and took @dead_cell's lock-set.  It
					 * also built the unsplice and freeze
					 * edges.  ft_flip_txn_commit can only
					 * discard that txn.  The cost was 9
					 * records per miss, 21-43k misses per
					 * ft_inv leg.  Run that discard now.
					 * It is the same call
					 * ft_ord_cell_flip_into ends in, so
					 * the aging, the errno and the exit
					 * are the same.
					 */
					if (caa_unlikely(txn->acquire_miss)) {
						ret = ft_flip_status_to_errno(
							ft_flip_txn_commit(ft, txn));
					} else
#endif
#if defined(FEATURE_FT_SKIP_COMPRESSED) && !defined(FT_DEBUG_PREFIX_CLEAR_RESIDUE)
					/*
					 * The collapse the plan did not see.
					 * The fused arm above is chosen from
					 * an unheld nr_child.  This plan saw a
					 * second child, so a plain clear would
					 * leave a legal keyless node with two.
					 * A peer that removed that second
					 * child before our take changes the
					 * answer.  Clearing the last external
					 * of a 1-child node leaves a 1-child
					 * keyless internal, which
					 * cds_ft_verify refuses in skip mode.
					 * The post-prune below is a separate
					 * flip that loses under contention.
					 * That leaves the residue on which a
					 * later remove beneath it livelocks
					 * (exponential spacing, owned-key
					 * row).  Under the holder's lock
					 * nr_child is exact.  Re-plan, and the
					 * next attempt takes the fused
					 * collapse.
					 */
					if (!fused_declined &&
					    ft_group_skip_compressed(ft->group) &&
					    ft_meta_nr_child_load(holder_meta) == 1 &&
					    ft_parent_node(holder_meta->parent_word) !=
							NULL) {
						ft_flip_txn_destroy(txn);
						ret = -EAGAIN;
					} else
#endif
					{
						ft_flip_txn_record_count_parent(ft, txn,
							holder_flag, -1);
						ret = ft_remove_one_commit(ft,
							(struct cds_ft_inode_flag **) &holder_meta->external_nodes,
							holder_meta,
							(struct cds_ft_inode_flag *) node, NULL,
							NULL, dead_cell, NULL, txn, node, false);
					}
					if (ret == 0 && fuse_remove)
						pub.armed = true;
				} else {
					/* List off + rank stats off: unchanged; no count. */
					ret = ft_unchain_node(ft, &lctx, holder_flag,
						holder_depth,
						(struct cds_ft_node **) &holder_meta->external_nodes,
						node, fused_declined ?
							FT_UNCHAIN_CLEAR_KEEP :
							FT_UNCHAIN_CLEAR);
				}
#ifdef FEATURE_FT_SKIP_COMPRESSED
				/*
				 * Out-of-bound residue: the merge above did not
				 * apply because merged_len exceeds the
				 * compressed bound.  Post-prune it,
				 * best-effort, via the standalone second flip.
				 * This leaves a 1-child internal if it still
				 * cannot merge.  Only on a successful unlink.
				 */
				if (ret == 0 && ft_group_skip_compressed(ft->group) &&
				    !holder_meta->external_nodes &&
				    ft_meta_nr_child(holder_meta) == 1 &&
				    ft_parent_node(holder_meta->parent_word) != NULL) {
					ft_canonicalize_chain_compress(ft, holder_flag,
						holder_depth, &lctx, holder_meta);
				}
#endif
			}
		} else {
			/* Duplicates remain: head promotion (fresh-cell swap). */
			ret = ft_unchain_node(ft, &lctx, holder_flag, holder_depth,
				(struct cds_ft_node **) &holder_meta->external_nodes,
				node, FT_UNCHAIN_PROMOTE);
		}
	} else {
		/*
		 * Internal holder, @node is a body child: leaf key.  Recover the
		 * holder's body slot for @node from the key's last byte.
		 */
		struct cds_ft_inode_flag *child;

		holder_meta = cds_ft_item_to_metadata(ft_node_ptr(holder_flag));
		child = ft_node_get_nth_skip(holder_flag, &head_slot,
			iter_key[key_len - 1], FT_PF_NONE);
		/* Resolve a peer's mid-commit proxy before the identity compare (§9). */
		child = ft_resolve_flip_proxy(child);
		if (!child ||
		    (struct cds_ft_node *) ft_node_ptr(child) != node) {
			dbg_printf("cds_ft_remove: node %p not at key slot\n", node);
			/* Drop the pre-reserved unsplice txn (nothing published yet). */
			if (unsplice_txn)
				ft_flip_txn_destroy(unsplice_txn);
			/*
			 * RE-HOMED UNDER A LIVE HOLDER -> RETRY THE WHOLE OP.
			 * See ft_rm_holder_rehomed: the liveness test is what
			 * makes this terminate where the bare double-read spun.
			 */
			if (ft_rm_holder_rehomed(ft, node, holder_flag) ||
					ft_rm_slot_regained(ft, node, holder_flag,
						head_slot) ||
					ft_rm_miss_unconfirmed(ft, &lctx, node,
						holder_flag, holder_flag,
						holder_meta, holder_depth,
						iter_key[key_len - 1])) {
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;
			}
			FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
		if (!succ_node) {
			/*
			 * Last/only entry: prune the now-empty branch.
			 * Propagate -1 before detach, which may free internal nodes.
			 */
			/* Name the derivation: see the compressed-holder arm above. */
			FT_HLIST_WHY_STAMP();
			ret = ft_detach_node(ft, &lctx, head_slot,
				ft_get_parent_slot(holder_meta, ft),
				key_len, true, fuse_cell, pubp, NULL, NULL, node,
				1 /* @node is this key's SOLE entry */,
				-1 /* leaf key removed: detach owns the -1 */,
				NULL, false, NULL, NULL,
				/* EXPERIMENT: delete tier open, for the trace. */
				ft_in_place_delete_ok(ft));
			/* @node's freeze rode the detach commit (freeze_leaf). */
		} else {
			/* Removing the head, duplicates remain: key count unchanged. */
			ret = ft_unchain_node(ft, &lctx, holder_flag, holder_depth,
				(struct cds_ft_node **) head_slot, node,
				FT_UNCHAIN_PROMOTE);
		}
	}

	/*
	 * Head with no successor: the key disappeared.  Its cell is unspliced
	 * from the ordered list (when enabled) and freed.  The free is
	 * deferred, for parked up-walkers.  A promotion (cell_succ) keeps the
	 * cell in place: the key is the same, and only cell->node is
	 * retargeted in ft_unchain_node.  So there is no list op.  An in-place
	 * leaf detach already fused the unsplice into its structural flip
	 * (pub.armed).  Only the deferred cell free remains here.
	 */
	if (fuse_remove) {
		/* cell_was_head implies ordered_list, so the list op always runs. */
		if (ret == 0) {
			/* An in-place / fused structural commit already carried the
			 * unsplice (pub.armed); otherwise commit it now through the
			 * txn pre-reserved before the structural change. */
			if (!pub.armed) {
				/*
				 * Two-commit fallback.  The structural commit
				 * is public, so this unsplice must complete.
				 * Backing out would leave the key gone from the
				 * structural index but present in the ordered
				 * list.  On a peer conflict, drive it forward:
				 * each ABORT means a peer committed
				 * (obstruction-free).  Retry the small bounded
				 * re-reservation too, because the aborted
				 * commit consumed the txn.
				 */
				while (ft_ord_cell_unsplice(ft, unsplice_txn,
						dead_cell) > 0) {
					do {
						unsplice_txn =
							ft_flip_txn_create_bounded(ft,
							FT_ORD_CELL_UNSPLICE_MAX_EDGES);
					} while (caa_unlikely(!unsplice_txn));
				}
			} else
				ft_flip_txn_destroy(unsplice_txn);
			ft_ord_cell_free(ft, dead_cell);
		} else {
			/* Removal aborted (MEMORY_ERROR or a peer conflict):
			 * nothing left the trie, the cell stays in the list --
			 * release the unused reservation. */
			ft_flip_txn_destroy(unsplice_txn);
		}
	}

	/*
	 * -ENOENT is not a bug under Phase 4.3 concurrent writers.  It means
	 * that detach's replace reached an emptied FT_NULL slot, or that the
	 * unchain found the node already gone.  The position was derived by a
	 * non-exclusive traversal.  A peer's recompaction can therefore retype
	 * or empty the cached parent slot between the derivation and the
	 * replace.  That path publishes nothing and destroys every
	 * pre-reserved txn (see end:), so it is a clean abort.  It is routed
	 * to the retry loop below exactly like -EAGAIN: re-derive the position
	 * from node->prev against the current tree.  Under single-writer
	 * exclusion the found-implies-replaceable invariant still holds, so
	 * -ENOENT does not arise there.
	 */

	/*
	 * Invalidate the iterator path. The trie structure may have
	 * changed due to node recompaction during detach, making the
	 * cached path stale.
	 */
	iter->cache_valid = false;
	iter_debug_path_clear(iter);
	iter->path_len = 0;

	switch (ret) {
	case 0:
		FT_TP(remove_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	case -ENOMEM:
		FT_TP(remove_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
		return CDS_FT_STATUS_MEMORY_ERROR;
	case -ESTALE:
		/*
		 * @node's chain left the trie (a whole-chain displacement
		 * freezes only its head).  ft_unchain_node established this
		 * under the holder.  It is definitive, so it answers NOT_FOUND
		 * and does not join the retry cases below.  Re-deriving would
		 * find the same retired chain every lap.
		 */
		FT_TP(remove_exit, (int) CDS_FT_STATUS_NOT_FOUND);
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
		ft_dbg_rm_site = __LINE__;
#endif
		return CDS_FT_STATUS_NOT_FOUND;
	case -EAGAIN:
	case -ENOENT:
		/*
		 * Two cases end here:
		 *
		 *  - a peer writer won a commit this attempt (ABORT);
		 *  - a peer moved / recompaction-retyped the position
		 *    pre-commit (-ENOENT: the cached slot emptied to FT_NULL,
		 *    or the node was already unchained).
		 *
		 * Nothing was published.  Every fused edge (freeze, count,
		 * tombstone) was discarded with it.  Signal the wrapper's retry
		 * loop to re-derive and re-attempt.
		 */
		FT_DBG_RETRY_SITE();
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		/*
		 * ★ Which of the two.  The exit fuses -EAGAIN (a peer won a
		 * commit, or a lock acquire refused) with -ENOENT (the derived
		 * position emptied / retyped pre-commit).  They have different
		 * cures: one is contention, the other is a derivation that can
		 * be stably dead.  A livelock here cannot be diagnosed until
		 * they are counted apart.
		 */
		if (ret == -EAGAIN)
			ft_dbg_rm_eagain++;
		else
			ft_dbg_rm_enoent++;
#endif
		*need_retry = true;
		return CDS_FT_STATUS_OK;	/* value unused: wrapper retries */
	default:
		abort();
	}
}

#ifdef FT_DEBUG_REMOVE_RETRY_CAP
# include <stdio.h>
# include <stdlib.h>
# ifndef FT_REMOVE_RETRY_CAP
#  define FT_REMOVE_RETRY_CAP	50000
# endif
/*
 * The wall-clock threshold for the per-op "FT REMOVE SLOW" diagnostics below,
 * in nanoseconds.  0 compiles them out entirely, leaving only the RETRY CAP's
 * livelock abort.
 *
 * This knob exists because the two existing modes cannot express what a gate
 * leg needs.  The gate arms this detector for one reason: a remove livelock
 * must abort loudly and not expire as a timeout.  On a box running 25 legs at
 * once, rc=124 cannot be told apart from CPU contention.  The gate does not
 * want the per-op tail data.  -DFT_REMOVE_TAIL_QUIET cannot silence that
 * data.  It deliberately keeps the >1ms metric.  That is the metric D.3's A/B
 * measured.  doc/design/mw-to-fine-locking-remainder.md §D.3 says that the
 * milestone dump biases a control arm, not this metric.  Measured on the imw
 * leg of this very config: 9,799 SLOW pairs and 1.9 MB of stderr in the
 * first 30 s.  That drowns the TAP stream the gate greps for its verdict.
 *
 * Default 1 ms, i.e. byte-identical to what every existing user compiled.
 */
# ifndef FT_REMOVE_BUSY_ATTEMPTS
/* 0 disables; a remove retrying more than this reports its breakdown. */
#  define FT_REMOVE_BUSY_ATTEMPTS	200
# endif
# ifndef FT_REMOVE_SLOW_NS
#  define FT_REMOVE_SLOW_NS	1000000
# endif
#endif

enum cds_ft_status cds_ft_remove(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node *node)
{
	struct ft_op optxn;
	struct ft_op_retry op_retry;
	enum cds_ft_status s;
	bool need_retry;
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
	unsigned int ft_remove_attempts = 0;
	uint64_t ft_remove_t0 = ft_dbg_now_ns();
	struct rusage ft_remove_ru0;

	getrusage(RUSAGE_THREAD, &ft_remove_ru0);
	uint64_t ft_acc_begin = 0, ft_acc_body = 0, ft_acc_bail = 0;
	uint64_t ft_tA, ft_tB, ft_tC;
#ifdef CDS_FAIR_MUTEX_DBG_POLL
	unsigned long ft_fmtx0 = cds_fmtx_dbg_polls;
	unsigned long ft_wfcq0 = cds_wfcq_dbg_polls;
#endif

	ft_dbg_acq_dirty_lock = 0;
	ft_dbg_acq_dirty_other = 0;
	ft_dbg_acq_cabort = 0;
	ft_dbg_gp_ns = 0;
	ft_dbg_gp_calls = 0;
	ft_dbg_arena_ns = 0;
	ft_dbg_arena_waits = 0;
#endif

	CDS_FT_SCOPED_WRITER(ft);
	if (caa_unlikely(ft_bulk_active(ft))) {
		/* See ft_iter_redescend_node: the gate is set, so re-derive. */
		s = ft_iter_redescend_node(ft, iter, node);
		if (s != CDS_FT_STATUS_OK)
			return s;
	}
	/*
	 * FT-owned per-op read-side bracket + retry identity (doc §11).
	 *
	 * On a concurrent trie, the body's position derivation, parked records
	 * and internal commits must run inside a read-side section.  A peer
	 * writer's call_rcu-deferred frees then cannot reclaim under them.  A
	 * caller-held section merely nests.  @node's liveness at entry remains
	 * the caller's obligation.  It is a lookup reference, valid only under
	 * the caller's own section (the §11 reference-lifetime contract).
	 *
	 * RETRY: a peer-conflict attempt is a commit ABORT or a pre-commit
	 * position conflict.  It publishes nothing and signals @need_retry.
	 * The loop re-derives the position from node->prev against the current
	 * tree and re-attempts.
	 *
	 * The op's internal txns are still standalone.  The pre-reserved
	 * unsplice txn coexists with the main commit txn, so they cannot share
	 * one handle.  Contention aging is therefore carried manually on the
	 * persistent @optxn via ft_op_conflict.  After its lane budget of
	 * conflicts, this writer escalates into the per-trie FIFO fair-mutex
	 * lane.  Every writer's begin() honors domain->active, so the lane
	 * drains the contention and the retry terminates (no livelock).
	 *
	 * Exclusive trie: the bracket opens nothing and no conflict ever
	 * fires.
	 */
	FT_SH_STALL_DECL;

	ft_op_retry_init(&op_retry, FT_OP_REMOVE, NULL, 0);
	ft_txn_op_init(ft, &optxn);
	for (;;) {
		need_retry = false;
		ft_op_retry_tick(ft, &op_retry, 0);
		FT_SH_STALL_TICK(ft, "cds_ft_remove");
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		ft_tA = ft_dbg_now_ns();
#endif
		ft_op_begin(&optxn);
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		ft_tB = ft_dbg_now_ns();
		ft_acc_begin += ft_tB - ft_tA;
#endif
#ifdef FT_DLM_LINGER
		ft_linger_word = NULL;	/* only THIS attempt's refusal counts */
#endif
		s = _cds_ft_remove_locked(ft, iter, node, &need_retry, &optxn);
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		ft_tC = ft_dbg_now_ns();
		ft_acc_body += ft_tC - ft_tB;
#endif
		if (!need_retry)
			break;
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		++ft_remove_attempts;
#ifndef FT_REMOVE_TAIL_QUIET
		if (caa_unlikely(ft_remove_attempts == 100 ||
				ft_remove_attempts == 1000 ||
				ft_remove_attempts == 10000)) {
			fprintf(stderr, "FT REMOVE RETRY TAIL: attempts=%u "
				"retry=%lu in_lane=%d(th %d) "
				"lane_published=%d active=%d "
				"dirtyLOCK=%u dirtyOTHER=%u cabort=%u\n",
				ft_remove_attempts, optxn.retry,
				optxn.in_lane, ft_op_in_lane(),
				optxn.lane_published,
				optxn.lane ? (int) uatomic_load(
					&optxn.lane->active, CMM_RELAXED)
					: -1,
				ft_dbg_acq_dirty_lock, ft_dbg_acq_dirty_other,
				ft_dbg_acq_cabort);
			fprintf(stderr, "FT REMOVE RETRY WHY: eagain=%lu "
				"enoent=%lu\n", ft_dbg_rm_eagain,
				ft_dbg_rm_enoent);
#ifdef FT_ENABLE_TRACING
			/*
			 * The traced build's milestone is the violation.  Emit
			 * the self-diagnosing event, persist the ring, and
			 * crash before the window scrolls out.
			 */
			if (ft_remove_attempts == 100 && ft_dbg_last_refused) {
				const struct cds_ft_metadata *m =
					ft_dbg_last_refused;
				const struct ft_dbg_take_slot *sl =
					ft_dbg_take_slot_of(m);
				int match = (sl->meta == m);

				FT_TP(remove_anchor_starved, (const void *) m,
					(unsigned long) CMM_LOAD_SHARED(
						m->state),
					ft_dbg_refused_streak,
					ft_remove_attempts,
					match ? sl->fn : "?",
					match ? sl->line : 0,
					match ? sl->op_bound : -1,
					match ? (unsigned long)
						((ft_dbg_now_ns() - sl->ts_ns)
							/ 1000) : 0);
				(void) system("lttng snapshot record 1>&2");
				abort();
			}
#else /* !FT_ENABLE_TRACING */
			/*
			 * Sample the refused word for 200us: is this ONE hold
			 * spanning the victim's episode, or a churn of takes?
			 * The registry names the taker; arena-backed metadata
			 * stays mapped, so a racy read cannot fault.
			 */
			if (ft_dbg_last_refused) {
				const struct cds_ft_metadata *m =
					ft_dbg_last_refused;
				const struct ft_dbg_take_slot *sl =
					ft_dbg_take_slot_of(m);
				uint64_t t0 = ft_dbg_now_ns(), tnow;
				unsigned int held = 0, total = 0, trans = 0;
				int prev_locked = -1;

				do {
					uintptr_t sw = CMM_LOAD_SHARED(
						m->state);
					int locked = !!(sw & FT_STATE_LOCK);

					total++;
					held += locked;
					if (prev_locked >= 0 &&
							locked != prev_locked)
						trans++;
					prev_locked = locked;
					caa_cpu_relax();
					tnow = ft_dbg_now_ns();
				} while (tnow - t0 < 200000);
				if (sl->meta == m) {
					fprintf(stderr, "FT REMOVE RETRY "
						"HOLDER: streak=%u "
						"held=%u/%u trans=%u "
						"taker=%s:%d op_bound=%d "
						"tid=%lx take_age_us=%llu "
						"phase=%d phase_age_us=%llu\n",
						ft_dbg_refused_streak,
						held, total, trans,
						sl->fn, sl->line,
						sl->op_bound, sl->tid,
						(unsigned long long)
						(t0 - sl->ts_ns) / 1000,
						sl->phase_line,
						sl->phase_ns ?
						(unsigned long long)
						(t0 - sl->phase_ns) / 1000 : 0);
				} else {
					fprintf(stderr, "FT REMOVE RETRY "
						"HOLDER: streak=%u "
						"held=%u/%u trans=%u "
						"taker=UNKNOWN (slot %p vs "
						"%p)\n",
						ft_dbg_refused_streak,
						held, total, trans,
						(void *) sl->meta,
						(void *) m);
				}
			}
#endif /* !FT_ENABLE_TRACING */
		}
#endif /* !FT_REMOVE_TAIL_QUIET */
		if (caa_unlikely(ft_remove_attempts > FT_REMOVE_RETRY_CAP)) {
			fprintf(stderr, "FT REMOVE LIVELOCK: %u attempts on "
				"one remove\n", ft_remove_attempts);
			abort();
		}
#endif
		/* Age the conflict, forfeit the turn, close the attempt. */
		ft_txn_attempt_bail(&optxn, true);
#ifdef FT_DLM_LINGER
		ft_dlm_linger(&optxn);	/* nothing held here; see the helper */
#endif
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
		ft_acc_bail += ft_dbg_now_ns() - ft_tC;
#endif
	}
	ft_op_end(&optxn);
#ifdef FT_DEBUG_REMOVE_RETRY_CAP
	{
		uint64_t wall = ft_dbg_now_ns() - ft_remove_t0;

		/*
		 * ☞ Busy is not slow, and only one of them was reported.
		 * This block fires on wall time, so a remove that retried two
		 * thousand times cheaply never printed.  The run that measured
		 * max-retry 38,244 produced exactly one "SLOW" line.  That
		 * line had attempts=0: a grace-period wait, not a retry storm.
		 * Fire on the attempt count too.  The retry question is asked
		 * on that axis.
		 */
		if (FT_REMOVE_BUSY_ATTEMPTS &&
				caa_unlikely(ft_remove_attempts >
					FT_REMOVE_BUSY_ATTEMPTS))
			fprintf(stderr, "FT REMOVE BUSY: attempts=%u "
				"dirtyLOCK=%u dirtyOTHER=%u cabort=%u "
				"wall_us=%llu\n",
				ft_remove_attempts, ft_dbg_acq_dirty_lock,
				ft_dbg_acq_dirty_other, ft_dbg_acq_cabort,
				(unsigned long long) (wall / 1000));
		if (FT_REMOVE_SLOW_NS && caa_unlikely(wall > FT_REMOVE_SLOW_NS))
			fprintf(stderr, "FT REMOVE SLOW: wall_us=%llu "
				"attempts=%u dirtyLOCK=%u dirtyOTHER=%u "
				"cabort=%u begin_us=%llu body_us=%llu "
				"bail_us=%llu other_us=%llu\n",
				(unsigned long long) (wall / 1000),
				ft_remove_attempts, ft_dbg_acq_dirty_lock,
				ft_dbg_acq_dirty_other, ft_dbg_acq_cabort,
				(unsigned long long) (ft_acc_begin / 1000),
				(unsigned long long) (ft_acc_body / 1000),
				(unsigned long long) (ft_acc_bail / 1000),
				(unsigned long long) ((wall - ft_acc_begin -
					ft_acc_body - ft_acc_bail) / 1000));
		if (FT_REMOVE_SLOW_NS && caa_unlikely(wall > FT_REMOVE_SLOW_NS)) {
			struct rusage ru;

			getrusage(RUSAGE_THREAD, &ru);
			fprintf(stderr, "FT REMOVE SLOW GP: gp_us=%llu "
				"gp_calls=%u arena_us=%llu arena_waits=%u "
				"nvcsw=%ld nivcsw=%ld\n",
				(unsigned long long) (ft_dbg_gp_ns / 1000),
				ft_dbg_gp_calls,
				(unsigned long long) (ft_dbg_arena_ns / 1000),
				ft_dbg_arena_waits,
				(long) (ru.ru_nvcsw - ft_remove_ru0.ru_nvcsw),
				(long) (ru.ru_nivcsw -
					ft_remove_ru0.ru_nivcsw));
		}
#ifdef CDS_FAIR_MUTEX_DBG_POLL
		if (FT_REMOVE_SLOW_NS && caa_unlikely(wall > FT_REMOVE_SLOW_NS))
			fprintf(stderr, "FT REMOVE SLOW POLLS: fmtx=%lu "
				"wfcq=%lu\n",
				cds_fmtx_dbg_polls - ft_fmtx0,
				cds_wfcq_dbg_polls - ft_wfcq0);
#endif
	}
#endif
	return s;
}

/*
 * ft_locate_chain_head: derive an external chain head's position with no
 * descent, from the head itself (head->prev is its holder) and the key.
 *
 * Set these outputs:
 * - *holder_flag_p: the head's internal/compressed holder.
 * - *is_prefix_p: true when the chain hangs off the holder's external_nodes
 *   (a prefix key).  False when it hangs off a body/compressed child slot.
 * - *head_slot_p, for the non-prefix case: the holder slot that holds @head.
 *   This is &cn->child for a compressed holder.  For an internal holder it
 *   is the body slot for the key's last byte.
 *
 * Return true iff @head is the live chain head at that position.  A cached
 * iter->node is then still current under the writer mutex.  The prefix case
 * is validated by holder->external_nodes == head.  The leaf case is
 * validated by *head_slot == head.
 *
 * Return false in these cases:
 * - @head is a non-head duplicate;
 * - @head has no holder;
 * - the holder no longer points at @head (stale cache).
 * The caller then re-seeds via a fresh lookup.
 */
static
bool ft_locate_chain_head(struct cds_ft *ft, struct cds_ft_node *head,
		const uint8_t *iter_key, size_t key_len,
		struct cds_ft_inode_flag **holder_flag_p,
		struct cds_ft_inode_flag ***head_slot_p,
		bool *is_prefix_p)
{
	struct cds_ft_inode_flag *holder_flag = ft_node_holder(ft, head);

	if (!holder_flag || ft_node_external(holder_flag))
		return false;	/* no holder, or @head is a non-head duplicate */
	*holder_flag_p = holder_flag;
	if (ft_node_compressed(holder_flag) ||
	    ft_node_skip_compressed(holder_flag)) {
		struct cds_ft_compressed_node *cn =
			ft_node_skip_compressed(holder_flag) ?
				ft_skip_to_compressed(ft, holder_flag) :
				ft_compressed_node_ptr(holder_flag);

		*is_prefix_p = false;
		*head_slot_p = &cn->child;
	} else if (ft_node_external_nodes(holder_flag) ==
			(struct cds_ft_node *) head) {
		*is_prefix_p = true;
		*head_slot_p = NULL;	/* external_nodes, not a body slot */
		return true;
	} else {
		struct cds_ft_inode_flag *child;

		*is_prefix_p = false;
		child = ft_node_get_nth_skip(holder_flag, head_slot_p,
			iter_key[key_len - 1], FT_PF_NONE);
		if (!child)
			return false;
	}
	/*
	 * /!\ RESOLVE BEFORE COMPARING.  A peer writer's txn that is committed
	 * but not settled parks a flip proxy in this very slot.  ft_node_ptr's
	 * contract is a RESOLVED pointer.  It asserts on a parked one under
	 * -DFT_DEBUG_PROXY_ASSERT.  That is how this was found: on 9 legs of a
	 * gate run, deterministically at the same test.
	 *
	 * Read RAW, the mask lands on a DESCRIPTOR and the compare fails.  This
	 * function then answers "not the live head" for a head that IS live.
	 * The two callers handle that answer differently:
	 * - The first absorbs it as a stale cache and re-seeds.
	 * - The second asks again AFTER a fresh lookup has already found the
	 *   node.  It turns a false here into CDS_FT_STATUS_NOT_FOUND.
	 * That is a wrong answer for a key that is present.  The oracles cannot
	 * see it, because a concurrent remove_all lane legitimately reports
	 * NOT_FOUND too.
	 *
	 * A refusal is a claim about the trie.  A word that merely happened to
	 * be in flight must not decide it.
	 */
	return (struct cds_ft_node *) ft_node_ptr(
			ft_resolve_flip_proxy(**head_slot_p)) == head;
}



static
enum cds_ft_status _cds_ft_remove_all_locked(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node **result_node,
		bool *need_retry,
		struct ft_op *op)
{
	struct cds_ft_node *chain_head;
	struct cds_ft_inode_flag *holder_flag;
	struct cds_ft_metadata *holder_meta;
	struct cds_ft_inode_flag **head_slot;
	bool is_prefix;
	int ret;
	const uint8_t *iter_key;
	size_t key_len = ft_key_len(ft, ft_iter_resolve_key_len(iter));

	/*
	 * If the iterator has a valid path, the RCU read-side lock must
	 * be held.
	 */
	if (iter->cache_valid)
		CDS_FT_ASSERT_CALLER_RCU_READ_LOCKED(ft);
	iter_debug_path_check(iter);

	if (!valid_key_len(ft, key_len)) {
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}

	/*
	 * Handle NIL key (key_len == 0): root is always internal,
	 * remove its external_nodes chain.
	 */
	if (!key_len) {
		struct cds_ft_metadata *metadata;
		struct cds_ft_node *external_nodes;
		struct cds_ft_inode_flag *root_nf;
		struct ft_flip_txn *root_txn = NULL;
		unsigned int nr_frozen;

		root_nf = ft_resolve_flip_proxy(rcu_dereference(ft->root));
		metadata = cds_ft_item_to_metadata(ft_node_ptr(root_nf));
		external_nodes = metadata->external_nodes;
		if (!external_nodes) {
			*result_node = NULL;
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
		/*
		 * The DERIVATION the freeze below is bounded by; see
		 * ft_hlist_freeze_chain_prepare.  Taken before either commit
		 * path so both reserve the same number of edges.
		 */
		nr_frozen = ft_hlist_chain_len(external_nodes);
		/*
		 * LOCK THE ROOT, THEN RE-READ THE PLAN UNDER IT
		 * (doc/design/ft-lockset-inventory.md §1, row 2).
		 *
		 * Both commits below clear the ROOT's @external_nodes and freeze
		 * the chain it heads.  Until now they did it holding nothing.
		 * That was ~58k commits per ft_inv run, at every spacing.
		 *
		 * The root is the word's owner.  At depth 0 it is also its own
		 * anchor under every spacing.  So this is one acquire with no
		 * descent.  It is registered in @root_txn with its release.
		 * The commit consumes that release, and an early destroy
		 * releases it.
		 *
		 * The steps are: derive, acquire, RE-VALIDATE, bail retriably.
		 * Two things can change in between:
		 * - The root is replaced (graft_swap, a root recompaction).
		 *   This refuses the acquire or fails the identity check.
		 * - The chain grows or is cleared.  This fails the head /
		 *   length check.
		 * Both are the retry the callers below already take on an
		 * aborted flip.  An exclusive or COARSE trie has no per-node
		 * peer to exclude.
		 */
		root_txn = ft_flip_txn_create_bounded(ft,
			FT_REMOVE_COMMIT_REC_MAX_EDGES +
			(ft->rank_stats ? 1 : 0) +
			nr_frozen * FT_HLIST_FREEZE_MAX_EDGES +
			1 /* the root's {LOCK|s -> s} release */);
		if (!root_txn) {
			*result_node = NULL;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		if (ft->lock_fine && !ft->exclusive) {
			struct ft_lock_ctx rctx;
			enum ft_lock_or_guard_exit rex;

			ft_lock_ctx_init(&rctx, NULL, root_txn, op);
			ft_flip_txn_lock_or_guard_parent_ex(__func__, __LINE__,
				ft, root_txn, &rctx, root_nf, 0, &rex);
			if (rex == FT_LOG_EXIT_MISS) {
				bool enomem = root_txn->acquire_enomem;

				ft_flip_txn_destroy(root_txn);
				*result_node = NULL;
				if (enomem)
					return CDS_FT_STATUS_MEMORY_ERROR;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;
			}
			if (ft_resolve_flip_proxy(rcu_dereference(ft->root)) !=
						root_nf ||
					metadata->external_nodes != external_nodes ||
					ft_hlist_chain_len(external_nodes) !=
						nr_frozen) {
				ft_flip_txn_destroy(root_txn);
				*result_node = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;
			}
		}
		*result_node = external_nodes;
		/*
		 * Ordered list on: the NIL key is the global minimum (a prefix of
		 * every key).  So its removal is the prefix-with-siblings clear
		 * at the root.  Fuse these in ONE flip:
		 * - the root external_nodes -> NULL store;
		 * - the head cell's unsplice;
		 * - the root nr_keys -1 (rank stats).
		 * A reader then never sees the key gone from one index but
		 * present in the other.  It never sees a count out of step with
		 * the structure either.  Readers resolve a parked proxy on
		 * external_nodes via ft_dereference_external, and on nr_keys
		 * via ft_nr_keys_load.
		 *
		 * List off, no rank stats: express the node -> NULL clear as a
		 * lone 1-edge flip (an infallible release store) rather than a
		 * bare store.
		 */
		if (ft->ordered_list || ft->rank_stats) {
			/*
			 * This prev is read under the root's lock, after the
			 * re-check above.  So it is settled: every producer of
			 * a parked head prev holds the chain holder.  It is
			 * resolved anyway, as the plan-time read below must be.
			 */
			struct ft_ord_cell *dead = ft->ordered_list ?
				ft_ord_cell_ptr(ft_dereference_prev_resolved(
					external_nodes)) : NULL;
			struct ft_flip_txn *txn = root_txn;

			/*
			 * R1 count fold: the NIL key lives at the root.  So its
			 * -1 is a single edge on the root's own nr_keys (no
			 * ancestors).  Reserve it plus the structural + cell
			 * edges.  Record the count walk from the root.  Then
			 * let ft_remove_one_commit flip them together.
			 *
			 * This is exact and atomic: the decrement goes live
			 * WITH the detach, not before it.  So there is no
			 * pre-decrement and no OOM rollback.  The pre-reserved
			 * txn commits infallibly and the arm is the only abort.
			 * The count record is a no-op when rank stats are off
			 * (list-on path).
			 */
			ft_flip_txn_record_count_parent(ft, txn, ft->root, -1);
			/*
			 * ☑ AND THE CHAIN'S FREEZE RIDES THIS FLIP, like the
			 * leaf and prefix arms.  It used to be a post-commit
			 * ft_ra_sweep_held.  The acquire of that sweep is gated
			 * on `lock_spacing == PER_NODE`.  So above per-node it
			 * never fired, and the marks were bare CASes holding
			 * NOTHING.  MEASURED: UNHELD=1 at exponential AND
			 * root-only on inv_remove_cross_view_prefix_siblings_all,
			 * 0 at per-node.  Folding the freeze in is
			 * spacing-INDEPENDENT.  That is why it is the cure, and
			 * widening the acquire is not.
			 */
			ft_ch_audit(ft, txn, external_nodes);
			/*
			 * The plan IS already validated under the lock here.
			 * The root acquire above re-reads it and bails
			 * retriably (doc §1 row 2).  That is the shape
			 * prescribed for this class.
			 */
			ft_hlist_freeze_chain_prepare_checked(ft,
				ft_flip_txn_handle(txn),
				external_nodes, nr_frozen);
			/*
			 * Same rule as the prefix clear below.  The
			 * pre-reserved txn cannot fail to ALLOCATE, but the
			 * flip can still ABORT.  An aborted flip left the NIL
			 * key in the trie.
			 * Returning OK there hands the caller a chain it may
			 * reclaim while the root still points at it.  It also
			 * frees @dead while it is still spliced into the list.
			 */
			/*
			 * PRE-RESERVED => the commit cannot fail to allocate.
			 * So its only non-zero status is -EAGAIN: a peer won
			 * this flip and NOTHING was installed.  The fused cell
			 * and count edges were discarded with it.  Reporting
			 * that as MEMORY_ERROR sent the caller freeing memory
			 * over a lock it merely lost.  It is the NIL key's copy
			 * of the mislabel that the tail below fixed for every
			 * other shape.  It is retriable: re-read external_nodes
			 * and re-attempt.
			 */
			if (ft_remove_one_commit(ft,
					(struct cds_ft_inode_flag **) &metadata->external_nodes,
					metadata,
					(struct cds_ft_inode_flag *) external_nodes, NULL,
					NULL, dead, NULL, txn, NULL, false)) {
				*result_node = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			}
			if (dead)
				ft_ord_cell_free(ft, dead);
		} else {
			/*
			 * List off AND rank off.  This was a bare lone flip with
			 * the tombstones swept afterwards.  The sweep could not
			 * hold anything above per-node (see above).  So take a
			 * pre-reserved bounded txn here too, and commit the
			 * clear WITH the chain's marks.  The prefix clear uses
			 * the same shape for exactly this reason ("no bare lone
			 * clear even with both flags off").
			 */
			struct ft_flip_txn *txn = root_txn;

			ft_ch_audit(ft, txn, external_nodes);
			/*
			 * The plan IS already validated under the lock here.
			 * The root acquire above re-reads it and bails
			 * retriably (doc §1 row 2).  That is the shape
			 * prescribed for this class.
			 */
			ft_hlist_freeze_chain_prepare_checked(ft,
				ft_flip_txn_handle(txn),
				external_nodes, nr_frozen);
			if (ft_remove_one_commit(ft,
					(struct cds_ft_inode_flag **) &metadata->external_nodes,
					metadata,
					(struct cds_ft_inode_flag *) external_nodes, NULL,
					NULL, NULL, NULL, txn, NULL, false)) {
				*result_node = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			}
		}
		/* The mutation invalidates the cached position (general-path parity). */
		iter->cache_valid = false;
		iter_debug_path_clear(iter);
		iter->path_len = 0;
		return CDS_FT_STATUS_OK;
	}

	iter_key = ft_iter_read_key(iter);
	dbg_printf("cds_ft_remove_all attempt\n");

	/*
	 * No top-down descent: anchor on the cached chain head (iter->node)
	 * under the writer mutex.  Use it if it is still the live head at
	 * iter->key: ft_locate_chain_head validates *head_slot == iter->node.
	 * Otherwise re-seed via a fresh exact lookup and locate from there.
	 * That lookup is the standard positioning descent, NOT a
	 * remove-specific re-descent.
	 */
	chain_head = NULL;
	if (iter->cache_valid && iter->node && !ft_node_is_removed(iter->node) &&
	    ft_locate_chain_head(ft, iter->node, iter_key, key_len,
		    &holder_flag, &head_slot, &is_prefix))
		chain_head = iter->node;
	if (!chain_head) {
		enum cds_ft_status s = (*ft->lookup_iter_fn)(ft, iter);
		bool found = s == CDS_FT_STATUS_OK && iter->node;

		if (!found || !ft_locate_chain_head(ft, iter->node, iter_key,
				key_len, &holder_flag, &head_slot, &is_prefix)) {
			/*
			 * ☠ FOUND, THEN NOT LOCATED, IS NOT A MISS.  The lookup
			 * just reached a live head.  ft_locate_chain_head then
			 * re-derives its position from two more UNHELD reads:
			 * the back edge, then the holder's slot.  A peer
			 * re-home between them makes them disagree.  Such a
			 * re-home is an insert of a key extending this one.  It
			 * republishes the slot with a junction that carries the
			 * head.  That is the identity-bail shape which
			 * _cds_ft_remove_locked confirms under the holder's
			 * lock (ft_rm_miss_unconfirmed).  Here nothing is held
			 * yet, so re-derive the attempt.
			 */
#ifndef FT_DEBUG_RA_LOCATE_MISS
			if (found && !ft_node_is_removed(iter->node)) {
				*result_node = NULL;
				FT_DBG_RETRY_SITE();
				*need_retry = true;
				return CDS_FT_STATUS_OK;	/* discarded by the retry loop */
			}
#endif
			*result_node = NULL;
#if defined(FT_ENABLE_TRACING) || defined(FT_DEBUG_RM_SITE)
			ft_dbg_rm_site = __LINE__;
#endif
			return CDS_FT_STATUS_NOT_FOUND;
		}
		chain_head = iter->node;
	}
	holder_meta = cds_ft_item_to_metadata(ft_node_ptr(holder_flag));
	*result_node = chain_head;

	/*
	 * The holder comes from the cached node's back-pointer.  Per-node
	 * locking anchors every member on itself.  So no descent (and no
	 * depth) is needed.
	 */
	struct ft_lock_ctx lctx;
	unsigned int holder_depth = 0;

	ft_lock_ctx_init(&lctx, NULL, NULL, op);

	/*
	 * Ordered list on: the whole key leaves the trie.  So its head's cell
	 * is unspliced + freed below.  The shapes differ:
	 * - An in-place leaf detach fuses that unsplice into its structural
	 *   flip (pub.armed).
	 * - The prefix and recompaction / compressed-parent shapes stay
	 *   two-commit (pub unarmed).
	 */
	struct ft_remove_pub pub = { .armed = false };
	/*
	 * THE HEAD'S CELL, FROM ONE RESOLVED LOAD -- cds_ft_remove's discipline.
	 * Nothing is held here.  A peer may remove the head before @chain_head
	 * and PROMOTE it.  That peer parks a flip proxy on @chain_head->prev
	 * for the length of its commit.
	 *
	 * This is what happened when that word was read raw:
	 * - ft_ord_cell_ptr() masked the proxy's tag into an address INSIDE the
	 *   peer's descriptor (...0xe).
	 * - The under-lock re-checks passed once the peer had committed.
	 * - The cell lock take then followed that "cell" into unmapped memory.
	 * That was the zoo's SEGV in ft_cell_word_lock.  A probe showed that
	 * every wild cell ended in 0xe and resolved to a real one.  The two
	 * that got past the re-checks reached ft_detach_node's take.
	 * insert_replace had the same raw read (f8f6cd3e).
	 *
	 * Resolved, the value is the word before or after that peer's commit,
	 * never a descriptor.  A stale-but-real cell is what the under-lock
	 * re-validation (the chain plan, the cell plan) already catches.  A prev
	 * naming a PREDECESSOR says @chain_head is no longer a head: re-plan.
	 */
#ifdef FT_DEBUG_RA_PLAN_CELL_RAW
	struct ft_ord_cell *dead_cell = ft->ordered_list ?
		ft_ord_cell_ptr(chain_head->prev) : NULL;
#else
	struct ft_ord_cell *dead_cell = NULL;

	if (ft->ordered_list) {
		void *head_prev = ft_dereference_prev_resolved(chain_head);

		if (ft_node_external((struct cds_ft_inode_flag *) head_prev)) {
			*result_node = NULL;
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			return CDS_FT_STATUS_OK;	/* discarded by the retry loop */
		}
		dead_cell = ft_ord_cell_ptr(head_prev);
	}
#endif
	/*
	 * PRE-RESERVE the dead cell's standalone-unsplice txn before any
	 * structural change (see cds_ft_remove).  The prefix / recompaction /
	 * compressed-parent shapes stay two-commit (pub unarmed).  They
	 * unsplice AFTER the structural commit is public, which is
	 * un-abortable.  OOM here aborts the removal cleanly.  The fused case
	 * frees the txn unused.
	 */
	struct ft_flip_txn *unsplice_txn = NULL;

	if (dead_cell) {
		unsplice_txn = ft_flip_txn_create_bounded(ft,
			FT_ORD_CELL_UNSPLICE_MAX_EDGES);
		if (!unsplice_txn) {
			*result_node = NULL;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	if (is_prefix) {
		/*
		 * Prefix key: the chain hangs off the internal holder's
		 * external_nodes.  Clear it (one key) and tombstone the chain.
		 * Propagate -1 before clearing (undercount ordering).
		 *
		 * The holder always KEEPS at least one child here.  An is_prefix
		 * holder is an internal node carrying external_nodes.  By
		 * construction such a node has nr_child >= 1.  A node that
		 * holds only external keys with no children is never
		 * represented as an internal node.  It is stored as an
		 * external-chain pointer in the parent slot.
		 * (ft_detach_node promotes a childless holder's
		 * external_nodes to the parent when its last child is
		 * removed.)  ft_locate_chain_head would classify it as a
		 * LEAF, not a prefix.
		 *
		 * So clearing external_nodes leaves a valid branch with
		 * children.  It never leaves a childless (dead-end) internal.
		 * A trie WITHOUT the optional ordinal cell list would break
		 * on such a node.  Its ordered traversal descends the
		 * structure, and a dead-end branch has no entry to find.  The
		 * only follow-up is canonicalizing a now-single-child holder
		 * under skip builds.
		 */
		{
			bool prefix_fused = false, fused_declined = false;
			/*
			 * The DERIVATION both arms below freeze against, taken
			 * once.  ft_hlist_freeze_chain_prepare treats it as a
			 * BOUND.  So a duplicate appended since is not this
			 * op's to retire.  It tears the derived tail's NULL
			 * instead.
			 */
			unsigned int nr_frozen = ft_hlist_chain_len(chain_head);
#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * Fuse the chain-compress prune INTO the key-removal
			 * commit (see cds_ft_remove).  This applies when the
			 * holder is left a non-root 1-child no-external
			 * internal.  The merged compressed node then REPLACES
			 * the holder, which subsumes the external_nodes -> NULL
			 * clear.  It carries @dead_cell's unsplice in the SAME
			 * flip.  There is no separate clear and no transient
			 * non-canonical state.  Allocation failure aborts the
			 * whole removal: key not removed, count rolled back,
			 * retriable MEMORY_ERROR with a NULL out-param.
			 */
			if (ft_group_skip_compressed(ft->group) &&
			    ft_meta_nr_child(holder_meta) == 1 &&
			    ft_parent_node(holder_meta->parent_word) != NULL) {
				uint8_t s_byte = 0;
				struct cds_ft_inode_flag *s_child =
					ft_node_get_minmax(ft, holder_flag,
						&s_byte, FT_LEFTMOST, false);
				int cret;

				/*
				 * R3 chain-compress fold: the merged compressed
				 * node replaces the collapsed 1-child holder.
				 * It is built with the retired key's -1.  It
				 * records the ancestor -1 walk into its OWN
				 * commit (count_delta -1).  So there is no
				 * pre-decrement here.
				 */
				/*
				 * INTENT: as above.  No body child is detached.
				 * The external entry this op clears is
				 * @chain_head.  The is_prefix arm derived that
				 * head and is about to unlink it.
				 */
				const struct ft_chain_compress_intent intent = {
					.detach_child = NULL,
					.expect_ext = chain_head,
					.detach_byte = 0,
				};

				/*
				 * No survivor: a peer edited the holder in
				 * place between the two unlocked reads above.
				 * Re-plan.  See the same arm in
				 * _cds_ft_remove_locked.
				 */
				if (caa_unlikely(!s_child))
					cret = -EAGAIN;
				else
					cret = ft_chain_compress_fused(ft,
						holder_flag, holder_depth, &lctx,
						holder_meta,
						s_child, s_byte,
						1 /* sole body child; the removed entry is external */,
						ft->ordered_list ? dead_cell : NULL,
						NULL, NULL, 0, NULL, NULL, 0 /* no orphan chain */,
						chain_head, nr_frozen,
						-1, ft->rank_stats ? key_len + 1 : 0,
						NULL, false,
						NULL /* no pending publish */, &intent, NULL);

				if (cret == 0) {
					/*
					 * ☑ THE CHAIN'S FREEZE RODE THE FUSED
					 * COMMIT (freeze_leaf + nr_frozen above),
					 * under the holder that commit owns.  It
					 * used to be a post-commit sweep whose own
					 * comment conceded the point: "the fuse may
					 * RETIRE @holder_flag ... an acquire that
					 * refuses a tombstone degrades to exactly
					 * today's unheld sweep".  A freeze that is
					 * IN the commit needs no acquire afterwards
					 * and cannot degrade.  It is also
					 * atomic with the unlink.  So no
					 * reader sees the key gone with the
					 * chain still live.
					 */
					if (ft->ordered_list)
						pub.armed = true;
					ret = 0;
					prefix_fused = true;
				} else if (cret < 0) {
					ret = cret;
					prefix_fused = true;
				} else {
					/* Merge out of bound -- fall back to plain clear. */
					fused_declined = true;
				}
			}
#endif
			if (!prefix_fused) {
				/*
				 * Non-fused external_nodes -> NULL clear,
				 * always on a pre-reserved bounded txn (no
				 * bare lone clear even with both flags off).
				 * ft_remove_one_commit flips these TOGETHER
				 * in ONE flip (ft_ord_cell_flip_into,
				 * infallible commit):
				 * - the external_nodes -> NULL clear;
				 * - the head cell's unsplice (ordered list
				 *   on).  @pub.armed tells the deferred-free
				 *   block below that it happened;
				 * - the prefix key's -1 count walk from
				 *   holder_flag (rank stats on);
				 * - the §4.B VALIDATE guard on the LIVE
				 *   holder.
				 * A reader then never sees the key gone from
				 * one index but present in the other.  It
				 * never sees a count out of step with the
				 * structure either.  Parked proxies resolve
				 * via ft_dereference_external, and nr_keys
				 * via ft_nr_keys_load.  A concurrent remove
				 * that froze the holder aborts this clear.
				 *
				 * The holder stays in place (it keeps its
				 * children).  So its -1 is the holder->root
				 * walk, a multi-edge R1.  Pre-reserved =>
				 * infallible commit.  So there is no
				 * pre-decrement and no OOM rollback.  The
				 * only abort is the arm, which leaves the
				 * key in place.  List off + rank off: the
				 * forward clear + the guard, a 2-record
				 * slab-allocated MCAS commit.
				 */
				struct ft_flip_txn *txn = ft_flip_txn_create_bounded(ft,
					FT_REMOVE_COMMIT_REC_MAX_EDGES + 1 +
					(ft->rank_stats ? key_len + 1 : 0) +
					nr_frozen * FT_HLIST_FREEZE_MAX_EDGES);

				if (!txn) {
					if (unsplice_txn)
						ft_flip_txn_destroy(unsplice_txn);
					*result_node = NULL;
					return CDS_FT_STATUS_MEMORY_ERROR;
				}
				/* VALIDATE (§4.B): lock (or guard-fallback) the LIVE
				 * holder.  This clear empties its
				 * external_nodes.  It is the value-swap
				 * target (§10.5). */
				ft_flip_txn_lock_or_guard_parent(ft, txn, &lctx,
					holder_flag, holder_depth);
				/*
				 * ☞ A MISSED TAKE ENDS THE ATTEMPT HERE.
				 * That is before the count walk and the
				 * chain freeze are recorded into a txn the
				 * commit is bound to discard.  The freeze's
				 * chain records are SW whoever holds the
				 * word.  So each is a stale plan against the
				 * holder's own in-flight freeze of the same
				 * chain.  (See the SW stale-old audit on
				 * inv_prefix_shape_zoo: the peer's record on
				 * the same slot, SUCCEEDED.)  Same exit as
				 * the plan bail below.
				 */
				if (caa_unlikely(txn->acquire_miss)) {
					ft_flip_txn_destroy(txn);
					if (unsplice_txn)
						ft_flip_txn_destroy(unsplice_txn);
					*result_node = NULL;
					FT_DBG_RETRY_SITE();
					*need_retry = true;
					return CDS_FT_STATUS_OK;
				}
#if defined(FEATURE_FT_SKIP_COMPRESSED) && !defined(FT_DEBUG_RA_CLEAR_RESIDUE)
				/*
				 * THE PLAIN CLEAR MUST NOT STRAND ITS HOLDER.
				 * The remove twin of this check is in
				 * _cds_ft_remove_locked.
				 *
				 * This arm was chosen from an UNHELD read:
				 * the holder had more than one child, so
				 * clearing its key left a legal node.  A
				 * peer that removed a child before the lock
				 * above changes the answer.  The clear would
				 * then leave a 1-child keyless internal,
				 * which skip mode refuses.  The post-prune
				 * below is a separate commit.  It can lose
				 * to a peer and leave that node.
				 *
				 * The holder is held and nothing is recorded
				 * yet: re-plan.  The retry reads one child
				 * and takes the fused collapse.  This does
				 * not apply where the fused collapse itself
				 * declined (out of bound): the retry would
				 * decline again.
				 */
				if (!fused_declined &&
				    ft_group_skip_compressed(ft->group) &&
				    ft_meta_nr_child_load(holder_meta) == 1 &&
				    ft_parent_node(holder_meta->parent_word) != NULL) {
					ft_flip_txn_destroy(txn);
					if (unsplice_txn)
						ft_flip_txn_destroy(unsplice_txn);
					*result_node = NULL;
					FT_DBG_RETRY_SITE();
					*need_retry = true;
					return CDS_FT_STATUS_OK;
				}
#else
				(void) fused_declined;
#endif
				ft_flip_txn_record_count_parent(ft, txn,
					holder_flag, -1);
				/*
				 * ☑ AND THE CHAIN'S FREEZE RIDES THIS SAME
				 * FLIP.  It is recorded onto @txn, which
				 * already carries the §4.B guard and the
				 * count walk.  So ft_remove_one_commit's
				 * ft_ord_cell_flip_into commits these
				 * together:
				 * - the external_nodes clear;
				 * - the unsplice;
				 * - the count;
				 * - every {v -> MARK(v)}.
				 * The holder is LOCKED by the guard above,
				 * so the whole chain is frozen under its
				 * owner.  This replaces the post-commit
				 * ft_ra_sweep_held.  That sweep had to
				 * re-acquire that word a second time to say
				 * the same thing.  It was also not atomic
				 * with the unlink.
				 *
				 * @nr_frozen is the DERIVATION and the walk
				 * is BOUND by it.  A duplicate appended
				 * since tears the derived tail's NULL and
				 * aborts this commit.  The key is then left
				 * in place for the retry.
				 */
				ft_ch_audit_ctx(ft, txn, &lctx, chain_head);
				/*
				 * ★ VALIDATE THE PLAN UNDER THE LOCK.
				 * @nr_frozen was counted before the §4.B
				 * acquire just above.  So the freeze's
				 * derived tail NULL has been standing in for
				 * an exclusion that the count did not have.
				 * We hold the holder here; ask the word.
				 * (This shrinks the window the install CAS
				 * arbitrates.  It does not replace the CAS;
				 * see ft_hlist_chain_plan_ok's header.)
				 *
				 * ☠ PRE-COMMIT TERMINAL, BOTH HANDLES.  @txn
				 * already carries the §4.B guard, the count
				 * records and the registered holder lock.
				 * @unsplice_txn is a reservation that this
				 * return would otherwise strand.  The tail
				 * that frees them is below.  So is
				 * ft_remove_one_commit, which drains @txn on
				 * a failed commit.  A retry return skips
				 * both.  Leaving without both destroys is
				 * how a bail leaks FT_STATE_LOCK and refuses
				 * every peer the holder forever.
				 */
				if (!ft_hlist_chain_plan_ok(ft, ft_flip_txn_handle(txn),
						chain_head, nr_frozen)) {
					FT_HLIST_PLAN_BAIL();
					ft_flip_txn_destroy(txn);
					if (unsplice_txn)
						ft_flip_txn_destroy(unsplice_txn);
					*result_node = NULL;
					FT_DBG_RETRY_SITE();
					*need_retry = true;
					return CDS_FT_STATUS_OK;
				}
				ft_hlist_freeze_chain_prepare_checked(ft,
					ft_flip_txn_handle(txn), chain_head,
					nr_frozen);
				/*
				 * The commit's status is the ANSWER, not a
				 * formality.  "pre-reserved => infallible"
				 * covers the ALLOCATION.  The §4.B VALIDATE
				 * arm above can still abort.  As that
				 * comment says, an abort LEAVES THE KEY IN
				 * PLACE.  Discarding the status and setting
				 * ret = 0 reported CDS_FT_STATUS_OK for a
				 * removal that did not happen.  A caller
				 * that believes OK reclaims a node still
				 * linked in the trie.  (The
				 * `ret = 0; if (ret == 0)` this replaces was
				 * left over from the dropped status.)
				 */
				ret = ft_remove_one_commit(ft,
					(struct cds_ft_inode_flag **) &holder_meta->external_nodes,
					holder_meta,
					(struct cds_ft_inode_flag *) chain_head, NULL,
					NULL, dead_cell, NULL, txn, NULL, false);
				if (ret == 0) {
					/*
					 * Only a COMMITTED flip carried the
					 * unsplice.  An aborted one leaves the
					 * cell spliced.  The tail's abort arm
					 * then releases the unused reservation.
					 */
					if (ft->ordered_list)
						pub.armed = true;
					assert(ft_meta_nr_child(holder_meta) > 0);
#ifdef FEATURE_FT_SKIP_COMPRESSED
					/*
					 * Out-of-bound residue: best-effort post-prune
					 * via the standalone second flip.
					 * If it still cannot merge, it
					 * leaves a 1-child internal.
					 */
					if (ft_group_skip_compressed(ft->group) &&
					    ft_meta_nr_child(holder_meta) == 1 &&
					    ft_parent_node(holder_meta->parent_word) != NULL) {
						ft_canonicalize_chain_compress(ft, holder_flag,
							holder_depth, &lctx, holder_meta);
					}
#endif
				}
			}
		}
	} else {
		/*
		 * Leaf key: the whole chain sits at head_slot.  That is a body
		 * slot, or a compressed holder's &cn->child.  Removing it
		 * empties the slot.  So prune the branch via ft_detach_node,
		 * bootstrapped from the holder.  It climbs via
		 * metadata->parent.  Propagate -1 before detach, which may free
		 * internal nodes.
		 */
		/*
		 * ☑ THE WHOLE CHAIN'S FREEZE RIDES THE DETACH COMMIT, under the
		 * holder that commit already owns.  It used to be a bare
		 * ft_chain_mark_removed_flip sweep AFTER the detach returned.
		 * That was one unlocked uatomic_cmpxchg per chain node, holding
		 * NOTHING.  It was the hold audit's only surviving class-A
		 * violation: 100% NOLOCKS at every row that reached it.
		 *
		 * ☠ AND IT COULD NOT BE FIXED BY TAKING A LOCK, which is why it
		 * waited for the structural phase.  By sweep time the holder is
		 * GONE, not contended.  The detach retires it IN the very
		 * commit that unlinks the chain.  So the acquire found
		 * FT_STATE_TOMBSTONE (measured: ok=0, refused=22007, of which
		 * 22005 were -EAGAIN on a tombstoned word).
		 *
		 * The acquire could not be hoisted above the detach either:
		 * - Unpublished, it misses against the op's OWN hold
		 *   (ft_detach_node climbs from holder_meta) and waits on
		 *   itself forever.
		 * - Published into @lctx, it becomes the whole-op scope.  That
		 *   scope was measured with a revalidate arm (since removed)
		 *   and refuted (20/41 hangs, reader-visible key loss).
		 * The lock was never the answer.
		 *
		 * @nr_frozen is the DERIVATION.  ft_hlist_freeze_chain_prepare
		 * treats it as a BOUND, not a hint.  A duplicate appended since
		 * is not this op's to retire.  Tearing the derived tail's NULL
		 * aborts the commit, so the retry re-derives the longer chain.
		 */
		unsigned int nr_frozen = ft_hlist_chain_len(chain_head);

		ft_removeall_fault_scope_enter();
		ret = ft_detach_node(ft, &lctx, head_slot,
			ft_get_parent_slot(holder_meta, ft), key_len, true,
			dead_cell, ft->ordered_list ? &pub : NULL, NULL, NULL,
			chain_head, nr_frozen,
			-1 /* leaf key removed: detach owns the -1 */,
			NULL, false, NULL, NULL,
			/* EXPERIMENT: delete tier open, for the trace. */
			ft_in_place_delete_ok(ft));
		ft_removeall_fault_scope_exit();
	}

	/*
	 * -ENOENT (detach's replace reached an emptied FT_NULL slot) is a PEER,
	 * not a bug.  This used to assert it away.  The premise was that the
	 * position "has been found by a mutex-protected traversal within this
	 * function".  No mutex exists here under LOCK_FINE: @head_slot is
	 * derived by ft_locate_chain_head with NOTHING HELD.  So a peer's
	 * recompaction can retype or empty that slot between the derivation and
	 * the replace.  That is exactly the window cds_ft_remove documents at
	 * its own tail.
	 *
	 * The path publishes nothing, so it joins -EAGAIN in the retry below.
	 * It no longer aborts a debug build.  Under NDEBUG, it no longer falls
	 * through to a MEMORY_ERROR that no allocation produced.
	 */

	/* Ordered list on: the whole key left the trie.  Its head's cell is
	 * unspliced and freed (deferred).  chain_head->prev still carries the
	 * cell: detach reshapes ancestors and head_slot, not the head's prev.
	 * An in-place leaf detach already fused the unsplice (pub.armed).
	 * Then only the deferred free remains.  List off: chain_head->prev is
	 * the flagged parent, no cell. */
	if (ft->ordered_list) {
		if (ret == 0) {
			/* An in-place / fused structural commit may
			 * already have carried the unsplice (pub.armed).
			 * Otherwise commit it now, through the txn
			 * pre-reserved before the structural change. */
			if (!pub.armed) {
				/*
				 * ☠ THIS DISCARDED THE STATUS on the premise
				 * that an "ABORT [is] unreachable under its
				 * current exclusion".  The fine locking
				 * conversion DELETES that premise.  The op's
				 * whole-chain standalone marks are already
				 * public here.  So the removal cannot be
				 * retried and the unsplice MUST complete.
				 * An aborted unsplice left the key gone from
				 * the structural index but present in the
				 * ordered list.  @dead_cell was then freed
				 * while it was still spliced into that list.
				 *
				 * DRIVE IT FORWARD instead.  Each ABORT
				 * means a peer committed (obstruction-free).
				 * The aborted commit consumed the txn, so
				 * the small bounded reservation is retried
				 * too.  This is byte-for-byte the tail
				 * cds_ft_remove already runs.
				 */
				FT_DBG_RA_UNSPLICE_ARM();
				while (ft_ord_cell_unsplice(ft, unsplice_txn,
						dead_cell) > 0) {
					FT_DBG_RA_UNSPLICE_ABORT();
					do {
						unsplice_txn =
							ft_flip_txn_create_bounded(ft,
							FT_ORD_CELL_UNSPLICE_MAX_EDGES);
					} while (caa_unlikely(!unsplice_txn));
				}
			} else {
				FT_DBG_RA_UNSPLICE_FUSED();
				ft_flip_txn_destroy(unsplice_txn);
			}
			ft_ord_cell_free(ft, dead_cell);
		} else {
			/* Removal aborted: the cell stays in the list.
			 * Release the unused reservation. */
			ft_flip_txn_destroy(unsplice_txn);
		}
	}

	/*
	 * ☠ KEEP THE KEY.  The retriable exit below re-attempts for this
	 * reason: "@iter's cache is invalidated just above, so the next attempt
	 * re-seeds through a fresh lookup".  That re-seed reads @iter's KEY.
	 * On a keycopy trie, the key is reachable only WHILE @cache_valid holds
	 * (ft_iter_key_referenced -> @iter->node + speculative_key_offset).  A
	 * bare clear here hands the retry a zeroed key.  The re-seed then
	 * reports NOT_FOUND for a key that is present.  This was MEASURED as
	 * inv_compact_keycopy_terminates' drain abort, key 248 of 2000.
	 */
	ft_iter_drop_position_keep_key(iter);

	if (ret) {
		/*
		 * Leaf-key detach ENOMEM: nothing was published.  The chain is
		 * still live in the trie.  The -1 count rides the commit
		 * (ft_flip_txn_record_count_parent), so a failed/aborted commit
		 * discarded it with the rest.  No standalone pre-decrement
		 * remains to restore: the old "count undo" is gone with the
		 * R-fold.  Surface the real error with a NULL out-param, as the
		 * header contract says.  The caller then cannot reclaim the
		 * still-reachable chain.
		 *
		 * -EAGAIN is a peer, -ENOMEM is memory, and the two report
		 * differently.  The distinction is only as good as the sources.
		 * That is what blocked this mapping before.  An acquire that
		 * could not allocate its own lock txn used to reach the commit
		 * as @acquire_miss and abort.  It arrived here as -EAGAIN, with
		 * no allocation failure anywhere in the errno.  That path now
		 * carries @acquire_enomem and commits MEMORY_ERROR instead.  So
		 * every -EAGAIN landing here is a peer.
		 *
		 * ⇒ A PEER IS NOW THIS OP'S OWN PROBLEM, not the caller's.  It
		 * used to be reported as BUSY_ERROR and handed back.  That is
		 * why 91% of remove_all calls FAILED under a same-key peer
		 * (measured: BUSY 25634 vs OK 2473 over 150 rounds).  The
		 * caller's own retry cannot fix that.  A fresh urcu_txn per
		 * attempt ages NO conflict, so it never escalates into the FIFO
		 * lane that drains the contention.
		 *
		 * Signal the wrapper's retry loop instead.  It re-derives the
		 * chain head against the current tree on the PERSISTENT @optxn.
		 * Nothing was published on either errno, so re-attempting is
		 * sound.  @iter's cache is invalidated just above, so the next
		 * attempt re-seeds through a fresh lookup.
		 */
		*result_node = NULL;
		if (ret == -EAGAIN || ret == -ENOENT) {
			FT_DBG_RETRY_SITE();
			*need_retry = true;
			return CDS_FT_STATUS_OK;	/* discarded by the retry loop */
		}
		return CDS_FT_STATUS_MEMORY_ERROR;
	}

	return CDS_FT_STATUS_OK;
}

/*
 * Public entry: FT-owned per-op read-side bracket + retry identity, mirroring
 * cds_ft_remove and cds_ft_replace.
 *
 * WHAT THIS OP WAS MISSING, and it is not what the other three were.
 * remove_all already ACQUIRES and RE-VALIDATES everything it derives:
 * - The leaf arm goes through ft_detach_node, the same primitive the
 *   converted point remove uses.
 * - The prefix arm goes through ft_flip_txn_lock_or_guard_parent, plus a
 *   commit whose expected-old IS the re-validation.
 * Measured against a same-key peer, the structure was never corrupted.  Over
 * 150 rounds x 64 keys x 3 duplicates, no node was lost and no node was
 * double-owned.  Every round verified green.
 *
 * What it lacked was PROGRESS.  The body ran EXACTLY ONCE.  Each of those
 * aborts was correct and had published nothing.  Yet each one was handed to
 * the caller as BUSY_ERROR.  91% of the calls failed that way.
 *
 * ⇒ the conversion is the LOOP, not new exclusion.  Aging is carried on the
 * PERSISTENT @optxn via ft_op_conflict (ft_txn_attempt_bail).  After its lane
 * budget of conflicts, this writer escalates into the per-trie FIFO
 * fair-mutex lane.  That lane drains the contention, so the retry TERMINATES.
 * That is also why a caller looping on BUSY_ERROR was never an adequate
 * substitute: a fresh txn per attempt ages nothing and can starve.  An
 * exclusive trie opens nothing and never conflicts.
 *
 * @iter is re-seeded by the body itself: every retriable exit runs after the
 * cache invalidation, so the next attempt re-descends through a fresh lookup.
 */
enum cds_ft_status cds_ft_remove_all(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		struct cds_ft_node **result_node)
{
	struct ft_op optxn;
	struct ft_op_retry op_retry;
	enum cds_ft_status s;
	bool need_retry;

	CDS_FT_SCOPED_WRITER(ft);
	/*
	 * Same rule as cds_ft_remove (ft_iter_redescend_node): with a bulk op
	 * live, the cached position may predate its flip.  Dropping it is
	 * enough here.
	 * When the cache is not valid, the locked body already re-seeds by a
	 * fresh lookup from the root.
	 */
	if (caa_unlikely(ft_bulk_active(ft)))
		ft_iter_drop_position_keep_key(iter);
	ft_op_retry_init(&op_retry, FT_OP_REMOVE_ALL, NULL, 0);
	ft_txn_op_init(ft, &optxn);
	for (;;) {
		need_retry = false;
		ft_op_retry_tick(ft, &op_retry, 0);
		ft_op_begin(&optxn);
		s = _cds_ft_remove_all_locked(ft, iter, result_node,
				&need_retry, &optxn);
		if (!need_retry)
			break;
		/* Age the conflict, keep the FIFO turn, close the attempt. */
		ft_txn_attempt_bail(&optxn, true);
	}
	ft_op_end(&optxn);
	return s;
}

