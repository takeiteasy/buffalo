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
 * file lifts that string into a comptime diagnostic pointed at the .bflo file.
 *
 * A successful run reads the spec, validates the token header, builds the
 * NFA (buf_nfa.h) then the DFA (buf_dfa.h), and emits the four `static
 * const` DFA tables plus the buf_next wrapper (buf_emit.h) into the .gen.c.
 * That file then builds with a plain `cc` against runtime/buf_rt.c and an
 * example _main.c (`make generated`); `cccc -c=native` does the whole thing
 * in one invocation (`make native`), and the two outputs must match.
 *
 * M3 instrumentation (see docs/performance.md):
 *
 *   -D BUF_STOP_AFTER=n   stop the pipeline early, for per-phase timing.
 *       0 nothing   1 +read   2 +tokcheck   3 +NFA   4 +DFA   5 +emit (default)
 *   -D BUF_STATS         print arena peaks (nfa/dfa state counts, pool use,
 *       class count, transition-table size) to stderr after the DFA build.
 *       buf_dfa.h's own -D BUF_DFA_STATS adds closure-call / find-compare
 *       counters on top.
 *   -D BUF_MINIMIZE      run buf_dfa.h's opt-in Moore minimisation pass after
 *       the subset construction (off by default -- see docs/performance.md).
 *
 * Invocation (see bin/buffalo and docs/getting-started.md):
 *
 *   cccc -c=generated src/buf_comptime.c -Iinclude/buffalo -Iruntime \
 *       -D BUF_SPEC='"examples/calc.bflo"' -o examples/calc.bflo.gen.c
 *
 * -D rather than a source #define: comptime bodies do not see ordinary
 * source #define's. BUF_SPEC is required. BUF_TOKENS_H is optional -- when
 * absent it is derived from BUF_SPEC by dropping a trailing ".bflo" and
 * appending "_tokens.h" (examples/calc.bflo -> examples/calc_tokens.h).
 */
#include @shared "buf_rt.h"
#include @comptime "buf_rx.h"
#include @comptime "buf_tokcheck.h"
#include @comptime "buf_nfa.h"
#include @comptime "buf_dfa.h"
#include @comptime "buf_emit.h"
#include @comptime <stdio.h>
#include @comptime <string.h>

#ifndef BUF_SPEC
#define BUF_SPEC "examples/calc.bflo"
#endif

/* M3 ablation ladder -- see the header comment. 5 (full pipeline) by default. */
#ifndef BUF_STOP_AFTER
#define BUF_STOP_AFTER 5
#endif

[[cccc::comptime]]
void buf_compile(void) {
    static BufRx  rx;
    static BufTc  tc;
    static BufNfa nfa;
    static BufDfa dfa;
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
    /* derive from the spec path: strip a trailing ".bflo", append "_tokens.h" */
    {
        const char *suffix = "_tokens.h";
        const char *ext    = ".bflo";
        int         n = 0, extlen = 0, base, matches = 0, i;
        while (spec[n]) n++;
        while (ext[extlen]) extlen++;
        if (n >= extlen) {
            matches = 1;
            for (i = 0; i < extlen; i++)
                if (spec[n - extlen + i] != ext[i]) { matches = 0; break; }
        }
        base = matches ? n - extlen : n;
        for (i = 0; i < base && k < (int)sizeof(tokens_h) - 1; i++)
            tokens_h[k++] = spec[i];
        for (i = 0; suffix[i] && k < (int)sizeof(tokens_h) - 1; i++)
            tokens_h[k++] = suffix[i];
        tokens_h[k] = '\0';
    }
#endif

    if (BUF_STOP_AFTER < 1) return;
    if (buf_rx_read_file(&rx, spec) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", rx.error);
        return;
    }

    if (BUF_STOP_AFTER < 2) return;
    if (buf_tc_read_file(&tc, tokens_h) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", tc.error);
        return;
    }
    if (buf_tc_check(&tc, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", tc.error);
        return;
    }

    if (BUF_STOP_AFTER < 3) return;
    if (buf_nfa_build(&nfa, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", nfa.error);
        return;
    }

    if (BUF_STOP_AFTER < 4) return;
    if (buf_dfa_build(&dfa, &nfa, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", dfa.error);
        return;
    }

#ifdef BUF_STATS
    /* %% -- a literal percent in a comptime printf format (memory: MACROS.md) */
    fprintf(stderr,
            "buffalo stats %s\n"
            "  rx    nodes   %6d / %d\n"
            "  rx    rules   %6d / %d\n"
            "  nfa   states  %6d / %d\n"
            "  dfa   states  %6d / %d  (pre-min %d)\n"
            "  dfa   classes %6d / 256\n"
            "  dfa   trans   %6d / %d  (nstates*nclass)\n"
            "  dfa   pool    %6d / %d\n",
            spec,
            rx.node_count,  BUF_RX_MAX_NODES,
            rx.rule_count,  BUF_RX_MAX_RULES,
            nfa.state_count, BUF_NFA_MAX_STATES,
            dfa.nstates,    BUF_DFA_MAX_STATES, dfa.nstates_premin,
            dfa.nclass,
            dfa.nstates * dfa.nclass, BUF_DFA_MAX_TRANS,
            dfa.pool_used,  BUF_DFA_SET_POOL);
#ifdef BUF_DFA_STATS
    fprintf(stderr,
            "  dfa   closure-calls  %8ld\n"
            "  dfa   find-compares  %8ld\n",
            dfa.stat_closure_calls, dfa.stat_find_compares);
#endif
#endif

    if (BUF_STOP_AFTER < 5) return;
    if (buf_emit_tables(&dfa, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", dfa.error);
        return;
    }
}

buf_compile();
