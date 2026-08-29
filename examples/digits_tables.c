/*
 * digits_tables.c -- HAND-WRITTEN stand-in for a buffalo-generated table file.
 *
 * M0 has no regex reader, no NFA/DFA construction and no comptime emitter
 * yet. This file is written by hand to the exact shape M4's emitter must
 * produce: four `static const` tables plus a one-line `buf_next` wrapper
 * that forwards to the generic `buf_run` driver in runtime/buf_rt.c.
 * M4 replaces this file with `buffalo lex digits.l -o digits.l.gen.c`.
 *
 * It encodes the spec that digits.l will carry:
 *
 *     %tokens INT
 *     INT    [0-9]+
 *     %skip  [ \t\r\n]+
 *
 * DFA (3 states, 3 equivalence classes):
 *
 *   classes   0 = other (246 bytes)   1 = [0-9]   2 = [ \t\r\n]
 *   states    0 = START (non-accepting)
 *             1 = IN_INT  (accepts rule 0 -> TOK_INT)
 *             2 = IN_SKIP (accepts rule 1 -> %skip)
 */
#include "buf_rt.h"
#include "digits_tokens.h"

/* byte -> equivalence class; total over 0..255, values in 0..NCLASS-1.
 * Zero-initialised entries are class 0 ("other"). */
static const unsigned char buf_dfa_class[256] = {
    ['0'] = 1, ['1'] = 1, ['2'] = 1, ['3'] = 1, ['4'] = 1,
    ['5'] = 1, ['6'] = 1, ['7'] = 1, ['8'] = 1, ['9'] = 1,
    [' '] = 2, ['\t'] = 2, ['\r'] = 2, ['\n'] = 2,
};

#define NSTATES 3
#define NCLASS  3
#define START   0

/* flat [NSTATES * NCLASS]; -1 == dead state. Row = state, column = class. */
static const short buf_dfa_next[NSTATES * NCLASS] = {
    /* state 0 START  */ -1,  1,  2,
    /* state 1 IN_INT */ -1,  1, -1,
    /* state 2 IN_SKIP*/ -1, -1,  2,
};

/* state -> winning rule index, or -1 if non-accepting. */
static const short buf_dfa_accept[NSTATES] = { -1, 0, 1 };

/* rule index -> TOK_* kind, or -1 for a %skip rule. */
static const short buf_rule_token[2] = { TOK_INT, -1 };

BufToken buf_next(BufLexer *lx)
{
    return buf_run(lx, buf_dfa_class, buf_dfa_next, buf_dfa_accept,
                   buf_rule_token, NSTATES, NCLASS, START);
}
