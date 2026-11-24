// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-merge.h
 *
 * Userspace RCU library - Fractal Trie: merge / merge_at (spine-copy, same-trie rekey).
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-merge.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Same-trie rekey mode threaded into ft_merge_at_inner:
 *
 *   FT_REKEY_NONE:  a plain CROSS-trie cds_ft_merge_at.  The entry enforces
 *                   src_ft != dst_ft.
 *   FT_REKEY_MERGE: the cds_ft_rekey_merge entry point.  It unions into an
 *                   occupied destination, like cds_ft_merge_at.
 *   FT_REKEY_GRAFT: the cds_ft_rekey_graft entry point.  It REQUIRES an empty
 *                   destination and returns POPULATED_ERROR otherwise, like
 *                   cds_ft_graft.
 *
 * For both rekey modes, src_ft == dst_ft == the trie.
 */
enum ft_rekey_mode {
	FT_REKEY_NONE = 0,
	FT_REKEY_MERGE,
	FT_REKEY_GRAFT,
};

/*
 * ★ OUTSIDE THE MERGE GUARD ON PURPOSE.  This enum belongs to REKEY, not to
 * merge.  With -DNO_FEATURE_FT_MERGE the rekey GRAFT stays available.  Only
 * the occupied-destination MERGE mode goes away.  So FT_REKEY_NONE and
 * FT_REKEY_GRAFT must still be declared.  FT_REKEY_MERGE stays declared too.
 * The feature guard removes the arms that ACT on it.  A mode nobody can reach
 * costs nothing.
 */

/*
 * ★ SHARED INFRASTRUCTURE, OUTSIDE THE MERGE GUARD.  The two functions below
 * are a read-only descent and a source-subtree unlink.  The REKEY paths that
 * survive -DNO_FEATURE_FT_MERGE call both, so they are not part of the
 * feature.  Only the occupied-destination merge mode goes away with that
 * flag.  Neither function calls another merge-internal helper.  This makes
 * the split at this line exact.
 */
/*
 * Locate the merge point at the end of @key, read-only.  Reuse
 * ft_graft_swap_descend: it has the same three outcomes and publishes
 * nothing.  Also report the cursor's compressed offset and the subtree's key
 * count.  cds_ft_merge_at needs both:
 *
 *   FT_GRAFT_SWAP_EXACT:       @d->nf is the live subtree at @key; @off_ret 0;
 *                              @count_ret its nr_keys (1 for an external / dup
 *                              chain -- one unique key).
 *   FT_GRAFT_SWAP_KEY_SHORTER: @key ends inside compressed @d->nf; @off_ret =
 *                              key_len - d->depth (bytes consumed into the
 *                              node); @count_ret the node's subtree nr_keys.
 *   FT_GRAFT_SWAP_DELEGATE:    no content under @key (src side -> the merge is
 *                              a no-op; dst side -> the atomic fast-path graft).
 *
 * @off_ret / @count_ret are 0 for DELEGATE.
 */
static
enum ft_graft_swap_case ft_merge_descend(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, struct ft_descent *d,
		unsigned int *off_ret, unsigned long *count_ret)
{
	struct cds_ft_inode_flag *raw;	/* the merge has no forward-edge plan to quote it */
	enum ft_graft_swap_case kase = ft_graft_swap_descend(ft, key, key_len, d,
			&raw);

	switch (kase) {
	case FT_GRAFT_SWAP_KEY_SHORTER:
		*off_ret = (unsigned int) (key_len - d->depth);
		/* Whole subtree of the compressed node the short key lands in. */
		*count_ret = ft_node_key_count(ft, d->nf);
		break;
	case FT_GRAFT_SWAP_EXACT:
		*off_ret = 0;
		/* External -> one key (possibly a dup chain); else subtree count. */
		*count_ret = ft_node_key_count(ft, d->nf);
		break;
	default:	/* FT_GRAFT_SWAP_DELEGATE */
		*off_ret = 0;
		*count_ret = 0;
		break;
	}
	return kase;
}

#ifdef FEATURE_FT_MERGE
/*
 * Unlink the EXACT subtree at @src_key from @src_ft IN PLACE.  Keep the
 * subtree node, because the spine-copy merge still references it.  The commit
 * re-parents it into the merged cluster.  This is the non-root-src analogue
 * of the root-src ft->root swap.  It removes the merge source from @src_ft
 * and prunes the now-empty single-child branch above it.
 *
 * The descent, ft_detach_node and the chain reclaim mirror the non-root path
 * of ft_detach_keylen, with two differences:
 *   - @free_detached_subtree is false;
 *   - the subtree is NOT wrapped in a transient trie.  The merge owns it
 *     through @gd's referenced edges.  Wrapping it would double-own it.
 *
 * @detached_count is the subtree's unique-key count, from ft_merge_descend.
 * It is propagated out of the ancestors.
 *
 * The caller must call this as the LAST fallible commit step.  Return
 * -ENOMEM when the ft_detach_node recompaction fails.  @src_ft is then left
 * pristine.  The caller aborts the still-invisible build with both tries
 * intact, and needs no rollback.  Return 0 on success.  The caller then
 * drains @src_ft and runs the failure-free commit tail.
 */
static
int ft_merge_unlink_src_subtree(struct cds_ft *src_ft,
		const uint8_t *_src_key, size_t src_key_len,
		unsigned long detached_count, struct ft_detach_run *run,
		struct ft_glue *retire_glue)
{
	/*
	 * @src_key is ALREADY ORDINAL: cds_ft_merge_at converts it once at its
	 * entry.  Applying the key map again here would descend a different
	 * subtree than the one the spine build copied.
	 */
	const uint8_t *key = _src_key, *ik;
	struct ft_descent d;
	int ret;

	ik = key;

	/*
	 * Plain key-guided descent to the merge source's subtree root.  As in
	 * ft_detach_keylen, no branch-point snapshot is tracked: ft_detach_node
	 * is bootstrapped from the target's own slot and climbs parent pointers
	 * to the surviving ancestor.
	 *
	 * For a KEY_SHORTER source, the key ends inside a compressed node.
	 * The descent overshoots that node, and @d.nf becomes its child.  The
	 * climb's free walk reclaims the whole compressed node and preserves
	 * @d.nf.  This matches the old explicit chain reclaim.
	 */
	ft_descent_init(&d, src_ft);
	for (; d.depth < src_key_len; ) {
		uint8_t kv;

		/* Caller already established EXACT, so the path is present. */
		if (ft_node_compressed(d.nf)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(d.nf);
			unsigned int remaining =
				(unsigned int) (src_key_len - d.depth);
			unsigned int cmp = (unsigned int) cn->len < remaining ?
				(unsigned int) cn->len : remaining;

			/*
			 * This step does not compare the run with the key.  The
			 * caller's ft_merge_descend did, on this same source.
			 * Check it, so that a caller which skipped that
			 * classification does not unlink a subtree @src_key
			 * does not name.
			 */
			urcu_assert_debug(ft_match_compressed_key(ik, cn, cmp) ==
				cmp);
			ft_descent_traverse_compressed(src_ft, &d, cn, &ik);
			continue;
		}
		kv = *(ik++);
		ft_descent_step(src_ft, &d, kv);
	}


	/*
	 * Unlink the branch in place and preserve the move target.  The target
	 * is @d.nf, the subtree root the spine-copy merge keeps referencing.
	 * ft_detach_node is bootstrapped from the target's own slot and climbs
	 * to the surviving ancestor.  Its free walk has two phases:
	 *   - phase 1 reclaims the intermediate single-child chain, compressed
	 *     and skip-target nodes included;
	 *   - phase 2 would free the target.  It stays gated off for
	 *     move-style.
	 */
	{
		/*
		 * @run is EXCISE-ONLY (into==NULL).  It fuses the structural
		 * unlink with the run's removal from src's ordered list, in ONE
		 * flip.  ft_detach_node gates that fusion on a non-NULL @pub.
		 * @pub is the structural-publish deferral that ft_detach_node
		 * records and commits alongside the cell edges.  So supply one
		 * here, exactly as ft_detach's move-style unlink does.
		 *
		 * A NULL @run (list off) leaves both NULL.  This is the unfused
		 * two-store unlink.
		 */
		struct ft_remove_pub pub = { .armed = false };
		struct ft_remove_pub *pubp = run ? &pub : NULL;

		struct ft_lock_ctx lctx;

		ft_lock_ctx_init(&lctx, &d, NULL,
			retire_glue ? retire_glue->op : NULL);
		ret = ft_detach_node(src_ft, &lctx, d.nfp, d.pnfp, d.depth,
				/*free_detached_subtree=*/ false, NULL, pubp, run,
				retire_glue, NULL, 0,
				/*
				 * Fold the whole-subtree count removal onto
				 * the unlink.  -detached_count rides
				 * ft_detach_node's own commit.  It is exact
				 * under concurrent writers.  The leaf
				 * machinery is magnitude-agnostic.
				 */
				-(long) detached_count, NULL, false, NULL, NULL,
				/* bulk: exclusive-only tier */
				ft_in_place_delete_excl_ok(src_ft));
	}
	if (ret < 0) {
		/*
		 * Recompaction OOM.  The folded count edges rode the
		 * uncommitted txn, and an abort applies nothing.  So src is
		 * pristine and there is no propagation to undo.
		 */
		return -ENOMEM;
	}
	return 0;
}



/*
 * cds_ft_merge_at build-invisible spine-copy.  ft_merge_build takes two
 * subtrees rooted at the same suffix position: S from src and D from dst.
 * It works recursively:
 *   - it COPIES the overlapping spine of S and D;
 *   - it REFERENCES every disjoint subtree by pointer.  Each reference is
 *     recorded as a deferred re-parent edge, applied at commit.
 * All allocation happens here, build-invisibly.  On OOM the fresh copies are
 * freed and both tries stay pristine.  No rollback is needed.
 *
 * Return the PLAIN flag of the freshly-built merged node.  When the merged
 * position is leaf-only, return the surviving external head instead.  Return
 * FT_MERGE_OOM on allocation failure; the caller then aborts both glues.  On
 * success *nr_keys_ret holds the merged subtree's unique-key count.
 */
#define FT_MERGE_OOM		((struct cds_ft_inode_flag *) (long) -ENOMEM)

struct ft_merge_ctx {
	struct cds_ft *dst_ft;		/* all fresh merged nodes live here */
	struct ft_glue *gd;	/* dst cluster: built/deferred/dst frees/splices */
	struct ft_glue *gs;	/* src side: src-overlap frees (+ prune later) */
	/*
	 * DLM overlap-spine plan-lock (§9.4 M-2): fence each DST overlap node
	 * before reading its body, and retire it through the fenced terminal.
	 * Set by the caller when the dst trie is lock_fine.
	 */
	bool fence_overlap;
	/*
	 * Fence each SRC overlap node too, and retire it through the fenced
	 * terminal -- the mirror of @fence_overlap for the S side.
	 *
	 * A CROSS-TRIE merge does not need this.  cds_ft_merge_at requires the
	 * src exclusive, so no peer can touch S while the build copies it.
	 *
	 * The SAME-TRIE REKEY fold needs it.  The fold RECORDS its detach into
	 * the same txn as everything else.  So its source stays LIVE for the
	 * whole build window, reachable by readers and writers.  Without the
	 * fence, the hazard is the one the header of ft_merge_lock_overlap
	 * describes for the dst side:
	 *   - a peer inserts BELOW S in the copy window, and the build does not
	 *     see it;
	 *   - the late expected-old still matches and the commit succeeds;
	 *   - the peer's child is retired with the node.
	 * The result is silent key loss.
	 *
	 * The caller sets this when its source is live (the rekey fold).
	 * ft_merge_spine_copy leaves it false.  See
	 * feedback_rekey_src_is_live_shared_helpers.
	 */
	bool fence_src;
	/*
	 * Set when a fence acquire MISSED.  The build returns FT_MERGE_OOM, as
	 * for any other failure: one sentinel, one caller unwind.  This flag
	 * tells CONTENTION apart from a real allocation failure.  The caller
	 * can then re-descend instead of reporting MEMORY_ERROR.
	 */
	bool overlap_contended;
	/*
	 * The ABSOLUTE byte-depth of each side's merge point.  A fence needs
	 * it to date its node.  ft_merge_build's @depth counts bytes from the
	 * merge point: it says where the RECURSION is, not where the NODE is.
	 * An anchor is a function of the node's absolute depth (§2).
	 *
	 * Do not hand the relative value to ft_anchor_meta.  It would date the
	 * top overlap node as byte-depth 0.  Depth 0 does not mean "undated":
	 * it means THE ROOT.  The node would anchor on ITSELF, while every op
	 * that dates it correctly anchors on an ancestor.  Two ops would then
	 * use two words, with no exclusion.
	 *
	 * Each caller sets these at its single construction site, where the
	 * two descents are in scope.  @off is included because a KEY_SHORTER
	 * merge point sits INSIDE a compressed run.  The run starts at
	 * @d->depth and the cursor is @off bytes further.  Relative depth 0
	 * names the cursor.  The run itself is not fenced at that frame: a
	 * compressed node is fenced only when entered at offset 0.  So no
	 * fence reads a base that skipped its own node's start.
	 */
	unsigned int dst_base_depth;
	unsigned int src_base_depth;
};

/* Upper-bound counters for the read-only pre-pass that sizes the glues. */
struct ft_merge_counts {
	int nb;		/* fresh built nodes (-> gd) */
	int nd;		/* deferred re-parent edges (-> gd) */
	int nf_dst;	/* dst-overlap frees (-> gd) */
	int nf_src;	/* src-overlap frees (-> gs) */
	int ns;		/* duplicate-chain splices (-> gd) */
};

/* nr_keys of a (possibly skip-encoded) child subtree referenced by pointer. */
static
unsigned long ft_merge_child_count(struct cds_ft *ft, struct cds_ft_inode_flag *c)
{
	/*
	 * The count is one for an external leaf.  Otherwise it is the
	 * maintained nr_keys when the trie keeps order statistics, else a
	 * structural recount.  So the merge's count-driven sizing and
	 * short-circuits are correct on a rank-stats-off trie.
	 */
	return ft_node_key_count(ft, c);
}

static
struct cds_ft_inode_flag *ft_merge_build(struct ft_merge_ctx *c,
		struct cds_ft_inode_flag *S, unsigned int off_s,
		struct cds_ft_inode_flag *D, unsigned int off_d,
		unsigned int depth, unsigned int d_prov,
		unsigned long *nr_keys_ret);

/*
 * Advance a compressed cursor one byte past @next_off - 1.  While the cursor
 * is still inside the run, report the run's flag at @next_off.  Else fall
 * through to the run's child at offset 0.  That child is internal or
 * external, never compressed.
 */
static inline
void ft_merge_advance(struct cds_ft_compressed_node *cn, unsigned int next_off,
		struct cds_ft_inode_flag **flag_ret, unsigned int *off_ret)
{
	if (next_off < cn->len) {
		*flag_ret = ft_compressed_node_flag(cn);
		*off_ret = next_off;
	} else {
		*flag_ret = cn->child;
		*off_ret = 0;
	}
}

/*
 * Materialize, build-invisibly, the subtree that a single-side compressed run
 * contributes to a fresh branch slot.  That subtree is the bytes
 * cn->key_bytes[off+1 .. len) down to cn's (live) child.  The parent branch's
 * slot consumes the byte cn->key_bytes[off], so this builds the part below it:
 *
 *   suffix_len == 0:                  the bare live child (no fresh node; the
 *                                     parent frame's Pass 2 wires it).
 *   suffix_len == 1 (non-skip):       a 1-child internal node.
 *   suffix_len >= 2, or == 1 (skip):  a fresh compressed node, returned in its
 *                                     skip-published form.
 *
 * This mirrors the old-direction suffix of ft_split_compressed_graft_build.
 * The back-pointer of cn's (live) child is deferred into @c->gd with
 * @dst_origin: false for a src run, true for a dst run.  A fresh wrapper is
 * tracked in @c->gd too.  @child_depth is the depth of the materialized node
 * itself (parent + 1).  Return the flag to store in the parent slot, or
 * FT_MERGE_OOM.
 */
static
struct cds_ft_inode_flag *ft_merge_materialize_suffix(struct ft_merge_ctx *c,
		struct cds_ft_compressed_node *cn, unsigned int off,
		unsigned int child_depth, bool dst_origin,
		unsigned long *ck_ret)
{
	struct cds_ft *ft = c->dst_ft;
	struct ft_glue *g = c->gd;	/* all fresh merged nodes -> gd */
	unsigned int suffix_len = cn->len - off - 1;
	unsigned long child_keys = ft_merge_child_count(ft, cn->child);
	struct cds_ft_inode_flag **slot;
	int ret;

	*ck_ret = child_keys;
	if (suffix_len >= 2
#ifdef FEATURE_FT_SKIP_COMPRESSED
			|| (suffix_len == 1 && ft_group_skip_compressed(ft->group))
#endif
	   ) {
		struct cds_ft_compressed_node *sfx;
		struct cds_ft_metadata *sfx_meta;
		struct cds_ft_inode_flag *plain;

		sfx = alloc_compressed_node(ft, (uint8_t) suffix_len, &sfx_meta);
		if (!sfx)
			return FT_MERGE_OOM;
		sfx->child = cn->child;
		sfx->len = (uint8_t) suffix_len;
		memcpy(sfx->key_bytes, &cn->key_bytes[off + 1], suffix_len);
		ft_meta_nr_child_set(sfx_meta, 1);
		ft_nr_keys_store(ft, sfx_meta, child_keys, CMM_RELAXED);
		plain = ft_compressed_node_flag(sfx);
		ft_glue_track(g, plain);
		ft_glue_defer_edge_origin(ft, g, cn->child, plain, &sfx->child,
				dst_origin);
		/*
		 * Return the PLAIN flag.  The skip form of a fresh compressed
		 * node cannot be resolved during the build, because its
		 * child's back-pointer is deferred.  So the parent stores the
		 * plain form, which ft_glue_is_fresh matches by direct
		 * identity.  The parent's Pass 2 re-encodes the slot to the
		 * skip form.
		 */
		return plain;
	} else if (suffix_len == 1) {
		/* 1-child internal suffix (non-skip-compressed). */
		struct cds_ft_inode_flag *dest = NULL;

		ret = ft_node_set_nth(ft, &dest, cn->key_bytes[off + 1],
				cn->child, NULL, NULL, child_depth, true);
		if (ret)
			return FT_MERGE_OOM;
		ft_nr_keys_store(ft, cds_ft_item_to_metadata(ft_node_ptr(dest)),
				child_keys, CMM_RELAXED);
		ft_glue_track(g, dest);
		ft_node_get_nth_skip(dest, &slot, cn->key_bytes[off + 1],
				FT_PF_NONE);
		ft_glue_defer_edge_origin(ft, g, cn->child, dest, slot,
				dst_origin);
		return dest;
	}
	/* suffix_len == 0: the bare child; parent Pass 2 wires its edge. */
	return cn->child;
}

/*
 * Both overlap sides are compressed runs sharing a @p (>= 1) byte prefix from
 * their cursors.  Emit ONE fresh compressed run for the shared bytes.  Then
 * recurse on what follows each cursor: the next byte, or the run's child once
 * a run is exhausted.
 *
 * The shared run's child is internal, external or another fresh branch.  It
 * is never another compressed node, because the child of a canonical
 * compressed node is never compressed.  So no two adjacent compressed nodes
 * result.
 *
 * Return the run's PLAIN flag, or FT_MERGE_OOM.  The parent's Pass 2
 * re-encodes the slot to the skip form.
 */
static
struct cds_ft_inode_flag *ft_merge_build_run(struct ft_merge_ctx *c,
		struct cds_ft_compressed_node *cn_s, unsigned int off_s,
		struct cds_ft_compressed_node *cn_d, unsigned int off_d,
		unsigned int p, unsigned int depth, unsigned long *nr_keys_ret)
{
	struct cds_ft *ft = c->dst_ft;
	struct cds_ft_compressed_node *run;
	struct cds_ft_metadata *run_meta;
	struct cds_ft_inode_flag *adv_s, *adv_d, *child, *plain;
	unsigned int aoff_s, aoff_d;
	unsigned long ck = 0;

	ft_merge_advance(cn_s, off_s + p, &adv_s, &aoff_s);
	ft_merge_advance(cn_d, off_d + p, &adv_d, &aoff_d);
	child = ft_merge_build(c, adv_s, aoff_s, adv_d, aoff_d, depth + p,
			c->dst_base_depth + depth - off_d, &ck);
	if (child == FT_MERGE_OOM)
		return child;

#ifndef FEATURE_FT_SKIP_COMPRESSED
	if (p == 1) {
		/*
		 * Non-skip-compressed: a 1-byte run is a 1-child internal node,
		 * not a len-1 compressed node.  This matches the canonical form
		 * of insert and split.  It has the same node+edge budget as the
		 * compressed run.
		 */
		struct cds_ft_inode_flag *dest = NULL, **slot;
		int ret;

		ret = ft_node_set_nth(ft, &dest, cn_s->key_bytes[off_s], child,
				NULL, NULL, depth, true);
		if (ret)
			return FT_MERGE_OOM;
		ft_nr_keys_store(ft, cds_ft_item_to_metadata(ft_node_ptr(dest)), ck,
				CMM_RELAXED);
		ft_glue_track(c->gd, dest);
		ft_node_get_nth_skip(dest, &slot, cn_s->key_bytes[off_s],
				FT_PF_NONE);
		ft_glue_defer_edge_origin(ft, c->gd, child, dest, slot,
				/*dst_origin=*/ true);
		*nr_keys_ret = ck;
		return dest;
	}
#endif
	run = alloc_compressed_node(ft, (uint8_t) p, &run_meta);
	if (!run) {
		/*
		 * The recursion's cluster is already tracked in the glue; the
		 * caller's abort frees it.  Only this frame's run failed.
		 */
		return FT_MERGE_OOM;
	}
	run->child = child;
	run->len = (uint8_t) p;
	memcpy(run->key_bytes, &cn_s->key_bytes[off_s], p);
	ft_meta_nr_child_set(run_meta, 1);
	ft_nr_keys_store(ft, run_meta, ck, CMM_RELAXED);
	plain = ft_compressed_node_flag(run);
	ft_glue_track(c->gd, plain);
	/*
	 * Wire run->child's back-pointer.  A fresh recursion result is stored
	 * immediately (is_fresh).  A live result can only be the dst splice
	 * head that the recursion returns.  So dst_origin is safe either way.
	 */
	ft_glue_defer_edge_origin(ft, c->gd, child, plain, &run->child,
			/*dst_origin=*/ true);
	/*
	 * Return the PLAIN flag, like ft_merge_materialize_suffix.  Until the
	 * run is wired, is_fresh can match it by identity.  Then the parent
	 * frame's Pass 2 re-encodes the holding slot to the skip form.
	 */
	*nr_keys_ret = ck;
	return plain;
}

/*
 * DLM overlap-spine plan-lock (§9.4 M-2).  Acquire the LOCK fence of a DST
 * overlap node BEFORE ft_merge_build reads its body into the merged cluster.
 * The copy plan and the retire that ratifies it then see the same state of
 * the node.
 *
 * WHAT IT CLOSES.  The retire was a PLAIN {s -> s|TOMBSTONE}.  Its
 * expected-old is read fresh at commit.  NO lock covered the window in which
 * the build copied the node's children:
 *   - a peer adds a child in that window, and the build does not see it;
 *   - the late expected-old still matches and the commit succeeds;
 *   - the peer's child is retired along with the node.
 * The result is silent key loss.  This is the "unlocked retire" class.  The
 * escalation model separates it from the lock_or_guard publish sites, which
 * do abort on a peer touch.
 *
 * Return -EAGAIN on a dirty word: a peer's parked proxy, a concurrent copier
 * or a real retire.  That aborts the whole build:
 *   - ft_merge_build returns FT_MERGE_OOM;
 *   - the caller calls ft_glue_abort;
 *   - ft_glue_clear_fenced releases the marks already taken.
 * Only DST overlap nodes are fenced -- a cross-trie merge REQUIRES its
 * source exclusive (and the rekey's source is a detach product), so the src side
 * has no peer to exclude and its glue keeps the plain retire.
 */
static inline
int ft_merge_lock_overlap(const struct cds_ft *ft,
		const struct ft_lock_ctx *ctx, struct cds_ft_inode_flag *nf,
		unsigned int depth, void *node, struct ft_held_anchor *held)
{
	struct cds_ft_metadata *m = cds_ft_item_to_metadata(
		(struct cds_ft_inode *) node);

	/*
	 * Hand the WHOLE held anchor back, not just @node's clean word.  The
	 * fenced retire this fence pays for is recorded against the word the
	 * acquire LOCKED.  With coarsening, that word belongs to an ancestor
	 * of @node.
	 */
	return ft_acquire_member(ft, ctx, nf, m, depth, held);
}

/*
 * @d_prov: ABSOLUTE byte-depth of the OLD dst node whose slot provided @D.
 * That node is the holder of @D when @D is EXTERNAL: a dup-chain head hangs
 * off its provider.  An anchor is a function of the holder's absolute depth
 * (see @dst_base_depth).
 *
 * The splice record used to get a merge-relative value.  It then dated the
 * holder as a near-root node, which anchors on ITSELF at coarse spacings.
 * The fence dated the holder correctly and coarsened to an ancestor.  So one
 * op used two exclusion words for one node, and the splice acquire refused
 * its own fence forever.  This was measured as the exponential-MW
 * occupied-dst graft storm.
 */
static
struct cds_ft_inode_flag *ft_merge_build(struct ft_merge_ctx *c,
		struct cds_ft_inode_flag *S, unsigned int off_s,
		struct cds_ft_inode_flag *D, unsigned int off_d,
		unsigned int depth, unsigned int d_prov,
		unsigned long *nr_keys_ret)
{
	struct cds_ft *ft = c->dst_ft;
	struct cds_ft_node *S_leaf, *D_leaf, *M_ext;
	struct cds_ft_compressed_node *cn_s, *cn_d;
	bool S_ext, D_ext, S_comp, D_comp;
	struct cds_ft_inode_flag *M = NULL;
	struct cds_ft_metadata *Mmeta = NULL;
	unsigned long total_keys = 0;
	bool tracked = false;
	unsigned int b, s_fb = 0, d_fb = 0;
	struct ft_held_anchor d_ov_held = { 0 };	/* this frame's dst overlap fence */
	bool d_ov_fenced = false;
	struct ft_held_anchor s_ov_held = { 0 };	/* and the src side's, when @fence_src */
	bool s_ov_fenced = false;

	S = ft_resolve_skip_compressed(ft, S);
	D = ft_resolve_skip_compressed(ft, D);

	S_comp = ft_node_compressed(S);
	D_comp = ft_node_compressed(D);
	cn_s = S_comp ? ft_compressed_node_ptr(S) : NULL;
	cn_d = D_comp ? ft_compressed_node_ptr(D) : NULL;

	/*
	 * Record each compressed overlap node's reclaim exactly once, on first
	 * entry (off == 0).  The shared-run recursion may re-enter a run at a
	 * deeper cursor, but the node is freed whole.  src -> gs, dst -> gd.
	 * Internal overlap nodes are recorded at the tail instead.
	 */
	/*
	 * Plan-lock THIS FRAME's SRC overlap node, on the same terms as the dst
	 * block below:
	 *   - lock before any body read;
	 *   - record the retire together with the mark.  A frame that fails
	 *     before its tail then cannot leave an unrecorded fence.
	 * Only the rekey fold sets @fence_src.  A cross-trie merge owns its
	 * source exclusively and skips this.
	 */
	if (c->fence_src) {
		void *snode = NULL;

		if (S_comp) {
			if (off_s == 0)
				snode = cn_s;
		} else if (!ft_node_external(S)) {
			snode = ft_node_ptr(S);
		}
		if (snode) {
			struct ft_lock_ctx sctx;

			ft_glue_lock_ctx(c->gs, &sctx);
			if (ft_merge_lock_overlap(ft, &sctx, S,
					c->src_base_depth + depth, snode,
					&s_ov_held)) {
				c->overlap_contended = true;
				return FT_MERGE_OOM;
			}
			ft_glue_defer_free_fenced(c->gs, snode, S_comp,
				&s_ov_held);
			s_ov_fenced = true;
		}
	}
	if (S_comp && off_s == 0 && !s_ov_fenced)
		ft_glue_defer_free(c->gs, cn_s, true);
	/*
	 * Plan-lock THIS FRAME's dst overlap node BEFORE any body read below.
	 * Three reads of @D follow:
	 *   - the compressed prefix scan;
	 *   - the Pass-1 ft_node_get_nth_skip sweep;
	 *   - the external_nodes fetch.
	 * The tail defer_free runs after those reads.  Acquire here instead, so
	 * that the copy plan and the retire that ratifies it see the same
	 * state of the node.
	 *
	 * The shared-run recursion may re-enter a compressed run at off_d > 0.
	 * The frame that entered the run at 0 already fenced it, so it must
	 * NOT be re-marked.  ft_meta_lock_acquire refuses an already-locked
	 * word, and we would fail against our own fence.  An EXTERNAL D
	 * retires nothing here, so it takes no lock.
	 */
	if (c->fence_overlap) {
		void *dnode = NULL;

		if (D_comp) {
			if (off_d == 0)
				dnode = cn_d;
		} else if (!ft_node_external(D)) {
			dnode = ft_node_ptr(D);
		}
		if (dnode) {
			struct ft_lock_ctx dctx;

			ft_glue_lock_ctx(c->gd, &dctx);
			if (ft_merge_lock_overlap(ft, &dctx, D,
					c->dst_base_depth + depth, dnode,
					&d_ov_held)) {
				c->overlap_contended = true;
				return FT_MERGE_OOM;
			}
			/*
			 * RECORD THE RETIRE TOGETHER WITH THE MARK.  The
			 * free-list entry makes the fence visible to
			 * ft_glue_clear_fenced.  This frame has failure paths
			 * BEFORE its tail that return straight out: a child
			 * recursion returning FT_MERGE_OOM, or a set_nth
			 * failure.  The plain retire is recorded at the tail.
			 * Recording the fenced one there would leave those
			 * paths holding an unrecorded fence.  The node would
			 * stay LOCK forever, and no later publish into it
			 * could succeed.  Recording early is harmless on the
			 * failing path: the free list tombstones a node only
			 * if this build reaches its commit.
			 */
			ft_glue_defer_free_fenced(c->gd, dnode, D_comp,
				&d_ov_held);
			d_ov_fenced = true;
		}
	}
	if (D_comp && off_d == 0 && !d_ov_fenced)
		ft_glue_defer_free(c->gd, cn_d, true);

	/*
	 * Both sides are compressed and share a prefix from their cursors:
	 * collapse the shared bytes into one fresh run and recurse past it.
	 * A zero-byte common prefix means the two runs diverge at the cursor:
	 * fall through to build a 2-way branch.
	 */
	if (S_comp && D_comp) {
		unsigned int rem_s = cn_s->len - off_s;
		unsigned int rem_d = cn_d->len - off_d;
		unsigned int maxp = rem_s < rem_d ? rem_s : rem_d;
		unsigned int p = 0;

		while (p < maxp &&
		       cn_s->key_bytes[off_s + p] == cn_d->key_bytes[off_d + p])
			p++;
		if (p >= 1)
			return ft_merge_build_run(c, cn_s, off_s, cn_d, off_d,
					p, depth, nr_keys_ret);
	}

	S_ext = ft_node_external(S);
	D_ext = ft_node_external(D);
	S_leaf = S_comp ? NULL : (S_ext ? (struct cds_ft_node *) ft_node_ptr(S)
		: cds_ft_item_to_metadata(ft_node_ptr(S))->external_nodes);
	D_leaf = D_comp ? NULL : (D_ext ? (struct cds_ft_node *) ft_node_ptr(D)
		: cds_ft_item_to_metadata(ft_node_ptr(D))->external_nodes);

	/*
	 * Both leaf-only -> the SAME full key terminates on both sides.
	 * Splice src after dst and return the (leaf-only) dst head; the
	 * parent frame wires the slot and the dst head's back-pointer.
	 */
	if (S_ext && D_ext) {
		ft_glue_record_splice(c->gd, D_leaf, S_leaf, d_prov);
		*nr_keys_ret = 1;
		return D;
	}

	/*
	 * Otherwise build a fresh internal branch M.  A compressed side
	 * contributes its single forced byte.  It never contributes
	 * external_nodes, which a compressed node may not carry.
	 *
	 * Pass 1: add the forward slot of every union child.  This uses
	 * cluster_leaf: no child back-pointer is written here, because a later
	 * set_nth may reallocate M.  Recurse on shared bytes.  For a one-side
	 * byte, reference the child (internal side) or materialize it
	 * (compressed side).
	 */
	if (S_comp)
		s_fb = cn_s->key_bytes[off_s];
	if (D_comp)
		d_fb = cn_d->key_bytes[off_d];
	for (b = 0; b < FT_ENTRY_PER_NODE; b++) {
		struct cds_ft_inode_flag *sc = NULL, *dc = NULL, *child;
		struct cds_ft_inode *old = NULL;
		bool sc_present, dc_present;
		unsigned long ck = 0;
		int ret;

		if (S_comp)
			sc_present = (b == s_fb);
		else if (!S_ext)
			sc_present = (sc = ft_node_get_nth_skip(S, NULL,
					(uint8_t) b, FT_PF_NONE)) != NULL;
		else
			sc_present = false;
		if (D_comp)
			dc_present = (b == d_fb);
		else if (!D_ext)
			dc_present = (dc = ft_node_get_nth_skip(D, NULL,
					(uint8_t) b, FT_PF_NONE)) != NULL;
		else
			dc_present = false;
		if (!sc_present && !dc_present)
			continue;
		/*
		 * F1 / RESOLVED-POINTER CONTRACT, at the build's source of children.
		 * ft_node_get_nth_skip is the RAW scanner.  Every other caller
		 * resolves its result: ft_node_get_nth and
		 * ft_node_get_nth_reanchor_slot both do.  This caller does
		 * not.  So a peer mid-commit on a live dst slot hands the
		 * build a parked type-7 proxy.  The low nibble of that proxy
		 * reads as an internal node.
		 *
		 * Do NOT resolve it.  @dc is EMBEDDED into the merged cluster
		 * below: child = dc, published by ft_node_set_nth.  Resolving
		 * picks the old or the new target by the peer's CURRENT
		 * status.  That status is still free to flip, so the merged
		 * node could end up naming a retired child.
		 *
		 * Using it unresolved is worse.  The count and glue-origin
		 * reads (ft_node_key_count, ft_glue_is_fresh) dereference it
		 * and compute off the latch.
		 *
		 * Bail on the CONTENTION channel instead, exactly as a missed
		 * overlap fence does.  FT_MERGE_OOM is the one unwind
		 * sentinel.  @overlap_contended tells the caller that the
		 * cause was a peer, not memory.  The caller then re-descends
		 * instead of reporting MEMORY_ERROR.  Both tries are pristine
		 * at this point: the build has published nothing, and
		 * ft_glue_abort releases every fence it took.
		 *
		 * @sc is guarded too, though no capture has ever named it.  A
		 * cross-trie source is exclusive, but the rekey fold's source
		 * is LIVE, which is why @fence_src exists.  The test costs
		 * one predicted-not-taken compare.
		 */
		if (caa_unlikely(ft_node_flip_proxy(sc) ||
				ft_node_flip_proxy(dc))) {
			c->overlap_contended = true;
			return FT_MERGE_OOM;
		}

		if (sc_present && dc_present) {
			/*
			 * Both present -> recurse.  At most one side is
			 * compressed here, because two divergent runs have
			 * different forced bytes.  So the other side's target
			 * is its internal child at offset 0.
			 */
			struct cds_ft_inode_flag *ts, *td;
			unsigned int os, od;

			if (S_comp)
				ft_merge_advance(cn_s, off_s + 1, &ts, &os);
			else {
				ts = sc;
				os = 0;
			}
			if (D_comp)
				ft_merge_advance(cn_d, off_d + 1, &td, &od);
			else {
				td = dc;
				od = 0;
			}
			child = ft_merge_build(c, ts, os, td, od, depth + 1,
					c->dst_base_depth + depth -
					(D_comp ? off_d : 0), &ck);
			if (child == FT_MERGE_OOM)
				return child;
		} else if (sc_present) {
			if (S_comp) {
				child = ft_merge_materialize_suffix(c, cn_s,
						off_s, depth + 1,
						/*dst_origin=*/ false, &ck);
				if (child == FT_MERGE_OOM)
					return child;
			} else {
				child = sc;	/* reference live src subtree */
				ck = ft_merge_child_count(ft, sc);
			}
		} else {
			if (D_comp) {
				child = ft_merge_materialize_suffix(c, cn_d,
						off_d, depth + 1,
						/*dst_origin=*/ true, &ck);
				if (child == FT_MERGE_OOM)
					return child;
			} else {
				child = dc;	/* reference live dst subtree */
				ck = ft_merge_child_count(ft, dc);
			}
		}
		ret = ft_node_set_nth(ft, &M, (uint8_t) b, child, &old, Mmeta,
				depth, /*cluster_leaf*/ true);
		if (ret)
			return FT_MERGE_OOM;
		if (old) {
			/*
			 * @old is not a displaced CHILD.  When the slot did
			 * not fit, the node had to GROW (ft_node_recompact,
			 * @old_node_ret).  ft_node_set_nth then returns the
			 * node's own SUPERSEDED BODY here.  That recompact has
			 * already marked it DEAD.  This is the §4.B
			 * freeze-on-free mark, set for every retire the
			 * recompact performs.  The recompact also states the
			 * matching obligation: "the caller ... frees @old_node
			 * AFTER A GRACE PERIOD".
			 *
			 * So take the RETIRE path, not the unpublished one.
			 * The contract of the immediate path is "never
			 * published, no reader can hold a reference".  It
			 * PROVES that by asserting that the node carries no
			 * tombstone.  This body carries one, so the immediate
			 * free read as a live-node free.  This was seen in unit
			 * test 60, in every FT_DEBUG_TOMBSTONE_AUDIT build.
			 *
			 * The cluster is build-invisible, but that does not
			 * change the ownership.  The mark says the retire is
			 * already owned.  That ownership costs a grace period,
			 * once per node GROWTH in a merge build.
			 */
			ft_glue_untrack(ft, c->gd, old);
			free_cds_ft_node(ft, old);
		}
		if (!tracked) {
			ft_glue_track(c->gd, M);
			tracked = true;
			Mmeta = cds_ft_item_to_metadata(ft_node_ptr(M));
			/*
			 * Clear the recycled allocation's stale parent before
			 * any later set_nth reallocation copies it forward.
			 */
			Mmeta->parent_word = NULL;
#ifdef FEATURE_FT_SKIP_COMPRESSED
			ft_meta_parent_slot_offset_set(Mmeta, 0);
#endif
		} else if (old) {
			ft_glue_track(c->gd, M);
		}
		Mmeta = cds_ft_item_to_metadata(ft_node_ptr(M));
		total_keys += ck;
	}

	/* Merged external_nodes (the key terminating at M itself). */
	if (S_leaf && D_leaf) {
		M_ext = D_leaf;
		ft_glue_record_splice(c->gd, D_leaf, S_leaf,
			D_ext ? d_prov : c->dst_base_depth + depth);
	} else if (D_leaf) {
		M_ext = D_leaf;
	} else {
		M_ext = S_leaf;		/* may be NULL */
	}
	if (M_ext) {
		ft_metadata_set_external_nodes(M, Mmeta, M_ext);
		total_keys += 1;
	}

	/*
	 * Pass 2: wire every child's back-pointer (deferred) plus M's
	 * external back-channel.  Fetch slots only now, because the Pass-1
	 * set_nth reallocations may have moved them.  A fresh child (recursed
	 * or materialized) stores its parent immediately, through defer_edge's
	 * is_fresh fast path.  A referenced live child truly defers to commit.
	 */
	for (b = 0; b < FT_ENTRY_PER_NODE; b++) {
		struct cds_ft_inode_flag **slot;
		struct cds_ft_inode_flag *child =
			ft_node_get_nth_skip(M, &slot, (uint8_t) b, FT_PF_NONE);
		bool dst_origin;

		if (!child)
			continue;
		/*
		 * A referenced one-side child is dst-origin iff D still holds a
		 * child at this byte.  A shared byte was recursed or
		 * materialized into a fresh cluster node.  defer_edge's
		 * is_fresh path applies that edge immediately, so its origin
		 * is irrelevant.  The flip-latch switches dst-origin
		 * back-pointers after the forward publish.  apply_deferred
		 * does not.
		 */
		if (D_comp)
			dst_origin = (b == d_fb);
		else
			dst_origin = !D_ext && ft_node_get_nth_skip(D, NULL,
					(uint8_t) b, FT_PF_NONE) != NULL;
		ft_glue_defer_edge_origin(ft, c->gd, child, M, slot, dst_origin);
		/*
		 * A freshly-built compressed child (run / suffix) was stored as
		 * its PLAIN flag, so that is_fresh could match it by identity
		 * above.  Its edge is now wired: re-encode the slot to the
		 * canonical skip form.  Referenced compressed children are
		 * already stored skip-encoded, and a skip flag is not
		 * ft_node_compressed.
		 */
		if (ft_node_compressed(child)) {
			struct cds_ft_inode_flag *skip = ft_publish_compressed(ft,
				ft_compressed_node_ptr(child), child);

			if (skip != child)
				*slot = skip;
		}
	}
	if (M_ext)
		ft_glue_defer_edge_origin(ft, c->gd,
			(struct cds_ft_inode_flag *) M_ext, M, NULL,
			/*dst_origin=*/ D_leaf != NULL);

	/*
	 * Reclaim the INTERNAL overlap nodes M copied (compressed ones were
	 * recorded on entry above).  src -> gs, dst -> gd.
	 */
	if (!S_ext && !S_comp
			/* Already recorded at entry, together with its fence. */
			&& !s_ov_fenced
	   )
		ft_glue_defer_free(c->gs, ft_node_ptr(S), false);
	if (!D_ext && !D_comp
			/* Already recorded at entry, together with its fence. */
			&& !d_ov_fenced
	   )
		ft_glue_defer_free(c->gd, ft_node_ptr(D), false);

	ft_nr_keys_store(ft, Mmeta, total_keys, CMM_RELAXED);
	*nr_keys_ret = total_keys;
	return M;
}

/*
 * Read-only pre-pass that mirrors the control flow of ft_merge_build.  It
 * accumulates upper bounds for ft_glue_reserve, so that the build never
 * asserts on a full inline floor.  No allocation, no mutation.
 */
static
void ft_merge_count(struct cds_ft *ft, struct cds_ft_inode_flag *S, unsigned int off_s,
		struct cds_ft_inode_flag *D, unsigned int off_d,
		struct ft_merge_counts *cnt)
{
	struct cds_ft_compressed_node *cn_s, *cn_d;
	bool S_ext, D_ext, S_comp, D_comp;
	struct cds_ft_node *S_leaf, *D_leaf;
	unsigned int b, s_fb = 0, d_fb = 0;

	S = ft_resolve_skip_compressed(ft, S);
	D = ft_resolve_skip_compressed(ft, D);
	S_comp = ft_node_compressed(S);
	D_comp = ft_node_compressed(D);
	cn_s = S_comp ? ft_compressed_node_ptr(S) : NULL;
	cn_d = D_comp ? ft_compressed_node_ptr(D) : NULL;

	/* Compressed overlap nodes are freed whole, recorded on first entry. */
	if (S_comp && off_s == 0)
		cnt->nf_src++;
	if (D_comp && off_d == 0)
		cnt->nf_dst++;

	/* Shared run: one fresh run node + its child edge, then recurse past. */
	if (S_comp && D_comp) {
		unsigned int rem_s = cn_s->len - off_s;
		unsigned int rem_d = cn_d->len - off_d;
		unsigned int maxp = rem_s < rem_d ? rem_s : rem_d;
		unsigned int p = 0;

		while (p < maxp &&
		       cn_s->key_bytes[off_s + p] == cn_d->key_bytes[off_d + p])
			p++;
		if (p >= 1) {
			struct cds_ft_inode_flag *as, *ad;
			unsigned int aos, aod;

			cnt->nb++;
			cnt->nd++;
			ft_merge_advance(cn_s, off_s + p, &as, &aos);
			ft_merge_advance(cn_d, off_d + p, &ad, &aod);
			ft_merge_count(ft, as, aos, ad, aod, cnt);
			return;
		}
	}

	S_ext = ft_node_external(S);
	D_ext = ft_node_external(D);
	S_leaf = S_comp ? NULL : (S_ext ? (struct cds_ft_node *) ft_node_ptr(S)
		: cds_ft_item_to_metadata(ft_node_ptr(S))->external_nodes);
	D_leaf = D_comp ? NULL : (D_ext ? (struct cds_ft_node *) ft_node_ptr(D)
		: cds_ft_item_to_metadata(ft_node_ptr(D))->external_nodes);
	if (S_ext && D_ext) {
		cnt->ns++;
		return;
	}
	cnt->nb++;		/* fresh branch M */
	if (S_comp)
		s_fb = cn_s->key_bytes[off_s];
	if (D_comp)
		d_fb = cn_d->key_bytes[off_d];
	for (b = 0; b < FT_ENTRY_PER_NODE; b++) {
		struct cds_ft_inode_flag *sc = NULL, *dc = NULL;
		bool sc_present, dc_present;

		if (S_comp)
			sc_present = (b == s_fb);
		else if (!S_ext)
			sc_present = (sc = ft_node_get_nth_skip(S, NULL,
					(uint8_t) b, FT_PF_NONE)) != NULL;
		else
			sc_present = false;
		if (D_comp)
			dc_present = (b == d_fb);
		else if (!D_ext)
			dc_present = (dc = ft_node_get_nth_skip(D, NULL,
					(uint8_t) b, FT_PF_NONE)) != NULL;
		else
			dc_present = false;
		if (!sc_present && !dc_present)
			continue;
		cnt->nd++;		/* M -> child back-pointer edge */
		if (sc_present && dc_present) {
			struct cds_ft_inode_flag *ts, *td;
			unsigned int os, od;

			if (S_comp)
				ft_merge_advance(cn_s, off_s + 1, &ts, &os);
			else {
				ts = sc;
				os = 0;
			}
			if (D_comp)
				ft_merge_advance(cn_d, off_d + 1, &td, &od);
			else {
				td = dc;
				od = 0;
			}
			ft_merge_count(ft, ts, os, td, od, cnt);
		} else if ((sc_present && S_comp) || (dc_present && D_comp)) {
			/* Materialized suffix; a fresh wrapper adds a node+edge. */
			struct cds_ft_compressed_node *cn = sc_present ? cn_s : cn_d;
			unsigned int off = sc_present ? off_s : off_d;
			unsigned int suffix_len = cn->len - off - 1;

			if (suffix_len >= 1) {
				cnt->nb++;
				cnt->nd++;
			}
		}
	}
	if (S_leaf && D_leaf) {
		cnt->ns++;
		cnt->nd++;
	} else if (S_leaf || D_leaf) {
		cnt->nd++;
	}
	if (!S_ext && !S_comp)
		cnt->nf_src++;
	if (!D_ext && !D_comp)
		cnt->nf_dst++;
}

/*
 * A surviving source head captured for the spine-copy interleave.  It holds
 * the head's cell and the key SUFFIX below the src merge point.  The suffix
 * is @suffix_len bytes at @suffix_off in the caller's packed pool.  The
 * capture happens BEFORE the src unlink, while S is still attached and
 * up-walkable.  The merge below compares these suffixes against the LIVE dst
 * suffixes.  See ft_merge_ord_interleave_collect.
 */
struct ft_merge_src_cap {
	struct ft_ord_cell *cell;
	size_t suffix_off;
	size_t suffix_len;
};

/*
 * ☠ THE SPINE-COPY CAPTURE'S BOUNDS ARE A PREMISE, NOT A DEBUG CHECK.
 *
 * The capture loop of ft_merge_spine_copy walks the src run from its first
 * head to its last.  The walk is sized by @cnt_src.  The
 * loops trust three facts:
 *   - the walk reaches @slast before the list ends;
 *   - every head's key is at least @src_key_len long;
 *   - the run holds fewer than @cnt_src + 8 heads.
 *
 * These were assert()s, so a -DNDEBUG build dropped all three.  A false one
 * then became, in the same order:
 *   - an up-walk of the sentinel (or of NULL);
 *   - a wrapped @suffix_len handed to memcpy;
 *   - a store past the end of the caps array.
 * This is silent heap corruption, where the debug build stopped.
 *
 * The facts are true by EXCLUSION, not by retry.  A merge's source is an
 * EXCLUSIVE trie with no peer.  A rekey runs inside the bulk gate with the
 * FT-wide lock held, and point writers wait on that lock for the window.  A
 * violation means that this exclusion is broken.  Bailing to a re-descend
 * would retry against the broken exclusion instead of reporting it.  So keep
 * the assert's meaning, in every build.
 */
static inline
void ft_merge_src_capture_premise(bool ok, const char *what,
		const struct ft_ord_cell *cell, const char *fn, int line)
{
	if (caa_likely(ok))
		return;
	fprintf(stderr, "FT SPINE-COPY CAPTURE: %s at cell %p (%s:%d) -- the "
		"src run changed under a capture its exclusion says is "
		"stable\n", what, (const void *) cell, fn, line);
	fflush(stderr);
	abort();
}
#define FT_MERGE_SRC_CAPTURE_PREMISE(ok_, what_, cell_)			\
	ft_merge_src_capture_premise((ok_), (what_), (cell_), __func__,	\
		__LINE__)

/*
 * ☠ LOCK WHAT THE SPINE-COPY INTERLEAVE NAMES BEFORE RECORDING IT.
 *
 * A merged run that becomes the list's new minimum or maximum writes
 * @dst_ft's sentinel .next / .prev.  A point insert at either end parks those
 * words SW, under ord_begin_lock / ord_end_lock.  Recording them here
 * unlocked made them MW on this path and SW on the point-insert path.  One
 * word then had two disciplines, excluded only by the bulk gate.  This was
 * the 09-21 skeptic's finding.
 *
 * The guard could not see it while ft_cell_word_lock tested @owner_cell
 * first, because the interleave names no owner cell.  Resolving the SLOT
 * first put these edges in front of the guard.
 *
 * Both callers are past the src unlink, so there is no bail left.  A miss
 * means that a peer was inside these words when none should have been: the
 * bulk-gate premise failed.  The answer is the same as the root take gives
 * at the same site (ft_flip_txn_lock_root_unfailable).  An interior edge
 * naming no cell resolves to no lock and is skipped here, as before.
 */
static inline
void ft_merge_take_interleave_locks(struct cds_ft *dst_ft,
		const struct ft_glue *gd, struct ft_flip_txn *txn,
		const struct ft_ord_cell_edge *edges, unsigned int n)
{
	struct ft_lock_ctx ctx;

	ft_glue_lock_ctx(gd, &ctx);
	if (ft_cell_lockset_take_edges(dst_ft, &ctx, txn, edges, n)) {
		fprintf(stderr, "FT SPINE-COPY INTERLEAVE: lock take failed on "
			"an unfailable publish (ft %p) producer %p -- the "
			"bulk-gate premise is false\n", (void *) dst_ft,
			__builtin_return_address(0));
		fflush(stderr);
		abort();
	}
}

/*
 * Compare two ordinal key suffixes.  The order matches the ordered-list key
 * ordering: a shorter key that is a prefix of a longer one sorts FIRST.
 * ft_subtree_minmax_head relies on this prefix-key rule.  Return <0 / 0 / >0.
 */
static inline
int ft_merge_suffix_cmp(const uint8_t *a, size_t la, const uint8_t *b, size_t lb)
{
	size_t m = la < lb ? la : lb;
	int c = m ? memcmp(a, b, m) : 0;

	if (c != 0)
		return c;
	if (la == lb)
		return 0;
	return la < lb ? -1 : 1;
}

/*
 * Set a SURVIVOR cell's own forward / back link during the interleave:
 *   - a plain store when the cell is not ord-reachable.  This is the
 *     cross-trie merge, whose survivor came from a consumed source list.
 *   - a RECORDED edge when it is ord-reachable.  This is an in-trie rekey,
 *     whose survivors are live in the list being rebuilt.
 * Return the new edge count.  See @record_all at
 * ft_merge_ord_interleave_collect.
 */
static
unsigned int ft_ord_survivor_link(struct ft_ord_cell *cell,
		struct ft_ord_cell *next, struct ft_ord_cell_edge *edges,
		unsigned int n, bool record_all)
{
	if (!record_all) {
		cell->lnode.next = ft_ord_cell_lnode(next);
		return n;
	}
	edges[n].tag = FT_TXN_TAG;		/* ordered-cell edge */
	edges[n].slot = (struct ft_ord_cell **) &cell->lnode.next;
	edges[n].old_target = ft_ord_cell_resolve_ord(&cell->lnode.next);
	edges[n].new_target = next;
	return n + 1;
}

static
unsigned int ft_ord_survivor_back(struct ft_ord_cell *cell,
		struct ft_ord_cell *prev, struct ft_ord_cell_edge *edges,
		unsigned int n, bool record_all)
{
	if (!record_all) {
		cell->lnode.prev = ft_ord_cell_lnode(prev);
		return n;
	}
	edges[n].tag = FT_TXN_TAG;		/* ordered-cell edge */
	edges[n].slot = (struct ft_ord_cell **) &cell->lnode.prev;
	edges[n].old_target = ft_ord_cell_resolve_ord(&cell->lnode.prev);
	edges[n].new_target = prev;
	return n + 1;
}

/*
 * COLLECT the ordered-list edges that interleave the surviving source cells
 * into @dst's ordered list, for a cds_ft_merge_at spine-copy.  This is a
 * two-pointer KEY-ORDER MERGE of two already-sorted LIVE cell runs:
 *   - the dst merge region: @dst_first .. (exclusive) @dst_succ, walked via
 *     ord_next.  Its heads are up-walked LIVE: ft_rebuild_key_upwalk runs
 *     over the intact D, with no proxy installed.  Their key SUFFIX below
 *     the dst merge point is suffix = full_key[@dst_key_len ..].
 *   - the surviving src run: @src_caps[0 .. @nsrc).  Each entry carries its
 *     pre-captured suffix below the src merge point.  The caller captured
 *     it BEFORE ft_merge_unlink_src_subtree, while S was attached.  The
 *     detach leaves S-root's parent stale, so a post-unlink src up-walk is
 *     unsafe.
 * Both runs share the merge-point prefix, so they merge by comparing those
 * suffixes.  An equal-suffix step is a COLLISION, and the dst head wins.
 * Its dup chain absorbs the src head via ft_glue_record_splices.  So the src
 * head is dropped: it is a floating duplicate, never reachable as a distinct
 * head.
 *
 * This runs in the txn's PREPARE phase, with NO structural proxy installed.
 * The merged ORDER is reconstructed from the two live runs.  It is not found
 * by walking the merged structure that is about to be published.  So the
 * collect needs neither the staged proxies nor a writer-side merged-view
 * resolution.  There is also no append-after-install: every edge is recorded
 * before install.
 *
 * The function accumulates the reader-VISIBLE boundary edges into @edges.
 * Such an edge is a dst-original cell's ord_next / ord_prev, or @dst's head
 * / tail.  Return the edge count.  The caller records the edges into the
 * structural flip txn, so that structure + interleave commit in ONE flip.
 * @prev_placed seeds at the region predecessor (@dst_first's ord_prev).
 *
 * ★ @record_all DECIDES WHAT A SURVIVOR'S OWN LINKS COST.
 *   - FALSE: the survivor's next/prev are PLAIN STORES.  This is sound only
 *     because a cross-trie merge's survivor "is not ord-reachable in @dst --
 *     never was -- and the caller already unlinked it from src".  An IN-TRIE
 *     rekey breaks both halves.  Its survivors are live in the very list
 *     being rebuilt.  A plain store there is a reader-visible, non-atomic
 *     mutation.
 *   - TRUE: those links are recorded as edges too, at up to 2 more per
 *     survivor.  The whole reorder then lands in the one commit.
 * The caller sizes @edges accordingly: <= 2*merged_keys+2 visible, plus
 * 2*nsrc when @record_all.
 *
 * @ncollide (optional) counts the equal-suffix steps.  @collided (optional)
 * receives the dropped cells.  A collision drops the src head from the
 * merged order.  The premise is that the src head became a DUPLICATE on the
 * dst head's chain and is "a floating duplicate never reachable as a
 * distinct head".
 *   - Cross-trie, the drop is free: the src list is CONSUMED, so nothing
 *     reaches the cell again.
 *   - IN-TRIE, the src list is the list being rebuilt, so the drop is only
 *     half the work.  The edges below route every surviving link around the
 *     cell.  Once the flip lands, no new reader can reach it.  The cell
 *     must still be RECLAIMED.  An in-trie caller takes @collided and frees
 *     each cell after its commit, as ft_glue_free_collided_cells does
 *     cross-trie.
 *
 * ★ THE TWO BOUNDARIES ARE LINK TARGETS, NOT THE WALK BOUNDS.
 *   - @dst_succ ends the REGION WALK;
 *   - @tail_link is the cell that the merged block's last element links to;
 *   - @prev_placed is the cell that its first element links back to.
 * The link targets and the walk bounds coincide for every caller whose
 * region is bounded by cells that stay put.  They DO NOT when the moved run
 * ABUTS the region.  There, the region's own neighbour on the abutting side
 * is a cell that is itself moving.  The block's real neighbour is the run's
 * outer one.
 *
 * @head_linked says that @prev_placed already points at the first merged
 * cell.  It is false only on that abutting side, where @prev_placed points
 * into the run.  The tail's counterpart is derived: @tail_link != @dst_succ
 * IS the abutting tail.  An abutting caller passes the run's outer
 * neighbours here.  It can then emit ONE boundary pair.  Otherwise it would
 * need a boundary pair plus a separate gap closure naming the same slots.
 *
 * Identity key_map only (matches the rest of the ordered-list machinery).
 */
static
unsigned int ft_merge_ord_interleave_collect(struct cds_ft *dst,
		size_t dst_key_len, struct ft_ord_cell *dst_first,
		struct ft_ord_cell *dst_succ, struct ft_ord_cell *prev_placed,
		struct ft_ord_cell *tail_link, bool head_linked,
		const struct ft_merge_src_cap *src_caps, unsigned long nsrc,
		const uint8_t *src_pool, struct ft_ord_cell_edge *edges,
		bool record_all, unsigned long *ncollide,
		struct ft_ord_cell **collided)
{
	size_t max_len = dst->group->max_key_len;
	uint8_t dbuf[FT_MAX_KEY_LEN];
	struct ft_ord_cell *dcur = dst_first;
	struct ft_ord_cell *prev = prev_placed;
	/*
	 * Sentinel topology: @prev_placed is the region predecessor.  When the
	 * region is at the list head, it now resolves to @dst's sentinel
	 * pseudo-cell (never NULL).
	 *
	 * prev_is_dst means "the previously-placed cell is already linked to
	 * its successor and needs no forward edge".  It is true at the start,
	 * because the predecessor->region-first link pre-exists.  This holds
	 * for a real dst cell and for the sentinel: sentinel.next already
	 * points at the region's first cell.  So the old "new list minimum" /
	 * "list tail" special cases fold into the general neighbour-edge
	 * path: the sentinel IS the neighbour.
	 *
	 * A false @head_linked denies that pre-existing link.  @prev_placed is
	 * then the run's outer neighbour and still points INTO the run.  So
	 * the first merged cell needs its forward edge written, whether it is
	 * a survivor or a dst-original.  Seeding the flag false is enough:
	 * with @record_all the two branches emit the same recorded edge.
	 */
	bool prev_is_dst = head_linked;
	unsigned long si = 0;
	unsigned int n = 0;
	const uint8_t *dsuf = NULL;
	size_t dsuf_len = 0;
	bool dsuf_valid = false;

	/*
	 * Two-pointer key-order merge of the dst region run and the src
	 * survivor run.  Each step emits the smaller-suffix head:
	 *   - a dst-original head stays put.  Its back edge changes only when
	 *     a survivor run precedes it.
	 *   - a surviving src head splices in after the last placed cell.
	 *   - a tie is a collision: emit the dst head, drop the src head.
	 * Dst-original cells keep their relative order, so a dst<->dst step
	 * needs no edge.  Each survivor RUN costs at most two edges: one
	 * entering, one leaving.  So 2 * merged_keys + 2 bounds @edges.
	 */
	while (dcur != dst_succ || si < nsrc) {
		bool take_dst;

		if (dcur == dst_succ) {
			take_dst = false;		/* dst run exhausted */
		} else if (si == nsrc) {
			take_dst = true;		/* src run exhausted */
		} else {
			int cmp;

			if (!dsuf_valid) {
				size_t dfl = ft_rebuild_key_upwalk(dst, dcur,
						dbuf, max_len);

				assert(dfl >= dst_key_len);
				dsuf = dbuf + (max_len - dfl) + dst_key_len;
				dsuf_len = dfl - dst_key_len;
				dsuf_valid = true;
			}
			cmp = ft_merge_suffix_cmp(dsuf, dsuf_len,
					src_pool + src_caps[si].suffix_off,
					src_caps[si].suffix_len);
			/* Tie: dst head wins, drop the colliding src head. */
			if (cmp == 0) {
				if (collided)
					collided[ncollide ? *ncollide : 0] =
						src_caps[si].cell;
				si++;
				if (ncollide)
					(*ncollide)++;
			}
			take_dst = (cmp <= 0);
		}

		if (take_dst) {
			struct ft_ord_cell *cell = dcur;

			/*
			 * Dst-original cell: stays put, already linked in key
			 * order.  Its back edge changes only when a survivor run
			 * was just placed before it (prev is a survivor).
			 */
			if (!prev_is_dst) {
				n = ft_ord_survivor_link(prev, cell, edges, n,
						record_all);
				edges[n].tag = FT_TXN_TAG;	/* ordered-cell edge */
				edges[n].slot = (struct ft_ord_cell **) &cell->lnode.prev;
				edges[n].old_target =
					ft_ord_cell_resolve_ord(&cell->lnode.prev);
				edges[n].new_target = prev;
				n++;
			}
			prev = cell;
			prev_is_dst = true;
			dcur = ft_ord_cell_resolve_ord(&dcur->lnode.next);
			dsuf_valid = false;
		} else {
			struct ft_ord_cell *cell = src_caps[si].cell;

			/* Surviving src cell: pre-set its back link (prev may be the
			 * sentinel: a new list minimum links its prev to &sentinel). */
			n = ft_ord_survivor_back(cell, prev, edges, n, record_all);
			if (prev_is_dst) {
				/*
				 * prev is a dst cell OR the sentinel (region at head):
				 * flip its forward edge to the survivor.  When prev is the
				 * sentinel this IS the old "new list minimum: flip head".
				 */
				edges[n].tag = FT_TXN_TAG;	/* ordered-cell edge */
				edges[n].slot = (struct ft_ord_cell **) &prev->lnode.next;
				edges[n].old_target =
					ft_ord_cell_resolve_ord(&prev->lnode.next);
				edges[n].new_target = cell;
				n++;
			} else {
				n = ft_ord_survivor_link(prev, cell, edges, n,
						record_all);
			}
			prev = cell;
			prev_is_dst = false;
			si++;
		}
	}
	/*
	 * Close the trailing edge: link the last placed cell to @tail_link (the
	 * sentinel at the list tail) and flip that neighbour's back edge.  A
	 * last placed cell that is a dst-original already points there.  The
	 * exception is the abutting tail, where @tail_link is not the region's
	 * own successor.  The region's successor is then a cell inside the
	 * moving run.  So even a dst-original tail has to be re-pointed at the
	 * run's outer neighbour.  Sentinel topology: when @tail_link is the
	 * sentinel this IS the old "flip @dst's tail".
	 */
	if (!prev_is_dst || tail_link != dst_succ) {
		n = ft_ord_survivor_link(prev, tail_link, edges, n, record_all);
		edges[n].tag = FT_TXN_TAG;	/* ordered-cell edge */
		edges[n].slot = (struct ft_ord_cell **) &tail_link->lnode.prev;
		edges[n].old_target =
			ft_ord_cell_resolve_ord(&tail_link->lnode.prev);
		edges[n].new_target = prev;
		n++;
	}
	return n;
}


/*
 * KEY_SHORTER dst: the merge point sits @off bytes inside a compressed dst
 * node whose prefix bytes @key[@d_depth .. @key_len) lie ABOVE it.  Wrap the
 * freshly-built merged node @M (with @mk unique keys) under that prefix.
 * Return the PLAIN flag to publish into the old compressed node's forward
 * slot, or FT_MERGE_OOM.  The caller skip-encodes the flag.
 *
 * @M internal: ft_build_branch lays a compressed prefix over the fresh
 * internal M.  M's edge into the prefix applies immediately, through
 * defer_edge's is_fresh fast path.  So ft_build_branch's dst_origin=false
 * leaf edge is correct.  M's own children keep the dst_origin that
 * ft_merge_build gave them.  A 1-child-no-external M is first canonicalized
 * to compressed.  Its lone child is always a FRESH recursion result, so the
 * fold is dst_origin-safe too.
 *
 * @M compressed: M is a fresh run produced by ft_merge_build_run, the sole
 * path returning a compressed top.  Its child may be a LIVE dst splice head
 * carrying dst_origin=true: a duplicate-key merge through the compressed
 * node.  ft_build_branch's chain-merge would re-defer that child as
 * dst_origin=false.  That would demote a live re-parent out of the flip
 * latch.  So fuse the prefix into M here by hand, and carry the run child's
 * dst_origin (always true).  The fused length @off + M->len <= cn_d->len, so
 * it is a valid compressed length.
 */
static
struct cds_ft_inode_flag *ft_merge_wrap_prefix(struct ft_merge_ctx *c,
		const uint8_t *key, unsigned int d_depth, unsigned int key_len,
		struct cds_ft_inode_flag *M, unsigned long mk)
{
	struct cds_ft *ft = c->dst_ft;
	unsigned int prefix_len = key_len - d_depth;

	if (ft_node_compressed(M)) {
		struct cds_ft_compressed_node *mcn = ft_compressed_node_ptr(M);
		unsigned int merged_len = prefix_len + mcn->len;
		struct cds_ft_compressed_node *merged;
		struct cds_ft_metadata *merged_meta;
		struct cds_ft_inode_flag *mflag;

		merged = alloc_compressed_node(ft, (uint8_t) merged_len,
				&merged_meta);
		if (!merged)
			return FT_MERGE_OOM;
		memcpy(merged->key_bytes, &key[d_depth], prefix_len);
		memcpy(&merged->key_bytes[prefix_len], mcn->key_bytes, mcn->len);
		merged->len = (uint8_t) merged_len;
		merged->child = mcn->child;
		ft_meta_nr_child_set(merged_meta, 1);
		ft_nr_keys_store(ft, merged_meta, mk, CMM_RELAXED);
		mflag = ft_compressed_node_flag(merged);
		ft_glue_track(c->gd, mflag);
		/*
		 * Carry the run child's dst_origin, which ft_merge_build_run
		 * records as true.  A fresh child applies via is_fresh.  A
		 * live splice head flips with the forward slot.  The defer
		 * de-dup supersedes M's stale &mcn->child edge with this one
		 * (same @child).
		 */
		ft_glue_defer_edge_origin(ft, c->gd, mcn->child, mflag,
				&merged->child, /*dst_origin=*/ true);
		ft_glue_untrack(ft, c->gd, mcn);
		free_compressed_node_unpublished(ft, mcn);
		return mflag;	/* PLAIN; caller skip-encodes before publish */
	} else {
		struct cds_ft_inode_flag *canon, *top;

		canon = ft_compress_single_child_if_needed(ft, M, c->gd);
		if (canon == (struct cds_ft_inode_flag *) (long) -ENOMEM)
			return FT_MERGE_OOM;
		top = ft_build_branch(ft, key, d_depth, key_len, canon, mk,
				/*has_external_nodes=*/ false, c->gd);
		if (!top)
			return FT_MERGE_OOM;
		return top;	/* PLAIN compressed; caller skip-encodes */
	}
}

/*
 * Merge @src_ft's subtree at @src_key into @dst_ft at the live EXACT subtree
 * @d_dst.  The merge is a build-invisible spine copy.
 *
 * Keys are in ORDINAL form.  cds_ft_merge_at converts the caller's
 * application keys once at its entry.  Every consumer below expects ordinal
 * bytes: the spine build, the source unlink and the ordered-list interleave.
 *
 * All allocation is in the build phase.  The one exception is the fallible
 * src unlink at commit, for a non-root src.  So any OOM frees the fresh
 * copies and leaves both tries pristine.  No rollback is needed.
 *
 * @d_src is the src subtree and @cnt_src is its key count.  @d_dst is a
 * non-empty dst subtree.
 *
 * The src unlink depends on the src key:
 * - a root src (src_key_len == 0) unlinks via the ft->root swap;
 * - a non-root src unlinks its branch in place at commit.  This is the only
 *   post-build fallible step.
 *
 * @off_src selects the src merge-point shape:
 * - 0 is an EXACT subtree at @d_src->nf;
 * - > 0 is KEY_SHORTER.  The src key ends @off_src bytes inside the
 *   compressed node @d_src->nf.  The build enters that node at the cursor.
 *   ft_merge_unlink_src_subtree reclaims the whole node via its
 *   chain-reclaim.  The move-style detach preserves cn_s->child.
 *
 * @off_dst selects the dst merge-point shape:
 * - 0 is an EXACT subtree at @d_dst->nf;
 * - > 0 is KEY_SHORTER.  The dst key ends @off_dst bytes inside the
 *   compressed node @d_dst->nf.  ft_merge_wrap_prefix wraps the prefix
 *   bytes of that node around the merged cluster.
 *
 * Every (EXACT | KEY_SHORTER) src x (EXACT | KEY_SHORTER) dst shape is handled.
 * Compressed overlaps are handled.  Both root and non-root dst merge points
 * are handled.  A non-root point flips the interior forward slot through a
 * type-7 proxy.  The read-side descent resolves that proxy at every child
 * fetch.
 *
 * Returns OK on a committed merge, or MEMORY_ERROR on OOM (both tries pristine).
 *
 * @contended is the CONTENTION channel.  It is deliberately kept OUT of enum
 * cds_ft_status.  Under MW LOCK_FINE, this attempt may fail to acquire a
 * dup-chain holder lock.  That means "the tree moved, rebuild and try
 * again".  It is not one of merge's outcomes.  cds_ft_merge_at consumes an
 * EXCLUSIVE source, so it has no transient failure to report.  An attempt
 * that bails has moved nothing and is trivially undone.  The caller just
 * re-descends.
 *
 * Merge's one public BUSY_ERROR says something else entirely: the source is
 * LIVE, call cds_ft_make_exclusive first.  It is a precondition, checked
 * before any allocation.  It is permanent: retrying the identical call
 * returns the identical answer.  Overloading it would leave a caller unable
 * to tell "fix your source" from "just loop".
 *
 * So on contention @contended is set and the RETURN is MEMORY_ERROR.  Every
 * other bail here already uses that conservative surface.  A caller that
 * forgets to check @contended then still reports a no-op on two pristine
 * tries, not a false success.  The retry belongs to the caller, the only one
 * that can re-descend.
 */
static
enum cds_ft_status ft_merge_spine_copy(struct cds_ft *dst_ft,
		struct cds_ft *src_ft, struct ft_descent *d_src,
		const uint8_t *src_key, size_t src_key_len, unsigned long cnt_src,
		unsigned int off_src, struct ft_descent *d_dst,
		unsigned long cnt_dst, unsigned int off_dst,
		size_t dst_key_len, bool *contended, struct ft_op *op)
{
	struct ft_glue gd, gs;
	struct ft_merge_ctx ctx = { .dst_ft = dst_ft, .gd = &gd, .gs = &gs };
	struct ft_merge_counts cnt = { 0, 0, 0, 0, 0 };
	bool root_src = (src_key_len == 0);
	bool ks_dst = (off_dst > 0);
	bool ed;
	struct cds_ft_inode_flag *S = d_src->nf;
	struct cds_ft_inode_flag *D = d_dst->nf;
	struct cds_ft_inode_flag *M, *M_slot = NULL, *pub, *D_old;
	struct cds_ft_inode_flag *pub_parent = d_dst->pnf, **pub_slot = d_dst->nfp;
	struct cds_ft_inode *fresh_root = NULL;
	struct cds_ft_metadata *fresh_meta;
	struct ft_flip_txn *txn;
	unsigned long merged_keys = 0;
	bool ms_ord = dst_ft->group->ordered_list_set;
	struct ft_ord_cell *ms_cursor = NULL, *ms_prev = NULL, *ms_succ = NULL;
	struct cds_ft_node *ms_s_first = NULL, *ms_s_last = NULL;
	struct ft_ord_cell_edge *ms_edges = NULL;
	struct ft_merge_src_cap *ms_src_caps = NULL;	/* src survivor suffixes, pre-captured */
	uint8_t *ms_src_pool = NULL;			/* packed suffix byte pool */
	unsigned long ms_nsrc = 0;			/* src run cells captured */
	unsigned int ms_cap = 0;	/* interleave edge cap, set once merged_keys is known */
	unsigned int ms_n = 0;		/* interleave edges collected (staged pre-commit) */
	*contended = false;

	/*
	 * Every dst merge-point shape is handled:
	 * - the flip proxies the publish slot @pub_slot;
	 * - the read-side descent resolves the type-7 proxy at every child
	 *   fetch (ft_resolve_flip_proxy, before the skip handler);
	 * - ft_glue_set_publish wires the published node's parent to
	 *   @pub_parent.
	 * So a descent and an up-walk see a coherent old-XOR-merged view across
	 * the flip.
	 *
	 * Edge D: the merge point's PARENT is a COMPRESSED node cn_p, reached
	 * via a grandparent skip slot.  cn_p's own parent cannot be compressed
	 * ("no two adjacent compressed" invariant).  So the grandparent slot
	 * d_dst->pnfp is a plain internal slot that merely *holds* skip(cn_p).
	 * Handle it one level up:
	 * - the merge is EXACT at d_dst->nf (off_dst == 0);
	 * - M is wrapped under a fresh copy of the WHOLE cn_p;
	 * - that copy is published into d_dst->pnfp in place of skip(cn_p).
	 * This is the machinery of a KEY_SHORTER dst wrap, with cn_p as the
	 * wrapped node and the grandparent as the publish point.  ks_dst and
	 * ed are mutually exclusive: a KEY_SHORTER merge point sits inside
	 * cn_d, whose parent is internal.
	 */
	ed = (d_dst->pnf &&
	      ft_node_compressed(ft_resolve_skip_compressed(dst_ft, d_dst->pnf)));

	/* Size both glues from a read-only pre-pass (with headroom). */
	ft_merge_count(dst_ft, S, off_src, D, off_dst, &cnt);
	ft_glue_init(&gd);
	ft_glue_init(&gs);
	gd.op = gs.op = op;	/* both glues' lock ctxs age this op */
	/*
	 * Hand the destination descent to the dst glue.  The dst glue's own
	 * acquires fire from commit helpers that never see a descent.  The
	 * main one is @pub_parent.  This is the single place that holds both.
	 * The graft does the same at its ft_glue_set_publish.
	 *
	 * @pub_parent is @d_dst's own parent or grandparent, so the window
	 * dates it.  Without this, the acquire has no depth under a coarse
	 * spacing.  Its miss then aborts the commit, and the op retries
	 * against the same miss.  That is a LIVELOCK, not a failure.
	 */
	gd.lock_d = d_dst;
	/*
	 * Do the same for the SOURCE glue.  A src overlap fence is dated on
	 * the path the node is on NOW, and only @d_src describes that path.
	 * This is inert for a cross-trie merge: @fence_src stays false because
	 * the source is exclusive.  It is set so that the two glues answer
	 * from the same rule.
	 */
	gs.lock_d = d_src;
	/*
	 * Where relative depth 0 IS, on each side (see @dst_base_depth).
	 */
	ctx.dst_base_depth = (unsigned int) d_dst->depth + off_dst;
	ctx.src_base_depth = (unsigned int) d_src->depth + off_src;
	if (ft_glue_reserve(&gd, cnt.nb + 8, cnt.nd + 8,
				cnt.nf_dst + 8, cnt.ns + 8) ||
	    ft_glue_reserve(&gs, 0, 0, cnt.nf_src + 8, 0)) {
		ft_glue_fini(&gd);
		ft_glue_fini(&gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}

	/* Root src: pre-allocate the fresh empty root for the commit swap. */
	if (root_src) {
		fresh_root = alloc_cds_ft_node(src_ft, &ft_types[0], &fresh_meta);
		if (!fresh_root) {
			ft_glue_fini(&gd);
			ft_glue_fini(&gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		fresh_meta->parent_word = ft_trie_parent(src_ft);
		ft_nr_keys_store(src_ft, fresh_meta, 0, CMM_RELAXED);
	}

	/* Build the merged cluster invisibly (the only build-phase fallible step). */
	/*
	 * DLM overlap-spine plan-lock (§9.4 M-2).  Fence each dst overlap node
	 * before the build reads it, and retire it through the fenced terminal.
	 * Then a peer growing a node this merge is copying cannot be silently
	 * retired with it.
	 */
	ctx.fence_overlap = dst_ft->lock_fine;
	ctx.fence_src = false;		/* cross-trie: the source is exclusive */
	M = ft_merge_build(&ctx, S, off_src, D, off_dst, 0, d_dst->pdepth,
			&merged_keys);
	if (M == FT_MERGE_OOM) {
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		/*
		 * A missed overlap fence shares FT_MERGE_OOM's unwind, but it
		 * is CONTENTION, not memory.  Both tries are pristine: the
		 * build published nothing, and ft_glue_abort released every
		 * fence it took.  So report it on the retry channel and let
		 * the caller re-descend.
		 */
		if (ctx.overlap_contended)
			*contended = true;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	/*
	 * The ordered-list interleave is folded into the structural flip: one
	 * commit for structure + ordered list.  So its <= 2*merged_keys+2 cell
	 * edges share the structural flip batch.  Each survivor run costs at
	 * most two visible edges.  Bound it now that @merged_keys is known.
	 */
	if (ms_ord)
		ms_cap = 2u * (unsigned int) merged_keys + 2u;

	/*
	 * Compute @pub (the value to publish) and @pub_parent / @pub_slot
	 * (where).  For the compressed shapes, also reclaim the replaced node.
	 *
	 * KEY_SHORTER dst (off_dst > 0): the merge point sits off_dst bytes
	 * inside the compressed node cn_d = D.  The prefix bytes of cn_d lie
	 * above it.  Wrap M under cn_d->key_bytes[0..off_dst) and replace cn_d
	 * at its own slot (d_dst->nfp).
	 *
	 * Edge D (ed): the merge point's parent is the compressed node cn_p.
	 * Wrap M under the WHOLE cn_p and replace cn_p at the grandparent slot
	 * d_dst->pnfp (pub_parent = d_dst->ppnf).
	 *
	 * Either way, reclaim the replaced compressed node.  ft_merge_build
	 * entered it (or, for Edge D, did not touch it) without recording the
	 * free.  ft_merge_wrap_prefix canonicalizes a single-child M and
	 * preserves the dst_origin of a compressed M's child.
	 *
	 * EXACT dst with an internal/absent parent (off_dst == 0, !ed): M
	 * itself replaces the subtree.  Canonicalize a non-root single-child
	 * internal merge top.  When S and D contribute exactly one shared byte
	 * (a churned "ab"-prefix shape), ft_merge_build returns M as a 1-child
	 * internal with no external_nodes.  That is forbidden at a non-root
	 * position under skip mode (chain-compress invariant).  cds_ft_verify
	 * catches it at the merge depth.  Collapse it to a 1-byte compressed
	 * via the glue.  Only the TOP M can hit this.  Its lone child is always
	 * a FRESH recursion result, so the deferred child edge carries
	 * dst_origin=false correctly and disturbs no flip edge.  A root dst
	 * merge point (d_dst->pnf == NULL) is exempt: a 1-child internal is
	 * canonical at the root.
	 */
	if (ks_dst || ed) {
		struct cds_ft_compressed_node *wrap_cn;
		uint8_t kbuf[FT_MAX_KEY_LEN];
		unsigned int wrap_depth, wrap_len;

		if (ed) {
			wrap_cn = ft_compressed_node_ptr(
				ft_resolve_skip_compressed(dst_ft, d_dst->pnf));
			wrap_len = wrap_cn->len;
			wrap_depth = (unsigned int) d_dst->depth - wrap_len;
			pub_parent = d_dst->ppnf;
			pub_slot = d_dst->pnfp;
		} else {	/* ks_dst */
			wrap_cn = ft_compressed_node_ptr(D);
			wrap_len = off_dst;
			wrap_depth = (unsigned int) d_dst->depth;
		}
		ft_glue_defer_free(&gd, wrap_cn, true);
		memcpy(&kbuf[wrap_depth], wrap_cn->key_bytes, wrap_len);
		pub = ft_merge_wrap_prefix(&ctx, kbuf, wrap_depth,
				wrap_depth + wrap_len, M, merged_keys);
		if (pub == FT_MERGE_OOM) {
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else if (pub_parent) {
		pub = ft_compress_single_child_if_needed(dst_ft, M, &gd);
		if (pub == (struct cds_ft_inode_flag *) (long) -ENOMEM) {
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else {
		pub = M;
	}

	/*
	 * Take or create the flip-txn.  It needs:
	 * - one latch per dst-origin re-parent edge;
	 * - one for the merge-point forward slot;
	 * - @ms_cap for the ordered-list interleave's boundary edges;
	 * - one per collided duplicate-chain splice (the src run tail-append,
	 *   folded in below so the concatenation flips with the structure).
	 * Structure, ordered list AND duplicate chains commit in ONE flip, so
	 * they share this txn.  Reserve it up front to that bound.  Then every
	 * post-drain record is allocation-free: the structural edges, the
	 * INSTALLED-state cell edges and the splice edges.  The splice edges
	 * append into the reserved head chunk.
	 * Create it here, where failure aborts the still-invisible build
	 * (both tries pristine).
	 */
	{
		unsigned int nr_dst = 0;
		int j;

		for (j = 0; j < gd.nr_deferred; j++)
			if (gd.deferred[j].dst_origin)
				nr_dst++;
		txn = ft_flip_txn_create(dst_ft);
		/*
		 * SIZE THE LOCK REGISTRY FROM THE PLAN, here where
		 * -ENOMEM still propagates.  The fenced overlap retires
		 * are what load it: ft_glue_tombstone_free_list
		 * registers one word per !shared fenced holder, on BOTH
		 * sides.  @cap_free is exactly that plan-time count.
		 * Without this the registry grows mid-commit, past the
		 * abort-impossible point.  An allocation failure there
		 * has nowhere to unwind to.
		 */
		if (txn && !ft_flip_txn_reserve_locks(txn,
				gd.cap_free + gs.cap_free
					+ FT_FLIP_TXN_FLOOR_LOCKS)) {
			ft_flip_txn_destroy(txn);
			txn = NULL;
		}
		if (txn && !ft_flip_txn_reserve(txn,
				nr_dst + 1 + ms_cap + gd.cap_free
					+ 2 * gd.nr_splices /* forward link + the head's prev */
					+ 1 /* §4.B parent guard */
					/* + count walk: the (merged_keys - cnt_dst) nr_keys ancestor
					 * edges (BULK fold), bounded by the merge-point depth */
					+ (dst_ft->rank_stats ? (int) dst_key_len + 1 : 0)
			/* the split-retire terminal's second word, if any */
			+ ft_glue_split_cn_reserve(dst_ft))) {
			ft_flip_txn_destroy(txn);
			txn = NULL;
		} else if (txn) {
			/*
			 * The txn was created here, with gd.cap_free
			 * free-list headroom reserved.  Fuse each
			 * dst-overlap retire's freeze into @txn (atomic
			 * detach, §4.B of
			 * doc/design/mcas-multiwriter-readiness.md).
			 * Then the whole retired dst spine freezes
			 * dead atomically with the forward publish
			 * that unlinks it.
			 */
			gd.fuse_free_list = true;
		}
	}
	if (!txn) {
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	ft_glue_set_publish(dst_ft, &gd, pub_parent, pub_slot, pub);
	/*
	 * The dst forward publish commits through @txn (step 4 below).  Point
	 * gd->txn at it.  Then ft_glue_apply_deferred's tombstone_free_list
	 * records the fused dst retires into the same txn.
	 */
	gd.txn = txn;

	/*
	 * @M_slot is the slot-canonical form of @pub for the @pub_slot stores.
	 * Those stores are the flip proxy's new target and the settle.  A
	 * compressed @pub is published SKIP-ENCODED, exactly as a direct slot
	 * write would store it.  So a reader resolving the proxy gets a value
	 * with the encoding of a normal slot read, and runs the same skip
	 * handling.  set_publish already wired @pub's parent + skip_slot via
	 * the plain flag.  An internal @pub needs no re-encode.  That is EXACT
	 * only: a compressed-wrap @pub is always compressed.
	 *
	 * @D_old is the flip proxy's old target: the value @pub_slot currently
	 * holds.  Read it from the live slot.  For the compressed shapes the
	 * descent resolved d->nf / d->pnf to the PLAIN flag, while the slot
	 * holds the SKIP form.  And *pub_slot is untouched until the flip.  The
	 * build is invisible, and apply_deferred wires back-pointers, not this
	 * forward slot.
	 */
	if (ft_node_compressed(pub))
		M_slot = ft_publish_compressed(dst_ft,
				ft_compressed_node_ptr(pub), pub);
	else
		M_slot = pub;
	/*
	 * A txn load, not a raw one.  @pub_slot is this flip's forward edge,
	 * and this txn may already hold a pending value for it.  See "LOADS
	 * INSIDE A TXN" in ft-txn-engine.h.
	 */
	D_old = ft_txn_load(txn->mtxn, (void **) pub_slot,
		FT_FLIP_PROXY_TAG);

	/*
	 * Ordered list: capture two cells while D is still intact.  The first
	 * is the dst merge subtree's min head, the cursor for the post-commit
	 * interleave walk.  The second is the region predecessor.  D is still
	 * intact because the build only copied its spine.  The flip below
	 * moves its leaves into M.  The surviving src cells are spliced in
	 * after the commit.
	 */
	if (ms_ord) {
		ms_cursor = ft_ord_cell_ptr(rcu_dereference(
			ft_subtree_minmax_head(dst_ft, D, false)->prev));
		ms_prev = ft_ord_cell_resolve_ord(&ms_cursor->lnode.prev);
		/*
		 * The dst region run is [@ms_cursor .. D's max head].  @ms_succ
		 * is the cell following it (NULL at the list tail).  It is the
		 * stop boundary for the interleave walk, and the trailing
		 * survivor's successor.
		 */
		ms_succ = ft_ord_cell_resolve_ord(&ft_ord_cell_ptr(rcu_dereference(
			ft_subtree_minmax_head(dst_ft, D, true)->prev))->lnode.next);
		/*
		 * Capture the endpoints of src's merged-subtree (S) run now,
		 * while S is still intact.  Defer the actual run-unlink until
		 * AFTER the last fallible step (ft_merge_unlink_src_subtree).
		 * The run unlink is an externally observable ordered-list
		 * mutation.  Doing it here would leave src's list inconsistent
		 * if the src subtree unlink below OOMs and we roll the whole
		 * merge back.  The unlink still happens before the drain, so
		 * sync drains src ord-readers of the run too.
		 */
		ms_s_first = ft_subtree_minmax_head(dst_ft, S, false);
		ms_s_last = ft_subtree_minmax_head(dst_ft, S, true);
		/*
		 * Pre-allocate the interleave's edge scratch NOW, while the
		 * build is still abortable.  The interleave is collected
		 * pre-commit.  Its proxies are recorded into the structural
		 * @txn, which is already reserved.  So the commit tail has no
		 * allocation left, and cannot degrade to a non-atomic per-edge
		 * fallback.  Each survivor run costs at most two visible edges,
		 * so @ms_cap (2*merged_keys+2) bounds @ms_edges.
		 */
		/*
		 * ☠ CALLOC, not malloc.  The producers fill these edges FIELD
		 * BY FIELD.  So anything that consumes a field no producer
		 * sets reads heap garbage.  That was harmless while the
		 * optional fields (@ctx, @owner_cell) were only read under a
		 * debug assert.  It became a SEGV the moment the recorder
		 * began deriving a cell's nearest-ancestor owner from them.
		 */
		ms_edges = calloc((size_t) ms_cap, sizeof(*ms_edges));
		if (!ms_edges) {
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/*
		 * Capture the src run's key SUFFIXES (below the src merge
		 * point) NOW, while S is still attached and up-walkable.  The
		 * post-commit interleave merges them against the LIVE dst
		 * suffixes to rebuild the merged order.  It does so WITHOUT
		 * walking the about-to-be-published merged structure, so no
		 * proxy need be installed before the collect.
		 *
		 * Capturing them here (not at the collect) is mandatory.
		 * ft_merge_unlink_src_subtree below leaves S-root's parent
		 * stale.  So a src up-walk after the unlink could run into
		 * freed / relocated structure.
		 *
		 * The run length is bounded by @cnt_src: the src subtree's key
		 * count is >= its distinct heads.  The variable-length suffix
		 * bytes pack into a realloc-growable pool.  The pool is
		 * addressed by offset, because a byte pointer would dangle
		 * across the realloc.
		 *
		 * This is a fallible pre-pass before the last fallible step.
		 * So any OOM aborts the still-invisible build, both tries
		 * pristine.
		 */
		ms_src_caps = malloc((size_t) (cnt_src + 8) * sizeof(*ms_src_caps));
		if (ms_src_caps) {
			struct ft_ord_cell *sc = ft_ord_cell_ptr(
				rcu_dereference(ms_s_first->prev));
			struct ft_ord_cell *slast = ft_ord_cell_ptr(
				rcu_dereference(ms_s_last->prev));
			size_t s_max_len = dst_ft->group->max_key_len;
			size_t pool_cap = 0, pool_len = 0;
			uint8_t sbuf[FT_MAX_KEY_LEN];
			bool oom = false;

			for (;;) {
				size_t sfl, suf_len;

				/* Premises, not asserts: see the helper. */
				FT_MERGE_SRC_CAPTURE_PREMISE(
					!ft_ord_is_end(src_ft, sc),
					"walked off the list before the last head",
					sc);
				FT_MERGE_SRC_CAPTURE_PREMISE(
					ms_nsrc < cnt_src + 8,
					"more heads than @cnt_src + 8", sc);
				sfl = ft_rebuild_key_upwalk(dst_ft, sc, sbuf,
						s_max_len);
				FT_MERGE_SRC_CAPTURE_PREMISE(
					sfl >= src_key_len,
					"a head key shorter than the merge point",
					sc);
				suf_len = sfl - src_key_len;
				if (pool_len + suf_len > pool_cap) {
					size_t ncap = pool_cap ? pool_cap * 2 : 256;
					uint8_t *np;

					while (ncap < pool_len + suf_len)
						ncap *= 2;
					np = realloc(ms_src_pool, ncap);
					if (!np) {
						oom = true;
						break;
					}
					ms_src_pool = np;
					pool_cap = ncap;
				}
				memcpy(ms_src_pool + pool_len,
					sbuf + (s_max_len - sfl) + src_key_len,
					suf_len);
				ms_src_caps[ms_nsrc].cell = sc;
				ms_src_caps[ms_nsrc].suffix_off = pool_len;
				ms_src_caps[ms_nsrc].suffix_len = suf_len;
				pool_len += suf_len;
				ms_nsrc++;
				if (sc == slast)
					break;
				sc = ft_ord_cell_resolve_ord(&sc->lnode.next);
			}
			if (oom) {
				free(ms_src_pool);
				ms_src_pool = NULL;
				free(ms_src_caps);
				ms_src_caps = NULL;
			}
		}
		if (!ms_src_caps) {
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	/*
	 * 1. Unlink the merge source from src.  Root src: swap in the pre-
	 *    allocated empty root.  Non-root src: detach its branch in place --
	 *    the LAST fallible step.  On its OOM both tries are pristine (the
	 *    detach self-undoes, the cluster is still unpublished), so abort the
	 *    build; this preserves the no-rollback property.  Then drain src
	 *    readers of the moved content.
	 *
	 *    ===== Everything from the drain onward is failure-free. =====
	 */
	struct ft_detach_run sdrun = { .into = NULL };

	if (ms_ord && !root_src) {
		/*
		 * Non-root src: fuse the src run-unlink into the structural
		 * unlink flip of ft_merge_unlink_src_subtree.  The run is
		 * EXCISE-ONLY (into == NULL): the run cells disperse to dst via
		 * the post-commit interleave.  The subpos src side does exactly
		 * the same.  This closes the src-side disappear window.
		 */
		sdrun.rfirst = ms_s_first;
		sdrun.rlast = ms_s_last;
	}
	/*
	 * Pre-reserve the root src's root + endpoint swap txn before the
	 * (still fallible) src unlink.  The swap is un-abortable once the
	 * structural unlink is public, so it commits through this
	 * pre-reserved txn.  OOM here aborts the still-invisible build (both
	 * tries pristine).  A non-root src needs none: its unlink folds the
	 * run's removal from src's list into its own commit (see the assert
	 * after it).  The lone-edge list-off paths (ft_root_edge_flip) need no
	 * txn either.
	 */
	struct ft_flip_txn *src_side_txn = NULL;

	/*
	 * Size the src-side commit txn per shape.  A root src fuses the gs
	 * free-list freeze into its root swap / lone root flip (atomic detach,
	 * doc §4.B): + gs.cap_free tombstone edges (cnt.nf_src + 8, the exact
	 * upper bound ft_glue_defer_free asserts against).  A non-root src's
	 * gs freeze stays standalone, applied below AFTER
	 * ft_merge_unlink_src_subtree's (still fallible) unlink so a rolled-back
	 * merge never freezes-then-strands (full non-root fusion -- into
	 * ft_detach_node's flip -- is a §4.B residual).  A list-off root with an
	 * EMPTY gs keeps its bare lone-edge store (no txn, no grace period).
	 * gs is fully populated by ft_merge_build above, so nr_free/cap_free are
	 * final here.  (gd is stamped in ft_glue_apply_deferred for the dst
	 * forward publish.)
	 */
	if (ms_ord && root_src) {
		src_side_txn = ft_flip_txn_create_bounded(src_ft,
			FT_ROOT_LIST_SWAP_MAX_EDGES + (unsigned int) gs.cap_free);
		if (!src_side_txn) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	} else if (root_src && gs.nr_free) {
		/* List-off root with retires: 1 root edge + gs.cap_free tombstones. */
		src_side_txn = ft_flip_txn_create_bounded(src_ft,
			1u + (unsigned int) gs.cap_free);
		if (!src_side_txn) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}

	/*
	 * Dup-chain lock-set (MW LOCK_FINE).  Step 3c appends collided src
	 * runs to dst chains.  Take the node lock of every distinct holder of
	 * such a chain.  The tail walk + append then run under the same
	 * per-node lock that insert, remove, promote and replace take on that
	 * chain.
	 *
	 * HERE is the last point where a miss is free.  Every fallible
	 * allocation above has succeeded, and the src unlink below is the point
	 * of no return.  So an -EAGAIN unwinds a build that is still entirely
	 * invisible.  The locks are held across the unlink, the drain and the
	 * single commit that installs the appends.  They are released just
	 * after it.
	 *
	 * UNDOING THE ATTEMPT IS TRIVIAL, which is what makes the bail cheap.
	 * cds_ft_merge_at consumes an EXCLUSIVE source, so nothing is published
	 * and nothing has moved.  Discarding the fresh cluster leaves both
	 * tries byte-for-byte as they were, and the caller just re-descends.
	 * That is why contention needs no place in enum cds_ft_status (see
	 * @contended).
	 *
	 * No-op on a non-lock_fine trie or a collision-free merge.  Every merge
	 * of disjoint key sets is collision-free, so the batch-staging workload
	 * pays nothing.
	 */
	if (ft_glue_acquire_splice_holders(dst_ft, &gd)) {
		free(ms_src_pool);
		free(ms_src_caps);
		free(ms_edges);
		ft_flip_txn_destroy(txn);
		if (src_side_txn)
			ft_flip_txn_destroy(src_side_txn);
		if (fresh_root)
			free_cds_ft_node_unpublished(src_ft, fresh_root);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		*contended = true;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}

	/*
	 * PRE-ACQUIRE THE PUBLISH TARGET, here, for the same reason: this is the
	 * last point at which failing to get it is free.
	 *
	 * The forward publish used to acquire @pub_parent at step 3, AFTER the
	 * src unlink, through ft_flip_txn_lock_or_guard_parent.  A miss there
	 * is not a failure that function can report.  It sets @acquire_miss,
	 * and the commit then ABORTS.  By then the src subtree is unlinked and
	 * the merged cluster is unpublished, so the keys exist in NEITHER trie.
	 * The commit status was ignored, which let this function return
	 * CDS_FT_STATUS_OK on top of it.  That was silent data loss with a
	 * success code, measured across most merge shapes.
	 *
	 * @pub_parent is above the overlap spine, so it is never in the fenced
	 * set.  It CAN be a dup-chain holder we just locked (an external dst
	 * merge point).  Then take that fence over.  Do not re-mark our own
	 * word: that is the self-deadlock this file has now hit twice.  Either
	 * way the holder rides @gd to the publish.  The publish records the
	 * {LOCK|s -> s} release directly, so it never consults lock_or_guard.
	 * No acquire is left to miss, so that abort cause is gone, not merely
	 * less likely.
	 *
	 * A miss here bails exactly like the splice acquire above: both tries
	 * pristine, contention reported, caller re-descends.
	 */
	if (dst_ft->lock_fine && pub_parent) {
		struct cds_ft_metadata *pm = ft_flag_to_metadata(dst_ft, pub_parent);
		uintptr_t psnap = 0;

		struct ft_lock_ctx pctx;
		struct ft_held_anchor ph;
		unsigned int pdep;
		bool took = ft_glue_splice_holder_take(&gd, pm, &psnap);

		ft_glue_lock_ctx(&gd, &pctx);
		if (!took && (!ft_lock_ctx_depth_of(dst_ft, &pctx, pub_parent,
					&pdep) ||
				ft_acquire_member(dst_ft, &pctx, pub_parent, pm,
					pdep, &ph))) {
			free(ms_src_pool);
			free(ms_src_caps);
			free(ms_edges);
			ft_flip_txn_destroy(txn);
			if (src_side_txn)
				ft_flip_txn_destroy(src_side_txn);
			if (fresh_root)
				free_cds_ft_node_unpublished(src_ft, fresh_root);
			ft_glue_abort(dst_ft, &gd);
			ft_glue_abort(src_ft, &gs);
			*contended = true;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		/*
		 * The holder is whichever word protects @pub_parent: the splice
		 * fence this op already took over, or the anchor just acquired.
		 */
		/*
		 * A SHARED acquire owes no release.  The claim above is that
		 * @pub_parent is above the overlap spine and so never in the
		 * fenced set.  It holds for the NODE and not for its ANCHOR.
		 * Coarsening can put that anchor on an overlap node this op has
		 * already fenced, and root-only puts EVERY member on one word.
		 * Claiming it here would record a second terminal on a word
		 * whose fenced retire already owns one.  A DEBUG_RCU build
		 * asserts on that in urcu_txn_sw_record_chain().  Under the
		 * MCAS engine it poisoned the descriptor.
		 */
		if (!took && ph.shared) {
			gd.publish_parent_holder = NULL;
			gd.publish_parent_snap = 0;
		} else {
			gd.publish_parent_holder = took ? pm : ph.lock;
			gd.publish_parent_snap = took ? psnap : ph.lock_snap;
		}
	}

	if (root_src) {
		/*
		 * A root src moves the WHOLE source, so its run is the whole
		 * src ordered list.  Fuse the src->root swap with the head/tail
		 * clear into ONE flip (ft_root_list_swap_publish).  Then a src
		 * reader never sees src structurally empty but its ordered-list
		 * front still populated.  The run cells keep their internal
		 * links: only the head/tail endpoints flip.  So the post-commit
		 * interleave still re-homes them to dst.
		 */
		if (ms_ord) {
			/*
			 * Empty src's sentinel (relink_dest NULL).  The
			 * post-commit interleave re-homes the run cells into
			 * dst.  It sets each survivor's links individually.
			 * Collided heads are freed.
			 *
			 * Fuse the gs free-list freeze into this root-swap txn
			 * (atomic detach, doc §4.B).  First record each retired
			 * src-overlap node's tombstone into src_side_txn.  Then
			 * the swap records the root + sentinel endpoint edges
			 * into the same txn and commits them all in ONE flip.
			 * An empty gs records nothing.
			 */
			gs.txn = src_side_txn;
			gs.fuse_free_list = true;
			ft_glue_tombstone_free_list(&gs);
			ft_root_list_swap_publish(src_ft, src_side_txn,
				&src_ft->root,
				src_ft->root, ft_node_flag(fresh_root, 0),
				ft_ord_first(src_ft), NULL,
				ft_ord_last(src_ft), NULL, NULL, false);
			src_side_txn = NULL;	/* consumed */
		} else if (gs.nr_free) {
			/*
			 * List-off root with retires: fuse the gs free-list
			 * freeze into the root flip (atomic detach, doc §4.B).
			 * Record the lone root edge + each retired node's
			 * tombstone into src_side_txn, and commit them in ONE
			 * flip.  This is a group flip, like the list-on path.
			 * It replaces a bare store followed by standalone
			 * freezes.  The root edge normalizes to the same
			 * FT_FLIP_PROXY_TAG the swap uses.
			 */
			/* Stage 5: the root's own lock, so this record can park SW. */
			(void) ft_root_lock_take(src_ft, NULL, src_side_txn);
			ft_flip_txn_record_root(src_side_txn,
				(void **) &src_ft->root,
				(void *) src_ft->root,
				(void *) ft_node_flag(fresh_root, 0));
			gs.txn = src_side_txn;
			gs.fuse_free_list = true;
			ft_glue_tombstone_free_list(&gs);
			ft_flip_txn_commit_unfailable(src_ft, src_side_txn);
			src_side_txn = NULL;	/* consumed */
		} else {
			/*
			 * No ordered list, no retires: src->root is the only
			 * reader-visible slot.  Express the lone root edge as a
			 * single-edge flip descriptor.  It is one release
			 * store, like a bare rcu_assign_pointer.  This keeps
			 * the swap MCAS-expressible like the list-on path.
			 */
			ft_root_edge_flip(src_ft, &src_ft->root,
				src_ft->root, ft_node_flag(fresh_root, 0));
		}
		FT_TP(root_publish, (const void *) src_ft, (const void *) src_ft->root);
	} else if (ft_merge_unlink_src_subtree(src_ft, src_key, src_key_len,
				cnt_src, ms_ord ? &sdrun : NULL, &gs) < 0) {
		/*
		 * OOM in the last fallible step.  @src_ft is left pristine: the
		 * run was not yet applied.  The run commits in ft_detach_node's
		 * flip, which is past the fallible alloc.  So abort the
		 * still-invisible build.
		 */
		free(ms_src_pool);
		free(ms_src_caps);
		free(ms_edges);
		ft_flip_txn_destroy(txn);
		if (src_side_txn)
			ft_flip_txn_destroy(src_side_txn);
		ft_glue_abort(dst_ft, &gd);
		ft_glue_abort(src_ft, &gs);
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	/*
	 * Non-root src: gs (the src overlap-spine free-list) is now frozen dead
	 * ATOMICALLY with the unlink.  ft_merge_unlink_src_subtree threaded gs
	 * into ft_detach_node as its @retire_glue.  So each retired node's
	 * tombstone rode the SAME commit_txn flip that unlinked S (doc §4.B
	 * atomic detach).  On the OOM abort above, ft_detach_node left gs
	 * unmarked: src stays pristine and gs is still live.  A root src froze
	 * gs fused into its root swap / flip above.
	 */
	/*
	 * The run of src's merged subtree (S) is out of src's ordered list
	 * now.  A root src removed it in its root swap above.  A non-root src
	 * removed it in the unlink's own commit: ft_detach_node arms @sdrun on
	 * every success.  The one exception replaces a compressed trie ROOT,
	 * which never exists at rest (root-is-internal invariant, ft-helpers.h).
	 * Every root path consumed @src_side_txn.
	 */
	assert(!ms_ord || root_src || sdrun.armed);
	assert(!src_side_txn);
	/*
	 * NO SRC DRAIN.  This worker is now CROSS-TRIE ONLY: the same-trie
	 * rekey moved to ft-rekey.h.  The gates of ft_merge_at_inner leave
	 * both src_ft != dst_ft and src_ft->exclusive holding unconditionally
	 * by the time it calls here.  An exclusive trie has no concurrent
	 * reader, so there is nothing to wait out.  The grace period that used
	 * to stand here served the same-trie caller, which no longer reaches
	 * this body.
	 *
	 * The precondition is asserted, not deleted outright, so that it stays
	 * VISIBLE.  Otherwise a future caller that reaches this worker with a
	 * shared src would silently skip a drain it needs.
	 */
	assert(src_ft->exclusive);

	/*
	 * 2. Re-parent the SRC-origin referenced subtrees directly.  The src
	 *    trie is exclusive (asserted above), so no reader sees them.  The
	 *    cluster is not yet forward-published either, so this is
	 *    invisible.  dst-origin edges are NOT applied here -- they go
	 *    through the flip.
	 */
	ft_glue_apply_deferred(dst_ft, &gd);

	/*
	 * 3. Record the structural edges into @txn (still PREPARE): every
	 *    dst-origin child's parent re-parent and the merge-point forward slot.
	 *    ft_glue_record_back_edge mirrors ft_set_parent's child-kind
	 *    dispatch.  It reads the field's current value as the old target,
	 *    and records the parent slot offset with it.  The commit's
	 *    settle below writes each field to its new parent.  That subsumes
	 *    the old step-7 ft_set_parent.  Nothing is parked yet.  Commit's
	 *    auto-install stages every proxy (selector 0, resolving to old).
	 *    So up-walks and the root descent still see the pre-merge dst
	 *    until the flip.
	 */
	{
		int j;

		for (j = 0; j < gd.nr_deferred; j++) {
			if (!gd.deferred[j].dst_origin)
				continue;
			ft_glue_record_back_edge(dst_ft, txn,
				gd.deferred[j].child, gd.deferred[j].parent,
				gd.deferred[j].slot);
		}
		/* Forward publish: old dst subtree -> merged cluster. */
		/*
		 * VALIDATE (§4.B) / LOCK_FINE (step 6, §9.4 M-2): acquire the
		 * LIVE dst parent above the overlap spine as a RELEASE lock.
		 * The forward publish is a same-slot REPLACE (nr_child
		 * invariant, §9.4 finding 1).  So pub_parent is a value-swap
		 * survivor that is not recompacted, and guard-fallback on an
		 * acquire miss is correct.  Non-lock_fine falls to the §4.B
		 * guard.
		 *
		 * UNLESS WE ALREADY HOLD IT.  When the dst merge point is an
		 * EXTERNAL node, the collided head IS that node.  Its chain
		 * holder is its immediate parent, @pub_parent.  Re-marking a
		 * word this op already fenced MISSES.  A miss now sets
		 * acquire_miss and ABORTS a commit with no bail path left (the
		 * src is already unlinked).  Hand the held fence to the txn
		 * instead.  hold_or_lock records the {LOCK|s -> s} release,
		 * the guard's strictly stronger twin.  The txn owns the unlock
		 * from here, so the take clears our entry.
		 */
		/*
		 * The holder was acquired BEFORE the src unlink (see the
		 * pre-acquire above).  It was acquired either by marking
		 * @pub_parent here, or by taking over the dup-chain fence when
		 * the two coincide.  Recording its release directly keeps this
		 * publish off lock_or_guard.  A miss in lock_or_guard would
		 * abort a commit that has no way left to fail safely.  A NULL
		 * holder means non-lock_fine.  That
		 * routes to the ordinary acquire-or-guard exactly as before.
		 */
		{
			struct ft_lock_ctx mctx;

			ft_glue_lock_ctx(&gd, &mctx);
			ft_flip_txn_hold_or_lock_parent(dst_ft, txn, &mctx,
				pub_parent, FT_DEPTH_FROM_DESCENT,
				gd.publish_parent_holder,
				gd.publish_parent_snap);
			/*
			 * No @pub_parent: the slot is &dst_ft->root, and the
			 * root's own lock is P's -- taken where P's is.
			 */
			if (pub_slot == &dst_ft->root)
				ft_flip_txn_lock_root_unfailable(txn, &mctx,
					(void **) pub_slot, D_old);
		}
		/*
		 * OWNERSHIP TRANSFER (mirrors the graft): @txn's registry now
		 * owns this fence.  A commit consumes it through the recorded
		 * release, and an abort clears it.  So drop the glue's claim.
		 * There is no bail left between here and the commit.  But a
		 * stale holder left behind is the foot-gun that makes the NEXT
		 * bail added here a double clear.
		 */
		gd.publish_parent_holder = NULL;
		gd.publish_parent_snap = 0;
		/*
		 * @pub_slot is d_dst->nfp: &dst_ft->root at depth 0 (whose
		 * record is always MW and ignores the owner), and a child slot
		 * inside @pub_parent below it -- the node the hold_or_lock
		 * above put in this txn's registry.
		 */
		ft_flip_txn_record_publish(txn, dst_ft,
			pub_parent ? ft_flag_to_metadata(dst_ft, pub_parent) :
				NULL,
			pub_slot, D_old, M_slot);
	}

	/*
	 * 3b. Ordered list: collect the interleave and record its cell edges into
	 *    the SAME txn, STILL IN PREPARE -- no install before the collect.
	 *    The merged order is reconstructed by a two-pointer merge of the
	 *    two LIVE cell runs:
	 *    - the dst region, up-walked over the intact D;
	 *    - the src run's suffixes, captured pre-unlink.
	 *    So the collect does NOT need the about-to-be-published merged
	 *    structure.  The last append-after-install is gone, and every edge
	 *    is recorded before install.
	 *
	 *    Structure + ordered list thus commit in ONE flip: commit
	 *    auto-installs every recorded proxy, then flips.  A reader never
	 *    sees the merged structural minimum ahead of the ordered-list
	 *    front.  The collect pre-sets the surviving cells' own links
	 *    invisibly (not yet ord-reachable) and returns only the visible
	 *    boundary edges.
	 *
	 *    The collect runs before the splice fold (step 3c), but collisions
	 *    are invariant.  A collided src head is a floating duplicate, only
	 *    in the splice record.  It is never a distinct reachable head.  So
	 *    the merge enumerates the same heads either way.  And 3c reads each
	 *    demoted head's cell from its still-intact src-run prev, which this
	 *    collect leaves untouched.
	 */
	if (ms_ord) {
		ms_n = ft_merge_ord_interleave_collect(dst_ft, dst_key_len,
			ms_cursor, ms_succ, ms_prev, /*tail_link=*/ ms_succ,
			/*head_linked=*/ true, ms_src_caps, ms_nsrc,
			ms_src_pool, ms_edges, /*record_all=*/ false,
			/*ncollide=*/ NULL, /*collided=*/ NULL);
		/*
		 * ☑ ROUTED THROUGH THE TAG-DISPATCHING RECORDER, which is what
		 * the comment that stood here asked for and what the sibling
		 * GRAFT path has always done.
		 *
		 * This used to be a RAW record_tag loop, so every edge took the
		 * structural_sw dispatch and a CELL edge parked SW under an
		 * armed txn even though no cell carries a node lock -- sound
		 * only because the modes that arm (COARSE, exclusive) exclude
		 * trie-wide.  What stopped it at the Phase B arm was the
		 * per-edge @owner: a cell's owner is NULL, so the record-time
		 * check refused the park rather than taking it silently, and
		 * every one of those refusals landed in OWN_MISS.
		 *
		 * ft_ord_cell_record_into_ft dispatches BY TAG instead: a
		 * structural edge keeps the owner-checked path, and a CELL /
		 * hlist edge goes to record_tag_excluded, the lane for words
		 * that no node lock covers.
		 * So the edges stop being counted as an exclusion gap because
		 * they stop being ON the conversion surface, which is the
		 * honest answer rather than a suppressed counter.
		 *
		 * MEASURED: ft-merge.h's REAL OWN_MISS (the population left
		 * after the doomed-commit split) 40,280 -> 0 per ft_inv leg.
		 */
		ft_merge_take_interleave_locks(dst_ft, &gd, txn, ms_edges, ms_n);
		ft_ord_cell_record_into_ft(dst_ft, txn, ms_edges, ms_n);
	}

	/*
	 * 3c. Duplicate-chain concatenation: fold each collided src run's
	 *    tail-append into the SAME txn, still in PREPARE.
	 *
	 *    It is recorded AFTER the interleave collect (3b).  So each demoted
	 *    src head's cell is captured from its intact src-run prev before
	 *    the append overwrites it.
	 *
	 *    It is recorded BEFORE the commit.  So the src duplicates become
	 *    reachable ATOMICALLY with the merged structure.  A collided key
	 *    never momentarily shows only its dst duplicates (the old
	 *    post-commit apply's window).
	 *
	 *    Each splice is one forward edge (dst tail -> src run), so only the
	 *    tail carries a proxy; src_head->next rides along.
	 *
	 *    Every append here runs under the chain holder's node lock.
	 *    ft_glue_acquire_splice_holders takes it before the point of no
	 *    return, and it is released after the commit below.  So the walk to
	 *    the tail cannot race a peer's append/unchain/promote on the same
	 *    chain.
	 */
#ifdef FT_DEBUG_SPLICE_SEAM
	{
		uatomic_inc(&ft_ss_site_merge_calls);
		if (gd.nr_splices)
			uatomic_inc(&ft_ss_site_merge);
	}
#endif
	ft_glue_record_splices(dst_ft, &gd, txn);

	/*
	 * Order-statistics fold (BULK).  The dst net key-count delta is
	 * (merged_keys - cnt_dst).  Record its walk from @pub_parent up into
	 * the SAME txn.  Then the aggregate flips ATOMICALLY with the merged
	 * spine's forward publish (step 4), which is exact under concurrent
	 * writers.  @pub is a fresh node already carrying merged_keys, so the
	 * walk begins one level up at the stable @pub_parent.  A no-op when
	 * rank stats off or the count is unchanged.
	 */
	if (pub_parent && merged_keys != cnt_dst)
		ft_flip_txn_record_count_parent(dst_ft, txn, pub_parent,
			(long) merged_keys - (long) cnt_dst);

	/*
	 * 4. Commit: one selector flip atomically switches every dst-origin
	 *    parent, the forward slot AND every interleave cell edge from old
	 *    to merged.  It then settles each slot to its direct merged target.
	 *    (With the list off, this can reduce to a single release store of
	 *    the forward slot.  That needs no dst-origin re-parent and no
	 *    collided splice.  Then there is no proxy and no grace period.)
	 *    The forward slot flips with the back-pointers, so a reader
	 *    (descend then up-walk) only progresses old->merged.  And the
	 *    ordered-list front advances in the same instant the merged
	 *    minimum becomes reachable.
	 *
	 *    The src is unlinked by now, so this commit cannot fail safely.
	 *    A failure would lose the src keys and free live dst nodes.  The
	 *    locks, the records and the edge scratch were all taken before
	 *    the unlink, and the bulk gate excludes every dst peer writer.
	 */
	ft_flip_txn_commit_unfailable(dst_ft, txn);

	/*
	 * 5. Drop the dup-chain holder locks: the appends are installed, so peers
	 *    may mutate those chains again.  Do it BEFORE the step-7 reclaim
	 *    below.  Most holders are dst overlap-spine nodes this merge
	 *    retires, and the fence has to come off while the node is still
	 *    there to clear.  (The fence survives the commit: the free-list
	 *    retire records a plain {LOCK|s -> LOCK|s|TOMBSTONE} upgrade.  The
	 *    one holder that IS the publish target was handed to @txn above
	 *    and is already released by its flip.)
	 */
	ft_glue_release_splice_holders(&gd);
	/*
	 *    Same for the overlap-spine plan-locks.  A COMMITTED fenced retire
	 *    consumed each fence into TOMBSTONE, so the sweep releases nothing.
	 *    Before the step-7 reclaim, while the nodes are still addressable.
	 */
	ft_glue_clear_fenced(&gd, /*committed=*/ true);

	/*
	 * 6. The dst net key-count delta (merged_keys - cnt_dst) is FOLDED into
	 *    the step-4 commit above.  It was recorded from @pub_parent into
	 *    @txn before the flip.  So it goes live ATOMICALLY with the merged
	 *    spine, with no post-commit propagate walk.
	 */

	/*
	 * 7. Reclaim the old overlap spines (src-side to src, dst-side to dst).
	 *    The flip-txn was already committed-and-reclaimed at step 4.  The
	 *    commit settled every proxied slot to its direct merged target:
	 *    the dst-origin parents, the forward slot and the interleave cell
	 *    edges.  So the proxies are unreferenced.  The txn's parked block
	 *    is deferred through the FT flavor, or freed in place when the
	 *    commit owed no grace period.  No dst synchronize_rcu: the flip
	 *    subsumed the dst drain.
	 */
	ft_glue_free_old(src_ft, &gs);
	ft_glue_free_old(dst_ft, &gd);

	/*
	 * 8. Free the collided (demoted) src heads' cells.  The interleave
	 * (folded into the flip above) already rewired the surviving cells'
	 * stale src-run links away from these cells.  So they are unreachable
	 * to new readers.  The grace-period defer inside the free covers
	 * readers already holding such a link or parked on a demoted head.
	 */
	if (ms_ord) {
		free(ms_edges);
		free(ms_src_caps);
		free(ms_src_pool);
	}
	ft_glue_free_collided_cells(dst_ft, &gd);

	ft_glue_fini(&gd);
	ft_glue_fini(&gs);
	return CDS_FT_STATUS_OK;
}

/*
 * Graft a SUB-position source subtree into a dst position that is ABSENT at
 * @dst_key (the diverged-dst case).  The graft is build-invisible and does
 * NOT re-root the payload.  It is the leak-free reorder for cds_ft_merge_at's
 * residual shapes.
 *
 * The commit is modeled on ft_graft_keylen's.  The source side is
 * ft_merge_spine_copy's:
 * - the payload stays @payload (= d_src->nf).  It is referenced in place,
 *   not detached into a fresh root;
 * - ft_merge_unlink_src_subtree is the single last fallible step.  It
 *   self-undoes on OOM, preserving @payload.
 * The dst attach is built invisibly BEFORE that unlink and published
 * failure-free AFTER the drain.  So nothing can strand the moved externals
 * on an allocation failure.  There is no rollback, hence no leak (contrast
 * the legacy detach-then-graft fallback).
 *
 * Covers both diverged-dst shapes:
 * - a GLUE diverge: the dst key diverges inside a compressed node.  So
 *   ft_graft_build assembles the whole split cluster invisibly, with no
 *   reserve;
 * - a NOSPLIT point: an empty slot at @dst_key, or a built branch /
 *   displaced external.  ft_store_at_graft_point grafts it post-drain.  It
 *   draws from a pre-filled reserve, so it cannot fail on the arena.
 * *handled is always set true here, so the caller never falls through.
 *
 * @okey_dst / @okey_src are ORDINAL.  @dst_key is the APPLICATION dst key
 * (the ordered-list splice-pos descent remaps it).
 *
 * @payload is the source subtree to move, with @cnt_src unique keys:
 * - d_src->nf for an EXACT source;
 * - cn_s->child for a KEY_SHORTER source.  Then @okey_dst / @dst_key /
 *   @dst_key_len are already EXTENDED by the residual cn_s bytes (the
 *   caller's reduction to EXACT).
 * @okey_src / @src_key_len remain the ORIGINAL source key (the unlink
 * overshoots cn_s on its own).
 */
static
enum cds_ft_status ft_merge_graft_subpos_inplace(struct cds_ft *dst_ft,
		struct cds_ft *src_ft,
		const uint8_t *okey_dst, const uint8_t *dst_key, size_t dst_key_len,
		const uint8_t *okey_src, size_t src_key_len,
		struct cds_ft_inode_flag *payload, unsigned long cnt_src,
		bool *handled)
{
	struct ft_glue glue;
	struct cds_ft_alloc_reserve reserve;
	struct ft_descent d;
	enum ft_graft_prep prep;
	struct cds_ft_inode_flag *attached_nf;
	bool ms_ord = dst_ft->group->ordered_list_set;
	struct ft_ord_cell *pred = NULL, *succ = NULL;
	struct ft_ord_cell *run_first = NULL, *run_last = NULL;
	struct cds_ft_node *s_first = NULL, *s_last = NULL;
	struct ft_graft_run mrun;
	struct ft_graft_run *run_arg = NULL;
	struct ft_detach_run srun = { .into = NULL, .armed = false };
	struct ft_detach_run *srunp = NULL;
	size_t src_max, nm, dm;
	/*
	 * MW LOCK_FINE drop: the "failure-free" commit below is only
	 * unfailable under the FT-wide lock.  Under the drop, its MCAS commit
	 * (GLUE) or its recompact prepare (NOSPLIT) can conflict with a peer.
	 * It then fails AFTER @ft_merge_unlink_src_subtree already excised the
	 * source subtree.  That unlink PRESERVES @payload (owned by this op,
	 * src pristine-empty).  So the whole dst-side attach (re-descend ->
	 * build -> commit) is a retryable one-shot:
	 * - @already_unlinked guards the src-side excise + the ordered-list
	 *   run capture, so a retry re-runs only the dst attach;
	 * - @glue.fence_split_cn fences the compressed divergence node (as
	 *   cds_ft_graft), so a concurrent grow of it is arbitrated (fence
	 *   miss -> FT_GRAFT_PREP_RETRY -> re-descend).
	 * Mirrors cds_ft_graft's post-swap store retry.
	 */
	bool already_unlinked = false;
	struct ft_flip_txn *run_splice_txn = NULL;

	*handled = false;

	/*
	 * Classify the dst graft point with a build-invisible prep.  A GLUE
	 * diverge builds the whole split cluster into @glue (referencing @payload
	 * by deferred edge), published failure-free after the drain.  A NOSPLIT
	 * point is grafted post-drain by ft_store_at_graft_point drawing from a
	 * pre-filled reserve, so it likewise cannot fail on an arena allocation.
	 * Either way the source unlink is the single last fallible step, so an OOM
	 * leaves both tries pristine -- no rollback, no leak.
	 */
	unsigned long rm_depth __attribute__((unused)) = 0;
	/*
	 * PERSISTENT OP HANDLE for this retry loop.  Without one, every attempt
	 * built its transaction with the UNBOUND ft_flip_txn_create().  That
	 * gives a fresh handle whose retry age restarts at zero.  So
	 * ft_op_conflict() never advances it, ft_op_earned_lane() is never
	 * reached, and the writer can never take its per-trie FIFO turn.  A
	 * contended writer then livelocks by construction:
	 * ft_flip_txn_create's own docstring says so.  This loop was measured
	 * spinning 266-587 attempts deep on ONE op in runs that PASS, with the
	 * tail reaching 1036 under load.  The sibling loops that already have
	 * a handle top out around 90 on the same workload.
	 *
	 * THE BRACKET IS CONDITIONAL.  @rm_open is a VARIABLE, not a re-test
	 * of @rm_bracket at each exit.  A re-test is a second chance to
	 * disagree with the arm actually opened.
	 *
	 * WHY THE BODY MAY BE BRACKETED AT ALL.  ft_op_begin() enters the RCU
	 * read side, and ft_op_conflict() ages into the trie's FIFO
	 * lane -- where ft_writer_lock_gp_wait asserts !ft_op_in_lane().
	 * Either one is fatal over a grace period, so the body must take none.
	 * This was established by REACHABILITY CLOSURE over the whole
	 * translation unit, not by grep.  The seed is {ft_writer_lock_gp_wait,
	 * ft_move_gate_enter}.  These are the only two functions in the FT that
	 * reach update_synchronize_rcu.  They reach it INDIRECTLY through the
	 * flavor struct, which no call graph sees.  Taking the fixpoint gives
	 * 19 GP-reaching functions FT-wide.  This function reaches 432, and the
	 * intersection is EMPTY.  The same tool reproduces ft_graft_keylen's
	 * known answer, its own direct ft_writer_lock_gp_wait.  That is what
	 * makes the empty set here credible, not merely convenient.
	 *
	 * @dst_ft->lock_fine is the contract the class uses, and the source is
	 * always exclusive at this label.  The coarse arm is left unbracketed
	 * on purpose.  There ft_merge_at_inner holds the FT-wide writer lock
	 * across this whole loop.  So no peer can make its commit abort, and
	 * there is nothing for an escalation turn to win.
	 */
	const bool rm_bracket = dst_ft->lock_fine;
	struct ft_op optxn;
	bool rm_open = false;

	if (rm_bracket)
		ft_txn_op_init(dst_ft, &optxn);
retry_merge:
	if (rm_bracket) {
		ft_op_begin(&optxn);
		rm_open = true;
	}
	RSPIN_ENTER_X(0, rm_depth, 0, rm_bracket);
	RSPIN_SITE_ENTER(0, rm_depth, rm_bracket);
	ft_glue_init(&glue);
	glue.op = &optxn;
	/*
	 * Fence the compressed divergence node (like cds_ft_graft).  Then a
	 * concurrent grow of it is arbitrated, and the build re-descends on a
	 * miss (FT_GRAFT_PREP_RETRY).  ft_glue_init reset it, so set it on
	 * each attempt.
	 */
	glue.fence_split_cn = true;
	memset(&reserve, 0, sizeof(reserve));
	/*
	 * Every attach shape commits through @glue.txn, exactly like
	 * cds_ft_graft.  The shapes are the GLUE diverge and the NOSPLIT
	 * in-place store.  The build (below) tags its displaced old child
	 * dst_origin.  The NOSPLIT store reserves its slot proxy + run-splice
	 * edges.  All of this comes out of this txn.  Create it BEFORE the
	 * build.  It is destroyed on a POPULATED point (nothing to commit).
	 */
	glue.txn = ft_flip_txn_create(dst_ft);
	if (!glue.txn || !ft_flip_txn_reserve(glue.txn,
			/* +1: fused recompact-relocate tombstone (§4.B);
			 * + FLOOR_FREE: fused free-list tombstones;
			 * + count walk: the +cnt_src nr_keys ancestor edges (BULK fold) */
			FT_GLUE_FLOOR_DEFERRED + 7 + 1 /* +1 §4.B parent guard */ + FT_GLUE_FLOOR_FREE
				+ (dst_ft->rank_stats ? (int) dst_key_len + 1 : 0))) {
		if (glue.txn)
			ft_flip_txn_destroy(glue.txn);
		ft_glue_fini(&glue);
		if (already_unlinked) {
			ft_txn_attempt_bail(&optxn, rm_open);
			rm_open = false;
			goto retry_merge;	/* src consumed: OOM is transient */
		}
		ft_txn_attempt_end(&optxn, rm_open);
		rm_open = false;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	glue.fuse_free_list = true;	/* reserved free-list headroom above (§4.B) */
	prep = ft_graft_build(dst_ft, okey_dst, dst_key_len, payload, cnt_src,
			&d, &glue, /*outer*/ NULL);
	*handled = true;
	if (prep == FT_GRAFT_PREP_RETRY) {
		/*
		 * Compressed-divergence @cn fence miss: a peer owns @cn.  Nothing
		 * built; a clean re-descend (pre-unlink both tries pristine, post-
		 * unlink src is empty + @payload owned).
		 */
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		ft_txn_attempt_bail(&optxn, rm_open);
		rm_open = false;
		goto retry_merge;
	}
	if (prep == FT_GRAFT_PREP_OOM) {
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		if (already_unlinked) {
			ft_txn_attempt_bail(&optxn, rm_open);
			rm_open = false;
			goto retry_merge;	/* src consumed: OOM is transient */
		}
		ft_txn_attempt_end(&optxn, rm_open);
		rm_open = false;
		return CDS_FT_STATUS_MEMORY_ERROR;	/* both tries pristine */
	}
	if (prep == FT_GRAFT_PREP_POPULATED) {
		/*
		 * Defensive: cnt_dst == 0 should never yield an occupied point.
		 * Impossible post-unlink (the payload is already excised + owned;
		 * no clean rc remains) -- assert rather than orphan it.
		 */
		assert(!already_unlinked);
		ft_flip_txn_destroy(glue.txn);
		ft_glue_fini(&glue);
		ft_txn_attempt_end(&optxn, rm_open);
		rm_open = false;
		return CDS_FT_STATUS_POPULATED_ERROR;
	}
	if (prep == FT_GRAFT_PREP_NOSPLIT) {
		/*
		 * Pre-fill the store's node reserve while both tries are still
		 * pristine.  The store's slot proxy + run-splice edges draw from the
		 * already-reserved @glue.txn, so no separate flip batch is needed.
		 * Securing the reserve here, before the source unlink, makes
		 * the post-drain store wholly failure-free.  So the unlink is
		 * the true last fallible step and nothing strands.
		 *
		 * Use the GENEROUS superset fill, not a hand-rolled exact
		 * manifest.  ft_graft_keylen's NOSPLIT attach does the same for
		 * the same ft_store_at_graft_point.  Predicting the store's
		 * node set is fragile.  For example, attaching a high byte
		 * forces a RANGE recompact of the attach node even within its
		 * child capacity.  An exact count heuristic misses that grow:
		 * it underflowed the reserve and aborted.  A bulk op already
		 * pays a grace period, so the throwaway pops/pushes are
		 * negligible.
		 */
		if (ft_bulk_node_reserve_fill(dst_ft, &reserve)) {
			cds_ft_alloc_reserve_drain(dst_ft, &reserve);
			ft_glue_abort(dst_ft, &glue);
			ft_flip_txn_destroy(glue.txn);
			if (already_unlinked) {
				ft_txn_attempt_bail(&optxn, rm_open);
				rm_open = false;
				goto retry_merge;	/* src consumed: transient */
			}
			ft_txn_attempt_end(&optxn, rm_open);
			rm_open = false;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
	}
	/* GLUE: cluster built invisibly.  NOSPLIT: reserve secured (+ glue.txn). */

	/*
	 * Graft-parity (cds_ft_graft hoists the same test pre-swap): an exact-
	 * depth OCCUPIED NOSPLIT point is a PERMANENT POPULATED condition.  Catch
	 * it HERE, on the FIRST pass, BEFORE the irreversible source unlink.
	 * Then a misusing (non-disjoint) caller gets a clean POPULATED_ERROR
	 * with BOTH tries pristine.  Otherwise it would reach the post-unlink
	 * store, assert, and strand @payload.  For the supported
	 * absent/diverged dst point this is dead code.  That point is d.nf ==
	 * NULL at key_len, the only shape this helper is dispatched for.  A
	 * retry (post-unlink) cannot newly occupy the point under the disjoint
	 * exclusive-src contract.  So the store's own POPULATED discrimination
	 * (below) stays a dead assert there too.
	 */
	if (!already_unlinked && prep == FT_GRAFT_PREP_NOSPLIT
			&& d.depth == dst_key_len && d.nf) {
		cds_ft_alloc_reserve_drain(dst_ft, &reserve);
		ft_glue_abort(dst_ft, &glue);
		ft_flip_txn_destroy(glue.txn);
		ft_txn_attempt_end(&optxn, rm_open);
		rm_open = false;
		return CDS_FT_STATUS_POPULATED_ERROR;
	}

	/*
	 * Ordered list: locate the dst splice neighbours.  They are RE-FOUND
	 * on each attempt, because dst may have grown across a retry.  On the
	 * FIRST attempt only, capture the source subtree's run endpoints +
	 * cells while the subtree is still intact in src.  @run_first /
	 * @run_last / @s_first / @s_last persist across retries.  The unlink
	 * below excises the run ONCE.  The run is then owned (out of both
	 * lists), so a retry re-splices the same owned run.
	 */
	if (ms_ord) {
		ft_ord_cell_find_splice_pos(dst_ft, dst_key, dst_key_len,
			&pred, &succ, NULL);
		if (!already_unlinked) {
			s_first = ft_subtree_minmax_head(src_ft, payload, false);
			s_last = ft_subtree_minmax_head(src_ft, payload, true);
			run_first = ft_ord_cell_ptr(rcu_dereference(s_first->prev));
			run_last = ft_ord_cell_ptr(rcu_dereference(s_last->prev));
			/* Src side: EXCISE-ONLY run so the structural unlink and the
			 * run's removal from src's ordered list commit in ONE flip
			 * (the disappear-side cross-view fix). */
			srun.rfirst = s_first;
			srun.rlast = s_last;
			srunp = &srun;
		}
		mrun.run_first = run_first;
		mrun.run_last = run_last;
		mrun.pred = pred;
		mrun.succ = succ;
		mrun.armed = false;
		run_arg = &mrun;
	}

	/*
	 * Source-side excise (ONE-SHOT, @already_unlinked):
	 * - reserve the run txns;
	 * - unlink the source subtree in place (preserving @payload);
	 * - remove the run from src's list;
	 * - drain src readers;
	 * - stamp an external payload's edge byte.
	 * A retry after a post-unlink commit abort skips all of this: src is
	 * already empty and @payload is owned.  It re-runs only the dst attach.
	 */
	if (!already_unlinked) {
		if (ms_ord) {
			/*
			 * Pre-reserve the standalone run-splice txn (a rare
			 * unfused shape touches dst's list AFTER the structural
			 * flip is public, un-abortable).  OOM here aborts the
			 * still-invisible build (both tries pristine).
			 */
			run_splice_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_ORD_CELL_RUN_SPLICE_MAX_EDGES);
			if (!run_splice_txn) {
				cds_ft_alloc_reserve_drain(dst_ft, &reserve);
				ft_glue_abort(dst_ft, &glue);
				ft_flip_txn_destroy(glue.txn);
				ft_txn_attempt_end(&optxn, rm_open);
				rm_open = false;
				return CDS_FT_STATUS_MEMORY_ERROR;
			}
		}

		/*
		 * Last fallible SRC step: unlink the source subtree in place,
		 * preserving @payload.  On OOM the unlink self-undoes (src pristine)
		 * and the still-invisible cluster / reserve is released -- no rollback.
		 */
		if (ft_merge_unlink_src_subtree(src_ft, okey_src, src_key_len,
				cnt_src, srunp, /*retire_glue=*/ NULL) < 0) {
			if (run_splice_txn)
				ft_flip_txn_destroy(run_splice_txn);
			run_splice_txn = NULL;
			cds_ft_alloc_reserve_drain(dst_ft, &reserve);
			ft_glue_abort(dst_ft, &glue);
			ft_flip_txn_destroy(glue.txn);
			ft_txn_attempt_end(&optxn, rm_open);
			rm_open = false;
			return CDS_FT_STATUS_MEMORY_ERROR;
		}
		already_unlinked = true;	/* POINT OF NO RETURN: @payload owned */

		/*
		 * The structural unlink removed the source run from src's
		 * ordered list in its own commit.  The cells keep their
		 * internal links for the dst splice.  See
		 * ft_merge_spine_copy's assert for why @srun is always armed.
		 */
		assert(!ms_ord || srun.armed);

		/* No src drain: cross-trie only, src is exclusive.  See
		 * ft_merge_spine_copy's assert for the full reasoning. */
		assert(src_ft->exclusive);

		/*
		 * An EXTERNAL payload's edge byte changes (src_key's last byte
		 * -> dst_key's).  Stamp it in the head's CELL metadata now.
		 * The cell is in NEITHER list and structurally invisible, so no
		 * reader rebuilds a key from it.  Stamping while still in src's
		 * list would let a src reader rematerialize an
		 * out-of-namespace key.  Internal/compressed payloads keep
		 * every leaf's edge byte (the subtree moves wholesale).
		 */
		if (ms_ord && ft_node_external(payload))
			cds_ft_item_to_metadata(run_first)->incoming_byte =
				okey_dst[dst_key_len - 1];
	}

	if (prep == FT_GRAFT_PREP_GLUE) {
		/*
		 * Failure-free commit of the build-invisible diverge cluster.
		 * The displaced old child is LIVE: it is still reachable
		 * through the compressed node being split until the forward
		 * publish.  So via @glue.txn it flips atomically with the
		 * forward publish and the ordered-list run-splice.  The whole
		 * attach is observed old XOR new.  The payload's hidden
		 * back-pointers are wired immediately inside the commit.  Then
		 * reclaim the replaced compressed node.
		 */
		/*
		 * Order-statistics fold (BULK): the diverge cluster raises
		 * @glue.publish_parent's subtree by +cnt_src; record that walk
		 * into the same commit (a no-op when rank stats off).
		 */
		enum urcu_txn_status cst;

		glue.count_delta = (long) cnt_src;
		cst = ft_glue_txn_commit(dst_ft, &glue, run_arg);
		if (cst != URCU_TXN_STATUS_OK) {
			/*
			 * MW LOCK_FINE drop: the GLUE commit's MCAS footprint
			 * is the forward publish + fused run-splice + fenced
			 * retires.  It can conflict with a peer and ABORT even
			 * though it allocates nothing.  The flip rolled back:
			 * dst is byte-for-byte unchanged and the run-splice is
			 * not applied.  ft_glue_txn_commit consumed glue.txn.
			 * Free the failed attempt's invisible cluster and
			 * re-attach the owned @payload via the retry.
			 * @run_splice_txn / @run_first / @run_last persist
			 * (src already excised).
			 */
			ft_glue_abort(dst_ft, &glue);
			ft_txn_attempt_bail(&optxn, rm_open);
			rm_open = false;
			goto retry_merge;
		}
		attached_nf = glue.attached_nf;
		ft_glue_free_old(dst_ft, &glue);
		ft_glue_fini(&glue);
	} else {
		/*
		 * NOSPLIT: graft the in-place payload at @d.  Node allocations draw
		 * from the reserve and the slot proxy + run-splice edges from the
		 * pre-reserved @glue.txn.  Under the drop, the store's
		 * recompact of the (contended) spine can still FAIL its prepare
		 * after the source is already excised.  ft_store_at_graft_point
		 * then returns -EAGAIN / MEMORY_ERROR, not the FT-wide-lock
		 * "cannot fail".  Re-attach the owned @payload via the retry.
		 */
		unsigned int adepth = 0;
		enum cds_ft_status st;

		cds_ft_alloc_reserve_activate(dst_ft, &reserve);
		st = ft_store_at_graft_point(dst_ft, okey_dst, dst_key_len, &d,
				payload, cnt_src, &attached_nf, &adepth, &glue,
				run_arg,
				(long) cnt_src);
		cds_ft_alloc_reserve_deactivate(dst_ft);
		cds_ft_alloc_reserve_drain(dst_ft, &reserve);
		if (st != CDS_FT_STATUS_OK) {
			/*
			 * ft_store_at_graft_point already freed its invisible build +
			 * txn (and, on a commit abort, the relocated copy) and cleaned
			 * up @glue.  Discriminate the failure class:
			 *
			 * - POPULATED is a PERMANENT occupied-point condition, NOT a
			 *   transient conflict -- retrying it would LIVELOCK and strand
			 *   the owned @payload.  Post-unlink it is impossible for the
			 *   supported disjoint exclusive-src contract (no peer creates
			 *   the moved key), so assert as the pre-unlink PREP_POPULATED
			 *   arm does.
			 *   A general (non-disjoint) caller would still orphan
			 *   @payload here.  That is a peer racing the moved
			 *   key into the target during the
			 *   owned-but-unattached window.  The window exists
			 *   because this retry-based recovery excises the
			 *   source BEFORE the dst attach.
			 *   cds_ft_graft no longer has it.  Its exclusive
			 *   src-swap-fused arm lands the src retire ATOMICALLY
			 *   with the attach (only on commit success).  So the
			 *   same race MCAS-aborts, retries, and re-detects
			 *   POPULATED pre-commit with the source never swapped.
			 *   Closing this gap for the sub-position move means
			 *   fusing ft_merge_unlink_src_subtree into the attach
			 *   flip.  It is a deferred parity follow-up (not
			 *   needed for any disjoint use).
			 * - Everything else is transient: re-descend and
			 *   re-attach the owned @payload.  BUSY_ERROR is
			 *   contention: a peer filled the reserve's byte, a
			 *   recompact could not lock, or the store's MCAS
			 *   commit ABORTed.  MEMORY_ERROR is a real allocation
			 *   failure.  Both retry; they are kept distinct so
			 *   a conflict never has to be read as an OOM.
			 */
			if (st == CDS_FT_STATUS_POPULATED_ERROR) {
				assert(!already_unlinked);
				ft_txn_attempt_end(&optxn, rm_open);
				rm_open = false;
				return CDS_FT_STATUS_POPULATED_ERROR;
			}
			ft_txn_attempt_bail(&optxn, rm_open);
		rm_open = false;
		goto retry_merge;
		}
		ft_glue_fini(&glue);
	}

	/*
	 * Order-statistics: the +cnt_src ancestor walk is now FOLDED onto the
	 * attach commit above.  The GLUE diverge and displaced-external shapes
	 * use glue.count_delta.  The in-place / recompact-relocate slot shapes
	 * use ft_store_at_graft_point's count_delta.  So the walk flips
	 * ATOMICALLY with the structural publish, which is exact under
	 * concurrent writers.
	 */

	/*
	 * Ordered list: every attach shape (GLUE, displaced-external, in-place
	 * slot) FUSED the run-splice into its structural flip (@armed).  The
	 * external edge-byte was stamped before that splice, above.  The
	 * standalone two-commit splice remains as a defensive fallback for any
	 * not-yet-fused shape (none today).  Without it an unfused shape would
	 * strand the moved run out of the ordered list.  It commits through
	 * the pre-reserved @run_splice_txn (un-abortable post-drain); the
	 * fused common case frees that txn unused.
	 */
	if (ms_ord && !mrun.armed) {
		ft_ord_cell_run_splice(dst_ft, run_splice_txn, run_first,
			run_last, pred, succ);
		run_splice_txn = NULL;	/* consumed */
	}
	if (run_splice_txn)
		ft_flip_txn_destroy(run_splice_txn);	/* fused: unused */

	/* Raise dst's max_used_key_len for the moved keys (dst_key || suffix). */
	src_max = uatomic_load(&src_ft->max_used_key_len, CMM_RELAXED);
	nm = src_max > src_key_len ? dst_key_len + (src_max - src_key_len) :
		dst_key_len;
	dm = uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED);
	if (nm > dm)
		uatomic_store(&dst_ft->max_used_key_len, nm, CMM_RELAXED);

	ft_txn_attempt_end(&optxn, rm_open);
	rm_open = false;
	return CDS_FT_STATUS_OK;
}

static enum cds_ft_status ft_merge_at_inner(struct cds_ft *dst_ft,
		const uint8_t *dst_key, size_t dst_key_len,
		struct cds_ft *src_ft,
		const uint8_t *src_key, size_t src_key_len)
{
	struct cds_ft *subtree = NULL;
	enum cds_ft_status status;
	struct ft_descent d_src, d_dst;
	unsigned int off_src, off_dst;
	unsigned long cnt_src, cnt_dst;
	enum ft_graft_swap_case ks, kd;
	uint8_t okey_dst_buf[FT_MAX_KEY_LEN], okey_src_buf[FT_MAX_KEY_LEN];
	const uint8_t *okey_dst = dst_key, *okey_src = src_key;

	FT_TP(merge_enter, (const void *) dst_ft, (const void *) src_ft);

	if (!dst_ft || !src_ft) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_ft->group != src_ft->group) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if (dst_key_len > dst_ft->group->max_key_len ||
			src_key_len > dst_ft->group->max_key_len) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	if ((dst_key_len > 0 && !dst_key) ||
			(src_key_len > 0 && !src_key)) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * Fixed-length groups: each source key K has length fixed_len.
	 * The moved subtree's stripped keys have length
	 * (fixed_len - src_key_len).  The resulting destination key
	 * is dst_key || stripped, of length
	 * (dst_key_len + fixed_len - src_key_len).  For that result to
	 * equal fixed_len (the only key length the destination group
	 * accepts), src_key_len and dst_key_len must be equal.
	 */
	if (dst_ft->group->key_len != CDS_FT_LEN_VARIABLE
			&& dst_key_len != src_key_len) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * merge_at is CROSS-TRIE only.  A same-trie move (src_ft == dst_ft) is a
	 * REKEY -- expressed by cds_ft_rekey_graft / cds_ft_rekey_merge, which reach
	 * this worker with @rekey set.  A same-trie cds_ft_merge_at (@rekey ==
	 * FT_REKEY_NONE) is rejected; use the dedicated rekey entry points instead.
	 */
	if (src_ft == dst_ft) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_INVALID_ARGUMENT_ERROR);
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	/*
	 * A rekey on a SPECULATIVE (leaf-stored-key) trie is refused.  The move
	 * re-parents each leaf under @dst_key WITHOUT rewriting its app-owned
	 * stored key: the library never writes that field.  So every moved leaf
	 * would carry a key that no longer matches its structural position.  A
	 * speculative lookup would then return the wrong key.
	 * ft_verify_speculative_key catches this only under
	 * FEATURE_FT_VERIFY_AT_MUTATION.  Only EAGER tries may rekey: they
	 * reconstruct the key from structure and never read the stored field.
	 */
	/*
	 * The STAGED rekey needs a VARIABLE-length group.  The refusal belongs
	 * HERE, before anything is read or reserved.  The move is staged in two
	 * steps.  It detaches @src_key's subtree into a transient trie.  Then
	 * it merges that trie back in at @dst_key.  A detached subtree carries
	 * keys STRIPPED of the prefix.  They are shorter than the one key
	 * length of a fixed-length group.  This is why cds_ft_detach and
	 * cds_ft_graft take a non-root key on variable-length groups only.  The
	 * staged rekey is composed of those two operations, so it inherits the
	 * restriction.
	 *
	 * A fixed-length group is served by the ATOMIC writer instead
	 * (ft_rekey_one_decide, dispatched before this worker).  It stages
	 * through no transient trie, so it has no such restriction.  It is the
	 * only rekey a fixed-length group gets.  Reaching here means its cut
	 * did not cover the shape.
	 *
	 * Enforcing it at the entry is what makes the refusal SAFE.  The
	 * placement is a merge of that transient at @dst_key.  So it meets the
	 * fixed-length equal-prefix-length guard above with
	 * src_key_len == 0 != dst_key_len, and that guard refuses.  But it
	 * refuses only AFTER the detach has committed.  The caller then gets
	 * INVALID_ARGUMENT_ERROR, which means "argument rejected, nothing
	 * happened".  But the trie has just lost every moved key to the
	 * destroyed transient.
	 */
	/*
	 * Combined-length overflow validation (mirrors cds_ft_graft).  A moved
	 * key K becomes dst_key || (K - src_key prefix), of length
	 * dst_key_len + len(K) - src_key_len.  Bound len(K) exactly
	 * (ft_moved_keys_overflow).  Without this check, a variable-length
	 * merge with dst_key_len > src_key_len could create keys longer than
	 * the group's max_key_len.  Such keys overflow the fixed-size key
	 * buffers downstream (the spine's compressed-wrap kbuf, the iterator
	 * buffers).
	 */
	if (ft_moved_keys_overflow(src_ft, src_key, src_key_len, dst_key_len,
			dst_ft->group->max_key_len)) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OVERFLOW_ERROR);
		return CDS_FT_STATUS_OVERFLOW_ERROR;
	}

	/*
	 * MW LOCK_FINE (step 6, §9.5): a CROSS-trie merge consumes @src_ft, so
	 * @src_ft must be EXCLUSIVE.  An exclusive source skips its FT-wide
	 * lock.  Only dst's lock remains: one lock, no cross-trie deadlock.
	 * A live (lock-mode, non-exclusive) cross-trie source is REJECTED with
	 * BUSY before any lock is taken.  The caller makes it exclusive first
	 * (cds_ft_make_exclusive).  A SAME-trie rekey (src == dst) is excluded:
	 * it takes one lock reentrantly.  The check applies under every
	 * writer strategy.
	 */
	/*
	 * THE GATE: it makes src exclusivity an invariant for everything
	 * below.  This includes ft_merge_spine_copy and
	 * ft_merge_graft_subpos_inplace, which only this function calls.  The
	 * former `src_ft != dst_ft &&` term is gone: same-trie is rejected
	 * above, so it was constant-true here.
	 */
	if (!src_ft->exclusive) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_BUSY_ERROR);
		return CDS_FT_STATUS_BUSY_ERROR;
	}

	/*
	 * merge_at is a mutator.  The application provides mutual exclusion
	 * between mutators.  Take the reentrant writer-validation scope (like
	 * cds_ft_graft_swap), NOT a blanket flavor read lock.
	 *
	 * The reason is the REKEY entries, not merge_at.  cds_ft_merge_at
	 * consumes an EXCLUSIVE source.  So every ft_writer_lock_gp_wait on its
	 * path is !src_ft->exclusive-gated and never runs.  This holds for both
	 * calls here and both calls in ft_graft_keylen.  That path syncs
	 * nowhere, and this is why the spine copy pins it under a read lock.
	 * cds_ft_rekey_{graft,merge} share this body with src_ft == dst_ft, a
	 * LIVE trie.  There those waits DO run.  A read lock held across one
	 * would be a writer waiting on its own grace period.  One body serves
	 * two source contracts, so the pin is taken per path, not here.
	 */
	ft_crosstrie_lock_mode_guard(dst_ft, src_ft);
	CDS_FT_SCOPED_WRITER(dst_ft);
	CDS_FT_SCOPED_WRITER(src_ft);

	/*
	 * Convert both keys to ORDINAL form ONCE.  Every internal consumer
	 * takes the ordinal form:
	 * - the merge-point descents;
	 * - the spine build;
	 * - the source-subtree unlink;
	 * - the ordered-list interleave.
	 * The detach+graft fallback below receives the ORIGINAL application
	 * keys instead.  Those entry points remap internally.
	 * Previously the descents consumed the application bytes raw while
	 * the unlink remapped.  On a non-identity key map, the build copied
	 * one subtree and the unlink targeted another.
	 */
	{
		const struct cds_ft_key_map *km = &dst_ft->group->key_map;

		if (caa_unlikely(!km->identity)) {
			ft_key_to_ordinals(okey_dst_buf, dst_key, dst_key_len,
				km);
			ft_key_to_ordinals(okey_src_buf, src_key, src_key_len,
				km);
			okey_dst = okey_dst_buf;
			okey_src = okey_src_buf;
		}
	}

	/*
	 * Same-trie "rekey" (src_ft == dst_ft): moving @src_key's subtree to
	 * @dst_key within one trie is allowed.  But the two keys must be
	 * DISJOINT: neither is a prefix of the other.  A prefix relationship
	 * means one key lies inside the other's subtree.  The move would then
	 * be circular, and @dst_key would not be absent.  Equal keys are a
	 * degenerate self-move.  The check is byte-position-wise.  It is
	 * identical in application and ordinal space, because the key map is a
	 * per-position bijection.  Cross-trie subtrees are disjoint by
	 * construction, so the guard is same-trie only.
	 */

	/*
	 * Locate both merge points read-only (a writer descends its own
	 * stable state).  With no content under @src_key, the merge is a
	 * no-op.  cnt_src == 0 also covers an empty @src_ft root
	 * (src_key_len == 0): there the descent still reports EXACT at the
	 * always-present root.
	 */
	ks = ft_merge_descend(src_ft, okey_src, src_key_len, &d_src,
			&off_src, &cnt_src);
	MRG_SKIPCONF_PROBE(0, d_src);
	if (ks == FT_GRAFT_SWAP_DELEGATE || cnt_src == 0) {
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

	/*
	 * Same-trie "rekey": detach @src_key's subtree into a transient trie,
	 * then merge that whole trie back in at @dst_key.  The detach fully
	 * commits BEFORE the placement reads it: it recompacts the shared
	 * common-prefix ancestor and drains.  So the two sides of the move no
	 * longer alias the same node.  The in-place reorder CANNOT be used
	 * here.  Its source unlink would recompact that shared ancestor out
	 * from under the build-invisible destination placement (a stale-slot
	 * publish).
	 *
	 * The cross-trie merge handles an occupied @dst_key by MERGING and an
	 * absent one by grafting.  It is made UNFAILABLE: its nodes + flip
	 * batches are pre-reserved before the detach below.  So it always
	 * commits the move.  No failure path restores the content to @src_key,
	 * i.e. there is no reader-observable rollback.
	 *
	 * A KEY_SHORTER source (off_src > 0) has @src_key ending inside the
	 * compressed node @d_src.nf.  It is reduced to the EXACT node boundary
	 * first, exactly as the cross-trie path does:
	 * - detach @cn_s->child via the full-compressed key (src prefix ++ all
	 *   of cn_s);
	 * - merge at @dst_key extended by the residual cn_s bytes.
	 * ft_detach_keylen overshoots a compressed node, so it must be handed a
	 * boundary key, not an interior one.
	 */
	/*
	 * The STAGED rekey lived here.  It was a detach of @src_key's subtree
	 * into a transient trie, then a merge of that trie back in at @dst_key.
	 * The argument checks that only it needed lived here too.  They moved
	 * to ft_rekey_refuse_uncovered (ft-rekey.h).  So this worker is now CROSS-TRIE ONLY and no longer recursive: the
	 * recursive ft_merge_at_inner() call was inside that block.
	 */

	bool md_open = false;
	bool md_contended;
	unsigned long md_spin = 0;	/* this op's merge_spine_retry depth (probe) */
	/*
	 * The op's PERSISTENT engine handle.  It spans the whole
	 * merge_spine_retry loop, the way ft_txn_op_init does for insert,
	 * remove and replace (doc §11).  Without it this loop ages nothing.
	 * Every attempt started at retry 0, so the domain never escalated the
	 * writer into the per-trie FIFO fair-mutex lane.  The loop had no
	 * termination argument at all.  Measured under an exponential spacing:
	 * 585250 declined lock sets over 235141 merges, one op re-descending
	 * 348 times.
	 *
	 * It is bracketed only on a FINE dst.  ft_op_begin() enters the RCU
	 * read side, so the bracket must not span a grace period.  The src is
	 * exclusive here (a live src is refused with BUSY), so the spine copy
	 * waits for none.
	 */
	struct ft_op optxn;

	MRG_SPIN_PROBE(0);
	ft_txn_op_init(dst_ft, &optxn);

	/*
	 * Re-entered when a spine-copy attempt could not take its dup-chain lock
	 * set (@md_contended).  The attempt moved nothing.  An EXCLUSIVE source
	 * means the merge is build-invisible until its one commit.  So a
	 * declined attempt is trivially undone: both tries are byte-for-byte as
	 * they were.  Re-descend and rebuild.  The acquire declined because a
	 * peer is reshaping the dst spine we planned against.  This is the same
	 * plan->commit retry shape as cds_ft_graft's retry_attach and
	 * cds_ft_graft_swap's retry_swap.
	 */
merge_spine_retry:
	/*
	 * The spine-copy path below descends the live dst here.  It then node
	 * locks descent-captured dst nodes (@d_dst->pnf / ->ppnf, the dup-chain
	 * holders) inside ft_merge_spine_copy.  Two things keep them from being
	 * freed in between:
	 * - the dst writer scope holds the FT-wide lock, so no peer writer
	 *   runs;
	 * - on a shared dst, the attempt's op handle holds a read section
	 *   (ft_op_begin), from descent to commit.
	 * An exclusive dst has no readers and frees synchronously.
	 *
	 * @md_open marks an open attempt.  Every site that clears it closes
	 * the attempt: before the spine-copy return, on the fall-through to
	 * the detach/graft paths, and before each retry.
	 */
	if (dst_ft->lock_fine) {		/* src exclusive: gated above */
		ft_op_begin(&optxn);
		md_open = true;
	}
	kd = ft_merge_descend(dst_ft, okey_dst, dst_key_len, &d_dst,
			&off_dst, &cnt_dst);
	MRG_SKIPCONF_PROBE(2, d_dst);
	/*
	 * Reanchor level-move: a peer chain-merge absorbed this slot's level
	 * into a longer compressed node while the descent walked it.  So
	 * @d_dst names a position that has moved.  Every count and offset
	 * derived from it describes the old shape.  Insert and graft already
	 * bail on this; merge read it and continued.
	 *
	 * Re-descend down the SAME unwind that the spine copy's contention
	 * path uses.  Nothing is built here: the label is above the attempt's
	 * open, and the detach branch returned long before.  So this is the
	 * state that path already returns to.  Close the attempt first: the
	 * retry opens a new one.
	 */
	if (caa_unlikely(d_dst.skip_conflict)) {
		if (md_open) {
			/* Nothing moved: age the conflict, close the attempt. */
			ft_txn_attempt_bail(&optxn, true);
			md_open = false;
		}
		md_spin++;
		MRG_SPIN_PROBE(2);
		MRG_SPIN_MAX(md_spin);
		goto merge_spine_retry;
	}

	/*
	 * Atomic build-invisible spine-copy of @src_ft's subtree at @src_key
	 * into @dst_ft's non-empty subtree at @dst_key.  Each side is one of:
	 * - EXACT (@off == 0): the subtree at @d->nf;
	 * - KEY_SHORTER (@off > 0): the key ends inside a compressed node.
	 *   The src node is reclaimed by the commit's unlink.  The dst node
	 *   is wrapped under its prefix.
	 * ft_merge_spine_copy handles every such shape: internal, external,
	 * compressed and compressed-parent dst merge points.  Only a dst that
	 * is empty under @dst_key falls through to the detach-based graft
	 * below.
	 */
	if ((ks == FT_GRAFT_SWAP_EXACT || ks == FT_GRAFT_SWAP_KEY_SHORTER)
			&& cnt_dst > 0
			&& (kd == FT_GRAFT_SWAP_EXACT
				|| kd == FT_GRAFT_SWAP_KEY_SHORTER)) {
		md_contended = false;
		status = ft_merge_spine_copy(dst_ft, src_ft, &d_src,
				okey_src, src_key_len, cnt_src, off_src,
				&d_dst, cnt_dst, off_dst, dst_key_len,
				&md_contended, &optxn);
		if (md_contended) {
			/*
			 * Contention, nothing moved.  Re-pin, re-descend
			 * and rebuild.
			 */
			if (md_open) {
				/*
				 * AGE IT.  This is the arm that spun 348 deep
				 * with nothing to make it terminate.
				 * ft_op_conflict carries the retry count on
				 * the persistent handle.  So this writer
				 * escalates into the FIFO lane and the
				 * contention drains.  end() then FORFEITS the
				 * turn.  This is a pre-commit bail.  A bail
				 * that keeps its turn while the peer it waits
				 * on queues behind that same turn is the
				 * insert livelock.
				 */
				ft_txn_attempt_bail(&optxn, true);
				md_open = false;
			}
			md_spin++;
			MRG_SPIN_PROBE(1);
			MRG_SPIN_PROBE(4);	/* src always exclusive here */
			MRG_SPIN_MAX(md_spin);
			goto merge_spine_retry;
		}
		if (status == CDS_FT_STATUS_OK) {
			/*
			 * Raise dst's max_used_key_len for the moved keys
			 * (dst_key_len + the longest stripped suffix), as
			 * the graft paths do.  Later graft overflow
			 * validations feed off it.
			 */
			size_t src_max = uatomic_load(&src_ft->max_used_key_len,
					CMM_RELAXED);
			size_t nm = src_max > src_key_len ?
				dst_key_len + (src_max - src_key_len) :
				dst_key_len;
			size_t dm = uatomic_load(&dst_ft->max_used_key_len,
					CMM_RELAXED);

			if (nm > dm)
				uatomic_store(&dst_ft->max_used_key_len, nm,
					CMM_RELAXED);
		}
		if (md_open) {
			ft_op_end(&optxn);
			md_open = false;
		}
		FT_TP(merge_exit, (int) status);
		return status;
	}
	/*
	 * Fall-through: the spine-copy shape did not apply.  This is a
	 * whole-source move, or an empty dst under @dst_key (the detach graft
	 * below).  Close the spine-copy attempt here.  The src_key_len==0
	 * graft re-descends in its own attempts.  The detach-graft path
	 * re-resolves its own (parent, slot).
	 */
	if (md_open) {
		ft_op_end(&optxn);
		md_open = false;
	}

	/*
	 * Whole-source move (src_key_len == 0): the entire @src_ft is the
	 * payload.  So graft it directly, with no detach and no fallible
	 * rollback.  ft_graft is itself leak-free:
	 * - a diverge split is built invisibly;
	 * - a NOSPLIT attach restores the saved old source root
	 *   allocation-free on OOM.
	 * So an allocation failure leaves both tries pristine.  This covers
	 * an empty dst root and a diverged dst_key alike.  It is also the
	 * leak-free path for "rekey within a trie".  That is: detach a
	 * sub-trie, then merge_at it at a new key in the same group.  The new
	 * key must be currently absent.  A
	 * dst_key occupied by an empty-internal leftover yields
	 * POPULATED_ERROR here, exactly as the detach-then-graft path did.
	 */
	if (src_key_len == 0) {
		/*
		 * A live (non-exclusive) cross-trie source was already
		 * rejected with BUSY at merge_at's entry.  So this graft sees
		 * an exclusive source, and ft_graft_keylen runs its fused
		 * body directly.
		 */
		/* No read section: see cds_ft_graft. */
		status = ft_graft_keylen(dst_ft, dst_key, dst_key_len, src_ft);
		FT_TP(merge_exit, (int) status);
		return status;
	}

	/*
	 * Empty dst ROOT: dst_key_len == 0, so the whole @dst_ft is empty at
	 * the merge point.  Move @src_ft@src_key in WITHOUT a fallible graft,
	 * so that no rollback can strand the moved externals.  The steps:
	 * - pre-allocate the source's replacement root (fallible while
	 *   @src_ft is still pristine);
	 * - detach the source subtree (clean on its own failure);
	 * - SWAP it into @dst_ft's root.
	 * The swap is a failure-free root-to-root move.  There is no
	 * parent-pointer change and no "jump out" window: both ends are
	 * roots with parent NULL.  The detach already drained @src_ft's
	 * readers.  This is the same reasoning as ft_graft's root path.
	 * Nothing fallible follows the detach, so this path has no rollback
	 * and cannot leak (contrast the diverged path below).
	 */
	if (dst_key_len == 0 && cnt_dst == 0) {
		struct cds_ft_inode *fresh_root;
		struct cds_ft_metadata *fresh_meta;
		struct cds_ft_metadata *dst_rmeta;
		struct cds_ft_inode_flag *dst_root_fenced;
		/*
		 * DEDUPED, not taken: an outer frame owns this fence and
		 * owns its release.  Clearing it here drops a fence the op
		 * still writes under.  One owner per fence.
		 */
		bool dst_root_shared = false;
		uintptr_t dst_root_snap;
		struct cds_ft_inode *old_dst_root;
		struct ft_flip_txn *appear_txn = NULL;
		size_t sm;
		int fence_ret;

		/*
		 * @cnt_dst was sampled far above.  An ENTIRE
		 * ft_detach_keylen of the source runs before the swap below.
		 * This is the widest empty-dst window in the FT.  Re-decide
		 * emptiness UNDER the old root's node lock.  A contract-legal
		 * peer attach can then no longer land inside that window and
		 * be freed with the root it landed on.  If a peer populated
		 * dst, fall through to the DIVERGED path below.  That path
		 * merges into a populated destination.  An up-front
		 * cnt_dst != 0 would have taken the same path.  On a held
		 * root, report BUSY.
		 */
		fence_ret = ft_root_attach_fence_empty(dst_ft, &dst_root_fenced,
			&dst_rmeta, &dst_root_snap, &dst_root_shared,
			&optxn);
		if (fence_ret == -EEXIST)
			goto diverged;
		if (fence_ret) {
			/* As in ft_graft_keylen: no attempt owns the refusal. */
			ft_acq_contended = 0;
			status = CDS_FT_STATUS_BUSY_ERROR;
			goto out;
		}

		fresh_root = alloc_cds_ft_node(src_ft, &ft_types[0], &fresh_meta);
		if (!fresh_root) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			status = CDS_FT_STATUS_MEMORY_ERROR;
			goto out;
		}
		fresh_meta->parent_word = ft_trie_parent(src_ft);
		ft_nr_keys_store(src_ft, fresh_meta, 0, CMM_RELAXED);

		/*
		 * Pre-reserve the dst-appear root-swap txn BEFORE the detach.
		 * The swap is failure-free (post-detach), so it commits
		 * through this pre-reserved txn (ft_ord_cell_flip_into).  OOM
		 * here aborts while @src_ft is still pristine (no detach
		 * yet).  The dst old-root freeze-on-free tombstone rides the
		 * SAME flip (atomic detach, §4.B):
		 * - list-on: +1 edge;
		 * - list-off: a 2-edge txn (root edge + tombstone) instead of
		 *   the former lone ft_root_edge_flip store.
		 */
		if (dst_ft->group->ordered_list_set)
			appear_txn = ft_flip_txn_create_bounded(dst_ft,
				FT_ROOT_LIST_SWAP_MAX_EDGES + 1);
		else
			appear_txn = ft_flip_txn_create_bounded(dst_ft, 2);
		if (!appear_txn) {
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			free_cds_ft_node_unpublished(src_ft, fresh_root);
			status = CDS_FT_STATUS_MEMORY_ERROR;
			goto out;
		}

		status = ft_detach_keylen(src_ft, src_key, src_key_len, &subtree);
		if (status < 0) {
			/* NOT_FOUND impossible: @src_ft had content. */
			if (!dst_root_shared)
				ft_meta_lock_release(dst_rmeta);
			if (appear_txn)
				ft_flip_txn_destroy(appear_txn);
			free_cds_ft_node_unpublished(src_ft, fresh_root);
			goto out;
		}

		/*
		 * Failure-free swap.  @subtree->root carries the moved content
		 * with parent already NULL (set by the detach), so it drops
		 * straight into @dst_ft's empty root slot.  @subtree keeps the
		 * pre-allocated empty root so its destroy below frees nothing of
		 * the moved content.  @dst_ft was empty, so it adopts @subtree's
		 * whole ordered cell list wholesale (head/tail endpoints only).
		 */
		/*
		 * The FENCED root, not a fresh read.  Under the fence the two
		 * are equal by construction.  Using the fenced value keeps
		 * the retire, the swap's expected-old and the free naming ONE
		 * node.
		 */
		old_dst_root = ft_node_ptr(dst_root_fenced);
		/*
		 * Fuse the structural root swap with the ordered-list
		 * head/tail transfer into ONE flip
		 * (ft_root_list_swap_publish).  A reader then never sees the
		 * moved keys reachable in @dst_ft's structure while its
		 * ordered list is still empty.  That is the dst-appear
		 * cross-view window, closed the same way as the graft
		 * empty-dst path.  Only @dst_ft has concurrent readers here.
		 * @subtree is the fresh EXCLUSIVE trie that the detach above
		 * just produced.  So its root reset + head/tail clear are
		 * plain stores (no src-side disappear window, unlike
		 * ft_graft's cross-trie empty-dst).
		 *
		 * Freeze-on-free (doc §4.B): the dst old root that this swap
		 * retires gets its tombstone recorded INTO the swap txn.  So
		 * the mark and the unlink flip atomically (atomic detach).
		 * This is failure-free past the detach above, so it runs only
		 * on the committing path.
		 */
		/*
		 * @subtree->root becomes @dst_ft's root, so re-name its owner
		 * before either branch publishes it.  A cross-trie move
		 * leaves the back-edge naming the trie it came FROM.  That is
		 * exactly the aliasing cds_ft_verify reports at depth 0.
		 * @subtree is the fresh EXCLUSIVE detach product with no
		 * readers, so the store is safe ahead of the flip.
		 */
		cds_ft_item_to_metadata(ft_node_ptr(subtree->root))->parent_word =
			ft_trie_parent(dst_ft);
		if (dst_ft->group->ordered_list_set) {
			/*
			 * dst FILLS by adopting @subtree's whole list.
			 * @subtree is the fresh EXCLUSIVE detach product (no
			 * readers).  So its incoming run relinks to dst's
			 * sentinel in the SAME flip (relink_dest = subtree,
			 * relink_incoming = true).  @subtree's sentinel
			 * resets to empty with a plain store.
			 */
			ft_flip_txn_lock_register(appear_txn, dst_rmeta,
				dst_root_snap);
			ft_flip_txn_record_tombstone_locked(appear_txn,
				dst_rmeta, dst_root_snap);
			ft_root_list_swap_publish(dst_ft, appear_txn, &dst_ft->root,
				dst_root_fenced, subtree->root,
				NULL, ft_ord_first(subtree),
				NULL, ft_ord_last(subtree), subtree, true);
			urcu_txn_sw_list_init(&subtree->ord_sentinel);
		} else {
			/*
			 * No ordered list: dst's root is the only
			 * reader-visible structural slot.  Commit the appear
			 * root edge and the old-root tombstone as ONE 2-edge
			 * flip through the pre-reserved txn.  A lone root
			 * edge would reduce to a release store, but the
			 * fused tombstone makes it multi-edge.  Readers
			 * resolve the transient root proxy exactly as on the
			 * list-on path.  (@subtree is the fresh EXCLUSIVE
			 * trie, so its root reset below stays a plain
			 * store.)
			 */
			/* Stage 5: the root's own lock, so this record can park SW. */
			(void) ft_root_lock_take(dst_ft, NULL, appear_txn);
			ft_flip_txn_record_root(appear_txn,
				(void **) &dst_ft->root,
				(void *) dst_root_fenced, (void *) subtree->root);
			ft_flip_txn_lock_register(appear_txn, dst_rmeta,
				dst_root_snap);
			ft_flip_txn_record_tombstone_locked(appear_txn,
				dst_rmeta, dst_root_snap);
			ft_flip_txn_commit_unfailable(dst_ft, appear_txn);
		}
		FT_TP(root_publish, (const void *) dst_ft,
			(const void *) dst_ft->root);
		subtree->root = ft_node_flag(fresh_root, 0);
		free_cds_ft_node(dst_ft, old_dst_root);

		/* dst_key_len == 0: dst keys equal the moved keys, same lengths. */
		sm = uatomic_load(&subtree->max_used_key_len, CMM_RELAXED);
		if (sm > uatomic_load(&dst_ft->max_used_key_len, CMM_RELAXED))
			uatomic_store(&dst_ft->max_used_key_len, sm, CMM_RELAXED);

		cds_ft_destroy(subtree);
		FT_TP(merge_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

diverged:
	/*
	 * Sub-position source into a dst absent at @dst_key: graft the source
	 * subtree IN PLACE (no detach, no re-root).  The build-invisible
	 * cluster then references @d_src.nf directly, and the source unlink is
	 * the last fallible step.  This is the leak-free reorder.
	 *
	 * This handles every source-root shape:
	 * - plain multi-child internal;
	 * - an internal carrying external_nodes;
	 * - external;
	 * - compressed;
	 * - 1-child.
	 * It handles them at both a GLUE diverge and a NOSPLIT (at-node /
	 * build-branch) dst point.
	 *
	 * A KEY_SHORTER source (the src key ends @off_src bytes inside the
	 * compressed node @d_src.nf) reduces to the EXACT shape.  The moved
	 * subtree is @cn_s->child.  Its keys gain the residual bytes
	 * cn_s->key_bytes[off_src .. len) as a prefix.  So graft @cn_s->child
	 * at @dst_key extended by that residual.
	 * ft_merge_unlink_src_subtree overshoots @cn_s (preserves
	 * @cn_s->child), so the source side is identical.  @cn_s itself is
	 * reclaimed by that unlink.
	 */
	if (ks == FT_GRAFT_SWAP_EXACT || ks == FT_GRAFT_SWAP_KEY_SHORTER) {
		bool handled;
		struct cds_ft_inode_flag *payload;
		const uint8_t *gokey = okey_dst, *gappkey = dst_key;
		size_t glen = dst_key_len;
		uint8_t ext_okey[FT_MAX_KEY_LEN], ext_app[FT_MAX_KEY_LEN];

		if (ks == FT_GRAFT_SWAP_EXACT) {
			payload = d_src.nf;	/* off_src == 0 by construction */
		} else {
			struct cds_ft_compressed_node *cn_s =
				ft_compressed_node_ptr(d_src.nf);
			unsigned int rlen = cn_s->len - off_src, j;
			const struct cds_ft_key_map *km =
				&dst_ft->group->key_map;

			payload = cn_s->child;
			memcpy(ext_okey, okey_dst, dst_key_len);
			memcpy(&ext_okey[dst_key_len], &cn_s->key_bytes[off_src],
				rlen);
			memcpy(ext_app, dst_key, dst_key_len);
			if (km->identity) {
				memcpy(&ext_app[dst_key_len],
					&cn_s->key_bytes[off_src], rlen);
			} else {
				for (j = 0; j < rlen; j++)
					ext_app[dst_key_len + j] =
						km->ordinal_to_key[
						cn_s->key_bytes[off_src + j]];
			}
			gokey = ext_okey;
			gappkey = ext_app;
			glen = dst_key_len + rlen;
		}

		status = ft_merge_graft_subpos_inplace(dst_ft, src_ft,
			gokey, gappkey, glen, okey_src, src_key_len,
			payload, cnt_src, &handled);
		if (handled) {
			FT_TP(merge_exit, (int) status);
			return status;
		}
	}

	/*
	 * Unreachable.  ft_merge_descend reports EXACT, KEY_SHORTER or
	 * DELEGATE.  DELEGATE returned OK at the top (cnt_src == 0).  Every
	 * EXACT / KEY_SHORTER source is handled above:
	 * - a non-empty dst by the build-invisible spine copy;
	 * - an empty dst root or whole-source move by their failure-free root
	 *   swaps;
	 * - a diverged / absent dst point by ft_merge_graft_subpos_inplace
	 *   (which always reports handled).
	 * No shape falls through.  So the old detach-then-graft fallback and
	 * its rollback are gone.  That fallback was the last path that carried
	 * the double-OOM leak.
	 */
	assert(0 && "cds_ft_merge_at: unhandled merge shape");
	status = CDS_FT_STATUS_OK;
out:
	FT_TP(merge_exit, (int) status);
	return status;
}

#endif /* FEATURE_FT_MERGE */

enum cds_ft_status cds_ft_merge_at(struct cds_ft *dst_ft,
		const uint8_t *dst_key, size_t dst_key_len,
		struct cds_ft *src_ft,
		const uint8_t *src_key, size_t src_key_len)
{
#ifdef FEATURE_FT_MERGE
	{
		enum cds_ft_status st__;

		FT_BW_OP(FT_BW_MERGE_AT);
		/*
		 * ☠ VALIDATION LIVES IN ft_merge_at_inner, NOT HERE.  This
		 * entry is a pure delegation, so a NULL @dst_ft reaches it.
		 * The gate must not dereference one on the way.  Let the
		 * inner reject it, unchanged.
		 */
		if (!dst_ft)
			return ft_merge_at_inner(dst_ft, dst_key, dst_key_len,
					src_ft, src_key, src_key_len);
		/*
		 * G5.5: @src_ft is exclusive by contract, so only @dst_ft has
		 * point-op peers to exclude.
		 */
		CDS_FT_SCOPED_BULK_GATE(dst_ft, FT_BULK_WRITER_ONLY);

		st__ = ft_merge_at_inner(dst_ft, dst_key, dst_key_len, src_ft,
				src_key, src_key_len);
		return st__;
	}
#else
	(void) dst_ft; (void) dst_key; (void) dst_key_len;
	(void) src_ft; (void) src_key; (void) src_key_len;
	return CDS_FT_STATUS_NOT_SUPPORTED;
#endif
}

#ifdef FEATURE_FT_MERGE
/*
 * The in-trie move uses TWO writers.  The ATOMIC one (one decide) runs where
 * it applies.  The staged one runs everywhere else.
 *
 * WHY THE ATOMIC WRITER IS TRIED FIRST.  It is not an optimization.  A rekey
 * exists to provide one property.  A key present throughout the move is never
 * read as ABSENT, neither at the old position nor at the new one.  The staged
 * writer
 * cannot provide it by construction.  It detaches the subtree into a
 * transient trie as ONE COMMIT and merges that trie back as ANOTHER.  Between
 * the two, the moved keys are in no trie at all.  This lasts at least the
 * grace period that the detach drains.  inv_rekey_public_no_gap measured
 * ~745 absence observations per move.  ft_rekey_one_decide commits the
 * src-slot clear, the destination publish and the re-parents as ONE flip.  So
 * a reader sees the subtree at the source XOR the destination, and never at
 * neither.
 *
 * The atomic writer covers a CUT of the shapes, not all of them.  It reports
 * -EINVAL for the rest:
 * - a compressed or external S_top;
 * - a source junction that would collapse;
 * - an ordered-list interleave;
 * - a destination abutting the moved run's own ordered neighbourhood;
 * - an unequal-length destination.
 * Those fall back here.  Either group flavour may take the atomic writer.  A
 * fixed-length group MUST, because it has nothing to fall back TO.  The
 * staged writer is variable-length-only, since a detached subtree's keys are
 * stripped of the prefix.  So ft_merge_at_inner refuses those rather than
 * losing the subtree between its two commits.
 *
 * ARM THE MOVE GATE around both.  ft_move_gate_enter publishes @move_active
 * and waits ONE grace period.  So every reader already inside a critical
 * section finishes before anything moves.  Such a reader may have branched to
 * the FAST, non-verifying lookup.  Readers that start after it see the gate
 * and take the coherent two-pass path.  That path re-descends when its two
 * traversals disagree.  A burst of concurrent rekeys pays ~one grace period
 * in total.  Without the gate the two-pass machinery is unreachable in
 * production: the coherent specializations are selected by @rekey_coherence
 * but ENTERED only under ft_move_active().  ONE bracket covers the attempt
 * AND the fallback.  Two brackets would pay two grace periods for one move.
 *
 * CALLER CONTRACT, inherent to the gate: this BLOCKS on a grace period.  So
 * it must not be called from inside an RCU read-side critical section: the
 * grace period would wait on the caller's own section.  The cross-trie
 * cds_ft_merge_at has no such contract.  Its source is exclusive, so it moves
 * no live key and arms no gate.
 */

/*
 * ============================================================================
 * REKEY TWINS of the three merge workers that carry ATOMICITY.
 *
 * A same-trie rekey (cds_ft_rekey_{graft,merge}) reaches these bodies with
 * src_ft == dst_ft, so its "src side" IS the live shared destination.  A
 * cross-trie merge reaches them with an EXCLUSIVE src that the entry gates
 * enforce (src_ft != dst_ft && !src_ft->exclusive -> BUSY_ERROR).  So the
 * two callers need OPPOSITE src-side atomicity.  The shared bodies had NO
 * fork on which caller they had: 0 tests on @rekey or src_ft == dst_ft
 * across 599 lines of worker code.  So both were served by the stricter
 * contract, unconditionally and invisibly.
 *
 * Split by DUPLICATION first, deliberately.  These are byte-identical copies
 * modulo the three names, so this step changes no behaviour.  The
 * specialisation of each side can then be reviewed as its own diff against a
 * known-equal base.
 *
 * The boundary is DERIVED, not chosen.  The twinned functions are exactly
 * those that COMMIT, DRAIN or CREATE A TXN.  The other 15 functions
 * reachable from here compute shape only.  They carry no atomicity contract.
 * Examples are ft_merge_build, ft_merge_count and
 * ft_merge_ord_interleave_collect.  So they stay SHARED.  The atomic rekey
 * path already demonstrates this by calling three of them.
 * ============================================================================
 */


#endif /* FEATURE_FT_MERGE */


enum cds_ft_status cds_ft_merge(struct cds_ft *dst_ft,
		const uint8_t *key, size_t key_len,
		struct cds_ft *src_ft)
{
	return cds_ft_merge_at(dst_ft, key, key_len, src_ft, key, key_len);
}
