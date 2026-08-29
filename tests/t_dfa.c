/*
 * t_dfa.c -- host unit tests for include/buffalo/buf_dfa.h.
 *
 * Plain `cc`, no cccc, but linked against runtime/buf_rt.c: the DFA is built
 * in-process from a `.l` spec, its four tables are handed straight to the
 * real `buf_run` driver, and the token stream is asserted. That is the M5
 * generated/native parity check arriving early -- it pins the table layout
 * (`next[state*nclass + cls[byte]]`, `cls` totality, the `%skip` / -1
 * convention) against the hand-written M0 reference in examples/digits_tables.c
 * and against t_nfa.c's independent NFA walk.
 */
#include <stdio.h>
#include <string.h>

#include "buf_rx.h"
#include "buf_nfa.h"
#include "buf_dfa.h"
#include "buf_rt.h"

/* the local constant in buf_dfa.h must track the runtime enum */
typedef char assert_first_user_tok
    [(BUF_DFA_FIRST_USER_TOK == BUF_TOK_FIRST_USER) ? 1 : -1];

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
static BufDfa dfa;
static BufDfa dfa2;   /* second build, for the determinism check */

static int build_string(const char *spec)
{
    int rc = buf_rx_parse_string(&rx, "<t>", spec);
    CHECK(rc == 0, "spec string parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "NFA builds");
    if (rc != 0) { printf("  -> %s\n", nfa.error); return -1; }
    rc = buf_dfa_build(&dfa, &nfa, &rx);
    CHECK(rc == 0, "DFA builds");
    if (rc != 0) { printf("  -> %s\n", dfa.error); return -1; }
    return 0;
}

static int build(const char *path)
{
    int rc = buf_rx_read_file(&rx, path);
    CHECK(rc == 0, "spec parses");
    if (rc != 0) { printf("  -> %s\n", rx.error); return -1; }
    rc = buf_nfa_build(&nfa, &rx);
    CHECK(rc == 0, "NFA builds");
    if (rc != 0) { printf("  -> %s\n", nfa.error); return -1; }
    rc = buf_dfa_build(&dfa, &nfa, &rx);
    CHECK(rc == 0, "DFA builds");
    if (rc != 0) { printf("  -> %s\n", dfa.error); return -1; }
    return 0;
}

/* Run the real buf_run over `src` using the freshly built dfa tables. */
static int run_lex(const char *src, int len, int *kinds, int *lens, int max)
{
    BufLexer lx;
    int n = 0;
    buf_lexer_init(&lx, src, len);
    for (;;) {
        BufToken tk = buf_run(&lx, dfa.cls, dfa.next, dfa.accept,
                              dfa.rule_token, dfa.nstates, dfa.nclass,
                              dfa.start);
        if (tk.kind == BUF_TOK_EOF) break;
        if (n >= max) break;
        kinds[n] = tk.kind;
        lens[n]  = tk.length;
        n++;
    }
    return n;
}

static void test_cls_total(void)
{
    int c, bad = 0;
    if (build("examples/calc.l") != 0) return;
    for (c = 0; c < 256; c++)
        if (dfa.cls[c] < 0 || dfa.cls[c] >= dfa.nclass) bad = 1;
    CHECK(!bad, "cls[] is total over 0..255, every entry in 0..nclass-1");
    CHECK(dfa.accept[dfa.start] < 0, "start state is non-accepting");
    CHECK(dfa.nclass < 32, "calc.l collapses to well under 32 classes");
}

static void test_digits_parity(void)
{
    int k[32], l[32], n;
    /* mirrors examples/digits_main.c expectations for the hand table */
    if (build("examples/digits.l") != 0) return;

    CHECK(dfa.rule_token[0] == BUF_TOK_FIRST_USER, "rule 0 -> TOK_INT value");
    CHECK(dfa.rule_token[1] == -1, "rule 1 (%skip) -> -1");

    n = run_lex("12 34\n5 x 6\n", (int)strlen("12 34\n5 x 6\n"), k, l, 32);
    CHECK(n == 5, "digits stream: 5 tokens");
    if (n == 5) {
        CHECK(k[0] == BUF_TOK_FIRST_USER && l[0] == 2, "INT '12'");
        CHECK(k[1] == BUF_TOK_FIRST_USER && l[1] == 2, "INT '34'");
        CHECK(k[2] == BUF_TOK_FIRST_USER && l[2] == 1, "INT '5'");
        CHECK(k[3] == BUF_TOK_ERROR     && l[3] == 1, "ERROR 'x'");
        CHECK(k[4] == BUF_TOK_FIRST_USER && l[4] == 1, "INT '6'");
    }
}

static void test_calc_stream(void)
{
    int k[32], l[32], n;
    if (build("examples/calc.l") != 0) return;

    /* %tokens INT FLOAT IDENT PLUS STAR LPAREN RPAREN -> values 2..8 */
    n = run_lex("1 + 2.5 * foo", (int)strlen("1 + 2.5 * foo"), k, l, 32);
    CHECK(n == 5, "calc stream: 5 tokens");
    if (n == 5) {
        CHECK(k[0] == 2 && l[0] == 1, "INT '1'");
        CHECK(k[1] == 5 && l[1] == 1, "PLUS");
        CHECK(k[2] == 3 && l[2] == 3, "FLOAT '2.5' (longest match beats INT '.'?)");
        CHECK(k[3] == 6 && l[3] == 1, "STAR");
        CHECK(k[4] == 4 && l[4] == 3, "IDENT 'foo'");
    }

    n = run_lex("1 # x\n2", (int)strlen("1 # x\n2"), k, l, 32);
    CHECK(n == 2 && k[0] == 2 && k[1] == 2, "'#' comment skipped");

    n = run_lex("$", 1, k, l, 32);
    CHECK(n == 1 && k[0] == BUF_TOK_ERROR, "stray '$' -> ERROR + resync");
}

/* An epsilon-cycle spec (nullable sub-expression, non-nullable rule) must
 * still build a finite DFA and drive buf_run without spinning. */
static void test_epsilon_cycle_dfa(void)
{
    int k[16], l[16], n;
    if (build_string("%tokens A\nA (a?)+b\n") != 0) return;
    CHECK(dfa.accept[dfa.start] < 0, "(a?)+b start state non-accepting");
    n = run_lex("aaab", 4, k, l, 16);
    CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 4,
          "(a?)+b DFA matches 'aaab'");
    n = run_lex("b", 1, k, l, 16);
    CHECK(n == 1 && k[0] == BUF_TOK_FIRST_USER && l[0] == 1,
          "(a?)+b DFA matches 'b'");
}

static void test_clike(void)
{
    int k[64], l[64], n;
    if (build("examples/clike.l") != 0) return;

    CHECK(dfa.nclass < 64, "clike.l alphabet is far below 256 classes");
    CHECK(dfa.accept[dfa.start] < 0, "clike start state is non-accepting");

    /* KW_IF is %tokens slot 0 -> value 2; IDENT is slot 8 -> value 10 */
    n = run_lex("if x", 4, k, l, 64);
    CHECK(n == 2, "'if x' -> 2 tokens");
    if (n == 2) {
        CHECK(k[0] == 2 && l[0] == 2, "'if' is KW_IF, not IDENT (earlier rule wins tie)");
        CHECK(k[1] == 10 && l[1] == 1, "'x' is IDENT");
    }

    n = run_lex("iffy", 4, k, l, 64);
    CHECK(n == 1 && k[0] == 10 && l[0] == 4, "'iffy' is one IDENT (longest match)");

    n = run_lex("a /* c */ b", (int)strlen("a /* c */ b"), k, l, 64);
    CHECK(n == 2 && k[0] == 10 && k[1] == 10, "block comment skipped");

    n = run_lex("a // trailing\nb", (int)strlen("a // trailing\nb"), k, l, 64);
    CHECK(n == 2 && k[0] == 10 && k[1] == 10, "line comment skipped");

    n = run_lex("x->y", 4, k, l, 64);
    CHECK(n == 3, "'x->y' -> 3 tokens");
    if (n == 3)
        CHECK(l[1] == 2, "'->' lexes as one 2-byte token (longest match over '-')");
}

/* The construction must be deterministic -- first-appearance class numbering,
 * discovery-order state ids -- or the M5 generated/native paths won't produce
 * byte-identical tables. Build clike.l twice and compare the four tables. */
static void test_determinism(void)
{
    static BufRx  rx_b;
    static BufNfa nfa_b;
    int i, ntrans;

    if (build("examples/clike.l") != 0) return;         /* fills dfa */
    if (buf_rx_read_file(&rx_b, "examples/clike.l") != 0) return;
    if (buf_nfa_build(&nfa_b, &rx_b) != 0) return;
    if (buf_dfa_build(&dfa2, &nfa_b, &rx_b) != 0) return;

    CHECK(dfa2.nclass == dfa.nclass && dfa2.nstates == dfa.nstates &&
          dfa2.nrules == dfa.nrules && dfa2.start == dfa.start,
          "second build agrees on nclass / nstates / nrules / start");

    for (i = 0; i < 256; i++)
        if (dfa2.cls[i] != dfa.cls[i]) break;
    CHECK(i == 256, "cls[] is byte-identical across builds");

    ntrans = dfa.nstates * dfa.nclass;
    for (i = 0; i < ntrans; i++)
        if (dfa2.next[i] != dfa.next[i]) break;
    CHECK(i == ntrans, "next[] is byte-identical across builds");

    for (i = 0; i < dfa.nstates; i++)
        if (dfa2.accept[i] != dfa.accept[i]) break;
    CHECK(i == dfa.nstates, "accept[] is byte-identical across builds");

    for (i = 0; i < dfa.nrules; i++)
        if (dfa2.rule_token[i] != dfa.rule_token[i]) break;
    CHECK(i == dfa.nrules, "rule_token[] is byte-identical across builds");
}

int main(void)
{
    test_cls_total();
    test_digits_parity();
    test_calc_stream();
    test_epsilon_cycle_dfa();
    test_clike();
    test_determinism();

    printf("%s: %d checks, %d failures\n",
           failures ? "FAIL" : "ok  ", checks, failures);
    return failures ? 1 : 0;
}
