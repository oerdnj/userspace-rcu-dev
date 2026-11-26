// SPDX-FileCopyrightText: 2009-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * test_urcu_ft.c
 *
 * Userspace RCU library - Fractal Trie Test Program
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "test_urcu_ft.h"
#include "debug-yield.h"
#include <inttypes.h>
#include <stdint.h>
#include <endian.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <getopt.h>

#define key_len_split(var)	var, sizeof(var)

DEFINE_URCU_TLS(unsigned int, rand_lookup);
DEFINE_URCU_TLS(unsigned long, nr_insert);
DEFINE_URCU_TLS(unsigned long, nr_insertexist);
DEFINE_URCU_TLS(unsigned long, nr_remove);
DEFINE_URCU_TLS(unsigned long, nr_removenoent);
DEFINE_URCU_TLS(unsigned long, lookup_fail);
DEFINE_URCU_TLS(unsigned long, lookup_ok);

static struct cds_ft *test_ft;
static struct cds_ft_group *test_ft_group;
/* Provide mutual exclusion across Fractal Trie updates. */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

int test_go, test_stop;

unsigned long wdelay;

unsigned long duration;

/* read-side C.S. duration, in loops */
unsigned long rduration;

unsigned long init_populate;
int insert_only;

unsigned long init_pool_offset, lookup_pool_offset, write_pool_offset;
unsigned long init_pool_size = DEFAULT_RAND_POOL,
	lookup_pool_size = DEFAULT_RAND_POOL,
	write_pool_size = DEFAULT_RAND_POOL;
int validate_lookup;
int sanity_test, sanity_test_varlen, sanity_test_varlen_string, test_dictionary, reverse_sort, torture_test_string;
unsigned int key_len = 4;

/*
 * Verify-at-mutation sampling period override.  When the
 * --verify-at-mutation-period CLI flag is given,
 * verify_at_mutation_period_set is true and verify_at_mutation_period
 * holds the value that should be applied to every trie this test
 * creates (via cds_ft_verify_at_mutation_period_set).  Without VAM
 * compiled into the library, the setter returns NOT_SUPPORTED and we
 * abort early so the mismatch is loud.
 */
static bool verify_at_mutation_period_set = false;
static unsigned long verify_at_mutation_period = 0;

static
void test_apply_verify_period(struct cds_ft *ft)
{
	enum cds_ft_status s;

	if (!verify_at_mutation_period_set)
		return;
	s = cds_ft_verify_at_mutation_period_set(ft,
			verify_at_mutation_period);
	if (s != CDS_FT_STATUS_OK) {
		fprintf(stderr,
			"--verify-at-mutation-period requested but the library was built without FEATURE_FT_VERIFY_AT_MUTATION; aborting.\n");
		exit(1);
	}
}

int count_pipe[2];

int verbose_mode;

unsigned int cpu_affinities[NR_CPUS];
unsigned int next_aff = 0;
int use_affinity = 0;

pthread_mutex_t affinity_mutex = PTHREAD_MUTEX_INITIALIZER;

DEFINE_URCU_TLS(unsigned long long, nr_writes);
DEFINE_URCU_TLS(unsigned long long, nr_reads);

unsigned int nr_readers;
static enum cds_ft_writer_strategy writer_strategy = CDS_FT_WRITER_LOCK_FINE;
unsigned int nr_writers;

static unsigned int insert_ratio = 50;
static uint64_t key_mul = 1ULL;

static int insert_unique, insert_replace;

static int leak_detection, show_stats;
static unsigned long test_nodes_allocated, test_nodes_freed;

static void set_affinity(void)
{
#ifdef HAVE_SCHED_SETAFFINITY
	cpu_set_t mask;
	int cpu, ret;
#endif /* HAVE_SCHED_SETAFFINITY */

	if (!use_affinity)
		return;

#if HAVE_SCHED_SETAFFINITY
	ret = pthread_mutex_lock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex lock");
		exit(-1);
	}
	cpu = cpu_affinities[next_aff++];
	ret = pthread_mutex_unlock(&affinity_mutex);
	if (ret) {
		perror("Error in pthread mutex unlock");
		exit(-1);
	}

	CPU_ZERO(&mask);
	CPU_SET(cpu, &mask);
        sched_setaffinity(0, sizeof(mask), &mask);
#endif /* HAVE_SCHED_SETAFFINITY */
}

/*
 * The writer mutex is the APPLICATION's exclusion, so it exists only for the
 * strategy whose contract asks the application for it: external-sync.  Under
 * fine or coarse the trie excludes its own writers, and taking this mutex
 * around every insert and remove serialized them in the HARNESS -- every
 * multi-writer throughput this tool reported for those strategies measured
 * one writer at a time plus a futex hand-off (64% of the writers' samples in
 * futex wait/wake at 4 in-place writers), never the trie's own locking.
 */
static
bool mutex_mt_needed(void)
{
	return nr_writers > 1 &&
		writer_strategy == CDS_FT_WRITER_EXTERNAL_SYNC;
}

static
void mutex_lock_mt(void)
{
	if (!mutex_mt_needed())
		return;
	if (pthread_mutex_lock(&lock)) {
		perror("Error in pthread mutex lock");
		abort();
	}
}

static
void mutex_unlock_mt(void)
{
	if (!mutex_mt_needed())
		return;
	if (pthread_mutex_unlock(&lock)) {
		perror("Error in pthread mutex unlock");
		abort();
	}
}

static
struct ft_test_node *node_alloc(void)
{
	struct ft_test_node *node;

	node = calloc(1, sizeof(*node));
	if (leak_detection && node)
		uatomic_inc(&test_nodes_allocated);
	return node;
}

static
void free_test_node(struct ft_test_node *node)
{
	poison_free(node);
	if (leak_detection)
		uatomic_inc(&test_nodes_freed);
}

static
void free_test_node_cb(struct rcu_head *head)
{
	struct ft_test_node *node =
		caa_container_of(head, struct ft_test_node, head);
	free_test_node(node);
}

static
void rcu_free_test_node(struct ft_test_node *test_node)
{
	call_rcu(&test_node->head, free_test_node_cb);
}

static
void free_node(struct cds_ft_node *node)
{
	struct ft_test_node *test_node = to_test_node(node);

	free_test_node(test_node);
}

static
void show_usage(char **argv)
{
	printf("Usage : %s nr_readers nr_writers duration (s) [options]\n", argv[0]);
	printf("\n");
	printf("Options:\n");
	printf("\n");
#ifdef DEBUG_YIELD
	printf("  --yield-reader                Yield reader.\n");
	printf("  --yield-writer                Yield writer.\n");
	printf("\n");
#endif
	printf("  -d, --writer-delay <us>       Writer period (us).\n");
	printf("  -c, --reader-duration <n>     Reader C.S. duration (in loops).\n");
	printf("  -v, --verbose                 Verbose output.\n");
	printf("  -a, --affinity <cpu#>         Set CPU affinity (use multiple times).\n");
	printf("\n");
	printf("  -u, --insert-unique           Add unique keys.\n");
	printf("  -s, --insert-replace          Replace existing keys.\n");
	printf("      [neither -u nor -s]       Add entries (supports redundant keys).\n");
	printf("  -r, --insert-ratio <%%>        Insert ratio (in %% of insert+removal).\n");
	printf("\n");
	printf("  -k, --populate                Populate init nodes.\n");
	printf("  -R, --lookup-pool-offset <n>  Lookup pool offset.\n");
	printf("  -S, --write-pool-offset <n>   Write pool offset.\n");
	printf("  -T, --init-pool-offset <n>    Init pool offset.\n");
	printf("  -M, --lookup-pool-size <n>    Lookup pool size.\n");
	printf("  -N, --write-pool-size <n>     Write pool size.\n");
	printf("  -O, --init-pool-size <n>      Init pool size.\n");
	printf("\n");
	printf("  -V, --validate-lookup         Validate lookups of init values (use with\n");
	printf("                                filled init pool, same lookup range, with\n");
	printf("                                different write range).\n");
	printf("\n");
	printf("  -t, --sanity-test             Run sanity test.\n");
	printf("  -x, --sanity-test-varlen      Run variable length sanity test.\n");
	printf("  -y, --sanity-test-varlen-string\n");
	printf("                                Run variable length string sanity test.\n");
	printf("\n");
	printf("  -B, --key-len <bytes>         Key bytes for multithread test (default: 4).\n");
	printf("  -m, --key-mul <factor>        Key multiplication factor.\n");
	printf("\n");
	printf("  -l, --leak-detection          Memory leak detection.\n");
	printf("  -Z, --show-stats              Show statistics.\n");
	printf("  -D, --dictionary              Dictionary (stdin) test.\n");
	printf("  -q, --reverse-sort            Reverse sort dictionary.\n");
	printf("  -o, --torture-test-string     Torture test strings.\n");
	printf("\n");
	printf("      --verify-at-mutation-period <n>\n");
	printf("                                Verify-at-mutation sampling period\n");
	printf("                                (requires -DFEATURE_FT_VERIFY_AT_MUTATION):\n");
	printf("                                0 disables, 1 verifies every mutation,\n");
	printf("                                N>1 verifies once every N mutations.\n");
	printf("\n");
}

static
int test_free_all_nodes(struct cds_ft *ft)
{
	struct cds_ft_iter *iter;
	enum cds_ft_status status;

	status = cds_ft_iter_create(ft, &iter);
	if (status < 0) {
		fprintf(stderr, "Error creating iterator: %s\n",
			cds_ft_status_to_string(status));
		return -1;
	}

	rcu_read_lock();

	while (cds_ft_lookup_first(ft, iter) == CDS_FT_STATUS_OK) {
		struct cds_ft_node *head, *tmp_node;

		status = cds_ft_remove_all(ft, iter, &head);
		if (status) {
			uint8_t key[256];
			size_t entry_key_len;

			cds_ft_iter_get_key(iter, key, sizeof(key), &entry_key_len);
			fprintf(stderr, "Error removing node %" PRIu64 ": %s\n",
				cds_ft_key_to_u64(ft, key, entry_key_len), cds_ft_status_to_string(status));
			goto end;
		}
		cds_ft_for_each_duplicate_safe_rcu(head, tmp_node) {
			/* Alone using Fractal Trie, OK to free now */
			free_node(head);
		}
	}
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return 0;

end:
	rcu_read_unlock();
	cds_ft_iter_destroy(iter);
	return -1;
}

static
int test_1byte_key(void)
{
	enum cds_ft_status status;
	size_t i;
	uint64_t key;
	uint64_t ka[] = { 5, 17, 100, 222 };
	uint64_t ka_test_offset = 5;
	struct cds_ft_node *ft_node;
	struct cds_ft_group_attr *attr;
	struct cds_ft_iter *iter;
	uint8_t ftkey[1];

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 1) < 0)
		abort();

	/* Test with 1-byte key */
	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	/* Add keys */
	printf("Test #1: insert keys (1-byte).\n");
	for (key = 0; key < 200; key++) {
		struct ft_test_node *node = node_alloc();

		ft_test_node_init(node, key);
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_insert(test_ft, key_len_split(ftkey), &node->node);
		rcu_read_unlock();
		if (status) {
			fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
	}
	printf("OK\n");

	printf("Test #2: successful key lookup (1-byte).\n");
	for (key = 0; key < 200; key++) {
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #3: unsuccessful key lookup (1-byte).\n");
	for (key = 200; key < 240; key++) {
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr,
				"Error unexpected lookup node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #4: remove keys (1-byte).\n");
	for (key = 0; key < 200; key++) {
		struct ft_test_node *node;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		node = caa_container_of(ft_node, struct ft_test_node, node);
		status = cds_ft_remove(test_ft, iter, &node->node);
		if (status) {
			fprintf(stderr, "Error removing node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_free_test_node(node);
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after remove) failed. Node is not expected: %s\n", key, ft_node, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	rcu_quiescent_state();

	printf("Test #5: lookup lower/greater equal (1-byte).\n");

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct ft_test_node *node = node_alloc();

		key = ka[i];
		ft_test_node_init(node, key);
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_insert(test_ft, key_len_split(ftkey), &node->node);
		rcu_read_unlock();
		if (status) {
			fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i] + ka_test_offset;
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_le(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup lower equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup lower equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i] - ka_test_offset;
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_ge(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup greater equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup greater equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i];	/* without offset */
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_le(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup lower equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup lower equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}

		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_ge(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup greater equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup greater equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	printf("OK\n");
	rcu_quiescent_state();

	cds_ft_iter_destroy(iter);

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}
	rcu_quiescent_state();

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);
	return 0;
}

static
int test_2bytes_key(void)
{
	enum cds_ft_status status;
	size_t i;
	uint64_t key;
	uint64_t ka[] = { 105, 206, 4000, 4111, 59990, 65435 };
	uint64_t ka_test_offset = 100;
	struct cds_ft_group_attr *attr;
	struct cds_ft_iter *iter;
	uint8_t ftkey[2];

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, 2) < 0)
		abort();

	/* Test with 2-bytes key */
	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	/* Add keys */
	printf("Test #1: insert keys (2-byes).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct ft_test_node *node = node_alloc();

		ft_test_node_init(node, key);
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_insert(test_ft, key_len_split(ftkey), &node->node);
		rcu_read_unlock();
		if (status) {
			fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
	}
	printf("OK\n");

	printf("Test #2: successful key lookup (2-byte).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct cds_ft_node *ft_node;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #3: unsuccessful key lookup (2-byte).\n");
	for (key = 11000; key <= 11002; key++) {
		struct cds_ft_node *ft_node;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr,
				"Error unexpected lookup node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	printf("Test #4: remove keys (2-byte).\n");
	for (key = 0; key < 10000; key++) {
	//for (key = 0; key < 65536; key+=256) {
		struct cds_ft_node *ft_node;
		struct ft_test_node *node;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		node = caa_container_of(ft_node, struct ft_test_node, node);
		status = cds_ft_remove(test_ft, iter, &node->node);
		if (status) {
			fprintf(stderr, "Error removing node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_free_test_node(node);
		status = cds_ft_eager_lookup_key(test_ft, ftkey, sizeof(ftkey), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after remove) failed. Node is not expected: %s\n", key, ft_node, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	rcu_quiescent_state();

	printf("Test #5: lookup lower/greater equal (2-byte).\n");

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct ft_test_node *node = node_alloc();

		key = ka[i];
		ft_test_node_init(node, key);
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		status = cds_ft_insert(test_ft, key_len_split(ftkey), &node->node);
		rcu_read_unlock();
		if (status) {
			fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct cds_ft_node *ft_node;
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i] + ka_test_offset;
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_le(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup lower equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup lower equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct cds_ft_node *ft_node;
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i] - ka_test_offset;
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_ge(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup greater equal. Cannot find expected key %" PRIu64" greater or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup greater equal. Expecting key %" PRIu64 " greater or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	for (i = 0; i < CAA_ARRAY_SIZE(ka); i++) {
		struct cds_ft_node *ft_node;
		struct ft_test_node *node;
		uint8_t result_key[8];
		size_t result_key_len;

		key = ka[i];	/* without offset */
		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, key_len_split(ftkey));
		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_le(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup lower equal. Cannot find expected key %" PRIu64" lower or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup lower equal. Expecting key %" PRIu64 " lower or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}

		cds_ft_iter_set_key(iter, key_len_split(ftkey));
		status = cds_ft_lookup_ge(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);
		if (!ft_node) {
			fprintf(stderr, "Error lookup greater equal. Cannot find expected key %" PRIu64" greater or equal to %" PRIu64 ": %s\n",
				ka[i], key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_iter_get_key(iter, result_key, sizeof(result_key), &result_key_len);
		node = caa_container_of(ft_node, struct ft_test_node, node);
		if (node->key != ka[i] || cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT) != ka[i]) {
			fprintf(stderr, "Error lookup greater equal. Expecting key %" PRIu64 " greater or equal to %" PRIu64 ", but found %" PRIu64 "/%" PRIu64" instead: %s\n",
				ka[i], key, node->key, cds_ft_key_to_u64(test_ft, result_key, CDS_FT_LEN_DEFAULT), cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}

	printf("OK\n");
	rcu_quiescent_state();

	cds_ft_iter_destroy(iter);

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}
	rcu_quiescent_state();

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);
	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_sparse_key(unsigned int len, int nr_dup)
{
	uint64_t key, max_key;
	int zerocount, i;
	enum cds_ft_status status;
	struct cds_ft_node *ft_node;
	unsigned int bits = len * CHAR_BIT;
	struct cds_ft_group_attr *attr;
	struct cds_ft_iter *iter;

	if (len == 8)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, len) < 0)
		abort();

	printf("Sparse key test begins for %u-byte keys\n", len);
	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	/* Add keys */
	printf("Test #1: insert keys (%u-byte).\n", len);
	for (i = 0; i < nr_dup; i++) {
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			struct ft_test_node *node = node_alloc();
			uint8_t ftkey[8];

			ft_test_node_init(node, key);
			rcu_read_lock();
			cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
			status = cds_ft_insert(test_ft, ftkey, CDS_FT_LEN_DEFAULT, &node->node);
			rcu_read_unlock();
			if (status) {
				fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
					key, cds_ft_status_to_string(status));
				assert(0);
			}
			if (key == 0)
				zerocount++;
		}
	}
	printf("OK\n");

	if (show_stats)
		cds_ft_show_stats(test_ft, stderr);

	printf("Test #2: successful key lookup (%u-byte).\n", len);
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		uint8_t ftkey[8];
		int count = 0;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
		status = cds_ft_eager_lookup_key(test_ft, ftkey, CDS_FT_LEN_DEFAULT, 0, &ft_node);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_for_each_duplicate_rcu(ft_node) {
			count++;
		}
		if (count != nr_dup) {
			fprintf(stderr, "Unexpected number of match for key %" PRIu64 ", expected %d, got %d.\n", key, nr_dup, count);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");
	if (len > 1) {
		printf("Test #3: unsuccessful key lookup (%u-byte).\n", len);
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			uint8_t ftkey[8];

			rcu_read_lock();
			cds_ft_u64_to_key(test_ft, key + 42, ftkey, CDS_FT_LEN_DEFAULT);
			status = cds_ft_eager_lookup_key(test_ft, ftkey, CDS_FT_LEN_DEFAULT, 0, &ft_node);
			if (ft_node) {
				fprintf(stderr,
					"Error unexpected lookup node %" PRIu64 ": %s\n",
					key + 42, cds_ft_status_to_string(status));
				assert(0);
			}
			rcu_read_unlock();
			if (key == 0)
				zerocount++;
		}
		printf("OK\n");
	}
	printf("Test #4: remove keys (%u-byte).\n", len);
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		uint8_t ftkey[8];
		int count = 0;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
		cds_ft_iter_set_key(iter, ftkey, CDS_FT_LEN_DEFAULT);
		status = cds_ft_lookup(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);

		cds_ft_for_each_duplicate_rcu(ft_node) {
			struct cds_ft_node *test_ft_node;
			struct ft_test_node *node;

			count++;
			node = caa_container_of(ft_node,
				struct ft_test_node, node);
			status = cds_ft_remove(test_ft, iter, &node->node);
			if (status) {
				fprintf(stderr, "Error removing node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
				assert(0);
			}
			rcu_free_test_node(node);
			status = cds_ft_eager_lookup_key(test_ft, ftkey, CDS_FT_LEN_DEFAULT, 0, &test_ft_node);
			if (count < nr_dup && !test_ft_node) {
				fprintf(stderr, "Error no node found after removal of some nodes of a key: %s\n", cds_ft_status_to_string(status));
				assert(0);
			}
		}
		status = cds_ft_eager_lookup_key(test_ft, ftkey, CDS_FT_LEN_DEFAULT, 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after remove) failed. Node is not expected: %s\n", key, ft_node, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");
	rcu_quiescent_state();

	cds_ft_iter_destroy(iter);

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}
	rcu_quiescent_state();

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);
	printf("Test ends\n");

	return 0;
}

static
int do_sanity_test(void)
{
	int i, j, ret;

	printf("Sanity test start.\n");

	for (i = 0; i < 3; i++) {
		ret = test_1byte_key();
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}
	ret = test_2bytes_key();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	/* key length (bytes) */
	for (i = 1; i <= 8; i *= 2) {
		/* nr of nodes per key */
		for (j = 1; j < 4; j++) {
			ret = test_sparse_key(i, j);
			if (ret) {
				return ret;
			}
			rcu_quiescent_state();
		}
	}
	printf("Sanity test end.\n");

	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_varlen_sparse_key_insert(unsigned int len, int nr_dup)
{
	uint64_t key, max_key;
	int zerocount, i;
	enum cds_ft_status status;
	unsigned int bits = len * CHAR_BIT;

	if (len == 8)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	/* Add keys */
	printf("Test #1: insert keys (%u-byte).\n", len);
	for (i = 0; i < nr_dup; i++) {
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			struct ft_test_node *node = node_alloc();
			uint8_t ftkey[8];

			ft_test_node_init(node, key);
			rcu_read_lock();
			cds_ft_u64_to_key(test_ft, key, ftkey, len);
			status = cds_ft_insert(test_ft, ftkey, len, &node->node);
			rcu_read_unlock();
			if (status) {
				fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
					key, cds_ft_status_to_string(status));
				assert(0);
			}
			if (key == 0)
				zerocount++;
		}
	}
	printf("OK\n");
	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_varlen_sparse_key_lookup(unsigned int len, int nr_dup)
{
	uint64_t key, max_key;
	unsigned int bits = len * CHAR_BIT;
	int zerocount;

	if (len == 8)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	printf("Test #2: successful key lookup (%u-byte).\n", len);
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		struct cds_ft_node *ft_node;
		enum cds_ft_status status;
		uint8_t ftkey[8];
		int count = 0;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, ftkey, len);
		status = cds_ft_eager_lookup_key(test_ft, ftkey, len, 0, &ft_node);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_for_each_duplicate_rcu(ft_node) {
			count++;
		}
		if (count != nr_dup) {
			fprintf(stderr, "Unexpected number of match for key %" PRIu64 ", expected %d, got %d.\n", key, nr_dup, count);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");
	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_varlen_sparse_key_lookup_fail(unsigned int len)
{
	uint64_t key, max_key;
	int zerocount;
	unsigned int bits = len * CHAR_BIT;

	if (len == 8)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	if (len > 1) {
		printf("Test #3: unsuccessful key lookup (%u-byte).\n", len);
		zerocount = 0;
		for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
			struct cds_ft_node *ft_node;
			enum cds_ft_status status;
			uint8_t ftkey[8];

			rcu_read_lock();
			cds_ft_u64_to_key(test_ft, key + 42, ftkey, len);
			status = cds_ft_eager_lookup_key(test_ft, ftkey, len, 0, &ft_node);
			if (ft_node) {
				fprintf(stderr,
					"Error unexpected lookup node %" PRIu64 ": %s\n",
					key + 42, cds_ft_status_to_string(status));
				assert(0);
			}
			rcu_read_unlock();
			if (key == 0)
				zerocount++;
		}
		printf("OK\n");
	}
	return 0;
}

/*
 * nr_dup is number of nodes per key.
 */
static
int test_varlen_sparse_key_remove(unsigned int len, int nr_dup)
{
	uint64_t key, max_key;
	int zerocount;
	enum cds_ft_status status;
	struct cds_ft_node *ft_node;
	unsigned int bits = len * CHAR_BIT;
	struct cds_ft_iter *iter;

	if (len == 8)
		max_key = UINT64_MAX;
	else
		max_key = (1ULL << bits) - 1;

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	printf("Test #4: remove keys (%u-byte).\n", len);
	zerocount = 0;
	for (key = 0; key <= max_key && (key != 0 || zerocount < 1); key += 1ULL << (bits - 8)) {
		uint8_t ftkey[8];
		int count = 0;

		rcu_read_lock();
		cds_ft_u64_to_key(test_ft, key, ftkey, len);
		cds_ft_iter_set_key(iter, ftkey, len);
		status = cds_ft_lookup(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);

		cds_ft_for_each_duplicate_rcu(ft_node) {
			struct cds_ft_node *test_ft_node;
			struct ft_test_node *node;

			count++;
			node = caa_container_of(ft_node,
				struct ft_test_node, node);
			status = cds_ft_remove(test_ft, iter, &node->node);
			if (status) {
				fprintf(stderr, "Error removing node %" PRIu64 ": %s\n", key, cds_ft_status_to_string(status));
				assert(0);
			}
			rcu_free_test_node(node);
			status = cds_ft_eager_lookup_key(test_ft, ftkey, len, 0, &test_ft_node);
			if (count < nr_dup && !test_ft_node) {
				fprintf(stderr, "Error no node found after removal of some nodes of a key: %s\n", cds_ft_status_to_string(status));
				assert(0);
			}
		}
		status = cds_ft_eager_lookup_key(test_ft, ftkey, len, 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error lookup %" PRIu64 ": %p (after remove) failed. Node is not expected: %s\n", key, ft_node, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
		if (key == 0)
			zerocount++;
	}
	printf("OK\n");

	cds_ft_iter_destroy(iter);
	return 0;
}

static
int do_sanity_test_varlen_dup(int nr_dup)
{
	int i, ret;
	struct cds_ft_group_attr *attr;

	printf("Variable length key sanity test start.\n");

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	/* Use variable length keys (default). */

	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	/* key length (bytes) */
	for (i = 1; i <= 8; i *= 2) {
		ret = test_varlen_sparse_key_insert(i, nr_dup);
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}

	if (show_stats)
		cds_ft_show_stats(test_ft, stderr);

	/* key length (bytes) */
	for (i = 1; i <= 8; i *= 2) {
		ret = test_varlen_sparse_key_lookup(i, nr_dup);
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}


	/* key length (bytes) */
	for (i = 1; i <= 8; i *= 2) {
		ret = test_varlen_sparse_key_lookup_fail(i);
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}


	/* key length (bytes) */
	for (i = 1; i <= 8; i *= 2) {
		ret = test_varlen_sparse_key_remove(i, nr_dup);
		if (ret) {
			return ret;
		}
		rcu_quiescent_state();
	}

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);
	printf("Sanity test ends\n");

	return 0;
}

static
int do_sanity_test_varlen(void)
{
	int nr_dup;

	for (nr_dup = 1; nr_dup < 4; nr_dup++) {
		if (do_sanity_test_varlen_dup(nr_dup))
			return -1;
	}
	return 0;
}

static const char *test_strings[] = {
	"abc",
	"abcd",
	"eee",
	"eef",
	"eeeeeeeeeee",
	"zzz",
	"zz1",
	"z",
	"y",
	"yy1",
	"yyy",
	"abcd",	/* twice */
	"a",
	"x",
	"ffffffffffffffffffffffffffffffffffffffffffffff",
};

static const char *fail_strings[] = {
	"abc!",
	"abcd!",
	"eee!",
	"eef!",
	"eeeeeeeeeee!",
	"zzzzzzzzzzz!",
	"zzzzzzzzzz1!",
	"zzzzzzzzz!",
	"!abcd",
	"a!",
	"x!",
	"ffffffffffffffffffffffffffffffffffffffffffffff!",
};

static
int test_varlen_string_key_insert(void)
{
	unsigned int i;

	/* Add keys */
	printf("Test #1: insert string keys.\n");
	for (i = 0; i < CAA_ARRAY_SIZE(test_strings); i++) {
		const char *string = test_strings[i];
		struct ft_test_node *node = node_alloc();
		enum cds_ft_status status;

		ft_test_node_init(node, 0);
		rcu_read_lock();
		status = cds_ft_insert(test_ft, (uint8_t *) string, strlen(string), &node->node);
		rcu_read_unlock();
		if (status) {
			fprintf(stderr, "Error inserting node \"%s\": %s\n",
				string, cds_ft_status_to_string(status));
			assert(0);
		}
	}
	printf("OK\n");
	return 0;
}

static
int test_varlen_string_key_lookup(void)
{
	unsigned int i;

	printf("Test #2: successful string key lookup.\n");

	for (i = 0; i < CAA_ARRAY_SIZE(test_strings); i++) {
		const char *string = test_strings[i];
		struct cds_ft_node *ft_node;
		enum cds_ft_status status;
		int count = 0;

		rcu_read_lock();
		status = cds_ft_eager_lookup_key(test_ft, (uint8_t *) string, strlen(string), 0, &ft_node);
		if (!ft_node) {
			fprintf(stderr, "Error lookup node \"%s\": %s\n", string, cds_ft_status_to_string(status));
			assert(0);
		}
		cds_ft_for_each_duplicate_rcu(ft_node) {
			count++;
		}
		if (count > 1 && strcmp(string, "abcd") != 0) {
			fprintf(stderr, "Unexpected number of match for key \"%s\", expected %d, got %d.\n", string, 2, count);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	return 0;
}

static
int test_varlen_string_key_lookup_fail(void)
{
	unsigned int i;

	printf("Test #2: fail string key lookup.\n");

	for (i = 0; i < CAA_ARRAY_SIZE(fail_strings); i++) {
		const char *string = fail_strings[i];
		struct cds_ft_node *ft_node;
		enum cds_ft_status status;

		rcu_read_lock();
		status = cds_ft_eager_lookup_key(test_ft, (uint8_t *) string, strlen(string), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error unexpected lookup node \"%s\": %s\n", string, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");
	return 0;
}

static
int test_varlen_string_key_remove(void)
{
	unsigned int i;
	struct cds_ft_iter *iter;

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	printf("Test #4: remove string keys.\n");

	for (i = 0; i < CAA_ARRAY_SIZE(test_strings); i++) {
		const char *string = test_strings[i];
		struct cds_ft_node *ft_node;
		int count = 0;
		enum cds_ft_status status;

		rcu_read_lock();
		cds_ft_iter_set_key(iter, (uint8_t *) string, strlen(string));
		cds_ft_lookup(test_ft, iter);
		ft_node = cds_ft_iter_node(iter);

		cds_ft_for_each_duplicate_rcu(ft_node) {
			struct cds_ft_node *test_ft_node;
			struct ft_test_node *node;

			count++;
			node = caa_container_of(ft_node,
				struct ft_test_node, node);
			status = cds_ft_remove(test_ft, iter, &node->node);
			if (status) {
				fprintf(stderr, "Error removing node \"%s\": %s\n", string, cds_ft_status_to_string(status));
				assert(0);
			}
			rcu_free_test_node(node);
			status = cds_ft_eager_lookup_key(test_ft, (uint8_t *) string, strlen(string), 0, &test_ft_node);
			if (count < 2 && strcmp(string, "abcd") == 0 && !test_ft_node) {
				fprintf(stderr, "Error no node found after removal of some nodes of a key: %s\n", cds_ft_status_to_string(status));
				assert(0);
			}
		}
		status = cds_ft_eager_lookup_key(test_ft, (uint8_t *) string, strlen(string), 0, &ft_node);
		if (ft_node) {
			fprintf(stderr, "Error lookup \"%s\": %p (after remove) failed. Node is not expected: %s\n", string, ft_node, cds_ft_status_to_string(status));
			assert(0);
		}
		rcu_read_unlock();
	}
	printf("OK\n");

	cds_ft_iter_destroy(iter);
	return 0;
}

static
int do_test_varlen_string(void)
{
	int ret;
	struct cds_ft_group_attr *attr;

	printf("Variable length string key test start.\n");

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	/* Use variable length keys (default). */

	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	ret = test_varlen_string_key_insert();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	if (show_stats)
		cds_ft_show_stats(test_ft, stderr);

	ret = test_varlen_string_key_lookup();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	ret = test_varlen_string_key_lookup_fail();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	ret = test_varlen_string_key_remove();
	if (ret) {
		return ret;
	}
	rcu_quiescent_state();

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);
	printf("Sanity test ends\n");

	return 0;
}

enum urcu_ft_insertremove {
	AR_RANDOM = 0,
	AR_INSERT = 1,
	AR_REMOVE = -1,
};	/* 1: insert, -1 remove, 0: random */

static enum urcu_ft_insertremove insertremove; /* 1: insert, -1 remove, 0: random */

static
void test_ft_rw_sigusr1_handler(int signo __attribute__((unused)))
{
	switch (insertremove) {
	case AR_INSERT:
		printf("Add/Remove: random.\n");
		insertremove = AR_RANDOM;
		break;
	case AR_RANDOM:
		printf("Add/Remove: remove only.\n");
		insertremove = AR_REMOVE;
		break;
	case AR_REMOVE:
		printf("Add/Remove: insert only.\n");
		insertremove = AR_INSERT;
		break;
	}
}

static
void *test_ft_rw_thr_reader(void *_count)
{
	unsigned long long *count = _count;
	struct cds_ft_node *ft_node;
	enum cds_ft_status status;
	uint64_t key;

	printf_verbose("thread_begin %s, tid %lu\n",
			"reader", urcu_get_thread_id());

	URCU_TLS(rand_lookup) = urcu_get_thread_id() ^ time(NULL);

	set_affinity();

	rcu_register_thread();

	while (!uatomic_load(&test_go, CMM_ACQUIRE))
	{
	}
	cmm_smp_mb();

	for (;;) {
		uint8_t ftkey[8];

		rcu_read_lock();

		/* note: only looking up ulong keys */
		key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % lookup_pool_size) + lookup_pool_offset;
		key *= key_mul;
		cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
		status = cds_ft_eager_lookup_key(test_ft, ftkey, CDS_FT_LEN_DEFAULT, 0, &ft_node);
		if (!ft_node) {
			if (validate_lookup) {
				printf("[ERROR] Lookup cannot find initial node: %s\n", cds_ft_status_to_string(status));
				exit(-1);
			}
			URCU_TLS(lookup_fail)++;
		} else {
			URCU_TLS(lookup_ok)++;
		}
		rcu_debug_yield_read();
		if (caa_unlikely(rduration))
			loop_sleep(rduration);
		rcu_read_unlock();
		URCU_TLS(nr_reads)++;
		if (caa_unlikely(!test_duration_read()))
			break;
		if (caa_unlikely((URCU_TLS(nr_reads) & ((1 << 10) - 1)) == 0))
			rcu_quiescent_state();
	}

	rcu_unregister_thread();

	*count = URCU_TLS(nr_reads);
	printf_verbose("thread_end %s, tid %lu\n",
			"reader", urcu_get_thread_id());
	printf_verbose("readid : %lx, lookupfail %lu, lookupok %lu\n",
			pthread_self(), URCU_TLS(lookup_fail),
			URCU_TLS(lookup_ok));
	return ((void*)1);
}

static
int is_insert(void)
{
	return ((unsigned int) rand_r(&URCU_TLS(rand_lookup)) % 100) < insert_ratio;
}

static
void *test_ft_rw_thr_writer(void *_count)
{
	struct wr_count *count = _count;
	uint64_t key;
	enum cds_ft_status status;
	struct cds_ft_iter *iter;

	printf_verbose("thread_begin %s, tid %lu\n",
			"writer", urcu_get_thread_id());

	URCU_TLS(rand_lookup) = urcu_get_thread_id() ^ time(NULL);

	set_affinity();

	rcu_register_thread();

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	while (!uatomic_load(&test_go, CMM_ACQUIRE))
	{
	}
	cmm_smp_mb();

	for (;;) {
		if ((insertremove == AR_INSERT)
				|| (insertremove == AR_RANDOM && is_insert())) {
			struct ft_test_node *node = node_alloc();
			struct cds_ft_node *ret_node;
			uint8_t ftkey[8];

			/* note: only inserting ulong keys */
			key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % write_pool_size) + write_pool_offset;
			key *= key_mul;
			cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
			ft_test_node_init(node, key);
			rcu_read_lock();
			if (insert_unique) {
				mutex_lock_mt();
				status = cds_ft_insert_unique(test_ft, ftkey, CDS_FT_LEN_DEFAULT, &node->node, &ret_node);
				mutex_unlock_mt();
				if (status == CDS_FT_STATUS_DUPLICATE_FOUND) {
					free_test_node(node);
					URCU_TLS(nr_insertexist)++;
				} else if (status == CDS_FT_STATUS_OK) {
					URCU_TLS(nr_insert)++;
				} else {
					fprintf(stderr, "Error in cds_ft_insert_unique: %s\n", cds_ft_status_to_string(status));
					free_test_node(node);
				}
			} else if (insert_replace) {
				assert(0);	/* not implemented yet. */
			} else {
				mutex_lock_mt();
				status = cds_ft_insert(test_ft, ftkey, CDS_FT_LEN_DEFAULT, &node->node);
				mutex_unlock_mt();
				if (status) {
					fprintf(stderr, "Error in cds_ft_insert: %s\n", cds_ft_status_to_string(status));
					free_test_node(node);
				} else {
					URCU_TLS(nr_insert)++;
				}
			}
			rcu_read_unlock();
		} else {
			struct cds_ft_node *ft_node;
			struct ft_test_node *node;
			uint8_t ftkey[8];

			/* May remove */
			/* note: only remove ulong keys */
			key = ((unsigned long) rand_r(&URCU_TLS(rand_lookup)) % write_pool_size) + write_pool_offset;
			key *= key_mul;
			cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);

			rcu_read_lock();

			cds_ft_iter_set_key(iter, ftkey, CDS_FT_LEN_DEFAULT);
			status = cds_ft_lookup(test_ft, iter);
			ft_node = cds_ft_iter_node(iter);
			/* Remove first entry */
			if (ft_node) {
				node = caa_container_of(ft_node,
					struct ft_test_node, node);
				mutex_lock_mt();
				status = cds_ft_remove(test_ft, iter, &node->node);
				mutex_unlock_mt();
				if (status == CDS_FT_STATUS_OK) {
					rcu_free_test_node(node);
					URCU_TLS(nr_remove)++;
				} else {
					URCU_TLS(nr_removenoent)++;
				}
			} else {
				URCU_TLS(nr_removenoent)++;
			}
			rcu_read_unlock();
		}

		URCU_TLS(nr_writes)++;
		if (caa_unlikely(!test_duration_write()))
			break;
		if (caa_unlikely(wdelay))
			loop_sleep(wdelay);
		if (caa_unlikely((URCU_TLS(nr_writes) & ((1 << 10) - 1)) == 0))
			rcu_quiescent_state();
	}

	cds_ft_iter_destroy(iter);

	rcu_unregister_thread();

	printf_verbose("thread_end %s, tid %lu\n",
			"writer", urcu_get_thread_id());
	printf_verbose("info id %lx: nr_insert %lu, nr_insertexist %lu, nr_remove %lu, "
			"nr_removenoent %lu\n", pthread_self(), URCU_TLS(nr_insert),
			URCU_TLS(nr_insertexist), URCU_TLS(nr_remove),
			URCU_TLS(nr_removenoent));
	count->update_ops = URCU_TLS(nr_writes);
	count->insert = URCU_TLS(nr_insert);
	count->insert_exist = URCU_TLS(nr_insertexist);
	count->remove = URCU_TLS(nr_remove);
	return ((void*)2);
}

static
int do_mt_populate_ft(void)
{
	uint64_t iter;
	enum cds_ft_status status;

	if (!init_populate)
		return 0;

	printf("Starting rw test\n");

	for (iter = init_pool_offset; iter < init_pool_offset + init_pool_size; iter++) {
		struct ft_test_node *node = node_alloc();
		uint64_t key;
		uint8_t ftkey[8];

		/* note: only inserting ulong keys */
		key = (unsigned long) iter;
		key *= key_mul;
		cds_ft_u64_to_key(test_ft, key, ftkey, CDS_FT_LEN_DEFAULT);
		ft_test_node_init(node, key);
		rcu_read_lock();
		status = cds_ft_insert(test_ft, ftkey, CDS_FT_LEN_DEFAULT, &node->node);
		URCU_TLS(nr_insert)++;
		URCU_TLS(nr_writes)++;
		rcu_read_unlock();
		/* Hash table resize only occurs in call_rcu thread */
		if (!(iter % 100))
			rcu_quiescent_state();
		if (status) {
			fprintf(stderr, "Error inserting node %" PRIu64 ": %s\n",
				key, cds_ft_status_to_string(status));
			assert(0);
		}
	}

	return 0;
}

static
int do_mt_test(void)
{
	pthread_t *tid_reader, *tid_writer;
	void *tret;
	int ret, err;
	unsigned int i;
	unsigned long long *count_reader;
	struct wr_count *count_writer;
	unsigned long long tot_reads = 0, tot_writes = 0,
		tot_insert = 0, tot_insert_exist = 0, tot_remove = 0;
	unsigned int remain;
	struct cds_ft_group_attr *attr;

	tid_reader = malloc(sizeof(*tid_reader) * nr_readers);
	tid_writer = malloc(sizeof(*tid_writer) * nr_writers);
	count_reader = malloc(sizeof(*count_reader) * nr_readers);
	count_writer = malloc(sizeof(*count_writer) * nr_writers);

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	if (cds_ft_group_attr_set_key_len(attr, key_len) < 0)
		abort();
	if (cds_ft_group_attr_set_writer_strategy(attr, writer_strategy)
			!= CDS_FT_STATUS_OK)
		abort();

	printf("Allocating Fractal Trie for %u-byte keys\n", key_len);
	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		ret = -1;
		goto end;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		ret = -1;
		goto end;
	}
	test_apply_verify_period(test_ft);

	do_mt_populate_ft();

	if (show_stats)
		cds_ft_show_stats(test_ft, stderr);

	next_aff = 0;

	for (i = 0; i < nr_readers; i++) {
		err = pthread_create(&tid_reader[i],
				     NULL, test_ft_rw_thr_reader,
				     &count_reader[i]);
		if (err != 0)
			exit(1);
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_create(&tid_writer[i],
				     NULL, test_ft_rw_thr_writer,
				     &count_writer[i]);
		if (err != 0)
			exit(1);
	}

	cmm_smp_mb();

	uatomic_store(&test_go, 1, CMM_RELEASE);

	urcu_qsbr_thread_offline();

	remain = duration;
	do {
		remain = sleep(remain);
	} while (remain > 0);

	uatomic_store(&test_stop, 1, CMM_RELEASE);

	for (i = 0; i < nr_readers; i++) {
		err = pthread_join(tid_reader[i], &tret);
		if (err != 0)
			exit(1);
		tot_reads += count_reader[i];
	}
	for (i = 0; i < nr_writers; i++) {
		err = pthread_join(tid_writer[i], &tret);
		if (err != 0)
			exit(1);
		tot_writes += count_writer[i].update_ops;
		tot_insert += count_writer[i].insert;
		tot_insert_exist += count_writer[i].insert_exist;
		tot_remove += count_writer[i].remove;
	}
	urcu_qsbr_thread_online();

	printf("SUMMARY nr_readers=%u nr_writers=%u duration=%lu "
		"reads=%llu writes=%llu inserts=%llu insert_exists=%llu "
		"removes=%llu\n",
		nr_readers, nr_writers, duration,
		tot_reads, tot_writes, tot_insert, tot_insert_exist,
		tot_remove);

	if (show_stats)
		cds_ft_show_stats(test_ft, stderr);

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);

	free(tid_reader);
	free(tid_writer);
	free(count_reader);
	free(count_writer);
	ret = 0;
end:
	return ret;
}

static
int check_memory_leaks(void)
{
	unsigned long na, nf;

	na = uatomic_read(&test_nodes_allocated);
	nf = uatomic_read(&test_nodes_freed);
	if (na != nf) {
		fprintf(stderr, "Memory leak of %ld test nodes detected. Allocated: %lu, freed: %lu\n",
			na - nf, na, nf);
		return -1;
	}
	return 0;
}

static const char *torture_test_strings[] = {
	"apple",
	"apples",
	"apply",
	"ball",
	"bat",
	"bath",
	"bathe",
	"car",
	"cart",
	"cat",
	"dog",
};

static
int do_test_dictionary(void)
{
	struct cds_ft_group_attr *attr;
	struct cds_ft_iter *iter;
	char *line = NULL;
	size_t len = 0;
	ssize_t read_len;
	enum cds_ft_status status;

	printf("Allocating Fractal Trie string keys\n");

	if (cds_ft_group_attr_create(&attr) < 0)
		abort();
	/* Use variable length keys (default). */

	if (cds_ft_group_create(attr, &test_ft_group) < 0) {
		cds_ft_group_attr_destroy(attr);
		printf("Error allocating Fractal Trie group.\n");
		return -1;
	}
	cds_ft_group_attr_destroy(attr);
	if (cds_ft_create(test_ft_group, NULL, &test_ft) < 0) {
		cds_ft_group_destroy(test_ft_group);
		printf("Error allocating Fractal Trie.\n");
		return -1;
	}
	test_apply_verify_period(test_ft);

	if (cds_ft_iter_create(test_ft, &iter) < 0)
		abort();

	if (torture_test_string) {
		size_t i;

		for (i = 0; i < CAA_ARRAY_SIZE(torture_test_strings); i++) {
			const char *string = torture_test_strings[i];
			struct ft_test_node *node = node_alloc();

			ft_test_node_init(node, 0);
			rcu_read_lock();
			status = cds_ft_insert(test_ft, (uint8_t *) string, strlen(string), &node->node);
			rcu_read_unlock();
			if (status) {
				fprintf(stderr, "Error inserting node \"%s\": %s\n",
					string, cds_ft_status_to_string(status));
				assert(0);
			}
		}
	} else {
		printf("Provide input on stdin, one string per line, followed by end of stream (CTRL-D)\n");

		for (;;) {
			struct ft_test_node *node;

			read_len = getline(&line, &len, stdin);
			if (read_len <= 0)
				break;
			/* Skip empty lines. */
			if (read_len == 1)
				continue;
			line[read_len - 1] = '\0';
			node = node_alloc();

			ft_test_node_init(node, 0);
			rcu_read_lock();
			status = cds_ft_insert(test_ft, (uint8_t *) line, strlen(line), &node->node);
			rcu_read_unlock();
			if (status) {
				fprintf(stderr, "Error inserting node \"%s\": %s\n",
					line, cds_ft_status_to_string(status));
				assert(0);
			}
		}
		free(line);
	}

	if (show_stats)
		cds_ft_show_stats(test_ft, stdout);

	/* Show sorted output. */
	printf("%sorted dictionary\n", reverse_sort ? "Reverse S" : "S");
	printf("---------------------------------\n");

	rcu_read_lock();

	if (!reverse_sort) {
		cds_ft_for_each_rcu(test_ft, iter) {
			struct cds_ft_node *node = cds_ft_iter_node(iter);
			uint8_t key[256];
			size_t entry_key_len;

			cds_ft_iter_get_key(iter, key, sizeof(key), &entry_key_len);
			cds_ft_for_each_duplicate_rcu(node)
				printf("%.*s\n", (int) entry_key_len, key);
		}
	} else {
		cds_ft_for_each_reverse_rcu(test_ft, iter) {
			struct cds_ft_node *node = cds_ft_iter_node(iter);
			uint8_t key[256];
			size_t entry_key_len;

			cds_ft_iter_get_key(iter, key, sizeof(key), &entry_key_len);
			cds_ft_for_each_duplicate_rcu(node)
				printf("%.*s\n", (int) entry_key_len, key);
		}
	}
	rcu_read_unlock();

	status = cds_ft_iter_status(iter);
	if (status < 0) {
		fprintf(stderr, "Error iterating on trie: %s\n",
			cds_ft_status_to_string(status));
		cds_ft_iter_destroy(iter);
		return -1;
	}

	cds_ft_iter_destroy(iter);

	printf("---------------------------------\n");

	if (test_free_all_nodes(test_ft)) {
		fprintf(stderr, "Error freeing all nodes\n");
		return -1;
	}

	cds_ft_destroy(test_ft);
	cds_ft_group_destroy(test_ft_group);

	return 0;
}

/*
 * Long-only options use values >= 256 to avoid collisions with
 * single-character option values.
 */
enum {
	OPT_YIELD_READER = 256,
	OPT_YIELD_WRITER,
	OPT_VERIFY_AT_MUTATION_PERIOD,
};

int main(int argc, char **argv)
{
	int ret, err, opt;
	struct sigaction act;

	static struct option long_options[] = {
		{ "writer-delay",		required_argument,	NULL, 'd' },
		{ "reader-duration",		required_argument,	NULL, 'c' },
		{ "verbose",			no_argument,		NULL, 'v' },
		{ "affinity",			required_argument,	NULL, 'a' },
		{ "insert-ratio",		required_argument,	NULL, 'r' },
		{ "populate",			no_argument,		NULL, 'k' },
		{ "lookup-pool-offset",		required_argument,	NULL, 'R' },
		{ "write-pool-offset",		required_argument,	NULL, 'S' },
		{ "init-pool-offset",		required_argument,	NULL, 'T' },
		{ "lookup-pool-size",		required_argument,	NULL, 'M' },
		{ "write-pool-size",		required_argument,	NULL, 'N' },
		{ "init-pool-size",		required_argument,	NULL, 'O' },
		{ "validate-lookup",		no_argument,		NULL, 'V' },
		{ "sanity-test",		no_argument,		NULL, 't' },
		{ "sanity-test-varlen",		no_argument,		NULL, 'x' },
		{ "sanity-test-varlen-string",	no_argument,		NULL, 'y' },
		{ "key-len",			required_argument,	NULL, 'B' },
		{ "key-mul",			required_argument,	NULL, 'm' },
		{ "insert-unique",		no_argument,		NULL, 'u' },
		{ "insert-replace",		no_argument,		NULL, 's' },
		{ "leak-detection",		no_argument,		NULL, 'l' },
		{ "show-stats",			no_argument,		NULL, 'Z' },
		{ "dictionary",			no_argument,		NULL, 'D' },
		{ "reverse-sort",		no_argument,		NULL, 'q' },
		{ "torture-test-string",	no_argument,		NULL, 'o' },
		{ "verify-at-mutation-period",	required_argument,	NULL, OPT_VERIFY_AT_MUTATION_PERIOD },
#ifdef DEBUG_YIELD
		{ "yield-reader",		no_argument,		NULL, OPT_YIELD_READER },
		{ "yield-writer",		no_argument,		NULL, OPT_YIELD_WRITER },
#endif
		{ NULL, 0, NULL, 0 },
	};

	if (argc < 4)
		goto usage_error;

	err = sscanf(argv[1], "%u", &nr_readers);
	if (err != 1)
		goto usage_error;

	err = sscanf(argv[2], "%u", &nr_writers);
	if (err != 1)
		goto usage_error;

	err = sscanf(argv[3], "%lu", &duration);
	if (err != 1)
		goto usage_error;

	optind = 4;
	while ((opt = getopt_long(argc, argv, "d:c:va:r:kR:S:T:M:N:O:Vtxyb:B:m:uslZDqoW:",
				  long_options, NULL)) != -1) {
		switch (opt) {
		case 'd':
			wdelay = atol(optarg);
			break;
		case 'W':
			/*
			 * E.4: writer strategy, "fine" (default), "coarse", or
			 * "external-sync".
			 *
			 * ☠ "external-sync" IS A CONTRACT, AND THIS HARNESS CAN
			 * BREAK IT: the application promises writer exclusion,
			 * so it is only in contract with a SINGLE writer thread
			 * (-n 1).  Run it with more and the trie is being used
			 * out of contract on purpose -- which is what a
			 * -DFEATURE_FT_EXCL_VALIDATE build is for: the access
			 * validator keeps its single-owner claim on any
			 * non-fine trie and reports the overlap.
			 */
			if (!strcmp(optarg, "coarse"))
				writer_strategy = CDS_FT_WRITER_LOCK_COARSE;
			else if (!strcmp(optarg, "fine"))
				writer_strategy = CDS_FT_WRITER_LOCK_FINE;
			else if (!strcmp(optarg, "external-sync"))
				writer_strategy = CDS_FT_WRITER_EXTERNAL_SYNC;
			else
				goto usage_error;
			break;
		case 'c':
			rduration = atol(optarg);
			break;
		case 'v':
			verbose_mode = 1;
			break;
		case 'a':
			cpu_affinities[next_aff++] = atoi(optarg);
			use_affinity = 1;
			printf_verbose("Adding CPU %d affinity\n", atoi(optarg));
			break;
		case 'r':
			insert_ratio = atoi(optarg);
			break;
		case 'k':
			init_populate = 1;
			break;
		case 'R':
			lookup_pool_offset = atol(optarg);
			break;
		case 'S':
			write_pool_offset = atol(optarg);
			break;
		case 'T':
			init_pool_offset = atol(optarg);
			break;
		case 'M':
			lookup_pool_size = atol(optarg);
			break;
		case 'N':
			write_pool_size = atol(optarg);
			break;
		case 'O':
			init_pool_size = atol(optarg);
			break;
		case 'V':
			validate_lookup = 1;
			break;
		case 't':
			sanity_test = 1;
			break;
		case 'x':
			sanity_test_varlen = 1;
			break;
		case 'y':
			sanity_test_varlen_string = 1;
			break;
		case 'B':
			key_len = atol(optarg);
			break;
		case 'm':
			key_mul = atoll(optarg);
			break;
		case 'u':
			insert_unique = 1;
			break;
		case 's':
			insert_replace = 1;
			break;
		case 'l':
			leak_detection = 1;
			break;
		case 'Z':
			show_stats = 1;
			break;
		case 'D':
			test_dictionary = 1;
			break;
		case 'q':
			reverse_sort = 1;
			break;
		case 'o':
			torture_test_string = 1;
			break;
		case OPT_VERIFY_AT_MUTATION_PERIOD:
			verify_at_mutation_period = strtoul(optarg, NULL, 0);
			verify_at_mutation_period_set = true;
			break;
#ifdef DEBUG_YIELD
		case OPT_YIELD_READER:
			yield_active |= YIELD_READ;
			break;
		case OPT_YIELD_WRITER:
			yield_active |= YIELD_WRITE;
			break;
#endif
		default:
			goto usage_error;
		}
	}

	printf_verbose("running test for %lu seconds, %u readers, %u writers.\n",
		duration, nr_readers, nr_writers);
	printf_verbose("Writer delay : %lu loops.\n", wdelay);
	printf_verbose("Reader duration : %lu loops.\n", rduration);
	printf_verbose("Insert ratio: %u%%.\n", insert_ratio);
	printf_verbose("Mode:%s%s.\n",
		" insert/remove",
		insert_unique ? " uniquify" : ( insert_replace ? " replace" : " insert"));
	printf_verbose("Key multiplication factor: %" PRIu64 ".\n", key_mul);
	printf_verbose("Init pool size offset %lu size %lu.\n",
		init_pool_offset, init_pool_size);
	printf_verbose("Lookup pool size offset %lu size %lu.\n",
		lookup_pool_offset, lookup_pool_size);
	printf_verbose("Update pool size offset %lu size %lu.\n",
		write_pool_offset, write_pool_size);
	if (validate_lookup)
		printf_verbose("Validating lookups.\n");
	if (leak_detection)
		printf_verbose("Memory leak dection activated.\n");
	printf_verbose("thread %-6s, tid %lu\n",
			"main", urcu_get_thread_id());

	memset(&act, 0, sizeof(act));
	ret = sigemptyset(&act.sa_mask);
	if (ret == -1) {
		perror("sigemptyset");
		return -1;
	}
	act.sa_handler = test_ft_rw_sigusr1_handler;
	act.sa_flags = SA_RESTART;
	ret = sigaction(SIGUSR1, &act, NULL);
	if (ret == -1) {
		perror("sigaction");
		return -1;
	}

	err = create_all_cpu_call_rcu_data(0);
	if (err) {
		printf("Per-CPU call_rcu() worker threads unavailable. Using default global worker thread.\n");
	}

	rcu_register_thread();

	if (sanity_test) {
		ret = do_sanity_test();
	} else if (sanity_test_varlen) {
		ret = do_sanity_test_varlen();
	} else if (sanity_test_varlen_string) {
		ret = do_test_varlen_string();
	} else if (test_dictionary) {
		ret = do_test_dictionary();
	} else {
		ret = do_mt_test();
	}

	/* Wait for in-flight call_rcu free to complete for leak detection */
	rcu_barrier();

	ret |= check_memory_leaks();

	rcu_unregister_thread();
	free_all_cpu_call_rcu_data();

	if (ret) {
		printf("Test ended with error: %d\n", ret);
	}
	return ret;

usage_error:
	show_usage(argv);
	return -1;
}
