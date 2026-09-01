/*
 * buf_comptime.c -- the file buffalo passes to cccc; it drives the comptime
 * pass.
 *
 * It runs entirely inside cccc's comptime VM. The pure-C modules are ordinary
 * `.h`/`.c` pairs (buf_rx, buf_tokcheck, buf_nfa, buf_dfa) whose `.c` bodies
 * are pulled straight in here with `#include @comptime "buf_*.c"` (so
 * `-Isrc` is on the cccc command line) -- the bodies compile into the
 * comptime program and are callable, recursively. The same `.c` files link
 * into the host unit tests under a plain `cc`. buf_emit.h stays header-only:
 * it uses the reflection builtins and only ever compiles here.
 * `buf_rt.h` comes in with `#include @shared` (not `@comptime`): the emitted
 * `buf_next` wrapper names `buf_run` and the table symbols in a Quote()
 * template, and a plain-include extern is rejected by Quote's identifier
 * resolver.
 *
 * This file is the boundary: the modules never call MacroErrorAt (they must
 * also build under a plain `cc` for the host unit tests), so they accumulate
 * a sticky first-wins error string, and this file lifts that string into a
 * comptime diagnostic pointed at the .bflo file.
 *
 * A successful run reads the spec, validates the token header, builds the
 * NFA (buf_nfa) then the DFA (buf_dfa), and emits the four `static const`
 * DFA tables plus the buf_next wrapper (buf_emit.h) into the .gen.c. That
 * file then builds with a plain `cc` against runtime/buf_rt.c and an example
 * _main.c (`make generated`); `cccc -c=native` does the whole thing in one
 * invocation (`make native`), and the two outputs must match.
 *
 * With -D BUF_EMIT_PARSER (what `buffalo parse` passes), the spec's %grammar
 * section is additionally lowered: buf_grammar builds LALR(1) parser tables
 * and buf_emit emits four more `static const int` tables plus the
 * buf_parse_tree wrapper. A spec with no %grammar section is then an error.
 *
 * M3 instrumentation (see docs/performance.md):
 *
 *   -D BUF_STOP_AFTER=n   stop the pipeline early, for per-phase timing.
 *       0 nothing   1 +read   2 +tokcheck   3 +NFA   4 +DFA   5 +emit
 *       6 +grammar  7 +parser emit
 *       Default 5 (lexer-only); `buffalo parse` passes 7. Rungs 6-7 are
 *       no-ops unless BUF_EMIT_PARSER is defined.
 *   -D BUF_STATS         print arena peaks (nfa/dfa state counts, pool use,
 *       class count, transition-table size; +grammar automaton size under
 *       BUF_EMIT_PARSER) to stderr after the DFA/grammar build.
 *       buf_dfa.h's own -D BUF_DFA_STATS adds closure-call / find-compare
 *       counters on top.
 *   -D BUF_MINIMIZE      run buf_dfa.h's opt-in Moore minimisation pass after
 *       the subset construction (off by default -- see docs/performance.md).
 *
 * Invocation (see bin/buffalo and docs/getting-started.md):
 *
 *   cccc -c=generated src/buf_comptime.c -Iinclude/buffalo -Isrc -Iruntime \
 *       -D BUF_SPEC='"examples/calc.bflo"' -o examples/calc.bflo.gen.c
 *
 * -D rather than a source #define: comptime bodies do not see ordinary
 * source #define's. BUF_SPEC is required. BUF_TOKENS_H is optional -- when
 * absent it is derived from BUF_SPEC by dropping a trailing ".bflo" and
 * appending "_tokens.h" (examples/calc.bflo -> examples/calc_tokens.h).
 */
#include @shared "buf_rt.h"
#include @comptime "buf_rx.c"
#include @comptime "buf_tokcheck.c"
#include @comptime "buf_nfa.c"
#include @comptime "buf_dfa.c"
/* Only `buffalo parse` pulls the grammar module in -- keeping it out of a
 * plain `buffalo lex` run saves ~0.5 s of comptime interpretation and ~1.5 MB
 * of VM data segment. cccc echoes this (empty, in lex mode) conditional into
 * the .gen.c the same way it already does `#ifndef BUF_SPEC`; it is inert. */
#ifdef BUF_EMIT_PARSER
#include @comptime "buf_grammar.c"
#endif
#include @comptime "buf_emit.h"
#include @comptime <stdio.h>
#include @comptime <string.h>

#ifndef BUF_SPEC
#define BUF_SPEC "examples/calc.bflo"
#endif

/* M3 ablation ladder -- see the header comment. 5 (lexer-only full pipeline)
 * by default; `buffalo parse` passes 7 to add the grammar build + parser emit
 * (rungs 6-7 are no-ops without BUF_EMIT_PARSER). */
#ifndef BUF_STOP_AFTER
#define BUF_STOP_AFTER 5
#endif

[[cccc::comptime]]
void buf_compile(void) {
    static BufRx  rx;
    static BufTc  tc;
    static BufNfa nfa;
    static BufDfa dfa;
#ifdef BUF_EMIT_PARSER
    static BufGrammar g;
#endif
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

#ifdef BUF_EMIT_PARSER
    if (!rx.has_grammar) {
        MacroErrorAt(NULL, "buffalo: %s: `buffalo parse` needs a %%grammar "
                     "section (none found)", spec);
        return;
    }
#endif

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

#ifdef BUF_EMIT_PARSER
    if (BUF_STOP_AFTER < 6) return;
    if (buf_grammar_build(&g, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", g.error);
        return;
    }

#ifdef BUF_STATS
    fprintf(stderr,
            "  lalr  states  %6d / %d\n"
            "  lalr  items   %6d / %d\n"
            "  lalr  edges   %6d / %d\n"
            "  lalr  ntok    %6d    nnonterm %d\n",
            g.nstates,   BUF_LALR_MAX_STATES,
            g.pool_used, BUF_LALR_MAX_ITEMS,
            g.nedges,    BUF_LALR_MAX_EDGES,
            g.ntok,      g.nnonterm);
#endif

    if (BUF_STOP_AFTER < 7) return;
    if (buf_emit_parser_tables(&dfa, &g, &rx) != 0) {
        MacroErrorAt(NULL, "buffalo: %s", g.error);
        return;
    }
#endif /* BUF_EMIT_PARSER */
}

buf_compile();
