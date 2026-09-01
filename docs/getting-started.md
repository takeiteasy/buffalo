# Getting started

## Prerequisites

- A C compiler as `cc` (clang or gcc).
- For `buffalo lex`, `make spec`, `make generated`, `make native`: the `cccc`
  binary on `PATH`, or `$CCCC` pointing at it. The default `make` build and
  the host unit tests need only `cc`.

## The no-cccc build

`make` builds `build/digits` from the runtime and a hand-written table file
(`examples/digits_tables.c`) with the system `cc` alone — no cccc. That file
is also the reference the emitter's output is diffed against (see
`make native` below).

```sh
make          # cc -O2 -Wall, no cccc -> build/digits
make check    # host unit tests + the digits golden diff; also spec/generated/native when cccc is on PATH
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

## The comptime pipeline

`bin/buffalo lex` reads a `.bflo` spec at compile time — inside `cccc`'s comptime
pass — into a regex AST, validates the checked-in token header against the
spec's `%tokens` list, builds the ε-NFA and DFA, and emits four
`static const` DFA tables plus a `buf_next` wrapper into the `.gen.c`:

```sh
$ bin/buffalo lex examples/calc.bflo
Generated C written to examples/calc.bflo.gen.c
```

The generated file builds with a stock `cc` against `runtime/buf_rt.c` and an
example `_main.c` — no cccc past this point. Running the pipeline at compile
time costs ~0.38 s for a ~45-rule lexer, measured in
[performance.md](performance.md); `-D BUF_STOP_AFTER=n` stops it early (`0`
none … `5` emit, `6` grammar, `7` parser emit) for per-phase timing.

### `buffalo parse`

`bin/buffalo parse` runs the same pipeline with `-D BUF_EMIT_PARSER`: after the
DFA tables it also builds LALR(1) parser tables from the spec's `%grammar`
section and emits four more `static const int` tables plus a `buf_parse_tree`
wrapper. The spec must carry a `%grammar` section — `buffalo parse` on one
without it is an error.

```sh
$ bin/buffalo parse examples/expr.bflo
Generated C written to examples/expr.bflo.parse.gen.c
```

The generated file links against `runtime/buf_rt.c` exactly as the `lex`
output does; `buf_parse_tree(&ps, &lx)` then drives
[`buf_parse`](design.md#the-parser-driver-buf_parse) over the baked tables
into a concrete syntax tree. `examples/expr_main.c` is a worked driver —
`echo '1 + 2 * 3' | build/expr_pgen` prints the parenthesised parse tree.

What you also get is a spec reader and DFA builder that fail loudly and
precisely. A malformed regex is reported at its `line:col` in the `.bflo` file:

```sh
$ bin/buffalo lex broken.bflo
buffalo: broken.bflo:3:8: unterminated character class
```

and so is a token header that has drifted from the spec:

```sh
$ bin/buffalo lex examples/calc.bflo
buffalo: examples/calc_tokens.h: token header has 'TOK_FLOAT' where 'TOK_INT' is expected
```

### The token header

`buffalo lex SPEC.bflo` validates a checked-in `<name>_tokens.h` derived from the
spec path: the trailing `.bflo` is replaced with `_tokens.h`, so
`examples/calc.bflo` pairs with `examples/calc_tokens.h`. Pass `--tokens PATH` to
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
$ bin/buffalo lex   examples/calc.bflo [-o OUT.gen.c] [--tokens TOK.h]
$ bin/buffalo parse examples/expr.bflo [-o OUT.parse.gen.c] [--tokens TOK.h]
```

## The two build paths

There are two ways to get from a `.bflo` spec to a program, and they must
produce byte-identical output. `make generated` and `make native` run both
over every example in the suite — the lexer examples `digits`, `calc`,
`clike`, `json` (`buffalo lex`), and the lex+parse example `expr` (`buffalo
parse`, `_pgen` / `_pnative` build stems); `make check` invokes them under a
no-cccc skip gate.

```sh
make generated   # bin/buffalo lex -> .gen.c, then plain cc; golden diff per example
make native      # one cccc -c=native invocation per example; generated/native parity
                 # diff, plus a three-way diff against the hand-written build/digits
```

The underlying invocations, for `digits`:

```sh
# inspectable path: lower, then build with a stock cc
bin/buffalo lex examples/digits.bflo -o build/digits.bflo.gen.c
cc -O2 -Iruntime -Iexamples -o build/digits_gen \
    build/digits.bflo.gen.c examples/digits_main.c runtime/buf_rt.c

# one-shot path: cccc does lowering + build in one step, no intermediate file.
# buf_comptime.c pulls the src/buf_*.c comptime modules in itself with
# `#include @comptime`, so cccc just needs -Isrc on the include path.
cccc -c=native src/buf_comptime.c runtime/buf_rt.c examples/digits_main.c \
    -Iinclude/buffalo -Isrc -Iruntime -Iexamples \
    -D BUF_SPEC='"examples/digits.bflo"' -D BUF_STOP_AFTER=5 -o build/digits_native
```

`-D` (not a source `#define`) carries `BUF_SPEC` and `BUF_STOP_AFTER` —
comptime bodies do not see ordinary source `#define`s, which is why the
`native` invocation passes `BUF_STOP_AFTER=5` explicitly where `bin/buffalo`
does it for you.
