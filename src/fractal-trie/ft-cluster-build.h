// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-cluster-build.h
 *
 * Userspace RCU library - Fractal Trie: node-cluster builders.  The shared
 * primitives that assemble a fresh, unpublished subtrie cluster for the
 * insert / graft / merge attach paths, cooperating with the ft_glue
 * build-invisible transaction:
 *   - ft_build_branch                build an internal branch down to a leaf;
 *   - ft_free_branch_unpublished     reclaim a fresh branch on a later failure;
 *   - ft_compress_single_child_if_needed   collapse a 1-child internal node;
 *   - ft_try_compress_chain          compress a single-child run.
 * They are mutually recursive, so a small forward-declaration block precedes
 * the definitions.
 *
 * #included after ft-mutation-node.h, because the builders call the node
 * set_nth / recompact ops.  #included before ft-insert.h, their first user.
 * Graft and merge reach them from further down the translation unit.
 *
 * Implementation unit: #included once into the fractal-trie.c translation unit
 * (preserves cross-module inlining).  Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-cluster-build.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/* Mutually-recursive builders -- forward decls to close the cycle. */
static struct cds_ft_inode_flag *ft_build_branch(struct cds_ft *ft,
		const uint8_t *key, unsigned int start, unsigned int end,
		struct cds_ft_inode_flag *leaf,
		unsigned long subtree_external_count,
		bool has_external_nodes,
		struct ft_glue *glue);
static void ft_free_branch_unpublished(struct cds_ft *ft,
		struct cds_ft_inode_flag *top, struct cds_ft_inode_flag *leaf);
static struct cds_ft_inode_flag *ft_compress_single_child_if_needed(
		struct cds_ft *ft, struct cds_ft_inode_flag *child,
		struct ft_glue *glue);
static struct cds_ft_inode_flag *ft_try_compress_chain(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, unsigned int level,
		struct cds_ft_inode_flag *child,
		struct cds_ft_node *external_nodes,
		struct ft_glue *glue);

/*
 * Build a branch for key[start .. end-1] with @leaf at the bottom.
 * When the path is 2+ bytes, a single compressed node is used instead
 * of a chain of single-child internal nodes.  Returns the topmost
 * flagged node, or NULL on allocation failure.
 *
 * @glue: when non-NULL (graft build-invisible mode), @leaf is LIVE
 * payload data:
 *  - its back-pointer into the bottom branch node is deferred to the
 *    post-sync commit;
 *  - every fresh branch node is tracked in @glue;
 *  - on OOM the function returns NULL WITHOUT freeing.  The caller's
 *    ft_glue_abort reclaims the tracked nodes.
 * When NULL, the legacy immediate path runs (leaf back-pointer set now,
 * self-free on OOM).
 */
static
struct cds_ft_inode_flag *ft_build_branch(struct cds_ft *ft,
		const uint8_t *key, unsigned int start, unsigned int end,
		struct cds_ft_inode_flag *leaf,
		unsigned long subtree_external_count,
		bool has_external_nodes,
		struct ft_glue *glue)
{
	/*
	 * When the caller will attach external_nodes to the top,
	 * the top must be an internal node (compressed nodes cannot
	 * carry metadata->external_nodes).  Bias compression to start
	 * one byte deeper so an internal node is created at `start`.
	 */
	unsigned int compress_start = has_external_nodes ? start + 1 : start;
	struct cds_ft_inode_flag *cur = leaf;
	int loop_top, i;

	if (start == end)
		return leaf;	/* path_len == 0. */

	/*
	 * Try compression over [compress_start, end).  Floor is len 1.
	 * Under FEATURE_FT_SKIP_COMPRESSED a 1-byte compressed publishes
	 * as a SKIP_X-tagged slot pointer (free dispatch).  It is the
	 * canonical replacement for what would otherwise be a non-root
	 * 1-child internal node.  In glue mode ft_try_compress_chain
	 * defers the live leaf's back-pointer (and absorbs a compressed
	 * canonicalization-wrapper @leaf into this compressed).
	 */
	if (end >= compress_start + 1) {
		struct cds_ft_inode_flag *compressed;

		compressed = ft_try_compress_chain(ft, key, end,
			compress_start, leaf, NULL, glue);
		if (compressed == (void *) (long) -ENOMEM)
			return NULL;
		if (compressed) {
			struct cds_ft_metadata *m =
				ft_flag_to_metadata(ft, compressed);

			ft_nr_keys_store(ft,m,
				subtree_external_count, CMM_RELAXED);
			/*
			 * ft_try_compress_chain already tracked the compressed
			 * node by its PLAIN flag in glue mode (the @compressed
			 * return here is the skip form, unsafe to track).
			 */
			cur = compressed;
			if (!has_external_nodes)
				return cur;
			/*
			 * has_external_nodes: fall through to create
			 * an internal node at `start` wrapping the
			 * compressed chunk.
			 */
		}
	}

	/*
	 * Create internal nodes from loop_top down to start.
	 *   Compression succeeded: only need an internal at `start`
	 *     (compress_start == start + 1, loop_top == start).
	 *   No compression:        create internal nodes for each
	 *     byte in [start, end).
	 */
	loop_top = (cur != leaf) ? (int) compress_start - 1 : (int) end - 1;
	for (i = loop_top; i >= (int) start; i--) {
		struct cds_ft_inode_flag *dest = NULL;
		/*
		 * Bottom internal whose child is the live @leaf (no
		 * compression happened): defer @leaf's back-pointer
		 * (cluster_leaf) and record it for commit.  Higher internals
		 * and the compressed-wrapping case have only fresh children,
		 * whose back-pointers are safe to set during the build.
		 */
		bool leaf_edge = (glue != NULL) && (cur == leaf);
		int ret;

		ret = ft_node_set_nth(ft, &dest, key[i], cur,
			NULL, NULL, i, leaf_edge);
		if (ret) {
			if (glue)
				return NULL;	/* abort frees tracked nodes */
			/*
			 * Legacy: free the created internal chain and, if
			 * present, the compressed chunk at the bottom.
			 */
			while (cur != leaf) {
				if (ft_node_compressed(cur)) {
					free_compressed_node(ft,
						ft_compressed_node_ptr(cur));
					cur = leaf;
				} else {
					struct cds_ft_inode_flag *next;
					uint8_t kv = key[i + 1];

					next = ft_node_get_nth(ft, cur, NULL, kv, FT_PF_NONE);
					free_cds_ft_node(ft, ft_node_ptr(cur));
					cur = next;
					i++;
				}
			}
			return NULL;
		}
		ft_nr_keys_store(ft,
			cds_ft_item_to_metadata(ft_node_ptr(dest)),
			subtree_external_count, CMM_RELAXED);
		if (glue) {
			ft_glue_track(glue, dest);
			if (leaf_edge) {
				struct cds_ft_inode_flag **slot = NULL;

				ft_node_get_nth_skip(dest, &slot, key[i],
					FT_PF_NONE);
				ft_glue_defer_edge(ft, glue, leaf, dest, slot);
			}
		}
		/*
		 * Initialize density: this node's child (cur) may be
		 * the graft payload with an existing subtree.
		 * Bottom-up order ensures child density is set before
		 * parent.
		 */
		cur = dest;
	}
	return cur;
}

/*
 * Free a FRESH, never-published single-path branch built by ft_build_branch
 * (legacy glue == NULL mode).  Call it after a LATER fallible step failed.
 * Walk the single-child chain from @top down to @leaf, and free every fresh
 * node.  Do not free @leaf: it is the caller's payload.  The memory is
 * writer-private, so immediate frees are safe.  A skip-encoded link resolves
 * through the leaf's back-pointer, which the build wired before returning.
 */
static
void ft_free_branch_unpublished(struct cds_ft *ft,
		struct cds_ft_inode_flag *top, struct cds_ft_inode_flag *leaf)
{
	while (top && top != leaf) {
		struct cds_ft_inode_flag *next;

		if (ft_node_skip_compressed(top)) {
			struct cds_ft_compressed_node *cn =
				ft_skip_to_compressed(ft, top);

			next = cn->child;
			free_compressed_node_unpublished(ft, cn);
		} else if (ft_node_compressed(top)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(top);

			next = cn->child;
			free_compressed_node_unpublished(ft, cn);
		} else if (ft_node_external(top)) {
			/* Only @leaf may be external on a fresh branch. */
			assert(top == leaf);
			break;
		} else {
			uint8_t v;

			next = ft_node_get_direction(ft, top, -1, &v,
				FT_RIGHT, false);
			free_cds_ft_node_unpublished(ft, ft_node_ptr(top));
		}
		top = next;
	}
}

/*
 * ft_compress_single_child_if_needed: convert a 1-child internal node
 * (no external_nodes) to a compressed/skip-encoded node when the trie
 * group has skip-compressed enabled.  Used at graft sites where a
 * moved root is being placed at a non-root position.  The moved root
 * was an internal at trie-root position.  Under skip mode, a non-root
 * 1-child internal without external_nodes must be a compressed
 * (chain-compress invariant).
 *
 * Two cases on the single child:
 *  - non-compressed:    build a 1-byte cn(byte) -> single_child.
 *  - compressed (or
 *    skip-encoded):     chain-merge -- build a cn(byte ++
 *                       single_cn.key_bytes) -> single_cn.child,
 *                       free the absorbed cn.  Bounded by
 *                       FT_SKIP_LEN_MAX; on overflow leave @child
 *                       unchanged.
 *
 * Returns the converted compressed/skip-encoded flag on success.
 * Returns @child unchanged when conversion isn't applicable (and the
 * unchanged @child is itself canonical at the target position):
 *   - skip-compressed mode is disabled,
 *   - @child is not an internal node,
 *   - @child has more than one child,
 *   - @child has external_nodes attached,
 *   - chain-merge length would overflow FT_SKIP_LEN_MAX.
 * Returns (void *)(long)-ENOMEM on allocation failure.  Conversion WAS
 * required (a non-root 1-child internal) but could not be done.  So the
 * caller must NOT publish @child non-canonically: it must fail the
 * mutation.  @child is left untouched in that case (nothing freed), so
 * the caller can still roll it back.
 *
 * On successful conversion the old internal is freed.  Write-side
 * only (mutex held); the caller is the sole owner of @child.
 *
 * @glue: when non-NULL, build-invisible mode for the graft transaction.
 * The wrapper @cn is built, but these steps are DEFERRED to the post-sync
 * commit (recorded in @glue) rather than performed here:
 *  - the re-parent of the live @cn->child;
 *  - the free of the old internal;
 *  - the free of any absorbed sub-cn.
 * The wrapper is tracked in @glue so an OOM elsewhere in the build frees
 * it.  When NULL, the legacy immediate path runs (caller has already
 * drained the source).
 */
static
struct cds_ft_inode_flag *ft_compress_single_child_if_needed(struct cds_ft *ft,
		struct cds_ft_inode_flag *child,
		struct ft_glue *glue)
{
#ifdef FEATURE_FT_SKIP_COMPRESSED
	struct cds_ft_inode *node;
	unsigned int type_index;
	const struct cds_ft_type *type;
	struct cds_ft_metadata *meta;
	uint8_t byte;
	struct cds_ft_inode_flag *single_child;
	struct cds_ft_compressed_node *single_cn = NULL;
	unsigned int single_len = 0;
	struct cds_ft_compressed_node *cn;
	struct cds_ft_metadata *cn_meta;
	struct cds_ft_inode_flag *cflag;
	unsigned int cn_len;

	if (!ft_group_skip_compressed(ft->group))
		return child;
	if (!ft_node_internal(child))
		return child;
	node = ft_node_ptr(child);
	type_index = ft_node_type(child);
	type = &ft_types[type_index];
	meta = cds_ft_item_to_metadata_fast(node, type->order);
	if (ft_meta_nr_child(meta) != 1 || meta->external_nodes != NULL)
		return child;

	/*
	 * Find the single set byte.  ft_node_get_minmax returns the
	 * resolved child (skip-encoded -> compressed flag).  But we
	 * need the raw slot value, to preserve any skip encoding when
	 * placing it as cn->child.  Walk via the low-level get_ith.
	 */
	switch (type->type_class) {
	case FT_POPCOUNT:
	{
		/*
		 * The first NON-NULL position, not position 0.  A bitmap bit
		 * stays set over a soft-deleted slot: the in-place delete
		 * tier NULLs the slot and decrements nr_child, and never
		 * clears the bit.  So position 0 can be a hole while the one
		 * child @nr_child counts sits further on.  The pointer is the
		 * source of truth.
		 */
		unsigned int i, pc = ft_popcount_node_get_nr_child(type, node);

		single_child = NULL;
		byte = 0;
		for (i = 0; i < pc && !single_child; i++)
			ft_popcount_node_get_ith_pos(type, node, (uint8_t) i,
				&byte, &single_child);
		break;
	}
	case FT_PIGEON:
	{
		unsigned int i;

		single_child = NULL;
		byte = 0;
		for (i = 0; i < FT_ENTRY_PER_NODE; i++) {
			struct cds_ft_inode_flag *v =
				ft_pigeon_node_get_ith_pos(type, node, i);
			if (v) {
				byte = (uint8_t) i;
				single_child = v;
				break;
			}
		}
		break;
	}
	default:
		return child;
	}
	if (!single_child)
		return child;

	/*
	 * Chain-merge when the single child is itself a compressed
	 * (or skip-encoded) cn.  Absorb its path bytes, so the result
	 * is one compressed spanning [byte ++ single_cn->key_bytes]
	 * -> single_cn->child.  Preserves the "no two adjacent
	 * compresseds" invariant.
	 */
	if (ft_node_skip_compressed(single_child))
		single_cn = ft_skip_to_compressed(ft, single_child);
	else if (ft_node_compressed(single_child))
		single_cn = ft_compressed_node_ptr(single_child);
	if (single_cn) {
		single_len = single_cn->len;
		if (1U + single_len > FT_CN_LEN_MAX) {
			/* Overflow: leave un-canonicalized. */
			return child;
		}
	}
	cn_len = 1U + single_len;

	cn = alloc_compressed_node(ft, cn_len, &cn_meta);
	if (!cn)
		return (struct cds_ft_inode_flag *) (long) -ENOMEM;
	cn->len = (uint8_t) cn_len;
	cn->key_bytes[0] = byte;
	if (single_cn) {
		memcpy(&cn->key_bytes[1], single_cn->key_bytes, single_len);
		cn->child = single_cn->child;
	} else {
		cn->child = single_child;
	}
	ft_meta_nr_child_set(cn_meta, 1);
	ft_nr_keys_store(ft,cn_meta, ft_nr_keys_get(meta), CMM_RELAXED);
	cflag = ft_compressed_node_flag(cn);
	if (glue) {
		/*
		 * Build-invisible path (graft transaction).  @node and
		 * @single_cn are LIVE source data that must survive an OOM
		 * elsewhere in the build.  @cn->child's back-pointer must
		 * not be flipped until the post-sync commit.  Defer all three.
		 * Track @cn so the abort path frees it.  The deferred edge
		 * records the PLAIN @cflag: ft_set_parent recovers @cn
		 * directly and records its skip_slot at commit.
		 */
		ft_glue_track(glue, cflag);
		ft_glue_defer_edge(ft, glue, cn->child, cflag, &cn->child);
		ft_glue_defer_free(glue, node, false);
		if (single_cn)
			ft_glue_defer_free(glue, single_cn, true);
		/*
		 * Return the PLAIN flag, NOT the skip form.  @cn->child's
		 * back-pointer is deferred, so a skip pointer would not yet
		 * resolve: ft_skip_to_compressed recovers @cn via that stale
		 * back-pointer.  The caller installs @cn directly
		 * (chain-merge recovers it via ft_compressed_node_ptr).  It
		 * re-encodes the holding slot to the skip form before
		 * publish.  This is the same sequence as the split's suffix.
		 */
		return cflag;
	}
	ft_set_parent(ft, cn->child, cflag, &cn->child);
	/*
	 * Legacy immediate path.  The caller has already unlinked @node
	 * from src and waited a grace period (cds_ft_graft / graft_swap
	 * protocol).  @node has not been published to dst.  No reader can
	 * be inside it, so the immediate-free path is safe.  It saves a
	 * grace period of deferred-free pressure.  The same applies to
	 * single_cn (the compressed sub-node we just absorbed): it was
	 * reachable only via @node, which is itself unpublished here.
	 */
	free_cds_ft_node_unpublished(ft, node);
	if (single_cn)
		free_compressed_node_unpublished(ft, single_cn);
	return ft_publish_compressed(ft, cn, cflag);
#else
	(void) ft;
	(void) glue;
	return child;
#endif
}

/*
 * Try to create a compressed path for a chain of single-child nodes.
 * Returns the compressed node flag on success, NULL if compression
 * is not applicable (path too short) or disabled, -ENOMEM cast to
 * pointer on allocation failure.
 *
 * @glue: when non-NULL (graft build-invisible mode), the live child's
 * back-pointer is recorded as a deferred edge rather than set now.  An
 * absorbed compressed @child (a fresh canonicalization wrapper) is
 * untracked from the glue as it is freed during the merge.
 */
#ifdef FEATURE_FT_COMPRESS
static
struct cds_ft_inode_flag *ft_try_compress_chain(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, unsigned int level,
		struct cds_ft_inode_flag *child,
		struct cds_ft_node *external_nodes __attribute__((unused)),
		struct ft_glue *glue)
{
	uint8_t path_len = (uint8_t)(key_len - level);
	struct cds_ft_compressed_node *cn;
	struct cds_ft_metadata *cn_meta;
	struct cds_ft_compressed_node *child_cn = NULL;
	unsigned int child_len = 0;
	uint8_t merged_len;
	struct ft_held_anchor absorb_held;
	bool absorb_fenced = false;
	int j;

	/*
	 * Length-1 compressed nodes are canonical under
	 * FEATURE_FT_SKIP_COMPRESSED.  The publish wraps cn into a
	 * SKIP_X-tagged slot pointer.  That pointer dispatches for free
	 * relative to the 1-child internal node it replaces.
	 */
	if (path_len < 1)
		return NULL;

	/*
	 * Chain-merge: if @child is already a compressed (or skip-
	 * compressed) node, wrapping it in another compressed prefix
	 * would violate the "no two adjacent compresseds" invariant.
	 * Absorb the child's path bytes into the outer cn so the
	 * result is a single compressed spanning
	 * (key[level..key_len-1] ++ child_cn->key_bytes) ->
	 * child_cn->child.  Bounded by FT_CN_LEN_MAX, which a key of
	 * FT_MAX_KEY_LEN bytes cannot exceed below the root.
	 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_node_skip_compressed(child))
		child_cn = ft_skip_to_compressed(ft, child);
	else
#endif
	if (ft_node_compressed(child))
		child_cn = ft_compressed_node_ptr(child);
	if (child_cn) {
		child_len = child_cn->len;
		/*
		 * Cap the fused path at what one compressed node can hold: its
		 * uint8_t len field (FT_CN_LEN_MAX), with or without
		 * skip-compression.  The merged node need NOT stay
		 * skip-encodable.  ft_publish_compressed publishes a longer run
		 * through a plain compressed flag.  Capping at FT_SKIP_LEN_MAX
		 * left two adjacent compresseds for every longer run
		 * (cds_ft_graft and cds_ft_graft_swap on keys of up to 250
		 * bytes).  That broke the invariant this fuse exists to keep.
		 */
		if ((unsigned int) path_len + child_len > FT_CN_LEN_MAX) {
			/* Overflow: leave adjacency in place. */
			child_cn = NULL;
			child_len = 0;
		}
	}
	/*
	 * ☠ FENCE A LIVE NODE BEFORE READING ITS BYTES INTO THE RUN.
	 *
	 * @child_cn is now final.  When it is LIVE, this build is about to copy
	 * its run into @cn and RETIRE it.  Two things require a lock over that:
	 *  - the read window.  A peer that mutates @child_cn between the copy
	 *    and the commit is retired with it (see the header of
	 *    ft_glue_defer_free_fenced).
	 *  - the retire's RECORD.  A committer that fuses its free list parks
	 *    the tombstone SW.  The STATE-WORD PROTOCOL permits that for step 2
	 *    only because step 1 took the word.
	 *
	 * Take the lock HERE, and not at the retire below.  This is the last
	 * point before any allocation.  So a miss returns with NOTHING to
	 * unwind.  @record_only is the gate.  It names exactly the callers
	 * whose commit is still AHEAD of this build.  For them, an unfenced
	 * retire is also a ONE-WAY mark that an aborting attempt leaves
	 * behind.  ft_dlm_lock then refuses that word forever.  Every other
	 * caller keeps its existing unfenced retire.
	 *
	 * ☞ TWO WAYS TO GET THE DEPTH.  The caller-supplied one is not
	 * redundant.
	 *  - @absorb_node means the caller already took this fence.  It used a
	 *    depth that only it could compute: a node on the op's SRC path,
	 *    which @glue's descent does not describe.  Handing that fence over
	 *    is the only thing that keeps such a site from missing forever.
	 *  - Where the node IS on the descent's path, FT_DEPTH_FROM_DESCENT
	 *    lets the acquire compute the depth itself.  A lookup miss is then
	 *    the same clean re-descend that a contended acquire already
	 *    produces.
	 */
	if (child_cn && glue && glue->record_only &&
			!ft_glue_is_fresh(ft, glue,
				ft_compressed_node_flag(child_cn))) {
		if (glue->absorb_node == child_cn) {
			absorb_held = glue->absorb_held;
			absorb_fenced = true;
			/*
			 * CONSUMED: this function owns the mark from here, and
			 * the retire below files it.  Clearing the hand-off
			 * tells ft_glue_clear_fenced NOT to release it a second
			 * time.  Symmetrically, the hand-off left set lets it
			 * release a fence the caller took on a build that
			 * bailed before it reached this absorb.
			 */
			glue->absorb_node = NULL;
		} else {
			struct ft_lock_ctx actx;

			ft_glue_lock_ctx(glue, &actx);
			if (ft_acquire_member(ft, &actx,
					ft_compressed_node_flag(child_cn),
					cds_ft_item_to_metadata(
						(struct cds_ft_inode *) child_cn),
					FT_DEPTH_FROM_DESCENT, &absorb_held)) {
				glue->absorb_miss = true;
				return (struct cds_ft_inode_flag *) (long) -ENOMEM;
			}
			absorb_fenced = true;
		}
	}
	merged_len = (uint8_t)(path_len + child_len);

	cn = alloc_compressed_node(ft, merged_len, &cn_meta);
	if (!cn) {
		/*
		 * ☠ THE ONLY EXIT BETWEEN THE FENCE AND THE RETIRE THAT FILES IT.
		 * Until ft_glue_defer_free_fenced below hands the mark to the glue,
		 * no choke point owns it.  ft_glue_abort's ft_glue_clear_fenced
		 * sweeps free-list entries, and this entry does not exist yet.
		 * So bailing here would leave the word LOCKED with nothing left
		 * to release it.  ft_dlm_lock refuses a locked word.  So the
		 * node would be unmutable for the life of the trie.  Release it
		 * by hand.
		 *
		 * A SHARED anchor took nothing and owes nothing.  The acquire
		 * that first took that word owns its release, exactly as
		 * ft_glue_clear_fenced reasons.
		 */
		/*
		 * STRICT: the paragraph above is the ownership proof.  No
		 * choke point owns this mark yet.  So nothing of ours can have
		 * consumed it, and no peer can have taken it while we hold it.
		 */
		if (absorb_fenced && !absorb_held.shared && absorb_held.lock)
			ft_meta_lock_release(absorb_held.lock);
		return (struct cds_ft_inode_flag *) (long) -ENOMEM;
	}
	if (child_cn)
		cn->child = child_cn->child;
	else
		cn->child = child;
	cn->len = merged_len;
	for (j = 0; j < path_len; j++)
		cn->key_bytes[j] = key[level + j];
	if (child_cn)
		memcpy(&cn->key_bytes[path_len],
			child_cn->key_bytes, child_len);
	ft_meta_nr_child_set(cn_meta, 1);
	ft_nr_keys_store(ft,cn_meta, 1, CMM_RELAXED);
	/* Compressed nodes must not carry external_nodes. */
	assert(!external_nodes);
	{
		struct cds_ft_inode_flag *cflag = ft_compressed_node_flag(cn);

		if (glue) {
			/*
			 * Build-invisible (graft): cn->child is LIVE.  It is
			 * either the merged-away child_cn's grandchild or the
			 * leaf itself.  Record its back-pointer for the
			 * post-sync commit instead of flipping it now.
			 *
			 * The absorbed child_cn is freed in one of two ways:
			 *  - a fresh canonicalization wrapper (tracked in
			 *    @glue) is dropped from tracking and freed here.
			 *    So the abort path cannot double-free it.
			 *  - a LIVE compressed leaf is never tracked.  It is a
			 *    re-rooted-in-place merge source absorbed into the
			 *    branch run.  It stays reader-reachable until the
			 *    commit, so its free is DEFERRED past the grace
			 *    period instead.
			 *
			 * Track the PLAIN @cflag, not the skip form returned to
			 * the caller.  The abort path resolves a tracked node
			 * via ft_compressed_node_ptr.  So it must not depend on
			 * cn->child's back-pointer, which is still deferred.  A
			 * skip pointer recovers its compressed node through
			 * that back-pointer.
			 */
			ft_glue_defer_edge(ft, glue, cn->child, cflag,
				&cn->child);
			if (child_cn) {
				if (ft_glue_is_fresh(ft, glue,
						ft_compressed_node_flag(child_cn))) {
					ft_glue_untrack(ft, glue, child_cn);
					free_compressed_node_unpublished(ft,
						child_cn);
				} else if (absorb_fenced) {
					/*
					 * FENCED ABOVE -- by this function, or by a
					 * caller that handed its anchor over in
					 * @glue->absorb_node.  Either way the retire
					 * rides the committer's txn as
					 * {LOCK|s -> TOMBSTONE|s} instead of a
					 * standalone one-way mark.
					 */
					ft_glue_defer_free_fenced(glue, child_cn,
						true, &absorb_held);
				} else {
					ft_glue_defer_free(glue, child_cn,
						true);
				}
			}
			ft_glue_track(glue, cflag);
			/*
			 * Emit the creation trace but return the PLAIN flag.
			 * The caller installs @cn directly and resolves it via
			 * ft_compressed_node_ptr.  cn->child's back-pointer is
			 * deferred, so the skip form would not yet resolve.
			 * The caller re-encodes the holding slot to skip before
			 * publish.
			 */
			(void) ft_publish_compressed(ft, cn, cflag);
			return cflag;
		}
		/*
		 * HIDDEN, and the @glue branch above is what proves it.  A LIVE
		 * @cn->child is the GLUE case.  That branch says so: "cn->child
		 * is LIVE ... Record its back-pointer for the post-sync commit
		 * instead of flipping it now".  That branch RETURNS, so this
		 * line runs only with @glue == NULL.  The absorb fence is also
		 * gated on @glue, so no live child_cn reaches here either.
		 *
		 * With @glue NULL there are exactly two entry paths.  Both
		 * carry an unpublished child:
		 *  - ft_attach_node passes the cluster it is building from the
		 *    caller's new external node;
		 *  - ft_build_branch(glue == NULL) has a single such caller,
		 *    ft_insert_compressed_past_child, which passes the fresh
		 *    @node.  ft_build_branch's contract says that the live leaf
		 *    is the non-NULL-glue mode.
		 */
		ft_set_parent_excl(ft, cn->child, cflag, &cn->child,
			FT_EXCL_HIDDEN);
		if (child_cn)
			free_compressed_node_unpublished(ft, child_cn);
		/* compressed_publish emitted by ft_publish_compressed. */
		return ft_publish_compressed(ft, cn, cflag);
	}
}
#else
static inline
struct cds_ft_inode_flag *ft_try_compress_chain(
		struct cds_ft *ft __attribute__((unused)),
		const uint8_t *key __attribute__((unused)),
		size_t key_len __attribute__((unused)),
		unsigned int level __attribute__((unused)),
		struct cds_ft_inode_flag *child __attribute__((unused)),
		struct cds_ft_node *external_nodes __attribute__((unused)),
		struct ft_glue *glue __attribute__((unused)))
{
	return NULL;
}
#endif

/*
 * Root-cluster builders: assemble a fresh root internal node from a glue
 * transaction.  The graft_swap re-root uses them.  ft-detach uses them when a
 * detach re-roots the trie.  ft_make_root_internal_glue layers over
 * ft_build_extracted_root_glue, so the latter precedes it.
 */
/*
 * Build (invisibly) a swap_ft root node holding the extracted subtree
 *   @first_byte ++ @rest[0 .. @rest_len-1]  ->  @child
 * That is an internal root whose @first_byte slot leads to the LIVE @child.
 * When @rest_len >= 1, it leads there via a fresh compressed suffix.  Used
 * (via ft_make_root_internal_glue) by the extract side of cds_ft_graft_swap
 * and by ft_detach_keylen's pre-publish root materialization.
 *
 * @child is LIVE data relocated into swap_ft:
 *  - its back-pointer is recorded as a deferred edge in @glue rather than
 *    flipped now;
 *  - every fresh node is tracked.  So an OOM elsewhere in the swap build
 *    frees the cluster (both tries pristine, nothing published).
 * Nothing is freed here.
 *
 * @child is always a plain (internal / external) node.  A compressed node's
 * child is plain by the chain-merge invariant.  The suffix path is held by
 * the fresh compressed @rest.  So no chain-merge is needed.
 *
 * Returns the new internal-tagged root flag, or (void *)(long)-ENOMEM.
 */
static
struct cds_ft_inode_flag *ft_build_extracted_root_glue(struct cds_ft *ft,
		struct ft_glue *glue,
		uint8_t first_byte, const uint8_t *rest, unsigned int rest_len,
		struct cds_ft_inode_flag *child, unsigned long subtree_count)
{
	struct cds_ft_inode_flag *slot_value;
	struct cds_ft_inode_flag *skip_value = NULL;
	struct cds_ft_compressed_node *new_cn = NULL;
	struct cds_ft_inode *root_node;
	struct cds_ft_metadata *root_meta;
	struct cds_ft_inode_flag *dest;
	struct cds_ft_inode_flag **slot = NULL;
	int ret;

	if (rest_len == 0) {
		slot_value = child;	/* LIVE; back-pointer deferred below. */
	} else {
		struct cds_ft_metadata *new_cn_meta;

		new_cn = alloc_compressed_node(ft, rest_len, &new_cn_meta);
		if (!new_cn)
			return (struct cds_ft_inode_flag *) (long) -ENOMEM;
		new_cn->len = (uint8_t) rest_len;
		new_cn->child = child;
		memcpy(new_cn->key_bytes, rest, rest_len);
		ft_meta_nr_child_set(new_cn_meta, 1);
		ft_nr_keys_store(ft,new_cn_meta, subtree_count, CMM_RELAXED);
		slot_value = ft_compressed_node_flag(new_cn);	/* PLAIN */
		ft_glue_track(glue, slot_value);
		/* @child (live) -> new_cn, deferred to the post-sync commit. */
		ft_glue_defer_edge(ft, glue, child, slot_value, &new_cn->child);
		/* Skip form for the root slot (resolves once the edge applies). */
		skip_value = ft_publish_compressed(ft, new_cn, slot_value);
	}

	root_node = alloc_cds_ft_node(ft, &ft_types[0], &root_meta);
	if (!root_node)
		/* new_cn (if any) is tracked in @glue; the caller's abort frees it. */
		return (struct cds_ft_inode_flag *) (long) -ENOMEM;
	dest = ft_node_flag(root_node, 0);
	/*
	 * @root_node is a BUILD node, and the set says so (@cluster_leaf true).
	 * So the store lands in place.  The child's back-pointer is this
	 * frame's job:
	 *  - deferred below when @child is live (rest_len == 0);
	 *  - set explicitly below when the slot holds the fresh new_cn.  That
	 *    is a fresh-to-fresh edge, safe during the build.
	 *
	 * ☠ IT USED TO SAY "live" FOR rest_len >= 1.  A first occupancy on a
	 * live popcount node is not stored in place.  ft_node_set_nth
	 * recompacted the empty root into a fresh copy (ADD_SAME).  It handed
	 * the displaced body to nobody (@old_node_ret NULL).  So ONE INTERNAL
	 * NODE LEAKED per cds_ft_graft_swap / cds_ft_detach whose extracted run
	 * was two bytes or longer.  MEASURED with the group node balance.
	 * ft_inv's inv_ordered_bulk_consistency reported a six-node "internal
	 * node leak" on every DEBUG_COUNTERS run.  That was exactly its six
	 * swaps.  A one-shot swap or detach of a two-byte run leaks one.
	 * Every other fresh-node set_nth in the tree does one of two things.
	 * It starts from a NULL target (no body to displace).  Or it hands
	 * the old body back and frees it.
	 */
	ret = ft_node_set_nth(ft, &dest, first_byte, slot_value,
			NULL, root_meta, 0, true /* cluster_leaf: build node */);
	if (ret)
		return (struct cds_ft_inode_flag *) (long) -ENOMEM;
	ft_glue_track(glue, dest);
	ft_nr_keys_store(ft,ft_flag_to_metadata(ft, dest), subtree_count, CMM_RELAXED);
	ft_node_get_nth_skip(dest, &slot, first_byte, FT_PF_NONE);
	if (rest_len == 0) {
		ft_glue_defer_edge(ft, glue, child, dest, slot);
	} else {
		/* Re-encode the root slot to the skip form (new_cn is compressed). */
		if (skip_value && skip_value != slot_value && slot)
			*slot = skip_value;
		/*
		 * HIDDEN: this arm's @slot_value is the FRESH compressed node
		 * allocated above for the remaining run.  The LIVE case is the
		 * OTHER branch, and the code labels it there.  In that branch
		 * slot_value = child is marked LIVE, with its back-pointer
		 * deferred.  That branch routes it to ft_glue_defer_edge
		 * instead of here.
		 */
		ft_set_parent_excl(ft, slot_value, dest, slot,
			FT_EXCL_HIDDEN);
	}
	return dest;
}

/*
 * Build-invisible internal-root materialization, preserving the trie-wide
 * invariant that the root pointer always tags an internal node (never
 * compressed, never skip-compressed).  Used by cds_ft_graft_swap's extract
 * side and by ft_detach_keylen.  Materializes an internal-node root from the
 * LIVE displaced @old_child.  It neither publishes nor mutates live data:
 *  - fresh nodes are tracked in @glue;
 *  - the moved grandchild's back-pointer is deferred;
 *  - the peeled-away compressed node is recorded for deferred free.
 * Nothing is freed here.
 *
 *   - @old_child internal:    returned unchanged (already a valid internal
 *                             root; the caller clears its parent at commit).
 *                             No glue node, no deferred edge.
 *   - @old_child compressed:  peel the first path byte into a fresh internal
 *                             root.  Peel the rest (if any) into a fresh
 *                             compressed node.  Defer the live grandchild's
 *                             back-pointer.  Record the old compressed node
 *                             for deferred free.
 *
 * External @old_child must be filtered by the caller (externals attach as
 * external_nodes, not via this helper).
 *
 * Returns the new internal-tagged root flag, or (void *)(long)-ENOMEM.
 */
static
struct cds_ft_inode_flag *ft_make_root_internal_glue(struct cds_ft *ft,
		struct ft_glue *glue, struct cds_ft_inode_flag *old_child)
{
	struct cds_ft_compressed_node *cn;
	struct cds_ft_metadata *cn_meta;
	struct cds_ft_inode_flag *root;

	old_child = ft_resolve_skip_compressed(ft, old_child);
	if (caa_likely(!ft_node_compressed(old_child)))
		return old_child;	/* already internal */
	cn = ft_compressed_node_ptr(old_child);
	cn_meta = cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
	root = ft_build_extracted_root_glue(ft, glue,
			cn->key_bytes[0], &cn->key_bytes[1], cn->len - 1,
			cn->child, ft_nr_keys_get(cn_meta));
	if (root == (struct cds_ft_inode_flag *) (long) -ENOMEM)
		return root;
	/* Reclaim the peeled-away compressed node after the commit. */
	ft_glue_defer_free(glue, cn, true);
	return root;
}
