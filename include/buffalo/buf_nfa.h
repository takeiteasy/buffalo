/*
 * buf_nfa.h -- buffalo Thompson construction: regex AST -> epsilon-NFA.
 *
 * Pure C, .h/.c pair, fixed arenas, no malloc. Compiled twice: by `cc` for
 * the host unit tests (tests/t_nfa.c), and inside cccc's comptime VM --
 * src/buf_comptime.c does `#include @comptime "buf_nfa.c"`. Like buf_rx it
 * never calls MacroErrorAt (it must also build under a plain `cc`); errors
 * accumulate first-wins in a sticky string, and src/buf_comptime.c -- the
 * boundary -- lifts that into a comptime diagnostic.
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

#include "buf_rx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* M3 measured examples/clike.bflo at 306 NFA states and the 103-rule
 * examples/big.bflo at 1077 -- 4% and 13% of this. NFA construction stays in the
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

/* --- entry point (defined in src/buf_nfa.c) --------------------- */

/* Build the epsilon-NFA for every rule in `rx`. Returns 0 / -1
 * (nfa->error set on failure). */
int buf_nfa_build(BufNfa *nfa, BufRx *rx);

#ifdef __cplusplus
}
#endif

#endif /* BUF_NFA_H */
