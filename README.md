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

**M3 — spike passed, minimal emitter landed.** `bin/buffalo lex` reads a `.l`
spec at compile time into a regex AST with `line:col` diagnostics, validates
the checked-in `<name>_tokens.h` against the `%tokens` list, runs Thompson
construction (regex AST → ε-NFA) and subset construction (ε-NFA → DFA over
alphabet equivalence classes), and emits the four DFA tables plus a `buf_next`
wrapper into the generated `.gen.c` — all inside cccc's comptime pass. The M3
gate measured the comptime cost of this pipeline and reworked the DFA
construction hot spots off the back of it: ~0.38 s added compile time for a
~45-rule lexer, emission free (see
[docs/performance.md](docs/performance.md)). M4 turns the generated file into
one that builds with a stock `cc`. See [ROADMAP.md](ROADMAP.md).

## Build

```sh
make          # builds the M0 demo lexer with the system cc, no cccc
make check    # host unit tests + digits golden test; also `make spec` if cccc is present
make spec     # runs the full comptime pipeline (read + NFA + DFA + emit) over the specs (needs cccc)
make bench    # M3 per-phase comptime-cost measurement (needs cccc + perl)
```

## Docs

- [ROADMAP.md](ROADMAP.md) — milestones M1–M6, Phase 1.5, Phase 2.
- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/performance.md](docs/performance.md) — the M3 comptime-cost measurement and how to re-run it.
- [docs/lex-spec-format.md](docs/lex-spec-format.md) — the `.l` spec reference.
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
