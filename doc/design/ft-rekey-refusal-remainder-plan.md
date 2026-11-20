# Fractal Trie: the same-trie rekey refusal remainder -- execution plan

Status: PLAN, 2026-09-10, written at `ft/unpub-free-audit` @088ec182 for the
session that performs the changes.  Author of direction: Mathieu Desnoyers.

## 0. What this plan is, and the two decisions it rests on

`cds_ft_rekey_merge` / `cds_ft_rekey_graft` is the ATOMIC writer
(`ft_rekey_graft_simple_attempt` in `src/fractal-trie/ft-rekey.h`). On the
default build, it still answers NOT_SUPPORTED for 236 of the 3000 generated
shapes. Rank stats on or off give the same 236. Mathieu's position: **there
should be NO refused shapes for rekey**. The one exception is
`cds_ft_rekey_graft` declining a non-empty destination (API semantics).

Two decisions frame every item below:

1. **No spine copy.**  "I don't see any need to do a spine copy anymore now
   that we have the RCU txn mechanism to atomically publish a complex mutation
   to readers."  The rekey composes its src detach edges and its dst attach
   edges into ONE flip txn. When both edits touch one node, that node is
   copied ONCE. The second edit is folded into the copy by identity. The
   carriers for this are:
   - `@pending_pub_slot`, `@pending_dual_slot`, `@pending_del_slot`;
   - the fold modes DROP / PROMOTE / REPLACE / COLLAPSE;
   - `@old_dir_replace`.

   The work is to make those carriers speak every encoding and shape the trie
   has.
2. **Analyse the code before ablating a refusal.**  "opus approach of
   removing refusal and check what breaks hit recurring walls."  For each
   item, do these steps in order:
   - state the SHAPE in trie terms;
   - walk the op's steps against it, from the code;
   - name the step that has no owner / no fresh body / wrong encoding;
   - THEN change it.

   Ablation sweeps only pick a representative shape or confirm a derived
   mechanism.

Live-reader constraint. This is why a same-trie move is harder than the
cross-trie graft/merge, which consume an EXCLUSIVE source. Both sides of a
same-trie move are read concurrently. So:
- the src drop and the dst attach land in one flip;
- the moved top is COW'd (fresh address, `ft_rekey_cow_stop`);
- any node both edits rewrite gets exactly one fresh copy.

## 1. Where the 236 are (default build, exits SUM to the refused count)

| exit (ft-rekey.h unless noted) | n | family |
|---|---|---|
| `-ENOTSUP` shape gate, term `d_src.ppnf == graft_c` | 143 | W1 (83) + W2 (49) + W3 (9) + 2 |
| `-ENOTSUP` shape gate, term `climb_rest_under_graft` | 44 | W1 |
| ft-remove.h `-EDOM` ~1633 / ~1662 / ~3554 / ~3839 | 17 / 6 / 7 / 5 | W4 |
| merge dst-freshness post-loop (~3566) | 14 | W5 |

noskip: 219 refused (same families, ~3839 is 26 there).  nocompress: 12 (all
W5).  The other gate terms (`skip_compressed(graft_p)`, `d_src.pnf == graft_c
&& prep != NOSPLIT`, `climb_steps >= 2`) fire ZERO times.  The root-junction
gate fires zero times since @721e58d0.

The 187 `-ENOTSUP` shapes are ONE family. The FOLD is armed for their PLAIN
versions (served). It is refused for their compressed versions by exactly two
predicates:
- `!climb_compressed` in the arming (ft-rekey.h ~4047, ~4088);
- `!src_cut` / plain-node tests in `ft_rekey_fold_shape_ok` (~1339).

## 2. Standing orders for the executing session (all are Mathieu's)

* Every test, corpus run, repro and oracle runs in its own memcg:
  `systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 -- timeout -s KILL <s> <cmd>`
  FT livelocks allocate ~1 GB/s, and a timeout is not a memory bound.  Suites
  16G.  Heavy rounds go in a `setsid nohup` driver that writes a summary and
  a marker file. The harness's memory guard kills background Bash bursts,
  even at 400+ GB free.
* Run the 17-config gate ONCE at the end, not per fix.
* Per change, run the cheap checks:
  - ft_unit + ft_inv on these six builds: `bctop` (rcu-debug), `bctoprel`
    (release -O2), `b-nocomp`, `b-noskip`, `bclaim` (-DFT_REKEY_CLAIM),
    `breserve` (-DURCU_TXN_DEBUG_RESERVE);
  - the 3000-shape corpus in 4 modes (rank x list) on all six;
  - the 30000-seed rank-on run against the pristine control
    (`scratchpad/ctl`, @721e58d0 -- rebuild a control at the parent of your
    change).

  A leg is green only when rc == 0 AND ok == plan AND zero `not ok` AND zero
  assertion output.
* A refusal is a claim about the trie.  Lifting one needs three things:
  - (a) the derived mechanism;
  - (b) a regression test with the shape pinned and BOTH red arms run
    (refusal restored -> NOT_SUPPORTED; fix ablated -> the corruption);
  - (c) the fix shown to FIRE (tally at the arming and at the consumer).
* A classifier that names a mechanism class has named a test obligation. Run
  that class on every lane that can see it:
  - rank stats ON (counts);
  - list ON (cells);
  - rcu-debug (owner/kind);
  - claim (parks).

  A shape whose grandparent is the ROOT tests nothing about grandparents. The
  root never compresses, fuses, or carries a dual. Construct the below-root
  variant by prefixing every key with one or two bytes. `{xab,xabac,xccc}`
  was found that way.
* One adversarial skeptic per FINISHED change (Agent tool, general-purpose).
  Hand it the pre-built trees and a tool-call budget (~90). Tell it to REFUTE
  with file:line or a run, and to default to refuted on uncertainty.  On the
  last change, two skeptics each found something the corpus did not.
* `grep -i "best.effort"` the staged diff before committing; each such claim
  gets its own skeptic (CLAUDE.md).
* Commit messages carry the mechanism, the measurement and the red arms. They
  end with the session's attribution lines.

## 3. Assets (all built at @088ec182 unless noted)

* Main repo builds: `bctop`, `bctoprel`, `b-nocomp`, `b-noskip`, `bclaim`
  and `breserve`. Each `config.log` holds the configure line.  After a source
  change, run `make -j32` in the build dir. Then run
  `make -C tests/unit test_urcu_ft_unit` and
  `make -C tests/regression test_urcu_ft_inv`.  Test binaries are libtool
  wrappers (`<build>/libtool --mode=execute gdb --args ...`).
* `/mnt/data/efficios/ft-rf`: the instrumented scratch tree, synced to
  @088ec182. It carries:
  - line-neutral refusal tallies: `FT_TALLY(__LINE__)` on every refusal
    exit, `FT_CRUMB` on every goto;
  - per-term tallies 21001..21005 on the `-ENOTSUP` gate;
  - env ablations `ABL_MF` / `ABL_RJ` / `ABL_RJ2` / `ABL_NS`.

  Build `bd` (rcu-debug -O1 -g -DFT_TMP_TALLY -include tally.h). Only `src/`
  builds: tests/common clashes with the force-include.  Recipe to re-sync
  after a commit:
  1. copy the four FT files from HEAD;
  2. run `instrument.py <root>` (in the 6e32e78b scratchpad);
  3. re-apply the hand edits (the python snippets in the 09-10b session log);
  4. `make -C bd/src`.
* Corpus: two drivers.
  - `rkfuzz-rank.c`: 3000 deterministic shapes, `./rkf <seed0> <n>`. Env
    `ALLSTATUS=1` gives per-seed status and `VERBOSE=1` gives keys. Other
    env: `RANK=1`, `LIST=1`. ☠ an EMPTY `RANK=` still counts as set -- use
    `env -u RANK`.
  - `rkfuzz-tally.c`: adds `EXITS=1`. It prints one line per seed naming the
    tally counters that fired -- the per-seed exit attribution.

  Link against a build:
  `gcc -O1 -g -I<repo>/include -I<build>/include -o rkf rkfuzz-rank.c
  -L<build>/src/.libs -lurcu-cds -lurcu-qsbr -lurcu-common
  -Wl,-rpath,<build>/src/.libs`.
* `sk1.c`: one-shot shape driver, `sk1 <rank> <list> <dst> <src> <key>...`.
  It prints the status, the count, verify CLEAN/RED and per-key presence.
* `sweep.sh <bin> <ABLVAR> <seedfile> <out>`: one caged process per seed.
  It classifies rc: 0 clean / 1 BAD / 134 abort / 137 memcg / 124 timeout.
* `final-all.sh`: the detached six-build verification round. Builds run
  sequentially and suites run one build at a time. The corpus runs 4 legs per
  build, then the wide run.
* `N-bd.out`: the 09-10b classifier output. Per refused seed, it gives prep,
  cut, climb steps/reach/compressed, BP/rest kinds, nr_child and
  external_nodes.
* Memory (auto-memory dir):
  - `project_ft_handoff_2026_09_10b`;
  - `project_ft_rekey_refusal_inventory`;
  - `project_ft_compressed_publish_parent_dual_unannounced`;
  - the feedback files named in section 2.

## 4. The work items, in order

### W1 -- the fold with a COMPRESSED RUN in the src chain (127 shapes; 83 DROP + 44 REPLACE/PROMOTE/COLLAPSE)

**Shape.**  The moved top S_top is the ONLY child of a compressed run R.
BP = `d_src.pnf` = R's plain compressed flag. `d_src.pdepth` = R's ENTRY
depth, set by `ft_descent_traverse_compressed`.  R hangs off one of two
places:
- off `graft_c` directly (the climb ARRIVES: t3, 83 shapes);
- off a child P of `graft_c` that survives the drop (t5, 44).

S_top may be an internal node, a run tail or a bare external head. For
example, seed 1097 `{a, cb}` dst bccc src cb is a run over a single head.
Representative corpus seeds (keys / dst / src):
- DROP -- 1097 `{a, cb}` bccc <- cb; 1672 `{ab, b}` caba <- ab; 238
  `{b, ac}` ccab <- ac.
- REPLACE/PROMOTE -- 633 `{aaac, a}` bb <- aaac; 2253 `{abc, acac}` b <-
  acac; 988 `{bcca, bb}` acc <- bcca.

Prefix every key with a byte to get the below-root variants. Turn rank stats
on for every pin.

**What the code does today (derived).**
* `ft_rekey_climb_reaches_graft` (ft-rekey.h ~1033-1120) WALKS runs on the
  NOSPLIT arm (`walk_runs`) and reports `climb_compressed`. It returns
  `climb_top` = R (the last node the drop empties) and `climb_rest` (where it
  stops).  So the climb already models these shapes correctly.
* The arming (~4040-4190) refuses them at three points:
  - the `!climb_compressed` term in both arming conditions;
  - `ft_rekey_fold_shape_ok` (~1339), which requires the EDITED node to be
    plain (for DROP the edited node is `climb_top` = R);
  - the slot identity check `rcu_dereference(*slot) == climb_top` /
    `rcu_dereference(*dslot) == climb_top` (~4053, ~4113). It can never hold
    for a SKIP-encoded slot: a skip word names the run's CHILD, not the run.
* `ft_rekey_fold_freeze_orphans` (~1400-1520) refuses a compressed or
  skip-encoded node in the chain with FT_REKEY_UNCOVERED ("PLAIN INTERIORS
  ONLY, deliberately"). It also requires `nr_child == 1 && !external_nodes`.
* The CONSUMERS are already encoding-agnostic:
  - DROP drops by SLOT ADDRESS (`txn->pending_del_slot = fold_top_slot`,
    consumed by identity in `ft_node_recompact`'s copy loops);
  - REPLACE and COLLAPSE compare and store the RAW slot word
    (`fold_drop_expected`, since @3341ef69 -- "speak the slot's encoded
    word");
  - post-commit reclaim `ft_rekey_detach_free_orphans` already frees
    compressed orphans.
* The detach's own elevation is the model for a compressed orphan. In
  ft-remove.h ~2985-3075, it does three things:
  - it acquires the trailing run through `ft_compressed_node_flag(cn)` with
    its `cds_ft_item_to_metadata((struct cds_ft_inode *) cn)`;
  - it checks `ft_meta_nr_child_load == 1` (a compressed node's state word
    carries nr_child = 1, set by `ft_meta_nr_child_set`);
  - it freezes it with `ft_detach_freeze_one` /
    `ft_flip_txn_record_tombstone`.

  Its upward climb steps depth by `ft_node_span` (~2744), not by 1.

**Why the stated reason for the refusal does not hold here.**  The arming
comment says a compressed link "puts a SECOND path (the SKIP_X dual) on the
same child, and refreshing that dual is a second publish this cut has no owner
for."  For DROP, REPLACE, PROMOTE and COLLAPSE, nothing is REFRESHED. The
slot that held the skip word is dropped from a FRESH copy (graft_c' or P').
The run R and its child word both die with the retired chain.  The dual only
needs refreshing when a compressed node SURVIVES and its child is
republished. That is the compressed-publish-parent case. It is served
@088ec182 by announcing the reserve's dual.

**The change (what, not code).**
1. Arming: replace `!climb_compressed` by the real conditions.
   - The fold may proceed when every node the climb cleared is plain OR
     compressed. A run has exactly one child and no keys BY CONSTRUCTION, so
     the drop always empties it.
   - Keep refusing a SKIP-ENCODED `climb_top` flag. The climb never produces
     one: it walks resolved flags.
   - `ft_rekey_fold_shape_ok` keeps `graft_c` plain (the fold WRITES into
     it). But the `edited` node may be a compressed run when the mode is DROP
     (it is dropped, not edited).
2. Slot identity: compare the node the slot DENOTES, not the word. Use this
   test:
   `ft_flag_to_metadata(ft, *slot) == ft_flag_to_metadata(ft, climb_top)`.
   The helper resolves a skip word to its run's metadata.  Keep the RAW word
   as `fold_drop_expected` for REPLACE/COLLAPSE.
3. Freeze walk: accept a compressed node in the chain, as follows:
   - metadata via `ft_flag_to_metadata`;
   - single-child premise from the state word (`ft_detach_orphan_planlock`
     with `require_single_child`);
   - no `external_nodes` (a run cannot carry any);
   - tombstone as the detach does.

   Depth: the chain may cross a run whose CHILD is below it. Then step by the
   run's span, as the detach's climb does. For the observed shapes, one step
   is one depth (R is BP, `pdepth` is R's entry depth).
4. The freeze walk's `promoted` node stays plain. PROMOTE promotes a keyed
   plain node's head, and a run has no keys.
5. Reservation: a compressed orphan costs the same tombstone record as a
   plain one. Check that the folded-collapse and orphan terms of the txn
   reservation (~4440-4480) still cover the chain.

**Acceptance.**
- Corpus: default 2764 -> ~2891 (+127), noskip +~125, nocompress unchanged.
  0 bad on all 24 legs. The exits still sum to the refused count.
- Tally the arming per mode and the consumer (copy loop / replace_ptr). This
  proves the modes FIRE for these shapes.
- Pin in ft_unit, each on both list modes, with rank stats on for the
  below-root ones:
  - one DROP at `graft_c` == root;
  - one DROP with `graft_c` below the root;
  - one REPLACE and one PROMOTE with the run under a surviving child.
- Red arms:
  - `!climb_compressed` restored -> NOT_SUPPORTED;
  - the freeze walk's compressed acceptance ablated alone -> the walk must
    refuse (UNCOVERED), never corrupt.
- Run `bclaim`. The freeze parks SW on the run's state word, and the op must
  HOLD it.

**Traps.**
- (a) `climb_steps` counts NODES. `d_src.pdepth` is BP's depth. They agree
  only while every cleared node is one byte deep. When in doubt, read
  `ft_node_span`.
- (b) The t3 shapes with S_top a BARE EXTERNAL HEAD go through
  `glue.payload_live` (no COW of the head). The head's old parent is R. Make
  sure the fold's copy loop re-parents the head's back edge. It must not be
  left naming R.
- (c) Do NOT route these through `ft_chain_compress_fused`. The
  fold-collapse design memo already ruled that out: the fold's product is
  graft_c', the collapse's is a run.

### W2 -- the fold with a CUT SOURCE and a surviving BP (49 shapes: 42 REPLACE, 7 PROMOTE, some COLLAPSE)

**Shape.**  The src key ends INSIDE a run R. So `src_cut` > 0,
`d_src.nf` = R, and `d_src.nf_raw` = the skip word BP holds for it.  S_top'
is the run's TAIL, manufactured by `ft_rekey_cow_stop(..., cut)` (~196-340).
That function copies `key_bytes[cut..len)` and the one child. It retires R
WHOLE and hands back the plain flag.  BP = `d_src.pnf` is PLAIN and is a
child of `graft_c` (t3). BP SURVIVES the drop (nr_child >= 2, or one child
plus keys).  Seeds:
- 1413 `{bb, ac, abaa}` c <- aba;
- 690 `{cbbcb, cccc}` ab <- cbb;
- 18 `{aabcb, abbaa}` c <- abba;
- PROMOTE-like 84 `{aacca, a}` cc <- aac;
- 1000 `{a, aaba}` cbac <- aab.

**What the code does today.**  `ft_rekey_fold_shape_ok` refuses on
`!src_cut`. The arming comment gives DROP's reason: "the src slot holds the
RUN and S_top is a COW'd tail, so 'BP is emptied by the drop' is not the
shape at all."  That is true for DROP. It is irrelevant for REPLACE /
PROMOTE / COLLAPSE. There BP survives, and the fold's product is a
DEL-recompacted copy of BP without the run's slot.

The REPLACE arm already captures the RAW word (`dexp = d_src.nf_raw`,
`dslot = d_src.nfp`) when `climb_steps == 0`.  R's retire and post-commit
free are ALREADY OWNED by two places:
- `ft_rekey_cow_stop`;
- the driver's `cds_ft_free_item_deferred(ft, s_top_meta)` (~6002, gated on
  `!(src_cut && glue.old_dir_dropped)`).

So the fold owes R nothing.  Every key under R passes through all of R's
bytes. So a cut moves EVERYTHING below BP's slot, and dropping the whole slot
is the right product.

**The change.**
- Lift `!src_cut` for the REPLACE / PROMOTE / COLLAPSE arm only
  (climb_steps == 0, `climb_rest` == BP). Keep it for DROP.
- Check that `ft_node_other_child` (COLLAPSE) and `ft_node_find_child`
  (REPLACE) are handed the raw skip word (they are).
- Check the count. The moved count `cnt` is the tail's whole subtree.
  `ft_rekey_move_folded` / the graft's `count_delta` must treat a cut exactly
  as an uncut REPLACE (net zero on graft_c's ancestors).

**Acceptance.**
- +49 on default (and the matching noskip count), 0 bad, rank on both list
  modes.
- Pin one REPLACE and one PROMOTE cut shape (below-root variants, rank on).
- Red arm: `!src_cut` restored -> NOT_SUPPORTED.

### W3 -- GLUE prep, src junction is the split node's own child (9 shapes)

**Shape.**  The dst diverges INSIDE the compressed node cn that BP hangs
under (prep GLUE, `graft_c` = cn). In addition, one of these holds:
- BP is cn's single child with one child + keys (PROMOTE-like);
- the source is cut.

Seeds:
- 1227 `{ccccc, cc}` ca <- ccc;
- 1911 `{bac, bacca}` bbccb <- bacc;
- 2890 `{a, bbbacc, bbcc}` ba <- bbbac;
- 2070 `{baaa, aab, baa, ac}` bacabb <- baaa.

**What the code does today.**  `glue.old_dir_replace` is in ft-rekey.h
~3705-3760. `ft_split_compressed_graft_build`'s old-direction arm consumes it
(ft-graft.h ~254-330). It is armed only for `!src_cut && nr_child >= 2`
(REPLACE or COLLAPSE). `glue.drop_old_dir_of` handles the emptied case.  The
9 are the old direction's PROMOTE (one child + keys: the head takes the slot)
and the cut variants.

**The change.**  Add the old-direction PROMOTE to the split's arm. It mirrors
the fold's PROMOTE: the head IS the slot value, and its back edge rides the
re-parent sweep. Also admit a cut source there, on the W2 reasoning.  This is
small. Do it after W1/W2 so their tests exist.

### W4 -- the detach-side `-EDOM` arms in ft-remove.h (35 shapes, 4 sites)

These fire AFTER the graft store, inside the (unfolded) detach. Each site's
own comment names what is owed.  Verify each claim against the code before
acting. These were read, not traced, when this plan was written.

| site | n | the comment's own account | what is owed |
|---|---|---|---|
| ~1633 `publish_parent != raw_parent` (fused collapse) | 17 | the collapse derives its publish target from a RAW back-pointer while the caller's commit re-parents that compressed node; landing on the retired copy left "one node under two parents" | resolve the publish parent read-your-own-writes through the shared descriptor (the pattern `ft_count_walk_survivor_meta` uses), or take the caller's pending re-parent as the hint |
| ~1662 `ft_glue_that_split(ctx, parent_cn)` | 6 | the collapse would absorb a node the caller's graft SPLIT, and republish at the split node's home | the merged node re-based BELOW the split point -- a different product; likely folds into W3's old-direction arms |
| ~3554 compressed `fold_pending` | 7 | the fold's pending child (the graft's cluster top) is compressed / skip-encoded; fusing would retire a never-published fresh node and re-aim its deferred edges | let the fuse absorb a compressed pending top by extending the run with its bytes and RE-AIMING the cluster's deferred edges at the merged node (glue-tracked), or canonicalize the cluster top before the fuse |
| ~3839 compressed LANDING parent after a climb with `src_held_hint` | 5 (26 noskip) | the elevated hint has no great-grandparent pair, and the compressed landing parent's SKIP_X dual needs one; deriving it raw has the staleness the hint avoids | derive `(gp, gp_slot)` for the elevated boundary from the walk that moved, coherently (`ft_resolve_parent_slot` on the boundary's metadata), and pass it in `elevated_hint` -- the same triple the NOSPLIT reserve already carries |

Seeds:
- 1556 `{abb, abac, b}` c <- aba (1633);
- 1304 `{babca, baca}` bbcaac <- babca (1662);
- 1914 `{aa, cbaccc, bb, b, cccbc}` cc <- cbacc (3554);
- 2650 `{aa, aabc, ccc, cbb, aacbcc, bbccab}` bc <- aabc (3839).

Note ~3839 is the NOSKIP-heavy one (26). Plan its test on `b-noskip`.

### W5 -- the merge arm's dst-freshness post-loop (14 shapes; 12 on nocompress -- NOT compression debt)

**Shape.**  The destination is occupied (merge). `ft_merge_build` returns a
LIVE node as the merged top. Its `ft_merge_materialize_suffix`
`suffix_len == 0` exit returns `cn->child` (the shared-run collapse). That is
right for a subtree. It is wrong when that helper produces the merge's TOP.
ft-rekey.h ~3566 refuses it by
`merged_pub == d_dst.nf || !ft_glue_is_fresh(...)`. The reason: the
rekey-coherent reader's two-descent witness needs a FRESH node at the attach
point. The bare-head src leg rests on it too.  Seeds:
- 1424 `{acc, cbc, ac, b}` acc <- b;
- 1212 `{ca, bc, aab, ba}` ba <- aab.

**The change.**  COW the merged top when `ft_merge_build` hands back a node
`ft_glue_is_fresh` does not match. Use the same re-parent-plus-retire
bookkeeping that `ft_rekey_cow_stop` does for the graft arm. Memory
`project_ft_compressed_stop_refusal_is_two_defects` located this.  Ablating
the gate without the COW LOSES A KEY on release (seed 712 of an earlier
corpus). So do not lift it first.

### W6 -- pre-existing rank residue (not a refusal; record, then fix if cheap)

Seed 29976 `{b, abcbab, abcaa, abcbbb, c, bc}` abbca <- abcbab, with rank
stats on. The result is OK and the keys are right, but the check reports
`depth 2: nr_keys mismatch: stored 4, computed 3`.
Identical on the pristine @721e58d0 control.  It is the same family as the
fused-GP double bake (@088ec182), seen from the DETACH's -cnt walk. BP `abcb`
is left one-child and fused. graft_c `ab` is the node whose count is one too
high. Before changing anything, trace the two walks with an env-gated print
at `ft_nr_keys_store`. That method found the rank lane's last four.

### W7 -- the gate

Run it when W1-W5 are in, or when each remaining refusal is written down as
"by design". Run the full 17-config gate from a detached driver:
- light configs in pairs;
- heavy ones one at a time;
- `txndbg`'s ft_unit legs sequential per spacing (memory
  `reference_ft_gate_run_heavy_configs_alone`).

Run `df -i /tmp` first. Delete gate dirs after reading the `.out`.

## 5. Verification recipe (per change)

```
# rebuild (each build dir):  make -j32 && make -C tests/unit test_urcu_ft_unit && make -C tests/regression test_urcu_ft_inv
# suites, caged, one build at a time (unit || inv):
systemd-run --user --scope -q -p MemoryMax=16G -p MemorySwapMax=0 -- timeout -s KILL 1500 <build>/tests/unit/test_urcu_ft_unit
# corpus, 4 modes:   env -u RANK -u LIST | env -u LIST RANK=1 | env -u RANK LIST=1 | env RANK=1 LIST=1
systemd-run --user --scope -q -p MemoryMax=8G -p MemorySwapMax=0 -- timeout -s KILL 900 env ... ALLSTATUS=1 ./rkf-<build> 1 3000
# green = rc 0, ok == plan, 0 not ok, 0 assert lines on stderr; corpus = "== ran=3000 bad=0", served count, exits summed
# wide:  ./rkf-<build> 1 30000 with RANK=1, both list modes, and the SAME on the control build; diff the BAD seed sets
# per-seed exit attribution:  EXITS=1 ./I-<build> (rkfuzz-tally.c against the ft-rf instrumented build)
```

Before the commit of each item:
- one skeptic on the finished diff;
- the best-effort grep;
- the commit message with mechanism, numbers, red arms.

## 6. Traps already paid in this area (do not pay again)

* A release-only measurement reads an unowned SW park as "served". Run
  rcu-debug AND `bclaim`.  Rank stats ON masks one owner assert and exposes
  the count walks. So test legs need OPPOSITE attributes.  `||` between test
  legs hides the second one -- accumulate with `|=`.
* `ft_node_find_child` compares RAW words: pass the encoded word a slot
  HOLDS, reason about the node it DENOTES.
* The count walk follows retired chain levels to their SURVIVOR. A fused
  collapse maps 2-3 levels onto one run. The middle level then FALLS THROUGH
  (no record at its slot).  Charge a body once per walk (identity set,
  @088ec182).
* `pending_pub_slot` / `pending_dual_slot` are single fields: check no two
  producers can fire in one op before adding a producer.
* A refusal inside a BUILD helper can only answer -EAGAIN. A deterministic
  shape then re-plans for ever. So structural refusals answer
  FT_REKEY_UNCOVERED at PLAN time (`ft_rekey_fold_freeze_orphans`'s two-kinds
  rule).
* The 3000-seed corpus rarely produces a fusable non-root grandparent (first
  at seed ~9744); CONSTRUCT below-root shapes by prefixing keys.
* A `RANK=` that is empty still selects rank stats; `env -u RANK`.
* The `#ifdef FEATURE_FT_COMPRESS return false` class: a guard on a BUILD is
  never a shape argument; look for the value the guard is really protecting.
