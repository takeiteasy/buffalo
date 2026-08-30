# buffalo

A lexer and parser generator that runs entirely inside
[cccc](https://git.sr.ht/~takeiteasy/cccc)'s comptime pass.

A spec file is read at compile time, compiled to DFA/parser tables, and lowered
to plain C. The generated C builds and runs with a stock `cc` — no `cccc`, no
external `lex`/`yacc` binary, no `.bflo → .c` build step, no checked-in
generated file.

- **lex** — `.bflo` spec (`%tokens` + `NAME regex` rules) → DFA tables + a
  `buf_next()` driver.
- **parse** — grammar productions in the same `.bflo` file → parser tables.
  *(Not implemented yet.)*

## Build

```sh
make            # builds the digits demo lexer with the system cc, no cccc
make check      # host unit tests + digits golden test; also spec/generated/native if cccc is present
make spec       # runs the comptime pipeline (read + NFA + DFA + emit) over the specs (needs cccc)
make generated  # lower every example to a .gen.c, then build + golden-diff each with a plain cc (needs cccc)
make native     # one-shot cccc -c=native build of every example; generated/native parity diff (needs cccc)
make bench      # per-phase comptime-cost measurement (needs cccc + perl)
```

## Docs

- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/performance.md](docs/performance.md) — the comptime-cost measurement and how to re-run it.
- [docs/lex-spec-format.md](docs/lex-spec-format.md) — the `.bflo` spec reference.
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
