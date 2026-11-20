# MW → fine locking: the remaining half — transition plan (2026-08-23)

Status: **IN EXECUTION**. Branch `ft/unpub-free-audit` (Phase A is COMPLETE —
G2, A1 and A2 landed; Phase B's mechanism — the owner parameter, the
record-time assert and its readiness counters — is LANDED as step B0, and its
first measurement says NO site is owner-complete yet, so each B1-B5 step now
opens by closing that site's ownership gap: §4).

Companion docs: `mw-writer-lock-escalation-model.md` (the pivot's master note,
§9 lock-sets / §11 migration posture), `ft-dlm-lock-coarseness.md` (the anchor
schedule + acquire-site inventory), `whole-trie-mcas-to-sw-migration.md`
(the pure-sw engine scope — partially superseded, see gate G3).

---

## Execution guide (implementer: START HERE)

Decisions already taken (2026-08-23, Mathieu): **G1 = YES** (writer exclusion
is sufficient for readers — arming unblocked), **G2 = APPROVED** (always-MW
root helper, retire `sw_exempt_slot`). G3–G5 do not block the steps below.

### The rig

    build-g7   ../configure --enable-rcu-debug          # knob OFF
    build-tk   CFLAGS="-g -O2 -DFT_DEBUG_TXN_KIND" ../configure --enable-rcu-debug
    ulimit -v 40000000 ; always run from the BUILD dir, never the source tree
    smoke = test_urcu_ft_unit + test_urcu_ft_inv (FT_INV_MW=1), in parallel
    the kind-counter table prints to STDERR at process exit (destructor)

Before trusting any `--enable-rcu-debug` green, verify the engine self-checks
are ARMED: a two-line TU probing
`#if defined(DEBUG_RCU) || defined(CONFIG_RCU_DEBUG)` must compile the armed
branch — a release build silently poisons the descriptor and retry loops
absorb it (§10). Expected baseline: ft_unit 313 ok / 3 deliberate rekey
failures (109/111/122); ft_inv 119/119; ☠ 4 concurrent ft_inv copies FAIL
`inv_remove_cross_view` — PRE-EXISTING (§9.2), record a control run before
your first change and compare after each step; it must not get worse.

### Step 1 — G2: always-MW root records — ☑ LANDED `a961c6e6`

`ft_flip_txn_record_root` is the mechanism; `sw_exempt_slot`, the record-time
compare, the constructor plumbing and the slot parameter of
`ft_flip_txn_set_structural_sw` are deleted; the two ☠ dual caveats are
rewritten. Behaviour-identical by construction (armed or not, a root edge
reached `urcu_txn_store_mw` before and after), so no abort measurement applies.

☠ **THE §2/G2 SITE MAP BELOW WAS INCOMPLETE — do not trust a site list for
this class.** Two site classes it does not name were found by the detector,
and both would have parked a root the moment A1 arms:

* `cds_ft_graft_swap` (`ft-graft.h:3998`) builds its OWN edge array with
  `&swap_ft->root` and commits it through neither choke point.
* `ft_merge_spine_copy` / its rekey twin publish into `pub_slot ==
  d_dst->nfp`, which **is** `&dst_ft->root` at depth 0.

The lesson generalizes to every later step: root-ness is carried per EDGE
(`ft_pub_rec.root`, `ft_ord_cell_edge.root`), set at the publish branch that
already emits `root_publish`, and dispatched in all five replays. A new
publish path must set it, and the `--enable-rcu-debug` detector in
`ft_flip_txn_record_tag` (armed regardless of `structural_sw`) is what will
say so — it was proven to FIRE, by reverting one `edges[].root` into a red
control, not merely proven armed.

Measured: the reclassification lands at exactly ten root-swap sites; the armed
rekey site keeps `MW_STRUCT == 0`, and its expected small MW_STRUCT →
MW_ALWAYS shift is **zero** — its baseline MW_STRUCT was already zero, so that
writer records no roots in this workload.

### Step 2 — A1: arm COARSE non-exclusive — ☑ LANDED, FOUR commits

Not one commit, and not a predicate flip. The arm itself is
`ft_txn_content_sw_ok` → `!ft->lock_fine && !ft->exclusive` (`543e7c53`), but
three prerequisites had to land first, and each is the same lesson the §2/G2
site map already taught:

* `dc2cfde0` — **the glue's re-parent marks are a FINE-mode acquire.**
  `ft_glue_acquire_reparent_marks` gated on arming while its sibling
  `ft_glue_acquire_splice_holders` gates on `ft->lock_fine`; the mark defends
  against ONE peer (`ft_meta_nr_child_inc` from an insert below the re-homed
  child), and it is FINE that leaves that peer unexcluded. Behaviour-neutral
  at the time it landed BY MEASUREMENT: 1,272,204 glue commits over the
  FT_INV_MW plan, 26,787 armed, ZERO of them COARSE.
* `a34db457` — **a deferred edge carries its own reader-reachability.**
  `ft_glue_apply_deferred` chose store-vs-record from
  `g->txn->structural_sw`, which answers how a record is KINDED, not whether
  a reader can see the slot. The new `@live` bit is set at defer time and
  selected the identical edge set under an equivalence probe (0 divergences
  over 1,595,734 src-origin edges), so it too landed neutral.
* `9ba72990` — `FT_TK_TXN_INIT` ran AFTER the constructor's arm decision, so
  arming counted against an uninitialised `dbg_site`. Latent exactly as long
  as nothing armed; the counter build faults inside the trie's insert path
  the moment A1 lands, which reads as a trie defect rather than an instrument
  that has not been told its own name.

**The positive control** (`inv_concurrent_writers_coarse_lock`, build-tk):
MW_STRUCT 369,694 → **0** across `ft-insert.h:769`, `ft-remove.h:2875` and
`ft-remove.h:880`; armSW 0 → 8,898; SW 0 → 415,790; ABORT and MEMERR stay 0.
`test_writer_lock_mode_*` (5) and both `*_coarse_lock` oracles run, not skip.

**Protocol results.** rcu-debug gate 313 ok / 3 deliberate + 119/119; ASAN
same, no AddressSanitizer report; fault-inject 367 ok / 3 + 119/119; the
concurrent-copy control run like-for-like against a `5051c223` worktree built
the same way — 4/4 and 8/8 clean on BOTH arms, so §9.2's pre-existing
`inv_remove_cross_view` did not fire this time and the control bounds only
NEW breakage, not the pre-existing rate.
Reservations re-checked under `-DURCU_TXN_DEBUG_RESERVE` (proven to fire by
under-reserving the graft glue txn into a red control): no overflow.

☠ **THE ABORT COLUMN DOES NOT MOVE, and that is the correct outcome — do not
go looking for A1's share of the 29%.** On a COARSE trie every writer holds
the FT-wide mutex, so the content lanes had NO aborts to relieve: the coarse
oracle's ABORT column is 0 before and 0 after. Over the whole FT_INV_MW plan
the total abort RATE is 1.95–2.37% before and 1.82–2.39% after (n=3 each,
alternated) — overlapping ranges, i.e. no effect distinguishable from
run-to-run spread, while armSW rises 390k–475k → 544k–586k and SW 2.9M–3.6M →
4.5M–4.8M. A single before/after pair reads as a 29% abort drop and is an
artifact; the acquire lane, which A1 does not touch, moves further than the
content lanes do. **The 29% content-lane bound is Phase B's to collect.**

☞ What A1 buys is the record-kind conversion and the path it clears, not
abort relief. Two red controls stand behind it: reverting `a34db457` under
the arm aborts ft_unit at test 61 on `_ft_publish_to_parent_meta`'s
`assert(cp != NULL)` and silently records 137 HIDDEN back-edges over the
FT_INV_MW plan with every test still passing; reverting `dc2cfde0` alone is
green. The 08-23e residual `not ok 70 inv_graft_swap_shared_dst_ksfix_solo`
does NOT reproduce in any of the three configurations.

### Step 3 — A2: + exclusive — ☑ LANDED `f6093f8b` (one commit)

`ft_txn_content_sw_ok` → `!ft->lock_fine || ft->exclusive`. ☠ A SECOND RULE,
not a widening of A1's: an exclusive trie takes **no mutex at all**
(`ft_writer_lock_scope_enter` returns early on it, deliberately, so a
cross-trie op can hold one live side's lock while an exclusive consumed source
rides through the fused body). The exclusion is the caller's single-writer
contract, which `cds_ft_make_exclusive` drains readers behind
(`ft_writer_lock_gp_wait` before the flag is set).

**The audit that mattered was the CROSS-TRIE direction**, because a txn arms
off the trie it NAMES while it may record a second trie's slots, and
"exclusive" is a claim about one trie. §3's existing note covers only the
dst-named direction. The src-named one — graft's `src_retire` / `swap_retire`
/ `extract`, merge's `src_side` / `run_unlink` — holds because those record
NOTHING BUT their own trie's slots: `ft_glue_apply_deferred`'s src pass stores
rather than records for them (`@live` false), root edges carry `@root` and are
forced MW, ordered-cell edges carry `URCU_TXN_TAG` and are likewise forced MW.
Exclusivity spans the whole op — the BUSY gates refuse a live `swap_ft`
(ft-graft.h) / `src_ft` (ft-merge.h) up front, and the one place that MUTATES
the flag (graft_swap handing `swap_ft` dst's discipline) runs after every
commit.

☞ **The site list came from the COUNTERS, not from reading.** Arming into a
throwaway build and diffing armSW per site named the seven sites A2 actually
opens, which is a far smaller and more honest audit surface than the ~40
creation sites — and it is the same lesson as §2/G2's incomplete site map,
applied before the fact instead of after.

**Positive control.** `ft_unit` under `-DFT_DEBUG_TXN_KIND` is deterministic
and both runs agree on `created` to the txn (3,695,680), so this is exact
rather than a sample: armSW 2,310 → 4,625, SW 27,774 → 41,552, ABORT 0 both
ways, and MW_STRUCT goes to exactly **0** at every newly-armed site. On the
FT_INV_MW plan the extract-side txn (`ft-graft.h:3545`) moves MW_STRUCT
898,064–941,821 → **0**, with 926,651 SW records and 1,088,219 armed.

Gates: rcu-debug 313 ok / 3 deliberate + 119/119; `-DURCU_TXN_DEBUG_RESERVE`
clean; ASAN same, no report; fault-inject 367 ok / 3 + 119/119; 8-copy control
8/8 clean on BOTH arms against a `570f0a3c` worktree. No abort claim, per the
step protocol above.

☞ The hand-arming is NOT retired by this step: the rekey writer arms under
FINE (a per-op argument, Phase B) and the root-COW driver names an arbitrary
trie. Both still arm on their own reasoning.

### Step 4 — B0: the record-time owner mechanism — ☑ LANDED

The owner parameter, the record-time assert, the dry-run claim and its red
control, plus the `OWN_HELD`/`OWN_MISS` readiness counters — all in §4, with
the measurement they produced. Behaviour-neutral by construction: the owner is
read only under `--enable-rcu-debug`, and no site arms per-op yet.

Gates: rcu-debug 313 ok / 3 deliberate + 119/119 with `FT_INV_MW=1`, no
assertion; `FT_RED_OWNER_CLAIM_ON_LOCK` aborts on ft_unit's second test, by
this assert's name. No abort claim applies (nothing converted).

★ Behaviour-neutrality is MEASURED, not asserted: the full 13-config
`ft_parallel_gate.sh` was run at this commit and at `4887aaab`, and **all 115
legs are identical** — same ok / notok / abrt per leg. That is the strongest
form of "changes nothing" available here, and it is what a step whose whole
product is a DETECTOR should be held to. ☠ It also means the gate was already
RED before this step: see §9.5.

☞ The owner is an UNCONDITIONAL parameter — unlike `FT_TK_SITE_PARAM`, which
is knob-gated precisely so an instrument cannot perturb what it measures — so
some sites now compute a metadata pointer a release build never reads.
Measured rather than argued, like-for-like `-g -O2`: `liburcu-cds` text
603,177 B before, 602,853 B after (**−324 B**). The optimizer drops the
derivations; the parameter is free where it is not read.

### Then

Phase B's mechanism is done; what it revealed is that **no site is
owner-complete**, so each site step now starts with closing its ownership gap
(§4). Read §2/G5 for the bulk-op freeze gate that Phase B's bulk sites will
sit behind. §11 has the full landing order.

---

## 0. Where we stand (the done half)

The model, one breath: a structural op acquires its per-node DLM lock-set in
ONE all-or-none MCAS (the MW arbitration point), then edits under the held
locks; readers stay wait-free (proxy resolve, never see the lock). The
transition's first half converted the EXCLUSION machinery; the second half
converts the CONTENT records from MW (validated CAS per slot) to SW (plain
parks under the held locks) and then collects the payoffs that exclusion
enables.

Landed (exclusion, complete):

* DLM one-commit acquire on every op family (recompact, insert, remove/detach,
  merge, graft/graft_swap, rekey); cross-trie ops require an EXCLUSIVE
  consumed source (BUSY gate), dst-side-only lock-sets.
* `CDS_FT_WRITER_OPTIMISTIC` is **deleted** — the enum is
  `{LOCK_COARSE = 1, LOCK_FINE = 2}` and **FINE is the default**
  (`include/urcu/fractal-trie.h:2867`). The per-slot-CAS MW mode Mathieu
  marked for deprecation no longer exists.
* The FT-wide writer lock is dropped per-domain under FINE; escalation-domain
  enrolment is complete at every measured acquire site (0 domain-less, was
  2.3M), with aging-on-refusal (which beat the `_on` binding: 4 vs 9.5
  starving removes).
* The mixed SW/MW txn engine is the vendored substrate (`rcu-txn-mcas.h`):
  MW records commit-ordered FIRST (can fail, back out clean), SW records park
  just before the flip. This is what makes a per-record-kind conversion
  possible at all.
* Lock-spacing (anchor coarseness): capture, knob, and the FULL acquire-site
  conversion are in — every acquire resolves through the two choke points and
  the raw primitives do not compile under `FEATURE_FT_ANCHOR_VALIDATE`
  (`ft-dlm-lock-coarseness.md` §9). The coarse settings are still refused at
  the API (`ft-lifecycle.h:369`) pending certification (Phase E below).

Landed (content, staged but UNARMED):

* `6815f178` — per-creation-site record-kind counters
  (`-DFT_DEBUG_TXN_KIND`, `ft-txn-kind-stats.h`).
* `a45869d2` — the CONTENT / ACQUIRE constructor split: content constructors
  take `struct cds_ft *ft` (56 sites, compiler-enforced); the one acquire
  site uses `ft_flip_txn_acquire_bounded()` and names no trie.
  `ft_txn_content_sw_ok(ft)` (`ft-mutation-helpers.h:1154`) is the single
  policy switch and returns **false** — nothing armed.
* Two sites still HAND-arm `structural_sw` on their own reasoning: the rekey
  writer (`ft-rekey.h:1754`) and the root-COW debug driver
  (`fractal-trie.c:357`). Retiring that hand-arming is part of the done
  definition.

### The baseline (ft_inv FT_INV_MW=1, 507 threads, 94.9M txns)

    MW_STRUCT   136,857,021   <- THE CONVERSION SURFACE, 27 content sites
    MW_ALWAYS    85,087,969      cells / counts / conditional re-parents: stays MW
    MW_LOCK      52,667,070      the lock take: stays MW forever
    VALIDATE     23,383,128      §4.B guards
    cell/hlist   17,551,703      ordered cells: stays MW (deliberate)
    ABORT         2,415,932   <- the acceptance number

* 71% of all aborts are the ACQUIRE lane (`ft_dlm_acquire_set_at`,
  1,718,411). Content lanes total 697,521 — **the conversion's upper bound on
  abort relief is ~29% of aborts**, not the headline count. Say so in any
  claimed win.
* MW_STRUCT is concentrated: `ft-insert.h:774` (50.2M) + `ft-remove.h:2875`
  (46.8M) + `ft-remove.h:3716` (15.7M) + `ft-remove.h:880` (9.6M) +
  `ft-remove.h:3942` (4.9M) = **127M of 137M. Five creation sites, all
  single-trie.** Convert those and the rest is noise.
* Positive control: the one already-armed site (the rekey writer) reports
  armSW 457,757 / SW 3,472,296 / MW_STRUCT **0**. A conversion that leaves
  MW_STRUCT in place at its site did not run.

Baseline test state: `--enable-rcu-debug` ft_unit 313 ok / 3 deliberate rekey
failures (109/111/122); ft_inv 119/119 with FT_INV_MW=1; ☠ ft_inv ABORTS with
4 concurrent process copies (`inv_remove_cross_view`) — pre-existing, and the
single-process gate cannot see it (§9.2).

---

## 1. End state (definition of done)

Two writer modes, one granularity axis, one mixed-commit engine:

1. **Single-writer** — the caller guarantees one writer (readers stay
   concurrent and wait-free). Today this mode is only approximated
   (`ft->exclusive` excludes readers too; COARSE pays a mutex the caller may
   not need). A validation-only strategy is owed (§8.4).
2. **DLM / LOCK_FINE** — per-node lock-sets, with the lock-spacing axis
   (per-node … exponential … root-only) as the coarseness knob. At root-only
   spacing this degenerates into today's LOCK_COARSE, which then becomes a
   redundant strategy to fold away (Phase E).

Record kinds at end state:

| kind        | fate                                                          |
|-------------|---------------------------------------------------------------|
| MW_LOCK     | stays MW forever — the lock take IS the arbitration point     |
| MW_STRUCT   | **→ SW** wherever the op's lock-set owns the written word     |
| MW_ALWAYS   | stays MW unless the Phase-C decision converts a class         |
| cell/hlist  | stays MW (deliberate: no per-cell serialization)              |
| VALIDATE    | stays — read-set validation is orthogonal to record kind      |

Done means: `ft_txn_content_sw_ok` answers true for COARSE, exclusive, and
the armed FINE ops; the two hand-armed sites route through the switch;
MW_STRUCT ≈ 0 at every converted site under the counter build; the engine's
`--enable-rcu-debug` kind-conflict self-checks are green under load; the
abort delta is measured and reported against the 29% bound.

---

## 2. Decision gates (before Phase A arms anything)

### G1 — is writer exclusion SUFFICIENT FOR READERS? — **ANSWERED YES (Mathieu, 2026-08-23)**

An SW park is legal only where the op excludes every peer WRITER of the slot.
Established: resolve is identical for both kinds (a parked SW record is
reader-visible exactly like an MW one), with ONE asymmetry — a LONE SW record
commits as a plain release store with **no proxy and no grace period**
(`urcu_txn_desc_commit`, `t->nr == 1`), where a lone MW record below the
escalation age commits as a bare CAS. Neither publishes through the flip.
**Decision: writer exclusion is sufficient for readers; the lone-record
asymmetry is acceptable. Arming is unblocked.**

### G2 — retire `sw_exempt_slot`: route root republishes through an always-MW helper — **APPROVED (Mathieu, 2026-08-23)**

A cross-trie dual txn names ONE trie and may write TWO roots
(`ft-graft.h:1362`, `:2853`), while the exemption holds one slot — arming
such a txn would park the second root unarbitrated. Two ways out were named;
**decision: the always-MW record helper** (a `ft_root_record_*` that
records any `&ft->root` edge MW regardless of `structural_sw`), because the
rule it encodes is "a trie root lives in no node" — a property of the slot,
not of the txn's named trie — and it deletes the exemption plumbing
(`sw_exempt_slot`, the record-time pointer compare at
`ft-mutation-helpers.h:2675`) instead of generalizing it. Land this FIRST; it
makes every later arming step shape-independent and turns the two ☠ call-site
caveats into dead text.

#### G2 implementation map (surveyed 2026-08-23 at `a45869d2`)

★ **The exemption is a record-time compare BECAUSE root-slot records reach
the generic record path through DYNAMICALLY RESOLVED slots** — a NULL parent
degrades to a publish into `&ft->root` (`ft-mutation-helpers.h:118`, `:4347`,
`:4359`; `ft-mutation-node.h:1479`, `:1526`). So G2 is not a purely static
site conversion: those arms already KNOW the root shape (they branch on the
NULL parent and emit the `root_publish` tracepoint, `ft-helpers.h:2628`), so
each routes to the always-MW helper from its existing branch — the compare
moves from record time into branches the sites already take.

Conversion surface:

* **Two choke points cover the root/list swaps**: `ft_root_list_swap_publish`
  (7 call sites: ft-detach.h:187, ft-graft.h:2251/3787, ft-merge.h:2104/3555,
  ft-rekey.h:4200/5579) and `ft_root_list_swap_publish_dual` (the 2 dual
  sites, ft-graft.h:1462/2903). Convert INSIDE the helpers once.
* Direct `(void **) &…_ft->root` records beside those calls:
  ft-merge.h:2120/3572, ft-graft.h:2222/2264, ft-rekey.h:4216/5596.
* The root COW driver's forward publish: `fractal-trie.c:381`
  (`ft_flip_txn_record_reserved(txn, (void **) &ft->root, …)` — its comment
  at :367 already states the MW intent).
* The dynamic NULL-parent publish arms listed above.

Deletions once routed: the `sw_exempt_slot` field
(ft-mutation-helpers.h:1003), the compare (:2675), the constructor plumbing
(:1199/:1419/:1472 with their `&ft->root` arguments), the slot parameter of
`ft_flip_txn_set_structural_sw` (:1119/:2724), the ☠ block in
`ft_txn_content_sw_ok`'s header, and the two ☠ call-site caveats
(ft-graft.h:1362/:2853) — rewrite those as "roots record MW by
construction".

Counter classification: root records under an armed txn currently count
MW_STRUCT (they take the non-exempt branch's else). The helper should count
them **MW_ALWAYS** — a root can never convert, so moving it out of MW_STRUCT
cleans the conversion-surface metric. Consequently the armed rekey site's
table row shifts a small count from MW_STRUCT to MW_ALWAYS: expected, not a
regression. Optionally keep a debug-only assert that an SW park's slot is
never the NAMED trie's root — cheap belt while the plumbing is deleted; it
cannot cover the second trie's root, which is exactly why the helper, not
the assert, is the mechanism.

### G3 — confirm the engine end state is the MIXED commit, not pure-sw

`whole-trie-mcas-to-sw-migration.md` (2026-07-15) scoped REPLACING the MCAS
engine with `rcu-txn-sw`. The mixed SW/MW engine adopted a week later
(2026-07-22) changed the ground: cells, locks, and rank counts stay MW, so a
pure-sw engine can never carry a whole FT commit. Proposed doc-status update:
the pure-sw migration is RETIRED as a whole-engine goal; its §4 content
survives as exactly this plan's per-record conversion. (Its §4.G — the layout
split — re-enters at Phase F.)

### G4 — the residual MW_ALWAYS lanes (decide AFTER Phase B measures)

Mathieu's open granularity question: do ordered cells (and dup-chain splices,
rank propagation) keep a narrow MW lane, or grow a state word and join the
lock-sets? Deferred deliberately — see Phase C for the measurement that
decides it. Do not decide it on argument now.

### G5 — bulk vs point ops: who carries the exclusion, and by which mechanism

Goal (Mathieu, 2026-08-23): point ops must not contend on near-root nodes, so
the bulk/point mutual exclusion is the BULK op's to carry. The §9 point-op
lock-sets are already leaf-local; the residual near-root cases are (a) a
point op mutating a direct child of a junction — a genuine conflict, small
and correct; (b) recompact cascade climbs — rare by construction; (c)
rank-stats — coerced serial anyway; and (d) ☠ COARSE LOCK SPACING, which
anchors deep ops near the root BY DESIGN — a bulk-heavy trie should stay
per-node spacing, and Phase E's bench must measure the spacing axis against a
bulk MIX, not the point mix alone.

Five candidate mechanisms, with a proposed split:

**(A) The bulk op locks its WRITTEN FAN — never the subtree.** A moved
subtree's INTERIOR moves wholesale: no interior word is written, so a deep
point op shares no word with the bulk op and COMMUTES with the move (its
edit lands in the subtree wherever the subtree now hangs). Exclusion is owed
only on words both sides write: {junction / prune path} ∪ {the fan of
children whose edge words the op rewrites}. Cost O(fan) ≤
`FT_ENTRY_PER_NODE`, not O(subtree) — the §9.6 wholesale-move result
survives. Precedent: `ft_rekey_cow_stop` / `ft_glue_acquire_reparent_marks`
(`child_marked`). ☠ Bound from the REVERTED lock-the-sweep experiment:
fan-locking the HOT recompact path livelocks (a contended child fails the
ACQUIRE; escalation rescues only a COMMIT) — fan locks belong on RARE ops
only. Note the fan lock exists because pre-§8.3 the child's
`parent_slot_offset` shares the child's STATE word; **after the §8.3 layout
split the junction lock alone owns the whole written frontier and the fan
locks retire** (§8.1 carries this as a payoff). What (A) does NOT give:
semantic freeze — deep point ops proceed and commute. No lock frontier can
exclude them without point ops READING an ancestor word, which is exactly
the near-root traffic the goal forbids.

**(B) Swap the trie's concurrency mode and DRAIN MUTATIONS — the move-gate
pattern applied to writers (Mathieu's alternative).** The rekey reader gate
(`move_active` + piggybacked GP; one stable load per reader) generalizes: the
bulk op flips a per-trie writer-mode word, then drains in-flight point
mutations, runs writer-exclusive, flips back. Because every point mutation
already executes inside the caller's RCU read-side bracket, the drain is ONE
grace period: a mutation that loaded the old mode word finishes inside its
bracket; new arrivals see DRAINING and park. The point-op cost is a single
LOAD of a trie-level word that changes only at bulk boundaries — the line
stays shared in every cache, no stores, no refcount: strictly better for the
near-root goal than any lock. What (B) buys over (A): TRUE semantic freeze
(deep mutations excluded, not merely commuting), and the bulk window is
writer-exclusive so the bulk op's content records become SW-able by the
EXCLUSIVE argument — no fan discipline, no `child_marked` subtleties, the
hardest Phase-B cases collapse into the easy arm. What it costs: every point
mutation on the trie stalls for the window (tail latency), and the window
pays a GP even when no reader coherence needed one — though for the rekey
family the gate + GP are ALREADY on the path for reader coherence, so the
writer-side drain adds no new GP there. Constraints proven elsewhere that
bind (B): parked mutators must go `thread_offline` around their wait (the
QSBR owner-waits-for-waiter deadlock, fixed once already for the mover's
cond_wait); gate entry needs FIFO fairness both ways; ☠ and the drain's
latency bound is the longest point-op retry storm — under QSBR a spinning
retry loop has NO quiescent window, so Phase D's liveness work is a
PREREQUISITE for (B)'s drain to have a bound at all.  ☞ SUPERSEDED BY G5.3:
under (F) the drain GP is taken holding `writer_lock`, whose only waiters are
bulk ops that may park OFFLINE, so the bound does not depend on a point-op
park at all.

**(C) The exclusivity contract** stays what it is: cross-trie ops require an
EXCLUSIVE consumed source; detach-and-hand-off is a mode swap by definition.

**(D) The hybrid (Mathieu): point mutations LOAD a locking-mode state on
every node of their descent, root to leaf — bulk ops flip it only on the
JUNCTION they affect.** The mutation descent already loads each entered
node's state word for dispatch (kind, proxy, skip), so a per-level mode
check is a BRANCH on a word already in hand: zero extra loads, zero stores,
and loads of a rarely-written line do not contend — the strongest possible
fit for the near-root goal. Bulk protocol: set a FREEZE state on the
junction (taken UNDER the junction's already-held DLM lock-set, so
bulk-vs-bulk composition rides the existing acquire/validate machinery),
then ONE grace period drains the concerned in-flight mutations — every point
mutation runs inside the caller's RCU bracket, so after the GP each has
either finished or will see the freeze on its descent and park. The bulk op
then edits writer-excluded over the subtree — SW content by the exclusive
argument, scoped — clears the state and wakes. No commit-time validation of
the freeze is needed: load-and-branch + GP is the move-gate publication
pattern. Stall scope is exactly the ops whose descent crosses the frozen
junction; disjoint bulk ops run in parallel. **Placement generalizes: the
freeze on the ROOT is (B)** — one mechanism, placement chooses scope, the
same shape as the lock-spacing axis. Obligations: (1) handle-based
mutations do not descend (the §5.3 node-handle hole) — they RE-DESCEND when
the feature can be armed, the precedent `ft_anchor_descend` already
established for coarse spacing; (2) parked mutators go `thread_offline`
around the wait (the QSBR mover fix, again); (3) the drain bound is still
the longest in-flight point op, so Phase D remains the prerequisite —
though only retries INTO the frozen subtree matter (☞ moot: (D) is not taken,
and G5.3 retires this prerequisite for the mechanism that was); (4) readers never check
the state: wait-freedom untouched.

**(E) WIDEN THE POINT OP'S LOCK SET — a DYNAMIC, SUBTREE-LOCAL LOCK SPACING
(Mathieu, 2026-08-28).**  Not an exclusion primitive and not a freeze: a
GRANULARITY change.  A bulk op MARKS the junction J it is working on; a point op
whose ancestors include a marked node ADDS that node's lock to its own set.  The
two then arbitrate through the ordinary DLM acquire — nothing is evicted,
nothing aborts on principle, and no new engine mechanism appears (which retires
step4 §6.3's "per-subtree epoch the engine does not provide").  ★ **It IS the
anchor rule applied dynamically**: static lock spacing chooses how far up a
member anchors, and a bulk op at J makes J a RUNTIME anchor for its subtree.
"Swap the mode locally" is literal — the mode is the LOCK SPACING, scoped to a
subtree instead of to the trie.

*Detection is two-tier, so the steady state pays nothing.*  **Tier 1**: one
trie-level word, "is any bulk op live?", `caa_likely` false — the
`ft_move_active` shape (internal.h:2573).  Zero cost when no bulk op runs, which
is the case the near-root goal is actually about.  **Tier 2**: when it is
nonzero, the point op walks its ancestors via `meta->parent_word` and adds every
marked ancestor's lock to its set.  ★ The walk already loads that metadata line
at every level, so the mark is FREE exactly where it is read — and the mark
never needs to be visible on the DESCENT, which is what G5.1's encoding survey
was unable to make cheap.

☠☠ **THE ANCESTOR LOCKS ARE *ADDED*, NEVER SUBSTITUTED — a CORRECTNESS
REQUIREMENT, not a simplification.**  During the transition window point ops
divide into OBSERVERS (they saw the mark) and NON-OBSERVERS (they built their
set before it was visible to them).  If an observer REPLACED its leaf lock L
with J — the pure anchor reading — then an observer holding {J} and a
non-observer holding {L} on the SAME LEAF would share no word at all: two point
ops excluding nothing.  That is precisely the hazard
`cds_ft_group_attr_set_lock_spacing` refuses coarse spacings to prevent
("anchoring is all-or-nothing … converted sites anchoring on an ancestor and
unconverted ones on the node — excluding nothing, **and quietly**, since a
mostly single-writer suite still passes"), reproduced **IN TIME** rather than in
space: the window IS a partially-converted trie.  Keeping L gives the invariant
the design rests on —
* **L is never surrendered** ⇒ point-vs-point exclusion is IDENTICAL before,
  during and after the window; the mark changes nothing about how point ops
  synchronize with each other.
* **J is purely additive** ⇒ point-vs-bulk exclusion, for observers.
* **the GP** covers the non-observers, and is the only thing that must.

*Protocol.*  (1) bulk op acquires J through its txn; (2) marks J, bumps tier 1;
(3) ONE GRACE PERIOD — mutations run in the caller's RCU bracket, so afterwards
every non-observer has retired; (4) edits, holding only J: every op that could
commit underneath now takes J itself, so the bulk op's lock set is O(1), not
O(fan) as in (A) nor O(subtree); (5) unmark, decrement, release.  ★ **The exit
needs NO second GP**, and that asymmetry is a direct consequence of ADD: a stale
observer afterwards holds {L, J} against a fresh {L} and is still excluded on L.
Under substitution both ends would need one.

*What the address-ordered acquire imposes, and gives.*  Deadlock-freedom comes
from the txn sorting MW installs by SLOT ADDRESS at age 1+
(rcu-txn-mcas.h:542), not from any tree order — so an ancestor lock taken
"after" a descendant is a non-issue.  ☠ But the FT's own consequence binds
(ft-mutation-helpers.h:975): *"a site that wants ordering must present its WHOLE
SET AT ONCE."*  So the ancestor walk MUST complete before the set is presented;
a late-discovered ancestor cannot be appended.  ⇒ if the walk raced a
concurrent mark or a re-parent and missed one, the remedy is to re-plan and
present a new whole set — an ordinary abort/retry, i.e. age escalation, not a
new mechanism.  ☞ And an op whose tier-1 check says a window is live is by
definition an op that expects to conflict: `urcu_txn_expect_conflict`
(rcu-txn.h:583) skips the age-0 optimistic attempt and runs sorted+blocking from
the first try, instead of burning an attempt predestined to lose.  ☞ The lock
set grows by up to DEPTH entries during a window — affordable only because
@758f432d already made the registry GROW; that fix is a prerequisite for this
candidate and is already in.

☑ **THE WAIT IS NOT A BLOCK — MEASURED IN THE ENGINE, and it REFUTES both
liveness items this entry first carried.**  The age-1+ install spins
`URCU_TXN_WAIT_PATIENCE` = **8192** `caa_cpu_relax()` iterations
(rcu-txn-mcas.h:211, :577) and then does NOT block: it records `wait_capped`,
calls `urcu_txn_decide(FAILED)` and returns, after which SETTLE restores the
parked prefix `[0..planted)` to its OLD values (":617 — its new value on
SUCCEEDED, its old on FAILED").  So a contended acquire ABORTS; it never waits
out its opponent.  Two corrections follow, and they apply to **(B) and (D)
equally** — any candidate that makes a point op wait on a word a bulk op holds:

* ☠ **"BLOCKED WAITERS ARE PARKED, HENCE QUIESCENT, HENCE THE DRAIN GP
  COMPLETES" IS FALSE.**  Post-mark ops do not park.  They spin 8192, abort,
  re-descend, re-plan, spin again — for the whole window.  Under QSBR that
  retry loop has NO quiescent window, so it can starve **the bulk op's own
  drain GP**.  (E) therefore does NOT escape the starvation hazard; it
  inherits it.  ☠ And the abort is the EXPECTED outcome, not the exception:
  8192 relax iterations is tens to a few hundred microseconds against a window
  of p99 ~240us of hold (D.5) **plus a grace period**.
* ☠ **THE "STALL THAT LEAKS OUT OF THE SUBTREE" IS BOUNDED, not window-length.**
  An op does hold the locks sorting below J while it waits — but only for the
  capped spin, since settle-on-FAILED unwinds the parked prefix.  A real
  secondary stall, bounded by patience rather than by the bulk op.

⇒ ★ **THE DESIGN REQUIREMENT THIS PRODUCES: the mark must do DOUBLE DUTY.**  It
is not only "widen your lock set"; it must also route the waiter to a **PARK**
(`thread_offline` + wake at the unmark) instead of the ordinary
spin-abort-retry.  Without that, a bulk window converts into a trie-wide retry
storm across every op under J — wasted re-descents, and the drain GP endangered
by the very ops it is waiting on.  ⇒ **Phase D's open acquire lane ("the cure is
in the ACQUIRE — queue") is a HARD PREREQUISITE for (E), not an adjacent
concern**, and D.5's wait-ladder is the tool.  ☞ `urcu_txn_expect_conflict`
skips the doomed age-0 attempt but does NOT change this: the age-1+ path is the
one that caps and fails.

☐ **STILL OPEN.**  (i) Tier 1 is trie-wide, so any live bulk op makes every
point op ANYWHERE pay the O(depth) walk; refinable (a depth bound, a coarse
filter over the marked set) but start simple and measure.  (ii) A near-root J
serializes broadly by construction — the honest cost of bulk-near-root, worth
stating since G5's goal is about near-root contention.

**☑☑ DECIDED (Mathieu, 2026-08-28): (E) IS THE MECHANISM.**  Bulk/point
coordination is a GRANULARITY change, not an exclusion — the bulk op marks its
junction, point ops ADD the marked ancestors' locks, and the ordinary DLM
acquire arbitrates.  The split that stands:

* **(E) for the bulk family.**  It alone needs no new primitive, no per-node
  encoding and no engine change, and it reaches the interior writer that §5.3
  says a boundary cannot.  ★ Placement generalizes exactly as (D)'s did: a mark
  on the ROOT is the trie-wide case, so **(B) falls out of (E)** rather than
  competing with it — one mechanism, placement chooses scope.
* **(A) is RETIRED as an exclusion mechanism.**  Under (E) the bulk op holds
  O(1) — just the junction — because every op that could commit underneath
  takes it; the fan lock buys nothing it does not already have, and its fan half
  was retiring at §8.3 regardless.
* **(D) is NOT taken**: it freezes the boundary, which does not evict a writer
  already inside (§5.3), and its per-level check has no cheap home (G5.1).
* **(C) unchanged** for cross-trie: an EXCLUSIVE consumed source;
  detach-and-hand-off is a mode swap by definition.

☞ **SUPERSEDED BY (F), G5.3 (Mathieu, 2026-08-28).**  (E)'s granularity idea is
kept whole; what changes is WHERE THE EXCLUSION AUTHORITY LIVES — the FT-wide
`writer_lock` instead of the junction's own node lock.  That single move
dissolves this candidate's recorded prerequisite (the Phase-D park) and the two
hazards that killed every variant of it.  ☠ The paragraphs that stood here —
"PHASE D'S ACQUIRE LANE LANDS FIRST" and the (E) implementation order ending in
"the mark's second duty, routing waiters to the park" — are RETIRED; the park
was never implementable at the ops it had to serve (G5.3).  Read G5.3 for the
mechanism; (A)-(E) above remain the survey that produced it.

### G5.0 — (D)'s "ZERO EXTRA LOADS" PREMISE IS REFUTED; the COST CLAIM built on it was WRONG TWICE and is now scoped

Checked in-tree 2026-08-28, because (D) was proposed as the default on the
strength of one claim: *"the mutation descent already loads each entered node's
state word for dispatch (kind, proxy, skip), so a per-level mode check is a
BRANCH on a word already in hand."*  **That word is not loaded** — but the
first version of this section over-read that into a cost conclusion an
adversarial review then refuted on four counts, all confirmed in code.  Both
halves are recorded, because the survivor is narrower than it first looked.

☑ **WHAT HOLDS.**  Kind, proxy and skip are read from the TAGGED POINTER's low
nibble, never from `cds_ft_metadata::state`: `ft_node_get_nth_skip` reads the
node BODY's child array (ft-lookup-node.h:1584), `ft_resolve_flip_proxy` is a
tag test (ft-helpers.h:995).  **No per-level `state` read exists on any
mutation descent.**  So (D)'s literal premise is false, and a `FT_STATE_*` bit
remains the wrong encoding for the freeze (below).

☠ **WHAT DOES NOT.**
1. **"the reanchor's parent-chain walk runs on the MISMATCH path only" —
   FALSE.**  `ft_reanchor_flag` (ft-helpers.h:2361) fires on the skip-compressed
   ENCODING, not on a mismatch, and `ft_skip_reanchor` loads metadata
   (`cds_ft_item_to_metadata(...)->parent_word`) on its consistent exit
   (ft-helpers.h:1808, :1852).  So the mutation descent ALREADY touches 1-2
   metadata lines per SKIP-ENCODED level — and skip encoding is the default
   steady-state form of internal->compressed edges on 64-bit, not a transient.
   ★ This cuts BOTH ways: it strengthens "metadata is not free on the descent"
   while destroying the per-level accounting the conclusion rested on.
2. **"in a different arena from the node bodies" — FALSE.**  Same arena RANGE:
   items at offset 0, the `cds_ft_metadata_alloc` array right after the range
   header at `cds_ft_page_size` (fractal-trie-alloc.c:26-38).  A different cache
   line, not a different arena.
3. **"one EXTRA metadata load per level" is a `rank_stats`-OFF statement**, and
   was written unconditionally.  With rank_stats ON,
   `ft_flip_txn_record_count_parent` (ft-mutation-helpers.h:9105) already walks
   every ancestor to the root touching those same `cds_ft_metadata` lines
   (`nr_keys` and `state` share the 64 B alloc).  rank_stats is OFF by default
   (ft-lifecycle.h:126), so the claim holds in the DEFAULT config only.
4. **"there is no cheaper home" — NOT ESTABLISHED**, and two of its three
   evidence legs are wrong.  `struct cds_ft_inode` is a bare array in C, but
   popcount tiers carry an in-body header the descent loads for dispatch, and
   some tiers have slack ON that line (scan_32_8: 4 pad bytes at node+12;
   max_child=16: 48 B trailing slack — ft-lookup-node.h:285, :291).  And the low
   nibble is full only for INTERNAL pointers: a compressed pointer is `0b010`
   at 16-byte alignment (bits 2-3 free) and an external pointer is 8-byte
   aligned (bit 2 free) — internal.h:150-161.

☞ **THE UNSURVEYED CANDIDATE, and it is the one that matters:** an in-band
marker on the junction's INCOMING SLOT — a word the descent already dispatches
on per level, with the `0xF` flip-proxy encoding as the precedent for parking a
marker there.  That would give junction-scoped freeze at ZERO extra fast-path
loads, which is what (D) claimed all along.  ☠ Its own objection, which this
section must not skip: readers dispatch on that SAME word, so an in-band marker
lands on the READER path and collides with (D)'s obligation (4), "readers never
check the state: wait-freedom untouched".  A flip proxy costs readers a resolve;
a freeze that outlives a grace period is not transient the way a proxy is.
Unproven either way — **survey it before any B-vs-D decision.**

⇒ **THE CONCLUSION, CORRECTED.**  "The cost comparison inverts" does NOT follow
as stated.  What is established: (D) implemented as a PER-LEVEL METADATA CHECK
costs a load per level in the default rank_stats-off config, while (B)'s root
placement costs ONE trie-level load — `ft_move_active` (internal.h:2573), one
stable load behind `caa_likely`, though note it is consulted on READER paths
only today (ft-lookup.h:136-294), so (B)'s mutation-side gate is a NEW check,
exactly as §2 already says.  What is NOT established is that (D) REQUIRES a
per-level check at all.  So the B-vs-D default stays open, and the missing input
is the encoding survey above, not a measurement.

☞ **IF (D) IS CHOSEN, THE FREEZE STILL WANTS ITS OWN WORD, NOT AN `FT_STATE_*`
BIT.**  State-word bits 11-18 and 20+ are free (bit 19 = `FT_STATE_LOCK`, pinned
literally), but that word is MCAS-transacted and NODE-owned while the freeze is
JUNCTION-owned: a bit there lands inside every recorded edge's expected-old
capture on that word, must join LOCK/TOMBSTONE in the §4.B guard's masked set or
become spurious aborts, and inherits `FT_STATE_INPLACE_WAIT_MASK` semantics
(internal.h:1000-1046).  §8.3's "one word cannot be owned by two locks" is the
argument; the `parent_slot_offset` split out of that word is the precedent.

### G5.1 — THE ENCODING SURVEY, and ☠ THE CANDIDATE G5 NEVER LISTED: the LOCAL MODE SWAP (now (E))

Two results.  The survey closes the per-node-encoding question; the second
result makes most of it moot, and it was already written down elsewhere —
`mcas-multiwriter-readiness.md` §5.2/§5.3 and `step4-concurrent-engine-plan.md`
§3.5/§4.4/§6.3.  **G5's candidate list above is incomplete: it never mentions
transitioning a SHARED SUBTREE TO EXCLUSIVE — a mode swap done LOCALLY.**

#### ☠ THE CANDIDATE THAT WAS MISSING — now (E) above

`mcas-multiwriter-readiness.md` §5.2/§5.3 and `step4-concurrent-engine-plan.md`
§3.5/§4.4/§6.3 work out the LOCAL MODE SWAP — shared subtree -> exclusive — and
that doc has carried it as its own **open question 2** since before this plan
was written.  G5's list here never folded it in, so the encoding survey below
was optimising a mechanism a neighbouring doc had already undercut.  What it
contributes, and where its framing had to be replaced:

* ☠ **THE HAZARD (A)/(B)/(D) CANNOT ANSWER**, and it stands whatever the
  mechanism: *"A writer deep in the subtree does not touch the boundary edge, so
  freezing only the boundary does not evict it."*  (D) freezes exactly the
  boundary.  Its one-GP drain is sound only if every in-flight mutation
  COMPLETES or RE-DESCENDS through the junction — and (D)'s own obligations (1)
  and (3) (the handle path; retry loops with no QSBR quiescent window) are the
  two ways that fails.  §5.3 calls quiescing the writers ALREADY INSIDE **the
  irreducible Tier-3 cost**, not a checklist item.
* ☠ **BUT ITS "SEALED EPOCH" FRAMING IS SUPERSEDED** (Mathieu, 2026-08-28).  A
  seal is an EXCLUSION primitive — it evicts, and eviction here means
  abort-and-retry, which is MW-flavoured arbitration and therefore BACKWARDS
  against this transition's direction of travel
  ([[feedback_direction_is_toward_fine_locking_away_from_mw]]).  §5.3 was
  written when the target was genuine exclusivity (writers ABSENT); under DLM
  the question is not how to make writers absent but **at what granularity two
  writers arbitrate**.  (E) above is that answer: widen the point op's lock set
  instead of evicting it, and let the ordinary acquire do the work.
* ★ Both the hazard and (E)'s answer reach the case the boundary cannot,
  because a point op's ANCESTOR WALK starts from where the op actually is —
  including a handle-based mutation that never descended at all, which is (D)'s
  obligation (1) dissolved rather than discharged.

#### ☑ THE ENCODING SURVEY (and why (E) makes most of it moot)

Asked: can (D)'s per-level check live somewhere the descent already loads?
**Answer: no — the only uniform per-node home is metadata.**

* ☠ **In-body slack: DEAD.**  Five of the seven 64-bit tiers are EXACTLY full
  (header + max_child*8 == node size: 128, 256, 512, 1024, and pigeon's 2048).
  Only tier 1 (scan_32_8) has 4 spare bytes.  Stealing a pointer slot is not
  available on PIGEON, whose 256 slots ARE the 256 byte values with no header
  at all — a flag there means order 11 -> 12, i.e. DOUBLING the densest
  near-root nodes, which is where junctions live.
* ☠ **Spare tag bits: DEAD where it matters.**  An INTERNAL pointer spends all
  four low bits (1 internal + 3 type index, and all 8 type values are used:
  0-6 real + 7 = NULL).  Compressed (`0b010`, 16-byte aligned) and external
  (8-byte aligned) do have spare bits — but a junction is an internal node.
* ☑ **Metadata word: the only uniform home**, costing one load per level on
  non-skip levels in the default rank_stats-off config (free on skip-encoded
  levels, which already load metadata — G5.0; and free with rank_stats on).
* ☞ **The two-tier gate that makes it cheap**, and the shape (E) generalizes:
  a trie-level "is ANY freeze active?" word, checked `caa_likely`-style, with
  the per-level metadata check reached ONLY inside an active window.  Steady
  state then costs exactly (B)'s one trie-level load.  ★ Precedent in-tree:
  `ft_move_active` (internal.h:2573) is literally this — "with no move in
  flight the reader takes the FAST path".
* ⇒ **But (E) asks the question differently and better**: check at the writer's
  COMMIT/RETRY point rather than on the descent, and the per-node encoding
  question does not arise at all.  ☞ The survey's real conclusion is therefore
  that **the descent is the wrong place to look**, which is also §5.3's point.

#### ☞ WHAT THIS DOES TO THE FORK

B-vs-D was the wrong axis to decide first.  The prior question is **(E) vs the
boundary mechanisms**: whether the bulk op coordinates by making point ops
WIDEN their lock set (E), or by a freeze the descent reads ((A)/(B)/(D)), which
§5.3 says cannot reach an interior writer.  ☐ OPEN, and it needs Mathieu — but
note (E) needs no new primitive, no per-node encoding, and no engine change,
which none of the others can say.

### G5.2 — ☠ WIDENED IS NOT EXCLUSIVE: the "plain stores" prize is HALF of what §5.2 grants

An earlier revision of G5.1 wrote that a locally-exclusive subtree pays "NO
TRANSACTION AT ALL" on its interior.  ☠ **That overstates it for a subtree held
by (E) — or by any of (A)/(B)/(D) — and the distinction is why this is a design
rather than a copy of detach.**

§5.2 grants vacuity to **`ft->exclusive`**, a trie with neither concurrent
writers NOR concurrent readers, and its two invariants are SEPARABLE:
* **Invariant 2 (writer-vs-writer)** — dies.  With every op that could commit
  under J taking J, and J held, no peer writer can commit into the subtree:
  interior edges need no MCAS, no descriptor, no expected-old arbitration.
  **This is the expensive half, and it is genuinely won.**
* **Invariant 1 (reader-visibility)** — **SURVIVES**.  The subtree is still
  LINKED, so new readers keep arriving, and readers check nothing (their
  wait-freedom is untouched, which is a requirement, not an oversight).
  Interior edges therefore still need release publication and DEFERRED frees.

So the honest statement is **writer-quiesced, not exclusive**: interior work
becomes plain RCU publication instead of transacted multi-writer arbitration —
a large win, and strictly better than (D)'s "SW content records" — but NOT
§5.2's free-at-any-size Tier-1.

☠ **And the tempting fix is the one this project already rejected.**  Full
exclusivity needs the reader drain too — UNLINK, GP, work, relink — which is
detach's shape and exactly what [[project_ft_staged_rekey_writer_deleted]] was
deleted for: a tmp-trie HIDES LIVE KEYS for a grace period per move.  Unlinking
a populated subtree makes its keys vanish from lookups for the whole window.
⇒ the subtree must stay LINKED, and the design can claim only the writer half.

### G5.3 — (F) THE DIRECTION: the BULK WINDOW'S AUTHORITY IS A TRIE-LIFETIME OBJECT, and the flags are DERIVED STATE — ☠ THE PROTOCOL BELOW IS REFUTED AS WRITTEN (G5.4)

☠☠ **READ G5.4 FIRST.**  The REFRAME survives and is the useful part; the
five-step protocol as written below does NOT — an adversarial review refuted it
on three independently verified grounds, including one that is fatal by the
bulk ops' OWN code.  The steps are kept because the refutation is only legible
against them.

**Mathieu, 2026-08-28.**  (E)'s granularity insight is kept whole — bulk/point
coordination is a LOCK-SET WIDENING, not an exclusion — but the exclusion
AUTHORITY moves off the junction node and onto the trie-lifetime FT-wide
`writer_lock`.  Everything that killed (E)'s variants was a consequence of the
authority being a NODE.

**PROTOCOL.**  1 the bulk op takes `ft->writer_lock` (REUSED — not a second
lock; on a FINE trie only bulk ops take it, coarse tries already take it for
every op, so there is one lock and one ordering).  2 under it, set a BULK-OP
FLAG on every node of the op's UPCOMING LOCK SET.  3 `synchronize_rcu` — the
mode flip — **while holding `writer_lock`**.  4 edit, acquiring node locks
normally.  5 clear the flags, release.  A point op's UPWARD WALK reads the
flags and ADDS each flagged node's lock to its own set (the walk already loads
`meta->parent_word`, so the check is free where it is read — G5.1's encoding
result is unchanged), then presents ONE whole set (`ft-mutation-helpers.h:975`).

☠☠ **TWO CONSTRAINTS THE WORD "REUSE" HIDES — get these wrong and (F) wedges.**
`ft_writer_lock_scope_enter` returns at `internal.h:2532` (`if (ft->lock_fine)`)
BEFORE it ever takes the lock, so **on a FINE trie NOBODY takes `writer_lock`
today** — not even bulk ops.  (F) therefore does not merely reuse an existing
acquisition, it ADDS one on the fine path, and:
1. **That new fine-path acquisition MUST use the OFFLINE park**
   (`ft_writer_lock_park`), NOT the scope-entry wait.  Since 2026-08-28 the
   scope-entry wait is `ft_writer_lock_take`, which stays ONLINE deliberately
   (it must not quiesce a point-op caller's read section — see the two-waits
   comment at `internal.h`).  A bulk op waiting ONLINE while the holder sits in
   step 3's `synchronize_rcu` is exactly the non-quiescent reader that GP waits
   for: **deadlock**, and it is 781e0b9a's measured group wedge.  Offline is
   legal here precisely because bulk entries forbid a read-section caller.
2. **(F) IS A FINE-TRIE MECHANISM.**  On COARSE the flags are pointless (every
   writer already serializes on `writer_lock`) and step 3 is UNSAFE (point ops
   wait at the online scope entry, so holding across the flip GP wedges them).
   Coarse keeps its existing discipline: DROP the lock across every GP via
   `ft_writer_lock_gp_wait`.  ⇒ the two paths must not share one acquisition
   helper by accident.

★★ **WHY THE GP MAY BE HELD HERE AND NEVER COULD BE HELD ON J.**  The
grace-period rule (`fractal-trie-internal.h:1806`) forbids waiting on a GP while
holding a lock whose waiters are RCU-ONLINE non-quiescent readers.  On
`writer_lock` (fine trie) **every waiter is another BULK op, and every bulk
entry FORBIDS a read-section caller** — `fractal-trie.h:1715` ("Do NOT call
these operations from within an RCU read-side critical section: they can block
internally on `synchronize_rcu()`") for graft/detach/merge/swap, `:3024` for
rekey.  So its waiters may park OFFLINE and cannot block the flip GP.  That is
exactly the property NODE locks lack: point ops may NOT quiesce
(`rcu-txn.h:688`, and QSBR cannot even detect a caller's section), which is why
holding J across a GP was a deadlock and no park could fix it.

**WHAT IT BUYS OVER (E), beyond being implementable:**
* **The widening is EXACT.**  Flagging the actual upcoming lock set, not a
  subtree, means only ops that TOUCH a flagged node widen.  ⇒ (E)'s open cost
  "a near-root J serializes broadly" is RETIRED.
* **Bulk-vs-bulk falls out** of the lock — no counter, no owner field, no new
  mechanism (G5.2's open question closed).
* **No retire/COW hazard.**  The authority is a trie-lifetime singleton that
  cannot die; the per-node flags are DERIVED STATE re-established under the
  lock.  A flagged node retired by a peer is a RE-PLAN, not a silent exclusion
  gap — which is precisely what killed the (E) variants (below).

**☠ TWO RULES THAT BIND, both discovered while deciding this:**
1. **FLAG ADDITIONS COST A FLIP GP; REMOVALS DO NOT.**  A flag added mid-window
   creates fresh non-observers who may commit under that node without taking
   it.  A flag dropped only makes a point op conservative.  ⇒ flag a SUPERSET
   up front; a narrowing re-plan is free, a growing one pays another GP.
2. **THE SEAM RULE BECOMES LOAD-BEARING FOR POINT-OP LIVENESS.**  The flip GP
   is NOT the only one: bulk edits carry their own reader drains
   (`ft-detach.h:219/511/575`, `ft-graft.h:2960/4011`, `ft-rekey.h:4510/5035`,
   all `!exclusive`-gated, so LIVE on the fine non-exclusive trie this
   targets).  Those stay safe only while NO NODE LOCK is held across them —
   `internal.h:1811`, "the GP always sits at a seam BETWEEN two distinct
   commits".  Under (F) a point op spinning on a widened set IS the reader such
   a GP would wait for.  ☑☑ **ARMED @`d48b5c94`, AND IT HOLDS** — see G5.8.

**☠☠ WHAT WAS REFUTED GETTING HERE — do not re-tread:**
* **The Phase-D park prerequisite is DISSOLVED, not satisfied.**  §2 (E) recorded
  "the mark must route waiters to a PARK".  It was never implementable: the ops
  that had to park are `cds_ft_insert`/`cds_ft_remove`, whose callers hold read
  sections and reader-derived references across the call.  Under (F) point ops
  never wait on `writer_lock` at all.
* **Releasing the junction across the drain GP** (an attempt to obey the seam
  rule with J as the authority) is REFUTED: in the gap a point op legitimately
  holding J can RETIRE it (one-way TOMBSTONE) or COW-replace it, and the mark —
  living in J's metadata — dies with it, silently ending the exclusion
  mid-edit.  A trie-lifetime authority is the fix, not a better gap.
* ☞ The GRACE-PERIOD RULE was independently reaffirmed this day on the coarse
  path: the FT-wide lock's SCOPE-ENTRY wait was made ONLINE
  (`ft_writer_lock_take`) so it stops quiescing the caller's read section, and
  that is a DELAY rather than a wedge *only* because the holder never waits on a
  GP while holding.  Measured cost of an online wait, 16 writers on one coarse
  trie: hold p50 >= 16us / p99 >= 66us; wait p50 >= 524us / p99 >= 1.05ms at
  ~75% lock utilization.  The `gp_wait` RE-ACQUIRE keeps its offline park
  (`ft_writer_lock_park`), which is where 781e0b9a's measured wedge actually was.

☐ **STILL OPEN:** the tier-1 "is any bulk op live?" trie word (the
`ft_move_active` shape) so the steady state pays nothing; whether the flag rides
an existing metadata word or needs its own; and the seam-rule arm above.

### G5.4 — ☠☠ (F)'s PROTOCOL IS REFUTED; the REFRAME SURVIVES (adversarial review, 2026-08-28)

Three grounds, each verified in-tree, not taken on the reviewer's word.

1. ☠☠ **FATAL, AND IT IS THE BULK OP'S OWN CODE.**  `ft_writer_lock_gp_wait`
   UNCONDITIONALLY DROPS `writer_lock` whenever `ft_wlock_held` is set
   (`internal.h:2631-2686`: `held = ft_wlock_held; if (held) { ...
   cds_fair_mutex_unlock ... }`), and the bulk bodies call it at SEVEN live
   `!exclusive`-gated sites (`ft-detach.h:219/511/575`,
   `ft-graft.h:2960/4011`, `ft-rekey.h:4510/5035`).  So (F)'s "lock held
   throughout steps 1-5" is TORN OPEN by the op itself at every mid-edit
   drain.  A peer bulk op then enters the gap, flags an overlapping set, and
   its step-5 CLEAR erases the first op's flag on any shared node — a SINGLE
   BIT cannot tell the two windows apart — so point ops stop widening while
   the first op is still editing: **a silent exclusion gap**.  ⇒ "bulk-vs-bulk
   falls out of the lock" is FALSE at every drain seam.
2. ☠ **IT CONTRADICTS TWO SHIPPED API PROMISES, both for the DEFAULT (fine)
   mode.**  `fractal-trie.h:1703` — *"No writer mutex needed under
   CDS_FT_WRITER_LOCK_FINE (the default): concurrent grafts into one live
   destination are supported, and serializing them here would discard exactly
   the parallelism fine-grained locking exists to provide."*  (F) serializes
   bulk ops on `writer_lock`, which is that discard.  And `:3023` — *"A burst
   of concurrent moves pays about ONE grace period between them, not one
   each"* — is delivered by the MOVE GATE's piggyback
   (`internal.h:2749-2756`); under (F) each window takes its OWN flip GP under
   an exclusive lock, so a burst pays one GP EACH.
3. ☠ **THE FLAG STORE HAS NO SOUND HOME AS SPECIFIED.**  If the flag rides the
   state word / `parent_word`, those are the plan's own SW-park targets — a
   point op holding a flagged node's lock SW-parks that word with a plain
   store and no expected-old, silently CLOBBERING the flag; an unintentional
   removal is an ADDITION hazard with no GP behind it (bits 0-19 already
   assigned, `internal.h:938-1009`).  If it takes its own word, G5.1's "the
   check is FREE where the walk already loads it" is withdrawn — an extra load
   per walked node, on every point op, forever.

☞ **ALSO REFUTED:** "no retire/COW hazard" — moving the AUTHORITY to a
singleton did not move the MECHANISM.  A COW of a flagged member publishes the
replacement WITHOUT the flag (no propagation exists), and until the bulk op next
touches that dead word, point ops commit under the replacement un-widened.
☞ "widening is EXACT" is steady-state only: a window whose set includes a
near-root node still funnels every point op beneath it onto ONE ABORTING
try-lock for a window containing 1 flip GP + up to 7 drain GPs.

☑ **WHAT SURVIVES, and it is the part worth keeping:** the REFRAME — the
window's authority belongs on a TRIE-LIFETIME object rather than on a mortal
node; widening, not exclusion; and the **Phase-D park prerequisite is genuinely
DISSOLVED for point ops** (on a FINE trie every `CDS_FT_SCOPED_WRITER`
early-outs at `internal.h:2532`, so point ops never wait on `writer_lock` at
all).  Point-op whole-set presentation stays deadlock-free (all-or-none sorted
acquire), so ADDING widening creates no lock-only cycle.  The flip-GP
observer/non-observer argument holds as a SCHEMA, provided every point path
re-reads the flag inside each attempt.

### G5.5 — ☑ THE FIRST STEP (Mathieu, 2026-08-28): the MOVE GATE, and while it is set POINT OPS TAKE EVERY ANCESTOR UP TO THE ROOT

Deliberately the COARSEST point on the same axis: **no per-node flags at all.**
The bulk op enters the existing move gate; while `move_active` is set, a point
op ADDS every ancestor's lock, up to and including the root, to its own set.
Refine later only if measurement demands it.

★ **IT SIDESTEPS ALL THREE OF G5.4's GROUNDS, BY SUBTRACTION** — which is why
it is the right first step rather than a retreat:
* **Ground 3 (the flag has no sound home) — VOID.**  There is no flag: the
  tier-1 word IS `move_active`, one trie-level word that already exists.
* **Ground 1 (a peer's clear erases an overlapping window) — VOID.**  There is
  no per-node state to erase, and the gate is REFCOUNTED (`move_gate_nr`), so
  concurrent bulk windows compose by construction.
* **Ground 2 (serialization + a GP per op) — VOID.**  The gate does not
  serialize movers, and its 0->1 owner owns the GP while later arrivals
  PIGGYBACK it: "a burst of moves therefore costs ~one GP, not one per move"
  (`internal.h:2755`).  Both shipped promises survive.

**WHY EVERY ANCESTOR AND NOT JUST THE ROOT.**  The bulk op holds its own set
(a junction J and its frontier), which need not include the root; a point op
holding only the root would then share NO WORD with it.  Taking the whole chain
guarantees overlap wherever the bulk op is working.  ☠ It is an **ADD, NEVER A
SUBSTITUTE** — the leaf lock is never surrendered, so point-vs-point exclusion
is unchanged and the observer/non-observer split (separated by the gate's GP)
cannot leave two ops on one leaf sharing no word.

**THE THREE PREREQUISITES ARE ALREADY IN THE TREE:**
* the whole set is presented AT ONCE and the engine sorts by SLOT ADDRESS, so a
  depth-sized set is deadlock-free with no hold-and-wait
  (`ft-mutation-helpers.h:975`);
* the txn lock registry GROWS (@`758f432d`), so a set that grows by DEPTH is
  affordable — this was a hard prerequisite and it is landed;
* the gate itself, with its offline piggyback wait (`internal.h:2820`).

☐ **THE COST, STATED PLAINLY:** while ANY bulk op is live, every point op takes
the ROOT, so all point ops serialize trie-wide for the window.  That is the
honest price of the coarsest step — it is candidate **(B)**, the root
placement, reached through the gate instead of through a new mode word.

☐ **TWO THINGS TO SETTLE BEFORE IMPLEMENTING:**
1. **The gate is entered ONLY by rekey today** (`ft-rekey.h:3667`, `:5991`);
   detach / graft / merge do NOT enter it.  Extending it to the whole bulk
   family is small but real work.
2. ☑ **DECIDED (Mathieu, 2026-08-28): A SEPARATE WRITER STATE.**  `move_active`
   is READER-facing (`internal.h:1737`: nonzero => readers take the COHERENT
   path), so reusing it would put READERS on the coherent path for every
   detach/graft — a cost they do not pay today and that the widening does not
   need.  The writer side gets **its own word beside it**.
   ★ That word IS the tier-1 "is any bulk op live?" gate that (E)/(F) both
   wanted — one trie-level word, `caa_likely` false, zero steady-state cost —
   so this closes that open item too.
   ☞ **ONE GATE, ONE GP, TWO WORDS, TWO REFCOUNTS.**  The gate's machinery
   (`move_gate_lock` / `_cond` / `_nr` / `_gp`, `internal.h:2777`) is generic and
   should be shared, so a rekey burst and a detach burst still amortize into ONE
   grace period; what must NOT be shared is the refcount that decides when each
   WORD clears.  A rekey publishes BOTH words (it needs reader coherence); a
   detach / graft / merge publishes ONLY the writer word.  ☠ A single
   `move_gate_nr` would leave `move_active` set by a detach — the exact cost
   this decision exists to avoid — or clear it while a rekey is still live,
   which is a correctness bug, not a cost.  Two counters, one gate.

☠☠ **AND THE ONE THING THIS STILL DOES NOT SOLVE — the same linchpin under
EVERY variant.**  The bulk op's mid-edit drain GPs (`ft_writer_lock_gp_wait`,
7 live sites) run while point ops may be spinning on a word the bulk op holds;
those point ops CANNOT quiesce, so such a GP would never complete.  What makes
it safe is the SEAM RULE — no NODE lock is held across a GP,
`internal.h:1811`.  ☑☑ **NO LONGER AN ASSUMPTION — ARMED @`d48b5c94` and
measured CLEAN with coverage (G5.8).**  It was the prerequisite shared by
(B)/(E)/(F) alike, and it is discharged.

### G5.6 — ☞ EXPLORING: the tier-2 refinement, a PER-NODE BULK REFCOUNT (Mathieu, 2026-08-28)

G5.5's widening is trie-wide (every ancestor to the root) while any bulk op is
live.  The refinement: give each NODE a **refcount of the live bulk windows
covering it**, and have the upward walk add a node's lock only where that count
is nonzero.  ★ It is exactly what G5.4's ground 1 demanded — *"shared flags
would have to be COUNTERS or owner-stamped, not bits"* — so it is the
PRINCIPLED fix for overlapping windows, not a second guess at (F)'s bit.

☠☠ **THE PARAGRAPH BELOW IS HALF WRONG — see G5.7.  The sizeof/padding half
holds; the CACHE-LINE half is REFUTED for 100% of nodes.**  Kept as written
because the correction is only legible against it.

☑ **IT COSTS NOTHING TO CARRY — MEASURED, not estimated (2026-08-28).**
`sizeof(struct cds_ft_metadata)` is **48 bytes**, with the packed tail word
(`alloc_index` + `incoming_byte`) at offset 40..43 and **4 BYTES OF PADDING at
44..47** (the struct rounds to 8 for `parent_word`'s alignment).  A
`uint32_t bulk_refcount` at 44 leaves the struct **still 48 bytes**:
* **zero memory cost per node** — it lands in existing padding;
* **offset 44 < 64, the SAME cache line as `parent_word` (offset 0)** — so the
  tier-2 check is free on the walk that already loads `parent_word`.
  ★ This RESCUES G5.1's "free where it is read" claim, which G5.4 ground 3 had
  forced us to withdraw — with the honest correction that it is the same LINE,
  not the same word.
  ☐ Verified for the non-FAR, non-HOLD_TRACE layout (the shipping one);
  `FT_FAR_METADATA` keeps the same 40..43 tail, so it should be identical —
  confirm before relying on it.

☠ **IT MUST HAVE ITS OWN WORD, AND THE TREE ALREADY SETTLED WHY.**  Not in
`state`: §8.3 split `parent_slot_offset` OUT of `state` precisely because *"a
word cannot be owned by two locks"*.  The bulk refcount has that exact problem —
it is written by a bulk op that does NOT hold the node's lock, while `state` is
node-lock-owned and is the SW-PARK TARGET, so a park's plain store would clobber
it.  Own word, `uatomic_add`/`uatomic_sub`.  ⇒ **G5.4's ground 3 is answered**,
by the tree's own established rule rather than a new argument.

★ **KEEP THE PER-FT WRITER STATE AS TIER 1 — this REFINES G5.5, it does not
replace it.**  If the refcount were the only signal, every point op would have
to walk its ancestors checking counts on EVERY operation; at per-node spacing a
point op anchors on ITSELF and does not climb today, so that is a NEW O(depth)
cost paid always.  With the per-FT word as a `caa_likely`-false tier 1, the walk
happens only while a window is live.  ⇒ this is (E)'s two-tier structure at
last, with a tier 2 that survives review.

☑ **WHAT IT BUYS:** the widening becomes EXACT — a point op takes only the
COVERED ancestors — so a deep bulk op stops serializing the whole trie.  That
retires G5.5's accepted cost, which is the reason to want it.

☠☠ **THE NEW HAZARD, AND IT IS THE REAL RISK: EXACT TRANSFER.**  A BIT can be
re-set conservatively; a COUNT must MOVE EXACTLY across a COW / re-parent /
retire.  Leak it and the node stays "covered" forever (point ops widen onto it
for the rest of the trie's life); lose it and the exclusion silently ends
mid-window.  This is (F)'s ground-5 COW hazard, made STRICTER by counting rather
than softer.  ⇒ it needs an explicit transfer rule at every republish site AND
an arm (leak / underflow detector) — ☐ neither exists yet, and this is where
this refinement will actually be won or lost.

☞ Unchanged from G5.5: increments land BEFORE the flip GP; a mid-window
increment needs ANOTHER flip GP (additions cost one, removals are free);
ADD-never-substitute; and the SEAM RULE remains the shared liveness linchpin.

### G5.7 — ☠☠ G5.6 IS REFUTED (adversarial review, 2026-08-28); the COUNTER PRINCIPLE survives, the WORD does not

Seven of eight claims fell.  The two that matter most are re-verified here
in-tree, not taken on the reviewer's word.

1. ☠☠ **THE "FREE CHECK" IS WRONG FOR EVERY NODE — I ASSERTED IT, AND THE
   ARENA REFUTES IT.**  Metadata is not a bare struct: it is embedded in
   `struct cds_ft_metadata_alloc` (`rcu_head` at 0, `.metadata` at **+16**,
   sizeof **64**, `internal.h:3143`) inside the flexible array
   `cds_ft_alloc_range::metadata[]`, which starts at **+72** (`internal.h:3186`).
   Compiled probe against the real headers: every element's metadata sits at
   **mod-64 offset 24**, so `parent_word` (metadata+0) and the proposed word at
   metadata+44 are **on DIFFERENT CACHE LINES for 100% of nodes** — one extra
   line per walked level, not a free field.
   ☠ And worse than neutral: node N's refcount line CARRIES NODE N+1's HOT HALF
   (`parent_word`..`state`), so a bulk op's `uatomic_add`/`sub` FALSE-SHARES
   with a neighbouring node's read path and lock word.
   ☑ The sizeof half stands: 48 bytes with padding at 44..47, in near, FAR
   (the 64-bit DEFAULT) and HOLD_TRACE alike.
2. ☠☠ **CLAIM 2 AND CLAIM 6 CONTRADICT EACH OTHER, INHERENTLY.**  The word must
   live OUTSIDE engine-owned state to escape the SW-park clobber (that is what
   makes G5.4 ground 3 answerable) — but a word outside the engine CANNOT RIDE
   THE FLIP TXN as a recorded edge, so its transfer can never be made atomic
   with the republish it must accompany.  A peer window's `uatomic_add` on the
   OLD word lands between a republisher's load and its flip: the increment is
   lost (that window silently uncovered on the new node) and the peer's exit
   decrement then targets a freed slot.  ⇒ **not an implementation gap — the
   two properties the design needs are mutually exclusive as specified.**

☠ **ALSO REFUTED:** *the ancestor walk cannot be enumerated safely* —
`parent_word` is transiently NULL during detach/graft_swap re-homes
(`internal.h:1051`, plain NULL store `ft-graft.h:294`) and **NULL reads as THE
ROOT to a walker** (`ft-mutation-helpers.h:6605`), so a point op that descended
before the clear and climbs after silently TRUNCATES its ancestor set, misses
covered ancestors, and commits sharing no word with the bulk op — a silent
exclusion gap.  *Increments cannot all land before the GP* — bulk lock sets are
DISCOVERED during the edit, not plannable (merge registers each overlap node as
it walks, 254 measured; `ft-mutation-helpers.h:992`), and each growth owes
another flip GP with nowhere legal to pay it (mid-edit it violates the seam
rule; after dropping everything it is a full re-plan → livelock under churn).
*Decrements are not free* — nothing PINS a covered member (a refcount is not a
lock), so a point op may retire or COW it; after the GP the slot is freelisted
and re-allocated memset-0, and the window-exit decrement UNDERFLOWS AN
UNRELATED NODE.  *Liveness is unbudgeted* — D.1's measured tail is exactly this
mechanism (an aborting queueless acquire re-sampling a bulk-held word, 100
re-descends per episode) applied to a strictly LARGER population for whole
windows; no D-section bounds it.

☑ **WHAT SURVIVES, and it is not nothing:** the PRINCIPLE that overlapping
windows need COUNTERS rather than bits; the sizeof/padding headroom; the
tier-1 word being genuinely cheap IN ITSELF (read-mostly, written only at gate
enter/exit); own-word immunity to IN-PLACE clobber; and tier-2 being strictly
narrower than G5.5.  ☠ But note claim 3's premise died too: point ops do NOT
climb today (a member anchors on ITSELF at per-node spacing,
`ft-mutation-helpers.h:3088`, `:3323`), so the walk is NEW code — and on a
bulk-heavy workload tier 1 is ~always set, making that walk the steady state.

☞ **THE ONE UNEXPLORED EXIT, recorded as a QUESTION, not a proposal:** the
contradiction in (2) is between "outside the engine" and "atomic with the
republish".  A count that is itself a TRANSACTED SLOT — its own word, but
engine-owned, so a republish carries it as a recorded edge — would satisfy both
halves at once.  ☐ UNVETTED: it re-opens the SW-park question the own-word
choice was made to escape, and nobody has checked whether a counter can be a
flip-txn edge at all.  Do not build on it before it is skepticked.

### G5.8 — ☑☑ THE SEAM RULE IS ARMED, AND IT HOLDS (@`d48b5c94`)

The prerequisite every G5 variant shared is discharged.  The arm runs at the
two sites a writer waits for a GP — `ft_writer_lock_gp_wait` (which all seven
bulk mid-edit drains route through) and `ft_move_gate_enter` — and asserts that
this thread holds NO node lock across it.  It changes no behaviour.

★ **EXACT, not over-reporting, which is what makes its zeros worth anything.**
The hold ledger over-reports by construction (a commit that CONSUMED a fence
leaves its entry behind), so an entry alone would fire on words a peer has
since legitimately taken.  Two facts together are exact: the entry's word still
carries `FT_STATE_LOCK` **and** the member's `dbg_owner_tid` is still OURS.
Both already existed — no new state.

    RED CONTROL (-DFT_RED_SEAM, checked right after an acquire files
    its locks, where they provably ARE held):
        ft_unit      1,360,090 checks   2,732,148 violations   ✅ live
    GREEN:
        ft_unit            728 checks           0 violations
        ft_inv MW    2,267,316 checks           0 violations

☠ The CALL counter is the half that makes the zero legible — an earlier version
of this probe reported 0 on three tests it had entered 0, 0 and 1 times.
Gate: identical per leg to the pre-arm run, zero aborts; the `holdtrace` config
is clean at ALL THREE lock spacings, which is the stronger reading since coarse
anchoring holds node locks more widely than per-node does.

⇒ The bulk op's mid-edit drains do sit at commit seams, as the design assumed.
☐ What the arm does NOT do: it observes the CURRENT tree.  It is a regression
detector for the moment a G5 mechanism starts holding a widened lock across one
of those drains — which is precisely when it will matter.

★ **WHERE (F) POINTED, from G5.4 ground 2:** the MOVE GATE is already the
"flip a mode with ONE amortized GP" primitive this needs — it publishes
`move_active`, waits one GP, lets a burst PIGGYBACK, waits OFFLINE
(`internal.h:2749-2756`, `:2820`), and does NOT serialize the movers.  A flip
carried by a gate rather than by an exclusive lock preserves both promises
ground 2 cites.  ☐ It does not by itself answer the overlapping-window problem
of ground 1: shared flags would have to be COUNTERS or owner-stamped, not bits.
☐ UNDECIDED — this is a direction, not a third protocol.

### G5.9 — ☠☠ THE BULK WINDOW IS MEASURED, AND IT HAS NO DURATION: DUTY ~99.9% (@`8034fe5f`)

G5.5's accepted cost — "point ops serialize trie-wide FOR THE WINDOW" — reads
as a bounded burst.  **It is not bounded.**  `-DFT_DEBUG_BULK_WINDOW` measures
the UNION window (the interval `move_gate_nr > 0`), FINE trie, `-O2 -DNDEBUG`,
8 movers + 6 insert/remove + 8 readers, 2.0 s runs, 3 reps each:

    arm                       moves/2s   body p50   body mean  WINDOWS   DUTY
    contended (ONE junction)   527-584   >=2-4 ms    28-31 ms      2    99.91-99.92%
    fine (disjoint bands)    15980-17075 >=524 us  0.94-1.00 ms    2    99.76-99.92%

**TWO windows in a two-second run, in all six runs.**  The gate brackets the
WHOLE rekey op (`ft-rekey.h:5991..6040`), so a mover in a loop is inside it
essentially always; with 8 movers the refcount never returns to 0.  Wherever
bulk ops OVERLAP IN TIME, the window IS the workload, and under G5.5 point ops
would widen to the root permanently.

★★ **AND THE CAUSE IS THE PROPERTY THE DESIGN LEANED ON.**  "The gate amortizes
ONE GP across a burst" (G5.5, G5.8) is TRUE and then some: only **7** movers out
of up to **17,076** enters ever waited on the owner's GP, and the GP is
0.05-0.09% of open time.  But AMORTIZING THE GP *IS* KEEPING THE GATE OPEN.  The
property that makes the gate cheap for READERS is exactly what makes it maximally
expensive for G5.5's POINT OPS — one fact read from two ends.

☠ **"duty = duration x rate" IS REFUTED**, and that was the reporting plan an
adversarial review killed before the run.  Windows COALESCE (movers per window
264-8,538), so duty SATURATES rather than accumulating.  The threshold is
OVERLAP, not rate: strictly serial bulk ops one every T give duty ~ d/T; bulk
ops ever CONCURRENT give duty -> 100%.  At the measured uncontended body
d ~ 1 ms, ONE bulk producer above ~1000 ops/s pins the gate open by itself.

**GP is not the cost; CONTENTION is.**  In-body GP is 0.01% of rekey body time
(1 op in ~16,000).  The same op is ~1 ms disjoint and ~30 ms when 14 writers
share one junction — **30x**.  Shortening grace periods buys nothing here.

**PER-CLASS BODY** (full suite, 120 scenarios, 0 failures) — the window each op
WOULD open once the gate is extended past rekey:

    class          n          mean      in-body GP   ops taking >=1 GP
    graft          312,854    9.7 us    0.00%        0 / 312,854
    graft_swap   1,134,935   11.9 us   80.59%        1,102,270 / 1,134,935
    detach             317    1.83 ms   7.08%        275 / 317
    merge_at       185,438   12.4 us    0.00%        0 / 185,438
    rekey_graft     72,705     915 us   0.24%        2,981 / 72,705
    rekey_merge          —    NOT EXERCISED (configuration miss)

★ This REFUTES the structural prediction that graft/detach carry the family's
LONGEST windows because their bodies hold 4 and 3 `ft_writer_lock_gp_wait`
sites.  **CALL SITES ARE NOT EXECUTED GPs**: graft took ZERO in-body GPs in
312,854 ops, merge_at zero in 185,438.  `graft_swap` takes one in ~97% of ops
but they are CHEAP (80% of an 11.9 us body).  rekey is not the cheapest member
of the family — it is among the most expensive; only detach is longer.

☠ **WHAT THIS DOES NOT PRICE — and it cannot, before the build.**  This is
EXPOSURE only.  G5.5's second risk — point ops widened to the root
spin-abort-retrying on a queueless ABORTING acquire — lives in a regime THAT
DOES NOT EXIST IN THIS BINARY, because point ops do not widen yet.  Duration
does not bound it: a starving lane is already on record (remove's retry lane
does not drain under lock-holder preemption; acquire backoff REFUTED;
escalation rescues a COMMIT, never an ACQUIRE).  ⇒ Liveness must be the FIRST
GATE ON THE G5.5 BUILD, behind its own flag, with per-point-op abort/age
histograms sampled INSIDE the attempt.  ★ And this result makes that risk
WORSE, not better: the widened-acquire regime would not be an occasional window
a point op waits out — it would be the STEADY STATE.

☐ **THE QUESTION FOR MATHIEU, and the plan is NOT changed pending it.**  G5.5
was chosen as the deliberately coarsest point on the axis, to be refined later
ON MEASUREMENT.  This is that measurement, and it says the coarse point costs
~100% point-op serialization under any overlapping bulk load rather than a
bounded burst.  Three ways forward, none taken here:
  1. accept it (bulk-op load in real workloads may simply be sparse and
     NON-OVERLAPPING — the duty cycle is a property of the WORKLOAD's bulk
     concurrency, not of the library);
  2. refine now rather than later — G5.6's tier-2 was refuted on its WORD, not
     on its principle (G5.7), and the counter principle survives;
  3. narrow the WINDOW instead of the lock set — the gate currently brackets
     the whole op, and most of the body is contention, not publication.

☐ LIMITS: WINDOW percentiles are meaningless at n=2 (the finding is "2 windows
cover 99.9% of the span").  Instrument perturbation 1.23x, so body durations are
inflated by about that.  Per-class bodies aggregate a mostly low-contention
suite.  ft_unit's 3 rekey/merge failures are PRE-EXISTING (proven against a
pristine control build), not instrument-induced.

### G5.10 — G5.5's GATE HALF IS BUILT; the WIDENING is BLOCKED on three prerequisites

**LANDED.** The writer-side state is separate from `move_active`, as decided:
ONE gate (`move_gate_lock` / `_cond`), TWO refcounts, and `gate_gp_nr` as a
COUNTER rather than the old boolean -- the grace period runs OUTSIDE the lock,
so a detach publishing the writer word and a rekey then publishing
`move_active` can each own one, and a boolean cannot name two owners.  Gate
entry now covers detach / graft / graft_swap / merge_at; rekey alone publishes
both words.  The offline piggyback, `assert(!urcu_txn_in_fallback())` and the
seam check are carried over unchanged.

**☑ THE PACKED {refcount, min level} WORD (Mathieu, 2026-08-28).**  The word
carries the refcount in its high bits and, in the low 16, the SHALLOWEST level
at which any in-flight bulk op locks.  A point op then widens from that level
DOWN, never above it.
* ★ WHY IT IS SOUND: a bulk op holds a junction J and its frontier and holds
  NOTHING ABOVE J.  A point op's leaf->root chain meets J exactly when the point
  op is inside J's subtree -- the conflict case -- so locks strictly shallower
  than J buy no exclusion.  ★★ AND THIS IS THE POINT: G5.9 measured duty at
  ~99.9%, so "every ancestor to the root" is not an occasional widening but
  PERMANENT trie-wide serialization on the root.  The level word is what stops
  concurrent point ops from all serializing there.
* ONE WORD, ONE LOAD: read as two words a point op could see refcount > 0 with a
  STALE DEEPER level and widen too little -- an exclusion gap.
* ☠ THE MIN IS EXACT, NOT MONOTONE.  A min that only decreased and reset at
  refcount 0 would RATCHET to the shallowest level ever used and stay: G5.9
  measured the refcount essentially never reaching 0 under load.  An exact
  per-level count array under the gate mutex makes it RISE again as deep ops
  drain -- and the mutex already serializes every enter/exit, so no CAS.
* LOWERING the level OWNS A GRACE PERIOD (Mathieu): point ops in flight sampled
  the deeper value and are missing the newly-shallow locks.
* ★ THE LEVEL IS FIXED AT GATE ENTRY, never lowered mid-op.  Lowering mid-op
  would need a GP while the op already holds locks, which would break the SEAM
  RULE armed @`d48b5c94`.  An op can only lower on the way IN, before it holds
  anything.  An op that cannot name its junction passes 0 and degrades exactly
  to G5.5, so ops refine ONE AT A TIME.

**☠☠ THE WIDENING IS NOT BUILT, and must not be built in the obvious shape.**
Three prerequisites, two of them fatal (adversarial review + independent
confirmation):
1. **NO ANCESTOR DATA EXISTS AT THE DEFAULT SPACING.**  The descent's `anchor[]`
   table is never built at `CDS_FT_LOCK_SPACING_PER_NODE`, the shipping default
   (`ft_descent_enter_node` returns early; `anchor_crossed` stays 0 and the
   table is deliberately uninitialised stack).  A widening reading it would take
   garbage or the EMPTY SET -- a SILENT no-op in the default configuration.  The
   `parent_word` up-walk is no fallback: it is refuted in-tree (transiently NULL
   during detach/graft re-homes, and NULL reads as ROOT -> silent truncation).
   ⇒ needs a DEDICATED per-descent ancestor ledger filled at ALL spacings, and
   an acquire with no descent must be REFUSED (-EAGAIN) while a bulk op is live,
   never silently unwidened.
2. **APPENDED HOLDS HAVE NO RELEASE OWNER.**  Callers record each lock's release
   from their own `set[i].held`; anything appended inside
   `ft_dlm_acquire_set_at` is invisible to that sweep, so its `FT_STATE_LOCK` is
   never cleared -- the ROOT stays locked forever and every later acquire
   refuses: trie-wide livelock after ONE widened commit.  `insert` acquires
   BEFORE its content txn exists, so there is no uniform registry to park them
   in.  ⇒ ownership must be written down per caller family first.
3. **THE FIXED BOUND OVERFLOWS.**  `taken[]` / `taken_snap[]` are
   FT_FLIP_TXN_MAX_LOCKS (257) behind an assert COMPILED OUT UNDER NDEBUG; a
   widened PER_NODE set reaches FT_MAX_DEPTH (257) path nodes plus the op's own
   members.  This is the exact prior-art shape this file already records (merge
   overlap, assert at 254, NDEBUG turned it into a clobbered count).  ⇒ size for
   the widened set and make the bound a RUNTIME REFUSAL.

☞ The level word helps (3) and makes (2) bite less often; it removes neither.

**☠☠ MEASURED COST: THE COALESCING COLLAPSES.**  Extending the gate to the whole
bulk family was priced with the G5.9 instrument (full suite, 0 failures; a
throughput A/B on `inv_graft_swap_shared_dst` was ABANDONED as inconclusive --
its noise floor is ~3x in BOTH arms, 9,660-30,051 before and 8,417-23,537
after).  The gate's own counters are n=10^5-10^6 and carry no such noise:

    metric                 rekey-only gate      whole bulk family
    windows (opens)              3,249                848,712
    enters                     124,835              1,407,425
    movers per window            38.42                   1.66
    GP share of open time         2.19%                 30.43%
    waited on owner GP           1,880                278,328
    DUTY                         12.84%                 36.99%

★★ **movers per window 38.42 -> 1.66.**  "A burst of moves costs ~one GP, not
one per move" (`internal.h`) is the amortization EVERY G5 variant leans on, and
it DOES NOT SURVIVE the extension: almost every bulk op now opens its own window
and pays its own grace period.  The reason is a property of the family, not of
the gate -- graft / graft_swap / merge_at are SHORT (10-50 us) and NUMEROUS
(269k / 911k / 107k in one suite), where rekey is long (915 us) and rare (67k).
Short ops do not overlap, so they cannot piggyback.

Per-class body cost, same run (the gate now sits inside the bracket, so its GP
shows as in-body GP):

    class         mean before   mean after   in-body GP before -> after
    graft            9.7 us       17.8 us      0.00%  ->  18.45%
    graft_swap      11.9 us       20.8 us     80.59%  ->  79.60%
    merge_at        12.4 us       52.9 us      0.00%  ->  70.64%
    rekey_graft      915 us        993 us      0.24%  ->   0.18%

⇒ **1.8x-4.3x on the short bulk ops**, and rekey (which already paid the gate)
is unchanged, which is the control that makes the attribution clean.

☑ **AND THE GOVERNING ASSUMPTION, WHICH DEFLATES MOST OF THE ABOVE (Mathieu,
2026-08-28): TYPICAL BULK OPS ARE INFREQUENT**, so the refcount DOES return to
zero between them, and what must be preserved is **the speed of concurrent POINT
OPS**.  Read against that workload:
* G5.9's ~99.9% duty is a property of an arm running EIGHT DEDICATED MOVER
  THREADS IN A TIGHT LOOP.  It is the right number for that arm and the wrong
  one to design against; the sparse regime has duty ~ (bulk rate x bulk
  duration), which is small.
* The coalescing collapse is then NOT a cost: with no burst to amortize, one
  grace period per bulk op is simply the expected behaviour.  The measured
  1.8x-4.3x lands on BULK-OP LATENCY -- rare, and already the slow path -- not
  on point ops.
* ⇒ the hysteresis lever (letting the word LINGER to restore coalescing) is
  NOT needed, and would be actively wrong here: it would trade a cost nobody
  pays for MORE TIME WIDENED, which is the one thing this workload cannot
  afford.

☞ **SO THE METRIC THAT GATES THIS WORK IS POINT-OP SPEED, in two regimes:**
1. **STEADY STATE (no bulk op live) -- the one that matters most.**  Cost must be
   ONE relaxed load of the packed word behind `caa_likely`, and nothing else.
   ☐ NOT YET MEASURED; nothing reads the word today, so point ops are currently
   UNTOUCHED and the baseline is free to take.
2. **INSIDE A WINDOW.**  Cost is the widened acquire, from the published min
   level DOWN -- which is exactly what the level word exists to bound, and why
   concurrent point ops do not all serialize on the root.

### G5.11 — the ANCESTOR LEDGER lands; the RELEASE OWNER is settled; point ops measure UNCHANGED

**☑ PREREQUISITE 1 IS BUILT: a per-thread ANCESTOR LEDGER.**  Nothing already in
the tree can answer "every ancestor" at the shipping default, which is what made
the obvious widening a silent no-op there:
* `anchor[]` is never built at `CDS_FT_LOCK_SPACING_PER_NODE` --
  `ft_descent_enter_node` returns before filling it, "the zero-cost path the
  default rests on";
* the descent keeps only a FOUR-DEEP window (`nf`/`pnf`/`ppnf`/`pppnf`);
* the `metadata->parent` up-walk is REFUTED: transiently NULL while a detach or
  graft re-homes a node, and a NULL parent reads as ROOT, so the walk truncates
  SILENTLY.

So the path is recorded where the descent already crosses it, in
`ft_descent_enter_node` BEFORE the per-node early return.
* ★ **PER-THREAD, NOT IN `struct ft_descent`**, which is a per-op STACK object
  on every point op: FT_MAX_DEPTH entries is ~4 KB and would land on the hot
  path whether or not a bulk op is live.  One TLS block per thread instead.
* ★ **SAMPLED ONCE**, at `ft_descent_init`, from the packed bulk word.  Steady
  state is one load per DESCENT plus one predictable branch per level -- never a
  metadata read.  Sampling once is also what keeps the path SELF-CONSISTENT: a
  mid-descent flip would record part of a path, which is the same
  silent-truncation shape the `parent_word` walk was refuted for.
* ☠ **OVERFLOW REFUSES, IT DOES NOT TRUNCATE** (`@overflow`).

☑ **PROVEN LIVE, not assumed** (`-DFT_DEBUG_ANC_LEDGER`; nothing consumes the
ledger yet, so a zero would have been unfalsifiable without a REACH counter):

    arm                              descents   entries   overflow  disorder
    inv_rekey_contended_mixed_writers 421,394  1,271,384         0         0
    inv_rekey_fine_mixed_writers      396,373    958,597         0         0

`disorder` counts any entry not strictly deeper than its predecessor: the
widening needs the path ROOT-FIRST and strictly deepening, and zero across 2.2 M
entries is that claim measured rather than argued.

**☑ PREREQUISITE 2 IS SETTLED (design), and it is SMALLER than it looked.**  The
release owner for a widened hold is `ft_flip_txn_lock_own(txn, &mark)`, the same
hand-off `ft_rekey_cow_stop` already uses via `ft_rekey_marks_to_txn` -- it sets
`@txn_owned`, so the txn's sweep drops the word.  The rule:
* `ctx->held.txn != NULL` -> the choke point hands the ancestors straight to it.
  No per-site work.  This covers the remove sites.
* `ctx->held.txn == NULL` -> the caller must carry them.  **`insert` is the only
  such site** (`ft_lock_ctx_init(&lctx, d, NULL, ...)`; "@ic->txn is not live at
  this point"), so it is ONE site of plumbing, not four.
* ☠ REGISTER BEFORE THE RECORDS THE MARK LICENSES -- the 17-site class closed at
  `55c0350c`.

☐ **PREREQUISITE 3 REMAINS:** `taken[]` / `taken_snap[]` are sized
FT_FLIP_TXN_MAX_LOCKS behind an assert compiled out under NDEBUG, and a widened
per-node set reaches FT_MAX_DEPTH plus the op's own members.  Size for the
widened set and make the bound a RUNTIME REFUSAL.

**☑ AND THE METRIC THAT GATES THIS WORK IS UNMOVED SO FAR.**  Steady-state point
ops (`inv_concurrent_writers_disjoint`, 16 writers, no bulk op live), 10 reps per
arm, INTERLEAVED A/B/A/B against `350141cd`:

    AFTER  median 7,493 ops     BEFORE median 7,433 ops     delta < 1%

☠ Read against a NOISE FLOOR of ~8% (7,210-7,789 across twelve reps of ONE
build), so the honest statement is NOT DETECTABLE, not "0.8% faster".  The gate
half adds a ~516-byte per-trie array and no point-op read, and that is what the
number says.  ☞ Re-take this baseline once the widening actually reads the word.

### G5.12 — the ACQUIRE BOUND is a RUNTIME REFUSAL; prerequisite 2 is INSEPARABLE from the widening

**☑ PREREQUISITE 3 IS BUILT AND EXERCISED.**  `ft_dlm_acquire_set_at` sized its
`taken[]` / `taken_snap[]` by FT_FLIP_TXN_MAX_LOCKS behind an assert COMPILED
OUT UNDER NDEBUG, and G5.5's widening ADDS the member's ancestors, so a set can
reach FT_FLIP_TXN_MAX_LOCKS + FT_MAX_DEPTH.  That is the shape which already
produced a live defect once here (a lock_fine `cds_ft_merge_at` overflowed at
254 and, under NDEBUG, the first overflowing store clobbered `@nr_locks` with a
pointer's low word so the release sweep CASed through garbage).

* **EMBEDDED ARRAY + HEAP OVERFLOW**, the shape FT_FLIP_TXN_FLOOR_LOCKS itself
  prescribes -- NOT a bigger fixed array.  ★ Sizing the stack for the widened
  worst case would put ~8 KB on EVERY point op, including the overwhelming
  majority that never widen, and POINT-OP SPEED is the metric this step is gated
  on (G5.10).  The ordinary set stays exactly as cheap as before.
* **THE BOUND IS A RUNTIME REFUSAL** (`FT_DLM_ACQUIRE_MAX_SET`), not an assert.
  ★ Refusing is SAFE HERE in a way that dropping a registration is not: NOTHING
  IS ACQUIRED YET at that point, so there is no word whose release is orphaned
  -- unlike `ft_flip_txn_lock_register`, which must `abort()` because a dropped
  registration leaks a lock forever.
* ☠ **AND THE PATH IS EXERCISED, NOT MERELY COMPILED.**  Until the widening
  lands no caller presents a set past the embedded bound, so the overflow branch
  would ship never having executed once -- unvisited, not covered.
  `-DFT_DEBUG_FORCE_ACQ_HEAP` drops the embedded bound to 1 and sends
  essentially every acquire down it.  Under ASAN with `detect_leaks=1`:

      inv_rekey_contended_mixed_writers   170,615 overflow allocations, PASS
      inv_concurrent_writers_disjoint                                   PASS
      inv_rekey_fine_mixed_writers                                      PASS
      AddressSanitizer / LeakSanitizer                                  CLEAN

  The counter is what makes that green mean anything: a build reporting zero
  allocations says so in words rather than passing quietly.

**☠ PREREQUISITE 2 CANNOT LAND ON ITS OWN, and should not be attempted to.**
The owner is settled (`ft_flip_txn_lock_own`, the hand-off `ft_rekey_cow_stop`
already uses; `ctx->held.txn != NULL` covers the remove sites, and `insert` is
the only site whose txn is not live at acquire time).  But `insert` creates its
txn **BOUNDED** (`ft-insert.h:866`, `ft_flip_txn_create_bounded(ft, 15 +
anchored + count_edges)`) AFTER the acquire at `:771`, and every widened hold
needs a RELEASE EDGE in that reservation.  So producing the holds, SIZING the
reservation for them, and recording their release are ONE edit -- and without
the widening there is nothing to size, nothing to release, and nothing to test.
⇒ prerequisite 2 lands WITH the widening, as one reviewable change.  Building it
first would add an untestable path to the hottest point op, which is the same
mistake the overflow branch above only avoided by being force-exercised.

☞ **REMAINING, IN ORDER:** the widening itself (consuming the ledger from the
published min level down, and handing its holds to the owner above), then the
LIVENESS gate -- per-point-op abort/age histograms sampled INSIDE the attempt,
which G5.9 established cannot be measured until the widened acquire exists.

### G5.13 — ☠☠ PREREQUISITE 1 IS NOT DONE: the ledger had no OWNER STAMP, and half the acquires that must widen HAVE NO DESCENT AT ALL

G5.11 landed the ancestor ledger and G5.12 called prerequisite 1 discharged.
Measurement says it is not.  Two gaps, neither of which the ledger commit could
have seen, because nothing consumed the ledger yet and the question "is this
ledger MINE?" had no way to be asked.

**THE INSTRUMENT** (`-DFT_DEBUG_WIDEN_OWNER`, opt-in).  It widens NOTHING --
the build takes exactly the locks it takes without it -- and only counts, at
`ft_dlm_acquire_set_at`, what the widening WOULD find.  Its FIRST version was
REFUTED by an adversarial review before any conclusion was drawn from it; what
follows is the surviving half, and the refutations are recorded below because
each names a limit the widening itself inherits.

    arm (2 s, FT_INV_MW=1, -O2 -DNDEBUG)   contended        fine
    acquires                                 866,646       778,511
    gate enters                                  490        16,176
    LIVE (bulk up, caller not bulk)          420,671       306,215
    self_bulk (the caller IS the bulk op)    445,657       471,960
    ---- release owner, a LOWER BOUND ----
    ctx->held.txn (top frame)                201,101       146,897
    an OUTER frame's txn                           0             0
    no txn in ANY frame                      219,570       159,318
    ---- ledger verdict ----
    VALID                                    219,569       159,284
    NO DESCENT (ctx->d == NULL)              201,102       146,931
    not recording / superseded / overflow      0/0/0         0/0/0

    SITE                                  live      txn_top   led_ok
    ft_node_recompact          :1357   420,628      201,101  219,527
    ft_insert_dlm_acquire_split: 771        42            0       42
    ft_chain_compress_fused    :1021         1            0        0
    ft_detach_orphan_planlock  : 705  (fine arm only, 33)  0        0

**☠ GAP 1 -- THE LEDGER COULD NOT SAY WHOSE IT WAS.**  `@anc_rec` is sampled
ONCE at `ft_descent_init` while the acquire samples the gate LIVE, and an op
runs more than one descent (`ft_detach_node`'s orphan walk, `ft_anchor_descend`).
A bulk op that goes live MID-DESCENT therefore hands an acquire "widen now" over
a ledger some EARLIER descent on that thread left behind -- possibly another
op's, and (the mixed arms run two tries) possibly another trie's.  The result is
populated, self-consistent, and describes THE WRONG PATH, so the widening would
lock the wrong ancestors and take every one of them successfully: an exclusion
gap with no symptom.
☑ CLOSED by a GENERATION STAMP -- `@gen` (per-thread, monotone) and `@ft` on the
ledger, `@anc_gen` on the descent, and `ft_anc_ledger_valid()` as the predicate a
consumer must call.  A descent COPY carries the generation it was copied from,
which is what keeps a copy legal until a fresh `ft_descent_init` supersedes it.
★ The measured verdict is now a FALSIFIABLE NEGATIVE, which it could not be
before: `superseded = 0` and `not_recording = 0` over 726,886 live acquires,
against `led_bad` NON-ZERO -- so the classifier is demonstrably not inert.

**☠☠ GAP 2 -- 48% OF THE ACQUIRES THAT MUST WIDEN HAVE NO DESCENT.**  Not a
stale ledger: no ledger at all, and none possible.  `ctx->d == NULL` at 201,102
of 420,671 live acquires (146,931 of 306,215 in the fine arm), all of them at
`ft_node_recompact`.  The cause is the SAME SHAPE the ledger commit fixed one
level down: the shipping default's zero-cost path skips the work the widening
needs.  `ft_descent_enter_node` returns before filling `anchor[]` at PER_NODE --
fixed @`fb7e2ce5` by recording BEFORE that early return -- but the remove paths
gate the anchor descent ITSELF on the spacing (`ft-remove.h:4514`, `:5470`:
`if (ft->lock_spacing != CDS_FT_LOCK_SPACING_PER_NODE)`), so at the default they
build their lock context with NO descent whatsoever and there is nothing to
record.  ⇒ prerequisite 1 needs those descents to run whenever a bulk op is
live, exactly as the ledger recording does.

**★ AND THE TWO HALVES ARE AN EXACT PARTITION** -- the finding that reorders the
work.  ☠ FIRST STATED OFF TWO EQUAL MARGINALS, WHICH DO NOT ESTABLISH IT: equal
`led_ok` and `txn_none` totals read identically for a population where the two
properties are INDEPENDENT and merely equinumerous.  So the 2x2 was counted:

    joint (led_ok x has-txn-in-ANY-frame)   contended        fine
    can widen  AND has a ctx txn                    0            0
    can widen, NO ctx txn                     246,143      160,148
    cannot widen, HAS a ctx txn               234,335      146,414
    cannot widen, no ctx txn                        0            4

**No acquire both can widen and carries its owner in the ctx.**  (The 4 are the
two single-digit sites.)  ⇒ prerequisite 2 is not the next blocker;
prerequisite 1 is.

**☑ AND THE NO-DESCENT CLASS IS ATTRIBUTED, not hypothesised.**  Counters at the
two candidate sites place **234,335 of 234,335** (and 146,414 of 146,418) at
`_cds_ft_remove_locked` alone; the structurally identical chain-head site
(`ft-remove.h:5470`) measured **zero** in these arms -- which is a statement
about the arm, not about the site.

**☠ G5.12's RELEASE-OWNER PREDICATE IS REFUTED -- as a PREDICATE, not as a
plan.**  "`ctx->held.txn != NULL` covers the remove sites; `insert` is the ONLY
site needing plumbing" is false twice over.  By weight, `insert` is 42 of 420,671
live acquires and `ft_node_recompact` is 420,628.  And `ctx->held.txn` is not the
ownership question at all: `ft_node_recompact`'s release owner is its
`@retire_txn` PARAMETER -- non-NULL by its own acquire's guard
(`ft-mutation-node.h:1252`, `if (ft->lock_fine && retire_txn && ...)`) and
already the owner it registers C/P/GP releases on (`:2400`, `:2430`) -- which sits
in no ctx frame.  The detach family is the same shape the other way round: it
ACQUIRES FIRST AND HANDS OFF LATER (`ft_flip_txn_lock_own`, `ft-remove.h:573`),
stitching the txn into `lctx.held.txn` only afterwards (`:2409`, `:2991`, `:3185`).
⇒ the counter's `txn_none` is a LOWER BOUND on ownership and must never be read
as "no owner exists".

**☠ WHAT THIS INSTRUMENT CANNOT ANSWER, recorded so no one reads it as if it
could.**
1. **HOW WIDE the widened set is.**  Every gate entry in the tree passes level 0
   (`ft-detach.h:646`, `ft-graft.h:2591` / `:2842`, `ft-merge.h:3753`,
   `ft_move_gate_enter`), so `ft_bulk_min_level()` is CONSTANTLY 0, "from the min
   level down" filters nothing, and the count is the arm's KEY DEPTH (max 4 and
   3 here) rather than a design quantity.  It is reported under that name.  ☠ And
   it must not size anything: the bound is STRUCTURAL
   (`FT_DLM_ACQUIRE_MAX_SET`), and replacing a structural bound with a measured
   maximum is the defect class that fired at 254 (§ the MAX_LOCKS overflow).
2. **WHETHER A SITE IS THE ONLY ONE.**  Reachability is a code fact; a site
   absent from the table was not exercised by the arm.
3. **RATIOS.**  Every counter is a process-wide atomic on the acquire path, so
   the instrument perturbs what it measures (G5.9 paid 2x for this).  EXISTENCE
   and MAXIMA survive; percentages do not.

**☑ THE CONTROLS THE FIRST VERSION LACKED**, each answering a zero that would
otherwise be unfalsifiable: `acq_total` (the choke point ran at all -- a
non-`lock_fine` trie makes every number below vacuous); `gate_enters`, bumped
inside `ft_bulk_gate_enter` UNDER THE SAME `-D` (a different flag's counter would
be a feature-flag-matrix miss reported as a measurement); and `lock_fine` +
`lock_spacing` on every site row, so a dump that aggregates coarse and fine arms
is readable rather than silently mixed.

### G5.14 — ☑ PREREQUISITE 1 IS COMPLETE, and finishing it EXPOSED a silent ledger corruption

**THE ARM.**  `_cds_ft_remove_locked` gains a third arm: at PER_NODE, when a bulk
op is live, descend for the LEDGER.  Deliberately NOT the coarse arm's body --
that one RETARGETS the holder and can demand a retry, a coarse-spacing
correction this spacing has never taken and that nothing measures here, so
`@holder_flag`, `@holder_depth` and `@need_retry` are left exactly as the handle
derived them.  ☠ But only the RETARGET is dropped, never the VALIDATION: the
descent reaches the lock context only when the walk demonstrably PASSED the
holder and no reanchor rewind occurred (`@skip_conflict`).  A holder the descent
did not pass is one the back-edge names STALELY -- the arm above measures that
lane at 78% of its recoveries -- so the recorded path would be the key's FORWARD
branch while the op mutates the other one, and **the ledger stamp has no path
term to catch that**.  On a mismatch the context gets NO descent, the widening
refuses, and the op's retry loop re-derives.

    ledger verdict at the acquire     BEFORE            AFTER
    live acquires                 420,671 / 306,215  557,041 / 320,419
    VALID                         219,569 / 159,284  557,041 / 320,419
    NO DESCENT                    201,102 / 146,931          0 / 0
    led_bad, every site                    non-zero          0 / 0

**☠☠ AND IT ACTIVATED A DORMANT CORRUPTION -- caught by an adversarial review
BEFORE it landed, then MEASURED.**  `ft_detach_node` COPIES the caller's descent
and the orphan walk extends the copy through `ft_walk_extend` ->
`ft_descent_enter_node`, whose ledger push sits BEFORE the per-node early
return.  So a copy that kept `@anc_rec` FILES THE ORPHANED SUBTREE onto the key
path **under the original generation**, and `ft_anc_ledger_valid` still vouches
for the result.  It was dormant only because the descent was missing -- the very
gap this arm closes.  Measured with a copied-push counter: **0 before the arm,
13 copied pushes and 13 disorder events with it**, in one two-second run.

☑ **CLOSED BY SPLITTING THE CREDENTIAL.**  `@anc_rec` means "may WRITE";
`@anc_gen` means "may READ".  A copy keeps the second and loses the first, so it
can consume the ledger and can never append to it.  Re-measured: `copy_push = 0`,
`disorder = 0`, over 4.1M ledger entries, with `led_ok == live` exactly.
★ The two roles had been conflated since the ledger landed; nothing could see it
while no descent reached the copy.
☠ `ft_walk_extend`'s header still claimed "a no-op under per-node granularity" --
false since the push was hoisted above that return, and a stale mechanism on a
true claim is the worst shape.  Corrected.

**☐ NOT APPLIED AT THE CHAIN-HEAD SITE** (`ft-remove.h:5470`), and the reason is
not that it measured zero.  `cds_ft_remove_all` has **NO RETRY LOOP** -- it calls
`_cds_ft_remove_all_locked` once and maps -EAGAIN to a user-visible
BUSY/MEMORY error -- so a widening refusal there would turn an op that succeeds
today into a hard failure.  That site needs its own answer (an internal
re-descend, or unconditional descent semantics) before it can widen at all.

### G5.15 — the WIDENING IS BUILT, and it FAILS ITS OWN GATE: 5.8x on point ops, at 1.2% coverage

Behind `-DFT_FEATURE_WIDEN`, OFF BY DEFAULT, because G5.9 established that the
regime it creates cannot be measured until it exists and that LIVENESS, not
correctness, is its first gate.  The widening lives in the universal acquire
choke point; what is per-SITE is the RELEASE OWNER, and `ft_node_recompact`
supplies its `retire_txn` through a CHAINED frame (`@widen_txn`) rather than by
repointing `@ctx->held.txn`, which decides DEDUPE for every member of that set.

**☑ CORRECT.**  ft_inv 119/119 with the widening ON; ft_unit at its three
pre-existing failures.  `max=4` widened locks, 141,010 taken over 70,448
widened acquires.

**☠☠ AND IT FAILS THE GATING METRIC.**  Interleaved A/B on
`inv_rekey_contended_mixed_writers`, 3 reps, same instrument in both arms:

    widening OFF   77,675 / 86,478 / 80,975 point ops
    widening ON    14,027 / 14,013 / 15,866

**~5.8x, non-overlapping, against a ~8% noise floor.**  ★ AND THE MECHANISM IS
NOT THE LOCK-TAKING: only 9% of live acquires widened at all (14,258 of
153,902 -- the rest DEDUPED onto words the op already held), yet throughput fell
5.8x.  The cost is the CONTENTION the extra word creates, and it is G5.5's own
stated price arriving on schedule: **every gate entry passes level 0**, so
`ft_bulk_min_level()` is constantly 0 and EVERY widened acquire takes THE ROOT.
"All point ops serialize trie-wide for the window" is no longer a prediction.
☞ ⇒ the PER-OP JUNCTION LEVEL is a REQUIREMENT, not the later refinement G5.10
filed it as.  The packed word already carries the level; what is missing is ops
that pass one.

**☠☠ AND COVERAGE IS 1.2%.**  Per-site attribution of the acquires that COULD
widen and had no owner wired, full suite:

    SITE                            live      led_ok    NO_OWNER
    ft_unchain_node      :4188  2,499,561   2,342,792   2,499,561
    _cds_ft_insert       :3329  2,295,275   2,273,329   2,295,275
    ft_node_recompact    :1380  1,729,268   1,693,101           0
    ft_insert_dlm_acq_sp : 771    418,483     417,158     418,483
    ft_chain_compress    :1028    396,742     394,452     396,742

70,152 widened against 5,604,209 declined.  ☠ The two-arm measurement that put
`ft_node_recompact` at 99.99% of the population was TRUE OF THOSE ARMS AND
BADLY WRONG OF THE SUITE -- the largest single class is
**a remove that does not recompact** (`ft_unchain_node`), which acquires through
the same choke point and needs the same widening.

☠ **AND FOUR OF THE FIVE SITES ACQUIRE BEFORE THEIR TXN EXISTS** --
`ft_unchain_node` creates its txn inside the branches BELOW its acquire, exactly
as `insert` does (G5.12).  So the remaining wiring is prerequisite 2's
STRUCTURAL problem at four sites, not four one-liners.

☞ **AND WIRING THEM IS THE WRONG NEXT MOVE.**  It would multiply, by ~80x in
acquire count, a cost that ALREADY fails the gate -- every newly wired site
would take the root too.  The order that follows from the measurement is:
**(1) per-op junction levels so ops stop taking the root, (2) re-take the A/B,
(3) only then complete the owner plumbing.**

### G5.16 — ☠ A STATIC PER-OP JUNCTION LEVEL IS UNSOUND: bulk ops REACH THE ROOT

G5.15 made per-op levels a requirement.  The first question is whether an op can
DECLARE one at all, and the gate's own design makes that strict: the level is
**fixed at gate ENTRY** -- lowering it later owns a grace period, and the seam
rule forbids one under a held lock -- so a declaration must bound the shallowest
word the op will EVER lock, before its descent has run.

The obvious bound is the op's KEY LENGTH: `cds_ft_graft`, `cds_ft_merge_at` and
`cds_ft_detach_at` all take one, so the junction depth is a parameter and needs
no descent.  ☠ **AND IT IS NOT SOUND, because recompaction CLIMBS** -- a DEL
that empties a node recompacts its parent, which can cascade upward.  Measured
at the acquire, for acquires made BY a bulk op (`ft_bulk_self_depth > 0`),
asking only whether the set contains the trie ROOT:

    arm         bulk acquires   members    reaching the ROOT
    contended         442,483   513,610      71,127   (16.1%)
    fine              471,342   547,132      75,790   (16.1%)

**About one bulk acquire in six touches the root**, so a key-anchored
declaration would be violated constantly and silently.
☠ **READ IT QUALITATIVELY, NOT QUANTITATIVELY.**  Both arms hold 159-166 live
keys and the ledger's depth maxes at 3-4, so almost everything is within a few
hops of the root and 16% is a statement about THIS GEOMETRY.  What generalises
is only the sign: the set is NOT confined below the junction, so a static
declaration is unsound at any depth.  ★ Asking by IDENTITY rather than by depth
is deliberate -- a member's `@depth` is 0 and meaningless at the shipping
PER_NODE spacing (`ft_lock_ctx_depth_of` answers 0/true there), so a depth
histogram would have measured the spacing instead of the op.

### ☠☠ G5.17 — THE 5.8x AND THE 16% ARE BOTH MEASURED ON BULK-AT-DEPTH-1, so NEITHER JUDGES THE LEVEL CEILING

**The arms cannot discriminate the thing G5.15/G5.16 were read as judging.**
Both mixed-writer arms drive their movers with **TWO-BYTE keys**
(`rkmix_probe_writer_reached`, `src_key[2] = { w->bp, w->sb }`), and the
contended arm gives EVERY one of its eight movers `bp = 1`, `dp = 2` -- so every
rekey moves a subtree between the ROOT'S CHILD 1 and the ROOT'S CHILD 2.  The
junction sits at **DEPTH 1**, in a trie whose keys are `RKP_KLEN = 4` bytes (the
ancestor ledger's depth maxes at 3-4, which agrees).  The disjoint arm differs
only in giving each mover its own pair (`bp = 2i+1`, `dp = 2i+2`) -- still depth
1.

⇒ **A per-op junction level in these arms would publish ~1**, and widening "from
level 1 down" instead of "from 0 down" excludes exactly ONE WORD -- the root --
out of a 3-4 deep path, while the contention on the shared depth-1 junction
remains in full.  **The array-based level ceiling has almost nothing to cut
here.**

☠ So the 5.8x is the cost of **bulk-at-depth-1 with every writer in one
junction**, which is candidate (E)'s already-listed open item ("a near-root J
serializes broadly -- the honest cost of bulk-near-root"), NOT a verdict on the
ceiling.  And the 16% root-reach is the same geometry seen from the other side:
at depth 1, touching the root is nearly unavoidable.  **Both numbers are SILENT
on the ceiling's viability.**
★ Same error class as reading a joint off two equal marginals: generalising from
arms that cannot carry the claim.  The arms were built to expose an
unarbitrated SW park on a shared child, not to vary DEPTH.

☞ **WHAT WOULD DECIDE IT** -- an arm the suite does not have: LONG keys, a bulk
junction genuinely DEEP in the trie, and point ops working in a DIFFERENT
subtree.  Only there does a published min level make the widened set small, and
only there is "point ops stop taking the root" worth anything.  Until such an
arm exists, the ceiling is UNMEASURED, not refuted.

**☑ AND IF THE ARRAY SCAN COSTS TOO MUCH, THE MONOTONE CEILING IS THE FALLBACK.**
`bulk_level_nr[]` is `uint16_t[FT_MAX_DEPTH + 1]` -- 516 bytes per trie
(`fractal-trie-internal.h:1807`; the `level > FT_MAX_DEPTH` clamp indexes 257,
which the `+ 1` makes valid).  Entry is O(1), but EXIT rescans
`while (l < FT_MAX_DEPTH && !bulk_level_nr[l]) l++` -- up to 257 iterations
**under `move_gate_lock`**, on the exit that empties the current min.  ☠ That is
work on the CONTENDED mutex, the exact shape G5.9 paid 2x for.

The fallback is the MONOTONE low-water mark this design started from: enter
`if (level < min) min = level`, exit does nothing but reset to NONE at refcount
0.  O(1) both ends, no array.  ★ **It loses no precision in the regime that
matters** -- it is imprecise only while windows OVERLAP, and under G5.10's
governing assumption (bulk ops INFREQUENT, refcount returns to 0) it is exactly
as precise as the array.  Its imprecision is OVER-widening, which
ADD-never-substitute makes safe: more locks are never wrong, only slower.
⇒ the array's only advantage is the HAMMERING regime, which the governing
assumption says not to design for.  ☠ This REVERSES the preference recorded when
the packed word landed ("the per-level array is correct in BOTH regimes for a few
bytes") -- that was priced against a refcount that never reaches 0, itself the
hammering artifact.
☞ Today the scan is inert: every op passes 0, so slot 0 is the only live one and
emptying it coincides with `bulk_gate_nr == 0`, which takes the `else` branch.
The cost appears only when ops publish REAL levels -- i.e. exactly when the
ceiling is built.

### G5.18 — ☑☑ THE DEEP-JUNCTION ARM: the 5.8x WAS THE GEOMETRY.  Deep, it is 1.48x

`inv_widen_deep_junction` (`tests/regression/test_urcu_ft_inv.c`) varies the one
thing both rekey arms hold fixed -- DEPTH.  Movers own a junction at byte 12,
point writers live under their own top bytes (a subtree the movers never enter,
so the two diverge at depth 0), keys are 16 bytes.  A THROUGHPUT probe, not an
oracle: it asserts only progress, and the mixed-writer arms keep the
correctness job.

    point ops / run          OFF                    ON            ratio
    shallow (junction d=1)   77,675/86,478/80,975   14,027/14,013/15,866   5.8x
    DEEP    (junction d=12)  752,504/843,554/759,444 513,634/495,214/533,180 1.48x

**☑☑ THE 5.8x WAS MOSTLY THE GEOMETRY.**  Same widening, same level 0, same
instrument -- only the depth and the subtree disjointness change, and the cost
falls from 5.8x to ~1.48x, distributions still separated.  ⇒ G5.15's number was
the price of BULK-NEAR-ROOT-WITH-EVERY-WRITER-IN-ONE-JUNCTION, and reading it as
the widening's cost was wrong.
★ **AND THE RESIDUAL IS EXACTLY WHAT A LEVEL WOULD ATTACK**: this is still level
0, so every widened acquire STILL TAKES THE ROOT and still widens over the whole
15-node path.  A published junction level of 12 would cut that to the handful of
nodes below it.  The ceiling now has something to cut, and an arm that can
measure whether it does.

**☠☠ AND THE ARM COST TWO REAL LESSONS BEFORE IT MEASURED ANYTHING.**

1. **BYTE DEPTH IS NOT NODE DEPTH, and the widening walks NODES.**  The first cut
   used 16-byte keys with a long shared prefix and produced an ancestor ledger
   whose depth maxed at **2**: a sparse key PATH-COMPRESSES, so a run of bytes
   nobody branches on collapses into ONE node.  Deep keys bought nothing.  The
   fix is to FORCE A BRANCH AT EVERY LEVEL -- for each byte position d, one extra
   key agreeing on [0,d) and differing at d, which is exactly the condition for a
   node to exist at d.  Ledger depth 2 -> **15**, and point ops rose 6x (136k ->
   822k) merely from the trie having a shape.
   ☞ Generalises: any FT arm that means to be "deep" must SEED DENSITY, not long
   keys.  `ledger nr max` under `-DFT_DEBUG_WIDEN_OWNER` is how to check it.
2. **THE ORDERED LIST COUPLES DISJOINT SUBTREES, and it LIVELOCKS this arm.**
   With the list on, the point writers' churn -- in a subtree the movers never
   enter -- keeps `ft_ord_cell_find_splice_pos_coherent`'s two derivations from
   agreeing; the rekey returns -EAGAIN ("torn derivation: re-descend") and its
   internal retry never terminates.  Measured: one mover stuck inside a single
   `_cds_ft_debug_rekey_graft_simple`, spinning in `ft_node_get_minmax`, every
   other thread already stopped.  The cell list is TRIE-WIDE, so subtree
   disjointness does not buy list disjointness.
   ☞ ☐ **A REAL FINDING WORTH ITS OWN ARM** -- an unbounded internal retry driven
   by unrelated traffic is a liveness bug, not a tuning matter.  Turned OFF here
   because it confounds the trie-lock contention this arm isolates.

☐ Also visible and not chased: a deep rekey holds the gate for essentially the
whole run (8 gate entries, 1.65M acquires seen as "bulk live"), so a single deep
move is orders slower than a shallow one.

### G5.19 — the CEILING REPRESENTATION: TWO PACKED EPOCH WORDS, flipped ON DEMAND

The per-level count array buys an EXACT minimum and pays for it on the contended
mutex (G5.16).  The single MONOTONE word is O(1) but is a ONE-WAY CRANK: the min
only lowers while ops are live and resets only when the GLOBAL refcount reaches
0, so an overlapping stream pins it at the shallowest level ever published.
**TWO PACKED {refcount, level} WORDS -- two EPOCHS -- fix the crank without the
array**, and the flip is DEMAND-DRIVEN:

    ENTER(level)                       [under move_gate_lock]
      if other.refcount == 0 and level != cur.level:
          gp = (level < cur.level)       # ☠ GP iff we LOWER -- NOT because we flip
          other = {1, level};  cur = other
      elif level < cur.level:
          cur.level = level;  cur.refcount++;  gp = true
      else:
          cur.refcount++;  gp = false

    EXIT(level)   e.refcount--;  if 0: e.level = NONE        # recovers, NO GP
    POINT OP      load BOTH words; min over those with refcount != 0

☠☠ **THE GRACE PERIOD IS OWED BY THE LOWERING OF THE EFFECTIVE MINIMUM, NOT BY
THE FLIP.**  The flip is only WHERE the new value is stored, and conflating the
two hides a case worth having:

* **A flip that LOWERS** -- peers sampled the old, deeper ceiling and widened too
  little, so the exclusion gap is real and the GP is unavoidable.
* **A flip that does NOT lower** -- the arriving op's level is DEEPER than
  `cur.level`.  After it, `min(cur_old.level, deep) == cur_old.level`: the
  effective minimum is UNCHANGED, no peer needs to widen more than it already
  has, and **no grace period is owed**.
  ★ Worth exploiting, because otherwise a new DEEP op enrolling into the current
  SHALLOW epoch keeps that shallow ceiling alive for its whole duration -- the
  crank again in a different guise.  Flipping it into a fresh epoch lets the old
  shallow epoch drain on its own, and the ceiling RISES the moment it does.

⇒ **the flip never buys GP avoidance on the lowering path; it buys CEILING
RECOVERY.**  The old epoch keeps its deeper level and drains independently, so
when the shallow op leaves the ceiling springs back immediately instead of
waiting for global quiescence.
* ☑ An ORDINARY enrolment (`level >= cur.level`, peer busy) owns NO GP: it
  publishes nothing a peer could be running against.  That keeps the common case
  cheap.
* ★ Safe against the SEAM RULE throughout: the level is fixed at gate ENTRY,
  before the op takes any lock, so no GP is ever taken under one.
* ☑ EXIT owns none either -- the ceiling only RISES there, and a point op holding
  more locks than it needs is never wrong.
* ☠ **THE OP MUST REMEMBER WHICH EPOCH IT ENROLLED IN**: exit decrements THAT
  one, not the current one, which may have flipped underneath it.
* ☠ **EACH EPOCH STAYS ONE PACKED WORD.**  The two point-op loads are not atomic
  together and need not be -- a reader that sampled before the flip is a
  NON-OBSERVER, which is what the GP waits for.  What no GP can rescue is a torn
  read WITHIN an epoch (live refcount + stale DEEPER level = too few locks), so
  the per-epoch packing is not optional.
* Enter and exit both hold `move_gate_lock`, so the flip races with nothing.
* When BOTH epochs are occupied the rule degrades to the single-word crank --
  correct, and only in a genuinely busy window.

⇒ O(1) at both ends, no 257-slot rescan under the gate mutex, 516 bytes back, and
one extra relaxed load per point op (put the words adjacent).

### G5.20 — ☠ THE ENFORCEMENT PREDICATE: the LEDGER CANNOT DATE A BULK OP'S MEMBERS (72% off-path)

The declaration half of a junction level is useless without an ENFORCEMENT half:
a bulk op must detect that it is about to lock ABOVE the level it declared.  That
needs a DEPTH per member, and `set[i].depth` cannot supply one at the shipping
spacing -- `ft_anchor_meta` returns before reading it, `ft_lock_ctx_depth_of`
MANUFACTURES 0, and a back-pointer-resolved member carries
`FT_DEPTH_FROM_DESCENT`.

**THE ASYMMETRY IS THE POINT: the POINT side HAS the information, the BULK side
does NOT.**  A point op reads its ancestors from the ledger, whose depths are the
`start` values the DESCENT passed to `ft_descent_enter_node` -- real, and
recorded at EVERY spacing because the push sits before the per-node early return.
That is why the widening resolves its widened members correctly even coarse.  A
bulk op has no such source for the members it LOCKS.

The obvious candidate is the bulk op's OWN ledger -- it descends too.  ☠
**REFUTED BY MEASUREMENT: most of its members are not on its own path.**  Asked
by IDENTITY, for acquires made by a bulk op:

    arm                bulk members   datable from own ledger   UNDATABLE
    contended (d=1)         489,277        135,284 (27.7%)   353,993 (72.3%)
    fine      (d=1)         543,037        150,546 (27.7%)   392,491 (72.3%)
    deep      (d=12)             32             16                 16

⇒ **~72% OFF-PATH, and structurally so**: `ft_node_recompact` resolves P and GP
through BACK-POINTERS, and the detach / orphan walks reach siblings and children
the key descent never entered.  A predicate that refused on an undatable member
would escalate nearly every bulk op to level 0 -- which is what ships today.
★ The DEEP arm reaches the root **0** times against 16% shallow: a third
independent confirmation that G5.16's fraction was geometry.

☞ **AND THIS IS THE SAME SHAPE FOR THE THIRD TIME.**  The default's zero-cost
path keeps skipping exactly what the widening needs, and each fix makes the work
conditional on THE GATE BEING OPEN rather than on the spacing:

    @fb7e2ce5   the ledger was not RECORDED at PER_NODE
                -> record BEFORE ft_descent_enter_node's early return
    @3acd5488   the anchor descent did not RUN at PER_NODE
                -> run it when a bulk op is live
    ☐ next      member DEPTHS are not SUPPLIED at PER_NODE
                (ft_lock_ctx_depth_of manufactures 0)
                -> supply real ones when a bulk op is live

That third step is what the enforcement predicate waits on, and it is the honest
cost of the ceiling: PER_NODE's "a member anchors on itself, so no depth is
needed" is precisely the optimisation the widening cannot keep.
☠ NOT free: dating a back-pointer-resolved member is what
`ft_lock_ctx_depth_of_parent` and the descent WINDOW exist for, and 72% of
members reach the acquire without a descent that describes them.  Whether the
ONE-HOP rule can date them, and at what cost, is the open question.

### G5.21 — ☠☠ THE ONE-HOP RULE DOES NOT CLOSE IT EITHER: ~55% of a bulk op's members stay UNDATABLE

G5.20 left one question: the ledger cannot date a bulk op's members, but the
REAL dating path is not the ledger -- coarse spacings use
`ft_descent_depth_of`, the descent's FOUR-DEEP WINDOW.  That is a genuinely
different source: for a COPIED descent the window keeps advancing through
`ft_walk_extend` while the ledger deliberately no longer records (@`3acd5488`),
so the window can date members the ledger has never seen.  Measured, by running
the actual function against a bulk op's members:

    arm            members   ledger-datable    WINDOW-datable   undatable
    contended      498,173   137,822 (27.7%)   222,529 (44.7%)     55.3%
    fine           552,290   153,196 (27.7%)   245,898 (44.5%)     55.5%
    deep (n=32)         32        16 (50%)          24 (75%)       25%

★ The window IS better than the ledger -- 28% -> 45% -- and for the predicted
reason.  ☠ **But it still leaves the MAJORITY undatable.**  A conservative
enforcement predicate must treat "undatable" as "possibly above my declared
level", so it would escalate on 55% of members, i.e. on very nearly every
acquire, i.e. to level 0 -- which is what ships today.

⇒ **OPTION 1's ENFORCEMENT HALF IS NOT BUILDABLE ON THE EXISTING DATING
MACHINERY.**  Supplying real depths at PER_NODE (G5.20's third step) is
necessary and NOT sufficient.

☠ The deep arm's 75% is far better and is the geometry the ceiling exists for --
but n=32, from 8 moves.  It is a hint, not a result; the arm needs movers that
cycle before it can carry that number.

☞ **WHAT IS LEFT, none of it cheap:**
1. **STORE a depth on the node.**  Then every member dates by a load.  ☠ But a
   REKEY MOVES SUBTREES, so every node under a moved junction changes depth --
   a stored depth would need rewriting across the whole subtree on every move,
   which is very likely why it is not stored today.
2. **AN IDENTITY-BASED PREDICATE that needs no depth.**  ☠ The obvious one -- walk
   up from the member -- is the `parent_word` up-walk, refuted in-tree
   (transiently NULL during re-homes; NULL reads as ROOT, so it truncates
   SILENTLY).
3. **MAKE THE DECLARATION SOUND BY CONSTRUCTION** -- bound the climb before the
   gate.  No cheap bound is known (G5.16); the worst case is the root.
4. **RE-OPEN THE PER-NODE MARK** (candidate (E)'s tier 2, G5.6/G5.7).  It was
   refuted on its WORD, not on its principle, and it is the one shape that asks
   the ANCESTOR rather than the member -- which is the side that HAS the
   information.

### G5.22 — ☑☑ THE UP-WALK IS NO LONGER REFUTED, AND IT DATES **100%** OF A BULK OP'S MEMBERS

Every "the parent_word up-walk is refuted" in this document and in @`fb7e2ce5`
rests on ONE fact: *"it goes transiently NULL while a detach or graft re-homes a
node, and a NULL parent reads as ROOT, so the walk truncates SILENTLY."*
**THAT FACT NO LONGER HOLDS.**  A root does not store NULL any more -- it stores
the TRIE POINTER:

    ft_parent_word(ft, parent_node) = parent_node ? parent_node
                                                  : ft_trie_parent(ft)
    ft_parent_is_trie(p)            = p && !((uintptr_t) p & FT_PARENT_TAG_MASK)

with `struct cds_ft` statically asserted aligned past the whole parent tag.  So
the raw word has THREE distinguishable states -- a tagged node (a real parent),
the trie pointer (**the ROOT**), and NULL (**a transient re-home**) -- and a walk
can STOP at the trie and REFUSE on NULL instead of confusing the two.  The
silent truncation is gone; only an honest refusal remains.
☠ The `struct cds_ft_metadata::parent_word` comment still said "the only NULL a
reader can observe is at the root", which is how the dead refutation kept
propagating.  Corrected.

**MEASURED** -- byte-depth by walking `parent_word` to the trie, summing each
parent's `ft_node_span`, for every member of every bulk-op acquire:

    source                       contended        fine        deep
    ledger                  132,576 (27.7%)  154,578 (27.7%)   16/32
    descent WINDOW          214,267 (44.7%)  248,576 (44.6%)   24/32
    UP-WALK                 479,419 (100%)   557,732 (100%)    32/32
    refused (NULL) / capped        0 / 0          0 / 0        0 / 0
    cross-check vs window   agree 214,267    agree 248,576     agree 24
                            DISAGREE 0       DISAGREE 0        DISAGREE 0

★ **100% COVERAGE, AND IT AGREES EXACTLY WITH THE WINDOW** on all 462,843
members where both can answer.  The summed spans reconstruct precisely the
byte-depth the descent recorded -- which is what makes this a measurement rather
than a plausible-looking number.

⇒ **THE ENFORCEMENT PREDICATE IS UNBLOCKED**, and G5.20's "third step" (supply
real member depths at PER_NODE) is no longer needed for it: the up-walk needs
NEITHER a descent NOR a stored depth, so it sidesteps the whole
`ft_lock_ctx_depth_of`-manufactures-0 problem, and it works at every spacing.

☠ **THREE HONEST CAVEATS, none fatal:**
1. **`refused = 0` over ~1.04M members DOES NOT PROVE the transient-NULL window
   is unreachable** -- it is a rare event and a high-volume proxy cannot exclude
   one.  The predicate must still handle it, and refusing (re-plan) is the
   correct handling, so this is a cost question and not a correctness one.
2. **COST is O(depth) per member**, one metadata load per hop.  Paid only while a
   bulk op is live (gated) and only for a BULK op's own members -- the point side
   keeps the ledger, which gives the whole path in one pass.
3. **STALE BACK-EDGES.**  A back-pointer is lazily updated and can name a
   SUPERSEDED parent copy, so a walk could date against an old path.  The zero
   disagreement bounds this only where the window can also answer, and the window
   may be stale in the same way; it is NOT independently confirmed.

☞ **THIS ALSO WEAKENS -- BUT DOES NOT VOID -- THE LEDGER'S JUSTIFICATION**
(@`fb7e2ce5`).  The ledger is still the right source for the POINT side: it
yields the whole path once per descent, where an up-walk would be O(depth) PER
MEMBER on the hottest path.  What changes is that it is no longer the ONLY
possible source.

### G5.23 — ☠☠ DECLARE + ENFORCE + ESCALATE IS REFUTED (adversarial review, verified)

The up-walk unblocked the enforcement PREDICATE (G5.22), so the obvious next step
was: DECLARE a level from the op's key, ENFORCE it at the acquire with the
up-walk, ESCALATE (leave the gate, re-enter lower) on refusal.  **It does not
work**, on two independently fatal grounds, both verified in-tree:

**1. ☠ THE DECLARATION IS IN THE WRONG UNIT, and the op's OWN CORE LOCK SET
violates it before any climb.**  The ledger stores a node's **START** offset --
`ft_descent_enter_node(d, nf, start, len)` -> `ft_anc_ledger_push(nf, start)` --
and the widening compares against it (`e[w0].depth < min_lvl`).  A KEY LENGTH is
where the junction's key ENDS, so `start(J) = key_len - span(J) < key_len`: **the
junction itself falls below its own declared level**, and the slot-holding parent
every detach/graft/merge must lock starts lower still.  Under path compression
the gap is unbounded.  ⇒ the refusal fires STRUCTURALLY, on ~every op, not
exceptionally.  ★ This is the SECOND unit trap in the same area, after G5.18's
byte-depth-is-not-node-depth.
A sound declaration is `start(shallowest locked word)` -- which needs the descent
the gate PRECEDES.  That circularity is the real obstacle.

**2. ☠☠ ESCALATION IS IMPOSSIBLE PAST THE CONSUMED-SOURCE POINT, and that is
exactly where the climbs are.**  graft and merge have phases whose failure may
not unwind -- six sites, `goto retry_attach` / `retry_merge` with *"src consumed:
OOM is transient"* (`ft-graft.h:1754`, `:1786`, `:1976`; `ft-merge.h:2643`,
`:2671`, `:2714`).  Past that point the moved keys exist only in the consumed
source: the op cannot drop its locks and leave the gate, cannot lower in place (a
lowering owns a GP and the SEAM RULE forbids one under a held lock), and cannot
be refused.  Recompaction climbs happen in precisely that attach phase.  So where
enforcement matters most, the only outcomes are violating the declaration
silently or wedging.

**AND THREE MORE THAT WOULD EACH NEED SOLVING ANYWAY:**
3. **PHASE 1 IS NOT INERT.**  Publishing real levels into the PRODUCTION packed
   word with the widening ON and no enforcement makes point ops widen from
   `key_len` down and MISS J (defect 1) -- exclusion breaks SILENTLY, and the
   plan's own "re-take the A/B" step would come back GREEN AND WRONG.  Even with
   the widening off it changes the GP schedule: today every entry passes 0 so the
   lowering branch is DEAD, and heterogeneous levels make arrivals below the
   current min own a `synchronize_rcu()` that never ran before -- plus the exit
   rescan, 257 slots under the contended `move_gate_lock`.
   ⇒ a measurement must publish to a DEBUG SHADOW word, never the packed one.
4. **NO CARRIER FOR THE REFUSAL.**  `detach` has no retry loop and maps `ret < 0`
   to MEMORY_ERROR; graft/merge retry INSIDE the gate scope at the SAME level, so
   a structural refusal riding -EAGAIN is a deterministic livelock;
   `cds_ft_remove_all` has no retry loop at all.  And -EAGAIN's attribution
   already carries an explicit "every -EAGAIN here is a peer" contract that a
   refusal would re-break.  ESCALATE needs a NEW status plumbed through five ops'
   retry lanes plus a loop AROUND the scoped gate at all five entries.
5. **THE PREDICATE HAS BLIND SPOTS.**  `ft_insert_park_external_nodes` publishes
   `external_nodes` with NO lock (a documented hole), so a bulk body that parks
   externals mutates above its declaration through no acquire at all; and the
   empty-dst arms legitimately lock the ROOT, which guarantees refusal against any
   nonzero declaration on those shapes.

**☠ AND A CORRECTION TO G5.22.**  Its "0 disagreements over 462,843 members" is
agreement measured ONLY ON THE SAFE HALF: the cross-check runs where the WINDOW
can also answer, i.e. on near-path members whose back-edges the descent has just
walked.  The ~55% window-undatable, back-pointer-resolved members -- **exactly
where lazy and superseded back-edges live** -- get NO cross-check by
construction.  A stale edge mis-sums spans in the PASS direction (enforcement
passes what it should refuse: a silent exclusion gap).  Independent confirmation
would need an exclusive-trie oracle recomputing every depth from the root and
diffing, a per-hop re-parent epoch stamp, or an LTTng violation-abort on
up-walk-vs-fresh-descent mismatch.  ★ The up-walk's 100% COVERAGE stands; its
CORRECTNESS on the risk population does not.

☞ **WHAT SURVIVES:** the gate mechanism (packed word, one-load sample,
lowering-owns-a-GP); the up-walk as an INSTRUMENT; and escalation's termination
argument where it can run at all.  ⇒ **Option 1 is dead.**  The live candidates
are option 3 (LEVEL 0, what ships) and option 4 -- **re-open the PER-NODE MARK**
(candidate (E) tier 2, G5.6/G5.7), refuted on its WORD and not its PRINCIPLE, and
the only shape that asks the ANCESTOR rather than the member.  This session's
whole finding is that the information lives ancestor-side.

### G5.24 — ☑ LEVEL 0 ACCEPTED (Mathieu, 2026-08-29); the remaining gap is COVERAGE, and it is STRUCTURAL

Decision: **take the approach that needs no level ceiling, and accept the root
contention for now.**  That retires G5.16's fork -- every gate entry keeps
passing 0 -- and makes the widening's only open item its COVERAGE: 1.2%, because
only `ft_node_recompact` had a release owner wired.

**☑ THE OWNER NOW DEFAULTS TO `@txn`, and that is principled rather than
convenient.**  `ft_held_set::txn` is defined as *"the commit's lock registry"* --
the txn whose TERMINAL releases this op's locks.  A widened hold registered there
is given back by exactly the terminal that gives back every other word the op
took: a recorded `{LOCK|s -> s}` on commit, or the registry's CAS-clear on abort
/ memory error / a pre-commit bail.  Nothing about a widened hold wants a
different lifetime.  `@widen_txn` still overrides, for the sites where the two
DIFFER -- `ft_node_recompact`, whose owner is its `retire_txn` PARAMETER and sits
in no ctx frame at all.

**☠ AND IT BUYS ALMOST NOTHING, because the four remaining sites have NO TXN AT
ALL.**  With the site counter using the SAME predicate as `ft_widen_owner`
(fallback included -- a counter asking only about `@widen_txn` would answer for a
rule the code no longer follows):

    SITE                            live       led_ok    NO_OWNER
    ft_unchain_node      :4188  2,682,988  2,536,055   2,682,988
    _cds_ft_insert       :3329  2,523,199  2,497,840   2,523,199
    ft_node_recompact    :1380  1,494,210  1,467,123           0
    ft_insert_dlm_acq_sp : 771    412,232    411,144     412,232
    ft_chain_compress    :1028    388,610    386,318     388,610

**NO_OWNER == live at all four.**  Not "the wrong txn" -- NO txn in any frame.
⇒ G5.12's STRUCTURAL half is confirmed exactly: these sites ACQUIRE BEFORE THEIR
TXN EXISTS.  `_cds_ft_insert` builds its ctx with `ic.txn`, but
`ft_insert_commit_arm` arms it LATER, so the field is NULL at the acquire.

☠ **DO NOT READ THE GLOBAL `WIDENED` COUNT AS A DELTA.**  It varies 57,540 /
70,152 / 85,260 across identical builds -- concurrency-dependent, so a
before/after difference in it is NOISE.  The per-site `NO_OWNER == live` is the
result; the global count is not.

☞ **SO COVERAGE NEEDS PER-SITE RESTRUCTURING, one of two shapes:**
1. **ARM THE TXN BEFORE THE ACQUIRE.**  `ft_insert_commit_arm` sizes from
   `15 + anchored + count_edges`, computed after the descent, so this is a
   reordering with real constraints rather than a move.
2. **CARRY THE WIDENED HOLDS until the txn is armed**, then register + record.
   Needs a per-op holding area AND a release-on-bail path for every early return
   between acquire and arm -- the shape `ft_insert_dlm_release_parent` already
   implements for P.
☠ Neither is a one-liner, and (2) reintroduces the very "appended holds have no
release owner" hazard the choke-point design exists to avoid, merely moved.

### G5.25 — ☑☑ THE FT-WIDE LOCK, TAKEN ON THE REFCOUNT: same exclusion, none of the machinery

**Mathieu, 2026-08-29:** *"if we accept contention on root, then we can simply
add a ft wide lock.  the point ops take that lock when concurrent with a bulk.
the reference counter decides if this lock is needed.  no upwalk needed."*

★ **AND THE LOCK ALREADY EXISTS.**  `ft_writer_lock_scope_enter` DROPS the
FT-wide writer lock for a FINE trie -- unconditionally, all-at-once, since the
§11 drop.  So this is not a new mechanism but a one-line *un*-drop:

    if (ft->lock_fine &&
            !(FT_BULK_WIDE_LOCK && caa_unlikely(ft_bulk_active(ft))))
            return;                 /* steady state: still dropped */

⇒ **the entire remaining problem list evaporates**: no ancestor ledger to
consume, no up-walk to date members, no reservation to size, and -- the one that
actually blocked coverage -- **NO RELEASE-OWNER PROBLEM AT ALL**.  A scoped mutex
has no registry, so "the four sites acquire before their txn exists" (G5.24)
simply does not arise.
★ **AND IT QUEUES.**  The widened DLM acquire spins `URCU_TXN_WAIT_PATIENCE` and
then ABORTS -- the retry-storm hazard (E) inherited; `cds_fair_mutex` is FIFO.
★ **Seam rule: free.**  `ft_writer_lock_gp_wait` already DROPS this lock across
every grace period ("the GP always sits at a seam BETWEEN two distinct commits,
so releasing there costs no atomicity").
★ **The sample races nothing**: a point op that read the gate clear is inside the
read section `urcu_txn_begin` took, which is what the gate's publish-then-one-GP
waits for -- the same argument the widening uses.

**MEASURED, three reps per arm:**

    arm                    wide lock OFF          wide lock ON        ratio
    deep    (junction d=12) 885,578/801,744/823,702  95,148/93,144/98,570  8.66x
    shallow (junction d=1)   77,366/71,127            12,780/13,063        5.8x

☑☑ **WHERE THE TWO MECHANISMS ARE COMPARABLE THEY COST THE SAME.**  On the
SHALLOW arm the widening also costs 5.8x (G5.15) -- and it must, because there
"take every ancestor up to the root" IS a trie-wide lock.  The measurement agrees
to within noise.
☠ **DO NOT READ THE DEEP ARM AS "the widening is 6x cheaper".**  Its 1.48x was
measured at **1.2% COVERAGE** (G5.24) -- cheap largely because it barely fires,
against a wide lock giving COMPLETE exclusion.  The honest statement is
*complete bulk-vs-point exclusion costs 8.66x on the deep arm*; the widening has
never been measured complete, and cannot be until its four blocked sites are
restructured.
☞ The mechanism of the gap is nonetheless real and worth knowing: the widening
holds the root's DLM lock across the ACQUIRE COMMIT, while the FT-wide lock is
held across the WHOLE OP BODY -- descent, build and commit.  A complete widening
would still have the shorter critical section.

⇒ **THE TRADE, stated plainly:** the FT-wide lock buys the SAME exclusion the
widening was built for, at the same cost where comparable, with none of the
machinery and better liveness -- and it is COMPLETE today, which the widening is
not.  Its cost is a longer critical section, which a complete widening would
avoid at the price of the ledger, the owner plumbing and the abort-retry regime.

### G5.27 — ☑ THE WIDENING IS REMOVED (Mathieu, 2026-08-29)

`-DFT_FEATURE_WIDEN` and everything that existed only to serve it are DELETED:
`ft_widen_owner`, `ft_held_set::widen_txn` and its `holds()` arm, the widened
selection / take loop / release hand-off in `ft_dlm_acquire_set_at`, the
`nr_widen` accounting in its bound and heap split, `ft_node_recompact`'s chained
owner frame, and the `ft_wd_*` counters.  **309 lines out, 10 in.**

★ **KEPT AS A DEAD OPTION IT IS NOT.**  A flag that stays behind "in case" is a
second exclusion mechanism nobody exercises: it would rot against every later
change to the acquire, and the one thing this section establishes is that where
the two are COMPARABLE they cost the SAME (G5.25).  The FT-wide lock is the
exclusion; there is no second one.

☠ **AND THE PER-SITE `no_owner` COUNTER WENT WITH IT.**  With nothing to widen it
could only ever print 0, and a permanently-zero counter beside live ones reads as
"this class does not occur" rather than "this class is not measured" -- the
silent-zero trap this section has paid for more than once.

☞ **WHAT SURVIVES, and why it is not orphaned:**
* the BULK GATE and its packed `{refcount, level}` word -- `ft_bulk_active()` is
  what the FT-wide lock is gated on;
* the ANCESTOR LEDGER and its generation/trie stamp -- ☐ its only consumer was
  the widening, so it is now UNCONSUMED library code.  Kept for the moment
  because the `-DFT_DEBUG_WIDEN_OWNER` instrument still reads it and because its
  credential split documents a real hazard, but it is a candidate for removal.
* `_cds_ft_remove_locked`'s bulk-live anchor descent (@`3acd5488`) -- same
  status: it exists to fill the ledger.
☞ Removing those two is a SEPARATE call: they are landed library code with their
own correctness argument, and the instrument that proves the gate behaves is
built on them.

### G5.28 — ☑ THE BULK-LIVE ANCHOR DESCENT IS DROPPED (Mathieu, 2026-08-29)

`_cds_ft_remove_locked`'s third arm -- the descent that ran at PER_NODE when a
bulk op was live, purely to fill the ancestor ledger (@`3acd5488`) -- is REMOVED.
Its only consumer was the widening, and the widening is gone (G5.27), so it was
a full extra key descent per remove inside every bulk window, buying nothing.
The spacing gate returns to what it was: a descent runs for ANCHORING, and
per-node anchors on itself.

☑ **Measured back to the pre-fix picture, which is the point**: `no_descent`
returns to ~50% of in-window acquires (6,200 of 12,498), all attributed to
`_cds_ft_remove_locked`.  That is now a property of a ledger nobody reads, not a
gap in anything.

☠ **WHAT IS DELIBERATELY KEPT, and is NOT dead weight:**
* the LEDGER's `@anc_rec` / `@anc_gen` CREDENTIAL SPLIT ("may WRITE" vs "may
  READ") and `ft_anc_ledger_valid`.  The copied-descent corruption it closes is
  RARE again without this arm -- but NOT unreachable: the TOMBSTONE-RECOVERY arm
  sets `have_descent` at EVERY spacing, so a copy can still reach
  `ft_walk_extend` and append the orphan branch under the original generation.
  Removing the fix because its trigger got rarer is how a silent corruption
  comes back.
* `ft_walk_extend`'s corrected header -- it stopped being "a no-op under
  per-node granularity" when the ledger push was hoisted above that early
  return, and that is still true.
* the probe's no-descent ATTRIBUTION counters, which is how the number above is
  attributable at all.

☐ The LEDGER ITSELF now has no shipping consumer -- only
`-DFT_DEBUG_WIDEN_OWNER` reads it.  Left in place as the instrument's subject; a
separate call.

### G5.29 — ☑ THE LEDGER IS DEBUG-ONLY: it now depends on the flag that reads it

The ancestor ledger's only consumer was the widening; with that gone (G5.27) and
its filling descent gone (G5.28), it was live library code nobody read.  It is
now compiled out of the default build entirely, gated on the measurement flags
that use it:

    #if defined(FT_DEBUG_WIDEN_OWNER) || defined(FT_DEBUG_ANC_LEDGER)
    # define FT_ANC_LEDGER  1
    #endif

★ **AND IT WAS NOT FREE TO KEEP.**  The default build loses, per thread and per
op: ~4 KB of TLS (`FT_MAX_DEPTH` entries), a SHARED-WORD LOAD PER DESCENT
(`ft_descent_init`'s `ft_bulk_active` sample), a BRANCH PER LEVEL
(`ft_descent_enter_node`'s push test), and two fields off `struct ft_descent` --
which is a per-op STACK object.  All on the hot path, and POINT-OP SPEED is the
metric this transition is gated on, so "harmless if unused" was not true.
☑ Verified by symbol: `ft_anc_ledger` is ABSENT from the default
`liburcu-cds.so` and PRESENT in the measurement build, whose probe still
reports.

☞ `-DFT_DEBUG_ANC_LEDGER` is kept in the predicate deliberately: it COUNTS the
ledger (reach, overflow, disorder, copied pushes) without consuming it, so it
must be able to turn the ledger on by itself rather than depending on the probe.

☞ WHAT THIS LEAVES: the ledger, its `@anc_rec`/`@anc_gen` credential split and
`ft_anc_ledger_valid` all still exist and are still correct -- they are simply
DEBUG-ONLY.  If a future consumer wants the path again, it is one flag away, with
its hazard documentation intact.

### G5.30 — ☑ THE GATE'S LEVEL HALF IS REMOVED: its last consumer went with the widening

`bulk_state` was a PACKED `{refcount, min level}` word, and the level half
existed for one purpose: to tell a widening point op how far up to stop.  The
widening is gone (G5.27), so the half had no consumer at all --
`ft_bulk_min_level()` had ZERO call sites and `ft_bulk_sample()` had one, inside
`-DFT_DEBUG_WIDEN_OWNER`, which used only the boolean it returns.

★ **AND IT WAS NEVER EXERCISED EVEN BEFORE THAT.**  Every gate entry in the tree
passes level 0 (G5.24, decided), so `bulk_level_nr[0]` was the only slot ever
touched, and the "LOWERING owns a grace period" branch was UNREACHABLE -- nothing
can lower below 0.  What shipped was an exact-min occupancy array maintained for
a min that was structurally always 0.

Removed: `bulk_level_nr[FT_MAX_DEPTH + 1]`, `bulk_min_level`, the
`FT_BULK_LEVEL_*` packing and `ft_bulk_pack`, both level accessors, the lowering
GP branch, the exit rescan, and the `level` parameter of
`ft_bulk_gate_enter` / `ft_bulk_gate_exit` / `CDS_FT_SCOPED_BULK_GATE`.
`bulk_state` is now the published refcount and `ft_bulk_active` a plain `!= 0`.

☑ **MEASURED, and it is not a rounding error: `sizeof(struct cds_ft)` goes
864 -> 336 bytes, -61%** (the array alone was 516; alignment gives back the
rest).  The array sat at offset `0xf0`, between `bulk_state` (`0xe0`) and
`lock_fine` (`0x340`) -- two words `ft_writer_lock_scope_enter` reads on the same
call, which it held 608 bytes apart.  ☞ The layout consequence is UNMEASURED; the
size is the claim.

☞ THIS DOES NOT CLOSE THE CEILING.  The junction-level ceiling is still
UNMEASURED rather than refuted, and its banked design (G5.19) is TWO PACKED EPOCH
WORDS with a demand-driven flip -- which does not use this array, so nothing was
kept by keeping it.

### G5.26 — ☐ FOUND, NOT CHASED: a DEEP rekey is starved ~12,000x by point-op traffic, INSIDE one call

Isolating `inv_widen_deep_junction`'s movers to explain their tiny bulk-side
sample (n=32 members from 8 moves) turned up a liveness result that has nothing
to do with the widening -- it reproduces with `-DFT_FEATURE_WIDEN` OFF and
`-DFT_BULK_WIDE_LOCK=0`:

    8 movers ALONE (no point writers, no readers)     97,526 moves
    8 movers + 6 point writers + 8 readers                 8 moves

Exactly **8** -- one per mover -- stable across runs, with **0 refused**.  So the
movers are not being refused and re-planning: each is stuck INSIDE a single
`_cds_ft_debug_rekey_graft_simple` call for essentially the whole window, and the
starvation happens in the library's INTERNAL retry where no caller can see it.
Per mover that is ~12,190 moves alone against 1 under load.

☠ **AND IT IS INVISIBLE TO EVERY REFUSAL-BASED INSTRUMENT** -- an internal retry
that never returns produces no -EAGAIN, no abort, no counter.  It is the
[liveness lane]'s shape (`ft_remove`'s retry lane does not drain; the cure is in
the ACQUIRE) seen from the BULK side.
☞ Why it matters beyond the arm: it is what kept the deep arm's bulk-side
numbers at n=32, so several of this section's bulk-side fractions rest on a
sample that this starvation created.  Any future arm that wants bulk statistics
must fix this first or size around it.
☐ Not chased: it is a Phase D question, not a G5 one.

☞ **THE FORK THIS LEAVES**, and it needs a decision rather than a default:
1. **DECLARE + ENFORCE + RE-ENTER.**  The op declares from its key; an acquire
   ABOVE the declared level REFUSES; the op then drops its locks, leaves the
   gate, re-enters with a lower level and re-plans.  ★ It lands the cost on
   BULK-OP LATENCY -- rare, and already slow -- which is where G5.10's governing
   assumption says cost belongs, and it keeps point ops off the root in the
   common case.  ☠ It is a NEW MECHANISM (a bulk op that fails its own
   declaration), it costs a second grace period on every escalation, and the
   enforcement predicate needs a depth the PER_NODE spacing does not supply.
2. **DECLARE THE TRUE BOUND.**  Compute, before the gate, how far a climb could
   reach.  ☠ No cheap bound is known; the worst case is the root, which is what
   ships today.
3. **LEAVE LEVEL 0** and accept the 5.8x while any bulk op is live, betting on
   G5.10's assumption that bulk ops are INFREQUENT so the window is rare.  ☠
   That is the honest reading of "the coarsest first step", but it means the
   widening buys exclusion at a 5.8x point-op price whenever it is armed.

☞ **REVISED ORDER.**  (1) ☑ prerequisite 1 -- DONE; (2) the widening carrying
prerequisite 2, whose owner is a per-SITE txn (`retire_txn` at recompact) rather
than `ctx->held.txn`; (3) the liveness gate; (4) re-take the point-op baseline.
☠ Point-op steady state is untouched (one `caa_unlikely` load `ft_descent_init`
already pays); inside a bulk window a remove now pays one extra key descent,
which is what every coarse spacing and the tombstone recovery already pay, and
there is no cheaper correct source -- the back-pointer climb is refuted twice
over (absolute depth unrecoverable; back-edges lazily stale).

---

## 3. Phase A — arm COARSE, then exclusive

Order: **COARSE non-exclusive → exclusive**. COARSE first because the FT-wide
mutex is the simplest exclusion argument (every structural writer serializes;
cells are already forced MW via `ft_hlist_store_mw` /
`ft_ord_cell_record_into_ft`; with G2 landed the root is always-MW), and
because it is cheaply measurable end to end. Exclusive second (single writer
by contract; also covers teardown paths — note `ft->exclusive` switches
reclaim to `ft_flip_txn_call_rcu_now`, so the lone-SW no-GP asymmetry from G1
is most exposed here — G1's answer covers it, Mathieu 2026-08-23).

Arm predicates (fields verified at `a45869d2`: `bool exclusive`
fractal-trie-internal.h:1592, `bool lock_fine` :1747):

    A1  COARSE non-exclusive:   !ft->lock_fine && !ft->exclusive
    A2  + exclusive:            !ft->lock_fine || ft->exclusive
    B   FINE non-exclusive stays false at THIS switch — it arms per-op,
        from the held set, never from the trie (§4)

Note a cross-trie commit consults the switch for its NAMED trie while
recording a second trie's slots: with an exclusive consumed src (the BUSY
gate guarantees it), src-side slots are writer-excluded by exclusivity, so
the named-trie answer remains sound in both A1 and A2.

Per-step protocol (same for every arming step in this plan):

1. Flip the corresponding arm of `ft_txn_content_sw_ok` only.
2. Counter build (`build-tk`): MW_STRUCT must MOVE to SW at the armed sites —
   the positive-control pattern; a zero that appears without the armSW
   counter moving means the mechanism did not run, not that it is clean.
3. `--enable-rcu-debug` gate: the engine's nine `urcu_assert_debug`
   self-checks are the SW/MW kind-conflict detector; verify the detector is
   ARMED (the two-line TU probe) before trusting a green — a release build
   silently poisons the descriptor and the retry loop absorbs it.
4. ft_unit + ft_inv (FT_INV_MW=1) + the 4-concurrent-copies control run
   (§9.2) + ASAN + the fault-inject builds that reach the abort arms.
5. ABORT column, and ☠ **NOT against the 29% bound — that is an END-STATE
   number and reporting it per step manufactures a false win.** While the
   transition is incomplete the plan-wide total is dominated by the lanes
   this step did NOT convert: the FINE tries still recording all-MW, plus the
   acquire lane that is 71% of aborts and is never converted at all. So the
   step-local signal is buried under run-to-run spread. Measured on A1: one
   before/after pair reads as a 29% abort drop, and three alternated runs each
   way dissolve it into overlapping ranges (1.95–2.37% vs 1.82–2.39%). What
   the column IS good for per step:
   * a **wrong-direction alarm** — an SW park cannot fail, so mis-arming does
     not show up as an abort, but it often shows up as aborts RISING (an MW
     guard edge whose expected-old now mismatches the op's own mark). Cheap,
     keep it.
   * a **per-mode baseline**, read on the oracles that actually DRIVE the
     armed mode rather than on the plan total.
   Never report a plan-wide abort delta from a single before/after pair; n>=3
   alternated runs, or say nothing.

Risk to keep in front: **an SW park cannot fail** — a wrongly-armed park does
not abort, it silently erases a peer's committed edge. That is why each arm
is gated on its exclusion argument, and why step 3's detector must be proven
armed rather than assumed.

## 4. Phase B — FINE non-exclusive, per-op

Under FINE the promise is per-op, not trie-wide: **an op may park a word SW
only if its held lock-set OWNS that word.** Ownership per the §8 edge
principle: a parent→child edge's four fields are owned by the PARENT's lock;
a node's state word is owned by its own COPYING. The invariant proven at the
shared-junction oracle binds here: *a state word may be SW-parked only by the
txn holding that node's COPYING* — a recompaction's child edges stay MW
unless the op marks each child (the A3 lesson; the reverted lock-the-sweep
experiment must not be silently re-run).

### Step B0 — the mechanism — ☑ LANDED

`ft_txn_content_sw_ok` alone cannot answer a per-op question, so the answer
travels with the RECORD. Three pieces, all in place:

* **An `owner` on every SW-capable record helper**, compiler-enforced exactly
  as the CONTENT/ACQUIRE split was — `ft_flip_txn_record_tag`,
  `_record_reserved`, `_record_publish`, and per EDGE in `ft_pub_rec.owner[]`
  / `ft_ord_cell_edge.owner` (the `root[]` shape G2 landed, and for the same
  reason: the producer is the only place that knows). ☠ A bare `void **slot`
  cannot yield its owner — the owner differs by FIELD KIND (§8) and no
  arithmetic on the address recovers it.
* **The record-time assert** (`FT_OWNER_ASSERT_OWNED`, `--enable-rcu-debug`
  only), asking `ft_flip_txn_owns` of every SW-capable record on a txn that
  claims per-op ownership. Zero cost in a release build.
* **`ft_flip_txn_claim_per_op` — the CLAIM WITHOUT THE ARM**, and it is the
  tool the site steps below are run with. Converting a site asks two
  questions — "does the op own what it writes?" and "does parking it pay?" —
  and only the first can make the structure wrong. Claiming answers it with
  an abort AT the offending record on a build that is otherwise
  byte-identical to the unconverted one. It is also why the red control is
  sound: a control that ARMED would plant SW parks its txn never reserved
  for, and the engine's own kind / duplicate-slot self-checks would answer
  first — proving the engine detects a malformed descriptor, not that this
  check detects an unowned park.

Proven RED by `FT_RED_OWNER_CLAIM_ON_LOCK` (claim on the first
`ft_flip_txn_lock_register`): `ft_unit` aborts on its second test, and the
abort is this assert by name, not the engine's.

☠ `ft_flip_txn_owns` reads THE TXN REGISTRY, not `ft_held_set_snap` — a
record helper cannot see the op's `ft_held_set`, which lives on a stack frame
above it. A lock whose terminal this commit records must be registered on it
anyway, so a word owned by an unregistered lock is a finding; but it does
mean the numbers below are a LOWER BOUND on ownership. Exact at the default
per-node lock spacing; conservative above it (a coarse anchor is held while
the owner itself is absent).

### The readiness measurement — ☠ NO SITE CAN ARM YET

`OWN_HELD` / `OWN_LEDGER` / `OWN_MISS` (ft-txn-kind-stats.h) run the same
predicate on every record in EVERY mode, so a FINE **unarmed** run says whether
an arm would be legal BEFORE the arm is written. They split `MW_STRUCT` exactly
— the surface, by whether the op holds the word's owner — and their sum being
`MW_STRUCT` is an invariant of the table worth checking.

The three columns are the two WITNESSES of a hold, plus neither. `OWN_HELD` is
the txn's `locks[]` registry. `OWN_LEDGER` is `FEATURE_FT_HOLD_TRACE`'s
per-thread ledger, maintained at the lock PRIMITIVES so it sees a hold
whichever `ft_lock_ctx` frame filed it — a REGISTRY gap, not an exclusion gap.
Neither subsumes the other, so the held set is the UNION: the ledger drops its
entry the moment a release is *recorded* while the word keeps LOCK until that
commit lands, and there the registry is the only witness. ☠ `OWN_LEDGER` reads
a constant 0 without `-DFEATURE_FT_HOLD_TRACE`, and that zero is not evidence.

ft_inv, `FT_INV_MW=1`, 507 threads, per-node spacing, `build-ownwide`
(`--enable-rcu-debug CPPFLAGS="-DFT_DEBUG_TXN_KIND -DFEATURE_FT_HOLD_TRACE"`):

| creation site | MW_STRUCT | registry | ledger | owner-held |
|---|---|---|---|---|
| `ft-insert.h:776` — insert one-commit | 41,124,651 | 5,991,155 | 0 | **14.6%** |
| `ft-remove.h:2877` — remove commit_rec | 39,893,261 | 4,101,447 | 1,427,533 | **13.9%** |
| `ft-remove.h:3735` — head promote | 12,977,124 | 4,325,708 | 0 | **33.3%** |
| `ft-remove.h:882` — detach-side creator | 8,083,737 | 3,447,004 | 218,259 | **45.3%** |
| `ft-remove.h:3958` — unchain publish | 5,010,706 | 0 | 0 | **0.0%** |
| `ft-graft.h:3463` | 2,250,327 | 0 | 0 | **0.0%** |
| `ft-graft.h:1705` | 1,080,992 | 198,055 | 136,632 | 31.0% |
| **TOTAL** | **111,531,468** | **18,163,222** | **1,953,955** | **18.0%** |

☠ ft_inv is concurrent and its totals move a few percent run to run, so compare
COLUMNS WITHIN ONE RUN, never a percentage against an older run's.

**NOT ONE creation site is owner-complete, so not one of the five sites below
can arm as things stand** — the assert would fire at every one of them. That
is the finding, and it reorders the phase: the step per site is no longer
"arm it", it is *make it owner-complete, prove it with the claim dry-run,
then arm it*. The arm is the cheap half.

### ☑ THE PREDICATE WAS PART-BLIND — measured, and it is worth 1.7 points

`ft_flip_txn_owns` asked the txn registry only, and the claim dry-run's first
abort said that was too narrow: it landed in `ft_flip_txn_record_retire_anchored`
(reached from `ft_detach_node`) inside `if (h->lock == node || h->node_held)`
**and** `if (h->shared || h->node_held)` — both of which require the op to hold
`node`'s own word. Structurally proven, no debugging needed: read the branch the
frame is in.

Widening it to registry ∪ hold-ledger (`92e27199`) puts a number on it:
**1,953,955 of 111,531,468 records, 1.7 points**, all of it at
`ft-remove.h:2877` and `:882` and none at the insert site. The readiness
conclusion does not move — no site is owner-complete — so the remainder can now
be read as a REAL exclusion gap, which is what the site work needed.

☠☠ **AND ITS FIRST ANSWER WAS A ZERO THAT WAS AN INSTRUMENT BUG.**
`OWN_LEDGER` read 0 at every site over 110M records — not because the widening
buys nothing, but because `ft_flip_txn_record_retire_anchored` called
`ft_hold_trace_drop(node)` at the TOP, before planting the records that ask
whether the op owns `node`. The one class where out-of-registry holds actually
occur was erasing its own evidence. Two checks were needed and neither alone
sufficed: a RED CONTROL (force the query true → every MISS moves to LEDGER,
proving the wiring) and a LIVENESS PROBE (`return ft_hold_trace_n > 0` → 84.2M
of 85.5M records are planted with a NON-EMPTY ledger, proving the zero was about
*which* words it named). The red control alone would have "proved" the wiring
and left the bug standing.

### The external-head class — ☠ NOT one design question, and §8.2 is the OTHER doc

The 0.0% rows were recorded as ONE class, `FT_OWNER_NONE_EXTERNAL_HEAD` (11
sites) — an external head's back-channel word (`cell->parent`, `en->prev`,
`next_node->prev`) has no owning lock, because neither a cell nor an external
node carries a state word — and as "the largest single item in Phase B, a
design question, not plumbing", on the strength of *"§8.2 puts the entry list
under the HOLDER's lock"*.

☠ **THE CITATION IS TO `mw-writer-lock-escalation-model.md` §8.2
("Field-by-field ownership"), NOT to §8.2 of THIS document** (which is In-place
mutation, Phase F). That table is real and it answers more than the marker
claimed: `external_nodes` → **self (C)**, and — the line that matters for the
open fork — the `parent` pointer → **parent (P)**. ☞ Section numbers are reused
across the two design docs; say WHICH doc.

★★★★ **AND THE FORK IS NARROWER THAN "UNDECIDED" BECAUSE OF IT.** The
escalation model already assigns the parent pointer to the PARENT, while the
landed code keys the edge kind on holding the **child**
(`ft_flip_txn_record_parent_word`'s `@child_held`). So the design question is
not "pick a convention from scratch" — it is *the model and the code disagree,
and one of them must move*. That is a much cheaper question to put to Mathieu.

Read the three rows apart and they are three different things:

* **Head promote** (`ft-remove.h`, the `ft_promote_head` row). The holder was
  never missing: it is a PARAMETER (`held_holder`), `@head_slot` is the holder's
  own slot, and `ft_flip_txn_hold_or_lock_parent` REGISTERS it into this very
  txn. The only obstacle was ORDERING — the `&next_node->prev` record was
  planted before that call. Hoisting the acquire and naming
  `ft_flag_to_metadata(ft, parent_nf)` took the row from **0.0% to 33.3%**, and
  exactly so: 4,325,708 × 3 = 12,977,124, one owned back edge per two unowned
  publish edges. Landed; it was plumbing, not design.
* **Unchain publish** (`ft-remove.h:3958`) is NOT the external-head class at
  all. Its acquire is already ahead of the publish; its edges arrive through
  `struct ft_pub_rec`, whose `owner[3]` array **no producer ever fills**, so
  every one of them is NULL by default. `ft_pub_rec_add` has no `owner`
  parameter to fill it with. That is a PLUMBING item on the publish lane — add
  the parameter, name the owner at its four call sites — and it is the same
  two-thirds remainder the promote row still carries, so it is worth
  substantially more than this one row.
* **Back-edge re-parent of an external child** (`ft-graft.h:3463`, and the
  `ft-insert.h` / glue sites) is the REAL design question, and it is the
  SMALLEST of the three. `ft_reparent_record_meta` sets the convention —
  `owner = meta`, the CHILD's own word — and an external has none. Closing it
  is a CHOICE: give externals a state word (§8.1's layout split, Phase F), or
  change the convention so a back edge is owned by the HOLDER — which is what
  the escalation model's §8.2 already says for the `parent` pointer, against
  what the code does.

  ☑ **THE KIND IS SETTLED AHEAD OF THAT ANSWER, AT EVERY SITE** — `5fa631c7`
  ruled it and converted one; `4da182f2` converted the other seven records,
  behind one named helper (`ft_flip_txn_record_head_back_edge`). Whoever ends
  up owning the word, NO op can hold it today, so it is
  `ft_flip_txn_record_parent_word`'s permanent false arm and takes the
  always-MW `ft_flip_txn_record_root` treatment. The last one needed the
  `ft_pub_rec` detour removed rather than a kind swap — a rec's per-edge
  answers are `@root` and `@owner`, and neither can say "always MW" — which
  also retired `ft_pub_rec_add_back_edge` into `ft_record_child_back_edge`.
  What stays open is only OWNERSHIP, and it is Phase C's ledger to re-examine
  now that the traffic has left the MW_STRUCT surface.

☐ **Still unproven for all three, and it must not be skipped**: owner-AVAILABLE
is not owner-SUFFICIENT. Nobody has yet shown the holder's lock EXCLUDES every
writer of a chain member's `->prev` — an insert adding a duplicate, a
rekey/graft moving the head, `cds_ft_compact` relocating cells
(`ft-compact.h:403` writes `&head->prev`). The `ft-txn-hlist.h` `->prev` stores
are a SEPARATE lane, MW_ALWAYS by design and out of scope (the G4 decision).
Naming an owner only makes a site *eligible*; the exclusion argument is what
the arm needs, and it gets its own adversarial skeptic.

☞ **A NEIGHBOURING exclusion gap is now MEASURED and written up**:
`doc/design/ft-stale-disposal-predicate.md` (2026-09-04).  It does not close the
box above — its words are a holder's `external_nodes` and its body slots, not
`->prev` — but it settles the *shape* of the answer for those, in the form that
survives the transition: **every writer of those words already holds the node's
lock**, and the acquires are ordered by it (the second takes it 110 ns after the
first's commit released it).  The key is lost anyway because the disposal's PLAN
is read BEFORE the acquire and never again after — an **ORDER INVERSION, not a
missing arbitration**.  So the fix is lock-native (read the predicate under the
mark, or derive it there), it spends no reservation budget, it routes nothing to
the always-MW lane, and the shipping two-writer key loss does NOT need G4
decided first.

`FT_OWNER_UNPLUMBED` (1 site, `ft-compact.h`) is the other marker: the owner
exists and is simply not in scope. Both are greppable.

### Site order

Unchanged as an ordering, but each step is now
*owner-complete → claim → arm*, one site per step, one adversarial skeptic per
claimed exclusion argument:

1. `ft-insert.h` insert one-commit — ☑ **ARMED** `e2ba43f8`. (Was owner-complete
   at 14.3% → 40.1% held.)
   Its claim dry-run passes ft_unit AND ft_inv `FT_INV_MW=1` at 507 threads with
   no abort. NOT armed: the arm waits on 9.1, and its placement is an open API
   question (`ft_flip_txn_arm_per_op` has zero call sites and refuses an empty
   registry, yet kind dispatch happens at RECORD time). ☞ Read the DRY RUN as
   the readiness signal, not the percentage — the counter also prices records on
   txns the arm refuses outright.
2. `ft-remove.h` remove commit_rec — ☑ **OWNER-COMPLETE** `a9ff9549`, after ONE
   fix. Dry run (`-DFT_REMOVE_CLAIM`, `a3c75659`) started at ft_unit test **239**
   — the rekey writer's started at 112, and the difference is the classes closed
   there, which were shared machinery. The single item was an UNNAMED owner on
   `ft_remove_one_commit`'s structural edge: `b7334aa4`'s shape at a producer
   that builds its edge inline rather than through a rec. Fixed at the PRODUCER
   (`struct ft_remove_pub` gains `@slot_owner`, filled where both
   `ft_*_node_replace_ptr` arms already hold the node) rather than re-derived at
   the replay — this session paid twice for that shape.
   ☞ The dry run is now clean on ft_unit AND ft_inv `FT_INV_MW=1`, and it claims
   EARLIER than an arm would, so it covers strictly more records than an arm
   converts — the readiness is established for every arm point on this txn, not
   just the first.
   ☑ **IN-PLACE PUBLISH PATH ARMED** `6024f167`, after
   `ft_remove_one_commit`'s last register — correct, and it converts almost
   nothing.
   ☠☠ **AND THE "0.7%" WAS NOT A MEASUREMENT OF THAT ARM.** It was read off
   `armSW`, and that column cannot answer the question: `ft-txn-kind-stats`
   prices a site by TXN CREATION, so an arm that never RUNS and an arm that runs
   and is REFUSED report the same number, and a *different* site hand-arming the
   same txn (the rekey fold) reports as this site's `armSW`.
   `-DFT_B2_ARM_PROBE` (`19befc1f`) splits REACH from REFUSAL, and the real
   number is **10 reaches in a whole ft_inv run** — every
   one of that 34,685 was the fold's. The mechanism is not a defect in the arm:
   an in-place delete needs `ft_in_place_ok()`, which needs an EXCLUSIVE trie,
   which is a trie the per-op arm refuses outright — so on a shared trie EVERY
   delete recompacts (`ft_popcount_node_replace_ptr` returns `-EFBIG`) and only
   the external PROMOTE sub-case reaches that path at all.
   ★ **THE LESSON GENERALISES TO EVERY REMAINING B STEP**: `armSW` is not an
   arm's yield. Pair each arm with a REACH counter, or the next site's zero will
   read the same way.
   ☑ **RECOMPACTION REPUBLISH ARMED** `21b6b559` (both paths) — that is where
   the site commits. They do not share a last register:
   the FUSED path's is `ft_node_recompact`'s RELEASE half ({P}, +{GP} when P is
   compressed), planted inside `ft_node_replace_ptr`; the NON-FUSED path's is
   `ft_flip_txn_lock_or_guard_parent` on its own `else` arm. Refused on the FOLD
   path (`record_only`): there the txn is the CALLER's and the registry's
   completeness is the caller's judgement.
   Measured, WITHIN ONE RUN (ft_inv `FT_INV_MW=1`, 507 threads, per-node):
   `created 5,103,780 / armSW 4,000,657 / SW 3,252,905 / MW_STRUCT 9,827,650 /
   OK 3,999,992 / ABORT 665`, with the probe reading `in-place 10 armed 0 |
   republish A 1,318,159 all armed | republish B 2,801,578 all armed`.
   **`armSW` now tracks `OK` almost exactly**: essentially every committing txn
   at this site is armed.
   ☐ The REMAINDER is ~2.4 records per commit planted BEFORE the arm — the
   reparent sweep's held-child edges, the tombstones, the anchor releases, the
   orphan freezes. Converting those needs the op's ACQUIRES to finish earlier,
   which no arm can do from a publish site; that is Phase C/E, not B2's.
   ☠ A cross-run delta against the unarmed measurement is NOT sound — ft_inv's
   totals move run to run, so columns compare only within one run.
   ☞ `OWN_MISS` barely moves (1,377,786 → 1,405,328) and that is the expected
   shape: the counter prices records on txns the arm refuses outright, which the
   assert exempts via `!nr_locks`. Read the DRY RUN as the readiness signal.
3. The PUBLISH LANE — ☑ **LANDED** `6f54e698`: `ft_pub_rec_add` takes an `owner`
   and all four producers name it. Every such slot is a BODY slot, so the owner
   is the node the slot LIVES IN — which is not always `@parent_nf`, and
   `695d23c1` fixed the one caller that passes it for a different job.
   ☑ REMAINDER LANDED `b7334aa4`: the rec → `ft_ord_cell_edge` conversions
   copied `.root` and DROPPED `.owner` at all three (`ft_pub_rec_sedges`
   assigned `.root` TWICE, and the second of those was the owner), so every
   converted edge reached the engine owner-NULL however well the producer had
   named it. Byte-neutral in a release build — `ft_flip_txn_record_tag` reads
   `@owner` only for the assert and the counters, and parks on
   `@t->structural_sw` alone.
4. `ft-remove.h:882` `ft_chain_compress_fused` — the detach-side creator, and
   the largest CONTENT-LANE abort source. ☑ **ARMED** `ec00e234`, after
   `ft_detach_freeze_orphans` (this op's last register; under `lock_fine` the
   rest was taken in the ONE all-or-none acquire, and the incremental
   `lock_or_guard` beside the publish is the `!lock_fine` arm the helper
   refuses anyway). Refused on the FOLD path.
   ☑ Its dry run `-DFT_COLLAPSE_CLAIM` (`cec68299`) was **clean at first
   reading on BOTH suites** — no fixing pass at all, because the classes B2 paid
   for were shared machinery. `-DFT_ARM_REACH`: 1,645,902 reaches, 1,642,370
   armed (99.8%), only trie-wide refusals.
   ☠ **AND THE ABORT COLUMN NEEDED n=3, ALTERNATED.** One armed run read as a
   wrong-direction alarm (site aborts 5.69 → 6.74 per 100 created) and it did
   not survive. Alternated ctl/arm ×3, per 1k txns created at the site:
   `control 62.57 aborts / 2291.5 MW_STRUCT / 3.1 SW` vs
   `armed 55.44 / 1763.2 / 565.4`. ☞ The control's own spread (51.25 / 72.81 /
   63.64) is WIDER than the gap while the armed leg is tight (56.69 / 55.81 /
   53.82), so what this establishes is the ABSENCE of a wrong-direction signal,
   not a certified 11% ([[feedback_abort_column_is_not_a_per_step_number]]).
5. The EXTERNAL-HEAD lane — split by what the evidence shows, after an
   adversarial skeptic took the first framing apart in BOTH directions.
   ☑ **`ft_unchain_node`'s HEAD CLEAR — ARMED** `8cd52c64`. It never emits a SKIP_X
   dual, by ROUTING: `_ft_publish_to_parent`'s dual arm is gated on
   `ft_node_compressed(@parent_nf)`, and `_cds_ft_remove_locked` sends a
   COMPRESSED holder to `ft_detach_node` (no successor) or to the PROMOTE arm
   (one or more), so this branch only ever sees a NULL or INTERNAL parent. The
   branch comment naming "a compressed holder's SKIP_X dual" is STALE.
   ☞ The arm rests on a DETECTOR (`urcu_assert_debug(n_s < 2)`), not on that
   paragraph — a routing invariant is a code fact and code moves. Red control:
   inverted it does NOT fire on ft_unit (that suite never reaches the lane) and
   aborts 6× on ft_inv `FT_INV_MW=1`. Reach 2,346,297 / armed 2,346,297, ZERO
   refusals; the site goes 2.00 → 1.00 MW_STRUCT per txn (the residue is
   `hold_or_lock_parent`'s own release terminal, which cannot move above its
   own register). `ABORT` 0 before and after — fairness only, as expected.
   ☑ **`ft_promote_head`'s TWO ARMS — ARMED** `1c59dc9c`, after the per-edge
   `@owner_held` fix `af22756b` (option (a) of the fork).
   ☠☠ **THE GAP IT CLOSED, and the reason it was worse than a missing arm:**
   `_ft_publish_to_parent_meta` emits a **SKIP_X DUAL into the GRANDPARENT's
   body**, a node neither arm acquires — and `struct ft_pub_rec`'s claim that a
   NULL `@owner` leaves a record "never eligible for a per-op SW park" was a
   **STALE MECHANISM**: `ft_flip_txn_record_tag` dispatches on `structural_sw`
   ALONE, so an armed txn parks an unnamed slot SW and, without
   `--enable-rcu-debug`, silently.
   ☞ **THE FIX.** `@owner_held` is now a real per-edge word on `ft_pub_rec` and
   `ft_ord_cell_edge`, false by default, false ⇒ MW; carried across all three
   rec→edge conversions and honoured at all three replays. The FORWARD edge
   inherits the caller's `@slot_owner_nf` declaration (so every existing
   conversion is preserved); the DUAL's owner is DERIVED from a back-pointer, so
   `_ft_publish_to_parent{,_meta}` take `@dual_owner_held` and all **19** call
   sites state it, compiler-enforced.
   ☠☠ **AND EVERY ONE OF THEM NOW SAYS FALSE** (`46f33b7c`). The two
   `ft_detach_node` republishes briefly said `old_recompacted_node != NULL`, on
   the ground that "the recompact takes {C,P,GP} exactly when P is compressed".
   That is a **PLAN-TIME** fact: `pf_gp` is resolved BEFORE the acquire and GP
   enters the set only `if (pf_gp)`; an empty member is skipped by
   `if (!set[i].nf) continue` — its GUARD with it — while the dual's slot AND
   owner are derived FRESH at publish. A compressed P that was root-attached at
   plan time yields no GP and no guard, so a peer re-home in that window makes
   the publish derive a grandparent the op never acquired: an SW park on an
   unowned word, at an ARMED site, silent without `--enable-rcu-debug`.
   Unproven-reachable (no current op live-re-homes a root-attached compressed
   node) — **not a defence**. The parameter stays: it is where a publish-time
   answer belongs — the ACQUIRED GP compared against the DERIVED dual owner.
   ☞ Verified on the construction that found it: the public-API repro
   (`fractal-trie-review-2026-06/repro_listoff_promote_skipx_dual.c`) now
   SURVIVES both arms, and both dry runs are clean. Reach 4,847,315 + 115,511,
   ALL armed; the site goes 0% → 33.3% converted.
   ☠ **THE "CONVERSION FRACTION" EVIDENCE IS RETRACTED.** `!owner_held` routes
   through `ft_flip_txn_record_tag_mw`, which counts **MW_ALWAYS** — outside the
   conversion surface by design — so the dual population leaves BOTH numerator
   and denominator of `SW/(SW+MW_STRUCT)`, and the fraction can hold still while
   conversions are lost. The honest figure, per 1k txns created, ONE RUN per
   column (an estimate, not a certified delta): B2's `SW` goes 668 → 600 → 534
   across the mechanism and then the fail-close — a tenth to a fifth of its SW
   records, which is what closing the window above costs. `ABORT` at these sites
   is unchanged.
6. The remaining content sites in descending count.

★ **EVERY ARM FROM HERE CARRIES A REACH COUNTER.** `-DFT_ARM_REACH` lives in
`ft_flip_txn_arm_per_op` itself and tallies reach against each refusal term per
CALL SITE, because `armSW` cannot tell an arm that never RAN from one that ran
and was refused, and credits another frame's hand-arm to the site whose txn it
armed ([[feedback_armsw_is_not_an_arms_yield]]).

☠ A raw `ft_flip_txn_record_tag` loop over ordered-cell edges survives at
`ft-merge.h` and `ft-rekey.h` (the sibling graft path routes through the
tag-dispatching recorder precisely to avoid it). Sound today only because the
modes that arm exclude trie-wide; the per-edge `owner` stops it at the Phase B
arm, and routing the loop through the dispatching recorder is the real fix,
owed with those sites' arm.

Then retire the hand-arming at the rekey writer and root-COW driver onto the
same helper, so the switch has no bypass.

☠☠ **B6 IS NOT BLOCKED — the entry that said so was WRONG, and this is its
retraction.** It claimed the rekey writer's fences "reach the txn only as
RECORDS ... no `ft_flip_txn_lock_register` at all", and that registering them was
closed by the `FT_ENTRY_PER_NODE + 1` registry bound. Both false:

* `ft_rekey_marks_to_txn` (`ft-rekey.h:995`) loops `ft_flip_txn_lock_register`
  over every non-shared mark and is called at **:1836** (right after
  `ft_rekey_cow_stop`) and **:2684**. It landed at **`a91ebafd`** — an ANCESTOR
  of the commit that claimed it did not exist.
* Measured over a full ft_inv `FT_INV_MW=1` run: at the hand-arm point
  `nr_locks == 0` in 383,140/383,140, but immediately after the stop it is
  NONZERO in **285,294/285,294** (max 5) and at the commit in
  **123,689/123,689** (max 15) — against a bound of **257**. The overflow
  argument belongs to `ft_detach_freeze_one`'s orphan chain, a different op.

⇒ **B6 is a PLACEMENT problem, exactly like B1–B5**: the fences reach the
registry, just LATER than the first records — the register-before-record
ordering class already closed once as a 17-site sweep (`55c0350c`). The
"second witness" framing was invented to explain a refusal whose real cause is
that the hand-arm sits at txn CREATION, where every witness is empty.

**The shape of the fix** (small, and half of it is already built):
  1. `ft_rekey_cow_stop`'s `assert(txn->structural_sw)` (`ft-rekey.h:192`) is a
     DECLARATION, not an algorithmic dependency — `structural_sw` is consulted
     only per record at plant time — so it can move below the stop-fence take.
  2. Hoist the idempotent `ft_rekey_marks_to_txn` to the take, and put the one
     sanctioned `ft_flip_txn_arm_per_op(ft, txn)` there, inside `cow_stop`,
     which has both `ft` and `txn`.
  3. Port the same handover to `_cds_ft_debug_cow_replace_root`, which — unlike
     the writer — never adopted `ft_rekey_marks_to_txn`. **The two sites are NOT
     the same shape**, contrary to the retracted entry.
☞ COARSE / exclusive legs need nothing: `ft_txn_content_sw_ok` means the
constructor already armed them, and 39,420 of the measured arm reaches were
`lock_fine == 0` — there the hand-arm is redundant and the swap is a no-op. The
whole B6 residue is the FINE lane.
☑ **THE HOIST WAS IMPLEMENTED AND IT WORKS AT PER-NODE.** `ft_rekey_marks_to_txn`
at every mark-growth point in `ft_rekey_cow_stop`, `ft_flip_txn_arm_per_op` at
the stop take, the assert moved below it, the hand-arms retired at BOTH sites,
and the driver's sweep gaining the `!marks[i].txn_owned` clause registration
requires: ft_unit 315 ok / 3 deliberate, ft_inv `FT_INV_MW=1` 119/119, no
assertion — and the only `set_structural_sw` callers left were the constructor's
trie-wide arm and the helper itself. The predicted ordering findings DID appear
(a marked child's re-parent record ahead of its registration) and registering at
every growth point closed them.

☠☠ **THEN THE GATE KILLED IT AT THE OTHER TWO SPACINGS.** `txndbg`, `anchorval`
and `proxyassert` went RED at **exponential and root-only**, ft_unit dying after
109 tests. The mechanism is the arm's SPACING REFUSAL, not its placement:
`ft_flip_txn_arm_per_op` declines any spacing but per-node, so at a coarser FINE
spacing `structural_sw` stays false — and this writer **cannot run all-MW**.
`ft_reparent_record_meta` (`ft-mutation-helpers.h:9324-9326`): a MARKED child's
edge MUST stay SW, because "an MW edge expecting live_state would mismatch the
op's OWN mark and **abort every commit**". A LIVELOCK, not a slowdown — and
exactly what `assert(txn->structural_sw)` was guarding.

☠☠ **AND "BLOCKED ON PHASE E" IS ITSELF REFUTED — B6 IS LANDABLE NOW.** The
"two doors" argument conflated CALL SITES with POLICIES. The two doors B6 exists
to remove are the two HAND-ARM CALL SITES; a spacing dispatch *inside* the one
sanctioned arm point is ONE door, retires both hand-arms today, and is
byte-identical at every spacing. It is also exactly as sound as the status quo,
because it IS the status quo's policy behind one entry point.
☞ And the coarse spacings it covers are not shippable anyway:
`cds_ft_group_attr_set_lock_spacing` refuses exponential / root-only without
`FEATURE_FT_ANCHOR_VALIDATE` — "not a shippable configuration ... development
sweep" (`ft-lifecycle.h:355-375`). **Phase E is the prerequisite for CERTIFYING
the coarse SW fallback, not for consolidating the arm.**

☑☑ **B6 IS LANDED** `f8b6317f`, on the fourth attempt.  `ft_flip_txn_arm_structural`
is the ONE DOOR: it takes the sanctioned per-op arm and, when that refuses, says
SW explicitly.  Both hand-arms call it **at TXN CREATION** — the one point every
branch of the op passes — and no site outside the two arm helpers touches
`ft_flip_txn_set_structural_sw`.
☞ The per-op arm STAYS at `cow_stop`'s stop fence, additive: only a per-op arm
sets `@dbg_arm_per_op`, and at creation `nr_locks == 0` so it cannot.  Its marks
are registered first at every growth point (register-BEFORE-record), and the
driver's sweep gained `!marks[i].txn_owned` because registration TRANSFERS the
clear.
☞ `assert(txn->structural_sw)` now also sits before the writer's commit — the
choke point every branch crosses.
☠ The fallback line is **Phase E's debt**: a policy branch behind one entry
point, one line to certify or replace.
Verified: per-node ft_unit 315/3 deliberate, ft_inv `FT_INV_MW=1` 119/119,
per-op reach 269,738 / armed 228,109 (all 41,629 refusals trie-wide); the
**nocompress canary** back to 316/2, matching control; all 65 gate legs
identical.

★★★★★ **WHAT THE THREE FAILED ATTEMPTS TAUGHT**, and it generalises past B6:
  1. A CLAIM belongs at a choke point EVERY path crosses, even where the ARM
     cannot live.  Moving the assert with the arm made the un-armed branch the
     un-asserted branch — a deterministic livelock presented as a silent hang.
  2. Ask the LAST CONFIRMED mechanism whether it has another ROUTE before
     inventing a new one.  The livelock was already documented; only its REACH
     was open.
  3. `gdb -p` on a hang is the FIRST move.  A leaked fence stalls the NEXT op at
     an ACQUIRE; a livelock inside ONE op at COMMIT is a different signature.
  4. Gate legs are CONFIGS, not branches.  When exactly one config diverges, ask
     which BRANCH only that config drives (`nocompress` was the only leg driving
     the merge_dst fold).

☠☠ **ROOT CAUSE — the arm was moved onto a branch one caller never visits.**
Retiring the hand-arm at txn creation and putting the helper arm inside
`ft_rekey_cow_stop` leaves the **merge_dst** branch of
`ft_rekey_graft_simple_attempt` with NO arm point at all: `cow_stop` is called
only on `!merge_dst`.  That branch then runs all-MW, and its MW edges on the
state words the op ITSELF fenced expect `live_state`, read the op's OWN LOCK
marks, and abort the commit **deterministically — no peer needed**.  The retry
wrapper re-plans the identical shape forever.  It is the SAME livelock this
entry already documented, reached by a BRANCH-shaped route rather than a
spacing-shaped one.
Proof, from the hung process: `txn->structural_sw = false`, `merge_dst = true`,
`nr_marks = 0` (cow_stop never entered), commit returning `ABORT` repeatedly with
`nr_locks = 4` climbing fresh every round.  `nocompress` is the only gate leg
whose ft_unit drives that fold — with compression on, the shape gates route this
rekey elsewhere.  A COVERAGE ARTIFACT, the §9.5 class again.

☠ **THE `@txn_owned` SUSPECT IS REFUTED.**  No fence is leaked: `acquire_miss`
is false and `nr_locks` climbs fresh each retry (a leaked LOCK would fail the
next round's ACQUIRE, not its commit), and the release audit HOLDS — every
`goto sweep` in the writer is destroy-preceded, `bail_build` routes through
`ft_flip_txn_destroy`, commit-OK consumes registered marks via state edges,
commit-ABORT clears them via `ft_flip_txn_lock_release_all`, and both sweeps skip
`txn_owned`.  The registration half of the patch was always sound.

☑ **THE SMALLEST CORRECT LANDING** (verified: restores `nocompress` to
316 ok / 2 deliberate, byte-identical to control):
  1. Put the arm+fallback pair **at txn CREATION** in
     `ft_rekey_graft_simple_attempt` and in `_cds_ft_debug_cow_replace_root` —
     where the hand-arm was.  At creation `nr_locks == 0`, so the helper always
     declines and the FALLBACK carries correctness; this covers EVERY branch,
     merge_dst included.
  2. KEEP the arm-at-take inside `cow_stop` (idempotent).  At creation the helper
     can never CLAIM (the per-op owner assert needs `nr_locks > 0`), so the
     take-site arm is what buys B6's audit value on that path.
  3. Keep the registration at every growth point and both `txn_owned` sweep
     clauses — they closed the ordering findings and audit clean.
  4. RESTORE `assert(txn->structural_sw)` at `cow_stop`'s top; valid again at
     every spacing and every caller.
  5. **ADD `assert(txn->structural_sw)` immediately before the writer's
     `ft_flip_txn_commit`** — the commit is the one choke point EVERY branch
     crosses, and that assert would have turned this hang into a named abort.
  6. Optional: fold the pair into one `ft_flip_txn_arm_structural` helper so
     neither site has a naked `set_structural_sw`.
★ **THE LESSON THIS COST**: a CLAIM belongs at a choke point every path crosses,
even where the ARM cannot live.  Moving the assert WITH the arm made the
un-armed branch the un-asserted branch, which is why a livelock presented as a
silent hang.

☠ **THE RED CELLS WERE ALREADY RED AT HEAD, and this entry hid it.** On a clean
tree with the gate's anchorval flags, ft_unit dies at **294** (exponential) and
**238** (root-only), and ft_inv MW root-only after **73**, all on the engine's
`urcu_txn_record_chain: r->kind == kind` assert (`rcu-txn-mcas.h:879`) — the
§9.5 class-2 failures. The hoist did not turn those cells red; it died EARLIER
in them (110 vs 294/238). Those pre-existing reds gate any future coarse-cell
green and are NOT B6's.

☞ **THE all-MW LIVELOCK IS REAL BUT NARROWER than stated**: measured, all-MW is
outcome-identical to control at root-only and across the whole ft_unit surface
(every child acquire DEDUPES there), and livelocks only on ft_inv `FT_INV_MW=1`
at **exponential** — 7/119 tests in 480 s, with 112,271,789 child acquires taking
the child's OWN word against 527,664 dedupes. The assert is load-bearing at
exactly one measured spacing.

☞ Per-node measurements, corrected: the writer gives up **~0.6-0.8 records/txn,
~8-12%** of its structural records. Two identical runs gave 0.64/8.4% and
0.81/11.6%, so the single figure this entry first quoted was one draw from a
distribution as wide as its own precision. Reach/armed totals likewise vary ±25%
run to run; what is stable is the refusal SPLIT (all trie-wide, three zeros).

## 5. Phase C — the residual MW_ALWAYS lanes (the G4 decision)

### C.1 — THE RE-MEASURE (Phase B complete, B1-B6 all landed)

ft_inv `FT_INV_MW=1`, 507 threads, per-node, `--enable-rcu-debug`
`-DFT_DEBUG_TXN_KIND`, against §4's pre-Phase-B baseline. Normalised per 1k txns
created, because the two runs differ in size (94.9M vs 88.6M):

    lane          baseline      post-B6 |  base/1k  post/1k   delta
    SW           3,472,296   21,752,370 |     36.6    245.7   +571%
    MW_STRUCT  136,857,021   44,580,604 |   1442.1    503.4    -65%
    MW_ALWAYS   85,087,969  149,372,031 |    896.6   1686.9    +88%
    MW_LOCK     52,667,070   46,413,021 |    555.0    524.1     -6%
    VALIDATE    23,383,128   18,173,415 |    246.4    205.2    -17%
    cell/hlist  17,551,703   14,565,802 |    184.9    164.5    -11%
    ABORT        2,415,932    1,906,296 |     25.5     21.5    -15%

☑ **THE CONVERSION SURFACE IS DOWN 65%** and SW is up 6.7×. That is Phase B's
result, and it is large enough not to be run noise.

☠ **THE ABORT NUMBER IS NOT.** −15% is inside the ±40% run-to-run spread
measured on this column ([[feedback_abort_column_is_not_a_per_step_number]]), and
§4's own ceiling for the whole conversion was ~29% of aborts (the content lanes;
71% are the ACQUIRE lane, still 46.4M MW_LOCK records here). Read the direction,
not the figure. A defensible number needs n≥3 alternated against a control
build, which C should run before anything is claimed. ⇒ **RUN in C.1(c) below.
The −15% is superseded: the plan-wide column does not survive n=3, and the lane
the conversion actually touches does — by −34.5%.**

☠☠ **MW_ALWAYS NEARLY DOUBLED, and part of that is BOOKKEEPING, not traffic.**
`af22756b`'s per-edge `@owner_held` routes every unheld SKIP_X dual through
`ft_flip_txn_record_tag_mw`, which counts MW_ALWAYS — so edges that used to sit
in MW_STRUCT (or in SW, unsoundly) now land here. **The split between real
MW_ALWAYS traffic and reclassified duals is not measured, and G4 must not be
decided on this number until it is.** That is C's first task, and it is cheap:
count the dual edges at their producer. ⇒ **DONE in C.1(b) below — and the
suspect named in this paragraph turned out to be 0.4% of the column.**

### C.1(b) — MW_ALWAYS DECOMPOSED, and this plan's own suspect was WRONG

`ft_flip_txn_record_tag_mw` now takes a CLASS naming the population its caller
joins — required in the instrumented build, absent from every other one, so the
default `.text` is byte-identical (proved: same size, the only differing
instructions are 30 `mov $imm,%edx` assert `__LINE__` constants). Eleven record
branches, nine classes, and the classes SUM to the MW_ALWAYS column exactly —
that equality is printed as the table's self-check, so a future branch added
without a class announces itself instead of vanishing into a residue.

Two runs, same rig as C.1 (ft_inv `FT_INV_MW=1`, 507 threads, per-node,
`--enable-rcu-debug`), 119/119 both times, sum == column both times:

    class          run 2 records   share   run 1 share   fate
    HEAD_BACK         53,670,264   36.0%      36.2%      NEVER converts (no state word)
    PARENT_WORD       39,082,973   26.2%      25.7%      lock-set REACH  ⎫ the recompaction
    STATE             39,082,973   26.2%      25.8%*     a VALIDATE      ⎭ re-parent sweep
    CELL               8,435,084    5.7%       6.2%      ☞ THE G4 LANE
    ROOT               4,185,351    2.8%       3.0%      NEVER converts (no node to lock)
    PSO                4,033,026    2.7%       2.7%      §8.3 retires it
    DUAL                 526,699    0.4%       0.4%      ☠ THE SUSPECT ABOVE
    GUARD                127,955    0.1%        —*       a VALIDATE
    RANK                  19,582    0.0%       0.0%      Phase E (root-only spacing)
    sum              149,163,907             (* run 1 counted GUARD inside STATE)

**READ THE SHARES, NOT THE PER-1k.** Between these two runs the same build gave
1527/1k and 1776/1k for the same column while every share moved ≤0.5pp — so the
NORMALISER is the noisy part here, not the populations. Compare columns within
one run, exactly as the per-1k tables above already warn.

☠☠ **THE RECLASSIFICATION IS REAL AND IT IS THE WRONG COMMIT.** `af22756b`'s
duals are 0.4%. The reclassification that moved ~36% of this column out of
MW_STRUCT is **`f79438e7`** — it converted eight external-head back-edge sites
from `ft_flip_txn_record_reserved` (which counts MW_STRUCT on an unarmed txn) to
`ft_flip_txn_record_head_back_edge` (always MW). All three suspects
(`5fa631c7`, `f79438e7`, `af22756b`) landed AFTER §4's baseline was recorded, so
the whole HEAD_BACK population was MW_STRUCT in that baseline — 553-639 records
per 1k txns, against a total rise of ~630/1k. ⇒ The doubling is bookkeeping, as
this plan suspected, of a lane this plan had already ruled on.

☑☑ **THE LARGEST POPULATION IS THE RECOMPACTION RE-PARENT SWEEP** — PARENT_WORD
and STATE are the same records to the unit (39,082,973 each: two words per
re-parented child), 52.4% of MW_ALWAYS and **1.8× the whole remaining MW_STRUCT
conversion surface** (43.4M). This is the "B2 remainder" Phase B scoped out
because no arm at a publish site can reach it, now with a size. Its two halves
have DIFFERENT answers and that is why they are counted apart:

* STATE is a `{live -> live}` VALIDATE on a child the sweep does not hold, and
  it must stay MW — a park validates nothing. Marking those children instead
  was implemented in full and does not live (a contended child fails the
  acquire; escalation cannot rescue it — it holds its FIFO turn while spinning
  for a holder funnelled behind that turn).
* PARENT_WORD is a real STORE, and it is MW only because the sweep's acquire
  takes {C,P,(GP)} and never C's children. With PSO (2.7%) it is 28.9% of the
  column and it is §8.3's prize, quantified for the first time: parent-owned
  edge WORDS make both plain stores under the junction lock.

### C.1(c) — THE n≥3 ALTERNATED ABORT MEASUREMENT

Control `c8ce38c2` (the commit that recorded §4's baseline) against HEAD, both
`--enable-rcu-debug -DFT_DEBUG_TXN_KIND -O1`, ft_inv `FT_INV_MW=1`, 507 threads,
per-node, alternated ctl/arm ×3, SEQUENTIAL, 119/119 on all six.
☑ `test_urcu_ft_inv.c` is byte-identical between the two commits, so both legs
run the same workload — checked, not assumed.

Aborts per 1k txns created, split by lane at the txn CREATION SITE (an acquire
txn is one whose site records MW_LOCK and VALIDATE and nothing else):

    lane      control runs          armed runs         paired Δ       verdict
    TOTAL     24.86 22.58 18.22     19.81 17.72 14.43  −20.3/−21.5/−20.8  ☠ OVERLAP
    ACQUIRE   18.09 14.20 11.71     15.19 12.41 10.21  −16.0/−12.6/−12.8  ☠ OVERLAP
    CONTENT    6.77  8.38  6.51      4.62  5.31  4.22  −31.7/−36.6/−35.2  ☑ SEPARATED

☑☑ **THE CONTENT LANE IS −34.5%, AND IT SEPARATES COMPLETELY.** Every control
run's content-abort rate (min 6.51) is above every armed run's (max 5.31) — all
nine cross-comparisons agree, which at n=3 v 3 is the strongest rank result
available (p = 1/20 one-sided). The three paired deltas span 4.9pp. **This is
the conversion's own lane and this is its number.**

☠ **THE PLAN-WIDE COLUMN STILL DOES NOT SURVIVE, AND NOW THE REASON IS NAMED:
IT DRIFTS.** Both legs fall monotonically across the session — control 24.86 →
22.58 → 18.22, armed 19.81 → 17.72 → 14.43 — and the ACQUIRE lane carries the
whole drift (18.09 → 14.20 → 11.71). The CONTENT lane does NOT drift (6.77 →
8.38 → 6.51, non-monotone), which is exactly why it separates and the other two
do not. A paired Δ on a drifting column credits the arm with the drift, so
−20.9% plan-wide is NOT a result; −34.5% on the content lane is.
⇒ **Any future abort claim must be made PER LANE and paired within a session.**

☞ **AN UNEXPLAINED SECOND-ORDER EFFECT, AND IT IS NOT CLAIMED HERE.** The
ACQUIRE lane fell too (−13.8% paired, direction consistent in 3/3), and the
conversion does not touch it. The mechanism that would explain it is real: an
MW_STRUCT record on `&meta->state` (a fused count, a tombstone) is a CAS
contender on the very word the acquire's take arbitrates, so converting it to
an SW park removes a competitor from the acquire's own arbitration point — which
would mean §4's "~29% ceiling" UNDERSTATED the conversion's reach. It is a
hypothesis. It shares its column with the drift above, so it cannot be read off
this table; the test is a state-word-only conversion measured against the
acquire lane alone, and Phase D owns it.

☑ §4's ceiling holds as a share: CONTENT is 27/37/36% of all aborts on the
control and 23/30/29% armed.

### C.2 — G4's actual input

With MW_STRUCT at 503/1k, the residue is dominated by MW_ALWAYS (1687/1k) and
MW_LOCK (524/1k). MW_LOCK is the lock take and stays MW forever. So the G4
question — does the ordered-cell / hlist lane stay MW? — is now the largest open
lever, and `cell/hlist` (164/1k, unattributed by site) is only the part recorded
straight on the engine handle; the rest is inside MW_ALWAYS above.
☑ **MW_ALWAYS IS DECOMPOSED (C.1(b)), and G4's lane is now a number.** The
ordered-cell / duplicate-chain lane is CELL (8.4M inside MW_ALWAYS) plus the
`cell/hlist` line recorded straight on the engine handle (14.7M) =
**23.1M of 252.8M MW records, 9.1%** (run 1: 27.2M of 274.0M, 9.9%). Against
this section's own decision rule — "if the cell lane is minor, keep the narrow
MW lane" — 9% on the same order as §4's expectation (17.5M of 315M) is the
MINOR reading, and the mixed commit already backs a cell conflict out clean
before any SW side effect. ☞ **G4's remaining input is therefore the ABORT
attribution, not the record volume**: what fraction of aborts is the cell lane.
C.1(c) has now run the alternated measurement and split aborts ACQUIRE vs
CONTENT — but not CELL vs the rest of the content lane, and it cannot: the split
is done at the txn CREATION SITE, and a content txn carries cell and structural
records together. ☠ The obvious proxy — "aborts at sites that plant cell
records" — is a loose bound, not attribution
([[feedback_a_correlated_flag_is_not_attribution]]): the top content-abort sites
(ft-remove.h:881, ft-insert.h:866, ft-graft.h:3496) all plant both.
⇒ **The exact answer needs the LOSING RECORD's class**, which the engine has at
`t->recs[planted]` in `urcu_txn_desc_commit` and did not report. ☠ And the
proxy TAG cannot stand in for it: `FT_STATE_PROXY`, `FT_NR_KEYS_PROXY_TAG` and
`URCU_TXN_TAG` are all `1`, so the tag separates structural from everything
else and nothing more — checked. ⇒ **BUILT (Mathieu approved the engine touch):
C.1(d) below.**

### C.1(d) — ABORT ATTRIBUTED TO THE RECORD THAT LOST IT

`eaf883af` (engine, default-inert) + `9e263841` (the FT label). Same rig, two
runs, 119/119 both, and **the classes sum to the ABORT column exactly with ZERO
unattributed** — which is also the proof that the three engine exits cover every
abort, the lone-MW-edge fast path included.

    losing record's class    run 2 aborts   share   run 1 share
    MW_LOCK                     2,178,560   86.3%      78.1%     the lock take: losing IS its job
    mwa:STATE                     261,276   10.4%      15.2%     the re-parent sweep's §4.B validate
    MW_STRUCT                      61,347    2.4%       4.8%     ☠ see the alarm
    mwa:CELL                       22,463    0.9%       1.9%     ☞ THE G4 LANE
    VALIDATE                            —      —        0.0%
    mwa:PARENT_WORD                     —      —        0.0%

☑☑ **G4's LANE IS 0.9-1.9% OF ABORTS.** The ordered-cell lane is 6% of the
always-MW record volume and **~1% of what actually aborts**. Against C.2's own
decision rule — "if the cell lane is minor, keep the narrow MW lane" — this is
as minor as the rule can be given, and it closes the volume-vs-abort gap C.1(c)
left open. A per-cell lock would buy ~1% of aborts and cost a state word per
cell plus neighbour serialization. ⇒ **KEEP THE NARROW MW LANE.**

☠☠ **AND THE DETECTOR FIRED, WHICH IS THE REAL RESULT.** 60,727 of the 61,347
MW_STRUCT losses are records whose owner the op **HOLDS** (79,445 of 80,179 in
run 1) — and **every one of them is on a child POINTER slot**, zero on the
packed state word. That distinction is the whole finding: a state word has
lock-free writers by design, a child pointer slot may only be written by a
holder of the owning node's lock. Two runs agree, and it is concentrated at
**exactly two sites, both in the GRAFT lane**:

    ft-graft.h:3496 create    ptr 56,461 / state 0      (glue_insert.txn)
    ft-graft.h:3520 bounded   ptr  4,266 / state 0      (glue_publish_txn)

Every Phase B armed site reports **zero**. `ft_flip_txn_owns` is registry-only
with no trivially-true arm, so the hold is real
([[feedback_the_first_question_at_a_claim_abort_is_lock_fine]] cleared).

☞ **WHAT IT MEANS TODAY, AND WHAT IT DOES NOT.** Unarmed, the record is MW, the
CAS loses, the op retries, and nothing is published wrong — this is not a live
correctness bug. What it says is that **the graft lane must not be armed**: an
armed txn parks that word SW, and an SW park neither arbitrates nor is visible
to the peer that just won. The candidate causes are not yet separated —
a peer writing that slot without the lock, an `owner` naming the wrong node
([[feedback_a_raw_rederivation_at_the_write_site_is_a_class]], three known
instances), an expected-old captured before the acquire, or the cross-trie shape
(`glue_insert.txn` is created on `dst_ft`). 

### C.1(e) — THE CLAIM DUMP: the owner is CORRECT, and the question moves

`f2eaa3e1`. `-DFT_ABORT_CLAIM` prints, at the abort, every record in the
descriptor — class, kind, witness, {slot, old, new}, the slot's LIVE value, the
tag, the owner the record NAMED, its call site — with the loser marked, then the
op's LOCK REGISTRY, which the engine cannot see. Three independent samples,
identical shape:

    rec[0] MW_STRUCT MW/held slot=..0148 old=..1a1 new=..0e1 now=..081 tag=0xf  <== LOST
    rec[1] MW_STRUCT MW/held slot=..2f8  old=0x80004 new=0x4    now=0x80004 tag=0x1
    registry: nr_locks=1   held[0] meta=..2d8

`rec[1]` is the held node's own state word (`meta+0x20`, `offsetof(state)`
confirmed) — the `{LOCK|s -> s}` release. `rec[0]` is a child POINTER slot whose
value cycles among exactly **three** tagged pointers across samples: several
peers are writing that one word. addr2line through the inlined chain names it
exactly: `ft_glue_txn_commit_edges` → `ft_flip_txn_record_pub_rec` →
`ft_flip_txn_record_reserved` — the glue's FORWARD PUBLISH, whose owner is
`@publish_parent` by construction (`_ft_publish_to_parent` passes
`slot_owner_nf = parent_nf`).

☠☠ **AND THE "MISNAMED OWNER" EXPLANATION IS REFUTED.** The raw-re-derivation
class was the obvious suspect and the two addresses are 2 MB apart — but that is
an inference from an ADDRESS, and metadata and node bodies live in separate
arenas, so the gap says nothing
([[feedback_print_dont_debug_a_value_the_code_has]]). `ft_slot_in_node` is the
pairing test for exactly this question — its own header was written for it —
and counted at the publish it reads **in 1,231,998 / NOT-IN 0**. The slot IS one
of the named parent's own child slots, every time.

☞ **SO THE FINDING NARROWS RATHER THAN DISSOLVES**: the op holds P's DLM lock,
writes a child slot inside P, and a peer still wins that word. The remaining
question is about the **WINNER, not the loser** — who writes a child slot of P
without holding P — and it needs a different instrument (stamp the writer's
identity into the value, or trace the word). ☠ The two surviving candidates are
a peer path that writes that slot without taking the lock, and the CROSS-TRIE
shape (`cds_ft_graft_swap`'s glue txn is created on `dst_ft` while the oracle
`gs_shared_writer` swaps subtrees between two tries). Not separated.

★ METHOD NOTE, paid for twice in this section: a counter that reads 0 was quoted
here as refuting the forward-publish reading, and the call site had never been
added — two patch scripts aborted before reaching it. `in + NOT-IN` is a REACH
counter and it is what caught the mistake
([[feedback_verify_the_mechanism_ran_before_believing_a_zero]]).

☠ COVERAGE: ft_unit reports ZERO aborts, so the detector has none there. It
lives on ft_inv MW.

☠ **AND THE ABORT COLUMN'S SPREAD IS NOW MEASURED ON THIS BUILD**: the two runs
above are the same binary on the same rig and reported ABORT 2,981,880 and
1,514,561 — a factor of 2.0. Any abort claim smaller than that is noise.

### C.1(f) — THE WINNER NAMED: the alarm is the graft swap's own conflict
### detection, and the exclusion is INTACT

The winner-side instrument (`-DFT_WINNER_DBG`, on two new default-inert engine
hooks). The PRIMARY witness is the value the losing CAS **observed**, captured
by the engine at the loss itself (`URCU_TXN_REC_LOST` — the compare-exchange
returns that word and it had always been thrown away; any later re-read races
every subsequent writer). A proxy observed there IS the winning record,
decoded directly. A plain value is matched against a WINNER LEDGER — a global
slot-keyed table of the last engine write {label, the owner the writer named
(its own ring, its own thread), record call site, tid}, fed by
`URCU_TXN_REC_WROTE` after every engine slot store, try-claim only — and a
match is **corroboration, never proof** (ABA can name an older write of the
same value). The non-engine release-store lane (`ft_ord_cell_flip_one`, the
remove head-promote store) files raw notes so a raw winner is provable rather
than inferred from a miss; two loser-side probes check the re-home candidate
at the alarm itself (registered owner TOMBSTONED? `parent_word` NULL?).

**THE ANSWER** (ft_inv MW, 507 threads, per-node, 119/119; claim sample
agreeing, inside `inv_graft_swap_shared_dst_*`):

    alarms 81,859 (create 77,364 / bounded 4,495), ledger reach 505M stamps
    engine winner 81,859 = 100%, label MW_STRUCT witness=HELD 100%
    winner's named owner == the loser's 100%; self-tid 0; proxy 0
    raw-lane 0 / ledger-mismatch 0 / no-entry 0 / torn 0 / DRIFT 0
    loser's owner at the alarm: TOMBSTONED 0 / MID-RE-HOME 0
    claim sample ra: ft_glue_txn_commit_edges → ft_flip_txn_record_pub_rec
      — the glue FORWARD PUBLISH, the same site the LOSER records through

⇒ **The winner is a racing glue commit that held P's lock, published the same
graft-point slot, and released before the loser acquired.** Two ops cannot
hold one DLM lock at once, so the loser's expected-old predates its own
acquire — and it does, BY DESIGN: `@publish_old` is the PLAN-SNAPSHOT
expected-old `cds_ft_graft_swap` records precisely so a peer that swaps the
graft point between the descent and the commit turns into an abort +
re-descend instead of a publish over the peer's attach. DRIFT 0 over 81,859
alarms is the exclusion's own witness: once the loser holds P, nobody writes
that slot. Both surviving C.1(e) candidates are DEAD: no peer writes without
the lock, and the cross-trie / re-home shape never fired its probes.

**SO THE DETECTOR'S PREMISE GAINS ITS ONE EXCEPTION**: "an owner-held
structural record cannot lose" is true of a record whose expected-old was
read UNDER the held lock; the graft forward publish deliberately carries an
OLDER one. Not a bug — and the lane is still **NOT ARMABLE**, now with the
precise reason: an SW park does not CAS, so it cannot implement the
plan-snapshot conflict check; arming would ratify the stale plan, which is
the lost update `@publish_old` exists to prevent.

☞ FOLLOW-UP (small, optional): stamp records whose expected-old is a declared
plan snapshot (`publish_old_set`) with a label bit and EXEMPT them from the
alarm — the alarm then returns to being a true invariant (today it is 100%
this expected lane, and a real exclusion violation would drown in it).

☠ METHOD, paid twice in this section: (1) a value-match ledger WITHOUT the
raw lane hooked and WITHOUT the captured observed value gave the same headline
number — but only coincidentally, and it could not have defended it; (2) the
second attempt misfiled every winner as "raw" because
`snap.code >= FT_AB_CLS_NR` compared the FULL label, which carries the
ownership witness at bit 8 (`MW_STRUCT/held` = 0x102). ONE `-DFT_ABORT_CLAIM`
sample printing the fields the counter had already binned caught it — a
witness-bearing code must be compared by its CLASS FIELD, and a counter's
first reading should always be checked against a sample that prints what was
binned.


After Phase B, re-run the counter baseline. The decision input Mathieu asked
for is the granularity comparison: what fraction of remaining aborts and
retries is the CELL lane (vs the acquire lane), and what would a per-cell
lock serialize (adjacent-key ops share neighbors by construction)?

* If the cell lane is minor (expected: cells were 17.5M of 315M records):
  **keep the narrow MW lane** — the mixed commit already backs a cell
  conflict out clean before any SW side effect, which is the property a cell
  lock would buy, without a state word per cell or neighbor serialization.
* If it dominates: cells grow a state word and join lock-sets per the §9
  decision (cells are ordinary lock-set members in the same commit) — a
  larger, separately-planned workstream.

Dup-chain splices follow the same decision; rank propagation does not — its
unlocked-ancestor stores are only SW-able where the whole path is covered,
which is the root-only-spacing / rank-stats shape (Phase E folds that in).

## 6. Phase D — acquire-side liveness (parallel track)

The conversion does not touch the dominant abort source (71% acquire lane),
and the measured starvation lives there: a `cds_ft_remove` can lose 100+
attempts to a DLM acquire miss (159/179 never ran a commit); root cause is
lock-holder preemption on the hottest word (6,611 takes/100ms, median hold
160ns, the four longest holds each one involuntary context switch). Two
constraints are already proven and MUST bound any fix: a lock acquire cannot
ride the escalation lane (circular wait — structural), and plain backoff on
the acquire is REFUTED (median 40→61, worse tail). Aging-on-refusal is
landed and is the current best (4 starving removes vs 9.5).

Direction to design, with Mathieu: a FAIR HANDOFF on the contended word
itself (queue the acquire beside the engine, the way the COARSE lock is a
`cds_fair_mutex` beside the lane), or accept coarser spacing as the
mitigation on hot tries once Phase E certifies it. On QSBR the retry loop has
no quiescent window at all — any queued design must park offline, not spin.

### D.0/D.1 — the tail re-measured AND attributed at HEAD (2026-08-27)

Instrument: `-DFT_DEBUG_REMOVE_RETRY_CAP` (landed, byte-neutral by default;
the rekey cap detector's twin), with three per-op refusal classifiers.
Removes reaching 100 attempts, 8 sequential reps each, idle 192-core box:
`inv_prefix_key_park_vs_holder_churn` 7 8 5 9 11 8 9 5 (median 8);
`inv_writer_progress_chainmerge` 2 3 5 11 19 3 3 6 (median 4.5); the
1000-attempt milestone 0 of 17 runs.  ⇒ Phase B's −65% MW_STRUCT did NOT
dissolve the tail, and the enrolment fix's gain held (no 374-class runs).

☑☑ **THE ATTRIBUTION INVERTS THE DESIGN PREMISE.**  Of 61 victims at the
100-attempt milestone, 52 report `dirtyLOCK=100 dirtyOTHER=0 cabort=0` and 7
more `dirtyLOCK=99 cabort=1`: the victim's refusals are ~100% `ft_dlm_lock`
pre-checks OBSERVING THE WORD HELD, with zero-to-one set-commit losses.
Every victim is at the lane head (`in_fallback=1, active=1`).  So the
residual tail is NOT an arbitration loss among runnable contenders — with
the lane already silencing the crowd at begin(), the victim burns a full
re-descend+replan per attempt only to re-sample a word that is STILL HELD
across its whole episode (100 µs-scale attempts fit a single 119–380µs
preemption-stretched hold; on an idle box, chained/long RUNNING holds — the
08-20 p99 was 2.2µs, max 33µs — produce the same observation and the
hold-length distribution needs a re-capture to split the two).

**Consequences for the direction sentence above, all skeptic-verified
(adversarial review, 2026-08-27):**

* A RESERVATION / priority-token scheme (peers defer at begin) is REFUTED
  three ways: the defer point sits under held locks on COARSE/nested paths
  (inflating the very holds under attack); the claimant either honors
  `domain->active` and parks at the lane's TAIL exactly when starving, or is
  exempted and voids the lane's landed termination argument; and the
  arithmetic — a begin()-time defer is a ~2% attempt-rate shift placed
  µs upstream of the take, where flipping a streak would need a 10–30×
  cut.  D.1's data then moots the whole family: there is no crowd left to
  thin.
* A FAIR-HANDOFF QUEUE on the word survives the wait-shape objections only
  as GRANT-AT-RELEASE (nobody parks, nobody spins; the release terminal
  grants {LOCK|snap → LOCK|next}), but that holds the hottest word for an
  ABSENTEE winner across its re-descent (~100× duty-cycle inflation) and
  needs lease machinery for a winner that never returns.  D.1 moots this
  family too: the victim is not losing races at the release instant.
* What the data actually prices: the cost of ONE MORE SAMPLE of a held word
  is a full re-descent.  The un-refuted directions are (i) a BOUNDED linger
  at the refused word (µs-scale, online, at `ft_dlm_lock`'s pre-check — the
  inverse of the refuted backoff: the victim keeps its temporal position
  instead of unwinding; must be refused/capped where earlier frames are
  held), (ii) release-side notification (futex-class wake on release; a
  protocol change on the release terminal), (iii) rseq time-slice extension
  for holders (the preemption half only; the fair-mutex header already
  anticipates it), (iv) accept-and-bound (the cap as a loud detector; the
  tail is heavy but bounded on idle boxes).  Sizing (i)'s bound needs the
  CURRENT hold-length distribution: re-run the 08-20 LTTng dlm_take/drop
  capture at HEAD first (D.2).

### D.2 — the LTTng capture: the holds are LEGITIMATE BULK-OP TAILS (2026-08-27)

Mathieu named three candidate scenarios — (1) an acquire refusal forfeits the
FIFO turn, (2) descent/acquire under a NULL domain, (3) a bulk mutation
legitimately holds the lock long — and suspected (3).  The rig (committed
this time: `cds_ft:dlm_take/dlm_drop/dlm_long_hold/remove_anchor_starved`,
`442ff66f`) confirms (3) with the mechanism named:

    one snapshot window, full ft_inv, traced -O2 -DNDEBUG:
    546,877 takes; 19,025 holds ≥ 50µs — median 81µs, p90 122, p99 238
    86%  ft_node_recompact:1357   (then detach_node:2727,
         insert_dlm_acquire_split:771, detach_orphan_planlock:693,
         chain_compress_fused:1009)
    98.3% of long holds: nivcsw = 0 — the holder RAN the whole time
    99.8% of takes carry a domain-bound op (scenario 2 retired: 0.2%,
         all recompact:1357 / insert_split:771 — worth a look, not a cause)

Untraced churn victims agree (the 200µs word-sampler): the refused word is
held with `trans=0` across the whole window, take ages 160–535µs, takers
`ft_unchain_node:4147` / `_cds_ft_insert:3329` — i.e. a DLM hold lasts from
the acquire-set commit to the release terminal, the compound op's WHOLE
build+publish tail.  Scenario (1) is true by construction (the bail must
forfeit — the circular-wait rule) but secondary: with the lane parking every
enrolled peer, the victim's collisions are with these running holds, not
with a crowd.  ⇒ The 08-20 "lock-holder PREEMPTION" conclusion was the
under-load special case; the idle-box steady state is scenario (3).

### D.3 — the bounded-linger palliative, MEASURED (2026-08-27, @d3fe955a)

The retry-edge linger (spin on the refused word after the bail, nothing
held, 200µs cap) was skeptic-amended THREE ways before measuring — TLS word
cleared at every begin (a stale word is the refuted blind backoff), gated on
the ESCALATED state (aging is attempt-denominated), and the A/B run with the
attempt-triggered milestone compiled out (`-DFT_REMOVE_TAIL_QUIET`) since it
biases the control arm.  Clean rig (-O2 -DNDEBUG, wall-clock >1ms metric,
interleaved, sequential):

    churn      slow/run 136.5 → 87.5 (−36%); starved-class 71.5 → 38 (−47%)
    chainmerge NO separation (311 vs 258, overlapping, lin outliers above)
    max wall   ~10–20ms in BOTH arms — untouched
    ops        within spread (~−3%)

⇒ A CLASS-SPECIFIC palliative: it deflates attempts/CPU for the
stable-single-word victim class (churn's unchain/insert holds) and does
nothing for the rotating {C,P,GP} set class (chainmerge's recompact) —
exactly the skeptic's prediction.  No latency-tail win anywhere (sub-µs
inter-hold gaps cannot be captured by a µs wake-to-acquire path), and no
tail growth from the de-escalation window.  Landed DEFAULT-OFF; whether it
ever turns on is a separate decision from the curative work below.  The
~10ms stall class visible in both arms is unattributed — a candidate next
question.

### D.4 — the ~10ms stall class ROOT-CAUSED: the lane's 10ms poll quanta (@d41219c9)

The class D.3 left unattributed (removes >8ms, ~20% of the >1ms
population and its whole extreme tail, including attempts=0 ops, both
arms).  Decomposition put the wall in `urcu_txn_begin` (the domain lane)
or the bail's `end()`, clustered just above exactly 10ms.  The constant
is the adaptative-wait fallbacks: the fair mutex's granted-waiter
TEARDOWN wait (1000 cpu_relax then `poll(NULL,0,10)` — unwakeable BY
DESIGN, the granter must never touch the node after TEARDOWN) and
wfcqueue's `sync_next` busy-wait on the unlock side (same quantum).
Verified by per-thread poll counters: 6 victims ate the teardown poll,
17 the sync_next poll, each in its predicted column — and the ~446
others counted ZERO polls of their own, because the lane is FIFO: **one
thread sleeping its 10ms quantum mid-handshake convoys the whole
enrolled queue** (23 pollers × 10–13 queued writers ≈ the downstream
victims).  A µs-scale handshake race lost costs 10ms × queue depth.

☑ **FIXED @2d857e80 (Mathieu's design): the graduated ladder** —
busy-wait, then nanosleep 10→640µs doubling, then poll 1/2/4/8ms, then
16ms per rung.  A lost race now costs ~10–60µs typical (timer slack is
the floor), ≤2× overshoot, instead of a flat 10ms.  Measured: removes
>8ms 96→0, max wall 17–20ms→4.9ms on churn; test_fair_mutex green;
ft_unit/ft_inv smoke green; the full 64-leg gate's reds are exactly the
documented set, and a pre-fix CONTROL run of the txndbg config
reproduces its 15 legs identically, abort for abort.  Two recordings
from the verification: §9.5 class 2's signature is the engine's
`r->kind == kind` assert (rcu-txn-mcas.h:1000) via
`ft_flip_txn_guard_parent`, and the class covers the ft_inv root-only
legs too, not only ft_unit.  ☐ REMAINS OPEN: a small ~10ms residue on
chainmerge that is NOT this mechanism — body-located (inside
`_cds_ft_remove_locked`), zero rung-counter hits, tightly clustered
just above 10ms, convoying the lane secondarily (~4/run).  The next
unattributed constant.

Note the two classes compose: D.1–D.3's starvation is 1–8ms of
legitimate bulk holds; D.4 was the lane's own handoff machinery
quantizing rare µs races into 10ms convoys.

### D.5 — the ladder is a PUBLIC API, and the residue is closed (@027eeb9d)

Mathieu's call: the graduated delay lifted into `urcu/wait-ladder.h`
(header-only; schedule constants overridable; `rung_us`/`sleep_us`/
`clamp` primitives) and EVERY poll-for-delay site converted — wfcqueue
(hooks preserved, ms-only overriders keep their every-sleep contract),
fair-mutex, urcu-bp's GP re-check (clamped 8ms), workqueue pause
(clamped at the old 1ms — the loop spans fork) and RT idle (8ms),
compat-futex (POLL RUNGS ONLY: the function is documented
async-signal-safe and nanosleep is not on the POSIX list), and the four
DISTRUST trylock loops.  The adversarial review's amendments are all
folded in.  Verified: 64-leg gate IDENTICAL PER LEG to the ladder
baseline, fair-mutex/bp-torture/fork/FT suites green.

☑ **The chainmerge/churn in-body ~10ms residue is CLOSED as a
machine-load artifact** (@da868037): it fires only while concurrent
builds saturate the box, every code-owned wait column reads zero (GP
bracket: gp_calls=0 on 269+ slow removes — the remove body runs NO
grace period on these workloads; ladder rungs 0; arena waits 0 idle),
and its one discriminated instance is a single ~10ms deschedule
(nvcsw=1 nivcsw=1) convoying the lane.  The instrument's new
begin/body/bail + gp/arena/rusage columns make any future occurrence
self-attributing.  Under real oversubscription the mitigation is
scheduler-side (rseq slice extension for lane holders), not wait
policy — noted for Phase F.

**What this does to the fork above:** the starvation is priced by
(bulk-op hold time × recompaction rate), so the CURATIVE lever is
SHORTENING THE HOLD — which is §8.2 in-place mutation's exact target
(~130 structural records per recompaction commit, re-parenting every
child), now carrying a LIVENESS justification on top of its throughput
one; a build-outside-the-lock / flip-inside recompaction shape is the same
lever.  The victim-side BOUNDED LINGER (~1–2× median hold) remains the
cheap PALLIATIVE for the retry-storm cost (100 re-descents per episode),
and rseq slice extension addresses only the 1.7% preemption sliver.
G5(D)'s freeze-drain bound inherits the hold tail: p99 ~240µs + one GP.

## 7. Phase E — lift the lock-spacing gate

### E.0 — §9.5 CLASS 2 RESOLVED (@8e24cb1b): the first debt installment paid

The certification's standing blocker — the txndbg/anchorval aborts at
exponential/root-only, "pre-existing and unattributed, visible in every
gate diff" — root-caused from a deterministic repro and fixed.  An ARMED
txn's SW lock-release and `ft_flip_txn_guard_parent`'s MW validate
coincide on the same root-anchored word at coarse spacing; the engine's
kind police (`r->kind == kind`, rcu-txn-mcas.h:1000) refuses the (benign,
designed) mixed-kind fold.  Fix at the choke point, per the tree's own
rule ("a locked node needs NO separate guard_parent"): the guard is
skipped when `ft_flip_txn_owns()` says the registry holds the word, with
a descriptor-asked tombstone carve-out keeping a self-retire loud.  The
engine assert is untouched — it remains a real same-txn misownership
detector.  Gate: exactly the ten formerly-aborting legs improve (abrt→0,
complete runs), all other legs identical; ft_unit completes all 318
tests at both coarse spacings for the first time.

☐ Named residual (adversarial review, not yet observed): an armed SW
`nr_child` edge fusing with a guard on a word covered only via a
DIFFERENT anchor (no registry entry) would re-raise the assert;
`ft_flip_txn_guard_installed_child` shares the gap.  The E.3 sweep is
where it would surface.

The acquire-site conversion is complete and build-enforced; what remains is
certification, then the API gate lift (`ft-lifecycle.h:369`):

1. ☑ STALE ROW — `ft_compact_relocate_at` dates and anchors its lock set
   since `ff2c12da` (2026-08-09, before this doc was written): the walk
   builds a real `ft_lock_ctx` from its descent and passes the tracked
   depth.  Verified in-tree 2026-08-27.
2. ☑ LANDED @d8e42e5d — the owner-stamp oracle, MEMBER-keyed (anchor
   disagreement collides on the node itself), claim at the DLM choke /
   yield at the ledger drop, red control MOVED to `ft_dlm_lock` (the old
   one neutered the COLD fence primitive — why it only ever hung).  Red:
   full ft_inv dies in 0.58s, both owners named.  Green: every shipping
   config clean, MW per-node included.  Four ledger-lifetime lanes were
   fixed to get there (pre-commit drop, tolerant backstop, closing-hold
   and stale-plan filing skips, the op-init leak canary).
   ☑ **(A) RESOLVED @aa03d23b** as a protocol rule reached through seven
   measured rounds: *a frame entry answers holds() exactly as long as
   its word is unclaimable by peers* (LOCK/PROXY/TOMBSTONE or a release
   recorded in a live descriptor — any record shape — dedupe; released
   live-free — scrubbed at the sweeps and the consumption point,
   tombstone-guarded; record-less free — refuse through the aging
   path).  The GLUE's self-consultation lane is exempted (its accessory
   hold arms are not scrub-true; every enforcement form measured as a
   livelock) — ☐ finding A stays open for that one lane behind the GLUE
   HOLD-LIFECYCLE REDESIGN, which is plausibly the same work as (B)'s
   fence-under-anchor direction (one truthful-ownership redesign serves
   both; see fractal-trie-review-2026-06/FINDING_B_ANALYSIS_2026-08-27.md).
   Exponential MW runs 119/119 for the first time; the full gate's
   per-leg diff vs the E.0 baseline is exactly the nine holdtrace legs,
   all green.
   ☑ **(A) GLUE LANE CLOSED (2026-08-27 evening, redesign step 1,
   @ec9e68f8 + @e49f6c13)**: the exemption is DELETED and the glue lane
   runs ENFORCED — exponential MW 119/119 with 0 refusals, the config
   that refuted seven enforcement forms.  The livelock's root cause was
   never the arms' lifecycle: both ft_glue_record_splice filings dated
   their holder MERGE-RELATIVE (ft_merge_build's @depth counts from the
   merge point) while the fences dated absolutely — one op, two
   exclusion words for one node, the dst_base_depth comment's own
   documented hazard.  Fixed at the producer (@d_prov), plus an
   identity fast-path (ft_glue_fence_holds_node) so a mis-dated depth
   can never again split a node across two words, a HOLD_TRACE
   disagreement detector keeping the depth arithmetic continuously
   certified, and a CONSUMED lifecycle flag closing the fenced free
   list's post-reclaim answering (own-terminal token; skeptic-shaped
   gates on all three matchers).
   ☑ **(B) ROOT-CAUSED @19ade8f7** (LTTng flight recorder; the analysis
   note carries the interleaving).  NOT mark-vs-anchor dual coverage —
   both parties are DLM acquire-sets, the anchors AGREE, and the
   release drops by the key it filed by.  The defect is an op DEDUPING
   ON A WORD ITS OWN COMMIT ALREADY GAVE BACK: the ledger is drained
   pre-commit correctly, but the FRAME EXTRAS are not scrubbed when the
   commit consumes the hold, so the op's next acquire is told it still
   holds a word a peer has since taken for real, and both mutate under
   one word's protection.  ⇒ finding B is finding A's class at a
   consumption point the @aa03d23b scrubs missed; the remedy is the
   rekey fold's `marks_consumed` discipline generalised to every commit
   that consumes registered locks (R3).  D1/fence-under-anchor is
   UNNECESSARY and its evidence is withdrawn.
   ☑ **(B) FIXED (2026-08-28) — THE TERMINAL SCRUB**.  A txn's terminal is
   the only place that can retire the entry which HANDED it a word: the
   caller's sweeps deliberately skip a txn-owned entry, and the op's
   exclusion ends exactly there.  Each hand-off links a SILENCER into its
   `ft_flip_txn_lock` slot; `ft_flip_txn_scrub_owned` runs at the commit-OK
   arm (skipping slots whose terminal RETIRES the word — dedupe-on-dead is
   the designed flow) and at `ft_flip_txn_lock_release_all`.  ☠ Whether a
   terminal retires is CAPTURED AT THE RECORD: a load at the terminal races
   a peer's retire.  ☠ And it must be captured at EVERY recorder that can
   chain a retire onto an already-recorded release (`record_tombstone`'s RYW
   load IS that chaining) — one `mark_retiring` caller against several
   chaining recorders left the carve-out INVERTED, silencing an entry whose
   word ends dead, which turns the op's next dedupe into a take of a
   tombstone and hard-refuses forever.
   MEASURED, root-only MW, arm vs `-DFT_SCRUB_OFF` control, interleaved:
   arm 8/8 (and 4/4 after the skeptic fixes) at 119/119 with zero
   violations; control 8/8 reproducing.  Fisher p ~ 1e-4.  Green legs: inv
   119/119 and unit 315+3 at all three spacings.
   ★ **THE ORACLE IS THE BAR; THE DETECTORS ARE NOT.**  Its print and abort
   are unconditional, and an over-silencing scrub shows as NON-COMPLETION
   (re-acquire → hard refuse), which no report cap can hide.  The two STALE
   detectors cannot corroborate it on a scrubbed lane: the frame-extras
   detector fires inside `!extra[i].shared` and the scrub's silencer IS that
   `shared` flag, so its silence is ENTAILED by the fix compiling.  They
   remain evidence only for lanes the scrub does NOT reach.
   ☠ They were also, until 2026-08-28, ATTENUATED to the point of
   meaninglessness: the 200-report budget is per-thread and shared with the
   routine FT REFUSED line, which prints ~52,000 times and silenced ~270
   threads per root-only MW run.  The detectors now hold a SEPARATE budget
   and announce their own exhaustion; a zero is readable only when that
   sentinel is absent.
   ☑ **SCOPE**: root-only reproduces 8/8; exponential 0/20; per-node 0/6
   (all with the scrub OFF).  ☠ Read as BOUNDS, not as zeroes: 0/20 puts the
   exponential rate at ≤0.14/run (95%), which does NOT exclude the
   historical 0.125/run event, and the detector has no positive control at
   that spacing.  ☞ Per-node immunity continues to rest on the CENSUS
   (453,873 acquisitions, zero dedupes) and the structural argument that
   every member anchors on itself — 0/6 bounds only ≤0.39/run and is
   STRICTLY WEAKER than the census.  The earlier "coarse-only" wording was
   NOT wrong; it was a claim about dedupe existence, which this data does
   not contradict.
   ☞ **THE GLUE LANE**.  Counters per arm (hand-offs / consultations /
   scrub-saves) across both suites and builds: 262k hand-offs in one control
   run, consultations ONLY on `publish_parent` at root-only, ZERO
   scrub-saves.  ☠ What that licenses is narrow: no post-terminal
   consultation reached a STILL-ARMED fence.  It is not "never consulted
   after a terminal" — `ft_glue_txn_commit_edges` NULLs the holders BEFORE
   the terminal, so an ordinary-path post-terminal consult misses on
   identity and NO counter observes it.  The free-list lane carries NO
   silencer: a pointer into that malloc'd array is a use-after-free, several
   bails running `ft_glue_abort` (which frees it) before the txn's terminal.
   ☐ **THE DISPUTED EXPONENTIAL EVENT IS STILL UNATTRIBUTED**, and no null
   rehabilitates it.  A powered classified A/B (`-DFT_EXCL_REPORT_ONLY`),
   20 runs per arm interleaved, returned ZERO collisions in BOTH arms.  The
   fix shows no measurable cost, but 0-vs-0 BOUNDS rather than confirms
   (~0.15/run per arm, spanning the 0.125 observed).  ☠ Pooled, 0/40 against
   a build-invariant 0.125/run has probability 0.5% — the instrumented build
   most likely does NOT expose the phenomenon at the original build's rate,
   so "powered" overstates the power against the ORIGINAL event.  More runs
   will not settle it; it needs a different instrument or a deterministic
   trigger.  The earlier "pre-existing, safe to land over" attribution
   stays REFUTED.
   ☑☑ **E.2's COMPLETION CRITERION IS MET (2026-08-28)**: the gate's
   holdtrace config carries its `imw` legs again, at ALL THREE spacings --
   `ft_inv mw` 119/119 with zero aborts at per-node, exponential AND
   root-only.  Those legs were removed because the owner-stamp oracle
   failed by violation on findings A and B at the coarse spacings; both
   are now closed, and the oracle runs armed inside the matrix rather than
   being verified by hand.  ★ The full gate is otherwise PER-LEG IDENTICAL
   to the finding-A baseline (`gate-fa.log`, 178 legs), so the scrub, the
   growable registry and the freeze-lane closure regress nothing -- read
   the per-leg diff, never the verdict, which is deliberately FAIL on this
   branch for the three 9.1(B) rekey tests.
   ⇒ E.5's remaining blocker on the coarse spacings is no longer E.2.
3. ☑ FIRST-PASS CLEAN (2026-08-27, unblocked by E.0): both suites at BOTH
   exponential and root-only under `-DDEBUG_RCU -DFEATURE_FT_HOLD_TRACE`:
   zero SELF-COLLISION, zero asserts, 315+3/119 everywhere.  The zero is
   proven live: the collision-checking function's informational sibling
   fired 2,205 times in one leg alone.  Now CONTINUOUSLY certified: the
   gate matrix gained a swept `holdtrace` config (a collision aborts, so a
   red is a leg abort, not a grep).
4. ☑ BENCHED (2026-08-27; harness fractal-trie-review-2026-06/
   e4_spacing_bench.sh, writes/10s, 5 reps, sequential, idle box; the
   whole spacing matrix under FEATURE_FT_ANCHOR_VALIDATE for uniform
   overhead + a plain per-node reference; -W coarse driver option):

       load     plain-pn  av-pn    av-exp   av-ro    COARSE
       4r/4w     859,626  995,022  985,608  979,994    986,078
       32r/8w  1,044,316 1,054,858 1,086,384 1,033,456 1,181,500
       96r/24w   795,659  789,602  770,462  772,692    881,490

   TWO READINGS.  (i) The three spacings are WITHIN NOISE of each other
   at every load (≤5% spread, no consistent winner): the default stays
   per-node — the only certified spacing — and the choice costs nothing
   today.  (ii) COARSE beats every FINE config by 11–14% at the two
   concurrent loads: exactly §8.2's prediction that "FINE still COWs and
   the bench measures the wrong thing" — the FINE write path pays
   recompaction copy churn per publish that the wide lock never incurs.
   ⇒ the FINE-vs-COARSE sweep is NOT actionable until §8.2 in-place
   mutation lands, and step 5's alias question resolves NEGATIVE for
   now: root-only does NOT match COARSE, so LOCK_COARSE stays (neither
   alias nor retire).  Caveat: the plain-pn 4×4 cell ran first and reads
   ~14% under av-pn — a warm-up artifact; treat same-flag columns as the
   comparison, as designed.
5. Lift the `FEATURE_FT_ANCHOR_VALIDATE`-only refusal; then fold the
   redundant strategy: rank-stats coercion retargets ROOT_ONLY spacing, and
   LOCK_COARSE becomes an alias (or is retired) once root-only spacing
   matches it on the bench.

   ☞ **THE BLOCKER RE-DERIVED FROM CODE (2026-08-28), because "E.2 blocks
   E.5" had gone stale the moment E.2 completed.**  Step 4 already resolved
   the strategy fold NEGATIVE (root-only does not match COARSE), so only the
   refusal is live.  What stands against it, read out of the tree rather
   than off this doc's own status rows:

   * **α — the owner predicate is anchor-blind, and this is a VALUE blocker,
     not a soundness one.**  `ft_txn_per_op_spacing_ok` is literally
     `lock_spacing == PER_NODE`, and it gates EVERY Phase B per-op arm
     (`ft_flip_txn_arm_per_op_at`) and the dry-run claim.  Its own header
     gives the reason: `ft_flip_txn_owns` is EXACT at per-node and
     CONSERVATIVE above it -- a coarser spacing puts the word's lock on an
     anchor ANCESTOR which the registry holds while `@owner` is absent, so
     `owns()` reports a MISS for a word that IS excluded, and under an ARM a
     false miss is fatal (measured: txndbg/anchorval RED at exponential and
     root-only, ft_unit dying after 8 tests).  Resolving the anchor needs the
     op's DESCENT, which a record helper does not have.  ⇒ lifting the
     refusal today ships two spacings on which ALL of Phase B is inert:
     content records fall back to all-MW -- sound, stricter, and the
     pre-Phase-B engine.  With step 4 measuring the three spacings within
     noise, **the lift currently buys nothing**, which makes α the item that
     decides whether E.5 is worth doing at all rather than a chore blocking
     it.  The code names this exact debt at `ft_flip_txn_arm_structural`
     ("THE SECOND LINE IS PHASE E's DEBT") and again at the predicate
     ("Lifting the gate is Phase E's spacing certification").
   * **β — there was NO probe-free evidence at the coarse spacings.  ☑ NOW
     DISCHARGED.**  All four configs that swept the axis carried
     ANCHOR_VALIDATE or HOLD_TRACE, so the whole corpus was instrumented
     builds and the shipping shape had never run.  The `spacingenv` config
     (`-DFEATURE_FT_LOCK_SPACING_ENV` alone) is the control: 12 legs
     PER-LEG IDENTICAL to anchorval's twelve, the pair differing by exactly
     `-DDEBUG_RCU -DFEATURE_FT_ANCHOR_VALIDATE`.  ★ The knob was proven LIVE
     in the gate's own tree (`_cds_ft_group_create` resolves 1 / 3 / 2 / 1
     for unset / root-only / exponential / unrecognised) -- the sweep guard
     checks CPPFLAGS and cannot see whether the build honours the value.
     ☠ A green here is a CONTROL, not a certification: by α both coarse legs
     ran the all-MW path.
   * **γ — E.0's named residual is still ☐.**  An armed SW `nr_child` edge
     fusing with a guard on a word covered only via a DIFFERENT anchor;
     `ft_flip_txn_guard_installed_child` shares the gap.  This section said
     the E.3 sweep is where it would surface, and E.3 is clean -- but "not
     observed" is not "closed", and the class is coarse-spacing-specific by
     construction.
   * **δ — the disputed exponential event stays UNATTRIBUTED** (above), and
     it lives on one of the two spacings being lifted.

   ☠ **`FEATURE_FT_ANCHOR_COMPLETE` IS A DEAD FLAG.**  It appears in exactly
   one place in the tree -- `test_lifecycle_lock_spacing`, whose comment says
   it is what "claims they WORK" -- and NO build defines it, so that test's
   functional sweep over EXPONENTIAL and ROOT_ONLY is compiled out of every
   configuration.  The one test that would drive the coarse spacings through
   the PUBLIC SETTER has never run anywhere.  It cannot simply be defined:
   the setter refuses those spacings without ANCHOR_VALIDATE, so arming the
   flag and lifting the refusal are ONE step, and that step is E.5 itself.

## 8. Phase F — the payoffs (strictly last)

### 8.1 §8.3 layout split

Move the parent-owned fields (`parent_slot_offset`, `incoming_byte`) out of
the state/alloc words into their own parent-owned word. Under MW the sharing
costs only spurious aborts; under SW content it is what turns the remaining
CAS loops (`nr_child`, pso) into plain stores under the held lock, and it
retires the G5(A) fan locks: once the edge fields are parent-owned WORDS, the
junction lock alone covers a bulk op's whole written frontier. Approved
long ago but SEQUENCED: it lands only with the re-home rewrite that stops
relying on the one-MCAS-state-edge packing. It regressed the soak when tried
standalone — do not land it early.

### 8.2 In-place mutation

The DLM+SW engine's step 2: rank changes as plain stores + one selector
commit under the held lock (the SW variant of the txn bitmap — under the DLM
lock the MW bitmap's per-word MCAS is redundant CAS). Attacks recompaction
copy churn (~130 structural records per insert/remove commit is real: a
recompaction re-parents every child, two records each). Only after this
lands is the FINE-vs-COARSE throughput sweep meaningful — before it, FINE
still COWs and the bench measures the wrong thing.

### 8.3 Terminology + doc sweep

"lock-free" → "optimistic" is purged from new code but pervasive in older
comments and the design notes; sweep once the machinery stops moving.

### 8.4 The missing single-writer strategy — ☑ LANDED

Add the validation-only mode (caller guarantees one writer, library takes no
lock, `CDS_FT_SCOPED_WRITER` machinery already validates it). Under it,
content SW is trivially sound — it is the cheapest full consumer of this
whole conversion.  ☑ `f6ab074d` (`CDS_FT_WRITER_EXCL_CALLER`), renamed
`CDS_FT_WRITER_EXTERNAL_SYNC` in `d2543946`.

---

## 9. Cross-cutting blockers folded into the transition

### 9.1 The rekey writer's fine-lock completion

The three deliberate ft_unit failures (109/111/122) are the rekey decide's
missing fine-lock conversions (109's root cause and single-recompaction fold
are closed; 111 is measured at six gates with the arity floor now first; 122's
collision unlink is implemented but banked, the abut duplicate-slot fix
next). This thread OWNS the only armed SW content site — it stays the
conversion's live proving ground and should land ahead of Phase B's remove
sites, which share its machinery.

#### ☑ THE GLUE-FENCE TIMING ITEM — LANDED `6e77bab7`

Eight items closed (`695d23c1`, `d67851c6`, `bec0c726`, `280fdac1`,
`c6ac8c42`, `b7334aa4`, `f79438e7`, `6e77bab7`); the claim reaches ft_unit
**test 119** (was 113 at the start of the session).

The abort was `ft_chain_compress_fused`'s republish into `publish_parent`'s
body, reached through `ft_detach_node` from the rekey fold's detach. The record
named its owner correctly and the op HELD the word — `owner->state` carried
`FT_STATE_LOCK`, `glue.publish_gp_holder == owner`, `publish_gp_shared ==
false`, single-threaded. Only the WITNESS failed: the fence lived in the glue's
named field and reached `t->locks[]` at `ft_glue_txn_commit_edges`, after the
detach. The detach's own acquire of the word DEDUPED onto that glue witness and
so registered nothing (`ft-remove.h:1008` registers only `!held.shared`) —
rightly, since the earlier acquire owes the release.

Nothing was missing except WHEN the fence changes hands, so it changes hands at
the TAKE, with `@publish_gp_txn_owned` moving only the CLEAR
(`ft_glue_free_entry`'s `@holder_txn_owned`, applied to this fence). The FIELD
stays set — it is still the witness every narrow glue predicate reads.

☠ **TWO OBJECTIONS TO THAT HOIST WERE RAISED AND BOTH REFUTED** against the
code. Recorded because each is a plausible trap:

* *"Registering transfers the unlock, but `ft_glue_abort` still clears the fence
  on every bail, before `ft_flip_txn_destroy`'s strict release."* The ordering
  is real; the objection describes a register-WITHOUT-disown the idiom never
  writes. Every transfer in the tree pairs the register with disowning the clear
  (`ft-mutation-helpers.h:10440` / `:10484`, `ft-rekey.h:3226`'s
  `pp_meta = NULL`, `@holder_txn_owned` at `:7819`).
* *"A registration is sound only where the commit is guaranteed to record that
  word's terminal."* Contradicted two functions from the abort:
  `ft_chain_compress_register_retire` (`ft-remove.h:783`) registers at the
  ACQUIRE, with *"The tombstone itself is recorded later, at the commit."* And
  the companion worry — *"recording the release early is worse, a retire
  outranks"* — is backwards:
  `ft_flip_txn_record_anchor_release_held`'s header puts the release half AT the
  register, *"rather than at the commit"*, and says *"the later retire chains
  onto the release's clean pending value"*. Only the OTHER order needs the yield,
  which is what its RYW self-guard is for.

☠ **AND WIDENING `ft_flip_txn_owns` TO CONSULT THE GLUE IS REJECTED**, not
merely bigger. It already refused the wider witness in writing
(`ft-mutation-helpers.h:2192`), and it has a hole the registry does not:
`ft_glue_held_snap_one`'s `caller_holder` arm (`:9424`) returns held with
`ratified = false` for a fence whose RELEASE belongs to an OUTER frame — an SW
park on a word this commit does not own the outcome of. B0 already carries the
wide answer as a deliberately separate category: `FT_TK_COUNT_OWN`
(ft-txn-kind-stats.h:466) is three-way, registry-first, `ft_hold_trace_holds` =
`OWN_LEDGER`. Widening would erase exactly that distinction.

#### ☑ TWO MORE CLOSED — the same class, three instances now

    d4b4d2d7  the glue free-list retire's anchor: register, and BEFORE the record
    05356918  the glue's PUBLISH PARENT fence, handed over at the take

`d4b4d2d7` was the ordering class again, twice over. `ft_glue_tombstone_free_list`
recorded a fused `{LOCK|s -> TOMBSTONE|s}` whose owner is the retired node, then
registered the anchor BELOW that record — and only when the anchor was a
different word. Under per-node the anchor IS the node always, so the common case
registered nothing. The `h.lock != meta` gate was about SAFETY (a SURVIVING
anchor must leave `ft_glue_clear_fenced`, a tombstoned one cannot be stolen), not
about the registry, and `ft_chain_compress_register_retire` already registers the
same retires with no such gate.

`05356918` is `6e77bab7`'s sibling: a fold whose publish parent is PLAIN takes no
SKIP_X dual and therefore no GP fence, so the word the detach republishes into is
the publish parent itself. The rule now lives once, in
`ft_glue_take_publish_parent` — this holder has FIVE producers across rekey /
merge / graft, and a rule spread over five assignment triples is one a sixth
producer will not know about. The two rekey-fold takes adopt it; merge and graft
keep today's behaviour until their own sites are converted.

☠ **THE CLAIM'S ABORT NUMBER CAN GO DOWN ON A CORRECT FIX, AND IT DID** — 120 →
112 at `05356918`. `FT_OWNER_ASSERT_OWNED` refuses on `!t->nr_locks` ("this
commit owns nothing, so do not check"), so registering a lock SOONER puts records
that previously escaped the check under it. Read a backwards jump as coverage
gained, not as a regression, and confirm it by identifying the newly-exposed
record — never by reverting on the number alone.

#### ☑ THE WITNESS CLASS IS CLOSED — four instances

    6e77bab7  the glue's SKIP_X GRANDPARENT fence      (take -> txn)
    d4b4d2d7  the glue FREE-LIST retire's anchor       (register, and BEFORE the record)
    05356918  the glue's PUBLISH PARENT fence          (take -> txn, one shared helper)
    bf9c490c  the glue's per-node RE-PARENT MARK       (register; @marked stays false)

All four were one shape: *the op HOLDS the word, a witness that is not the txn
registry knows it, and the record-time owner check reads `t->locks[]` alone.*
Recognise it in gdb on a single-threaded run — `owner->state & FT_STATE_LOCK`
set, and `owner` findable in some other witness. Three of the four had a
carve-out saying the registry was unnecessary, and each of those carve-outs was
about who CLEARS the mark, never about whether the commit owns the word.

☠ **THE CLAIM'S ABORT NUMBER CAN GO DOWN ON A CORRECT FIX**, and it did: 120 →
112 at `05356918`. `FT_OWNER_ASSERT_OWNED` refuses on `!t->nr_locks` ("this
commit owns nothing, so do not check"), so registering a lock SOONER puts
records that previously escaped the check under it. Read a backwards jump as
coverage gained; confirm it by identifying the newly-exposed record, never by
reverting on the number.

☐ **ONE BOUND IS OWED.** `bf9c490c` registers one entry per deferred child, and
`FT_FLIP_TXN_MAX_LOCKS` (`FT_ENTRY_PER_NODE + 1` = 257) was sized on exactly
that set — *"the widest such set is a node's children plus the node itself"*.
Measured with `-DFT_LOCKS_HIGHWATER` (new; prints at each new maximum,
immediately, because `atexit` does not run on `abort`): **15 / 257** on ft_inv
`FT_INV_MW=1` at all three spacings, 13 on ft_unit. Seventeen-fold headroom on
the suite as it stands; the worst case — a glue whose deferred list is one
node's whole fan-out, plus its publish parent — is NOT proven, and ☠ the guard
is a plain `assert`, so an overflow is silent under `NDEBUG`.

#### ☑ THE SKIP_X DUAL — LANDED `ebf24686`, and the claim jumped 125 → 315

The dual lives in the compressed parent's OWN parent, and the publish resolves
that parent READ-YOUR-OWN-WRITES. So when the SAME commit re-parents the
compressed node, the dual's home MOVES — off the live node the descent fenced,
onto the fresh copy this op built. Verified rather than inferred:
`skip_owner_nf == detach_rc.new_flag` exactly, with the gate's dual slot inside
`detach_rc.old_node`.

A fresh copy is BUILD-INVISIBLE, so no owner encoding can make a record into it
legal — `bec0c726`'s rule applies: the txn publishes REACHABILITY, not INTERIORS.
The sink now dispatches per edge; a re-homed home takes the plain-store arm that
already existed for the `rec == NULL` case. ☠ DEMOTED, never DROPPED: the copy is
born holding the skip pointer to the OLD child, stale the instant the forward
publish lands. The test asks the DESCRIPTOR (`urcu_txn_find`), because
`urcu_txn_load` falls through to a fresh read and cannot tell "not re-homed" from
"no record".

☠☠ **THREE READINGS, TWO WRONG, AND BOTH WRONG ONES CAME FROM AN ADDRESS.**
First "the SPLIT arm never holds GP" (no — `merge_dst == true`, the MERGE arm,
which does reach the gate). Then "the dual lands in `g->top`", from *"absent from
`g->built[]`, therefore live"* — and that premise is false: `ft_glue_track`
inventories only what the GLUE builds, while `struct ft_detach_recompact_out`
(ft-remove.h:1407), the collapse reclaim and the orphan chain carry the rest.
**Three abort-free inventories, not one.** Printing `detach_rc.new_flag` settled
in one command what three rounds of address arithmetic did not.

#### ☑ THE FIFTH WITNESS — LANDED `a91ebafd`, and **ft_unit's claim is CLEAN**

`ft_rekey_cow_stop`'s fences live in a fn-scope array rather than a lock-SET, and
rightly: it reaches 17 anchors on the unit fixture, while a SET is the unit the
MCAS install sorts for deadlock-free acquisition. But *"not a set"* was read as
*"not in the registry either"*, and the registry answers a different question —
it is what a RECORD-time owner check can see. Every later acquire of those words
dedupes against the array through `@held.extra` and correctly registers nothing
of its own, so the registry never learned them by any route, while the re-parent
records those marks exist to license named them as owner.

`ft_rekey_marks_to_txn` registers at the TAKE and sets `@txn_owned` — the field
`ft_held_anchor` has carried all along for exactly this handover — and the
post-abort sweep skips those entries. The release RECORDS stay put; their own
comment already called that placement order-independent. What was never
order-independent is the REGISTRATION.

★★★★★ **`-DFT_REKEY_CLAIM` NOW PASSES ft_unit ENTIRELY: 315 ok, 3 deliberate, NO
ABORT.** The walk started this session at test 112.

Registry high-water (`-DFT_LOCKS_HIGHWATER`): unchanged at **15/257** (ft_inv MW)
and 13/257 (ft_unit) — the marks dedupe or peak below the existing maximum, so
the bound is no more stressed than before.

#### ☠ THE "REAL EXCLUSION GAP" ABOVE WAS AN INSTRUMENT ARTIFACT — `67f72278`

The previous revision of this row reported `ft_store_at_graft_point_commit`'s
relocation republish as an op writing a live node it never fenced: unlocked
owner, `acquire_miss == false`, absent from a populated registry, every fresh
inventory empty. All true. **`ft->lock_fine` was `false`.**

On a COARSE (or exclusive) trie the exclusion is the FT-wide mutex —
`ft_rekey_graft_simple_locked` takes `CDS_FT_SCOPED_WRITER` — and the recompact's
DLM acquire block is gated on `lock_fine`, so P is correctly never locked and
never registered. The registry is still non-empty (retire anchors, cow_stop
marks), so the assert's `!nr_locks` escape does not fire, and the first record
whose owner coarse mode never marks aborts.

`658989ef` reached for the raw `ft_flip_txn_claim_per_op`;
`ft_flip_txn_claim_per_op_armable` exists for exactly this and says so in its own
doc block, and `6f55710b` had already taught B1's dry run to use it. `67f72278`
gates the rekey claim the same way. Diagnostic-only — the hunk is inside
`#ifdef FT_REKEY_CLAIM`, which no shipped or gate configuration defines.

☠☠ **THE FIRST QUESTION AT ANY CLAIM ABORT IS `p ft->lock_fine`** (and
`ft->exclusive`, and the spacing). It costs one gdb line. Skipping it cost a full
misdiagnosis and a doc revision that had to be retracted. ☞ And read a
suite split — "ft_unit clean, only ft_inv aborts" — as a COVERAGE question
first: ft_inv has dedicated COARSE rekey arms, ft_unit's rekeys run on FINE
tries.

#### ☑ §9.1(A) IS COMPLETE — `64085ea3`. The claim is CLEAN on BOTH suites.

    -DFT_REKEY_CLAIM, --enable-rcu-debug:
      ft_unit                      315 ok / 3 deliberate / NO ABORT
      ft_inv FT_INV_MW=1 per-node  119 / 119            / NO ASSERTION

That is the same pair `bbb8e795` used to declare B1 owner-complete, and it is
§9.1(A)'s completion criterion: **the rekey writer — the one site in the tree
that already parks SW under FINE — now names an owner it holds at every record it
plants.** The walk began this session at ft_unit test 112.

The last witness turned on a distinction worth keeping: **an owner-check miss
splits two ways, and only one of them is a defect.** Where the TXN owns the
mark's clearing, a missing registry entry IS the double-clearing race — the five
fixes before this one closed exactly that. Where a caller's SWEEP owns it, the
entry is absent by design: the anchored retire's fused `{LOCK|s -> TOMBSTONE|s}`
leaves the word tombstoned, `ft_meta_lock_acquire` refuses a tombstone forever,
so no peer can re-mark it and the sweep is deterministic on both outcomes. That
site also *cannot* register — `FT_MAX_DEPTH == FT_ENTRY_PER_NODE + 1 == 257`.

**The mechanism**: the witness travels with the record, the judgment stays at the
choke point. `__ft_flip_txn_record_tag_ctx` takes a debug-only `@dbg_ctx`;
`record_tag` / `record_state_kind` / `record_tombstone_locked` become wrappers
passing NULL, so every existing caller's predicate is unchanged to the bit. Only
`ft_flip_txn_record_retire_anchored_arms` passes it, at its four record sites.

★ **The SHAPE decides, not the call path.** `ft_owner_retire_witnessed` widens
only for a record whose slot is `@owner`'s own state word and whose NEW value
sets `FT_STATE_TOMBSTONE`. A `ctx` supplied on a release, an edge or a count gets
the narrow predicate back — misuse fails CLOSED. And it is a WITNESS, never a
VERDICT: the ctx is looked up HERE, through the same `ft_held_set_snap` the
exclusion logic already trusts for dedupe. A caller may not pass "I already
checked" — a verdict token is mintable and unfalsifiable at the point that
consumes it, which is how a red control goes blind.

☑ **DETECTOR VERIFIED, NOT ASSUMED**: `-DFT_RED_OWNER_CLAIM_ON_LOCK` still aborts
ft_unit on its third test, on this assert by name, with the wide term present and
correctly declining to save it. All 118 gate legs identical to the control.

☞ It closed THREE witnesses, not one — the detach's orphan freeze plus
`ft_rekey_cow_stop`'s two stop retires.

#### ☞ WHAT §9.1(A) BEING DONE UNLOCKS, AND WHAT IT DOES NOT

☑ **B1's arm is no longer blocked by 9.1(A)** — that was the whole dependency:
arming removes the last detection of the rekey writer's unowned parks, and there
are now none to detect.

☑ **9.1(B) HAS ITS FIRST SHAPE — the ORDERED-LIST INTERLEAVE, `test_merge_rekey
_same_trie_ordered` (123), is GREEN.** Two refusals stood in front of it and they
had to land together, because 123 is the only shape that reaches the second.

* **The COLLISION** was refused because the collect drops a colliding src head as
  "a floating duplicate never reachable as a distinct head" — true only when the
  src list is CONSUMED. In-trie the edges already route every surviving link
  around that cell, so the drop was never the problem: what was missing is the
  RECLAIM. The collect now returns the dropped cells and the caller frees them on
  the far side of its commit, exactly as `ft_glue_free_collided_cells` does
  cross-trie.
* **The ABUT** refusal was REAL, and the earlier bracket proved it: lifting it
  with the collision handled did not give a wrong answer, it LIVELOCKED
  (65,897,286 attempts). The cause is a DUPLICATE SLOT. When the run abuts the
  region, the region's neighbour on that side is a cell OF THE RUN, so the
  collect seeds a boundary on a moving cell — and the gap closure
  (`ft_ord_cell_run_detach_edges`) writes `run_pred->next`, the very slot the
  collect writes when a survivor follows the last region cell. Two records, one
  slot; the commit aborts and the retry loop spins forever.
* ☞ **THE FIX IS NOT A SPECIAL CASE IN THE EDGE BUILDING.** Abutting, the merged
  block occupies the UNION of the two ranges, so it has ONE boundary pair — the
  run's outer neighbour on the abutting side, the region's on the other — and the
  vacated gap is INTERIOR to it. So the abutting side's boundary is handed to the
  collect as a link target (`tail_link` / `prev_placed` + `head_linked`, kept
  distinct from `dst_succ`, which still bounds the region WALK) and the gap
  closure is not emitted at all. The duplicate cannot arise because the second
  writer is gone, not because the two happen to disagree.
* ☑ **RED CONTROL**: restoring the unconditional gap closure reproduces the
  livelock (`test_urcu_ft_unit` times out at 123, last completed 121). ☠ And note
  WHAT detects it — **not** the engine's duplicate-slot assert, which does not
  fire even under `--enable-rcu-debug`: the descriptor is poisoned and the op's
  own retry loop absorbs it. The livelock IS the detector.
* ☑ `test_rekey_occupied_dst_behind_compressed` (118) was written AROUND the
  collision refusal: its `if (s == CDS_FT_STATUS_OK)` body had never executed, and
  the expectations inside it (`"azzm"`, 5 keys) came from a `src="q"` scenario.
  The shape now moves, so the test asserts it: `azm azq azx qwm`, 4 keys over 5
  entries (the collided `qzm` absorbed onto `azm`'s chain). It does NOT abut
  (`qwm` sits between run and region), so 118 is the collision witness and 123
  the abutting one — both paths have one.

☐ **THE REST OF 9.1(B) IS UNTOUCHED**: 110 `test_merge_rekey_same_trie` and 112
`test_rekey_graft_vs_merge` are the VARIABLE-LENGTH shapes, tier 1 — never
dispatched to the atomic writer at all (`src_key_len != dst_key_len`). Large, and
never a B1 blocker.

☐ **Arm PLACEMENT remains the open API question**: `ft_flip_txn_arm_per_op` has
zero call sites, refuses `!t->nr_locks`, and its doc says "after the last
lock_register" — yet kind dispatch happens at RECORD time, so a doc-legal arm
point converts only records planted after it.

### 9.2 The load-sensitive concurrency class

`ft_inv` is clean alone and was recorded aborting with 4 concurrent process
copies (`inv_remove_cross_view`, plus aborts around the graft_swap solo
family) at the pre-split control commit `6815f178`, at roughly a 50% per-round
rate. The single-process gate cannot see the class. Two obligations: add a
multi-process arm to the gate, and root-cause it BEFORE Phase B certification
— it is the one known FINE-mode concurrency failure, and arming SW content on
top of an undiagnosed exclusion failure would convert its aborts into silent
erasure (an SW park cannot fail).

**Obligation 1 — ☑ DONE `8adf179e`.** `ft_parallel_gate.sh` grew an `imwx`
leg: `FT_GATE_COPIES` (default 4) concurrent copies on the default config,
reported per copy, each keeping its own completeness checks and core dir.
Proven to go red by a 2-second-timeout red control, not merely to run.

**Obligation 2 — ☠ BLOCKED: IT NO LONGER REPRODUCES, and that is not the same
as fixed.** Re-run 2026-08-24 at the very commit the failure was measured at
(`6815f178`, worktree, `--enable-rcu-debug`, `FT_INV_MW=1`):

    4 concurrent copies x 3 rounds     12/12 clean
    8 concurrent copies x 2 rounds     16/16 clean
    12 concurrent copies x 2 rounds    24/24 clean
    16 concurrent copies x 2 rounds    32/32 clean
    4 copies x 3 rounds under a concurrent `make -j96` churn loop   12/12 clean

**96 clean runs where ~50% were expected to fail.** So the recorded repro
conditions are INCOMPLETE — the variable that mattered was not captured. The
leading hypothesis is that the original "control" tree was not clean
`6815f178`: it is described as that commit *with the constructor split
stashed*, and this working tree routinely carries other uncommitted WIP, so
the control may have carried changes of its own. Machine load is the other
candidate and is the one ruled out above.

☞ **What this means for Phase B.** The obligation cannot be discharged by
root-causing a failure that will not reproduce, and it must not be waved
through either. Two things stand in for it, and BOTH are prerequisites:

1. `imwx` above, so the class is visible from now on rather than only when
   someone happens to run copies by hand.
2. §4's **record-time owner assert**, which is the direct machine check for
   precisely the hazard the gate exists to prevent — an SW park on a word the
   op does not own. It converts "silent erasure" into an abort, which is what
   made the undiagnosed failure dangerous in the first place. Build it BEFORE
   arming any site (B1–B5), and run it under `imwx`.

If the class resurfaces under `imwx`, catch a core
(`tests/regression/ft_corecatch.sh`) and root-cause it then — the evidence
will exist, which today it does not.

### 9.5 The gate is RED, in three PRE-EXISTING classes

Found by running the full matrix at B0 and, as the control, at `4887aaab`:
all 115 legs identical, so none of this is B0's and none of it is new. It is
recorded here because it was not previously written down, and because the
first class is a Phase B prerequisite that has come due EARLY.

1. **`nocompress` / per-node — `assert(g->record_only)`.** `ft_unit` aborts
   at `test_exclusive_graft_swap_inherit_non_root` (243 ok / 2 notok / 245
   tests), in `ft_glue_txn_commit_edges` under
   `ft->lock_fine && g->txn && g->txn->structural_sw`.

   **The trigger is not what was predicted.** The prediction (a code comment
   at the assert, `ft-mutation-helpers.h`, introduced with `dc2cfde0`) was
   that FINE arming would trip it at Phase B, and that the fix was to hoist
   `ft_glue_acquire_reparent_marks` above the source unlink + drain. It is
   EXCLUSIVE arming that trips it, so **A2 (`f6093f8b`) is already red in a
   config it was never run against.**

   ☠ **THE FIX IS OPEN — do not treat the hoist as decided.** The prescription
   lives in ONE code comment, not in a signed-off plan step; an earlier
   revision of this section claimed it discharged "§4's prerequisite 1", and
   no such item exists in §4 — that cross-reference was wrong. What §4's A1
   step actually records (`dc2cfde0`) is a GATING change to that acquire, not
   a hoist. The candidates, and what is now established about each, are in
   the handoff note `project_ft_glue_reparent_hoist_open_question`. In brief:

   * All SIX paths into `ft_glue_txn_commit_edges` honour an ABORT — verified
     per caller — so `record_only` is a stale PROXY for the property the
     assert wants ("this caller absorbs an ABORT here"). ☞ But "honour"
     differs: `ft-rekey.h:3001` and `ft-graft.h:3625` have a genuinely clean
     bail, while `ft-merge.h:2849` / `ft-rekey.h:4836` / the non-fused graft
     arms are PAST a point of no return and honour it only by a retry that
     must eventually succeed.
   * A full hoist is impossible for ATTACH-TIME-discovered edges: the acquire
     needs `g->deferred[]` complete, and the displaced child's identity is
     attach-time knowledge a retry's re-descend replaces. It is achievable
     only for build-time edges. `ft_glue_acquire_splice_holders` — the model
     the hoist appeals to — is itself SKIPPED on the `unfailable` path for
     the same reason (`ft-merge.h:1978-1993`).
   * ★ **A fourth candidate, and it addresses the cause:** on an EXCLUSIVE
     trie the mark defends against nobody. The acquire's own header already
     makes exactly this argument for COARSE — the mark arbitrates against one
     peer (`ft_meta_nr_child_inc` from an insert below the child), and where
     that peer is excluded "the mark has nobody to arbitrate against" — and
     exclusivity excludes it more strongly than the COARSE mutex does. So the
     gate may simply be wrong: `lock_fine && armed` should perhaps read
     `lock_fine && armed && !exclusive`, which keeps the assert un-relaxed.
     ☠ NOT free: skipping the acquire also skips its `ft_glue_op_holds`
     detection, so `held_lock` stays false and the live re-parent records the
     §4.B MW guard — a deterministic self-abort if any exclusive shape
     self-holds a re-parented child's word. Unproven either way by reading.

   ☞ Reachability is SETTLED BY READING, and it does not narrow the problem:
   `structural_sw` has exactly two sources (the constructor arm
   `!lock_fine || exclusive`, and the rekey one-decide writer);
   `ft_flip_txn_arm_per_op` has zero call sites. Combined with `lock_fine`
   the branch is reachable iff the glue txn's trie is FINE **and** exclusive
   — and no graft/merge/rekey path gates an exclusive DST out (only src /
   swap exclusivity is checked, `ft-graft.h:1283`, `:2795`). So graft-attach
   can reach it. ☠ A runtime call counter would answer the WEAKER question of
   which callers the SUITE drives there — see
   `feedback_reachability_is_a_code_fact`.

   ☞ The lesson generalizes past this bug: A2's gate list (§4 step 3) was
   ft_unit + ft_inv + ASAN + fault-inject + reserve + an 8-copy control, and
   every one of them passed. None of them is the config that fails. An arming
   step must be gated on the FEATURE-FLAG MATRIX, not on the default build.

2. **`txndbg` and `anchorval` at `exponential` and `root-only` spacing.**
   `ft_unit` aborts (291 and 236 tests in), and at root-only `ft_inv` aborts
   after 73 tests on all three arms. `proxyassert` runs the same three
   spacings and passes all of them, so this is not the coarse-spacing axis
   alone — it is those two detectors ON that axis. Unattributed; no root
   cause yet.

Per-node — the default, and the spacing every other config runs at — is clean
everywhere except class 1.

### 9.3 The deleted staged rekey writer

8 tests fail on purpose until the staged writer is reimplemented atomically
(a tmp-trie hides live keys for a GP per move; an FT-wide lock does not
redeem it). Independent of the arming steps, but it holds test-suite green
hostage; schedule it with the 9.1 thread.

### 9.4 Monitors

`skip_conflict`'s slot arm rewinds 50,316/run (the "never fires" note is
stale) — watch it across Phase B, it shares words with the converted sites.

---

## 10. Method rules that bind this work

* Prove the mechanism ran before believing any zero (counter must MOVE; the
  positive control is the pattern). FT code lives in `liburcu-cds.so`, not
  the test binary.
* The kind-conflict detector must be verified ARMED per build
  (`DEBUG_RCU`/`CONFIG_RCU_DEBUG` probe TU) before a green certifies a step.
* One adversarial skeptic per exclusion claim, async; default refuted=true.
  The best-effort claim gate from CLAUDE.md applies to every abort path this
  plan touches.
* Byte-neutral claims are verified at the OBJECT (normalized `objdump -d` +
  `nm -S`), a lesson the constructor split already paid for.
* Benchmarks: `-O2 -DNDEBUG`, sequential, before/after on the same build
  pair; abort-relief claims quote the 29% bound.
* A status table is a dated claim — `git log -S` the identifier before
  acting on any ☐ row above.

## 11. Landing order (the one list)

    G2  always-MW root helper, retire sw_exempt_slot        ☑ LANDED a961c6e6 (site map in §2 was INCOMPLETE)
    G1  reader-sufficiency answer                           ☑ ANSWERED YES 2026-08-23 — cleared
    A1  arm COARSE non-exclusive  + protocol §3             ☑ LANDED (four commits, §4 step 2)
    A2  arm exclusive             + protocol §3             ☑ LANDED f6093f8b
    9.2 multi-process gate arm                              ☑ LANDED 8adf179e
    9.2 root-cause cross_view                               ☠ BLOCKED — 96 clean runs at
                                                              its own control commit; §9.2
                                                              names the two stand-ins
    9.1 rekey fine-lock completion                         (IN FLIGHT) — the reds are
                                                              110/112/123 now (two
                                                              regression tests shifted the
                                                              indices).  ☞ TWO HALVES: the
                                                              fine-lock conversions (what
                                                              blocks B1) and the atomic
                                                              writers for the deleted staged
                                                              writer's shapes (what greens
                                                              the three).  -DFT_REKEY_CLAIM
                                                              (658989ef) enumerates the
                                                              first, abort by abort;
                                                              695d23c1 / d67851c6 / bec0c726
                                                              / 280fdac1 / c6ac8c42 /
                                                              b7334aa4 / f79438e7 / 6e77bab7
                                                              / d4b4d2d7 / 05356918 / bf9c490c
                                                              landed — the WITNESS class is
                                                              CLOSED (4 instances); next is a
                                                              / ebf24686 / a91ebafd / 67f72278
                                                              / 64085ea3 — ☑ §9.1(A) COMPLETE:
                                                              the claim is CLEAN on ft_unit
                                                              AND on ft_inv FT_INV_MW=1.
                                                              B1's arm is no longer blocked by
                                                              it.  ☑ 9.1(B)'s FIRST SHAPE landed:
                                                              the ordered-list interleave (123)
                                                              greens once the COLLISION reclaim
                                                              and the ABUT boundary land together
                                                              -- abut was a DUPLICATE SLOT
                                                              (livelock, 65.9M attempts), fixed by
                                                              giving the collect the union range's
                                                              boundary and dropping the gap
                                                              closure.  ☐ 110/112 (VARLEN, tier 1)
                                                              and the arm-PLACEMENT question
                                                              remain (§9.1)
    B0  per-op arm helper + record-time owner assert        ☑ LANDED — and its first
                                                              measurement says NO site is
                                                              owner-complete (11.9% of the
                                                              surface); §4 has the table
    B0b the owner predicate: registry ∪ hold-ledger        ☑ LANDED @92e27199 — worth
                                                              1.7 pts; its first zero was an
                                                              INSTRUMENT BUG (§4)
    Bx  the external-head class                            ☠ SPLIT IN THREE (§4).  head
                                                              promote ☑ LANDED e9268e13
                                                              (0%→33.3%); publish-lane owner[]
                                                              ☑ LANDED 6f54e698; external
                                                              back-edge ☐ the real DESIGN call,
                                                              and the smallest of the three.
                                                              ☞ The "§8.2" reference is NOT
                                                              phantom — it is the ESCALATION
                                                              MODEL's §8.2 (b44d08cc); the two
                                                              design docs reuse section
                                                              numbers, so always name the doc
    B1  insert one-commit                                   ☑ ARMED e2ba43f8 -- 2.64M txns
                                                              armed, 6,998,341 records SW.
                                                              ☠ NEEDED A PER-NODE SPACING
                                                              GATE: ungated, per-node stayed
                                                              green and txndbg/anchorval went
                                                              RED at exponential + root-only
    B2,B4,B5 the other hot sites, one at a time            (each: owner-complete -> claim
                                                              dry-run -> arm; NOT mechanical).
                                                              B2 ☑ ARMED: owner-complete
                                                              a9ff9549, in-place path 6024f167
                                                              (reaches 10 times -- an EXCLUSIVE
                                                              trie shape), RECOMPACTION
                                                              REPUBLISH 21b6b559 -- armSW now
                                                              tracks OK (4.00M of 4.00M
                                                              commits).  ☠ "0.7%" was armSW
                                                              mis-read as yield; 19befc1f is
                                                              the REACH counter that separates
                                                              them.  Remainder = the ~2.4
                                                              records/commit planted BEFORE
                                                              the acquires finish (Phase C/E)
    B3  ft_chain_compress_fused (the collapse)              ☑ ARMED ec00e234 -- dry run
                                                              cec68299 CLEAN at FIRST reading,
                                                              no fixing pass; 99.8% of reaches
                                                              armed.  ☠ its abort column needed
                                                              n=3 ALTERNATED: one run read as a
                                                              wrong-direction alarm and did not
                                                              survive (ctl 62.6 vs arm 55.4
                                                              aborts/1k, control spread 51-73)
    B5  ft_unchain_node's head clear                        ☑ ARMED 8cd52c64 -- no SKIP_X dual
                                                              by ROUTING, and a DETECTOR says
                                                              so (red control: silent on
                                                              ft_unit, 6 aborts on ft_inv)
    B4  ft_promote_head (both arms)                         ☑ ARMED 1c59dc9c -- unblocked by
                                                              the per-edge @owner_held fix af22756b
                                                              (option (a)).  ☠☠ a NULL @owner
                                                              NEVER failed closed: record_tag
                                                              dispatches on structural_sw ALONE
    B6  retire hand-arming (rekey writer, root COW)         ☑ LANDED f8b6317f -- ONE
                                                              DOOR at txn CREATION
    C   re-measure; G4 cell-lane decision                   ☑ DONE.  ☑☑ G4 ANSWERED
                                                              by C.1(d): the ordered-cell
                                                              lane is 0.9-1.9% OF ABORTS
                                                              (6% of always-MW volume)
                                                              => KEEP THE NARROW MW LANE.
                                                              ☠ §2's G4 row still reads
                                                              "do not decide it on argument
                                                              now" -- STALE, the answer is
                                                              in §5 C.2/C.1(d)
    D   acquire fair-handoff design w/ Mathieu              ☑ DONE D.0-D.5 (@027eeb9d,
                                                              the wait-ladder public API).
                                                              ☠ It was recorded as G5's
                                                              PREREQUISITE; G5.3 RETIRES that
                                                              -- (F) takes its flip GP under
                                                              writer_lock, whose waiters park
                                                              OFFLINE, so no point-op park is
                                                              owed.  D.0-D.5 stand on their own
                                                              (the tail, the ladder, the
                                                              curative lever = §8.2)
    G5  bulk vs point exclusion                            ☑ LANDED @a2b683e1 -- the FT-WIDE
                                                              WRITER LOCK, taken when the bulk
                                                              refcount says a window is open.
                                                              ft_writer_lock_scope_enter DROPPED it
                                                              for FINE tries unconditionally; it now
                                                              keeps it while ft_bulk_active(ft)
                                                              (internal.h:2628, FT_BULK_WIDE_LOCK, on
                                                              by default -- a 0 build has NO
                                                              bulk-vs-point exclusion at all).  No
                                                              ancestor ledger, no up-walk, no release
                                                              owner, no reservation to size, and no
                                                              "acquire before the txn exists" blocker
                                                              -- a scoped mutex has no registry -- and
                                                              it QUEUES (cds_fair_mutex FIFO) where the
                                                              widened DLM acquire spun
                                                              URCU_TXN_WAIT_PATIENCE and ABORTED.
                                                              ☑ EVERY BULK OP ENTERS THE GATE @f0f2beac:
                                                              rekey through ft_move_gate_enter
                                                              (FT_BULK_COHERENT, ft-rekey.h:3667,
                                                              :5991) and detach / graft / graft_swap /
                                                              merge_at through CDS_FT_SCOPED_BULK_GATE
                                                              (FT_BULK_WRITER_ONLY, ft-detach.h:646,
                                                              ft-graft.h:2591, :2842, ft-merge.h:3753);
                                                              cds_ft_merge delegates to cds_ft_merge_at
                                                              ☠ THE COST IS COMPLETE EXCLUSION'S: 8.66x
                                                              on point ops on the DEEP arm while a
                                                              window is open.  NOT comparable to the
                                                              widening's 1.48x, which was cheap because
                                                              it fired at 1.2% COVERAGE (§2 G5.25)
                                                              ☑ REMOVED with it: the widening @8f837d3a,
                                                              its bulk-live anchor descent @22f2ae47;
                                                              the ancestor ledger is DEBUG-ONLY @dffe6ade
                                                              ★ The SEAM RULE (@d48b5c94) stays
                                                              load-bearing: ft_writer_lock_gp_wait drops
                                                              this lock across every GP, which is what
                                                              makes holding it over a bulk body sound
                                                              ☑ THE GATE'S LEVEL HALF IS REMOVED (§2
                                                              G5.30): the widening was its only consumer
                                                              and every entry passed 0 anyway, so the
                                                              occupancy array, the packing, both
                                                              accessors, the unreachable lowering GP and
                                                              the level parameter are gone.
                                                              sizeof(struct cds_ft) 864 -> 336, -61%
                                                              ☐ the junction-level CEILING is UNMEASURED,
                                                              not refuted -- banked in §2 G5.19 (two
                                                              packed epoch words, flipped on demand),
                                                              which does not use that array
                                                              ☞ §2 G5.0-G5.30 is the full record, incl.
                                                              (F)'s protocol REFUTED (G5.4), the tier-2
                                                              per-node refcount REFUTED (G5.7) and
                                                              declare+enforce+escalate REFUTED (G5.23)
    E   spacing certification + gate lift + strategy fold   ☠ BLOCKED, and NOT on E.2 --
                                                              re-derived @54c6358e: the blocker
                                                              is ft_txn_per_op_spacing_ok ==
                                                              PER_NODE, so a lift ships two
                                                              spacings where ALL of Phase B is
                                                              inert.  With E.4 measuring the
                                                              spacings within noise, the lift
                                                              BUYS NOTHING today: a VALUE
                                                              question for Mathieu, not a
                                                              certification chore.  ☑ E.1-E.4
                                                              done; ☑ the probe-free control
                                                              landed @c90228da
    F   §8.3 layout split + in-place + single-writer mode   (large, last).
                                                              ☑ single-writer mode LANDED
                                                              f6ab074d/d2543946 (§8.4).
                                                              ◐ in-place: the point-op INSERT
                                                              tier landed (opt-in, see
                                                              ft-reintroduce-in-place-
                                                              mutations.md §6); DELETE tier,
                                                              bulk reserves and the default
                                                              flip open.  ☐ layout split,
                                                              ☐ terminology sweep.  ★ §8.2 in-place
                                                              is now DOUBLY motivated: E.4
                                                              says the FINE-vs-COARSE sweep is
                                                              not actionable until it lands,
                                                              and D.5 says shortening the hold
                                                              is the CURATIVE liveness lever

Each step is bisectable, each gated by §3's protocol; nothing below the C
line starts before the C measurement exists, because C is what prices D–F.
☑ That gate is now SATISFIED — C is done and priced D–F — so the ordering
constraint it expressed has been discharged rather than merely deferred.

## Moved from the public header (2026-11-19)

The text below came from comments in `include/urcu/fractal-trie.h`. It is
transition history and design notes, not API documentation, so it was moved
here and the header keeps only the current rule.

Each subsection names the header comment it came from. The facts, figures
and names are those of the header text. A line that starts with "Status
2026-11-19:" says what the code does today, where the moved text is no
longer true. File names are relative to `src/fractal-trie/` unless they
start with `include/` or `tests/`.

### M1. `enum cds_ft_writer_strategy`: the transition log

The header comment had a block titled "☐ TODO -- THE FINE-LOCKING
TRANSITION IS INCOMPLETE, BUT EVERY POINT OPERATION IS NOW CONVERTED". All
of it is below, in the order of the header.

#### M1.1 What is converted and what remains

The fine-locking transition is incomplete, but every POINT operation is now
converted. The converted operations are:

- `cds_ft_insert`;
- `cds_ft_remove`;
- `cds_ft_replace`;
- `cds_ft_insert_unique`;
- `cds_ft_remove_all`;
- `cds_ft_insert_replace`, since the whole-chain freeze went into its
  commit.

Their notes say they may run concurrently with each other "including on the
same key".

The BULK ops are NOT in the waiting set at all: graft, graft_swap, merge_at,
merge, detach and the rekeys. See M1.8 (the mode flip).

What remains: `cds_ft_compact_step` and `cds_ft_recompute_stats`, which
carry no writer-strategy paragraph at all. By plan, concurrent compaction is
the last step of the transition, after rekey.

Status 2026-11-19: `cds_ft_compact_step` now has a FINE paragraph in the
header. Its code enters `CDS_FT_SCOPED_WRITER(ft)` on each step
(`ft-compact.h`, `cds_ft_compact_step`, "THE MODE FLIP, PER STEP"). The
header still asks the caller to hold its writer exclusion across the call.
`cds_ft_recompute_stats` still has no writer-strategy paragraph. Its header
comment asks for the caller's mutual exclusion with other writers.

#### M1.2 ☠ "Every point op is converted" is not "the transition is done"

The per-op notes are the wrong place to look for the difference. A converted
op is one whose CALLERS may run it concurrently. The transition also owes
the WORD-KIND conversions:

- the duplicate list to SW under the nearest ancestor lock;
- the SKIP_X dual;
- `metadata.parent_word` / `parent_slot_offset`.

Those are tracked in the word-kind table in `fractal-trie-internal.h`, not
in the public header.

#### M1.3 ★ The order is part of the plan

The order is not an accident of what got done first:

1. point ops;
2. bulk;
3. rekey;
4. concurrent compaction, as the final step.

Until then `cds_ft_compact` / `cds_ft_compact_step` require the caller's
exclusion and are NOT in the validated set. This lets a conversion land
while a compaction producer of the same word is still MW. The two never run
concurrently, so they cannot disagree on a word's kind.

⇒ Do NOT hold a conversion back on account of a compaction-only producer.
Do NOT read "compaction still CASes it MW" as a blocker.

#### M1.4 ☠ Read those sentences as a TODO, not as a contract

They describe work not yet done, not a property anyone wants. No design
reason prevents the rest from coordinating through the same per-node
lock-sets as insert and remove. A caller-side mutex around them defeats the
point of the default strategy. This was documented once, at the enum,
because per-op notes with identical wording read like deliberate decisions.

#### M1.5 ★ They are not all one defect shape

The note used to assume that they were, and that was a trap.

- Most unconverted ops derive something with NOTHING HELD and then trust it.
  The converted sibling goes on to ACQUIRE the owning node and RE-VALIDATE.
- `cds_ft_remove_all` was different. It already acquired and re-validated
  everything it derived. What it lacked was PROGRESS. Its body ran exactly
  once. So every correct abort, which had published nothing, was handed to
  the caller as BUSY_ERROR.
- Under a same-key peer, 91% of its calls failed that way (measured 25634
  BUSY vs 2473 OK). It lost no node and corrupted nothing.

⇒ Ask what an op is missing before applying the recipe. Three of the four
needed exclusion; the fourth needed a loop.

#### M1.6 ☑ The four conversions

**`cds_ft_replace` was the first of the four converted.** It is the worked
example to copy, with one lesson that generalises. Its conversion is smaller
than remove's. How much re-validation an op owes after its acquire is a
question about the op, not a template to apply:

- remove re-validates its ROUTING ("★ RE-VALIDATE THE CALLER'S ROUTING",
  `ft-remove.h`). Its three arms are three DIFFERENT structural transforms,
  so a stale decision publishes an ILLEGAL SHAPE.
- replace has ONE transform in every arm. It is recorded with the target
  node as the EXPECTED-OLD, so a stale slot ABORTS. The expected-old IS the
  routing validation. The lock adds EXCLUSION.
- The lock also adds the one thing a value-CAS cannot see: a stale slot
  ADDRESS inside the retired copy of a COW'd holder. The body of that copy
  still holds the expected old node, so the CAS would succeed into a dead
  node. The lock catches it because `ft_dlm_lock` refuses a TOMBSTONE.
- NEITHER re-validates from the root. Both checks are a back-pointer hop.

**☠ A hold cancels the §4.B guard on the same word.** The conversion has to
know about this trap.

- The expectation of `ft_flip_txn_guard_parent` masks `FT_STATE_LOCK` out.
- So it can never match on a word the op itself locked, and 100% of commits
  abort.
- Measured: 49966 of 49974, attributed to the VALIDATE class by
  `-DFT_WINNER_DBG`.
- `ft_flip_txn_lock_or_guard_parent_at` already encodes the skip on its
  `@held.shared` path. A hand-rolled acquire must spell it itself.

**☑ `cds_ft_insert_replace` answered that question with a yes.** It is the
one op that needed MORE than the acquire+revalidate recipe.

- It replaces the whole duplicate chain.
- The composed version froze only the HEAD. That left 21.7% of
  displacements with unmarked LIVE members.
- A concurrent `cds_ft_insert` appends onto one of them and the key is LOST,
  with nothing in the trie to say so.
- The cure is one commit that freezes the whole chain, with the length as a
  BOUND: the last member is recorded against the derived NULL.
- A concurrently appended duplicate then tears the expected-old and aborts,
  instead of being marked into a tombstone.

⇒ "Ask what an op is missing" (M1.5) applies a fourth time. Three needed
exclusion, one needed a loop, and this one needed its marks folded into the
COMMIT.

The `cds_ft_insert_replace` comment in the header carried the same figure:
"Freezing only the head left 21.7% of displacements with unmarked live
members. A concurrent cds_ft_insert could then append onto them and lose the
key." The header now states the rule without the figure.

**☑ `cds_ft_remove_all` was converted by adding its retry loop.** Three
premises died with its exclusion. Each was a comment asserting something the
conversion falsifies:

- an "-EAGAIN from here is the whole CALL failing" in the orphan acquire;
- an assert that detach cannot return -ENOENT "because it has been found by
  a mutex-protected traversal within this function" (no mutex exists there
  under FINE);
- a discarded unsplice status justified by "ABORT unreachable under its
  current exclusion".

A FOURTH was only reachable at EXPONENTIAL spacing. The op derives its
holder from the chain head's BACK POINTER. It then re-derives it TOP-DOWN by
the key, for the lock-set depths. Under a peer the two can disagree. That
used to be impossible. It now bails retriably instead of asserting.

#### M1.7 The cross-trie / bulk ops: the source side is settled

The destination's story is NOT the per-node conversion an earlier version of
the note claimed.

Settled:

- `cds_ft_graft` and `cds_ft_graft_swap` require an EXCLUSIVE source under
  FINE;
- `cds_ft_merge_at` likewise (its code returns BUSY for a concurrent
  source);
- `cds_ft_detach` hands back a trie exclusive by construction.

An exclusive trie has no concurrent writer to coordinate with, so nothing is
owed there.

☠ Their FINE paragraphs used to promise the opposite of the design, and were
corrected. They said bulk ops "may run concurrently on the same destination"
and handed point exclusion to the caller. Neither is the goal, and neither
is what the code does.

#### M1.8 ☑ Bulk-vs-point exclusion: the mode flip is the design

Bulk-vs-point exclusion is not the caller's, nor is it owed. A bulk op
publishes the gate and waits a GP. From then on a FINE point op FLIPS MODE:
it takes the FT-wide writer lock instead of skipping it. This is the test in
`ft_writer_lock_scope_enter` (G5.25):

    ft->lock_fine && !(FT_BULK_WIDE_LOCK && ft_bulk_active(ft))

So bulk and point writers arbitrate on one word. The FT-wide lock protects
the bulk-op mode transition, which is also the reader dual-descent
transition. That mode flip IS the target design for bulk-vs-point, not a
placeholder for per-node work.

⇒ The bulk ops' own paragraphs now say so. The POINT ops' notes still do not
mention bulk ops at all. That is the remaining under-description.

★ The FT-wide lock is the mechanism, not an over-hold awaiting relaxation.
The goal is one word for both questions. BULK-vs-BULK is the FT-wide writer
lock. BULK-vs-POINT is the same lock, reached by flipping the point ops onto
it. The bulk-in-progress state is the flip, not a second mechanism:

1. publish it;
2. wait a grace period, so that the point mutations already in flight
   drain;
3. from then on every writer scope takes the lock.

Bulk peers queue on it for the same reason point ops do.

⇒ An exclusion argument for a bulk op MAY rest on the FT-wide lock; that is
what it is for. It may NOT rest on the lock being held CONTINUOUSLY across
the whole body, because it is not. See M1.9.

Status 2026-11-19: "BULK-vs-BULK is the FT-wide writer lock" is no longer
the whole mechanism. Bulk ops now serialize on a second lock, `bulk_lock`
(`fractal-trie-internal.h`, `struct cds_ft` and `ft_bulk_lock_enter`). The
test in `ft_writer_lock_scope_enter` is unchanged.

#### M1.9 ☐ The drain-seam gap

There is one gap against that goal, and it is in the bulk op's OWN code.

- `ft_writer_lock_gp_wait` UNCONDITIONALLY drops `writer_lock` at seven live
  sites inside the bulk bodies (`ft-detach.h:219/511/575`,
  `ft-graft.h:2960/4011`, `ft-rekey.h:4510/5035`).
- So the hold is NOT end-to-end, and a peer bulk op can enter at a drain
  seam.
- Read that as a HOLE IN THE MECHANISM to close, not as a reason to pick a
  different one.

The drop is the COARSE discipline. There, point ops wait at the ONLINE scope
entry, so holding across a GP would wedge them. On a FINE trie every waiter
on this lock is another BULK op, and bulk entries forbid a read-section
caller. So the GP may be taken while HOLDING the lock, with the waiters
parked OFFLINE. The two paths must not share one acquisition helper by
accident.

Until then, two things cover the seam:

- the REFCOUNTED gate (overlapping windows compose, with no per-node bit for
  a peer's clear to erase);
- each op's own acquire/validate machinery.

This is a backstop, not the design. ☞ G5.3/G5.4/G5.25 in this document.

⇒ So the open question is narrow and named. It is the SEAM, not the choice
of mechanism. What separates two bulk ops that meet inside a drain seam,
where the lock is momentarily not held? FT-SLOT-3 is the live instance.
"every caller here is a BULK op ... so the two arbitrate on one word"
answers bulk-vs-point. Two bulk ops that both park SW on one live parent
word have nothing stated to separate them across a seam. Known issues get
stabilised first; this is later.

⇒ The rule that follows is for anyone adding exclusion arguments here.

- You MAY rest one on the FT-wide lock: that is the design. But state WHICH
  SIDE of a drain seam it needs.
- An argument that needs the lock held CONTINUOUSLY across a GP is not true
  today. One that needs it held per-edit is.
- Where neither holds, state it on the per-node lock-set or on the gate
  flip.

FT-SLOT-3 is an EXISTING instance, flagged as such.
`ft_glue_record_back_edge` passes `@child_held` TRUE unconditionally. So it
parks SW on a live node's parent word that the op does not hold. This is
sound only because every caller is a bulk op: "☠ SO IT IS A DEPENDENCY, NOT
AN INVARIANT". It is debt against the relaxation, NOT a missing destination
conversion. It is debt on the BULK-vs-BULK side as much as on the point
side.

Status 2026-11-19:

- The call sites of `ft_writer_lock_gp_wait` inside the bulk bodies are now
  nine: `ft-detach.h:219/513/577`, `ft-graft.h:2339/3180/3804/4876`,
  `ft-rekey.h:9651/10199`. The function still drops `writer_lock` whenever
  the thread holds it.
- The gap between two bulk ops is closed by a second lock. `struct cds_ft`
  has a `bulk_lock` ("BULK-vs-BULK, AND IT IS A SECOND LOCK ON PURPOSE",
  `fractal-trie-internal.h`). `ft_bulk_gate_enter` takes it after the gate's
  grace period, through `ft_bulk_lock_enter`. It is held across the drain
  seams. Only bulk ops wait on it, and they park OFFLINE.
- An EXCLUSIVE trie and a `CDS_FT_WRITER_EXTERNAL_SYNC` trie skip
  `bulk_lock` (`ft_bulk_lock_enter`).
- The comments of `cds_ft_detach` and `cds_ft_merge` in the public header
  still say "☐ The drain-seam gap applies". They were not changed.
- The FT-SLOT-3 statements were not re-checked against the code.

#### M1.10 ☠ Do not call the bulk ops "unconverted"

Do not call the bulk ops "UNCONVERTED FOR FINE DESTINATION LOCKING", as an
earlier version of the note did. Their bulk-vs-point story is finished by
design. The unconverted list is the POINT ops above, plus compact /
compact_step / recompute_stats. Those three are caller-excluded by their own
contracts. And do not cite merge_at as a destination-conversion template:
there is nothing there to copy that graft and detach do not already do.

Status 2026-11-19: "The unconverted list is the POINT ops above"
contradicts M1.1, which says every point operation is converted. The
per-function comments in the header agree with M1.1.

#### M1.11 ☐ Two header bugs found while enumerating

The note said both were still present:

- `cds_ft_merge` says "NOT concurrency-safe" yet its body is one line:
  `return cds_ft_merge_at(dst_ft, key, key_len, src_ft, key, key_len);`.
  Both notes cannot be right; the code says merge_at's is.
- `cds_ft_merge_at` says "@src_ft ... May be in either exclusive or
  concurrent mode" in one paragraph and "The SOURCE must be EXCLUSIVE" in
  the next, while the code refuses a concurrent source with BUSY.

Status 2026-11-19:

- The first is fixed. The `cds_ft_merge` comment no longer says "NOT
  concurrency-safe". Its FINE paragraph says "identical to cds_ft_merge_at".
  The body is still the one line above (`ft-merge.h`, `cds_ft_merge`).
- The second is still present in the `cds_ft_merge_at` comment.

#### M1.12 ☐ Four mutators the enumeration first missed

A reminder that the list is the declarations, not the ones with a
writer-strategy block:

- `cds_ft_rekey_graft` / `cds_ft_rekey_merge` have NO writer-strategy
  paragraph at all. This is against the section's own promise that "each
  operation below states the caller's mutual-exclusion requirement". In
  code they arm the COHERENT bulk gate and then take a writer scope, so
  G5.25 excludes them exactly as it does the other bulk ops. A DOCUMENTATION
  gap, not a code gap.
- `cds_ft_compact` and `cds_ft_compact_step` say the caller must exclude,
  and mean it: the one-shot takes a writer scope, `_step` takes NO scope and
  NO bulk gate. Its per-relocation -EAGAIN -> `CDS_FT_COMPACT_BUSY` plumbing
  is already there for the conversion, unused.
- `cds_ft_recompute_stats` likewise, and the rekey path calls it internally
  inside its own bulk window.

Status 2026-11-19:

- The rekey functions still have no writer-strategy paragraph in the
  header. The entry arms the gate with `ft_move_gate_enter`, which is
  `ft_bulk_gate_enter(ft, FT_BULK_COHERENT)` (`ft-rekey.h`,
  `fractal-trie-internal.h`).
- "`_step` takes NO scope" is no longer true. `cds_ft_compact_step` enters
  `CDS_FT_SCOPED_WRITER(ft)` inside each step's read-side section
  (`ft-compact.h`). It returns `CDS_FT_COMPACT_BUSY` when a relocation bails
  with -EAGAIN, so that plumbing is used.
- `cds_ft_recompute_stats` takes `CDS_FT_SCOPED_WRITER(ft)` and calls
  `ft_recompute_max_used_key_len` (`ft-lifecycle.h`). The rekey path now
  calls `ft_recompute_max_used_key_len` directly (`ft-rekey.h`).

#### M1.13 ☑ Exempt, not unconverted: order statistics

A group that maintains order statistics is coerced to
`CDS_FT_WRITER_LOCK_COARSE`, so the FINE question does not arise for any op
on it.

#### M1.14 ⇒ The recipe, and how to test it

The recipe exists in-tree. The point remove was converted exactly this way:

1. derive the holder;
2. acquire it;
3. RE-VALIDATE that it still names what the plan read;
4. bail RETRIABLY when it does not.

See `ft_chain_head_holder`'s re-validation and its callers. Reuse that
rather than inventing a new rule.

☞ And test it in-contract. A concurrency row for an unconverted op belongs
on the FINE arm. It is expected RED until the conversion lands. Writing it
COARSE-only would bless the gap as the contract. COARSE stays the
adjudicator for a shape that IS converted (the same-key rows of
`tests/regression/test_urcu_ft_inv.c`).

### M2. "IN-TRIE REKEY COHERENCE is automatic" (after `cds_ft_attr_set_speculative_keys`)

This was a standalone comment block before `cds_ft_make_exclusive`. Its
text:

IN-TRIE REKEY COHERENCE is automatic; there is no attribute for it.

A rekey (`cds_ft_rekey_graft` / `cds_ft_rekey_merge`) moves a live subtree
from one key prefix to another WITHIN one trie. So a reader whose traversal
races the move could otherwise be torn onto a position the key no longer
occupies. Every trie that can host such a move verifies its reads against a
concurrent move. A trie can host a move when its group does not use
speculative stored keys. The reason is that the library cannot rewrite an
application-stored key when a leaf's key changes.

The cost is confined to the move itself.

- A mover first publishes a per-trie "a move is in flight" mode and waits
  one grace period. So every reader has observed the mode before any
  structure changes.
- Readers check that mode once per traversal.
- With no move in flight, that check is a single load of a word nobody
  writes, and the read runs exactly as it always did.
- While a move IS in flight, readers on this trie verify each traversal and
  repeat it if the move restructured their path. So reads on that trie are
  lock-free rather than wait-free for the duration of the move, and
  unaffected otherwise.

A burst of concurrent moves pays about ONE grace period between them, not
one each. A rekey therefore BLOCKS and must not be called from an RCU
read-side critical section (see the rekey functions' own documentation).

Status 2026-11-19:

- "Automatic; there is no attribute for it" is no longer true.
  `cds_ft_group_attr_set_rekey` exists and defaults to false
  (`ft-lifecycle.h`). Without the opt-in, the rekey entry returns
  `CDS_FT_STATUS_NOT_SUPPORTED` before it touches the trie (`ft-rekey.h`,
  the `!ft->group->rekey_set` test). There is still no per-trie attribute.
- Under `CDS_FT_WRITER_EXTERNAL_SYNC`, the opt-in turns on
  `rekey_always_coherent`. Readers then run the coherent path always, and
  the move takes no gate grace period (comment of
  `cds_ft_group_attr_set_rekey` in `ft-lifecycle.h`).
- "Readers on this trie verify each traversal" is reported not to hold for
  the partial, longest-match and candidate lookups (review note
  readers-point.md A2). This was not re-checked here.
- The one-grace-period-per-burst claim is still stated by the code
  (`ft_bulk_gate_enter`, `fractal-trie-internal.h`).

### M3. History sentences from function comments

#### M3.1 `cds_ft_remove_all`: BUSY_ERROR

The header said: "CDS_FT_STATUS_BUSY_ERROR is NO LONGER RETURNED. A lock
lost to a concurrent writer is absorbed by this op's own retry loop. The
loop ages the conflict, so a contended caller escalates into the trie's FIFO
writer lane instead of spinning. Callers that tested for BUSY_ERROR may keep
doing so, because the value is still declared. That branch is dead."

The history is in M1.5 and M1.6: before the retry loop, every abort was
returned as BUSY_ERROR. The header now says that the function never returns
`CDS_FT_STATUS_BUSY_ERROR`. This matches the code: `cds_ft_remove_all`
loops while `_cds_ft_remove_all_locked` sets `need_retry` (`ft-remove.h`).

#### M3.2 `cds_ft_insert_replace`: the 21.7% figure

See the end of the `cds_ft_insert_replace` item in M1.6.

#### M3.3 `cds_ft_max_used_key_len`: no recompute before a bulk operation

The header said, about a bulk operation with an EXCLUSIVE source: "the
operation recomputes the source's value before answering. So a caller no
longer needs cds_ft_recompute_stats() for that."

The sentence implies the history: a caller once had to call
`cds_ft_recompute_stats()` on the source first, so that a stale estimate
did not refuse it with `CDS_FT_STATUS_OVERFLOW_ERROR`. The header now says
only that the caller does not need to call it first. The recompute of the
source is in `ft-graft.h` (the call to
`ft_recompute_max_used_key_len(src_ft)`).

#### M3.4 `cds_ft_group_attr_set_writer_strategy`: the optimistic engine

The paragraph on the coercion of FINE with order statistics ended with:
"(The optimistic engine this sentence also weighed no longer exists.)"

The sentence it refers to gives the two reasons for the coercion. The note
records that this sentence once also covered an optimistic engine, and that
the engine is gone. The header now gives the two reasons only.
