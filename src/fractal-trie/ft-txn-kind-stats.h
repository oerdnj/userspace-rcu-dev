// SPDX-FileCopyrightText: 2026 Mathieu Desnoyers <mathieu.desnoyers@efficios.com>
//
// SPDX-License-Identifier: LGPL-2.1-or-later

#ifndef _FT_TXN_KIND_STATS_H
#define _FT_TXN_KIND_STATS_H

/*
 * ft-txn-kind-stats: count the records each flip-txn plants, and what its
 * commit returns.  The counts are per TXN CREATION SITE.  Built only under
 * -DFT_DEBUG_TXN_KIND.  Otherwise it is byte-neutral: the struct fields, the
 * increments and this whole unit compile out.
 *
 * Every record is SW.  A record that the op neither owns nor excludes aborts
 * the process (ft_flip_txn_struct_mw_abort), so there is no MW class to count.
 * The table gives, per site:
 *   - the txns created, and those armed SW at creation;
 *   - the SW records;
 *   - the commit outcomes (enum ft_tk_end_class).
 *
 * The duplicate-chain stores record straight onto the engine handle
 * (ft-txn-hlist.h).  They hold no back-pointer to their flip-txn, so they are
 * counted GLOBALLY rather than per site.
 *
 * COUNTERS ARE PER THREAD, summed at the dump.  A shared counter on the hot
 * path would be a contended cacheline, and it would disturb the fairness this
 * unit measures.  Each thread's block is registered on a global list and is
 * never freed, so a thread that exits before the dump still reports.
 */

#ifndef FRACTAL_TRIE_IMPL
#error "ft-txn-kind-stats.h is an implementation unit; #include it from fractal-trie.c only"
#endif

#ifdef FT_DEBUG_TXN_KIND

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

enum ft_tk_rec_class {
	FT_TK_SW = 0,
	FT_TK_REC_NR,
};

/* See the door-1/3 exclusion probe at the SW park in __ft_flip_txn_record_tag_ctx. */
extern unsigned long ft_tk_d13_body, ft_tk_d13_excl, ft_tk_d13_coarse,
	ft_tk_d13_wlock, ft_tk_d13_none;
unsigned long ft_tk_d13_body, ft_tk_d13_excl, ft_tk_d13_coarse,
	ft_tk_d13_wlock, ft_tk_d13_none;
extern unsigned long ft_tk_d3_excl, ft_tk_d3_coarse, ft_tk_d3_wlock,
	ft_tk_d3_fine;
unsigned long ft_tk_d3_excl, ft_tk_d3_coarse, ft_tk_d3_wlock, ft_tk_d3_fine;
/* See the lock-held probe in ft_flip_txn_record_tombstone. */
extern unsigned long ft_tk_ret_held_us, ft_tk_ret_held_other,
	ft_tk_ret_unlocked_fine, ft_tk_ret_unlocked_coarse,
	ft_tk_ret_unlocked_excl, ft_tk_ret_unlocked_wlock;
unsigned long ft_tk_ret_held_us, ft_tk_ret_held_other,
	ft_tk_ret_unlocked_fine, ft_tk_ret_unlocked_coarse,
	ft_tk_ret_unlocked_excl, ft_tk_ret_unlocked_wlock;

enum ft_tk_end_class {
	FT_TK_OK = 0,		/* urcu_txn_commit_flavor -> OK */
	FT_TK_ABORT,		/* a peer won an MW record or a guard */
	FT_TK_MEMERR,		/* sticky ENOMEM: nothing was parked */
	FT_TK_MISS,		/* acquire_miss: discarded before the engine */
	FT_TK_BAILED,		/* ft_flip_txn_destroy: no commit attempted */
	FT_TK_END_NR,
};

/*
 * The last id is the OVERFLOW bucket.  A site that finds the table full still
 * counts, into a row named as such, instead of counting nowhere.
 */
#define FT_TK_MAX_SITES		256
#define FT_TK_OVERFLOW_ID	(FT_TK_MAX_SITES - 1)

/*
 * One txn creation site.  It lives as a function-static at the site itself
 * (FT_TK_SITE_HERE).  So its identity is the source line, and the site does
 * not have to be told anything.
 */
struct ft_tk_site {
	const char *file;
	int line;
	const char *what;	/* which constructor */
	int id;			/* -1 until registered */
};


struct ft_tk_tls {
	struct ft_tk_tls *next;
	unsigned long rec[FT_TK_MAX_SITES][FT_TK_REC_NR];
	unsigned long end[FT_TK_MAX_SITES][FT_TK_END_NR];
	unsigned long created[FT_TK_MAX_SITES];
	unsigned long armed[FT_TK_MAX_SITES];
	/* the duplicate-chain stores, not site-attributed: see the header */
	unsigned long chain_sw;
};

static pthread_mutex_t ft_tk_lock = PTHREAD_MUTEX_INITIALIZER;
static struct ft_tk_site *ft_tk_sites[FT_TK_MAX_SITES];
static int ft_tk_nr_sites;
static struct ft_tk_tls *ft_tk_threads;
static __thread struct ft_tk_tls *ft_tk_self;

static
struct ft_tk_site ft_tk_overflow_site = { "<sites overflowed>", 0, "-", FT_TK_OVERFLOW_ID };

/*
 * Assign @s a stable id.  Two threads reaching a cold site both take the lock;
 * the second finds the id already published and uses it.
 */
static
int ft_tk_site_register(struct ft_tk_site *s)
{
	int id;

	pthread_mutex_lock(&ft_tk_lock);
	if (s->id >= 0) {
		id = s->id;
		goto unlock;
	}
	if (ft_tk_nr_sites >= FT_TK_OVERFLOW_ID) {
		ft_tk_sites[FT_TK_OVERFLOW_ID] = &ft_tk_overflow_site;
		id = FT_TK_OVERFLOW_ID;
	} else {
		id = ft_tk_nr_sites++;
		ft_tk_sites[id] = s;
	}
	CMM_STORE_SHARED(s->id, id);
unlock:
	pthread_mutex_unlock(&ft_tk_lock);
	return id;
}

static inline
int ft_tk_site_id(struct ft_tk_site *s)
{
	int id = CMM_LOAD_SHARED(s->id);

	if (caa_likely(id >= 0))
		return id;
	return ft_tk_site_register(s);
}

static
struct ft_tk_tls *ft_tk_tls_create(void)
{
	struct ft_tk_tls *tls = (struct ft_tk_tls *) calloc(1, sizeof(*tls));

	if (!tls)
		abort();	/* a debug build that cannot count is a lie */
	pthread_mutex_lock(&ft_tk_lock);
	tls->next = ft_tk_threads;
	ft_tk_threads = tls;
	pthread_mutex_unlock(&ft_tk_lock);
	ft_tk_self = tls;
	return tls;
}

static inline
struct ft_tk_tls *ft_tk_tls_get(void)
{
	struct ft_tk_tls *tls = ft_tk_self;

	if (caa_likely(tls != NULL))
		return tls;
	return ft_tk_tls_create();
}

/*
 * @site NULL means the txn was built by a path that predates this instrument.
 * There is none today.  Count it in the overflow row rather than nowhere.
 */
static inline
void ft_tk_count_rec(struct ft_tk_site *site, enum ft_tk_rec_class c)
{
	/*
	 * ☞ A CLASS OUT OF RANGE IS NOT A COUNTING ERROR, IT IS A SPRAY.  It would
	 * index past this row, into whatever .bss follows it.  An uninitialised
	 * bool once did that here, and corrupted the neighbouring counters.  The
	 * bound costs a predicted compare in a debug build.  It turns that failure
	 * into an immediate, located abort.
	 */
	if (caa_unlikely((unsigned int) c >= (unsigned int) FT_TK_REC_NR)) {
		fprintf(stderr, "FT_TK BOUNDS: class=%d site=%s:%d ra=%p\n",
			(int) c, site ? site->file : "?",
			site ? site->line : -1,
			__builtin_return_address(0));
		abort();
	}
	ft_tk_tls_get()->rec[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID][c]++;
}

static inline
void ft_tk_count_end(struct ft_tk_site *site, enum ft_tk_end_class c)
{
	ft_tk_tls_get()->end[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID][c]++;
}

static inline
void ft_tk_count_created(struct ft_tk_site *site)
{
	ft_tk_tls_get()->created[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID]++;
}

static inline
void ft_tk_count_armed(struct ft_tk_site *site)
{
	ft_tk_tls_get()->armed[site ? ft_tk_site_id(site) : FT_TK_OVERFLOW_ID]++;
}

static inline
void ft_tk_count_chain_sw(void)
{
	ft_tk_tls_get()->chain_sw++;
}


/*
 * The site object is a function-static: one per expansion of the constructor
 * macro, which is one per source line that creates a txn.
 */
#define FT_TK_SITE_HERE(what_)						\
	__extension__ ({						\
		static struct ft_tk_site __ft_tk_site = {		\
			__FILE__, __LINE__, (what_), -1,		\
		};							\
		&__ft_tk_site;						\
	})

/*
 * THE SITE PARAMETER EXISTS ONLY IN THE INSTRUMENTED BUILD.  It is spelled
 * into the constructors' signatures rather than added unconditionally.  So a
 * build without the knob is not merely "cheap" but IDENTICAL.  An extra
 * always-NULL argument is free at runtime.  But it still moves the
 * compiler's inlining and layout decisions.  This instrument must not
 * perturb the code paths whose latency it is used to compare.
 */
#define FT_TK_SITE_PARAM	struct ft_tk_site *dbg_site,
#define FT_TK_SITE_FWD		dbg_site,

struct ft_tk_row {
	const struct ft_tk_site *site;
	unsigned long rec[FT_TK_REC_NR];
	unsigned long end[FT_TK_END_NR];
	unsigned long created;
	unsigned long armed;
};

static
int ft_tk_row_cmp(const void *a, const void *b)
{
	const struct ft_tk_row *ra = (const struct ft_tk_row *) a;
	const struct ft_tk_row *rb = (const struct ft_tk_row *) b;

	if (ra->end[FT_TK_ABORT] != rb->end[FT_TK_ABORT])
		return ra->end[FT_TK_ABORT] > rb->end[FT_TK_ABORT] ? -1 : 1;
	return strcmp(ra->site->file, rb->site->file);
}

static
void ft_tk_dump(void)
{
	struct ft_tk_row *rows;
	struct ft_tk_tls *tls;
	unsigned long chain_sw = 0;
	struct ft_tk_row tot;
	int nr, i, threads = 0;
	char name[96];

	pthread_mutex_lock(&ft_tk_lock);
	nr = ft_tk_nr_sites;
	if (ft_tk_sites[FT_TK_OVERFLOW_ID])
		nr = FT_TK_MAX_SITES;
	rows = (struct ft_tk_row *) calloc(nr ? nr : 1, sizeof(*rows));
	if (!rows) {
		pthread_mutex_unlock(&ft_tk_lock);
		return;
	}
	for (i = 0; i < nr; i++)
		rows[i].site = ft_tk_sites[i];
	for (tls = ft_tk_threads; tls; tls = tls->next) {
		int c;

		threads++;
		chain_sw += tls->chain_sw;
		for (i = 0; i < nr; i++) {
			if (!rows[i].site)
				continue;
			for (c = 0; c < FT_TK_REC_NR; c++)
				rows[i].rec[c] += tls->rec[i][c];
			for (c = 0; c < FT_TK_END_NR; c++)
				rows[i].end[c] += tls->end[i][c];
			rows[i].created += tls->created[i];
			rows[i].armed += tls->armed[i];
		}
	}
	pthread_mutex_unlock(&ft_tk_lock);

	/* Drop the rows that never registered (a full table has holes). */
	for (i = 0; i < nr; ) {
		if (rows[i].site) {
			i++;
			continue;
		}
		rows[i] = rows[--nr];
	}
	qsort(rows, nr, sizeof(*rows), ft_tk_row_cmp);

	memset(&tot, 0, sizeof(tot));
	fprintf(stderr,
"\n=== FT_DEBUG_TXN_KIND: record kind + commit outcome, per txn creation site ===\n"
"    threads=%d sites=%d\n"
"%-44s %9s %7s %10s %10s %9s %8s %7s %8s\n",
		threads, nr,
		"site", "created", "armSW", "SW",
		"OK", "ABORT", "MEMERR", "MISS", "BAILED");
	for (i = 0; i < nr; i++) {
		int c;

		if (rows[i].site->line)
			snprintf(name, sizeof(name), "%s:%d %s",
				rows[i].site->file, rows[i].site->line,
				rows[i].site->what);
		else
			snprintf(name, sizeof(name), "%s", rows[i].site->file);
		fprintf(stderr,
"%-44s %9lu %7lu %10lu %10lu %9lu %8lu %7lu %8lu\n",
			name, rows[i].created, rows[i].armed,
			rows[i].rec[FT_TK_SW],
			rows[i].end[FT_TK_OK], rows[i].end[FT_TK_ABORT],
			rows[i].end[FT_TK_MEMERR], rows[i].end[FT_TK_MISS],
			rows[i].end[FT_TK_BAILED]);
		for (c = 0; c < FT_TK_REC_NR; c++)
			tot.rec[c] += rows[i].rec[c];
		for (c = 0; c < FT_TK_END_NR; c++)
			tot.end[c] += rows[i].end[c];
		tot.created += rows[i].created;
		tot.armed += rows[i].armed;
	}
	fprintf(stderr,
"%-44s %9lu %7lu %10lu %10lu %9lu %8lu %7lu %8lu\n",
		"TOTAL", tot.created, tot.armed,
		tot.rec[FT_TK_SW],
		tot.end[FT_TK_OK], tot.end[FT_TK_ABORT],
		tot.end[FT_TK_MEMERR], tot.end[FT_TK_MISS],
		tot.end[FT_TK_BAILED]);
	fprintf(stderr,
"    DUPLICATE-CHAIN stores (cds_ft_node.next/.prev; straight on the engine handle, not site-attributed): SW=%lu\n",
		chain_sw);

	{
		fprintf(stderr,
"    DOOR 1/3 SW parks, is the trie-wide exclusion REALLY in force?\n"
"      sw_body(door 3)=%lu  exclusive=%lu  coarse=%lu  FT-wide-lock=%lu  ☠ NONE(must be 0)=%lu\n",
			uatomic_read(&ft_tk_d13_body),
			uatomic_read(&ft_tk_d13_excl),
			uatomic_read(&ft_tk_d13_coarse),
			uatomic_read(&ft_tk_d13_wlock),
			uatomic_read(&ft_tk_d13_none));
		fprintf(stderr,
"      ☞ door 1's zero above is TAUTOLOGICAL (its arm predicate IS !lock_fine||exclusive).\n"
"      DOOR 3 (@sw_body) by mode -- the only licence here that no predicate checks:\n"
"        exclusive=%lu  coarse=%lu  FT-wide-lock=%lu  ☠ FINE, self-fenced only=%lu\n",
			uatomic_read(&ft_tk_d3_excl),
			uatomic_read(&ft_tk_d3_coarse),
			uatomic_read(&ft_tk_d3_wlock),
			uatomic_read(&ft_tk_d3_fine));
		fprintf(stderr,
"    RETIRE, is the LOCK-BEARING word held at the tombstone?  held-by-US=%lu  held-by-OTHER=%lu\n"
"      unlocked: FINE(the only one owed an answer)=%lu  coarse=%lu  exclusive=%lu  FT-wide-lock=%lu\n",
			uatomic_read(&ft_tk_ret_held_us),
			uatomic_read(&ft_tk_ret_held_other),
			uatomic_read(&ft_tk_ret_unlocked_fine),
			uatomic_read(&ft_tk_ret_unlocked_coarse),
			uatomic_read(&ft_tk_ret_unlocked_excl),
			uatomic_read(&ft_tk_ret_unlocked_wlock));
	}
	free(rows);
}

static __attribute__((destructor))
void ft_tk_dump_at_exit(void)
{
	ft_tk_dump();
}

/*
 * What a struct ft_flip_txn carries for this instrument, and how the record
 * helpers reach it.  @dbg_lock_take is the one piece the dispatch cannot
 * infer.  The DLM lock take goes through the SAME structural record helper as
 * every content edge.  Only its caller knows it is the arbitration point.
 */
#define FT_TK_TXN_FIELDS						\
	struct ft_tk_site *dbg_site;					\
	bool dbg_lock_take;						\
	bool dbg_ended;

/*
 * ☠ FIRST, before ANY other counting call on @t.  Every other FT_TK_* macro
 * reads @t->dbg_site to find the row it credits.  A constructor's @t comes
 * from malloc.  So a count taken before this one dereferences uninitialised
 * memory as a struct ft_tk_site *.  The resulting fault does not look like an
 * instrumentation bug either.  It happens inside the trie's own hot path, on
 * a thread doing ordinary work.  FT_TK_COUNT_ARMED is the one that will find
 * it, because arming is decided inside the constructor itself.
 */
#define FT_TK_TXN_INIT(t, site)						\
	do {								\
		(t)->dbg_site = (site);					\
		(t)->dbg_lock_take = false;				\
		(t)->dbg_ended = false;					\
		ft_tk_count_created(site);				\
	} while (0)

#define FT_TK_TXN_SITE(t)		((t)->dbg_site)
#define FT_TK_TXN_IS_TAKE(t)		((t)->dbg_lock_take)
#define FT_TK_TXN_SET_TAKE(t, v)	do { (t)->dbg_lock_take = (v); } while (0)
#define FT_TK_COUNT_REC(t, c)		ft_tk_count_rec((t)->dbg_site, (c))
#define FT_TK_COUNT_ARMED(t)		ft_tk_count_armed((t)->dbg_site)
#define FT_TK_COUNT_CHAIN_SW()		ft_tk_count_chain_sw()
/*
 * An outcome is recorded ONCE per txn.  ft_flip_txn_commit's acquire-miss arm
 * reports MISS and then destroys the handle.  The destroy must not also
 * report BAILED for it.
 */
#define FT_TK_COUNT_END(t, c)						\
	do {								\
		if (!(t)->dbg_ended) {					\
			(t)->dbg_ended = true;				\
			ft_tk_count_end((t)->dbg_site, (c));		\
		}							\
	} while (0)

#else	/* !FT_DEBUG_TXN_KIND */

struct ft_tk_site;	/* incomplete: the NULL the constructors take */

#define FT_TK_SITE_PARAM
#define FT_TK_SITE_FWD
#define FT_TK_TXN_FIELDS
#define FT_TK_TXN_INIT(t, site)		do { } while (0)
#define FT_TK_TXN_IS_TAKE(t)		0
#define FT_TK_TXN_SET_TAKE(t, v)	do { } while (0)
#define FT_TK_COUNT_REC(t, c)		do { } while (0)
#define FT_TK_COUNT_END(t, c)		do { } while (0)
#define FT_TK_COUNT_ARMED(t)		do { } while (0)
#define FT_TK_COUNT_CHAIN_SW()		do { } while (0)

#endif	/* FT_DEBUG_TXN_KIND */

#endif /* _FT_TXN_KIND_STATS_H */
