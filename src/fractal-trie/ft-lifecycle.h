// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

/*
 * src/fractal-trie/ft-lifecycle.h
 *
 * Userspace RCU library - Fractal Trie: group / trie / attr create +
 * destroy, group accessors, verify-at-mutation knobs.
 *
 * Implementation unit: #included once by fractal-trie.c, in dependency
 * order, into a single translation unit (preserves cross-module inlining).
 * Not a standalone header.
 */
#ifndef FRACTAL_TRIE_IMPL
#error "ft-lifecycle.h is an implementation unit; #include it from fractal-trie.c only"
#endif

unsigned long cds_ft_count_entries(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;
	enum cds_ft_status status;
	unsigned long count = 0;

	CDS_FT_SCOPED_READER(ft);
	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	status = cds_ft_iter_create(ft, &iter);
	if (status != CDS_FT_STATUS_OK) {
		FT_TP(count_entries, (unsigned long) 0);
		return 0;
	}
	cds_ft_for_each_rcu(ft, iter) {
		struct cds_ft_node *node = cds_ft_iter_node(iter);

		cds_ft_for_each_duplicate_rcu(node)
			count++;
	}
	if (cds_ft_iter_status(iter) < 0)
		count = 0;
	cds_ft_iter_destroy(iter);
	FT_TP(count_entries, count);
	return count;
}

/*
 * Validate that the pointer bits used by the skip-compressed encoding
 * are outside the process's virtual address range.  Return true when
 * the bit is available.
 *
 * Try to mmap a page at the encoding boundary:
 *   - The mapping succeeds, or fails with EEXIST: the bit is within the
 *     VA range.  Skip-compressed cannot be used safely.
 *   - It fails with ENOMEM (address beyond TASK_SIZE): the bit is
 *     available.  A positive control must first rule out a spurious
 *     ENOMEM from a global mmap failure (see below).
 *   - Any other failure (EPERM, EINVAL, EAGAIN, seccomp, ...): treat
 *     the bit as unavailable, to stay conservative.
 */
#ifdef FEATURE_FT_SKIP_COMPRESSED
static
bool ft_skip_compressed_validate(void)
{
	size_t page = urcu_get_page_len();
	void *p, *ctrl;

	p = mmap((void *)(1UL << FT_SKIP_LEN_SHIFT), page, PROT_NONE,
		 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
	if (p != MAP_FAILED) {
		/* Mapping succeeded: the bit is within the VA range. */
		munmap(p, page);
		return false;
	}
	if (errno != ENOMEM)
		return false;	/* EEXIST / EPERM / EINVAL / seccomp / ... */
	/*
	 * ENOMEM is overloaded.  It means "address beyond TASK_SIZE", which
	 * is what we want to confirm.  It is also returned for a global mmap
	 * failure, regardless of the address.  The notable case is
	 * vm.max_map_count exhaustion.
	 *
	 * Cross-check with a positive control: map one page at a
	 * kernel-chosen address.
	 *   - The control succeeds: mmap is healthy.  The boundary ENOMEM
	 *     was a genuine out-of-range rejection, so the bit is available.
	 *   - The control also fails: mmap is failing globally.  The
	 *     boundary ENOMEM proves nothing.  Stay conservative and leave
	 *     skip-compressed disabled.
	 */
	ctrl = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (ctrl == MAP_FAILED)
		return false;
	munmap(ctrl, page);
	return true;
}
#endif

enum cds_ft_status cds_ft_group_attr_create(struct cds_ft_group_attr **result)
{
	struct cds_ft_group_attr *attr = calloc(1, sizeof(struct cds_ft_group_attr));

	if (!attr) {
		*result = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	attr->key_len = CDS_FT_LEN_DEFAULT;
	attr->max_key_len = FT_MAX_KEY_LEN;
	attr->key_map.identity = true;
	/*
	 * Default lookup optimization is SPECULATIVE: speculative
	 * descent with validate-and-retry, plus opportunistic
	 * skip-compressed pointer encoding on supported archs.
	 * Callers that need strict EAGER (per-step exact compare, no
	 * skip-compressed) must call
	 * cds_ft_group_attr_set_lookup_optimization(attr,
	 * CDS_FT_LOOKUP_OPTIMIZE_EAGER) explicitly.
	 */
	attr->speculative = true;
#ifdef FEATURE_FT_SKIP_COMPRESSED
	if (ft_skip_compressed_validate())
		attr->flags |= CDS_FT_FLAG_SKIP_COMPRESSED;
#endif
	/*
	 * The ordered sibling list is ON by default.  The library-owned cell
	 * is allocated per head regardless.  So the key-ordered fast paths
	 * are available out of the box: cds_ft_next, cds_ft_prev and
	 * cds_ft_for_each*_batched.  A group that never iterates in key
	 * order may want to skip the per-mutation splice / unsplice.  It
	 * opts out with cds_ft_group_attr_set_ordered_list(attr, false).
	 */
	attr->ordered_list_set = true;
	/*
	 * Order-statistics (per-node nr_keys) default OFF: calloc already left
	 * rank_stats_set false.  A group opts in with
	 * cds_ft_group_attr_set_rank_stats(attr, true).  It then gets:
	 *   - O(1) cds_ft_count_keys;
	 *   - O(depth) cds_ft_lookup_nth and cds_ft_iter_skip_*.
	 * Otherwise those queries fall back to enumeration / iteration.
	 */
	/*
	 * NUMA placement default: CDS_FT_NUMA_DEFAULT, which defers to an
	 * explicit process / libnuma policy (see ft_apply_interleave).
	 *   - The process policy is MPOL_BIND, PREFERRED or LOCAL (numactl
	 *     wrappers, set_mempolicy() calls): the library applies no
	 *     mbind() of its own.  The kernel honors the process policy.
	 *   - The process policy is MPOL_INTERLEAVE, or no process policy
	 *     is set: the library interleaves its own arenas.
	 * Applications that want explicit library-side placement should opt
	 * in via cds_ft_group_attr_set_numa_policy:
	 *   - CDS_FT_NUMA_INTERLEAVE for multi-reader workloads;
	 *   - CDS_FT_NUMA_LOCAL for single-threaded / sharded workloads.
	 */
	attr->numa_policy = CDS_FT_NUMA_DEFAULT;
	attr->optimize = CDS_FT_OPTIMIZE_THROUGHPUT;
	*result = attr;
	return CDS_FT_STATUS_OK;
}

void cds_ft_group_attr_destroy(struct cds_ft_group_attr *attr)
{
	free(attr);
}

enum cds_ft_status cds_ft_group_attr_set_key_len(struct cds_ft_group_attr *attr, size_t key_len)
{
	attr->key_len = key_len;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_max_key_len(struct cds_ft_group_attr *attr, size_t max_key_len)
{
	if (max_key_len == CDS_FT_MAX_LEN_UNLIMITED) {
		attr->max_key_len = FT_MAX_KEY_LEN;
		return CDS_FT_STATUS_OK;
	}
	if (max_key_len > FT_MAX_KEY_LEN)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->max_key_len = max_key_len;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_key_map(struct cds_ft_group_attr *attr,
		const uint8_t *key_to_ordinal, const uint8_t *ordinal_to_key)
{
#ifndef FEATURE_FT_KEY_MAP
	/* Built without non-identity key-map support (byte-ordinal keys only). */
	(void) attr;
	(void) key_to_ordinal;
	(void) ordinal_to_key;
	return CDS_FT_STATUS_NOT_SUPPORTED;
#else
	size_t i;

	if (!key_to_ordinal || !ordinal_to_key)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	/*
	 * Verify the two maps are exact inverse permutations.  Checking
	 * ordinal_to_key[key_to_ordinal[i]] == i for every i is sufficient:
	 *   - it forces key_to_ordinal to be injective, hence a bijection
	 *     over the byte values;
	 *   - it forces ordinal_to_key to be its inverse, hence also a
	 *     bijection.
	 * So both maps are valid permutations.
	 */
	for (i = 0; i < sizeof(attr->key_map.key_to_ordinal); i++) {
		if (ordinal_to_key[key_to_ordinal[i]] != i)
			return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	attr->key_map.identity = false;
	memcpy(attr->key_map.key_to_ordinal, key_to_ordinal, sizeof(attr->key_map.key_to_ordinal));
	memcpy(attr->key_map.ordinal_to_key, ordinal_to_key, sizeof(attr->key_map.ordinal_to_key));
	return CDS_FT_STATUS_OK;
#endif /* FEATURE_FT_KEY_MAP */
}

enum cds_ft_status cds_ft_group_attr_set_lookup_optimization(
		struct cds_ft_group_attr *attr,
		enum cds_ft_lookup_optimization opt)
{
	switch (opt) {
	case CDS_FT_LOOKUP_OPTIMIZE_EAGER:
		attr->speculative = false;
		attr->flags &= ~CDS_FT_FLAG_SKIP_COMPRESSED;
		return CDS_FT_STATUS_OK;
	case CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE:
		/*
		 * Speculative descent always works.  Opportunistically
		 * enable the skip-compressed pointer encoding when the
		 * arch supports it.  On archs without it, the descent
		 * still skips per-step byte compares.  It only lacks the
		 * extra compressed-CL bypass.
		 */
		attr->speculative = true;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_skip_compressed_validate())
			attr->flags |= CDS_FT_FLAG_SKIP_COMPRESSED;
#endif
		return CDS_FT_STATUS_OK;
	}
	return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
}

enum cds_ft_status cds_ft_group_attr_set_speculative_key_offset(
		struct cds_ft_group_attr *attr,
		size_t key_offset)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->speculative_key_offset = key_offset;
	attr->speculative_key_offset_set = true;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_key_len_offset(
		struct cds_ft_group_attr *attr,
		size_t offset)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->key_len_offset = offset;
	attr->key_len_offset_set = true;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_ordered_list(
		struct cds_ft_group_attr *attr, bool ordered_list)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->ordered_list_set = ordered_list;
	return CDS_FT_STATUS_OK;
}

/*
 * ☞ REKEY IS OPT-IN, and the default is OFF.  (MATHIEU, 2026-09-20.)
 *
 * There are two reasons.  The second is a correctness one.
 *
 * 1. A move can start at any time.  So a trie that can host an in-trie MOVE
 *    has to carry the COHERENT lookup specializations.  With them, every
 *    exact lookup is able to run a second, key-rematerializing walk.  A
 *    trie that will never rekey pays for a capability it never uses.  This
 *    flag is what lets ft_install_lookup_ops select the plain variants
 *    instead.
 *
 * 2. Under CDS_FT_WRITER_EXTERNAL_SYNC the move gate's grace period is taken
 *    while the APPLICATION holds its own writer exclusion.  The library can
 *    neither drop that exclusion nor get outside of it.  Opting IN there
 *    turns on @rekey_always_coherent, which removes that grace period
 *    entirely.  So the opt-in is also the place where the library decides
 *    how to serve the request.  See cds_ft::rekey_always_coherent.
 *
 * ☠ A REFUSAL IS A CLAIM ABOUT THE TRIE, not a return code.  A caller that
 * does not opt in gets CDS_FT_STATUS_NOT_SUPPORTED from the rekey entries,
 * NOT a silent no-op.  Tests that exercise rekey must opt in.  Both suites
 * carry a positive control that FAILS when no rekey actually ran.  Without
 * it, a missed creator reads as a green suite with zero rekey coverage.
 */
enum cds_ft_status cds_ft_group_attr_set_rekey(
		struct cds_ft_group_attr *attr, bool rekey)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->rekey_set = rekey;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_rank_stats(
		struct cds_ft_group_attr *attr, bool rank_stats)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->rank_stats_set = rank_stats;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_attr_set_numa_policy(
		struct cds_ft_group_attr *attr,
		enum cds_ft_numa_policy policy)
{
	switch (policy) {
	case CDS_FT_NUMA_DEFAULT:
	case CDS_FT_NUMA_INTERLEAVE:
	case CDS_FT_NUMA_LOCAL:
		attr->numa_policy = policy;
		return CDS_FT_STATUS_OK;
	}
	return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
}

enum cds_ft_status cds_ft_group_attr_set_optimize(
		struct cds_ft_group_attr *attr,
		enum cds_ft_optimize opt)
{
	switch (opt) {
	case CDS_FT_OPTIMIZE_THROUGHPUT:
	case CDS_FT_OPTIMIZE_RSS:
		attr->optimize = opt;
		return CDS_FT_STATUS_OK;
	}
	return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
}

enum cds_ft_status cds_ft_group_attr_set_writer_strategy(
		struct cds_ft_group_attr *attr,
		enum cds_ft_writer_strategy strategy)
{
	switch (strategy) {
	case CDS_FT_WRITER_LOCK_COARSE:
	case CDS_FT_WRITER_LOCK_FINE:
	case CDS_FT_WRITER_EXTERNAL_SYNC:
		attr->writer_strategy = strategy;
		attr->writer_strategy_set = true;
		return CDS_FT_STATUS_OK;
	}
	return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
}

enum cds_ft_status cds_ft_attr_create(struct cds_ft_attr **result)
{
	struct cds_ft_attr *attr = calloc(1, sizeof(struct cds_ft_attr));

	if (!attr) {
		*result = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	/*
	 * The per-instance attr carries two flags.  calloc leaves both
	 * false, which is the default:
	 *   - @exclusive false: concurrent RCU readers are permitted (see
	 *     cds_ft_attr_set_exclusive);
	 *   - @speculative_keys_disabled false: the trie inherits the
	 *     group's setting (see cds_ft_attr_set_speculative_keys).
	 */
	*result = attr;
	return CDS_FT_STATUS_OK;
}

void cds_ft_attr_destroy(struct cds_ft_attr *attr)
{
	free(attr);
}

enum cds_ft_status cds_ft_attr_set_exclusive(struct cds_ft_attr *attr,
		bool exclusive)
{
	attr->exclusive = exclusive;
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_attr_set_speculative_keys(struct cds_ft_attr *attr,
		bool enabled)
{
	if (!attr)
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	attr->speculative_keys_disabled = !enabled;
	return CDS_FT_STATUS_OK;
}

void cds_ft_make_exclusive(struct cds_ft *ft)
{
	CDS_FT_SCOPED_WRITER(ft);
	if (ft->exclusive)
		return;
	ft_writer_lock_gp_wait(ft);
	ft->exclusive = true;
}

void cds_ft_make_shared(struct cds_ft *ft)
{
	CDS_FT_SCOPED_WRITER(ft);
	ft->exclusive = false;
}

bool cds_ft_is_exclusive(const struct cds_ft *ft)
{
	return ft->exclusive;
}

bool cds_ft_excl_validate_enabled(void)
{
#ifdef FEATURE_FT_EXCL_VALIDATE
	return true;
#else
	return false;
#endif
}

bool cds_ft_merge_enabled(void)
{
#ifdef NO_FEATURE_FT_MERGE
	return false;
#else
	return true;
#endif
}

bool cds_ft_verify_at_mutation_enabled(void)
{
#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	return true;
#else
	return false;
#endif
}

/*
 * Set the verify-at-mutation sampling period for @ft.  When the
 * library is built with -DFEATURE_FT_VERIFY_AT_MUTATION, the writer
 * scope-exit hook runs cds_ft_verify once every @period mutations.
 *
 *   period == 0 : disable the verify walk on this trie (the
 *                 increment-and-compare still runs in the hook).
 *   period == 1 : verify at every mutation (the historical
 *                 -DFEATURE_FT_VERIFY_AT_MUTATION cadence).
 *   period >  1 : verify every @period mutations -- useful on large
 *                 tries where O(N) per mutation is impractical.
 *
 * The counter is reset to 0 on each period boundary.  So it never
 * exceeds @period - 1.  Long-running workloads have no overflow and
 * no cadence drift to worry about.
 *
 * Returns CDS_FT_STATUS_OK on success.  Returns
 * CDS_FT_STATUS_NOT_SUPPORTED if the library was built without
 * FEATURE_FT_VERIFY_AT_MUTATION.  The call reports the mismatch
 * loudly rather than silently doing nothing.  So a test that relies
 * on the verify cadence cannot accidentally run with
 * verify-at-mutation compiled out.  Use
 * cds_ft_verify_at_mutation_enabled() to gate the call.
 *
 * Write-side only (mutex-held); not safe to call concurrently
 * with writers on the same trie.
 */
enum cds_ft_status cds_ft_verify_at_mutation_period_set(struct cds_ft *ft,
		unsigned long period)
{
#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	ft->verify_at_mutation_period = period;
	ft->verify_at_mutation_counter = 0;
	return CDS_FT_STATUS_OK;
#else
	(void) ft;
	(void) period;
	return CDS_FT_STATUS_NOT_SUPPORTED;
#endif
}

/*
 * Read the verify-at-mutation sampling period for @ft into
 * *@period.
 *
 * Returns CDS_FT_STATUS_OK on success.  Returns
 * CDS_FT_STATUS_NOT_SUPPORTED if the library was built without
 * FEATURE_FT_VERIFY_AT_MUTATION.  This distinguishes the
 * build-disabled case from a runtime period == 0.
 */
enum cds_ft_status cds_ft_verify_at_mutation_period_get(
		const struct cds_ft *ft, unsigned long *period)
{
#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	*period = ft->verify_at_mutation_period;
	return CDS_FT_STATUS_OK;
#else
	(void) ft;
	(void) period;
	return CDS_FT_STATUS_NOT_SUPPORTED;
#endif
}

enum cds_ft_status _cds_ft_group_create(const struct cds_ft_group_attr *attr,
		struct cds_ft_group **result_ft_group,
		const struct rcu_flavor_struct *flavor)
{
	struct cds_ft_group *ft_group;
	size_t key_len = CDS_FT_LEN_DEFAULT,
	       max_key_len = FT_MAX_KEY_LEN;

	ft_specialized_scan_layout_assert();
	if (attr) {
		key_len = attr->key_len;
		max_key_len = attr->max_key_len;
	}
	/* max_tree_depth 0 is for pointer to root node */
	if (key_len != CDS_FT_LEN_VARIABLE && key_len > max_key_len) {
		*result_ft_group = NULL;
		return CDS_FT_STATUS_INVALID_ARGUMENT_ERROR;
	}
	ft_group = calloc(1, sizeof(*ft_group));
	if (!ft_group) {
		*result_ft_group = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	ft_group->key_len = key_len;
	ft_group->max_key_len = max_key_len;
	ft_group->max_tree_depth = max_key_len + 1;
	assert(ft_group->max_tree_depth <= FT_MAX_DEPTH);
	ft_group->flavor = flavor;
	pthread_mutex_init(&ft_group->arena_lock, NULL);
	if (attr) {
		ft_group->key_map = attr->key_map;
		ft_group->flags = attr->flags;
		ft_group->speculative = attr->speculative;
		ft_group->speculative_key_offset = attr->speculative_key_offset;
		ft_group->speculative_key_offset_set = attr->speculative_key_offset_set;
		ft_group->key_len_offset = attr->key_len_offset;
		ft_group->key_len_offset_set = attr->key_len_offset_set;
		ft_group->ordered_list_set = attr->ordered_list_set;
		ft_group->rekey_set = attr->rekey_set;
		ft_group->rank_stats_set = attr->rank_stats_set;
		ft_group->numa_policy = attr->numa_policy;
		ft_group->optimize = attr->optimize;
		/*
		 * DLM (per-node lock-sets) is the default.  calloc-zero is not a
		 * valid strategy any more: the enumerator that occupied 0 is
		 * gone.  Hence @writer_strategy_set.  Unset means "no choice
		 * made", which resolves to LOCK_FINE.
		 */
		ft_group->writer_strategy = attr->writer_strategy_set ?
			attr->writer_strategy : CDS_FT_WRITER_LOCK_FINE;
		/*
		 * Order statistics maintain ONE global count on the root's
		 * nr_keys word.  EVERY count-changing mutation walks up to the
		 * root and updates it (ft_flip_txn_record_count_parent).  So no
		 * two count-changing writers are ever disjoint.
		 *
		 * FINE exploits disjointness it does not have here.  Its
		 * per-node lock-sets let would-be-disjoint writers proceed
		 * without the FT-wide lock.  But with rank stats all writers
		 * collide on the root count, so FINE buys no concurrency over
		 * the single lock.
		 *
		 * Worse, the count-parent walk assumes WRITER EXCLUSION for a
		 * stable, proxy-free ancestor chain
		 * (ft_flip_txn_record_count_parent).  FINE breaks that
		 * assumption under the FT-wide-lock drop.
		 *
		 * A rank-stats trie is therefore the COARSE single-lock target
		 * (§10.5).  Coerce the strategy to COARSE so the trie keeps the
		 * FT-wide lock (writer exclusion).
		 */
		/*
		 * ☞ ONLY *FINE* IS COERCED.  COARSE is already the target.
		 * CDS_FT_WRITER_EXTERNAL_SYNC already serializes every
		 * structural writer, by the caller's contract rather than by
		 * the FT-wide mutex.  So the count walk has the exclusion it
		 * requires.  Coercing EXTERNAL_SYNC would silently hand back a
		 * lock the caller explicitly asked the library not to take.
		 */
		if (ft_group->rank_stats_set
				&& ft_group->writer_strategy
					== CDS_FT_WRITER_LOCK_FINE)
			ft_group->writer_strategy = CDS_FT_WRITER_LOCK_COARSE;
	} else {
		/*
		 * NULL attr: mirror the defaults set by
		 * cds_ft_group_attr_create:
		 *   - identity key map;
		 *   - SPECULATIVE lookup optimization, with opportunistic
		 *     SKIP_COMPRESSED on supported archs;
		 *   - the DEFAULT NUMA policy: interleave unless the process
		 *     set an explicit preference (see ft_apply_interleave).
		 */
		ft_group->key_map.identity = true;
		ft_group->speculative = true;
#ifdef FEATURE_FT_SKIP_COMPRESSED
		if (ft_skip_compressed_validate())
			ft_group->flags |= CDS_FT_FLAG_SKIP_COMPRESSED;
#endif
		ft_group->ordered_list_set = true;	/* on by default; see attr_create */
		ft_group->rekey_set = false;		/* OFF by default: opt-in */
		ft_group->numa_policy = CDS_FT_NUMA_DEFAULT;
		ft_group->optimize = CDS_FT_OPTIMIZE_THROUGHPUT;
		ft_group->writer_strategy = CDS_FT_WRITER_LOCK_FINE;	/* DLM default */
	}
	*result_ft_group = ft_group;
	FT_TP(group_create, (const void *) ft_group);
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_group_destroy(struct cds_ft_group *ft_group)
{
	if (uatomic_load(&ft_group->nr_ft_instances, CMM_RELAXED) != 0)
		return CDS_FT_STATUS_BUSY_ERROR;
	FT_TP(group_destroy, (const void *) ft_group);
	/*
	 * All tries are destroyed (nr_ft_instances == 0), and each
	 * cds_ft_destroy drained its deferred frees.  So every allocated
	 * ordered-list cell and every allocated internal/compressed node
	 * should have had its free issued.  Both balances are group-scoped
	 * because the bulk ops migrate cells and nodes between the group's
	 * tries.
	 */
	if (ft_debug_counters() &&
	    ft_group->nr_cells_allocated != ft_group->nr_cells_freed) {
		fprintf(stderr,
			"[error] Fractal Trie leaked %ld ordered-list cells. Allocated: %lu, freed: %lu.\n",
			(long) (ft_group->nr_cells_allocated - ft_group->nr_cells_freed),
			ft_group->nr_cells_allocated, ft_group->nr_cells_freed);
	}
	if (ft_debug_counters() &&
	    ft_group->nr_nodes_allocated != ft_group->nr_nodes_freed) {
		fprintf(stderr, "[error] Fractal Trie leaked %ld nodes. Allocated: %lu, freed: %lu.\n",
			(long) (ft_group->nr_nodes_allocated - ft_group->nr_nodes_freed),
			ft_group->nr_nodes_allocated, ft_group->nr_nodes_freed);
		fprintf(stderr, "  internal: alloc=%lu freed=%lu leaked=%ld\n",
			ft_group->nr_internal_alloc, ft_group->nr_internal_freed,
			(long) (ft_group->nr_internal_alloc - ft_group->nr_internal_freed));
		fprintf(stderr, "  compressed: alloc=%lu freed=%lu leaked=%ld\n",
			ft_group->nr_compressed_alloc, ft_group->nr_compressed_freed,
			(long) (ft_group->nr_compressed_alloc - ft_group->nr_compressed_freed));
	}
	cds_ft_free_all_arenas(ft_group);
	pthread_mutex_destroy(&ft_group->arena_lock);
	free(ft_group);
	return CDS_FT_STATUS_OK;
}

#ifdef DEBUG_COUNTERS
/*
 * DEBUG_COUNTERS-only cell leak introspection (no public header decl; tests
 * weak-reference it).  Reports the group's ordered-list cell alloc / free
 * balance.
 *
 * Cells migrate between the group's tries via the bulk ops.  So the balance
 * is meaningful only at group granularity.  After every trie is drained,
 * @allocated should equal @freed.
 *
 * Caller must ensure no concurrent cell mutation.
 */
void cds_ft_debug_cell_balance(const struct cds_ft_group *group,
		unsigned long *allocated, unsigned long *freed)
{
	if (allocated)
		*allocated = group->nr_cells_allocated;
	if (freed)
		*freed = group->nr_cells_freed;
}

/*
 * DEBUG_COUNTERS-only node leak introspection (no public header decl; tests
 * weak-reference it).  Reports the group's internal/compressed node alloc /
 * free balance.
 *
 * Nodes migrate between the group's tries via the bulk ops.  So the balance
 * is meaningful only at group granularity.  After every trie is drained,
 * @allocated should equal @freed.
 *
 * Caller must ensure no concurrent node mutation.
 */
void cds_ft_debug_node_balance(const struct cds_ft_group *group,
		unsigned long *allocated, unsigned long *freed)
{
	if (allocated)
		*allocated = group->nr_nodes_allocated;
	if (freed)
		*freed = group->nr_nodes_freed;
}

/*
 * Report how many up-walks started on a duplicate-chain MEMBER and hopped to
 * its head (ft_ext_head_word).
 *
 * This is the positive control for the same-parent splice oracle.  A run of
 * the oracle's merge flavours that never took that branch has not exercised
 * what it claims to.  Test-only, no public header declaration.
 */
void cds_ft_debug_ext_member_hops(const struct cds_ft_group *group,
		unsigned long *hops)
{
	if (hops)
		*hops = group->nr_ext_member_hops;
}
#endif

/*
 * Root-is-internal probe (no public header decl; the inv suite weak-references
 * it).  Reads ft->root under the caller's RCU read lock and returns whether it
 * tags a plain internal node.
 *
 * The inv suite uses it to sample the root CONCURRENTLY with a re-rooting
 * bulk op (graft_swap).  No mutator should ever publish a
 * transient compressed / skip-compressed root.  If one did, it would show up
 * here.  A post-op probe cannot see it, because it runs after
 * canonicalization.
 */
int cds_ft_debug_root_is_internal(struct cds_ft *ft);
int cds_ft_debug_root_is_internal(struct cds_ft *ft)
{
	/*
	 * Resolve a flip proxy.  The empty-dst root-level graft fuses the root
	 * swap with the ordered-list head/tail transfer in one flip.  That
	 * parks a transient proxy here.  The proxy resolves to an internal
	 * node in BOTH phases (old empty root XOR new src root).  So the
	 * root-always-internal invariant this probe checks still holds
	 * through the flip.
	 */
	struct cds_ft_inode_flag *rf = ft_root_dereference(ft);

	return ft_node_internal(rf) ? 1 : 0;
}

/*
 * Install per-API lookup function pointers on @ft based on group
 * flags.  Called once at cds_ft_create.  Pointers are stable for
 * the trie's lifetime because the group flags (key_map.identity,
 * CDS_FT_FLAG_SKIP_COMPRESSED) are immutable after group creation.
 */
static
void ft_install_lookup_ops(struct cds_ft *ft)
{
	const struct cds_ft_group *group = ft->group;
	bool sc = ft_group_skip_compressed(group);
	/*
	 * Limit-none relational entries.  Resolve the immutable use_keycopy
	 * config once and install the matching specialization.  Then
	 * cds_ft_lookup_le/ge/lt/gt (and cds_ft_next/prev) tail-call through
	 * @ft with no per-call config branch.
	 *
	 * This is independent of key_map.identity: ft_speculative_keycopy
	 * handles non-identity via ft_key_to_ordinals.  So it is installed
	 * before the early non-identity return below.
	 */
	bool kc = ft->speculative_key_offset_active && group->speculative &&
			(group->flags & CDS_FT_FLAG_SKIP_COMPRESSED);

	/*
	 * REKEY-coherent relational entries: the two-pass (visited-node witness)
	 * variants.  They fall through to the plain specializations below
	 * whenever no move is in flight.  @kc is necessarily false here,
	 * because rekey_coherence implies !speculative_key_offset_active.  So
	 * only the EAGER siblings exist.
	 */
	if (ft->rekey_coherence) {
		assert(!kc);
		ft->lookup_le_fn = ft_ineq_le_coherent;
		ft->lookup_ge_fn = ft_ineq_ge_coherent;
		ft->lookup_lt_fn = ft_ineq_lt_coherent;
		ft->lookup_gt_fn = ft_ineq_gt_coherent;
	} else {
		ft->lookup_le_fn = kc ? ft_ineq_le_keycopy : ft_ineq_le_eager;
		ft->lookup_ge_fn = kc ? ft_ineq_ge_keycopy : ft_ineq_ge_eager;
		ft->lookup_lt_fn = kc ? ft_ineq_lt_keycopy : ft_ineq_lt_eager;
		ft->lookup_gt_fn = kc ? ft_ineq_gt_keycopy : ft_ineq_gt_eager;
	}

	/*
	 * Iter-form lookup_iter_fn.  The iter always carries ordinals-mapped
	 * key bytes (see cds_ft_iter_set_key).  So the non-identity key_map
	 * case is handled at iter-set time.  The lookup-time inner only
	 * needs (skip_compressed) specialization.
	 */
	/*
	 * REKEY-coherent iterator exact lookup (opt-in) swaps in the second-walk
	 * re-descend variant; identity-agnostic (iter holds ordinal bytes).
	 */
	if (ft->rekey_coherence)
		ft->lookup_iter_fn = sc
			? ft_lookup_iter_precise_coherent_sc
			: ft_lookup_iter_precise_coherent_nosc;
	else
		ft->lookup_iter_fn = sc
			? ft_lookup_iter_precise_sc : ft_lookup_iter_precise_nosc;
	/*
	 * Partial-match (tracking=PARTIAL) is precise descent.  Iter form
	 * has no non-identity issue (iter holds ordinals already).
	 */
	ft->lookup_partial_iter_fn = sc
		? ft_lookup_partial_iter_sc : ft_lookup_partial_iter_nosc;
	/*
	 * Longest-match (tracking=LONGEST) is also always precise
	 * descent.  Same shape as partial.
	 */
	ft->lookup_longest_match_iter_fn = sc
		? ft_lookup_longest_match_iter_sc
		: ft_lookup_longest_match_iter_nosc;
#ifdef FEATURE_FT_KEY_MAP
	if (caa_unlikely(!group->key_map.identity)) {
		/* REKEY-coherent exact lookup swaps in the second-walk variant. */
		ft->lookup_key_fn = ft->rekey_coherence ?
			ft_lookup_key_coherent_nonidentity :
			ft_lookup_key_nonidentity;
		ft->lookup_candidate_key_fn = ft_lookup_candidate_key_nonidentity;
		ft->lookup_partial_key_fn = ft_lookup_partial_key_nonidentity;
		ft->lookup_longest_match_key_fn = ft_lookup_longest_match_key_nonidentity;
		return;
	}
#endif
	/*
	 * REKEY-coherent exact lookup (opt-in) swaps in the second-walk
	 * re-descend variant; the plain variant is byte-identical otherwise.
	 */
	if (ft->rekey_coherence)
		ft->lookup_key_fn = sc
			? ft_lookup_precise_coherent_sc
			: ft_lookup_precise_coherent_nosc;
	else
		ft->lookup_key_fn = sc
			? ft_lookup_precise_sc : ft_lookup_precise_nosc;
	ft->lookup_candidate_key_fn = sc
		? ft_lookup_cand_sc : ft_lookup_cand_nosc;
	ft->lookup_partial_key_fn = sc
		? ft_lookup_partial_key_sc : ft_lookup_partial_key_nosc;
	ft->lookup_longest_match_key_fn = sc
		? ft_lookup_longest_match_key_sc
		: ft_lookup_longest_match_key_nosc;
}

enum cds_ft_status cds_ft_create(struct cds_ft_group *ft_group,
		const struct cds_ft_attr *attr,
		struct cds_ft **result_ft)
{
	struct cds_ft *ft;
	struct cds_ft_inode *root_node;
	struct cds_ft_metadata *metadata;
	const struct cds_ft_type *type0 = &ft_types[0];

	/*
	 * Use posix_memalign, not calloc.  The trie pointer is stored in the
	 * TRANSACTED metadata->parent slot of its root (ft_trie_parent).  So
	 * it must clear FT's whole in-band tag.  malloc only promises
	 * _Alignof(max_align_t).  That is 16 on LP64, but 8 on common 32-bit
	 * ABIs, which would let a trie pointer alias the tag space.  Freed
	 * with free() as before.
	 */
	if (posix_memalign((void **) &ft, FT_PARENT_TRIE_ALIGN, sizeof(*ft))) {
		*result_ft = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	memset(ft, 0, sizeof(*ft));
	urcu_posix_assert(!((uintptr_t) ft & FT_PARENT_TAG_MASK));
	ft->group = ft_group;
	/* Cache the group's ordered-list mode for the read-side cell gate. */
	ft->ordered_list = ft_group->ordered_list_set;
	/* Cache the group's order-statistics mode (per-node nr_keys upkeep). */
	ft->rank_stats = ft_group->rank_stats_set;
	/*
	 * A fresh trie's ordinal-cell list is empty.  Init the circular
	 * sentinel so it points at itself.  The zeroed struct would leave it
	 * NULL, which is not a valid empty circular list.  Done
	 * unconditionally, because it is cheap.  Every created trie flows
	 * through here, including the transient detach / merge tries.
	 */
	urcu_txn_list_init(&ft->ord_sentinel);
	/*
	 * The three ownerless words' lock anchors.  A zeroed @state is CLEAN,
	 * which is what ft_meta_lock_acquire expects to CAS from.  Nothing
	 * else in cds_ft_metadata is read for a lock-only anchor.
	 */
	memset(&ft->ord_begin_lock, 0, sizeof(ft->ord_begin_lock));
	memset(&ft->ord_end_lock, 0, sizeof(ft->ord_end_lock));
	memset(&ft->root_lock, 0, sizeof(ft->root_lock));
	/*
	 * Effective per-trie speculative-key state.  It is true when both
	 * hold:
	 *   - the group is configured for speculative result-key capture;
	 *   - this trie did not opt out via
	 *     cds_ft_attr_set_speculative_keys(attr, false).
	 * Set BEFORE ft_install_lookup_ops, which keys @kc off it.  An
	 * opted-out trie then gets the EAGER lookup specializations.
	 */
	ft->speculative_key_offset_active = ft_group->speculative_key_offset_set &&
		(!attr || !attr->speculative_keys_disabled);
	/*
	 * History: REKEY coherence was for a time not an opt-in, and the
	 * opt-in attr was removed.  The reason was that the MOVE GATE
	 * (struct cds_ft::move_active) makes coherence free when no move is
	 * in flight.  So every trie that CAN host an in-trie move carried
	 * the coherent lookup variants and decided per call.  The next
	 * comment says why it is an opt-in again.
	 *
	 * Which tries can host a move: EAGER ones only.  A move changes a
	 * leaf's key, and the library cannot rewrite an application-stored
	 * speculative key.  So a speculative trie must never host a move,
	 * and needs no coherence.  The ordered list is NOT required.  The
	 * point witness is two forward descents, and the continuation rides
	 * the carried key.  Neither needs a cell.  Set BEFORE
	 * ft_install_lookup_ops, which selects the specializations off it.
	 */
	/*
	 * ☞ AND NOW IT IS AN OPT-IN AGAIN, for a reason the move gate does
	 * not cover.  The note above is still right about COST: the gate
	 * does make coherence free while no move is in flight.  But it
	 * assumed the gate's grace period is always payable.  Under
	 * CDS_FT_WRITER_EXTERNAL_SYNC it is not: it is taken under an
	 * exclusion the library does not own.  So the capability is declared
	 * (cds_ft_group_attr_set_rekey).  A trie that never rekeys carries
	 * no coherent variant at all.
	 *
	 * Which tries can still host a move, given the opt-in: EAGER ones
	 * only.  A move changes a leaf's key, and the library cannot rewrite
	 * an application-stored speculative key.
	 */
	ft->rekey_coherence = ft_group->rekey_set &&
			!ft->speculative_key_offset_active;
	/*
	 * EXTERNAL_SYNC + rekey => readers never leave the coherent path.
	 * So the move gate publishes no mode change and owes no grace
	 * period.  Decided HERE, before ft_install_lookup_ops, and immutable
	 * after it.  The reason is the same as for @rekey_coherence: the
	 * specializations are selected off it.  So a concurrent reader's
	 * fn-ptr choice is race-free.
	 */
	/*
	 * ☠ FROM THE GROUP, NOT FROM @ft->external_sync, WHICH IS NOT SET
	 * YET.  That assignment is ~30 lines below this one.  Reading the
	 * trie's own field here would be a constant false, and the failure
	 * would be SILENT.  Every external-sync trie would quietly keep the
	 * move gate's grace period.  Removing that grace period is the
	 * exact thing this flag exists for.
	 */
	ft->rekey_always_coherent = ft->rekey_coherence &&
			ft_group->writer_strategy == CDS_FT_WRITER_EXTERNAL_SYNC;
	ft_install_lookup_ops(ft);
#ifdef FEATURE_FT_VERIFY_AT_MUTATION
	/*
	 * Default to verify-every-mutation cadence to preserve the
	 * historical -DFEATURE_FT_VERIFY_AT_MUTATION behavior.  Tests
	 * working with large tries can call
	 * cds_ft_verify_at_mutation_period_set() to dial it down.
	 * Counter is already zero from the memset above.
	 */
	ft->verify_at_mutation_period = 1;
#endif
	if (attr)
		ft->exclusive = attr->exclusive;
	/*
	 * MW lock-mode gates: a lock-strategy group's tries (COARSE or FINE)
	 * take the FT-wide writer lock at every mutation (writer-scope hook).
	 * A FINE trie ALSO acquires the per-node lock-sets of the op-domains
	 * converted so far (§11.3 step 3 onwards: recompact {C, P}); it keeps
	 * taking the FT-wide lock too until every domain is converted, so a
	 * converted op never races an unconverted one (§11.1).  Hot-path mirrors
	 * of the group strategy so the hooks read one trie field each.
	 */
	ft->lock_fine = (ft_group->writer_strategy == CDS_FT_WRITER_LOCK_FINE);
	/*
	 * The caller's contract replaces the FT-wide writer mutex, and NOTHING
	 * else.  @lock_fine is false here.  So every kind, lock-set and door
	 * decision reads exactly as it does on a COARSE trie.  @exclusive
	 * stays false, so the reader-visible discipline is untouched.
	 */
	ft->external_sync = (ft_group->writer_strategy
			== CDS_FT_WRITER_EXTERNAL_SYNC);
	cds_fair_mutex_init(&ft->writer_lock);
	cds_fair_mutex_init(&ft->bulk_lock);
	/* Move mode gate (struct cds_ft::move_active): movers only. */
	pthread_mutex_init(&ft->move_gate_lock, NULL);
	pthread_cond_init(&ft->move_gate_cond, NULL);
	/*
	 * Writer-contention escalation lane for the concurrent-mode ops
	 * (ft_txn_op_init, doc §11).
	 */
	cds_fair_mutex_init(&ft->lane.lock);
	ft->lane.active = 0;

	/*
	 * Allocate the root node (smallest popcount_2l type, initially empty).
	 *
	 * ft->root always points to an internal node, even when the
	 * trie has no entries (nr_child == 0).  This is the one place
	 * where a node with 0 children is allowed; all other internal
	 * nodes are pruned when their last child is removed.  The node
	 * itself may be replaced by graft or graft-swap, but the
	 * invariant on the slot is maintained across all operations.
	 *
	 * The root is a regular internal node whose metadata
	 * (nr_child, external_nodes) is accessed the same way as any
	 * other node's.  Its metadata carries the NIL-key entries, so
	 * transplanting a root node between tries is a single pointer
	 * swap with no metadata relocation.
	 */
	root_node = alloc_cds_ft_node(ft, type0, &metadata);
	if (!root_node) {
		free(ft);
		*result_ft = NULL;
		return CDS_FT_STATUS_MEMORY_ERROR;
	}
	ft->root = ft_node_flag(root_node, 0);
	/* The root's back-edge names its owning trie (see ft_trie_parent). */
	metadata->parent_word = ft_trie_parent(ft);
	FT_TP(root_publish, (const void *) ft, (const void *) ft->root);

	uatomic_inc(&ft_group->nr_ft_instances, CMM_RELAXED);
	*result_ft = ft;
	FT_TP(ft_create, (const void *) ft, (const void *) ft_group);
	return CDS_FT_STATUS_OK;
}

/*
 * There should be no more concurrent add, delete, nor look-up performed
 * on the Fractal Trie while it is being destroyed (ensured by the
 * caller).
 */
void cds_ft_destroy(struct cds_ft *ft)
{
	const struct rcu_flavor_struct *flavor = ft->group->flavor;

	assert(ft->move_gate_nr == 0);		/* no move may still hold the gate */
	assert(ft->bulk_gate_nr == 0);		/* nor any other bulk op */
	assert(ft->gate_gp_nr == 0);		/* nor a grace period in flight */

	/*
	 * A compaction the caller never ended would otherwise strand its
	 * private ranges (relocated nodes in arena-untracked ranges) and leak
	 * its state/iter.  Finalize it here.  cds_ft_compact_end merges the
	 * private ranges back into the arenas and frees the state.  That
	 * leaves a consistent trie to tear down.
	 */
	if (caa_unlikely(ft->active_compact != NULL)) {
		fprintf(stderr,
			"cds_ft_destroy: trie %p destroyed with a compaction still in progress; finalizing (missing cds_ft_compact_end)\n",
			(void *) ft);
		cds_ft_compact_end(ft->active_compact);
	}
	FT_TP(ft_destroy, (const void *) ft);
	/*
	 * Free root node.  No concurrent readers at this point.  The root was a
	 * published (reader-visible) node throughout the trie's life.  So it
	 * gets its freeze-on-free tombstone like any other retired node (doc
	 * §4.B).  Here it is a teardown no-op, but it keeps the
	 * mark-before-free invariant universal (see FT_DEBUG_TOMBSTONE_AUDIT).
	 */
	ft_meta_tombstone_set_flip(cds_ft_item_to_metadata(ft_node_ptr(ft->root)));
	free_cds_ft_node(ft, ft_node_ptr(ft->root));
	/*
	 * Wait for in-flight call_rcu free to complete.  Every deferred
	 * node free this trie issued is then counted (group-scoped) before
	 * the trie's instance count is dropped.  The group-level node-leak
	 * check runs in cds_ft_group_destroy, once all tries have drained.
	 */
	flavor->barrier();
	pthread_cond_destroy(&ft->move_gate_cond);
	pthread_mutex_destroy(&ft->move_gate_lock);
	uatomic_dec(&ft->group->nr_ft_instances, CMM_RELAXED);
	/* The take/release balance must not outlive the lock words it keys. */
	FT_DT_NOTE_FORGOTTEN(&ft->root_lock);
	FT_DT_NOTE_FORGOTTEN(&ft->ord_begin_lock);
	FT_DT_NOTE_FORGOTTEN(&ft->ord_end_lock);
	free(ft);
}

/*
 * Group / trie accessors: key lengths, key map, and emptiness query.
 */
size_t cds_ft_group_key_len(const struct cds_ft_group *group)
{
	return group->key_len;
}

size_t cds_ft_group_max_key_len(const struct cds_ft_group *group)
{
	return group->max_key_len;
}

size_t cds_ft_max_used_key_len(const struct cds_ft *ft)
{
	return uatomic_load(&ft->max_used_key_len, CMM_RELAXED);
}

enum cds_ft_status cds_ft_group_key_map(const struct cds_ft_group *group, uint8_t *key_to_ordinal, uint8_t *ordinal_to_key)
{
	if (group->key_map.identity)
		return CDS_FT_STATUS_NOT_FOUND;
	memcpy(key_to_ordinal, group->key_map.key_to_ordinal, sizeof(group->key_map.key_to_ordinal));
	memcpy(ordinal_to_key, group->key_map.ordinal_to_key, sizeof(group->key_map.ordinal_to_key));
	return CDS_FT_STATUS_OK;
}

bool cds_ft_empty(struct cds_ft *ft)
{
	struct cds_ft_inode_flag *root_flag;
	struct cds_ft_inode *root_node;
	struct cds_ft_metadata *rmeta;

	CDS_FT_SCOPED_READER(ft);
	CDS_FT_ASSERT_RCU_READ_LOCKED(ft);

	root_flag = ft_root_dereference(ft);
	root_node = ft_node_ptr(root_flag);

	/*
	 * The root is always an internal node.  ft_make_root_internal_glue
	 * enforces this invariant at every site that publishes ft->root.
	 * So no tag dispatch is needed before reading metadata.
	 */
	rmeta = cds_ft_item_to_metadata(root_node);

	/*
	 * Empty trie: the root has no children and no NIL-key entries.
	 * For popcount root, nr_child is derived from the bitmap.  A
	 * freshly-allocated (calloc'd) root with bitmap == 0 correctly
	 * reports nr_child == 0.  Resolve the state word through the MCAS
	 * proxy (§9): a peer's mid-commit FT_STATE_PROXY word would
	 * otherwise read as garbage count bits.
	 */
	if (ft_meta_nr_child_load(rmeta) != 0)
		return false;
	return !uatomic_load(&rmeta->external_nodes, CMM_RELAXED);
}

/*
 * Trie statistics introspection: walk the trie collecting per-level,
 * per-node-type counts and child-count distributions, then render them.
 * Public debug API (cds_ft_recompute_stats / cds_ft_show_stats); off all hot
 * paths.  Lives here beside the other group-level introspection accessors.
 */
struct cds_ft_node_stats {
	uint64_t count;
	uint64_t distribution[257];
};

struct cds_ft_stats_level {
	uint64_t nr_external_nodes;
	uint64_t nr_metadata_external_nodes;
	uint64_t nr_duplicate_external_nodes;
	uint64_t nr_internal_nodes;
	uint64_t nr_compressed_nodes;
	struct cds_ft_node_stats node_stats[FT_TYPE_MAX_NR];
	bool has_nodes;
};

struct cds_ft_stats {
	struct cds_ft_stats_level level[FT_MAX_DEPTH];
};

/*
 * Compute the exact maximum key length of the keys the trie holds RIGHT NOW.
 * Store it over the conservative hint.  This is the recompute proper, WITHOUT
 * the writer scope.  Return CDS_FT_STATUS_OK, or the iterator's error status.
 *
 * It is split out because a caller that is ALREADY inside the writer scope
 * needs it.  ft_rekey_graft_simple_locked repairs the hint from here before
 * its shape gate reads it.  CDS_FT_SCOPED_WRITER nests on one trie
 * (@ft_wlock_depth), so that caller could take it again.  The split says
 * which of the two things a call site wants, rather than relying on the
 * nesting.
 *
 * ☠ THE EXCLUSION IS THE CALLER'S, and the store LOWERS.  A walk misses a key
 * when a peer writer inserts a longer one under it.  Such a walk under-states
 * the hint, and the hint is what bounds the key buffers.  That is the same
 * requirement cds_ft_recompute_stats documents to its own callers.
 */
static
enum cds_ft_status ft_recompute_max_used_key_len(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;
	enum cds_ft_status status;
	size_t max_len = 0;

	status = cds_ft_iter_create(ft, &iter);
	if (status != CDS_FT_STATUS_OK)
		return status;
	cds_ft_for_each_rcu(ft, iter) {
		size_t klen = ft_iter_resolve_key_len(iter);

		if (klen > max_len)
			max_len = klen;
	}
	status = cds_ft_iter_status(iter);
	cds_ft_iter_destroy(iter);
	if (status < 0)
		return status;
	uatomic_store(&ft->max_used_key_len, max_len, CMM_RELAXED);
	/*
	 * HINT FIRST, THEN THE WATERMARK OF THE WALK.  A peer raise that lands
	 * between the two stores leaves them unequal.  That costs one extra
	 * walk later and never skips a needed one.  The reverse order could
	 * record "already exact" over a value the walk never saw.
	 */
	uatomic_store(&ft->max_used_key_len_walked, max_len, CMM_RELAXED);
	return CDS_FT_STATUS_OK;
}

enum cds_ft_status cds_ft_recompute_stats(struct cds_ft *ft)
{
	CDS_FT_SCOPED_WRITER(ft);
	return ft_recompute_max_used_key_len(ft);
}

static
void calc_stats_node(const struct cds_ft *ft __attribute__((unused)),
		struct cds_ft_inode_flag *node_flag, struct cds_ft_stats *stats, int level)
{
	unsigned long node_type = ft_node_type(node_flag);
	struct cds_ft_node_stats *node_stats = &stats->level[level].node_stats[node_type];
	const struct cds_ft_metadata *metadata;

	metadata = cds_ft_item_to_metadata(ft_node_ptr(node_flag));
	node_stats->count++;
	/*
	 * Proxy-resolved load (§9).  The raw state word is the ARRAY INDEX
	 * here.  The 9-bit count field of a mid-flip FT_STATE_PROXY word can
	 * read up to 511, which overruns distribution[257].
	 */
	node_stats->distribution[ft_meta_nr_child_load(metadata)]++;
	stats->level[level].nr_internal_nodes++;
	stats->level[level].has_nodes = true;
}

static
void calc_stats_node_recursive(const struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		struct cds_ft_stats *stats, int level);

static
void calc_stats_node_recursive(const struct cds_ft *ft, struct cds_ft_inode_flag *node_flag,
		struct cds_ft_stats *stats, int level)
{
	unsigned int key;

	for (key = 0; key < 256; key++) {
		struct cds_ft_inode_flag *child_node_flag;

		child_node_flag = ft_node_get_nth(ft, node_flag, NULL, (uint8_t) key, FT_PF_NONE);
		if (!child_node_flag)
			continue;
		if (ft_node_internal(child_node_flag)) {
			struct cds_ft_metadata *metadata = cds_ft_item_to_metadata(ft_node_ptr(child_node_flag));
			struct cds_ft_node *external_nodes =
				ft_dereference_external_acquire(metadata->external_nodes);

			calc_stats_node(ft, child_node_flag, stats, level);
			if (external_nodes) {
				struct cds_ft_node *iter_node;
				unsigned int count = 0;

				iter_node = external_nodes;
				cds_ft_for_each_duplicate_rcu(iter_node) {
					if (count++ == 0)
						stats->level[level].nr_metadata_external_nodes++;
					else
						stats->level[level].nr_duplicate_external_nodes++;
					stats->level[level].has_nodes = true;
				}
			}
			calc_stats_node_recursive(ft, child_node_flag, stats, level + 1);
		} else if (ft_node_compressed(child_node_flag)) {
			struct cds_ft_compressed_node *cn =
				ft_compressed_node_ptr(child_node_flag);
			struct cds_ft_metadata *metadata =
				cds_ft_item_to_metadata((struct cds_ft_inode *) cn);
			struct cds_ft_node *external_nodes =
				ft_dereference_external_acquire(metadata->external_nodes);
			struct cds_ft_inode_flag *cn_child;
			int j;

			stats->level[level].nr_internal_nodes++;
			stats->level[level].nr_compressed_nodes++;
			stats->level[level].has_nodes = true;
			if (external_nodes) {
				struct cds_ft_node *iter_node;
				unsigned int count = 0;

				iter_node = external_nodes;
				cds_ft_for_each_duplicate_rcu(iter_node) {
					if (count++ == 0)
						stats->level[level].nr_metadata_external_nodes++;
					else
						stats->level[level].nr_duplicate_external_nodes++;
					stats->level[level].has_nodes = true;
				}
			}
			for (j = 1; j < cn->len; j++) {
				stats->level[level + j].nr_internal_nodes++;
				stats->level[level + j].nr_compressed_nodes++;
				stats->level[level + j].has_nodes = true;
			}
			/* Proxy-resolved (§9): raw cn->child is type-classified + recursed. */
			cn_child = ft_cn_child_dereference_acquire_prefetch(cn);
			if (cn_child &&
			    !ft_node_external(cn_child))
				calc_stats_node_recursive(ft, cn_child, stats, level + cn->len);
			else if (cn_child) {
				struct cds_ft_node *iter_node;
				unsigned int count = 0;

				iter_node = (struct cds_ft_node *) ft_node_ptr(cn_child);
				cds_ft_for_each_duplicate_rcu(iter_node) {
					if (count++ == 0)
						stats->level[level + cn->len].nr_external_nodes++;
					else
						stats->level[level + cn->len].nr_duplicate_external_nodes++;
					stats->level[level + cn->len].has_nodes = true;
				}
			}
		} else {
			struct cds_ft_node *iter_node;
			unsigned int count = 0;

			iter_node = (struct cds_ft_node *) ft_node_ptr(child_node_flag);
			cds_ft_for_each_duplicate_rcu(iter_node) {
				if (count++ == 0)
					stats->level[level].nr_external_nodes++;
				else
					stats->level[level].nr_duplicate_external_nodes++;
				stats->level[level].has_nodes = true;
			}
		}
	}
}

static
void do_show_stats(const struct cds_ft *ft, FILE *out, const struct cds_ft_stats *stats)
{
	int level;

	fprintf(out, "Fractal Trie (%p) Statistics\n", ft);
	fprintf(out, "---------------------------------------------------\n");
	for (level = 0; level < FT_MAX_DEPTH; level++) {
		const struct cds_ft_stats_level *stats_level = &stats->level[level];
		unsigned long type;

		if (!stats_level->has_nodes)
			break;
		fprintf(out, "Level: %d\n", level);
		if (stats_level->nr_external_nodes) {
			print_indent(out, 1);
			fprintf(out, "External nodes: %" PRIu64 "\n", stats_level->nr_external_nodes);
		}
		if (stats_level->nr_metadata_external_nodes) {
			print_indent(out, 1);
			fprintf(out, "Metadata external nodes: %" PRIu64 "\n", stats_level->nr_metadata_external_nodes);
		}
		if (stats_level->nr_duplicate_external_nodes) {
			print_indent(out, 1);
			fprintf(out, "Duplicate external nodes: %" PRIu64 "\n", stats_level->nr_duplicate_external_nodes);
		}
		if (stats_level->nr_internal_nodes) {
			print_indent(out, 1);
			fprintf(out, "Internal nodes: %" PRIu64 "\n", stats_level->nr_internal_nodes);
		}
		if (stats_level->nr_compressed_nodes) {
			print_indent(out, 1);
			fprintf(out, "Compressed nodes: %" PRIu64 "\n", stats_level->nr_compressed_nodes);
		}
		for (type = 0; type < FT_TYPE_MAX_NR; type++) {
			const struct cds_ft_node_stats *node_stats = &stats->level[level].node_stats[type];
			uint64_t nr_nodes = node_stats->count;

			if (nr_nodes) {
				unsigned int i;
				bool first = true;

				print_indent(out, 2);
				fprintf(out, "Internal node type %lu: %" PRIu64 " (", type, nr_nodes);
				for (i = 0; i <= 256; i++) {
					if (node_stats->distribution[i]) {
						fprintf(out, "%s%u: %" PRIu64,
							(!first ? ", " : ""), i, node_stats->distribution[i]);
						first = false;
					}
				}
				fprintf(out, ")\n");
			}
		}
	}
	fprintf(out, "---------------------------------------------------\n");
}

void cds_ft_show_stats(const struct cds_ft *ft, FILE *out)
{
	struct cds_ft_inode_flag *node_flag;
	struct cds_ft_stats *stats;
	int level = 0;

	/*
	 * struct cds_ft_stats is several MiB.  It holds a per-level
	 * node-type distribution histogram (257 buckets) for each of
	 * FT_MAX_DEPTH levels.  That is far too large for the stack on
	 * threads with a small stack (e.g. musl's 128 KiB default).  So
	 * heap-allocate it.
	 */
	stats = calloc(1, sizeof(*stats));
	if (!stats) {
		fprintf(out, "Fractal Trie (%p) Statistics: out of memory\n", ft);
		return;
	}

	/* Proxy-resolved root load (§9) -- cds_ft_show_stats has no exclusion contract. */
	node_flag = ft_root_dereference(ft);

	/* Root is always present and always internal. */
	calc_stats_node(ft, node_flag, stats, level);
	calc_stats_node_recursive(ft, node_flag, stats, level + 1);
	do_show_stats(ft, out, stats);
	free(stats);
}
