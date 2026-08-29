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

**M0 — pre-generator scaffold.** The runtime (`runtime/buf_rt.{h,c}`) and the
shape of a generated table file are in place and verified by a hand-written
demo; the regex reader, DFA construction and the comptime emitter are not built
yet. See [ROADMAP.md](ROADMAP.md).

## Build

```sh
make          # builds the M0 demo lexer with the system cc, no cccc
make check    # lexes examples/digits.txt, diffs against the golden output
```

## Docs

- [ROADMAP.md](ROADMAP.md) — milestones M1–M6, Phase 1.5, Phase 2.
- [docs/design.md](docs/design.md) — the two-universe split, decision log, known limitations.
- [docs/lex-spec-format.md](docs/lex-spec-format.md) — the `.l` spec reference.
- [docs/getting-started.md](docs/getting-started.md) — building a spec end to end.

## License

GPLv3 — see [LICENSE](LICENSE).
