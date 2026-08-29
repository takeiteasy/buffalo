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
string into a comptime diagnostic pointed at the `.l` file.

`#include @shared "buf_rt.h"` (not `@comptime`) is deliberate: a `Quote()`
template that names an extern — `buf_run`, the table symbols — is rejected by
cccc's identifier resolver unless that extern arrived via `@shared`.

### Comptime modules (M1 onward)

- `buf_rx.h` *(M1)* — spec-file + regex reader: `.l` text → regex AST,
  `line:col` per node. Character classes are desugared into a 256-bit byte set
  at parse time (so M2's alphabet partitioning is a set-grouping pass). A rule
  whose regex is nullable is rejected here with a `line:col` error.
- `buf_tokcheck.h` *(M1)* — validate a checked-in `<name>_tokens.h` against
  `%tokens`.
- `buf_nfa.h` *(M2)* — Thompson construction: regex AST → ε-NFA. One fragment
  per AST node; each NFA state has at most one labelled (byte-set) edge and up
  to two ε edges, which covers every fragment. Each rule's fragment ends in an
  accept state tagged with the rule index; all rule fragments hang off state 0
  down a two-ε spine (a rule count above two rules out one fan-out state). A
  nullable *sub-expression* — `(a?)+`, `(a*)*` — survives `buf_rx.h` (only a
  nullable *rule* is rejected) and puts an ε cycle in the NFA, so every ε walk
  is worklist + visited-set, never recursive. Fragment entry/exit come back
  through out-params, not a returned struct: cccc's comptime VM mishandles a
  by-value struct return from a recursive comptime function.
- `buf_dfa.h` *(M2)* — subset construction: ε-NFA → DFA over alphabet
  equivalence classes (not raw bytes). The alphabet partition starts with all
  256 bytes in one class and splits on each CLASS-leaf byte set, then
  **renumbers classes by first appearance over bytes 0..255** so the numbering
  is deterministic and hash/iteration-order independent. Each DFA state is a
  sorted run of NFA-state indices in one **shared pool** (`set_off`/`set_len`
  index into it), capped independently of the DFA state count so M3 can tune
  the two arena knobs separately; runs are collected by sweeping NFA ids
  ascending, so they double as the identity key. State ids are handed out in
  worklist **discovery order** only. Each DFA state carries the winning rule
  index (lowest rule index among the NFA accept states it contains — earlier
  rule wins ties), and `rule_token[r]` maps a rule to its `TOK_*` **value**
  (`tok_index + BUF_TOK_FIRST_USER`), or `-1` for `%skip`. Construction ends by
  asserting `accept[start] < 0` — the contract `buf_run` relies on with no
  runtime guard — and reports a violation at the offending rule's `line:col`.
- `buf_emit.h` — DFA → four file-scope tables (raw `GlobalVarSetInitData`
  blobs) + the `buf_next` wrapper fn. Unlike the other comptime headers this
  one uses cccc's reflection builtins and is never dual-compiled by a plain
  `cc`. The M3 spike (see [performance.md](performance.md)) landed the minimal
  version; M4 extends it into the build-with-stock-`cc` path.

### Runtime module

`runtime/buf_rt.{h,c}` holds everything that is not generated: the input buffer,
`line:col` tracking, and the longest-match driver loop `buf_run` itself. Keeping
the loop here rather than emitting it means the emitter produces only *data*
(`static const` arrays) plus a one-line wrapper — which sidesteps the `Quote()`
sharp edges around `while` / `break` / `continue` (see below).

## Generated table file — shape

`cccc -c=generated src/buf_comptime.c -D BUF_SPEC='"calc.l"'` produces a
`.gen.c` containing four file-scope `static` tables and one wrapper:

```c
static char  buf_dfa_class[256];              /* byte -> class 0..NCLASS-1 */
static short buf_dfa_next[NSTATES * NCLASS];  /* flat; -1 = dead */
static short buf_dfa_accept[NSTATES];         /* rule index, -1 = non-accepting */
static short buf_rule_token[NRULES];          /* rule -> TOK_* kind, -1 for %skip */

BufToken buf_next(BufLexer *lx) {
    return buf_run(lx, (const unsigned char *)buf_dfa_class, buf_dfa_next,
                   buf_dfa_accept, buf_rule_token, NSTATES, NCLASS, 0);
}
```

`examples/digits_tables.c` is a hand-written file of the same shape — the M0
stand-in, so the runtime and harness could be exercised before the emitter
existed. (`digits_tables.c` is `const`-qualified and `unsigned char`; M4
decides whether the emitter matches that — see the class-table note below.)

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

**Class table is `char`, not `unsigned char`.** cccc's comptime `GetType`
resolves only `"char"`, `"short"`, `"int"` — not `"unsigned char"`. The class
table holds values `0..nclass-1` (≤ ~40), so a plain `char` blob is
bit-identical and the wrapper casts it back to `const unsigned char *` for
`buf_run`.

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
  under a plain `cc` (M3: `buf_dfa_build` for `big.l` 1.8 ms native vs. ~2.25 s
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
  @shared`, not `@comptime`.
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
against the `.l` spec, erroring on any drift. `%tokens` in the `.l` is the
authoritative list. `TOK_EOF = 0` and `TOK_ERROR = 1` are reserved (see
`BUF_TOK_*` in `buf_rt.h`).

The header path is not a directive in the spec: it is derived from the spec
path by replacing a trailing `.l` with `_tokens.h` (`calc.l` →
`calc_tokens.h`), overridable with `buffalo lex --tokens PATH` (which the
wrapper forwards as `-D BUF_TOKENS_H`). `src/buf_comptime.c` does the
derivation when `BUF_TOKENS_H` is undefined.

## Known limitations

- **Raw init-data bakes in host width and endianness.** The table blob is a
  `memcpy` of the comptime host's integers. Fine while comptime host and target
  are the same machine (the cccc norm); a cross-arch `.gen.c` would need
  byte-swapping. Revisit if it ever bites.
- **`buf_next` is a fixed global name**, so one generated lexer per program. A
  program that needs two lexers needs a name-parameterised emitter — not planned
  for Phase 1.
- **Bytes only, no Unicode.** Input is bytes; UTF-8 is the caller's problem
  (multibyte sequences pass through inside identifiers/strings as raw bytes).
- **No DFA minimisation in Phase 1** (alphabet equivalence classes are in M2;
  Hopcroft minimisation is Phase 1.5).
