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
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tap.h"

#define NR_TESTS	4

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
 * 0 before the pass, 1 during it, 2 after it.  The pass is
 * cds_ft_compact: it is not inside any read-side section of the test.
 */
static int compact_phase;
static unsigned long gp_inside, gp_total;

static void *gp_thread(void *arg)
{
	(void) arg;
	while (uatomic_read(&compact_phase) != 2) {
		int before = uatomic_read(&compact_phase);
		int after;

		synchronize_rcu();
		after = uatomic_read(&compact_phase);
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
	uatomic_set(&compact_phase, 0);
	gp_inside = gp_total = 0;
	if (pthread_create(&gp, NULL, gp_thread, NULL))
		abort();
	/* Let the GP thread start its loop before the pass begins. */
	while (!uatomic_read(&gp_total))
		caa_cpu_relax();
	t0 = now_ns();
	uatomic_set(&compact_phase, 1);
	cs = cds_ft_compact(ft);
	uatomic_set(&compact_phase, 2);
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

	rcu_unregister_thread();
	return exit_status();
}
