# Building a binary classifier with `libn00b`

A practical introduction to the n00b ML library (`include/ml/ml.h`)
for engineers who want to *use* it, not derive it.

---

## Applied data science, lightly

Most data-science writing is by mathematicians for mathematicians.
That's fine if you're going to publish papers.  It's a problem if
you're an engineer who'd just like to know whether the input you're
holding is "the kind of thing we care about" — because the answer
is buried under formula notation that's mostly irrelevant to the
decision you actually need to make.

This document is the other thing: **applied data science** for
engineers who don't want to wade through the math.  Just enough
intuition to use the tool well, plus the operational details that
let you get it deployed and watched in production.  When we hit a
math-flavored idea we'll explain it in one sentence and move on.

### What we'll cover

- A specific kind of problem: **binary classification.**  You have
  some kind of input (an event, a process, an HTTP request, a
  document — whatever) and you want a yes/no answer about it.
  Spam-or-not, write-or-read, code-editor-or-not.
- A specific kind of model: a **linear classifier**, trained
  online.  Tens of kilobytes on disk, microseconds per prediction.
- The full lifecycle: rule design → training → save/load →
  deployed scoring → drift monitoring → guided refinement from
  feedback.

### What we won't cover (but you should know exists)

There are lots of related problems and tools that your "I just want
to ship a thing" instincts will eventually want.  None of them are
in this document; many of them will likely show up in n00b over time.
A short map so you know what to search for when:

| When you have… | The technique you want is called… |
|---|---|
| More than two outcomes ("which of these eight?") | **Multi-class classification** (one-vs-rest is the simplest path; n00b's binary classifier can be stacked into one if needed) |
| A continuous output ("how many seconds will this take?") | **Regression** |
| Unlabeled data and you want to find natural groups | **Clustering**.  Especially good for *discovering candidate rules* — clusters often correspond to "kinds of inputs" that you can then write a binary classifier to recognize |
| You want to know which inputs are weird | **Anomaly detection** (often a clustering result run sideways) |
| You're handling text where word order matters | **Embeddings + neural models** |
| You want to know whether two distributions are actually different | **Hypothesis testing** |

For most of these, the gap between "could solve in a notebook in an
afternoon" and "shipped in your binary as a few KB of weights" is
just plumbing.  Engineers tend to under-use these tools because the
literature pretends they're harder than they are.  This document is
trying to push the other way for one specific tool.

---

## The shape of the problem

You're trying to assign each input a yes/no.  Concretely, this
document will train a model that answers:

> Is this process invocation an actor that *produces* source code?

Inputs are events from the [Crayon](https://crashoverride.com/crayon)
warehouse.  Each event describes a process running on the machine
along with structured metadata (executable path, argv, signing
identity, etc.).  We'll auto-label using Crayon's existing rule-based
classifier — `EDITOR` or `AI` bit set means yes, anything else means
no.  We let Crayon's rules be the "ground truth" and train a model
that learns the same answer from a richer rule set.  The payoff is
the model can recognize new editors / AI tools that the rule table
doesn't list yet, and it gives us a confidence number, not a boolean.

You don't need to understand Crayon to follow along.  Anywhere a
"warehouse event" shows up below, just imagine "a structured input
with a few labeled fields."

---

## You already know what a rule is

Here's the framing that makes everything else click.

A rule is something like:

```
if exe_basename == "vim":  EDITOR
```

If you had a perfect rule — one that's never wrong — you wouldn't
need a model.  You'd just write the rule and ship it.

But real rules have *grey space.*  Edge cases, exceptions, things
they catch by accident, things they miss.  And often you have lots of
candidate rules — "exe is vim", "argv contains `-c`", "running from
`/Applications`" — and you don't know which ones really matter or how
much to weigh each one against the others.

**The library trains a weight for each rule you propose.**  When you
say "this could be a rule," you hand it a `(group, value)` pair —
a category like `"LEX"` and a value like `"vim"`.  The library
maintains one float weight per rule and adjusts it on every labeled
example you show it.  After training, the weight tells you two things:

1. **Was your rule any good?**  High positive weight → strong rule
   for the positive class.  High negative → strong rule for the
   negative class.  Near zero → your rule was useless; the data
   didn't support it.
2. **How does it stack up against your other rules?**
   `n00b_ml_strongest_rules` returns the top-magnitude rules across
   all groups so you can see at a glance which ones the model is
   really leaning on.

This is liberating.  You don't need to be right about which rules
matter up front.  You just need to propose a *reasonable collection*
of rules and let the library sort them out.  The cost of a bad
proposal is a near-zero weight; the cost of *missing* a good one is
the model can't learn it.  So lean toward proposing more.

### Examples of rules you'd propose

For our "is this an editor?" classifier:

- "exe basename is `vim`" — strong direct signal, definitely a rule.
- "argv contains `-c`" — vim uses it, but so does python, so it's
  weak on its own.  Worth proposing; the model will tell us.
- "exe path starts with `/Applications/`" — Mac apps live there.
- "the signing identity is `com.apple.vim`" — corroborates the exe
  basename, which is useful when you encounter something *not* named
  `vim` but signed by Apple as vim.
- "argv has more than 10 items" — a rough heuristic that build tools
  tend to have long argv.  Probably weak, but cheap to include.
- "current working directory is somewhere under `/Users/`" — tells
  you the user is involved.  Indirect signal; the model decides.

Each of those is a rule you can imagine writing by hand.  Each, when
you hand it to the library, gets a learnable weight.  Some will work;
some won't.  You don't have to be right.

### What makes a *good* rule?

A good rule is one that, when it fires, actually shifts your guess
about the answer.  Operationally:

- **Categorical strings that are diagnostic.**  An exe basename, a
  content-type, a path component.  When you see them, you reach a
  conclusion.
- **Bucketed numbers, not raw numbers.**  Never propose "argv length
  is 17" as a rule — that rule fires for that one input, never
  again, and the model can't learn anything.  Propose "argv length
  is in 11+" — coarse buckets accumulate enough hits to be
  informative.
- **Shape categories.**  Path-prefix bucket (`/Applications/...`,
  `/usr/bin/...`, `/opt/homebrew/...`).  Path-depth bucket
  (`depth:1-2` vs `depth:7+`).  These are rules you'd write by
  hand; they're rules you can propose by hand too.
- **Combinations you couldn't easily write by hand.**  This is where
  the model earns its keep.  Linear models can't combine rules the
  way a deep network does, but if you propose a few dozen
  single-rule signals, the model's ability to weight them jointly
  outperforms any one of them alone.

### What makes a *bad* rule?

- **Never-repeating values.**  PIDs, UUIDs, timestamps, full paths.
  "argv[0] is `/Users/viega/projects/foo/build/Debug/bin/x_a8f2d3`"
  is a single observation that will never fire again.  Strip these
  to repeating shapes (basename + path-prefix bucket).
- **Always-true rules.**  If every input fires the same rule, the
  model just rolls it into the bias term and you learn nothing.
- **Rules whose evidence comes from outside the input.**  If a rule
  depends on something not in the input, it's not really a rule
  about the input — it's noise.

A useful sanity check before proposing a rule: *can you say in one
sentence why this rule, in isolation, would fire more often for one
class than the other?*  If you can't, it's not worth the slot.

---

## Rule groups: organizing your rule collection

A **rule group** is a named pool of rules.  Every rule you propose
is tagged with a group.  We split the rule space into multiple pools
because:

1. **Collisions stay local.**  Internally, the library hashes
   `(group, value)` into a slot in that group's pool.  If two
   unrelated rules — say, exe-basename `"vim"` and path-depth
   `"depth:7+"` — happened to land in the same slot in one giant
   table, their weights would interfere.  Different groups live in
   disjoint slot ranges, so collisions only happen *within* a group.

2. **You can reason about kinds of rules.**  When you ask "what
   rules did the model learn from?" you can tell whether it's
   leaning on lexical strings, structural shape, or environmental
   context — at a glance.  Useful for sanity-checking and for
   knowing where to add more proposals.

The default layout used throughout this tutorial:

| Rule group | Default size | What kind of rules go there |
|---|---|---|
| `LEX`  | 16 K rules | Lexical strings: "exe basename is X", "argv token is Y", "signing ID is Z" |
| `FLOW` | 4 K rules  | Relationships: "event kind is X", "ancestry is N deep" |
| `GEOM` | 256 rules  | Shape categories: "argv has N items", "exe path is N components deep" |
| `ENV`  | 1 K rules  | Environmental: "exe is in /Applications", "cwd is under /Users" |

You don't have to use these names.  They're just labels.  The save
format records names + sizes, so loading a model whose groups don't
line up with what your code expects fails cleanly.

### How big should rule groups be?

A tradeoff between memory, collision rate, and visibility:

- **Memory** = total rule count × 4 bytes.  16 K × 4 = 64 KB.
- **Collision rate** — too few slots means rules with different
  values share weights.  The model can't tell them apart.
- **Visibility** — when match-tracking is on, the library remembers
  which raw values landed in each slot, for human-readable
  inspection.  Smaller groups give shorter lists per slot.

Defaults are good for "tens of thousands of events, a few hundred
distinct values per group."  Bump LEX if you're dealing with a
vocabulary of tens of thousands of distinct strings.  Shrink GEOM
further if you only have a dozen shape labels.

---

## The walkthrough

The rest of the document follows a real session.  The goal is to
make you comfortable with the *output* — what you see, what to look
for, what each piece means — without you having to run anything.

The classifier is the "is this actor producing source code?" model
described above.  Rules come from each event's actor metadata,
sorted into LEX/FLOW/GEOM/ENV.  Auto-labels come from Crayon's
existing classification bits.

### Phase 1 — start fresh

```
$ build_debug/crayon_dev_activity --every 25 --save /tmp/dev.bin
crayon_dev_activity: bootstrapping a fresh model — save with --save PATH
```

`--every 25` prints a status block every 25 successfully-trained
events (or every 5 seconds, whichever first).  `--save /tmp/dev.bin`
writes the model when you Ctrl-C.

The classifier is now subscribed to the event stream and silently
training online.

### Phase 2 — drive a positive-class burst

In another terminal, fire vim a bunch:

```bash
for i in $(seq 1 25); do
  vim -es -c 'q' /tmp/x.txt < /dev/null
  sleep 0.4
done
```

Each invocation produces a process-spawn event.  The actor classifies
as `EDITOR`, so our auto-labeler treats it as positive.  Watch:

```
=== events: 50   pos: 46   neg: 4
skipped: 79   dropped: 0 ===
 +0.607  LEX   rule=9926   vim (n=92)
 -0.305  LEX   rule=470    ninja (n=10)
 +0.304  LEX   rule=7439   com.apple.vim (n=46)
 +0.304  LEX   rule=4904   -c (n=46)
 +0.304  LEX   rule=13206  q (n=46)
 +0.304  GEOM  rule=247    depth:3-4 (n=46)
 +0.304  LEX   rule=11129  -es (n=46)
 +0.304  ENV   rule=124    exe:usrbin (n=46)
```

How to read it.  The header:

| Field | Meaning |
|---|---|
| `events: 50` | Training observations actually used. |
| `pos: 46 / neg: 4` | Class breakdown.  46 positives (the vim invocations); 4 ambient negatives. |
| `skipped: 79` | Events that arrived but lacked the metadata we needed.  Always non-zero in real streams.  Not an error. |
| `dropped: 0` | Events thrown out because the cross-thread queue was full.  Stays 0 in normal use. |

Each strongest-rules row is `<weight>  <group>  rule=<id>  <example> (n=<count>)`.

- `weight` is signed.  Sign tells you which class.  Magnitude tells
  you how strong the rule is.  Strongest-rules is sorted by
  `|weight|`, so rules from *both* classes can show up.
- `(n=…)` is how many times this rule has fired during training.
  Use it to distinguish "high weight from a single observation"
  (suspect) from "high weight backed by 100 hits" (well-supported).
- `example` is the most-frequent raw value seen for this rule
  during this session, populated only when match-tracking is on.
  Useful to human-read your learned rules; turn it off in prod to
  save memory.

What rules did the model just learn?

| Rule | Verdict |
|---|---|
| `exe basename == "vim"` (LEX) | Strong positive (+0.607).  We expected this. |
| `signing ID == "com.apple.vim"` (LEX) | Strong positive (+0.304).  Corroborates the basename. |
| `argv contains "-c"` (LEX) | Positive but moderate (+0.304).  Works for *our* vim invocations but probably also fires for python, so the model will need to nuance this later. |
| `argv contains "q"` (LEX) | Same story; we passed `:q` so vim stored "q" as an argv token. |
| `path depth is 3-4 components` (GEOM) | Positive.  `/usr/bin/vim` is depth 3. |
| `exe path starts with /usr/bin/` (ENV) | Positive.  This rule will likely become *less* informative as more data arrives, since lots of non-editor things also live in `/usr/bin/`. |
| `exe basename == "ninja"` (LEX) | Negative (-0.305).  Some `ninja` events slipped in from a background build that was running concurrently.  The model correctly identified them as negative-class. |

This is the rule-design moment paying off.  We didn't need to be
right about which rules would dominate — we proposed a few, the model
ranked them, and we get to read the ranking.  The signing ID rule
turned out useful; the path-depth rule turned out useful; the
argv-contains-`-c` rule will probably weaken as we see more data.

### Phase 3 — the world doesn't pause

Drive 50 python invocations:

```bash
for i in $(seq 1 25); do
  python3 -c 'pass' < /dev/null
  sleep 0.3
done
```

Python classifies as `INTERPRETER`, which our auto-labeler treats as
negative (we asked "is it *producing* code?" — interpreters consume
and run code, they don't write it).

You'd expect `python3` to show up in strongest-rules with a negative
weight.  Reality:

```
=== events: 1500   pos: 52   neg: 1448 ...
 -1.040  GEOM  rule=92     depth:7+ (n=1314)
 -0.971  ENV   rule=6      cwd:none (n=1500)
 -0.902  ENV   rule=237    exe:brew (n=1140)
 -0.781  LEX   rule=470    ninja (n=532)
 -0.715  FLOW  rule=240    anc:6+ (n=1261)
 +0.609  LEX   rule=9926   vim (n=100)
 -0.608  GEOM  rule=179    argv:11+ (n=966)
 -0.581  LEX   rule=11368  clang-21 (n=1188)
```

Between phase 2 and phase 3, ~1,400 build events poured in from a
parallel compile that was running on the machine.  The model trained
on all of them.  Python *did* get learned — it has weights somewhere
— but the build-class rules (`clang-21`, `ninja`, `exe:brew`,
`depth:7+`, `anc:6+`) all have stronger weights from much higher
counts, so they crowd out the python rules in the top 8.

Two takeaways:

1. **Real streams don't pause for tutorials.**  Whatever's happening
   on the machine ends up in the model.  In production you filter at
   subscription time, train during quiet windows, or accept that the
   model learns the actual distribution rather than the toy one.

2. **Strongest-rules shows the strongest, not all.**  A rule can be
   present, learned, and reasonable without surfacing in your
   printed top 8.  The full weight array is available; iterate it
   directly if you need to inspect a specific rule.

### Phase 4 — strong rules can win against noise

Drive 100 more vim invocations:

```
=== events: 1700   pos: 246   neg: 1454 ...
 +1.418  LEX   rule=9926   vim (n=488)
 -1.044  GEOM  rule=92     depth:7+ (n=1320)
 -0.906  ENV   rule=237    exe:brew (n=1146)
 -0.780  LEX   rule=470    ninja (n=532)
 +0.709  LEX   rule=11129  -es (n=244)
 ...
```

`vim` is now top-1 at +1.418, beating `depth:7+` at -1.044.  The
positive cluster (`-es`, `com.apple.vim`, `q`, signing ID) all moved
in lockstep from +0.304 → +0.709.

The takeaway: a strong, repeated signal can win even when
outnumbered ~6:1 by noise.  Linear models are surprisingly robust to
class imbalance.  The way they handle it is by pumping up the
per-rule magnitudes on the under-represented class — those rules
have to "fight harder" to overcome the bias toward the majority.

### Phase 5 — save

Press Ctrl-C in the running classifier:

```
=== events: 1708   pos: 252   neg: 1456 ...
crayon_dev_activity: wrote 87134 bytes to /tmp/dev.bin
```

87 KB.  That's the whole shippable model.  It contains:

- A 24-byte header: magic, version, flags, bias, total rule count.
- A rule-group table: one entry per group with name + size.
- A weights array: one float per rule, in group order.
- An optional trainer-state tail (when produced by `trainer_save`):
  hyperparameters + observation count.

Format is versioned.  A loader written today rejects a too-new blob
with a clear error.

### Phase 6 — load and refine

```
$ build_debug/crayon_dev_activity --load /tmp/dev.bin --every 25
crayon_dev_activity: loaded base from /tmp/dev.bin — refining with correction layer + drift monitor
```

This is the *production* shape.  The loaded weights are frozen — no
further changes to the deployed predictions unless you explicitly
choose.  Two new pieces appear in the status output once events
start landing:

```
=== events: 1   pos: 0   neg: 1 ...
residual mean: 0.0003549   ewma: 0.0003549
weight drift: 0.003884 ===
```

#### Residual: the "is the deployed model still right?" signal

Both `residual mean` and `residual ewma` measure the same thing —
how often the deployed model is wrong, in squared-error terms:
`(predicted_probability - actual_label)²`, averaged.  They average
differently.

- **`residual mean`** is the simple long-term average.  Slow to
  react.  Tells you the long-run accuracy of the deployed model.
- **`residual ewma`** is exponentially-weighted: recent events count
  more, older events decay out.  Reacts quickly to changes.  Tells
  you whether things just broke.

The two together are the actionable signal:

| Mean | EWMA | Interpretation |
|---|---|---|
| Low | Low | Model is right both historically and recently.  Healthy. |
| Low | Climbing | World shifted recently.  Investigate. |
| Climbing | High | Model has been wrong for a while.  Time to retrain. |
| Climbing | Falling | Recovered (or world stabilized).  Watch a bit longer. |

A simple production alarm: fire when `ewma > k * mean` for some
`k` (say 5×).  Don't fire on isolated EWMA blips.

#### Weight drift: the "would retraining produce something different?" signal

`weight drift` is the L2 distance between the deployed weights and
the **shadow trainer's** weights — a parallel trainer that keeps
learning from new events as if it were starting fresh.  Drift
answers: "if we retrained from scratch on the data we've seen since
deployment, how different would the result be?"

On its own, drift is just an activity counter — it grows whenever
anything happens.  It becomes informative when you read it *with*
the residual:

- Low residual + climbing drift = "we've seen lots of data, model
  still right."  Don't retrain unless you're optimizing.
- Climbing residual + climbing drift = "world has changed and a
  retrain would land somewhere different."  Time to act.

### Phase 7 — in-distribution traffic stays calm

Drive a balanced mix similar to what we trained on:

```bash
for i in $(seq 1 30); do
  vim -es -c 'q' /tmp/x.txt < /dev/null
  python3 -c 'pass' < /dev/null
  sleep 0.4
done
```

Result:

```
=== events: 4139   pos: 70   neg: 4069 ...
residual mean: 0.001226   ewma: 0.01963
weight drift: 1.362 ===
```

Both residual numbers stayed *low* — vim was correctly predicted
positive, python correctly negative.  Drift climbed because the
shadow trainer kept training, but that's volume, not a quality
signal.  Strongest-rules hasn't reordered.

This is the **good steady state.**

### Phase 8 — novel input, drift detection in action

Drive emacs (which the base model has never seen) with no UI:

```bash
for i in $(seq 1 60); do
  emacs --batch --eval '(message "x")' < /dev/null > /dev/null
  sleep 0.3
done
```

Result:

```
=== events: 4813   pos: 510   neg: 4303 ...
residual mean: 0.04389   ewma: 0.26
weight drift: 2.456 ===
```

**EWMA jumped from 0.0196 → 0.26 — a 13× spike.**  That's the alarm.

What happened, in rule terms: the base model never had a rule for
"exe basename == emacs."  So when emacs events arrive, the LEX rule
for `emacs` has near-zero weight, and the model predicts
~`sigmoid(0)` = 0.5 for every emacs event.  The auto-label says
`1.0` (EDITOR bit set).  Squared error per event = `(0.5 − 1.0)² =
0.25`.  EWMA averaged in those high-error samples and surfaced the
shift immediately.

The mean climbed slower (0.001 → 0.044) because it's averaged over
thousands of in-distribution events that are still being predicted
correctly.  **The ratio between mean and EWMA is more informative
than either alone.**

Strongest-rules *didn't* reorder during this phase.  The
**correctable model**'s correction layer is updating with
`feedback_learning_rate = 0.005` — small by design — so even after
60+ disagreements the correction hasn't accumulated enough weight on
the `emacs` rule to push it into the top 8.  This is intentional: in
production you don't want the deployed model to lurch in response to
unusual events; you want to *detect* them, decide whether to adapt,
and adapt slowly when you do.

If you wanted faster adaptation you'd raise
`feedback_learning_rate`.  But faster = noisier.  The library
defaults conservative.

### Phase 9 — corruption rejection

The save format has structural validation built in.  Make a corrupt
blob and try to load it:

```
$ head -c 5 /tmp/dev.bin > /tmp/bad.bin
$ build_debug/crayon_dev_activity --load /tmp/bad.bin
load: blob ended mid-record (truncated)
```

Other corruption modes return the same kind of clean error:

```
load: bad magic (not an n00b ML blob)
load: unsupported blob format version
load: blob header is internally inconsistent
load: blob has no trainer-state tail   # n00b_ml_trainer_load on a base-only blob
```

Always handle these — they're not crashes, they're explicit error
returns from the load functions (`n00b_ml_scorer_load`,
`n00b_ml_trainer_load`, `n00b_ml_correctable_load` — all return
`n00b_result_t<...>`).

---

## What didn't fit

### Class imbalance

Our demo had ~6:1 negatives, and the model handled it fine.  For
more skewed data (1:100 or worse) use the per-input `.weight` kwarg
to upweight the rare class:

```c
n00b_ml_trainer_observe(trainer, input, /*label=*/1.0f, .weight = 10.0f);
```

### Threshold selection

The default 0.5 threshold for converting probability → yes/no is
fine for symmetric problems.  For asymmetric costs (a false negative
is 10× as bad as a false positive, say), pick a threshold from
held-out validation data:

```c
n00b_ml_evaluation_t *e = n00b_ml_evaluation_new();
// for each held-out (input, label) pair:
n00b_ml_evaluation_record(e, n00b_ml_scorer_predict(scorer, input), label);

float t = n00b_ml_evaluation_pick_threshold(e, N00B_ML_THRESHOLD_BY_RECALL,
                                            .target = 0.95f);
n00b_ml_confusion_t c = n00b_ml_evaluation_confusion(e, t);
double precision = n00b_ml_confusion_precision(&c);
double recall    = n00b_ml_confusion_recall(&c);
double f1        = n00b_ml_confusion_f1(&c);
double auc       = n00b_ml_evaluation_auc(e);
```

Available policies: `N00B_ML_THRESHOLD_BY_F1`,
`N00B_ML_THRESHOLD_BY_YOUDEN`, `N00B_ML_THRESHOLD_BY_PRECISION`,
`N00B_ML_THRESHOLD_BY_RECALL`.

### Weight decay

The trainer constructor takes `.weight_decay`.  It shrinks all
weights toward zero by a small fraction on every step, preventing
weights from running away when you have many rules and few examples
per rule.  Defaults to `0.0`.  A reasonable starting point is
`1e-4`; tune up if you see weights ballooning, down if learning
stalls.

(In the literature this knob is called *L2 regularization*.  Same
idea; we use the more descriptive name in the API.)

### Other DS techniques you might reach for

If you find yourself hitting any of the following situations, the
binary classifier is the wrong tool — use the indicated technique
instead.  None of these are in n00b yet, but most are coming:

- **You don't know what your classes are yet.**  Use clustering to
  find natural groups in your data first.  Often the clusters
  themselves become the classes you'd then build classifiers for.
- **You have more than two outcomes.**  Multi-class classification.
- **You want a continuous output.**  Regression.
- **You want to flag unusual inputs without having labeled
  "unusual" examples.**  Anomaly detection (typically clustering with
  a distance threshold).
- **You're working with text where word order or context matters.**
  Embeddings + a richer model.

---

## API quick reference

```c
#include "ml/ml.h"
```

### Bootstrapping a model

```c
n00b_ml_trainer_t *t = n00b_ml_trainer_new(.learning_rate = 0.1f,
                                           .weight_decay  = 1e-4f,
                                           .track_matches = true);

// Define your rule groups.  IDs are how you'll record matches later.
n00b_ml_rule_group_id_t lex
    = n00b_ml_trainer_define_rule_group_cstr(t, "LEX",  1u << 14);
n00b_ml_rule_group_id_t flow
    = n00b_ml_trainer_define_rule_group_cstr(t, "FLOW", 1u << 12);
n00b_ml_rule_group_id_t geom
    = n00b_ml_trainer_define_rule_group_cstr(t, "GEOM", 256);
n00b_ml_rule_group_id_t env
    = n00b_ml_trainer_define_rule_group_cstr(t, "ENV",  1u << 10);

// One input object, reused across observations.
n00b_ml_input_t *x = n00b_ml_input_new(t->rules);

// Per training observation:
n00b_ml_input_reset(x);
n00b_ml_input_match_cstr(x, lex,  "vim");          // "exe is vim"
n00b_ml_input_match_cstr(x, env,  "exe:usrbin");   // "exe in /usr/bin"
n00b_ml_input_match_cstr(x, geom, "argv:3-5");     // "argv has 3-5 items"
n00b_ml_trainer_observe(t, x, /*label=*/1.0f);

// Inspect what the model learned:
n00b_list_t(n00b_ml_learned_rule_t) top
    = n00b_ml_strongest_rules(t->model, t->rules, 8);

// Save:
n00b_buffer_t *blob = n00b_ml_trainer_save(t);
// write blob->data[0..blob->byte_len) wherever you like.
```

### Deploying

```c
n00b_buffer_t *blob = ...;   // read from disk, network, etc.
n00b_result_t(n00b_ml_scorer_t *) r = n00b_ml_scorer_load(blob);
if (n00b_result_is_err(r)) {
    fprintf(stderr, "%s\n", n00b_ml_err_str(n00b_result_get_err(r)));
    return 1;
}
n00b_ml_scorer_t *s = n00b_result_get(r);

// Per inference:
n00b_ml_input_t *x = n00b_ml_input_new(s->rules);
n00b_ml_rule_group_id_t lex;
n00b_ml_lookup_rule_group(s->rules, n00b_string_from_cstr("LEX"), &lex);

n00b_ml_input_reset(x);
n00b_ml_input_match_cstr(x, lex, "...");
float p   = n00b_ml_scorer_predict(s, x);     // probability 0..1
bool  yes = n00b_ml_scorer_classify(s, x, /*threshold=*/0.5f);
```

### Refining + monitoring

```c
n00b_ml_correctable_t *c
    = n00b_ml_correctable_new(s, .feedback_learning_rate = 0.005f);
n00b_ml_monitor_t *m
    = n00b_ml_monitor_new(s, .shadow_learning_rate = 0.05f);

// Per labeled event:
n00b_ml_monitor_observe(m, input, label);
float p = n00b_ml_correctable_predict(c, input);
if ((p > 0.5f) != (label > 0.5f)) {
    n00b_ml_correctable_correct(c, input,
        label > 0.5f ? N00B_ML_FB_FALSE_NEGATIVE
                     : N00B_ML_FB_FALSE_POSITIVE);
}

// Read the monitor:
double mean  = n00b_ml_monitor_residual_mean(m);
double ewma  = n00b_ml_monitor_residual_ewma(m);
double drift = n00b_ml_monitor_weight_drift(m);
```

Full reference is in `include/ml/ml.h`.

---

## Common pitfalls

These are real bugs we hit while building the demo.  None of them
are about ML; all of them are about plumbing.

### 1. Register file-scope globals as GC roots

If you stash any of these in a file-scope global, you must call
`n00b_gc_register_root(your_global)` after `n00b_init`.  Otherwise
the next garbage collection moves the underlying allocations and
your stale pointer faults the next time you touch it.

```c
static struct {
    n00b_ml_trainer_t *trainer;
    n00b_ml_input_t   *input;
} g_app;

int main(int argc, char **argv) {
    n00b_runtime_t rt;
    n00b_init(&rt, argc, argv);
    n00b_gc_register_root(g_app);   // <— don't forget
    // ...
}
```

Symptom of forgetting: SIGSEGV deep in `n00b_ml_trainer_observe` after
a few hundred training events.

### 2. Don't call ML APIs from foreign threads

n00b's allocator and GC require the calling thread to be registered
with the runtime.  Threads created via `n00b_thread_spawn` are
registered automatically.  A pthread or a libdispatch queue thread
is *not*.  The right pattern is:

- Foreign thread does no n00b work.  Just enqueues the raw event
  onto a lock-free SPSC ring.
- An `n00b_thread_spawn`'d worker dequeues and does all the n00b work.

`examples/crayon_dev_activity/main.c` shows the pattern.  Symptom of
getting it wrong: the worker hangs the first time it tries to
allocate.

### 3. Skipped events are normal

Many event types in real streams don't carry the metadata you're
filtering on.  The library counts these in `skipped:` and they're
not errors.  `skipped >> events` is common.

### 4. Strongest-rules only shows the strongest

Rules below the K cutoff are still in the model and still
participating in inference.  If you want to inspect specific rules,
increase K, or walk `model->weights.data` directly.

---

## Going further

- `examples/crayon_dev_activity/main.c` — this tutorial's source.
- `examples/crayon_interp_lang/main.c` — same architecture, smaller
  problem (interpreter language detection).  Good place to start
  reading example code if `dev_activity` feels too much.
- `examples/crayon_api_crud/main.c` — same architecture, evaluation-
  driven (uses `n00b_ml_evaluation_t` for threshold selection).
- `include/ml/ml.h` — full public API.  Every type has a Doxygen
  block; every kwarg is documented.
- `pii-api.md` (top-level) — the design doc that motivated the
  library.  Has the longer-form rationale for the hashing-trick +
  linear model architecture choice.
