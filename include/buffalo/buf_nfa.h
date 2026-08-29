/*
 * buf_nfa.h -- buffalo Thompson construction: regex AST -> epsilon-NFA.
 *
 * Pure C, header-only, fixed arenas, no malloc. Compiled twice: plain `cc`
 * for the host unit tests (tests/t_nfa.c), and inside cccc's comptime VM via
 * `#include @comptime` from src/buf_comptime.c. Like buf_rx.h it never calls
 * MacroErrorAt (it must also build under a plain `cc`); errors accumulate
 * first-wins in a sticky string, and src/buf_comptime.c -- the boundary --
 * lifts that into a comptime diagnostic.
 *
 * Input: a parsed BufRx (buf_rx.h) -- a regex AST per rule, plus the ordered
 * %tokens list. Output: one epsilon-NFA covering every rule.
 *
 * Shape. Each NFA state has at most one labelled (byte-set) edge and up to
 * two epsilon edges -- enough for every Thompson fragment below. A CLASS leaf
 * carries the reader's 256-bit byte set verbatim as its edge guard. Each
 * rule's fragment ends in an accept state tagged with that rule's index (file
 * order, %skip included); on a length tie the DFA keeps the lowest such index,
 * so earlier rules win. All rule fragments hang off state 0 (the start) down a
 * chain of epsilon links -- a plain two-epsilon spine, since a rule count of
 * more than two rules out fanning them all out of one state.
 *
 * A regex sub-expression may still be nullable even though buf_rx.h rejects a
 * nullable *rule* -- e.g. `(a?)+b`, `(a*)*x`. Those build fine here but put an
 * epsilon cycle in the NFA, so every epsilon walk in this file and its
 * consumers (buf_dfa.h) is worklist + visited-set, never recursive.
 */
#ifndef BUF_NFA_H
#define BUF_NFA_H

#include <string.h>

#include "buf_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M3 measured examples/clike.l at 306 NFA states and the 103-rule
 * examples/big.l at 1077 -- 4% and 13% of this. NFA construction stays in the
 * noise; the comptime cost is all in buf_dfa_build. The cap stays generous
 * (a static array; work is bounded by state_count). See docs/performance.md. */
#define BUF_NFA_MAX_STATES 8192

typedef struct {
    unsigned char bits[32];   /* labelled-edge byte set; all-zero if no edge */
    int  target;              /* labelled-edge target state, -1 if none      */
    int  eps_a, eps_b;        /* up to two epsilon targets, -1 if unused     */
    int  accept_rule;         /* rule index accepted here, -1 otherwise      */
} BufNfaState;

typedef struct {
    BufNfaState states[BUF_NFA_MAX_STATES];
    int  state_count;
    int  start;               /* always 0                                   */
    int  rule_count;          /* mirrors BufRx.rule_count                    */

    char error[BUF_RX_ERR_MAX];
    int  has_error;
    const char *spec_path;    /* borrowed from BufRx for diagnostics        */
} BufNfa;

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
 * *pin, its (epsilon-free) exit state in *pout. Out-params, not a returned
 * struct: cccc's comptime VM mishandles a by-value struct return from a
 * recursive comptime function ("return buffer pool was not rehydrated"). */
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
static int buf_nfa_build(BufNfa *nfa, BufRx *rx) {
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

#ifdef __cplusplus
}
#endif

#endif /* BUF_NFA_H */
