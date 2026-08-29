# Getting started

## Prerequisites

- A C compiler as `cc` (clang or gcc).
- For M4 onward: the `cccc` binary on `PATH`, or `$CCCC` pointing at it.

## M0: the hand-written demo

M0 ships no generator. It ships the runtime and a hand-written table file
(`examples/digits_tables.c`) of the exact shape the generator will emit, so the
runtime contract is pinned down and testable.

```sh
make          # cc -O2 -Wall, no cccc -> build/digits
make check    # build/digits < examples/digits.txt | diff against examples/digits.expected
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

## `bin/buffalo`

`bin/buffalo` is a thin shell wrapper that assembles the `cccc` invocation. It
has real subcommands now but the generator behind `lex` does not exist yet:

```sh
$ bin/buffalo help
$ bin/buffalo version
$ bin/buffalo lex examples/calc.l      # errors: src/buf_comptime.c lands at M1
```

## What each milestone unlocks

See [ROADMAP.md](../ROADMAP.md). In short:

- **M1** — `bin/buffalo lex` starts working: `.l` → regex AST, token-header
  validation.
- **M4** — `bin/buffalo lex examples/calc.l -o build/calc.l.gen.c` produces a
  real table file that builds with `cc` against `runtime/buf_rt.c` and a
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
