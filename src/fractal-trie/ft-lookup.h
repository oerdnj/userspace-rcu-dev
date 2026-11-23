// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-lookup.h
 *
 * Userspace RCU library - Fractal Trie: point lookups: exact / eager / candidate / partial / longest-match.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-lookup.h is an implementation unit; #include it from fractal-trie.c only"
#endif

/*
 * ===========================================================================
 * REKEY-COHERENT POINT LOOKUPS: TWO FORWARD DESCENTS.
 * ===========================================================================
 *
 * FIRST, the MODE GATE (struct cds_ft::move_active).  With no move in flight
 * the coherent entry points tail-call the ordinary specialization.  The whole
 * mechanism then costs one load of a quiet word.  This is sound because a
 * mover publishes the gate and waits a grace period BEFORE touching the
 * structure.  So a reader that samples "inactive" cannot have a move mutate
 * under it inside its own critical section.
 *
 * Otherwise: descend TWICE.  Accept only if both descents agree on the result
 * AND on the visited-node witness (struct ft_visit_witness).  This is sound
 * for two reasons:
 *
 *  - a single descent can witness a view torn across a move only by
 *    STRADDLING its commit;
 *  - two SEQUENTIAL descents cannot both straddle the same commit.
 *
 * The earlier up-walk construction got this antisymmetry from
 * descend-vs-up-walk.  Here it comes from pass1-before-pass2.  This is the
 * design's uniform choice: the relational path and the rekey writer use the
 * same primitive.
 *
 * WHY THIS REPLACED THE UP-WALK KEY WITNESS.  That witness compared the found
 * leaf's structurally rematerialized key against the descended key.
 *
 *  - It was HIT-ONLY.  A MISS has no leaf to walk up from.  So a move that
 *    tore the descent into a dead end produced a FALSE MISS with nothing to
 *    check.  Two descents cover hit and miss uniformly: the PATH is the
 *    witness, not the result.
 *  - It needed the ORDERED-LIST CELL as its up-walk source.  That is why
 *    coherence was gated on ->ordered_list.  Nothing about an in-trie move
 *    needs the cell, so that requirement is now gone.  Coherence is available
 *    on every EAGER trie, list or no list.
 *  - It read the METADATA cache line, which the forward descent does not
 *    need.
 *
 * Must run under the caller's RCU read lock, like any traversal.  Both
 * descents must be in the one critical section.  Only within it are the
 * compared addresses meaningful and the pointers valid.
 */
#ifdef FEATURE_FT_FAULT_INJECT
extern long cds_ft_fault_rekey_countdown;
#endif

/*
 * Test-only: force ONE coherence check to report a MISS.  A concurrent in-trie
 * rekey that restructured the reader's path would report exactly the same.
 * The point two-descent and the relational two-pass share this helper.  So a
 * single knob (cds_ft_fault_rekey_countdown) arms whichever check runs next.
 * Self-clearing,
 * so it cannot livelock the retry loop it exists to exercise.  Compiles to a
 * constant false without FEATURE_FT_FAULT_INJECT.
 */
static inline_lookup
bool ft_rekey_fault_miss(void)
{
#ifdef FEATURE_FT_FAULT_INJECT
	if (caa_unlikely(cds_ft_fault_rekey_countdown >= 0)) {
		if (cds_ft_fault_rekey_countdown == 0) {
			cds_ft_fault_rekey_countdown = -1;
			return true;
		}
		cds_ft_fault_rekey_countdown--;
	}
#endif
	return false;
}

/*
 * The two descents.  The SECOND (accepted) descent populates @iter.  So the
 * caller's iterator ends up exactly as a single successful descent would leave
 * it.  Retries on disagreement: an incoherent view cannot be repaired, only
 * re-taken.  This makes a reader LOCK-FREE (not wait-free) for as long as
 * moves keep restructuring the path it is reading.  That happens inside a
 * move window, and only there.
 */
static
enum cds_ft_status ft_lookup_two_descents(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, size_t key_readable_pad,
		struct cds_ft_node **result_node, struct cds_ft_iter *iter,
		bool skip_compressed)
{
	struct cds_ft_node *n1 = NULL, *n2 = NULL;
	struct ft_visit_witness w1, w2;
	enum cds_ft_status st1, st2;

	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);
	for (;;) {
		ft_witness_init(&w1);
		st1 = do_cds_ft_lookup_precise_wit(ft, key, key_len,
				key_readable_pad, &n1, iter, &w1,
				skip_compressed);
		ft_witness_init(&w2);
		st2 = do_cds_ft_lookup_precise_wit(ft, key, key_len,
				key_readable_pad, &n2, iter, &w2,
				skip_compressed);
		/*
		 * Fault injection last: it must not consume its countdown on a
		 * pair that genuinely disagreed.
		 */
		if (caa_likely(st1 == st2 && n1 == n2 &&
				ft_witness_equal(&w1, &w2) &&
				!ft_rekey_fault_miss())) {
			if (result_node)
				*result_node = n2;
			return st2;
		}
	}
}

/*
 * REKEY-coherent exact lookup specializations.  They are installed on
 * ft->lookup_key_fn in place of ft_lookup_precise_{sc,nosc} when
 * ft->rekey_coherence.  Fast mode tail-calls the ordinary specialization.  So
 * with no move in flight this costs the gate load and nothing else.
 * Otherwise they run the two descents above.  Not in the hot .text cluster.
 * They are real symbols (fn-ptr targets), so not inlined.
 */
static
enum cds_ft_status ft_lookup_precise_coherent_sc(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, size_t key_readable_pad,
		struct cds_ft_node **result_node)
{
	if (caa_likely(!ft_move_active(ft)))
		return ft_lookup_precise_sc(ft, key, key_len, key_readable_pad,
				result_node);
	key_len = ft_key_len(ft, key_len);
	if (!valid_key_len(ft, key_len))
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	CDS_FT_SCOPED_READER(ft);
	return ft_lookup_two_descents(ft, key, key_len, key_readable_pad,
			result_node, NULL, true);
}

static
enum cds_ft_status ft_lookup_precise_coherent_nosc(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, size_t key_readable_pad,
		struct cds_ft_node **result_node)
{
	if (caa_likely(!ft_move_active(ft)))
		return ft_lookup_precise_nosc(ft, key, key_len, key_readable_pad,
				result_node);
	key_len = ft_key_len(ft, key_len);
	if (!valid_key_len(ft, key_len))
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	CDS_FT_SCOPED_READER(ft);
	return ft_lookup_two_descents(ft, key, key_len, key_readable_pad,
			result_node, NULL, false);
}

#ifdef FEATURE_FT_KEY_MAP
/*
 * Non-identity key_map REKEY-coherent exact lookup.  Remap once, then run the
 * two descents.  They take ordinal bytes, like every internal descent.
 */
static
enum cds_ft_status ft_lookup_key_coherent_nonidentity(struct cds_ft *ft,
		const uint8_t *key, size_t key_len, size_t key_readable_pad,
		struct cds_ft_node **result_node)
{
	uint8_t ordinals[FT_MAX_KEY_LEN];

	if (caa_likely(!ft_move_active(ft)))
		return ft_lookup_key_nonidentity(ft, key, key_len,
				key_readable_pad, result_node);
	(void) key_readable_pad;
	key_len = ft_key_len(ft, key_len);
	if (!valid_key_len(ft, key_len))
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	CDS_FT_SCOPED_READER(ft);
	ft_key_to_ordinals(ordinals, key, key_len, &ft->group->key_map);
	return ft_lookup_two_descents(ft, ordinals, key_len,
			FT_KEY_READABLE_PAD, result_node, NULL,
			ft_group_skip_compressed(ft->group));
}
#endif /* FEATURE_FT_KEY_MAP */

enum cds_ft_status cds_ft_eager_lookup_key(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t _key_readable_pad,
		struct cds_ft_node **result_node)
{
	return (*ft->lookup_key_fn)(ft, key, _key_len,
			_key_readable_pad, result_node);
}

/*
 * cds_ft_lookup_candidate_key - Fast candidate lookup.
 *
 * Skips key comparison at compressed nodes during traversal,
 * returning a candidate node that may not be an exact match.
 * The caller MUST verify the returned node's key matches the
 * lookup key.  If it does not match, the key is not in the trie.
 *
 * This is faster than cds_ft_eager_lookup_key for workloads with long
 * compressed paths (e.g. reverse DNS, file paths).  It eliminates
 * per-node key comparisons and does a single verification at the end
 * instead.
 */
FT_LOOKUP_DISPATCH("lookup_candidate_key")
enum cds_ft_status cds_ft_lookup_candidate_key(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t _key_readable_pad,
		struct cds_ft_node **result_node)
{
	return (*ft->lookup_candidate_key_fn)(ft, key, _key_len,
			_key_readable_pad, result_node);
}

/*
 * Specialized iter-form lookup inners.  iter is always non-NULL on
 * this path: path tracking is the whole point of the iter API.  So
 * each inner bakes that into the wrapper call along with the
 * (descend_cand, skip_compressed) pair.  iter already holds
 * ordinals-mapped key bytes, so there is no non-identity branch on
 * this path.  See cds_ft_iter_set_key: non-identity key_map remapping
 * happens at iter-set time, not at lookup time.
 */
static FT_LOOKUP_FAST_PATH("lookup")
enum cds_ft_status ft_lookup_iter_precise_sc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	enum cds_ft_status status;
	CDS_FT_SCOPED_READER(ft);
	FT_TP_ITER_KEY(lookup_enter, iter);
	status = do_cds_ft_lookup_nodc_sc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_NONE, NULL, NULL);
	FT_TP(lookup_exit, (int) status);
	return status;
}

static FT_LOOKUP_SLOW_PATH("lookup")
enum cds_ft_status ft_lookup_iter_precise_nosc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	enum cds_ft_status status;
	CDS_FT_SCOPED_READER(ft);
	FT_TP_ITER_KEY(lookup_enter, iter);
	status = do_cds_ft_lookup_nodc_nosc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_NONE, NULL, NULL);
	FT_TP(lookup_exit, (int) status);
	return status;
}

/*
 * REKEY-coherent iterator EXACT lookup (installed on ft->lookup_iter_fn when
 * ft->rekey_coherence): the iter-form sibling of ft_lookup_precise_coherent_*.
 * The search key is SNAPSHOTTED first.  The descent's epilogue
 * (iter_auto_invalidate_cache, UNCACHED mode) may materialize the FOUND leaf's
 * key into iter_key(iter).  Without the snapshot, the second descent would
 * run with the wrong key.  The exact iter descent always top-descends from
 * the root: it has no cross-call cache path, unlike the inequality descent.
 * So re-calling it is a clean fresh descent.
 */
static
enum cds_ft_status ft_lookup_iter_precise_coherent_sc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	uint8_t search[FT_MAX_KEY_LEN];
	enum cds_ft_status status;
	size_t klen = iter->key_len;

	if (caa_likely(!ft_move_active(ft)))
		return ft_lookup_iter_precise_sc(ft, iter);
	CDS_FT_SCOPED_READER(ft);
	FT_TP_ITER_KEY(lookup_enter, iter);
	memcpy(search, iter_key(iter), klen);
	status = ft_lookup_two_descents(ft, search, klen, FT_KEY_READABLE_PAD,
			NULL, iter, true);
	FT_TP(lookup_exit, (int) status);
	return status;
}

static
enum cds_ft_status ft_lookup_iter_precise_coherent_nosc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	uint8_t search[FT_MAX_KEY_LEN];
	enum cds_ft_status status;
	size_t klen = iter->key_len;

	if (caa_likely(!ft_move_active(ft)))
		return ft_lookup_iter_precise_nosc(ft, iter);
	CDS_FT_SCOPED_READER(ft);
	FT_TP_ITER_KEY(lookup_enter, iter);
	memcpy(search, iter_key(iter), klen);
	status = ft_lookup_two_descents(ft, search, klen, FT_KEY_READABLE_PAD,
			NULL, iter, false);
	FT_TP(lookup_exit, (int) status);
	return status;
}

FT_LOOKUP_DISPATCH("lookup")
enum cds_ft_status cds_ft_lookup(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_iter_fn)(ft, iter);
}

/*
 * Specialized partial_key inners (tracking=PARTIAL, identity key_map).
 * Always precise descent (descend_cand=false) because partial-match
 * needs per-step compressed verification.
 */
static FT_LOOKUP_FAST_PATH("lookup_partial_key")
enum cds_ft_status ft_lookup_partial_key_sc(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *partial_node = NULL;
	size_t partial_len = 0;
	size_t key_len = ft_key_len(ft, _key_len);

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	do_cds_ft_lookup_nodc_sc(ft, key, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_PARTIAL, &partial_len, &partial_node);
	*match_len = partial_len;
	*result_node = partial_node;
	return partial_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
}

static FT_LOOKUP_SLOW_PATH("lookup_partial_key")
enum cds_ft_status ft_lookup_partial_key_nosc(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *partial_node = NULL;
	size_t partial_len = 0;
	size_t key_len = ft_key_len(ft, _key_len);

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	do_cds_ft_lookup_nodc_nosc(ft, key, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_PARTIAL, &partial_len, &partial_node);
	*match_len = partial_len;
	*result_node = partial_node;
	return partial_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
}

/*
 * Non-identity key_map fallback for partial_key.  Routes through
 * the generic do_cds_ft_lookup dispatcher because non-identity is
 * rare; ordinals[] is allocated on stack inside this fn so
 * call/return overhead is acceptable.
 */
#ifdef FEATURE_FT_KEY_MAP
static FT_LOOKUP_SLOW_PATH("lookup_partial_key")
enum cds_ft_status ft_lookup_partial_key_nonidentity(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *partial_node = NULL;
	size_t partial_len = 0;
	size_t key_len = ft_key_len(ft, _key_len);
	uint8_t ordinals[FT_MAX_KEY_LEN];

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	ft_key_to_ordinals(ordinals, key, key_len, &ft->group->key_map);
	do_cds_ft_lookup(ft, ordinals, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_PARTIAL, &partial_len, &partial_node,
			false);
	*match_len = partial_len;
	*result_node = partial_node;
	return partial_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
}
#endif /* FEATURE_FT_KEY_MAP */

FT_LOOKUP_DISPATCH("lookup_partial_key")
enum cds_ft_status cds_ft_lookup_partial_key(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	return (*ft->lookup_partial_key_fn)(ft, key, _key_len, match_len,
			result_node);
}

/*
 * Specialized partial (iter form) inners.  Same as lookup_iter_*
 * but with tracking=PARTIAL and the post-descent iter override.
 */
static FT_LOOKUP_FAST_PATH("lookup_partial")
enum cds_ft_status ft_lookup_partial_iter_sc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	struct cds_ft_node *partial_node = NULL;
	size_t partial_len = 0;

	CDS_FT_SCOPED_READER(ft);
	do_cds_ft_lookup_nodc_sc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_PARTIAL, &partial_len, &partial_node);
	iter->node = partial_node;
	iter->key_len = partial_len;
	iter->path_len = partial_len + 1;
	iter->status = partial_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
	return iter->status;
}

static FT_LOOKUP_SLOW_PATH("lookup_partial")
enum cds_ft_status ft_lookup_partial_iter_nosc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	struct cds_ft_node *partial_node = NULL;
	size_t partial_len = 0;

	CDS_FT_SCOPED_READER(ft);
	do_cds_ft_lookup_nodc_nosc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_PARTIAL, &partial_len, &partial_node);
	iter->node = partial_node;
	iter->key_len = partial_len;
	iter->path_len = partial_len + 1;
	iter->status = partial_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_NOT_FOUND;
	return iter->status;
}

FT_LOOKUP_DISPATCH("lookup_partial")
enum cds_ft_status cds_ft_lookup_partial(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_partial_iter_fn)(ft, iter);
}

/*
 * Helper: derive the public-API return value for longest_match
 * from the descent result.  Sets *match_len / *result_node and
 * returns CDS_FT_STATUS_OK / INTERNAL_MATCH, NOT_FOUND for an empty
 * trie, or a negative status when the descent itself failed.
 */
static inline
enum cds_ft_status ft_lookup_longest_match_key_finish(
		enum cds_ft_status ret,
		size_t longest_len, struct cds_ft_node *match_node,
		size_t *match_len, struct cds_ft_node **result_node)
{
	if (ret < 0) {
		*match_len = 0;
		*result_node = NULL;
		return ret;
	}
	if (longest_len == FT_MATCH_LEN_NONE) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_NOT_FOUND;
	}
	*match_len = longest_len;
	*result_node = match_node;
	return match_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_INTERNAL_MATCH;
}

static FT_LOOKUP_FAST_PATH("lookup_longest_match_key")
enum cds_ft_status ft_lookup_longest_match_key_sc(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *match_node = NULL;
	size_t longest_len = 0;
	enum cds_ft_status ret;
	size_t key_len = ft_key_len(ft, _key_len);

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	ret = do_cds_ft_lookup_nodc_sc(ft, key, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_LONGEST, &longest_len, &match_node);
	return ft_lookup_longest_match_key_finish(ret, longest_len,
			match_node, match_len, result_node);
}

static FT_LOOKUP_SLOW_PATH("lookup_longest_match_key")
enum cds_ft_status ft_lookup_longest_match_key_nosc(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *match_node = NULL;
	size_t longest_len = 0;
	enum cds_ft_status ret;
	size_t key_len = ft_key_len(ft, _key_len);

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	ret = do_cds_ft_lookup_nodc_nosc(ft, key, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_LONGEST, &longest_len, &match_node);
	return ft_lookup_longest_match_key_finish(ret, longest_len,
			match_node, match_len, result_node);
}

#ifdef FEATURE_FT_KEY_MAP
static FT_LOOKUP_SLOW_PATH("lookup_longest_match_key")
enum cds_ft_status ft_lookup_longest_match_key_nonidentity(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	struct cds_ft_node *match_node = NULL;
	size_t longest_len = 0;
	enum cds_ft_status ret;
	size_t key_len = ft_key_len(ft, _key_len);
	uint8_t ordinals[FT_MAX_KEY_LEN];

	if (!valid_key_len(ft, key_len)) {
		*match_len = 0;
		*result_node = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	CDS_FT_SCOPED_READER(ft);
	ft_key_to_ordinals(ordinals, key, key_len, &ft->group->key_map);
	ret = do_cds_ft_lookup(ft, ordinals, key_len, 0, NULL, NULL,
			FT_PREFIX_TRACK_LONGEST, &longest_len, &match_node, false);
	return ft_lookup_longest_match_key_finish(ret, longest_len,
			match_node, match_len, result_node);
}
#endif /* FEATURE_FT_KEY_MAP */

FT_LOOKUP_DISPATCH("lookup_longest_match_key")
enum cds_ft_status cds_ft_lookup_longest_match_key(struct cds_ft *ft,
		const uint8_t *key, size_t _key_len, size_t *match_len,
		struct cds_ft_node **result_node)
{
	return (*ft->lookup_longest_match_key_fn)(ft, key, _key_len,
			match_len, result_node);
}

/*
 * Helper: write descent result back into iter for longest_match iter.
 */
static inline
enum cds_ft_status ft_lookup_longest_match_iter_finish(
		enum cds_ft_status ret,
		size_t longest_len, struct cds_ft_node *match_node,
		struct cds_ft_iter *iter)
{
	if (ret < 0) {
		iter->node = NULL;
		iter->status = ret;
		goto end;
	}
	if (longest_len == FT_MATCH_LEN_NONE) {
		iter->node = NULL;
		iter->status = CDS_FT_STATUS_NOT_FOUND;
		goto end;
	}
	iter->node = match_node;
	iter->key_len = longest_len;
	iter->path_len = longest_len + 1;
	iter->status = match_node ? CDS_FT_STATUS_OK : CDS_FT_STATUS_INTERNAL_MATCH;
	iter->cache_valid = true;
	iter_debug_path_snapshot(iter);
end:
	iter_auto_invalidate_cache(iter);
	return iter->status;
}

static FT_LOOKUP_FAST_PATH("lookup_longest_match")
enum cds_ft_status ft_lookup_longest_match_iter_sc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	struct cds_ft_node *match_node = NULL;
	size_t longest_len = 0;
	enum cds_ft_status ret;

	CDS_FT_SCOPED_READER(ft);
	ret = do_cds_ft_lookup_nodc_sc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_LONGEST, &longest_len, &match_node);
	return ft_lookup_longest_match_iter_finish(ret, longest_len,
			match_node, iter);
}

static FT_LOOKUP_SLOW_PATH("lookup_longest_match")
enum cds_ft_status ft_lookup_longest_match_iter_nosc(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	struct cds_ft_node *match_node = NULL;
	size_t longest_len = 0;
	enum cds_ft_status ret;

	CDS_FT_SCOPED_READER(ft);
	ret = do_cds_ft_lookup_nodc_nosc(ft, iter_key(iter), iter->key_len,
			FT_KEY_READABLE_PAD, NULL, iter,
			FT_PREFIX_TRACK_LONGEST, &longest_len, &match_node);
	return ft_lookup_longest_match_iter_finish(ret, longest_len,
			match_node, iter);
}

FT_LOOKUP_DISPATCH("lookup_longest_match")
enum cds_ft_status cds_ft_lookup_longest_match(struct cds_ft *ft,
		struct cds_ft_iter *iter)
{
	ft_iter_assert_bound(ft, iter);
	return (*ft->lookup_longest_match_iter_fn)(ft, iter);
}
