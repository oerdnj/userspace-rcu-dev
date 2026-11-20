// SPDX-FileCopyrightText: 2026 EfficiOS Inc.
// SPDX-License-Identifier: MIT

/*
 * LTTng-UST tracepoint provider for Fractal Trie debugging.
 *
 * This header is only active when the library is built with
 * -DFT_ENABLE_TRACING.  Without that define, cds_ft_tp.c is empty and
 * fractal-trie.c's FT_TP() macro expands to a no-op, so the library
 * has no lttng-ust dependency in default builds.
 *
 * To enable: rebuild with CFLAGS including -DFT_ENABLE_TRACING and
 * link with -llttng-ust -llttng-ust-common.
 *
 * Keys appear in tracepoint payloads as byte sequences
 * (lttng_ust_field_sequence_hex) -- the trie is byte-oriented and
 * keys may have arbitrary length.
 *
 * Inequality-lookup mode, operation status, and compressed-handler
 * action are exposed as LTTng-UST enumerations so trace viewers
 * display the symbolic name rather than a raw integer.
 */

#ifdef FT_ENABLE_TRACING

/*
 * Helpers (struct/enum/function forward decls) are guarded separately
 * so they are only processed once, even though tracepoint-event.h
 * re-reads this header multiple times.  File-scope C declarations
 * are not legal in all re-read contexts.
 *
 * The ft_tp_node_kind enum lives in fractal-trie-internal.h so the
 * C-side return values and the LTTng enum values below cannot drift
 * apart.  ft_tp_node_kind() is defined in fractal-trie.c where
 * ft_types[] and the tag-bit helpers are visible.  Callers pass node
 * pointers.  The tracepoint field expressions below compute the kind
 * field, not the caller.
 */
#ifndef _FT_TP_H_HELPERS
#define _FT_TP_H_HELPERS
#include "fractal-trie-internal.h"
struct cds_ft_inode_flag;
uint16_t ft_tp_node_kind(struct cds_ft_inode_flag *nf);
uint16_t ft_tp_node_skip_len(struct cds_ft_inode_flag *nf);
#endif

#undef LTTNG_UST_TRACEPOINT_PROVIDER
#define LTTNG_UST_TRACEPOINT_PROVIDER cds_ft

#undef LTTNG_UST_TRACEPOINT_INCLUDE
#define LTTNG_UST_TRACEPOINT_INCLUDE "./cds_ft_tp.h"

#if !defined(_FT_TP_H) || defined(LTTNG_UST_TRACEPOINT_HEADER_MULTI_READ)
#define _FT_TP_H

#include <lttng/tracepoint.h>
#include <stdint.h>

/*
 * Enumerations for human-readable trace output.
 */

LTTNG_UST_TRACEPOINT_ENUM(cds_ft, ft_lookup_mode,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("FT_LOOKUP_GE", 0)
		lttng_ust_field_enum_value("FT_LOOKUP_LE", 1)
		lttng_ust_field_enum_value("FT_LOOKUP_GT", 2)
		lttng_ust_field_enum_value("FT_LOOKUP_LT", 3)
	)
)

LTTNG_UST_TRACEPOINT_ENUM(cds_ft, ft_status,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("OK",				 0)
		lttng_ust_field_enum_value("NOT_FOUND",				 1)
		lttng_ust_field_enum_value("DUPLICATE_FOUND",			 2)
		lttng_ust_field_enum_value("INTERNAL_MATCH",			 3)
		lttng_ust_field_enum_value("INVALID_ARGUMENT_ERROR",		-1)
		lttng_ust_field_enum_value("MEMORY_ERROR",			-2)
		lttng_ust_field_enum_value("OVERFLOW_ERROR",			-3)
		lttng_ust_field_enum_value("BUSY_ERROR",			-4)
		lttng_ust_field_enum_value("POPULATED_ERROR",			-5)
	)
)

LTTNG_UST_TRACEPOINT_ENUM(cds_ft, ft_compressed_action,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("CONTINUE",		0)
		lttng_ust_field_enum_value("BREAK",		1)
		lttng_ust_field_enum_value("END",		2)
		lttng_ust_field_enum_value("GOING_UP",		3)
		lttng_ust_field_enum_value("DESCEND_CHILDREN",	4)
	)
)

/*
 * Node-kind enum: maps a tagged cds_ft_inode_flag pointer to a
 * symbolic label that encodes both the structural class and the
 * specific size variant.  See ft_tp_node_kind() in fractal-trie.c
 * for the mapping.  Values must stay in sync with the C-side enum
 * defined alongside the helper.
 *
 * Internal labels are <class>_<size_in_bytes>; size = 2^order
 * where order is the ft_types[i].order field.  The exact labels
 * realized in a trace depend on build-time configuration (32-bit
 * vs 64-bit).  The full superset is listed here so the same
 * provider metadata works on any build.
 */
LTTNG_UST_TRACEPOINT_ENUM(cds_ft, ft_tp_node_kind,
	LTTNG_UST_TP_ENUM_VALUES(
		lttng_ust_field_enum_value("NULL",		FT_TP_NODE_NULL)
		lttng_ust_field_enum_value("EXTERNAL",		FT_TP_NODE_EXTERNAL)
		lttng_ust_field_enum_value("COMPRESSED",	FT_TP_NODE_COMPRESSED)
		lttng_ust_field_enum_value("P1L_128",		FT_TP_NODE_P1L_128)
		lttng_ust_field_enum_value("P2L_32",		FT_TP_NODE_P2L_32)
		lttng_ust_field_enum_value("P2L_64",		FT_TP_NODE_P2L_64)
		lttng_ust_field_enum_value("P2L_128",		FT_TP_NODE_P2L_128)
		lttng_ust_field_enum_value("P1L_256",		FT_TP_NODE_P1L_256)
		lttng_ust_field_enum_value("P1L_512",		FT_TP_NODE_P1L_512)
		lttng_ust_field_enum_value("P1L_1024",		FT_TP_NODE_P1L_1024)
		lttng_ust_field_enum_value("PIGEON_1024",	FT_TP_NODE_PIGEON_1024)
		lttng_ust_field_enum_value("PIGEON_2048",	FT_TP_NODE_PIGEON_2048)
		lttng_ust_field_enum_value("UNKNOWN",		FT_TP_NODE_UNKNOWN)
	)
)

/*
 * Event classes for common signatures to avoid boilerplate.
 */

/*
 * A MUTATION OP RETRIED PAST ITS CAP -- the livelock violation event.
 *
 * Fields:
 * - @op: the mutation entry point (ft_op_kind);
 * - @attempts: the count that tripped the cap;
 * - @last_ret: the code the last attempt returned.
 *
 * -EAGAIN PROMISES that retrying can help, i.e. that a PEER is responsible.
 * A count that runs away therefore names a SELF-refusal.  No re-descent can
 * clear a self-refusal.  @nr_writers separates the two readings at the
 * event: with one writer it is certain, with peers it is a hypothesis.
 *
 * The event is self-diagnosing by design.  The enter/step events around it
 * may have scrolled out of a 64K ring.  So the key that could not be moved
 * travels WITH the violation, and this one event is enough to recover the
 * shape.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, op_retry_violation,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		unsigned int, op,
		unsigned int, attempts,
		int, last_ret,
		unsigned int, nr_writers,
		const uint8_t *, key,
		size_t, key_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer(unsigned int, op, op)
		lttng_ust_field_integer(unsigned int, attempts, attempts)
		lttng_ust_field_integer(int, last_ret, last_ret)
		lttng_ust_field_integer(unsigned int, nr_writers, nr_writers)
		lttng_ust_field_sequence_hex(uint8_t, key, key, size_t, key_len)
	)
)

/*
 * One event per retry of a mutation op.  It carries the loop variable.  The
 * rate is high by construction.  Enable it only when the violation alone
 * has not named the site (see the tracing skill's "enable by hypothesis").
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, op_retry_step,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		unsigned int, op,
		unsigned int, attempts,
		int, last_ret
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer(unsigned int, op, op)
		lttng_ust_field_integer(unsigned int, attempts, attempts)
		lttng_ust_field_integer(int, last_ret, last_ret)
	)
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(cds_ft, ft_key_event_class,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const uint8_t *, key,
		size_t, key_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_sequence_hex(uint8_t, key, key, size_t, key_len)
	)
)

/*
 * Iter-keyed class for public APIs that act through an iterator.
 * Each event carries ft and iter, even though iter is bound to ft at
 * creation time.  A flight-recorder snapshot is finite.  The
 * iter_create event that links iter -> ft may have scrolled out of
 * the ring buffer when the snapshot is taken.  With ft in each event,
 * every event is self-contained.
 */
LTTNG_UST_TRACEPOINT_EVENT_CLASS(cds_ft, ft_iter_key_event_class,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, iter,
		const uint8_t *, key,
		size_t, key_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, iter, (uintptr_t) iter)
		lttng_ust_field_sequence_hex(uint8_t, key, key, size_t, key_len)
	)
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(cds_ft, ft_status_event_class,
	LTTNG_UST_TP_ARGS(
		int, status
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_status, int, status, status)
	)
)

LTTNG_UST_TRACEPOINT_EVENT_CLASS(cds_ft, ft_node_event_class,
	LTTNG_UST_TP_ARGS(
		const void *, node
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
	)
)

/*
 * Iter lifecycle/state class.  Every event carries both ft and iter.
 * A flight-recorder snapshot then stays self-contained even when the
 * iter_create event has rotated out of the ring buffer.
 */
LTTNG_UST_TRACEPOINT_EVENT_CLASS(cds_ft, ft_iter_event_class,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, iter
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, iter, (uintptr_t) iter)
	)
)

/*
 * Compressed-node mutation events.
 */

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, compressed_publish,
	LTTNG_UST_TP_ARGS(
		const void *, cn,
		unsigned int, len,
		const uint8_t *, key_bytes,
		const void *, child,
		const void *, parent
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, cn, (uintptr_t) cn)
		lttng_ust_field_integer(unsigned int, len, len)
		lttng_ust_field_sequence_hex(uint8_t, key_bytes, key_bytes,
			unsigned int, len)
		lttng_ust_field_integer_hex(uintptr_t, child, (uintptr_t) child)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, child_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) child))
		lttng_ust_field_integer_hex(uintptr_t, parent, (uintptr_t) parent)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, compressed_split,
	LTTNG_UST_TP_ARGS(
		const char *, kind,
		const void *, old_cn,
		unsigned int, old_len,
		const void *, new_top,
		unsigned int, diverge_pos
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(kind, kind)
		lttng_ust_field_integer_hex(uintptr_t, old_cn, (uintptr_t) old_cn)
		lttng_ust_field_integer(unsigned int, old_len, old_len)
		lttng_ust_field_integer_hex(uintptr_t, new_top, (uintptr_t) new_top)
		lttng_ust_field_integer(unsigned int, diverge_pos, diverge_pos)
	)
)

LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_node_event_class, cds_ft,
	compressed_free,
	LTTNG_UST_TP_ARGS(const void *, node))

/*
 * Lifecycle events that bind ft to its group.  A trace consumer uses
 * group_create + ft_create to build an ft -> group map.  It then
 * filters the reconstruction by group.  The group is the natural
 * reconstruction scope: all cross-ft subtree moves via graft /
 * graft_swap are bounded to one group.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, group_create,
	LTTNG_UST_TP_ARGS(
		const void *, ft_group
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft_group, (uintptr_t) ft_group)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, group_destroy,
	LTTNG_UST_TP_ARGS(
		const void *, ft_group
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft_group, (uintptr_t) ft_group)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ft_create,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, ft_group
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, ft_group, (uintptr_t) ft_group)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ft_destroy,
	LTTNG_UST_TP_ARGS(
		const void *, ft
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
	)
)

/*
 * root_publish: the trie's ft->root slot has been updated.
 *
 * Emitted at every site that writes ft->root.  This includes:
 * - the initial cds_ft_alloc() assignment;
 * - all graft/swap/detach paths that
 *   rcu_assign_pointer(ft->root, ...);
 * - implicitly, ft_publish_to_parent() when parent_nf is NULL.
 *   A NULL parent_nf indicates a root-slot publish, which has no
 *   structural parent node.
 *
 * The root_kind field gives the kind of the new root (internal,
 * compressed, external).  It uses the same enum as tree_edge_set's
 * parent_kind/child_kind.  The consumer can then tag the root node
 * correctly without observing a separate event.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, root_publish,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, root
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, root, (uintptr_t) root)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, root_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) root))
	)
)

/*
 * Structural edge change: a parent node's child slot at ordinal
 * byte `key_byte` is now `child`.  Fired after ft_node_set_nth()
 * and ft_node_replace_ptr() succeed.  These are the functions
 * where the real (parent, key_byte, child, parent_level) is
 * unambiguously known.  child == NULL means the slot was cleared
 * (detach / removal).
 *
 * A consumer should use this tracepoint to reconstruct the trie
 * topology.  publish_to_parent is a lower-level event about the
 * underlying publish primitive.  Its parent_nf is tied to
 * skip-pointer bookkeeping, not the structural parent.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, tree_edge_set,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, parent,
		unsigned int, parent_level,
		uint8_t, key_byte,
		const void *, child
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, parent, (uintptr_t) parent)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, parent_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) parent))
		lttng_ust_field_integer(unsigned int, parent_level, parent_level)
		lttng_ust_field_integer(uint8_t, key_byte, key_byte)
		lttng_ust_field_integer_hex(uintptr_t, child, (uintptr_t) child)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, child_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) child))
		lttng_ust_field_integer(uint16_t, child_skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) child))
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, publish_to_parent,
	LTTNG_UST_TP_ARGS(
		const void *, parent_nf,
		const void *, slot,
		const void *, old_child,
		const void *, new_child
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, parent_nf, (uintptr_t) parent_nf)
		lttng_ust_field_integer_hex(uintptr_t, slot, (uintptr_t) slot)
		lttng_ust_field_integer_hex(uintptr_t, old_child, (uintptr_t) old_child)
		lttng_ust_field_integer_hex(uintptr_t, new_child, (uintptr_t) new_child)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, split_compressed_insert_enter,
	LTTNG_UST_TP_ARGS(
		const void *, cn,
		unsigned int, len,
		unsigned int, diverge_pos
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, cn, (uintptr_t) cn)
		lttng_ust_field_integer(unsigned int, len, len)
		lttng_ust_field_integer(unsigned int, diverge_pos, diverge_pos)
	)
)

/*
 * Inequality lookup events.
 */

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_enter,
	LTTNG_UST_TP_ARGS(
		int, mode,
		const uint8_t *, key,
		size_t, key_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
		lttng_ust_field_sequence_hex(uint8_t, key, key, size_t, key_len)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_compressed_enter,
	LTTNG_UST_TP_ARGS(
		const void *, cn,
		int, level,
		int, mode
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, cn, (uintptr_t) cn)
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_compressed,
	LTTNG_UST_TP_ARGS(
		const void *, cn,
		int, level,
		int, cmp_result,
		unsigned int, mpos,
		int, action
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, cn, (uintptr_t) cn)
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_integer(int, cmp_result, cmp_result)
		lttng_ust_field_integer(unsigned int, mpos, mpos)
		lttng_ust_field_enum(cds_ft, ft_compressed_action, int, action, action)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_going_up_step,
	LTTNG_UST_TP_ARGS(
		int, level,
		const void *, path_entry,
		int, found_sibling,
		uint8_t, ord_key
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_integer_hex(uintptr_t, path_entry, (uintptr_t) path_entry)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, path_entry_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) path_entry))
		lttng_ust_field_integer(uint16_t, path_entry_skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) path_entry))
		lttng_ust_field_integer(int, found_sibling, found_sibling)
		lttng_ust_field_integer(uint8_t, ord_key, ord_key)
	)
)

/*
 * No-key variant of ineq_going_up_step for the speculative limit-none lookup
 * (use_keycopy && limit == LIMIT_NONE).  That path does not maintain
 * ordinal_key: the result key comes from the matched leaf.  So the dispatch
 * byte is unavailable.  This variant omits the ord_key field rather than
 * report a synthesized value.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_going_up_step_nokey,
	LTTNG_UST_TP_ARGS(
		int, level,
		const void *, path_entry,
		int, found_sibling
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_integer_hex(uintptr_t, path_entry, (uintptr_t) path_entry)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, path_entry_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) path_entry))
		lttng_ust_field_integer(uint16_t, path_entry_skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) path_entry))
		lttng_ust_field_integer(int, found_sibling, found_sibling)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ineq_result,
	LTTNG_UST_TP_ARGS(
		int, mode,
		const uint8_t *, query,
		size_t, query_len,
		const uint8_t *, result,
		size_t, result_len,
		int, status
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
		lttng_ust_field_sequence_hex(uint8_t, query, query, size_t, query_len)
		lttng_ust_field_sequence_hex(uint8_t, result, result, size_t, result_len)
		lttng_ust_field_enum(cds_ft, ft_status, int, status, status)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, fastpath_enter,
	LTTNG_UST_TP_ARGS(
		int, mode,
		const void *, node_flag,
		int, level_after
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
		lttng_ust_field_integer_hex(uintptr_t, node_flag, (uintptr_t) node_flag)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, node_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) node_flag))
		lttng_ust_field_integer(uint16_t, skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) node_flag))
		lttng_ust_field_integer(int, level_after, level_after)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, post_traversal,
	LTTNG_UST_TP_ARGS(
		int, mode,
		int, level,
		const void *, node_flag
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_integer_hex(uintptr_t, node_flag, (uintptr_t) node_flag)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, node_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) node_flag))
		lttng_ust_field_integer(uint16_t, skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) node_flag))
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, slowpath_enter,
	LTTNG_UST_TP_ARGS(
		int, mode,
		int, cache_valid,
		int, path_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_enum(cds_ft, ft_lookup_mode, int, mode, mode)
		lttng_ust_field_integer(int, cache_valid, cache_valid)
		lttng_ust_field_integer(int, path_len, path_len)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, slowpath_step,
	LTTNG_UST_TP_ARGS(
		int, level,
		uint8_t, key_value,
		const void *, node_flag,
		int, broke
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, level, level)
		lttng_ust_field_integer(uint8_t, key_value, key_value)
		lttng_ust_field_integer_hex(uintptr_t, node_flag, (uintptr_t) node_flag)
		lttng_ust_field_enum(cds_ft, ft_tp_node_kind, uint16_t, node_kind,
			ft_tp_node_kind((struct cds_ft_inode_flag *) node_flag))
		lttng_ust_field_integer(uint16_t, skip_len,
			ft_tp_node_skip_len((struct cds_ft_inode_flag *) node_flag))
		lttng_ust_field_integer(int, broke, broke)
	)
)

/*
 * Mis-wire violation: a compressed-tagged edge whose target fails the
 * identity round-trip.
 *
 * Fields:
 * - @site: the consumption point;
 * - @edge: the tagged value the descent held;
 * - @target: the derived compressed-node pointer;
 * - @len: the path length of @target (possibly garbage);
 * - @state: the target's resolved state word (tombstone bit =
 *   recycled/retired);
 * - @rt_parent/@rt_val: the round-trip through the target's own
 *   (parent, offset).
 *
 * The tag bits of @rt_val tell how the TREE currently wires this address.
 * An internal tag proves that the compressed edge is mis-tagged.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, miswire,
	LTTNG_UST_TP_ARGS(
		unsigned int, site,
		const void *, edge,
		const void *, target,
		unsigned int, len,
		uintptr_t, state,
		const void *, rt_parent,
		const void *, rt_val
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned int, site, site)
		lttng_ust_field_integer_hex(uintptr_t, edge, (uintptr_t) edge)
		lttng_ust_field_integer_hex(uintptr_t, target, (uintptr_t) target)
		lttng_ust_field_integer(unsigned int, len, len)
		lttng_ust_field_integer_hex(uintptr_t, state, state)
		lttng_ust_field_integer_hex(uintptr_t, rt_parent, (uintptr_t) rt_parent)
		lttng_ust_field_integer_hex(uintptr_t, rt_val, (uintptr_t) rt_val)
	)
)

/* One recorded txn edge {slot: old -> new} (records install at commit). */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, edge_record,
	LTTNG_UST_TP_ARGS(
		const void *, txn,
		const void *, slot,
		const void *, old,
		const void *, new_val,
		uintptr_t, tag
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, txn, (uintptr_t) txn)
		lttng_ust_field_integer_hex(uintptr_t, slot, (uintptr_t) slot)
		lttng_ust_field_integer_hex(uintptr_t, old, (uintptr_t) old)
		lttng_ust_field_integer_hex(uintptr_t, new_val, (uintptr_t) new_val)
		lttng_ust_field_integer_hex(uintptr_t, tag, tag)
	)
)

/* A lone-edge on-stack release store (no txn -- installs immediately). */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, edge_lone,
	LTTNG_UST_TP_ARGS(
		const void *, slot,
		const void *, old,
		const void *, new_val
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, slot, (uintptr_t) slot)
		lttng_ust_field_integer_hex(uintptr_t, old, (uintptr_t) old)
		lttng_ust_field_integer_hex(uintptr_t, new_val, (uintptr_t) new_val)
	)
)

/*
 * Pass-4 torn-navigation campaign: bracket the one-commit predecessor search
 * (the seeded LT).
 *
 * Fields:
 * - @phase: 0 = first search, 1 = confirm search;
 * - @pred_cell / @pred_head: the result (0 = new minimum);
 * - @key0: the first 8 key bytes, big-endian.  It correlates with the
 *   oracle's u64 keys.
 *
 * The rate is low (2 per list-on insert).  The pair of events timestamps
 * the search window.  The surrounding edge_record / txn_commit breadcrumbs
 * then show exactly which peer commits ran inside that window.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, pred_search,
	LTTNG_UST_TP_ARGS(
		uint64_t, key0,
		int, phase,
		int, from_root,
		const void *, pred_cell,
		const void *, pred_head
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uint64_t, key0, key0)
		lttng_ust_field_integer(int, phase, phase)
		lttng_ust_field_integer(int, from_root, from_root)
		lttng_ust_field_integer_hex(uintptr_t, pred_cell, (uintptr_t) pred_cell)
		lttng_ust_field_integer_hex(uintptr_t, pred_head, (uintptr_t) pred_head)
	)
)

/*
 * Pass-4 campaign: the outcome of a point remove.  The event is keyed by the
 * head NODE pointer, like pred_search's pred_head.  It makes a writer's own
 * removes visible in the window.  Without it a search result can look stale
 * when the key was in fact legally removed moments earlier.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, remove_head_exit,
	LTTNG_UST_TP_ARGS(
		const void *, head,
		int, status
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, head, (uintptr_t) head)
		lttng_ust_field_integer(int, status, status)
	)
)

/* Commit outcome: the recorded edges of @txn installed iff status == 0. */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, txn_commit,
	LTTNG_UST_TP_ARGS(
		const void *, txn,
		int, status
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, txn, (uintptr_t) txn)
		lttng_ust_field_integer(int, status, status)
	)
)

/*
 * Fatal-signal context (SIGSEGV class).  The test's sigaction handler emits
 * it before the snapshot dump.  It carries the faulting address (siginfo
 * si_addr) and the instruction pointer (sigframe REG_RIP).  The fault then
 * correlates by address with the breadcrumb window in the same timeline.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, fatal_signal,
	LTTNG_UST_TP_ARGS(
		int, signo,
		const void *, addr,
		const void *, ip
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, signo, signo)
		lttng_ust_field_integer_hex(uintptr_t, addr, (uintptr_t) addr)
		lttng_ust_field_integer_hex(uintptr_t, ip, (uintptr_t) ip)
	)
)

/* Arena item lifecycle: kind 0 = internal node, 1 = compressed node. */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, item_alloc,
	LTTNG_UST_TP_ARGS(
		const void *, item,
		unsigned int, kind,
		unsigned int, extra
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, item, (uintptr_t) item)
		lttng_ust_field_integer(unsigned int, kind, kind)
		lttng_ust_field_integer(unsigned int, extra, extra)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, item_free,
	LTTNG_UST_TP_ARGS(
		const void *, item,
		unsigned int, kind
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, item, (uintptr_t) item)
		lttng_ust_field_integer(unsigned int, kind, kind)
	)
)

/*
 * THE ACTUAL FREELIST PUSH, with the route that reached it.  item_free fires at
 * the RETIRE, before the defer decision.  So it cannot answer "did this one
 * pay a grace period".  This event can.  That is the whole question when a
 * reader faults on a link into reclaimed memory.
 *
 * @via is FT_DBG_VIA_*:
 * - 1: rcu_callback (GP paid);
 * - 2: exclusive;
 * - 3: unpublished (immediate);
 * - 4: reserve_drain (immediate).
 *
 * ☠ Emitted with the ITEM pointer, not the metadata.  item_alloc / item_free
 * use the item address.  Keying this event differently once cost a whole
 * analysis: a grep for the victim returned nothing and read as "never
 * reclaimed".
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, item_reclaim,
	LTTNG_UST_TP_ARGS(
		const void *, item,
		unsigned int, via
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, item, (uintptr_t) item)
		lttng_ust_field_integer(unsigned int, via, via)
	)
)

/*
 * THE READER-SIDE VIOLATION, fired at DETECTION rather than from a signal
 * handler.  The prior rig recorded that a SIGSEGV-handler snapshot fires long
 * after the ring has wrapped.
 *
 * ft_get_parent_rcu already asserts that a parent is never EXTERNAL.  A
 * freelist link (8-mod-16) clears that tag.  So the check is exactly where
 * the corruption becomes knowable.
 *
 * The event is self-diagnosing.  @rt round-trips the parent through
 * item->metadata->item:
 * - rt == parent: a valid live object;
 * - rt != parent: RECYCLED memory.
 * That single field separates use-after-free from a mis-wired link before
 * the window is even read.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, parent_external_violation,
	LTTNG_UST_TP_ARGS(
		const void *, node,
		const void *, parent,
		const void *, rt,
		unsigned long, pw
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
		lttng_ust_field_integer_hex(uintptr_t, parent, (uintptr_t) parent)
		lttng_ust_field_integer_hex(uintptr_t, rt, (uintptr_t) rt)
		lttng_ust_field_integer_hex(uintptr_t, pw, (unsigned long) pw)
	)
)

/*
 * TEMPORARY (MW firing-path attribution): the call_rcu RETIRE of a published
 * internal node.  The event carries the return address of free_cds_ft_node's
 * caller.  The exact retiring site can then be read from the snapshot with
 * addr2line.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, item_retire,
	LTTNG_UST_TP_ARGS(
		const void *, item,
		const void *, caller
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, item, (uintptr_t) item)
		lttng_ust_field_integer_hex(uintptr_t, caller, (uintptr_t) caller)
	)
)

/*
 * Test violation event.  query/returned are user-facing u64 values
 * already decoded by cds_ft_key_to_u64() in the test; not raw key
 * byte sequences.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, violation,
	LTTNG_UST_TP_ARGS(
		uint64_t, query,
		uint64_t, returned
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(uint64_t, query, query)
		lttng_ust_field_integer(uint64_t, returned, returned)
	)
)

/*
 * Public API entries with key byte-sequence.
 */
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	insert_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	insert_unique_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	insert_replace_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	lookup_key_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	graft_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	graft_swap_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_key_event_class, cds_ft,
	detach_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const uint8_t *, key, size_t, key_len))

/*
 * Merge: keyless entry, status exit (defined below in the status-only block).
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, merge_enter,
	LTTNG_UST_TP_ARGS(const void *, dst_ft, const void *, src_ft),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, dst_ft, (uintptr_t) dst_ft)
		lttng_ust_field_integer_hex(uintptr_t, src_ft, (uintptr_t) src_ft)
	)
)

/* Iter-keyed public APIs: emit both ft and iter for snapshot safety. */
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_key_event_class, cds_ft,
	remove_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter,
		const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_key_event_class, cds_ft,
	replace_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter,
		const uint8_t *, key, size_t, key_len))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_key_event_class, cds_ft,
	lookup_enter,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter,
		const uint8_t *, key, size_t, key_len))

/* Public API exits (status enum). */
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	insert_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	insert_unique_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	insert_replace_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	remove_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	replace_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	graft_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	graft_swap_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	detach_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	merge_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	lookup_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	lookup_key_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	lookup_nth_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	lookup_nth_last_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	iter_skip_forward_exit,
	LTTNG_UST_TP_ARGS(int, status))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	iter_skip_reverse_exit,
	LTTNG_UST_TP_ARGS(int, status))

/*
 * Rank/skip ops -- the "n" parameter is a numeric count, not a key.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lookup_nth_enter,
	LTTNG_UST_TP_ARGS(unsigned long, nth),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, nth, nth)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lookup_nth_last_enter,
	LTTNG_UST_TP_ARGS(unsigned long, nth),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, nth, nth)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, iter_skip_forward_enter,
	LTTNG_UST_TP_ARGS(unsigned long, skip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, skip, skip)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, iter_skip_reverse_enter,
	LTTNG_UST_TP_ARGS(unsigned long, skip),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, skip, skip)
	)
)

/* Counting ops. */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, count_entries,
	LTTNG_UST_TP_ARGS(unsigned long, count),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, count, count)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, count_prefix,
	LTTNG_UST_TP_ARGS(unsigned long, count),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned long, count, count)
	)
)

/* Iterator lifecycle / state events: (ft, iter) pair on every event. */
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_event_class, cds_ft,
	iter_create,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_event_class, cds_ft,
	iter_destroy,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_event_class, cds_ft,
	iter_bind_key,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter))
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_iter_event_class, cds_ft,
	iter_reset,
	LTTNG_UST_TP_ARGS(const void *, ft, const void *, iter))

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, iter_set_key_enter,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, iter,
		const uint8_t *, key,
		size_t, key_len,
		int, prev_key_len,
		int, prev_path_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, iter, (uintptr_t) iter)
		lttng_ust_field_sequence_hex(uint8_t, key, key, size_t, key_len)
		lttng_ust_field_integer(int, prev_key_len, prev_key_len)
		lttng_ust_field_integer(int, prev_path_len, prev_path_len)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, iter_set_key_exit,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, iter,
		int, path_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, iter, (uintptr_t) iter)
		lttng_ust_field_integer(int, path_len, path_len)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, iter_set_prefix_len,
	LTTNG_UST_TP_ARGS(
		const void *, ft,
		const void *, iter,
		int, prefix_len
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, ft, (uintptr_t) ft)
		lttng_ust_field_integer_hex(uintptr_t, iter, (uintptr_t) iter)
		lttng_ust_field_integer(int, prefix_len, prefix_len)
	)
)

/* Internal mutation primitives. */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, attach_node_enter,
	LTTNG_UST_TP_ARGS(
		const void *, attach_node,
		const void *, old_node,
		unsigned int, level
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, attach_node, (uintptr_t) attach_node)
		lttng_ust_field_integer_hex(uintptr_t, old_node, (uintptr_t) old_node)
		lttng_ust_field_integer(unsigned int, level, level)
	)
)
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	attach_node_exit,
	LTTNG_UST_TP_ARGS(int, status))

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, detach_node_enter,
	LTTNG_UST_TP_ARGS(
		const void *, detach_node,
		unsigned int, depth
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, detach_node, (uintptr_t) detach_node)
		lttng_ust_field_integer(unsigned int, depth, depth)
	)
)
LTTNG_UST_TRACEPOINT_EVENT_INSTANCE(cds_ft, ft_status_event_class, cds_ft,
	detach_node_exit,
	LTTNG_UST_TP_ARGS(int, status))

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, node_recompact,
	LTTNG_UST_TP_ARGS(
		const void *, old_node,
		const void *, new_node,
		const void *, new_meta,
		int, new_type
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, old_node, (uintptr_t) old_node)
		lttng_ust_field_integer_hex(uintptr_t, new_node, (uintptr_t) new_node)
		/*
		 * @new_meta MAPS THE LOCK WORD TO THE NODE.  Every acquire and
		 * release in the trace is an edge on &meta->state.  Without
		 * this pairing a lock take cannot be attributed to the node it
		 * protects.  That is exactly the question when two writers
		 * serialize correctly and the key is lost anyway.
		 */
		lttng_ust_field_integer_hex(uintptr_t, new_meta, (uintptr_t) new_meta)
		lttng_ust_field_integer(int, new_type, new_type)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, chain_node,
	LTTNG_UST_TP_ARGS(
		const void *, last_node,
		const void *, node
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, last_node, (uintptr_t) last_node)
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, unchain_node,
	LTTNG_UST_TP_ARGS(
		const void *, head_slot,
		const void *, node,
		int, was_head
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, head_slot, (uintptr_t) head_slot)
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
		lttng_ust_field_integer(int, was_head, was_head)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, set_parent,
	LTTNG_UST_TP_ARGS(
		const void *, child,
		const void *, parent
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, child, (uintptr_t) child)
		lttng_ust_field_integer_hex(uintptr_t, parent, (uintptr_t) parent)
	)
)
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, metadata_set_external_nodes,
	LTTNG_UST_TP_ARGS(
		const void *, node,
		const void *, external_nodes
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
		lttng_ust_field_integer_hex(uintptr_t, external_nodes, (uintptr_t) external_nodes)
	)
)

/*
 * Two-writer external-head campaign (2026-09-04).  The event records
 * ft_node_recompact's COW of the retiring holder's FORWARD head word.  The
 * COW is a PLAIN read of @old_meta->external_nodes and a PLAIN store into
 * the unpublished copy @new_nf.  It runs in no txn at all, so no edge_record
 * covers it.
 *
 * This is the one place the exclusion argument has a code-fact hole.  Pair
 * this event with the edge_record/txn_commit of the peer's head write to
 * date the loss.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, recompact_head_snap,
	LTTNG_UST_TP_ARGS(
		const void *, old_meta,
		const void *, new_nf,
		const void *, val
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, old_meta, (uintptr_t) old_meta)
		lttng_ust_field_integer_hex(uintptr_t, new_nf, (uintptr_t) new_nf)
		lttng_ust_field_integer_hex(uintptr_t, val, (uintptr_t) val)
	)
)

/*
 * Every entry into ft_chain_compress_fused, with the ARGUMENTS that identify
 * which of its four callers ran.  The boundary free (ft-remove.h) is a single
 * line shared by all four.  So item_retire's return address cannot tell them
 * apart.
 *
 * @ext is @iter_meta->external_nodes read AT ENTRY.  It is the plan-time
 * value of the word the disposal predicate rests on.  The predicate never
 * re-validates that word.  Pair @ext with the peer's park in the same trace
 * to date the loss.
 *
 * Caller discriminators:
 * - plan_nr_child 2 => the shape-D detach fold;
 * - otherwise, dead_cell/freeze_leaf both NULL =>
 *   ft_canonicalize_chain_compress;
 * - otherwise, freeze_leaf set => the leaf-removal collapse;
 * - otherwise, dead_cell alone => the prefix chain-head collapse.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, chain_compress_enter,
	LTTNG_UST_TP_ARGS(
		const void *, boundary,
		const void *, meta,
		const void *, ext,
		const void *, freeze_leaf,
		const void *, dead_cell,
		unsigned int, plan_nr_child,
		int, record_only
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, boundary, (uintptr_t) boundary)
		lttng_ust_field_integer_hex(uintptr_t, meta, (uintptr_t) meta)
		lttng_ust_field_integer_hex(uintptr_t, ext, (uintptr_t) ext)
		lttng_ust_field_integer_hex(uintptr_t, freeze_leaf, (uintptr_t) freeze_leaf)
		lttng_ust_field_integer_hex(uintptr_t, dead_cell, (uintptr_t) dead_cell)
		lttng_ust_field_integer(unsigned int, plan_nr_child, plan_nr_child)
		lttng_ust_field_integer(int, record_only, record_only)
	)
)

/*
 * The same op at its COMMIT boundary.  @ext_now is the word re-read just
 * before the merged node is published.  A pair (enter.ext, exit.ext_now)
 * that DIFFERS is the stale predicate caught in the act.  @status 0 == the
 * collapse committed and the boundary was retired.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, chain_compress_exit,
	LTTNG_UST_TP_ARGS(
		const void *, boundary,
		const void *, meta,
		const void *, ext_now,
		int, status
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, boundary, (uintptr_t) boundary)
		lttng_ust_field_integer_hex(uintptr_t, meta, (uintptr_t) meta)
		lttng_ust_field_integer_hex(uintptr_t, ext_now, (uintptr_t) ext_now)
		lttng_ust_field_integer(int, status, status)
	)
)

/*
 * Harness-side violation, emitted through the library so the in-process FAST
 * STOP (FT_TRACE_FREEZE) runs before the fork+exec of `lttng snapshot record`.
 *
 * @kind names the oracle shape:
 * - 0: RM-LOOKUP-MISS;
 * - 1: RM-FAIL;
 * - 2: STALE-FOUND;
 * - 3: RM-WRONG-NODE;
 * - 4: DUP-CHAINED.
 * @node is the key's own external node.
 *
 * ☠ TEN ARGUMENT PAIRS IS THE LTTNG-UST CEILING for LTTNG_UST_TP_ARGS.
 * Exceeding it fails as an implicit-declaration error on the generated
 * callback.  The error does not name the limit.  @prev (the head's cell) was
 * dropped for @parent: the cell is derivable from @holder, the parent is not
 * derivable from anything else in the record.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, ext_violation,
	LTTNG_UST_TP_ARGS(
		unsigned int, kind,
		const void *, node,
		const void *, holder,
		const void *, hext,
		uint64_t, hstate,
		unsigned int, rmsite,
		int, anc_tomb,
		int, anc_root,
		const void *, self,
		uint64_t, key0
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(unsigned int, kind, kind)
		lttng_ust_field_integer_hex(uintptr_t, node, (uintptr_t) node)
		lttng_ust_field_integer_hex(uintptr_t, holder, (uintptr_t) holder)
		lttng_ust_field_integer_hex(uintptr_t, hext, (uintptr_t) hext)
		lttng_ust_field_integer_hex(uint64_t, hstate, hstate)
		lttng_ust_field_integer(unsigned int, rmsite, rmsite)
		/*
		 * ☠ ONE LEVEL IS NOT ENOUGH.  The disposal that loses a key can
		 * be at the holder, at its parent, or anywhere above.  The
		 * whole cluster below a retired node is alive, correctly wired
		 * and holding the key.  It is also unreachable.  A classifier
		 * that inspects a fixed number of levels invents a residual
		 * shape for every defect that happens one level higher than it
		 * looks.
		 *
		 * @anc_tomb: hops from the holder to the nearest TOMBSTONED
		 * ancestor (0 == the holder itself), -1 if none up to the root.
		 * @anc_root: hops for the up-walk to reach the trie, -1 if it
		 * never does.  -1 states directly that the cluster is
		 * DETACHED.  That is exactly what a lookup miss means.
		 */
		lttng_ust_field_integer(int, anc_tomb, anc_tomb)
		lttng_ust_field_integer(int, anc_root, anc_root)
		lttng_ust_field_integer_hex(uintptr_t, self, (uintptr_t) self)
		lttng_ust_field_integer_hex(uint64_t, key0, key0)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, inv_violation,
	LTTNG_UST_TP_ARGS(
		const char *, test_name,
		const char *, message
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_string(test, test_name)
		lttng_ust_field_string(msg, message)
	)
)

/*
 * Skip-reanchor up-walk diagnostics (root-cause scaffolding for the
 * dangling-skip-slot UAF).  @skip_ptr is the raw skip-compressed slot
 * value; @cur is the decoded skip child (the node whose parent chain is
 * walked); @want the encoded skip length.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, reanchor_enter,
	LTTNG_UST_TP_ARGS(
		const void *, skip_ptr,
		const void *, cur,
		unsigned int, want
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, skip_ptr, (uintptr_t) skip_ptr)
		lttng_ust_field_integer_hex(uintptr_t, cur, (uintptr_t) cur)
		lttng_ust_field_integer(unsigned int, want, want)
	)
)

/* One up-walk step: @cur's resolved @parent at accumulated span @acc. */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, reanchor_walk,
	LTTNG_UST_TP_ARGS(
		const void *, cur,
		const void *, parent,
		unsigned int, acc
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, cur, (uintptr_t) cur)
		lttng_ust_field_integer_hex(uintptr_t, parent, (uintptr_t) parent)
		lttng_ust_field_integer(unsigned int, acc, acc)
	)
)

/*
 * Phase D.2, the DLM anchor rig.  It answers: who holds the word a starving
 * remove keeps hitting, and for how long.
 *
 * - Take/drop are keyed on the ANCHOR word.  The analysis can then grep one
 *   address across every thread.
 * - @op_bound says whether the taker's lock ctx had a domain-bound op (the
 *   enrolment discriminator).
 * - The starved violation carries the victim's streak and the registry's
 *   last-taker.  This makes it self-diagnosing.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, dlm_take,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		const char *, fn,
		int, line,
		int, op_bound
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_string(fn, fn)
		lttng_ust_field_integer(int, line, line)
		lttng_ust_field_integer(int, op_bound, op_bound)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, dlm_drop,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, state
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, state, state)
	)
)

/*
 * FINDING B (the E.2 oracle's second catch).  A violation says two ops covered
 * one node; these say WHICH hold-tracking entry outlived its hold.
 *
 * The pair that matters is stamp_note (the FILING key) against stamp_drop
 * (the RELEASE key).  An entry is filed under the anchor the acquire
 * DERIVED.  It is dropped under the anchor the release derives.  If those
 * two derivations disagree, the entry survives the release, and so does the
 * owner stamp it carries.
 *
 * @nmatch makes that visible without cross-referencing.  A drop that
 * matched NOTHING while the thread still holds entries is the disagreement,
 * caught in the act.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, stamp_note,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		const void *, member,
		int, shared,
		const char *, fn,
		int, line
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(uintptr_t, member,
			(uintptr_t) member)
		lttng_ust_field_integer(int, shared, shared)
		lttng_ust_field_string(fn, fn)
		lttng_ust_field_integer(int, line, line)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, stamp_drop,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, state,
		int, nmatch,
		int, nheld
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, state, state)
		lttng_ust_field_integer(int, nmatch, nmatch)
		lttng_ust_field_integer(int, nheld, nheld)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, stamp_yield,
	LTTNG_UST_TP_ARGS(
		const void *, member,
		unsigned long, old_tid
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, member,
			(uintptr_t) member)
		lttng_ust_field_integer_hex(unsigned long, old_tid, old_tid)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, stamp_violation,
	LTTNG_UST_TP_ARGS(
		const void *, member,
		const void *, anchor,
		const void *, owner_anchor,
		unsigned long, owner_tid,
		int, shared,
		int, ledger_holds,
		const char *, fn,
		int, line
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, member,
			(uintptr_t) member)
		lttng_ust_field_integer_hex(uintptr_t, anchor,
			(uintptr_t) anchor)
		lttng_ust_field_integer_hex(uintptr_t, owner_anchor,
			(uintptr_t) owner_anchor)
		lttng_ust_field_integer_hex(unsigned long, owner_tid, owner_tid)
		lttng_ust_field_integer(int, shared, shared)
		lttng_ust_field_integer(int, ledger_holds, ledger_holds)
		lttng_ust_field_string(fn, fn)
		lttng_ust_field_integer(int, line, line)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, dlm_long_hold,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, wall_us,
		const char *, fn,
		int, line,
		long, nvcsw,
		long, nivcsw
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer(unsigned long, wall_us, wall_us)
		lttng_ust_field_string(fn, fn)
		lttng_ust_field_integer(int, line, line)
		lttng_ust_field_integer(long, nvcsw, nvcsw)
		lttng_ust_field_integer(long, nivcsw, nivcsw)
	)
)

LTTNG_UST_TRACEPOINT_EVENT(cds_ft, remove_anchor_starved,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, state,
		unsigned int, streak,
		unsigned int, attempts,
		const char *, taker_fn,
		int, taker_line,
		int, taker_bound,
		unsigned long, take_age_us
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, state, state)
		lttng_ust_field_integer(unsigned int, streak, streak)
		lttng_ust_field_integer(unsigned int, attempts, attempts)
		lttng_ust_field_string(taker_fn, taker_fn)
		lttng_ust_field_integer(int, taker_line, taker_line)
		lttng_ust_field_integer(int, taker_bound, taker_bound)
		lttng_ust_field_integer(unsigned long, take_age_us, take_age_us)
	)
)

/*
 * A LOCK BIT CLEARED BY A THREAD THAT DID NOT TAKE IT.  The late symptom of
 * this defect is lock_release_violation.  The bit-keyed detector emits this
 * event before it dumps the flight recorder.  The snapshot then ends with
 * this event and carries the word's whole lock_ev history behind it.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lock_stolen,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned int, thief_tid,
		const void *, thief_pc,
		unsigned int, owner_tid,
		const void *, owner_pc
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer(unsigned int, thief_tid, thief_tid)
		lttng_ust_field_integer_hex(uintptr_t, thief_pc, (uintptr_t) thief_pc)
		lttng_ust_field_integer(unsigned int, owner_tid, owner_tid)
		lttng_ust_field_integer_hex(uintptr_t, owner_pc, (uintptr_t) owner_pc)
	)
)

/*
 * THE VIOLATION EVENT for ft_meta_lock_release finding its word already
 * unlocked.  It is emitted BEFORE the capture freezes emission.  So it is
 * the LAST event in the ring, not the first one its own flag drops
 * (fractal-trie-trace.h states the order).  @pc is the releasing caller.
 * The lock_ev stream cannot name that caller.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lock_release_violation,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, state,
		const void *, pc,
		unsigned int, tid
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, state, state)
		lttng_ust_field_integer_hex(uintptr_t, pc, (uintptr_t) pc)
		lttng_ust_field_integer(unsigned int, tid, tid)
	)
)

/*
 * -DFT_DEBUG_LOCK_LEAK: one transition of a node/cell LOCK WORD (see enum
 * ft_ll_kind in ft-mutation-helpers.h for @kind and what @a / @b carry).
 *
 * The event is emitted twice:
 * - live at each transition (@replay 0);
 * - again from the per-lock history ring when the leak detector fires
 *   (@replay 1).
 * The replay is needed because the take that leaked is found late.  When
 * enough refusals have piled up to reveal it, that take is long out of a
 * flight-recorder window.
 *
 * @ts_ns is CLOCK_MONOTONIC at the transition.  LTTng itself stamps with
 * that clock, so replayed and live events interleave.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lock_ev,
	LTTNG_UST_TP_ARGS(
		int, replay,
		int, kind,
		const void *, lock,
		unsigned long, a,
		unsigned long, b,
		const void *, txn,
		const void *, pc,
		unsigned int, tid,
		uint64_t, ts_ns
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer(int, replay, replay)
		lttng_ust_field_integer(int, kind, kind)
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, a, a)
		lttng_ust_field_integer_hex(unsigned long, b, b)
		lttng_ust_field_integer_hex(uintptr_t, txn, (uintptr_t) txn)
		lttng_ust_field_integer_hex(uintptr_t, pc, (uintptr_t) pc)
		lttng_ust_field_integer(unsigned int, tid, tid)
		lttng_ust_field_integer(uint64_t, ts_ns, ts_ns)
	)
)

/*
 * -DFT_DEBUG_LOCK_LEAK: a writer has been refused the SAME lock word in the
 * SAME state for @refusals takes over @stuck_us.  So the word is held by
 * nobody who will release it.  Emitted after the word's history replay, right
 * before the snapshot and abort.
 */
LTTNG_UST_TRACEPOINT_EVENT(cds_ft, lock_leak_violation,
	LTTNG_UST_TP_ARGS(
		const void *, lock,
		unsigned long, state,
		unsigned long, refusals,
		unsigned long, stuck_us,
		const void *, pc
	),
	LTTNG_UST_TP_FIELDS(
		lttng_ust_field_integer_hex(uintptr_t, lock, (uintptr_t) lock)
		lttng_ust_field_integer_hex(unsigned long, state, state)
		lttng_ust_field_integer(unsigned long, refusals, refusals)
		lttng_ust_field_integer(unsigned long, stuck_us, stuck_us)
		lttng_ust_field_integer_hex(uintptr_t, pc, (uintptr_t) pc)
	)
)

#endif /* _FT_TP_H */

#include <lttng/tracepoint-event.h>

#endif /* FT_ENABLE_TRACING */
