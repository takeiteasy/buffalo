/*
 * buf_nfa.c -- definitions for buf_nfa.h (Thompson construction: regex AST -> epsilon-NFA).
 *
 * Pure C, no cccc dependency. Compiled by `cc` for the host unit tests
 * and forwarded into the comptime program by cccc -- src/buf_comptime.c
 * pulls the declarations in with `#include @comptime "buf_nfa.h"` and lists
 * this file on the cccc command line. Never calls MacroErrorAt: the
 * sticky first-wins error string is lifted into a comptime diagnostic by
 * src/buf_comptime.c (see buf_rx.h).
 */
#include "buf_nfa.h"

#include <stdio.h>
#include <string.h>

/* --- diagnostics (sticky, first-wins) -------------------------------- */

static void buf_nfa_err(BufNfa *nfa, int line, int col, const char *msg) {
    if (nfa->has_error) return;
    snprintf(nfa->error, sizeof(nfa->error), "%s:%d:%d: %s",
             nfa->spec_path, line, col, msg);
    nfa->has_error = 1;
}

/* --- state arena --------------------------------------------------- */

static int buf_nfa_state(BufNfa *nfa) {
    int i;
    if (nfa->state_count >= BUF_NFA_MAX_STATES) {
        buf_nfa_err(nfa, 0, 0, "regex too complex (NFA state arena exhausted)");
        return 0;
    }
    i = nfa->state_count++;
    {
        BufNfaState *s = &nfa->states[i];
        int k;
        for (k = 0; k < 32; k++) s->bits[k] = 0;
        s->target      = -1;
        s->eps_a       = -1;
        s->eps_b       = -1;
        s->accept_rule = -1;
    }
    return i;
}

/* Attach one epsilon edge from `s` to `t` (uses eps_a, then eps_b). */
static void buf_nfa_eps(BufNfa *nfa, int s, int t) {
    BufNfaState *st = &nfa->states[s];
    if (st->eps_a < 0)      st->eps_a = t;
    else if (st->eps_b < 0) st->eps_b = t;
    else buf_nfa_err(nfa, 0, 0, "internal: epsilon fan-out over two");
}

/* --- fragment builder (recurses over the regex AST) ---------------- */

/* Compile AST node `node` into a Thompson fragment; its entry state lands in
 * *pin, its (epsilon-free) exit state in *pout. Out-params rather than a
 * returned `{in, out}` struct: the fragment cases already name the endpoints
 * as plain `int` locals, so threading them through pointers keeps the
 * recursion allocation-free with no wrapper type. */
static void buf_nfa_frag(BufNfa *nfa, BufRx *rx, int node, int *pin, int *pout) {
    BufRxNode *nd = &rx->nodes[node];

    *pin = 0;
    *pout = 0;
    if (nfa->has_error) return;

    switch (nd->kind) {
    case BUF_RX_CLASS: {
        int a = buf_nfa_state(nfa);
        int b = buf_nfa_state(nfa);
        if (nfa->has_error) return;
        memcpy(nfa->states[a].bits, nd->bits, 32);
        nfa->states[a].target = b;
        *pin = a; *pout = b;
        return;
    }
    case BUF_RX_CONCAT: {
        int xi, xo, yi, yo;
        buf_nfa_frag(nfa, rx, nd->a, &xi, &xo);
        buf_nfa_frag(nfa, rx, nd->b, &yi, &yo);
        if (nfa->has_error) return;
        buf_nfa_eps(nfa, xo, yi);
        *pin = xi; *pout = yo;
        return;
    }
    case BUF_RX_ALT: {
        int a = buf_nfa_state(nfa);
        int b, xi, xo, yi, yo;
        if (nfa->has_error) return;
        buf_nfa_frag(nfa, rx, nd->a, &xi, &xo);
        buf_nfa_frag(nfa, rx, nd->b, &yi, &yo);
        if (nfa->has_error) return;
        b = buf_nfa_state(nfa);
        if (nfa->has_error) return;
        buf_nfa_eps(nfa, a, xi);
        buf_nfa_eps(nfa, a, yi);
        buf_nfa_eps(nfa, xo, b);
        buf_nfa_eps(nfa, yo, b);
        *pin = a; *pout = b;
        return;
    }
    case BUF_RX_STAR: {
        int a = buf_nfa_state(nfa);
        int b, xi, xo;
        if (nfa->has_error) return;
        buf_nfa_frag(nfa, rx, nd->a, &xi, &xo);
        if (nfa->has_error) return;
        b = buf_nfa_state(nfa);
        if (nfa->has_error) return;
        buf_nfa_eps(nfa, a, xi);
        buf_nfa_eps(nfa, a, b);
        buf_nfa_eps(nfa, xo, xi);
        buf_nfa_eps(nfa, xo, b);
        *pin = a; *pout = b;
        return;
    }
    case BUF_RX_PLUS: {
        int b, xi, xo;
        buf_nfa_frag(nfa, rx, nd->a, &xi, &xo);
        if (nfa->has_error) return;
        b = buf_nfa_state(nfa);
        if (nfa->has_error) return;
        buf_nfa_eps(nfa, xo, xi);
        buf_nfa_eps(nfa, xo, b);
        *pin = xi; *pout = b;
        return;
    }
    case BUF_RX_OPT: {
        int a = buf_nfa_state(nfa);
        int b, xi, xo;
        if (nfa->has_error) return;
        buf_nfa_frag(nfa, rx, nd->a, &xi, &xo);
        if (nfa->has_error) return;
        b = buf_nfa_state(nfa);
        if (nfa->has_error) return;
        buf_nfa_eps(nfa, a, xi);
        buf_nfa_eps(nfa, a, b);
        buf_nfa_eps(nfa, xo, b);
        *pin = a; *pout = b;
        return;
    }
    }
    buf_nfa_err(nfa, nd->line, nd->col, "internal: unknown regex node kind");
}

/* --- public entry ------------------------------------------------- */

static void buf_nfa_init(BufNfa *nfa, BufRx *rx) {
    nfa->state_count = 0;
    nfa->start       = 0;
    nfa->rule_count  = rx->rule_count;
    nfa->error[0]    = '\0';
    nfa->has_error   = 0;
    nfa->spec_path   = rx->spec_path ? rx->spec_path : "<spec>";
}

/* Build the epsilon-NFA for every rule in `rx`. Returns 0 / -1. */
int buf_nfa_build(BufNfa *nfa, BufRx *rx) {
    int cur, r;

    buf_nfa_init(nfa, rx);
    if (rx->has_error) {
        buf_nfa_err(nfa, 1, 1, "spec has unresolved errors");
        return -1;
    }

    cur = buf_nfa_state(nfa);        /* state 0: the start */
    if (nfa->has_error) return -1;

    for (r = 0; r < rx->rule_count; r++) {
        BufRule *ru = &rx->rules[r];
        int      fin, fout, acc;

        buf_nfa_frag(nfa, rx, ru->root, &fin, &fout);
        if (nfa->has_error) return -1;

        acc = buf_nfa_state(nfa);
        if (nfa->has_error) return -1;
        nfa->states[acc].accept_rule = r;
        buf_nfa_eps(nfa, fout, acc);

        /* splice this rule onto the start spine */
        buf_nfa_eps(nfa, cur, fin);
        if (r < rx->rule_count - 1) {
            int link = buf_nfa_state(nfa);
            if (nfa->has_error) return -1;
            buf_nfa_eps(nfa, cur, link);
            cur = link;
        }
    }
    return nfa->has_error ? -1 : 0;
}
