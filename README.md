# buffalo

A lexer and parser generator that runs entirely inside
[cccc](https://git.sr.ht/~takeiteasy/cccc)'s comptime pass.

A spec file is read at compile time, compiled to DFA/parser tables, and lowered
to plain C. The generated C builds and runs with a stock `cc` — no `cccc`, no
external `lex`/`yacc` binary, no `.bflo → .c` build step, no checked-in
generated file.

- **lex** — `.bflo` spec (`%tokens` + `NAME regex` rules) → DFA tables + a
  `buf_next()` driver.
- **parse** — a `%grammar` section in the same `.bflo` file, read and
  validated by the spec reader, compiled to LALR(1) tables by `buffalo parse`,
  and driven at runtime by `buf_parse` into a concrete syntax tree. See
  `examples/expr.bflo` for a worked lexer + grammar.

## Build

```sh
cccc --build build.c                       # everything: tests, digits demo, all examples
cccc --build build.c --build-target=check  # the whole suite (same as the default build)
cccc --build build.c --build-target=run-t_dfa        # one host unit test
cccc --build build.c --build-target=calc_native      # one native example build
cccc --build build.c --build-cache                   # incremental; header deps tracked
cccc --build build.c --build-option=bench=1          # per-phase comptime-cost measurement
```

`cccc` must be on `PATH` — it is the compiler for every example target, not
just the build driver. The generated `.gen.c` files themselves still build
with a stock `cc`; only producing them (and the native targets) needs cccc.

## Docs

- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/performance.md](docs/performance.md) — the comptime-cost measurement and how to re-run it.
- [docs/bflo-format.md](docs/bflo-format.md) — the `.bflo` spec reference (lexer + grammar sections).
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
