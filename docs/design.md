# Design

## Two universes

buffalo's code splits into two disjoint bodies that share no source, only a
runtime data-layout contract. Copied wholesale from cccc's `ccccl` example.

| Universe | Files | Compiled by | Runs |
|---|---|---|---|
| **Comptime** | `src/buf_comptime.c` (the only file passed to cccc) + `include/buffalo/*.h` pulled in via `#include @comptime` | cccc's comptime VM | at compile time; never reaches the generated program |
| **Runtime** | `runtime/buf_rt.{h,c}` | system `cc` | in the final program; zero cccc dependency |

The comptime headers are ordinary, dependency-free, arena-based C: fixed-size
arrays, no `malloc`, `snprintf` into an error buffer, first-error-wins sticky
flag (they cannot call `MacroErrorAt` directly because they must also compile
under a plain `cc` for host unit tests). `src/buf_comptime.c` is the boundary: it
is the only place that calls `MacroErrorAt`, converting a header's sticky error
string into a comptime diagnostic pointed at the `.bflo` file.

`#include @shared "buf_rt.h"` (not `@comptime`) is deliberate: a `Quote()`
template that names an extern — `buf_run`, the table symbols — is rejected by
cccc's identifier resolver unless that extern arrived via `@shared`.

### Comptime modules

- `buf_rx.h` — spec-file + regex reader: `.bflo` text → regex AST,
  `line:col` per node. Character classes are desugared into a 256-bit byte set
  at parse time (so alphabet partitioning is a set-grouping pass). A rule
  whose regex is nullable is rejected here with a `line:col` error. It also
  reads the optional `%grammar` section (see below) — productions, resolved
  terminal/nonterminal symbols, `%start` — with the same `line:col`
  diagnostics; `buf_grammar.h` (below) builds LALR(1) parser tables from it.
- `buf_tokcheck.h` — validate a checked-in `<name>_tokens.h` against
  `%tokens`.
- `buf_nfa.h` — Thompson construction: regex AST → ε-NFA. One fragment
  per AST node; each NFA state has at most one labelled (byte-set) edge and up
  to two ε edges, which covers every fragment. Each rule's fragment ends in an
  accept state tagged with the rule index; all rule fragments hang off state 0
  down a two-ε spine (a rule count above two rules out one fan-out state). A
  nullable *sub-expression* — `(a?)+`, `(a*)*` — survives `buf_rx.h` (only a
  nullable *rule* is rejected) and puts an ε cycle in the NFA, so every ε walk
  is worklist + visited-set, never recursive. Fragment entry/exit come back
  through out-params, not a returned struct: cccc's comptime VM mishandles a
  by-value struct return from a recursive comptime function.
- `buf_dfa.h` — subset construction: ε-NFA → DFA over alphabet
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
- `buf_grammar.h` — LALR(1) parser tables over the `%grammar` section's
  productions. See [The `.bflo` grammar section](#the-bflo-grammar-section)
  and [LALR(1) table construction](#lalr1-table-construction) below.
  Host-test-only for now (`tests/t_grammar.c`) — not yet wired into
  `src/buf_comptime.c`'s comptime pipeline (follow-on work, see the tracker).
- `buf_emit.h` — DFA → four file-scope `static const` tables (raw
  `GlobalVarSetInitData` blobs) + the `buf_next` wrapper fn. Unlike the other
  comptime headers this one uses cccc's reflection builtins and is never
  dual-compiled by a plain `cc`. Its output builds with a stock `cc` against
  `runtime/buf_rt.c` and an example `_main.c`.

### Runtime module

`runtime/buf_rt.{h,c}` holds everything that is not generated: the input buffer,
`line:col` tracking, and the longest-match driver loop `buf_run` itself. Keeping
the loop here rather than emitting it means the emitter produces only *data*
(`static const` arrays) plus a one-line wrapper — which sidesteps the `Quote()`
sharp edges around `while` / `break` / `continue` (see below).

## Generated table file — shape

`cccc -c=generated src/buf_comptime.c -D BUF_SPEC='"calc.bflo"'` produces a
`.gen.c` containing four file-scope `static` tables and one wrapper:

```c
static const char  buf_dfa_class[256];              /* byte -> class 0..NCLASS-1 */
static const short buf_dfa_next[NSTATES * NCLASS];   /* flat; -1 = dead */
static const short buf_dfa_accept[NSTATES];          /* rule index, -1 = non-accepting */
static const short buf_rule_token[NRULES];           /* rule -> TOK_* kind, -1 for %skip */

BufToken buf_next(BufLexer *lx) {
    return buf_run(lx, (const unsigned char *)buf_dfa_class, buf_dfa_next,
                   buf_dfa_accept, buf_rule_token, NSTATES, NCLASS, 0);
}
```

`examples/digits_tables.c` is a hand-written file of the same shape. It is
the reference the emitter is diffed against (`make native`'s three-way
parity check) and the one build path that needs no cccc at all. The emitter
matches its `static const`; it does **not** match its `unsigned char` on the
class table — see the class-table note below.

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

**Class table is `char`, not `unsigned char`.** Two independent reasons:
cccc's comptime `GetType` resolves only `"char"`, `"short"`, `"int"` — not
`"unsigned char"` — and cccc emits its forward-declaration block *before* the
`@shared` `#include`, so a `typedef unsigned char BufClass;` in `buf_rt.h`
would not be in scope as an emitted global's element type either. (That
second point also forecloses `typedef`-based name-parameterisation of the
table symbols.) The class table holds values `0..nclass-1` (≤ ~40), so a
plain `char` blob is bit-identical and the wrapper casts it back to `const
unsigned char *` for `buf_run`.

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

- Returning a small struct by value from a comptime function triggered
  `error: return buffer pool was not rehydrated` (V1) — observed with
  `buf_nfa.h`'s recursive fragment builder; not further isolated (recursion
  may or may not be the trigger). Switching `{entry, exit}` to `int *`
  out-params cleared it. Returning scalars is fine (`buf_rx.h` does it
  throughout).
- A source `#define` is **not** forwarded into a comptime body — only `-D` on
  the cccc command line is. `BUF_SPEC` and `BUF_STOP_AFTER` are both `-D`s
  (`bin/buffalo` passes them); the `#ifndef` fallbacks in `src/buf_comptime.c`
  only satisfy the plain-`cc` host preprocessor.
- A plain `static` function in an `@comptime`-routed header **can** call the
  reflection builtins (`GlobalVar`, `Quote`, …) — `buf_emit.h` does. It does
  not need the `[[cccc::comptime]]` attribute; adding it actually made the
  function invisible to `src/buf_comptime.c`'s entry point.
- `GetType` in the comptime VM resolves `"char"`, `"short"`, `"int"` but not
  `"unsigned char"` (nor `"uchar"` / `"u8"`). Emit the widest signed type that
  fits and cast at the use site.
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
- `continue;` / `break;` as bare `Quote()` text is rejected ("stray continue") at
  template-parse time, before the node is spliced anywhere. **This is why the
  driver loop stays in `buf_rt.c` and the emitter produces only data + a
  one-line wrapper.**
- Externs referenced from a `Quote()` template must come in via `#include
  @shared`, not `@comptime`. cccc emits its forward-declaration block for the
  generated symbols *ahead* of that `@shared` include in the `.gen.c`, so a
  `typedef` from the shared header cannot be an emitted global's element type
  — only the base types `GetType` knows.
- cccc's `Quote()` lowering leaves a dead `BufToken __cccc_tmp0;` local in the
  generated wrapper (`-Wunused-variable`). It is cccc codegen, not buffalo's
  output; the `Makefile` scopes `-Wno-unused-variable` to the `.gen.c`
  translation unit (`GEN_CFLAGS`).
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
builds LALR(1) parser tables from it. A runtime parser driver and a worked
example remain follow-on work (see the tracker).

## LALR(1) table construction

`buf_grammar.h` turns a validated `%grammar` section into LALR(1) `action`/
`goto` tables — the parser-side counterpart to `buf_nfa.h`/`buf_dfa.h` on the
lexer side, and built the same way: header-only, fixed arenas, no malloc,
dual-compiled (host `cc` for `tests/t_grammar.c`; not yet wired into the
comptime pipeline — see [Comptime modules](#comptime-modules) above).

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
so a future driver indexes the table directly off what `buf_run` hands it.
`goto_tab[state*nnonterm+nt]` is a plain state id, or `-1`.

**Conflicts are always a hard error** — shift/reduce or reduce/reduce,
first-wins, naming the state, the conflicting lookahead token, and the
reducing production's `line:col`. There is no `%left`/`%right` to fall back
on (see [The `.bflo` grammar section](#the-bflo-grammar-section)), so a
conflict always means the grammar is genuinely ambiguous at that point.

**A known LALR limitation.** Because LALR merges LR(0)-core-identical states
before computing lookaheads, it can report a reduce/reduce conflict that a
canonical LR(1) automaton over the same grammar would not have. That is
LALR doing what LALR does, not a buffalo bug — splitting the offending
nonterminal into two (so the states no longer share an LR(0) core) is the
usual grammar-level fix.

## Known limitations

- **Raw init-data bakes in host width and endianness.** The table blob is a
  `memcpy` of the comptime host's integers. Fine while comptime host and target
  are the same machine (the cccc norm); a cross-arch `.gen.c` would need
  byte-swapping. Revisit if it ever bites.
- **`buf_next` is a fixed global name**, so one generated lexer per program. A
  program that needs two lexers needs a name-parameterised emitter — not
  currently planned.
- **Bytes only, no Unicode.** Input is bytes; UTF-8 is the caller's problem
  (multibyte sequences pass through inside identifiers/strings as raw bytes).
- **DFA minimisation is opt-in and rarely worth it.** Alphabet equivalence
  classes already do the load-bearing table shrink; the Moore minimisation
  pass (`-D BUF_MINIMIZE`) trims only a few percent more and costs comptime it
  does not earn back. Left in for callers that want the smallest possible
  table.
