# buffalo -- build / check / clean
#
# The M0 demo lexer (build/digits) still builds and runs with the system cc
# only, no cccc. M1 adds the comptime front half -- the .l spec + regex
# reader (include/buffalo/buf_rx.h) and the token-header validator
# (include/buffalo/buf_tokcheck.h) -- with host unit tests that also build
# with a plain cc. The `spec` target runs the comptime pass itself over
# examples/calc.l; `check` invokes it but skips it with a notice when cccc
# is not on PATH. The `generated` and `native` parity targets arrive at M4
# once the emitter exists.

CC     ?= cc
CFLAGS ?= -O2 -Wall
CCCC   ?= cccc

RT_SRC   := runtime/buf_rt.c
RT_HDRS  := runtime/buf_rt.h
RX_HDRS  := include/buffalo/buf_rx.h include/buffalo/buf_tokcheck.h

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
build/t_rx: tests/t_rx.c $(RX_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_rx.c

build/t_tokcheck: tests/t_tokcheck.c $(RX_HDRS) | build
	$(CC) $(CFLAGS) -Iinclude/buffalo -Iruntime -o $@ tests/t_tokcheck.c

test: build/t_rx build/t_tokcheck
	@build/t_rx
	@build/t_tokcheck

check: build/digits test
	@build/digits < examples/digits.txt | diff -u examples/digits.expected - \
	    && echo "ok   digits" \
	    || { echo "FAIL digits"; exit 1; }
	@command -v $(CCCC) >/dev/null 2>&1 \
	    && $(MAKE) --no-print-directory spec \
	    || echo "skip spec (no cccc on PATH)"

# Run the comptime pass over the reference spec. Needs cccc on PATH (or
# $CCCC). Produces an all-but-empty .gen.c at M1 -- what it proves is that
# the reader and the token-header validation load and pass inside cccc.
spec: | build
	@bin/buffalo lex examples/calc.l -o build/calc.l.gen.c && echo "ok   spec"

clean:
	rm -rf build examples/*.gen.c
