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

**M4 — the emitter, both build paths live.** `bin/buffalo lex` reads a `.l`
spec at compile time into a regex AST with `line:col` diagnostics, validates
the checked-in `<name>_tokens.h` against the `%tokens` list, runs Thompson
construction (regex AST → ε-NFA) and subset construction (ε-NFA → DFA over
alphabet equivalence classes), and emits four `static const` DFA tables plus
a `buf_next` wrapper into the generated `.gen.c` — all inside cccc's comptime
pass. That file then builds with a stock `cc` against `runtime/buf_rt.c` and
an example `_main.c` (`make generated`), and `cccc -c=native` does the whole
thing in one invocation (`make native`); the two outputs are byte-identical.
Comptime cost is ~0.38 s for a ~45-rule lexer, emission free (see
[docs/performance.md](docs/performance.md)). See [ROADMAP.md](ROADMAP.md).

## Build

```sh
make            # builds the digits demo lexer with the system cc, no cccc
make check      # host unit tests + digits golden test; also spec/generated/native if cccc is present
make spec       # runs the comptime pipeline (read + NFA + DFA + emit) over the specs (needs cccc)
make generated  # lower a spec to a .gen.c, then build it with a plain cc (needs cccc)
make native     # one-shot cccc -c=native build; three-way parity diff (needs cccc)
make bench      # per-phase comptime-cost measurement (needs cccc + perl)
```

## Docs

- [ROADMAP.md](ROADMAP.md) — milestones M1–M6, Phase 1.5, Phase 2.
- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/performance.md](docs/performance.md) — the M3 comptime-cost measurement and how to re-run it.
- [docs/lex-spec-format.md](docs/lex-spec-format.md) — the `.l` spec reference.
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
