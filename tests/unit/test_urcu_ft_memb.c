// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * Fractal Trie tests on the urcu-memb flavor.
 *
 * The other FT suites use QSBR.  There a read-side section is a no-op, so
 * they cannot see how long the library holds one.  These tests use real
 * read-side sections.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <urcu.h>		/* memb; must precede fractal-trie.h */
#include <urcu/fractal-trie.h>
#include <urcu-call-rcu.h>

#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "tap.h"

#define NR_TESTS	13

#define KEY_LEN		8
/*
 * Each key byte holds two bits of the index (values 0-3).  The trie is then
 * a full 4-ary tree of depth 8: 65536 keys and about 21845 internal nodes,
 * so one compaction pass runs a few hundred steps.
 */
#define NR_KEYS		(1U << (2 * KEY_LEN))
#define NR_READERS	4
#define NR_READER_PASSES 4
/*
 * A pass that holds one read-side section throughout lets at most two
 * grace periods both start and end inside it: one that started just before
 * the section was taken, and one that ends just before the pass reports
 * its end.  Require more than that.
 */
#define MIN_GP_INSIDE	8

struct test_node {
	struct cds_ft_node node;
	struct rcu_head head;
	uint32_t key;
};

static unsigned long nodes_allocated, nodes_freed;

static void index_to_key(uint32_t i, uint8_t *k)
{
	unsigned int j;

	for (j = 0; j < KEY_LEN; j++)
		k[j] = (i >> (2 * (KEY_LEN - 1 - j))) & 3;
}

static void node_free_cb(struct rcu_head *head)
{
	struct test_node *n = caa_container_of(head, struct test_node, head);

	memset(n, 0xfe, sizeof(*n));
	free(n);
	uatomic_inc(&nodes_freed);
}

static uint64_t now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t) ts.tv_sec * 1000000000ULL + ts.tv_nsec;
}

static struct cds_ft *create_ft(enum cds_ft_writer_strategy strategy,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, KEY_LEN) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr, strategy) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static int populate(struct cds_ft *ft)
{
	uint32_t i;

	for (i = 0; i < NR_KEYS; i++) {
		struct test_node *n = calloc(1, sizeof(*n));
		uint8_t k[KEY_LEN];

		if (!n)
			return -1;
		cds_ft_node_init(&n->node);
		n->key = i;
		uatomic_inc(&nodes_allocated);
		index_to_key(i, k);
		if (cds_ft_insert(ft, k, KEY_LEN, &n->node) != CDS_FT_STATUS_OK) {
			free(n);
			return -1;
		}
	}
	return 0;
}

/* Remove every key, free the nodes after a grace period, destroy the trie. */
static int drain_and_destroy(struct cds_ft *ft, struct cds_ft_group *group)
{
	struct cds_ft_iter *iter;
	int ret = 0;

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		return -1;
	rcu_read_lock();
	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp;

		if (cds_ft_remove_all(ft, iter, &head) < 0) {
			ret = -1;
			break;
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp) {
			struct test_node *n =
				caa_container_of(head, struct test_node, node);

			call_rcu(&n->head, node_free_cb);
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	cds_ft_destroy(ft);
	cds_ft_group_destroy(group);
	rcu_barrier();
	if (uatomic_read(&nodes_freed) != uatomic_read(&nodes_allocated)) {
		fprintf(stderr, "leak: %lu allocated, %lu freed\n",
			uatomic_read(&nodes_allocated),
			uatomic_read(&nodes_freed));
		ret = -1;
	}
	return ret;
}

static int verify_all_keys(struct cds_ft *ft)
{
	uint32_t i;
	int ret = 0;

	rcu_read_lock();
	for (i = 0; i < NR_KEYS; i++) {
		struct cds_ft_node *out = NULL;
		uint8_t k[KEY_LEN];

		index_to_key(i, k);
		if (cds_ft_eager_lookup_key(ft, k, KEY_LEN, 0, &out) !=
					CDS_FT_STATUS_OK ||
				caa_container_of(out, struct test_node,
					node)->key != i) {
			fprintf(stderr, "key %" PRIu32 " missing\n", i);
			ret = -1;
			break;
		}
	}
	rcu_read_unlock();
	return ret;
}

/*
 * 0 before the measured call, 1 during it, 2 after it.  The call is made
 * outside any read-side section of the test.
 */
static int op_phase;
static unsigned long gp_inside, gp_total;

static void *gp_thread(void *arg)
{
	(void) arg;
	while (uatomic_read(&op_phase) != 2) {
		int before = uatomic_read(&op_phase);
		int after;

		synchronize_rcu();
		after = uatomic_read(&op_phase);
		uatomic_inc(&gp_total);
		if (before == 1 && after == 1)
			gp_inside++;
	}
	return NULL;
}

/*
 * Grace periods must complete while cds_ft_compact runs.  Each step takes
 * its own read-side section and drops it before the next one.  Nothing may
 * hold a section across the whole pass: that would delay reclaim, every
 * bulk-op gate and every synchronize_rcu caller until the pass ends.
 */
static int test_compact_gp_progress(enum cds_ft_writer_strategy strategy,
		const char *name)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_ft(strategy, &group);
	enum cds_ft_compact_status cs;
	pthread_t gp;
	uint64_t t0, t1;
	int ret = 0;

	if (populate(ft) < 0) {
		fprintf(stderr, "%s: populate failed\n", name);
		ret = -1;
		goto out;
	}
	uatomic_set(&op_phase, 0);
	gp_inside = gp_total = 0;
	if (pthread_create(&gp, NULL, gp_thread, NULL))
		abort();
	/* Let the GP thread start its loop before the pass begins. */
	while (!uatomic_read(&gp_total))
		caa_cpu_relax();
	t0 = now_ns();
	uatomic_set(&op_phase, 1);
	cs = cds_ft_compact(ft);
	uatomic_set(&op_phase, 2);
	t1 = now_ns();
	if (pthread_join(gp, NULL))
		abort();
	diag("%s: pass %" PRIu64 " us, %lu grace periods inside it (%lu total)",
		name, (t1 - t0) / 1000, gp_inside, gp_total);
	if (cs != CDS_FT_COMPACT_DONE) {
		fprintf(stderr, "%s: compaction returned %d\n", name, (int) cs);
		ret = -1;
	}
	if (gp_inside < MIN_GP_INSIDE) {
		fprintf(stderr, "%s: only %lu grace periods completed inside the pass\n",
			name, gp_inside);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	if (verify_all_keys(ft) < 0)
		ret = -1;
out:
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

static struct cds_ft *reader_ft;
static int readers_stop;
static unsigned long reader_misses, reader_lookups;

static void *reader_thread(void *arg)
{
	unsigned int seed = (unsigned int) (uintptr_t) arg;
	unsigned long misses = 0, lookups = 0;

	rcu_register_thread();
	while (!uatomic_read(&readers_stop)) {
		uint32_t i = (uint32_t) rand_r(&seed) % NR_KEYS;
		struct cds_ft_node *out = NULL;
		uint8_t k[KEY_LEN];

		index_to_key(i, k);
		rcu_read_lock();
		if (cds_ft_eager_lookup_key(reader_ft, k, KEY_LEN, 0, &out) !=
					CDS_FT_STATUS_OK ||
				caa_container_of(out, struct test_node,
					node)->key != i)
			misses++;
		rcu_read_unlock();
		lookups++;
	}
	rcu_unregister_thread();
	uatomic_add(&reader_misses, misses);
	uatomic_add(&reader_lookups, lookups);
	return NULL;
}

/*
 * Readers in real read-side sections look up keys while compaction
 * relocates every internal node and frees the old copies after a grace
 * period.  Every lookup must find its key.  Reclaim runs during the
 * passes here, so a node freed while a reader still holds it would show
 * as a miss or a crash.
 */
static int test_compact_memb_readers(enum cds_ft_writer_strategy strategy,
		const char *name)
{
	struct cds_ft_group *group;
	struct cds_ft *ft = create_ft(strategy, &group);
	pthread_t readers[NR_READERS];
	unsigned int r, pass;
	int ret = 0;

	if (populate(ft) < 0) {
		fprintf(stderr, "%s: populate failed\n", name);
		ret = -1;
		goto out;
	}
	reader_ft = ft;
	uatomic_set(&readers_stop, 0);
	reader_misses = reader_lookups = 0;
	for (r = 0; r < NR_READERS; r++)
		if (pthread_create(&readers[r], NULL, reader_thread,
				(void *) (uintptr_t) (r + 1)))
			abort();
	for (pass = 0; pass < NR_READER_PASSES; pass++) {
		enum cds_ft_compact_status cs = cds_ft_compact(ft);

		if (cs != CDS_FT_COMPACT_DONE) {
			fprintf(stderr, "%s: pass %u returned %d\n", name, pass,
				(int) cs);
			ret = -1;
		}
	}
	uatomic_set(&readers_stop, 1);
	for (r = 0; r < NR_READERS; r++)
		if (pthread_join(readers[r], NULL))
			abort();
	diag("%s: %lu lookups, %lu misses", name, reader_lookups,
		reader_misses);
	if (reader_misses || !reader_lookups)
		ret = -1;
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	if (verify_all_keys(ft) < 0)
		ret = -1;
out:
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

/*
 * The rekey tests use a variable-length group.  Every populated key is
 * {0} || index_to_key(i), so src {0} holds all NR_KEYS keys.  One long key
 * {9, 0, ...} of LONG_KEY_LEN bytes lives outside it.
 */
#define MAX_KEY_LEN	255
#define LONG_KEY_LEN	MAX_KEY_LEN
#define VKEY_LEN	(KEY_LEN + 1)
/*
 * The rekey also waits for a grace period at its move gate, and runs a short
 * section per move attempt.  With a walk that holds one section, 4 to 6
 * grace periods still completed inside the rekey in testing.  With the walk
 * yielding, 61 to 69 did.
 */
#define MIN_GP_INSIDE_REKEY	24

static struct cds_ft *create_rekey_ft(enum cds_ft_writer_strategy strategy,
		struct cds_ft_group **group_out)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_group *group;
	struct cds_ft *ft;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, CDS_FT_LEN_VARIABLE) < 0)
		abort();
	if (cds_ft_group_attr_set_max_key_len(attr, MAX_KEY_LEN) < 0)
		abort();
	if (cds_ft_group_attr_set_rekey(attr, true) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr, strategy) < 0)
		abort();
	if (cds_ft_group_create(attr, &group) < 0)
		abort();
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(group, NULL, &ft) < 0)
		abort();
	*group_out = group;
	return ft;
}

static int insert_one(struct cds_ft *ft, const uint8_t *k, size_t len,
		uint32_t tag)
{
	struct test_node *n = calloc(1, sizeof(*n));

	if (!n)
		return -1;
	cds_ft_node_init(&n->node);
	n->key = tag;
	uatomic_inc(&nodes_allocated);
	if (cds_ft_insert(ft, k, len, &n->node) != CDS_FT_STATUS_OK) {
		free(n);
		uatomic_dec(&nodes_allocated);
		return -1;
	}
	return 0;
}

static int populate_under_prefix(struct cds_ft *ft)
{
	uint32_t i;

	for (i = 0; i < NR_KEYS; i++) {
		uint8_t k[VKEY_LEN];

		k[0] = 0;
		index_to_key(i, k + 1);
		if (insert_one(ft, k, VKEY_LEN, i) < 0)
			return -1;
	}
	return 0;
}

static int remove_key(struct cds_ft *ft, const uint8_t *k, size_t len)
{
	struct cds_ft_node *head, *tmp;
	struct cds_ft_iter *iter;
	int ret = 0;

	if (cds_ft_iter_create(ft, &iter) != CDS_FT_STATUS_OK)
		return -1;
	cds_ft_iter_set_key(iter, k, len);
	rcu_read_lock();
	if (cds_ft_lookup(ft, iter) != CDS_FT_STATUS_OK ||
			cds_ft_remove_all(ft, iter, &head) < 0) {
		ret = -1;
	} else {
		cds_ft_for_each_duplicate_safe_rcu(head, tmp) {
			struct test_node *n =
				caa_container_of(head, struct test_node, node);

			call_rcu(&n->head, node_free_cb);
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return ret;
}

/* Every moved key must be found under @dst, with its own index. */
static int verify_moved_keys(struct cds_ft *ft, const uint8_t *dst,
		size_t dst_len)
{
	uint32_t i;
	int ret = 0;

	rcu_read_lock();
	for (i = 0; i < NR_KEYS; i++) {
		struct cds_ft_node *out = NULL;
		uint8_t k[KEY_LEN + 2];

		memcpy(k, dst, dst_len);
		index_to_key(i, k + dst_len);
		if (cds_ft_eager_lookup_key(ft, k, dst_len + KEY_LEN, 0, &out) !=
					CDS_FT_STATUS_OK ||
				caa_container_of(out, struct test_node,
					node)->key != i) {
			fprintf(stderr, "moved key %" PRIu32 " missing\n", i);
			ret = -1;
			break;
		}
	}
	rcu_read_unlock();
	return ret;
}

enum rekey_walk {
	/*
	 * The long key was removed, so the key-length hint is stale.  The
	 * rekey repairs it with a walk of the whole trie
	 * (ft_recompute_max_used_key_len).
	 */
	REKEY_WALK_REPAIR,
	/*
	 * The long key stays and the hint is exact.  The rekey walks the
	 * moved keys to find their real length (ft_moved_keys_max_len).
	 */
	REKEY_WALK_MOVED_KEYS,
};

/*
 * Grace periods must complete while a rekey walks NR_KEYS keys to check
 * their length.  The move {0} -> {7, 7} lengthens every moved key by one
 * byte.  By the hint alone, that would overflow: 255 - 1 > 255 - 2.  So
 * the rekey must walk.  The walk must not hold one read-side section
 * throughout.
 */
static int test_rekey_walk_gp_progress(enum cds_ft_writer_strategy strategy,
		enum rekey_walk walk, const char *name)
{
	static const uint8_t src[] = { 0 }, dst[] = { 7, 7 };
	struct cds_ft_group *group;
	struct cds_ft *ft = create_rekey_ft(strategy, &group);
	uint8_t long_key[LONG_KEY_LEN] = { 9 };
	enum cds_ft_status s;
	pthread_t gp;
	uint64_t t0, t1;
	int ret = 0;

	if (populate_under_prefix(ft) < 0 ||
			insert_one(ft, long_key, LONG_KEY_LEN, UINT32_MAX) < 0) {
		fprintf(stderr, "%s: populate failed\n", name);
		ret = -1;
		goto out;
	}
	if (walk == REKEY_WALK_REPAIR) {
		if (remove_key(ft, long_key, LONG_KEY_LEN) < 0) {
			fprintf(stderr, "%s: remove failed\n", name);
			ret = -1;
			goto out;
		}
	} else {
		/* Make the hint exact, so the rekey skips the repair. */
		rcu_read_lock();
		s = cds_ft_recompute_stats(ft);
		rcu_read_unlock();
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "%s: recompute_stats: %s\n", name,
				cds_ft_status_to_string(s));
			ret = -1;
			goto out;
		}
	}
	if (cds_ft_max_used_key_len(ft) != LONG_KEY_LEN) {
		fprintf(stderr, "%s: hint %zu before the rekey, expected %d\n",
			name, cds_ft_max_used_key_len(ft), LONG_KEY_LEN);
		ret = -1;
		goto out;
	}
	uatomic_set(&op_phase, 0);
	gp_inside = gp_total = 0;
	if (pthread_create(&gp, NULL, gp_thread, NULL))
		abort();
	while (!uatomic_read(&gp_total))
		caa_cpu_relax();
	t0 = now_ns();
	uatomic_set(&op_phase, 1);
	s = cds_ft_rekey_graft(ft, dst, sizeof(dst), src, sizeof(src));
	uatomic_set(&op_phase, 2);
	t1 = now_ns();
	if (pthread_join(gp, NULL))
		abort();
	diag("%s: rekey %" PRIu64 " us, %lu grace periods inside it (%lu total)",
		name, (t1 - t0) / 1000, gp_inside, gp_total);
	if (s != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: rekey: %s\n", name,
			cds_ft_status_to_string(s));
		ret = -1;
		goto out;
	}
	if (gp_inside < MIN_GP_INSIDE_REKEY) {
		fprintf(stderr, "%s: only %lu grace periods completed inside the rekey\n",
			name, gp_inside);
		ret = -1;
	}
	if (walk == REKEY_WALK_REPAIR &&
			cds_ft_max_used_key_len(ft) == LONG_KEY_LEN) {
		fprintf(stderr, "%s: the rekey did not repair the hint\n", name);
		ret = -1;
	}
	if (cds_ft_verify(ft, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	if (verify_moved_keys(ft, dst, sizeof(dst)) < 0)
		ret = -1;
out:
	if (drain_and_destroy(ft, group) < 0)
		ret = -1;
	return ret;
}

/*
 * Grace periods must complete while a graft repairs its source's key-length
 * hint.  The source holds NR_KEYS keys of KEY_LEN bytes, and a removed
 * LONG_KEY_LEN key left its hint stale.  Grafting it at {7, 7} would
 * overflow by the hint alone, so the graft walks the whole source.  No read
 * section may span the graft, or the walk cannot yield.
 */
static int test_graft_walk_gp_progress(enum cds_ft_writer_strategy strategy,
		const char *name)
{
	static const uint8_t dst_key[] = { 7, 7 };
	struct cds_ft_group *group;
	struct cds_ft *dst = create_rekey_ft(strategy, &group);
	uint8_t long_key[LONG_KEY_LEN] = { 9 };
	struct cds_ft *src;
	enum cds_ft_status s;
	pthread_t gp;
	uint64_t t0, t1;
	int ret = 0;

	if (cds_ft_create(group, NULL, &src) < 0)
		abort();
	if (populate(src) < 0 ||
			insert_one(src, long_key, LONG_KEY_LEN, UINT32_MAX) < 0 ||
			remove_key(src, long_key, LONG_KEY_LEN) < 0) {
		fprintf(stderr, "%s: populate failed\n", name);
		ret = -1;
		goto out;
	}
	cds_ft_make_exclusive(src);
	if (cds_ft_max_used_key_len(src) != LONG_KEY_LEN) {
		fprintf(stderr, "%s: hint %zu before the graft, expected %d\n",
			name, cds_ft_max_used_key_len(src), LONG_KEY_LEN);
		ret = -1;
		goto out;
	}
	uatomic_set(&op_phase, 0);
	gp_inside = gp_total = 0;
	if (pthread_create(&gp, NULL, gp_thread, NULL))
		abort();
	while (!uatomic_read(&gp_total))
		caa_cpu_relax();
	t0 = now_ns();
	uatomic_set(&op_phase, 1);
	s = cds_ft_graft(dst, dst_key, sizeof(dst_key), src);
	uatomic_set(&op_phase, 2);
	t1 = now_ns();
	if (pthread_join(gp, NULL))
		abort();
	diag("%s: graft %" PRIu64 " us, %lu grace periods inside it (%lu total)",
		name, (t1 - t0) / 1000, gp_inside, gp_total);
	if (s != CDS_FT_STATUS_OK) {
		fprintf(stderr, "%s: graft: %s\n", name,
			cds_ft_status_to_string(s));
		ret = -1;
		goto out;
	}
	if (gp_inside < MIN_GP_INSIDE_REKEY) {
		fprintf(stderr, "%s: only %lu grace periods completed inside the graft\n",
			name, gp_inside);
		ret = -1;
	}
	if (cds_ft_verify(dst, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	if (verify_moved_keys(dst, dst_key, sizeof(dst_key)) < 0)
		ret = -1;
out:
	cds_ft_destroy(src);
	if (drain_and_destroy(dst, group) < 0)
		ret = -1;
	return ret;
}

/*
 * A cds_ft_graft_swap at an empty key delegates to cds_ft_graft.  A rekey on
 * the same trie runs beside it, so its move gate can own a grace period while
 * the graft's nested gate waits for that grace period.  The delegate must hold
 * no read-side section then, or each thread waits for the other.
 */
#define NR_SWAPS		2000
#define NR_REKEY_PINGPONG	2000
#define HANG_TIMEOUT_S		60

static struct cds_ft *swap_dst;
static int swap_done, rekey_done;
static unsigned long rekey_moves;

static void *rekey_thread(void *arg)
{
	static const uint8_t a[] = { 2 }, b[] = { 3 };
	unsigned int i;
	long ret = 0;

	(void) arg;
	rcu_register_thread();
	for (i = 0; i < NR_REKEY_PINGPONG && !uatomic_read(&swap_done); i++) {
		const uint8_t *from = (i & 1) ? b : a, *to = (i & 1) ? a : b;
		enum cds_ft_status s = cds_ft_rekey_graft(swap_dst, to, 1,
				from, 1);

		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "rekey %u: %s\n", i,
				cds_ft_status_to_string(s));
			ret = -1;
			break;
		}
		uatomic_inc(&rekey_moves);
	}
	rcu_unregister_thread();
	uatomic_set(&rekey_done, 1);
	return (void *) ret;
}

static void *hang_watchdog(void *arg)
{
	unsigned int s;

	(void) arg;
	for (s = 0; s < HANG_TIMEOUT_S * 10; s++) {
		if (uatomic_read(&swap_done) && uatomic_read(&rekey_done))
			return NULL;
		usleep(100000);
	}
	fprintf(stderr, "graft_swap delegate vs rekey: no progress in %d s\n",
		HANG_TIMEOUT_S);
	printf("not ok - graft_swap: a delegate beside a rekey hangs\n");
	fflush(stdout);
	_exit(2);	/* no core file */
}

static int test_graft_swap_delegate_vs_rekey(enum cds_ft_writer_strategy strategy,
		const char *name)
{
	struct cds_ft_group *group;
	pthread_t rk, wd;
	unsigned int i;
	void *rk_ret;
	int ret = 0;

	swap_dst = create_rekey_ft(strategy, &group);
	uatomic_set(&swap_done, 0);
	uatomic_set(&rekey_done, 0);
	uatomic_set(&rekey_moves, 0);
	for (i = 0; i < 16; i++) {
		uint8_t k[2] = { 2, (uint8_t) i };

		if (insert_one(swap_dst, k, sizeof(k), i) < 0)
			abort();
	}
	if (pthread_create(&wd, NULL, hang_watchdog, NULL) ||
			pthread_create(&rk, NULL, rekey_thread, NULL))
		abort();
	for (i = 0; i < NR_SWAPS; i++) {
		uint8_t key[3] = { 5, (uint8_t) i, (uint8_t) (i >> 8) };
		uint8_t sk[1] = { 0 };
		struct cds_ft *swap;
		enum cds_ft_status s;

		if (cds_ft_create(group, NULL, &swap) < 0)
			abort();
		if (insert_one(swap, sk, sizeof(sk), 100000 + i) < 0)
			abort();
		cds_ft_make_exclusive(swap);
		s = cds_ft_graft_swap(swap_dst, key, sizeof(key), swap);
		cds_ft_destroy(swap);
		if (s != CDS_FT_STATUS_OK) {
			fprintf(stderr, "%s: graft_swap %u: %s\n", name, i,
				cds_ft_status_to_string(s));
			ret = -1;
			break;
		}
	}
	uatomic_set(&swap_done, 1);
	if (pthread_join(rk, &rk_ret) || pthread_join(wd, NULL))
		abort();
	if (rk_ret)
		ret = -1;
	diag("%s: %u swaps beside %lu rekeys", name, i,
		uatomic_read(&rekey_moves));
	if (!uatomic_read(&rekey_moves))
		ret = -1;	/* the rekey never ran beside the swaps */
	if (cds_ft_verify(swap_dst, stderr) != CDS_FT_STATUS_OK)
		ret = -1;
	if (drain_and_destroy(swap_dst, group) < 0)
		ret = -1;
	return ret;
}

/*
 * Concurrent inserts of keys of different lengths must leave the key-length
 * hint at the longest one.  The rekey and graft overflow checks trust it, so
 * a hint below a key the trie holds lets a move overrun the key buffers.
 * Each round starts NR_HINT_THREADS inserts at once into a fresh trie.
 */
#define NR_HINT_THREADS	16
#define NR_HINT_ROUNDS	3000

static struct cds_ft *hint_ft;
static unsigned long hint_gen, hint_done;

static void *hint_inserter(void *arg)
{
	unsigned int id = (unsigned int) (uintptr_t) arg;
	unsigned long gen = 0;

	rcu_register_thread();
	for (;;) {
		uint8_t k[NR_HINT_THREADS + 1];
		unsigned long g;

		while ((g = uatomic_read(&hint_gen)) == gen)
			caa_cpu_relax();
		if (g == ULONG_MAX)
			break;
		gen = g;
		cmm_smp_rmb();	/* pairs with the wmb before the gen store */
		/* Key i is i + 1 bytes long; its first byte keeps it apart. */
		memset(k, 0, sizeof(k));
		k[0] = (uint8_t) (id + 1);
		if (insert_one(hint_ft, k, id + 1, id) < 0)
			abort();
		uatomic_inc(&hint_done);	/* full barrier: the insert is done */
	}
	rcu_unregister_thread();
	return NULL;
}

static int test_max_used_key_len_concurrent(void)
{
	pthread_t th[NR_HINT_THREADS];
	unsigned int i, round, low = 0;
	size_t low_seen = 0;

	for (i = 0; i < NR_HINT_THREADS; i++)
		if (pthread_create(&th[i], NULL, hint_inserter,
				(void *) (uintptr_t) i))
			abort();
	for (round = 1; round <= NR_HINT_ROUNDS; round++) {
		struct cds_ft_group *group;
		struct cds_ft *ft = create_rekey_ft(CDS_FT_WRITER_LOCK_FINE,
				&group);
		size_t hint;

		hint_ft = ft;
		uatomic_set(&hint_done, 0);
		cmm_smp_wmb();	/* hint_ft before the gen the workers wait on */
		uatomic_set(&hint_gen, round);
		while (uatomic_read(&hint_done) != NR_HINT_THREADS)
			caa_cpu_relax();
		cmm_smp_mb();
		hint = cds_ft_max_used_key_len(ft);
		if (hint != NR_HINT_THREADS) {
			if (!low++)
				low_seen = hint;
		}
		if (drain_and_destroy(ft, group) < 0)
			abort();
	}
	uatomic_set(&hint_gen, ULONG_MAX);
	for (i = 0; i < NR_HINT_THREADS; i++)
		if (pthread_join(th[i], NULL))
			abort();
	diag("max_used_key_len: %u of %u rounds ended below the longest key "
		"(first seen: %zu, longest %u)", low, NR_HINT_ROUNDS, low_seen,
		NR_HINT_THREADS);
	return low ? -1 : 0;
}

int main(void)
{
	if (create_all_cpu_call_rcu_data(0))
		diag("Per-CPU call_rcu() workers unavailable, using default.");
	rcu_register_thread();
	plan_tests(NR_TESTS);

	ok(test_compact_gp_progress(CDS_FT_WRITER_LOCK_FINE,
			"gp_progress_fine") == 0,
		"compact: grace periods complete during a pass (FINE)");
	ok(test_compact_gp_progress(CDS_FT_WRITER_LOCK_COARSE,
			"gp_progress_coarse") == 0,
		"compact: grace periods complete during a pass (COARSE)");
	ok(test_compact_memb_readers(CDS_FT_WRITER_LOCK_FINE,
			"readers_fine") == 0,
		"compact: memb readers find every key (FINE)");
	ok(test_compact_memb_readers(CDS_FT_WRITER_LOCK_COARSE,
			"readers_coarse") == 0,
		"compact: memb readers find every key (COARSE)");
	ok(test_rekey_walk_gp_progress(CDS_FT_WRITER_LOCK_FINE,
			REKEY_WALK_REPAIR, "rekey_repair_fine") == 0,
		"rekey: grace periods complete during the hint repair walk (FINE)");
	ok(test_rekey_walk_gp_progress(CDS_FT_WRITER_LOCK_COARSE,
			REKEY_WALK_REPAIR, "rekey_repair_coarse") == 0,
		"rekey: grace periods complete during the hint repair walk (COARSE)");
	ok(test_rekey_walk_gp_progress(CDS_FT_WRITER_LOCK_FINE,
			REKEY_WALK_MOVED_KEYS, "rekey_moved_fine") == 0,
		"rekey: grace periods complete during the moved-keys walk (FINE)");
	ok(test_rekey_walk_gp_progress(CDS_FT_WRITER_LOCK_COARSE,
			REKEY_WALK_MOVED_KEYS, "rekey_moved_coarse") == 0,
		"rekey: grace periods complete during the moved-keys walk (COARSE)");
	ok(test_graft_walk_gp_progress(CDS_FT_WRITER_LOCK_FINE,
			"graft_repair_fine") == 0,
		"graft: grace periods complete during the hint repair walk (FINE)");
	ok(test_graft_walk_gp_progress(CDS_FT_WRITER_LOCK_COARSE,
			"graft_repair_coarse") == 0,
		"graft: grace periods complete during the hint repair walk (COARSE)");
	ok(test_graft_swap_delegate_vs_rekey(CDS_FT_WRITER_LOCK_FINE,
			"swap_vs_rekey_fine") == 0,
		"graft_swap: a delegate beside a rekey completes (FINE)");
	ok(test_graft_swap_delegate_vs_rekey(CDS_FT_WRITER_LOCK_COARSE,
			"swap_vs_rekey_coarse") == 0,
		"graft_swap: a delegate beside a rekey completes (COARSE)");
	ok(test_max_used_key_len_concurrent() == 0,
		"insert: concurrent inserts keep max_used_key_len at the longest key");

	rcu_unregister_thread();
	return exit_status();
}
