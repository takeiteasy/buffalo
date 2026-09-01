/*
 * buf_grammar.h -- buffalo LALR(1) parser table construction, over the
 * %grammar section read by buf_rx.h.
 *
 * Pure C, .h/.c pair, fixed arenas, no malloc. Compiled by `cc` for the host
 * unit tests (tests/t_grammar.c), and inside cccc's comptime VM via
 * `#include @comptime "buf_grammar.c"` from src/buf_comptime.c under
 * `buffalo parse` / -D BUF_EMIT_PARSER, the same way the other modules do.
 * Sticky first-wins error string, never MacroErrorAt (see buf_rx.h).
 *
 * Input: a BufRx with a validated %grammar section (has_grammar, has_start,
 * nonterms[]/prods[]/syms[]). Output --
 *
 *   action[state * ntok + tok]      shift/reduce/accept/error, packed int
 *   goto_tab[state * nnonterm + nt] next state on a nonterminal, -1 if none
 *   nstates, start_state
 *
 * `tok` is the *runtime* TOK_* value (BUF_TOK_EOF=0, BUF_TOK_ERROR=1 never
 * appear as a table column with a real entry; user tokens start at
 * BUF_LALR_FIRST_USER_TOK=2), matching buf_dfa.h's rule_token[] convention --
 * a consuming driver indexes this table directly off what buf_run hands it,
 * no remap layer.
 *
 * Action encoding (see BUF_LALR_ACT_* macros below):
 *   0            error / no entry
 *   >0           shift to (value-1)
 *   BUF_LALR_ACT_ACCEPT   accept (only ever on the EOF column)
 *   <0, != ACCEPT         reduce production -(value)-1 (an rx->prods[]
 *                         index; never the synthetic augmenting production)
 *
 * Algorithm: LR(0) automaton (closure/goto over grammar items, item-set
 * identity + hashed dedup exactly mirroring buf_dfa.h's DFA-state-set pool),
 * then LALR(1) lookaheads via the classic "spontaneous generation +
 * propagation to a fixpoint" method (Aho/Sethi/Ullman/Lam, 2nd ed., section
 * 4.7.5 / Algorithm 4.62) -- not canonical LR(1) state sets (3-6x more
 * states on a many-tier expression grammar, and the comptime VM's ~1000x
 * slowdown already made the DFA phase earn an optimisation pass) and not
 * full DeRemer-Pennello reads/includes/SCC lookahead computation (correct,
 * but the textbook's own "hardest to get right" method, and unneeded at
 * buffalo's grammar sizes).
 *
 * A synthetic augmenting production S' -> %start is used for the closure
 * over the whole automaton and for the accept action, but is never written
 * into `rx` (shared, treated as const input); every production lookup goes
 * through buf_lalr_plhs/buf_lalr_plen/buf_lalr_psym/buf_lalr_pline/
 * buf_lalr_pcol, which special-case index rx->prod_count as that production.
 *
 * Validation (beyond #21's "used but never defined"): every nonterminal must
 * be both productive (derives some finite terminal string) and reachable
 * from %start, checked here via two more fixpoints before automaton
 * construction runs at all.
 *
 * Conflicts (shift/reduce or reduce/reduce) are always a hard, first-wins
 * error naming the state, the conflicting lookahead, and the reducing
 * production's line:col -- buffalo's grammar format has no %left/%right to
 * resolve one, so a real conflict always means the grammar is ambiguous at
 * that point (see docs/bflo-format.md#grammar-section).
 *
 * LALR is not canonical LR(1): merging LR(0)-core-identical states can
 * introduce a reduce/reduce conflict that a canonical LR(1) automaton would
 * not have (a known, textbook LALR limitation -- not a buffalo bug).
 */
#ifndef BUF_GRAMMAR_H
#define BUF_GRAMMAR_H

#include "buf_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Kept in sync with BUF_DFA_FIRST_USER_TOK / BUF_TOK_FIRST_USER by hand (this
 * header does not include buf_dfa.h or buf_rt.h); tests/t_grammar.c
 * static-asserts all three agree. */
#define BUF_LALR_FIRST_USER_TOK 2

#define BUF_LALR_MAX_STATES    512   /* calc.bflo ~15-20; a 10-tier expression
                                      * grammar (foundation dogfood) still
                                      * well under 200 */
#define BUF_LALR_MAX_ITEMS     8192  /* shared item-set pool, cf. buf_dfa.h's
                                      * BUF_DFA_SET_POOL */
#define BUF_LALR_HASH          2048  /* power of two, item-set index */
#define BUF_LALR_MAX_ITEMDEFS  (BUF_RX_MAX_SYMS + BUF_RX_MAX_PRODS + 4)
#define BUF_LALR_MAX_EDGES     32768 /* lookahead propagation edges */

/* Lookahead bitset: bit b < BUF_RX_MAX_TOKENS is terminal b (rx->tokens[]
 * index); BUF_LALR_EOF_BIT is the end-of-input lookahead; BUF_LALR_DUMMY_BIT
 * is the transient "#" marker used only inside one closure1 run (see below)
 * to tell a propagated lookahead apart from a spontaneously generated one --
 * it never appears in a committed la_pool entry. Sized to the largest
 * possible index regardless of a given spec's actual token_count, so no
 * bitset here is ever runtime-sized. */
#define BUF_LALR_EOF_BIT       BUF_RX_MAX_TOKENS
#define BUF_LALR_DUMMY_BIT     (BUF_RX_MAX_TOKENS + 1)
#define BUF_LALR_LA_BYTES      ((BUF_RX_MAX_TOKENS + 2 + 7) / 8)

/* --- action table encoding -------------------------------------------- */

#define BUF_LALR_ACT_ERROR      0
#define BUF_LALR_ACT_ACCEPT     (-1000000)
#define BUF_LALR_ACT_SHIFT(s)   ((s) + 1)
#define BUF_LALR_ACT_REDUCE(p)  (-((p) + 1))
#define BUF_LALR_IS_SHIFT(v)    ((v) > 0)
#define BUF_LALR_IS_REDUCE(v)   ((v) < 0 && (v) != BUF_LALR_ACT_ACCEPT)
#define BUF_LALR_SHIFT_STATE(v) ((v) - 1)
#define BUF_LALR_REDUCE_PROD(v) (-(v) - 1)

typedef struct {
    int aug_start_prod;   /* == rx->prod_count: the synthetic S' -> start */

    /* --- compact item ids: (prod, dot) -> a small dense integer -------- */
    int item_base[BUF_RX_MAX_PRODS + 2];
    int item_prod[BUF_LALR_MAX_ITEMDEFS];
    int item_dot[BUF_LALR_MAX_ITEMDEFS];
    int nitemdefs;

    /* --- productions grouped by lhs, CSR style -------------------------- */
    int prod_by_lhs_off[BUF_RX_MAX_NONTERMS + 1];
    int prod_by_lhs_list[BUF_RX_MAX_PRODS];

    /* --- nullable / FIRST, over nonterms[] ------------------------------ */
    unsigned char nullable[BUF_RX_MAX_NONTERMS];
    unsigned char first_set[BUF_RX_MAX_NONTERMS][32]; /* bitset, terminal idx */

    /* --- grammar validation ---------------------------------------------*/
    unsigned char productive[BUF_RX_MAX_NONTERMS];
    unsigned char reachable[BUF_RX_MAX_NONTERMS];

    /* --- LR(0) automaton: item sets as sorted runs in a shared pool ---- */
    int item_pool[BUF_LALR_MAX_ITEMS];
    int pool_used;
    int set_off[BUF_LALR_MAX_STATES];
    int set_len[BUF_LALR_MAX_STATES];
    int hash_head[BUF_LALR_HASH];
    int hash_next[BUF_LALR_MAX_STATES];
    int nstates;
    int start_state;

    int scratch[BUF_LALR_MAX_ITEMDEFS];   /* closure0 worklist            */
    unsigned mark[BUF_LALR_MAX_ITEMDEFS]; /* closure0 visited: ==mark_gen */
    unsigned mark_gen;
    int seedbuf[BUF_LALR_MAX_ITEMDEFS];   /* per-symbol GOTO seed scratch */

    /* --- LALR(1) lookaheads --------------------------------------------- */
    unsigned char la_pool[BUF_LALR_MAX_ITEMS][BUF_LALR_LA_BYTES]; /* parallel
                                                                   * to item_pool */
    unsigned char acc[BUF_LALR_MAX_ITEMDEFS][BUF_LALR_LA_BYTES]; /* closure1
                                                                  * scratch */
    int touched[BUF_LALR_MAX_ITEMDEFS];
    int wl[BUF_LALR_MAX_ITEMDEFS];

    int edge_from[BUF_LALR_MAX_EDGES]; /* pool index -> pool index          */
    int edge_to[BUF_LALR_MAX_EDGES];
    int nedges;

    /* --- output tables --------------------------------------------------*/
    int ntok, nnonterm;
    int action[BUF_LALR_MAX_STATES * (BUF_RX_MAX_TOKENS + BUF_LALR_FIRST_USER_TOK)];
    int goto_tab[BUF_LALR_MAX_STATES * BUF_RX_MAX_NONTERMS];

    char error[BUF_RX_ERR_MAX];
    int  has_error;
    const char *spec_path;
} BufGrammar;

/* --- entry points (defined in src/buf_grammar.c) --------------- */

/* Build LALR(1) parser tables from a validated %grammar section.
 * Returns 0 / -1 (g->error set on failure). rx must be error-free with
 * a %grammar section already read. */
int buf_grammar_build(BufGrammar *g, BufRx *rx);

/* Production accessors that special-case the synthetic S' -> %start
 * (index rx->prod_count). Used by the driver-side table bake-out and
 * the grammar tests. */
int buf_lalr_plhs(BufRx *rx, int p);
int buf_lalr_plen(BufRx *rx, int p);

#ifdef __cplusplus
}
#endif

#endif /* BUF_GRAMMAR_H */
