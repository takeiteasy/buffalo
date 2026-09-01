/*
 * buf_dfa.h -- buffalo subset construction: epsilon-NFA -> DFA, over the
 * alphabet's equivalence classes rather than raw bytes.
 *
 * Pure C, .h/.c pair, fixed arenas, no malloc. Compiled twice: by `cc` for
 * the host unit tests (tests/t_dfa.c), and inside cccc's comptime VM --
 * src/buf_comptime.c does `#include @comptime "buf_dfa.c"`. Sticky first-wins
 * error string, never MacroErrorAt (see buf_rx.h).
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
 * independently of the DFA state count. A closure is insertion-sorted
 * ascending as it lands in the pool, so the run is the canonical identity key,
 * and an FNV-1a index over that run (hash_head/hash_next) resolves an existing
 * state in ~O(1) rather than an O(nstates) linear scan. State ids are handed
 * out in worklist discovery order only. See docs/performance.md for why this
 * matters (comptime-VM cost).
 *
 * Minimisation (opt-in). With -D BUF_MINIMIZE, buf_dfa_build runs a Moore
 * partition-refinement pass (buf_dfa_minimize) over the finished subset
 * construction: initial blocks keyed on the exact accept[] value (so "earlier
 * rule wins" survives a merge), a dead transition (-1) compared as its own
 * signature entry, and deterministic renumbering by each block's least old
 * state id -- which keeps start == 0 and lets next[]/accept[] compact in
 * place with no shadow arena. It is OFF by default: for real lexer specs it
 * only trims ~5-8% of states (subset construction already lands near the
 * minimal DFA) and it does NOT cut subset-construction cost -- it adds its
 * own pass to the comptime hot path, a net loss on large specs. See
 * docs/performance.md for the measured trade. Once it has run the pool /
 * set_off / set_len / hash_* fields describe the pre-minimisation states and
 * are stale -- nothing downstream reads them, only the four emitted tables +
 * nstates / nclass / start.
 *
 * Ties: accept[s] is the lowest rule index among the NFA accept states in
 * s's set, so earlier rules (%skip included) win a length tie -- matching
 * buf_rt.h's contract.
 *
 * Invariant: accept[start] must be < 0. buf_rt.h relies on this with no
 * runtime guard -- a start state that accepts means some rule matches the
 * empty string, which spins buf_run on a %skip. It follows from buf_rx.h
 * rejecting nullable rules, but a mis-wired fragment would break it silently,
 * so it is asserted here and reported with the offending rule's line:col.
 */
#ifndef BUF_DFA_H
#define BUF_DFA_H

#include "buf_rx.h"
#include "buf_nfa.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M3 spike (docs/performance.md): examples/clike.bflo -> 85 DFA states, 43
 * classes; the 103-rule examples/big.bflo -> 324 states, 76 classes, 24624
 * transitions, 2157 pooled indices. Every DFA arena is 6-140x inside its cap,
 * and the comptime wall is VM interpretation of the loops below (native
 * big.bflo: 1.8 ms; comptime: ~2.25 s), not arena size -- so the caps stay
 * generous. The find/closure rework in this file (hash index, generation-
 * stamped mark) already landed at M3; buf_dfa_minimize (Moore, below) is an
 * opt-in -D BUF_MINIMIZE pass that trims the state count a few percent but
 * does not touch this cost. */
#define BUF_DFA_MAX_STATES  4096
#define BUF_DFA_MAX_CLASSES 256             /* worst case; typically 20-40  */
#define BUF_DFA_MAX_TRANS   (BUF_DFA_MAX_STATES * 64)  /* == MAX_STATES*64  */
#define BUF_DFA_SET_POOL    65536           /* shared NFA-index pool        */
#define BUF_DFA_HASH        8192            /* state-set index; power of two */

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

    /* state-set -> DFA id index: FNV-1a of the sorted run, chained. Replaces
     * an O(nstates) linear scan per subset step -- see docs/performance.md. */
    int    hash_head[BUF_DFA_HASH];
    int    hash_next[BUF_DFA_MAX_STATES];

    int    seedbuf[BUF_NFA_MAX_STATES];     /* move-step target scratch     */
    int    scratch[BUF_NFA_MAX_STATES];     /* closure worklist             */
    unsigned mark[BUF_NFA_MAX_STATES];      /* closure visited: mark==mark_gen */
    unsigned mark_gen;                      /* bumped per closure, not cleared */

    /* --- Moore minimisation scratch (buf_dfa_minimize) ----------------- */
    int    mblk[BUF_DFA_MAX_STATES];        /* current block id per state   */
    int    mblk_prev[BUF_DFA_MAX_STATES];   /* per-pass block-id snapshot   */
    int    morder[BUF_DFA_MAX_STATES];      /* states bucket-sorted by block */
    int    mblk_start[BUF_DFA_MAX_STATES];  /* per-block base in morder      */
    int    msub_rep[BUF_DFA_MAX_STATES];    /* sub-block representative state */
    int    msub_id[BUF_DFA_MAX_STATES];     /* sub-block index -> block id   */
    int    mnewid[BUF_DFA_MAX_STATES];      /* old state id -> minimised id  */
    int    nstates_premin;                  /* nstates before minimisation  */

    char error[BUF_RX_ERR_MAX];
    int  has_error;
    const char *spec_path;

#ifdef BUF_DFA_STATS
    /* M3 spike instrumentation (tracker). Off by default. closure-calls still
     * scales with nstates*nclass; find-compares is now chain length in the
     * hash index, not a full nstates scan. */
    long stat_closure_calls;
    long stat_find_compares;
#endif
} BufDfa;

/* --- entry points (defined in src/buf_dfa.c) ------------------- */

/* Build the DFA tables from a constructed NFA. buf_dfa_build_ex's
 * `minimize` runs the opt-in Moore pass before returning; buf_dfa_build
 * turns it on only under -D BUF_MINIMIZE (a comptime body sees the -D,
 * not a source #define). Host tests call buf_dfa_build_ex directly.
 * Returns 0 / -1 (dfa->error set on failure). */
int buf_dfa_build_ex(BufDfa *dfa, BufNfa *nfa, BufRx *rx, int minimize);
int buf_dfa_build(BufDfa *dfa, BufNfa *nfa, BufRx *rx);

#ifdef __cplusplus
}
#endif

#endif /* BUF_DFA_H */
