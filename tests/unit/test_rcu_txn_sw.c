// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

/*
 * Unit test for urcu_txn_sw_txn: the multi-edge transaction layer of the
 * single-updater engine, focusing on the commit() shortcuts -- the single-edge fast path
 * (no proxy at all) and auto-install -- the explicit-install caveat, the
 * record-array realloc-grow path, and the find() and discard() accessors.
 *
 * commit() owns reclaim: it frees the txn at once on the single-edge / empty
 * paths and defers it through call_rcu() once proxies are parked.  The test
 * therefore runs under an RCU flavor (memb) and drains the deferred frees with
 * rcu_barrier() before exit.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifndef _LGPL_SOURCE
#define _LGPL_SOURCE
#endif

#include <stdbool.h>

#include <urcu/compiler.h>
#include <urcu-qsbr.h>
#include <urcu-call-rcu.h>
#include <urcu/rcu-txn-sw.h>

#include "tap.h"

#define NR_TESTS 33

/* Per-record tag: bit 0 of the proxy pointer (latches are 16B-aligned). */
#define TEST_TAG	1UL

static int is_proxy(void *v)
{
	return (unsigned long) v & 1UL;
}

static void *resolve(void *v)
{
	if (is_proxy(v))
		return urcu_txn_sw_proxy_get(
			(struct urcu_txn_sw_proxy *) ((unsigned long) v & ~1UL));
	return v;
}

static unsigned long reclaim_calls;

/*
 * Counting reclaim deferral: proves commit_flavor() routes the parked group
 * block through the SUPPLIED call_rcu_fn (not a hardcoded call_rcu), then defers
 * the real free so rcu_barrier() drains it.  This is the shape a flavor-agnostic
 * embedder uses (passing its flavor->update_call_rcu).
 */
static void counting_call_rcu(struct rcu_head *head,
		void (*func)(struct rcu_head *))
{
	reclaim_calls++;
	call_rcu(head, func);
}

/*
 * Synchronous reclaim: an exclusive (no concurrent reader) embedder frees the
 * group block in place, with no grace period -- what the fractal trie does on
 * its exclusive build.
 */
static void sync_call_rcu(struct rcu_head *head,
		void (*func)(struct rcu_head *))
{
	func(head);
}

int main(void)
{
	int err;

	err = create_all_cpu_call_rcu_data(0);
	if (err)
		diag("Per-CPU call_rcu() workers unavailable, using default.");

	rcu_register_thread();
	plan_tests(NR_TESTS);

	/*
	 * 1. Single-edge fast path: a commit reached in PREPARE with exactly one
	 * recorded edge stores the new target directly -- no proxy is ever
	 * installed, so commit frees the txn at once (no grace period).
	 */
	{
		void *slot = (void *) 0x100;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init(t);
		ok(urcu_txn_sw_reserve(t, 4), "init + reserve");
		urcu_txn_sw_record(t, &slot, (void *) 0x100, (void *) 0x200, TEST_TAG);
		st = urcu_txn_sw_commit(t);		/* single edge: frees now */
		ok(st == URCU_TXN_STATUS_OK, "single-edge commit returns OK");
		ok(slot == (void *) 0x200, "single-edge slot holds the new target directly");
		ok(!is_proxy(slot), "single-edge never parks a proxy");
	}

	/*
	 * 2. Multi-edge auto-install: a commit reached in PREPARE with two or more
	 * edges installs every proxy first, then flips the group; all slots settle
	 * to new and commit defers the txn through call_rcu.
	 */
	{
		void *s1 = (void *) 0x10, *s2 = (void *) 0x20, *s3 = (void *) 0x30;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init(t);
		urcu_txn_sw_reserve(t, 4);
		urcu_txn_sw_record(t, &s1, (void *) 0x10, (void *) 0x11, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x20, (void *) 0x21, TEST_TAG);
		urcu_txn_sw_record(t, &s3, (void *) 0x30, (void *) 0x31, TEST_TAG);
		st = urcu_txn_sw_commit(t);		/* multi-edge: parks proxies */
		ok(st == URCU_TXN_STATUS_OK, "multi-edge commit returns OK");
		ok(s1 == (void *) 0x11 && s2 == (void *) 0x21 &&
			s3 == (void *) 0x31, "multi-edge slots all settled to new");
	}

	/*
	 * 3. Explicit install + single edge: once install() parks the proxy a
	 * reader may already hold it, so commit MUST flip the group (the fast path
	 * is unavailable) and defer reclaim through call_rcu.
	 */
	{
		void *slot = (void *) 0x100;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init(t);
		urcu_txn_sw_reserve(t, 4);
		urcu_txn_sw_record(t, &slot, (void *) 0x100, (void *) 0x200, TEST_TAG);
		urcu_txn_sw_install(t);
		ok(is_proxy(slot) && resolve(slot) == (void *) 0x100,
			"explicit install parks a proxy resolving to old");
		ok(urcu_txn_sw_is_proxy(slot, TEST_TAG),
			"urcu_txn_sw_is_proxy sees the parked proxy");
		st = urcu_txn_sw_commit(t);
		ok(st == URCU_TXN_STATUS_OK,
			"explicitly-installed single edge commits OK");
		ok(slot == (void *) 0x200, "explicitly-installed slot settles to new");
		ok(!urcu_txn_sw_is_proxy(slot, TEST_TAG),
			"urcu_txn_sw_is_proxy sees no proxy once settled");
	}

	/*
	 * 4. Realloc-grow: record more edges than the initial capacity WITHOUT a
	 * reserve(), forcing the record array to realloc-grow; commit auto-installs
	 * the whole set and every slot settles to its new target.
	 */
	{
		enum { NR_EDGES = 3 * URCU_TXN_SW_CAP + 1 };
		void *slots[NR_EDGES];
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;
		bool all_recorded = true, all_new = true;
		unsigned int i;

		urcu_txn_sw_init(t);
		for (i = 0; i < NR_EDGES; i++) {
			slots[i] = (void *) (((unsigned long) (i + 1)) << 8);
			if (!urcu_txn_sw_record(t, &slots[i], slots[i],
				(void *) ((((unsigned long) (i + 1)) << 8) | 0x10), TEST_TAG))
				all_recorded = false;
		}
		ok(all_recorded, "record realloc-grows past the initial capacity");
		st = urcu_txn_sw_commit(t);
		ok(st == URCU_TXN_STATUS_OK,
			"realloc-grown multi-edge commit returns OK");
		for (i = 0; i < NR_EDGES; i++)
			if (slots[i] != (void *) ((((unsigned long) (i + 1)) << 8) | 0x10))
				all_new = false;
		ok(all_new, "all realloc-grown slots settled to new");
	}

	/*
	 * 5. Inline (caller-storage) lone-edge: urcu_txn_sw_init_inline backs the
	 * record array with an on-stack latch buffer (no allocation).  A single
	 * recorded edge takes the fast path -- one direct store, no proxy -- and
	 * commit_flavor() never touches the reclaim fn or frees the inline buffer.
	 */
	{
		void *slot = (void *) 0x100;
		struct urcu_txn_sw_latch buf[1];
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		ok(((unsigned long) buf & 0xfUL) == 0,
			"inline latch buffer is 16-byte aligned (tag room)");
		urcu_txn_sw_init_inline(t, buf, 1);
		urcu_txn_sw_record(t, &slot, (void *) 0x100, (void *) 0x200, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, sync_call_rcu);	/* lone edge: fn unused */
		ok(st == URCU_TXN_STATUS_OK, "inline lone-edge commit_flavor returns OK");
		ok(slot == (void *) 0x200 && !is_proxy(slot),
			"inline lone-edge slot holds new directly, no proxy");
	}

	/*
	 * 6. commit_flavor routes parked-block reclaim through the SUPPLIED fn: a
	 * multi-edge commit defers exactly one block, through counting_call_rcu,
	 * which a later rcu_barrier() drains.  Confirms a flavor-agnostic embedder
	 * controls the deferral rather than the header's compile-time call_rcu.
	 */
	{
		void *s1 = (void *) 0x10, *s2 = (void *) 0x20, *s3 = (void *) 0x30;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		reclaim_calls = 0;
		urcu_txn_sw_init(t);
		urcu_txn_sw_record(t, &s1, (void *) 0x10, (void *) 0x11, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x20, (void *) 0x21, TEST_TAG);
		urcu_txn_sw_record(t, &s3, (void *) 0x30, (void *) 0x31, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, counting_call_rcu);
		ok(st == URCU_TXN_STATUS_OK, "commit_flavor multi-edge returns OK");
		ok(s1 == (void *) 0x11 && s2 == (void *) 0x21 &&
			s3 == (void *) 0x31, "commit_flavor slots all settled to new");
		ok(reclaim_calls == 1,
			"commit_flavor routed reclaim through the supplied call_rcu_fn");
	}

	/*
	 * 7. Synchronous (exclusive) reclaim: a multi-edge commit_flavor with an
	 * in-place reclaim fn frees the group block before returning -- no grace
	 * period owed.  Slots still settle to new; a leak/UAF here would be caught
	 * by ASAN since no rcu_barrier covers this block.
	 */
	{
		void *s1 = (void *) 0x40, *s2 = (void *) 0x50;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init(t);
		urcu_txn_sw_record(t, &s1, (void *) 0x40, (void *) 0x41, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x50, (void *) 0x51, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, sync_call_rcu);
		ok(st == URCU_TXN_STATUS_OK,
			"exclusive (synchronous-reclaim) multi-edge commit returns OK");
		ok(s1 == (void *) 0x41 && s2 == (void *) 0x51,
			"exclusive multi-edge slots settled to new, block freed in place");
	}

	/*
	 * 8. find: a recorded slot returns its record, with old and new in
	 * its proxy.  A slot not recorded returns NULL, and so does every
	 * slot once the commit consumed the records.
	 */
	{
		void *s1 = (void *) 0x10, *s2 = (void *) 0x20, *s3 = (void *) 0x30;
		const struct urcu_txn_sw_latch *l;
		struct urcu_txn_sw_txn _t, *t = &_t;

		urcu_txn_sw_init(t);
		ok(!urcu_txn_sw_find(t, &s1), "find on an empty txn is NULL");
		urcu_txn_sw_record(t, &s1, (void *) 0x10, (void *) 0x11, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x20, (void *) 0x21, TEST_TAG);
		l = urcu_txn_sw_find(t, &s2);
		ok(l && l->proxy.ptr[0] == (void *) 0x20 &&
			l->proxy.ptr[1] == (void *) 0x21,
			"find returns the record of a recorded slot");
		ok(!urcu_txn_sw_find(t, &s3), "find on a slot not recorded is NULL");
		urcu_txn_sw_commit_flavor(t, sync_call_rcu);
		ok(!urcu_txn_sw_find(t, &s1), "find after the commit is NULL");
	}

	/*
	 * 9. find past URCU_TXN_SW_BLOOM_MIN records, where the RYW filter is
	 * armed: every recorded slot is found, and a slot not recorded is not.
	 */
	{
		enum { NR_EDGES = 2 * URCU_TXN_SW_BLOOM_MIN };
		void *slots[NR_EDGES + 1];
		struct urcu_txn_sw_txn _t, *t = &_t;
		bool all_found = true;
		unsigned int i;

		urcu_txn_sw_init(t);
		for (i = 0; i < NR_EDGES; i++) {
			slots[i] = (void *) (((unsigned long) (i + 1)) << 8);
			urcu_txn_sw_record(t, &slots[i], slots[i],
				(void *) ((unsigned long) slots[i] | 0x10), TEST_TAG);
		}
		for (i = 0; i < NR_EDGES; i++) {
			const struct urcu_txn_sw_latch *l =
				urcu_txn_sw_find(t, &slots[i]);

			if (!l || l->proxy.ptr[1] !=
					(void *) ((unsigned long) slots[i] | 0x10))
				all_found = false;
		}
		ok(all_found, "find returns every record of a long txn");
		ok(!urcu_txn_sw_find(t, &slots[NR_EDGES]),
			"find on a slot not recorded is NULL in a long txn");
		urcu_txn_sw_commit_flavor(t, sync_call_rcu);
	}

	/*
	 * 10. discard: the slots keep their values and the txn has no
	 * records.  It then records and commits like a new one, both after a
	 * discard and after a commit followed by a discard.
	 */
	{
		void *s1 = (void *) 0x10, *s2 = (void *) 0x20;
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init(t);
		urcu_txn_sw_record(t, &s1, (void *) 0x10, (void *) 0x11, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x20, (void *) 0x21, TEST_TAG);
		urcu_txn_sw_discard(t);
		ok(s1 == (void *) 0x10 && s2 == (void *) 0x20 &&
			!urcu_txn_sw_find(t, &s1),
			"discard leaves the slots and drops the records");
		urcu_txn_sw_record(t, &s1, (void *) 0x10, (void *) 0x12, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x20, (void *) 0x22, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, sync_call_rcu);
		ok(st == URCU_TXN_STATUS_OK && s1 == (void *) 0x12 &&
			s2 == (void *) 0x22, "a discarded txn records and commits");
		urcu_txn_sw_discard(t);
		urcu_txn_sw_record(t, &s1, (void *) 0x12, (void *) 0x13, TEST_TAG);
		urcu_txn_sw_record(t, &s2, (void *) 0x22, (void *) 0x23, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, sync_call_rcu);
		ok(st == URCU_TXN_STATUS_OK && s1 == (void *) 0x13 &&
			s2 == (void *) 0x23,
			"a committed txn records and commits again after a discard");
	}

	/*
	 * 11. discard keeps caller storage: an inline txn records into the
	 * same buffer again, and its lone edge commits directly.
	 */
	{
		void *slot = (void *) 0x100;
		struct urcu_txn_sw_latch buf[1];
		struct urcu_txn_sw_txn _t, *t = &_t;
		enum urcu_txn_status st;

		urcu_txn_sw_init_inline(t, buf, 1);
		urcu_txn_sw_record(t, &slot, (void *) 0x100, (void *) 0x200, TEST_TAG);
		urcu_txn_sw_discard(t);
		ok(t->latches == buf && slot == (void *) 0x100,
			"inline discard keeps the caller's buffer");
		urcu_txn_sw_record(t, &slot, (void *) 0x100, (void *) 0x300, TEST_TAG);
		st = urcu_txn_sw_commit_flavor(t, sync_call_rcu);
		ok(st == URCU_TXN_STATUS_OK && slot == (void *) 0x300,
			"inline txn records and commits after a discard");
	}

	rcu_barrier();			/* drain deferred txn reclaim callbacks */
	rcu_unregister_thread();
	free_all_cpu_call_rcu_data();
	return exit_status();
}
