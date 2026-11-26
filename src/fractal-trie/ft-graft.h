// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-graft.h
 *
 * Userspace RCU library - Fractal Trie: graft / graft_swap + store-at-graft-point + glue.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-graft.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Build-invisible diverge split for cds_ft_graft (the merge variant of
 * ft_split_compressed_graft).
 *
 * The legacy split publishes a 1-child branch, and ft_store_at_graft_point
 * then completes it.  That later attach can fail.  An OOM there leaves a
 * non-canonical internal node live.  This function builds the COMPLETE
 * attach cluster invisibly instead:
 *
 *   [prefix] -> branch{ old_ordinal -> old_suffix -> old_child,
 *                       new_ordinal -> [path] -> payload }
 *
 * Nothing is published and @cn is not freed.  Every edge into LIVE data is
 * recorded as a deferred back-pointer in @glue.  Those edges lead to the
 * displaced @old_child and to the live @payload nodes.  On OOM the caller's
 * ft_glue_abort frees the cluster, and both tries are pristine.
 *
 * The branch is built with BOTH children up front, by two ft_node_set_nth
 * calls.  The second call may reallocate the node.  Both calls use
 * cluster_leaf, so neither child's back-pointer is set during the build.
 * Both back-pointers are then deferred against the FINAL branch.  This
 * avoids the publish-then-complete window.  It also keeps the deferred
 * old-child edge anchored to a stable node.
 *
 * @key/@key_len: full ordinal key being grafted.
 * @diverge_pos:  divergence offset within cn's path (cn->key_bytes).
 * @payload:      source's old root (live), attached at depth @key_len.
 * @src_count:    payload key count, for the deferred propagate.
 *
 * Return 0 on success: @glue then holds the cluster, its publish and
 * attached_nf.  Return -ENOMEM on allocation failure: the caller must then
 * run ft_glue_abort.
 */
/*
 * Find the ONE child of @parent_nf that is not @exclude.  Store its ordinal
 * in @n_ret and its raw slot word in @child_ret.  Return true if there is
 * such a child, false otherwise.
 *
 * The split's old-direction COLLAPSE uses this.  There the src junction has
 * exactly two children, and one of them is the subtree being moved away.
 *
 * The slot word is raw, never resolved.  The caller wires the value back
 * into a slot and matches it against one.  A skip-encoded form must survive
 * that round trip.  ft_node_find_child compares raw words for the same
 * reason.
 */
static
bool ft_node_other_child(struct cds_ft_inode_flag *parent_nf,
		const struct cds_ft_inode_flag *exclude,
		uint8_t *n_ret, struct cds_ft_inode_flag **child_ret)
{
	struct cds_ft_inode *node = ft_node_ptr(parent_nf);
	unsigned int type_index = ft_node_type(parent_nf);
	const struct cds_ft_type *type = &ft_types[type_index];
	unsigned int i;

	switch (type->type_class) {
	case FT_POPCOUNT:
	{
		uint8_t nr_child = ft_popcount_node_get_nr_child(type, node);

		for (i = 0; i < nr_child; i++) {
			struct cds_ft_inode_flag *iter;
			uint8_t v;

			ft_popcount_node_get_ith_pos(type, node, i, &v, &iter);
			if (!iter || iter == exclude)
				continue;
			*n_ret = v;
			*child_ret = iter;
			return true;
		}
		return false;
	}
	case FT_PIGEON:
		for (i = 0; i < FT_ENTRY_PER_NODE; i++) {
			struct cds_ft_inode_flag *iter =
				ft_pigeon_node_get_ith_pos(type, node, i);

			if (!iter || iter == exclude)
				continue;
			*n_ret = (uint8_t) i;
			*child_ret = iter;
			return true;
		}
		return false;
	default:
		return false;
	}
}

static
int ft_split_compressed_graft_build(struct cds_ft *ft,
		struct ft_descent *d,
		const uint8_t *key, size_t key_len,
		unsigned int diverge_pos,
		struct cds_ft_inode_flag *payload,
		unsigned long src_count,
		struct ft_glue *glue,
		const struct ft_held_set *outer)
{
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(d->nf);
	struct cds_ft_metadata *cn_meta =
		cds_ft_item_to_metadata((struct cds_ft_inode *) cn);

	/* Compressed metadata never carries external_nodes (see
	 * ft_split_compressed_insert). */
	assert(!cn_meta->external_nodes);
	/*
	 * F2 node lock, split-retire (MW LOCK_FINE drop).  Fence @cn BEFORE
	 * any plan read, exactly as ft_split_compressed_insert fences the
	 * node it splits.  This build derives its WHOLE plan from @cn and
	 * then RETIRES @cn.  The plan is:
	 * - the diverge slicing over cn->key_bytes;
	 * - the cn_child snapshot;
	 * - the deferred-edge captures.
	 *
	 * A peer may split, grow or recompact @cn.  It may also change
	 * cn_child (X->X') and release @cn.  Such a peer must do one of two
	 * things:
	 * - Hold the fence.  The mark here then fails with -EAGAIN.  This
	 *   op re-descends, and NOTHING is built.
	 * - Abort at commit against the fenced tombstone's precise expected
	 *   old.  ft_glue_txn_commit_edges records {LOCK|s -> TOMBSTONE|s}
	 *   from @split_cn_snap.
	 *
	 * Marking here, not in the caller's pre-swap fence block, closes the
	 * read-then-fence window.  The whole build runs under the fence, so
	 * a cn_child change during the build is caught too.
	 *
	 * The fence is gated on a txn'd graft under the drop.  The txn-less
	 * merge-rekey (glue->txn NULL) and the FT-wide-lock builds keep the
	 * prior behaviour.
	 *
	 * On the fence-miss path nothing is built and @cn is NOT marked: it
	 * is a clean re-descend.  An OOM AFTER the mark leaves @cn marked.
	 * The caller (ft_graft_keylen) then clears it.
	 */
	if (ft->lock_fine && glue->txn && glue->fence_split_cn) {
		struct ft_lock_ctx sctx;
		struct ft_held_anchor sh;

		ft_glue_lock_ctx(glue, &sctx);
		sctx.d = d;
		/*
		 * @outer is the REST of the op's held set.  An EARLIER step of
		 * the caller may have taken marks that reach no registry: the
		 * rekey fold's ft_rekey_cow_stop set.  Such a caller chains its
		 * frame here, exactly as ft_store_at_graft_point_prepare does.
		 *
		 * Under a coarse spacing those marks and @cn's fence collapse
		 * onto one word.  For an in-trie move that word is the trie
		 * root.  Without the frame this acquire refuses a word the op
		 * itself holds.  That refusal is not a failure but a SPIN: the
		 * caller re-descends, rebuilds the identical shape and refuses
		 * again.
		 */
		sctx.held.outer = outer;
		if (ft_acquire_member(ft, &sctx, d->nf, cn_meta, d->depth, &sh))
			return -EAGAIN;	/* peer owns @cn; nothing built */
		/*
		 * A SHARED hit is the dedupe WORKING, not a refusal.  The op
		 * already holds @cn's anchor.  The first acquire recorded the
		 * release and the anchor terminal, so this member owes neither.
		 *
		 * @cn's OWN word is still retired here: the tombstone lands on
		 * the node, not on the shared anchor.  So the node fields are
		 * recorded either way.  @split_cn_shared gates the release
		 * half.
		 */
		glue->split_cn_holder = sh.lock;
		glue->split_cn_snap = sh.lock_snap;
		glue->split_cn_shared = sh.shared;
		glue->split_cn_node = cn_meta;
		glue->split_cn_node_snap = sh.node_snap;
	}
	/*
	 * DROP THE OLD DIRECTION (@glue->drop_old_dir_of).  @cn's one child
	 * is a subtree this same decide MOVES, and @payload is its copy.  So
	 * the old half of the split has nothing to hold.  Build ONLY the new
	 * key's path over the span @cn covered, and publish that in its
	 * place.
	 *
	 * The path is @key[d->depth .. key_len), not the usual
	 * [new_depth .. key_len).  The prefix @cn shares with the new key is
	 * that key's own bytes.  So one ft_build_branch lays prefix, branch
	 * byte and remainder in a single canonical run.  No branch node is
	 * built for a fork that has only one arm left.
	 *
	 * @cn is fenced and deferred-freed exactly as the two-armed split
	 * does.  The retire side is unchanged; only the built shape differs.
	 *
	 * The displaced child gets NO deferred edge.  It is not re-parented:
	 * it is left behind, and the caller retires it as part of the move.
	 */
	if (glue->drop_old_dir_of && glue->drop_old_dir_of == d->nf) {
		struct cds_ft_inode_flag *canon, *top;
		unsigned long moved_keys = src_count;

		canon = ft_compress_single_child_if_needed(ft, payload, glue);
		if (canon == (struct cds_ft_inode_flag *) (long) -ENOMEM)
			return -ENOMEM;
		top = ft_build_branch(ft, key, d->depth, (unsigned int) key_len,
			canon, moved_keys, false, glue);
		if (!top)
			return -ENOMEM;
		ft_glue_set_publish(ft, glue, d->pnf, d->nfp, top);
		ft_glue_defer_free(glue, cn, true);
		/*
		 * Name @cn for the SAME reason the two-armed split names it
		 * (see @split_cn).  A later step of this op that edits @cn
		 * would edit a node this commit destroys.
		 *
		 * @old_dir_deferred stays -1: this arm re-homes nothing and
		 * leaves the old direction behind.  A consumer that needs the
		 * re-home then refuses rather than inventing one.
		 */
		glue->split_cn = cn;
		glue->attached_nf = top;
		glue->old_dir_dropped = true;
		return 0;
	}
	unsigned int suffix_len = cn->len - diverge_pos - 1;
	uint8_t old_ordinal = cn->key_bytes[diverge_pos];
	uint8_t new_ordinal = key[d->depth + diverge_pos];
	unsigned int new_depth = d->depth + diverge_pos + 1;
	bool branch_cluster_leaf = (suffix_len == 0);
	struct cds_ft_inode_flag *old_suffix_flag;
	struct cds_ft_inode_flag *sfx_skip_flag = NULL;
	struct cds_ft_inode_flag *branch_flag, *top_flag;
	struct cds_ft_inode_flag *new_dir, *payload_canon;
	struct cds_ft_inode_flag **slot;
	struct cds_ft_inode *old_branch = NULL;
	unsigned long old_child_nr_keys;
	unsigned long old_sfx_nr_keys = 0;	/* the fresh suffix's: see below */
	bool src_under = false;		/* below the split (old_dir_built jumps past its set) */
	int ret;
	/*
	 * THE ONE @cn->child LOAD, SETTLED.  This build derives its whole plan
	 * from @cn under the F2 fence, and the child is part of that plan.  The
	 * build uses the child three times:
	 * - it dereferences the child for the old nr_keys;
	 * - it wires the child into the fresh suffix node;
	 * - it hands the child to the deferred back-edges.
	 *
	 * A raw load can catch a peer's parked engine proxy, which is a
	 * descriptor-record POINTER.  The fence on @cn's state word does not
	 * exclude one.  A peer mid-commit on this slot has already parked.
	 * Only its COMMIT is arbitrated, against the fenced tombstone's
	 * expected-old.
	 *
	 * Dereferencing that pointer as a node faults.  Copying it into
	 * @sfx->child would publish it.  Resolving yields the value the slot
	 * denotes, and the commit still ratifies the plan.
	 */
	struct cds_ft_inode_flag *cn_child = ft_resolve_flip_proxy(cn->child);

	(void) branch_cluster_leaf;	/* documents intent; both set_nth defer */

	/*
	 * SUBSTITUTE THE OLD DIRECTION'S CHILD (@glue->old_dir_replace), before
	 * anything reads it.  See the field's header.  The run's one child
	 * keeps siblings, so the old half must hold that child MINUS the moved
	 * arm.
	 * Producing it HERE keeps the edit out of the body this split retires.
	 *
	 * Everything downstream then treats BP' as the displaced child.  It
	 * needs no further special case:
	 * - @old_child_nr_keys is read from BP', so the branch's total is BP's
	 *   original count.
	 * - The fresh suffix wires BP' before ft_publish_compressed encodes the
	 *   SKIP_X dual FROM it.
	 * - ft_glue_deferred_index reports -1 for BP', exactly as the drop arm
	 *   leaves @old_dir_deferred.  BP is RETIRED here, not re-homed, so
	 *   there is no re-parent for a later step to find.
	 *
	 * ☠ THAT LAST ITEM IS TRUE OF THESE TWO ARMS ONLY.  The PROMOTE arm
	 * below substitutes a LIVE external head, whose re-parent IS deferred.
	 * So @old_dir_deferred comes back as a VALID INDEX there, naming the
	 * head's edge.  Its one consumer is ft_detach_node's external-promote
	 * (ft-remove.h).  That consumer refuses on `idx < 0`.  Otherwise it
	 * would CLOBBER that entry with its own head.  This is unreachable
	 * today: the rekey driver runs no detach when @old_dir_replace.done.  A
	 * conditional breakpoint on that consumer never fired across the
	 * 3000-shape corpus.  So the consumer now ASSERTS the exclusion rather
	 * than resting on it.
	 *
	 * ☠ ITS BACK EDGE IS A PLAIN STORE, NOT A DEFERRED ONE.  BP' is a FRESH
	 * node no reader can reach until the forward publish.  So its parent
	 * word is a build-invisible interior write.  The txn publishes
	 * reachability, not interiors.  Deferring it instead would record a
	 * re-parent for a node the commit has no live edge to.  The drop arm
	 * makes the same call.
	 */
	/*
	 * PROMOTE.  It is neither of the two arms below.  The drop takes BP's
	 * ONLY child and BP KEEPS a co-located key chain.  So the old direction
	 * is that HEAD, and BP goes away entirely.  ft_detach_node publishes
	 * the promoted chain for exactly this shape.  The rekey fold's own
	 * FT_REKEY_FOLD_PROMOTE writes exactly this value into @graft_c's slot.
	 * This arm is the same answer one lane over: here the slot lives in the
	 * cluster the split is building.
	 *
	 * ☠ THE SUBSTITUTED CHILD IS LIVE AND APP-OWNED.  That is why @live is
	 * set and the suffix build below must DEFER its back edge.  The
	 * COLLAPSE and REPLACE arms substitute a node this build just created,
	 * and may plain-store it.  A head is reachable through BP until the
	 * forward publish flips.  An abort after a plain store would leave the
	 * application's node naming a body this attempt abandoned.
	 */
	if (glue->old_dir_replace.of && glue->old_dir_replace.of == cn_child &&
			ft_node_internal(cn_child) &&
			ft_meta_nr_child(ft_flag_to_metadata(ft, cn_child)) == 1 &&
			ft_flag_to_metadata(ft, cn_child)->external_nodes) {
		struct cds_ft_metadata *bpm = ft_flag_to_metadata(ft, cn_child);
		struct cds_ft_node *head;
		uint8_t rn = 0;

		if (!glue->txn)
			return -EAGAIN;		/* no txn to retire BP into */
		/*
		 * RE-VALIDATE the plan against the body as it stands:
		 * - the slot the drop names must still be BP's;
		 * - BP must still be the shape the caller armed: one child, and
		 *   a key chain to promote.
		 *
		 * Return -EAGAIN, never a shape refusal.  BP is unheld here, so
		 * a peer change is transient and a re-descent clears it.
		 */
		if (!ft_node_find_child(ft, cn_child,
				glue->old_dir_replace.drop_child, &rn, NULL))
			return -EAGAIN;		/* stale plan: re-descend */
		if (ft_meta_nr_child(bpm) != 1 || !bpm->external_nodes)
			return -EAGAIN;		/* BP grew or lost its keys */
		head = ft_dereference_external(bpm->external_nodes);
		if (!head)
			return -EAGAIN;
		/*
		 * ☐ RESIDUAL, INHERITED KNOWINGLY.  It is identical to the
		 * COLLAPSE arm's below.  It is stated here because this arm
		 * widens it to a new object class.
		 *
		 * The re-reads above are UNLOCKED.  Nothing holds BP across the
		 * window between them and the commit.  In that window a peer
		 * may splice a NEW head onto this chain, or add a second child.
		 * What the peer added is then retired along with BP.
		 *
		 * The rekey FOLD's own PROMOTE closes exactly this with
		 * @promote_val plus
		 * ft_detach_orphan_planlock(require_single_child).  It can,
		 * because it freezes the node in a later phase.  This arm IS
		 * the build and has no such phase.
		 *
		 * Closing it needs the split to plan-lock BP.  That is a change
		 * to the glue's ownership model rather than to this arm.  The
		 * COLLAPSE arm carries the same sentence.
		 */
		/*
		 * BP is RETIRED, not recompacted.  Nothing is left of it once
		 * its one child leaves and its keys move up.  This is the plain
		 * retire, on the same footing as @cn and the COLLAPSE arm's BP.
		 * ☞ The header there says why a FENCED free-list entry cannot
		 * be used here.
		 */
		ft_glue_defer_free(glue, ft_node_ptr(cn_child), false);
		cn_child = (struct cds_ft_inode_flag *) head;
		glue->old_dir_replace.done = true;
		glue->old_dir_replace.live = true;
		goto no_old_dir_replace;
	}
	if (glue->old_dir_replace.of && glue->old_dir_replace.of == cn_child &&
			ft_node_internal(cn_child) &&
			ft_meta_nr_child(ft_flag_to_metadata(ft, cn_child)) == 2 &&
			!ft_flag_to_metadata(ft, cn_child)->external_nodes) {
		/*
		 * COLLAPSE, not replace.  BP has exactly two children and no keys
		 * of its own.  Dropping the moved arm would leave a ONE-CHILD
		 * internal.  That is not a legal shape in skip mode
		 * (ft_verify: "1 child and no external_nodes (should be a 1-byte
		 * compressed)").  So copying BP minus the arm is the wrong
		 * answer here.  The old direction is ONE RUN and BP goes away
		 * entirely:
		 *
		 *   [cn suffix] ++ [surviving ordinal] (++ [absorbed run]) -> subtree
		 *
		 * ft_build_branch lays that run canonically.  On the way,
		 * ft_try_compress_chain ABSORBS a compressed surviving child
		 * into the same run.  It defers the final child's back-pointer
		 * and the absorbed node's retire.  The "no two adjacent
		 * compresseds" invariant is why the fusion has to happen here.
		 * Stacking a suffix node on a collapsed BP' is not an
		 * alternative.  The plain REPLACE below cannot express this
		 * fused chain-compress.
		 *
		 * ☠ THE GAP ft_glue_defer_free_fenced WAS WRITTEN FOR.  BP's
		 * body is READ here (the surviving ordinal and child).  The run
		 * is built from what was read.  A plain retire takes no lock
		 * over that window.  A child that a peer adds to BP in that
		 * window would be retired along with BP: silent key loss.  The
		 * retire below is plain all the same.  The comment there says
		 * why.
		 */
		struct cds_ft_metadata *bpm = ft_flag_to_metadata(ft, cn_child);
		struct cds_ft_compressed_node *scn = NULL;
		struct cds_ft_inode_flag *surv = NULL;
		uint8_t buf[FT_MAX_KEY_LEN];
		unsigned int plen, plen_total;
		uint8_t sb = 0;

		if (!glue->txn)
			return -EAGAIN;
		if (!ft_node_other_child(cn_child,
				glue->old_dir_replace.drop_child, &sb, &surv))
			return -EAGAIN;		/* stale plan: re-descend */
		if (ft_node_skip_compressed(surv))
			scn = ft_skip_to_compressed(ft, surv);
		else if (ft_node_compressed(surv))
			scn = ft_compressed_node_ptr(surv);
		plen = suffix_len + 1U;
		plen_total = plen + (scn ? (unsigned int) scn->len : 0U);
		/*
		 * ☞ AN OVERLONG RUN IS NOT REFUSED HERE.  A refusal inside the
		 * build could only answer -EAGAIN.  The shape is deterministic,
		 * so the op would re-plan onto it for ever.  Leave the replace
		 * UNARMED instead.  The caller's own shape gate then answers
		 * terminally, exactly as it did before this arm existed.
		 */
		if (plen > sizeof(buf) ||
				(ft_group_skip_compressed(ft->group) &&
					plen_total > FT_SKIP_LEN_MAX))
			goto no_old_dir_replace;
		if (suffix_len)
			memcpy(buf, &cn->key_bytes[diverge_pos + 1], suffix_len);
		buf[suffix_len] = sb;
		/*
		 * The surviving subtree's key count: BP held the moved arm and
		 * this one, and the moved arm's @src_count leaves.  The branch
		 * below then takes @old_child_nr_keys + src_count -- BP's original
		 * total -- so the cluster's net delta stays zero.
		 */
		old_child_nr_keys = ft_nr_keys_get(bpm) - src_count;
		old_suffix_flag = ft_build_branch(ft, buf, 0, plen, surv,
			old_child_nr_keys, false, glue);
		if (!old_suffix_flag)
			return -ENOMEM;
		if (ft_node_compressed(old_suffix_flag))
			sfx_skip_flag = ft_publish_compressed(ft,
				ft_compressed_node_ptr(old_suffix_flag),
				old_suffix_flag);
		/*
		 * ☞ THE PLAIN RETIRE, on the SAME footing the rest of this build
		 * uses.  A fenced free-list entry records its
		 * {LOCK|s -> TOMBSTONE|s} terminal INTO @txn.  It therefore
		 * asserts @fuse_free_list, which promotes EVERY entry to a txn
		 * record.  That includes @split_cn and the compressed node
		 * ft_try_compress_chain absorbs.  This build owns both through
		 * the GLUE rather than the txn registry.  They would then fail
		 * the record-time owner check.  So BP is retired exactly as @cn
		 * and the absorbed run are: by a standalone tombstone flip.
		 *
		 * ☐ RESIDUAL.  It is the one ft_glue_defer_free_fenced's header
		 * names.  The plain retire takes no lock across the window in
		 * which this build READ BP's body (the surviving ordinal and
		 * child).  A THIRD child that a peer adds to BP inside that
		 * window would be retired with it.  Closing this needs every
		 * free-list node to be owned by the txn.  That is a change to
		 * the glue's ownership model rather than to this arm.
		 */
		ft_glue_defer_free(glue, ft_node_ptr(cn_child), false);
		glue->old_dir_replace.done = true;
		glue->old_dir_replace.collapsed = true;
		goto old_dir_built;
	}
	if (glue->old_dir_replace.of && glue->old_dir_replace.of == cn_child) {
		struct cds_ft_inode_flag *fresh = cn_child;
		struct ft_remove_pub rpub;
		struct ft_lock_ctx rctx;
		uint8_t rn = 0;

		if (!glue->txn)
			return -EAGAIN;		/* no txn to retire BP into */
		if (!ft_node_find_child(ft, cn_child,
				glue->old_dir_replace.drop_child, &rn, NULL))
			return -EAGAIN;		/* stale plan: re-descend */
		memset(&rpub, 0, sizeof(rpub));
		ft_glue_lock_ctx(glue, &rctx);
		/*
		 * The slot being cleared is on the SRC path, so the descent that
		 * dates BP is the one that walked it.  @outer carries the rest of
		 * the op's held set, exactly as the fence acquire above uses it.
		 */
		if (glue->lock_d_src)
			rctx.d = glue->lock_d_src;
		rctx.held.outer = outer;
		ret = ft_node_replace_ptr(ft, glue->old_dir_replace.drop_slot,
				glue->old_dir_replace.drop_expected,
				&fresh, &glue->old_dir_replace.old_body,
				ft_flag_to_metadata(ft, cn_child),
				rn, NULL /*delete*/,
				false /*bulk: never in place*/,
				false /*is_root: a child of @cn*/,
				glue->old_dir_replace.depth, &rpub, glue->txn,
				NULL /*held_hint: no publish to guard*/,
				&rctx);
		if (ret)
			return ret == -ENOMEM ? -ENOMEM : -EAGAIN;
		/*
		 * An IN-PLACE recompaction leaves the live node in hand.  There
		 * is then no fresh body to wire.  BP would be edited where it
		 * stands, under a node this flip retires.  Refuse rather than
		 * publish that.  The caller's tier gate keeps this case away.
		 * This guard does not depend on the gate being right.
		 */
		if (fresh == cn_child)
			return -EAGAIN;
		/*
		 * ☠ BAKE THE POST-REMOVAL COUNT, exactly as the COLLAPSED arm
		 * above computes @old_child_nr_keys.  @fresh is BP minus the
		 * moved arm, and it SURVIVES with every other key.  The
		 * caller's net-zero (`old_dir_replace.done ? 0 : cnt`) speaks
		 * only for the publish parent.  The subtree leaves the run and
		 * re-enters under the same slot, so no ANCESTOR moves.  But BP
		 * is not an ancestor of the destination: it is the node the
		 * subtree left.  @fresh came out of ft_node_recompact carrying
		 * BP's OLD total.  The detach that normally bakes -@cnt into
		 * such a copy is skipped for this shape.
		 *
		 * This is a build-invisible plain store, on a copy nothing
		 * reaches until the flip.  It is a no-op when order statistics
		 * are off.
		 *
		 * MEASURED, rank stats on.  The sequence was:
		 *
		 *   insert {"abaccb","abcc","ba"}
		 *   rekey_merge(dst "aacac", src "abc")
		 *
		 * It returned OK with every key at the right name, and
		 * cds_ft_count_keys answered 3.  cds_ft_verify answered
		 * `depth 1: nr_keys mismatch: stored 3, computed 2`.
		 *
		 * This is the COMPRESSING-build half of a defect the rekey
		 * driver's FT_REKEY_FOLD_REPLACE arm also had (@7000b65e).  It
		 * hit 6 of 3000 shapes on the default build.  It was the last
		 * of the rank lane's residue.
		 */
		ft_nr_keys_store(ft, ft_flag_to_metadata(ft, fresh),
			ft_nr_keys_get(ft_flag_to_metadata(ft, fresh)) -
				src_count, CMM_RELAXED);
		glue->old_dir_replace.fresh = fresh;
		glue->old_dir_replace.done = true;
		ft_glue_track(glue, fresh);
		cn_child = fresh;
	}

no_old_dir_replace:
	/* Compute old child's nr_keys. */
	if (!ft_node_external(cn_child)) {
		struct cds_ft_metadata *cm =
			cds_ft_item_to_metadata(ft_node_ptr(cn_child));
		old_child_nr_keys = ft_nr_keys_get(cm);
	} else if (cn_child) {
		old_child_nr_keys = 1;
	} else {
		old_child_nr_keys = 0;
	}
	/*
	 * THE FRESH NODES BELOW THE TOP, when the source lives BELOW the split.
	 * @old_child_nr_keys is @cn_child's PRE-move total.  The source detach's
	 * -@src_count walk climbs the pre-op chain and reaches @cn's slot.
	 * There it is redirected onto the published TOP (@pending_pub_slot).
	 * It charges the top, and ONLY the top.  So the top is built pre-move,
	 * and the walk settles it.  Every fresh node strictly below the top on
	 * the old direction's path is on neither chain.  Such a node must be
	 * born post-move:
	 * - the suffix run takes @old_child_nr_keys - @src_count;
	 * - the junction itself takes @old_child_nr_keys, when a prefix sits
	 *   above it (@diverge_pos > 0).
	 *
	 * The substituted-child arms above already read a post-move count, hence
	 * !@old_dir_replace.done.
	 *
	 * MEASURED on every build, with rank stats on.  The defect is
	 * pre-existing (the rekey shape corpus).  Two shapes, each with what
	 * cds_ft_verify printed:
	 *
	 *   The suffix:
	 *     {aaaaa, baababbbabba, a, baabbbbbaaab, aaba, abbbbaaaaba,
	 *      abbaabab, baaaaabbbbb, aaabbbba}
	 *     + rekey_merge(dst "bbaaa", src "baabb")
	 *     -> `depth 2: compressed node nr_keys mismatch: stored 3,
	 *        computed 2`
	 *
	 *   The junction under a prefix:
	 *     {aabbbbbbb, a, bbabbaaab, bbaaabbaba, abba, bbabab, aabbb}
	 *     + rekey_merge(dst "bbb", src "bbabb")
	 *     -> `depth 2: internal node ... stored 4, computed 3`
	 */
	{
		size_t cn_end = (size_t) d->depth + cn->len;

		src_under = glue->src_key && !glue->old_dir_replace.done &&
			glue->src_len >= cn_end &&
			!memcmp(glue->src_key, key, d->depth) &&
			!memcmp(glue->src_key + d->depth, cn->key_bytes,
				cn->len) &&
			old_child_nr_keys >= src_count;
#ifdef FT_DEBUG_SPLIT_PRECOUNT
		src_under = false;
#endif
		old_sfx_nr_keys = old_child_nr_keys -
			(src_under ? src_count : 0);
	}

	/*
	 * 1. Build the OLD-direction suffix -> old child (mirrors the legacy
	 * split).  cn_child (live) is deferred into @glue.
	 */
	if (suffix_len >= 2
#ifdef FEATURE_FT_SKIP_COMPRESSED
			|| (suffix_len == 1 && ft_group_skip_compressed(ft->group))
#endif
	   ) {
		struct cds_ft_compressed_node *sfx;
		struct cds_ft_metadata *sfx_meta;

		sfx = alloc_compressed_node(ft, suffix_len, &sfx_meta);
		if (!sfx)
			return -ENOMEM;
		sfx->child = cn_child;
		sfx->len = suffix_len;
		memcpy(sfx->key_bytes, &cn->key_bytes[diverge_pos + 1],
			suffix_len);
		ft_meta_nr_child_set(sfx_meta, 1);
		ft_nr_keys_store(ft, sfx_meta, old_sfx_nr_keys, CMM_RELAXED);
		old_suffix_flag = ft_compressed_node_flag(sfx);
		sfx_skip_flag = ft_publish_compressed(ft, sfx, old_suffix_flag);
		ft_glue_track(glue, old_suffix_flag);
		/*
		 * The displaced child @cn_child is LIVE.  A reader can still
		 * descend to it through @cn.  @cn is the compressed node being
		 * split, and it is untouched until the forward publish replaces
		 * it.  So the re-parent of @cn_child onto the fresh @sfx is a
		 * reader-observable pointer.  Ride it on the flip-txn
		 * (dst_origin), so that it flips atomically with the forward
		 * edge.
		 *
		 * This holds for an external @cn_child too.  The up-walk
		 * readers resolve a flip proxy parked on an external's parent:
		 * ft_get_parent_rcu / ft_skip_to_compressed / ft_skip_reanchor.
		 *
		 * Every caller of the build creates @glue->txn first:
		 * cds_ft_graft, cds_ft_merge_at and the rekey.  So the edge is
		 * recorded.
		 */
		if (glue->old_dir_replace.done && !glue->old_dir_replace.live)
			ft_set_parent(ft, cn_child, old_suffix_flag,
				&sfx->child);
		else
			ft_glue_defer_edge_origin(ft, glue, cn_child,
				old_suffix_flag, &sfx->child,
				glue->txn != NULL);
	} else if (suffix_len == 1) {
		struct cds_ft_inode_flag *dest = NULL;

		/* 1-child internal suffix (non-SC): cluster-leaf. */
		ret = ft_node_set_nth(ft, &dest,
				cn->key_bytes[diverge_pos + 1],
				cn_child, NULL, NULL,
				d->depth + diverge_pos + 1, true);
		if (ret)
			return -ENOMEM;
		ft_nr_keys_store(ft, cds_ft_item_to_metadata(ft_node_ptr(dest)),
			old_sfx_nr_keys, CMM_RELAXED);
		old_suffix_flag = dest;
		ft_glue_track(glue, dest);
		ft_node_get_nth_skip(dest, &slot,
			cn->key_bytes[diverge_pos + 1], FT_PF_NONE);
		/* Displaced child: LIVE, ride the txn -- see the suffix_len>1 case. */
		if (glue->old_dir_replace.done && !glue->old_dir_replace.live)
			ft_set_parent(ft, cn_child, dest, slot);
		else
			ft_glue_defer_edge_origin(ft, glue, cn_child, dest,
				slot, glue->txn != NULL);
	} else {
		old_suffix_flag = cn_child;	/* suffix_len == 0 */
	}

old_dir_built:
	/*
	 * 2. Build the NEW-direction subtree: canonicalize the payload, then
	 * (when the key extends past the branch) a path down to it.  All
	 * build-invisible; payload back-pointers deferred via @glue.
	 */
	payload_canon = ft_compress_single_child_if_needed(ft, payload, glue);
	if (payload_canon == (struct cds_ft_inode_flag *) (long) -ENOMEM)
		return -ENOMEM;
	if (new_depth == key_len) {
		new_dir = payload_canon;
	} else {
		new_dir = ft_build_branch(ft, key, new_depth, key_len,
			payload_canon, src_count, false, glue);
		if (!new_dir)
			return -ENOMEM;
	}

	/*
	 * 3. Build the branch with BOTH children.  cluster_leaf on both
	 * set_nth: no child back-pointer is set during the build (the second
	 * set_nth may reallocate the branch).  Both are deferred below
	 * against the final branch.
	 */
	branch_flag = NULL;
	ret = ft_node_set_nth(ft, &branch_flag, old_ordinal, old_suffix_flag,
			NULL, NULL, d->depth + diverge_pos, true);
	if (ret)
		return -ENOMEM;
	ft_glue_track(glue, branch_flag);
	/*
	 * The branch is a fresh, unpublished node with no parent yet (it is
	 * wired to its prefix only at commit).  Clear its parent / skip_slot
	 * before the second child may reallocate it.  ft_node_recompact
	 * inherits the old node's parent.  If that parent looks like a
	 * compressed node, ft_node_recompact writes through its skip_slot.
	 * A recycled allocation can leave stale, non-NULL values there.
	 * The write would then corrupt an unrelated live node.
	 */
	{
		struct cds_ft_metadata *bm =
			cds_ft_item_to_metadata(ft_node_ptr(branch_flag));

		bm->parent_word = NULL;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		ft_meta_parent_slot_offset_set(bm, 0);
#endif
	}
	/*
	 * Second child.  The branch is now an existing (unpublished) node,
	 * so pass its metadata for the in-place update.  On overflow it
	 * reallocates, and the old order-1 copy is returned via @old_branch.
	 */
	ret = ft_node_set_nth(ft, &branch_flag, new_ordinal, new_dir,
			&old_branch,
			cds_ft_item_to_metadata(ft_node_ptr(branch_flag)),
			d->depth + diverge_pos, true);
	if (ret)
		return -ENOMEM;
	if (old_branch) {
		/*
		 * Reallocated: drop the order-1 copy from tracking + RETIRE it.
		 *
		 * The grow ran through ft_node_recompact, which marks every body
		 * it supersedes DEAD (§4.B freeze-on-free).  It states the
		 * matching obligation: "the caller ... frees @old_node after a
		 * grace period".
		 *
		 * This cluster is build-invisible, but the unpublished free
		 * path is still the wrong one.  That path proves its contract
		 * by asserting the node carries NO tombstone.  A marked body
		 * freed there reads as a live-node free.  ft-insert.h's own
		 * recompact-on-grow (@old_recompacted) already frees through
		 * here.
		 */
		ft_glue_untrack(ft, glue, old_branch);
		free_cds_ft_node(ft, old_branch);
		ft_glue_track(glue, branch_flag);
	}
	/*
	 * Order-statistics fold (BULK).  Build the fresh cluster's junction /
	 * prefix with their FULL post-commit count.  That count is the old
	 * direction's SURVIVING keys PLUS the +src_count payload.  The
	 * graft/merge attach fold then records only the +src_count walk from
	 * the STABLE @publish_parent above the cluster.  This is a
	 * build-invisible plain store, and a no-op when rank stats are off.
	 * The OLD-direction suffix nodes keep old_child_nr_keys (they hold no
	 * payload).
	 *
	 * ☠ Use @old_child_nr_keys, NEVER @cn_meta's OWN count.  The prefix
	 * arms below say the same.  The two are the same number for an
	 * ordinary split: @cn carries no external heads on this path, so the
	 * run's count IS its child's.  They DIVERGE for exactly one shape: a
	 * same-trie move whose SOURCE lives inside the run being split.  The
	 * old direction is then rebuilt WITHOUT the moved arm
	 * (@old_dir_replace, either the collapsed or the recompacted arm).
	 * So it sheds @src_count, while @cn_meta still reads the pre-op
	 * total.  The junction is then born holding the moved keys TWICE.
	 *
	 * MEASURED, rank stats on.  The sequence was:
	 *
	 *   insert {"abaccb","abcc","ba"}
	 *   rekey_merge(dst "aacac", src "abc")
	 *
	 * It returned OK with every key at the right name, and
	 * cds_ft_count_keys answered 3.  cds_ft_verify answered
	 * `depth 1: nr_keys mismatch: stored 3, computed 2`.  Traced store by
	 * store, the junction is written 2 here.  The @diverge_pos == 0 arm
	 * below then OVERWROTE it with 3, re-deriving it from @cn_meta.
	 */
	ft_nr_keys_store(ft, cds_ft_item_to_metadata(ft_node_ptr(branch_flag)),
		old_child_nr_keys + src_count -
			(src_under && diverge_pos > 0 ? src_count : 0),
		CMM_RELAXED);

	/* Wire the OLD direction (re-encode compressed slot to skip form). */
	ft_node_get_nth_skip(branch_flag, &slot, old_ordinal, FT_PF_NONE);
	if (sfx_skip_flag && sfx_skip_flag != old_suffix_flag && slot)
		*slot = sfx_skip_flag;
	/*
	 * The origin of this edge depends on suffix_len:
	 * - suffix_len == 0: @old_suffix_flag IS the live @cn_child, wired
	 *   directly under the branch (no fresh suffix node).  This edge
	 *   re-parents a reader-reachable node: dst_origin (ride the txn).
	 * - suffix_len > 0: @old_suffix_flag is the fresh sfx/dest.  This is
	 *   a hidden edge (src-origin).
	 *
	 * ☠ @old_dir_replace.done DOES NOT MEAN "FRESH".  It did while the
	 * only two arms substituted a node this build had just created (BP'
	 * recompacted, or the collapsed run).  The term above reads that way.
	 * The PROMOTE arm substitutes the app's OWN EXTERNAL HEAD.  That head
	 * is live and reader-reachable through BP until this commit flips.
	 * So its edge must ride the txn, exactly as an unsubstituted
	 * @cn_child's does.
	 *
	 * @live separates the two.  It is checked here as well as at the two
	 * suffix sites.  MEASURED: 4 of the 5 promote shapes in the
	 * 3000-shape corpus reach the branch through suffix_len == 0.  They
	 * touch NEITHER suffix site, so this site decides them.
	 */
	ft_glue_defer_edge_origin(ft, glue, old_suffix_flag, branch_flag, slot,
		suffix_len == 0 &&
			(!glue->old_dir_replace.done ||
				glue->old_dir_replace.live) &&
			glue->txn != NULL);
	/*
	 * ☠ NAME WHAT THIS BUILD REPLACED (see @split_cn), and name it HERE.
	 *
	 * This commit retires @cn, and re-homes its one live child @cn_child
	 * under the fresh cluster.  A LATER step of the same op cannot derive
	 * these two facts from the arguments it is handed.  The step that
	 * needs them is the detach's external-promote.  Without them it
	 * publishes into @cn->child and refreshes @cn's SKIP_X dual.  That is
	 * the very word the forward publish below repoints.  The result is
	 * two records on one slot whose expected-olds disagree.  A DEBUG_RCU
	 * build asserts on that in urcu_txn_sw_record_chain().  Under the MCAS
	 * engine it poisoned the descriptor, and the op re-planned for ever
	 * (ft-remove.h's promote arm carries the measurement).
	 *
	 * Name it AFTER the branch is final, never at the defer above.  The
	 * second ft_node_set_nth may reallocate the branch.  A slot captured
	 * before that points into a body already freed.  The index survives
	 * the move because ft_glue_defer_edge_origin rewrites the entry in
	 * place.
	 */
	glue->split_cn = cn;
	glue->old_dir_via_suffix = (suffix_len != 0);
	glue->old_dir_deferred = ft_glue_deferred_index(glue, cn_child);
	/* Wire the NEW direction. */
	ft_node_get_nth_skip(branch_flag, &slot, new_ordinal, FT_PF_NONE);
	if (ft_node_compressed(new_dir) && slot) {
		/*
		 * @new_dir is a PLAIN compressed flag (compress and the
		 * build-invisible ft_build_branch both return the plain form
		 * so the deferred edge recovers it via ft_compressed_node_ptr).
		 * Re-encode the holding slot to the skip form so the published
		 * trie is canonical; it resolves once the deferred grandchild
		 * back-pointer is applied at commit.
		 */
		struct cds_ft_inode_flag *skip = ft_publish_compressed(ft,
			ft_compressed_node_ptr(new_dir), new_dir);

		if (skip != new_dir)
			*slot = skip;
	}
	ft_glue_defer_edge(ft, glue, new_dir, branch_flag, slot);

	/* 4. Build prefix -> branch (no external_nodes on @cn). */
	if (diverge_pos >= 2) {
		struct cds_ft_compressed_node *pfx;
		struct cds_ft_metadata *pfx_meta;

		pfx = alloc_compressed_node(ft, diverge_pos, &pfx_meta);
		if (!pfx)
			return -ENOMEM;
		pfx->child = branch_flag;
		pfx->len = diverge_pos;
		memcpy(pfx->key_bytes, cn->key_bytes, diverge_pos);
		ft_meta_nr_child_set(pfx_meta, 1);
		ft_nr_keys_store(ft, pfx_meta,
			old_child_nr_keys + src_count, CMM_RELAXED);
		top_flag = ft_compressed_node_flag(pfx);
		/* HIDDEN: fresh branch under a prefix allocated here; the
		 * ft_glue_track below registers it as this build's. */
		ft_set_parent_excl(ft, branch_flag, top_flag, NULL,
			FT_EXCL_HIDDEN);
		/* Track the PLAIN form; the skip form is for the publish. */
		ft_glue_track(glue, top_flag);
		top_flag = ft_publish_compressed(ft, pfx, top_flag);
	} else if (diverge_pos == 1) {
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_group_skip_compressed(ft->group)) {
			struct cds_ft_compressed_node *pfx;
			struct cds_ft_metadata *pfx_meta;

			pfx = alloc_compressed_node(ft, 1, &pfx_meta);
			if (!pfx)
				return -ENOMEM;
			pfx->child = branch_flag;
			pfx->len = 1;
			pfx->key_bytes[0] = cn->key_bytes[0];
			ft_meta_nr_child_set(pfx_meta, 1);
			ft_nr_keys_store(ft, pfx_meta,
				old_child_nr_keys + src_count, CMM_RELAXED);
			top_flag = ft_compressed_node_flag(pfx);
			/* HIDDEN: same shape as the diverge_pos > 1 arm above. */
			ft_set_parent_excl(ft, branch_flag, top_flag,
				&pfx->child, FT_EXCL_HIDDEN);
			/* Track the PLAIN form; skip form for the publish. */
			ft_glue_track(glue, top_flag);
			top_flag = ft_publish_compressed(ft, pfx, top_flag);
			goto after_prefix;
		}
#endif
		{
		struct cds_ft_inode_flag *dest = NULL;
		struct cds_ft_metadata *pfx_meta;

		ret = ft_node_set_nth(ft, &dest, cn->key_bytes[0],
				branch_flag, NULL, NULL, d->depth, false);
		if (ret)
			return -ENOMEM;
		pfx_meta = cds_ft_item_to_metadata(ft_node_ptr(dest));
		ft_nr_keys_store(ft, pfx_meta,
			old_child_nr_keys + src_count, CMM_RELAXED);
		top_flag = dest;
		ft_glue_track(glue, dest);
		}
#ifdef FEATURE_FT_SKIP_COMPRESSED
	after_prefix:
		(void) 0;
#endif
	} else {
		/* diverge_pos == 0: branch IS the top. */
		ft_nr_keys_store(ft, cds_ft_item_to_metadata(ft_node_ptr(branch_flag)),
			old_child_nr_keys + src_count, CMM_RELAXED);
		top_flag = branch_flag;
	}

	/*
	 * 5. Record the single publish (top -> d->pnf's slot) and the old
	 * compressed node to free at commit.  attached_nf == new_dir: it
	 * carries the payload's key count, and the propagate starts from its
	 * parent.
	 */
	ft_glue_set_publish(ft, glue, d->pnf, d->nfp, top_flag);
	ft_glue_defer_free(glue, cn, true);
	glue->attached_nf = new_dir;
	return 0;
}

/*
 * The NOSPLIT graft-point store.  It is split into a fallible build (prepare)
 * and a failure-free publish (commit), so that the two halves can straddle
 * the caller's source mutation.  The two callers use it differently:
 * - cds_ft_graft runs both halves back-to-back, via the combined
 *   ft_store_at_graft_point wrapper below.  It does so after it has unlinked
 *   the source root and drained.
 * - cds_ft_merge_at's diverged-dst move runs prepare while the source is
 *   still pristine.  It then unlinks the source subtree, drains, and
 *   commits.  So its only fallible dst step finishes before the source is
 *   touched, and there is no rollback to leak from.
 *
 * (The diverging case is built invisibly by ft_split_compressed_graft_build,
 * not here.)
 *
 * Two non-diverging attach shapes:
 * - d->depth == key_len: the slot exists; add via ft_node_set_nth.
 * - d->depth < key_len: build intermediate internal nodes via ft_build_branch,
 *   displacing any external node on the path into the branch's metadata.
 *
 * prepare is non-destructive to the payload (live source data):
 * - the payload's back-pointers route through @glue;
 * - ft_compress_single_child_if_needed is non-destructive in glue mode;
 * - the forward slot is parked behind a flip proxy that resolves to the
 *   (empty) old value.
 *
 * So an OOM in prepare leaves both the payload and the destination pristine.
 * The caller then runs ft_glue_abort.
 *
 * commit applies the deferred edges, swings the forward publish and reclaims
 * the replaced source root (if canonicalized).  It cannot fail.
 */
struct ft_graft_store_state {
	struct ft_glue *glue;
	bool displaced_shape;
	struct cds_ft_inode_flag *attached;		/* payload (at-node) or branch */
	unsigned int attached_depth;
	/* flip publish (slot-at-node and built-branch-flip shapes): */
	struct cds_ft_inode_flag *dest;
	struct cds_ft_inode *old_recompacted_node;
	/*
	 * A recompact-relocating reserve records a compressed grandparent's
	 * SKIP_X dual here, instead of a premature bare store.  The commit
	 * folds the forward grandparent slot edge in alongside it.  Both then
	 * flip ATOMICALLY in glue->txn.
	 */
	struct ft_pub_rec reserve_rec;
	/*
	 * Set iff the reserve added its byte's occupancy IN PLACE on the LIVE
	 * @dest.  This graft then still owes @dest an nr_child++.  The
	 * commit's in-place arm records it into glue->txn.  So the count goes
	 * live with the publish, and is dropped with an aborted attempt.  A
	 * reserve that RELOCATED built the count into its fresh copy and
	 * leaves this false.  See ft_flip_txn_record_nr_child_inc.
	 */
	bool count_deferred;
	struct cds_ft_metadata *publish_pmeta;
	struct cds_ft_inode_flag **pnfp;
	struct cds_ft_inode_flag *slot_value;
	uint8_t slot_byte;
	/* displaced publish (built branch absorbing an existing external): */
	struct cds_ft_node *displaced;
	struct cds_ft_inode_flag *pnf;
	struct cds_ft_inode_flag **nfp;
	const uint8_t *tp_key;
	unsigned int tp_i;
};

/*
 * Flip-batch capacity for a run-fused graft store.  These edges are all
 * committed in ONE flip:
 * - 1 structural slot edge;
 * - the <=4 boundary edges of the ordered-list run-splice (ft_graft_run).
 *
 * A run-less store needs only the single slot edge.
 */
#define FT_GRAFT_RUN_FLIP_CAP	5

static
enum cds_ft_status ft_store_at_graft_point_prepare(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct ft_descent *d,
		struct cds_ft_inode_flag *graft_payload,
		unsigned long graft_external_count,
		struct ft_glue *glue,
		const struct ft_held_set *outer,
		struct ft_graft_store_state *st)
{
	struct ft_lock_ctx gctx;

	memset(st, 0, sizeof(*st));
	st->glue = glue;
	/*
	 * The recompactions below lock {p, its parent, its grandparent}.  @d
	 * is their anchor source.  @glue->txn is the registry naming what this
	 * op already holds.
	 *
	 * @outer is the REST of what the op holds.  An EARLIER step of the
	 * caller may have taken marks that reach no registry: the rekey
	 * fold's ft_rekey_cow_stop set.  Such a caller chains its frame here.
	 * The reason is that under a coarse spacing those marks and these
	 * recompactions collapse onto one word.  @outer is NULL for a caller
	 * with nothing outstanding.
	 */
	ft_lock_ctx_init(&gctx, d, glue->txn, glue->op);
	gctx.held.outer = outer;
	/*
	 * The glue's own acquires (its publish parent, its split CN) fire from
	 * commit helpers that never see @d.  So hand the glue the anchor
	 * source here, the one place holding both.
	 */
	glue->lock_d = d;

	/*
	 * Skip-mode chain-compress invariant: under SPECULATIVE-mode tries,
	 * non-root 1-child internals without external_nodes must be a
	 * compressed.  graft_payload may be the source trie's old root (a
	 * 1-child internal is permitted at root, forbidden at the non-root
	 * position we are placing it in).  Canonicalize per branch -- deferred
	 * past the POPULATED_ERROR check so that on failure the caller can
	 * still reach the original payload for rollback.
	 */
	if (d->depth == key_len) {
		struct cds_ft_metadata *pmeta;
		struct cds_ft_inode_flag *dest;
		struct cds_ft_inode_flag *slot_value;
		int ret;

		if (d->nf)
			return CDS_FT_STATUS_POPULATED_ERROR;

		graft_payload = ft_compress_single_child_if_needed(ft,
			graft_payload, glue);
		if (graft_payload == (struct cds_ft_inode_flag *) (long) -ENOMEM)
			return CDS_FT_STATUS_MEMORY_ERROR;

		pmeta = cds_ft_item_to_metadata(ft_node_ptr(d->pnf));

		/*
		 * R8 ordering (reserved-byte model).  The slot store and its
		 * possible recompact are the LAST fallible steps, so they run
		 * FIRST.  key[key_len-1]'s slot is occupied with a reserved
		 * bit-set+NULL byte that reads as not-present.  An ENOMEM here
		 * leaves both tries untouched (no live edge flipped).  The
		 * failure-free wiring runs behind the still-empty slot in
		 * commit.
		 *
		 * The slot edge (NULL -> slot_value) is recorded against the
		 * FINAL slot in commit, once the recompact-relocated address is
		 * known.  So one txn commit flips the slot store atomically
		 * with the back-pointers and the ordered-list run-splice.  This
		 * is the appear-side cross-view fix.  Nothing is stored in the
		 * slot during the build.
		 */
		slot_value = graft_payload;
		if (ft_node_compressed(graft_payload))
			slot_value = ft_publish_compressed(ft,
				ft_compressed_node_ptr(graft_payload),
				graft_payload);
		dest = d->pnf;
		/*
		 * @glue->txn is passed as the retire txn.  A reserve that
		 * RECOMPACTS the live dst attach node has two things to wire:
		 * its reparent sweep and the external-head back-channel.  It
		 * must record them INTO the glue commit, as co-committed
		 * (parent, offset) pairs.  It must not apply them as immediate
		 * stores onto the unpublished copy.  Such stores were
		 * reader-visible for the whole build-to-commit window, via
		 * up-walks from the still-reachable children.
		 *
		 * Every caller of the build creates @glue->txn first:
		 * cds_ft_graft, cds_ft_merge_at and the rekey.
		 */
		ret = ft_node_set_nth_rec(ft, &dest, key[key_len - 1], NULL,
			&st->old_recompacted_node, pmeta, d->depth - 1, false,
			&st->reserve_rec, glue->txn,
			/* §11: recompact {p} under its LIVE reanchored parent
			 * (d->ppnf, d->pnfp), not under {p}'s stale
			 * back-pointer.  For the SKIP_X dual one level up, also
			 * pass the LIVE great-grandparent (d->pppnf) and the
			 * coherent slot of {p}'s parent in it (d->ppnfp). */
			&(const struct ft_parent_hint){
				.parent = d->ppnf, .slot = d->pnfp,
				.gp = d->pppnf, .gp_slot = d->ppnfp,
				/*
				 * An ordinary hint deliberately skips the
				 * C.parent == P read-set guard.  The MW
				 * cross-trie graft's parent identity is the
				 * CALLER's.  It must not be validated against
				 * C's lazily-updated back-pointer.
				 *
				 * But the SAME hint serves the FOLD's NOSPLIT
				 * dst-parent recompaction.  Its republish
				 * PARKS SW into @parent's slot.  An SW park
				 * into a slot whose ownership rests on a shape
				 * argument is exactly what the guard exists to
				 * stop.  So ask for the guard when this is the
				 * fold (record_only).  This is byte-neutral for
				 * the MW graft.
				 */
				.parent_guard = glue->record_only },
			&gctx,
			/*
			 * BULK reserve: never in place.  Nothing here holds
			 * @dest before the reserve writes it.  And a same-trie
			 * move's reader-coherence witness needs the dst attach
			 * parent to RELOCATE.
			 */
			false, &st->count_deferred);
		/*
		 * -EAGAIN is a TRANSIENT peer conflict, not an allocation
		 * failure: the reserve found its byte filled under it.  Report
		 * it as BUSY.  A retry-driving caller then need not read "out
		 * of memory" to mean "re-descend".  Both callers already retry
		 * every non-OK status but POPULATED: cds_ft_merge_at at
		 * retry_merge, cds_ft_graft at retry_attach.  So this is a
		 * truthfulness fix, not a control-flow change.
		 */
		if (ret)
			return ret == -EAGAIN ?
				CDS_FT_STATUS_BUSY_ERROR :
				CDS_FT_STATUS_MEMORY_ERROR;

		st->attached = graft_payload;
		st->attached_depth = (unsigned int) key_len;
		st->dest = dest;
		st->publish_pmeta = pmeta;
		st->pnfp = d->pnfp;
		st->slot_value = slot_value;
		st->slot_byte = key[key_len - 1];
	} else {
		unsigned int i = d->depth;
		struct cds_ft_inode_flag *branch;
		struct cds_ft_node *displaced = NULL;

		if (d->nf && ft_node_external(d->nf))
			displaced = (struct cds_ft_node *) ft_node_ptr(d->nf);

		graft_payload = ft_compress_single_child_if_needed(ft,
			graft_payload, glue);
		if (graft_payload == (struct cds_ft_inode_flag *) (long) -ENOMEM)
			return CDS_FT_STATUS_MEMORY_ERROR;

		branch = ft_build_branch(ft, key, i, key_len, graft_payload,
				graft_external_count, displaced != NULL, glue);
		if (!branch)
			return CDS_FT_STATUS_MEMORY_ERROR;

		if (displaced) {
			struct cds_ft_metadata *bm =
				ft_flag_to_metadata(ft, branch);
			/*
			 * Phase 1 (build-invisible): wire branch's own
			 * back-pointer into d->pnf and the cluster-internal
			 * external_nodes pointer.  The back-channel publish
			 * (displaced->prev = branch) and the deferred live
			 * flips are applied in commit, fresh-before-live.
			 */
			ft_set_parent(ft, branch, d->pnf, d->nfp);
			ft_metadata_set_external_nodes(branch, bm, displaced);
			ft_nr_keys_store(ft, bm, ft_nr_keys_get(bm) + 1,
				CMM_RELAXED);

			st->displaced_shape = true;
			st->attached = branch;
			st->attached_depth = d->depth;
			st->displaced = displaced;
			st->pnf = d->pnf;
			st->nfp = d->nfp;
			st->tp_key = key;
			st->tp_i = i;
		} else {
			struct cds_ft_inode_flag *dest = d->pnf;
			struct cds_ft_metadata *pmeta;
			int ret;

			pmeta = cds_ft_item_to_metadata(ft_node_ptr(d->pnf));

			/*
			 * Same R8 + fresh-before-live discipline as the
			 * d->depth == key_len arm (reserved-byte model).
			 * Reserve key[i-1]'s slot (bit-set+NULL, reads
			 * not-present), so that the fallible slot store runs
			 * FIRST.  The slot edge (NULL -> branch) is recorded
			 * against the final slot.  The wiring completes
			 * invisibly in commit.
			 */
			/* Same retire-txn routing as the depth == key_len arm. */
			ret = ft_node_set_nth_rec(ft, &dest, key[i - 1], NULL,
				&st->old_recompacted_node, pmeta,
				d->depth - 1, false, &st->reserve_rec,
				glue->txn,
				/* §11: recompact {p} under its LIVE reanchored
				 * parent (d->ppnf, d->pnfp), not under the
				 * stale back-pointer.  For the SKIP_X dual one
				 * level up, also pass the LIVE
				 * great-grandparent (d->pppnf) and the coherent
				 * slot of {p}'s parent in it (d->ppnfp). */
				&(const struct ft_parent_hint){
					.parent = d->ppnf,
					.slot = d->pnfp,
					.gp = d->pppnf,
					.gp_slot = d->ppnfp,
					/* Fold republish parks SW into
					 * @parent's slot: guard the identity.
					 * See the depth == key_len arm. */
					.parent_guard = glue->record_only },
				&gctx,
				/* Bulk reserve: see the depth == key_len arm. */
				false, &st->count_deferred);
			/* Transient peer conflict, not OOM: see the
			 * depth == key_len arm above. */
			if (ret)
				return ret == -EAGAIN ?
					CDS_FT_STATUS_BUSY_ERROR :
					CDS_FT_STATUS_MEMORY_ERROR;

			st->attached = branch;
			st->attached_depth = d->depth;
			st->dest = dest;
			st->publish_pmeta = pmeta;
			st->pnfp = d->pnfp;
			st->slot_value = branch;
			st->slot_byte = key[i - 1];
		}
	}
	return CDS_FT_STATUS_OK;
}

static
enum urcu_txn_status ft_store_at_graft_point_commit(struct cds_ft *ft,
		struct cds_ft_inode_flag **attached_nf,
		unsigned int *attached_depth,
		struct ft_graft_run *run,
		struct ft_graft_store_state *st,
		long count_delta)
{
	enum urcu_txn_status cst;

	if (st->displaced_shape) {
		/*
		 * The displaced external is LIVE.  The dst leaf stays reachable
		 * through the still-old slot until the forward publish replaces
		 * it.  So its re-parent onto the fresh @branch is a
		 * reader-observable pointer.  That re-parent is its
		 * back-channel: displaced->prev / cell->parent = branch.
		 *
		 * Record it as a dst_origin edge.  ft_glue_txn_commit then
		 * flips it atomically with the forward publish and the
		 * run-splice cell edges.  It is not a fresh-before-live store
		 * ahead of them.  (A flip proxy parked on an external's parent
		 * is resolved by the up-walk readers: ft_get_parent_rcu /
		 * ft_skip_to_compressed / ft_skip_reanchor.)
		 *
		 * The payload's hidden back-pointers are wired immediately by
		 * ft_glue_apply_deferred inside the commit.
		 */
		ft_glue_defer_edge_origin(ft, st->glue,
			(struct cds_ft_inode_flag *) st->displaced,
			st->attached, NULL, /*dst_origin=*/ true);
		st->glue->publish_parent = st->pnf;
		st->glue->publish_slot = st->nfp;
		st->glue->top = st->attached;
		/*
		 * ☠ ANNOUNCE THE PENDING PUBLISH.  This arm sets the three glue
		 * fields directly instead of going through ft_glue_set_publish.
		 * But ft_glue_set_publish is the ONLY thing that arms
		 * @pending_pub_slot.  Without this announce, these all stayed
		 * INERT for this arm:
		 * - the copy loop's publish fold;
		 * - the chain-compress boundary and deep folds;
		 * - the commit-edges skip.
		 *
		 * A recompaction of @st->pnf later in the same commit then
		 * built its fresh copy from COMMITTED words, and this publish
		 * was lost.  MEASURED: 12 of 3000 shapes LOST KEYS that way, 7
		 * through the copy loop and 5 through the chain-compress
		 * collapse.
		 *
		 * The announce is done by hand rather than by calling
		 * ft_glue_set_publish.  That function also defers a back edge
		 * for @top, and this arm wires its own (the displaced leaf,
		 * just above).
		 */
		if (st->glue->txn) {
			st->glue->txn->pending_pub_slot = st->nfp;
			st->glue->txn->pending_pub_val = st->attached;
			st->glue->txn->pending_pub_plain = NULL;
		}
		/*
		 * Order-statistics fold (BULK).  The fresh @branch absorbs the
		 * displaced external.  It raises @st->pnf's subtree by
		 * +count_delta.  @publish_parent == st->pnf, so the glue commit
		 * records that +count_delta walk into the same flip.
		 *
		 * ☠ EXCEPT UNDER @record_only, WHERE THIS COMMIT RUNS TOO EARLY.
		 * This arm is the one NOSPLIT shape that calls ft_glue_txn_commit
		 * itself.  It does so at the rekey driver's step 3a, BEFORE the
		 * src detach at step 3.  That detach can RECOMPACT any ancestor
		 * on the walk, the ROOT included.  A recompaction relocates the
		 * node.
		 * ft_flip_txn_record_count_parent follows the survivor of a
		 * relocation the descriptor ALREADY carries.  At step 3a the
		 * descriptor carries none.  So the walk charges the copy this
		 * same commit retires, and the surviving copy is born from the
		 * uncharged word.
		 *
		 * MEASURED, rank stats on, both list modes.  The sequence was:
		 *
		 *   insert {"c","aba"}
		 *   rekey_merge(dst "cbba", src "a")
		 *
		 * It returns OK with both keys at the right names.
		 * cds_ft_verify answers
		 * `depth 0: nr_keys mismatch: stored 1, computed 2`.  That is
		 * short by exactly the moved count.  Over 3000 generated
		 * shapes, 117 fail this way on the default build and 248 on
		 * nocompress.  Deferring takes both back to the rank lane's
		 * standing residue (6 / 106).  That residue is the count with
		 * this shape refused entirely.
		 *
		 * So hand the walk to the driver, which runs it at step 3a'
		 * after the detach.  The other NOSPLIT arm already uses the
		 * same deferral and the same field (@deferred_count_base).  The
		 * base is the publish parent, which is what @g->count_delta
		 * would have walked from.  A non-fold caller (the cross-trie
		 * graft) has no detach to order against.  It keeps walking
		 * inline, byte-identically.
		 */
		if (st->glue->record_only && count_delta) {
			st->glue->deferred_count_base = st->pnf;
			st->glue->deferred_count_delta = count_delta;
			st->glue->count_delta = 0;
		} else {
			st->glue->count_delta = count_delta;
		}
		cst = ft_glue_txn_commit(ft, st->glue, run);
		if (st->tp_i >= 1)
			FT_TP(tree_edge_set, (const void *) ft,
				(const void *) st->pnf,
				(unsigned int) (st->tp_i - 1),
				(uint8_t) st->tp_key[st->tp_i - 1],
				(const void *) st->attached);
	} else {
		struct cds_ft_inode_flag **slot = NULL;
		/*
		 * ☠ ZERO-INITIALISED, like every other edge array in the tree.
		 * The run producers fill these FIELD BY FIELD.  Any field a
		 * producer does not set reads STACK GARBAGE.  @owner_cell is
		 * exactly such a field on the paths that build a non-cell edge.
		 * This was the ONE declaration missing `= { 0 }`.  It SEGV'd
		 * deterministically at ft_inv test 46, the moment a consumer
		 * started reading that field.
		 */
		struct ft_ord_cell_edge redges[FT_ORD_CELL_RUN_SPLICE_MAX_EDGES] =
			{ 0 };
		struct cds_ft_inode_flag *count_base = NULL;
		unsigned int rn = 0;

		/*
		 * THE RE-PARENT ACQUIRE.  ft_glue_txn_commit runs this for the
		 * displaced arm above.  THIS arm commits by calling
		 * ft_glue_apply_deferred directly.  So without the call below
		 * the acquire never runs at all.  Every deferred entry then
		 * reaches ft_reparent_record_meta with @child_marked false and
		 * @hold_ctx NULL.  (This acquire is what sets @held_lock.)
		 * Those two are the whole SW/MW dispatch.
		 *
		 * The child's parent word is then recorded MW even when this op
		 * HOLDS it.  An SW park by the SAME txn then collides on kind.
		 * ft_rekey_cow_stop makes such a park, on a cut source.  On a
		 * debug build the engine asserts (rcu-txn-mcas.h,
		 * `r->kind == kind`).  On a release build it resolves the
		 * collision fail-safe by promoting to MW.  The op then
		 * re-attempts forever.
		 *
		 * The acquire is placed BEFORE the ft_set_parent below so that
		 * the bail is clean.  It is gated exactly as ft_glue_txn_commit
		 * gates it.  @record_only is what makes returning ABORT from
		 * here legitimate: the txn is the CALLER's, and the caller's
		 * single commit is still ahead.  So @record_only is TESTED
		 * rather than asserted.  A caller reaching this arm without it
		 * has no clean bail, and keeps its present behaviour untouched.
		 */
		if (st->glue->txn && st->glue->txn->structural_sw) {
			/*
			 * DETECT on every mode, TAKE only under FINE
			 * non-exclusive.  ft_glue_acquire_reparent_marks
			 * decides which, and its detect-only pass cannot
			 * refuse.  @record_only still gates the ARMED call
			 * alone.  It is what makes returning ABORT from here
			 * legitimate.  A caller reaching this arm without it
			 * has no clean bail.
			 */
			bool armed = ft->lock_fine && !ft->exclusive;

			if ((!armed || st->glue->record_only) &&
					ft_glue_acquire_reparent_marks(ft,
						st->glue) && armed) {
				return URCU_TXN_STATUS_ABORT;
			}
		}
		ft_node_get_nth_skip(st->dest, &slot, st->slot_byte, FT_PF_NONE);
		assert(slot);
		/*
		 * THE PAYLOAD'S BACK EDGE.  It is a plain store, because the
		 * payload is invisible until the forward publish below.  The
		 * cross-trie graft drained its source.  The rekey fold hands
		 * over a fresh COW copy.
		 *
		 * @payload_live says the payload is NOT invisible.  A BARE
		 * EXTERNAL HEAD is app-owned and cannot be copied.  It stays
		 * reachable at the SOURCE for the whole build window.  So the
		 * store would be reader-visible ahead of the publish, and would
		 * survive an abort.
		 *
		 * Defer the edge instead.  ft_glue_apply_deferred one line down
		 * then RECORDS it through ft_reparent_record's external arm
		 * (live = dst_origin || record_only, and the fold sets
		 * record_only).  So the head's parent flips with the slot that
		 * adopts it.  ☞ @glue->payload_live.
		 */
		if (st->glue->payload_live)
			ft_glue_defer_edge_origin(ft, st->glue, st->attached,
				st->dest, slot, /*dst_origin=*/ false);
		else
			ft_set_parent(ft, st->attached, st->dest, slot);
		ft_glue_apply_deferred(ft, st->glue);
		if (st->old_recompacted_node) {
			/*
			 * MW LOCK_FINE drop (§11, cross-trie).  The publish
			 * slot must come from st->dest's OWN (parent,
			 * offset), NOT from the descent-time st->pnfp.  That
			 * pair is the COHERENT one: the recompact locked it
			 * and stored it into st->dest's meta
			 * (ft_node_recompact inh_parent / inh_slot).
			 *
			 * Under the FT-wide-lock drop a peer can relocate
			 * the graft-point grandparent.  It can do so after
			 * this graft's descent captured st->pnfp, and before
			 * the recompact re-anchored st->dest onto the NEW
			 * grandparent.  Then st->pnfp names the OLD
			 * grandparent's slot, while st->dest->parent names
			 * the new one.  ft_set_parent_slot ->
			 * ft_slot_to_byte would then index the wrong body
			 * out of bounds.  This is the SAME coherence rule as
			 * the graft_swap merged publish and the remove-path
			 * PSO fix.
			 *
			 * The grandparent is release-locked by the
			 * recompact, so this resolve is stable through the
			 * commit.
			 */
			struct cds_ft_metadata *dest_meta =
				cds_ft_item_to_metadata(ft_node_ptr(st->dest));
			struct cds_ft_inode_flag *pub_parent;
			struct cds_ft_inode_flag **pub_slot =
				ft_resolve_parent_slot(dest_meta, ft, &pub_parent);

			/*
			 * The reserve recompacted (relocated) the dst attach
			 * node.  The reserve ft_node_set_nth_rec RECORDED a
			 * compressed grandparent's SKIP_X dual into
			 * st->reserve_rec.  The dual was deferred, not
			 * bare-stored prematurely ahead of the forward.
			 *
			 * Add the forward grandparent slot edge to that same
			 * rec.  @parent_nf is st->dest, the relocated node.
			 * It is a plain internal, so _ft_publish_to_parent
			 * does NOT re-emit the SKIP_X dual (no duplicate
			 * edge).
			 *
			 * Then record both into glue->txn.  The txn was
			 * pre-reserved before the build, so nothing is
			 * allocated here.  The relocation then flips
			 * ATOMICALLY with the grafted slot edge and the
			 * run-splice, in the single commit below.
			 *
			 * The src drain does not cover dst.  The old dst
			 * node stays resolved-to via the parked grandparent
			 * proxy until the commit.  It is freed below, after
			 * the commit.
			 */
			/*
			 * VALIDATE (§4.B): guard the LIVE grandparent this
			 * relocation republishes into.  Only a coarse trie
			 * calls it.  It records nothing: the FT-wide lock
			 * excludes every other writer.  It calls abort() (FT
			 * GUARD UNHELD) if this txn itself retired that
			 * grandparent.
			 *
			 * LOCK_FINE (§9.3, step 6): the recompact acquired
			 * this SAME word (its P) earlier in this same
			 * glue->txn.  That recompact was driven by
			 * ft_store_at_graft_point_prepare's
			 * ft_node_set_nth_rec, and it recorded a release on
			 * the word.  The release SUPERSEDES this guard: it
			 * makes peers abort up front, which is strictly
			 * stronger.  So under lock_fine we DROP the guard,
			 * exactly as insert (ft_insert_publish_or_park) and
			 * remove (ft_detach_node's recompact arm) do.
			 *
			 * The ordering is safe by construction.  The release
			 * was recorded first.  If the guard were kept, it
			 * would chain as a read-your-writes no-op.  It would
			 * never be the forbidden guard-then-release order
			 * (rule at ft_flip_txn_record_release_lock).
			 * Non-lock_fine keeps the guard.
			 */
			if (!ft->lock_fine)
				ft_flip_txn_guard_parent(ft, st->glue->txn,
					pub_parent);
			/*
			 * SETTLED, not raw, for the expected-old.  This is
			 * the same rule as the graft_swap merged publish and
			 * the five metadata reads fixed with it.  @pub_slot
			 * is a TRANSACTED word.  When the republish
			 * grandparent is the trie ROOT, @pub_slot is
			 * &ft->root.  A peer's root-level graft parks a
			 * proxy in that word.
			 *
			 * Under the MCAS engine, a raw load then recorded
			 * that descriptor POINTER as the expected-old.
			 * urcu_txn_settle stored it back blind on a matching
			 * commit.  Nothing ever cleared it there, because its
			 * owning transaction decided and settled long before.
			 * So every later acquire of that word bailed forever.
			 * The txn load also returns this txn's own pending
			 * value, which a raw load would miss.
			 *
			 * This was reached by a KEYED graft racing a ROOT
			 * graft into one destination:
			 * inv_empty_dst_root_graft_peer, ~1 run in 40 under
			 * -DDEBUG_RCU.
			 */
			/*
			 * ☠ @parent_nf IS st->dest FOR THE DUAL, @slot_owner_nf IS
			 * @pub_parent FOR THE OWNER, and they are different nodes.
			 * st->dest is passed as @parent_nf only so that a
			 * compressed grandparent's SKIP_X dual is not
			 * re-emitted.  The reserve already recorded it.  The
			 * slot being published lives in @pub_parent.  That
			 * is the word the recompact acquired and this txn
			 * holds.  Naming st->dest as the owner reports an
			 * exclusion gap that is not real.
			 */
			_ft_publish_to_parent_meta(ft, st->dest,
				pub_slot, st->dest,
				ft_resolve_flip_proxy(*pub_slot),
				NULL, NULL, &st->reserve_rec,
				/*slot_owner_nf=*/ pub_parent, false);
			ft_flip_txn_record_pub_rec(st->glue->txn,
				&st->reserve_rec);
			/*
			 * ANNOUNCE THE PENDING PUBLISH, exactly as the GLUE split
			 * builder and the merge do through ft_glue_set_publish
			 * (ft-mutation-helpers.h).  Until this existed, the
			 * NOSPLIT forward publish was the ONLY one that went
			 * unannounced.  The shape-gate comment claimed such
			 * a publish "rides the recompaction instead
			 * (@pending_pub_slot)".  That was true for GLUE and
			 * MERGE and FALSE here.
			 *
			 * ☠ WHAT IT FIXES.  A same-trie rekey's src detach
			 * recompacts THIS publish parent before the caller's
			 * single commit.  @pending_pub_slot is not
			 * predicate-armed against any node: the recompaction
			 * copy loop consumes it BY SLOT ADDRESS
			 * (ft-mutation-node.h).  Without the announcement
			 * the loop resolves this slot through
			 * ft_flip_txn_resolve_prio, which deliberately does
			 * NOT read the txn's own writes.  The loop reads the
			 * COMMITTED old child, and the fresh parent is born
			 * holding it.  The moved subtree is then reachable
			 * only from a body nothing descends into.  The op
			 * reports OK and the keys are gone.  Measured 4 -> 3
			 * and 5 -> 3 keys before this line.
			 *
			 * The edge recorded just above still commits into
			 * that superseded body once the fold takes the value
			 * across.  That is harmless, and it is NOT dead-node
			 * resurrection:
			 * - the copy loop only reads the old body;
			 * - the retire lands on the state word, never on
			 *   this slot word.
			 *
			 * So the edge's expected-old still holds at commit,
			 * and the write lands in a node already retired by
			 * the same flip.  Both children agree on the value.
			 * (The GLUE lane instead defers its publish record
			 * to commit-edges time and skips it via
			 * @pending_pub_folded.  The NOSPLIT record is
			 * already in the txn by then, so it cannot take that
			 * route.)
			 *
			 * ☞ NO COUNT RE-BASE HERE, deliberately.  The
			 * order-statistics walk below is the only consumer
			 * that would need one.
			 * ft_flip_txn_record_count_parent returns
			 * immediately unless ft->rank_stats
			 * (ft-mutation-helpers.h).  And every rank-stats
			 * trie reaching this fold LIVELOCKS today, for an
			 * unrelated pre-existing reason (rank stats coerce
			 * COARSE).  A re-base here would therefore be
			 * untestable code in the one lane whose bugs keep
			 * shipping.  So it waits for that livelock.
			 */
			if (st->glue->record_only && st->glue->txn) {
				unsigned int ri;

				st->glue->txn->pending_pub_slot = pub_slot;
				st->glue->txn->pending_pub_val = st->dest;
				st->glue->txn->pending_pub_plain = NULL;
				/*
				 * AND ANNOUNCE THE SKIP_X DUAL, when the reserve recorded
				 * one.  @reserve_rec holds the forward edge just added
				 * above.  For a COMPRESSED publish parent it
				 * also holds the re-encoded skip word in the
				 * GRANDPARENT's slot.  That word was recorded
				 * against the grandparent as it stood.
				 *
				 * The grandparent may be recompacted later in
				 * the caller's commit.  The rekey's src detach
				 * does it when BP IS that grandparent.  An
				 * elevating detach landing on it does it too.
				 * The recompaction builds its copy from
				 * COMMITTED words, and would carry the OLD
				 * skip word over.  Naming the slot lets the
				 * copy loop fold the refreshed value BY
				 * IDENTITY, exactly as
				 * ft_glue_txn_commit_edges does for the GLUE
				 * and MERGE publishes.
				 * ☞ ft_flip_txn @pending_dual_slot.
				 *
				 * MEASURED before this line, on the 3000-shape
				 * rekey corpus with the
				 * compressed-publish-parent refusal lifted.
				 * 17 shapes returned OK with cds_ft_verify
				 * reporting "stale SKIP_X dual".  In every one
				 * of them the detach had DEL-recompacted the
				 * grandparent as a plain copy.  In 24 other
				 * shapes the detach FUSED the grandparent into
				 * a run instead.  Those were already right,
				 * because ft_chain_compress_fused builds
				 * around the pending forward value.
				 */
				for (ri = 0; ri < st->reserve_rec.n; ri++) {
					if (st->reserve_rec.slot[ri] == pub_slot)
						continue;
					st->glue->txn->pending_dual_slot =
						st->reserve_rec.slot[ri];
					st->glue->txn->pending_dual_val =
						st->reserve_rec.new_val[ri];
					break;
				}
			}
			/*
			 * Order-statistics fold (BULK).  The reserve relocated
			 * the attach parent to the fresh @st->dest.  That copy
			 * was recompacted with only the OLD subtree count,
			 * because the payload slot read not-present during
			 * the reserve.
			 *
			 * Bake +count_delta into that fresh copy.  This is a
			 * build-invisible plain store, off the walk.  It rests
			 * on the insert-I7 reader-safety invariant.  A node's
			 * own nr_keys is only read root-descended.  Such a
			 * read lands on the old node via the parked
			 * grandparent proxy.
			 *
			 * Then walk the +count_delta up from the STABLE
			 * grandparent this relocation republishes into.
			 */
			if (count_delta) {
				struct cds_ft_metadata *dm =
					cds_ft_item_to_metadata(ft_node_ptr(st->dest));
				ft_nr_keys_store(ft, dm,
					ft_nr_keys_get(dm) + count_delta, CMM_RELAXED);
				count_base = pub_parent;
			}
		} else {
			/*
			 * In-place reserve: a redundant same-value republish.
			 * Resolve the grandparent slot COHERENTLY from
			 * st->dest's own (parent, offset), not from the stale
			 * descent st->pnfp.  This is the same coherence rule
			 * as the relocation arm above: a peer may have
			 * relocated the grandparent since this graft's
			 * descent.
			 */
			struct cds_ft_metadata *dest_meta =
				cds_ft_item_to_metadata(ft_node_ptr(st->dest));
			struct cds_ft_inode_flag *pub_parent;
			struct cds_ft_inode_flag **pub_slot =
				ft_resolve_parent_slot(dest_meta, ft, &pub_parent);

			ft_publish_to_parent(ft, pub_parent, pub_slot, st->dest);
			/*
			 * The in-place reserve owes @st->dest its nr_child++.
			 * Record it as an edge in glue->txn.  The count then
			 * goes live with the slot edge below, and is discarded
			 * with an aborted attempt.  Reserve-then-ABORT must be
			 * idempotent.  Otherwise a re-descend reserves the byte
			 * a second time and double-counts it (see
			 * ft_flip_txn_record_nr_child_inc).
			 *
			 * Reservation is net-zero.  This arm records no §4.B
			 * parent guard; only the relocation arm above does.  So
			 * it consumes that arm's already-reserved guard record.
			 */
			if (st->count_deferred)
				ft_flip_txn_record_nr_child_inc(st->glue->txn,
					dest_meta);
			/*
			 * Order-statistics fold (BULK): @st->dest (== d->pnf, a
			 * stable existing node) gains +count_delta; walk from it up.
			 */
			if (count_delta)
				count_base = st->dest;
		}
		/*
		 * Record the slot edge against the FINAL slot, now that the
		 * recompact-relocated address is known.  The edge is
		 * NULL -> slot_value, so the reserved bit-set+NULL byte settles
		 * to the grafted child at commit.
		 *
		 * Then fuse the ordered-list run-splice into the SAME
		 * glue->txn: record the run's <=4 boundary edges.  One commit
		 * then makes the grafted key appear in the structure and the
		 * ordered list atomically.
		 *
		 * The reserve set_nth was the last fallible step in prepare.
		 * The slot + cell edges have none.  They all draw from the
		 * reserved txn (no allocation here).
		 */
		/*
		 * ☠ WHETHER THIS SLOT IS RECORDED AT ALL IS AN ARM QUESTION,
		 * because the two arms write two different KINDS of word.
		 *
		 * RELOCATION: @st->dest is the fresh copy the reserve's recompact
		 * produced.  It is build-invisible: nothing reaches it until
		 * the grandparent forward edge above publishes it.  A slot in
		 * its INTERIOR is not a reachability edge.  So it takes a PLAIN
		 * STORE, like every other write into that private body:
		 * - the copy loops;
		 * - ft_set_parent at the resolve above;
		 * - the nr_keys bake above.
		 *
		 * Recording it buys no atomicity.  The engine flips a SET of
		 * slots.  The set that matters is the grandparent edge plus the
		 * cell run-splice, and that set is already this one commit.  So
		 * either way a reader sees nothing, and then a fully wired
		 * @st->dest.  This is ft_glue_apply_deferred's per-edge rule
		 * ("unreachable until the forward flip is what licenses the
		 * plain store").  This arm was the one outlier from it.
		 *
		 * IN-PLACE: @st->dest is LIVE, so its slot IS reader-visible
		 * now.  The write must ride the flip.  That is what makes the
		 * grafted key appear atomically with the ordered-list splice.
		 * @slot was resolved out of @st->dest itself.  So the node that
		 * owns it is @st->dest, the same node whose nr_child this arm
		 * records.
		 */
		if (st->old_recompacted_node)
			*slot = st->slot_value;
		else
			ft_flip_txn_record_reserved(st->glue->txn,
				/*owner=*/ ft_flag_to_metadata(ft, st->dest),
				(void **) slot, NULL, (void *) st->slot_value);
		if (run) {
			/*
			 * ☞ THE RUN SPLICE'S FOUR CELL LOCKS.
			 * ft_ord_cell_run_splice_edges writes exactly four
			 * words: &run_first->lnode.prev, &run_last->lnode.next,
			 * and the destination pair's &pred->lnode.next and
			 * &succ->lnode.prev.  @splice selects the DESTINATION
			 * validation (pred->next == succ), because the run's
			 * current links are the ones this op replaces.
			 */
			struct ft_cell_plan plan = {
				.cell = run->run_first,
				.last = run->run_last,
				.splice = true,
				.pred = ft_ord_or_sentinel(ft, run->pred),
				.succ = ft_ord_or_sentinel(ft, run->succ),
			};

			if (ft_cell_lockset_take(ft, NULL, st->glue->txn, &plan)) {
				ft_glue_take_refused(st->glue);	/* see its header */
				return URCU_TXN_STATUS_ABORT;
			}
			rn = ft_ord_cell_run_splice_edges(ft, run->run_first,
				run->run_last, run->pred, run->succ, redges, 0);
			/*
			 * Record through the tag-dispatching recorder, NOT
			 * through a raw ft_flip_txn_record_tag loop.  A CELL
			 * edge must be MW whatever this txn's structural_sw
			 * mode is.  The ordered list is lock-free: no cell
			 * carries a node lock to park an SW store under.  A
			 * raw record_tag keys off structural_sw alone.  Under
			 * a caller that opted in, it would silently demote
			 * these edges to an unvalidated SW park.
			 */
			ft_ord_cell_record_into_ft(ft, st->glue->txn, redges, rn);
		}
		/*
		 * Freeze-on-free (doc §4.B).  A reserve recompaction relocated
		 * the dst attach node.  Its old copy @old_recompacted_node
		 * stays resolved-to via the parked grandparent proxy until this
		 * commit.  The old copy's tombstone is recorded INTO glue->txn
		 * (reserved +1 above).  So the mark and the commit that unlinks
		 * it flip atomically (atomic detach).  (Not surfaced by
		 * ft_unit; the recompact-relocation graft shape is exercised by
		 * ft_inv.)
		 *
		 * LOCK_FINE (§9.3): under the FT-wide-lock drop the reserve's
		 * ft_node_recompact ALREADY records @old_recompacted_node's
		 * retire.  It records it as its C-half {LOCK|s -> TOMBSTONE|s}
		 * terminal, registered on THIS glue->txn (ft-mutation-node.h).
		 * That IS the atomic-detach tombstone.  A second plain
		 * ft_flip_txn_record_tombstone here would DOUBLE-record the
		 * same word with a conflicting expected-old (plain
		 * current-state vs the mark snapshot).  That is the
		 * double-tombstone hazard: a lost key and a double free.
		 *
		 * Insert relies solely on the recompact's retire (ft-insert.h,
		 * free_old_node, no commit-time tombstone).  The graft matches
		 * it under lock_fine.  It keeps the standalone tombstone only
		 * for the non-fenced (non-lock_fine) recompact.
		 */
		if (st->old_recompacted_node && !ft->lock_fine)
			ft_flip_txn_record_tombstone(st->glue->txn,
				cds_ft_item_to_metadata(st->old_recompacted_node));
		if (count_delta) {
			/*
			 * ☠ THE FOLD MUST NOT WALK HERE.  This runs at the
			 * driver's step 3a.  The src detach is step 3, and it
			 * can RELOCATE THE ROOT.  Walking now resolves every
			 * ancestor against the pre-detach structure.  So the
			 * delta is charged into a root copy this same commit
			 * retires, and the live root never receives it.
			 *
			 * MEASURED, rank stats on.  The sequence was:
			 *
			 *   insert {"ab","bcab","b"}
			 *   rekey "bbaaca" <- "a"
			 *
			 * It returns OK with every key at the right name.
			 * cds_ft_count_keys answers 2 for 3 keys, with
			 * `ft_verify: depth 0: nr_keys mismatch: stored 2,
			 * computed 3`.  The base and ft->root were ONE
			 * address at this line, and a DIFFERENT one after the
			 * detach.
			 *
			 * ft_flip_txn_record_count_parent already follows the
			 * survivor of a relocation, but only one the descriptor
			 * ALREADY carries.  Its header names the ordering the
			 * one-decide drivers owe it.  So hand the walk to the
			 * driver, which runs it after the detach.  That is
			 * exactly where the GLUE and MERGE lanes' walk already
			 * runs (ft_glue_txn_commit_edges at step 3c).
			 *
			 * A non-fold caller has no detach to order against and keeps
			 * walking inline, byte-identically.
			 */
			if (st->glue->record_only) {
				st->glue->deferred_count_base = count_base;
				st->glue->deferred_count_delta = count_delta;
			} else {
				ft_flip_txn_record_count_parent(ft, st->glue->txn,
					count_base, count_delta);
			}
		}
		if (st->glue->record_only) {
			/*
			 * FOLD (coherent rekey one-decide writer).  The whole
			 * dst-attach is now recorded into the caller's SHARED
			 * txn:
			 * - the slot NULL -> S_top';
			 * - the reserve recompaction's re-parents + fenced
			 *   retire;
			 * - the count.
			 *
			 * The caller runs the ONE commit that also carries the
			 * src-unlink + S_top COW.
			 *
			 * A reserve recompaction relocated the dst attach
			 * node.  Its OLD copy stays LIVE until the CALLER's
			 * commit, resolved through the parked grandparent
			 * proxy.  So its free is DEFERRED to the caller.  Hand
			 * it out via @st->old_recompacted_node, which the
			 * caller reads off its own @st.
			 *
			 * The caller owns the cells (run NULL here, list off).
			 * Leave st->glue->txn intact and report the recorded
			 * outputs.
			 */
			assert(!run);
			*attached_nf = st->attached;
			*attached_depth = st->attached_depth;
			return URCU_TXN_STATUS_OK;
		}
		cst = ft_flip_txn_commit(ft, st->glue->txn);
		st->glue->txn = NULL;
		/*
		 * ARM ONLY ON A COMMITTED FLIP.  The run-splice edges rode this
		 * txn, so an ABORT rolled them back with everything else: the run
		 * is NOT in @dst's ordered list.  @armed is what makes the caller
		 * skip its standalone ft_ord_cell_run_splice fallback.  Arming
		 * unconditionally would strand the run OUT of the list.  Its
		 * keys would be present in the structure but invisible to
		 * ordered iteration (and a cds_ft_verify ord-cell mismatch).
		 * The retire / glue_free_old reclaims below already use the
		 * same gate.
		 */
		if (run && cst == URCU_TXN_STATUS_OK)
			run->armed = true;
		/*
		 * Only the writer whose commit actually retired
		 * @old_recompacted_node ({LOCK|s -> TOMBSTONE|s}) may free
		 * it.  An aborted commit rolled the retire back: the registered
		 * fence was cleared, and the node stays LIVE.  Freeing it here
		 * would double-free the still-live node against the peer that
		 * legitimately retires it next.  The caller re-descends and
		 * re-commits on abort (ft_graft_keylen post-swap store retry).
		 */
		if (st->old_recompacted_node && cst == URCU_TXN_STATUS_OK)
			free_cds_ft_node(ft, st->old_recompacted_node);
	}
	/*
	 * @st->glue's OLD nodes are the pre-recompact copies gathered on its
	 * free list.  They are equally only reclaimable when the commit
	 * committed: an aborted commit left them LIVE.  Gate on @cst.
	 *
	 * Under record_only, the displaced branch above returns here without
	 * committing.  The OLD nodes then stay LIVE until the CALLER's
	 * commit.  So defer their free to the caller's post-commit finalize.
	 * Freeing here would reclaim a still-reachable node.
	 */
	if (!st->glue->record_only && cst == URCU_TXN_STATUS_OK)
		ft_glue_free_old(ft, st->glue);
	*attached_nf = st->attached;
	*attached_depth = st->attached_depth;
	return cst;
}

#ifdef FEATURE_FT_MERGE	/* only the merge calls it */
/*
 * Combined NOSPLIT store: run prepare, then commit, back to back.  The
 * callers run it after the source-root unlink and the drain.
 *
 * The slot proxy and the ordered-list run-splice both go through
 * @glue->txn.  The caller created or took that txn, and reserved it before
 * its last fallible step.  So this post-drain store has no fallible
 * allocation left:
 *  - node allocations draw from the reserve;
 *  - flip latches draw from the reserved txn.
 *
 * A non-NULL @run fuses an ordered-list run-splice into the structural
 * publish flip.  This holds for both store shapes: the in-place slot proxy
 * and the displaced-external forward publish.
 */
static
enum cds_ft_status ft_store_at_graft_point(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct ft_descent *d,
		struct cds_ft_inode_flag *graft_payload,
		unsigned long graft_external_count,
		struct cds_ft_inode_flag **attached_nf,
		unsigned int *attached_depth,
		struct ft_glue *glue,
			struct ft_graft_run *run,
		long count_delta)
{
	struct ft_graft_store_state st;
	enum cds_ft_status status;
	enum urcu_txn_status cst;

	status = ft_store_at_graft_point_prepare(ft, key, key_len, d,
			graft_payload, graft_external_count, glue,
			/*outer*/ NULL, &st);
	if (status != CDS_FT_STATUS_OK) {
		/*
		 * MW LOCK_FINE drop: prepare can FAIL here.  The recompact
		 * of the contended spine could not lock or reserve
		 * (-EAGAIN / MEMORY_ERROR).  Under the FT-wide lock it
		 * "cannot fail".
		 *
		 * Nothing is committed and @glue->txn is still alive.
		 * Free the invisible build and the txn.  The caller
		 * (cds_ft_merge_at's post-unlink retry) then only needs to
		 * re-descend.
		 *
		 * Only cds_ft_merge_at calls this combined wrapper.
		 * cds_ft_graft drives prepare and commit separately and
		 * does its own cleanup.
		 */
		ft_glue_abort(ft, glue);
		if (glue->txn)
			ft_flip_txn_destroy(glue->txn);
		return status;
	}
	cst = ft_store_at_graft_point_commit(ft, attached_nf, attached_depth,
			run, &st, count_delta);
	if (cst != URCU_TXN_STATUS_OK) {
		/*
		 * Drop: the store commit's MCAS footprint conflicted with
		 * a peer and ABORTED.  The commit consumed @glue->txn.
		 * The retire rolled back, so the OLD nodes stay LIVE.
		 * Whoever finally commits reclaims them.
		 *
		 * Free this attempt's UNPUBLISHED new nodes:
		 *  - the glue cluster (every fresh node ft_glue_track'd);
		 *  - @st.dest, when the reserve RELOCATED the attach node.
		 *    It is the unpublished relocated copy.
		 *    ft_node_recompact allocated it directly, so it is NOT
		 *    glue-tracked.
		 *
		 * An in-place recompact leaves @st.dest == the LIVE
		 * d->pnf: do NOT free it.  Signal the caller to re-descend
		 * and retry.
		 */
		ft_glue_abort(ft, glue);
		if (st.old_recompacted_node)
			free_cds_ft_node_unpublished(ft, ft_node_ptr(st.dest));
		/*
		 * Keep the engine's two outcome spaces apart.  Do not
		 * collapse both into "out of memory":
		 *  - URCU_TXN_STATUS_ABORT is contention.  It is "NOT an
		 *    error", as rcu-txn-status.h puts it.  Nothing was
		 *    published, so it is BUSY.
		 *  - Only a genuine URCU_TXN_STATUS_MEMORY_ERROR is an
		 *    allocation failure.
		 *
		 * Both are transient here, and both callers already retry
		 * on either.  So this changes only what the wrapper
		 * REPORTS.  But a conflict reported as ENOMEM sends a
		 * future reader after an allocator bug that never existed.
		 */
		return cst == URCU_TXN_STATUS_ABORT ?
			CDS_FT_STATUS_BUSY_ERROR :
			CDS_FT_STATUS_MEMORY_ERROR;
	}
	return CDS_FT_STATUS_OK;
}
#endif /* FEATURE_FT_MERGE */

/*
 * Outcome of ft_graft_build's build-invisible prep descent.
 */
enum ft_graft_prep {
	FT_GRAFT_PREP_GLUE,	/* diverge: full attach cluster built into @glue */
	FT_GRAFT_PREP_NOSPLIT,	/* graft point located in @d; legacy attach */
	FT_GRAFT_PREP_POPULATED,/* graft point occupied; tries pristine */
	FT_GRAFT_PREP_OOM,	/* allocation failed; caller runs glue_abort */
	FT_GRAFT_PREP_RETRY,	/* split-retire cn fence miss; re-descend (nothing
				 * built, cn NOT marked -- clean re-descend) */
};

/*
 * Build-invisible prep for cds_ft_graft.  Descend dst to the graft point
 * for @key.  The NOSPLIT and POPULATED outcomes are read-only on dst.
 *
 * Outcomes:
 *  - FT_GRAFT_PREP_GLUE: the key diverges inside a compressed node.  The
 *    COMPLETE attach cluster is built into @glue: the split rearrangement
 *    and the @payload subtrie.  Nothing is published or freed.  dst and
 *    the source stay pristine.
 *  - FT_GRAFT_PREP_OOM: that build ran out of memory.  The caller frees
 *    the glue, and there is nothing to roll back.
 *  - FT_GRAFT_PREP_NOSPLIT: the graft point is only located in @d.
 *    graft_keylen completes via the legacy post-sync
 *    ft_store_at_graft_point.
 *  - FT_GRAFT_PREP_POPULATED: the graft point is occupied.  The key ends
 *    inside an existing compressed path.
 */
static
enum ft_graft_prep ft_graft_build(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		struct cds_ft_inode_flag *payload, unsigned long src_count,
		struct ft_descent *d, struct ft_glue *glue,
		const struct ft_held_set *outer)
{
	const uint8_t *ik = key;

	ft_descent_init(d, ft);
	/*
	 * Give @glue its anchor source now, as the descent that supplies
	 * it is created.  The glue's own acquires fire from commit helpers
	 * that never see @d.  Its publish parent is the main one.  Under a
	 * coarse spacing, an acquire with no depth MISSES.  The miss aborts
	 * a commit that the unfailable arms cannot retry.  That is a
	 * livelock, not a failure.  ft_store_at_graft_point_prepare repeats
	 * this for the callers that reach it without building.
	 */
	glue->lock_d = d;
	for (; d->depth < key_len; ) {
		if (ft_node_external(d->nf))
			break;
		d->nf = ft_resolve_skip_compressed(ft, d->nf);
		if (ft_node_compressed(d->nf)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(d->nf);
			int remaining = (int) (key_len - d->depth);
			int cmp = cn->len < remaining ? cn->len : remaining;
			int j = ft_match_compressed_key(ik, cn, cmp);

			if (j == cmp && cn->len <= remaining) {
				ft_descent_traverse_compressed(ft, d, cn, &ik);
				continue;
			}
			if (j < cmp) {
				int bret = ft_split_compressed_graft_build(ft,
					d, key, key_len, j, payload, src_count,
					glue, outer);

				if (bret == -EAGAIN)
					return FT_GRAFT_PREP_RETRY;
				if (bret)
					return FT_GRAFT_PREP_OOM;
				return FT_GRAFT_PREP_GLUE;
			}
			/*
			 * Key shorter than the compressed path: the graft
			 * point lies inside an existing compressed (occupied).
			 */
			return FT_GRAFT_PREP_POPULATED;
		}
		ft_descent_step(ft, d, *(ik++));
	}
	return FT_GRAFT_PREP_NOSPLIT;
}

/* ft-lifecycle.h is included after this unit: one declaration, one TU. */
static enum cds_ft_status ft_recompute_max_used_key_len(struct cds_ft *ft);

/*
 * Compute the longest key a move of @src_key would carry.  Walk every key
 * of @src_ft that has @src_key as a prefix.  The walk is in key order and
 * starts at the first key at or after @src_key.
 *
 * The cost is O(moved keys): ask it only where the hint refuses.
 *
 * The caller must be in a read section.  The caller must also keep writers
 * out of the moved keys:
 *  - on a same-trie rekey, with the move gate and the FT-wide lock;
 *  - across tries, with an EXCLUSIVE source.
 *
 * Return 0, or -ENOMEM when no iterator can be had.
 */
static
int ft_moved_keys_max_len(struct cds_ft *src_ft, const uint8_t *src_key,
		size_t src_len, size_t *max_len)
{
	struct cds_ft_iter *iter;
	enum cds_ft_status s;
	uint8_t k[FT_MAX_KEY_LEN];
	size_t klen, max = 0;
	unsigned long nr_keys = 0;

	if (cds_ft_iter_create(src_ft, &iter) != CDS_FT_STATUS_OK)
		return -ENOMEM;
	cds_ft_iter_set_key(iter, src_key, src_len);
	for (s = cds_ft_lookup_ge(src_ft, iter); s == CDS_FT_STATUS_OK;
			s = cds_ft_next(src_ft, iter)) {
		if (cds_ft_iter_get_key(iter, k, sizeof(k), &klen) !=
				CDS_FT_STATUS_OK ||
		    klen < src_len || memcmp(k, src_key, src_len))
			break;		/* past the moved keys */
		if (klen > max)
			max = klen;
		if (!(++nr_keys % FT_WALK_KEYS_PER_SECTION))
			ft_walk_yield(iter);
	}
	cds_ft_iter_destroy(iter);
	*max_len = max;
	return 0;
}

/*
 * WOULD A MOVE PUSH ONE OF ITS KEYS PAST THE GROUP'S LIMIT?
 *
 * A moved key K becomes @dst_key || (K minus the @src_key prefix).  The
 * public contract of an OVERFLOW_ERROR refusal is "a moved key would exceed
 * the group maximum".
 *
 * @src_ft's max_used_key_len hint bounds len(K) for free.  It answers every
 * move that it admits.  Where it refuses, two cases differ:
 *
 *  - @src_len == 0: the WHOLE source moves.  This is a graft, a merge, a
 *    graft_swap or a root merge_at.  The hint bounds exactly the keys that
 *    move.  So a stale hint is repaired and asked again.  The repair is the
 *    walk that cds_ft_recompute_stats runs.
 *
 *    The repair runs only on an EXCLUSIVE source, which every bulk op
 *    requires.  The repair LOWERS the hint.  A writer that raises the hint
 *    between the walk and the store would be lost.  So the guard stays: it
 *    alone makes the store safe if a live source ever reaches here.
 *
 *    The repair runs only once per staleness.  A hint still at the value
 *    of the last walk (max_used_key_len_walked) is exact, and its refusal
 *    is true.
 *
 *  - @src_key names a SUBTREE: a same-trie rekey or a keyed merge_at.
 *    Even an exact hint bounds the whole trie.  One long key elsewhere
 *    refused every lengthening move of a short subtree.  Measured over
 *    random tries of keys of up to 250 bytes: 38 of 788 refusals were
 *    false.  A src with no keys under it was refused too.  No recompute
 *    can fix that.  So walk the moved keys and decide on their real
 *    lengths.
 *
 * -DFT_DEBUG_MOVED_KEYS_HINT_ONLY answers from the hint alone (red control).
 * Takes its own read section for the walk.
 */
static
bool ft_moved_keys_overflow(struct cds_ft *src_ft, const uint8_t *src_key,
		size_t src_len, size_t dst_len, size_t max_key_len)
{
	size_t hint = uatomic_load(&src_ft->max_used_key_len, CMM_RELAXED);
	size_t smax;
	int ret;

	if (dst_len > max_key_len)
		return true;
	if (hint <= src_len || hint - src_len <= max_key_len - dst_len)
		return false;
#ifdef FT_DEBUG_MOVED_KEYS_HINT_ONLY
	return true;
#endif
	if (!src_len) {
		if (!src_ft->exclusive ||
		    hint == uatomic_load(&src_ft->max_used_key_len_walked,
				CMM_RELAXED))
			return true;
		src_ft->group->flavor->read_lock();
		ret = (int) ft_recompute_max_used_key_len(src_ft);
		src_ft->group->flavor->read_unlock();
		if (ret)
			return true;
		hint = uatomic_load(&src_ft->max_used_key_len, CMM_RELAXED);
		return hint > max_key_len - dst_len;
	}
	src_ft->group->flavor->read_lock();
	ret = ft_moved_keys_max_len(src_ft, src_key, src_len, &smax);
	src_ft->group->flavor->read_unlock();
	if (ret)
		return true;	/* no answer: keep the conservative one */
	return smax > src_len && smax - src_len > max_key_len - dst_len;
}

/*
 * ft_graft_keylen - Internal graft helper.
 *
 * Identical to cds_ft_graft except that:
 *   - @key_len is already resolved into bytes (no CDS_FT_LEN_DEFAULT).
 *   - The fixed-length-vs-non-root rejection is NOT performed.  So
 *     cds_ft_merge can use a sub-prefix graft on fixed-length groups.
 *     It pairs the graft with a matching ft_detach_keylen at the same
 *     prefix.  The intermediate stripped-key state is purely internal
 *     and never visible to the caller.
 *   - Argument NULL/group/self checks and the FT_TP_KEY/FT_TP
 *     tracepoints are the public wrapper's responsibility.
 *
 * All other validation is performed here: overflow, memory, src empty.
 * So is the full structural body.  This helper is the single source of
 * truth for what graft actually does.
 *
 * Transaction shape (non-root):
 *   1. Build the dst-side attach invisibly (ft_graft_build).
 *   2. Run the failure-free commit:
 *      - unlink the source root;
 *      - synchronize;
 *      - apply the deferred live back-pointers;
 *      - publish the cluster;
 *      - reclaim the old nodes.
 *
 * A diverge split is fully build-invisible (no rollback).  The non-split
 * attach still uses the legacy post-sync store with a clean rollback.
 */
static
enum cds_ft_status ft_graft_keylen(struct cds_ft *dst_ft,
		const uint8_t *_key, size_t key_len,
		struct cds_ft *src_ft)
{
	/*
	 * FUNCTION SCOPE on purpose.  The retry loop used to own this
	 * handle.  But the key_len == 0 root-attach path below returns
	 * before that loop.  Its root fence takes the dst ROOT's lock, the
	 * most contended word under root-only spacing.  With the handle
	 * scoped to the retry loop, that acquire had nothing to age.
	 */
	struct ft_op optxn;
	struct cds_ft_metadata *src_rmeta;

	/*
	 * MW LOCK_FINE (step 6, §9.5): a cross-trie graft CONSUMES the
	 * whole source.  So the source must be EXCLUSIVE: no concurrent
	 * readers or writers.
	 *
	 * An exclusive src skips its FT-wide lock
	 * (ft_writer_lock_scope_enter).  Only dst's lock is ever taken.
	 * With one lock there is no cross-trie deadlock.
	 *
	 * Two LIVE lock-mode tries would need BOTH FT-wide locks, with no
	 * lock order.  Thread 1 grafts a->b while thread 2 grafts b->a:
	 * that is a circular wait.  So a live source (lock-mode,
	 * non-exclusive) is REJECTED with BUSY before anything is touched.
	 * Both tries are byte-for-byte unchanged.
	 *
	 * The caller makes the source exclusive first
	 * (cds_ft_make_exclusive).  This is the zone-graft "build private,
	 * graft" pattern.  With an exclusive src, the fused body below runs
	 * directly and is build-invisible.  So a rejected graft (POPULATED
	 * / OVERFLOW / OOM) leaves the source PRISTINE.  No residual sink
	 * is needed.
	 *
	 * The check is gated on the SOURCE alone.  dst may be a LIVE
	 * concurrent trie: that is the supported case.
	 *
	 * Both callers (cds_ft_graft, cds_ft_merge_at) reject src == dst.
	 * So past this check the source is always an exclusive, distinct
	 * trie, and this function has no shared-source path.
	 *
	 * The check applies under every writer strategy.
	 */
	if (!src_ft->exclusive)
		return CDS_FT_STATUS_BUSY_ERROR;
	assert(src_ft != dst_ft);

	ft_crosstrie_lock_mode_guard(dst_ft, src_ft);
	CDS_FT_SCOPED_WRITER(dst_ft);
	CDS_FT_SCOPED_WRITER(src_ft);

	size_t src_max;
	const struct cds_ft_key_map *km = &dst_ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *key;

	if (caa_likely(km->identity)) {
		key = _key;
	} else {
		ft_key_to_ordinals(ordinal_buf, _key, key_len, km);
		key = ordinal_buf;
	}

	if (ft_moved_keys_overflow(src_ft, NULL, 0, key_len,
			dst_ft->group->max_key_len))
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	/* After the check: a repair there may have lowered it. */
	src_max = uatomic_load(&src_ft->max_used_key_len, CMM_RELAXED);

	src_rmeta = ft_root_metadata(src_ft);

	/* Check if source trie is empty. */
	if (ft_meta_nr_child(src_rmeta) == 0 && !src_rmeta->external_nodes)
		return CDS_FT_STATUS_OK;

	if (key_len == 0) {
		struct cds_ft_metadata *dst_rmeta;
		struct cds_ft_inode_flag *dst_root_fenced;
		/*
		 * DEDUPED, not taken: an outer frame owns this fence and
		 * owns its release.  Clearing it here drops a fence the op
		 * still writes under.  One owner per fence.
		 */
		bool dst_root_shared = false;
		uintptr_t dst_root_snap;
		struct cds_ft_inode *fresh_root;
		struct cds_ft_metadata *fresh_meta;
		struct cds_ft_inode *old_dst_root;
		int fence_ret;

		/*
		 * A root-level graft needs an empty destination.  Decide
		 * that UNDER the old root's node lock.  The swap below then
		 * consumes this lock as its fenced retire.
		 *
		 * The old code tested emptiness unfenced and swapped later.
		 * A contract-legal peer attach could land in that window
		 * and be freed with the old root.
		 */
		ft_txn_op_init(dst_ft, &optxn);
		fence_ret = ft_root_attach_fence_empty(dst_ft, &dst_root_fenced,
			&dst_rmeta, &dst_root_snap, &dst_root_shared,
			&optxn);
		if (fence_ret == -EEXIST)
			return CDS_FT_STATUS_POPULATED_ERROR;
		if (fence_ret) {
			/*
			 * No attempt owns the refusal the fence counted.  Drop
			 * it, or the next op on this thread is aged for it.
			 */
			ft_acq_contended = 0;
			return CDS_FT_STATUS_BUSY_ERROR;
		}

		/*
		 * Allocate a fresh empty root for the source before
		 * swapping, so the source remains a valid trie.
		 */
		fresh_root = alloc_cds_ft_node(dst_ft, &ft_types[0], &fresh_meta);
		if (!fresh_root) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/* Fresh root for @dst_ft: name its owner while still invisible. */
		fresh_meta->parent_word = ft_trie_parent(dst_ft);

		/*
		 * The cross-trie dual root-swap txn.
		 *
		 * Create and reserve it here, while both tries are still
		 * pristine.  The swap is the op's sole
		 * reader-visible change, so this is the abort boundary.
		 *
		 * The dual always flips both roots.  So it is always
		 * multi-edge, even list-off: it cannot reduce to a lone
		 * store.
		 *
		 * There is +1 edge for the dst old-root freeze-on-free
		 * tombstone.  It is fused into the same swap (atomic detach,
		 * §4.B).
		 *
		 * A create OOM aborts cleanly: free the fresh root, and both
		 * tries are untouched.
		 *
		 * NAMED FOR @dst_ft, WRITES BOTH ROOTS: the dual flips
		 * &dst_ft->root AND &src_ft->root in one commit.  The trie
		 * named here decides only ARMING (ft_txn_content_sw_ok); both
		 * roots record MW by construction
		 * (ft_root_list_swap_publish_dual marks them), so the named
		 * trie need not stand for the foreign one.
		 */
		struct ft_flip_txn *dual_txn = ft_flip_txn_create_bounded(dst_ft,
			FT_ROOT_LIST_SWAP_DUAL_MAX_EDGES + 1);

		if (!dual_txn) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			free_cds_ft_node_unpublished(dst_ft, fresh_root);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}

		/*
		 * Root-level graft: the source's root becomes the
		 * destination's root with no parent-pointer change
		 * (both are root positions with parent == NULL).  No
		 * "jump out" window, so the STRUCTURAL swap needs no
		 * internal synchronize_rcu.
		 *
		 * Swap root pointers.  The source's root carries all
		 * metadata (nr_child, external_nodes) with it.  The
		 * swap orphans the destination's old (empty) root.
		 * Reclaim it via call_rcu.  Concurrent readers that
		 * entered before the swap then finish their descent
		 * first.
		 */
		/*
		 * Use the FENCED root, not a fresh read of @dst_ft->root.
		 * Under the fence the two are equal by construction.  With
		 * the fenced value, the retire, the swap's expected-old and
		 * the free all name the SAME node.
		 */
		old_dst_root = ft_node_ptr(dst_root_fenced);
		/*
		 * Ordered list: dst was empty (checked above), so src's
		 * WHOLE ordered list becomes dst's.  The cells' internal
		 * links are unchanged.  The head/tail endpoints transfer.
		 *
		 * The outer links of the moved run are NULL-terminated.  The
		 * dual does not relink them to a foreign sentinel.  There is
		 * no drain between detach and attach here.  So a src
		 * straddler must see a universal end, not dst's sentinel.
		 *
		 * Fuse BOTH sides into ONE cross-trie flip.  dst and src
		 * share a group, so a single epoch flip settles every
		 * root/endpoint proxy at once:
		 *  - dst (appear): src->root AND src's head/tail (dst's were
		 *    NULL);
		 *  - src (disappear): retire src->root to a fresh empty root
		 *    AND clear src's head/tail.
		 * A reader resolves all slots to ONE phase.  So it never
		 * sees the key reachable in BOTH tries (appear done,
		 * disappear pending) or in NEITHER.
		 *
		 * Capture src's live root and endpoints first: the flip
		 * overwrites them.
		 *
		 * List off: head/tail are unused.  The swap reduces to the
		 * two lone root edges, MCAS-expressible like the list-on
		 * path.  Both are root positions with parent == NULL, so no
		 * internal synchronize_rcu is required.
		 */
		/*
		 * The exclusive src has no straddler.  So its run lands in
		 * @dst_ft already linked to @dst_ft's sentinel.  The
		 * whole-trie graft_swap relinks @swap_ft's run the same way.
		 * Only the FT_RED_SEAM_NULLTERM red control leaves the
		 * universal end, for the finalize below.
		 */
		struct cds_ft *dst_relink = FT_SEAM_RELINK(dst_ft);

		{
			struct cds_ft_inode_flag *dst_old = dst_root_fenced;
			struct cds_ft_inode_flag *src_root = src_ft->root;
			struct ft_ord_cell *src_head = ft_ord_first(src_ft);
			struct ft_ord_cell *src_tail = ft_ord_last(src_ft);
			struct ft_root_swap_side appear = {
				.ft = dst_ft, .slot = &dst_ft->root,
				.old_root = dst_old, .new_root = src_root,
				.head_old = NULL, .head_new = src_head,
				.tail_old = NULL, .tail_new = src_tail,
			};
			struct ft_root_swap_side disappear = {
				.ft = src_ft, .slot = &src_ft->root,
				.old_root = src_root,
				.new_root = ft_node_flag(fresh_root, 0),
				.head_old = src_head, .head_new = NULL,
				.tail_old = src_tail, .tail_new = NULL,
				.out_relink = dst_relink,
			};

			/*
			 * Freeze-on-free (doc §4.B): this swap retires the
			 * dst old root.  Its one-way tombstone is recorded
			 * INTO dual_txn.  So the mark and the root-swap
			 * unlink flip atomically (atomic detach).
			 *
			 * src_root MOVES to dst, and fresh_root is the new
			 * src root.  Neither is freed here.  Only
			 * @old_dst_root == @dst_old is.
			 *
			 * FENCED form: the expected-old is the mark's clean
			 * snapshot with the fence bit.  A peer state change
			 * that slipped under the fence then ABORTS this
			 * commit.  A late re-read cannot ratify it.
			 *
			 * Registering hands the fence to the txn:
			 *  - the commit consumes it in the
			 *    {LOCK|s -> TOMBSTONE|s} transition;
			 *  - an abort CAS-clears it back to LIVE.
			 * So no bail path past this point owes a clear.
			 */
			ft_flip_txn_lock_register(dual_txn, dst_rmeta,
				dst_root_snap);
			ft_flip_txn_record_tombstone_locked(dual_txn, dst_rmeta,
				dst_root_snap);
			ft_root_list_swap_publish_dual(dual_txn, &appear,
				&disappear);
		}
		FT_TP(root_publish, (const void *) dst_ft,
			(const void *) dst_ft->root);
		FT_TP(root_publish, (const void *) src_ft,
			(const void *) src_ft->root);
		/*
		 * Ordered-list run finalize, red control only.  The dual
		 * NULL-terminated the moved run (universal end).  Repoint
		 * its outer links at dst's sentinel, so dst's list is
		 * circular again.  The remove folding and the reverse walk
		 * need first->prev == sentinel.  The exclusive src has no
		 * straddler, so there is no drain.  dst is live, so the
		 * finalize uses rcu_assign_pointer.  The race with dst's own
		 * readers is benign: for them both NULL and dst's sentinel
		 * mark the end.
		 */
		if (dst_ft->group->ordered_list_set && !dst_relink)
			ft_ord_finalize_circular(dst_ft);
		free_cds_ft_node(dst_ft, old_dst_root);
		goto done;
	}

	{
		struct ft_descent d;
		struct cds_ft_inode *fresh_node;
		struct cds_ft_metadata *fresh_meta;
		struct ft_glue glue;
		enum ft_graft_prep prep;
		unsigned long src_count = ft_nr_keys_get(src_rmeta);
		struct cds_ft_inode_flag *old_src_root;
		struct cds_ft_inode_flag *attached_nf = NULL;
		struct ft_ord_cell *graft_run_first = NULL, *graft_run_last = NULL;
		struct ft_ord_cell *graft_pred = NULL, *graft_succ = NULL;
		struct ft_graft_run graft_run;
		struct ft_graft_run *run_arg = NULL;
		/*
		 * The NOSPLIT store is split across the src-root swap (§11
		 * drop):
		 *  - PREPARE is the fallible recompact of the graft-point
		 *    node.  It runs BEFORE the swap.  A concurrent {p}
		 *    relocation or a stale descent is then a clean -EAGAIN
		 *    re-descend, with src untouched.
		 *  - COMMIT is unfailable and runs after the swap.  @st
		 *    carries the PREPARE result to it.
		 */
		struct ft_graft_store_state st;
		bool nosplit_prepared = false;
		/*
		 * Post-swap store commit status (§11 drop abort-safety).
		 * Under the drop the attach commit CAN abort.  Its MCAS
		 * footprint conflicts with a peer beyond the single fenced
		 * word of the pre-swap fence.
		 *
		 * Both commit arms set it.  It gates the OLD-node reclaim on
		 * a real retire.  On abort the store is attempted again.
		 */
		enum urcu_txn_status store_cst = URCU_TXN_STATUS_OK;
		/*
		 * Post-swap store RETRY (§11 drop, doc
		 * ft-graft-abort-safe-poststore-retry).
		 *
		 * The source-root swap commits below.  After it, the payload
		 * is detached from the EXCLUSIVE (reader-free) src and OWNED
		 * by this writer.  src stays empty across retries, with no
		 * reader-visible flicker.
		 *
		 * So the whole dst-side attach is a self-contained,
		 * retryable operation: re-descend -> build -> prepare ->
		 * commit.  On a commit ABORT we re-run ONLY the dst-side
		 * steps via @retry_attach.  We never touch src again.
		 *
		 * @already_swapped guards the one-shot src-side steps: the
		 * fresh-root alloc, the swap and the src-retire txn.  A
		 * retry skips them.
		 *
		 * It also turns the pre-swap OOM exits into re-attempts,
		 * since src is already consumed.  Returning MEMORY_ERROR
		 * there would orphan the payload.
		 */
		bool already_swapped = false;
		/*
		 * MW LOCK_FINE drop, FUSED cross-trie move (doc follow-up
		 * b).  An EXCLUSIVE src has no readers, so it needs no
		 * jump-out drain.  For it, the src-root retire is recorded
		 * INTO @glue.txn instead of a separate pre-commit flip.
		 *
		 * The src unlink AND the dst attach then commit as ONE
		 * atomic flip.  There is no orphan window.  A commit ABORT
		 * rolls BOTH back, and src is still full.  The retry is then
		 * a clean whole-op re-descend, identical to a pre-swap
		 * failure, with no owned-payload bookkeeping.
		 *
		 * @already_swapped is left FALSE until that fused commit
		 * SUCCEEDS.  It is set post-commit.  So the existing
		 * @already_swapped-guarded cleanup handles the abort.
		 *
		 * With the ordered list on, the src-root retire is still a
		 * separate flip (@src_retire_txn), and @already_swapped is
		 * set there.
		 */
		bool src_swap_fused = false;
		/*
		 * Self-secured NOSPLIT attach.  When no caller reserve is
		 * active, this graft reserves its own commit nodes before it
		 * publishes the empty source root.  The post-publish store
		 * then cannot fail.  It needs no reader-observable
		 * source-root rollback.
		 *
		 * Skipped under a caller reserve, which is the rekey.  Its
		 * reserve and its pre-reserved @glue.txn already make the
		 * store unfailable.
		 *
		 * The store's flip latches all draw from @glue.txn, reserved
		 * below.  So no separate flip batch is needed.
		 */
		struct cds_ft_alloc_reserve graft_reserve;
		bool self_secured = false;
		/*
		 * The src-root retire below (ft_root_list_swap_publish, list
		 * on) runs in the failure-free section.  Every fallible step
		 * has returned by then, so it cannot abort.  PRE-RESERVE its
		 * bounded txn here, in the fallible prefix, next to
		 * @glue.txn.
		 *
		 * It is reserved only when the ordered list is on.  List off
		 * retires via the lone-edge ft_root_edge_flip.  It is freed
		 * at every fallible exit below, and consumed by the retire.
		 */
		struct ft_flip_txn *src_retire_txn = NULL;
		/*
		 * Standalone run-splice fallback txn.  Every attach shape
		 * FUSES the run-splice into its structural flip
		 * (graft_run.armed).  So this fallback is not taken today.
		 * It is kept defensively.
		 *
		 * It runs in the failure-free section (post src-unlink),
		 * hence it is un-abortable.  So PRE-RESERVE its
		 * bounded txn here.
		 *
		 * It is reserved only when the ordered list is on.  The
		 * standalone splice consumes it.  When the run was fused
		 * (the common case), it is freed unused.
		 */
		struct ft_flip_txn *run_splice_txn = NULL;
		/*
		 * NIL-key-only source: the whole source is a single prefix key,
		 * stored as the root's external_nodes (a childless internal -- valid
		 * only AT a root).  Grafting that wrapper internal to a non-root
		 * position would leave a non-canonical childless internal there
		 * (cds_ft_remove_all's invariant).  Graft the external chain head
		 * DIRECTLY instead, so the placed node is a plain external, and free
		 * the orphaned wrapper on success.  (Cross-trie graft never hits this:
		 * a real source root always has children.)
		 */
		bool nil_key_root = (ft_meta_nr_child(src_rmeta) == 0
				&& src_rmeta->external_nodes != NULL);
		struct cds_ft_inode_flag *graft_payload = nil_key_root ?
			(struct cds_ft_inode_flag *) ft_dereference_external(
				src_rmeta->external_nodes) : src_ft->root;

		/*
		 * MW LOCK_FINE (§11 drop-mechanics): re-entry point for the
		 * contention retry.
		 *
		 * Under the FT-wide-lock drop a concurrent writer can
		 * RELOCATE the shared graft-point-parent {p}.  This can
		 * happen between this graft's reserve-fill and its store.
		 * The reserve manifest is then stale, and the store fails
		 * (MEMORY_ERROR).
		 *
		 * The store is build-invisible (both tries untouched,
		 * graft.h:436).  The consumed source is EXCLUSIVE (step 6,
		 * no readers).  So on that failure we roll the src-root swap
		 * back, which is reader-invisible.  Then we retry the whole
		 * attach on a fresh descent.
		 *
		 * @graft_payload and @nil_key_root are attach-invariant
		 * (computed above).  Every per-attempt resource is
		 * (re)acquired below and freed on the retry path.
		 *
		 * Progress: {p}'s node lock guarantees a winner each
		 * contention round.  This is a SYSTEM claim, not a per-op
		 * one.  One particular op can still lose every round.  Per-op
		 * progress is a different property, and @optxn supplies it.
		 */
		/*
		 * The op's PERSISTENT engine handle (doc §11).  It spans the
		 * whole @retry_attach loop, as ft_txn_op_init does for
		 * insert, remove and replace.
		 *
		 * It is what makes this loop terminate.  ft_op_conflict()
		 * ages contention ACROSS attempts.  So this writer escalates
		 * into its per-trie FIFO fair-mutex lane, and the contention
		 * drains.
		 *
		 * Unaged, every attempt restarts at retry 0.  The loop then
		 * has no termination argument at all.  Measured over one inv
		 * run: 679819 retries, 4214 of them for a single op.  That is
		 * STARVATION, not livelock.  The system still progresses, so
		 * a green suite is not evidence about it.
		 *
		 * @ra_txn is the bracket's condition, evaluated ONCE.  A
		 * bracket may only span a body that takes NO grace period.
		 * There are two independent reasons:
		 *  - ft_op_begin() enters the RCU read side.  A GP under
		 *    it waits on this very thread.
		 *  - An aged op enters the escalation lane.
		 *    ft_writer_lock_gp_wait asserts !ft_op_in_lane().  A peer
		 *    parked on that lane is an ONLINE, non-quiescent reader
		 *    that holds the GP open.
		 *
		 * The source is exclusive, so this body drains nothing: it
		 * reaches no grace period.  And dst_ft->lock_fine is what
		 * binds the escalation lane at all.  So it IS the GP-free
		 * contract.
		 */
		const bool ra_txn = dst_ft->lock_fine;
		unsigned long ra_depth __attribute__((unused)) = 0;

		ft_txn_op_init(dst_ft, &optxn);

retry_attach:
		/*
		 * Open the attempt BEFORE anything it must undo: an aged retry
		 * escalates here, and escalation blocks on the lane's fair
		 * mutex.
		 */
		if (ra_txn)
			ft_op_begin(&optxn);
		RSPIN_ENTER_X(2, ra_depth, 1, ra_txn);
		/*
		 * Preallocate a fresh empty root for the source trie before
		 * the point of no return.  A memory shortage then fails
		 * cleanly instead of calling abort().
		 *
		 * One-shot: a post-swap retry keeps src's already-published
		 * fresh root (@already_swapped).  So @fresh_node stays NULL
		 * and every src-side free below is skipped.
		 */
		if (!already_swapped) {
			fresh_node = alloc_cds_ft_node(src_ft, &ft_types[0], &fresh_meta);
			if (!fresh_node) {
				ft_txn_attempt_end(&optxn, ra_txn);
				return CDS_FT_STATUS_MEMORY_ERROR;
			}
			/* Fresh root for @src_ft: named while still invisible. */
			fresh_meta->parent_word = ft_trie_parent(src_ft);
		}

		/*
		 * PREP (dst + source pristine): build the dst-side attach.
		 * A diverge split builds its whole cluster invisibly into
		 * @glue; otherwise just locate the graft point in @d.
		 */
		ft_glue_init(&glue);
		glue.op = &optxn;
		/*
		 * Enable the split-retire @cn fence for this graft: ft_graft_keylen's
		 * retry_attach loop handles a fence-miss (FT_GRAFT_PREP_RETRY) as a
		 * clean re-descend.  ft_glue_init reset it, so set it each attempt.
		 */
		glue.fence_split_cn = true;
		/*
		 * Every attach shape commits through @glue.txn: the GLUE
		 * diverge, the displaced-external, and the in-place NOSPLIT
		 * slot store.  Its live re-parents, its forward publish and
		 * its ordered-list run-splice then flip atomically.  There
		 * is no deferred-edge ordering window.
		 *
		 * Create it here and reserve it up front to the
		 * floor-bounded cluster size.  Records then cannot fail
		 * mid-build, like the glue floor arrays.
		 *
		 * The + 7 headroom covers:
		 *  - the forward edge's 1-2 stores;
		 *  - the <=4 run-splice cell edges.  This also covers the
		 *    in-place slot proxy + its run edges
		 *    (FT_GRAFT_RUN_FLIP_CAP);
		 *  - the +1 recompact-relocate retire tombstone fused into
		 *    the commit (atomic detach, §4.B;
		 *    st->old_recompacted_node below).
		 *
		 * The txn is created before the build only to keep its
		 * lifecycle in one place.  The build records nothing into
		 * it.  Post-unlink, the commit replays g->deferred through
		 * it, and the store reserves its slot proxy through it.
		 */
		glue.txn = ft_flip_txn_create(dst_ft);
		if (!glue.txn || !ft_flip_txn_reserve(glue.txn,
				/* + FLOOR_FREE: fused free-list tombstones (§4.B) */
				FT_GLUE_FLOOR_DEFERRED + 7 + 1 /* +1 §4.B parent guard */ + FT_GLUE_FLOOR_FREE
				/* +1 fused src-root retire edge (follow-up b) */
				+ 1
				/* +1 fused nil-key wrapper tombstone */
				+ (nil_key_root ? 1 : 0)
				/* + count walk: the +src_count nr_keys ancestor edges (BULK fold) */
				+ (dst_ft->rank_stats ? (int) key_len + 1 : 0)
				/* the split-retire terminal's second word, if any */
				+ ft_glue_split_cn_reserve(dst_ft))) {
			if (glue.txn)
				ft_flip_txn_destroy(glue.txn);
			if (already_swapped) {
				ft_txn_attempt_bail(&optxn, ra_txn);
				goto retry_attach;	/* src consumed: OOM is transient */
			}
			free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_end(&optxn, ra_txn);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/*
		 * Fuse each retired free-list node's freeze into glue.txn
		 * (atomic detach, §4.B).  Each tombstone rides the same
		 * commit as the forward publish that unlinks it.  So the
		 * retired set freezes dead atomically with the unlink.
		 *
		 * The create above reserves the <= FT_GLUE_FLOOR_FREE
		 * tombstone headroom (+ FLOOR_FREE).
		 *
		 * The graft never calls ft_glue_reserve.  So cap_free stays
		 * FT_GLUE_FLOOR_FREE, and that headroom is the exact bound.
		 */
		glue.fuse_free_list = true;
		prep = ft_graft_build(dst_ft, key, key_len, graft_payload,
				src_count, &d, &glue, /*outer*/ NULL);
		if (prep == FT_GRAFT_PREP_OOM) {
			/*
			 * The GLUE build may have marked @cn's split-retire
			 * LOCK fence before it hit OOM.  ft_glue_abort below
			 * releases it (single clear point).  So @cn stays
			 * LIVE for the re-descend.
			 */
			ft_glue_abort(dst_ft, &glue);
			if (glue.txn)
				ft_flip_txn_destroy(glue.txn);
			if (already_swapped) {
				ft_txn_attempt_bail(&optxn, ra_txn);
				goto retry_attach;	/* src consumed: OOM is transient */
			}
			free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_end(&optxn, ra_txn);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		if (prep == FT_GRAFT_PREP_RETRY) {
			/*
			 * Split-retire @cn fence miss (MW LOCK_FINE drop): a
			 * peer owns @cn.  It is splitting, growing or
			 * recompacting it, or changing cn->child.
			 *
			 * ft_split_compressed_graft_build built NOTHING and
			 * did NOT mark @cn.  So this is a clean re-descend.
			 * dst is byte-for-byte unchanged.  src is pristine
			 * (pre-swap), or empty and owned by this writer
			 * (post-swap).
			 *
			 * @cn's try-or-bail LOCK mark guarantees a winner
			 * each contention round (no livelock).
			 */
			ft_glue_abort(dst_ft, &glue);
			if (glue.txn)
				ft_flip_txn_destroy(glue.txn);
			if (src_retire_txn) {
				ft_flip_txn_destroy(src_retire_txn);
				src_retire_txn = NULL;
			}
			if (run_splice_txn) {
				ft_flip_txn_destroy(run_splice_txn);
				run_splice_txn = NULL;
			}
			if (!already_swapped)
				free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_bail(&optxn, ra_txn);
			goto retry_attach;
		}
		if (prep == FT_GRAFT_PREP_POPULATED) {
			/*
			 * Post-swap POPULATED is impossible for the supported
			 * contract.  That contract is an EXCLUSIVE src, and a
			 * live dst whose graft key is this writer's own: no
			 * peer creates @key.
			 *
			 * The payload is already detached, so there is no
			 * clean status left.  Assert rather than silently
			 * orphan it.  A general (non-disjoint) caller must
			 * move to the fused src-retire+dst-store txn (doc,
			 * open follow-up).
			 */
			assert(!already_swapped);
			if (glue.txn)
				ft_flip_txn_destroy(glue.txn);
			if (!already_swapped)	/* NDEBUG: @fresh_node is NULL post-swap */
				free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_end(&optxn, ra_txn);
			return CDS_FT_STATUS_POPULATED_ERROR;
		}
		/*
		 * Reanchor level-move (ft_descent skip_conflict): a peer
		 * chain-merge moved an encoded position SHALLOWER
		 * mid-descent.  The captured publish chain is then at the
		 * wrong level.  That chain is @d->pnf / @d->pnfp, and the
		 * coherent graft-point parent @d->ppnf that the recompact
		 * inherits.
		 *
		 * A mutating caller must re-descend against the now-current
		 * tree.  Insert takes the same bail (ft-insert.h
		 * skip_conflict).
		 *
		 * Nothing is reader-visible yet: the build is invisible and
		 * d is a pure descent.  So the abort boundary is
		 * byte-for-byte clean.  Drop the invisible build, the
		 * reserved txn and the fresh root, then re-descend.
		 */
		if (caa_unlikely(d.skip_conflict)) {
			ft_glue_abort(dst_ft, &glue);
			if (glue.txn)
				ft_flip_txn_destroy(glue.txn);
			if (!already_swapped)
				free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_bail(&optxn, ra_txn);
			goto retry_attach;
		}
		/*
		 * The NOSPLIT graft point is already occupied.  The store
		 * would report this post-swap.  Report POPULATED here,
		 * BEFORE the source-root swap, so the source stays pristine.
		 * There is no rollback and no reader-observable
		 * empty-then-full flicker.  The store checks the same
		 * condition at d->depth == key_len.
		 */
		if (prep == FT_GRAFT_PREP_NOSPLIT && d.depth == key_len && d.nf) {
			assert(!already_swapped);	/* see the PREP_POPULATED note above */
			ft_flip_txn_destroy(glue.txn);
			if (!already_swapped)	/* NDEBUG: @fresh_node is NULL post-swap */
				free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_end(&optxn, ra_txn);
			return CDS_FT_STATUS_POPULATED_ERROR;
		}

		/*
		 * Pre-reserve the txn of the failure-free src-root retire
		 * now.  This is after the last POPULATED / OOM exit, and
		 * before the only remaining fallible step (the self-secure
		 * reserve below).  The retire is past the point of no return
		 * and cannot abort.  So it commits through this pre-reserved
		 * txn (ft_ord_cell_flip_into).
		 *
		 * When to reserve:
		 *  - List off: the src-root retire and a nil-key wrapper's
		 *    tombstone both fuse into @glue.txn (the src-swap-fused
		 *    arm below), reserved above.  So no retire txn.
		 *  - List on: +1 for a nil-key wrapper's fused tombstone.
		 *
		 * One-shot (src-side): the swap below consumes the
		 * src-retire txn.  So a post-swap retry keeps the src empty
		 * and does NOT re-reserve it (@already_swapped).  Both txns
		 * stay NULL, and the fused commit needs neither:
		 *  - @src_retire_txn was consumed;
		 *  - every attach shape fuses the run into glue.txn.  So the
		 *    standalone splice that would use @run_splice_txn is
		 *    never taken.
		 */
		if (!already_swapped && dst_ft->group->ordered_list_set) {
			src_retire_txn = ft_flip_txn_create_bounded(src_ft,
				FT_ROOT_LIST_SWAP_MAX_EDGES + 1);
			run_splice_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_ORD_CELL_RUN_SPLICE_MAX_EDGES);
			if (!src_retire_txn || !run_splice_txn) {
				/*
				 * Release the GLUE build's @cn split-retire fence (if
				 * held) so @cn stays LIVE -- this OOM bail does not route
				 * through ft_glue_abort.
				 */
				if (glue.split_cn_holder) {
					/* SHARED: the caller's earlier acquire owns the release. */
					if (!glue.split_cn_shared)
						ft_meta_lock_release(glue.split_cn_holder);
					glue.split_cn_holder = NULL;
					glue.split_cn_shared = false;
					glue.split_cn_snap = 0;
					glue.split_cn_node = NULL;
					glue.split_cn_node_snap = 0;
				}
				if (src_retire_txn)
					ft_flip_txn_destroy(src_retire_txn);
				if (run_splice_txn)
					ft_flip_txn_destroy(run_splice_txn);
				ft_flip_txn_destroy(glue.txn);
				free_cds_ft_node_unpublished(src_ft, fresh_node);
				ft_txn_attempt_end(&optxn, ra_txn);
				return CDS_FT_STATUS_MEMORY_ERROR;
			}
		}

		/*
		 * Ordered list: locate the dst splice neighbours NOW, while
		 * dst is still payload-free.  The attach is built invisibly
		 * and is not yet published.  A relational descent after the
		 * payload is live would return a payload head as the
		 * boundary.
		 */
		if (dst_ft->group->ordered_list_set)
			ft_ord_cell_find_splice_pos(dst_ft, _key, key_len,
				&graft_pred, &graft_succ, NULL);

		/*
		 * Self-secure the NOSPLIT store BEFORE the point of no
		 * return (the source-root swap below).  Draw a generous node
		 * reserve here, where failure is clean: nothing is published
		 * yet.  It makes the post-swap ft_store_at_graft_point
		 * unfailable.
		 *
		 * The old rollback re-published the source root on a store
		 * OOM.  Readers could observe the source flicker: empty,
		 * then full again.  That rollback is gone.
		 *
		 * The store's flip latches draw from the pre-reserved
		 * @glue.txn.
		 *
		 * Skipped when a caller reserve is already active: that
		 * reserve and the pre-reserved txn secure the store.
		 */
		if (prep == FT_GRAFT_PREP_NOSPLIT
				&& !cds_ft_alloc_reserve_covers(dst_ft)) {
			memset(&graft_reserve, 0, sizeof(graft_reserve));
			if (ft_bulk_node_reserve_fill(dst_ft, &graft_reserve)) {
				cds_ft_alloc_reserve_drain(dst_ft, &graft_reserve);
				ft_glue_abort(dst_ft, &glue);
				ft_flip_txn_destroy(glue.txn);
				if (src_retire_txn)
					ft_flip_txn_destroy(src_retire_txn);
				if (run_splice_txn)
					ft_flip_txn_destroy(run_splice_txn);
				if (already_swapped) {
					ft_txn_attempt_bail(&optxn, ra_txn);
					goto retry_attach;	/* src consumed: OOM is transient */
				}
				free_cds_ft_node_unpublished(src_ft, fresh_node);
				ft_txn_attempt_end(&optxn, ra_txn);
				return CDS_FT_STATUS_MEMORY_ERROR;
			}
			self_secured = true;
		}

		/*
		 * MW LOCK_FINE drop (§11, cross-trie): run the FALLIBLE half
		 * of the NOSPLIT store BEFORE the point-of-no-return
		 * src-root swap.  That half is
		 * ft_store_at_graft_point_prepare, i.e. the recompact of the
		 * graft-point node {p}.
		 *
		 * Each of these then surfaces as a clean re-descend:
		 *  - a concurrent relocation of {p};
		 *  - a peer that holds the node lock of {p} or of the
		 *    grandparent;
		 *  - a stale descent.
		 * NOTHING on src is touched: no reader-visible src flicker,
		 * no rollback.
		 *
		 * The remaining commit is unfailable.  It runs after the
		 * swap through @st.
		 *
		 * The reserve (self-secured above, or the caller's) covers
		 * the recompact copy.  It is deactivated and drained here.
		 * The copy survives in @st, and the commit allocates
		 * nothing.
		 *
		 * Staleness guard: a NOSPLIT graft point is an INTERNAL slot
		 * owner.  A compressed d.pnf means the descent raced a
		 * concurrent chain-compress / relocation
		 * (ft_node_set_nth_rec cannot target a compressed node).
		 * Re-descend.
		 *
		 * A tombstoned / proxied / peer-locked {p} is caught INSIDE
		 * the recompact's lock acquire (ft_meta_lock_acquire ->
		 * -EAGAIN -> prepare MEMORY_ERROR).
		 */
		if (prep == FT_GRAFT_PREP_NOSPLIT) {
			enum cds_ft_status pstatus;
			/*
			 * Displaced-external shape: the descent broke on an
			 * external d.nf.  The store re-parents that external
			 * under a fresh branch.  It publishes the branch at
			 * d.pnf's CHILD slot
			 * (ft_store_at_graft_point_prepare's displaced arm).
			 * It never targets d.pnf's byte-slots.
			 *
			 * A compressed d.pnf is then the STABLE, expected
			 * structure: the graft key runs through a compressed
			 * path to an external leaf.  It is NOT a transient
			 * race.  So the store handles it as-is.
			 */
			bool displaced_shape = d.nf && ft_node_external(d.nf);

			/*
			 * A BYTE-SLOT store is depth==key_len, or a
			 * non-displaced diverge.  A compressed d.pnf there
			 * means the descent raced a concurrent chain-compress
			 * / relocation.  ft_node_set_nth_rec cannot target a
			 * compressed node, so re-descend.
			 *
			 * The displaced shape above is EXEMPT.  Guarding it
			 * spun forever in single-writer.  No peer is there to
			 * un-compress a stable compressed parent, so the
			 * re-descend never made progress.
			 */
			if ((ft_node_compressed(d.pnf)
					|| ft_node_skip_compressed(d.pnf))
					&& !displaced_shape) {
				pstatus = CDS_FT_STATUS_MEMORY_ERROR;
			} else {
				if (self_secured)
					cds_ft_alloc_reserve_activate(dst_ft,
						&graft_reserve);
				pstatus = ft_store_at_graft_point_prepare(dst_ft,
					key, key_len, &d, graft_payload,
					src_count, &glue, /*outer*/ NULL, &st);
				if (self_secured)
					cds_ft_alloc_reserve_deactivate(dst_ft);
			}
			if (self_secured) {
				cds_ft_alloc_reserve_drain(dst_ft, &graft_reserve);
				self_secured = false;
			}
			if (pstatus != CDS_FT_STATUS_OK) {
				/*
				 * Prepare failed BEFORE the swap: src is pristine,
				 * only the invisible dst build + reserved txns need
				 * unwinding.  Re-descend -- {p}'s node lock
				 * guarantees a winner each contention round.
				 */
				ft_glue_abort(dst_ft, &glue);
				if (glue.txn)
					ft_flip_txn_destroy(glue.txn);
				if (src_retire_txn) {
					ft_flip_txn_destroy(src_retire_txn);
					src_retire_txn = NULL;
				}
				if (run_splice_txn) {
					ft_flip_txn_destroy(run_splice_txn);
					run_splice_txn = NULL;
				}
				if (!already_swapped)
					free_cds_ft_node_unpublished(src_ft, fresh_node);
				ft_txn_attempt_bail(&optxn, ra_txn);
				goto retry_attach;
			}
			nosplit_prepared = true;
		}

		/*
		 * MW LOCK_FINE drop (§11, cross-trie): the commit does a
		 * forward publish into the captured dst spine parent
		 * @fence_parent.  That parent is d.pnf, a LIVE node from the
		 * descent.  The commit runs AFTER the src-root swap below.
		 *
		 * Under the FT-wide-lock drop a concurrent peer can RETIRE
		 * that spine node between this graft's descent and its
		 * commit.  The peer can be a sibling graft that grows the
		 * node, or a point-remove that recompacts it.  Two bad
		 * outcomes follow:
		 *  - The commit's value-swap guard passes on a
		 *    dead-but-stable state word.  The forward publish then
		 *    stores into a tombstoned node.  This wild store
		 *    corrupts the arena.
		 *  - Worse, the guard MISMATCHES the tombstoned word.  The
		 *    commit then ABORTS past the point of no return and
		 *    silently drops the whole subtree.
		 *
		 * So lock @fence_parent's node lock HERE, before the swap.
		 * No peer can then retire it through the commit.  The commit
		 * records the held {LOCK|s -> s} release via
		 * @publish_parent_holder, so the commit is truly unfailable.
		 *
		 * A miss (already retired / proxied / peer-locked) is a
		 * clean re-descend with src pristine.  ft_meta_lock_acquire
		 * did NOT set the fence.  So only the invisible dst build
		 * needs unwinding.
		 *
		 * TWO shapes reach an unfenced forward publish into d.pnf:
		 *  - the GLUE diverge (@glue.publish_parent set at build);
		 *  - the NOSPLIT DISPLACED-external attach.  Its commit
		 *    publishes the fresh branch into d.pnf's child slot
		 *    (st.pnf == d.pnf).  @st is valid once
		 *    @nosplit_prepared.
		 *
		 * The in-place NOSPLIT store needs no fence here.  Its
		 * ft_node_set_nth_rec recompacts and node-locks {p} inside
		 * prepare, and holds it through commit.
		 *
		 * Gated on lock_fine, so the FT-wide-lock build is
		 * byte-identical (the mutex already serialises retires).
		 */
		{
			struct cds_ft_inode_flag *fence_parent = NULL;

			if (dst_ft->lock_fine) {
				if (prep == FT_GRAFT_PREP_GLUE)
					fence_parent = glue.publish_parent;
				else if (nosplit_prepared && st.displaced_shape)
					fence_parent = st.pnf;
			}
			/*
			 * The GLUE build already fenced @cn (== d.nf), the
			 * compressed divergence node it splits and replaces.
			 * ft_split_compressed_graft_build marks it BEFORE it
			 * reads its plan.  So the whole build runs under the
			 * fence, and glue.split_cn_holder is already set.
			 *
			 * Here we fence only @publish_parent, the LIVE node
			 * the forward publish stores INTO.  @cn's fence does
			 * not cover it.
			 */
			if (fence_parent) {
				struct cds_ft_metadata *pp_meta =
					ft_flag_to_metadata(dst_ft, fence_parent);
				struct ft_lock_ctx fctx;
				struct ft_held_anchor fh;
				unsigned int fdep;

				/*
				 * ROUTED, not raw.  Under a coarse spacing
				 * this fence collided with the glue split's
				 * own anchor.  The op missed against its OWN
				 * hold, aborted, and retried into the
				 * identical shape.  A miss is a clean
				 * re-descend for a PEER's hold, never for
				 * the op's own.  So the dedupe has to see
				 * this one.
				 *
				 * That takes the GLUE's ctx, not a bare one.
				 * The glue's marks are part of this op's
				 * held set.  And @fence_parent is routinely
				 * the very word the build fenced as
				 * @split_cn_holder.  ft_lock_ctx_init
				 * carries no glue.  With it, the dedupe
				 * cannot see that hold, and the acquire
				 * refuses the op's own mark after all.  The
				 * descent stays this site's own.
				 */
				ft_glue_lock_ctx(&glue, &fctx);
				fctx.d = &d;
				if (!ft_lock_ctx_depth_of(dst_ft, &fctx,
							fence_parent, &fdep) ||
						ft_acquire_member(dst_ft, &fctx,
							fence_parent, pp_meta,
							fdep, &fh)) {
					/*
					 * A miss (@publish_parent already retired / proxied /
					 * peer-locked) is a clean re-descend, src pristine.
					 * ft_glue_abort below releases the @cn split-retire
					 * fence held by the GLUE build (if any), so no fence
					 * leaks (the commit never registered it).
					 */
					ft_glue_abort(dst_ft, &glue);
					if (glue.txn)
						ft_flip_txn_destroy(glue.txn);
					if (src_retire_txn) {
						ft_flip_txn_destroy(src_retire_txn);
						src_retire_txn = NULL;
					}
					if (run_splice_txn) {
						ft_flip_txn_destroy(run_splice_txn);
						run_splice_txn = NULL;
					}
					if (!already_swapped)
						free_cds_ft_node_unpublished(src_ft, fresh_node);
					ft_txn_attempt_bail(&optxn, ra_txn);
					goto retry_attach;
				}
				/*
				 * SHARED is the dedupe SUCCEEDING, not a
				 * miss.  The op already holds the word that
				 * protects @fence_parent.  So the exclusion
				 * this fence exists for is in force.  The
				 * member owes no release and no terminal:
				 * the acquire that first took the word
				 * recorded both.
				 *
				 * Leave the holder unset.  The commit's
				 * ft_flip_txn_hold_or_lock_parent then takes
				 * its ordinary acquire-or-guard route.  Its
				 * own shared arm records nothing, for that
				 * same reason.
				 *
				 * Bailing here refuses the op's OWN hold,
				 * which no re-descend can clear.
				 */
				if (!fh.shared) {
					glue.publish_parent_holder = fh.lock;
					glue.publish_parent_snap = fh.lock_snap;
				}
			}
		}

		/*
		 * No "jump out" drain.  A reader inside src_ft's payload
		 * would see dst_ft's ancestors once the payload is
		 * re-parented.  But the source is exclusive: it has no
		 * readers to drain.
		 */
		/*
		 * Point of no return -- ONE-SHOT (doc post-store retry).
		 * Capture @old_src_root and the src ordered-list run.  Then
		 * unlink the src root (publish a fresh empty root).
		 *
		 * A post-swap commit-abort retry keeps src empty and re-runs
		 * only the dst-side attach.  So this whole block runs
		 * exactly once:
		 *  - @already_swapped guards it;
		 *  - @fresh_node is nulled.  It is now src's LIVE root, so
		 *    every src-side free above is skipped;
		 *  - @src_retire_txn is consumed here and nulled.  A retry's
		 *    earlier exits then never re-destroy it.
		 *
		 * @graft_run_first / _last persist across retries.  The run
		 * moved with the payload.  src is now empty, so the run
		 * cannot be re-read from it.
		 */
		if (!already_swapped) {
			old_src_root = src_ft->root;

			/*
			 * Freeze-on-free (doc §4.B): a NIL-key graft frees
			 * the orphaned src-root wrapper @old_src_root below.
			 * Its external chain was grafted into dst, which
			 * leaves the wrapper empty.  Its tombstone rides the
			 * SAME flip as the src-root retire that unlinks it
			 * (atomic detach).  Each arm below records it into
			 * the retire txn.
			 *
			 * Non-NIL: @old_src_root IS the payload, moved LIVE
			 * into dst.  It must NOT be marked.
			 *
			 * There is no fallible step between here and the
			 * retire: all returned above.
			 *
			 * Ordered list: capture src's whole list (the run to
			 * graft).  Unlink it from src together with the
			 * structural src-root retire, FUSED into ONE flip.  A
			 * src reader then never sees src structurally empty
			 * while its ordered list still shows the run, or
			 * vice versa.
			 *
			 * The run is spliced into dst after the structural
			 * publish (same commit point).
			 *
			 * No rollback: every failure mode (OOM / populated)
			 * returned above, before this retire.
			 */
			if (!dst_ft->group->ordered_list_set) {
				/*
				 * FUSED cross-trie move (list off): record the src-root retire INTO
				 * @glue.txn instead of a separate flip.  The
				 * src unlink then flips ATOMICALLY with the
				 * dst attach below.  There is no orphan
				 * window, and no drain (no readers).
				 *
				 * @already_swapped stays FALSE until that
				 * commit succeeds (set post-commit).  So a
				 * commit abort rolls this src edge back too.
				 * The retry re-descends with src still full.
				 * It is clean, like a pre-swap failure.
				 */
				/* Stage 5: the root's own lock, so this record can park SW. */
				(void) ft_root_lock_take(src_ft, NULL, glue.txn);
				ft_flip_txn_record_root(glue.txn,
					(void **) &src_ft->root,
					(void *) old_src_root,
					(void *) ft_node_flag(fresh_node, 0));
				/*
				 * NIL-key src: @old_src_root is the emptied
				 * wrapper.  Its external chain moved LIVE
				 * into dst.  Its freeze-on-free tombstone
				 * rides the SAME fused flip (atomic detach
				 * §4.B).  So the unlink and the freeze
				 * commit atomically with the attach.  An
				 * abort rolls the tombstone back too.  The
				 * wrapper is reclaimed at the nil-key free
				 * below.
				 *
				 * Non-nil: @old_src_root IS the payload,
				 * moved LIVE.  No tombstone.
				 */
				if (nil_key_root)
					ft_flip_txn_record_tombstone(glue.txn,
						cds_ft_item_to_metadata(
							ft_node_ptr(old_src_root)));
				src_swap_fused = true;
			} else {
				graft_run_first = ft_ord_first(src_ft);
				graft_run_last = ft_ord_last(src_ft);
				/*
				 * Just empty src's sentinel here
				 * (relink_dest NULL).  The run-splice below
				 * re-homes the run's cells into dst.  It
				 * repoints their outer links to dst's
				 * neighbours / sentinel.
				 */
				if (nil_key_root)
					ft_flip_txn_record_tombstone(src_retire_txn,
						cds_ft_item_to_metadata(
							ft_node_ptr(old_src_root)));
				ft_root_list_swap_publish(src_ft, src_retire_txn,
					&src_ft->root,
					old_src_root, ft_node_flag(fresh_node, 0),
					graft_run_first, NULL, graft_run_last, NULL,
					NULL, false);
			}
			if (!src_swap_fused) {
				/*
				 * Separate swap committed: src is now empty
				 * and the payload is owned.
				 *
				 * The FUSED arm defers all of this to its
				 * atomic commit (below).  src->root is
				 * unchanged here.  So that arm keeps
				 * @already_swapped false and @fresh_node
				 * live until then.
				 */
				FT_TP(root_publish, (const void *) src_ft,
					(const void *) src_ft->root);
				already_swapped = true;
				fresh_node = NULL;	/* now src's live root: never freed below */
				src_retire_txn = NULL;	/* consumed by the swap above */
			}
		}

		/*
		 * Ordered list: arm the run-splice fusion so the NOSPLIT store
		 * commits the structural attach and the ordered-list splice in ONE
		 * flip (closing the appear-side cross-view window).  The GLUE path
		 * and the displaced-external store shape leave @armed false, falling
		 * back to the standalone two-commit splice below.
		 */
		if (graft_run_first) {
			/*
			 * NIL-key graft: stamp the spliced cell's key-rebuild
			 * byte BEFORE the cell is fused into dst's ordered
			 * list.  Once the fused splice publishes the cell, a
			 * reader that iterates to it rematerializes the key
			 * via incoming_byte.
			 *
			 * ft_set_parent does not maintain it for externals.
			 * The stamp is harmless when the parent is
			 * compressed: the up-walk ignores it there.
			 */
			if (nil_key_root)
				cds_ft_item_to_metadata(graft_run_first)->incoming_byte =
					key[key_len - 1];
			graft_run.run_first = graft_run_first;
			graft_run.run_last = graft_run_last;
			graft_run.pred = graft_pred;
			graft_run.succ = graft_succ;
			graft_run.armed = false;
			run_arg = &graft_run;
		}

		if (prep == FT_GRAFT_PREP_GLUE) {
			/*
			 * Failure-free commit of the build-invisible diverge
			 * cluster.  Nothing can fail.  The steps:
			 *  - wire the deferred live back-pointers (the
			 *    displaced old child, the payload, and the
			 *    cluster top);
			 *  - splice the cluster into dst with a single
			 *    forward publish;
			 *  - then reclaim the old compressed node and the
			 *    source's old root.
			 *
			 * glue.txn commits these as ONE atomic flip-txn:
			 *  - the back-pointers;
			 *  - the forward publish;
			 *  - (list on) the <=4 ordered-list run-splice cell
			 *    edges.
			 * So the whole attach, structure and ordered list,
			 * is observed old XOR new.  There is no
			 * deferred-edge ordering window.  ft_glue_txn_commit
			 * arms @run_arg, so the standalone splice below is
			 * skipped.
			 *
			 * Order-statistics fold (BULK): the diverge cluster
			 * raises @glue.publish_parent's subtree by
			 * +src_count.  Record that walk into the same commit
			 * (a no-op when rank stats off).
			 */
			glue.count_delta = (long) src_count;
			store_cst = ft_glue_txn_commit(dst_ft, &glue, run_arg);
			attached_nf = glue.attached_nf;
			/*
			 * Defect D / abort-safety: the diverge cluster's OLD
			 * nodes are reclaimable only when the commit
			 * committed.  They are the compressed graft-point
			 * node, gathered on glue's free list.
			 *
			 * Under the drop ft_glue_txn_commit CAN abort.  A
			 * peer conflicts on the forward-publish footprint
			 * that the pre-swap fence does not cover.  An
			 * aborted commit left those nodes LIVE.  Freeing
			 * them here would double-free them against the peer.
			 *
			 * Gate on @store_cst.  The post-swap retry below
			 * re-descends and re-commits.
			 */
			if (store_cst == URCU_TXN_STATUS_OK)
				ft_glue_free_old(dst_ft, &glue);
		} else {
			/*
			 * Non-split attach: the payload subtrie is built into
			 * @glue with its back-pointers deferred, recompacted into
			 * the live graft-point node, and published -- all inside
			 * ft_store_at_graft_point.  Every node draws from the
			 * reserve (self-secured above, or the caller's) and the
			 * slot proxy + run-splice edges draw from the pre-reserved
			 * @glue.txn, so the store has no fallible step left: it
			 * cannot fail, and there is NO source-root rollback (which
			 * would have flickered the source empty-then-full).
			 */
			unsigned int attached_depth = 0;

			/*
			 * NOSPLIT commit: the fallible half (the recompact of
			 * the graft-point node {p}) already ran
			 * build-invisibly BEFORE the swap (@st /
			 * @nosplit_prepared).  The commit only records three
			 * things into the pre-reserved @glue.txn and flips
			 * them:
			 *  - the forward slot edge;
			 *  - the relocation edges;
			 *  - the ordered-list run-splice.
			 * There is no allocation, no fallible step and no
			 * src rollback.
			 */
			assert(nosplit_prepared);
			store_cst = ft_store_at_graft_point_commit(dst_ft,
				&attached_nf, &attached_depth, run_arg, &st,
				(long) src_count);
		}

		/*
		 * FUSED cross-trie move: the src-root retire rode @glue.txn.
		 * A SUCCESSFUL commit atomically emptied src AND published
		 * the payload into dst.  src's new root is @fresh_node.
		 * There was no orphan window.
		 *
		 * Only NOW is the swap done.  Mark it, so that the abort
		 * handling and any teardown treat src as consumed.  Stop
		 * guarding @fresh_node: it is src's LIVE root.
		 *
		 * A FAILED commit rolled the src-root edge back too.
		 * @already_swapped stays false.  The store-abort retry below
		 * re-descends with src still full.  It is a clean
		 * pre-swap-style re-attempt.
		 */
		if (store_cst == URCU_TXN_STATUS_OK && src_swap_fused) {
			already_swapped = true;
			fresh_node = NULL;
			FT_TP(root_publish, (const void *) src_ft,
				(const void *) src_ft->root);
		}

		/*
		 * Post-swap store ABORT (§11 drop, doc
		 * ft-graft-abort-safe-poststore-retry).  Under the
		 * FT-wide-lock drop the attach commit can ABORT.  It
		 * conflicts with a peer beyond the single word that the
		 * pre-swap fence covers.
		 *
		 * State after the abort:
		 *  - the rolled-back flip left dst byte-for-byte unchanged;
		 *  - every registered node lock auto-cleared.  The
		 *    recompact's {p} retire and the Fix-A publish fence are
		 *    both back to LIVE;
		 *  - glue.txn is consumed;
		 *  - the payload is still detached from the emptied
		 *    EXCLUSIVE (reader-free) src, and OWNED by us.
		 *
		 * So RE-ATTEMPT the dst-side attach.  Free this attempt's
		 * UNPUBLISHED products, then re-descend.  The aborted txn
		 * rolled its edges back but freed no nodes.
		 *
		 * The retry terminates.  A peer committed a conflicting
		 * flip, so it made progress.  {p}'s LOCK try-lock guarantees
		 * a winner each round.  The re-descent runs inside the
		 * next attempt's read section.
		 */
		if (store_cst != URCU_TXN_STATUS_OK) {
			/*
			 * Free the failed attempt's unpublished new nodes.
			 *
			 * GLUE: the whole diverge cluster (every fresh node
			 * ft_glue_track'd).  glue.txn is already NULL: the
			 * commit consumed it.
			 *
			 * NOSPLIT: the displaced branch and any glue cluster.
			 * If the reserve RELOCATED {p}, also free the
			 * unpublished relocated {p}' (@st.dest).
			 * ft_node_recompact allocated it directly, so it is
			 * NOT glue-tracked.  An IN-PLACE recompact
			 * (old_recompacted_node == NULL) leaves @st.dest ==
			 * the LIVE d->pnf: do NOT free it.
			 *
			 * Never ft_glue_free_old here.  Those OLD nodes stay
			 * LIVE, because the abort rolled their retire back.
			 * Whoever finally commits reclaims them.
			 */
			ft_glue_abort(dst_ft, &glue);
			if (prep != FT_GRAFT_PREP_GLUE && st.old_recompacted_node)
				free_cds_ft_node_unpublished(dst_ft,
					ft_node_ptr(st.dest));
			/*
			 * Reset the per-attempt state before re-descending.
			 *
			 * The NOSPLIT prepare re-fills @st and re-sets
			 * @nosplit_prepared from scratch.
			 *
			 * The run-splice fallback txn (list on) was created
			 * one-shot but never consumed: the run fused into
			 * glue.txn.  Drop it.  A retry stays fused and needs
			 * none.
			 *
			 * SEPARATE swap (@already_swapped): @fresh_node and
			 * @src_retire_txn are the one-shot src state.  The
			 * swap guard holds them across the retry.
			 *
			 * FUSED move: the abort rolled the src-root edge back
			 * too.  So @already_swapped stayed FALSE, and
			 * @fresh_node is this attempt's UNPUBLISHED empty
			 * root.  Free it, exactly as the fallible-prefix
			 * retries do.  The retry re-allocs a fresh one at the
			 * loop top.  Without the free, each aborted round
			 * leaks one arena node.
			 */
			nosplit_prepared = false;
			if (run_splice_txn) {
				ft_flip_txn_destroy(run_splice_txn);
				run_splice_txn = NULL;
			}
			if (!already_swapped)
				free_cds_ft_node_unpublished(src_ft, fresh_node);
			ft_txn_attempt_bail(&optxn, ra_txn);
			goto retry_attach;
		}

		/*
		 * Order-statistics: the +src_count ancestor walk is now
		 * FOLDED onto the attach commit above:
		 *  - glue.count_delta for the GLUE diverge and
		 *    displaced-external shapes;
		 *  - ft_store_at_graft_point's count_delta for the in-place
		 *    / recompact-relocate slot shapes.
		 * So it flips ATOMICALLY with the structural publish, and is
		 * exact under concurrent writers.  There is no post-commit
		 * propagate walk.
		 */

		/*
		 * Ordered list: src is now structurally empty.
		 * The payload is published under @key in dst.
		 *
		 * Every ordered graft shape FUSES the run-splice into its
		 * structural flip (@armed): GLUE, displaced-external and
		 * in-place slot.  So a reader never sees the run in one
		 * index but not the other.
		 *
		 * The standalone two-commit splice remains as a defensive
		 * fallback for any not-yet-fused shape (none today).
		 * Without it, an unfused shape would strand the run out of
		 * the list.  It commits through the pre-reserved
		 * @run_splice_txn (un-abortable post-unlink).  The fused
		 * common case frees that txn unused.
		 */
		if (graft_run_first && !graft_run.armed) {
			ft_ord_cell_run_splice(dst_ft, run_splice_txn,
				graft_run_first, graft_run_last, graft_pred,
				graft_succ);
			run_splice_txn = NULL;	/* consumed */
		}
		if (run_splice_txn)
			ft_flip_txn_destroy(run_splice_txn);	/* fused: unused */

		/*
		 * NIL-key graft succeeded: the external chain head was placed
		 * directly, so the orphaned wrapper internal (the old source root)
		 * is reclaimed.  No reader is inside it: the source is exclusive.
		 */
		if (nil_key_root)
			free_cds_ft_node(src_ft, ft_node_ptr(old_src_root));
		/*
		 * The attach SUCCEEDED and nothing below re-attempts: close the
		 * attempt without aging it.  This is the loop's only fall-out
		 * edge; every other exit closes at its own return / goto.
		 */
		ft_txn_attempt_end(&optxn, ra_txn);
	}

done:
	{
		size_t nm = key_len + src_max;

		if (nm > uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED))
			uatomic_store(&dst_ft->max_used_key_len, nm,
				      CMM_RELAXED);
	}

	uatomic_store(&src_ft->max_used_key_len, 0, CMM_RELAXED);

	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_graft(struct cds_ft *dst_ft,
		const uint8_t *_key, size_t _key_len,
		struct cds_ft *src_ft)
{
	size_t key_len;
	enum cds_ft_status status;
	FT_BW_OP(FT_BW_GRAFT);

	FT_TP_KEY(graft_enter, dst_ft, _key, _key_len);

	if (!dst_ft || !src_ft || dst_ft == src_ft) {
		FT_TP(graft_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_ft->group != src_ft->group) {
		FT_TP(graft_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}

	/*
	 * Root-level graft (key_len == 0) is valid for both
	 * variable-length and fixed-length groups: it swaps the entire
	 * root, so no key-length constraint applies.  Bypass
	 * ft_key_len() which would reject 0 != fixed_len.
	 */
	if (_key_len == 0) {
		key_len = 0;
	} else {
		key_len = ft_key_len(dst_ft, _key_len);
		if (!valid_key_len(dst_ft, key_len) ||
				dst_ft->group->key_len != CDS_FT_LEN_VARIABLE) {
			FT_TP(graft_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
			return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		}
	}

	/*
	 * G5.5: the gate waits a grace period, so no read section may be held
	 * here.
	 */
	CDS_FT_SCOPED_BULK_GATE(dst_ft, FT_BULK_WRITER_ONLY);

	/*
	 * No read section spans the op.  The dst nodes the descent captures
	 * are safe without one:
	 * - the dst writer scope sees the gate raised and takes the FT-wide
	 *   lock, so no peer writer can free them;
	 * - on a FINE trie, each attach attempt runs inside its op handle's
	 *   read section (ft_op_begin), from descent to commit.
	 * The src is exclusive, so its frees are synchronous.
	 */
	status = ft_graft_keylen(dst_ft, _key, key_len, src_ft);
	FT_TP(graft_exit, (int) status);
	return status;
}

#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
extern unsigned long cds_ft_probe_gs_commit_ok;
extern unsigned long cds_ft_probe_gs_reoccupy;
extern unsigned long cds_ft_probe_gs_slot_moved;
extern unsigned long cds_ft_probe_gs_retry;
extern unsigned long cds_ft_probe_gs_exact;
extern unsigned long cds_ft_probe_gs_kshort;
extern unsigned long cds_ft_probe_gs_delegate;
extern unsigned long cds_ft_probe_gs_fused;
extern unsigned long cds_ft_probe_gs_ext_child;
extern unsigned long cds_ft_probe_gs_torn;
extern unsigned long cds_ft_probe_gs_alias;
extern unsigned long cds_ft_probe_gs_canon_alias;
extern unsigned long cds_ft_probe_gs_fuse_pcn;
extern unsigned long cds_ft_probe_gs_fuse_ccn;
extern unsigned long cds_ft_probe_gs_fuse_len;
extern unsigned long cds_ft_probe_gs_fuse_incoh;
extern unsigned long cds_ft_probe_gs_fuse_incoh_committed;
#endif	/* FT_GS_PROBE_INC comes from ft-mutation-helpers.h */

/*
 * Outcome of ft_graft_swap_descend's read-only descent toward the swap key.
 */
enum ft_graft_swap_case {
	FT_GRAFT_SWAP_EXACT,		/* reached key_len at a live subtree (d->nf) */
	FT_GRAFT_SWAP_KEY_SHORTER,	/* key ends strictly inside compressed d->nf */
	FT_GRAFT_SWAP_DELEGATE,		/* diverge / dead-end: no content at key */
};

/*
 * Settle the graft point's PAIR from ONE load of the slot.  The pair is:
 * - @raw_ret: the RAW slot value, which the commit quotes as its
 *   expected-old;
 * - @d->nf: the RESOLVED occupant, which the extract side re-roots into
 *   @swap_ft.
 *
 * ★ ONE LOAD, NOT TWO.  If @d->nf and @raw_ret come from separate loads of
 * the slot, a peer graft_swap can land between them.  The pair then names
 * DIFFERENT objects:
 *
 *   this op                                 peer graft_swap
 *   load the slot: @d->nf = X
 *                                           take X, install Y
 *   load the slot: @raw_ret = Y
 *   commit: ratify displacing Y
 *   extract side: re-root X
 *
 * Both swap tries then own that subtree.  Later the aliased node comes back
 * round to the graft point.  It is then BOTH what this op publishes and what
 * it quotes as expected-old.  That is a no-op replace that reports OK.  After
 * it, the extract side NULLs the parent of a node still wired into dst.
 *
 * The window is wide.  The two loads are observed to disagree hundreds of
 * times per run of inv_graft_swap_shared_dst_nolist.  So this is a routine
 * interleaving, not a corner.  FEATURE_FT_PROBE_GRAFT_SWAP counts it.
 *
 * Re-derive through the SAME primitive ft_node_get_nth_reanchor_slot uses:
 * resolve the flip proxy, then reanchor.  The comparison is then exact, not a
 * skip-encoding artifact.
 *
 * It IS exact because @d->nf always holds the REANCHORED form at both exits.
 * Every producer yields a value that is "never skip-encoded":
 * - ft_descent_init's root;
 * - ft_descent_step;
 * - ft_descent_traverse_compressed.
 * For the same reason, the descent loop's defensive
 * ft_resolve_skip_compressed(@d->nf) is a no-op.  So the KEY_SHORTER exit
 * does NOT compare a one-hop resolve against a reanchor.
 *
 * Keep that invariant.  A future producer that skips the reanchor would turn
 * this guard into a permanent mismatch, i.e. an unbounded re-descend.
 *
 * On disagreement, or on a reanchor level-move, raise @d->skip_conflict and
 * leave @raw_ret NULL.  @d->skip_conflict is the existing "a mutating caller
 * must re-descend" flag (ft-mutation-helpers.h).  A NULL expected-old cannot
 * match a populated slot.  So a caller that somehow skipped the check gets an
 * aborted commit, not a corrupted trie.
 *
 * Returns true when the pair is coherent.
 */
static
bool ft_graft_swap_settle(struct cds_ft *ft, struct ft_descent *d,
		struct cds_ft_inode_flag **raw_ret)
{
	struct cds_ft_inode_flag *raw = *d->nfp;	/* THE one load */
	/*
	 * Resolve ONCE, and hand the SETTLED form out.  The validation below
	 * already resolves.  @raw_ret feeds the forward publish's
	 * expected-old.  An engine proxy is not a value the slot HAS.  It is a
	 * parking marker for a record, and the record's owner writes the real
	 * value at settle.
	 *
	 * Handing the marker out records a bad expected-old:
	 * - either it never matches;
	 * - or (worse, on a validate edge) it matches while the parker is
	 *   still parked.  It then republishes a pointer into a descriptor
	 *   that nobody will ever settle again.
	 *
	 * Resolving keeps the "one load" coherence: both uses come from @raw.
	 * It yields the FT-form value (SKIP_X included) that the CAS must
	 * compare against.
	 */
	struct cds_ft_inode_flag *settled = ft_resolve_flip_proxy(raw);
	unsigned int rewind = 0;

	if (caa_unlikely(ft_reanchor_flag(ft, settled,
			&rewind) != d->nf || rewind != 0)) {
		FT_GS_PROBE_INC(cds_ft_probe_gs_torn);
		d->skip_conflict = true;
		return false;
	}
	*raw_ret = settled;
	return true;
}

/*
 * Descend read-only to the graft point for cds_ft_graft_swap.  Unlike
 * ft_descend_to_graft_point, publish nothing.  A key-shorter or diverging
 * key is reported, never split in place.  So the whole swap can be assembled
 * as a build-invisible transaction.
 *
 * Return one of:
 *
 *   FT_GRAFT_SWAP_EXACT:       d->depth == key_len and d->nf is the existing
 *                              subtree at @key (the displaced old-child).
 *   FT_GRAFT_SWAP_KEY_SHORTER: @key ends inside the compressed node d->nf
 *                              (d->depth is the node's start depth, d->pnf /
 *                              d->nfp hold it).
 *   FT_GRAFT_SWAP_DELEGATE:    the path diverges, dead-ends, or the slot at
 *                              @key is empty -- there is nothing to extract, so
 *                              the swap reduces to an insert (the caller routes
 *                              to the now-atomic cds_ft_graft).
 *
 * @raw_ret (EXACT / KEY_SHORTER): the graft-point slot's RAW content, read here
 * at the descent's own cursor.  The caller quotes it as the forward publish's
 * expected-old.  So one descent decides both the edge that DISPLACES the
 * occupant and which occupant the extract side RE-ROOTS.
 *
 * Re-reading the slot later widens the gap between those two decisions.
 * "Later" means at the caller's plan, or (worse) at record time inside the
 * glue committer.  A peer can swap the graft point in between.  The late read
 * then reports the PEER's node.  The commit ratifies displacing content this
 * op never planned to take.  The extract side still moves the stale occupant
 * into @swap_ft.  Both then own the same subtree.
 *
 * ★ IT MUST BE THE SLOT, NOT @d->nf.  ft_descent_step stores the REANCHORED
 * resolution of the slot (ft_node_get_nth_reanchor_slot) while @d->nfp names
 * the raw slot, so for a SKIP_X occupant the two differ -- and an expected-old
 * that can never match turns the KEY_SHORTER legacy publish, whose commit
 * status is dropped, into a silent no-op that still frees the replaced node.
 *
 * ★ AND IT MUST BE THE SAME LOAD THAT PRODUCED @d->nf.  A second read of the
 * slot here is the gap that the ft_graft_swap_settle exit closes.  The pair
 * is settled from ONE load.  A disagreement raises @d->skip_conflict, so that
 * the caller re-descends.  @raw_ret then stays NULL.
 *
 * A CALLER MUST TEST @d->skip_conflict before trusting @d or @raw_ret.
 * cds_ft_graft_swap does (retry_swap).  Of ft_merge_descend's three callers
 * only the dst descent does (fractal-trie.c:1016); the src descent
 * (ft-merge.h:2794) and the merge-point re-descend (:2883) still ignore it,
 * as they did before this exit existed -- the flag simply raises slightly
 * more often now.  Those two are unaudited, not known-safe.
 */
static
enum ft_graft_swap_case ft_graft_swap_descend(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, struct ft_descent *d,
		struct cds_ft_inode_flag **raw_ret)
{
	const uint8_t *ik = key;

	*raw_ret = NULL;
	ft_descent_init(d, ft);
	for (; d->depth < key_len; ) {
		if (ft_node_external(d->nf))
			return FT_GRAFT_SWAP_DELEGATE;
		d->nf = ft_resolve_skip_compressed(ft, d->nf);
		if (ft_node_compressed(d->nf)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(d->nf);
			int remaining = (int) (key_len - d->depth);
			int cmp = cn->len < remaining ? cn->len : remaining;
			int j = ft_match_compressed_key(ik, cn, cmp);

			if (j < cmp)
				return FT_GRAFT_SWAP_DELEGATE;	/* diverge */
			if (cn->len <= remaining) {
				ft_descent_traverse_compressed(ft, d, cn, &ik);
				continue;
			}
			/*
			 * j == cmp == remaining < cn->len: key ends inside cn.
			 * @cn came from the earlier load, so settle the pair (and
			 * with it this classification) against ONE load of the slot.
			 */
			(void) ft_graft_swap_settle(ft, d, raw_ret);
			return FT_GRAFT_SWAP_KEY_SHORTER;
		}
		if (!ft_descent_step(ft, d, *(ik++)))
			return FT_GRAFT_SWAP_DELEGATE;	/* dead-end */
	}
	if (!d->nf)
		return FT_GRAFT_SWAP_DELEGATE;	/* empty slot at key */
	(void) ft_graft_swap_settle(ft, d, raw_ret);
	return FT_GRAFT_SWAP_EXACT;
}

enum cds_ft_status cds_ft_graft_swap(struct cds_ft *dst_ft,
		const uint8_t *_key, size_t _key_len,
		struct cds_ft *swap_ft)
{
	size_t key_len, swap_max;
	FT_BW_OP(FT_BW_GRAFT_SWAP);

	FT_TP_KEY(graft_swap_enter, dst_ft, _key, _key_len);

	if (!dst_ft || !swap_ft || dst_ft == swap_ft) {
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_ft->group != swap_ft->group) {
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}

	/*
	 * MW LOCK_FINE (step 6, §9.5): @swap_ft is the consumed source of the
	 * exchange.  So like cds_ft_graft it must be EXCLUSIVE:
	 * - An exclusive swap skips its FT-wide lock.  So only dst's lock is
	 *   taken: one lock, no cross-trie deadlock.
	 * - The dual root/subtree swap runs directly and is build-invisible.
	 *   A rejected swap leaves both tries pristine.
	 *
	 * A LIVE (lock-mode, non-exclusive) @swap_ft is REJECTED with BUSY
	 * before anything is touched.  The caller makes it exclusive first
	 * (cds_ft_make_exclusive).  @dst_ft may be a live concurrent trie.
	 * The check applies under every writer strategy.
	 *
	 * On success @swap_ft inherits @dst_ft's access discipline (see the
	 * header).  So it may end up concurrent again.
	 */
	if (!swap_ft->exclusive) {
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_BUSY_ERROR);
		return CDS_FT_STATUS_BUSY_ERROR;
	}

	/* G5.5: before the writer scopes -- the gate must not wait a GP under a lock. */
	CDS_FT_SCOPED_BULK_GATE(dst_ft, FT_BULK_WRITER_ONLY);

	ft_crosstrie_lock_mode_guard(dst_ft, swap_ft);
	CDS_FT_SCOPED_WRITER(dst_ft);
	CDS_FT_SCOPED_WRITER(swap_ft);

	/*
	 * Root-level swap (key_len == 0) is valid for both
	 * variable-length and fixed-length groups.  See cds_ft_graft.
	 */
	if (_key_len == 0) {
		key_len = 0;
	} else {
		key_len = ft_key_len(dst_ft, _key_len);
		if (!valid_key_len(dst_ft, key_len) ||
				dst_ft->group->key_len != CDS_FT_LEN_VARIABLE) {
			FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
			return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
		}
	}

	const struct cds_ft_key_map *km = &dst_ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *key;

	if (caa_likely(km->identity)) {
		key = _key;
	} else {
		ft_key_to_ordinals(ordinal_buf, _key, key_len, km);
		key = ordinal_buf;
	}

	if (ft_moved_keys_overflow(swap_ft, NULL, 0, key_len,
			dst_ft->group->max_key_len)) {
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_OVERFLOW_ERROR);
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	}
	/* After the check: a repair there may have lowered it. */
	swap_max = uatomic_load(&swap_ft->max_used_key_len, CMM_RELAXED);

	if (key_len == 0) {
		/*
		 * Swap entire tries: exchange root pointers.
		 * Each root carries its own metadata (nr_child,
		 * external_nodes), so no relocation is needed.
		 *
		 * ☞ FLIP @dst_ft ONLY, DRAIN, THEN ATTACH @swap_ft (MATHIEU).
		 *
		 *  (1) ONE flip, on @dst_ft alone.  It makes three changes:
		 *      - @dst_ft's root becomes @swap_root;
		 *      - @dst_ft's sentinel points at @swap_ft's run;
		 *      - that run's two outer links point at @dst_ft's
		 *        sentinel.
		 *      The SAME flip is sound because @swap_ft is EXCLUSIVE
		 *      (BUSY at entry otherwise).  No reader of it exists, so
		 *      no straddler can resolve those links to a foreign
		 *      sentinel.  @dst_ft's OLD run keeps its ends on @dst_ft's
		 *      sentinel.  Its straddlers are @dst_ft readers, for whom
		 *      that sentinel IS the end.  @dst_ft is well formed from
		 *      the flip on.
		 *  (2) The drain.  ft_writer_lock_gp_wait drops @dst_ft's writer
		 *      lock for it.  So this is a SEAM that admits point
		 *      writers (G5.25).  What they meet is exactly (1).  It is
		 *      a trie whose root, list and list ends all agree.  A
		 *      writer admitted here may still hold a node of the old
		 *      content.
		 *      It re-derives the node by key from the new root
		 *      (ft_iter_redescend_node) and gets NOT_FOUND.
		 *  (3) Plain stores on the still-EXCLUSIVE @swap_ft.  They set
		 *      its root, the old root's owner stamp, its sentinel and
		 *      the old run's ends.  Until the drain a @dst_ft reader
		 *      may still be inside the old content.  So it keeps naming
		 *      @dst_ft until then.  The order is: detach, drain, THEN
		 *      attach.
		 *
		 * This replaces the cross-trie dual flip.  That flip moved BOTH
		 * runs in one commit.  So it had to NULL-terminate the live
		 * side's outgoing run.  The seam then handed that transient end
		 * to point writers.  inv_graft_swap_whole_seam_points showed
		 * two effects: a SEGV through a NULL neighbour, and a remover
		 * spinning online against the drain.
		 *
		 * Nothing about @swap_ft needs to be atomic with @dst_ft's flip:
		 * nobody can look at @swap_ft until this call returns.
		 */
		struct cds_ft_inode_flag *tmp = dst_ft->root;
		struct cds_ft_inode_flag *swap_root = swap_ft->root;
		struct ft_ord_cell *dh = NULL, *dt = NULL, *sh = NULL, *st = NULL;
		bool gs_ord = dst_ft->group->ordered_list_set;
		size_t dm;
		bool dst_was_exclusive = dst_ft->exclusive;
		struct ft_flip_txn *appear_txn;
		/*
		 * Where @swap_ft's run points its ends in the flip: @swap_ft names
		 * the run's source, relinked to @dst_ft's sentinel.  NULL only
		 * under the red control, which leaves them on @swap_ft's sentinel
		 * so -DFT_DEBUG_SEAM must fire at the seam below.
		 */
		struct cds_ft *relink = FT_SEAM_RELINK(swap_ft);

		assert(swap_ft->exclusive);
		/*
		 * PRE-RESERVE the flip's txn.  It is the only fallible step,
		 * and it is taken while both tries are still pristine.  The txn
		 * is reserved with the list off too.  The root edge then
		 * commits alone, but still through the take.  The take is what
		 * holds &dst_ft->root's lock for it (a live dst).
		 */
		appear_txn = ft_flip_txn_create_bounded(dst_ft,
			FT_ROOT_LIST_SWAP_MAX_EDGES);
		if (!appear_txn) {
			FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}

		/*
		 * @swap_root changes trie: name @dst_ft before the flip publishes
		 * it.  @swap_ft is exclusive, so nothing reads the word before.
		 */
		if (swap_root && !ft_node_flip_proxy(swap_root))
			cds_ft_item_to_metadata(ft_node_ptr(swap_root))
				->parent_word = ft_trie_parent(dst_ft);
		if (gs_ord) {
			dh = ft_ord_first(dst_ft);
			dt = ft_ord_last(dst_ft);
			sh = ft_ord_first(swap_ft);
			st = ft_ord_last(swap_ft);
		}
		/*
		 * (1) Root + sentinel + @swap_ft's run ends, ONE flip.  List off
		 * (all ends NULL), ft_ord_sentinel_edges records nothing and the
		 * root edge is the whole flip.
		 */
		ft_root_list_swap_publish(dst_ft, appear_txn, &dst_ft->root,
			tmp, swap_root, dh, sh, dt, st, relink, true);
		FT_TP(root_publish, (const void *) dst_ft,
			(const void *) dst_ft->root);

		/*
		 * (2) Drain @dst_ft's readers still inside the old content or
		 * straddling its old run.  An exclusive @dst_ft has none.
		 */
		if (!dst_was_exclusive)
			ft_writer_lock_gp_wait(dst_ft);

		/* (3) Attach, plain: @swap_ft is exclusive and nobody reads it. */
		swap_ft->root = tmp;
		if (tmp && !ft_node_flip_proxy(tmp))
			cds_ft_item_to_metadata(ft_node_ptr(tmp))->parent_word =
				ft_trie_parent(swap_ft);
		FT_TP(root_publish, (const void *) swap_ft,
			(const void *) swap_ft->root);
		if (gs_ord) {
			if (!relink)
				ft_ord_finalize_circular(dst_ft);	/* red control */
			if (dh)
				ft_ord_cell_run_install(swap_ft, dh, dt);
			else
				urcu_txn_sw_list_init(&swap_ft->ord_sentinel);
			ft_ord_finalize_circular(swap_ft);
		}

		dm = uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED);
		if (swap_max > dm)
			uatomic_store(&dst_ft->max_used_key_len,
				      swap_max, CMM_RELAXED);
		uatomic_store(&swap_ft->max_used_key_len, dm,
			      CMM_RELAXED);

		/*
		 * swap_ft now holds what was dst_ft's content; inherit
		 * dst_ft's prior access discipline.  dst_ft keeps its
		 * own discipline.
		 */
		swap_ft->exclusive = dst_was_exclusive;

		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

	{
		struct ft_descent d;
		enum ft_graft_swap_case kase;
		struct cds_ft_metadata *pmeta, *swap_rmeta;
		struct cds_ft_inode_flag *old_child, *old_swap_root;
		struct cds_ft_inode *fresh = NULL;
		struct cds_ft_metadata *fresh_meta = NULL;
		struct ft_flip_txn *swap_retire_txn = NULL;
		struct ft_flip_txn *extract_txn = NULL;
		struct ft_flip_txn *glue_publish_txn = NULL;
		struct ft_flip_txn *run_replace_txn = NULL;
		struct ft_glue glue_insert, glue_extract;
		struct cds_ft_inode_flag *canon = NULL;
		struct cds_ft_inode_flag *top_B = NULL;	/* extracted swap root, NULL = external/none */
		struct cds_ft_compressed_node *ks_cn = NULL;	/* key-shorter original cn */
		/*
		 * The RAW graft-point slot value this attempt PLANNED against,
		 * as the DESCENT read it (ft_graft_swap_descend's @raw_ret).
		 * It is the displaced occupant that the extract side is about
		 * to re-root into @swap_ft.
		 *
		 * It becomes the forward publish's expected-old
		 * (ft_glue_set_publish_old).  graft_swap holds @dst_ft's
		 * FT-wide lock from the descent to the commit, so no peer can
		 * swap the graft point in between.  (Under the MCAS engine
		 * such a swap made the commit abort.)
		 *
		 * Raw, not @d.nf: the descent RESOLVES a skip-compressed
		 * occupant, and the expected-old is compared against the slot.
		 */
		struct cds_ft_inode_flag *gs_pub_old = NULL;
		bool swap_empty;
		bool swap_nil_only;	/* swap holds only its zero-length key */
		bool old_child_external = false;
		bool have_insert = false;
		unsigned long old_count = 0, swap_count;
		/* run_D = dst's subtree-at-key heads; run_S = swap's whole list. */
		struct ft_ord_cell *gs_d_first = NULL, *gs_d_last = NULL;
		struct ft_ord_cell *gs_s_first = NULL, *gs_s_last = NULL;
		bool gs_ord = dst_ft->group->ordered_list_set;
		/*
		 * Empty-swap (remove) where the graft point is its parent's SOLE
		 * child.  Publishing NULL would leave the parent a childless
		 * (invalid) node: a compressed node, or a single-child
		 * internal.  Prune it via ft_detach_node.  The detach is
		 * move-style: it preserves the displaced subtree for the
		 * extract side, and frees the parent + the single-child chain.
		 *
		 * The prune can recompact a surviving ancestor.  So the
		 * fallible prep fills a reserve, and the detach draws from it.
		 * This keeps the commit failure-free.
		 */
		struct cds_ft_alloc_reserve gs_reserve;
		bool gs_reserved = false;
		bool empty_pruned = false;
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
		/* What this attempt published into the graft-point slot. */
		struct cds_ft_inode_flag *gs_top = NULL;
		bool gs_fuse_incoh = false;
#endif

		/*
		 * Read-only descent: nothing is published, so the whole swap can be
		 * assembled as a build-invisible transaction and an allocation failure
		 * leaves both tries pristine.
		 */
		/*
		 * The dst nodes this descent captures need no read section of
		 * the op's own.  The dst writer scope holds the FT-wide lock,
		 * so no peer writer can free them.  On a FINE trie each attempt
		 * also runs inside its op handle's read section (ft_op_begin).
		 */
	unsigned long rs_depth __attribute__((unused)) = 0;

	/*
	 * ESCALATION LANE for retry_swap.  Without a persistent handle this loop
	 * ages nothing:
	 * - every attempt commits through its own ft_flip_txn, so
	 *   ft_op_conflict() is never called;
	 * - the op's retry never advances;
	 * - ft_op_earned_lane() is never reached;
	 * - a standalone handle carries no lane anyway.
	 * A contended writer then spins with no termination argument instead
	 * of taking its FIFO turn.
	 *
	 * SCOPED to the contended region, not the whole op.  All four retry
	 * edges are above the dst drain, and the bracket is closed before it.
	 * See the two close sites.
	 *
	 * @optxn is NEVER bound into a commit: they all use standalone
	 * ft_flip_txn_create_bounded.  So optxn->desc stays NULL and
	 * urcu_txn_end() never reaches urcu_txn_destroy().  An earlier attempt
	 * at this pattern was reverted for a double free.  That double free is
	 * designed out here.  The invariant is the grep "optxn appears only
	 * in init/begin/end/bail".
	 *
	 * @gs_open is a VARIABLE, not a re-test of @gs_bracket at each exit.
	 * A re-evaluated condition is a second chance to disagree with the
	 * entry.
	 */
	const bool gs_bracket = dst_ft->lock_fine && swap_ft->exclusive;
	struct ft_op optxn;
	bool gs_open = false;

	if (gs_bracket)
		ft_txn_op_init(dst_ft, &optxn);
retry_swap:
	if (gs_bracket) {
		ft_op_begin(&optxn);
		gs_open = true;
	}
	/*
	 * ASK @gs_bracket, not the expression it was built from.  The bracket
	 * below keys on the const bool so that the condition is evaluated ONCE
	 * (see @gs_open's comment above).  A probe that re-derives it is a
	 * second chance to disagree with the arm it claims to be measuring.
	 */
	RSPIN_ENTER_X(4, rs_depth, 2, gs_bracket);
	RSPIN_SITE_ENTER(2, rs_depth, gs_bracket);
		/*
		 * MW LOCK_FINE drop: the re-descend point.  graft_swap's commit
		 * is failure-free under the FT-wide lock.  With the lock
		 * dropped, a peer can relocate the contended dst spine between
		 * this op's descent and its commit.  The empty-swap prune's
		 * ft_detach_node then cannot lock the recompacting ancestor and
		 * returns -EAGAIN.  That failure is build-invisible: nothing
		 * published, dst byte-for-byte as before.
		 *
		 * Re-plan against the settled tree.  This mirrors
		 * ft_graft_keylen's retry_attach and
		 * ft_merge_graft_subpos_inplace's retry_merge.
		 *
		 * Reset every per-attempt build-product handle: the declaration
		 * initializers ran once, and a goto here does not re-run them.
		 * The re-descent runs inside the next attempt's read section.
		 */
		fresh = NULL;
		fresh_meta = NULL;
		swap_retire_txn = NULL;
		extract_txn = NULL;
		glue_publish_txn = NULL;
		run_replace_txn = NULL;
		canon = NULL;
		top_B = NULL;
		ks_cn = NULL;
		gs_pub_old = NULL;
		old_child_external = false;
		have_insert = false;
		empty_pruned = false;
		gs_reserved = false;
		gs_d_first = gs_d_last = gs_s_first = gs_s_last = NULL;
		kase = ft_graft_swap_descend(dst_ft, key, key_len, &d,
				&gs_pub_old);
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
		gs_top = NULL;
		gs_fuse_incoh = false;
		if (kase == FT_GRAFT_SWAP_EXACT)
			FT_GS_PROBE_INC(cds_ft_probe_gs_exact);
		else if (kase == FT_GRAFT_SWAP_KEY_SHORTER)
			FT_GS_PROBE_INC(cds_ft_probe_gs_kshort);
		else
			FT_GS_PROBE_INC(cds_ft_probe_gs_delegate);
#endif
		/*
		 * The descent could not settle a coherent (raw slot, resolved
		 * occupant) pair.  Either a peer moved the graft point under
		 * it, or a chain-merge reanchored the captured slot to another
		 * level.  ft_graft_swap_settle / ft_descent_step both raise
		 * this.
		 *
		 * A pure read-only descent has published and allocated NOTHING
		 * at this point.  So re-plan against the settled tree.
		 * cds_ft_graft takes the same bail (retry_attach), and so does
		 * insert (ft-insert.h skip_conflict).  cds_ft_graft_swap had
		 * never honoured it before.
		 */
		if (caa_unlikely(d.skip_conflict)) {
			FT_GS_PROBE_INC(cds_ft_probe_gs_retry);
			ft_txn_attempt_bail(&optxn, gs_open);
			gs_open = false;
			goto retry_swap;
		}
		if (kase == FT_GRAFT_SWAP_DELEGATE) {
			/*
			 * No content at @key: the swap reduces to inserting
			 * swap_ft's content at @key, which empties swap_ft.
			 * cds_ft_graft is itself a build-invisible transaction
			 * and empties the source.
			 *
			 * Pass the ORIGINAL application key.  cds_ft_graft
			 * applies the key map itself.  On a non-identity
			 * group, the already-remapped @key would be remapped
			 * twice: wrong graft point, wrong splice position.
			 *
			 * Close the attempt first: it holds a read-side section.
			 * cds_ft_graft's nested gate may wait for a peer gate's
			 * grace period (a rekey's move gate), and that grace
			 * period would wait for this section.  The delegate uses
			 * no node this descent captured.
			 */
			enum cds_ft_status s;

			ft_txn_attempt_end(&optxn, gs_open);
			gs_open = false;
			s = cds_ft_graft(dst_ft, _key, _key_len, swap_ft);
			FT_TP(graft_swap_exit, (int) s);
			return s;
		}

		old_swap_root = swap_ft->root;
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
		if (old_swap_root && old_swap_root == gs_pub_old)
			FT_GS_PROBE_INC(cds_ft_probe_gs_alias);
#endif
		swap_rmeta = ft_root_metadata(swap_ft);
		swap_empty = (ft_meta_nr_child(swap_rmeta) == 0 && !swap_rmeta->external_nodes);
		swap_nil_only = (ft_meta_nr_child(swap_rmeta) == 0 && swap_rmeta->external_nodes);
		swap_count = swap_empty ? 0 : ft_nr_keys_get(swap_rmeta);

		/*
		 * Identify the displaced old-child and its key count.  KEY_SHORTER: the
		 * extracted subtree is everything below the prefix, i.e. the suffix of
		 * the compressed node d.nf (its whole subtree count).  EXACT: d.nf is
		 * the displaced node.
		 */
		if (kase == FT_GRAFT_SWAP_KEY_SHORTER) {
			ks_cn = ft_compressed_node_ptr(d.nf);
			old_count = ft_nr_keys_get(
				cds_ft_item_to_metadata((struct cds_ft_inode *) ks_cn));
			old_child = ks_cn->child;
		} else {	/* FT_GRAFT_SWAP_EXACT */
			old_child = d.nf;
			if (!ft_node_external(old_child))
				old_count = ft_nr_keys_get(
					cds_ft_item_to_metadata(ft_node_ptr(old_child)));
			else
				old_count = 1;	/* one key (possibly a dup chain) */
			/*
			 * Skip-encoded externals carry a compressed prefix; treat them as
			 * non-external so the prefix is materialized into swap_ft's root.
			 */
			old_child_external = ft_node_external(old_child) &&
				!ft_node_skip_compressed(old_child);
			if (old_child_external)
				FT_GS_PROBE_INC(cds_ft_probe_gs_ext_child);
		}

		ft_glue_init(&glue_insert);
		glue_insert.op = &optxn;
		ft_glue_init(&glue_extract);
		glue_extract.op = &optxn;
		/*
		 * @d is the DST graft-point descent, so it dates the insert glue's
		 * publish parent (that glue's whole cluster hangs off the graft
		 * point).  The extract glue works the SWAP trie, which this descent
		 * does not describe, so it gets none -- see its own commit.
		 */
		glue_insert.lock_d = &d;

		/* ===== PREP: build clusters A and B (both tries pristine) ===== */

		/*
		 * Empty-swap remove: the COMMIT routes it through
		 * ft_detach_node.  The detach does the correct parent
		 * bookkeeping for every shape: sole-child prune, multi-child
		 * in-place delete, recompaction.
		 *
		 * Secure a node reserve here for any recompaction the detach
		 * may do.  This step is build-invisible: an OOM here leaves
		 * both tries pristine.  The commit detach then cannot fail.
		 */
		if (swap_empty) {
			memset(&gs_reserve, 0, sizeof(gs_reserve));
			if (ft_bulk_node_reserve_fill(dst_ft, &gs_reserve)) {
				cds_ft_alloc_reserve_drain(dst_ft, &gs_reserve);
				goto prep_oom;
			}
			gs_reserved = true;
		}

		/*
		 * Insert side (cluster A): canonicalized swap content, placed at the
		 * graft point in dst.  Empty swap inserts nothing (a remove).
		 */
		if (swap_nil_only) {
			/*
			 * The swap content is one key, held by a childless root.
			 * Such a node is only legal AT a root.  Placed below the
			 * dst root it is a childless internal node where a leaf
			 * belongs: with a @key as long as the group allows, the
			 * next descent walks past the maximum key length.  Place
			 * the key's chain head itself, as cds_ft_graft does.  The
			 * emptied root is retired once swap_ft lets go of it.
			 */
			canon = (struct cds_ft_inode_flag *) ft_dereference_external(
				swap_rmeta->external_nodes);
		} else if (!swap_empty) {
			canon = ft_compress_single_child_if_needed(dst_ft,
				old_swap_root, &glue_insert);
			if (canon == (struct cds_ft_inode_flag *) (long) -ENOMEM)
				goto prep_oom;
		}

#ifdef FEATURE_FT_SKIP_COMPRESSED
		{
			/*
			 * EXACT + compressed parent + compressed canon.  The
			 * slot already sits under a compressed node.  Placing
			 * another compressed there would violate "no two
			 * adjacent compresseds".  Fuse them into one compressed
			 * at the grandparent slot.
			 *
			 * This is build-invisible:
			 * - the merged cn's live child (canon's grandchild) is
			 *   deferred;
			 * - @canon (a fresh absorbed wrapper) is freed now;
			 * - the live parent cn is reclaimed at commit.
			 */
			struct cds_ft_compressed_node *pcn = NULL, *ccn = NULL;

			if (!swap_empty && kase == FT_GRAFT_SWAP_EXACT) {
				if (ft_node_skip_compressed(d.pnf))
					pcn = ft_skip_to_compressed(dst_ft, d.pnf);
				else if (ft_node_compressed(d.pnf))
					pcn = ft_compressed_node_ptr(d.pnf);
				if (ft_node_compressed(canon))
					ccn = ft_compressed_node_ptr(canon);
			}
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
			if (!swap_empty && kase == FT_GRAFT_SWAP_EXACT) {
				if (pcn) FT_GS_PROBE_INC(cds_ft_probe_gs_fuse_pcn);
				if (ccn) FT_GS_PROBE_INC(cds_ft_probe_gs_fuse_ccn);
				if (pcn && ccn && (unsigned int) pcn->len + ccn->len
						<= FT_CN_LEN_MAX)
					FT_GS_PROBE_INC(cds_ft_probe_gs_fuse_len);
			}
#endif
			/*
			 * Fuse up to what one compressed node can spell, not up
			 * to a skip pointer's run.  The publish below takes a
			 * plain compressed flag for a longer run.  And NOT
			 * fusing leaves @ccn hung directly under @pcn: two
			 * adjacent compresseds.
			 */
			if (pcn && ccn &&
			    (unsigned int) pcn->len + ccn->len <= FT_CN_LEN_MAX) {
				struct cds_ft_metadata *pcn_meta =
					cds_ft_item_to_metadata((struct cds_ft_inode *) pcn);
				unsigned int merged_len = pcn->len + ccn->len;
				struct cds_ft_compressed_node *merged;
				struct cds_ft_metadata *merged_meta;
				struct cds_ft_inode_flag *merged_flag, *merged_skip;
				struct cds_ft_inode_flag **pub_slot;
				struct cds_ft_inode_flag *pub_parent, *pub_old;

				merged = alloc_compressed_node(dst_ft, merged_len,
						&merged_meta);
				if (!merged)
					goto prep_oom;
				memcpy(merged->key_bytes, pcn->key_bytes, pcn->len);
				memcpy(&merged->key_bytes[pcn->len], ccn->key_bytes,
					ccn->len);
				merged->len = (uint8_t) merged_len;
				merged->child = ccn->child;	/* live swap grandchild */
				ft_meta_nr_child_set(merged_meta, 1);
				/*
				 * @merged now roots the swap content (child == ccn->child), so
				 * it carries @swap_count -- baked build-invisible.  The dst NET
				 * delta (swap_count - old_count) folds onto @glue_insert's
				 * publish txn from @pub_parent (the grandparent) below.
				 */
				ft_nr_keys_store(swap_ft, merged_meta,
					swap_count, CMM_RELAXED);
				/*
				 * Resolve pcn's grandparent slot COHERENTLY:
				 * parent + offset from one snapshot.  Do not
				 * read pcn_meta->parent raw and fetch the
				 * slot separately.  Between those two reads,
				 * a peer can recompact pcn's grandparent to
				 * a larger node type, or Phase-4.3 can
				 * re-home pcn.  pub_parent would then name
				 * the OLD grandparent while pub_slot indexes
				 * the NEW body.  So ft_set_parent_slot ->
				 * ft_slot_to_byte would index the wrong body
				 * out of bounds.  It would bake a wild
				 * offset into the build-invisible @merged.
				 *
				 * The forward publish below
				 * (ft_glue_set_publish) reuses the same
				 * coherent (pub_parent, pub_slot).  The
				 * commit guard_parent ratifies pub_parent at
				 * the flip.
				 */
				pub_slot = ft_resolve_parent_slot(pcn_meta, dst_ft,
					&pub_parent);
				/*
				 * The plan value for THIS slot (the
				 * grandparent's, not @d.nfp's).  @merged was
				 * built from @pcn's bytes, read a few lines
				 * up.  So the occupant read here is the
				 * world the fuse planned against.
				 */
				/* SETTLED, not raw: a parked engine proxy is not a
				 * value this slot has (see ft_graft_swap_settle). */
				pub_old = ft_resolve_flip_proxy(*pub_slot);
				/*
				 * ★ THE EXPECTED-OLD CANNOT ARBITRATE THIS ON ITS OWN.
				 * @merged encodes @pcn's bytes and length, read above;
				 * @pub_old is a LATER, independent read of the
				 * grandparent slot.  A peer that replaces
				 * @pcn between the two leaves them
				 * describing different worlds.  The commit
				 * still SUCCEEDS: @pub_old was read from the
				 * peer's world, so it matches the slot.
				 * What lands is @merged, encoding bytes that
				 * are already stale, over the peer's node.
				 *
				 * So settle the pair here: the slot must
				 * still denote the @pcn the fuse planned
				 * against.  If it does not, drop this
				 * attempt and re-descend.  Nothing is
				 * published yet: @merged is untracked and
				 * unpublished, and both glues hold only
				 * build-invisible state.
				 * ft_graft_swap_settle takes the same bail
				 * for the graft point itself.
				 */
				if (caa_unlikely(ft_resolve_skip_compressed(dst_ft,
						pub_old)
						!= ft_compressed_node_flag(pcn))) {
					FT_GS_PROBE_INC(cds_ft_probe_gs_fuse_incoh);
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
					gs_fuse_incoh = true;
#endif
					free_compressed_node_unpublished(dst_ft, merged);
					ft_glue_abort(dst_ft, &glue_insert);
					ft_glue_abort(swap_ft, &glue_extract);
					FT_GS_PROBE_INC(cds_ft_probe_gs_retry);
					ft_txn_attempt_bail(&optxn, gs_open);
					gs_open = false;
					goto retry_swap;
				}
				merged_meta->parent_word = ft_parent_word(dst_ft, pub_parent);
				ft_set_parent_slot(merged_meta, pub_parent, pub_slot);
				merged_flag = ft_compressed_node_flag(merged);
				ft_glue_track(&glue_insert, merged_flag);
				ft_glue_defer_edge(dst_ft, &glue_insert, ccn->child,
					merged_flag, &merged->child);
				/*
				 * @canon is the fresh wrapper just absorbed:
				 * drop it from tracking and free it.  Its
				 * deferred child edge is superseded by the
				 * one above, via the defer-edge de-dup on
				 * @child.
				 */
				ft_glue_untrack(dst_ft, &glue_insert, ccn);
				free_compressed_node_unpublished(dst_ft, ccn);
				merged_skip = ft_publish_compressed(dst_ft, merged,
						merged_flag);
				ft_glue_set_publish_old(dst_ft, &glue_insert,
					pub_parent, pub_slot, merged_skip, pub_old);
				ft_glue_defer_free(&glue_insert, pcn, true);
				d.pnf = merged_flag;	/* structural edits target @merged */
				have_insert = true;
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
				gs_top = merged_skip;
				FT_GS_PROBE_INC(cds_ft_probe_gs_fused);
#endif
			}
		}
#endif /* FEATURE_FT_SKIP_COMPRESSED */

		if (!swap_empty && !have_insert) {
			struct cds_ft_inode_flag *top_A;

			if (kase == FT_GRAFT_SWAP_KEY_SHORTER) {
				/*
				 * Replace the whole compressed node with a fresh prefix
				 * [d.depth, key_len) wrapping @canon (the chain-merge folds the
				 * prefix bytes into @canon when it is compressed).  d.pnf is the
				 * cn's parent (never compressed), so no grandparent fuse.
				 */
				top_A = ft_build_branch(dst_ft, key, d.depth, key_len,
						canon, swap_count, false, &glue_insert);
				if (!top_A)
					goto prep_oom;
				ft_glue_defer_free(&glue_insert, ks_cn, true);
			} else {
				/* EXACT, simple replace of d.nf at d.nfp by @canon. */
				top_A = canon;
			}
			/*
			 * A compressed cluster top installs as the SKIP form in
			 * the live parent slot.  Its child's back-pointer is
			 * deferred.  So the skip only resolves once
			 * ft_glue_apply_deferred has run.  At commit, it runs
			 * before the forward publish.  The set_publish
			 * deferred edge (top -> d.pnf) is recorded LAST.  So
			 * the child back-pointer is already in place by the
			 * time that edge is applied.
			 */
			if (ft_node_compressed(top_A))
				top_A = ft_publish_compressed(dst_ft,
					ft_compressed_node_ptr(top_A), top_A);
			ft_glue_set_publish_old(dst_ft, &glue_insert, d.pnf, d.nfp,
				top_A, gs_pub_old);
			have_insert = true;
#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
			gs_top = top_A;
			if (top_A == gs_pub_old)
				FT_GS_PROBE_INC(cds_ft_probe_gs_canon_alias);
#endif
		}

		/*
		 * Extract side (cluster B): materialize the displaced subtree as
		 * swap_ft's new root.  KEY_SHORTER builds the root from the suffix path
		 * + live grandchild; EXACT runs the build-invisible make_root_internal
		 * on the displaced node.  External (or absent) content attaches as
		 * external_nodes at commit instead (no build).
		 */
		if (kase == FT_GRAFT_SWAP_KEY_SHORTER) {
			unsigned int prefix_len = (unsigned int) (key_len - d.depth);

			top_B = ft_build_extracted_root_glue(swap_ft, &glue_extract,
				ks_cn->key_bytes[prefix_len],
				&ks_cn->key_bytes[prefix_len + 1],
				ks_cn->len - prefix_len - 1,
				ks_cn->child, old_count);
			if (top_B == (struct cds_ft_inode_flag *) (long) -ENOMEM)
				goto prep_oom;
		} else if (!old_child_external && old_child) {
			top_B = ft_make_root_internal_glue(swap_ft, &glue_extract,
					old_child, 0);
			if (top_B == (struct cds_ft_inode_flag *) (long) -ENOMEM)
				goto prep_oom;
		}

		/*
		 * Insert side: commit the replace through a flip-txn, following
		 * the bulk-op rule:
		 * - a pointer NOT reader-observable during the commit window is
		 *   set immediately with a plain store;
		 * - only a LIVE publish rides the txn.
		 *
		 * Here the whole inserted cluster is the swap content,
		 * unlinked + drained below, so all its back-pointers are hidden:
		 * ft_glue_txn_commit_replace sets them immediately (ft_glue_apply_
		 * deferred) and rides only the forward replace edge into dst + the <=4
		 * run-replace cell edges on the txn, so structure and ordered list flip
		 * together in ONE selector flip (closing the cross-view window).
		 *
		 * Reserve generous headroom up front, so that records can't
		 * fail mid-commit.  The headroom is forward 1-2 stores + <=4
		 * cell edges, plus the deferred floor.  The txn is created
		 * before the failure-free section, so an OOM here is still a
		 * clean prep_oom.
		 *
		 * The extract side stays on plain rcu_assign: it
		 * publishes into the drained swap_ft where no reader is present.
		 *
		 * NOT for KEY_SHORTER (the swap key ends INSIDE a compressed
		 * node).  That shape wraps a LIVE dst compressed node.  The
		 * re-parent of that node is a live pointer not yet classified
		 * for the txn.  So its EDGE commit stays on the legacy
		 * apply-deferred + publish-replace path (glue_insert.txn ==
		 * NULL selects it) until that wrap re-parent is folded in.  Its
		 * free-list FREEZE still fuses, into glue_publish_txn.  That is
		 * wired up in the commit below (doc §4.B).
		 */
		if (have_insert && kase != FT_GRAFT_SWAP_KEY_SHORTER) {
			glue_insert.txn = ft_flip_txn_create(dst_ft);
			if (!glue_insert.txn || !ft_flip_txn_reserve(glue_insert.txn,
					/* +1: fused recompact-relocate tombstone (§4.B);
					 * + FLOOR_FREE: fused free-list tombstones */
					FT_GLUE_FLOOR_DEFERRED + 7 + 1 /* +1 §4.B parent guard */ + FT_GLUE_FLOOR_FREE
					+ (dst_ft->rank_stats ? (int) key_len + 1 : 0) /* nr_keys fold walk */))
				goto prep_oom;
			glue_insert.fuse_free_list = true;
		} else if (have_insert) {
			/*
			 * KEY_SHORTER legacy publish.  glue_insert.txn stays
			 * NULL here, which selects ft_glue_apply_deferred +
			 * ft_glue_publish_replace below.
			 *
			 * The publish-replace runs in the failure-free section.
			 * So pre-reserve its commit txn here, where an OOM is
			 * still a clean prep_oom.  The txn covers the
			 * forward(+SKIP_X dual) + run-replace.  It is always
			 * consumed on the legacy path: the reservation
			 * condition mirrors the consume condition.  A later
			 * prep_oom frees it.
			 *
			 * The +FT_GLUE_FLOOR_FREE headroom lets the free-list
			 * FREEZE fuse into this same flip (doc §4.B atomic
			 * detach).  glue_insert's retired-node tombstones ride
			 * the publish-replace flip that unlinks them (wired up
			 * in the commit below).  They do not take a standalone
			 * lone-edge flip.  Only the free-list freeze fuses.
			 * The live wrap re-parent stays on the immediate
			 * apply_deferred path (not yet txn-classified).
			 */
			glue_publish_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_GLUE_PUBLISH_REPLACE_MAX_EDGES + FT_GLUE_FLOOR_FREE
				+ 1 /* +1 §4.B parent guard */
				+ (dst_ft->rank_stats ? (int) key_len + 1 : 0) /* nr_keys fold walk */);
			if (!glue_publish_txn)
				goto prep_oom;
		}

		/* Transient empty swap root for the unlink window (fallible). */
		if (!swap_empty) {
			fresh = alloc_cds_ft_node(swap_ft, &ft_types[0], &fresh_meta);
			if (!fresh)
				goto prep_oom;
			/* Fresh root for @swap_ft: named while still invisible. */
			fresh_meta->parent_word = ft_trie_parent(swap_ft);
			/*
			 * Pre-reserve the swap-root retire's txn -- a fallible step
			 * before the failure-free commit.  The retire below runs past
			 * the COMMIT marker and cannot abort, so it commits through this
			 * pre-reserved txn (ft_ord_cell_flip_into).  Reserved only when
			 * the list is on (list off retires via the lone-edge
			 * ft_root_edge_flip).  OOM here is a clean pre-commit abort; a
			 * later prep_oom (e.g. the extract-txn reserve below failing)
			 * frees it.
			 */
			if (gs_ord) {
				swap_retire_txn = ft_flip_txn_create_bounded(swap_ft,
					FT_ROOT_LIST_SWAP_MAX_EDGES);
				if (!swap_retire_txn)
					goto prep_oom;
			}
		}

		/*
		 * Pre-reserve the extract-side run_D install txn (the post-drain
		 * fused root-install + run_D head/tail flip far below): it commits
		 * after both syncs, in the failure-free section, so it cannot abort.
		 * Reserved only when the list is on -- list off makes that flip at
		 * most the lone top_B root-install edge (an infallible on-stack
		 * store).  Plus the standalone dst run-replace fallback txn (the
		 * gs_reserved sole-child detach-prune path, where the publish leaves
		 * swap_run unarmed; every other shape fuses the replace and frees it
		 * unused).  These are the last fallible steps before the COMMIT marker.
		 */
		/*
		 * extract_txn carries the swap-root install:
		 * - list on: the full pre-reserved txn.  It holds the root
		 *   install, run_D head/tail, the fused top_B transient-root
		 *   tombstone and the fused free-list tombstone.
		 * - list off with a top_B retire: a 3-edge txn rather than a
		 *   lone store.  It holds the root install, the top_B tombstone
		 *   and the free-list tombstone (atomic detach §4.B).
		 * - !gs_ord && !top_B: it keeps its lone external attach store.
		 * One shared goto keeps this block's abort footprint unchanged.
		 *
		 * Fuse the extract-side glue free-list retire into extract_txn.
		 * ft_make_root_internal_glue defers at most one node: the
		 * peeled compressed old_child.  That node freezes dead
		 * atomically with the root install that unlinks it (atomic
		 * detach, §4.B).  It
		 * does not take a standalone flip in ft_glue_apply_deferred
		 * below.  The +1 headroom in each arm covers it.
		 *
		 * A non-empty free_list means a compressed old_child.  Hence
		 * top_B is non-NULL, and extract_txn is this reserved txn.  It
		 * is never the !gs_ord && !top_B NULL case.  In that case the
		 * free_list is empty and ft_glue_tombstone_free_list's loop is
		 * skipped (no NULL-txn deref).
		 */
		if (gs_ord || top_B) {
			extract_txn = ft_flip_txn_create_bounded(swap_ft, gs_ord ?
				FT_ROOT_LIST_SWAP_MAX_EDGES + 2 : 3);
			if (!extract_txn)
				goto prep_oom;
			glue_extract.txn = extract_txn;
			glue_extract.fuse_free_list = true;
			if (gs_ord) {
				run_replace_txn = ft_flip_txn_create_bounded(dst_ft,
					FT_ORD_CELL_RUN_REPLACE_MAX_EDGES);
				if (!run_replace_txn)
					goto prep_oom;
			}
		}

		/* ===== COMMIT (failure-free) ===== */

		/*
		 * Ordered list: capture both runs while both lists are intact.
		 * run_D = dst's subtree-at-key (old_child's heads), which becomes
		 * swap_ft's whole list; run_S = swap_ft's whole list, which replaces
		 * run_D in dst.  The mutations land at the matching structural
		 * sub-points below so the existing per-side syncs drain each side.
		 */
		if (gs_ord) {
			gs_d_first = ft_ord_cell_ptr(ft_dereference_prev_resolved(
				ft_subtree_minmax_head(dst_ft, old_child, false)));
			gs_d_last = ft_ord_cell_ptr(ft_dereference_prev_resolved(
				ft_subtree_minmax_head(dst_ft, old_child, true)));
			gs_s_first = ft_ord_first(swap_ft);	/* NULL if swap empty */
			gs_s_last = ft_ord_last(swap_ft);
		}
		/*
		 * Fuse the run_D->run_S ordered-list replace into the SAME flip
		 * as the structural publish below.  A reader then never sees
		 * run_S present in the structure at @key while the ordered list
		 * still shows run_D (or the reverse).
		 *
		 * It is armed by the publish shapes that record their forward
		 * edge: have_insert glue / empty-swap publish-NULL.  The
		 * sole-child detach-prune (gs_reserved) leaves it unarmed.  It
		 * falls back to the standalone two-commit run-replace below.
		 */
		struct ft_graft_swap_run swap_run = {
			gs_d_first, gs_d_last, gs_s_first, gs_s_last, false
		};
		struct ft_graft_swap_run *swap_run_arg = gs_ord ? &swap_run : NULL;

		/*
		 * Insert side: wire the deferred live back-pointers, then the single
		 * forward publish that splices cluster A into dst (detaching the old
		 * content).  Empty swap publishes NULL (a remove).
		 *
		 * ★ THE DST ATTACH IS THE POINT OF NO RETURN, AND IT RUNS
		 * FIRST.  The swap-root retire used to precede it, on a "jump
		 * out" argument.  That argument required draining swap's
		 * readers between the unlink and the re-parent.  That drain has
		 * been !exclusive-gated ever since a LIVE @swap_ft became a
		 * BUSY rejection at entry, i.e. it is dead code.  The consumed
		 * source is ALWAYS exclusive here.  So it has no readers to
		 * jump out and no ordering to honour.
		 *
		 * What the old order did cost was the only thing that mattered.
		 * A commit that ABORTS (a peer touched the contended dst spine)
		 * left swap already emptied.  So the abort was unrecoverable
		 * and its status was DROPPED.  Retiring AFTER the attach makes
		 * every abort build-invisible on both sides.  That is what lets
		 * the status be honoured with a plain re-descend.
		 */
		if (have_insert) {
			enum urcu_txn_status ins_cst = URCU_TXN_STATUS_OK;

			/*
			 * Order-statistics (BULK): the replace swaps @old_count
			 * keys for @swap_count.  So the dst NET delta rides the
			 * replace publish instead of a post-commit walk.
			 * ft_glue_txn_commit_edges / ft_glue_publish_replace
			 * record it from @glue_insert.publish_parent.
			 */
			glue_insert.count_delta = (long) swap_count - (long) old_count;
			if (glue_insert.txn)
				ins_cst = ft_glue_txn_commit_replace(dst_ft,
					&glue_insert, swap_run_arg);
			else {
				/*
				 * Fuse glue_insert's free-list FREEZE into
				 * the publish-replace flip (doc §4.B atomic
				 * detach).  Point glue_insert at
				 * glue_publish_txn HERE.  This is AFTER the
				 * glue_insert.txn dispatch above already
				 * chose this legacy branch.  Then:
				 * - ft_glue_apply_deferred records the
				 *   retired-node tombstones into it;
				 * - ft_glue_publish_replace commits them
				 *   with the forward replace that unlinks
				 *   them.
				 * apply_deferred still applies the live wrap
				 * re-parent immediately (a hidden, drained
				 * back-edge).  Only the freeze rides the
				 * txn.
				 */
				glue_insert.txn = glue_publish_txn;
				glue_insert.fuse_free_list = true;
				ft_glue_apply_deferred(dst_ft, &glue_insert);
				/*
				 * ★ HONOUR THIS STATUS.  This is the same
				 * point of no return as the fused arm above.
				 * It aborts for the same reason: a peer took
				 * the graft point.  Ignoring the status
				 * would:
				 * - retire @swap_ft's root;
				 * - re-root the displaced subtree into it;
				 * - FREE the dst node the publish never
				 *   replaced.
				 * That leaves a live dst grandchild whose
				 * parent points into the OTHER trie.
				 *
				 * Route it into the same unwind.  The abort
				 * is build-invisible on both sides:
				 * - dst: nothing published;
				 * - @swap_ft: it still holds its content,
				 *   and apply_deferred rewrote only its own
				 *   exclusive interior.
				 * So a plain re-descend is clean.
				 */
				ins_cst = ft_glue_publish_replace(dst_ft,
					glue_publish_txn, &glue_insert,
					swap_run_arg);
				glue_publish_txn = NULL;	/* consumed */
				glue_insert.txn = NULL;	/* the commit reclaimed it */
			}
			/*
			 * MW LOCK_FINE drop: a peer touched the contended dst
			 * spine between this op's descent and its replace
			 * commit.  Either:
			 * - it relocated @publish_parent.
			 *   ft_flip_txn_guard_parent's clean-live expectation
			 *   then fails against the retired/re-homed parent
			 *   word;
			 * - or it swapped the graft point out from under us.
			 *   The forward record's expected-old then mismatches.
			 *
			 * A flip-txn commit is all-or-none.  The swap-root
			 * retire is now BELOW this point.  So NOTHING has been
			 * published on either side: dst is byte-for-byte as
			 * before, and @swap_ft still holds its full content.
			 *
			 * The replace's apply_deferred did already rewrite
			 * swap's interior parent back-pointers.  These are
			 * immediate stores, not rolled back.  They are
			 * invisible:
			 * - swap is exclusive;
			 * - the content nodes are never freed;
			 * - the rebuild reads forward-only and overwrites them.
			 *
			 * Free this attempt's build-invisible clusters +
			 * reserve + txns, then re-descend.  This mirrors
			 * ft_graft_keylen's store-abort retry_attach.
			 *
			 * ★ EVERY SHAPE MUST HONOUR @ins_cst, not just the
			 * fused arm.  Two graft_swaps at ONE dst position can
			 * both reach here with only one attach landed.  A
			 * loser that ignores the status still runs its extract
			 * side.  It re-roots into @swap_ft a subtree that the
			 * abort left wired into dst.  It NULLs that subtree's
			 * parent back-pointer where it still hangs at depth 1
			 * of the destination.  inv_graft_swap_shared_dst
			 * reports this as "parent mismatch: got (nil)".
			 */
			if (ins_cst != URCU_TXN_STATUS_OK) {
				ft_glue_abort(dst_ft, &glue_insert);
				ft_glue_abort(swap_ft, &glue_extract);
				if (fresh) {
					/*
					 * UNPUBLISHED.  The retire that would
					 * have installed @fresh as swap's
					 * root is below this bail.  So @fresh
					 * never became reader-visible and
					 * carries no tombstone.  The audit
					 * build's freeze-on-free assert is
					 * exactly right to refuse it through
					 * the published path.
					 */
					free_cds_ft_node_unpublished(swap_ft, fresh);
					fresh = NULL;
				}
				if (swap_retire_txn) {
					ft_flip_txn_destroy(swap_retire_txn);
					swap_retire_txn = NULL;
				}
				if (extract_txn) {
					ft_flip_txn_destroy(extract_txn);
					extract_txn = NULL;
				}
				if (run_replace_txn) {
					ft_flip_txn_destroy(run_replace_txn);
					run_replace_txn = NULL;
				}
				if (gs_reserved) {
					cds_ft_alloc_reserve_drain(dst_ft, &gs_reserve);
					gs_reserved = false;
				}
				FT_GS_PROBE_INC(cds_ft_probe_gs_retry);
				ft_txn_attempt_bail(&optxn, gs_open);
				gs_open = false;
				goto retry_swap;
			}

#ifdef FEATURE_FT_PROBE_GRAFT_SWAP
			/*
			 * B4 residual probe.  Our commit reported OK.  So the
			 * graft point must no longer hold @gs_pub_old.  That
			 * is the occupant the extract side below is about to
			 * re-root into @swap_ft, and whose parent back-pointer
			 * it NULLs.  Re-descend and check.  This is still
			 * inside the §11 read section.  So nothing read here
			 * can have been reclaimed out from under the
			 * comparison.
			 */
			{
				struct ft_descent d2;
				struct cds_ft_inode_flag *raw2 = NULL;
				enum ft_graft_swap_case k2;

				FT_GS_PROBE_INC(cds_ft_probe_gs_commit_ok);
				if (gs_fuse_incoh)
					FT_GS_PROBE_INC(
						cds_ft_probe_gs_fuse_incoh_committed);
				k2 = ft_graft_swap_descend(dst_ft, key, key_len,
					&d2, &raw2);
				if (raw2 && raw2 == gs_pub_old) {
					FT_GS_PROBE_INC(cds_ft_probe_gs_reoccupy);
					fprintf(stderr, "GSPROBE reoccupy: kase=%d k2=%d "
						"pub_old=%p published=%p old_child=%p "
						"nfp=%p->%p nfp2=%p->%p pnf=%p pnf2=%p\n",
						(int) kase, (int) k2,
						(void *) gs_pub_old, (void *) gs_top,
						(void *) old_child,
						(void *) d.nfp,
						(void *) (d.nfp ? *d.nfp : NULL),
						(void *) d2.nfp,
						(void *) (d2.nfp ? *d2.nfp : NULL),
						(void *) d.pnf, (void *) d2.pnf);
				} else if (d2.nfp != d.nfp) {
					FT_GS_PROBE_INC(cds_ft_probe_gs_slot_moved);
				}
			}
#endif

			/*
			 * Retire swap's root to an empty node AND (paired)
			 * unlink run_S from swap's ordered list, FUSED in ONE
			 * flip.  (List off: just the lone root edge.)
			 *
			 * This is past the dst attach's commit, so it is the
			 * failure-free section.  The pre-reserved
			 * @swap_retire_txn cannot abort.  @swap_ft is
			 * exclusive, so the cross-view window that the fusion
			 * closes has no observer left anyway.  The fusion is
			 * kept because the pairing is the invariant, not the
			 * audience.  run_D is installed as swap's list after
			 * the extract publish below.
			 *
			 * @swap_ft's content is momentarily reachable from BOTH
			 * tries here: dst published it above, and swap has not
			 * yet let go.  Only a reader of @swap_ft could see
			 * that, and an exclusive trie has none.  The same
			 * premise lets the replace's apply_deferred re-parent
			 * swap's interior into dst before this point.
			 */
			if (!swap_empty) {
				struct cds_ft_inode_flag *empty =
					ft_node_flag(fresh, 0);

				assert(swap_ft->exclusive);
				if (gs_ord)
					/*
					 * Empty swap's sentinel (relink_dest NULL):
					 * run_S's cells were re-homed into dst by the
					 * run-replace above; run_D is installed as
					 * swap's list after the extract publish.
					 */
					ft_root_list_swap_publish(swap_ft,
						swap_retire_txn, &swap_ft->root,
						swap_ft->root, empty,
						ft_ord_first(swap_ft), NULL,
						ft_ord_last(swap_ft), NULL,
						NULL, false);
				else
					ft_root_edge_flip(swap_ft, &swap_ft->root,
						swap_ft->root, empty);
				swap_retire_txn = NULL;	/* consumed */
				FT_TP(root_publish, (const void *) swap_ft,
					(const void *) swap_ft->root);
				/*
				 * The childless root whose key chain went into
				 * dst on its own: nothing names it any more, and
				 * the exclusive swap_ft has no reader inside it.
				 */
				if (swap_nil_only) {
					ft_meta_tombstone_set_flip(swap_rmeta);
					free_cds_ft_node(swap_ft,
						ft_node_ptr(old_swap_root));
				}
			}
		} else {
			/*
			 * Empty-swap remove (a REMOVE of @old_child at @key).
			 * Route it through a move-style ft_detach_node.  The
			 * parent's bookkeeping is then correct for EVERY
			 * graft-point shape:
			 * - a sole-child prune (compressed / single-child
			 *   internal): frees @d.pnf and the chain to a
			 *   surviving ancestor;
			 * - a multi-child in-place delete: clears the parent's
			 *   pigeon bitmap bit / popcount slot, which a raw
			 *   ft_publish_to_parent(NULL) does NOT;
			 * - any recompaction a shrinking parent needs.
			 *
			 * free_detached_subtree == false preserves @old_child
			 * for the extract side below.  Propagate -@old_count
			 * FIRST (undercount ordering, while @d.pnf is still
			 * live).  Then detach from the reserve secured in PREP,
			 * so that the detach cannot fail.  The detach owns the
			 * parent nr_child + any prune.  So the post-publish
			 * nr_child-- / propagate are skipped (empty_pruned).
			 *
			 * Fuse run_D's ordered-list removal into the detach's
			 * structural flip.  This closes the disappear-side
			 * cross-view window.  The ft_detach_run is EXCISE-ONLY
			 * (into == NULL): run_D is re-homed to swap_ft below.
			 * - ft_detach_node arms @drun on whichever commit path
			 *   it takes (in-place / recompaction /
			 *   compressed-external-promote).
			 * - @dpub is the gating/arm flag.
			 * - swap_run.armed then suppresses the standalone
			 *   run-replace.
			 * run_D's heads are read while @old_child is still
			 * intact (before the detach).
			 */
			int dret;
			struct ft_remove_pub dpub = { .armed = false };
			struct ft_detach_run drun = { .into = NULL };

			if (gs_ord) {
				drun.rfirst = ft_subtree_minmax_head(dst_ft, old_child,
						false);
				drun.rlast = ft_subtree_minmax_head(dst_ft, old_child,
						true);
			}
			cds_ft_alloc_reserve_activate(dst_ft, &gs_reserve);
			struct ft_lock_ctx lctx;

			ft_lock_ctx_init(&lctx, &d, NULL, &optxn);
			dret = ft_detach_node(dst_ft, &lctx, d.nfp, d.pnfp, d.depth,
					false, NULL, gs_ord ? &dpub : NULL,
					gs_ord ? &drun : NULL, NULL, NULL, 0,
					-(long) old_count /* fold -old_count onto the detach commit */,
					NULL, false, NULL, NULL,
					false /*bulk: never in place*/);
			cds_ft_alloc_reserve_deactivate(dst_ft);
			/*
			 * MW LOCK_FINE drop: under the FT-wide lock this detach
			 * is failure-free.  With the lock dropped it can fail
			 * two ways.  BOTH are build-invisible in
			 * ft_detach_node: the failure aborts before any
			 * reader-visible store.  And this is the empty-swap
			 * REMOVE, so no swap content is consumed.  So both
			 * tries stay pristine.
			 *
			 *  - -EAGAIN: a peer relocated the contended spine, and
			 *    the detach's recompaction cannot lock its
			 *    ancestor.  Free this attempt's build-invisible
			 *    extract cluster + reserve + txns and re-descend
			 *    (mirrors retry_attach / retry_merge).
			 *  - -ENOMEM: the detach's commit flip-txn is a malloc
			 *    NOT backed by the node reserve.  So it can still
			 *    fail under memory pressure (ft-remove.h,
			 *    "aborts before any side-effect").  This is
			 *    terminal: fall into prep_oom rather than spinning
			 *    a hopeless re-descend.  prep_oom releases the §11
			 *    pin, frees every handle and returns MEMORY_ERROR.
			 */
			if (dret == -EAGAIN) {
				ft_glue_abort(swap_ft, &glue_extract);
				ft_glue_abort(dst_ft, &glue_insert);
				if (extract_txn) {
					ft_flip_txn_destroy(extract_txn);
					extract_txn = NULL;
				}
				if (run_replace_txn) {
					ft_flip_txn_destroy(run_replace_txn);
					run_replace_txn = NULL;
				}
				if (gs_reserved) {
					cds_ft_alloc_reserve_drain(dst_ft, &gs_reserve);
					gs_reserved = false;
				}
				FT_GS_PROBE_INC(cds_ft_probe_gs_retry);
				ft_txn_attempt_bail(&optxn, gs_open);
				gs_open = false;
				goto retry_swap;
			}
			if (dret != 0) {
				assert(dret == -ENOMEM);
				goto prep_oom;
			}
			(void) dret;
			swap_run.armed = drun.armed;
			empty_pruned = true;
		}

		/*
		 * graft_swap edits the subtree at @key via ft_publish_to_parent
		 * directly (no ft_node_set_nth), so emit the structural edge for
		 * consumers.  The pruned case freed @d.pnf and emits its own
		 * detach tracepoints.
		 */
		if (d.depth >= 1 && !empty_pruned)
			FT_TP(tree_edge_set, (const void *) dst_ft,
				(const void *) d.pnf,
				(unsigned int) (d.depth - 1),
				(uint8_t) _key[d.depth - 1],
				(const void *) (have_insert ? glue_insert.top : NULL));

		/*
		 * Parent nr_child on the non-NULL -> NULL transition (remove) +
		 * external-count propagation.  Skipped for @empty_pruned: the detach
		 * above owns nr_child and propagated the count first (@d.pnf is freed).
		 */
		if (!empty_pruned) {
			pmeta = cds_ft_item_to_metadata(ft_node_ptr(d.pnf));
			if (!have_insert)
				ft_meta_nr_child_dec_flip(pmeta);
			/* NET count delta folded onto the replace publish above. */
		}

		/*
		 * Replace run_D with run_S in dst's ordered list.  run_S now
		 * lives at @key structurally.  For an empty swap run_S is NULL,
		 * and run_D just leaves.  This is paired with the dst-side
		 * drain below, which removes any reader still holding run_D in
		 * dst.
		 *
		 * Skipped when the publish above already FUSED it into the
		 * structural flip (swap_run.armed).  Only the gs_reserved
		 * sole-child detach-prune path still uses this standalone form:
		 * its prune commits separately via ft_detach_node.  It commits
		 * through the pre-reserved @run_replace_txn (un-abortable
		 * post-drain).  Every fused shape frees that txn unused.
		 */
		if (gs_ord && !swap_run.armed) {
			ft_ord_cell_run_replace(dst_ft, run_replace_txn,
				gs_d_first, gs_d_last, gs_s_first, gs_s_last);
			run_replace_txn = NULL;	/* consumed */
		}
		if (run_replace_txn)
			ft_flip_txn_destroy(run_replace_txn);	/* fused: unused */

		/*
		 * Drain the dst-side readers that may still hold the displaced
		 * subtree, or any node within it, in their RCU snapshot.  They
		 * hold it with its OLD parent pointing into dst.
		 *
		 * Without this sync, the extract apply_deferred below rewires
		 * that parent to point into cluster B (in swap_ft).  A reader
		 * walking up via the rewired pointer would CROSS-TRIE-ESCAPE
		 * from dst into swap_ft.  It would observe top_B's NULL parent
		 * at non-root depth.
		 *
		 * The earlier sync at the swap unlink only drains swap_ft
		 * readers; this one drains dst_ft readers that captured the
		 * displaced data before it was detached from dst by the
		 * insert-side publish above.
		 *
		 * Exclusive dst carries no RCU readers, so the sync is
		 * skipped in that case.
		 */
		/*
		 * Close the escalation bracket BEFORE the dst drain below.
		 * ft_writer_lock_gp_wait() waits a grace period.  A writer
		 * parked on the trie's FIFO lane is an ONLINE, non-quiescent
		 * reader that holds that grace period open.  This is why that
		 * function asserts !ft_op_in_lane().  Closing the attempt also
		 * closes its read section.
		 */
		ft_txn_attempt_end(&optxn, gs_open);
		gs_open = false;
		if (!dst_ft->exclusive)
			ft_writer_lock_gp_wait(dst_ft);

		/*
		 * Extract side: wire cluster B's deferred back-pointer, then install
		 * swap_ft's new root.  This re-parents the displaced subtree AFTER it
		 * has been detached from dst by the publish above and after the
		 * dst-side drain above.
		 */
		ft_glue_apply_deferred(dst_ft, &glue_extract);
		{
			struct ft_ord_cell_edge edges[3] = { 0 };	/* root + head + tail */
			unsigned int n = 0;

			if (top_B) {
				struct cds_ft_metadata *bm =
					cds_ft_item_to_metadata(ft_node_ptr(top_B));

				/* @top_B is swap_ft's new root: name that trie. */
				bm->parent_word = ft_trie_parent(swap_ft);
				ft_meta_parent_slot_offset_set(bm, 0);
				/*
				 * Structural root install, deferred into the fused flip
				 * below so it commits atomically with run_D's head/tail --
				 * the appear-side cross-view window (structure-present /
				 * list-empty).
				 */
				edges[n].slot = (struct ft_ord_cell **) &swap_ft->root;
				edges[n].old_target =
					(struct ft_ord_cell *) swap_ft->root;
				edges[n].new_target = (struct ft_ord_cell *) top_B;
				/* A root records MW: no node owns it. */
				edges[n].root = true;
				n++;
			} else {
				/*
				 * External (or absent) displaced content:
				 * attach it as external_nodes on swap_ft's
				 * root.  That root is the transient @fresh
				 * for a non-empty swap, or old_swap_root's
				 * empty root for an empty swap.  The
				 * preceding dst-side drain makes this
				 * displaced (pre-existing) content's plain
				 * forward store safe.  The target is a
				 * NULL-parent root (DRAIN_EXEMPT).
				 */
				struct cds_ft_inode_flag *root_nf = swap_empty ?
					old_swap_root : ft_node_flag(fresh, 0);
				struct cds_ft_metadata *rm = swap_empty ?
					swap_rmeta : fresh_meta;

				if (old_child) {
					ft_metadata_set_external_nodes(root_nf, rm,
						(struct cds_ft_node *) ft_node_ptr(old_child));
					/*
					 * Root: parent is legitimately NULL.
					 * Publishing prev here is safe: this
					 * back-pointer chain has no fresh
					 * non-root cluster node.  It is kept
					 * paired with the metadata write for
					 * consistency with the other attach
					 * sites.
					 */
					ft_publish_external_nodes_prev(dst_ft, root_nf,
						(struct cds_ft_node *) ft_node_ptr(old_child));
					ft_nr_keys_store(swap_ft, rm, old_count, CMM_RELEASE);
				}
			}

			/*
			 * Install run_D (the extracted subtree's heads) as
			 * swap_ft's whole ordered list.  In the top_B case it
			 * is FUSED with the structural root install above into
			 * ONE flip.  A reader then never sees swap
			 * structure-present / list-empty.
			 * swap_ft was drained at the
			 * unlink sync, so clearing run_D's boundary links is a plain store.
			 */
			if (gs_ord) {
				/*
				 * swap_ft was emptied (swap_retire / already
				 * empty), so its sentinel currently points
				 * at itself.  Point the run's outer links at
				 * swap's sentinel (plain: swap was drained).
				 * Then fuse the sentinel endpoint flips
				 * (self -> run_D first/last) with the
				 * structural root install above.
				 */
				gs_d_first->lnode.prev = &swap_ft->ord_sentinel.node;
				gs_d_last->lnode.next = &swap_ft->ord_sentinel.node;
				edges[n].tag = FT_TXN_TAG;	/* ordered-cell edge */
				edges[n].slot = (struct ft_ord_cell **)
					&swap_ft->ord_sentinel.node.next;
				edges[n].sentinel_ft = swap_ft;
				edges[n].old_target = ft_ord_sentinel_cell(swap_ft);
				edges[n].new_target = gs_d_first;
				n++;
				edges[n].tag = FT_TXN_TAG;	/* ordered-cell edge */
				edges[n].slot = (struct ft_ord_cell **)
					&swap_ft->ord_sentinel.node.prev;
				edges[n].sentinel_ft = swap_ft;
				edges[n].old_target = ft_ord_sentinel_cell(swap_ft);
				edges[n].new_target = gs_d_last;
				n++;
			}
			/*
			 * Freeze-on-free (doc §4.B).  When @top_B replaces
			 * swap_ft's root, this install retires a transient
			 * root:
			 * - @fresh for a non-empty swap;
			 * - the old empty @old_swap_root for an empty swap.
			 * Both were reader-visible as swap_ft->root during the
			 * unlink window.  The transient root gets its
			 * tombstone recorded INTO extract_txn.  So the mark
			 * and the root install unlink flip atomically (atomic
			 * detach).
			 *
			 * Only on the top_B path: the external/absent case
			 * keeps the root live.  top_B always installed the
			 * root edge into @edges.  So extract_txn is reserved
			 * (list-on) or the 3-edge list-off txn just reserved.
			 *
			 * This transient-root tombstone is distinct from the
			 * fused free-list tombstone already recorded by
			 * ft_glue_apply_deferred.
			 */
			if (top_B)
				ft_flip_txn_record_tombstone(extract_txn,
					cds_ft_item_to_metadata(swap_empty ?
						ft_node_ptr(old_swap_root) : fresh));
			/*
			 * Post-drain commit (un-abortable).  A reserved
			 * extract_txn commits every recorded edge.  It is freed
			 * unused if none changed.  It is one of:
			 * - list on: root install + run_D head/tail + the fused
			 *   top_B and free-list tombstones;
			 * - the list-off 3-edge top_B retire: root install +
			 *   top_B tombstone + free-list tombstone.
			 * When extract_txn is NULL (list off, no top_B retire),
			 * @n is 0 and the free_list is empty.  So the lone-edge
			 * arm is unreached.
			 */
			if (extract_txn) {
				if (n)
					/* Bulk op, not yet MW-hardened: ABORT
					 * unreachable under its exclusion. */
					(void) ft_ord_cell_flip_into(swap_ft,
						extract_txn, edges, n);
				else
					ft_flip_txn_destroy(extract_txn);
				extract_txn = NULL;	/* consumed / freed */
			} else if (n) {
				ft_ord_cell_flip_one(&edges[0]);
			}
			FT_TP(root_publish, (const void *) swap_ft,
				(const void *) swap_ft->root);
			if (top_B) {
				if (swap_empty)
					free_cds_ft_node(swap_ft,
						ft_node_ptr(old_swap_root));
				else
					free_cds_ft_node(swap_ft, fresh);
			}
		}

		/*
		 * Reclaim the old (replaced) live nodes after the publishes.
		 *
		 * ★ BOTH free lists hold DST nodes, so BOTH reclaim through
		 * @dst_ft.  @glue_extract's list is the compressed @old_child
		 * that ft_make_root_internal_glue PEELED to materialize swap's
		 * new root.  That node lived in the DESTINATION and was
		 * reader-visible there.  Only the destination's grace period
		 * drains its readers.
		 *
		 * Reclaiming it through @swap_ft instead took the trie's access
		 * discipline from the WRONG side:
		 * - cds_ft_free_item frees IMMEDIATELY on an exclusive trie (no
		 *   grace period, straight onto the arena free list);
		 * - @swap_ft is exclusive by contract right up to the inherit
		 *   below.
		 * The node was therefore recycled while dst readers, and a peer
		 * writer's in-flight descent, still held it.  The arena handed
		 * the same address back as the next allocation: a second trie's
		 * root wearing a live destination node's address.
		 * inv_graft_swap_shared_dst_nolist saw that as a depth-1 node
		 * whose parent had gone NULL.
		 *
		 * ft_glue_apply_deferred above was already passed @dst_ft for
		 * this same glue; this call was the odd one out.
		 */
		ft_glue_free_old(dst_ft, &glue_insert);
		ft_glue_free_old(dst_ft, &glue_extract);

		{
			size_t nm = key_len + swap_max;
			size_t dm = uatomic_load(&dst_ft->max_used_key_len,
						 CMM_RELAXED);
			if (nm > dm)
				uatomic_store(&dst_ft->max_used_key_len, nm,
					      CMM_RELAXED);
			uatomic_store(&swap_ft->max_used_key_len,
				      dm > key_len ? dm - key_len : 0,
				      CMM_RELAXED);
		}
		/*
		 * swap_ft now holds content displaced from dst_ft; inherit dst_ft's
		 * access discipline for that content.  dst_ft keeps its own.
		 */
		swap_ft->exclusive = dst_ft->exclusive;
		/* Free any unused empty-swap prune reserve. */
		if (gs_reserved)
			cds_ft_alloc_reserve_drain(dst_ft, &gs_reserve);
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;

	prep_oom:
		/*
		 * Allocation failed during the build.  Free every fresh glue
		 * node (both clusters), drop the transient swap root and
		 * surface MEMORY_ERROR.  No deferred edge was applied and
		 * nothing was published.  So dst_ft and swap_ft are both
		 * pristine: there is nothing to roll back.
		 */
		/* Every build error jumps here before the dst-drain release above. */
		/*
		 * Close the escalation bracket BEFORE the build-error unwind.
		 * ft_writer_lock_gp_wait() waits a grace period.  A writer
		 * parked on the trie's FIFO lane is an ONLINE, non-quiescent
		 * reader that holds that grace period open.  This is why that
		 * function asserts !ft_op_in_lane().  Closing the attempt also
		 * closes its read section.
		 */
		ft_txn_attempt_end(&optxn, gs_open);
		gs_open = false;
		ft_glue_abort(dst_ft, &glue_insert);
		ft_glue_abort(swap_ft, &glue_extract);
		if (glue_insert.txn)
			ft_flip_txn_destroy(glue_insert.txn);
		if (glue_publish_txn)
			ft_flip_txn_destroy(glue_publish_txn);
		if (swap_retire_txn)
			ft_flip_txn_destroy(swap_retire_txn);
		if (extract_txn)
			ft_flip_txn_destroy(extract_txn);
		if (run_replace_txn)
			ft_flip_txn_destroy(run_replace_txn);
		if (fresh)
			free_cds_ft_node_unpublished(swap_ft, fresh);	/* never published */
		if (gs_reserved)
			cds_ft_alloc_reserve_drain(dst_ft, &gs_reserve);
		FT_TP(graft_swap_exit, (int) CDS_FT_STATUS_MEMORY_ERROR);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
}

