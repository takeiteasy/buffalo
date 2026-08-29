# buffalo -- build / check / clean
#
# M0 builds and verifies the hand-written demo lexer with the system cc only;
# no cccc is involved yet. The `generated` and `native` parity targets arrive
# at M4 once src/buf_comptime.c and the emitter exist.

CC     ?= cc
CFLAGS ?= -O2 -Wall
CCCC   ?= cccc

RT_SRC  := runtime/buf_rt.c
RT_HDRS := runtime/buf_rt.h

.PHONY: all check clean
.PRECIOUS: build/digits

all: build/digits

build:
	mkdir -p build

build/digits: examples/digits_main.c examples/digits_tables.c $(RT_SRC) $(RT_HDRS) \
              examples/digits_tokens.h | build
	$(CC) $(CFLAGS) -Iruntime -Iexamples -o $@ \
	    examples/digits_main.c examples/digits_tables.c $(RT_SRC)

check: build/digits
	@build/digits < examples/digits.txt | diff -u examples/digits.expected - \
	    && echo "ok   digits" \
	    || { echo "FAIL digits"; exit 1; }

clean:
	rm -rf build
