// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_DT_ARM_H
#define _FT_DT_ARM_H

/*
 * ☞ THE NON-OWNER-CLEAR DETECTOR'S ARM, AND NOTHING ELSE.
 *
 * The detector has three consumers in two translation units:
 *   - the table and the engine store hook, in fractal-trie.c (via
 *     ft-mutation-helpers.h and ft-txn-rec-dbg.h);
 *   - the allocator's forget, in fractal-trie-alloc.c (via
 *     fractal-trie-internal.h).
 * A build where those DISAGREE is worse than one where the detector is off.
 * An armed table whose forget compiled out keeps a stale owner against a
 * recycled address.  Every legitimate take of the new item then reads as a
 * steal.  So the gate is a value, defined once, in a header carrying nothing
 * else.
 *
 * ☠ IT CANNOT LIVE IN ft-txn-rec-dbg.h.  That header also declares the engine's
 * URCU_TXN_REC_WROTE hook as a static.  The definition of that static exists
 * only in fractal-trie.c.  Including ft-txn-rec-dbg.h from
 * fractal-trie-internal.h to reach the allocator put that hook into the
 * allocator's TU as well.  There, the engine headers reference a function
 * nothing defines ("'ft_dt_wrote' used but never defined").  A gate wants a
 * header with no other content.
 *
 * Armed by --enable-rcu-debug.  The question it answers is: does an op clear
 * an FT_STATE_LOCK bit it did not take?  That is a lock-discipline violation
 * of the same family as the urcu_assert_debug self-checks.  The releases it
 * watches have no other witness.  ft_meta_lock_release / _if_held take the
 * word's address and nothing else.  So they must re-derive ownership from the
 * shared state word.  That word cannot tell "I hold it" from "a peer holds
 * it".
 */
#if defined(FT_DEBUG_DOUBLE_TAKE) || defined(DEBUG_RCU) || \
		defined(CONFIG_RCU_DEBUG)
# define FT_DT_ARMED	1
#else
# define FT_DT_ARMED	0
#endif

#endif /* _FT_DT_ARM_H */
