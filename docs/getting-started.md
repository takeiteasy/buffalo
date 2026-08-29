# Getting started

## Prerequisites

- A C compiler as `cc` (clang or gcc).
- For `buffalo lex` and `make spec`: the `cccc` binary on `PATH`, or `$CCCC`
  pointing at it. The M0 demo and the host unit tests need only `cc`.

## M0: the hand-written demo

M0 ships no generator. It ships the runtime and a hand-written table file
(`examples/digits_tables.c`) of the exact shape the generator will emit, so the
runtime contract is pinned down and testable.

```sh
make          # cc -O2 -Wall, no cccc -> build/digits
make check    # host unit tests + the digits golden diff; runs `make spec` too when cccc is on PATH
```

`build/digits` reads stdin and prints one line per token — `NAME "lexeme"
line:col`:

```
$ printf '12 34\n5 x 6\n' | ./build/digits
INT   "12" 1:1
INT   "34" 1:4
INT   "5" 2:1
ERROR "x" 2:3
INT   "6" 2:5
EOF   "" 3:1
```

This exercises the whole runtime: `[0-9]+` longest-match, `%skip` for
whitespace, `line:col` tracking across a newline, `TOK_ERROR` + single-byte
resync on a stray byte, and `TOK_EOF` at end.

## M1–M3: the comptime pipeline

`bin/buffalo lex` reads a `.l` spec at compile time — inside `cccc`'s comptime
pass — into a regex AST, validates the checked-in token header against the
spec's `%tokens` list, builds the ε-NFA and DFA, and emits the four DFA tables
plus a `buf_next` wrapper into the `.gen.c`:

```sh
$ bin/buffalo lex examples/calc.l
Generated C written to examples/calc.l.gen.c
```

The generated file is minimal — four `static` table arrays and the wrapper.
M4 turns it into one that builds cleanly with a stock `cc` against
`runtime/buf_rt.c` and a `calc_main.c`. Running the whole thing at compile
time costs ~0.38 s for a ~45-rule lexer, measured in
[performance.md](performance.md); `-D BUF_STOP_AFTER=n` stops the pipeline
early (`0` none … `5` emit) for per-phase timing.

What you also get is a spec reader and DFA builder that fail loudly and
precisely. A malformed regex is reported at its `line:col` in the `.l` file:

```sh
$ bin/buffalo lex broken.l
buffalo: broken.l:3:8: unterminated character class
```

and so is a token header that has drifted from the spec:

```sh
$ bin/buffalo lex examples/calc.l
buffalo: examples/calc_tokens.h: token header has 'TOK_FLOAT' where 'TOK_INT' is expected
```

### The token header

`buffalo lex SPEC.l` validates a checked-in `<name>_tokens.h` derived from the
spec path: the trailing `.l` is replaced with `_tokens.h`, so
`examples/calc.l` pairs with `examples/calc_tokens.h`. Pass `--tokens PATH` to
point somewhere else. The header must open with

```c
enum { TOK_EOF = 0, TOK_ERROR = 1, TOK_<N0>, TOK_<N1>, ... };
```

where `N0, N1, ...` is the spec's `%tokens` list in order. Any missing kind,
extra kind, reordering, or explicit value on a non-reserved kind is an error.

## `bin/buffalo`

`bin/buffalo` is a thin shell wrapper that assembles the `cccc` invocation:

```sh
$ bin/buffalo help
$ bin/buffalo version
$ bin/buffalo lex examples/calc.l [-o OUT.gen.c] [--tokens TOK.h]
```

## What each milestone unlocks

See [ROADMAP.md](../ROADMAP.md). In short:

- **M1** — `bin/buffalo lex` works: `.l` → regex AST, token-header validation.
- **M2** — the comptime pass also builds the ε-NFA and the DFA (alphabet
  equivalence classes) in memory; `make test` adds `t_nfa` and `t_dfa`, the
  latter driving the real `buf_run` over the freshly built tables.
- **M3** — a minimal emit step (`buf_emit.h`) writes the four tables + the
  `buf_next` wrapper into the `.gen.c`; `make bench` measures the comptime
  cost of the whole pipeline (spike passed — see performance.md).
- **M4** — `bin/buffalo lex examples/calc.l -o build/calc.l.gen.c` produces a
  table file that builds cleanly with `cc` against `runtime/buf_rt.c` and a
  `calc_main.c`.
- **M5** — `make check` (generated path) and `make native` (one-shot cccc path)
  both pass with byte-identical output for `calc`, `json`, `clike`.

Once M4 exists the two invocations are:

```sh
# inspectable path
cccc -c=generated src/buf_comptime.c -Iinclude/buffalo -Iruntime \
    -D BUF_SPEC='"examples/calc.l"' -o build/calc.l.gen.c
cc -O2 -Iruntime -Iexamples -o build/calc \
    examples/calc_main.c build/calc.l.gen.c runtime/buf_rt.c

# one-shot path
cccc -c=native src/buf_comptime.c examples/calc_main.c runtime/buf_rt.c \
    -Iinclude/buffalo -Iruntime -D BUF_SPEC='"examples/calc.l"' -o build/calc_native
```

`-D` (not a source `#define`) carries the spec path — comptime bodies do not see
ordinary source `#define`s.
