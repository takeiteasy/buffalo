# Design

## Two universes

buffalo's code splits into two disjoint bodies that share no source, only a
runtime data-layout contract. Copied wholesale from cccc's `ccccl` example.

| Universe | Files | Compiled by | Runs |
|---|---|---|---|
| **Comptime** | `src/buf_comptime.c` (the pass driver) + the `buf_rx` / `buf_tokcheck` / `buf_nfa` / `buf_dfa` / `buf_grammar` `.h`/`.c` module pairs, plus `include/buffalo/buf_emit.h` | cccc's comptime VM | at compile time; never reaches the generated program |
| **Runtime** | `runtime/buf_rt.{h,c}` | system `cc` | in the final program; zero cccc dependency |

Each comptime module is an ordinary `.h`/`.c` pair: the header declares, the
`src/buf_*.c` defines. `src/buf_comptime.c` pulls the module bodies it needs
straight in with `#include @comptime "buf_rx.c"` … (so `cccc` runs with
`-Isrc`); the bodies compile into the comptime program and are callable
recursively. The same `.c` files link into the host unit tests under a plain
`cc`. `buf_emit` stays header-only — it uses the reflection builtins and is
pulled in the same way, the shape `ccccl` uses for its emission code.

The module code is dependency-free, arena-based C: fixed-size arrays, no
`malloc`, `snprintf` into an error buffer, first-error-wins sticky flag
(they cannot call `MacroErrorAt` directly because they must also compile
under a plain `cc` for the host unit tests). `src/buf_comptime.c` is the
boundary: it is the only place that calls `MacroErrorAt`, converting a
module's sticky error string into a comptime diagnostic pointed at the
`.bflo` file.

`#include @shared "buf_rt.h"` (not `@comptime`) is deliberate: a `Quote()`
template that names an extern — `buf_run`, the table symbols — is rejected by
cccc's identifier resolver unless that extern arrived via `@shared`.

### Comptime modules

- `buf_rx.{h,c}` — spec-file + regex reader: `.bflo` text → regex AST,
  `line:col` per node. Character classes are desugared into a 256-bit byte set
  at parse time (so alphabet partitioning is a set-grouping pass). A rule
  whose regex is nullable is rejected here with a `line:col` error. It also
  reads the optional `%grammar` section (see below) — productions, resolved
  terminal/nonterminal symbols, `%start` — with the same `line:col`
  diagnostics; `buf_grammar.{h,c}` (below) builds LALR(1) parser tables from it.
- `buf_tokcheck.{h,c}` — validate a checked-in `<name>_tokens.h` against
  `%tokens`.
- `buf_nfa.{h,c}` — Thompson construction: regex AST → ε-NFA. One fragment
  per AST node; each NFA state has at most one labelled (byte-set) edge and up
  to two ε edges, which covers every fragment. Each rule's fragment ends in an
  accept state tagged with the rule index; all rule fragments hang off state 0
  down a two-ε spine (a rule count above two rules out one fan-out state). A
  nullable *sub-expression* — `(a?)+`, `(a*)*` — survives `buf_rx.h` (only a
  nullable *rule* is rejected) and puts an ε cycle in the NFA, so every ε walk
  is worklist + visited-set, never recursive. Fragment entry/exit come back
  through `int *` out-params: the fragment cases already name their endpoints
  as plain locals, so pointer threading keeps the recursion allocation-free
  with no wrapper type.
- `buf_dfa.{h,c}` — subset construction: ε-NFA → DFA over alphabet
  equivalence classes (not raw bytes). The alphabet partition starts with all
  256 bytes in one class and splits on each CLASS-leaf byte set, then
  **renumbers classes by first appearance over bytes 0..255** so the numbering
  is deterministic and hash/iteration-order independent. Each DFA state is a
  sorted run of NFA-state indices in one **shared pool** (`set_off`/`set_len`
  index into it), capped independently of the DFA state count so the two
  arena knobs can be tuned separately; runs are collected by sweeping NFA ids
  ascending, so they double as the identity key. State ids are handed out in
  worklist **discovery order** only. Each DFA state carries the winning rule
  index (lowest rule index among the NFA accept states it contains — earlier
  rule wins ties), and `rule_token[r]` maps a rule to its `TOK_*` **value**
  (`tok_index + BUF_TOK_FIRST_USER`), or `-1` for `%skip`. Construction ends by
  asserting `accept[start] < 0` — the contract `buf_run` relies on with no
  runtime guard — and reports a violation at the offending rule's `line:col`.
  An **opt-in** Moore minimisation pass (`buf_dfa_minimize`, `-D BUF_MINIMIZE`)
  can then run: initial blocks keyed on the exact `accept[]` value so "earlier
  rule wins" survives a merge, a dead edge (`-1`) compared as its own
  signature entry, and renumbering by each block's least old state id — which
  keeps `start == 0` and lets `next[]` / `accept[]` compact in place. It is
  off by default because it only trims ~4–8 % of states for real lexer specs
  (subset construction already lands near the minimal DFA) while adding its
  own `O(passes · nstates · nclass)` pass to the comptime hot path — see
  [performance.md](performance.md).
- `buf_grammar.{h,c}` — LALR(1) parser tables over the `%grammar` section's
  productions. See [The `.bflo` grammar section](#the-bflo-grammar-section)
  and [LALR(1) table construction](#lalr1-table-construction) below. Built by
  `tests/t_grammar.c` under a plain `cc`, and `#include @comptime`'d into
  `src/buf_comptime.c` under `buffalo parse` / `-D BUF_EMIT_PARSER` (kept out
  of a plain `buffalo lex` run, which does not need it).
- `buf_emit.h` — DFA → four file-scope `static const` tables (raw
  `GlobalVarSetInitData` blobs) + the `buf_next` wrapper fn; under `-D
  BUF_EMIT_PARSER` also the four LALR(1) tables + the `buf_parse_tree`
  wrapper. Header-only and unlike the other modules never split into a `.c`:
  it uses cccc's reflection builtins, is pulled straight into
  `src/buf_comptime.c` with `#include @comptime`, and is never compiled by a
  plain `cc`. Its output builds with a stock `cc` against `runtime/buf_rt.c`
  and an example `_main.c`.

### Runtime module

`runtime/buf_rt.{h,c}` holds everything that is not generated: the input buffer,
`line:col` tracking, and the longest-match driver loop `buf_run` itself. Keeping
the loop here rather than emitting it means the emitter produces only *data*
(`static const` arrays) plus a one-line wrapper — which sidesteps the `Quote()`
sharp edges around `while` / `break` / `continue` (see below).

## Generated table file — shape

`bin/buffalo lex calc.bflo` (which runs `cccc -c=generated src/buf_comptime.c
-Iinclude/buffalo -Isrc -D BUF_SPEC='"calc.bflo"'`) produces a `.gen.c`
containing four file-scope `static` tables and one wrapper:

```c
static const BufClass buf_dfa_class[256];             /* byte -> class 0..NCLASS-1 */
static const short buf_dfa_next[NSTATES * NCLASS];    /* flat; -1 = dead */
static const short buf_dfa_accept[NSTATES];           /* rule index, -1 = non-accepting */
static const short buf_rule_token[NRULES];            /* rule -> TOK_* kind, -1 for %skip */

BufToken buf_next(BufLexer *lx) {
    return buf_run(lx, buf_dfa_class, buf_dfa_next,
                   buf_dfa_accept, buf_rule_token, NSTATES, NCLASS, 0);
}
```

`examples/digits_tables.c` is a hand-written file of the same shape. It is
the reference the emitter is diffed against (`make native`'s three-way
parity check) and the one build path that needs no cccc at all. The emitter
matches its `static const` and its `unsigned char` class-table element type
(emitted as the `BufClass` typedef — see the class-table note below).

`bin/buffalo parse expr.bflo` (which adds `-D BUF_EMIT_PARSER -D
BUF_STOP_AFTER=7`) writes a `.parse.gen.c` with everything above **plus** the
LALR(1) tables and a second wrapper:

```c
static const int buf_lalr_action[NSTATES * NTOK];      /* BUF_LALR_ACT_*-packed */
static const int buf_lalr_goto[NSTATES * NNONTERM];    /* state id, -1 = none */
static const int buf_lalr_prod_lhs[NPRODS];            /* prod -> nonterminal index */
static const int buf_lalr_prod_len[NPRODS];            /* prod -> RHS length */

int buf_parse_tree(BufParser *ps, BufLexer *lx) {
    return buf_parse(ps, lx, buf_dfa_class, buf_dfa_next, buf_dfa_accept,
                     buf_rule_token, NSTATES, NCLASS, 0,
                     buf_lalr_action, buf_lalr_goto,
                     buf_lalr_prod_lhs, buf_lalr_prod_len,
                     NTOK, NNONTERM, START_STATE);
}
```

### How the tables are emitted

Each table is one `GlobalVar(name, MakeArray(elem_ty, len))` +
`GlobalVarSetInitData(var, raw_bytes, byte_len)` + `GlobalVarSetStatic(var, 1)`
+ `PublishNodeAt(var, ...)`. `GlobalVarSetInitData` takes a **raw byte blob**
whose length must equal the emitted type's `ty->size`, so the emitter `memcpy`s
each table's **live prefix** (`nstates*nclass` for `next`, etc. — never
`sizeof` the whole arena) — it does not build thousands of `MakeIntLiteral`
nodes. `InitArray` / `CompoundLiteral` are not usable here: `CompoundLiteral`
is function-scope-only in cccc V1. cccc serialises the blob into the `.gen.c`
as a C string literal (`"\000\001…"`), ~2× the raw bytes in source text.

The `buf_next` wrapper is `MakeFunction` + `FunctionSetBody(Quote("return
buf_run(...);"))`; `buf_run` reaches the template because `buf_rt.h` comes in
via `#include @shared`, and the four freshly-created globals reach it because
each is `PublishNodeAt`'d right after creation (the auto-synthesised `extern`
alone is not visible to a same-parse-point `Quote()`).

Every table element type is wrapped in `MakeConst(...)`, so the tables emit
`static const` — matching `digits_tables.c`.

**The parser tables (`buf_emit_parser_tables`)** use the identical mechanism,
one `MakeConst(GetType("int"))` blob per table. `action`/`goto_tab` are
already contiguous live prefixes in `BufGrammar` (stride `g->ntok` /
`g->nnonterm`, never the arena max), so each is one `memcpy`.
`prod_lhs`/`prod_len` are not `BufGrammar` fields — the emitter bakes them
into file-scope scratch arrays from `buf_lalr_plhs`/`buf_lalr_plen` over
`0..rx->prod_count-1` (the synthetic augmenting production is never emitted).
It runs *after* the DFA emitter so the `buf_parse_tree` `Quote()` template can
name `buf_dfa_class` … `buf_rule_token`, which that pass published.

**Class table element type is `BufClass`** — `typedef unsigned char
BufClass;` in `buf_rt.h`. The emitter names it with `GetType("BufClass")`;
that resolves because `buf_rt.h` arrives via `#include @shared` and cccc's
generated forward-declaration block sits *below* that include, so the
typedef is in scope as an emitted global's element type. A bare
`GetType("unsigned char")` returns `NULL` — the comptime type resolver has
no spelling for a multi-word base type — so the table type has to go through
the typedef name. The emitted `buf_dfa_class` is then a true `unsigned char`
array matching `buf_run`'s `const unsigned char *cls` parameter with no cast
in the wrapper.

## The driver: `buf_run`

`buf_run(lx, cls, next, accept, rule_token, nstates, nclass, start)` is the
generic longest-match loop, hand-written in `buf_rt.c`, never emitted:

- from `start`, step `state = next[state * nclass + cls[byte]]`;
- remember the last accepting `(rule, pos)`;
- stop at a dead state (`-1`), roll forward to the last accept (updating
  `line:col` as it goes);
- if that rule maps to `-1` in `rule_token` it was `%skip` — consume and rescan;
- no rule matching at the current byte yields a `TOK_ERROR` token over that one
  byte and advances one byte, so the caller can report and resync;
- end of input yields `TOK_EOF` forever.

`buf_dfa_class` must be **total** over all 256 byte values, every entry in
`0..nclass-1` — the driver indexes `next[... + cls[byte]]` with no guard.

`buf_run` carries an unreachable tail `return` after its `for (;;)`: every
loop exit is already a `return`, but cccc's `-c=native` flow analysis does
not prove that and rejects a non-void aggregate function that can fall off
the end. Plain `cc` accepts either form.

`BufToken` and `BufLexer` carry explicit tags (`typedef struct BufToken {…}
BufToken;`) — plain good C, kept regardless of what cccc's own lowering does
with an anonymous typedef.

## Comptime VM gotchas

- `buf_nfa.h`'s recursive fragment builder returns its `{entry, exit}` pair
  through `int *` out-params rather than a by-value struct — the cases
  already name the endpoints as plain locals, so there is no wrapper type to
  define. A by-value struct return from a recursive comptime function is fine
  if you want one.
- A source `#define` is **not** forwarded into a comptime body — only `-D` on
  the cccc command line is. `BUF_SPEC` and `BUF_STOP_AFTER` are both `-D`s
  (`bin/buffalo` passes them); the `#ifndef` fallbacks in `src/buf_comptime.c`
  only satisfy the plain-`cc` host preprocessor.
- A plain `static` function in an `@comptime`-routed header **can** call the
  reflection builtins (`GlobalVar`, `Quote`, …) — `buf_emit.h` does. It does
  not need the `[[cccc::comptime]]` attribute; adding it actually made the
  function invisible to `src/buf_comptime.c`'s entry point.
- `GetType` in the comptime VM resolves single-keyword base types (`"char"`,
  `"short"`, `"int"`) but not a multi-word spelling like `"unsigned char"`
  (nor `"uchar"` / `"u8"`) — it returns `NULL`. Reach an unsigned or
  fixed-width element type through a `typedef` in an `#include @shared`
  header and name *that* (`GetType("BufClass")`); the class table does this.
- The comptime VM runs interpreted, ~1000–1300× slower than the same code
  under a plain `cc` (`buf_dfa_build` for `big.bflo`: 1.8 ms native vs. ~2.25 s
  comptime). Keep the hot construction loops tight — an O(nstates) scan or a
  per-call array clear that is free natively becomes seconds here; see
  [performance.md](performance.md).

## `Quote()` gotchas (carried from ccccl)

- Every statement hole in a `Quote()` template needs an explicit trailing `;` in
  the template text: `Quote("{ $1; }", s)`, not `"{ $1 }"` — a `$N` splice
  always parses in expression position even when the node behind it is a
  complete statement.
- An eager `Quote()` template is validated (including `break`/`continue` and
  variable-scope checks) at the point it is parsed, not after it is spliced
  into an outer template, so a loop body composed from a separate `Quote()`
  call can't `break`/`continue` out of the loop it will only be inside of
  once spliced. `QuoteLazy()` defers parsing until the splice and lifts that
  restriction. buffalo does not need it: the emitter deliberately produces
  only *data* (the `static const` tables) plus a one-line wrapper, and the
  longest-match loop stays hand-written in `buf_rt.c` — a single shared,
  plain-`cc`, zero-cccc runtime across every generated lexer, which keeps the
  `.gen.c` small and the generated/native parity check simple.
- Externs referenced from a `Quote()` template must come in via `#include
  @shared`, not `@comptime` (a plain-include extern is rejected by `Quote`'s
  identifier resolver). A `typedef` from that `@shared` header **can** be an
  emitted global's element type: cccc's generated forward-declaration block
  sits below the `@shared` include in the `.gen.c`, so the name is in scope
  where the tables are declared. `buf_dfa_class`'s `BufClass` element type
  relies on this.
- A global the same macro just created with `GlobalVar` is not visible to a
  `Quote()` template at the same parse point via its auto-synthesised
  `extern` — `PublishNodeAt(var, SyntheticToken("name"))` it first.
- State held across emitter calls: `GlobalVar` + `GlobalVarSetStatic` (the
  gensym pattern).
- `WithFn` / `WithBlock` etc. are single-iteration `for` loops — exit with
  `continue`, not `break`, or the context is left unrestored.

## Token-kind visibility

Token kinds (`TOK_INT`, …) are `enum` constants — not linkable symbols, so they
must be textually present in each translation unit that uses them. The caller's
`*_main.c` cannot `#include` the generated `.c`, and in `-c=native` mode there is
no generated file at all. Resolution: the token header is **hand-written and
checked in** (`examples/calc_tokens.h`), and the comptime pass validates it
against the `.bflo` spec, erroring on any drift. `%tokens` in the `.bflo` is the
authoritative list. `TOK_EOF = 0` and `TOK_ERROR = 1` are reserved (see
`BUF_TOK_*` in `buf_rt.h`).

The header path is not a directive in the spec: it is derived from the spec
path by replacing a trailing `.bflo` with `_tokens.h` (`calc.bflo` →
`calc_tokens.h`), overridable with `buffalo lex --tokens PATH` (which the
wrapper forwards as `-D BUF_TOKENS_H`). `src/buf_comptime.c` does the
derivation when `BUF_TOKENS_H` is undefined.

## The `.bflo` grammar section

The grammar lives in the **same `.bflo` file** as the lexer spec, opened by a
`%grammar` directive that runs to end of file (the ANTLR model) — not a
separate `.y` file paired with the `.bflo` (the yacc model). `lex` and
`parse` are subcommands of one tool over one file format, so there is no
reason to split the spec across two files; a combined grammar also lets
productions reference the same `%tokens` vocabulary without an import
mechanism.

Terminal-vs-nonterminal is resolved implicitly against `%tokens` (no
`%nonterm` declaration list to keep in sync), and `%start` is required rather
than defaulting to the first production, so reordering productions cannot
silently change the recognised language. See
[bflo-format.md](bflo-format.md#grammar-section) for the full syntax.

`buf_rx.h` reads and validates the grammar section; `buf_grammar.h` (below)
builds LALR(1) parser tables from it; `buf_parse` (below) drives those tables
at runtime. `buffalo parse` wires all three into the comptime pipeline and
`examples/expr.bflo` is the worked lexer + grammar, with generated/native
parity checked the same way as the lexer examples.

## LALR(1) table construction

`buf_grammar.{h,c}` turns a validated `%grammar` section into LALR(1)
`action`/`goto` tables — the parser-side counterpart to `buf_nfa`/`buf_dfa`
on the lexer side, and built the same way: `.h`/`.c` pair, fixed arenas, no
malloc, dual-compiled (host `cc` for `tests/t_grammar.c`; `#include @comptime`
under `buffalo parse` / `-D BUF_EMIT_PARSER` — see [Comptime
modules](#comptime-modules) above).

**The augmented grammar.** A synthetic production `S' -> %start` drives the
closure over the whole automaton and the accept action. It is never written
into `BufRx` (shared, read-only input) — every production lookup goes
through an accessor (`buf_lalr_plhs`/`buf_lalr_plen`/`buf_lalr_psym`/...)
that special-cases the synthetic index `rx->prod_count`.

**Construction method: LR(0) automaton + lookahead propagation to a
fixpoint** — the classic "spontaneous generation + propagation" LALR method
(Aho/Sethi/Ullman/Lam, Algorithm 4.62), not canonical LR(1) item sets merged
by core afterward. Canonical LR(1) is typically 3-6x more states on a
many-tier expression grammar (each precedence tier is its own nonterminal,
and LR(1) sees every one with each distinguishing lookahead as a separate
state before merging) — not worth it given the DFA phase already earned an
optimisation pass under the comptime VM's ~1000x slowdown. Full
DeRemer-Pennello lookahead computation (reads/includes relations, SCC via
Tarjan) is the asymptotically better method but is famously easy to get
subtly wrong, and buffalo's grammars are small enough that it buys nothing.

The LR(0) automaton itself mirrors `buf_dfa.h` almost exactly: item sets are
sorted runs of compact `(prod, dot)` item ids in one shared pool
(`set_off`/`set_len` index into it), an FNV-1a hash over the sorted run
resolves an existing state instead of an O(nstates) scan, and state ids are
handed out in worklist discovery order only — determinism, for the same
reason `buf_dfa.h` needs it.

**Action/goto encoding.** `action[state*ntok+tok]` is a packed `int`: `0`
error, positive `shift to (value-1)`, `BUF_LALR_ACT_ACCEPT` a dedicated
sentinel, any other negative `reduce production -(value)-1`. `tok` is the
*runtime* `TOK_*` value (matching `buf_dfa.h`'s `rule_token[]` convention),
so `buf_parse` indexes the table directly off what `buf_run` hands it — no
remap layer. `goto_tab[state*nnonterm+nt]` is a plain state id, or `-1`.

**Conflicts are always a hard error** — shift/reduce or reduce/reduce,
first-wins, naming the state, the conflicting lookahead token, and the
reducing production's `line:col`. There is no `%left`/`%right` to fall back
on (see [The `.bflo` grammar section](#the-bflo-grammar-section)), so a
conflict always means the grammar is genuinely ambiguous at that point.
Because conflicts are rejected at table-construction time, `buf_parse`
(below) never needs conflict-resolution logic — every populated `action[]`
cell it reads is unambiguous by construction.

## The parser driver: `buf_parse`

`buf_parse(ps, lx, ...)` is the generic LALR(1) shift/reduce driver, the
parser-side counterpart to `buf_run`: hand-written in `buf_rt.c`, never
emitted, driving `buf_run` itself for each lookahead token. The generated
`.parse.gen.c` calls it through the one-line `buf_parse_tree(ps, lx)` wrapper
`buf_emit.h` emits — the parser-side analogue of `buf_next`, with this spec's
DFA and LALR tables and scalars baked into the call.

- Parallel `state_stack`/`node_stack` arrays (caller-provided, like
  `BufLexer`'s stack-allocated storage — no allocation inside `buf_rt.c`);
  `BufParser` also takes a caller-sized `BufCstNode` pool and a `child[]`
  pool for the tree it builds.
- Decode `action[state * ntok + lookahead.kind]` with the `BUF_PARSE_ACT_*`
  macros: `0` is error, `BUF_PARSE_ACT_ACCEPT` accepts, positive shifts,
  any other negative reduces production `-(value)-1`.
- **Shift** allocates a leaf `BufCstNode` (`is_terminal = 1`, carrying the
  `BufToken`), pushes the new state, fetches the next lookahead.
- **Reduce** pops `prod_len[p]` stack entries into the child pool, allocates
  an interior node (`is_terminal = 0`, `index = prod_lhs[p]`, `nchild =
  prod_len[p]`), then pushes `goto_tab[state * nnonterm + prod_lhs[p]]`. An
  interior node's `line:col` is its leftmost child's; on an **epsilon
  reduce** (`prod_len[p] == 0`, no children to inherit from) it's the
  current lookahead's position instead.
- **Accept** returns the node under the stack top — the CST root.
- On failure (`status != BUF_PARSE_OK`), `error_tok`/`error_state` say what
  the offending lookahead and LR state were; a `BUF_TOK_ERROR` lookahead
  from `buf_run` is reported the same way as any other token with no
  `action[]` entry.

**Why `buf_parse` cannot take a `BufRx *`.** `BufRx` is a comptime-only,
megabyte-scale fixed-arena struct, not a runtime type — `buf_rt.c` never
links against it. So instead of calling `buf_lalr_plhs`/`buf_lalr_plen`
directly, `buf_parse` takes two flat arrays baked from those accessors ahead
of time: `prod_lhs[nprods]` and `prod_len[nprods]`, the runtime-side
counterpart to `buf_dfa.h`'s `rule_token[]`. In the generated path
`buf_emit_parser_tables` bakes them; `tests/t_parse.c` does the same by hand.
`buf_lalr_psym` isn't needed by the driver at all — a reduce's popped stack
entries already *are* its children.

**`action[]` is `int`, not `short`.** Unlike `buf_run`'s DFA tables,
`BufGrammar.action` must be `int` because `BUF_LALR_ACT_ACCEPT ==
-1000000` doesn't fit a `short`.

**`BUF_PARSE_ACT_*` is an independent copy of `buf_grammar.h`'s
`BUF_LALR_ACT_*` encoding**, not a `#include` of the comptime header —
`buf_rt.h` cannot depend on `buf_grammar.h` any more than it can depend on
`BufRx`. Cross-checked by a static assert in `tests/t_parse.c`, the same
pattern as the `BUF_TOK_FIRST_USER` / `BUF_LALR_FIRST_USER_TOK` /
`BUF_DFA_FIRST_USER_TOK` triple.

**Errors are struct fields, not a formatted string.** `buf_rt.c` has no
`<stdio.h>`/`snprintf` — unlike the comptime headers' sticky `error[]`
string convention (`buf_rx.h`/`buf_dfa.h`/`buf_grammar.h`), `BufParser`
reports failure via `status` (a `BUF_PARSE_ERR_*` enum) plus `error_tok`/
`error_state`, matching how `buf_run` itself reports a lex error as data
(`BUF_TOK_ERROR`) rather than an out-of-band message.

`buf_parse` carries the same unreachable tail `return` after its `for
(;;)` that `buf_run` does, for the same `-c=native` flow-analysis reason.

**A known LALR limitation.** Because LALR merges LR(0)-core-identical states
before computing lookaheads, it can report a reduce/reduce conflict that a
canonical LR(1) automaton over the same grammar would not have. That is
LALR doing what LALR does, not a buffalo bug — splitting the offending
nonterminal into two (so the states no longer share an LR(0) core) is the
usual grammar-level fix.

**Bug fixed in this round: epsilon productions never reduced.**
`buf_lalr_closure1`'s lookahead-propagation pass only wrote `la_pool` for
closure items with a symbol after the dot (`d < L`), so it could forward a
lookahead to a *successor* state's kernel item. A completed item produced
purely by closure — e.g. the reduce item for an epsilon production such as
`args : | INT args ;` — has no successor to shift/goto into; its lookahead
belongs on its own `la_pool` slot in the *current* state. The old code hit
`if (d >= L) continue;` and silently dropped it, so `buf_lalr_fill_reduces`
never saw a lookahead bit and never installed the reduce action — nullable
productions built a table that could never actually reduce them. Fixed by
handling the `d >= L` case explicitly: find the item's own slot in the
current state via `buf_lalr_find_item(g, state, t)` and write the
spontaneous bits / propagation edge there instead of skipping it. Caught by
`buf_parse`'s epsilon-production test (`tests/t_parse.c`), which is the
first thing to actually *run* an LALR table end-to-end against a nullable
grammar — `tests/t_grammar.c`'s own epsilon test only checked that a
non-trivial automaton got built, not that it recognised anything.

## Known limitations

- **Raw init-data bakes in host width and endianness.** The table blob is a
  `memcpy` of the comptime host's integers. Fine while comptime host and target
  are the same machine (the cccc norm); a cross-arch `.gen.c` would need
  byte-swapping. Revisit if it ever bites.
- **`buf_next` / `buf_parse_tree` are fixed global names**, so one generated
  lexer (and one parser) per program. Supporting two would need a
  name-parameterised emitter — not currently planned.
- **Bytes only, no Unicode.** Input is bytes; UTF-8 is the caller's problem
  (multibyte sequences pass through inside identifiers/strings as raw bytes).
- **DFA minimisation is opt-in and rarely worth it.** Alphabet equivalence
  classes already do the load-bearing table shrink; the Moore minimisation
  pass (`-D BUF_MINIMIZE`) trims only a few percent more and costs comptime it
  does not earn back. Left in for callers that want the smallest possible
  table.
