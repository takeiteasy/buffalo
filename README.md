# buffalo

A lexer and parser generator that runs entirely inside
[cccc](https://git.sr.ht/~takeiteasy/cccc)'s comptime pass.

A spec file is read at compile time, compiled to DFA/parser tables, and lowered
to plain C. The generated C builds and runs with a stock `cc` — no `cccc`, no
external `lex`/`yacc` binary, no `.l → .c` build step, no checked-in generated
file.

- **lex** — `.l` spec (`%tokens` + `NAME regex` rules) → DFA tables + a
  `buf_next()` driver.
- **parse** — `.y` grammar → parser tables. *(Phase 2, not implemented.)*

## Status

**M2 — NFA + DFA construction.** `bin/buffalo lex` reads a `.l` spec at compile
time into a regex AST with `line:col` diagnostics, validates the checked-in
`<name>_tokens.h` against the `%tokens` list, then runs Thompson construction
(regex AST → ε-NFA) and subset construction (ε-NFA → DFA over alphabet
equivalence classes) inside cccc's comptime pass. The comptime emitter (M4) is
not built yet, so the generated `.gen.c` is still all but empty — but the four
runtime tables are computed and host-tested against the real `buf_run` driver.
See [ROADMAP.md](ROADMAP.md).

## Build

```sh
make          # builds the M0 demo lexer with the system cc, no cccc
make check    # host unit tests + digits golden test; also `make spec` if cccc is present
make spec     # runs the comptime front half (read + NFA + DFA) over examples/calc.l (needs cccc)
```

## Docs

- [ROADMAP.md](ROADMAP.md) — milestones M1–M6, Phase 1.5, Phase 2.
- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/lex-spec-format.md](docs/lex-spec-format.md) — the `.l` spec reference.
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
