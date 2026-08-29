/*
 * t_nfa.c -- host unit tests for include/buffalo/buf_nfa.h.
 *
 * Plain `cc`, no cccc. Builds the epsilon-NFA from a spec and exercises it
 * with a reference matcher that walks the NFA directly (worklist epsilon
 * closure, longest match, lowest rule index on a tie) -- an implementation
 * independent of the DFA path, so t_dfa.c cross-checks against the same
 * expectations.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_nfa.h"

/* TOK_EOF = 0, TOK_ERROR = 1 reserved; first %tokens entry is value 2
 * (BUF_TOK_FIRST_USER in runtime/buf_rt.h, BUF_DFA_FIRST_USER_TOK in
 * buf_dfa.h). */
#define FIRST_USER_TOK 2

static int failures = 0;
static int checks   = 0;

#define CHECK(cond, what)                                                     \
    do {                                                                     \
        checks++;                                                            \
        if (!(cond)) {                                                       \
            failures++;                                                      \
            printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, (what));          \
        }                                                                    \
    } while (0)

static BufRx  rx;
static BufNfa nfa;

/* --- reference NFA simulator ------------------------------------------ */

/* One reference token: kind is the rule's TOK_* value, or -2 for ERROR. */
typedef struct { int kind; int len; } RefTok;

#define REF_ERROR (-2)

static unsigned char cl_mark[BUF_NFA_MAX_STATES];
static int           cl_list[BUF_NFA_MAX_STATES];

/* epsilon-closure of `seed[0..n)` -> cl_list / cl_len (ascending). */
static int closure(const int *seed, int n, int *cl_len)
{
    int i, wl = 0, len = 0, s;
    for (i = 0; i < nfa.state_count; i++) cl_mark[i] = 0;
    for (i = 0; i < n; i++) {
        s = seed[i];
        if (s >= 0 && s < nfa.state_count && !cl_mark[s]) {
            cl_mark[s] = 1;
            cl_list[wl++] = s;
        }
    }
    for (i = 0; i < wl; i++) {
        BufNfaState *st = &nfa.states[cl_list[i]];
        int e;
        e = st->eps_a;
        if (e >= 0 && !cl_mark[e]) { cl_mark[e] = 1; cl_list[wl++] = e; }
        e = st->eps_b;
        if (e >= 0 && !cl_mark[e]) { cl_mark[e] = 1; cl_list[wl++] = e; }
    }
    for (s = 0, len = 0; s < nfa.state_count; s++)
        if (cl_mark[s]) cl_list[len++] = s;
    *cl_len = len;
    return len;
}

/* Lowest accept rule among cl_list[0..cl_len), or -1. */
static int accept_in_closure(int cl_len)
{
    int i, win = -1;
    for (i = 0; i < cl_len; i++) {
        int ar = nfa.states[cl_list[i]].accept_rule;
        if (ar >= 0 && (win < 0 || ar < win)) win = ar;
    }
    return win;
}

static int cur_set[BUF_NFA_MAX_STATES];
static int seed_set[BUF_NFA_MAX_STATES];

/* Tokenise `src` with the reference matcher. Writes up to `max` tokens into
 * out[], returns the count (the trailing EOF is not stored). Guards against a
 * non-advancing loop. */
static int ref_lex(const char *src, int len, RefTok *out, int max)
{
    int pos = 0, nout = 0, guard = 0;

    while (pos < len && nout < max) {
        int cl_len, i, scan;
        int last_rule = -1, last_pos = pos;

        if (++guard > 4 * len + 16) { printf("  ref_lex: no progress\n"); break; }

        /* start set */
        { int s0 = nfa.start; closure(&s0, 1, &cl_len); }
        { int ncur = cl_len;
          for (i = 0; i < ncur; i++) cur_set[i] = cl_list[i];
          if (accept_in_closure(ncur) >= 0) { last_rule = accept_in_closure(ncur); last_pos = pos; }

          scan = pos;
          while (scan < len) {
              unsigned char c = (unsigned char)src[scan];
              int nseed = 0, j, nlen;
              for (j = 0; j < ncur; j++) {
                  BufNfaState *st = &nfa.states[cur_set[j]];
                  if (st->target >= 0 && buf_rx_bits_get(st->bits, c))
                      seed_set[nseed++] = st->target;
              }
              if (nseed == 0) break;
              closure(seed_set, nseed, &nlen);
              ncur = nlen;
              for (j = 0; j < ncur; j++) cur_set[j] = cl_list[j];
              scan++;
              { int w = accept_in_closure(ncur);
                if (w >= 0) { last_rule = w; last_pos = scan; } }
          }
        }

        if (last_rule < 0) {
            out[nout].kind = REF_ERROR;
            out[nout].len  = 1;
            nout++;
            pos += 1;
            continue;
        }
        if (rx.rules[last_rule].is_skip) {
            pos = last_pos;
            continue;
        }
        out[nout].kind = rx.rules[last_rule].tok_index + FIRST_USER_TOK;
        out[nout].len  = last_pos - pos;
        nout++;
        pos = last_pos;
    }
    return nout;
}

/* --- tests --------------------------------------------------------- */

static void build(const char *spec)
{
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    CHECK(rc == 0, "spec parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "NFA builds");
    if (rc != 0) printf("  -> %s\n", nfa.error);
}

static void test_fragments(void)
{
    build("%tokens A\nA [0-9]+\n");
    /* [0-9]+ : one CLASS pair + PLUS exit + accept + start spine */
    CHECK(nfa.start == 0, "start state is 0");
    CHECK(nfa.state_count >= 4, "PLUS fragment allocates states");
    {
        int i, found = 0;
        for (i = 0; i < nfa.state_count; i++)
            if (nfa.states[i].accept_rule == 0) found = 1;
        CHECK(found, "an accept state carries rule 0");
    }
}

static void test_calc_stream(void)
{
    RefTok t[32];
    int n;
    int rc;

    rc = buf_rx_read_file(&rx, "examples/calc.l");
    CHECK(rc == 0, "calc.l parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "calc.l NFA builds");
    if (rc != 0) { printf("  -> %s\n", nfa.error); return; }

    /* calc.l %tokens: INT FLOAT IDENT PLUS STAR LPAREN RPAREN
     * -> TOK values 2..8 */
    n = ref_lex("1 + 2.5 * foo", (int)strlen("1 + 2.5 * foo"), t, 32);
    CHECK(n == 5, "1 + 2.5 * foo -> 5 tokens");
    if (n == 5) {
        CHECK(t[0].kind == 2 && t[0].len == 1, "INT '1'");
        CHECK(t[1].kind == 5 && t[1].len == 1, "PLUS '+'");
        CHECK(t[2].kind == 3 && t[2].len == 3, "FLOAT '2.5' (longest match)");
        CHECK(t[3].kind == 6 && t[3].len == 1, "STAR '*'");
        CHECK(t[4].kind == 4 && t[4].len == 3, "IDENT 'foo'");
    }

    n = ref_lex("(12)", 4, t, 32);
    CHECK(n == 3, "(12) -> 3 tokens");
    if (n == 3) {
        CHECK(t[0].kind == 7, "LPAREN");
        CHECK(t[1].kind == 2 && t[1].len == 2, "INT '12'");
        CHECK(t[2].kind == 8, "RPAREN");
    }

    n = ref_lex("1 # comment\n2", (int)strlen("1 # comment\n2"), t, 32);
    CHECK(n == 2, "'#' comment is skipped");
    if (n == 2) {
        CHECK(t[0].kind == 2 && t[0].len == 1, "INT '1' before comment");
        CHECK(t[1].kind == 2 && t[1].len == 1, "INT '2' after comment newline");
    }

    n = ref_lex("@", 1, t, 32);
    CHECK(n == 1 && t[0].kind == REF_ERROR, "stray '@' -> ERROR");
}

/* nullable sub-expressions survive buf_rx.h (only whole-rule nullability is
 * rejected), so the NFA has epsilon cycles here -- the closure must not spin. */
static void test_epsilon_cycles(void)
{
    RefTok t[16];
    int n;

    build("%tokens A\nA (a?)+b\n");
    n = ref_lex("aaab", 4, t, 16);
    CHECK(n == 1 && t[0].kind == 2 && t[0].len == 4, "(a?)+b matches 'aaab'");
    n = ref_lex("b", 1, t, 16);
    CHECK(n == 1 && t[0].kind == 2 && t[0].len == 1, "(a?)+b matches 'b'");

    build("%tokens A\nA (a*)*x\n");
    n = ref_lex("aax", 3, t, 16);
    CHECK(n == 1 && t[0].kind == 2 && t[0].len == 3, "(a*)*x matches 'aax'");
    n = ref_lex("x", 1, t, 16);
    CHECK(n == 1 && t[0].kind == 2 && t[0].len == 1, "(a*)*x matches 'x'");
}

static void test_tie_lowest_rule(void)
{
    RefTok t[8];
    int n;

    /* both rules match "if" with length 2; the earlier rule must win */
    build("%tokens KW ID\nKW \"if\"\nID [a-z]+\n");
    n = ref_lex("if", 2, t, 8);
    CHECK(n == 1 && t[0].kind == 2, "length tie resolves to the earlier rule (KW)");
    n = ref_lex("iff", 3, t, 8);
    CHECK(n == 1 && t[0].kind == 3 && t[0].len == 3, "'iff' is the longer ID match");
}

static void test_clike_builds(void)
{
    int rc = buf_rx_read_file(&rx, "examples/clike.l");
    CHECK(rc == 0, "clike.l parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "clike.l NFA builds within the arena");
    if (rc != 0) printf("  -> %s\n", nfa.error);
}

int main(void)
{
    test_fragments();
    test_calc_stream();
    test_epsilon_cycles();
    test_tie_lowest_rule();
    test_clike_builds();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
