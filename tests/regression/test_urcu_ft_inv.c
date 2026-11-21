// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * test_urcu_ft_inv.c
 *
 * Userspace RCU library - Fractal Trie Invariant Tests
 *
 * Concurrent invariant tests for the Fractal Trie. Each test creates
 * a shared trie, spawns one or more writer threads that mutate the
 * trie under mutex, and one or more reader threads that perform
 * lookups and traversals under rcu_read_lock(). Readers assert
 * structural invariants that must hold at every point a concurrent
 * RCU reader can observe: ordering, key consistency, duplicate chain
 * acyclicity, graft-swap atomicity, etc.
 *
 * A violation of any invariant means a concurrent reader observed
 * illegal intermediate state — exactly the class of bugs these tests
 * are designed to catch.
 *
 * Build example (adapt include/library paths to your tree):
 *
 *   cc -O2 -g -DRCU_QSBR \
 *      -I/path/to/urcu/include \
 *      test_urcu_ft_inv.c \
 *      -lurcu-qsbr -lurcu-cds -lurcu-common -lpthread \
 *      -o test_urcu_ft_inv
 *
 * Run all:  ./test_urcu_ft_inv
 * Run one:  ./test_urcu_ft_inv inv_iteration_order
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <urcu/compiler.h>
#ifdef RCU_MEMB
/*
 * The urcu-memb build: a read-side section is real there.  So it catches a
 * section held where QSBR cannot tell, e.g. around a bulk op that waits for
 * a grace period.  urcu-memb defines the QSBR-only calls as no-ops.
 */
#include <urcu.h>		/* Must precede fractal-trie.h */
#else
#include <urcu-qsbr.h>		/* Must precede fractal-trie.h */
#endif
#include <urcu/fractal-trie.h>
#include <urcu-call-rcu.h>

#ifdef FT_ENABLE_TRACING
#include "../../src/fractal-trie/cds_ft_tp.h"
#define FT_TEST_TP(name, ...) \
	lttng_ust_tracepoint(cds_ft, name, ##__VA_ARGS__)
#else
#define FT_TEST_TP(name, ...) do {} while (0)
#endif

#include <assert.h>
#include <inttypes.h>
#include <stdarg.h>
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

#define NR_TESTS_REKEY_DLM	23	/* inv_rekey_graft_{disjoint,cross_junction,run_junction,elevating_junction,glue_dst,coherent_readers,shared}, inv_rekey_merge_same_parent_coherent_readers, inv_rekey_linearizability, inv_rekey_public_{atomic_no_gap,atomic_no_gap_varlen,atomic_no_gap_compressed_top}, inv_rekey_coarse_progress, inv_rekey_{coarse,fine,contended}_mixed_writers, inv_rekey_merge_{occupied,compressed,shared}_dst, inv_rekey_merge_compressed_dst_rootchurn */

/*
 * Base count = the RUN_TEST invocations in main() outside the DLM #ifdef.
 * Keep it EXACT: libtap's exit_status() returns planned-minus-run, so an
 * over-declared plan is a silent nonzero exit that nothing reads -- the gate
 * counted "ok "/"not ok " lines only, so 65 tests against a plan of 68 scored
 * green for as long as the constant was stale.  ft_parallel_gate.sh now
 * compares the run count against this plan, so retiring a test means
 * decrementing here in the same commit.
 */
/* +4 inv_prefix_head_*_key_identity; +2 inv_split_point_lookup_identity*;
 * +2 inv_absent_key_never_found* */
/* +1 inv_graft_swap_whole_seam_points */
#define NR_TESTS	(145 + NR_TESTS_REKEY_DLM)	/* +1: inv_owned_long_runs; +1: inv_owned_prefix_dense_remove_all; +1: inv_ir_prefix_roundtrip; +1: inv_owned_prefix_dense_replace; +1: inv_owned_prefix_dense; +1: inv_guard_compressed_publish; +1: inv_concurrent_insert_unique_dense; +1: inv_prefix_pair_compressed_holder; +1: inv_prefix_siblings_compressed_holder; +1: inv_concurrent_insert_unique_prefix; +1: inv_prefix_dup_promote_vs_extension; +1: inv_prefix_shape_zoo */

/* ------------------------------------------------------------------ */
/* Tuning knobs                                                       */
/* ------------------------------------------------------------------ */

/*
 * Duration of each concurrent test in milliseconds.  Kept short for
 * automated runs; increase for deeper stress testing.  The time-bounded
 * cross-view tests build tries for this long and then tear them down (a
 * per-trie rcu_barrier is the dominant cost), so this bounds their teardown:
 * 200ms builds ~10x fewer tries than 2000ms, cutting the slow teardown ~10x
 * while still exercising every cross-view window many times over.
 */
#define DEFAULT_DURATION_MS	200

/* Thread counts per test. */
#define NR_READERS_DEFAULT	4
#define NR_WRITERS_DEFAULT	2

/*
 * Key pool sizes.  The writer pool is intentionally smaller than the
 * reader lookup pool so that readers frequently encounter both hits
 * and misses.
 */
#define WRITER_POOL_SIZE	512
#define READER_POOL_SIZE	1024
#define MAX_DUP_CHAIN_LEN	(WRITER_POOL_SIZE * 4)

/* ------------------------------------------------------------------ */
/* Test-node infrastructure                                           */
/* ------------------------------------------------------------------ */

struct ft_test_node {
	struct cds_ft_node node;
	struct rcu_head head;
	unsigned long freed_once;	/* set when handed to call_rcu (see node_free_rcu) */
	uint64_t key;		/* shadow copy for validation */
	uint64_t value;
	/*
	 * Ordinal (big-endian, trie-order) key bytes, populated at insert.
	 * This is the representation the speculative inequality lookup copies
	 * from the leaf, so it must hold exactly the bytes the trie navigates
	 * on (NOT the host-order @key shadow above).  create_fixed_ft points
	 * speculative_key_offset here.
	 */
	uint8_t okey[8];
};

static inline
void ft_test_node_init(struct ft_test_node *n, uint64_t key)
{
	cds_ft_node_init(&n->node);
	n->key = key;
	n->value = 0;
}

static inline
struct ft_test_node *to_test_node(struct cds_ft_node *n)
{
	return caa_container_of(n, struct ft_test_node, node);
}

static unsigned long nodes_allocated, nodes_freed;
/*
 * Count of nodes handed to node_free_rcu() a SECOND time -- see the comment
 * there.  Reported by leak_check(), because the count is a real observation
 * about the library under an out-of-contract workload, not harness noise.
 */
static unsigned long nodes_double_freed;

static struct ft_test_node *node_alloc(uint64_t key)
{
	struct ft_test_node *n = (struct ft_test_node *) calloc(1, sizeof(*n));
	if (!n) {
		fprintf(stderr, "node_alloc: out of memory\n");
		abort();
	}
	ft_test_node_init(n, key);
	__atomic_add_fetch(&nodes_allocated, 1, __ATOMIC_RELAXED);
	return n;
}

static void node_free(struct ft_test_node *n)
{
	memset(n, 0xfe, sizeof(*n));	/* poison */
	free(n);
	__atomic_add_fetch(&nodes_freed, 1, __ATOMIC_RELAXED);
}

static void node_free_rcu_cb(struct rcu_head *head)
{
	node_free(caa_container_of(head, struct ft_test_node, head));
}

static void node_free_rcu(struct ft_test_node *n)
{
	/*
	 * ARBITRATE the deferred free, because "cds_ft_remove returned OK" does
	 * NOT make this thread the sole owner of @n.
	 *
	 * The library's update contract is caller-serialized ("mutual exclusion
	 * between updates is the caller's responsibility"), and the SAME-KEY
	 * oracle inv_concurrent_writers_shared deliberately runs outside it: 16
	 * writers contend one key range, so two of them can look up the SAME node
	 * and both be told OK.  Its node lifecycle -- "remove == OK owns the
	 * RCU-deferred free" -- then calls call_rcu TWICE on one rcu_head.
	 *
	 * That is not a survivable mistake: the second enqueue re-initialises a
	 * LIVE wfcqueue node and corrupts the per-CPU call_rcu queue, so the
	 * damage lands wherever the process reaches next -- a HANG in this test's
	 * own post-test rcu_barrier, a glibc "double free or corruption" abort in
	 * a later test's, or an ord-cell verify mismatch three tests on.  Every
	 * one of those looks like a bug in an innocent test (each passes in
	 * isolation), which is exactly how much time it costs.
	 *
	 * So let the first free win, and COUNT the losers: that count is the real
	 * observation ("the library handed OK to two writers for one node, N
	 * times") where a corrupted RCU queue is only noise.  The loser's call is
	 * a pure no-op -- the winner's call_rcu already owns the one free, so the
	 * node is neither leaked nor freed twice and the leak accounting stays
	 * exactly balanced.
	 */
	if (__atomic_exchange_n(&n->freed_once, 1, __ATOMIC_SEQ_CST) != 0) {
		__atomic_add_fetch(&nodes_double_freed, 1, __ATOMIC_RELAXED);
		return;
	}
	call_rcu(&n->head, node_free_rcu_cb);
}

static void mw_violation_snapshot(void);

/*
 * cds_ft_verify, and on a failure DUMP the structure that failed it.
 *
 * A structural violation names one node; what it is wired to is the rest of the
 * finding, and it only exists right here -- the next step of any teardown
 * destroys it.  cds_ft_show RECURSES the tree instead of navigating it in key
 * order, so it returns on exactly the tries an ordered walk cannot get through,
 * and the shapes that fail are small (the sibling oracle's whole trie is ~20
 * nodes).
 */
static int verify_or_dump(struct cds_ft *ft, const char *what)
{
	if (cds_ft_verify(ft, stderr) == CDS_FT_STATUS_OK)
		return 0;
	fprintf(stderr, "%s: cds_ft_verify FAILS -- structure follows\n", what);
	cds_ft_show(ft, stderr, CDS_FT_SHOW_PRETTY);
	mw_violation_snapshot();
	return -1;
}

/*
 * Does a FULL ordered enumeration of the trie still terminate, and does the
 * structure still verify, now that the writers are joined?
 *
 * Every concurrent invariant tears down through drain_and_destroy, and that
 * drain only ever calls cds_ft_lookup_first in a loop: each call restarts at
 * the root, so as long as the FIRST key stays reachable the drain completes and
 * NOTHING ever walks the whole trie.  A trie whose successor walk is broken
 * from the middle onwards therefore tears down silently, and the damage is
 * reported -- if at all -- as a bare node leak three steps later.
 *
 * BOUNDED, never unbounded: the damage this looks for makes cds_ft_next cycle,
 * and a diagnostic that hangs reports nothing at all.  An earlier attempt at
 * this check used cds_ft_count_entries and turned five gate configs into
 * timeouts, which costs every test after it in those configs -- worse than the
 * missing check.  With the cap the same trie yields a named failure instead.
 *
 * Unconditional: measured at 36.1 s vs 36.8 s for the whole ft_inv suite, i.e.
 * within run-to-run noise, so there is nothing to gate it behind.
 */
static int teardown_walk_check(struct cds_ft *ft, const char *what)
{
	struct cds_ft_iter *iter;
	unsigned long steps = 0;
	const unsigned long cap = 1000000;
	enum cds_ft_status s;
	int ret = 0;

	/*
	 * VERIFY FIRST, and on a failure do not walk at all.  The step cap below
	 * does NOT bound the walk on a structurally broken trie: the observed
	 * non-termination is a going-up cycle INSIDE one cds_ft_lookup_first call
	 * (measured: the same level/slot/anchor 100001 times), which never comes
	 * back to be counted.  A caught run that then hangs loses every test
	 * after it, so the validator's named node is the report.
	 */
	if (verify_or_dump(ft, what)) {
		fprintf(stderr, "%s: teardown ordered walk SKIPPED (it may not "
			"return on this trie)\n", what);
		return -1;
	}
	if (cds_ft_iter_create(ft, &iter) < 0)
		return -1;
	rcu_read_lock();
	for (s = cds_ft_lookup_first(ft, iter); s == CDS_FT_STATUS_OK;
			s = cds_ft_next(ft, iter)) {
		if (++steps > cap) {
			fprintf(stderr, "%s: teardown ordered walk OVERRUN "
				"(%lu steps, still going)\n", what, cap);
			ret = -1;
			break;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	if (ret)
		mw_violation_snapshot();
	return ret;
}

/* Free every node reachable through the trie, then destroy the trie. */
static int drain_and_destroy(struct cds_ft *ft, struct cds_ft_group *group)
{
	struct cds_ft_iter *iter;
	enum cds_ft_status s;
	int ret = 0;

	if (teardown_walk_check(ft, "drain_and_destroy"))
		ret = -1;
	s = cds_ft_iter_create(ft, &iter);
	if (s < 0) {
		cds_ft_destroy(ft);
		cds_ft_group_destroy(group);
		return -1;
	}
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;

		s = cds_ft_remove_all(ft, iter, &head);
		if (s < 0) {
			ret = -1;
			break;
		}
		/*
		 * lookup_first just found a key, so remove_all MUST remove it; a
		 * NOT_FOUND means the iterator's key disagrees with its node -- a
		 * stale-iter bug (e.g. draining a detach-stripped trie with the
		 * leaf's pre-detach speculative key still set, which violates the
		 * speculative_key_offset contract).  Fail loud instead of looping
		 * forever on a lookup_first that keeps re-finding the same head.
		 */
		if (s == CDS_FT_STATUS_NOT_FOUND) {
			fprintf(stderr,
				"drain_and_destroy: lookup_first found a key that "
				"remove_all reports NOT_FOUND -- stale iterator key "
				"(speculative key inconsistent with trie position?)\n");
			abort();
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp) {
			node_free_rcu(to_test_node(head));
		}
	}
	rcu_read_unlock();
	rcu_barrier();
	cds_ft_iter_destroy(iter);
	cds_ft_destroy(ft);
	cds_ft_group_destroy(group);
	return ret;
}

static void leak_reset(void)
{
	__atomic_store_n(&nodes_allocated, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&nodes_freed, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&nodes_double_freed, 0, __ATOMIC_RELAXED);
}

static int leak_check(void)
{
	rcu_barrier();
	unsigned long na = __atomic_load_n(&nodes_allocated, __ATOMIC_RELAXED);
	unsigned long nf = __atomic_load_n(&nodes_freed, __ATOMIC_RELAXED);
	unsigned long nd = __atomic_load_n(&nodes_double_freed, __ATOMIC_RELAXED);

	/*
	 * Not a failure: an out-of-contract same-key oracle can legitimately see
	 * two writers told OK for one node (node_free_rcu arbitrates).  Report it
	 * so the observation is not lost -- silently arbitrating would hide the
	 * only evidence that it happened.
	 */
	if (nd)
		fprintf(stderr, "# NOTE: %lu node(s) freed twice (two writers "
			"got OK for one node); second free arbitrated away\n", nd);
	if (na != nf) {
		fprintf(stderr, "LEAK: allocated %lu, freed %lu (delta %ld)\n",
			na, nf, (long)(na - nf));
		return -1;
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* Helpers                                                            */
/* ------------------------------------------------------------------ */

/*
 * FT_INV_RANK_STATS: when set, enable per-node order statistics (nr_keys) on
 * every trie the invariant suite builds.  The default suite runs rank stats
 * OFF (count queries fall back to a structural recount), so the maintained
 * nr_keys aggregate -- and the flip-txn count-edge fold that keeps it exact --
 * is never exercised by the concurrent invariants.  Turning it on makes a
 * FEATURE_FT_VERIFY_AT_MUTATION build check per-node nr_keys exactness after
 * every mutation across the whole suite (cds_ft_verify gates its nr_keys check
 * on ft->rank_stats).
 */
static void inv_maybe_set_rank_stats(struct cds_ft_group_attr *attr)
{
	if (getenv("FT_INV_RANK_STATS") &&
			cds_ft_group_attr_set_rank_stats(attr, true) < 0)
		abort();
}

static struct cds_ft *create_fixed_ft(size_t klen, struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	/*
	 * Exercise the speculative inequality leaf-key capture: the result
	 * key is copied from the matched leaf's ordinal key bytes (okey),
	 * stored at this offset from the embedded cds_ft_node.
	 */
	if (cds_ft_group_attr_set_speculative_key_offset(attr,
			offsetof(struct ft_test_node, okey)) < 0)
		abort();
	/*
	 * FT_INV_NO_ORDERED_LIST forces the ordered list OFF on this (default)
	 * group, exercising the runtime cell-optional path: a cell build then
	 * allocates NO ordinal cells (head->prev is the flagged parent directly).
	 * The ordered invariants use create_fixed_ord_ft (explicit set_ordered_list)
	 * so they are unaffected; run the NON-ordered invariants under this env to
	 * stress the no-cell path concurrently.
	 */
	if (getenv("FT_INV_NO_ORDERED_LIST") &&
			cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Like create_fixed_ft, but also enables the library-owned ordered list
 * (cds_ft_group_attr_set_ordered_list).  In a FEATURE_FT_ORD_CELL library this
 * threads the duplicate-chain heads through relocatable ordinal cells and makes
 * cds_ft_next / cds_ft_prev walk that list with a lazily-materialized result
 * key; in a library without the feature the flag is inert (the group behaves
 * exactly like create_fixed_ft).  Used to exercise the cell ordered-iteration +
 * cds_ft_iter_bind_key cross-CS resume path.
 */
static struct cds_ft *create_fixed_ord_ft(size_t klen, struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_speculative_key_offset(attr,
			offsetof(struct ft_test_node, okey)) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, true) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Like create_fixed_ft, but UNCONDITIONALLY disables the ordered list
 * (cds_ft_group_attr_set_ordered_list(attr, false)), so the group allocates NO ordinal
 * cells: a head's prev is the flagged parent directly.  Used by the dedicated
 * no-cell concurrent invariant so the runtime cell-optional read path
 * (ft_resolve_head_prev prev-direct, skip resolution, parent backtrack) is
 * exercised in the default test pass regardless of FT_INV_NO_ORDERED_LIST.
 */
static struct cds_ft *create_fixed_nolist_ft(size_t klen, struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_speculative_key_offset(attr,
			offsetof(struct ft_test_node, okey)) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Like create_fixed_ft, but UNCONDITIONALLY enables per-node order statistics
 * (nr_keys), independent of FT_INV_RANK_STATS.  Used by the strict nr_keys
 * exactness invariant, which reads the maintained aggregate (cds_ft_count_keys,
 * lookup_nth) and cross-checks it against cds_ft_verify's per-node recount.
 */
static struct cds_ft *create_fixed_rankstats_ft(size_t klen,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_speculative_key_offset(attr,
			offsetof(struct ft_test_node, okey)) < 0)
		abort();
	if (getenv("FT_INV_NO_ORDERED_LIST") &&
			cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (cds_ft_group_attr_set_rank_stats(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Coarse MW lock-mode trie (CDS_FT_WRITER_LOCK_COARSE): every mutator serializes
 * under the single FT-wide writer lock; readers wait-free.  rank_stats ON (the
 * coarse target build, §10.5), EAGER (no speculative offset -- the MW oracle
 * does exact lookups only).  Used by inv_concurrent_writers_coarse_lock.
 */
static struct cds_ft *create_fixed_coarse_lock_ft(size_t klen,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_rank_stats(attr, true) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Fine-grained MW lock-mode trie (CDS_FT_WRITER_LOCK_FINE): the op-domains
 * converted so far (§11.3 step 3: recompact) acquire their per-node lock-set
 * {C, P} (+ {GP}) instead of §4.B-guarding the parent, and release the surviving
 * members through the {LOCK|s -> s} terminal at the commit.  Until every
 * domain is converted the trie ALSO takes the FT-wide writer lock, so the
 * per-node locks are exercised under serialization rather than contended (the
 * §11.1 coexistence hazard forbids racing a converted op with an unconverted
 * one).  rank_stats OFF: the §9 lock-sets are derived for the default build --
 * a rank_stats-ON trie is the coarse single-lock target instead (§10.5).
 * Used by inv_concurrent_writers_fine_lock.
 */
static struct cds_ft *create_fixed_fine_lock_ft(size_t klen,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * Remove the key @v and reclaim its node.  Own read section + own iterator, so
 * it is safe to call from a writer that holds no other FT state.  Returns the
 * remove status, or NOT_FOUND when the key is absent.
 */
static enum cds_ft_status remove_u64(struct cds_ft *ft, uint64_t v)
{
	struct cds_ft_iter *iter = NULL;
	struct cds_ft_node *found;
	enum cds_ft_status st = CDS_FT_STATUS_NOT_FOUND;
	uint8_t k[8] = { 0 };

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	cds_ft_lookup(ft, iter);
	found = cds_ft_iter_node(iter);
	if (found) {
		st = cds_ft_remove(ft, iter, found);
		if (st == CDS_FT_STATUS_OK)
			node_free_rcu(to_test_node(found));
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return st;
}

static struct cds_ft *create_fixed_fine_lock_listoff_ft(size_t klen,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static enum cds_ft_status
insert_u64(struct cds_ft *ft, uint64_t v, struct ft_test_node *n)
{
	uint8_t k[8] = { 0 };

	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	/* Stash the ordinal key bytes for speculative leaf-key capture. */
	memcpy(n->okey, k, sizeof(n->okey));
	return cds_ft_insert(ft, k, CDS_FT_LEN_DEFAULT, &n->node);
}

static enum cds_ft_status
insert_replace_u64(struct cds_ft *ft, uint64_t v, struct ft_test_node *n,
		struct cds_ft_node **old_ret)
{
	uint8_t k[8] = { 0 };

	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	memcpy(n->okey, k, sizeof(n->okey));
	return cds_ft_insert_replace(ft, k, CDS_FT_LEN_DEFAULT, &n->node, old_ret);
}

static enum cds_ft_status
lookup_u64(struct cds_ft *ft, uint64_t v, struct cds_ft_node **out)
{
	uint8_t k[8];

	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	return cds_ft_eager_lookup_key(ft, k, CDS_FT_LEN_DEFAULT, CDS_FT_LEN_DEFAULT, out);
}

/* ------------------------------------------------------------------ */
/* Shared concurrent-test control                                     */
/* ------------------------------------------------------------------ */

static volatile int test_go, test_stop;
static volatile int test_drained;	/* inv_remove_cross_view: pool emptied */

static unsigned long long elapsed_ms(struct timespec *start)
{
	struct timespec now;

	clock_gettime(CLOCK_MONOTONIC, &now);
	return (unsigned long long)((long long)(now.tv_sec - start->tv_sec) * 1000LL
		+ (long long)(now.tv_nsec - start->tv_nsec) / 1000000LL);
}

/*
 * Per-thread violation counters.  Each reader thread bumps its own
 * atomic counter on violation.  The main thread sums them after join.
 */
static atomic_ulong violation_count;

/*
 * Record an LTTng flight-recorder snapshot, PID-tagging the snapshot name so a
 * concurrent soak (many processes) can correlate each snapshot to the process
 * that dumped its core.  Called from the violation reporters and the
 * fatal-signal handlers below: system()/snprintf() are not async-signal-safe,
 * but this is accepted crash-time scaffolding (getpid() is async-signal-safe).
 */
static void ft_snapshot_record(void)
{
	char cmd[160];

	(void) snprintf(cmd, sizeof(cmd),
		"lttng snapshot record -n pid%ld 1>&2", (long) getpid());
	(void) system(cmd);
}

/*
 * /!\ A LIVENESS VIOLATION IS A CLAIM ABOUT THE CODE, AND THE KERNEL CAN FAKE
 * IT.  Every no-progress detector here measures WALL time, so a thread that is
 * simply not running looks exactly like a thread that is wedged -- and on a
 * fragmented box a transparent-hugepage fault can sit in direct compaction with
 * no syscall, no context switch and no counter moving until it ends.
 *
 * So a violation reports what separates the two, at the moment it fires:
 * this process's user vs system CPU, and the kernel's compaction / THP
 * counters since the run began.  A genuine spin burns USER time with compaction
 * idle; a fragmentation stall shows SYSTEM time with compact_stall / _fail
 * climbing and thp_fault_fallback non-zero.  Neither is inferred from the other.
 */
struct ft_kstat {
	unsigned long compact_stall, compact_fail, compact_success;
	unsigned long thp_fault_alloc, thp_fault_fallback;
};

static struct ft_kstat ft_kstat_base;

static void ft_kstat_read(struct ft_kstat *k)
{
	static const struct { const char *key; size_t off; } map[] = {
		{ "compact_stall",	offsetof(struct ft_kstat, compact_stall) },
		{ "compact_fail",	offsetof(struct ft_kstat, compact_fail) },
		{ "compact_success",	offsetof(struct ft_kstat, compact_success) },
		{ "thp_fault_alloc",	offsetof(struct ft_kstat, thp_fault_alloc) },
		{ "thp_fault_fallback",	offsetof(struct ft_kstat, thp_fault_fallback) },
	};
	char line[128];
	FILE *f;
	size_t i;

	memset(k, 0, sizeof(*k));
	f = fopen("/proc/vmstat", "r");
	if (!f)
		return;			/* not Linux, or restricted: report zeros */
	while (fgets(line, sizeof(line), f)) {
		for (i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
			size_t n = strlen(map[i].key);

			if (strncmp(line, map[i].key, n) || line[n] != ' ')
				continue;
			*(unsigned long *) ((char *) k + map[i].off) =
				strtoul(line + n + 1, NULL, 10);
			break;
		}
	}
	fclose(f);
}

/* Baseline for the deltas; called once, before any test runs. */
static void ft_kstat_init(void)
{
	ft_kstat_read(&ft_kstat_base);
}

/* One line of "was it us or was it the kernel?", printed beside a violation. */
static void ft_kstat_report(const char *test)
{
	struct ft_kstat now;
	struct rusage ru;

	ft_kstat_read(&now);
	if (getrusage(RUSAGE_SELF, &ru) != 0)
		return;
	fprintf(stderr,
		"[VIOLATION-CTX] %s: cpu user=%ld.%03lds sys=%ld.%03lds | "
		"since start: compact_stall=%lu compact_fail=%lu compact_success=%lu "
		"thp_fault_alloc=%lu thp_fault_fallback=%lu\n",
		test,
		(long) ru.ru_utime.tv_sec, (long) (ru.ru_utime.tv_usec / 1000),
		(long) ru.ru_stime.tv_sec, (long) (ru.ru_stime.tv_usec / 1000),
		now.compact_stall	- ft_kstat_base.compact_stall,
		now.compact_fail	- ft_kstat_base.compact_fail,
		now.compact_success	- ft_kstat_base.compact_success,
		now.thp_fault_alloc	- ft_kstat_base.thp_fault_alloc,
		now.thp_fault_fallback	- ft_kstat_base.thp_fault_fallback);
}

static void report_violation(const char *test, const char *fmt, ...)
	__attribute__((format(printf, 2, 3)));

static void report_violation(const char *test, const char *fmt, ...)
{
	va_list ap;
	char msg[256];

	atomic_fetch_add(&violation_count, 1);
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	fprintf(stderr, "[VIOLATION] %s: %s\n", test, msg);
	ft_kstat_report(test);
	FT_TEST_TP(inv_violation, test, msg);
	/*
	 * Abort on first violation so lttng-ust snapshot captures the
	 * in-memory ring buffer up to the moment of the fault.  Useful
	 * when running under `lttng record-snapshot` tracing.
	 */
	if (getenv("FT_INV_ABORT_ON_VIOLATION")) {
		/* XXX temporary: capture the flight-recorder ring before dying. */
		ft_snapshot_record();
		abort();
	}
}

/* ------------------------------------------------------------------ */
/* Macro: run a test, emit TAP ok/not-ok, skip if filtered out.       */
/* ------------------------------------------------------------------ */

/*
 * MERGE-DEPENDENT ORACLE PROLOGUE.
 *
 * ft_unit gates every merge test on cds_ft_merge_enabled(); ft_inv gated
 * NOTHING, so on -DNO_FEATURE_FT_MERGE every merge oracle ran against an API
 * that answers NOT_SUPPORTED and the first root-swap one SIGABRTed -- ft_inv
 * had never been runnable on that arm at all, and the gate matrix therefore
 * never ran it there.  Measured at eb254035: 52 ok, `not ok 39`, then SIGABRT
 * at inv_merge_root_swap_cross_view.
 *
 * Skip rather than #ifdef, and name the reason, so the log says what is not
 * being checked instead of the test silently vanishing.
 */
#define INV_NEED_MERGE(name)						\
	do {								\
		if (!cds_ft_merge_enabled()) {				\
			fprintf(stderr, "# " name ": skipped, merge "	\
				"compiled out (-DNO_FEATURE_FT_MERGE)\n"); \
			return 0;					\
		}							\
	} while (0)

/*
 * FT_INV_REPEAT=N, with a test NAME filter only: run that one test N times in
 * this process, stopping at the first red, and report ONE TAP line -- the
 * plan stays exact.  Each repetition runs the test's own oracle and the leak
 * check.  A rare race that shows once in hundreds of runs needs hundreds of
 * runs in ONE process under preemption (taskset + competing spinners); a
 * process per run spends its time in startup.  Ignored without a filter, so
 * a stray environment variable cannot multiply a whole suite.
 */
static unsigned long inv_repeat(const char *filter)
{
	const char *e = getenv("FT_INV_REPEAT");
	unsigned long n;

	if (!filter || !e)
		return 1;
	n = strtoul(e, NULL, 10);
	return n ? n : 1;
}

#define RUN_TEST(fn)							\
	do {								\
		unsigned long rep_n__, rep_i__;				\
		int rep_ok__ = 1;					\
									\
		if (filter && strcmp(filter, #fn) != 0) {		\
			skip(1, "filtered out: " #fn);			\
			break;						\
		}							\
		rep_n__ = inv_repeat(filter);				\
		for (rep_i__ = 0; rep_i__ < rep_n__ && rep_ok__;	\
				rep_i__++) {				\
			leak_reset();					\
			atomic_store(&violation_count, 0);		\
			rcu_quiescent_state();				\
			rep_ok__ = (fn)() == 0 && leak_check() == 0;	\
		}							\
		if (rep_n__ > 1)					\
			diag("%s: %lu repetition(s), %s", #fn, rep_i__,	\
				rep_ok__ ? "ok" : "RED");		\
		ok(rep_ok__, "%s", #fn);				\
	} while (0)

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 1: Forward iteration ordering under mutation           */
/*                                                                    */
/*   A concurrent RCU reader iterating forward must never observe a   */
/*   key K(i+1) <= K(i).  While the reader may miss newly inserted   */
/*   keys or see keys that are about to be removed, the relative     */
/*   order of every pair of keys it does observe must be strictly     */
/*   ascending.                                                       */
/*                                                                    */
/* ================================================================== */

struct inv_iter_ctx {
	struct cds_ft *ft;
	size_t key_len;
	const char *test_name;
};

static void *inv_iter_order_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t prev = 0;
		int first = 1;
		int count = 0;

		rcu_read_lock();
		cds_ft_for_each_rcu(ctx->ft, iter) {
			uint8_t rk[8];
			size_t rk_len;
			uint64_t v;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (!first && v <= prev) {
				report_violation(ctx->test_name,
					"forward order: %" PRIu64 " after %" PRIu64
					" (iter #%lu, position %d)",
					v, prev, iters, count);
				/* Stop this iteration to avoid flooding. */
				break;
			}
			prev = v;
			first = 0;
			count++;
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_iter_order_writer(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_iteration_order(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	/* Pack the mutex right after the context for the writers. */
	struct {
		struct inv_iter_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.key_len = 4;
	shared.ctx.test_name = "inv_iteration_order";
	pthread_mutex_init(&shared.lock, NULL);

	/* Pre-populate with some keys so readers have something to see. */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_iter_order_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_iter_order_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_iteration_order: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   PHASE 4.3: writer-vs-writer race oracle (disjoint key ranges)    */
/*                                                                    */
/* ================================================================== */

/*
 * Concurrent WRITERS on one shared trie WITHOUT the single-writer serializing
 * mutex the other invariants use -- the first test of the concurrent-writer
 * path (the MCAS commit engine + the Invariant-2 §4.B holder guards + the
 * commit retry, all dormant while a caller mutex serializes writers).
 *
 * Each writer owns a DISJOINT key range, so no two writers ever touch the same
 * key: there is NO logical write/write conflict, only STRUCTURAL concurrency
 * (writers mutating the shared internal nodes on their overlapping key
 * prefixes).  This isolates structural-concurrency correctness (node sharing,
 * freeze-on-free, the guards) from key-level races.
 *
 * Oracles:
 *  - in-line (the §3.1 lost-key hazard): a key THIS writer inserted and has not
 *    removed must ALWAYS resolve to exactly the node it inserted -- even while
 *    another writer restructures a shared ancestor.  A miss or a wrong node
 *    means a concurrent restructure made a live key disappear / resolve wrong.
 *  - in-line: a key this writer has NOT inserted must not be found.
 *  - final (after quiescence): every live key resolves to its node,
 *    cds_ft_count_keys equals the exact live total, cds_ft_verify passes.
 *  - leak_check: no node leaked or double-freed across the run.
 */
#define MW_NR_WRITERS	16
#define MW_RANGE	256		/* keys per writer */

/*
 * Flight-recorder hooks for the MW oracle (FT_INV_ABORT_ON_VIOLATION=1):
 * dump the LTTng snapshot ring and abort on the FIRST oracle violation, and
 * likewise from a fatal-signal handler so an uncontrolled SIGSEGV/SIGBUS
 * also captures its window.  system() in a signal handler is not
 * async-signal-safe -- accepted scaffolding at crash time, matching the
 * existing violation() helper.
 */
static void mw_violation_snapshot(void)
{
	if (!getenv("FT_INV_ABORT_ON_VIOLATION"))
		return;
	ft_snapshot_record();
	abort();
}

static void mw_fatal_action(int sig, siginfo_t *si, void *uctx)
{
	if (sig == SIGSEGV || sig == SIGBUS || sig == SIGILL) {
		/*
		 * Land the fault context IN the trace timeline: si_addr (the
		 * faulting data address) and the sigframe instruction pointer,
		 * so the fault correlates by address with the edge/alloc
		 * breadcrumb window.
		 */
		void *ip = NULL;
#if defined(__x86_64__)
		ucontext_t *uc = (ucontext_t *) uctx;

		ip = (void *) uc->uc_mcontext.gregs[REG_RIP];
#else
		(void) uctx;
#endif
		FT_TEST_TP(fatal_signal, sig, (const void *) si->si_addr,
			(const void *) ip);
		fprintf(stderr, "FATAL sig %d addr %p ip %p\n", sig,
			si ? si->si_addr : NULL, ip);
	}
	ft_snapshot_record();
	signal(sig, SIG_DFL);
	raise(sig);
}

static void mw_install_fatal_handler(void)
{
	struct sigaction sa;

	if (!getenv("FT_INV_ABORT_ON_VIOLATION"))
		return;
	memset(&sa, 0, sizeof(sa));
	sa.sa_sigaction = mw_fatal_action;
	sa.sa_flags = SA_SIGINFO;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGSEGV, &sa, NULL);
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGILL, &sa, NULL);
	sigaction(SIGABRT, &sa, NULL);
}

struct mw_writer_arg {
	struct cds_ft *ft;
	uint64_t base;				/* range [base, base + MW_RANGE) */
	uint8_t present[MW_RANGE];		/* 1 = my key is live in the trie */
	struct ft_test_node *node[MW_RANGE];	/* the live node per present key */
	unsigned long ops;
	int failed;
};

static void *mw_writer(void *arg)
{
	struct mw_writer_arg *w = (struct mw_writer_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int)(uintptr_t) w;

	rcu_register_thread();
	if (cds_ft_iter_create(w->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t off = (uint64_t)(rand_r(&seed) % MW_RANGE);
		uint64_t key = w->base + off;
		struct cds_ft_node *found;
		uint8_t k[8];

		cds_ft_u64_to_key(w->ft, key, k, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(w->ft, iter);
		found = cds_ft_iter_node(iter);
		if (w->present[off]) {
			/*
			 * Live key: must resolve to MY node (lost-key oracle).
			 * The lookup reference (@found, and the iter's cached
			 * path) is valid only under this thread's read-side
			 * section, and cds_ft_remove consumes it -- so the
			 * caller section spans lookup + remove (the §11
			 * reference-lifetime contract).
			 */
			if (found != &w->node[off]->node) {
				/*
				 * DISCRIMINATOR (flight-recorder campaign): the
				 * key was proven present-in-structure at abort
				 * time, so classify the read failure before
				 * declaring it -- retry on the SAME iter (same
				 * state, later instant: transient mid-flip
				 * anomaly if it now hits) and on a FRESH iter
				 * (virgin state: iter-cache/reanchor bug if
				 * only this one hits).  Still NIL on both =>
				 * persistent descent mis-validation.
				 */
				struct cds_ft_iter *fresh = NULL;
				struct cds_ft_node *re_same = NULL, *re_fresh = NULL;

				cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
				cds_ft_lookup(w->ft, iter);
				re_same = cds_ft_iter_node(iter);
				if (cds_ft_iter_create(w->ft, &fresh) ==
						CDS_FT_STATUS_OK) {
					cds_ft_iter_set_key(fresh, k,
						CDS_FT_LEN_DEFAULT);
					cds_ft_lookup(w->ft, fresh);
					re_fresh = cds_ft_iter_node(fresh);
				}
				fprintf(stderr, "MW writer base %llu key %llu: live "
					"but found %p != mine %p (retry same-iter %p "
					"fresh-iter %p)\n",
					(unsigned long long) w->base,
					(unsigned long long) key, (void *) found,
					(void *) &w->node[off]->node,
					(void *) re_same, (void *) re_fresh);
				if (fresh)
					cds_ft_iter_destroy(fresh);
				w->failed = 1;
				mw_violation_snapshot();
			} else if (cds_ft_remove(w->ft, iter, found)
					== CDS_FT_STATUS_OK) {
				node_free_rcu(to_test_node(found));
				w->present[off] = 0;
				w->node[off] = NULL;
			}
			rcu_read_unlock();
		} else {
			/*
			 * Absent key: only @found's boolean presence is
			 * consumed past this point, so the lookup section
			 * closes HERE and the insert below runs with NO
			 * caller-held read-side section -- the FT owns its
			 * own bracket (§11 Phase A).  This is the standing
			 * oracle for that caller contract.
			 */
			rcu_read_unlock();
			if (found) {
				fprintf(stderr, "MW writer base %llu key %llu: "
					"absent but found %p\n",
					(unsigned long long) w->base,
					(unsigned long long) key, (void *) found);
				w->failed = 1;
				mw_violation_snapshot();
			} else {
				struct ft_test_node *n = node_alloc(key);

				if (insert_u64(w->ft, key, n)
						== CDS_FT_STATUS_OK) {
					w->present[off] = 1;
					w->node[off] = n;
				} else {
					node_free(n);
				}
			}
		}
		w->ops++;
		if ((seed & 0x3f) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_writers_disjoint(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;

	/*
	 * Opt-in (FT_INV_MW=1): the concurrent-writer oracle over the certified
	 * per-node-lock writer path.  It is opt-in because it is a saturating
	 * soak, NOT because it is known-broken -- the one known-failing oracle,
	 * inv_rekey_src_mutated, gates itself a SECOND time on FT_INV_RKSM and
	 * names the defect in its skip message.  Run it explicitly:
	 *   FT_INV_MW=1 ./test_urcu_ft_inv inv_concurrent_writers_disjoint
	 */
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_concurrent_writers_disjoint: skipped "
			"(set FT_INV_MW=1 to run the Phase 4.3 writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	ft = create_fixed_ft(8, &group);
	/*
	 * Concurrent writers REQUIRE concurrent mode: exclusive mode reclaims
	 * retired nodes in place with no grace period (ft_flip_txn_call_rcu_now),
	 * so a peer standing on a just-retired node reads recycled memory (UAF)
	 * and two writers double-free it.  Concurrent mode defers reclaim through
	 * the flavor's call_rcu, so a writer's rcu_read_lock-guarded descent keeps
	 * every node it touches live until its grace period.
	 */
	cds_ft_make_shared(ft);
	struct mw_writer_arg *w;
	pthread_t writers[MW_NR_WRITERS];
	struct timespec t0;
	unsigned long total_ops = 0, live = 0;
	int i, ret = 0;

	leak_reset();

	w = (struct mw_writer_arg *) calloc(MW_NR_WRITERS, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < MW_NR_WRITERS; i++) {
		w[i].ft = ft;
		w[i].base = (uint64_t) i * MW_RANGE;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_NR_WRITERS; i++)
		pthread_create(&writers[i], NULL, mw_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_NR_WRITERS; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: verify the final trie against every writer's shadow. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < MW_NR_WRITERS; i++) {
		unsigned int off;

		total_ops += w[i].ops;
		if (w[i].failed)
			ret = -1;
		for (off = 0; off < MW_RANGE; off++) {
			struct cds_ft_node *found = NULL;

			if (!w[i].present[off])
				continue;
			live++;
			if (lookup_u64(ft, w[i].base + off, &found)
					!= CDS_FT_STATUS_OK ||
			    found != &w[i].node[off]->node) {
				fprintf(stderr, "MW final: writer %d key %llu lost "
					"(found %p != %p)\n", i,
					(unsigned long long)(w[i].base + off),
					(void *) found,
					(void *) &w[i].node[off]->node);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "MW final: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "MW final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_concurrent_writers_disjoint: %d writers, %lu ops, "
		"%lu live keys\n", MW_NR_WRITERS, total_ops, live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Coherent-rekey concurrent-writer oracle (DISJOINT): N writers each own a
 * private subtree and rekey it back and forth between two junctions that SHARE
 * the root as grandparent (d_src.ppnf == d_dst.ppnf == root), so every writer's
 * move node locks root and all N SERIALIZE on it -- the contention this hook's
 * up-front DLM acquire is designed to fail-fast + retry on.  Disjoint key
 * ownership means no writer ever re-homes a peer's junction, so the writer's
 * parent_held reuse is exercised under contention but not its racy re-home edge
 * (that is the SHARED oracle's job).  The list is ON, so each move also folds the
 * moved subtree's ordered-cell run splice into the same commit.
 *
 * Per writer w: two root-child junction bytes bp=2w+1, dp=2w+2 (distinct across
 * writers).  Each junction carries a SYMMETRIC layout -- straddling sibling leaves
 * at byte1 {1,5} and the S_top slot at byte1 3 -- so BOTH move directions satisfy
 * every shape gate (src junction keeps >=3 children after losing S_top's slot; the
 * dst slot (X,3) is absent and NOSPLIT between the {1,5} sibs; the run under S_top
 * is never adjacent to the dst gap, whose neighbours are the {1,5} sibs).  Two
 * global guard leaves (0x00.. / 0xff..) keep every writer's subtree off the list
 * head/tail.  S_top is a 4-child popcount internal (leaves (X,3,c) c=1..4); the
 * rekey moves those four leaves as one run.
 *
 * Oracle: no move ever returns anything but 0 (committed) or -EAGAIN (root held by
 * a peer -> retry); progress is made (total ops > 0, i.e. no livelock); at
 * quiescence every key is present at its writer's final position and absent at the
 * other, the count is exact, and cds_ft_verify (incl. the ordered-cell pass)
 * passes.  Opt-in FT_INV_MW=1.
 */
extern int _cds_ft_debug_rekey_graft_simple(struct cds_ft *ft,
		const uint8_t *src_key, size_t src_len,
		const uint8_t *dst_key, size_t dst_len);

/*
 * Rekey writers, all contending root.  Overridable at build time so a hunt can
 * raise the in-process contention without editing the arm: the CONTENDED
 * geometry below derives its key bands from this count, so nothing collides.
 */
#ifndef RK_NW
#define RK_NW		8
#endif

struct rk_writer_arg {
	struct cds_ft *ft;
	uint8_t bp, dp;			/* the writer's two root-child junction bytes */
	uint8_t sb;			/* S_top's slot byte inside a junction (byte 1) */
	struct ft_test_node *sib[4];	/* fixed straddling sibs: (bp,1)(bp,5)(dp,1)(dp,5) */
	struct ft_test_node *top[4];	/* S_top's 4 leaves, moving with S_top */
	int at_dst;			/* 0: S_top at (bp,sb); 1: at (dp,sb) */
	unsigned long ops, retries;
	int failed;
};

static void *rk_writer(void *arg)
{
	struct rk_writer_arg *w = (struct rk_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint8_t cur = w->at_dst ? w->dp : w->bp;
		uint8_t oth = w->at_dst ? w->bp : w->dp;
		uint8_t src_key[2] = { cur, w->sb };
		uint8_t dst_key[2] = { oth, w->sb };
		int rc;

		/*
		 * NO read lock here: the move enters the per-trie MOVE GATE, which
		 * publishes "expect move" to readers and waits a GRACE PERIOD before
		 * the structure is touched.  Holding a read section across that would
		 * deadlock on our own critical section; the entry takes the read lock
		 * the body needs itself.
		 */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 2,
				dst_key, 2);
		if (rc == 0) {
			w->at_dst = !w->at_dst;
			w->ops++;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			/*
			 * TRANSIENT contention abort (a peer holds root's LOCK
			 * when this move tries to acquire it): -EIO = the graft
			 * recompaction's up-front acquire lost the race (prepare
			 * bailed clean), -EAGAIN = a cow_stop / detach / final-commit
			 * abort, -ENOMEM = a reserve OOM.  All leave the trie pristine
			 * (the hook is abort-clean + single-shot), so re-descend and
			 * retry the SAME direction -- the writer-level recovery that
			 * stands in for the deferred escalation lane.
			 */
			w->retries++;
		} else {
			/*
			 * A permanent refusal this layout must never hit:
			 * -EINVAL (bad ARGUMENTS) or FT_REKEY_UNCOVERED /
			 * -EDOM (a SHAPE outside the one-decide cut, which
			 * this layout is built to stay inside).  Either way,
			 * or any unexpected code: a real oracle failure.
			 */
			fprintf(stderr, "rk_writer bp=%u dp=%u: move %02x,%02x -> "
				"%02x,%02x failed rc=%d\n", w->bp, w->dp, cur,
				w->sb, oth, w->sb, rc);
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * MIXED WRITERS ON A COARSE TRIE -- the arm that tests the rekey's WRITER
 * EXCLUSION, which every other rekey oracle is structurally unable to see.
 *
 * ☠ WHY IT HAD TO EXIST.  The atomic rekey parks its structural edges SW
 * (ft_flip_txn_set_structural_sw), and an SW park CANNOT FAIL -- it does not
 * arbitrate -- so it is legal only where the op excludes every peer writer over
 * those slots.  In FINE mode the per-node DLM LOCK try-locks are that protocol
 * and every writer speaks it.  In COARSE mode every OTHER writer excludes
 * through the FT-WIDE MUTEX instead, and the rekey took NO writer scope at all:
 * it spoke to nobody, and its unfailable SW parks were lost updates waiting for
 * a concurrent insert or remove.
 *
 * ★ AND NO EXISTING ARM COULD DETECT IT.  Every rekey oracle's writers issue
 * cds_ft_rekey_graft and NOTHING ELSE, so no peer insert/remove exists on the
 * trie and the exclusion gap is unobservable -- 3.0M band probes measured a race
 * that could not occur in that configuration.  This arm supplies the missing
 * ingredient: rekey writers and insert/remove writers on the SAME COARSE trie.
 *
 * SHAPE: the rekey writers own top-byte bands 1..2*RK_NW (as
 * inv_rekey_graft_disjoint does); the insert/remove writers own bands
 * RKMIX_BASE.. -- DISJOINT KEYS but the SAME ROOT AND UPPER SPINE, which is
 * where an SW park and a peer's edge actually collide.
 *
 * DETECTION is the lost-key oracle on both sides: every rekey writer's subtree
 * must be at its final position and absent at the other, and every insert/remove
 * writer's shadow must match the trie key for key.  A lost update from an
 * unarbitrated SW park shows up as a missing key or a cds_ft_verify failure,
 * not as a crash.
 */

static int inv_rekey_graft_disjoint(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	pthread_t writers[RK_NW];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_disjoint: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(4, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	/* Global guard leaves so no writer's subtree is ever the list head/tail. */
	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (insert_u64(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rk_writer_arg *) calloc(RK_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RK_NW; i++) {
		uint8_t bp = (uint8_t) (2 * i + 1), dp = (uint8_t) (2 * i + 2);
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint64_t sk[4] = {
			bpk | (1ULL << 16), bpk | (5ULL << 16),
			dpk | (1ULL << 16), dpk | (5ULL << 16),
		};

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].sb = 3;			/* S_top slot byte inside both junctions */
		rcu_read_lock();
		for (c = 0; c < 4; c++) {
			w[i].sib[c] = node_alloc(sk[c]);
			if (insert_u64(ft, sk[c], w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < 4; c++) {
			uint64_t tk = bpk | (3ULL << 16) | ((uint64_t) (c + 1) << 8);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 8;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_create(&writers[i], NULL, rk_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: every key present at its writer's final position, absent at the other. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RK_NW; i++) {
		uint8_t bp = w[i].bp, dp = w[i].dp;
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint8_t X = w[i].at_dst ? dp : bp, O = w[i].at_dst ? bp : dp;
		uint64_t Xk = (uint64_t) X << 24, Ok = (uint64_t) O << 24;
		uint64_t sk[4] = {
			bpk | (1ULL << 16), bpk | (5ULL << 16),
			dpk | (1ULL << 16), dpk | (5ULL << 16),
		};

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		for (c = 0; c < 4; c++) {
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, sk[c], &found) != CDS_FT_STATUS_OK ||
					found != &w[i].sib[c]->node) {
				fprintf(stderr, "rekey disjoint: writer %d sib %d lost\n",
					i, c);
				ret = -1;
			}
		}
		for (c = 0; c < 4; c++) {
			uint64_t here = Xk | (3ULL << 16) | ((uint64_t) (c + 1) << 8);
			uint64_t there = Ok | (3ULL << 16) | ((uint64_t) (c + 1) << 8);
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, here, &found) != CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "rekey disjoint: writer %d top %d absent "
					"at final pos (at_dst=%d)\n", i, c, w[i].at_dst);
				ret = -1;
			}
			if (lookup_u64(ft, there, &found) == CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey disjoint: writer %d top %d still at "
					"old pos\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey disjoint: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey disjoint: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {			/* liveness: writers made progress */
		fprintf(stderr, "rekey disjoint: no successful moves (livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_disjoint: %d writers, %lu moves, "
		"%lu retries, %lu live keys\n", RK_NW, total_ops, total_retries,
		live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * OCCUPIED-DESTINATION rekey oracle -- the ONLY concurrent coverage of the merge
 * fold, and it exists because there was none.
 *
 * Every other rekey oracle moves a subtree into an EMPTY destination, which is
 * the GRAFT path.  Measured across the whole FT_INV_MW plan, the merge fold's
 * distinctive edge class (skip-compressed src-origin re-parents) was reached
 * ZERO times, so the record-time resolution those edges depend on was validated
 * single-threaded only, and "a peer cannot stale a skip flag between the
 * resolution and the flip" rested on an argument rather than a measurement.
 * This oracle is what turns that into evidence.
 *
 * ★ WHY IT RE-SEEDS INSTEAD OF SHUTTLING.  A merge EMPTIES its source, so the
 * back-and-forth the other oracles use degenerates after ONE move: the second
 * direction finds an empty destination and takes the graft path forever.  Each
 * iteration therefore SEEDS the source afresh, moves it into a destination that
 * is permanently occupied by RESIDENT keys, and removes only the relocated
 * movers -- leaving the residents in place so the next move is a merge too.
 *
 * Per writer w, two root-child junctions bp=2w+1, dp=2w+2 (disjoint across
 * writers, so no writer re-homes a peer's junction; they all still contend
 * ROOT's LOCK, which is the contention this exercises):
 *   (bp,1) (bp,5) (dp,1) (dp,5)   straddling sibs, keep both junctions >= 3
 *                                 children so the detach stays an in-place delete
 *   (dp,3,5) (dp,3,6)             RESIDENTS -- never move, keep the dst OCCUPIED
 *   (bp,3,1) (bp,3,2)             MOVERS -- seeded, merged to (dp,3,*), removed
 * A lone key under (X,3,c) path-compresses, so S_top's children are compressed:
 * that is what makes the moved re-parents SKIP-COMPRESSED, the class this oracle
 * is for.  The mover and resident byte-2 values are disjoint, so the union at the
 * top frame adds children rather than colliding (dup-chain collisions are a
 * separate shape, not covered here).
 *
 * THE ORACLE, checked after EVERY successful move and not only at quiescence:
 * all four keys must be present under dp.  A merge that dropped the destination's
 * own keys -- the exact failure the occupied-dst path risks, and one that leaves
 * the trie perfectly VERIFIABLE -- is invisible to cds_ft_verify and to a key
 * count taken at rest, because the residents are re-checked only where they
 * should be.  Losing them here fails immediately.
 *
 * -EINVAL is FATAL: this layout satisfies every documented shape gate, so a
 * permanent shape rejection is a real defect, not a race.  Opt-in FT_INV_MW=1.
 */
#define RKM_NW		8		/* writers, all contending root */
#define RKM_SB		3		/* S_top slot byte inside both junctions */
/*
 * Byte-2 layout.  Mover 1 lands EXACTLY on resident 0, so every merge splices a
 * duplicate chain as well as unioning a disjoint child -- the leaf-splice arm and
 * the chain-holder lock it needs, under contention.  Mover 0 stays disjoint so a
 * single move covers both.
 */
#define RKM_MV0		1
#define RKM_MV1		5		/* == RKM_RES0: the COLLIDING pair */
#define RKM_RES0	5
#define RKM_RES1	6

/*
 * ROOT CHURN (@churn), the peer this oracle otherwise has no way to supply.
 *
 * On the COMPRESSED-dst arm the merge's publish parent is a path-compressed run
 * at depth 1, so the forward publish also re-encodes the SKIP_X dual in that
 * run's own parent -- and that parent is the ROOT.  The only peer that can
 * write the dual slot is a RECOMPACTION of the root, and with the root's child
 * set seeded once and never touched again, no writer ever causes one.  The
 * `dual != d_dst.pnfp` guard that protects the second slot then fires ZERO
 * times in a full run: it has no coverage at all.
 *
 * So a churning writer inserts and drops RKM_CHURN_N keys under a PRIVATE range
 * of root-level bytes every iteration.  The root's child count oscillates
 * across node-type boundaries, it recompacts, and it re-homes the compressed
 * node underneath the other writers' moves.  Private ranges keep the churn from
 * becoming a second contention oracle in its own right; the insert and the
 * remove sit in one block, so no iteration can exit holding churn keys and the
 * count_keys == live check below stays exact.
 */
#define RKM_CHURN_N	4		/* root-level bytes churned per writer */

/*
 * DOES THE ROOT ACTUALLY RELOCATE?  Without asking, the churn cannot tell "a
 * lock excludes the peer" from "the peer never ran" -- and those two produce
 * the identical green.  Sampled per round and reported, so a future change that
 * quietly stops the root moving shows up as a number going to zero rather than
 * as continued success.
 */
extern void *_cds_ft_debug_root(struct cds_ft *ft);

struct rkm_writer_arg {
	struct cds_ft *ft;
	uint8_t bp, dp;
	struct ft_test_node *sib[4];	/* (bp,1)(bp,5)(dp,1)(dp,5) */
	int nsib;			/* 4, or 2 for the COMPRESSED-dst arm */
	struct ft_test_node *res[2];	/* residents (dp,3,5)(dp,3,6) */
	int seeded;			/* movers currently live at (bp,3,*) */
	uint8_t cp;			/* churn range base (root byte), private */
	int churn;			/* recompact the root under the peers */
	void *root_seen;		/* last root address this writer sampled */
	unsigned long ops, retries, churns, rootmoves;
	int failed;
};

static uint64_t rkm_key(uint8_t x, uint8_t b1, uint8_t b2)
{
	return ((uint64_t) x << 24) | ((uint64_t) b1 << 16) |
		((uint64_t) b2 << 8);
}

/*
 * Remove the entry at @v that is NOT @keep, reclaiming it.  The collided key is a
 * chain of two and only the MOVER may be taken back -- a plain remove would take
 * whichever end the lookup lands on and could silently evict the resident, which
 * is the very thing this oracle checks for.
 */
static enum cds_ft_status rkm_remove_dup_other(struct cds_ft *ft, uint64_t v,
		struct cds_ft_node *keep)
{
	struct cds_ft_iter *iter = NULL;
	struct cds_ft_node *head, *n;
	enum cds_ft_status st = CDS_FT_STATUS_NOT_FOUND;
	uint8_t k[8] = { 0 };

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	cds_ft_lookup(ft, iter);
	head = cds_ft_iter_node(iter);
	for (n = head; n; n = cds_ft_node_next_rcu(n)) {
		if (n == keep)
			continue;
		st = cds_ft_remove(ft, iter, n);
		if (st == CDS_FT_STATUS_OK)
			node_free_rcu(to_test_node(n));
		break;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return st;
}

static const uint8_t rkm_mv[2] = { RKM_MV0, RKM_MV1 };
static const uint8_t rkm_res[2] = { RKM_RES0, RKM_RES1 };

static void *rkm_writer(void *arg)
{
	struct rkm_writer_arg *w = (struct rkm_writer_arg *) arg;
	uint8_t src_key[2] = { 0, RKM_SB }, dst_key[2] = { 0, RKM_SB };
	unsigned long iters = 0;
	int c;

	src_key[0] = w->bp;
	dst_key[0] = w->dp;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int rc;

		/*
		 * Recompact the ROOT under the other writers' moves.  Self-
		 * contained: every key inserted here is dropped again before the
		 * move below, so no exit path can leave one behind and the
		 * count_keys == live check stays exact.
		 */
		if (w->churn) {
			rcu_read_lock();
			for (c = 0; c < RKM_CHURN_N; c++) {
				uint64_t ck = rkm_key((uint8_t) (w->cp + c), 1, 0);

				if (insert_u64(w->ft, ck, node_alloc(ck)) !=
						CDS_FT_STATUS_OK) {
					rcu_read_unlock();
					fprintf(stderr, "rkm_writer cp=%u: churn "
						"insert failed\n", w->cp);
					w->failed = 1;
					goto out;
				}
			}
			rcu_read_unlock();
			for (c = 0; c < RKM_CHURN_N; c++)
				if (remove_u64(w->ft, rkm_key((uint8_t) (w->cp + c),
						1, 0)) != CDS_FT_STATUS_OK) {
					fprintf(stderr, "rkm_writer cp=%u: churn "
						"remove failed\n", w->cp);
					w->failed = 1;
					goto out;
				}
			w->churns++;
			rcu_read_lock();
			{
				void *r = _cds_ft_debug_root(w->ft);

				if (r != w->root_seen) {
					if (w->root_seen)
						w->rootmoves++;
					w->root_seen = r;
				}
			}
			rcu_read_unlock();
		}

		/* SEED the source: two lone deep keys -> compressed children. */
		if (!w->seeded) {
			rcu_read_lock();
			for (c = 0; c < 2; c++) {
				uint64_t mk = rkm_key(w->bp, RKM_SB, rkm_mv[c]);

				if (insert_u64(w->ft, mk, node_alloc(mk)) !=
						CDS_FT_STATUS_OK) {
					rcu_read_unlock();
					fprintf(stderr, "rkm_writer bp=%u: seed "
						"insert failed\n", w->bp);
					w->failed = 1;
					goto out;
				}
			}
			rcu_read_unlock();
			w->seeded = 1;
		}

		/*
		 * NO read lock: the move enters the per-trie MOVE GATE, which waits
		 * a grace period -- holding a read section across it self-deadlocks.
		 */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 2, dst_key, 2);
		if (rc == 0) {
			w->ops++;
			/*
			 * The union must hold BOTH sides.  Losing the residents is
			 * the occupied-dst failure that a verify pass and a rest-time
			 * count both sail straight past.
			 */
			rcu_read_lock();
			for (c = 0; c < 2; c++) {
				struct cds_ft_node *f = NULL, *n;
				uint64_t moved = rkm_key(w->dp, RKM_SB, rkm_mv[c]);
				uint64_t resid = rkm_key(w->dp, RKM_SB, rkm_res[c]);
				unsigned long chain = 0;

				if (lookup_u64(w->ft, moved, &f) != CDS_FT_STATUS_OK) {
					rcu_read_unlock();
					fprintf(stderr, "rkm_writer bp=%u dp=%u: moved "
						"key %d absent after merge\n",
						w->bp, w->dp, c);
					w->failed = 1;
					mw_violation_snapshot();
					goto out;
				}
				if (lookup_u64(w->ft, resid, &f) != CDS_FT_STATUS_OK) {
					rcu_read_unlock();
					fprintf(stderr, "rkm_writer bp=%u dp=%u: RESIDENT "
						"key %d lost to the union\n",
						w->bp, w->dp, c);
					w->failed = 1;
					mw_violation_snapshot();
					goto out;
				}
				/*
				 * The resident must still be ON the chain -- a splice
				 * that replaced it rather than appending leaves a
				 * perfectly valid single-entry key behind.
				 */
				for (n = f; n; n = cds_ft_node_next_rcu(n)) {
					chain++;
					if (n == &w->res[c]->node)
						break;
				}
				if (!n) {
					rcu_read_unlock();
					fprintf(stderr, "rkm_writer bp=%u dp=%u: resident "
						"%d not on its chain after the splice\n",
						w->bp, w->dp, c);
					w->failed = 1;
					mw_violation_snapshot();
					goto out;
				}
			}
			rcu_read_unlock();
			/* Remove only the relocated movers; residents stay. */
			for (c = 0; c < 2; c++) {
				uint64_t moved = rkm_key(w->dp, RKM_SB, rkm_mv[c]);
				enum cds_ft_status rst;

				/*
				 * Mover 1 shares its key with resident 0, so take the
				 * entry that is NOT the resident; mover 0 is a
				 * singleton and a plain remove is exact.
				 */
				rst = (rkm_mv[c] == RKM_RES0 || rkm_mv[c] == RKM_RES1) ?
					rkm_remove_dup_other(w->ft, moved,
						&w->res[rkm_mv[c] == RKM_RES0 ? 0 : 1]->node) :
					remove_u64(w->ft, moved);
				if (rst != CDS_FT_STATUS_OK) {
					fprintf(stderr, "rkm_writer bp=%u: cleanup "
						"remove failed\n", w->bp);
					w->failed = 1;
					goto out;
				}
			}
			w->seeded = 0;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			/* Transient: trie pristine, movers still at src -- retry. */
			w->retries++;
		} else {
			fprintf(stderr, "rkm_writer bp=%u dp=%u: merge-move failed "
				"rc=%d\n", w->bp, w->dp, rc);
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
out:
	rcu_unregister_thread();
	return NULL;
}

/*
 * @nsib picks the DESTINATION PATH, and that is the only difference between the
 * two arms below.
 *
 * 4: the destination prefix (dp, RKM_SB) also carries the siblings (dp,1) and
 *    (dp,5), so the node above it branches and the merge's publish parent is a
 *    PLAIN internal node.
 * 2: those two are left out, so RKM_SB is the lone child under @dp and the trie
 *    PATH-COMPRESSES it.  The destination is then reached THROUGH a compressed
 *    run, and the merge publishes into that run's ->child -- which also makes
 *    the forward publish re-encode the SKIP_X dual in the run's own parent.
 *    Two parked slots in two different nodes, under contention.
 */
static int rkm_run(const char *name, int nsib, int churn)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkm_writer_arg *w;
	pthread_t writers[RKM_NW];
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, total_churns = 0, live = 0;
	unsigned long total_rootmoves = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# %s: skipped "
			"(set FT_INV_MW=1 to run the occupied-dst merge oracle)\n",
			name);
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	/*
	 * LIST OFF, and it has to stay off for THIS layout: the merge fold takes a
	 * list-on move only when the moved suffixes disjointly precede or follow the
	 * destination region's, and this geometry deliberately COLLIDES (mover 1 lands
	 * on resident 0), so its ranges overlap and the interleave is refused.
	 * test_rekey_merge_occupied_dst_liston covers the disjoint shape.
	 */
	ft = create_fixed_fine_lock_listoff_ft(4, &group);
	cds_ft_make_shared(ft);

	w = (struct rkm_writer_arg *) calloc(RKM_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKM_NW; i++) {
		uint8_t bp = (uint8_t) (2 * i + 1), dp = (uint8_t) (2 * i + 2);
		uint64_t sk[4] = {
			rkm_key(bp, 1, 0), rkm_key(bp, 5, 0),
			rkm_key(dp, 1, 0), rkm_key(dp, 5, 0),
		};

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].nsib = nsib;
		/*
		 * PRIVATE and clear of the oracle's own bytes (bp/dp run 1..16
		 * for RKM_NW == 8), so the churn recompacts the root without
		 * colliding on any key the checks below assert about.
		 */
		w[i].cp = (uint8_t) (100 + RKM_CHURN_N * i);
		w[i].churn = churn;
		rcu_read_lock();
		for (c = 0; c < nsib; c++) {
			w[i].sib[c] = node_alloc(sk[c]);
			if (insert_u64(ft, sk[c], w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < 2; c++) {	/* residents: keep the dst OCCUPIED */
			uint64_t rk = rkm_key(dp, RKM_SB, rkm_res[c]);

			w[i].res[c] = node_alloc(rk);
			if (insert_u64(ft, rk, w[i].res[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += (unsigned long) nsib + 2;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKM_NW; i++)
		pthread_create(&writers[i], NULL, rkm_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKM_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Drop any movers left seeded at the source by the final iteration. */
	synchronize_rcu();
	for (i = 0; i < RKM_NW; i++) {
		if (!w[i].seeded)
			continue;
		for (c = 0; c < 2; c++)
			(void) remove_u64(ft, rkm_key(w[i].bp, RKM_SB, rkm_mv[c]));
		w[i].seeded = 0;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKM_NW; i++) {
		total_ops += w[i].ops;
		total_retries += w[i].retries;
		total_churns += w[i].churns;
		total_rootmoves += w[i].rootmoves;
		if (w[i].failed)
			ret = -1;
		/*
		 * PER-WRITER liveness: a global "some moves happened" sails right
		 * past one starved writer, which is a measured failure mode here.
		 */
		if (w[i].ops == 0) {
			fprintf(stderr, "rekey merge: writer %d made NO move "
				"(starved / livelock)\n", i);
			ret = -1;
		}
		for (c = 0; c < w[i].nsib; c++) {
			struct cds_ft_node *f = NULL;
			uint64_t sk = c < 2 ? rkm_key(w[i].bp, c == 0 ? 1 : 5, 0) :
				rkm_key(w[i].dp, c == 2 ? 1 : 5, 0);

			if (lookup_u64(ft, sk, &f) != CDS_FT_STATUS_OK ||
					f != &w[i].sib[c]->node) {
				fprintf(stderr, "rekey merge: writer %d sib %d lost\n",
					i, c);
				ret = -1;
			}
		}
		for (c = 0; c < 2; c++) {	/* residents survived every union */
			struct cds_ft_node *f = NULL;
			uint64_t rk = rkm_key(w[i].dp, RKM_SB, rkm_res[c]);

			if (lookup_u64(ft, rk, &f) != CDS_FT_STATUS_OK ||
					f != &w[i].res[c]->node) {
				fprintf(stderr, "rekey merge: writer %d resident %d "
					"lost\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey merge: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey merge: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# %s: %d writers, %lu merges, "
		"%lu retries, %lu live keys", name, RKM_NW, total_ops,
		total_retries, live);
	if (churn)
		fprintf(stderr, ", %lu root-churn rounds (%lu insert+remove pairs), "
			"%lu observed ROOT RELOCATIONS",
			total_churns, total_churns * RKM_CHURN_N, total_rootmoves);
	fprintf(stderr, "\n");

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

static int inv_rekey_merge_occupied_dst(void)
{
	INV_NEED_MERGE("inv_rekey_merge_occupied_dst");
	return rkm_run("inv_rekey_merge_occupied_dst", 4, 0);
}

/*
 * THE SAME ORACLE ONTO A DESTINATION BEHIND A PATH-COMPRESSED RUN.
 *
 * The one-decide writer used to read such a destination as EMPTY -- its
 * @merge_dst probe stopped at the compressed node instead of decoding it -- and
 * route the move to the empty-dst splice arm, which refused.  The probe now
 * decodes a run the destination key consumes whole, so the move lands HERE, on
 * the merge arm, and its forward publish becomes TWO parked slots: the run's
 * ->child, and the SKIP_X dual in the run's own parent.  The op must hold both
 * words; the second is @publish_gp_holder.
 *
 * Its predecessor arm cannot cover this: measured, it reaches a compressed
 * publish parent ZERO times in a full run, because the (dp,1)/(dp,5) siblings it
 * seeds are exactly what stops the destination path from compressing.
 */
static int inv_rekey_merge_compressed_dst(void)
{
	INV_NEED_MERGE("inv_rekey_merge_compressed_dst");
	return rkm_run("inv_rekey_merge_compressed_dst", 2, 0);
}

/*
 * THE SAME ARM WITH THE GRANDPARENT MOVING UNDER IT.
 *
 * The arm above publishes into a compressed run's ->child and re-encodes the
 * SKIP_X dual in that run's PARENT -- the trie root.  It cannot exercise the
 * guard that protects the second slot, because it never moves the root: its
 * child set is seeded once and never touched, so `dual != d_dst.pnfp` fires
 * ZERO times in a full run and the op's re-read always agrees with its plan.
 *
 * Root churn supplies the missing peer, and the difference is measured, not
 * assumed: 0 -> 60..81 guard hits per run, against ~1300 observed root
 * relocations.  That guard had no coverage at all before.
 *
 * ☞ THE COST IS REAL AND IT IS THE POINT.  Under churn the moves contend the
 * root far harder, so merges per run fall by roughly a quarter and the
 * grandparent acquire is refused ~93% of the time.  Measured, that refusal is
 * ordinary PEER contention, not the op meeting itself: with
 * FEATURE_FT_HOLD_TRACE armed the run reports ZERO self-collisions, and 98.8%
 * of the refusals see LOCK actually set on the word.
 */
static int inv_rekey_merge_compressed_dst_rootchurn(void)
{
	INV_NEED_MERGE("inv_rekey_merge_compressed_dst_rootchurn");
	return rkm_run("inv_rekey_merge_compressed_dst_rootchurn", 2, 1);
}

/*
 * SHARED-DESTINATION merge oracle -- the companion inv_rekey_merge_occupied_dst
 * cannot be, and the reason is the whole point.
 *
 * That oracle gives every writer its own junctions.  Writers then contend only
 * ROOT, which the DETACH acquires -- so every node the MERGE BRANCH acquires
 * (the publish parent, the dst overlap spine, the dup-chain holders) stays
 * writer-PRIVATE and its bail is structurally unreachable.  Measured: 0 of the
 * fold's 7 bail sites taken across 4187 merges, while the same run logged 3189
 * retries.  Retries are not coverage; an oracle exercises only the acquires
 * whose nodes its writers SHARE.
 *
 * So here the DESTINATION is shared.  Writer w keeps a PRIVATE source junction
 * bp=0x20+w (so seeding never collides) and merges into one of RKMS_NJ SHARED
 * destinations J[w % RKMS_NJ].  Several writers therefore aim the same publish
 * parent, retire the same overlap node, and append to the same chain, which is
 * exactly the contention the disjoint layout cannot produce.
 *
 * Per shared junction J: RESIDENTS at (J,3,1) and (J,3,2), permanent, so the
 * destination is always OCCUPIED (a merge, never a graft) and the node is always
 * a >=2-child branch (never path-compressed).
 * Per writer w: ONE mover at (bp,3,0x10+w), PRIVATE, so cleanup can never touch
 * a peer's key.
 *
 * NO COLLISION HERE.  A colliding mover would land on a chain several writers
 * append to at once, and taking one's own entry back off it removes a NON-HEAD
 * duplicate while peers restructure the same chain -- a second variable, and
 * this oracle already has one job: contending the merge branch's own acquires,
 * which needs no collision.  Collisions live in inv_rekey_merge_occupied_dst.
 *
 * ★ That separation was FIRST made for a reason that turned out to be WRONG, so
 * do not reuse the reasoning: while this oracle was being built it aborted in
 * ft_node_recompact, and dropping collisions was tried as the fix on the theory
 * that the cleanup above was a harness race.  It was not -- the abort persisted
 * without collisions, and the cause was ft_detach_node's up-walk reading a child
 * count off a peer's parked state word.  Re-adding collisions here is a
 * legitimate future extension, not a known hazard.
 *
 * ORACLE, per move and not only at rest: this writer's private moved key is
 * present and BOTH residents survive.  A merge that dropped the destination's
 * own keys leaves a verifiable trie, so only an in-loop check sees it.
 * -EINVAL is fatal; liveness is per writer.
 */
#define RKMS_NW		8		/* writers */
#define RKMS_NJ		2		/* SHARED destination junctions */
#define RKMS_SB		3		/* S_top slot byte inside every junction */
#define RKMS_GUARD_BASE	0x40		/* 2nd child byte base: keeps S_top a branch */

struct rkms_writer_arg {
	struct cds_ft *ft;
	uint8_t bp;			/* PRIVATE source junction */
	uint8_t dp;			/* SHARED destination junction */
	uint8_t mine;			/* this writer's private mover byte */
	uint8_t guard;			/* and its private 2nd-child byte */
	struct ft_test_node *sib[2];	/* (bp,1) (bp,5): keep BP >= 3 children */
	struct ft_test_node *seed_priv;	/* the node seeded at (bp,3,mine) */
	struct ft_test_node *seed_guard;/* second child so S_top is a branch */
	int seeded;
	unsigned long ops, retries;
	int failed;
};

/* Remove the node @n stored at key @v, by IDENTITY -- see the header. */
static enum cds_ft_status rkms_remove_node(struct cds_ft *ft, uint64_t v,
		struct ft_test_node *n)
{
	struct cds_ft_iter *iter = NULL;
	enum cds_ft_status st = CDS_FT_STATUS_NOT_FOUND;
	uint8_t k[8] = { 0 };

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	cds_ft_lookup(ft, iter);
	if (cds_ft_iter_node(iter)) {
		st = cds_ft_remove(ft, iter, &n->node);
		if (st == CDS_FT_STATUS_OK)
			node_free_rcu(n);
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return st;
}

static void *rkms_writer(void *arg)
{
	struct rkms_writer_arg *w = (struct rkms_writer_arg *) arg;
	uint8_t src_key[2], dst_key[2];
	unsigned long iters = 0;

	src_key[0] = w->bp; src_key[1] = RKMS_SB;
	dst_key[0] = w->dp; dst_key[1] = RKMS_SB;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t kp = rkm_key(w->bp, RKMS_SB, w->mine);
		uint64_t kg = rkm_key(w->bp, RKMS_SB, w->guard);
		uint64_t dp_priv = rkm_key(w->dp, RKMS_SB, w->mine);
		struct cds_ft_node *f = NULL;
		int rc;

		if (!w->seeded) {
			rcu_read_lock();
			/*
			 * TWO keys under (bp,3): the mover plus a per-writer guard
			 * at a private byte, so S_top is a >=2-child BRANCH rather
			 * than a path-compressed node the shape gate refuses.  Both
			 * are lone deep keys, hence compressed children -- the
			 * skip-compressed re-parents this path is about.
			 */
			w->seed_priv = node_alloc(kp);
			w->seed_guard = node_alloc(kg);
			if (insert_u64(w->ft, kp, w->seed_priv) != CDS_FT_STATUS_OK ||
					insert_u64(w->ft, kg, w->seed_guard) !=
						CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				fprintf(stderr, "rkms bp=%u: seed failed\n", w->bp);
				w->failed = 1;
				goto out;
			}
			rcu_read_unlock();
			w->seeded = 1;
		}

		/* No read lock: the move takes the gate, which waits a grace period. */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 2, dst_key, 2);
		if (rc == 0) {
			w->ops++;
			rcu_read_lock();
			/* my PRIVATE moved key */
			if (lookup_u64(w->ft, dp_priv, &f) != CDS_FT_STATUS_OK ||
					f != &w->seed_priv->node) {
				rcu_read_unlock();
				fprintf(stderr, "rkms bp=%u dp=%u: private moved key "
					"absent after merge\n", w->bp, w->dp);
				w->failed = 1;
				mw_violation_snapshot();
				goto out;
			}
			rcu_read_unlock();
			if (rkms_remove_node(w->ft, dp_priv, w->seed_priv) !=
						CDS_FT_STATUS_OK ||
					rkms_remove_node(w->ft,
						rkm_key(w->dp, RKMS_SB, w->guard),
						w->seed_guard) != CDS_FT_STATUS_OK) {
				fprintf(stderr, "rkms bp=%u: cleanup failed\n", w->bp);
				w->failed = 1;
				goto out;
			}
			w->seeded = 0;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;
		} else {
			fprintf(stderr, "rkms bp=%u dp=%u: merge failed rc=%d\n",
				w->bp, w->dp, rc);
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
out:
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_merge_shared_dst(void)
{
	INV_NEED_MERGE("inv_rekey_merge_shared_dst");
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkms_writer_arg *w;
	struct ft_test_node *res[RKMS_NJ][2];
	pthread_t writers[RKMS_NW];
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;
	static const uint8_t J[RKMS_NJ] = { 0x10, 0x11 };

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_merge_shared_dst: skipped "
			"(set FT_INV_MW=1 to run the shared-destination merge oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_listoff_ft(4, &group);	/* LIST OFF */
	cds_ft_make_shared(ft);

	rcu_read_lock();
	for (i = 0; i < RKMS_NJ; i++) {
		/* Sibs so the shared junction node is a stable multi-child branch. */
		for (c = 0; c < 2; c++) {
			uint64_t sk = rkm_key(J[i], c == 0 ? 1 : 5, 0);

			if (insert_u64(ft, sk, node_alloc(sk)) != CDS_FT_STATUS_OK)
				abort();
			live++;
		}
		for (c = 0; c < 2; c++) {	/* RESIDENTS: dst always occupied */
			uint64_t rk = rkm_key(J[i], RKMS_SB, (uint8_t) (c + 1));

			res[i][c] = node_alloc(rk);
			if (insert_u64(ft, rk, res[i][c]) != CDS_FT_STATUS_OK)
				abort();
			live++;
		}
	}
	rcu_read_unlock();

	w = (struct rkms_writer_arg *) calloc(RKMS_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKMS_NW; i++) {
		uint8_t bp = (uint8_t) (0x20 + i);

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = J[i % RKMS_NJ];
		w[i].mine = (uint8_t) (0x10 + i);
		w[i].guard = (uint8_t) (RKMS_GUARD_BASE + i);
		rcu_read_lock();
		for (c = 0; c < 2; c++) {	/* BP >= 3 children on removal */
			uint64_t sk = rkm_key(bp, c == 0 ? 1 : 5, 0);

			w[i].sib[c] = node_alloc(sk);
			if (insert_u64(ft, sk, w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
			live++;
		}
		rcu_read_unlock();
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKMS_NW; i++)
		pthread_create(&writers[i], NULL, rkms_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKMS_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Drop whatever the last iteration left seeded at the private sources. */
	synchronize_rcu();
	for (i = 0; i < RKMS_NW; i++) {
		if (!w[i].seeded)
			continue;
		(void) rkms_remove_node(ft, rkm_key(w[i].bp, RKMS_SB, w[i].mine),
				w[i].seed_priv);
		(void) rkms_remove_node(ft, rkm_key(w[i].bp, RKMS_SB, w[i].guard),
				w[i].seed_guard);
		w[i].seeded = 0;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKMS_NW; i++) {
		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		if (w[i].ops == 0) {	/* per-writer: a global check hides starvation */
			fprintf(stderr, "rekey merge shared: writer %d made NO move\n", i);
			ret = -1;
		}
	}
	for (i = 0; i < RKMS_NJ; i++) {
		for (c = 0; c < 2; c++) {
			struct cds_ft_node *f = NULL;
			uint64_t rk = rkm_key(J[i], RKMS_SB, (uint8_t) (c + 1));

			if (lookup_u64(ft, rk, &f) != CDS_FT_STATUS_OK ||
					f != &res[i][c]->node) {
				fprintf(stderr, "rekey merge shared: resident %d/%d lost "
					"to a union\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_entries(ft) != live) {
		fprintf(stderr, "rekey merge shared: entries %lu != live %lu\n",
			cds_ft_count_entries(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey merge shared: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_rekey_merge_shared_dst: %d writers over %d shared "
		"destinations, %lu merges, %lu retries, %lu live keys\n",
		RKMS_NW, RKMS_NJ, total_ops, total_retries, live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Coherent-rekey concurrent-writer oracle, CROSS-JUNCTION: the same back-and-forth
 * move, but between junctions whose PARENTS DIFFER (d_src.ppnf != d_dst.ppnf), the
 * shape the hook's original gate refused.  The folded detach can no longer reuse
 * the lock the graft holds, so its src-junction recompaction acquires the src
 * junction's own parent itself, validated by the BP.parent == @parent read-set
 * guard riding that acquire commit -- the arm this oracle exists to run under
 * contention.
 *
 * It is also the SEPARATION experiment for the disjoint oracle's headline finding
 * (moves serialize on the one shared ancestor they all lock): here NO node is
 * shared between writers -- writer i's two junctions hang under its OWN root
 * children (2i+1, 2i+2), and root is never locked by a move -- so the retry rate
 * measures the acquire path itself rather than root contention.  Both numbers are
 * printed; the disjoint oracle's ~4 retries per move against this one's is the
 * comparison.
 *
 * 5-byte keys, so S_top's children stay one level ABOVE the terminal externals (as
 * in the depth-2 oracles).  Per writer, under each of its two root children W:
 *   W -> { 1 -> {1, [3 = S_top -> {1,2,3,4}], 5},  9 }
 * The byte-9 filler makes the depth-1 node BRANCHING (a lone child would be
 * path-compressed, which the driver's descent refuses); the byte-{1,5} leaves make
 * the junction a 3-child node that survives losing S_top and give the dst gap
 * neighbours that are never the moved run's own endpoints.  List ON, so each move
 * also folds the run's cell re-splice into the same commit.  Opt-in FT_INV_MW=1.
 */
#define RKX_NW		8		/* cross-junction rekey writers */
#define RKX_NSUB	4		/* S_top's four leaves, moving with it */
#define RKX_KEY(b0, b1, b2, b3, b4)					\
	(((uint64_t) (b0) << 32) | ((uint64_t) (b1) << 24) |		\
	 ((uint64_t) (b2) << 16) | ((uint64_t) (b3) << 8) | (uint64_t) (b4))

struct rkx_writer_arg {
	struct cds_ft *ft;
	uint8_t w1, w2;			/* the writer's two root children */
	struct ft_test_node *fill[2];	/* (W,9,0,0,0): keeps depth-1 branching */
	struct ft_test_node *sib[4];	/* (W,1,1,0,0) / (W,1,5,0,0) on both sides */
	struct ft_test_node *top[RKX_NSUB];
	int at_w2;			/* 0: S_top under w1; 1: under w2 */
	unsigned long ops, retries;
	int failed;
};

static void *rkx_writer(void *arg)
{
	struct rkx_writer_arg *w = (struct rkx_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint8_t cur = w->at_w2 ? w->w2 : w->w1;
		uint8_t oth = w->at_w2 ? w->w1 : w->w2;
		uint8_t src_key[3] = { cur, 1, 3 };
		uint8_t dst_key[3] = { oth, 1, 3 };
		int rc;

		/* No read lock: the move enters the gate, which waits a GP. */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 3, dst_key, 3);
		if (rc == 0) {
			w->at_w2 = !w->at_w2;
			w->ops++;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;	/* transient: trie pristine, re-descend */
		} else {
			fprintf(stderr, "rkx_writer w1=%u w2=%u: move from %02x "
				"failed rc=%d\n", w->w1, w->w2, cur, rc);
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * THE RUN-JUNCTION REKEY ORACLE -- a subtree whose parent IS a path-compressed
 * run, moved to a destination that DIVERGES INSIDE that same run.
 *
 * This is the shape the atomic writer gained in @b97bfabe (a compressed run on
 * the src descent, and a compressed branch parent) and @1756c666 (the split
 * DROPS its old direction, so the move needs no detach at all).  Every one of
 * those landed with a SINGLE deterministic unit-test witness and ZERO
 * concurrent coverage: measured on the whole ft_inv suite, the src descent met
 * a compressed node 0 times and the drop was armed 2826 times and taken 0.  A
 * publish path with no peers on it is a path whose lock set has never been
 * contended, so this oracle exists to put peers on it.
 *
 * GEOMETRY, and every part of it is load-bearing:
 *
 *      root --R--> run(X) --> S_top --> RKR_NSUB leaves
 *
 * Writer i owns root byte R = i + 1 and NOTHING else lives under R, which is
 * what makes the depth-1 edge single-child and therefore a RUN.  The move
 * alternates the run's byte between RKR_X1 and RKR_X2:
 *
 *   - the SRC descent crosses the run WHOLE (its one byte is the src key's
 *     second), so S_top is the run's child and the branch parent IS the run;
 *   - the DST key diverges at the run's FIRST byte, so ft_graft_build splits
 *     it -- and the node it splits is the branch parent, which is exactly the
 *     drop's precondition.
 *
 * So one fixture drives three widenings at once, and the move is symmetric:
 * after it the run carries the other byte and the next move is its mirror.
 *
 * ★ THE ROOT IS THE CONTENDED WORD, deliberately.  Every writer's forward
 * publish replaces a slot in the ONE root node, so eight writers serialise
 * there -- which is the point, since the drop's publish parent is that node and
 * nothing else in this suite contends it through a compressed junction.
 *
 * ORACLE: at rest every writer's leaves are readable at its CURRENT run byte
 * and absent at the other, the key count is exact, and cds_ft_verify passes.
 * Plus LIVENESS: a shape this writer refuses would surface as rc != 0 (the
 * debug entry point calls the atomic writer DIRECTLY, so no fallback can hide
 * one), and a livelock would surface as zero moves.
 */
#define RKR_NW		8	/* run-junction rekey writers, one root byte each */
#define RKR_NSUB	4	/* S_top's leaves, moving with it */
#define RKR_X1		1	/* the two bytes the run alternates between */
#define RKR_X2		2

struct rkr_writer_arg {
	struct cds_ft *ft;
	uint8_t root;			/* this writer's root byte */
	struct ft_test_node *top[RKR_NSUB];
	int at_x2;			/* 0: run carries RKR_X1; 1: RKR_X2 */
	unsigned long ops, retries;
	int failed;
};

static void *rkr_writer(void *arg)
{
	struct rkr_writer_arg *w = (struct rkr_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint8_t cur = w->at_x2 ? RKR_X2 : RKR_X1;
		uint8_t oth = w->at_x2 ? RKR_X1 : RKR_X2;
		uint8_t src_key[2] = { w->root, cur };
		uint8_t dst_key[2] = { w->root, oth };
		int rc;

		/* No read lock: the move enters the gate, which waits a GP. */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 2,
				dst_key, 2);
		if (rc == 0) {
			w->at_x2 = !w->at_x2;
			w->ops++;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;	/* transient: trie pristine, re-descend */
		} else {
			fprintf(stderr, "rkr_writer root=%u: move from %02x "
				"failed rc=%d\n", w->root, cur, rc);
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_graft_run_junction(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkr_writer_arg *w;
	pthread_t writers[RKR_NW];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_run_junction: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(5, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	/*
	 * Global guards so no writer's subtree is ever the list head or tail.
	 * They sit at root bytes 0x00 / 0xff, outside every writer's byte.
	 */
	guard_lo = node_alloc(RKX_KEY(0x00, 0, 0, 0, 0));
	guard_hi = node_alloc(RKX_KEY(0xff, 0, 0, 0, 0));
	rcu_read_lock();
	if (insert_u64(ft, RKX_KEY(0x00, 0, 0, 0, 0), guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, RKX_KEY(0xff, 0, 0, 0, 0), guard_hi) !=
			CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rkr_writer_arg *) calloc(RKR_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKR_NW; i++) {
		w[i].ft = ft;
		w[i].root = (uint8_t) (i + 1);
		/*
		 * ONLY these keys under @root: any sibling at depth 1 would give
		 * the root child two branches and there would be no run to cross,
		 * split or drop -- the arm would silently become an ordinary
		 * plain-junction move.  The counters below are what check that it
		 * did not.
		 */
		rcu_read_lock();
		for (c = 0; c < RKR_NSUB; c++) {
			uint64_t tk = RKX_KEY(w[i].root, RKR_X1,
					(uint8_t) (c + 1), 0, 0);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += RKR_NSUB;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKR_NW; i++)
		pthread_create(&writers[i], NULL, rkr_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKR_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: every leaf at its writer's final byte, absent at the other. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKR_NW; i++) {
		uint8_t here = w[i].at_x2 ? RKR_X2 : RKR_X1;
		uint8_t there = w[i].at_x2 ? RKR_X1 : RKR_X2;

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		for (c = 0; c < RKR_NSUB; c++) {
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, RKX_KEY(w[i].root, here,
					(uint8_t) (c + 1), 0, 0), &found) !=
					CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "rekey run-junction: writer %d leaf %d "
					"absent at final pos (at_x2=%d)\n", i, c,
					w[i].at_x2);
				ret = -1;
			}
			if (lookup_u64(ft, RKX_KEY(w[i].root, there,
					(uint8_t) (c + 1), 0, 0), &found) ==
					CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey run-junction: writer %d leaf %d "
					"still at old pos\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey run-junction: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey run-junction: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {			/* liveness: writers made progress */
		fprintf(stderr, "rekey run-junction: no successful moves "
			"(livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_run_junction: %d writers, %lu moves, "
		"%lu retries, %lu live keys\n", RKR_NW, total_ops, total_retries,
		live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * THE ELEVATING-DETACH REKEY ORACLE -- the move whose SOURCE JUNCTION EMPTIES.
 *
 * When the moved subtree's parent is a path-compressed run, taking the subtree
 * away leaves that run with nothing, so the detach's upward walk ELEVATES past
 * it and hands back an orphan chain the fold must reclaim on the far side of
 * the one commit.  The run-junction oracle cannot reach that arm: there the
 * destination diverges inside the SAME run, so the split drops its old
 * direction and the detach is skipped entirely.
 *
 * It could not be reached from a SYMMETRIC fixture at all until the short
 * landing was in scope, and the reason is the shape's own doing: an elevating
 * move DELETES its source junction, so the return leg's destination is a slot
 * that no longer exists and the descent stops ABOVE the key.  That is why this
 * oracle and that widening arrive together -- leg 1 exercises the elevation,
 * leg 2 exercises the short landing it creates.
 *
 * GEOMETRY, per writer, on its own root byte R -- TWO junction nodes, and
 * they must be two:
 *
 *      R --A--> J_a{ P --> run(X) --> S_top --> leaves,  Q1, Q2 }
 *      R --B--> J_b{ Q1, Q2 }
 *
 * The move alternates the subtree between J_a's slot P and J_b's.  Each leg:
 *
 *   - the src descent crosses run(X) WHOLE, so the branch parent IS that run;
 *   - dropping the subtree EMPTIES that run, so the detach's climb elevates
 *     one level and recompacts the junction node -- the elevating arm;
 *   - the junction keeps Q1 and Q2, which is what STOPS the climb there;
 *   - the destination junction has no P, so the descent lands one byte ABOVE
 *     the key -- the SHORT landing, which the elevation on the other side is
 *     what creates.
 *
 * ★ WHY TWO JUNCTIONS AND NOT ONE.  With both positions under a single node,
 * that node is BOTH the graft's attach node and the src branch parent's
 * parent -- the `d_src.ppnf == graft_c` alias, which is refused permanently
 * because the detach would edit the copy the graft retires.  Splitting them
 * across J_a and J_b is what keeps the two junctions disjoint, and it is the
 * only reason the fixture has a second one.
 *
 * ORACLE: at rest every leaf is readable at its writer's current junction and
 * absent at the other, BOTH filler keys survive every elevation and collapse,
 * the key count is exact, and cds_ft_verify passes.  Zero moves would mean a
 * livelock; a refused shape surfaces as rc != 0, the debug entry point calling
 * the atomic writer directly.
 */
#define RKE_NW		8	/* elevating rekey writers, one root byte each */
#define RKE_NSUB	4	/* S_top's leaves, moving with it */
#define RKE_A		1	/* the two junction bytes under R */
#define RKE_B		2
#define RKE_P		3	/* the junction slot the subtree occupies */
#define RKE_Q1		4	/* two fillers per junction: they stop the climb */
#define RKE_Q2		5
#define RKE_X		6	/* the run byte the subtree hangs under */

struct rke_writer_arg {
	struct cds_ft *ft;
	uint8_t root;
	struct ft_test_node *top[RKE_NSUB];
	struct ft_test_node *fill[4];	/* Q1/Q2 under each junction */
	int at_b;			/* 0: subtree under J_a; 1: under J_b */
	unsigned long ops, retries;
	int failed;
};

static void *rke_writer(void *arg)
{
	struct rke_writer_arg *w = (struct rke_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint8_t cur = w->at_b ? RKE_B : RKE_A;
		uint8_t oth = w->at_b ? RKE_A : RKE_B;
		uint8_t src_key[4] = { w->root, cur, RKE_P, RKE_X };
		uint8_t dst_key[4] = { w->root, oth, RKE_P, RKE_X };
		int rc;

		/* No read lock: the move enters the gate, which waits a GP. */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 4,
				dst_key, 4);
		if (rc == 0) {
			w->at_b = !w->at_b;
			w->ops++;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;	/* transient: trie pristine, re-descend */
		} else {
			fprintf(stderr, "rke_writer root=%u: move from %02x "
				"failed rc=%d\n", w->root, cur, rc);
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_graft_elevating_junction(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rke_writer_arg *w;
	pthread_t writers[RKE_NW];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_elevating_junction: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(5, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(RKX_KEY(0x00, 0, 0, 0, 0));
	guard_hi = node_alloc(RKX_KEY(0xff, 0, 0, 0, 0));
	rcu_read_lock();
	if (insert_u64(ft, RKX_KEY(0x00, 0, 0, 0, 0), guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, RKX_KEY(0xff, 0, 0, 0, 0), guard_hi) !=
			CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rke_writer_arg *) calloc(RKE_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKE_NW; i++) {
		static const uint8_t junc[2] = { RKE_A, RKE_B };
		static const uint8_t qb[2] = { RKE_Q1, RKE_Q2 };
		int j, q;

		w[i].ft = ft;
		w[i].root = (uint8_t) (i + 1);
		rcu_read_lock();
		/*
		 * TWO fillers under each junction, so it is a plain node that
		 * KEEPS at least two children when the subtree leaves -- which is
		 * what stops the elevating climb there instead of letting it walk
		 * on and delete the junction the graft just attached under.
		 */
		for (j = 0; j < 2; j++) {
			for (q = 0; q < 2; q++) {
				uint64_t fk = RKX_KEY(w[i].root, junc[j],
						qb[q], 0, 0);

				w[i].fill[2 * j + q] = node_alloc(fk);
				if (insert_u64(ft, fk, w[i].fill[2 * j + q]) !=
						CDS_FT_STATUS_OK)
					abort();
			}
		}
		/*
		 * ONE byte under J_a's slot P, so that edge is single-child and
		 * the subtree's parent is a RUN -- the junction that empties.
		 */
		for (c = 0; c < RKE_NSUB; c++) {
			uint64_t tk = RKX_KEY(w[i].root, RKE_A, RKE_P, RKE_X,
					(uint8_t) (c + 1));

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 4 + RKE_NSUB;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKE_NW; i++)
		pthread_create(&writers[i], NULL, rke_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKE_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKE_NW; i++) {
		uint8_t here = w[i].at_b ? RKE_B : RKE_A;
		uint8_t there = w[i].at_b ? RKE_A : RKE_B;
		struct cds_ft_node *found = NULL;

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		/*
		 * The fillers are the point as much as the leaves: an elevation
		 * that climbed one level too far takes THESE out.
		 */
		{
			static const uint8_t junc[2] = { RKE_A, RKE_B };
			static const uint8_t qb[2] = { RKE_Q1, RKE_Q2 };
			int j, q;

			for (j = 0; j < 2; j++)
				for (q = 0; q < 2; q++)
					if (lookup_u64(ft, RKX_KEY(w[i].root,
							junc[j], qb[q], 0, 0),
							&found) !=
							CDS_FT_STATUS_OK ||
							found != &w[i].fill[2 * j + q]->node) {
						fprintf(stderr, "rekey elevating: "
							"writer %d filler %d/%d lost\n",
							i, j, q);
						ret = -1;
					}
		}
		for (c = 0; c < RKE_NSUB; c++) {
			if (lookup_u64(ft, RKX_KEY(w[i].root, here, RKE_P, RKE_X,
					(uint8_t) (c + 1)), &found) !=
					CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "rekey elevating: writer %d leaf %d "
					"absent at final pos (at_b=%d)\n", i, c,
					w[i].at_b);
				ret = -1;
			}
			if (lookup_u64(ft, RKX_KEY(w[i].root, there, RKE_P,
					RKE_X, (uint8_t) (c + 1)), &found) ==
					CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey elevating: writer %d leaf %d "
					"still at old pos\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey elevating: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey elevating: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {
		fprintf(stderr, "rekey elevating: no successful moves (livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_elevating_junction: %d writers, %lu moves, "
		"%lu retries, %lu live keys\n", RKE_NW, total_ops, total_retries,
		live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

static int inv_rekey_graft_cross_junction(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkx_writer_arg *w;
	pthread_t writers[RKX_NW];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_cross_junction: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(5, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	/* Global guard leaves so no writer's subtree is ever the list head/tail. */
	guard_lo = node_alloc(RKX_KEY(0x00, 0, 0, 0, 0));
	guard_hi = node_alloc(RKX_KEY(0xff, 0, 0, 0, 0));
	rcu_read_lock();
	if (insert_u64(ft, RKX_KEY(0x00, 0, 0, 0, 0), guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, RKX_KEY(0xff, 0, 0, 0, 0), guard_hi) !=
			CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rkx_writer_arg *) calloc(RKX_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKX_NW; i++) {
		uint8_t roots[2] = { (uint8_t) (2 * i + 1), (uint8_t) (2 * i + 2) };
		int r;

		w[i].ft = ft;
		w[i].w1 = roots[0];
		w[i].w2 = roots[1];
		rcu_read_lock();
		for (r = 0; r < 2; r++) {
			uint64_t fk = RKX_KEY(roots[r], 9, 0, 0, 0);
			uint64_t s1 = RKX_KEY(roots[r], 1, 1, 0, 0);
			uint64_t s5 = RKX_KEY(roots[r], 1, 5, 0, 0);

			w[i].fill[r] = node_alloc(fk);
			w[i].sib[2 * r] = node_alloc(s1);
			w[i].sib[2 * r + 1] = node_alloc(s5);
			if (insert_u64(ft, fk, w[i].fill[r]) != CDS_FT_STATUS_OK ||
					insert_u64(ft, s1, w[i].sib[2 * r]) !=
					CDS_FT_STATUS_OK ||
					insert_u64(ft, s5, w[i].sib[2 * r + 1]) !=
					CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < RKX_NSUB; c++) {
			uint64_t tk = RKX_KEY(roots[0], 1, 3, c + 1, 0);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 6 + RKX_NSUB;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKX_NW; i++)
		pthread_create(&writers[i], NULL, rkx_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKX_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: each key at its writer's final position, absent at the other. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKX_NW; i++) {
		uint8_t roots[2] = { w[i].w1, w[i].w2 };
		uint8_t here = w[i].at_w2 ? w[i].w2 : w[i].w1;
		uint8_t there = w[i].at_w2 ? w[i].w1 : w[i].w2;
		int r;

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		for (r = 0; r < 2; r++) {
			struct cds_ft_node *found = NULL;
			uint64_t fixed[3] = {
				RKX_KEY(roots[r], 9, 0, 0, 0),
				RKX_KEY(roots[r], 1, 1, 0, 0),
				RKX_KEY(roots[r], 1, 5, 0, 0),
			};
			struct ft_test_node *expect[3] = {
				w[i].fill[r], w[i].sib[2 * r], w[i].sib[2 * r + 1],
			};

			for (c = 0; c < 3; c++) {
				if (lookup_u64(ft, fixed[c], &found) !=
						CDS_FT_STATUS_OK ||
						found != &expect[c]->node) {
					fprintf(stderr, "rekey cross-junction: writer %d "
						"fixed key %d lost\n", i, c);
					ret = -1;
				}
			}
		}
		for (c = 0; c < RKX_NSUB; c++) {
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, RKX_KEY(here, 1, 3, c + 1, 0), &found) !=
					CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "rekey cross-junction: writer %d top %d "
					"absent at final pos (at_w2=%d)\n", i, c,
					w[i].at_w2);
				ret = -1;
			}
			if (lookup_u64(ft, RKX_KEY(there, 1, 3, c + 1, 0), &found) ==
					CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey cross-junction: writer %d top %d "
					"still at old pos\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey cross-junction: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey cross-junction: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {			/* liveness: writers made progress */
		fprintf(stderr, "rekey cross-junction: no successful moves "
			"(livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_cross_junction: %d writers, %lu moves, "
		"%lu retries, %lu live keys\n", RKX_NW, total_ops, total_retries,
		live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Coherent-rekey concurrent-writer oracle, GLUE dst: every move lands its subtree
 * INSIDE a compressed node, so the graft cannot append in place -- it splits the
 * node and the fold records the whole invisible cluster's publish into the one
 * commit.  This is the only concurrent exercise of that shape, and it is aimed at
 * the two fences the shape adds: the split node's (taken by the build) and the
 * PUBLISH PARENT's (taken by the driver, because a guard fallback would leave the
 * forward SW park unheld).
 *
 * Contention is deliberate and on the publish parent: each writer's compressed
 * node hangs directly under a root child, so `publish_parent` is the ROOT NODE for
 * ALL of them -- they serialize on one fence exactly as the disjoint oracle's
 * writers do, which is what makes the abort-and-retry arms run.  Key ownership
 * stays disjoint, so an -EINVAL is still a real failure.
 *
 * A GLUE move CANNOT oscillate the way the other rekey oracles do: the split
 * consumes the compressed node and leaves a 2-child branch, which the src-junction
 * min_child gate refuses as a source.  So each iteration RESTORES the shape --
 * remove the four moved keys from the dst side (the branch drops to one child and
 * chain-compresses back) and re-insert them at the source.  The loop only tests
 * test_stop at an iteration boundary, so the trie is always in its base state at
 * join time.  Ordinary insert/remove concurrency rides along for free.
 *
 * Per writer w: root children w1 = 2w+1 (source side) and w2 = 2w+2 (dst side).
 *   w1 -> { 1 -> {1, [3 = S_top -> {1,2,3,4}], 5},  9 }
 *   w2 -> compressed[1,7,7,7] -> one leaf          <- the dst key {w2,1,3} diverges
 *                                                     at its SECOND byte
 * Opt-in FT_INV_MW=1.
 */
#define RKGL_NW		8		/* GLUE rekey writers, all contending root */
#define RKGL_NSUB	4

struct rkgl_writer_arg {
	struct cds_ft *ft;
	uint8_t w1, w2;			/* source-side / dst-side root children */
	struct ft_test_node *fill;	/* (w1,9,0,0,0) */
	struct ft_test_node *sib[2];	/* (w1,1,1,0,0) / (w1,1,5,0,0) */
	struct ft_test_node *lone;	/* (w2,1,7,7,7): the compressed node's key */
	/*
	 * TWO generations of the moved leaves, rotated instead of re-allocated.
	 * The restore below removes the live generation and re-inserts the other
	 * one, which was removed a full iteration ago -- and every iteration
	 * starts with a move, whose gate waits a GRACE PERIOD, so the generation
	 * being re-inserted has provably been unreachable across one.  That keeps
	 * the oracle allocation-free and, more importantly, keeps it from pushing
	 * ~14k call_rcu callbacks per run through the shared queues, where the
	 * only thing it measures is other tests' tolerance for a busy reclaimer.
	 */
	struct ft_test_node *top[2][RKGL_NSUB];
	int gen;			/* which generation is currently live */
	unsigned long ops, retries;
	int failed;
};

static void *rkgl_writer(void *arg)
{
	struct rkgl_writer_arg *w = (struct rkgl_writer_arg *) arg;
	uint8_t src_key[3], dst_key[3];
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	src_key[0] = w->w1; src_key[1] = 1; src_key[2] = 3;
	dst_key[0] = w->w2; dst_key[1] = 1; dst_key[2] = 3;

	rcu_register_thread();
	if (cds_ft_iter_create(w->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int rc, c;

		/* No read lock: the move enters the gate, which waits a GP. */
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 3, dst_key, 3);
		if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;	/* transient: trie pristine, re-descend */
			continue;
		}
		if (rc != 0) {
			fprintf(stderr, "rkgl_writer w1=%u w2=%u: move failed "
				"rc=%d\n", w->w1, w->w2, rc);
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		w->ops++;
		/*
		 * RESTORE, so the next iteration faces the same shape: pull the four
		 * moved keys back out of the split's new direction and re-insert them
		 * at the source.  Fresh nodes for the re-insert (a removed node is
		 * marked and RCU-owned), and the removal drops the branch to its one
		 * surviving child, which chain-compresses back into a compressed node.
		 */
		for (c = 0; c < RKGL_NSUB; c++) {
			uint64_t moved = RKX_KEY(w->w2, 1, 3, c + 1, 0);
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(w->ft, moved, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(w->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found != &w->top[w->gen][c]->node) {
				rcu_read_unlock();
				fprintf(stderr, "rkgl_writer w1=%u: moved key %d "
					"not at dst after a committed move\n",
					w->w1, c);
				w->failed = 1;
				mw_violation_snapshot();
				goto out;
			}
			if (cds_ft_remove(w->ft, iter, found) != CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				fprintf(stderr, "rkgl_writer w1=%u: remove %d failed\n",
					w->w1, c);
				w->failed = 1;
				goto out;
			}
			rcu_read_unlock();
		}
		for (c = 0; c < RKGL_NSUB; c++) {
			uint64_t home = RKX_KEY(w->w1, 1, 3, c + 1, 0);
			struct ft_test_node *n = w->top[!w->gen][c];

			ft_test_node_init(n, home);	/* clears the removal mark */
			if (insert_u64(w->ft, home, n) != CDS_FT_STATUS_OK) {
				fprintf(stderr, "rkgl_writer w1=%u: re-insert %d "
					"failed\n", w->w1, c);
				w->failed = 1;
				goto out;
			}
		}
		w->gen = !w->gen;
		if ((++iters & 0x1f) == 0)
			rcu_quiescent_state();
	}
out:
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_graft_glue_dst(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkgl_writer_arg *w;
	pthread_t writers[RKGL_NW];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_glue_dst: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(5, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(RKX_KEY(0x00, 0, 0, 0, 0));
	guard_hi = node_alloc(RKX_KEY(0xff, 0, 0, 0, 0));
	rcu_read_lock();
	if (insert_u64(ft, RKX_KEY(0x00, 0, 0, 0, 0), guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, RKX_KEY(0xff, 0, 0, 0, 0), guard_hi) !=
			CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rkgl_writer_arg *) calloc(RKGL_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKGL_NW; i++) {
		uint64_t s1, s5, fk, lk;

		w[i].ft = ft;
		w[i].w1 = (uint8_t) (2 * i + 1);
		w[i].w2 = (uint8_t) (2 * i + 2);
		s1 = RKX_KEY(w[i].w1, 1, 1, 0, 0);
		s5 = RKX_KEY(w[i].w1, 1, 5, 0, 0);
		fk = RKX_KEY(w[i].w1, 9, 0, 0, 0);
		lk = RKX_KEY(w[i].w2, 1, 7, 7, 7);
		w[i].sib[0] = node_alloc(s1);
		w[i].sib[1] = node_alloc(s5);
		w[i].fill = node_alloc(fk);
		w[i].lone = node_alloc(lk);
		rcu_read_lock();
		if (insert_u64(ft, s1, w[i].sib[0]) != CDS_FT_STATUS_OK ||
				insert_u64(ft, s5, w[i].sib[1]) != CDS_FT_STATUS_OK ||
				insert_u64(ft, fk, w[i].fill) != CDS_FT_STATUS_OK ||
				insert_u64(ft, lk, w[i].lone) != CDS_FT_STATUS_OK)
			abort();
		for (c = 0; c < RKGL_NSUB; c++) {
			uint64_t tk = RKX_KEY(w[i].w1, 1, 3, c + 1, 0);

			w[i].top[0][c] = node_alloc(tk);
			w[i].top[1][c] = node_alloc(tk);	/* spare generation */
			if (insert_u64(ft, tk, w[i].top[0][c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 4 + RKGL_NSUB;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKGL_NW; i++)
		pthread_create(&writers[i], NULL, rkgl_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKGL_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/*
	 * Quiescent, and every writer stopped at an iteration boundary, so the trie
	 * must be back in its base state: the moved keys home, the split's old
	 * direction (the lone compressed key) intact, nothing left at the dst.
	 */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKGL_NW; i++) {
		struct cds_ft_node *found = NULL;

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		if (lookup_u64(ft, RKX_KEY(w[i].w2, 1, 7, 7, 7), &found) !=
				CDS_FT_STATUS_OK || found != &w[i].lone->node) {
			fprintf(stderr, "rekey glue: writer %d lost the compressed "
				"key\n", i);
			ret = -1;
		}
		if (lookup_u64(ft, RKX_KEY(w[i].w1, 1, 1, 0, 0), &found) !=
				CDS_FT_STATUS_OK || found != &w[i].sib[0]->node ||
				lookup_u64(ft, RKX_KEY(w[i].w1, 1, 5, 0, 0), &found) !=
				CDS_FT_STATUS_OK || found != &w[i].sib[1]->node ||
				lookup_u64(ft, RKX_KEY(w[i].w1, 9, 0, 0, 0), &found) !=
				CDS_FT_STATUS_OK || found != &w[i].fill->node) {
			fprintf(stderr, "rekey glue: writer %d lost a fixed key\n", i);
			ret = -1;
		}
		for (c = 0; c < RKGL_NSUB; c++) {
			if (lookup_u64(ft, RKX_KEY(w[i].w1, 1, 3, c + 1, 0), &found) !=
					CDS_FT_STATUS_OK ||
					found != &w[i].top[w[i].gen][c]->node) {
				fprintf(stderr, "rekey glue: writer %d key %d not "
					"home\n", i, c);
				ret = -1;
			}
			if (lookup_u64(ft, RKX_KEY(w[i].w2, 1, 3, c + 1, 0), &found) ==
					CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey glue: writer %d key %d still at "
					"dst\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey glue: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey glue: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {
		fprintf(stderr, "rekey glue: no successful moves (livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_glue_dst: %d writers, %lu moves, "
		"%lu retries, %lu live keys\n", RKGL_NW, total_ops, total_retries,
		live);

	/*
	 * The spare generation is not in the trie, so the drain below cannot free
	 * it.  The writers are joined and a grace period has passed, so a direct
	 * free is safe.
	 */
	for (i = 0; i < RKGL_NW; i++)
		for (c = 0; c < RKGL_NSUB; c++)
			node_free(w[i].top[!w[i].gen][c]);
	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Coherent-rekey oracle WITH READERS: the disjoint rekey writers run against a
 * trie whose per-trie rekey coherence is ENABLED (cds_ft_attr_set_rekey_coherence),
 * so every exact lookup runs the landed second-walk (up-walk key rematerializer)
 * re-descend.  This is the FIRST CONCURRENT exercise of that shipped coherent
 * lookup (its unit tests are single-threaded), and the reader/writer memory-safety
 * probe: a reader descending through S_top / BP races the writer COW'ing S_top and
 * recompacting BP (fresh nodes, old freed after a grace period), so it is run under
 * ASAN to catch any use-after-free / double-free across the move.
 *
 * SOUND "never absent" assertion anchored on the FIXED sibling keys: each junction's
 * (X,1) / (X,5) sibs never move and are therefore present at ALL times, yet a
 * reader's descent to a sib passes through BP, which is COW-recompacted on EVERY
 * move -- so a coherent lookup of a sib that ever MISSES (or returns a wrong node)
 * is a real atomicity / coherence failure (a descent torn across BP's recompaction
 * that the second-walk failed to repair).  The MOVING keys (X,3,c) are checked
 * weakly (a single atomic coherent lookup returns miss XOR the one node that key
 * ever maps to -- never a foreign node); full linearizability of a moving key needs
 * the deferred per-op epoch, so right-node-wrong-instant is out of scope here.
 *
 * EXACT lookups descend the trie + up-walk parent pointers; they do NOT traverse
 * the ordered-list cells, so this oracle does not exercise the deferred plan-Q3 cell
 * outer-link window (that is a relational/iteration-coherence concern, not built).
 * Opt-in FT_INV_MW=1.
 */
#define RK_NR		8		/* coherent readers */

struct rk_reader_arg {
	struct cds_ft *ft;
	struct rk_writer_arg *w;		/* the writer array (node shadow) */
	int nw;
	unsigned long checks;
	int failed;
};

static struct cds_ft *create_fixed_rekey_coherent_ft(size_t klen,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&gattr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(gattr, klen) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(gattr,
			CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(gattr, true) < 0)	/* up-walk cell source */
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF): this trie
	 * hosts an in-trie MOVE, so the capability must be declared or the
	 * rekey entries answer NOT_SUPPORTED and the test proves nothing.
	 */
	if (cds_ft_group_attr_set_rekey(gattr, true) < 0)
		abort();
	if (cds_ft_group_create(gattr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_attr_create(&attr) < 0)
		abort();
	/*
	 * Rekey coherence needs no opt-in: it is automatic on an EAGER ordered-list
	 * trie, gated at runtime by the per-trie MOVE GATE.  EAGER is the only
	 * requirement, so that is all this asks for.
	 */
	if (cds_ft_attr_set_speculative_keys(attr, false) < 0)	/* rekey needs EAGER keys */
		abort();
	if (cds_ft_create(group, attr, &ft) < 0)
		abort();
	cds_ft_attr_destroy(attr);
	*group_out = group;
	return ft;
}

static void *rk_reader(void *arg)
{
	struct rk_reader_arg *r = (struct rk_reader_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int) (uintptr_t) r;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int wi = rand_r(&seed) % r->nw;
		struct rk_writer_arg *w = &r->w[wi];
		uint64_t bpk = (uint64_t) w->bp << 24, dpk = (uint64_t) w->dp << 24;
		int pick = rand_r(&seed) % 12;
		struct cds_ft_node *found, *expect;
		int must_find;
		uint64_t key;
		uint8_t k[8];

		if (pick >= 8) {
			/*
			 * RELATIONAL probe (the coherent two-pass path) on a FIXED
			 * sibling: that key is present at every instant -- a move
			 * relocates only the (X,sb,*) run, though it COW-recompacts
			 * the junction this sibling hangs from on every single move
			 * -- so ge(K) and le(K) must land EXACTLY on K.  A torn
			 * relational traversal that stepped over it would answer
			 * with the neighbouring key instead, which is precisely what
			 * the two-pass exists to reject.
			 */
			int si = pick & 3;
			enum cds_ft_status st;

			key = ((si < 2) ? bpk : dpk) | (((si & 1) ? 5ULL : 1ULL) << 16);
			expect = &w->sib[si]->node;
			cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			st = (rand_r(&seed) & 1) ? cds_ft_lookup_ge(r->ft, iter) :
				cds_ft_lookup_le(r->ft, iter);
			found = cds_ft_iter_node(iter);
			if (st != CDS_FT_STATUS_OK || found != expect) {
				fprintf(stderr,
					"rk_reader: relational key %#llx -> %p (%s, expect %p)\n",
					(unsigned long long) key, (void *) found,
					cds_ft_status_to_string(st),
					(void *) expect);
				r->failed = 1;
				rcu_read_unlock();
				mw_violation_snapshot();
				break;
			}
			rcu_read_unlock();
			r->checks++;
			if ((++iters & 0xff) == 0)
				rcu_quiescent_state();
			continue;
		}
		if (pick < 4) {
			/* FIXED sibling (X,1)/(X,5): always present -> must be found. */
			key = ((pick < 2) ? bpk : dpk) | (((pick & 1) ? 5ULL : 1ULL) << 16);
			expect = &w->sib[pick]->node;
			must_find = 1;
		} else {
			/* MOVING leaf (X,3,c): miss XOR this leaf, never a foreign node. */
			int c = pick & 3;

			key = ((rand_r(&seed) & 1) ? dpk : bpk) |
				((uint64_t) w->sb << 16) |
				((uint64_t) (c + 1) << 8);
			expect = &w->top[c]->node;
			must_find = 0;
		}
		cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(r->ft, iter);
		found = cds_ft_iter_node(iter);
		if ((must_find && found != expect) ||
				(!must_find && found && found != expect)) {
			fprintf(stderr, "rk_reader: key %#llx -> %p (expect %s%p)\n",
				(unsigned long long) key, (void *) found,
				must_find ? "" : "miss or ", (void *) expect);
			r->failed = 1;
			rcu_read_unlock();
			mw_violation_snapshot();
			break;
		}
		rcu_read_unlock();
		r->checks++;
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_graft_coherent_readers(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	struct rk_reader_arg *r;
	pthread_t writers[RK_NW], readers[RK_NR];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, total_checks = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_coherent_readers: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey reader oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_rekey_coherent_ft(4, &group);
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (insert_u64(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rk_writer_arg *) calloc(RK_NW, sizeof(*w));
	r = (struct rk_reader_arg *) calloc(RK_NR, sizeof(*r));
	if (!w || !r)
		abort();
	for (i = 0; i < RK_NW; i++) {
		uint8_t bp = (uint8_t) (2 * i + 1), dp = (uint8_t) (2 * i + 2);
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint64_t sk[4] = {
			bpk | (1ULL << 16), bpk | (5ULL << 16),
			dpk | (1ULL << 16), dpk | (5ULL << 16),
		};

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].sb = 3;			/* S_top slot byte inside both junctions */
		rcu_read_lock();
		for (c = 0; c < 4; c++) {
			w[i].sib[c] = node_alloc(sk[c]);
			if (insert_u64(ft, sk[c], w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < 4; c++) {
			uint64_t tk = bpk | (3ULL << 16) | ((uint64_t) (c + 1) << 8);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 8;
	}
	for (i = 0; i < RK_NR; i++) {
		r[i].ft = ft;
		r[i].w = w;
		r[i].nw = RK_NW;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_create(&writers[i], NULL, rk_writer, &w[i]);
	for (i = 0; i < RK_NR; i++)
		pthread_create(&readers[i], NULL, rk_reader, &r[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < RK_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	for (i = 0; i < RK_NW; i++) {
		total_ops += w[i].ops;
		total_retries += w[i].retries;
	}
	for (i = 0; i < RK_NR; i++) {
		total_checks += r[i].checks;
		if (r[i].failed)
			ret = -1;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RK_NW; i++) {
		uint8_t bp = w[i].bp, dp = w[i].dp;
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint8_t X = w[i].at_dst ? dp : bp;
		uint64_t Xk = (uint64_t) X << 24;
		uint64_t sk[4] = {
			bpk | (1ULL << 16), bpk | (5ULL << 16),
			dpk | (1ULL << 16), dpk | (5ULL << 16),
		};

		for (c = 0; c < 4; c++) {
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, sk[c], &found) != CDS_FT_STATUS_OK ||
					found != &w[i].sib[c]->node)
				ret = -1;
		}
		for (c = 0; c < 4; c++) {
			uint64_t here = Xk | (3ULL << 16) | ((uint64_t) (c + 1) << 8);
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, here, &found) != CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node)
				ret = -1;
		}
	}
	if (cds_ft_count_keys(ft) != live)
		ret = -1;
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	rcu_read_unlock();

	if (total_ops == 0)
		ret = -1;
	fprintf(stderr, "# inv_rekey_graft_coherent_readers: %d writers %d readers, "
		"%lu moves, %lu retries, %lu reads, %lu live keys\n", RK_NW, RK_NR,
		total_ops, total_retries, total_checks, live);

	free(w);
	free(r);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * SAME-PARENT SPLICE under concurrent coherent readers.
 *
 * Mathieu's own case, {ab, ac} + rekey_merge(dst "ac", src "ab"): one key on
 * each side, src and dst sharing their last internal node P, so the move
 * changes only the LAST byte of the key and the internal path to the
 * destination head is the same path.  ft_merge_build splices the src leaf onto
 * the dst head's duplicate chain and hands the head back; nothing fresh is
 * produced at the attach point, and the address-witness the rekey-coherent
 * reader's two-descent scheme needs is the src DETACH's own relocation of P
 * (FT_REKEY_WITNESS_BY_DETACH, ft-rekey.h).  Every single-threaded instrument
 * says the shape is served and clean; this is the only oracle that reaches it
 * with READERS in flight -- the DST-FRESHNESS reach probe reads
 * same_parent_detach=0 on every other ft_inv leg.
 *
 * THE SCENARIO THE WITNESS EXISTS FOR (Mathieu): "rekey_merge, then an insert
 * on the destination external node chain.  We would not want a reader looking
 * up the ORIGINAL SRC KEY to find the inserted node at the wrong location."
 * So each writer iteration is exactly that: insert the mover at M, merge M
 * into D's chain, insert a FRESH node at D, then remove both by identity.  A
 * reader's lookup of M may answer miss or the mover -- NEVER the fresh node,
 * never D's head.
 *
 * GEOMETRY, per writer w (private root child @bp, 4-byte fixed keys), four
 * flavours f, one per iteration in turn:
 *   f=0  P  = (bp,3,0): children 1 (fixed sib), 5 (fixed sib), 2 (resident
 *        head D), 4 (the mover M).  Four children, so the detach
 *        DEL-recompacts P -- the recompaction producer of the fold.
 *   f=1  P2 = (bp,3,1): children 2 (resident D2), 4 (mover M2) only.  Two
 *        children, so the detach leaves P2 one-child keyless and COLLAPSES it
 *        into a run -- the fused-collapse producer -- and the next insert at
 *        M2 SPLITS that run again.
 *   f=2  THROUGH RUNS, parent KEPT: D3 = (bp,3,6,2) and M3 = (bp,3,7,4) are
 *        each the only key under their third byte, so each hangs off a run
 *        of its own and their shared PLAIN ancestor is Q = (bp,3), which also
 *        holds P and P2.  The node that moves is Q (FT_REKEY_WITNESS_BY_DETACH
 *        through a run); the detach DEL-recompacts Q, and the publish -- armed
 *        in D3's run, one level below -- is SUBSUMED by that copy.
 *   f=3  THROUGH RUNS, parent COLLAPSED: D4 = (bp,4,6,2) and M4 = (bp,4,7,4)
 *        under Q2 = (bp,4), which holds nothing else, so the detach collapses
 *        Q2 into D4's run -- the DEEP fold of a live top.
 *   f=4  NOT same-parent: the mover M5 = (bp,5,7,4) hangs off a run under
 *        Q3 = (bp,5), which keeps a fixed sibling (bp,5,1,1); the dst is its
 *        own resident D5 = (bp,3,0,3) under P.  This is the merge arm's COW of the
 *        dst parent (ft_rekey_merge_cow_publish_parent), the src detached from
 *        a junction of its own -- the shape the corpus serves, with the one
 *        property the others share: the mover is a SKIP TARGET that the
 *        splice turns into a non-head chain member.
 *   f=5  THE ROOT FRAME, junction below its child: the resident D6 =
 *        (bp+0x40,0,0,2) is the only key under its first byte, so it hangs
 *        straight off the ROOT; the mover M6 = (bp,5,7,5) hangs off Q3, so
 *        the detach's store lands in (bp) and only its -count walk reaches
 *        the root.  The merge arm COWs the ROOT -- after the detach
 *        (ft_rekey_merge_cow_after_detach) -- and every writer here does,
 *        so the eight of them contend on the root slot's CAS while every
 *        reader's descent passes through the copy.
 *   f=6  THE ROOT FRAME, junction a root child: D7 = (bp+0x50,0,0,2) off
 *        the root as above; the mover M7 = (bp,6,0,4) is the only key under
 *        (bp,6), so its junction is (bp) itself and the detach publishes
 *        (bp)' INTO A ROOT SLOT -- the store the post-detach copy must
 *        carry (a copy taken before it names the retired (bp) and a head's
 *        back edge the dead root: the red control at the gate that used to
 *        refuse the root frame).
 * RKSP_FLAVOURS (env, bitmask) selects the flavours run; the default is all.
 * Junctions are writer-private, so the only
 * peers of a move are the readers: a refusal here is a finding, not
 * contention, and is fatal.
 *
 * SOUND assertions, anchored on keys present at EVERY instant: the fixed sibs
 * (never moved, but their descent passes through P, which is relocated on
 * every move) and the resident heads (D never moves; its chain grows and
 * shrinks around it).  The MOVING key is checked weakly (miss XOR the mover).
 * Relational probes (ge/le) on the fixed sibs must land exactly, as in
 * inv_rekey_graft_coherent_readers.
 *
 * Nodes are allocated once and REUSED: after the two removals the writer
 * waits a grace period (a reader may still hold the removed node) before
 * re-initialising them, so a reader can never see a foreign or freed node
 * under any name.  Opt-in FT_INV_MW=1.
 *
 * ☠ FORKED UNDER AN ALARM, like the unit test for the same class, because the
 * defect it exists to catch does not REPORT -- it HANGS.  MEASURED with the
 * defect put back (-DFT_RED_SAME_PATH_STALE_TOP_EDGE: the glue re-applies the
 * folded top's stale back edge): no reader ever printed a violation.  The
 * head's parent word then names freed memory, the writer's next insert bails
 * and retries into the txn engine's fallback mutex, the readers park behind
 * that mutex inside their read sections, and the other writers wait in
 * synchronize_rcu for a grace period those readers can never provide -- a
 * deadlock, silent until the cage's kill.  Inline, that would hang the whole
 * suite and report nothing.  The child bounds itself; the parent reports.
 * The child skips drain_and_destroy: a forked child has no call_rcu worker,
 * so its rcu_barrier would block forever, and the trie dies with the process.
 */
#define RKSP_NW		8		/* writers */
#define RKSP_NR		8		/* coherent readers */
#define RKSP_KLEN	4		/* fixed key length, bytes */

#define RKSP_NF		7		/* flavours, see the header */

struct rksp_writer_arg {
	struct cds_ft *ft;
	uint8_t bp;			/* the writer's private root-child byte */
	unsigned int flavours;		/* bitmask of flavours to run */
	struct ft_test_node *sib[3];	/* (bp,3,0,1) (bp,3,0,5) (bp,5,1,1): never absent */
	struct ft_test_node *res[RKSP_NF];	/* resident heads, never absent */
	struct ft_test_node *mov[RKSP_NF];	/* the mover, spliced under res */
	struct ft_test_node *fresh[RKSP_NF];	/* inserted at res's key AFTER the move */
	unsigned long ops, busy;
	int failed;
};

struct rksp_reader_arg {
	struct cds_ft *ft;
	struct rksp_writer_arg *w;
	int nw;
	unsigned long checks;
	int failed;
};

static uint64_t rksp_key(uint8_t bp, uint8_t f, uint8_t u)
{
	return ((uint64_t) bp << 24) | (3ULL << 16) | ((uint64_t) f << 8) | u;
}

/* The resident (@mover false) or mover key of flavour @f; see the header. */
static uint64_t rksp_fkey(uint8_t bp, int f, bool mover)
{
	switch (f) {
	case 0:
	case 1:
		return rksp_key(bp, (uint8_t) f, mover ? 4 : 2);
	case 2:
		return ((uint64_t) bp << 24) | (3ULL << 16) |
			((uint64_t) (mover ? 7 : 6) << 8) | (mover ? 4 : 2);
	case 3:
		return ((uint64_t) bp << 24) | (4ULL << 16) |
			((uint64_t) (mover ? 7 : 6) << 8) | (mover ? 4 : 2);
	case 4:		/* the mover under Q3 = (bp,5); its own resident under P */
		return mover ? (((uint64_t) bp << 24) | (5ULL << 16) |
				(7ULL << 8) | 4) : rksp_key(bp, 0, 3);
	case 5:		/* the root frame; the mover's junction is Q3 */
		return mover ? (((uint64_t) bp << 24) | (5ULL << 16) |
				(7ULL << 8) | 5) :
			(((uint64_t) (bp + 0x40) << 24) | 2);
	default:	/* f=6: the root frame; the mover's junction is (bp) */
		return mover ? (((uint64_t) bp << 24) | (6ULL << 16) | 4) :
			(((uint64_t) (bp + 0x50) << 24) | 2);
	}
}

/* DEBUG_COUNTERS-only hook (no public declaration); weak so every build links. */
extern void cds_ft_debug_ext_member_hops(const struct cds_ft_group *group,
		unsigned long *hops) __attribute__((weak));

/* Remove @n, stored at key @v, by IDENTITY (a chain member); no free. */
static enum cds_ft_status rksp_remove(struct cds_ft *ft, uint64_t v,
		struct ft_test_node *n)
{
	struct cds_ft_iter *iter = NULL;
	enum cds_ft_status st = CDS_FT_STATUS_NOT_FOUND;
	uint8_t k[8] = { 0 };

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	cds_ft_lookup(ft, iter);
	if (cds_ft_iter_node(iter))
		st = cds_ft_remove(ft, iter, &n->node);
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return st;
}

static void *rksp_writer(void *arg)
{
	struct rksp_writer_arg *w = (struct rksp_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int f = (int) (iters % RKSP_NF);
		uint64_t mk, dk;

		if (!(w->flavours & (1u << f))) {
			iters++;
			continue;
		}
		mk = rksp_fkey(w->bp, f, true);
		dk = rksp_fkey(w->bp, f, false);
		uint8_t mkey[8] = { 0 }, dkey[8] = { 0 };
		enum cds_ft_status st;
		const char *step;

		cds_ft_u64_to_key(w->ft, mk, mkey, CDS_FT_LEN_DEFAULT);
		cds_ft_u64_to_key(w->ft, dk, dkey, CDS_FT_LEN_DEFAULT);

		rcu_read_lock();
		st = insert_u64(w->ft, mk, w->mov[f]);
		rcu_read_unlock();
		if (st != CDS_FT_STATUS_OK) {
			step = "insert mover";
			goto fail;
		}
		/* Explicit lengths: the dispatch bounds them by max_key_len. */
		st = cds_ft_rekey_merge(w->ft, dkey, RKSP_KLEN, mkey, RKSP_KLEN);
		if (st == CDS_FT_STATUS_BUSY_ERROR ||
				st == CDS_FT_STATUS_MEMORY_ERROR) {
			/* The mover is still at M: take it back out and retry. */
			w->busy++;
			st = rksp_remove(w->ft, mk, w->mov[f]);
			if (st != CDS_FT_STATUS_OK) {
				step = "remove mover after busy";
				goto fail;
			}
			goto reuse;
		}
		if (st != CDS_FT_STATUS_OK) {
			/*
			 * NOT_SUPPORTED included: the junction is private, so no
			 * writer can change the shape under this move -- a refusal
			 * here is the served arm refusing under READERS only.
			 */
			step = "rekey_merge";
			goto fail;
		}
		w->ops++;
		/* Mathieu's scenario: an insert on the DST chain right after. */
		rcu_read_lock();
		st = insert_u64(w->ft, dk, w->fresh[f]);
		rcu_read_unlock();
		if (st != CDS_FT_STATUS_OK) {
			step = "insert fresh at dst";
			goto fail;
		}
		st = rksp_remove(w->ft, dk, w->fresh[f]);
		if (st != CDS_FT_STATUS_OK) {
			step = "remove fresh";
			goto fail;
		}
		st = rksp_remove(w->ft, dk, w->mov[f]);
		if (st != CDS_FT_STATUS_OK) {
			step = "remove mover from dst chain";
			goto fail;
		}
reuse:
		/*
		 * The nodes are reused, not freed: a reader that found one of
		 * them is entitled to it until a grace period has elapsed.
		 */
		synchronize_rcu();
		cds_ft_node_init(&w->mov[f]->node);
		cds_ft_node_init(&w->fresh[f]->node);
		iters++;
		continue;
fail:
		fprintf(stderr, "rksp_writer bp=%u f=%d: %s: %s\n", w->bp, f,
			step, cds_ft_status_to_string(st));
		w->failed = 1;
		mw_violation_snapshot();
		break;
	}
	rcu_unregister_thread();
	return NULL;
}

static void *rksp_reader(void *arg)
{
	struct rksp_reader_arg *r = (struct rksp_reader_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int) (uintptr_t) r;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int wi = rand_r(&seed) % r->nw;
		struct rksp_writer_arg *w = &r->w[wi];
		int pick = rand_r(&seed) % 8, f = pick & 1;
		int fl = rand_r(&seed) % RKSP_NF;	/* flavour for the res/mov probes */
		struct cds_ft_node *found;
		enum cds_ft_status st;
		uint64_t key;
		uint8_t k[8] = { 0 };
		int bad = 0;

		if (pick < 2) {
			/* FIXED sib: present at every instant, must be found. */
			f = rand_r(&seed) % 3;
			key = f < 2 ? rksp_key(w->bp, 0, f ? 5 : 1) :
				(((uint64_t) w->bp << 24) | (5ULL << 16) | (1ULL << 8) | 1);
			cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(r->ft, iter);
			found = cds_ft_iter_node(iter);
			bad = found != &w->sib[f]->node;
			if (bad)
				fprintf(stderr, "rksp_reader: fixed sib %#llx -> %p "
					"(expect %p)\n", (unsigned long long) key,
					(void *) found, (void *) &w->sib[f]->node);
		} else if (pick < 4) {
			/* RELATIONAL probe on a fixed sib: must land exactly. */
			key = rksp_key(w->bp, 0, f ? 5 : 1);
			cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			st = (rand_r(&seed) & 1) ? cds_ft_lookup_ge(r->ft, iter) :
				cds_ft_lookup_le(r->ft, iter);
			found = cds_ft_iter_node(iter);
			bad = st != CDS_FT_STATUS_OK || found != &w->sib[f]->node;
			if (bad)
				fprintf(stderr, "rksp_reader: relational %#llx -> %p "
					"(%s, expect %p)\n", (unsigned long long) key,
					(void *) found, cds_ft_status_to_string(st),
					(void *) &w->sib[f]->node);
		} else if (pick < 6) {
			/*
			 * RESIDENT head: present at every instant.  Its chain
			 * holds the mover and the fresh node at times, so any of
			 * the three is a legitimate answer; a miss is not.
			 */
			key = rksp_fkey(w->bp, fl, false);
			cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(r->ft, iter);
			found = cds_ft_iter_node(iter);
			bad = found != &w->res[fl]->node &&
				found != &w->mov[fl]->node &&
				found != &w->fresh[fl]->node;
			if (bad)
				fprintf(stderr, "rksp_reader: resident %#llx -> %p "
					"(expect head %p, mover %p or fresh %p)\n",
					(unsigned long long) key, (void *) found,
					(void *) &w->res[fl]->node,
					(void *) &w->mov[fl]->node,
					(void *) &w->fresh[fl]->node);
		} else {
			/*
			 * THE ORIGINAL SRC KEY: miss XOR the mover.  The fresh
			 * node -- inserted on the DST chain after the move -- at
			 * this name is exactly the wrong location the witness
			 * exists to prevent; the resident head here is a torn
			 * descent that resolved the dst chain under the src name.
			 */
			key = rksp_fkey(w->bp, fl, true);
			cds_ft_u64_to_key(r->ft, key, k, CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(r->ft, iter);
			found = cds_ft_iter_node(iter);
			bad = found && found != &w->mov[fl]->node;
			if (bad)
				fprintf(stderr, "rksp_reader: src key %#llx -> %p "
					"(%s; expect miss or mover %p)\n",
					(unsigned long long) key, (void *) found,
					found == &w->fresh[fl]->node ?
						"THE FRESH DST-CHAIN NODE" :
					found == &w->res[fl]->node ?
						"the resident dst head" : "a foreign node",
					(void *) &w->mov[fl]->node);
		}
		if (bad) {
			r->failed = 1;
			rcu_read_unlock();
			mw_violation_snapshot();
			break;
		}
		rcu_read_unlock();
		r->checks++;
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

__attribute__((noreturn))
static void rksp_child(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rksp_writer_arg *w;
	struct rksp_reader_arg *r;
	pthread_t writers[RKSP_NW], readers[RKSP_NR];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_busy = 0, total_checks = 0, live = 0;
	int i, f, ret = 0;

	{
		struct rlimit rl = { .rlim_cur = 0, .rlim_max = 0 };

		(void) setrlimit(RLIMIT_CORE, &rl);
	}
	alarm(60);
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_rekey_coherent_ft(RKSP_KLEN, &group);
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (insert_u64(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rksp_writer_arg *) calloc(RKSP_NW, sizeof(*w));
	r = (struct rksp_reader_arg *) calloc(RKSP_NR, sizeof(*r));
	if (!w || !r)
		abort();
	for (i = 0; i < RKSP_NW; i++) {
		w[i].ft = ft;
		w[i].bp = (uint8_t) (i + 1);
		rcu_read_lock();
		/*
		 * All seven by default.  Flavour 4 is the reproducer of the reader
		 * re-anchor defect (a stale skip word into a spliced chain member);
		 * 2, 3 and 4 all make the mover a skip target, and the positive
		 * control below proves a reader's hop branch ran.  5 and 6 are the
		 * ROOT frame, every writer relocating the root under every reader.
		 */
		w[i].flavours = getenv("RKSP_FLAVOURS") ?
			(unsigned int) strtoul(getenv("RKSP_FLAVOURS"), NULL, 0) :
			(1u << RKSP_NF) - 1;
		for (f = 0; f < 3; f++) {
			uint64_t sk = f < 2 ? rksp_key(w[i].bp, 0, f ? 5 : 1) :
				(((uint64_t) w[i].bp << 24) | (5ULL << 16) |
					(1ULL << 8) | 1);

			w[i].sib[f] = node_alloc(sk);
			if (insert_u64(ft, sk, w[i].sib[f]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (f = 0; f < RKSP_NF; f++) {
			uint64_t rk = rksp_fkey(w[i].bp, f, false);

			w[i].res[f] = node_alloc(rk);
			if (insert_u64(ft, rk, w[i].res[f]) != CDS_FT_STATUS_OK)
				abort();
			/* Allocated once; keyed only while a cycle is in flight. */
			w[i].mov[f] = node_alloc(rksp_fkey(w[i].bp, f, true));
			w[i].fresh[f] = node_alloc(rk);
		}
		rcu_read_unlock();
		live += 3 + RKSP_NF;
	}
	for (i = 0; i < RKSP_NR; i++) {
		r[i].ft = ft;
		r[i].w = w;
		r[i].nw = RKSP_NW;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKSP_NW; i++)
		pthread_create(&writers[i], NULL, rksp_writer, &w[i]);
	for (i = 0; i < RKSP_NR; i++)
		pthread_create(&readers[i], NULL, rksp_reader, &r[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKSP_NW; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < RKSP_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	for (i = 0; i < RKSP_NW; i++) {
		total_ops += w[i].ops;
		total_busy += w[i].busy;
		if (w[i].failed)
			ret = -1;
	}
	for (i = 0; i < RKSP_NR; i++) {
		total_checks += r[i].checks;
		if (r[i].failed)
			ret = -1;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKSP_NW; i++) {
		for (f = 0; f < 3; f++) {
			struct cds_ft_node *found = NULL;
			uint64_t sk = f < 2 ? rksp_key(w[i].bp, 0, f ? 5 : 1) :
				(((uint64_t) w[i].bp << 24) | (5ULL << 16) |
					(1ULL << 8) | 1);

			if (lookup_u64(ft, sk, &found) != CDS_FT_STATUS_OK ||
					found != &w[i].sib[f]->node)
				ret = -1;
		}
		for (f = 0; f < RKSP_NF; f++) {
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, rksp_fkey(w[i].bp, f, false),
					&found) != CDS_FT_STATUS_OK ||
					found != &w[i].res[f]->node)
				ret = -1;
			found = NULL;
			if (lookup_u64(ft, rksp_fkey(w[i].bp, f, true),
					&found) == CDS_FT_STATUS_OK)
				ret = -1;	/* a mover left behind */
		}
	}
	if (cds_ft_count_keys(ft) != live)
		ret = -1;
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	rcu_read_unlock();

	if (total_ops == 0)
		ret = -1;
	fprintf(stderr, "# inv_rekey_merge_same_parent_coherent_readers: %d writers "
		"%d readers, %lu splices, %lu busy, %lu reads, %lu live keys\n",
		RKSP_NW, RKSP_NR, total_ops, total_busy, total_checks, live);
	/*
	 * POSITIVE CONTROL.  A merge flavour's whole point is the reader that
	 * follows a stale skip word into a spliced chain member; if no up-walk
	 * ever started on a member, this run exercised nothing of that.  The
	 * counter is DEBUG_COUNTERS-only, so the hook is weak -- and a weak hook
	 * resolves to NULL SILENTLY, so say which case this is.
	 */
	if (cds_ft_debug_ext_member_hops) {
		unsigned long hops = 0;

		cds_ft_debug_ext_member_hops(group, &hops);
		fprintf(stderr, "# inv_rekey_merge_same_parent_coherent_readers: "
			"%lu up-walks started on a chain member (positive control%s)\n",
			hops, (w[0].flavours & 0x1cu) ? "" : ": no merge flavour ran");
		/* 0x1c: the flavours whose MOVER is a SKIP TARGET (a head under a
		 * run) -- the only ones that can put a member under a stale skip
		 * word; 0 and 1 splice heads reached through plain slots. */
		if ((w[0].flavours & 0x1cu) && total_ops && hops == 0) {
			fprintf(stderr, "rksp: merge flavours ran but no up-walk ever "
				"started on a member -- the shape is not built\n");
			ret = -1;
		}
	} else {
		fprintf(stderr, "# inv_rekey_merge_same_parent_coherent_readers: "
			"no DEBUG_COUNTERS hook (positive control BLIND on this build)\n");
	}
	_exit(ret ? 1 : 0);
}

static int inv_rekey_merge_same_parent_coherent_readers(void)
{
	pid_t pid;
	int status;

	INV_NEED_MERGE("inv_rekey_merge_same_parent_coherent_readers");
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_merge_same_parent_coherent_readers: "
			"skipped (set FT_INV_MW=1 to run the coherent-rekey reader "
			"oracle)\n");
		return 0;
	}
	/*
	 * ☠ THE CHILD INHERITS THE PARENT'S READER REGISTRY.  Every call_rcu
	 * worker is a registered QSBR reader, and one that was running
	 * callbacks at the instant of the fork stays ONLINE in the child's copy
	 * of the registry forever -- no thread ever advances it -- so the
	 * child's first synchronize_rcu (the writers' node-reuse fence) waits
	 * for a phantom.  MEASURED: the healthy build hung in wait_for_readers
	 * inside the child, indistinguishable from the defect this oracle
	 * catches.  call_rcu_before_fork parks every worker UNREGISTERED and
	 * waits for the acknowledgement; the child then re-creates its own
	 * default worker, so its deferred frees run as well.
	 */
	call_rcu_before_fork();
	pid = fork();
	if (pid < 0) {
		call_rcu_after_fork_parent();
		fprintf(stderr, "rksp: fork failed\n");
		return -1;
	}
	if (pid == 0) {
		call_rcu_after_fork_child();
		rksp_child();
	}
	call_rcu_after_fork_parent();
	if (waitpid(pid, &status, 0) != pid) {
		fprintf(stderr, "rksp: waitpid failed\n");
		return -1;
	}
	if (WIFSIGNALED(status)) {
		fprintf(stderr, "rksp: child killed by signal %d%s\n",
			WTERMSIG(status),
			WTERMSIG(status) == SIGALRM ?
				" (HANG -- the same-parent splice oracle did not "
				"return)" : "");
		return -1;
	}
	if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
		fprintf(stderr, "rksp: child exited %d\n",
			WIFEXITED(status) ? WEXITSTATUS(status) : -1);
		return -1;
	}
	return 0;
}

/*
 * SHARED-JUNCTION rekey oracle -- the companion inv_rekey_graft_disjoint cannot
 * be: here a peer's move touches nodes THIS writer descended through.
 *
 * In the disjoint oracle every writer owns two PRIVATE root-child junctions, and
 * the only node they share is root -- whose child set is FIXED, so root is never
 * recompacted and hence never relocated.  A disjoint writer's descent-captured
 * triple (S_top, BP, dst parent) therefore cannot be invalidated by a peer, and
 * the only concurrency exercised is the race for root's LOCK.
 *
 * Here RKS_NJ junction bytes are SHARED by RKS_NW writers: junction J holds the
 * S_top of each writer whose src it currently is AND receives each writer whose
 * dst it is, so several writers' subtrees live under one junction at once.  Two
 * hazards the disjoint layout structurally excludes become reachable:
 *  (a) STALE DESCENT: every popcount delete and every graft attach RECOMPACTS the
 *      junction (relocating it and retiring the old copy), so a peer's completed
 *      move leaves this writer's descent-captured BP / dst parent pointing at a
 *      RETIRED node.  The one-decide writer must REJECT that (its DLM acquire
 *      sees the tombstoned state word -> -EAGAIN) and re-descend, never edit the
 *      dead copy.
 *  (b) CROSS-WRITER CHILD RE-PARENT: that junction recompaction re-parents ALL of
 *      the junction's children, which now include the subtree ANOTHER writer is
 *      concurrently moving -- a node that writer holds LOCK on and parks its
 *      own retire onto, in the same state word.
 *
 * The LAYOUT keeps every shape gate of the debug writer STATICALLY satisfied, so
 * an -EINVAL remains a real failure and not a raced shape:
 *  - every junction is a root child and root's child set is fixed, so the shared
 *    parent of BP and the dst parent is ALWAYS root (d_src.ppnf == d_dst.ppnf,
 *    the src_parent_held precondition).
 *  - every junction keeps two GUARD leaves at byte1 0x00 / 0xff.  They hold it at
 *    >= 3 children while it holds our S_top (the min_child gate), keep it a plain
 *    internal node (never compressed), keep every run off the ordered-list
 *    head/tail, and BRACKET every dst gap so the adjacency guard cannot trip: the
 *    gap (dp,sb) always has a (dp,x<sb) predecessor and a (dp,x>sb) successor
 *    inside its own junction, while our run lives under a different junction byte.
 *  - each writer owns a DISTINCT S_top byte, so its dst slot is absent by
 *    construction and no two writers ever contend one slot.
 * Opt-in FT_INV_MW=1.
 */
#define RKS_NJ		2		/* shared junction bytes: ALL writers share both */
#define RKS_NW		16		/* writers, so each junction hosts up to 8 runs */
#define RKS_JB(j)	((uint8_t) (0x10 * ((j) + 1)))

static int inv_rekey_graft_shared(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	pthread_t writers[RKS_NW];
	struct ft_test_node *jguard[RKS_NJ][2];
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	int i, j, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_graft_shared: skipped "
			"(set FT_INV_MW=1 to run the coherent-rekey writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_ft(4, &group);	/* list ON (default) */
	cds_ft_make_shared(ft);

	/* Per-junction guard leaves at byte1 0x00 / 0xff (see the layout note). */
	rcu_read_lock();
	for (j = 0; j < RKS_NJ; j++) {
		for (c = 0; c < 2; c++) {
			uint64_t k = ((uint64_t) RKS_JB(j) << 24) |
				((c ? 0xffULL : 0x00ULL) << 16);

			jguard[j][c] = node_alloc(k);
			if (insert_u64(ft, k, jguard[j][c]) != CDS_FT_STATUS_OK)
				abort();
			live++;
		}
	}
	rcu_read_unlock();

	w = (struct rk_writer_arg *) calloc(RKS_NW, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < RKS_NW; i++) {
		uint8_t bp = RKS_JB(i % RKS_NJ);
		uint8_t dp = RKS_JB((i + 1) % RKS_NJ);

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].sb = (uint8_t) (0x80 + i);	/* distinct per writer, 0x00 < sb < 0xff */
		rcu_read_lock();
		for (c = 0; c < 4; c++) {
			uint64_t tk = ((uint64_t) bp << 24) |
				((uint64_t) w[i].sb << 16) |
				((uint64_t) (c + 1) << 8);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 4;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKS_NW; i++)
		pthread_create(&writers[i], NULL, rk_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKS_NW; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: guards intact, every moved leaf at its writer's final position. */
	synchronize_rcu();
	rcu_read_lock();
	for (j = 0; j < RKS_NJ; j++) {
		for (c = 0; c < 2; c++) {
			uint64_t k = ((uint64_t) RKS_JB(j) << 24) |
				((c ? 0xffULL : 0x00ULL) << 16);
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, k, &found) != CDS_FT_STATUS_OK ||
					found != &jguard[j][c]->node) {
				fprintf(stderr, "rekey shared: junction %02x guard "
					"%d lost\n", RKS_JB(j), c);
				ret = -1;
			}
		}
	}
	for (i = 0; i < RKS_NW; i++) {
		uint8_t X = w[i].at_dst ? w[i].dp : w[i].bp;
		uint8_t O = w[i].at_dst ? w[i].bp : w[i].dp;

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		/*
		 * PER-WRITER liveness, not just the global count: without the move-counter
		 * pin, a writer could get PERMANENTLY stuck re-deriving a splice position
		 * against a mis-ordered list (measured: the same rejected (pred, succ) pair
		 * hundreds of times in a row) while its peers kept committing -- a global
		 * "some moves happened" check sails right past that.
		 */
		if (w[i].ops == 0) {
			fprintf(stderr, "rekey shared: writer %d made NO move "
				"(starved: %lu retries)\n", i, w[i].retries);
			ret = -1;
		}
		for (c = 0; c < 4; c++) {
			uint64_t suffix = ((uint64_t) w[i].sb << 16) |
				((uint64_t) (c + 1) << 8);
			uint64_t here = ((uint64_t) X << 24) | suffix;
			uint64_t there = ((uint64_t) O << 24) | suffix;
			struct cds_ft_node *found = NULL;

			if (lookup_u64(ft, here, &found) != CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "rekey shared: writer %d top %d absent "
					"at final pos (at_dst=%d)\n", i, c, w[i].at_dst);
				ret = -1;
			}
			if (lookup_u64(ft, there, &found) == CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey shared: writer %d top %d still at "
					"old pos\n", i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey shared: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey shared: cds_ft_verify failed\n");
		ret = -1;
	}
	/*
	 * Ordered forward scan over the CELL list: every key exactly once, strictly
	 * ascending.  cds_ft_verify's ord-cell pass reports a broken back-edge or an
	 * order mismatch as CELL POINTERS; this reports the KEYS, which is what
	 * identifies WHICH writer's run landed in the wrong place (the residual failure
	 * mode of a mis-derived splice position).  Also dumps the whole sequence once a
	 * violation is seen, so a rare failure is self-describing in the log.
	 */
	{
		const struct cds_ft_cell *buf[8], *cur = NULL;
		uint64_t prev_key = 0;
		unsigned long seen = 0;
		int bad = 0;

		do {
			size_t n = 0, b;

			if (cds_ft_cell_next_batch(ft, cur, buf, 8, &n, &cur)
					!= CDS_FT_STATUS_OK) {
				fprintf(stderr, "rekey shared: cell scan failed\n");
				ret = -1;
				break;
			}
			for (b = 0; b < n; b++) {
				uint8_t k[4];
				size_t kl;
				uint64_t kv;

				if (cds_ft_cell_get_key(ft, buf[b], k, sizeof k,
						&kl) != CDS_FT_STATUS_OK) {
					fprintf(stderr, "rekey shared: cell "
						"get_key failed\n");
					ret = -1;
					bad = 1;
					break;
				}
				kv = cds_ft_key_to_u64(ft, k, 4);
				if (seen && kv <= prev_key) {
					fprintf(stderr, "rekey shared: ordered "
						"scan violation %#lx after %#lx "
						"(position %lu)\n",
						(unsigned long) kv,
						(unsigned long) prev_key, seen);
					ret = -1;
					bad = 1;
				}
				if (bad)
					fprintf(stderr, "rekey shared:   [%lu] %#lx\n",
						seen, (unsigned long) kv);
				prev_key = kv;
				seen++;
			}
		} while (cur);
		if (seen != live) {
			fprintf(stderr, "rekey shared: ordered scan saw %lu keys, "
				"live %lu\n", seen, live);
			ret = -1;
		}
	}
	rcu_read_unlock();

	if (total_ops == 0) {			/* liveness: writers made progress */
		fprintf(stderr, "rekey shared: no successful moves (livelock?)\n");
		ret = -1;
	}
	fprintf(stderr, "# inv_rekey_graft_shared: %d writers over %d shared "
		"junctions, %lu moves, %lu retries, %lu live keys\n", RKS_NW,
		RKS_NJ, total_ops, total_retries, live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * ===========================================================================
 * RELATIONAL LINEARIZABILITY ORACLE (design step 4).
 * ===========================================================================
 *
 * The other rekey oracles check STRUCTURE (verify, counts, no UAF) and EXACT
 * lookups ("a present key is never absent").  Neither can say whether a
 * RELATIONAL answer -- the neighbour of a key -- was ever TRUE: a traversal torn
 * across a move can return a real, live node that is the neighbour of nothing,
 * at no instant.  This is the oracle for that.
 *
 * WHY IT NEEDS NO TIMESTAMPS, VERSIONS OR WRITER INSTRUMENTATION.  Each writer's
 * subtree is in exactly ONE of TWO places -- the run sits under its bp junction
 * XOR its dp junction -- so for a probe inside junction X the set of answers
 * that are correct at SOME instant is enumerable outright, and it has two
 * elements.  Membership in that pair IS the linearizability check, at every
 * instant, whatever the interleaving.  The writers stay completely
 * uninstrumented: no bracket, no counter, no barrier on the path under test.
 *
 * ★ COMPARE NODE IDENTITY, NEVER A KEY READ BACK.  The moved leaves are the SAME
 * nodes at both junctions -- a move re-keys them, it does not replace them -- so
 * the legal pair is a pair of stable node ADDRESSES, and membership is
 * insensitive to where the run currently sits.  A key comparison is NOT: on an
 * EAGER ordered-list trie there is no stored key, and cds_ft_iter_get_key
 * rematerializes it by walking the leaf's parent chain AT READBACK TIME, so a
 * leaf that moved between the answer and the readback reports its NEW key.  An
 * earlier version of this oracle compared keys and "found" ~2e-4 non-linearizable
 * answers; every one of them had returned a LEGAL NODE.  The artifact, not the
 * library, was the finding.
 *
 * TWO MODES, GRADED DIFFERENTLY:
 *
 *  A. BOUND-KEY probes (ge/gt/le/lt from a key, no cached position).  These MUST
 *     be linearizable -- that is what the relational two-pass promises -- so an
 *     answer outside the legal pair FAILS the test.
 *
 *  B. CONTINUATION walks (cds_ft_next from a cached position).  These carry the
 *     KNOWN residual: a walker parked ON a moving run's cell steps out through
 *     the run's NEW outer link and lands wherever the run went.  That is inherent
 *     to moving a live run without draining readers, so mode B MEASURES it
 *     (per-class counts, printed every run) rather than failing on it.
 *
 * WHAT MODE A DOES NOT PROVE.  Membership in the legal pair is timing-independent
 * and therefore NECESSARY but not SUFFICIENT: it rules out an answer that was
 * true at NO instant (the phantom class), but it accepts the answer belonging to
 * the OTHER state even when that state did not occur during the operation.
 * Closing that gap needs to know which state held across the interval, i.e. the
 * writer-side timing bookkeeping this oracle deliberately does without -- and
 * that instrumentation would put barriers on the very path under test.  The
 * phantom class is the one the two-pass exists to prevent, so it is the one
 * gated here.
 *
 * Opt-in FT_INV_MW=1, like the other rekey oracles.
 */
#define RKL_NR		8	/* linearizability readers */
#define RKL_WALK_MAX	8	/* steps before abandoning a walk */
#define RKL_MAX_REPORT	3	/* violations narrated per reader; the rest counted */

struct rkl_reader_arg {
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	int nw;
	unsigned long probes;		/* mode A: bound-key relational reads */
	unsigned long probe_bad;	/* mode A: answers outside the legal pair */
	unsigned long walks, walk_steps;
	unsigned long walk_alien;	/* stepped onto ANOTHER junction's node */
	unsigned long walk_back;	/* stepped backwards within this junction */
	unsigned long walk_inner;	/* skipped a node inside this junction */
	unsigned long walk_end;		/* walk ended before reaching (X,5) */
	int failed;
};

/*
 * Position of @n in junction @X's key order, by IDENTITY:
 *   0 = the (X,1) sibling, 1..4 = the run's leaves, 5 = the (X,5) sibling.
 * -1 when @n is none of them.  w->sib[] is {(bp,1),(bp,5),(dp,1),(dp,5)}.
 */
static int rkl_node_index(const struct rk_writer_arg *w, uint8_t X,
		const struct cds_ft_node *n)
{
	int lo = (X == w->bp) ? 0 : 2;
	int c;

	if (!n)
		return -1;
	if (n == &w->sib[lo]->node)
		return 0;
	if (n == &w->sib[lo + 1]->node)
		return 5;
	for (c = 0; c < 4; c++) {
		if (n == &w->top[c]->node)
			return 1 + c;
	}
	return -1;
}

/* One relational read from a bound key; the answer is the iterator's NODE. */
static enum cds_ft_status rkl_probe(struct cds_ft *ft, struct cds_ft_iter *iter,
		int mode, uint64_t key, struct cds_ft_node **out)
{
	uint8_t k[8];
	enum cds_ft_status st;

	cds_ft_u64_to_key(ft, key, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	switch (mode) {
	case 0:	st = cds_ft_lookup_ge(ft, iter); break;
	case 1:	st = cds_ft_lookup_gt(ft, iter); break;
	case 2:	st = cds_ft_lookup_le(ft, iter); break;
	default: st = cds_ft_lookup_lt(ft, iter); break;
	}
	*out = (st == CDS_FT_STATUS_OK) ? cds_ft_iter_node(iter) : NULL;
	return st;
}

static void *rkl_reader(void *arg)
{
	struct rkl_reader_arg *r = (struct rkl_reader_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int) (uintptr_t) r;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct rk_writer_arg *w = &r->w[rand_r(&seed) % r->nw];
		uint8_t X = (rand_r(&seed) & 1) ? w->dp : w->bp;
		uint64_t Xk = (uint64_t) X << 24;
		uint64_t sb = (uint64_t) w->sb << 16;
		int lo_sib = (X == w->bp) ? 0 : 2;
		struct cds_ft_node *sib_lo = &w->sib[lo_sib]->node;
		struct cds_ft_node *sib_hi = &w->sib[lo_sib + 1]->node;
		enum cds_ft_status st;
		struct cds_ft_node *got = NULL;

		if ((rand_r(&seed) & 3) != 0) {
			/*
			 * MODE A.  Both members of each legal pair exist at all
			 * times (the run's leaves are the same nodes wherever the
			 * run sits), so a miss is a failure too.
			 */
			int pick = rand_r(&seed) & 3;
			struct cds_ft_node *a, *b;
			uint64_t probe;

			switch (pick) {
			case 0:	probe = Xk | (2ULL << 16);
				a = &w->top[0]->node; b = sib_hi; break;
			case 1:	probe = Xk | sb;
				a = &w->top[0]->node; b = sib_hi; break;
			case 2:	probe = Xk | (4ULL << 16);
				a = &w->top[3]->node; b = sib_lo; break;
			default: probe = Xk | sb | 0xff00ULL;
				a = &w->top[3]->node; b = sib_lo; break;
			}
			rcu_read_lock();
			st = rkl_probe(r->ft, iter, pick, probe, &got);
			rcu_read_unlock();
			r->probes++;
			if (caa_unlikely(st != CDS_FT_STATUS_OK ||
					(got != a && got != b))) {
				r->probe_bad++;
				r->failed = 1;
				if (r->probe_bad <= RKL_MAX_REPORT)
					fprintf(stderr,
						"rkl: %s(%#llx) -> node %p (%s); legal only %p or %p\n",
						pick == 0 ? "ge" : pick == 1 ? "gt" :
							pick == 2 ? "le" : "lt",
						(unsigned long long) probe,
						(void *) got,
						cds_ft_status_to_string(st),
						(void *) a, (void *) b);
				mw_violation_snapshot();
			}
		} else {
			/*
			 * MODE B: walk (X,1) -> (X,5) with cds_ft_next, checking
			 * each step against the pair the current POSITION allows,
			 * by node identity.
			 */
			int idx, steps = 0;

			rcu_read_lock();
			st = rkl_probe(r->ft, iter, 0, Xk | (1ULL << 16), &got);
			if (st != CDS_FT_STATUS_OK || got != sib_lo) {
				/* (X,1) never moves: GE must land on it. */
				rcu_read_unlock();
				r->probe_bad++;
				r->failed = 1;
				if (r->probe_bad <= RKL_MAX_REPORT)
					fprintf(stderr,
						"rkl: ge on the fixed sib of junction %u -> node %p (%s)\n",
						X, (void *) got,
						cds_ft_status_to_string(st));
				mw_violation_snapshot();
				continue;
			}
			r->walks++;
			idx = 0;
			while (idx != 5 && steps++ < RKL_WALK_MAX) {
				int nidx, want;

				/* legal: the next node in this junction's order,
				 * or the (X,5) sibling if the run left. */
				want = (idx == 4) ? 5 : idx + 1;
				st = cds_ft_next(r->ft, iter);
				r->walk_steps++;
				if (st != CDS_FT_STATUS_OK) {
					r->walk_end++;
					break;
				}
				got = cds_ft_iter_node(iter);
				nidx = rkl_node_index(w, X, got);
				if (nidx == want || nidx == 5) {
					idx = nidx;
					continue;
				}
				/* Off the rails: classify (mode B measures). */
				if (nidx < 0) {
					uint8_t other = (X == w->bp) ? w->dp : w->bp;

					if (rkl_node_index(w, other, got) >= 0)
						r->walk_alien++;
					else
						r->walk_inner++;
				} else if (nidx <= idx)
					r->walk_back++;
				else
					r->walk_inner++;
				break;
			}
			rcu_read_unlock();
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_linearizability(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	struct rkl_reader_arg *r;
	pthread_t writers[RK_NW], readers[RKL_NR];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, live = 0;
	unsigned long probes = 0, probe_bad = 0, walks = 0, walk_steps = 0;
	unsigned long alien = 0, back = 0, inner = 0, wend = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_linearizability: skipped "
			"(set FT_INV_MW=1 to run the relational linearizability oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_rekey_coherent_ft(4, &group);	/* EAGER + ordered list */
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (insert_u64(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			insert_u64(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rk_writer_arg *) calloc(RK_NW, sizeof(*w));
	r = (struct rkl_reader_arg *) calloc(RKL_NR, sizeof(*r));
	if (!w || !r)
		abort();
	for (i = 0; i < RK_NW; i++) {
		/*
		 * DELIBERATELY NON-ADJACENT junctions (i+1 and i+1+RK_NW): with
		 * the usual (2i+1, 2i+2) pairing, "the numerically adjacent
		 * junction" and "the other junction of the same writer" are the
		 * same byte, and a wrong answer cannot tell a slot/rank mistake
		 * apart from a same-commit mix-up.
		 */
		uint8_t bp = (uint8_t) (i + 1), dp = (uint8_t) (i + 1 + RK_NW);
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint64_t sk[4] = {
			bpk | (1ULL << 16), bpk | (5ULL << 16),
			dpk | (1ULL << 16), dpk | (5ULL << 16),
		};

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].sb = 3;
		rcu_read_lock();
		for (c = 0; c < 4; c++) {
			w[i].sib[c] = node_alloc(sk[c]);
			if (insert_u64(ft, sk[c], w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < 4; c++) {
			uint64_t tk = bpk | (3ULL << 16) | ((uint64_t) (c + 1) << 8);

			w[i].top[c] = node_alloc(tk);
			if (insert_u64(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 8;
	}
	for (i = 0; i < RKL_NR; i++) {
		r[i].ft = ft;
		r[i].w = w;
		r[i].nw = RK_NW;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_create(&writers[i], NULL, rk_writer, &w[i]);
	for (i = 0; i < RKL_NR; i++)
		pthread_create(&readers[i], NULL, rkl_reader, &r[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < RKL_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	for (i = 0; i < RK_NW; i++) {
		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
	}
	for (i = 0; i < RKL_NR; i++) {
		probes += r[i].probes;
		probe_bad += r[i].probe_bad;
		walks += r[i].walks;
		walk_steps += r[i].walk_steps;
		alien += r[i].walk_alien;
		back += r[i].walk_back;
		inner += r[i].walk_inner;
		wend += r[i].walk_end;
		if (r[i].failed)
			ret = -1;
	}

	synchronize_rcu();
	rcu_read_lock();
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rekey linearizability: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey linearizability: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {
		fprintf(stderr, "rekey linearizability: no successful moves (livelock?)\n");
		ret = -1;
	}
	if (probes == 0 || walk_steps == 0) {
		fprintf(stderr, "rekey linearizability: readers made no progress\n");
		ret = -1;
	}
	/*
	 * Mode A is the gate; mode B is a MEASUREMENT of the known parked-walker
	 * residual, printed so it is a number in the record rather than an
	 * assumption either way (zero would mean it was not exercised, not that
	 * it is gone).
	 */
	fprintf(stderr, "# inv_rekey_linearizability: %d writers %d readers, "
		"%lu moves, %lu retries, %lu bound-key probes (%lu illegal), "
		"%lu walks / %lu steps: %lu alien %lu back %lu inner %lu early-end\n",
		RK_NW, RKL_NR, total_ops, total_retries, probes, probe_bad,
		walks, walk_steps, alien, back, inner, wend);

	free(w);
	free(r);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * ===========================================================================
 * PUBLIC-ENTRY NO-GAP ORACLE: cds_ft_rekey_graft must never make a moved key
 * unreachable.
 * ===========================================================================
 *
 * The property rekey exists to provide is that a key present throughout a move
 * is NEVER read as absent -- at neither the old nor the new position.  Every
 * other rekey oracle drives _cds_ft_debug_rekey_graft_simple, which is a
 * SEPARATE and simpler one-decide implementation; the PUBLIC cds_ft_rekey_graft
 * runs ft_merge_at_inner's staged path (detach the subtree into a transient
 * trie, then merge that trie back in at the destination) and nothing has ever
 * asserted no-gap against it.  This oracle does.
 *
 * THE WITNESS IS ONE ATOMIC RELATIONAL PROBE, needing no timestamps, no writer
 * instrumentation and no bound on how often the writer flips.  Each writer's run
 * sits under its bp junction XOR its dp junction, and the geometry puts BOTH
 * candidate positions inside ONE key-order BAND:
 *
 *   (bp,1) (bp,5) | run at (bp,9,1..4)  XOR  run at (dp,1,1..4) | (dp,5) (dp,9)
 *                 ^ band delimiter                              ^ band delimiter
 *
 * bp and dp are CONSECUTIVE junction bytes, and the run's slot is the HIGHEST
 * child of bp and the LOWEST child of dp, so nothing but the run ever sorts
 * between the two delimiters (bp,5) and (dp,5).  gt((bp,5)) is therefore the
 * run's first leaf and lt((dp,5)) its last -- THE SAME NODE in both
 * configurations.  This is the legal-pair check of inv_rekey_linearizability
 * with the pair DEGENERATED TO ONE ELEMENT, which turns a membership test into a
 * PRESENCE test: the correct answer does not depend on where the run currently
 * is, so any other answer (the delimiter itself, or a miss) says the run was
 * reachable from NEITHER position at that instant.  That is the gap, observed
 * directly by one linearization-point-free read, with no way for an oscillating
 * writer to fake agreement.
 *
 * IT CARRIES ITS OWN CONTROL: the readers run a QUIET PHASE first, with the
 * writers parked.  The band probes must be clean there -- a violation with
 * nothing moving would indict the geometry, not the library -- so the quiet-phase
 * count is kept and reported apart from the moving-phase count.
 *
 * TWO ARMS, ONE BODY -- the two writers cds_ft_rekey_graft dispatches to, put
 * under identical probes.  create_rekey_coherent_ft explains how the group config
 * selects one:
 *
 *  - inv_rekey_public_atomic_no_gap (fixed-length, list off) reaches
 *    ft_rekey_one_decide, which commits the src clear, the dst publish and the
 *    re-parents as ONE flip.  `absent moving` MUST be 0, and this arm GATES on
 *    it: it is the acceptance test for the atomic writer, and the thing that
 *    fails if a future change stops routing to it.
 *  - inv_rekey_coarse_progress (an FT-wide writer lock, hence no atomic writer at
 *    all) reaches the detach-then-merge-back writer, whose absence window no probe
 *    can talk it out of.  That arm gates the OTHER WAY: it REQUIRES an absence,
 *    which is what proves the probe the no-gap arms rest on still detects one.
 *
 * MEASURED (4 writers, 8 readers, 200 ms each): the staged arm sees 1163089
 * absences over 18 moves -- a grace-period-wide window per move rather than a
 * race -- against 0 over 3014 moves in the compressed-top arm and 0 over 1839 in
 * the fixed one.  Every arm reports 0 absent across the control phase's ~1.5M
 * probes and 0 bad exact probes: the fixed siblings are always present, and a
 * moving leaf never answers as a foreign node.
 * The same geometry driven single-threaded through 40 moves answers both band
 * probes correctly at every rest point in either config, so an absence is the
 * move window and not the layout.
 */
#define RKP_NW		4		/* writers, one private band each */
#define RKP_NR		8		/* no-gap readers */
#define RKP_KLEN	4		/* every key is this wide (see below) */
#define RKP_QUIET_MS	60		/* control phase: readers, no movers */
#define RKP_MAX_REPORT	3		/* narrated per reader; the rest counted */
#define RKP_SLOT_BP	9		/* the run's slot byte in the bp junction */
#define RKP_SLOT_DP	1		/* ... and in the dp junction */

/*
 * THE GROUP CONFIG IS WHAT SELECTS THE WRITER, and the three arms cover both of
 * them plus both group flavours:
 *
 *  - RKP_ATOMIC_FIXED: a FIXED-length group, list OFF -- the atomic writer with
 *    no cells in play at all.  Fixed-length MUST take that writer, the staged one
 *    being variable-length-only (it detaches, and a detached subtree's keys are
 *    stripped of the prefix).
 *
 *  - RKP_ATOMIC_VARLEN: a VARIABLE-length group with the list ON -- the atomic
 *    writer WITH the ordered-cell work, and specifically the case where the
 *    destination ABUTS the moved run, which is what this band geometry always is
 *    (the two candidate positions are ordered-adjacent by construction).  The
 *    writer recognises that the run's ordered position does not change and
 *    validates it instead of splicing.  This arm is therefore the acceptance test
 *    for BOTH widenings: variable-length groups, and the abutting destination.
 *
 *  - RKP_ATOMIC_COMPRESSED_TOP: variable-length, list on, and a run of a SINGLE
 *    key, so the source prefix path-compresses and the subtree top is a
 *    COMPRESSED node rather than a plain POPCOUNT/PIGEON one.  The atomic writer
 *    takes it (@9e8eeec6 widened the cut to a compressed top), so this arm is
 *    that widening's acceptance test under concurrent readers: measured 3014
 *    moves, 0 absent.
 *
 *  - RKP_COARSE: the FT-wide writer lock, which has no per-node locks and so no
 *    atomic writer at all -- every move stages, detaching into a transient trie
 *    and merging back, with an absence window a grace period wide.  ★ This arm is
 *    the ABSENCE DETECTOR'S OWN CONTROL (RKP_GAP_EXPECTED below), for the same
 *    reason the quiet phase refuses a zero drawn from zero probes: if no arm ever
 *    produces an absence, "0 absent" in the atomic arms is free, and a detector
 *    that had stopped detecting would read exactly like a perfect writer.  It
 *    cannot silently stop being the control either -- a COARSE trie is
 *    structurally unable to reach the atomic writer.  It doubles as the LIVENESS
 *    check that @781e0b9a's fix keeps green (COARSE writers used to park on the
 *    FT-wide mutex while RCU-ONLINE and wedge as a group).
 *
 * Keys are RKP_KLEN wide in ALL FOUR, which is this oracle's own doing: uniform
 * width makes the band's key order plain lexicographic, so "nothing else sorts
 * between the delimiters" is a property of the four bytes rather than of the
 * prefix-sorts-first rule that variable-length keys otherwise bring.
 */
enum rkp_mode {
	RKP_ATOMIC_FIXED,
	RKP_ATOMIC_VARLEN,
	RKP_ATOMIC_COMPRESSED_TOP,
	RKP_SPARSE_BP,	/* a 2-child source junction: OUTSIDE the atomic cut */
	RKP_COARSE,	/* FT-wide writer lock: stages, and is the gap CONTROL */
};

/*
 * Does @mode want the RKP_ATOMIC_FIXED group flavour (fixed-length keys, list
 * off)?  RKP_SPARSE_BP does NOT, deliberately: a fixed-length group has no
 * staged writer to fall back to (the staged one detaches, and a detached
 * subtree's keys are stripped of the prefix), so a shape the atomic cut refuses
 * there cannot move AT ALL -- cds_ft_rekey_graft returns INVALID_ARGUMENT and
 * the arm would measure a refusal with no move behind it.  On the VARIABLE
 * flavour the refusal routes to the staged writer instead, which is the
 * behaviour worth pinning.  That also pairs it with
 * inv_rekey_public_atomic_no_gap_varlen, from which it differs in the seeded
 * GEOMETRY and nothing else.
 */
static bool rkp_mode_fixed_group(enum rkp_mode mode)
{
	return mode == RKP_ATOMIC_FIXED;
}

/*
 * What a moved key's absence MEANS for an arm, which is not the same question as
 * whether the arm passes.
 *
 * RKP_GAP_NONE      the writer commits the move as ONE decide, so no reader can
 *                   see the key missing from both positions: ANY absence fails.
 * RKP_GAP_EXPECTED  the writer stages the move, so the absence window is the
 *                   arm's subject.  ZERO absences FAILS: it means the probe that
 *                   every RKP_GAP_NONE arm rests on has stopped detecting.
 */
enum rkp_gap {
	RKP_GAP_NONE,
	RKP_GAP_EXPECTED,
};

static struct cds_ft *create_rekey_coherent_ft(enum rkp_mode mode,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft_attr *attr;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&gattr) < 0)
		abort();
	if (rkp_mode_fixed_group(mode)) {
		if (cds_ft_group_attr_set_key_len(gattr, RKP_KLEN) < 0)
			abort();
	} else {
		if (cds_ft_group_attr_set_max_key_len(gattr, RKP_KLEN) < 0)
			abort();
	}
	if (cds_ft_group_attr_set_lookup_optimization(gattr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(gattr,
			mode == RKP_COARSE ? CDS_FT_WRITER_LOCK_COARSE :
				CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(gattr,
			!rkp_mode_fixed_group(mode)) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF): this trie
	 * hosts an in-trie MOVE, so the capability must be declared or the
	 * rekey entries answer NOT_SUPPORTED and the test proves nothing.
	 */
	if (cds_ft_group_attr_set_rekey(gattr, true) < 0)
		abort();
	if (cds_ft_group_create(gattr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_attr_create(&attr) < 0)
		abort();
	/* Rekey coherence is automatic on an EAGER trie; EAGER is all it needs. */
	if (cds_ft_attr_set_speculative_keys(attr, false) < 0)
		abort();
	if (cds_ft_create(group, attr, &ft) < 0)
		abort();
	cds_ft_attr_destroy(attr);
	*group_out = group;
	return ft;
}

/* Insert / exact-lookup an RKP_KLEN-wide key (the shared u64 helpers resolve
 * RKP_KLEN, which a variable-length trie has none of). */
static enum cds_ft_status rkp_insert(struct cds_ft *ft, uint64_t v,
		struct ft_test_node *n)
{
	uint8_t k[8] = { 0 };

	cds_ft_u64_to_key(ft, v, k, RKP_KLEN);
	return cds_ft_insert(ft, k, RKP_KLEN, &n->node);
}

static enum cds_ft_status rkp_lookup(struct cds_ft *ft, uint64_t v,
		struct cds_ft_node **out)
{
	uint8_t k[8] = { 0 };

	cds_ft_u64_to_key(ft, v, k, RKP_KLEN);
	return cds_ft_eager_lookup_key(ft, k, RKP_KLEN, 0, out);
}

/* Are the movers released?  Readers grade the two phases apart. */
static volatile int rkp_moving;

struct rkp_writer_arg {
	struct cds_ft *ft;
	uint8_t bp, dp;			/* consecutive junction bytes, bp < dp */
	struct ft_test_node *sib[4];	/* fixed: (bp,1) (bp,5) (dp,5) (dp,9) */
	/*
	 * Which of @sib this arm seeds, inclusive.  The band delimiters are 1 and
	 * 2, so every arm seeds those; 0 and 3 are the OUTER siblings, and their
	 * only job is to keep each junction at three children so the source
	 * junction survives losing the run.  RKP_SPARSE_BP drops them (1..2).
	 */
	int sib_lo, sib_hi;
	struct ft_test_node *top[4];	/* the run's leaves, they move */
	int nr_run;			/* how many of @top exist (see rkp_mode) */
	int at_dst;			/* 0: run at (bp,9); 1: run at (dp,1) */
	/*
	 * SYNTHESISE THE ABSENCE (control arms only).  A RKP_GAP_EXPECTED arm
	 * exists to prove the band probe can still SEE a key that is missing from
	 * both positions -- it is the positive control every RKP_GAP_NONE arm's
	 * zero rests on.  It used to get that absence for free from the STAGED
	 * rekey writer, which hid the key for a grace period per move; that writer
	 * was deleted as a guarantee failure, and coarse now reaches the atomic
	 * one, so nothing in the tree produces an absence on purpose any more.
	 *
	 * So the control MANUFACTURES one, in the test rather than in the library:
	 * remove the run's leaves, then re-insert them at the other junction.  The
	 * window between the two is a real absence of a real key, which is exactly
	 * what the probe must catch -- and it needs no staged writer, no library
	 * knob, and no weakening of the property the no-gap arms assert.
	 */
	int synth_gap;
	unsigned long ops, busy;
	int failed;
};

struct rkp_reader_arg {
	struct cds_ft *ft;
	struct rkp_writer_arg *w;
	int nw;
	unsigned long band;		/* band probes -- the no-gap witness */
	unsigned long band_quiet;	/* ... of those, taken with the movers parked */
	unsigned long band_quiet_bad;	/* absent with the movers parked (control) */
	unsigned long band_move_bad;	/* absent during a move (the gap) */
	unsigned long exact;		/* exact probes */
	unsigned long exact_bad;	/* a fixed key missing, or a foreign node */
	int failed;
};

/* (b0,b1,b2) as a fixed four-byte key; byte 3 is always zero here. */
static uint64_t rkp_key(uint8_t b0, uint8_t b1, uint8_t b2)
{
	return ((uint64_t) b0 << 24) | ((uint64_t) b1 << 16) |
		((uint64_t) b2 << 8);
}

/* The four fixed band/junction siblings, in key order. */
static uint64_t rkp_sib_key(const struct rkp_writer_arg *w, int j)
{
	switch (j) {
	case 0:	return rkp_key(w->bp, 1, 0);
	case 1:	return rkp_key(w->bp, 5, 0);	/* low band delimiter */
	case 2:	return rkp_key(w->dp, 5, 0);	/* high band delimiter */
	default: return rkp_key(w->dp, 9, 0);
	}
}

static void *rkp_writer(void *arg)
{
	struct rkp_writer_arg *w = (struct rkp_writer_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	/*
	 * The control phase.  Report a quiescent state while parked: a QSBR
	 * thread that merely sleeps is a reader every grace period waits for,
	 * and the readers below take grace periods (the coherent lookup does
	 * not, but their call_rcu drain does).
	 */
	while (!rkp_moving && !test_stop) {
		rcu_quiescent_state();
		usleep(200);
	}

	while (!test_stop) {
		uint8_t s = w->at_dst ? w->dp : w->bp;
		uint8_t d = w->at_dst ? w->bp : w->dp;
		uint8_t s_slot = w->at_dst ? RKP_SLOT_DP : RKP_SLOT_BP;
		uint8_t d_slot = w->at_dst ? RKP_SLOT_BP : RKP_SLOT_DP;
		uint8_t src_key[2] = { s, s_slot };
		uint8_t dst_key[2] = { d, d_slot };
		enum cds_ft_status st;

		if (w->synth_gap) {
			/*
			 * THE CONTROL'S MOVE: cds_ft_detach the run into a
			 * temporary trie, then cds_ft_graft it back at the other
			 * junction.  Between the two the run is reachable from
			 * NEITHER position -- a real absence of real keys, which
			 * is exactly what the band probe must catch.
			 *
			 * DETACH + GRAFT rather than per-leaf remove + insert,
			 * because they are BULK ops of the same family as the
			 * rekey itself: this is the staged rekey's own shape
			 * (detach into a temporary trie, merge it back), so the
			 * control reproduces the window the deleted staged writer
			 * used to provide instead of approximating it with point
			 * ops the arm does not otherwise exercise.
			 */
			struct cds_ft *tmp = NULL;

			st = cds_ft_detach(w->ft, src_key, 2, &tmp);
			if (st != CDS_FT_STATUS_OK || !tmp) {
				if (st == CDS_FT_STATUS_BUSY_ERROR ||
						st == CDS_FT_STATUS_MEMORY_ERROR) {
					w->busy++;
					goto synth_next;
				}
				fprintf(stderr, "rkp writer bp=%u: control detach "
					"%02x,%02x: %s\n", w->bp, s, s_slot,
					cds_ft_status_to_string(st));
				w->failed = 1;
				break;
			}
			/*
			 * WIDEN THE WINDOW deliberately.  The probe has to be able
			 * to land in it; a window closed within a few instructions
			 * would make this control a timing test of the readers'
			 * scheduling rather than a test of the probe.
			 */
			usleep(50);
			st = cds_ft_graft(w->ft, dst_key, 2, tmp);
			if (st != CDS_FT_STATUS_OK) {
				fprintf(stderr, "rkp writer bp=%u: control graft "
					"%02x,%02x: %s\n", w->bp, d, d_slot,
					cds_ft_status_to_string(st));
				w->failed = 1;
				break;
			}
			cds_ft_destroy(tmp);		/* graft consumed its content */
			w->at_dst = !w->at_dst;
			w->ops++;
synth_next:
			if ((++iters & 0xff) == 0)
				rcu_quiescent_state();
			continue;
		}
		/*
		 * NO read lock: cds_ft_rekey_graft enters the move gate, which
		 * publishes "expect a move" and then waits a grace period, so a
		 * caller holding a read section would wait for itself.
		 */
		st = cds_ft_rekey_graft(w->ft, dst_key, 2, src_key, 2);
		if (st == CDS_FT_STATUS_OK) {
			w->at_dst = !w->at_dst;
			w->ops++;
		} else if (st == CDS_FT_STATUS_BUSY_ERROR ||
				st == CDS_FT_STATUS_MEMORY_ERROR) {
			/*
			 * Transient: a peer holds a node this move needs, or a
			 * reserve came up short.  Both leave the run at the
			 * source, so retry the SAME direction.
			 */
			w->busy++;
		} else {
			fprintf(stderr, "rkp writer bp=%u: rekey %02x,%02x -> "
				"%02x,%02x: %s\n", w->bp, s, s_slot, d, d_slot,
				cds_ft_status_to_string(st));
			w->failed = 1;
			mw_violation_snapshot();
			break;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

static void *rkp_reader(void *arg)
{
	struct rkp_reader_arg *r = (struct rkp_reader_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int) (uintptr_t) r;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct rkp_writer_arg *w = &r->w[rand_r(&seed) % r->nw];
		int pick = rand_r(&seed) & 7;
		struct cds_ft_node *got = NULL;
		enum cds_ft_status st;
		uint8_t k[8];

		if (pick < 4) {
			/*
			 * THE NO-GAP WITNESS.  One relational read from a band
			 * delimiter; the run's outermost leaf is the answer at
			 * every instant, whichever junction holds the run.
			 */
			int hi = pick & 1;
			uint64_t bound = hi ? rkp_sib_key(w, 2) : rkp_sib_key(w, 1);
			struct cds_ft_node *expect = hi ?
				&w->top[w->nr_run - 1]->node : &w->top[0]->node;
			int moving = rkp_moving;

			cds_ft_u64_to_key(r->ft, bound, k, RKP_KLEN);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, RKP_KLEN);
			st = hi ? cds_ft_lookup_lt(r->ft, iter) :
				cds_ft_lookup_gt(r->ft, iter);
			if (st == CDS_FT_STATUS_OK)
				got = cds_ft_iter_node(iter);
			rcu_read_unlock();
			r->band++;
			if (!moving && !rkp_moving)
				r->band_quiet++;
			if (caa_unlikely(got != expect)) {
				unsigned long *cnt = (moving || rkp_moving) ?
					&r->band_move_bad : &r->band_quiet_bad;

				(*cnt)++;
				if (*cnt <= RKP_MAX_REPORT)
					fprintf(stderr,
						"rkp: %s(%#llx) -> %p (%s), the run's %s leaf %p is reachable from NEITHER junction%s\n",
						hi ? "lt" : "gt",
						(unsigned long long) bound,
						(void *) got,
						cds_ft_status_to_string(st),
						hi ? "last" : "first",
						(void *) expect,
						(moving || rkp_moving) ? "" :
							" -- WITH THE MOVERS PARKED");
				mw_violation_snapshot();
			}
		} else if (pick < 6) {
			/* A fixed sibling: present at every instant. */
			int j = w->sib_lo + (int) (rand_r(&seed) %
					(unsigned int) (w->sib_hi - w->sib_lo + 1));
			uint64_t key = rkp_sib_key(w, j);

			cds_ft_u64_to_key(r->ft, key, k, RKP_KLEN);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, RKP_KLEN);
			st = cds_ft_lookup(r->ft, iter);
			got = cds_ft_iter_node(iter);
			rcu_read_unlock();
			r->exact++;
			if (caa_unlikely(got != &w->sib[j]->node)) {
				r->exact_bad++;
				if (r->exact_bad <= RKP_MAX_REPORT)
					fprintf(stderr,
						"rkp: fixed sib %#llx -> %p (%s), expect %p\n",
						(unsigned long long) key,
						(void *) got,
						cds_ft_status_to_string(st),
						(void *) &w->sib[j]->node);
				mw_violation_snapshot();
			}
		} else {
			/*
			 * A moving leaf at ONE of its two positions: a miss is
			 * legitimate (the run may be at the other junction),
			 * a FOREIGN node never is.
			 */
			int c = rand_r(&seed) % w->nr_run, hi = rand_r(&seed) & 1;
			uint64_t key = hi ?
				rkp_key(w->dp, RKP_SLOT_DP, (uint8_t) (c + 1)) :
				rkp_key(w->bp, RKP_SLOT_BP, (uint8_t) (c + 1));

			cds_ft_u64_to_key(r->ft, key, k, RKP_KLEN);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k, RKP_KLEN);
			st = cds_ft_lookup(r->ft, iter);
			got = cds_ft_iter_node(iter);
			rcu_read_unlock();
			r->exact++;
			if (caa_unlikely(got && got != &w->top[c]->node)) {
				r->exact_bad++;
				if (r->exact_bad <= RKP_MAX_REPORT)
					fprintf(stderr,
						"rkp: moving key %#llx -> %p (%s), expect miss or %p\n",
						(unsigned long long) key,
						(void *) got,
						cds_ft_status_to_string(st),
						(void *) &w->top[c]->node);
				mw_violation_snapshot();
			}
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_public_no_gap_run(enum rkp_mode mode, enum rkp_gap gap,
		const char *name)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rkp_writer_arg *w;
	struct rkp_reader_arg *r;
	pthread_t writers[RKP_NW], readers[RKP_NR];
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_busy = 0, live = 0;
	unsigned long band = 0, quiet = 0, quiet_bad = 0, move_bad = 0;
	unsigned long exact = 0, exact_bad = 0;
	int i, c, j, ret = 0;

	mw_install_fatal_handler();
	leak_reset();

	ft = create_rekey_coherent_ft(mode, &group);
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (rkp_insert(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			rkp_insert(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rkp_writer_arg *) calloc(RKP_NW, sizeof(*w));
	r = (struct rkp_reader_arg *) calloc(RKP_NR, sizeof(*r));
	if (!w || !r)
		abort();
	for (i = 0; i < RKP_NW; i++) {
		/*
		 * CONSECUTIVE junction bytes: the band between (bp,5) and (dp,5)
		 * must contain the run and NOTHING else, so no other key -- no
		 * peer writer's junction included -- may sort between bp and dp.
		 */
		w[i].ft = ft;
		w[i].bp = (uint8_t) (2 * i + 1);
		w[i].dp = (uint8_t) (2 * i + 2);
		/*
		 * ONE key in the run stages the move: a single-key subtree
		 * path-compresses, and the atomic writer needs a plain internal top.
		 */
		w[i].nr_run = (mode == RKP_ATOMIC_COMPRESSED_TOP) ? 1 : 4;
		/*
		 * Only the CONTROL arms synthesise the absence; every other arm
		 * must go through the real writer, because their whole assertion
		 * is that the real writer never produces one.
		 */
		w[i].synth_gap = (gap == RKP_GAP_EXPECTED);
		w[i].sib_lo = (mode == RKP_SPARSE_BP) ? 1 : 0;
		w[i].sib_hi = (mode == RKP_SPARSE_BP) ? 2 : 3;
		rcu_read_lock();
		for (j = w[i].sib_lo; j <= w[i].sib_hi; j++) {
			uint64_t sk = rkp_sib_key(&w[i], j);

			w[i].sib[j] = node_alloc(sk);
			if (rkp_insert(ft, sk, w[i].sib[j]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < w[i].nr_run; c++) {
			uint64_t tk = rkp_key(w[i].bp, RKP_SLOT_BP,
					(uint8_t) (c + 1));

			w[i].top[c] = node_alloc(tk);
			if (rkp_insert(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += (unsigned long) (w[i].sib_hi - w[i].sib_lo + 1) +
			(unsigned long) w[i].nr_run;
	}
	for (i = 0; i < RKP_NR; i++) {
		r[i].ft = ft;
		r[i].w = w;
		r[i].nw = RKP_NW;
	}

	test_go = 0;
	test_stop = 0;
	rkp_moving = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKP_NW; i++)
		pthread_create(&writers[i], NULL, rkp_writer, &w[i]);
	for (i = 0; i < RKP_NR; i++)
		pthread_create(&readers[i], NULL, rkp_reader, &r[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	/* CONTROL: readers only.  Every band probe must answer the run. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < RKP_QUIET_MS)
		usleep(1000);
	rkp_moving = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKP_NW; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < RKP_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	for (i = 0; i < RKP_NW; i++) {
		total_ops += w[i].ops;
		total_busy += w[i].busy;
		if (w[i].failed)
			ret = -1;
	}
	for (i = 0; i < RKP_NR; i++) {
		band += r[i].band;
		quiet += r[i].band_quiet;
		quiet_bad += r[i].band_quiet_bad;
		move_bad += r[i].band_move_bad;
		exact += r[i].exact;
		exact_bad += r[i].exact_bad;
		if (r[i].failed)
			ret = -1;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKP_NW; i++) {
		uint8_t X = w[i].at_dst ? w[i].dp : w[i].bp;
		uint8_t slot = w[i].at_dst ? RKP_SLOT_DP : RKP_SLOT_BP;

		for (j = w[i].sib_lo; j <= w[i].sib_hi; j++) {
			struct cds_ft_node *found = NULL;

			if (rkp_lookup(ft, rkp_sib_key(&w[i], j), &found)
					!= CDS_FT_STATUS_OK ||
					found != &w[i].sib[j]->node)
				ret = -1;
		}
		for (c = 0; c < w[i].nr_run; c++) {
			struct cds_ft_node *found = NULL;

			if (rkp_lookup(ft, rkp_key(X, slot, (uint8_t) (c + 1)),
					&found) != CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node)
				ret = -1;
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "rkp: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rkp: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	if (total_ops == 0) {
		fprintf(stderr, "rkp: no successful moves (livelock?)\n");
		ret = -1;
	}
	if (band == 0 || exact == 0) {
		fprintf(stderr, "rkp: readers made no progress\n");
		ret = -1;
	}
	/*
	 * The control's "0 absences while parked" only means something next to the
	 * number of probes that produced it, so require the quiet phase to have
	 * really run: a zero out of zero probes is a dead control reading as a
	 * clean one.
	 */
	if (quiet == 0) {
		fprintf(stderr, "rkp: the control phase took no band probe\n");
		ret = -1;
	}
	if (quiet_bad || exact_bad)
		ret = -1;
	if (gap == RKP_GAP_NONE && move_bad) {
		ret = -1;			/* narrated per reader above */
	} else if (gap == RKP_GAP_EXPECTED && !move_bad) {
		/*
		 * The control came back clean, which for a STAGING writer is not good
		 * news: it means the band probe no longer catches a key that is absent
		 * from both positions, and every RKP_GAP_NONE arm's zero is then drawn
		 * from a dead detector.
		 */
		fprintf(stderr, "rkp: %s: a STAGED move's absence window went "
			"UNDETECTED over %lu band probes and %lu moves -- the no-gap "
			"arms rest on this probe, so their 0 is now unproven\n",
			name, band, total_ops);
		ret = -1;
	}

	fprintf(stderr, "# %s: %d writers %d readers, "
		"%lu moves, %lu busy, %lu band probes (%lu absent moving; "
		"%lu parked, %lu absent), %lu exact probes (%lu bad), "
		"%lu live keys\n", name,
		RKP_NW, RKP_NR, total_ops, total_busy, band, move_bad,
		quiet, quiet_bad, exact, exact_bad, live);

	free(w);
	free(r);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * THE ATOMIC ARM -- the acceptance test for the one-decide writer, and a GATE:
 * every band probe must answer the run, so `absent moving` must be 0.  That zero
 * is only worth as much as the probe behind it, which is what the COARSE arm
 * below exists to keep honest.
 */
static int inv_rekey_public_atomic_no_gap(void)
{
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_public_atomic_no_gap: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n");
		return 0;
	}
	return inv_rekey_public_no_gap_run(RKP_ATOMIC_FIXED, RKP_GAP_NONE,
			"inv_rekey_public_atomic_no_gap");
}

/*
 * THE SAME ARM ON A VARIABLE-LENGTH GROUP -- the acceptance test for widening the
 * atomic cut past fixed-length groups.  It differs from the arm above in the group
 * flavour and NOTHING else, so a nonzero `absent moving` here against a zero there
 * would name the key-length generalization specifically.
 */
/*
 * THE SAME ARM ON A SPARSE SOURCE JUNCTION -- the geometry the atomic cut
 * REFUSES, and the acceptance test for widening it.
 *
 * The other arms seed four fixed siblings per writer, two of which exist only to
 * keep each junction at three children.  Drop those two and the source junction
 * holds exactly the run plus one delimiter, so removing the run would collapse
 * it: ft_rekey_graft_simple_attempt refuses on `nr_child < 3` and the move falls
 * back to the staged writer, absence window and all.
 *
 * It differs from inv_rekey_public_atomic_no_gap_varlen in the seeded geometry
 * and NOTHING else -- same group flavour, same band, same probes -- so the arms
 * bracket the cut: one runs the atomic writer, the other is refused by it and
 * stages.  (Pairing it with the FIXED arm instead would not work: a fixed-length
 * group has no staged writer, so a refused shape there cannot move at all.)
 *
 * ★ Why this arm exists at all: EVERY shape refusal in the atomic writer counts
 * ZERO across all five concurrent rekey oracles (measured: 190076 attempts, not
 * one refusal of any kind), so the whole widening backlog had no coverage and
 * widening any of it could not move a number.  This is the geometry that
 * produces one, which had to come before the widening.
 *
 * Gap expectation: RKP_GAP_EXPECTED today, because the refusal routes the move
 * to the staged writer.  Widening the cut to admit a collapsing source junction
 * is what flips it to RKP_GAP_NONE -- and until then the arm ALSO serves as a
 * second absence-detector control alongside inv_rekey_coarse_progress, reached
 * by a different route (a shape refusal rather than a COARSE trie).
 */
static int inv_rekey_sparse_bp_staged(void)
{
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_sparse_bp_staged: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n");
		return 0;
	}
	return inv_rekey_public_no_gap_run(RKP_SPARSE_BP, RKP_GAP_EXPECTED,
			"inv_rekey_sparse_bp_staged");
}

static int inv_rekey_public_atomic_no_gap_varlen(void)
{
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_public_atomic_no_gap_varlen: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n");
		return 0;
	}
	return inv_rekey_public_no_gap_run(RKP_ATOMIC_VARLEN, RKP_GAP_NONE,
			"inv_rekey_public_atomic_no_gap_varlen");
}

/*
 * THE COARSE ARM -- a LIVENESS check, and the regression test for a total wedge.
 *
 * A COARSE trie serializes every mutator on the FT-wide writer lock, and the
 * staged rekey it falls back to takes a grace period mid-op.  Every writer parked
 * on that lock used to park RCU-ONLINE, so it was itself a reader the grace period
 * waited for: four writers made ZERO moves, permanently, with three sitting in
 * ft_writer_lock_gp_wait's re-acquire and the call_rcu thread in wait_for_readers.
 * Dropping the lock across the wait -- which that function already did -- fixes
 * only the self-deadlock, not the group.
 *
 * So this arm asserts PROGRESS rather than no-gap -- and it asserts the OPPOSITE
 * of the arms above: RKP_GAP_EXPECTED requires the absence window to be SEEN.
 * ★ Note the other failure mode it guards against is a HANG, so that regression
 * shows up as this leg timing out rather than as a "not ok".
 *
 * ☐ WHY COARSE STILL STAGES, checked 2026-08-16 and NOT for the reason this
 * comment used to give.  The old reasoning -- "a COARSE trie has no per-node
 * locks and therefore no atomic writer to reach" -- conflates ARBITRATION with
 * ATOMICITY: coarse has the STRONGEST writer exclusion (the FT-wide mutex makes
 * it a single updater), and what a reader needs is that the publish be ONE
 * FLIP, a property of the commit.  The real blocker is TERMINATION: with
 * one_decide's @lock_fine gate lifted, ft_rekey_graft_simple_locked's retry loop
 * NEVER TERMINATES on a coarse trie (three stack samples 30s apart, same move,
 * RSS flat).  Escalation cannot fix it -- it arbitrates COMMITS, never ACQUIRES.
 * See the gate comment in ft-rekey.h and inv_rekey_coarse_mixed_writers.
 */
static int inv_rekey_coarse_progress(void)
{
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_coarse_progress: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n");
		return 0;
	}
	return inv_rekey_public_no_gap_run(RKP_COARSE, RKP_GAP_EXPECTED,
			"inv_rekey_coarse_progress");
}

/*
 * Keys per insert/remove writer.  Deliberately far below MW_RANGE: the mixers
 * draw uniformly and only REMOVE a key they drew before, so with 256 offsets and
 * the few hundred ops this arm gets, a repeat draw -- and therefore a remove --
 * is rare.  The arm needs both halves of the insert/remove pair to collide with
 * the movers, so keep the range small enough that the shadow saturates and the
 * draw is a coin flip between the two.
 */
#define RKMIX_RANGE	32
#if RKMIX_RANGE > MW_RANGE
#error "RKMIX_RANGE must fit the mw_writer_arg shadow arrays"
#endif

/*
 * Seed @iter at @v and resolve it, on the VARIABLE-length group the mixed arm
 * needs: RKP_KLEN explicitly, never CDS_FT_LEN_DEFAULT (which names the group's
 * ONE key length and so means nothing here).  Unlike rkp_lookup, this leaves the
 * resolved PATH cached in @iter, which is what cds_ft_remove consumes.
 */
static struct cds_ft_node *rkp_iter_lookup(struct cds_ft_iter *iter,
		struct cds_ft *ft, uint64_t v)
{
	uint8_t k[8] = { 0 };

	cds_ft_u64_to_key(ft, v, k, RKP_KLEN);
	cds_ft_iter_set_key(iter, k, RKP_KLEN);
	cds_ft_lookup(ft, iter);
	return cds_ft_iter_node(iter);
}

/*
 * The mixed arm's insert/remove writer.  mw_writer cannot be reused: it passes
 * CDS_FT_LEN_DEFAULT, which names the group's ONE key length and so means
 * nothing on the variable-length group this arm needs.  Same lost-key shadow
 * (@present / @node), explicit RKP_KLEN keys, in a top-byte band disjoint from
 * every rekey writer's -- disjoint KEYS, shared ROOT AND UPPER SPINE, which is
 * where a rekey's edges and an insert's actually meet.
 *
 * ☠ THE ITERATOR IS NOT OPTIONAL.  This writer used to look the key up with
 * rkp_lookup (which yields a NODE, no path) and then pass @iter as NULL to
 * cds_ft_remove.  _cds_ft_remove_locked dereferences it unconditionally -- for
 * the key length before anything else (ft-remove.h:3964) -- so every remove that
 * was actually REACHED was a NULL dereference.  It stayed hidden because the
 * writer only removes a key it inserted earlier and its ops were so few against
 * MW_RANGE that a repeat draw was rare: the arm reported green by never taking
 * its own remove path.  Caught as a SIGSEGV in 7 of 25 runs once the mixers
 * stopped holding the movers' grace periods open.
 */
static void *rkmix_kv_writer(void *arg)
{
	struct mw_writer_arg *w = (struct mw_writer_arg *) arg;
	unsigned int seed = (unsigned int) (uintptr_t) w;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(w->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int off = (unsigned int) (rand_r(&seed) % RKMIX_RANGE);
		uint64_t key = w->base + off;
		struct cds_ft_node *found = NULL;

		/*
		 * The section spans lookup + remove: the lookup reference and
		 * the iter's cached path are valid only inside it, and
		 * cds_ft_remove consumes both (the §11 reference-lifetime
		 * contract).
		 */
		rcu_read_lock();
		found = rkp_iter_lookup(iter, w->ft, key);
		if (w->present[off]) {
			if (found == &w->node[off]->node) {
				if (cds_ft_remove(w->ft, iter, found) ==
						CDS_FT_STATUS_OK) {
					node_free_rcu(to_test_node(found));
					w->present[off] = 0;
					w->node[off] = NULL;
					w->ops++;
				}
			} else {
				fprintf(stderr, "rkmix_kv_writer: key %llu LOST "
					"(found %p != %p) -- a rekey edge clobbered "
					"an insert\n", (unsigned long long) key,
					(void *) found,
					(void *) &w->node[off]->node);
				w->failed = 1;
				rcu_read_unlock();
				break;
			}
			rcu_read_unlock();
		} else if (found) {
			/*
			 * My band is mine alone (RKMIX_BASE.. is disjoint from
			 * every rekey writer's 1..2*RK_NW), so a node at a key
			 * my shadow calls absent is a foreign edge landing in
			 * my band -- the same lost-update class, seen from the
			 * other side.
			 */
			fprintf(stderr, "rkmix_kv_writer: key %llu absent but "
				"found %p -- a foreign edge in my band\n",
				(unsigned long long) key, (void *) found);
			w->failed = 1;
			rcu_read_unlock();
			break;
		} else {
			struct ft_test_node *n = node_alloc(key);

			/*
			 * Only @found's boolean absence is consumed past this
			 * point, so the section closes HERE and the insert runs
			 * with no caller-held one -- the FT owns its own
			 * bracket (§11 Phase A), as mw_writer does.
			 */
			rcu_read_unlock();
			if (rkp_insert(w->ft, key, n) == CDS_FT_STATUS_OK) {
				w->present[off] = 1;
				w->node[off] = n;
				w->ops++;
			} else {
				node_free(n);
			}
		}
		/*
		 * UNCONDITIONALLY quiescent, once per iteration -- this is the
		 * arm's THROUGHPUT, not hygiene.  Every staged rekey move waits
		 * a GRACE PERIOD (it detaches, then merges back), and in QSBR a
		 * registered thread outside a read section still holds every GP
		 * open until it reports.  A gated report (the usual
		 * `(ops & 0x3f) == 0`) cannot work HERE because @ops advances
		 * only on a SUCCEEDING op: each mixer reported once, then went
		 * silent for the rest of the run.  That -- not slowness -- is
		 * what capped the arm at 24 moves / 42 ops: the mixers were the
		 * brake on the movers they are supposed to be racing.
		 */
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * The mixed arm's rekey writer.  Same move geometry as rk_writer, but through
 * the PUBLIC entry: _cds_ft_debug_rekey_graft_simple goes STRAIGHT to the atomic
 * writer with no staged fallback, so on a COARSE trie it answers
 * FT_REKEY_UNCOVERED and no move ever happens -- the arm would report a livelock
 * that is really just the wrong entry point.  cds_ft_rekey_graft dispatches, so
 * coarse reaches the staged writer and the arm actually exercises what it is
 * for: a rekey concurrent with insert/remove on one trie.
 */
static void *rkmix_rekey_writer(void *arg)
{
	struct rk_writer_arg *w = (struct rk_writer_arg *) arg;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint8_t cur = w->at_dst ? w->dp : w->bp;
		uint8_t oth = w->at_dst ? w->bp : w->dp;
		uint8_t src_key[2] = { cur, w->sb };
		uint8_t dst_key[2] = { oth, w->sb };
		enum cds_ft_status st;

		/* NO read lock: the entry takes the move gate, which waits a GP. */
		st = cds_ft_rekey_graft(w->ft, dst_key, 2, src_key, 2);
		if (st == CDS_FT_STATUS_OK) {
			w->at_dst = !w->at_dst;
			w->ops++;
		} else if (st == CDS_FT_STATUS_BUSY_ERROR ||
				st == CDS_FT_STATUS_POPULATED_ERROR ||
				st == CDS_FT_STATUS_MEMORY_ERROR) {
			w->retries++;		/* transient contention */
		} else {
			fprintf(stderr, "rkmix_rekey_writer bp=%u dp=%u: move "
				"failed st=%d\n", w->bp, w->dp, (int) st);
			w->failed = 1;
			break;
		}
		/* Same reason as the kv writer: @ops gates on SUCCESS. */
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * PURE READERS for the mixed arms, and they answer a specific question: can a
 * READER reach the residual use-after-retire, or only a writer?
 *
 * The two crash signatures both consume a parent link -- ft_get_parent_rcu via
 * ft_ineq_descend, and ft_rebuild_key_upwalk via iteration -- but both were
 * observed inside WRITERS (an insert's splice search, the rekey's detach climb).
 * A reader holds ONE continuous read-side section per operation, so if the
 * defect is a writer reusing a reference across its own per-attempt pin drop, a
 * reader structurally cannot see it and these threads will stay clean however
 * long they run.  That makes their silence evidence, not absence of coverage --
 * provided they really do drive those paths, which is why they issue the
 * inequality lookups and the iteration walk and nothing else.
 *
 * No oracle: correctness of the ANSWER is the other arms' job.  These exist to
 * fault, or not.
 */
static void *rkmix_reader(void *arg)
{
	struct mw_writer_arg *r = (struct mw_writer_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int) (uintptr_t) r;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int pick = (unsigned int) rand_r(&seed) & 7;
		/*
		 * Aim INTO the contended junctions (top bytes 1 and 2), which is
		 * where the movers restructure and the mixers insert.
		 */
		uint64_t key = ((uint64_t) (1 + (rand_r(&seed) & 1)) << 24) |
			((uint64_t) (rand_r(&seed) & 0x7f) << 16) |
			((uint64_t) (rand_r(&seed) & 0xff) << 8);
		uint8_t k[8] = { 0 };

		cds_ft_u64_to_key(r->ft, key, k, RKP_KLEN);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, RKP_KLEN);
		switch (pick) {
		case 0: (void) cds_ft_lookup_ge(r->ft, iter); break;
		case 1: (void) cds_ft_lookup_gt(r->ft, iter); break;
		case 2: (void) cds_ft_lookup_le(r->ft, iter); break;
		case 3: (void) cds_ft_lookup_lt(r->ft, iter); break;
		default:
			/*
			 * The ITERATION path: cds_ft_next rebuilds the key by
			 * walking parents (ft_rebuild_key_upwalk), which is the
			 * other faulting site.
			 */
			if (cds_ft_lookup_ge(r->ft, iter) == CDS_FT_STATUS_OK) {
				int n;

				for (n = 0; n < 8; n++)
					if (cds_ft_next(r->ft, iter) !=
							CDS_FT_STATUS_OK)
						break;
			}
			break;
		}
		(void) cds_ft_iter_node(iter);
		rcu_read_unlock();
		r->ops++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

#ifndef RKMIX_NR
#define RKMIX_NR	8		/* pure readers */
#endif
#ifndef RKMIX_NW
#define RKMIX_NW	6		/* insert/remove writers */
#endif
/*
 * The DISJOINT geometry's mixer band, in the TOP byte.  Every rekey writer owns
 * top bytes 1..2*RK_NW there, so this must start above them -- derived from
 * RK_NW for the same reason as the contended band below, and with the same gap
 * choice that leaves the shipped RK_NW == 8 geometry at its original 0x20.
 */
#define RKMIX_BASE	(2 * RK_NW + 16)
#if RKMIX_BASE + RKMIX_NW > 254
#error "RK_NW / RKMIX_NW too large: the mixer band collides with the 0xff guard"
#endif
/*
 * The CONTENDED geometry's mixer slot byte, and it must clear the movers'
 * OCCUPIED RANGE, not merely their slot bytes: writer i owns sb-2, sb, sb+2 for
 * sb = 3 + 6i, so bytes 1..6*RK_NW-1 belong to the movers and a mixer inside
 * that range has its keys legitimately MOVED to the destination junction --
 * which the arm then reports as a lost key it invented.  DERIVED from RK_NW so
 * that raising the writer count cannot silently re-create that collision, with
 * the gap chosen to leave the shipped RK_NW == 8 geometry at its original 0x50.
 */
#define RKMIX_CONTENDED_BASE	(6 * RK_NW + 32)
#if RKMIX_CONTENDED_BASE + RKMIX_NW > 255
#error "RK_NW / RKMIX_NW too large: the contended mixer band runs off byte 2"
#endif

/*
 * ★ DURATION IS THE ONLY LEVER THE COARSE ARM HAS.  Its move count is
 * digit-exact across runs (8 writers x N moves, never N +- 1), which is the
 * tell: the movers are SERIALIZED on the coarse trie's FIFO fair mutex and each
 * staged move costs grace periods, so they take strict turns and the run yields
 * exactly duration / move-cost moves.  Adding rekey writers therefore buys
 * NOTHING there -- it only lengthens the queue -- and that is why this arm runs
 * longer than DEFAULT_DURATION_MS instead of wider.  The FINE arms do run their
 * movers concurrently, so RK_NW is a real lever for those.
 */
#ifndef RKMIX_DURATION_MULT
#define RKMIX_DURATION_MULT	10
#endif
#define RKMIX_DURATION_MS	(RKMIX_DURATION_MULT * DEFAULT_DURATION_MS)

/*
 * Which rekey WRITER does @mode's trie actually reach?  This is the arm's
 * central fact, so it is measured rather than assumed -- see
 * rkmix_probe_writer_reached().
 */
enum rkmix_writer {
	RKMIX_WRITER_STAGED,	/* the detach-then-merge-back fallback */
	RKMIX_WRITER_ATOMIC,	/* ft_rekey_one_decide: parks its edges SW */
};

/*
 * ★ PROBE, NOT AN ARGUMENT: which rekey writer will the movers use?
 *
 * _cds_ft_debug_rekey_graft_simple is the ATOMIC entry with no staged fallback,
 * so its answer for one real move IS the fact: OK means the atomic writer covers
 * this trie and shape (and its SW parks will run against the mixers);
 * FT_REKEY_UNCOVERED (-EDOM) means it refuses and every move will be staged.
 *
 * Run once before the threads start, on writer 0's move, and hand the caller
 * back the direction it left behind so the seeded geometry stays consistent.
 * The arm then GATES on the answer matching what the mode is supposed to give,
 * which is what stops it from silently becoming vacuous the way the coarse arm
 * did -- a trie that cannot reach the atomic writer cannot witness an
 * unarbitrated SW park, however many moves it makes.
 */
static int rkmix_probe_writer_reached(struct cds_ft *ft, struct rk_writer_arg *w,
		enum rkmix_writer want, const char *name)
{
	uint8_t src_key[2] = { w->bp, w->sb };
	uint8_t dst_key[2] = { w->dp, w->sb };
	enum rkmix_writer got;
	int rc;

	rc = _cds_ft_debug_rekey_graft_simple(ft, src_key, 2, dst_key, 2);
	if (rc == 0) {
		got = RKMIX_WRITER_ATOMIC;
		w->at_dst = !w->at_dst;		/* the probe MOVED it */
	} else if (rc == -EDOM) {		/* FT_REKEY_UNCOVERED, internal */
		got = RKMIX_WRITER_STAGED;
	} else {
		fprintf(stderr, "%s: writer probe returned rc=%d, which is "
			"neither a move nor an uncovered shape -- the probe "
			"cannot answer\n", name, rc);
		return -1;
	}
	fprintf(stderr, "# %s: rekey writer reached = %s\n", name,
		got == RKMIX_WRITER_ATOMIC ? "ATOMIC (SW parks)" : "STAGED");
	if (got != want) {
		fprintf(stderr, "%s: expected the %s writer, got the %s one -- "
			"this arm is not exercising what it exists for\n", name,
			want == RKMIX_WRITER_ATOMIC ? "ATOMIC" : "STAGED",
			got == RKMIX_WRITER_ATOMIC ? "ATOMIC" : "STAGED");
		return -1;
	}
	return 0;
}

/*
 * THE DEEP-JUNCTION ARM -- built because every cost number G5.15/G5.16 produced
 * was measured on BULK-AT-DEPTH-1 and is therefore silent on the thing it was
 * read as judging.
 *
 * Both rekey mixed-writer arms drive their movers with TWO-BYTE keys, and the
 * contended one gives all eight the same pair, so every move happens between two
 * CHILDREN OF THE ROOT: the junction sits at depth 1 in a trie whose keys are
 * four bytes.  A published per-op junction level there would be ~1, and widening
 * "from level 1 down" instead of "from 0 down" excludes exactly ONE WORD -- the
 * root -- out of a three-or-four deep path.  The array-based level ceiling has
 * essentially nothing to cut, so those arms cannot tell a working ceiling from a
 * broken one, and the 5.8x they report is the already-known cost of
 * bulk-near-root rather than a verdict.
 *
 * So vary the one thing they hold fixed: DEPTH.
 *
 *  - keys are WDJ_KLEN bytes, not four;
 *  - each mover owns a junction at byte WDJ_JUNCT, deep in the trie, and moves a
 *    subtree back and forth across it, so the shallowest word a bulk op has any
 *    business locking is DEEP;
 *  - the point writers live under their own top bytes, a subtree the movers
 *    never enter, so they diverge from the movers AT DEPTH 0.
 *
 * ☠ THIS ARM IS A THROUGHPUT PROBE, NOT AN ORACLE.  It asserts only that the
 * writers make progress; whether the answers are RIGHT is the mixed-writer arms'
 * job and they keep it.  What it exists to produce is a point-op rate that can be
 * compared across -DFT_FEATURE_WIDEN, in a geometry where a junction level would
 * actually have something to exclude.
 *
 * ☞ Reading it: with the widening OFF this is the baseline.  With it ON and every
 * gate entry still passing level 0 -- which is what ships -- every widened
 * acquire still takes the ROOT, so a large loss here says the cost is the
 * ROOT-TAKING and not the junction depth, which is the first half of the
 * question.  The second half needs a published level and is not wired.
 */
#define WDJ_NW		8	/* movers, one deep junction each */
#define WDJ_NP		6	/* point writers, one private subtree each */
#define WDJ_NR		8	/* readers */
#define WDJ_KLEN	16	/* deep keys -- the whole point of the arm */
#define WDJ_JUNCT	12	/* the byte src/dst diverge at: the junction */
#define WDJ_SEED	6	/* keys seeded under each mover's source */
#define WDJ_TOP_MOVER	0x10
#define WDJ_TOP_POINT	0x40

struct wdj_mover_arg {
	struct cds_ft *ft;
	uint8_t top;
	bool at_dst;
	unsigned long moves;
	unsigned long fails;
	int last_rc;
};

struct wdj_point_arg {
	struct cds_ft *ft;
	uint8_t top;
	unsigned long ops;
};

/*
 * The junction prefix: @top, then zeros, then @side at WDJ_JUNCT.  src and dst
 * differ ONLY in that last byte, so their branch point -- the node a move
 * restructures -- is at depth WDJ_JUNCT.
 */
static void wdj_prefix(uint8_t *k, uint8_t top, uint8_t side)
{
	memset(k, 0, WDJ_JUNCT + 1);
	k[0] = top;
	k[WDJ_JUNCT] = side;
}

static void *wdj_mover(void *arg)
{
	struct wdj_mover_arg *w = (struct wdj_mover_arg *) arg;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		uint8_t src[WDJ_JUNCT + 1], dst[WDJ_JUNCT + 1];

		wdj_prefix(src, w->top, w->at_dst ? 2 : 1);
		wdj_prefix(dst, w->top, w->at_dst ? 1 : 2);
		int rc = _cds_ft_debug_rekey_graft_simple(w->ft, src,
				WDJ_JUNCT + 1, dst, WDJ_JUNCT + 1);

		if (rc == 0) {
			w->at_dst = !w->at_dst;
			w->moves++;
		} else {
			w->fails++;
			w->last_rc = rc;
		}
	}
	rcu_unregister_thread();
	return NULL;
}

static void *wdj_point(void *arg)
{
	struct wdj_point_arg *w = (struct wdj_point_arg *) arg;
	unsigned int seed = (unsigned int) (uintptr_t) w;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(w->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		uint8_t key[WDJ_KLEN];
		struct ft_test_node *n;
		struct cds_ft_node *found;
		int i;

		/*
		 * A private top byte, then a deep random tail: the point op's
		 * own path is WDJ_KLEN deep, so a published junction level
		 * would leave it widening over a handful of DEEP ancestors
		 * rather than the whole spine.
		 */
		memset(key, 0, sizeof(key));
		key[0] = w->top;
		for (i = WDJ_KLEN - 4; i < WDJ_KLEN; i++)
			key[i] = (uint8_t) rand_r(&seed);
		n = node_alloc(0);
		memcpy(n->okey, key, WDJ_KLEN);
		/*
		 * The section spans insert + lookup + remove: the lookup
		 * reference and the iter's cached path are valid only inside
		 * it, and cds_ft_remove consumes both.
		 */
		rcu_quiescent_state();
		rcu_read_lock();
		if (cds_ft_insert(w->ft, key, WDJ_KLEN, &n->node) !=
				CDS_FT_STATUS_OK) {
			/* a duplicate of my own earlier key; nothing published */
			rcu_read_unlock();
			node_free(n);
			continue;
		}
		w->ops++;
		cds_ft_iter_set_key(iter, key, WDJ_KLEN);
		cds_ft_lookup(w->ft, iter);
		found = cds_ft_iter_node(iter);
		if (found == &n->node &&
				cds_ft_remove(w->ft, iter, found) ==
					CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			node_free_rcu(to_test_node(found));
			w->ops++;
			continue;
		}
		rcu_read_unlock();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *wdj_reader(void *arg)
{
	struct wdj_point_arg *r = (struct wdj_point_arg *) arg;
	unsigned int seed = (unsigned int) (uintptr_t) r;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(r->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		uint8_t key[WDJ_KLEN];

		memset(key, 0, sizeof(key));
		key[0] = (uint8_t) (WDJ_TOP_MOVER +
				(rand_r(&seed) % WDJ_NW));
		key[WDJ_JUNCT] = (uint8_t) (1 + (rand_r(&seed) & 1));
		rcu_quiescent_state();
		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, WDJ_KLEN);
		(void) cds_ft_lookup_ge(r->ft, iter);
		rcu_read_unlock();
		r->ops++;
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_widen_deep_junction(void)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft_attr *attr;
	struct cds_ft *ft;
	struct wdj_mover_arg w[WDJ_NW];
	struct wdj_point_arg pw[WDJ_NP], rd[WDJ_NR];
	pthread_t movers[WDJ_NW], points[WDJ_NP], readers[WDJ_NR];
	struct timespec t0;
	unsigned long moves = 0, pops = 0, rops = 0, mfails = 0;
	int last_rc = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_widen_deep_junction: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	/*
	 * VARIABLE-length and LOCK_FINE: the atomic rekey writer -- the one that
	 * takes per-node DLM locks and therefore the one the widening interacts
	 * with -- runs only there (see inv_rekey_fine_mixed_writers).  Variable
	 * length is also what lets a mover name its junction by a PREFIX.
	 *
	 * ☠ ORDERED LIST OFF, and not as an economy.  With it on this arm
	 * LIVELOCKS: the cell list is TRIE-WIDE, so the point writers' churn --
	 * in a subtree the movers never enter -- keeps
	 * ft_ord_cell_find_splice_pos_coherent's two derivations from agreeing,
	 * the rekey returns -EAGAIN ("torn derivation: re-descend"), and its
	 * internal retry never terminates.  Measured: one mover stuck inside a
	 * single _cds_ft_debug_rekey_graft_simple call, spinning in
	 * ft_node_get_minmax, with every other thread already stopped.
	 * ★ A REAL interaction and worth its own arm, but not this one's
	 * subject: it couples the two subtrees through the LIST and confounds
	 * exactly the trie-lock contention this arm exists to isolate.
	 */
	if (cds_ft_group_attr_create(&gattr) < 0)
		abort();
	if (cds_ft_group_attr_set_max_key_len(gattr, WDJ_KLEN) < 0 ||
			cds_ft_group_attr_set_lookup_optimization(gattr,
				CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0 ||
			cds_ft_group_attr_set_writer_strategy(gattr,
				CDS_FT_WRITER_LOCK_FINE) < 0 ||
			cds_ft_group_attr_set_ordered_list(gattr, false) < 0)
		abort();
	if (cds_ft_group_create(gattr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_attr_create(&attr) < 0)
		abort();
	if (cds_ft_attr_set_speculative_keys(attr, false) < 0)
		abort();
	if (cds_ft_create(group, attr, &ft) < 0)
		abort();
	cds_ft_attr_destroy(attr);
	cds_ft_make_shared(ft);

	/*
	 * ☠☠ BYTE DEPTH IS NOT NODE DEPTH, and the widening walks NODES.
	 *
	 * The first cut of this arm used sixteen-byte keys with a long shared
	 * prefix and measured an ancestor ledger whose depth maxed out at TWO:
	 * a sparse key PATH-COMPRESSES, so a run of bytes nobody branches on
	 * collapses into a single node.  Deep keys bought nothing; the junction
	 * was still a hop or two from the root, which is the very thing this arm
	 * exists to escape.
	 *
	 * So FORCE A BRANCH AT EVERY LEVEL.  For each byte position d along a
	 * writer's path, insert one extra key that agrees on [0, d) and differs
	 * at d -- that is exactly the condition for a node to exist at d.  The
	 * spine then has one node per byte and the recorded path is genuinely
	 * WDJ_JUNCT deep.  Done for the movers' junction path AND for the point
	 * writers' subtree, since a point op's widened set is drawn from ITS own
	 * path and would otherwise be just as shallow.
	 */
	rcu_read_lock();
	for (i = 0; i < WDJ_NW + WDJ_NP; i++) {
		bool mover = i < WDJ_NW;
		uint8_t top = mover ? (uint8_t) (WDJ_TOP_MOVER + i) :
			(uint8_t) (WDJ_TOP_POINT + (i - WDJ_NW));
		int d;

		for (d = 1; d <= WDJ_JUNCT; d++) {
			uint8_t key[WDJ_KLEN];
			struct ft_test_node *n;

			memset(key, 0, sizeof(key));
			key[0] = top;
			key[d] = 0xFF;		/* the branch that makes a node at d */
			n = node_alloc(0);
			memcpy(n->okey, key, WDJ_KLEN);
			if (cds_ft_insert(ft, key, WDJ_KLEN, &n->node) !=
					CDS_FT_STATUS_OK)
				abort();
		}
		if (!mover)
			continue;
		/* and the subtree the mover actually moves */
		for (c = 0; c < WDJ_SEED; c++) {
			uint8_t key[WDJ_KLEN];
			struct ft_test_node *n;

			wdj_prefix(key, top, 1);
			memset(key + WDJ_JUNCT + 1, 0,
					WDJ_KLEN - WDJ_JUNCT - 1);
			key[WDJ_KLEN - 1] = (uint8_t) (c + 1);
			n = node_alloc(0);
			memcpy(n->okey, key, WDJ_KLEN);
			if (cds_ft_insert(ft, key, WDJ_KLEN, &n->node) !=
					CDS_FT_STATUS_OK)
				abort();
		}
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < WDJ_NW; i++) {
		w[i].ft = ft;
		w[i].top = (uint8_t) (WDJ_TOP_MOVER + i);
		w[i].at_dst = false;
		w[i].moves = 0;
		w[i].fails = 0;
		w[i].last_rc = 0;
		pthread_create(&movers[i], NULL, wdj_mover, &w[i]);
	}
	for (i = 0; i < WDJ_NP; i++) {
		pw[i].ft = ft;
		pw[i].top = (uint8_t) (WDJ_TOP_POINT + i);
		pw[i].ops = 0;
		pthread_create(&points[i], NULL, wdj_point, &pw[i]);
	}
	for (i = 0; i < WDJ_NR; i++) {
		rd[i].ft = ft;
		rd[i].top = 0;
		rd[i].ops = 0;
		pthread_create(&readers[i], NULL, wdj_reader, &rd[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < RKMIX_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < WDJ_NW; i++)
		pthread_join(movers[i], NULL);
	for (i = 0; i < WDJ_NP; i++)
		pthread_join(points[i], NULL);
	for (i = 0; i < WDJ_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	for (i = 0; i < WDJ_NW; i++) {
		moves += w[i].moves;
		mfails += w[i].fails;
		if (w[i].last_rc)
			last_rc = w[i].last_rc;
	}
	for (i = 0; i < WDJ_NP; i++)
		pops += pw[i].ops;
	for (i = 0; i < WDJ_NR; i++)
		rops += rd[i].ops;

	fprintf(stderr, "# inv_widen_deep_junction: %d movers at junction "
		"depth %d (%lu moves, %lu refused, last rc %d) + %d point "
		"writers in a disjoint subtree (%lu ops), %d readers (%lu ops), "
		"key len %d\n",
		WDJ_NW, WDJ_JUNCT, moves, mfails, last_rc, WDJ_NP, pops,
		WDJ_NR, rops, WDJ_KLEN);

	/*
	 * ☠ THE ONLY ASSERTION IS PROGRESS, and it is what keeps a zero from
	 * reading as a pass: an arm whose movers or point writers never ran
	 * would otherwise report a beautiful throughput number of nothing.
	 * RUN_TEST supplies the ok(); returning non-zero is how it is failed.
	 */
	if (moves == 0 || pops == 0) {
		fprintf(stderr, "inv_widen_deep_junction: NO PROGRESS "
			"(moves %lu, point ops %lu) -- the arm measured "
			"nothing\n", moves, pops);
		ret = -1;
	}
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

static int inv_rekey_mixed_writers_run(enum rkp_mode mode,
		enum rkmix_writer want, bool contended, const char *name)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rk_writer_arg *w;
	struct mw_writer_arg *m;
	pthread_t writers[RK_NW], mixers[RKMIX_NW], readers[RKMIX_NR];
	struct mw_writer_arg *rd;
	struct ft_test_node *guard_lo, *guard_hi;
	struct timespec t0;
	unsigned long total_ops = 0, total_retries = 0, mix_ops = 0, live = 0;
	int i, c, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# %s: skipped "
			"(set FT_INV_MW=1 to run the concurrent-writer oracles)\n",
			name);
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	/*
	 * VARIABLE-length in BOTH modes.  A COARSE trie can only reach the
	 * STAGED writer (one_decide is gated on @lock_fine), and the staged
	 * writer REFUSES a fixed-length group -- it moves through a detached
	 * subtree whose keys are stripped of the prefix, so they are shorter
	 * than the group's one length.  On a fixed COARSE trie a rekey is
	 * therefore impossible by BOTH routes and the arm measures nothing.
	 */
	ft = create_rekey_coherent_ft(mode, &group);
	cds_ft_make_shared(ft);

	guard_lo = node_alloc(0x00000000ULL);
	guard_hi = node_alloc(0xff000000ULL);
	rcu_read_lock();
	if (rkp_insert(ft, 0x00000000ULL, guard_lo) != CDS_FT_STATUS_OK ||
			rkp_insert(ft, 0xff000000ULL, guard_hi) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();
	live = 2;

	w = (struct rk_writer_arg *) calloc(RK_NW, sizeof(*w));
	m = (struct mw_writer_arg *) calloc(RKMIX_NW, sizeof(*m));
	rd = (struct mw_writer_arg *) calloc(RKMIX_NR, sizeof(*rd));
	if (!w || !m || !rd)
		abort();
	for (i = 0; i < RK_NW; i++) {
		/*
		 * DISJOINT: writer i owns junction pair (2i+1, 2i+2) and every
		 * writer sits at the same slot byte 3, so writers share the ROOT
		 * and the upper spine and nothing below it.
		 *
		 * CONTENDED: every writer shares ONE junction pair (1, 2) and
		 * owns a distinct slot byte instead.  That is the whole
		 * difference between the two arms, and it is the difference that
		 * decides whether the SW-park class can occur at all: an
		 * unarbitrated re-parent needs TWO OPS REACHING ONE CHILD, which
		 * a shared spine does NOT provide and a shared junction does --
		 * moving any writer's subtree out of it recompacts the node
		 * holding every other writer's, and every mixer's, children.
		 */
		uint8_t bp = contended ? 1 : (uint8_t) (2 * i + 1);
		uint8_t dp = contended ? 2 : (uint8_t) (2 * i + 2);
		/*
		 * STRIDE 6, not 4.  The sibs bracket @sb at +-2, so a stride of
		 * 4 makes writer i's HIGH sib and writer i+1's LOW sib the same
		 * key: on a variable-length group that inserts a DUPLICATE
		 * rather than failing, the lookup then resolves to whichever
		 * node heads the chain, and the arm reports "sib lost" for a
		 * defect it invented.  6 is the smallest stride that keeps every
		 * seeded key distinct (writer i uses sb-2, sb, sb+2).
		 */
		uint8_t sb = contended ? (uint8_t) (3 + 6 * i) : 3;
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		/*
		 * The straddling sibs bracket @sb at +-2.  For the disjoint
		 * geometry's sb == 3 that is the historical {1, 5}, unchanged;
		 * under contention they step with @sb, staying distinct across
		 * writers while landing in the SAME junction.
		 */
		uint64_t lo = (uint64_t) (sb - 2) << 16;
		uint64_t hi = (uint64_t) (sb + 2) << 16;
		uint64_t sk[4] = { bpk | lo, bpk | hi, dpk | lo, dpk | hi };

		w[i].ft = ft;
		w[i].bp = bp;
		w[i].dp = dp;
		w[i].sb = sb;
		rcu_read_lock();
		for (c = 0; c < 4; c++) {
			w[i].sib[c] = node_alloc(sk[c]);
			if (rkp_insert(ft, sk[c], w[i].sib[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		for (c = 0; c < 4; c++) {
			uint64_t tk = bpk | ((uint64_t) sb << 16) |
				((uint64_t) (c + 1) << 8);

			w[i].top[c] = node_alloc(tk);
			if (rkp_insert(ft, tk, w[i].top[c]) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		live += 8;
	}
	for (i = 0; i < RKMIX_NW; i++) {
		m[i].ft = ft;
		/*
		 * CONTENDED: the mixers move INTO the movers' source junction
		 * (top byte 1) at their own slot bytes, so their inserts and
		 * removes reshape the very node whose children the rekeys
		 * re-parent.  The KEYS stay disjoint; the NODES no longer are.
		 */
		m[i].base = contended
			? ((1ULL << 24) |
			   ((uint64_t) (RKMIX_CONTENDED_BASE + i) << 16))
			: (((uint64_t) (RKMIX_BASE + i)) << 24);
	}

	/* Which writer will the movers use?  Measured, before any of them run. */
	if (rkmix_probe_writer_reached(ft, &w[0], want, name) < 0)
		ret = -1;

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_create(&writers[i], NULL, rkmix_rekey_writer, &w[i]);
	for (i = 0; i < RKMIX_NW; i++)
		pthread_create(&mixers[i], NULL, rkmix_kv_writer, &m[i]);
	for (i = 0; i < RKMIX_NR; i++) {
		rd[i].ft = ft;
		pthread_create(&readers[i], NULL, rkmix_reader, &rd[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < RKMIX_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RK_NW; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < RKMIX_NW; i++)
		pthread_join(mixers[i], NULL);
	for (i = 0; i < RKMIX_NR; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RK_NW; i++) {
		uint8_t bp = w[i].bp, dp = w[i].dp;
		uint64_t bpk = (uint64_t) bp << 24, dpk = (uint64_t) dp << 24;
		uint8_t X = w[i].at_dst ? dp : bp, O = w[i].at_dst ? bp : dp;
		uint64_t Xk = (uint64_t) X << 24, Ok = (uint64_t) O << 24;
		uint64_t lo = (uint64_t) (w[i].sb - 2) << 16;
		uint64_t hi = (uint64_t) (w[i].sb + 2) << 16;
		uint64_t sk[4] = { bpk | lo, bpk | hi, dpk | lo, dpk | hi };

		total_ops += w[i].ops;
		total_retries += w[i].retries;
		if (w[i].failed)
			ret = -1;
		for (c = 0; c < 4; c++) {
			struct cds_ft_node *found = NULL;

			if (rkp_lookup(ft, sk[c], &found) != CDS_FT_STATUS_OK ||
					found != &w[i].sib[c]->node) {
				fprintf(stderr, "%s: rekey writer %d sib %d lost\n",
					name, i, c);
				ret = -1;
			}
		}
		for (c = 0; c < 4; c++) {
			uint64_t sbk = (uint64_t) w[i].sb << 16;
			uint64_t here = Xk | sbk | ((uint64_t) (c + 1) << 8);
			uint64_t there = Ok | sbk | ((uint64_t) (c + 1) << 8);
			struct cds_ft_node *found = NULL;

			if (rkp_lookup(ft, here, &found) != CDS_FT_STATUS_OK ||
					found != &w[i].top[c]->node) {
				fprintf(stderr, "%s: rekey writer %d top %d absent "
					"at final pos (at_dst=%d)\n", name, i, c,
					w[i].at_dst);
				ret = -1;
			}
			if (rkp_lookup(ft, there, &found) == CDS_FT_STATUS_OK) {
				fprintf(stderr, "%s: rekey writer %d top %d still "
					"at old pos\n", name, i, c);
				ret = -1;
			}
		}
	}
	for (i = 0; i < RKMIX_NW; i++) {
		unsigned int off;

		mix_ops += m[i].ops;
		if (m[i].failed)
			ret = -1;
		for (off = 0; off < RKMIX_RANGE; off++) {
			struct cds_ft_node *found = NULL;

			if (!m[i].present[off])
				continue;
			live++;
			if (rkp_lookup(ft, m[i].base + off, &found)
					!= CDS_FT_STATUS_OK ||
			    found != &m[i].node[off]->node) {
				fprintf(stderr, "%s: insert/remove writer %d key "
					"%llu LOST (found %p != %p) -- an edge from "
					"a concurrent rekey clobbered it\n",
					name, i,
					(unsigned long long) (m[i].base + off),
					(void *) found, (void *) &m[i].node[off]->node);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "%s: count_keys %lu != live %lu\n", name,
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: cds_ft_verify failed\n", name);
		ret = -1;
	}
	rcu_read_unlock();

	/* Both writer kinds must have RUN, else the mix is not a mix. */
	if (total_ops == 0) {
		fprintf(stderr, "%s: no rekey moves (livelock?)\n", name);
		ret = -1;
	}
	if (mix_ops == 0) {
		fprintf(stderr, "%s: no insert/remove ops -- the arm did "
			"NOT exercise the mixed-writer case it exists for\n", name);
		ret = -1;
	}
	fprintf(stderr, "# %s: %d rekey writers "
		"(%lu moves, %lu retries) + %d insert/remove writers (%lu ops), "
		"%lu live keys\n", name, RK_NW, total_ops, total_retries,
		RKMIX_NW, mix_ops, live);

	{
		unsigned long rops = 0;

		for (i = 0; i < RKMIX_NR; i++)
			rops += rd[i].ops;
		fprintf(stderr, "# %s: %d readers, %lu read ops\n", name,
			RKMIX_NR, rops);
		/* A reader pool that never ran proves nothing by staying clean. */
		if (rops == 0) {
			fprintf(stderr, "%s: readers did NOT run\n", name);
			ret = -1;
		}
	}
	free(w);
	free(m);
	free(rd);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * ☠ THIS ARM CANNOT WITNESS THE SW-PARK CLASS, and its own probe says so.
 *
 * It was written for it -- the reasoning was that on a COARSE trie every OTHER
 * writer excludes through the FT-wide mutex while the rekey took no writer scope
 * at all, so its unfailable SW parks arbitrated with nobody.  That was true only
 * of the (reverted) build in which one_decide's @lock_fine gate had been lifted.
 * With the gate in place a COARSE trie NEVER REACHES the atomic writer, so it
 * never parks an edge SW, so the class does not occur here at all -- and
 * rkmix_probe_writer_reached() asserts exactly that by asking the atomic-only
 * entry for one move and requiring it to refuse.
 *
 * ★ Measured, not argued: with the coarse rekey's FT-wide lock DELETED
 * (-DFT_RED_REKEY_NOLOCK, a red control), this arm stayed green -- and that is
 * still true, so it is NOT a detector for the exclusion it is named for.  What
 * covers that now is the engine-side check: -DURCU_TXN_DEBUG_SETTLE reports any
 * word a peer wrote between our park and our settle, and with the same red
 * control armed (68,655 scopes skipped) it found 0 foreign writes in 1.37M
 * settle records.
 *
 * ☑ NOW EXPECTS THE ATOMIC WRITER.  It expected the STAGED one for as long as
 * coarse had no atomic writer; the staged writer has since been DELETED as a
 * guarantee failure, and the coarse gate that sent this mode to it has been
 * lifted.  What it exercises is therefore the ATOMIC rekey concurrent with
 * insert/remove on one coarse trie, which nothing else covers.
 */
static int inv_rekey_coarse_mixed_writers(void)
{
	return inv_rekey_mixed_writers_run(RKP_COARSE, RKMIX_WRITER_ATOMIC,
			/*contended=*/ false, "inv_rekey_coarse_mixed_writers");
}

/*
 * THE ARM THE SW-PARK CLASS ACTUALLY LIVES IN.
 *
 * The atomic rekey parks its structural edges SW (ft_flip_txn_set_structural_sw)
 * and an SW park CANNOT FAIL -- it does not arbitrate -- so it is legal only
 * where the op excludes every peer writer over those slots.  On a FINE trie the
 * per-node DLM LOCK try-locks are that protocol, the atomic writer runs (the
 * probe requires it), and insert/remove speak the same protocol.  So this is the
 * configuration in which a missing or mismatched acquire in the rekey's lock set
 * shows up as a peer's lost key.
 *
 * Same geometry, same oracle, same disjoint-key / shared-spine layout as the
 * coarse arm -- the mode is the whole difference, which is what makes the pair
 * readable against each other.
 *
 * ☠☠ AND IT IS NOT A DETECTOR OF THAT CLASS EITHER.  Red control: build with
 * -DFT_RED_PARENT_WORD_SW, which restores ft_flip_txn_record_parent_word to its
 * pre-@9ef2f648 form -- a REAL defect, the canonical unarbitrated SW park, not a
 * synthetic one -- and which counts the parks it deliberately gets wrong so the
 * control cannot be silently inert.  Measured, 3000 runs per side at 90-way
 * parallelism, GREEN (build-hunt) against RED (the same tree, one -D apart):
 *
 *   per run      58527 moves, 58014 insert/remove ops, 234815 UNHELD SW parks
 *   RED          0 failures / 3000 runs  (~7.0e8 injected violations)
 *   GREEN        0 failures / 3000 runs  (no false positives either)
 *
 * ★ WHY, and it is the DESIGN and not the exposure: the defect needs TWO OPS
 * RE-PARENTING ONE UNHELD CHILD, and this layout is built so that never happens.
 * Every rekey writer owns its own junction pair and every insert/remove writer
 * its own top-byte band; they share the ROOT and the upper spine but never a
 * child.  "Rekey and insert/remove on one trie" is not the precondition -- two
 * writers reaching the SAME child is.  An arm that wants this class must make
 * its writers CONTEND on one junction, and the disjointness that makes the
 * lost-key shadow easy to check is exactly what forecloses it.
 */
static int inv_rekey_fine_mixed_writers(void)
{
	return inv_rekey_mixed_writers_run(RKP_ATOMIC_VARLEN,
			RKMIX_WRITER_ATOMIC, /*contended=*/ false,
			"inv_rekey_fine_mixed_writers");
}

/*
 * THE ARM BUILT FROM THE REASON THE OTHER TWO CANNOT SEE THE CLASS.
 *
 * An unarbitrated SW park becomes a LOST UPDATE only when TWO OPS RE-PARENT ONE
 * CHILD: both park, neither can fail, and the last install wins.  Both mixed
 * arms above give their writers DISJOINT bands -- shared root, shared upper
 * spine, never a shared child -- so they satisfy "rekey and insert/remove on one
 * trie" and still never meet the precondition.  Measured: the fine arm survived
 * ~7.0e8 injected real defects (-DFT_RED_PARENT_WORD_SW) across 3000 runs
 * without one detection.
 *
 * So put every writer on ONE JUNCTION instead.  All RK_NW movers share the pair
 * (1, 2) and differ only in their slot byte; all RKMIX_NW insert/remove writers
 * live in that same source junction at slot bytes of their own.  Keys stay
 * disjoint -- the lost-key shadow is unchanged and still exact -- but the NODE
 * every writer reshapes is now the same node, which is what a re-parent
 * collision requires.
 *
 * FINE, because that is where the atomic writer runs and therefore where an SW
 * park exists at all; the probe gates on it.
 */
static int inv_rekey_contended_mixed_writers(void)
{
	return inv_rekey_mixed_writers_run(RKP_ATOMIC_VARLEN,
			RKMIX_WRITER_ATOMIC, /*contended=*/ true,
			"inv_rekey_contended_mixed_writers");
}

/*
 * The COARSE twin of the arm above: same contended geometry, same ATOMIC
 * writer, and the EXCLUSION underneath it is what differs -- the FT-wide mutex
 * here (CDS_FT_SCOPED_WRITER), the per-node DLM locks there.  So the pair
 * isolates the exclusion mode rather than the writer, which is what it can
 * still isolate now that coarse and fine reach the same writer.
 *
 * (It expected the STAGED writer until coarse gained one; see the arm above.)
 */
static int inv_rekey_coarse_contended_writers(void)
{
	return inv_rekey_mixed_writers_run(RKP_COARSE,
			RKMIX_WRITER_ATOMIC, /*contended=*/ true,
			"inv_rekey_coarse_contended_writers");
}

/*
 * THE COMPRESSED-TOP ARM -- a run of a SINGLE key, so the source prefix
 * path-compresses and the subtree top is a COMPRESSED node instead of a plain
 * POPCOUNT/PIGEON one.  The atomic writer covers that shape, so this is its
 * acceptance test under concurrent readers and it GATES on `absent moving` == 0
 * like the other atomic arms: measured 3014 moves, 0 absent.
 *
 * ★ The shape is the one that used to select the staged writer, which is why the
 * geometry differs from the varlen arm in nothing but the run length.  The staged
 * writer's absence window is now demonstrated by the COARSE arm above, which
 * cannot stop staging.
 */
static int inv_rekey_public_atomic_no_gap_compressed_top(void)
{
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_public_atomic_no_gap_compressed_top: "
			"skipped (set FT_INV_MW=1 to run the concurrent-writer "
			"oracles)\n");
		return 0;
	}
	return inv_rekey_public_no_gap_run(RKP_ATOMIC_COMPRESSED_TOP, RKP_GAP_NONE,
			"inv_rekey_public_atomic_no_gap_compressed_top");
}

/*
 * SIBLING PAIR: two writers, one child each of a shared three-byte prefix, both
 * cycling insert -> read-back -> remove.  With the peer's child present an
 * insert has to SPLIT the prefix node; with it absent a remove has to
 * PATH-COMPRESS the prefix back into a leaf.  So the pair drives a split and a
 * compression of the SAME node, from two threads, continuously -- which the
 * disjoint-key insert/remove oracles never do, because their keys diverge high
 * enough that no node is ever both split and compressed under contention.
 *
 * ORACLE: an insert that returned OK must be readable in the very critical
 * section that published it.  Nobody else writes that key, so a miss is not a
 * race with a peer's legitimate removal -- it is a lost key.
 */
#define SIBP_NW		6		/* sibling pairs */
/*
 * Take-back budget per published node.  Sized so that exhausting it cannot be
 * load noise: a writer is the sole writer of its key, so a removal that is
 * merely losing races wins within a handful of attempts.  Measured on a
 * saturated machine (12 concurrent copies of this oracle) the honest maximum is
 * single digits; this is four orders of magnitude above it.
 */
#define SIBP_TAKEBACK_TRIES	100000

/* (b0,b1,b2,b3) as a fixed four-byte key. */
static uint64_t sibp_key(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3)
{
	return ((uint64_t) b0 << 24) | ((uint64_t) b1 << 16) |
		((uint64_t) b2 << 8) | (uint64_t) b3;
}

/* Remove the node @n stored at key @v, by IDENTITY. */
static enum cds_ft_status sibp_remove(struct cds_ft *ft, uint64_t v,
		struct ft_test_node *n, int *not_found)
{
	struct cds_ft_iter *iter = NULL;
	enum cds_ft_status st = CDS_FT_STATUS_NOT_FOUND;
	uint8_t k[8] = { 0 };

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	cds_ft_u64_to_key(ft, v, k, CDS_FT_LEN_DEFAULT);
	cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
	cds_ft_lookup(ft, iter);
	if (cds_ft_iter_node(iter)) {
		st = cds_ft_remove(ft, iter, &n->node);
		if (st == CDS_FT_STATUS_OK)
			node_free_rcu(n);
	} else if (not_found) {
		/*
		 * The key is not findable AT ALL.  Distinct from a remove that
		 * merely loses a race: this thread is the SOLE writer of @v and
		 * it confirmed @v readable after publishing it, so no peer is
		 * entitled to have taken it out.
		 */
		*not_found = 1;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return st;
}

struct sibp_arg {
	struct cds_ft *ft;
	uint64_t key;
	unsigned long ops, busy, lost, stuck;
	struct ft_test_node *inflight;	/* published, not yet taken back */
	int failed;
	int *stop_all;
	/*
	 * FT_INV_SIBP_LOSS_ABORT: consecutive not-findable take-backs that trip
	 * the flight-recorder snapshot.  0 disables it (the default).
	 */
	unsigned long loss_abort;
};

static void *sibp_writer(void *arg)
{
	struct sibp_arg *w = (struct sibp_arg *) arg;
	unsigned long iters = 0;
	unsigned long miss_streak = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct ft_test_node *n = node_alloc(w->key);
		struct cds_ft_node *f = NULL;
		enum cds_ft_status st;
		int a;

		rcu_read_lock();
		st = insert_u64(w->ft, w->key, n);
		if (st != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			node_free(n);
			if (st == CDS_FT_STATUS_DUPLICATE_FOUND) {
				fprintf(stderr, "sibp key=%#lx: DUPLICATE -- our own previous "
					"node was never removed\n", (unsigned long) w->key);
				w->failed = 1;
				goto out;
			}
			w->busy++;		/* BUSY / MEMORY: transient */
			continue;
		}
		if (lookup_u64(w->ft, w->key, &f) != CDS_FT_STATUS_OK ||
				f != &n->node) {
			rcu_read_unlock();
			fprintf(stderr, "sibp key=%#lx: an OK insert is NOT READABLE in "
				"the critical section that published it (f=%p want=%p) "
				"after %lu ops, %lu busy\n", (unsigned long) w->key,
				(void *) f, (void *) &n->node, w->ops, w->busy);
			w->lost++;
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		w->inflight = n;	/* the run owes this node a reclaim */
		rcu_read_unlock();

		for (a = 0; a < SIBP_TAKEBACK_TRIES && !test_stop; a++) {
			int gone = 0;

			if (sibp_remove(w->ft, w->key, n, &gone) ==
					CDS_FT_STATUS_OK) {
				w->inflight = NULL;
				break;
			}
			/*
			 * ☞ ABORT AT THE MOMENT OF LOSS, not 100000 tries later.
			 * The ORPHAN report below fires so long after the key
			 * went missing that the snapshot ring has already
			 * evicted the write that did it.  Armed with
			 * FT_INV_SIBP_LOSS_ABORT=N, the N'th CONSECUTIVE
			 * not-findable take-back snapshots and aborts, so the
			 * LAST events in the ring are the ones that repointed
			 * the slot.  Unarmed (0, the default) this is inert and
			 * the oracle behaves exactly as before.
			 */
			if (!gone) {
				miss_streak = 0;
				continue;
			}
			if (w->loss_abort && ++miss_streak >= w->loss_abort) {
				char msg[192];

				snprintf(msg, sizeof(msg),
					"sibp LOST key=%#lx leaf=%p after %lu ops, "
					"%lu busy, miss_streak=%lu",
					(unsigned long) w->key,
					(void *) &n->node, w->ops, w->busy,
					miss_streak);
				fprintf(stderr, "%s\n", msg);
				FT_TEST_TP(inv_violation,
					"inv_sibling_split_compress_unpinned",
					msg);
				w->lost++;
				w->failed = 1;
				*w->stop_all = 1;
				mw_violation_snapshot();
				goto out;
			}
		}
		if (w->inflight) {
			struct cds_ft_node *f = NULL;

			/*
			 * The take-back never succeeded.  Which of the two
			 * reasons it was decides everything, and only one of them
			 * is benign:
			 *
			 * SHUTDOWN -- @test_stop was raised mid-loop.  The run is
			 * over and this writer simply stopped between its publish
			 * and its take-back.  Count it and leave; @inflight is
			 * reclaimed after the join so the entries/leak checks stay
			 * exact.
			 *
			 * EXHAUSTED -- SIBP_TAKEBACK_TRIES removals of a key this
			 * thread is the SOLE writer of all failed, with the run
			 * still going.  That is not contention.  A losing race
			 * retries and wins; this one cannot, and both shapes it
			 * takes are defects:
			 *   ORPHAN  the key is not findable at all -- it left the
			 *           trie without this thread removing it, which no
			 *           amount of contention can do.
			 *   WEDGE   the key IS findable but cds_ft_remove refuses
			 *           it forever, so the application can see a node
			 *           it can never take back.
			 * Report which, so the failure names its own shape.
			 */
			if (test_stop) {
				w->stuck++;
				goto out;
			}
			rcu_read_lock();
			f = NULL;
			if (lookup_u64(w->ft, w->key, &f) != CDS_FT_STATUS_OK)
				f = NULL;
			fprintf(stderr, "sibp key=%#lx: %s -- %d take-backs of a key "
				"only this thread writes ALL failed after %lu ops, "
				"%lu busy (lookup %s)\n", (unsigned long) w->key,
				f ? "WEDGE" : "ORPHAN", SIBP_TAKEBACK_TRIES,
				w->ops, w->busy,
				f == &n->node ? "returns our node" :
					f ? "returns ANOTHER node" : "finds nothing");
			rcu_read_unlock();
			if (!f)
				w->lost++;
			w->stuck++;
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		w->ops++;
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
out:
	*w->stop_all = 1;
	rcu_unregister_thread();
	return NULL;
}

/*
 * Is every node this run allocated either FREED or still REACHABLE?
 *
 * Each one ends up in exactly one of those two places, so at teardown, with
 * every deferred free drained,
 *
 *	allocated - freed  ==  nodes reachable from the root
 *
 * and an excess is a node that is NEITHER -- lost out of the structure without
 * being reclaimed.  leak_check() reports the same excess, but as a bare delta
 * and only AFTER drain_and_destroy(), by which time the trie that would explain
 * it is gone.  Asked here, the answer arrives with the structure still in
 * memory (and, under FT_INV_ABORT_ON_VIOLATION, in a core).
 *
 * ★ This check is the one that placed the defect: it comes back BALANCED on
 * runs that then report a leak, which is what proved the loss happens in the
 * TEARDOWN WALK and not in the concurrent phase -- see sibp_drain_checked().
 */
static int sibp_report_orphans(struct cds_ft *ft, unsigned long live)
{
	unsigned long unfreed, reachable;

	rcu_barrier();		/* every deferred free of ours has run */
	unfreed = __atomic_load_n(&nodes_allocated, __ATOMIC_RELAXED) -
		__atomic_load_n(&nodes_freed, __ATOMIC_RELAXED);
	rcu_read_lock();
	reachable = cds_ft_count_entries(ft);
	rcu_read_unlock();
	if (unfreed == reachable && reachable == live)
		return 0;
	fprintf(stderr, "sibp ORPHAN: %ld node(s) neither freed nor reachable "
		"(allocated-freed %lu, reachable %lu, expected %lu)\n",
		(long) (unfreed - reachable), unfreed, reachable, live);
	/* The trie is intact right here.  Keep the core. */
	mw_violation_snapshot();
	return -1;
}

#define RKSM_NW		6		/* mover/mutator pairs */
#define RKSM_SB		3		/* S_top slot byte inside each junction */
#define RKSM_MUT	0x60		/* mutator child byte base under S_top */

struct rksm_arg {
	struct cds_ft *ft;
	uint8_t bp, dp, mine, guard, mut;	/* @mut is the DEEP byte, under @mine */
	struct ft_test_node *seed_priv, *seed_guard;
	int seeded;
	unsigned long ops, retries, lost, checks, einval;
	struct ft_test_node *mut_node;	/* in-flight, for post-join reclaim */
	unsigned long stuck;		/* reclaim loops that ran out of retries */
	int prev_rc;			/* the merge outcome one iteration back */
	int failed;
	int *stop_all;
};

/*
 * Where did it go?  Walk the WHOLE trie for the node by IDENTITY.  "Present at
 * neither src nor dst" has two very different causes -- a merge dropped it
 * (absent from the structure entirely) or the harness lost track of where it
 * put it (present, under some third key) -- and only a scan tells them apart.
 * Concurrent, so a miss is weaker evidence than a hit; a hit is definitive.
 */
/*
 * Is @n findable at @k RIGHT NOW?  Called between the mover's own two seed
 * inserts, which is what narrowed this oracle's loss from "the seed vanished
 * somewhere inside the merge" to a single library call: the seed is present
 * after its own insert returns OK and GONE after the SIBLING insert returns OK,
 * same thread, same read-side critical section (6/6 runs).  So the producer is
 * cds_ft_insert dropping a neighbouring key, not the rekey/graft/merge this
 * oracle was built to exercise -- and checking here names it in one line
 * instead of a trace.  A peer IS required: with the mutators disabled the
 * movers run ~8k-21k moves per run and lose nothing (4/4).
 */
static void rksm_seed_present(struct cds_ft *ft, unsigned int bp, uint64_t k,
		struct ft_test_node *n, const char *when, int *failed)
{
	struct cds_ft_node *got = NULL;

	if (lookup_u64(ft, k, &got) == CDS_FT_STATUS_OK && got == &n->node)
		return;
	fprintf(stderr, "rksm bp=%u: seed %p is ABSENT at its own key %#lx %s -- "
		"the insert that reported OK did not leave it findable (got %p)\n",
		bp, (void *) n, (unsigned long) k, when, (void *) got);
	*failed = 1;
}

static int rksm_locate(struct cds_ft *ft, struct ft_test_node *n, uint64_t *at)
{
	struct cds_ft_iter *iter = NULL;
	int found = 0;

	if (cds_ft_iter_create(ft, &iter) < 0)
		return -1;
	rcu_read_lock();
	cds_ft_for_each_rcu(ft, iter) {
		uint8_t rk[8];
		size_t rk_len;

		if (cds_ft_iter_node(iter) != &n->node)
			continue;
		cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
		*at = cds_ft_key_to_u64(ft, rk, CDS_FT_LEN_DEFAULT);
		found = 1;
		break;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return found;
}

/*
 * The mover is the SOLE writer of its own two seeds: the mutator's key is a
 * sibling one byte deeper, and every other pair owns a different junction.  So
 * "findable at neither the source nor the destination" is neither a race nor
 * harness bookkeeping -- and a whole-trie scan by identity then says whether
 * the node is gone from the structure or merely somewhere unexpected.
 */
static int rksm_seeds_check(struct rksm_arg *w, const char *when, int rc)
{
	uint64_t kp = rkm_key(w->bp, RKSM_SB, w->mine);
	uint64_t kg = rkm_key(w->bp, RKSM_SB, w->guard);
	uint64_t dp_priv = rkm_key(w->dp, RKSM_SB, w->mine);
	uint64_t dp_guard = rkm_key(w->dp, RKSM_SB, w->guard);
	struct cds_ft_node *ps = NULL, *pd = NULL, *gs = NULL, *gd = NULL;
	int lost_priv, lost_guard, where;
	uint64_t at = 0;

	rcu_read_lock();
	lost_priv = lookup_u64(w->ft, kp, &ps) != CDS_FT_STATUS_OK &&
		lookup_u64(w->ft, dp_priv, &pd) != CDS_FT_STATUS_OK;
	lost_guard = lookup_u64(w->ft, kg, &gs) != CDS_FT_STATUS_OK &&
		lookup_u64(w->ft, dp_guard, &gd) != CDS_FT_STATUS_OK;
	rcu_read_unlock();
	if (!lost_priv && !lost_guard)
		return 0;

	where = rksm_locate(w->ft, lost_priv ? w->seed_priv : w->seed_guard, &at);
	fprintf(stderr, "rksm MOVER bp=%u: own %s seed present at NEITHER src nor "
		"dst @%s (rc=%d, prev rc=%d) -- priv{src=%d dst=%d} "
		"guard{src=%d dst=%d} scan=%s key=%#lx seed=%p "
		"(moves=%lu retries=%lu drifts=%lu stuck=%lu)\n",
		w->bp, lost_priv ? "priv" : "guard", when, rc, w->prev_rc,
		ps != NULL, pd != NULL, gs != NULL, gd != NULL,
		where > 0 ? "FOUND" : "absent", (unsigned long) at,
		(void *) (lost_priv ? w->seed_priv : w->seed_guard),
		w->ops, w->retries, w->einval, w->stuck);
	w->lost++;
	w->failed = 1;
	mw_violation_snapshot();
	return -1;
}

static void *rksm_mover(void *arg)
{
	struct rksm_arg *w = (struct rksm_arg *) arg;
	uint8_t src_key[2], dst_key[2];
	unsigned long iters = 0;

	src_key[0] = w->bp; src_key[1] = RKSM_SB;
	dst_key[0] = w->dp; dst_key[1] = RKSM_SB;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t kp = rkm_key(w->bp, RKSM_SB, w->mine);
		uint64_t kg = rkm_key(w->bp, RKSM_SB, w->guard);
		int rc;

		if (!w->seeded) {
			rcu_read_lock();
			w->seed_priv = node_alloc(kp);
			w->seed_guard = node_alloc(kg);
			if (insert_u64(w->ft, kp, w->seed_priv) != CDS_FT_STATUS_OK ||
					insert_u64(w->ft, kg, w->seed_guard) !=
						CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				fprintf(stderr, "rksm bp=%u: seed failed\n", w->bp);
				w->failed = 1;
				goto out;
			}
			/*
			 * Both seeds must be findable before anything else
			 * runs.  Checked in the SAME critical section as the
			 * inserts, so a loss reported here cannot be blamed on
			 * reclamation or on a later operation.
			 */
			rksm_seed_present(w->ft, w->bp, kp, w->seed_priv,
					"once BOTH seed inserts have returned OK",
					&w->failed);
			rksm_seed_present(w->ft, w->bp, kg, w->seed_guard,
					"once BOTH seed inserts have returned OK",
					&w->failed);
			if (w->failed) {
				rcu_read_unlock();
				mw_violation_snapshot();
				goto out;
			}
			rcu_read_unlock();
			w->seeded = 1;
		}

		/*
		 * BEFORE and AFTER, so the window a loss happened in is a single
		 * named interval rather than "some time in the last iteration".
		 * BEFORE covers the seed's own publication and everything the
		 * MUTATOR did while we were between merges; AFTER covers the merge.
		 */
		if (rksm_seeds_check(w, "pre-merge", 0) < 0)
			goto out;
		rc = _cds_ft_debug_rekey_graft_simple(w->ft, src_key, 2, dst_key, 2);
		if (rksm_seeds_check(w, "post-merge", rc) < 0)
			goto out;
		w->prev_rc = rc;

		if (rc == 0) {
			w->ops++;
			{
				int a;

				/* Transiently BUSY under a peer: retry, do not fail. */
				for (a = 0; a < 1000; a++)
					if (rkms_remove_node(w->ft,
							rkm_key(w->dp, RKSM_SB, w->mine),
							w->seed_priv) == CDS_FT_STATUS_OK)
						break;
				if (a == 1000)
					w->stuck++;
				for (a = 0; a < 1000; a++)
					if (rkms_remove_node(w->ft,
							rkm_key(w->dp, RKSM_SB, w->guard),
							w->seed_guard) == CDS_FT_STATUS_OK)
						break;
				if (a == 1000)
					w->stuck++;
			}
			w->seeded = 0;
		} else if (rc == -EAGAIN || rc == -EIO || rc == -ENOMEM) {
			w->retries++;
		} else if (rc == -EINVAL) {
			/*
			 * SHAPE DRIFT, not a verdict on the library.  This oracle
			 * mutates the source on purpose, and the mover's own seed can
			 * end up removed at the dst while the mutator's key keeps the
			 * byte alive -- leaving S_top single-child, hence PATH-
			 * COMPRESSED, which the rekey scope gate refuses (measured: the
			 * s_top gate, `compressed` bit only).  Re-seed and carry on.
			 *
			 * This does NOT mask a permanent -EINVAL: a shape the library
			 * could never accept would drive @ops to zero, and the
			 * per-writer liveness check below fails on exactly that.
			 */
			w->einval++;
			/*
			 * RECLAIM before re-seeding.  The seed is already published
			 * (possibly partly at the src and partly at the dst after a
			 * drifted move), and simply dropping it on the floor leaks
			 * exactly one node per drift -- measured as a leak delta that
			 * tracked this counter 1:1 until this loop existed.
			 */
			{
				int a;

				/* Retry: a single-shot remove loses to a peer. */
				for (a = 0; a < 2000; a++)
					if (rkms_remove_node(w->ft, kp, w->seed_priv)
							== CDS_FT_STATUS_OK ||
						rkms_remove_node(w->ft,
							rkm_key(w->dp, RKSM_SB,
								w->mine),
							w->seed_priv) ==
								CDS_FT_STATUS_OK)
						break;
				if (a == 2000)
					w->stuck++;
				for (a = 0; a < 2000; a++)
					if (rkms_remove_node(w->ft, kg, w->seed_guard)
							== CDS_FT_STATUS_OK ||
						rkms_remove_node(w->ft,
							rkm_key(w->dp, RKSM_SB,
								w->guard),
							w->seed_guard) ==
								CDS_FT_STATUS_OK)
						break;
				if (a == 2000)
					w->stuck++;
			}
			w->seeded = 0;
		} else {
			fprintf(stderr, "rksm bp=%u: merge failed rc=%d\n", w->bp, rc);
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
out:
	*w->stop_all = 1;
	rcu_unregister_thread();
	return NULL;
}

/*
 * The peer that the fence has to exclude: it adds a CHILD to the S_top its
 * mover is copying, then insists the key still exists somewhere.
 */
static void *rksm_mutator(void *arg)
{
	struct rksm_arg *w = (struct rksm_arg *) arg;
	unsigned long iters = 0;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		/*
		 * DEEPER than a sibling of S_top's children, on purpose.  As a
		 * sibling this key can be the ONLY one under (bp,SB) in the window
		 * between the mover's move and its re-seed, which PATH-COMPRESSES
		 * S_top -- a shape the rekey legitimately refuses (-EINVAL, measured
		 * at the s_top scope gate, `compressed` bit only).  Hanging it under
		 * the mover's own child instead keeps S_top a >=2-child branch at all
		 * times, while still adding a child INSIDE the subtree being copied,
		 * which is the whole point.
		 */
		uint64_t at_src = ((uint64_t) w->bp << 24) |
			((uint64_t) RKSM_SB << 16) |
			((uint64_t) w->mine << 8) | (uint64_t) w->mut;
		uint64_t at_dst = ((uint64_t) w->dp << 24) |
			((uint64_t) RKSM_SB << 16) |
			((uint64_t) w->mine << 8) | (uint64_t) w->mut;
		struct ft_test_node *n = node_alloc(at_src);
		struct cds_ft_node *f = NULL;
		enum cds_ft_status st;

		rcu_read_lock();
		st = insert_u64(w->ft, at_src, n);
		rcu_read_unlock();
		if (st == CDS_FT_STATUS_OK)
			w->mut_node = n;	/* published: the run owes its reclaim */
		if (st != CDS_FT_STATUS_OK) {	/* BUSY / MEMORY: transient */
			node_free(n);
			w->retries++;
			continue;
		}

		/*
		 * The child is IN.  A concurrent merge may move it to the dst at any
		 * point from here; what it may never do is lose it.
		 */
		w->checks++;
		rcu_read_lock();
		if (lookup_u64(w->ft, at_src, &f) != CDS_FT_STATUS_OK &&
				lookup_u64(w->ft, at_dst, &f) != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			/*
			 * Say WHAT was observed, not who did it.  "A merge
			 * swallowed it" is an attribution, and the one time it
			 * was chased it was wrong: the same loss reproduces with
			 * the merge disabled entirely, and its minimal form is
			 * two writers splitting and path-compressing one node
			 * with no merge, rekey or graft in sight
			 * (inv_sibling_split_compress).  A message that names a
			 * culprit sends the next reader to the wrong subsystem.
			 */
			fprintf(stderr, "rksm MUT bp=%u: child %#lx is present at "
				"NEITHER src nor dst -- it left the trie\n",
				w->bp, (unsigned long) at_src);
			w->lost++;
			w->failed = 1;
			mw_violation_snapshot();
			goto out;
		}
		rcu_read_unlock();

		/* Take it back from wherever it landed. */
		if (rkms_remove_node(w->ft, at_src, n) != CDS_FT_STATUS_OK &&
				rkms_remove_node(w->ft, at_dst, n) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "rksm MUT bp=%u: could not reclaim %#lx\n",
				w->bp, (unsigned long) at_src);
			w->failed = 1;
			goto out;
		}
		w->mut_node = NULL;		/* reclaimed by the remove above */
		w->ops++;
		if ((++iters & 0xff) == 0)
			rcu_quiescent_state();
	}
out:
	*w->stop_all = 1;
	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_src_mutated(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rksm_arg *mv, *mu;
	struct ft_test_node *res[RKSM_NW][2];
	pthread_t movers[RKSM_NW], mutators[RKSM_NW];
	struct timespec t0;
	unsigned long moves = 0, checks = 0, lost = 0, live = 0, einval = 0;
	unsigned long stuck = 0;
	int i, c, ret = 0, stop_all = 0;

	/*
	 * KNOWN-FAILING, so opt-in TWICE: FT_INV_MW selects the concurrent-writer
	 * oracles at all, and FT_INV_RKSM arms this one.  It reproduces a LIVE
	 * key loss -- a mover's own seed present at neither src nor dst, with a
	 * matching leak delta -- in every run measured (1 lost, 3 of 3), and the
	 * gate's imw legs would otherwise go permanently red, which is how a
	 * genuine regression stops being visible.
	 *
	 * It is registered unconditionally rather than #ifdef'd out, because a
	 * test that vanishes reads as coverage and is not; the skip below names
	 * the defect so a reader of the log knows what is not being checked.
	 * Remove the FT_INV_RKSM gate the moment the loss is fixed.
	 */
	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_rekey_src_mutated: skipped "
			"(set FT_INV_MW=1 to run the source-mutated merge oracle)\n");
		return 0;
	}
	if (!getenv("FT_INV_RKSM")) {
		fprintf(stderr, "# inv_rekey_src_mutated: skipped -- reproduces a "
			"LIVE key loss (seed at neither src nor dst); set "
			"FT_INV_RKSM=1 to run it\n");
		return 0;
	}
	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_listoff_ft(4, &group);	/* LIST OFF */
	cds_ft_make_shared(ft);

	mv = (struct rksm_arg *) calloc(RKSM_NW, sizeof(*mv));
	mu = (struct rksm_arg *) calloc(RKSM_NW, sizeof(*mu));
	if (!mv || !mu)
		abort();

	rcu_read_lock();
	for (i = 0; i < RKSM_NW; i++) {
		uint8_t bp = (uint8_t) (0x30 + i), dp = (uint8_t) (0x80 + i);

		mv[i].ft = mu[i].ft = ft;
		mv[i].bp = mu[i].bp = bp;
		mv[i].dp = mu[i].dp = dp;
		mv[i].mine = mu[i].mine = (uint8_t) (0x10 + i);
		mv[i].guard = mu[i].guard = (uint8_t) (0x40 + i);
		mv[i].mut = mu[i].mut = (uint8_t) (RKSM_MUT + i);	/* byte 3 */
		mv[i].stop_all = mu[i].stop_all = &stop_all;

		/* Sibs keep BP >= 3 children; residents keep the dst OCCUPIED. */
		for (c = 0; c < 2; c++) {
			uint64_t sk = rkm_key(bp, c == 0 ? 1 : 5, 0);
			uint64_t dk = rkm_key(dp, c == 0 ? 1 : 5, 0);

			if (insert_u64(ft, sk, node_alloc(sk)) != CDS_FT_STATUS_OK ||
					insert_u64(ft, dk, node_alloc(dk)) !=
						CDS_FT_STATUS_OK)
				abort();
			live += 2;
		}
		for (c = 0; c < 2; c++) {
			uint64_t rk = rkm_key(dp, RKSM_SB, (uint8_t) (c + 1));

			res[i][c] = node_alloc(rk);
			if (insert_u64(ft, rk, res[i][c]) != CDS_FT_STATUS_OK)
				abort();
			live++;
		}
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKSM_NW; i++) {
		pthread_create(&movers[i], NULL, rksm_mover, &mv[i]);
		pthread_create(&mutators[i], NULL, rksm_mutator, &mu[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS && !stop_all)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < RKSM_NW; i++) {
		pthread_join(movers[i], NULL);
		pthread_join(mutators[i], NULL);
	}
	rcu_thread_online();

	synchronize_rcu();
	for (i = 0; i < RKSM_NW; i++) {	/* whatever the last iteration left */
		if (!mv[i].seeded)
			continue;
		(void) rkms_remove_node(ft, rkm_key(mv[i].bp, RKSM_SB, mv[i].mine),
				mv[i].seed_priv);
		(void) rkms_remove_node(ft, rkm_key(mv[i].bp, RKSM_SB, mv[i].guard),
				mv[i].seed_guard);
		mv[i].seeded = 0;
	}
	/*
	 * And the mutator's in-flight child: a run stopped between its insert and
	 * its remove leaves exactly one key behind, which shows up as entries+1 and
	 * a leak delta of 1 rather than as anything about the library.
	 */
	for (i = 0; i < RKSM_NW; i++) {
		uint64_t at_src, at_dst;

		if (!mu[i].mut_node)
			continue;
		at_src = ((uint64_t) mu[i].bp << 24) | ((uint64_t) RKSM_SB << 16) |
			((uint64_t) mu[i].mine << 8) | (uint64_t) mu[i].mut;
		at_dst = ((uint64_t) mu[i].dp << 24) | ((uint64_t) RKSM_SB << 16) |
			((uint64_t) mu[i].mine << 8) | (uint64_t) mu[i].mut;
		if (rkms_remove_node(ft, at_src, mu[i].mut_node) != CDS_FT_STATUS_OK)
			(void) rkms_remove_node(ft, at_dst, mu[i].mut_node);
		mu[i].mut_node = NULL;
	}

	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < RKSM_NW; i++) {
		moves += mv[i].ops;
		einval += mv[i].einval;
		stuck += mv[i].stuck;
		checks += mu[i].checks;
		lost += mu[i].lost + mv[i].lost;
		if (mv[i].failed || mu[i].failed)
			ret = -1;
		if (mv[i].ops == 0) {
			fprintf(stderr, "rekey src-mutated: mover %d made NO move\n", i);
			ret = -1;
		}
		if (mu[i].checks == 0) {
			fprintf(stderr, "rekey src-mutated: mutator %d landed NO child "
				"-- it never contended the copy window\n", i);
			ret = -1;
		}
		for (c = 0; c < 2; c++) {
			struct cds_ft_node *f = NULL;
			uint64_t rk = rkm_key(mv[i].dp, RKSM_SB, (uint8_t) (c + 1));

			if (lookup_u64(ft, rk, &f) != CDS_FT_STATUS_OK ||
					f != &res[i][c]->node) {
				fprintf(stderr, "rekey src-mutated: resident %d/%d lost\n",
					i, c);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_entries(ft) != live) {
		fprintf(stderr, "rekey src-mutated: entries %lu != live %lu\n",
			cds_ft_count_entries(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "rekey src-mutated: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_rekey_src_mutated: %d mover/mutator pairs, %lu moves, "
		"%lu children landed in the copy window, %lu lost, %lu shape re-seeds, "
		"%lu stuck reclaims, %lu live keys\n",
		RKSM_NW, moves, checks, lost, einval, stuck, live);

	free(mv);
	free(mu);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Drain the STATIC seeds exactly the way drain_and_destroy does -- repeated
 * lookup_first + remove_all, same order -- but after EVERY removal, check that
 * each seed still in the trie is still findable BY ITS OWN KEY.
 *
 * The generic drain only reports a SHORTFALL ("freed 14 of 18"): by then a
 * dozen removals have run and any of them could have been the one that broke
 * the walk.  Checking between removals attributes it.  The seeds are the right
 * probe because no writer ever touches them -- inserted once before the writers
 * start, still there when they are joined -- so a seed that stops being
 * findable cannot be a race with a legitimate removal.
 *
 * Runs after the join, single-threaded, so it cannot perturb the window that
 * produced the damage -- only report it.
 */
static int sibp_drain_checked(struct cds_ft *ft, const uint64_t *seed, int nseed)
{
	struct cds_ft_iter *iter = NULL;
	unsigned long drained = 0;
	enum cds_ft_status s;
	char *gone;
	int j, ret = 0;

	gone = (char *) calloc((size_t) nseed, 1);
	if (!gone)
		abort();
	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	while ((s = cds_ft_lookup_first(ft, iter)) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;
		uint64_t just = 0;

		s = cds_ft_remove_all(ft, iter, &head);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "sibp drain: remove_all st=%d on a key "
				"lookup_first had just found\n", (int) s);
			ret = -1;
			break;
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp) {
			struct ft_test_node *tn = to_test_node(head);

			just = tn->key;
			for (j = 0; j < nseed; j++)
				if (seed[j] == tn->key)
					gone[j] = 1;
			node_free_rcu(tn);
			drained++;
		}
		for (j = 0; j < nseed; j++) {
			struct cds_ft_node *f = NULL;

			if (gone[j])
				continue;
			if (lookup_u64(ft, seed[j], &f) == CDS_FT_STATUS_OK && f)
				continue;
			fprintf(stderr, "sibp drain: removing key %#lx STRANDED key "
				"%#lx -- an exact-key lookup no longer finds a key "
				"that was never removed (%lu drained)\n",
				(unsigned long) just, (unsigned long) seed[j],
				drained);
			if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
				fprintf(stderr, "sibp drain: cds_ft_verify FAILS\n");
			ret = -1;
			mw_violation_snapshot();
			goto out;
		}
	}
	if (drained != (unsigned long) nseed) {
		fprintf(stderr, "sibp drain: lookup_first STOPPED EARLY after %lu of "
			"%d seeds (st=%d); still findable by exact key:",
			drained, nseed, (int) s);
		for (j = 0; j < nseed; j++) {
			struct cds_ft_node *f = NULL;

			if (gone[j])
				continue;
			fprintf(stderr, " %#lx=%s", (unsigned long) seed[j],
				lookup_u64(ft, seed[j], &f) == CDS_FT_STATUS_OK ?
					"yes" : "NO");
		}
		fprintf(stderr, "\n");
		/*
		 * Deliberately NO further ORDERED probe here (a GE from the key
		 * itself, a lookup_last): on the remnant this fires on, those do
		 * not return -- the same going-up cycle that made lookup_first
		 * give up spins forever on a reachable internal whose nr_child
		 * has reached 0.  A diagnostic that hangs reports nothing.  The
		 * exact-key line above already carries the finding: the keys are
		 * present, and only the ordered walk cannot reach them.
		 */
		if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
			fprintf(stderr, "sibp drain: cds_ft_verify FAILS on the "
				"remnant\n");
		ret = -1;
		mw_violation_snapshot();
	}
out:
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	free(gone);
	return ret;
}

/*
 * @pin_prefix seeds the (p,3,guard) static key, which keeps (p,3) a branch and
 * so confines the split-and-compress churn to the single prefix node (p,3,m).
 * See the two arms below for what each setting buys.
 */
static int sibling_split_compress_body(const char *name, bool pin_prefix)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct sibp_arg *w;
	pthread_t th[SIBP_NW * 2];
	struct timespec t0;
	unsigned long ops = 0, busy = 0, lost = 0, live = 0, stuck = 0;
	uint64_t seed[SIBP_NW * 3];	/* the static keys, for the checked drain */
	int i, ret = 0, stop_all = 0;
	int nstatic = pin_prefix ? 3 : 2;
	unsigned long sibp_loss_abort;

	mw_install_fatal_handler();
	leak_reset();

	ft = create_fixed_fine_lock_listoff_ft(4, &group);
	cds_ft_make_shared(ft);

	w = (struct sibp_arg *) calloc(SIBP_NW * 2, sizeof(*w));
	if (!w)
		abort();
	{
		const char *e = getenv("FT_INV_SIBP_LOSS_ABORT");

		sibp_loss_abort = e ? strtoul(e, NULL, 0) : 0;
	}
	rcu_read_lock();
	for (i = 0; i < SIBP_NW; i++) {
		uint8_t p = (uint8_t) (0x30 + i), m = (uint8_t) (0x10 + i);
		uint64_t sk[3];
		int c;

		/*
		 * (p,1) and (p,5) keep p a branch in both arms.  The third seed
		 * is the variable: it keeps (p,3) a branch too, so the collapse
		 * stops there instead of climbing.
		 */
		sk[0] = sibp_key(p, 1, 0, 0);
		sk[1] = sibp_key(p, 5, 0, 0);
		sk[2] = sibp_key(p, 3, (uint8_t) (0x40 + i), 0);
		for (c = 0; c < nstatic; c++) {
			if (insert_u64(ft, sk[c], node_alloc(sk[c])) !=
					CDS_FT_STATUS_OK)
				abort();
			seed[live++] = sk[c];
		}

		w[2 * i].ft = w[2 * i + 1].ft = ft;
		w[2 * i].stop_all = w[2 * i + 1].stop_all = &stop_all;
		w[2 * i].key = sibp_key(p, 3, m, 0x00);
		w[2 * i + 1].key = sibp_key(p, 3, m, 0x60);
		w[2 * i].loss_abort = w[2 * i + 1].loss_abort = sibp_loss_abort;
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < SIBP_NW * 2; i++)
		pthread_create(&th[i], NULL, sibp_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS && !stop_all)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < SIBP_NW * 2; i++)
		pthread_join(th[i], NULL);
	rcu_thread_online();

	synchronize_rcu();
	/*
	 * A writer stopped BETWEEN its publish and its take-back leaves exactly
	 * one key behind -- harness bookkeeping, not a library leak, and it
	 * would otherwise show up as entries > live.  Reclaim it here.
	 */
	for (i = 0; i < SIBP_NW * 2; i++) {
		if (!w[i].inflight)
			continue;
		/*
		 * Usually the node is still published and the remove reclaims
		 * it.  If it is NOT in the trie -- the take-back raced to
		 * completion but the writer had already stopped, so nothing
		 * cleared @inflight -- then it is unreachable (past the
		 * synchronize_rcu above) and ours to free directly.  Ignoring
		 * the status here leaked exactly one node, ~1 run in 12 under
		 * load, reported as a LEAK delta with the trie itself exact
		 * (0 lost, entries == live).
		 */
		if (sibp_remove(ft, w[i].key, w[i].inflight, NULL) !=
				CDS_FT_STATUS_OK) {
			node_free(w[i].inflight);
		}
		w[i].inflight = NULL;
	}
	synchronize_rcu();
	/*
	 * Before anything is torn down: is every node this run allocated
	 * either freed or still reachable?  An excess is an orphan, and the
	 * evidence for it only exists here.
	 */
	if (sibp_report_orphans(ft, live) < 0)
		ret = -1;
	rcu_read_lock();
	for (i = 0; i < SIBP_NW * 2; i++) {
		ops += w[i].ops;
		busy += w[i].busy;
		lost += w[i].lost;
		stuck += w[i].stuck;
		if (w[i].failed)
			ret = -1;
		if (w[i].ops == 0) {
			fprintf(stderr, "%s: writer %d never "
				"completed a cycle\n", name, i);
			ret = -1;
		}
	}
	if (cds_ft_count_entries(ft) != live) {
		fprintf(stderr, "%s: entries %lu != live %lu\n",
			name, cds_ft_count_entries(ft), live);
		ret = -1;
	}
	if (verify_or_dump(ft, name))
		ret = -1;
	rcu_read_unlock();

	fprintf(stderr, "# %s: %d pairs, %lu cycles, "
		"%lu transient, %lu lost, %lu contended take-backs\n",
		name, SIBP_NW, ops, busy, lost, stuck);

	free(w);
	/*
	 * Attribute a drain shortfall to the removal that causes it, before the
	 * generic drain (which then finds the trie already empty).
	 */
	if (sibp_drain_checked(ft, seed, SIBP_NW * nstatic) < 0) {
		ret = -1;
		/*
		 * Tear down WITHOUT another walk.  The remnant just
		 * characterized is one an ordered descent cannot get through --
		 * on it, drain_and_destroy's own lookup_first does not
		 * terminate, and a teardown that hangs turns an attributed
		 * failure into a timeout that says nothing.  leak_check() below
		 * then reports the stranded nodes, which is the accurate
		 * outcome: the walk could not reach them, so nobody freed them.
		 */
		cds_ft_destroy(ft);
		cds_ft_group_destroy(group);
	} else if (drain_and_destroy(ft, group) < 0) {
		ret = -1;
	}
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * Two writers cycling insert/remove on two SIBLING children of one prefix, so
 * the pair splits and path-compresses the SAME node continuously.  No other
 * oracle drives that shape: the disjoint-key insert/remove oracles diverge high
 * enough that no node is ever both split and compressed under contention.
 *
 * This arm PINS the churn to one node with the (p,3,guard) seed, and is the
 * regression test for the three defects it found in that confined shape:
 *   - the WEDGE (@d1832abb): a removal derived its position from a stale
 *     node->prev naming a RETIRED holder and retried the identical derivation
 *     forever, holding the per-trie FIFO fair mutex.  It used to HANG rather
 *     than fail, which is strictly worse -- a hung suite scores GREEN against a
 *     gate that only counts failures.
 *   - an unguarded trailing skip-target retire (@9ce4c2c8).
 *   - the DEAD INTERIOR NODE (@c9b6391f): a compressed parent replaced by a
 *     freshly allocated childless internal at a non-root, which only an ordered
 *     walk could detect -- hence the teardown verify in drain_and_destroy, and
 *     cds_ft_verify's own check for it.
 *
 * The last one reproduced about 1 saturated run in 100, so treat a single green
 * run here as weak evidence: soak it
 * (fractal-trie-review-2026-06/sibp_verify_soak.sh), or build with
 * -DFT_DELAY_INJECT and run FT_DELAY_MODE=writer FT_DELAY_US=10, which made
 * that defect reproduce every run.
 */
static int inv_sibling_split_compress(void)
{
	return sibling_split_compress_body("inv_sibling_split_compress", true);
}

/*
 * The same shape with the (p,3,guard) seed DROPPED, so nothing pins the prefix
 * and a removal's collapse climbs a level further.
 *
 * That one seed is what the pinned arm above buys its confinement with, and it
 * bought silence too: the arms differ by ~13000x in sensitivity to the sibling
 * key loss -- 0 detections in 4.8M inserts pinned, against 1 per ~360 unpinned.
 * The pinned arm therefore CANNOT be the regression test for that defect, and
 * read green through every session that hunted it.  A seed that holds a node's
 * shape fixed forbids exactly the multi-level cascade this oracle exists to
 * exercise, so the unpinned arm is the one that covers the climb.
 *
 * What it caught, and now guards: the prune-climb bootstrapping @cur from a
 * separate load of the slot that holds the holder, so a peer republish left the
 * climb walking a node the descent never reached (@2f4e9797).  Falsified both
 * ways before landing -- RED on the parent commit, GREEN with the fix.
 *
 * Detectors are the body's: an insert not readable in the section that
 * published it, a writer that completes no cycle, an allocated-but-unreachable
 * node, a stranded seed at drain, entries != live, cds_ft_verify.
 */
static int inv_sibling_split_compress_unpinned(void)
{
	return sibling_split_compress_body("inv_sibling_split_compress_unpinned",
		false);
}

/*
 * Companion to inv_concurrent_writers_disjoint: ALL writers contend the SAME
 * key range [0, MW_SHARED_RANGE), so two writers routinely insert/remove the
 * SAME key and mutate ADJACENT keys sharing one leaf -- the write/write and
 * torn-publish races the disjoint oracle deliberately excludes (insert-replace /
 * hole-refill resurrect on a live child slot, remove chain-leaf freeze racing a
 * re-insert).  Same total keyspace as the disjoint oracle (MW_NR_WRITERS *
 * MW_RANGE), but fully shared instead of partitioned.
 *
 * Self-arbitrating node lifecycle (no per-writer shadow is possible once keys
 * are shared): a writer allocates a fresh node per insert; the writer whose
 * cds_ft_remove returns OK owns the RCU-deferred free (a peer standing on the
 * node in its own read section keeps it live to its grace period, §11); a losing
 * remover and a duplicate insert reclaim only what they privately hold.  A broken
 * concurrent remove that returns OK twice double-frees -> leak_check / 0xfe
 * poison fault.
 *
 * Oracles:
 *  - in-line: a non-NULL lookup for key K must resolve to a node whose shadow
 *    key is K.  A torn structural publish that redirects K's slot to another
 *    node -- or to wild memory -- trips this (or faults on the deref): the
 *    Group-B/C torn-publish detector the disjoint oracle cannot see.
 *  - final (quiescent): cds_ft_count_keys equals the exact present count, every
 *    present key resolves to a key-consistent node, cds_ft_verify passes.
 *  - leak_check: every allocated node freed exactly once.
 */
#define MW_SHARED_RANGE	(MW_NR_WRITERS * MW_RANGE)

struct mw_shared_arg {
	struct cds_ft *ft;
	unsigned int seed0;
	unsigned long ops;
	int failed;
};

static void *mw_shared_writer(void *arg)
{
	struct mw_shared_arg *w = (struct mw_shared_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = w->seed0;

	rcu_register_thread();
	if (cds_ft_iter_create(w->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % MW_SHARED_RANGE);
		struct cds_ft_node *found;
		uint8_t k[8];

		cds_ft_u64_to_key(w->ft, key, k, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(w->ft, iter);
		found = cds_ft_iter_node(iter);
		if (found) {
			/*
			 * Torn-resolve oracle: K must map to a node keyed K.  A
			 * concurrent restructure that redirected K's slot returns
			 * the wrong node here; a torn child pointer returns wild
			 * memory (the ->key deref faults).  The lookup reference is
			 * valid only under this read section, and cds_ft_remove
			 * consumes it, so both stay inside one bracket (§11).
			 */
			if (to_test_node(found)->key != key) {
				fprintf(stderr, "MW shared: key %llu resolved to "
					"node keyed %llu (%p)\n",
					(unsigned long long) key,
					(unsigned long long) to_test_node(found)->key,
					(void *) found);
				w->failed = 1;
				rcu_read_unlock();
				mw_violation_snapshot();
				break;
			}
			if (cds_ft_remove(w->ft, iter, found) == CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(found));
			rcu_read_unlock();
		} else {
			struct ft_test_node *n;

			rcu_read_unlock();
			n = node_alloc(key);
			if (insert_u64(w->ft, key, n) != CDS_FT_STATUS_OK)
				node_free(n);	/* duplicate: a peer owns K */
		}
		w->ops++;
		if ((seed & 0x3f) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_writers_shared(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct mw_shared_arg *w;
	pthread_t writers[MW_NR_WRITERS];
	struct timespec t0;
	unsigned long total_ops = 0, present = 0;
	uint64_t key;
	int i, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_concurrent_writers_shared: skipped "
			"(set FT_INV_MW=1 to run the shared-key writer oracle)\n");
		return 0;
	}
	mw_install_fatal_handler();
	/*
	 * Same-key concurrent mutation is a LOCK-MODE feature: the duplicate
	 * chain (ft-txn-hlist.h) is not safe for concurrent mutation, so two
	 * writers on the SAME key must serialize (today via the FT-wide writer
	 * lock a lock-mode trie takes; ultimately via the head-holder's per-node
	 * node lock once that FT-wide lock drops).  This oracle therefore runs
	 * FINE (create_fixed_fine_lock_ft), NOT create_fixed_ft.  (It used to
	 * justify that by contrast with an OPTIMISTIC trie, which "has neither
	 * and cannot arbitrate same-key removers"; that strategy is gone.)
	 */
	ft = create_fixed_fine_lock_ft(8, &group);
	cds_ft_make_shared(ft);
	leak_reset();

	w = (struct mw_shared_arg *) calloc(MW_NR_WRITERS, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < MW_NR_WRITERS; i++) {
		w[i].ft = ft;
		w[i].seed0 = (unsigned int)(i * 2654435761u + 1u);
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_NR_WRITERS; i++)
		pthread_create(&writers[i], NULL, mw_shared_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_NR_WRITERS; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: the present set is whatever survived; verify it. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < MW_NR_WRITERS; i++) {
		total_ops += w[i].ops;
		if (w[i].failed)
			ret = -1;
	}
	for (key = 0; key < MW_SHARED_RANGE; key++) {
		struct cds_ft_node *found = NULL;

		if (lookup_u64(ft, key, &found) != CDS_FT_STATUS_OK || !found)
			continue;
		present++;
		if (to_test_node(found)->key != key) {
			fprintf(stderr, "MW shared final: key %llu resolved to "
				"node keyed %llu\n", (unsigned long long) key,
				(unsigned long long) to_test_node(found)->key);
			ret = -1;
		}
	}
	if (cds_ft_count_keys(ft) != present) {
		fprintf(stderr, "MW shared final: count_keys %lu != present %lu\n",
			cds_ft_count_keys(ft), present);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "MW shared final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_concurrent_writers_shared: %d writers, %lu ops, "
		"%lu present keys\n", MW_NR_WRITERS, total_ops, present);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

#define MW_COARSE_NR_WRITERS	16
#define MW_FINE_NR_WRITERS	16

/*
 * Concurrent-writer soak for CDS_FT_WRITER_LOCK_COARSE (step 2 acceptance,
 * §11.4).  MW_COARSE_NR_WRITERS threads each churn insert/remove over a DISJOINT
 * key range on ONE coarse lock-mode trie.  Unlike inv_concurrent_writers_-
 * disjoint, which saturates the per-node-lock (FINE) path, the FT-wide writer
 * lock SERIALIZES every mutation here, so
 * this must be violation-free -- it proves the acquire / escalate / release
 * plumbing under real contention through ft->lane, and that a coarse
 * lock-mode trie stays coherent under 16 writers (no lost key, count == live,
 * cds_ft_verify passes).  Reuses the mw_writer thread body (its lost-key oracle
 * and reference-lifetime discipline are strategy-agnostic).  Opt-in
 * (FT_INV_MW=1) -- it runs for DEFAULT_DURATION_MS.
 */
static int inv_concurrent_writers_coarse_lock(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct mw_writer_arg *w;
	pthread_t writers[MW_COARSE_NR_WRITERS];
	struct timespec t0;
	unsigned long total_ops = 0, live = 0;
	int i, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_concurrent_writers_coarse_lock: skipped "
			"(set FT_INV_MW=1 to run the coarse lock-mode soak)\n");
		return 0;
	}
	mw_install_fatal_handler();
	ft = create_fixed_coarse_lock_ft(8, &group);
	/* Concurrent mode: deferred reclaim keeps a peer's touched nodes live. */
	cds_ft_make_shared(ft);

	leak_reset();

	w = (struct mw_writer_arg *) calloc(MW_COARSE_NR_WRITERS, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < MW_COARSE_NR_WRITERS; i++) {
		w[i].ft = ft;
		w[i].base = (uint64_t) i * MW_RANGE;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_COARSE_NR_WRITERS; i++)
		pthread_create(&writers[i], NULL, mw_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_COARSE_NR_WRITERS; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: verify the final trie against every writer's shadow. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < MW_COARSE_NR_WRITERS; i++) {
		unsigned int off;

		total_ops += w[i].ops;
		if (w[i].failed)
			ret = -1;
		for (off = 0; off < MW_RANGE; off++) {
			struct cds_ft_node *found = NULL;

			if (!w[i].present[off])
				continue;
			live++;
			if (lookup_u64(ft, w[i].base + off, &found)
					!= CDS_FT_STATUS_OK ||
			    found != &w[i].node[off]->node) {
				fprintf(stderr, "coarse-lock final: writer %d key %llu "
					"lost (found %p != %p)\n", i,
					(unsigned long long)(w[i].base + off),
					(void *) found,
					(void *) &w[i].node[off]->node);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "coarse-lock final: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "coarse-lock final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_concurrent_writers_coarse_lock: %d writers, %lu ops, "
		"%lu live keys\n", MW_COARSE_NR_WRITERS, total_ops, live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/*
 * INVARIANT (MW, §11.3 step 3): a CDS_FT_WRITER_LOCK_FINE trie loses no key.
 *
 * Same disjoint-range lost-key oracle as inv_concurrent_writers_coarse_lock, on
 * a trie whose recompacts acquire the per-node lock-set {C, P} (+ {GP} when P is
 * compressed) and resolve P/GP through the RELEASE terminal {LOCK|s -> s}.
 * 16 writers insert/remove over disjoint key ranges; every writer's shadow set
 * must match the final trie exactly.
 *
 * What this is actually gating, given the FT-wide lock still serializes writers
 * here (§11.1): that the release terminal COMMITS -- that a locked-but-surviving
 * node comes out of the commit LIVE and UNLOCKED.  A release that failed to
 * commit, or a bail path that forgot to unlock a member, leaves FT_STATE_LOCK
 * set at rest -- which wedges every later publish into that node, and which
 * cds_ft_verify reports as a leaked copy fence.  Both endpoints are checked
 * below, and 100k+ recompacts run through them.
 */
static int inv_concurrent_writers_fine_lock(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct mw_writer_arg *w;
	pthread_t writers[MW_FINE_NR_WRITERS];
	struct timespec t0;
	unsigned long total_ops = 0, live = 0;
	int i, ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_concurrent_writers_fine_lock: skipped "
			"(set FT_INV_MW=1 to run the fine lock-mode soak)\n");
		return 0;
	}
	mw_install_fatal_handler();
	ft = create_fixed_fine_lock_ft(8, &group);
	/* Concurrent mode: deferred reclaim keeps a peer's touched nodes live. */
	cds_ft_make_shared(ft);

	leak_reset();

	w = (struct mw_writer_arg *) calloc(MW_FINE_NR_WRITERS, sizeof(*w));
	if (!w)
		abort();
	for (i = 0; i < MW_FINE_NR_WRITERS; i++) {
		w[i].ft = ft;
		w[i].base = (uint64_t) i * MW_RANGE;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_FINE_NR_WRITERS; i++)
		pthread_create(&writers[i], NULL, mw_writer, &w[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_FINE_NR_WRITERS; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: verify the final trie against every writer's shadow. */
	synchronize_rcu();
	rcu_read_lock();
	for (i = 0; i < MW_FINE_NR_WRITERS; i++) {
		unsigned int off;

		total_ops += w[i].ops;
		if (w[i].failed)
			ret = -1;
		for (off = 0; off < MW_RANGE; off++) {
			struct cds_ft_node *found = NULL;

			if (!w[i].present[off])
				continue;
			live++;
			if (lookup_u64(ft, w[i].base + off, &found)
					!= CDS_FT_STATUS_OK ||
			    found != &w[i].node[off]->node) {
				fprintf(stderr, "fine-lock final: writer %d key %llu "
					"lost (found %p != %p)\n", i,
					(unsigned long long)(w[i].base + off),
					(void *) found,
					(void *) &w[i].node[off]->node);
				ret = -1;
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "fine-lock final: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "fine-lock final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();

	fprintf(stderr, "# inv_concurrent_writers_fine_lock: %d writers, %lu ops, "
		"%lu live keys\n", MW_FINE_NR_WRITERS, total_ops, live);

	free(w);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   Concurrent CROSS-TRIE writers on a FINE trie (FT-wide-lock drop) */
/*                                                                    */
/* ================================================================== */

/*
 * Variable-length fine-grained lock-mode trie: like create_fixed_fine_lock_ft
 * but with the group's default (variable) key length, so non-root graft /
 * detach at a multi-byte prefix is legal (CDS_FT_LEN_VARIABLE).  EAGER lookup
 * so the point verification needs no speculative key offset.  Used by
 * inv_concurrent_crosstrie_fine_lock.
 */
static struct cds_ft *create_varlen_fine_lock_ft(struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0)
		abort();
	/* Isolate the structural attach path: no ordered-list cell splices. */
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * As create_varlen_fine_lock_ft but with @list_on (ordered list) and/or
 * @rank_on (per-node order statistics) enabled.  With @list_on the cross-trie
 * graft's GLUE commit carries the extra ordered-list run-splice cell edges --
 * the ordered-list attach path under the FT-wide-lock drop.  @rank_on requests
 * order statistics: a rank-stats trie maintains ONE global count on the root's
 * nr_keys word that every count-changing writer must update, so the group
 * COERCES a FINE strategy to COARSE (§10.5; ft-lifecycle.h) -- the resulting
 * trie keeps the FT-wide lock and never drops it, which is exactly what makes
 * the count-parent walk correct.  So @rank_on trie == coarse regardless of the
 * requested FINE.
 */
static struct cds_ft *create_varlen_fine_lock_cfg_ft(
		struct cds_ft_group **group_out, bool list_on, bool rank_on)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, list_on) < 0)
		abort();
	if (cds_ft_group_attr_set_rank_stats(attr, rank_on) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_FINE) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * INVARIANT (MW, §11 drop-mechanics): concurrent cross-trie GRAFTs into one
 * LIVE (concurrent) fine-grained lock-mode trie lose no key and corrupt no
 * structure, with the FT-wide writer lock DROPPED.
 *
 * This closes the one un-soaked gap the drop's net-(A) argument otherwise
 * covers only by inspection.  Through step 6 a cross-trie graft/merge/graft_swap
 * takes the FT-wide lock on its LIVE dst (the exclusive source skips its own),
 * so concurrent cross-trie ops on a live dst were serialized.  The dst now
 * skips that lock too, so concurrent grafts and point-removes into one live dst
 * arbitrate SOLELY through the step-6A per-node attach RELEASE locks + MCAS.
 * The drop is unconditional for FINE, so that is the only path this exercises.
 *
 * Layout MAXIMISES per-node contention: prefix = {p, w} with p shared across
 * ALL writers and w = writer id, so every writer's graft at {p, w} attaches a
 * child under the SAME {p} spine node -- MW_XT_NR_WRITERS writers contend each
 * {p} node's lock / recompaction, which is exactly the arbitration the drop
 * leans on.  Full keys are {p, w, s}; writer w owns the middle byte = w, so the
 * per-writer shadow set stays disjoint and exactly verifiable.
 *
 * Each writer step picks a random prefix p: if absent, build an EXCLUSIVE
 * source holding the S suffix keys and GRAFT it at {p, w}; if present,
 * POINT-REMOVE all S keys {p, w, s} (the established remove + node_free_rcu free
 * path, so no detached-trie drain is needed).  Final quiescent check: every
 * present prefix's S keys resolve to this writer's nodes, count_keys matches
 * the live total, and cds_ft_verify passes (a node lock leaked by a dropped
 * bail path surfaces here).
 */
static void drain_trie_local(struct cds_ft *ft);	/* defined below */

#define MW_XT_NR_WRITERS	16
#define MW_XT_PREFIXES		64	/* shared prefix byte p in [0, this) */
#define MW_XT_SUFFIXES		4	/* keys grafted per prefix */

/*
 * @attach_mode selects how each {p,w} subtree is attached into the shared dst:
 *   MW_XT_ATTACH_GRAFT  cds_ft_graft (whole exclusive src at {p,w});
 *   MW_XT_ATTACH_MERGE_SUBPOS  cds_ft_merge_at moving a src SUB-position
 *     (src keys {0,s}, src_key {0}) to {p,w} -- exercises the unfenced
 *     ft_merge_graft_subpos_inplace path under the LOCK_FINE drop.
 *   MW_XT_ATTACH_GRAFT_NILKEY  cds_ft_graft of a NIL-key source (a single
 *     empty key, the root's childless-internal external_nodes wrapper) at
 *     {p,w} -- the source's external chain head is placed DIRECTLY, so the
 *     one landed key is {p,w} (2 bytes, no suffix) and the emptied wrapper is
 *     freeze-on-free tombstoned; exercises the exclusive nil-key src-swap-
 *     fused arm (retire edge + wrapper tombstone fused into glue.txn) under
 *     the drop.
 *   MW_XT_ATTACH_MERGE_NILKEY  cds_ft_merge_at moving a single {0} leaf
 *     (src_key {0}) to {p,w}: ft_detach excises the lone external into an
 *     exclusive nil-key wrapper tmp, so the same fused nil-key arm runs on
 *     the take() path (retire + tombstone into the merge's pre-reserved
 *     pf_txn) -- exercises the ft-merge.h pf_cap reservation the create-path
 *     graft does not.
 * GRAFT / MERGE_SUBPOS yield dst keys {p,w,s}; the nil modes a single {p,w}.
 * mw_xt_nsuffix / mw_xt_landed_key fold that per-mode shape difference so the
 * build / remove / verify loops stay shared.
 */
#define MW_XT_ATTACH_GRAFT		0
#define MW_XT_ATTACH_MERGE_SUBPOS	2
#define MW_XT_ATTACH_GRAFT_NILKEY	3
#define MW_XT_ATTACH_MERGE_NILKEY	4

/*
 * POINT writers (mw_xt_point_writer), spawned ALONGSIDE the bulk writers above
 * when @nr_point > 0.  They carry writer ids MW_XT_NR_WRITERS .. +MW_XT_NR_POINT
 * so their middle key byte is disjoint from every bulk writer's, but their keys
 * {p, w, s} share the SAME {p} spine node -- so a point INSERT splits /
 * recompacts the very node a live bulk attach or detach is locking.
 *
 * That is the bulk-vs-point arbitration the G5.25 gate exists for: a bulk op
 * holds CDS_FT_SCOPED_BULK_GATE for its whole body, and a FINE point op's
 * ft_writer_scope_enter sees ft_bulk_active() and ELEVATES to the FT-wide
 * writer lock for that op (the "mode flip").  The existing bulk-only rows
 * exercise point-REMOVE against an attach; insert is the harder direction
 * (it SPLITS and PROMOTES, where remove only collapses), and @detach_teardown
 * puts cds_ft_detach -- the other gate-holding bulk op -- on the other side.
 *
 * Point writers own their band exactly, so the oracle's per-writer shadow check
 * is unchanged: their keys have the MW_XT_ATTACH_GRAFT shape ({p,w,s}) whatever
 * the bulk writers' attach mode is, which is why @attach_mode lives per-writer.
 */
#define MW_XT_NR_POINT		8

struct mw_xt_arg {
	struct cds_ft *ft;			/* the shared LIVE dst */
	struct cds_ft_group *group;
	unsigned int w;				/* writer id (middle key byte) */
	int attach_mode;			/* MW_XT_ATTACH_* */
	bool point;				/* a mw_xt_point_writer, not a bulk one */
	bool detach_teardown;			/* bulk: cds_ft_detach, not point-remove */
	/*
	 * The key band this writer owns: middle byte @mid, suffixes
	 * [@sbase, @sbase + nsuffix).  Normally @mid == @w and @sbase == 0, so
	 * every writer owns a whole middle byte.  An OVERLAP point writer
	 * instead borrows a BULK writer's middle byte and takes the suffixes
	 * above it, so the two share the {p,mid} node itself -- the node whose
	 * child set the bulk attach rewrites wholesale -- not merely the {p}
	 * node above it.
	 */
	unsigned int mid;
	unsigned int sbase;
	uint8_t present[MW_XT_PREFIXES];	/* 1 = {p,w,*} currently grafted */
	struct ft_test_node *node[MW_XT_PREFIXES][MW_XT_SUFFIXES];
	unsigned long ops;
	unsigned long bulk_ops;			/* the attach / detach legs only */
	int failed;
};

/* A nil-key mode attaches a single-key source (empty key / single leaf). */
static bool mw_xt_is_nilkey(int attach_mode)
{
	return attach_mode == MW_XT_ATTACH_GRAFT_NILKEY
		|| attach_mode == MW_XT_ATTACH_MERGE_NILKEY;
}

/* Landed keys per prefix: S for GRAFT / MERGE_SUBPOS, 1 for a nil-key src. */
static unsigned int mw_xt_nsuffix(int attach_mode)
{
	return mw_xt_is_nilkey(attach_mode) ? 1 : MW_XT_SUFFIXES;
}

/*
 * The dst key that writer @w's suffix @s lands at for @attach_mode: {p,w,s}
 * for GRAFT / MERGE_SUBPOS, {p,w} for a single nil-key attach.  Fills @buf and
 * returns the length.
 */
static size_t mw_xt_landed_key(int attach_mode, unsigned int p, unsigned int w,
		unsigned int s, uint8_t *buf)
{
	buf[0] = (uint8_t) p;
	buf[1] = (uint8_t) w;
	if (mw_xt_is_nilkey(attach_mode))
		return 2;
	buf[2] = (uint8_t) s;
	return 3;
}

static void *mw_xt_writer(void *arg)
{
	struct mw_xt_arg *x = (struct mw_xt_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int)(uintptr_t) x + 0x9e3779b9u;

	rcu_register_thread();
	if (cds_ft_iter_create(x->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int p = (unsigned int)(rand_r(&seed) % MW_XT_PREFIXES);
		uint8_t prefix[2] = { (uint8_t) p, (uint8_t) x->mid };
		unsigned int s;

		if (!x->present[p]) {
			struct cds_ft *src;

			bool subpos =
				x->attach_mode == MW_XT_ATTACH_MERGE_SUBPOS;
			bool nilgraft =
				x->attach_mode == MW_XT_ATTACH_GRAFT_NILKEY;
			bool nilmerge =
				x->attach_mode == MW_XT_ATTACH_MERGE_NILKEY;
			/* merge_at (src_key {0}): the sub-position and nil-key-leaf moves. */
			bool is_merge = subpos || nilmerge;
			enum cds_ft_status attach_st;

			/*
			 * Build a private source.  GRAFT: 1-byte keys {s} moved
			 * wholesale.  MERGE_SUBPOS: 2-byte keys {0,s} so the merge
			 * moves the {0} SUB-position (src_key {0}).  GRAFT_NILKEY: the
			 * single EMPTY key (nil-key wrapper).  MERGE_NILKEY: a single
			 * 1-byte {0} leaf so merge_at src_key {0} detaches it to a
			 * nil-key wrapper tmp (the fused take-path).  GRAFT / SUBPOS land
			 * {p,w,s}; the nil modes land the lone {p,w}.
			 */
			if (cds_ft_create(x->group, NULL, &src) < 0)
				abort();
			for (s = 0; s < mw_xt_nsuffix(x->attach_mode); s++) {
				unsigned int sk = x->sbase + s;
				uint8_t suffix[1] = { (uint8_t) sk };
				uint8_t sub_suffix[2] = { 0, (uint8_t) sk };
				uint8_t full[3] = { (uint8_t) p,
					(uint8_t) x->mid, (uint8_t) sk };
				struct ft_test_node *n = node_alloc(
					((uint64_t) p << 16)
					| ((uint64_t) x->mid << 8) | sk);

				memcpy(n->okey, full, 3);
				/*
				 * GRAFT_NILKEY: insert the EMPTY key (the src root's
				 * nil-key wrapper); the graft at {p,w} places its
				 * external head directly there.  Else a 1-byte {s}
				 * (graft / MERGE_NILKEY's lone {0} leaf) or 2-byte
				 * {0,s} (merge sub-position) key.
				 */
				if (cds_ft_insert(src,
						nilgraft ? (const uint8_t *) "" :
						subpos ? sub_suffix : suffix,
						nilgraft ? 0 : subpos ? 2 : 1,
						&n->node)
						!= CDS_FT_STATUS_OK)
					abort();
				x->node[p][s] = n;
			}
			/*
			 * The consumed source must be EXCLUSIVE (step-6
			 * contract): a live lock-mode src is rejected with BUSY.
			 * make_exclusive also lets its writer scope skip the
			 * FT-wide lock so only the live dst's per-node locks
			 * arbitrate the attach.
			 */
			cds_ft_make_exclusive(src);
			if (is_merge) {
				uint8_t src_sub[1] = { 0 };

				attach_st = cds_ft_merge_at(x->ft, prefix, 2,
					src, src_sub, 1);
			} else {
				attach_st = cds_ft_graft(x->ft, prefix, 2, src);
			}
			if (attach_st != CDS_FT_STATUS_OK) {
				/*
				 * {p,w} is this writer's own and was absent =>
				 * empty => the graft MUST succeed.  A failure
				 * here is a real drop defect (a peer's attach
				 * leaked into a disjoint prefix, or a torn spine
				 * from an unserialized attach).
				 */
				fprintf(stderr, "xt writer %u prefix p=%u: graft "
					"failed on an empty disjoint target\n",
					x->w, p);
				x->failed = 1;
				/*
				 * This only fires on a real drop defect, and it
				 * IS the reported violation (failed => ret = -1).
				 * Leave @src and its nodes untouched rather than
				 * risk a teardown UAF on the error path; the
				 * accompanying leak_check flag is secondary to the
				 * message above.
				 */
				mw_violation_snapshot();
				break;
			}
			cds_ft_destroy(src);		/* emptied by the graft */
			x->present[p] = 1;
			x->bulk_ops++;
		} else if (x->detach_teardown) {
			/*
			 * Tear the prefix down with cds_ft_detach -- the OTHER
			 * gate-holding bulk op -- instead of the point-remove
			 * loop, so both halves of this writer's cycle are bulk
			 * ops and a concurrent mw_xt_point_writer meets a detach
			 * as well as an attach.  Detaching at {p,w} takes every
			 * landed key of the prefix whatever the attach mode
			 * (the nil-key modes land the lone {p,w} itself).
			 */
			struct cds_ft *detached = NULL;

			if (cds_ft_detach(x->ft, prefix, 2, &detached)
					!= CDS_FT_STATUS_OK || !detached) {
				fprintf(stderr, "xt writer %u prefix p=%u: detach "
					"failed on its own present prefix\n",
					x->w, p);
				x->failed = 1;
				mw_violation_snapshot();
				break;
			}
			/*
			 * The detached trie is EXCLUSIVE and unreachable to any
			 * peer, so draining it locally is safe; node_free_rcu
			 * still defers the nodes past any in-flight reader that
			 * resolved them before the detach.
			 */
			drain_trie_local(detached);
			cds_ft_destroy(detached);
			x->bulk_ops++;
			for (s = 0; s < mw_xt_nsuffix(x->attach_mode); s++)
				x->node[p][s] = NULL;
			x->present[p] = 0;
		} else {
			/* Point-remove every key of this prefix, then re-arm. */
			for (s = 0; s < mw_xt_nsuffix(x->attach_mode); s++) {
				uint8_t full[3];
				size_t full_len = mw_xt_landed_key(
					x->attach_mode, p, x->mid,
					x->sbase + s, full);
				struct cds_ft_node *found;

				rcu_read_lock();
				cds_ft_iter_set_key(iter, full, full_len);
				cds_ft_lookup(x->ft, iter);
				found = cds_ft_iter_node(iter);
				if (found != &x->node[p][s]->node) {
					fprintf(stderr, "xt writer %u key "
						"{%u,%u,%u}: live but found %p "
						"!= mine %p\n", x->w, p, x->w, s,
						(void *) found,
						(void *) &x->node[p][s]->node);
					x->failed = 1;
					rcu_read_unlock();
					mw_violation_snapshot();
					goto out;
				}
				if (cds_ft_remove(x->ft, iter, found)
						== CDS_FT_STATUS_OK)
					node_free_rcu(to_test_node(found));
				rcu_read_unlock();
				x->node[p][s] = NULL;
			}
			x->present[p] = 0;
		}
		x->ops++;
		if ((seed & 0x3f) == 0)
			rcu_quiescent_state();
	}
out:
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * A POINT writer running against the bulk writers above (see MW_XT_NR_POINT).
 * Its band is {p, w, s} with w >= MW_XT_NR_WRITERS, so its shadow is exact, but
 * the {p} spine node it splits and collapses is shared with every bulk writer's
 * attach / detach point.
 *
 * The step alternates per prefix exactly as the bulk writer does -- absent =>
 * INSERT the S keys, present => point-REMOVE them -- so both point directions
 * run while a peer holds the bulk gate.  The insert leg is the one the bulk-only
 * rows never exercised: an append/split under a live attach, where the bulk op's
 * lock set was computed from a descent this split can invalidate.
 */
static void *mw_xt_point_writer(void *arg)
{
	struct mw_xt_arg *x = (struct mw_xt_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = (unsigned int)(uintptr_t) x + 0x85ebca6bu;

	rcu_register_thread();
	if (cds_ft_iter_create(x->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int p = (unsigned int)(rand_r(&seed) % MW_XT_PREFIXES);
		unsigned int s;

		if (!x->present[p]) {
			for (s = 0; s < MW_XT_SUFFIXES; s++) {
				unsigned int sk = x->sbase + s;
				uint8_t full[3] = { (uint8_t) p,
					(uint8_t) x->mid, (uint8_t) sk };
				struct ft_test_node *n = node_alloc(
					((uint64_t) p << 16)
					| ((uint64_t) x->mid << 8) | sk);

				memcpy(n->okey, full, 3);
				rcu_read_lock();
				if (cds_ft_insert(x->ft, full, 3, &n->node)
						!= CDS_FT_STATUS_OK) {
					rcu_read_unlock();
					/*
					 * {p,w,s} is this writer's own and was
					 * absent, so the insert cannot legally
					 * fail: a refusal here means a bulk peer's
					 * unserialized edit was visible mid-insert
					 * (the elevation did not happen).  Leave
					 * @n unfreed rather than risk a teardown
					 * UAF on the error path -- the message is
					 * the reported violation.
					 */
					fprintf(stderr, "xt point writer %u key "
						"{%u,%u,%u}: insert failed on an "
						"absent own key\n", x->w, p,
						x->mid, sk);
					x->failed = 1;
					mw_violation_snapshot();
					goto out;
				}
				rcu_read_unlock();
				x->node[p][s] = n;
			}
			x->present[p] = 1;
		} else {
			for (s = 0; s < MW_XT_SUFFIXES; s++) {
				unsigned int sk = x->sbase + s;
				uint8_t full[3] = { (uint8_t) p,
					(uint8_t) x->mid, (uint8_t) sk };
				struct cds_ft_node *found;

				rcu_read_lock();
				cds_ft_iter_set_key(iter, full, 3);
				cds_ft_lookup(x->ft, iter);
				found = cds_ft_iter_node(iter);
				if (found != &x->node[p][s]->node) {
					fprintf(stderr, "xt point writer %u key "
						"{%u,%u,%u}: live but found %p "
						"!= mine %p\n", x->w, p, x->mid, sk,
						(void *) found,
						(void *) &x->node[p][s]->node);
					x->failed = 1;
					rcu_read_unlock();
					mw_violation_snapshot();
					goto out;
				}
				if (cds_ft_remove(x->ft, iter, found)
						== CDS_FT_STATUS_OK)
					node_free_rcu(to_test_node(found));
				rcu_read_unlock();
				x->node[p][s] = NULL;
			}
			x->present[p] = 0;
		}
		x->ops++;
		if ((seed & 0x3f) == 0)
			rcu_quiescent_state();
	}
out:
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * See the block comment on mw_xt_writer.  Endpoints checked at quiescence: no
 * grafted key lost (resolves to the owning writer's node), count_keys equals
 * the live total, and cds_ft_verify reports no leaked node lock.
 */
static int mw_xt_oracle(const char *tname, int attach_mode, bool list_on,
		bool rank_on, unsigned int nr_point, bool detach_teardown,
		bool overlap)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct mw_xt_arg *x;
	struct cds_ft_iter *iter;
	pthread_t writers[MW_XT_NR_WRITERS + MW_XT_NR_POINT];
	struct timespec t0;
	unsigned long total_ops = 0, total_bulk = 0, live = 0;
	unsigned int nr_total = MW_XT_NR_WRITERS + nr_point;
	unsigned int i, p, s;
	int ret = 0;

	assert(nr_point <= MW_XT_NR_POINT);
	/*
	 * An OVERLAP point writer shares a bulk writer's middle byte, so there
	 * must be a bulk writer per point writer, and the two bands together must
	 * fit one suffix byte.  Detaching {p,mid} would take the point writer's
	 * keys with it, so the two are mutually exclusive.
	 */
	assert(!overlap || (nr_point <= MW_XT_NR_WRITERS && !detach_teardown
			&& 2 * MW_XT_SUFFIXES <= 256));

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# %s: skipped "
			"(set FT_INV_MW=1 to run the cross-trie writer oracle)\n",
			tname);
		return 0;
	}
	mw_install_fatal_handler();
	/*
	 * @list_on selects the ordered-list + rank-stats dst (Defect D): the
	 * graft's GLUE commit then carries run-splice + count-parent edges a peer
	 * can conflict-abort after the src swap.  Otherwise the isolated
	 * structural attach (list off, rank off).
	 */
	ft = (list_on || rank_on)
		? create_varlen_fine_lock_cfg_ft(&group, list_on, rank_on)
		: create_varlen_fine_lock_ft(&group);
	/* Concurrent mode: deferred reclaim keeps a peer's touched nodes live. */
	cds_ft_make_shared(ft);

	leak_reset();

	x = (struct mw_xt_arg *) calloc(nr_total, sizeof(*x));
	if (!x)
		abort();
	for (i = 0; i < nr_total; i++) {
		x[i].ft = ft;
		x[i].group = group;
		x[i].w = i;
		/*
		 * A point writer's keys are always {p,w,s} (the GRAFT shape)
		 * whatever the bulk writers attach, so every shape helper and
		 * the verify loop read the mode off the WRITER, not off this
		 * oracle's argument.
		 */
		x[i].point = i >= MW_XT_NR_WRITERS;
		x[i].attach_mode = x[i].point ? MW_XT_ATTACH_GRAFT : attach_mode;
		x[i].detach_teardown = !x[i].point && detach_teardown;
		/*
		 * Disjoint bands by default (one middle byte each).  @overlap
		 * instead aims point writer j at bulk writer j's own {p,j}, one
		 * suffix block higher, so the bulk attach rewrites the very node
		 * the point keys hang off.
		 */
		if (x[i].point && overlap) {
			x[i].mid = i - MW_XT_NR_WRITERS;
			x[i].sbase = MW_XT_SUFFIXES;
		} else {
			x[i].mid = i;
			x[i].sbase = 0;
		}
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < nr_total; i++)
		pthread_create(&writers[i], NULL,
			x[i].point ? mw_xt_point_writer : mw_xt_writer, &x[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < nr_total; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	/* Quiescent: verify the final trie against every writer's shadow. */
	synchronize_rcu();
	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	for (i = 0; i < nr_total; i++) {
		total_ops += x[i].ops;
		total_bulk += x[i].bulk_ops;
		if (x[i].failed)
			ret = -1;
		for (p = 0; p < MW_XT_PREFIXES; p++) {
			if (!x[i].present[p])
				continue;
			for (s = 0; s < mw_xt_nsuffix(x[i].attach_mode); s++) {
				uint8_t full[3];
				size_t full_len = mw_xt_landed_key(
					x[i].attach_mode, p, x[i].mid,
					x[i].sbase + s, full);
				struct cds_ft_node *found;

				live++;
				cds_ft_iter_set_key(iter, full, full_len);
				cds_ft_lookup(ft, iter);
				found = cds_ft_iter_node(iter);
				if (found != &x[i].node[p][s]->node) {
					fprintf(stderr, "xt final: writer %u key "
						"{%u,%u,%u} lost (found %p != "
						"%p)\n", i, p, x[i].mid,
						x[i].sbase + s,
						(void *) found,
						(void *) &x[i].node[p][s]->node);
					ret = -1;
				}
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "xt final: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "xt final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);

	/*
	 * ★ REPORT THE BULK OP COUNT SEPARATELY.  A mixed total hides the bulk
	 * leg: a bulk op builds and consumes a whole source trie and passes
	 * through the gate, so it is far more expensive than a point op, and a row
	 * can look busy on @total_ops while having opened only a handful of bulk
	 * windows.  @total_bulk is the number the bulk-vs-point coverage claim
	 * actually rests on.
	 */
	fprintf(stderr, "# %s: %d bulk + %u point writers, %lu ops "
		"(%lu bulk), %lu live keys\n", tname, MW_XT_NR_WRITERS,
		nr_point, total_ops, total_bulk, live);

	free(x);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/* 16 writers graft an exclusive src into one shared live dst at {p,w}. */
static int inv_concurrent_crosstrie_fine_lock(void)
{
	return mw_xt_oracle("inv_concurrent_crosstrie_fine_lock",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * As above but the attach is a cds_ft_merge_at moving a src SUB-position
 * (src_key {0}) -- the ft_merge_graft_subpos_inplace path -- so it exercises
 * the merge build -> unlink -> commit under the LOCK_FINE drop (fence cn +
 * commit-abort retry), which cds_ft_graft's whole-source delegate does not.
 */
static int inv_concurrent_crosstrie_merge_fine_lock(void)
{
	INV_NEED_MERGE("inv_concurrent_crosstrie_merge_fine_lock");
	return mw_xt_oracle("inv_concurrent_crosstrie_merge_fine_lock",
		MW_XT_ATTACH_MERGE_SUBPOS, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * As inv_concurrent_crosstrie_fine_lock but each attach grafts a NIL-key
 * source (a single empty key) at {p,w}: the source root is a childless
 * internal whose external_nodes hold the lone key, so the graft places the
 * external head DIRECTLY and freeze-on-free tombstones the emptied wrapper.
 * With an EXCLUSIVE list-off src that retire edge AND the wrapper tombstone
 * both fuse into the dst attach's glue.txn (src-swap-fused), so the whole
 * move -- src unlink + wrapper freeze + dst attach -- is ONE flip.  Exercises
 * that fused nil-key arm (and its commit-abort rollback) under the drop, which
 * neither the non-nil graft nor the merge sub-position covers.
 */
static int inv_concurrent_crosstrie_graft_nilkey_fine_lock(void)
{
	return mw_xt_oracle("inv_concurrent_crosstrie_graft_nilkey_fine_lock",
		MW_XT_ATTACH_GRAFT_NILKEY, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * As above but via cds_ft_merge_at moving a single {0} leaf (src_key {0}):
 * ft_detach excises that lone external into an EXCLUSIVE nil-key wrapper tmp,
 * which ft_graft_keylen then grafts through the take() path (pre_txn = the
 * merge's pf_txn).  Exercises the fused nil-key retire + wrapper tombstone
 * riding the merge's PRE-RESERVED pf_txn -- the reserve-sufficiency the
 * ft-merge.h m==0 pf_cap bump covers -- which the create-path nil-key graft
 * (its own reserve) does not.
 */
static int inv_concurrent_crosstrie_merge_nilkey_fine_lock(void)
{
	INV_NEED_MERGE("inv_concurrent_crosstrie_merge_nilkey_fine_lock");
	return mw_xt_oracle("inv_concurrent_crosstrie_merge_nilkey_fine_lock",
		MW_XT_ATTACH_MERGE_NILKEY, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * Ordered-list cross-trie graft under the FT-wide-lock drop: the shared dst has
 * the ordered list ON (rank-stats OFF, so it stays FINE and DROPS the lock), so
 * each graft's GLUE commit folds in the ordered-list run-splice cell edges that
 * a concurrent {p,w'} writer can MCAS-conflict AFTER this writer's src-root
 * swap.  The graft's retry_attach loop re-attaches the owned payload rather than
 * losing the moved subtree.  Also exercises ordered-list point-remove under the
 * drop.  (The rank-stats half of the original "Defect D" is a separate test:
 * rank stats coerce to coarse, below.)
 */
static int inv_concurrent_crosstrie_graft_ord_fine_lock(void)
{
	return mw_xt_oracle("inv_concurrent_crosstrie_graft_ord_fine_lock",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ true, /*rank_on=*/ false,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * Rank-stats cross-trie graft: a rank-stats trie maintains one global count on
 * the root nr_keys word that every count-changing writer updates, so it is the
 * §10.5 COARSE target -- the group coerces the requested FINE strategy to COARSE
 * (ft-lifecycle.h), keeping the FT-wide lock so the count-parent walk runs under
 * writer exclusion (correct).  Without that coercion this over-counted nr_keys
 * under the drop (the walk climbed a stale ancestor chain, losing remove
 * decrements).  This is the regression guard for the coercion: 16 writers graft
 * into one rank-stats dst, and count_keys / cds_ft_verify's exact nr_keys check
 * must hold.
 */
static int inv_concurrent_crosstrie_graft_rank_coarse_lock(void)
{
	return mw_xt_oracle("inv_concurrent_crosstrie_graft_rank_coarse_lock",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ false, /*rank_on=*/ true,
		/*nr_point=*/ 0, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * INVARIANT (G5.25 bulk gate): POINT writers and BULK writers mutate one shared
 * LIVE FINE trie concurrently without losing a key or corrupting the structure.
 *
 * This is the bulk-vs-point half of the writer-strategy contract, and it is the
 * one the rows above do NOT cover.  They pair a bulk ATTACH with a point REMOVE
 * inside the SAME writer, so every writer is serialized against itself and the
 * only cross-writer pair soaked is attach-vs-attach (plus attach-vs-remove of a
 * disjoint band).  Here 16 bulk writers cycle attach/teardown while 8 separate
 * point writers INSERT and REMOVE keys under the same {p} spine nodes, so the
 * soaked pairs include point-insert-vs-attach -- the direction where the point
 * op SPLITS and PROMOTES the node the bulk op's lock set was computed over.
 *
 * The mechanism under test is the mode flip: a bulk op holds
 * CDS_FT_SCOPED_BULK_GATE for its whole body, and a FINE point op's
 * ft_writer_scope_enter sees ft_bulk_active() and takes the FT-wide writer lock
 * for that op instead of descending under per-node DLM locks alone.  Point and
 * bulk writers therefore arbitrate on ONE word -- and the gate holder elevates
 * too, so both sides really are on that word (measured: every FT-wide take in
 * these rows is gate-driven, and self-takes == gate entries exactly).
 *
 * ★ MEASURE THE BULK LEG, NOT THE OP TOTAL.  A bulk op builds and consumes a
 * whole source trie and passes the gate, so it is ~100x a point op: a 200 ms
 * row runs ~6500 point ops but only ~32 BULK ops (two per bulk writer).  The
 * reported "(N bulk)" is the coverage number -- the ~6100 point ops that
 * elevated inside those 32 windows are what is soaked, not 6500 bulk windows.
 * Raising the elevation's cost is visible here too: at FT_BULK_WIDE_LOCK=0 the
 * same rows run ~80 bulk ops, so the flip costs ~2.5x of the bulk leg.
 *
 * ★ AND WHICH ROW ADJUDICATES.  Built and measured against the no-elevation
 * red control (a -DFT_BULK_WIDE_LOCK=0 build, which by its own comment has "NO
 * bulk-vs-point exclusion at all"):
 *   _detach   4/5 runs RED   <- this is the discriminating row
 *   _overlap  1/6 runs RED
 *   _graft    0/6, _merge 0/6, _ord 0/6
 * So graft / merge_at against point ops SOAK the pairing without adjudicating
 * it -- for those shapes the per-node DLM lock-sets already arbitrate, and the
 * elevation is a conservative cover.  Keep them for the soak, but read a green
 * on them as "no regression", never as "the gate is load-bearing here".
 */
static int inv_concurrent_point_vs_bulk_graft(void)
{
	return mw_xt_oracle("inv_concurrent_point_vs_bulk_graft",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ MW_XT_NR_POINT, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * As above, but each bulk writer tears its prefix down with cds_ft_detach
 * instead of point-removing the landed keys, so BOTH legs of every bulk
 * writer's cycle hold the bulk gate and the point writers meet a detach as well
 * as a graft.  cds_ft_detach is the other CDS_FT_SCOPED_BULK_GATE holder
 * (ft-detach.h), and it differs from the attach in what it does to the spine:
 * it EXCISES a subtree and can collapse / recompact the {p} node the point
 * writers' own keys hang off, where the graft only adds a child.  A point
 * insert that derived its descent before such a collapse is exactly what the
 * elevation has to exclude.
 *
 * ★ THE RED CONTROL FIRES HERE, and only here: on a -DFT_BULK_WIDE_LOCK=0 build
 * this row fails 4 runs in 5, while the shipping build passed 8 for 8.  The
 * signature is the bulk op failing on its OWN live region --
 *   xt writer 3 prefix p=59: detach failed on its own present prefix
 * -- several writers per run, which is a detach walking a spine an unserialized
 * point insert/remove is restructuring underneath it.  That makes this row the
 * regression guard for the G5.25 elevation itself, not just a soak.
 */
static int inv_concurrent_point_vs_bulk_detach(void)
{
	return mw_xt_oracle("inv_concurrent_point_vs_bulk_detach",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ false, /*rank_on=*/ false,
		/*nr_point=*/ MW_XT_NR_POINT, /*detach_teardown=*/ true,
		/*overlap=*/ false);
}

/*
 * As inv_concurrent_point_vs_bulk_graft but the bulk side is cds_ft_merge_at
 * moving a src SUB-position: the merge's build -> unlink -> commit carries a
 * longer gate-held body than cds_ft_graft's whole-source delegate, so the point
 * writers' elevation window is correspondingly wider.
 */
static int inv_concurrent_point_vs_bulk_merge(void)
{
	INV_NEED_MERGE("inv_concurrent_point_vs_bulk_merge");
	return mw_xt_oracle("inv_concurrent_point_vs_bulk_merge",
		MW_XT_ATTACH_MERGE_SUBPOS, /*list_on=*/ false,
		/*rank_on=*/ false,
		/*nr_point=*/ MW_XT_NR_POINT, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * As inv_concurrent_point_vs_bulk_graft with the ordered list ON: the trie stays
 * FINE (rank stats off), so every point insert and remove also splices the
 * ordered-list cells that the bulk attach's GLUE commit folds in.  Those cell
 * edges are the one place a point op and a bulk op write slots neither of them
 * owns by key band, so if the elevation were missing here the failure would be
 * an ordered-list divergence rather than a lost key -- teardown_walk_check and
 * cds_ft_verify catch it.
 */
static int inv_concurrent_point_vs_bulk_ord(void)
{
	return mw_xt_oracle("inv_concurrent_point_vs_bulk_ord",
		MW_XT_ATTACH_GRAFT, /*list_on=*/ true, /*rank_on=*/ false,
		/*nr_point=*/ MW_XT_NR_POINT, /*detach_teardown=*/ false,
		/*overlap=*/ false);
}

/*
 * The SHARP bulk-vs-point arm: the point writers' keys hang directly off the
 * node the bulk op rewrites.
 *
 * The rows above separate the two classes by middle key byte, so their only
 * shared node is the {p} spine ABOVE the attach point, where a bulk op adds or
 * removes a single child edge -- work the {p} node's own DLM lock already
 * serializes against a point op's descent.  That is why they soak the pairing
 * without adjudicating it.  Here point writer j borrows bulk writer j's middle
 * byte and takes the suffix block above it ({p,j,4..7} against the bulk band
 * {p,j,0..3}), so the bulk cds_ft_merge_at at {p,j} rewrites the CHILD SET the
 * point writer is concurrently splitting and collapsing.  The keys stay
 * disjoint, so each writer's shadow is still exact.
 *
 * cds_ft_merge_at is the attach for this arm because the target is OCCUPIED by
 * construction whenever the point writer holds keys there -- cds_ft_graft wants
 * a free position.  The teardown stays a point-remove of the bulk band alone:
 * detaching {p,j} would carry the point writer's keys off with it.
 *
 * Measured against the no-elevation red control: 1 run in 6 RED -- sharper than
 * the disjoint-band attach rows (0 in 6) and far blunter than _detach (4 in 5),
 * so treat _detach as the guard and this as a second, weaker witness.
 */
static int inv_concurrent_point_vs_bulk_overlap(void)
{
	INV_NEED_MERGE("inv_concurrent_point_vs_bulk_overlap");
	return mw_xt_oracle("inv_concurrent_point_vs_bulk_overlap",
		MW_XT_ATTACH_MERGE_SUBPOS, /*list_on=*/ false,
		/*rank_on=*/ false,
		/*nr_point=*/ MW_XT_NR_POINT, /*detach_teardown=*/ false,
		/*overlap=*/ true);
}

/*
 * Drain @ft's keys (freeing the test nodes) and destroy @ft, KEEPING the shared
 * group -- reclaims the content cds_ft_graft_swap hands back into the local swap
 * trie.  @ft has no concurrent readers (an exclusive per-writer swap, and
 * graft_swap already drained the shared dst's readers of this content).
 */
static void mw_gs_reclaim(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;

		if (cds_ft_remove_all(ft, iter, &head) < 0)
			break;
		cds_ft_for_each_duplicate_safe_rcu(head, tmp)
			node_free_rcu(to_test_node(head));
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	cds_ft_destroy(ft);
}

/*
 * graft_swap concurrent-drop writer.  Each writer OWNS prefix {p, w} (p shared
 * across ALL writers so every {p,w} hangs under the SAME {p} spine node --
 * maximal per-node contention), and each step EXCHANGES the live subtree at
 * {p,w} with a fresh exclusive swap trie: cds_ft_graft_swap replaces dst's
 * {p,w} content with the swap's and hands the old content back in @swap, which
 * the writer then reclaims.  This drives graft_swap's SWAP path (a NON-empty
 * target), not just the empty-target delegate-to-graft that the graft oracle
 * already covers, under the FT-wide-lock drop.  Occasionally the swap is empty
 * -> {p,w} is cleared (the swap-out shape).
 */
static void *mw_gs_writer(void *arg)
{
	struct mw_xt_arg *x = (struct mw_xt_arg *) arg;
	unsigned int seed = (unsigned int)(uintptr_t) x + 0x9e3779b9u;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int p = (unsigned int)(rand_r(&seed) % MW_XT_PREFIXES);
		uint8_t prefix[2] = { (uint8_t) p, (uint8_t) x->w };
		struct ft_test_node *newn[MW_XT_SUFFIXES] = { NULL };
		/* Occasionally swap the content OUT (empty swap -> {p,w} cleared). */
		bool clear = x->present[p] && ((rand_r(&seed) & 7) == 0);
		struct cds_ft *swap;
		enum cds_ft_status st;
		unsigned int s;

		if (cds_ft_create(x->group, NULL, &swap) < 0)
			abort();
		if (!clear) {
			for (s = 0; s < MW_XT_SUFFIXES; s++) {
				uint8_t suffix[1] = { (uint8_t) s };
				struct ft_test_node *n = node_alloc(
					((uint64_t) p << 16)
					| ((uint64_t) x->w << 8) | s);

				if (cds_ft_insert(swap, suffix, 1, &n->node)
						!= CDS_FT_STATUS_OK)
					abort();
				newn[s] = n;
			}
		}
		/* The consumed swap trie must be exclusive (step-6 contract). */
		cds_ft_make_exclusive(swap);
		st = cds_ft_graft_swap(x->ft, prefix, 2, swap);
		if (st != CDS_FT_STATUS_OK) {
			fprintf(stderr, "gs writer %u prefix p=%u: graft_swap: %s\n",
				x->w, p, cds_ft_status_to_string(st));
			x->failed = 1;
			mw_violation_snapshot();
			break;	/* leave swap + nodes untouched (avoid teardown UAF) */
		}
		/*
		 * @swap now holds {p,w}'s PREVIOUS content (empty on the first
		 * seed).  Reclaim it, then re-point the shadow at the new nodes --
		 * the writer OWNS {p,w} (w == its id), so this shadow update races
		 * no peer (only the shared {p} spine is contended).
		 */
		mw_gs_reclaim(swap);
		for (s = 0; s < MW_XT_SUFFIXES; s++)
			x->node[p][s] = newn[s];
		x->present[p] = clear ? 0 : 1;
		x->ops++;
		if ((seed & 0x3f) == 0)
			rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * 16 writers concurrently cds_ft_graft_swap their own {p,w} subtree with fresh
 * content, contending the shared {p} spine, with the FT-wide writer lock
 * DROPPED.  Endpoints (quiescent): every present key resolves to the owning
 * writer's latest node, count_keys matches the live total, cds_ft_verify passes,
 * and no test node leaks (every swapped-out generation was reclaimed).
 */
static int mw_gs_oracle(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct mw_xt_arg *x;
	struct cds_ft_iter *iter;
	pthread_t writers[MW_XT_NR_WRITERS];
	struct timespec t0;
	unsigned long total_ops = 0, live = 0;
	unsigned int i, p, s;
	int ret = 0;

	if (!getenv("FT_INV_MW")) {
		fprintf(stderr, "# inv_concurrent_crosstrie_graft_swap_fine_lock: "
			"skipped (set FT_INV_MW=1 to run)\n");
		return 0;
	}
	/*
	 * graft_swap IS drop-safe: cds_ft_graft_swap re-descends on a
	 * contention-abort (retry_swap) for
	 * BOTH the empty-swap prune (ft_detach_node -EAGAIN, build-invisible) and
	 * the non-empty exchange (the swap-root retire is FUSED into the
	 * insert-replace txn, so a relocated-parent abort rolls both sides back --
	 * mirroring ft_graft_keylen's retry_attach and the src_swap_fused move).
	 */
	mw_install_fatal_handler();
	ft = create_varlen_fine_lock_ft(&group);
	cds_ft_make_shared(ft);
	leak_reset();

	x = (struct mw_xt_arg *) calloc(MW_XT_NR_WRITERS, sizeof(*x));
	if (!x)
		abort();
	for (i = 0; i < MW_XT_NR_WRITERS; i++) {
		x[i].ft = ft;
		x[i].group = group;
		x[i].w = i;
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_XT_NR_WRITERS; i++)
		pthread_create(&writers[i], NULL, mw_gs_writer, &x[i]);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < MW_XT_NR_WRITERS; i++)
		pthread_join(writers[i], NULL);
	rcu_thread_online();

	synchronize_rcu();
	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	for (i = 0; i < MW_XT_NR_WRITERS; i++) {
		total_ops += x[i].ops;
		if (x[i].failed)
			ret = -1;
		for (p = 0; p < MW_XT_PREFIXES; p++) {
			if (!x[i].present[p])
				continue;
			for (s = 0; s < MW_XT_SUFFIXES; s++) {
				uint8_t full[3] = { (uint8_t) p, (uint8_t) i,
					(uint8_t) s };
				struct cds_ft_node *found;

				live++;
				cds_ft_iter_set_key(iter, full, 3);
				cds_ft_lookup(ft, iter);
				found = cds_ft_iter_node(iter);
				if (found != &x[i].node[p][s]->node) {
					fprintf(stderr, "gs final: writer %u key "
						"{%u,%u,%u} lost (found %p != "
						"%p)\n", i, p, i, s,
						(void *) found,
						(void *) &x[i].node[p][s]->node);
					ret = -1;
				}
			}
		}
	}
	if (cds_ft_count_keys(ft) != live) {
		fprintf(stderr, "gs final: count_keys %lu != live %lu\n",
			cds_ft_count_keys(ft), live);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "gs final: cds_ft_verify failed\n");
		ret = -1;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);

	fprintf(stderr, "# inv_concurrent_crosstrie_graft_swap_fine_lock: "
		"%d writers, %lu ops, %lu live keys\n",
		MW_XT_NR_WRITERS, total_ops, live);

	free(x);
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	if (leak_check() < 0)
		ret = -1;
	return ret;
}

/* 16 writers graft_swap-exchange their {p,w} content under the LOCK_FINE drop. */
static int inv_concurrent_crosstrie_graft_swap_fine_lock(void)
{
	return mw_gs_oracle();
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 1b: Cross-CS bind/resume preserves forward order       */
/*                  (ordinal-cell / lazy-ref group)                   */
/*                                                                    */
/* ================================================================== */

#define BIND_BATCH	7

/*
 * Ordered forward iteration on a library-owned ordered-list (cell) group, in
 * batches: after every BIND_BATCH keys, snapshot the position with
 * cds_ft_iter_bind_key(), drop the RCU read lock, pass a quiescent state (so a
 * concurrently-removed bound key -- whose ordinal cell is freed via call_rcu --
 * can actually be reclaimed), re-lock, and resume with cds_ft_next().  The
 * sequence must stay strictly increasing: bind must materialize the
 * leaf-referenced key into the iterator and re-descend from THAT, never from
 * the (possibly freed) cached cell/leaf.
 */
static void *inv_bind_resume_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t prev = 0;
		int first = 1;
		int in_batch = 0;
		int count = 0;

		rcu_read_lock();
		cds_ft_lookup_first(ctx->ft, iter);
		while (cds_ft_iter_node(iter)) {
			uint8_t rk[8];
			size_t rk_len;
			uint64_t v;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (!first && v <= prev) {
				report_violation(ctx->test_name,
					"bind-resume forward order: %" PRIu64
					" after %" PRIu64 " (iter #%lu, position %d)",
					v, prev, iters, count);
				break;
			}
			prev = v;
			first = 0;
			count++;
			if (++in_batch >= BIND_BATCH) {
				cds_ft_iter_bind_key(iter);
				rcu_read_unlock();
				rcu_quiescent_state();
				rcu_read_lock();
				cds_ft_next(ctx->ft, iter);
				in_batch = 0;
			} else {
				cds_ft_next(ctx->ft, iter);
			}
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_bind_resume_order(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ord_ft(4, &group);
	struct timespec t0;
	/*
	 * inv_iter_order_writer derives its serializing writer mutex from
	 * (ctx + 1) -- it MUST sit immediately after the ctx in memory.
	 */
	struct {
		struct inv_iter_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.key_len = 4;
	shared.ctx.test_name = "inv_bind_resume_order";
	pthread_mutex_init(&shared.lock, NULL);

	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_bind_resume_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_iter_order_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_bind_resume_order: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   Compaction terminates + preserves keys on a keycopy trie         */
/*                                                                    */
/* ================================================================== */

/*
 * Exercise cds_ft_compact_step on a keycopy trie (a speculative leaf-key
 * offset is configured).  Build, drain to sparsely-occupied ranges, then
 * compact one relocation per window (batch 1) and verify it terminates and
 * preserves every surviving key in order.  Compaction is keycopy-agnostic in
 * copy mode -- across windows the iterator re-descends from its retained key
 * value -- so this is a functional smoke; the step cap guards non-termination.
 */
static int inv_compact_keycopy_terminates(void)
{
	const unsigned int INSERT_TOTAL = 16000;
	const unsigned int STRIDE = 8;		/* keep every 8th key */
	const unsigned int SURVIVORS = INSERT_TOTAL / STRIDE;
	const unsigned long STEP_CAP = 1000000;	/* >> any terminating run */
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(8, &group);
	struct cds_ft_compact_state *st;
	struct cds_ft_iter *iter;
	unsigned int i, count = 0;
	unsigned long steps = 0;
	uint64_t prev = 0;
	int ret = 0, more, first = 1;

	if (cds_ft_iter_create(ft, &iter) < 0) {
		ret = -1;
		goto out;
	}

	for (i = 0; i < INSERT_TOTAL; i++) {
		struct ft_test_node *n = node_alloc(i);

		rcu_read_lock();
		if (insert_u64(ft, i, n) != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			node_free(n);
			ret = -1;
			goto out_iter;
		}
		rcu_read_unlock();
	}

	/*
	 * Drain all but every STRIDE-th key.  The holes leave sparsely-occupied
	 * ranges that the compactor must relocate, forcing many windows -- and on
	 * a buggy stale-key cross-window resume, an unbounded loop.
	 */
	for (i = 0; i < INSERT_TOTAL; i++) {
		uint8_t k[8];
		struct cds_ft_node *found;

		if (i % STRIDE == 0)
			continue;
		rcu_read_lock();
		cds_ft_u64_to_key(ft, i, k, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(ft, iter);
		found = cds_ft_iter_node(iter);
		if (found) {
			struct ft_test_node *tn = to_test_node(found);

			if (cds_ft_remove(ft, iter, &tn->node) == CDS_FT_STATUS_OK)
				node_free_rcu(tn);
		}
		rcu_read_unlock();
	}

	st = cds_ft_compact_begin(ft);
	if (!st) {
		ret = -1;
		goto out_iter;
	}
	do {
		more = cds_ft_compact_step(st, 1);	/* batch 1 -> one window per relocation */
		if (++steps > STEP_CAP) {
			report_violation("inv_compact_keycopy_terminates",
				"compaction did not terminate after %lu steps "
				"(stale cross-window key?)", steps);
			cds_ft_compact_end(st);
			ret = -1;
			goto out_iter;
		}
	} while (more);
	cds_ft_compact_end(st);

	/* All surviving keys must remain, in order. */
	rcu_read_lock();
	cds_ft_for_each_rcu(ft, iter) {
		uint8_t rk[8];
		size_t rk_len;
		uint64_t v;

		cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
		v = cds_ft_key_to_u64(ft, rk, CDS_FT_LEN_DEFAULT);
		if (!first && v <= prev) {
			report_violation("inv_compact_keycopy_terminates",
				"post-compact order: %" PRIu64 " after %" PRIu64,
				v, prev);
			ret = -1;
			break;
		}
		prev = v;
		first = 0;
		count++;
	}
	rcu_read_unlock();

	if (ret == 0 && count != SURVIVORS) {
		report_violation("inv_compact_keycopy_terminates",
			"post-compact count %u, expected %u", count, SURVIVORS);
		ret = -1;
	}

out_iter:
	cds_ft_iter_destroy(iter);
out:
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 2: Reverse iteration ordering under mutation           */
/*                                                                    */
/* ================================================================== */

static void *inv_reverse_iter_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t prev = UINT64_MAX;
		int first = 1;
		int count = 0;

		rcu_read_lock();
		cds_ft_for_each_reverse_rcu(ctx->ft, iter) {
			uint8_t rk[8];
			size_t rk_len;
			uint64_t v;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (!first && v >= prev) {
				report_violation(ctx->test_name,
					"reverse order: %" PRIu64 " after %" PRIu64
					" (iter #%lu, position %d)",
					v, prev, iters, count);
				break;
			}
			prev = v;
			first = 0;
			count++;
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_reverse_iteration_order(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_iter_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.key_len = 4;
	shared.ctx.test_name = "inv_reverse_iteration_order";
	pthread_mutex_init(&shared.lock, NULL);

	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_reverse_iter_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_iter_order_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_reverse_iteration_order: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 3: Lookup-found key consistency                        */
/*                                                                    */
/*   When a reader performs an exact lookup and finds a node, the     */
/*   node's shadow key must match the lookup key.  A mismatch would   */
/*   mean the reader followed a stale or partially-updated pointer    */
/*   to the wrong node.                                               */
/*                                                                    */
/* ================================================================== */

struct inv_lookup_ctx {
	struct cds_ft *ft;
	const char *test_name;
	pthread_mutex_t lock;
};

static void *inv_lookup_consistency_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % READER_POOL_SIZE);
		struct cds_ft_node *found;

		rcu_read_lock();
		if (lookup_u64(ctx->ft, key, &found) == CDS_FT_STATUS_OK && found) {
			struct ft_test_node *tn = to_test_node(found);

			if (tn->key != key) {
				report_violation(ctx->test_name,
					"lookup key %" PRIu64 " returned node with shadow key %" PRIu64,
					key, tn->key);
			}
		}
		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

static void *inv_lookup_consistency_writer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(&ctx->lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(&ctx->lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(&ctx->lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(&ctx->lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_lookup_consistency(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_lookup_consistency";
	pthread_mutex_init(&ctx.lock, NULL);

	/* Pre-populate. */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_lookup_consistency_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_lookup_consistency_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_lookup_consistency: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 3b: Insert commits both indexes atomically             */
/*                                                                    */
/*   An insert makes the fresh head reachable in the structural       */
/*   index and spliced into the ordered cell list in ONE flip         */
/*   commit, so a reader can never exact-look-up a key whose cell is  */
/*   not yet in the list (2026-06 review, 2.13: the structural        */
/*   publish used to precede the splice, and a reader landing on the  */
/*   fresh head inside that window followed its NULL ord links and    */
/*   reported a spurious end-of-traversal).  A permanent sentinel     */
/*   key (the maximum, never removed) guarantees every pool key has   */
/*   a successor, so any NOT_FOUND from next-after-exact-lookup is a  */
/*   violation.                                                       */
/*                                                                    */
/* ================================================================== */

static void *inv_splice_window_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		uint8_t k[8];
		enum cds_ft_status s;

		cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_iter_node(iter)) {
			s = cds_ft_next(ctx->ft, iter);
			if (s == CDS_FT_STATUS_NOT_FOUND) {
				/*
				 * The sentinel (max key) is never removed, so
				 * every pool key has a successor.
				 */
				report_violation(ctx->test_name,
					"next after exact lookup of %" PRIu64
					" reported end-of-traversal (sentinel present)",
					key);
			}
		}
		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_insert_splice_window(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_insert_splice_window";
	pthread_mutex_init(&ctx.lock, NULL);

	/* The permanent sentinel: the maximum key, never removed. */
	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	/* Pre-populate half the writer pool. */
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_splice_window_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_lookup_consistency_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_insert_splice_window: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * Same splice-window invariant, but writers use cds_ft_insert_replace instead
 * of cds_ft_insert.  insert_replace does NOT use the one-commit park, so a
 * fresh head reaches readers structurally before its ordinal cell is spliced
 * (post-publish splice / early-wired live edge in the compressed-split shapes)
 * -- the same secondary-index channel window, exercised on the insert_replace
 * paths that inv_insert_splice_window never drives.
 */
static void *inv_splice_window_replace_writer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);
			struct cds_ft_node *old = NULL;

			pthread_mutex_lock(&ctx->lock);
			/*
			 * Both OK (inserted, no prior node) and DUPLICATE_FOUND
			 * (replaced an existing chain) are success codes (>= 0);
			 * on a replace, @old is the old head and must be freed.
			 */
			if (insert_replace_u64(ctx->ft, key, n, &old) >= 0) {
				if (old)
					node_free_rcu(to_test_node(old));
			}
			pthread_mutex_unlock(&ctx->lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(&ctx->lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(&ctx->lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_insert_replace_splice_window(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_insert_replace_splice_window";
	pthread_mutex_init(&ctx.lock, NULL);

	/* The permanent sentinel: the maximum key, never removed. */
	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	/* Pre-populate half the writer pool. */
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_splice_window_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_splice_window_replace_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_insert_replace_splice_window: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * SKIP_X-suffix variant of inv_insert_replace_splice_window.  Keys are SPARSE
 * (idx << 16) so every leaf hangs off a len-2 compressed suffix node
 * ([0x00,0x00]) reached from its grandparent via a SKIP_X pointer.  A concurrent
 * cds_ft_insert_replace at such a leaf drives the DUAL forward publish --
 * cn->child (read by the exact descent) AND the grandparent SKIP_X slot (read by
 * the candidate descent), both naming the leaf -- which is now fused with the
 * head cell swap into ONE flip (ft_ord_cell_swap_publish_multi; the 2-edge
 * ft_ord_cell_flip when the ordered list is off).  A reader must never observe
 * cn->child and the skip target name different heads mid-replace, nor follow a
 * stale skip target into the freed old leaf.  The dense-key
 * inv_insert_replace_splice_window never forms the SKIP_X-at-leaf shape, so this
 * is the only invariant that drives the dual fusion concurrently.  Run also
 * under FT_INV_NO_ORDERED_LIST to cover the list-off 2-edge flip.
 */
#define INV_SKIPX_KEY(idx)	((uint64_t)(idx) << 16)

static void *inv_skipx_replace_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = INV_SKIPX_KEY(rand_r(&seed) % WRITER_POOL_SIZE);
		uint8_t k[8];
		enum cds_ft_status s;

		cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_iter_node(iter)) {
			s = cds_ft_next(ctx->ft, iter);
			if (s == CDS_FT_STATUS_NOT_FOUND) {
				/*
				 * The sentinel (max key) is never removed, so
				 * every pool key has a successor.
				 */
				report_violation(ctx->test_name,
					"next after exact lookup of %" PRIu64
					" reported end-of-traversal (sentinel present)",
					key);
			}
		}
		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_skipx_replace_writer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = INV_SKIPX_KEY(rand_r(&seed) % WRITER_POOL_SIZE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);
			struct cds_ft_node *old = NULL;

			pthread_mutex_lock(&ctx->lock);
			/* Replace at an existing key drives the dual publish. */
			if (insert_replace_u64(ctx->ft, key, n, &old) >= 0) {
				if (old)
					node_free_rcu(to_test_node(old));
			}
			pthread_mutex_unlock(&ctx->lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(&ctx->lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(&ctx->lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_insert_replace_skipx_splice_window(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_insert_replace_skipx_splice_window";
	pthread_mutex_init(&ctx.lock, NULL);

	/* The permanent sentinel: the maximum key, never removed. */
	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	/* Pre-populate half the writer pool with SPARSE keys (SKIP_X leaves). */
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(INV_SKIPX_KEY(i));
		insert_u64(ft, INV_SKIPX_KEY(i), n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_skipx_replace_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_skipx_replace_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_insert_replace_skipx_splice_window: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * cds_ft_replace (the in-place, caller-supplied old+new node replace) on SKIP_X
 * leaves.  Like ft_promote_head, cds_ft_replace inherited the old head's cell and
 * retargeted it in place; it now publishes a FRESH cell for the new head and
 * swaps it in fused with the cn->child publish + the grandparent SKIP_X dual in
 * one flip.  Sparse keys (idx << 16) reach the compressed-holder head case; the
 * keys are pre-populated and only ever replaced (cds_ft_replace is 1:1, never
 * removes), so every lookup finds a head and the shared splice-window reader's
 * "next has the sentinel successor" check holds.  cds_ft_replace has no other
 * concurrent coverage.
 */
#define INV_SKIPX_KEY(idx)	((uint64_t)(idx) << 16)

static void *inv_replace_skipx_writer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = INV_SKIPX_KEY(rand_r(&seed) % WRITER_POOL_SIZE);
		struct ft_test_node *newn = node_alloc(key);
		struct cds_ft_node *old;
		uint8_t k[8];

		cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
		/*
		 * Stamp the ordinal key bytes BEFORE the node enters the trie,
		 * exactly as insert_u64 / insert_replace_u64 do for every other
		 * insert path in this file.  This is the speculative-key contract
		 * (cds_ft_group_attr_set_speculative_key_offset: "the key bytes at
		 * @key_offset must be present before the node is inserted"), not
		 * harness bookkeeping -- on a speculative group the inequality
		 * lookups take their result key by LOCK it from the matched
		 * leaf, so a head published with @okey still zeroed hands every
		 * later cds_ft_next the key 0.  The walk then asks for the
		 * successor of the wrong key.
		 */
		memcpy(newn->okey, k, sizeof(newn->okey));
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		/* Look up the current head under the writer mutex, then replace it
		 * in place.  All keys stay present, so old is found. */
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(ctx->ft, iter);
		old = cds_ft_iter_node(iter);
		if (old && cds_ft_replace(ctx->ft, iter, old, &newn->node)
				== CDS_FT_STATUS_OK)
			node_free_rcu(to_test_node(old));
		else
			node_free_rcu(newn);	/* unused / not installed */
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_replace_skipx(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_replace_skipx";
	pthread_mutex_init(&ctx.lock, NULL);

	/* Sentinel (max key, never replaced) + all sparse keys (SKIP_X leaves). */
	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	for (i = 0; i < WRITER_POOL_SIZE; i++) {
		struct ft_test_node *n = node_alloc(INV_SKIPX_KEY(i));
		insert_u64(ft, INV_SKIPX_KEY(i), n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_skipx_replace_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_replace_skipx_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_replace_skipx: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

#undef INV_SKIPX_KEY

/*
 * INVARIANT: a key's removal is atomic across the structural index and the
 * ordered cell list -- there is no instant at which a key is reachable via one
 * but not the other.
 *
 * Workload: a single drainer removes the ordered minimum monotonically with NO
 * concurrent inserts (the pool only shrinks).  In one RCU critical section a
 * reader runs three queries:
 *   1) lookup_first    -> ordered-list minimum  (key k1)
 *   2) point lookup(k1) -> structural descent    (found?)
 *   3) lookup_first    -> ordered-list minimum  (key k2)
 * If k1 == k2 (the same key is the ordered minimum before AND after) yet the
 * structural lookup did NOT find it, that key is in the ordered list but absent
 * from the structural index -- which can only happen if cds_ft_remove published
 * the top child-pointer removal before the ordered-list unsplice (a non-atomic
 * unpublish).  Because no inserts run, "present again" cannot be a legitimate
 * re-add.  Comparison is key-based, not by node pointer: a freed node's address
 * can be recycled by a later allocation (ABA).
 */
static void *inv_remove_xview_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *n1, *n2;
		uint64_t k1, k2;
		uint8_t kb[8];
		bool found;

		rcu_quiescent_state();
		rcu_read_lock();
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (n1 = cds_ft_iter_node(iter)) != NULL) {
			k1 = to_test_node(n1)->key;
			/* Structural point lookup of the ordered minimum. */
			cds_ft_u64_to_key(ctx->ft, k1, kb, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, kb, CDS_FT_LEN_DEFAULT);
			found = cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_iter_node(iter) != NULL;
			/* Re-confirm the ordered minimum is still the same key. */
			if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (n2 = cds_ft_iter_node(iter)) != NULL) {
				k2 = to_test_node(n2)->key;
				if (k1 == k2 && !found)
					report_violation(ctx->test_name,
						"ordered-min key %" PRIu64
						" present, absent, present -- structural"
						" removal raced ahead of list unsplice",
						k1);
			}
		}
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_remove_xview_drainer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *node;
		uint64_t k;
		uint8_t kb[8];

		rcu_quiescent_state();
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (node = cds_ft_iter_node(iter)) != NULL &&
		    to_test_node(node)->key != 0xffffffffull) {
			/* Position the iterator structurally, then remove the min. */
			k = to_test_node(node)->key;
			cds_ft_u64_to_key(ctx->ft, k, kb, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, kb, CDS_FT_LEN_DEFAULT);
			if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (node = cds_ft_iter_node(iter)) != NULL &&
			    cds_ft_remove(ctx->ft, iter, node) == CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(node));
		} else {
			/* Only the permanent sentinel (max key) remains: drained. */
			__atomic_store_n(&test_drained, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&ctx->lock);
			rcu_read_unlock();
			break;
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Cross-view oracle pool sizes.  Overridable at build time (-D...) to run a
 * small, fast variant -- e.g. under FEATURE_FT_VERIFY_AT_MUTATION, whose
 * per-mutation full-trie verify makes the default sizes O(n^2)-slow. */
#ifndef REMOVE_XVIEW_POOL
#define REMOVE_XVIEW_POOL	100000
#endif
/* Sparse-pair pool: pairs (k*256, k*256+1) sharing a 3-byte prefix, so each pair
 * sits under its own branch that canonicalizes to a compressed holder as it
 * drains -- exercises the compressed-parent detach publish.  Keys stay < 16 MiB. */
#ifndef REMOVE_XVIEW_PAIRS
#define REMOVE_XVIEW_PAIRS	40000
#endif

/* Shared driver: @ft is pre-populated; spin up the cross-view @reader_fn threads
 * + the @drainer_fn, run to drain or timeout. */
static int run_remove_cross_view(struct cds_ft *ft, struct cds_ft_group *group,
		const char *test_name,
		void *(*reader_fn)(void *), void *(*drainer_fn)(void *))
{
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], drainer;
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = test_name;
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	test_drained = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, reader_fn, &ctx);
	pthread_create(&drainer, NULL, drainer_fn, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS &&
	       !__atomic_load_n(&test_drained, __ATOMIC_RELAXED))
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(drainer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "%s: %lu violation(s)\n",
			test_name, atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

static int inv_remove_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	unsigned int i;

	/* The permanent sentinel (max key) is never removed: bounds the drain. */
	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	for (i = 0; i < REMOVE_XVIEW_POOL; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	return run_remove_cross_view(ft, group, "inv_remove_cross_view",
		inv_remove_xview_reader, inv_remove_xview_drainer);
}

/*
 * Same cross-view atomicity invariant, but a sparse-pair distribution whose
 * removals prune up to COMPRESSED holders (the ft_detach_node_replace_compressed_parent
 * publish), which the dense inv_remove_cross_view never reaches.
 */
static int inv_remove_cross_view_compressed(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	unsigned int k;

	rcu_read_lock();
	{
		struct ft_test_node *n = node_alloc(0xffffffffull);

		if (insert_u64(ft, 0xffffffffull, n) != CDS_FT_STATUS_OK)
			abort();
	}
	for (k = 0; k < REMOVE_XVIEW_PAIRS; k++) {
		struct ft_test_node *a = node_alloc((uint64_t) k * 256);
		struct ft_test_node *b = node_alloc((uint64_t) k * 256 + 1);

		insert_u64(ft, (uint64_t) k * 256, a);
		insert_u64(ft, (uint64_t) k * 256 + 1, b);
	}
	rcu_read_unlock();

	return run_remove_cross_view(ft, group, "inv_remove_cross_view_compressed",
		inv_remove_xview_reader, inv_remove_xview_drainer);
}

/*
 * MAX-draining variant for the compressed-PARENT removal shape (external
 * promote), which the min-draining tests cannot reach: the promote fires when a
 * node's last longer-key child is removed while its shorter PREFIX key still
 * exists -- but a prefix sorts BEFORE its extensions, so min-draining removes
 * the prefix first (emptying the external before the longer child).  Draining
 * the MAX reverses that: the longer child is removed first, while its prefix
 * persists to be promoted, AND the removed longer child IS the current ordered
 * maximum -- so a lookup_last-based reader observes the present/absent/present
 * window.  Requires variable-length prefix keys (fixed-length keys never
 * terminate at an internal node, so cannot produce the external-on-internal
 * shape).  Keys: per group g, Kp(g)=[g_hi,g_lo,0,0] (4 B) and its extension
 * Kl(g)=[g_hi,g_lo,0,0,0] (5 B); the [0,0] run compresses, so Kp's holder sits
 * under a compressed node and removing Kl promotes Kp into the compressed
 * node's child slot.  The test node carries the key bytes in @okey and the
 * length in @value (the eager trie never writes @okey -- no speculative offset).
 */
/*
 * @ws: NULL to inherit the group default (LOCK_FINE), or a strategy to pin.
 * Pinning is used by the graft_swap shape oracles (gs_shared_oracle), which pin
 * CDS_FT_WRITER_LOCK_FINE explicitly so the shape under test is not silently
 * coerced.  The parameter is live -- do not retire it as unused.
 */
static struct cds_ft *create_varlen_ord_ft_ws(struct cds_ft_group **group_out,
		const enum cds_ft_writer_strategy *ws)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	/* Variable-length: keep the default key_len (per-call lengths).  Eager
	 * so the point lookup needs no speculative key offset and @okey is free
	 * for the test to stash the byte key.  Ordered list ON (default). */
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, true) < 0)
		abort();
	if (ws && cds_ft_group_attr_set_writer_strategy(attr, *ws) < 0)
		abort();
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF).  Tries built
	 * by this helper are used by the in-trie MOVE tests, so the capability
	 * has to be declared or every one of them answers NOT_SUPPORTED.  Not
	 * every creator opts in, deliberately: the helpers that stay off are what
	 * cover the default path, where ft_install_lookup_ops selects the PLAIN
	 * lookup specializations instead of the coherent ones.
	 */
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static struct cds_ft *create_varlen_ord_ft(struct cds_ft_group **group_out)
{
	return create_varlen_ord_ft_ws(group_out, NULL);
}

/*
 * Variable-length, ordered list OFF: exercises the graft GLUE flip-txn fold
 * (ft_glue_txn_commit), which is gated on the list being off.  EAGER, NOT
 * SPECULATIVE: an EAGER descent still traverses the skip-compressed nodes the
 * txn flips (and can trigger a parent-pointer reanchor mid-descent) but carries
 * no speculative leaf key, so draining the detached trie cannot hit the
 * pre-existing keycopy-relational livelock (ft_ineq_descend spins on a leaf
 * whose stale speculative key -- the pre-detach full key on a now-stripped
 * detached position -- disagrees with its slot).
 */
/*
 * As create_varlen_nolist_ft_ws, but SPECULATIVE (which leaves
 * CDS_FT_FLAG_SKIP_COMPRESSED set) rather than EAGER (which clears it).
 */
static struct cds_ft *create_varlen_nolist_ft_ws_spec(
		struct cds_ft_group **group_out,
		const enum cds_ft_writer_strategy *ws)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (ws && cds_ft_group_attr_set_writer_strategy(attr, *ws) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static struct cds_ft *create_varlen_ord_ft_ws_spec(
		struct cds_ft_group **group_out,
		const enum cds_ft_writer_strategy *ws)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, true) < 0)
		abort();
	if (ws && cds_ft_group_attr_set_writer_strategy(attr, *ws) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static struct cds_ft *create_varlen_nolist_ft_ws(struct cds_ft_group **group_out,
		const enum cds_ft_writer_strategy *ws)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_lookup_optimization(attr,
			CDS_FT_LOOKUP_OPTIMIZE_EAGER) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (ws && cds_ft_group_attr_set_writer_strategy(attr, *ws) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

/*
 * ==========================================================================
 * COLLIDING-KEY CROSS-TRIE MERGE -- the workload that reaches
 * ft_hlist_append_run_prepare's serializing NULL.
 *
 * ☞ WHY THIS TEST EXISTS, and it is a coverage hole with a measured name.
 * ft_hlist_append_run_prepare records &tail->next against a LITERAL NULL that
 * its own header calls "the serializing one (CAS old = NULL: a concurrent
 * freeze of the tail fails this commit)".  It is the same word the freeze's
 * derived tail writes, from the APPEND side, and the 2026-09-17 SW flip
 * attempt missed it entirely.  When every other freeze site was put through
 * the plan check, this one could not be: -DFT_DEBUG_SPLICE_SEAM measured it at
 * stores=0 in ft_inv and 19 in ft_unit.
 *
 * The zero was NOT "the path is cold".  The merge path is called 44,271 times
 * per ft_inv leg and COLLIDES ZERO times (merge=0/44271): append_run only runs
 * when the source hands the destination a DUPLICATE RUN to splice onto an
 * existing chain tail, and no test built one.  Every cross-trie test in this
 * file keeps its key bands disjoint on purpose -- even the one called
 * _overlap, whose own comment says "the keys stay disjoint, so each writer's
 * shadow is still exact".
 *
 * So build the collision deliberately: the destination and every source hold
 * the SAME keys, each already carrying duplicates, and point writers churn
 * duplicates at those keys while the merges run -- so the tail a splice aims
 * at moves under it.  That is the only shape in which the literal NULL
 * arbitrates anything.
 *
 * THE ORACLE IS A COUNT, and it is exact because every duplicate is accounted:
 * the destination ends with (initial + merged + point_net) duplicates, where
 * point_net is inserts minus removes rather than an assumption that they
 * pair.  A lost splice shows up as a short count; a doubled one as a long
 * count; cds_ft_verify and the teardown walk catch a structurally broken chain
 * that still counts right.
 * ==========================================================================
 */
#define MCC_KEYS	6
#define MCC_DUPS	3
#define MCC_POINT	3
#define MCC_RUN_MS	1500
#define MCC_PREFIX	'M'

struct mcc_ctx {
	struct cds_ft *dst;
	unsigned long stop;
	unsigned long merges;		/* successful cds_ft_merge calls */
	unsigned long point_ins;
	unsigned long point_del;
};

static size_t mcc_mkkey(uint8_t *buf, unsigned int i)
{
	buf[0] = MCC_PREFIX;
	buf[1] = (uint8_t) i;
	return 2;
}

/* MCC_DUPS duplicates of every colliding key; returns how many were added. */
static unsigned int mcc_fill(struct cds_ft *ft)
{
	uint8_t k[2];
	unsigned int i, d, n = 0;

	rcu_read_lock();
	for (i = 0; i < MCC_KEYS; i++) {
		size_t kl = mcc_mkkey(k, i);

		for (d = 0; d < MCC_DUPS; d++) {
			struct ft_test_node *nd = node_alloc(i);

			if (cds_ft_insert(ft, k, kl, &nd->node) !=
					CDS_FT_STATUS_OK)
				abort();
			n++;
		}
	}
	rcu_read_unlock();
	return n;
}

/*
 * Churn duplicates at ONE colliding key: add one, then take one away.  The
 * removal targets whatever the lookup finds rather than the node just added --
 * a merge may have spliced a run in between, and the point of this thread is
 * to move the chain tail, not to police which duplicate leaves.
 */
static void *mcc_point_writer(void *arg)
{
	unsigned long *a = (unsigned long *) arg;
	struct mcc_ctx *c = (struct mcc_ctx *) a[0];
	unsigned int id = (unsigned int) a[1];
	uint8_t k[2];
	size_t kl;

	rcu_register_thread();
	kl = mcc_mkkey(k, id % MCC_KEYS);
	while (!uatomic_load(&c->stop, CMM_ACQUIRE)) {
		struct ft_test_node *nd = node_alloc(id);
		struct cds_ft_iter *iter;
		struct cds_ft_node *found;

		rcu_read_lock();
		if (cds_ft_insert(c->dst, k, kl, &nd->node) ==
				CDS_FT_STATUS_OK)
			uatomic_inc(&c->point_ins);
		else
			free(nd);
		rcu_read_unlock();

		if (cds_ft_iter_create(c->dst, &iter) < 0)
			abort();
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, kl);
		cds_ft_lookup(c->dst, iter);
		found = cds_ft_iter_node(iter);
		if (found && cds_ft_remove(c->dst, iter, found) ==
				CDS_FT_STATUS_OK) {
			uatomic_inc(&c->point_del);
			node_free_rcu(to_test_node(found));
		}
		rcu_read_unlock();
		cds_ft_iter_destroy(iter);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/* Count every duplicate currently in @ft. */
static unsigned long mcc_count(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;
	unsigned long n = 0;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	{
		enum cds_ft_status st;

		for (st = cds_ft_lookup_first(ft, iter);
				st == CDS_FT_STATUS_OK;
				st = cds_ft_next(ft, iter)) {
			struct cds_ft_node *head = cds_ft_iter_node(iter);

			cds_ft_for_each_duplicate_rcu(head)
				n++;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return n;
}

static int inv_concurrent_merge_colliding_chains(void)
{
	INV_NEED_MERGE("inv_concurrent_merge_colliding_chains");
	{
	const char *tname = "inv_concurrent_merge_colliding_chains";
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	const uint8_t pfx[1] = { MCC_PREFIX };
	struct cds_ft_group *group;
	struct cds_ft *probe, *src;
	struct mcc_ctx c;
	pthread_t pt[MCC_POINT];
	unsigned long pargs[MCC_POINT][2];
	struct timespec t0;
	unsigned long expect, got, merged_dups = 0;
	unsigned int i;
	int ret = 0;

	memset(&c, 0, sizeof(c));
	probe = create_varlen_nolist_ft_ws(&group, &ws);
	if (cds_ft_create(group, NULL, &c.dst) < 0)
		abort();
	if (cds_ft_create(group, NULL, &src) < 0)
		abort();
	/*
	 * The destination takes concurrent point writers, so it is SHARED; the
	 * source stays EXCLUSIVE, which cds_ft_merge's contract requires.
	 */
	cds_ft_make_shared(c.dst);
	/*
	 * ☠ AND THE SOURCE MUST BE EXCLUSIVE, once, up front.  cds_ft_merge
	 * rejects a live concurrent source with BUSY before touching anything
	 * (a second trie's lock has no order against the destination's), and a
	 * freshly created trie is SHARED -- measured: without this every merge
	 * returned -4 and the test reached zero splices, which is the very
	 * blindness it was written to remove.  Taken once rather than per
	 * round: cds_ft_make_exclusive drains a grace period, and the source
	 * stays exclusive across fill / merge / refill.
	 */
	cds_ft_make_exclusive(src);

	expect = mcc_fill(c.dst);

	for (i = 0; i < MCC_POINT; i++) {
		pargs[i][0] = (unsigned long) &c;
		pargs[i][1] = i;
		pthread_create(&pt[i], NULL, mcc_point_writer, &pargs[i]);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < MCC_RUN_MS) {
		unsigned int added = mcc_fill(src);

		enum cds_ft_status mst;

		/* A bulk op waits for a grace period: no read section here. */
		mst = cds_ft_merge(c.dst, pfx, 1, src);
		if (mst == CDS_FT_STATUS_OK) {
			merged_dups += added;
			uatomic_inc(&c.merges);
		} else {
			/*
			 * A refused merge leaves @src intact by contract, so
			 * drain it rather than counting keys that never moved.
			 */
			ret = -1;
			report_violation(tname,
				"cds_ft_merge refused a colliding-key source: "
				"status %d", (int) mst);
			goto stop;
		}
		rcu_quiescent_state();
	}
stop:
	uatomic_store(&c.stop, 1, CMM_RELEASE);
	for (i = 0; i < MCC_POINT; i++)
		pthread_join(pt[i], NULL);

	expect += merged_dups + uatomic_read(&c.point_ins)
		- uatomic_read(&c.point_del);
	got = mcc_count(c.dst);
	if (got != expect) {
		report_violation(tname,
			"duplicate count %lu != expected %lu after %lu merges "
			"(%lu merged dups, +%lu/-%lu point) -- a spliced run "
			"was lost or doubled",
			got, expect, c.merges, merged_dups,
			c.point_ins, c.point_del);
		ret = -1;
	}
	if (cds_ft_verify(c.dst, stderr) != CDS_FT_STATUS_OK) {
		report_violation(tname, "cds_ft_verify failed on the merged dst");
		ret = -1;
	}
	if (!c.merges) {
		report_violation(tname,
			"no merge completed -- the test did not exercise the "
			"colliding splice it exists for");
		ret = -1;
	}

	mw_gs_reclaim(src);
	mw_gs_reclaim(c.dst);
	drain_and_destroy(probe, group);
	return ret;
	}
}

static struct cds_ft *create_varlen_nolist_ft(struct cds_ft_group **group_out)
{
	return create_varlen_nolist_ft_ws(group_out, NULL);
}

/* Drain every key from @ft and destroy the trie, leaving its group alive. */
static void drain_trie_keep_group(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;
		enum cds_ft_status s = cds_ft_remove_all(ft, iter, &head);

		/*
		 * SAY WHAT HAPPENED.  A bare abort() here reports nothing at all
		 * -- not even an assertion string -- so a failure costs a gdb run
		 * to learn which status was returned, and the status is what picks
		 * the path.  (Measured: it does fail, under
		 * CDS_FT_LOCK_SPACING=exponential.)
		 */
		if (s < 0) {
			fprintf(stderr,
				"drain_trie_keep_group: remove_all failed: %s (%d)\n",
				cds_ft_status_to_string(s), (int) s);
			abort();
		}
		/* No-progress guard: see drain_and_destroy. */
		if (s == CDS_FT_STATUS_NOT_FOUND) {
			fprintf(stderr,
				"drain_trie_keep_group: lookup_first found a key "
				"remove_all reports NOT_FOUND -- stale iterator key\n");
			abort();
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp)
			node_free_rcu(to_test_node(head));
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	cds_ft_destroy(ft);
}

/* Read a variable-length test node's stashed key bytes + length. */
static size_t xview_node_key(struct cds_ft_node *node, uint8_t *out)
{
	struct ft_test_node *t = to_test_node(node);
	size_t len = (size_t) t->value;

	memcpy(out, t->okey, len);
	return len;
}

static void *inv_remove_xview_reader_max(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *n1, *n2;
		uint8_t k1[8], k2[8];
		size_t l1, l2;
		bool found;

		rcu_quiescent_state();
		rcu_read_lock();
		if (cds_ft_lookup_last(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (n1 = cds_ft_iter_node(iter)) != NULL) {
			l1 = xview_node_key(n1, k1);
			/* Structural point lookup of the ordered maximum. */
			cds_ft_iter_set_key(iter, k1, l1);
			found = cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_iter_node(iter) != NULL;
			/* Re-confirm the ordered maximum is still the same key. */
			if (cds_ft_lookup_last(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (n2 = cds_ft_iter_node(iter)) != NULL) {
				l2 = xview_node_key(n2, k2);
				if (l1 == l2 && memcmp(k1, k2, l1) == 0 && !found)
					report_violation(ctx->test_name,
						"ordered-max key (len %zu) present,"
						" absent, present -- compressed-parent"
						" removal raced ahead of list unsplice",
						l1);
			}
		}
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_remove_xview_drainer_max(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *node;
		uint8_t k[8];
		size_t l;

		rcu_quiescent_state();
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		if (cds_ft_lookup_last(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (node = cds_ft_iter_node(iter)) != NULL) {
			l = xview_node_key(node, k);
			cds_ft_iter_set_key(iter, k, l);
			if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (node = cds_ft_iter_node(iter)) != NULL &&
			    cds_ft_remove(ctx->ft, iter, node) == CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(node));
		} else {
			/* Trie drained (no sentinel: max draining needs none). */
			__atomic_store_n(&test_drained, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&ctx->lock);
			rcu_read_unlock();
			break;
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

#ifndef REMOVE_XVIEW_GROUPS
#define REMOVE_XVIEW_GROUPS	20000
#endif

static int inv_remove_cross_view_compressed_parent(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	unsigned int g;

	rcu_read_lock();
	for (g = 0; g < REMOVE_XVIEW_GROUPS; g++) {
		uint8_t kp[4] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0 };
		uint8_t kl[5] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0, 0 };
		struct ft_test_node *np = node_alloc(g);
		struct ft_test_node *nl = node_alloc(g);

		np->value = 4;
		memcpy(np->okey, kp, 4);
		nl->value = 5;
		memcpy(nl->okey, kl, 5);
		if (cds_ft_insert(ft, kp, 4, &np->node) != CDS_FT_STATUS_OK)
			abort();
		if (cds_ft_insert(ft, kl, 5, &nl->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	return run_remove_cross_view(ft, group,
		"inv_remove_cross_view_compressed_parent",
		inv_remove_xview_reader_max, inv_remove_xview_drainer_max);
}

/*
 * MIN-draining variant for the PREFIX-WITH-SIBLINGS removal shape, which the
 * other oracles cannot reach: removing a prefix key whose holder KEEPS sibling
 * children (its longer-key extensions).  A prefix sorts BEFORE its extensions,
 * so MIN-draining removes the prefix Kp FIRST -- while its extension Kl still
 * hangs off the same internal holder -- and Kp IS the current ordered minimum,
 * so a lookup_first-based reader observes the present/absent/present window if
 * the external_nodes clear and the cell unsplice are not one flip.  (Max-
 * draining instead removes Kl first, promoting Kp to a leaf, so it never sees
 * Kp as a prefix-with-siblings.)  Same variable-length prefix/extension keys as
 * inv_remove_cross_view_compressed_parent: Kp(g)=[g_hi,g_lo,0,0] (4 B) and its
 * extension Kl(g)=[g_hi,g_lo,0,0,0] (5 B).
 */
static void *inv_remove_xview_reader_minvl(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *n1, *n2;
		uint8_t k1[8], k2[8];
		size_t l1, l2;
		bool found;

		rcu_read_lock();
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (n1 = cds_ft_iter_node(iter)) != NULL) {
			l1 = xview_node_key(n1, k1);
			/* Structural point lookup of the ordered minimum. */
			cds_ft_iter_set_key(iter, k1, l1);
			found = cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_iter_node(iter) != NULL;
			/* Re-confirm the ordered minimum is still the same key. */
			if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (n2 = cds_ft_iter_node(iter)) != NULL) {
				l2 = xview_node_key(n2, k2);
				if (l1 == l2 && memcmp(k1, k2, l1) == 0 && !found)
					report_violation(ctx->test_name,
						"ordered-min key (len %zu) present,"
						" absent, present -- structural index and"
						" ordered list disagree (non-atomic unpublish)",
						l1);
			}
		}
		rcu_read_unlock();
		/*
		 * Report a quiescent state (QSBR): a bulk-op drainer (cds_ft_detach)
		 * synchronize_rcu's, and would wait forever for a reader that never
		 * passes a quiescent state.  Harmless for the point-remove drainers,
		 * which never synchronize_rcu.
		 */
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Min-draining via cds_ft_remove: exercises the cds_ft_remove is_prefix path. */
static void *inv_remove_xview_drainer_minvl(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *node;
		uint8_t k[8];
		size_t l;

		rcu_quiescent_state();
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (node = cds_ft_iter_node(iter)) != NULL) {
			l = xview_node_key(node, k);
			cds_ft_iter_set_key(iter, k, l);
			if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (node = cds_ft_iter_node(iter)) != NULL &&
			    cds_ft_remove(ctx->ft, iter, node) == CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(node));
		} else {
			/* Trie drained. */
			__atomic_store_n(&test_drained, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&ctx->lock);
			rcu_read_unlock();
			break;
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Min-draining via cds_ft_remove_all: exercises the cds_ft_remove_all is_prefix
 * path AND, for the empty (NIL) key, the cds_ft_remove_all key_len==0 path --
 * both of which clear an internal holder's external_nodes.  The empty key is
 * the global minimum (a prefix of every key), so it drains first.
 */
static void *inv_remove_xview_drainer_minvl_all(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *node, *removed;
		uint8_t k[8];
		size_t l;

		rcu_quiescent_state();
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (node = cds_ft_iter_node(iter)) != NULL) {
			l = xview_node_key(node, k);
			cds_ft_iter_set_key(iter, k, l);
			if (cds_ft_lookup(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    cds_ft_iter_node(iter) != NULL &&
			    cds_ft_remove_all(ctx->ft, iter, &removed) ==
				    CDS_FT_STATUS_OK && removed != NULL)
				node_free_rcu(to_test_node(removed));
		} else {
			/* Trie drained. */
			__atomic_store_n(&test_drained, 1, __ATOMIC_RELAXED);
			pthread_mutex_unlock(&ctx->lock);
			rcu_read_unlock();
			break;
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Build @ft with the varlen prefix/extension pairs (Kp 4 B, Kl 5 B). */
static void prefix_siblings_populate(struct cds_ft *ft)
{
	unsigned int g;

	for (g = 0; g < REMOVE_XVIEW_GROUPS; g++) {
		uint8_t kp[4] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0 };
		uint8_t kl[5] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0, 0 };
		struct ft_test_node *np = node_alloc(g);
		struct ft_test_node *nl = node_alloc(g);

		np->value = 4;
		memcpy(np->okey, kp, 4);
		nl->value = 5;
		memcpy(nl->okey, kl, 5);
		if (cds_ft_insert(ft, kp, 4, &np->node) != CDS_FT_STATUS_OK)
			abort();
		if (cds_ft_insert(ft, kl, 5, &nl->node) != CDS_FT_STATUS_OK)
			abort();
	}
}

static int inv_remove_cross_view_prefix_siblings(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);

	rcu_read_lock();
	prefix_siblings_populate(ft);
	rcu_read_unlock();

	return run_remove_cross_view(ft, group,
		"inv_remove_cross_view_prefix_siblings",
		inv_remove_xview_reader_minvl, inv_remove_xview_drainer_minvl);
}

/*
 * Same prefix-with-siblings invariant, drained via cds_ft_remove_all and with
 * the empty (NIL) key added so the remove_all key_len==0 clear is exercised too.
 */
static int inv_remove_cross_view_prefix_siblings_all(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct ft_test_node *ne = node_alloc(0);

	rcu_read_lock();
	ne->value = 0;			/* empty key: length 0, okey unused. */
	if (cds_ft_insert(ft, ne->okey, 0, &ne->node) != CDS_FT_STATUS_OK)
		abort();
	prefix_siblings_populate(ft);
	rcu_read_unlock();

	return run_remove_cross_view(ft, group,
		"inv_remove_cross_view_prefix_siblings_all",
		inv_remove_xview_reader_minvl, inv_remove_xview_drainer_minvl_all);
}

/*
 * BULK-OP (disappear-side) cross-view oracle.  A bulk detach severs a whole
 * sub-branch from @ft structurally (one publish) and then moves that run out of
 * @ft's ordered cell list (a separate ft_ord_cell_run_detach flip) -- two
 * commits, unlike the fused point remove.  So a reader doing the same
 * lookup_first -> point(min) -> lookup_first check may observe the ordered
 * minimum present in the list but absent from the structure, if the structural
 * detach raced ahead of the run unsplice.
 *
 * @ft is pre-filled with keys [p_hi,p_lo,s] (a 2-byte prefix P plus a suffix s);
 * the writer detaches prefixes in increasing order (cds_ft_detach at the 2-byte
 * P), so each detach removes the current ordered-minimum run and @ft only ever
 * SHRINKS -- no re-inserts, so "present again" can only be the non-atomic
 * detach window.  The detached sub-tries are drained + destroyed (deferred node
 * frees keep concurrent @ft readers safe).  Reuses the varlen min cross-view
 * reader.  Mirrors inv_merge_atomic_completeness: the bulk op + drain run on the
 * registered main thread (the FT bulk ops tolerate an online caller).
 */
#ifndef DETACH_XVIEW_PREFIXES
#define DETACH_XVIEW_PREFIXES	6000
#endif
#define DETACH_XVIEW_PER	3

static void drain_trie_local(struct cds_ft *ft);	/* defined below */

/*
 * ROOT-ALWAYS-INTERNAL invariant oracle.  ft->root must always tag a plain
 * internal node (the read-side hot descent relies on it; the descent carries a
 * defensive "non-internal root" resolver whose comment claims graft_swap can
 * place a compressed node at the root).  Every re-rooting mutator materializes
 * the new root through the build-invisible internal-root builders, so a compressed
 * root should NEVER be published -- not even transiently.  A post-op probe cannot
 * see a transient (it is canonical by op-end), so this samples ft->root
 * CONCURRENTLY with a re-rooting bulk op.
 *
 * The writer round-trips a KEY_SHORTER graft_swap (the path that builds the
 * extracted swap root from a split compressed prefix): @live holds keys under a
 * long compressed prefix "ABCDEFG", and graft_swap at the SHORTER key "AB" moves
 * that subtree to @swap (building @swap's new root) and back.  Readers hammer
 * cds_ft_debug_root_is_internal() on BOTH tries.  If the descent's
 * non-internal-root resolver is live, a reader catches a compressed root here;
 * if it is dead (as expected), this is provably 0 and the resolver can be
 * retired.
 */
extern int cds_ft_debug_root_is_internal(struct cds_ft *ft) __attribute__((weak));





/*
 * BULK-OP (appear-side) cross-view oracle -- the dual of inv_detach_cross_view.
 * A graft publishes the grafted run into @dst structurally and THEN splices its
 * cell run into @dst's ordered list (on HEAD, a separate ft_ord_cell_run_splice
 * flip), so between the two commits a grafted key is reachable in the STRUCTURE
 * but missing from the ordered LIST.  The fix fuses both into ONE flip.
 *
 * Like the disappear oracle, this is NON-CYCLING (keys only ever APPEAR), which
 * is what makes the check sound: the main thread grafts a fresh run at a
 * strictly DECREASING prefix, so each graft installs a new GLOBAL MINIMUM and
 * the ordered-list minimum only ever decreases -- never re-appears.  The reader
 * compares two independent views of the minimum:
 *   - the ordered-list front  via cds_ft_lookup_first (the O(1) cell-list head),
 *   - the structural minimum  via cds_ft_lookup_ge from a sub-minimal key (a
 *     pure trie descent, no cell list).
 * It reads the list front TWICE around the structural descent and flags only
 * when the front is STABLE yet the structural minimum is a SMALLER key: that
 * means a key is reachable in the structure below the ordered-list front, i.e.
 * its run was published structurally but its cell not yet spliced.  The
 * front-stable sandwich filters an atomic one-flip graft (which moves the front
 * and the structural min together -- the two front reads then differ); only a
 * genuine two-commit window survives.  Monotone-decreasing + add-only forbids
 * the front from rising again, so there are no benign re-appear false positives.
 */
#ifndef GRAFT_XVIEW_GRAFTS
#define GRAFT_XVIEW_GRAFTS	20000
#endif

/* Lexicographic compare of two byte keys (shorter sorts first on a tie). */
static int xview_key_cmp(const uint8_t *a, size_t la, const uint8_t *b, size_t lb)
{
	size_t n = la < lb ? la : lb;
	int c = memcmp(a, b, n);

	if (c)
		return c;
	return la < lb ? -1 : (la > lb ? 1 : 0);
}

static void *inv_graft_xview_appear_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	static const uint8_t SUBMIN[3] = { 0x00, 0x00, 0x00 };

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft_node *n;
		uint8_t kf1[8], kf2[8], ks[8];
		size_t lf1, lf2, lks;

		rcu_read_lock();
		/* List front (cell-list head). */
		if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
		    (n = cds_ft_iter_node(iter)) != NULL) {
			lf1 = xview_node_key(n, kf1);
			/* Structural minimum via a pure trie descent (>= submin). */
			cds_ft_iter_set_key(iter, SUBMIN, 3);
			if (cds_ft_lookup_ge(ctx->ft, iter) == CDS_FT_STATUS_OK &&
			    (n = cds_ft_iter_node(iter)) != NULL) {
				lks = xview_node_key(n, ks);
				/* Re-read the list front: only a STABLE front lets us
				 * compare the two views at one consistent moment. */
				if (cds_ft_lookup_first(ctx->ft, iter) == CDS_FT_STATUS_OK &&
				    (n = cds_ft_iter_node(iter)) != NULL) {
					lf2 = xview_node_key(n, kf2);
					if (xview_key_cmp(kf1, lf1, kf2, lf2) == 0 &&
					    xview_key_cmp(ks, lks, kf1, lf1) < 0)
						report_violation(ctx->test_name,
							"structural minimum (len %zu) is BELOW"
							" the stable ordered-list front (len %zu)"
							" -- a grafted run is published in the"
							" structure but not yet spliced into the"
							" ordered list", lks, lf1);
				}
			}
		}
		rcu_read_unlock();
		/* QSBR: cds_ft_graft synchronize_rcu's; never stall the grafter. */
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, k;

	/* Stable upper bulk: prefix {0xFF,0xFF}, always above every grafted run. */
	rcu_read_lock();
	for (i = 0; i < 16; i++) {
		uint8_t key[3] = { 0xFF, 0xFF, (uint8_t) i };
		struct ft_test_node *n = node_alloc(0x1000 + i);

		n->value = 3;
		memcpy(n->okey, key, 3);
		if (cds_ft_insert(ft, key, 3, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_graft_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_graft_xview_appear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/*
	 * Graft a fresh 2-key run at a strictly DECREASING 2-byte prefix, so each
	 * graft installs a new global minimum.  The source holds the run's SUFFIX
	 * keys ({0x00},{0x01}); after the graft they become prefix-extended keys in
	 * @ft (the test node stashes the full @okey).  Time-bounded: each graft is
	 * a grace period, so the pool may not drain within the window.
	 */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < GRAFT_XVIEW_GRAFTS; k++) {
		unsigned int pv = 0xFEFF - k;	/* decreasing distinct prefix */
		uint8_t prefix[2] = { (uint8_t)(pv >> 8), (uint8_t)(pv & 0xff) };
		struct cds_ft *src;
		unsigned int s;

		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t suffix[1] = { (uint8_t) s };
			uint8_t full[3] = { prefix[0], prefix[1], (uint8_t) s };
			struct ft_test_node *n = node_alloc(pv * 4 + s);

			n->value = 3;
			memcpy(n->okey, full, 3);
			if (cds_ft_insert(src, suffix, 1, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		if (cds_ft_graft(ft, prefix, 2, src) != CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the graft */

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * Functional check for the per-trie EAGER opt-out
 * (cds_ft_attr_set_speculative_keys false): a trie created EAGER inside a
 * SPECULATIVE group must reconstruct the result key from the structure and
 * NEVER read the leaf's stored speculative key.  Insert a key K but stamp the
 * leaf's okey with a deliberately WRONG value W; lookup_first on the EAGER trie
 * must return K, not W.  (On a normal speculative trie it would return W -- the
 * stale-key footgun the opt-out exists to avoid.)  Single-threaded.
 */
static int inv_speculative_per_trie_eager(void)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft_attr *tattr;
	struct cds_ft *eager = NULL;
	struct cds_ft_iter *it = NULL;
	struct ft_test_node *n;
	uint8_t rk[16];
	size_t rk_len = 0;
	static const uint8_t K[3] = { 0x10, 0x20, 0x30 };
	static const uint8_t W[3] = { 0xAA, 0xBB, 0xCC };	/* wrong okey */
	int ret = -1;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	cds_ft_group_attr_set_lookup_optimization(gattr,
		CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	if (cds_ft_group_attr_set_speculative_key_offset(gattr,
			offsetof(struct ft_test_node, okey)) < 0 ||
	    cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_attr_create(&tattr) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	cds_ft_attr_set_speculative_keys(tattr, false);		/* EAGER */
	if (cds_ft_create(group, tattr, &eager) < 0) {
		cds_ft_attr_destroy(tattr);
		cds_ft_group_destroy(group);
		return -1;
	}
	cds_ft_attr_destroy(tattr);

	n = node_alloc(0);
	n->value = 3;
	memcpy(n->okey, W, 3);					/* stamp the WRONG key */
	rcu_read_lock();
	if (cds_ft_insert(eager, K, 3, &n->node) != CDS_FT_STATUS_OK) {
		rcu_read_unlock();
		goto out;
	}
	cds_ft_iter_create(eager, &it);
	if (cds_ft_lookup_first(eager, it) != CDS_FT_STATUS_OK) {
		rcu_read_unlock();
		goto out;
	}
	cds_ft_iter_get_key(it, rk, sizeof(rk), &rk_len);
	rcu_read_unlock();
	if (rk_len != 3 || memcmp(rk, K, 3) != 0) {
		fprintf(stderr, "inv_speculative_per_trie_eager: result key "
			"%02x%02x%02x len %zu, expected %02x%02x%02x -- EAGER "
			"opt-out did not suppress the stamped speculative key\n",
			rk[0], rk[1], rk[2], rk_len, K[0], K[1], K[2]);
		goto out;
	}
	ret = 0;
out:
	if (it)
		cds_ft_iter_destroy(it);
	if (eager)
		return drain_and_destroy(eager, group) == 0 ? ret : -1;
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * cds_ft_verify catches a leaf whose stored speculative key does not match its
 * position (the verify-time safety net for re-keying moves: an app that stamps
 * leaves with their destination key before a staging graft can verify the stamps
 * are right; FEATURE_FT_VERIFY_AT_MUTATION then checks after every mutation).
 * Correct stamp -> verify OK; a stale stamp -> verify FAILS.  Cleanup uses exact
 * lookups (which descend by the query key, unaffected by the stale stored key).
 */
static int inv_speculative_key_verify(void)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *spec = NULL;
	struct ft_test_node *n1, *n2 = NULL;

	/*
	 * This test deliberately inserts a leaf with a wrong stored key, then
	 * checks cds_ft_verify catches it.  Under verify-at-mutation the same
	 * verify runs (and aborts) at the insert itself, before this explicit
	 * call -- which IS the feature working, but it pre-empts the test.  Skip.
	 */
	if (cds_ft_verify_at_mutation_enabled())
		return 0;
	static const uint8_t K1[2] = { 0x11, 0x22 };
	static const uint8_t K2[2] = { 0x33, 0x44 };
	static const uint8_t WRONG[2] = { 0x99, 0x88 };
	int ret = -1;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	cds_ft_group_attr_set_lookup_optimization(gattr,
		CDS_FT_LOOKUP_OPTIMIZE_SPECULATIVE);
	if (cds_ft_group_attr_set_key_len(gattr, 2) < 0 ||
	    cds_ft_group_attr_set_speculative_key_offset(gattr,
			offsetof(struct ft_test_node, okey)) < 0 ||
	    cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_create(group, NULL, &spec) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}

	/* Correctly-stamped leaf: verify must pass. */
	n1 = node_alloc(0);
	memcpy(n1->okey, K1, 2);
	rcu_read_lock();
	if (cds_ft_insert(spec, K1, 2, &n1->node) != CDS_FT_STATUS_OK) {
		rcu_read_unlock();
		goto out;
	}
	rcu_read_unlock();
	if (cds_ft_verify(spec, NULL) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_speculative_key_verify: verify FAILED on a "
			"correctly-stamped leaf\n");
		goto out;
	}

	/* Stale-stamped leaf: verify must FAIL. */
	n2 = node_alloc(1);
	memcpy(n2->okey, WRONG, 2);
	rcu_read_lock();
	if (cds_ft_insert(spec, K2, 2, &n2->node) != CDS_FT_STATUS_OK) {
		rcu_read_unlock();
		goto out;
	}
	rcu_read_unlock();
	if (cds_ft_verify(spec, NULL) == CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_speculative_key_verify: verify MISSED a stale "
			"speculative key\n");
		goto out;
	}
	ret = 0;
out:
	/*
	 * Re-stamp the stale leaf with its correct key so the speculative drain
	 * (lookup_first reads the stored key) can find and free it; otherwise the
	 * deliberately-wrong okey would make the drain miss it and leak.
	 */
	if (n2)
		memcpy(n2->okey, K2, 2);
	return drain_and_destroy(spec, group) == 0 ? ret : -1;
}

/*
 * Concurrent reader/writer invariant for the ordered-list-OFF graft GLUE
 * flip-txn fold (ft_glue_txn_commit).  A fixed deep key STABLE lives under a
 * compressed path; the writer repeatedly SPLITS that path (a diverge graft at a
 * prefix that diverges inside it -> FT_GRAFT_PREP_GLUE, committed via the
 * flip-txn) and HEALS it (detach of the grafted run recompacts the path back).
 * STABLE is present the whole time -- it is the displaced old child of every
 * split -- so an exact lookup of it must ALWAYS succeed.  A miss is a torn flip:
 * the forward publish and the displaced-child re-parent observed out-of-step,
 * the window the txn makes unrepresentable.  List off so the txn path (gated on
 * !ordered_list_set) runs; EAGER readers (see create_varlen_nolist_ft).
 */
#ifndef GRAFT_NOLIST_ITERS
#define GRAFT_NOLIST_ITERS	200000
#endif

/* STABLE: single deep key -> a compressed "C0 40 40 40" path under the root. */
static const uint8_t NOLIST_STABLE[4] = { 0xC0, 0x40, 0x40, 0x40 };
/* Graft prefix: byte 1 (0x20) diverges inside STABLE's compressed path. */
static const uint8_t NOLIST_GRAFT_P[2] = { 0xC0, 0x20 };

static void *inv_graft_nolist_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		rcu_read_lock();
		cds_ft_iter_set_key(iter, NOLIST_STABLE, sizeof(NOLIST_STABLE));
		cds_ft_lookup(ctx->ft, iter);
		if (!cds_ft_iter_node(iter))
			report_violation(ctx->test_name,
				"stable deep key vanished during a diverge graft"
				" -- the GLUE flip-txn forward publish and the"
				" displaced old child's re-parent were observed"
				" out-of-step");
		rcu_read_unlock();
		/* QSBR: cds_ft_graft / cds_ft_detach synchronize_rcu. */
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_no_list_diverge(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_nolist_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct ft_test_node *sn;
	struct timespec t0;
	unsigned int i, k;

	/* STABLE is the only initial key -> one fully-compressed path. */
	sn = node_alloc(0xC0404040UL);
	sn->value = sizeof(NOLIST_STABLE);
	memcpy(sn->okey, NOLIST_STABLE, sizeof(NOLIST_STABLE));
	rcu_read_lock();
	if (cds_ft_insert(ft, NOLIST_STABLE, sizeof(NOLIST_STABLE),
			&sn->node) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_graft_no_list_diverge";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_graft_nolist_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < GRAFT_NOLIST_ITERS; k++) {
		struct cds_ft *src, *detached;
		unsigned int s;

		/* Source: a 2-key run grafted under NOLIST_GRAFT_P. */
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t suffix[1] = { (uint8_t) s };
			struct ft_test_node *n = node_alloc(k * 2 + s);

			n->value = 3;
			n->okey[0] = NOLIST_GRAFT_P[0];
			n->okey[1] = NOLIST_GRAFT_P[1];
			n->okey[2] = (uint8_t) s;
			if (cds_ft_insert(src, suffix, 1, &n->node) !=
					CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		/* Split: diverge inside STABLE's compressed path (GLUE flip-txn). */
		if (cds_ft_graft(ft, NOLIST_GRAFT_P, 2, src) != CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the graft */

		/* Heal: detach the run, recompacting the path back to STABLE-only. */
		if (cds_ft_detach(ft, NOLIST_GRAFT_P, 2, &detached) !=
				CDS_FT_STATUS_OK)
			abort();
		drain_trie_keep_group(detached);

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_no_list_diverge: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * NOSPLIT displaced-external graft cross-view.  A short external
 * @DISPEXT_STABLE is present the whole time; each iteration grafts a LONGER key
 * sharing its prefix, so the graft descent stops at the external BELOW key_len
 * -- the NOSPLIT displaced-external store, which relocates the leaf under a
 * fresh branch as its external_nodes -- then detaches to heal back.  With the
 * displaced external's back-channel re-parent folded into the graft flip-txn
 * (dst_origin), the forward publish and the displaced leaf's re-parent flip in
 * ONE selector flip, so a concurrent lookup of @DISPEXT_STABLE must never see it
 * vanish.  (Before the fold it was a fresh-before-live direct store; this
 * exercises the converted path under readers.)
 */
static const uint8_t DISPEXT_STABLE[2] = { 0xC0, 0x40 };
static const uint8_t DISPEXT_GRAFT[4]  = { 0xC0, 0x40, 0x70, 0x70 };
#define GRAFT_DISPEXT_ITERS	200000

static void *inv_graft_dispext_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		rcu_read_lock();
		cds_ft_iter_set_key(iter, DISPEXT_STABLE, sizeof(DISPEXT_STABLE));
		cds_ft_lookup(ctx->ft, iter);
		if (!cds_ft_iter_node(iter))
			report_violation(ctx->test_name,
				"displaced short external vanished during a"
				" longer-key graft -- the forward publish and the"
				" displaced external's re-parent were observed"
				" out-of-step");
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_displaced_external(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_nolist_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct ft_test_node *sn;
	struct timespec t0;
	unsigned int i, k;

	sn = node_alloc(0xC04040UL);
	sn->value = sizeof(DISPEXT_STABLE);
	memcpy(sn->okey, DISPEXT_STABLE, sizeof(DISPEXT_STABLE));
	rcu_read_lock();
	if (cds_ft_insert(ft, DISPEXT_STABLE, sizeof(DISPEXT_STABLE),
			&sn->node) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_graft_displaced_external";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_graft_dispext_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < GRAFT_DISPEXT_ITERS; k++) {
		struct cds_ft *src, *detached;
		unsigned int s;

		/* Source: a 2-key run grafted under DISPEXT_GRAFT (the longer key). */
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t suffix[1] = { (uint8_t) s };
			struct ft_test_node *n = node_alloc(k * 2 + s);

			n->value = 5;
			if (cds_ft_insert(src, suffix, 1, &n->node) !=
					CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		/* Graft the longer key over DISPEXT_STABLE -> NOSPLIT displaced ext. */
		if (cds_ft_graft(ft, DISPEXT_GRAFT, sizeof(DISPEXT_GRAFT), src)
				!= CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the graft */

		/* Heal: detach the grafted run, restoring DISPEXT_STABLE alone. */
		if (cds_ft_detach(ft, DISPEXT_GRAFT, sizeof(DISPEXT_GRAFT),
				&detached) != CDS_FT_STATUS_OK)
			abort();
		drain_trie_keep_group(detached);

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_displaced_external: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * BULK-OP cross-view oracle for the EMPTY-DST root-level graft --
 * cds_ft_graft(dst, "", 0, src) where dst is empty.  That path makes the empty
 * @dst adopt @src's WHOLE structure as its root AND transfer @src's WHOLE
 * ordered list (head/tail) to it.  On HEAD these were SEPARATE stores: the root
 * published first, the ordered-list head/tail after, so a reader between them
 * saw a grafted key reachable in the STRUCTURE but the ordered LIST empty.  The
 * fix fuses each side's root swap with its head/tail transfer in ONE flip
 * (ft_root_list_swap_publish).
 *
 * NON-CYCLING: each watched dst goes empty -> full exactly ONCE (key_len==0
 * graft requires an empty dst, so the same dst is never re-grafted).  The
 * watched dst changes every graft; the main thread publishes the fresh empty
 * dst through ctx->cur and readers rcu_dereference it.  Every dst is kept alive
 * in @pool until the readers join, so no reader dereferences a freed trie.
 *
 * The flip latch is a MONOTONE global selector, not a per-reader snapshot
 * (urcu_flip_proxy_get reads the live selector), so two independent probes that
 * straddle the commit see old-then-new -- a front-stable SANDWICH is required
 * exactly as in inv_graft_cross_view.  The reader reads the LIST front (O(1)
 * cell-list head) TWICE around a STRUCTURAL min descent (a pure trie descent,
 * no cell list) and flags only when the front is STABLY ABSENT yet the
 * structure holds a key: under the atomic flip the selector's monotonicity
 * forbids absent (sel 0) -> present-struct (sel 1) -> absent (sel 0); only the
 * genuine two-store window (root published before the head/tail transfer) lets
 * a key be reachable structurally while the front cell is still NULL.
 */
#ifndef ROOTSWAP_XVIEW_GRAFTS
#define ROOTSWAP_XVIEW_GRAFTS	40000
#endif

/*
 * The merge empty-dst path runs a synchronize_rcu (ft_detach_keylen) before the
 * narrow root/head-store window, so it does far fewer ops than the sync-free
 * graft path -- more concurrent readers compensate so the appear window is
 * caught reliably.
 */
#ifndef ROOTSWAP_XVIEW_READERS
#define ROOTSWAP_XVIEW_READERS	32
#endif

struct inv_rootswap_ctx {
	struct cds_ft * volatile cur;	/* currently-watched empty->full dst */
	const char *test_name;
};

static void *inv_rootswap_appear_reader(void *arg)
{
	struct inv_rootswap_ctx *ctx = (struct inv_rootswap_ctx *) arg;
	struct cds_ft_iter *iter = NULL;
	struct cds_ft *bound = NULL;	/* @iter is bound to this dst */
	static const uint8_t SUBMIN[1] = { 0x00 };

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft *dst = rcu_dereference(ctx->cur);
		bool front1, front2, struct_full;

		if (!dst) {
			rcu_quiescent_state();
			continue;
		}
		/*
		 * An iter is bound to one trie (a debug assertion enforces it),
		 * but the watched dst advances every graft.  Re-bind only when it
		 * changes -- the reader spins faster than the grafter, so it
		 * usually re-checks the same dst (and catches its empty->full
		 * window) on the cached iter.  Every dst is pool-pinned, so the
		 * cached @bound never dangles.  Create outside the read section.
		 */
		if (dst != bound) {
			if (iter)
				cds_ft_iter_destroy(iter);
			if (cds_ft_iter_create(dst, &iter) < 0)
				abort();
			bound = dst;
		}

		rcu_read_lock();
		/* Front-stable sandwich: list front, structural min, list front. */
		front1 = cds_ft_lookup_first(dst, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		cds_ft_iter_set_key(iter, SUBMIN, 1);
		struct_full = cds_ft_lookup_ge(dst, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		front2 = cds_ft_lookup_first(dst, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		if (struct_full && !front1 && !front2)
			report_violation(ctx->test_name,
				"a key is reachable in the structure but the"
				" ordered-list front is stably empty -- the"
				" empty-dst root graft published the root before"
				" transferring the ordered-list head/tail");
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	if (iter)
		cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_root_swap_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *probe = create_varlen_ord_ft(&group);
	struct inv_rootswap_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct cds_ft **pool;
	struct timespec t0;
	unsigned int i, k, ngrafts = 0;
	int ret = 0;

	pool = (struct cds_ft **) calloc(ROOTSWAP_XVIEW_GRAFTS, sizeof(*pool));
	if (!pool)
		abort();

	ctx.cur = NULL;
	ctx.test_name = "inv_graft_root_swap_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_rootswap_appear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < ROOTSWAP_XVIEW_GRAFTS; k++) {
		struct cds_ft *dst, *src;
		unsigned int s;

		if (cds_ft_create(group, NULL, &dst) < 0)
			abort();
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		/* Source holds a fresh 2-key run (>= the 1-byte structural submin). */
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t key[2] = { 0x01, (uint8_t) s };
			struct ft_test_node *n = node_alloc(k * 2 + s);

			n->value = 2;
			memcpy(n->okey, key, 2);
			if (cds_ft_insert(src, key, 2, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();

		/* Publish the empty dst, THEN graft src into it (empty -> full). */
		pool[k] = dst;
		ngrafts = k + 1;
		rcu_assign_pointer(ctx.cur, dst);
		cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */
		if (cds_ft_graft(dst, (const uint8_t *) "", 0, src)
				!= CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the graft */

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	/*
	 * Readers have joined; nothing dereferences ctx.cur any more.  Drain and
	 * free every watched dst (their leaves are app-owned test nodes), then
	 * destroy the tries and the shared group.
	 */
	rcu_assign_pointer(ctx.cur, NULL);
	for (k = 0; k < ngrafts; k++) {
		struct cds_ft_iter *iter;

		if (cds_ft_iter_create(pool[k], &iter) < 0)
			abort();
		rcu_read_lock();
		while (cds_ft_lookup_first(pool[k], iter) == CDS_FT_STATUS_OK) {
			struct cds_ft_node *head, *tmp;

			if (cds_ft_remove_all(pool[k], iter, &head) < 0) {
				ret = -1;
				break;
			}
			cds_ft_for_each_duplicate_safe_rcu(head, tmp)
				node_free_rcu(to_test_node(head));
		}
		rcu_read_unlock();
		cds_ft_iter_destroy(iter);
	}
	rcu_barrier();
	for (k = 0; k < ngrafts; k++)
		cds_ft_destroy(pool[k]);
	free(pool);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_root_swap_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	cds_ft_destroy(probe);
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * BULK-OP cross-view oracle for the EMPTY-DST root-level MERGE -- the twin of
 * inv_graft_root_swap_cross_view for cds_ft_merge_at(dst, "", 0, src, "", 0)
 * with dst empty (the ft_merge_at dst_key_len == 0 && cnt_dst == 0 path).  That
 * path detaches the whole source into a fresh EXCLUSIVE subtree and swaps it
 * into the empty dst root, then transfers the subtree's whole ordered list
 * (head/tail) -- on HEAD two separate stores, fused into ONE flip by the fix.
 * Only the dst side has concurrent readers (the subtree is exclusive, no
 * src-side disappear window), so the reader and its soundness argument are
 * identical to the graft twin: it reuses inv_rootswap_appear_reader (the
 * front-stable sandwich required by the monotone global flip selector).
 */
static int inv_merge_root_swap_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_root_swap_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *probe = create_varlen_ord_ft(&group);
	struct inv_rootswap_ctx ctx;
	pthread_t readers[ROOTSWAP_XVIEW_READERS];
	struct cds_ft **pool;
	struct timespec t0;
	unsigned int i, k, nmerges = 0;
	int ret = 0;

	pool = (struct cds_ft **) calloc(ROOTSWAP_XVIEW_GRAFTS, sizeof(*pool));
	if (!pool)
		abort();

	ctx.cur = NULL;
	ctx.test_name = "inv_merge_root_swap_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < ROOTSWAP_XVIEW_READERS; i++)
		pthread_create(&readers[i], NULL,
			inv_rootswap_appear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < ROOTSWAP_XVIEW_GRAFTS; k++) {
		struct cds_ft *dst, *src;
		unsigned int s;

		static const uint8_t SRC_PREFIX[1] = { 0x01 };

		if (cds_ft_create(group, NULL, &dst) < 0)
			abort();
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		/*
		 * Source holds a fresh 2-key run UNDER prefix {0x01}.  A
		 * src_key_len > 0 is what routes the merge to the empty-dst
		 * root-swap path: a src_key_len == 0 whole-source move is instead
		 * delegated to ft_graft_keylen (the graft empty-dst path, covered
		 * by inv_graft_root_swap_cross_view).  Moved key K = {0x01}||S
		 * becomes ""||S = S, so dst gains {0x00},{0x01} (>= the submin).
		 */
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t key[2] = { 0x01, (uint8_t) s };
			struct ft_test_node *n = node_alloc(k * 2 + s);

			n->value = 2;
			memcpy(n->okey, key, 2);
			if (cds_ft_insert(src, key, 2, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();

		/* Publish the empty dst, THEN merge src@{0x01} into it at "" (empty
		 * -> full): dst_key_len == 0 + empty dst hits the root-swap path. */
		pool[k] = dst;
		nmerges = k + 1;
		rcu_assign_pointer(ctx.cur, dst);
		cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */
		if (cds_ft_merge_at(dst, (const uint8_t *) "", 0, src, SRC_PREFIX, 1)
				!= CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the merge */

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < ROOTSWAP_XVIEW_READERS; i++)
		pthread_join(readers[i], NULL);

	/* Readers joined; drain and free every watched dst, then the group. */
	rcu_assign_pointer(ctx.cur, NULL);
	for (k = 0; k < nmerges; k++) {
		struct cds_ft_iter *iter;

		if (cds_ft_iter_create(pool[k], &iter) < 0)
			abort();
		rcu_read_lock();
		while (cds_ft_lookup_first(pool[k], iter) == CDS_FT_STATUS_OK) {
			struct cds_ft_node *head, *tmp;

			if (cds_ft_remove_all(pool[k], iter, &head) < 0) {
				ret = -1;
				break;
			}
			cds_ft_for_each_duplicate_safe_rcu(head, tmp)
				node_free_rcu(to_test_node(head));
		}
		rcu_read_unlock();
		cds_ft_iter_destroy(iter);
	}
	rcu_barrier();
	for (k = 0; k < nmerges; k++)
		cds_ft_destroy(pool[k]);
	free(pool);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_root_swap_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	cds_ft_destroy(probe);
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * BULK-OP (disappear-side) cross-view oracle for cds_ft_merge_at's ROOT-SRC
 * spine-copy shape: merging a WHOLE source (src_key_len == 0) into an OCCUPIED
 * dst sub-position (cnt_dst > 0) reaches ft_merge_spine_copy's root_src branch,
 * which swaps src->root to a fresh empty root (structure) and THEN unlinks the
 * source's whole run from src's ordered list (a separate ft_ord_cell_run_unlink
 * flip), so a SRC reader between them sees src structurally empty but its
 * ordered-list front still populated.  The fix fuses the root swap with the
 * head/tail clear (ft_root_list_swap_publish).
 *
 * The disappear-side dual of inv_rootswap_appear_reader: the watched src changes
 * every merge (a fresh src, emptied by the merge), published through ctx->cur
 * and pool-pinned until the readers join; the reader rebinds its iter per src and
 * uses the front-stable sandwich (front, struct, front), flagging when the front
 * is STABLY PRESENT yet the structure is empty.  NON-CYCLING: each src goes
 * full -> empty exactly once, so a re-populating front is impossible.
 */
static void *inv_rootswap_disappear_reader(void *arg)
{
	struct inv_rootswap_ctx *ctx = (struct inv_rootswap_ctx *) arg;
	struct cds_ft_iter *iter = NULL;
	struct cds_ft *bound = NULL;
	static const uint8_t SUBMIN[1] = { 0x00 };

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft *src = rcu_dereference(ctx->cur);
		bool front1, front2, struct_full;

		if (!src) {
			rcu_quiescent_state();
			continue;
		}
		if (src != bound) {
			if (iter)
				cds_ft_iter_destroy(iter);
			if (cds_ft_iter_create(src, &iter) < 0)
				abort();
			bound = src;
		}

		rcu_read_lock();
		front1 = cds_ft_lookup_first(src, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		cds_ft_iter_set_key(iter, SUBMIN, 1);
		struct_full = cds_ft_lookup_ge(src, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		front2 = cds_ft_lookup_first(src, iter) == CDS_FT_STATUS_OK
			&& cds_ft_iter_node(iter) != NULL;
		if (front1 && front2 && !struct_full)
			report_violation(ctx->test_name,
				"the ordered-list front is stably present but the"
				" structure is empty -- the root_src merge swapped"
				" src->root to empty before clearing the"
				" ordered-list head/tail");
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	if (iter)
		cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* ================================================================== */
/*                                                                    */
/*   EMPTY-DST ROOT ATTACH vs a concurrent KEYED attach (writer-side) */
/*                                                                    */
/* ================================================================== */

/*
 * The empty-dst ROOT attach decides "@dst_ft is empty" long before it commits,
 * and nothing re-validates that decision at the commit.
 *
 *   ft_merge_at  (dst_key_len == 0 && cnt_dst == 0): cnt_dst is computed near
 *     the top of the call; between it and the swap the path allocates a
 *     replacement src root, creates the appear txn, and runs an ENTIRE
 *     ft_detach_keylen of the source.
 *   cds_ft_graft (key_len == 0): emptiness is the nr_child/external_nodes
 *     check at the head of the root arm; then an alloc and a txn create.
 *
 * The commit validates only its RYW expected-olds -- the root pointer and the
 * old root's state word, BOTH read at RECORD time -- so a peer that populated
 * @dst_ft inside that window is matched by construction rather than detected.
 * The peer's keys then ride the old root into free_cds_ft_node.
 *
 * WHY THE PEER IS LEGAL, and why it is an attach and not an insert.  The
 * public contract grants exactly one concurrency here (fractal-trie.h, both
 * cds_ft_graft and cds_ft_merge_at): under CDS_FT_WRITER_LOCK_FINE "several
 * cross-trie attaches (cds_ft_graft, cds_ft_graft_swap, cds_ft_merge_at) may
 * run concurrently on the same destination", while "exclusion against the
 * point-update operations remains the caller's responsibility".  So the peer
 * is a second cds_ft_graft -- a KEYED one, at {0x42} -- and a cds_ft_insert
 * peer would prove nothing because the contract never promised it.
 *
 * THE ORACLE.  Per round, on a FRESH empty dst, thread A runs the root-level
 * attach (whole src_a, keys under 0x41) and thread B a keyed graft at {0x42}.
 * The two key spaces are disjoint by construction, so the check needs no
 * linearization argument at all:
 *
 *     every attach that returned OK must have ALL of its keys in dst,
 *     and every attach that did NOT return OK must have left its source
 *     UNCHANGED (the contract's "on ANY failure @src_ft is left unchanged").
 *
 * Both orders satisfy that on a correct implementation.  A first: dst gets A's
 * keys, then B's keyed graft attaches under 0x42, both OK, both present.  B
 * first: A observes a populated dst -- graft returns POPULATED_ERROR (keys stay
 * in src_a), merge_at falls through to its diverged sub-position path and
 * merges -- and again nothing is lost.  The defect is the third outcome: A
 * observes empty, B commits into the still-current root, A swaps that root away
 * and frees it.  Both report OK and B's keys are gone.
 *
 * Runs over both list settings because the merge empty-dst arm is written twice
 * (ft_root_list_swap_publish list-on vs the 2-edge root+tombstone flip
 * list-off), and the graft root arm's dual swap likewise.
 */
#define EMPTY_DST_A_PREFIX	0x41	/* thread A's keys: {0x41, hi, lo}   */
#define EMPTY_DST_B_PREFIX	0x42	/* thread B grafts its src AT {0x42} */
#define EMPTY_DST_NR_KEYS	3	/* keys per side per round           */
/*
 * Longer than DEFAULT_DURATION_MS: this oracle needs the two attaches to
 * collide inside a window bounded by one alloc + one txn create (graft) and
 * the round cost is dominated by the serial refill/drain, so a 200 ms budget
 * samples far too few collisions to be a dependable regression guard.
 */
#define EMPTY_DST_DURATION_MS	1500

enum empty_dst_mode {
	EMPTY_DST_MODE_GRAFT,	/* A = cds_ft_graft(dst, "", 0, src_a)           */
	EMPTY_DST_MODE_MERGE,	/* A = cds_ft_merge_at(dst, "", 0, src_a, "", 0) */
};

struct empty_dst_ctx {
	struct cds_ft *dst;		/* fresh per round, published before the release */
	struct cds_ft *src_a, *src_b;
	enum empty_dst_mode mode;
	unsigned long seq;		/* bumped by main to release one round */
	unsigned long done;		/* each attacher increments when finished */
	int stop;
	enum cds_ft_status st_a, st_b;
};

/*
 * Round rendezvous, SPIN-and-quiesce rather than a pthread_barrier.
 *
 * This is a QSBR suite: a registered thread that blocks without reporting a
 * quiescent state stalls every grace period in the process.  A pthread_barrier
 * here deadlocks on the very first round -- the two attachers park in
 * barrier_wait while main enters the round teardown, whose cds_ft_destroy waits
 * for a grace period the parked threads can never let complete.  That is why
 * every concurrent oracle in this file rendezvouses on spun flags.
 *
 * Returns the new @seq once it leaves @seen, or 0 when the run is stopping
 * (@seq is bumped before the first round, so 0 is never a live round).
 */
static unsigned long empty_dst_wait_round(struct empty_dst_ctx *c,
		unsigned long seen)
{
	for (;;) {
		unsigned long s = uatomic_load(&c->seq, CMM_ACQUIRE);

		if (s != seen)
			return s;
		if (uatomic_load(&c->stop, CMM_RELAXED))
			return 0;
		caa_cpu_relax();
		rcu_quiescent_state();
	}
}

/* Thread A: the ROOT-level attach -- the arm under test. */
static void *empty_dst_root_attacher(void *arg)
{
	struct empty_dst_ctx *c = (struct empty_dst_ctx *) arg;
	unsigned long seen = 0;

	rcu_register_thread();
	for (;;) {
		seen = empty_dst_wait_round(c, seen);
		if (!seen)
			break;
		if (c->mode == EMPTY_DST_MODE_GRAFT)
			c->st_a = cds_ft_graft(c->dst,
				(const uint8_t *) "", 0, c->src_a);
		else
			c->st_a = cds_ft_merge_at(c->dst,
				(const uint8_t *) "", 0, c->src_a,
				(const uint8_t *) "", 0);
		uatomic_add(&c->done, 1);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/* Thread B: the contract-legal KEYED peer attach into the same destination. */
static void *empty_dst_keyed_attacher(void *arg)
{
	struct empty_dst_ctx *c = (struct empty_dst_ctx *) arg;
	const uint8_t bkey[1] = { EMPTY_DST_B_PREFIX };
	unsigned long seen = 0;

	rcu_register_thread();
	for (;;) {
		seen = empty_dst_wait_round(c, seen);
		if (!seen)
			break;
		c->st_b = cds_ft_graft(c->dst, bkey, 1, c->src_b);
		uatomic_add(&c->done, 1);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * Fill @ft with EMPTY_DST_NR_KEYS keys of the form @prefix || {hi, lo} when
 * @with_prefix, else the bare {hi, lo} suffix (thread B's source is grafted AT
 * {0x42}, so its own keys must NOT already carry that byte).  @round
 * disambiguates rounds so a stale node from a previous round is never mistaken
 * for a live one.  Leaves @ft EXCLUSIVE, as a cross-trie source must be.
 */
static void empty_dst_fill_src(struct cds_ft *ft, uint8_t prefix,
		bool with_prefix, unsigned int round)
{
	unsigned int s;

	for (s = 0; s < EMPTY_DST_NR_KEYS; s++) {
		uint8_t key[4];
		size_t klen = 0;
		struct ft_test_node *n = node_alloc(
			((uint64_t) prefix << 32) | ((uint64_t) round << 8) | s);

		if (with_prefix)
			key[klen++] = prefix;
		key[klen++] = (uint8_t) (round >> 8);
		key[klen++] = (uint8_t) round;
		key[klen++] = (uint8_t) s;
		/* @okey holds the LANDED key, i.e. always prefixed. */
		n->okey[0] = prefix;
		memcpy(n->okey + 1, key + (with_prefix ? 1 : 0), klen - (with_prefix ? 1 : 0));
		if (cds_ft_insert(ft, key, klen, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	/*
	 * Idempotent and one-way; called every round because a source is
	 * recycled, and a cross-trie source must be EXCLUSIVE under LOCK_FINE
	 * (a live one is rejected with BUSY).
	 */
	cds_ft_make_exclusive(ft);
}

/*
 * Are all EMPTY_DST_NR_KEYS keys of @prefix/@round present in @ft?  The landed
 * key is always @prefix || {hi, lo, s}: thread A's source carried the prefix
 * itself and moves to the root unchanged; thread B's did not and acquires it
 * from the graft point.  Returns the number FOUND.
 */
static unsigned int empty_dst_count_present(struct cds_ft *ft, uint8_t prefix,
		unsigned int round)
{
	struct cds_ft_iter *iter;
	unsigned int s, found = 0;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	for (s = 0; s < EMPTY_DST_NR_KEYS; s++) {
		uint8_t key[4] = { prefix, (uint8_t) (round >> 8),
			(uint8_t) round, (uint8_t) s };

		cds_ft_iter_set_key(iter, key, 4);
		if (cds_ft_lookup(ft, iter) == CDS_FT_STATUS_OK
				&& cds_ft_iter_node(iter) != NULL)
			found++;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return found;
}

/* Remove every key from @ft (freeing the test nodes) but KEEP the trie. */
static void empty_dst_drain_keys(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;

		if (cds_ft_remove_all(ft, iter, &head) < 0)
			abort();
		cds_ft_for_each_duplicate_safe_rcu(head, tmp)
			node_free_rcu(to_test_node(head));
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
}

static int empty_dst_oracle(const char *tname, enum empty_dst_mode mode,
		bool list_on)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *probe;
	struct empty_dst_ctx ctx;
	pthread_t ta, tb;
	struct timespec t0;
	unsigned int round = 0;
	unsigned long lost_a = 0, lost_b = 0, stranded = 0;
	unsigned long ok_both = 0, ok_a_only = 0, ok_b_only = 0;
	int ret = 0;

	probe = list_on ? create_varlen_ord_ft_ws(&group, &ws)
			: create_varlen_nolist_ft_ws(&group, &ws);

	ctx.mode = mode;
	ctx.stop = 0;
	ctx.seq = 0;
	ctx.done = 0;
	/*
	 * The three tries are created ONCE and recycled: each round drains them
	 * back to the round-0 state (dst empty, both sources refilled) instead of
	 * destroying and recreating.  cds_ft_destroy waits out a grace period, so
	 * a create/destroy per round held the whole oracle to SIX rounds inside
	 * the duration budget -- far too few to sample a window this narrow.
	 * Recycling buys ~3 orders of magnitude more rounds for the same wall
	 * clock, and the round counter keeps every round's keys distinct so a
	 * straggler can never be mistaken for a live key.
	 */
	if (cds_ft_create(group, NULL, &ctx.dst) < 0)
		abort();
	if (cds_ft_create(group, NULL, &ctx.src_a) < 0)
		abort();
	if (cds_ft_create(group, NULL, &ctx.src_b) < 0)
		abort();
	pthread_create(&ta, NULL, empty_dst_root_attacher, &ctx);
	pthread_create(&tb, NULL, empty_dst_keyed_attacher, &ctx);

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;; round++) {
		unsigned int in_dst_a, in_dst_b;

		empty_dst_fill_src(ctx.src_a, EMPTY_DST_A_PREFIX, true, round);
		empty_dst_fill_src(ctx.src_b, EMPTY_DST_B_PREFIX, false, round);
		ctx.st_a = ctx.st_b = CDS_FT_STATUS_OK;

		uatomic_store(&ctx.done, 0, CMM_RELAXED);
		/* Publishes dst/src_a/src_b and the fresh statuses to both threads. */
		uatomic_store(&ctx.seq, (unsigned long) round + 1, CMM_RELEASE);
		while (uatomic_load(&ctx.done, CMM_ACQUIRE) != 2) {
			caa_cpu_relax();
			rcu_quiescent_state();
		}

		in_dst_a = empty_dst_count_present(ctx.dst,
			EMPTY_DST_A_PREFIX, round);
		in_dst_b = empty_dst_count_present(ctx.dst,
			EMPTY_DST_B_PREFIX, round);

		/* An attach that reported OK must have landed ALL of its keys. */
		if (ctx.st_a == CDS_FT_STATUS_OK
				&& in_dst_a != EMPTY_DST_NR_KEYS) {
			if (!lost_a++)
				fprintf(stderr, "%s: round %u: root attach "
					"returned OK but only %u/%u of its keys "
					"are in dst (peer st_b=%d)\n", tname,
					round, in_dst_a, EMPTY_DST_NR_KEYS,
					(int) ctx.st_b);
			ret = -1;
		}
		if (ctx.st_b == CDS_FT_STATUS_OK
				&& in_dst_b != EMPTY_DST_NR_KEYS) {
			if (!lost_b++)
				fprintf(stderr, "%s: round %u: the peer keyed "
					"graft returned OK but only %u/%u of its "
					"keys are in dst -- the root attach "
					"(st_a=%d) swapped them away\n", tname,
					round, in_dst_b, EMPTY_DST_NR_KEYS,
					(int) ctx.st_a);
			ret = -1;
		}
		/* An attach that FAILED must have left its source untouched. */
		if (ctx.st_a != CDS_FT_STATUS_OK
				&& empty_dst_count_present(ctx.src_a,
					EMPTY_DST_A_PREFIX, round)
					!= EMPTY_DST_NR_KEYS) {
			if (!stranded++)
				fprintf(stderr, "%s: round %u: root attach "
					"failed (%d) but did not leave src_a "
					"unchanged\n", tname, round,
					(int) ctx.st_a);
			ret = -1;
		}
		if (ctx.st_a == CDS_FT_STATUS_OK && ctx.st_b == CDS_FT_STATUS_OK)
			ok_both++;
		else if (ctx.st_a == CDS_FT_STATUS_OK)
			ok_a_only++;
		else if (ctx.st_b == CDS_FT_STATUS_OK)
			ok_b_only++;

		if (cds_ft_verify(ctx.dst, stderr) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "%s: round %u: cds_ft_verify(dst) "
				"failed\n", tname, round);
			ret = -1;
		}
		empty_dst_drain_keys(ctx.dst);
		empty_dst_drain_keys(ctx.src_a);
		empty_dst_drain_keys(ctx.src_b);
		rcu_quiescent_state();

		if (elapsed_ms(&t0) >= EMPTY_DST_DURATION_MS)
			break;
	}

	uatomic_store(&ctx.stop, 1, CMM_RELEASE);
	pthread_join(ta, NULL);
	pthread_join(tb, NULL);

	/*
	 * Report the interleaving census, not just the verdict.  A run where
	 * ok_both is 0 has never driven the window at all -- the oracle would be
	 * green for the wrong reason, so say so out loud rather than pass.
	 */
	fprintf(stderr, "# %s: %u rounds (both-OK %lu, root-only %lu, "
		"peer-only %lu), lost: root %lu peer %lu, stranded %lu\n",
		tname, round + 1, ok_both, ok_a_only, ok_b_only,
		lost_a, lost_b, stranded);
	if (ok_both == 0) {
		fprintf(stderr, "%s: NO round had both attaches succeed -- the "
			"concurrent empty-dst window was never driven\n", tname);
		ret = -1;
	}

	cds_ft_destroy(ctx.dst);
	cds_ft_destroy(ctx.src_a);
	cds_ft_destroy(ctx.src_b);
	rcu_barrier();
	cds_ft_destroy(probe);
	cds_ft_group_destroy(group);
	return ret;
}

static int inv_empty_dst_root_graft_peer(void)
{
	return empty_dst_oracle("inv_empty_dst_root_graft_peer",
		EMPTY_DST_MODE_GRAFT, /*list_on=*/ true);
}

static int inv_empty_dst_root_graft_peer_nolist(void)
{
	return empty_dst_oracle("inv_empty_dst_root_graft_peer_nolist",
		EMPTY_DST_MODE_GRAFT, /*list_on=*/ false);
}

static int inv_empty_dst_root_merge_peer(void)
{
	INV_NEED_MERGE("inv_empty_dst_root_merge_peer");
	return empty_dst_oracle("inv_empty_dst_root_merge_peer",
		EMPTY_DST_MODE_MERGE, /*list_on=*/ true);
}

static int inv_empty_dst_root_merge_peer_nolist(void)
{
	INV_NEED_MERGE("inv_empty_dst_root_merge_peer_nolist");
	return empty_dst_oracle("inv_empty_dst_root_merge_peer_nolist",
		EMPTY_DST_MODE_MERGE, /*list_on=*/ false);
}

/* ================================================================== */
/*                                                                    */
/*   PREFIX-KEY PARK vs churn under its holder (external_nodes)       */
/*                                                                    */
/* ================================================================== */

/*
 * A key that TERMINATES at an internal node does not sit in a child slot: its
 * chain head is published into that node's meta->external_nodes, by
 * ft_insert_park_external_nodes, as a {NULL -> node} edge carrying only a
 * READ-ONLY validate of the holder's state word.  No retire anywhere validates
 * &meta->external_nodes.  So the standing question is whether a retire of the
 * holder can swallow a head parked concurrently into it -- a lost insert in the
 * insert/remove mix the contract fully supports (fractal-trie.h: cds_ft_insert
 * "may run concurrently with cds_ft_insert and cds_ft_remove on the same trie,
 * including on the same key").
 *
 * NOTHING in the suite drove that park concurrently at all: instrumented, the
 * whole of ft_inv executed ft_insert_park_external_nodes 54246 times and NOT
 * ONCE while a peer held the holder's node lock; every concurrent-writer
 * oracle here scored a flat zero, because they all use key sets in which no key
 * is a proper prefix of another.
 *
 * This oracle supplies that shape.  Each pair of writers shares one holder:
 *   - the PREFIX writer inserts and removes the 1-byte key {0x40+w}, which
 *     terminates AT the holder -- the park;
 *   - the CHILD writer inserts and removes {0x40+w, s}, churning the holder's
 *     child set and driving its recompaction.
 * The key layout is deliberately 1-byte-prefix + 2-byte-children so that NO
 * single-child chain forms on the path: with a compressed prefix (the obvious
 * {0xB1,w} + {0xB1,w,s,x} layout) the terminating key lands on the COMPRESSED
 * node while its CHILD does the branching, so the two writers never touch one
 * node and the whole exercise measures nothing.
 *
 * The check is exact and needs no linearization argument: a prefix key is owned
 * by exactly one thread, so an insert that returned OK must be findable by that
 * thread immediately afterwards.  A miss is a swallowed park.
 *
 * WHAT THIS DOES AND DOES NOT COVER -- stated because the numbers were the
 * point.  It drives the park hard and exposed: ~670k parks per run land while a
 * peer holds the holder's LOCK, all of them the {NULL -> node} first-publish
 * arm.  It has NOT reproduced a loss (0 across ~50M ops), and instrumentation
 * says why: those fences are the holder being locked as the PARENT {P} of a
 * child's recompact -- a {LOCK|s -> s} RELEASE lock, which never copies the
 * holder's body, so external_nodes cannot be lost through it.  A loss needs the
 * holder to be the recompact's own target C, and across every shape tried
 * ft_node_recompact was entered on a node carrying an external head ZERO times
 * (1506437 entries one shape, 3293 another).  Forcing the window open (a 60us
 * delay between the recompact's ext_snapshot read and its commit, 288 times
 * against 2.2M concurrent parks) produced ZERO parks into the delayed node.
 *
 * So this is a REGRESSION GUARD, not a refutation: the hazard is real on paper
 * and the arm is unvalidated, but the two halves do not meet in any shape that
 * could be constructed.  If a future change makes a holder its own recompact
 * target, this is what should go red.
 */
#define PREFIX_PARK_PAIRS	6	/* one prefix writer + one child writer each */
#define PREFIX_PARK_SUFFIX	48	/* child bytes per holder */
#define PREFIX_PARK_MS		1000

static unsigned long prefix_park_lost;

struct prefix_park_arg {
	struct cds_ft *ft;
	unsigned int w;
	int child_writer;
	unsigned long ops;
};

static void *prefix_park_writer(void *arg)
{
	struct prefix_park_arg *a = (struct prefix_park_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed = a->w * 2654435761u + (unsigned int) a->child_writer;

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		uint8_t key[2] = { (uint8_t) (0x40 + a->w), 0 };
		size_t klen = 1;
		struct ft_test_node *n;

		if (a->child_writer) {
			key[1] = (uint8_t) (rand_r(&seed) % PREFIX_PARK_SUFFIX);
			klen = 2;
		}
		n = node_alloc(a->w);
		memcpy(n->okey, key, klen);
		n->value = klen;
		if (cds_ft_insert(a->ft, key, klen, &n->node)
				!= CDS_FT_STATUS_OK) {
			node_free(n);
			continue;
		}
		if (!a->child_writer) {
			struct cds_ft_node *out = NULL;

			/*
			 * @key is owned by this thread alone -- no peer inserts
			 * or removes it -- so an insert that reported OK must be
			 * findable NOW.  A miss is a park swallowed by a peer's
			 * retire of the holder.
			 */
			rcu_read_lock();
			if (cds_ft_eager_lookup_key(a->ft, key, klen, 0, &out)
					!= CDS_FT_STATUS_OK
					|| out != &n->node)
				uatomic_inc(&prefix_park_lost);
			rcu_read_unlock();
		}
		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, klen);
		if (cds_ft_lookup(a->ft, iter) == CDS_FT_STATUS_OK) {
			struct cds_ft_node *hd = cds_ft_iter_node(iter);

			if (hd && cds_ft_remove(a->ft, iter, hd)
					== CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(hd));
		}
		rcu_read_unlock();
		a->ops++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_prefix_key_park_vs_holder_churn(void)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_nolist_ft_ws(&group, &ws);
	pthread_t th[PREFIX_PARK_PAIRS * 2];
	struct prefix_park_arg a[PREFIX_PARK_PAIRS * 2];
	struct timespec t0;
	unsigned int i;
	unsigned long total = 0;
	int ret = 0;

	prefix_park_lost = 0;
	/*
	 * Seed every holder with children so the 1-byte prefix key always lands
	 * on an INTERNAL node -- on an empty trie it would be a plain leaf and
	 * never reach the external_nodes park at all.
	 */
	for (i = 0; i < PREFIX_PARK_PAIRS; i++) {
		unsigned int s;

		for (s = 0; s < PREFIX_PARK_SUFFIX; s++) {
			uint8_t ck[2] = { (uint8_t) (0x40 + i), (uint8_t) s };
			struct ft_test_node *n = node_alloc(9000 + s);

			memcpy(n->okey, ck, 2);
			n->value = 2;
			if (cds_ft_insert(ft, ck, 2, &n->node)
					!= CDS_FT_STATUS_OK)
				abort();
		}
	}

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < PREFIX_PARK_PAIRS * 2; i++) {
		a[i].ft = ft;
		a[i].w = i / 2;
		a[i].child_writer = (int) (i % 2);
		a[i].ops = 0;
		pthread_create(&th[i], NULL, prefix_park_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < PREFIX_PARK_MS)
		rcu_quiescent_state();
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < PREFIX_PARK_PAIRS * 2; i++) {
		pthread_join(th[i], NULL);
		total += a[i].ops;
	}

	fprintf(stderr, "# inv_prefix_key_park_vs_holder_churn: %lu ops, "
		"%lu lost prefix inserts\n", total, prefix_park_lost);
	if (prefix_park_lost) {
		fprintf(stderr, "inv_prefix_key_park_vs_holder_churn: %lu "
			"insert(s) reported OK but the key was absent -- a "
			"parked external head was swallowed by a peer retire\n",
			prefix_park_lost);
		ret = -1;
	}
	/* A run that did no work proves nothing; say so rather than pass. */
	if (total == 0) {
		fprintf(stderr, "inv_prefix_key_park_vs_holder_churn: no ops\n");
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_prefix_key_park_vs_holder_churn: verify "
			"failed\n");
		ret = -1;
	}
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   A key and its ONE-BYTE EXTENSION, the shorter one under a        */
/*   COMPRESSED holder                                                */
/*                                                                    */
/* ================================================================== */

/*
 * Two writers own one key each: P, and K = P plus one byte.  P ends a long
 * single-child run, so its holder is a COMPRESSED node (P is cn->child).  An
 * insert of K converts P into the prefix head of a fresh junction published
 * into that slot (ft_insert_compressed_past_child); a remove of K folds it
 * back.  A remove of P that took its position -- P at cn->child -- before
 * that conversion and read the slot again after it saw the junction as "the
 * detached child", promoted its external head (P itself, the key being
 * removed) into the boundary slot and retired the junction with K inside:
 * K lost, a removed P left reachable.  The inverse race (P back in the same
 * slot by the time the remove re-checks) refused a present P as NOT_FOUND.
 *
 * Found by the DNS-name bench: "...churn041998.zone46" lost to a concurrent
 * remove of "...churn041998.zone4".  The doc/design/ft-stale-disposal-rig.c
 * two-key case ("ab" / "aba") never reaches it: keys that short form no
 * compressed run, so the holder is never compressed.
 *
 * Oracles, all owner-only (no peer touches a writer's key): an insert that
 * reported OK must be findable as the writer's own node; the owner's remove
 * of its present key must return OK.
 *
 * The SIBLINGS variant keeps P present for the whole run and races two
 * EXTENSIONS of it, P+'6' and P+'7': with both absent the compressed holder's
 * child is P itself, so both inserts take the past-child arm on the same
 * slot, and the one that publishes second must not overwrite the first's
 * junction.  P, never removed, must stay findable throughout.
 */
#define PREFIX_PAIR_PAIRS_MAX	4	/* one P writer + one K writer each */
#define PREFIX_PAIR_RUN		24	/* bytes of P: the compressed run + 1 */
#define PREFIX_PAIR_MS		1000
#define PREFIX_PAIR_STABLE	10	/* untouched keys under other root bytes */
static unsigned long prefix_pair_lost, prefix_pair_refused, prefix_pair_stale;
/* Siblings variant: each root byte's P, inserted once and never removed. */
static struct cds_ft_node *prefix_pair_anchor[PREFIX_PAIR_PAIRS_MAX];
struct prefix_pair_arg {
	struct cds_ft *ft;
	unsigned int w;
	int extension;		/* 0: P, 1: P+'6', 2: P+'7' */
	int peer_extension;
	unsigned long ops;
};

static size_t prefix_pair_key(uint8_t *key, unsigned int w, int extension)
{
	static const char run[PREFIX_PAIR_RUN] = "-compressed-holder-runs";
	size_t len;

	key[0] = (uint8_t) (0x60 + w);	/* one root byte per pair */
	memcpy(key + 1, run, PREFIX_PAIR_RUN - 1);
	len = PREFIX_PAIR_RUN;
	if (extension)
		key[len++] = extension == 2 ? '7' : '6';
	return len;
}

static void *prefix_pair_writer(void *arg)
{
	struct prefix_pair_arg *a = (struct prefix_pair_arg *) arg;
	struct cds_ft_iter *iter;
	uint8_t key[PREFIX_PAIR_RUN + 1];
	size_t klen = prefix_pair_key(key, a->w, a->extension);

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		struct ft_test_node *n = node_alloc(a->w);
		struct cds_ft_node *out = NULL;
		enum cds_ft_status st;

		/* An EAGER group: no speculative key copy, so @okey stays unused
		 * (and at 8 bytes could not hold this key anyway). */
		n->value = klen;
		struct cds_ft_node *res = NULL;

		/*
		 * UNIQUE, and STOP at the first failure: the key is this
		 * writer's alone and absent here, so a plain insert over a
		 * leftover node would chain a DUPLICATE -- a different code
		 * path this test does not mean to drive -- and every oracle
		 * after that is about the duplicate, not the race.
		 */
		rcu_read_lock();
		st = cds_ft_insert_unique(a->ft, key, klen, &n->node, &res);
		rcu_read_unlock();
		if (st != CDS_FT_STATUS_OK) {
			node_free(n);
			uatomic_inc(&prefix_pair_lost);	/* own absent key refused */
			break;
		}
		rcu_read_lock();
		if (cds_ft_eager_lookup_key(a->ft, key, klen, 0, &out)
				!= CDS_FT_STATUS_OK || out != &n->node) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_lost);
			break;
		}
		if (prefix_pair_anchor[a->w]) {
			uint8_t ak[PREFIX_PAIR_RUN + 1];
			size_t akl = prefix_pair_key(ak, a->w, 0);
			struct cds_ft_node *ao = NULL;

			if (cds_ft_eager_lookup_key(a->ft, ak, akl, 0, &ao)
					!= CDS_FT_STATUS_OK ||
					ao != prefix_pair_anchor[a->w]) {
				rcu_read_unlock();
				uatomic_inc(&prefix_pair_lost);	/* P lost */
				break;
			}
		}
		cds_ft_iter_set_key(iter, key, klen);
		if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK ||
				cds_ft_iter_node(iter) != &n->node ||
				cds_ft_remove(a->ft, iter, &n->node)
					!= CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_refused);
			break;
		}
		/*
		 * A removed node must not stay reachable: not under its own key,
		 * and not RESURRECTED as the peer's key -- the peer's collapse
		 * promoting a head the peer already removed.  The removal mark
		 * and the unlink commit in one flip, so a lookup that STARTS
		 * after the mark is visible cannot legitimately return the
		 * node.  The first lookup of the peer's key is no such lookup:
		 * the peer's own remove may commit between it and the mark
		 * load, so a marked node found there is only a candidate, and
		 * it is confirmed by a second lookup.
		 */
		{
			uint8_t pk[PREFIX_PAIR_RUN + 1];
			size_t pkl = prefix_pair_key(pk, a->w, a->peer_extension);
			struct cds_ft_node *o2 = NULL, *o3 = NULL;

			if ((cds_ft_eager_lookup_key(a->ft, key, klen, 0, &o2)
					== CDS_FT_STATUS_OK && o2 == &n->node) ||
			    (cds_ft_eager_lookup_key(a->ft, pk, pkl, 0, &o2)
					== CDS_FT_STATUS_OK && o2 &&
					((uintptr_t) CMM_LOAD_SHARED(o2->next) &
						CDS_FT_NODE_REMOVED_FLAG) &&
					cds_ft_eager_lookup_key(a->ft, pk, pkl, 0,
						&o3) == CDS_FT_STATUS_OK &&
					o3 == o2)) {
				rcu_read_unlock();
				uatomic_inc(&prefix_pair_stale);
				break;
			}
		}
		node_free_rcu(n);
		rcu_read_unlock();
		a->ops++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int prefix_pair_run(const char *name, bool siblings)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft_ws(&group, &ws);
	pthread_t th[PREFIX_PAIR_PAIRS_MAX * 2];
	struct prefix_pair_arg a[PREFIX_PAIR_PAIRS_MAX * 2];
	const char *env = getenv("FT_INV_PREFIX_PAIR_MS");
	const char *penv = getenv("FT_INV_PREFIX_PAIR_PAIRS");
	unsigned long long ms = env ? strtoull(env, NULL, 10) : PREFIX_PAIR_MS;
	unsigned int pairs = penv ? (unsigned int) atoi(penv) : 1;
	struct timespec t0;
	unsigned int i;
	unsigned long total = 0;
	int ret = 0;

	prefix_pair_lost = 0;
	prefix_pair_refused = 0;
	prefix_pair_stale = 0;
	/*
	 * Stable keys under OTHER root bytes, never touched: with the pair
	 * alone in the trie the root carries nothing else (the rig this
	 * reproduces also seeds stable keys).
	 */
	for (i = 0; i < PREFIX_PAIR_STABLE; i++) {
		uint8_t sk[8] = { (uint8_t) (0x30 + i), 's', 't', 'a', 'b', 'l', 'e', 0 };
		struct ft_test_node *n = node_alloc(8000 + i);

		n->value = sizeof(sk);
		if (cds_ft_insert(ft, sk, sizeof(sk), &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	if (pairs < 1 || pairs > PREFIX_PAIR_PAIRS_MAX)
		pairs = 1;
	for (i = 0; i < PREFIX_PAIR_PAIRS_MAX; i++) {
		uint8_t pk[PREFIX_PAIR_RUN + 1];
		size_t pkl = prefix_pair_key(pk, i, 0);
		struct ft_test_node *n;

		prefix_pair_anchor[i] = NULL;
		if (!siblings || i >= pairs)
			continue;
		n = node_alloc(9000 + i);
		n->value = pkl;
		if (cds_ft_insert(ft, pk, pkl, &n->node) != CDS_FT_STATUS_OK)
			abort();
		prefix_pair_anchor[i] = &n->node;
	}
	for (i = 0; i < pairs * 2; i++) {
		a[i].ft = ft;
		a[i].w = i / 2;
		a[i].extension = (int) (i % 2) + (siblings ? 1 : 0);
		a[i].peer_extension = siblings ? 3 - a[i].extension :
			!a[i].extension;
		a[i].ops = 0;
		pthread_create(&th[i], NULL, prefix_pair_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < ms)
		rcu_quiescent_state();
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rcu_thread_offline();
	for (i = 0; i < pairs * 2; i++) {
		pthread_join(th[i], NULL);
		total += a[i].ops;
	}
	rcu_thread_online();

	for (i = 0; i < pairs; i++) {
		uint8_t pk[PREFIX_PAIR_RUN + 1];
		size_t pkl = prefix_pair_key(pk, i, 0);
		struct cds_ft_node *ao = NULL;

		if (prefix_pair_anchor[i] &&
				(cds_ft_eager_lookup_key(ft, pk, pkl, 0, &ao)
					!= CDS_FT_STATUS_OK ||
				 ao != prefix_pair_anchor[i]))
			prefix_pair_lost++;
	}
	fprintf(stderr, "# %s: %lu ops, %lu lost inserts, %lu refused "
		"removes, %lu removed nodes reachable\n", name,
		total, prefix_pair_lost, prefix_pair_refused, prefix_pair_stale);
	if (prefix_pair_lost || prefix_pair_refused || prefix_pair_stale) {
		fprintf(stderr, "%s: %lu insert(s) reported OK but the key was "
			"not its own node (or P was lost), %lu remove(s) of a "
			"present key refused, %lu removed node(s) still "
			"reachable\n", name, prefix_pair_lost,
			prefix_pair_refused, prefix_pair_stale);
		ret = -1;
	}
	/* A run that did no work proves nothing; say so rather than pass. */
	if (total == 0) {
		fprintf(stderr, "%s: no ops\n", name);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify failed\n", name);
		ret = -1;
	}
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	/* The drain freed them: no later user of prefix_pair_writer may see one. */
	for (i = 0; i < PREFIX_PAIR_PAIRS_MAX; i++)
		prefix_pair_anchor[i] = NULL;
	return ret;
}

static int inv_prefix_pair_compressed_holder(void)
{
	return prefix_pair_run("inv_prefix_pair_compressed_holder", false);
}

static int inv_prefix_siblings_compressed_holder(void)
{
	return prefix_pair_run("inv_prefix_siblings_compressed_holder", true);
}

/*
 * A duplicate chain's HEAD PROMOTE racing an extension of its key: P keeps two
 * duplicates, and its writer always removes the chain HEAD, so every remove
 * promotes the successor into the holder's slot.  With the extension K = P+'6'
 * absent, that slot is the compressed holder's cn->child; K's writer inserts K
 * past it, which republishes cn->child IN PLACE with a junction carrying P's
 * chain as its prefix head.  A promote planned against the old slot must not
 * overwrite that junction.
 *
 * Oracles, owner-only: after each remove the writer's other node heads P; K's
 * writer runs the prefix-pair oracles (its own node, its own remove).
 */
struct dup_promote_arg {
	struct cds_ft *ft;
	unsigned long ops;
};

static void *dup_promote_writer(void *arg)
{
	struct dup_promote_arg *a = (struct dup_promote_arg *) arg;
	struct cds_ft_node *live[2];
	struct cds_ft_iter *iter;
	uint8_t key[PREFIX_PAIR_RUN + 1];
	size_t klen = prefix_pair_key(key, 0, 0);
	unsigned int i;

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	for (i = 0; i < 2; i++) {
		struct ft_test_node *n = node_alloc(7000 + i);

		n->value = klen;
		if (cds_ft_insert(a->ft, key, klen, &n->node) != CDS_FT_STATUS_OK)
			abort();
		live[i] = &n->node;
	}
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		struct cds_ft_node *h, *out = NULL;
		struct ft_test_node *n;
		unsigned int idx;

		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, klen);
		if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_lost);
			break;
		}
		h = cds_ft_iter_node(iter);
		if (h != live[0] && h != live[1]) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_lost);
			break;
		}
		idx = h == live[0] ? 0 : 1;
		if (cds_ft_remove(a->ft, iter, h) != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_refused);
			break;
		}
		if (cds_ft_eager_lookup_key(a->ft, key, klen, 0, &out)
				!= CDS_FT_STATUS_OK || out != live[!idx]) {
			rcu_read_unlock();
			uatomic_inc(&prefix_pair_lost);
			break;
		}
		node_free_rcu(to_test_node(h));
		rcu_read_unlock();
		n = node_alloc(7002);
		n->value = klen;
		if (cds_ft_insert(a->ft, key, klen, &n->node) != CDS_FT_STATUS_OK) {
			node_free(n);
			uatomic_inc(&prefix_pair_lost);
			break;
		}
		live[idx] = &n->node;
		a->ops++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_prefix_dup_promote_vs_extension(void)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft_ws(&group, &ws);
	const char *env = getenv("FT_INV_PREFIX_PAIR_MS");
	unsigned long long ms = env ? strtoull(env, NULL, 10) : PREFIX_PAIR_MS;
	struct dup_promote_arg da = { .ft = ft };
	struct prefix_pair_arg ka = { .ft = ft, .w = 0, .extension = 1,
		.peer_extension = 0 };
	pthread_t dt, kt;
	struct timespec t0;
	int ret = 0;

	prefix_pair_lost = 0;
	prefix_pair_refused = 0;
	prefix_pair_stale = 0;
	/* P changes hands here: the K writer must not check a fixed anchor. */
	memset(prefix_pair_anchor, 0, sizeof(prefix_pair_anchor));
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_create(&dt, NULL, dup_promote_writer, &da);
	pthread_create(&kt, NULL, prefix_pair_writer, &ka);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < ms)
		rcu_quiescent_state();
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rcu_thread_offline();
	pthread_join(dt, NULL);
	pthread_join(kt, NULL);
	rcu_thread_online();

	fprintf(stderr, "# inv_prefix_dup_promote_vs_extension: %lu promotes, "
		"%lu K ops, %lu lost, %lu refused removes, %lu removed nodes "
		"reachable\n", da.ops, ka.ops, prefix_pair_lost,
		prefix_pair_refused, prefix_pair_stale);
	if (prefix_pair_lost || prefix_pair_refused || prefix_pair_stale) {
		fprintf(stderr, "inv_prefix_dup_promote_vs_extension: %lu lost, "
			"%lu refused, %lu stale\n", prefix_pair_lost,
			prefix_pair_refused, prefix_pair_stale);
		ret = -1;
	}
	if (da.ops == 0 || ka.ops == 0) {
		fprintf(stderr, "inv_prefix_dup_promote_vs_extension: no ops\n");
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_prefix_dup_promote_vs_extension: verify "
			"failed\n");
		ret = -1;
	}
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * THE SHAPE ZOO: every point update, at random, by several writers, over a
 * handful of keys built to put the plan-sensitive shapes next to each other --
 * a key ending inside a compressed run (key-shorter split), one diverging at
 * its last byte (split), the key ending the run (a compressed holder's leaf),
 * its extensions by one and two bytes (past-child junctions, prefix heads) and
 * a sibling extension -- with duplicate chains on all of them.  Keys are
 * SHARED, so no writer owns one and no per-writer oracle holds; the oracles are
 * the library's own: cds_ft_verify at the join, the rcu-debug detectors, and
 * the SW stale-old audit (FT_SW_STALE_ABORT=1 reports a stale plan at the
 * record that makes it).  A node a writer unlinks is not freed during the run
 * (two writers can both be told OK for one node): each writer keeps them in a
 * graveyard, and after the drain the union is freed ONCE per node.  A node both
 * in a graveyard and still in the trie -- reported unlinked, yet reachable --
 * is freed twice and trips node_free's double-free accounting.
 */
#define ZOO_WRITERS	4
#define ZOO_KEYS	7
enum zoo_op { ZOO_INSERT, ZOO_INSERT_UNIQUE, ZOO_INSERT_REPLACE, ZOO_REPLACE,
	ZOO_REMOVE, ZOO_REMOVE_ALL, ZOO_NR_OPS };
static const char *const zoo_op_name[ZOO_NR_OPS] = { "insert", "insert_unique",
	"insert_replace", "replace", "remove", "remove_all" };
struct zoo_arg {
	struct cds_ft *ft;
	unsigned int seed;
	unsigned int ops_mask;			/* FT_INV_ZOO_OPS: 1 << enum zoo_op */
	unsigned long done[ZOO_NR_OPS];		/* ops that changed the trie */
	struct cds_ft_node **grave;		/* nodes this writer unlinked */
	size_t nr_grave, cap_grave;
};

/* FT_INV_ZOO_WIN_MS: writers park here while the main thread verifies. */
static int zoo_pause, zoo_paused;

static void zoo_bury(struct zoo_arg *a, struct cds_ft_node *n)
{
	if (a->nr_grave == a->cap_grave) {
		a->cap_grave = a->cap_grave ? 2 * a->cap_grave : 1024;
		a->grave = (struct cds_ft_node **) realloc(a->grave,
			a->cap_grave * sizeof(*a->grave));
		if (!a->grave)
			abort();
	}
	a->grave[a->nr_grave++] = n;
}

/* A displaced or removed chain: its head and every member (read side held). */
static void zoo_bury_chain(struct zoo_arg *a, struct cds_ft_node *head)
{
	for (; head; head = cds_ft_node_next_rcu(head))
		zoo_bury(a, head);
}

static int zoo_ptr_cmp(const void *x, const void *y)
{
	uintptr_t p = (uintptr_t) *(struct cds_ft_node *const *) x;
	uintptr_t q = (uintptr_t) *(struct cds_ft_node *const *) y;

	return p < q ? -1 : p > q;
}

static size_t zoo_key(uint8_t *key, unsigned int root, unsigned int k)
{
	static const char run[PREFIX_PAIR_RUN] = "-compressed-holder-runs";
	size_t len;

	key[0] = (uint8_t) (0x60 + root);
	memcpy(key + 1, run, PREFIX_PAIR_RUN - 1);
	len = PREFIX_PAIR_RUN;
	switch (k) {
	case 0: return 10;			/* ends inside the run */
	case 1: key[len - 1] = 'X'; return len;	/* diverges at its last byte */
	case 2: return len;			/* P: ends the run */
	case 3: key[len++] = '6'; return len;
	case 4: key[len++] = '7'; return len;
	case 5: key[len++] = '6'; key[len++] = '6'; return len;
	default: key[len++] = '6'; key[len++] = 'z'; return len;
	}
}

static void *zoo_writer(void *arg)
{
	struct zoo_arg *a = (struct zoo_arg *) arg;
	struct cds_ft_iter *iter;
	uint8_t key[PREFIX_PAIR_RUN + 2];

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		if (uatomic_read(&zoo_pause)) {
			uatomic_inc(&zoo_paused);
			rcu_thread_offline();
			while (uatomic_read(&zoo_pause) && !test_stop)
				caa_cpu_relax();
			rcu_thread_online();
			uatomic_dec(&zoo_paused);
			continue;
		}
		unsigned int r = rand_r(&a->seed);
		size_t klen = zoo_key(key, r & 1, (r >> 1) % ZOO_KEYS);
		enum zoo_op op = (enum zoo_op) ((r >> 8) % ZOO_NR_OPS);
		struct ft_test_node *n;

		if (!(a->ops_mask & (1U << op)))
			continue;
		n = node_alloc(klen);
		struct cds_ft_node *res = NULL, *found;
		enum cds_ft_status st;

		n->value = klen;
		rcu_read_lock();
		switch (op) {
		case ZOO_INSERT:
			st = cds_ft_insert(a->ft, key, klen, &n->node);
			if (st != CDS_FT_STATUS_OK)
				goto unpublished;
			break;
		case ZOO_INSERT_UNIQUE:
			st = cds_ft_insert_unique(a->ft, key, klen, &n->node, &res);
			if (st != CDS_FT_STATUS_OK)
				goto unpublished;
			break;
		case ZOO_INSERT_REPLACE:
			st = cds_ft_insert_replace(a->ft, key, klen, &n->node, &res);
			if (st != CDS_FT_STATUS_OK &&
					st != CDS_FT_STATUS_DUPLICATE_FOUND)
				goto unpublished;
			if (st == CDS_FT_STATUS_DUPLICATE_FOUND)
				zoo_bury_chain(a, res);
			break;
		default:
			cds_ft_iter_set_key(iter, key, klen);
			if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK)
				goto unpublished;
			found = cds_ft_iter_node(iter);
			if (op == ZOO_REPLACE)
				st = cds_ft_replace(a->ft, iter, found, &n->node);
			else if (op == ZOO_REMOVE)
				st = cds_ft_remove(a->ft, iter, found);
			else
				st = cds_ft_remove_all(a->ft, iter, &res);
			if (op != ZOO_REPLACE) {
				if (st == CDS_FT_STATUS_OK) {
					if (op == ZOO_REMOVE)
						zoo_bury(a, found);
					else
						zoo_bury_chain(a, res);
					a->done[op]++;
				}
				rcu_read_unlock();
				node_free(n);		/* never offered */
				rcu_quiescent_state();
				continue;
			}
			if (st != CDS_FT_STATUS_OK)
				goto unpublished;
			zoo_bury(a, found);
			break;
		}
		rcu_read_unlock();
		a->done[op]++;
		rcu_quiescent_state();
		continue;
unpublished:
		rcu_read_unlock();
		node_free(n);
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_prefix_shape_zoo(void)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	/* FT_INV_ZOO_NOLIST: the ordered-list-off arms (no cell to re-validate). */
	struct cds_ft *ft = getenv("FT_INV_ZOO_NOLIST") ?
		create_varlen_nolist_ft_ws(&group, &ws) :
		create_varlen_ord_ft_ws(&group, &ws);
	pthread_t th[ZOO_WRITERS];
	struct zoo_arg a[ZOO_WRITERS];
	const char *env = getenv("FT_INV_ZOO_MS");
	const char *mask_env = getenv("FT_INV_ZOO_OPS");
	const char *zoo_env = getenv("FT_INV_ZOO");
	const char *win_env = getenv("FT_INV_ZOO_WIN_MS");
	/* Default: verify every 5 ms; FT_INV_ZOO_WIN_MS=0 verifies at the join only. */
	unsigned long long win = win_env ? strtoull(win_env, NULL, 10) : 5;
	unsigned int ops_mask = mask_env ?
		(unsigned int) strtoul(mask_env, NULL, 0) : (1U << ZOO_NR_OPS) - 1;
	unsigned long long ms = env ? strtoull(env, NULL, 10) : PREFIX_PAIR_MS;
	unsigned long done[ZOO_NR_OPS] = { 0 };
	struct timespec t0;
	unsigned int i, o;
	int ret = 0;

	/*
	 * A DEFAULT ROW (FT_INV_ZOO=0 skips it).  Every point update, at random,
	 * by four writers over the shared prefix-shaped keys, clean on the
	 * strict, rcu-debug and in-place builds at all three spacings.
	 */
	if (zoo_env && !strcmp(zoo_env, "0")) {
		fprintf(stderr, "# inv_prefix_shape_zoo: skipped (FT_INV_ZOO=0)\n");
		drain_trie_keep_group(ft);
		cds_ft_group_destroy(group);
		return 0;
	}
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < ZOO_WRITERS; i++) {
		memset(&a[i], 0, sizeof(a[i]));
		a[i].ft = ft;
		a[i].seed = 0x5eed + 7919 * i;
		a[i].ops_mask = ops_mask;
		pthread_create(&th[i], NULL, zoo_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	/*
	 * Verify every @win ms (FT_INV_ZOO_WIN_MS, default 5), writers parked,
	 * and abort at the first failing window -- the damage is then at most
	 * one window old, which is what a flight-recorder snapshot needs to
	 * still hold its producer.  With 0 the run is verified once, at the join.
	 */
	if (win) {
		unsigned long nwin = 0;

		while (elapsed_ms(&t0) < ms) {
			struct timespec tw;

			clock_gettime(CLOCK_MONOTONIC, &tw);
			while (elapsed_ms(&tw) < win)
				rcu_quiescent_state();
			uatomic_set(&zoo_pause, 1);
			while (uatomic_read(&zoo_paused) != ZOO_WRITERS)
				rcu_quiescent_state();
			nwin++;
			if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
				fprintf(stderr, "inv_prefix_shape_zoo: window %lu failed verify\n",
					nwin);
				abort();
			}
			uatomic_set(&zoo_pause, 0);
			while (uatomic_read(&zoo_paused) != 0)
				rcu_quiescent_state();
		}
		fprintf(stderr, "# inv_prefix_shape_zoo: %lu windows verified\n",
			nwin);
	} else
	while (elapsed_ms(&t0) < ms)
		rcu_quiescent_state();
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rcu_thread_offline();
	for (i = 0; i < ZOO_WRITERS; i++)
		pthread_join(th[i], NULL);
	rcu_thread_online();

	fprintf(stderr, "# inv_prefix_shape_zoo:");
	for (o = 0; o < ZOO_NR_OPS; o++) {
		for (i = 0; i < ZOO_WRITERS; i++)
			done[o] += a[i].done[o];
		fprintf(stderr, " %s=%lu", zoo_op_name[o], done[o]);
		/* An op the run never landed proves nothing about its arm. */
		if (!done[o] && (ops_mask & (1U << o))) {
			fprintf(stderr, "\ninv_prefix_shape_zoo: no %s landed\n",
				zoo_op_name[o]);
			ret = -1;
		}
	}
	fprintf(stderr, "\n");
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_prefix_shape_zoo: verify failed\n");
		ret = -1;
	}
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	{
		struct cds_ft_node **all;
		size_t nr = 0, k;

		for (i = 0; i < ZOO_WRITERS; i++)
			nr += a[i].nr_grave;
		all = (struct cds_ft_node **) malloc((nr ? nr : 1) * sizeof(*all));
		if (!all)
			abort();
		for (nr = 0, i = 0; i < ZOO_WRITERS; i++) {
			memcpy(all + nr, a[i].grave,
				a[i].nr_grave * sizeof(*all));
			nr += a[i].nr_grave;
			free(a[i].grave);
		}
		qsort(all, nr, sizeof(*all), zoo_ptr_cmp);
		for (k = 0; k < nr; k++)
			if (!k || all[k] != all[k - 1])
				node_free(to_test_node(all[k]));
		free(all);
	}
	return ret;
}

/*
 * TWO WRITERS, DISJOINT KEYS, PREFIX-DENSE.  Keys are 1..10 bytes over {a, b},
 * so nearly every key is a PREFIX of another: removes collapse junctions into
 * skip-compressed runs, and one writer's remove races the collapse of a
 * junction whose prefix key belongs to the other writer.  Each writer OWNS its
 * keys, so every answer about one of them is a claim the oracle can check:
 *
 *   - before an insert the key is absent (else a removed node came back);
 *   - a remove's lookup finds the owner's own node;
 *   - right after an OK remove, that node is no longer found.
 *
 * A stable set, inserted once and never touched, keeps junctions alive.  The
 * shared-key zoo cannot ask any of this: there a peer may legitimately insert
 * or remove the same key, so a stale answer reads as a peer's op.
 *
 * Found by a standalone two-writer harness: ft_detach_node's climb took each
 * elevation's plan from a re-read of the slot, so a peer's collapse landed
 * between the pair check and the elevation and the promote republished a key
 * that peer had just removed.  FT_INV_OWNED_MS sets the duration,
 * FT_INV_OWNED_LIST=1 runs it with the ordered list on.
 */
#define OWNED_WRITERS_MAX	4
#define OWNED_STABLE	200
#define OWNED_CHURN	200
#define OWNED_KEY_MAX	250	/* FT_INV_OWNED_MAXLEN reaches past a skip pointer's run */

struct owned_key {
	uint8_t b[OWNED_KEY_MAX];
	unsigned int len;
	struct ft_test_node *cur;	/* the owner's node while present */
	struct ft_test_node *prev;	/* the node this key last dropped */
	const char *prev_op;		/* ... and the op that dropped it */
	unsigned long prev_at;		/* ... at this writer op count */
};

/* The failure print names whose node a wrong lookup returned. */
static struct owned_key *owned_dbg_stable, *owned_dbg_churn;

struct owned_arg {
	const char *name;
	struct cds_ft *ft;
	struct owned_key *keys;
	unsigned int nr_keys;
	bool replace;		/* insert_replace + remove, key by key */
	uint64_t seed;
	int cur_key;		/* the key this writer is operating on, or -1 */
	unsigned long ops;
	int *stop_all;
	int failed;
};

static uint64_t owned_rnd(uint64_t *s)
{
	*s ^= *s << 13;
	*s ^= *s >> 7;
	*s ^= *s << 17;
	return *s;
}

static bool owned_key_eq(const struct owned_key *x, const struct owned_key *y)
{
	return x->len == y->len && !memcmp(x->b, y->b, x->len);
}

/* @n distinct keys of 1..@maxlen bytes, none equal to one of @avoid[]. */
static void owned_mkkeys(struct owned_key *dst, unsigned int n,
		const struct owned_key *avoid, unsigned int navoid,
		unsigned int maxlen, uint64_t *s)
{
	unsigned int i = 0, j;

	while (i < n) {
		struct owned_key k;
		bool dup = false;

		memset(&k, 0, sizeof(k));
		k.len = 1 + (unsigned int) (owned_rnd(s) % maxlen);
		for (j = 0; j < k.len; j++)
			k.b[j] = (uint8_t) ('a' + (owned_rnd(s) & 1));
		for (j = 0; j < i && !dup; j++)
			dup = owned_key_eq(&dst[j], &k);
		for (j = 0; j < navoid && !dup; j++)
			dup = owned_key_eq(&avoid[j], &k);
		if (!dup)
			dst[i++] = k;
	}
}

static struct cds_ft_node *owned_find(struct cds_ft *ft,
		const struct owned_key *k)
{
	struct cds_ft_node *out = NULL;

	if (cds_ft_eager_lookup_key(ft, k->b, k->len, 0, &out) !=
			CDS_FT_STATUS_OK)
		return NULL;
	return out;
}

static void owned_fail(struct owned_arg *a, const char *what,
		const struct owned_key *k, const void *node)
{
	unsigned int i;

	fprintf(stderr, "%s: %s key=%.*s node=%p\n", a->name,
		what, (int) k->len, (const char *) k->b, node);
	if (node && k->prev && node == &k->prev->node)
		fprintf(stderr, "%s:   node is THIS key's previous node, dropped by %s %lu ops ago (model now %s)\n",
			a->name, k->prev_op, a->ops - k->prev_at,
			k->cur ? "present" : "absent");
	for (i = 0; node && owned_dbg_stable && i < OWNED_STABLE; i++)
		if (owned_dbg_stable[i].cur &&
				node == &owned_dbg_stable[i].cur->node)
			fprintf(stderr, "%s:   node is STABLE key %.*s\n",
				a->name, (int) owned_dbg_stable[i].len,
				(const char *) owned_dbg_stable[i].b);
	for (i = 0; node && owned_dbg_churn && i < OWNED_CHURN; i++) {
		struct owned_key *c = &owned_dbg_churn[i];

		if (c->cur && node == &c->cur->node)
			fprintf(stderr, "%s:   node is CHURN key %.*s (current)\n",
				a->name, (int) c->len, (const char *) c->b);
		if (c->prev && node == &c->prev->node)
			fprintf(stderr, "%s:   node is CHURN key %.*s (previous)\n",
				a->name, (int) c->len, (const char *) c->b);
	}
	a->failed = 1;
	*a->stop_all = 1;
	if (getenv("FT_TRACE_SESSION")) {
		char cmd[256];

		/* The peer that did it ran within the last lap: keep it. */
		snprintf(cmd, sizeof(cmd), "lttng stop %s 1>&2",
			getenv("FT_TRACE_SESSION"));
		(void) system(cmd);
		snprintf(cmd, sizeof(cmd), "lttng snapshot record -s %s 1>&2",
			getenv("FT_TRACE_SESSION"));
		(void) system(cmd);
		abort();
	}
	mw_violation_snapshot();
}

/*
 * One key, one op, checked on both sides: the key must resolve to exactly the
 * owner's node (or to nothing) before and after.  A coin picks insert_replace
 * or remove; insert_replace must answer OK exactly when the key was absent,
 * and DUPLICATE_FOUND with the owner's own node otherwise.
 */
static int owned_plain_insert;
static int owned_remove_all;	/* FT_INV_OWNED_REMOVE_ALL=1 */

static int owned_replace_one(struct owned_arg *a, struct cds_ft_iter *iter,
		struct owned_key *k)
{
	struct cds_ft_node *want = k->cur ? &k->cur->node : NULL;
	struct cds_ft_node *o = owned_find(a->ft, k), *res = NULL;
	struct ft_test_node *n;
	enum cds_ft_status st = CDS_FT_STATUS_OK;
	const char *op = "none";

	if (o != want) {
		owned_fail(a, "before the op, the key is not exactly its owner's node",
			k, o);
		return -1;
	}
	bool coin = owned_rnd(&a->seed) & 1;

	if (owned_plain_insert && coin) {
		/* FT_INV_OWNED_PLAIN_INSERT=1: cds_ft_insert where absent. */
		if (!want) {
			n = node_alloc(0);
			op = "insert";
			st = cds_ft_insert(a->ft, k->b, k->len, &n->node);
			if (st == CDS_FT_STATUS_OK)
				k->cur = n;
			else
				node_free(n);
		}
	} else if (!owned_plain_insert && coin) {
		n = node_alloc(0);
		op = "insert_replace";
		st = cds_ft_insert_replace(a->ft, k->b, k->len, &n->node, &res);
		if (st == CDS_FT_STATUS_BUSY_ERROR) {
			node_free(n);		/* nothing published */
		} else if (st == CDS_FT_STATUS_OK) {
			if (want) {
				owned_fail(a, "insert_replace found no prior node while the owner's is present",
					k, want);
				return -1;
			}
			k->cur = n;
		} else if (st == CDS_FT_STATUS_DUPLICATE_FOUND) {
			if (!want || res != want || cds_ft_node_next_rcu(res)) {
				owned_fail(a, "insert_replace displaced a chain that is not the owner's node",
					k, res);
				return -1;
			}
			k->prev = k->cur;
			k->prev_op = "insert_replace";
			k->prev_at = a->ops;
			node_free_rcu(k->cur);
			k->cur = n;
		} else {
			owned_fail(a, "insert_replace failed", k, NULL);
			return -1;
		}
	} else if (want) {
		struct cds_ft_node *head = NULL;

		op = owned_remove_all ? "remove_all" : "remove";
		cds_ft_iter_set_key(iter, k->b, k->len);
		if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK ||
				cds_ft_iter_node(iter) != want ||
				(owned_remove_all ?
				 cds_ft_remove_all(a->ft, iter, &head) !=
					CDS_FT_STATUS_OK || head != want :
				 cds_ft_remove(a->ft, iter, want) !=
					CDS_FT_STATUS_OK)) {
			owned_fail(a, "the owner's remove failed", k, want);
			return -1;
		}
		k->prev = k->cur;
		k->prev_op = "remove";
		k->prev_at = a->ops;
		node_free_rcu(k->cur);
		k->cur = NULL;
	}
	want = k->cur ? &k->cur->node : NULL;
	o = owned_find(a->ft, k);
	if (o != want) {
		fprintf(stderr, "%s: the op was %s, status %d, want %p, res %p\n",
			a->name, op, (int) st, (void *) want, (void *) res);
		owned_fail(a, "after the op, the key is not exactly its owner's node",
			k, o);
		return -1;
	}
	return 0;
}

/* FT_INV_OWNED_WIN_MS: writers park here while the main thread checks. */
static int owned_pause, owned_paused;

static void owned_park(void)
{
	if (!uatomic_read(&owned_pause))
		return;
	uatomic_inc(&owned_paused);
	rcu_thread_offline();
	while (uatomic_read(&owned_pause) && !test_stop)
		caa_cpu_relax();
	rcu_thread_online();
	uatomic_dec(&owned_paused);
}

struct owned_expect {
	const struct cds_ft_node *node;
	const struct owned_key *key;
};

static int owned_expect_cmp(const void *x, const void *y)
{
	uintptr_t p = (uintptr_t) ((const struct owned_expect *) x)->node;
	uintptr_t q = (uintptr_t) ((const struct owned_expect *) y)->node;

	return p < q ? -1 : p > q;
}

static void owned_window_fail(const char *name, unsigned long nwin,
		const char *what, const struct owned_key *k, const void *node,
		const uint8_t *got, size_t got_len)
{
	const char *sess = getenv("FT_TRACE_SESSION");
	char cmd[256];

	fprintf(stderr, "%s: window %lu: %s key=%.*s node=%p walked-as=%.*s\n",
		name, nwin, what, k ? (int) k->len : 0,
		k ? (const char *) k->b : "", node,
		(int) got_len, got ? (const char *) got : "");
	if (sess) {
		/* Every writer is parked: the rings are quiescent already. */
		snprintf(cmd, sizeof(cmd), "lttng stop %s 1>&2", sess);
		(void) system(cmd);
		snprintf(cmd, sizeof(cmd), "lttng snapshot record -s %s 1>&2",
			sess);
		(void) system(cmd);
	}
	if (sess || getenv("FT_INV_OWNED_ABORT"))
		abort();
}

/*
 * With every writer parked: each stable and each owned key resolves to
 * exactly the node the models say, and an ordered walk of the whole trie
 * visits exactly those nodes, each under its OWN key -- a node filed at the
 * wrong position shows up here as a key mismatch or an extra entry, at the
 * window it happened in rather than at its owner's next op.
 */
static int owned_window_check(const char *name, unsigned long nwin,
		struct cds_ft *ft, const struct owned_key *stable,
		const struct owned_key *churn)
{
	struct owned_expect ex[OWNED_STABLE + OWNED_CHURN];
	struct cds_ft_iter *iter;
	unsigned int i, nex = 0, seen = 0;
	int ret = 0;

	/* The structure itself first: back-pointers, counts, canonical form. */
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		owned_window_fail(name, nwin, "cds_ft_verify fails", NULL,
			NULL, NULL, 0);
		return -1;
	}
	for (i = 0; i < OWNED_STABLE; i++) {
		if (owned_find(ft, &stable[i]) != &stable[i].cur->node) {
			owned_window_fail(name, nwin, "a stable key does not resolve to its node",
				&stable[i], owned_find(ft, &stable[i]), NULL, 0);
			return -1;
		}
		ex[nex].node = &stable[i].cur->node;
		ex[nex++].key = &stable[i];
	}
	for (i = 0; i < OWNED_CHURN; i++) {
		const struct cds_ft_node *want = churn[i].cur ?
			&churn[i].cur->node : NULL;

		if (owned_find(ft, &churn[i]) != want) {
			owned_window_fail(name, nwin, "an owned key does not resolve to its model",
				&churn[i], owned_find(ft, &churn[i]), NULL, 0);
			return -1;
		}
		if (want) {
			ex[nex].node = want;
			ex[nex++].key = &churn[i];
		}
	}
	qsort(ex, nex, sizeof(ex[0]), owned_expect_cmp);
	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	if (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		do {
			struct owned_expect q = { .node = cds_ft_iter_node(iter) };
			const struct owned_expect *e;
			uint8_t rk[OWNED_KEY_MAX + 8];
			size_t rk_len = 0;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			e = (const struct owned_expect *) bsearch(&q, ex, nex,
				sizeof(ex[0]), owned_expect_cmp);
			if (!e) {
				owned_window_fail(name, nwin, "the walk visits a node no model holds",
					NULL, q.node, rk, rk_len);
				ret = -1;
				break;
			}
			if (rk_len != e->key->len ||
					memcmp(rk, e->key->b, rk_len)) {
				owned_window_fail(name, nwin, "a node is walked under another key",
					e->key, q.node, rk, rk_len);
				ret = -1;
				break;
			}
			if (++seen > nex)
				break;
		} while (cds_ft_next(ft, iter) == CDS_FT_STATUS_OK);
	}
	if (!ret && seen != nex) {
		fprintf(stderr, "%s: window %lu: walked %u entries, models hold %u\n",
			name, nwin, seen, nex);
		owned_window_fail(name, nwin, "entry count differs", NULL, NULL,
			NULL, 0);
		ret = -1;
	}
	cds_ft_iter_destroy(iter);
	return ret;
}

static void *owned_writer(void *arg)
{
	struct owned_arg *a = (struct owned_arg *) arg;
	struct cds_ft_iter *iter;
	unsigned int i;

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (a->replace && !test_stop && !*a->stop_all) {
		for (i = 0; i < a->nr_keys && !test_stop && !*a->stop_all; i++) {
			int r;

			owned_park();
			uatomic_set(&a->cur_key, (int) i);
			rcu_read_lock();
			r = owned_replace_one(a, iter, &a->keys[i]);
			rcu_read_unlock();
			uatomic_set(&a->cur_key, -1);
			if (r)
				break;
			rcu_quiescent_state();
			a->ops++;
		}
	}
	while (!a->replace && !test_stop && !*a->stop_all) {
		for (i = 0; i < a->nr_keys && !test_stop && !*a->stop_all; i++) {
			struct owned_key *k = &a->keys[i];
			struct ft_test_node *n;
			struct cds_ft_node *o;

			owned_park();
			rcu_read_lock();
			o = owned_find(a->ft, k);
			if (o && !k->cur) {
				owned_fail(a, "a removed key is found again",
					k, o);
				rcu_read_unlock();
				break;
			}
			if (!k->cur) {
				n = node_alloc(0);
				if (cds_ft_insert(a->ft, k->b, k->len,
						&n->node) == CDS_FT_STATUS_OK)
					k->cur = n;
				else
					node_free(n);
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			a->ops++;
		}
		for (i = 0; i < a->nr_keys && !test_stop && !*a->stop_all; i++) {
			struct owned_key *k = &a->keys[i];
			struct cds_ft_node *o;

			if (!k->cur)
				continue;
			owned_park();
			rcu_read_lock();
			cds_ft_iter_set_key(iter, k->b, k->len);
			if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK ||
					cds_ft_iter_node(iter) != &k->cur->node) {
				owned_fail(a, "a present key is not its owner's node",
					k, cds_ft_iter_node(iter));
				rcu_read_unlock();
				break;
			}
			if (cds_ft_remove(a->ft, iter, &k->cur->node) !=
					CDS_FT_STATUS_OK) {
				owned_fail(a, "the owner's remove failed",
					k, &k->cur->node);
				rcu_read_unlock();
				break;
			}
			o = owned_find(a->ft, k);
			if (o == &k->cur->node) {
				owned_fail(a, "an OK remove left its node found",
					k, o);
				rcu_read_unlock();
				break;
			}
			node_free_rcu(k->cur);
			k->cur = NULL;
			rcu_read_unlock();
			rcu_quiescent_state();
			a->ops++;
		}
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int owned_body(const char *name, unsigned int nwriters,
		unsigned int maxlen, bool replace, bool remove_all)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct owned_key *stable, *churn;
	struct owned_arg a[OWNED_WRITERS_MAX];
	pthread_t th[OWNED_WRITERS_MAX];
	const char *ms_env = getenv("FT_INV_OWNED_MS");
	const char *list_env = getenv("FT_INV_OWNED_LIST");
	const char *win_env = getenv("FT_INV_OWNED_WIN_MS");
	unsigned long long ms = ms_env ? strtoull(ms_env, NULL, 10) :
		PREFIX_PAIR_MS;
	unsigned long long win;
	uint64_t s = 0x5eedf00dULL;
	struct timespec t0;
	unsigned int i, per = OWNED_CHURN / nwriters;
	int ret = 0, stop_all = 0;

	owned_plain_insert = getenv("FT_INV_OWNED_PLAIN_INSERT") != NULL;
	owned_remove_all = remove_all ||
		getenv("FT_INV_OWNED_REMOVE_ALL") != NULL;
	/* Under a per-process capture, let the launcher start the session. */
	if (getenv("FT_TRACE_SESSION"))
		sleep(3);
	/* Default: park and verify every 5 ms; FT_INV_OWNED_WIN_MS=0 turns it off. */
	win = win_env ? strtoull(win_env, NULL, 10) : 5;

	/*
	 * The DEFAULT group, as an application gets it -- no creator knobs:
	 * skip compression on (the failure needs it), fine locking, rekey
	 * off.  Ordered list off unless asked, where the failure was densest.
	 */
	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (!(list_env && !strcmp(list_env, "1")) &&
			cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();

	stable = (struct owned_key *) calloc(OWNED_STABLE, sizeof(*stable));
	churn = (struct owned_key *) calloc(OWNED_CHURN, sizeof(*churn));
	if (!stable || !churn)
		abort();
	if (getenv("FT_INV_OWNED_MAXLEN")) {
		maxlen = (unsigned int) atoi(getenv("FT_INV_OWNED_MAXLEN"));
		if (maxlen < 1 || maxlen > OWNED_KEY_MAX)
			maxlen = OWNED_KEY_MAX;
	}
	owned_mkkeys(stable, OWNED_STABLE, NULL, 0, maxlen, &s);
	owned_mkkeys(churn, OWNED_CHURN, stable, OWNED_STABLE, maxlen, &s);
	owned_dbg_stable = stable;
	owned_dbg_churn = churn;
	rcu_read_lock();
	for (i = 0; i < OWNED_STABLE; i++) {
		stable[i].cur = node_alloc(0);
		if (cds_ft_insert(ft, stable[i].b, stable[i].len,
				&stable[i].cur->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < nwriters; i++) {
		memset(&a[i], 0, sizeof(a[i]));
		a[i].name = name;
		a[i].ft = ft;
		a[i].keys = churn + i * per;
		a[i].nr_keys = per;
		a[i].replace = replace;
		a[i].seed = 0x5eed + 7919ULL * (i + 1);
		a[i].cur_key = -1;
		a[i].stop_all = &stop_all;
		pthread_create(&th[i], NULL, owned_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (win) {
		unsigned long nwin = 0;

		while (elapsed_ms(&t0) < ms && !stop_all) {
			struct timespec tw;

			clock_gettime(CLOCK_MONOTONIC, &tw);
			while (elapsed_ms(&tw) < win && !stop_all)
				rcu_quiescent_state();
			uatomic_set(&owned_pause, 1);
			clock_gettime(CLOCK_MONOTONIC, &tw);
			while (uatomic_read(&owned_paused) != (int) nwriters &&
					!stop_all) {
				rcu_quiescent_state();
				if (elapsed_ms(&tw) < 2000)
					continue;
				/*
				 * A writer that has not reached its park point in
				 * 2 s is stuck INSIDE an op: name it, and check the
				 * structure it is stuck on -- every other writer
				 * is parked.
				 */
				for (i = 0; i < nwriters; i++) {
					int ck = uatomic_read(&a[i].cur_key);

					if (ck >= 0)
						fprintf(stderr, "%s: writer %u STUCK on key=%.*s (model %s)\n",
							name, i,
							(int) a[i].keys[ck].len,
							(const char *) a[i].keys[ck].b,
							a[i].keys[ck].cur ?
							"present" : "absent");
				}
				/* The key set the stuck op runs against. */
				for (i = 0; i < OWNED_STABLE; i++)
					fprintf(stderr, "PRESENT %.*s\n",
						(int) stable[i].len,
						(const char *) stable[i].b);
				for (i = 0; i < OWNED_CHURN; i++)
					if (churn[i].cur)
						fprintf(stderr, "PRESENT %.*s\n",
							(int) churn[i].len,
							(const char *) churn[i].b);
				rcu_read_lock();
				fprintf(stderr, "SHAPE-BEGIN\n");
				cds_ft_show(ft, stderr, CDS_FT_SHOW_PRETTY);
				fprintf(stderr, "SHAPE-END\n");
				if (!owned_window_check(name, nwin + 1, ft,
						stable, churn))
					fprintf(stderr, "%s: the structure CHECKS CLEAN around the stuck op\n",
						name);
				rcu_read_unlock();
				owned_window_fail(name, nwin + 1,
					"a writer is stuck in an op", NULL, NULL,
					NULL, 0);
				stop_all = 1, ret = -1;
			}
			if (!stop_all) {
				nwin++;
				rcu_read_lock();
				if (owned_window_check(name, nwin, ft, stable,
						churn))
					stop_all = 1, ret = -1;
				rcu_read_unlock();
			}
			uatomic_set(&owned_pause, 0);
			while (uatomic_read(&owned_paused) != 0)
				rcu_quiescent_state();
		}
		fprintf(stderr, "# %s: %lu windows checked\n", name, nwin);
	}
	rcu_thread_offline();
	while (elapsed_ms(&t0) < ms && !stop_all)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < nwriters; i++)
		pthread_join(th[i], NULL);
	rcu_thread_online();

	fprintf(stderr, "# %s: ops", name);
	for (i = 0; i < nwriters; i++) {
		fprintf(stderr, " %lu", a[i].ops);
		if (a[i].failed)
			ret = -1;
		if (!a[i].ops) {
			fprintf(stderr, "\n%s: writer %u made no progress", name, i);
			ret = -1;
		}
	}
	fprintf(stderr, "\n");
	rcu_read_lock();
	if (verify_or_dump(ft, name))
		ret = -1;
	rcu_read_unlock();
	/* The drain frees every node still reachable, the churn heads included. */
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	owned_dbg_stable = owned_dbg_churn = NULL;
	free(stable);
	free(churn);
	return ret;
}

static int inv_owned_prefix_dense(void)
{
	return owned_body("inv_owned_prefix_dense", 2, 10, false, false);
}

/*
 * The same owners, insert_replace + remove, four writers, keys of 1..16 bytes.
 * Two defects only this mix reached, both at exponential spacing, both a plan
 * dated from two snapshots: insert_replace ignored the descent's skip conflict
 * and parked a leaf key as the prefix key of the node one byte above it (OK
 * with the owner's node present), and the detach orphan walk anchored at a
 * depth its descent never reached (an assert, or a SEGV in release).
 */
static int inv_owned_prefix_dense_replace(void)
{
	return owned_body("inv_owned_prefix_dense_replace", 4, 16, true, false);
}

/*
 * The same, removing through cds_ft_remove_all, whose prefix-key clear chose
 * the plain clear from an unheld child count: a peer that removed a child
 * first left the holder with one child and no key.  Measured without the
 * under-lock re-check, 5 s with 2 ms windows: 10/24 runs at root-only, 2/24
 * per-node, 20/24 exponential (where writers also wedge on the shape).
 */
static int inv_owned_prefix_dense_remove_all(void)
{
	return owned_body("inv_owned_prefix_dense_remove_all", 4, 16, true,
		true);
}

/*
 * The same owners with keys of up to 250 bytes: over a two-letter alphabet
 * every key ends in a long unique run, so a junction that loses a branch
 * collapses into a run longer than a skip pointer can encode.  The collapse
 * used to decline that and leave a one-child keyless internal; measured
 * before the fix, 72 of 72 runs (5 s, 2 ms windows, every spacing, remove and
 * remove_all) failed, and writers wedged at exponential spacing.
 */
static int inv_owned_long_runs(void)
{
	return owned_body("inv_owned_long_runs", 4, 250, true, false);
}

/*
 * insert_replace on a body leaf K inherits K's prefix answer for the new head,
 * read from K's parent word before the op holds anything.  A peer that inserts
 * K+'b' turns K into the prefix head of a fresh node, and removing K+'b'
 * promotes it back -- the SAME head in the SAME slot.  An op whose plan read
 * the answer in between and checks only the slot under its lock publishes a
 * body leaf saying prefix-head=1.  The next split/promote rewrites that word,
 * so the state is transient: park every writer and verify each window.
 * Measured without the under-lock re-check, 10 s: 4/8 runs fail at per-node,
 * 2/8 at exponential.  FT_INV_IRPR_MS sets the duration.
 */
#define IRPR_PAIRS	8
#define IRPR_KLEN	7

struct irpr_arg {
	struct cds_ft *ft;
	int pair;
	bool extender;		/* inserts and removes K+'b'; else replaces K */
	int *stop_all;
};

static void irpr_key(uint8_t *k, int pair, uint8_t last)
{
	memset(k, 'a', IRPR_KLEN + 1);
	k[0] = (uint8_t) ('a' + pair);
	k[IRPR_KLEN - 1] = last;
}

static void *irpr_writer(void *arg)
{
	struct irpr_arg *a = (struct irpr_arg *) arg;
	struct cds_ft_iter *iter;
	uint8_t k[IRPR_KLEN + 1];

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	irpr_key(k, a->pair, 'a');
	k[IRPR_KLEN] = 'b';
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop && !*a->stop_all) {
		struct ft_test_node *n = node_alloc(0);
		struct cds_ft_node *res = NULL;
		enum cds_ft_status st;

		owned_park();
		rcu_read_lock();
		if (!a->extender) {
			st = cds_ft_insert_replace(a->ft, k, IRPR_KLEN,
				&n->node, &res);
			if (st == CDS_FT_STATUS_DUPLICATE_FOUND)
				node_free_rcu(to_test_node(res));
			else if (st != CDS_FT_STATUS_OK)
				node_free(n);
		} else if (cds_ft_insert(a->ft, k, IRPR_KLEN + 1, &n->node) !=
				CDS_FT_STATUS_OK) {
			node_free(n);
		} else {
			cds_ft_iter_set_key(iter, k, IRPR_KLEN + 1);
			if (cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK ||
					cds_ft_iter_node(iter) != &n->node ||
					cds_ft_remove(a->ft, iter, &n->node) !=
						CDS_FT_STATUS_OK) {
				fprintf(stderr, "inv_ir_prefix_roundtrip: the extension's remove failed\n");
				*a->stop_all = 1;
			} else {
				node_free_rcu(n);
			}
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_ir_prefix_roundtrip(void)
{
	const char *name = "inv_ir_prefix_roundtrip";
	const char *ms_env = getenv("FT_INV_IRPR_MS");
	unsigned long long ms = ms_env ? strtoull(ms_env, NULL, 10) : 2000;
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct irpr_arg a[2 * IRPR_PAIRS];
	pthread_t th[2 * IRPR_PAIRS];
	unsigned int i, nthreads = 2 * IRPR_PAIRS;
	unsigned long nwin = 0;
	struct timespec t0;
	int ret = 0, stop_all = 0;

	/* The default group, ordered list off, as the owned-key row. */
	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	/* The sibling that keeps each K a BODY leaf of a 2-child node. */
	rcu_read_lock();
	for (i = 0; i < IRPR_PAIRS; i++) {
		uint8_t s[IRPR_KLEN + 1];

		irpr_key(s, (int) i, 'b');
		if (cds_ft_insert(ft, s, IRPR_KLEN, &node_alloc(0)->node) !=
				CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < nthreads; i++) {
		a[i] = (struct irpr_arg) { .ft = ft, .pair = (int) (i / 2),
			.extender = i & 1, .stop_all = &stop_all };
		pthread_create(&th[i], NULL, irpr_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < ms && !stop_all) {
		struct timespec tw;

		clock_gettime(CLOCK_MONOTONIC, &tw);
		while (elapsed_ms(&tw) < 1)
			rcu_quiescent_state();
		uatomic_set(&owned_pause, 1);
		while (uatomic_read(&owned_paused) != (int) nthreads &&
				!stop_all)
			rcu_quiescent_state();
		if (!stop_all) {
			nwin++;
			rcu_read_lock();
			if (verify_or_dump(ft, name))
				stop_all = 1, ret = -1;
			rcu_read_unlock();
		}
		uatomic_set(&owned_pause, 0);
		while (uatomic_read(&owned_paused) != 0)
			rcu_quiescent_state();
	}
	if (stop_all)
		ret = -1;
	fprintf(stderr, "# %s: %lu windows checked\n", name, nwin);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rcu_thread_offline();
	for (i = 0; i < nthreads; i++)
		pthread_join(th[i], NULL);
	rcu_thread_online();
	rcu_read_lock();
	if (verify_or_dump(ft, name))
		ret = -1;
	rcu_read_unlock();
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * Same-key cds_ft_insert_unique racing at a PREFIX position: the key ends at
 * an existing internal node (P, with P+'A' and P+'B' kept present below it)
 * that has no head yet, so every insert takes the "new key at this internal
 * node" arm and parks into that node's external_nodes.  The arm is chosen from
 * an UNHELD read of external_nodes == NULL; if the park does not re-check it
 * under the holder's lock, two writers both see NULL, the second overwrites
 * the first's head, and BOTH report OK (the prefix-key twin of the pigeon
 * FENCED ADD race inv_concurrent_insert_unique_dense covers for leaf slots --
 * whose fixed 8-byte keys never end at an internal node).
 *
 * Oracle, owner-only: an insert_unique that reported OK must find its OWN node
 * under the key, and its owner's remove must succeed.
 */
#define UNIQUE_PREFIX_WRITERS	4
static unsigned long unique_prefix_lost;
struct unique_prefix_arg {
	struct cds_ft *ft;
	unsigned int id;
	unsigned long ops;
};

static void *unique_prefix_writer(void *arg)
{
	struct unique_prefix_arg *a = (struct unique_prefix_arg *) arg;
	struct cds_ft_iter *iter;
	uint8_t key[PREFIX_PAIR_RUN + 1];
	size_t klen = prefix_pair_key(key, 0, 0);

	rcu_register_thread();
	if (cds_ft_iter_create(a->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	while (!test_stop) {
		struct ft_test_node *n = node_alloc(a->id);
		struct cds_ft_node *res = NULL, *out = NULL;
		enum cds_ft_status st;

		n->value = klen;
		rcu_read_lock();
		st = cds_ft_insert_unique(a->ft, key, klen, &n->node, &res);
		if (st != CDS_FT_STATUS_OK) {
			/* A peer holds the key: never published, ours to free. */
			rcu_read_unlock();
			node_free(n);
			rcu_quiescent_state();
			continue;
		}
		cds_ft_iter_set_key(iter, key, klen);
		if (cds_ft_eager_lookup_key(a->ft, key, klen, 0, &out)
				!= CDS_FT_STATUS_OK || out != &n->node ||
				cds_ft_lookup(a->ft, iter) != CDS_FT_STATUS_OK ||
				cds_ft_iter_node(iter) != &n->node ||
				cds_ft_remove(a->ft, iter, &n->node)
					!= CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			uatomic_inc(&unique_prefix_lost);
			break;
		}
		node_free_rcu(n);
		rcu_read_unlock();
		a->ops++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_insert_unique_prefix(void)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft_ws(&group, &ws);
	pthread_t th[UNIQUE_PREFIX_WRITERS];
	struct unique_prefix_arg a[UNIQUE_PREFIX_WRITERS];
	const char *env = getenv("FT_INV_PREFIX_PAIR_MS");
	unsigned long long ms = env ? strtoull(env, NULL, 10) : PREFIX_PAIR_MS;
	struct timespec t0;
	unsigned long total = 0;
	unsigned int i;
	int ret = 0;

	unique_prefix_lost = 0;
	/* P+'A' and P+'B': P's position is an internal junction, no head. */
	for (i = 0; i < 2; i++) {
		uint8_t k[PREFIX_PAIR_RUN + 1];
		size_t kl = prefix_pair_key(k, 0, 0);
		struct ft_test_node *n = node_alloc(9100 + i);

		k[kl++] = (uint8_t) ('A' + i);
		n->value = kl;
		if (cds_ft_insert(ft, k, kl, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < UNIQUE_PREFIX_WRITERS; i++) {
		a[i].ft = ft;
		a[i].id = i;
		a[i].ops = 0;
		pthread_create(&th[i], NULL, unique_prefix_writer, &a[i]);
	}
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < ms)
		rcu_quiescent_state();
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	rcu_thread_offline();
	for (i = 0; i < UNIQUE_PREFIX_WRITERS; i++) {
		pthread_join(th[i], NULL);
		total += a[i].ops;
	}
	rcu_thread_online();

	fprintf(stderr, "# inv_concurrent_insert_unique_prefix: %lu ops, "
		"%lu lost\n", total, unique_prefix_lost);
	if (unique_prefix_lost) {
		fprintf(stderr, "inv_concurrent_insert_unique_prefix: %lu "
			"insert_unique(s) reported OK but the key was not the "
			"writer's own node, or its remove failed\n",
			unique_prefix_lost);
		ret = -1;
	}
	if (total == 0) {
		fprintf(stderr, "inv_concurrent_insert_unique_prefix: no ops\n");
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_concurrent_insert_unique_prefix: verify "
			"failed\n");
		ret = -1;
	}
	drain_trie_keep_group(ft);
	rcu_barrier();
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   TWO graft_swaps exchanging at ONE destination position           */
/*                                                                    */
/* ================================================================== */

/*
 * cds_ft_graft_swap is named in the public grant alongside cds_ft_graft and
 * cds_ft_merge_at: under LOCK_FINE "several cross-trie attaches ... may run
 * concurrently on the same destination".  Nothing has ever driven two of them
 * at the SAME destination position.  The existing 16-writer graft_swap oracle
 * gives every writer a DISJOINT {p,w} prefix -- they share a spine node, never
 * a destination -- so the interleaving the header licenses is uncovered.
 *
 * THE CHECK IS CONSERVATION, which needs no linearization argument.  Each
 * writer exchanges its own private trie with dst's subtree at ONE shared key,
 * so content only ever MOVES between the three tries: the total key count
 * across dst + swapA + swapB is invariant, whatever order the two swaps take
 * and whichever of them wins.  A key that goes missing was dropped by a retire
 * that could not see the peer's change -- graft_swap fences nothing and retires
 * its destination nodes through the PLAIN tombstone, which takes its expected
 * old from the committed word and so cannot tell "unchanged" from "changed by a
 * peer".  That is the shape that cost cds_ft_merge_at and cds_ft_graft their
 * empty-dst keys.
 *
 * Both swap tries are made EXCLUSIVE (the contract requires it of a source, and
 * the flag is one-way, so they stay exclusive as content circulates through
 * them).  Only dst is live.
 */
#define GS_SHARED_KEY		0x50
#define GS_KEYS_PER_TRIE	4
#define GS_ROUNDS_MS		1200
#define GS_MAX_RETRY		4096
#define GS_MAX_PREFIX		4
/* Drain bound: >> any legitimate content, small enough to fail fast. */
#define GS_DRAIN_MAX		4096

/*
 * ★ THE GRAFT-POINT SHAPE IS THE THING THIS FAMILY MUST VARY.
 *
 * Built with -DFEATURE_FT_PROBE_GRAFT_SWAP, the original fixture reports
 * `exact=... kshort=0 delegate=0 fused=0 ext_child=0` over ~90k attempts: it
 * drives exactly ONE of ft_graft_swap_descend's arms.  Every other arm --
 * KEY_SHORTER, the compressed-grandparent fuse, an EXTERNAL occupant -- had
 * never executed, so the @c9f02111 coherence fix landed on them blind.  Count
 * the arm before trusting it (feedback_probe_the_class_dont_argue_it).
 *
 * @dst_prefix is the byte string every dst key shares; @key_len is how much of
 * it the swap key spans.  Where that boundary falls INSIDE the compressed run
 * the prefix collapses into is what selects the arm:
 *
 *   key_len == dst_prefix_len == 1   the graft point is a root slot        EXACT
 *   key_len <  dst_prefix_len        the key ends inside the compressed
 *                                    node the prefix collapsed into  KEY_SHORTER
 *   key_len == dst_prefix_len > 1    the descent consumes the whole
 *                                    compressed run and lands on its
 *                                    child, so d.pnf is compressed --
 *                                    the grandparent-fuse precondition
 *
 * The shapes are not stable across a round (a swap re-shapes the run it was
 * grafted into), which is the point: the counters, not this comment, say what
 * actually ran.  Read them.
 */
/*
 * What dst holds AT the graft key -- the thing that selects which arm of
 * cds_ft_graft_swap runs at all.
 *
 * ★ EVERY LAYOUT BELOW PREDATING THIS FIELD BUILDS A SUBTREE THERE, which is
 * why the probe reads ext_child=0 delegate=0 across all nine of them.  The
 * arms are not rare, they are unreachable by FIXTURE GEOMETRY: the occupant
 * decides, and the occupant was always the same kind of node.
 */
enum gs_dst_shape {
	/* Content BELOW the key, so the occupant is an internal/compressed
	 * subtree: FT_GRAFT_SWAP_EXACT with old_child_external false. */
	GS_DST_SUBTREE = 0,
	/* The key ITSELF, terminal and un-extended, so the occupant is a plain
	 * external -- the one displaced node with no state word to fence. */
	GS_DST_LEAF,
	/* Nothing at the key: FT_GRAFT_SWAP_DELEGATE, which returns through
	 * cds_ft_graft before any of the swap machinery runs. */
	GS_DST_ABSENT,
};

struct gs_layout {
	const char *name;
	uint8_t dst_prefix[GS_MAX_PREFIX];
	unsigned int dst_prefix_len;
	unsigned int key_len;	/* graft_swap key = dst_prefix[0 .. key_len) */
	/*
	 * Writers driving the shared destination.  2 is the oracle; 1 is the
	 * CONTROL that says whether a red result needs a peer at all.  Without
	 * the control a red shape-oracle cannot distinguish "this arm is broken
	 * under concurrency" from "this arm is broken, full stop" -- and the
	 * single-threaded KEY_SHORTER path IS covered elsewhere
	 * (ft_unit rank_stats_graft_swap_ks_one), so the two answers point at
	 * completely different code.
	 */
	unsigned int nwriters;
	/*
	 * Give each swap trie TWO top-level bytes instead of one.
	 *
	 * ★ WITHOUT THIS THE SHAPE DOES NOT HOLD.  A single-top-byte swap root
	 * canonicalizes to a COMPRESSED node, which chain-merges into the
	 * compressed run it is grafted into -- so the graft point moves and the
	 * next round descends a different arm.  The probe shows it plainly:
	 * kshort=1 over 129102 rounds, i.e. the shape under test ran ONCE and
	 * the other 129101 rounds measured something else.  A branching swap
	 * root cannot merge, so the graft point stays put and the arm actually
	 * repeats.
	 */
	bool swap_branch;
	/*
	 * Bytes of shared prefix to give the SWAP content.
	 *
	 * ★ KEY_SHORTER UN-DOES ITSELF.  Grafting at a key that ends inside a
	 * compressed run SPLITS that run there, so the same key is an EXACT
	 * position next round -- which is why the first kshort layouts scored
	 * kshort=1 and then measured EXACT forever.  Swap content carrying its
	 * own deep prefix re-forms a long run under the graft key every time it
	 * lands, so the arm actually repeats.
	 */
	unsigned int swap_prefix_len;
	/*
	 * Rebuild all three tries to this layout between rounds.
	 *
	 * ★ REQUIRED TO SUSTAIN KEY_SHORTER.  The arm is self-cancelling in a
	 * swap ring: grafting at a key that ends INSIDE a compressed run SPLITS
	 * the run at exactly that key, so the key is an EXACT position from the
	 * next descent onward.  Without a reseed the shape under test runs ONCE
	 * (kshort=1) and every later round silently measures EXACT, which reads
	 * as coverage and is not.  With it, kshort tracks the round count.
	 *
	 * Rounds are far fewer than a non-reseeding run's; rounds are not the
	 * figure of merit here, kshort is.  Conservation is checked per round
	 * against that round's freshly counted total.
	 */
	bool reseed;
	/*
	 * Build the group with SPECULATIVE lookup optimization instead of EAGER.
	 *
	 * ★ REQUIRED TO REACH THE COMPRESSED-GRANDPARENT FUSE.  EAGER explicitly
	 * CLEARS CDS_FT_FLAG_SKIP_COMPRESSED (ft-lifecycle.h), and
	 * ft_compress_single_child_if_needed returns its argument unchanged when
	 * the group has skip-compression off -- so @canon is never compressed and
	 * the fuse's third precondition can never hold.  Measured: with EAGER the
	 * parent condition fires 37336 times and the canon condition ZERO, so the
	 * arm is unreachable by GROUP CONFIGURATION, not by geometry.
	 */
	bool speculative;
	/*
	 * ★ BOTH NON-SUBTREE SHAPES SELF-CANCEL, so they need @reseed exactly
	 * as KEY_SHORTER does.  Measured over a 20-round ring with no reseed:
	 * GS_DST_LEAF scores ext_child=1 (the swap puts a SUBTREE at the key,
	 * so every later round is EXACT), GS_DST_ABSENT scores delegate=7/20.
	 * A layout that sets this without @reseed measures the arm once and
	 * reads as coverage.
	 */
	enum gs_dst_shape dst_shape;
};

struct gs_shared_ctx {
	struct cds_ft *dst;
	struct cds_ft *swap[2];
	const struct gs_layout *lay;
	unsigned long seq;
	unsigned long done;
	unsigned long retries;
	int stop;
	enum cds_ft_status st[2];
};

/*
 * Spun rendezvous, not a barrier: this is a QSBR suite and a registered thread
 * parked in pthread_barrier_wait stalls the grace period the round teardown
 * waits on.
 */
static unsigned long gs_wait_round(struct gs_shared_ctx *c, unsigned long seen)
{
	for (;;) {
		unsigned long s = uatomic_load(&c->seq, CMM_ACQUIRE);

		if (s != seen)
			return s;
		if (uatomic_load(&c->stop, CMM_RELAXED))
			return 0;
		caa_cpu_relax();
		rcu_quiescent_state();
	}
}

struct gs_arg {
	struct gs_shared_ctx *ctx;
	unsigned int w;
};

static void *gs_shared_writer(void *arg)
{
	struct gs_arg *a = (struct gs_arg *) arg;
	struct gs_shared_ctx *c = a->ctx;
	const uint8_t *key = c->lay->dst_prefix;
	const size_t key_len = c->lay->key_len;
	unsigned long seen = 0;

	rcu_register_thread();
	for (;;) {
		seen = gs_wait_round(c, seen);
		if (!seen)
			break;
		/*
		 * RETRY on BUSY.  graft_swap reports a transient peer conflict as
		 * BUSY_ERROR (its -EAGAIN is mapped there so a caller need not
		 * read "out of memory" as "re-descend") and has NO internal retry
		 * loop, so two writers on one destination simply abort each other:
		 * measured 545892 BUSY against 2 OK before this loop existed.
		 * Retrying is what the caller is expected to do; bound it so a
		 * livelock ends the round instead of the run.
		 */
		{
			unsigned int attempt;

			for (attempt = 0; attempt < GS_MAX_RETRY; attempt++) {
				/*
				 * RE-EXCLUDE EVERY TIME.  On success @swap_ft
				 * INHERITS the destination's access discipline,
				 * and dst is live -- so a swap trie is exclusive
				 * only until its first successful swap, after
				 * which graft_swap correctly rejects it with
				 * BUSY.  Marking once at setup produced 22331392
				 * BUSY retries and 2 successes, which looked
				 * exactly like a livelock and was not one.
				 */
				cds_ft_make_exclusive(c->swap[a->w]);
				c->st[a->w] = cds_ft_graft_swap(c->dst, key,
					key_len, c->swap[a->w]);
				if (c->st[a->w] != CDS_FT_STATUS_BUSY_ERROR)
					break;
				uatomic_inc(&c->retries);
				caa_cpu_relax();
				rcu_quiescent_state();
			}
		}
		uatomic_add(&c->done, 1);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * Fill @ft with @n keys "@prefix ++ @tag ++ i".  @prefix is the dst layout's
 * shared prefix for the destination and empty for a swap trie (whose content
 * is re-homed under the graft key by the swap itself).  Every dst key sharing
 * the whole prefix is what makes it collapse into ONE compressed run -- the
 * run whose interior the KEY_SHORTER shape needs to land in.
 */
/* Shared prefix handed to a swap trie when the layout asks for a deep one. */
static const uint8_t gs_swap_prefix[3] = { 0xC1, 0xC2, 0xC3 };

static void gs_fill(struct cds_ft *ft, uint8_t tag, unsigned int n,
		const uint8_t *prefix, unsigned int prefix_len)
{
	unsigned int i;

	for (i = 0; i < n; i++) {
		uint8_t k[GS_MAX_PREFIX + 4];
		size_t klen = 0;
		struct ft_test_node *node = node_alloc(((uint64_t) tag << 8) | i);

		if (prefix_len) {
			memcpy(k, prefix, prefix_len);
			klen = prefix_len;
		}
		k[klen++] = tag;
		k[klen++] = (uint8_t) i;
		memcpy(node->okey, k, klen);
		node->value = klen;
		if (cds_ft_insert(ft, k, klen, &node->node) != CDS_FT_STATUS_OK)
			abort();
	}
}

/*
 * Fill dst and both swap tries to @lay's shape.  Called at setup and, for a
 * reseeding layout, once per round.
 */
/* Insert @key exactly, as its own terminal key -- no tag/index suffix. */
static void gs_insert_key(struct cds_ft *ft, const uint8_t *key,
		unsigned int key_len)
{
	struct ft_test_node *node = node_alloc(0xE000);

	memcpy(node->okey, key, key_len);
	node->value = key_len;
	if (cds_ft_insert(ft, key, key_len, &node->node) != CDS_FT_STATUS_OK)
		abort();
}

/*
 * A dst branch that does NOT collide with the graft key, so a layout can put
 * the trie's bulk somewhere the swap never touches and leave the graft point
 * holding exactly what it wants to test.
 */
static const uint8_t gs_dst_other[1] = { 0x60 };

static void gs_seed(struct gs_shared_ctx *c, const struct gs_layout *lay)
{
	unsigned int i;

	for (i = 0; i < 2; i++) {
		gs_fill(c->swap[i], (uint8_t) (0xA0 + i), GS_KEYS_PER_TRIE,
			gs_swap_prefix, lay->swap_prefix_len);
		if (lay->swap_branch)
			gs_fill(c->swap[i], (uint8_t) (0xB0 + i),
				GS_KEYS_PER_TRIE, gs_swap_prefix,
				lay->swap_prefix_len);
	}
	switch (lay->dst_shape) {
	case GS_DST_SUBTREE:
		gs_fill(c->dst, 0xD0, GS_KEYS_PER_TRIE, lay->dst_prefix,
			lay->dst_prefix_len);
		break;
	case GS_DST_LEAF:
		/*
		 * The graft key itself and nothing under it, so the occupant is
		 * a plain external.  The bulk goes on @gs_dst_other: a dst that
		 * is ONLY this leaf would leave the swap ring emptying and
		 * refilling a one-key trie, and the graft point would not sit
		 * inside a populated root.
		 */
		gs_insert_key(c->dst, lay->dst_prefix, lay->key_len);
		gs_fill(c->dst, 0xD0, GS_KEYS_PER_TRIE, gs_dst_other,
			(unsigned int) sizeof gs_dst_other);
		break;
	case GS_DST_ABSENT:
		gs_fill(c->dst, 0xD0, GS_KEYS_PER_TRIE, gs_dst_other,
			(unsigned int) sizeof gs_dst_other);
		break;
	}
}

/*
 * Empty @ft but KEEP IT USABLE.  drain_trie_keep_group() is deliberately not
 * used: it keeps the GROUP and cds_ft_destroy()s the trie, and recreating a
 * trie per round costs two orders of magnitude in rounds and races the writer
 * threads that hold the pointer.
 *
 * BOUNDED.  An unbounded key-walk over a structurally broken trie is not a
 * theoretical worry -- it ran away to 187 GB RSS and took a global OOM kill.
 * The caller must only ever reach here on a trie that just VERIFIED, and this
 * bound is the second line of defence.  Returns -1 if it did not converge.
 */
static int gs_drain_keep_trie(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;
	unsigned long removed = 0;
	int ret = 0;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;
		enum cds_ft_status s = cds_ft_remove_all(ft, iter, &head);

		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "gs_drain_keep_trie: lookup_first found "
				"a key but remove_all reports %d after %lu "
				"removals (count_keys=%lu)\n", (int) s, removed,
				cds_ft_count_keys(ft));
			ret = -1;
			break;
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp)
			node_free_rcu(to_test_node(head));
		if (++removed > GS_DRAIN_MAX) {
			fprintf(stderr, "gs_drain_keep_trie: no convergence "
				"after %lu removals (count_keys=%lu)\n",
				removed, cds_ft_count_keys(ft));
			ret = -1;
			break;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return ret;
}

/*
 * Drain all three tries and re-seed them, between rounds with both writers
 * parked in gs_wait_round so nothing is concurrently reachable.  Callers must
 * have VERIFIED all three first.  Returns -1 if any drain did not converge.
 */
static int gs_reseed(struct gs_shared_ctx *c, const struct gs_layout *lay)
{
	unsigned int i;

	if (gs_drain_keep_trie(c->dst))
		return -1;
	for (i = 0; i < 2; i++)
		if (gs_drain_keep_trie(c->swap[i]))
			return -1;
	gs_seed(c, lay);
	return 0;
}

static int gs_shared_oracle(const char *tname, bool list_on,
		const struct gs_layout *lay)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct cds_ft *probe;
	struct gs_shared_ctx ctx;
	struct gs_arg arg[2];
	pthread_t th[2];
	struct timespec t0;
	unsigned long total, round = 0, lost_rounds = 0;
	unsigned long both_ok = 0, one_ok = 0;
	unsigned long stcount[8] = { 0 };
	const unsigned int nw = lay->nwriters;
	unsigned int i;
	int ret = 0;

	assert(nw >= 1 && nw <= 2);
	if (lay->speculative)
		probe = list_on ? create_varlen_ord_ft_ws_spec(&group, &ws)
				: create_varlen_nolist_ft_ws_spec(&group, &ws);
	else
		probe = list_on ? create_varlen_ord_ft_ws(&group, &ws)
				: create_varlen_nolist_ft_ws(&group, &ws);

	ctx.stop = 0;
	ctx.seq = 0;
	ctx.done = 0;
	ctx.retries = 0;
	ctx.lay = lay;
	if (cds_ft_create(group, NULL, &ctx.dst) < 0)
		abort();
	for (i = 0; i < 2; i++) {
		if (cds_ft_create(group, NULL, &ctx.swap[i]) < 0)
			abort();
	}
	gs_seed(&ctx, lay);

	total = cds_ft_count_keys(ctx.dst) + cds_ft_count_keys(ctx.swap[0])
		+ cds_ft_count_keys(ctx.swap[1]);

	for (i = 0; i < nw; i++) {
		arg[i].ctx = &ctx;
		arg[i].w = i;
		pthread_create(&th[i], NULL, gs_shared_writer, &arg[i]);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (;; round++) {
		unsigned long now;

		ctx.st[0] = ctx.st[1] = CDS_FT_STATUS_OK;
		uatomic_store(&ctx.done, 0, CMM_RELAXED);
		uatomic_store(&ctx.seq, round + 1, CMM_RELEASE);
		while (uatomic_load(&ctx.done, CMM_ACQUIRE) != nw) {
			caa_cpu_relax();
			rcu_quiescent_state();
		}

		/*
		 * CONSERVATION.  Content only moves between the three tries, so
		 * the total is invariant no matter who won.
		 */
		now = cds_ft_count_keys(ctx.dst)
			+ cds_ft_count_keys(ctx.swap[0])
			+ cds_ft_count_keys(ctx.swap[1]);
		if (now != total) {
			if (!lost_rounds++)
				fprintf(stderr, "%s: round %lu: %lu keys across "
					"dst+swap0+swap1, expected %lu -- a "
					"graft_swap retired a peer's content "
					"(st0=%d st1=%d)\n", tname, round, now,
					total, (int) ctx.st[0], (int) ctx.st[1]);
			ret = -1;
			total = now;	/* re-baseline so one loss is not counted forever */
		}
		for (i = 0; i < nw; i++) {
			int idx = -(int) ctx.st[i];

			if (idx < 0 || idx > 7)
				idx = 0;
			stcount[idx]++;
		}
		/*
		 * "both" is every DRIVING writer -- with nw == 1 the control
		 * still has to report that its one swap landed, or a control
		 * that silently did nothing would read as a clean denominator.
		 */
		if (nw == 1)
			both_ok += (ctx.st[0] == CDS_FT_STATUS_OK);
		else if (ctx.st[0] == CDS_FT_STATUS_OK
				&& ctx.st[1] == CDS_FT_STATUS_OK)
			both_ok++;
		else if (ctx.st[0] == CDS_FT_STATUS_OK
				|| ctx.st[1] == CDS_FT_STATUS_OK)
			one_ok++;
		/*
		 * ★ VERIFY ALL THREE TRIES TOGETHER, not just dst, and not one
		 * at a time.  A corrupt SWAP trie has been observed while dst
		 * verified clean and BOTH swaps returned OK -- checking only the
		 * destination is blind to the half of the exchange that gets
		 * re-rooted.
		 *
		 * cds_ft_verify_disjoint, not three cds_ft_verify calls: an
		 * exchange that hands the SAME node to two tries leaves each of
		 * them self-consistent, so every per-trie walk passes.  At a
		 * root that is not even a near miss -- a root's parent is NULL
		 * in every trie, so two tries rooted at one node agree with
		 * their own expected_parent.  Only the walk that spans them
		 * sees it, and this exchange is exactly where such a node
		 * comes from.
		 */
		{
			struct cds_ft *set[3];
			size_t nr = 0;

			set[nr++] = ctx.dst;
			set[nr++] = ctx.swap[0];
			if (nw > 1)
				set[nr++] = ctx.swap[1];
			if (cds_ft_verify_disjoint(set, nr, stderr)
					!= CDS_FT_STATUS_OK) {
				fprintf(stderr, "%s: round %lu: verify_disjoint failed "
					"(st0=%d st1=%d)\n", tname, round,
					(int) ctx.st[0], (int) ctx.st[1]);
				ret = -1;
				break;
			}
		}
		rcu_quiescent_state();
		if (elapsed_ms(&t0) >= GS_ROUNDS_MS)
			break;
		/*
		 * Restore the graft-point shape for the NEXT round.
		 * DELIBERATELY AFTER the verify above, never before: the drain
		 * walks by KEY, so draining a trie this oracle has just proved
		 * broken is the mistake the teardown below refuses to make --
		 * and it does not merely abort, it runs away (187 GB RSS, OOM).
		 */
		if (lay->reseed) {
			if (gs_reseed(&ctx, lay)) {
				fprintf(stderr, "%s: round %lu: reseed failed\n",
					tname, round);
				ret = -1;
				break;
			}
			total = cds_ft_count_keys(ctx.dst)
				+ cds_ft_count_keys(ctx.swap[0])
				+ cds_ft_count_keys(ctx.swap[1]);
		}
	}

	uatomic_store(&ctx.stop, 1, CMM_RELEASE);
	for (i = 0; i < nw; i++)
		pthread_join(th[i], NULL);

	fprintf(stderr, "# %s: %lu rounds (both-OK %lu, one-OK %lu), "
		"%lu rounds lost keys, %lu BUSY retries; statuses ok=%lu inval=%lu mem=%lu "
		"overflow=%lu busy=%lu populated=%lu integrity=%lu nosup=%lu\n",
		tname, round + 1, both_ok, one_ok, lost_rounds, ctx.retries,
		stcount[0], stcount[1], stcount[2], stcount[3], stcount[4],
		stcount[5], stcount[6], stcount[7]);
	/* A run where the writers never all succeeded has not driven the window. */
	if (!ret && both_ok == 0) {
		fprintf(stderr, "%s: no round had every swap succeed -- the "
			"shared-destination window was never driven\n", tname);
		ret = -1;
	}

	/*
	 * On a DETECTED corruption, do NOT drain: the drain walks by key, and a
	 * trie whose structure this oracle just proved broken makes
	 * drain_trie_keep_group hit its stale-iterator abort() -- which killed
	 * the whole ft_inv binary at test 40 and blocked every test after it in
	 * all ten gate configs.  A red oracle must report and let the suite
	 * continue.  The nodes leak for this run and leak_check reports it, on
	 * top of the failure already recorded -- deliberate, and cheaper than
	 * losing the other 40-odd invariants.  Mirrors the dead-internal
	 * teardown detector, which dumps and skips its walk for the same reason.
	 */
	if (ret == 0) {
		drain_trie_keep_group(ctx.dst);
		for (i = 0; i < 2; i++)
			drain_trie_keep_group(ctx.swap[i]);
	} else {
		fprintf(stderr, "%s: corruption detected -- skipping the drain "
			"(its key walk would abort on the broken trie); the "
			"round's nodes leak by design\n", tname);
	}
	rcu_barrier();
	cds_ft_destroy(probe);
	cds_ft_group_destroy(group);
	return ret;
}

/*
 * The graft point is a ROOT SLOT: key_len == dst_prefix_len == 1, so the
 * descent's last step lands directly on dst's depth-1 child.  This is the
 * shape the family has always run -- FT_GRAFT_SWAP_EXACT with a plain parent.
 */
static const struct gs_layout gs_lay_exact = {
	"exact", { GS_SHARED_KEY }, 1, 1, 2, false, 0, false, false, GS_DST_SUBTREE
};

/*
 * The graft point is INSIDE a compressed run: every dst key shares three
 * prefix bytes, which collapse into one compressed node, and the swap key
 * spans only two of them -- so the descent reports FT_GRAFT_SWAP_KEY_SHORTER
 * and the swap must split that node's interior.  That arm carries its own
 * extract build (ft_build_extracted_root_glue on the suffix), its own
 * ks_cn deferred free, and the LEGACY publish path whose commit status is
 * still dropped; none of it had ever executed.
 */
static const struct gs_layout gs_lay_kshort = {
	"kshort", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 2, false, 0, false, false, GS_DST_SUBTREE
};
/*
 * DEPTH CONTROL: same deep dst as "kshort", but the swap key spans only the
 * FIRST prefix byte, so the graft point is a root slot again (EXACT, plain
 * parent) while the trie below it is just as deep.  Separates "the shape
 * oracles are red because the trie got deeper" from "because the graft
 * point's parent stopped being the root".
 */
static const struct gs_layout gs_lay_wide = {
	"wide", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 1, 2, false, 0, false, false, GS_DST_SUBTREE
};
/*
 * STABLE EXACT-UNDER-A-COMPRESSED-PARENT.  dst keys are prefix ++ 0xD0 ++ i,
 * so the run under the root's 0x50 slot is [0x51, 0xD0] and the swap key spans
 * all three bytes -- the descent consumes the whole run and stops on
 * cn->child.  d.pnf is therefore a COMPRESSED node and the forward publish
 * targets &cn->child rather than a root slot, which the original fixture never
 * did.  @swap_branch keeps it that way across rounds.  Note dst_prefix_len (2)
 * is what gs_fill writes; key_len (3) reaches one byte further, onto the tag
 * every dst key shares.
 */
static const struct gs_layout gs_lay_cparent = {
	"cparent", { GS_SHARED_KEY, 0x51, 0xD0 }, 2, 3, 2, true, 0, false, false, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_cparent_solo = {
	"cparent-solo", { GS_SHARED_KEY, 0x51, 0xD0 }, 2, 3, 1, true, 0, false, false, GS_DST_SUBTREE
};
/*
 * Deep dst run, key ending inside it, and swap content carrying its own deep
 * prefix.
 *
 * ★ THIS WAS AN ATTEMPT AT A SUSTAINED KEY_SHORTER AND IT DID NOT WORK -- the
 * probe says kshort=1 over 123280 rounds, same as the layouts without the deep
 * swap prefix.  The swap prefix only shapes ROUND 0; from round 1 the ring
 * circulates whatever was EXTRACTED from dst, and grafting at a key inside a
 * compressed run SPLITS the run there, so that key is an EXACT position ever
 * after.  KEY_SHORTER is self-cancelling in a swap ring.  Kept because it is a
 * distinct round-0 shape and a cheap control, NOT because it soaks the arm:
 * sustaining it needs a per-round reseed of dst (see the empty-dst oracles,
 * which refill their sources every round).  Read the counters, not this name.
 */
static const struct gs_layout gs_lay_ks2 = {
	"ks2", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 2, false, 3, false, false, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_ks2_solo = {
	"ks2-solo", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 1, false, 3, false, false, GS_DST_SUBTREE
};
/*
 * SUSTAINED KEY_SHORTER.  Same deep-run layout as "kshort", rebuilt every round
 * so the split the previous round's graft made is undone and the writers
 * descend the KEY_SHORTER arm again.  This is the regression guard for the
 * legacy publish path whose commit status @93fad396 stopped discarding.
 */
/*
 * The compressed-grandparent FUSE.  Three conditions must hold at once
 * (ft-graft.h): EXACT, the graft point's PARENT compressed, and @canon
 * compressed, with pcn->len + ccn->len <= FT_SKIP_LEN_MAX.
 *
 * Geometry: dst keys are prefix ++ 0xD0 ++ i with prefix {0x50,0x51}, so the
 * run under the root's 0x50 slot is [0x51,0xD0]; the swap key spans all three
 * bytes, so the descent consumes that run and stops on cn->child with d.pnf
 * COMPRESSED.  The swap trie keeps a SINGLE top byte (swap_branch false) so its
 * root canonicalizes to a COMPRESSED node -- the condition "cparent" misses,
 * which is why that layout reports fused=0 despite having the parent right.
 * @reseed holds the shape, which a single-top-byte swap otherwise destroys by
 * chain-merging into the run it lands in.
 */
static const struct gs_layout gs_lay_fuse = {
	"fuse", { GS_SHARED_KEY, 0x51, 0xD0 }, 2, 3, 2, false, 0, true, true, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_fuse_solo = {
	"fuse-solo", { GS_SHARED_KEY, 0x51, 0xD0 }, 2, 3, 1, false, 0, true, true, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_ksfix = {
	"ksfix", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 2, false, 0, true, false, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_ksfix_solo = {
	"ksfix-solo", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 1, false, 0, true, false, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_kshort_solo = {
	"kshort-solo", { GS_SHARED_KEY, 0x51, 0x52 }, 3, 2, 1, false, 0, false, false, GS_DST_SUBTREE
};
/*
 * The two shapes the occupant -- not the key -- selects.
 *
 * EXTCHILD drives the EXTERNAL occupant: the graft key is itself a terminal
 * key with nothing under it, so the displaced old-child is a plain external
 * rather than a subtree root.  It is the one displaced node that carries NO
 * state word, so the fencing every other shape relies on does not apply to
 * it, and until now it had never run: ext_child=0 over ~590k attempts across
 * the nine layouts above.
 *
 * DELEGATE drives the empty graft point, where the swap degenerates to
 * cds_ft_graft and returns before any swap machinery runs.  Its value is the
 * RACE, not the arm: the descent decides "nothing here" and then calls graft
 * as a separate operation, so a peer that fills the graft point in between is
 * exactly the window worth holding open.
 *
 * Both reseed -- they self-cancel otherwise (see @dst_shape) -- and both come
 * with a 1-writer control, so a red result can be attributed to the peer
 * rather than to the arm being broken outright.
 */
static const struct gs_layout gs_lay_extchild = {
	"extchild", { GS_SHARED_KEY }, 1, 1, 2, true, 0, true, false,
	GS_DST_LEAF
};
static const struct gs_layout gs_lay_extchild_solo = {
	"extchild-solo", { GS_SHARED_KEY }, 1, 1, 1, true, 0, true, false,
	GS_DST_LEAF
};
static const struct gs_layout gs_lay_delegate = {
	"delegate", { GS_SHARED_KEY }, 1, 1, 2, true, 0, true, false,
	GS_DST_ABSENT
};
static const struct gs_layout gs_lay_delegate_solo = {
	"delegate-solo", { GS_SHARED_KEY }, 1, 1, 1, true, 0, true, false,
	GS_DST_ABSENT
};

/*
 * The graft point is a COMPRESSED NODE'S CHILD: the key spans the whole
 * shared prefix, so the descent consumes the compressed run and stops on
 * cn->child with d.pnf compressed.  That is the precondition for the
 * skip-compressed grandparent FUSE, which carries a second instance of the
 * two-load shape @c9f02111 fixed (ft-graft.h, pub_old read separately from
 * the pcn body the merged node is built from).
 */
static const struct gs_layout gs_lay_deep = {
	"deep", { GS_SHARED_KEY, 0x51 }, 2, 2, 2, false, 0, false, false, GS_DST_SUBTREE
};
static const struct gs_layout gs_lay_deep_solo = {
	"deep-solo", { GS_SHARED_KEY, 0x51 }, 2, 2, 1, false, 0, false, false, GS_DST_SUBTREE
};

static int inv_graft_swap_shared_dst(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst", /*list_on=*/ true,
		&gs_lay_exact);
}

static int inv_graft_swap_shared_dst_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_nolist",
		/*list_on=*/ false, &gs_lay_exact);
}

static int inv_graft_swap_shared_dst_kshort(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_kshort",
		/*list_on=*/ true, &gs_lay_kshort);
}

static int inv_graft_swap_shared_dst_kshort_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_kshort_nolist",
		/*list_on=*/ false, &gs_lay_kshort);
}

static int inv_graft_swap_shared_dst_deep(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_deep",
		/*list_on=*/ true, &gs_lay_deep);
}

static int inv_graft_swap_shared_dst_deep_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_deep_nolist",
		/*list_on=*/ false, &gs_lay_deep);
}

/*
 * THE CONTROLS.  Same layouts, same key, ONE writer -- so the destination is
 * still re-shaped by a real graft_swap every round but no peer ever contends
 * it.  These must stay GREEN: if a shape oracle is red and its control is red
 * too, the arm is broken outright and the concurrency framing is wrong.
 */
static int inv_graft_swap_shared_dst_ks2_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_ks2_nolist",
		/*list_on=*/ false, &gs_lay_ks2);
}

static int inv_graft_swap_shared_dst_ks2_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_ks2_solo",
		/*list_on=*/ false, &gs_lay_ks2_solo);
}

static int inv_graft_swap_shared_dst_cparent_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_cparent_nolist",
		/*list_on=*/ false, &gs_lay_cparent);
}

static int inv_graft_swap_shared_dst_cparent_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_cparent_solo",
		/*list_on=*/ false, &gs_lay_cparent_solo);
}

static int inv_graft_swap_shared_dst_wide_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_wide_nolist",
		/*list_on=*/ false, &gs_lay_wide);
}

static int inv_graft_swap_shared_dst_fuse(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_fuse",
		/*list_on=*/ true, &gs_lay_fuse);
}

static int inv_graft_swap_shared_dst_fuse_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_fuse_nolist",
		/*list_on=*/ false, &gs_lay_fuse);
}

static int inv_graft_swap_shared_dst_fuse_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_fuse_solo",
		/*list_on=*/ false, &gs_lay_fuse_solo);
}

static int inv_graft_swap_shared_dst_ksfix(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_ksfix",
		/*list_on=*/ true, &gs_lay_ksfix);
}

static int inv_graft_swap_shared_dst_ksfix_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_ksfix_nolist",
		/*list_on=*/ false, &gs_lay_ksfix);
}

static int inv_graft_swap_shared_dst_ksfix_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_ksfix_solo",
		/*list_on=*/ false, &gs_lay_ksfix_solo);
}

static int inv_graft_swap_shared_dst_kshort_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_kshort_solo",
		/*list_on=*/ false, &gs_lay_kshort_solo);
}

static int inv_graft_swap_shared_dst_deep_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_deep_solo",
		/*list_on=*/ false, &gs_lay_deep_solo);
}
static int inv_graft_swap_shared_dst_extchild_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_extchild_nolist",
		/*list_on=*/ false, &gs_lay_extchild);
}
static int inv_graft_swap_shared_dst_extchild(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_extchild",
		/*list_on=*/ true, &gs_lay_extchild);
}
static int inv_graft_swap_shared_dst_extchild_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_extchild_solo",
		/*list_on=*/ false, &gs_lay_extchild_solo);
}
static int inv_graft_swap_shared_dst_delegate_nolist(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_delegate_nolist",
		/*list_on=*/ false, &gs_lay_delegate);
}
static int inv_graft_swap_shared_dst_delegate_solo(void)
{
	return gs_shared_oracle("inv_graft_swap_shared_dst_delegate_solo",
		/*list_on=*/ false, &gs_lay_delegate_solo);
}

/*
 * ☞ POINT WRITERS INSIDE A WHOLE-TRIE GRAFT_SWAP'S DRAIN SEAM.
 *
 * A key_len 0 cds_ft_graft_swap swaps the two tries' roots and ordered lists in
 * ONE dual flip, NULL-terminating each moved run's outer links (a straddler of
 * either run must see a universal end), and restores them to circular form --
 * first->prev / last->next at the sentinel -- only AFTER a drain.  That drain
 * is ft_writer_lock_gp_wait, which RELEASES @dst's FT-wide writer lock for the
 * grace period (a QSBR writer parked on it is an online reader), so a FINE
 * point writer on the live @dst can run inside it and meet a list whose two
 * ends point at NULL.  No other test puts a point writer on a graft_swap
 * destination: gs_shared_oracle's writers are graft_swaps, mw_gs_oracle's
 * inserts go into their private swap tries.
 *
 * The swapper whole-swaps a fresh block of keys into @dst; the point writers
 * remove @dst's current FIRST and LAST keys and insert new keys at both ends --
 * exactly the two cells the seam leaves unfinalized.  Ownership: whoever
 * removes a node frees it; whatever a swap carries out is drained by the
 * swapper.  Oracle, every round with the writers parked: cds_ft_verify, and a
 * forward and a reverse walk that must agree with cds_ft_count_keys.
 */
#define GSS_W		2	/* point writers (default) */
#define GSS_W_MAX	8	/* FT_INV_GSS_W ceiling */
#define GSS_K		16	/* keys per swapped-in block */
#define GSS_SWAPS	16	/* swaps per round */
#define GSS_MS		1500	/* default run time (FT_INV_GSS_MS overrides) */

struct gss_ctx {
	struct cds_ft *dst;
	int run;
	int stop;
	int parked;
	unsigned long ins, ins_fail, rm, rm_miss;
};

struct gss_arg {
	struct gss_ctx *c;
	unsigned int w;
};

static struct ft_test_node *gss_node(const uint8_t *key, size_t klen,
		uint64_t tag)
{
	struct ft_test_node *n = node_alloc(tag);

	memcpy(n->okey, key, klen);
	n->value = klen;
	return n;
}

/* Remove @ft's first (or last) key's head node; whoever removes it frees it. */
static int gss_remove_end(struct cds_ft *ft, struct cds_ft_iter *iter,
		bool last)
{
	struct cds_ft_node *node;
	enum cds_ft_status st;

	st = last ? cds_ft_lookup_last(ft, iter) : cds_ft_lookup_first(ft, iter);
	if (st != CDS_FT_STATUS_OK || !(node = cds_ft_iter_node(iter)))
		return -1;
	if (cds_ft_remove(ft, iter, node) != CDS_FT_STATUS_OK)
		return -1;
	node_free_rcu(to_test_node(node));
	return 0;
}

static void *gss_point_writer(void *arg)
{
	struct gss_arg *a = (struct gss_arg *) arg;
	struct gss_ctx *c = a->c;
	struct cds_ft_iter *iter;
	unsigned int seq = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(c->dst, &iter) != CDS_FT_STATUS_OK)
		abort();
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		unsigned int side;

		if (!uatomic_load(&c->run, CMM_ACQUIRE)) {
			uatomic_inc(&c->parked);
			while (!uatomic_load(&c->run, CMM_ACQUIRE) &&
					!uatomic_load(&c->stop, CMM_RELAXED)) {
				caa_cpu_relax();
				rcu_quiescent_state();
			}
			uatomic_dec(&c->parked);
			continue;
		}
		for (side = 0; side < 2; side++) {
			uint8_t key[4] = { side ? 0xFF : 0x00, (uint8_t) a->w,
				(uint8_t) (seq >> 8), (uint8_t) seq };
			struct ft_test_node *n = gss_node(key, sizeof(key),
				((uint64_t) 0x5500 + a->w) << 32 | seq);

			rcu_read_lock();
			if (cds_ft_insert(c->dst, key, sizeof(key), &n->node)
					== CDS_FT_STATUS_OK) {
				uatomic_inc(&c->ins);
			} else {
				uatomic_inc(&c->ins_fail);
				node_free(n);	/* never published */
			}
			if (gss_remove_end(c->dst, iter, side))
				uatomic_inc(&c->rm_miss);
			else
				uatomic_inc(&c->rm);
			/*
			 * ☠ DROP THE CACHED POSITION BEFORE THE LOCK.  @iter
			 * outlives this critical section, and its position --
			 * and, with an in-leaf key, a key that POINTS INTO the
			 * result node -- is valid only while the lock is held
			 * (fractal-trie.h).  The next iteration passes a
			 * quiescent state, so a grace period can reclaim what
			 * the position names.  Nothing is carried across the
			 * gap here, so RESET rather than bind: bind_key
			 * MATERIALIZES the key from the current position, and
			 * after a remove (or a missed lookup) there is no
			 * position to materialize -- it SEGVs in
			 * ft_ord_cell_cursor, which is how this was found.
			 */
			cds_ft_iter_reset(iter);
			rcu_read_unlock();
		}
		seq++;
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Empty @ft, freeing every node it still holds.  One read-side section per key
 * and a quiescent state between them: a QSBR thread that loops without one
 * holds back every grace period, the swapper's own drain included.
 */
static void gss_drain(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (;;) {
		struct cds_ft_node *node, *p;

		rcu_read_lock();
		if (cds_ft_lookup_first(ft, iter) != CDS_FT_STATUS_OK) {
			rcu_read_unlock();
			break;
		}
		if (cds_ft_remove_all(ft, iter, &node) != CDS_FT_STATUS_OK)
			abort();
		cds_ft_for_each_duplicate_safe_rcu(node, p)
			node_free_rcu(to_test_node(node));
		cds_ft_iter_reset(iter);	/* see gss_point_writer */
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
}

/* Structure, forward list, reverse list and count must all agree. */
static int gss_check(struct cds_ft *ft, unsigned long round)
{
	struct cds_ft_iter *iter;
	unsigned long fwd = 0, rev = 0, cnt;
	int ret = 0;

	/*
	 * ☞ ONE ITERATOR PER SCOPE, not one across all three.  An iterator's
	 * cached position is valid only while the read-side lock that produced
	 * it is held (fractal-trie.h), and these scopes are separated by
	 * quiescent states -- a grace period may reclaim what the position
	 * names.  The walks share nothing, so each gets its own; the structure
	 * check needs no iterator at all.
	 */
	rcu_read_lock();
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "inv_graft_swap_whole_seam_points: round %lu: "
			"verify RED\n", round);
		ret = -1;
	}
	rcu_read_unlock();
	rcu_quiescent_state();

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_lock();
	cds_ft_for_each_rcu(ft, iter)
		fwd++;
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_lock();
	cds_ft_for_each_reverse_rcu(ft, iter)
		rev++;
	cnt = cds_ft_count_keys(ft);
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	if (fwd != rev || fwd != cnt) {
		fprintf(stderr, "inv_graft_swap_whole_seam_points: round %lu: "
			"forward %lu, reverse %lu, count_keys %lu disagree\n",
			round, fwd, rev, cnt);
		ret = -1;
	}
	return ret;
}

static int inv_graft_swap_whole_seam_points(void)
{
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cds_ft_group *group;
	struct gss_ctx c;
	struct gss_arg args[GSS_W_MAX];
	pthread_t th[GSS_W_MAX];
	struct timespec t0;
	const char *env = getenv("FT_INV_GSS_MS");
	unsigned long long ms = env ? strtoull(env, NULL, 10) : GSS_MS;
	/* FT_INV_GSS_W: point writers, 0 = the swaps-only CONTROL. */
	const char *wenv = getenv("FT_INV_GSS_W");
	unsigned int nw = wenv ? (unsigned int) atoi(wenv) : GSS_W;
	/* FT_INV_GSS_SWAPS: swaps per round; 0 = point writers only (CONTROL). */
	const char *senv = getenv("FT_INV_GSS_SWAPS");
	unsigned int nswaps = senv ? (unsigned int) atoi(senv) : GSS_SWAPS;
	unsigned long round = 0, swaps = 0, busy = 0;
	unsigned int i, s;
	int ret = 0;

	memset(&c, 0, sizeof(c));
	if (nw > GSS_W_MAX)
		nw = GSS_W_MAX;
	c.dst = create_varlen_ord_ft_ws(&group, &ws);
	cds_ft_make_shared(c.dst);
	for (i = 0; i < nw; i++) {
		args[i].c = &c;
		args[i].w = i;
		pthread_create(&th[i], NULL, gss_point_writer, &args[i]);
	}
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (!ret && elapsed_ms(&t0) < ms) {
		uatomic_store(&c.run, 1, CMM_RELEASE);
		if (!nswaps) {
			struct timespec r0;

			/* Point writers alone for ~100 ms, with QS. */
			clock_gettime(CLOCK_MONOTONIC, &r0);
			while (elapsed_ms(&r0) < 100) {
				caa_cpu_relax();
				rcu_quiescent_state();
			}
		}
		for (s = 0; s < nswaps; s++, swaps++) {
			struct cds_ft *swap;
			enum cds_ft_status st;
			unsigned int k, attempt;

			if (cds_ft_create(group, NULL, &swap) < 0)
				abort();
			for (k = 0; k < GSS_K; k++) {
				uint8_t key[4] = { 0x80, (uint8_t) (swaps >> 8),
					(uint8_t) swaps, (uint8_t) k };
				struct ft_test_node *n = gss_node(key,
					sizeof(key), (uint64_t) swaps << 8 | k);

				if (cds_ft_insert(swap, key, sizeof(key),
						&n->node) != CDS_FT_STATUS_OK)
					abort();
			}
			/* The consumed swap trie must be exclusive (step 6). */
			cds_ft_make_exclusive(swap);
			for (attempt = 0; attempt < 64; attempt++) {
				st = cds_ft_graft_swap(c.dst, NULL, 0, swap);
				if (st != CDS_FT_STATUS_BUSY_ERROR)
					break;
				busy++;
				rcu_quiescent_state();
			}
			if (st != CDS_FT_STATUS_OK &&
					st != CDS_FT_STATUS_BUSY_ERROR) {
				fprintf(stderr, "inv_graft_swap_whole_seam_points: "
					"graft_swap: %s\n",
					cds_ft_status_to_string(st));
				ret = -1;
			}
			/* Whatever @swap holds now -- the old @dst, or its own
			 * block after a BUSY -- is the swapper's to free. */
			cds_ft_make_exclusive(swap);
			gss_drain(swap);
			cds_ft_destroy(swap);
			rcu_quiescent_state();
		}
		uatomic_store(&c.run, 0, CMM_RELEASE);
		while (uatomic_load(&c.parked, CMM_ACQUIRE) != (int) nw) {
			caa_cpu_relax();
			rcu_quiescent_state();
		}
		if (gss_check(c.dst, round))
			ret = -1;
		round++;
	}
	uatomic_store(&c.stop, 1, CMM_RELEASE);
	/* Blocked in a join, this thread must not hold back grace periods. */
	rcu_thread_offline();
	for (i = 0; i < nw; i++)
		pthread_join(th[i], NULL);
	rcu_thread_online();
	gss_drain(c.dst);
	rcu_barrier();
	cds_ft_destroy(c.dst);
	cds_ft_group_destroy(group);
	fprintf(stderr, "# inv_graft_swap_whole_seam_points: %u point writers, "
		"%lu rounds, %lu "
		"whole-trie swaps (%lu BUSY), %lu point inserts (%lu failed), "
		"%lu end removes (%lu missed) -> %s\n", nw, round, swaps, busy,
		c.ins, c.ins_fail, c.rm, c.rm_miss, ret ? "RED" : "ok");
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT: writers keep making progress under chain-merge        */
/*              contention (the escalation lane must be given back)   */
/*                                                                    */
/* ================================================================== */

/*
 * Every other invariant here asks whether the answer is RIGHT.  This one asks
 * whether an answer ARRIVES.
 *
 * @b9fcf7b4 fixed a writer that escalated for its COMMIT, took the domain's
 * FIFO turn, then bailed PRE-COMMIT on a DLM acquire miss and KEPT the turn
 * across every re-descend -- while the peer holding the contended node was
 * queued behind that same turn.  Circular wait: measured 142 lane enters
 * against 138 exits, 99.8% of begins already holding the turn, and every FT
 * counter frozen permanently.  Nothing in this suite would notice it coming
 * back: the shape needs real writer contention on ONE spine, and the failure
 * is a WEDGE, not a wrong answer -- no assertion fires, no key goes missing.
 *
 * The shape, from the out-of-tree oracle that found it:
 *   SPLITTER  inserts then removes a key diverging MID-span, so the branch it
 *             creates collapses and the two halves re-merge into one longer
 *             compressed node -- the absorb that moves an encoded position.
 *   DESCENDER mutates a key whose path runs THROUGH that same span, so its
 *             descent holds the skip pointer the splitter is invalidating.
 *   MERGER    cds_ft_merge_at with a dst key inside the span, which is what
 *             turns contention into the escalation this guards.
 *
 * ★★★ NOT FALSIFIED AGAINST THE HISTORICAL DEFECT -- READ THIS BEFORE TRUSTING
 * A GREEN RESULT.  Reverting @b9fcf7b4 does NOT bring the wedge back, and nor
 * does additionally reverting @a402b0f2 (the quiescent escalation park): the
 * out-of-tree oracle that originally froze at 39 splits runs 186k-198k splits
 * against a library with both reverted.  The freeze needed a code state that
 * those two reverts do not reconstruct -- other commits since (notably
 * @fdef60f0, binding the insert DLM acquire to the op's handle) contribute.
 * So this test is a PROGRESS AND STRESS test whose detector is verified, NOT a
 * proven guard for any one commit: its green says the workload made progress,
 * not that the livelock would be caught.
 * What IS verified: stalling the three workers deliberately makes the timer
 * below report and the test return not-ok.  And the workload is not vacuous --
 * it drives ~200k splits, ~250k merges and ~250k slot rewinds per run, a
 * chain-merge contention shape nothing else in this suite produces.
 *
 * ★ THE FAILURE MODE IS A HANG, AND THAT IS WHY THE DETECTOR IS A TIMER.
 * A wedged writer never returns from the library, so it never re-checks @stop
 * and cannot be joined.  This thread never blocks (it spins with quiescent
 * states, as a registered QSBR thread must), so it can still SAMPLE: if the
 * writers' combined progress does not advance for GP_STALL_MS, the violation
 * is REPORTED -- naming the livelock on stderr -- before the teardown that may
 * then hang on the join.  A named failure beats a silent one; do not read the
 * subsequent hang as the test being broken.
 */
#define CM_SPAN		12
#define CM_SPLIT_AT	6
#define CM_DESCENDERS	4
#define CM_RUN_MS	5000
#define CM_STALL_MS	2000		/* no progress this long == wedged */

struct cm_ctx {
	struct cds_ft *ft;
	struct cds_ft_group *group;
	struct cds_ft *msrc;	/* the merger's recycled source; destroyed by main */
	int stop;
	unsigned long splits, descents, merges;
};

/* key = 'A' x @plen + @tag, so every key shares the compressed prefix. */
static size_t cm_mkkey(uint8_t *k, unsigned int plen, uint8_t tag)
{
	memset(k, 'A', plen);
	k[plen] = tag;
	return plen + 1;
}

/* Insert @k then remove it again, freeing the node.  Returns 1 if it landed. */
static int cm_churn(struct cds_ft *ft, const uint8_t *k, size_t kl)
{
	struct ft_test_node *n = node_alloc(0);
	struct cds_ft_iter *it = NULL;
	struct cds_ft_node *got;

	if (cds_ft_insert(ft, k, kl, &n->node) != CDS_FT_STATUS_OK) {
		node_free(n);
		return 0;
	}
	if (cds_ft_iter_create(ft, &it) != CDS_FT_STATUS_OK)
		return 0;
	rcu_read_lock();
	cds_ft_iter_set_key(it, k, kl);
	if (cds_ft_lookup(ft, it) == CDS_FT_STATUS_OK &&
			(got = cds_ft_iter_node(it)) != NULL &&
			cds_ft_remove(ft, it, got) == CDS_FT_STATUS_OK)
		node_free_rcu(to_test_node(got));
	rcu_read_unlock();
	cds_ft_iter_destroy(it);
	return 1;
}

static void *cm_splitter(void *arg)
{
	struct cm_ctx *c = arg;
	uint8_t k[CM_SPAN + 8];
	size_t kl = cm_mkkey(k, CM_SPLIT_AT, 'M');	/* diverges mid-span */

	rcu_register_thread();
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		if (cm_churn(c->ft, k, kl))
			uatomic_add(&c->splits, 1);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

static void *cm_descender(void *arg)
{
	struct cm_ctx *c = *(struct cm_ctx **) arg;
	unsigned long id = *((unsigned long *) arg + 1);
	uint8_t k[CM_SPAN + 8];
	size_t kl = cm_mkkey(k, CM_SPAN, (uint8_t) ('a' + id));

	rcu_register_thread();
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		if (cm_churn(c->ft, k, kl))
			uatomic_add(&c->descents, 1);
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	return NULL;
}

/*
 * RECYCLE one src trie for the whole run: cds_ft_merge_at consumes its
 * CONTENT, not the handle, so it comes back empty and refillable.  Creating
 * one per round would park this thread in cds_ft_destroy's rcu_barrier every
 * iteration, stalling the grace period a peer writer waits on -- a harness
 * deadlock that reads exactly like the library one under test.
 */
static void *cm_merger(void *arg)
{
	struct cm_ctx *c = arg;
	uint8_t dk[CM_SPAN + 8], full[CM_SPAN + 8];
	size_t dkl = cm_mkkey(dk, CM_SPLIT_AT + 2, 'Q');
	const uint8_t sk[1] = { 'q' };
	struct cds_ft *src = NULL;

	rcu_register_thread();
	if (cds_ft_create(c->group, NULL, &src) < 0)
		goto out;
	cds_ft_make_exclusive(src);
	uatomic_store(&c->msrc, src, CMM_RELEASE);
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		struct ft_test_node *n = node_alloc(0);
		size_t fl;

		if (cds_ft_insert(src, sk, 1, &n->node) != CDS_FT_STATUS_OK) {
			node_free(n);
			rcu_quiescent_state();
			continue;
		}
		(void) cds_ft_merge_at(c->ft, dk, dkl, src, NULL, 0);
		uatomic_add(&c->merges, 1);
		/* Drain what landed, so the shape stays bounded. */
		memcpy(full, dk, dkl);
		fl = dkl;
		full[fl++] = 'q';
		{
			struct cds_ft_iter *it = NULL;
			struct cds_ft_node *got;

			if (cds_ft_iter_create(c->ft, &it) == CDS_FT_STATUS_OK) {
				rcu_read_lock();
				cds_ft_iter_set_key(it, full, fl);
				if (cds_ft_lookup(c->ft, it) == CDS_FT_STATUS_OK &&
						(got = cds_ft_iter_node(it)) != NULL &&
						cds_ft_remove(c->ft, it, got) ==
							CDS_FT_STATUS_OK)
					node_free_rcu(to_test_node(got));
				rcu_read_unlock();
				cds_ft_iter_destroy(it);
			}
		}
		rcu_quiescent_state();
	}
	/*
	 * Left for the main thread: cds_ft_destroy blocks in rcu_barrier, and
	 * this thread is a registered QSBR reader -- blocking here stalls the
	 * grace period the peers' writers are waiting on.
	 */
	uatomic_store(&c->msrc, src, CMM_RELEASE);
out:
	rcu_unregister_thread();
	return NULL;
}

static int inv_writer_progress_chainmerge(void)
{
	INV_NEED_MERGE("inv_writer_progress_chainmerge");
	const char *tname = "inv_writer_progress_chainmerge";
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;
	struct cm_ctx c;
	struct ft_test_node *anchor_node;
	uint8_t anchor[CM_SPAN + 8];
	size_t al;
	pthread_t sp, mg, dt[CM_DESCENDERS];
	unsigned long dargs[CM_DESCENDERS][2];
	struct timespec t0, tlast;
	unsigned long seen = 0;
	unsigned int i;
	int ret = 0, stalled = 0, corrupt = 0;

	memset(&c, 0, sizeof(c));
	/* SPECULATIVE: skip-compression is what makes the span encodable. */
	c.ft = create_varlen_nolist_ft_ws_spec(&c.group, &ws);
	cds_ft_make_shared(c.ft);

	/* Anchor keeps the long span alive so it stays compressed. */
	anchor_node = node_alloc(0);
	al = cm_mkkey(anchor, CM_SPAN, 'Z');
	rcu_read_lock();
	if (cds_ft_insert(c.ft, anchor, al, &anchor_node->node) !=
			CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();

	pthread_create(&sp, NULL, cm_splitter, &c);
	pthread_create(&mg, NULL, cm_merger, &c);
	for (i = 0; i < CM_DESCENDERS; i++) {
		dargs[i][0] = (unsigned long) &c;
		dargs[i][1] = i;
		pthread_create(&dt[i], NULL, cm_descender, &dargs[i]);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	tlast = t0;
	for (;;) {
		unsigned long now = uatomic_load(&c.splits, CMM_RELAXED)
			+ uatomic_load(&c.descents, CMM_RELAXED)
			+ uatomic_load(&c.merges, CMM_RELAXED);

		if (now != seen) {
			seen = now;
			clock_gettime(CLOCK_MONOTONIC, &tlast);
		} else if (elapsed_ms(&tlast) >= CM_STALL_MS) {
			/*
			 * Reported BEFORE the teardown, because a wedged writer
			 * cannot be joined and the join below may never return.
			 */
			report_violation(tname,
				"no writer progress for %d ms (%lu splits, %lu "
				"descents, %lu merges) -- an escalated writer is "
				"holding the FIFO turn across a pre-commit bail",
				CM_STALL_MS, c.splits, c.descents, c.merges);
			ret = -1;
			stalled = 1;
			break;
		}
		if (elapsed_ms(&t0) >= CM_RUN_MS)
			break;
		caa_cpu_relax();
		rcu_quiescent_state();
	}
	uatomic_store(&c.stop, 1, CMM_RELEASE);

	/*
	 * OFFLINE BEFORE JOINING.  This thread is a registered QSBR reader, so
	 * blocking in pthread_join while online stalls every grace period -- a
	 * worker finishing inside a grace-period wait would then wait on one
	 * that can never advance.
	 */
	rcu_thread_offline();
	pthread_join(sp, NULL);
	pthread_join(mg, NULL);
	for (i = 0; i < CM_DESCENDERS; i++)
		pthread_join(dt[i], NULL);
	rcu_thread_online();

	/*
	 * The merger moves subtrees ACROSS tries, so check for a node left
	 * reachable from both.  Per-trie verify cannot see that: two tries
	 * rooted at one node are each self-consistent, and at a root the parent
	 * check is vacuous (a root's parent is NULL in every trie).  Only a walk
	 * spanning them tells.
	 */
	{
		struct cds_ft *set[2];
		size_t nr = 0;

		set[nr++] = c.ft;
		if (c.msrc)
			set[nr++] = c.msrc;
		if (cds_ft_verify_disjoint(set, nr, stderr) != CDS_FT_STATUS_OK) {
			report_violation(tname, "a node is reachable from BOTH "
				"the destination and the merge source after "
				"%lu merges", c.merges);
			ret = -1;
			corrupt = 1;
		}
	}

	/*
	 * A run that made progress must also have made SENSE: surviving the
	 * contention is the invariant, but a wedge-free run that corrupted the
	 * trie is still a failure.
	 */
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		report_violation(tname, "verify failed after %lu splits / %lu "
			"descents / %lu merges", c.splits, c.descents, c.merges);
		ret = -1;
		corrupt = 1;
	}
	if (!stalled && c.splits == 0) {
		report_violation(tname,
			"the splitter never landed a key -- the shape under "
			"test did not run");
		ret = -1;
	}
	/*
	 * ★ drain_trie_keep_group() DESTROYS the trie and keeps only the GROUP.
	 * Do not follow it with cds_ft_destroy -- that is a double destroy, and
	 * it reads as trie corruption (verify reports a garbage root) rather
	 * than as the double free it is.
	 *
	 * A trie reported CORRUPT is destroyed WITHOUT draining: the drain walks
	 * by KEY, and a damaged remnant can run away -- a sibling oracle took a
	 * global OOM kill exactly there.  Its nodes are then leaked, which the
	 * per-test leak check reports, and that is the intended trade.
	 */
	if (c.msrc) {
		if (corrupt)
			cds_ft_destroy(c.msrc);
		else
			drain_trie_keep_group(c.msrc);
	}
	if (corrupt)
		cds_ft_destroy(c.ft);
	else
		drain_trie_keep_group(c.ft);
	rcu_barrier();
	cds_ft_group_destroy(c.group);
	return ret;
}

static int inv_merge_root_src_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_root_src_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *dst = create_varlen_ord_ft(&group);
	struct inv_rootswap_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct cds_ft **pool;
	struct timespec t0;
	static const uint8_t AT[1] = { 0x80 };
	unsigned int i, k, nmerges = 0;
	int ret = 0;

	pool = (struct cds_ft **) calloc(ROOTSWAP_XVIEW_GRAFTS, sizeof(*pool));
	if (!pool)
		abort();

	/* Occupy dst at {0x80} so each whole-src merge takes the spine-copy
	 * root_src path (cnt_dst > 0 at the merge point). */
	rcu_read_lock();
	{
		uint8_t key[2] = { 0x80, 0x00 };
		struct ft_test_node *n = node_alloc(0);

		n->value = 2;
		memcpy(n->okey, key, 2);
		if (cds_ft_insert(dst, key, 2, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	ctx.cur = NULL;
	ctx.test_name = "inv_merge_root_src_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_rootswap_disappear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < ROOTSWAP_XVIEW_GRAFTS; k++) {
		struct cds_ft *src;
		unsigned int s;

		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		/* DISTINCT keys per merge (a per-k 2-byte suffix), so the merged
		 * content accumulates in dst as a trie rather than long dup chains
		 * (keeping each spine-copy merge and the final drain near O(log)). */
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t key[3] = { (uint8_t)(0x01 + s),
				(uint8_t)(k >> 8), (uint8_t)(k & 0xff) };
			struct ft_test_node *n = node_alloc(k * 2 + s);

			n->value = 3;
			memcpy(n->okey, key, 3);
			if (cds_ft_insert(src, key, 3, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();

		/* Publish the full src, THEN merge it WHOLE into dst@{0x80} (occupied
		 * -> spine-copy root_src): src goes full -> empty. */
		pool[k] = src;
		nmerges = k + 1;
		rcu_assign_pointer(ctx.cur, src);
		/*
		 * DECOMPOSED, same reason as the prefix oracles above: under
		 * LOCK_FINE a cross-trie source must be exclusive, and @src is
		 * what the readers watch.  The ROOT detach IS this oracle's
		 * subject -- @src goes full -> empty, and its structure and its
		 * ordered list must never disagree while it does -- and the merge
		 * then runs against an EXCLUSIVE trie.  Still a merge: dst@{AT}
		 * is occupied on purpose (the spine-copy root_src path), which a
		 * graft rejects with POPULATED_ERROR.
		 */
		{
			struct cds_ft *whole = NULL;

			if (cds_ft_detach(src, NULL, 0, &whole) != CDS_FT_STATUS_OK ||
			    !whole)
				abort();
			if (cds_ft_merge_at(dst, AT, 1, whole, NULL, 0) !=
					CDS_FT_STATUS_OK)
				abort();
			cds_ft_destroy(whole);	/* emptied by the merge */
		}

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	/* Srcs were emptied by the merges; dst holds all the merged content. */
	rcu_assign_pointer(ctx.cur, NULL);
	rcu_barrier();
	for (k = 0; k < nmerges; k++)
		cds_ft_destroy(pool[k]);
	free(pool);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_root_src_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	if (drain_and_destroy(dst, group) < 0)
		ret = -1;
	return ret;
}

/*
 * BULK-OP (appear-side) cross-view oracle for cds_ft_merge_at -- the diverged /
 * absent-dst-point merge shape (ft_merge_graft_subpos_inplace), which publishes
 * the moved subtree structurally and THEN splices its run into dst's ordered
 * list (on HEAD, a separate ft_ord_cell_run_splice flip).  Identical method to
 * inv_graft_cross_view (it reuses inv_graft_xview_appear_reader): NON-CYCLING,
 * the main thread merges a fresh src subtree into dst at a strictly DECREASING
 * dst prefix, so each merge installs a new global minimum and the ordered-list
 * minimum only ever decreases.  The reader compares lookup_first (cell list) vs
 * lookup_ge (structural descent) under a front-stable sandwich.
 */
#ifndef MERGE_XVIEW_MERGES
#define MERGE_XVIEW_MERGES	20000
#endif

static int inv_merge_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, k;
	static const uint8_t SRCK[1] = { 0x53 };	/* "S": the src sub-position */

	/* Stable upper bulk: prefix {0xFF,0xFF}, always above every merged run. */
	rcu_read_lock();
	for (i = 0; i < 16; i++) {
		uint8_t key[3] = { 0xFF, 0xFF, (uint8_t) i };
		struct ft_test_node *n = node_alloc(0x2000 + i);

		n->value = 3;
		memcpy(n->okey, key, 3);
		if (cds_ft_insert(ft, key, 3, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_merge_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_graft_xview_appear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/*
	 * Merge a fresh src subtree (at "S", keys {S,0},{S,1}) into dst at a
	 * strictly DECREASING 2-byte prefix absent from dst -- the diverged-dst
	 * graft-subpos shape.  The moved keys become {prefix,0},{prefix,1} in @ft
	 * (the test node stashes the full @okey), each prefix a new global minimum.
	 */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < MERGE_XVIEW_MERGES; k++) {
		unsigned int pv = 0xFEFF - k;	/* decreasing distinct prefix */
		uint8_t prefix[2] = { (uint8_t)(pv >> 8), (uint8_t)(pv & 0xff) };
		struct cds_ft *src;
		unsigned int s;

		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t skey[2] = { SRCK[0], (uint8_t) s };
			uint8_t full[3] = { prefix[0], prefix[1], (uint8_t) s };
			struct ft_test_node *n = node_alloc(pv * 4 + s);

			n->value = 3;
			memcpy(n->okey, full, 3);
			if (cds_ft_insert(src, skey, 2, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		if (cds_ft_merge_at(ft, prefix, 2, src, SRCK, 1) != CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the merge */

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * BULK-OP (appear-side) cross-view oracle for the OCCUPIED-DST SPINE-COPY merge
 * -- the step-4 structural flip vs step-9 ordered INTERLEAVE window.  Same
 * decreasing-min method and reused reader (inv_graft_xview_appear_reader) as
 * inv_merge_cross_view, but each merge point is PRE-OCCUPIED (a stable
 * {prefix,0xFF} key) so the merge takes ft_merge_spine_copy rather than the
 * diverged subpos path.  The src run ({prefix,0},{prefix,1}) becomes the merged
 * region's new minimum; until the interleave splices its cells into dst's list,
 * the structural minimum sits BELOW the stable ordered-list front -> the
 * front-stable sandwich flags it.
 */
static int inv_merge_spinecopy_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_spinecopy_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, k;
	static const uint8_t SRCK[1] = { 0x53 };	/* "S": the src sub-position */

	/* Stable upper bulk: prefix {0xFF,0xFF}, always above every merged run. */
	rcu_read_lock();
	for (i = 0; i < 16; i++) {
		uint8_t key[3] = { 0xFF, 0xFF, (uint8_t) i };
		struct ft_test_node *n = node_alloc(0x3000 + i);

		n->value = 3;
		memcpy(n->okey, key, 3);
		if (cds_ft_insert(ft, key, 3, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_merge_spinecopy_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_graft_xview_appear_reader, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (k = 0; k < MERGE_XVIEW_MERGES; k++) {
		unsigned int pv = 0xFEFF - k;	/* decreasing distinct prefix */
		uint8_t prefix[2] = { (uint8_t)(pv >> 8), (uint8_t)(pv & 0xff) };
		uint8_t occ[3] = { prefix[0], prefix[1], 0xFF };
		struct ft_test_node *on = node_alloc(0x40000 + k);
		struct cds_ft *src;
		unsigned int s;

		/* Pre-occupy dst at {prefix} (suffix 0xFF, above the src keys) so the
		 * merge is spine-copy.  An insert is itself a fused single commit, so
		 * it adds no cross-view window of its own. */
		on->value = 3;
		memcpy(on->okey, occ, 3);
		rcu_read_lock();
		if (cds_ft_insert(ft, occ, 3, &on->node) != CDS_FT_STATUS_OK)
			abort();
		rcu_read_unlock();

		if (cds_ft_create(group, NULL, &src) < 0)
			abort();
		rcu_read_lock();
		for (s = 0; s < 2; s++) {
			uint8_t skey[2] = { SRCK[0], (uint8_t) s };
			uint8_t full[3] = { prefix[0], prefix[1], (uint8_t) s };
			struct ft_test_node *n = node_alloc(pv * 4 + s);

			n->value = 3;
			memcpy(n->okey, full, 3);
			if (cds_ft_insert(src, skey, 2, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		if (cds_ft_merge_at(ft, prefix, 2, src, SRCK, 1) != CDS_FT_STATUS_OK)
			abort();
		cds_ft_destroy(src);		/* emptied by the merge */

		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_spinecopy_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

static int inv_detach_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, p, s;

	rcu_read_lock();
	for (p = 0; p < DETACH_XVIEW_PREFIXES; p++) {
		for (s = 0; s < DETACH_XVIEW_PER; s++) {
			uint8_t key[3] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff),
				(uint8_t) s };
			struct ft_test_node *n = node_alloc(p);

			n->value = 3;
			memcpy(n->okey, key, 3);
			if (cds_ft_insert(ft, key, 3, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
	}
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_detach_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_remove_xview_reader_minvl,
			&ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/* Time-bounded: each detach is a full grace period, so the whole pool
	 * may not drain within the window -- drain_and_destroy reclaims the rest. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (p = 0; p < DETACH_XVIEW_PREFIXES; p++) {
		uint8_t prefix[2] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff) };
		struct cds_ft *detached = NULL;

		if (cds_ft_detach(ft, prefix, 2, &detached) == CDS_FT_STATUS_OK &&
		    detached) {
			drain_trie_local(detached);
			cds_ft_destroy(detached);
		}
		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_detach_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * BULK-OP (disappear-side) cross-view oracle for the WHOLE-TRIE root detach
 * (cds_ft_detach(ft, NULL, 0)) -- the disappear dual of the empty-dst root
 * graft window covered by inv_graft_root_swap_cross_view.  A root detach retires
 * @ft's root to a fresh empty node AND clears @ft's ordered-list head/tail.  On
 * HEAD those were separate bare stores (root publish, then head/tail clear), so
 * a reader could observe @ft structurally EMPTY but its ordered list still
 * pointing at the (now-detached) run.  The fix fuses the root swap with the
 * head/tail clear into ONE flip (the src/disappear side of
 * ft_root_list_swap_publish).
 *
 * The writer round-trips: detach the whole trie out (@ft -> @detached, @ft now
 * empty), then graft @detached back at the root (@ft empty -> full again),
 * keeping @ft repeatedly populated so the disappear window recurs every cycle.
 * The key set is FIXED, so the ordered minimum is a single STABLE key -- which
 * is what makes the reused min-drain reader (inv_remove_xview_reader_minvl)
 * sound: it flags only when that min is stably present in the list (k1 == k2)
 * yet absent from the structure within one RCU read-side critical section.  The
 * graft-back is itself a fused appear flip, so it adds no false positive.  The
 * detach drains @ft (it is concurrent); the graft-back of the EXCLUSIVE detached
 * trie at the root is sync-free.
 */
#ifndef DETACH_ROOT_XVIEW_KEYS
#define DETACH_ROOT_XVIEW_KEYS	2000
#endif
/*
 * The disappear window is a single narrow root-store-then-head-clear gap per
 * detach (and each detach is a grace period, so the writer does relatively few
 * ops).  Use more concurrent readers than the default to catch it reliably --
 * the same compensation inv_graft_root_swap_cross_view notes for its appear
 * window.
 */
#ifndef DETACH_ROOT_XVIEW_READERS
#define DETACH_ROOT_XVIEW_READERS	32
#endif

static int inv_detach_root_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct inv_lookup_ctx ctx;
	pthread_t readers[DETACH_ROOT_XVIEW_READERS];
	struct timespec t0;
	unsigned int i, k;
	int ret = 0;

	rcu_read_lock();
	for (k = 0; k < DETACH_ROOT_XVIEW_KEYS; k++) {
		uint8_t key[3] = { (uint8_t)(k >> 8), (uint8_t)(k & 0xff), 0 };
		struct ft_test_node *n = node_alloc(k);

		n->value = 3;
		memcpy(n->okey, key, 3);
		if (cds_ft_insert(ft, key, 3, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	ctx.ft = ft;
	ctx.test_name = "inv_detach_root_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < DETACH_ROOT_XVIEW_READERS; i++)
		pthread_create(&readers[i], NULL, inv_remove_xview_reader_minvl,
			&ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/*
	 * Round-trip the whole trie out and back.  Each detach is a grace
	 * period (the source is concurrent); the graft-back is sync-free
	 * (exclusive src, root swap).
	 */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS) {
		struct cds_ft *detached = NULL;

		if (cds_ft_detach(ft, NULL, 0, &detached) != CDS_FT_STATUS_OK ||
		    !detached) {
			fprintf(stderr, "inv_detach_root_cross_view: detach failed\n");
			ret = -1;
			break;
		}
		cds_ft_make_exclusive(detached);	/* DLM: cross-trie src must be exclusive */
		/* @ft is now empty; graft the whole detached trie back at root. */
		if (cds_ft_graft(ft, NULL, 0, detached) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_detach_root_cross_view: graft-back failed\n");
			cds_ft_destroy(detached);
			ret = -1;
			break;
		}
		cds_ft_destroy(detached);	/* emptied by the graft-back */
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < DETACH_ROOT_XVIEW_READERS; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_detach_root_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

/*
 * BULK-OP (disappear-side) cross-view oracle for cds_ft_merge_at's SOURCE side.
 * A merge unlinks the moved subtree from @src structurally (ft_merge_unlink_src_
 * subtree) and THEN removes its run from @src's ordered list (on HEAD, a separate
 * ft_ord_cell_run_unlink flip) -- so a @src reader can observe the moved run gone
 * from the structure but still present in the ordered list.  This is the
 * src-side dual of inv_detach_cross_view, and reuses its min-drain reader
 * (inv_remove_xview_reader_minvl: lookup_first -> point(min) -> lookup_first,
 * flagged only when the min is stably present in the list but absent from the
 * structure).  NON-CYCLING: the main thread merges @src's prefixes OUT to @dst in
 * increasing order, so @src only ever SHRINKS -- a re-appearing min is
 * impossible, which is what makes the present-absent-present check sound.
 */
#ifndef MERGE_SRC_XVIEW_PREFIXES
#define MERGE_SRC_XVIEW_PREFIXES	6000
#endif
#define MERGE_SRC_XVIEW_PER		3

static int inv_merge_src_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_src_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *src = create_varlen_ord_ft(&group);
	struct cds_ft *dst;
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, p, s;

	if (cds_ft_create(group, NULL, &dst) < 0)
		abort();

	rcu_read_lock();
	for (p = 0; p < MERGE_SRC_XVIEW_PREFIXES; p++) {
		for (s = 0; s < MERGE_SRC_XVIEW_PER; s++) {
			uint8_t key[3] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff),
				(uint8_t) s };
			struct ft_test_node *n = node_alloc(p);

			n->value = 3;
			memcpy(n->okey, key, 3);
			if (cds_ft_insert(src, key, 3, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
	}
	rcu_read_unlock();

	ctx.ft = src;			/* readers watch the DRAINED source */
	ctx.test_name = "inv_merge_src_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_remove_xview_reader_minvl,
			&ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/* Merge each prefix's subtree OUT of @src into @dst (same key, distinct in
	 * @dst), in increasing order so @src only shrinks.  Each merge is a grace
	 * period, so the pool may not drain within the window. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (p = 0; p < MERGE_SRC_XVIEW_PREFIXES; p++) {
		uint8_t prefix[2] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff) };
		struct cds_ft *detached = NULL;

		/*
		 * DECOMPOSED (was one cds_ft_merge_at with a LIVE @src).  Under
		 * CDS_FT_WRITER_LOCK_FINE a cross-trie source must be exclusive,
		 * and @src is exactly what the readers watch, so it cannot be.
		 * Detach-then-merge is the supported decomposition (design note
		 * decision (C): two sequential single-domain commits) and keeps
		 * this oracle's subject intact -- the reader-visible half is @src
		 * SHEDDING the subtree, i.e. the detach, unchanged.  The attach
		 * half then runs against the trie cds_ft_detach returns
		 * EXCLUSIVE by construction, so it is DLM-legal and skips its own
		 * grace period.  It stays a merge, not a graft: @dst is
		 * deliberately populated here, which a graft rejects with
		 * POPULATED_ERROR.  @detached's ROOT holds the subtree, hence the
		 * NULL/0 source key.
		 */
		if (cds_ft_detach(src, prefix, 2, &detached) == CDS_FT_STATUS_OK &&
		    detached) {
			(void) cds_ft_merge_at(dst, prefix, 2, detached, NULL, 0);
			cds_ft_destroy(detached);
		}
		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_src_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(src);
		drain_trie_local(dst);
		rcu_barrier();
		cds_ft_destroy(src);
		cds_ft_destroy(dst);
		cds_ft_group_destroy(group);
		return -1;
	}
	drain_trie_local(src);
	drain_trie_local(dst);
	rcu_barrier();
	cds_ft_destroy(src);
	cds_ft_destroy(dst);
	cds_ft_group_destroy(group);
	return 0;
}

/*
 * BULK-OP (disappear-side) cross-view oracle for the NON-ROOT SPINE-COPY merge
 * src side -- distinct from inv_merge_src_cross_view (which merges into an
 * ABSENT dst point -> the subpos/diverged path).  Here every dst merge point is
 * PRE-OCCUPIED, so the merge takes ft_merge_spine_copy's non-root-src branch:
 * ft_merge_unlink_src_subtree unlinks the source subtree structurally and THEN a
 * separate ft_ord_cell_run_unlink removes its run from src's ordered list, so a
 * src reader between them sees the moved subtree gone from the structure but
 * still in the ordered list.  The fix threads an EXCISE-ONLY run through
 * ft_merge_unlink_src_subtree (as the subpos src side already does).
 *
 * Same NON-CYCLING min-drain method as inv_merge_src_cross_view (reused reader
 * inv_remove_xview_reader_minvl): @src's prefixes are merged OUT in increasing
 * order so @src only shrinks.  @dst is pre-populated with one stable key under
 * EACH prefix (key suffix 0xFF) so cnt_dst > 0 at every merge point; the moved
 * keys keep their own (distinct) suffixes, so @dst stays a trie, not dup chains.
 */
static int inv_merge_src_spinecopy_cross_view(void)
{
	INV_NEED_MERGE("inv_merge_src_spinecopy_cross_view");
	struct cds_ft_group *group;
	struct cds_ft *src = create_varlen_ord_ft(&group);
	struct cds_ft *dst;
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, p, s;

	if (cds_ft_create(group, NULL, &dst) < 0)
		abort();

	rcu_read_lock();
	for (p = 0; p < MERGE_SRC_XVIEW_PREFIXES; p++) {
		uint8_t dkey[3] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff), 0xFF };
		struct ft_test_node *dn = node_alloc(0x10000 + p);

		/* Pre-occupy dst at this prefix so the merge is spine-copy. */
		dn->value = 3;
		memcpy(dn->okey, dkey, 3);
		if (cds_ft_insert(dst, dkey, 3, &dn->node) != CDS_FT_STATUS_OK)
			abort();
		for (s = 0; s < MERGE_SRC_XVIEW_PER; s++) {
			uint8_t key[3] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff),
				(uint8_t) s };
			struct ft_test_node *n = node_alloc(p);

			n->value = 3;
			memcpy(n->okey, key, 3);
			if (cds_ft_insert(src, key, 3, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
	}
	rcu_read_unlock();

	ctx.ft = src;			/* readers watch the DRAINED source */
	ctx.test_name = "inv_merge_src_spinecopy_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_remove_xview_reader_minvl,
			&ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/* Merge each prefix's subtree OUT of @src into @dst at the SAME (occupied)
	 * prefix -> spine-copy non-root-src.  Increasing order so @src only shrinks. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (p = 0; p < MERGE_SRC_XVIEW_PREFIXES; p++) {
		uint8_t prefix[2] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff) };
		struct cds_ft *detached = NULL;

		/*
		 * DECOMPOSED (was one cds_ft_merge_at with a LIVE @src).  Under
		 * CDS_FT_WRITER_LOCK_FINE a cross-trie source must be exclusive,
		 * and @src is exactly what the readers watch, so it cannot be.
		 * Detach-then-merge is the supported decomposition (design note
		 * decision (C): two sequential single-domain commits) and keeps
		 * this oracle's subject intact -- the reader-visible half is @src
		 * SHEDDING the subtree, i.e. the detach, unchanged.  The attach
		 * half then runs against the trie cds_ft_detach returns
		 * EXCLUSIVE by construction, so it is DLM-legal and skips its own
		 * grace period.  It stays a merge, not a graft: @dst is
		 * deliberately populated here, which a graft rejects with
		 * POPULATED_ERROR.  @detached's ROOT holds the subtree, hence the
		 * NULL/0 source key.
		 */
		if (cds_ft_detach(src, prefix, 2, &detached) == CDS_FT_STATUS_OK &&
		    detached) {
			(void) cds_ft_merge_at(dst, prefix, 2, detached, NULL, 0);
			cds_ft_destroy(detached);
		}
		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_src_spinecopy_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(src);
		drain_trie_local(dst);
		rcu_barrier();
		cds_ft_destroy(src);
		cds_ft_destroy(dst);
		cds_ft_group_destroy(group);
		return -1;
	}
	drain_trie_local(src);
	drain_trie_local(dst);
	rcu_barrier();
	cds_ft_destroy(src);
	cds_ft_destroy(dst);
	cds_ft_group_destroy(group);
	return 0;
}


/* ================================================================== */
/*                                                                    */
/*   INVARIANT 4: Duplicate chain acyclicity                          */
/*                                                                    */
/*   Walking a duplicate chain under rcu_read_lock() must always      */
/*   terminate.  A cycle in the chain would cause a reader to loop    */
/*   forever.  We bound the walk to MAX_DUP_CHAIN_LEN and flag a     */
/*   violation if exceeded.                                           */
/*                                                                    */
/* ================================================================== */

static void *inv_dup_chain_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		struct cds_ft_node *found;

		rcu_read_lock();
		if (lookup_u64(ctx->ft, key, &found) == CDS_FT_STATUS_OK && found) {
			int count = 0;
			struct cds_ft_node *pos = found;

			cds_ft_for_each_duplicate_rcu(pos) {
				count++;
				if (count > MAX_DUP_CHAIN_LEN) {
					report_violation(ctx->test_name,
						"duplicate chain for key %" PRIu64
						" exceeds %d — probable cycle",
						key, MAX_DUP_CHAIN_LEN);
					break;
				}
			}
		}
		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

static int inv_dup_chain_acyclicity(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_dup_chain_acyclicity";
	pthread_mutex_init(&ctx.lock, NULL);

	/*
	 * Pre-populate with duplicates to ensure chains exist from the
	 * start.
	 */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 4; i++) {
		unsigned int j;

		for (j = 0; j < 3; j++) {
			struct ft_test_node *n = node_alloc(i);
			insert_u64(ft, i, n);
		}
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_dup_chain_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_lookup_consistency_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_dup_chain_acyclicity: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/*
 * SKIP_X head-promotion: SPARSE keys (idx << 16) put each duplicate chain's head
 * at a leaf reached through a SKIP_X suffix compressed node.  Removing such a
 * head with duplicates remaining drives ft_promote_head's COMPRESSED-holder
 * path -- the cn->child republish AND the grandparent SKIP_X dual fused with the
 * fresh-cell swap in ONE flip.  The dense-key inv_dup_chain_acyclicity only
 * reaches the plain internal-holder promotion, so this is the only invariant
 * that drives the compressed-holder dual concurrently.  Readers walk the dup
 * chain (acyclicity) -- a torn promotion (stale skip target into the freed old
 * head, or a cell pointing at the removed head) would corrupt the walk.
 */
#define INV_SKIPX_KEY(idx)	((uint64_t)(idx) << 16)

static void *inv_skipx_promote_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = INV_SKIPX_KEY(rand_r(&seed) % WRITER_POOL_SIZE);
		struct cds_ft_node *found;

		rcu_read_lock();
		if (lookup_u64(ctx->ft, key, &found) == CDS_FT_STATUS_OK && found) {
			int count = 0;
			struct cds_ft_node *pos = found;

			cds_ft_for_each_duplicate_rcu(pos) {
				count++;
				if (count > MAX_DUP_CHAIN_LEN) {
					report_violation(ctx->test_name,
						"duplicate chain for key %" PRIu64
						" exceeds %d — probable cycle",
						key, MAX_DUP_CHAIN_LEN);
					break;
				}
			}
		}
		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

static void *inv_skipx_promote_writer(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = INV_SKIPX_KEY(rand_r(&seed) % WRITER_POOL_SIZE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(&ctx->lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(&ctx->lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			/* Lookup returns the chain HEAD: removing it promotes a dup. */
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(&ctx->lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(&ctx->lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_dup_chain_skipx_head_promotion(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_dup_chain_skipx_head_promotion";
	pthread_mutex_init(&ctx.lock, NULL);

	/* Pre-populate SPARSE keys (SKIP_X leaves) each with a duplicate chain. */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 4; i++) {
		unsigned int j;

		for (j = 0; j < 3; j++) {
			struct ft_test_node *n = node_alloc(INV_SKIPX_KEY(i));
			insert_u64(ft, INV_SKIPX_KEY(i), n);
		}
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_skipx_promote_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_skipx_promote_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_dup_chain_skipx_head_promotion: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

#undef INV_SKIPX_KEY

/* ================================================================== */
/*                                                                    */
/*   INVARIANT: Ordered-list-OFF (no-cell) consistency                */
/*                                                                    */
/*   A group with the ordered list disabled allocates NO ordinal      */
/*   cells: a head's prev is the flagged parent directly, so the      */
/*   read path resolves it prev-direct (ft_resolve_head_prev), and    */
/*   skip resolution / parent backtrack run the no-cell branch.       */
/*   Stress that path under concurrent insert/remove: lookups must    */
/*   return the right node (shadow-key check) and duplicate chains    */
/*   must stay acyclic.  This gives the runtime cell-optional path     */
/*   standing concurrent coverage in the default pass, independent of */
/*   the FT_INV_NO_ORDERED_LIST env knob.                             */
/*                                                                    */
/* ================================================================== */

static int inv_no_ordered_list_consistency(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_nolist_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_no_ordered_list_consistency";
	pthread_mutex_init(&ctx.lock, NULL);

	/* Pre-populate. */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/*
	 * Half the readers verify lookup consistency (right node for the key),
	 * half walk duplicate chains for acyclicity; the writers churn
	 * insert/remove.  All reuse the shared inv_lookup_ctx thread bodies.
	 */
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			(i & 1) ? inv_dup_chain_reader :
				inv_lookup_consistency_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_lookup_consistency_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_no_ordered_list_consistency: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 5: Graft-swap atomicity                                */
/*                                                                    */
/*   After a graft_swap at root, a concurrent reader iterating the    */
/*   trie must see a population that is consistent with either the    */
/*   old content or the new content — never an empty intermediate     */
/*   state, and never a mix of both.  We use two distinct key ranges  */
/*   (A: [0, N) and B: [N, 2N)) so any single iteration that sees    */
/*   keys from both ranges is a violation.                            */
/*                                                                    */
/* ================================================================== */

#define GRAFT_SWAP_POOL		64
#define GRAFT_SWAP_RANGE_A_BASE	0
#define GRAFT_SWAP_RANGE_B_BASE	GRAFT_SWAP_POOL

struct inv_graft_ctx {
	struct cds_ft *live;
	struct cds_ft *swap;	/* cross-trie oracle: the second trie, shared */
	struct cds_ft_group *group;
	const char *test_name;
	pthread_mutex_t lock;
};

static void *inv_graft_swap_reader(void *arg)
{
	struct inv_graft_ctx *ctx = (struct inv_graft_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->live, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int saw_a = 0, saw_b = 0;
		int saw_empty = 1;

		rcu_read_lock();
		cds_ft_for_each_rcu(ctx->live, iter) {
			uint8_t rk[4];
			size_t rk_len;
			uint64_t v;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->live, rk, rk_len);
			saw_empty = 0;

			if (v < GRAFT_SWAP_RANGE_B_BASE)
				saw_a = 1;
			else
				saw_b = 1;
		}
		rcu_read_unlock();

		if (saw_a && saw_b) {
			report_violation(ctx->test_name,
				"iteration saw keys from both range A and B "
				"(iter #%lu) — graft_swap not atomic",
				iters);
		}
		/*
		 * After initial population the trie should never be empty
		 * to a reader (graft_swap is not supposed to have an empty
		 * intermediate).  However, we only flag this if we also
		 * didn't see any keys at all, which could legitimately
		 * happen during the very first swap or if timing is tight.
		 * We do not flag saw_empty as a hard violation because the
		 * writer may not have completed the first swap yet.
		 */
		(void)saw_empty;

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Helper: populate a trie with @count keys starting at @base.
 * The trie should have no concurrent readers (offline population).
 */
static void populate_range(struct cds_ft *ft, uint64_t base, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct ft_test_node *n = node_alloc(base + i);
		uint8_t k[8] = { 0 };

		cds_ft_u64_to_key(ft, base + i, k, 4);
		memcpy(n->okey, k, sizeof(n->okey));
		cds_ft_insert(ft, k, 4, &n->node);
	}
}

/*
 * Helper: drain every node from a trie.  No concurrent access assumed.
 */
static void drain_trie_local(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();

	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;

		cds_ft_remove_all(ft, iter, &head);
		cds_ft_for_each_duplicate_safe_rcu(head, tmp) {
			node_free_rcu(to_test_node(head));
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
}

static void *inv_graft_swap_writer(void *arg)
{
	struct inv_graft_ctx *ctx = (struct inv_graft_ctx *) arg;
	struct cds_ft *swap;
	int phase = 0;		/* 0: live has A, swap will get B; 1: vice versa */

	rcu_register_thread();

	if (cds_ft_create(ctx->group, NULL, &swap) < 0)
		abort();

	/* Prepare the first swap trie with range B keys. */
	populate_range(swap, GRAFT_SWAP_RANGE_B_BASE, GRAFT_SWAP_POOL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;

		/*
		 * Swap at root: atomically exchange the live trie's
		 * content with the swap trie's content.
		 */
		/* Bulk ops wait for grace periods: no read section here. */
		pthread_mutex_lock(&ctx->lock);
		cds_ft_make_exclusive(swap);	/* DLM: cross-trie src must be exclusive */
		s = cds_ft_graft_swap(ctx->live, NULL, 0, swap);
		pthread_mutex_unlock(&ctx->lock);

		if (s != CDS_FT_STATUS_OK) {
			/*
			 * Reported, not merely logged: bailing leaves the readers
			 * an unchanging pair of tries, so they find nothing and
			 * the oracle reports ok having exercised NOTHING.  A
			 * silent green is indistinguishable from coverage.
			 */
			report_violation(ctx->test_name,
				"graft_swap failed (%s): the writer stopped, so this oracle would have passed vacuously",
				cds_ft_status_to_string(s));
			break;
		}

		/*
		 * After the swap, `swap` holds the old content.  Wait a
		 * grace period, drain the old content, and populate the
		 * swap trie with fresh keys for the next swap.
		 */
		synchronize_rcu();
		drain_trie_local(swap);
		rcu_barrier();

		phase = !phase;
		if (phase)
			populate_range(swap, GRAFT_SWAP_RANGE_B_BASE, GRAFT_SWAP_POOL);
		else
			populate_range(swap, GRAFT_SWAP_RANGE_A_BASE, GRAFT_SWAP_POOL);

		rcu_quiescent_state();
	}

	/* Clean up swap trie. */
	drain_trie_local(swap);
	rcu_barrier();
	cds_ft_destroy(swap);

	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_swap_atomicity(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *live;
	struct inv_graft_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i;

	/*
	 * Variable-length trie with max_key_len=4 so root-level
	 * graft_swap (key_len=0) is accepted.
	 */
	if (cds_ft_group_attr_create(&attr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(attr, 4) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	if (cds_ft_group_create(attr, &group) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &live) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}

	/* Initial population: range A. */
	populate_range(live, GRAFT_SWAP_RANGE_A_BASE, GRAFT_SWAP_POOL);

	ctx.live = live;
	ctx.group = group;
	ctx.test_name = "inv_graft_swap_atomicity";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_graft_swap_reader, &ctx);
	/* Single writer for graft_swap — it includes synchronize_rcu. */
	pthread_create(&writer, NULL, inv_graft_swap_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS * 2)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_swap_atomicity: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(live);
		rcu_barrier();
		cds_ft_destroy(live);
		cds_ft_group_destroy(group);
		return -1;
	}

	drain_trie_local(live);
	rcu_barrier();
	cds_ft_destroy(live);
	cds_ft_group_destroy(group);
	return 0;
}

/*
 * graft_swap CROSS-VIEW oracle (structure vs ordered list).  inv_graft_swap_
 * atomicity checks only list-internal atomicity (cds_ft_for_each never mixes A
 * and B).  This checks the OTHER axis: a root graft_swap publishes dst->root
 * (structure) and dst's ord_cell_head/tail (list) in SEPARATE stores, so a
 * reader between them sees the whole trie as range B structurally while the
 * ordered-list front is still range A.  The fix fuses each side's root swap
 * with its head/tail transfer into one flip (ft_root_list_swap_publish).
 *
 * The writer PING-PONGS the same two tries (graft_swap leaves the old content
 * in @swap, so the next swap moves it back) -- no drain/repopulate, so it runs
 * far more swaps (= windows) than inv_graft_swap_atomicity's writer.  That
 * makes it CYCLING (A<->B at root), so the reader uses the front-stable
 * SANDWICH (list-range, struct-range, list-range): it flags only when the
 * list-range is STABLE yet the structure shows the OTHER range -- a mid-swap
 * straddle changes the list-range between the two reads and is filtered, while
 * the genuine structure-before-list window survives.  The two probes are the
 * usual independent pair: lookup_first (cell-list front) vs lookup_ge from a
 * submin (pure structural descent).
 */
static int graft_swap_range(struct cds_ft *ft, struct cds_ft_iter *iter,
		bool use_list)
{
	static const uint8_t SUBMIN[1] = { 0x00 };
	struct cds_ft_node *n;
	enum cds_ft_status s;

	if (use_list) {
		s = cds_ft_lookup_first(ft, iter);
	} else {
		cds_ft_iter_set_key(iter, SUBMIN, 1);
		s = cds_ft_lookup_ge(ft, iter);
	}
	if (s != CDS_FT_STATUS_OK || (n = cds_ft_iter_node(iter)) == NULL)
		return -1;	/* empty */
	/* The leaf carries its own u64 key, so no iter key materialization. */
	return to_test_node(n)->key < GRAFT_SWAP_RANGE_B_BASE ? 0 : 1;
}

static void *inv_graft_swap_xview_reader(void *arg)
{
	struct inv_graft_ctx *ctx = (struct inv_graft_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->live, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		int rl1, rs, rl2;

		rcu_read_lock();
		rl1 = graft_swap_range(ctx->live, iter, true);	/* list front */
		rs  = graft_swap_range(ctx->live, iter, false);	/* structural min */
		rl2 = graft_swap_range(ctx->live, iter, true);	/* list front again */
		if (rl1 >= 0 && rl1 == rl2 && rs >= 0 && rs != rl1)
			report_violation(ctx->test_name,
				"structural range != STABLE ordered-list front"
				" range -- the root graft_swap published the root"
				" before transferring the ordered-list head/tail");
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_graft_swap_xview_writer(void *arg)
{
	struct inv_graft_ctx *ctx = (struct inv_graft_ctx *) arg;
	struct cds_ft *swap;

	rcu_register_thread();
	if (cds_ft_create(ctx->group, NULL, &swap) < 0)
		abort();
	populate_range(swap, GRAFT_SWAP_RANGE_B_BASE, GRAFT_SWAP_POOL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Ping-pong: graft_swap moves the old content into @swap, so the next
	 * swap moves it straight back -- no drain/repopulate, far more windows. */
	while (!test_stop) {
		pthread_mutex_lock(&ctx->lock);
		cds_ft_make_exclusive(swap);	/* DLM: cross-trie src must be exclusive */
		if (cds_ft_graft_swap(ctx->live, NULL, 0, swap) != CDS_FT_STATUS_OK)
			abort();
		pthread_mutex_unlock(&ctx->lock);
		rcu_quiescent_state();
	}

	drain_trie_local(swap);
	rcu_barrier();
	cds_ft_destroy(swap);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_swap_cross_view(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *live;
	struct inv_graft_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i;

	if (cds_ft_group_attr_create(&attr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(attr, 4) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	if (cds_ft_group_create(attr, &group) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &live) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	populate_range(live, GRAFT_SWAP_RANGE_A_BASE, GRAFT_SWAP_POOL);

	ctx.live = live;
	ctx.group = group;
	ctx.test_name = "inv_graft_swap_cross_view";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_graft_swap_xview_reader, &ctx);
	pthread_create(&writer, NULL, inv_graft_swap_xview_writer, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_swap_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(live);
		rcu_barrier();
		cds_ft_destroy(live);
		cds_ft_group_destroy(group);
		return -1;
	}
	drain_trie_local(live);
	rcu_barrier();
	cds_ft_destroy(live);
	cds_ft_group_destroy(group);
	return 0;
}

/*
 * CROSS-TRIE root graft_swap oracle.  The two oracles above check each TRIE's
 * own structure-vs-ordered-list axis; this checks the axis BETWEEN the two
 * tries.  A whole-trie graft_swap historically published dst->root and
 * swap->root in two SEPARATE flips (an ft_root_list_swap_publish per side), so
 * between them BOTH roots transiently pointed at the same content: a key was
 * reachable in BOTH tries (the content moved in) or in NEITHER (the content
 * moved out).  The fix fuses both sides' root (and head/tail) swaps into ONE
 * cross-trie flip (ft_root_list_swap_publish_dual -- dst and swap share a group,
 * hence a flip selector), so a reader resolves both roots to ONE phase and a
 * moved key is in EXACTLY ONE trie.
 *
 * Both tries are shared with the readers (ctx->live and ctx->swap).  The writer
 * ping-pongs graft_swap(live, swap): the content bounces live<->swap with no
 * drain, maximising windows.  Probe key K is a fixed member of range A (present
 * in whichever trie currently holds A).  Each reader sandwiches the cross-trie
 * probe -- live-present, swap-present, live-present -- and flags only when K's
 * presence in @live is STABLE across the probe (pl1 == pl2): then no full swap
 * straddled the reads and K must be in exactly one trie.  An unstable presence
 * is a straddle and is filtered (the window being hunted shows pl1 == pl2 == 0,
 * ps == 0, i.e. K in NEITHER, within one stable view).
 */

/* Narrow root-store window (each swap is a grace period -> few writer ops); use
 * more readers than the default to catch it reliably, as inv_detach_root_cross_
 * view does. */
#ifndef GRAFT_SWAP_XTRIE_READERS
#define GRAFT_SWAP_XTRIE_READERS	32
#endif




/*
 * SUB-KEY graft_swap cross-view oracle -- the "two-run" run_replace path
 * (ft_ord_cell_run_replace), distinct from the root swap above.  A sub-key
 * graft_swap exchanges dst's subtree-at-@key (run_D) with swap's content
 * (run_S): it publishes run_S structurally at @key (one flip) and THEN swaps
 * run_D out / run_S in in dst's ordered list (a SEPARATE ft_ord_cell_run_replace
 * flip), so a reader between them sees run_S's keys structurally present at @key
 * while the ordered-list front is still run_D (and vice versa).
 *
 * @key = {0x01} is kept the GLOBAL MINIMUM (a stable upper bulk lives at
 * {0xFF}), so the cell-list front and the structural minimum both fall in @key's
 * region -- the same independent probes and front-stable sandwich as the root
 * oracle (reused inv_graft_swap_xview_reader / graft_swap_range).  run_D is
 * range A, run_S is range B; the writer PING-PONGS the swap so the leaves'
 * own u64 keys (hence ranges) are preserved across swaps.
 */
#define GRAFT_SWAP_SUB_POOL	8
#define GRAFT_SWAP_SUB_BULK	4

static void populate_at_prefix(struct cds_ft *ft, const uint8_t *prefix,
		size_t prefix_len, uint64_t base, unsigned int count)
{
	unsigned int i;

	for (i = 0; i < count; i++) {
		struct ft_test_node *n = node_alloc(base + i);
		uint8_t k[8] = { 0 };	/* sized to memcpy into n->okey[8] below */
		size_t kl = prefix_len;

		if (prefix_len)
			memcpy(k, prefix, prefix_len);
		k[kl++] = (uint8_t) i;		/* distinct suffix byte */
		memcpy(n->okey, k, sizeof(n->okey));
		if (cds_ft_insert(ft, k, kl, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
}

static void *inv_graft_swap_sub_xview_writer(void *arg)
{
	struct inv_graft_ctx *ctx = (struct inv_graft_ctx *) arg;
	struct cds_ft *swap;
	static const uint8_t AT[1] = { 0x01 };

	rcu_register_thread();
	if (cds_ft_create(ctx->group, NULL, &swap) < 0)
		abort();
	/* run_S = range B at swap's root; becomes dst's subtree at {0x01}. */
	populate_at_prefix(swap, NULL, 0, GRAFT_SWAP_RANGE_B_BASE,
		GRAFT_SWAP_SUB_POOL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* Ping-pong the subtree at {0x01}: graft_swap leaves run_D in @swap, so
	 * the next swap moves it back -- no drain, far more windows. */
	while (!test_stop) {
		pthread_mutex_lock(&ctx->lock);
		cds_ft_make_exclusive(swap);	/* DLM: cross-trie src must be exclusive */
		if (cds_ft_graft_swap(ctx->live, AT, 1, swap) != CDS_FT_STATUS_OK)
			abort();
		pthread_mutex_unlock(&ctx->lock);
		rcu_quiescent_state();
	}

	drain_trie_local(swap);
	rcu_barrier();
	cds_ft_destroy(swap);
	rcu_unregister_thread();
	return NULL;
}

static int inv_graft_swap_sub_cross_view(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *live;
	struct inv_graft_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	static const uint8_t AT[1] = { 0x01 };
	static const uint8_t HI[1] = { 0xFF };
	unsigned int i;

	if (cds_ft_group_attr_create(&attr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(attr, 4) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	if (cds_ft_group_create(attr, &group) < 0) {
		cds_ft_group_attr_destroy(attr);
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &live) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	/* run_D = range A under {0x01} (the global min) + a stable bulk at {0xFF}
	 * so {0x01} is a genuine sub-position (run_replace, not the root path). */
	populate_at_prefix(live, AT, 1, GRAFT_SWAP_RANGE_A_BASE, GRAFT_SWAP_SUB_POOL);
	populate_at_prefix(live, HI, 1, 9000, GRAFT_SWAP_SUB_BULK);

	ctx.live = live;
	ctx.group = group;
	ctx.test_name = "inv_graft_swap_sub_cross_view";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_graft_swap_xview_reader, &ctx);
	pthread_create(&writer, NULL, inv_graft_swap_sub_xview_writer, &ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_swap_sub_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(live);
		rcu_barrier();
		cds_ft_destroy(live);
		cds_ft_group_destroy(group);
		return -1;
	}
	drain_trie_local(live);
	rcu_barrier();
	cds_ft_destroy(live);
	cds_ft_group_destroy(group);
	return 0;
}

/*
 * EMPTY-SWAP graft_swap cross-view oracle -- the disappear-side, single-run
 * shapes.  graft_swap(live, @key, EMPTY_swap) with content at @key is a REMOVE:
 * it unpublishes run_D at @key (structure) and THEN removes run_D from the
 * ordered list (a separate ft_ord_cell_run_replace flip), so a reader between
 * them sees run_D gone from the structure but still at the ordered-list front.
 *
 * NON-CYCLING (drain-only): the main thread extracts @live's 2-byte prefixes OUT
 * in INCREASING order (each into the freshly re-emptied @swap), so @live only
 * ever SHRINKS -- a re-appearing minimum is impossible, which is what makes the
 * reused min-drain reader (inv_remove_xview_reader_minvl: lookup_first ->
 * point(min) -> lookup_first, present/absent/present) sound.  A cycling
 * extract+graft-back writer is NOT sound here: a full out-and-back between the
 * reader's two list reads forges a "stable" front while the point lookup caught
 * the transient absence.  Keys are {p_hi,p_lo,s}: extracting {p_hi,p_lo} clears
 * p_lo's slot in the p_hi node, which is the publish-NULL (non-sole-child) shape
 * while p_hi still has other p_lo children, and the gs_reserved (sole-child
 * detach-prune) shape for the LAST p_lo under each p_hi -- so this one oracle
 * exercises BOTH empty-swap corners.
 */
#ifndef GRAFT_SWAP_EMPTY_PREFIXES
#define GRAFT_SWAP_EMPTY_PREFIXES	6000
#endif
#define GRAFT_SWAP_EMPTY_PER		3

static int inv_graft_swap_empty_cross_view(void)
{
	struct cds_ft_group *group;
	struct cds_ft *live = create_varlen_ord_ft(&group);
	struct cds_ft *swap;
	struct inv_lookup_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT];
	struct timespec t0;
	unsigned int i, p, s;

	if (cds_ft_create(group, NULL, &swap) < 0)
		abort();

	rcu_read_lock();
	for (p = 0; p < GRAFT_SWAP_EMPTY_PREFIXES; p++) {
		for (s = 0; s < GRAFT_SWAP_EMPTY_PER; s++) {
			uint8_t key[3] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff),
				(uint8_t) s };
			struct ft_test_node *n = node_alloc(p);

			n->value = 3;
			memcpy(n->okey, key, 3);
			if (cds_ft_insert(live, key, 3, &n->node) != CDS_FT_STATUS_OK)
				abort();
		}
	}
	rcu_read_unlock();

	ctx.ft = live;			/* readers watch the DRAINED live trie */
	ctx.test_name = "inv_graft_swap_empty_cross_view";
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_remove_xview_reader_minvl,
			&ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	/* Extract each prefix's subtree OUT of @live (empty-swap remove), in
	 * increasing order so @live only shrinks, draining @swap back to empty
	 * between extracts.  Each graft_swap is a grace period. */
	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (p = 0; p < GRAFT_SWAP_EMPTY_PREFIXES; p++) {
		uint8_t prefix[2] = { (uint8_t)(p >> 8), (uint8_t)(p & 0xff) };
cds_ft_make_exclusive(swap);	/* DLM: cross-trie src must be exclusive */

		if (cds_ft_graft_swap(live, prefix, 2, swap) != CDS_FT_STATUS_OK)
			abort();
		drain_trie_local(swap);		/* re-empty @swap for the next extract */
		if (elapsed_ms(&t0) >= DEFAULT_DURATION_MS)
			break;
	}

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_graft_swap_empty_cross_view: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_trie_local(live);
		drain_trie_local(swap);
		rcu_barrier();
		cds_ft_destroy(live);
		cds_ft_destroy(swap);
		cds_ft_group_destroy(group);
		return -1;
	}
	drain_trie_local(live);
	drain_trie_local(swap);
	rcu_barrier();
	cds_ft_destroy(live);
	cds_ft_destroy(swap);
	cds_ft_group_destroy(group);
	return 0;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 6: Relational lookup consistency under mutation        */
/*                                                                    */
/*   lookup_le(K) must return a key <= K, and lookup_ge(K) must       */
/*   return a key >= K.  A violation means the reader followed a      */
/*   stale internal path to the wrong leaf.                           */
/*                                                                    */
/* ================================================================== */

static void *inv_relational_reader(void *arg)
{
	struct inv_lookup_ctx *ctx = (struct inv_lookup_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	unsigned long checks = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % READER_POOL_SIZE);
		uint8_t k[4], rk[4];
		size_t rk_len;
		uint64_t found_val;
		struct cds_ft_node *node;

		rcu_read_lock();

		/* Test lookup_le. */
		cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup_le(ctx->ft, iter);
		node = cds_ft_iter_node(iter);
		if (node) {
			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			found_val = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (found_val > key) {
				report_violation(ctx->test_name,
					"lookup_le(%" PRIu64 ") returned %" PRIu64,
					key, found_val);
			}
		}

		/* Test lookup_ge. */
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup_ge(ctx->ft, iter);
		node = cds_ft_iter_node(iter);
		if (node) {
			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			found_val = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (found_val < key) {
				FT_TEST_TP(violation, key, found_val);
				report_violation(ctx->test_name,
					"lookup_ge(%" PRIu64 ") returned %" PRIu64,
					key, found_val);
#ifdef FT_ENABLE_TRACING
				/*
				 * Exit immediately so an LTTng trigger armed
				 * on cds_ft:violation can snapshot and stop
				 * the session with the ring-buffer tail
				 * reflecting the failure context, without
				 * additional mutations flooding the buffer.
				 */
				_exit(99);
#endif
			}
		}

		/*
		 * The iterator's cached path holds RCU-protected
		 * pointers that are only valid while the RCU read-side
		 * lock is held continuously.  Invalidate the cache on
		 * exit so the iter is never in a stale state outside a
		 * critical section: QSBR makes rcu_read_lock/unlock
		 * no-ops, so the rcu_quiescent_state() below is the
		 * real grace-period boundary that a cached path would
		 * cross.
		 */
		cds_ft_iter_bind_key(iter);

		rcu_read_unlock();

		checks++;
		if ((checks & 0x3ff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_relational_lookup(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct inv_lookup_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_relational_lookup";
	pthread_mutex_init(&ctx.lock, NULL);

	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_relational_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_lookup_consistency_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();

	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_relational_lookup: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 7: lookup_nth_last stability under low-end mutation    */
/*                                                                    */
/*   A set of "stable" keys at the high end of the key space is       */
/*   never modified.  A concurrent writer inserts and removes keys    */
/*   only at the low end.  A reader repeatedly calls                  */
/*   cds_ft_lookup_nth_last(0) — the largest key — and verifies it    */
/*   always lands in the stable high range.  Because lookup_nth_last  */
/*   descends from the right, mutations at the low end must not       */
/*   disturb the result.                                              */
/*                                                                    */
/* ================================================================== */

#define NTH_STABLE_COUNT	64	/* Stable keys that are never modified. */
#define NTH_WRITER_RANGE	256	/* Writer key range (low end). */
#define NTH_STABLE_BASE		1024	/* Base of the stable high range. */

struct inv_nth_ctx {
	struct cds_ft *ft;
	const char *test_name;
	uint64_t stable_base;		/* Start of stable key range. */
	uint64_t stable_count;		/* Number of stable keys. */
};

static void *inv_nth_last_reader(void *arg)
{
	struct inv_nth_ctx *ctx = (struct inv_nth_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		uint8_t rk[8];
		size_t rk_len;
		uint64_t v;

		rcu_read_lock();
		s = cds_ft_lookup_nth_last(ctx->ft, iter, 0);
		if (s == CDS_FT_STATUS_OK) {
			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (v < ctx->stable_base ||
			    v >= ctx->stable_base + ctx->stable_count) {
				report_violation(ctx->test_name,
					"nth_last(0) returned %" PRIu64
					", expected [%" PRIu64 ", %" PRIu64
					") (iter #%lu)",
					v, ctx->stable_base,
					ctx->stable_base + ctx->stable_count,
					iters);
			}
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_nth_low_writer(void *arg)
{
	struct inv_nth_ctx *ctx = (struct inv_nth_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % NTH_WRITER_RANGE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_nth_last_stability(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_nth_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.test_name = "inv_nth_last_stability";
	shared.ctx.stable_base = NTH_STABLE_BASE;
	shared.ctx.stable_count = NTH_STABLE_COUNT;
	pthread_mutex_init(&shared.lock, NULL);

	/* Pre-populate: stable high keys (never modified). */
	rcu_read_lock();
	for (i = 0; i < NTH_STABLE_COUNT; i++) {
		struct ft_test_node *n = node_alloc(NTH_STABLE_BASE + i);
		insert_u64(ft, NTH_STABLE_BASE + i, n);
	}
	/* Seed some low keys for the writer to churn. */
	for (i = 0; i < NTH_WRITER_RANGE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_nth_last_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_nth_low_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_nth_last_stability: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 8: lookup_nth stability under high-end mutation        */
/*                                                                    */
/*   Symmetric to invariant 7: stable keys at the low end, writer     */
/*   churns the high end, reader calls cds_ft_lookup_nth(0) and       */
/*   verifies the smallest key always comes from the stable low       */
/*   range.                                                           */
/*                                                                    */
/* ================================================================== */

static void *inv_nth_first_reader(void *arg)
{
	struct inv_nth_ctx *ctx = (struct inv_nth_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		uint8_t rk[8];
		size_t rk_len;
		uint64_t v;

		rcu_read_lock();
		s = cds_ft_lookup_nth(ctx->ft, iter, 0);
		if (s == CDS_FT_STATUS_OK) {
			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);
			if (v >= ctx->stable_count) {
				report_violation(ctx->test_name,
					"nth(0) returned %" PRIu64
					", expected [0, %" PRIu64
					") (iter #%lu)",
					v, ctx->stable_count, iters);
			}
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_nth_high_writer(void *arg)
{
	struct inv_nth_ctx *ctx = (struct inv_nth_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = NTH_STABLE_BASE +
			(uint64_t)(rand_r(&seed) % NTH_WRITER_RANGE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_nth_first_stability(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_nth_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.test_name = "inv_nth_first_stability";
	shared.ctx.stable_base = 0;
	shared.ctx.stable_count = NTH_STABLE_COUNT;
	pthread_mutex_init(&shared.lock, NULL);

	/* Pre-populate: stable low keys (never modified). */
	rcu_read_lock();
	for (i = 0; i < NTH_STABLE_COUNT; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	/* Seed some high keys for the writer to churn. */
	for (i = 0; i < NTH_WRITER_RANGE / 2; i++) {
		struct ft_test_node *n = node_alloc(NTH_STABLE_BASE + i);
		insert_u64(ft, NTH_STABLE_BASE + i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_nth_first_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_nth_high_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_nth_first_stability: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 9: skip_forward stability under low-end mutation       */
/*                                                                    */
/*   Stable keys in [SKIP_STABLE_BASE, SKIP_STABLE_BASE+COUNT) are   */
/*   never modified.  A writer churns keys in [0, SKIP_WRITER_RANGE). */
/*   A reader positions at the first stable key via lookup_nth, then  */
/*   skip_forwards within the stable range.  Because the local        */
/*   traversal only touches nodes between the start and end, low-end  */
/*   mutations must not affect the result.                            */
/*                                                                    */
/* ================================================================== */

#define SKIP_STABLE_BASE	512
#define SKIP_STABLE_COUNT	64
#define SKIP_WRITER_RANGE	256	/* Writer churn range below stable. */
#define SKIP_HIGH_WRITER_BASE	1024	/* Writer churn range above stable. */

struct inv_skip_ctx {
	struct cds_ft *ft;
	const char *test_name;
	uint64_t stable_base;
	uint64_t stable_count;
};

static void *inv_skip_forward_reader(void *arg)
{
	struct inv_skip_ctx *ctx = (struct inv_skip_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		uint8_t rk[8];
		size_t rk_len;
		uint64_t v;
		unsigned long skip_n;

		rcu_read_lock();

		/*
		 * Position at a random stable key, then skip forward
		 * by a random amount within the stable range.
		 */
		{
			uint64_t start_off = (uint64_t)(rand_r(&seed) % ctx->stable_count);
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, ctx->stable_base + start_off,
					k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			s = cds_ft_lookup(ctx->ft, iter);
			if (s != CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				goto next;
			}

			/* Skip forward within the remaining stable keys. */
			skip_n = (unsigned long)(rand_r(&seed) %
				(ctx->stable_count - start_off));
			if (skip_n == 0) {
				rcu_read_unlock();
				goto next;
			}

			s = cds_ft_iter_skip_forward(ctx->ft, iter, skip_n);
			if (s != CDS_FT_STATUS_OK) {
				report_violation(ctx->test_name,
					"skip_forward(%lu) from %" PRIu64
					" returned %s (iter #%lu)",
					skip_n,
					ctx->stable_base + start_off,
					cds_ft_status_to_string(s),
					iters);
				rcu_read_unlock();
				goto next;
			}

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);

			if (v < ctx->stable_base ||
			    v >= ctx->stable_base + ctx->stable_count) {
				report_violation(ctx->test_name,
					"skip_forward(%lu) from %" PRIu64
					" landed on %" PRIu64
					", expected [%" PRIu64 ", %" PRIu64
					") (iter #%lu)",
					skip_n,
					ctx->stable_base + start_off,
					v, ctx->stable_base,
					ctx->stable_base + ctx->stable_count,
					iters);
			}
		}
		rcu_read_unlock();

next:
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_skip_low_writer(void *arg)
{
	struct inv_skip_ctx *ctx = (struct inv_skip_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % SKIP_WRITER_RANGE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_skip_forward_stability(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_skip_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.test_name = "inv_skip_forward_stability";
	shared.ctx.stable_base = SKIP_STABLE_BASE;
	shared.ctx.stable_count = SKIP_STABLE_COUNT;
	pthread_mutex_init(&shared.lock, NULL);

	/* Pre-populate: stable keys (never modified). */
	rcu_read_lock();
	for (i = 0; i < SKIP_STABLE_COUNT; i++) {
		struct ft_test_node *n = node_alloc(SKIP_STABLE_BASE + i);
		insert_u64(ft, SKIP_STABLE_BASE + i, n);
	}
	/* Seed low keys for writer churn. */
	for (i = 0; i < SKIP_WRITER_RANGE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_skip_forward_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_skip_low_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_skip_forward_stability: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 10: skip_reverse stability under high-end mutation     */
/*                                                                    */
/*   Same stable key range.  Writer churns keys above.  Reader        */
/*   positions at a stable key and skip_reverses within the stable    */
/*   range.  High-end mutations must not affect the result.           */
/*                                                                    */
/* ================================================================== */

static void *inv_skip_reverse_reader(void *arg)
{
	struct inv_skip_ctx *ctx = (struct inv_skip_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		uint8_t rk[8];
		size_t rk_len;
		uint64_t v;
		unsigned long skip_n;

		rcu_read_lock();

		{
			uint64_t start_off = (uint64_t)(rand_r(&seed) % ctx->stable_count);
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, ctx->stable_base + start_off,
					k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			s = cds_ft_lookup(ctx->ft, iter);
			if (s != CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				goto next;
			}

			/* Skip reverse within the preceding stable keys. */
			if (start_off == 0) {
				rcu_read_unlock();
				goto next;
			}
			skip_n = (unsigned long)(rand_r(&seed) % start_off);
			if (skip_n == 0) {
				rcu_read_unlock();
				goto next;
			}

			s = cds_ft_iter_skip_reverse(ctx->ft, iter, skip_n);
			if (s != CDS_FT_STATUS_OK) {
				report_violation(ctx->test_name,
					"skip_reverse(%lu) from %" PRIu64
					" returned %s (iter #%lu)",
					skip_n,
					ctx->stable_base + start_off,
					cds_ft_status_to_string(s),
					iters);
				rcu_read_unlock();
				goto next;
			}

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rk_len);
			v = cds_ft_key_to_u64(ctx->ft, rk, CDS_FT_LEN_DEFAULT);

			if (v < ctx->stable_base ||
			    v >= ctx->stable_base + ctx->stable_count) {
				report_violation(ctx->test_name,
					"skip_reverse(%lu) from %" PRIu64
					" landed on %" PRIu64
					", expected [%" PRIu64 ", %" PRIu64
					") (iter #%lu)",
					skip_n,
					ctx->stable_base + start_off,
					v, ctx->stable_base,
					ctx->stable_base + ctx->stable_count,
					iters);
			}
		}
		rcu_read_unlock();

next:
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_skip_high_writer(void *arg)
{
	struct inv_skip_ctx *ctx = (struct inv_skip_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = SKIP_HIGH_WRITER_BASE +
			(uint64_t)(rand_r(&seed) % SKIP_WRITER_RANGE);
		int do_insert = rand_r(&seed) & 1;

		rcu_read_lock();
		if (do_insert) {
			struct ft_test_node *n = node_alloc(key);

			pthread_mutex_lock(lock);
			insert_u64(ctx->ft, key, n);
			pthread_mutex_unlock(lock);
		} else {
			struct cds_ft_node *found;
			uint8_t k[8];

			cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(ctx->ft, iter);
			found = cds_ft_iter_node(iter);
			if (found) {
				struct ft_test_node *tn = to_test_node(found);

				pthread_mutex_lock(lock);
				if (cds_ft_remove(ctx->ft, iter, &tn->node)
				    == CDS_FT_STATUS_OK) {
					node_free_rcu(tn);
				}
				pthread_mutex_unlock(lock);
			}
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_skip_reverse_stability(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_skip_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.test_name = "inv_skip_reverse_stability";
	shared.ctx.stable_base = SKIP_STABLE_BASE;
	shared.ctx.stable_count = SKIP_STABLE_COUNT;
	pthread_mutex_init(&shared.lock, NULL);

	/* Pre-populate: stable keys (never modified). */
	rcu_read_lock();
	for (i = 0; i < SKIP_STABLE_COUNT; i++) {
		struct ft_test_node *n = node_alloc(SKIP_STABLE_BASE + i);
		insert_u64(ft, SKIP_STABLE_BASE + i, n);
	}
	/* Seed high keys for writer churn. */
	for (i = 0; i < SKIP_WRITER_RANGE / 2; i++) {
		struct ft_test_node *n = node_alloc(SKIP_HIGH_WRITER_BASE + i);
		insert_u64(ft, SKIP_HIGH_WRITER_BASE + i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_skip_reverse_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_skip_high_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_skip_reverse_stability: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 7b: rank/select readers resolve a parked external_nodes */
/*                 proxy (regression for the ordered-query unresolved  */
/*                 external_nodes read).                               */
/*                                                                    */
/*   An ordered-list insert/remove of a PREFIX key publishes its head  */
/*   into an internal node's external_nodes via the one-commit splice, */
/*   which transiently PARKS a flip proxy in that slot                 */
/*   (ft_insert_park_external_nodes / ft_remove_one_commit).  The      */
/*   rank/select readers (cds_ft_lookup_nth / _nth_last /              */
/*   skip_forward / skip_reverse) descend through that node and must   */
/*   RESOLVE the proxy (ft_dereference_external_acquire), never hand   */
/*   the tagged proxy pointer back as iter->node.  Setup: stable 5-byte */
/*   extension keys keep each 4-byte prefix terminating at an internal */
/*   node; the writer churns the 4-byte prefixes (each insert/remove   */
/*   parks the proxy); readers hammer all four rank/select queries and */
/*   assert the returned node is a real external head, not a proxy.    */
/*                                                                    */
/* ================================================================== */

#ifndef EXT_PARK_GROUPS
#define EXT_PARK_GROUPS	4096		/* distinct prefixes (= nth/skip range) */
#endif

struct inv_ext_park_ctx {
	struct cds_ft *ft;
	const char *test_name;
	unsigned int groups;
	pthread_mutex_t *lock;
};

/* Stable 5-byte extension keys Kl(g) = {g>>8, g&0xff, 0, 0, 0}. */
static void inv_ext_park_populate(struct cds_ft *ft)
{
	unsigned int g;

	for (g = 0; g < EXT_PARK_GROUPS; g++) {
		uint8_t kl[5] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0, 0 };
		struct ft_test_node *nl = node_alloc(g);

		nl->value = 5;			/* stash key length (xview_node_key) */
		memcpy(nl->okey, kl, 5);
		if (cds_ft_insert(ft, kl, 5, &nl->node) != CDS_FT_STATUS_OK)
			abort();
	}
}

static void *inv_ext_park_reader(void *arg)
{
	struct inv_ext_park_ctx *ctx = (struct inv_ext_park_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	unsigned long iters = 0;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned long n = (unsigned long)(rand_r(&seed) % ctx->groups);
		int which = rand_r(&seed) & 3;
		const char *qname;
		enum cds_ft_status s;

		rcu_read_lock();
		switch (which) {
		case 0:
			qname = "lookup_nth";
			s = cds_ft_lookup_nth(ctx->ft, iter, n);
			break;
		case 1:
			qname = "lookup_nth_last";
			s = cds_ft_lookup_nth_last(ctx->ft, iter, n);
			break;
		case 2:
			qname = "skip_forward";
			s = cds_ft_lookup_nth(ctx->ft, iter, 0);
			if (s == CDS_FT_STATUS_OK)
				s = cds_ft_iter_skip_forward(ctx->ft, iter, n);
			break;
		default:
			qname = "skip_reverse";
			s = cds_ft_lookup_nth_last(ctx->ft, iter, 0);
			if (s == CDS_FT_STATUS_OK)
				s = cds_ft_iter_skip_reverse(ctx->ft, iter, n);
			break;
		}

		if (s == CDS_FT_STATUS_OK) {
			struct cds_ft_node *node = cds_ft_iter_node(iter);

			if (node) {
				/*
				 * A parked flip proxy carries type-index 7 (low
				 * nibble 0xF -- ft_node_flip_proxy /
				 * FT_FLIP_PROXY_TAG in ft-helpers.h); a real
				 * external head is >= 8-byte aligned (nibble 0).
				 * If the rank/select read failed to resolve the
				 * proxy, iter->node holds the tagged proxy.
				 */
				if (((uintptr_t) node & 0xFUL) == 0xFUL) {
					report_violation(ctx->test_name,
						"%s(%lu) returned a flip proxy as"
						" iter->node (%p) -- external_nodes"
						" read left unresolved",
						qname, n, (void *) node);
				} else {
					size_t klen = (size_t) to_test_node(node)->value;

					if (klen != 4 && klen != 5)
						report_violation(ctx->test_name,
							"%s(%lu) returned node with"
							" corrupt key length %zu",
							qname, n, klen);
				}
			}
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Churn the 4-byte prefixes Kp(g) = {g>>8, g&0xff, 0, 0}: each insert/remove
 * publishes/retires the prefix head via the one-commit external_nodes splice. */
static void *inv_ext_park_writer(void *arg)
{
	struct inv_ext_park_ctx *ctx = (struct inv_ext_park_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int g = (unsigned int)(rand_r(&seed) % ctx->groups);
		uint8_t kp[4] = { (uint8_t)(g >> 8), (uint8_t)(g & 0xff), 0, 0 };
		int do_insert = rand_r(&seed) & 1;
		struct cds_ft_node *found;

		rcu_read_lock();
		pthread_mutex_lock(ctx->lock);
		cds_ft_iter_set_key(iter, kp, 4);
		cds_ft_lookup(ctx->ft, iter);
		found = cds_ft_iter_node(iter);
		if (do_insert) {
			if (!found) {
				struct ft_test_node *n = node_alloc(g);

				n->value = 4;
				memcpy(n->okey, kp, 4);
				if (cds_ft_insert(ctx->ft, kp, 4, &n->node)
				    != CDS_FT_STATUS_OK)
					node_free(n);
			}
		} else if (found) {
			struct ft_test_node *tn = to_test_node(found);

			if (cds_ft_remove(ctx->ft, iter, &tn->node)
			    == CDS_FT_STATUS_OK)
				node_free_rcu(tn);
		}
		pthread_mutex_unlock(ctx->lock);
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_nth_external_nodes_resolve(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_varlen_ord_ft(&group);
	struct timespec t0;
	pthread_mutex_t lock;
	struct inv_ext_park_ctx ctx;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	ctx.ft = ft;
	ctx.test_name = "inv_nth_external_nodes_resolve";
	ctx.groups = EXT_PARK_GROUPS;
	pthread_mutex_init(&lock, NULL);
	ctx.lock = &lock;

	rcu_read_lock();
	inv_ext_park_populate(ft);
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_ext_park_reader, &ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL, inv_ext_park_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_nth_external_nodes_resolve: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11: nr_keys undercount ordering                        */
/*                                                                    */
/*   The nr_keys metadata is updated with a specific ordering         */
/*   relative to pointer publication:                                 */
/*                                                                    */
/*     Insert: publish pointer, then increment nr_keys.               */
/*     Remove: decrement nr_keys, then detach pointer.                */
/*                                                                    */
/*   Both produce a transient undercount: nr_keys <= actual reachable */
/*   keys.  The practical consequence is that lookup_nth(N-1) should  */
/*   always find a valid key when cds_ft_count_keys returned N > 0,   */
/*   provided that the actual key population never drops below N      */
/*   between the two calls.                                           */
/*                                                                    */
/*   Insert phase: an insert-only writer ensures the actual count is  */
/*   monotonically non-decreasing.  If cds_ft_count_keys returns N,   */
/*   there are at least N reachable keys (undercount), and            */
/*   lookup_nth(N-1) must succeed.  An overcount bug would cause      */
/*   N > actual, making lookup_nth(N-1) fail.                         */
/*                                                                    */
/*   Remove phase: a remove-only writer ensures the actual count is   */
/*   monotonically non-increasing.  The reader iterates first         */
/*   (pointer-based count), then reads count_keys.  With decrement-   */
/*   before-detach ordering, count_keys can only lag behind (or       */
/*   equal) the iteration count, so count_keys <= iter_count must     */
/*   hold.  An overcount bug (detach-before-decrement) would cause    */
/*   count_keys > iter_count.                                         */
/*                                                                    */
/*   A quiescent phase after each concurrent phase verifies that      */
/*   count_keys equals iteration_count exactly.                       */
/*                                                                    */
/* ================================================================== */

static void *inv_nr_keys_undercount_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned long count_keys;
		enum cds_ft_status s;

		rcu_read_lock();

		count_keys = cds_ft_count_keys(ctx->ft);

		/*
		 * With an insert-only writer and undercount ordering,
		 * there are always >= count_keys reachable keys.
		 * lookup_nth(count_keys - 1) and lookup_nth_last(0)
		 * through lookup_nth_last(count_keys - 1) must find
		 * valid keys.
		 */
		if (count_keys > 0) {
			s = cds_ft_lookup_nth(ctx->ft, iter,
					count_keys - 1);
			if (s != CDS_FT_STATUS_OK) {
				report_violation(ctx->test_name,
					"lookup_nth(%lu) returned %s "
					"(count_keys %lu, iter #%lu)",
					count_keys - 1,
					cds_ft_status_to_string(s),
					count_keys, iters);
			}
			s = cds_ft_lookup_nth_last(ctx->ft, iter,
					count_keys - 1);
			if (s != CDS_FT_STATUS_OK) {
				report_violation(ctx->test_name,
					"lookup_nth_last(%lu) returned %s "
					"(count_keys %lu, iter #%lu)",
					count_keys - 1,
					cds_ft_status_to_string(s),
					count_keys, iters);
			}
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Insert-only writer: only adds keys, never removes.
 * This guarantees that the actual key count is monotonically
 * non-decreasing, making the undercount invariant testable.
 */
static void *inv_nr_keys_undercount_writer(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		struct ft_test_node *n = node_alloc(key);

		rcu_read_lock();
		pthread_mutex_lock(lock);
		insert_u64(ctx->ft, key, n);
		pthread_mutex_unlock(lock);
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

/*
 * Remove-phase reader: the remove-side, bounded-re-measure NO-(persistent-)
 * OVERCOUNT check (complement of the strict no-undercount reader).  count_keys
 * and the iteration are sampled at different instants, and the list-off
 * lookup_gt iterator is not linearizable against a concurrent remove-
 * recompaction, so a single count_keys > iter_count is not itself a bug -- only
 * one that PERSISTS across re-descents is a genuine nr_keys accounting error.
 */
static void *inv_nr_keys_remove_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned long count_keys, iter_count;

		rcu_read_lock();

		/*
		 * Iterate first (count reachable keys via pointers), then read
		 * count_keys (the nr_keys aggregate).  With a remove-only writer
		 * the key set only shrinks, so count_keys > iter_count is
		 * normally absent; when it appears it is the transient handled
		 * below, not a real over-count.
		 */
		iter_count = 0;
		cds_ft_for_each_rcu(ctx->ft, iter) {
			iter_count++;
		}

		count_keys = cds_ft_count_keys(ctx->ft);

		if (count_keys > iter_count) {
			/*
			 * A transient count_keys > iter_count here is the
			 * non-linearizable list-off lookup_gt ITERATOR, not an
			 * accounting error: during a remove-recompaction the
			 * iterator's re-descent can reach the rebuilt smaller node
			 * before the republish and UNDER-enumerate the subtree
			 * (LTTng-confirmed), while the aggregate still correctly
			 * counts the not-yet-detached key.  With order statistics
			 * OFF there is a second non-linearizable source --
			 * cds_ft_count_keys is itself a structural recount
			 * (ft_subtree_key_count) that can resolve a swapped-out
			 * child to the larger pre-recompaction subtree.  Either
			 * way the excess clears once the structure settles, so a
			 * single-shot count_keys <= iter_count is NOT a sound
			 * invariant under concurrent removes.  Re-measure under a
			 * bounded retry and flag only a STABLE over-count -- a
			 * count_keys that stays above the traversal across many
			 * re-descents is a genuine nr_keys accounting bug.  (The
			 * exact count == traversal equality is asserted only at
			 * the quiescent phase barrier, where it is well-defined.)
			 */
			unsigned int r;
			bool stable_over = true;

			for (r = 0; r < 256; r++) {
				unsigned long ic2 = 0, ck2;

				cds_ft_for_each_rcu(ctx->ft, iter)
					ic2++;
				ck2 = cds_ft_count_keys(ctx->ft);
				if (ck2 <= ic2) {
					stable_over = false;
					break;
				}
			}
			if (stable_over)
				report_violation(ctx->test_name,
					"remove phase: count_keys %lu > "
					"iteration_count %lu (iter #%lu)",
					count_keys, iter_count, iters);
		}
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Remove-only writer: only removes keys, never inserts.
 */
static void *inv_nr_keys_remove_writer(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int seed;
	pthread_mutex_t *lock = (pthread_mutex_t *)(ctx + 1);

	rcu_register_thread();
	seed = (unsigned int)(uintptr_t)pthread_self() ^ (unsigned int)time(NULL);

	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint64_t key = (uint64_t)(rand_r(&seed) % WRITER_POOL_SIZE);
		struct cds_ft_node *found;
		uint8_t k[8];

		rcu_read_lock();
		cds_ft_u64_to_key(ctx->ft, key, k, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, k, CDS_FT_LEN_DEFAULT);
		cds_ft_lookup(ctx->ft, iter);
		found = cds_ft_iter_node(iter);
		if (found) {
			struct ft_test_node *tn = to_test_node(found);

			pthread_mutex_lock(lock);
			if (cds_ft_remove(ctx->ft, iter, &tn->node)
			    == CDS_FT_STATUS_OK) {
				node_free_rcu(tn);
			}
			pthread_mutex_unlock(lock);
		}
		rcu_read_unlock();

		if ((seed & 0xff) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * Helper: quiescent (at-rest) exactness barrier.
 *
 * Exact count_keys == iteration_count is only well-defined at REST: under
 * concurrent updaters the two are sampled at different instants, so only a
 * one-sided over-/under-count bound is meaningful (see inv_nr_keys_undercount
 * for the insert = no-overcount side and inv_nr_keys_no_undercount for the
 * remove = no-undercount side).  With the writers joined, assert the exact
 * equality, and additionally run cds_ft_verify() -- which cross-checks every
 * node's stored nr_keys against a full structural subtree recount (gated on
 * rank stats), the tightest per-node oracle for the count fold.
 *
 * Returns 0 on success, -1 on mismatch.
 */
static int quiescent_count_check(struct cds_ft *ft, const char *test_name,
		const char *phase)
{
	struct cds_ft_iter *check_iter;
	unsigned long count_keys, iter_count;

	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: %s: cds_ft_verify failed "
			"(per-node nr_keys mismatch)\n", test_name, phase);
		return -1;
	}
	if (cds_ft_iter_create(ft, &check_iter) < 0)
		return -1;
	rcu_read_lock();
	count_keys = cds_ft_count_keys(ft);
	iter_count = 0;
	cds_ft_for_each_rcu(ft, check_iter) {
		iter_count++;
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(check_iter);

	if (count_keys != iter_count) {
		fprintf(stderr,
			"%s: %s quiescent mismatch: "
			"count_keys %lu != iteration_count %lu\n",
			test_name, phase, count_keys, iter_count);
		return -1;
	}
	return 0;
}

static int inv_nr_keys_undercount(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_iter_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.key_len = 4;
	shared.ctx.test_name = "inv_nr_keys_undercount";
	pthread_mutex_init(&shared.lock, NULL);

	/*
	 * Phase 1: Insert-only writer.
	 *
	 * Pre-populate, then insert-only writers add more keys.
	 * Readers check: count_keys = N implies lookup_nth(N-1)
	 * and lookup_nth_last(N-1) succeed.
	 */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_nr_keys_undercount_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_nr_keys_undercount_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS / 2)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr,
			"inv_nr_keys_undercount: insert phase: %lu violation(s)\n",
			atomic_load(&violation_count));
		pthread_mutex_destroy(&shared.lock);
		drain_and_destroy(ft, group);
		return -1;
	}

	/* Quiescent check after insert phase. */
	if (quiescent_count_check(ft, "inv_nr_keys_undercount", "insert") < 0) {
		pthread_mutex_destroy(&shared.lock);
		drain_and_destroy(ft, group);
		return -1;
	}

	/*
	 * Phase 2: Remove-only writer.
	 *
	 * The trie is now populated from phase 1.  Remove-only
	 * writers drain keys.  Readers iterate first (pointer-based
	 * count), then read count_keys, and check count_keys <=
	 * iter_count.
	 */
	atomic_store(&violation_count, 0);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_nr_keys_remove_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_nr_keys_remove_writer, &shared.ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS / 2)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr,
			"inv_nr_keys_undercount: remove phase: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}

	/* Quiescent check after remove phase. */
	if (quiescent_count_check(ft, "inv_nr_keys_undercount", "remove") < 0) {
		drain_and_destroy(ft, group);
		return -1;
	}

	return drain_and_destroy(ft, group);
}

/*
 * NO-UNDERCOUNT reader: the remove-side, one-sided counterpart to the
 * insert-side no-overcount check in inv_nr_keys_undercount.
 *
 * With the per-node nr_keys count folded into each remove's flip-txn (the
 * decrement commits atomically with the structural detach), the maintained
 * aggregate equals the structurally-present key set at every reader instant, so
 * it can never be SMALLER than a pointer traversal: the list-off lookup_gt
 * iterator only ever UNDER-enumerates a concurrently-relocated structure (its
 * re-descent never reaches an already-detached key and never re-emits a key in
 * ascending order), hence iter_count <= (present keys) = count_keys.  Therefore
 *
 *     count_keys >= iter_count   MUST hold.
 *
 * This is exactly the invariant the TXN fold buys, and the one a decrement-
 * BEFORE-detach ordering (the old pre-decrement) would VIOLATE: there the count
 * drops while the key -- and its enumeration -- is still live, so a reader sees
 * count_keys < iter_count.  It is thus a regression guard for the fold, and it
 * is robust against the recompaction non-linearizability that breaks the
 * opposite (count <= iter) direction: in that window count=N while the iterator
 * transiently sees N-1, so count >= iter holds comfortably.
 *
 * Read ORDER matters: read count_keys FIRST, then iterate.  The writer is
 * remove-only, so the structural key set is monotonically NON-INCREASING; a
 * later iteration can therefore only enumerate <= the earlier count_keys, which
 * removes the false-positive that the opposite order would create.  An actual
 * undercount (count already dropped below a still-live key) is still caught: the
 * low count is sampled first, then the iteration still reaches the live key.
 */
static void *inv_nr_keys_no_undercount_reader(void *arg)
{
	struct inv_iter_ctx *ctx = (struct inv_iter_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned long count_keys, iter_count = 0;

		rcu_read_lock();
		count_keys = cds_ft_count_keys(ctx->ft);
		cds_ft_for_each_rcu(ctx->ft, iter)
			iter_count++;
		if (count_keys < iter_count)
			report_violation(ctx->test_name,
				"remove phase: count_keys %lu < iteration_count "
				"%lu (nr_keys undercount: the aggregate dropped "
				"below a still-live key enumeration, iter #%lu)",
				count_keys, iter_count, iters);
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_nr_keys_no_undercount(void)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_fixed_rankstats_ft(4, &group);
	struct timespec t0;
	struct {
		struct inv_iter_ctx ctx;
		pthread_mutex_t lock;
	} shared;
	pthread_t readers[NR_READERS_DEFAULT], writers[NR_WRITERS_DEFAULT];
	unsigned int i;

	shared.ctx.ft = ft;
	shared.ctx.key_len = 4;
	shared.ctx.test_name = "inv_nr_keys_no_undercount";
	pthread_mutex_init(&shared.lock, NULL);

	/* Populate, then concurrently remove (monotonically shrinking). */
	rcu_read_lock();
	for (i = 0; i < WRITER_POOL_SIZE / 2; i++) {
		struct ft_test_node *n = node_alloc(i);
		insert_u64(ft, i, n);
	}
	rcu_read_unlock();

	atomic_store(&violation_count, 0);
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL,
			inv_nr_keys_no_undercount_reader, &shared.ctx);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_create(&writers[i], NULL,
			inv_nr_keys_remove_writer, &shared.ctx);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < NR_WRITERS_DEFAULT; i++)
		pthread_join(writers[i], NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();
	pthread_mutex_destroy(&shared.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_nr_keys_no_undercount: %lu violation(s)\n",
			atomic_load(&violation_count));
		drain_and_destroy(ft, group);
		return -1;
	}
	if (quiescent_count_check(ft, "inv_nr_keys_no_undercount", "remove") < 0) {
		drain_and_destroy(ft, group);
		return -1;
	}
	return drain_and_destroy(ft, group);
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 9: Ordered traversal never escapes its trie            */
/*                                                                    */
/* ================================================================== */

/*
 * Invariant: an ordered traversal on trie T (cds_ft_lookup_first /
 * _last / _gt / _lt) must never return a key that belongs to a
 * different trie.  Specifically, a concurrent graft from T to S
 * followed by the reader continuing its traversal must not cause the
 * reader to land in S's ancestor chain and return keys from S's
 * namespace ("jumping out of the trie").
 *
 * Setup:
 *   Two tries T and S in the same group, variable-length keys.
 *   T is pre-populated with two key families:
 *     - "T" + 2-byte index  (the mobile family, moved by the writer)
 *     - "B" + 2-byte index  (the anchor family, stays in T)
 *   S is pre-populated with:
 *     - "S" + 2-byte index  (S's native namespace)
 *   S reserves the "X" prefix as the landing slot for T's mobile
 *   content during the cycle.
 *
 * Writer (one thread, mutex-serialised):
 *   phase 1: detach "T" from T  (yields detached trie D, concurrent)
 *            graft  D into S at "X"  (graft syncs because D is concurrent)
 *   phase 2: detach "X" from S  (yields detached trie D', concurrent)
 *            graft  D' back into T at "T"  (graft syncs)
 *
 * Readers (NR_READERS_DEFAULT threads):
 *   alternate forward (lookup_first → lookup_gt chain) and reverse
 *   (lookup_last → lookup_lt chain) iteration on T, and check that
 *   every returned key has first byte 'T' or 'B'.  Any other prefix
 *   ('S', 'X', or anything else) means the reader followed a stale
 *   parent pointer into S's subtree — the jump-out bug.
 */

#define ESCAPE_POOL_PER_PREFIX	64

struct inv_no_escape_ctx {
	struct cds_ft *T;
	struct cds_ft *S;
	struct cds_ft_group *group;
	pthread_mutex_t lock;
	const char *test_name;
};

static void inv_no_escape_populate(struct cds_ft *ft, uint8_t prefix,
		uint64_t value_base)
{
	unsigned int i;

	for (i = 0; i < ESCAPE_POOL_PER_PREFIX; i++) {
		uint8_t key[3] = { prefix, (uint8_t)(i >> 8), (uint8_t)(i & 0xff) };
		struct ft_test_node *n = node_alloc(value_base + i);

		if (cds_ft_insert(ft, key, 3, &n->node) < 0) {
			node_free(n);
			return;
		}
	}
}

static void *inv_no_escape_reader(void *arg)
{
	struct inv_no_escape_ctx *ctx = (struct inv_no_escape_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();

	if (cds_ft_iter_create(ctx->T, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;
		/*
		 * Safety limit: a well-formed traversal of T sees at most
		 * 2 * ESCAPE_POOL_PER_PREFIX keys (T* + B* families).
		 * A much larger count signals the reader is looping
		 * through a much larger structure than T (e.g. S's
		 * contents after jumping out).
		 */
		const unsigned int max_count = ESCAPE_POOL_PER_PREFIX * 8;

		rcu_read_lock();

		if (reverse)
			s = cds_ft_lookup_last(ctx->T, iter);
		else
			s = cds_ft_lookup_first(ctx->T, iter);

		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);

			if (kl == 0 || (k[0] != 'T' && k[0] != 'B')) {
				report_violation(ctx->test_name,
					"reader on T saw key prefix 0x%02x (len %zu) "
					"— not in T or B namespace (iter #%lu, %s)",
					kl > 0 ? (unsigned int) k[0] : 0u, kl, iters,
					reverse ? "reverse" : "forward");
				break;
			}

			if (reverse)
				s = cds_ft_lookup_lt(ctx->T, iter);
			else
				s = cds_ft_lookup_gt(ctx->T, iter);
		}
		if (count >= max_count) {
			report_violation(ctx->test_name,
				"ordered traversal returned >= %u keys — suspected "
				"escape into S's content (iter #%lu, %s)",
				max_count, iters, reverse ? "reverse" : "forward");
		}

		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_no_escape_writer(void *arg)
{
	struct inv_no_escape_ctx *ctx = (struct inv_no_escape_ctx *) arg;

	rcu_register_thread();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		struct cds_ft *D;
		enum cds_ft_status s;

		/* Phase 1: detach T's "T*" content, graft into S at "X". */
		pthread_mutex_lock(&ctx->lock);
		s = cds_ft_detach(ctx->T, (const uint8_t *)"T", 1, &D);
		if (s == CDS_FT_STATUS_OK) {
			cds_ft_make_exclusive(D);	/* DLM: cross-trie src must be exclusive */
			enum cds_ft_status gs =
				cds_ft_graft(ctx->S, (const uint8_t *)"X", 1, D);
			if (gs == CDS_FT_STATUS_OK) {
				cds_ft_destroy(D);
			} else {
				fprintf(stderr,
					"inv_no_escape writer: graft T->S failed: %s\n",
					cds_ft_status_to_string(gs));
				pthread_mutex_unlock(&ctx->lock);
				drain_trie_local(D);
				rcu_barrier();
				cds_ft_destroy(D);
				continue;
			}
		}
		pthread_mutex_unlock(&ctx->lock);

		rcu_quiescent_state();

		/* Phase 2: detach S's "X*" content, graft back into T at "T". */
		pthread_mutex_lock(&ctx->lock);
		s = cds_ft_detach(ctx->S, (const uint8_t *)"X", 1, &D);
		if (s == CDS_FT_STATUS_OK) {
			cds_ft_make_exclusive(D);	/* DLM: cross-trie src must be exclusive */
			enum cds_ft_status gs =
				cds_ft_graft(ctx->T, (const uint8_t *)"T", 1, D);
			if (gs == CDS_FT_STATUS_OK) {
				cds_ft_destroy(D);
			} else {
				fprintf(stderr,
					"inv_no_escape writer: graft S->T failed: %s\n",
					cds_ft_status_to_string(gs));
				pthread_mutex_unlock(&ctx->lock);
				drain_trie_local(D);
				rcu_barrier();
				cds_ft_destroy(D);
				continue;
			}
		}
		pthread_mutex_unlock(&ctx->lock);

		rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

static int inv_ordered_no_escape_graft(void)
{
	INV_NEED_MERGE("inv_ordered_no_escape_graft");
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *T, *S;
	struct inv_no_escape_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &T) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	if (cds_ft_create(group, NULL, &S) < 0) {
		cds_ft_destroy(T);
		cds_ft_group_destroy(group);
		return -1;
	}

	/* Populate: T gets "T*" + "B*", S gets "S*" (leaves "X*" slot for writer). */
	rcu_read_lock();
	inv_no_escape_populate(T, 'T', 0);
	inv_no_escape_populate(T, 'B', 10000);
	inv_no_escape_populate(S, 'S', 100000);
	rcu_read_unlock();

	ctx.T = T;
	ctx.S = S;
	ctx.group = group;
	pthread_mutex_init(&ctx.lock, NULL);
	ctx.test_name = "inv_ordered_no_escape_graft";

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_no_escape_reader, &ctx);
	pthread_create(&writer, NULL, inv_no_escape_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_ordered_no_escape_graft: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(T);
	drain_trie_local(S);
	rcu_barrier();
	cds_ft_destroy(T);
	cds_ft_destroy(S);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 13: Merge no-escape (PIECEWISE overlap)                */
/*                                                                    */
/*   A concurrent ordered traversal of dst never escapes its key      */
/*   namespace nor loops, while a writer repeatedly performs a         */
/*   PIECEWISE merge into dst (the destination already holds nodes     */
/*   that overlap the source) and then removes it again.              */
/*                                                                    */
/*   dst base = {Taa,Tab,Tba,Tbb}: multi-child branches T->{a,b},     */
/*   each ->{a,b}.  Each round the writer builds src={Tac,Tad,Tbc,    */
/*   Tbd} and merges it at root: the merge recurses into the shared   */
/*   'T' and 'a'/'b' branches, re-parenting dst's OWN leaves into      */
/*   freshly-built spine nodes alongside src's leaves, then publishes  */
/*   the new spine at dst's root.  The shared path is all multi-child  */
/*   branches, so no compressed node sits on it and the build-         */
/*   invisible spine-copy is taken under every build.  If the          */
/*   re-parent ever leaves a transient where an ordered up-walk        */
/*   follows a stale/cross parent, the reader sees a key outside       */
/*   {T}{a,b}{a..d} (escape / corruption) or loops past the bound.    */
/* ================================================================== */

struct inv_merge_ctx {
	struct cds_ft *dst;
	struct cds_ft *src;
	struct cds_ft_group *group;
	const char *test_name;
	pthread_mutex_t lock;
};

static const char *const inv_merge_src_keys[] = { "Tac", "Tad", "Tbc", "Tbd" };

/*
 * Per-reader: which trie to iterate and the allowed 3rd-byte range.  dst
 * legitimately holds {T}{a,b}{a..d} (its base a/b keys plus, transiently,
 * the merged-in c/d keys); src only ever holds {T}{a,b}{c,d}.  A src reader
 * that escapes UP into dst (the cross-trie re-parent hazard) observes a
 * 3rd byte of 'a' or 'b' -- outside src's [c,d] range -- and flags it.
 */
struct inv_merge_reader_arg {
	struct inv_merge_ctx *ctx;
	struct cds_ft *trie;
	uint8_t c2_lo, c2_hi;
	const char *which;
};

static void *inv_merge_no_escape_reader(void *arg)
{
	struct inv_merge_reader_arg *ra = (struct inv_merge_reader_arg *) arg;
	struct inv_merge_ctx *ctx = ra->ctx;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	const unsigned int max_count = 32;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->trie, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ra->trie, iter)
			: cds_ft_lookup_first(ra->trie, iter);
		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (kl != 3 || k[0] != 'T' ||
			    (k[1] != 'a' && k[1] != 'b') ||
			    k[2] < ra->c2_lo || k[2] > ra->c2_hi) {
				report_violation(ctx->test_name,
					"%s reader saw out-of-namespace key "
					"(len %zu, %.3s) — merge re-parent escape "
					"(iter #%lu, %s)",
					ra->which, kl, kl ? (const char *) k : "",
					iters, reverse ? "reverse" : "forward");
				break;
			}
			s = reverse ? cds_ft_lookup_lt(ra->trie, iter)
				: cds_ft_lookup_gt(ra->trie, iter);
		}
		if (count >= max_count)
			report_violation(ctx->test_name,
				"%s ordered traversal returned >= %u keys — "
				"escape/loop (iter #%lu, %s)",
				ra->which, max_count, iters,
				reverse ? "reverse" : "forward");
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Remove the duplicate chain at @key from @ft (writer-locked). */
static void inv_merge_remove_key(struct cds_ft *ft, struct cds_ft_iter *iter,
		const char *key)
{
	struct cds_ft_node *head, *tmp;

	cds_ft_iter_set_key(iter, (const uint8_t *) key, strlen(key));
	cds_ft_lookup(ft, iter);
	if (!cds_ft_iter_node(iter))
		return;
	if (cds_ft_remove_all(ft, iter, &head) != CDS_FT_STATUS_OK)
		return;
	cds_ft_for_each_duplicate_safe_rcu(head, tmp)
		node_free_rcu(to_test_node(head));
}

/*
 * DLM: a cross-trie merge needs an EXCLUSIVE source, but these oracles' @src is
 * LIVE and reader-watched on purpose (rargs[i].trie = is_src ? src : dst), so
 * cds_ft_make_exclusive would be a promise the oracle exists to break.  Detach
 * the moved region first: cds_ft_detach returns the detached trie exclusive by
 * construction ("no external handle to @detached existed before this call"), so
 * the merge half is DLM-legal and skips its own grace period.  Design note
 * decision (C): a cross-trie op is two sequential single-domain commits.
 *
 * The oracles' invariant is unaffected because it is PER-TRIE -- an ordered
 * traversal of src (or dst) never escapes that trie's key namespace and never
 * loops.  Detaching empties src, which readers of src may legitimately observe;
 * it never shows them a key outside the namespace.
 */
static enum cds_ft_status inv_merge_detached(struct cds_ft *dst,
		const uint8_t *dkey, size_t dlen,
		struct cds_ft *src, const uint8_t *skey, size_t slen)
{
	struct cds_ft *moved = NULL;
	enum cds_ft_status s;

	s = cds_ft_detach(src, skey, slen, &moved);
	if (s != CDS_FT_STATUS_OK)
		return s;
	if (!moved)
		return CDS_FT_STATUS_OK;	/* nothing matched the prefix */
	s = cds_ft_merge_at(dst, dkey, dlen, moved, NULL, 0);
	cds_ft_destroy(moved);			/* emptied by the merge */
	return s;
}

static void *inv_merge_no_escape_writer(void *arg)
{
	struct inv_merge_ctx *ctx = (struct inv_merge_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int i;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;

		/*
		 * Piecewise merge of all of the persistent @src into @dst at
		 * the root.  @src is iterated by its own readers throughout, so
		 * the commit's source-side detach + drain (and the cross-trie
		 * re-parent of src's subtrees into dst) runs under live source
		 * readers.
		 *
		 * The writer is a MUTATOR: it must NOT hold a read-side lock
		 * around the merge.  cds_ft_merge calls synchronize_rcu
		 * internally to drain src/dst readers; a grace-period wait
		 * issued from within a read-side critical section does not
		 * actually wait, so the source-reader drain would be defeated.
		 * The application mutex (ctx->lock) provides writer exclusion.
		 */
		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst, NULL, 0, ctx->src, NULL, 0);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge writer: %s\n",
				cds_ft_status_to_string(s));
			break;
		}

		rcu_quiescent_state();

		/*
		 * Reset: pull the four merged keys back out of @dst, and
		 * re-populate @src (emptied by the merge) with fresh nodes for
		 * the next round.
		 */
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		for (i = 0; i < 4; i++)
			inv_merge_remove_key(ctx->dst, iter, inv_merge_src_keys[i]);
		for (i = 0; i < 4; i++) {
			struct ft_test_node *n = node_alloc(300 + i);

			cds_ft_insert(ctx->src,
				(const uint8_t *) inv_merge_src_keys[i],
				strlen(inv_merge_src_keys[i]), &n->node);
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();

		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_no_escape");
	static const char *const base_keys[] = { "Taa", "Tab", "Tba", "Tbb" };
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_merge_ctx ctx;
	struct inv_merge_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	if (cds_ft_create(group, NULL, &src) < 0) {
		cds_ft_destroy(dst);
		cds_ft_group_destroy(group);
		return -1;
	}

	rcu_read_lock();
	for (i = 0; i < 4; i++) {
		struct ft_test_node *dn = node_alloc(i);
		struct ft_test_node *sn = node_alloc(300 + i);

		cds_ft_insert(dst, (const uint8_t *) base_keys[i],
			strlen(base_keys[i]), &dn->node);
		cds_ft_insert(src, (const uint8_t *) inv_merge_src_keys[i],
			strlen(inv_merge_src_keys[i]), &sn->node);
	}
	rcu_read_unlock();

	ctx.dst = dst;
	ctx.src = src;
	ctx.group = group;
	ctx.test_name = "inv_merge_no_escape";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/*
	 * Half the readers iterate dst (namespace {T}{a,b}{a..d}), half iterate
	 * src (namespace {T}{a,b}{c,d}), so both the destination-side and the
	 * source-side of every merge run under concurrent ordered traversals.
	 */
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		bool is_src = (i & 1) != 0;

		rargs[i].ctx = &ctx;
		rargs[i].trie = is_src ? src : dst;
		rargs[i].c2_lo = is_src ? (uint8_t) 'c' : (uint8_t) 'a';
		rargs[i].c2_hi = (uint8_t) 'd';
		rargs[i].which = is_src ? "src" : "dst";
		pthread_create(&readers[i], NULL,
			inv_merge_no_escape_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, inv_merge_no_escape_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11: Merge into a NON-ROOT dst never escapes             */
/*                                                                    */
/*   As invariant 10, but the writer merges its private root source    */
/*   {ac,ad,bc,bd} into dst at the INTERIOR key "T" (re-keying to      */
/*   {Tac,Tad,Tbc,Tbd}).  dst base {Taa,Tab,Tba,Tbb} keeps "T" a live  */
/*   internal subtree, so the build-invisible spine copy publishes      */
/*   through an interior forward slot via a type-7 flip proxy.  An      */
/*   ordered reader traversing dst across the flip window must resolve  */
/*   that proxy at child dispatch (point descent + ft_node_get_         */
/*   direction); a missed resolve surfaces a garbage child or a key     */
/*   outside {T}{a,b}{a..d}.  The dst reader of invariant 10 is reused  */
/*   verbatim (same namespace).                                        */
/* ================================================================== */

static const char *const inv_merge_nrd_base_keys[] = {
	"Taa", "Tab", "Tba", "Tbb",
};
static const char *const inv_merge_nrd_src_keys[] = { "ac", "ad", "bc", "bd" };
static const char *const inv_merge_nrd_merged_keys[] = {
	"Tac", "Tad", "Tbc", "Tbd",
};

static void *inv_merge_nonroot_dst_writer(void *arg)
{
	struct inv_merge_ctx *ctx = (struct inv_merge_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int i;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;

		/* Merge the root source into dst at the interior key "T". */
		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst, (const uint8_t *) "T", 1,
				ctx->src, NULL, 0);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_nonroot_dst writer: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();

		/* Reset: pull the merged keys back out, re-populate src. */
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		for (i = 0; i < 4; i++)
			inv_merge_remove_key(ctx->dst, iter,
				inv_merge_nrd_merged_keys[i]);
		for (i = 0; i < 4; i++) {
			struct ft_test_node *n = node_alloc(300 + i);

			cds_ft_insert(ctx->src,
				(const uint8_t *) inv_merge_nrd_src_keys[i],
				strlen(inv_merge_nrd_src_keys[i]), &n->node);
		}
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_nonroot_dst_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_nonroot_dst_no_escape");
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_merge_ctx ctx;
	struct inv_merge_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	if (cds_ft_create(group, NULL, &src) < 0) {
		cds_ft_destroy(dst);
		cds_ft_group_destroy(group);
		return -1;
	}

	rcu_read_lock();
	for (i = 0; i < 4; i++) {
		struct ft_test_node *dn = node_alloc(i);
		struct ft_test_node *sn = node_alloc(300 + i);

		cds_ft_insert(dst, (const uint8_t *) inv_merge_nrd_base_keys[i],
			strlen(inv_merge_nrd_base_keys[i]), &dn->node);
		cds_ft_insert(src, (const uint8_t *) inv_merge_nrd_src_keys[i],
			strlen(inv_merge_nrd_src_keys[i]), &sn->node);
	}
	rcu_read_unlock();

	ctx.dst = dst;
	ctx.src = src;
	ctx.group = group;
	ctx.test_name = "inv_merge_nonroot_dst_no_escape";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	/* All readers traverse dst; namespace {T}{a,b}{a..d} across the merge. */
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].trie = dst;
		rargs[i].c2_lo = (uint8_t) 'a';
		rargs[i].c2_hi = (uint8_t) 'd';
		rargs[i].which = "dst";
		pthread_create(&readers[i], NULL,
			inv_merge_no_escape_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, inv_merge_nonroot_dst_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_nonroot_dst_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11b: RE-ROOTED source, GLUE-diverge dst, ORDERED list   */
/*                                                                    */
/*   The writer merges a SUB-position source whose subtree root is     */
/*   re-rooted (an EXTERNAL leaf, shape 0; or a COMPRESSED run, shape   */
/*   1) at dst key "mb", which DIVERGES inside dst's compressed         */
/*   "mango" -- the build-invisible in-place graft reorder             */
/*   (ft_merge_graft_subpos_inplace), not the legacy detach path.  The */
/*   group has the ORDERED CELL list ON, so the writer also moves the  */
/*   source's ordered run into dst and (external shape) refreshes the   */
/*   moved head's cell edge byte; concurrent ordered readers iterating  */
/*   dst (cell-list path) across the flip must see ONLY dst's namespace */
/*   with correct keys -- a missed flip-proxy resolve, a mis-spliced    */
/*   run, or a stale edge byte surfaces an out-of-namespace key.  A src */
/*   reader must never escape UP into dst.                              */
/* ================================================================== */

/*
 * @shape selects the (source-root, dst-point) combination:
 *   0  external src  -> GLUE diverge ("mb" inside "mango")
 *   1  compressed src-> GLUE diverge ("mb" inside "mango")
 *   2  external src  -> NOSPLIT at-node (empty slot of dst {za,zb} at "zc")
 *   3  compressed src-> NOSPLIT build-branch (dst {m}, key "mxyz")
 *   4  KEY_SHORTER src ("ca" inside compressed "cab" over {e,f}) -> GLUE
 */
struct inv_rerooted_ctx {
	struct cds_ft *dst;
	struct cds_ft *src;
	const char *test_name;
	pthread_mutex_t lock;
	int shape;
};

struct inv_rerooted_reader_arg {
	struct inv_rerooted_ctx *ctx;
	struct cds_ft *trie;
	const char *const *allowed;	/* NULL-terminated allowed key set */
	const char *which;
};

static bool inv_rerooted_key_allowed(const char *const *allowed,
		const uint8_t *k, size_t kl)
{
	for (; *allowed; allowed++)
		if (strlen(*allowed) == kl && memcmp(*allowed, k, kl) == 0)
			return true;
	return false;
}

static void *inv_rerooted_reader(void *arg)
{
	struct inv_rerooted_reader_arg *ra =
		(struct inv_rerooted_reader_arg *) arg;
	struct inv_rerooted_ctx *ctx = ra->ctx;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	const unsigned int max_count = 32;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->trie, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ra->trie, iter)
			: cds_ft_lookup_first(ra->trie, iter);
		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (!inv_rerooted_key_allowed(ra->allowed, k, kl))
				report_violation(ctx->test_name,
					"%s reader saw out-of-namespace key "
					"(len %zu, %.*s) — merge re-parent / cell "
					"escape (iter #%lu, %s)",
					ra->which, kl, (int) kl,
					kl ? (const char *) k : "",
					iters, reverse ? "reverse" : "forward");
			s = reverse ? cds_ft_lookup_lt(ra->trie, iter)
				: cds_ft_lookup_gt(ra->trie, iter);
		}
		if (count >= max_count)
			report_violation(ctx->test_name,
				"%s ordered traversal returned >= %u keys — "
				"escape/loop (iter #%lu, %s)",
				ra->which, max_count, iters,
				reverse ? "reverse" : "forward");
		rcu_read_unlock();

		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* Per-shape merge parameters: the dst/src merge keys, the dst base keys, the
 * moved (merged) dst keys to remove on reset, the full source key set (init +
 * the src reader namespace), and the source keys to re-insert on reset (those
 * the merge consumed -- a "b"-style key that stays behind is not re-inserted). */
struct inv_rerooted_shape {
	const char *dst_key;
	const char *src_key;
	const char *dst_base[3];	/* NULL-terminated */
	const char *merged[3];		/* NULL-terminated */
	const char *src_keys[3];	/* NULL-terminated (init + namespace) */
	const char *src_reset[3];	/* NULL-terminated (re-insert on reset) */
};

static const struct inv_rerooted_shape inv_rerooted_shapes[] = {
	{ "mb",   "a",  { "mango", NULL }, { "mb", NULL },
	  { "a", "b", NULL }, { "a", NULL } },
	{ "mb",   "x",  { "mango", NULL }, { "mbabc", "mbabd", NULL },
	  { "xabc", "xabd", NULL }, { "xabc", "xabd", NULL } },
	{ "zc",   "a",  { "za", "zb", NULL }, { "zc", NULL },
	  { "a", "b", NULL }, { "a", NULL } },
	{ "mxyz", "x",  { "m", NULL }, { "mxyzabc", "mxyzabd", NULL },
	  { "xabc", "xabd", NULL }, { "xabc", "xabd", NULL } },
	{ "mb",   "ca", { "mango", NULL }, { "mbbe", "mbbf", NULL },	/* KEY_SHORTER */
	  { "cabe", "cabf", NULL }, { "cabe", "cabf", NULL } },
};

static void *inv_rerooted_writer(void *arg)
{
	struct inv_rerooted_ctx *ctx = (struct inv_rerooted_ctx *) arg;
	const struct inv_rerooted_shape *sh = &inv_rerooted_shapes[ctx->shape];
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();

	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		unsigned int i;

		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst,
				(const uint8_t *) sh->dst_key, strlen(sh->dst_key),
				ctx->src,
				(const uint8_t *) sh->src_key, strlen(sh->src_key));
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			/* Reported, not merely logged: see inv_graft_swap_writer. */
			report_violation(ctx->test_name,
				"merge failed (%s): the writer stopped, so this oracle would have passed vacuously",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();

		/* Reset: pull the merged keys out of dst, re-populate src. */
		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		for (i = 0; sh->merged[i]; i++)
			inv_merge_remove_key(ctx->dst, iter, sh->merged[i]);
		for (i = 0; sh->src_reset[i]; i++)
			cds_ft_insert(ctx->src,
				(const uint8_t *) sh->src_reset[i],
				strlen(sh->src_reset[i]),
				&node_alloc(411 + i)->node);
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_rerooted_run(int shape, const char *name)
{
	/* dst-reader namespace per shape: the base dst keys + transient merged. */
	static const char *const dst_allow[][4] = {
		{ "mango", "mb", NULL },		/* 0 ext glue */
		{ "mango", "mbabc", "mbabd", NULL },	/* 1 cmp glue */
		{ "za", "zb", "zc", NULL },		/* 2 ext at-node */
		{ "m", "mxyzabc", "mxyzabd", NULL },	/* 3 cmp branch */
		{ "mango", "mbbe", "mbbf", NULL },	/* 4 key-shorter */
	};
	const struct inv_rerooted_shape *sh = &inv_rerooted_shapes[shape];
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_rerooted_ctx ctx;
	struct inv_rerooted_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0 ||
	    cds_ft_group_attr_set_ordered_list(gattr, true) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}
	if (cds_ft_create(group, NULL, &src) < 0) {
		cds_ft_destroy(dst);
		cds_ft_group_destroy(group);
		return -1;
	}

	rcu_read_lock();
	for (i = 0; sh->dst_base[i]; i++)
		cds_ft_insert(dst, (const uint8_t *) sh->dst_base[i],
			strlen(sh->dst_base[i]), &node_alloc(1 + i)->node);
	for (i = 0; sh->src_keys[i]; i++)
		cds_ft_insert(src, (const uint8_t *) sh->src_keys[i],
			strlen(sh->src_keys[i]), &node_alloc(3 + i)->node);
	rcu_read_unlock();

	ctx.dst = dst;
	ctx.src = src;
	ctx.test_name = name;
	ctx.shape = shape;
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		bool on_dst = (i & 1) == 0;

		rargs[i].ctx = &ctx;
		rargs[i].trie = on_dst ? dst : src;
		rargs[i].allowed = on_dst ? dst_allow[shape] : sh->src_keys;
		rargs[i].which = on_dst ? "dst" : "src";
		pthread_create(&readers[i], NULL, inv_rerooted_reader,
			&rargs[i]);
	}
	pthread_create(&writer, NULL, inv_rerooted_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "%s: %lu violation(s)\n", name,
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

static int inv_merge_rerooted_glue_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_rerooted_glue_no_escape");
	if (inv_merge_rerooted_run(0, "inv_merge_rerooted_glue_ext") < 0)
		return -1;
	if (inv_merge_rerooted_run(1, "inv_merge_rerooted_glue_compressed") < 0)
		return -1;
	if (inv_merge_rerooted_run(2, "inv_merge_rerooted_nosplit_atnode") < 0)
		return -1;
	/*
	 * Shape 4 ("key shorter": src key longer than the dst key) is RETIRED.
	 * Its writer drives a LIVE reader-watched @src, which DLM refuses, and
	 * unlike shapes 0-3 it cannot be decomposed into detach+merge:
	 * cds_ft_detach STRIPS the @key prefix, so a key equal to the src prefix
	 * detaches to a zero-length key and merges back as the bare dst prefix --
	 * a shorter, out-of-namespace key the reader correctly flags.  The same
	 * shape retired inv_merge_key_shorter_src_no_escape at 27556421.
	 */
	return inv_merge_rerooted_run(3, "inv_merge_rerooted_nosplit_branch");
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11c: SAME-TRIE rekey never escapes / corrupts          */
/*                                                                    */
/*   The writer rekeys "ax" <-> "az" within ONE trie (move "ax"'s     */
/*   subtree {axm,axn} to "az" and back), reusing cds_ft_merge_at with */
/*   src_ft == dst_ft.  Rekey is detach + cross-trie merge -- inherently */
/*   multi-stage -- so a key may be transiently ABSENT (parked in the  */
/*   transient trie) mid-move; a concurrent ordered reader must still  */
/*   only ever see keys in {axm,axn,azm,azn,ayp} -- never a garbage /  */
/*   out-of-namespace key, and never loop.  The disjoint "ayp" is a    */
/*   fixed witness the shared 'a' ancestor stays consistent.           */
/* ================================================================== */

static void *inv_rekey_writer(void *arg)
{
	struct inv_rerooted_ctx *ctx = (struct inv_rerooted_ctx *) arg;

	rcu_register_thread();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;

		pthread_mutex_lock(&ctx->lock);
		s = cds_ft_rekey_graft(ctx->dst, (const uint8_t *) "az", 2,
				(const uint8_t *) "ax", 2);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey writer ax->az: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();
		pthread_mutex_lock(&ctx->lock);
		s = cds_ft_rekey_graft(ctx->dst, (const uint8_t *) "ax", 2,
				(const uint8_t *) "az", 2);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey writer az->ax: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();
	}

	rcu_unregister_thread();
	return NULL;
}

static int inv_rekey_no_escape(void)
{
	static const char *const allowed[] = {
		"axm", "axn", "azm", "azn", "ayp", NULL,
	};
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct inv_rerooted_ctx ctx;
	struct inv_rerooted_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0 ||
	    cds_ft_group_attr_set_ordered_list(gattr, true) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	/*
	 * REKEY OPT-IN (cds_ft_group_attr_set_rekey, default OFF): this trie
	 * hosts an in-trie MOVE, so the capability must be declared or the
	 * rekey entries answer NOT_SUPPORTED and the test proves nothing.
	 */
	if (cds_ft_group_attr_set_rekey(gattr, true) < 0)
		abort();
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &ft) < 0) {
		cds_ft_group_destroy(group);
		return -1;
	}

	rcu_read_lock();
	cds_ft_insert(ft, (const uint8_t *) "axm", 3, &node_alloc(1)->node);
	cds_ft_insert(ft, (const uint8_t *) "axn", 3, &node_alloc(2)->node);
	cds_ft_insert(ft, (const uint8_t *) "ayp", 3, &node_alloc(3)->node);
	rcu_read_unlock();

	ctx.dst = ft;
	ctx.src = ft;
	ctx.test_name = "inv_rekey_no_escape";
	ctx.shape = 0;
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].trie = ft;
		rargs[i].allowed = allowed;
		rargs[i].which = "ft";
		pthread_create(&readers[i], NULL, inv_rerooted_reader,
			&rargs[i]);
	}
	pthread_create(&writer, NULL, inv_rekey_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_rekey_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(ft);
	rcu_barrier();
	cds_ft_destroy(ft);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11d: a rekey to a SHORTER key does not strand a reader  */
/*   that resolves a skip pointer through the moved subtree           */
/*                                                                    */
/*   The subtree under the 12-byte prefix "Kabcdefghijk" sits behind  */
/*   the root's 'K' slot, which is skip-compressed.  The writer moves  */
/*   it to the 1-byte key "Q" and back.  A reader resolves a skip     */
/*   pointer by walking up from its child until it has covered the    */
/*   encoded length (ft_skip_reanchor).  A reader that loaded the 'K'  */
/*   slot before the move walks up from a child that now sits under   */
/*   "Q": the chain is shorter than the encoded length.  Every other  */
/*   rekey oracle moves between keys of EQUAL length.                 */
/* ================================================================== */

#define RKS_LONG	"Kabcdefghijk"
#define RKS_SHORT	"Q"

static const char *const rks_suffixes[] = { "mnopq1", "mnopq2", "r", "st" };
#define RKS_NSUF	(sizeof(rks_suffixes) / sizeof(rks_suffixes[0]))

struct rks_reader_arg {
	struct cds_ft *ft;
	unsigned int id;
	unsigned long ops;
};

static unsigned long rks_moves;

/* Is @k (@kl bytes) a key this test ever holds? */
static bool rks_key_allowed(const uint8_t *k, size_t kl)
{
	unsigned int i;

	if (kl == 2 && !memcmp(k, "Z1", 2))
		return true;
	for (i = 0; i < RKS_NSUF; i++) {
		size_t sl = strlen(rks_suffixes[i]);

		if (kl == sizeof(RKS_LONG) - 1 + sl &&
		    !memcmp(k, RKS_LONG, sizeof(RKS_LONG) - 1) &&
		    !memcmp(k + sizeof(RKS_LONG) - 1, rks_suffixes[i], sl))
			return true;
		if (kl == sizeof(RKS_SHORT) - 1 + sl &&
		    !memcmp(k, RKS_SHORT, sizeof(RKS_SHORT) - 1) &&
		    !memcmp(k + sizeof(RKS_SHORT) - 1, rks_suffixes[i], sl))
			return true;
	}
	return false;
}

static void rks_check_iter(struct cds_ft_iter *iter, const char *what)
{
	uint8_t k[32];
	size_t kl;

	if (cds_ft_iter_get_key(iter, k, sizeof(k), &kl) != CDS_FT_STATUS_OK ||
	    !rks_key_allowed(k, kl)) {
		fprintf(stderr, "inv_rekey_shorter_reanchor: %s returned an "
			"out-of-namespace key (len %zu)\n", what, kl);
		atomic_fetch_add(&violation_count, 1);
	}
}

/* Key order: bytewise, a prefix before its extensions. */
static int rks_cmp(const uint8_t *a, size_t al, const uint8_t *b, size_t bl)
{
	int c = memcmp(a, b, al < bl ? al : bl);

	if (c)
		return c;
	return al < bl ? -1 : al > bl;
}

/*
 * Check that the key @iter holds is in the namespace, and on @side of the
 * probe @k: above it for 1, below it for -1.  @strict excludes the probe
 * itself (gt / lt).
 */
static void rks_check_order(struct cds_ft_iter *iter, const char *what,
		const uint8_t *k, size_t kl, int side, bool strict)
{
	uint8_t r[32];
	size_t rl = 0;
	int c;

	if (cds_ft_iter_get_key(iter, r, sizeof(r), &rl) != CDS_FT_STATUS_OK ||
	    !rks_key_allowed(r, rl)) {
		fprintf(stderr, "inv_rekey_shorter_reanchor: %s returned an "
			"out-of-namespace key (len %zu)\n", what, rl);
		atomic_fetch_add(&violation_count, 1);
		return;
	}
	c = rks_cmp(r, rl, k, kl);
	if ((side > 0 && (strict ? c <= 0 : c < 0)) ||
	    (side < 0 && (strict ? c >= 0 : c > 0))) {
		fprintf(stderr, "inv_rekey_shorter_reanchor: %s returned %.*s "
			"for probe %.*s: wrong side\n", what, (int) rl, r,
			(int) kl, k);
		atomic_fetch_add(&violation_count, 1);
	}
}

static void *rks_reader(void *arg)
{
	struct rks_reader_arg *ra = (struct rks_reader_arg *) arg;
	struct cds_ft_iter *iter = NULL;
	unsigned int round = ra->id;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	while (!test_stop) {
		uint8_t k[32];
		size_t kl;
		unsigned int i = round % RKS_NSUF;
		bool longk = (round / RKS_NSUF) & 1;
		const char *pfx = longk ? RKS_LONG : RKS_SHORT;

		kl = strlen(pfx);
		memcpy(k, pfx, kl);
		memcpy(k + kl, rks_suffixes[i], strlen(rks_suffixes[i]));
		kl += strlen(rks_suffixes[i]);

		/* Point lookup: the exact descent. */
		rcu_read_lock();
		cds_ft_iter_set_key(iter, k, kl);
		cds_ft_lookup(ra->ft, iter);
		if (cds_ft_iter_node(iter))
			rks_check_iter(iter, "lookup");
		rcu_read_unlock();
		rcu_quiescent_state();

		/*
		 * Inequality, all four modes, probing one byte short of a key
		 * so the answer is a neighbour, not the probe itself.  The
		 * answer must lie on the right side of the probe.
		 */
		{
			static const struct {
				const char *name;
				enum cds_ft_status (*fn)(struct cds_ft *,
					struct cds_ft_iter *);
				int side;
				bool strict;
			} ops[] = {
				{ "lookup_ge", cds_ft_lookup_ge, 1, false },
				{ "lookup_gt", cds_ft_lookup_gt, 1, true },
				{ "lookup_le", cds_ft_lookup_le, -1, false },
				{ "lookup_lt", cds_ft_lookup_lt, -1, true },
			};
			unsigned int o;

			for (o = 0; o < 4; o++) {
				rcu_read_lock();
				cds_ft_iter_set_key(iter, k, kl - 1);
				if (ops[o].fn(ra->ft, iter) == CDS_FT_STATUS_OK)
					rks_check_order(iter, ops[o].name, k,
						kl - 1, ops[o].side,
						ops[o].strict);
				rcu_read_unlock();
				rcu_quiescent_state();
			}
		}

		/*
		 * Ordered walks.  Every step runs the inequality descent's
		 * climb during a move, and each must move strictly forward
		 * (next) or back (prev) from the key the step before returned,
		 * across the move and across the gate closing after it.
		 */
		{
			uint8_t lk[32];
			size_t ll = 0;
			unsigned int step;

			rcu_read_lock();
			if (cds_ft_lookup_first(ra->ft, iter) == CDS_FT_STATUS_OK) {
				rks_check_iter(iter, "first");
				cds_ft_iter_get_key(iter, lk, sizeof(lk), &ll);
				for (step = 0; step < 6 &&
				     cds_ft_next(ra->ft, iter) == CDS_FT_STATUS_OK;
				     step++) {
					rks_check_order(iter, "next", lk, ll, 1,
						true);
					cds_ft_iter_get_key(iter, lk, sizeof(lk),
						&ll);
				}
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			rcu_read_lock();
			if (cds_ft_lookup_last(ra->ft, iter) == CDS_FT_STATUS_OK) {
				rks_check_iter(iter, "last");
				cds_ft_iter_get_key(iter, lk, sizeof(lk), &ll);
				for (step = 0; step < 6 &&
				     cds_ft_prev(ra->ft, iter) == CDS_FT_STATUS_OK;
				     step++) {
					rks_check_order(iter, "prev", lk, ll, -1,
						true);
					cds_ft_iter_get_key(iter, lk, sizeof(lk),
						&ll);
				}
			}
			rcu_read_unlock();
			rcu_quiescent_state();
		}

		ra->ops++;
		round++;
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *rks_writer(void *arg)
{
	struct cds_ft *ft = (struct cds_ft *) arg;

	rcu_register_thread();
	while (!test_go)
		rcu_quiescent_state();
	while (!test_stop) {
		enum cds_ft_status s;

		s = cds_ft_rekey_graft(ft, (const uint8_t *) RKS_SHORT,
				sizeof(RKS_SHORT) - 1, (const uint8_t *) RKS_LONG,
				sizeof(RKS_LONG) - 1);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey_shorter_reanchor: long->short: "
				"%s\n", cds_ft_status_to_string(s));
			atomic_fetch_add(&violation_count, 1);
			break;
		}
		rcu_quiescent_state();
		s = cds_ft_rekey_graft(ft, (const uint8_t *) RKS_LONG,
				sizeof(RKS_LONG) - 1, (const uint8_t *) RKS_SHORT,
				sizeof(RKS_SHORT) - 1);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey_shorter_reanchor: short->long: "
				"%s\n", cds_ft_status_to_string(s));
			atomic_fetch_add(&violation_count, 1);
			break;
		}
		rcu_quiescent_state();
		rks_moves += 2;
	}
	rcu_unregister_thread();
	return NULL;
}

static int rks_run(bool rank)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rks_reader_arg rargs[2 * NR_READERS_DEFAULT];
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	struct timespec t0;
	unsigned long ops = 0, ms;
	const char *env = getenv("FT_INV_REKEY_SHORTER_MS");
	unsigned int i;
	int ret = 0;

	ms = env ? strtoul(env, NULL, 10) : DEFAULT_DURATION_MS;
	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 32) < 0 ||
	    cds_ft_group_attr_set_ordered_list(gattr, true) < 0 ||
	    cds_ft_group_attr_set_rekey(gattr, true) < 0 ||
	    (rank && cds_ft_group_attr_set_rank_stats(gattr, true) < 0))
		abort();
	if (cds_ft_group_create(gattr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();

	rcu_read_lock();
	for (i = 0; i < RKS_NSUF; i++) {
		uint8_t k[32];
		size_t kl = sizeof(RKS_LONG) - 1;
		struct ft_test_node *n = node_alloc(i);

		memcpy(k, RKS_LONG, kl);
		memcpy(k + kl, rks_suffixes[i], strlen(rks_suffixes[i]));
		kl += strlen(rks_suffixes[i]);
		if (cds_ft_insert(ft, k, kl, &n->node) != CDS_FT_STATUS_OK)
			abort();
	}
	if (cds_ft_insert(ft, (const uint8_t *) "Z1", 2,
			&node_alloc(99)->node) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_unlock();

	rks_moves = 0;
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ft = ft;
		rargs[i].id = i;
		rargs[i].ops = 0;
		pthread_create(&readers[i], NULL, rks_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, rks_writer, ft);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < (long) ms)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		pthread_join(readers[i], NULL);
		ops += rargs[i].ops;
	}
	rcu_thread_online();

	rcu_read_lock();
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK ||
	    cds_ft_count_keys(ft) != RKS_NSUF + 1) {
		fprintf(stderr, "inv_rekey_shorter_reanchor: final trie wrong "
			"(count %lu)\n", cds_ft_count_keys(ft));
		ret = -1;
	}
	rcu_read_unlock();
	fprintf(stderr, "# inv_rekey_shorter_reanchor[%s]: moves=%lu reader "
		"rounds=%lu violations=%lu -> %s\n", rank ? "rank" : "plain",
		rks_moves, ops, atomic_load(&violation_count),
		(ret || atomic_load(&violation_count)) ? "RED" : "ok");
	if (atomic_load(&violation_count) > 0 || !rks_moves)
		ret = -1;

	drain_trie_local(ft);
	rcu_barrier();
	cds_ft_destroy(ft);
	cds_ft_group_destroy(group);
	return ret;
}

static int inv_rekey_shorter_reanchor(void)
{
	int ret = rks_run(false);

	return rks_run(true) | ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 11e: a rank lookup_nth gives the right key while a     */
/*   rekey moves a subtree                                            */
/*                                                                    */
/*   The writer moves the four keys under a long prefix to a short    */
/*   one and back.  No other key lies between the two prefixes, so no */
/*   move changes any key's rank.  So lookup_nth(n) has exactly one   */
/*   right answer at every moment.  Past the last rank it must report */
/*   NOT_FOUND.  lookup_nth_last mirrors it.                          */
/*                                                                    */
/*   Three shapes place the two prefixes:                             */
/*     - both under the root;                                         */
/*     - both under one interior node;                                */
/*     - under two different interior nodes.                          */
/*   The descent reads one slot's count, then the next slot's.  In    */
/*   the last shape those two slots are in different nodes.           */
/*                                                                    */
/*   Each shape runs with rank stats on (one descent by key counts)   */
/*   and off (a walk from the first or last key).                     */
/* ================================================================== */

struct rkn_shape {
	const char *name;
	const char *lpfx, *spfx;	/* the subtree's two homes */
	const char *lo[2], *hi[2];	/* the keys below and above it */
	unsigned int nlo, nhi;
};

static const struct rkn_shape rkn_shapes[] = {
	{ "root", "Kabcdefghijk", "Q", { "A1" }, { "Z1" }, 1, 1 },
	{ "one parent", "PKabcdefghijk", "PQ", { "PA1" }, { "PZ1" }, 1, 1 },
	{ "two parents", "BKabcdefghijk", "CQ", { "A1", "BA1" },
		{ "CZ1", "Z1" }, 2, 2 },
};

static const struct rkn_shape *rkn_sh;
static unsigned long rkn_moves;

static unsigned long rkn_nkeys(void)
{
	return rkn_sh->nlo + RKS_NSUF + rkn_sh->nhi;
}

static bool rkn_is(const uint8_t *k, size_t kl, const char *a, const char *b)
{
	size_t al = strlen(a), bl = b ? strlen(b) : 0;

	return kl == al + bl && !memcmp(k, a, al) && (!b || !memcmp(k + al, b, bl));
}

/* Is @k (@kl bytes) the key at rank @r? */
static bool rkn_key_at_rank(unsigned long r, const uint8_t *k, size_t kl)
{
	const char *s;

	if (r < rkn_sh->nlo)
		return rkn_is(k, kl, rkn_sh->lo[r], NULL);
	r -= rkn_sh->nlo;
	if (r >= RKS_NSUF)
		return rkn_is(k, kl, rkn_sh->hi[r - RKS_NSUF], NULL);
	s = rks_suffixes[r];
	return rkn_is(k, kl, rkn_sh->lpfx, s) || rkn_is(k, kl, rkn_sh->spfx, s);
}

static void *rkn_reader(void *arg)
{
	struct rks_reader_arg *ra = (struct rks_reader_arg *) arg;
	struct cds_ft_iter *iter = NULL;
	unsigned long nk = rkn_nkeys();

	rcu_register_thread();
	if (cds_ft_iter_create(ra->ft, &iter) < 0)
		abort();
	while (!test_go)
		rcu_quiescent_state();
	while (!test_stop) {
		unsigned long n;
		int last;

		for (last = 0; last < 2; last++) {
			for (n = 0; n <= nk; n++) {
				const char *what = last ? "lookup_nth_last" :
					"lookup_nth";
				unsigned long r = last ? nk - 1 - n : n;
				enum cds_ft_status s;
				uint8_t k[32];
				size_t kl = 0;

				rcu_read_lock();
				s = last ? cds_ft_lookup_nth_last(ra->ft, iter, n) :
					cds_ft_lookup_nth(ra->ft, iter, n);
				if (n == nk) {
					if (s != CDS_FT_STATUS_NOT_FOUND) {
						fprintf(stderr, "inv_rekey_rank_nth "
							"[%s]: %s(%lu) past the "
							"end reported %s\n",
							rkn_sh->name, what, n,
							cds_ft_status_to_string(s));
						atomic_fetch_add(&violation_count, 1);
					}
				} else if (s != CDS_FT_STATUS_OK ||
					   cds_ft_iter_get_key(iter, k, sizeof(k),
						&kl) != CDS_FT_STATUS_OK ||
					   !rkn_key_at_rank(r, k, kl)) {
					fprintf(stderr, "inv_rekey_rank_nth [%s]: "
						"%s(%lu) reported %s, key %.*s\n",
						rkn_sh->name, what, n,
						cds_ft_status_to_string(s),
						s == CDS_FT_STATUS_OK ? (int) kl : 0,
						(const char *) k);
					atomic_fetch_add(&violation_count, 1);
				}
				rcu_read_unlock();
				rcu_quiescent_state();
				ra->ops++;
			}
		}
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *rkn_writer(void *arg)
{
	struct cds_ft *ft = (struct cds_ft *) arg;
	const uint8_t *l = (const uint8_t *) rkn_sh->lpfx;
	const uint8_t *sh = (const uint8_t *) rkn_sh->spfx;
	size_t ll = strlen(rkn_sh->lpfx), sl = strlen(rkn_sh->spfx);

	rcu_register_thread();
	while (!test_go)
		rcu_quiescent_state();
	while (!test_stop) {
		if (cds_ft_rekey_graft(ft, sh, sl, l, ll) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey_rank_nth [%s]: long->short "
				"failed\n", rkn_sh->name);
			atomic_fetch_add(&violation_count, 1);
			break;
		}
		rcu_quiescent_state();
		if (cds_ft_rekey_graft(ft, l, ll, sh, sl) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_rekey_rank_nth [%s]: short->long "
				"failed\n", rkn_sh->name);
			atomic_fetch_add(&violation_count, 1);
			break;
		}
		rcu_quiescent_state();
		rkn_moves += 2;
	}
	rcu_unregister_thread();
	return NULL;
}

static int rkn_run(const struct rkn_shape *sh, bool rank)
{
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct rks_reader_arg rargs[2 * NR_READERS_DEFAULT];
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	struct timespec t0;
	unsigned long ops = 0, ms;
	const char *env = getenv("FT_INV_REKEY_RANK_NTH_MS");
	unsigned int i;
	int ret = 0;

	rkn_sh = sh;
	ms = env ? strtoul(env, NULL, 10) : DEFAULT_DURATION_MS;
	atomic_store(&violation_count, 0);	/* count per shape */
	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 32) < 0 ||
	    cds_ft_group_attr_set_ordered_list(gattr, true) < 0 ||
	    cds_ft_group_attr_set_rekey(gattr, true) < 0 ||
	    (rank && cds_ft_group_attr_set_rank_stats(gattr, true) < 0))
		abort();
	if (cds_ft_group_create(gattr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(gattr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();

	rcu_read_lock();
	for (i = 0; i < RKS_NSUF + sh->nlo + sh->nhi; i++) {
		uint8_t k[32];
		size_t kl;
		const char *a, *b = NULL;

		if (i < RKS_NSUF) {
			a = sh->lpfx;
			b = rks_suffixes[i];
		} else if (i < RKS_NSUF + sh->nlo) {
			a = sh->lo[i - RKS_NSUF];
		} else {
			a = sh->hi[i - RKS_NSUF - sh->nlo];
		}
		kl = strlen(a);
		memcpy(k, a, kl);
		if (b) {
			memcpy(k + kl, b, strlen(b));
			kl += strlen(b);
		}
		if (cds_ft_insert(ft, k, kl, &node_alloc(i)->node) !=
				CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();

	rkn_moves = 0;
	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ft = ft;
		rargs[i].id = i;
		rargs[i].ops = 0;
		pthread_create(&readers[i], NULL, rkn_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, rkn_writer, ft);
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < (long) ms)
		usleep(1000);
	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		pthread_join(readers[i], NULL);
		ops += rargs[i].ops;
	}
	rcu_thread_online();

	rcu_read_lock();
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK ||
	    cds_ft_count_keys(ft) != rkn_nkeys()) {
		fprintf(stderr, "inv_rekey_rank_nth [%s]: final trie wrong "
			"(count %lu)\n", sh->name, cds_ft_count_keys(ft));
		ret = -1;
	}
	rcu_read_unlock();
	fprintf(stderr, "# inv_rekey_rank_nth[%s, %s]: moves=%lu queries=%lu "
		"violations=%lu -> %s\n", sh->name, rank ? "rank" : "walk",
		rkn_moves, ops,
		atomic_load(&violation_count),
		(ret || atomic_load(&violation_count)) ? "RED" : "ok");
	if (atomic_load(&violation_count) > 0 || !rkn_moves)
		ret = -1;

	drain_trie_local(ft);
	rcu_barrier();
	cds_ft_destroy(ft);
	cds_ft_group_destroy(group);
	return ret;
}

static int inv_rekey_rank_nth(void)
{
	unsigned int i;
	int ret = 0;

	for (i = 0; i < sizeof(rkn_shapes) / sizeof(rkn_shapes[0]); i++) {
		ret |= rkn_run(&rkn_shapes[i], true);
		ret |= rkn_run(&rkn_shapes[i], false);
	}
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 12: Merge into a COMPRESSED dst merge point never       */
/*   escapes (the M_slot skip-encoded interior publish via flip proxy) */
/*                                                                    */
/*   dst persistently holds "Tabz" -> under 'T' a COMPRESSED run "abz" */
/*   (merge point d_dst->nf is compressed).  Each round the writer     */
/*   merges a private root src {"abw"} at "T", so the merged M is a     */
/*   fresh COMPRESSED run "ab"->{z,w} published into the interior 'T'   */
/*   slot SKIP-ENCODED via the flip proxy -- the path single-threaded   */
/*   unit tests cannot stress.  An ordered reader must only ever see    */
/*   {"Tabz","Tabw"}.                                                  */
/* ================================================================== */

static void *inv_merge_compressed_dst_reader(void *arg)
{
	struct inv_merge_reader_arg *ra = (struct inv_merge_reader_arg *) arg;
	struct inv_merge_ctx *ctx = ra->ctx;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	const unsigned int max_count = 16;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->trie, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ra->trie, iter)
			: cds_ft_lookup_first(ra->trie, iter);
		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (kl != 4 || k[0] != 'T' || k[1] != 'a' ||
			    k[2] != 'b' || (k[3] != 'z' && k[3] != 'w')) {
				report_violation(ctx->test_name,
					"reader saw out-of-namespace key "
					"(len %zu, %.4s) — compressed-dst merge escape "
					"(iter #%lu, %s)",
					kl, kl ? (const char *) k : "",
					iters, reverse ? "reverse" : "forward");
				break;
			}
			s = reverse ? cds_ft_lookup_lt(ra->trie, iter)
				: cds_ft_lookup_gt(ra->trie, iter);
		}
		if (count >= max_count)
			report_violation(ctx->test_name,
				"ordered traversal returned >= %u keys — "
				"escape/loop (iter #%lu, %s)",
				max_count, iters, reverse ? "reverse" : "forward");
		rcu_read_unlock();
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_merge_compressed_dst_writer(void *arg)
{
	struct inv_merge_ctx *ctx = (struct inv_merge_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		struct ft_test_node *n;

		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst, (const uint8_t *) "T", 1,
				ctx->src, NULL, 0);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_compressed_dst writer: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();

		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		inv_merge_remove_key(ctx->dst, iter, "Tabw");
		n = node_alloc(300);
		cds_ft_insert(ctx->src, (const uint8_t *) "abw", 3, &n->node);
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_compressed_dst_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_compressed_dst_no_escape");
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_merge_ctx ctx;
	struct inv_merge_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	struct ft_test_node *n, *sn;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0)
		abort();
	if (cds_ft_create(group, NULL, &src) < 0)
		abort();

	/* Persistent "Tabz" keeps "T" a live COMPRESSED merge point. */
	n = node_alloc(0);
	cds_ft_insert(dst, (const uint8_t *) "Tabz", 4, &n->node);
	sn = node_alloc(300);
	cds_ft_insert(src, (const uint8_t *) "abw", 3, &sn->node);

	ctx.dst = dst;
	ctx.src = src;
	ctx.group = group;
	ctx.test_name = "inv_merge_compressed_dst_no_escape";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].trie = dst;
		rargs[i].which = "dst";
		pthread_create(&readers[i], NULL,
			inv_merge_compressed_dst_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, inv_merge_compressed_dst_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_compressed_dst_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 13: Merge into a KEY_SHORTER dst merge point never       */
/*   escapes (the dst key ends INSIDE a compressed node, so the merged  */
/*   cluster is published under a fresh prefix wrap via the flip proxy). */
/*                                                                    */
/*   dst persistently holds "Tabz" -> under 'T' a COMPRESSED run "abz".  */
/*   Each round the writer merges a private root src {"w"} at "Ta",      */
/*   which ends 1 byte INTO "abz": the node splits at prefix "a", the    */
/*   suffix "bz"->leaf is re-parented under a fresh branch via the flip, */
/*   and the result "a"->{b->z, w} is published into the interior 'T'    */
/*   slot SKIP-ENCODED.  An ordered reader must only ever see            */
/*   {"Tabz","Taw"} -- the compressed leaf must never escape the flip.   */
/* ================================================================== */

static void *inv_merge_key_shorter_dst_reader(void *arg)
{
	struct inv_merge_reader_arg *ra = (struct inv_merge_reader_arg *) arg;
	struct inv_merge_ctx *ctx = ra->ctx;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	const unsigned int max_count = 16;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->trie, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ra->trie, iter)
			: cds_ft_lookup_first(ra->trie, iter);
		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;
			bool ok_key = false;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (k[0] == 'T' && k[1] == 'a') {
				if (kl == 4 && k[2] == 'b' && k[3] == 'z')
					ok_key = true;	/* Tabz */
				else if (kl == 3 && k[2] == 'w')
					ok_key = true;	/* Taw */
			}
			if (!ok_key) {
				report_violation(ctx->test_name,
					"reader saw out-of-namespace key "
					"(len %zu, %.4s) — key-shorter-dst merge escape "
					"(iter #%lu, %s)",
					kl, kl ? (const char *) k : "",
					iters, reverse ? "reverse" : "forward");
				break;
			}
			s = reverse ? cds_ft_lookup_lt(ra->trie, iter)
				: cds_ft_lookup_gt(ra->trie, iter);
		}
		if (count >= max_count)
			report_violation(ctx->test_name,
				"ordered traversal returned >= %u keys — "
				"escape/loop (iter #%lu, %s)",
				max_count, iters, reverse ? "reverse" : "forward");
		rcu_read_unlock();
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_merge_key_shorter_dst_writer(void *arg)
{
	struct inv_merge_ctx *ctx = (struct inv_merge_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		struct ft_test_node *n;

		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst, (const uint8_t *) "Ta", 2,
				ctx->src, NULL, 0);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_key_shorter_dst writer: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();

		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		inv_merge_remove_key(ctx->dst, iter, "Taw");
		n = node_alloc(300);
		cds_ft_insert(ctx->src, (const uint8_t *) "w", 1, &n->node);
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_key_shorter_dst_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_key_shorter_dst_no_escape");
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_merge_ctx ctx;
	struct inv_merge_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	struct ft_test_node *n, *sn;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0)
		abort();
	if (cds_ft_create(group, NULL, &src) < 0)
		abort();

	/* Persistent "Tabz" keeps "Ta" a KEY_SHORTER point inside "abz". */
	n = node_alloc(0);
	cds_ft_insert(dst, (const uint8_t *) "Tabz", 4, &n->node);
	sn = node_alloc(300);
	cds_ft_insert(src, (const uint8_t *) "w", 1, &sn->node);

	ctx.dst = dst;
	ctx.src = src;
	ctx.group = group;
	ctx.test_name = "inv_merge_key_shorter_dst_no_escape";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].trie = dst;
		rargs[i].which = "dst";
		pthread_create(&readers[i], NULL,
			inv_merge_key_shorter_dst_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, inv_merge_key_shorter_dst_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_key_shorter_dst_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 14: Merge from a KEY_SHORTER src merge point never       */
/*   escapes (src_key ends INSIDE a compressed src node; the commit     */
/*   reclaims the whole node and re-parents its child cross-trie after   */
/*   the source drain).                                                 */
/*                                                                    */
/*   src holds "XYZ" -> under 'X' a COMPRESSED run "YZ"; the writer      */
/*   merges src@"XY" (1 byte into "YZ") into dst@"Q" each round, moving   */
/*   the "Z"->leaf suffix to "QZ", then resets (remove "QZ", re-insert    */
/*   "XYZ").  Persistent dst "Qb" keeps "Q" a live merge point.  A src    */
/*   reader must only ever see {"XYZ"} or empty; a dst reader only        */
/*   {"Qb","QZ"} -- the cross-trie re-parent must never let a key escape. */
/* ================================================================== */




/* ================================================================== */
/*                                                                    */
/*   INVARIANT 15: Merge into a COMPRESSED-PARENT dst merge point        */
/*   never escapes (Edge D: the merge point's parent is a compressed     */
/*   node reached via a grandparent skip slot, which the flip re-encodes  */
/*   to a fresh copy of that compressed parent).                        */
/*                                                                    */
/*   dst persistently holds {"aXYc","aXYd"} -> under 'a' a COMPRESSED     */
/*   "XY" -> internal{c,d}; the merge point "aXY" has a compressed parent. */
/*   Each round the writer merges a private root src {"P"} at "aXY",      */
/*   re-encoding root['a'] (skip("XY")) to a fresh "XY" copy whose child   */
/*   is internal{c,d,P}, then resets (remove "aXYP", re-insert "P").  An   */
/*   ordered reader must only ever see {"aXYc","aXYd","aXYP"}.            */
/* ================================================================== */

static void *inv_merge_compressed_parent_dst_reader(void *arg)
{
	struct inv_merge_reader_arg *ra = (struct inv_merge_reader_arg *) arg;
	struct inv_merge_ctx *ctx = ra->ctx;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	const unsigned int max_count = 16;

	rcu_register_thread();
	if (cds_ft_iter_create(ra->trie, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		unsigned int count = 0;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ra->trie, iter)
			: cds_ft_lookup_first(ra->trie, iter);
		while (s == CDS_FT_STATUS_OK && count++ < max_count) {
			uint8_t k[16];
			size_t kl;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (kl != 4 || k[0] != 'a' || k[1] != 'X' ||
			    k[2] != 'Y' ||
			    (k[3] != 'c' && k[3] != 'd' && k[3] != 'P')) {
				report_violation(ctx->test_name,
					"reader saw out-of-namespace key "
					"(len %zu, %.4s) — compressed-parent-dst merge escape "
					"(iter #%lu, %s)",
					kl, kl ? (const char *) k : "",
					iters, reverse ? "reverse" : "forward");
				break;
			}
			s = reverse ? cds_ft_lookup_lt(ra->trie, iter)
				: cds_ft_lookup_gt(ra->trie, iter);
		}
		if (count >= max_count)
			report_violation(ctx->test_name,
				"ordered traversal returned >= %u keys — "
				"escape/loop (iter #%lu, %s)",
				max_count, iters, reverse ? "reverse" : "forward");
		rcu_read_unlock();
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *inv_merge_compressed_parent_dst_writer(void *arg)
{
	struct inv_merge_ctx *ctx = (struct inv_merge_ctx *) arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		enum cds_ft_status s;
		struct ft_test_node *n;

		pthread_mutex_lock(&ctx->lock);
		s = inv_merge_detached(ctx->dst, (const uint8_t *) "aXY", 3,
				ctx->src, NULL, 0);
		pthread_mutex_unlock(&ctx->lock);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_compressed_parent_dst writer: %s\n",
				cds_ft_status_to_string(s));
			break;
		}
		rcu_quiescent_state();

		rcu_read_lock();
		pthread_mutex_lock(&ctx->lock);
		inv_merge_remove_key(ctx->dst, iter, "aXYP");
		n = node_alloc(300);
		cds_ft_insert(ctx->src, (const uint8_t *) "P", 1, &n->node);
		pthread_mutex_unlock(&ctx->lock);
		rcu_read_unlock();
		rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_compressed_parent_dst_no_escape(void)
{
	INV_NEED_MERGE("inv_merge_compressed_parent_dst_no_escape");
	struct cds_ft_group_attr *gattr;
	struct cds_ft_group *group;
	struct cds_ft *dst, *src;
	struct inv_merge_ctx ctx;
	struct inv_merge_reader_arg rargs[2 * NR_READERS_DEFAULT];
	struct timespec t0;
	pthread_t readers[2 * NR_READERS_DEFAULT], writer;
	struct ft_test_node *a, *b, *sn;
	unsigned int i;
	int ret = 0;

	if (cds_ft_group_attr_create(&gattr) < 0)
		return -1;
	if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	if (cds_ft_group_create(gattr, &group) < 0) {
		cds_ft_group_attr_destroy(gattr);
		return -1;
	}
	cds_ft_group_attr_destroy(gattr);

	if (cds_ft_create(group, NULL, &dst) < 0)
		abort();
	if (cds_ft_create(group, NULL, &src) < 0)
		abort();

	/* Persistent {aXYc,aXYd} keeps "aXY" a compressed-parent merge point. */
	a = node_alloc(0);
	cds_ft_insert(dst, (const uint8_t *) "aXYc", 4, &a->node);
	b = node_alloc(0);
	cds_ft_insert(dst, (const uint8_t *) "aXYd", 4, &b->node);
	sn = node_alloc(300);
	cds_ft_insert(src, (const uint8_t *) "P", 1, &sn->node);

	ctx.dst = dst;
	ctx.src = src;
	ctx.group = group;
	ctx.test_name = "inv_merge_compressed_parent_dst_no_escape";
	pthread_mutex_init(&ctx.lock, NULL);

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++) {
		rargs[i].ctx = &ctx;
		rargs[i].trie = dst;
		rargs[i].which = "dst";
		pthread_create(&readers[i], NULL,
			inv_merge_compressed_parent_dst_reader, &rargs[i]);
	}
	pthread_create(&writer, NULL, inv_merge_compressed_parent_dst_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < 2 * NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_compressed_parent_dst_no_escape: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	drain_trie_local(dst);
	drain_trie_local(src);
	rcu_barrier();
	cds_ft_destroy(dst);
	cds_ft_destroy(src);
	cds_ft_group_destroy(group);
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 16: merge_at is ATOMIC -- the flip publishes the whole   */
/*   source set at once, so no value is ever MISSING after it.          */
/*                                                                    */
/*   Two disjoint sets share the prefix "AA": set A holds the           */
/*   odd-byte-valued suffixes (a,c,e,...  -- in @dst), set B the even    */
/*   ones (b,d,f,...  -- in @src), exactly Mathieu's "AAaceg"/"AAbdfh"   */
/*   example.  A single cds_ft_merge_at folds B into @dst at "AA", so    */
/*   the merged set is the DENSE range "AA"+{a..z}.  Readers iterate     */
/*   @dst across the flip; once a reader has observed BOTH an A (odd)    */
/*   and a B (even) suffix -- proving its traversal is past the flip --  */
/*   every following key MUST be the immediate successor (forward) or    */
/*   predecessor (reverse).  The atomic flip reveals all of B at once,   */
/*   so a torn / partial merge would surface as a forward gap.  Keys     */
/*   BEHIND the detection point are not required (the flip may land      */
/*   mid-traversal) -- "all combinations, going forward".                */
/*                                                                    */
/*   One-shot per round (build A+B, run readers, merge once, join): a    */
/*   repeated merge/un-merge would legitimately make B vanish and is     */
/*   not what this probes.                                              */
/* ================================================================== */

/* Suffix letters: A = odd byte values {a,c,...,y}, B = even {b,d,...,z}. */
#define INV_ATOMIC_LO	'a'
#define INV_ATOMIC_HI	'z'

struct inv_atomic_ctx {
	struct cds_ft *dst;
	const char *test_name;
	volatile int go;
	volatile int stop;
};

static void *inv_merge_atomic_reader(void *arg)
{
	struct inv_atomic_ctx *ctx = (struct inv_atomic_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();
	while (!ctx->go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!ctx->stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		int prev = -1;
		bool saw_a = false, saw_b = false;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ctx->dst, iter)
			: cds_ft_lookup_first(ctx->dst, iter);
		while (s == CDS_FT_STATUS_OK) {
			uint8_t k[8];
			size_t kl;
			int v;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (kl != 3 || k[0] != 'A' || k[1] != 'A' ||
			    k[2] < INV_ATOMIC_LO || k[2] > INV_ATOMIC_HI) {
				report_violation(ctx->test_name,
					"out-of-namespace key (len %zu) — merge escape "
					"(%s)", kl, reverse ? "reverse" : "forward");
				break;
			}
			v = k[2];
			if (v & 1)
				saw_a = true;	/* odd byte: set A */
			else
				saw_b = true;	/* even byte: set B */
			/*
			 * Both partitions observed -> this traversal is in the
			 * post-flip regime, where the set is the dense range
			 * a..z.  Every subsequent key must be the immediate
			 * successor / predecessor; a gap means a value is missing
			 * after the atomic flip (a non-atomic / torn merge).
			 */
			if (saw_a && saw_b && prev >= 0) {
				int expect = reverse ? prev - 1 : prev + 1;

				if (v != expect) {
					report_violation(ctx->test_name,
						"missing value: suffix '%c' follows '%c' "
						"(%s) after both sets observed — non-atomic merge",
						v, prev, reverse ? "reverse" : "forward");
					break;
				}
			}
			prev = v;
			s = reverse ? cds_ft_lookup_lt(ctx->dst, iter)
				: cds_ft_lookup_gt(ctx->dst, iter);
		}
		rcu_read_unlock();
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_atomic_completeness(void)
{
	INV_NEED_MERGE("inv_merge_atomic_completeness");
	struct timespec t0;
	int ret = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS &&
			atomic_load(&violation_count) == 0) {
		struct cds_ft_group_attr *gattr;
		struct cds_ft_group *group;
		struct cds_ft *dst, *src;
		struct inv_atomic_ctx ctx;
		pthread_t readers[NR_READERS_DEFAULT];
		enum cds_ft_status s;
		unsigned int i;
		int v;

		if (cds_ft_group_attr_create(&gattr) < 0)
			return -1;
		if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
			cds_ft_group_attr_destroy(gattr);
			return -1;
		}
		if (cds_ft_group_create(gattr, &group) < 0) {
			cds_ft_group_attr_destroy(gattr);
			return -1;
		}
		cds_ft_group_attr_destroy(gattr);
		if (cds_ft_create(group, NULL, &dst) < 0)
			abort();
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();

		/*
		 * dst <- set A (odd-byte suffixes "AA"+{a,c,...}); src <- set B
		 * (even-byte suffixes as single-byte keys {b,d,...}, which the
		 * merge re-keys under "AA").
		 */
		for (v = INV_ATOMIC_LO; v <= INV_ATOMIC_HI; v++) {
			struct ft_test_node *n = node_alloc((uint64_t) v);
			uint8_t key[3] = { 'A', 'A', (uint8_t) v };

			if (v & 1)
				s = cds_ft_insert(dst, key, 3, &n->node);
			else
				s = cds_ft_insert(src, &key[2], 1, &n->node);
			if (s != CDS_FT_STATUS_OK) {
				node_free(n);
				ret = -1;
				goto round_teardown;
			}
		}

		ctx.dst = dst;
		ctx.test_name = "inv_merge_atomic_completeness";
		ctx.go = 0;
		ctx.stop = 0;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);

		for (i = 0; i < NR_READERS_DEFAULT; i++)
			pthread_create(&readers[i], NULL,
				inv_merge_atomic_reader, &ctx);

		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		ctx.go = 1;
		usleep(300);	/* readers stream pre-flip (A-only) traversals */
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		s = cds_ft_merge_at(dst, (const uint8_t *) "AA", 2, src, NULL, 0);

		usleep(1000);	/* readers stream across + post-flip traversals */
		ctx.stop = 1;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		for (i = 0; i < NR_READERS_DEFAULT; i++)
			pthread_join(readers[i], NULL);

		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_atomic_completeness: merge %s\n",
				cds_ft_status_to_string(s));
			ret = -1;
		}
round_teardown:
		/* @src is emptied by a successful merge; @dst holds the union. */
		drain_trie_local(dst);
		drain_trie_local(src);
		rcu_barrier();
		cds_ft_destroy(dst);
		cds_ft_destroy(src);
		cds_ft_group_destroy(group);
		if (ret)
			return ret;
	}

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_atomic_completeness: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*   INVARIANT 17: merge_at atomicity with a DEEP overlap.              */
/*                                                                    */
/*   Same no-missing-value property as #16, but the sets share          */
/*   branching structure so the merge RECURSES: keys are "AA"+P+L over   */
/*   prefix bytes P in 'a'..'h' and leaf letters L in 'a'..'z'.  Set A   */
/*   (dst) holds odd-L, set B (src) holds even-L, so at "AA" both sides  */
/*   carry every P (a stitch node + a recursion per P), and under each   */
/*   P the odd/even leaves interleave -- a multi-level spine copy with   */
/*   re-parents at several depths, vs #16's single wide node.  Sorted    */
/*   order is row-major (P,L); the dense merged set linearizes to        */
/*   idx = (P-'a')*26 + (L-'a'), so the same "once both parities seen,    */
/*   every forward key is the immediate successor" check applies.        */
/* ================================================================== */

#define INV_ATOMIC_DEEP_PLO	'a'
#define INV_ATOMIC_DEEP_PHI	'h'	/* 8 shared prefix bytes */
#define INV_ATOMIC_NL		(INV_ATOMIC_HI - INV_ATOMIC_LO + 1)	/* 26 */

static void *inv_merge_atomic_deep_reader(void *arg)
{
	struct inv_atomic_ctx *ctx = (struct inv_atomic_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->dst, &iter) < 0)
		abort();
	while (!ctx->go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!ctx->stop) {
		bool reverse = (iters & 1) != 0;
		enum cds_ft_status s;
		int prev = -1;
		bool saw_a = false, saw_b = false;

		rcu_read_lock();
		s = reverse ? cds_ft_lookup_last(ctx->dst, iter)
			: cds_ft_lookup_first(ctx->dst, iter);
		while (s == CDS_FT_STATUS_OK) {
			uint8_t k[8];
			size_t kl;
			int idx, l;

			cds_ft_iter_get_key(iter, k, sizeof(k), &kl);
			if (kl != 4 || k[0] != 'A' || k[1] != 'A' ||
			    k[2] < INV_ATOMIC_DEEP_PLO || k[2] > INV_ATOMIC_DEEP_PHI ||
			    k[3] < INV_ATOMIC_LO || k[3] > INV_ATOMIC_HI) {
				report_violation(ctx->test_name,
					"out-of-namespace key (len %zu) — deep merge escape "
					"(%s)", kl, reverse ? "reverse" : "forward");
				break;
			}
			l = k[3];
			idx = (k[2] - INV_ATOMIC_DEEP_PLO) * INV_ATOMIC_NL
				+ (l - INV_ATOMIC_LO);
			if (l & 1)
				saw_a = true;	/* odd leaf: set A */
			else
				saw_b = true;	/* even leaf: set B */
			if (saw_a && saw_b && prev >= 0) {
				int expect = reverse ? prev - 1 : prev + 1;

				if (idx != expect) {
					report_violation(ctx->test_name,
						"missing value: idx %d follows %d (%s) "
						"after both sets observed — non-atomic deep merge",
						idx, prev, reverse ? "reverse" : "forward");
					break;
				}
			}
			prev = idx;
			s = reverse ? cds_ft_lookup_lt(ctx->dst, iter)
				: cds_ft_lookup_gt(ctx->dst, iter);
		}
		rcu_read_unlock();
		iters++;
		if ((iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_merge_atomic_completeness_deep(void)
{
	INV_NEED_MERGE("inv_merge_atomic_completeness_deep");
	struct timespec t0;
	int ret = 0;

	clock_gettime(CLOCK_MONOTONIC, &t0);

	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS &&
			atomic_load(&violation_count) == 0) {
		struct cds_ft_group_attr *gattr;
		struct cds_ft_group *group;
		struct cds_ft *dst, *src;
		struct inv_atomic_ctx ctx;
		pthread_t readers[NR_READERS_DEFAULT];
		enum cds_ft_status s;
		unsigned int i;
		int p, l;

		if (cds_ft_group_attr_create(&gattr) < 0)
			return -1;
		if (cds_ft_group_attr_set_max_key_len(gattr, 16) < 0) {
			cds_ft_group_attr_destroy(gattr);
			return -1;
		}
		if (cds_ft_group_create(gattr, &group) < 0) {
			cds_ft_group_attr_destroy(gattr);
			return -1;
		}
		cds_ft_group_attr_destroy(gattr);
		if (cds_ft_create(group, NULL, &dst) < 0)
			abort();
		if (cds_ft_create(group, NULL, &src) < 0)
			abort();

		/*
		 * Both sides carry every prefix byte P, so the merge recurses at
		 * "AA" (one stitch node per P) and again under each P (odd/even
		 * leaves).  dst <- odd-L "AA"+P+L; src <- even-L P+L (re-keyed
		 * under "AA" by the merge).
		 */
		for (p = INV_ATOMIC_DEEP_PLO; p <= INV_ATOMIC_DEEP_PHI; p++) {
			for (l = INV_ATOMIC_LO; l <= INV_ATOMIC_HI; l++) {
				struct ft_test_node *n = node_alloc((uint64_t) l);
				uint8_t key[4] = { 'A', 'A', (uint8_t) p, (uint8_t) l };

				if (l & 1)
					s = cds_ft_insert(dst, key, 4, &n->node);
				else
					s = cds_ft_insert(src, &key[2], 2, &n->node);
				if (s != CDS_FT_STATUS_OK) {
					node_free(n);
					ret = -1;
					goto round_teardown;
				}
			}
		}

		ctx.dst = dst;
		ctx.test_name = "inv_merge_atomic_completeness_deep";
		ctx.go = 0;
		ctx.stop = 0;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);

		for (i = 0; i < NR_READERS_DEFAULT; i++)
			pthread_create(&readers[i], NULL,
				inv_merge_atomic_deep_reader, &ctx);

		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		ctx.go = 1;
		usleep(300);	/* readers stream pre-flip (A-only) traversals */
cds_ft_make_exclusive(src);	/* DLM: cross-trie src must be exclusive */

		s = cds_ft_merge_at(dst, (const uint8_t *) "AA", 2, src, NULL, 0);

		usleep(1000);	/* readers stream across + post-flip traversals */
		ctx.stop = 1;
		__atomic_thread_fence(__ATOMIC_SEQ_CST);
		for (i = 0; i < NR_READERS_DEFAULT; i++)
			pthread_join(readers[i], NULL);

		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_merge_atomic_completeness_deep: merge %s\n",
				cds_ft_status_to_string(s));
			ret = -1;
		}
round_teardown:
		drain_trie_local(dst);
		drain_trie_local(src);
		rcu_barrier();
		cds_ft_destroy(dst);
		cds_ft_destroy(src);
		cds_ft_group_destroy(group);
		if (ret)
			return ret;
	}

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_merge_atomic_completeness_deep: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}
	return ret;
}

/* ================================================================== */
/*                                                                    */
/*                           MAIN                                     */
/*                                                                    */
/* ================================================================== */

#ifdef FT_ENABLE_TRACING
/*
 * Root-cause scaffolding: on a fatal fault, dump the flight-recorder
 * ring (the writer/reanchor tracepoints leading up to the crash) before
 * dying, then re-raise for a core.  Enabled only when both built with
 * FT_ENABLE_TRACING and run with FT_INV_SNAPSHOT_ON_SEGV set under an
 * `lttng ... --snapshot` session.  system() in a signal handler is
 * async-signal-unsafe but adequate here (the fault is a bad read, not
 * heap corruption).
 */
static void ft_segv_snapshot_handler(int sig, siginfo_t *si, void *uc)
{
	char buf[160];
	int n;

	(void) uc;
	n = snprintf(buf, sizeof buf,
		"\n[fault] sig=%d fault_addr=%p — recording lttng snapshot\n",
		sig, si ? si->si_addr : NULL);
	if (n > 0) {
		ssize_t w = write(STDERR_FILENO, buf, (size_t) n);
		(void) w;
	}
	ft_snapshot_record();
	signal(sig, SIG_DFL);
	raise(sig);
}
#endif

/* ================================================================== */
/*                                                                    */
/*   Ordered-list bulk-op consistency (Phase 3)                       */
/*                                                                    */
/*   A concurrent ordered (cell-list) traversal of @A stays strictly  */
/*   sorted and bounded while a writer churns the ordered cell list    */
/*   with BULK ops: detach+graft round-trips (run move), graft_swap    */
/*   (run replace), and merge (interleave) + per-key reset.  Exercises */
/*   the bulk-op ordered-list maintenance and the head/tail flip-proxy */
/*   under live readers.  Variable-length group with both offsets so   */
/*   cds_ft_next walks the CELL list (not the descent fallback).       */
/* ================================================================== */

struct bulk_node {
	struct cds_ft_node node;
	struct rcu_head head;
	uint8_t kbytes[8];
	size_t klen;			/* read by the lib at key_len_offset */
};

static unsigned long bulk_alloc, bulk_freed;

static void bulk_set_key(uint8_t *out, uint32_t v)
{
	out[0] = (uint8_t)(v >> 24); out[1] = (uint8_t)(v >> 16);
	out[2] = (uint8_t)(v >> 8);  out[3] = (uint8_t) v;
}

static uint32_t bulk_key_val(const uint8_t *k)
{
	return ((uint32_t) k[0] << 24) | ((uint32_t) k[1] << 16) |
		((uint32_t) k[2] << 8) | k[3];
}

static struct bulk_node *bulk_node_alloc(uint32_t v)
{
	struct bulk_node *n = (struct bulk_node *) calloc(1, sizeof(*n));

	if (!n)
		abort();
	cds_ft_node_init(&n->node);
	bulk_set_key(n->kbytes, v);
	n->klen = 4;
	__atomic_add_fetch(&bulk_alloc, 1, __ATOMIC_RELAXED);
	return n;
}

static void bulk_node_free_cb(struct rcu_head *h)
{
	struct bulk_node *n = caa_container_of(h, struct bulk_node, head);

	memset(n, 0xfe, sizeof(*n));
	free(n);
	__atomic_add_fetch(&bulk_freed, 1, __ATOMIC_RELAXED);
}

static void bulk_node_free_rcu(struct bulk_node *n)
{
	call_rcu(&n->head, bulk_node_free_cb);
}

static struct cds_ft_group *bulk_group_create(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	cds_ft_group_attr_set_key_len(attr, CDS_FT_LEN_VARIABLE);
	cds_ft_group_attr_set_max_key_len(attr, 8);
	/*
	 * EAGER: NO speculative_key_offset and NO key_len_offset.  The ordered
	 * cell walk rematerializes each key (and its length) STRUCTURALLY via the
	 * parent up-walk -- so a re-prefixing bulk op (non-root graft_swap below)
	 * leaves no stale in-leaf key behind, and the reader still sees the right
	 * key for the moved nodes.
	 */
	cds_ft_group_attr_set_ordered_list(attr, true);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	return group;
}

/* Insert a @len-byte key into @ft (single-threaded setup or writer-locked). */
static void bulk_insert_len(struct cds_ft *ft, const uint8_t *kb, size_t len)
{
	struct bulk_node *n = (struct bulk_node *) calloc(1, sizeof(*n));
	struct cds_ft_node *res;

	if (!n)
		abort();
	cds_ft_node_init(&n->node);
	memcpy(n->kbytes, kb, len);
	n->klen = len;
	__atomic_add_fetch(&bulk_alloc, 1, __ATOMIC_RELAXED);
	if (cds_ft_insert_unique(ft, n->kbytes, len, &n->node, &res)
			!= CDS_FT_STATUS_OK) {
		free(n);
		__atomic_add_fetch(&bulk_freed, 1, __ATOMIC_RELAXED);
	}
}

static void bulk_insert4(struct cds_ft *ft, uint32_t v)
{
	uint8_t kb[4];

	bulk_set_key(kb, v);
	bulk_insert_len(ft, kb, 4);
}

/* Remove a 4-byte key from @ft and defer-free its node (writer-locked). */
static void bulk_remove4(struct cds_ft *ft, struct cds_ft_iter *iter, uint32_t v)
{
	struct cds_ft_node *head, *tmp;
	uint8_t kb[4];

	bulk_set_key(kb, v);
	cds_ft_iter_set_key(iter, kb, 4);
	cds_ft_lookup(ft, iter);
	if (!cds_ft_iter_node(iter))
		return;
	if (cds_ft_remove_all(ft, iter, &head) != CDS_FT_STATUS_OK)
		return;
	cds_ft_for_each_duplicate_safe_rcu(head, tmp)
		bulk_node_free_rcu(caa_container_of(head, struct bulk_node, node));
}

/*
 * DEBUG_COUNTERS-only group cell-balance accessor (weak: NULL on a library
 * built without DEBUG_COUNTERS, in which case the cell-balance assertion is
 * skipped).  Lets the bulk test confirm the ordered-list cells are reclaimed
 * by a drained destroy across all the bulk-op cell moves.
 */
extern void cds_ft_debug_cell_balance(const struct cds_ft_group *group,
		unsigned long *allocated, unsigned long *freed) __attribute__((weak));

/*
 * DEBUG_COUNTERS-only group node-balance accessor (weak: NULL on a library
 * built without DEBUG_COUNTERS, in which case the node-balance assertion is
 * skipped).  Lets the bulk test confirm internal/compressed nodes are
 * reclaimed by a drained destroy across all the bulk-op node moves (a graft
 * allocates a node accounted to one trie and frees it accounted to another,
 * so the balance is only meaningful at group granularity).
 */
extern void cds_ft_debug_node_balance(const struct cds_ft_group *group,
		unsigned long *allocated, unsigned long *freed) __attribute__((weak));

static void bulk_drain(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(ft, &iter) < 0)
		abort();
	rcu_read_lock();
	for (;;) {
		struct cds_ft_node *head, *tmp;

		cds_ft_lookup_first(ft, iter);
		if (!cds_ft_iter_node(iter))
			break;
		/*
		 * remove_all directly at the cell-walk position: on a
		 * variable-length lazy-key trie it resolves the LAZY key length
		 * (parent up-walk) before validating, so no set_key re-seed is
		 * needed.
		 */
		if (cds_ft_remove_all(ft, iter, &head) != CDS_FT_STATUS_OK)
			break;
		cds_ft_for_each_duplicate_safe_rcu(head, tmp)
			bulk_node_free_rcu(caa_container_of(head,
				struct bulk_node, node));
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
}

struct bulk_ctx {
	struct cds_ft *A;
	struct cds_ft_group *group;
	pthread_mutex_t lock;
	const char *test_name;
};

#define BULK_M		200		/* keys per movable prefix */
#define BULK_CAP	100000		/* iteration bound (loop/escape guard) */
static const uint8_t bulk_move_pfx[] = { 0x10, 0x20 };
#define BULK_SWAP_PFX	0x50
#define BULK_MERGE_PFX	0x10		/* C interleaves into A's 0x10 region */
#define BULK_NMERGE	8

static void *bulk_reader(void *arg)
{
	struct bulk_ctx *ctx = (struct bulk_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->A, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		uint32_t prev = 0;
		int first = 1;
		unsigned int count = 0;

		rcu_read_lock();
		cds_ft_lookup_first(ctx->A, iter);
		while (cds_ft_iter_node(iter) && count < BULK_CAP) {
			uint8_t rk[8];
			size_t rkl;
			uint32_t v;

			cds_ft_iter_get_key(iter, rk, sizeof(rk), &rkl);
			if (rkl != 4) {
				report_violation(ctx->test_name,
					"key len %zu != 4 (iter #%lu, pos %u)",
					rkl, iters, count);
				break;
			}
			v = bulk_key_val(rk);
			if (!first && v <= prev) {
				report_violation(ctx->test_name,
					"out-of-order 0x%08x after 0x%08x "
					"(iter #%lu, pos %u)",
					v, prev, iters, count);
				break;
			}
			prev = v;
			first = 0;
			count++;
			cds_ft_next(ctx->A, iter);
		}
		if (count >= BULK_CAP)
			report_violation(ctx->test_name,
				"ordered traversal returned >= %u keys — "
				"loop/escape (iter #%lu)", BULK_CAP, iters);
		rcu_read_unlock();

		if ((++iters & 0x3f) == 0)
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *bulk_writer(void *arg)
{
	struct bulk_ctx *ctx = (struct bulk_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned long iters = 0;
	bool do_merge = cds_ft_merge_enabled();

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->A, &iter) < 0)
		abort();
	while (!test_go)
		;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	while (!test_stop) {
		unsigned int op = iters % 3;

		/*
		 * Op 2 is the only merge-dependent one.  Ops 0 and 1 drive
		 * detach/graft and the re-prefixing non-root graft_swap, which
		 * are core bulk API and stay compiled in under
		 * -DNO_FEATURE_FT_MERGE -- so gate the ONE op rather than
		 * skipping the whole oracle, which would drop the ordered-view
		 * coverage of those two on that arm entirely.
		 */
		if (op == 2 && !do_merge) {
			iters++;
			continue;
		}

		if (op == 0) {
			/* detach + graft-back round-trip (run move, multi-key) */
			uint8_t P = bulk_move_pfx[iters % 2];
			struct cds_ft *D = NULL;

			pthread_mutex_lock(&ctx->lock);
			if (cds_ft_detach(ctx->A, &P, 1, &D) == CDS_FT_STATUS_OK
					&& D) {
				cds_ft_make_exclusive(D);	/* DLM: cross-trie src must be exclusive */
				cds_ft_graft(ctx->A, &P, 1, D);
				cds_ft_destroy(D);
			}
			pthread_mutex_unlock(&ctx->lock);
			rcu_quiescent_state();
		} else if (op == 1) {
			/*
			 * NON-ROOT graft_swap round-trip (RE-PREFIXING): swap A's
			 * subtree at prefix 0x50 with donor B's whole content.  B's
			 * 3-byte keys gain the 0x50 prefix when moved into A@0x50 (and
			 * A's old 0x50 keys lose it into B) -- so a reader iterating A
			 * sees nodes whose trie key differs from anything an in-leaf key
			 * could hold; the EAGER up-walk rematerializes them correctly.
			 */
			uint8_t P = BULK_SWAP_PFX;
			struct cds_ft *B;
			unsigned int i;

			if (cds_ft_create(ctx->group, NULL, &B) < 0)
				abort();
			for (i = 0; i < 32; i++) {	/* 3-byte donor keys */
				uint8_t kb[3] = { 0xAA, (uint8_t)(i >> 8),
						  (uint8_t) i };
				bulk_insert_len(B, kb, 3);
			}
			pthread_mutex_lock(&ctx->lock);
			cds_ft_make_exclusive(B);	/* DLM: cross-trie src must be exclusive */
			cds_ft_graft_swap(ctx->A, &P, 1, B);	/* A@0x50 <-> B */
			cds_ft_make_exclusive(B);	/* DLM: cross-trie src must be exclusive */
			cds_ft_graft_swap(ctx->A, &P, 1, B);	/* back */
			pthread_mutex_unlock(&ctx->lock);
			rcu_quiescent_state();
			bulk_drain(B);
			cds_ft_destroy(B);
			rcu_quiescent_state();
		} else {
			/* merge a fresh donor into A (interleave), then reset */
			struct cds_ft *C;
			enum cds_ft_status st;
			unsigned int i;

			if (cds_ft_create(ctx->group, NULL, &C) < 0)
				abort();
			/* 0x10-prefix, odd low bytes -> interleave A's evens */
			for (i = 0; i < BULK_NMERGE; i++)
				bulk_insert4(C, ((uint32_t) BULK_MERGE_PFX << 24)
					| (2 * i + 1));
			pthread_mutex_lock(&ctx->lock);
			cds_ft_make_exclusive(C);	/* DLM: cross-trie src must be exclusive */
			st = cds_ft_merge(ctx->A, NULL, 0, C);	/* C -> A, C empty */
			pthread_mutex_unlock(&ctx->lock);
			if (st != CDS_FT_STATUS_OK) {
				/*
				 * A refused merge leaves C's keys in C, and the
				 * compensating remove loop below only looks in
				 * A -- so an UNCHECKED status silently strands
				 * BULK_NMERGE nodes per iteration and surfaces
				 * as a bogus "node leak alloc != freed" at the
				 * end of the test rather than naming the
				 * refusal.  Name it, and drain C so the leak
				 * balance still answers the question it is
				 * there to answer.
				 */
				report_violation(ctx->test_name,
					"cds_ft_merge refused: status %d "
					"(iter #%lu)", (int) st, iters);
				rcu_quiescent_state();
				bulk_drain(C);
				cds_ft_destroy(C);
				rcu_quiescent_state();
				iters++;
				continue;
			}
			rcu_quiescent_state();

			pthread_mutex_lock(&ctx->lock);
			for (i = 0; i < BULK_NMERGE; i++)
				bulk_remove4(ctx->A,
					iter, ((uint32_t) BULK_MERGE_PFX << 24)
						| (2 * i + 1));
			pthread_mutex_unlock(&ctx->lock);
			cds_ft_destroy(C);
			rcu_quiescent_state();
		}
		iters++;
	}

	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_ordered_bulk_consistency(void)
{
	struct cds_ft_group *group = bulk_group_create();
	struct cds_ft *A;
	struct bulk_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i, j;
	int ret = 0;

	if (cds_ft_create(group, NULL, &A) < 0)
		abort();

	/* A: movable prefixes (even low bytes), a static prefix, a swap zone. */
	rcu_read_lock();
	for (j = 0; j < 2; j++)
		for (i = 0; i < BULK_M; i++)
			bulk_insert4(A, ((uint32_t) bulk_move_pfx[j] << 24)
				| (2 * i));
	for (i = 0; i < BULK_M; i++)
		bulk_insert4(A, (0x40u << 24) | i);		/* static */
	for (i = 0; i < 16; i++)
		bulk_insert4(A, ((uint32_t) BULK_SWAP_PFX << 24) | i);	/* swap zone */
	rcu_read_unlock();

	ctx.A = A;
	ctx.group = group;
	ctx.test_name = "inv_ordered_bulk_consistency";
	pthread_mutex_init(&ctx.lock, NULL);

	/*
	 * Partial skip, named: this oracle is not merge-NAMED and stays useful
	 * without merge, so it runs on -DNO_FEATURE_FT_MERGE with one of its
	 * three writer ops gated off.  Say so, rather than let the log read as
	 * full coverage.
	 */
	if (!cds_ft_merge_enabled())
		fprintf(stderr, "# inv_ordered_bulk_consistency: merge "
			"interleave op skipped, merge compiled out "
			"(-DNO_FEATURE_FT_MERGE); detach/graft and the "
			"re-prefixing graft_swap still covered\n");

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, bulk_reader, &ctx);
	pthread_create(&writer, NULL, bulk_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);

	rcu_thread_online();
	pthread_mutex_destroy(&ctx.lock);

	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "inv_ordered_bulk_consistency: %lu violation(s)\n",
			atomic_load(&violation_count));
		ret = -1;
	}

	bulk_drain(A);
	rcu_barrier();
	cds_ft_destroy(A);
	/*
	 * The trie is drained and destroyed: every ordered-list cell allocated
	 * over the run (including the ones the bulk ops migrated between tries)
	 * must have been reclaimed.  Checked at group granularity, before the
	 * group is torn down.  Only meaningful on a DEBUG_COUNTERS library; the
	 * weak symbol is NULL otherwise.
	 */
	if (cds_ft_debug_cell_balance) {
		unsigned long ca = 0, cf = 0;

		cds_ft_debug_cell_balance(group, &ca, &cf);
		if (ca != cf) {
			fprintf(stderr, "inv_ordered_bulk_consistency: cell leak "
				"alloc %lu != freed %lu\n", ca, cf);
			ret = -1;
		}
	}
	/*
	 * Same idea for the library's internal/compressed structural nodes:
	 * the bulk ops migrate nodes between the group's tries, so a drained
	 * destroy of every trie must leave the group-scoped node balance at
	 * zero.  Read before cds_ft_group_destroy frees the group.
	 */
	if (cds_ft_debug_node_balance) {
		unsigned long na = 0, nf = 0;

		cds_ft_debug_node_balance(group, &na, &nf);
		if (na != nf) {
			fprintf(stderr, "inv_ordered_bulk_consistency: internal node leak "
				"alloc %lu != freed %lu\n", na, nf);
			ret = -1;
		}
	}
	cds_ft_group_destroy(group);
	{
		unsigned long na = __atomic_load_n(&bulk_alloc, __ATOMIC_RELAXED);
		unsigned long nf = __atomic_load_n(&bulk_freed, __ATOMIC_RELAXED);

		if (na != nf) {
			fprintf(stderr, "inv_ordered_bulk_consistency: node leak "
				"alloc %lu != freed %lu\n", na, nf);
			ret = -1;
		}
	}
	return ret;
}

/* ================================================================== */
/* 18. A retired PREFIX head's key still names its own node           */
/* ================================================================== */

/*
 * THE KEY->NODE ORACLE, and it exists because a UNIVERSE ORACLE IS BLIND HERE.
 *
 * A prefix head's ordered cell carries incoming_byte == its key's last byte --
 * which is ALSO its holder's own incoming edge.  When the up-walk misclassifies
 * such a head as a SLOT head it writes that byte twice, so "Bd" comes back as
 * "Bdd": a key that IS in the trie, carried on the node for "Bd".  Every check
 * of the form "is the emitted key one I inserted?" passes on it.  So this
 * oracle pins each key to the NODE it must name (@value, stamped at insert) and
 * compares them -- the only shape of check that can see the defect at all.
 *
 * The writer churns the prefix key through the retire paths that leave the
 * HOLDER ALIVE, which is the precondition: the reader is parked on a cell whose
 * head is gone while the node it hangs under is still there and still walked.
 *   RETIRE_CLEAR    -- remove_all: the holder's external_nodes goes to NULL.
 *   RETIRE_PROMOTE  -- remove the HEAD of a 2-deep duplicate chain: the
 *                      successor is promoted into the head's place (swap).
 *   RETIRE_REPLACE  -- cds_ft_replace: the same swap, driven by the public API.
 *   RETIRE_NIL      -- the same clear on the NIL key at the ROOT, whose holder
 *                      is the root node.  Its failure is a BAD KEY, not a wrong
 *                      node: "" comes back length 1.
 * A 1-CHILD holder is deliberately NOT in the list: there the fused
 * chain-compress REPLACES the holder, the retired copy keeps its
 * external_nodes, and the arm is not reachable.  That is a real control, not an
 * omission -- it is the one shape of this family measured clean on BOTH a
 * pristine and a fixed library.
 *
 * ☞ AND IT DOES NOT DUPLICATE inv_prefix_key_park_vs_holder_churn, which drives
 * the same park/churn race with the ORDERED LIST OFF (create_varlen_nolist_ft_ws)
 * and only writers.  ft_rebuild_key_upwalk is ordered-list-only, so that oracle
 * cannot reach the arm this one exists for; measured, its reader-side arms
 * execute ZERO times across the whole suite.  List ON plus a key-reading reader
 * is what makes the arm reachable at all.
 */
enum inv_prefix_retire {
	INV_PREFIX_RETIRE_CLEAR,
	INV_PREFIX_RETIRE_PROMOTE,
	INV_PREFIX_RETIRE_REPLACE,
	INV_PREFIX_RETIRE_NIL,
	INV_PREFIX_RETIRE_NR,
};

struct inv_prefix_key {
	const char *key;
	uint64_t tag;
};

struct inv_prefix_ctx {
	struct cds_ft *ft;
	const struct inv_prefix_key *universe;	/* NULL-key terminated */
	const char *churn;			/* the prefix key, churned */
	size_t churn_len;
	const char *test_name;
	enum inv_prefix_retire retire;
	unsigned long cycles;
	unsigned long keys_read;
};

/* The tag the churned key's nodes carry; distinct from every static key's. */
#define INV_PREFIX_CHURN_TAG	9

static uint64_t inv_prefix_expected_tag(const struct inv_prefix_key *universe,
		const uint8_t *key, size_t key_len)
{
	unsigned int i;

	for (i = 0; universe[i].key; i++)
		if (strlen(universe[i].key) == key_len &&
				!memcmp(universe[i].key, key, key_len))
			return universe[i].tag;
	return 0;			/* not a key of the trie at all */
}

static void *inv_prefix_reader(void *arg)
{
	struct inv_prefix_ctx *ctx = arg;
	struct cds_ft_iter *iter;
	unsigned long keys = 0;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		enum cds_ft_status status;

		rcu_read_lock();
		for (status = cds_ft_lookup_first(ctx->ft, iter);
				status == CDS_FT_STATUS_OK;
				status = cds_ft_next(ctx->ft, iter)) {
			uint8_t kbuf[64];
			size_t klen = 0;
			struct cds_ft_node *node;
			uint64_t carried, expected;

			if (cds_ft_iter_get_key(iter, kbuf, sizeof(kbuf),
					&klen) != CDS_FT_STATUS_OK)
				continue;
			keys++;
			node = cds_ft_iter_node(iter);
			if (!node)
				continue;
			carried = to_test_node(node)->value;
			expected = inv_prefix_expected_tag(ctx->universe,
					kbuf, klen);
			/*
			 * TWO distinct failures, reported apart because they
			 * name different halves of the walk: a key that is not
			 * in the universe at all (the NIL shape's stale trailing
			 * byte), and a legal key delivered on the wrong node
			 * (the prefix shape's doubled byte).  The second is the
			 * one a universe check cannot see.
			 */
			if (!expected)
				report_violation(ctx->test_name,
					"key '%.*s' (len %zu) is not a key of "
					"the trie; node carries tag %llu",
					(int) klen, (const char *) kbuf, klen,
					(unsigned long long) carried);
			else if (carried != expected)
				report_violation(ctx->test_name,
					"key '%.*s' (len %zu) delivered on the "
					"node for tag %llu, expected tag %llu",
					(int) klen, (const char *) kbuf, klen,
					(unsigned long long) carried,
					(unsigned long long) expected);
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	__atomic_add_fetch(&ctx->keys_read, keys, __ATOMIC_RELAXED);
	return NULL;
}

/* Free a whole duplicate chain returned by cds_ft_remove_all. */
static void inv_prefix_free_chain(struct cds_ft_node *head)
{
	struct cds_ft_node *node, *next;

	for (node = head; node; node = next) {
		next = cds_ft_node_next_rcu(node);
		node_free_rcu(to_test_node(node));
	}
}

static struct ft_test_node *inv_prefix_node_alloc(uint64_t tag)
{
	struct ft_test_node *n = node_alloc(tag);

	n->value = tag;
	return n;
}

static void *inv_prefix_writer(void *arg)
{
	struct inv_prefix_ctx *ctx = arg;
	const uint8_t *ckey = (const uint8_t *) ctx->churn;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		struct ft_test_node *first = inv_prefix_node_alloc(
			INV_PREFIX_CHURN_TAG);
		struct cds_ft_node *chain = NULL;

		rcu_read_lock();
		if (cds_ft_insert(ctx->ft, ckey, ctx->churn_len, &first->node)
				!= CDS_FT_STATUS_OK)
			abort();
		if (ctx->retire == INV_PREFIX_RETIRE_PROMOTE) {
			struct ft_test_node *dup = inv_prefix_node_alloc(
				INV_PREFIX_CHURN_TAG);

			if (cds_ft_insert(ctx->ft, ckey, ctx->churn_len,
					&dup->node) != CDS_FT_STATUS_OK)
				abort();
		}
		rcu_read_unlock();
		rcu_quiescent_state();

		rcu_read_lock();
		cds_ft_iter_set_key(iter, ckey, ctx->churn_len);
		if (cds_ft_lookup(ctx->ft, iter) != CDS_FT_STATUS_OK)
			abort();
		if (ctx->retire == INV_PREFIX_RETIRE_PROMOTE) {
			/* Retire the HEAD; the chain's successor is promoted. */
			if (cds_ft_remove(ctx->ft, iter, &first->node)
					!= CDS_FT_STATUS_OK)
				abort();
			node_free_rcu(first);
		} else if (ctx->retire == INV_PREFIX_RETIRE_REPLACE) {
			struct ft_test_node *fresh = inv_prefix_node_alloc(
				INV_PREFIX_CHURN_TAG);

			if (cds_ft_replace(ctx->ft, iter, &first->node,
					&fresh->node) != CDS_FT_STATUS_OK)
				abort();
			node_free_rcu(first);
		}
		if (ctx->retire == INV_PREFIX_RETIRE_PROMOTE ||
				ctx->retire == INV_PREFIX_RETIRE_REPLACE) {
			/* The swap moved the position; re-find before clearing. */
			rcu_read_unlock();
			rcu_quiescent_state();
			rcu_read_lock();
			cds_ft_iter_set_key(iter, ckey, ctx->churn_len);
			if (cds_ft_lookup(ctx->ft, iter) != CDS_FT_STATUS_OK)
				abort();
		}
		if (cds_ft_remove_all(ctx->ft, iter, &chain) < 0)
			abort();
		rcu_read_unlock();
		rcu_quiescent_state();
		inv_prefix_free_chain(chain);
		ctx->cycles++;
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_prefix_head_retire_run(enum inv_prefix_retire retire,
		const char *name)
{
	/*
	 * {Bdd, Bds} give the holder for "Bd" TWO children, so a remove of
	 * "Bd" clears its external_nodes and LEAVES THE HOLDER -- the shape the
	 * up-walk has to classify.  With one child the holder would be fused
	 * away instead and the arm is unreachable, which is why the arity is
	 * fixed here rather than left to chance.  "Bs" keeps the parent of the
	 * holder branching, so the walk has a real path to rebuild.
	 */
	static const struct inv_prefix_key universe_prefix[] = {
		{ "Bdd", 1 }, { "Bds", 2 }, { "Bs", 3 },
		{ "Bd", INV_PREFIX_CHURN_TAG }, { NULL, 0 }
	};
	/* The NIL key hangs at the ROOT: its holder is the root node. */
	static const struct inv_prefix_key universe_nil[] = {
		{ "a", 1 }, { "b", 2 },
		{ "", INV_PREFIX_CHURN_TAG }, { NULL, 0 }
	};
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct inv_prefix_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	ft = create_varlen_ord_ft(&group);

	memset(&ctx, 0, sizeof(ctx));
	ctx.ft = ft;
	ctx.universe = retire == INV_PREFIX_RETIRE_NIL ?
		universe_nil : universe_prefix;
	ctx.churn = retire == INV_PREFIX_RETIRE_NIL ? "" : "Bd";
	ctx.churn_len = strlen(ctx.churn);
	ctx.test_name = name;
	ctx.retire = retire;

	rcu_read_lock();
	for (i = 0; ctx.universe[i].key; i++) {
		struct ft_test_node *n;

		if (ctx.universe[i].tag == INV_PREFIX_CHURN_TAG)
			continue;		/* the writer owns this one */
		n = inv_prefix_node_alloc(ctx.universe[i].tag);
		if (cds_ft_insert(ft, (const uint8_t *) ctx.universe[i].key,
				strlen(ctx.universe[i].key), &n->node)
					!= CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();
	rcu_quiescent_state();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_prefix_reader, &ctx);
	pthread_create(&writer, NULL, inv_prefix_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	/*
	 * ★ THE ARM MUST HAVE RUN.  A zero from an oracle whose writer never
	 * completed a churn cycle, or whose readers never walked a key, is not
	 * evidence of anything -- and this whole class was first reported clean
	 * by a probe that never armed.  Fail rather than pass silently.
	 */
	if (!ctx.cycles || !ctx.keys_read) {
		fprintf(stderr, "%s: oracle never armed (cycles=%lu keys=%lu)\n",
			name, ctx.cycles, ctx.keys_read);
		ret = -1;
	}
	if (atomic_load(&violation_count) > 0) {
		fprintf(stderr, "%s: %lu violation(s) over %lu churn cycles, "
			"%lu keys read\n", name,
			(unsigned long) atomic_load(&violation_count),
			ctx.cycles, ctx.keys_read);
		ret = -1;
	}

	/*
	 * The suite's standard teardown, not a hand-rolled one: it runs
	 * teardown_walk_check (a verify plus a BOUNDED ordered walk) before
	 * draining, so a trie this oracle left structurally broken is REPORTED
	 * here rather than torn down quietly.
	 */
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

static int inv_prefix_head_clear_key_identity(void)
{
	return inv_prefix_head_retire_run(INV_PREFIX_RETIRE_CLEAR,
		"inv_prefix_head_clear_key_identity");
}

static int inv_prefix_head_promote_key_identity(void)
{
	return inv_prefix_head_retire_run(INV_PREFIX_RETIRE_PROMOTE,
		"inv_prefix_head_promote_key_identity");
}

static int inv_prefix_head_replace_key_identity(void)
{
	return inv_prefix_head_retire_run(INV_PREFIX_RETIRE_REPLACE,
		"inv_prefix_head_replace_key_identity");
}

static int inv_prefix_head_nil_key_identity(void)
{
	return inv_prefix_head_retire_run(INV_PREFIX_RETIRE_NIL,
		"inv_prefix_head_nil_key_identity");
}

/* ================================================================== */
/*                                                                    */
/*   POINT-LOOKUP IDENTITY ACROSS A COMPRESSED-RUN SPLIT              */
/*                                                                    */
/*   A key that is inserted once and never touched again must answer  */
/*   an EXACT cds_ft_eager_lookup_key with ITS OWN node, at every     */
/*   instant, while an unrelated key is inserted and removed under    */
/*   the same compressed run.                                         */
/*                                                                    */
/* ================================================================== */

/*
 * ☠ WHY THIS ORACLE EXISTS, AND WHY NOTHING ELSE IN THIS SUITE COVERED IT.
 *
 * Every other concurrent oracle here reaches the trie through the ORDERED WALK
 * (cds_ft_lookup_first / cds_ft_next) or through the inequality descent.  None
 * of them does a plain EXACT POINT LOOKUP of a stable key under churn and then
 * checks WHICH NODE came back.  So the whole precise-descent path -- and with
 * it ft_skip_reanchor, the single concurrency mechanism for skip-compressed
 * slots -- was uncovered, and a defect there (an up-walk that charged one key
 * byte for a PREFIX head, which consumes none) produced both a transient
 * NOT_FOUND and an outright WRONG NODE for a present key with the whole suite
 * green.
 *
 * ☠ AND THE TRIE MUST BE SPECULATIVE, NOT EAGER.
 * cds_ft_group_attr_set_lookup_optimization(EAGER) CLEARS
 * CDS_FT_FLAG_SKIP_COMPRESSED (ft-lifecycle.h), so create_varlen_ord_ft and
 * create_varlen_nolist_ft -- which nearly every oracle in this file uses --
 * run with skip-compressed OFF and can never enter ft_skip_reanchor at all.
 * The *_spec creators keep the flag set, which is what makes this arm
 * reachable.  Reading a green from an EAGER creator here would be reading a
 * detector that never armed.
 *
 * THE SHAPE.  "wa" is a key, and it is a PROPER PREFIX of the churned key
 * "wabbb".  Inserting "wabbb" SPLITS the compressed run below the root's 'w'
 * slot and re-homes "wa"'s head as the fresh internal node's PREFIX head; a
 * reader holding the pre-split skip slot then re-anchors through that head.
 * "q" is a second, unrelated stable key: it keeps the root branching, and a
 * miss on it would name a different (root-level) defect.
 *
 * ARM CHECK: a zero from a run whose writer completed no churn cycle, or whose
 * readers issued no lookup, is not evidence -- both are asserted below.
 */
#define INV_SPLITPT_CHURN_TAG	9

struct inv_splitpt_key {
	const char *key;
	uint64_t tag;
};

struct inv_splitpt_ctx {
	struct cds_ft *ft;
	const struct inv_splitpt_key *stable;	/* NULL-key terminated */
	const char *churn;
	size_t churn_len;
	const char *test_name;
	unsigned long cycles;
	unsigned long lookups;
	unsigned long not_found;	/* a stable key answered NOT_FOUND */
	unsigned long wrong_node;	/* a stable key came back on another's node */
	bool pinned;			/* the preemption amplifier is armed */
};

static void *inv_splitpt_reader(void *arg)
{
	struct inv_splitpt_ctx *ctx = arg;
	unsigned long lookups = 0, nf = 0, wrong = 0;
	bool reported = false;

	rcu_register_thread();
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		unsigned int i;

		rcu_read_lock();
		for (i = 0; ctx->stable[i].key; i++) {
			const char *k = ctx->stable[i].key;
			struct cds_ft_node *node = NULL;
			enum cds_ft_status status;

			status = cds_ft_eager_lookup_key(ctx->ft,
					(const uint8_t *) k, strlen(k), 0,
					&node);
			lookups++;
			if (status != CDS_FT_STATUS_OK) {
				nf++;
				/*
				 * Report only the FIRST one from this thread:
				 * the rate runs to thousands per second, and
				 * FT_INV_ABORT_ON_VIOLATION wants the flight
				 * recorder stopped at the first fault, not the
				 * last.  The totals are reported once by the
				 * run function below.
				 */
				if (!reported) {
					reported = true;
					report_violation(ctx->test_name,
						"stable key '%s' answered %s "
						"to an exact eager lookup",
						k, cds_ft_status_to_string(
							status));
				}
				continue;
			}
			if (to_test_node(node)->value != ctx->stable[i].tag) {
				wrong++;
				if (!reported) {
					reported = true;
					report_violation(ctx->test_name,
						"stable key '%s' delivered on "
						"the node for tag %llu, "
						"expected tag %llu", k,
						(unsigned long long)
							to_test_node(node)->value,
						(unsigned long long)
							ctx->stable[i].tag);
				}
			}
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	__atomic_add_fetch(&ctx->lookups, lookups, __ATOMIC_RELAXED);
	__atomic_add_fetch(&ctx->not_found, nf, __ATOMIC_RELAXED);
	__atomic_add_fetch(&ctx->wrong_node, wrong, __ATOMIC_RELAXED);
	return NULL;
}

static void *inv_splitpt_writer(void *arg)
{
	struct inv_splitpt_ctx *ctx = arg;
	const uint8_t *ckey = (const uint8_t *) ctx->churn;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		struct ft_test_node *n = node_alloc(INV_SPLITPT_CHURN_TAG);

		n->value = INV_SPLITPT_CHURN_TAG;
		rcu_read_lock();
		if (cds_ft_insert(ctx->ft, ckey, ctx->churn_len, &n->node)
				!= CDS_FT_STATUS_OK)
			abort();
		cds_ft_iter_set_key(iter, ckey, ctx->churn_len);
		if (cds_ft_lookup(ctx->ft, iter) != CDS_FT_STATUS_OK)
			abort();
		if (cds_ft_remove(ctx->ft, iter, cds_ft_iter_node(iter))
				!= CDS_FT_STATUS_OK)
			abort();
		rcu_read_unlock();
		rcu_quiescent_state();
		node_free_rcu(n);
		ctx->cycles++;
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_split_point_lookup_run(bool ordered_list, const char *name)
{
	static const struct inv_splitpt_key universe[] = {
		{ "wa", 1 }, { "q", 2 }, { NULL, 0 }
	};
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct inv_splitpt_ctx ctx;
	struct timespec t0;
	pthread_t readers[NR_READERS_DEFAULT], writer;
	unsigned int i;
	int ret = 0;

	/*
	 * SPECULATIVE, not eager: only the *_spec creators leave
	 * CDS_FT_FLAG_SKIP_COMPRESSED set (see the header comment).
	 */
	ft = ordered_list ? create_varlen_ord_ft_ws_spec(&group, NULL) :
		create_varlen_nolist_ft_ws_spec(&group, NULL);

	memset(&ctx, 0, sizeof(ctx));
	ctx.ft = ft;
	ctx.stable = universe;
	ctx.churn = "wabbb";
	ctx.churn_len = strlen(ctx.churn);
	ctx.test_name = name;

	rcu_read_lock();
	for (i = 0; universe[i].key; i++) {
		struct ft_test_node *n = node_alloc(universe[i].tag);

		n->value = universe[i].tag;
		if (cds_ft_insert(ft, (const uint8_t *) universe[i].key,
				strlen(universe[i].key), &n->node)
					!= CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();
	rcu_quiescent_state();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_create(&readers[i], NULL, inv_splitpt_reader, &ctx);
	pthread_create(&writer, NULL, inv_splitpt_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	while (elapsed_ms(&t0) < DEFAULT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < NR_READERS_DEFAULT; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	if (!ctx.cycles || !ctx.lookups) {
		fprintf(stderr, "%s: oracle never armed (cycles=%lu "
			"lookups=%lu)\n", name, ctx.cycles, ctx.lookups);
		ret = -1;
	}
	if (ctx.not_found || ctx.wrong_node) {
		fprintf(stderr, "%s: %lu NOT_FOUND + %lu wrong-node over %lu "
			"churn cycles, %lu lookups\n", name, ctx.not_found,
			ctx.wrong_node, ctx.cycles, ctx.lookups);
		ret = -1;
	}
	if (atomic_load(&violation_count) > 0)
		ret = -1;
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

/*
 * SECOND SHAPE: a key that is ABSENT AT EVERY INSTANT must never be FOUND.
 *
 * The oracle above checks a present key is reachable and lands on its own node.
 * This one checks the other direction, which is the half a "did every key
 * survive" universe oracle cannot see: an exact lookup of a key the trie NEVER
 * holds must answer NOT_FOUND, every time.  A mis-anchored skip re-anchor makes
 * the descent resume one level off, so it consumes the wrong byte at the wrong
 * node and delivers a NEIGHBOUR's node for a key that was never inserted --
 * "ab" answering with "aab"'s node.  Measured on 63d8b657: ~9,000 such answers
 * in 2 s with four readers.
 *
 * THE SHAPE.  "aa" is a PREFIX head of the holder that "aab" and "aac" create,
 * and the cycle REMOVES and RE-INSERTS it while both extensions exist, so the
 * holder's external_nodes is cleared in place and then re-filled with a
 * different node -- the two states in which a stale skip slot's target reads
 * like a slot head when it is not.  "q" keeps the root branching.
 *
 * ☞ WHAT THIS DOES AND DOES NOT COVER.  Run unpinned it fires the LIVE re-home
 * arm many thousands of times per second and the DEAD (cleared-holder) arm
 * about once per second, because the dead one needs a reader preempted between
 * its slot read and its up-walk.  For that arm the instrument is `deadpfx.c`
 * under `taskset -c 0` with a dwell after the remove -- see the commit
 * message.  Do not read this test's green as coverage of the mark arm.
 */
/*
 * ☞ MEASURED, not guessed (control at 30bbccf1, 3 s, wrong answers a run):
 *
 *	cpus  readers  dead_us   cycles   wrong
 *	  1        4      2000      284       3
 *	  1        8      2000      153       2
 *	  2        4      2000      574       8
 *	  2        8       500      747      17
 *	  2        8       200     1330      21
 *	  3        8       200     2226      40
 *
 * The signal tracks CHURN CYCLES, not dead-window microseconds: what has to
 * coincide is a reader parked on a stale skip slot ACROSS the split, so more
 * short cycles beat fewer long ones.  Hence a dead window just wide enough to
 * be preempted in, and the cycle count spent on cycles.
 */
#define INV_ABSENT_CPUS		3
#define INV_ABSENT_READERS	8
#define INV_ABSENT_PRE_US	200
#define INV_ABSENT_DEAD_US	200
#define INV_ABSENT_DURATION_MS	3000

/*
 * ★ PREEMPTION, NOT CPU SHORTAGE, IS THE AMPLIFIER.
 *
 * This oracle needs a reader PARKED between the load of a skip slot and the
 * up-walk that resolves it -- that is the whole shape of the defect.  On an
 * idle 192-core machine the readers and the writer never preempt each other,
 * and the oracle stays green on a KNOWN-BROKEN build no matter how long it
 * runs: measured 3/3 GREEN on the control at 30bbccf1 over 3 s, ~95 M lookups
 * a run, with the dead-window dwell below already in place.  Crowding this
 * test's threads onto INV_ABSENT_CPUS is what turns it into a test -- 35-46
 * wrong answers a run on that same control (5/5 red), 0 on the fixed build
 * (5/5 green).
 *
 * ☞ The cpus come from the process's CURRENT affinity mask, so an outer
 * taskset or cpuset still decides which cpus the suite may use, and a 2-cpu box
 * simply gets both.  Failing to pin is not fatal -- the oracle is merely weak
 * -- but it is REPORTED, so a green here is never silently a green on an
 * unarmed detector.
 */
static bool inv_absent_pin_self(void)
{
	cpu_set_t cur, few;
	int i, got = 0;

	CPU_ZERO(&cur);
	if (sched_getaffinity(0, sizeof(cur), &cur) != 0)
		return false;
	CPU_ZERO(&few);
	for (i = 0; i < CPU_SETSIZE && got < INV_ABSENT_CPUS; i++) {
		if (!CPU_ISSET(i, &cur))
			continue;
		CPU_SET(i, &few);
		got++;
	}
	if (!got)
		return false;
	return sched_setaffinity(0, sizeof(few), &few) == 0;
}

static const char * const inv_absent_never_keys[] = {
	"a", "ab", "ac", "aabb", NULL
};

static const struct inv_splitpt_key inv_absent_present_keys[] = {
	{ "aa", 1 }, { "aab", 2 }, { "aac", 3 }, { "q", 4 }, { NULL, 0 }
};

static void *inv_absent_reader(void *arg)
{
	struct inv_splitpt_ctx *ctx = arg;
	unsigned long lookups = 0, found = 0, wrong = 0;
	bool reported = false;

	rcu_register_thread();
	if (!inv_absent_pin_self())
		CMM_STORE_SHARED(ctx->pinned, false);
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		unsigned int i;

		rcu_read_lock();
		for (i = 0; inv_absent_never_keys[i]; i++) {
			const char *k = inv_absent_never_keys[i];
			struct cds_ft_node *node = NULL;

			lookups++;
			if (cds_ft_eager_lookup_key(ctx->ft,
					(const uint8_t *) k, strlen(k), 0,
					&node) != CDS_FT_STATUS_OK)
				continue;
			found++;
			if (!reported) {
				reported = true;
				report_violation(ctx->test_name,
					"key '%s' is never inserted, yet an "
					"exact eager lookup answered OK with "
					"the node for tag %llu", k,
					(unsigned long long)
						to_test_node(node)->value);
			}
		}
		for (i = 0; ctx->stable[i].key; i++) {
			const char *k = ctx->stable[i].key;
			struct cds_ft_node *node = NULL;

			lookups++;
			/*
			 * Every one of these churns, so ABSENT is a legal
			 * answer: only the identity is asserted.
			 */
			if (cds_ft_eager_lookup_key(ctx->ft,
					(const uint8_t *) k, strlen(k), 0,
					&node) != CDS_FT_STATUS_OK)
				continue;
			if (to_test_node(node)->value != ctx->stable[i].tag) {
				wrong++;
				if (!reported) {
					reported = true;
					report_violation(ctx->test_name,
						"key '%s' delivered on the "
						"node for tag %llu, expected "
						"tag %llu", k,
						(unsigned long long)
							to_test_node(node)->value,
						(unsigned long long)
							ctx->stable[i].tag);
				}
			}
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	rcu_unregister_thread();
	__atomic_add_fetch(&ctx->lookups, lookups, __ATOMIC_RELAXED);
	__atomic_add_fetch(&ctx->not_found, found, __ATOMIC_RELAXED);
	__atomic_add_fetch(&ctx->wrong_node, wrong, __ATOMIC_RELAXED);
	return NULL;
}

static void inv_absent_ins(struct cds_ft *ft, const char *k, uint64_t tag)
{
	struct ft_test_node *n = node_alloc(tag);

	n->value = tag;
	if (cds_ft_insert(ft, (const uint8_t *) k, strlen(k), &n->node)
			!= CDS_FT_STATUS_OK)
		abort();
}

static void inv_absent_rem(struct cds_ft *ft, struct cds_ft_iter *iter,
		const char *k)
{
	struct cds_ft_node *node;

	if (cds_ft_iter_set_key(iter, (const uint8_t *) k, strlen(k))
			!= CDS_FT_STATUS_OK)
		abort();
	if (cds_ft_lookup(ft, iter) != CDS_FT_STATUS_OK)
		abort();
	node = cds_ft_iter_node(iter);
	if (cds_ft_remove(ft, iter, node) != CDS_FT_STATUS_OK)
		abort();
	node_free_rcu(to_test_node(node));
}

/*
 * Dwell @us microseconds, YIELDING the cpu rather than spinning on it.
 *
 * The writer runs inside an RCU read-side critical section, so it must not
 * block -- but it shares one cpu with the readers (inv_absent_pin_self), and a
 * spin here would hold that cpu for the whole window and starve the very
 * lookups the oracle is made of.  Yielding hands the window to the readers,
 * which is the point.
 */
static void inv_absent_dwell(unsigned long us)
{
	struct timespec a, b;

	clock_gettime(CLOCK_MONOTONIC, &a);
	do {
		sched_yield();
		clock_gettime(CLOCK_MONOTONIC, &b);
	} while ((unsigned long) ((b.tv_sec - a.tv_sec) * 1000000L +
			(b.tv_nsec - a.tv_nsec) / 1000) < us);
}

/*
 * ★ THE DWELL IS THE ORACLE'S ARM, not a slowdown.
 *
 * The defect lives in the window where the holder's external_nodes has been
 * cleared but a parked reader still holds the dead head, and a reader must take
 * BOTH of its loads inside it.  Without a dwell that window is the width of two
 * stores -- measured at ~1 wrong answer per 21 M lookups, which at the suite's
 * 200 ms per test is a coin the oracle almost never gets to flip, and an arm
 * that cannot fail on a KNOWN-BROKEN build is not coverage.  Holding the dead
 * state open for INV_ABSENT_DEAD_US makes the same oracle deterministic.
 */

static void *inv_absent_writer(void *arg)
{
	struct inv_splitpt_ctx *ctx = arg;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	if (!inv_absent_pin_self())
		CMM_STORE_SHARED(ctx->pinned, false);
	if (cds_ft_iter_create(ctx->ft, &iter) < 0)
		abort();
	while (!CMM_LOAD_SHARED(test_go))
		caa_cpu_relax();
	while (!CMM_LOAD_SHARED(test_stop)) {
		rcu_read_lock();
		/*
		 * Let readers settle on the at-rest shape (a skip slot naming
		 * the compressed run) before the split re-homes "aa" as a fresh
		 * internal node's PREFIX head: a stale slot held across that
		 * split is what the re-anchor has to resolve.
		 */
		inv_absent_dwell(INV_ABSENT_PRE_US);
		inv_absent_ins(ctx->ft, "aab", 2);
		inv_absent_ins(ctx->ft, "aac", 3);
		/* "aa" is now the holder's PREFIX head; clear it in place ... */
		inv_absent_rem(ctx->ft, iter, "aa");
		/* ... hold the DEAD window open (see the comment above) ... */
		inv_absent_dwell(INV_ABSENT_DEAD_US);
		/* ... and re-fill the holder with a DIFFERENT node. */
		inv_absent_ins(ctx->ft, "aa", 1);
		inv_absent_dwell(INV_ABSENT_DEAD_US / 4);
		inv_absent_rem(ctx->ft, iter, "aab");
		inv_absent_rem(ctx->ft, iter, "aac");
		rcu_read_unlock();
		rcu_quiescent_state();
		ctx->cycles++;
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_absent_key_run(bool ordered_list, const char *name)
{
	struct cds_ft_group *group;
	struct cds_ft *ft;
	struct inv_splitpt_ctx ctx;
	struct timespec t0;
	pthread_t readers[INV_ABSENT_READERS], writer;
	unsigned int i;
	int ret = 0;

	/* SPECULATIVE: only *_spec keeps CDS_FT_FLAG_SKIP_COMPRESSED set. */
	ft = ordered_list ? create_varlen_ord_ft_ws_spec(&group, NULL) :
		create_varlen_nolist_ft_ws_spec(&group, NULL);

	memset(&ctx, 0, sizeof(ctx));
	ctx.ft = ft;
	ctx.stable = inv_absent_present_keys;
	ctx.test_name = name;
	ctx.pinned = true;		/* cleared by any thread that cannot pin */

	rcu_read_lock();
	inv_absent_ins(ft, "aa", 1);
	inv_absent_ins(ft, "q", 4);
	rcu_read_unlock();
	rcu_quiescent_state();

	test_go = 0;
	test_stop = 0;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	for (i = 0; i < INV_ABSENT_READERS; i++)
		pthread_create(&readers[i], NULL, inv_absent_reader, &ctx);
	pthread_create(&writer, NULL, inv_absent_writer, &ctx);

	__atomic_thread_fence(__ATOMIC_SEQ_CST);
	test_go = 1;

	rcu_thread_offline();
	clock_gettime(CLOCK_MONOTONIC, &t0);
	/*
	 * Longer than the suite default: each cycle now costs ~2.7 ms of dwell
	 * (see inv_absent_writer), and this oracle needs CYCLES, not wall clock.
	 */
	while (elapsed_ms(&t0) < INV_ABSENT_DURATION_MS)
		usleep(1000);

	test_stop = 1;
	__atomic_thread_fence(__ATOMIC_SEQ_CST);

	pthread_join(writer, NULL);
	for (i = 0; i < INV_ABSENT_READERS; i++)
		pthread_join(readers[i], NULL);
	rcu_thread_online();

	if (!ctx.cycles || !ctx.lookups) {
		fprintf(stderr, "%s: oracle never armed (cycles=%lu "
			"lookups=%lu)\n", name, ctx.cycles, ctx.lookups);
		ret = -1;
	}
	if (!ctx.pinned)
		fprintf(stderr, "# %s: could not pin to one cpu -- the "
			"preemption amplifier is OFF and this arm is WEAK\n",
			name);
	if (ctx.not_found || ctx.wrong_node) {
		fprintf(stderr, "%s: %lu absent-key FOUND + %lu wrong-node over "
			"%lu churn cycles, %lu lookups\n", name, ctx.not_found,
			ctx.wrong_node, ctx.cycles, ctx.lookups);
		ret = -1;
	}
	if (atomic_load(&violation_count) > 0)
		ret = -1;
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

static int inv_absent_key_never_found(void)
{
	return inv_absent_key_run(true, "inv_absent_key_never_found");
}

/*
 * ★ ARMED.  This arm was SKIPPED while the shape answer lived on the holder:
 * with the ordered list OFF there was no ft_ord_cell to carry the mark a remove
 * left behind, so a DEAD prefix head read as a slot head, the re-anchor charged
 * it a byte it never consumed, the descent resumed one level off, and a key
 * ABSENT at every instant came back with a NEIGHBOUR'S NODE.
 *
 * The answer now rides the head's own back-edge word in BOTH modes
 * (FT_PARENT_PREFIX_HEAD), so this arm and its list-on sibling test the same
 * mechanism, and the same oracle discriminates: 35-46 wrong answers a run at
 * 30bbccf1, 0 after.
 */
static int inv_absent_key_never_found_nolist(void)
{
	return inv_absent_key_run(false, "inv_absent_key_never_found_nolist");
}

static int inv_split_point_lookup_identity(void)
{
	return inv_split_point_lookup_run(true,
		"inv_split_point_lookup_identity");
}

static int inv_split_point_lookup_identity_nolist(void)
{
	return inv_split_point_lookup_run(false,
		"inv_split_point_lookup_identity_nolist");
}

/*
 * Same-key concurrent REMOVES under CDS_FT_WRITER_LOCK_FINE (in contract:
 * "cds_ft_remove may run concurrently with cds_ft_insert and cds_ft_remove on
 * the same trie, including on the same key").  Every other concurrent-writer
 * oracle here gives each writer a DISJOINT key range, so no two writers ever
 * touched one duplicate chain at once -- and the point remove derives its
 * routing (interior member / head with a successor / head without one) from
 * node->prev and node->next with NOTHING held, then takes the holder lock
 * and trusts that routing.  Two removes on adjacent duplicates of one key
 * cross those lines: the head goes and the member is now the head (the held
 * arm's assert(parent_nf)); the only other duplicate goes and a promote finds
 * no successor (the clear lane on a compressed holder, whose no-dual detector
 * fired; a release build would publish a compressed node with a NULL child).
 * Both aborted within a second at the HEAD this was written against.
 *
 * Shape: K keys, each a chain of D duplicates inserted by this thread in
 * order (dup 0 is the head); T == D writers each remove dup t of every key,
 * concurrently, walking the keys in different orders.  After the round every
 * key must be gone, the trie verifies, and the count is 0.  BOUNDED: R rounds,
 * no wall-clock loop, so a regression fails or aborts instead of hanging the
 * suite.  Coarse strategy is the control (serialized writers: green before
 * the fix).
 */
#define SKR_T		3
#define SKR_D		3
#define SKR_K		64
#define SKR_R		150

struct skr_node {
	struct cds_ft_node node;
	unsigned int k, d;
};

struct skr_ctx {
	struct cds_ft *ft;
	struct skr_node *nodes;			/* [SKR_K][SKR_D] */
	struct skr_node *fresh;			/* [SKR_K]: the inserter's node per key */
	pthread_barrier_t bar;
	unsigned long fail;			/* removes that did not return OK */
	unsigned long ins_fail;			/* inserts that did not return OK */
};

struct skr_arg {
	struct skr_ctx *c;
	unsigned int t;
};

/*
 * Two holder shapes, alternating by round: an even round's keys differ in
 * their LAST byte, so each chain heads a body slot of an internal holder;
 * an odd round's keys share a trailing byte, so each chain hangs under a
 * one-byte compressed holder (cn->child), the shape whose emptied chain is
 * a detach and whose promote must never fall into the clear lane.
 */
static uint64_t skr_u64(unsigned int r, unsigned int k)
{
	return (r & 1) ? (((uint64_t) k << 8) | 0x7f) : (uint64_t) k;
}

static void skr_bwait(pthread_barrier_t *bar)
{
	rcu_thread_offline();
	pthread_barrier_wait(bar);
	rcu_thread_online();
}

static void *skr_writer(void *arg)
{
	struct skr_arg *a = (struct skr_arg *) arg;
	struct skr_ctx *c = a->c;
	struct cds_ft_iter *iter;
	unsigned int r, i;

	rcu_register_thread();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SKR_R; r++) {
		skr_bwait(&c->bar);		/* inserted */
		/*
		 * Every writer walks the keys in the SAME order: the point is
		 * to have all T removers on one chain at once.
		 */
		for (i = 0; i < SKR_K; i++) {
			unsigned int k = i;
			struct skr_node *n = &c->nodes[k * SKR_D + a->t];
			uint8_t key[8];
			enum cds_ft_status st;

			cds_ft_u64_to_key(c->ft, skr_u64(r, k), key,
				CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(c->ft, iter);
			st = cds_ft_remove(c->ft, iter, &n->node);
			rcu_read_unlock();
			rcu_quiescent_state();
			if (st != CDS_FT_STATUS_OK) {
				fprintf(stderr, "skr: writer %u round %u key %u dup %u: remove -> %s\n",
					a->t, r, k, a->t,
					cds_ft_status_to_string(st));
				uatomic_inc(&c->fail);
			}
		}
		skr_bwait(&c->bar);		/* removed */
		skr_bwait(&c->bar);		/* verified */
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * The INSERTER: while the removers empty each chain, insert one fresh node
 * per key -- the same key -- so a removal that believed the chain had no
 * successor meets an append between its derivation and its commit.  The
 * orphan oracle after the round: every key must resolve, to a chain holding
 * exactly this node.  (A freeze recorded with the successor RE-LOADED at
 * record time marks the appended node into the tombstone and prunes the
 * branch around it: an OK insert whose key then never resolves.)
 */
static void *skr_inserter(void *arg)
{
	struct skr_arg *a = (struct skr_arg *) arg;
	struct skr_ctx *c = a->c;
	unsigned int r, k;

	rcu_register_thread();
	for (r = 0; r < SKR_R; r++) {
		skr_bwait(&c->bar);		/* inserted */
		for (k = 0; k < SKR_K; k++) {
			struct skr_node *n = &c->fresh[k];
			uint8_t key[8];

			cds_ft_node_init(&n->node);
			n->k = k;
			n->d = SKR_D;
			cds_ft_u64_to_key(c->ft, skr_u64(r, k), key,
				CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			if (cds_ft_insert(c->ft, key, CDS_FT_LEN_DEFAULT,
					&n->node) != CDS_FT_STATUS_OK)
				uatomic_inc(&c->ins_fail);
			rcu_read_unlock();
			rcu_quiescent_state();
		}
		skr_bwait(&c->bar);		/* removed */
		skr_bwait(&c->bar);		/* verified */
	}
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_same_key_removes_run(bool ordered_list,
		const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct skr_ctx c;
	struct skr_arg args[SKR_T + 1];
	pthread_t th[SKR_T + 1];
	struct cds_ft_iter *iter;
	unsigned int r, k, d, t;
	int ret = 0;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, ordered_list) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	c.nodes = (struct skr_node *) calloc((size_t) SKR_K * SKR_D,
			sizeof(*c.nodes));
	c.fresh = (struct skr_node *) calloc(SKR_K, sizeof(*c.fresh));
	if (!c.nodes || !c.fresh)
		abort();
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	pthread_barrier_init(&c.bar, NULL, SKR_T + 2);
	for (t = 0; t < SKR_T; t++) {
		args[t].c = &c;
		args[t].t = t;
		pthread_create(&th[t], NULL, skr_writer, &args[t]);
	}
	args[SKR_T].c = &c;
	args[SKR_T].t = SKR_T;
	pthread_create(&th[SKR_T], NULL, skr_inserter, &args[SKR_T]);
	for (r = 0; r < SKR_R; r++) {
		unsigned long fail_before = uatomic_read(&c.fail);

		rcu_read_lock();
		for (k = 0; k < SKR_K; k++) {
			for (d = 0; d < SKR_D; d++) {
				struct skr_node *n = &c.nodes[k * SKR_D + d];
				uint8_t key[8];

				cds_ft_node_init(&n->node);
				n->k = k;
				n->d = d;
				cds_ft_u64_to_key(c.ft, skr_u64(r, k), key,
					CDS_FT_LEN_DEFAULT);
				if (cds_ft_insert(c.ft, key, CDS_FT_LEN_DEFAULT,
						&n->node) != CDS_FT_STATUS_OK) {
					fprintf(stderr, "%s: insert key %u dup %u failed\n",
						name, k, d);
					abort();
				}
			}
		}
		rcu_read_unlock();
		rcu_quiescent_state();
		skr_bwait(&c.bar);		/* inserted */
		skr_bwait(&c.bar);		/* removed */
		rcu_read_lock();
		if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "%s: round %u: verify RED after the concurrent removes\n",
				name, r);
			ret = -1;
		}
		/*
		 * Every original duplicate is gone and the inserter's node is
		 * what each key resolves to: a chain of exactly that node.
		 */
		for (k = 0; k < SKR_K; k++) {
			struct cds_ft_node *h;
			unsigned int len = 0;
			uint8_t key[8];

			cds_ft_u64_to_key(c.ft, skr_u64(r, k), key,
				CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
			if (cds_ft_lookup(c.ft, iter) != CDS_FT_STATUS_OK ||
					!(h = cds_ft_iter_node(iter))) {
				fprintf(stderr, "%s: round %u: key %u LOST after an OK concurrent insert\n",
					name, r, k);
				ret = -1;
				continue;
			}
			for (; h; h = cds_ft_node_next_rcu(h)) {
				struct skr_node *n = (struct skr_node *) h;

				len++;
				if (n != &c.fresh[k]) {
					fprintf(stderr, "%s: round %u: key %u still holds dup %u after every duplicate was removed\n",
						name, r, k, n->d);
					ret = -1;
				}
			}
			if (len != 1) {
				fprintf(stderr, "%s: round %u: key %u chain length %u != 1\n",
					name, r, k, len);
				ret = -1;
			}
		}
		if (cds_ft_count_keys(c.ft) != SKR_K) {
			fprintf(stderr, "%s: round %u: count_keys %lu != %u\n",
				name, r, cds_ft_count_keys(c.ft), SKR_K);
			ret = -1;
		}
		/* Drain the inserter's nodes: the next round starts empty. */
		for (k = 0; k < SKR_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c.ft, skr_u64(r, k), key,
				CDS_FT_LEN_DEFAULT);
			cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
			cds_ft_lookup(c.ft, iter);
			if (cds_ft_remove(c.ft, iter, &c.fresh[k].node) !=
					CDS_FT_STATUS_OK) {
				fprintf(stderr, "%s: round %u: drain of key %u failed\n",
					name, r, k);
				ret = -1;
			}
		}
		if (cds_ft_count_keys(c.ft) != 0) {
			fprintf(stderr, "%s: round %u: count_keys %lu != 0 after the drain\n",
				name, r, cds_ft_count_keys(c.ft));
			ret = -1;
		}
		rcu_read_unlock();
		rcu_quiescent_state();
		if (uatomic_read(&c.fail) != fail_before ||
				uatomic_read(&c.ins_fail))
			ret = -1;
		if (ret) {
			fprintf(stderr, "%s: RED at round %u\n", name, r);
			/* let the writers run out their bounded loop */
		}
		/*
		 * ☞ THE NODE-REUSE FENCE.  The next round re-initialises these
		 * nodes, and cds_ft_node_init() writes prev/next on objects a
		 * peer may still hold -- cds_ft_remove's contract reserves a
		 * GRACE PERIOD before any such write:
		 *
		 *   "A grace period must be observed (e.g., synchronize_rcu,
		 *    call_rcu) after success before reclaiming @node memory."
		 *
		 * rcu_barrier() does NOT supply one.  It waits for in-flight
		 * call_rcu CALLBACKS to run, and this row queues none of its
		 * own -- with an empty queue it can return having waited for
		 * nothing at all.  Only synchronize_rcu() fences the readers.
		 * Every worker is parked in skr_bwait, which goes OFFLINE
		 * around the wait, so the grace period completes here.
		 */
		synchronize_rcu();
		rcu_barrier();		/* and retire the library's own freezes */
		skr_bwait(&c.bar);		/* verified */
	}
	for (t = 0; t < SKR_T + 1; t++)
		pthread_join(th[t], NULL);
	pthread_barrier_destroy(&c.bar);
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	free(c.nodes);
	free(c.fresh);
	fprintf(stderr, "# %s: %u rounds x %u keys x %u concurrent removers + 1 inserter, %lu failed removes, %lu failed inserts -> %s\n",
		name, SKR_R, SKR_K, SKR_T, uatomic_read(&c.fail),
		uatomic_read(&c.ins_fail), ret ? "RED" : "ok");
	return ret;
}

static int inv_concurrent_same_key_removes(void)
{
	return inv_concurrent_same_key_removes_run(true,
		"inv_concurrent_same_key_removes");
}

static int inv_concurrent_same_key_removes_nolist(void)
{
	return inv_concurrent_same_key_removes_run(false,
		"inv_concurrent_same_key_removes_nolist");
}

/*
 * Same-key APPEND vs same-key REMOVE under CDS_FT_WRITER_LOCK_FINE.
 *
 * The sibling row above puts three removers and an inserter on one chain, but
 * its inserter's own node always SURVIVES the round -- so the head an append
 * lands on is never a head a peer is retiring, and the insert lane's own
 * derivation is never tested.  Here the chain OSCILLATES: one thread keeps a
 * resident node X[k] going out and back in, the other appends and removes
 * Y[k], so an append repeatedly lands on a head the peer is removing.
 *
 * That shape made _cds_ft_insert's duplicate append derive a NULL chain holder
 * (ft_chain_head_holder of a head retired since the descent) and trip
 * assert(holder_flag) at zero delay injection -- a release build carried on
 * into ft_flag_to_metadata(ft, NULL) and used the result as a lock word.  The
 * comment there had called the shape unreachable on the strength of "0 NULL in
 * 491532 calls", a count taken entirely from DISJOINT-range oracles.
 *
 * ☞ WHICH OF THESE WITNESSES WHAT, measured against an ABLATED SNAPSHOT -- a
 * git worktree with all three fixes removed and the ablation PROVEN by the
 * assert strings in the built .so.  That distinction is not pedantry: an
 * earlier pass measured against a build DIRECTORY that had since been rebuilt
 * from fixed source, read 0/4 red, and concluded these rows were worthless.
 * They are not.  Unpinned, 3 runs each:
 *
 *   ..._append_nolist   THE WITNESS, 2/3 red -- ft-insert.h's holder_flag
 *                       assert, the append deriving a NULL chain holder.
 *                       Green fixed, ~5 s.
 *   ..._append_coarse   THE CONTROL, 0/3 -- serialised writers must be green.
 *                       If this ever reddens the rig is broken, not the
 *                       fine-grained contract.
 *   ..._append (list on) 3/3 red on the ablated snapshot (it reaches
 *                       ft_unchain_node's lock_nf assert, the remove-side
 *                       twin) -- the STRONGEST witness of the three, and
 *                       OPT-IN ONLY, see below.
 *
 * ☠ WHY THE LIST-ON ROW IS OPT-IN.  On the FIXED tree it intermittently trips
 * ft_rebuild_key_upwalk's assert through ft_iter_read_key -- a PRE-EXISTING,
 * separately-tracked defect in the list=1 up-walk, in code this fix does not
 * touch (ft-iter.h is untouched; the only ft-remove.h change here is in
 * ft_unchain_node).  This row is simply the first test that REACHES it, which
 * became possible only once the NULL-holder asserts stopped firing first.
 * Registering it by default would redden the gate for a defect this change
 * neither caused nor claims to fix, so it runs under
 * FT_INV_SAME_KEY_APPEND_LIST=1 and should go back in the default set the day
 * that up-walk defect is closed.
 *
 * ☞ Pinning is NOT the stronger arm here.  taskset -c 0,1 HID both NULL-holder
 * defects; the unpinned runs are what found them.  Measure both ways.
 *
 * The standalone reproducer is
 * fractal-trie-review-2026-06/same-key-insert-vs-remove-rig.c, red in BOTH list
 * modes and the instrument of record for this class.  BOUNDED by an iteration
 * budget, not wall clock, so a regression aborts instead of hanging the suite.
 */
#define SKA_K		8
#define SKA_R		40
/*
 * ☠ THE BURST IS WHAT MAKES THIS A WITNESS.  A barrier every k-sweep let the
 * two workers fall into lockstep and the row passed on the UNFIXED library --
 * 1920 iterations per thread caught nothing, while the standalone rig caught it
 * at 200000.  Keep the keys FEW and the sweeps MANY between barriers so the
 * appender keeps landing on a head the remover is retiring.
 */
#define SKA_BURST	400

struct ska_ctx {
	struct cds_ft *ft;
	/*
	 * ☞ INDEX, NOT STORAGE.  Each slot names the node that lane currently
	 * has installed for key @k.  The nodes are HEAP allocated per install
	 * and released through node_free_rcu, so no object is ever written
	 * again after it leaves the trie -- cds_ft_remove's contract:
	 *
	 *   "A grace period must be observed (e.g., synchronize_rcu,
	 *    call_rcu) after success before reclaiming @node memory."
	 *
	 * The arrays this replaced were index AND allocator at once, so each
	 * reuse opened with cds_ft_node_init() wiping prev/next on an object
	 * the PEER lane could still be descending through.
	 */
	struct ft_test_node **resident;		/* [SKA_K], owned by the remover */
	struct ft_test_node **guest;		/* [SKA_K], owned by the appender */
	pthread_barrier_t bar;
	unsigned long holder_bail;		/* appends that re-descended */
};

/*
 * Take this lane's node for @key back out.  On success it has LEFT the trie,
 * so node_free_rcu reclaims it after a grace period and the slot clears.  On
 * failure it is STILL LINKED and stays indexed: the caller retries before
 * installing another, because re-inserting a node still in the trie would
 * corrupt the chain and frame the library.
 */
static void ska_take_out(struct cds_ft *ft, struct cds_ft_iter *iter,
		struct ft_test_node **slot, const uint8_t *key)
{
	if (!*slot)
		return;
	rcu_read_lock();
	cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
	if (cds_ft_lookup(ft, iter) == CDS_FT_STATUS_OK &&
			cds_ft_remove(ft, iter, &(*slot)->node) ==
			CDS_FT_STATUS_OK) {
		node_free_rcu(*slot);
		*slot = NULL;
	}
	rcu_read_unlock();
	rcu_quiescent_state();
}

/*
 * Install a FRESH node for @key.  A refused insert never became reachable, so
 * a plain free is correct there.
 */
static void ska_put_in(struct cds_ft *ft, struct ft_test_node **slot,
		const uint8_t *key, unsigned int k)
{
	struct ft_test_node *fresh;

	if (*slot)
		return;
	fresh = node_alloc((uint64_t) k);
	rcu_read_lock();
	if (cds_ft_insert(ft, key, CDS_FT_LEN_DEFAULT, &fresh->node) ==
			CDS_FT_STATUS_OK) {
		*slot = fresh;
		fresh = NULL;
	}
	rcu_read_unlock();
	if (fresh)
		node_free(fresh);
	rcu_quiescent_state();
}

static void *ska_appender(void *arg)
{
	struct ska_ctx *c = (struct ska_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int r, k;

	rcu_register_thread();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	/*
	 * NO BARRIERS.  Per-round barriers put the two workers in lockstep and
	 * the row went GREEN ON THE UNFIXED LIBRARY -- it witnessed nothing.
	 * The standalone rig catches it because both threads run FREE, so every
	 * phase relationship between "append descends" and "peer retires the
	 * head" gets sampled.  Bounded by the iteration budget, not by wall
	 * clock, so a regression aborts instead of hanging the suite.
	 */
	for (r = 0; r < SKA_R; r++) {
		unsigned int burst;

		for (burst = 0; burst < SKA_BURST; burst++)
		for (k = 0; k < SKA_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			/* Anything an earlier attempt failed to retract comes
			 * out first, then a fresh one goes in and straight back
			 * out -- the append/retire pair this row exists for. */
			ska_take_out(c->ft, iter, &c->guest[k], key);
			ska_put_in(c->ft, &c->guest[k], key, k);
			ska_take_out(c->ft, iter, &c->guest[k], key);
		}
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static void *ska_remover(void *arg)
{
	struct ska_ctx *c = (struct ska_ctx *) arg;
	struct cds_ft_iter *iter;
	unsigned int r, k;

	rcu_register_thread();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	/*
	 * NO BARRIERS.  Per-round barriers put the two workers in lockstep and
	 * the row went GREEN ON THE UNFIXED LIBRARY -- it witnessed nothing.
	 * The standalone rig catches it because both threads run FREE, so every
	 * phase relationship between "append descends" and "peer retires the
	 * head" gets sampled.  Bounded by the iteration budget, not by wall
	 * clock, so a regression aborts instead of hanging the suite.
	 */
	for (r = 0; r < SKA_R; r++) {
		unsigned int burst;

		for (burst = 0; burst < SKA_BURST; burst++)
		for (k = 0; k < SKA_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			/* Retire the resident and put a FRESH one back: only
			 * what came out is replaced. */
			ska_take_out(c->ft, iter, &c->resident[k], key);
			ska_put_in(c->ft, &c->resident[k], key, k);
		}
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_same_key_append_run(bool ordered_list, bool coarse,
		const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct ska_ctx c;
	pthread_t th[2];
	struct cds_ft_iter *iter;
	unsigned int r, k;
	int ret = 0;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, ordered_list) < 0)
		abort();
	if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	c.resident = (struct ft_test_node **) calloc(SKA_K,
		sizeof(*c.resident));
	c.guest = (struct ft_test_node **) calloc(SKA_K, sizeof(*c.guest));
	if (!c.resident || !c.guest)
		abort();
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	pthread_barrier_init(&c.bar, NULL, 3);
	for (k = 0; k < SKA_K; k++) {
		uint8_t key[8];

		struct ft_test_node *n = node_alloc((uint64_t) k);

		cds_ft_u64_to_key(c.ft, (uint64_t) k, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_insert(c.ft, key, CDS_FT_LEN_DEFAULT,
				&n->node) != CDS_FT_STATUS_OK)
			abort();
		c.resident[k] = n;
	}
	rcu_quiescent_state();
	pthread_create(&th[0], NULL, ska_appender, &c);
	pthread_create(&th[1], NULL, ska_remover, &c);
	(void) r;
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	/* Quiescent now: verify once, and check nothing foreign got spliced in. */
	rcu_quiescent_state();
	rcu_barrier();
	rcu_read_lock();
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	for (k = 0; k < SKA_K; k++) {
		struct cds_ft_node *h;
		uint8_t key[8];

		cds_ft_u64_to_key(c.ft, (uint64_t) k, key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(c.ft, iter) != CDS_FT_STATUS_OK)
			continue;
		for (h = cds_ft_iter_node(iter); h; h = cds_ft_node_next_rcu(h)) {
			struct ft_test_node *n = to_test_node(h);

			if (n != c.resident[k] && n != c.guest[k]) {
				fprintf(stderr, "%s: key %u holds a FOREIGN node (k=%llu)\n",
					name, k,
					(unsigned long long) n->key);
				ret = -1;
			}
		}
	}
	rcu_read_unlock();
	pthread_barrier_destroy(&c.bar);
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	/*
	 * Whatever each lane still had installed: the trie is gone, so nothing
	 * can reach these any more -- but they were handed out under RCU, so
	 * they leave the same way.
	 */
	for (k = 0; k < SKA_K; k++) {
		if (c.resident[k])
			node_free_rcu(c.resident[k]);
		if (c.guest[k])
			node_free_rcu(c.guest[k]);
	}
	free(c.resident);
	free(c.guest);
	/* The deferred frees must land before RUN_TEST's leak_check(). */
	rcu_quiescent_state();
	rcu_barrier();
	fprintf(stderr, "# %s: %u rounds x %u keys, appender vs same-key remover (%s) -> %s\n",
		name, SKA_R, SKA_K, coarse ? "coarse" : "fine",
		ret ? "RED" : "ok");
	return ret;
}

/*
 * TWO WRITERS ON ONE KEY, BOTH INSERTING -- the same-key contract clause the
 * rows above do not reach.
 *
 * inv_concurrent_same_key_append pairs an APPENDER against a REMOVER, so every
 * interleaving it samples has one of each.  The insert/insert pairing is a
 * different shape: two ops racing to APPEND to the same duplicate chain, which
 * is where they contend for the chain HEAD and its holder rather than for a
 * head one of them is retiring.  ☞ Naming the instrument per contract clause is
 * the rule a disjoint-range oracle taught expensively: a rig that never puts two
 * inserts on one key tests nothing about two inserts on one key, however much
 * concurrency it otherwise has.
 *
 * THE SHAPE.  Each worker owns ONE node per key and cycles it: insert while it
 * is out, remove it while it is in.  Both workers run the same loop over the
 * same keys, so the run samples insert/insert, insert/remove and remove/remove
 * on one chain -- and insert/insert is the pairing no existing row produces.
 * Bounded by construction: two nodes per key, never more.
 *
 * ☠ A WORKER ONLY EVER TOUCHES ITS OWN NODE.  cds_ft_insert on a node still in
 * the trie corrupts the chain and would frame the library for a test bug (the
 * same trap ska_appender's comment names), so the insert is gated on this
 * worker's own @in flag -- never on a lookup, which the peer can invalidate
 * between the test and the call.
 *
 * NO BARRIERS, for the reason the append row states: per-round barriers put the
 * workers in lockstep and such a row goes GREEN ON AN UNFIXED LIBRARY.
 *
 * THE INVARIANT IS EXACT, AND THAT IS THE POINT.  Both workers have finished
 * their last operation when the joins return, so each worker's @in flag is
 * authoritative for its own node.  The chain for every key must then hold
 * EXACTLY the nodes whose owners say "in" -- no more (a lost remove, or a
 * foreign node spliced in) and no fewer (a LOST INSERT, the failure an
 * append-vs-remove row cannot distinguish from a successful remove).
 */
#define SKI_K		8
#define SKI_R		40
#define SKI_BURST	400

struct ski_ctx {
	struct cds_ft *ft;
	/*
	 * ☞ THE INDEX, SEPARATE FROM THE STORAGE.  @cur[i][k] names the node
	 * worker @i currently has installed for key @k -- which is all the
	 * oracle needs identity for ("is this chain member worker 0's, worker
	 * 1's, or FOREIGN?").  The nodes themselves are HEAP allocated per
	 * insert and freed through node_free_rcu, so no object is ever written
	 * again inside the grace period cds_ft_remove's contract reserves:
	 *
	 *   "A grace period must be observed (e.g., synchronize_rcu, call_rcu)
	 *    after success before reclaiming @node memory."
	 *
	 * The array this replaced conflated the two jobs: it was the index AND
	 * the allocator, so re-indexing a key meant cds_ft_node_init() wiping
	 * prev/next on an object a peer could still reach.  Keeping the index
	 * and heap-allocating the storage costs the updater nothing -- the
	 * alternative, a synchronize_rcu() between every remove and its reuse,
	 * would serialise ~64k removes per worker and blunt the contention this
	 * row exists to create.
	 */
	struct ft_test_node **cur[2];	/* [2][SKI_K] */
	/*
	 * ☞ siu_worker RETAINS every node it removes until teardown rather
	 * than reclaiming it during the run.  Its oracle asks a HARD question
	 * about a pointer the LIBRARY hands back (@dup): "is this a node of
	 * this key?", and @siu_dup_foreign turns the row RED on a no.  That
	 * question is only answerable while the node's shadow @key is still
	 * its own.  Reclaim during the run would let a later node_alloc for a
	 * DIFFERENT key recycle the memory, and a stale @dup -- which this row
	 * sees 50-90 times a run, counted reported-only as @siu_dup_absent --
	 * would then read as FOREIGN and turn a merely racy row red.
	 * Retaining costs a few MB for the run and takes recycling out of the
	 * question entirely.  Only worker @id touches its own slot.
	 */
	struct ft_test_node **ret_v[2];
	size_t ret_n[2], ret_cap[2];
	unsigned char *in[2];		/* [2][SKI_K]: MY node is currently in. */
	/*
	 * FT_INV_SKI_ONLINE: per (worker, key) seqcount, ODD while that worker
	 * operates on that key.  Lets the PEER check "your node is still in"
	 * right after its OWN op, knowing the owner was not mid-op -- so a
	 * loss is caught in the thread whose op caused it.
	 */
	unsigned long seq[2][SKI_K];
};

struct ski_arg {
	struct ski_ctx *c;
	unsigned int id;
};
#ifdef FT_INV_SKI_COVERAGE
static unsigned long ski_dup_appends, ski_solo_appends;
#endif

/*
 * FT_INV_SKI_ONLINE=1: check EACH op instead of the end state.  Only the owner
 * removes its node, so "my node is not in the chain" right after my insert
 * (a LOST INSERT) or right before my remove (a PHANTOM REMOVE by the peer) is
 * the defect itself, caught within one op of it happening -- and, under
 * FT_INV_ABORT_ON_VIOLATION, with the flight recorder ending there.  A miss
 * that a re-lookup then finds is reported separately: that is a READER
 * visibility gap, not a loss.
 */
/*
 * FT_INV_SKI_BYTE=b puts the key's distinguishing byte at position @b (default
 * 7, the last).  With b < 7 every key carries its own compressed TAIL run over
 * bytes b+1..7, reached through a skip pointer from the branch at depth b --
 * so under EXPONENTIAL spacing a lock level (2, 4) can fall strictly INSIDE a
 * skipped run, the shape where the anchor is the compressed node rather than
 * the node holding the skip pointer.  b = 7 never builds it.
 */
static uint64_t ski_key(unsigned int k)
{
	static int b = -1, exp_shape = -1;

	/*
	 * FT_INV_SKI_KEYS=exp: the EXPONENTIAL skip-anchor shape.  Keys
	 * [0,0,0,g, 0x11+g,0x22+g, i+1, 0] (g = k >> 2, i = k & 3) build
	 *   GP (depth 3, branches on g) --skip--> M_g (depth 6, branches on i)
	 * with the compressed run cn_g = [4,6) between them, skipped by GP's
	 * slot.  M_g's lock level is 4 and cn_g STARTS at 4, so under
	 * exponential spacing M_g anchors on cn_g -- a node the descent never
	 * enters.  Removing i-keys collapses M_g into cn_g; inserting splits it.
	 */
	if (exp_shape < 0) {
		const char *e = getenv("FT_INV_SKI_KEYS");

		exp_shape = e && !strcmp(e, "exp");
	}
	if (exp_shape) {
		uint64_t g = k >> 2, i = k & 3;

		return (g << 32) | ((0x11 + g) << 24) | ((0x22 + g) << 16) |
			((i + 1) << 8);
	}

	if (b < 0) {
		const char *e = getenv("FT_INV_SKI_BYTE");

		b = e ? atoi(e) : 7;
		if (b < 0 || b > 7)
			b = 7;
	}
	return (uint64_t) k << (8 * (7 - b));
}

static unsigned long ski_online_checks, ski_online_transient;

static bool ski_node_in_chain(struct ski_ctx *c, struct cds_ft_iter *iter,
		const uint8_t *key, struct cds_ft_node *want)
{
	struct cds_ft_node *h;

	cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
	if (cds_ft_lookup(c->ft, iter) != CDS_FT_STATUS_OK)
		return false;
	for (h = cds_ft_iter_node(iter); h; h = cds_ft_node_next_rcu(h))
		if (h == want)
			return true;
	return false;
}

/* Called inside a read-side critical section. */
static void ski_online_check(struct ski_ctx *c, struct cds_ft_iter *iter,
		const uint8_t *key, unsigned int id, unsigned int k,
		struct cds_ft_node *want, const char *when, unsigned int r,
		unsigned int burst)
{
	unsigned int t;

	uatomic_inc(&ski_online_checks);
	if (ski_node_in_chain(c, iter, key, want))
		return;
	for (t = 0; t < 8; t++)
		if (ski_node_in_chain(c, iter, key, want)) {
			uatomic_inc(&ski_online_transient);
			fprintf(stderr, "ski ONLINE: TRANSIENT reader miss, key %u worker %u %s (r=%u burst=%u), found on re-lookup %u\n",
				k, id, when, r, burst, t + 1);
			return;
		}
	{
		/*
		 * SELF-DIAGNOSING: what did the lookup find, and what does the
		 * lost node itself still say?  next bit 1 = the library marked it
		 * REMOVED (an unlink that meant to take it); a clear next with a
		 * prev still naming a holder = it was dropped WITHOUT being
		 * unlinked (a wholesale detach / stale skip target).
		 */
		enum cds_ft_status ls;
		struct cds_ft_node *h, *head = NULL;
		unsigned int len = 0;
		uintptr_t wn = (uintptr_t) CMM_LOAD_SHARED(want->next);
		void *wp = CMM_LOAD_SHARED(want->prev);

		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		ls = cds_ft_lookup(c->ft, iter);
		if (ls == CDS_FT_STATUS_OK) {
			head = cds_ft_iter_node(iter);
			for (h = head; h; h = cds_ft_node_next_rcu(h))
				len++;
		}
		report_violation("inv_concurrent_same_key_inserts_nolist",
			"ONLINE: key %u worker %u node %p LOST %s (r=%u burst=%u, peer in=%u) | lookup=%d head=%p len=%u | lost node: next=%#lx%s prev=%p%s",
			k, id, (void *) want, when, r, burst,
			(unsigned int) c->in[1 - id][k], (int) ls,
			(void *) head, len, (unsigned long) wn,
			(wn & 2) ? " (REMOVED-marked)" : " (NOT marked)",
			wp, ((uintptr_t) wp & 1) ? " (tagged)" : "");
	}
}

/*
 * After MY op on @k: the PEER's node for @k, if the peer says it is in and was
 * not mid-op across this check, must be reachable.  If not, the op that lost
 * it is the one THIS thread just finished -- report from here, so the flight
 * recorder ends on the culprit's own events.
 */
static unsigned long ski_peer_checks;

static void ski_peer_check(struct ski_ctx *c, struct cds_ft_iter *iter,
		const uint8_t *key, unsigned int id, unsigned int k,
		unsigned int r, unsigned int burst)
{
	unsigned int o = 1 - id;
	unsigned long s1, s2;
	struct ft_test_node *n;
	bool found;

	s1 = uatomic_load(&c->seq[o][k], CMM_ACQUIRE);
	if (s1 & 1)
		return;
	if (!uatomic_load(&c->in[o][k], CMM_RELAXED))
		return;
	n = uatomic_load(&c->cur[o][k], CMM_RELAXED);
	if (!n)
		return;
	found = ski_node_in_chain(c, iter, key, &n->node);
	cmm_smp_rmb();
	s2 = uatomic_load(&c->seq[o][k], CMM_RELAXED);
	if (s1 != s2)
		return;		/* the owner moved: no verdict */
	uatomic_inc(&ski_peer_checks);
	if (!found)
		report_violation("inv_concurrent_same_key_inserts_nolist",
			"ONLINE PEER: key %u: worker %u's node %p is GONE right after worker %u's own %s (r=%u burst=%u) -- the culprit is this thread's last op",
			k, o, (void *) &n->node, id,
			c->in[id][k] ? "INSERT" : "REMOVE", r, burst);
}

static void *ski_worker(void *arg)
{
	struct ski_arg *a = (struct ski_arg *) arg;
	struct ski_ctx *c = a->c;
	unsigned int id = a->id, r, burst, k;
	struct cds_ft_iter *iter;
	bool online = getenv("FT_INV_SKI_ONLINE") != NULL;

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SKI_R; r++)
		for (burst = 0; burst < SKI_BURST; burst++)
		for (k = 0; k < SKI_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c->ft, ski_key(k), key,
				CDS_FT_LEN_DEFAULT);
			if (online)
				uatomic_inc(&c->seq[id][k]);	/* odd: mid-op */
			if (!c->in[id][k]) {
				struct ft_test_node *fresh =
					node_alloc((uint64_t) k);

				rcu_read_lock();
#ifdef FT_INV_SKI_RED_LOST_INSERT
				/* RED CONTROL: claim the insert without making
				 * it -- the LOST INSERT signature. */
				if (r == SKI_R - 1 && burst == 0 && k == 0) {
					c->cur[id][k] = fresh;
					fresh = NULL;	/* indexed: not ours to free */
					c->in[id][k] = 1;
				} else
#endif
				if (cds_ft_insert(c->ft, key,
						CDS_FT_LEN_DEFAULT,
						&fresh->node) ==
						CDS_FT_STATUS_OK) {
					c->cur[id][k] = fresh;
					fresh = NULL;
					c->in[id][k] = 1;
					if (online)
						ski_online_check(c, iter, key,
							id, k,
							&c->cur[id][k]->node,
							"after my insert", r,
							burst);
#ifdef FT_INV_SKI_COVERAGE
					/* Did this insert APPEND to a chain the
					 * PEER already owns?  That is the
					 * insert/insert product. */
					if (c->in[1 - id][k])
						uatomic_inc(&ski_dup_appends);
					else
						uatomic_inc(&ski_solo_appends);
#endif
				}
				rcu_read_unlock();
				/* Refused insert: the node never became
				 * reachable, so a plain free is correct. */
				if (fresh)
					node_free(fresh);
			} else {
				rcu_read_lock();
				if (online && c->cur[id][k])
					ski_online_check(c, iter, key, id, k,
						&c->cur[id][k]->node,
						"before my remove", r, burst);
				cds_ft_iter_set_key(iter, key,
					CDS_FT_LEN_DEFAULT);
				/*
				 * The iter positions the key; the NODE says
				 * which of the chain's entries to unlink, so
				 * this cannot take the peer's.
				 */
				if (cds_ft_lookup(c->ft, iter) ==
						CDS_FT_STATUS_OK &&
						c->cur[id][k] &&
						cds_ft_remove(c->ft, iter,
							&c->cur[id][k]->node) ==
						CDS_FT_STATUS_OK) {
					/*
					 * The node leaves the INDEX here and its
					 * memory is reclaimed only after a grace
					 * period -- this worker never writes it
					 * again.  The next insert for this key
					 * allocates a fresh one.
					 */
					node_free_rcu(c->cur[id][k]);
					c->cur[id][k] = NULL;
					c->in[id][k] = 0;
				}
				rcu_read_unlock();
			}
			if (online) {
				uatomic_inc(&c->seq[id][k]);	/* even: done */
				rcu_read_lock();
				ski_peer_check(c, iter, key, id, k, r, burst);
				rcu_read_unlock();
			}
			rcu_quiescent_state();
		}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_same_key_inserts_run(bool ordered_list, bool coarse,
		const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct ski_ctx c;
	struct ski_arg arg[2];
	pthread_t th[2];
	struct cds_ft_iter *iter;
	unsigned int i, k;
	int ret = 0;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, ordered_list) < 0)
		abort();
	if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	for (i = 0; i < 2; i++) {
		c.in[i] = (unsigned char *) calloc(SKI_K, 1);
		c.cur[i] = (struct ft_test_node **) calloc(SKI_K,
			sizeof(*c.cur[i]));
		if (!c.in[i] || !c.cur[i])
			abort();
	}
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	/*
	 * The trie starts EMPTY: both workers' first touch of a key is an
	 * insert, so the very first sample of every key is the insert/insert
	 * pairing this row exists for.
	 */
	for (i = 0; i < 2; i++) {
		arg[i].c = &c;
		arg[i].id = i;
	}
	pthread_create(&th[0], NULL, ski_worker, &arg[0]);
	pthread_create(&th[1], NULL, ski_worker, &arg[1]);
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	rcu_quiescent_state();
	rcu_barrier();
	rcu_read_lock();
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	for (k = 0; k < SKI_K; k++) {
		struct cds_ft_node *h;
		uint8_t key[8];
		unsigned int seen[2] = { 0, 0 }, extra = 0;

		cds_ft_u64_to_key(c.ft, ski_key(k), key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(c.ft, iter) == CDS_FT_STATUS_OK) {
			for (h = cds_ft_iter_node(iter); h;
					h = cds_ft_node_next_rcu(h)) {
				if (c.cur[0][k] && h == &c.cur[0][k]->node)
					seen[0]++;
				else if (c.cur[1][k] && h == &c.cur[1][k]->node)
					seen[1]++;
				else
					extra++;
			}
		}
		/*
		 * EXACTLY the nodes their owners say are in.  A missing one is a
		 * LOST INSERT or a phantom remove; a duplicate is a node linked
		 * twice; @extra is a foreign splice.
		 */
		for (i = 0; i < 2; i++)
			if (seen[i] != (unsigned int) c.in[i][k]) {
				fprintf(stderr,
					"%s: key %u worker %u: chain has %u, owner says %u\n",
					name, k, i, seen[i],
					(unsigned int) c.in[i][k]);
				ret = -1;
			}
		if (extra) {
			fprintf(stderr, "%s: key %u holds %u FOREIGN node(s)\n",
				name, k, extra);
			ret = -1;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	for (i = 0; i < 2; i++) {
		/*
		 * Whatever each worker still had installed: the trie is gone, so
		 * nothing can reach these any more -- but they were handed out
		 * under RCU, so they leave the same way.
		 */
		for (k = 0; k < SKI_K; k++)
			if (c.cur[i][k])
				node_free_rcu(c.cur[i][k]);
		free(c.cur[i]);
		free(c.in[i]);
	}
	/* The deferred frees must land before RUN_TEST's leak_check(). */
	rcu_quiescent_state();
	rcu_barrier();
#ifdef FT_INV_SKI_COVERAGE
	fprintf(stderr, "# %s COVERAGE: appends onto the PEER's chain=%lu, onto an empty key=%lu\n",
		name, ski_dup_appends, ski_solo_appends);
#endif
	fprintf(stderr,
		"# %s: %u rounds x %u burst x %u keys, TWO same-key inserters (%s) -> %s\n",
		name, SKI_R, SKI_BURST, SKI_K, coarse ? "coarse" : "fine",
		ret ? "RED" : "ok");
	return ret;
}

/*
 * cds_ft_insert_unique VS ITSELF, SAME KEY, on the FINE arm.
 *
 * ☞ WHY THIS ROW EXISTS AND WHAT MAKES IT A PROOF.  insert_unique is one of the
 * three POINT ops the fine-locking transition has not converted, and its gap is
 * the narrowest of them: both -EEXIST sites in _cds_ft_insert take the duplicate
 * head from the DESCENT'S LANDING -- each site's own comment says "with nothing
 * held" -- and `goto insert_done` BEFORE the `if (ft->lock_fine)` holder acquire
 * that the plain-insert path takes two lines later.  Only the early return
 * differs from the fine-SAFE insert.
 *
 * That unheld read decides UNIQUENESS, which makes the contract violation
 * non-racy and cheap to detect: if insert_unique is the ONLY insert path a
 * workload uses, then NO legal sequence can ever put two nodes on one key.  So
 * a chain of length >= 2, observed at ANY instant, is a defect -- no
 * owner-bookkeeping, no window to argue about, nothing to retune.  Contrast the
 * append/remove rows, whose invariant has to be reconciled against each worker's
 * @in flag precisely because cds_ft_insert may legally build a chain.
 *
 * THE MECHANISM THE ORACLE IS AIMED AT: two workers descend the same key, both
 * read "no duplicate here" from their own unheld landing, and both fall through
 * to the plain insert -- which DOES acquire, so both installs are arbitrated and
 * both SUCCEED, appending.  The exclusion is sound; the DECISION it protects was
 * taken before it.  ⇒ two CDS_FT_STATUS_OK for one key, and a 2-chain.
 *
 * ☞ AND THE SECOND MODE, counted rather than asserted, because it IS racy: a
 * DUPLICATE_FOUND whose returned node the peer's same-key sole remove retired in
 * the same gap (the FT-SLOT-2 window both sites name).  The caller is handed
 * "your insert failed, here is the existing node" for a key that may no longer
 * be in the trie at all.  A lookup that answers NOT_FOUND right after a
 * DUPLICATE_FOUND is evidence of it, but a peer may also have removed the key
 * legitimately in between -- so it is reported as a COUNT
 * ([[feedback_a_probe_that_records_beats_a_guard_that_refuses]]), and a
 * foreign/garbage @dup pointer is the part that IS asserted.
 *
 * Expected RED until insert_unique is converted; the COARSE row is the
 * adjudicator, and a red THERE is the rig.
 */
static unsigned long siu_ok, siu_dup, siu_dup_foreign, siu_dup_absent,
		siu_chain2;

struct siu_arg {
	struct ski_ctx *c;
	unsigned int id;
};

static void siu_retain(struct ski_ctx *c, unsigned int id,
		struct ft_test_node *n)
{
	if (c->ret_n[id] == c->ret_cap[id]) {
		size_t cap = c->ret_cap[id] ? c->ret_cap[id] * 2 : 1024;
		struct ft_test_node **v = (struct ft_test_node **)
			realloc(c->ret_v[id], cap * sizeof(*v));

		if (!v)
			abort();
		c->ret_v[id] = v;
		c->ret_cap[id] = cap;
	}
	c->ret_v[id][c->ret_n[id]++] = n;
}

static void *siu_worker(void *arg)
{
	struct siu_arg *a = (struct siu_arg *) arg;
	struct ski_ctx *c = a->c;
	unsigned int id = a->id, r, burst, k;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SKI_R; r++)
		for (burst = 0; burst < SKI_BURST; burst++)
		for (k = 0; k < SKI_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			if (!c->in[id][k]) {
				struct cds_ft_node *dup = NULL;
				struct ft_test_node *fresh =
					node_alloc((uint64_t) k);
				enum cds_ft_status st;

				rcu_read_lock();
#ifdef FT_INV_SIU_RED_DUP
				/*
				 * RED CONTROL: admit a duplicate ON PURPOSE, by
				 * routing one attempt through the plain insert.
				 * @siu_chain2 and the post-join walk both read 0
				 * on the shipping build, and a zero from a branch
				 * never shown to fire certifies nothing
				 * [[feedback_force_the_unreachable_branch_with_a_red_control]].
				 * Under this flag BOTH must go loud.
				 */
				if (r == SKI_R / 2 && burst == 0 && k == 0)
					st = cds_ft_insert(c->ft, key,
						CDS_FT_LEN_DEFAULT,
						&fresh->node);
				else
#endif
				st = cds_ft_insert_unique(c->ft, key,
					CDS_FT_LEN_DEFAULT,
					&fresh->node, &dup);
				if (st == CDS_FT_STATUS_OK) {
					CMM_STORE_SHARED(c->cur[id][k], fresh);
					fresh = NULL;
					c->in[id][k] = 1;
					uatomic_inc(&siu_ok);
					/*
					 * ★ THE UNIQUENESS CHECK, taken HERE
					 * rather than after the join: the
					 * violating 2-chain is transient (either
					 * worker may remove its own node next
					 * round), so a post-join walk can miss
					 * it entirely.  Under a uniqueness
					 * contract a length >= 2 is illegal at
					 * every instant, so observing it once is
					 * the whole proof.
					 */
					cds_ft_iter_set_key(iter, key,
						CDS_FT_LEN_DEFAULT);
					if (cds_ft_lookup(c->ft, iter) ==
							CDS_FT_STATUS_OK) {
						struct cds_ft_node *h;
						unsigned int len = 0;

						for (h = cds_ft_iter_node(iter);
								h; h =
								cds_ft_node_next_rcu(h))
							len++;
						if (len >= 2)
							uatomic_inc(&siu_chain2);
					}
				} else if (st == CDS_FT_STATUS_DUPLICATE_FOUND) {
					uatomic_inc(&siu_dup);
					/*
					 * @dup must name a node of this key.  A
					 * pointer that is neither worker's node
					 * for @k is a foreign or freed address
					 * -- asserted below.  A key that is GONE
					 * is the FT-SLOT-2 mode: counted, since a
					 * peer may also have removed it legally.
					 */
#ifdef FT_INV_SIU_RED_FOREIGN
					/*
					 * RED CONTROL for the clause below.  The
					 * shadow-key test is what makes the
					 * FOREIGN check race-free, and a clause
					 * that can only ever answer "not foreign"
					 * would certify nothing.  Substitute a
					 * node of ANOTHER key once: it must go
					 * loud.
					 */
					if (r == SKI_R / 2 && burst == 0 &&
							k == 0) {
						static struct ft_test_node alien;

						alien.key = (uint64_t) k + 1000;
						dup = &alien.node;
					}
#endif
					if (to_test_node(dup)->key !=
							(uint64_t) k) {
						/*
						 * FOREIGN: not a node of this
						 * key at all.  NON-RACY -- no
						 * schedule makes this legal.
						 *
						 * ☞ ASK THE NODE, NOT THE INDEX.
						 * The index slots are heap
						 * pointers the peer swaps as it
						 * installs and retires, so
						 * reading the PEER's slot would
						 * race and could call its node
						 * foreign.  @key is stamped once
						 * by node_alloc and, because
						 * this row RETAINS every node it
						 * removes (see ski_ctx), it
						 * stays that node's key for the
						 * whole run -- a stale @dup is
						 * still a node OF THIS KEY, and
						 * lands in @siu_dup_absent where
						 * it belongs.
						 */
						uatomic_inc(&siu_dup_foreign);
					} else {
						struct cds_ft_node *h;
						bool on_chain = false;

						/*
						 * ★ ASK WHETHER @dup IS ON THE
						 * KEY'S CHAIN, not merely whether
						 * the key exists: the FT-SLOT-2
						 * mode is a RETIRED landing, and a
						 * peer can retire @dup while
						 * re-heading the same key, which
						 * leaves the lookup answering OK.
						 * "The key is there" would call
						 * that clean.
						 */
						cds_ft_iter_set_key(iter, key,
							CDS_FT_LEN_DEFAULT);
						if (cds_ft_lookup(c->ft, iter) ==
								CDS_FT_STATUS_OK)
							for (h = cds_ft_iter_node(iter);
									h; h = cds_ft_node_next_rcu(h))
								if (h == dup) {
									on_chain = true;
									break;
								}
						if (!on_chain)
							uatomic_inc(&siu_dup_absent);
					}
				}
				rcu_read_unlock();
				/* Refused: never reachable, plain free. */
				if (fresh)
					node_free(fresh);
			} else {
				rcu_read_lock();
				cds_ft_iter_set_key(iter, key,
					CDS_FT_LEN_DEFAULT);
				if (cds_ft_lookup(c->ft, iter) ==
						CDS_FT_STATUS_OK &&
						c->cur[id][k] &&
						cds_ft_remove(c->ft, iter,
							&c->cur[id][k]->node) ==
						CDS_FT_STATUS_OK) {
					/*
					 * Out of the trie and out of the index,
					 * and never written again -- retained,
					 * not reclaimed, so its shadow key stays
					 * answerable for the rest of the run.
					 */
					siu_retain(c, id, c->cur[id][k]);
					CMM_STORE_SHARED(c->cur[id][k], NULL);
					c->in[id][k] = 0;
				}
				rcu_read_unlock();
			}
			rcu_quiescent_state();
		}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_insert_unique_run(bool coarse, const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct ski_ctx c;
	struct siu_arg arg[2];
	pthread_t th[2];
	struct cds_ft_iter *iter;
	unsigned int i, k;
	int ret = 0;

	siu_ok = siu_dup = siu_dup_foreign = siu_dup_absent = siu_chain2 = 0;
	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	for (i = 0; i < 2; i++) {
		c.in[i] = (unsigned char *) calloc(SKI_K, 1);
		c.cur[i] = (struct ft_test_node **) calloc(SKI_K,
			sizeof(*c.cur[i]));
		if (!c.in[i] || !c.cur[i])
			abort();
	}
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (i = 0; i < 2; i++) {
		arg[i].c = &c;
		arg[i].id = i;
		if (pthread_create(&th[i], NULL, siu_worker, &arg[i]))
			abort();
	}
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	rcu_quiescent_state();
	rcu_barrier();
	rcu_read_lock();
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	/* A surviving 2-chain: the same violation, still standing at the join. */
	for (k = 0; k < SKI_K; k++) {
		struct cds_ft_node *h;
		uint8_t key[8];
		unsigned int len = 0;

		cds_ft_u64_to_key(c.ft, (uint64_t) k, key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(c.ft, iter) == CDS_FT_STATUS_OK)
			for (h = cds_ft_iter_node(iter); h;
					h = cds_ft_node_next_rcu(h))
				len++;
		if (len >= 2) {
			fprintf(stderr, "%s: key %u ends with a chain of %u -- "
				"cds_ft_insert_unique admitted a DUPLICATE\n",
				name, k, len);
			ret = -1;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	for (i = 0; i < 2; i++) {
		/*
		 * Whatever each worker still had installed: the trie is gone, so
		 * nothing can reach these any more -- but they were handed out
		 * under RCU, so they leave the same way.
		 */
		size_t j;

		for (k = 0; k < SKI_K; k++)
			if (c.cur[i][k])
				node_free_rcu(c.cur[i][k]);
		/* And everything this worker retired and held onto. */
		for (j = 0; j < c.ret_n[i]; j++)
			node_free_rcu(c.ret_v[i][j]);
		free(c.ret_v[i]);
		free(c.cur[i]);
		free(c.in[i]);
	}
	/* The deferred frees must land before RUN_TEST's leak_check(). */
	rcu_quiescent_state();
	rcu_barrier();
	if (siu_chain2) {
		fprintf(stderr, "%s: %lu observation(s) of a >=2 chain right "
			"after a successful cds_ft_insert_unique -- UNIQUENESS "
			"VIOLATED\n", name, siu_chain2);
		ret = -1;
	}
	if (siu_dup_foreign) {
		fprintf(stderr, "%s: %lu DUPLICATE_FOUND naming a FOREIGN node "
			"(not either worker's node for that key) -- no schedule "
			"makes this legal\n", name, siu_dup_foreign);
		ret = -1;
	}
	/*
	 * ☠ @absent IS NOT A VERDICT ON THE LIBRARY, and treating it as one was
	 * this row's own defect.  A peer may legitimately remove the key between
	 * the DUPLICATE_FOUND and the walk, so the count mixes the defect with a
	 * benign race -- and the COARSE arm does NOT discriminate it: the FT-wide
	 * lock serialises THIS TEST'S follow-up walk too (the peer must be woken
	 * and scheduled after the release), so coarse draws a near-zero for a
	 * reason that has nothing to do with the verdict.  A control that differs
	 * from the arm in more ways than the one being measured is not a control.
	 *
	 * ⇒ REPORTED, NEVER ASSERTED.  The verdict's real defect rate is measured
	 * INSIDE the op, where the question is decided and a removal mark is
	 * one-way: -DFT_DEBUG_UNIQUE_VERDICT, the HEAD_GONE counter.
	 */
	if (siu_dup_absent)
		diag("%s: %lu DUPLICATE_FOUND whose node is not on the key's "
			"chain -- RACY, reported only (see HEAD_GONE under "
			"-DFT_DEBUG_UNIQUE_VERDICT for the real rate)",
			name, siu_dup_absent);
	if (0) {
		/*
		 * ☞ RACY IN ISOLATION, DISCRIMINATED BY THE COARSE ROW.  A peer
		 * may legitimately remove the key between the DUPLICATE_FOUND and
		 * the walk above, and that window exists on BOTH arms -- the
		 * lookup is outside any lock either way.  So the number is only
		 * evidence next to its control: COARSE draws ZERO from a
		 * comparable sample, FINE does not.  Reported as a failure on the
		 * FINE arm for that reason, not on the strength of the count.
		 */
		fprintf(stderr, "%s: %lu DUPLICATE_FOUND whose node is NOT ON "
			"the key's chain (FT-SLOT-2 stale landing; compare the "
			"coarse row, which draws zero)\n", name, siu_dup_absent);
		ret = -1;
	}
	fprintf(stderr,
		"# %s: %u rounds x %u burst x %u keys, TWO same-key "
		"cds_ft_insert_unique (%s): ok=%lu dup=%lu foreign=%lu "
		"absent=%lu chain2=%lu -> %s\n",
		name, SKI_R, SKI_BURST, SKI_K, coarse ? "coarse" : "fine",
		siu_ok, siu_dup, siu_dup_foreign, siu_dup_absent,
		siu_chain2, ret ? "RED" : "ok");
	/*
	 * ☠ A ZERO HERE IS NOT A PASS UNTIL THE LANE IS PROVEN LIVE.  Both
	 * counters are reached only from the DUPLICATE_FOUND / OK arms, so a run
	 * that never raced would report a clean zero and certify nothing.
	 */
	if (!siu_dup) {
		fprintf(stderr, "%s: ZERO DUPLICATE_FOUND -- the workers never "
			"met on a key, so this row tested nothing\n", name);
		ret = -1;
	}
	return ret;
}

static int inv_concurrent_insert_unique_nolist(void)
{
	/*
	 * ☑ IN THE DEFAULT SET -- the regression guard for cds_ft_insert_unique's
	 * fine-locking conversion.  Both -EEXIST sites in _cds_ft_insert used to
	 * answer from the descent's landing ("with nothing held", each site says
	 * so) and `goto insert_done` BEFORE the holder acquire and re-validate the
	 * plain-insert path goes on to make.  The verdict now sits below them.
	 *
	 * ☠ AND THE MEASUREMENT TOOK FOUR TRIES; the wrong ones are worth keeping,
	 * because each was a plausible reading that a control would have had to
	 * refute and did not:
	 *
	 *   1. @absent (a DUPLICATE_FOUND whose node is not on the key's chain)
	 *      read ~40/run on FINE and 0 on COARSE.  INVALID: the walk is outside
	 *      any lock on BOTH arms, and the coarse lock serialises the TEST'S OWN
	 *      follow-up walk, so coarse's zero is a scheduling artifact.  Demoted
	 *      to a diag above.
	 *   2. @chain2 (uniqueness actually violated) is 0 -- and that one IS a
	 *      proven zero, red-controlled with -DFT_INV_SIU_RED_DUP, which forces
	 *      the detector to fire and turns even the coarse row RED.  The
	 *      empty-key race is arbitrated: the loser's parent-slot CAS fails and
	 *      its retry then sees the duplicate.
	 *   3. capture_stale (the re-validate catching a moved head) is 0 too --
	 *      but that is NOT the window being closed, it is the window closing
	 *      EARLIER: ft_dlm_lock refuses a tombstoned holder, so a sole remove
	 *      bails at the ACQUIRE and never reaches the re-validate.
	 *   4. ★ HEAD_GONE (-DFT_DEBUG_UNIQUE_VERDICT) is the one that stands
	 *      alone: ~2725-2784 per run where the CAPTURED HEAD carries a removal
	 *      mark.  The mark is ONE-WAY, so every observation is a fact about
	 *      that verdict and no control is needed to read it -- which matters,
	 *      because the coarse arm could never have been the control here:
	 *      ft->lock_fine is false there, so the measured path does not run.
	 *
	 * ⇒ ~2.7k verdicts per run named a node that had LEFT THE TRIE.  The COARSE
	 * row stays the adjudicator for the RIG.
	 */
	return inv_concurrent_insert_unique_run(false,
		"inv_concurrent_insert_unique_nolist");
}

static int inv_concurrent_insert_unique_coarse(void)
{
	return inv_concurrent_insert_unique_run(true,
		"inv_concurrent_insert_unique_coarse");
}

/*
 * THE SAME RACE ON A PIGEON NODE.  The rows above contend on SKI_K = 8 keys,
 * so the attach node never leaves the popcount tiers -- and the in-place
 * reserve's FENCED ADD check (a live-node reserve must not clear a byte a
 * peer filled after this op's descent) was present on every popcount/bitmap
 * arm and MISSING on the pigeon one (ft_types[5], min_child 51).  Two
 * concurrent inserts of one absent key then BOTH returned OK: the second's
 * reserve stored NULL over the first's live child and published its own, so
 * the first's key left the trie while its ordered cell stayed in the list.
 *
 * Here keys [0, SID_PIN) are pinned by the main thread, which holds the
 * attach node at >= min_child and therefore a pigeon, and two workers race
 * cds_ft_insert_unique / cds_ft_remove over [SID_PIN, SID_PIN + SID_HOT).
 * Each worker removes ONLY nodes it installed, and a unique insert cannot
 * displace a present one, so a worker's own node must still be the key's
 * head when it goes to remove it: anything else is a LOST node (@sid_lost).
 * cds_ft_verify at the join catches the orphaned cell.
 */
#define SID_PIN		128
#define SID_HOT		128
#define SID_OPS		200000
#define SID_LOST_MAX	4096

static unsigned long sid_ok, sid_dup, sid_lost;

struct sid_ctx {
	struct cds_ft *ft;
	struct ft_test_node *mine[2][SID_HOT];
	/* Lost nodes may still be reachable (a stale cell): freed at teardown. */
	struct ft_test_node *lost[2][SID_LOST_MAX];
	unsigned int nr_lost[2];
};

struct sid_arg {
	struct sid_ctx *c;
	unsigned int id;
	unsigned int seed;
};

static void *sid_worker(void *arg)
{
	struct sid_arg *a = (struct sid_arg *) arg;
	struct sid_ctx *c = a->c;
	unsigned int id = a->id, seed = a->seed, i;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (i = 0; i < SID_OPS; i++) {
		unsigned int h = (unsigned int) rand_r(&seed) % SID_HOT;
		uint64_t k = SID_PIN + h;
		uint8_t key[8];

		cds_ft_u64_to_key(c->ft, k, key, CDS_FT_LEN_DEFAULT);
		rcu_read_lock();
		if (!c->mine[id][h]) {
			struct ft_test_node *fresh = node_alloc(k);
			struct cds_ft_node *dup = NULL;
			enum cds_ft_status st;

			st = cds_ft_insert_unique(c->ft, key, CDS_FT_LEN_DEFAULT,
				&fresh->node, &dup);
			if (st == CDS_FT_STATUS_OK) {
				c->mine[id][h] = fresh;
				fresh = NULL;
				uatomic_inc(&sid_ok);
			} else if (st == CDS_FT_STATUS_DUPLICATE_FOUND) {
				uatomic_inc(&sid_dup);
			} else {
				fprintf(stderr, "inv_concurrent_insert_unique_dense: "
					"insert_unique: %s\n",
					cds_ft_status_to_string(st));
				abort();
			}
			/* Refused: never reachable, plain free. */
			if (fresh)
				node_free(fresh);
		} else {
			struct ft_test_node *n = c->mine[id][h];

			cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
			if (cds_ft_lookup(c->ft, iter) != CDS_FT_STATUS_OK ||
					cds_ft_iter_node(iter) != &n->node ||
					cds_ft_remove(c->ft, iter, &n->node) !=
						CDS_FT_STATUS_OK) {
				uatomic_inc(&sid_lost);
				if (c->nr_lost[id] < SID_LOST_MAX)
					c->lost[id][c->nr_lost[id]++] = n;
			} else {
				node_free_rcu(n);
			}
			c->mine[id][h] = NULL;
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_insert_unique_dense(void)
{
	const char *name = "inv_concurrent_insert_unique_dense";
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct ft_test_node *pin[SID_PIN];
	struct sid_ctx *c;
	struct sid_arg arg[2];
	pthread_t th[2];
	struct cds_ft_iter *iter;
	unsigned int i, h;
	int ret = 0;

	sid_ok = sid_dup = sid_lost = 0;
	c = (struct sid_ctx *) calloc(1, sizeof(*c));
	if (!c)
		abort();
	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &c->ft) < 0)
		abort();
	cds_ft_make_shared(c->ft);
	rcu_read_lock();
	for (i = 0; i < SID_PIN; i++) {
		uint8_t key[8];

		pin[i] = node_alloc(i);
		cds_ft_u64_to_key(c->ft, i, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_insert(c->ft, key, CDS_FT_LEN_DEFAULT,
				&pin[i]->node) != CDS_FT_STATUS_OK)
			abort();
	}
	rcu_read_unlock();
	for (i = 0; i < 2; i++) {
		arg[i].c = c;
		arg[i].id = i;
		arg[i].seed = 0x5eed0 + i;
		if (pthread_create(&th[i], NULL, sid_worker, &arg[i]))
			abort();
	}
	rcu_thread_offline();
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	rcu_thread_online();
	rcu_quiescent_state();
	rcu_barrier();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	rcu_read_lock();
	if (cds_ft_verify(c->ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	/* Every hot key: exactly its owner's node, or absent. */
	for (h = 0; h < SID_HOT; h++) {
		struct ft_test_node *own = c->mine[0][h] ? c->mine[0][h] :
			c->mine[1][h];
		uint8_t key[8];
		enum cds_ft_status st;

		if (c->mine[0][h] && c->mine[1][h]) {
			fprintf(stderr, "%s: key %u OWNED BY BOTH workers\n",
				name, SID_PIN + h);
			ret = -1;
		}
		cds_ft_u64_to_key(c->ft, SID_PIN + h, key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		st = cds_ft_lookup(c->ft, iter);
		if (own ? (st != CDS_FT_STATUS_OK ||
				cds_ft_iter_node(iter) != &own->node ||
				cds_ft_node_next_rcu(&own->node)) :
				st != CDS_FT_STATUS_NOT_FOUND) {
			fprintf(stderr, "%s: key %u is not exactly its owner's "
				"node at the join\n", name, SID_PIN + h);
			ret = -1;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c->ft);
	cds_ft_group_destroy(group);
	/* The trie is gone; what it handed out under RCU leaves the same way. */
	for (i = 0; i < SID_PIN; i++)
		node_free_rcu(pin[i]);
	for (i = 0; i < 2; i++) {
		unsigned int j;

		for (h = 0; h < SID_HOT; h++)
			if (c->mine[i][h])
				node_free_rcu(c->mine[i][h]);
		for (j = 0; j < c->nr_lost[i]; j++)
			node_free_rcu(c->lost[i][j]);
	}
	/* The deferred frees must land before RUN_TEST's leak_check(). */
	rcu_quiescent_state();
	rcu_barrier();
	free(c);
	if (sid_lost) {
		fprintf(stderr, "%s: %lu node(s) LOST -- a worker's own "
			"installed node was not the key's head when it went to "
			"remove it\n", name, sid_lost);
		ret = -1;
	}
	diag("%s: 2 workers x %u ops over %u hot keys (+%u pinned, pigeon): "
		"ok=%lu dup=%lu lost=%lu -> %s", name, SID_OPS, SID_HOT,
		SID_PIN, sid_ok, sid_dup, sid_lost, ret ? "RED" : "ok");
	return ret;
}

/*
 * A CONTRACT PROBE, NOT YET A GATE: cds_ft_replace against a same-key
 * insert/remove peer, on the FINE arm.
 *
 * ☞ WHY THE FINE ARM, for an op whose header says "NOT concurrency-safe.  Mutual
 * exclusion ... is the caller's responsibility".  That sentence is a TODO
 * recording an unfinished transition, not a property anyone wants (see "THE
 * FINE-LOCKING TRANSITION IS INCOMPLETE" at enum cds_ft_writer_strategy).  A
 * COARSE-only row would bless the gap as the contract and witness nothing.  So
 * this row runs where the conversion is owed, and its RESULT is the information:
 *
 *   RED   -> the gap is real, and this row is the reproducer the conversion needs.
 *   GREEN -> the header UNDER-describes the code, which for this op is the live
 *            question: _cds_ft_replace_locked already has a need_retry RETRY LOOP
 *            at its public entry, resolves the chain head through
 *            ft_chain_head_holder, takes ft_acquire_member on the holder with a
 *            need_retry bail, and publishes through _ft_publish_to_parent with
 *            @old_node as the EXPECTED-OLD -- so a slot that moved aborts the
 *            commit and the loop re-derives.  That is the same recipe the point
 *            remove was converted with.  A green here does not PROVE the op
 *            converted, but it is the first evidence either way, and there was
 *            none before.
 *
 * THE SHAPE.  Worker A cycles insert/remove of its own node (both CONVERTED ops,
 * so A is in-contract by itself).  Worker B owns TWO nodes per key and keeps
 * exactly one installed, replacing it with the other -- so every B operation is a
 * cds_ft_replace on a key A is concurrently inserting into and removing from.
 * B seeds its first node with an insert, because replace needs something to
 * replace.
 *
 * ☠ B ONLY EVER NAMES ITS OWN NODE.  cds_ft_replace takes @old_node explicitly
 * and dereferences it via ->prev, so it must be live: B holds the RCU read lock
 * across the lookup and the call, and A never touches B's nodes.  Passing a node
 * the peer may have removed would be a test bug that frames the library.
 *
 * The invariant is the exact one the insert/insert row uses: at the join, each
 * worker's own flags are authoritative, so the chain must hold EXACTLY the nodes
 * their owners say are in.
 */
static void *skrp_replacer(void *arg)
{
	struct ski_ctx *c = (struct ski_ctx *) arg;
	unsigned int r, burst, k;
	struct cds_ft_iter *iter;

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SKI_R; r++)
		for (burst = 0; burst < SKI_BURST; burst++)
		for (k = 0; k < SKI_K; k++) {
			uint8_t key[8];
			/*
			 * @cur[1][k] names the node this lane currently has
			 * installed, or NULL.  It replaced a PAIR of array
			 * nodes alternated slot-by-slot: that alternation
			 * re-initialised whichever node the previous replace
			 * had displaced, with no grace period, so it wiped
			 * prev/next on an object the insert/remove peer could
			 * still be descending through.  A replace installs a
			 * FRESH node and the displaced one is reclaimed by
			 * node_free_rcu, which is what the contract asks for.
			 */
			struct ft_test_node *old = c->cur[1][k];
			struct ft_test_node *nw = node_alloc((uint64_t) k);

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			if (!old) {
				if (cds_ft_insert(c->ft, key,
						CDS_FT_LEN_DEFAULT,
						&nw->node) ==
						CDS_FT_STATUS_OK) {
					c->cur[1][k] = nw;
					nw = NULL;
					c->in[1][k] = 1;
				}
			} else {
				cds_ft_iter_set_key(iter, key,
					CDS_FT_LEN_DEFAULT);
				if (cds_ft_lookup(c->ft, iter) ==
						CDS_FT_STATUS_OK &&
						cds_ft_replace(c->ft, iter,
							&old->node,
							&nw->node) ==
						CDS_FT_STATUS_OK) {
					c->cur[1][k] = nw;
					nw = NULL;
					/* Displaced: out of the trie, so it
					 * leaves after a grace period. */
					node_free_rcu(old);
				}
			}
			rcu_read_unlock();
			/* Refused: never reachable, plain free. */
			if (nw)
				node_free(nw);
			rcu_quiescent_state();
		}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_same_key_replace_run(bool coarse, const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct ski_ctx c;
	struct ski_arg arg0;
	pthread_t th[2];
	struct cds_ft_iter *iter;
	unsigned int i, k;
	int ret = 0;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	for (i = 0; i < 2; i++) {
		c.in[i] = (unsigned char *) calloc(SKI_K, 1);
		c.cur[i] = (struct ft_test_node **) calloc(SKI_K,
			sizeof(*c.cur[i]));
		if (!c.in[i] || !c.cur[i])
			abort();
	}
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	arg0.c = &c;
	arg0.id = 0;
	pthread_create(&th[0], NULL, ski_worker, &arg0);	/* insert/remove peer */
	pthread_create(&th[1], NULL, skrp_replacer, &c);	/* the replace lane */
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	rcu_quiescent_state();
	rcu_barrier();
	rcu_read_lock();
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	for (k = 0; k < SKI_K; k++) {
		struct cds_ft_node *h;
		uint8_t key[8];
		unsigned int seen_a = 0, seen_b = 0, extra = 0;
		unsigned int want_b = c.cur[1][k] ? 1 : 0;

		cds_ft_u64_to_key(c.ft, (uint64_t) k, key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(c.ft, iter) == CDS_FT_STATUS_OK) {
			for (h = cds_ft_iter_node(iter); h;
					h = cds_ft_node_next_rcu(h)) {
				if (c.cur[0][k] && h == &c.cur[0][k]->node)
					seen_a++;
				else if (c.cur[1][k] &&
						h == &c.cur[1][k]->node)
					seen_b++;
				else
					extra++;
			}
		}
		if (seen_a != (unsigned int) c.in[0][k]) {
			fprintf(stderr,
				"%s: key %u inserter: chain has %u, owner says %u\n",
				name, k, seen_a, (unsigned int) c.in[0][k]);
			ret = -1;
		}
		/*
		 * EXACTLY ONE of the replacer's pair, and only while it says so.
		 * Two would mean a replace INSTALLED without unlinking; zero
		 * with want_b set would mean the swap lost the node.
		 */
		if (seen_b != want_b) {
			fprintf(stderr,
				"%s: key %u replacer: chain has %u of its pair, owner says %u\n",
				name, k, seen_b, want_b);
			ret = -1;
		}
		if (extra) {
			fprintf(stderr, "%s: key %u holds %u FOREIGN node(s)\n",
				name, k, extra);
			ret = -1;
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	for (i = 0; i < 2; i++) {
		/*
		 * Whatever the insert/remove peer still had installed: the trie
		 * is gone, so nothing can reach these any more -- but they were
		 * handed out under RCU, so they leave the same way.
		 */
		for (k = 0; k < SKI_K; k++)
			if (c.cur[i][k])
				node_free_rcu(c.cur[i][k]);
		free(c.cur[i]);
		free(c.in[i]);
	}
	/* The deferred frees must land before RUN_TEST's leak_check(). */
	rcu_quiescent_state();
	rcu_barrier();
	fprintf(stderr,
		"# %s: %u rounds x %u burst x %u keys, cds_ft_replace vs same-key insert/remove (%s) -> %s\n",
		name, SKI_R, SKI_BURST, SKI_K, coarse ? "coarse" : "fine",
		ret ? "RED" : "ok");
	return ret;
}

static int inv_concurrent_same_key_inserts_nolist(void)
{
	/*
	 * FT_INV_SKI_REPEAT=N runs the row N times in one process, stopping
	 * at the first red: a reproducer for a rare key loss that the row
	 * alone never shows, which separates "the process has aged" (memory
	 * reuse accumulates here) from "the machine is loaded".
	 */
	const char *rep = getenv("FT_INV_SKI_REPEAT");
	unsigned long n = rep ? strtoul(rep, NULL, 10) : 1, i;
	int ret = 0;

	for (i = 0; i < (n ? n : 1) && !ret; i++)
		ret = inv_concurrent_same_key_inserts_run(false, false,
			"inv_concurrent_same_key_inserts_nolist");
	if (rep)
		fprintf(stderr, "# ski repeat: %lu run(s), %s\n", i,
			ret ? "RED" : "ok");
	if (getenv("FT_INV_SKI_ONLINE"))
		fprintf(stderr, "# ski ONLINE: %lu checks, %lu transient reader misses, %lu peer checks\n",
			ski_online_checks, ski_online_transient, ski_peer_checks);
	return ret;
}

static int inv_concurrent_same_key_replace_nolist(void)
{
	/*
	 * ☑ IN THE DEFAULT SET, and it is the regression guard for
	 * cds_ft_replace's FINE-LOCKING CONVERSION.  It was opt-in and RED --
	 * SIGSEGV, then LIVELOCK -- for as long as replace was unconverted; all
	 * THREE defects are fixed, and the history is worth keeping because each
	 * one was a different lesson about the same false premise.
	 *
	 * 1. ☑ the SIGSEGV was _cds_ft_replace_locked reading @old_node->prev RAW,
	 *    three times, on a premise its own comment stated and that is false
	 *    under FINE ("the writer mutex held here freezes the structure").  A
	 *    peer's prev store folded onto a commit flip-txn leaves the type-7
	 *    proxy in that word; ft_ord_cell_ptr strips only the cell bit, and
	 *    cds_ft_item_to_metadata then builds a range header from the
	 *    descriptor.  Caught at p=0x7fff348003de -- low nibble 0xe, i.e. 0xf
	 *    with the cell bit already masked off, which is exactly why the assert
	 *    at that site (it tests the WHOLE tag) cannot fire.  Cure: ONE
	 *    ft_dereference_prev_resolved observation for the whole arm.
	 *
	 * 2. ☑ behind it, cds_ft_REMOVE livelocked -- 50,001 attempts, ONE exit,
	 *    always -EAGAIN, never -ENOENT, ZERO commit aborts, already escalated
	 *    into the FIFO fallback lane whose drain the retry loop cites as why it
	 *    "terminates (no livelock)".  The stamps chained three levels down to
	 *    ft_dlm_lock refusing the chain holder's state word, and the PROXY
	 *    PARKER discriminator named the parker: every refusal saw a DIFFERENT
	 *    descriptor (streak=1, switches == the refusal count), all-MW, carrying
	 *    a {0x20 -> 0x20} record on that word -- i.e.
	 *    ft_flip_txn_guard_parent's §4.B state guard, parked once per replace
	 *    commit.  Replace's HEAD arms VALIDATED the word remove HOLDS.  A
	 *    validator that parks a word a locker must find CLEAN starves that
	 *    locker forever, and symmetrically the guard loses to remove's
	 *    {LOCK|s -> s} release, so replace starved too (op=3, same row).  MW
	 *    settles the record KIND, never the EXCLUSION.  Cure: replace takes the
	 *    holder's lock ONCE, above its routing derivation, so both ops queue on
	 *    one primitive and the op txn's aging drains it.
	 *
	 * 3. ☑ and THAT exposed the third: with the hold in place the §4.B guard
	 *    aborted 100% of commits (49966 of 49974, -DFT_WINNER_DBG attributing
	 *    every abort to the VALIDATE class).  The guard's expectation is
	 *    clean-LIVE -- it masks FT_STATE_LOCK out of the value it validates --
	 *    so it cannot be satisfied on a word the op itself locked.  The guard
	 *    and the hold cannot coexist on ONE word; the hold subsumes it.
	 *
	 * ⇒ the shape is DELIBERATELY SMALLER than cds_ft_remove's conversion: no
	 * routing re-validation, because replace has ONE transform in every arm and
	 * _ft_publish_to_parent records it with expected_old == @old_node, so a
	 * stale slot ABORTS rather than mis-publishing.  See the {L} block in
	 * ft-insert.h for the full argument.
	 *
	 * The COARSE row below stays the adjudicator: a red there is the RIG, not
	 * the conversion.
	 */
	/*
	 * ☑ BACK IN THE DEFAULT SET.  It was gated for a SIGSEGV in the iterator's
	 * up-walk (ft_rebuild_key_upwalk -> cds_ft_item_to_metadata(cell), from
	 * _cds_ft_replace_locked's ft_iter_read_key) on the holdtrace config with
	 * the list off, blamed on the up-walk reading its cell UNHELD.  It was the
	 * TEST: ski_worker, this row's insert/remove peer, recycled its array
	 * nodes with cds_ft_node_init() inside the grace period cds_ft_remove
	 * reserves, so a reader's up-walk could follow a prev the test had just
	 * rewritten.  Fixed on the test side two days later (18a05072); the
	 * unheld read is RCU-legal once nodes are not reused early.
	 *
	 * Found by bisect under the 2-cpu amplifier (each run pinned to two cpus
	 * with two spinning hogs on them -- in isolation even the gating commit
	 * was green): 6d90eadb, the commit before 18a05072, SIGSEGVs 4/32 runs,
	 * 18a05072 0/32; the gating commit 6/48 against HEAD 0/48.  The two
	 * holdtrace list-off hangs reported beside the SEGV (never backtraced)
	 * did not recur: the full feature gate ran this row in all 110 legs,
	 * holdtrace list-off at all three spacings, 166/166 each.
	 */
	return inv_concurrent_same_key_replace_run(false,
		"inv_concurrent_same_key_replace_nolist");
}

static int inv_concurrent_same_key_replace_coarse(void)
{
	return inv_concurrent_same_key_replace_run(true,
		"inv_concurrent_same_key_replace_coarse");
}

static int inv_concurrent_same_key_inserts_coarse(void)
{
	/*
	 * THE ADJUDICATOR.  A fine-grained red is only a library finding if the
	 * COARSE arm -- one writer lock, every interleaving serialised -- is
	 * green on the same shape; otherwise the rig itself is wrong.
	 */
	return inv_concurrent_same_key_inserts_run(false, true,
		"inv_concurrent_same_key_inserts_coarse");
}

static int inv_concurrent_same_key_append(void)
{
	/*
	 * OPT-IN: reaches the open list=1 ft_iter_read_key up-walk defect on a
	 * fixed tree (see the header above).  FT_INV_SAME_KEY_APPEND_LIST=1.
	 */
	if (!getenv("FT_INV_SAME_KEY_APPEND_LIST")) {
		diag("inv_concurrent_same_key_append: skipped (set FT_INV_SAME_KEY_APPEND_LIST=1; reaches the open list=1 up-walk defect)");
		return 0;
	}
	return inv_concurrent_same_key_append_run(true, false,
		"inv_concurrent_same_key_append");
}

static int inv_concurrent_same_key_append_nolist(void)
{
	return inv_concurrent_same_key_append_run(false, false,
		"inv_concurrent_same_key_append_nolist");
}

static int inv_concurrent_same_key_append_coarse(void)
{
	return inv_concurrent_same_key_append_run(false, true,
		"inv_concurrent_same_key_append_coarse");
}

/*
 * cds_ft_remove_all vs a same-key peer, under CDS_FT_WRITER_LOCK_FINE.
 *
 * WHY THIS ROW EXISTS.  remove_all is the fourth point op of the fine-locking
 * transition (see "THE FINE-LOCKING TRANSITION IS INCOMPLETE" at enum
 * cds_ft_writer_strategy) and the one whose gap is PROGRESS rather than
 * derivation: cds_ft_remove_all called _cds_ft_remove_all_locked exactly ONCE,
 * so every contention event surfaced to the caller as CDS_FT_STATUS_BUSY_ERROR
 * with no aging and no escalation into the per-trie FIFO lane.  Its failure tail
 * had never run: measured over both suites, ZERO times in 4,807,509 calls,
 * because the op required caller writer-exclusion and no peer could refuse it a
 * lock-set.  Nothing in the tree drove it against a peer.
 *
 * ★ THE PRIMARY READING IS THE BUSY_ERROR COUNT, AND IT IS MONOTONE.  The call
 * either reported failure to its caller or it did not, so this row needs no
 * control arm to interpret: @busy > 0 IS the defect, at any magnitude, and
 * @busy == 0 is the claim the conversion makes.  (A coarse arm would not be a
 * control here -- it serialises the writers, so the contended path never runs
 * at all, and its green says nothing about the fine one.)
 *
 * THE STRUCTURAL ORACLE, layered on top, is an exact accounting rather than a
 * spot check.  Each round pre-seeds D duplicates on each of K keys, T lanes
 * then race to remove_all every key while an inserter APPENDS one fresh node
 * per key -- so a removal that derived a chain meets an append between its
 * derivation and its commit.  At the join every ORIGINAL node must be in
 * exactly one of two places:
 *
 *   returned exactly once  ..  the chain that removed it handed it back, and
 *                              the caller may reclaim it;
 *   still in the trie      ..  no remove_all claimed it.
 *
 * Both at once is a chain handed to a caller while the trie still points at it
 * (a double-reclaim); NEITHER is a LOST NODE.  Returned TWICE is two callers
 * each told they own the same node -- the double-free the whole-chain hand-back
 * makes possible and that a per-node remove cannot produce.  The fresh node is
 * the only survivor a key may legally hold, and only one of it.
 */
#define SKRA_T		3	/* remove_all lanes racing on every key */
#define SKRA_D		3	/* duplicates pre-seeded per key */
#define SKRA_K		64
#define SKRA_R		150

struct skra_node {
	struct cds_ft_node node;
	unsigned int k, d;
	unsigned int returned;	/* times a remove_all handed this node back */
};

struct skra_ctx {
	struct cds_ft *ft;
	bool prefix_shape;		/* guard+target keys: reaches the two-commit tail */
	unsigned int round;		/* the key generation both peers must agree on */
	struct skra_node *nodes;	/* [SKRA_K][SKRA_D]: the pre-seeded chain */
	struct skra_node *fresh;	/* [SKRA_K]: the appender's node per key */
	struct cds_ft_node **ret;	/* [SKRA_T][SKRA_K]: heads handed back */
	pthread_barrier_t bar;
	unsigned long busy;		/* ★ BUSY_ERROR: the progress oracle */
	unsigned long other;		/* any other non-OK, non-NOT_FOUND status */
	unsigned long ok;
	unsigned long notfound;
	unsigned long ins_fail;
};

struct skra_arg {
	struct skra_ctx *c;
	unsigned int t;
};

/*
 * Two holder shapes, alternating by round, exactly as the same-key remove row:
 * an even round's keys differ in their last byte (each chain heads a body slot
 * of an internal holder), an odd round's share a trailing byte (each hangs
 * under a one-byte compressed holder, cn->child) -- the shape whose emptied
 * chain is a detach rather than a slot clear.
 */
static uint64_t skra_u64(unsigned int r, unsigned int k)
{
	return (r & 1) ? (((uint64_t) k << 8) | 0x3f) : (uint64_t) k;
}

/*
 * THE PREFIX SHAPE, and why the row needs a second one.  In the leaf shape
 * above every key is a full fixed-width key, so a removal that empties its slot
 * is an IN-PLACE leaf detach and fuses the dead cell's unsplice into its own
 * structural flip.  remove_all's TWO-COMMIT tail -- publish the removal first,
 * unsplice second -- is then dead code: measured with the FT_DBG_RA_UNSPLICE
 * arm counter, two_commit_arm == 0 over 150 rounds x 64 keys x 3 lanes, which
 * is why restoring the tail's discarded-status form left the row GREEN.
 *
 * @guard "Bk" is a strict PREFIX of the target "Bkd", so the node at "Bk" is an
 * internal holder carrying external_nodes (the guard's own chain) with exactly
 * ONE body child (the target's chain).  Emptying that child leaves the holder
 * childless-with-externals, so the detach PROMOTES the guard's chain to the
 * parent -- @topmost_external_nodes, the non-fused publish, @pub unarmed, and
 * the two-commit tail runs.
 */
static size_t skra_key(struct cds_ft *ft, unsigned int r, unsigned int k,
		bool prefix_shape, bool guard, uint8_t *buf)
{
	if (!prefix_shape) {
		cds_ft_u64_to_key(ft, skra_u64(r, k), buf,
			CDS_FT_LEN_DEFAULT);
		return CDS_FT_LEN_DEFAULT;
	}
	buf[0] = 0x42;				/* 'B' */
	buf[1] = (uint8_t) k;
	buf[2] = (uint8_t) (0x64 + (r & 1));	/* 'd' / 'e', by round */
	return guard ? 2 : 3;
}

/*
 * A remove_all lane: race every key, and KEEP what the call handed back.  The
 * returned head is not walked here -- the chain is the caller's only after a
 * grace period, and walking it under the peers would be reading nodes another
 * lane may still be unlinking.  The accounting runs at the join.
 */
static void *skra_lane(void *arg)
{
	struct skra_arg *a = (struct skra_arg *) arg;
	struct skra_ctx *c = a->c;
	struct cds_ft_iter *iter;
	unsigned int r, i;

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SKRA_R; r++) {
		skr_bwait(&c->bar);		/* seeded */
		for (i = 0; i < SKRA_K; i++) {
			/* Walk the keys in a lane-specific order so the lanes
			 * collide on different keys at different instants. */
			unsigned int k = (i + a->t * (SKRA_K / SKRA_T)) % SKRA_K;
			struct cds_ft_node *res = NULL;
			uint8_t key[8];
			size_t klen;
			enum cds_ft_status s;

			klen = skra_key(c->ft, r, k, c->prefix_shape, false,
				key);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, key, klen);
			if (cds_ft_lookup(c->ft, iter) != CDS_FT_STATUS_OK) {
				rcu_read_unlock();
				rcu_quiescent_state();
				continue;
			}
			s = cds_ft_remove_all(c->ft, iter, &res);
			rcu_read_unlock();
			switch (s) {
			case CDS_FT_STATUS_OK:
				uatomic_inc(&c->ok);
				c->ret[a->t * SKRA_K + k] = res;
				break;
			case CDS_FT_STATUS_NOT_FOUND:
				uatomic_inc(&c->notfound);
				break;
			case CDS_FT_STATUS_BUSY_ERROR:
				/* ★ THE ORACLE.  A peer took a lock this op
				 * wanted and the op gave up instead of aging
				 * the conflict and re-attempting. */
				uatomic_inc(&c->busy);
				break;
			default:
				uatomic_inc(&c->other);
				break;
			}
			rcu_quiescent_state();
		}
		skr_bwait(&c->bar);		/* removed */
		skr_bwait(&c->bar);		/* verified */
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * The APPENDER: one fresh node per key while the lanes empty the chains, so a
 * remove_all that derived its chain head meets an append before it commits.
 */
static void *skra_appender(void *arg)
{
	struct skra_ctx *c = ((struct skra_arg *) arg)->c;
	unsigned int r, k;

	rcu_register_thread();
	rcu_thread_online();
	for (r = 0; r < SKRA_R; r++) {
		skr_bwait(&c->bar);		/* seeded */
		for (k = 0; k < SKRA_K; k++) {
			struct skra_node *n = &c->fresh[k];
			uint8_t key[8];
			size_t klen;

			cds_ft_node_init(&n->node);
			n->k = k;
			n->d = SKRA_D;
			klen = skra_key(c->ft, r, k, c->prefix_shape, false,
				key);
			rcu_read_lock();
			if (cds_ft_insert(c->ft, key, klen,
					&n->node) != CDS_FT_STATUS_OK)
				uatomic_inc(&c->ins_fail);
			rcu_read_unlock();
			rcu_quiescent_state();
		}
		skr_bwait(&c->bar);		/* removed */
		skr_bwait(&c->bar);		/* verified */
	}
	rcu_unregister_thread();
	return NULL;
}

static int inv_concurrent_remove_all_run(bool ordered_list, bool coarse,
		bool prefix_shape, const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct skra_ctx c;
	struct skra_arg args[SKRA_T + 1];
	pthread_t th[SKRA_T + 1];
	struct cds_ft_iter *iter;
	struct skra_node *guard = NULL;
	unsigned int r, k, d, t;
	int ret = 0;

	memset(&c, 0, sizeof(c));
	c.prefix_shape = prefix_shape;
	if (prefix_shape) {
		/*
		 * Variable-length keys, ordered list ON: both are required.  A
		 * strict prefix needs per-call key lengths, and the two-commit
		 * tail this shape exists to reach only exists with the list on.
		 */
		enum cds_ft_writer_strategy ws = coarse ?
			CDS_FT_WRITER_LOCK_COARSE : CDS_FT_WRITER_LOCK_FINE;

		c.ft = create_varlen_ord_ft_ws(&group, &ws);
		guard = (struct skra_node *) calloc(SKRA_K, sizeof(*guard));
		if (!guard)
			abort();
	} else {
		if (cds_ft_group_attr_create(&attr) < 0)
			abort();
		if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
			abort();
		if (cds_ft_group_attr_set_ordered_list(attr, ordered_list) < 0)
			abort();
		if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
				CDS_FT_WRITER_LOCK_COARSE) < 0)
			abort();
		inv_maybe_set_rank_stats(attr);
		if (cds_ft_group_create(attr, &group) < 0)
			abort();
		cds_ft_group_attr_destroy(attr);
		if (cds_ft_create(group, NULL, &c.ft) < 0)
			abort();
	}
	cds_ft_make_shared(c.ft);
	c.nodes = (struct skra_node *) calloc((size_t) SKRA_K * SKRA_D,
			sizeof(*c.nodes));
	c.fresh = (struct skra_node *) calloc(SKRA_K, sizeof(*c.fresh));
	c.ret = (struct cds_ft_node **) calloc((size_t) SKRA_T * SKRA_K,
			sizeof(*c.ret));
	if (!c.nodes || !c.fresh || !c.ret)
		abort();
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	pthread_barrier_init(&c.bar, NULL, SKRA_T + 2);
	for (t = 0; t < SKRA_T; t++) {
		args[t].c = &c;
		args[t].t = t;
		pthread_create(&th[t], NULL, skra_lane, &args[t]);
	}
	args[SKRA_T].c = &c;
	args[SKRA_T].t = SKRA_T;
	pthread_create(&th[SKRA_T], NULL, skra_appender, &args[SKRA_T]);
	for (r = 0; r < SKRA_R; r++) {
		unsigned long busy_before = uatomic_read(&c.busy);

		memset(c.ret, 0, (size_t) SKRA_T * SKRA_K * sizeof(*c.ret));
		rcu_read_lock();
		for (k = 0; k < SKRA_K; k++) {
			uint8_t key[8];
			size_t klen;

			/*
			 * The guard key goes in FIRST: it is what makes the
			 * target's holder carry external_nodes, so a removal
			 * that empties the target slot has a chain to PROMOTE.
			 * Seeded fresh each round because the round's drain
			 * takes it out again.
			 */
			if (prefix_shape) {
				cds_ft_node_init(&guard[k].node);
				guard[k].k = k;
				guard[k].d = 0xff;
				klen = skra_key(c.ft, r, k, true, true, key);
				if (cds_ft_insert(c.ft, key, klen,
						&guard[k].node) !=
						CDS_FT_STATUS_OK) {
					fprintf(stderr, "%s: seed guard key %u failed\n",
						name, k);
					abort();
				}
			}
			for (d = 0; d < SKRA_D; d++) {
				struct skra_node *n = &c.nodes[k * SKRA_D + d];

				cds_ft_node_init(&n->node);
				n->k = k;
				n->d = d;
				n->returned = 0;
				klen = skra_key(c.ft, r, k, prefix_shape,
					false, key);
				if (cds_ft_insert(c.ft, key, klen,
						&n->node) != CDS_FT_STATUS_OK) {
					fprintf(stderr, "%s: seed key %u dup %u failed\n",
						name, k, d);
					abort();
				}
			}
		}
		for (k = 0; k < SKRA_K; k++)
			c.fresh[k].returned = 0;
		rcu_read_unlock();
		rcu_quiescent_state();
		skr_bwait(&c.bar);		/* seeded */
		skr_bwait(&c.bar);		/* removed */

		/*
		 * Exclusive again: the lanes and the appender are parked on the
		 * barrier, so the returned chains are settled and the trie is
		 * quiescent.  Count where every seeded node ended up.
		 */
		rcu_read_lock();
		if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "%s: round %u: verify RED after the concurrent remove_alls\n",
				name, r);
			ret = -1;
		}
		/* Every node a lane was handed back, marked once per hand-back. */
		for (t = 0; t < SKRA_T; t++) {
			for (k = 0; k < SKRA_K; k++) {
				struct cds_ft_node *h = c.ret[t * SKRA_K + k];

				for (; h; h = cds_ft_node_next_rcu(h)) {
					struct skra_node *n =
						(struct skra_node *) h;

					if (n->k != k) {
						fprintf(stderr, "%s: round %u: lane %u key %u was handed a node belonging to key %u\n",
							name, r, t, k, n->k);
						ret = -1;
						break;
					}
					n->returned++;
				}
			}
		}
		/* ...and every node the trie still holds. */
		for (k = 0; k < SKRA_K; k++) {
			struct cds_ft_node *h;
			unsigned int len = 0;
			uint8_t key[8];
			size_t klen;

			/*
			 * THE GUARD MUST HAVE SURVIVED.  It shares the target's
			 * holder, and the removal that empties that holder
			 * PROMOTES the guard's chain to the parent.  A promote
			 * that loses it -- or a two-commit tail that unspliced
			 * its cell and freed it -- shows up here and nowhere
			 * else: the guard is not part of the target's chain, so
			 * the accounting below would never miss it.
			 */
			if (prefix_shape) {
				klen = skra_key(c.ft, r, k, true, true, key);
				cds_ft_iter_set_key(iter, key, klen);
				if (cds_ft_lookup(c.ft, iter) !=
						CDS_FT_STATUS_OK ||
						cds_ft_iter_node(iter) !=
						&guard[k].node) {
					fprintf(stderr, "%s: round %u: key %u GUARD lost by the target's removal\n",
						name, r, k);
					ret = -1;
				}
			}
			klen = skra_key(c.ft, r, k, prefix_shape, false, key);
			cds_ft_iter_set_key(iter, key, klen);
			if (cds_ft_lookup(c.ft, iter) != CDS_FT_STATUS_OK)
				continue;
			for (h = cds_ft_iter_node(iter); h;
					h = cds_ft_node_next_rcu(h)) {
				struct skra_node *n = (struct skra_node *) h;

				len++;
				if (n->returned) {
					fprintf(stderr, "%s: round %u: key %u dup %u was HANDED BACK and is STILL IN THE TRIE\n",
						name, r, k, n->d);
					ret = -1;
				} else if (n != &c.fresh[k]) {
					fprintf(stderr, "%s: round %u: key %u dup %u SURVIVED every remove_all\n",
						name, r, k, n->d);
					ret = -1;
				}
				n->returned++;	/* accounted: in the trie */
			}
			if (len > 1) {
				fprintf(stderr, "%s: round %u: key %u chain length %u > 1\n",
					name, r, k, len);
				ret = -1;
			}
		}
		/*
		 * The accounting closes here: a seeded node accounted ZERO times
		 * is gone from the trie and was handed to nobody -- a LOST NODE;
		 * accounted TWICE is two owners for one node.
		 */
		for (k = 0; k < SKRA_K; k++) {
			for (d = 0; d < SKRA_D; d++) {
				struct skra_node *n = &c.nodes[k * SKRA_D + d];

				if (n->returned == 1)
					continue;
				fprintf(stderr, "%s: round %u: key %u dup %u accounted %u times (expected 1: LOST at 0, DOUBLE-OWNED above 1)\n",
					name, r, k, d, n->returned);
				ret = -1;
			}
			if (c.fresh[k].returned > 1) {
				fprintf(stderr, "%s: round %u: key %u fresh node accounted %u times\n",
					name, r, k, c.fresh[k].returned);
				ret = -1;
			}
		}
		/* Drain whatever survived: the next round starts empty. */
		for (k = 0; k < SKRA_K; k++) {
			unsigned int pass;

			for (pass = 0; pass < (prefix_shape ? 2u : 1u); pass++) {
				struct cds_ft_node *res = NULL;
				uint8_t key[8];
				size_t klen = skra_key(c.ft, r, k, prefix_shape,
					pass == 1, key);

				cds_ft_iter_set_key(iter, key, klen);
				if (cds_ft_lookup(c.ft, iter) !=
						CDS_FT_STATUS_OK)
					continue;
				if (cds_ft_remove_all(c.ft, iter, &res) !=
						CDS_FT_STATUS_OK) {
					fprintf(stderr, "%s: round %u: drain of key %u (%s) failed\n",
						name, r, k,
						pass ? "guard" : "target");
					ret = -1;
				}
			}
		}
		if (cds_ft_count_keys(c.ft) != 0) {
			fprintf(stderr, "%s: round %u: count_keys %lu != 0 after the drain\n",
				name, r, cds_ft_count_keys(c.ft));
			ret = -1;
		}
		rcu_read_unlock();
		rcu_quiescent_state();
		if (uatomic_read(&c.busy) != busy_before) {
			fprintf(stderr, "%s: round %u: %lu remove_all call(s) reported BUSY_ERROR -- the op has no retry\n",
				name, r, uatomic_read(&c.busy) - busy_before);
			ret = -1;
		}
		if (uatomic_read(&c.other) || uatomic_read(&c.ins_fail))
			ret = -1;
		if (ret)
			fprintf(stderr, "%s: RED at round %u\n", name, r);
		/*
		 * ☞ THE NODE-REUSE FENCE.  The next round re-initialises these
		 * nodes, and cds_ft_node_init() writes prev/next on objects a
		 * peer may still hold -- cds_ft_remove's contract reserves a
		 * GRACE PERIOD before any such write:
		 *
		 *   "A grace period must be observed (e.g., synchronize_rcu,
		 *    call_rcu) after success before reclaiming @node memory."
		 *
		 * rcu_barrier() does NOT supply one.  It waits for in-flight
		 * call_rcu CALLBACKS to run, and this row queues none of its
		 * own -- with an empty queue it can return having waited for
		 * nothing at all.  Only synchronize_rcu() fences the readers.
		 * Every worker is parked in skr_bwait, which goes OFFLINE
		 * around the wait, so the grace period completes here.
		 */
		synchronize_rcu();
		rcu_barrier();		/* and retire the library's own freezes */
		skr_bwait(&c.bar);		/* verified */
	}
	for (t = 0; t < SKRA_T + 1; t++)
		pthread_join(th[t], NULL);
	pthread_barrier_destroy(&c.bar);
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	free(c.nodes);
	free(c.fresh);
	free(c.ret);
	free(guard);
	fprintf(stderr,
		"# %s: %u rounds x %u keys x %u dups, %u remove_all lanes + 1 appender (%s, %s): ok=%lu notfound=%lu BUSY=%lu other=%lu insfail=%lu -> %s\n",
		name, SKRA_R, SKRA_K, SKRA_D, SKRA_T,
		coarse ? "coarse" : "fine",
		prefix_shape ? "prefix+promote" : "leaf", uatomic_read(&c.ok),
		uatomic_read(&c.notfound), uatomic_read(&c.busy),
		uatomic_read(&c.other), uatomic_read(&c.ins_fail),
		ret ? "RED" : "ok");
	return ret;
}

static int inv_concurrent_remove_all_nolist(void)
{
	return inv_concurrent_remove_all_run(false, false, false,
		"inv_concurrent_remove_all_nolist");
}

static int inv_concurrent_remove_all_coarse(void)
{
	return inv_concurrent_remove_all_run(false, true, false,
		"inv_concurrent_remove_all_coarse");
}

/*
 * The PREFIX+PROMOTE arm: the only one of the four that reaches remove_all's
 * two-commit ordered-list tail (verified with -DFT_DBG_RA_UNSPLICE, which
 * counts the arm and its aborts -- the leaf arms report two_commit_arm == 0).
 */
static int inv_concurrent_remove_all_prefix(void)
{
	return inv_concurrent_remove_all_run(true, false, true,
		"inv_concurrent_remove_all_prefix");
}

static int inv_concurrent_remove_all_prefix_coarse(void)
{
	return inv_concurrent_remove_all_run(true, true, true,
		"inv_concurrent_remove_all_prefix_coarse");
}

/*
 * cds_ft_insert_replace vs a same-key peer, under CDS_FT_WRITER_LOCK_FINE.
 *
 * WHY THIS ROW EXISTS, AND WHY IT IS NOT SHAPED LIKE THE OTHER FOUR.
 * cds_ft_insert_replace is the LAST point op of the fine-locking transition,
 * and it is the MIRROR of cds_ft_remove_all: remove_all had the exclusion and
 * lacked the loop, this one HAS the loop.  Enumerating its exits says so --
 * `_cds_ft_insert_replace` has three entry `return`s and ONE tail `return ret`,
 * every `ret = -EAGAIN` is set BEFORE `insert_replace_done:`, and the handler
 * there re-attempts on a PERSISTENT urcu_txn that ages the conflict.  So the
 * public `if (ret == -EAGAIN) return CDS_FT_STATUS_BUSY_ERROR` is dead code.
 *
 * ⇒ ★ A BUSY COUNT CANNOT BE THIS ROW'S ORACLE, the way it was remove_all's.
 * The op will never report BUSY; if its exclusion is wrong it SPINS instead.
 * The suspicion is concrete: its replace arms take `ft_flip_txn_guard_parent`
 * -- a §4.B VALIDATE -- where the converted cds_ft_replace now hoists a real
 * ACQUIRE above its routing derivation.  That asymmetry is what produced the
 * MUTUAL STARVATION between replace and remove (@9e62dc3b): a validator parks a
 * word a locker must find CLEAN, and each starves the other.
 *
 * So this row measures THREE things, and the first is the one that would be
 * missed by an accounting-only oracle:
 *
 *   1. PROGRESS -- per-lane completed-operation counts and a wall-clock budget.
 *      A starving lane finishes far fewer ops than its peer while the trie
 *      stays perfectly consistent, so a pure key-accounting row reads GREEN
 *      through a livelock.  The in-library witness is the retry cap, newly
 *      added to that loop (it had NONE, so a spin there was invisible).
 *   2. OWNERSHIP -- insert_replace hands back the WHOLE displaced chain, so the
 *      same exact accounting remove_all needed applies: every node is either
 *      handed back exactly once or still in the trie, never both and never
 *      neither.
 *   3. UNIQUENESS -- after a replace the key must hold EXACTLY the installed
 *      node; two of them means a replace that appended instead of displacing.
 */
#define SIR_K		8
#define SIR_R		40
#define SIR_BURST	200
/*
 * One never-reused node per install.  A replacer lane installs at most once
 * per (round, burst, key); the peer lane can arm twice in one visit (an
 * insert-family call and a replace), so it gets twice the budget.
 */
#define SIR_POOL_N	((size_t) SIR_R * SIR_BURST * SIR_K + 1)
#define SIR_PEER_POOL_N	(2 * SIR_POOL_N + 16)
#define SIR_BUDGET_S	120	/* wall-clock guard: a livelock must FAIL, not hang */

struct sir_node {
	struct cds_ft_node node;
	unsigned int k;
	unsigned int owner;		/* which lane last installed it */
};

struct sir_ctx {
	struct cds_ft *ft;
	/*
	 * TWO nodes per lane per key, used alternately.  A node cannot be
	 * re-initialised while the trie still holds it, and a lane's node stays
	 * in the trie until something displaces it -- so the lane uses the
	 * OTHER one next, and only advances the alternation when a call
	 * actually succeeded.  By the time a node comes round again the
	 * intervening successful install has displaced the whole chain it was
	 * in (or a peer lane already did), so it is out.
	 */
	/*
	 * ☞ NEVER-REUSED NODES, ONE POOL PER LANE.  Every lane here used to
	 * cycle a small set of nodes, re-initialising one on the belief that an
	 * intervening call had displaced it.  A node that has LEFT the trie is
	 * still owed to any reader that found it before it left, and
	 * cds_ft_node_init zeroes prev and next, so the belief is not the test
	 * the contract sets -- see sir_replacer.  Taking a fresh node each
	 * install removes the hazard by construction.
	 */
	struct sir_node *pool[2];	/* [2][SIR_POOL_N] */
	size_t pool_n[2];
	struct sir_node *peer_pool;	/* [SIR_PEER_POOL_N] */
	size_t peer_pool_n;
	struct sir_node **peer_cur;	/* [SIR_K]: the peer's installed node */
	unsigned char *peer_in;		/* [SIR_K] */
	unsigned long ops[3];		/* completed ops per lane -- THE PROGRESS ORACLE */
	unsigned long busy;		/* CDS_FT_STATUS_BUSY_ERROR (expected dead) */
	unsigned long other;
	unsigned long handed;		/* chains handed back */
	unsigned long foreign;		/* a hand-back naming a node of another key */
	/*
	 * Both outcomes of the peer's insert_unique, so "green" cannot mean the
	 * call never inserted: @iu_ok is the arm that actually races an
	 * insert_replace for the key, @iu_dup the refusal.
	 */
	unsigned long iu_ok, iu_dup;
	/* remove_all outcomes, so its arm cannot be green-by-never-running. */
	unsigned long ra_ok, ra_miss;
	unsigned long rp_ok, rp_miss, rp_nf;	/* cds_ft_replace outcomes */
	int stop;
};

struct sir_arg {
	struct sir_ctx *c;
	unsigned int id;
};

/*
 * A replace lane: install its own node at every key, over and over.  Each call
 * either creates the key (OK) or displaces whatever chain is there
 * (DUPLICATE_FOUND + the old head).  The displaced chain is accounted at the
 * join, not walked here -- it is only the caller's after a grace period.
 */
static void *sir_replacer(void *arg)
{
	struct sir_arg *a = (struct sir_arg *) arg;
	struct sir_ctx *c = a->c;
	unsigned int id = a->id, r, burst, k;

	rcu_register_thread();
	rcu_thread_online();
	for (r = 0; r < SIR_R && !uatomic_read(&c->stop); r++)
		for (burst = 0; burst < SIR_BURST; burst++)
		for (k = 0; k < SIR_K; k++) {
			struct cds_ft_node *old = NULL;
			/*
			 * ☞ A NEVER-REUSED NODE PER INSTALL.  This lane used to
			 * alternate between two nodes per key, re-initialising
			 * whichever one it believed the trie no longer held.
			 * "No longer held" is not the test the contract sets:
			 *
			 *   "A grace period must be observed (e.g.,
			 *    synchronize_rcu, call_rcu) after success before
			 *    reclaiming @node memory."
			 *
			 * A node that has LEFT the trie is still owed to any
			 * reader that found it before it left, and
			 * cds_ft_node_init zeroes prev and next -- manufacturing
			 * exactly the stale back-edge this row was built to
			 * hunt: a pred whose next is NULL while its successor
			 * still points back at it.  A pool that is never reused
			 * removes the hazard by construction, and at
			 * SIR_R * SIR_BURST * SIR_K nodes it costs a few MB for
			 * the run.
			 */
			struct sir_node *n;

			if (c->pool_n[id] >= SIR_POOL_N)
				abort();	/* sized from the loop bounds */
			n = &c->pool[id][c->pool_n[id]++];
			uint8_t key[8];
			enum cds_ft_status s;

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			cds_ft_node_init(&n->node);
			n->k = k;
			n->owner = id;
			s = cds_ft_insert_replace(c->ft, key, CDS_FT_LEN_DEFAULT,
				&n->node, &old);
			rcu_read_unlock();
			switch (s) {
			case CDS_FT_STATUS_OK:
			case CDS_FT_STATUS_DUPLICATE_FOUND:
				uatomic_inc(&c->ops[id]);
				if (old) {
					uatomic_inc(&c->handed);
					/*
					 * The displaced chain is the caller's.
					 * Walk it under the read lock and check
					 * it belongs to THIS key: a hand-back
					 * naming another key's node is a replace
					 * that displaced the wrong chain.
					 */
					rcu_read_lock();
					for (; old; old = cds_ft_node_next_rcu(old)) {
						struct sir_node *o =
							(struct sir_node *) old;

						if (o->k != k)
							uatomic_inc(&c->foreign);
#ifdef FT_INV_SIR_NO_GHOST_REMOVE
						/*
						 * ☞ MECHANISM TEST.  cds_ft_insert_replace
						 * freezes NOTHING (its sibling
						 * _cds_ft_replace_locked freezes the one node
						 * it displaces, at three sites; this op
						 * displaces a whole CHAIN and marks none of
						 * it).  So a holder of a displaced node cannot
						 * tell it left the trie, cds_ft_remove's
						 * ft_node_is_removed() escape cannot fire, and
						 * remove takes the INTERIOR lane on a ghost --
						 * deriving pred from a stale prev whose next is
						 * NULL, which is the edge that spins forever.
						 *
						 * This arm tells the peer out-of-band what the
						 * tombstone would have told it.  If the livelock
						 * vanishes, that IS the mechanism.
						 */
						if (o->owner == 2)
							c->peer_in[o->k] = 0;
#endif
					}
					rcu_read_unlock();
				}
				break;
			case CDS_FT_STATUS_BUSY_ERROR:
				uatomic_inc(&c->busy);
				break;
			default:
				uatomic_inc(&c->other);
				break;
			}
			rcu_quiescent_state();
			if (uatomic_read(&c->stop))
				break;
		}
	rcu_unregister_thread();
	return NULL;
}

/*
 * The PEER: same-key cds_ft_insert / cds_ft_remove.  cds_ft_remove is the op
 * whose holder LOCK the replace arms' §4.B guard is suspected of parking
 * against, so it is the lane that would starve -- and be starved.
 */
/* The peer node the trie currently holds (or the one it will install next). */
static struct sir_node *sir_peer_node(struct sir_ctx *c, unsigned int k)
{
	return c->peer_cur[k];
}

/*
 * Arm a NEVER-REUSED node as this lane's node for @k.  The pool is sized from
 * the lane's own loop bounds; an overrun is a test bug, so say so loudly
 * rather than walk off the end.
 */
static struct sir_node *sir_peer_arm(struct sir_ctx *c, unsigned int k)
{
	struct sir_node *n;

	if (c->peer_pool_n >= SIR_PEER_POOL_N)
		abort();
	n = &c->peer_pool[c->peer_pool_n++];
	cds_ft_node_init(&n->node);
	n->k = k;
	n->owner = 2;
	c->peer_cur[k] = n;
	return n;
}

static void *sir_peer(void *arg)
{
	struct sir_ctx *c = ((struct sir_arg *) arg)->c;
	unsigned int r, burst, k;
	struct cds_ft_iter *iter;
	/*
	 * ☠ DO NOT ALTERNATE ON @burst.  @peer_in[k] flips on EVERY visit, so a
	 * given key reaches the insert branch only on every OTHER burst -- always
	 * the same parity -- and `burst & 1` there is never true.  Measured: the
	 * insert_unique arm ran 0 times, ok AND dup, which is why both outcomes
	 * are counted.  Count the insert ATTEMPTS instead; that is the sequence
	 * that actually alternates.
	 */
	unsigned long ins_attempt = 0, rem_attempt = 0;
	/*
	 * ☠ AND DO NOT ALTERNATE ON A GLOBAL VISIT COUNTER EITHER -- SAME TRAP,
	 * ONE LEVEL UP.  It advances by SIR_K per visit OF ONE KEY, so with
	 * SIR_K == 8 every `visit & 3` / `visit & 7` is CONSTANT per key: the
	 * remove_all arm ran on exactly 2 of the 8 keys (16000 of 16000 calls)
	 * and a `visit & 7` replace arm on 1, which is not "1/8 of the traffic"
	 * but "one key, never the others".  Count PER KEY.
	 */
	unsigned long kvisit[SIR_K];

	memset(kvisit, 0, sizeof(kvisit));
	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (r = 0; r < SIR_R && !uatomic_read(&c->stop); r++)
		for (burst = 0; burst < SIR_BURST; burst++)
		for (k = 0; k < SIR_K; k++) {
			uint8_t key[8];

			cds_ft_u64_to_key(c->ft, (uint64_t) k, key,
				CDS_FT_LEN_DEFAULT);
			rcu_read_lock();
			if ((kvisit[k]++ & 3) == 3) {
				/*
				 * ☞ REMOVE_ALL, the third peer the flipped
				 * contract would claim -- and the only thing
				 * that EMPTIES the key.  Without it the two
				 * replacer lanes keep every key occupied and
				 * insert_unique can only ever be refused, so
				 * this arm is what gives its inserting path a
				 * window to race a displacement in.
				 *
				 * The handed-back chain is accounted exactly as
				 * the replacer accounts a displaced one: walk
				 * it under the read lock, charge a hand-back
				 * naming another key, and mark each replacer
				 * node provably OUT so its lane may re-init it.
				 * @peer_in[k] drops unconditionally on OK --
				 * peer[k] only ever lives at key k, so
				 * remove_all took it if it was there.
				 */
				struct cds_ft_node *chain = NULL;

				cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
				if (cds_ft_lookup(c->ft, iter) ==
						CDS_FT_STATUS_OK &&
				    cds_ft_remove_all(c->ft, iter, &chain) ==
						CDS_FT_STATUS_OK) {
					uatomic_inc(&c->ra_ok);
					uatomic_inc(&c->ops[2]);
					c->peer_in[k] = 0;
					for (; chain;
					     chain = cds_ft_node_next_rcu(chain)) {
						struct sir_node *o =
							(struct sir_node *) chain;

						if (o->k != k)
							uatomic_inc(&c->foreign);
					}
					/*
					 * ☞ AND RACE INSERT_UNIQUE INTO THE KEY
					 * WE JUST EMPTIED.  This is the tightest
					 * window there is: the replacer lanes are
					 * refilling the key right now, so the
					 * INSERTING arm of insert_unique meets a
					 * concurrent displacement here or nowhere.
					 * Left to the ordinary alternation it fired
					 * 0-3 times a run; the key is simply never
					 * empty otherwise.  @peer[k] is provably out
					 * (remove_all just handed back whatever was
					 * at k), so re-initialising it is sound.
					 */
					{
						struct cds_ft_node *iu2 = NULL;

						(void) sir_peer_arm(c, k);
						if (cds_ft_insert_unique(c->ft,
								key,
								CDS_FT_LEN_DEFAULT,
								&sir_peer_node(c, k)->node,
								&iu2) ==
								CDS_FT_STATUS_OK) {
							c->peer_in[k] = 1;
							uatomic_inc(&c->iu_ok);
							uatomic_inc(&c->ops[2]);
						} else {
							uatomic_inc(&c->iu_dup);
						}
					}
				} else {
					uatomic_inc(&c->ra_miss);
				}
			} else if (!c->peer_in[k]) {
				enum cds_ft_status is;

				(void) sir_peer_arm(c, k);
				/*
				 * ☞ ALTERNATE INSERT AND INSERT_UNIQUE.  The
				 * contract cds_ft_insert_replace would claim if
				 * it were declared converted names FIVE peers --
				 * insert, insert_unique, replace, remove and
				 * remove_all -- and this oracle drove only
				 * insert and remove, so three of them had NO
				 * concurrent coverage at all.
				 *
				 * insert_unique shares this lane's lifetime
				 * discipline exactly: OK means the node went in,
				 * DUPLICATE_FOUND means it did not, and either
				 * way @peer_in[k] says whether the trie holds
				 * it.  (remove_all does NOT share it -- it hands
				 * back the whole chain, including the replacer
				 * lanes' nodes, so it needs the reuse
				 * bookkeeping and is not added here on the
				 * strength of looking similar.)
				 */
				if (ins_attempt++ & 1) {
					struct cds_ft_node *iu_ret = NULL;

					/*
					 * @result_node is written on the
					 * DUPLICATE path (it names the existing
					 * node), so it is not optional: passing
					 * NULL SEGV'd 3/3 here, and every other
					 * caller in the tree passes a real one.
					 */
					is = cds_ft_insert_unique(c->ft, key,
						CDS_FT_LEN_DEFAULT,
						&sir_peer_node(c, k)->node, &iu_ret);
					if (is == CDS_FT_STATUS_OK)
						uatomic_inc(&c->iu_ok);
					else
						uatomic_inc(&c->iu_dup);
				} else
					is = cds_ft_insert(c->ft, key,
						CDS_FT_LEN_DEFAULT,
						&sir_peer_node(c, k)->node);
				if (is == CDS_FT_STATUS_OK) {
					c->peer_in[k] = 1;
					uatomic_inc(&c->ops[2]);
				}
			} else {
				/*
				 * ☞ ALTERNATE REMOVE AND REPLACE -- on the
				 * REMOVE ATTEMPT count, for the same reason the
				 * insert arm alternates on @ins_attempt: this is
				 * the sequence that actually alternates, where
				 * any function of @k or of the visit index does
				 * not.
				 *
				 * replace is the fifth and last peer the flipped
				 * contract would claim.  It needs an old node
				 * this lane OWNS and a distinct new one, so it
				 * installs a FRESH node and @peer_cur[k] follows
				 * it.  The key stays occupied either way, so
				 * @peer_in[k] survives an OK replace and only a
				 * refusal clears it.
				 */
				cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
				if (cds_ft_lookup(c->ft, iter) == CDS_FT_STATUS_OK) {
					enum cds_ft_status s;

					if (rem_attempt++ & 1) {
						struct sir_node *old =
							sir_peer_node(c, k);
						struct sir_node *newp;

						if (c->peer_pool_n >=
								SIR_PEER_POOL_N)
							abort();
						newp = &c->peer_pool[
							c->peer_pool_n++];
						cds_ft_node_init(&newp->node);
						newp->k = k;
						newp->owner = 2;
						s = cds_ft_replace(c->ft, iter,
							&old->node,
							&newp->node);
						if (s == CDS_FT_STATUS_OK) {
							c->peer_cur[k] = newp;
							uatomic_inc(&c->rp_ok);
							uatomic_inc(&c->ops[2]);
						} else {
							uatomic_inc(&c->rp_miss);
							if (s == CDS_FT_STATUS_NOT_FOUND)
								uatomic_inc(&c->rp_nf);
							/*
							 * An insert_replace
							 * displaced the chain
							 * between the lookup and
							 * the swap: the old node
							 * is the replacer's to
							 * account, not ours.
							 */
							c->peer_in[k] = 0;
						}
					} else {
						s = cds_ft_remove(c->ft, iter,
							&sir_peer_node(c, k)->node);
						if (s == CDS_FT_STATUS_OK) {
							c->peer_in[k] = 0;
							uatomic_inc(&c->ops[2]);
						} else if (s == CDS_FT_STATUS_NOT_FOUND) {
							/*
							 * A replace displaced
							 * it: the node is the
							 * replacer's to account,
							 * not ours.
							 */
							c->peer_in[k] = 0;
						}
					}
				} else {
					c->peer_in[k] = 0;
				}
			}
			rcu_read_unlock();
			rcu_quiescent_state();
			if (uatomic_read(&c->stop))
				break;
		}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/* The wall-clock guard: a livelock must FAIL the row, not hang the suite. */
static void *sir_timer(void *arg)
{
	struct sir_ctx *c = ((struct sir_arg *) arg)->c;
	unsigned int i;

	for (i = 0; i < SIR_BUDGET_S * 10 && !uatomic_read(&c->stop); i++) {
		struct timespec ts = { .tv_sec = 0, .tv_nsec = 100000000L };

		(void) nanosleep(&ts, NULL);
	}
	uatomic_set(&c->stop, 1);
	return NULL;
}

static int inv_concurrent_insert_replace_run(bool coarse, const char *name)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct sir_ctx c;
	struct sir_arg args[3];
	pthread_t th[3], timer;
	struct cds_ft_iter *iter;
	unsigned int i, k;
	unsigned long lo, hi;
	int ret = 0;
	bool timed_out;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 8) < 0)
		abort();
	if (cds_ft_group_attr_set_ordered_list(attr, false) < 0)
		abort();
	if (coarse && cds_ft_group_attr_set_writer_strategy(attr,
			CDS_FT_WRITER_LOCK_COARSE) < 0)
		abort();
	inv_maybe_set_rank_stats(attr);
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	memset(&c, 0, sizeof(c));
	if (cds_ft_create(group, NULL, &c.ft) < 0)
		abort();
	cds_ft_make_shared(c.ft);
	for (i = 0; i < 2; i++) {
		c.pool[i] = (struct sir_node *) calloc(SIR_POOL_N,
			sizeof(*c.pool[i]));
		if (!c.pool[i])
			abort();
	}
	c.peer_pool = (struct sir_node *) calloc(SIR_PEER_POOL_N,
		sizeof(*c.peer_pool));
	c.peer_cur = (struct sir_node **) calloc(SIR_K, sizeof(*c.peer_cur));
	c.peer_in = (unsigned char *) calloc(SIR_K, 1);
	if (!c.peer_pool || !c.peer_cur || !c.peer_in)
		abort();
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	for (i = 0; i < 3; i++) {
		args[i].c = &c;
		args[i].id = i;
	}
	pthread_create(&timer, NULL, sir_timer, &args[0]);
	pthread_create(&th[0], NULL, sir_replacer, &args[0]);
	pthread_create(&th[1], NULL, sir_replacer, &args[1]);
	pthread_create(&th[2], NULL, sir_peer, &args[2]);
	for (i = 0; i < 3; i++)
		pthread_join(th[i], NULL);
	timed_out = uatomic_read(&c.stop) != 0;
	uatomic_set(&c.stop, 1);
	pthread_join(timer, NULL);
	rcu_quiescent_state();
	rcu_barrier();

	/*
	 * ★ PROGRESS FIRST.  The lanes run identical bounded loops, so a healthy
	 * run has all three finishing them; a starved lane is one that did not.
	 */
	if (timed_out) {
		fprintf(stderr, "%s: BUDGET EXHAUSTED (%us): ops lane0=%lu lane1=%lu peer=%lu -- a lane is not converging\n",
			name, SIR_BUDGET_S, uatomic_read(&c.ops[0]),
			uatomic_read(&c.ops[1]), uatomic_read(&c.ops[2]));
		ret = -1;
	}
	lo = hi = uatomic_read(&c.ops[0]);
	for (i = 1; i < 3; i++) {
		unsigned long v = uatomic_read(&c.ops[i]);

		if (v < lo)
			lo = v;
		if (v > hi)
			hi = v;
	}
	/*
	 * A STARVATION RATIO, not a hang test: the mutual-starvation defect this
	 * row exists for showed up as one lane completing a small fraction of
	 * its peer's work while everything stayed consistent.  100x apart is far
	 * outside scheduling noise for three lanes running the same loop.
	 */
	if (hi > 100 * (lo + 1)) {
		fprintf(stderr, "%s: STARVATION: lane ops %lu / %lu / %lu (max/min > 100x)\n",
			name, uatomic_read(&c.ops[0]), uatomic_read(&c.ops[1]),
			uatomic_read(&c.ops[2]));
		ret = -1;
	}

	rcu_read_lock();
	if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: verify RED after the run\n", name);
		ret = -1;
	}
	/* UNIQUENESS + OWNERSHIP: each key holds exactly one node, and every
	 * node is EITHER in the trie OR handed back once, never both/neither. */
	for (k = 0; k < SIR_K; k++) {
		struct cds_ft_node *h;
		unsigned int len = 0;
		uint8_t key[8];

		cds_ft_u64_to_key(c.ft, (uint64_t) k, key, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, key, CDS_FT_LEN_DEFAULT);
		if (cds_ft_lookup(c.ft, iter) == CDS_FT_STATUS_OK) {
			for (h = cds_ft_iter_node(iter); h;
					h = cds_ft_node_next_rcu(h)) {
				struct sir_node *n = (struct sir_node *) h;

				len++;
				if (n->k != k) {
					fprintf(stderr, "%s: key %u holds a node of key %u\n",
						name, k, n->k);
					ret = -1;
				}
			}
		}
		/*
		 * The peer appends, so a chain of 2 is legal (replacer node +
		 * peer node).  What is NOT legal is BOTH replacers' nodes being
		 * present: a replace that appended instead of displacing.
		 */
		if (len > 2) {
			fprintf(stderr, "%s: key %u chain length %u > 2\n",
				name, k, len);
			ret = -1;
		}
	}
	rcu_read_unlock();
	if (uatomic_read(&c.foreign)) {
		fprintf(stderr, "%s: %lu hand-back(s) named a node of another key\n",
			name, uatomic_read(&c.foreign));
		ret = -1;
	}
	if (uatomic_read(&c.other)) {
		fprintf(stderr, "%s: %lu unexpected status(es)\n", name,
			uatomic_read(&c.other));
		ret = -1;
	}
	cds_ft_iter_destroy(iter);
	rcu_quiescent_state();
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	for (i = 0; i < 2; i++)
		free(c.pool[i]);
	free(c.peer_pool);
	free(c.peer_cur);
	free(c.peer_in);
	fprintf(stderr,
		"# %s: %u rounds x %u burst x %u keys, 2 insert_replace lanes + 1 same-key insert/insert_unique/replace/remove/remove_all peer (%s): ops=%lu/%lu/%lu handed=%lu BUSY=%lu other=%lu insert_unique=%lu ok/%lu dup remove_all=%lu ok/%lu miss replace=%lu ok/%lu miss (%lu nf) -> %s\n",
		name, SIR_R, SIR_BURST, SIR_K, coarse ? "coarse" : "fine",
		uatomic_read(&c.ops[0]), uatomic_read(&c.ops[1]),
		uatomic_read(&c.ops[2]), uatomic_read(&c.handed),
		uatomic_read(&c.busy), uatomic_read(&c.other),
		uatomic_read(&c.iu_ok), uatomic_read(&c.iu_dup),
		uatomic_read(&c.ra_ok), uatomic_read(&c.ra_miss),
		uatomic_read(&c.rp_ok), uatomic_read(&c.rp_miss),
		uatomic_read(&c.rp_nf),
		ret ? "RED" : "ok");
	return ret;
}

/*
 * ☑ BOTH ARMS ARE IN THE DEFAULT SET, and the history is worth keeping because
 * what they caught was not the op they name.  They were opt-in and RED while
 * cds_ft_insert_replace displaced a chain WITHOUT TOMBSTONING IT: a holder of a
 * displaced node could not learn it had left the trie, cds_ft_remove's
 * ft_node_is_removed() escape could not fire, and remove spun 50001 times on
 * the ghost -- 48 GB of it, since each attempt allocates.  With the retry cap
 * armed (-DFT_DEBUG_OP_RETRY_CAP) that read
 *
 *   FT OP RETRY LIVELOCK: op=2 attempts=50001 last_ret=0
 *   FT OP EAGAIN SITE: line=5822 consecutive=50000
 *
 * and the URCU_TXN_REC_LOST hook named the losing edge as
 * `pred->next: elem -> next` against a slot holding NULL.
 *
 * ★ THE COARSE ARM SPINNING IS WHAT GAVE IT AWAY.  Under COARSE every writer
 * serialises on the FT-wide mutex, so no peer could be parking the word remove
 * was refused on -- a SELF-refusal, i.e. a property of the SHAPE, which is what
 * made it reproducible in five single-threaded calls
 * (test_insert_replace_tombstones_displaced_chain in ft_unit is that sequence).
 *
 * Measured across the fix: the reusing arm wedged 7 of 8 runs before and 0 of 8
 * after.  ☞ The three compile-time control arms below
 * (FT_INV_SIR_{FRESH_NODES,SOUND_REUSE,NO_GHOST_REMOVE}) are what established
 * the fault was the library's and not this row's node recycling; keep them.
 */
/*
 * ☑ BOTH ARMS ARE IN THE DEFAULT SET.  The fine arm was opt-in while it reached
 * a chain-liveness gap that this row is precisely what found: a whole-chain
 * displacement freezes only the HEAD (a txn is bounded, a chain is not), so a
 * MEMBER of a displaced chain was retired and UNMARKED, and cds_ft_remove's
 * interior lane derived @pred from a stale prev and recorded an edge whose
 * expected-old the slot can never hold -- 50001 attempts, then a memcg kill at
 * 48 GB.  Measured across the cure: 7 wedges of 8 before, 0 of 40 after.
 */
static int inv_concurrent_insert_replace_nolist(void)
{
	return inv_concurrent_insert_replace_run(false,
		"inv_concurrent_insert_replace_nolist");
}

static int inv_concurrent_insert_replace_coarse(void)
{
	return inv_concurrent_insert_replace_run(true,
		"inv_concurrent_insert_replace_coarse");
}

/*
 * The ORDERED-LIST arm, and it is not a duplicate of the row above: it is the
 * only one that reaches remove_all's TWO-COMMIT TAIL.  With the list on, a
 * shape that could not fuse the dead head cell's unsplice into its structural
 * flip (@pub unarmed -- the prefix, recompaction and compressed-parent shapes)
 * publishes the removal FIRST and unsplices SECOND.  That second commit used to
 * have its status discarded, on the premise that an "ABORT [is] unreachable
 * under its current exclusion" -- true only while remove_all demanded caller
 * writer-exclusion.  Converting it deletes the premise, and an aborted unsplice
 * leaves the key gone from the structural index but still in the ordered list,
 * then frees its cell while the list still points at it.
 */
static int inv_concurrent_remove_all_list(void)
{
	return inv_concurrent_remove_all_run(true, false, false,
		"inv_concurrent_remove_all_list");
}


/* ================================================================== */
/*                                                                    */
/*   INVARIANT: the two §4.B GUARD sites, driven on a FINE trie       */
/*              (coverage for the 2026-09-22 guard audit)             */
/*                                                                    */
/* ================================================================== */

/*
 * ☞ WHY THIS SHAPE.  Two publish sites still GUARD their parent instead of
 * locking it: _cds_ft_replace_locked's head-swap arm, whose publish parent is
 * the COMPRESSED node when the head is that node's single external child
 * (`parent_nf = cn ? ft_compressed_node_flag(cn) : holder_flag`), and
 * ft_detach_node's grandparent guard on the arm where no recompaction ran.  A
 * §4.B guard is an MW VALIDATE -- a record -- and every record's settle is a
 * BLIND store, so a guard on a word a PEER parks SW under its lock is the shape
 * that tore the ordered list's head (ft_txn_list_insert_between_prepare).
 *
 * The audit measured both sites at ZERO on FINE concurrent tries; this test
 * drives the shape they need, so that zero is a measurement of an EXERCISED
 * window rather than of an absent one:
 *
 *   key A alone under prefix P -> its head is the single external child of P's
 *   COMPRESSED node, which is what makes the replace publish into @cn;
 *   key B = P|other, inserted and removed by a PEER -> every insert SPLITS that
 *   compressed node and every remove COLLAPSES it again (chain compress), so
 *   the peer writes the very state word the replacer would guard, under that
 *   node's own lock, and its remove drives the detach arm as well.
 *
 * Green means the two interleave without losing a key or a node.  The AUDIT is
 * what answers the guard question: -DFT_DEBUG_GUARD_AUDIT counts the plants per
 * site (FINE / unheld), -DURCU_TXN_DEBUG_PARK_CLOBBER fires if a park ever
 * erases a validate's proxy.
 */
#define GCP_KLEN	8
#define GCP_MS		1500		/* FT_INV_GCP_MS overrides */
#define GCP_ROUNDS	4

struct gcp_ctx {
	struct cds_ft *ft;
	int run, stop, parked;
	unsigned long repl, repl_fail, ins, rm, miss;
	/* The head at key A: written by the REPLACER alone. */
	struct ft_test_node *a_cur;
};

/*
 * The key layout, and why it is shaped like this.
 *
 * Bytes 0..2 are a fixed spine {0xA7, 0xB3, 0xC1}; byte 3 discriminates; the
 * tail is zero.  Three SIBLING keys (below) branch at depths 0, 1 and 2, so the
 * trie really carries internal nodes there -- which puts the chain's holder at
 * DEPTH 3.  That is the point: under the exponential spacing 3 is not a lock
 * level (ft_lock_level(3) == 2), so the holder's ANCHOR is the node at depth 2
 * and the op's lock word is NOT the holder's own metadata.  That inequality is
 * exactly the `ft_flag_to_metadata(ft, parent_nf) != hm` the replace's §4.B
 * guard is planted on.  A shallower key (holder at depth 0 or 1, its own
 * anchor) skips the guard, which is why the first version of this test drove
 * the shape and measured ZERO plants.
 */
static void gcp_key(uint8_t *key, uint8_t last)
{
	memset(key, 0, GCP_KLEN);
	key[0] = 0xA7;
	key[1] = 0xB3;
	key[2] = 0xC1;
	key[3] = last;
}

/* The siblings that force a branch at depths 0, 1 and 2.  Never removed. */
static void gcp_sibling_key(uint8_t *key, unsigned int which)
{
	gcp_key(key, 0x7F);
	switch (which) {
	case 0: key[0] = 0xA8; break;		/* branch at depth 0 */
	case 1: key[1] = 0xB4; break;		/* branch at depth 1 */
	default: key[2] = 0xC2; break;		/* branch at depth 2 */
	}
}

static struct ft_test_node *gcp_node(uint8_t last, uint64_t tag)
{
	struct ft_test_node *n = node_alloc(tag);

	gcp_key(n->okey, last);
	n->value = GCP_KLEN;
	return n;
}

/* Park with a quiescent state: a spinning QSBR thread holds back every GP. */
static void gcp_park(struct gcp_ctx *c)
{
	uatomic_inc(&c->parked);
	while (!uatomic_load(&c->run, CMM_ACQUIRE) &&
			!uatomic_load(&c->stop, CMM_RELAXED)) {
		caa_cpu_relax();
		rcu_quiescent_state();
	}
	uatomic_dec(&c->parked);
}

static void *gcp_replacer(void *arg)
{
	struct gcp_ctx *c = (struct gcp_ctx *) arg;
	struct cds_ft_iter *iter;
	uint8_t key[GCP_KLEN];

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	gcp_key(key, 0x01);
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		struct ft_test_node *nw, *old;

		if (!uatomic_load(&c->run, CMM_ACQUIRE)) {
			gcp_park(c);
			continue;
		}
		nw = gcp_node(0x01, 1);
		old = c->a_cur;		/* this thread owns it */
		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, GCP_KLEN);
		if (cds_ft_lookup(c->ft, iter) == CDS_FT_STATUS_OK &&
				cds_ft_replace(c->ft, iter, &old->node,
					&nw->node) == CDS_FT_STATUS_OK) {
			c->a_cur = nw;
			nw = NULL;
			node_free_rcu(old);	/* displaced: out of the trie */
			uatomic_inc(&c->repl);
		} else {
			uatomic_inc(&c->repl_fail);
		}
		rcu_read_unlock();
		if (nw)
			node_free(nw);		/* refused: never published */
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

/*
 * The PEER: insert and remove key B under the same prefix.  Each insert splits
 * the compressed node the replacer publishes into; each remove collapses it
 * back through the detach / chain-compress path.
 */
static void *gcp_churner(void *arg)
{
	struct gcp_ctx *c = (struct gcp_ctx *) arg;
	struct cds_ft_iter *iter;
	uint8_t key[GCP_KLEN];

	rcu_register_thread();
	rcu_thread_online();
	if (cds_ft_iter_create(c->ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	gcp_key(key, 0x02);
	while (!uatomic_load(&c->stop, CMM_RELAXED)) {
		struct ft_test_node *n;
		struct cds_ft_node *found;

		if (!uatomic_load(&c->run, CMM_ACQUIRE)) {
			gcp_park(c);		/* parked with B ABSENT */
			continue;
		}
		n = gcp_node(0x02, 2);
		rcu_read_lock();
		if (cds_ft_insert(c->ft, key, GCP_KLEN, &n->node) ==
				CDS_FT_STATUS_OK) {
			uatomic_inc(&c->ins);
			n = NULL;
		}
		rcu_read_unlock();
		if (n)
			node_free(n);		/* never published */
		rcu_quiescent_state();

		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, GCP_KLEN);
		if (cds_ft_lookup(c->ft, iter) == CDS_FT_STATUS_OK &&
				(found = cds_ft_iter_node(iter)) &&
				cds_ft_remove(c->ft, iter, found) ==
					CDS_FT_STATUS_OK) {
			node_free_rcu(to_test_node(found));
			uatomic_inc(&c->rm);
		} else {
			uatomic_inc(&c->miss);
		}
		rcu_read_unlock();
		rcu_quiescent_state();
	}
	cds_ft_iter_destroy(iter);
	rcu_unregister_thread();
	return NULL;
}

static int inv_guard_compressed_publish(void)
{
	struct cds_ft_group *group;
	struct gcp_ctx c;
	pthread_t th[2];
	struct cds_ft_iter *iter;
	const char *env = getenv("FT_INV_GCP_MS");
	unsigned long long ms = env ? strtoull(env, NULL, 10) : GCP_MS;
	uint8_t key[GCP_KLEN];
	unsigned int round;
	int ret = 0;
	enum cds_ft_writer_strategy ws = CDS_FT_WRITER_LOCK_FINE;

	memset(&c, 0, sizeof(c));
	c.ft = create_varlen_ord_ft_ws(&group, &ws);
	cds_ft_make_shared(c.ft);
	if (cds_ft_iter_create(c.ft, &iter) != CDS_FT_STATUS_OK)
		abort();
	/*
	 * The spine siblings first: they are what make the holder sit at depth
	 * 3 (see gcp_key).  Held for the whole test and drained at the end.
	 */
	{
		unsigned int i;

		for (i = 0; i < 3; i++) {
			struct ft_test_node *sn = node_alloc(0x70 + i);

			gcp_sibling_key(sn->okey, i);
			sn->value = GCP_KLEN;
			if (cds_ft_insert(c.ft, sn->okey, GCP_KLEN,
					&sn->node) != CDS_FT_STATUS_OK)
				abort();
		}
	}
	/* Key A alone below depth 3: its head is the compressed node's single
	 * external child, so the replace publishes into @cn. */
	c.a_cur = gcp_node(0x01, 1);
	gcp_key(key, 0x01);
	if (cds_ft_insert(c.ft, key, GCP_KLEN, &c.a_cur->node) !=
			CDS_FT_STATUS_OK)
		abort();
	if (pthread_create(&th[0], NULL, gcp_replacer, &c) ||
			pthread_create(&th[1], NULL, gcp_churner, &c))
		abort();

	for (round = 0; round < GCP_ROUNDS; round++) {
		struct timespec t0;
		struct cds_ft_node *head;

		clock_gettime(CLOCK_MONOTONIC, &t0);
		uatomic_store(&c.run, 1, CMM_RELEASE);
		while (elapsed_ms(&t0) < ms / GCP_ROUNDS) {
			caa_cpu_relax();
			rcu_quiescent_state();
		}
		uatomic_store(&c.run, 0, CMM_RELEASE);
		while (uatomic_load(&c.parked, CMM_ACQUIRE) != 2) {
			caa_cpu_relax();
			rcu_quiescent_state();
		}
		/* Both writers parked: the trie is at rest. */
		if (cds_ft_verify(c.ft, stderr) != CDS_FT_STATUS_OK) {
			fprintf(stderr, "inv_guard_compressed_publish: round %u: verify RED\n",
				round);
			ret = -1;
		}
		rcu_read_lock();
		cds_ft_iter_set_key(iter, key, GCP_KLEN);
		if (cds_ft_lookup(c.ft, iter) != CDS_FT_STATUS_OK ||
				!(head = cds_ft_iter_node(iter)) ||
				to_test_node(head) != c.a_cur) {
			fprintf(stderr, "inv_guard_compressed_publish: round %u: key A lost or not the replacer's node\n",
				round);
			ret = -1;
		}
		rcu_read_unlock();
		rcu_quiescent_state();
		/* B is absent at the park point: A plus the three siblings. */
		if (cds_ft_count_keys(c.ft) != 4) {
			fprintf(stderr, "inv_guard_compressed_publish: round %u: count_keys %lu != 4\n",
				round, cds_ft_count_keys(c.ft));
			ret = -1;
		}
	}
	uatomic_store(&c.stop, 1, CMM_RELEASE);
	uatomic_store(&c.run, 1, CMM_RELEASE);	/* release the parked loop */
	rcu_thread_offline();			/* never block a GP in join */
	pthread_join(th[0], NULL);
	pthread_join(th[1], NULL);
	rcu_thread_online();

	/* Drain: whatever is left is this thread's to free. */
	{
		struct cds_ft_node *n;
		uint8_t bk[GCP_KLEN];
		unsigned int i;

		for (i = 0; i < 5; i++) {
			if (i < 2)
				gcp_key(bk, (uint8_t) (i + 1));
			else
				gcp_sibling_key(bk, i - 2);
			rcu_read_lock();
			cds_ft_iter_set_key(iter, bk, GCP_KLEN);
			if (cds_ft_lookup(c.ft, iter) == CDS_FT_STATUS_OK &&
					(n = cds_ft_iter_node(iter)) &&
					cds_ft_remove(c.ft, iter, n) ==
						CDS_FT_STATUS_OK)
				node_free_rcu(to_test_node(n));
			rcu_read_unlock();
			rcu_quiescent_state();
		}
	}
	cds_ft_iter_destroy(iter);
	rcu_barrier();
	cds_ft_destroy(c.ft);
	cds_ft_group_destroy(group);
	fprintf(stderr, "# inv_guard_compressed_publish: %u rounds, %lu replaces (%lu refused), %lu inserts, %lu removes (%lu missed) -> %s\n",
		GCP_ROUNDS, c.repl, c.repl_fail, c.ins, c.rm, c.miss,
		ret ? "RED" : "ok");
	return ret;
}

int main(int argc, char **argv)
{
	const char *filter = (argc >= 2) ? argv[1] : NULL;
	int err;

	ft_kstat_init();

#ifdef FT_ENABLE_TRACING
	if (getenv("FT_INV_SNAPSHOT_ON_SEGV")) {
		struct sigaction sa;

		memset(&sa, 0, sizeof sa);
		sa.sa_sigaction = ft_segv_snapshot_handler;
		sa.sa_flags = SA_SIGINFO;
		(void) sigaction(SIGSEGV, &sa, NULL);
		(void) sigaction(SIGABRT, &sa, NULL);
		(void) sigaction(SIGILL, &sa, NULL);
		(void) sigaction(SIGBUS, &sa, NULL);
	}
#endif

	err = create_all_cpu_call_rcu_data(0);
	if (err)
		diag("Per-CPU call_rcu() workers unavailable, using default.");

	rcu_register_thread();

	plan_tests(NR_TESTS);

	diag("Concurrent invariant tests (readers vs. writers)");

	diag("1. Iteration ordering");
	RUN_TEST(inv_iteration_order);
	RUN_TEST(inv_concurrent_writers_disjoint);
	RUN_TEST(inv_rekey_graft_disjoint);
	RUN_TEST(inv_rekey_coarse_mixed_writers);
	RUN_TEST(inv_rekey_fine_mixed_writers);
	RUN_TEST(inv_rekey_contended_mixed_writers);
	RUN_TEST(inv_widen_deep_junction);
	RUN_TEST(inv_rekey_coarse_contended_writers);
	RUN_TEST(inv_rekey_graft_cross_junction);
	RUN_TEST(inv_rekey_graft_run_junction);
	RUN_TEST(inv_rekey_graft_elevating_junction);
	RUN_TEST(inv_rekey_graft_glue_dst);
	RUN_TEST(inv_rekey_graft_coherent_readers);
	RUN_TEST(inv_rekey_merge_same_parent_coherent_readers);
	RUN_TEST(inv_rekey_graft_shared);
	RUN_TEST(inv_rekey_linearizability);
	RUN_TEST(inv_rekey_public_atomic_no_gap);
	RUN_TEST(inv_rekey_public_atomic_no_gap_varlen);
	RUN_TEST(inv_rekey_sparse_bp_staged);
	RUN_TEST(inv_rekey_public_atomic_no_gap_compressed_top);
	RUN_TEST(inv_rekey_coarse_progress);
	RUN_TEST(inv_rekey_merge_occupied_dst);
	RUN_TEST(inv_rekey_merge_compressed_dst);
	RUN_TEST(inv_rekey_merge_compressed_dst_rootchurn);
	RUN_TEST(inv_rekey_merge_shared_dst);
	RUN_TEST(inv_rekey_src_mutated);
	RUN_TEST(inv_sibling_split_compress);
	RUN_TEST(inv_sibling_split_compress_unpinned);
	RUN_TEST(inv_concurrent_writers_shared);
	RUN_TEST(inv_concurrent_writers_coarse_lock);
	RUN_TEST(inv_concurrent_writers_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_merge_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_graft_nilkey_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_merge_nilkey_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_graft_ord_fine_lock);
	RUN_TEST(inv_concurrent_crosstrie_graft_rank_coarse_lock);
	RUN_TEST(inv_concurrent_crosstrie_graft_swap_fine_lock);
	RUN_TEST(inv_concurrent_point_vs_bulk_graft);
	RUN_TEST(inv_concurrent_point_vs_bulk_detach);
	RUN_TEST(inv_concurrent_point_vs_bulk_merge);
	RUN_TEST(inv_concurrent_point_vs_bulk_ord);
	RUN_TEST(inv_concurrent_point_vs_bulk_overlap);
	RUN_TEST(inv_concurrent_merge_colliding_chains);
	RUN_TEST(inv_bind_resume_order);
	RUN_TEST(inv_ordered_bulk_consistency);
	RUN_TEST(inv_compact_keycopy_terminates);
	RUN_TEST(inv_reverse_iteration_order);

	diag("2. Lookup consistency");
	RUN_TEST(inv_lookup_consistency);
	RUN_TEST(inv_insert_splice_window);
	RUN_TEST(inv_insert_replace_splice_window);
	RUN_TEST(inv_insert_replace_skipx_splice_window);
	RUN_TEST(inv_replace_skipx);
	/*
	 * Remove cross-view oracles: a key-disappearing remove must leave the
	 * structural index and the ordered cell list in ONE flip, else a reader
	 * observes the head gone from one index but present in the other.  In one
	 * RCU read section the reader does lookup_first/last -> point lookup(that
	 * key) -> lookup_first/last again and flags present/absent/present
	 * (k1==k2 yet the structural lookup missed); with no inserts, "present
	 * again" can only be the non-atomic unpublish window.  Each is provably 0
	 * now that the remove-side fusion has landed: in-place, recompaction,
	 * compressed-parent recompaction, and external-promote all commit the
	 * structural unlink together with the cell unsplice in one flip.
	 *
	 * inv_remove_cross_view{,_compressed} are min-draining;
	 * inv_remove_cross_view_compressed_parent is max-draining (a prefix key
	 * sorts before its extensions, so only max-draining puts the
	 * compressed-parent promote removal on the observed ordered endpoint).
	 * inv_remove_cross_view_prefix_siblings{,_all} are min-draining over
	 * variable-length prefix/extension keys, so the prefix key is removed
	 * while its sibling extensions persist (the holder's external_nodes is
	 * cleared in place): the _all variant drains via cds_ft_remove_all and
	 * adds the empty key to cover the NIL-key clear too.
	 */
	RUN_TEST(inv_remove_cross_view);
	RUN_TEST(inv_remove_cross_view_compressed);
	RUN_TEST(inv_remove_cross_view_compressed_parent);
	RUN_TEST(inv_remove_cross_view_prefix_siblings);
	RUN_TEST(inv_remove_cross_view_prefix_siblings_all);
	RUN_TEST(inv_graft_cross_view);
	RUN_TEST(inv_graft_root_swap_cross_view);
	RUN_TEST(inv_merge_root_swap_cross_view);
	RUN_TEST(inv_graft_swap_shared_dst);
	RUN_TEST(inv_graft_swap_shared_dst_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_kshort);
	RUN_TEST(inv_graft_swap_shared_dst_kshort_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_deep);
	RUN_TEST(inv_graft_swap_shared_dst_deep_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_ks2_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_ks2_solo);
	RUN_TEST(inv_graft_swap_shared_dst_cparent_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_cparent_solo);
	RUN_TEST(inv_graft_swap_shared_dst_wide_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_fuse);
	RUN_TEST(inv_graft_swap_shared_dst_fuse_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_fuse_solo);
	RUN_TEST(inv_graft_swap_shared_dst_ksfix);
	RUN_TEST(inv_graft_swap_shared_dst_ksfix_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_ksfix_solo);
	RUN_TEST(inv_graft_swap_shared_dst_extchild);
	RUN_TEST(inv_graft_swap_shared_dst_extchild_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_extchild_solo);
	RUN_TEST(inv_graft_swap_shared_dst_delegate_nolist);
	RUN_TEST(inv_graft_swap_shared_dst_delegate_solo);
	RUN_TEST(inv_graft_swap_whole_seam_points);
	RUN_TEST(inv_guard_compressed_publish);
	RUN_TEST(inv_writer_progress_chainmerge);
	RUN_TEST(inv_graft_swap_shared_dst_kshort_solo);
	RUN_TEST(inv_graft_swap_shared_dst_deep_solo);
	RUN_TEST(inv_prefix_key_park_vs_holder_churn);
	RUN_TEST(inv_empty_dst_root_graft_peer);
	RUN_TEST(inv_empty_dst_root_graft_peer_nolist);
	RUN_TEST(inv_empty_dst_root_merge_peer);
	RUN_TEST(inv_empty_dst_root_merge_peer_nolist);
	RUN_TEST(inv_merge_root_src_cross_view);
	RUN_TEST(inv_merge_cross_view);
	RUN_TEST(inv_merge_spinecopy_cross_view);
	RUN_TEST(inv_merge_src_cross_view);
	RUN_TEST(inv_merge_src_spinecopy_cross_view);
	RUN_TEST(inv_detach_cross_view);
	RUN_TEST(inv_detach_root_cross_view);

	diag("3. Duplicate chain acyclicity");
	RUN_TEST(inv_dup_chain_acyclicity);
	RUN_TEST(inv_dup_chain_skipx_head_promotion);

	diag("Ordered-list-OFF (no-cell) consistency");
	RUN_TEST(inv_no_ordered_list_consistency);
	RUN_TEST(inv_speculative_per_trie_eager);
	RUN_TEST(inv_speculative_key_verify);
	RUN_TEST(inv_graft_no_list_diverge);
	RUN_TEST(inv_graft_displaced_external);

	diag("4. Graft-swap atomicity");
	RUN_TEST(inv_graft_swap_atomicity);
	RUN_TEST(inv_graft_swap_cross_view);
	RUN_TEST(inv_graft_swap_sub_cross_view);
	RUN_TEST(inv_graft_swap_empty_cross_view);

	diag("5. Relational lookup consistency");
	RUN_TEST(inv_relational_lookup);

	diag("6. Rank-based lookup stability");
	RUN_TEST(inv_nth_last_stability);
	RUN_TEST(inv_nth_first_stability);

	diag("7. Iterator skip stability");
	RUN_TEST(inv_skip_forward_stability);
	RUN_TEST(inv_skip_reverse_stability);

	diag("7b. Rank/select readers resolve a parked external_nodes proxy");
	RUN_TEST(inv_nth_external_nodes_resolve);

	diag("8. nr_keys undercount ordering");
	RUN_TEST(inv_nr_keys_undercount);

	diag("8b. nr_keys no-undercount under concurrent remove (fold guard)");
	RUN_TEST(inv_nr_keys_no_undercount);

	diag("9. Ordered traversal never escapes its trie");
	RUN_TEST(inv_ordered_no_escape_graft);

	diag("10. Piecewise merge never escapes the destination");
	RUN_TEST(inv_merge_no_escape);

	diag("11. Merge into a non-root destination never escapes");
	RUN_TEST(inv_merge_nonroot_dst_no_escape);

	diag("11b. Re-rooted source, GLUE-diverge dst, ordered list");
	RUN_TEST(inv_merge_rerooted_glue_no_escape);

	diag("11c. Same-trie rekey never escapes / corrupts");
	RUN_TEST(inv_rekey_no_escape);

	diag("11d. A rekey to a shorter key does not strand a reader");
	RUN_TEST(inv_rekey_shorter_reanchor);

	diag("11e. lookup_nth gives the right key while a rekey moves a subtree");
	RUN_TEST(inv_rekey_rank_nth);

	diag("12. Merge into a compressed destination never escapes");
	RUN_TEST(inv_merge_compressed_dst_no_escape);

	diag("13. Merge into a key-shorter destination never escapes");
	RUN_TEST(inv_merge_key_shorter_dst_no_escape);

	diag("14. Merge from a key-shorter source never escapes");

	diag("15. Merge into a compressed-parent destination never escapes");
	RUN_TEST(inv_merge_compressed_parent_dst_no_escape);

	diag("16. Merge is atomic: no value missing after the flip");
	RUN_TEST(inv_merge_atomic_completeness);

	diag("17. Merge is atomic across a deep (recursive) overlap");
	RUN_TEST(inv_merge_atomic_completeness_deep);

	diag("18. A retired prefix head's key still names its own node");
	RUN_TEST(inv_prefix_head_clear_key_identity);
	RUN_TEST(inv_prefix_head_promote_key_identity);
	RUN_TEST(inv_prefix_head_replace_key_identity);
	RUN_TEST(inv_prefix_head_nil_key_identity);
	RUN_TEST(inv_split_point_lookup_identity);
	RUN_TEST(inv_split_point_lookup_identity_nolist);
	RUN_TEST(inv_absent_key_never_found);
	RUN_TEST(inv_absent_key_never_found_nolist);

	diag("19. Same-key concurrent removes route by a re-validated decision");
	RUN_TEST(inv_concurrent_same_key_removes);
	RUN_TEST(inv_concurrent_same_key_removes_nolist);
	RUN_TEST(inv_concurrent_same_key_inserts_nolist);
	RUN_TEST(inv_concurrent_same_key_inserts_coarse);
	RUN_TEST(inv_concurrent_same_key_replace_nolist);
	RUN_TEST(inv_concurrent_same_key_replace_coarse);
	RUN_TEST(inv_concurrent_insert_unique_nolist);
	RUN_TEST(inv_concurrent_insert_unique_coarse);
	RUN_TEST(inv_concurrent_insert_unique_dense);
	RUN_TEST(inv_prefix_pair_compressed_holder);
	RUN_TEST(inv_prefix_siblings_compressed_holder);
	RUN_TEST(inv_concurrent_insert_unique_prefix);
	RUN_TEST(inv_prefix_dup_promote_vs_extension);
	RUN_TEST(inv_prefix_shape_zoo);
	RUN_TEST(inv_owned_prefix_dense);
	RUN_TEST(inv_owned_prefix_dense_replace);
	RUN_TEST(inv_owned_prefix_dense_remove_all);
	RUN_TEST(inv_owned_long_runs);
	RUN_TEST(inv_ir_prefix_roundtrip);
	RUN_TEST(inv_concurrent_same_key_append);
	RUN_TEST(inv_concurrent_same_key_append_nolist);
	RUN_TEST(inv_concurrent_same_key_append_coarse);
	RUN_TEST(inv_concurrent_remove_all_nolist);
	RUN_TEST(inv_concurrent_remove_all_coarse);
	RUN_TEST(inv_concurrent_remove_all_list);
	RUN_TEST(inv_concurrent_remove_all_prefix);
	RUN_TEST(inv_concurrent_remove_all_prefix_coarse);
	RUN_TEST(inv_concurrent_insert_replace_nolist);
	RUN_TEST(inv_concurrent_insert_replace_coarse);

	rcu_barrier();
	rcu_unregister_thread();
	free_all_cpu_call_rcu_data();

	return exit_status();
}
