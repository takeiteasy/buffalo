# Roadmap

buffalo is built in one phase of numbered milestones, then a stretch phase, then
a second phase for the parser generator. M0 (the scaffold: runtime surface,
`Makefile`, `bin/buffalo`, docs, and a hand-written demo lexer) is done — this
list starts at M1.

Two build paths must stay in lockstep from M4 on, mirroring cccc's own `ccccl`
example:

- `cccc -c=generated` lowers `src/buf_comptime.c` + a `.l` spec to an
  inspectable `.gen.c`, which then builds with a plain `cc` against
  `runtime/buf_rt.c`.
- `cccc -c=native` does the whole thing in one invocation, no intermediate file.

Both must produce byte-identical program output.

## Phase 1 — lexer generator

### M1 — spec + regex reader

- `include/buffalo/buf_rx.h` — spec-file reader and regex parser: `.l` text →
  regex AST, tracking `line:col` per node for diagnostics pointed back into the
  `.l` file.
- Regex v1 grammar: literals, `"..."` strings, `.`, `[...]` classes with ranges
  and negation, the `\n \t \r \f \v \0` and metacharacter escapes, `\d \D \w \W
  \s \S` shorthands (desugared to classes), concatenation, `|`, `* + ?`, `(...)`
  grouping. No `{n,m}`, `^ $`, `\b`, lookaround, non-greedy or backreferences.
- `include/buffalo/buf_tokcheck.h` — validate a checked-in `<name>_tokens.h`
  against the spec's `%tokens` list; error on drift (missing kind, wrong order,
  extra kind). `%tokens` is the authoritative order; every named rule's `NAME`
  must appear in it, and every `%tokens` entry must have a rule.
- `src/buf_comptime.c` — the only file passed to cccc; wires the headers in via
  `#include @comptime`, `#include @shared "buf_rt.h"`, reads `BUF_SPEC`.
- `bin/buffalo lex` starts working (it errors cleanly until this milestone).
- `tests/t_rx.c` — host unit tests, plain `cc`, no cccc.

### M2 — NFA + DFA + alphabet classes

- `include/buffalo/buf_nfa.h` — Thompson construction: regex AST → ε-NFA.
- `include/buffalo/buf_dfa.h` — subset construction: ε-NFA → DFA, built over
  **alphabet equivalence classes**, not raw bytes. Partition 0..255 by "which
  character-class leaves does this byte belong to"; a real lexer collapses to
  20–40 classes, shrinking the transition table 6–12×. Each DFA state carries
  the winning rule index (lowest rule index among the NFA accept states it
  contains — earlier rule wins ties).
- `tests/t_nfa.c`, `tests/t_dfa.c` — assert token streams against a reference
  matcher; `t_dfa.c` also asserts the class count for a `clike.l`-sized spec is
  far below 256.

### M3 — spike (go / no-go) — **done, GO**

Ran M1–M2 **plus a minimal emit step** (`include/buffalo/buf_emit.h`: four
`GlobalVar` tables + raw `GlobalVarSetInitData` blobs + the `Quote()`d
`buf_next` wrapper) inside the comptime VM for the ~40-rule `examples/clike.l`.
Full method and numbers in [docs/performance.md](../docs/performance.md);
`src/buf_comptime.c`'s `-D BUF_STOP_AFTER` ladder + `tests/bench.sh` reproduce
it (`make bench`).

Result (aarch64-darwin, cccc 0.1.0, `-O2`; measured on `clike.l` — 45 rules,
85 DFA states — and `examples/big.l`, a deliberately oversized 103-rule /
324-state fixture added for the spike):

- **added compile time for `clike.l`, emit included: ~0.38 s** over a ~0.31 s
  no-op comptime baseline. Gate was < 1 s → **GO**. `big.l`: ~2.25 s — a
  lexer that size wants Phase 1.5's DFA minimisation first, but M4's targets
  (`calc`, `clike`) are well inside the gate.
- **emission is free** at every size — raw init-data blobs + one wrapper fn,
  lost in the noise. The `GlobalVar` + `memcpy` bet paid off.
- the entire cost is the **DFA construction phase**, and it is cccc-VM
  interpretation overhead (~1000–1300× vs. native `cc`), superlinear in DFA
  state count. The first measurement was ~3× worse; **M3 reworked two spots in
  `buf_dfa.h`** — `buf_dfa_find` (linear scan → FNV-1a hash index) and
  `buf_dfa_closure` (per-call `mark[]` clear → generation counter; full
  `0..N_nfa` collect → insertion sort of the closure) — for ~6× on `big.l`.
  Determinism is unchanged and regression-tested on `big.l` in `t_dfa.c`.
- **arena peaks**: everything 2.5–140× inside its cap. `big.l` at 103 rules
  first drove `BUF_RX_MAX_RULES` 128 → 256 (now matches `BUF_RX_MAX_TOKENS`).
  Caps stay generous: `static` arrays, compile time tracks the live counts.

| Arena | Cap | `clike.l` | `big.l` |
|---|---|---|---|
| rules | 256 | 45 | 103 |
| regex AST nodes | 4096 | 162 | 739 |
| NFA states | 8192 | 306 | 1077 |
| DFA states | 4096 | 85 | 324 |
| equivalence classes | 256 (typically 20–40) | 43 | 76 |
| transition table | 262144 | 3655 | 24624 |
| state-set pool | 65536 | 477 | 2157 |

- **generated-file size**: `calc.l` 2.2 KB, `clike.l` 16.8 KB, `big.l` 100 KB.
  cccc serialises each table blob as a C string literal; the escaped text is
  ~2× the raw table bytes (~7.6 KB for `clike.l`), and the file scales with
  `nstates·nclass` (mostly the `next` table).

### M4 — the emitter

- `include/buffalo/buf_emit.h` — DFA → four file-scope tables via
  `GlobalVar(name, MakeArray(elem_ty, len))` + `GlobalVarSetInitData(var,
  raw_blob, byte_len)` + `GlobalVarSetStatic`, plus the `buf_next` wrapper as
  `MakeFunction` + `FunctionSetBody(Quote("return buf_run(...);"))`. The driver
  loop is **not** emitted — it stays in `runtime/buf_rt.c`, which sidesteps
  cccc's rejection of bare `Quote("continue;")` / `Quote("break;")`. M3 landed
  the minimal version of this (see above); M4 extends it (`const`
  qualification, the class table's `unsigned char` vs. the `char`-blob
  workaround, name-parameterisation if pursued) and adds an emitted-table
  correctness test — M3's blob was verified against `buf_run` by hand once,
  not in CI.
- `cccc -c=generated` yields a `.gen.c` that builds with plain `cc` against
  `runtime/buf_rt.c` + `examples/calc_main.c` + `examples/calc_tokens.h`.
- `Makefile` gains `generated` and `native` targets.

### M5 — example suite + parity

- Complete the `calc` example (`calc_main.c`, `calc.expected`) and add `json`
  and `clike` (`.l`, `_tokens.h`, `_main.c`, `.expected` each).
- `make check` (generated path) and `make native` (one-shot path) both pass and
  produce byte-identical output.

### M6 — docs pass

README, `docs/design.md`, `docs/lex-spec-format.md`, `docs/getting-started.md`
brought current: real invocations, the raw-init-data limitation, the decision
log.

## Phase 1.5 — stretch (not blocking)

- Switch-based (`re2c`-style) emission as an alternative to table mode; same spec
  input and generated API either way.
- **DFA minimisation (Hopcroft).** M3 showed comptime cost is superlinear in
  DFA state count (~2.25 s for `examples/big.l`'s 324 states); minimisation is
  the lever that attacks the count itself and lifts the ~150-state ceiling.
- `{n,m}` repetition counts.
- Line anchors `^ $`.
- `--emit-tokens` mode: write the `<name>_tokens.h` instead of validating a
  checked-in one.

## Phase 2 — parser generator

`.y` grammar files → parser tables, lowered the same way. Inherits Phase 1's
two-universe split, the `Makefile` / `check` / `native` harness, the spec
reader's `line:col` diagnostic style, and the `TOK_*` vocabulary + `BufToken` as
the terminal vocabulary.

**Open decision:** whether the grammar is a separate `.y` file (yacc model) or
merged with the `.l` spec into one combined grammar (ANTLR model). Sharper now
that lex and parse are subcommands of one tool — but still deferred to when
Phase 2 starts.
