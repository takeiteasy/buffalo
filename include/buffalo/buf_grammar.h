/*
 * buf_grammar.h -- buffalo LALR(1) parser table construction, over the
 * %grammar section read by buf_rx.h.
 *
 * Pure C, header-only, fixed arenas, no malloc. Compiled twice: plain `cc`
 * for the host unit tests (tests/t_grammar.c), and (once a follow-on ticket
 * wires it in) inside cccc's comptime VM via `#include @comptime`. Sticky
 * first-wins error string, never MacroErrorAt (see buf_rx.h).
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

#include <string.h>

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

/* --- diagnostics (sticky, first-wins) ---------------------------------- */

static void buf_lalr_err(BufGrammar *g, const char *msg) {
    if (g->has_error) return;
    snprintf(g->error, sizeof(g->error), "%s: %s", g->spec_path, msg);
    g->has_error = 1;
}
static void buf_lalr_err_at(BufGrammar *g, int line, int col, const char *msg) {
    if (g->has_error) return;
    snprintf(g->error, sizeof(g->error), "%s:%d:%d: %s",
             g->spec_path, line, col, msg);
    g->has_error = 1;
}

/* --- production accessors (special-case the synthetic S' -> start) ----- */

static int buf_lalr_plhs(BufRx *rx, int p) {
    return (p == rx->prod_count) ? rx->nonterm_count : rx->prods[p].lhs;
}
static int buf_lalr_plen(BufRx *rx, int p) {
    return (p == rx->prod_count) ? 1 : rx->prods[p].rhs_len;
}
static BufSym buf_lalr_psym(BufRx *rx, int p, int k) {
    if (p == rx->prod_count) {
        BufSym s;
        s.is_terminal = 0;
        s.index = rx->start_nt;
        s.line  = rx->start_line;
        s.col   = rx->start_col;
        return s;
    }
    return rx->syms[rx->prods[p].rhs_start + k];
}
static int buf_lalr_pline(BufRx *rx, int p) {
    return (p == rx->prod_count) ? rx->start_line : rx->prods[p].line;
}
static int buf_lalr_pcol(BufRx *rx, int p) {
    return (p == rx->prod_count) ? rx->start_col : rx->prods[p].col;
}

/* --- lookahead bitset helpers (BUF_LALR_LA_BYTES bytes) ---------------- */

static void buf_lalr_bits_zero(unsigned char *b) {
    int i;
    for (i = 0; i < BUF_LALR_LA_BYTES; i++) b[i] = 0;
}
static void buf_lalr_bits_set(unsigned char *b, int bit) {
    b[bit >> 3] = (unsigned char)(b[bit >> 3] | (1u << (bit & 7)));
}
static void buf_lalr_bits_clear(unsigned char *b, int bit) {
    b[bit >> 3] = (unsigned char)(b[bit >> 3] & ~(1u << (bit & 7)));
}
static int buf_lalr_bits_get(const unsigned char *b, int bit) {
    return (b[bit >> 3] >> (bit & 7)) & 1;
}
static int buf_lalr_bits_any(const unsigned char *b) {
    int i;
    for (i = 0; i < BUF_LALR_LA_BYTES; i++) if (b[i]) return 1;
    return 0;
}
/* dst |= src; returns 1 if dst changed. */
static int buf_lalr_bits_or_changed(unsigned char *dst, const unsigned char *src) {
    int i, changed = 0;
    for (i = 0; i < BUF_LALR_LA_BYTES; i++) {
        unsigned char n = (unsigned char)(dst[i] | src[i]);
        if (n != dst[i]) changed = 1;
        dst[i] = n;
    }
    return changed;
}

/* --- item id table: (prod, dot) -> a small dense integer ---------------- */

static void buf_lalr_init_items(BufGrammar *g, BufRx *rx) {
    int p, d, running = 0, id;

    for (p = 0; p < rx->prod_count; p++) {
        int L = rx->prods[p].rhs_len;
        g->item_base[p] = running;
        for (d = 0; d <= L; d++) {
            id = running + d;
            if (id >= BUF_LALR_MAX_ITEMDEFS) {
                buf_lalr_err(g, "too many grammar items for the arena");
                return;
            }
            g->item_prod[id] = p;
            g->item_dot[id]  = d;
        }
        running += L + 1;
    }
    g->item_base[rx->prod_count] = running;
    for (d = 0; d <= 1; d++) {
        id = running + d;
        if (id >= BUF_LALR_MAX_ITEMDEFS) {
            buf_lalr_err(g, "too many grammar items for the arena");
            return;
        }
        g->item_prod[id] = rx->prod_count;
        g->item_dot[id]  = d;
    }
    running += 2;
    g->nitemdefs = running;
}

static void buf_lalr_build_lhs_index(BufGrammar *g, BufRx *rx) {
    int count[BUF_RX_MAX_NONTERMS];
    int cursor[BUF_RX_MAX_NONTERMS];
    int i, p, acc = 0;

    for (i = 0; i < rx->nonterm_count; i++) count[i] = 0;
    for (p = 0; p < rx->prod_count; p++) count[rx->prods[p].lhs]++;
    for (i = 0; i < rx->nonterm_count; i++) {
        g->prod_by_lhs_off[i] = acc;
        cursor[i] = acc;
        acc += count[i];
    }
    g->prod_by_lhs_off[rx->nonterm_count] = acc;
    for (p = 0; p < rx->prod_count; p++) {
        int lhs = rx->prods[p].lhs;
        g->prod_by_lhs_list[cursor[lhs]++] = p;
    }
}

/* --- nullable / FIRST fixpoint, over nonterms[] ------------------------- */

static void buf_lalr_nullable_first(BufGrammar *g, BufRx *rx) {
    int i, p, changed;

    for (i = 0; i < rx->nonterm_count; i++) {
        g->nullable[i] = 0;
        buf_rx_bits_zero(g->first_set[i]);
    }
    do {
        changed = 0;
        for (p = 0; p < rx->prod_count; p++) {
            int lhs = rx->prods[p].lhs;
            int L   = rx->prods[p].rhs_len;
            int k, all_nullable = 1;
            for (k = 0; k < L; k++) {
                BufSym s = rx->syms[rx->prods[p].rhs_start + k];
                if (s.is_terminal) {
                    if (!buf_rx_bits_get(g->first_set[lhs], s.index)) {
                        buf_rx_bits_set(g->first_set[lhs], s.index);
                        changed = 1;
                    }
                    all_nullable = 0;
                    break;
                }
                {
                    unsigned char before[32];
                    memcpy(before, g->first_set[lhs], 32);
                    buf_rx_bits_or(g->first_set[lhs], g->first_set[s.index]);
                    if (memcmp(before, g->first_set[lhs], 32) != 0) changed = 1;
                }
                if (!g->nullable[s.index]) { all_nullable = 0; break; }
            }
            if (all_nullable && !g->nullable[lhs]) {
                g->nullable[lhs] = 1;
                changed = 1;
            }
        }
    } while (changed);
}

/* --- validation: every nonterminal must be productive and reachable --- */

static int buf_lalr_check_productive(BufGrammar *g, BufRx *rx) {
    int i, p, changed;

    for (i = 0; i < rx->nonterm_count; i++) g->productive[i] = 0;
    do {
        changed = 0;
        for (p = 0; p < rx->prod_count; p++) {
            int lhs = rx->prods[p].lhs;
            int L   = rx->prods[p].rhs_len;
            int k, ok = 1;
            if (g->productive[lhs]) continue;
            for (k = 0; k < L; k++) {
                BufSym s = rx->syms[rx->prods[p].rhs_start + k];
                if (!s.is_terminal && !g->productive[s.index]) { ok = 0; break; }
            }
            if (ok) { g->productive[lhs] = 1; changed = 1; }
        }
    } while (changed);

    for (i = 0; i < rx->nonterm_count; i++) {
        if (!g->productive[i]) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "nonterminal '%s' is non-productive (derives no finite "
                     "string)", rx->nonterms[i].name);
            buf_lalr_err_at(g, rx->nonterms[i].line, rx->nonterms[i].col, msg);
            return -1;
        }
    }
    return 0;
}

static int buf_lalr_check_reachable(BufGrammar *g, BufRx *rx) {
    int wl[BUF_RX_MAX_NONTERMS];
    int wl_n = 0, i;

    for (i = 0; i < rx->nonterm_count; i++) g->reachable[i] = 0;
    g->reachable[rx->start_nt] = 1;
    wl[wl_n++] = rx->start_nt;
    while (wl_n > 0) {
        int nt = wl[--wl_n];
        int off = g->prod_by_lhs_off[nt], end = g->prod_by_lhs_off[nt + 1];
        int qi;
        for (qi = off; qi < end; qi++) {
            int p = g->prod_by_lhs_list[qi];
            int L = rx->prods[p].rhs_len, k;
            for (k = 0; k < L; k++) {
                BufSym s = rx->syms[rx->prods[p].rhs_start + k];
                if (!s.is_terminal && !g->reachable[s.index]) {
                    g->reachable[s.index] = 1;
                    wl[wl_n++] = s.index;
                }
            }
        }
    }
    for (i = 0; i < rx->nonterm_count; i++) {
        if (!g->reachable[i]) {
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "nonterminal '%s' is unreachable from %%start '%s'",
                     rx->nonterms[i].name, rx->nonterms[rx->start_nt].name);
            buf_lalr_err_at(g, rx->nonterms[i].line, rx->nonterms[i].col, msg);
            return -1;
        }
    }
    return 0;
}

/* --- LR(0) closure ------------------------------------------------------
 *
 * Mirrors buf_dfa_closure: a generation-stamped visited set (no O(N) clear
 * per call) and an ascending-sorted result written straight into the shared
 * pool, so the sorted run is the state's canonical identity key. `seed`
 * must not alias g->scratch.
 */
static int buf_lalr_closure0(BufGrammar *g, BufRx *rx, const int *seed, int nseed) {
    int i, wl = 0;
    unsigned gen;

    if (++g->mark_gen == 0) {
        for (i = 0; i < g->nitemdefs; i++) g->mark[i] = 0;
        g->mark_gen = 1;
    }
    gen = g->mark_gen;

    for (i = 0; i < nseed; i++) {
        int it = seed[i];
        if (g->mark[it] != gen) { g->mark[it] = gen; g->scratch[wl++] = it; }
    }
    for (i = 0; i < wl; i++) {
        int itemid = g->scratch[i];
        int p = g->item_prod[itemid], d = g->item_dot[itemid];
        BufSym s;
        int off, end, qi;
        if (d >= buf_lalr_plen(rx, p)) continue;
        s = buf_lalr_psym(rx, p, d);
        if (s.is_terminal) continue;
        off = g->prod_by_lhs_off[s.index];
        end = g->prod_by_lhs_off[s.index + 1];
        for (qi = off; qi < end; qi++) {
            int target = g->item_base[g->prod_by_lhs_list[qi]];
            if (g->mark[target] != gen) {
                g->mark[target] = gen;
                if (wl >= BUF_LALR_MAX_ITEMDEFS) {
                    buf_lalr_err(g, "LR(0) closure worklist exhausted");
                    return -1;
                }
                g->scratch[wl++] = target;
            }
        }
    }

    if (g->pool_used + wl > BUF_LALR_MAX_ITEMS) {
        buf_lalr_err(g, "LALR item-set pool exhausted");
        return -1;
    }
    /* insertion-sort ascending into the pool (wl is small) */
    for (i = 0; i < wl; i++) {
        int v = g->scratch[i], j = g->pool_used + i;
        while (j > g->pool_used && g->item_pool[j - 1] > v) {
            g->item_pool[j] = g->item_pool[j - 1];
            j--;
        }
        g->item_pool[j] = v;
    }
    return wl;
}

static unsigned buf_lalr_hash(const int *run, int len) {
    unsigned h = 2166136261u;
    int j;
    for (j = 0; j < len; j++) { h ^= (unsigned)run[j]; h *= 16777619u; }
    h ^= (unsigned)len * 2654435761u;
    return h & (unsigned)(BUF_LALR_HASH - 1);
}

static int buf_lalr_find(BufGrammar *g, int off, int len) {
    unsigned b = buf_lalr_hash(&g->item_pool[off], len);
    int i, j;
    for (i = g->hash_head[b]; i >= 0; i = g->hash_next[i]) {
        if (g->set_len[i] != len) continue;
        for (j = 0; j < len; j++)
            if (g->item_pool[g->set_off[i] + j] != g->item_pool[off + j]) break;
        if (j == len) return i;
    }
    return -1;
}

static void buf_lalr_hash_insert(BufGrammar *g, int id) {
    unsigned b = buf_lalr_hash(&g->item_pool[g->set_off[id]], g->set_len[id]);
    g->hash_next[id] = g->hash_head[b];
    g->hash_head[b]  = id;
}

/* Binary search state `state`'s sorted item run for `itemid`. The caller
 * guarantees presence. */
static int buf_lalr_find_item(BufGrammar *g, int state, int itemid) {
    int lo = g->set_off[state], hi = lo + g->set_len[state] - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int v = g->item_pool[mid];
        if (v == itemid) return mid;
        if (v < itemid) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* --- LR(0) automaton construction ---------------------------------------
 *
 * Discovers states in worklist order (determinism), and fills the shift
 * half of `action` and the whole of `goto_tab` directly -- both are pure
 * functions of the LR(0) automaton, independent of lookahead.
 */
static int buf_lalr_build_lr0(BufGrammar *g, BufRx *rx) {
    int seed0, len, si;

    seed0 = g->item_base[rx->prod_count]; /* S' -> . start */
    len = buf_lalr_closure0(g, rx, &seed0, 1);
    if (len < 0) return -1;
    g->set_off[0] = g->pool_used;
    g->set_len[0] = len;
    g->pool_used += len;
    g->nstates = 1;
    buf_lalr_hash_insert(g, 0);

    for (si = 0; si < g->nstates; si++) { /* nstates grows in the loop */
        int axis, naxis = rx->token_count + rx->nonterm_count;
        for (axis = 0; axis < naxis; axis++) {
            int is_term = axis < rx->token_count;
            int sidx = is_term ? axis : axis - rx->token_count;
            int nseed = 0, j, id;

            for (j = 0; j < g->set_len[si]; j++) {
                int itemid = g->item_pool[g->set_off[si] + j];
                int p = g->item_prod[itemid], d = g->item_dot[itemid];
                BufSym s;
                if (d >= buf_lalr_plen(rx, p)) continue;
                s = buf_lalr_psym(rx, p, d);
                if (s.is_terminal != is_term || s.index != sidx) continue;
                if (nseed >= BUF_LALR_MAX_ITEMDEFS) {
                    buf_lalr_err(g, "LALR GOTO seed buffer exhausted");
                    return -1;
                }
                g->seedbuf[nseed++] = g->item_base[p] + d + 1;
            }
            if (nseed == 0) continue;

            len = buf_lalr_closure0(g, rx, g->seedbuf, nseed);
            if (len < 0) return -1;
            id = buf_lalr_find(g, g->pool_used, len);
            if (id < 0) {
                if (g->nstates >= BUF_LALR_MAX_STATES) {
                    buf_lalr_err(g, "grammar produced too many LALR states");
                    return -1;
                }
                id = g->nstates++;
                g->set_off[id] = g->pool_used;
                g->set_len[id] = len;
                g->pool_used  += len;
                buf_lalr_hash_insert(g, id);
            }
            if (is_term)
                g->action[si * g->ntok + (sidx + BUF_LALR_FIRST_USER_TOK)] =
                    BUF_LALR_ACT_SHIFT(id);
            else
                g->goto_tab[si * g->nnonterm + sidx] = id;
        }
    }
    g->start_state = 0;
    return 0;
}

/* --- LALR(1) lookahead computation ---------------------------------------
 *
 * Algorithm 4.62 (Aho/Sethi/Ullman/Lam): for each kernel item k of each
 * state s, close {[k, #]} with a dummy terminal '#' standing in for "the
 * lookahead k already carries". An item reached with a real terminal in its
 * lookahead is a spontaneous contribution to the kernel item on the far
 * side of the transition it is about to make; one reached still carrying
 * '#' means that transition's target item merely inherits LA(s, k)
 * unchanged -- a propagation edge, resolved to a fixpoint afterward.
 */
static void buf_lalr_closure1(BufGrammar *g, BufRx *rx, int state, int kidx) {
    int k_itemid = g->item_pool[kidx];
    int touched_n = 0, wl_n = 0;

    buf_lalr_bits_set(g->acc[k_itemid], BUF_LALR_DUMMY_BIT);
    g->touched[touched_n++] = k_itemid;
    g->wl[wl_n++] = k_itemid;

    while (wl_n > 0) {
        int itemid = g->wl[--wl_n];
        int p = g->item_prod[itemid], d = g->item_dot[itemid];
        int L = buf_lalr_plen(rx, p);
        BufSym s;
        unsigned char newbits[BUF_LALR_LA_BYTES];
        unsigned char firstbeta[32];
        int beta_nullable = 1, kk, off, end, qi;

        if (d >= L) continue;
        s = buf_lalr_psym(rx, p, d);
        if (s.is_terminal) continue;

        buf_rx_bits_zero(firstbeta);
        for (kk = d + 1; kk < L; kk++) {
            BufSym s2 = buf_lalr_psym(rx, p, kk);
            if (s2.is_terminal) {
                buf_rx_bits_set(firstbeta, s2.index);
                beta_nullable = 0;
                break;
            }
            buf_rx_bits_or(firstbeta, g->first_set[s2.index]);
            if (!g->nullable[s2.index]) { beta_nullable = 0; break; }
        }
        buf_lalr_bits_zero(newbits);
        {
            int b;
            for (b = 0; b < rx->token_count; b++)
                if (buf_rx_bits_get(firstbeta, b)) buf_lalr_bits_set(newbits, b);
        }
        if (beta_nullable) buf_lalr_bits_or_changed(newbits, g->acc[itemid]);

        off = g->prod_by_lhs_off[s.index];
        end = g->prod_by_lhs_off[s.index + 1];
        for (qi = off; qi < end; qi++) {
            int target = g->item_base[g->prod_by_lhs_list[qi]];
            int was_empty = !buf_lalr_bits_any(g->acc[target]);
            int changed = buf_lalr_bits_or_changed(g->acc[target], newbits);
            if (was_empty) g->touched[touched_n++] = target;
            if (changed) g->wl[wl_n++] = target;
        }
    }

    {
        int ti;
        for (ti = 0; ti < touched_n; ti++) {
            int t = g->touched[ti];
            int p = g->item_prod[t], d = g->item_dot[t];
            int L = buf_lalr_plen(rx, p);
            BufSym s;
            int s2, idx2;
            unsigned char spon[BUF_LALR_LA_BYTES];

            if (d >= L) {
                /* A completed item produced purely by closure (e.g. an
                 * epsilon production's reduce item) has no successor to
                 * shift/goto into -- its lookahead belongs on ITS OWN
                 * la_pool slot in the current state, not a successor's. */
                idx2 = buf_lalr_find_item(g, state, t);

                memcpy(spon, g->acc[t], BUF_LALR_LA_BYTES);
                buf_lalr_bits_clear(spon, BUF_LALR_DUMMY_BIT);
                if (buf_lalr_bits_any(spon))
                    buf_lalr_bits_or_changed(g->la_pool[idx2], spon);

                if (buf_lalr_bits_get(g->acc[t], BUF_LALR_DUMMY_BIT) &&
                    g->nedges < BUF_LALR_MAX_EDGES) {
                    g->edge_from[g->nedges] = kidx;
                    g->edge_to[g->nedges]   = idx2;
                    g->nedges++;
                }
                continue;
            }
            s = buf_lalr_psym(rx, p, d);
            if (s.is_terminal)
                s2 = BUF_LALR_SHIFT_STATE(
                    g->action[state * g->ntok + (s.index + BUF_LALR_FIRST_USER_TOK)]);
            else
                s2 = g->goto_tab[state * g->nnonterm + s.index];

            idx2 = buf_lalr_find_item(g, s2, g->item_base[p] + d + 1);

            memcpy(spon, g->acc[t], BUF_LALR_LA_BYTES);
            buf_lalr_bits_clear(spon, BUF_LALR_DUMMY_BIT);
            if (buf_lalr_bits_any(spon))
                buf_lalr_bits_or_changed(g->la_pool[idx2], spon);

            if (buf_lalr_bits_get(g->acc[t], BUF_LALR_DUMMY_BIT) &&
                g->nedges < BUF_LALR_MAX_EDGES) {
                g->edge_from[g->nedges] = kidx;
                g->edge_to[g->nedges]   = idx2;
                g->nedges++;
            }
        }
        for (ti = 0; ti < touched_n; ti++)
            buf_lalr_bits_zero(g->acc[g->touched[ti]]);
    }
}

static int buf_lalr_compute_lookaheads(BufGrammar *g, BufRx *rx) {
    int s, j, idx0;

    for (s = 0; s < g->nstates; s++) {
        for (j = 0; j < g->set_len[s]; j++) {
            int idx = g->set_off[s] + j;
            int itemid = g->item_pool[idx];
            int is_kernel = (g->item_dot[itemid] > 0) ||
                            (s == 0 && itemid == g->item_base[rx->prod_count]);
            if (!is_kernel) continue;
            if (g->nedges >= BUF_LALR_MAX_EDGES - g->nitemdefs) {
                buf_lalr_err(g, "LALR propagation-edge arena exhausted");
                return -1;
            }
            buf_lalr_closure1(g, rx, s, idx);
        }
    }

    idx0 = buf_lalr_find_item(g, 0, g->item_base[rx->prod_count]);
    buf_lalr_bits_set(g->la_pool[idx0], BUF_LALR_EOF_BIT);

    {
        int changed;
        do {
            changed = 0;
            for (j = 0; j < g->nedges; j++)
                if (buf_lalr_bits_or_changed(g->la_pool[g->edge_to[j]],
                                             g->la_pool[g->edge_from[j]]))
                    changed = 1;
        } while (changed);
    }
    return g->has_error ? -1 : 0;
}

/* --- fill the reduce/accept half of `action`, detecting conflicts ------- */

static int buf_lalr_conflict_shift_reduce(BufGrammar *g, BufRx *rx, int state,
                                          const char *tokname, int prod) {
    char msg[220];
    snprintf(msg, sizeof(msg),
             "shift/reduce conflict on '%s' in state %d: reduce '%s' vs. shift",
             tokname, state, rx->nonterms[buf_lalr_plhs(rx, prod)].name);
    buf_lalr_err_at(g, buf_lalr_pline(rx, prod), buf_lalr_pcol(rx, prod), msg);
    return -1;
}
static int buf_lalr_conflict_reduce_reduce(BufGrammar *g, BufRx *rx, int state,
                                           const char *tokname, int pa, int pb) {
    char msg[220];
    snprintf(msg, sizeof(msg),
             "reduce/reduce conflict in state %d between '%s' (%d:%d) and "
             "'%s' (%d:%d) on lookahead '%s'",
             state, rx->nonterms[buf_lalr_plhs(rx, pa)].name,
             buf_lalr_pline(rx, pa), buf_lalr_pcol(rx, pa),
             rx->nonterms[buf_lalr_plhs(rx, pb)].name,
             buf_lalr_pline(rx, pb), buf_lalr_pcol(rx, pb), tokname);
    buf_lalr_err_at(g, buf_lalr_pline(rx, pa), buf_lalr_pcol(rx, pa), msg);
    return -1;
}

static int buf_lalr_fill_reduces(BufGrammar *g, BufRx *rx) {
    int s, j;

    for (s = 0; s < g->nstates; s++) {
        for (j = 0; j < g->set_len[s]; j++) {
            int idx = g->set_off[s] + j;
            int itemid = g->item_pool[idx];
            int p = g->item_prod[itemid], d = g->item_dot[itemid];
            int L = buf_lalr_plen(rx, p);
            if (d < L) continue; /* not a reduce/accept item */

            if (p == rx->prod_count) {
                if (buf_lalr_bits_get(g->la_pool[idx], BUF_LALR_EOF_BIT)) {
                    int *cell = &g->action[s * g->ntok + 0];
                    if (*cell == BUF_LALR_ACT_ERROR) *cell = BUF_LALR_ACT_ACCEPT;
                    else if (*cell != BUF_LALR_ACT_ACCEPT)
                        return buf_lalr_conflict_shift_reduce(g, rx, s, "EOF", p);
                }
                continue;
            }

            {
                int b;
                for (b = 0; b < rx->token_count; b++) {
                    int *cell;
                    int newv;
                    if (!buf_lalr_bits_get(g->la_pool[idx], b)) continue;
                    cell = &g->action[s * g->ntok + (b + BUF_LALR_FIRST_USER_TOK)];
                    newv = BUF_LALR_ACT_REDUCE(p);
                    if (*cell == BUF_LALR_ACT_ERROR) { *cell = newv; continue; }
                    if (*cell == newv) continue;
                    if (BUF_LALR_IS_SHIFT(*cell))
                        return buf_lalr_conflict_shift_reduce(g, rx, s,
                                                              rx->tokens[b], p);
                    return buf_lalr_conflict_reduce_reduce(
                        g, rx, s, rx->tokens[b], p, BUF_LALR_REDUCE_PROD(*cell));
                }
                if (buf_lalr_bits_get(g->la_pool[idx], BUF_LALR_EOF_BIT)) {
                    int *cell = &g->action[s * g->ntok + 0];
                    int newv = BUF_LALR_ACT_REDUCE(p);
                    if (*cell == BUF_LALR_ACT_ERROR) { *cell = newv; }
                    else if (*cell == BUF_LALR_ACT_ACCEPT)
                        return buf_lalr_conflict_shift_reduce(g, rx, s, "EOF", p);
                    else if (*cell != newv) {
                        if (BUF_LALR_IS_SHIFT(*cell))
                            return buf_lalr_conflict_shift_reduce(g, rx, s, "EOF", p);
                        return buf_lalr_conflict_reduce_reduce(
                            g, rx, s, "EOF", p, BUF_LALR_REDUCE_PROD(*cell));
                    }
                }
            }
        }
    }
    return 0;
}

/* --- public entry point -------------------------------------------------- */

static void buf_lalr_init(BufGrammar *g, BufRx *rx) {
    int i;
    g->pool_used = 0;
    g->nstates   = 0;
    g->nedges    = 0;
    g->mark_gen  = 0;
    for (i = 0; i < BUF_LALR_MAX_ITEMDEFS; i++) g->mark[i] = 0;
    for (i = 0; i < BUF_LALR_HASH; i++) g->hash_head[i] = -1;
    for (i = 0; i < BUF_LALR_MAX_ITEMDEFS; i++) buf_lalr_bits_zero(g->acc[i]);
    for (i = 0; i < BUF_LALR_MAX_ITEMS; i++) buf_lalr_bits_zero(g->la_pool[i]);
    for (i = 0; i < BUF_LALR_MAX_STATES * (BUF_RX_MAX_TOKENS + BUF_LALR_FIRST_USER_TOK); i++)
        g->action[i] = BUF_LALR_ACT_ERROR;
    for (i = 0; i < BUF_LALR_MAX_STATES * BUF_RX_MAX_NONTERMS; i++)
        g->goto_tab[i] = -1;
    g->error[0]  = '\0';
    g->has_error = 0;
    g->spec_path = rx->spec_path ? rx->spec_path : "<spec>";
}

/* Build LALR(1) parser tables from a validated grammar section. Returns
 * 0 / -1 (g->error set on failure). Requires rx to already be error-free
 * with a %grammar section (buf_rx_parse_string/buf_rx_read_file, checked
 * via rx->has_error / rx->has_grammar). */
static int buf_grammar_build(BufGrammar *g, BufRx *rx) {
    buf_lalr_init(g, rx);
    if (rx->has_error) {
        buf_lalr_err(g, "spec has unresolved errors");
        return -1;
    }
    if (!rx->has_grammar) {
        buf_lalr_err(g, "spec has no %grammar section");
        return -1;
    }

    g->aug_start_prod = rx->prod_count;
    g->ntok           = rx->token_count + BUF_LALR_FIRST_USER_TOK;
    g->nnonterm       = rx->nonterm_count;

    buf_lalr_init_items(g, rx);
    if (g->has_error) return -1;
    buf_lalr_build_lhs_index(g, rx);
    buf_lalr_nullable_first(g, rx);

    if (buf_lalr_check_productive(g, rx) < 0) return -1;
    if (buf_lalr_check_reachable(g, rx) < 0) return -1;

    if (buf_lalr_build_lr0(g, rx) < 0) return -1;
    if (buf_lalr_compute_lookaheads(g, rx) < 0) return -1;
    if (buf_lalr_fill_reduces(g, rx) < 0) return -1;

    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* BUF_GRAMMAR_H */
