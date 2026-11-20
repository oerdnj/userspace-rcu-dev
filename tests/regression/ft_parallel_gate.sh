#!/bin/bash
#
# ft_parallel_gate.sh -- build + run the fractal-trie TAP suites
# (test_urcu_ft_unit and test_urcu_ft_inv) across every feature-flag
# configuration CONCURRENTLY, as a pre-commit gate.
#
# Why a separate tree per config:
#   The feature flags (verify-at-mutation, skip-compress off, compress
#   off, fault-inject + tombstone-audit, in-place off) are compile
#   time -D defines.  A single in-tree build can only hold one set at a
#   time (and an out-of-tree build is refused while the source is
#   configured), so a sequential "clean; rebuild; test" per config
#   serializes the whole matrix behind the slowest leg (VAM).  Instead
#   we keep one CONFIGURED copy of the source tree per config under
#   $FT_GATE_DIR and build+test them all at once.
#
#   The flags are applied at CONFIGURE time (CPPFLAGS=... ./configure),
#   not via a make-time `make CPPFLAGS=` override: the override reaches
#   the library but NOT the test-side #ifdefs (e.g. VAM's skip of the
#   O(keys^2) test_compact_dense_full_node), which would silently build
#   the tests as default and mis-report coverage.
#
# Usage:
#   tests/regression/ft_parallel_gate.sh [config ...]
#     no args   -- run the full matrix
#     config... -- run only the named configs (default fault-audit vam
#                  noskip nocompress no-in-place)
#
# Env:
#   FT_GATE_DIR  per-config tree copies live here   (default:
#                $HOME/.cache/ft-parallel-gate -- OUTSIDE the repo so the
#                GB of copies never pollute git status)
#
#                ☠ AND NOT UNDER /tmp, WHICH IS A tmpfs ON THIS CLASS OF
#                BOX.  The matrix keeps one CONFIGURED SOURCE TREE PER
#                CONFIG -- fourteen of them, several GB in total -- so the
#                old ${TMPDIR:-/tmp} default spent that out of RAM, on top
#                of whatever the legs themselves were caged for.  Measured
#                2026-09-18: a single default-dir gate run filled a 378 GB
#                tmpfs that already held 138 GB, and every subsequent
#                command on the box failed ENOSPC -- including the ones
#                needed to clean it up.  A disk-backed default costs one
#                rsync's worth of I/O and cannot take the machine with it.
#                Override it to a tmpfs deliberately if you want the speed
#                and have measured the room.
#   FT_GATE_J    make -j per config                 (default: cores/12,
#                so nconfigs*J stays near the core count)
#   FT_GATE_EXTRA_CPPFLAGS  appended to every config's CPPFLAGS (default
#                none), e.g. -DDEBUG_RCU for a debug sweep of the whole
#                matrix.  Use a separate FT_GATE_DIR for it.
#   FT_GATE_SLICE  systemd user slice every leg's memory cage is created
#                under (default: none -- app.slice).  Give it a slice with a
#                MemoryMax to bound the SUM of the legs, which the per-leg
#                caps do not.
#   FT_GATE_REPEAT    run each leg N times (default 1).  For hunting an
#                INTERMITTENT: the two measured instances of the
#                raw-read-of-a-parked-slot class fired at ~10% and ~8%,
#                so one green leg is not evidence.  Multiplies the whole
#                matrix, hence opt-in.
#
#                ★ WHAT IT COSTS AND BUYS.  Measured 2026-08-11 on a
#                384-thread box, per-config trees already configured, with
#                the spacings running CONCURRENTLY (see run_one):
#                  * The full 14-config matrix is 383 s at N=1, and anchorval
#                    -- the only config that has ever reproduced that class --
#                    is 365 s of it, so the gate is still exactly as long as
#                    its longest swept config.
#                  * Repeats are SEQUENTIAL within a spacing while the three
#                    spacings are not, so each +1 adds one spacing's legs
#                    (~330 s), not three.  N=1 383 s, N=3 ~17 min,
#                    N=5 ~28 min.
#                  * The per-leg cost is flat across the swept axis (per-node
#                    328 s, exponential 325, root-only 328); the inv legs are
#                    83% of it (unit 55 s, ion 89, ioff 88, imw 95).
#                  * Detection per gate run: 3 inv invocations land on the
#                    ONE spacing that reproduces, so an 8%-per-run defect is
#                    seen 1-0.92^3 = 23% of the time at N=1 -- and 54% at
#                    N=3, 73% at N=5.
#                The standing default is 1: 23% per commit still surfaces a
#                NEWLY introduced defect within a few commits, and it now
#                keeps the CORE when it does (see run_one), which is what
#                makes a hit actionable rather than merely alarming.
#                ★ N=3 is now affordable in a way it was not before the
#                spacings were parallelised -- it costs ~17 min, which is
#                what N=1 cost when they ran one after another.  Raise it if
#                you would rather spend that; nothing else needs to change.
#                For a deliberate hunt prefer tests/regression/ft_corecatch.sh,
#                which repeats ONE suite with the evidence kept and no
#                build-matrix tax.
#
# Requires a bootstrapped source (./configure present -- run
# ./bootstrap first on a fresh clone).  Exit status is non-zero if any
# config fails to build or reports a failing/aborted test.
#
# SPDX-License-Identifier: LGPL-2.1-or-later

set -u

ROOT=$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)
# Disk-backed by default, NEVER $TMPDIR: see the FT_GATE_DIR note above.
GATE=${FT_GATE_DIR:-${HOME:-$(getent passwd "$(id -u)" | cut -d: -f6)}/.cache/ft-parallel-gate}
NCPU=$( (nproc 2>/dev/null || echo 8) )
J=${FT_GATE_J:-$(( NCPU/12 > 3 ? NCPU/12 : 4 ))}
mkdir -p "$GATE"

if [ ! -x "$ROOT/configure" ]; then
	echo "ft_parallel_gate: $ROOT/configure not found -- run ./bootstrap first." >&2
	exit 2
fi

# name | configure-time CPPFLAGS | tests
#   u=ft_unit  ion=ft_inv-on  ioff=ft_inv-off  imw=ft_inv-on + FT_INV_MW=1
#   imb=ft_inv built on urcu-memb (test_urcu_ft_inv_memb) + FT_INV_MW=1
ALL_CONFIGS=(
	# DLM (per-node lock-sets) is no longer a build flag: it is the ONLY
	# multi-writer implementation, selected at RUNTIME by ft->lock_fine
	# (the writer strategy).  The former "dlm" / "dlm-fault" configs folded
	# into "default" / "fault-audit" -- every config now exercises the one
	# MW implementation, so a failure no longer has to be attributed to a
	# build mode first.  "default" keeps imw because the concurrent-writer
	# and rekey oracles gate on FT_INV_MW at RUNTIME: without it they
	# compile in and then skip, which reads as coverage and is not.
	# imb runs ft_inv on urcu-memb.  Under QSBR a read-side section is a
	# no-op, so the other legs cannot see one held where it must not be:
	# around a bulk op, or across a nested bulk gate.  Both happened, and
	# both only showed on memb.
	"default||u ion ioff imw imwx imb"
	# Fault injection drives the acquire bail + re-descend paths: those
	# acquires never miss single-threaded, and the concurrent oracles merge
	# DISJOINT key sets, so injection is the only thing that reaches them.
	"fault-audit|-DFEATURE_FT_FAULT_INJECT -DFT_DEBUG_TOMBSTONE_AUDIT|u ioff"
	# FT_DEBUG_REKEY_RETRY_CAP rides here rather than buying a config of its
	# own: it costs one increment and one compare per rekey attempt, and this
	# config already runs the unit suite and both single-writer inv legs, which
	# is where every rekey shape the tests can reach gets driven.  A detector
	# compiled into no configuration is not coverage.
	"audit|-DFT_DEBUG_TOMBSTONE_AUDIT -DFT_DEBUG_REKEY_RETRY_CAP|u ion ioff"
	# ★ THE REMOVE RETRY CAP.  Its rekey twin rides on "audit" above; this one
	# could not, and the gap was VERIFIED: none of the 15 legs armed
	# -DFT_DEBUG_REMOVE_RETRY_CAP, so the gate had NO WAY TO TELL A REMOVE
	# LIVELOCK FROM CPU CONTENTION.  Both read as rc=124 (or rc=137 once the
	# spinning lane leaks into its cage), and "the box was busy" is the reading
	# a tired reviewer reaches for -- which is exactly the confound that cost a
	# session.  Armed, the same run aborts with "FT REMOVE LIVELOCK: %u attempts
	# on one remove" at 50,000 attempts, which is a CLAIM ABOUT THE TRIE and not
	# about the schedule.  A detector compiled into no configuration is not
	# coverage; a timeout it cannot be distinguished from is worse, because it
	# reads as a result.
	#
	# imw is the leg that pays for this one.  The livelock this cap exists to
	# catch is a CONCURRENT-writer phenomenon -- the shipping instance was
	# 2,000,000+ consecutive attempts re-deriving one dead holder with 11 of 12
	# writer threads parked behind the FIFO lane (ft-remove.h, the STALE
	# BACK-EDGE arm) -- and a single-writer leg cannot reach it at all.  u/ion/
	# ioff ride along because, measured, they cost nothing to add.
	#
	# -DFT_REMOVE_SLOW_NS=0 compiles out the per-op tail diagnostics and leaves
	# ONLY the abort.  -DFT_REMOVE_TAIL_QUIET alone does not do that: it keeps
	# the >1 ms "FT REMOVE SLOW" metric on purpose (it is the metric §D.3's A/B
	# was measured with), and on imw that metric printed 9,799 pairs and 1.9 MB
	# of stderr in 30 s -- into the very stream this harness greps for its
	# verdict.  The knob defaults to 1 ms, so every other user compiles what it
	# always compiled.
	#
	# COST, MEASURED (idle 384-core box, one leg per suite at a time, each in
	# its own memcg, default per-node spacing, armed vs the default config at
	# the same commit): unit 48 s vs 46, ion 88 vs 87, ioff 88 vs 87, imw 105 vs
	# 105; every leg green on both arms (331/331, 128/128 x3) and the captured
	# output byte-comparable.  The residual is one getrusage per remove.
	#
	# NOT swept across lock spacings, unlike txndbg/proxyassert: those sweep
	# because the class they detect HIDES IN ONE spacing.  This one detects
	# non-termination, which the coarse spacings already surface as their own
	# hangs; the sweep is a separate question from arming the detector at all.
	"rmcap|-DFT_DEBUG_REMOVE_RETRY_CAP -DFT_REMOVE_TAIL_QUIET -DFT_REMOVE_SLOW_NS=0|u ion ioff imw"
	"vam|-DFEATURE_FT_VERIFY_AT_MUTATION|u"
	# The TRANSACTION ENGINE's own debug features.  Every other config
	# compiles them out, so an engine-contract violation the FT commits is
	# invisible to the whole matrix:
	#   DEBUG_RCU                 -> urcu_assert_debug.  A record value that
	#                                is itself a proxy (the embedder read the
	#                                slot raw instead of through
	#                                urcu_txn_load), two records on one slot,
	#                                an MW record on the sw-only commit, a
	#                                same-slot kind conflict.
	#   URCU_TXN_DEBUG_READ_POLICY-> a slot that enters the read/write set
	#                                after an OPTIMISTIC load.
	# The two are complementary: the read-policy table only marks slots
	# loaded through the engine's API, so a RAW C read of a transacted word
	# leaves no mark and only the assert catches it.
	#
	# imw matters MOST here, for the same reason it does on "default": the
	# writers that do the heaviest transacted-slot work -- the concurrent-writer
	# and rekey oracles -- gate on FT_INV_MW at RUNTIME, so without it the one
	# config that carries the engine's assertions checks only the SINGLE-writer
	# subset.  A proxy can only be another writer's, and an expected-old only
	# arbitrates against a peer, so the raw-read class this config exists to
	# detect is the class the missing leg hid.  The leg costs 82 s, alongside
	# ioff's 76 s.
	"txndbg|-DDEBUG_RCU -DURCU_TXN_DEBUG_READ_POLICY -DURCU_TXN_DEBUG_SETTLE -DFEATURE_FT_ANCHOR_VALIDATE|u ion ioff imw sp imb"
	# The FT's own resolved-pointer assertion (ft_assert_resolved): a parked
	# flip proxy handed to an accessor that requires a resolved flag.  It is
	# the embedder-side counterpart to txndbg's engine-side DEBUG_RCU, and it
	# had NO config at all -- the flag appeared nowhere but its own definition,
	# so every one of the accessors carrying it was asserting into a build
	# nobody made.
	#
	# imw for the same reason txndbg needs it, only more so: a proxy is BY
	# CONSTRUCTION another writer's, so without the MW leg this config asserts
	# over a workload that cannot produce the thing it detects.
	#
	# noskip is in the CFLAGS deliberately.  The class this catches is the raw
	# child-slot read, and skip-compression collapses the chains those reads
	# walk -- the free-walk defect that motivated the config reproduced 10 of
	# 10 without skip-compression and never with it.
	"proxyassert|-DFT_DEBUG_PROXY_ASSERT -DNO_FEATURE_FT_SKIP_COMPRESSED -DFEATURE_FT_ANCHOR_VALIDATE|u ion ioff imw"
	# Phase E.3's certification config: the self-collision ledger
	# (FEATURE_FT_HOLD_TRACE).  A collision aborts (ft_hold_trace_refused), so
	# a red here is a leg abort, not a grep.  DEBUG_RCU rides along so the
	# engine's own asserts stay armed.
	"holdtrace|-DDEBUG_RCU -DFEATURE_FT_HOLD_TRACE -DFEATURE_FT_ANCHOR_VALIDATE|u ion ioff imw"
	"noskip|-DNO_FEATURE_FT_SKIP_COMPRESSED|u ioff"
	# ★ THE DUPLICATE CHAIN'"'"'S MW ABLATION.  cds_ft_node.next/.prev park SW by
	# default; this builds the MW arm back.  It is here so the arm cannot ROT:
	# -DNO_FEATURE_FT_MERGE went unbuildable for fourteen commits precisely
	# because no config compiled it, and an ablation nobody builds is worthless
	# exactly when a bisect needs it.
	#
	# ☠ It needs imw.  The property the flip changes is what happens when a
	# commit ABORTS -- an SW record parks and its abort writes old_ptr back
	# blind, where a failed MW CAS writes nothing -- and aborts are a
	# multi-writer phenomenon.  A leg without concurrent writers cannot tell
	# the two arms apart at all.
	"chainmw|-DNO_FEATURE_FT_CHAIN_SW|u ion ioff imw"
	#
	# THE ABLATION FOR THE BULK-GATE FENCE (2026-09-20).  The shipping build
	# fences @bulk_state only where something LOADS it -- a FINE trie, at any
	# spacing.  This leg restores the unconditional fence, so "coarse and
	# external-sync do not need it" stays a claim the matrix can refute
	# rather than a remembered measurement.
	"bulkfence|-DFT_RED_BULK_FENCE_ALWAYS|u ion ioff"
	#
	# THE REKEY OPT-IN's POSITIVE CONTROL (2026-09-20).  rekey is opt-in per
	# group and OFF by default, so a test whose trie creator forgets
	# cds_ft_group_attr_set_rekey keeps calling rekey, collects
	# NOT_SUPPORTED every time, and reports GREEN with zero rekey coverage.
	# This leg turns that refusal into an abort, which is the only form of
	# the check a green run cannot absorb.
	"rekeyoptin|-DFT_DEBUG_REKEY_OPTIN_STRICT|u ion ioff imw"
	"nocompress|-DNO_FEATURE_FT_COMPRESS|u ioff"
	# In-place point insert AND delete are the DEFAULT since 2026-09-26, so
	# every other config runs both tiers.  These three keep the recompact
	# paths covered -- the pre-flip default, and still what the bulk ops fall
	# back to on a shared trie -- in every mode the old default ran, MW
	# included.  The two tiers are independent switches, so each mix a build
	# can select has its own config: delete-on / insert-off is supported by
	# design (a hole refill is an insert-tier store; with that tier off the
	# refill recompacts and drops the hole) and is otherwise never run.
	"no-in-place|-DNO_FEATURE_FT_INSERT_IN_PLACE -DNO_FEATURE_FT_DELETE_IN_PLACE|u ion ioff imw"
	"no-in-place-insert|-DNO_FEATURE_FT_INSERT_IN_PLACE|u ion ioff imw"
	"no-in-place-delete|-DNO_FEATURE_FT_DELETE_IN_PLACE|u ion ioff imw"
	# The byte-key-only build (~25 KiB less .text): compiles out the
	# non-identity key-map lookup specializations, after which
	# cds_ft_group_attr_set_key_map returns NOT_SUPPORTED.  It had no gate
	# config, so that state was untested by construction.
	"nokeymap|-DNO_FEATURE_FT_KEY_MAP|u ion ioff"
	# The access-discipline validator.  Its writer/writer check is now
	# MODE-AWARE, so it runs against the concurrent oracles too: a FINE trie
	# counts writers instead of claiming a single owner, because disjoint
	# writers running in parallel is the design there, not a violation.
	"excl|-DFEATURE_FT_EXCL_VALIDATE|u ion ioff imw"
	# ★ THE ORDERED LOCK TAKE.  Lock words are taken by a plain CAS in
	# ascending ANCHOR order instead of being recorded into a txn and
	# installed by its commit -- exclusion needs ORDERING, not atomicity, and
	# a failed CAS writes nothing a peer can reference, so a losing acquire
	# no longer owes a grace period for its descriptor (165,006 -> 18,334
	# grace-period-owing aborts on one ft_inv MW leg).
	#
	# ☠ IT NEEDS imw.  The property at risk is EXCLUSION under concurrent
	# writers, and the two bugs this change already produced were both
	# invisible single-threaded: an in-place sort of the caller's set (SEGV,
	# caught by ft_unit) and a leaked lock on the commit-failure exit (a HANG
	# at ft_inv test 40 with ZERO test failures, which only the MW oracle
	# ★ "lockordered" IS GONE, and its absence is the point.  The ordered take
	# is no longer a configuration: FEATURE_FT_LOCK_TAKE_ORDERED was removed
	# and ft_dlm_acquire_set_at now sorts every lock set by anchor address
	# unconditionally, with no unordered fallback to select between.  A config
	# defining a macro nothing reads would run the DEFAULT build under a name
	# that claims otherwise -- coverage theatre, and worse than no config.
	# -DNO_FEATURE_FT_MERGE compiles out the merge subsystem (~20 KiB .text;
	# cds_ft_merge then returns NOT_SUPPORTED).  It had no gate config and had
	# ROTTED: the rekey fold's occupied-dst arm -- which IS a merge -- was not
	# guarded, so the library did not compile at all (9 errors).
	#
	# It was BUILD-ONLY until the 48 ft_unit tests that exercise merge
	# directly could tell "compiled out" from "broken": they now ask
	# cds_ft_merge_enabled() and skip, so the config runs the suite (295,
	# 48 of them skips) instead of only proving it links.  Compiling is a
	# weak claim about a configuration -- the rot above got in while this
	# config compiled fine.
	#
	# ft_inv joins it now that both its ordered legs are 120/120 here (the
	# last holdout, inv_ordered_bulk_consistency, was stranding its donor
	# trie's keys on the refused merge).  Running ft_unit alone is what let
	# that leak sit unseen: the unit suite never builds the concurrent
	# ordered view the leak showed up in.
	#
	# imw is armed too, and it is the leg that pays for itself here: it was
	# 112/120 when first run, with SEVEN reds -- the three
	# inv_rekey_public_atomic_no_gap* and four inv_rekey_*_writers -- all
	# reporting "no moves (livelock?)" because cds_ft_rekey_graft answered
	# NOT_SUPPORTED on this arm.  It should not have: an empty destination is
	# a GRAFT, not a union, and none of the merge subsystem is on its path.
	# The entry point was gated anyway, contradicting its own public
	# contract.  Un-gated; these seven are the only thing that checks it
	# stays that way.
	"nomerge|-DNO_FEATURE_FT_MERGE|u ion ioff imw"
	# ★ THE TRACING BUILD.  -DFT_ENABLE_TRACING was compiled by NO CONFIGURATION
	# AT ALL, and the cost of that is measured: the flight-recorder mis-wire
	# detector -- code that exists only in this build -- carried a defect for two
	# months (it aborted on its own torn read, @9d50ce91) because nothing ever
	# built it, let alone ran it.  A detector no configuration compiles is one
	# nobody runs.
	#
	# It RUNS the suites rather than merely building.  Compiling is the weak
	# claim -- the same lesson nomerge above records -- and it is the weaker one
	# here specifically: the detector's defect was an abort at RUNTIME on a shape
	# a green build says nothing about.  imw is in the list for the usual reason
	# (the torn read it aborted on is BY CONSTRUCTION a peer writer's), and it is
	# affordable: measured 2026-09-02, u 45 s, ion 80, ioff 79, imw 98 -- 302 s
	# sequential, inside the 365 s the swept anchorval config already costs, so
	# the gate's wall clock does not move.
	#
	# @TREE@ expands to this config's own tree (setup_tree), and it is MANDATORY,
	# not decoration: cds_ft_tp.h sets LTTNG_UST_TRACEPOINT_INCLUDE to
	# "./cds_ft_tp.h", which lttng re-includes from its own header directory, so
	# "./" needs an explicit -I pointing at the directory that actually holds it.
	# It must be THIS tree's copy -- the main tree's would compile the wrong
	# headers against this config's sources.
	# @LTTNG_CFLAGS@ expands to the probed lttng-ust include flags (see the
	# prerequisite probe below); the config is DROPPED, loudly, on a box without
	# lttng-ust rather than reported as a spurious CONFIG ERROR.
	"tracing|-DFT_ENABLE_TRACING -I@TREE@/src/fractal-trie @LTTNG_CFLAGS@|u ion ioff imw"
)

# Optional positional filter: run only the named configs.
CONFIGS=()
if [ "$#" -gt 0 ]; then
	for want in "$@"; do
		for c in "${ALL_CONFIGS[@]}"; do
			[ "${c%%|*}" = "$want" ] && CONFIGS+=("$c")
		done
	done
	[ "${#CONFIGS[@]}" -eq 0 ] && { echo "no matching configs in: $*" >&2; exit 2; }
else
	CONFIGS=("${ALL_CONFIGS[@]}")
fi

# ---------------------------------------------------------------------------
# THE TRACING CONFIG'S PREREQUISITE PROBE.
#
# -DFT_ENABLE_TRACING needs LTTng-UST headers, and lttng-ust is NOT a build
# dependency of liburcu -- so on a box without it the tracing config would fail
# at BUILD time and read exactly like a real regression.  Probe instead, and
# DROP the config with a loud line: an absent toolchain must not be reported as
# a red gate, and a red gate must not be explained away as an absent toolchain.
#
# ☠ Dropping it must NOT be able to empty the matrix.  `ft_parallel_gate.sh
# tracing` on a box with no lttng-ust would otherwise run ZERO configs and the
# results loop -- which iterates over CONFIGS -- would print nothing and exit 0.
# A GATE PASS over no configs is the worst false green there is, so that case
# exits 2 instead.
FT_GATE_LTTNG_CFLAGS=${FT_GATE_LTTNG_CFLAGS:-$(
	PKG_CONFIG_PATH="${PKG_CONFIG_PATH:-}:/usr/local/lib/pkgconfig" \
		pkg-config --cflags lttng-ust 2>/dev/null)}
: "${FT_GATE_LTTNG_CFLAGS:=-I/usr/local/include}"
lttng_ust_available() {
	# Compile-probe, not a file test: the header that must resolve is the one
	# the tracepoint provider includes, through whatever -I the box needs.
	printf '#include <lttng/tracepoint.h>\n' | \
		${CC:-cc} $FT_GATE_LTTNG_CFLAGS -E -x c - >/dev/null 2>&1
}
if ! lttng_ust_available; then
	_kept=()
	for c in "${CONFIGS[@]}"; do
		if [ "${c%%|*}" = tracing ]; then
			echo "WARNING: config 'tracing' DROPPED -- <lttng/tracepoint.h> does not" >&2
			echo "WARNING: resolve with CFLAGS [$FT_GATE_LTTNG_CFLAGS].  Install lttng-ust" >&2
			echo "WARNING: or set FT_GATE_LTTNG_CFLAGS.  -DFT_ENABLE_TRACING is NOT covered." >&2
		else
			_kept+=("$c")
		fi
	done
	CONFIGS=("${_kept[@]}")
	if [ "${#CONFIGS[@]}" -eq 0 ]; then
		echo "ft_parallel_gate: every requested config was dropped -- nothing to run." >&2
		exit 2
	fi
fi

setup_tree() {	# $1=name $2=cppflags -- one-time: copy source + configure WITH flags
	local dir=$GATE/$1 flags=$2
	# Per-tree tokens.  @TREE@ cannot be written literally in ALL_CONFIGS
	# because the path depends on $GATE, and @LTTNG_CFLAGS@ because it depends
	# on the box.  Substituted HERE, before the cache check, so .gate_flags
	# records the flags that were actually passed to configure -- comparing the
	# unsubstituted form would make every tree look correctly configured.
	flags=${flags//@TREE@/$dir}
	flags=${flags//@LTTNG_CFLAGS@/${FT_GATE_LTTNG_CFLAGS:-}}
	# FT_GATE_EXTRA_CPPFLAGS rides on EVERY config (e.g. -DDEBUG_RCU to run
	# the whole matrix with the engine's and FT's debug checks armed).
	# Appended before the cache check, so a changed set reconfigures.
	flags="$flags${FT_GATE_EXTRA_CPPFLAGS:+ $FT_GATE_EXTRA_CPPFLAGS}"
	if [ -f "$dir/config.status" ] && \
	   grep -qxF "CPPFLAGS=$flags" "$dir/.gate_flags" 2>/dev/null; then
		return 0	# already configured with these exact flags
	fi
	rm -rf "$dir"; mkdir -p "$dir"
	# ☠ A FAILED SETUP MUST NOT REPORT OK, AND MUST NOT BE CACHED.
	#
	# Both halves were wrong and they compounded.  rsync's status was
	# discarded, so a copy that died part way (a full disk: see the excludes
	# above) left a tree missing arbitrary files; success was then inferred
	# from config.status EXISTING, which a configure that failed on a missing
	# Makefile.in still leaves behind; and .gate_flags was written regardless,
	# so the NEXT run took the early-return above and reused the broken tree.
	# The visible result was three consecutive runs reporting "setup ok"
	# followed by an autotools error from a disk-space failure.
	#
	# So: check both commands, keep their output for the report, prove the
	# configure by the file the build actually needs (src/Makefile -- not
	# config.status), and stamp .gate_flags ONLY on success so a failure
	# retries from scratch instead of sticking.
	#
	# ☠ AND EXCLUDE .claude/, WHICH IS NOT SOURCE.  Agent worktrees live in
	# $ROOT/.claude/worktrees/ -- each a FULL checkout with a built src/.libs
	# -- and they accumulate.  Measured 2026-09-20: 16 of them, 4.3 GB, which
	# this copy multiplied by 21 configs.  It did not merely cost I/O: rsync
	# hit "symlink ... failed: File exists (17)" on their libtool .so links,
	# exited 23, and EVERY config then reported BUILD FAIL with "no makefile
	# found" -- because configure never ran.  A gate that fails 21/21 on a
	# directory that is not part of the build reads as a source regression.
	if ! rsync -aS --exclude='.git' --exclude='.claude' \
			--exclude='build-*' --exclude='ft-parallel-gate-*' \
			--exclude='ft-hunt-*' --exclude='ft-segv-*' \
			--exclude='core' --exclude='core.*' --exclude='vgcore.*' --exclude='*.core' \
			"$ROOT/"  "$dir/" >"$GATE/$1.setup" 2>&1; then
		echo "  setup $1 FAILED (copy -- see $GATE/$1.setup)"
		echo "$1: CONFIG ERROR (source copy failed; see $GATE/$1.setup)" >> "$GATE/$1.result"
		return
	fi
	if ! ( cd "$dir" && make distclean >/dev/null 2>&1
	       CPPFLAGS="$flags" ./configure --quiet >>"$GATE/$1.setup" 2>&1 ); then
		echo "  setup $1 FAILED (configure -- see $GATE/$1.setup)"
		echo "$1: CONFIG ERROR (configure failed; see $GATE/$1.setup)" >> "$GATE/$1.result"
		return
	fi
	if [ ! -f "$dir/src/Makefile" ]; then
		echo "  setup $1 FAILED (no src/Makefile -- see $GATE/$1.setup)"
		echo "$1: CONFIG ERROR (configure left no src/Makefile; see $GATE/$1.setup)" >> "$GATE/$1.result"
		return
	fi
	printf 'CPPFLAGS=%s\n' "$flags" > "$dir/.gate_flags"
	echo "  setup $1 ok"
}

sync_src() {	# $1=name -- refresh live sources into the (already-configured) tree
	local dir=$GATE/$1
	rsync -a --delete "$ROOT/src/fractal-trie/" "$dir/src/fractal-trie/" 2>/dev/null
	rsync -a "$ROOT/src/"     "$dir/src/"     --exclude='.libs' --exclude='*.o' --exclude='*.lo' 2>/dev/null
	rsync -aS "$ROOT/tests/"  "$dir/tests/"   --exclude='.libs' --exclude='*.o' --exclude='*.lo' \
		--exclude='core' --exclude='core.*' --exclude='vgcore.*' --exclude='*.core' 2>/dev/null
	rsync -a "$ROOT/include/" "$dir/include/" 2>/dev/null
}

# ---------------------------------------------------------------------------
# THE MEMORY CAGE.  Mathieu's standing order is that every test and repro runs
# in its own memcg; the gate was the one harness that did not, and it is the one
# that runs 25 legs at once.
#
# Defaults are ~3x the MEASURED peak RSS of a HEALTHY leg on the default config
# (VmHWM, per-node): ft_unit 5648 MiB, ft_inv on 4551, ft_inv off 5134,
# ft_inv mw 7948, txn settle ~0.  ☞ The MW leg peaks just OVER 8 GiB, which is
# why an 8G cage SIGKILLs it at ~test 105 with no `not ok` -- a truncation that
# reads as green.  Do not lower these without re-measuring; a cap that is too
# small does not corrupt anything, but it reports as INCOMPLETE (signal 9) and
# costs a bisect.
#
# WORST-CASE CONCURRENCY, since these multiply: legs are SEQUENTIAL within a
# spacing and the spacings and configs are not, so the bound is one leg per
# (config, spacing) pair -- 25 today -- of which at most 18 can be on imw.
# 18*24G + 7*16G = 544 GiB, which leaves headroom on this 755 GiB box for the
# 15 concurrent builds and the page cache.  Re-do that sum before raising a cap.
FT_GATE_MEM_U=${FT_GATE_MEM_U:-16G}
FT_GATE_MEM_INV=${FT_GATE_MEM_INV:-16G}
FT_GATE_MEM_IMW=${FT_GATE_MEM_IMW:-24G}
FT_GATE_MEM_SP=${FT_GATE_MEM_SP:-4G}
# ☠ A PER-LEG CAP BOUNDS NO SUM, and wrapping the gate in an outer scope does
# not bound it either: each leg's `systemd-run --scope` lands in the DEFAULT
# slice (app.slice), not under the scope it was started from.  FT_GATE_SLICE
# names a slice (e.g. ftgate.slice, with its own MemoryMax) that EVERY leg's
# cage is created under, so the whole matrix shares one total cap.
FT_GATE_SLICE=${FT_GATE_SLICE:-}

# Probed, never assumed: a gate that silently ran uncaged would be exactly the
# configuration that took the machine down, so say so loudly instead.
if systemd-run --user --scope -q -p MemoryMax=64M -p MemorySwapMax=0 \
		-- true >/dev/null 2>&1; then
	CAGE=1
else
	CAGE=0
	echo "WARNING: systemd-run --user --scope unavailable -- legs run UNCAGED." >&2
	echo "WARNING: a LEAKING livelock can then take the whole machine down." >&2
fi

run_leg() {	# $1=cwd $2=timeout-secs $3=MemoryMax ; $4.. = the command
	local d=$1 tmo=$2 mem=$3; shift 3
	( cd "$d" || exit 99
	  # A crash is an EXPECTED outcome for this harness, so let it dump...
	  ulimit -c unlimited 2>/dev/null
	  # ...and drop bash's own "Aborted (core dumped)" job line, which would
	  # otherwise land on the gate's stderr once per red leg and read like
	  # the harness itself failing.  The command's own stderr is folded into
	  # stdout below and is not affected by this.
	  exec 2>/dev/null
	  # ★ A TIMEOUT IS NOT A MEMORY BOUND.  Some FT livelocks spin with a FLAT
	  # RSS and some LEAK while they spin, and the gate cannot tell which it
	  # is holding: measured at ~46 MB/s for one of them, which over the
	  # 900 s ft_unit timeout is ~42 GB -- times the ten coarse-spacing legs
	  # that run concurrently.  That took a 755 GB machine down (and with it
	  # the whole gate run, which reports NOTHING).  Cage every leg so a
	  # leaking one dies alone as signal 9, which the INCOMPLETE check below
	  # already reports legibly, instead of taking its 24 siblings with it.
	  if [ "$CAGE" = 1 ]; then
		timeout "$tmo" systemd-run --user --scope -q \
			${FT_GATE_SLICE:+--slice="$FT_GATE_SLICE"} \
			-p MemoryMax="$mem" -p MemorySwapMax=0 -- "$@" 2>&1
	  else
		timeout "$tmo" "$@" 2>&1
	  fi )
}

run_leg_multi() {	# $1=name $2=cdir $3=spacing $4=bin $5=lib $6=outfile
	# ★ THE MULTI-PROCESS ARM.  Every other leg runs ONE process, so the gate
	# is structurally blind to the load-sensitive class: test_urcu_ft_inv is
	# clean alone and has been recorded failing (`not ok inv_remove_cross_view`,
	# aborts around the graft_swap solo family) only with several copies in
	# flight.  A patch that "passes the gate" has therefore not been tested
	# under load at all.
	#
	# Copies are SEPARATE PROCESSES on purpose, not more threads inside one:
	# what the class needs is independent allocators, independent call_rcu
	# worker sets and independent RCU grace-period domains competing for the
	# same cores -- none of which raising a thread count inside one process
	# reproduces.
	#
	# Reported per COPY, never merged: concatenating N TAP streams multiplies
	# the ok-count and breaks the plan check that every other leg relies on to
	# catch a hang, and an aggregate "notok=3" cannot say whether one copy
	# failed three times or three copies failed once -- which is exactly the
	# distinction this leg exists to make.
	local name=$1 cdir=$2 sp=$3 bin=$4 lib=$5 out=$6
	local n=${FT_GATE_COPIES:-4}
	local i o rc ok notok plan ran clean=0 red=0 c
	local -a pids=() dirs=()
	for i in $(seq 1 "$n"); do
		mkdir -p "$cdir/copy-$i"
		dirs+=("$cdir/copy-$i")
		( run_leg "$cdir/copy-$i" 1800 "$FT_GATE_MEM_IMW" \
			env LD_LIBRARY_PATH="$lib" \
			FT_INV_MW=1 "$bin" \
			> "$cdir/copy-$i.out" 2>&1; echo $? > "$cdir/copy-$i.rc" ) &
		pids+=($!)
	done
	wait "${pids[@]}" 2>/dev/null
	for i in $(seq 1 "$n"); do
		o=$(cat "$cdir/copy-$i.out" 2>/dev/null)
		rc=$(cat "$cdir/copy-$i.rc" 2>/dev/null); rc=${rc:-99}
		ok=$(printf '%s' "$o" | grep -c '^ok ')
		notok=$(printf '%s' "$o" | grep -c '^not ok ')
		plan=$(printf '%s' "$o" | sed -n 's/^1\.\.\([0-9]\{1,\}\)$/\1/p' | head -1)
		ran=$((ok + notok))
		# Same completeness checks as the single-process leg, and for the
		# same reason: a HUNG copy emits no `not ok` and would otherwise
		# score green on a partial run.
		if [ "$notok" -gt 0 ] || [ "$rc" -ne 0 ] || [ -z "$plan" ] \
				|| [ "$ran" -ne "$plan" ]; then
			red=$((red + 1))
			printf '%s' "$o" | grep '^not ok ' \
				| sed "s/^/      [$name] ft_inv mwx $sp copy$i /" >> "$out"
			if [ "$rc" -eq 124 ]; then
				echo "$name: ft_inv mwx $sp copy$i INCOMPLETE (TIMEOUT/hang after $ran tests)" >> "$out"
			elif [ "$rc" -gt 128 ]; then
				echo "$name: ft_inv mwx $sp copy$i INCOMPLETE (killed by signal $((rc - 128)) after $ran tests)" >> "$out"
			elif [ -z "$plan" ]; then
				echo "$name: ft_inv mwx $sp copy$i NO TAP PLAN (completeness unverifiable)" >> "$out"
			elif [ "$ran" -ne "$plan" ]; then
				echo "$name: ft_inv mwx $sp copy$i INCOMPLETE (ran $ran of $plan planned)" >> "$out"
			elif [ "$rc" -ne 0 ]; then
				echo "$name: ft_inv mwx $sp copy$i NONZERO EXIT ($rc) with a complete run" >> "$out"
			fi
			c=$(ls "$cdir/copy-$i"/core* 2>/dev/null | head -1)
			[ -n "$c" ] && echo "$name: ft_inv mwx $sp copy$i CORE kept -- LD_LIBRARY_PATH=$lib gdb $bin $c" >> "$out"
		else
			clean=$((clean + 1))
			rm -rf "$cdir/copy-$i" "$cdir/copy-$i.out" "$cdir/copy-$i.rc"
		fi
	done
	printf '  [%-11s] ft_inv mwx  %s copies=%s clean=%s red=%s\n' \
		"$name" "$sp" "$n" "$clean" "$red" >> "$out"
	[ "$red" -eq 0 ] && rm -rf "$cdir"
	return 0
}

run_one() {	# $1=name $2=tests $3=spacings $4=cppflags -- build lib+tests, run TAP
	local name=$1 tests=$2 spacings=${3:-} flags=${4:-}
	# Per-node is the only lock spacing (exponential and root-only were
	# removed), so every config runs one pass, labelled per-node.
	spacings=per-node
	local dir=$GATE/$name out=$GATE/$name.result
	local LIB=$dir/src/.libs U=$dir/tests/unit/.libs/test_urcu_ft_unit
	local I=$dir/tests/regression/.libs/test_urcu_ft_inv
	local SP=$dir/tests/unit/.libs/test_rcu_txn_settle_premise
	: > "$out"
	if ! make -C "$dir/src" -j"$J" >"$GATE/$name.build" 2>&1; then
		echo "$name: BUILD FAIL (see $GATE/$name.build)" >> "$out"; return
	fi
	# libtap et al must exist first, else the test binaries fail to link
	# silently and every suite reports ok=0 (a false PASS -- see the ok=0
	# gate below).
	make -C "$dir/tests/utils"      -j"$J" >>"$GATE/$name.build" 2>&1
	# The TEST binaries' build status is checked, not just the library's: a
	# test source that fails to compile ONLY in this config (an #ifdef'd
	# region the default build never sees) would otherwise leave the previous
	# run's binary in place, and the suite would report that STALE binary's
	# results as if they were this config's -- a false PASS, or a false FAIL
	# attributed to the wrong change.  Both happened before this check.
	if ! make -C "$dir/tests/unit" test_urcu_ft_unit -j"$J" \
			>>"$GATE/$name.build" 2>&1; then
		echo "$name: TEST BUILD FAIL (ft_unit; see $GATE/$name.build)" >> "$out"
		return
	fi
	if ! make -C "$dir/tests/regression" test_urcu_ft_inv -j"$J" \
			>>"$GATE/$name.build" 2>&1; then
		echo "$name: TEST BUILD FAIL (ft_inv; see $GATE/$name.build)" >> "$out"
		return
	fi
	# Only the configs that ASK for it (tests list contains sp) build the
	# settle-premise validator, because it is only meaningful where
	# -DURCU_TXN_DEBUG_SETTLE is defined -- elsewhere it compiles to a skip.
	case " $tests " in
	*" sp "*)
		if ! make -C "$dir/tests/unit" test_rcu_txn_settle_premise -j"$J" \
				>>"$GATE/$name.build" 2>&1; then
			echo "$name: TEST BUILD FAIL (txn settle; see $GATE/$name.build)" >> "$out"
			return
		fi ;;
	esac
	# Likewise the urcu-memb ft_inv: only the configs that list imb.
	case " $tests " in
	*" imb "*)
		if ! make -C "$dir/tests/regression" test_urcu_ft_inv_memb -j"$J" \
				>>"$GATE/$name.build" 2>&1; then
			echo "$name: TEST BUILD FAIL (ft_inv memb; see $GATE/$name.build)" >> "$out"
			return
		fi ;;
	esac
	echo "$name: build ok" >> "$out"
	local t o ok notok abrt lbl rc plan ran sp rep cdir bin tmo c
	local env_x
	# ★ KEEP THE CORE.  Counting aborts names the CONFIG; it does not name
	# the slot, the writer or the arm -- and both instances of the
	# raw-read-of-a-parked-slot class closed so far were root-caused from a
	# CORE, after gdb had failed to reproduce either one under a debugger
	# (single-stepping closes the window).  So every leg below runs in its
	# OWN directory with the core limit raised, and that directory SURVIVES
	# iff the leg went red: a green matrix leaves nothing behind, a red one
	# leaves the evidence next to the line that names it.
	# The per-leg cwd is not tidiness.  core_pattern is a bare RELATIVE
	# "core" on the usual configuration, so without it the cores land
	# wherever the gate was invoked from -- the repo, polluting git status --
	# and concurrent legs overwrite each other's.  core_uses_pid then names
	# the file core.PID, so glob core*, never exactly "core".
	local cores=$GATE/$name.cores
	rm -rf "$cores"; mkdir -p "$cores"
	# ★ THE SPACINGS RUN CONCURRENTLY, and this is what makes a swept config
	# affordable.  Serialised, each three-spacing config took 981 s and was
	# the whole gate's critical path -- the 14-config matrix measured 1053 s,
	# i.e. one swept config plus noise, with eleven others idling alongside
	# it.  The axis is embarrassingly parallel (separate processes, separate
	# cwds, one shared read-only build), so sweeping it now costs ONE
	# spacing rather than three: anchorval 1017 s -> 365 s, the whole matrix
	# 1053 s -> 383 s, same 61 legs, same verdict.
	# Peak concurrency rises from ~14 legs to ~19; measured load stayed near
	# 54 on this 192-core box, so the axis fits without a throttle.  Add one
	# here if a future matrix makes (configs x spacings) outgrow the machine.
	#
	# Each spacing writes its OWN result file.  They are concatenated below in
	# the DECLARED order, never in completion order: appending concurrently to
	# one file interleaves the lines of a red with the lines of a green, and an
	# unattributable red is worse than a slow gate.  It also keeps two runs of
	# the gate diffable, which completion order would not.
	local sp
	for sp in $spacings; do
		run_spacing "$name" "$tests" "$sp" "$GATE/$name.result.$sp" &
	done
	wait
	for sp in $spacings; do
		cat "$GATE/$name.result.$sp" >> "$out" 2>/dev/null
		rm -f "$GATE/$name.result.$sp"
	done
	# Every leg was green, so nothing was kept: drop the empty spine too.
	rmdir "$cores" 2>/dev/null
}

run_spacing() {	# $1=name $2=tests $3=spacing $4=outfile -- every leg at ONE spacing
	local name=$1 tests=$2 sp=$3 out=$4
	local t rep
	: > "$out"
	# REPEAT: an intermittent defect is a coin flip at one run.  The two
	# measured instances of the raw-read class fired at ~10% and ~8%, so a
	# single green leg is not evidence.  Opt-in (default 1) because it
	# multiplies the whole matrix; use it when hunting, not on every commit.
	#
	# The legs of one config run IN PARALLEL.  They are separate processes
	# with separate cwds over one read-only build, like the spacings above.
	# Run one after another, a config's wall time was the SUM of its legs,
	# and the machine sat mostly idle (measured: 14% busy on 384 CPUs).
	# Each leg writes its own result file.  They are joined in DECLARED
	# order, for the same reason as the spacing files.
	for rep in $(seq 1 "${FT_GATE_REPEAT:-1}"); do
	for t in $tests; do
		run_test_leg "$name" "$t" "$sp" "$rep" "$out.$rep.$t" &
	done
	done
	wait
	for rep in $(seq 1 "${FT_GATE_REPEAT:-1}"); do
	for t in $tests; do
		cat "$out.$rep.$t" >> "$out" 2>/dev/null
		rm -f "$out.$rep.$t"
	done
	done
}

run_test_leg() {	# $1=name $2=test $3=spacing $4=rep $5=outfile -- ONE leg
	local name=$1 t=$2 sp=$3 rep=$4 out=$5
	local dir=$GATE/$name
	local LIB=$dir/src/.libs U=$dir/tests/unit/.libs/test_urcu_ft_unit
	local I=$dir/tests/regression/.libs/test_urcu_ft_inv
	local IM=$dir/tests/regression/.libs/test_urcu_ft_inv_memb
	local SP=$dir/tests/unit/.libs/test_rcu_txn_settle_premise
	local cores=$GATE/$name.cores
	local o ok notok abrt lbl rc plan ran cdir bin tmo c mem
	local env_x
	: > "$out"
	cdir=$cores/$sp-$t-$rep
	mkdir -p "$cdir"
	# The multi-process arm reports per copy and keeps its own cores, so
	# it bypasses the single-process parsing below entirely.
	if [ "$t" = imwx ]; then
		run_leg_multi "$name" "$cdir" "$sp" "$I" "$LIB" "$out"
		return 0
	fi
	case $t in
	u)    bin=$U; tmo=900;  lbl="ft_unit   "; env_x=() mem=$FT_GATE_MEM_U ;;
	ion)  bin=$I; tmo=300;  lbl="ft_inv on "; env_x=() mem=$FT_GATE_MEM_INV ;;
	ioff) bin=$I; tmo=300;  lbl="ft_inv off"; env_x=(FT_INV_NO_ORDERED_LIST=1) mem=$FT_GATE_MEM_INV ;;
	# The concurrent-writer oracles (MW writers, coherent rekey) all
	# gate on FT_INV_MW at runtime and are otherwise skipped, so ion
	# / ioff never exercise them.  They are the long leg -- hence the
	# larger timeout.
	imw)  bin=$I; tmo=1800; lbl="ft_inv mw "; env_x=(FT_INV_MW=1) mem=$FT_GATE_MEM_IMW ;;
	# The same oracles on urcu-memb, where a read-side section is
	# real.  A section held across a grace period hangs or stalls
	# reclaim here; under QSBR it is invisible.
	imb)  bin=$IM; tmo=1800; lbl="ft_inv memb"; env_x=(FT_INV_MW=1) mem=$FT_GATE_MEM_IMW ;;
	# The settle-premise DETECTOR's own validation.  It belongs to a
	# config that DEFINES -DURCU_TXN_DEBUG_SETTLE and nowhere else:
	# without the flag it skips, and a detector whose self-test only
	# ever skips is indistinguishable from one that is blind.  It
	# constructs its violation rather than waiting for one, so it is
	# ~20 ms and carries no spacing dependence.
	sp)   bin=$SP; tmo=120; lbl="txn settle"; env_x=() mem=$FT_GATE_MEM_SP ;;
	esac
	o=$(run_leg "$cdir" "$tmo" "$mem" env LD_LIBRARY_PATH="$LIB" \
		${env_x[@]+"${env_x[@]}"} "$bin"); rc=$?
	# The SPACING rides every label below: a red that names only the
	# suite is unattributable when three spacings run the same leg.
	lbl="$lbl $sp"
	ok=$(printf '%s' "$o" | grep -c '^ok ')
	notok=$(printf '%s' "$o" | grep -c '^not ok ')
	abrt=$(printf '%s' "$o" | grep -c -i 'assert')
	printf '  [%-11s] %s ok=%s notok=%s abrt=%s\n' "$name" "$lbl" "$ok" "$notok" "$abrt" >> "$out"
	# NAME the failures, do not just count them.  An INTERMITTENT red is
	# unattributable from a count alone: you cannot tell a known flake
	# from a new regression without re-running and hoping it recurs.
	# (Cost real time once: a fault-audit red that never reproduced.)
	if [ "$notok" -gt 0 ]; then
		printf '%s' "$o" | grep '^not ok ' | sed "s/^/      [$name] $lbl /" >> "$out"
	fi
	# A HUNG suite used to score GREEN.  The run is killed, its PARTIAL
	# ok-count is reported, and with no 'not ok ' line and no abort the
	# config passed -- so a single-threaded DLM self-deadlock rode in on
	# a "GATE PASS" while dlm's ft_unit silently fell from 290 tests to
	# 36.  The count drop was the only signal and nothing checked it.
	# Two independent checks now, because each catches what the other
	# misses: a non-zero exit (124 == timeout kill, or a crash after the
	# last emitted line), and a run that never reached its TAP plan
	# (self-maintaining -- the plan is the suite's own test count, so
	# adding tests needs no gate update).
	# Most-specific diagnosis first: a timeout and a plan shortfall have
	# very different causes (hang vs retired-but-still-planned test), and
	# libtap's exit_status() is planned-minus-run, so a bare "exit 3"
	# would report the shortfall in its least legible form.
	plan=$(printf '%s' "$o" | sed -n 's/^1\.\.\([0-9]\{1,\}\)$/\1/p' | head -1)
	ran=$((ok + notok))
	if [ "$rc" -eq 124 ]; then
		echo "$name: $lbl INCOMPLETE (TIMEOUT/hang after $ran tests)" >> "$out"
	elif [ "$rc" -gt 128 ]; then
		echo "$name: $lbl INCOMPLETE (killed by signal $((rc - 128)) after $ran tests)" >> "$out"
	elif [ -z "$plan" ]; then
		echo "$name: $lbl NO TAP PLAN (completeness unverifiable)" >> "$out"
	elif [ "$ran" -ne "$plan" ]; then
		echo "$name: $lbl INCOMPLETE (ran $ran of $plan planned)" >> "$out"
	elif [ "$rc" -ne 0 ]; then
		echo "$name: $lbl NONZERO EXIT ($rc) with a complete run" >> "$out"
	fi
	# Keep this leg's directory iff it went red, and say where the core
	# is (see the KEEP THE CORE note above).  "Red" is deliberately
	# WIDER here than the RESULTS grep below: a leg killed by a timeout
	# or a signal leaves a core too, and that core is the entire point
	# of having kept the directory.
	if [ "$notok" -gt 0 ] || [ "$abrt" -gt 0 ] || [ "$rc" -ne 0 ] \
			|| [ -z "$plan" ] || [ "$ran" -ne "$plan" ]; then
		c=$(ls "$cdir"/core* 2>/dev/null | head -1)
		if [ -n "$c" ]; then
			echo "$name: $lbl CORE kept -- LD_LIBRARY_PATH=$LIB gdb $bin $c" >> "$out"
		fi
	else
		rm -rf "$cdir"
	fi
}

echo "gate dir : $GATE   (-j$J x ${#CONFIGS[@]} configs on $NCPU cores)"
echo "=== [1/3] configure per-config trees (parallel; one-time) ==="
for c in "${CONFIGS[@]}"; do IFS='|' read -r n f _ <<< "$c"; setup_tree "$n" "$f" & done; wait
echo "=== [2/3] sync live sources (parallel) ==="
for c in "${CONFIGS[@]}"; do sync_src "${c%%|*}" & done; wait
echo "=== [3/3] build + test (parallel) ==="
for c in "${CONFIGS[@]}"; do IFS='|' read -r n f t sp <<< "$c"; run_one "$n" "$t" "$sp" "$f" & done; wait

echo "=== RESULTS ==="
rc=0
for c in "${CONFIGS[@]}"; do
	n=${c%%|*}
	if [ ! -s "$GATE/$n.result" ]; then
		echo "  [$n] NO RESULT (harness error -- see $GATE/$n.build)"; rc=1; continue
	fi
	cat "$GATE/$n.result"
	# Fail on a bad build, any failing/aborted test, or a missing 'build ok'.
	# Also fail on ok=0: a test suite that produced zero 'ok' lines built
	# but never ran (missing/unlinked binary, e.g. tests/utils not built) --
	# notok/abrt alone would let that pass as a false GREEN.
	if ! grep -q 'build ok' "$GATE/$n.result" || \
	# NONZERO EXIT belongs in this list: run_one PRINTS it and nothing acted
	# on it, so a leg that completed its plan with no failing TAP line and a
	# non-zero status -- a crash in teardown after the last test, an explicit
	# exit(1) -- reported its own red line under a "GATE PASS".
	   grep -qE 'BUILD FAIL|CONFIG ERROR|notok=[1-9]|abrt=[1-9]|ok=0 notok=|INCOMPLETE|NO TAP PLAN|NONZERO EXIT' "$GATE/$n.result"; then
		rc=1
	fi
done
if [ "$rc" -eq 0 ]; then echo "=== GATE PASS ==="; else echo "=== GATE FAIL ==="; fi
exit "$rc"
