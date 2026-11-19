// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-inequality.h
 *
 * Userspace RCU library - Fractal Trie: inequality lookups: le / ge / lt / gt / first / last.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-inequality.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * Iterator-based inequality lookup. The input key and key_len are read
 * from @iter (set via cds_ft_iter_set_key). On success the result key,
 * key length, node, status, and path are written back into @iter so that
 * subsequent iteration / backtracking calls can reuse the state.
 *
 * @limit overrides the key: FT_LOOKUP_LIMIT_FIRST uses key_len
 * prefix_len, FT_LOOKUP_LIMIT_LAST uses max_key_len.
 *
 * Prefix-scoped traversal: when iter->prefix_len > 0, the traversal
 * is confined to the subtree rooted at the prefix. Backtracking stops
 * at the prefix boundary instead of the root. LIMIT_FIRST finds the
 * smallest key within the prefix subtree. LIMIT_LAST descends using
 * the actual prefix key bytes followed by 0xFF to find the greatest
 * key within the prefix subtree.
 */
/*
 * Handle a compressed node during inequality descent.
 *
 * Matches key bytes against the compressed path (respecting the
 * limit mode), fills ordinal_key and iter_path, and determines
 * the action: continue descent, break, go up, or descend into
 * children.
 *
 * On mismatch, the direction relative to the lookup mode decides
 * whether to backtrack (GOING_UP) or descend into the compressed
 * subtree (DESCEND_CHILDREN).
 */
static inline_lookup
enum ft_descent_action ft_inequality_compressed(struct cds_ft_inode_flag **node_flag_p,
		ssize_t *level_p, ssize_t key_depth,
		enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit,
		const uint8_t **iter_key_p,
		const uint8_t *input_key,
		struct cds_ft_iter *iter,
		uint8_t *ordinal_key,
		bool *skip_eq_external_nodes_p,
		const bool fill_ordinal)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(node_flag);
	int level = *level_p;
	int remaining = key_depth - level;
	int cmp = cn->len < remaining ? cn->len : remaining;
	const uint8_t *cmp_key;
	uint8_t last_key_buf[FT_MAX_KEY_LEN];
	unsigned int mpos = 0;
	int cmp_result;
	int j;

	FT_TP(ineq_compressed_enter, (const void *) cn, level, (int) mode);

	/* Build contiguous comparison key for this limit mode. */
	switch (limit) {
	case FT_LOOKUP_LIMIT_NONE:
		cmp_key = *iter_key_p;
		break;
	case FT_LOOKUP_LIMIT_FIRST:
		cmp_key = input_key + level - 1;
		break;
	case FT_LOOKUP_LIMIT_LAST: {
		unsigned int prefix_bytes = 0;

		if ((size_t)level <= iter->prefix_len) {
			prefix_bytes = iter->prefix_len - level + 1;
			if (prefix_bytes > (unsigned int)cmp)
				prefix_bytes = cmp;
			memcpy(last_key_buf, input_key + level - 1,
				prefix_bytes);
		}
		if (prefix_bytes < (unsigned int)cmp)
			memset(last_key_buf + prefix_bytes, 0xff,
				cmp - prefix_bytes);
		cmp_key = last_key_buf;
		break;
	}
	default:
		cmp_key = NULL;
		assert(0);
	}

	cmp_result = ft_key_cmp_ordinals(cmp_key, cn->key_bytes, cmp, cmp,
					true, &mpos);

	/* Fill ordinal_key and iter_path for the matched prefix. */
	if (fill_ordinal)
		for (j = 0; j < (cmp_result ? (int)mpos : cmp); j++)
			ordinal_key[level - 1 + j] = cmp_key[j];
	/* Advance iter_key for LIMIT_NONE. */
	if (limit == FT_LOOKUP_LIMIT_NONE)
		*iter_key_p += (cmp_result ? mpos + 1 : (unsigned int)cmp);

	if (cmp_result) {
		/*
		 * Mismatch at position mpos.  Check direction
		 * relative to the lookup mode.
		 *
		 * key > path at mismatch: GE/GT go up, LE/LT descend.
		 * key < path at mismatch: GE/GT descend, LE/LT go up.
		 */
		if (fill_ordinal)
			ordinal_key[level - 1 + mpos] = cmp_key[mpos];
		if ((cmp_result > 0 && (mode == FT_LOOKUP_GE || mode == FT_LOOKUP_GT)) ||
		    (cmp_result < 0 && (mode == FT_LOOKUP_LE || mode == FT_LOOKUP_LT))) {
			/*
			 * Going-up: the whole compressed subtree is on the wrong
			 * side of the key, so back-track from the compressed node
			 * itself.  Return its SHALLOW boundary depth (@level, where
			 * its internal parent is the dispatcher at @level-1), not the
			 * interior divergence position @level+mpos: the divergence
			 * offset inside a compressed span is irrelevant to going-up
			 * (a compressed node has no within-span siblings).  The
			 * parent-pointer cursor seeds up_parent = the internal parent
			 * directly and proceeds to the sibling search.
			 */
			*level_p = level;
			iter_debug_path_snapshot(iter);
			FT_TP(ineq_compressed, (const void *) cn, level,
				cmp_result, mpos, (int) FT_DESCENT_GOING_UP);
			return FT_DESCENT_GOING_UP;
		}
		/* Descend into compressed subtree. */
		if (fill_ordinal) {
			ordinal_key[level - 1 + mpos] = cn->key_bytes[mpos];
			for (j = mpos + 1; j < cn->len; j++)
				ordinal_key[level - 1 + j] = cn->key_bytes[j];
		}
		level += cn->len - 1;
		node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
		assert(node_flag != NULL);	/* compressed node always has a live child */
		/*
		 * The descent result at this position is cn->child -- the
		 * dispatcher for the byte at index level - 1 (the byte AFTER
		 * the compressed prefix), not the compressed node itself.
		 * going_up relies on this to find the sibling of the failing
		 * byte in cn->child.
		 */
		*skip_eq_external_nodes_p = false;
		*node_flag_p = node_flag;
		*level_p = level;
		iter_debug_path_snapshot(iter);
		FT_TP(ineq_compressed, (const void *) cn, level,
			cmp_result, mpos,
			(int) FT_DESCENT_DESCEND_CHILDREN);
		return FT_DESCENT_DESCEND_CHILDREN;
	}

	if (cn->len > remaining) {
		/* Key shorter than compressed path. */
		if (mode == FT_LOOKUP_GE || mode == FT_LOOKUP_GT) {
			int k;

			if (fill_ordinal)
				for (k = cmp; k < cn->len; k++)
					ordinal_key[level - 1 + k] = cn->key_bytes[k];
			level += cn->len - 1;
			node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
			assert(node_flag != NULL);	/* compressed node always has a live child */
			*skip_eq_external_nodes_p = false;
			*node_flag_p = node_flag;
			*level_p = level;
			iter_debug_path_snapshot(iter);
			FT_TP(ineq_compressed, (const void *) cn, level,
				cmp_result, (unsigned int) cmp,
				(int) FT_DESCENT_DESCEND_CHILDREN);
			return FT_DESCENT_DESCEND_CHILDREN;
		}
		/* Going-up from the compressed node: shallow boundary (see
		 * the mismatch case above) -- not the interior @level+cmp-1. */
		*level_p = level;
		iter_debug_path_snapshot(iter);
		FT_TP(ineq_compressed, (const void *) cn, level,
			cmp_result, (unsigned int) cmp,
			(int) FT_DESCENT_GOING_UP);
		return FT_DESCENT_GOING_UP;
	}

	/* Full match: advance past compressed path. */
	level += cn->len - 1; /* -1: for loop increments */
	node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
	assert(node_flag != NULL);	/* compressed node always has a live child (by construction) */
	if (ft_node_external(node_flag))
		goto out_break;

	*node_flag_p = node_flag;
	*level_p = level;
	FT_TP(ineq_compressed, (const void *) cn, level,
		cmp_result, (unsigned int) cmp,
		(int) FT_DESCENT_CONTINUE);
	return FT_DESCENT_CONTINUE;

out_break:
	*node_flag_p = node_flag;
	*level_p = level;
	FT_TP(ineq_compressed, (const void *) cn, level,
		cmp_result, (unsigned int) cmp,
		(int) FT_DESCENT_BREAK);
	return FT_DESCENT_BREAK;
}

/*
 * Handle a compressed node during the inequality minmax descent.
 *
 * On LEFTMOST (GE/GT) descent, check external_nodes at the
 * compressed node's entry depth first; if present and not
 * suppressed, return FT_DESCENT_FOUND_MINMAX with *ret_node_p
 * set and *level_p decremented by one, so the caller can jump
 * straight to found_minmax.
 *
 * Otherwise fill ordinal_key and iter_path across every level
 * spanned by the compressed path, advance @level to the span's
 * end, and step to cn->child.  Returns FT_DESCENT_BREAK when
 * the child is external (descent is done), otherwise
 * FT_DESCENT_CONTINUE.
 */
static inline_lookup
enum ft_descent_action ft_inequality_minmax_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		ssize_t *level_p,
		struct cds_ft_node **ret_node_p,
		bool *skip_eq_external_nodes_p,
		uint8_t *ordinal_key,
		enum ft_direction dir,
		const bool fill_ordinal)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(node_flag);
	ssize_t level = *level_p;

	if (dir == FT_LEFTMOST) {
		struct cds_ft_metadata *cn_meta = cds_ft_item_to_metadata(
			(struct cds_ft_inode *) cn);
		struct cds_ft_node *ext =
			ft_dereference_external(cn_meta->external_nodes);

		if (ext && !*skip_eq_external_nodes_p) {
			*ret_node_p = ext;
			*level_p = level - 1;
			return FT_DESCENT_FOUND_MINMAX;
		}
	}
	/*
	 * Fill ordinal_key and path entries for every level spanned
	 * by the compressed path.  The going-up code needs a valid
	 * entry at each level to call ft_node_get_direction (which
	 * returns NULL for siblings, causing the going-up walk to
	 * continue ascending).
	 */
	if (fill_ordinal)
		ft_fill_compressed_path(cn, ordinal_key, level - 1);
	level += cn->len - 1;
	node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
	if (!node_flag) {
		/*
		 * Invariant violation: a reachable compressed node always
		 * has a non-NULL child, *even transiently*.  cn->child is
		 * wired before the compressed is published to its parent's
		 * slot and is never cleared in place (empty compresseds
		 * are pruned by replacing the parent's slot with the
		 * compressed's external_nodes, or by detaching the whole
		 * branch -- either way the compressed itself is no longer
		 * reachable when it becomes empty).
		 *
		 * Unlike the internal minmax == NULL case (transiently
		 * observable and handled via going_up above/below), there
		 * is no race window in which a reachable compressed has
		 * cn->child == NULL.  Observing NULL here is a real bug:
		 * abort loudly instead of silently propagating a corrupt
		 * node_flag.
		 */
		fprintf(stderr,
			"BUG: cds_ft_lookup_inequality minmax: "
			"compressed %p has NULL child\n",
			(const void *) node_flag);
		abort();
	}
	*node_flag_p = node_flag;
	*level_p = level;
	if (ft_node_external(node_flag))
		return FT_DESCENT_BREAK;
	*skip_eq_external_nodes_p = false;
	return FT_DESCENT_CONTINUE;
}

/*
 * Tier-2 inequality descent: the relational key seek (LE/GE/LT/GT/first/last)
 * and the up/down tree traversal it uses to cross subtree boundaries.  Reached
 * only when the tier-1 ordered-cell fast path in cds_ft_lookup_inequality_impl
 * did not apply (an uncached / scoped / first-or-last / seed-from-node entry, or
 * a relational seek from a key).  Inlining governed by inline_ineq: default-off
 * it is one shared, runtime-mode copy; with FEATURE_INLINE_INEQUALITY_LOOKUP it
 * is force-inlined and per-mode specialized at each caller, as before.
 */
static inline_ineq
enum cds_ft_status ft_ineq_descend(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit,
		const bool use_keycopy,
		const bool seed_from_node __attribute__((unused)),
		/*
		 * COHERENCE witness (NULL for every ordinary traversal, and a
		 * compile-time NULL at those call sites, so the accumulation below
		 * folds away entirely): when set, every node this traversal visits
		 * is folded into @wit so a SECOND traversal can be compared against
		 * the first and a torn read detected.  See struct ft_visit_witness.
		 */
		struct ft_visit_witness *wit)
{
	ssize_t key_depth, level;
	struct cds_ft_inode_flag *node_flag;
	struct cds_ft_node *ret_node;
	uint8_t ordinal_key[FT_MAX_KEY_LEN];
	/*
	 * OFF-KEY BOUNDARY: the shallowest level at which the traversal stopped
	 * following the search key and took a branch of its own -- a going-up
	 * sibling, or a min/max child in the descend_children descent.  SSIZE_MAX
	 * = still on the key.
	 *
	 * At and below it the going-up sibling scan must start from the byte the
	 * traversal ACTUALLY took (ordinal_key[level - 1]), never from the search
	 * key: the key names a branch that is already behind us, so scanning from
	 * it hands back a subtree the traversal has already consumed.  Above it
	 * the two agree by construction (the descent fills ordinal_key with the
	 * key's own bytes), and the search key is the cheaper source -- it needs
	 * no ordinal_key under @keep_ordinal == false.
	 *
	 * Declared at the top of the function: every `goto` into going_up /
	 * descend_children is backward or forward past this point, so no jump
	 * skips the init.
	 */
	ssize_t off_key_level = SSIZE_MAX;
	/*
	 * @keep_ordinal: compile-time true for every instantiation EXCEPT
	 * (use_keycopy && limit == LIMIT_NONE).  In that one case the matched leaf
	 * is the sole result-key source (ft_speculative_keycopy_unconditional) and
	 * on-key going-up dispatch reads input_key, so ordinal_key is dead ABOVE
	 * @off_key_level: its memset, per-level descent fills (here and in the
	 * compressed helpers via fill_ordinal), and fallback copies all DCE.  The
	 * two off-key byte-record slots -- the going-up sibling scan and the minmax
	 * scan -- are written on both paths: they are what @off_key_level reads
	 * back, and the store costs the same as a scratch byte.
	 */
	const bool keep_ordinal = !(use_keycopy &&
			limit == FT_LOOKUP_LIMIT_NONE);
	enum ft_direction dir;
	const uint8_t *input_key = NULL;	/* set below; init for the hoisted cell-fastpath goto end */
	const uint8_t *iter_key;
	size_t key_len = 0;
	bool going_up = false, skip_eq_external_nodes;
	/*
	 * Did the key walk already step STRICTLY PAST the search key before
	 * entering the min/max descent?  A compressed divergence does exactly
	 * that: ft_inequality_compressed() answers DESCEND_CHILDREN having
	 * followed cn->child on the greater side of the mismatch, so every key
	 * at or below that child -- INCLUDING the child's own external_nodes --
	 * is already strictly greater than the search key.  Distinct from
	 * @going_up, which records the same fact for the backtracking route.
	 */
	bool descended_past_key = false;
	/*
	 * Parent-pointer going-up cursor (structural up_node form,
	 * mirroring the iter_skip walk-up).  @up_node is the deepest live
	 * node descent established; @up_node_lo is the shallowest depth it
	 * covers -- its TRUE shallow boundary (several levels below @up_node
	 * for a compressed run).  Tracked as descent proceeds so the going-up
	 * seed is live (taken from the descent, not read back from a
	 * recorded path), then climbed via ft_get_parent_rcu up to the
	 * going-up @level.  Seeding from the
	 * deepest node + true shallow boundary (rather than from @node_flag
	 * and the end-of-key-adjusted @level) avoids both the level
	 * adjustment desync and the compressed-span boundary ambiguity.
	 */
	struct cds_ft_inode_flag *up_node = NULL;
	ssize_t up_node_lo = 0;
	/*
	 * @use_keycopy is a compile-time literal at each instantiation (see the
	 * two cds_ft_lookup_inequality_impl callers in the dispatcher below), so
	 * always_inline constant-folds every per-call gate on it.  When set, a
	 * configured speculative skip-compressed group recovers the result key
	 * from the matched leaf and the min-descent follows skip pointers without
	 * reading the compressed node or filling ordinal_key; otherwise the
	 * descent rebuilds ordinal_key from the live compressed nodes (the fill +
	 * re-anchor path).
	 */

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	switch (limit) {
	case FT_LOOKUP_LIMIT_NONE:
		/*
		 * Continuation from the current position: its length may be the
		 * deferred LAZY sentinel (a cell walk that fell to the descent on a
		 * scoped step) -- resolve it from the leaf.  No-op for fixed /
		 * descent / set_key positions.
		 */
		key_len = ft_key_len(ft, ft_iter_resolve_key_len(iter));
		if (!valid_key_len(ft, key_len)) {
			iter->node = NULL;
			iter->cache_valid = false;
			iter_debug_path_update(iter);
			iter->status = CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
			goto end;
		}
		break;
	case FT_LOOKUP_LIMIT_FIRST:
		key_len = iter->prefix_len;
		break;
	case FT_LOOKUP_LIMIT_LAST:
		key_len = ft->group->max_key_len;
		break;
	}

	key_depth = key_len + 1;

	switch (mode) {
	case FT_LOOKUP_GE:
	case FT_LOOKUP_LE:
	case FT_LOOKUP_GT:
	case FT_LOOKUP_LT:
		break;
	default:
		abort();	/* Internal library error. */
	}

	/*
	 * Read the input key in place from the iterator buffer -- no snapshot
	 * copy.  Every write to iter_key(iter) (the result) is terminal (goto
	 * end) or sourced from the separate ordinal_key / leaf-copy buffer, and
	 * all input reads (the descent and the going-up backtrack) precede any
	 * terminal write, so the input bytes are never overwritten while still
	 * needed.  The lone self-aliasing case -- the equal-match write below --
	 * is a no-op and is guarded.
	 *
	 * Input-key source: only a LIMIT_NONE continuation (cds_ft_next/prev, the
	 * relational lookups) iterates from the iterator's CURRENT key, which on a
	 * lazy-ref cell group is read IN PLACE from the live node (no prior copy).
	 * LIMIT_FIRST/LIMIT_LAST are absolute and key off the prefix in iter_key,
	 * NOT the current position, so they must NOT reference the node (a reused
	 * iterator's stale node would mis-seed the search).
	 */
	if (limit == FT_LOOKUP_LIMIT_NONE)
		input_key = ft_iter_read_key(iter);
	else
		input_key = iter_key(iter);
	iter_key = input_key;

	FT_TP(ineq_enter, (int) mode, input_key, key_len);

	if (keep_ordinal)
		memset(ordinal_key, 0, ft->group->max_key_len * sizeof(ordinal_key[0]));
	node_flag = ft_root_dereference_prefetch(ft);
	up_node = node_flag;		/* root covers depth 0 */
	up_node_lo = 0;

	/*
	 * Empty root short-circuit: when the root has no children,
	 * there is nothing to traverse and no inequality match is
	 * possible. An empty root is always a type-0 popcount_2l node
	 * with an all-zero bitmap header.  Compressed roots are never
	 * empty (recompaction replaces emptied compressed roots with
	 * internal nodes).
	 */
	if (!ft_node_compressed(node_flag)) {
		unsigned int type_idx = ft_node_type(node_flag);
		const struct cds_ft_type *type = &ft_types[type_idx];
		struct cds_ft_metadata *metadata = cds_ft_item_to_metadata_fast(
				ft_node_ptr(node_flag), type->order);

		/*
		 * Empty trie: no internal children and no NIL-key entries.
		 * Loading nr_child instead of probing the bitmap works for
		 * every internal node class.
		 */
		if (ft_meta_nr_child_load(metadata) == 0 &&
				!uatomic_load(&metadata->external_nodes, CMM_RELAXED)) {
			iter->node = NULL;
			iter->cache_valid = true;
			iter_debug_path_snapshot(iter);
			iter->path_len = 1;
			iter->status = CDS_FT_STATUS_NOT_FOUND;
			goto end;
		}
	}

	/*
	 * Fast path: reuse the iterator's cached position from a prior
	 * lookup when it is still valid and covers the full key
	 * depth.  This avoids redundant per-level ft_node_get_nth(ft, )
	 * lookups (which are the expensive, cache-miss-prone part of
	 * the downward walk).  The caller must hold the RCU read-side
	 * lock continuously for the cached pointers to remain valid.
	 */
	iter_debug_path_check(iter);
	/*
	 * Continuation fast path: recover the position from iter->node by
	 * backtracking up the live parent chain.  Reuse it only when the
	 * cached position is EXACTLY at this key (path_len == key_depth and
	 * iter->node set) -- iter->node is then the deepest position for the
	 * key.  A deeper cached position (path_len > key_depth, e.g. an
	 * inequality result that landed on a longer key) is not the right
	 * cursor for this key and falls to slow_path.  The caller must hold
	 * the RCU read-side lock continuously for iter->node to stay valid.
	 */
	if (iter->cache_valid && iter->node &&
			(ssize_t)iter->path_len == key_depth &&
			key_depth > 1) {
		for (level = 1; level < key_depth; level++) {
			if (!keep_ordinal)
				continue;
			switch (limit) {
			case FT_LOOKUP_LIMIT_NONE:
				ordinal_key[level - 1] =
					input_key[level - 1];
				break;
			case FT_LOOKUP_LIMIT_FIRST:
				ordinal_key[level - 1] =
					input_key[level - 1];
				break;
			case FT_LOOKUP_LIMIT_LAST:
				if ((size_t) level <= iter->prefix_len)
					ordinal_key[level - 1] =
						input_key[level - 1];
				else
					ordinal_key[level - 1] = 0xff;
				break;
			}
		}
		{
			/*
			 * Cross-call continuation: recover the deepest trie node
			 * for iter->key from the cached position iter->node (the
			 * dup-chain head), not from a recorded descent path.  A
			 * prefix key sits at an internal/compressed holder whose
			 * external_nodes == iter->node; otherwise iter->node is a
			 * leaf child (a tag-0 external flag).  ft_get_parent_rcu
			 * on the head reaches the holder in O(1) (head->prev ==
			 * holder) and never walks the dup chain.
			 */
			struct cds_ft_inode_flag *cur =
				(struct cds_ft_inode_flag *) iter->node;
			struct cds_ft_inode_flag *holder =
				ft_get_parent_rcu(ft, cur);
			unsigned int cont_rewind = 0;

			if (holder && !ft_node_external(holder) &&
			    ft_node_external_nodes(holder) ==
					(struct cds_ft_node *) iter->node) {
				node_flag = holder;
			} else if (holder && !ft_node_external(holder) &&
					!ft_node_compressed(holder) &&
					!ft_node_skip_compressed(holder) &&
					(ft_node_get_nth_reanchor(ft, holder,
						input_key[key_depth - 2],
						&cont_rewind) != cur ||
					 cont_rewind)) {
				/*
				 * ☠ NEITHER SHAPE: DO NOT INFER "SLOT LEAF"
				 * FROM THE FAILED external_nodes EQUALITY.
				 *
				 * The arm below reads a failed equality as "then
				 * @cur is a leaf child", which is a negative
				 * inference over two DIFFERENT shapes.  It is
				 * wrong for a PREFIX-KEY HEAD whose holder has
				 * just had its external_nodes cleared by a
				 * concurrent remove (ft-remove.h, the in-place
				 * one-key clear): the head is still linked and
				 * still reachable, the equality fails, and the
				 * head is then treated as a leaf.  A leaf's
				 * holder sits one level ABOVE it; a prefix-key
				 * head's holder sits AT its own depth.  So the
				 * going-up seed below is planted one level too
				 * shallow, and every key the iteration
				 * reconstructs from there is short by one or
				 * more LEADING bytes -- on the correct node,
				 * with CDS_FT_STATUS_OK, and cached so the next
				 * step inherits it.
				 *
				 * Ask the POSITIVE question instead: a genuine
				 * slot leaf is the value the holder's slot for
				 * this key's last byte actually holds -- asked
				 * with the READER accessor
				 * (ft_node_get_nth_reanchor; ft_node_get_nth is
				 * documented writer/debug-only), and a non-zero
				 * @rewind is itself a "cannot classify", handled
				 * exactly as the descent's own reanchor call
				 * below handles it.
				 *
				 * ☠ THE INDEX IS @key_depth - 2, NOT - 1.
				 * @key_depth is key_len + 1 (see its assignment),
				 * and the slow-path loop consumes
				 * input_key[level - 1] for level in
				 * 1 .. key_depth - 1, so the DEEPEST dispatched
				 * byte -- the one a leaf's holder keys on -- is
				 * input_key[key_depth - 2].  @key_depth - 1 is
				 * one PAST the key: it reads the buffer's
				 * FT_KEY_READABLE_PAD on the buffer source and
				 * runs off the allocation on the IN-LEAF source
				 * (ASAN heap-buffer-overflow), and its stale byte
				 * mismatches most live leaves, dumping ~70% of
				 * leaf continuations onto slow_path.  When
				 * neither shape answers, the cached position
				 * cannot be classified at all -- re-descend from
				 * the root, which is authoritative.
				 *
				 * Compressed / skip-compressed holders keep the
				 * old arm verbatim: they are not implicated
				 * (measured zero on the failing leg) and the
				 * slot question is not asked the same way of a
				 * run.  A holder that is absent or external
				 * likewise falls through unchanged.
				 */
				node_flag = ft_root_dereference_prefetch(ft);
				iter_key = input_key;
				goto slow_path;
			} else {
				node_flag = cur;
			}
		}
		/*
		 * If the cached path entry is a compressed node, the
		 * fast path cannot determine the correct loop exit
		 * state (compressed nodes span multiple levels).
		 * Fall back to the slow path.
		 */
		if (ft_node_compressed(node_flag) ||
		    ft_node_skip_compressed(node_flag)) {
			node_flag = ft_root_dereference_prefetch(ft);
			iter_key = input_key;
			goto slow_path;
		}
		/*
		 * Reconstruct the loop exit value of @level to match
		 * what the traversal loop would have produced:
		 *  - key_depth     if last node is internal (loop ran
		 *                  to completion),
		 *  - key_depth - 1 if last node is external, NULL, or
		 *                  the path ended early (loop broke).
		 */
		if (!ft_node_external(node_flag))
			level = key_depth;
		else
			level = key_depth - 1;
		/*
		 * Cross-call fast path: the cached deepest node is the going-up
		 * seed.  It is never compressed/skip here (those fall back to
		 * slow_path above), so its shallow boundary is its own depth.
		 */
		up_node = node_flag;
		up_node_lo = key_depth - 1;
		FT_TP(fastpath_enter, (int) mode,
			(const void *) node_flag, (int) level);
		goto post_traversal;
	}

slow_path:
	FT_TP(slowpath_enter, (int) mode, (int) iter->cache_valid,
		(int) iter->path_len);
	for (level = 1; level < key_depth; level++) {
		uint8_t key_value;

		if (wit)			/* K-side descent */
			ft_witness_visit(wit, ft_node_ptr(node_flag));
		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;
			ssize_t cmp_entry_level = level;

			act = ft_inequality_compressed(&node_flag,
				&level, key_depth, mode, limit,
				&iter_key, input_key, iter,
				ordinal_key, &skip_eq_external_nodes,
				keep_ordinal);
			if (act == FT_DESCENT_GOING_UP) {
				/*
				 * @node_flag is the compressed node; it occupies
				 * depths [cmp_entry_level-1, +cn->len).  Seed the
				 * cursor at its TRUE shallow boundary so the
				 * going-up climb reaches its internal parent.
				 */
				up_node = node_flag;
				up_node_lo = cmp_entry_level - 1;
				goto going_up;
			}
			/*
			 * Descend / break / continue: @node_flag is cn->child
			 * at the advanced @level (span 1 -- chain-merge forbids
			 * compressed-under-compressed).
			 */
			up_node = node_flag;
			up_node_lo = level;
			if (act == FT_DESCENT_DESCEND_CHILDREN) {
				/*
				 * A DIVERGENCE, not an end-of-key stop: the helper
				 * already cleared @skip_eq_external_nodes for this
				 * exact reason.  Record it so the assignment at the
				 * descent entry does not put the skip back.
				 */
				descended_past_key = true;
				goto descend_children;
			}
			if (act == FT_DESCENT_BREAK)
				break;
			if (level + 1 >= key_depth) {
				skip_eq_external_nodes = false;
				/*
				 * A compressed full-match consumed the whole key and
				 * landed on cn->child (which may hold this key as a
				 * prefix-key external).  For GE/GT (RIGHT) descend into
				 * that subtree: its leftmost is the smallest key >= the
				 * search key.  But for LE/LT (LEFT) every key in the
				 * subtree is >= the search key, so descending would
				 * return the subtree MAX, which is strictly greater --
				 * wrong.  Break to post_traversal instead, exactly as the
				 * non-compressed path does: it returns cn->child's own
				 * external_nodes for LE (the equal match) and climbs for
				 * LT's strict predecessor.
				 */
				if (mode == FT_LOOKUP_LE || mode == FT_LOOKUP_LT)
					break;
				goto descend_children;
			}
			continue;
		}

		switch (limit) {
		case FT_LOOKUP_LIMIT_NONE:
			key_value = *(iter_key++);
			break;
		case FT_LOOKUP_LIMIT_FIRST:
			key_value = input_key[level - 1];
			break;
		case FT_LOOKUP_LIMIT_LAST:
			if ((size_t) level <= iter->prefix_len)
				key_value = input_key[level - 1];
			else
				key_value = 0xff;
			break;
		}
		/*
		 * Record ordinal_key[level - 1] BEFORE the NULL-break so the
		 * going-up phase has the dispatch byte even for a dead-end
		 * descent.  The parent-pointer walk keys off ordinal_key
		 * (iter_key desyncs once level steps by span > 1), so this
		 * must be set regardless of whether descent continues.
		 */
		if (keep_ordinal)
			ordinal_key[level - 1] = key_value;
		{
			unsigned int rewind;

			/*
			 * Surgical re-anchor on a skip-compressed mismatch (no
			 * spin on a frozen slot).  rewind > 0: a concurrent
			 * chain-merge moved the encoded position shallower;
			 * re-descend the fixed key path from the root, exactly
			 * like the fast-path fallback above.
			 */
			node_flag = ft_node_get_nth_reanchor(ft, node_flag,
					key_value, &rewind);
			if (caa_unlikely(rewind)) {
				node_flag = ft_root_dereference_prefetch(ft);
				iter_key = input_key;
				goto slow_path;
			}
		}
		if (!node_flag) {
			FT_TP(slowpath_step, (int) level, key_value,
				(const void *) node_flag, 1);
			break;
		}
		up_node = node_flag;		/* child established at @level (span 1) */
		up_node_lo = level;
		FT_TP(slowpath_step, (int) level, key_value,
			(const void *) node_flag, 0);
		dbg_printf("cds_ft_lookup_inequality iter key lookup %u finds node_flag %p\n",
				(unsigned int) key_value, node_flag);
		if (ft_node_external(node_flag))
			break;
	}

	/*
	 * The slow-path traversal has freshly populated the iterator
	 * path.  Capture a grace-period snapshot so that subsequent
	 * check() calls can validate this new path.  The fast path
	 * (goto post_traversal above) skips this: its cached path was
	 * already validated by iter_debug_path_check().
	 */
	iter_debug_path_snapshot(iter);

post_traversal:
	FT_TP(post_traversal, (int) mode, (int) level,
		(const void *) node_flag);
	ft_delay_reader();
	/*
	 * If the descent consumed the whole key, normalize @level to the
	 * end-of-key depth (key_depth - 1) BEFORE the equal-match check below.
	 * The loop overshoots by one when the key terminates on an INTERNAL
	 * node (level reaches key_depth) rather than breaking on an external
	 * leaf (level == key_depth - 1): an internal node that holds the key as
	 * a prefix-key external (it also has a longer-key subtree) is the exact
	 * match, and the LE/GE arm's ft_node_internal branch returns its
	 * external_nodes.  Without this normalization that arm is skipped and
	 * the exact prefix-key match is missed (the non-compressed analogue of
	 * the compressed full-match handled in the slow-path loop above).
	 */
	if (level >= key_depth)
		level = key_depth - 1;
	switch (mode) {
	case FT_LOOKUP_LE:
	case FT_LOOKUP_GE:
		if (level == key_depth - 1) {
			struct cds_ft_node *external_nodes;

			if (ft_node_internal(node_flag)) {
				struct cds_ft_metadata *metadata;
				const struct cds_ft_type *type = &ft_types[ft_node_type(node_flag)];

				metadata = cds_ft_item_to_metadata_fast(ft_node_ptr(node_flag), type->order);
				external_nodes = ft_dereference_external(metadata->external_nodes);
			} else if (ft_node_compressed(node_flag)) {
				struct cds_ft_metadata *metadata =
					cds_ft_item_to_metadata(ft_node_ptr(node_flag));
				external_nodes = ft_dereference_external(metadata->external_nodes);
			} else {
				external_nodes = (struct cds_ft_node *) node_flag;
			}
			if (external_nodes) {
				/* End of key lookup succeded. We got an equal match.
				 * The result key equals the input, which is read in
				 * place from iter_key(iter), so the copy is a no-op
				 * self-copy unless a separate buffer is in use.
				 * memmove: @input_key may be the up-walk key at this
				 * same buffer's TAIL (iter_key + key_off), which the
				 * front write overlaps. */
				iter->key_len = key_len;
				iter->key_off = 0;
				if (!ft_speculative_keycopy(ft, external_nodes,
						iter_key(iter), (ssize_t) key_len) &&
						input_key != iter_key(iter))
					memmove(iter_key(iter), input_key, key_len);
				iter->node = external_nodes;
				iter->cache_valid = true;
				iter_debug_path_update(iter);
				iter->path_len = level + 1;
				iter->status = CDS_FT_STATUS_OK;
				goto end;
			}
		}
		break;
	case FT_LOOKUP_LT:
	case FT_LOOKUP_GT:
		break;
	default:
		assert(0);
	}

	/*
	 * LE/LT dead-ended at an external LEAF that is a PROPER PREFIX of the
	 * search key: the descent matched the leaf's whole key (depth @level)
	 * but the search key is longer, so leaf_key < search_key.  A leaf has
	 * no extensions, so no key lies strictly between it and the search key
	 * on this path -- it is the largest key <= the search key, larger than
	 * any left-sibling branch (which diverges at a smaller byte).  going_up
	 * skips external leaves (it only returns internal/compressed nodes'
	 * external_nodes), so return the leaf here.  The exact-length match
	 * (@level == key_depth - 1) is already handled by the LE/GE arm above
	 * (LE) or excluded by strictness (LT), so this is only the proper-prefix
	 * case.
	 *
	 * node_flag may be NULL here (the descent's empty-slot break), which
	 * ft_node_external() also matches: that is a mid-key dead-end, NOT a
	 * proper-prefix leaf -- fall through to going_up, which backtracks to
	 * the nearest lesser key.
	 *
	 * Scoped traversal: a leaf above the scope prefix (level < prefix_len)
	 * is outside the scope.  The scope guard before descend_children
	 * answers NOT_FOUND for it.
	 */
	if (limit == FT_LOOKUP_LIMIT_NONE &&
			(mode == FT_LOOKUP_LE || mode == FT_LOOKUP_LT) &&
			node_flag && ft_node_external(node_flag) &&
			level >= (ssize_t) iter->prefix_len &&
			level < key_depth - 1) {
		int j;

		assert(level <= (int) ft->group->max_key_len);
		iter->key_len = level;
		iter->key_off = 0;
		if (!keep_ordinal)
			ft_speculative_keycopy_unconditional(ft,
				(const struct cds_ft_node *) ft_node_ptr(node_flag),
				iter_key(iter), level);
		else if (!ft_speculative_keycopy(ft,
				(const struct cds_ft_node *) ft_node_ptr(node_flag),
				iter_key(iter), level)) {
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
		}
		iter->node = (struct cds_ft_node *) ft_node_ptr(node_flag);
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = level + 1;
		iter->status = CDS_FT_STATUS_OK;
		goto end;
	}

	/*
	 * For GE/GT: if the descent completed and the node at end-of-key
	 * is internal, any descendant key is strictly longer and therefore
	 * strictly greater. Skip backtracking and descend into children
	 * directly.
	 *
	 * For GE, the post-traversal above already returned if the node
	 * had external_nodes (the equal match). Reaching this point means
	 * no equal match exists, so descendant keys are the closest >=.
	 *
	 * For GT, the skip_eq_external_nodes flag (set below) will
	 * prevent the minmax descent from returning this node's own
	 * external_nodes (which are the equal match, not GT).
	 *
	 * LE/LT do not need this: their upward backtracking already
	 * checks external_nodes at each internal node going up, which is
	 * the correct direction to find shorter (lesser) prefix keys.
	 */
	if ((mode == FT_LOOKUP_GT || mode == FT_LOOKUP_GE) &&
			!ft_node_external(node_flag))
		goto descend_children;

	/*
	 * LE/LT, relational (LIMIT_NONE), with the search key exhausted at or
	 * above the prefix boundary: the empty key (level == prefix_len == 0), or
	 * a query equal to the scope prefix.  The going-up loop below would not
	 * run (no shorter key to back-track to) and control would fall through to
	 * descend_children, wrongly returning the subtree MAX.  No key is strictly
	 * less than the boundary; the only <= match is the boundary node's own
	 * external_nodes, which LE returns and LT excludes -> NOT_FOUND otherwise.
	 * (LIMIT_FIRST/LAST want the subtree min/max here, so they are excluded.)
	 */
	if (limit == FT_LOOKUP_LIMIT_NONE &&
			(mode == FT_LOOKUP_LE || mode == FT_LOOKUP_LT) &&
			level <= (ssize_t) iter->prefix_len && node_flag &&
			!ft_node_external(node_flag)) {
		struct cds_ft_node *ext = NULL;

		if (mode == FT_LOOKUP_LE) {
			struct cds_ft_metadata *m;

			if (ft_node_compressed(node_flag))
				m = cds_ft_item_to_metadata(ft_node_ptr(node_flag));
			else
				m = cds_ft_item_to_metadata_fast(ft_node_ptr(node_flag),
					ft_types[ft_node_type(node_flag)].order);
			ext = ft_dereference_external(m->external_nodes);
		}
		if (ext) {
			int j;

			iter->key_len = iter->prefix_len;
			iter->key_off = 0;
			if (!keep_ordinal)
				ft_speculative_keycopy_unconditional(ft, ext,
					iter_key(iter), (ssize_t) iter->prefix_len);
			else if (!ft_speculative_keycopy(ft, ext, iter_key(iter),
					(ssize_t) iter->prefix_len)) {
				for (j = 0; j < (int) iter->prefix_len; j++)
					iter_key(iter)[j] = ordinal_key[j];
			}
			iter->node = ext;
		} else {
			iter->node = NULL;
		}
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = iter->prefix_len + 1;
		iter->status = ext ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
		goto end;
	}

going_up:
	/* Ensure iter_key is exactly at the position matching the level we stopped at. */
	iter_key = input_key + level;

	/*
	 * Find highest value left/right of current node.
	 * Start at current level. If we cannot find any key left/right
	 * of ours, go one level up, seek highest value left/right of
	 * current (recursively), and when we find one, get the
	 * rightmost/leftmost child of its rightmost/leftmost child
	 * (recursively).
	 *
	 * Prefix-scoped traversal: backtracking stops at
	 * iter->prefix_len instead of 0, confining the search to the
	 * prefix subtree. When prefix_len == 0 this is identical to
	 * the original behavior.
	 */
	switch (mode) {
	case FT_LOOKUP_LE:
	case FT_LOOKUP_LT:
		dir = FT_LEFT;
		break;
	case FT_LOOKUP_GE:
	case FT_LOOKUP_GT:
		dir = FT_RIGHT;
		break;
	default:
		assert(0);
	}
	/*
	 * Parent-pointer going-up cursor.  The node at @level and the
	 * dispatcher at @level-1 (scanned for a sibling) are obtained via live
	 * back-pointer reads,
	 * which may observe a fresher version than the descent snapshot
	 * (intentional -- freshness -- and semantically valid).
	 *
	 * Anchor on @up_parent = node at depth @level-1: it is always valid
	 * for level > prefix_len (descent filled it), unlike node-at-level
	 * which is NULL on a dead-end descent.  @up_parent_lo is the shallowest
	 * depth @up_parent covers (> 1 for a compressed run).  As the loop
	 * decrements @level, climb @up_parent to its own parent via
	 * ft_get_parent_rcu once @level-1 drops below @up_parent_lo, accumulating
	 * span (compressed = cn->len, internal = 1).  @up_node (node at @level)
	 * is carried from the previous iteration's @up_parent -- the node we
	 * just climbed past -- so it is never derived by dereferencing a
	 * possibly-NULL node-at-level.  It is read only inside the LE/LT block
	 * below, which the first iteration (going_up == false) skips.
	 */
	{	/* Scope the going-up cursor locals so they fall out of scope before descend_children/end (avoids -Wjump-misses-init false positives). */
		struct cds_ft_inode_flag *up_parent;
		ssize_t up_parent_lo;
		bool up_first = true;

		/*
		 * Climb @up_node (the deepest live node descent established) up to
		 * the node covering the going-up @level: the loop @level may have
		 * been knocked back one by the end-of-key adjustment, and a
		 * compressed run spans several levels.  @up_node then is node-at-level
		 * (read by the LE/LT block, carried by the loop).
		 */
		while (level < up_node_lo) {
			struct cds_ft_inode_flag *gp;

			if (wit)		/* the climb */
				ft_witness_visit(wit, ft_node_ptr(up_node));
			gp = ft_get_parent_rcu(ft, up_node);

			up_node_lo -= (gp && ft_node_compressed(gp)) ?
				(ssize_t) ft_compressed_node_ptr(gp)->len : 1;
			up_node = gp;
		}
		/*
		 * Derive @up_parent = node at @level-1 (the dispatcher scanned for a
		 * sibling).  If @up_node already covers @level-1 it IS the dispatcher
		 * -- a compressed run (no within-span sibling, skipped by the
		 * !ft_node_internal test below) or a dead-end whose dispatcher we
		 * never descended past.  Otherwise it is @up_node's parent, one depth
		 * shallower (parenthood is by slot).
		 */
		if (up_node_lo <= level - 1) {
			up_parent = up_node;
			up_parent_lo = up_node_lo;
		} else {
			up_parent = ft_get_parent_rcu(ft, up_node);
			up_parent_lo = (up_parent && ft_node_compressed(up_parent)) ?
				level - (ssize_t) ft_compressed_node_ptr(up_parent)->len :
				level - 1;
		}
		for (; level > (ssize_t) iter->prefix_len; level--) {
			uint8_t key_value;

			ft_delay_reader();
			if (!up_first) {
				/*
				 * Climbed one level (level-- since last iter): the old
				 * dispatcher becomes the new node-at-level, and the new
				 * dispatcher is its parent once we cross below its span.
				 */
				up_node = up_parent;
				if (level - 1 < up_parent_lo) {
					struct cds_ft_inode_flag *gp =
						ft_get_parent_rcu(ft, up_parent);

					up_parent_lo -= (gp && ft_node_compressed(gp)) ?
						(ssize_t) ft_compressed_node_ptr(gp)->len : 1;
					up_parent = gp;
				}
			}
			up_first = false;
			/*
			 * Return external node if trying to find LE/LT
			 * inequality and encountering an external node when
			 * going upward.
			 */
			if (going_up && dir == FT_LEFT &&
			    !ft_node_external(up_node)) {
				struct cds_ft_metadata *metadata;

				if (ft_node_compressed(up_node))
					metadata = cds_ft_item_to_metadata(
						ft_node_ptr(up_node));
				else {
					const struct cds_ft_type *type = &ft_types[ft_node_type(up_node)];
					metadata = cds_ft_item_to_metadata_fast(
						ft_node_ptr(up_node),
						type->order);
				}
				{
				struct cds_ft_node *external_nodes = ft_dereference_external(metadata->external_nodes);

				if (external_nodes) {
					int j;

					assert(level <= (int) ft->group->max_key_len);
					iter->key_len = level;
					iter->key_off = 0;
					if (!keep_ordinal)
						ft_speculative_keycopy_unconditional(ft,
							external_nodes, iter_key(iter), level);
					else if (!ft_speculative_keycopy(ft, external_nodes,
							iter_key(iter), level)) {
						for (j = 0; j < level; j++)
							iter_key(iter)[j] = ordinal_key[j];
					}
					iter->node = external_nodes;
					iter->cache_valid = true;
					iter_debug_path_update(iter);
					iter->path_len = level + 1;
					iter->status = CDS_FT_STATUS_OK;
					goto end;
				}
				}
			}

			switch (limit) {
			case FT_LOOKUP_LIMIT_NONE:
				key_value = *(--iter_key);
				break;
			case FT_LOOKUP_LIMIT_FIRST:
				key_value = input_key[level - 1];
				break;
			case FT_LOOKUP_LIMIT_LAST:
				if ((size_t) level <= iter->prefix_len)
					key_value = input_key[level - 1];
				else
					key_value = 0xff;
				break;
			}
			/*
			 * Standard sibling lookup. Parent is level - 1. We are
			 * looking for sibling of the byte at ordinal_key[level - 1].
			 * Skip levels where the path entry is not an internal node
			 * (compressed or external entries from compressed path
			 * traversal have no siblings).
			 */
			if (!ft_node_internal(up_parent)) {
				FT_TP(ineq_going_up_step, level,
					(const void *) up_parent,
					0, (uint8_t) key_value);
				going_up = true;
				continue;
			}
			/*
			 * OFF-KEY LEVEL: the traversal already left the search key
			 * here (a sibling take, or a min/max step of the
			 * descend_children descent), so the key names a branch behind
			 * us.  Scan from the byte actually taken instead; being
			 * strictly ordered, ft_node_get_leftright then returns the
			 * next branch and the scan always advances.
			 *
			 * Placed after the non-internal skip, not before: only a level
			 * that scans needs an origin, and only such a level is
			 * guaranteed to HAVE the byte -- it is the level whose internal
			 * parent a scan dispatched through, which is where the two
			 * byte-record slots write.  The interior levels of a compressed
			 * span have no ordinal byte under @keep_ordinal == false, and
			 * they take the skip above.
			 *
			 * The switch above still runs on every level: it carries the
			 * LIMIT_NONE iter_key back-step that the shallower, on-key
			 * levels of this same climb read.
			 */
			if (caa_unlikely(level >= off_key_level))
				key_value = ordinal_key[level - 1];
			node_flag = ft_node_get_leftright(ft, up_parent, key_value,
					&ordinal_key[level - 1],
					dir, true /* validate_lookup */);
	#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * Fill path only (!use_keycopy): a skip-encoded sibling must be
			 * resolved to fill ordinal_key.  Re-anchor the scanned parent on
			 * the live structure via the child's parent chain and re-scan it
			 * (the single skip concurrency mechanism), rather than spin.
			 * @rewind > 0 (merge) means the parent merged shallower: drop
			 * @level and recompute the dispatch byte at the new level.
			 * ft_skip_reanchor never returns NULL on a well-formed trie.
			 *
			 * Under use_keycopy the skip-encoded sibling is left raw and
			 * followed by the descend_children skip-follow (the leaf copy
			 * supplies the spanned bytes), so no re-anchor is needed here.
			 */
			while (!use_keycopy && node_flag &&
					caa_unlikely(ft_node_skip_compressed(node_flag))) {
				unsigned int rewind;
				struct cds_ft_inode_flag *at_pos;
				struct cds_ft_inode_flag *anchor =
					ft_skip_reanchor(ft, node_flag, &rewind, &at_pos);

				assert(anchor != NULL);
				if (caa_likely(rewind == 0)) {
					/*
					 * Surgical: @at_pos is the live resolved sibling at
					 * this depth that re-scanning @anchor for the
					 * unchanged dispatch byte ordinal_key[level - 1] would
					 * find -- the sibling byte was already recorded by the
					 * ft_node_get_leftright above, so use @at_pos directly
					 * instead of re-scanning.  Keep the path's parent entry
					 * live.
					 */
					node_flag = at_pos;
					break;
				}
				/*
				 * @rewind > 0 (merge): the parent merged shallower; drop
				 * @level and re-scan the live parent at the new level.
				 */
				level -= (ssize_t) rewind;
				/*
				 * The merge rewound @level and re-anchored the live
				 * holder at the new level-1.  Re-sync the parent cursor to
				 * @anchor (an internal node, span 1).  If the re-scan below
				 * finds no sibling, the for-loop's level-- then carries
				 * up_node = anchor = node at the new level, keeping the walk
				 * consistent with the rewound descent position.
				 */
				up_parent = anchor;
				up_parent_lo = level - 1;
				switch (limit) {
				case FT_LOOKUP_LIMIT_NONE:
					key_value = ordinal_key[level - 1];
					break;
				case FT_LOOKUP_LIMIT_FIRST:
					key_value = input_key[level - 1];
					break;
				case FT_LOOKUP_LIMIT_LAST:
					key_value = ((size_t) level <= iter->prefix_len) ?
						input_key[level - 1] : (uint8_t) 0xff;
					break;
				}
				node_flag = ft_node_get_leftright(ft, anchor, key_value,
						&ordinal_key[level - 1], dir,
						true /* validate_lookup */);
			}
	#endif
			if (keep_ordinal)
				dbg_printf("cds_ft_lookup_inequality find sibling from %u at %u finds node_flag %p\n",
						(unsigned int) key_value, (unsigned int) ordinal_key[level - 1],
						node_flag);
			else
				dbg_printf("cds_ft_lookup_inequality find sibling from %u finds node_flag %p\n",
						(unsigned int) key_value, node_flag);
			/* If found left/right sibling, find rightmost/leftmost child. */
			if (node_flag) {
				/* Record the sibling in the path. */
				/*
				 * Seed the cursor at the found sibling (depth @level, its
				 * shallow boundary) before descend_children -> minmax, so a
				 * transiently-empty first minmax step re-enters going-up
				 * with the cursor live.
				 */
				up_node = node_flag;
				up_node_lo = level;
				/*
				 * The sibling is this traversal's own branch, not the
				 * search key's: from here down, a later climb scans from
				 * ordinal_key.
				 */
				if (level < off_key_level)
					off_key_level = level;
				/*
				 * Reaching a sibling IS backtracking (up to the
				 * parent, then over to a strictly-greater/lesser
				 * sibling subtree), even when the sibling is found on
				 * the first iteration without climbing further.  Mark
				 * @going_up so the minmax descent below does NOT set
				 * skip_eq_external_nodes: the sibling subtree's first
				 * external_nodes is a different (strictly greater for
				 * GT) prefix key, not the search key's equal match, so
				 * it must not be skipped.  Without this, GT into an
				 * immediate sibling whose subtree root is a prefix key
				 * (e.g. next("y") with keys "z" < "zz1") skips that
				 * prefix key entirely.
				 */
				going_up = true;
				if (keep_ordinal)
					FT_TP(ineq_going_up_step, level,
						(const void *) up_parent,
						1, ordinal_key[level - 1]);
				else
					FT_TP(ineq_going_up_step_nokey, level,
						(const void *) up_parent, 1);
				break;
			}
			FT_TP(ineq_going_up_step, level,
				(const void *) up_parent,
				0, (uint8_t) key_value);
			going_up = true;
		}

		/*
		 * Prefix-scoped traversal: if backtracking exhausted the
		 * scope without finding a sibling, handle the prefix
		 * boundary.
		 *
		 * For LE/LT the prefix key itself (shorter than the search
		 * key) may be the closest match: return its external_nodes
		 * if present.
		 *
		 * For GE/GT no key within the scope satisfies the inequality.
		 *
		 * When going_up is false (e.g. LIMIT_FIRST/LIMIT_LAST
		 * reaching the prefix node without backtracking), we fall
		 * through to the downward min/max search below.
		 */
		if (going_up && level == (ssize_t) iter->prefix_len) {
			if (dir == FT_LEFT) {
				/*
				 * Node at prefix_len is the going-up cursor's @up_parent.
				 * This block is reached only via normal loop exit (no
				 * sibling found while backtracking down to prefix_len),
				 * whose last iteration ran at level == prefix_len + 1 with
				 * the invariant up_parent == node at level - 1 ==
				 * node at prefix_len.  Obtained via a live back-pointer
				 * read.
				 */
				struct cds_ft_inode_flag *pfx_flag = up_parent;

				if (!ft_node_external(pfx_flag)) {
					struct cds_ft_metadata *metadata;

					if (ft_node_compressed(pfx_flag))
						metadata = cds_ft_item_to_metadata(
							ft_node_ptr(pfx_flag));
					else {
						const struct cds_ft_type *type =
							&ft_types[ft_node_type(pfx_flag)];
						metadata = cds_ft_item_to_metadata_fast(
							ft_node_ptr(pfx_flag),
							type->order);
					}
					struct cds_ft_node *external_nodes =
						ft_dereference_external(metadata->external_nodes);

					if (external_nodes) {
						int j;

						iter->key_len = iter->prefix_len;
						iter->key_off = 0;
						if (!keep_ordinal)
							ft_speculative_keycopy_unconditional(ft,
								external_nodes, iter_key(iter),
								(ssize_t) iter->prefix_len);
						else if (!ft_speculative_keycopy(ft, external_nodes,
								iter_key(iter),
								(ssize_t) iter->prefix_len)) {
							for (j = 0; j < (int) iter->prefix_len; j++)
								iter_key(iter)[j] = ordinal_key[j];
						}
						iter->node = external_nodes;
						iter->cache_valid = true;
						iter_debug_path_update(iter);
						iter->path_len = iter->prefix_len + 1;
						iter->status = CDS_FT_STATUS_OK;
						goto end;
					}
				}
			}
			iter->node = NULL;
			iter->cache_valid = true;
			iter_debug_path_update(iter);
			iter->path_len = iter->prefix_len + 1;
			iter->status = CDS_FT_STATUS_NOT_FOUND;
			goto end;
		}
	}

	/*
	 * Fall-through dead-end within the scope prefix: the descent broke
	 * on an empty slot at level <= prefix_len (the prefix path itself
	 * does not exist), so the going-up loop above -- bounded at
	 * prefix_len -- never ran and the going_up NOT_FOUND arm did not
	 * trigger.  descend_children below would misread the NULL (tag 0)
	 * as an external-node match and return OK with iter->node == NULL.
	 * No key with the scope prefix exists: report NOT_FOUND.  Every
	 * `goto descend_children` jumps past this guard with a non-NULL
	 * @node_flag; only the fall-through path can carry NULL.
	 */
	if (!node_flag) {
		iter->node = NULL;
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = iter->prefix_len + 1;
		iter->status = CDS_FT_STATUS_NOT_FOUND;
		goto end;
	}

	/*
	 * Scope guard: the descent stopped on an external leaf at or above
	 * the scope prefix.  The going-up loop is bounded at prefix_len, so
	 * it did not run.  descend_children below would return this leaf.
	 *
	 * - level < prefix_len: the leaf's key is shorter than the scope
	 *   prefix.  It is outside the scope.
	 * - level == prefix_len: the leaf's key is the scope prefix.  It is
	 *   the only key in the scope.  LIMIT_FIRST and LIMIT_LAST return it
	 *   below.  For a relational lookup, the arms above already returned
	 *   it when it satisfies the inequality.
	 *
	 * In the other cases no key in the scope matches: report NOT_FOUND.
	 * A sibling found by the going-up loop is deeper than prefix_len, so
	 * it never reaches this guard.
	 */
	if (!(use_keycopy && ft_node_skip_compressed(node_flag)) &&
			ft_node_external(node_flag) &&
			(level < (ssize_t) iter->prefix_len ||
			 (level == (ssize_t) iter->prefix_len &&
			  limit == FT_LOOKUP_LIMIT_NONE))) {
		iter->node = NULL;
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = iter->prefix_len + 1;
		iter->status = CDS_FT_STATUS_NOT_FOUND;
		goto end;
	}

descend_children:
	/*
	 * A skip-encoded sibling carried over from going_up (use_keycopy) is
	 * resolved by the loop's skip-follow below, not tested as external here
	 * -- its tag bits would otherwise be misread as an external node.
	 */
	if (!(use_keycopy && ft_node_skip_compressed(node_flag))
			&& ft_node_external(node_flag)) {
		int j;

		assert(level <= (int) ft->group->max_key_len);
		iter->key_len = level;
		iter->key_off = 0;
		if (!keep_ordinal)
			ft_speculative_keycopy_unconditional(ft,
				(const struct cds_ft_node *) ft_node_ptr(node_flag),
				iter_key(iter), level);
		else if (!ft_speculative_keycopy(ft,
				(const struct cds_ft_node *) ft_node_ptr(node_flag),
				iter_key(iter), level)) {
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
		}
		iter->node = (struct cds_ft_node *) ft_node_ptr(node_flag);
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = level + 1;
		iter->status = CDS_FT_STATUS_OK;
		goto end;
	}

	level++;

	/*
	 * The min/max descent below picks its own branch at every level it
	 * reaches, so all of them are off-key.  A descent entered straight from
	 * the key walk (GE/GT past the end of the key, or a compressed
	 * divergence) has taken no sibling and so has not marked one yet.
	 */
	if (level < off_key_level)
		off_key_level = level;

	/*
	 * From this point, we are guaranteed to be able to find a
	 * "lower than"/"greater than" match. ft_attach_node() and
	 * ft_detach_node() both guarantee that it is not possible for a
	 * lookup to reach a dead-end.
	 */

	/*
	 * Find rightmost/leftmost child of rightmost/leftmost child
	 * (recursively).
	 *
	 * skip_eq_external_nodes: when entering the minmax descent
	 * without backtracking (going_up == false) and the mode is
	 * strictly GT, the external_nodes at the first node are at the
	 * same position as the search key -- equal, not strictly
	 * greater. Skip them on the first iteration so the descent
	 * continues to a proper child.
	 */
	/*
	 * ☠ @descended_past_key is the third case, and leaving it out SKIPPED A
	 * LIVE KEY.  The skip is only correct when this node's external_nodes
	 * sit AT the search key -- the end-of-key stop, and the compressed
	 * FULL MATCH that lands on cn->child.  After a compressed DIVERGENCE
	 * the descent is already strictly past the key, so the first node's
	 * external_nodes are the GT answer and skipping them returns the next
	 * key instead: cds_ft_lookup_gt("aa") over {"ab","abb"} answered
	 * "abb".  ft_inequality_compressed() had cleared the flag on that
	 * path; this assignment used to overwrite the answer.
	 */
	skip_eq_external_nodes = (!going_up && !descended_past_key &&
			mode == FT_LOOKUP_GT);
	switch (mode) {
	case FT_LOOKUP_LE:
	case FT_LOOKUP_LT:
		dir = FT_RIGHTMOST;
		break;
	case FT_LOOKUP_GE:
	case FT_LOOKUP_GT:
		dir = FT_LEFTMOST;
		break;
	default:
		assert(0);
	}
	for (; level < (int) ft->group->max_tree_depth; level++) {
		if (wit)			/* the sibling descent */
			ft_witness_visit(wit, ft_node_ptr(node_flag));
#ifdef FEATURE_FT_SKIP_COMPRESSED
		/*
		 * Skip-encoded compressed child: follow it directly without
		 * reading the compressed node.  The spanned length is in the
		 * pointer's upper bits (ft_skip_len == cn->len), so this
		 * reproduces ft_inequality_minmax_compressed's level advance
		 * exactly; the only thing dropped is the ordinal_key span fill,
		 * which the leaf-key copy supplies for the result.  A
		 * skip-compressed node cannot carry external_nodes (a terminating
		 * fork the encoding cannot express), so nothing terminates inside
		 * the span -- no min/max candidate is skipped.  Resolving here, at
		 * the loop top, also keeps the external / internal tests below
		 * from misreading the skip pointer's tag bits.
		 */
		if (use_keycopy && caa_unlikely(ft_node_skip_compressed(node_flag))) {
			level += (ssize_t) ft_skip_len(node_flag) - 1;
			node_flag = ft_skip_child_ptr(node_flag);
			skip_eq_external_nodes = false;
			up_node = node_flag;	/* live child below the span */
			up_node_lo = level;
			/*
			 * Mirror ft_inequality_minmax_compressed: an external child
			 * is the leaf at the span end -- break with @level already at
			 * the key length (no loop level++); an internal child
			 * continues the descent (the loop level++ lands on it).
			 */
			if (ft_node_external(node_flag))
				break;
			continue;
		}
#endif
		/*
		 * Return external node associated to internal node if
		 * trying to find GE/GT inequality and encountering an
		 * external node when going downward.
		 */
		if (dir == FT_LEFTMOST && ft_node_internal(node_flag)
				&& !skip_eq_external_nodes) {
			const struct cds_ft_type *type = &ft_types[ft_node_type(node_flag)];
			struct cds_ft_metadata *metadata = cds_ft_item_to_metadata_fast(
					ft_node_ptr(node_flag), type->order);
			struct cds_ft_node *external_nodes = ft_dereference_external(metadata->external_nodes);

			if (external_nodes) {
				ret_node = external_nodes;
				level--;
				goto found_minmax;
			}
		}
		/* Return external node. */
		if (ft_node_external(node_flag))
			break;
		/*
		 * Resolve a skip-encoded node to its compressed form.  No-op under
		 * use_keycopy (skip pointers were already followed at the loop
		 * top); on the fill path it converts a skip pointer so the
		 * compressed handler can read cn->key_bytes.
		 */
		node_flag = ft_resolve_skip_compressed(ft, node_flag);
		/*
		 * Compressed node: traverse the compressed path to reach the
		 * child, filling ordinal_key.  Under use_keycopy this is only the
		 * regular (non-skip-encoded) form for spans longer than
		 * FT_SKIP_LEN_MAX, and the fill is harmless (the result key comes
		 * from the leaf copy).
		 */
		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;

			act = ft_inequality_minmax_compressed(
				&node_flag, &level, &ret_node,
				&skip_eq_external_nodes,
				ordinal_key, dir, keep_ordinal);
			if (act == FT_DESCENT_FOUND_MINMAX)
				goto found_minmax;
			if (act == FT_DESCENT_BREAK)
				break;
			assert(act == FT_DESCENT_CONTINUE);
			up_node = node_flag;	/* minmax cn->child at the advanced @level */
			up_node_lo = level;
			continue;
		}
		skip_eq_external_nodes = false;
		node_flag = ft_node_get_minmax(ft, node_flag,
				&ordinal_key[level - 1], dir,
				true /* validate_lookup */);
		/*
		 * Prefetch the min/max child's body for the next iteration's scan.
		 * ft_maybe_prefetch fires only on internal/compressed children
		 * (external/NULL/non-canonical-skip are dropped), so it never
		 * reintroduces the harmful external-leaf prefetch.  The lead comes
		 * from the GE/GT external-nodes metadata read at the loop top, which
		 * is enough to overlap the body miss: measured ~+3-4% on
		 * single-thread cds_ft_next over the 1M-key DNS set (the going-up
		 * sibling scan has no such lead and did NOT benefit -- not added).
		 */
		ft_maybe_prefetch(node_flag);
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (node_flag && caa_unlikely(ft_node_skip_compressed(node_flag))) {
			if (use_keycopy) {
				/*
				 * Leaf-copy path: a skip-encoded min/max child is returned
				 * raw and followed at the loop top next iteration -- the
				 * key drives the descent and the leaf copy supplies the
				 * spanned bytes, so no re-anchor.
				 */
				up_node = node_flag;
				up_node_lo = level;
				continue;
			}
			{
				/*
				 * Fill path (no leaf-key offset): ordinal_key must be
				 * filled from the live compressed node, so re-anchor the
				 * scanned node on the live structure via the child's parent
				 * chain and re-scan, rather than spin.  @rewind > 0 (merge)
				 * re-anchors shallower; @level -= rewind + 1 then the loop's
				 * level++ nets a -rewind step.  ft_skip_reanchor never
				 * returns NULL on a well-formed trie.
				 */
				unsigned int rewind;
				struct cds_ft_inode_flag *at_pos;
				struct cds_ft_inode_flag *anchor =
					ft_skip_reanchor(ft, node_flag, &rewind, &at_pos);

				assert(anchor != NULL);
				if (caa_likely(rewind == 0)) {
					node_flag = at_pos;
				} else {
					level -= (ssize_t) rewind + 1;
					node_flag = anchor;
					up_node = anchor;
					up_node_lo = level;
					continue;
				}
			}
		}
#endif
		/*
		 * Transiently empty internal node (popcount/pigeon): a reader may
		 * observe every slot of a reachable internal node as
		 * NULL in the narrow window between a writer's per-slot
		 * detach and the upward walk's slot-replace at a higher
		 * ancestor.  Quiescently, reachable internal nodes have
		 * nr_child >= 1 (the upward walk prunes single-child
		 * chains wholesale and replaces at the first multi-child
		 * ancestor, so a slot-emptied internal is never left in
		 * place).  Treat as "empty at this step" and let
		 * going_up find the next sibling at a higher level.
		 * Back level one step so the going-up cursor resumes at
		 * the parent.
		 */
		if (caa_unlikely(!node_flag)) {
			level--;
			/*
			 * The climb out of the empty subtree must EXCLUDE the branch
			 * that led into it, or the going-up scan re-derives it
			 * forever.  @off_key_level, set on the way in, already covers
			 * every level of this descent: each one scans from the byte it
			 * took (ordinal_key[level - 1]), and ft_node_get_leftright is
			 * strictly ordered (smallest v > n for RIGHT), so the origin
			 * advances at every level and the climb is monotone.
			 *
			 * That is the cross-trie oracle's long-standing STALL: LTTng
			 * caught 100001 byte-identical going_up steps (same level,
			 * same parent, same ord_key, found_sibling=1) with no peer
			 * event interleaved -- a self-contained infinite loop, not a
			 * contention livelock.  The comment above assumes going_up
			 * "find[s] the NEXT sibling", which holds only if the scan
			 * starts PAST the exhausted branch AT EVERY LEVEL IT CLIMBS:
			 * a level whose dispatcher is compressed cannot scan at all,
			 * so an exclusion bound to one level is dropped there and the
			 * next level up falls back to the search key -- which names
			 * the emptied subtree again.
			 */
			going_up = true;
			goto going_up;
		}
		up_node = node_flag;		/* minmax child established at @level */
		up_node_lo = level;
		if (keep_ordinal)
			dbg_printf("cds_ft_lookup_inequality find minmax at %u finds node_flag %p\n",
					(unsigned int) ordinal_key[level - 1], node_flag);
		else
			dbg_printf("cds_ft_lookup_inequality find minmax finds node_flag %p\n",
					node_flag);
		if (ft_node_external(node_flag))
			break;
	}
	/*
	 * Every break path in the descent loop sets node_flag to a
	 * validated external (compressed-branch external child or
	 * minmax-branch external return).  Transiently-empty
	 * intermediate steps (non-root minmax == NULL above) do not
	 * reach this assert -- they jump to going_up and re-enter the
	 * search at a higher level.
	 */
	assert(ft_node_ptr(node_flag));
	ret_node = (struct cds_ft_node *) node_flag;
	/*
	 * The trie should always have external nodes at the
	 * very last level, so level should never grow large enough to overflow
	 * max_key_len.
	 */
	assert(level <= (int) ft->group->max_key_len);
	assert(level <= (int) ft->group->max_key_len);
found_minmax:
	{
		int j;

		iter->key_len = level;
		iter->key_off = 0;
		if (!keep_ordinal)
			ft_speculative_keycopy_unconditional(ft, ret_node,
				iter_key(iter), level);
		else if (!ft_speculative_keycopy(ft, ret_node, iter_key(iter),
				level)) {
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
		}
		iter->node = ret_node;
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = level + 1;
		iter->status = ret_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
	}
end:
	FT_TP(ineq_result, (int) mode,
		input_key, key_len,
		iter->node ? iter_key(iter) : NULL,
		iter->node ? iter->key_len : 0,
		(int) iter->status);
	iter_auto_invalidate_cache(iter);
	return iter->status;
}

/*
 * Inequality lookup entry point: tier-1 ordered-cell fast path, then the tier-2
 * descent.  cds_ft_next / cds_ft_prev on a cached cursor (GT/LT, LIMIT_NONE)
 * resolve the successor / predecessor cell with a single dependent load and
 * never touch the tree; everything else (uncached / scoped / first / last /
 * seed / relational seek) falls through to ft_ineq_descend.  Always inlined
 * under FEATURE_INLINE_LOOKUP, so the cell hop stays a few loads regardless of
 * the tier-2 inlining choice.
 */
static inline_lookup
enum cds_ft_status cds_ft_lookup_inequality_impl(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit,
		const bool use_keycopy,
		const bool seed_from_node,
		struct ft_visit_witness *wit)	/* NULL = no coherence witness */
{
	struct ft_ord_cell *cur = NULL;

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	/*
	 * Tier-1 ordered-cell fast path.  Hoisted above the key_len computation:
	 * it advances via ord_next / ord_prev and never resolves the (deferred,
	 * LAZY) length.  @seed_from_node suppresses it for the splice-time
	 * predecessor seed (a FRESH head whose cell is not yet spliced); that
	 * caller wants the cross-call node-recovery path in the descent instead.
	 */
	if (!seed_from_node &&
			(mode == FT_LOOKUP_GT || mode == FT_LOOKUP_LT) &&
			limit == FT_LOOKUP_LIMIT_NONE &&
			iter->cache_valid && iter->node &&
			ft_ord_cell_fastpath_ok(ft, iter) &&
			/* No cell to hop from: the descent below re-derives. */
			(cur = ft_ord_cell_cursor(iter)) != NULL) {
		struct ft_ord_cell *nxt = (mode == FT_LOOKUP_GT) ?
			ft_ord_cell_resolve_ord(&cur->lnode.next) :
			ft_ord_cell_resolve_ord(&cur->lnode.prev);

		if (wit) {			/* the cell hop's two cells */
			ft_witness_visit(wit, cur);
			ft_witness_visit(wit, nxt);
		}

		/* Sentinel topology: off-the-end resolves to the sentinel pseudo-cell;
		 * map it to NULL so iter_land reports NOT_FOUND (end of list). */
		if (ft_ord_is_end(ft, nxt))
			nxt = NULL;
		ft_ord_cell_iter_land(ft, iter, nxt);
#ifndef FT_NO_ORD_PREFETCH
		/* One-hop NTA prefetch of the cell the next call will land on. */
		if (nxt) {
			struct ft_ord_cell *nn = (mode == FT_LOOKUP_GT) ?
				ft_ord_cell_resolve_ord(&nxt->lnode.next) :
				ft_ord_cell_resolve_ord(&nxt->lnode.prev);
			if (!ft_ord_is_end(ft, nn))
				__builtin_prefetch((const void *) nn, 0, 0);
		}
#endif
		FT_TP(ineq_result, (int) mode, NULL, 0,
			iter->node ? iter_key(iter) : NULL,
			iter->node ? iter->key_len : 0,
			(int) iter->status);
		iter_auto_invalidate_cache(iter);
		return iter->status;
	}

	return ft_ineq_descend(ft, iter, mode, limit, use_keycopy,
			seed_from_node, wit);
}

/*
 * Dispatcher: resolve the immutable @use_keycopy config (a speculative
 * skip-compressed group with a leaf-key offset) once, then tail-call the
 * matching always_inline instantiation so each variant's hot loop has the
 * per-call use_keycopy gates constant-folded away.  Mirrors the
 * do_cds_ft_lookup_inner (descend_cand, skip_compressed) specialization.
 * The config is immutable after group create, so this branch is perfectly
 * predicted and amortized over a full traversal.
 *
 * inline_lookup (force-inline) into each entry point: cds_ft_lookup_le/ge/lt/gt
 * pass compile-time-constant @mode and @limit (and cds_ft_next/prev resolve to
 * _gt/_lt), so inlining lets @mode AND @limit -- not just @use_keycopy -- DCE
 * each wrapper down to its single arm.  The six specialized bodies cost .so size
 * but the HOT footprint shrinks (a workload runs one DCE'd wrapper, smaller than
 * the shared generic): measured ~+10% ST and +4-22% MT on 1M-key DNS iterate.
 */
static inline_lookup
enum cds_ft_status cds_ft_lookup_inequality(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit)
{
	if (ft->speculative_key_offset_active && ft->group->speculative &&
			(ft->group->flags & CDS_FT_FLAG_SKIP_COMPRESSED))
		return cds_ft_lookup_inequality_impl(ft, iter, mode, limit,
				true, false, NULL);
	return cds_ft_lookup_inequality_impl(ft, iter, mode, limit,
			false, false, NULL);
}

/*
 * Limit-none relational specializations, installed as fn-pointers by
 * ft_install_lookup_ops.  Each bakes its mode, LIMIT_NONE, and use_keycopy
 * into the force-inline cds_ft_lookup_inequality_impl so the body DCEs to a
 * single arm; referenced only via the installed pointer, so they stay real
 * out-of-line symbols and the public entry is a bare indirect tail-call.  The
 * RCU read lock is taken here (mirroring the lookup-API inners).
 */
#define FT_INEQ_SPEC(name, mode, kc)					\
	static enum cds_ft_status name(struct cds_ft *ft,		\
			struct cds_ft_iter *iter)			\
	{								\
		CDS_FT_SCOPED_READER(ft);				\
		return cds_ft_lookup_inequality_impl(ft, iter,		\
				(mode), FT_LOOKUP_LIMIT_NONE, (kc), false, \
				NULL);					\
	}
FT_INEQ_SPEC(ft_ineq_le_keycopy, FT_LOOKUP_LE, true)
FT_INEQ_SPEC(ft_ineq_le_eager,   FT_LOOKUP_LE, false)
FT_INEQ_SPEC(ft_ineq_ge_keycopy, FT_LOOKUP_GE, true)
FT_INEQ_SPEC(ft_ineq_ge_eager,   FT_LOOKUP_GE, false)
FT_INEQ_SPEC(ft_ineq_lt_keycopy, FT_LOOKUP_LT, true)
FT_INEQ_SPEC(ft_ineq_lt_eager,   FT_LOOKUP_LT, false)
FT_INEQ_SPEC(ft_ineq_gt_keycopy, FT_LOOKUP_GT, true)
FT_INEQ_SPEC(ft_ineq_gt_eager,   FT_LOOKUP_GT, false)
#undef FT_INEQ_SPEC

/*
 * ===========================================================================
 * REKEY-COHERENT RELATIONAL LOOKUPS (le / ge / lt / gt, hence next / prev).
 * ===========================================================================
 *
 * The reader half of the in-trie-move coherence design.  A relational answer is
 * a statement about the trie's SHAPE (the neighbour of a key), so unlike an
 * exact lookup it cannot be validated against the key it was asked for: a
 * traversal that STRADDLES a move's commit can return a node that is a real,
 * live neighbour of nothing -- the trie never held that relation at any instant.
 *
 * The check is therefore a REPETITION, not a validation: run the whole traversal
 * TWICE and accept only if both passes visited exactly the same nodes in the
 * same order (struct ft_visit_witness) and landed on the same result.  Sound
 * because a single traversal can witness a torn view only by straddling a
 * commit, and two SEQUENTIAL traversals cannot both straddle the same commit
 * (the second starts after the first ended) -- the antisymmetry the exact-key
 * proof gets from descend-then-up-walk comes here from pass1-before-pass2.  This
 * is the same primitive the rekey WRITER already uses to derive its splice
 * position (ft_ord_cell_find_splice_pos_coherent).
 *
 * Comparing the visited ADDRESSES rather than just the result is what makes it
 * work: a torn pass can land on a stable node (so results alone agree), but a
 * move COWs the stitch points it moves (junction, subtree top, boundary cells)
 * into FRESH addresses, so any pass that saw the neighbourhood mid-move folds a
 * different address set.  It is also ABA-immune: RCU will not hand an address
 * back inside a read section, so an oscillating rekey cannot forge agreement.
 *
 * COST: gated by struct cds_ft::move_active (ft_move_active) -- with no move in
 * flight these entry points tail-call the ordinary specialization and the whole
 * mechanism costs ONE load of a quiet word.  The doubled traversal (and its
 * doubled tracepoints) is paid only inside a move window.
 *
 * NOT COVERED (both need design step 4's linearizability oracle, not more
 * checking here): a walker parked ON a moving run's interior cell steps out
 * through the new outer link and skips the keys in between; and a leaf that only
 * changes KEY (not address, not neighbourhood) between the two passes is
 * accepted with the pass-2 key.
 */

/* Defined below; the coherent path uses the ONE shared out-of-line descent
 * rather than force-inlining a second copy per mode. */
static enum cds_ft_status cds_ft_lookup_inequality_impl_shared(struct cds_ft *ft,
		struct cds_ft_iter *iter, enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit, const bool use_keycopy,
		const bool seed_from_node, struct ft_visit_witness *wit);

/*
 * Everything a relational lookup READS from and WRITES to the iterator.  The
 * two passes must be fed byte-identical input, and pass 1 has already
 * overwritten that input with its RESULT by the time pass 2 starts (the search
 * key, the cached position, the cell cursor), so the input is snapshotted once
 * and restored between passes.  @key covers the whole key buffer because the
 * descent both reads its search key from it and writes its result key into it.
 */
struct ft_ineq_iter_state {
	struct cds_ft_node *node;
	struct ft_ord_cell *ord_cell;
	struct cds_ft_node *ord_cell_node;
	size_t key_len;
	size_t key_off;
	size_t path_len;
	enum cds_ft_status status;
	bool cache_valid;
#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH
	struct urcu_gp_poll_state gp_state;
	bool gp_state_valid;
#endif
	uint8_t key[FT_MAX_KEY_LEN];
};

static
void ft_ineq_iter_state_save(struct cds_ft_iter *iter,
		struct ft_ineq_iter_state *s)
{
	s->node = iter->node;
	s->ord_cell = iter->ord_cell;
	s->ord_cell_node = iter->ord_cell_node;
	s->key_len = iter->key_len;
	s->key_off = iter->key_off;
	s->path_len = iter->path_len;
	s->status = iter->status;
	s->cache_valid = iter->cache_valid;
#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH
	s->gp_state = iter->gp_state;
	s->gp_state_valid = iter->gp_state_valid;
#endif
	memcpy(s->key, iter_key(iter), iter->ft->group->max_key_len);
}

static
void ft_ineq_iter_state_restore(struct cds_ft_iter *iter,
		const struct ft_ineq_iter_state *s)
{
	iter->node = s->node;
	iter->ord_cell = s->ord_cell;
	iter->ord_cell_node = s->ord_cell_node;
	iter->key_len = s->key_len;
	iter->key_off = s->key_off;
	iter->path_len = s->path_len;
	iter->status = s->status;
	iter->cache_valid = s->cache_valid;
#ifdef URCU_FRACTAL_TRIE_DEBUG_PATH
	iter->gp_state = s->gp_state;
	iter->gp_state_valid = s->gp_state_valid;
#endif
	memcpy(iter_key(iter), s->key, iter->ft->group->max_key_len);
}

/*
 * Will the tier-1 ordered-cell hop serve this call?  Mirrors the gate at the top
 * of cds_ft_lookup_inequality_impl (with seed_from_node false, which every
 * public relational entry passes).  Only used to decide whether the search key
 * gets read at all -- a drift from the real gate costs the hop, never
 * correctness (the descent answers the same relational question).
 */
static
bool ft_ineq_cell_hop_applies(const struct cds_ft *ft,
		const struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode, enum ft_lookup_limit limit)
{
	return (mode == FT_LOOKUP_GT || mode == FT_LOOKUP_LT) &&
		limit == FT_LOOKUP_LIMIT_NONE &&
		iter->cache_valid && iter->node &&
		ft_ord_cell_fastpath_ok(ft, iter);
}

/*
 * Derive the CURRENT position's key from the structure, twice, accepting it only
 * if both walks agree -- the repetition argument applied to the up-walk.  On
 * success @out[0 .. *@out_len) holds the key, moved to the front.  @torn
 * separates "the walks disagreed" (re-derive) from "no key to derive here".
 */
static
bool ft_ineq_upwalk_key_twice(struct cds_ft *ft, struct cds_ft_iter *iter,
		uint8_t *out, size_t *out_len, bool *torn)
{
	size_t max_len = ft->group->max_key_len;
	uint8_t first_pass[FT_MAX_KEY_LEN];
	struct ft_ord_cell *cell;
	size_t n1, n2;

	*torn = false;
	/*
	 * Cell-sourced: on a list-off trie a head's ->prev is its HOLDER, not a
	 * cell, so there is nothing to walk from here.  Nothing is lost -- a
	 * list-off continuation's search key is already a VALUE in iter_key (the
	 * previous descent wrote it there), which is the carried key.
	 */
	if (!ft->ordered_list)
		return false;
	cell = ft_ord_cell_cursor(iter);
	if (!cell)
		return false;
	n1 = ft_rebuild_key_upwalk(ft, cell, first_pass, max_len);
	if (!n1)
		return false;
	cell = ft_ord_cell_cursor(iter);
	n2 = ft_rebuild_key_upwalk(ft, cell, out, max_len);
	if (n1 != n2 || memcmp(first_pass + (max_len - n1),
			out + (max_len - n2), n1) != 0) {
		*torn = true;
		return false;
	}
	memmove(out, out + (max_len - n2), n2);
	*out_len = n2;
	return true;
}

/*
 * CONFIRM the position a traversal just returned, so the next continuation step
 * has a baseline.  Free and drift-free: an EAGER traversal already wrote the
 * answer's key into iter_key as it walked it, so the key the answer was FOUND
 * UNDER is right there -- no up-walk, and therefore no chance of carrying a key
 * the leaf only acquired after the answer.
 */
static
void ft_ineq_carry_traversed_key(struct cds_ft_iter *iter)
{
	iter->pos_key_node = (iter->cache_valid && iter->node) ?
		iter->node : NULL;
}

/*
 * PIN the search key of a CONTINUATION (a relational step taken from the
 * iterator's current position rather than from a set_key bound).
 *
 * On a coherence-capable trie there is no in-leaf key, so ft_iter_read_key()
 * rematerializes the current key STRUCTURALLY by walking the position's parent
 * chain -- a long multi-read that a move can tear, and one that each pass would
 * redo, so a torn walk would feed BOTH passes the same bogus key and they would
 * agree on the wrong answer.  The witness cannot see it either: it folds the
 * nodes the TRAVERSAL visits, and this walk happens before the traversal starts.
 *
 * So the key is derived here instead, by the same repetition argument (two
 * sequential up-walks that agree cannot have straddled a commit), pinned as a
 * VALUE at the front of the iterator buffer, and the cached position is cleared
 * so the descent reads that buffer -- which also makes both passes a clean
 * top-descent from the root, the design's uniform shape.
 *
 * Returns false when the two walks disagree: a move is restructuring this
 * position's ancestry, so the caller must re-derive.  Returns true WITHOUT
 * pinning whenever the search key is already a stable value in the buffer (a
 * set_key bound, a LIMIT_FIRST/LAST endpoint).  A continuation that HAS a
 * carried key never reaches here (the caller turns it into a descent from that
 * key); this derives one for a position that does not, which is why it must be
 * the two-walk form and not a single materialize.
 */
static
bool ft_ineq_pin_search_key(struct cds_ft *ft, struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode, enum ft_lookup_limit limit)
{
	size_t n;
	bool torn;

	(void) mode;
	if (limit != FT_LOOKUP_LIMIT_NONE || !iter->cache_valid || !iter->node)
		return true;		/* key is a value in iter_key already */
	if (!ft_ineq_upwalk_key_twice(ft, iter, iter_key(iter), &n, &torn))
		return !torn;		/* torn -> re-derive; else nothing to pin */
	/*
	 * Defensive: on a FIXED-length group the descent validates the search
	 * length against the group's (ft_key_len), and ft_iter_read_key's own
	 * fixed-length branch leaves iter->key_len alone -- so a walk that
	 * accounted for a different number of bytes must not be pinned as the
	 * length here.
	 */
	if (ft->group->key_len != CDS_FT_LEN_VARIABLE &&
			n != ft->group->key_len)
		return true;
	iter->key_off = 0;		/* the walk moved it to the front */
	iter->key_len = n;
	iter->path_len = n + 1;
	iter->cache_valid = false;	/* pinned: read the buffer, top-descend */
	return true;
}

/*
 * The two-pass relational lookup itself.  Runs under the caller's RCU read-side
 * lock (both passes and the input snapshot must be one section: the compared
 * addresses are only meaningful, and the pointers only valid, within it).
 *
 * Retries on disagreement rather than reporting an error: an incoherent
 * derivation cannot be repaired, only re-taken.  That makes a reader LOCK-FREE
 * (not wait-free) for as long as moves keep restructuring the neighbourhood it
 * is reading, which is the price the design accepts inside a move window, and
 * only there.
 */
static
enum cds_ft_status ft_ineq_two_pass(struct cds_ft *ft, struct cds_ft_iter *iter,
		enum ft_lookup_inequality mode, enum ft_lookup_limit limit)
{
	struct ft_ineq_iter_state in, pinned;
	struct ft_visit_witness w1, w2;
	struct cds_ft_node *node1;
	enum cds_ft_status st1, st2;

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);
	ft_ineq_iter_state_save(iter, &in);
	/*
	 * AGREED DESIGN (with Mathieu, 2026-07-25): A CONTINUATION DOES NOT HOP
	 * THE ORDERED-LIST CELLS WHILE A MOVE IS IN FLIGHT -- it re-descends from
	 * the key the last confirmed step produced.
	 *
	 * The move is made detectable by the COW of the moved subtree's TOP, and
	 * that is ALL the COW that is needed.  But detectability only reaches
	 * traversals that read the TREE: the cell hop deliberately bypasses it
	 * (that is its whole point), and the link it reads is one the move rewrites
	 * IN PLACE on a cell the graft SHARES rather than replaces.  So a walker
	 * parked on the moved run's last cell hops into the run's new
	 * neighbourhood and skips every key in between -- and no reader can detect
	 * that, at any number of passes, because nothing it observed changed
	 * address.  Re-descending puts the step back on the path where the COW is
	 * visible, and the two-pass then covers it like any other descent.
	 *
	 * REJECTED on the way here (do not re-attempt): COWing the cells instead.
	 * Boundary-only cell COW does not work -- giving the run's last cell an old
	 * copy forces its predecessor's link to be rewritten in place, so the
	 * escape point just moves inward, and induction takes it to the whole run.
	 * Full-run cell COW would work but makes a move O(n) in the moved subtree's
	 * keys, which is the property rekey exists to avoid.  Measured: a
	 * reader-side hop validation does NOT substitute (residual unchanged per
	 * step, at 2x reader cost).
	 *
	 * The hop remains the steady-state path: with no move in flight the gate
	 * keeps this whole branch unreachable and next/prev stays one dependent
	 * load.
	 */
	if (limit == FT_LOOKUP_LIMIT_NONE && iter->cache_valid && iter->node &&
			iter->pos_key_node == iter->node) {
		in.cache_valid = false;		/* carried key = the search key */
		ft_ineq_iter_state_restore(iter, &in);
	}
	for (;;) {
		if (ft_ineq_pin_search_key(ft, iter, mode, limit)) {
			ft_ineq_iter_state_save(iter, &pinned);
			ft_witness_init(&w1);
			st1 = cds_ft_lookup_inequality_impl_shared(ft, iter,
					mode, limit, false, false, &w1);
			node1 = iter->node;
			ft_ineq_iter_state_restore(iter, &pinned);
			ft_witness_init(&w2);
			st2 = cds_ft_lookup_inequality_impl_shared(ft, iter,
					mode, limit, false, false, &w2);
			/*
			 * Fault injection last: it must not consume its
			 * countdown on a pass pair that genuinely disagreed.
			 */
			if (caa_likely(st1 == st2 && node1 == iter->node &&
					ft_witness_equal(&w1, &w2) &&
					!ft_rekey_fault_miss())) {
				ft_ineq_carry_traversed_key(iter);
				return st2;
			}
		}
		ft_ineq_iter_state_restore(iter, &in);
	}
}

/*
 * REKEY-coherent relational specializations, installed on
 * ft->lookup_{le,ge,lt,gt}_fn in place of the ft_ineq_*_eager ones when
 * ft->rekey_coherence (ft_install_lookup_ops).  Only the EAGER (no in-leaf key)
 * variants have coherent siblings: rekey_coherence implies
 * !speculative_key_offset_active, hence use_keycopy false -- a speculative trie
 * can never host a move (the library cannot rewrite an application-stored key).
 *
 * The gate sample rides the CALLER's read-side critical section, the same one
 * the traversal below runs in, which is what makes one sample per call sound
 * (struct cds_ft::move_active).  Fast mode tail-calls the ordinary
 * specialization so no second copy of the force-inlined descent is emitted.
 */
#define FT_INEQ_SPEC_COHERENT(name, fast, mode)				\
	static enum cds_ft_status name(struct cds_ft *ft,		\
			struct cds_ft_iter *iter)			\
	{								\
		if (caa_likely(!ft_move_active(ft)))			\
			return fast(ft, iter);				\
		CDS_FT_SCOPED_READER(ft);				\
		return ft_ineq_two_pass(ft, iter, (mode),		\
				FT_LOOKUP_LIMIT_NONE);			\
	}
FT_INEQ_SPEC_COHERENT(ft_ineq_le_coherent, ft_ineq_le_eager, FT_LOOKUP_LE)
FT_INEQ_SPEC_COHERENT(ft_ineq_ge_coherent, ft_ineq_ge_eager, FT_LOOKUP_GE)
FT_INEQ_SPEC_COHERENT(ft_ineq_lt_coherent, ft_ineq_lt_eager, FT_LOOKUP_LT)
FT_INEQ_SPEC_COHERENT(ft_ineq_gt_coherent, ft_ineq_gt_eager, FT_LOOKUP_GT)
#undef FT_INEQ_SPEC_COHERENT

/*
 * Iterator-based inequality lookup public API.
 * The caller sets the key via cds_ft_iter_set_key() before calling.
 * On return the iterator holds the result key, key length, node, path,
 * and status.
 */
enum cds_ft_status cds_ft_lookup_le(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_le_fn)(ft, iter);
}

enum cds_ft_status cds_ft_lookup_ge(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_ge_fn)(ft, iter);
}

enum cds_ft_status cds_ft_lookup_lt(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_lt_fn)(ft, iter);
}

enum cds_ft_status cds_ft_lookup_gt(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_gt_fn)(ft, iter);
}

enum cds_ft_status cds_ft_lookup_first(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	size_t saved_key_len = iter->key_len;
	enum cds_ft_status status;

	ft_iter_assert_bound(ft, iter);
	CDS_FT_SCOPED_READER(ft);
	dbg_printf("cds_ft_lookup_first\n");
	/* O(1) endpoint: the ordinal-cell list's minimum cell (unscoped only).
	 * Resolve a flip proxy: head/tail transition atomically with the
	 * neighbour edges during a concurrent splice/unsplice/run move.
	 * iter_auto_invalidate_cache: honour the UNCACHED contract like every
	 * other epilogue -- materialize the lazy leaf-referenced key and drop
	 * the cached position so it is not reused across a lock window. */
	if (ft_ord_cell_fastpath_ok(ft, iter)) {
		/* ft_ord_first: NULL when the list is empty (iter_land -> NOT_FOUND). */
		status = ft_ord_cell_iter_land(ft, iter, ft_ord_first(ft));
		iter_auto_invalidate_cache(iter);
		return status;
	}
	/*
	 * LIMIT_FIRST sets key_len to prefix_len internally.
	 * When prefix_len == 0 this corresponds to a traversal of the
	 * entire trie.
	 * When prefix_len > 0 it descends through the prefix key
	 * bytes, then the GE post-traversal returns the prefix key
	 * itself if it has external_nodes, or the LEFTMOST minmax
	 * descent finds the smallest descendant.
	 */
	iter->key_len = iter->prefix_len;
	/* Scoped endpoint: a real relational traversal, so it takes the
	 * two-pass under a move exactly like le/ge/lt/gt.  (The unscoped O(1)
	 * endpoint above needs none: the ordered list's head transitions
	 * atomically with the move's flip, so a single load cannot tear.) */
	if (caa_unlikely(ft->rekey_coherence && ft_move_active(ft)))
		status = ft_ineq_two_pass(ft, iter, FT_LOOKUP_GE,
				FT_LOOKUP_LIMIT_FIRST);
	else
		status = cds_ft_lookup_inequality(ft, iter,
				FT_LOOKUP_GE, FT_LOOKUP_LIMIT_FIRST);
	if (status != CDS_FT_STATUS_OK)
		iter->key_len = saved_key_len;
	return status;
}

enum cds_ft_status cds_ft_lookup_last(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	size_t saved_key_len = iter->key_len;
	enum cds_ft_status status;

	ft_iter_assert_bound(ft, iter);
	CDS_FT_SCOPED_READER(ft);
	dbg_printf("cds_ft_lookup_last\n");
	/* O(1) endpoint: the ordinal-cell list's maximum cell (unscoped only).
	 * Resolve a flip proxy (see cds_ft_lookup_first, incl. the UNCACHED
	 * auto-invalidate rationale). */
	if (ft_ord_cell_fastpath_ok(ft, iter)) {
		/* ft_ord_last: NULL when the list is empty (iter_land -> NOT_FOUND). */
		status = ft_ord_cell_iter_land(ft, iter, ft_ord_last(ft));
		iter_auto_invalidate_cache(iter);
		return status;
	}
	/*
	 * LIMIT_LAST always uses key_len = max_key_len. When
	 * prefix_len > 0, the traversal uses actual prefix key bytes
	 * for levels 1..prefix_len then 0xFF for the remaining
	 * levels, descending as deep as possible along the rightmost
	 * path within the prefix subtree. LE backtracking (bounded
	 * at prefix_len) then finds the greatest actual key.
	 */
	iter->key_len = ft->group->max_key_len;
	/* See cds_ft_lookup_first: the scoped endpoint takes the two-pass. */
	if (caa_unlikely(ft->rekey_coherence && ft_move_active(ft)))
		status = ft_ineq_two_pass(ft, iter, FT_LOOKUP_LE,
				FT_LOOKUP_LIMIT_LAST);
	else
		status = cds_ft_lookup_inequality(ft, iter,
				FT_LOOKUP_LE, FT_LOOKUP_LIMIT_LAST);
	if (status != CDS_FT_STATUS_OK)
		iter->key_len = saved_key_len;
	return status;
}

/*
 * Shared, non-inlined copy of the inequality descent for the write path.  The
 * ordered-cell maintenance in ft-detach (ft_ord_cell_find_rel,
 * ft_ord_cell_find_pred_from_head, ft_merge_ord_interleave) reaches
 * cds_ft_lookup_inequality_impl at five sites.  The impl is force-inlined for
 * the read hot path -- the ft_ineq_* specializations DCE it on constant
 * mode/limit -- so without this each of those write-side sites inlined the full
 * ~20 KB descent, ~90 KB total.  The mutation modules are redirected to this one
 * shared copy via #define in fractal-trie.c; read-side callers keep the inlined
 * original.
 */
static enum cds_ft_status cds_ft_lookup_inequality_impl_shared(struct cds_ft *ft,
		struct cds_ft_iter *iter, enum ft_lookup_inequality mode,
		enum ft_lookup_limit limit, const bool use_keycopy,
		const bool seed_from_node, struct ft_visit_witness *wit)
{
	return cds_ft_lookup_inequality_impl(ft, iter, mode, limit, use_keycopy,
			seed_from_node, wit);
}
