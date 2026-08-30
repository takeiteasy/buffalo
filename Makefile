# buffalo -- build / check / clean
#
# The digits demo lexer (build/digits) still builds and runs with the system
# cc only, no cccc. The comptime front half is header-only, dependency-free C
# pulled into cccc via `#include @comptime`, and also built with a plain cc
# for host unit tests:
#
#   buf_rx.h        .bflo spec + regex reader         -> tests/t_rx.c
#   buf_tokcheck.h  <name>_tokens.h validator         -> tests/t_tokcheck.c
#   buf_nfa.h       Thompson construction (AST->NFA)  -> tests/t_nfa.c
#   buf_dfa.h       subset construction (NFA->DFA)    -> tests/t_dfa.c
#   buf_grammar.h   LALR(1) table construction        -> tests/t_grammar.c
#
# buf_grammar.h is host-test-only for now (not yet wired into
# src/buf_comptime.c's comptime pipeline -- see the tracker) but follows the
# same dual-compile shape as the other comptime headers above.
#
# buf_emit.h (DFA -> GlobalVar tables + buf_next) is comptime-VM only -- it
# uses cccc's reflection builtins, so no host test includes it.
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
            include/buffalo/buf_grammar.h

EXAMPLES := digits calc clike json

.PHONY: all check test spec generated native bench clean
.PRECIOUS: build/digits $(EXAMPLES:%=build/%_gen) $(EXAMPLES:%=build/%_native)

all: build/digits

build:
	mkdir -p build

build/digits: examples/digits_main.c examples/digits_tables.c $(RT_SRC) $(RT_HDRS) \
              examples/digits_tokens.h | build
	$(CC) $(CFLAGS) -Iruntime -Iexamples -o $@ \
	    examples/digits_main.c examples/digits_tables.c $(RT_SRC)

# Host unit tests for the comptime headers -- plain cc, no cccc.
build/t_rx: tests/t_rx.c $(CT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_rx.c

build/t_tokcheck: tests/t_tokcheck.c $(CT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_tokcheck.c

build/t_nfa: tests/t_nfa.c $(CT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_nfa.c

build/t_dfa: tests/t_dfa.c $(CT_HDRS) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_dfa.c $(RT_SRC)

build/t_grammar: tests/t_grammar.c $(CT_HDRS) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_grammar.c $(RT_SRC)

build/t_parse: tests/t_parse.c $(CT_HDRS) $(RT_SRC) $(RT_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_parse.c $(RT_SRC)

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
# .gen.c. clike.bflo is the ~40-rule stress case -- keep it here so a comptime
# regression on the big spec is caught by `make check`.
spec: | build
	@bin/buffalo lex examples/calc.bflo  -o build/calc.bflo.gen.c  && echo "ok   spec calc"
	@bin/buffalo lex examples/clike.bflo -o build/clike.bflo.gen.c && echo "ok   spec clike"
	@bin/buffalo lex examples/json.bflo  -o build/json.bflo.gen.c  && echo "ok   spec json"

# -- generated / native parity ------------------------------------------------
#
# The lowered .gen.c: `bin/buffalo lex` runs cccc -c=generated over
# src/buf_comptime.c with the spec as -D BUF_SPEC.
build/%.bflo.gen.c: examples/%.bflo examples/%_tokens.h src/buf_comptime.c \
                 include/buffalo/buf_emit.h $(CT_HDRS) $(RT_HDRS) | build
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
build/%_native: src/buf_comptime.c examples/%_main.c $(RT_SRC) \
                include/buffalo/buf_emit.h $(CT_HDRS) $(RT_HDRS) \
                examples/%.bflo examples/%_tokens.h | build
	$(CCCC) -c=native src/buf_comptime.c $(RT_SRC) examples/$*_main.c \
	    -Iinclude/buffalo -Iruntime -Iexamples \
	    -D BUF_SPEC='"examples/$*.bflo"' -D BUF_STOP_AFTER=5 -o $@

# generated: build every example's lowered spec with a plain cc and run its
# golden diff.
generated: $(EXAMPLES:%=build/%_gen)
	@for e in $(EXAMPLES); do \
	    build/$${e}_gen < examples/$${e}.txt | diff -u examples/$${e}.expected - \
	        && echo "ok   generated $$e" \
	        || { echo "FAIL generated $$e"; exit 1; }; \
	done

# native: the one-shot cccc path. Every example must match its own golden
# output *and* be byte-identical to the generated build; digits additionally
# has a hand-written reference (build/digits) for a three-way parity check.
native: $(EXAMPLES:%=build/%_native) $(EXAMPLES:%=build/%_gen) build/digits
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
	@a=$$(build/digits        < examples/digits.txt); \
	 b=$$(build/digits_gen    < examples/digits.txt); \
	 c=$$(build/digits_native < examples/digits.txt); \
	 [ "$$a" = "$$b" ] && [ "$$b" = "$$c" ] \
	    && echo "ok   parity hand-written == generated == native (digits)" \
	    || { echo "FAIL three-way parity"; exit 1; }

# Per-phase comptime cost of the pipeline (the BUF_STOP_AFTER ablation ladder
# in src/buf_comptime.c). Needs cccc + perl. Not part of `check` -- it is
# slow. See docs/performance.md.
bench:
	@tests/bench.sh

clean:
	rm -rf build examples/*.gen.c
