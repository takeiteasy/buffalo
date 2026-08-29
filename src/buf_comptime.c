/*
 * buf_comptime.c -- the only file buffalo ever passes to cccc.
 *
 * It runs entirely inside cccc's comptime VM. The spec reader (buf_rx.h) and
 * the token-header validator (buf_tokcheck.h) are pulled in with `#include
 * @comptime` -- ordinary static function bodies in an @comptime-routed
 * header compile into the comptime program and are callable, recursively.
 * `buf_rt.h` comes in with `#include @shared` (not `@comptime`): from M4 the
 * emitted `buf_next` wrapper will name `buf_run` and the table symbols in a
 * Quote() template, and a plain-include extern is rejected by Quote's
 * identifier resolver.
 *
 * This file is the boundary: buf_rx.h / buf_tokcheck.h never call
 * MacroErrorAt (they must also build under a plain `cc` for the host unit
 * tests), so they accumulate a sticky first-wins error string, and this
 * file lifts that string into a comptime diagnostic pointed at the .l file.
 *
 * At M1 there is no emit step: a successful run touches no nodes and yields
 * an all-but-empty .gen.c. The DFA tables and the buf_next wrapper arrive
 * at M4.
 *
 * Invocation (see bin/buffalo and docs/getting-started.md):
 *
 *   cccc -c=generated src/buf_comptime.c -Iinclude/buffalo -Iruntime \
 *       -D BUF_SPEC='"examples/calc.l"' -o examples/calc.l.gen.c
 *
 * -D rather than a source #define: comptime bodies do not see ordinary
 * source #define's. BUF_SPEC is required. BUF_TOKENS_H is optional -- when
 * absent it is derived from BUF_SPEC by dropping a trailing ".l" and
 * appending "_tokens.h" (examples/calc.l -> examples/calc_tokens.h).
 */
#include @shared "buf_rt.h"
#include @comptime "buf_rx.h"
#include @comptime "buf_tokcheck.h"
#include @comptime < stdio.h>
#include @comptime < string.h>

#ifndef BUF_SPEC
#define BUF_SPEC "examples/calc.l"
#endif

[[cccc::comptime]]
void buf_compile(void) {
    static BufRx rx;
    static BufTc tc;
    const char  *spec = BUF_SPEC;
    char         tokens_h[512];
    int          k = 0;

#ifdef BUF_TOKENS_H
    {
        const char *t = BUF_TOKENS_H;
        while (t[k] && k < (int)sizeof(tokens_h) - 1) {
            tokens_h[k] = t[k];
            k++;
        }
        tokens_h[k] = '\0';
    }
#else
    /* derive from the spec path: strip a trailing ".l", append "_tokens.h" */
    {
        const char *suffix = "_tokens.h";
        int         n = 0, base, i;
        while (spec[n]) n++;
        base = (n >= 2 && spec[n - 2] == '.' && spec[n - 1] == 'l') ? n - 2 : n;
        for (i = 0; i < base && k < (int)sizeof(tokens_h) - 1; i++)
            tokens_h[k++] = spec[i];
        for (i = 0; suffix[i] && k < (int)sizeof(tokens_h) - 1; i++)
            tokens_h[k++] = suffix[i];
        tokens_h[k] = '\0';
    }
#endif

    if (buf_rx_read_file(&rx, spec) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", rx.error);
        return;
    }

    if (buf_tc_read_file(&tc, tokens_h) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", tc.error);
        return;
    }
    if (buf_tc_check(&tc, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", tc.error);
        return;
    }

    /* M1 stops here: reader + token-header validation only. The DFA
     * construction (M2) and the emitter (M4) plug in below. */
}

buf_compile();
