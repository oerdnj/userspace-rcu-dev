// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-ordered-query.h
 *
 * Userspace RCU library - Fractal Trie: ordered-trie query operations --
 * rank / select / skip.
 *
 *   rank   -- cds_ft_count_keys / cds_ft_count_keys_prefix
 *   select -- cds_ft_lookup_nth / cds_ft_lookup_nth_last
 *   skip   -- cds_ft_iter_skip_forward / cds_ft_iter_skip_reverse
 *
 * These ordered-position queries are read-only.  They walk the trie and use
 * the per-node key counters to skip whole subtrees in O(depth).  The
 * iterator skip also uses ft_rebuild_path for its cursor.  They were parked
 * at the tail of ft-merge.h after the merge consolidation.  They have
 * nothing to do with merge.
 *
 * Implementation unit: fractal-trie.c #includes it once, in dependency
 * order, directly after ft-merge.h.  It is not a standalone header.
 *
 * It sits in the write zone on purpose, after the shared-scanner redirect.
 * Rank, select and skip are cold compared to point lookups.  So they share
 * the out-of-line node scanners instead of inlining a copy at each site.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-ordered-query.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * When order statistics are off, the count falls back to a structural walk
 * of the subtree with ft_subtree_key_count.  That function is defined in
 * ft-mutation-helpers.h.  The bulk ops share it: they also need on-demand
 * subtree key counts when rank stats are off.
 *
 * Handle compressed node in cds_ft_count_keys_prefix().
 *
 * Return FT_DESCENT_CONTINUE to advance past the compressed path.
 * Return FT_DESCENT_END when the prefix is fully consumed inside the
 * compressed node.  The count is then written to *count_ret.
 */
static
enum ft_descent_action ft_count_prefix_compressed(
		struct cds_ft *ft,
		struct cds_ft_inode_flag **node_flag_p,
		unsigned int *i_p, const uint8_t *prefix,
		size_t prefix_len, unsigned long *count_ret)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	unsigned int i = *i_p;
	struct cds_ft_compressed_node *cn =
		ft_compressed_node_ptr(node_flag);
	unsigned int remaining = prefix_len - i;
	unsigned int cmp = cn->len < remaining ? cn->len : remaining;
	unsigned int j;

	j = ft_match_compressed_key(&prefix[i], cn, cmp);
	if (j < cmp) {
		*count_ret = 0;
		return FT_DESCENT_END;
	}
	if (cn->len >= remaining) {
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata(
				(struct cds_ft_inode *) cn);

		/*
		 * Prefix ends inside this compressed path: the keys under the
		 * prefix are exactly those in the compressed node's subtree.
		 */
		*count_ret = ft->rank_stats ? ft_nr_keys_load(cn_meta) :
			ft_subtree_key_count(ft, node_flag);
		return FT_DESCENT_END;
	}
	*i_p = i + cn->len - 1;
	*node_flag_p = ft_cn_child_dereference_acquire_prefetch(cn);
	return FT_DESCENT_CONTINUE;
}

unsigned long cds_ft_count_keys_prefix(struct cds_ft *ft,
		const uint8_t *_prefix, size_t prefix_len)
{
	struct cds_ft_inode_flag *node_flag;
	unsigned int i;
	const struct cds_ft_key_map *km = &ft->group->key_map;
	uint8_t ordinal_buf[FT_MAX_KEY_LEN];
	const uint8_t *prefix;
	unsigned long count;

	CDS_FT_SCOPED_READER(ft);
	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	if (prefix_len > ft->group->max_key_len) {
		count = 0;
		goto out;
	}
	if (caa_likely(km->identity)) {
		prefix = _prefix;
	} else {
		ft_key_to_ordinals(ordinal_buf, _prefix, prefix_len, km);
		prefix = ordinal_buf;
	}

	node_flag = ft_root_dereference_acquire_prefetch(ft);

	for (i = 0; i < prefix_len; i++) {
		uint8_t kv;

		if (ft_node_external(node_flag)) {
			count = 0;
			goto out;
		}
		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;

			act = ft_count_prefix_compressed(
				ft, &node_flag, &i, prefix,
				prefix_len, &count);
			if (act == FT_DESCENT_END)
				goto out;
			continue;
		}
		kv = prefix[i];
		{
			unsigned int rewind;

			/*
			 * Surgical re-anchor on a skip-compressed mismatch (no
			 * spin on a frozen slot).  rewind > 0 means that a
			 * concurrent chain-merge moved the encoded position
			 * shallower.  The count is over a fixed prefix, so just
			 * re-descend from the root.  This is idempotent, and
			 * there is no rank to undercount.
			 */
			node_flag = ft_node_get_nth_reanchor(ft, node_flag, kv,
					&rewind);
			if (caa_unlikely(rewind)) {
				node_flag = ft_root_dereference_acquire_prefetch(ft);
				i = (unsigned int) -1;	/* loop ++ -> restart at 0 */
				continue;
			}
		}
	}

	if (!node_flag) {
		count = 0;
		goto out;
	}
	if (ft_node_internal(node_flag)) {
		struct cds_ft_metadata *metadata =
			cds_ft_item_to_metadata(ft_node_ptr(node_flag));
		count = ft->rank_stats ? ft_nr_keys_load(metadata) :
			ft_subtree_key_count(ft, node_flag);
		goto out;
	}
	if (ft_node_compressed(node_flag)) {
		struct cds_ft_metadata *cn_meta =
			cds_ft_item_to_metadata(ft_node_ptr(node_flag));
		count = ft->rank_stats ? ft_nr_keys_load(cn_meta) :
			ft_subtree_key_count(ft, node_flag);
		goto out;
	}
	/* External node: one key (possibly with duplicates). */
	count = 1;
out:
	FT_TP(count_prefix, count);
	return count;
}

unsigned long cds_ft_count_keys(struct cds_ft *ft)
{
	return cds_ft_count_keys_prefix(ft, NULL, 0);
}

/*
 * Count keys in a child node (nr_keys if internal, 1 if external).
 */
static inline
unsigned long ft_child_key_count(struct cds_ft_inode_flag *child)
{
	if (!ft_node_external(child)) {
		struct cds_ft_metadata *m =
			cds_ft_item_to_metadata(ft_node_ptr(child));
		return ft_nr_keys_load(m);
	}
	return 1;
}

/*
 * Handle compressed node in cds_ft_lookup_nth().
 *
 * Fill the compressed path into ordinal_key.  Advance level and
 * node_flag past the compressed segment.
 *
 * Return FT_DESCENT_BREAK if the child is an external (leaf) node,
 * FT_DESCENT_CONTINUE otherwise.
 */
static
enum ft_descent_action ft_lookup_nth_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		int *level_p, uint8_t *ordinal_key)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	int level = *level_p;
	struct cds_ft_compressed_node *cn =
		ft_compressed_node_ptr(node_flag);

	ft_fill_compressed_path(cn, ordinal_key, level - 1);
	level += cn->len - 1;
	node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
	assert(node_flag != NULL);	/* compressed node always has a live child (by construction) */
	if (ft_node_external(node_flag)) {
		/*
		 * The caller's loop exits on BREAK, so it skips the level++
		 * that lands a descended child at its depth.  Take that step
		 * here, so the leaf's key length counts the run's last byte.
		 */
		*node_flag_p = node_flag;
		*level_p = level + 1;
		return FT_DESCENT_BREAK;
	}
	*node_flag_p = node_flag;
	*level_p = level;
	return FT_DESCENT_CONTINUE;
}

/*
 * Lookup the nth key (0-indexed) in forward (smallest-first) order.
 *
 * Descends through the trie using per-node nr_keys counters to skip
 * entire subtrees, yielding O(depth) time complexity rather than
 * O(n) iteration.
 *
 * At each internal node:
 * 1. If external_nodes are present, they represent the key at this
 *    depth (shortest in the subtree). If n == 0, found. Else n -= 1.
 * 2. Iterate children in ascending ordinal order. For each child,
 *    determine its key count (nr_keys if internal, 1 if external).
 *    If n < count, descend. Else n -= count and continue.
 */
/*
 * EXCLUDE BULK OPS FOR A RANK WALK.  With rank stats off, a rank op is a walk:
 * lookup_first or lookup_last, then n steps.  A bulk op that moves keys
 * during it can make it count a key twice or not at all.  A lookup retries
 * until two passes agree.  A whole walk cannot: a stream of moves could keep
 * it from ever finishing.
 *
 * So exclude bulk ops for the length of the walk, as a point writer does
 * (ft_writer_lock_scope_enter).  With a bulk gate up, take the FT-wide lock:
 * a bulk op commits only while it holds that lock.  A bulk op whose gate
 * rises after this sample waits, in the gate's grace period, for the
 * caller's read section, which spans the walk.  On a COARSE trie only a
 * rekey owns that grace period.  A detach or graft may still start, but it
 * adds or removes keys, it does not move one.
 *
 * Nest if this thread already holds the lock.  Return true if the caller
 * must call ft_writer_lock_scope_exit() after the walk.
 */
static inline
bool ft_rank_walk_exclude_enter(struct cds_ft *ft)
{
	if (ft->exclusive || ft->external_sync)
		return false;
	if (ft_wlock_held == ft) {
		ft_wlock_depth++;
		return true;
	}
	if (caa_likely(!ft_bulk_active(ft)))
		return false;
	if (caa_unlikely(ft_wlock_held != NULL)) {
		fprintf(stderr, "cds_ft: a rank walk on cds_ft=%p while holding "
			"the FT-wide writer lock of cds_ft=%p\n", (void *) ft,
			(void *) ft_wlock_held);
		fflush(stderr);
		abort();
	}
	ft_writer_lock_take(ft);
	ft_wlock_held = ft;
	ft_wlock_depth = 1;
	return true;
}

/*
 * REKEY-COHERENT RANK DESCENT: the point lookup's two passes
 * (ft_lookup_two_descents).
 *
 * A rank descent reads one child's key count, then the next child's.  A
 * rekey that moves a subtree between two nodes changes both counts in one
 * commit.  A single descent that straddles it counts the subtree twice or
 * not at all, and returns the key at a wrong rank.
 *
 * With no move in flight, descend once.  The mover raises the gate and
 * waits a grace period before it changes anything, so nothing moves under
 * this read section.  Otherwise descend twice, and accept only if both
 * passes return the same status and node, over the same nodes.  Two
 * sequential passes cannot both straddle one commit.  The second pass
 * fills @iter.
 */
static
void ft_rank_two_pass(struct cds_ft *ft, struct cds_ft_iter *iter,
		unsigned long n,
		enum cds_ft_status (*pass)(struct cds_ft *,
			struct cds_ft_iter *, unsigned long,
			struct ft_visit_witness *))
{
	struct ft_visit_witness w1, w2;
	struct cds_ft_node *n1;
	enum cds_ft_status s1, s2;

	if (caa_likely(!ft_move_active(ft))) {
		(void) pass(ft, iter, n, NULL);
		return;
	}
	for (;;) {
		ft_witness_init(&w1);
		s1 = pass(ft, iter, n, &w1);
		n1 = iter->node;
		ft_witness_init(&w2);
		s2 = pass(ft, iter, n, &w2);
		if (s1 == s2 && n1 == iter->node && ft_witness_equal(&w1, &w2))
			return;
	}
}

/*
 * One rank descent of cds_ft_lookup_nth.
 * If @wit is not NULL, record in it every node the descent reads.
 */
static
enum cds_ft_status ft_lookup_nth_rank_pass(struct cds_ft *ft,
		struct cds_ft_iter *iter, unsigned long n,
		struct ft_visit_witness *wit)
{
	struct cds_ft_inode_flag *node_flag;
	uint8_t ordinal_key[FT_MAX_KEY_LEN];
	int level;
	unsigned long remaining = n;

	iter_debug_path_snapshot(iter);
	memset(ordinal_key, 0, ft->group->max_key_len * sizeof(ordinal_key[0]));

	node_flag = ft_root_dereference_acquire_prefetch(ft);

	ft_delay_reader();

	for (level = 1; ; level++) {
		struct cds_ft_metadata *metadata;
		struct cds_ft_inode_flag *child;
		struct cds_ft_node *ext;
		uint8_t child_key = 0;
		int pivot;

		if (wit)
			ft_witness_visit(wit, ft_node_ptr(node_flag));
		if (ft_node_external(node_flag))
			break;

		metadata = cds_ft_item_to_metadata(ft_node_ptr(node_flag));

		/* Keys at this node's depth come first in ordinal order. */
		ext = ft_dereference_external_acquire(metadata->external_nodes);
		if (ext) {
			if (remaining == 0) {
				/* Found: the key at this node's depth. */
				iter->key_len = level - 1;
				iter->key_off = 0;
				{
					int j;

					for (j = 0; j < level - 1; j++)
						iter_key(iter)[j] = ordinal_key[j];
				}
				iter->node = ext;
				iter->cache_valid = true;
				iter_debug_path_update(iter);
				iter->path_len = level;
				iter->status = CDS_FT_STATUS_OK;
				goto end;
			}
			remaining--;
		}

		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;

			act = ft_lookup_nth_compressed(&node_flag,
				&level, ordinal_key);
			if (act == FT_DESCENT_BREAK)
				break;
			continue;
		}
		/* Iterate children in ascending ordinal order. */
		pivot = -1;
		child = ft_node_get_direction(ft, node_flag, pivot, &child_key, FT_RIGHT, true);
		while (child) {
			unsigned long child_keys;

#ifdef FEATURE_FT_SKIP_COMPRESSED
			/*
			 * Skip-validate failure: a concurrent split/merge
			 * reparented the skip child.  @node_flag may have
			 * been recompacted away at the same time.  Re-anchor
			 * on the live structure through the skip child's
			 * parent chain.  The precise / inequality readers
			 * use the same single mechanism.  It is never a spin
			 * or a frozen re-read.
			 *   rewind == 0: @node_flag was recompacted in place.
			 *     Re-scan its live version from the same pivot.
			 *     The rank accumulation is unchanged: we only
			 *     re-scan children past @pivot.
			 *   rewind > 0: a transient single-child @node_flag
			 *     merged into a longer compressed.  Its pivot is
			 *     -1, so nothing is accumulated for it yet.
			 *     Descend INTO that merged node: re-scanning the
			 *     shallower holder would double-count.
			 *     level -= rewind + 1, so that the loop's level++
			 *     lands the compressed handler at the merged
			 *     node's depth.  remaining is unchanged.
			 */
			if (caa_unlikely(ft_node_skip_compressed(child))) {
				unsigned int rewind;
				struct cds_ft_inode_flag *merged;
				struct cds_ft_inode_flag *anchor;

				/*
				 * Surgical re-anchor: resolve @child via the
				 * live skip child's parent chain and continue
				 * forward.  Never re-scan a slot or restart
				 * from the root.  A re-descent from the root
				 * would re-count a concurrently-growing trie
				 * and undercount the rank.  ft_skip_reanchor
				 * never returns NULL on a well-formed trie:
				 * the writer wires every fresh cluster's
				 * parent before the cluster becomes reachable.
				 *   rewind == 0: @merged is the resolved live
				 *     child at @child_key.  Use it and fall
				 *     through to the rank accumulation, which
				 *     only advances.
				 *   rewind > 0:  @node_flag (a transient single
				 *     child) was absorbed into a longer
				 *     compressed.  Descend INTO it.
				 */
				anchor = ft_skip_reanchor(ft, child, &rewind, &merged);
				assert(anchor != NULL);
				if (caa_likely(rewind == 0)) {
					node_flag = anchor;
					child = merged;
				} else {
					node_flag = merged;
					level -= (int) rewind + 1;
					goto next_level;
				}
			}
#endif
			if (wit)
				ft_witness_visit(wit, ft_node_ptr(child));
			child_keys = ft_child_key_count(child);

			if (remaining < child_keys) {
				/* Target is in this child's subtree. Descend. */
				ordinal_key[level - 1] = child_key;
				node_flag = child;
				goto next_level;
			}
			remaining -= child_keys;
			pivot = child_key;
			/* Widen the step to the next child (FT_DELAY_INJECT). */
			ft_delay_reader();
			child = ft_node_get_direction(ft, node_flag, pivot, &child_key, FT_RIGHT, true);
		}

		/* Exhausted all children without finding. */
		break;

next_level:
		;
	}

	/* Reached a leaf (external node). */
	if (node_flag && ft_node_external(node_flag) && remaining == 0) {
		iter->key_len = level - 1;
		iter->key_off = 0;
		{
			int j;

			for (j = 0; j < level - 1; j++)
				iter_key(iter)[j] = ordinal_key[j];
		}
		iter->node = (struct cds_ft_node *) ft_node_ptr(node_flag);
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = level;
		iter->status = CDS_FT_STATUS_OK;
		goto end;
	}

	iter->node = NULL;
	iter->cache_valid = true;
	iter_debug_path_update(iter);
	iter->path_len = 0;
	iter->status = CDS_FT_STATUS_NOT_FOUND;

end:
	return iter->status;
}

enum cds_ft_status cds_ft_lookup_nth(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		unsigned long n)
{
	CDS_FT_SCOPED_READER(ft);
	FT_TP(lookup_nth_enter, n);

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	/*
	 * Order statistics off: there is no per-node rank to descend by.
	 * Position at the first (smallest) key and step forward n.
	 * skip_forward itself falls back to n x cds_ft_next.  When n is out of
	 * range, skip_forward returns NOT_FOUND and leaves the iterator
	 * unpositioned.  This matches the rank-on path.
	 */
	if (!ft->rank_stats) {
		bool excl = ft_rank_walk_exclude_enter(ft);
		enum cds_ft_status st = cds_ft_lookup_first(ft, iter);

		if (st == CDS_FT_STATUS_OK)
			st = cds_ft_iter_skip_forward(ft, iter, n);
		if (excl)
			ft_writer_lock_scope_exit(ft);
		FT_TP(lookup_nth_exit, (int) st);
		return st;
	}

	ft_rank_two_pass(ft, iter, n, ft_lookup_nth_rank_pass);
	iter_auto_invalidate_cache(iter);
	FT_TP(lookup_nth_exit, (int) iter->status);
	return iter->status;
}

/*
 * Handle compressed node in cds_ft_lookup_nth_last().
 *
 * In reverse order, process the child subtree first (larger keys),
 * then fall through to external_nodes (smallest = last).
 *
 * Return:
 * - FT_DESCENT_CONTINUE when descending into the child;
 * - FT_DESCENT_BREAK when the child is external (leaf);
 * - FT_DESCENT_END when the child keys are exhausted.  *remaining_p
 *   is updated.  The caller falls through to check_ext_nth_last.
 */
static
enum ft_descent_action ft_lookup_nth_last_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		int *level_p, unsigned long *remaining_p,
		uint8_t *ordinal_key)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	int level = *level_p;
	struct cds_ft_compressed_node *cn =
		ft_compressed_node_ptr(node_flag);

	unsigned long child_keys;

	assert(cn->child != NULL);	/* compressed node always has a live child */
	child_keys = ft_child_key_count(ft_resolve_flip_proxy(cn->child));

	if (*remaining_p < child_keys) {
		ft_fill_compressed_path(cn,
			ordinal_key, level - 1);
		level += cn->len - 1;
		node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
		assert(node_flag != NULL);	/* compressed node always has a live child */
		if (ft_node_external(node_flag)) {
			/* As in ft_lookup_nth_compressed: the loop skips level++. */
			*node_flag_p = node_flag;
			*level_p = level + 1;
			return FT_DESCENT_BREAK;
		}
		*node_flag_p = node_flag;
		*level_p = level;
		return FT_DESCENT_CONTINUE;
	}
	*remaining_p -= child_keys;
	return FT_DESCENT_END;
}

/*
 * Lookup the nth key (0-indexed) in reverse (largest-first) order.
 *
 * Same principle as cds_ft_lookup_nth, but it descends from the right.
 * At each internal node, iterate children in descending ordinal order
 * first.  Then check external_nodes last: they are the smallest key
 * in the subtree.
 */
/*
 * One rank descent of cds_ft_lookup_nth_last.
 * If @wit is not NULL, record in it every node the descent reads.
 */
static
enum cds_ft_status ft_lookup_nth_last_rank_pass(struct cds_ft *ft,
		struct cds_ft_iter *iter, unsigned long n,
		struct ft_visit_witness *wit)
{
	struct cds_ft_inode_flag *node_flag;
	uint8_t ordinal_key[FT_MAX_KEY_LEN];
	int level;
	unsigned long remaining = n;

	iter_debug_path_snapshot(iter);
	memset(ordinal_key, 0, ft->group->max_key_len * sizeof(ordinal_key[0]));

	node_flag = ft_root_dereference_acquire_prefetch(ft);

	for (level = 1; ; level++) {
		struct cds_ft_metadata *metadata;
		struct cds_ft_inode_flag *child;
		struct cds_ft_node *ext;
		uint8_t child_key = 0;
		int pivot;

		if (wit)
			ft_witness_visit(wit, ft_node_ptr(node_flag));
		if (ft_node_external(node_flag))
			break;

		metadata = cds_ft_item_to_metadata(ft_node_ptr(node_flag));

		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;

			act = ft_lookup_nth_last_compressed(
				&node_flag, &level, &remaining,
				ordinal_key);
			if (act == FT_DESCENT_BREAK)
				break;
			if (act == FT_DESCENT_END)
				goto check_ext_nth_last;
			continue;
		}
		/* Iterate children in descending ordinal order first. */
		pivot = FT_ENTRY_PER_NODE;
		child = ft_node_get_direction(ft, node_flag, pivot, &child_key, FT_LEFT, true);
		while (child) {
			unsigned long child_keys;

#ifdef FEATURE_FT_SKIP_COMPRESSED
			/* Skip-validate failure: re-anchor on the live structure
			 * (see cds_ft_lookup_nth -- same single mechanism). */
			if (caa_unlikely(ft_node_skip_compressed(child))) {
				unsigned int rewind;
				struct cds_ft_inode_flag *merged;
				struct cds_ft_inode_flag *anchor;

				/*
				 * Surgical re-anchor (see cds_ft_lookup_nth):
				 * resolve @child via the live skip child's
				 * parent chain and continue forward.  Never
				 * re-scan or restart from root.
				 * ft_skip_reanchor never returns NULL on a
				 * well-formed trie: the writer wires parents
				 * before publishing.
				 */
				anchor = ft_skip_reanchor(ft, child, &rewind, &merged);
				assert(anchor != NULL);
				if (caa_likely(rewind == 0)) {
					node_flag = anchor;
					child = merged;
				} else {
					node_flag = merged;
					level -= (int) rewind + 1;
					goto next_level;
				}
			}
#endif
			if (wit)
				ft_witness_visit(wit, ft_node_ptr(child));
			child_keys = ft_child_key_count(child);

			if (remaining < child_keys) {
				/* Target is in this child's subtree. Descend. */
				ordinal_key[level - 1] = child_key;
				node_flag = child;
				goto next_level;
			}
			remaining -= child_keys;
			pivot = child_key;
			/* Widen the step to the next child (FT_DELAY_INJECT). */
			ft_delay_reader();
			child = ft_node_get_direction(ft, node_flag, pivot, &child_key, FT_LEFT, true);
		}

check_ext_nth_last:
		/* External_nodes at this depth are the smallest (last in reverse). */
		ext = ft_dereference_external_acquire(metadata->external_nodes);
		if (ext) {
			if (remaining == 0) {
				iter->key_len = level - 1;
				iter->key_off = 0;
				{
					int j;

					for (j = 0; j < level - 1; j++)
						iter_key(iter)[j] = ordinal_key[j];
				}
				iter->node = ext;
				iter->cache_valid = true;
				iter_debug_path_update(iter);
				iter->path_len = level;
				iter->status = CDS_FT_STATUS_OK;
				goto end;
			}
			remaining--;
		}

		/* Exhausted all children without finding. */
		break;

next_level:
		;
	}

	/* Reached a leaf (external node). */
	if (node_flag && ft_node_external(node_flag) && remaining == 0) {
		iter->key_len = level - 1;
		iter->key_off = 0;
		{
			int j;

			for (j = 0; j < level - 1; j++)
				iter_key(iter)[j] = ordinal_key[j];
		}
		iter->node = (struct cds_ft_node *) ft_node_ptr(node_flag);
		iter->cache_valid = true;
		iter_debug_path_update(iter);
		iter->path_len = level;
		iter->status = CDS_FT_STATUS_OK;
		goto end;
	}

	iter->node = NULL;
	iter->cache_valid = true;
	iter_debug_path_update(iter);
	iter->path_len = 0;
	iter->status = CDS_FT_STATUS_NOT_FOUND;

end:
	return iter->status;
}

enum cds_ft_status cds_ft_lookup_nth_last(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		unsigned long n)
{
	CDS_FT_SCOPED_READER(ft);
	FT_TP(lookup_nth_last_enter, n);

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	/*
	 * Order statistics off: position at the last (largest) key and step
	 * back n.  skip_reverse falls back to n x cds_ft_prev.  Out-of-range n
	 * leaves the iterator unpositioned.  This matches the rank-on path.
	 */
	if (!ft->rank_stats) {
		bool excl = ft_rank_walk_exclude_enter(ft);
		enum cds_ft_status st = cds_ft_lookup_last(ft, iter);

		if (st == CDS_FT_STATUS_OK)
			st = cds_ft_iter_skip_reverse(ft, iter, n);
		if (excl)
			ft_writer_lock_scope_exit(ft);
		FT_TP(lookup_nth_last_exit, (int) st);
		return st;
	}

	ft_rank_two_pass(ft, iter, n, ft_lookup_nth_last_rank_pass);
	iter_auto_invalidate_cache(iter);
	FT_TP(lookup_nth_last_exit, (int) iter->status);
	return iter->status;
}

/*
 * Match a compressed node's path bytes against @key starting at @i
 * and step past it.
 *
 * On success, return FT_DESCENT_CONTINUE after these steps:
 * - fill ordinal_key across the compressed span;
 * - advance *i_p, so that the caller's for-loop increment lands on
 *   the next byte after the span;
 * - update *node_flag_p to cn->child.
 *
 * Return FT_DESCENT_END on any mismatch: the key is shorter than the
 * compressed path, or a byte differs.
 */
static inline_lookup
enum ft_descent_action ft_rebuild_path_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		unsigned int *i_p,
		const uint8_t *key, size_t key_len,
		uint8_t *ordinal_key)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	struct cds_ft_compressed_node *cn = ft_compressed_node_ptr(node_flag);
	unsigned int i = *i_p;
	unsigned int j;

	if (i + cn->len > key_len)
		return FT_DESCENT_END;
	for (j = 0; j < cn->len; j++) {
		uint8_t ord = key[i + j];

		if (ord != cn->key_bytes[j])
			return FT_DESCENT_END;
		ordinal_key[i + j] = ord;
	}
	i += cn->len - 1;
	node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
	assert(node_flag != NULL);	/* compressed node always has a live child (by construction) */
	*node_flag_p = node_flag;
	*i_p = i;
	return FT_DESCENT_CONTINUE;
}

/*
 * Re-descend from the root following @key to rebuild the iterator path
 * and ordinal_key arrays.  Returns the depth reached, or -1 on error.
 */
static inline_lookup
int ft_rebuild_path(struct cds_ft *ft,
		const uint8_t *key, size_t key_len,
		uint8_t *ordinal_key,
		struct cds_ft_inode_flag **deepest_p)
{
	struct cds_ft_inode_flag *node_flag;
	unsigned int i;

	node_flag = ft_root_dereference_acquire_prefetch(ft);

	for (i = 0; i < key_len; i++) {
		uint8_t ordinal;

		if (ft_node_external(node_flag))
			return -1;

		if (ft_node_compressed(node_flag)) {
			enum ft_descent_action act;

			act = ft_rebuild_path_compressed(&node_flag, &i,
				key, key_len, ordinal_key);
			if (act == FT_DESCENT_END)
				return -1;
			assert(act == FT_DESCENT_CONTINUE);
			continue;
		}
		ordinal = key[i];
		ordinal_key[i] = ordinal;
		{
			unsigned int rewind;

			/*
			 * Surgical re-anchor (no frozen-slot spin).  rewind > 0:
			 * a concurrent chain-merge moved the position
			 * shallower.  Rebuild this fixed path from the root
			 * (idempotent).
			 */
			node_flag = ft_node_get_nth_reanchor(ft, node_flag, ordinal,
					&rewind);
			if (caa_unlikely(rewind)) {
				node_flag = ft_root_dereference_acquire_prefetch(ft);
				i = (unsigned int) -1;	/* loop ++ -> restart at 0 */
				continue;
			}
		}
		if (!node_flag)
			return -1;
	}
	/*
	 * Deepest node reached: the node at depth key_len.  It is always
	 * placed at depth key_len as a child, so its shallow boundary is
	 * key_len.  This lets the going-up cursor seed live from the descent.
	 */
	*deepest_p = node_flag;
	return (int) key_len;
}

/*
 * Handle compressed node in cds_ft_iter_skip_forward's descend_forward
 * loop.
 *
 * Fill the ordinal_key entries for the compressed path.  Then advance
 * level and node_flag past the compressed segment.
 *
 * Return FT_DESCENT_BREAK if the child is an external (leaf) node,
 * FT_DESCENT_CONTINUE otherwise.
 */
static inline_lookup
enum ft_descent_action ft_skip_forward_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		int *level_p, uint8_t *ordinal_key)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	int level = *level_p;
	struct cds_ft_compressed_node *cn =
		ft_compressed_node_ptr(node_flag);
	int j;

	for (j = 0; j < cn->len; j++) {
		ordinal_key[level + j] = cn->key_bytes[j];
	}
	level += cn->len;
	node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
	assert(node_flag != NULL);	/* compressed node always has a live child (by construction) */
	if (ft_node_external(node_flag)) {
		*node_flag_p = node_flag;
		*level_p = level;
		return FT_DESCENT_BREAK;
	}
	*node_flag_p = node_flag;
	*level_p = level;
	return FT_DESCENT_CONTINUE;
}

/*
 * Skip forward by @n keys from the current iterator position using
 * local traversal.
 *
 * Walk up from the current leaf.  At each ancestor, count the keys in
 * rightward siblings, until enough are accumulated.  Then descend into
 * the target subtree using the lookup_nth algorithm.  Only touch
 * nodes between the start and end positions, so concurrent mutations
 * in unrelated key ranges do not affect the result.
 */
enum cds_ft_status cds_ft_iter_skip_forward(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		unsigned long n)
{
	uint8_t ordinal_key[FT_MAX_KEY_LEN];
	unsigned long remaining;
	int depth, level = 0;
	bool at_external_nodes;
	struct cds_ft_inode_flag *deepest = NULL;
	struct cds_ft_inode_flag *descend_from = NULL;

	CDS_FT_SCOPED_READER(ft);
	FT_TP(iter_skip_forward_enter, n);

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	if (!iter->node) {
		FT_TP(iter_skip_forward_exit, (int) CDS_FT_STATUS_NOT_FOUND);
		return CDS_FT_STATUS_NOT_FOUND;
	}
	if (n == 0) {
		FT_TP(iter_skip_forward_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

	/*
	 * Order statistics off: no per-node count to jump whole subtrees by, so
	 * step forward one key at a time.  cds_ft_next unpositions the iterator
	 * and returns NOT_FOUND when it runs off the end.  This is exactly the
	 * out-of-range result the rank-on path produces.
	 */
	if (!ft->rank_stats) {
		bool excl = ft_rank_walk_exclude_enter(ft);
		enum cds_ft_status st = CDS_FT_STATUS_OK;
		unsigned long i;

		for (i = 0; i < n && st == CDS_FT_STATUS_OK; i++)
			st = cds_ft_next(ft, iter);
		if (excl)
			ft_writer_lock_scope_exit(ft);
		FT_TP(iter_skip_forward_exit, (int) st);
		return st;
	}

	iter_debug_path_snapshot(iter);

	/* Rebuild path from root to current key. */
	depth = ft_rebuild_path(ft, ft_iter_read_key(iter), ft_iter_resolve_key_len(iter),
			ordinal_key, &deepest);
	if (depth < 0)
		goto not_found;
	(void) deepest;		/* used by the going-up seed under PP backtrack */

	remaining = n;

	/*
	 * Determine whether the current key sits at an internal or
	 * compressed node's external_nodes (variable-length prefix key)
	 * or at a leaf child.
	 */
	at_external_nodes = !ft_node_external(deepest);

	/*
	 * If at external_nodes of an internal/compressed node, all
	 * children of that node are to the right.  Try to satisfy the
	 * skip within them.
	 */
	if (at_external_nodes) {
		struct cds_ft_inode_flag *parent = deepest;
		struct cds_ft_metadata *pmeta =
			cds_ft_item_to_metadata(ft_node_ptr(parent));
		unsigned long right_keys = ft_nr_keys_load(pmeta) - 1; /* exclude self */

		if (remaining <= right_keys) {
			/*
			 * Target is among the children.  Use lookup_nth
			 * descent within this subtree, skipping the
			 * external_nodes (already behind us).
			 */
			remaining--;  /* skip external_nodes key (us) */

			if (ft_node_compressed(parent)) {
				struct cds_ft_compressed_node *cn =
					ft_compressed_node_ptr(parent);
				unsigned long ck;
				int j;

				assert(cn->child != NULL);	/* compressed node always has a live child */
				ck = ft_child_key_count(ft_resolve_flip_proxy(cn->child));
				if (remaining < ck) {
					for (j = 0; j < cn->len; j++) {
						ordinal_key[depth + j] =
							cn->key_bytes[j];
					}
					level = depth + cn->len;
					descend_from = ft_resolve_flip_proxy(cn->child);
					goto descend_forward;
				}
				remaining -= ck;
			} else {
				struct cds_ft_inode_flag *child;
				uint8_t child_key = 0;
				int pivot = -1;

				child = ft_node_get_direction(ft, parent, pivot,
						&child_key, FT_RIGHT, true);
				while (child) {
					unsigned long ck;

#ifdef FEATURE_FT_SKIP_COMPRESSED
					/*
					 * Surgical re-anchor: @parent carries
					 * external_nodes, so it cannot merge.
					 * rewind is always 0 (recompacted in
					 * place).  @at_pos is the live resolved
					 * sibling at @child_key.  Use it
					 * directly.  ft_skip_reanchor never
					 * returns NULL on a well-formed trie.
					 */
					if (caa_unlikely(ft_node_skip_compressed(child))) {
						unsigned int rewind;
						struct cds_ft_inode_flag *at_pos;
						struct cds_ft_inode_flag *anchor =
							ft_skip_reanchor(ft, child, &rewind, &at_pos);

						assert(anchor != NULL);
						assert(rewind == 0);
						parent = anchor;
						child = at_pos;
						continue;
					}
#endif
					ck = ft_child_key_count(child);
					if (remaining < ck) {
						ordinal_key[depth] = child_key;
						level = depth + 1;
						descend_from = child;
						goto descend_forward;
					}
					remaining -= ck;
					pivot = child_key;
					child = ft_node_get_direction(ft, parent, pivot,
							&child_key, FT_RIGHT, true);
				}
			}
		}
		remaining -= right_keys;
		/* Continue walking up from depth-1. */
		level = depth;
	} else {
		/* At a leaf child: start walking up from the parent. */
		level = depth;
	}

	/*
	 * Walk up: at each ancestor, count keys in rightward siblings
	 * of the child we came from.
	 *
	 * Parent-pointer backtrack: @up_node is the live node covering
	 * depth @level.  It climbs via ft_get_parent_rcu as @level
	 * decrements.  A compressed node spans cn->len levels, an internal
	 * node spans 1.  @up_node is seeded live from ft_rebuild_path's
	 * deepest node: @deepest is the node at @level == @depth.  It is
	 * placed there as a child, so its shallow boundary is @level.
	 */
	{	/* Scope the going-up cursor locals so they are out of scope at not_found/descend_* (avoids -Wjump-misses-init false positives). */
		struct cds_ft_inode_flag *up_node = deepest;
		ssize_t up_node_lo = level;
		for (level--; level >= 0; level--) {
			struct cds_ft_inode_flag *ancestor;
			struct cds_ft_inode_flag *child;
			uint8_t child_key = 0;
			int pivot;

			while (level < up_node_lo) {
				struct cds_ft_inode_flag *gp = ft_get_parent_rcu(ft, up_node);

				up_node_lo -= (gp && ft_node_compressed(gp)) ?
					(ssize_t) ft_compressed_node_ptr(gp)->len : 1;
				up_node = gp;
			}
			ancestor = up_node;
			if (ft_node_external(ancestor))
				continue;
			/*
			 * Compressed path levels have no siblings: skip.
			 */
			if (ft_node_compressed(ancestor))
				continue;

			pivot = ordinal_key[level];
			child = ft_node_get_direction(ft, ancestor, pivot,
					&child_key, FT_RIGHT, true);
			while (child) {
				unsigned long ck;

	#ifdef FEATURE_FT_SKIP_COMPRESSED
				/*
				 * Surgical re-anchor: @ancestor has siblings to
				 * scan, so it is multi-child and cannot merge.
				 * So rewind is 0.  @at_pos is the live resolved
				 * sibling at @child_key.  Use it directly.
				 * ft_skip_reanchor never returns NULL on a
				 * well-formed trie.
				 */
				if (caa_unlikely(ft_node_skip_compressed(child))) {
					unsigned int rewind;
					struct cds_ft_inode_flag *at_pos;
					struct cds_ft_inode_flag *anchor =
						ft_skip_reanchor(ft, child, &rewind, &at_pos);

					assert(anchor != NULL);
					assert(rewind == 0);
					ancestor = anchor;
					up_node = anchor;	/* live holder at @level (internal, span 1) */
					up_node_lo = level;
					child = at_pos;
					continue;
				}
	#endif
				ck = ft_child_key_count(child);
				if (remaining <= ck) {
					remaining--;  /* enter this subtree (1-indexed within) */
					ordinal_key[level] = child_key;
					level = level + 1;
					descend_from = child;
					goto descend_forward;
				}
				remaining -= ck;
				pivot = child_key;
				child = ft_node_get_direction(ft, ancestor, pivot,
						&child_key, FT_RIGHT, true);
			}
			/*
			 * No external_nodes to count going up in forward direction
			 * (they sort before children, so they're behind us).
			 */
		}
	}

	/* Exhausted the trie. */
not_found:
	iter->node = NULL;
	iter->cache_valid = true;
	iter_debug_path_update(iter);
	iter->path_len = 0;
	iter->status = CDS_FT_STATUS_NOT_FOUND;
	goto end;

descend_forward:
	/*
	 * We found the subtree containing the target.  Now descend into
	 * it using the forward lookup_nth algorithm.  At each internal
	 * node, check external_nodes first.  Then iterate children in
	 * ascending ordinal order.
	 */
	{
		/*
		 * @descend_from is the node at descent level @level.  The
		 * going-up / at-external block placed it there right before
		 * its goto here.
		 */
		struct cds_ft_inode_flag *node_flag = descend_from;

		for (;;) {
			struct cds_ft_metadata *metadata;
			struct cds_ft_inode_flag *child;
			uint8_t child_key = 0;
			int pivot;

			if (ft_node_external(node_flag))
				break;

			metadata = cds_ft_item_to_metadata(
					ft_node_ptr(node_flag));

			{
				struct cds_ft_node *ext =
					ft_dereference_external_acquire(
						metadata->external_nodes);

				if (ext) {
				if (remaining == 0) {
					int j;

					iter->key_len = level;
					iter->key_off = 0;
					for (j = 0; j < level; j++)
						iter_key(iter)[j] = ordinal_key[j];
					iter->node = ext;
					iter->cache_valid = true;
					iter_debug_path_update(iter);
					iter->path_len = level + 1;
					iter->status = CDS_FT_STATUS_OK;
					goto end;
				}
				remaining--;
			}
			} /* ext scope */

			if (ft_node_compressed(node_flag)) {
				enum ft_descent_action act;

				act = ft_skip_forward_compressed(
					&node_flag, &level,
					ordinal_key);
				if (act == FT_DESCENT_BREAK)
					break;
				continue;
			}
			pivot = -1;
			child = ft_node_get_direction(ft, node_flag, pivot,
					&child_key, FT_RIGHT, true);
			while (child) {
				unsigned long ck;

#ifdef FEATURE_FT_SKIP_COMPRESSED
				/*
				 * Surgical re-anchor (descend phase; see cds_ft_lookup_nth).
				 *   rewind == 0: @merged is the live resolved child at
				 *     @child_key.  Use it directly.
				 *   rewind > 0: a transient single-child
				 *     @node_flag (pivot == -1, nothing
				 *     accumulated) merged into a longer
				 *     compressed.  Descend INTO it.
				 *     level -= rewind lands the compressed
				 *     handler at the merged node's depth:
				 *     ft_skip_forward_compressed fills from
				 *     level.  remaining is unchanged.
				 * ft_skip_reanchor never returns NULL on a well-formed trie.
				 */
				if (caa_unlikely(ft_node_skip_compressed(child))) {
					unsigned int rewind;
					struct cds_ft_inode_flag *merged;
					struct cds_ft_inode_flag *anchor =
						ft_skip_reanchor(ft, child, &rewind, &merged);

					assert(anchor != NULL);
					if (caa_likely(rewind == 0)) {
						node_flag = anchor;
						child = merged;
						continue;
					}
					node_flag = merged;
					level -= (int) rewind;
					goto next_forward_level;
				}
#endif
				ck = ft_child_key_count(child);
				if (remaining < ck) {
					ordinal_key[level] = child_key;
					level++;
					node_flag = child;
					goto next_forward_level;
				}
				remaining -= ck;
				pivot = child_key;
				child = ft_node_get_direction(ft, node_flag, pivot,
						&child_key, FT_RIGHT, true);
			}
			break;

next_forward_level:
			;
		}

		/* Reached a leaf. */
		if (ft_node_ptr(node_flag) &&
		    ft_node_external(node_flag) &&
		    remaining == 0) {
			int j;

			iter->key_len = level;
			iter->key_off = 0;
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
			iter->node = (struct cds_ft_node *)
				ft_node_ptr(node_flag);
			iter->cache_valid = true;
			iter_debug_path_update(iter);
			iter->path_len = level + 1;
			iter->status = CDS_FT_STATUS_OK;
			goto end;
		}
		goto not_found;
	}

end:
	iter_auto_invalidate_cache(iter);
	FT_TP(iter_skip_forward_exit, (int) iter->status);
	return iter->status;
}

/*
 * Handle compressed node in cds_ft_iter_skip_reverse's descend_reverse
 * loop.
 *
 * In reverse, process the child subtree first (larger keys), then
 * fall through to external_nodes (smallest = last).
 *
 * Return:
 * - FT_DESCENT_CONTINUE when descending into the child;
 * - FT_DESCENT_BREAK when the child is external (leaf);
 * - FT_DESCENT_END when the child keys are exhausted.  *remaining_p
 *   is updated.  The caller falls through to
 *   check_ext_descend_reverse.
 */
static inline_lookup
enum ft_descent_action ft_skip_reverse_compressed(
		struct cds_ft_inode_flag **node_flag_p,
		int *level_p, unsigned long *remaining_p,
		uint8_t *ordinal_key)
{
	struct cds_ft_inode_flag *node_flag = *node_flag_p;
	int level = *level_p;
	struct cds_ft_compressed_node *cn =
		ft_compressed_node_ptr(node_flag);

	unsigned long ck;

	assert(cn->child != NULL);	/* compressed node always has a live child */
	ck = ft_child_key_count(ft_resolve_flip_proxy(cn->child));

	if (*remaining_p < ck) {
		int j;

		for (j = 0; j < cn->len; j++) {
			ordinal_key[level + j] = cn->key_bytes[j];
		}
		level += cn->len;
		node_flag = ft_cn_child_dereference_acquire_prefetch(cn);
		assert(node_flag != NULL);	/* compressed node always has a live child */
		if (ft_node_external(node_flag)) {
			*node_flag_p = node_flag;
			*level_p = level;
			return FT_DESCENT_BREAK;
		}
		*node_flag_p = node_flag;
		*level_p = level;
		return FT_DESCENT_CONTINUE;
	}
	*remaining_p -= ck;
	return FT_DESCENT_END;
}

/*
 * Handle a compressed ancestor in cds_ft_iter_skip_reverse's walk-up
 * loop.  The caller skips intermediate path levels (same compressed
 * node at adjacent levels).  At the entry level, the only candidate is
 * the compressed node's external_nodes.  They sort before all children,
 * so they are leftward of the current key.  Count or claim them.
 *
 * Return FT_DESCENT_END when the external_nodes match: the iter
 * has been written and iter->status is set to CDS_FT_STATUS_OK.
 * Otherwise return FT_DESCENT_CONTINUE for the caller to keep walking up.
 */
static inline_lookup
enum ft_descent_action ft_skip_reverse_walk_up_compressed(
		struct cds_ft_inode_flag *ancestor,
		int level,
		unsigned long *remaining_p,
		uint8_t *ordinal_key,
		struct cds_ft_iter *iter)
{
	struct cds_ft_metadata *ameta;
	struct cds_ft_node *a_ext;

	/*
	 * The caller (cds_ft_iter_skip_reverse) guarantees that this is the
	 * compressed ancestor's shallow boundary.  The intermediate-level
	 * skip is decided there, so this code need not re-derive it.
	 */
	ameta = cds_ft_item_to_metadata(ft_node_ptr(ancestor));
	a_ext = ft_dereference_external_acquire(ameta->external_nodes);
	if (a_ext) {
		if (*remaining_p == 1) {
			int j;

			iter->key_len = level;
			iter->key_off = 0;
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
			iter->node = a_ext;
			iter->cache_valid = true;
			iter_debug_path_update(iter);
			iter->path_len = level + 1;
			iter->status = CDS_FT_STATUS_OK;
			return FT_DESCENT_END;
		}
		(*remaining_p)--;
	}
	return FT_DESCENT_CONTINUE;
}

/*
 * Skip backward by @n keys from the current iterator position using
 * local traversal.
 *
 * Walk up from the current leaf.  At each ancestor, count the keys in
 * leftward siblings and in external_nodes, which sort before children.
 * When enough are accumulated, descend into the target subtree using
 * the reverse lookup_nth_last algorithm.
 */
enum cds_ft_status cds_ft_iter_skip_reverse(struct cds_ft *ft,
		struct cds_ft_iter *iter,
		unsigned long n)
{
	uint8_t ordinal_key[FT_MAX_KEY_LEN];
	unsigned long remaining;
	int depth, level;
	bool at_external_nodes;
	struct cds_ft_inode_flag *deepest = NULL;
	struct cds_ft_inode_flag *descend_from = NULL;

	CDS_FT_SCOPED_READER(ft);
	FT_TP(iter_skip_reverse_enter, n);

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	if (!iter->node) {
		FT_TP(iter_skip_reverse_exit, (int) CDS_FT_STATUS_NOT_FOUND);
		return CDS_FT_STATUS_NOT_FOUND;
	}
	if (n == 0) {
		FT_TP(iter_skip_reverse_exit, (int) CDS_FT_STATUS_OK);
		return CDS_FT_STATUS_OK;
	}

	/*
	 * Order statistics off: step backward one key at a time.  cds_ft_prev
	 * unpositions the iterator and returns NOT_FOUND when it runs off the
	 * front.  This matches the out-of-range result of the rank-on path.
	 */
	if (!ft->rank_stats) {
		bool excl = ft_rank_walk_exclude_enter(ft);
		enum cds_ft_status st = CDS_FT_STATUS_OK;
		unsigned long i;

		for (i = 0; i < n && st == CDS_FT_STATUS_OK; i++)
			st = cds_ft_prev(ft, iter);
		if (excl)
			ft_writer_lock_scope_exit(ft);
		FT_TP(iter_skip_reverse_exit, (int) st);
		return st;
	}

	iter_debug_path_snapshot(iter);

	/* Rebuild path from root to current key. */
	depth = ft_rebuild_path(ft, ft_iter_read_key(iter), ft_iter_resolve_key_len(iter),
			ordinal_key, &deepest);
	if (depth < 0)
		goto not_found;
	(void) deepest;		/* used by the going-up seed under PP backtrack */

	remaining = n;

	/*
	 * Determine whether the current key sits at an internal or
	 * compressed node's external_nodes or at a leaf child.
	 */
	at_external_nodes = !ft_node_external(deepest);
	(void) at_external_nodes;

	/*
	 * Whether at external_nodes or at a leaf child, we start the
	 * upward walk from the same level.  (At external_nodes all
	 * children are to the right, so there's nothing to the left.)
	 */
	level = depth;

	/*
	 * Walk up: at each ancestor, count keys in leftward siblings
	 * of the child we came from.  Also count external_nodes at the
	 * ancestor: they sort before all children.
	 *
	 * Parent-pointer backtrack: @up_node is the live node covering
	 * depth @level.  It climbs via ft_get_parent_rcu as @level
	 * decrements.  It is seeded live from ft_rebuild_path's deepest
	 * node: @deepest is the node at @level == @depth.  It is placed
	 * there as a child, so its shallow boundary is @level.
	 */
	{	/* Scope the going-up cursor locals so they are out of scope at not_found/descend_* (avoids -Wjump-misses-init false positives). */
		struct cds_ft_inode_flag *up_node = deepest;
		ssize_t up_node_lo = level;
		for (level--; level >= 0; level--) {
			struct cds_ft_inode_flag *ancestor;
			struct cds_ft_inode_flag *child;
			struct cds_ft_metadata *ameta;
			uint8_t child_key = 0;
			unsigned long left_keys = 0;
			int pivot;

			while (level < up_node_lo) {
				struct cds_ft_inode_flag *gp = ft_get_parent_rcu(ft, up_node);

				up_node_lo -= (gp && ft_node_compressed(gp)) ?
					(ssize_t) ft_compressed_node_ptr(gp)->len : 1;
				up_node = gp;
			}
			ancestor = up_node;
			if (ft_node_external(ancestor))
				continue;
			/*
			 * Skip intermediate compressed path levels (same
			 * compressed node at adjacent levels).  At the entry
			 * level, only external_nodes matter (no siblings).
			 */
			if (ft_node_compressed(ancestor)) {
				enum ft_descent_action act;
				bool at_shallow_boundary;

				/*
				 * Process the compressed ancestor's external_nodes only
				 * at its shallow boundary; skip the intermediate in-span
				 * levels.  The live cursor's shallow bound up_node_lo
				 * equals level there.
				 */
				at_shallow_boundary = (level == 0) ||
					(up_node_lo == level);
				if (!at_shallow_boundary)
					continue;
				act = ft_skip_reverse_walk_up_compressed(ancestor,
					level, &remaining, ordinal_key, iter);
				if (act == FT_DESCENT_END)
					goto end;
				assert(act == FT_DESCENT_CONTINUE);
				continue;
			}

			ameta = cds_ft_item_to_metadata(ft_node_ptr(ancestor));

			/* Count leftward siblings. */
			pivot = ordinal_key[level];
			child = ft_node_get_direction(ft, ancestor, pivot,
					&child_key, FT_LEFT, true);
			while (child) {
	#ifdef FEATURE_FT_SKIP_COMPRESSED
				/*
				 * Surgical re-anchor: @ancestor has siblings to
				 * count, so it is multi-child and cannot merge.
				 * So rewind is 0.  @at_pos is the live resolved
				 * sibling at @child_key.  Use it directly.
				 * ft_skip_reanchor never returns NULL on a
				 * well-formed trie.
				 */
				if (caa_unlikely(ft_node_skip_compressed(child))) {
					unsigned int rewind;
					struct cds_ft_inode_flag *at_pos;
					struct cds_ft_inode_flag *anchor =
						ft_skip_reanchor(ft, child, &rewind, &at_pos);

					assert(anchor != NULL);
					assert(rewind == 0);
					ancestor = anchor;
					up_node = anchor;
					up_node_lo = level;
					child = at_pos;
					continue;
				}
	#endif
				left_keys += ft_child_key_count(child);
				pivot = child_key;
				child = ft_node_get_direction(ft, ancestor, pivot,
						&child_key, FT_LEFT, true);
			}

			/* External_nodes at ancestor sort before all children. */
			{
				struct cds_ft_node *a_ext =
					ft_dereference_external_acquire(ameta->external_nodes);

				if (a_ext)
					left_keys++;

				if (remaining <= left_keys) {
					/*
					 * Target is among the leftward siblings or
					 * external_nodes.  Descend in reverse order:
					 * iterate leftward siblings from the current
					 * child in descending ordinal order, then
					 * check external_nodes last.
					 */
					pivot = ordinal_key[level];
					child = ft_node_get_direction(ft, ancestor, pivot,
							&child_key, FT_LEFT, true);
					while (child) {
						unsigned long ck;

	#ifdef FEATURE_FT_SKIP_COMPRESSED
						/*
						 * Surgical re-anchor: @ancestor
						 * is the multi-child node whose
						 * leftward siblings we iterate.
						 * So it cannot merge, and
						 * rewind is 0.  @at_pos is
						 * the live resolved sibling
						 * at @child_key.  Use it
						 * directly.  ft_skip_reanchor
						 * never returns NULL on a
						 * well-formed trie.
						 */
						if (caa_unlikely(ft_node_skip_compressed(child))) {
							unsigned int rewind;
							struct cds_ft_inode_flag *at_pos;
							struct cds_ft_inode_flag *anchor =
								ft_skip_reanchor(ft, child, &rewind, &at_pos);

							assert(anchor != NULL);
							assert(rewind == 0);
							ancestor = anchor;
							up_node = anchor;
							up_node_lo = level;
							child = at_pos;
							continue;
						}
	#endif
						ck = ft_child_key_count(child);
						if (remaining <= ck) {
							remaining--;
							ordinal_key[level] = child_key;
							level = level + 1;
							descend_from = child;
							goto descend_reverse;
						}
						remaining -= ck;
						pivot = child_key;
						child = ft_node_get_direction(ft, 
								ancestor, pivot,
								&child_key, FT_LEFT, true);
					}

					/* Must be the external_nodes. */
					if (a_ext && remaining == 1) {
						int j;

						iter->key_len = level;
						iter->key_off = 0;
						for (j = 0; j < level; j++)
							iter_key(iter)[j] = ordinal_key[j];
						iter->node = a_ext;
					iter->cache_valid = true;
					iter_debug_path_update(iter);
					iter->path_len = level + 1;
					iter->status = CDS_FT_STATUS_OK;
					goto end;
				}
				/* Shouldn't happen if left_keys was correct. */
				goto not_found;
			}
			remaining -= left_keys;
			}
		}
	}

	/* Exhausted the trie. */
not_found:
	iter->node = NULL;
	iter->cache_valid = true;
	iter_debug_path_update(iter);
	iter->path_len = 0;
	iter->status = CDS_FT_STATUS_NOT_FOUND;
	goto end;

descend_reverse:
	/*
	 * Descend into the target subtree using the reverse algorithm.
	 * At each internal node, iterate children in descending ordinal
	 * order first.  Then check external_nodes last.
	 */
	{
		/*
		 * @descend_from is the node at descent level @level.  The
		 * going-up block placed it there right before its goto here.
		 */
		struct cds_ft_inode_flag *node_flag = descend_from;

		for (;;) {
			struct cds_ft_metadata *metadata;
			struct cds_ft_inode_flag *child;
			uint8_t child_key = 0;
			int pivot;

			if (ft_node_external(node_flag))
				break;

			metadata = cds_ft_item_to_metadata(
					ft_node_ptr(node_flag));

			if (ft_node_compressed(node_flag)) {
				enum ft_descent_action act;

				act = ft_skip_reverse_compressed(
					&node_flag, &level, &remaining,
					ordinal_key);
				if (act == FT_DESCENT_BREAK)
					break;
				if (act == FT_DESCENT_END)
					goto check_ext_descend_reverse;
				continue;
			}
			pivot = FT_ENTRY_PER_NODE;
			child = ft_node_get_direction(ft, node_flag, pivot,
					&child_key, FT_LEFT, true);
			while (child) {
				unsigned long ck;

#ifdef FEATURE_FT_SKIP_COMPRESSED
				/*
				 * Surgical re-anchor (descend phase; see cds_ft_lookup_nth).
				 *   rewind == 0: @merged is the live resolved child at
				 *     @child_key.  Use it directly.
				 *   rewind > 0: a transient single-child
				 *     @node_flag merged into a longer
				 *     compressed.  Descend INTO it (level -=
				 *     rewind; remaining unchanged).
				 * ft_skip_reanchor never returns NULL on a well-formed trie.
				 */
				if (caa_unlikely(ft_node_skip_compressed(child))) {
					unsigned int rewind;
					struct cds_ft_inode_flag *merged;
					struct cds_ft_inode_flag *anchor =
						ft_skip_reanchor(ft, child, &rewind, &merged);

					assert(anchor != NULL);
					if (caa_likely(rewind == 0)) {
						node_flag = anchor;
						child = merged;
						continue;
					}
					node_flag = merged;
					level -= (int) rewind;
					goto next_reverse_level;
				}
#endif
				ck = ft_child_key_count(child);
				if (remaining < ck) {
					ordinal_key[level] = child_key;
					level++;
					node_flag = child;
					goto next_reverse_level;
				}
				remaining -= ck;
				pivot = child_key;
				child = ft_node_get_direction(ft, node_flag, pivot,
						&child_key, FT_LEFT, true);
			}

check_ext_descend_reverse:
			{
				struct cds_ft_node *ext =
					ft_dereference_external_acquire(
						metadata->external_nodes);

				if (ext && remaining == 0) {
				int j;

				iter->key_len = level;
				iter->key_off = 0;
				for (j = 0; j < level; j++)
					iter_key(iter)[j] = ordinal_key[j];
				iter->node = ext;
				iter->cache_valid = true;
				iter_debug_path_update(iter);
				iter->path_len = level + 1;
				iter->status = CDS_FT_STATUS_OK;
				goto end;
			}
			}
			break;

next_reverse_level:
			;
		}

		/* Reached a leaf. */
		if (ft_node_ptr(node_flag) &&
		    ft_node_external(node_flag) &&
		    remaining == 0) {
			int j;

			iter->key_len = level;
			iter->key_off = 0;
			for (j = 0; j < level; j++)
				iter_key(iter)[j] = ordinal_key[j];
			iter->node = (struct cds_ft_node *)
				ft_node_ptr(node_flag);
			iter->cache_valid = true;
			iter_debug_path_update(iter);
			iter->path_len = level + 1;
			iter->status = CDS_FT_STATUS_OK;
			goto end;
		}
		goto not_found;
	}

end:
	iter_auto_invalidate_cache(iter);
	FT_TP(iter_skip_reverse_exit, (int) iter->status);
	return iter->status;
}

