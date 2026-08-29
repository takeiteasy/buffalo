# buffalo -- build / check / clean
#
# The M0 demo lexer (build/digits) still builds and runs with the system cc
# only, no cccc. The comptime front half is header-only, dependency-free C
# pulled into cccc via `#include @comptime`, and also built with a plain cc
# for host unit tests:
#
#   buf_rx.h        .l spec + regex reader           -> tests/t_rx.c
#   buf_tokcheck.h  <name>_tokens.h validator        -> tests/t_tokcheck.c
#   buf_nfa.h       Thompson construction (AST->NFA) -> tests/t_nfa.c
#   buf_dfa.h       subset construction (NFA->DFA)   -> tests/t_dfa.c
#
# t_dfa also links runtime/buf_rt.c and drives the real buf_run over the
# freshly built tables. The `spec` target runs the whole comptime front half
# over examples/calc.l; `check` invokes it but skips it with a notice when
# cccc is not on PATH. The `generated` and `native` parity targets arrive at
# M4 once the emitter exists.

CC     ?= cc
CFLAGS ?= -O2 -Wall
CCCC   ?= cccc

RT_SRC   := runtime/buf_rt.c
RT_HDRS  := runtime/buf_rt.h
CT_HDRS  := include/buffalo/buf_rx.h include/buffalo/buf_tokcheck.h \
            include/buffalo/buf_nfa.h include/buffalo/buf_dfa.h

.PHONY: all check test spec clean
.PRECIOUS: build/digits

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

test: build/t_rx build/t_tokcheck build/t_nfa build/t_dfa
	@build/t_rx
	@build/t_tokcheck
	@build/t_nfa
	@build/t_dfa

check: build/digits test
	@build/digits < examples/digits.txt | diff -u examples/digits.expected - \
	    && echo "ok   digits" \
	    || { echo "FAIL digits"; exit 1; }
	@command -v $(CCCC) >/dev/null 2>&1 \
	    && $(MAKE) --no-print-directory spec \
	    || echo "skip spec (no cccc on PATH)"

# Run the comptime front half over both reference specs. Needs cccc on PATH
# (or $CCCC). At M2 it reads the spec, validates the token header, and builds
# the NFA then the DFA inside the comptime VM; emission (a populated .gen.c)
# arrives at M4. clike.l is the ~40-rule stress case -- keep it here so a
# comptime regression on the big spec is caught by `make check`, not at M3.
spec: | build
	@bin/buffalo lex examples/calc.l  -o build/calc.l.gen.c  && echo "ok   spec calc"
	@bin/buffalo lex examples/clike.l -o build/clike.l.gen.c && echo "ok   spec clike"

clean:
	rm -rf build examples/*.gen.c
