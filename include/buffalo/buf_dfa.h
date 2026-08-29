/*
 * buf_dfa.h -- buffalo subset construction: epsilon-NFA -> DFA, over the
 * alphabet's equivalence classes rather than raw bytes.
 *
 * Pure C, header-only, fixed arenas, no malloc. Compiled twice: plain `cc`
 * for the host unit tests (tests/t_dfa.c), and inside cccc's comptime VM via
 * `#include @comptime` from src/buf_comptime.c. Sticky first-wins error
 * string, never MacroErrorAt (see buf_rx.h).
 *
 * Input: a built BufNfa (buf_nfa.h) plus its BufRx (for the %tokens mapping
 * and diagnostics). Output: the four tables runtime/buf_rt.c's buf_run driver
 * consumes --
 *
 *   cls[256]        byte -> equivalence class, total, values 0..nclass-1
 *   next[nstates*nclass]   flat, row-major; -1 == dead state
 *   accept[nstates] winning rule index per state, -1 if non-accepting
 *   rule_token[nrules]     rule index -> TOK_* value, -1 for a %skip rule
 *
 * Alphabet classes. buf_rx.h already stores every character class as a
 * 256-bit byte set, so partitioning is a refinement pass: start with all 256
 * bytes in one class, then for each CLASS leaf split any class that the
 * leaf's set cuts. A real lexer collapses to 20-40 classes, shrinking `next`
 * 6-12x. Classes are renumbered by first appearance over bytes 0..255 so the
 * numbering is deterministic and independent of iteration or hash order --
 * the two cccc build paths must stay byte-identical.
 *
 * DFA state sets. Each DFA state is a sorted run of NFA state indices held in
 * one shared pool (set_off/set_len index into it); the pool is capped
 * independently of the DFA state count so M3 can tune the two knobs
 * separately. Runs are collected by sweeping NFA ids 0..state_count, so they
 * are ascending by construction and double as the canonical identity key.
 * State ids are handed out in worklist discovery order only.
 *
 * Ties: accept[s] is the lowest rule index among the NFA accept states in
 * s's set, so earlier rules (%skip included) win a length tie -- matching the
 * ROADMAP and buf_rt.h's contract.
 *
 * Invariant: accept[start] must be < 0. buf_rt.h relies on this with no
 * runtime guard -- a start state that accepts means some rule matches the
 * empty string, which spins buf_run on a %skip. It follows from buf_rx.h
 * rejecting nullable rules, but a mis-wired fragment would break it silently,
 * so it is asserted here and reported with the offending rule's line:col.
 */
#ifndef BUF_DFA_H
#define BUF_DFA_H

#include <string.h>

#include "buf_rx.h"
#include "buf_nfa.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BUF_DFA_MAX_STATES  4096            /* ROADMAP M3 arena table       */
#define BUF_DFA_MAX_CLASSES 256             /* worst case; typically 20-40  */
#define BUF_DFA_MAX_TRANS   (BUF_DFA_MAX_STATES * 64)  /* next[]; M3 tunes  */
#define BUF_DFA_SET_POOL    65536           /* shared NFA-index pool; M3    */

/* Value of the first %tokens entry. Must equal BUF_TOK_FIRST_USER in
 * runtime/buf_rt.h (TOK_EOF = 0, TOK_ERROR = 1 are reserved); kept as a
 * local constant so this comptime header needs no runtime include.
 * tests/t_dfa.c static-asserts the two agree. */
#define BUF_DFA_FIRST_USER_TOK 2

typedef struct {
    unsigned char cls[256];
    int    nclass;
    int    rep[BUF_DFA_MAX_CLASSES];        /* a representative byte per class */

    short  next[BUF_DFA_MAX_TRANS];
    short  accept[BUF_DFA_MAX_STATES];
    short  rule_token[BUF_RX_MAX_RULES];
    int    nstates, nrules, start;

    int    pool[BUF_DFA_SET_POOL];          /* sorted NFA-index runs        */
    int    pool_used;
    int    set_off[BUF_DFA_MAX_STATES];
    int    set_len[BUF_DFA_MAX_STATES];

    int    seedbuf[BUF_NFA_MAX_STATES];     /* move-step target scratch     */
    int    scratch[BUF_NFA_MAX_STATES];     /* closure worklist             */
    unsigned char mark[BUF_NFA_MAX_STATES]; /* closure visited set          */

    char error[BUF_RX_ERR_MAX];
    int  has_error;
    const char *spec_path;
} BufDfa;

/* --- diagnostics (sticky, first-wins) -------------------------------- */

static void buf_dfa_err(BufDfa *dfa, const char *msg) {
    if (dfa->has_error) return;
    snprintf(dfa->error, sizeof(dfa->error), "%s: %s", dfa->spec_path, msg);
    dfa->has_error = 1;
}
static void buf_dfa_err_at(BufDfa *dfa, int line, int col, const char *msg) {
    if (dfa->has_error) return;
    snprintf(dfa->error, sizeof(dfa->error), "%s:%d:%d: %s",
             dfa->spec_path, line, col, msg);
    dfa->has_error = 1;
}

/* --- alphabet equivalence classes ---------------------------------- */

static void buf_dfa_partition(BufDfa *dfa, BufRx *rx) {
    int c, n, g, ncur, next_id;
    unsigned char remap_seen[BUF_DFA_MAX_CLASSES];
    int           remap[BUF_DFA_MAX_CLASSES];

    for (c = 0; c < 256; c++) dfa->cls[c] = 0;
    dfa->nclass = 1;

    for (n = 1; n < rx->node_count; n++) {   /* node 0 is the sentinel */
        const unsigned char *B;
        if (rx->nodes[n].kind != BUF_RX_CLASS) continue;
        B = rx->nodes[n].bits;
        ncur = dfa->nclass;                  /* freeze: new splits are all in-B */
        for (g = 0; g < ncur; g++) {
            int seen_in = 0, seen_out = 0, ng;
            for (c = 0; c < 256; c++) {
                if (dfa->cls[c] != g) continue;
                if (buf_rx_bits_get(B, c)) seen_in = 1;
                else                       seen_out = 1;
            }
            if (!seen_in || !seen_out) continue;
            if (dfa->nclass >= BUF_DFA_MAX_CLASSES) {
                buf_dfa_err(dfa, "alphabet partition exceeded 256 classes");
                return;
            }
            ng = dfa->nclass++;
            for (c = 0; c < 256; c++)
                if (dfa->cls[c] == g && buf_rx_bits_get(B, c))
                    dfa->cls[c] = (unsigned char)ng;
        }
    }

    /* renumber by first appearance over bytes 0..255 */
    for (g = 0; g < BUF_DFA_MAX_CLASSES; g++) remap_seen[g] = 0;
    next_id = 0;
    for (c = 0; c < 256; c++) {
        int old = dfa->cls[c];
        if (!remap_seen[old]) { remap_seen[old] = 1; remap[old] = next_id++; }
    }
    for (c = 0; c < 256; c++)
        dfa->cls[c] = (unsigned char)remap[dfa->cls[c]];
    dfa->nclass = next_id;

    for (g = 0; g < dfa->nclass; g++) dfa->rep[g] = -1;
    for (c = 0; c < 256; c++)
        if (dfa->rep[dfa->cls[c]] < 0) dfa->rep[dfa->cls[c]] = c;
}

/* --- epsilon-closure into the pool -------------------------------- */

/* Closure of `seed[0..nseed)` written (uncommitted) at pool[pool_used..].
 * Returns the ascending run length, or -1 if the pool is exhausted. The
 * seed array must not alias dfa->scratch. */
static int buf_dfa_closure(BufDfa *dfa, BufNfa *nfa,
                           const int *seed, int nseed) {
    int i, s, wl = 0, len = 0;

    for (i = 0; i < nfa->state_count; i++) dfa->mark[i] = 0;
    for (i = 0; i < nseed; i++) {
        s = seed[i];
        if (s >= 0 && s < nfa->state_count && !dfa->mark[s]) {
            dfa->mark[s] = 1;
            dfa->scratch[wl++] = s;
        }
    }
    for (i = 0; i < wl; i++) {
        BufNfaState *st = &nfa->states[dfa->scratch[i]];
        int e;
        e = st->eps_a;
        if (e >= 0 && !dfa->mark[e]) { dfa->mark[e] = 1; dfa->scratch[wl++] = e; }
        e = st->eps_b;
        if (e >= 0 && !dfa->mark[e]) { dfa->mark[e] = 1; dfa->scratch[wl++] = e; }
    }
    for (s = 0; s < nfa->state_count; s++) {
        if (!dfa->mark[s]) continue;
        if (dfa->pool_used + len >= BUF_DFA_SET_POOL) {
            buf_dfa_err(dfa, "DFA state-set pool exhausted");
            return -1;
        }
        dfa->pool[dfa->pool_used + len] = s;
        len++;
    }
    return len;
}

/* Existing DFA state whose set equals the uncommitted run at pool[off..off+len),
 * or -1. Linear scan -- fine at M2/M3 sizes; a hash index is a Phase 1.5
 * improvement (see tracker). */
static int buf_dfa_find(BufDfa *dfa, int off, int len) {
    int i, j;
    for (i = 0; i < dfa->nstates; i++) {
        if (dfa->set_len[i] != len) continue;
        for (j = 0; j < len; j++)
            if (dfa->pool[dfa->set_off[i] + j] != dfa->pool[off + j]) break;
        if (j == len) return i;
    }
    return -1;
}

/* --- subset construction ---------------------------------------- */

static void buf_dfa_init(BufDfa *dfa, BufRx *rx) {
    dfa->nclass    = 0;
    dfa->nstates   = 0;
    dfa->nrules    = 0;
    dfa->start     = 0;
    dfa->pool_used = 0;
    dfa->error[0]  = '\0';
    dfa->has_error = 0;
    dfa->spec_path = rx->spec_path ? rx->spec_path : "<spec>";
}

/* Build the DFA tables from a constructed NFA. Returns 0 / -1. */
static int buf_dfa_build(BufDfa *dfa, BufNfa *nfa, BufRx *rx) {
    int r, si, k, j, seed0, len;

    buf_dfa_init(dfa, rx);
    if (rx->has_error || nfa->has_error) {
        buf_dfa_err(dfa, "NFA or spec has unresolved errors");
        return -1;
    }

    buf_dfa_partition(dfa, rx);
    if (dfa->has_error) return -1;

    dfa->nrules = rx->rule_count;
    for (r = 0; r < rx->rule_count; r++)
        dfa->rule_token[r] = rx->rules[r].is_skip
            ? (short)-1
            : (short)(rx->rules[r].tok_index + BUF_DFA_FIRST_USER_TOK);

    /* start state = closure({nfa->start}) */
    seed0 = nfa->start;
    len = buf_dfa_closure(dfa, nfa, &seed0, 1);
    if (len < 0) return -1;
    dfa->set_off[0] = dfa->pool_used;
    dfa->set_len[0] = len;
    dfa->pool_used += len;
    dfa->nstates    = 1;

    for (si = 0; si < dfa->nstates; si++) {   /* nstates grows in the loop */
        int win = -1;
        for (j = 0; j < dfa->set_len[si]; j++) {
            int ar = nfa->states[dfa->pool[dfa->set_off[si] + j]].accept_rule;
            if (ar >= 0 && (win < 0 || ar < win)) win = ar;
        }
        dfa->accept[si] = (short)win;

        if ((long)(si + 1) * dfa->nclass > BUF_DFA_MAX_TRANS) {
            buf_dfa_err(dfa, "DFA transition table exceeded its arena");
            return -1;
        }

        for (k = 0; k < dfa->nclass; k++) {
            int rep = dfa->rep[k], nseed = 0, id;
            for (j = 0; j < dfa->set_len[si]; j++) {
                BufNfaState *st =
                    &nfa->states[dfa->pool[dfa->set_off[si] + j]];
                if (st->target >= 0 && buf_rx_bits_get(st->bits, rep))
                    dfa->seedbuf[nseed++] = st->target;
            }
            if (nseed == 0) {
                dfa->next[si * dfa->nclass + k] = -1;
                continue;
            }
            len = buf_dfa_closure(dfa, nfa, dfa->seedbuf, nseed);
            if (len < 0) return -1;
            id = buf_dfa_find(dfa, dfa->pool_used, len);
            if (id < 0) {
                if (dfa->nstates >= BUF_DFA_MAX_STATES) {
                    buf_dfa_err(dfa, "spec produced too many DFA states");
                    return -1;
                }
                id = dfa->nstates++;
                dfa->set_off[id] = dfa->pool_used;
                dfa->set_len[id] = len;
                dfa->pool_used  += len;      /* commit the run */
                if ((long)dfa->nstates * dfa->nclass > BUF_DFA_MAX_TRANS) {
                    buf_dfa_err(dfa, "DFA transition table exceeded its arena");
                    return -1;
                }
            }
            dfa->next[si * dfa->nclass + k] = (short)id;
        }
    }

    dfa->start = 0;
    if (dfa->accept[0] >= 0) {
        int rr = dfa->accept[0];
        buf_dfa_err_at(dfa, rx->rules[rr].line, rx->rules[rr].col,
                       "rule matches the empty string "
                       "(DFA start state accepts)");
        return -1;
    }
    return dfa->has_error ? -1 : 0;
}

#ifdef __cplusplus
}
#endif

#endif /* BUF_DFA_H */
