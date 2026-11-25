// SPDX-FileCopyrightText: 2012-2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-only

#ifndef _URCU_FT_TRACE_H
#define _URCU_FT_TRACE_H

/*
 * src/fractal-trie-trace.h
 *
 * Userspace RCU library - Fractal Trie tracing macros
 *
 * FT_TP() and friends expand to LTTng-UST tracepoints when FT_ENABLE_TRACING
 * is defined.  Otherwise they expand to no-ops: the arguments are discarded
 * and no code is emitted.
 * The key-payload variants resolve the key length before the LTTng sequence
 * field serializes it.  These are macros.  So the symbols they reference need
 * to be in scope only at each FT_TP* use site in fractal-trie.c, not here.
 * These symbols are ft_key_len(), iter_key(), the iter fields and the
 * CDS_FT_LEN_* sentinels.
 *
 * Included by fractal-trie.c AFTER fractal-trie-internal.h.
 */

#include <stdio.h>
#include <urcu/uatomic.h>

/*
 * ★ WAS THE UNHELD CAPTURE ALREADY WRONG BY THE TIME THE LOCK LANDED?
 *
 * cds_ft_insert_unique used to answer -EEXIST from the descent's landing.  No
 * exclusion covered that word.  A TEST cannot measure whether that cost
 * anything.  The only observable is "the returned node is not on the key's
 * chain".  But a peer may legitimately remove the key between the call and
 * the check.  So the count mixes the defect with a benign race.
 *
 * ☠ AND THE COARSE ARM DOES NOT DISCRIMINATE IT.  The FT-wide lock also
 * serialises the TEST'S OWN follow-up walk: the peer must be woken and
 * scheduled after the release.  So coarse draws a near-zero for a reason that
 * has nothing to do with the verdict.  A control that differs from the arm in
 * more ways than the one being measured is not a control.
 *
 * The honest instrument lives INSIDE the op.  There the question is decided
 * under the holder lock, and the answer cannot change under it.  Compare the
 * capture against the re-read that the walk already performs.  A mismatch or a
 * retired capture means that the verdict taken at the OLD position WOULD HAVE
 * BEEN WRONG.  That is a fact, not a sample.  It costs one increment on a path
 * that is already aborting.
 */
#ifdef FT_DEBUG_UNIQUE_VERDICT
static unsigned long ft_dbg_unique_stale;	/* capture wrong under the lock */
static unsigned long ft_dbg_unique_ok;		/* capture still good */
/*
 * ☠ AND THE OTHER HALF, or the zero above is a WRONG ZERO.  @stale is counted
 * AFTER the holder acquire.  The acquire itself refuses a word with PROXY |
 * TOMBSTONE | LOCK.  So the cases the verdict most needs protection from bail
 * out BEFORE the re-validate runs, and are invisible there.  An example is a
 * same-key SOLE remove that RETIRED the holder.  Counted here, these cases are
 * the rest of the window.
 */
static unsigned long ft_dbg_unique_acqfail;
/*
 * ★ AND SPLIT THAT REFUSAL BY ITS BIT, or it over-claims.  ft_dlm_lock refuses
 * PROXY | TOMBSTONE | LOCK as one test.  Only one of the three says that the
 * VERDICT is wrong:
 *   LOCK       a peer merely holds the word.  The captured head may be perfectly
 *              live.  This is exposure, not error.
 *   PROXY      a commit is mid-flight on the holder: in doubt.
 *   TOMBSTONE  the holder is RETIRED.  This is one-way, so observing it is a
 *              FACT.  The captured chain is gone.  The unheld verdict would
 *              have named a dead head.  THIS is the defect population.
 */
static unsigned long ft_dbg_unique_af_lock, ft_dbg_unique_af_proxy,
		ft_dbg_unique_af_tomb;
/*
 * ★★ AND THE ONE COUNTER THAT NEEDS NO CONTROL AT ALL.  Everything above is an
 * upper bound on something:
 *   - a LOCK refusal is exposure, not error;
 *   - a TOMBSTONED HOLDER means that the capture's ROUTING is stale.  But a
 *     recompaction can re-home the chain with the SAME head still live.  Then
 *     "-EEXIST, here is the head" was arguably still TRUE;
 *   - and the COARSE arm cannot bound any of it.  ft->lock_fine is false
 *     there, so this code never runs.  ☠ Twice in one investigation the coarse
 *     arm was reached for as a control.  The second time it was not even
 *     executing the measured path.
 *
 * One question is decidable on its own: has the RETURNED HEAD left the trie?
 * A removal mark is ONE-WAY, so a marked head is marked forever.  A single
 * observation is then a fact about that verdict.  No peer schedule can make it
 * innocent, and no control is needed to interpret it.
 */
static unsigned long ft_dbg_unique_head_gone;
# define FT_DBG_UNIQUE(stale, is_unique)				\
	do {								\
		if (is_unique)						\
			uatomic_inc((stale) ? &ft_dbg_unique_stale :	\
				&ft_dbg_unique_ok);			\
	} while (0)
# define FT_DBG_UNIQUE_ACQFAIL(is_unique, meta, head)			\
	do {								\
		if (is_unique) {					\
			uintptr_t s__ = ft_meta_state_load((meta));	\
									\
			uatomic_inc(&ft_dbg_unique_acqfail);		\
			if (ft_node_is_removed(head))			\
				uatomic_inc(&ft_dbg_unique_head_gone);	\
			if (s__ & FT_STATE_TOMBSTONE)			\
				uatomic_inc(&ft_dbg_unique_af_tomb);	\
			else if (s__ & FT_STATE_PROXY)			\
				uatomic_inc(&ft_dbg_unique_af_proxy);	\
			else						\
				uatomic_inc(&ft_dbg_unique_af_lock);	\
		}							\
	} while (0)
static __attribute__((destructor))
void ft_dbg_unique_dump(void)
{
	unsigned long st = uatomic_read(&ft_dbg_unique_stale);
	unsigned long ok = uatomic_read(&ft_dbg_unique_ok);
	unsigned long af = uatomic_read(&ft_dbg_unique_acqfail);

	if (!(st | ok | af))
		return;
	fprintf(stderr, "FT UNIQUE VERDICT: capture_good=%lu capture_stale=%lu "
		"acquire_refused=%lu (tomb=%lu proxy=%lu lock=%lu)\n"
		"  ** HEAD_GONE=%lu ** = the captured head is MARKED REMOVED, so the "
		"unheld verdict named a node that HAS LEFT THE TRIE.  One-way mark: "
		"a fact per observation, no control needed.  tomb/proxy/lock above "
		"are upper bounds only (see the note at the counters).\n",
		ok, st, af, uatomic_read(&ft_dbg_unique_af_tomb),
		uatomic_read(&ft_dbg_unique_af_proxy),
		uatomic_read(&ft_dbg_unique_af_lock),
		uatomic_read(&ft_dbg_unique_head_gone));
}
#else
# define FT_DBG_UNIQUE(stale, is_unique)	do { } while (0)
# define FT_DBG_UNIQUE_ACQFAIL(is_unique, meta, head)	do { } while (0)
#endif

#ifdef FT_ENABLE_TRACING
#include <stdio.h>
#include <stdlib.h>
#include "cds_ft_tp.h"



/*
 * FAST STOP.  `lttng stop` is a round trip to the session daemon.
 * `lttng snapshot record` is a fork+exec.  Both take MILLISECONDS.  A workload
 * may emit millions of events per second.  A 64 KiB ring then wraps many
 * times over in that window.  So a violation site that calls them directly
 * dumps a snapshot without its own violation event: it was already evicted.
 * (Measured: 6 snapshots, 0 violation events in any of them.)
 *
 * So freeze emission in-process FIRST, with a single relaxed store.  Only
 * then pay for the slow calls.  Every emitter checks the flag.  So the ring
 * stops advancing within a store-buffer drain of the violation, instead of
 * within a process spawn.
 *
 * ORDER AT THE SITE: emit the violation, THEN freeze.  The violation must be
 * the last event IN the buffer, not the first one dropped by its own flag.
 *
 * SINGLE-PROCESS ONLY, and that is exactly the case here.  The flag is
 * process-local.  It silences this process's emitters and no others.  A
 * multi-process session still needs `lttng stop` to freeze its peers.  This
 * is why the slow path stays.
 *
 * RELAXED is deliberate.  This is not ordering data.  It is a "stop shouting"
 * hint, and its only requirement is to become visible fast.  On x86 a relaxed
 * store lands in tens of nanoseconds.  A straggler event or two from a thread
 * that has not yet observed it is harmless next to a full ring wrap.
 */
extern int ft_trace_frozen;

#define FT_TRACE_FREEZE()	uatomic_store(&ft_trace_frozen, 1, CMM_RELAXED)

/*
 * ONE SESSION PER PROCESS, over PER-UID buffers.
 *
 * The session daemon owns the per-UID rings, so they SURVIVE the traced
 * process.  Per-process rings do not.  A process that vanishes before the
 * snapshot is issued takes its trace with it.  Isolation comes instead from
 * giving each process its OWN session (`lttng track -u --pid`).  Then peers
 * cannot evict each other's window.  And a process-local freeze corresponds
 * exactly to one session's buffers.
 *
 * The session name arrives in FT_TRACE_SESSION from the launcher that created
 * and PID-tracked it.  The default "ftrd" keeps the single-process case
 * working unchanged.
 */
static inline
const char *ft_trace_session(void)
{
	static const char *name;

	if (caa_unlikely(!name)) {
		const char *e = getenv("FT_TRACE_SESSION");

		name = (e && *e) ? e : "ftrd";
	}
	return name;
}

/*
 * Freeze, then stop and dump THIS process's session.  The order matters.  The
 * caller must already have emitted its violation event, because the freeze
 * silences every emitter in this process.
 */
static inline
void ft_trace_capture(void)
{
	char cmd[256];

	FT_TRACE_FREEZE();
	snprintf(cmd, sizeof(cmd), "lttng stop %s 1>&2", ft_trace_session());
	(void) system(cmd);
	snprintf(cmd, sizeof(cmd), "lttng snapshot record -s %s 1>&2",
		ft_trace_session());
	(void) system(cmd);
}

#define FT_TP(name, ...)						\
	do {								\
		if (caa_likely(!uatomic_load(&ft_trace_frozen,		\
				CMM_RELAXED)))				\
			lttng_ust_tracepoint(cds_ft, name, ##__VA_ARGS__); \
	} while (0)
/*
 * Emit a tracepoint with a key byte-sequence payload (LTTng's
 * sequence_hex field).  When tracing is disabled, the arguments are
 * discarded by the FT_TP no-op expansion, so no code is emitted.
 *
 * FT_TP_KEY_RESOLVED: caller has already resolved keylen to a real
 *   byte count (e.g. from ft_key_len() or iter->key_len).
 *
 * FT_TP_KEY: user-facing sentinel values (CDS_FT_LEN_DEFAULT ==
 *   SIZE_MAX, etc.) must be resolved via ft_key_len(ft_, keylen)
 *   before the sequence field reads keylen bytes from keybuf, or
 *   the sequence reader would attempt to serialize SIZE_MAX bytes.
 *   On resolution failure (LEN_ERROR), the key sequence is empty.
 */
#define FT_TP_KEY_RESOLVED(name, ft_, keybuf, keylen)			\
	FT_TP(name, (const void *) (ft_), (keybuf), (keylen))
#define FT_TP_KEY(name, ft_, keybuf, keylen)				\
	do {								\
		size_t _tp_klen = ft_key_len((ft_), (keylen));		\
		FT_TP_KEY_RESOLVED(name, (ft_), (keybuf),		\
			_tp_klen == CDS_FT_LEN_ERROR ? 0 : _tp_klen);	\
	} while (0)
/*
 * FT_TP_ITER_KEY: emit an iter-keyed key event.  Use the resolved
 * iter->key_len.  ft is carried alongside iter for snapshot safety:
 * iter_create may have already scrolled out of a flight recorder.
 */
#define FT_TP_ITER_KEY(name, iter)					\
	FT_TP(name, (const void *) (iter)->ft, (const void *) (iter),	\
		iter_key(iter), FT_TP_ITER_KEY_LEN(iter))
/*
 * The length of @iter's key, for an event that carries the key.  An ordered
 * walk leaves the key unbuilt: key_len is then FT_ITER_KEY_LEN_LAZY (SIZE_MAX).
 * Give 0 instead.  A sequence of SIZE_MAX bytes corrupts the trace buffer.
 * Do not build the key here: an event must not change the iterator.
 */
#define FT_TP_ITER_KEY_LEN(iter)					\
	((iter)->key_len == FT_ITER_KEY_LEN_LAZY ? 0 : (iter)->key_len)
/*
 * enum ft_tp_node_kind (node-kind identifiers) lives in
 * fractal-trie-internal.h so the C side and the LTTng enum in
 * src/cds_ft_tp.h reference a single definition.  ft_tp_node_kind()
 * in fractal-trie.c maps a tagged cds_ft_inode_flag pointer to one of
 * those values.
 */
#else
#define FT_TP(name, ...)			do {} while (0)
#define FT_TRACE_FREEZE()			do {} while (0)
#define ft_trace_capture()			do {} while (0)
#define FT_TP_KEY_RESOLVED(name, ft_, keybuf, keylen)	do {} while (0)
#define FT_TP_KEY(name, ft_, keybuf, keylen)	do {} while (0)
#define FT_TP_ITER_KEY(name, iter)		do {} while (0)
#endif

/*
 * THE PER-OP RETRY CAP: one counter, every mutation entry, one loud failure.
 *
 * ☠ WHY A COUNTER IS A CORRECTNESS INSTRUMENT AND NOT A PERFORMANCE ONE.
 * -EAGAIN in this tree carries a PROMISE: "retrying can help, because a PEER is
 * responsible for this refusal".  So a retry count that runs away does not
 * mean that the machine is busy.  It means that the op is refusing ITSELF.  No
 * re-descent can change the condition.  Single-threaded, that reading is
 * certain.  With peers it is a hypothesis.  The event's @nr_writers lets the
 * reader judge it.
 *
 * The rekey writer already had a private version of this
 * (FT_DEBUG_REKEY_RETRY_CAP).  It found three self-refusals by hand.  This is
 * that detector made GENERIC: insert, remove, replace and rekey share it.  It
 * is also made TRACEABLE.  Instead of only printing, it does three things:
 * - it fires an LTTng violation event;
 * - it records the flight-recorder snapshot;
 * - it aborts.
 * So the window that led to the spin is on disk.  The spin itself does not
 * overwrite it.
 *
 * ☞ IT IS PER OP, NOT PER THREAD.  Reset it at every entry.  A thread that
 * does a million ordinary mutations must not accumulate them into a false
 * positive.  The question is always "did THIS call converge".
 *
 * The cap is deliberately far above real contention.  A saturated
 * multi-writer arm re-attempts in single digits.  So five figures cannot be a
 * peer.
 */
#ifdef FT_DEBUG_OP_RETRY_CAP
# include <stdio.h>
# include <stdlib.h>
# ifndef FT_OP_RETRY_CAP
#  define FT_OP_RETRY_CAP	50000
# endif

enum ft_op_kind {
	FT_OP_INSERT = 1,
	FT_OP_REMOVE,
	FT_OP_REPLACE,
	FT_OP_REKEY,
	FT_OP_REMOVE_ALL,
	FT_OP_INSERT_REPLACE,
};

/*
 * ★ WHICH RETRY SITE.  The cap says that an op spun.  It does not say WHERE.
 * An op with four distinct "publishes nothing, retry" exits has four
 * different diagnoses.  This mirrors ft_dbg_rm_site (ft-helpers.h).  Each
 * site stamps its own __LINE__.  The livelock report names:
 * - the line that spun;
 * - how many times it spun consecutively;
 * - the last OTHER line seen.
 * So a single spinning exit is distinguishable from a cycle between two.
 */
static __thread unsigned int ft_dbg_retry_line;
static __thread unsigned long ft_dbg_retry_line_nr;
static __thread unsigned int ft_dbg_retry_line_other;

static inline
void ft_dbg_retry_stamp(unsigned int line)
{
	if (line == ft_dbg_retry_line) {
		ft_dbg_retry_line_nr++;
		return;
	}
	if (ft_dbg_retry_line)
		ft_dbg_retry_line_other = ft_dbg_retry_line;
	ft_dbg_retry_line = line;
	ft_dbg_retry_line_nr = 1;
}
# define FT_DBG_RETRY_SITE()	ft_dbg_retry_stamp(__LINE__)

/*
 * ★ WHICH -EAGAIN.  The retry-exit stamp above names the EXIT.  On an op whose
 * body has forty-odd -EAGAIN producers, that is still a whole file.  This
 * stamps the PRODUCER.  It is an expression, so it is safe in a braceless
 * `if`.  It works in both `return -EAGAIN;` and `ret = -EAGAIN;` forms.
 *
 * NOT applied at rest.  Wrapping fifty-odd returns costs readability in a
 * shipping header.  That cost is not worth it once the ACQUIRE stamp below
 * exists.  And the two ends of the path usually pin the middle.  When they do
 * not, re-apply it mechanically:
 *   sed 's/return -EAGAIN;/return FT_DBG_EAGAIN(-EAGAIN);/' over the op's
 *   file, plus the same for `<var> = -EAGAIN;`.
 * This is how line 5622 of ft-remove.h (the holder acquire) was named.
 */
static __thread unsigned int ft_dbg_eagain_line;
static __thread unsigned long ft_dbg_eagain_line_nr;
static __thread unsigned int ft_dbg_eagain_line_other;

static inline
int ft_dbg_eagain_stamp(int v, unsigned int line)
{
	if (line == ft_dbg_eagain_line) {
		ft_dbg_eagain_line_nr++;
		return v;
	}
	if (ft_dbg_eagain_line)
		ft_dbg_eagain_line_other = ft_dbg_eagain_line;
	ft_dbg_eagain_line = line;
	ft_dbg_eagain_line_nr = 1;
	return v;
}
# define FT_DBG_EAGAIN(v)	ft_dbg_eagain_stamp((v), __LINE__)

/*
 * ★ AND WHICH REFUSAL INSIDE THE ACQUIRE.  One -EAGAIN producer in the caller
 * can be four in ft_dlm_acquire_set_at.  That function funnels every refusal
 * through one `eagain:` label, so the label erases the reason.  Stamp at the
 * GOTO.  Use a do/while, so that it stays one statement inside a braceless
 * `if`.
 */
static __thread unsigned int ft_dbg_acq_line;
static __thread unsigned long ft_dbg_acq_line_nr;
static __thread unsigned int ft_dbg_acq_line_other;

static inline
void ft_dbg_acq_stamp(unsigned int line)
{
	if (line == ft_dbg_acq_line) {
		ft_dbg_acq_line_nr++;
		return;
	}
	if (ft_dbg_acq_line)
		ft_dbg_acq_line_other = ft_dbg_acq_line;
	ft_dbg_acq_line = line;
	ft_dbg_acq_line_nr = 1;
}
# define FT_DBG_ACQ_SITE()	ft_dbg_acq_stamp(__LINE__)

/*
 * ★ AND WHICH BIT.  ft_dlm_lock refuses on PROXY | TOMBSTONE | LOCK as one
 * test.  The three have different cures:
 * - LOCK is contention.  It clears when the holder commits.
 * - PROXY is a parked flip that settles.
 * - TOMBSTONE is PERMANENT.  It is a retired word whose expected-old can never
 *   recur.  This is the immortal-old livelock, not contention.
 * Count them apart.  Keep the last refused word, so that the report can name
 * the state.
 */
static __thread unsigned long ft_dbg_lock_refuse_lock;
static __thread unsigned long ft_dbg_lock_refuse_proxy;
static __thread unsigned long ft_dbg_lock_refuse_tomb;
static __thread unsigned long ft_dbg_lock_refuse_state;
/*
 * ★ LEAKED OR CHURNING.  A LOCK refusal that is always on the SAME word is a
 * lock that nobody will release: a peer's bail path lost it.  A refusal that
 * moves is ordinary contention that the op is losing.  This is the same
 * cure-splitting question as the bit above.
 */
static __thread const void *ft_dbg_lock_refuse_meta;
static __thread unsigned long ft_dbg_lock_refuse_streak;
static __thread unsigned long ft_dbg_lock_refuse_switches;
/*
 * ★ AND FOR A PROXY, ASK THE DESCRIPTOR.  The split above does not worry
 * about a proxy refusal, because "PROXY settles".  But a settle is an ACT.
 * The parking commit's owner performs it.  A word that stays proxied across
 * thousands of refusals says that this act never happened.  Only the
 * descriptor can tell the two apart:
 *
 *   UNDECIDED, one descriptor, forever  the parker is stuck mid-commit (it is
 *                                       blocked on something this op holds, or
 *                                       on a word of its own).
 *   SUCCEEDED / FAILED, still parked     the DECIDE ran and the SETTLE did not
 *                                       -- an abandoned descriptor; the word is
 *                                       readable (resolve) but unlockable
 *                                       forever, which is the immortal-old
 *                                       livelock wearing a proxy.
 *   a CHURN of descriptors               ordinary contention this op is losing.
 *
 * Captured at the refusal, reported at the cap.  @poisoned is carried too.  It
 * is the MCAS engine's poison flag: a poisoned descriptor could never commit,
 * so it could never settle by committing.  The single-writer engine has no
 * such flag, and nothing sets @poisoned now.
 */
static __thread const void *ft_dbg_proxy_desc;
static __thread unsigned long ft_dbg_proxy_status;
static __thread unsigned long ft_dbg_proxy_streak;
static __thread unsigned long ft_dbg_proxy_switches;
static __thread unsigned long ft_dbg_proxy_nr;
static __thread unsigned long ft_dbg_proxy_nr_mw;
static __thread unsigned long ft_dbg_proxy_poisoned;
static __thread unsigned long ft_dbg_proxy_retry;
static __thread unsigned long ft_dbg_proxy_rec_old;
static __thread unsigned long ft_dbg_proxy_rec_new;

struct ft_op_retry {
	unsigned int attempts;
	unsigned int op;
	const uint8_t *key;
	size_t key_len;
};

/*
 * @nr_writers separates "certain" from "hypothesis" at the event.  It is
 * sampled rather than tracked: the trie knows how many writer scopes are
 * open.  Return zero when the build cannot answer.  Zero reads as "unknown",
 * never as "single".
 */
static inline
unsigned int ft_op_retry_nr_writers(const struct cds_ft *ft)
{
	(void) ft;
	return 0;
}

static inline
void ft_op_retry_init(struct ft_op_retry *r, unsigned int op,
		const uint8_t *key, size_t key_len)
{
	/*
	 * ☠ PER OP, NOT PER THREAD.  The refusal and parker counters are
	 * `static __thread`.  Nothing used to clear them.  So the cap's
	 * fingerprint summed every refusal the thread had EVER suffered.  A
	 * `streak=15878 switches=2` read as "one word refused 15878 times in
	 * this op".  In fact it was the thread's lifetime total.  The
	 * workload's eight keys all hung off ONE junction.  So the number said
	 * nothing.  The cap's whole question is "did THIS call converge".  So
	 * its evidence has to have THIS call's scope.
	 */
	ft_dbg_lock_refuse_lock = 0;
	ft_dbg_lock_refuse_proxy = 0;
	ft_dbg_lock_refuse_tomb = 0;
	ft_dbg_lock_refuse_state = 0;
	ft_dbg_lock_refuse_meta = NULL;
	ft_dbg_lock_refuse_streak = 0;
	ft_dbg_lock_refuse_switches = 0;
	ft_dbg_proxy_desc = NULL;
	ft_dbg_proxy_status = 0;
	ft_dbg_proxy_streak = 0;
	ft_dbg_proxy_switches = 0;
	ft_dbg_proxy_nr = 0;
	ft_dbg_proxy_nr_mw = 0;
	ft_dbg_proxy_poisoned = 0;
	ft_dbg_proxy_retry = 0;
	ft_dbg_proxy_rec_old = 0;
	ft_dbg_proxy_rec_new = 0;
	ft_dbg_retry_line = 0;
	ft_dbg_retry_line_nr = 0;
	ft_dbg_retry_line_other = 0;
	ft_dbg_eagain_line = 0;
	ft_dbg_eagain_line_nr = 0;
	ft_dbg_eagain_line_other = 0;
	ft_dbg_acq_line = 0;
	ft_dbg_acq_line_nr = 0;
	ft_dbg_acq_line_other = 0;
	r->attempts = 0;
	r->op = op;
	r->key = key;
	r->key_len = key_len;
}

/*
 * Call once per attempt.  Emit the per-retry step event.  That event has a
 * high rate, and low value until the violation has named a site: enable it by
 * name.  On the cap, fire the violation, record the snapshot and abort.
 *
 * ☠ TAKE THE SNAPSHOT BEFORE THE ABORT AND AFTER THE VIOLATION.  The spin
 * itself overwrites the ring.  So the event that explains the window has to
 * be IN the window it explains.
 */
static inline
void ft_op_retry_tick(const struct cds_ft *ft, struct ft_op_retry *r, int last_ret)
{
	FT_TP(op_retry_step, (const void *) ft, r->op, r->attempts, last_ret);
	if (caa_likely(++r->attempts <= FT_OP_RETRY_CAP))
		return;
	FT_TP(op_retry_violation, (const void *) ft, r->op, r->attempts,
		last_ret, ft_op_retry_nr_writers(ft),
		r->key, r->key ? r->key_len : 0);
	fprintf(stderr,
		"  acquire bailed at ft-mutation-helpers.h:%u (x%lu), previous site :%u\n",
		ft_dbg_acq_line, ft_dbg_acq_line_nr, ft_dbg_acq_line_other);
	fprintf(stderr,
		"FT OP RETRY LIVELOCK: op=%u attempts=%u last_ret=%d -- an "
		"-EAGAIN no re-descent can clear.  Single-threaded this is "
		"certain; under peers it is the leading hypothesis.\n",
		r->op, r->attempts, last_ret);
	fprintf(stderr,
		"FT OP RETRY SITE: line=%u consecutive=%lu other_line=%u\n",
		ft_dbg_retry_line, ft_dbg_retry_line_nr,
		ft_dbg_retry_line_other);
	fprintf(stderr,
		"FT OP EAGAIN SITE: line=%u consecutive=%lu other_line=%u\n",
		ft_dbg_eagain_line, ft_dbg_eagain_line_nr,
		ft_dbg_eagain_line_other);
	fprintf(stderr,
		"FT OP ACQUIRE SITE: line=%u consecutive=%lu other_line=%u\n",
		ft_dbg_acq_line, ft_dbg_acq_line_nr, ft_dbg_acq_line_other);
	fprintf(stderr,
		"FT OP LOCK REFUSE: lock=%lu proxy=%lu tombstone=%lu "
		"last_state=0x%lx\n",
		ft_dbg_lock_refuse_lock, ft_dbg_lock_refuse_proxy,
		ft_dbg_lock_refuse_tomb, ft_dbg_lock_refuse_state);
	fprintf(stderr,
		"FT OP LOCK REFUSE WORD: meta=%p streak=%lu switches=%lu\n",
		ft_dbg_lock_refuse_meta, ft_dbg_lock_refuse_streak,
		ft_dbg_lock_refuse_switches);
	fprintf(stderr,
		"FT OP PROXY PARKER: desc=%p status=%lu streak=%lu "
		"switches=%lu nr=%lu nr_mw=%lu poisoned=%lu retry=%lu "
		"rec_old=0x%lx rec_new=0x%lx\n",
		ft_dbg_proxy_desc, ft_dbg_proxy_status, ft_dbg_proxy_streak,
		ft_dbg_proxy_switches, ft_dbg_proxy_nr, ft_dbg_proxy_nr_mw,
		ft_dbg_proxy_poisoned, ft_dbg_proxy_retry,
		ft_dbg_proxy_rec_old, ft_dbg_proxy_rec_new);
#ifdef FT_DEBUG_RETRY_CAP_SHOW
	/*
	 * ★ THE SHAPE, not just the word.  A cap says that an op cannot
	 * converge.  The refusal stamps say WHICH word it keeps losing.
	 * Neither says what the trie LOOKS LIKE.  A single-threaded replay
	 * needs that.  Under COARSE this dump is safe by construction.  The
	 * spinning writer HOLDS the FT-wide mutex.  So every peer is parked
	 * in cds_fair_mutex_park, and the structure cannot move under the
	 * walk.  COARSE is exactly the arm a self-refusal lives on.
	 */
	fprintf(stderr, "FT OP RETRY SHAPE:\n");
	cds_ft_show(ft, stderr, CDS_FT_SHOW_PRETTY);
#endif
	if (system("lttng snapshot record 1>&2") == -1)
		fprintf(stderr, "FT OP RETRY: snapshot record failed\n");
	/*
	 * ☠ abort() RUNS NO DESTRUCTOR.  So it silently voids every exit-time
	 * dump in the tree.  ft_tk_dump_at_exit is among them: it holds the
	 * per-site txn census (-DFT_DEBUG_TXN_KIND).  That is a report you
	 * WANT at a retry cap.  The cap says that an op cannot converge.  The
	 * dump says what its txns recorded and returned.  Arming both and
	 * getting nothing reads as "the instrument found nothing".  That is the
	 * wrong zero
	 * [[feedback_an_instrument_can_be_armed_firing_and_blind]].
	 * -DFT_RETRY_CAP_EXIT trades the core for the destructors.
	 */
#ifdef FT_RETRY_CAP_EXIT
	exit(2);
#else
	abort();
#endif
}
#else
struct ft_op_retry { int unused; };
# define ft_op_retry_init(r, op, key, key_len)	do { (void) (r); } while (0)
# define ft_op_retry_tick(ft, r, last_ret)	do { (void) (r); } while (0)
# define FT_DBG_RETRY_SITE()			do { } while (0)
# define FT_DBG_EAGAIN(v)			(v)
# define FT_DBG_ACQ_SITE()			do { } while (0)
#endif	/* FT_DEBUG_OP_RETRY_CAP */

#endif /* _URCU_FT_TRACE_H */
