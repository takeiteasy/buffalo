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

#include <string.h>

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
 * seed array must not alias dfa->scratch.
 *
 * `mark` is a generation-stamped visited set: a cell counts as marked when it
 * equals `mark_gen`, which is bumped once per call instead of zeroing the
 * whole array (M3: that clear was O(N_nfa) on every one of ~nstates*nclass
 * calls). The ascending run is produced by insertion-sorting the closure
 * worklist -- which is a handful of states -- rather than sweeping all
 * N_nfa ids. See docs/performance.md. */
static int buf_dfa_closure(BufDfa *dfa, BufNfa *nfa,
                           const int *seed, int nseed) {
    int      i, s, wl = 0;
    unsigned g;

#ifdef BUF_DFA_STATS
    dfa->stat_closure_calls++;
#endif
    if (++dfa->mark_gen == 0) {                 /* generation counter wrapped */
        for (i = 0; i < BUF_NFA_MAX_STATES; i++) dfa->mark[i] = 0;
        dfa->mark_gen = 1;
    }
    g = dfa->mark_gen;

    for (i = 0; i < nseed; i++) {
        s = seed[i];
        if (s >= 0 && s < nfa->state_count && dfa->mark[s] != g) {
            dfa->mark[s] = g;
            dfa->scratch[wl++] = s;
        }
    }
    for (i = 0; i < wl; i++) {
        BufNfaState *st = &nfa->states[dfa->scratch[i]];
        int e;
        e = st->eps_a;
        if (e >= 0 && dfa->mark[e] != g) { dfa->mark[e] = g; dfa->scratch[wl++] = e; }
        e = st->eps_b;
        if (e >= 0 && dfa->mark[e] != g) { dfa->mark[e] = g; dfa->scratch[wl++] = e; }
    }

    if (dfa->pool_used + wl > BUF_DFA_SET_POOL) {
        buf_dfa_err(dfa, "DFA state-set pool exhausted");
        return -1;
    }
    /* scratch[0..wl) is discovery order; insertion-sort ascending into the
     * pool so the run is the canonical identity key. wl is small. */
    for (i = 0; i < wl; i++) {
        int v = dfa->scratch[i], j = dfa->pool_used + i;
        while (j > dfa->pool_used && dfa->pool[j - 1] > v) {
            dfa->pool[j] = dfa->pool[j - 1];
            j--;
        }
        dfa->pool[j] = v;
    }
    return wl;
}

/* FNV-1a of a sorted NFA-index run, folded to a BUF_DFA_HASH bucket. */
static unsigned buf_dfa_hash(const int *run, int len) {
    unsigned h = 2166136261u;
    int j;
    for (j = 0; j < len; j++) { h ^= (unsigned)run[j]; h *= 16777619u; }
    h ^= (unsigned)len * 2654435761u;
    return h & (unsigned)(BUF_DFA_HASH - 1);
}

/* Existing DFA state whose set equals the uncommitted run at
 * pool[off..off+len), or -1. Hash index, chained on collision. */
static int buf_dfa_find(BufDfa *dfa, int off, int len) {
    unsigned b = buf_dfa_hash(&dfa->pool[off], len);
    int i, j;
    for (i = dfa->hash_head[b]; i >= 0; i = dfa->hash_next[i]) {
#ifdef BUF_DFA_STATS
        dfa->stat_find_compares++;
#endif
        if (dfa->set_len[i] != len) continue;
        for (j = 0; j < len; j++)
            if (dfa->pool[dfa->set_off[i] + j] != dfa->pool[off + j]) break;
        if (j == len) return i;
    }
    return -1;
}

/* Register a committed DFA state (set_off/set_len already filled) in the index. */
static void buf_dfa_hash_insert(BufDfa *dfa, int id) {
    unsigned b = buf_dfa_hash(&dfa->pool[dfa->set_off[id]], dfa->set_len[id]);
    dfa->hash_next[id] = dfa->hash_head[b];
    dfa->hash_head[b]  = id;
}

/* --- subset construction ---------------------------------------- */

static void buf_dfa_init(BufDfa *dfa, BufRx *rx) {
    int i;
    dfa->nclass    = 0;
    dfa->nstates   = 0;
    dfa->nstates_premin = 0;
    dfa->nrules    = 0;
    dfa->start     = 0;
    dfa->pool_used = 0;
    dfa->mark_gen  = 0;
    /* one clear per build so a reused BufDfa cannot carry a stale generation
     * stamp into the first closure (mark[s]==mark_gen is the visited test). */
    for (i = 0; i < BUF_NFA_MAX_STATES; i++) dfa->mark[i] = 0;
    for (i = 0; i < BUF_DFA_HASH; i++) dfa->hash_head[i] = -1;
    dfa->error[0]  = '\0';
    dfa->has_error = 0;
    dfa->spec_path = rx->spec_path ? rx->spec_path : "<spec>";
#ifdef BUF_DFA_STATS
    dfa->stat_closure_calls = 0;
    dfa->stat_find_compares = 0;
#endif
}

/* --- Moore minimisation ------------------------------------------------
 *
 * Coarsest partition refinement over the finished DFA. Cost is
 * O(passes * nstates * nclass) with `passes` a handful (blocks are tiny once
 * the accept[] split is in). All scratch lives in BufDfa -- no malloc, no
 * recursion. Rewrites next[] / accept[] / nstates / start in place.
 *
 * Signature of a state = the tuple of its transition targets' block ids, with
 * a dead edge (-1) carried as the sentinel -1. Two states in one block split
 * apart when their signatures differ; the pass reads a frozen snapshot
 * (mblk_prev) so splits within a pass do not perturb each other.
 *
 * Renumber: new ids in ascending order of each block's least old state id.
 * Deterministic (needed for the two cccc build paths to stay byte-identical),
 * puts state 0's block at id 0 so `start` stays 0, and makes the old-id of
 * new state m always >= m -- so next[]/accept[] compact in place, low to
 * high, with no shadow arena. */
static void buf_dfa_minimize(BufDfa *dfa) {
    int n  = dfa->nstates;
    int nc = dfa->nclass;
    int s, k, b, m, nblk, pass_nblk, nnew;

    if (n <= 1) return;

    /* initial partition: one block per distinct accept[] value, ids handed
     * out in first-appearance order over state ids (deterministic). */
    {
        int acc2blk[BUF_RX_MAX_RULES + 1];   /* accept[] is -1 .. nrules-1 */
        int a;
        for (a = 0; a <= dfa->nrules; a++) acc2blk[a] = -1;
        nblk = 0;
        for (s = 0; s < n; s++) {
            a = dfa->accept[s] + 1;
            if (acc2blk[a] < 0) acc2blk[a] = nblk++;
            dfa->mblk[s] = acc2blk[a];
        }
    }

    /* refine until the block count stops growing */
    for (;;) {
        pass_nblk = nblk;
        for (s = 0; s < n; s++) dfa->mblk_prev[s] = dfa->mblk[s];

        /* bucket-sort states into morder[] by block; within a bucket the
         * order is ascending state id (s scanned low..high below). */
        for (b = 0; b < nblk; b++) dfa->mblk_start[b] = 0;
        for (s = 0; s < n; s++) dfa->mblk_start[dfa->mblk_prev[s]]++;
        {
            int acc = 0, t;
            for (b = 0; b < nblk; b++) {
                t = dfa->mblk_start[b];
                dfa->mblk_start[b] = acc;
                acc += t;
            }
        }
        for (b = 0; b < nblk; b++) dfa->mnewid[b] = dfa->mblk_start[b]; /* cursors */
        for (s = 0; s < n; s++)
            dfa->morder[dfa->mnewid[dfa->mblk_prev[s]]++] = s;

        nnew = nblk;
        for (b = 0; b < nblk; b++) {
            int lo = dfa->mblk_start[b];
            int hi = (b + 1 < nblk) ? dfa->mblk_start[b + 1] : n;
            int nsub = 0;
            for (m = lo; m < hi; m++) {
                int st = dfa->morder[m], j, found = -1;
                for (j = 0; j < nsub; j++) {
                    int rp = dfa->msub_rep[j], eq = 1;
                    for (k = 0; k < nc; k++) {
                        int ts = dfa->next[st * nc + k];
                        int tr = dfa->next[rp * nc + k];
                        int bs = (ts < 0) ? -1 : dfa->mblk_prev[ts];
                        int br = (tr < 0) ? -1 : dfa->mblk_prev[tr];
                        if (bs != br) { eq = 0; break; }
                    }
                    if (eq) { found = j; break; }
                }
                if (found >= 0) {
                    dfa->mblk[st] = dfa->msub_id[found];
                } else {
                    int nb = (nsub == 0) ? b : nnew++;
                    dfa->msub_rep[nsub] = st;
                    dfa->msub_id[nsub]  = nb;
                    nsub++;
                    dfa->mblk[st] = nb;
                }
            }
        }
        nblk = nnew;
        if (nblk == pass_nblk) break;
    }

    /* renumber + in-place compaction */
    {
        int *blk_newid = dfa->mblk_start;   /* reuse: block id -> new state id */
        int *rep_old   = dfa->morder;       /* reuse: new state id -> old id   */
        for (b = 0; b < nblk; b++) blk_newid[b] = -1;
        nnew = 0;
        for (s = 0; s < n; s++) {
            b = dfa->mblk[s];
            if (blk_newid[b] < 0) { blk_newid[b] = nnew; rep_old[nnew] = s; nnew++; }
        }
        for (s = 0; s < n; s++) dfa->mnewid[s] = blk_newid[dfa->mblk[s]];

        for (m = 0; m < nnew; m++) {
            int src = rep_old[m];           /* always >= m */
            for (k = 0; k < nc; k++) {
                int t = dfa->next[src * nc + k];
                dfa->next[m * nc + k] = (short)((t < 0) ? -1 : dfa->mnewid[t]);
            }
            dfa->accept[m] = dfa->accept[src];
        }
        dfa->nstates = nnew;
        dfa->start   = dfa->mnewid[0];      /* == 0 */
    }
}

/* Build the DFA tables from a constructed NFA. `minimize` runs the Moore pass
 * above before returning. Returns 0 / -1. */
static int buf_dfa_build_ex(BufDfa *dfa, BufNfa *nfa, BufRx *rx, int minimize) {
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
    buf_dfa_hash_insert(dfa, 0);

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
                buf_dfa_hash_insert(dfa, id);
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

    dfa->nstates_premin = dfa->nstates;
    if (minimize) {
        buf_dfa_minimize(dfa);
        if (dfa->accept[dfa->start] >= 0) {   /* defensive -- cannot happen */
            buf_dfa_err(dfa, "internal: minimised start state accepts");
            return -1;
        }
    }
    return dfa->has_error ? -1 : 0;
}

/* Default entry: minimisation is OFF unless -D BUF_MINIMIZE. It shrinks the
 * DFA only ~5-8% for real lexer specs (subset construction already lands near
 * the minimal DFA) while adding its own O(passes*nstates*nclass) pass to the
 * comptime hot path -- net negative on the large specs it was meant to help,
 * see docs/performance.md. Kept opt-in for the cases that want the smaller
 * table and can spend the compile time. Comptime bodies see only command-line
 * -D, not a source #define, so bench.sh / `buffalo lex -- -D BUF_MINIMIZE`
 * pass it; host tests call buf_dfa_build_ex directly. */
static int buf_dfa_build(BufDfa *dfa, BufNfa *nfa, BufRx *rx) {
#ifdef BUF_MINIMIZE
    return buf_dfa_build_ex(dfa, nfa, rx, 1);
#else
    return buf_dfa_build_ex(dfa, nfa, rx, 0);
#endif
}

#ifdef __cplusplus
}
#endif

#endif /* BUF_DFA_H */
