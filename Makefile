# buffalo -- build / check / clean
#
# The digits demo lexer (build/digits) still builds and runs with the system
# cc only, no cccc. The comptime front half is ordinary .h/.c module pairs:
# the headers declare, src/buf_*.c define. src/buf_comptime.c drives the pass
# and pulls each module's .c body straight in with `#include @comptime
# "buf_*.c"` (so cccc runs with -Isrc). The same .c files also build with a
# plain cc, linked into the host unit tests:
#
#   buf_rx.{h,c}        .bflo spec + regex reader         -> tests/t_rx.c
#   buf_tokcheck.{h,c}  <name>_tokens.h validator         -> tests/t_tokcheck.c
#   buf_nfa.{h,c}       Thompson construction (AST->NFA)  -> tests/t_nfa.c
#   buf_dfa.{h,c}       subset construction (NFA->DFA)    -> tests/t_dfa.c
#   buf_grammar.{h,c}   LALR(1) table construction        -> tests/t_grammar.c
#
# buf_grammar runs in the comptime pipeline too, behind `buffalo parse` /
# -D BUF_EMIT_PARSER (src/buf_comptime.c `#include @comptime`s it only then);
# it follows the same dual-compile shape as the other modules above.
#
# buf_emit.{h,c} (DFA -> GlobalVar tables + buf_next) is comptime-VM only --
# it uses cccc's reflection builtins, so no host test links it and it is not
# built by a plain cc.
#
# t_dfa and t_grammar also link runtime/buf_rt.c and drive the real buf_run
# over freshly built tables. The `spec` target runs the full comptime pipeline over
# every reference spec; `check` invokes it -- plus the `generated` and
# `native` parity targets -- but skips all three with a notice when cccc is
# not on PATH. `bench` is the per-phase cost measurement (docs/performance.md).
#
# `generated` lowers a spec to a .gen.c with `bin/buffalo lex` (cccc
# -c=generated) then builds it with a plain cc against the runtime and an
# example _main.c. `native` does the whole thing in one cccc -c=native
# invocation. Both must produce byte-identical output to each other, and (for
# digits, the one example with a hand-written reference table file) to
# build/digits too. EXAMPLES lists the example suite; big.bflo stays out of
# it -- it's a bench/stress fixture, not a worked example.

CC     ?= cc
CFLAGS ?= -O2 -Wall
CCCC   ?= cccc

RT_SRC   := runtime/buf_rt.c
RT_HDRS  := runtime/buf_rt.h
CT_HDRS  := include/buffalo/buf_rx.h include/buffalo/buf_tokcheck.h \
            include/buffalo/buf_nfa.h include/buffalo/buf_dfa.h \
            include/buffalo/buf_grammar.h include/buffalo/buf_emit.h
# The pure-C comptime modules: `.h` declares, `src/buf_*.c` defines. `cc`
# links them into the host tests; src/buf_comptime.c `#include @comptime`s
# buf_rx/tokcheck/nfa/dfa always and buf_grammar under -D BUF_EMIT_PARSER.
# They are build inputs to the cccc targets but not command-line arguments.
CT_SRC   := src/buf_rx.c src/buf_tokcheck.c src/buf_nfa.c src/buf_dfa.c \
            src/buf_grammar.c

# EXAMPLES are lexer-only (`buffalo lex`); PARSE_EXAMPLES additionally carry a
# %grammar section and are lowered by `buffalo parse` to parser tables too.
EXAMPLES       := digits calc clike json
PARSE_EXAMPLES := expr

.PHONY: all check test spec generated native bench clean
.PRECIOUS: build/digits $(EXAMPLES:%=build/%_gen) $(EXAMPLES:%=build/%_native) \
           $(PARSE_EXAMPLES:%=build/%_pgen) $(PARSE_EXAMPLES:%=build/%_pnative)

all: build/digits

build:
	mkdir -p build

build/digits: examples/digits_main.c examples/digits_tables.c $(RT_SRC) $(RT_HDRS) \
              examples/digits_tokens.h | build
	$(CC) $(CFLAGS) -Iruntime -Iexamples -o $@ \
	    examples/digits_main.c examples/digits_tables.c $(RT_SRC)

# Host unit tests for the comptime headers -- plain cc, no cccc.
# Each test links the dual-compiled module sources (CT_SRC). Unused modules
# in a test are dead code in the link, not an error -- simpler than tracking
# a per-test subset. t_dfa/t_grammar/t_parse additionally drive the runtime.
build/t_rx: tests/t_rx.c $(CT_HDRS) $(CT_SRC) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_rx.c $(CT_SRC)

build/t_tokcheck: tests/t_tokcheck.c $(CT_HDRS) $(CT_SRC) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_tokcheck.c $(CT_SRC)

build/t_nfa: tests/t_nfa.c $(CT_HDRS) $(CT_SRC) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_nfa.c $(CT_SRC)

build/t_dfa: tests/t_dfa.c $(CT_HDRS) $(CT_SRC) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_dfa.c $(CT_SRC) $(RT_SRC)

build/t_grammar: tests/t_grammar.c $(CT_HDRS) $(CT_SRC) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_grammar.c $(CT_SRC) $(RT_SRC)

build/t_parse: tests/t_parse.c $(CT_HDRS) $(CT_SRC) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_parse.c $(CT_SRC) $(RT_SRC)

test: build/t_rx build/t_tokcheck build/t_nfa build/t_dfa build/t_grammar build/t_parse
	@build/t_rx
	@build/t_tokcheck
	@build/t_nfa
	@build/t_dfa
	@build/t_grammar
	@build/t_parse

check: build/digits test
	@build/digits < examples/digits.txt | diff -u examples/digits.expected - \
	    && echo "ok   digits" \
	    || { echo "FAIL digits"; exit 1; }
	@command -v $(CCCC) >/dev/null 2>&1 \
	    && $(MAKE) --no-print-directory spec generated native \
	    || echo "skip spec/generated/native (no cccc on PATH)"

# Run the full comptime pipeline over every reference spec. Needs cccc on
# PATH (or $CCCC): it reads the spec, validates the token header, builds the
# NFA then the DFA, and emits the four tables + the buf_next wrapper into the
# .gen.c; the expr line additionally builds the LALR(1) tables + buf_parse_tree
# (`buffalo parse`). clike.bflo is the ~40-rule stress case -- keep it here so
# a comptime regression on the big spec is caught by `make check`.
spec: | build
	@bin/buffalo lex examples/calc.bflo  -o build/calc.bflo.gen.c  && echo "ok   spec calc"
	@bin/buffalo lex examples/clike.bflo -o build/clike.bflo.gen.c && echo "ok   spec clike"
	@bin/buffalo lex examples/json.bflo  -o build/json.bflo.gen.c  && echo "ok   spec json"
	@bin/buffalo parse examples/expr.bflo -o build/expr.bflo.parse.gen.c && echo "ok   spec expr (parse)"

# -- generated / native parity ------------------------------------------------
#
# The lowered .gen.c: `bin/buffalo lex` runs cccc -c=generated over
# src/buf_comptime.c with the spec as -D BUF_SPEC.
build/%.bflo.gen.c: examples/%.bflo examples/%_tokens.h src/buf_comptime.c \
                 $(CT_SRC) $(CT_HDRS) $(RT_HDRS) | build
	@bin/buffalo lex examples/$*.bflo -o $@

# Plain-cc build of a lowered spec: .gen.c + runtime + that example's driver.
build/%_gen: build/%.bflo.gen.c examples/%_main.c $(RT_SRC) $(RT_HDRS) \
             examples/%_tokens.h | build
	$(CC) $(CFLAGS) -Iruntime -Iexamples -o $@ \
	    build/$*.bflo.gen.c examples/$*_main.c $(RT_SRC)

# One-shot: cccc -c=native lowers spec + runtime + driver to an executable
# in a single invocation, no intermediate .gen.c. BUF_STOP_AFTER=5 must be a
# -D (the source #define fallback is not forwarded into the comptime body) --
# bin/buffalo passes it on the generated path; here it is explicit.
build/%_native: src/buf_comptime.c $(CT_SRC) examples/%_main.c $(RT_SRC) \
                $(CT_HDRS) $(RT_HDRS) \
                examples/%.bflo examples/%_tokens.h | build
	$(CCCC) -c=native src/buf_comptime.c $(RT_SRC) examples/$*_main.c \
	    -Iinclude/buffalo -Isrc -Iruntime -Iexamples \
	    -D BUF_SPEC='"examples/$*.bflo"' -D BUF_STOP_AFTER=5 -o $@

# -- parser examples: same two paths, plus -D BUF_EMIT_PARSER (grammar build +
# parser-table emit) and BUF_STOP_AFTER=7. Distinct `.parse.gen.c` / `_pgen` /
# `_pnative` stems so these never collide with the lexer-only patterns above.
build/%.bflo.parse.gen.c: examples/%.bflo examples/%_tokens.h src/buf_comptime.c \
                          $(CT_SRC) $(CT_HDRS) $(RT_HDRS) | build
	@bin/buffalo parse examples/$*.bflo -o $@

build/%_pgen: build/%.bflo.parse.gen.c examples/%_main.c $(RT_SRC) $(RT_HDRS) \
              examples/%_tokens.h | build
	$(CC) $(CFLAGS) -Iruntime -Iexamples -o $@ \
	    build/$*.bflo.parse.gen.c examples/$*_main.c $(RT_SRC)

build/%_pnative: src/buf_comptime.c $(CT_SRC) examples/%_main.c $(RT_SRC) \
                 $(CT_HDRS) $(RT_HDRS) \
                 examples/%.bflo examples/%_tokens.h | build
	$(CCCC) -c=native src/buf_comptime.c $(RT_SRC) examples/$*_main.c \
	    -Iinclude/buffalo -Isrc -Iruntime -Iexamples \
	    -D BUF_SPEC='"examples/$*.bflo"' -D BUF_EMIT_PARSER \
	    -D BUF_STOP_AFTER=7 -o $@

# generated: build every example's lowered spec with a plain cc and run its
# golden diff. PARSE_EXAMPLES go down the `buffalo parse` path (`_pgen`).
generated: $(EXAMPLES:%=build/%_gen) $(PARSE_EXAMPLES:%=build/%_pgen)
	@for e in $(EXAMPLES); do \
	    build/$${e}_gen < examples/$${e}.txt | diff -u examples/$${e}.expected - \
	        && echo "ok   generated $$e" \
	        || { echo "FAIL generated $$e"; exit 1; }; \
	done
	@for e in $(PARSE_EXAMPLES); do \
	    build/$${e}_pgen < examples/$${e}.txt | diff -u examples/$${e}.expected - \
	        && echo "ok   generated $$e (parse)" \
	        || { echo "FAIL generated $$e (parse)"; exit 1; }; \
	done

# native: the one-shot cccc path. Every example must match its own golden
# output *and* be byte-identical to the generated build; digits additionally
# has a hand-written reference (build/digits) for a three-way parity check.
native: $(EXAMPLES:%=build/%_native) $(EXAMPLES:%=build/%_gen) build/digits \
        $(PARSE_EXAMPLES:%=build/%_pnative) $(PARSE_EXAMPLES:%=build/%_pgen)
	@for e in $(EXAMPLES); do \
	    build/$${e}_native < examples/$${e}.txt | diff -u examples/$${e}.expected - \
	        && echo "ok   native $$e" \
	        || { echo "FAIL native $$e"; exit 1; }; \
	    a=$$(build/$${e}_gen    < examples/$${e}.txt); \
	    b=$$(build/$${e}_native < examples/$${e}.txt); \
	    [ "$$a" = "$$b" ] \
	        && echo "ok   parity generated == native ($$e)" \
	        || { echo "FAIL generated/native parity ($$e)"; exit 1; }; \
	done
	@for e in $(PARSE_EXAMPLES); do \
	    build/$${e}_pnative < examples/$${e}.txt | diff -u examples/$${e}.expected - \
	        && echo "ok   native $$e (parse)" \
	        || { echo "FAIL native $$e (parse)"; exit 1; }; \
	    a=$$(build/$${e}_pgen    < examples/$${e}.txt); \
	    b=$$(build/$${e}_pnative < examples/$${e}.txt); \
	    [ "$$a" = "$$b" ] \
	        && echo "ok   parity generated == native ($$e parse)" \
	        || { echo "FAIL generated/native parity ($$e parse)"; exit 1; }; \
	done
	@a=$$(build/digits        < examples/digits.txt); \
	 b=$$(build/digits_gen    < examples/digits.txt); \
	 c=$$(build/digits_native < examples/digits.txt); \
	 [ "$$a" = "$$b" ] && [ "$$b" = "$$c" ] \
	    && echo "ok   parity hand-written == generated == native (digits)" \
	    || { echo "FAIL three-way parity"; exit 1; }

# Per-phase comptime cost of the pipeline (the BUF_STOP_AFTER ablation ladder
# in src/buf_comptime.c): lexer rungs 0-5 for every spec, plus a parser-mode
# sweep (rungs 5-7, -D BUF_EMIT_PARSER) for a spec with a %grammar section.
# Needs cccc + perl. Not part of `check` -- it is slow. See docs/performance.md.
bench:
	@tests/bench.sh

clean:
	rm -rf build examples/*.gen.c
